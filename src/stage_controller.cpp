#include "hook_broker.hpp"
#include "stage_controller.hpp"
#include "numeric_config.hpp"
#include "stage_logic.hpp"
#include "stage_edge_effect.hpp"
#include "tiles_controller.hpp"
#include "hook_attempt.hpp"
#include "hook_symbols.hpp"
#include "input_guard.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <linux/input-event-codes.h>

// Collect only the passes appended by a window render. Avoid hooking
// CRenderPass::add, which other plugins (including Viewflow) also intercept.
#include <hyprland/src/render/pass/PassElement.hpp>
#define private public
#include <hyprland/src/render/pass/Pass.hpp>
#undef private

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/Fadeout.hpp>
#include <hyprland/src/desktop/state/FadingOutState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

// CSpace has no plugin work-area contributor API. Change only its freshly
// computed boxes after the original function, never monitor/bar reservations.
#define private public
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include "overview_controller.hpp"
#undef private

#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>

#include "state_json.hpp"

#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/workspace/RegularWorkspace.hpp>
#include "workspace_key.hpp"

#include "render_context.hpp"
#include "decoration_gradient.hpp"

namespace hymission {

namespace {
using Render::GL::g_pHyprOpenGL;
using Clock = std::chrono::steady_clock;

stage::PreviewLayer previewLayer(const PHLWINDOW& window) {
    return stage::previewLayer(window->m_workspace && Fullscreen::controller()->hasFullscreen(window->m_workspace),
        Fullscreen::controller()->isFullscreen(window), window->isFloating(), window->isAllowedOverFullscreen());
}

bool scrollingWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || (workspace->type() == Workspace::eWorkspaceType::SPECIAL) || Fullscreen::controller()->hasFullscreen(workspace) || !workspace->space())
        return false;
    const auto algorithm = workspace->space()->algorithm();
    return algorithm && algorithm->tiledAlgo() &&
        Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(&typeid(*algorithm->tiledAlgo())) == "scrolling";
}

bool preservesScrollingWidth(const PHLWORKSPACE& workspace) {
    if (!scrollingWorkspace(workspace))
        return false;
    auto* scrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(workspace->space()->algorithm()->tiledAlgo().get());
    const std::size_t columns = scrolling && scrolling->m_scrollingData ? scrolling->m_scrollingData->columns.size() : 0;
    return stage::preserveScrollingWidth(true, columns);
}

bool scrollingTiled(const PHLWINDOW& window) {
    return window && !window->isFloating() && !static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) && scrollingWorkspace(window->m_workspace);
}

Rect rect(const CBox& box) { return {box.x, box.y, box.w, box.h}; }
CBox box(const Rect& rect) { return {rect.x, rect.y, rect.width, rect.height}; }

Rect flightBounds(const PHLMONITOR& monitor) {
    static auto gapsOut = CConfigValue<Config::IComplexConfigValue>("general:gaps_out");
    const auto& gap = *static_cast<Config::CCssGapData*>(gapsOut.ptr());
    const double left = std::clamp(static_cast<double>(gap.m_left), 0.0, monitor->m_size.x);
    const double top = std::clamp(static_cast<double>(gap.m_top), 0.0, monitor->m_size.y);
    const double right = std::clamp(static_cast<double>(gap.m_right), 0.0, monitor->m_size.x - left);
    const double bottom = std::clamp(static_cast<double>(gap.m_bottom), 0.0, monitor->m_size.y - top);
    return {monitor->m_position.x + left, monitor->m_position.y + top,
            monitor->m_size.x - left - right, monitor->m_size.y - top - bottom};
}

long setting(const char* suffix, long fallback) {
    const auto value = Config::mgr()->getConfigValue(std::string("plugin:hymission:") + suffix);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(bool))
        return **reinterpret_cast<bool* const*>(value.dataptr);
    if (*value.type == typeid(Config::INTEGER))
        return **reinterpret_cast<Config::INTEGER* const*>(value.dataptr);
    return fallback;
}

double numberSetting(const char* key, double fallback, double minimum = 0.0, double maximum = 1000000.0) {
    const auto value = Config::mgr()->getConfigValue(key);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(Config::FLOAT))
        return finiteConfig(**reinterpret_cast<Config::FLOAT* const*>(value.dataptr), fallback, minimum, maximum);
    if (*value.type == typeid(bool))
        return **reinterpret_cast<bool* const*>(value.dataptr) ? 1 : 0;
    if (*value.type == typeid(Config::INTEGER))
        return finiteConfig(**reinterpret_cast<Config::INTEGER* const*>(value.dataptr), fallback, minimum, maximum);
    return fallback;
}

CBox baseArea(const PHLMONITOR& monitor) {
    return monitor->m_reservedArea.apply(CBox{monitor->m_position, monitor->m_size});
}

bool sameBox(const CBox& a, const CBox& b) {
    return a.pos() == b.pos() && a.size() == b.size();
}

void stopTimer(SP<CEventLoopTimer>& timer) {
    if (!timer)
        return;
    timer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(timer);
    timer.reset();
}

// Draw through the render pass so later passes do not overwrite immediate GL
// output. The callback never starts an offscreen render inside the main pass.
class StagePassElement final : public IPassElement {
  public:
    StagePassElement(std::function<void()> draw, const CBox& bounds) : m_draw(std::move(draw)), m_bounds(bounds) {}
    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override {
        RenderContextScope contextScope{ctx};
        m_draw();
        return {};
    }
    bool needsLiveBlur(Render::CRenderContext& ctx) override { return true; }
    bool needsPrecomputeBlur(Render::CRenderContext& ctx) override { return false; }
    bool undiscardable(Render::CRenderContext& ctx) override { return true; }
    bool disableSimplification(Render::CRenderContext& ctx) override { return false; }
    const char* passName() override { return "HymissionStage"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override { return m_bounds; }
    CRegion opaqueRegion(Render::CRenderContext& ctx) override { return {}; }
  private:
    std::function<void()> m_draw;
    CBox m_bounds;
};

// Preserve native window passes (decorations, subsurfaces and popups included),
// but make their opaque region conservative: the viewport edge can be transparent.
class StageWindowPassElement final : public IPassElement {
  public:
    using Elements = std::vector<UP<IPassElement>>;
    StageWindowPassElement(Elements elements, std::function<void(Elements&)> draw, const CBox& bounds) :
        m_elements(std::move(elements)), m_draw(std::move(draw)), m_bounds(bounds) {}
    std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override {
        RenderContextScope contextScope{ctx};
        m_draw(m_elements);
        return {};
    }
    void discard(Render::CRenderContext& ctx) override { for (auto& element : m_elements) element->discard(ctx); }
    bool needsLiveBlur(Render::CRenderContext& ctx) override { return true; }
    bool needsPrecomputeBlur(Render::CRenderContext& ctx) override {
        return std::ranges::any_of(m_elements, [&](const auto& element) { return element->needsPrecomputeBlur(ctx); });
    }
    bool undiscardable(Render::CRenderContext& ctx) override { return true; }
    const char* passName() override { return "HymissionScrollingWindow"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override { return m_bounds; }
    CRegion opaqueRegion(Render::CRenderContext& ctx) override { return {}; }
  private:
    Elements m_elements;
    std::function<void(Elements&)> m_draw;
    CBox m_bounds;
};
} // namespace

struct StageController::Impl {
    struct Preview {
        PHLWINDOWREF window;
        CBox target;
        CBox natural;
    };
    struct CapturePreview {
        PHLWINDOWREF window;
        CBox selection;
        CBox clip;
        double rounding = 0;
    };
    struct Flight {
        Preview preview;
        CBox from;
        CBox to;
        float fromRadius = 0;
        float toRadius = 0;
        bool dropToCard = false;
        bool offscreen = false;
        std::optional<stage::EdgeViewport> fromViewport;
        std::optional<stage::EdgeViewport> toViewport;
        bool edgeTransfer = false;
        bool desktopDecorations = false;
        bool nativeDestination = false;
    };
    struct DragHover {
        PHLWINDOWREF window;
        PHLMONITORREF monitor;
        PHLWORKSPACEREF workspace;
        WorkspaceKey syntheticId = 0;
        CBox from;
        CBox to;
        Clock::time_point start;
        bool returningToNative = false;
        stage::EdgeViewport fromViewport;
        stage::EdgeViewport toViewport;
        bool onDesktop = false;
        bool edgeTransfer = false;
        bool tiled = false;
    };
    struct Card {
        PHLWORKSPACEREF workspace;
        std::vector<Preview> previews;
        bool dirty = true;
        bool previewsReady = false;
        double shift = 0;
        Clock::time_point shiftStart;
        WorkspaceKey syntheticId = 0; // > 0: transparent empty slot for a workspace that does not exist yet
    };
    struct Screen {
        PHLMONITORREF monitor;
        PHLWORKSPACEREF workspace;
        tiles::Id tile = 0;
        CBox base;
        stage::Geometry geometry;
        std::vector<Card> cards;
        std::vector<CapturePreview> capturePreviews;
        double scroll = 0;
        float scale = 1;
        int transform = 0;
        WorkspaceKey active = WORKSPACE_INVALID;
        std::optional<double> frozenWidth;
        SP<Render::IFramebuffer> background;
        std::string backgroundError;
        Clock::time_point backgroundRendered{};
        std::optional<std::size_t> reorderHover;
        bool covered = false;
        bool preserveScrollingWidth = false;
        bool suspended = false;
        bool revealFromBottom = false;
        double shown = 1;
        double slideFrom = 1;
        Clock::time_point slideStart = Clock::now();
        std::optional<std::size_t> hovered;
        std::vector<Flight> flights;
        Clock::time_point flightStart;
        double flightDuration = 300;
        bool right = false;
        bool paneTransition = false;
        std::vector<Card> departingCards;
        stage::Geometry departingGeometry;
        double departingScroll = 0;
        bool departingRight = false;
        Clock::time_point paneStart;
        double paneDuration = 300;
        Clock::time_point frameSampleStart = Clock::now();
        Clock::time_point lastFrame;
        unsigned frameCount = 0;
        double previewRenderFPS = 0;
    };
    struct Swipe {
        CUnifiedWorkspaceSwipeGesture* native = nullptr;
        PHLMONITORREF monitor;
        PHLWORKSPACEREF origin;
        PHLWORKSPACEREF target;
        WorkspaceKey requestedTarget = WORKSPACE_INVALID;
        Screen visual;
        bool prepared = false;
        bool released = false;
        bool committed = false;
        bool targetCovered = false;
        bool sourceCovered = false;
        bool cancelled = false;
        double progress = 0;
        double releaseFrom = 0;
        double releaseTo = 0;
        double releaseDuration = 0;
        double settleProgress = 0;
        Clock::time_point releaseStart;
    };

    using RecheckFn = void (*)(Layout::CSpace*);
    using DragEndFn = bool (*)(Layout::Supplementary::CDragStateController*);
    using WindowFocusFn = void (*)(Desktop::CFocusState*, PHLWINDOW, Desktop::eFocusReason, SP<CWLSurfaceResource>);
    using ScrollFocusFn = void (*)(Layout::Tiled::CScrollingAlgorithm*, SP<Layout::ITarget>, Layout::Tiled::CScrollingAlgorithm::eInputMode);
    using WindowAtFn = PHLWINDOW (*)(const Desktop::CViewHitTester*, const Vector2D&, uint16_t, PHLWINDOW);
    using RoundingFn = float (*)(Desktop::View::CWindowPresentation*);
    using RenderWindowFn = void (*)(Render::IHyprRenderer*, Render::CRenderContext&, PHLWINDOW, PHLMONITOR, const Render::SWindowRenderPresentation&, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
    using ShouldBlurFn = bool (*)(const Desktop::View::CWindow*, const Render::SWindowRenderPresentation&);
    using SurfaceBoxFn = CBox (*)(CSurfacePassElement*);
    using SurfaceVisibleFn = CRegion (*)(CSurfacePassElement*, Render::CRenderContext&, bool&);
    using SurfaceUVFn = void (*)(void*, Render::CRenderContext&, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool, const Vector2D&, const Vector2D&, bool);
    using AddPassFn = void (*)(Render::CRenderContext&, UP<IPassElement>&&);
    using WorkspaceAnimationFn = void (*)(PHLWORKSPACE, Animation::Workspace::eAnimationType, bool, bool, std::optional<std::string>);
    using ChangeWorkspaceFn = void (*)(Monitor::CMonitor*, const PHLWORKSPACE&, bool, bool, bool);
    using SwipeBeginFn = bool (*)(CUnifiedWorkspaceSwipeGesture*, PHLMONITOR);
    using SwipeUpdateFn = void (*)(CUnifiedWorkspaceSwipeGesture*, double);
    using SwipeEndFn = void (*)(CUnifiedWorkspaceSwipeGesture*);
    inline static Impl* instance = nullptr;
    inline static bool overviewRendering = false;

    HANDLE handle;
    std::function<bool()> overviewSuspended;
    std::function<bool()> inputSuppressed;
    std::function<stage::OverviewPhase(const PHLMONITOR&)> overviewPhase;
    std::function<std::optional<stage::EdgeFrame>(const PHLWINDOW&, const PHLMONITOR&)> overviewFrame;
    std::function<std::optional<double>(const PHLMONITOR&)> sidebarProgress;
    std::vector<Screen> overviewExitScreens;
    bool finishingOverview = false;
    bool stagePassQueued = false;
    Screen* drawingScreen = nullptr;
    bool tileDragPending = false;
    PHLWORKSPACEREF tileDragWorkspace;
    PHLWINDOWREF tileDragClicked;
    PHLMONITORREF tileDragMonitor;
    tiles::Id tileDragSource = 0;
    std::vector<Screen> screens;
    std::vector<PHLWINDOWREF> livePreviewWindows;
    std::optional<Swipe> swipe;
    std::optional<DragHover> dragHover;
    PHLWINDOWREF thumbnailDrag;
    nlohmann::json dropTrace = nlohmann::json::array();
    Clock::time_point dropTraceStart;
    std::string dropPhase;
    PHLWINDOWREF silentDropWindow;
    PHLWORKSPACEREF silentDropSource;
    Clock::time_point silentDropStart;
    uint64_t swipeBegins = 0;
    uint64_t swipeUpdates = 0;
    uint64_t swipeEnds = 0;
    uint64_t rawSwipeUpdates = 0;
    std::vector<CHyprSignalListener> listeners;
    hooks::Lease* areaHook = nullptr;
    hooks::Lease* dragHook = nullptr;
    hooks::Lease* windowFocusHook = nullptr;
    hooks::Lease* scrollFocusHook = nullptr;
    hooks::Lease* hitHook = nullptr;
    hooks::Lease* roundingHook = nullptr;
    hooks::Lease* renderWindowHook = nullptr;
    hooks::Lease* surfaceBoxHook = nullptr;
    hooks::Lease* surfaceVisibleHook = nullptr;
    hooks::Lease* surfaceUVHook = nullptr;
    hooks::Lease* addPassHook = nullptr;
    hooks::Lease* workspaceAnimationHook = nullptr;
    hooks::Lease* changeWorkspaceHook = nullptr;
    hooks::Lease* blurFramebufferHook = nullptr;
    hooks::AttemptCache edgeHookAttempt;
    hooks::Lease* fadeoutsHook = nullptr;
    using BlurFramebufferFn = SP<Render::IFramebuffer> (*)(Render::IHyprRenderer*, Render::CRenderContext&, float, const CRegion&, const Render::SBlurContext&);
    using FadeoutsFn = void (*)(Render::IHyprRenderer*, Render::CRenderContext&, PHLMONITOR, Desktop::eFadeoutPlane, PHLWORKSPACE);
    StageEdgeEffect edgeEffect;
    bool capturingWindowPasses = false;
    SwipeBeginFn swipeBeginOriginal = nullptr;
    bool drawingDecoration = false;
    float decorationRadius = 0;
    PHLWINDOWREF roundingWindow;
    float roundingOverride = 0;
    CBox decorationClip;
    struct SurfaceTransform {
        CBox source;
        CBox target;
        double scale = 1;
    };
    std::optional<SurfaceTransform> surfaceTransform;
    RenderWindowFn renderWindow = nullptr;
    ShouldBlurFn shouldBlur = nullptr;
    struct CanvasScroll {
        PHLWORKSPACEREF workspace;
        double offset = 0;
        bool activating = false;
    };
    std::vector<CanvasScroll> canvasScrolls;
    UP<SEventLoopDoLaterLock> canvasRelease;
    void restoreCanvasScroll(const PHLWORKSPACE& workspace);
    void beginCanvasActivation(const PHLWORKSPACE& workspace);
    UP<SEventLoopDoLaterLock> deferred;
    SP<CEventLoopTimer> refreshTimer;
    SP<CEventLoopTimer> motionTimer;
    std::optional<bool> runtimeVisible;
    double visibility = 1;
    double visibilityFrom = 1;
    double visibilityTarget = 1;
    double visibilityDelta = 0;
    bool visibilityGesture = false;
    Clock::time_point visibilityStart;
    void cancelInput();
    void applyVisibility(double progress);
    void settleVisibility(bool visible);
    bool enabled = false;
    bool hooksReady = false;
    bool hookFailure = false;
    bool rendering = false;
    bool syncing = false;
    bool shuttingDown = false;
    bool cancelDrop = false;
    bool forceRefresh = false;
    bool reconfigure = false;
    bool maximizeCover = false;
    bool smartisan = false;
    stage::Settings settings;
    PHLWORKSPACEREF hoverCenterGuardWorkspace;
    double hoverCenterGuardX = 0;
    bool hoverFocus = false;
    PHLWORKSPACEREF pressed;
    WorkspaceKey pressedSyntheticId = 0;
    Vector2D pressPoint;
    PHLMONITORREF pressMonitor;
    std::size_t pressCard = 0;
    bool reorderArmed = false;
    std::unordered_set<uint32_t> swallowedButtons;
    PHLWINDOWREF lastDragged;
    std::string error;

    static WorkspaceKey cardID(const Card& card) {
        if (const auto workspace = card.workspace.lock())
            return workspaceKey(workspace);
        return card.syntheticId;
    }

    static PHLWORKSPACE ensureWorkspace(const PHLMONITOR& monitor, WorkspaceKey id) {
        if (!monitor || id <= 0)
            return nullptr;
        if (const auto existing = workspaceForKey(id))
            return existing;
        return createWorkspaceForKey(id, monitor->m_id, "", true);
    }

    // Nearest card to a sidebar point, gaps included: stretched spacing would
    // otherwise leave dead zones where a reorder drop is ignored.
    std::optional<std::size_t> reorderTargetIndex(const Screen& screen, const Vector2D& point) const {
        const auto& g = screen.geometry;
        const auto local = point - sidebar(screen).pos();
        if (local.x < g.padding || local.x >= g.padding + g.cardWidth || local.y < g.paddingTop || local.y >= screen.base.h - g.paddingBottom)
            return std::nullopt;
        const double step = g.cardHeight + g.cardGap;
        const double t = (local.y - g.paddingTop - g.cardHeight / 2) / (step > 0 ? step : 1);
        const std::size_t index = static_cast<std::size_t>(std::clamp(std::round(t), 0.0, static_cast<double>(screen.cards.size() - 1)));
        return screen.cards.empty() ? std::nullopt : std::optional<std::size_t>(index);
    }

    Impl(HANDLE h, std::function<bool()> suspended, std::function<stage::OverviewPhase(const PHLMONITOR&)> phase,
         std::function<std::optional<stage::EdgeFrame>(const PHLWINDOW&, const PHLMONITOR&)> frame, std::function<std::optional<double>(const PHLMONITOR&)> slide,
         std::function<bool()> suppressInput) :
        handle(h), overviewSuspended(std::move(suspended)), inputSuppressed(std::move(suppressInput)), overviewPhase(std::move(phase)), overviewFrame(std::move(frame)), sidebarProgress(std::move(slide)) { instance = this; }
    ~Impl();
    void initialize();
    void request(bool dirty = true);
    void sync();
    void armRefresh();
    void armMotion();
    void motion();
    bool installHooks();
    bool ensureEdgeHook();
    void releaseHooks();
    void afterRecheck(Layout::CSpace* space);
    bool endDrag(Layout::Supplementary::CDragStateController* drag);
    void renderStage(eRenderStage stage);
    void draw(const PHLMONITOR& monitor);
    void drawOne(const PHLMONITOR& monitor);
    void drawFlights(Screen& screen, const PHLMONITOR& monitor);
    void drawPreview(const PHLWINDOW& window, const PHLMONITOR& monitor, const CBox& target, const CBox& clip, double radius,
                     std::optional<stage::EdgeViewport> viewport = std::nullopt, double* captureRounding = nullptr, bool forceDecorations = false);
    void drawWindowPasses(StageWindowPassElement::Elements& elements, const PHLMONITOR& monitor, const stage::EdgeViewport& viewport,
                          const Rect& clip);
    void captureWindowPasses(Render::IHyprRenderer* renderer, const PHLMONITOR& monitor, const stage::EdgeViewport& viewport, const std::function<void()>& render,
                             std::optional<Rect> overviewClip = std::nullopt, bool separateFadeouts = false);
    stage::EdgeViewport desktopViewport(const Screen& screen) const;
    stage::EdgeViewport cardViewport(const CBox& card) const;
    std::pair<Rect, std::optional<stage::EdgeViewport>> flightFrame(const Flight& flight, double progress, const PHLMONITOR& monitor) const;
    void retargetFlight(Flight& flight, double progress, const PHLMONITOR& monitor);
    void startFlights(Screen& screen, WorkspaceKey previous, const std::vector<Card>& oldCards, const stage::Geometry& oldGeometry, double oldScroll, bool oldRight,
                      const std::vector<std::pair<PHLWINDOWREF, CBox>>& origins, PHLWORKSPACE visualActive = nullptr, bool stopNative = true);
    bool flying(const Screen& screen, const PHLWINDOW& window) const;
    double flightProgress(const Screen& screen) const;
    void refreshPreviews();
    void ensureBackground(Screen& screen, const PHLMONITOR& monitor);
    void updatePreviewLiveness();
    bool updatePreviews(Screen& screen, Card& card);
    bool updatePreviews(Screen& screen, Card& card, const Vector2D& tiledOffset);
    void prepareSwipe(const PHLWORKSPACE& target);
    void updateSwipe(double delta);
    void endSwipe();
    void clearSwipe();
    Screen* visualScreenFor(const PHLMONITOR& monitor);
    Screen* preparedOverviewScreen(const PHLMONITOR& monitor, tiles::Id tile=UINT64_MAX);
    stage::OverviewPhase overviewPhaseFor(const PHLMONITOR& monitor) const;
    std::optional<stage::OverviewEndpoint> cardEndpoint(const Screen& screen, const PHLWINDOW& window, bool settled) const;
    stage::Settings settingsForSide(bool right) const;
    bool blocked() const;
    bool renderBlocked(const PHLMONITOR& monitor) const;
    bool interactive(const Screen& screen) const;
    bool ownsTransition(const PHLWORKSPACE& workspace, bool allowCovered = false) const;
    Screen* screenFor(const PHLMONITOR& monitor, tiles::Id tile = UINT64_MAX);
    Screen* screenForWorkspace(const PHLWORKSPACE& workspace);
    PHLWORKSPACE screenWorkspace(const Screen& screen) const {
        if (!screen.tile) return screen.monitor?screen.monitor->m_activeWorkspace:nullptr;
        return screen.workspace.lock()?screen.workspace.lock():screen.monitor?screen.monitor->m_activeWorkspace:nullptr;
    }
    CBox screenBase(const PHLMONITOR& monitor, tiles::Id tile) const {
        auto base=baseArea(monitor);
        if (auto* tiles=TilesController::get()) for (const auto& view:tiles->views(monitor)) if (view.id==tile) return base.intersection(view.allocation);
        return base;
    }
    std::vector<Card> cardsForWorkspace(const PHLMONITOR& monitor, const PHLWORKSPACE& activeWorkspace) const;
    std::pair<Screen*, std::optional<std::size_t>> hit(const Vector2D& point, bool activation = false);
    void pointer();
    void updateDragHover();
    stage::ScrollingFlightFrame dragHoverFrame() const;
    void clearDragHover();
    void traceDrop(const char* phase, const PHLWINDOW& window, const PHLWORKSPACE& destination, bool begin = false);
    void finishDrop(const PHLWORKSPACE& destination, const PHLWINDOW& window, const CBox& dropOrigin,
                    float dropRadius, const Vector2D& dropPoint, const Vector2D& grabOffset,
                    std::optional<Vector2D> floatingCenter, std::optional<stage::EdgeViewport> originViewport, bool edgeTransfer);
    void finishThumbnailDrag();
    void button(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info);
    void axis(const IPointer::SAxisEvent& event, Event::SCallbackInfo& info);
    void activate(const PHLWORKSPACE& workspace, bool cardClick = false);
    void swapWorkspaces(const PHLWORKSPACE& a, const PHLWORKSPACE& b);
    void correctFloating(const PHLMONITOR& monitor, const CBox& desktop, const PHLWORKSPACE& workspace = nullptr);
    void damage(const Screen& screen);
    double cardTop(const Screen& screen, std::size_t index) const;
    std::optional<std::size_t> cardHit(const Screen& screen, const Vector2D& point, bool activation = false) const;
    CBox sidebar(const Screen& screen) const;
    CBox desktop(const Screen& screen) const;
    std::string stateJson() const;

    static void recheckThunk(Layout::CSpace* space) {
        auto* self = instance;
        reinterpret_cast<RecheckFn>(self->areaHook->m_original)(space);
        self->afterRecheck(space);
    }
    static bool dragThunk(Layout::Supplementary::CDragStateController* drag) { return instance->endDrag(drag); }
    static void windowFocusThunk(Desktop::CFocusState* focus, PHLWINDOW window, Desktop::eFocusReason reason, SP<CWLSurfaceResource> surface) {
        auto* self = instance;
        if (sessionLocked()) {
            reinterpret_cast<WindowFocusFn>(self->windowFocusHook->m_original)(focus, std::move(window), reason, std::move(surface));
            return;
        }
        const bool explicitFocus = reason == Desktop::FOCUS_REASON_KEYBIND || reason == Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW ||
            reason == Desktop::FOCUS_REASON_DISPATCH_MOVEWINDOWINTOGROUP ||
            reason == Desktop::FOCUS_REASON_CLICK || reason == Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD ||
            reason == Desktop::FOCUS_REASON_WORKSPACE_CHANGE || reason == Desktop::FOCUS_REASON_TOGGLE_SPECIAL_WORKSPACE;
        const auto source = self->silentDropSource.lock();
        if (!explicitFocus && self->enabled && !self->blocked() && window && self->silentDropWindow == window && source && source->visible() &&
            window->m_workspace && !window->m_workspace->visible() && !setting("stage_drop_follow", 0) &&
            Clock::now() - self->silentDropStart < std::chrono::seconds(2)) {
            if (self->dropTrace.size() < 24)
                self->dropTrace.push_back({{"event", "suppressed_drop_focus"}, {"reason", static_cast<int>(reason)},
                    {"destination", workspaceKey(window->m_workspace)}});
            return;
        }
        const auto workspace = window ? window->m_workspace : PHLWORKSPACE{};
        if (explicitFocus && reason != Desktop::FOCUS_REASON_WORKSPACE_CHANGE)
            std::erase_if(self->canvasScrolls, [&](const auto& saved) { return saved.workspace == workspace; });
        const bool previousHoverFocus = std::exchange(self->hoverFocus, reason == Desktop::FOCUS_REASON_FFM);
        reinterpret_cast<WindowFocusFn>(self->windowFocusHook->m_original)(focus, std::move(window), reason, std::move(surface));
        self->hoverFocus = previousHoverFocus;
        if (reason == Desktop::FOCUS_REASON_WORKSPACE_CHANGE)
            self->restoreCanvasScroll(workspace);
    }
    static void scrollFocusThunk(Layout::Tiled::CScrollingAlgorithm* algorithm, SP<Layout::ITarget> target,
                                 Layout::Tiled::CScrollingAlgorithm::eInputMode input) {
        auto* self = instance;
        if (sessionLocked()) {
            reinterpret_cast<ScrollFocusFn>(self->scrollFocusHook->m_original)(algorithm, std::move(target), input);
            return;
        }
        const auto original = reinterpret_cast<ScrollFocusFn>(self->scrollFocusHook->m_original);
        const auto workspace = target ? target->workspace() : PHLWORKSPACE{};
        const auto screen = self->screenForWorkspace(workspace);
        // Keep FFM focus, but leave the scrolling camera in place while the
        // pointer travels right from a clicked Stage card into the desktop.
        if (self->hoverFocus && input == Layout::Tiled::CScrollingAlgorithm::INPUT_MODE_SOFT &&
            self->enabled && !self->blocked() && setting("stage_click_hover_recenter_guard", 1) &&
            workspace && self->hoverCenterGuardWorkspace == workspace && workspace->visible())
            return;
        const double minimum = numberSetting("scrolling:follow_min_visible", 0);
        if (input != Layout::Tiled::CScrollingAlgorithm::INPUT_MODE_SOFT || minimum <= 0 ||
            !screen || !self->interactive(*screen) || !scrollingWorkspace(workspace) ||
            target->space() != workspace->space() || workspace->space()->algorithm()->tiledAlgo().get() != algorithm) {
            original(algorithm, std::move(target), input);
            return;
        }
        const auto data = algorithm->m_scrollingData;
        const auto targetData = algorithm->dataFor(target);
        if (!data || !data->controller || !targetData)
            return;
        // Native soft focus measures against monitor.logicalBox(), counting
        // content hidden under Stage. Both intersection and denominator must
        // instead use the visible desktop. The scrolling sizing work area can
        // intentionally retain full width, so intersect it with Stage's clip.
        const auto desktop = workspace->space()->workArea().intersection(self->desktop(*screen));
        if (stage::visibleAxisFraction(rect(target->position()), rect(desktop), data->controller->isPrimaryHorizontal()) <
            std::clamp(minimum, 0.0, 1.0))
            return;
        const auto column = targetData->column.lock();
        if (!column || data->visible(column, true))
            return;
        // Retain the native soft-focus tail, without rerunning its full-output
        // percentage gate or promoting this input to forced/hard focus.
        data->centerOrFitCol(column);
        data->recalculate();
    }
    static bool swipeBeginThunk(CUnifiedWorkspaceSwipeGesture* gesture, PHLMONITOR monitor) {
        auto* self = instance;
        if (sessionLocked())
            return self->swipeBeginOriginal(gesture, monitor);
        if (self->swipe)
            self->clearSwipe();
        auto* tiles=TilesController::get();
        const bool owns=self->ownsTransition(monitor?monitor->m_activeWorkspace:nullptr,true);
        if (owns && tiles && !tiles->acquire(TilesController::Owner::Swipe)) return false;
        const bool began = self->swipeBeginOriginal(gesture, monitor);
        if ((!began || !gesture->isGestureInProgress()) && owns && tiles) tiles->release(TilesController::Owner::Swipe);
        if (began && monitor && self->ownsTransition(monitor->m_activeWorkspace, true) && gesture->isGestureInProgress()) {
            ++self->swipeBegins;
            self->swipe.emplace();
            self->swipe->native = gesture;
            self->swipe->monitor = monitor;
            self->swipe->origin = monitor->m_activeWorkspace;
        }
        return began;
    }
    static void workspaceAnimationThunk(PHLWORKSPACE workspace, Animation::Workspace::eAnimationType type, bool left, bool instant,
                                        std::optional<std::string> style) {
        auto* self = instance;
        if (sessionLocked()) {
            reinterpret_cast<WorkspaceAnimationFn>(self->workspaceAnimationHook->m_original)(workspace, type, left, instant, std::move(style));
            return;
        }
        // A prepared Stage swipe retains animation ownership through commit,
        // even when its destination is fullscreen and hides the resting strip.
        const bool settlingSwipe = self->swipe && self->swipe->prepared && self->swipe->committed && workspace &&
            workspace->m_monitor == self->swipe->monitor &&
            (workspace == self->swipe->origin || workspace == self->swipe->target);
        if (workspace && type == Animation::Workspace::ANIMATION_TYPE_IN) {
            if (auto* screen = self->screenFor(workspace->m_monitor.lock()))
                screen->revealFromBottom = left;
        }
        const auto monitor = workspace ? workspace->m_monitor.lock() : PHLMONITOR{};
        const auto* screen = self->screenFor(monitor);
        // Overview owns window motion while Stage is suspended. Its exit now
        // lands directly in Stage cards; a native slide must not run underneath.
        const bool overviewOwnsMotion = self->enabled && self->visibility > 0 && workspace && !(workspace->type() == Workspace::eWorkspaceType::SPECIAL) && screen && screen->geometry.enabled() &&
            self->overviewPhaseFor(monitor) != stage::OverviewPhase::Inactive;
        const bool owned = settlingSwipe || overviewOwnsMotion || self->ownsTransition(workspace);
        reinterpret_cast<WorkspaceAnimationFn>(self->workspaceAnimationHook->m_original)(workspace, type, left, instant || owned, std::move(style));
        if (owned)
            workspace->m_renderOffset->setValueAndWarp(Vector2D{});
    }
    static void changeWorkspaceThunk(Monitor::CMonitor* monitor, const PHLWORKSPACE& workspace, bool internal, bool noMouseMove, bool noFocus) {
        auto* self = instance;
        if (sessionLocked()) {
            reinterpret_cast<ChangeWorkspaceFn>(self->changeWorkspaceHook->m_original)(monitor, workspace, internal, noMouseMove, noFocus);
            return;
        }
        if (!self->dropTrace.empty() && self->dropTrace.size() < 24 &&
            Clock::now() - self->dropTraceStart < std::chrono::seconds(2)) {
            self->dropTrace.push_back({{"event", "workspace_request"}, {"phase", self->dropPhase},
                {"from", monitor->m_activeWorkspace ? workspaceKey(monitor->m_activeWorkspace) : -1}, {"to", workspace ? workspaceKey(workspace) : -1}});
        }
        if (self->swipe && self->swipe->monitor == monitor->m_self && (!self->swipe->released || self->swipe->target != workspace))
            self->clearSwipe();
        if (const auto guarded = self->hoverCenterGuardWorkspace.lock(); guarded &&
            guarded->m_monitor.lock().get() == monitor && guarded != workspace)
            self->hoverCenterGuardWorkspace.reset();
        if (monitor->m_activeWorkspace != workspace)
            self->beginCanvasActivation(workspace);
        reinterpret_cast<ChangeWorkspaceFn>(self->changeWorkspaceHook->m_original)(monitor, workspace, internal, noMouseMove, noFocus);
        self->restoreCanvasScroll(workspace);
        if (self->enabled && !self->blocked() && !self->syncing) {
            // Finish preparing our transition in the dispatch itself, before a
            // frame can expose the new workspace or a native workspace slide.
            self->forceRefresh = true;
            self->sync();
            self->restoreCanvasScroll(workspace);
        }
    }
    static void addPassThunk(Render::CRenderContext& ctx, UP<IPassElement>&& element) {
        RenderContextScope contextScope{ctx};
        auto* renderer = g_pHyprRenderer.get();
        auto* self = instance;
        if (sampledDecorationGradient)
            return;
        if (!self->drawingDecoration) {
            reinterpret_cast<AddPassFn>(self->addPassHook->m_original)(ctx, std::move(element));
            return;
        }
        // Native decorations normally enqueue passes. While composing a stage
        // preview, draw them here instead of modifying the pass being iterated.
        renderContext().m_data.clipBox = self->decorationClip;
        renderer->draw(renderContext(), element, renderContext().m_data.damage);
    }
    static CBox surfaceBoxThunk(CSurfacePassElement* element) {
        auto* self = instance;
        auto box = reinterpret_cast<SurfaceBoxFn>(self->surfaceBoxHook->m_original)(element);
        if (self->surfaceTransform) {
            const auto& transform = *self->surfaceTransform;
            box.translate(-transform.source.pos()).scale(transform.scale).translate(transform.target.pos());
        }
        return box;
    }
    static CRegion surfaceVisibleThunk(CSurfacePassElement* element, Render::CRenderContext& ctx, bool& cancel) {
        RenderContextScope contextScope{ctx};
        auto* self = instance;
        if (!self->surfaceTransform)
            return reinterpret_cast<SurfaceVisibleFn>(self->surfaceVisibleHook->m_original)(element, ctx, cancel);
        // Native visible regions describe the full-size desktop surface. Use
        // the transformed surface bounds, as overview does, before clipping.
        cancel = false;
        return CRegion{surfaceBoxThunk(element).scale(element->m_data.pMonitor->m_scale).round()};
    }
    static void surfaceUVThunk(void* renderer, Render::CRenderContext& ctx, PHLWINDOW window, SP<CWLSurfaceResource> surface, PHLMONITOR monitor, bool main,
                               const Vector2D& projected, const Vector2D& unscaled, bool misaligned) {
        RenderContextScope contextScope{ctx};
        auto* self = instance;
        auto size = projected;
        auto logical = unscaled;
        if (self->surfaceTransform && surface && monitor) {
            // Scaling a preview is not a client resize: retain the complete
            // committed viewport instead of native resize-time UV cropping.
            logical = surface->m_current.viewport.hasDestination ? surface->m_current.viewport.destination :
                surface->m_current.viewport.hasSource ? surface->m_current.viewport.source.size() : surface->m_current.size;
            size = (logical * monitor->m_scale).round();
            misaligned = false;
        }
        reinterpret_cast<SurfaceUVFn>(self->surfaceUVHook->m_original)(renderer, ctx, window, std::move(surface), monitor, main, size, logical, misaligned);
    }
    static void renderWindowThunk(Render::IHyprRenderer* renderer, Render::CRenderContext& ctx, PHLWINDOW window, PHLMONITOR monitor, const Render::SWindowRenderPresentation& presentation, const Time::steady_tp& time,
                                  bool decorate, Render::eRenderPassMode mode, bool ignorePosition, bool standalone) {
        RenderContextScope contextScope{ctx};
        auto* self = instance;
        const auto* screen = TilesController::get() && TilesController::get()->enabled(monitor) ? self->screenForWorkspace(window ? window->m_workspace : nullptr) : self->visualScreenFor(monitor);
        if (!standalone && !ignorePosition && !self->rendering && !renderContext().m_renderingSnapshot && self->enabled && !self->blocked() && screen && !screen->covered &&
            !monitor->m_activeSpecialWorkspace && (self->flying(*screen, window) ||
                (window && !static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) && previewLayer(window) == stage::PreviewLayer::Hidden &&
                    std::ranges::any_of(screen->flights, [&](const Flight& flight) {
                        const auto owner = flight.preview.window.lock();
                        return owner && owner->m_workspace == window->m_workspace;
                    })) ||
                (self->dragHover && self->dragHover->window == window)))
            return;
        const auto original = reinterpret_cast<RenderWindowFn>(self->renderWindowHook->m_original);
        const auto* drag = g_layoutManager->dragController().get();
        const bool tiledDrag = drag && drag->mode() == MBIND_MOVE && drag->draggingTiled() && drag->target() && drag->target()->window() == window;
        // Pinned floats and temporarily floated tiled drags stay above Stage,
        // including their decorations. Never redraw their native content.
        if (window && ((static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) && window->isFloating()) || tiledDrag) && !standalone && !ignorePosition &&
            !self->rendering && !self->capturingWindowPasses && !renderContext().m_renderingSnapshot)
            self->renderStage(RENDER_POST_WINDOWS);
        if (overviewRendering && !standalone && !self->rendering && !self->capturingWindowPasses && !renderContext().m_renderingSnapshot && self->overviewFrame) {
            if (const auto frame = self->overviewFrame(window, monitor)) {
                self->captureWindowPasses(renderer, monitor, frame->viewport, [&] {
                    original(renderer, ctx, window, monitor, presentation, time, decorate, mode, ignorePosition, standalone);
                }, frame->clip);
                return;
            }
        }
        if (!standalone && !ignorePosition && !self->rendering && !self->capturingWindowPasses && !renderContext().m_renderingSnapshot &&
            self->enabled && !self->blocked() && screen && screen->geometry.enabled() && !screen->covered && !screen->suspended &&
            !monitor->m_activeSpecialWorkspace && window && !tiledDrag && stage::desktopEdgeApplies(static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED), window->isFloating(),
                scrollingWorkspace(window->m_workspace), window->onSpecialWorkspace(), Fullscreen::controller()->isFullscreen(window))) {
            const CBox bounds{window->positionAnimation()->value() + window->m_workspace->m_renderOffset->value() + window->presentation().floatingOffset(), window->sizeAnimation()->value()};
            const auto viewport = stage::edgeViewportForWindow(self->desktopViewport(*screen), rect(bounds));
            self->captureWindowPasses(renderer, monitor, viewport, [&] {
                original(renderer, ctx, window, monitor, presentation, time, decorate, mode, ignorePosition, standalone);
            });
            return;
        }
        original(renderer, ctx, window, monitor, presentation, time, decorate, mode, ignorePosition, standalone);
    }
    static void fadeoutsThunk(Render::IHyprRenderer* renderer, Render::CRenderContext& ctx, PHLMONITOR monitor, Desktop::eFadeoutPlane plane, PHLWORKSPACE workspace) {
        RenderContextScope contextScope{ctx};
        auto* self = instance;
        const auto original = reinterpret_cast<FadeoutsFn>(self->fadeoutsHook->m_original);
        const auto* screen = TilesController::get() && TilesController::get()->enabled(monitor)?self->screenForWorkspace(workspace):self->visualScreenFor(monitor);
        if (plane == Desktop::FADEOUT_PLANE_WINDOW_TILED && scrollingWorkspace(workspace) && !self->rendering && !self->capturingWindowPasses &&
            !renderContext().m_renderingSnapshot && self->enabled && !self->blocked() && screen && screen->geometry.enabled() &&
            !screen->covered && !screen->suspended && !monitor->m_activeSpecialWorkspace) {
            self->captureWindowPasses(renderer, monitor, self->desktopViewport(*screen), [&] { original(renderer, ctx, monitor, plane, workspace); }, std::nullopt, true);
            return;
        }
        original(renderer, ctx, monitor, plane, workspace);
    }
    static SP<Render::IFramebuffer> blurFramebufferThunk(Render::IHyprRenderer* renderer, Render::CRenderContext& ctx, float alpha, const CRegion& damage, const Render::SBlurContext& blurContext) {
        RenderContextScope contextScope{ctx};
        auto* self = instance;
        const auto original = reinterpret_cast<BlurFramebufferFn>(self->blurFramebufferHook->m_original);
        const auto backdrop = self->edgeEffect.backdrop();
        if (!backdrop)
            return original(renderer, ctx, alpha, damage, blurContext);
        const auto destination = renderContext().m_data.currentFB;
        renderer->bindFB(renderContext(), backdrop);
        Hyprutils::Utils::CScopeGuard restore{[&] { renderer->bindFB(renderContext(), destination); }};
        return original(renderer, ctx, alpha, damage, blurContext);
    }
    static float roundingThunk(Desktop::View::CWindowPresentation* presentation) {
        auto* self = instance;
        // Native decorations query a pre-scale radius; surfaces receive their
        // final displayed radius directly without changing the real window.
        if (self->roundingWindow && &self->roundingWindow->presentation() == presentation)
            return self->roundingOverride;
        return self->drawingDecoration ? self->decorationRadius : reinterpret_cast<RoundingFn>(self->roundingHook->m_original)(presentation);
    }
    static PHLWINDOW windowAtThunk(const Desktop::CViewHitTester* tester, const Vector2D& position, uint16_t properties, PHLWINDOW ignore) {
        auto* self = instance;
        if (sessionLocked())
            return reinterpret_cast<WindowAtFn>(self->hitHook->m_original)(tester, position, properties, ignore);
        const auto candidate = reinterpret_cast<WindowAtFn>(self->hitHook->m_original)(tester, position, properties, ignore);
        if (candidate && static_cast<bool>(candidate->m_state & Desktop::View::WINDOW_STATE_PINNED) && candidate->isFloating())
            return candidate;
        if (self->hit(position).first)
            return nullptr;
        return candidate;
    }
};

