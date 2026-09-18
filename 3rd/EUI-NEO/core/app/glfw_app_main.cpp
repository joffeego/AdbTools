#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#endif

#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <GLFW/glfw3native.h>
#endif

#include "eui/app.h"
#include "eui/detail/dsl_app_impl.h"
#include "core/app/app_runner.h"
#include "core/app/dsl_window_manager.h"
#include "core/app/dsl_window_runtime.h"
#include "core/app/frame_pacing.h"
#include "core/app/main_window_runtime.h"
#include "core/input/input_state.h"
#include "core/platform/platform.h"
#include "core/window/window_backend.h"
#include "core/render/render_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace app {

// Main window handle, used by the frameless custom title-bar controls.
static GLFWwindow* g_mainWindow = nullptr;
static GLFWwindow* g_mirrorWindow = nullptr;
static int g_mirrorAspectW = 9;
static int g_mirrorAspectH = 19;
static std::function<void()> g_mirrorResizeHandler;

void setMirrorAspectRatio(int width, int height) {
    if (width > 0 && height > 0) {
        g_mirrorAspectW = width;
        g_mirrorAspectH = height;
    }
}

void setMirrorResizeHandler(std::function<void()> handler) {
    g_mirrorResizeHandler = std::move(handler);
}

void minimizeWindow() {
    if (g_mainWindow != nullptr) {
        glfwIconifyWindow(g_mainWindow);
    }
}

void toggleMaximizeWindow() {
    if (g_mainWindow == nullptr) {
        return;
    }
    if (glfwGetWindowAttrib(g_mainWindow, GLFW_MAXIMIZED) == GLFW_TRUE) {
        glfwRestoreWindow(g_mainWindow);
    } else {
        glfwMaximizeWindow(g_mainWindow);
    }
}

void closeWindow() {
    if (g_mainWindow != nullptr) {
        glfwSetWindowShouldClose(g_mainWindow, GLFW_TRUE);
    }
}

bool isWindowMaximized() {
    return g_mainWindow != nullptr &&
           glfwGetWindowAttrib(g_mainWindow, GLFW_MAXIMIZED) == GLFW_TRUE;
}

void* mainWindowHwnd() {
#ifdef _WIN32
    if (g_mainWindow != nullptr) {
        return glfwGetWin32Window(g_mainWindow);
    }
#endif
    return nullptr;
}

void setWindowSize(int width, int height) {
    if (g_mainWindow == nullptr) {
        return;
    }
    glfwSetWindowSize(g_mainWindow, width, height);
}

void startWindowDrag() {
#ifdef _WIN32
    // Drag whichever window is under the cursor so child windows (e.g. the
    // mirror window) get the same frameless drag behavior as the main window.
    POINT pt;
    GetCursorPos(&pt);
    HWND hwnd = WindowFromPoint(pt);
    if (hwnd == nullptr && g_mainWindow != nullptr) {
        hwnd = glfwGetWin32Window(g_mainWindow);
    }
    if (hwnd != nullptr) {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        // The native move loop swallows the mouse-up, leaving the input state
        // stale (so the next click is treated as a continuation). Reset it for
        // both the main window and any child (mirror) window.
        core::cancelInput(g_mainWindow);
        if (g_mirrorWindow != nullptr) {
            core::cancelInput(g_mirrorWindow);
        }
    }
#endif
}

void setUiScale(float scale) {
    const float target = scale > 0.0f ? scale : 0.0f;
    const bool first = runtimeUiScaleTarget() == 0.0f;
    const float current = uiScale();
    runtimeUiScaleFrom() = current;
    runtimeUiScaleTarget() = target;
    if (first || (current >= target - 0.001f && current <= target + 0.001f)) {
        runtimeUiScaleAnimating() = false;
    } else {
        runtimeUiScaleAnimStart() = core::window::timeSeconds();
        runtimeUiScaleAnimating() = true;
    }
    detail::requestFullPaint();
}

void setFontScale(float scale) {
    core::runtimeFontScale() = scale > 0.0f ? scale : 1.0f;
    detail::requestFullPaint();
}

