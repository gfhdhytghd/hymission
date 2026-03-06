#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "mission_layout.hpp"
#include "overview_logic.hpp"

namespace hymission {

class OverviewController {
  public:
    explicit OverviewController(HANDLE handle);
    ~OverviewController();

    OverviewController(const OverviewController&) = delete;
    OverviewController& operator=(const OverviewController&) = delete;

    bool initialize();

    [[nodiscard]] SDispatchResult open();
    [[nodiscard]] SDispatchResult close();
    [[nodiscard]] SDispatchResult toggle();
    [[nodiscard]] SDispatchResult debugCurrentLayout() const;

    void renderStage(eRenderStage stage);
    void handleMouseMove();
    void handleMouseButton(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info);
    void handleKeyboard(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info);
    void handleTick();
    void handleWindowSetChange(PHLWINDOW window);
    void handleWorkspaceChange();
    void handleMonitorChange(PHLMONITOR monitor);

    void renderWindowHook(void* rendererThisptr, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& now, bool decorate, eRenderPassMode passMode, bool ignorePosition,
                          bool standalone);
    void renderWorkspaceWindowsHook(void* rendererThisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now);
    void renderWorkspaceWindowsFullscreenHook(void* rendererThisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now);

  private:
    enum class Phase {
        Inactive,
        Opening,
        Active,
        Closing,
    };

    struct ManagedWindow {
        PHLWINDOW    window;
        std::string  title;
        Rect         naturalGlobal;
        Rect         targetGlobal;
        WindowSlot   slot;
        bool         isFloating = false;
        bool         isPinned = false;
    };

    struct State {
        Phase                                  phase = Phase::Inactive;
        PHLMONITOR                             ownerMonitor;
        PHLWORKSPACE                           ownerWorkspace;
        PHLWINDOW                              focusBeforeOpen;
        PHLWINDOW                              focusDuringOverview;
        std::vector<ManagedWindow>             windows;
        std::vector<WindowSlot>                slots;
        std::optional<std::size_t>             hoveredIndex;
        std::optional<std::size_t>             selectedIndex;
        double                                 animationProgress = 0.0;
        std::chrono::steady_clock::time_point  animationStart = {};
    };

    using RenderWindowFn = void (*)(void*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, eRenderPassMode, bool, bool);
    using RenderWorkspaceWindowsFn = void (*)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);

    [[nodiscard]] LayoutConfig loadLayoutConfig() const;
    [[nodiscard]] bool         focusFollowsMouseEnabled() const;
    [[nodiscard]] bool         isScrollingWorkspace(const PHLWORKSPACE& workspace) const;
    void                       setScrollingFollowFocusOverride(bool disable);
    [[nodiscard]] bool         installHooks();
    [[nodiscard]] bool         activateHooks();
    void                       deactivateHooks();
    [[nodiscard]] bool         hookFunction(const std::string& symbolName, const std::string& demangledNeedle, CFunctionHook*& hook, void* destination);
    [[nodiscard]] void*        findFunction(const std::string& symbolName, const std::string& demangledNeedle) const;

    [[nodiscard]] bool         isAnimating() const;
    [[nodiscard]] bool         isVisible() const;
    [[nodiscard]] bool         shouldHandleInput() const;
    [[nodiscard]] bool         ownsMonitor(const PHLMONITOR& monitor) const;
    [[nodiscard]] bool         ownsWorkspace(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         hasManagedWindow(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldAutoCloseFor(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldManageWindow(const PHLWINDOW& window, const PHLMONITOR& monitor) const;
    [[nodiscard]] std::string  collectionSummary(const PHLMONITOR& monitor) const;
    [[nodiscard]] std::vector<Rect> targetRects() const;
    [[nodiscard]] std::optional<std::size_t> hitTestTarget(double x, double y) const;
    [[nodiscard]] Rect         currentPreviewRect(const ManagedWindow& window) const;
    [[nodiscard]] double       visualProgress() const;

    void beginOpen(const PHLMONITOR& monitor);
    void beginClose();
    void deactivate();
    void refreshScene(const PHLMONITOR& monitor, const std::vector<ManagedWindow>& windows) const;
    void updateAnimation();
    void updateFocusPolicy();
    void updateHoveredFromPointer();
    void moveSelection(Direction direction);
    void activateSelection();
    void notify(const std::string& message, const CHyprColor& color, float durationMs) const;
    void renderBackdrop() const;
    void renderSelectionChrome() const;
    void renderOutline(const Rect& rect, const CHyprColor& color, double thickness) const;
    void renderManagedWorkspace(void* rendererThisptr, const PHLMONITOR& monitor, const PHLWORKSPACE& workspace, const Time::steady_tp& now);

    State  buildState(const PHLMONITOR& monitor) const;
    State  m_state;
    HANDLE m_handle = nullptr;

    CFunctionHook*            m_renderWindowHook = nullptr;
    CFunctionHook*            m_renderWorkspaceWindowsHook = nullptr;
    CFunctionHook*            m_renderWorkspaceWindowsFullscreenHook = nullptr;
    RenderWindowFn            m_renderWindowOriginal = nullptr;
    RenderWorkspaceWindowsFn  m_renderWorkspaceWindowsOriginal = nullptr;
    RenderWorkspaceWindowsFn  m_renderWorkspaceWindowsFullscreenOriginal = nullptr;
    bool                      m_hooksActive = false;
    bool                      m_scrollingFollowFocusOverridden = false;
    long                      m_scrollingFollowFocusBackup = 1;

    CHyprSignalListener       m_renderStageListener;
    CHyprSignalListener       m_mouseMoveListener;
    CHyprSignalListener       m_mouseButtonListener;
    CHyprSignalListener       m_keyboardListener;
    CHyprSignalListener       m_tickListener;
    CHyprSignalListener       m_windowOpenListener;
    CHyprSignalListener       m_windowDestroyListener;
    CHyprSignalListener       m_windowCloseListener;
    CHyprSignalListener       m_windowMoveWorkspaceListener;
    CHyprSignalListener       m_workspaceActiveListener;
    CHyprSignalListener       m_monitorRemovedListener;
    CHyprSignalListener       m_monitorFocusedListener;
};

} // namespace hymission
