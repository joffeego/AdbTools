#!/usr/bin/env python3
"""Build assets/icon.ico (and a PNG master) from assets/icon-source.jpg.

    python scripts/make_icon.py                 # regenerate the icons
    python scripts/make_icon.py --check         # regenerate and verify only

Why this is a script and not a hand-made .ico
---------------------------------------------

The source artwork is a rounded tile that is flush with the canvas on all four
straight edges, so the only background in it is the white left in the four
corner arcs. Turning that into a Windows icon is not "make white transparent":
the artwork is white too (the folder and the Android), and an .ico whose corners
keep a rim of the JPEG's white-blended edge is exactly the "white edge at the
four corners" defect this file exists to prevent.

So the boundary is measured, not guessed:

  1. A smooth tile-colour *field* is fitted over the plain tile only, by
     rejecting whatever does not match it. The generator's watermark, the
     artwork and JPEG speckle are all rejected without any of them being
     located - a fit made *around* the watermark carries no trace of it.
  2. Every pixel is then un-mixed against that field: if the observed blue
     channel is obs = a*field + (1-a)*255, then a follows directly. That
     recovers the real anti-aliased contour without assuming a shape.
  3. alpha is forced to exactly 1 away from the contour and to exactly 0 away
     from the tile, so the JPEG's own noise cannot leave stray translucency.
  4. The plain tile is rebuilt from the field and only the artwork is carried
     over from the source.

Every step is then checked (--check runs the checks and exits non-zero on
failure): corners fully transparent, straight edges fully opaque, no opaque
near-white pixel anywhere near the contour, a smooth contour, no watermark
residue, and no implausibly dark pixels.

Pillow and numpy are the only dependencies.
"""

from __future__ import annotations

import argparse
import io
import os
import struct
import sys

import numpy as np
from PIL import Image

# Sizes Windows actually asks for. 16/32/48/256 are the ones it must have; the
# rest cover the 125%, 150% and 175% DPI steps without a rescale.
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
# Up to 64px the plain BMP form is used (every tool can read it); 128 and 256
# are stored as PNG, which keeps the file small and is what Windows expects.
PNG_FROM = 128

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------------------
# array helpers (numpy only - no scipy)
# ---------------------------------------------------------------------------

def box_blur_1d(a: np.ndarray, r: int, axis: int) -> np.ndarray:
    """Uniform box blur of radius r along `axis`, edge-padded.

    A running sum, so the cost does not depend on r - the wide reference field
    below uses a large radius.
    """
    if r < 1:
        return a
    n = a.shape[axis]
    pad = [(0, 0)] * a.ndim
    pad[axis] = (r, r)
    p = np.pad(a, pad, mode="edge")
    c = np.cumsum(p, axis=axis)
    shape = list(c.shape)
    shape[axis] = 1
    c = np.concatenate([np.zeros(shape, dtype=c.dtype), c], axis=axis)
    hi = np.take(c, np.arange(2 * r + 1, n + 2 * r + 1), axis=axis)
    lo = np.take(c, np.arange(0, n), axis=axis)
    return (hi - lo) / float(2 * r + 1)


def gaussian_blur(a: np.ndarray, sigma: float) -> np.ndarray:
    """Three box passes, a close and cheap Gaussian approximation."""
    if sigma <= 0:
        return a
    # variance of three box passes of width (2r+1) is 3*((2r+1)^2-1)/12
    r = max(1, int(round((np.sqrt(1.0 + 4.0 * sigma * sigma / 3.0) - 1.0) / 2.0)))
    out = a
    for _ in range(3):
        out = box_blur_1d(out, r, 0)
        out = box_blur_1d(out, r, 1)
    return out


def normalized_convolution(img: np.ndarray, weight: np.ndarray, sigma: float):
    """blur(img*weight) / blur(weight), and how much support each pixel had.

    The second value is the denominator: for a 0/1 weight it is exactly the
    fraction of the blur window that was weighted. Deep inside a large
    unweighted area both terms are ~0 and the quotient is meaningless (it comes
    out black), which is why callers need to know.
    """
    w = weight.astype(np.float64)
    num = gaussian_blur(img * w[..., None], sigma)
    den = gaussian_blur(w, sigma)
    safe = np.where(den < 1e-6, 1e-6, den)
    return num / safe[..., None], den


