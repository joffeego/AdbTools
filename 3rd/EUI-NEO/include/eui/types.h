#pragma once

#include <core/render/shadertoy.h>

#include "core/animation.h"
#include "core/layout.h"
#include "core/input/input_types.h"
#include "core/render/render_types.h"
#include "core/render/text.h"
#include "core/render/text_types.h"

namespace eui {

using Align = core::Align;
using AnimProperty = core::AnimProperty;
using Ease = core::Ease;
using Transition = core::Transition;
using Color = core::Color;
using Vec2 = core::Vec2;
using Vec3 = core::Vec3;
using Rect = core::Rect;
using SizeValue = core::SizeValue;
using Gradient = core::Gradient;
using GradientDirection = core::GradientDirection;
using Border = core::Border;
using Shadow = core::Shadow;
using Transform = core::Transform;
using TransformMatrix = core::TransformMatrix;
using HorizontalAlign = core::HorizontalAlign;
using VerticalAlign = core::VerticalAlign;
using TextStyle = core::TextStyle;
using CursorShape = core::CursorShape;
using InputKey = core::InputKey;
using KeyAction = core::KeyAction;
using KeyModifiers = core::KeyModifiers;
using KeyEvent = core::KeyEvent;
using TextInputEvent = core::TextInputEvent;
using PointerAction = core::PointerAction;
using PointerButton = core::PointerButton;
using PointerButtons = core::PointerButtons;
using PointerEvent = core::PointerEvent;
using ShaderToyChannel = core::render::ShaderToyChannel;
using ShaderToyChannelKind = core::render::ShaderToyChannelKind;
using ShaderToyError = core::render::ShaderToyError;
using ShaderToyErrorCode = core::render::ShaderToyErrorCode;
using ShaderToyGraph = core::render::ShaderToyGraph;
using ShaderToyPass = core::render::ShaderToyPass;
using ShaderToySourceKind = core::render::ShaderToySourceKind;
using ShaderToyUniform = core::render::ShaderToyUniform;
using ShaderToyUniformKind = core::render::ShaderToyUniformKind;
using core::render::loadShaderToyGraphJson;
using core::render::parseShaderToyGraphJson;
using TextPrimitive = core::TextPrimitive;

// Width of `text` in logical pixels, measured by the very engine that draws it.
//
// Callers that have to cut a string to fit a box (the file list's name column, a
// device label in a fixed-width selector) should ask this instead of estimating from
// a character count: an estimate is wrong in both directions - it clips an ASCII name
// while the box is still half empty, and it lets a CJK name run past the edge, because
// those glyphs are about twice as wide.
inline float measureTextWidth(const std::string& text, const std::string& fontFamily = {},
                              float fontSize = 16.0f, int fontWeight = 400) {
    return TextPrimitive::measureTextWidth(text, fontFamily, fontSize, fontWeight);
}

inline Color mixColor(const Color& from, const Color& to, float amount) {
    return core::mixColor(from, to, amount);
}

} // namespace eui