void setDefaultTextFont(const std::string& fontPath) {
    if (fontPath.empty()) {
        return;
    }
    core::TextPrimitive::setDefaultFontFiles(fontPath, dslAppConfig().iconFontFileValue);
    detail::requestFullPaint();
}

} // namespace app

struct WindowState : app::AppRunner {
    bool hideToTrayRequested = false;
    bool forceClose = false;
    bool iconified = false;
    GLFWwindow* modalChildWindow = nullptr;
    // Invoked from the framebuffer-size callback so the UI keeps rendering
    // while Windows runs its modal resize loop (otherwise the main loop stays
    // blocked inside DefWindowProc until the drag ends).
    std::function<void()> renderDuringResize;
};

struct ManagedWindow {
    GLFWwindow* window = nullptr;
    WindowState state;
    app::DslWindowRuntime content;
    std::unique_ptr<core::render::RenderBackend> renderBackend;
    bool rendering = false;
};

struct TimerResolutionGuard {
    TimerResolutionGuard() {
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
    }

    ~TimerResolutionGuard() {
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    }
};

float getDpiScale(GLFWwindow* window) {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    return (scaleX + scaleY) * 0.5f;
}

#ifdef _WIN32
// Apply DWM window chrome: immersive dark mode (keeps the thin resize border
// consistent) and Windows 11 rounded corners.
void applyDarkTitleBar(GLFWwindow* window) {
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == nullptr) {
        return;
    }
    HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (dwmapi == nullptr) {
        return;
    }
    using DwmSetWindowAttributeFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    const auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    if (setAttribute != nullptr) {
        const BOOL dark = TRUE;
        // 20 = Windows 11 / Windows 10 20H1+; 19 = Windows 10 1809-1909.
        setAttribute(hwnd, 20, &dark, sizeof(dark));
        setAttribute(hwnd, 19, &dark, sizeof(dark));

        // Rounded corners on Windows 11.
        // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2.
        const int cornerPreference = 2;
        setAttribute(hwnd, 33, &cornerPreference, sizeof(cornerPreference));
    }
    FreeLibrary(dwmapi);
}

// Center the frameless window on the primary monitor's work area (a frameless
// window would otherwise default to the top-left corner).
void centerWindow(GLFWwindow* window) {
    int winWidth = 0;
    int winHeight = 0;
    glfwGetWindowSize(window, &winWidth, &winHeight);
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor == nullptr) {
        return;
    }
    int mx = 0;
    int my = 0;
    int mw = 0;
    int mh = 0;
    glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
    glfwSetWindowPos(window, mx + (mw - winWidth) / 2, my + (mh - winHeight) / 2);
}
#endif

float getPointerScale(GLFWwindow* window) {
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    if (windowWidth <= 0 || windowHeight <= 0) {
        return 1.0f;
    }

    const float scaleX = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
    const float scaleY = static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight);
    return (scaleX + scaleY) * 0.5f;
}

GLFWmonitor* getWindowMonitor(GLFWwindow* window) {
    if (GLFWmonitor* monitor = glfwGetWindowMonitor(window)) {
        return monitor;
    }

    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    GLFWmonitor* bestMonitor = glfwGetPrimaryMonitor();
    int bestArea = 0;

    for (int i = 0; i < monitorCount; ++i) {
        GLFWmonitor* monitor = monitors[i];
        int monitorX = 0;
        int monitorY = 0;
        glfwGetMonitorPos(monitor, &monitorX, &monitorY);
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (!mode) {
            continue;
        }

        const int overlapLeft = std::max(windowX, monitorX);
        const int overlapTop = std::max(windowY, monitorY);
        const int overlapRight = std::min(windowX + windowWidth, monitorX + mode->width);
        const int overlapBottom = std::min(windowY + windowHeight, monitorY + mode->height);
        const int overlapWidth = std::max(0, overlapRight - overlapLeft);
        const int overlapHeight = std::max(0, overlapBottom - overlapTop);
        const int overlapArea = overlapWidth * overlapHeight;
        if (overlapArea > bestArea) {
            bestArea = overlapArea;
            bestMonitor = monitor;
        }
    }

    return bestMonitor;
}

