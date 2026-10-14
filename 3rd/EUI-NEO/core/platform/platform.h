#pragma once

#include <string>
#include <vector>

#include "core/window/window_backend.h"

namespace core::platform {

struct TrayOptions {
    std::string tooltip;
    std::string iconPath;
};

struct FileDialogOptions {
    std::string prompt;
    std::vector<std::string> allowedExtensions;
    std::string initialDirectory;
    std::string filterName;
    bool allowMultiple = false;
};

enum class FileDialogStatus {
    Selected,
    Cancelled,
    Failed
};

struct FileDialogResult {
    FileDialogStatus status = FileDialogStatus::Cancelled;
    std::vector<std::string> paths;
    std::string error;

    bool selected() const {
        return status == FileDialogStatus::Selected && !paths.empty();
    }
};

bool repairCurrentWorkingDirectory();
std::string resolveResourcePath(const std::string& path);
bool openUrl(const std::string& url);
FileDialogResult openFileDialog(const FileDialogOptions& options = {});
std::string chooseFile(const FileDialogOptions& options = {});
std::vector<std::string> chooseFiles(const FileDialogOptions& options = {});
// Pick a *directory*. A separate entry point rather than a flag on FileDialogOptions,
// because the existing dialog has no folder mode at all: openFileDialog() is built on
// GetOpenFileNameW, which cannot select a folder - Windows needs IFileOpenDialog with
// FOS_PICKFOLDERS for that. Returns an empty string when the user cancels.
std::string chooseFolder(const std::string& prompt = {});

// Whether the OS asks for animations. Windows has a "Show animations in Windows"
// switch (SPI_GETCLIENTAREAANIMATION); every other platform reports true. UI code
// should degrade to instant/cross-fade transitions when this is false.
bool animationsEnabled();

// Whether the OS itself is in dark mode (Windows: AppsUseLightTheme). Used by the
// "follow system" theme option.
bool systemPrefersDark();
bool initializeTray(const TrayOptions& options);
bool isTrayInitialized();
void pollTray(bool blocking = false);
bool consumeTrayShowRequested();
bool consumeTrayExitRequested();
void shutdownTray();
void setImeCursorRect(window::Handle window, float x, float y, float width, float height);
void requestFrame();
void requestUiUpdate();
bool consumeUiUpdate();
bool consumeFrameRequest();

} // namespace core::platform
