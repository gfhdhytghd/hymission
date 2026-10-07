#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "mission_layout.hpp"
#include "stage_logic.hpp"
#include <string>
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace hymission {

// Owns persistent desktop mode and its layout, rendering and gestures. Overview
// supplies suspension state and coordinates ownership of overlapping native hooks.
class StageController {
  public:
    StageController(HANDLE handle, std::function<bool()> overviewSuspended,
                    std::function<stage::OverviewPhase(const PHLMONITOR&)> overviewPhase,
                    std::function<std::optional<stage::EdgeFrame>(const PHLWINDOW&, const PHLMONITOR&)> overviewFrame,
                    std::function<std::optional<double>(const PHLMONITOR&)> sidebarProgress,
                    std::function<bool()> inputSuppressed = {});
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
    // Overview owns the shared hooks while Stage draws its sliding previews.
    static bool renderingPreview();
    // Scope a native shadow cutout radius without changing window rules.
    static void withWindowRounding(const PHLWINDOW& window, float radius, const std::function<void()>& draw);
    static CBox transformPreviewBox(CBox box);
    static std::optional<stage::OverviewEndpoint> overviewOrigin(const PHLWINDOW& window);
    // Settled card endpoint after overview activates the requested workspace.
    static std::optional<stage::OverviewEndpoint> overviewDestination(const PHLWINDOW& window, const PHLWORKSPACE& activeWorkspace, const Rect& desktopWindow);
    static void prepareOverviewExit(const std::vector<PHLMONITOR>& monitors, const PHLWORKSPACE& activeWorkspace);
    static void finishOverview();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace hymission