double getWindowRefreshRate(GLFWwindow* window) {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    HMONITOR nativeMonitor = hwnd != nullptr ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (nativeMonitor != nullptr) {
        MONITORINFOEXW monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(nativeMonitor, &monitorInfo)) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency > 1) {
                return static_cast<double>(mode.dmDisplayFrequency);
            }
        }
    }
#endif
    GLFWmonitor* monitor = getWindowMonitor(window);
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (mode && mode->refreshRate > 0) {
        return static_cast<double>(mode->refreshRate);
    }
    return 60.0;
}

void updateFrameInterval(GLFWwindow* window, WindowState& windowState, double now, bool force = false) {
    windowState.updateFrameInterval(getWindowRefreshRate(window), now, force);
}

void waitForNextFrame(GLFWwindow* window, const WindowState& windowState) {
    while (!glfwWindowShouldClose(window)) {
        const double remaining = windowState.nextFrameTime - glfwGetTime();
        if (remaining <= 0.0) {
            break;
        }

        app::detail::waitForFrameDuration(remaining);
    }
}

void hideWindowToTray(GLFWwindow* window, WindowState& windowState, core::render::RenderBackend& renderBackend) {
    if (!windowState.trayAvailable || windowState.hiddenToTray) {
        return;
    }

    core::render::ScopedRenderBackend scopedRenderBackend(renderBackend);
    app::releaseGraphicsResources();
    core::cancelInput(window);
    glfwHideWindow(window);
    windowState.hiddenToTray = true;
    windowState.hideToTrayRequested = false;
    windowState.paintRequested = false;
    windowState.renderedFrames = 0;
    windowState.nextFrameTime = glfwGetTime();
}

void restoreWindowFromTray(GLFWwindow* window, WindowState& windowState) {
    if (!windowState.hiddenToTray) {
        return;
    }

    glfwRestoreWindow(window);
    glfwShowWindow(window);
    glfwFocusWindow(window);
    windowState.hiddenToTray = false;
    windowState.hideToTrayRequested = false;
    windowState.paintRequested = true;
    app::detail::requestFullPaint();
    windowState.nextFrameTime = glfwGetTime();
}

void installWindowCallbacks(GLFWwindow* window, WindowState& windowState) {
    glfwSetWindowUserPointer(window, &windowState);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* currentWindow, int w, int h) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (state == nullptr) {
            return;
        }
        state->paintRequested = true;
        if (w > 0 && h > 0) {
            app::detail::requestFullPaint();
            if (state->renderDuringResize && !state->iconified && !state->hiddenToTray) {
                state->renderDuringResize();
            }
        }
    });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* currentWindow) {
        static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow))->paintRequested = true;
        app::detail::requestFullPaint();
    });
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow* currentWindow, float, float) {
        static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow))->paintRequested = true;
        app::detail::requestFullPaint();
    });
    glfwSetWindowFocusCallback(window, [](GLFWwindow* currentWindow, int focused) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (!state) {
            return;
        }
        if (focused != GLFW_TRUE) {
            core::cancelInput(currentWindow);
        }
        state->paintRequested = true;
        if (focused && state->modalChildWindow != nullptr && !glfwWindowShouldClose(state->modalChildWindow)) {
            glfwFocusWindow(state->modalChildWindow);
        }
    });
    glfwSetWindowIconifyCallback(window, [](GLFWwindow* currentWindow, int iconified) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (!state) {
            return;
        }
        state->iconified = iconified == GLFW_TRUE;
        if (state->iconified) {
            core::cancelInput(currentWindow);
        } else {
            state->paintRequested = true;
        }
    });
}