bool StageController::Impl::blocked() const {
    return sessionLocked() || (overviewSuspended && overviewSuspended());
}

bool StageController::Impl::renderBlocked(const PHLMONITOR& monitor) const {
    if (sessionLocked())
        return true;
    if (sidebarProgress && sidebarProgress(monitor))
        return false;
    const bool prepared = std::ranges::any_of(overviewExitScreens, [&](const auto& screen) { return screen.monitor == monitor; });
    return stage::overviewRenderOwner(overviewPhaseFor(monitor), prepared) == stage::OverviewRenderOwner::Overview;
}

bool StageController::Impl::ownsTransition(const PHLWORKSPACE& workspace, bool allowCovered) const {
    if (!enabled || visibility != 1 || visibilityGesture || !workspace || (workspace->type() == Workspace::eWorkspaceType::SPECIAL) || blocked())
        return false;
    const auto monitor = workspace->m_monitor.lock();
    if (TilesController::get() && TilesController::get()->enabled(monitor)) return false;
    if (!monitor || monitor->m_activeSpecialWorkspace)
        return false;
    const auto screen = std::ranges::find_if(screens, [&](const auto& s) { return s.monitor == monitor; });
    if (screen == screens.end() || !screen->geometry.enabled() || (!allowCovered && screen->covered) || screen->suspended)
        return false;
    const auto targetMode = monitor->m_activeWorkspace ? Fullscreen::controller()->getFullscreenModes(monitor->m_activeWorkspace).internal : Fullscreen::FSMODE_NONE;
    return allowCovered || (targetMode != Fullscreen::FSMODE_FULLSCREEN && !(maximizeCover && targetMode == Fullscreen::FSMODE_MAXIMIZED));
}

bool StageController::Impl::interactive(const Screen& screen) const {
    return visibility == 1 && !visibilityGesture && enabled && !rendering && !blocked() && !(inputSuppressed && inputSuppressed()) && screen.geometry.enabled() && !screen.covered && !screen.suspended;
}

StageController::Impl::Screen* StageController::Impl::screenFor(const PHLMONITOR& monitor, tiles::Id tile) {
    if (tile==UINT64_MAX) {
        tile=0;
        if (auto* tiles=TilesController::get()) for (const auto& view:tiles->views(monitor)) if (view.focused) tile=view.id;
    }
    const auto it = std::ranges::find_if(screens, [&](const auto& s) { return s.monitor == monitor && s.tile==tile; });
    return it == screens.end() ? nullptr : &*it;
}

StageController::Impl::Screen* StageController::Impl::screenForWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace) return nullptr;
    const auto mon=workspace->m_monitor.lock();
    if (auto* tiles=TilesController::get();tiles && tiles->available()) return screenFor(mon,tiles->preferredTile(workspace,mon));
    return screenFor(mon);
}

stage::OverviewPhase StageController::Impl::overviewPhaseFor(const PHLMONITOR& monitor) const {
    return overviewPhase ? overviewPhase(monitor) : stage::OverviewPhase::Inactive;
}

StageController::Impl::Screen* StageController::Impl::preparedOverviewScreen(const PHLMONITOR& monitor, tiles::Id tile) {
    if (tile==UINT64_MAX) { const auto* screen=screenFor(monitor); tile=screen?screen->tile:0; }
    const auto it = std::ranges::find_if(overviewExitScreens, [&](const auto& screen) { return screen.monitor == monitor && screen.tile==tile; });
    return it == overviewExitScreens.end() ? nullptr : &*it;
}

StageController::Impl::Screen* StageController::Impl::visualScreenFor(const PHLMONITOR& monitor) {
    if (drawingScreen && drawingScreen->monitor==monitor) {
        if (overviewPhaseFor(monitor)==stage::OverviewPhase::Releasing || (sidebarProgress && sidebarProgress(monitor)))
            if (auto* prepared=preparedOverviewScreen(monitor,drawingScreen->tile)) return prepared;
        return drawingScreen;
    }
    if (overviewPhaseFor(monitor) == stage::OverviewPhase::Releasing || (sidebarProgress && sidebarProgress(monitor)))
        if (auto* prepared = preparedOverviewScreen(monitor))
            return prepared;
    if (swipe && swipe->prepared && swipe->monitor == monitor)
        return &swipe->visual;
    return screenFor(monitor);
}

stage::Settings StageController::Impl::settingsForSide(bool right) const {
    auto result = settings;
    result.evenSpacing = setting("stage_even_spacing", 0) != 0;
    if (right && setting("stage_padding", -1) < 0) {
        static auto gapsIn = CConfigValue<Config::IComplexConfigValue>("general:gaps_in");
        static auto gapsOut = CConfigValue<Config::IComplexConfigValue>("general:gaps_out");
        const auto& inner = *static_cast<Config::CCssGapData*>(gapsIn.ptr());
        const auto& outer = *static_cast<Config::CCssGapData*>(gapsOut.ptr());
        result.padding = std::max(0.0, static_cast<double>(inner.m_left + inner.m_right - outer.m_right));
        result.paddingRight = outer.m_right;
    }
    return result;
}

void StageController::Impl::clearSwipe() {
    if (!swipe)
        return;
    if (swipe->native)
        swipe->native->cancel();
    const auto monitor = swipe->monitor.lock();
    if (swipe->prepared) {
        if (auto* actual = screenFor(monitor)) {
            actual->frameSampleStart = swipe->visual.frameSampleStart;
            actual->frameCount = swipe->visual.frameCount;
            actual->lastFrame = swipe->visual.lastFrame;
            actual->previewRenderFPS = swipe->visual.previewRenderFPS;
        }
    }
    swipe.reset();
    if (auto* tiles=TilesController::get()) tiles->release(TilesController::Owner::Swipe);
    if (monitor)
        g_pHyprRenderer->damageMonitor(monitor);
}

void StageController::Impl::prepareSwipe(const PHLWORKSPACE& target) {
    if (!swipe || (!target && swipe->requestedTarget == WORKSPACE_INVALID))
        return;
    const auto monitor = swipe->monitor.lock();
    const auto origin = swipe->origin.lock();
    const auto* source = screenFor(monitor);
    if (!monitor || !origin || !source)
        return;
    swipe->prepared = false;
    swipe->target = target;
    const auto targetMode = target ? Fullscreen::controller()->getFullscreenModes(target).internal : Fullscreen::FSMODE_NONE;
    swipe->targetCovered = targetMode == Fullscreen::FSMODE_FULLSCREEN || (maximizeCover && targetMode == Fullscreen::FSMODE_MAXIMIZED);
    if (!numberSetting("animations:enabled", 1) || setting("stage_transition_ms", 300) <= 0)
        return;
    auto& visual = swipe->visual;
    visual = *source;
    swipe->sourceCovered = source->covered;
    visual.covered = false;
    visual.shown = 1;
    visual.revealFromBottom = swipe->native->m_delta > 0;
    // A new workspace has no native object until the gesture commits. Build
    // its empty desktop now so outgoing windows still follow the finger.
    visual.active = target ? workspaceKey(target) : swipe->requestedTarget;
    visual.right = smartisan ? !source->right : source->right;
    visual.cards.clear();
    for (const auto& workspace : State::workspaceState()->workspaces()) {
        if ((workspace->type() == Workspace::eWorkspaceType::SPECIAL) || workspace->m_monitor != monitor ||
            (setting("stage_show_active", 0) == 0 && workspace == target) ||
            (!setting("stage_show_empty", 1) && workspace->getWindowCount(std::nullopt, false) == 0))
            continue;
        const auto old = std::ranges::find_if(source->cards, [&](const auto& card) { return card.workspace == workspace; });
        visual.cards.push_back(old == source->cards.end() ? Card{.workspace = workspace} : *old);
    }
    for (const auto& card : source->cards) {
        if (card.syntheticId == 0)
            continue;
        const auto id = cardID(card);
        if (std::ranges::find_if(visual.cards, [&](const Card& c) { return cardID(c) == id; }) == visual.cards.end())
            visual.cards.push_back(card);
    }
    std::ranges::sort(visual.cards, [](const Card& a, const Card& b) {
        const auto ida = cardID(a), idb = cardID(b);
        if ((ida > 0) != (idb > 0))
            return ida > 0;
        return ida > 0 ? ida < idb :
            (a.workspace.lock() && b.workspace.lock() && a.workspace.lock()->displayName() < b.workspace.lock()->displayName());
    });
    visual.geometry = stage::layout(visual.base.w, visual.base.h, visual.cards.size(), settingsForSide(visual.right), std::nullopt, monitor->m_size.x);
    visual.scroll = visual.geometry.clampScroll(visual.scroll);
    const auto tiledOffset = desktop(visual).pos() - desktop(*source).pos();
    for (std::size_t i = 0; i < visual.cards.size(); ++i) {
        auto& card = visual.cards[i];
        const auto old = std::ranges::find_if(source->cards, [&](const auto& c) { return c.workspace == card.workspace; });
        card.shift = old == source->cards.end() ? 0 : cardTop(*source, std::distance(source->cards.begin(), old)) - visual.geometry.cardTop(i, visual.scroll);
        card.shiftStart = Clock::now();
        updatePreviews(visual, card, tiledOffset);
    }
    visual.departingCards = source->cards;
    visual.departingGeometry = source->geometry;
    visual.departingScroll = source->scroll;
    visual.departingRight = source->right;
    visual.paneTransition = visual.right != source->right;
    std::vector<std::pair<PHLWINDOWREF, CBox>> origins;
    for (const auto& window : Desktop::windowState()->windows())
        if (window->m_monitor == monitor && window->mapped() && !window->isHidden() && !static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED))
            origins.emplace_back(window, setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
                CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()});
    startFlights(visual, workspaceKey(origin), source->cards, source->geometry, source->scroll, source->right, origins, target, false);
    for (auto& flight : visual.flights) {
        const auto window = flight.preview.window.lock();
        if (!window)
            continue;
        if (window->m_workspace == target) {
            flight.to = flight.preview.natural;
            if (!window->isFloating())
                flight.to.translate(tiledOffset);
        } else {
            for (std::size_t i = 0; i < visual.cards.size(); ++i) {
                const auto& card = visual.cards[i];
                if (card.workspace != window->m_workspace)
                    continue;
                const auto preview = std::ranges::find_if(card.previews, [&](const auto& p) { return p.window == window; });
                if (preview != card.previews.end())
                    flight.to = preview->target.copy().translate(Vector2D{sidebar(visual).x + visual.geometry.padding,
                        visual.base.y + visual.geometry.cardTop(i, visual.scroll)});
            }
        }
    }
    // Keep the card-sized endpoints, translating only the hidden sidebar end
    // beyond its own output edge. The other end remains the native desktop.
    const auto hiddenSidebarOffset = [&](bool right, double width) {
        return right ? monitor->m_position.x + monitor->m_size.x - (visual.base.x + visual.base.w) + width :
            -(visual.base.x - monitor->m_position.x + width);
    };
    for (auto& flight : visual.flights) {
        const auto window = flight.preview.window.lock();
        if (!window)
            continue;
        if (window->m_workspace == target && swipe->sourceCovered) {
            const double offset = hiddenSidebarOffset(source->right, source->geometry.bandWidth);
            flight.from.translate(Vector2D{offset, 0.0});
            if (flight.fromViewport) flight.fromViewport->box.x += offset;
            flight.offscreen = true;
        } else if (window->m_workspace != target && swipe->targetCovered) {
            const double offset = hiddenSidebarOffset(visual.right, visual.geometry.bandWidth);
            flight.to.translate(Vector2D{offset, 0.0});
            if (flight.toViewport) flight.toViewport->box.x += offset;
            flight.offscreen = true;
        }
    }
    swipe->prepared = true;
    updatePreviewLiveness();
    armMotion();
}

