#pragma once

#include "components/button.h"
#include "components/theme.h"
#include "core/dsl.h"
#include "eui/signal.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace components {

struct DialogStyle {
    DialogStyle() : DialogStyle(theme::dark()) {}

    explicit DialogStyle(const theme::ThemeColorTokens& tokens) {
        backdrop = theme::color(0.0f, 0.0f, 0.0f, tokens.dark ? 0.46f : 0.28f);
        surface = tokens.surface;
        border = theme::withOpacity(tokens.border, 0.82f);
        title = theme::pageVisuals(tokens).titleColor;
        message = theme::pageVisuals(tokens).bodyColor;
        primary = tokens.primary;
        secondary = tokens.surfaceHover;
        primaryHover = theme::buttonHover(tokens, primary);
        primaryPressed = theme::buttonPressed(tokens, primary);
        secondaryHover = theme::buttonHover(tokens, secondary);
        secondaryPressed = theme::buttonPressed(tokens, secondary);
        shadow = theme::panelShadow(tokens);
        radius = tokens.metrics.radius.section;
    }

    core::Color backdrop;
    core::Color surface;
    core::Color border;
    core::Color title;
    core::Color message;
    core::Color primary;
    core::Color secondary;
    core::Color primaryHover;
    core::Color primaryPressed;
    core::Color secondaryHover;
    core::Color secondaryPressed;
    core::Shadow shadow;
    float radius = 18.0f;
};

class DialogBuilder {
public:
    DialogBuilder(core::dsl::Ui& ui, std::string id)
        : ui_(ui), id_(std::move(id)) {}

    DialogBuilder& open(bool value = true) { open_ = value; return *this; }
    DialogBuilder& bindOpen(eui::Signal<bool>& signal) {
        open(signal.get());
        onOpenChange([&signal](bool value) { signal.set(value); });
        return *this;
    }
    DialogBuilder& screen(float width, float height) { screenWidth_ = width; screenHeight_ = height; return *this; }
    DialogBuilder& size(float width, float height) { width_ = width; height_ = height; return *this; }
    DialogBuilder& title(const std::string& value) { title_ = value; return *this; }
    DialogBuilder& message(const std::string& value) { message_ = value; return *this; }
    DialogBuilder& primaryText(const std::string& value) { primaryText_ = value; return *this; }
    DialogBuilder& secondaryText(const std::string& value) { secondaryText_ = value; return *this; }
    DialogBuilder& style(const DialogStyle& value) { style_ = value; return *this; }
    DialogBuilder& theme(const theme::ThemeColorTokens& tokens) {
        style_ = DialogStyle(tokens);
        metrics_ = tokens.metrics;
        tokens_ = tokens;
        return *this;
    }
    DialogBuilder& transition(const core::Transition& value) { transition_ = value; return *this; }
    DialogBuilder& zIndex(int value) { zIndex_ = value; return *this; }
    // Lets the user move the panel by dragging its header strip. On by default: a
    // dialog that covers the thing you are looking at and cannot be pushed aside
    // is worse than one that can be nudged away, and the offset is remembered per
    // dialog id so it stays where it was put.
    DialogBuilder& draggable(bool value = true) { draggable_ = value; return *this; }
    DialogBuilder& content(std::function<void()> callback) { content_ = std::move(callback); return *this; }
    DialogBuilder& onPrimary(std::function<void()> callback) { onPrimary_ = std::move(callback); return *this; }
    DialogBuilder& onSecondary(std::function<void()> callback) { onSecondary_ = std::move(callback); return *this; }
    DialogBuilder& onOpenChange(std::function<void(bool)> callback) { onOpenChange_ = std::move(callback); return *this; }