def dilate(binary: np.ndarray, r: int) -> np.ndarray:
    """Grow by a square of radius r, one pixel per pass."""
    out = binary
    for _ in range(r):
        d = out.copy()
        d[:, 1:] |= out[:, :-1]
        d[:, :-1] |= out[:, 1:]
        d[1:, :] |= out[:-1, :]
        d[:-1, :] |= out[1:, :]
        out = d
    return out


def erode(binary: np.ndarray, r: int) -> np.ndarray:
    """Shrink by a square of radius r, one pixel per pass."""
    out = binary
    for _ in range(r):
        e = out.copy()
        e[:, 1:] &= out[:, :-1]
        e[:, :-1] &= out[:, 1:]
        e[1:, :] &= out[:-1, :]
        e[:-1, :] &= out[1:, :]
        out = e
    return out


def fill_holes(binary: np.ndarray) -> np.ndarray:
    """True inside the shape, holes included: flood the outside from the border."""
    outside = np.zeros_like(binary)
    outside[0, :] = ~binary[0, :]
    outside[-1, :] = ~binary[-1, :]
    outside[:, 0] = ~binary[:, 0]
    outside[:, -1] = ~binary[:, -1]
    for _ in range(8192):
        grown = dilate(outside, 1) & ~binary
        if np.array_equal(grown, outside):
            break
        outside = grown
    return ~outside


# ---------------------------------------------------------------------------
# 1. analysis - everything the compose step needs, so diagnostics and the build
#    itself always see identical numbers
# ---------------------------------------------------------------------------

def analyse(src_path: str, report=print) -> dict:
    obs = np.asarray(Image.open(src_path).convert("RGB")).astype(np.float64) / 255.0
    h, w, _ = obs.shape
    blue = obs[:, :, 2]

    def overlay_strength(C):
        """t such that obs = (1-t)*C + t*white.

        The watermark is the tile blended with white, so its deviation from the
        tile field is proportional to (255 - C) *per channel*. The blue channel
        is nearly blind to it (the mark's blue, 181, is within a level of the
        tile's 180) while red moves ~30 levels; averaging the per-channel
        solution recovers its strength whatever one channel does.
        """
        return np.mean((obs - C) / np.maximum(1.0 - C, 1.0 / 255.0), axis=2)

    # ---- where is the tile? -------------------------------------------------
    rough, _ = normalized_convolution(obs, blue < (200.0 / 255.0), 24.0)
    c_blue = np.clip(rough[:, :, 2], 1.0 / 255.0, 254.0 / 255.0)
    alpha_rough = np.clip((1.0 - blue) / (1.0 - c_blue), 0.0, 1.0)
    inside = fill_holes(alpha_rough > 0.5)

    # ---- where is the artwork? ---------------------------------------------
    # The folder and the Android are strongly whitened (t ~ 1) where the
    # watermark only reaches ~0.54, so one threshold separates them. Hole
    # filling then picks up the grey outline strokes the white encloses (the
    # Android's outline inside the folder) and the dilation carries in the
    # anti-aliased fringe plus the strokes wrapped around the white legs that
    # stick out below the folder.
    artwork = fill_holes(inside & (overlay_strength(rough) > 0.70))

    # ---- the background field ----------------------------------------------
    # Fit a smooth tile-colour field over plain tile only.
    #
    # "Plain tile" cannot be decided against the narrow field the fit is trying
    # to produce: the watermark biases its own reference, its fainter parts then
    # look like tile, and their bias leaves a ghost of the mark. Measuring the
    # deviation against a much wider field avoids that, because a mark ~2400px
    # across is about a percent of a 64px-radius average, while the tile's own
    # gradient over that distance stays far below the tolerance.
    base = erode(inside, 12)
    # That wide reference must itself be fitted from tile only. Left to average
    # the white folder in, it would be pulled bright across a ring as wide as
    # its own reach, every genuine tile pixel in that ring would fail the test
    # below, and the narrow fit would then have nothing to average exactly along
    # the folder's edge - which paints black outlines around it.
    tile_only = base & ~dilate(artwork, 20)
    wide, _ = normalized_convolution(obs, tile_only, 64.0)
    plain = base & (np.abs(obs - wide).max(axis=2) < 10.0 / 255.0)
    field, _ = normalized_convolution(obs, plain, 24.0)

    # ---- contour ------------------------------------------------------------
    # Un-mix against the field. The contour band always has full support, so the
    # field is meaningful there.
    c_blue = np.clip(field[:, :, 2], 1.0 / 255.0, 254.0 / 255.0)
    alpha = np.clip((1.0 - blue) / (1.0 - c_blue), 0.0, 1.0)

    inner = erode(inside, 4)          # deep inside: alpha is exactly 1
    band = inside & ~inner            # the contour band: trust the un-mixing
    out_alpha = np.where(inner, 1.0, np.where(band, alpha, 0.0))
    out_alpha[out_alpha < 0.004] = 0.0

    # ---- the rebuilt background --------------------------------------------
    # The support comes from the *artwork* mask, not from the fit: the fit
    # deliberately drops the watermark, and asking it where it has data would
    # hand the watermark back to the source exactly where it must not.
    support_weight = erode(inside, 2) & ~dilate(artwork, 14)
    _, support = normalized_convolution(obs, support_weight, 24.0)

    # The thresholds are deliberately low. The field is a smooth interpolation
    # of a gently varying background, so it stays accurate well past the point
    # where half its window falls outside the tile - and the watermark's box
    # runs right up against the corner arc, which is where a support fraction
    # never reaches 1. Asking for 0.75 there would leave the mark's outer stroke
    # composited from the source. The artwork is excluded from the support mask
    # with a 14px margin, so nothing that matters lies near this edge.
    blend = np.clip((support - 0.15) / (0.40 - 0.15), 0.0, 1.0)[..., None]
    # Plain tile is rebuilt from the field; only the artwork is carried over
    # from the source. The watermark lives in plain tile, so it is not part of
    # anything that survives.
    rgb = np.clip(obs * (1.0 - blend) + field * blend, 0.0, 1.0)

    return dict(obs=obs, inside=inside, artwork=artwork, base=base, plain=plain,
                wide=wide, field=field, support=support, blend=blend[..., 0],
                rgb=rgb, alpha=out_alpha, size=(h, w))