void StageController::Impl::updateSwipe(double delta) {
    if (sessionLocked())
        return;

    if (!swipe || swipe->released)
        return;
    ++swipeUpdates;
    const auto monitor = swipe->monitor.lock();
    if (!monitor || monitor->m_activeWorkspace != swipe->origin || !ownsTransition(monitor->m_activeWorkspace, true) || Desktop::focusState()->monitor() != monitor) {
        clearSwipe();
        return;
    }
    auto* native = swipe->native;
    const double distance = std::max(1.0, numberSetting("gestures:workspace_swipe_distance", 300));
    native->m_avgSpeed = (native->m_avgSpeed * native->m_speedPoints + std::abs(native->m_delta - delta)) / (native->m_speedPoints + 1);
    ++native->m_speedPoints;
    native->m_delta = std::clamp(delta, -distance, distance);
    if (numberSetting("gestures:workspace_swipe_direction_lock", 1)) {
        const int direction = native->m_delta < 0 ? -1 : 1;
        if (native->m_initialDirection && direction != native->m_initialDirection)
            native->m_delta = 0;
        else if (!native->m_initialDirection && std::abs(native->m_delta) > numberSetting("gestures:workspace_swipe_direction_lock_threshold", 10))
            native->m_initialDirection = direction;
    }
    if (std::abs(native->m_delta) < 0.001) {
        swipe->progress = 0;
        g_pHyprRenderer->damageMonitor(monitor);
        return;
    }
    const bool left = native->m_delta < 0;
    const bool useR = numberSetting("gestures:workspace_swipe_use_r", 0);
    auto id = resolveWorkspaceKey(useR ? (left ? "r-1" : "r+1") : (left ? "m-1" : "m+1")).id;
    const bool create = numberSetting("gestures:workspace_swipe_create_new", 1);
    if (!left && id <= workspaceKey(swipe->origin) && create)
        id = resolveWorkspaceKey("r+1").id;
    if ((left && id >= workspaceKey(swipe->origin)) || id == WORKSPACE_INVALID || id == workspaceKey(swipe->origin)) {
        native->m_delta = 0;
        swipe->progress = 0;
        return;
    }
    swipe->requestedTarget = id;
    const auto target = workspaceForKey(id);
    if ((target && target->m_monitor == monitor) || (!target && create)) {
        if (!swipe->prepared || swipe->target != target || swipe->visual.active != id)
            prepareSwipe(target);
        swipe->progress = std::clamp(std::abs(native->m_delta) / distance, 0.0, 1.0);
        g_pHyprRenderer->damageMonitor(monitor);
    } else {
        swipe->target.reset();
        swipe->prepared = false;
        swipe->progress = 0;
        if (!create)
            native->m_delta = 0;
    }
    if (numberSetting("gestures:workspace_swipe_forever", 0) && std::abs(native->m_delta) >= distance) {
        const auto session = native->sessionID();
        endSwipe();
        swipeBeginThunk(native, monitor);
        // Native trackpad/touch owners keep the same session across segments,
        // as CUnifiedWorkspaceSwipeGesture::update() does for native forever.
        native->m_sessionID = session;
    }
}

void StageController::Impl::endSwipe() {
    if (sessionLocked())
        return;

    if (!swipe || swipe->released)
        return;
    const auto monitor = swipe->monitor.lock();
    if (!monitor || monitor->m_activeWorkspace != swipe->origin || !ownsTransition(monitor->m_activeWorkspace, true)) {
        clearSwipe();
        return;
    }
    auto* native = swipe->native;
    const double distance = std::max(1.0, numberSetting("gestures:workspace_swipe_distance", 300));
    const double force = numberSetting("gestures:workspace_swipe_min_speed_to_force", 30);
    const bool commit = !swipe->cancelled && std::abs(native->m_delta) >= 2 &&
        (std::abs(native->m_delta) >= distance * numberSetting("gestures:workspace_swipe_cancel_ratio", 0.5, 0, 1) || (force > 0 && native->m_avgSpeed >= force));
    native->cancel();
    swipe->released = true;
    swipe->releaseFrom = swipe->progress;
    swipe->releaseTo = commit ? 1 : 0;
    swipe->releaseStart = Clock::now();
    swipe->releaseDuration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L) * std::abs(swipe->releaseTo - swipe->releaseFrom);
    if (commit) {
        if (swipe->prepared) {
            for (auto& flight : swipe->visual.flights) {
                retargetFlight(flight, swipe->progress, monitor);
            }
            swipe->releaseDuration = std::max(16.0, swipe->releaseDuration);
        }
        auto target = swipe->target.lock();
        if (!target && swipe->requestedTarget != WORKSPACE_INVALID && numberSetting("gestures:workspace_swipe_create_new", 1))
            target = createWorkspaceForKey(swipe->requestedTarget, monitor->m_id);
        if (!target) {
            clearSwipe();
            return;
        }
        swipe->target = target;
        swipe->committed = true;
        monitor->changeWorkspace(target, false, true);
        if (!swipe)
            return;
        if (swipe->prepared) {
            auto* actual = screenFor(monitor);
            if (!actual || actual->suspended) {
                clearSwipe();
                return;
            }
            swipe->visual.cards = actual->cards;
            swipe->visual.geometry = actual->geometry;
            swipe->visual.scroll = actual->scroll;
            for (auto& flight : swipe->visual.flights) {
                const auto window = flight.preview.window.lock();
                if (!window)
                    continue;
                const auto next = std::ranges::find_if(actual->flights, [&](const auto& f) { return f.preview.window == window; });
                if (flight.offscreen) {
                    if (window->m_workspace == target) {
                        window->positionAnimation()->warp();
                        window->sizeAnimation()->warp();
                        flight.to = setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
                            CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
                    }
                    continue;
                }
                if (next != actual->flights.end()) {
                    flight.to = next->to;
                    flight.toViewport = next->toViewport;
                } else if (actual->covered && window->m_workspace == target) {
                    window->positionAnimation()->warp();
                    window->sizeAnimation()->warp();
                    flight.to = setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
                        CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
                }
            }
            actual->flights.clear();
            actual->paneTransition = false;
            actual->departingCards.clear();
        } else {
            clearSwipe();
            return;
        }
    }
    if (!swipe->prepared || swipe->releaseDuration <= 0)
        clearSwipe();
    else
        armMotion();
}

void StageController::Impl::damage(const Screen& screen) {
    if (const auto monitor = screen.monitor.lock(); monitor && g_pHyprRenderer)
        g_pHyprRenderer->damageMonitor(monitor);
}

bool StageController::Impl::installHooks() {
    static_assert(std::is_same_v<RecheckFn, decltype(&recheckThunk)>);
    static_assert(std::is_same_v<DragEndFn, decltype(&dragThunk)>);
    static_assert(std::is_same_v<WindowFocusFn, decltype(&windowFocusThunk)>);
    static_assert(std::is_same_v<ScrollFocusFn, decltype(&scrollFocusThunk)>);
    static_assert(std::is_same_v<WindowAtFn, decltype(&windowAtThunk)>);
    static_assert(std::is_same_v<RoundingFn, decltype(&roundingThunk)>);
    static_assert(std::is_same_v<RenderWindowFn, decltype(&renderWindowThunk)>);
    static_assert(std::is_same_v<SurfaceBoxFn, decltype(&surfaceBoxThunk)>);
    static_assert(std::is_same_v<SurfaceVisibleFn, decltype(&surfaceVisibleThunk)>);
    static_assert(std::is_same_v<SurfaceUVFn, decltype(&surfaceUVThunk)>);
    static_assert(std::is_same_v<AddPassFn, decltype(&addPassThunk)>);
    static_assert(std::is_same_v<WorkspaceAnimationFn, decltype(&workspaceAnimationThunk)>);
    static_assert(std::is_same_v<ChangeWorkspaceFn, decltype(&changeWorkspaceThunk)>);
    static_assert(std::is_same_v<SwipeBeginFn, decltype(&swipeBeginThunk)>);
    static_assert(std::is_same_v<BlurFramebufferFn, decltype(&blurFramebufferThunk)>);
    static_assert(std::is_same_v<FadeoutsFn, decltype(&fadeoutsThunk)>);
    if (hooksReady)
        return true;
    if (hookFailure)
        return false;
    const auto find = [&](const char* name, const char* qualified) -> void* {
        void* address=hooks::findSymbol(HyprlandAPI::findFunctionsByName(handle,name),qualified);
        if (!address)
            error = std::string("stage disabled: missing symbol ") + qualified;
        return address;
    };
    const auto attach = [&](hooks::Lease* hook, const char* name) {
        if (hook && hook->hook()) {
            return true;
        }
        error = std::string("stage disabled: failed to attach ") + name;
        return false;
    };
    const auto area = find("recheckWorkArea", "Layout::CSpace::recheckWorkArea()");
    const auto drag = find("dragEnd", "Layout::Supplementary::CDragStateController::dragEnd()");
    const auto scrollFocus = find("focusOnInput", "Layout::Tiled::CScrollingAlgorithm::focusOnInput(Hyprutils::Memory::CSharedPointer<Layout::ITarget>, Layout::Tiled::CScrollingAlgorithm::eInputMode)");
    const auto windowFocus = find("rawWindowFocus", "Desktop::CFocusState::rawWindowFocus(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Desktop::eFocusReason, Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>)");
    const auto hit = find("windowAt", "Desktop::CViewHitTester::windowAt(Hyprutils::Math::Vector2D const&, unsigned short, Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>) const");
    const auto rounding = find("rounding", "Desktop::View::CWindowPresentation::rounding()");
    const auto surfaceBox = find("getTexBox", "CSurfacePassElement::getTexBox()");
    const auto surfaceVisible = find("visibleRegion", "CSurfacePassElement::visibleRegion(Render::CRenderContext&, bool&)");
    const auto surfaceUV = find("calculateUVForSurface", "Render::IElementRenderer::calculateUVForSurface(Render::CRenderContext&, Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<CWLSurfaceResource>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, bool, Hyprutils::Math::Vector2D const&, Hyprutils::Math::Vector2D const&, bool)");
    const auto addPass = find("addPassElement", "Render::IHyprRenderer::addPassElement(Render::CRenderContext&, Hyprutils::Memory::CUniquePointer<IPassElement>&&)");
    const auto fadeouts = find("renderFadeouts", "Render::IHyprRenderer::renderFadeouts(Render::CRenderContext&, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, Desktop::eFadeoutPlane, Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>)");
    const auto workspaceAnimation = find("startAnimation", "Animation::Workspace::startAnimation(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace>, Animation::Workspace::eAnimationType, bool, bool, std::optional<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > >)");
    const auto changeWorkspace = find("changeWorkspace", "Monitor::CMonitor::changeWorkspace(Hyprutils::Memory::CSharedPointer<Workspace::CHLWorkspace> const&, bool, bool, bool)");
    renderWindow = reinterpret_cast<RenderWindowFn>(find("renderWindow", "Render::IHyprRenderer::renderWindow(Render::CRenderContext&, Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<Monitor::CMonitor>, Render::SWindowRenderPresentation const&, std::chrono::time_point<std::chrono::_V2::steady_clock, std::chrono::duration<long, std::ratio<1l, 1000000000l> > > const&, bool, Render::eRenderPassMode, bool, bool)"));
    shouldBlur = reinterpret_cast<ShouldBlurFn>(find("shouldBlur", "Desktop::View::CWindow::shouldBlur(Render::SWindowRenderPresentation const&) const"));
    if (area && drag && windowFocus && scrollFocus && hit && rounding && surfaceBox && surfaceVisible && surfaceUV && addPass && fadeouts && workspaceAnimation && changeWorkspace && renderWindow && shouldBlur && g_pHyprOpenGL) {
        areaHook = hooks::create(handle, area, hooks::target(&recheckThunk));
        if (areaHook) areaHook->priority=10; // native -> tile geometry -> Stage reservation
        dragHook = hooks::create(handle, drag, hooks::target(&dragThunk));
        windowFocusHook = hooks::create(handle, windowFocus, hooks::target(&windowFocusThunk));
        scrollFocusHook = hooks::create(handle, scrollFocus, hooks::target(&scrollFocusThunk));
        hitHook = hooks::create(handle, hit, hooks::target(&windowAtThunk));
        roundingHook = hooks::create(handle, rounding, hooks::target(&roundingThunk));
        renderWindowHook = hooks::create(handle, reinterpret_cast<void*>(renderWindow), hooks::target(&renderWindowThunk));
        surfaceBoxHook = hooks::create(handle, surfaceBox, hooks::target(&surfaceBoxThunk));
        surfaceVisibleHook = hooks::create(handle, surfaceVisible, hooks::target(&surfaceVisibleThunk));
        surfaceUVHook = hooks::create(handle, surfaceUV, hooks::target(&surfaceUVThunk));
        addPassHook = hooks::create(handle, addPass, hooks::target(&addPassThunk));
        fadeoutsHook = hooks::create(handle, fadeouts, hooks::target(&fadeoutsThunk));
        workspaceAnimationHook = hooks::create(handle, workspaceAnimation, hooks::target(&workspaceAnimationThunk));
        changeWorkspaceHook = hooks::create(handle, changeWorkspace, hooks::target(&changeWorkspaceThunk));
        hooksReady = attach(areaHook, "work area") && attach(dragHook, "drag end") && attach(windowFocusHook, "drop focus") &&
            attach(scrollFocusHook, "scrolling desktop focus") && attach(hitHook, "window hit test") &&
            attach(roundingHook, "rounding") && attach(renderWindowHook, "window rendering") &&
            (overviewRendering || (attach(surfaceBoxHook, "surface box") && attach(surfaceVisibleHook, "surface visible region") && attach(surfaceUVHook, "surface UV"))) &&
            attach(addPassHook, "decoration pass") && attach(fadeoutsHook, "fading window clipping") &&
            attach(workspaceAnimationHook, "workspace animation") && attach(changeWorkspaceHook, "workspace change");
        if (hooksReady)
            renderWindow = reinterpret_cast<RenderWindowFn>(renderWindowHook->m_original);
    }
    if (!hooksReady) {
        releaseHooks();
        hookFailure = true;
        if (error.empty())
            error = "stage disabled: OpenGL renderer unavailable";
        if (auto* tiles=TilesController::get()) tiles->fail(error);
        LOG(Log::ERR, "[hymission] {}", error);
        HyprlandAPI::addNotification(handle, "[hymission] " + error, CHyprColor(1.0, 0.3, 0.2, 1.0), 8000);
    } else {
        // Soft effects are optional. Missing blur interception must never
        // disable the independent viewport clip or Stage itself.
        (void)ensureEdgeHook();
        if (!blurFramebufferHook)
            LOG(Log::WARN, "[hymission] Stage edge blur hook unavailable; retaining hard clipping");
    }
    return hooksReady;
}

bool StageController::Impl::ensureEdgeHook() {
    if (blurFramebufferHook) return true;
    return edgeHookAttempt.run([&] {
    constexpr auto signature="Render::IHyprRenderer::blurMainFramebuffer(Render::CRenderContext&, float, Hyprutils::Math::CRegion const&, Render::SBlurContext const&)";
    void* address=hooks::findSymbol(HyprlandAPI::findFunctionsByName(handle,"blurMainFramebuffer"),signature,true);
    if (!address) return false;
    blurFramebufferHook=hooks::create(handle,address,hooks::target(&blurFramebufferThunk));
    if (blurFramebufferHook && blurFramebufferHook->hook()) return true;
    hooks::remove(handle,blurFramebufferHook); blurFramebufferHook=nullptr;
    return false;
    });
}

void StageController::Impl::releaseHooks() {
    renderWindow=nullptr;
    shouldBlur=nullptr;
    edgeHookAttempt.release();
    for (auto** hook : {&areaHook, &dragHook, &windowFocusHook, &scrollFocusHook, &hitHook, &roundingHook, &renderWindowHook, &surfaceBoxHook, &surfaceVisibleHook, &surfaceUVHook, &addPassHook,
                        &workspaceAnimationHook, &changeWorkspaceHook, &blurFramebufferHook, &fadeoutsHook}) {
        if (!*hook)
            continue;
        (*hook)->unhook();
        hooks::remove(handle, *hook);
        *hook = nullptr;
    }
    hooksReady = false;
}

void StageController::Impl::initialize() {
    auto& events = Event::bus()->m_events;
    listeners.emplace_back(events.config.reloaded.listen([this] { edgeHookAttempt.reset(); reconfigure = true; request(); }));
    listeners.emplace_back(events.config.props_refreshed.listen([this](bool) { request(); }));
    listeners.emplace_back(events.monitor.layoutChanged.listen([this] { request(); }));
    listeners.emplace_back(events.monitor.added.listen([this](PHLMONITOR) { request(); }));
    listeners.emplace_back(events.monitor.preRemoved.listen([this](PHLMONITOR) { StageController::cancelTileTransfer(); }));
    listeners.emplace_back(events.monitor.removed.listen([this](PHLMONITOR) { request(); }));
    listeners.emplace_back(events.workspace.active.listen([this](PHLWORKSPACE) { request(); }));
    listeners.emplace_back(events.workspace.created.listen([this](PHLWORKSPACEREF) { request(); }));
    listeners.emplace_back(events.workspace.removed.listen([this](PHLWORKSPACEREF) { request(); }));
    listeners.emplace_back(events.workspace.moveToMonitor.listen([this](PHLWORKSPACE, PHLMONITOR) { request(); }));
    listeners.emplace_back(events.workspace.specialActive.listen([this](PHLWORKSPACE, PHLMONITOR) { request(); }));
    listeners.emplace_back(events.window.open.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.close.listen([this](PHLWINDOW window) {
        if (auto* tiles=TilesController::get();tiles && tiles->transferMember(window)) StageController::cancelTileTransfer();
        request();
    }));
    listeners.emplace_back(events.window.floating.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.fullscreen.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.title.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.moveToWorkspace.listen([this](PHLWINDOW, PHLWORKSPACE) { request(); }));
    listeners.emplace_back(events.input.mouse.move.listen([this](const Vector2D& position, Event::SCallbackInfo& info) {
        if (sessionLocked())
            return;
        if (const auto workspace = hoverCenterGuardWorkspace.lock()) {
            const auto monitor = workspace->m_monitor.lock();
            if (!enabled || blocked() || !setting("stage_click_hover_recenter_guard", 1) ||
                !monitor || monitor->m_activeWorkspace != workspace || position.x < hoverCenterGuardX)
                hoverCenterGuardWorkspace.reset();
            hoverCenterGuardX = position.x;
        }
        pointer();
        if (tileDragPending) info.cancelled=true;
        const auto* drag = g_layoutManager->dragController().get();
        if (!info.cancelled && hit(position).first && (!drag || !drag->target()) && !g_pInputManager->hasHeldButtons()) {
            g_pSeatManager->setPointerFocus(nullptr, {});
            info.cancelled = true;
        }
    }));
    listeners.emplace_back(events.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        const auto copy = event;
        button(copy, info);
    }));
    listeners.emplace_back(events.input.mouse.axis.listen([this](const IPointer::SAxisEvent& event, Event::SCallbackInfo& info) {
        if (sessionLocked())
            return;
        axis(event, info);
    }));
    listeners.emplace_back(events.gesture.swipe.update.listen([this](const IPointer::SSwipeUpdateEvent&, Event::SCallbackInfo&) {
        if (sessionLocked())
            return;
        ++rawSwipeUpdates;
    }));
    listeners.emplace_back(events.gesture.swipe.end.listen([this](const IPointer::SSwipeEndEvent& event, Event::SCallbackInfo&) {
        if (sessionLocked())
            return;
        if (swipe)
            swipe->cancelled = event.cancelled;
    }));
    listeners.emplace_back(events.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
        if (sessionLocked())
            return;
        if (event.keycode==KEY_ESC && event.state==WL_KEYBOARD_KEY_STATE_PRESSED && tileDragPending) {
            if (auto* tiles=TilesController::get()) tiles->finishTransfer(false);
            tileDragPending=false; tileDragWorkspace.reset(); tileDragClicked.reset(); info.cancelled=true;
        }
        if (event.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            silentDropWindow.reset();
        const auto* drag = g_layoutManager->dragController().get();
        if (event.keycode == KEY_ESC && event.state == WL_KEYBOARD_KEY_STATE_PRESSED && thumbnailDrag) {
            thumbnailDrag.reset();
            clearDragHover();
        }
        if (event.keycode == KEY_ESC && event.state == WL_KEYBOARD_KEY_STATE_PRESSED && drag && drag->mode() == MBIND_MOVE && drag->target()) {
            cancelDrop = true;
            lastDragged = drag->target()->window();
        }
    }));
    listeners.emplace_back(events.render.stage.listen([this](Event::SRenderStageEvent event) {
        if (event.context) {
            RenderContextScope scope{event.context->get()};
            renderStage(event.stage);
        } else
            renderStage(event.stage);
    }));
    listeners.emplace_back(events.render.preChecks.listen([this](PHLMONITOR monitor) {
        if (!enabled || rendering)
            return;
        auto* screen = screenFor(monitor);
        if (screen && screen->geometry.enabled() && screen->shown > 0 && !renderBlocked(monitor) && !monitor->m_activeSpecialWorkspace)
            monitor->m_solitaryClient.reset(); // per-output: compose the slide before resuming direct scanout
        if (!screen || !sameBox(screen->base, screenBase(monitor,screen->tile)) || screen->scale != monitor->m_scale || screen->transform != static_cast<int>(monitor->m_transform))
            request();
        else if (screen->suspended != (blocked() || !!monitor->m_activeSpecialWorkspace))
            request();
    }));
    if (g_pSessionLockManager) {
        listeners.emplace_back(g_pSessionLockManager->m_events.lock.listen([this] { cancelInput(); }));
        listeners.emplace_back(g_pSessionLockManager->m_events.unlock.listen([this] { request(); }));
    }
    request();
}

void StageController::Impl::cancelInput() {
    if (auto* tiles=TilesController::get(); tileDragPending && tiles) tiles->finishTransfer(false);
    tileDragPending=false; tileDragWorkspace.reset(); tileDragClicked.reset();
    deferred.reset();
    canvasRelease.reset();
    canvasScrolls.clear();
    stopTimer(refreshTimer);
    stopTimer(motionTimer);
    clearSwipe();
    if (g_pUnifiedWorkspaceSwipe) {
        g_pUnifiedWorkspaceSwipe->cancel();

    }
    // Release a native grab without running Stage's drop/activation tail.
    if (auto* drag = g_layoutManager->dragController().get(); dragHook && drag && drag->target())
        reinterpret_cast<DragEndFn>(dragHook->m_original)(drag);

    clearDragHover();
    thumbnailDrag.reset();
    pressed.reset();
    pressedSyntheticId = 0;
    pressMonitor.reset();
    reorderArmed = false;
    swallowedButtons.clear();
    lastDragged.reset();
    cancelDrop = false;
    silentDropWindow.reset();
    silentDropSource.reset();
    hoverCenterGuardWorkspace.reset();
    hoverFocus = false;
    visibilityGesture = false;
    visibilityDelta = 0;
    // Return to the last committed visibility, without an animation/gesture end.
    visibility = visibilityFrom = visibilityTarget = runtimeVisible.value_or(setting("stage_enabled", 0) != 0) ? 1 : 0;
    overviewExitScreens.clear();
    stagePassQueued = false;
    for (auto& screen : screens) {
        screen.suspended = true;
        screen.hovered.reset();
        screen.reorderHover.reset();
        screen.flights.clear();
        screen.departingCards.clear();
        screen.paneTransition = false;
        screen.frozenWidth.reset();
        screen.shown = screen.slideFrom = screen.covered ? 0 : 1;
        for (auto& card : screen.cards)
            card.shift = 0;
        damage(screen);
    }
    updatePreviewLiveness();
    forceRefresh = true;
}

void StageController::Impl::request(bool dirty) {
    if (sessionLocked())
        return;

    if (shuttingDown || rendering)
        return;
    forceRefresh |= dirty;
    if (deferred || !g_pEventLoopManager)
        return;
    deferred = g_pEventLoopManager->doLaterLock([this] {
        deferred.reset();
        sync();
    });
}

void StageController::Impl::afterRecheck(Layout::CSpace* space) {
    if (sessionLocked())
        return;

    if (!enabled || !space)
        return;
    const auto workspace = space->workspace();
    if (!workspace || (workspace->type() == Workspace::eWorkspaceType::SPECIAL))
        return;
    const auto monitor = workspace->m_monitor.lock();
    auto* screen = screenForWorkspace(workspace);
    if (!screen || !screen->geometry.enabled())
        return;
    if (maximizeCover && Fullscreen::controller()->getFullscreenModes(workspace).internal == Fullscreen::FSMODE_MAXIMIZED)
        return;
    // Each invocation starts from the original recheckWorkArea output. Retain
    // workspace gaps, including float gaps, and never compound our reservation.
    // Count native columns, including stacked/grouped targets in one column.
    // A single column follows the visible desktop width; a multi-column tape
    // keeps its sizing viewport and is only translated and clipped.
    for (auto* box : {&space->m_workArea, &space->m_floatingWorkArea}) {
        const auto area = stage::reservedWorkArea(rect(*box), screen->geometry.reservation, visibility, screen->right,
                                                  box == &space->m_workArea && preservesScrollingWidth(workspace));
        *box = hymission::box(area);
    }
    if (!syncing && !sameBox(screen->base, screenBase(monitor,screen->tile)))
        request();
}

std::vector<StageController::Impl::Card> StageController::Impl::cardsForWorkspace(const PHLMONITOR& monitor, const PHLWORKSPACE& activeWorkspace) const {
    const auto workspaces = State::workspaceState()->workspacesCopy();
    const bool showEmpty = setting("stage_show_empty", 1) != 0;
    std::vector<Card> targets;
    for (const auto& workspace : workspaces) {
        if ((workspace->type() == Workspace::eWorkspaceType::SPECIAL) || workspace->m_monitor != monitor)
            continue;
        if (auto* tiles=TilesController::get();tiles && tiles->available()) {
            const auto home=tiles->preferredTile(activeWorkspace,monitor);
            if (tiles->preferredTile(workspace,monitor)!=home) continue;
        } else if (setting("stage_show_active", 0) == 0 && workspace == activeWorkspace)
            continue;
        if (!showEmpty && workspace->getWindowCount(std::nullopt, false) == 0)
            continue;
        targets.emplace_back(Card{.workspace = workspace});
    }
    if (const long emptySlots = setting("stage_empty_slots", 0); emptySlots > 0) {
        WorkspaceKey highest = 0;
        for (const auto& card : targets) {
            if (card.workspace.lock() && cardID(card) > highest)
                highest = cardID(card);
        }
        const WorkspaceKey last = std::max<WorkspaceKey>(highest, static_cast<WorkspaceKey>(emptySlots));
        for (WorkspaceKey id = 1; id <= last; ++id) {
            if (std::ranges::any_of(targets, [&](const Card& card) { return cardID(card) == id; }))
                continue;
            // Never synthesize a slot for an id owned by another monitor.
            if (std::ranges::any_of(workspaces, [&](const auto& workspace) {
                    return workspace && !(workspace->type() == Workspace::eWorkspaceType::SPECIAL) && workspaceKey(workspace) == id && workspace->m_monitor != monitor;
                }))
                continue;
            targets.emplace_back(Card{.syntheticId = id});
        }
    }
    std::ranges::sort(targets, [](const Card& a, const Card& b) {
        const auto ida = cardID(a), idb = cardID(b);
        if ((ida > 0) != (idb > 0))
            return ida > 0;
        return ida > 0 ? ida < idb :
            (a.workspace.lock() && b.workspace.lock() && a.workspace.lock()->displayName() < b.workspace.lock()->displayName());
    });
    return targets;
}