    void build() {
        const float width = std::min(width_, std::max(0.0f, screenWidth_ - metrics_.spacing.overlay));
        const float height = std::min(height_, std::max(0.0f, screenHeight_ - metrics_.spacing.overlay));
        const float x = std::max(metrics_.spacing.panel, (screenWidth_ - width) * 0.5f);
        const float y = std::max(metrics_.spacing.panel, (screenHeight_ - height) * 0.5f);

        // Dragging: the offset is kept per dialog id, so a dialog reappears where
        // the user left it, and clamped so a strip of the panel always stays on
        // screen (an off-screen dialog could never be dragged back).
        float panelX = x;
        float panelY = y;
        if (draggable_) {
            core::Vec2& offset = dragOffset(id_);
            const float minVisible = 90.0f;
            panelX = std::min(std::max(x + offset.x, minVisible - width), screenWidth_ - minVisible);
            panelY = std::min(std::max(y + offset.y, 0.0f), screenHeight_ - minVisible);
            offset.x = panelX - x;
            offset.y = panelY - y;
        }
        const float contentWidth = std::max(0.0f, width - metrics_.spacing.overlay);
        const float buttonGap = metrics_.spacing.content;
        const float buttonWidth = std::max(96.0f, std::min(150.0f, (contentWidth - buttonGap) * 0.5f));
        const float buttonRowWidth = buttonWidth * 2.0f + buttonGap;
        const float visible = open_ ? 1.0f : 0.0f;
        const float panelScale = open_ ? 1.0f : 0.965f;
        const float panelOffsetY = open_ ? 0.0f : 14.0f;
        const std::function<void()> requestClose = closeCallback();
        const std::function<void()> onPrimary = onPrimary_;
        const std::function<void()> onSecondary = onSecondary_ ? onSecondary_ : requestClose;

        ui_.stack(id_)
            .size(screenWidth_, screenHeight_)
            .zIndex(zIndex_)
            .content([&] {
                ui_.rect(id_ + ".backdrop")
                    .size(screenWidth_, screenHeight_)
                    .states(style_.backdrop, style_.backdrop, style_.backdrop)
                    .opacity(visible)
                    .transition(transition_)
                    .animate(core::AnimProperty::Opacity)
                    .disabled(!open_)
                    .onClick(requestClose)
                    .onScroll([](const core::ScrollEvent&) {})
                    .build();

                ui_.stack(id_ + ".panel")
                    .x(panelX)
                    .y(panelY)
                    .size(width, height)
                    .opacity(visible)
                    .translateY(panelOffsetY)
                    .scale(panelScale)
                    .transformOrigin(0.5f, 0.5f)
                    .transition(transition_)
                    .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform)
                    .disabled(!open_)
                    .content([&] {
                        ui_.rect(id_ + ".panel.bg")
                            .size(width, height)
                            .color(style_.surface)
                            .radius(style_.radius)
                            .border(metrics_.spacing.hairline, style_.border)
                            .shadow(style_.shadow)
                            .build();

                        ui_.rect(id_ + ".panel.hit")
                            .size(width, height)
                            .states(theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                    theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                    theme::color(0.0f, 0.0f, 0.0f, 0.0f))
                            .blockPointer()
                            .build();

                        // Drag strip: built before the content, so a title or a
                        // close button that overlaps it still gets the click - the
                        // content is drawn on top and wins where they overlap.
                        if (draggable_) {
                            const float strip = std::min(52.0f, height * 0.5f);
                            const std::string dragId = id_;
                            ui_.rect(id_ + ".drag")
                                .size(width, strip)
                                .states(theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                        theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                        theme::color(0.0f, 0.0f, 0.0f, 0.0f))
                                .blockPointer()
                                .onDrag([dragId](const core::dsl::DragEvent& event) {
                                    core::Vec2& offset = dragOffset(dragId);
                                    offset.x += static_cast<float>(event.deltaX);
                                    offset.y += static_cast<float>(event.deltaY);
                                })
                                .build();
                        }

                        if (content_) {
                            content_();
                        } else {
                            ui_.text(id_ + ".title")
                                .x(metrics_.spacing.panel)
                                .y(metrics_.control.indicator)
                                .size(contentWidth, metrics_.spacing.header + metrics_.spacing.micro)
                                .text(title_)
                                .fontSize(metrics_.typography.heading)
                                .lineHeight(metrics_.typography.heading + metrics_.typography.lineGapLoose)
                                .color(style_.title)
                                .build();

                            ui_.text(id_ + ".message")
                                .x(metrics_.spacing.panel)
                                .y(64.0f)
                                .size(contentWidth, std::max(0.0f, height - 138.0f))
                                .text(message_)
                                .fontSize(metrics_.typography.input)
                                .lineHeight(metrics_.typography.input + metrics_.typography.lineGapComfortable)
                                .maxWidth(contentWidth)
                                .wrap(true)
                                .color(style_.message)
                                .build();

                            ui_.row(id_ + ".actions")
                                .x(std::max(metrics_.spacing.panel, width - buttonRowWidth - metrics_.spacing.panel))
                                .y(std::max(88.0f, height - 58.0f))
                                .size(buttonRowWidth, metrics_.control.control)
                                .gap(buttonGap)
                                .content([&] {
                                    components::button(ui_, id_ + ".secondary")
                                        .theme(tokens_, false)
                                        .size(buttonWidth, metrics_.control.control)
                                        .text(secondaryText_)
                                        .fontSize(metrics_.typography.body)
                                        .colors(style_.secondary,
                                                style_.secondaryHover,
                                                style_.secondaryPressed)
                                        .textColor(style_.title)
                                        .iconColor(style_.title)
                                        .radius(metrics_.radius.popup)
                                        .border(metrics_.spacing.hairline, style_.border)
                                        .shadow(0.0f, 0.0f, 0.0f, theme::color(0.0f, 0.0f, 0.0f, 0.0f))
                                        .disabled(!open_)
                                        .onClick(onSecondary)
                                        .build();

                                    components::button(ui_, id_ + ".primary")
                                        .theme(tokens_, true)
                                        .size(buttonWidth, metrics_.control.control)
                                        .text(primaryText_)
                                        .fontSize(metrics_.typography.body)
                                        .colors(style_.primary,
                                                style_.primaryHover,
                                                style_.primaryPressed)
                                        .radius(metrics_.radius.popup)
                                        .border(1.0f, theme::withAlpha(style_.primary, 0.64f))
                                        .shadow(10.0f, 0.0f, 3.0f, theme::withAlpha(style_.primary, 0.18f))
                                        .disabled(!open_)
                                        .onClick(onPrimary)
                                        .build();
                                })
                                .build();
                        }
                    })
                    .build();
            })
            .build();
    }

