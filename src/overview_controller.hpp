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
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>

#include "mission_layout.hpp"
#include "overview_logic.hpp"

class CEventLoopTimer;

namespace hymission {

class OverviewController {
  public:
    enum class ScopeOverride {
        Default,
        OnlyCurrentWorkspace,
        ForceAll,
    };

    explicit OverviewController(HANDLE handle);
    ~OverviewController();

    OverviewController(const OverviewController&) = delete;
    OverviewController& operator=(const OverviewController&) = delete;

    bool initialize();

    [[nodiscard]] SDispatchResult open(const std::string& args = {});
    [[nodiscard]] SDispatchResult close();
    [[nodiscard]] SDispatchResult toggle(const std::string& args = {});
    [[nodiscard]] SDispatchResult debugCurrentLayout() const;
    [[nodiscard]] bool            allowsWorkspaceSwitchInOverviewForGestures() const;
    [[nodiscard]] bool            blocksWorkspaceSwitchInOverviewForGestures() const;
    [[nodiscard]] bool            beginOverviewWorkspaceSwipeGesture(eTrackpadGestureDirection direction);
    void                          updateOverviewWorkspaceSwipeGesture(double delta);
    void                          endOverviewWorkspaceSwipeGesture(bool cancelled);

    void renderStage(eRenderStage stage);
    void handleMouseMove();
    void handleMouseButton(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info);
    void handleKeyboard(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info);
    void handleWindowSetChange(PHLWINDOW window);
    void handleWorkspaceChange(PHLWORKSPACE workspace);
    void handleMonitorChange(PHLMONITOR monitor);
    bool                shouldRenderWindowHook(const PHLWINDOW& window, const PHLMONITOR& monitor);
    void                borderDrawHook(void* borderDecorationThisptr, const PHLMONITOR& monitor, const float& alpha);
    void                shadowDrawHook(void* shadowDecorationThisptr, const PHLMONITOR& monitor, const float& alpha);
    void                calculateUVForSurfaceHook(const PHLWINDOW& window, SP<CWLSurfaceResource> surface, const PHLMONITOR& monitor, bool main, const Vector2D& projSize,
                                                  const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1);
    [[nodiscard]] SDispatchResult fullscreenDispatcherHook(std::string args);
    [[nodiscard]] SDispatchResult fullscreenStateDispatcherHook(std::string args);
    [[nodiscard]] SDispatchResult changeWorkspaceDispatcherHook(std::string args);
    [[nodiscard]] SDispatchResult focusWorkspaceOnCurrentMonitorDispatcherHook(std::string args);
    bool                surfaceNeedsLiveBlurHook(void* surfacePassThisptr);
    bool                surfaceNeedsPrecomputeBlurHook(void* surfacePassThisptr);
    void                surfaceDrawHook(void* surfacePassThisptr, const CRegion& damage);
    CBox                surfaceTexBoxHook(void* surfacePassThisptr);
    std::optional<CBox> surfaceBoundingBoxHook(void* surfacePassThisptr);
    CRegion             surfaceOpaqueRegionHook(void* surfacePassThisptr);
    CRegion             surfaceVisibleRegionHook(void* surfacePassThisptr, bool& cancel);
    std::optional<std::string> handleGestureConfigHook(const std::string& keyword, const std::string& value);
    [[nodiscard]] bool         beginTrackpadGesture(bool openOnly, ScopeOverride requestedScope, eTrackpadGestureDirection direction,
                                                    const IPointer::SSwipeUpdateEvent& event, float deltaScale);
    void                       updateTrackpadGesture(const IPointer::SSwipeUpdateEvent& event);
    void                       endTrackpadGesture(bool cancelled);
    void                       workspaceSwipeBeginHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e);
    void                       workspaceSwipeUpdateHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e);
    void                       workspaceSwipeEndHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e);
  private:
    enum class Phase {
        Inactive,
        Opening,
        Active,
        ClosingSettle,
        Closing,
    };

    enum class CloseMode {
        Normal,
        ActivateSelection,
        Abort,
    };

    enum class PostCloseDispatcher {
        None,
        Fullscreen,
        FullscreenState,
    };

    struct CollectionPolicy {
        ScopeOverride requestedScope = ScopeOverride::Default;
        bool          onlyActiveWorkspace = false;
        bool          onlyActiveMonitor = true;
        bool          includeSpecial = false;
    };

    struct FullscreenWorkspaceBackup {
        PHLWORKSPACE    workspace;
        PHLWINDOW       originalFullscreenWindow;
        bool            hadFullscreenWindow = false;
        eFullscreenMode fullscreenMode = FSMODE_NONE;
        eFullscreenMode originalFullscreenMode = FSMODE_NONE;
    };

    struct ManagedWindow {
        PHLWINDOW    window;
        PHLMONITOR   targetMonitor;
        std::string  title;
        Rect         naturalGlobal;
        Rect         exitGlobal;
        Rect         relayoutFromGlobal;
        Rect         targetGlobal;
        WindowSlot   slot;
        float        previewAlpha = 1.0F;
        bool         isFloating = false;
        bool         isPinned = false;
    };

    struct State {
        Phase                                  phase = Phase::Inactive;
        PHLMONITOR                             ownerMonitor;
        PHLWORKSPACE                           ownerWorkspace;
        CollectionPolicy                       collectionPolicy;
        std::vector<PHLMONITOR>                participatingMonitors;
        std::vector<PHLWORKSPACE>              managedWorkspaces;
        std::vector<FullscreenWorkspaceBackup> fullscreenBackups;
        PHLWINDOW                              focusBeforeOpen;
        PHLWINDOW                              focusDuringOverview;
        PHLWINDOW                              pendingExitFocus;
        CloseMode                              closeMode = CloseMode::Normal;
        bool                                   fullscreenOverrideActive = false;
        std::vector<ManagedWindow>             windows;
        std::vector<ManagedWindow>             transientClosingWindows;
        std::vector<WindowSlot>                slots;
        std::optional<std::size_t>             hoveredIndex;
        std::optional<std::size_t>             selectedIndex;
        double                                 animationProgress = 0.0;
        double                                 animationFromVisual = 0.0;
        double                                 animationToVisual = 0.0;
        double                                 relayoutProgress = 1.0;
        bool                                   relayoutActive = false;
        std::size_t                            settleStableFrames = 0;
        bool                                   settleHasSample = false;
        bool                                   exitFullscreenReapplied = false;
        bool                                   deferredFullscreenWorkspaceClear = false;
        bool                                   deferredHiddenFullscreenReapply = false;
        std::chrono::steady_clock::time_point  animationStart = {};
        std::chrono::steady_clock::time_point  relayoutStart = {};
        std::chrono::steady_clock::time_point  settleStart = {};
    };

    struct SurfaceRenderDataSnapshot {
        Vector2D pos;
        Vector2D localPos;
        double   w = 0.0;
        double   h = 0.0;
        float    alpha = 1.0F;
        float    fadeAlpha = 1.0F;
        bool     blur = false;
        bool     blockBlurOptimization = false;
        CBox     clipBox = {};
    };

    struct WindowTransform {
        Rect   actualGlobal;
        Rect   targetGlobal;
        double scaleX = 1.0;
        double scaleY = 1.0;
    };

    struct GestureRegistration {
        std::size_t               fingerCount = 0;
        eTrackpadGestureDirection direction = TRACKPAD_GESTURE_DIR_NONE;
        uint32_t                  modMask = 0;
        float                     deltaScale = 1.0F;
        bool                      disableInhibit = false;
    };

    struct GestureSession {
        bool         active = false;
        bool         opening = true;
        ScopeOverride requestedScope = ScopeOverride::Default;
        eTrackpadGestureDirection direction = TRACKPAD_GESTURE_DIR_VERTICAL;
        double       openness = 0.0;
        double       lastSignedSpeed = 0.0;
        float        deltaScale = 1.0F;
    };

    struct WorkspaceNameBackup {
        PHLWORKSPACE workspace;
        std::string  name;
    };

    struct WorkspaceOverride {
        MONITORID    monitorId = MONITOR_INVALID;
        PHLWORKSPACE workspace;
        WORKSPACEID  workspaceId = WORKSPACE_INVALID;
        std::string  workspaceName;
        bool         syntheticEmpty = false;
    };

    enum class WorkspaceTransitionAxis {
        Horizontal,
        Vertical,
    };

    enum class WorkspaceTransitionMode {
        Gesture,
        TimedCommit,
        TimedRevert,
    };

    struct WorkspaceTransition {
        bool                                  active = false;
        PHLMONITOR                             monitor;
        eTrackpadGestureDirection              gestureDirection = TRACKPAD_GESTURE_DIR_NONE;
        WorkspaceTransitionAxis                axis = WorkspaceTransitionAxis::Horizontal;
        WorkspaceTransitionMode                mode = WorkspaceTransitionMode::Gesture;
        double                                 distance = 1.0;
        double                                 delta = 0.0;
        int                                    step = 0;
        int                                    initialDirection = 0;
        double                                 avgSpeed = 0.0;
        int                                    speedPoints = 0;
        WORKSPACEID                            targetWorkspaceId = WORKSPACE_INVALID;
        std::string                            targetWorkspaceName;
        bool                                   targetWorkspaceSyntheticEmpty = false;
        State                                  sourceState;
        State                                  targetState;
        double                                 animationFromDelta = 0.0;
        double                                 animationToDelta = 0.0;
        double                                 animationProgress = 0.0;
        std::chrono::steady_clock::time_point  animationStart = {};
    };

    struct WorkspaceSwipeGestureContext {
        bool                      active = false;
        PHLMONITOR                monitor;
        eTrackpadGestureDirection direction = TRACKPAD_GESTURE_DIR_NONE;
    };

    using SurfaceGetTexBoxFn = CBox (*)(void*);
    using SurfaceBoundingBoxFn = std::optional<CBox> (*)(void*);
    using SurfaceOpaqueRegionFn = CRegion (*)(void*);
    using SurfaceVisibleRegionFn = CRegion (*)(void*, bool&);
    using SurfaceDrawFn = void (*)(void*, const CRegion&);
    using SurfaceBlurNeedsFn = bool (*)(void*);
    using ShouldRenderWindowFn = bool (*)(void*, PHLWINDOW, PHLMONITOR);
    using BorderDrawFn = void (*)(void*, PHLMONITOR, const float&);
    using CalculateUVForSurfaceFn = void (*)(void*, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool, const Vector2D&, const Vector2D&, bool);
    using DispatcherFn = SDispatchResult (*)(std::string);
    using WorkspaceSwipeBeginFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureBegin&);
    using WorkspaceSwipeUpdateFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureUpdate&);
    using WorkspaceSwipeEndFn = void (*)(void*, const ITrackpadGesture::STrackpadGestureEnd&);
    using HandleGestureFn = std::optional<std::string> (*)(void*, const std::string&, const std::string&);
    [[nodiscard]] LayoutConfig loadLayoutConfig() const;
    [[nodiscard]] CollectionPolicy loadCollectionPolicy(ScopeOverride requestedScope) const;
    [[nodiscard]] std::optional<ScopeOverride> parseScopeOverride(const std::string& args, std::string& error) const;
    [[nodiscard]] bool         focusFollowsMouseEnabled() const;
    [[nodiscard]] bool         gestureInvertVerticalEnabled() const;
    [[nodiscard]] bool         workspaceSwipeInvertEnabled() const;
    [[nodiscard]] bool         workspaceChangeKeepsOverviewEnabled() const;
    [[nodiscard]] bool         debugLogsEnabled() const;
    [[nodiscard]] bool         debugSurfaceLogsEnabled() const;
    [[nodiscard]] bool         isScrollingWorkspace(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         hasScrollingWorkspace() const;
    [[nodiscard]] bool         shouldSyncRealFocusDuringOverview() const;
    [[nodiscard]] bool         allowsWorkspaceSwitchInOverview() const;
    [[nodiscard]] bool         shouldBlockWorkspaceSwitchInOverview() const;
    [[nodiscard]] bool         shouldOverrideWorkspaceNames(const State& state) const;
    [[nodiscard]] int          resolveOverviewWorkspaceSwipeStep(eTrackpadGestureDirection direction, double totalDelta, double lastDelta) const;
    [[nodiscard]] bool         switchOverviewWorkspaceByStep(int step);
    [[nodiscard]] double       gestureSwipeDistance() const;
    [[nodiscard]] double       gestureForceSpeedThreshold() const;
    [[nodiscard]] bool         gestureSwipeForeverEnabled() const;
    [[nodiscard]] bool         gestureSwipeCreateNewEnabled() const;
    [[nodiscard]] bool         gestureSwipeUseRelativeEnabled() const;
    [[nodiscard]] bool         gestureSwipeDirectionLockEnabled() const;
    [[nodiscard]] double       gestureSwipeDirectionLockThreshold() const;
    void                       setInputFollowMouseOverride(bool disable);
    void                       setScrollingFollowFocusOverride(bool disable);
    void                       setAnimationsEnabledOverride(bool disable, std::optional<std::chrono::milliseconds> restoreDelay = std::nullopt);
    void                       applyWorkspaceNameOverrides(const State& state);
    void                       restoreWorkspaceNameOverrides();
    void                       clearRegisteredTrackpadGestures();
    void                       rememberRegisteredTrackpadGesture(const GestureRegistration& gesture);
    [[nodiscard]] bool         installHooks();
    [[nodiscard]] bool         activateHooks();
    void                       deactivateHooks();
    [[nodiscard]] bool         hookFunction(const std::string& symbolName, const std::string& demangledNeedle, CFunctionHook*& hook, void* destination);
    [[nodiscard]] void*        findFunction(const std::string& symbolName, const std::string& demangledNeedle) const;

    [[nodiscard]] bool         isAnimating() const;
    [[nodiscard]] bool         isVisible() const;
    [[nodiscard]] bool         shouldHandleInput() const;
    [[nodiscard]] std::vector<PHLMONITOR> ownedMonitors() const;
    [[nodiscard]] bool         ownsMonitor(const PHLMONITOR& monitor) const;
    [[nodiscard]] bool         ownsWorkspace(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] bool         hasManagedWindow(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldAutoCloseFor(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldManageWindow(const PHLWINDOW& window, const State& state) const;
    [[nodiscard]] std::string  collectionSummary(const PHLMONITOR& monitor) const;
    [[nodiscard]] std::vector<Rect> targetRects() const;
    [[nodiscard]] const ManagedWindow* managedWindowFor(const State& state, const PHLWINDOW& window, bool includeTransient = false) const;
    [[nodiscard]] const ManagedWindow* managedWindowFor(const PHLWINDOW& window) const;
    [[nodiscard]] PHLWINDOW     selectedWindow() const;
    [[nodiscard]] float        managedPreviewAlphaFor(const PHLWINDOW& window, float fallback = 1.0F) const;
    [[nodiscard]] PHLMONITOR   preferredMonitorForWindow(const PHLWINDOW& window, const State& state) const;
    [[nodiscard]] PHLMONITOR   previewMonitorForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] const FullscreenWorkspaceBackup* fullscreenBackupForWorkspace(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] const FullscreenWorkspaceBackup* fullscreenBackupForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] Rect         liveGlobalRectForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] Rect         goalGlobalRectForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldUseGoalGeometryForStateSnapshot(const PHLWINDOW& window) const;
    void                       refreshWorkspaceLayoutSnapshot(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] std::optional<Vector2D> predictedScrollingExitTranslation(const PHLWINDOW& window) const;
    void                       prepareGestureCloseExitGeometry();
    [[nodiscard]] bool         workspaceSwipeUsesVerticalAxis(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] double       workspaceSwipeViewportDistance(const PHLMONITOR& monitor, WorkspaceTransitionAxis axis) const;
    [[nodiscard]] std::optional<Rect> workspaceTransitionRectForWindow(const PHLWINDOW& window) const;
    [[nodiscard]] bool         resolveOverviewWorkspaceTargetByStep(const PHLMONITOR& monitor, int step, WORKSPACEID& workspaceId, std::string& workspaceName,
                                                                    PHLWORKSPACE& workspace, bool& syntheticEmpty) const;
    [[nodiscard]] bool         beginOverviewWorkspaceTransition(const PHLMONITOR& monitor, WORKSPACEID workspaceId, std::string workspaceName, PHLWORKSPACE workspace,
                                                               bool syntheticEmpty, WorkspaceTransitionMode mode);
    [[nodiscard]] bool         startOverviewWorkspaceTransitionByStep(const PHLMONITOR& monitor, int step, WorkspaceTransitionMode mode);
    void                       updateOverviewWorkspaceTransition();
    void                       commitOverviewWorkspaceTransition(bool followGesture = false);
    void                       clearOverviewWorkspaceTransition();
    [[nodiscard]] SDispatchResult startOverviewWorkspaceTransitionForDispatcher(const std::string& args, bool currentMonitorOnly);
    [[nodiscard]] std::optional<WindowTransform> windowTransformFor(const PHLWINDOW& window, const PHLMONITOR& monitor) const;
    [[nodiscard]] bool                          transformSurfaceRenderDataForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor,
                                                                                   CSurfacePassElement::SRenderData& renderData) const;
    [[nodiscard]] bool                          shouldSuppressSurfaceBlur(void* surfacePassThisptr) const;
    [[nodiscard]] bool                          prepareSurfaceRenderData(void* surfacePassThisptr, const char* context, CSurfacePassElement::SRenderData*& renderData,
                                                                         PHLMONITOR& monitor, SurfaceRenderDataSnapshot& snapshot) const;
    void                                        restoreSurfaceRenderData(CSurfacePassElement::SRenderData* renderData, const SurfaceRenderDataSnapshot& snapshot) const;
    [[nodiscard]] std::optional<std::size_t> hitTestTarget(double x, double y) const;
    [[nodiscard]] Rect         currentPreviewRect(const ManagedWindow& window) const;
    [[nodiscard]] double       visualProgress() const;
    [[nodiscard]] double       relayoutVisualProgress() const;
    [[nodiscard]] bool         transformBoxForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, CBox& box, bool scaled) const;
    [[nodiscard]] CRegion      transformRegionForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, const CRegion& region, bool scaled) const;
    [[nodiscard]] PHLWINDOW    resolveExitFocus(CloseMode mode) const;
    [[nodiscard]] bool         exitFocusChangedWorkspace(const PHLWINDOW& window) const;
    [[nodiscard]] bool         shouldPreferGoalExitGeometry(const PHLWINDOW& window) const;
    [[nodiscard]] std::optional<Vector2D> visiblePointForWindowOnMonitor(const PHLWINDOW& window, const PHLMONITOR& monitor, bool preferGoal = false) const;
    [[nodiscard]] bool         clearWorkspaceFullscreenForExitTarget(const PHLWINDOW& window);
    [[nodiscard]] bool         shouldClearWorkspaceFullscreenForExitTarget(const PHLWINDOW& window) const;
    void                       commitOverviewExitFocus(const PHLWINDOW& window);
    [[nodiscard]] bool         syncScrollingWorkspaceSpotOnWindow(const PHLWINDOW& window) const;
    void                       refreshExitLayoutForFocus(const PHLWINDOW& window) const;
    void                       syncRealFocusDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot = true);
    void                       syncFocusDuringOverviewFromSelection(bool syncScrollingSpot = true);
    [[nodiscard]] bool         matchesPendingLiveFocusWorkspaceChange(const PHLWORKSPACE& workspace) const;
    void                       clearPostCloseForcedFocus();
    void                       clearPostCloseDispatcher();
    void                       queuePostCloseDispatcher(PostCloseDispatcher dispatcher, std::string args);
    [[nodiscard]] SDispatchResult runHookedDispatcher(PostCloseDispatcher dispatcher, std::string args);
    void                       setFullscreenRenderOverride(bool suppress);

    void beginOpen(const PHLMONITOR& monitor, ScopeOverride requestedScope);
    void beginClose(CloseMode mode = CloseMode::Normal, std::optional<double> fromVisualOverride = std::nullopt, bool deferFullscreenMutations = false);
    void deactivate();
    void refreshScene(const PHLMONITOR& monitor) const;
    void refreshOwnedMonitors() const;
    void damageOwnedMonitor() const;
    void updateAnimation();
    void updateHoveredFromPointer(bool syncSelection = true, bool syncRealFocus = true, bool syncScrollingSpot = true);
    void rebuildVisibleState();
    void moveSelection(Direction direction);
    void activateSelection();
    void notify(const std::string& message, const CHyprColor& color, float durationMs) const;
    void debugLog(const std::string& message) const;
    void debugSurfaceLog(const std::string& message) const;
    [[nodiscard]] std::string debugWorkspaceLabel(const PHLWORKSPACE& workspace) const;
    [[nodiscard]] std::string debugWindowLabel(const PHLWINDOW& window) const;
    void renderBackdrop() const;
    void renderSelectionChrome() const;
    void renderOutline(const Rect& rect, const CHyprColor& color, double thickness) const;
    State  buildState(const PHLMONITOR& monitor, ScopeOverride requestedScope, const std::vector<WorkspaceOverride>& workspaceOverrides = {},
                      bool keepEmptyParticipatingMonitors = false) const;
    State  m_state;
    HANDLE m_handle = nullptr;

    CFunctionHook*            m_surfaceTexBoxHook = nullptr;
    CFunctionHook*            m_surfaceBoundingBoxHook = nullptr;
    CFunctionHook*            m_surfaceOpaqueRegionHook = nullptr;
    CFunctionHook*            m_surfaceVisibleRegionHook = nullptr;
    CFunctionHook*            m_surfaceDrawHook = nullptr;
    CFunctionHook*            m_surfaceNeedsLiveBlurHook = nullptr;
    CFunctionHook*            m_surfaceNeedsPrecomputeBlurHook = nullptr;
    CFunctionHook*            m_shouldRenderWindowHook = nullptr;
    CFunctionHook*            m_borderDrawHook = nullptr;
    CFunctionHook*            m_shadowDrawHook = nullptr;
    CFunctionHook*            m_calculateUVForSurfaceHook = nullptr;
    CFunctionHook*            m_fullscreenActiveHook = nullptr;
    CFunctionHook*            m_fullscreenStateActiveHook = nullptr;
    CFunctionHook*            m_changeWorkspaceHook = nullptr;
    CFunctionHook*            m_focusWorkspaceOnCurrentMonitorHook = nullptr;
    CFunctionHook*            m_workspaceSwipeBeginFunctionHook = nullptr;
    CFunctionHook*            m_workspaceSwipeUpdateFunctionHook = nullptr;
    CFunctionHook*            m_workspaceSwipeEndFunctionHook = nullptr;
    CFunctionHook*            m_handleGestureHook = nullptr;
    SurfaceGetTexBoxFn        m_surfaceTexBoxOriginal = nullptr;
    SurfaceBoundingBoxFn      m_surfaceBoundingBoxOriginal = nullptr;
    SurfaceOpaqueRegionFn     m_surfaceOpaqueRegionOriginal = nullptr;
    SurfaceVisibleRegionFn    m_surfaceVisibleRegionOriginal = nullptr;
    SurfaceDrawFn             m_surfaceDrawOriginal = nullptr;
    SurfaceBlurNeedsFn        m_surfaceNeedsLiveBlurOriginal = nullptr;
    SurfaceBlurNeedsFn        m_surfaceNeedsPrecomputeBlurOriginal = nullptr;
    ShouldRenderWindowFn      m_shouldRenderWindowOriginal = nullptr;
    BorderDrawFn              m_borderDrawOriginal = nullptr;
    BorderDrawFn              m_shadowDrawOriginal = nullptr;
    CalculateUVForSurfaceFn   m_calculateUVForSurfaceOriginal = nullptr;
    DispatcherFn              m_fullscreenActiveOriginal = nullptr;
    DispatcherFn              m_fullscreenStateActiveOriginal = nullptr;
    DispatcherFn              m_changeWorkspaceOriginal = nullptr;
    DispatcherFn              m_focusWorkspaceOnCurrentMonitorOriginal = nullptr;
    WorkspaceSwipeBeginFn     m_workspaceSwipeBeginOriginal = nullptr;
    WorkspaceSwipeUpdateFn    m_workspaceSwipeUpdateOriginal = nullptr;
    WorkspaceSwipeEndFn       m_workspaceSwipeEndOriginal = nullptr;
    HandleGestureFn           m_handleGestureOriginal = nullptr;
    bool                      m_hooksActive = false;
    bool                      m_inputFollowMouseOverridden = false;
    long                      m_inputFollowMouseBackup = 1;
    bool                      m_restoreInputFollowMouseAfterPostClose = false;
    bool                      m_scrollingFollowFocusOverridden = false;
    long                      m_scrollingFollowFocusBackup = 1;
    bool                      m_animationsEnabledOverridden = false;
    long                      m_animationsEnabledBackup = 1;
    SP<CEventLoopTimer>       m_animationsEnabledRestoreTimer;
    bool                      m_deactivatePending = false;
    std::size_t               m_surfaceRenderDataTransformDepth = 0;
    PHLWINDOWREF              m_pendingLiveFocusWorkspaceChangeTarget;
    PHLWINDOWREF              m_postCloseForcedFocus;
    bool                      m_postCloseForcedFocusLatched = false;
    std::size_t               m_ignorePostCloseMouseMoveCount = 0;
    PostCloseDispatcher       m_postCloseDispatcher = PostCloseDispatcher::None;
    std::string               m_postCloseDispatcherArgs;
    std::vector<GestureRegistration> m_registeredGestures;
    std::vector<WorkspaceNameBackup> m_workspaceNameBackups;
    GestureSession            m_gestureSession;
    WorkspaceSwipeGestureContext m_workspaceSwipeGesture;
    WorkspaceTransition      m_workspaceTransition;
    bool                     m_applyingWorkspaceTransitionCommit = false;
    bool                      m_suppressInitialHoverUpdate = false;
    std::size_t               m_postOpenRefreshFrames = 0;

    CHyprSignalListener       m_renderStageListener;
    CHyprSignalListener       m_mouseMoveListener;
    CHyprSignalListener       m_mouseButtonListener;
    CHyprSignalListener       m_keyboardListener;
    CHyprSignalListener       m_windowOpenListener;
    CHyprSignalListener       m_windowDestroyListener;
    CHyprSignalListener       m_windowCloseListener;
    CHyprSignalListener       m_windowMoveWorkspaceListener;
    CHyprSignalListener       m_workspaceActiveListener;
    CHyprSignalListener       m_monitorRemovedListener;
    CHyprSignalListener       m_monitorFocusedListener;
};

} // namespace hymission