void StageController::Impl::sync() {
    if (sessionLocked())
        return;

    if (syncing || shuttingDown)
        return;
    if (g_pHyprRenderer && renderContext().m_data.pMonitor) {
        request(false);
        return;
    }
    syncing = true;
    const bool configured = setting("stage_enabled", 0) != 0;
    if (!runtimeVisible) {
        runtimeVisible = configured;
        visibility = visibilityTarget = configured ? 1 : 0;
    }
    const bool wanted = *runtimeVisible || enabled || visibilityGesture || visibility > 0;
    const bool wasEnabled = enabled;
    enabled = wanted && installHooks();
    if (!enabled) {
        canvasRelease.reset();
        canvasScrolls.clear();
        if (!TilesController::get() || std::ranges::none_of(State::monitorState()->monitors(), [](const auto& mon) { return TilesController::get()->enabled(mon); }))
            edgeEffect.reset();
        clearSwipe();
        stopTimer(refreshTimer);
        stopTimer(motionTimer);
        auto previous = std::move(screens);
        screens.clear();
        updatePreviewLiveness();
        if (wasEnabled) {
            for (const auto& screen : previous) {
                if (const auto monitor = screen.monitor.lock()) {
                    g_layoutManager->invalidateMonitorGeometries(monitor);
                    damage(screen);
                }
            }
            g_pInputManager->simulateMouseMovement();
        }
        forceRefresh = false;
        syncing = false;
        return;
    }
    if (reconfigure)
        edgeEffect.reset();
    if (swipe && (reconfigure || blocked()))
        clearSwipe();

    static auto gapsInValue = CConfigValue<Config::IComplexConfigValue>("general:gaps_in");
    static auto gapsOutValue = CConfigValue<Config::IComplexConfigValue>("general:gaps_out");
    const auto& inner = *static_cast<Config::CCssGapData*>(gapsInValue.ptr());
    const auto& outer = *static_cast<Config::CCssGapData*>(gapsOutValue.ptr());
    const double padding = setting("stage_padding", -1);
    const double cardGap = setting("stage_card_gap", -1);
    const double desktopGap = setting("stage_desktop_gap", -1);
    const stage::Settings raw{
        .minWidth = static_cast<double>(setting("stage_card_min_width", 120)),
        .maxWidth = static_cast<double>(setting("stage_card_max_width", 0)),
        .padding = padding < 0 ? static_cast<double>(outer.m_left) : padding,
        .cardGap = cardGap < 0 ? static_cast<double>(inner.m_top + inner.m_bottom) : cardGap,
        .desktopGap = desktopGap < 0 ? 0 : desktopGap,
        .paddingTop = padding < 0 ? static_cast<double>(outer.m_top) : padding,
        // Native windows already receive gaps_out on the reduced work area.
        // Add only the remainder of the desired two-sided inner gap here.
        .paddingRight = padding < 0 ? std::max(0.0, static_cast<double>(inner.m_left + inner.m_right - outer.m_left)) : padding,
        .paddingBottom = padding < 0 ? static_cast<double>(outer.m_bottom) : padding};
    settings = stage::normalize(raw);
    if ((reconfigure || !wasEnabled) && (raw.minWidth != settings.minWidth || raw.maxWidth != settings.maxWidth || raw.padding != settings.padding || raw.cardGap != settings.cardGap || raw.desktopGap != settings.desktopGap))
        LOG(Log::WARN, "[hymission] normalized invalid stage widths/padding/gaps");
    const bool nextMaximizeCover = setting("stage_maximize_cover_strip", 0) != 0;
    const bool changedPolicy = maximizeCover != nextMaximizeCover;
    maximizeCover = nextMaximizeCover;
    const bool nextSmartisan = setting("stage_smartisan_mode", 0) != 0;
    const bool changedMode = nextSmartisan != smartisan;
    if (changedMode)
        clearSwipe();
    smartisan = nextSmartisan;
    auto* drag = g_layoutManager->dragController().get();
    const auto dragged = drag && drag->mode() == MBIND_MOVE && drag->target() ? drag->target()->window() : nullptr;
    if (dragged != lastDragged) {
        cancelDrop = false;
        lastDragged = dragged;
    }

    std::erase_if(screens, [&](const auto& screen) {
        const auto monitor = screen.monitor.lock();
        if (!monitor || std::ranges::find(State::monitorState()->monitors(), monitor) == State::monitorState()->monitors().end()) return true;
        if (auto* tiles=TilesController::get();tiles && tiles->available()) {
            auto views=tiles->views(monitor);
            return std::ranges::none_of(views,[&](const auto& v) { return v.id==screen.tile; });
        }
        return screen.tile!=0;
    });
    std::vector<PHLMONITOR> relayout;
    struct PendingFlight {
        PHLMONITOR monitor;
        tiles::Id tile;
        WorkspaceKey previous;
        std::vector<Card> cards;
        stage::Geometry geometry;
        double scroll;
        bool right;
        std::vector<std::pair<PHLWINDOWREF, CBox>> origins;
    };
    std::vector<PendingFlight> pendingFlights;
    const auto monitorSnapshot=State::monitorState()->monitors();
    for (const auto& monitor : monitorSnapshot) {
        auto views=TilesController::get()?TilesController::get()->views(monitor):std::vector<TilesController::View>{};
        if (views.empty()) views.push_back({monitor,monitor->m_activeWorkspace,0,CBox{monitor->m_position,monitor->m_size},true});
        for (const auto& view:views) {
        const auto activeWorkspace=view.workspace;
        auto* screen = screenFor(monitor,view.id);
        if (!screen) {
            screens.emplace_back();
            screen = &screens.back();
            screen->monitor = monitor;
            screen->tile=view.id;
        }
        screen->workspace=activeWorkspace;
        const auto base = screenBase(monitor,view.id);
        const auto oldGeometry = screen->geometry;
        const bool switching = screen->active != WORKSPACE_INVALID && activeWorkspace && screen->active != workspaceKey(activeWorkspace);
        const bool changedOutput = !sameBox(base, screen->base) || screen->scale != monitor->m_scale || screen->transform != static_cast<int>(monitor->m_transform);
        if (visibility == 1 && !visibilityGesture && !finishingOverview && switching && !changedOutput && !changedPolicy && !reconfigure && !blocked() && !screen->covered && !screen->suspended) {
            pendingFlights.push_back({monitor, view.id, screen->active, screen->cards, oldGeometry, screen->scroll, screen->right, {}});
            for (const auto& window : Desktop::windowState()->windows()) {
                if (window->m_monitor == monitor && (!view.id || window->m_workspace==activeWorkspace || workspaceKey(window->m_workspace)==screen->active) && window->mapped() && !window->isHidden() && !static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED))
                    pendingFlights.back().origins.emplace_back(window, setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
                        CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()});
            }
        }
        const bool oldRight = screen->right;
        if (!smartisan || changedMode)
            screen->right = false;
        if (visibility == 1 && !visibilityGesture && !finishingOverview && smartisan && switching && !blocked() && !screen->covered && !screen->suspended)
            screen->right = !screen->right;
        const bool changedSide = oldRight != screen->right;
        if (changedSide && smartisan && switching && !changedOutput && !reconfigure && numberSetting("animations:enabled", 1) != 0 && setting("stage_transition_ms", 300) > 0) {
            screen->departingCards = screen->cards;
            screen->departingGeometry = oldGeometry;
            screen->departingScroll = screen->scroll;
            screen->departingRight = oldRight;
            screen->paneTransition = true;
            screen->paneStart = {};
        }
        screen->base = base;
        screen->scale = monitor->m_scale;
        screen->transform = static_cast<int>(monitor->m_transform);

        auto targets = cardsForWorkspace(monitor, activeWorkspace);
        bool changedCards = targets.size() != screen->cards.size();
        if (!changedCards) {
            for (std::size_t i = 0; i < targets.size(); ++i)
                changedCards |= screen->cards[i].workspace != targets[i].workspace || cardID(screen->cards[i]) != cardID(targets[i]);
        }
        std::vector<std::pair<WorkspaceKey, double>> oldTops;
        for (std::size_t i = 0; i < screen->cards.size(); ++i)
            oldTops.emplace_back(cardID(screen->cards[i]), cardTop(*screen, i) + screen->scroll);
        if (changedCards) {
            std::vector<Card> cards;
            for (const auto& target : targets) {
                auto it = std::ranges::find_if(screen->cards, [&](const auto& card) { return cardID(card) == cardID(target); });
                // Never reuse a synthetic slot for a workspace that now exists
                // (or vice versa): the card's rendering path differs.
                if (it != screen->cards.end() && (it->workspace.lock() ? !target.syntheticId : !!target.syntheticId))
                    cards.emplace_back(std::move(*it));
                else
                    cards.push_back(target);
            }
            screen->cards = std::move(cards);
        }
        if (dragged && !screen->frozenWidth && oldGeometry.enabled())
            screen->frozenWidth = oldGeometry.cardWidth;
        if (!dragged)
            screen->frozenWidth.reset();
        auto screenSettings = settingsForSide(screen->right);
        screen->geometry = stage::layout(base.w, base.h, targets.size(), screenSettings, screen->frozenWidth, screen->tile?base.w:monitor->m_size.x);
        ensureBackground(*screen, monitor);
        const bool changedGeometry = changedSide || changedOutput || oldGeometry.reservation != screen->geometry.reservation || oldGeometry.cardHeight != screen->geometry.cardHeight ||
            oldGeometry.padding != screen->geometry.padding || oldGeometry.paddingTop != screen->geometry.paddingTop ||
            oldGeometry.paddingBottom != screen->geometry.paddingBottom || oldGeometry.cardGap != screen->geometry.cardGap;
        screen->scroll = screen->geometry.clampScroll(screen->scroll);
        if (changedCards || changedGeometry) {
            for (std::size_t i = 0; i < screen->cards.size(); ++i) {
                auto& card = screen->cards[i];
                const auto old = std::ranges::find_if(oldTops, [&](const auto& entry) { return entry.first == cardID(card); });
                card.shift = !finishingOverview && !blocked() && !changedOutput && numberSetting("animations:enabled", 1) && setting("stage_transition_ms", 300) > 0 && old != oldTops.end() ?
                    old->second - screen->geometry.cardTop(i, 0) : 0;
                card.shiftStart = Clock::now();
                if (card.shift != 0)
                    armMotion();
            }
        }
        const WorkspaceKey active = activeWorkspace ? workspaceKey(activeWorkspace) : WORKSPACE_INVALID;
        const bool changedActive = screen->active != active;
        const bool preserveWidth = preservesScrollingWidth(activeWorkspace);
        const bool changedWidthPolicy = screen->preserveScrollingWidth != preserveWidth;
        screen->preserveScrollingWidth = preserveWidth;
        screen->active = active;
        const auto mode = activeWorkspace ? Fullscreen::controller()->getFullscreenModes(activeWorkspace).internal : Fullscreen::FSMODE_NONE;
        const bool cover = stage::coversStrip(mode == Fullscreen::FSMODE_FULLSCREEN ? stage::CoverMode::Fullscreen :
                                               mode == Fullscreen::FSMODE_MAXIMIZED ? stage::CoverMode::Maximized : stage::CoverMode::None, maximizeCover);
        const bool changedCover = screen->covered != cover;
        if (changedCover) {
            screen->slideFrom = screen->shown;
            screen->slideStart = Clock::now();
            screen->covered = cover;
            armMotion();
        }
        const bool suspended = blocked() || !!monitor->m_activeSpecialWorkspace;
        if (finishingOverview) {
            screen->shown = cover || !!monitor->m_activeSpecialWorkspace ? 0 : 1;
            screen->slideFrom = screen->shown;
        }
        const bool changedSuspension = screen->suspended != suspended;
        screen->suspended = suspended;
        if (suspended || cover || changedOutput || reconfigure || (!switching && (changedCards || changedGeometry)))
            screen->flights.clear();
        if (suspended || cover || changedOutput || reconfigure || changedMode || !screen->geometry.enabled()) {
            screen->paneTransition = false;
            screen->departingCards.clear();
        }
        if (changedGeometry || changedPolicy || changedWidthPolicy || changedCover || reconfigure)
            relayout.push_back(monitor);
        for (auto& card : screen->cards) {
            if (changedGeometry) {
                card.previews.clear();
                card.previewsReady = false;
            }
            card.dirty |= forceRefresh || changedGeometry || changedCards || changedActive || changedSuspension || changedCover;
        }
        if (changedGeometry || changedCards || changedActive || changedCover || changedSuspension)
            damage(*screen);
    }
        } // each tile
    for (const auto& monitor : relayout) {
        g_layoutManager->invalidateMonitorGeometries(monitor);
        for (const auto& screen:screens) if (screen.monitor==monitor && screen.geometry.enabled())
            correctFloating(monitor,desktop(screen),screen.tile?screenWorkspace(screen):nullptr);
    }
    for (const auto& pending : pendingFlights) {
        if (auto* screen = screenFor(pending.monitor,pending.tile); screen && !screen->covered && !screen->suspended) {
            // Native layout owns the destination, but Stage owns its motion.
            // Resolve native layout animations once, after saving source boxes,
            // so a second position/size animation cannot continue after a flight.
            for (const auto& window : Desktop::windowState()->windows()) {
                if (window->m_monitor != pending.monitor || !window->mapped() || window->isHidden() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) || window->onSpecialWorkspace())
                    continue;
                window->positionAnimation()->warp();
                window->sizeAnimation()->warp();
            }
            startFlights(*screen, pending.previous, pending.cards, pending.geometry, pending.scroll, pending.right, pending.origins,screenWorkspace(*screen));
        }
    }
    forceRefresh = false;
    reconfigure = false;
    syncing = false;
    refreshPreviews();
    updatePreviewLiveness();
    for (auto& screen : screens) {
        if (screen.paneTransition && screen.paneStart == Clock::time_point{}) {
            screen.paneStart = screen.flights.empty() ? Clock::now() : screen.flightStart;
            screen.paneDuration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L);
            armMotion();
        }
    }
    armRefresh();
}

void StageController::Impl::armRefresh() {
    if (sessionLocked())
        return;

    stopTimer(refreshTimer);
    if (!enabled || !g_pEventLoopManager)
        return;
    const auto interval = std::clamp(setting("stage_refresh_ms", 16), 1L, 16L);
    refreshTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(interval), [this](SP<CEventLoopTimer>, void*) {
        request();
    }, nullptr);
    g_pEventLoopManager->addTimer(refreshTimer);
}

void StageController::Impl::armMotion() {
    if (sessionLocked())
        return;

    if (motionTimer || !enabled)
        return;
    motionTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(16), [this](SP<CEventLoopTimer>, void*) {
        stopTimer(motionTimer);
        motion();
    }, nullptr);
    g_pEventLoopManager->addTimer(motionTimer);
}

void StageController::Impl::motion() {
    if (sessionLocked())
        return;

    bool again = false;
    const auto now = Clock::now();
    if (!visibilityGesture && visibility != visibilityTarget) {
        const double duration = numberSetting("animations:enabled", 1) ? std::clamp(setting("stage_transition_ms", 300), 0L, 2000L) * std::abs(visibilityTarget - visibilityFrom) : 0;
        const double elapsed = std::chrono::duration<double, std::milli>(now - visibilityStart).count();
        const double t = duration > 0 ? std::clamp(elapsed / duration, 0.0, 1.0) : 1;
        applyVisibility(t == 1 ? visibilityTarget : visibilityFrom + (visibilityTarget - visibilityFrom) * (1 - std::pow(1 - t, 3)));
        again |= t < 1;
    }
    if (swipe) {
        const auto monitor = swipe->monitor.lock();
        auto* actual = screenFor(monitor);
        const bool finishingStageSwipe = swipe->prepared && swipe->committed && swipe->released;
        if (!monitor || !actual || blocked() || (actual->covered && !finishingStageSwipe && !swipe->sourceCovered) || actual->suspended || (swipe->prepared && !sameBox(actual->base, swipe->visual.base)) ||
            (swipe->committed ? monitor->m_activeWorkspace != swipe->target : monitor->m_activeWorkspace != swipe->origin)) {
            clearSwipe();
        } else {
            if (swipe->released) {
                const double elapsed = std::chrono::duration<double, std::milli>(now - swipe->releaseStart).count();
                const double p = swipe->releaseDuration > 0 ? std::clamp(elapsed / swipe->releaseDuration, 0.0, 1.0) : 1;
                swipe->settleProgress = p;
                swipe->progress = swipe->releaseFrom + (swipe->releaseTo - swipe->releaseFrom) * p;
                if (swipe->committed && (swipe->targetCovered || swipe->sourceCovered))
                    actual->shown = swipe->targetCovered ? 1 - stage::transitionProgress(swipe->progress, 1) : stage::transitionProgress(swipe->progress, 1);
                if (p >= 1)
                    clearSwipe();
            }
            g_pHyprRenderer->damageMonitor(monitor);
            again = swipe.has_value();
        }
    }
    auto* drag = g_layoutManager->dragController().get();
    const bool dragging = thumbnailDrag || (drag && drag->mode() == MBIND_MOVE && drag->target());
    const auto point = g_pInputManager->getMouseCoordsInternal();
    for (auto& screen : screens) {
        if (screen.paneTransition) {
            const auto elapsed = std::chrono::duration<double, std::milli>(now - screen.paneStart).count();
            if (blocked() || screen.covered || screen.suspended || elapsed >= screen.paneDuration) {
                screen.paneTransition = false;
                screen.departingCards.clear();
                request();
            } else
                again = true;
            damage(screen);
        }
        for (auto& card : screen.cards) {
            if (card.shift == 0)
                continue;
            const double elapsed = std::chrono::duration<double, std::milli>(now - card.shiftStart).count();
            if (elapsed >= std::clamp(setting("stage_transition_ms", 300), 0L, 2000L))
                card.shift = 0;
            else
                again = true;
            damage(screen);
        }
        if (!screen.flights.empty()) {
            if (blocked() || screen.covered || screen.suspended || flightProgress(screen) >= 1) {
                screen.flights.clear();
                request();
            } else
                again = true;
            damage(screen);
        }
        const double target = screen.covered ? 0 : 1;
        const bool swipeOwnsVisibility = swipe && swipe->prepared && swipe->committed && (swipe->targetCovered || swipe->sourceCovered) && swipe->monitor == screen.monitor;
        if (screen.shown != target && !swipeOwnsVisibility) {
            const double duration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L);
            const auto t = duration > 0 ? std::clamp(std::chrono::duration<double, std::milli>(now - screen.slideStart).count() / duration, 0.0, 1.0) : 1;
            const auto eased = 1 - std::pow(1 - t, 3);
            screen.shown = t == 1 ? target : screen.slideFrom + (target - screen.slideFrom) * eased;
            damage(screen);
            again |= t < 1;
        }
        const auto& geometry = screen.geometry;
        const auto band = sidebar(screen);
        if (!dragging || !interactive(screen) || screen.paneTransition || point.x < band.x || point.x >= band.x + band.w ||
            point.y < screen.base.y || point.y >= screen.base.y + screen.base.h)
            continue;
        const double y = point.y - screen.base.y;
        const double delta = y < 32 ? -8 : y > screen.base.h - 32 ? 8 : 0;
        const double scroll = geometry.clampScroll(screen.scroll + delta);
        if (scroll != screen.scroll) {
            screen.scroll = scroll;
            screen.hovered = cardHit(screen, point);
            damage(screen);
            request(false);
            again = true;
        }
    }
    updateDragHover();
    again |= dragHover.has_value();
    if (again)
        armMotion();
}

std::pair<StageController::Impl::Screen*, std::optional<std::size_t>> StageController::Impl::hit(const Vector2D& point, bool activation) {
    // Use native input regions/stacking, including transparent input holes.
    // Call the original directly to avoid re-entering our windowAt hook.
    if (hitHook) {
        const auto tester = Desktop::viewState()->hitTest();
        const auto top = reinterpret_cast<WindowAtFn>(hitHook->m_original)(&tester, point,
            Desktop::View::ALLOW_FLOATING | Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS, nullptr);
        if (top && static_cast<bool>(top->m_state & Desktop::View::WINDOW_STATE_PINNED) && top->isFloating())
            return {nullptr, std::nullopt};
    }
    for (auto& screen : screens) {
        if (!interactive(screen))
            continue;
        const auto band = sidebar(screen);
        if (swipe && swipe->prepared && swipe->monitor == screen.monitor) {
            const auto destinationBand = sidebar(swipe->visual);
            if (point.y >= screen.base.y && point.y < screen.base.y + screen.base.h &&
                ((point.x >= band.x && point.x < band.x + band.w) ||
                 (point.x >= destinationBand.x && point.x < destinationBand.x + destinationBand.w)))
                return {&screen, std::nullopt};
            continue;
        }
        if (screen.paneTransition) {
            // Do not click through either moving copy into the live desktop.
            if (point.y >= screen.base.y && point.y < screen.base.y + screen.base.h &&
                ((point.x >= screen.base.x && point.x < screen.base.x + std::max(screen.departingGeometry.bandWidth, screen.geometry.bandWidth)) ||
                 (point.x >= screen.base.x + screen.base.w - std::max(screen.departingGeometry.bandWidth, screen.geometry.bandWidth) && point.x < screen.base.x + screen.base.w) ||
                 (point.x >= band.x && point.x < band.x + band.w)))
                return {&screen, std::nullopt};
            continue;
        }
        const auto monitor = screen.monitor.lock();
        const double left = !screen.right && monitor ? (screen.tile?screen.base.x:monitor->m_position.x) : band.x;
        const double right = screen.right && monitor ? (screen.tile?screen.base.x+screen.base.w:monitor->m_position.x+monitor->m_size.x) : band.x + band.w;
        if (point.x >= left && point.x < right && point.y >= band.y && point.y < band.y + band.h)
            return {&screen, cardHit(screen, point, activation)};
    }
    return {nullptr, std::nullopt};
}

void StageController::Impl::pointer() {
    if (sessionLocked())
        return;

    if (!enabled || rendering || (inputSuppressed && inputSuppressed()))
        return;
    const auto point = g_pInputManager->getMouseCoordsInternal();
    const auto [hoveredScreen, index] = hit(point);
    if (tileDragPending) {
        auto* tiles=TilesController::get();
        if (!tiles || !tiles->updateTransfer(point,hoveredScreen?hoveredScreen->monitor.lock():nullptr,hoveredScreen?hoveredScreen->tile:0,
                hoveredScreen && index?hoveredScreen->cards[*index].workspace.lock():nullptr)) {
            tileDragPending=false; tileDragWorkspace.reset(); tileDragClicked.reset();
        } else {
            if (auto mon=tileDragMonitor.lock()) g_pHyprRenderer->damageMonitor(mon);
            armMotion();
        }
        return;
    }
    // Reorder gesture: pressed on a card and moved onto a different one.
    if ((pressed.lock() || pressedSyntheticId > 0) && pressMonitor.lock() && (point.distanceSq(pressPoint) > 100.0 || reorderArmed))
        reorderArmed = true;
    for (auto& screen : screens) {
        const auto hovered = &screen == hoveredScreen ? index : std::nullopt;
        if (screen.hovered != hovered) {
            screen.hovered = hovered;
            damage(screen);
        }
        std::optional<std::size_t> reorderTarget;
        if (reorderArmed && screen.monitor.lock() == pressMonitor.lock() && !screen.cards.empty())
            reorderTarget = reorderTargetIndex(screen, point);
        if (screen.reorderHover != reorderTarget) {
            screen.reorderHover = reorderTarget;
            damage(screen);
        }
    }
    auto* drag = g_layoutManager->dragController().get();
    if (drag && drag->mode() == MBIND_MOVE && drag->target()) {
        const auto window = drag->target()->window();
        if (lastDragged != window)
            request(false);
        if (hoveredScreen)
            armMotion();
    }
    updateDragHover();
}

stage::ScrollingFlightFrame StageController::Impl::dragHoverFrame() const {
    if (!dragHover)
        return {};
    const auto monitor = dragHover->monitor.lock();
    if (!monitor)
        return {};
    const double duration = numberSetting("animations:enabled", 1) ? std::clamp(setting("stage_transition_ms", 300), 0L, 2000L) : 0;
    const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - dragHover->start).count();
    if (!dragHover->edgeTransfer && dragHover->onDesktop)
        return {stage::transitionBox(rect(dragHover->from), rect(dragHover->to), duration > 0 ? elapsed / duration : 1),
            stage::edgeViewport(rect(CBox{monitor->m_position, monitor->m_size}), 0, 0)};
    if (!dragHover->edgeTransfer)
        return {stage::transitionBoxWithin(rect(dragHover->from), rect(dragHover->to), duration > 0 ? elapsed / duration : 1,
            flightBounds(monitor)), stage::edgeViewport(flightBounds(monitor), 0, 0)};
    return stage::edgeTransferFrame(rect(dragHover->from), rect(dragHover->to), dragHover->fromViewport, dragHover->toViewport,
        duration > 0 ? elapsed / duration : 1);
}

void StageController::Impl::clearDragHover() {
    if (!dragHover)
        return;
    if (const auto monitor = dragHover->monitor.lock())
        g_pHyprRenderer->damageMonitor(monitor);
    if (const auto window = dragHover->window.lock(); window && window->m_monitor)
        g_pHyprRenderer->damageMonitor(window->m_monitor.lock());
    dragHover.reset();
}

void StageController::Impl::traceDrop(const char* phase, const PHLWINDOW& window, const PHLWORKSPACE& destination, bool begin) {
    if (!window || !destination)
        return;
    if (begin) {
        dropTrace = nlohmann::json::array();
        dropTraceStart = Clock::now();
    }
    dropPhase = phase;
    const auto monitor = destination->m_monitor.lock();
    dropTrace.push_back({{"event", phase}, {"window_workspace", window->m_workspace ? workspaceKey(window->m_workspace) : -1},
        {"destination", workspaceKey(destination)}, {"active", monitor && monitor->m_activeWorkspace ? workspaceKey(monitor->m_activeWorkspace) : -1},
        {"follow", setting("stage_drop_follow", 0)}, {"floating", window->isFloating()}});
}