# ---------------------------------------------------------------------------
# 2. resample and pack
# ---------------------------------------------------------------------------

def resize_rgba(rgb: np.ndarray, alpha: np.ndarray, size: int):
    """Lanczos resize done on *pre-multiplied* colour.

    Resampling R, G, B and A independently lets the colour of fully transparent
    pixels bleed in, which leaves a grey halo around the tile; multiplying
    first and dividing after is the only correct way to resample an edge against
    transparency.
    """
    h, w, _ = rgb.shape
    if (w, h) == (size, size):
        out_rgb, out_a = rgb.copy(), alpha.copy()
    else:
        pm = np.dstack([rgb * alpha[..., None], alpha])
        out = np.empty((size, size, 4), dtype=np.float64)
        for i in range(4):
            ch = Image.fromarray(pm[:, :, i].astype(np.float32), mode="F")
            out[:, :, i] = np.asarray(ch.resize((size, size), Image.LANCZOS), dtype=np.float64)
        out_rgb, out_a = out[:, :, :3], out[:, :, 3]
    a = np.clip(out_a, 0.0, 1.0)
    a[a < 0.004] = 0.0
    safe = np.where(a < 1e-6, 1.0, a)
    return np.clip(out_rgb / safe[..., None], 0.0, 1.0), a


