#pragma once

#include "eui/app.h"
#include "core/app/app_runner.h"
#include "core/app/frame_pacing.h"
#include "core/input/input_state.h"
#include "core/render/render_backend.h"
#include "core/render/render_surface.h"
#include "core/window/window_backend.h"

#include <chrono>
#include <thread>
#include <utility>

namespace app {

struct MainWindowMetrics {
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    float dpiScale = 1.0f;
    float pointerScale = 1.0f;

    bool valid() const {
        return framebufferWidth > 0 && framebufferHeight > 0 && dpiScale > 0.0f && pointerScale > 0.0f;
    }
};

class MainWindowRuntime {
public:
    explicit MainWindowRuntime(AppRunner& runner) : runner_(runner) {}

    template <typename AfterUpdateFn, typename UpdateChildrenFn, typename SetTitleFn, typename ChildAnimatingFn>
    void runFrame(core::window::Handle window,
                  core::render::RenderBackend& renderBackend,
                  const MainWindowMetrics& metrics,
                  double now,
                  double refreshRate,
                  bool inputEnabled,
                  AfterUpdateFn&& afterUpdate,
                  UpdateChildrenFn&& updateChildren,
                  SetTitleFn&& setTitle,
                  ChildAnimatingFn&& childAnimating) {
        runner_.updateFrameInterval(refreshRate, now);
        const bool updateRequested = runner_.consumeUpdateRequest();
        const bool frameWakeRequested = runner_.consumeFrameRequest();
        const bool frameRequested =
            runner_.paintRequested ||
            updateRequested ||
            frameWakeRequested ||
            runner_.anyAnimating(childAnimating()) ||
            core::hasPendingPointerInput(window, metrics.pointerScale);
        while (frameRequested) {
            const double remaining = runner_.nextFrameTime - core::window::timeSeconds();
            if (remaining <= 0.0) {
                break;
            }
            app::detail::waitForFrameDuration(remaining);
        }

        const double frameTime = core::window::timeSeconds();
        const float deltaSeconds = runner_.consumeFrameDelta(frameTime);

        updateAndRender(window,
                        renderBackend,
                        metrics,
                        deltaSeconds,
                        updateRequested,
                        inputEnabled,
                        std::forward<AfterUpdateFn>(afterUpdate));

        updateChildren(deltaSeconds, updateRequested);
        runner_.updateFrameTitle(core::window::timeSeconds(), std::forward<SetTitleFn>(setTitle));
        runner_.advanceFrameClock(core::window::timeSeconds(), runner_.anyAnimating(childAnimating()));
    }

    void markUnavailableFrame(double now) {
        runner_.paintRequested = true;
        runner_.resetTiming(now);
    }

    template <typename AfterUpdateFn>
    bool updateAndRender(core::window::Handle window,
                         core::render::RenderBackend& renderBackend,
                         const MainWindowMetrics& metrics,
                         float deltaSeconds,
                         bool updateRequested,
                         bool inputEnabled,
                         AfterUpdateFn&& afterUpdate) {
        if (!metrics.valid()) {
            runner_.paintRequested = true;
            return false;
        }

        renderBackend.makeCurrent();
        if (app::update(window,
                        deltaSeconds,
                        metrics.framebufferWidth,
                        metrics.framebufferHeight,
                        metrics.dpiScale,
                        metrics.pointerScale,
                        updateRequested,
                        inputEnabled)) {
            runner_.paintRequested = true;
        }

        afterUpdate();

        if (!runner_.paintRequested) {
            return false;
        }

        const auto renderStart = std::chrono::steady_clock::now();
        renderBackend.beginFrame({
            window,
            core::window::nativeWindowInfo(window),
            metrics.framebufferWidth,
            metrics.framebufferHeight,
            metrics.dpiScale
        });
        core::render::ScopedRenderBackend scopedRenderBackend(renderBackend);
        app::render(metrics.framebufferWidth, metrics.framebufferHeight, metrics.dpiScale);
        renderBackend.present();
        core::render::publishRenderFrameStats();
        runner_.recordRenderStats(core::render::lastRenderFrameStats());
        const auto renderEnd = std::chrono::steady_clock::now();
        runner_.recordRenderDuration(std::chrono::duration<double, std::milli>(renderEnd - renderStart).count());
        runner_.markRendered();
        return true;
    }

private:
    AppRunner& runner_;
};

} // namespace app