void StageController::Impl::updateDragHover() {
    const auto* drag = g_layoutManager->dragController().get();
    const auto dragged = drag && drag->mode() == MBIND_MOVE && drag->target() ? drag->target()->window() : nullptr;
    const auto window = thumbnailDrag ? thumbnailDrag.lock() : dragged ? dragged :
        dragHover && dragHover->returningToNative ? dragHover->window.lock() : nullptr;
    // Native move drags temporarily float tiled windows; retain their original policy.
    const bool tiled = window && ((dragHover && dragHover->window == window && dragHover->tiled) ||
        (dragged == window && drag->draggingTiled()) || !window->isFloating());
    const auto pointer = g_pInputManager->getMouseCoordsInternal();
    auto [screen, index] = hit(pointer);
    bool onDesktop = false;
    if (thumbnailDrag && !screen) {
        for (auto& candidate : screens) {
            const auto area = desktop(candidate);
            if (interactive(candidate) && area.containsPoint(pointer)) {
                screen = &candidate;
                onDesktop = true;
                break;
            }
        }
    }
    // Inter-card gaps remain inside Stage, including for native desktop drags.
    // Otherwise card -> gap -> card becomes two desktop edge transfers.
    const bool inGap = screen && !index && !onDesktop;
    if (!enabled || blocked() || cancelDrop || !window || !window->mapped() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED)) {
        clearDragHover();
        if (!enabled || blocked() || !window || !window->mapped())
            thumbnailDrag.reset();
        return;
    }
    if (!screen || (!index && !onDesktop && !inGap) ||
        (!thumbnailDrag && (!dragged || (index && screen->cards[*index].workspace == window->m_workspace)))) {
        // Keep ownership of the visual until it has returned to the moving
        // native window. Sampling first also preserves a mid-entry reversal.
        auto* nativeScreen = screenFor(window->m_monitor.lock());
        if (!thumbnailDrag && dragHover && dragHover->window == window && nativeScreen && interactive(*nativeScreen)) {
            const auto from = dragHoverFrame();
            if (!dragHover->returningToNative) {
                dragHover->from = box(from.window);
                dragHover->fromViewport = dragHover->edgeTransfer ? from.viewport : dragHover->toViewport;
                dragHover->start = Clock::now();
                dragHover->returningToNative = true;
                dragHover->onDesktop = true;
                dragHover->edgeTransfer = !tiled;
            }
            if (const auto oldMonitor = dragHover->monitor.lock())
                g_pHyprRenderer->damageMonitor(oldMonitor);
            dragHover->monitor = window->m_monitor;
            dragHover->toViewport = desktopViewport(*nativeScreen);
            dragHover->to = setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
                CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
            const double duration = numberSetting("animations:enabled", 1) ? std::clamp(setting("stage_transition_ms", 300), 0L, 2000L) : 0;
            if (std::chrono::duration<double, std::milli>(Clock::now() - dragHover->start).count() >= duration)
                clearDragHover();
            else {
                damage(*nativeScreen);
                armMotion();
            }
        } else
            clearDragHover();
        return;
    }
    const auto monitor = screen->monitor.lock();
    if (!monitor) {
        clearDragHover();
        return;
    }
    const PHLWORKSPACEREF workspace = onDesktop ? monitor->m_activeWorkspace : inGap ? PHLWORKSPACEREF{} : screen->cards[*index].workspace;
    const WorkspaceKey syntheticId = !onDesktop && !inGap ? screen->cards[*index].syntheticId : 0;
    const CBox native = setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
        CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
    // Preview placement follows the pointer inside the card, without moving the
    // real window or changing its workspace while the native drag is held.
    const double scale = onDesktop ? 1 : screen->geometry.cardWidth / screen->geometry.desktopWidth;
    const CBox card = onDesktop ? desktop(*screen) : inGap ? sidebar(*screen) : CBox{sidebar(*screen).x + screen->geometry.padding, screen->base.y + cardTop(*screen, *index),
        screen->geometry.cardWidth, screen->geometry.cardHeight};
    const CBox target = box(stage::dragTargetBox(rect(native), rect(card), pointer.x, pointer.y, scale, onDesktop));
    if (!dragHover || dragHover->returningToNative || dragHover->onDesktop != onDesktop || dragHover->window != window || dragHover->workspace != workspace || dragHover->syntheticId != syntheticId || dragHover->monitor != monitor) {
        const bool fromDesktop = !dragHover || dragHover->window != window || dragHover->onDesktop;
        const auto* nativeScreen = screenFor(window->m_monitor.lock());
        auto from = dragHover && dragHover->window == window ? dragHoverFrame() :
            stage::ScrollingFlightFrame{rect(native), nativeScreen ? desktopViewport(*nativeScreen) : stage::edgeViewport(flightBounds(monitor), 0, 0)};
        if (dragHover && dragHover->window == window && !dragHover->edgeTransfer && fromDesktop != onDesktop)
            from.viewport = dragHover->toViewport;
        clearDragHover();
        dragHover = DragHover{window, monitor, workspace, syntheticId, box(from.window), target, Clock::now()};
        dragHover->fromViewport = from.viewport;
        dragHover->onDesktop = onDesktop;
        dragHover->edgeTransfer = !thumbnailDrag && !tiled && fromDesktop != onDesktop;
        dragHover->tiled = tiled;
    } else
        dragHover->to = target;
    dragHover->toViewport = onDesktop ? desktopViewport(*screen) : cardViewport(card);
    g_pHyprRenderer->damageMonitor(monitor);
    if (window->m_monitor != monitor)
        g_pHyprRenderer->damageMonitor(window->m_monitor.lock());
    armMotion();
}

void StageController::Impl::button(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
    const bool suppressed=sessionLocked() || rendering || (inputSuppressed && inputSuppressed());
    if (tileDragPending && event.button==BTN_LEFT && event.state==WL_POINTER_BUTTON_STATE_RELEASED) {
        auto* tiles=TilesController::get();
        const bool owned=tiles && tiles->owner()==TilesController::Owner::StageDrag;
        const bool dragged=tiles && tiles->transferDragging();
        auto workspace=tileDragWorkspace.lock(); auto clicked=tileDragClicked.lock(); auto mon=tileDragMonitor.lock();
        const auto id=tileDragSource;
        tileDragPending=false; tileDragWorkspace.reset(); tileDragClicked.reset();
        swallowedButtons.erase(event.button); pressed.reset(); pressedSyntheticId=0; reorderArmed=false;
        info.cancelled=true;
        if (tiles) {
            tiles->finishTransfer(dragged && !cancelDrop && !suppressed);
            if (owned && !dragged && !suppressed && workspace && mon) {
                tiles->activate(mon,id,workspace);
                if (clicked && clicked->mapped()) Desktop::focusState()->fullWindowFocus(clicked,Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
            }
        }
        request();
        return;
    }
    if (suppressed) {
        StageController::cancelTileTransfer();
        if (event.state==WL_POINTER_BUTTON_STATE_RELEASED && swallowedButtons.erase(event.button)) info.cancelled=true;
        return;
    }
    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
        silentDropWindow.reset();
    if (auto* tiles=TilesController::get();tiles && tiles->handleDivider(event,info)) return;
    if (thumbnailDrag && event.button == BTN_LEFT && event.state == WL_POINTER_BUTTON_STATE_RELEASED) {
        swallowedButtons.erase(event.button);
        pressed.reset();
        pressedSyntheticId = 0;
        reorderArmed = false;
        info.cancelled = true;
        finishThumbnailDrag();
        return;
    }
    // A consumed press owns its release even if a reload/overview/fullscreen
    // transition happened in between. Never send an unmatched release to a client.
    if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && swallowedButtons.erase(event.button)) {
        const auto workspace = pressed.lock();
        const auto pressedSynthetic = std::exchange(pressedSyntheticId, 0);
        if (event.button == BTN_LEFT)
            pressed.reset();
        const auto [screen, indexRaw] = hit(g_pInputManager->getMouseCoordsInternal(), !reorderArmed);
        info.cancelled = true;
        std::optional<std::size_t> index = indexRaw;
        if (event.button == BTN_LEFT && screen && !index && reorderArmed && screen->monitor.lock() == pressMonitor.lock() && !screen->cards.empty())
            index = reorderTargetIndex(*screen, g_pInputManager->getMouseCoordsInternal());
        if (event.button == BTN_LEFT && screen && index) {
            const auto& card = screen->cards[*index];
            if (reorderArmed && pressMonitor.lock() == screen->monitor.lock() && pressCard != *index) {
                // Rearrange: swap the contents of the pressed and hovered cards.
                const auto monitor = screen->monitor.lock();
                auto dragged = pressed.lock();
                if (!dragged && pressedSynthetic > 0)
                    dragged = ensureWorkspace(monitor, pressedSynthetic);
                auto destination = card.workspace.lock();
                if (!destination && card.syntheticId > 0)
                    destination = ensureWorkspace(monitor, card.syntheticId);
                if (dragged && destination && dragged != destination)
                    swapWorkspaces(dragged, destination);
            } else if (workspace && card.workspace == workspace) {
                // Resolve the topmost visible preview before activating: switching
                // workspaces rebuilds cards and can invalidate this reference.
                PHLWINDOW clicked;
                if (!reorderArmed && card.previewsReady && cardHit(*screen, g_pInputManager->getMouseCoordsInternal()) == index) {
                    const Vector2D origin{sidebar(*screen).x + screen->geometry.padding, screen->base.y + cardTop(*screen, *index)};
                    const auto point = g_pInputManager->getMouseCoordsInternal();
                    for (auto it = card.previews.rbegin(); it != card.previews.rend(); ++it) {
                        const auto window = it->window.lock();
                        if (!window || !window->mapped() || window->isHidden() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) || window->m_workspace != workspace ||
                            previewLayer(window) == stage::PreviewLayer::Hidden || flying(*screen, window) || thumbnailDrag == window || (TilesController::get() && TilesController::get()->transferDragging() && TilesController::get()->transferMember(window)) ||
                            !it->target.copy().translate(origin).containsPoint(point))
                            continue;
                        clicked = window;
                        break;
                    }
                }
                // Selecting a window overrides a saved canvas position, including
                // the activation guard's deferred restore. Blank-card activation
                // still preserves the camera exactly as scrolled.
                if (clicked)
                    std::erase_if(canvasScrolls, [&](const auto& saved) { return saved.workspace == workspace; });
                activate(workspace, true);
                if (clicked && clicked->mapped() && !clicked->isHidden() && clicked->m_workspace == workspace) {
                    const bool alreadyFocused = Desktop::focusState()->window() == clicked;
                    Desktop::focusState()->fullWindowFocus(clicked, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
                    // Native focus is a no-op for an already focused window, but
                    // its column can still be outside the newly scrolled view.
                    if (alreadyFocused && Desktop::focusState()->window() == clicked && scrollingTiled(clicked)) {
                        auto* scrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(workspace->space()->algorithm()->tiledAlgo().get());
                        if (scrolling)
                            scrolling->focusOnInput(clicked->layoutTarget(), Layout::Tiled::CScrollingAlgorithm::INPUT_MODE_HARD);
                    }
                }
            } else if (pressedSynthetic && card.syntheticId == pressedSynthetic && !card.workspace.lock()) {
                if (const auto monitor = screen->monitor.lock())
                    activate(ensureWorkspace(monitor, pressedSynthetic), true);
            }
        }
        for (auto& s : screens) {
            if (s.reorderHover) {
                s.reorderHover.reset();
                damage(s);
            }
        }
        reorderArmed = false;
        return;
    }
    // Releases belonging to a press outside the strip (including application
    // file/text drag-and-drop) must reach the existing grab unchanged.
    if (event.state == WL_POINTER_BUTTON_STATE_RELEASED || info.cancelled || g_pInputManager->hasHeldButtons())
        return;
    auto* drag = g_layoutManager->dragController().get();
    if (drag && drag->target())
        return; // native grab must receive its release exactly once
    const auto [screen, index] = hit(g_pInputManager->getMouseCoordsInternal(),
        !reorderArmed && !(g_pInputManager->getModsFromAllKBs() & Input::HL_MODIFIER_META));
    if (!screen)
        return;
    info.cancelled = true;
    swallowedButtons.insert(event.button);
    if (event.button == BTN_LEFT && event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (auto* tiles=TilesController::get(); tiles && tiles->available() && index && screen->tile) {
            const auto& card=screen->cards[*index];
            auto workspace=card.workspace.lock();
            const Vector2D origin{sidebar(*screen).x+screen->geometry.padding,screen->base.y+cardTop(*screen,*index)};
            const CBox cardBox{origin,{screen->geometry.cardWidth,screen->geometry.cardHeight}};
            PHLWINDOW clicked;
            for (auto it=card.previews.rbegin();it!=card.previews.rend();++it) if (auto w=it->window.lock();w && it->target.copy().translate(origin).containsPoint(g_pInputManager->getMouseCoordsInternal())) { clicked=w; break; }
            std::vector<PHLWINDOW> members;
            if (clicked) {
                if (const auto group=clicked->grouping().group()) { for (const auto& ref:group->windows()) if (auto w=ref.lock()) members.push_back(w); }
                else members.push_back(clicked);
            } else if (workspace) {
                for (const auto& w:Desktop::windowState()->windows()) if (w->mapped() && w->m_workspace==workspace && !static_cast<bool>(w->m_state & Desktop::View::WINDOW_STATE_PINNED)) members.push_back(w);
            }
            if (tiles->beginTransfer(members,screen->monitor.lock(),screen->tile,cardBox,g_pInputManager->getMouseCoordsInternal())) {
                tileDragPending=true; tileDragWorkspace=workspace; tileDragClicked=clicked; tileDragMonitor=screen->monitor; tileDragSource=screen->tile;
                pressed.reset(); pressedSyntheticId=0; reorderArmed=false; cancelDrop=false;
                g_pSeatManager->setPointerFocus(nullptr,{});
                return;
            }
        }
        if (index && cardHit(*screen, g_pInputManager->getMouseCoordsInternal()) == index && (g_pInputManager->getModsFromAllKBs() & Input::HL_MODIFIER_META)) {
            const auto& card = screen->cards[*index];
            const Vector2D origin{sidebar(*screen).x + screen->geometry.padding, screen->base.y + cardTop(*screen, *index)};
            for (auto it = card.previews.rbegin(); it != card.previews.rend(); ++it) {
                const auto window = it->window.lock();
                const auto box = it->target.copy().translate(origin);
                if (!window || !window->mapped() || window->isHidden() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) || !box.containsPoint(g_pInputManager->getMouseCoordsInternal()))
                    continue;
                thumbnailDrag = window;
                pressed.reset();
                pressedSyntheticId = 0;
                cancelDrop = false;
                clearDragHover();
                dragHover = DragHover{window, screen->monitor, card.workspace, card.syntheticId, box, box, Clock::now()};
                dragHover->tiled = !window->isFloating();
                dragHover->fromViewport = dragHover->toViewport = cardViewport(CBox{origin, {screen->geometry.cardWidth, screen->geometry.cardHeight}});
                g_pSeatManager->setPointerFocus(nullptr, {});
                armMotion();
                damage(*screen);
                return;
            }
            return; // Win+click on empty card space does not switch workspace.
        }
        pressed = index ? screen->cards[*index].workspace : PHLWORKSPACEREF{};
        pressedSyntheticId = index ? screen->cards[*index].syntheticId : 0;
        pressPoint = g_pInputManager->getMouseCoordsInternal();
        pressMonitor = screen->monitor;
        pressCard = index.value_or(static_cast<std::size_t>(-1));
        reorderArmed = false;
    }
}

void StageController::Impl::axis(const IPointer::SAxisEvent& event, Event::SCallbackInfo& info) {
    if (sessionLocked())
        return;

    silentDropWindow.reset();
    if (info.cancelled)
        return;
    const auto [screen, index] = hit(g_pInputManager->getMouseCoordsInternal());
    if (!screen)
        return;
    info.cancelled = true;
    if (swipe && swipe->monitor == screen->monitor)
        return;
    if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;
    if (!screen->flights.empty()) {
        screen->flights.clear();
    }
    const double delta = event.delta != 0 ? event.delta * 3 : (event.deltaDiscrete / 120.0) * 32;
    screen->scroll = screen->geometry.clampScroll(screen->scroll + delta);
    pointer();
    damage(*screen);
    request(false);
}

void StageController::Impl::finishThumbnailDrag() {
    if (sessionLocked())
        return;

    updateDragHover();
    const auto window = thumbnailDrag.lock();
    auto destination = dragHover ? dragHover->workspace.lock() : nullptr;
    const auto syntheticId = dragHover ? dragHover->syntheticId : 0;
    const auto monitor = dragHover ? dragHover->monitor.lock() : nullptr;
    if (!destination && syntheticId > 0)
        destination = ensureWorkspace(monitor, syntheticId);
    const auto frame = dragHoverFrame();
    const bool edgeTransfer = dragHover && dragHover->edgeTransfer;
    const CBox from = box(frame.window);
    Vector2D center = dragHover && (edgeTransfer || dragHover->onDesktop) ? dragHover->to.middle() : from.middle();
    if (const auto [screen, index] = hit(g_pInputManager->getMouseCoordsInternal()); screen && index) {
        const auto area = desktop(*screen);
        const auto mapped = stage::mapPreviewCenter(
            {sidebar(*screen).x + screen->geometry.padding, screen->base.y + cardTop(*screen, *index),
                screen->geometry.cardWidth, screen->geometry.cardHeight},
            {area.x, area.y, area.w, area.h}, center.x, center.y);
        center = {mapped.first, mapped.second};
    }
    thumbnailDrag.reset();
    clearDragHover();
    if (!window || !destination || !monitor || blocked() || cancelDrop)
        return;
    const bool toDesktop = destination->visible();
    traceDrop("thumbnail_release", window, destination, true);
    finishDrop(destination, window, from, stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1, -1, 4096),
        numberSetting("decoration:rounding", 0), from.w, from.h), center, {}, center, frame.viewport, edgeTransfer);
    if (!toDesktop || window->m_workspace != destination)
        return;
    forceRefresh = true;
    sync();
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
    if (auto* screen = screenFor(monitor); screen && interactive(*screen) && numberSetting("animations:enabled", 1) && setting("stage_transition_ms", 300) > 0) {
        window->positionAnimation()->warp();
        window->sizeAnimation()->warp();
        const CBox target = setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
            CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
        screen->flights.push_back({Preview{window, target, target}, from, target,
            window->presentation().rounding(), window->presentation().rounding()});
        screen->flights.back().desktopDecorations = true;
        screen->flightStart = Clock::now();
        screen->flightDuration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L);
        updatePreviewLiveness();
        damage(*screen);
        armMotion();
    }
}

void StageController::Impl::activate(const PHLWORKSPACE& workspace, bool cardClick) {
    if (sessionLocked())
        return;

    silentDropWindow.reset();
    if (!workspace || (workspace->type() == Workspace::eWorkspaceType::SPECIAL))
        return;
    const auto monitor = workspace->m_monitor.lock();
    if (!monitor)
        return;
    hoverCenterGuardWorkspace.reset();
    if (auto* tiles=TilesController::get(); tiles && tiles->available()) {
        const auto [column,index]=hit(g_pInputManager->getMouseCoordsInternal(),true);
        const auto id=column && column->monitor==monitor?column->tile:tiles->preferredTile(workspace,monitor);
        if (id && tiles->activate(monitor,id,workspace)) { request(); return; }
    }
    const bool switched = monitor->m_activeWorkspace != workspace;
    Desktop::focusState()->rawMonitorFocus(monitor);
    monitor->changeWorkspace(workspace, false, true);
    if (cardClick && switched && scrollingWorkspace(workspace) && setting("stage_click_hover_recenter_guard", 1)) {
        hoverCenterGuardWorkspace = workspace;
        hoverCenterGuardX = g_pInputManager->getMouseCoordsInternal().x;
    }
    request();
}

// Rearrange: exchange the contents of two workspaces. Both destinations are
// pinned persistent for the duration of the swap: moving a's last window out
// would otherwise leave a empty, and Hyprland destroys empty workspaces, so
// the second loop could target a workspace that no longer exists. Persistence
// is restored afterwards; an endpoint that ends empty is then reaped exactly
// as Hyprland reaps it on its own.
void StageController::Impl::swapWorkspaces(const PHLWORKSPACE& a, const PHLWORKSPACE& b) {
    if (sessionLocked())
        return;

    if (!a || !b || a == b)
        return;
    std::vector<PHLWINDOW> aWindows, bWindows;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->mapped() || window->isHidden() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) || window->onSpecialWorkspace())
            continue;
        if (window->m_workspace == a)
            aWindows.push_back(window);
        else if (window->m_workspace == b)
            bWindows.push_back(window);
    }
    if (aWindows.empty() && bWindows.empty())
        return;
    const bool aWasPersistent = dynamicPointerCast<Workspace::CRegularWorkspace>(a)->isPersistent();
    const bool bWasPersistent = dynamicPointerCast<Workspace::CRegularWorkspace>(b)->isPersistent();
    dynamicPointerCast<Workspace::CRegularWorkspace>(a)->setPersistent(true);
    dynamicPointerCast<Workspace::CRegularWorkspace>(b)->setPersistent(true);
    for (const auto& window : aWindows)
        Desktop::globalWindowController()->moveWindowToWorkspace(window, b);
    for (const auto& window : bWindows)
        Desktop::globalWindowController()->moveWindowToWorkspace(window, a);
    dynamicPointerCast<Workspace::CRegularWorkspace>(a)->setPersistent(aWasPersistent);
    dynamicPointerCast<Workspace::CRegularWorkspace>(b)->setPersistent(bWasPersistent);
    forceRefresh = true;
    request();
}

bool StageController::Impl::endDrag(Layout::Supplementary::CDragStateController* drag) {
    if (sessionLocked()) {
        return reinterpret_cast<DragEndFn>(dragHook->m_original)(drag);
    }
    PHLWORKSPACE destination;
    PHLWINDOW window;
    Vector2D dropPoint;
    Vector2D grabOffset;
    std::optional<Vector2D> floatingCenter;
    CBox dropOrigin;
    std::optional<stage::EdgeViewport> originViewport;
    bool edgeTransfer = false;
    float dropRadius = 0;
    // Keybind handling clears the native threshold flag after starting a drag.
    // With a zero threshold, motion never sets it again: dragging is immediate.
    const bool thresholdReached = numberSetting("binds:drag_threshold", 0) <= 0 || drag->dragThresholdReached();
    if (enabled && !cancelDrop && drag->mode() == MBIND_MOVE && thresholdReached && drag->target()) {
        const auto [screen, index] = hit(g_pInputManager->getMouseCoordsInternal());
        if (screen && index) {
            destination = screen->cards[*index].workspace.lock();
            if (!destination && screen->cards[*index].syntheticId > 0) {
                if (const auto cardMonitor = screen->monitor.lock())
                    destination = ensureWorkspace(cardMonitor, screen->cards[*index].syntheticId);
            }
            window = drag->target()->window();
            const auto pointer = g_pInputManager->getMouseCoordsInternal();
            const auto& geometry = screen->geometry;
            const auto band = sidebar(*screen);
            const auto area = desktop(*screen);
            const auto mapped = stage::mapDropPoint(
                {band.x + geometry.padding, screen->base.y + cardTop(*screen, *index), geometry.cardWidth, geometry.cardHeight},
                {area.x, area.y, area.w, area.h}, pointer.x, pointer.y);
            dropPoint = {mapped.first, mapped.second};
            if (window) {
                grabOffset = pointer - window->positionAnimation()->value();
                dropOrigin = setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
                    CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
                dropRadius = window->presentation().rounding();
                if (dragHover && dragHover->window == window) {
                    const auto frame = dragHoverFrame();
                    dropOrigin = box(frame.window);
                    originViewport = frame.viewport;
                    edgeTransfer = dragHover->edgeTransfer;
                    dropRadius = stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1, -1, 4096),
                        numberSetting("decoration:rounding", 0), dropOrigin.w, dropOrigin.h);
                }
                const auto placement = edgeTransfer ? dragHover->to.middle() : dropOrigin.middle();
                const auto center = stage::mapPreviewCenter(
                    {band.x + geometry.padding, screen->base.y + cardTop(*screen, *index), geometry.cardWidth, geometry.cardHeight},
                    {area.x, area.y, area.w, area.h}, placement.x, placement.y);
                floatingCenter = Vector2D{center.first, center.second};
            }
        }
    }
    updateDragHover();
    if (destination || cancelDrop || !dragHover || !dragHover->returningToNative)
        clearDragHover();
    traceDrop("native_drag_end", window, destination, true);
    const bool ended = reinterpret_cast<DragEndFn>(dragHook->m_original)(drag);
    traceDrop("native_drag_ended", window, destination);
    finishDrop(destination, window, dropOrigin, dropRadius, dropPoint, grabOffset, floatingCenter, originViewport, edgeTransfer);
    return ended;
}

void StageController::Impl::finishDrop(const PHLWORKSPACE& destination, const PHLWINDOW& window, const CBox& dropOrigin,
                                     float dropRadius, const Vector2D& dropPoint, const Vector2D& grabOffset,
                                     std::optional<Vector2D> floatingCenter, std::optional<stage::EdgeViewport> originViewport, bool edgeTransfer) {
    if (sessionLocked())
        return;

    // Use the compositor's normal move path after its drag controller has
    // restored tiling/floating and completed the pointer grab.
    const bool sameWorkspace = destination && window && window->m_workspace == destination;
    const auto layoutTarget = window ? window->layoutTarget() : nullptr;
    // A thumbnail drag can reposition a floating window within its own card.
    // Tiled windows still require a different workspace for a Stage drop.
    if (destination && window && window->mapped() && (!sameWorkspace || (layoutTarget && layoutTarget->floating())) &&
        workspaceForKey(workspaceKey(destination)) == destination && destination->m_monitor) {
        const auto monitor = destination->m_monitor.lock();
        // A later client/FFM focus callback must not undo a silent Stage drop.
        silentDropWindow.reset();
        if (!setting("stage_drop_follow", 0) && !destination->visible()) {
            silentDropWindow = window;
            silentDropSource = monitor->m_activeWorkspace;
            silentDropStart = Clock::now();
        }
        traceDrop("move_window", window, destination);
        if (!sameWorkspace)
            Desktop::globalWindowController()->moveWindowToWorkspace(window, destination);
        traceDrop("place_window", window, destination);
        if (const auto target = window->layoutTarget(); target && target->space() == destination->space()) {
            if (target->floating()) {
                const auto size = target->position().size();
                target->setPositionGlobal(CBox{floatingCenter ? *floatingCenter - size / 2 : dropPoint - grabOffset, size});
            } else {
                // Reinsert through the destination algorithm with a global
                // focal point; never warp the real cursor into a hidden desktop.
                destination->space()->remove(target);
                destination->space()->move(target, dropPoint);
            }
        }
        if (auto* screen = screenFor(monitor))
            correctFloating(monitor, desktop(*screen));
        const bool followDrop = !sameWorkspace && setting("stage_drop_follow", 0);
        traceDrop("restore_focus", window, destination);
        if (followDrop) {
            activate(destination);
            Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
        } else if (!destination->visible() && Desktop::focusState()->window() == window) {
            const auto focusedMonitor = Desktop::focusState()->monitor();
            const auto current = focusedMonitor ? focusedMonitor->m_activeWorkspace : nullptr;
            auto focus = current ? current->getLastFocusedWindow() : nullptr;
            if (!focus || focus == window || focus->m_workspace != current)
                focus = current ? current->getFirstWindow() : nullptr;
            Desktop::focusState()->fullWindowFocus(focus, Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
        }
        const double duration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L);
        if (!followDrop && !destination->visible() && duration > 0 && numberSetting("animations:enabled", 1) && !blocked()) {
            // Resolve the destination layout and sidebar before constructing the
            // flight. The source was saved before native dragEnd restored tiling.
            lastDragged.reset();
            forceRefresh = true;
            sync();
            if (auto* screen = screenFor(monitor); screen && interactive(*screen)) {
                const auto card = std::ranges::find_if(screen->cards, [&](const auto& c) { return c.workspace == destination; });
                if (card != screen->cards.end()) {
                    if (const auto target = window->layoutTarget(); target && target->floating()) {
                        if (!edgeTransfer) {
                            const auto area = desktop(*screen);
                            const auto center = stage::mapPreviewCenter(
                                {sidebar(*screen).x + screen->geometry.padding,
                                 screen->base.y + cardTop(*screen, std::distance(screen->cards.begin(), card)),
                                 screen->geometry.cardWidth, screen->geometry.cardHeight},
                                {area.x, area.y, area.w, area.h}, dropOrigin.middle().x, dropOrigin.middle().y);
                            floatingCenter = Vector2D{center.first, center.second};
                        }
                        // Keep the mapped pointer placement after releasing frozen
                        // geometry; the animated source may now be outside its clip.
                        target->setPositionGlobal(CBox{floatingCenter.value_or(dropPoint) - target->position().size() / 2,
                            target->position().size()});
                    }
                    window->positionAnimation()->warp();
                    window->sizeAnimation()->warp();
                    updatePreviews(*screen, *card);
                    const auto preview = std::ranges::find_if(card->previews, [&](const auto& p) { return p.window == window; });
                    if (preview != card->previews.end()) {
                        const double p = flightProgress(*screen);
                        for (auto& flight : screen->flights) {
                            retargetFlight(flight, p, monitor);
                        }
                        std::erase_if(screen->flights, [&](const auto& f) { return f.preview.window == window; });
                        const auto target = preview->target.copy().translate(Vector2D{sidebar(*screen).x + screen->geometry.padding,
                            screen->base.y + cardTop(*screen, std::distance(screen->cards.begin(), card))});
                        screen->flights.push_back({*preview, dropOrigin, target, dropRadius, static_cast<float>(stage::previewRounding(
                            numberSetting("plugin:hymission:stage_window_rounding", -1, -1, 4096), numberSetting("decoration:rounding", 0), target.w, target.h)), true});
                        if (edgeTransfer || scrollingTiled(window)) {
                            screen->flights.back().fromViewport = edgeTransfer ? originViewport.value_or(desktopViewport(*screen)) :
                                stage::edgeViewport(rect(dropOrigin), 0, 0);
                            screen->flights.back().toViewport = cardViewport(CBox{sidebar(*screen).x + screen->geometry.padding,
                                screen->base.y + cardTop(*screen, std::distance(screen->cards.begin(), card)), screen->geometry.cardWidth, screen->geometry.cardHeight});
                        }
                        screen->flights.back().edgeTransfer = edgeTransfer;
                        screen->flightStart = Clock::now();
                        screen->flightDuration = duration;
                        updatePreviewLiveness();
                        damage(*screen);
                        armMotion();
                    }
                }
            }
        }
    }
    lastDragged.reset();
    cancelDrop = false;
    traceDrop("complete", window, destination);
    request();
}

