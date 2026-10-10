// Guard against picking icon codepoints the shipped icon font does not have.
//
// `.icon(0x...)` resolves to the Font Awesome file in assets/, so a codepoint that
// is not in that file cannot be drawn by it: the glyph then comes from whatever
// fallback font happens to have something there, which means the icon changes
// shape - or disappears - depending on the user's own font choice. That is exactly
// what happened to the 安装 APK button, which asked for 0xF50D (a Font Awesome Pro
// codepoint, absent from the Free font they ship): Microsoft YaHei drew an empty
// box, SimSun drew nothing, and a Nerd Font drew an unrelated glyph.
//
// The test therefore reads the *source* of the GUI (main.cpp) instead of a list
// copied here - a copy would be updated alongside the mistake and catch nothing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "tests/test_main.h"

#ifndef ADBTOOLS_SOURCE_DIR
#error "test_icons.cpp needs ADBTOOLS_SOURCE_DIR; see the compile definitions in CMakeLists.txt"
#endif

namespace {

// The same file CMake copies next to the executable; taken from the vendored
// framework so a fresh clone has it even before anything is built.
constexpr const char* kIconFontRelativePath = "/3rd/EUI-NEO/assets/Font Awesome 7 Free-Solid-900.otf";

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::uint32_t readU32(const std::string& data, std::size_t offset) {
    if (offset + 4 > data.size()) return 0;
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

std::uint16_t readU16(const std::string& data, std::size_t offset) {
    if (offset + 2 > data.size()) return 0;
    return static_cast<std::uint16_t>(
        (static_cast<unsigned int>(static_cast<unsigned char>(data[offset])) << 8) |
        static_cast<unsigned int>(static_cast<unsigned char>(data[offset + 1])));
}

// Codepoints the font can actually draw: the Unicode cmap subtables (formats 4 and
// 12 are what real fonts ship), counting only entries that point at a real glyph -
// a mapping to glyph 0 means "this font has no glyph for it".
bool readCmapCoverage(const std::string& font, std::set<std::uint32_t>* covered) {
    if (font.size() < 12) return false;

    const std::uint16_t tableCount = readU16(font, 4);
    std::size_t cmapOffset = 0;
    for (std::uint16_t i = 0; i < tableCount; ++i) {
        const std::size_t entry = 12 + static_cast<std::size_t>(i) * 16;
        if (entry + 16 > font.size()) return false;
        if (font.compare(entry, 4, "cmap") == 0) {
            cmapOffset = readU32(font, entry + 8);
            break;
        }
    }
    if (cmapOffset == 0 || cmapOffset + 4 > font.size()) return false;

    const std::uint16_t subtableCount = readU16(font, cmapOffset + 2);
    for (std::uint16_t i = 0; i < subtableCount; ++i) {
        const std::size_t record = cmapOffset + 4 + static_cast<std::size_t>(i) * 8;
        const std::size_t sub = cmapOffset + readU32(font, record + 4);
        if (sub + 2 > font.size()) continue;
        const std::uint16_t format = readU16(font, sub);

        if (format == 4) {
            const std::uint16_t segmentCountX2 = readU16(font, sub + 6);
            const std::size_t segments = segmentCountX2 / 2;
            const std::size_t ends = sub + 14;
            const std::size_t starts = ends + segmentCountX2 + 2;
            const std::size_t deltas = starts + segmentCountX2;
            const std::size_t ranges = deltas + segmentCountX2;
            for (std::size_t s = 0; s < segments; ++s) {
                const std::uint32_t start = readU16(font, starts + s * 2);
                const std::uint32_t end = readU16(font, ends + s * 2);
                if (end < start) continue;
                for (std::uint32_t cp = start; cp <= end && cp <= 0xFFFFu; ++cp) {
                    const std::uint16_t rangeOffset = readU16(font, ranges + s * 2);
                    std::uint16_t glyph = 0;
                    if (rangeOffset == 0) {
                        glyph = static_cast<std::uint16_t>(cp + readU16(font, deltas + s * 2));
                    } else {
                        const std::size_t address = ranges + s * 2 + rangeOffset + (cp - start) * 2;
                        glyph = readU16(font, address);
                        if (glyph != 0) {
                            glyph = static_cast<std::uint16_t>(glyph + readU16(font, deltas + s * 2));
                        }
                    }
                    if (glyph != 0) covered->insert(cp);
                }
            }
        } else if (format == 12) {
            const std::uint32_t groupCount = readU32(font, sub + 12);
            for (std::uint32_t g = 0; g < groupCount; ++g) {
                const std::size_t group = sub + 16 + static_cast<std::size_t>(g) * 12;
                const std::uint32_t start = readU32(font, group);
                const std::uint32_t end = readU32(font, group + 4);
                const std::uint32_t startGlyph = readU32(font, group + 8);
                if (startGlyph == 0 || end < start) continue;
                // Font Awesome 7 puts its newest icons in the supplementary private
                // use areas; those ranges are small in practice, but cap the walk so
                // a corrupt table cannot spin here.
                const std::uint32_t last = end - start > 0x10000u ? start + 0x10000u : end;
                for (std::uint32_t cp = start; cp <= last; ++cp) covered->insert(cp);
            }
        }
    }
    return !covered->empty();
}

// Private use areas: that is where Font Awesome keeps every icon codepoint.
bool isIconCodepoint(std::uint32_t cp) {
    return (cp >= 0xE000u && cp <= 0xF8FFu) ||      // BMP private use area
           (cp >= 0xF0000u && cp <= 0xFFFFDu);      // planes 15 and 16
}

struct IconUse {
    std::uint32_t codepoint = 0;
    int line = 0;
};

// Every icon codepoint main.cpp asks for. Comments are skipped on purpose:
// 0xFFFF appears in a comment about GGI_MARK_NONEXISTING_GLYPHS and is not an icon.
std::vector<IconUse> collectIconCodepoints(const std::string& source) {
    std::vector<IconUse> found;
    std::istringstream lines(source);
    std::string line;
    int lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        const std::size_t comment = line.find("//");
        const std::string code = comment == std::string::npos ? line : line.substr(0, comment);
        for (std::size_t i = 0; i + 3 < code.size(); ++i) {
            if (code[i] != '0' || code[i + 1] != 'x') continue;
            std::size_t end = i + 2;
            while (end < code.size() && std::isxdigit(static_cast<unsigned char>(code[end]))) ++end;
            const std::size_t digits = end - (i + 2);
            if (digits < 4 || digits > 6) continue;
            const std::uint32_t cp = static_cast<std::uint32_t>(
                std::strtoul(code.substr(i + 2, digits).c_str(), nullptr, 16));
            if (isIconCodepoint(cp)) {
                found.push_back({cp, lineNumber});
            }
            i = end - 1;
        }
    }
    return found;
}

}  // namespace