std::unique_ptr<ManagedWindow> createManagedWindow(const app::DslWindowRequest& request,
                                                   GLFWwindow* parentWindow,
                                                   core::render::RenderBackend& shareBackend) {
    core::window::WindowCreateRequest windowRequest;
    windowRequest.width = request.width;
    windowRequest.height = request.height;
    windowRequest.title = request.title.c_str();
    windowRequest.parent = parentWindow;
    windowRequest.renderApi = core::render::windowRenderApi();
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    GLFWwindow* childWindow = static_cast<GLFWwindow*>(core::window::createWindow(windowRequest));
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    if (!childWindow) {
        return {};
    }
#ifdef _WIN32
    // Match the main window: dark frame + Windows 11 rounded corners.
    applyDarkTitleBar(childWindow);
#endif
    if (request.title == "adb_browser_mirror") {
        app::g_mirrorWindow = childWindow;
        glfwSetWindowAspectRatio(childWindow, app::g_mirrorAspectW, app::g_mirrorAspectH);
        // The mirror window must not be maximizable: maximize/restore animation
        // is glitchy for this embedded frameless window. Strip WS_MAXIMIZEBOX so
        // neither double-click, Win+Up, nor snap-to-top can maximize it.
#ifdef _WIN32
        HWND mirrorHwnd = glfwGetWin32Window(childWindow);
        if (mirrorHwnd != nullptr) {
            const LONG style = GetWindowLongPtrW(mirrorHwnd, GWL_STYLE);
            SetWindowLongPtrW(mirrorHwnd, GWL_STYLE, style & ~WS_MAXIMIZEBOX);
        }
#endif
    }

    auto managed = std::make_unique<ManagedWindow>();
    managed->window = childWindow;
    managed->renderBackend = core::render::createRenderBackend(childWindow, &shareBackend);
    if (!managed->renderBackend) {
        core::window::destroyWindow(childWindow);
        return {};
    }
    if (!managed->renderBackend->initialize()) {
        core::window::destroyWindow(childWindow);
        return {};
    }
    managed->state.lastTitleUpdate = glfwGetTime();
    managed->state.nextFrameTime = managed->state.lastTitleUpdate;
    installWindowCallbacks(childWindow, managed->state);

    // Keep child windows live while Windows runs its modal resize loop.
    GLFWwindow* resizeWin = childWindow;
    app::DslWindowRuntime* resizeContent = &managed->content;
    core::render::RenderBackend* resizeBackend = managed->renderBackend.get();
    bool* rendering = &managed->rendering;
    managed->state.renderDuringResize = [resizeWin, resizeContent, resizeBackend, rendering] {
        if (*rendering) {
            return;
        }
        int fbw = 0;
        int fbh = 0;
        glfwGetFramebufferSize(resizeWin, &fbw, &fbh);
        if (fbw <= 0 || fbh <= 0) {
            return;
        }
        const float dpiScale = getDpiScale(resizeWin);
        const float pointerScale = getPointerScale(resizeWin);
        const float logicalWidth = static_cast<float>(fbw) / dpiScale;
        const float logicalHeight = static_cast<float>(fbh) / dpiScale;
        resizeBackend->makeCurrent();
        resizeContent->update(resizeWin, 0.0f, logicalWidth, logicalHeight, pointerScale, dpiScale, true);
        if (app::g_mirrorResizeHandler) {
            app::g_mirrorResizeHandler();
        }
        resizeBackend->beginFrame({resizeWin, core::window::nativeWindowInfo(resizeWin), fbw, fbh, dpiScale});
        resizeContent->render(*resizeBackend, fbw, fbh, dpiScale);
        resizeBackend->present();
    };

    if (!managed->content.initialize(childWindow, request)) {
        managed->renderBackend.reset();
        core::releaseInputQueue(childWindow);
        core::window::destroyWindow(childWindow);
        return {};
    }

    managed->state.paintRequested = true;
    if (managed->content.request().modal) {
        glfwFocusWindow(childWindow);
    }
    return managed;
}

void destroyManagedWindow(std::unique_ptr<ManagedWindow>& managed) {
    if (!managed || managed->window == nullptr) {
        managed.reset();
        return;
    }

    GLFWwindow* windowToDestroy = managed->window;
    if (managed->renderBackend) {
        managed->renderBackend->makeCurrent();
        managed->renderBackend->releaseRenderCache();
    }
    core::releaseInputQueue(windowToDestroy);
    if (managed->renderBackend) {
        core::render::ScopedRenderBackend scopedRenderBackend(*managed->renderBackend);
        managed->content.shutdown(false);
    } else {
        managed->content.shutdown(false);
    }
    managed->renderBackend.reset();
    core::window::destroyWindow(windowToDestroy);
    managed.reset();
}