void StageController::Impl::correctFloating(const PHLMONITOR& monitor, const CBox& desktop, const PHLWORKSPACE& workspace) {
    const auto* drag = g_layoutManager->dragController().get();
    for (const auto& window : Desktop::windowState()->windows()) {
        if ((workspace && (window->m_workspace!=workspace || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED))) || !window->mapped() || !window->isFloating() || window->m_monitor != monitor || window->onSpecialWorkspace() || Fullscreen::controller()->isFullscreen(window) ||
            (drag && drag->target() && drag->target()->window() == window))
            continue;
        const auto target = window->layoutTarget();
        if (!target)
            continue;
        auto box = target->position();
        // Keep the user's placement if at least a small reachable piece remains.
        if (box.x + box.w >= desktop.x + 24 && box.x <= desktop.x + desktop.w - 24 && box.y + box.h >= desktop.y + 24 && box.y <= desktop.y + desktop.h - 24)
            continue;
        box.x = std::clamp(box.x, desktop.x, std::max(desktop.x, desktop.x + desktop.w - box.w));
        box.y = std::clamp(box.y, desktop.y, std::max(desktop.y, desktop.y + desktop.h - box.h));
        target->setPositionGlobal(box);
    }
}

void StageController::Impl::renderStage(eRenderStage stage) {
    if (stage == RENDER_PRE && !rendering && !renderContext().m_renderingSnapshot)
        stagePassQueued = false;
    if (!enabled || rendering || stagePassQueued)
        return;
    const auto monitor = renderContext().m_data.pMonitor.lock();
    if (renderBlocked(monitor))
        return;
    const auto progress = sidebarProgress ? sidebarProgress(monitor) : std::nullopt;
    if (progress && *progress >= 1.0)
        return;
    auto* screen = visualScreenFor(monitor);
    if (TilesController::get() && TilesController::get()->available()) {
        const auto visible=std::ranges::find_if(screens,[&](const Screen& s) { return s.monitor==monitor && s.geometry.enabled() && s.shown>0; });
        if (visible!=screens.end()) screen=&*visible;
    }
    if (!screen || !screen->geometry.enabled() || monitor->m_activeSpecialWorkspace || (screen->shown <= 0 || visibility <= 0))
        return;
    if (stage == (progress ? RENDER_PRE_WINDOWS : RENDER_POST_WINDOWS)) {
        stagePassQueued = true;
        const PHLMONITORREF ref = monitor;
        renderContext().m_pass.add(makeUnique<StagePassElement>([this, ref] {
            if (const auto mon = ref.lock())
                draw(mon);
        }, !progress && !screen->tile && screen->flights.empty() && !screen->paneTransition && !dragHover ? sidebar(*screen).translate(-monitor->m_position)
                                  : CBox{{}, monitor->m_size}));
    }
}

void StageController::Impl::draw(const PHLMONITOR& monitor) {
    if (TilesController::get() && TilesController::get()->available()) {
        for (auto& screen:screens) if (screen.monitor==monitor) { drawingScreen=&screen; drawOne(monitor); }
        drawingScreen=nullptr;
        auto* tiles=TilesController::get();
        const auto members=tiles->transferMembers();
        if (tiles->transferDragging() && !members.empty() && monitor->logicalBox().containsPoint(tiles->transferPoint())) {
            const auto point=tiles->transferPoint();
            std::vector<WindowInput> inputs;
            for (size_t i=0;i<members.size();++i) if (!members[i]->isHidden()) {
                const auto b=members[i]->getFullWindowBoundingBox();
                inputs.push_back({.index=i,.natural={b.x,b.y,b.w,b.h}});
            }
            LayoutConfig config; config.minWindowLength=24; config.outerPaddingTop=config.outerPaddingBottom=config.outerPaddingLeft=config.outerPaddingRight=4;
            MissionControlLayout layout;
            const auto slots=layout.compute(inputs,{point.x-110,point.y-80,220,160},config);
            const CBox clip{{},monitor->m_transformedSize};
            for (const auto& slot:slots) drawPreview(members[slot.index],monitor,box(slot.target),clip,8,stage::edgeViewport({monitor->m_position.x,monitor->m_position.y,monitor->m_size.x,monitor->m_size.y},0,0));
        }
    } else drawOne(monitor);
}
void StageController::Impl::drawOne(const PHLMONITOR& monitor) {
    auto* screen = visualScreenFor(monitor);
    if (!screen || !hooksReady || renderBlocked(monitor) || rendering || !g_pHyprOpenGL)
        return;
    screen->capturePreviews.clear();
    const auto now = Clock::now();
    if (now - screen->lastFrame > std::chrono::seconds(1)) {
        screen->frameSampleStart = now;
        screen->frameCount = 0;
        screen->previewRenderFPS = 0;
    }
    screen->lastFrame = now;
    ++screen->frameCount;
    const double sampleSeconds = std::chrono::duration<double>(now - screen->frameSampleStart).count();
    if (sampleSeconds >= 1) {
        screen->previewRenderFPS = screen->frameCount / sampleSeconds;
        screen->frameSampleStart = now;
        screen->frameCount = 0;
    }
    const auto physical = [&](CBox box) { return box.translate(-monitor->m_position).scale(monitor->m_scale); };
    const auto previousClip = renderContext().m_data.clipBox;
    const CBox outputClip = physical(screen->tile?screen->base:CBox{monitor->m_position, monitor->m_size});
    const auto drawPane = [&](const std::vector<Card>& cards, const stage::Geometry& geometry, double scroll, bool right, double offset, double reveal = -1) {
      const auto area = stage::sidebarArea({screen->base.x, screen->base.y, screen->base.w, screen->base.h}, geometry, right);
      const auto topAt = [&](std::size_t i) {
        const auto& card = cards[i];
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - card.shiftStart).count();
        const double progress = swipe && &swipe->visual == screen ? stage::transitionProgress(swipe->progress, 1) :
            stage::transitionProgress(elapsed, std::clamp(setting("stage_transition_ms", 300), 0L, 2000L));
        return geometry.cardTop(i, scroll) + card.shift * (1 - progress);
      };
      std::vector<std::size_t> visible;
      for (std::size_t i = 0; i < cards.size(); ++i) {
        const double top = topAt(i);
        if (top + geometry.cardHeight > geometry.paddingTop && top < screen->base.h - geometry.paddingBottom)
            visible.push_back(i);
      }
      for (std::size_t rank = 0; rank < visible.size(); ++rank) {
        const auto i = visible[rank];
        const auto& card = cards[i];
        const double top = topAt(i);
        const double cardOffset = reveal < 0 ? offset : offset * (1 - stage::staggeredProgress(reveal, rank, visible.size(), screen->revealFromBottom));
        const auto stripClip = physical(CBox{area.x + cardOffset, area.y + geometry.paddingTop, area.width,
            area.height - geometry.paddingTop - geometry.paddingBottom}).intersection(outputClip);
        if (stripClip.w <= 0 || stripClip.h <= 0)
            continue;
        const auto workspace = card.workspace.lock();
        const CBox box{area.x + geometry.padding + cardOffset, screen->base.y + top, geometry.cardWidth, geometry.cardHeight};
        const auto cardClip = physical(box).intersection(stripClip);
        if (cardClip.w <= 0 || cardClip.h <= 0)
            continue;
        renderContext().m_data.clipBox = cardClip;
        // Backdrop: the desktop region of the cached background framebuffer
        // (wallpaper and background-layer content like desktop dashboards),
        // aligned so every card — occupied, active or synthetic empty — shows
        // the same desk content the desktop itself does. The clip trims the
        // off-desktop part of the framebuffer.
        if (screen->background && screen->background->getTexture()) {
            const auto deskRect = desktop(*screen);
            if (deskRect.w > 0 && deskRect.h > 0) {
                const double sx = box.w / deskRect.w;
                const double sy = box.h / deskRect.h;
                const CBox backdropBox{box.x - (deskRect.x - monitor->m_position.x) * sx,
                                       box.y - (deskRect.y - monitor->m_position.y) * sy,
                                       monitor->m_size.x * sx, monitor->m_size.y * sy};
                g_pHyprOpenGL->renderTexture(renderContext(), screen->background->getTexture(), physical(backdropBox), {.a = 1.0F});
            }
        }
        if (!workspace) {
            renderContext().m_data.clipBox = stripClip;
            continue;
        }
        if (card.previewsReady) {
            for (const auto& preview : card.previews) {
                const auto window = preview.window.lock();
                if (!window || !window->mapped() || window->isHidden() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) || window->m_workspace != workspace || flying(*screen, window) || thumbnailDrag == window || (TilesController::get() && TilesController::get()->transferDragging() && TilesController::get()->transferMember(window)))
                    continue;
                const auto radius = stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1, -1, 4096),
                    numberSetting("decoration:rounding", 0), preview.target.w, preview.target.h);
                const CBox target = preview.target.copy().translate(box.pos());
                // Export the geometry actually drawn into this frame, including
                // slide/scroll offsets and card/output clipping. Consumers select
                // the preview but capture the native window by its address.
                const CBox logicalClip = cardClip.copy().scale(1.0 / monitor->m_scale).translate(monitor->m_position);
                const CBox visible = target.intersection(logicalClip);
                double renderedRounding = 0;
                drawPreview(window, monitor, target, cardClip, radius,
                    (window->isFloating() || scrollingTiled(window)) ? std::optional{cardViewport(box)} : std::nullopt, &renderedRounding);
                if (previewLayer(window) != stage::PreviewLayer::Hidden && visible.w >= 1 && visible.h >= 1)
                    screen->capturePreviews.push_back({window, target, logicalClip, renderedRounding});
            }
            // The glow is centered on the card edges and clipped only by the
            // output: the sidebar clip would cut the desktop-facing edge.
            if (setting("stage_active_glow", 0) != 0 && workspace == screenWorkspace(*screen)) {
                const auto glow = CHyprColor(static_cast<uint64_t>(setting("focus_selected_color", 0xF23DC7FF)));
                const double glowRound = stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1, -1, 4096),
                    numberSetting("decoration:rounding", 0), box.w, box.h);
                const int ringRound = static_cast<int>(std::lround(glowRound * monitor->m_scale));
                const auto ringBox = physical(CBox{box.x - 2, box.y - 2, box.w + 4, box.h + 4});
                const auto haloBox = physical(CBox{box.x - 7, box.y - 7, box.w + 14, box.h + 14});
                renderContext().m_data.clipBox = outputClip;
                g_pHyprOpenGL->renderBorder(renderContext(), haloBox, Config::CGradientValueData{glow.modifyA(static_cast<float>(glow.a * 0.30))},
                    {.round = ringRound + 5, .roundingPower = 2.0F, .borderSize = 5, .a = 0.5F, .outerRound = -1});
                g_pHyprOpenGL->renderBorder(renderContext(), ringBox, Config::CGradientValueData{glow},
                    {.round = ringRound + 1, .roundingPower = 2.0F, .borderSize = 2, .a = 1.0F, .outerRound = -1});
                renderContext().m_data.clipBox = stripClip;
            }
            // Reorder target marker while dragging a card onto another one.
            if (screen->reorderHover == i) {
                const auto accent = CHyprColor(static_cast<uint64_t>(setting("focus_selected_color", 0xF23DC7FF)));
                const double hoverRound = stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1, -1, 4096),
                    numberSetting("decoration:rounding", 0), box.w, box.h);
                const int hoverRingRound = static_cast<int>(std::lround(hoverRound * monitor->m_scale));
                renderContext().m_data.clipBox = outputClip;
                g_pHyprOpenGL->renderRect(renderContext(), physical(box), accent.modifyA(0.18F), {});
                g_pHyprOpenGL->renderBorder(renderContext(), physical(box), Config::CGradientValueData{accent},
                    {.round = hoverRingRound, .roundingPower = 2.0F, .borderSize = 2, .a = 0.9F, .outerRound = -1});
                renderContext().m_data.clipBox = stripClip;
            }
            renderContext().m_data.clipBox = stripClip;
        }
      }
    };
    const auto progress = sidebarProgress ? sidebarProgress(monitor) : std::nullopt;
    if (progress) {
        // Only-current-workspace overview does not own the sidebar's windows.
        // Slide that separate pane out beneath the overview's window passes.
        const double travel = screen->geometry.bandWidth + 8 + (screen->right ?
            monitor->m_position.x + monitor->m_size.x - (screen->base.x + screen->base.w) :
            screen->base.x - monitor->m_position.x);
        const double hidden = 1 - screen->shown * (1 - std::clamp(*progress, 0.0, 1.0));
        drawPane(screen->cards, screen->geometry, screen->scroll, screen->right,
            (screen->right ? 1 : -1) * travel * hidden);
    } else if (swipe && &swipe->visual == screen && swipe->sourceCovered) {
        if (!swipe->targetCovered) {
            const double travel = screen->right ? monitor->m_position.x + monitor->m_size.x - (screen->base.x + screen->base.w) + screen->geometry.bandWidth :
                screen->base.x - monitor->m_position.x + screen->geometry.bandWidth;
            drawPane(screen->cards, screen->geometry, screen->scroll, screen->right,
                (screen->right ? 1 : -1) * travel, swipe->progress);
        }
    } else if (swipe && &swipe->visual == screen && swipe->targetCovered) {
        const double p = stage::transitionProgress(swipe->progress, 1);
        const double width = screen->departingGeometry.bandWidth;
        const double travel = screen->departingRight ? monitor->m_position.x + monitor->m_size.x - (screen->base.x + screen->base.w) + width :
            screen->base.x - monitor->m_position.x + width;
        drawPane(screen->departingCards, screen->departingGeometry, screen->departingScroll, screen->departingRight,
            (screen->departingRight ? 1 : -1) * travel * p);
    } else if (screen->paneTransition) {
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - screen->paneStart).count();
        const double t = swipe && &swipe->visual == screen ? swipe->progress :
            screen->paneDuration > 0 ? std::clamp(elapsed / screen->paneDuration, 0.0, 1.0) : 1;
        const double p = stage::transitionProgress(t, 1);
        const auto travel = [&](bool right, double width) {
            return right ? monitor->m_position.x + monitor->m_size.x - (screen->base.x + screen->base.w) + width :
                           screen->base.x - monitor->m_position.x + width;
        };
        drawPane(screen->departingCards, screen->departingGeometry, screen->departingScroll, screen->departingRight,
            (screen->departingRight ? 1 : -1) * travel(screen->departingRight, screen->departingGeometry.bandWidth) * p);
        drawPane(screen->cards, screen->geometry, screen->scroll, screen->right,
            (screen->right ? 1 : -1) * travel(screen->right, screen->geometry.bandWidth) * (1 - p));
    } else {
        const double slide = (screen->right ? 1 : -1) * (1 - screen->shown * visibility) * screen->geometry.bandWidth;
        if (!screen->covered && screen->shown < 1 && visibility == 1) {
            const double duration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L);
            const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - screen->slideStart).count();
            const double t = duration > 0 ? std::clamp(elapsed / duration, 0.0, 1.0) : 1;
            drawPane(screen->cards, screen->geometry, screen->scroll, screen->right,
                (screen->right ? 1 : -1) * (1 - screen->slideFrom) * screen->geometry.bandWidth, t);
        } else
            drawPane(screen->cards, screen->geometry, screen->scroll, screen->right, slide);
    }
    renderContext().m_data.clipBox = previousClip;
    if (progress)
        return;
    drawFlights(*screen, monitor);
    if (dragHover && dragHover->monitor == monitor) {
        if (const auto window = dragHover->window.lock()) {
            const auto frame = dragHoverFrame();
            const auto box = hymission::box(frame.window);
            const auto bounds = dragHover->onDesktop ? rect(CBox{monitor->m_position, monitor->m_size}) : flightBounds(monitor);
            const auto clip = physical(CBox{bounds.x, bounds.y, bounds.width, bounds.height});
            const double radius = dragHover->onDesktop ? window->presentation().rounding() : stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1, -1, 4096),
                numberSetting("decoration:rounding", 0), box.w, box.h);
            if (!dragHover->edgeTransfer)
                drawPreview(window, monitor, box, clip, radius, std::nullopt, nullptr, dragHover->onDesktop);
            else if (stage::overviewEndpointVisible({frame.window, frame.viewport, frame.viewport.box}))
                drawPreview(window, monitor, box, clip, radius, frame.viewport, nullptr, dragHover->onDesktop);
        }
    }
}

CBox StageController::Impl::sidebar(const Screen& screen) const {
    const auto box = stage::sidebarArea({screen.base.x, screen.base.y, screen.base.w, screen.base.h}, screen.geometry, screen.right);
    return {box.x, box.y, box.width, box.height};
}

CBox StageController::Impl::desktop(const Screen& screen) const {
    const auto box = stage::desktopArea({screen.base.x, screen.base.y, screen.base.w, screen.base.h}, screen.geometry, screen.right);
    const auto area = stage::visibilityDesktop(rect(screen.base), box, visibility);
    return {area.x, area.y, area.width, area.height};
}

double StageController::Impl::cardTop(const Screen& screen, std::size_t index) const {
    const auto& card = screen.cards[index];
    const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - card.shiftStart).count();
    const double p = stage::transitionProgress(elapsed, std::clamp(setting("stage_transition_ms", 300), 0L, 2000L));
    return screen.geometry.cardTop(index, screen.scroll) + card.shift * (1 - p);
}

std::optional<std::size_t> StageController::Impl::cardHit(const Screen& screen, const Vector2D& point, bool activation) const {
    if (!screen.covered && screen.shown < 1)
        return std::nullopt;
    const auto& g = screen.geometry;
    if (activation) {
        auto bounds = sidebar(screen);
        if (const auto monitor = screen.monitor.lock();monitor && !screen.tile) {
            if (screen.right)
                bounds.w = monitor->m_position.x + monitor->m_size.x - bounds.x;
            else {
                bounds.w += bounds.x - monitor->m_position.x;
                bounds.x = monitor->m_position.x;
            }
        }
        std::vector<Rect> cards;
        for (std::size_t i = 0; i < screen.cards.size(); ++i) {
            const double top = screen.base.y + cardTop(screen, i);
            const double visibleTop = std::max(top, screen.base.y + g.paddingTop);
            const double visibleBottom = std::min(top + g.cardHeight, screen.base.y + screen.base.h - g.paddingBottom);
            cards.push_back({bounds.x, visibleTop, bounds.w, std::max(0.0, visibleBottom - visibleTop)});
        }
        return stage::activationCardAt(cards, rect(bounds), point.x, point.y);
    }
    const auto local = point - sidebar(screen).pos();
    if (local.x < g.padding || local.x >= g.padding + g.cardWidth || local.y < g.paddingTop || local.y >= screen.base.h - g.paddingBottom)
        return std::nullopt;
    // Last drawn card wins while animated bounds overlap.
    for (std::size_t i = screen.cards.size(); i > 0; --i) {
        const double top = cardTop(screen, i - 1);
        if (local.y >= top && local.y < top + g.cardHeight)
            return i - 1;
    }
    return std::nullopt;
}

double StageController::Impl::flightProgress(const Screen& screen) const {
    if (swipe && swipe->prepared && &swipe->visual == &screen)
        return swipe->released && swipe->committed ? swipe->settleProgress : swipe->progress;
    if (screen.flightDuration <= 0)
        return 1;
    return std::clamp(std::chrono::duration<double, std::milli>(Clock::now() - screen.flightStart).count() / screen.flightDuration, 0.0, 1.0);
}

bool StageController::Impl::flying(const Screen& screen, const PHLWINDOW& window) const {
    return window && !static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) && std::ranges::any_of(screen.flights, [&](const auto& flight) { return flight.preview.window == window; });
}

void StageController::Impl::startFlights(Screen& screen, WorkspaceKey previous, const std::vector<Card>& oldCards,
                                       const stage::Geometry& oldGeometry, double oldScroll, bool oldRight,
                                       const std::vector<std::pair<PHLWINDOWREF, CBox>>& origins, PHLWORKSPACE visualActive, bool stopNative) {
    const auto monitor = screen.monitor.lock();
    const auto activeWorkspace = visualActive ? visualActive : screenWorkspace(screen);
    const double duration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L);
    if (!monitor || duration == 0 || numberSetting("animations:enabled", 1) == 0 || !oldGeometry.enabled() || !screen.geometry.enabled()) {
        screen.flights.clear();
        return;
    }
    const double oldProgress = flightProgress(screen);
    auto previousFlights = std::move(screen.flights);
    std::vector<PHLWORKSPACE> workspaces;
    const auto addWorkspace = [&](const PHLWORKSPACE& workspace) {
        if (workspace && workspace->m_monitor == monitor && !(workspace->type() == Workspace::eWorkspaceType::SPECIAL) && std::ranges::find(workspaces, workspace) == workspaces.end())
            workspaces.push_back(workspace);
    };
    addWorkspace(workspaceForKey(previous));
    addWorkspace(activeWorkspace);
    for (const auto& flight : previousFlights) {
        if (const auto window = flight.preview.window.lock())
            addWorkspace(window->m_workspace);
    }
    const auto miniature = [&](const PHLWORKSPACE& workspace, const CBox& natural, const std::vector<Card>& cards,
                               const stage::Geometry& geometry, double scroll, bool right, const PHLWINDOW& window, bool cached) -> std::optional<CBox> {
        const auto it = std::ranges::find_if(cards, [&](const Card& card) { return card.workspace == workspace; });
        if (it == cards.end())
            return std::nullopt;
        const double scale = geometry.cardWidth / geometry.desktopWidth;
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - it->shiftStart).count();
        const double shift = it->shift * (1 - stage::transitionProgress(elapsed, duration));
        const auto band = stage::sidebarArea({screen.base.x, screen.base.y, screen.base.w, screen.base.h}, geometry, right);
        const auto area = stage::desktopArea({screen.base.x, screen.base.y, screen.base.w, screen.base.h}, geometry, right);
        if (cached) {
            const auto preview = std::ranges::find_if(it->previews, [&](const Preview& preview) { return preview.window == window; });
            if (preview != it->previews.end())
                return preview->target.copy().translate(Vector2D{band.x + geometry.padding,
                    screen.base.y + geometry.cardTop(std::distance(cards.begin(), it), scroll) + shift});
        }
        return CBox{band.x + geometry.padding + (natural.x - area.x) * scale,
                    screen.base.y + geometry.cardTop(std::distance(cards.begin(), it), scroll) + shift + (natural.y - screen.base.y) * scale,
                    natural.w * scale, natural.h * scale};
    };
    const auto radius = [&](const CBox& box) {
        return stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1, -1, 4096), numberSetting("decoration:rounding", 0), box.w, box.h);
    };
    for (const auto& workspace : workspaces) {
        Screen capture;
        capture.monitor = monitor;
        capture.base = screen.base;
        capture.geometry = screen.geometry;
        capture.right = screen.right;
        // Collect window geometry for the flight. Content stays live throughout.
        capture.geometry.cardWidth = screen.geometry.desktopWidth;
        capture.geometry.cardHeight = screen.base.h;
        Card card;
        card.workspace = workspace;
        if (!updatePreviews(capture, card))
            continue; // Never hide a native window without a usable preview.
        for (auto& preview : card.previews) {
            const auto window = preview.window.lock();
            if (!window)
                continue;
            const auto old = std::ranges::find_if(previousFlights, [&](const Flight& flight) { return flight.preview.window == window; });
            const auto origin = std::ranges::find_if(origins, [&](const auto& entry) { return entry.first == window; });
            const auto originalBox = origin == origins.end() ? preview.natural : origin->second;
            const auto from = old != previousFlights.end() ? std::optional<CBox>{box(flightFrame(*old, oldProgress, monitor).first)} :
                workspaceKey(workspace) == previous ? std::optional<CBox>{originalBox} : miniature(workspace, originalBox, oldCards, oldGeometry, oldScroll, oldRight, window, true);
            const auto goal = setting("stage_window_decorations", 0) ? preview.natural : CBox{window->positionAnimation()->goal(), window->sizeAnimation()->goal()};
            const auto to = workspace == activeWorkspace ? std::optional<CBox>{preview.natural} :
                miniature(workspace, goal, screen.cards, screen.geometry, screen.scroll, screen.right, window, false);
            if (!from || !to)
                continue;
            const auto nativeRadius = window->presentation().rounding();
            const double fromRadius = old != previousFlights.end() ? old->fromRadius + (old->toRadius - old->fromRadius) * stage::transitionProgress(oldProgress, 1) :
                workspaceKey(workspace) == previous ? nativeRadius : radius(*from);
            const double toRadius = workspace == activeWorkspace ? nativeRadius : radius(*to);
            screen.flights.push_back({std::move(preview), *from, *to, static_cast<float>(fromRadius), static_cast<float>(toRadius)});
            auto& flight = screen.flights.back();
            flight.nativeDestination = window->isFloating() && workspace == activeWorkspace;
            if (flight.nativeDestination && old == previousFlights.end()) {
                // Preserve the sidebar's bounded starting frame, but never fit
                // the real desktop destination. Interrupted flights are already sampled.
                flight.from = box(stage::transitionBoxWithin(rect(flight.from), rect(flight.from), 0, flightBounds(monitor)));
            }
            if (scrollingTiled(window)) {
                const auto miniatureView = [&](const std::vector<Card>& cards, const stage::Geometry& geometry, double scroll, bool right) -> std::optional<stage::EdgeViewport> {
                    const auto area = stage::desktopArea(rect(screen.base), geometry, right);
                    const auto view = miniature(workspace, box(area), cards, geometry, scroll, right, window, false);
                    return view ? std::optional{cardViewport(*view)} : std::nullopt;
                };
                if (old != previousFlights.end())
                    flight.fromViewport = flightFrame(*old, oldProgress, monitor).second;
                if (!flight.fromViewport) {
                    if (workspaceKey(workspace) == previous) {
                        const auto area = stage::desktopArea(rect(screen.base), oldGeometry, oldRight);
                        const double width = blurFramebufferHook ? setting("stage_scrolling_desktop_edge_width", 32) * visibility : 0;
                        flight.fromViewport = stage::edgeViewport(area, oldRight ? 0 : width, oldRight ? width : 0);
                    } else
                        flight.fromViewport = miniatureView(oldCards, oldGeometry, oldScroll, oldRight);
                }
                flight.toViewport = workspace == activeWorkspace ? std::optional{desktopViewport(screen)} :
                    miniatureView(screen.cards, screen.geometry, screen.scroll, screen.right);
            }
        }
    }
    if (screen.flights.empty())
        return;
    // Stable partition retains each workspace's stacking order while keeping
    // the new desktop above every departing workspace, including on retarget.
    std::stable_partition(screen.flights.begin(), screen.flights.end(), [&](const Flight& flight) {
        const auto window = flight.preview.window.lock();
        return !window || window->m_workspace != activeWorkspace;
    });
    // Our flight owns the visual switch. Stop the native workspace slide/fade
    // so it cannot resume behind the live preview or flash at the handoff.
    if (stopNative)
        for (const auto& workspace : workspaces) {
            workspace->m_renderOffset->setValueAndWarp(Vector2D{});
            workspace->m_alpha->setValueAndWarp(workspace == activeWorkspace ? 1.F : 0.F);
        }
    screen.flightStart = Clock::now();
    screen.flightDuration = duration;
    damage(screen);
    armMotion();
}