private:
    // Where each dialog has been dragged to. Looked up by id on every access
    // rather than handing out a reference, because inserting another dialog's
    // entry can rehash the map and invalidate one held across frames.
    static core::Vec2& dragOffset(const std::string& id) {
        static std::unordered_map<std::string, core::Vec2> offsets;
        return offsets[id];
    }

    std::function<void()> closeCallback() const {
        const std::function<void(bool)> onOpenChange = onOpenChange_;
        return [onOpenChange] {
            if (onOpenChange) {
                onOpenChange(false);
            }
        };
    }

    core::dsl::Ui& ui_;
    std::string id_;
    DialogStyle style_;
    theme::ThemeMetricTokens metrics_;
    theme::ThemeColorTokens tokens_ = theme::dark();
    core::Transition transition_ = core::Transition::make(0.16f, core::Ease::OutCubic);
    std::function<void()> onPrimary_;
    std::function<void()> onSecondary_;
    std::function<void(bool)> onOpenChange_;
    std::function<void()> content_;
    std::string title_ = "Dialog";
    std::string message_ = "Use dialogs for focused confirmation or short blocking workflows.";
    std::string primaryText_ = "Confirm";
    std::string secondaryText_ = "Cancel";
    bool open_ = false;
    bool draggable_ = true;
    float screenWidth_ = 800.0f;
    float screenHeight_ = 600.0f;
    float width_ = 420.0f;
    float height_ = 220.0f;
    int zIndex_ = 1000;
};

inline DialogBuilder dialog(core::dsl::Ui& ui, const std::string& id) {
    return DialogBuilder(ui, id);
}

} // namespace components