bool updateManagedWindow(ManagedWindow& managed, float deltaSeconds, bool updateRequested) {
    if (managed.window == nullptr || glfwWindowShouldClose(managed.window)) {
        return false;
    }

    managed.renderBackend->makeCurrent();
    managed.rendering = true;

    managed.state.iconified = glfwGetWindowAttrib(managed.window, GLFW_ICONIFIED) == GLFW_TRUE;
    if (managed.state.iconified) {
        managed.rendering = false;
        managed.renderBackend->releaseRenderCache();
        managed.state.paintRequested = true;
        managed.content.requestFullPaint();
        managed.state.resetTiming(glfwGetTime());
        return true;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(managed.window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        managed.rendering = false;
        managed.renderBackend->releaseRenderCache();
        managed.state.paintRequested = true;
        managed.content.requestFullPaint();
        managed.state.resetTiming(glfwGetTime());
        return true;
    }

    const float dpiScale = getDpiScale(managed.window);
    const float pointerScale = getPointerScale(managed.window);
    const float logicalWidth = static_cast<float>(framebufferWidth) / dpiScale;
    const float logicalHeight = static_cast<float>(framebufferHeight) / dpiScale;

    if (managed.content.update(managed.window, deltaSeconds, logicalWidth, logicalHeight, pointerScale, dpiScale, updateRequested)) {
        managed.state.paintRequested = true;
    }

    if (managed.state.paintRequested || managed.content.paintRequested()) {
        managed.renderBackend->beginFrame({
            managed.window,
            core::window::nativeWindowInfo(managed.window),
            framebufferWidth,
            framebufferHeight,
            dpiScale
        });
        managed.content.render(*managed.renderBackend, framebufferWidth, framebufferHeight, dpiScale);
        managed.renderBackend->present();
        managed.state.paintRequested = false;
        ++managed.state.renderedFrames;
    }
    managed.rendering = false;
    return true;
}

bool isManagedWindowClosed(const ManagedWindow& managed) {
    return managed.window == nullptr || glfwWindowShouldClose(managed.window);
}

bool isManagedWindowRenderable(const ManagedWindow& managed) {
    if (managed.window == nullptr || glfwWindowShouldClose(managed.window)) {
        return false;
    }
    if (managed.state.iconified || glfwGetWindowAttrib(managed.window, GLFW_ICONIFIED) == GLFW_TRUE) {
        return false;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(managed.window, &framebufferWidth, &framebufferHeight);
    return framebufferWidth > 0 && framebufferHeight > 0;
}

bool anyRenderableManagedWindowAnimating(const app::DslWindowManager<ManagedWindow>& windows) {
    return windows.anyAnimating(isManagedWindowRenderable);
}

void pruneClosedWindows(app::DslWindowManager<ManagedWindow>& windows) {
    windows.pruneClosed(isManagedWindowClosed, destroyManagedWindow);
}

void createRequestedWindows(app::DslWindowManager<ManagedWindow>& windows,
                            GLFWwindow* shareWindow,
                            core::render::RenderBackend& shareBackend,
                            const std::vector<app::DslWindowRequest>& requests) {
    windows.createPending(requests, [&](const app::DslWindowRequest& request) {
        return createManagedWindow(request, shareWindow, shareBackend);
    });
    shareBackend.makeCurrent();
}

GLFWwindow* findModalChildWindow(app::DslWindowManager<ManagedWindow>& windows) {
    ManagedWindow* managed = windows.modalWindow(isManagedWindowClosed);
    return managed != nullptr ? managed->window : nullptr;
}

int eui_app_run() {
    core::platform::repairCurrentWorkingDirectory();
    core::render::initializeRenderBackendLoader();
    if (!glfwInit()) {
        return -1;
    }
    TimerResolutionGuard timerResolution;

    core::window::WindowCreateRequest windowRequest;
    // The DSL layout is expressed in logical (DPI-independent) units, while
    // GLFW window sizes are physical pixels. Scale the configured size to this
    // monitor and clamp it to the work area so it never overflows the screen.
    {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        float sx = 1.0f;
        float sy = 1.0f;
        int workW = 0;
        int workH = 0;
        if (monitor != nullptr) {
            glfwGetMonitorContentScale(monitor, &sx, &sy);
            int mx = 0;
            int my = 0;
            glfwGetMonitorWorkarea(monitor, &mx, &my, &workW, &workH);
        }
        const float scale = (sx + sy) * 0.5f;
        int w = static_cast<int>(app::initialWindowWidth() * scale);
        int h = static_cast<int>(app::initialWindowHeight() * scale);
        if (workW > 0 && w > workW) w = workW;
        if (workH > 0 && h > workH) h = workH;
        windowRequest.width = w;
        windowRequest.height = h;
    }
    windowRequest.title = app::windowTitle();
    windowRequest.renderApi = core::render::windowRenderApi();
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    // Create hidden and reveal after the first dark frame renders, so the user
    // doesn't see a white flash of an empty window on launch.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = static_cast<GLFWwindow*>(core::window::createWindow(windowRequest));
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (!window) {
        glfwTerminate();
        return -1;
    }
    app::g_mainWindow = window;
    glfwSetDropCallback(window, [](GLFWwindow*, int count, const char** paths) {
        std::vector<std::string> files;
        files.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (paths[i] != nullptr) files.emplace_back(paths[i]);
        }
        if (!files.empty()) app::dispatchDroppedFiles(files);
    });
#ifdef _WIN32
    applyDarkTitleBar(window);
    centerWindow(window);
#endif

    WindowState windowState;
    windowState.resetTiming(glfwGetTime());
    updateFrameInterval(window, windowState, windowState.lastTitleUpdate, true);
    if (app::showDebugStatsInTitle()) {
        char title[128];
        std::snprintf(title, sizeof(title), "%s - 0 FPS", app::windowTitle());
        glfwSetWindowTitle(window, title);
    }
    installWindowCallbacks(window, windowState);

    const auto cleanupMainWindow = [&] {
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        glfwTerminate();
    };

    auto renderBackend = core::render::createRenderBackend(window);
    if (!renderBackend) {
        cleanupMainWindow();
        return -1;
    }
    if (!renderBackend->initialize()) {
        cleanupMainWindow();
        return -1;
    }

    if (!app::initialize(window)) {
        app::shutdown();
        renderBackend.reset();
        cleanupMainWindow();
        return -1;
    }
    app::MainWindowRuntime mainWindowRuntime(windowState);
    windowState.initializeTray();
    glfwSetWindowCloseCallback(window, [](GLFWwindow* currentWindow) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (state && state->modalChildWindow != nullptr && !glfwWindowShouldClose(state->modalChildWindow)) {
            glfwFocusWindow(state->modalChildWindow);
            glfwSetWindowShouldClose(currentWindow, GLFW_FALSE);
            return;
        }
        if (state && state->trayAvailable && !state->forceClose) {
            state->hideToTrayRequested = true;
            glfwSetWindowShouldClose(currentWindow, GLFW_FALSE);
        }
    });
    glfwSetWindowIconifyCallback(window, [](GLFWwindow* currentWindow, int iconified) {
        WindowState* state = static_cast<WindowState*>(glfwGetWindowUserPointer(currentWindow));
        if (!state) {
            return;
        }
        state->iconified = iconified == GLFW_TRUE;
        if (iconified) {
            core::cancelInput(currentWindow);
        } else {
            state->paintRequested = true;
            app::detail::requestFullPaint();
        }
    });

    app::DslWindowManager<ManagedWindow> childWindows;

    // Guards against re-entering the render pipeline from a window-control
    // click: maximize/restore fire WM_SIZE synchronously inside runFrame.
    bool renderingFrame = false;

    // The main window is created hidden; reveal it once the first frame has
    // been rendered to avoid a white flash on launch.
    bool firstFrameShown = false;

    // Render a frame directly from the resize callback so live resizing stays
    // responsive while the OS modal size-move loop blocks the main loop.
    windowState.renderDuringResize = [&] {
        if (renderingFrame) {
            return;
        }
        int fbw = 0;
        int fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw <= 0 || fbh <= 0) {
            return;
        }
        const float dpiScale = getDpiScale(window);
        const float pointerScale = getPointerScale(window);
        mainWindowRuntime.updateAndRender(
            window,
            *renderBackend,
            {fbw, fbh, dpiScale, pointerScale},
            0.0f,
            true,
            true,
            [&] {
                createRequestedWindows(childWindows, window, *renderBackend, app::consumeWindowRequests());
                pruneClosedWindows(childWindows);
                windowState.modalChildWindow = findModalChildWindow(childWindows);
            });
    };

    while (!glfwWindowShouldClose(window)) {
        renderBackend->makeCurrent();
        windowState.pollTray(false);
        if (windowState.consumeTrayExitRequested()) {
            windowState.forceClose = true;
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            break;
        }
        if (windowState.consumeTrayShowRequested()) {
            restoreWindowFromTray(window, windowState);
        }
        pruneClosedWindows(childWindows);
        windowState.modalChildWindow = findModalChildWindow(childWindows);
        if (windowState.hideToTrayRequested && !childWindows.empty()) {
            windowState.hideToTrayRequested = false;
        }
        if (windowState.hideToTrayRequested) {
            renderBackend->releaseRenderCache();
            hideWindowToTray(window, windowState, *renderBackend);
        }
        if (windowState.hiddenToTray) {
            glfwWaitEventsTimeout(0.10);
            windowState.resetTiming(glfwGetTime());
            continue;
        }

        windowState.iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE;
        if (windowState.iconified) {
            renderBackend->releaseRenderCache();
            windowState.paintRequested = false;
            windowState.consumeFrameRequest();
            windowState.resetTiming(glfwGetTime());
            glfwWaitEventsTimeout(0.25);
            continue;
        }

        if (windowState.anyAnimating(anyRenderableManagedWindowAnimating(childWindows))) {
            waitForNextFrame(window, windowState);
        }

        const double currentFrameTime = glfwGetTime();

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth <= 0 || framebufferHeight <= 0) {
            renderBackend->releaseRenderCache();
            windowState.paintRequested = true;
            app::detail::requestFullPaint();
            windowState.consumeFrameRequest();
            glfwWaitEvents();
            mainWindowRuntime.markUnavailableFrame(glfwGetTime());
            continue;
        }

        const float dpiScale = getDpiScale(window);
        const float pointerScale = getPointerScale(window);
        const bool mainInputEnabled = windowState.modalChildWindow == nullptr;

        renderingFrame = true;
        mainWindowRuntime.runFrame(
            window,
            *renderBackend,
            {framebufferWidth, framebufferHeight, dpiScale, pointerScale},
            currentFrameTime,
            getWindowRefreshRate(window),
            mainInputEnabled,
            [&] {
                createRequestedWindows(childWindows, window, *renderBackend, app::consumeWindowRequests());
                pruneClosedWindows(childWindows);
                windowState.modalChildWindow = findModalChildWindow(childWindows);
            },
            [&](float frameDelta, bool updateRequested) {
                childWindows.updateAll([&](ManagedWindow& managed) {
                    updateManagedWindow(managed, frameDelta, updateRequested);
                });

                createRequestedWindows(childWindows, window, *renderBackend, app::consumeWindowRequests());
                pruneClosedWindows(childWindows);
                windowState.modalChildWindow = findModalChildWindow(childWindows);
            },
            [&](const char* title) {
                glfwSetWindowTitle(window, title);
            },
            [&] {
                return anyRenderableManagedWindowAnimating(childWindows);
            });
        renderingFrame = false;

        if (!firstFrameShown) {
            firstFrameShown = true;
            glfwShowWindow(window);
            glfwFocusWindow(window);
        }

        const bool anyAnimating = windowState.anyAnimating(anyRenderableManagedWindowAnimating(childWindows));
        if (anyAnimating) {
            glfwPollEvents();
        } else {
            glfwWaitEvents();
        }
    }

    childWindows.destroyAll(destroyManagedWindow);
    core::releaseInputQueue(window);
    renderBackend->makeCurrent();
    renderBackend->releaseRenderCache();
    core::platform::shutdownTray();
    {
        core::render::ScopedRenderBackend scopedRenderBackend(*renderBackend);
        app::shutdown();
    }
    renderBackend.reset();
    glfwTerminate();
    return 0;
}

#ifndef EUI_APP_RUNNER_LIBRARY
int main() {
    return eui_app_run();
}
#endif
