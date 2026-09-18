#pragma once

#include "eui/app.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace app {

template <typename WindowT>
class DslWindowManager {
public:
    using WindowPtr = std::unique_ptr<WindowT>;

    bool empty() const {
        return windows_.empty();
    }

    template <typename CreateFn>
    void createPending(const std::vector<DslWindowRequest>& requests, CreateFn&& createWindow) {
        for (const DslWindowRequest& request : requests) {
            if (!request.compose) {
                continue;
            }
            if (WindowPtr window = createWindow(request)) {
                windows_.push_back(std::move(window));
            }
        }
    }

    template <typename ClosedFn, typename DestroyFn>
    void pruneClosed(ClosedFn&& isClosed, DestroyFn&& destroyWindow) {
        windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [&](WindowPtr& window) {
            if (!window) {
                return true;
            }
            if (!isClosed(*window)) {
                return false;
            }
            destroyWindow(window);
            return true;
        }), windows_.end());
    }

    template <typename ClosedFn>
    WindowT* modalWindow(ClosedFn&& isClosed) {
        for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
            WindowPtr& window = *it;
            if (window && window->content.request().modal && !isClosed(*window)) {
                return window.get();
            }
        }
        return nullptr;
    }

    template <typename Predicate>
    WindowT* find(Predicate&& predicate) {
        for (WindowPtr& window : windows_) {
            if (window && predicate(*window)) {
                return window.get();
            }
        }
        return nullptr;
    }

    bool anyAnimating() const {
        return anyAnimating([](const WindowT&) {
            return true;
        });
    }

    template <typename ActiveFn>
    bool anyAnimating(ActiveFn&& isActive) const {
        return std::any_of(windows_.begin(), windows_.end(), [&](const WindowPtr& window) {
            return window && isActive(*window) && window->content.isAnimating();
        });
    }

    template <typename UpdateFn>
    void updateAll(UpdateFn&& updateWindow) {
        for (WindowPtr& window : windows_) {
            if (window) {
                updateWindow(*window);
            }
        }
    }

    template <typename DestroyFn>
    void destroyAll(DestroyFn&& destroyWindow) {
        for (WindowPtr& window : windows_) {
            destroyWindow(window);
        }
        windows_.clear();
    }

private:
    std::vector<WindowPtr> windows_;
};

} // namespace app