stage::EdgeViewport StageController::Impl::desktopViewport(const Screen& screen) const {
    const double width = blurFramebufferHook ? setting("stage_scrolling_desktop_edge_width", 32) * visibility : 0;
    return stage::edgeViewport(rect(desktop(screen)), screen.right ? 0 : width, screen.right ? width : 0);
}

stage::EdgeViewport StageController::Impl::cardViewport(const CBox& card) const {
    const double width = blurFramebufferHook ? setting("stage_scrolling_preview_edge_width", 16) : 0;
    return stage::edgeViewport(rect(card), width, width, width, width);
}

std::pair<Rect, std::optional<stage::EdgeViewport>> StageController::Impl::flightFrame(const Flight& flight, double progress, const PHLMONITOR& monitor) const {
    if (flight.fromViewport && flight.toViewport) {
        const auto frame = flight.edgeTransfer ?
            stage::edgeTransferFrame(rect(flight.from), rect(flight.to), *flight.fromViewport, *flight.toViewport, progress) :
            stage::scrollingFlightFrame(rect(flight.from), rect(flight.to), *flight.fromViewport, *flight.toViewport, progress);
        return {frame.window, frame.viewport};
    }
    return {flight.offscreen || flight.desktopDecorations || flight.nativeDestination ? stage::transitionBox(rect(flight.from), rect(flight.to), progress) :
        stage::transitionBoxWithin(rect(flight.from), rect(flight.to), progress, flightBounds(monitor)), std::nullopt};
}

void StageController::Impl::retargetFlight(Flight& flight, double progress, const PHLMONITOR& monitor) {
    const auto [window, viewport] = flightFrame(flight, progress, monitor);
    flight.from = box(window);
    flight.fromViewport = viewport;
    flight.fromRadius += (flight.toRadius - flight.fromRadius) * stage::transitionProgress(progress, 1);
}

void StageController::Impl::captureWindowPasses(Render::IHyprRenderer* renderer, const PHLMONITOR& monitor,
                                               const stage::EdgeViewport& viewport, const std::function<void()>& render, std::optional<Rect> overviewClip, bool separateFadeouts) {
    // Layout work-area bounds place windows, but must not trim their vertical
    // decorations. Keep the scrolling tape's horizontal clip; use the output
    // height for native desktop drawing only, not card/flight coordinate maps.
    const auto shadowViewport = overviewClip ? viewport : stage::desktopShadowViewport(viewport, rect(CBox{monitor->m_position, monitor->m_size}));
    const auto clip = overviewClip.value_or(shadowViewport.box);
    StageWindowPassElement::Elements elements;
    auto& passes = renderer->currentPass(renderContext()).m_passElements;
    const auto first = passes.size();
    capturingWindowPasses = true;
    {
        Hyprutils::Utils::CScopeGuard reset{[&] { capturingWindowPasses = false; }};
        render();
    }
    // Keep earlier desktop passes in place and preserve the native ordering of
    // every surface/decoration appended by this render, including direct adds.
    elements.reserve(passes.size() - first);
    for (size_t i = first; i < passes.size(); ++i)
        elements.push_back(std::move(passes[i].element));
    passes.resize(first);
    if (elements.empty())
        return;
    const PHLMONITORREF ref = monitor;
    const auto enqueue = [&](StageWindowPassElement::Elements batch, const stage::EdgeViewport& edge) {
        if (batch.empty())
            return;
        renderer->addPassElement(renderContext(), makeUnique<StageWindowPassElement>(std::move(batch), [this, ref, edge, clip](auto& passes) {
            if (const auto mon = ref.lock())
                drawWindowPasses(passes, mon, edge, clip);
            else
                for (auto& pass : passes) pass->discard(renderContext());
        }, box(clip).translate(-monitor->m_position)));
    };
    if (overviewClip) {
        // Overview's outer shadow is a separate decoration, not window content.
        // Keep native pass order, but do not put it inside the card's hard crop
        // or edge-gradient framebuffer. Its pass owns its output clipping.
        StageWindowPassElement::Elements batch;
        for (auto& element : elements) {
            if (element->passName() && std::string_view{element->passName()} == "OverviewShadowPassElement") {
                enqueue(std::move(batch), shadowViewport);
                batch.clear();
                renderer->addPassElement(renderContext(), std::move(element));
            } else
                batch.push_back(std::move(element));
        }
        enqueue(std::move(batch), shadowViewport);
        return;
    }
    if (!separateFadeouts) {
        enqueue(std::move(elements), shadowViewport);
        return;
    }
    // Native snapshots cover the whole output, even for a small window. Match
    // the texture to its fadeout and classify the actual animated window body,
    // not that framebuffer-sized render box. Keep each window's pre-blur/dim
    // passes together and preserve native order, alpha and animation geometry.
    StageWindowPassElement::Elements batch;
    for (auto& element : elements) {
        const auto* texture = dynamic_cast<CTexPassElement*>(element.get());
        std::optional<stage::EdgeViewport> edge;
        if (texture) {
            for (const auto& fadeout : Desktop::fadingOutState()->fadeouts()) {
                if (!fadeout)
                    continue;
                const auto fb = fadeout->framebuffer();
                if (fadeout->monitor() != monitor || fadeout->plane() != Desktop::FADEOUT_PLANE_WINDOW_TILED ||
                    !fb || fb->getTexture() != texture->m_data.tex)
                    continue;
                edge = stage::edgeViewportForWindow(shadowViewport, rect(fadeout->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT)));
                break;
            }
        }
        batch.push_back(std::move(element));
        if (edge) {
            enqueue(std::move(batch), *edge);
            batch.clear();
        }
    }
    // Unknown plugin passes have no trustworthy window geometry. Retain the
    // hard clip without inventing a blur classification for them.
    enqueue(std::move(batch), stage::edgeViewport(shadowViewport.box, 0, 0));
}

void StageController::Impl::drawWindowPasses(StageWindowPassElement::Elements& elements, const PHLMONITOR& monitor, const stage::EdgeViewport& viewport,
                                             const Rect& clipGlobal) {
    auto& state = renderContext().m_data;
    const auto previousModif = state.renderModif;
    Hyprutils::Utils::CScopeGuard restore{[&] { state.renderModif = previousModif; }};
    const CBox clip = box(clipGlobal).translate(-monitor->m_position).scale(monitor->m_scale);
    const bool drawn = edgeEffect.draw(monitor, viewport, clip, [&](const CBox& limit) {
        state.renderModif = previousModif;
        const auto damage = state.damage;
        for (auto& element : elements) {
            state.damage = damage;
            state.clipBox = limit;
            CBox* ownClip = nullptr;
            bool* optimize = nullptr;
            if (auto* surface = dynamic_cast<CSurfacePassElement*>(element.get())) {
                ownClip = &surface->m_data.clipBox;
                optimize = &surface->m_data.blockBlurOptimization;
            } else if (auto* texture = dynamic_cast<CTexPassElement*>(element.get()))
                ownClip = &texture->m_data.clipBox;
            else if (auto* rectangle = dynamic_cast<CRectPassElement*>(element.get()))
                ownClip = &rectangle->m_data.clipBox;
            const auto savedClip = ownClip ? *ownClip : CBox{};
            const bool savedOptimize = optimize && *optimize;
            Hyprutils::Utils::CScopeGuard restoreElement{[&] {
                if (ownClip) *ownClip = savedClip;
                if (optimize) *optimize = savedOptimize;
            }};
            if (ownClip) {
                *ownClip = savedClip.empty() ? limit : savedClip.intersection(limit);
                if (const auto* rectangle = dynamic_cast<CRectPassElement*>(element.get()); rectangle && rectangle->m_data.blur) {
                    // Native rect blur samples a full-output texture. Its texture
                    // draw uses clipBox instead of the box-limited damage, so a
                    // Stage-wide clip would paint blur outside a closing window
                    // (including stale pixels outside the blur's updated area).
                    auto blurBox = rectangle->m_data.box;
                    state.renderModif.applyToBox(blurBox);
                    *ownClip = ownClip->intersection(blurBox);
                }
                if (ownClip->empty()) {
                    element->discard(renderContext());
                    continue; // An empty native clip means UNCLIPPED, not invisible.
                }
            }
            if (optimize)
                *optimize = true;
            g_pHyprRenderer->draw(renderContext(), element, damage);
        }
    });
    if (auto* tiles=TilesController::get(); tiles && tiles->enabled(monitor)) {
        tiles->edgeError(edgeEffect.error());
        if (drawn && edgeEffect.error().empty() && (viewport.left>0 || viewport.right>0 || viewport.top>0 || viewport.bottom>0)) tiles->edgeActive();
    }
    if (!drawn)
        for (auto& element : elements)
            element->discard(renderContext());
}

void StageController::Impl::drawPreview(const PHLWINDOW& window, const PHLMONITOR& monitor, const CBox& target, const CBox& clip, double radius,
                                       std::optional<stage::EdgeViewport> viewport, double* captureRounding, bool forceDecorations) {
    if (!hooksReady || !renderWindowHook || !renderWindowHook->enabled || !renderWindowHook->m_original || !renderWindow || !window)
        return;
    // Cached cards/flights can outlive a fullscreen change. Do not resurrect a
    // covered window (or its blur) before the next preview collection.
    if (previewLayer(window) == stage::PreviewLayer::Hidden)
        return;
    if (viewport) {
        edgeEffect.draw(monitor, stage::edgeViewportForWindow(*viewport, rect(target)), clip, [&](const CBox& limit) { drawPreview(window, monitor, target, limit, radius, std::nullopt, captureRounding, forceDecorations); });
        return;
    }
    const auto root = window->wlSurface()->resource();
    if (!root || target.w <= 0 || target.h <= 0)
        return;
    const CBox source{window->positionAnimation()->value(), window->sizeAnimation()->value()};
    if (source.w <= 0 || source.h <= 0)
        return;
    const bool includeDecorationBounds = setting("stage_window_decorations", 0) != 0;
    const bool decorations = includeDecorationBounds || forceDecorations;
    // Enabling desktop decorations must not rescale the already sampled content.
    const CBox footprint = includeDecorationBounds ? window->getFullWindowBoundingBox() : source;
    if (footprint.w <= 0 || footprint.h <= 0)
        return;
    const double scale = std::min(target.w / footprint.w, target.h / footprint.h);
    const CBox fitted{target.middle() - footprint.size() * scale / 2, footprint.size() * scale};
    radius = std::clamp(radius, 0.0, std::min(source.w, source.h) * scale / 2);
    auto& state = renderContext().m_data;
    const auto previousClip = state.clipBox;
    const auto previousWindow = state.currentWindow;
    const auto previousSurface = state.surface;
    const auto previousNearest = state.useNearestNeighbor;
    const auto previousUVTL = state.primarySurfaceUVTopLeft;
    const auto previousUVBR = state.primarySurfaceUVBottomRight;
    const auto previousTransform = surfaceTransform;
    const auto previousRendering = rendering;
    const auto previousModif = state.renderModif;
    const auto previousDecoration = drawingDecoration;
    const auto previousRadius = decorationRadius;
    const auto previousDecorationClip = decorationClip;
    Hyprutils::Utils::CScopeGuard restore{[&] {
        surfaceTransform = previousTransform;
        rendering = previousRendering;
        state.clipBox = previousClip;
        state.currentWindow = previousWindow;
        state.surface = previousSurface;
        state.useNearestNeighbor = previousNearest;
        state.primarySurfaceUVTopLeft = previousUVTL;
        state.primarySurfaceUVBottomRight = previousUVBR;
        state.renderModif = previousModif;
        drawingDecoration = previousDecoration;
        decorationRadius = previousRadius;
        decorationClip = previousDecorationClip;
    }};
    surfaceTransform = SurfaceTransform{footprint.copy().translate(-monitor->m_position), fitted.copy().translate(-monitor->m_position), scale};
    rendering = true;
    state.renderModif = {};
    const auto drawDecorations = [&](bool below) {
        if (!decorations)
            return;
        Render::SRenderModifData transform;
        const auto offset = window->m_workspace ? window->m_workspace->m_renderOffset->value() : Vector2D{};
        transform.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE,
            (monitor->m_position - footprint.pos() - offset - window->presentation().floatingOffset()) * monitor->m_scale);
        transform.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, static_cast<float>(scale));
        transform.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, (fitted.pos() - monitor->m_position) * monitor->m_scale);
        state.renderModif = transform;
        state.currentWindow = window;
        drawingDecoration = true;
        decorationRadius = radius / scale;
        decorationClip = clip;
        for (const auto& decoration : window->presentation().decorations()) {
            const auto layer = decoration->getDecorationLayer();
            if (below == (layer == DECORATION_LAYER_BOTTOM || layer == DECORATION_LAYER_UNDER))
                decoration->draw(renderContext(), monitor, 1.F, window->presentation().renderPresentation());
        }
        drawingDecoration = false;
        state.renderModif = {};
    };
    drawDecorations(true);
    CSurfacePassElement::SRenderData data;
    data.pMonitor = monitor;
    data.pWindow = window;
    data.pos = source.pos();
    data.w = source.w;
    data.h = source.h;
    data.clipBox = clip;
    data.rounding = static_cast<int>(std::lround(radius * monitor->m_scale)) + 1;
    data.dontRound = radius <= 0;
    // Export the final surface radius after fitting, clamping and physical
    // pixel quantization, in the logical coordinates used by capture clients.
    if (captureRounding)
        *captureRounding = data.dontRound ? 0.0 : data.rounding / monitor->m_scale;
    data.roundingPower = window->presentation().roundingPower();
    data.blur = shouldBlur && shouldBlur(window.get(), window->presentation().renderPresentation());
    data.blockBlurOptimization = true;
    // Use the compositor's native surface path, including viewport/UV handling,
    // subsurfaces, buffer synchronization, and presentation/frame feedback.
    // No workspace visibility, animation, framebuffer or projection is changed.
    root->breadthfirst([&](SP<CWLSurfaceResource> surface, const Vector2D& offset, void*) {
        if (!surface->m_current.texture || surface->m_current.size.x < 1 || surface->m_current.size.y < 1)
            return;
        data.surface = surface;
        data.texture = surface->m_current.texture;
        data.localPos = offset;
        data.mainSurface = surface == root;
        g_pHyprRenderer->draw(renderContext(), data, state.damage);
        ++data.surfaceCounter;
    }, nullptr);
    drawDecorations(false);
}

void StageController::Impl::drawFlights(Screen& screen, const PHLMONITOR& monitor) {
    if (screen.flights.empty() || screen.covered || screen.suspended)
        return;
    const auto previousClip = renderContext().m_data.clipBox;
    const double p = flightProgress(screen);
    const auto bounds = screen.tile ? rect(screen.base) : flightBounds(monitor);
    const CBox clip{(bounds.x - monitor->m_position.x) * monitor->m_scale,
                    (bounds.y - monitor->m_position.y) * monitor->m_scale,
                    bounds.width * monitor->m_scale, bounds.height * monitor->m_scale};
    for (auto& flight : screen.flights) {
        const auto window = flight.preview.window.lock();
        if (!window || !window->mapped() || window->isHidden() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) || window->m_monitor != monitor)
            continue;
        if ((!swipe || &swipe->visual != &screen) && window->m_workspace == screenWorkspace(screen)) {
            flight.to = setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() : CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
            if (flight.toViewport)
                flight.toViewport = desktopViewport(screen);
        } else if (flight.dropToCard) {
            const auto card = std::ranges::find_if(screen.cards, [&](const auto& c) { return c.workspace == window->m_workspace; });
            if (card != screen.cards.end()) {
                const auto preview = std::ranges::find_if(card->previews, [&](const auto& p) { return p.window == window; });
                if (preview != card->previews.end()) {
                    flight.to = preview->target.copy().translate(Vector2D{sidebar(screen).x + screen.geometry.padding,
                        screen.base.y + cardTop(screen, std::distance(screen.cards.begin(), card))});
                    if (flight.toViewport)
                        flight.toViewport = cardViewport(CBox{sidebar(screen).x + screen.geometry.padding,
                            screen.base.y + cardTop(screen, std::distance(screen.cards.begin(), card)), screen.geometry.cardWidth, screen.geometry.cardHeight});
                }
            }
        }
        const auto [frame, viewport] = flightFrame(flight, p, monitor);
        if (flight.edgeTransfer && viewport && !stage::overviewEndpointVisible({frame, *viewport, viewport->box}))
            continue; // Do not leave a shadow visible during the hidden size change.
        const CBox drawClip = !screen.tile && flight.desktopDecorations ? CBox{0, 0, monitor->m_size.x * monitor->m_scale, monitor->m_size.y * monitor->m_scale} : clip;
        const double radius = flight.desktopDecorations ? window->presentation().rounding() :
            flight.fromRadius + (flight.toRadius - flight.fromRadius) * stage::transitionProgress(p, 1);
        drawPreview(window, monitor, box(frame), drawClip, radius, viewport, nullptr, flight.desktopDecorations);
    }
    renderContext().m_data.clipBox = previousClip;
}

void StageController::Impl::ensureBackground(Screen& screen, const PHLMONITOR& monitor) {
    if (!g_pHyprRenderer || !g_pHyprOpenGL || !monitor)
        return;
    if (setting("stage_backdrop", 0) == 0) {
        screen.background.reset();
        screen.backgroundError.clear();
        return;
    }
    if (renderContext().m_data.pMonitor)
        return; // never render offscreen inside a live pass
    const auto size = monitor->m_transformedSize;
    // Re-render periodically so live background-layer content (dashboards)
    // stays honest in the previews; size changes force it immediately.
    if (screen.background && screen.background->isAllocated() && screen.background->m_size == size &&
        Clock::now() - screen.backgroundRendered < std::chrono::seconds(2))
        return;
    auto framebuffer = g_pHyprRenderer->createFB("hymission stage background");
    if (!framebuffer || !framebuffer->alloc(std::max(1, static_cast<int>(std::lround(size.x))), std::max(1, static_cast<int>(std::lround(size.y))))) {
        screen.backgroundError = "framebuffer alloc failed";
        return;
    }
    framebuffer->setImageDescription(monitor->workBufferImageDescription());
    g_pHyprOpenGL->makeEGLCurrent();
    const auto previousBlockFeedback = renderContext().m_blockSurfaceFeedback;
    renderContext().m_blockSurfaceFeedback = true;
    const auto failure = OverviewController::renderBackgroundIntoFramebuffer(monitor, framebuffer);
    renderContext().m_blockSurfaceFeedback = previousBlockFeedback;
    if (!failure.empty()) {
        screen.backgroundError = failure;
        return;
    }
    screen.background = std::move(framebuffer);
    screen.backgroundRendered = Clock::now();
    screen.backgroundError.clear();
}

void StageController::Impl::refreshPreviews() {
    if (!enabled || blocked() || rendering || renderContext().m_data.pMonitor)
        return;
    for (auto& screen : screens) {
        if (screen.covered || screen.suspended || (visibility <= 0 && !visibilityGesture && !runtimeVisible.value_or(false)))
            continue;
        ensureBackground(screen, screen.monitor.lock());
        bool changed = false;
        for (std::size_t i = 0; i < screen.cards.size(); ++i) {
            auto& card = screen.cards[i];
            const auto top = cardTop(screen, i);
            if (!card.dirty || top + screen.geometry.cardHeight <= screen.geometry.paddingTop || top >= screen.base.h - screen.geometry.paddingBottom)
                continue;
            if (updatePreviews(screen, card)) {
                card.dirty = false;
                changed = true;
            }
        }
        if (changed) {
            if (!screen.flights.empty() || screen.paneTransition || (swipe && swipe->prepared && swipe->monitor == screen.monitor))
                damage(screen);
            else
                g_pHyprRenderer->damageBox(sidebar(screen));
        }
    }
}

void StageController::Impl::updatePreviewLiveness() {
    std::vector<PHLWINDOWREF> visible;
    const auto add = [&](const PHLWINDOWREF& ref) {
        const auto window = ref.lock();
        if (!window || !window->mapped() || window->isHidden() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED))
            return;
        if (std::ranges::find(visible, ref) == visible.end()) {
            visible.push_back(ref);
            window->setSuspended(false);
        }
    };
    if (enabled && !blocked()) {
        if (swipe && swipe->prepared)
            for (const auto& flight : swipe->visual.flights)
                add(flight.preview.window);
        for (const auto& screen : screens) {
            if (screen.covered || screen.suspended || (visibility <= 0 && !visibilityGesture && !runtimeVisible.value_or(false)))
                continue;
            for (std::size_t i = 0; i < screen.cards.size(); ++i) {
                const auto top = cardTop(screen, i);
                if (top + screen.geometry.cardHeight <= screen.geometry.paddingTop || top >= screen.base.h - screen.geometry.paddingBottom)
                    continue;
                for (const auto& preview : screen.cards[i].previews)
                    add(preview.window);
            }
            for (const auto& flight : screen.flights)
                add(flight.preview.window);
            if (screen.paneTransition)
                for (const auto& card : screen.departingCards)
                    for (const auto& preview : card.previews)
                        add(preview.window);
        }
    }
    for (const auto& ref : livePreviewWindows) {
        if (std::ranges::find(visible, ref) != visible.end())
            continue;
        if (const auto window = ref.lock())
            window->setSuspended(window->isHidden() || !window->m_workspace || !window->m_workspace->visible());
    }
    livePreviewWindows = std::move(visible);
}

bool StageController::Impl::updatePreviews(Screen& screen, Card& card) {
    return updatePreviews(screen, card, {});
}

bool StageController::Impl::updatePreviews(Screen& screen, Card& card, const Vector2D& tiledOffset) {
    card.previewsReady = false;
    const auto monitor = screen.monitor.lock();
    const auto workspace = card.workspace.lock();
    if (!monitor || !workspace || !g_pHyprOpenGL || !renderWindow)
        return false;
    const bool decorations = setting("stage_window_decorations", 0) != 0;
    std::vector<PHLWINDOW> windows;
    std::vector<CBox> boxes;
    std::vector<WindowInput> inputs;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->mapped() || window->isHidden() || window->onSpecialWorkspace() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED))
            continue;
        if (window->m_workspace == workspace && previewLayer(window) != stage::PreviewLayer::Hidden)
            windows.push_back(window);
    }
    // Hyprland renders tiled windows before floating windows regardless of
    // their position in the global list. Preserve order inside each layer.
    std::stable_sort(windows.begin(), windows.end(), [](const auto& a, const auto& b) { return previewLayer(a) < previewLayer(b); });
    boxes.resize(windows.size());
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];
        const bool tileLayout=screen.tile && !window->isFloating();
        const auto box = tileLayout ? CBox{window->positionAnimation()->goal(),window->sizeAnimation()->goal()} :
            decorations ? window->getFullWindowBoundingBox() : CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
        if (box.w <= 0 || box.h <= 0)
            continue;
        // A side change moves native tiled geometry. Hidden cards should
        // depict its destination, not bake the intermediate layout slide
        // into a thumbnail that persists until the next refresh.
        const auto offset = (smartisan && !tileLayout ? window->positionAnimation()->goal() - window->positionAnimation()->value() : Vector2D{}) +
            (window->isFloating() ? Vector2D{} : tiledOffset);
        inputs.push_back(WindowInput{.index = i, .natural = Rect{box.x + offset.x, box.y + offset.y, box.w, box.h}});
        boxes[i] = box;
    }
    const auto area = desktop(screen);
    const auto slots = stage::arrangeWindows(inputs, screen.geometry, Rect{area.x, area.y, area.w, area.h});
    std::vector<Preview> previews;
    for (const auto& slot : slots)
        previews.push_back(Preview{windows[slot.index], CBox{slot.target.x, slot.target.y, slot.target.width, slot.target.height}, boxes[slot.index]});
    card.previews = std::move(previews);
    card.previewsReady = true;
    return true;
}

StageController::Impl::~Impl() {
    shuttingDown = true;
    enabled = false;
    clearSwipe();
    updatePreviewLiveness();
    deferred.reset();
    canvasRelease.reset();
    stopTimer(refreshTimer);
    stopTimer(motionTimer);
    listeners.clear();
    edgeEffect.reset();
    releaseHooks();
    if (g_layoutManager) {
        for (const auto& screen : screens) {
            if (const auto monitor = screen.monitor.lock()) {
                g_layoutManager->invalidateMonitorGeometries(monitor);
                damage(screen);
            }
        }
    }
    screens.clear();
    instance = nullptr;
}