ADB_TEST(every_icon_codepoint_exists_in_the_shipped_icon_font) {
    const std::string fontPath = std::string(ADBTOOLS_SOURCE_DIR) + kIconFontRelativePath;
    const std::string font = readFile(fontPath);
    if (font.empty()) {
        std::printf("      cannot read %s\n", fontPath.c_str());
    }
    ADB_CHECK(!font.empty());

    std::set<std::uint32_t> covered;
    const bool parsed = readCmapCoverage(font, &covered);
    ADB_CHECK(parsed);

    const std::string source = readFile(std::string(ADBTOOLS_SOURCE_DIR) + "/main.cpp");
    ADB_CHECK(!source.empty());

    const std::vector<IconUse> used = collectIconCodepoints(source);
    // The scanner must keep finding the icons: if main.cpp moves or the pattern
    // changes, this test would otherwise pass while checking nothing at all.
    ADB_CHECK(used.size() >= 20);

    std::size_t missing = 0;
    for (const IconUse& use : used) {
        if (covered.count(use.codepoint) == 0) {
            ++missing;
            std::printf("      0x%04X (main.cpp:%d) is not in the icon font\n", use.codepoint, use.line);
        }
    }
    ADB_CHECK_EQ(missing, static_cast<std::size_t>(0));
}
