#pragma once

#include <functional>
#include <memory>
#include <optional>
#include "mission_layout.hpp"
#include <string>
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace hymission {

// Owns persistent desktop mode and its layout, rendering and gestures. Overview
// supplies suspension state and coordinates ownership of overlapping native hooks.
class StageController {
  public:
    StageController(HANDLE handle, std::function<bool()> overviewSuspended);
    ~StageController();
    StageController(const StageController&) = delete;
    StageController& operator=(const StageController&) = delete;
    void initialize();
    std::string stateJson() const;
    // Overview owns the native swipe entry points; Stage consumes only its own
    // gestures. Surface hook ownership is handed over before overview attaches.
    static bool beginWorkspaceSwipe(void* gesture, void (*original)(void*));
    static bool updateWorkspaceSwipe(void* gesture, double delta);
    static bool endWorkspaceSwipe(void* gesture);
    // The registered trackpad gesture routes directly, without native hooks.
    static bool beginTrackpadWorkspaceSwipe();
    static void updateTrackpadWorkspaceSwipe(double delta);
    static void endTrackpadWorkspaceSwipe(bool cancelled);
    static void setOverviewRendering(bool active);
    static std::optional<Rect> overviewOrigin(const PHLWINDOW& window);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace hymission