std::string StageController::Impl::stateJson() const {
    nlohmann::json result{{"visible", runtimeVisible.value_or(setting("stage_enabled", 0) != 0)}, {"visibility_progress", visibility}, {"enabled", enabled}, {"smartisan_mode", smartisan}, {"hooks_ready", hooksReady}, {"preview_renderer", "native_surfaces"},
        {"refresh_interval_ms", std::clamp(setting("stage_refresh_ms", 16), 1L, 16L)}, {"error", error},
        {"swipe_begin_count", swipeBegins}, {"swipe_update_count", swipeUpdates}, {"swipe_end_count", swipeEnds},
        {"raw_swipe_update_count", rawSwipeUpdates},
        {"scrolling_edge_blur_hook", blurFramebufferHook != nullptr}, {"scrolling_edge_error", edgeEffect.error()},
        {"drag_hover_active", dragHover.has_value()},
        {"thumbnail_drag_active", !!thumbnailDrag},
        {"last_drop_trace", dropTrace},
        {"captureVersion", 1}, {"captureWindows", nlohmann::json::array()},
        {"screens", nlohmann::json::array()}};
    for (const auto& screen : screens) {
        const auto monitor = screen.monitor.lock();
        if (!monitor)
            continue;
        const auto& rendered = swipe && swipe->prepared && swipe->monitor == monitor ? swipe->visual : screen;
        if (enabled && !blocked() && !renderBlocked(monitor) && !rendered.covered && !rendered.suspended &&
            !monitor->m_activeSpecialWorkspace && rendered.shown > 0) {
            const auto geometryJson = [](const CBox& r) {
                return nlohmann::json{{"x", r.x}, {"y", r.y}, {"width", r.w}, {"height", r.h}};
            };
            for (const auto& preview : rendered.capturePreviews) {
                const auto window = preview.window.lock();
                if (!window || !window->mapped() || window->isHidden() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) ||
                    previewLayer(window) == stage::PreviewLayer::Hidden)
                    continue;
                result["captureWindows"].push_back({{"address", std::format("0x{:x}", reinterpret_cast<uintptr_t>(window.get()))},
                    {"selectionGeometry", geometryJson(preview.selection)}, {"selectionClipGeometry", geometryJson(preview.clip)}, {"selectionRounding", preview.rounding}});
            }
        }
        nlohmann::json cards = nlohmann::json::array();
        for (const auto& card : screen.cards) {
            if (const auto workspace = card.workspace.lock())
            {
                const size_t index=&card-screen.cards.data();
                const auto band=sidebar(screen);
                const CBox frame{band.x+screen.geometry.padding,screen.base.y+cardTop(screen,index),screen.geometry.cardWidth,screen.geometry.cardHeight};
                nlohmann::json previews=nlohmann::json::array();
                for (const auto& preview:card.previews) if (auto window=preview.window.lock()) {
                    const auto b=preview.target.copy().translate(frame.pos());
                    previews.push_back({{"address",std::format("0x{:x}",reinterpret_cast<uintptr_t>(window.get()))},{"x",b.x},{"y",b.y},{"width",b.w},{"height",b.h}});
                }
                cards.push_back({{"workspace",workspaceKey(workspace)},{"name",workspace->displayName()},{"previews_ready",card.previewsReady},{"shift",card.shift},
                    {"x",frame.x},{"y",frame.y},{"width",frame.w},{"height",frame.h},{"windows",previews}});
            }
            else if (card.syntheticId > 0)
                cards.push_back({{"workspace", card.syntheticId}, {"name", "empty:" + std::to_string(card.syntheticId)}, {"synthetic", true},
                    {"previews_ready", true}});
        }
        const auto prepared = std::ranges::find_if(overviewExitScreens, [&](const auto& candidate) { return candidate.monitor == monitor; });
        const auto owner = stage::overviewRenderOwner(overviewPhaseFor(monitor), prepared != overviewExitScreens.end());
        nlohmann::json preparedCards = nlohmann::json::array();
        if (prepared != overviewExitScreens.end())
            for (const auto& card : prepared->cards)
                preparedCards.push_back(cardID(card));
        result["screens"].push_back({{"monitor", monitor->m_name},{"tile_id",screen.tile},{"active_workspace",screen.active},{"base",{{"x",screen.base.x},{"y",screen.base.y},{"width",screen.base.w},{"height",screen.base.h}}}, {"card_width", screen.geometry.cardWidth}, {"card_height", screen.geometry.cardHeight},
                                     {"overview_render_owner", owner == stage::OverviewRenderOwner::Overview ? "overview" :
                                         owner == stage::OverviewRenderOwner::PreparedStage ? "prepared_stage" : "stage"},
                                     {"prepared_active_workspace", prepared != overviewExitScreens.end() ? prepared->active : WORKSPACE_INVALID},
                                     {"prepared_cards", preparedCards},
                                     {"preview_render_fps", Clock::now() - rendered.lastFrame < std::chrono::seconds(1) ? rendered.previewRenderFPS : 0},
                                     {"background_ready", !!(screen.background && screen.background->isAllocated())},
                                     {"background_error", screen.backgroundError},
                                     {"sidebar_side", screen.right ? "right" : "left"}, {"sidebar_transition", screen.paneTransition},
                                     {"swipe_active", swipe && swipe->monitor == monitor},
                                     {"swipe_progress", swipe && swipe->monitor == monitor ? swipe->progress : 0},
                                     {"transition_windows", rendered.flights.size()}, {"transition_progress", rendered.flights.empty() ? 1.0 : flightProgress(rendered)},
                                     {"desktop_width", screen.geometry.desktopWidth}, {"desktop_height", screen.base.h}, {"reservation", screen.geometry.reservation},
                                     {"scroll", screen.scroll}, {"max_scroll", screen.geometry.maxScroll}, {"covered", screen.covered}, {"suspended", screen.suspended},
                                     {"cards", cards}});
    }
    return dumpStateJson(result) + "\n";
}

StageController::StageController(HANDLE handle, std::function<bool()> suspended,
                                 std::function<stage::OverviewPhase(const PHLMONITOR&)> phase,
                                 std::function<std::optional<stage::EdgeFrame>(const PHLWINDOW&, const PHLMONITOR&)> frame, std::function<std::optional<double>(const PHLMONITOR&)> slide, std::function<bool()> suppressInput) :
    m_impl(std::make_unique<Impl>(handle, std::move(suspended), std::move(phase), std::move(frame), std::move(slide), std::move(suppressInput))) {}
StageController::~StageController() = default;
void StageController::Impl::applyVisibility(double progress) {
    visibility = std::clamp(progress, 0.0, 1.0);
    for (auto& screen : screens) {
        screen.flights.clear();
        screen.paneTransition = false;
        screen.departingCards.clear();
        if (auto monitor = screen.monitor.lock()) {
            g_layoutManager->invalidateMonitorGeometries(monitor);
            // Finger progress already supplies the animation; native window
            // animations must not lag behind each new work-area endpoint.
            for (const auto& window : Desktop::windowState()->windows()) {
                if (window->m_monitor == monitor && window->mapped() && !window->isFloating() && !static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED)) {
                    window->positionAnimation()->warp();
                    window->sizeAnimation()->warp();
                }
            }
            damage(screen);
        }
    }
    request();
}

void StageController::Impl::settleVisibility(bool visible) {
    runtimeVisible = visible;
    visibilityGesture = false;
    visibilityFrom = visibility;
    visibilityTarget = visible ? 1 : 0;
    visibilityStart = Clock::now();
    armMotion();
}

bool StageController::toggleVisibility() {
    return setVisibility(!m_impl->runtimeVisible.value_or(setting("stage_enabled", 0) != 0));
}

bool StageController::setVisibility(bool visible) {
    auto& self = *m_impl;
    if (self.blocked() || self.swipe || self.visibilityGesture)
        return false;
    const bool previous = self.runtimeVisible.value_or(setting("stage_enabled", 0) != 0);
    if (previous == visible && !visible)
        return true;
    self.runtimeVisible = visible;
    self.sync();
    if (!self.enabled)
        return false;
    // Repeated requests must not restart an in-flight visibility animation.
    if (previous != visible)
        self.settleVisibility(visible);
    return true;
}

std::optional<Rect> StageController::holdRegion(const std::string& name) const {
    const auto& self = *m_impl;
    if (!self.enabled || self.blocked() || !self.runtimeVisible.value_or(setting("stage_enabled", 0) != 0))
        return std::nullopt;
    for (const auto& screen : self.screens) {
        auto monitor = screen.monitor.lock();
        if (!monitor || monitor->m_name != name || screen.covered || screen.suspended ||
            self.renderBlocked(monitor) || monitor->m_activeSpecialWorkspace)
            continue;
        // Target geometry remains stable through the zero-progress opening frame.
        const auto region = stage::holdStrip({monitor->m_position.x, monitor->m_position.y,
                                             monitor->m_size.x, monitor->m_size.y},
                                            rect(screen.base), screen.geometry.reservation, screen.right);
        if (region.width <= 0 || region.height <= 0)
            return std::nullopt;
        return region;
    }
    return std::nullopt;
}

bool StageController::beginVisibilityGesture() {
    if (sessionLocked())
        return false;

    auto* self = Impl::instance;
    if (!self || self->blocked() || self->swipe || self->visibilityGesture)
        return false;
    self->runtimeVisible = self->runtimeVisible.value_or(setting("stage_enabled", 0) != 0);
    self->visibilityGesture = true;
    self->sync();
    if (!self->enabled) {
        self->visibilityGesture = false;
        return false;
    }
    self->visibilityFrom = self->visibility;
    self->visibilityDelta = 0;
    return true;
}

void StageController::updateVisibilityGesture(double delta) {
    if (sessionLocked())
        return;

    auto* self = Impl::instance;
    if (!self || !self->visibilityGesture)
        return;
    if (self->blocked()) {
        endVisibilityGesture(true);
        return;
    }
    self->visibilityDelta += delta;
    const double distance = std::max(1.0, numberSetting("gestures:workspace_swipe_distance", 300));
    self->applyVisibility(stage::visibilityGestureProgress(self->visibilityFrom, self->visibilityDelta, distance));
}

void StageController::endVisibilityGesture(bool cancelled) {
    if (sessionLocked())
        return;

    auto* self = Impl::instance;
    if (!self || !self->visibilityGesture)
        return;
    self->settleVisibility(cancelled ? self->runtimeVisible.value_or(false) : self->visibility >= 0.5);
}

void StageController::initialize() { m_impl->initialize(); }
void StageController::handleTilesChange() { m_impl->request(); }
void StageController::renderInViewport(const PHLMONITOR& monitor, const stage::EdgeViewport& viewport, const Rect& clip, const std::function<void()>& render) {
    if (auto* self = Impl::instance)
        self->captureWindowPasses(g_pHyprRenderer.get(), monitor, viewport, render, clip);
    else
        render();
}
bool StageController::ensureEdgeService() { return Impl::instance && Impl::instance->ensureEdgeHook(); }
std::optional<Rect> StageController::nativeDesktop(const PHLWORKSPACE& workspace) {
    auto* self=Impl::instance;
    if (!self || !self->enabled || self->blocked()) return {};
    auto* screen=self->screenForWorkspace(workspace);
    if (!screen || screen->covered || screen->suspended || !screen->geometry.enabled()) return {};
    return rect(self->desktop(*screen));
}
std::optional<stage::EdgeFrame> StageController::overviewPresentation(const PHLWINDOW& window,const PHLMONITOR& monitor) {
    auto* self=Impl::instance;
    return self && self->overviewFrame ? self->overviewFrame(window,monitor):std::nullopt;
}
std::string StageController::stateJson() const { return m_impl->stateJson(); }

std::optional<stage::OverviewEndpoint> StageController::Impl::cardEndpoint(const Screen& screen, const PHLWINDOW& window, bool settled) const {
    const auto monitor = screen.monitor.lock();
    if (!monitor || !window)
        return std::nullopt;
    double offset = 0;
    if (!settled && screen.paneTransition) {
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - screen.paneStart).count();
        const double t = swipe && &swipe->visual == &screen ? swipe->progress :
            screen.paneDuration > 0 ? std::clamp(elapsed / screen.paneDuration, 0.0, 1.0) : 1;
        const double travel = screen.right ? monitor->m_position.x + monitor->m_size.x - (screen.base.x + screen.base.w) + screen.geometry.bandWidth :
            screen.base.x - monitor->m_position.x + screen.geometry.bandWidth;
        offset = (screen.right ? 1 : -1) * travel * (1 - stage::transitionProgress(t, 1));
    } else if (!settled)
        offset = (screen.right ? 1 : -1) * (1 - screen.shown * visibility) * screen.geometry.bandWidth;
    const auto area = sidebar(screen);
    const CBox output=screen.tile?screen.base:CBox{monitor->m_position,monitor->m_size};
    const CBox strip{area.x + offset, area.y + screen.geometry.paddingTop, area.w,
                     area.h - screen.geometry.paddingTop - screen.geometry.paddingBottom};
    for (std::size_t i = 0; i < screen.cards.size(); ++i) {
        const auto& card = screen.cards[i];
        if (card.workspace != window->m_workspace || !card.previewsReady)
            continue;
        const CBox cardBox{area.x + screen.geometry.padding + offset,
            screen.base.y + (settled ? screen.geometry.cardTop(i, screen.scroll) : cardTop(screen, i)),
            screen.geometry.cardWidth, screen.geometry.cardHeight};
        for (const auto& preview : card.previews) {
            if (preview.window != window)
                continue;
            const Rect target = rect(preview.target.copy().translate(cardBox.pos()));
            auto viewport = (window->isFloating() || scrollingTiled(window)) ? stage::edgeViewportForWindow(cardViewport(cardBox), target) :
                stage::edgeViewport(rect(cardBox), 0, 0);
            if (screen.right ? viewport.right > 0 : viewport.left > 0) {
                viewport.fixedOuterEdge = screen.right ? stage::FixedOuterEdge::Right : stage::FixedOuterEdge::Left;
                viewport.fixedOuterX = screen.right ? cardBox.x + cardBox.w : cardBox.x;
            }
            const CBox clip = cardBox.intersection(strip).intersection(output);
            return stage::OverviewEndpoint{target, viewport, previewLayer(window) == stage::PreviewLayer::Hidden ? Rect{} : rect(clip)};
        }
    }
    return std::nullopt;
}

std::optional<stage::OverviewEndpoint> StageController::overviewOrigin(const PHLWINDOW& window) {
    auto* self = Impl::instance;
    if (!self || !self->enabled || self->visibility <= 0 || self->blocked() || !window || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED))
        return std::nullopt;
    const auto monitor = window->m_monitor.lock();
    auto* screen = TilesController::get() && TilesController::get()->available()?self->screenForWorkspace(window->m_workspace):self->visualScreenFor(monitor);
    if (!monitor || !screen || !screen->geometry.enabled() || screen->covered || screen->suspended)
        return std::nullopt;
    const Rect output = rect(screen->tile?screen->base:CBox{monitor->m_position,monitor->m_size});
    if (self->dragHover && self->dragHover->window == window) {
        const auto frame = self->dragHoverFrame();
        return stage::OverviewEndpoint{frame.window, frame.viewport, rect(box(frame.viewport.box).intersection(box(output)))};
    }
    for (const auto& flight : screen->flights) {
        if (flight.preview.window != window)
            continue;
        const auto [frame, viewport] = self->flightFrame(flight, self->flightProgress(*screen), monitor);
        const auto edge = viewport ? stage::edgeViewportForWindow(*viewport, frame) : stage::edgeViewport(output, 0, 0);
        return stage::OverviewEndpoint{frame, edge, rect(box(edge.box).intersection(box(output)))};
    }
    if (window->m_workspace == self->screenWorkspace(*screen)) {
        if (!scrollingTiled(window))
            return std::nullopt;
        const Rect frame = rect(CBox{window->positionAnimation()->value() + window->m_workspace->m_renderOffset->value(), window->sizeAnimation()->value()});
        const auto edge = stage::edgeViewportForWindow(stage::desktopShadowViewport(self->desktopViewport(*screen), output), frame);
        return stage::OverviewEndpoint{frame, edge, rect(box(edge.box).intersection(box(output)))};
    }
    return self->cardEndpoint(*screen, window, false);
}

void StageController::prepareOverviewExit(const std::vector<PHLMONITOR>& monitors, const PHLWORKSPACE& activeWorkspace) {
    auto* self = Impl::instance;
    if (!self)
        return;
    self->overviewExitScreens.clear();
    if (!self->enabled || self->visibility <= 0)
        return;
    for (const auto& scene:self->screens) {
        const auto monitor=scene.monitor.lock();
        if (std::ranges::find(monitors,monitor)==monitors.end()) continue;
        const auto* source=&scene;
        if (!monitor || !source || !source->geometry.enabled())
            continue;
        const auto target = activeWorkspace && activeWorkspace->m_monitor == monitor && (!source->tile || TilesController::get()->preferredTile(activeWorkspace,monitor)==source->tile) ? activeWorkspace : self->screenWorkspace(*source);
        if (!target || (target->type() == Workspace::eWorkspaceType::SPECIAL))
            continue;

        // One settled scene supplies every exit endpoint and the first Stage
        // frame. Never expose the live list while it still describes the old
        // active workspace or has outstanding card-shift animations.
        Impl::Screen destination = *source;
        destination.workspace=target;
        destination.active = workspaceKey(target);
        destination.cards = self->cardsForWorkspace(monitor, target);
        destination.geometry = stage::layout(destination.base.w, destination.base.h, destination.cards.size(),
            self->settingsForSide(destination.right), std::nullopt, destination.tile?destination.base.w:monitor->m_size.x);
        destination.scroll = destination.geometry.clampScroll(source->scroll);
        destination.flights.clear();
        destination.departingCards.clear();
        destination.paneTransition = false;
        destination.hovered.reset();
        destination.reorderHover.reset();
        destination.suspended = false;
        const auto mode = Fullscreen::controller()->getFullscreenModes(target).internal;
        destination.covered = mode == Fullscreen::FSMODE_FULLSCREEN || (self->maximizeCover && mode == Fullscreen::FSMODE_MAXIMIZED);
        destination.shown = destination.covered || monitor->m_activeSpecialWorkspace ? 0 : 1;
        destination.slideFrom = destination.shown;
        for (auto& card : destination.cards) {
            if (card.workspace.lock())
                self->updatePreviews(destination, card);
        }
        self->overviewExitScreens.push_back(std::move(destination));
    }
}

std::optional<stage::OverviewEndpoint> StageController::overviewDestination(const PHLWINDOW& window, const PHLWORKSPACE& activeWorkspace, const Rect& desktopWindow) {
    auto* self = Impl::instance;
    if (!self || !self->enabled || !window || !window->mapped() || static_cast<bool>(window->m_state & Desktop::View::WINDOW_STATE_PINNED) || !window->m_workspace || window->onSpecialWorkspace())
        return std::nullopt;
    const auto monitor = window->m_workspace->m_monitor.lock();
    const auto* destination = self->preparedOverviewScreen(monitor,TilesController::get() && TilesController::get()->available()?TilesController::get()->preferredTile(window->m_workspace,monitor):0);
    if (!monitor || !destination || destination->shown <= 0 || !destination->geometry.enabled())
        return std::nullopt;
    const auto target = destination->tile?self->screenWorkspace(*destination):
        activeWorkspace && activeWorkspace->m_monitor==monitor?activeWorkspace:monitor->m_activeWorkspace;
    if (!target || destination->active != workspaceKey(target))
        return std::nullopt;
    if (window->m_workspace == target) {
        if (!scrollingTiled(window))
            return std::nullopt;
        const Rect output = rect(destination->tile?destination->base:CBox{monitor->m_position,monitor->m_size});
        const auto edge = stage::edgeViewportForWindow(stage::desktopShadowViewport(self->desktopViewport(*destination), output), desktopWindow);
        return stage::OverviewEndpoint{desktopWindow, edge, rect(box(edge.box).intersection(box(output)))};
    }
    return self->cardEndpoint(*destination, window, true);
}

void StageController::finishOverview() {
    if (sessionLocked())
        return;

    auto* self = Impl::instance;
    if (!self)
        return;
    // Called from deferred overview teardown, outside rendering, after final
    // focus/fullscreen restoration. Publish current cards before another frame
    // can render; an idle refresh would leave one frame of the old workspace.
    self->finishingOverview = true;
    for (auto& screen : self->screens) {
        const auto monitor = screen.monitor.lock();
        const auto* prepared = self->preparedOverviewScreen(monitor,screen.tile);
        if (prepared && monitor && self->screenWorkspace(screen) && prepared->active == workspaceKey(self->screenWorkspace(screen))) {
            screen.cards = prepared->cards;
            screen.scroll = prepared->scroll;
        }
        screen.flights.clear();
        screen.departingCards.clear();
        screen.paneTransition = false;
        for (auto& card : screen.cards)
            card.shift = 0;
    }
    self->forceRefresh = true;
    self->sync();
    self->finishingOverview = false;
    self->overviewExitScreens.clear();
}

std::optional<bool> StageController::beginWorkspaceSwipe(void* gesture, PHLMONITOR monitor, bool (*original)(void*, PHLMONITOR)) {
    if (sessionLocked())
        return std::nullopt;

    auto* self = Impl::instance;
    if (!self || !original || !monitor || !self->ownsTransition(monitor->m_activeWorkspace, true))
        return std::nullopt;
    self->swipeBeginOriginal = reinterpret_cast<Impl::SwipeBeginFn>(original);
    return Impl::swipeBeginThunk(static_cast<CUnifiedWorkspaceSwipeGesture*>(gesture), monitor);
}

bool StageController::updateWorkspaceSwipe(void* gesture, double delta) {
    if (sessionLocked())
        return false;

    auto* self = Impl::instance;
    if (!self || !self->swipe || self->swipe->native != gesture)
        return false;
    self->updateSwipe(delta);
    return true;
}

bool StageController::endWorkspaceSwipe(void* gesture) {
    if (sessionLocked())
        return false;

    auto* self = Impl::instance;
    if (!self || !self->swipe || self->swipe->native != gesture)
        return false;
    ++self->swipeEnds;
    self->endSwipe();
    return true;
}

bool StageController::beginTrackpadWorkspaceSwipe() {
    if (sessionLocked())
        return false;

    auto* self = Impl::instance;
    auto* native = g_pUnifiedWorkspaceSwipe.get();
    if (!self || !native || (native->isGestureInProgress() && !self->swipe))
        return false;
    const auto monitor = Desktop::focusState()->monitor();
    if (!monitor || !self->ownsTransition(monitor->m_activeWorkspace, true))
        return false;
    // The monitored overload initializes session IDs, listeners and fullscreen
    // layer state. Its overview hook hands Stage the original native function.
    return native->begin(monitor);
}

void StageController::updateTrackpadWorkspaceSwipe(double delta) {
    if (sessionLocked())
        return;

    auto* self = Impl::instance;
    if (!self || !self->swipe || self->swipe->released)
        return;
    const double signedDelta = numberSetting("gestures:workspace_swipe_invert", 1) ? -delta : delta;
    self->updateSwipe(self->swipe->native->m_delta + signedDelta);
}

void StageController::endTrackpadWorkspaceSwipe(bool cancelled) {
    if (sessionLocked())
        return;

    auto* self = Impl::instance;
    if (!self || !self->swipe)
        return;
    self->swipe->cancelled = cancelled;
    endWorkspaceSwipe(self->swipe->native);
}

void StageController::rememberCanvasScroll(const PHLWORKSPACE& workspace, double offset) {
    auto* self = Impl::instance;
    if (!self || !self->enabled || !workspace || workspace->visible())
        return;
    std::erase_if(self->canvasScrolls, [&](const auto& saved) { return !saved.workspace || saved.workspace == workspace; });
    self->canvasScrolls.push_back({workspace, offset, false});
}

void StageController::Impl::restoreCanvasScroll(const PHLWORKSPACE& workspace) {
    if (sessionLocked())
        return;

    if (!enabled || !scrollingWorkspace(workspace))
        return;
    const auto saved = std::ranges::find_if(canvasScrolls, [&](const auto& entry) {
        return entry.activating && entry.workspace == workspace;
    });
    if (saved == canvasScrolls.end())
        return;
    auto* scrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(workspace->space()->algorithm()->tiledAlgo().get());
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return;
    auto& data = scrolling->m_scrollingData;
    const auto area = scrolling->usableArea();
    auto* controller = data->controller.get();
    const double extent = controller->calculateMaxExtent(area, numberSetting("scrolling:fullscreen_on_one_column", 1) != 0);
    const double viewport = controller->isPrimaryHorizontal() ? area.w : area.h;
    const double offset = std::clamp(saved->offset, 0.0, std::max(0.0, extent - viewport));
    data->controller->setOffset(offset);
    data->recalculate(true);
}

void StageController::Impl::beginCanvasActivation(const PHLWORKSPACE& workspace) {
    if (sessionLocked())
        return;

    if (!enabled || blocked() || !workspace || !g_pEventLoopManager)
        return;
    const auto saved = std::ranges::find_if(canvasScrolls, [&](const auto& entry) { return entry.workspace == workspace; });
    if (saved == canvasScrolls.end())
        return;
    saved->activating = true;
    restoreCanvasScroll(workspace);
    // Native activation restores the last focused window. Choose a window in
    // the newly scrolled viewport, without focusing the hidden workspace now.
    if (scrollingWorkspace(workspace)) {
        auto* scrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(workspace->space()->algorithm()->tiledAlgo().get());
        const auto column = scrolling ? scrolling->getColumnAtViewportCenter() : nullptr;
        if (column) {
            for (const auto& entry : column->targetDatas) {
                const auto target = entry->target.lock();
                const auto window = target ? target->window() : PHLWINDOW{};
                if (window && window->mapped() && !window->isHidden() && window->m_workspace == workspace) {
                    workspace->rememberFocusedWindow(window);
                    break;
                }
            }
        }
    }
    if (canvasRelease)
        return;
    // Keep the guard through the card click's post-activation focus, then
    // release it. Subsequent deliberate navigation must own the camera again.
    canvasRelease = g_pEventLoopManager->doLaterLock([this] {
        canvasRelease.reset();
        for (const auto& entry : canvasScrolls)
            if (entry.activating)
                restoreCanvasScroll(entry.workspace.lock());
        std::erase_if(canvasScrolls, [](const auto& entry) { return entry.activating || !entry.workspace; });
        request();
    });
}

std::optional<PHLWORKSPACE> StageController::hoveredScrollWorkspace() {
    auto* self = Impl::instance;
    if (!self || !self->enabled || self->blocked() || !g_pInputManager ||
        (self->inputSuppressed && self->inputSuppressed()))
        return std::nullopt;
    const auto [screen, index] = self->hit(g_pInputManager->getMouseCoordsInternal());
    if (!screen)
        return std::nullopt;
    return index ? screen->cards[*index].workspace.lock() : PHLWORKSPACE{};
}

bool StageController::renderingPreview() {
    return Impl::instance && Impl::instance->rendering && Impl::instance->surfaceTransform.has_value();
}

void StageController::withWindowRounding(const PHLWINDOW& window, float radius, const std::function<void()>& draw) {
    auto* self = Impl::instance;
    if (!self || !self->hooksReady) {
        draw();
        return;
    }
    const auto savedWindow = self->roundingWindow;
    const auto savedRadius = self->roundingOverride;
    Hyprutils::Utils::CScopeGuard restore{[&] {
        self->roundingWindow = savedWindow;
        self->roundingOverride = savedRadius;
    }};
    self->roundingWindow = window;
    self->roundingOverride = radius;
    draw();
}

CBox StageController::transformPreviewBox(CBox box) {
    if (renderingPreview()) {
        const auto& transform = *Impl::instance->surfaceTransform;
        box.translate(-transform.source.pos()).scale(transform.scale).translate(transform.target.pos());
    }
    return box;
}

void StageController::setOverviewRendering(bool active) {
    if (Impl::overviewRendering == active)
        return;
    Impl::overviewRendering = active;
    auto* self = Impl::instance;
    if (!self || !self->hooksReady)
        return;
    for (auto* hook : {self->surfaceBoxHook, self->surfaceVisibleHook, self->surfaceUVHook}) {
        if (active) {
            if (hook)
                hook->unhook();
        } else if (!hook || !hook->hook()) {
            self->error = "stage disabled: native surface hook reattach failed after overview";
            self->hookFailure = true;
            self->releaseHooks();
            self->request();
            break;
        }
    }
}

void StageController::cancelTileTransfer() {
    auto* self=Impl::instance;
    if (!self || !self->tileDragPending) return;
    if (auto* tiles=TilesController::get()) tiles->finishTransfer(false);
    self->tileDragPending=false;
    self->tileDragWorkspace.reset(); self->tileDragClicked.reset(); self->tileDragMonitor.reset();
    self->pressed.reset(); self->pressedSyntheticId=0; self->reorderArmed=false;
    // Retain the consumed press until its release, including while overview owns input.
    self->request();
}

} // namespace hymission