def to_rgba8(rgb: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    return np.clip(np.round(np.dstack([rgb, alpha]) * 255.0), 0, 255).astype(np.uint8)


def ico_bmp_entry(rgba: np.ndarray) -> bytes:
    """BITMAPINFOHEADER + bottom-up BGRA + the 1bpp AND mask."""
    h, w, _ = rgba.shape
    header = np.zeros(40, dtype=np.uint8)
    struct.pack_into("<IiiHHIIiiII", header, 0,
                     40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    bgra = rgba[:, :, [2, 1, 0, 3]][::-1]
    mask = np.zeros((h, ((w + 31) // 32) * 4), dtype=np.uint8)
    return header.tobytes() + bgra.tobytes() + mask.tobytes()


def ico_png_entry(rgba: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(rgba, mode="RGBA").save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_ico(path: str, entries) -> None:
    """entries: (size, payload) in the order the directory should list them."""
    offset = 6 + 16 * len(entries)
    directory = bytearray()
    for size, payload in entries:
        dim = 0 if size >= 256 else size
        directory += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(payload), offset)
        offset += len(payload)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(entries)))
        f.write(bytes(directory))
        for _, payload in entries:
            f.write(payload)


# ---------------------------------------------------------------------------
# 3. verification
# ---------------------------------------------------------------------------

def verify(state: dict, rgba8: np.ndarray, report=print) -> list:
    """Return a list of failure strings (empty means everything passed)."""
    rgb, alpha = state["rgb"], state["alpha"]
    h, w = alpha.shape
    failures = []
    mn = rgba8[:, :, :3].min(axis=2)

    # 1. the headline requirement: no opaque near-white pixel near the contour.
    near = alpha <= 0.05
    for _ in range(6):
        near = dilate(near, 1)
    bad = near & (alpha >= 0.5) & (mn > 235)
    report(f"  opaque near-white pixels within 6px of transparency: {int(bad.sum())}")
    if bad.any():
        ys, xs = np.where(bad)
        failures.append(f"white fringe at the contour, e.g. "
                        f"{[(int(x), int(y)) for y, x in list(zip(ys, xs))[:5]]}")

    # 2. the four corners must be completely transparent.
    corners = {"top-left": (slice(0, 8), slice(0, 8)),
               "top-right": (slice(0, 8), slice(w - 8, w)),
               "bottom-left": (slice(h - 8, h), slice(0, 8)),
               "bottom-right": (slice(h - 8, h), slice(w - 8, w))}
    for name, sl in corners.items():
        m = float(alpha[sl].max())
        report(f"  corner {name:12s} max alpha {m:.3f}")
        if m > 0.0:
            failures.append(f"corner {name} is not fully transparent (alpha {m:.3f})")

    # 3. and the straight edges must be fully opaque.
    edges = {"left": alpha[h // 2, 0], "right": alpha[h // 2, w - 1],
             "top": alpha[0, w // 2], "bottom": alpha[h - 1, w // 2]}
    report("  straight edges: " + "  ".join(f"{k}={v:.3f}" for k, v in edges.items()))
    for k, v in edges.items():
        if v < 1.0:
            failures.append(f"the {k} edge is not fully opaque (alpha {v:.3f})")

    # 4. the contour must be smooth. Down the top-left corner the tile's left
    #    boundary is a single-valued function of y, so sample its sub-pixel
    #    position and compare it with a moving average of itself: that measures
    #    jaggedness without assuming the corner is a circular arc (it need not
    #    be - a squircle is preserved just as faithfully, since the contour is
    #    measured rather than fitted).
    #
    #    Only the well-conditioned part of the arc is sampled. Over the top of
    #    the arc x(y) is nearly horizontal - x moves 6px per row - so neither
    #    the position nor a moving average of it is meaningful there, and a
    #    "jaggedness" measured there is just the corner's own curvature.
    def left_contour(y):
        row = alpha[y]
        idx = np.where(row > 0.5)[0]
        if len(idx) == 0:
            return None
        i = int(idx[0])
        if i == 0:
            return 0.0
        a0, a1 = row[i - 1], row[i]
        return float(i) if a1 == a0 else (i - 1) + (0.5 - a0) / (a1 - a0)

    # Fit the radius first, then sample 0.25r..0.90r of it: past 0.9r the arc has
    # become the straight edge, and near 0 it is nearly horizontal.
    coarse = {}
    for y in range(6, min(h // 2, 400)):
        v = left_contour(y)
        if v is not None and 0.0 < v < w * 0.5:
            coarse[y] = v
    best_r, best_e = 0.0, 9e9
    for r in np.arange(200.0, 360.0, 0.5):
        pts = [(y, x) for y, x in coarse.items() if y < r]
        if len(pts) < 30:
            continue
        e = np.mean([(r - np.sqrt(max(0.0, r * r - (r - y) ** 2)) - x) ** 2 for y, x in pts])
        if e < best_e:
            best_r, best_e = r, e

    lo, hi = int(best_r * 0.25), int(best_r * 0.90)
    ys_c = np.array([y for y in range(4, min(h // 2, 400)) if y in coarse], dtype=float)
    xs_c = np.array([coarse[int(y)] for y in ys_c])
    if len(xs_c) > 40:
        # Average over a wider span than is judged: at the ends of the window the
        # moving average would otherwise be built from edge-replicated samples of
        # a steeply sloped arc, and that bias is not jaggedness.
        win = 11
        pad = np.pad(xs_c, win // 2, mode="edge")
        smooth = np.convolve(pad, np.ones(win) / win, mode="valid")
        sel = (ys_c >= lo) & (ys_c <= hi)
        jag = float(np.abs(xs_c - smooth)[sel].max())
        slopes = np.abs(np.diff(xs_c[sel]) / np.diff(ys_c[sel]))
        report(f"  contour jaggedness (vs its own moving average): {jag:.3f} px")
        report(f"  measured over y {lo}..{hi}")
        report(f"  corner shape: best circular radius {best_r:.1f}px "
               f"({best_r / w:.4f} of the side), circle fit residual {np.sqrt(best_e):.3f}px, "
               f"slope there {slopes.min():.2f}..{slopes.max():.2f}")
    else:
        jag = 9e9
        report("  contour: too few samples to measure")
    if jag > 0.75:
        failures.append(f"the contour is jagged (max deviation {jag:.3f}px)")

    # 5. no implausibly dark pixels - the classic symptom of a field that had no
    #    support somewhere and came out as zero.
    dark = int((rgba8[:, :, :3].min(axis=2) < 40).sum())
    report(f"  implausibly dark pixels (min channel < 40): {dark}")
    if dark:
        failures.append(f"{dark} pixels came out near-black")

    # 6. no watermark residue. Measured against the wide reference field, which a
    #    compact mark cannot shift, over the badge and over ordinary tile so the
    #    two numbers can be compared directly.
    field, wide = state["field"], state["wide"]
    plain = state["plain"]
    bump = np.abs(field[:, :, 0] - wide[:, :, 0]) * 255.0
    badge = np.zeros_like(plain)
    badge[880:1010, 800:1000] = True
    badge &= erode(state["inside"], 12) & ~state["artwork"]
    tile_bump = float(np.percentile(bump[plain], 99.9))
    badge_bump = float(bump[badge].max()) if badge.any() else 0.0
    report(f"  watermark residue: badge area max {badge_bump:.2f} levels, "
           f"ordinary tile p99.9 {tile_bump:.2f} levels")
    if badge_bump > max(4.0, tile_bump + 2.0):
        failures.append(f"watermark residue in the field ({badge_bump:.2f} levels)")

    return failures


# ---------------------------------------------------------------------------
# 4. main
# ---------------------------------------------------------------------------

def build(source: str, ico_path, png_path, report=print) -> list:
    """Analyse, verify, and (when a path is given) write. Returns failures."""
    report(f"source: {source}")
    state = analyse(source, report)
    h, w = state["size"]
    rgba8 = to_rgba8(state["rgb"], state["alpha"])
    report(f"master: {w}x{h}")

    report("verification:")
    failures = verify(state, rgba8, report)

    if ico_path:
        entries = []
        for size in ICO_SIZES:
            rgb_s, a_s = resize_rgba(state["rgb"], state["alpha"], size)
            a8 = to_rgba8(rgb_s, a_s)
            entries.append((size, ico_png_entry(a8) if size >= PNG_FROM else ico_bmp_entry(a8)))
        write_ico(ico_path, entries)
        report(f"wrote {ico_path} ({os.path.getsize(ico_path)} bytes, sizes "
               f"{', '.join(str(s) for s in ICO_SIZES)})")

    if png_path:
        Image.fromarray(rgba8, "RGBA").save(png_path)
        report(f"wrote {png_path} ({os.path.getsize(png_path)} bytes)")

    return failures


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source", default=os.path.join(REPO, "assets", "icon-source.jpg"))
    ap.add_argument("--out", default=os.path.join(REPO, "assets", "icon.ico"))
    ap.add_argument("--png", default=os.path.join(REPO, "assets", "icon.png"))
    ap.add_argument("--check", action="store_true",
                    help="verify only; write nothing")
    args = ap.parse_args(argv)

    failures = build(args.source,
                     None if args.check else args.out,
                     None if args.check else args.png)
    if failures:
        print("\nFAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
