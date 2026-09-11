#include "stage_controller.hpp"
#include "stage_logic.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <linux/input-event-codes.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
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
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

// CSpace has no plugin work-area contributor API. Change only its freshly
// computed boxes after the original function, never monitor/bar reservations.
#define private public
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#undef private

#include "vendor/nlohmann/json.hpp"

namespace hymission {
namespace {
using Render::GL::g_pHyprOpenGL;
using Clock = std::chrono::steady_clock;

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

double numberSetting(const char* key, double fallback) {
    const auto value = Config::mgr()->getConfigValue(key);
    if (!value.dataptr || !value.type)
        return fallback;
    if (*value.type == typeid(Config::FLOAT))
        return **reinterpret_cast<Config::FLOAT* const*>(value.dataptr);
    if (*value.type == typeid(bool))
        return **reinterpret_cast<bool* const*>(value.dataptr) ? 1 : 0;
    if (*value.type == typeid(Config::INTEGER))
        return **reinterpret_cast<Config::INTEGER* const*>(value.dataptr);
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
    std::vector<UP<IPassElement>> draw() override { m_draw(); return {}; }
    bool needsLiveBlur() override { return true; }
    bool needsPrecomputeBlur() override { return false; }
    bool undiscardable() override { return true; }
    bool disableSimplification() override { return false; }
    const char* passName() override { return "HymissionStage"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override { return m_bounds; }
    CRegion opaqueRegion() override { return {}; }
  private:
    std::function<void()> m_draw;
    CBox m_bounds;
};
} // namespace

struct StageController::Impl {
    struct Preview {
        PHLWINDOWREF window;
        CBox target;
        CBox natural;
    };
    struct Flight {
        Preview preview;
        CBox from;
        CBox to;
        float fromRadius = 0;
        float toRadius = 0;
    };
    struct Card {
        PHLWORKSPACEREF workspace;
        std::vector<Preview> previews;
        bool dirty = true;
        bool previewsReady = false;
        double shift = 0;
        Clock::time_point shiftStart;
    };
    struct Screen {
        PHLMONITORREF monitor;
        CBox base;
        stage::Geometry geometry;
        std::vector<Card> cards;
        double scroll = 0;
        float scale = 1;
        int transform = 0;
        WORKSPACEID active = WORKSPACE_INVALID;
        std::optional<double> frozenWidth;
        bool covered = false;
        bool suspended = false;
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
        WORKSPACEID requestedTarget = WORKSPACE_INVALID;
        Screen visual;
        bool prepared = false;
        bool released = false;
        bool committed = false;
        bool cancelled = false;
        double progress = 0;
        double releaseFrom = 0;
        double releaseTo = 0;
        double releaseDuration = 0;
        double settleProgress = 0;
        Clock::time_point releaseStart;
    };

    using RecheckFn = void (*)(Layout::CSpace*);
    using DragEndFn = void (*)(Layout::Supplementary::CDragStateController*);
    using WindowAtFn = PHLWINDOW (*)(const Desktop::CViewHitTester*, const Vector2D&, uint16_t, PHLWINDOW);
    using RoundingFn = float (*)(Desktop::View::CWindow*);
    using RenderWindowFn = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
    using ShouldBlurFn = bool (*)(Render::IHyprRenderer*, PHLWINDOW);
    using SurfaceBoxFn = CBox (*)(CSurfacePassElement*);
    using SurfaceVisibleFn = CRegion (*)(CSurfacePassElement*, bool&);
    using SurfaceUVFn = void (*)(void*, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool, const Vector2D&, const Vector2D&, bool);
    using AddPassFn = void (*)(Render::IHyprRenderer*, UP<IPassElement>&&);
    using WorkspaceAnimationFn = void (*)(PHLWORKSPACE, Animation::Workspace::eAnimationType, bool, bool, std::optional<std::string>);
    using ChangeWorkspaceFn = void (*)(Monitor::CMonitor*, const PHLWORKSPACE&, bool, bool, bool);
    using SwipeBeginFn = void (*)(CUnifiedWorkspaceSwipeGesture*);
    using SwipeUpdateFn = void (*)(CUnifiedWorkspaceSwipeGesture*, double);
    using SwipeEndFn = void (*)(CUnifiedWorkspaceSwipeGesture*);
    inline static Impl* instance = nullptr;
    inline static bool overviewRendering = false;

    HANDLE handle;
    std::function<bool()> overviewSuspended;
    std::vector<Screen> screens;
    std::vector<PHLWINDOWREF> livePreviewWindows;
    std::optional<Swipe> swipe;
    std::vector<CHyprSignalListener> listeners;
    CFunctionHook* areaHook = nullptr;
    CFunctionHook* dragHook = nullptr;
    CFunctionHook* hitHook = nullptr;
    CFunctionHook* roundingHook = nullptr;
    CFunctionHook* renderWindowHook = nullptr;
    CFunctionHook* surfaceBoxHook = nullptr;
    CFunctionHook* surfaceVisibleHook = nullptr;
    CFunctionHook* surfaceUVHook = nullptr;
    CFunctionHook* addPassHook = nullptr;
    CFunctionHook* workspaceAnimationHook = nullptr;
    CFunctionHook* changeWorkspaceHook = nullptr;
    SwipeBeginFn swipeBeginOriginal = nullptr;
    bool drawingDecoration = false;
    float decorationRadius = 0;
    CBox decorationClip;
    struct SurfaceTransform {
        CBox source;
        CBox target;
        double scale = 1;
    };
    std::optional<SurfaceTransform> surfaceTransform;
    RenderWindowFn renderWindow = nullptr;
    ShouldBlurFn shouldBlur = nullptr;
    UP<SEventLoopDoLaterLock> deferred;
    SP<CEventLoopTimer> refreshTimer;
    SP<CEventLoopTimer> motionTimer;
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
    PHLWORKSPACEREF pressed;
    std::unordered_set<uint32_t> swallowedButtons;
    PHLWINDOWREF lastDragged;
    std::string error;

    Impl(HANDLE h, std::function<bool()> suspended) : handle(h), overviewSuspended(std::move(suspended)) { instance = this; }
    ~Impl();
    void initialize();
    void request(bool dirty = true);
    void sync();
    void armRefresh();
    void armMotion();
    void motion();
    bool installHooks();
    void releaseHooks();
    void afterRecheck(Layout::CSpace* space);
    void endDrag(Layout::Supplementary::CDragStateController* drag);
    void renderStage(eRenderStage stage);
    void draw(const PHLMONITOR& monitor);
    void drawFlights(Screen& screen, const PHLMONITOR& monitor);
    void drawPreview(const PHLWINDOW& window, const PHLMONITOR& monitor, const CBox& target, const CBox& clip, double radius);
    void startFlights(Screen& screen, WORKSPACEID previous, const std::vector<Card>& oldCards, const stage::Geometry& oldGeometry, double oldScroll, bool oldRight,
                      const std::vector<std::pair<PHLWINDOWREF, CBox>>& origins, PHLWORKSPACE visualActive = nullptr, bool stopNative = true);
    bool flying(const Screen& screen, const PHLWINDOW& window) const;
    double flightProgress(const Screen& screen) const;
    void refreshPreviews();
    void updatePreviewLiveness();
    bool updatePreviews(Screen& screen, Card& card);
    bool updatePreviews(Screen& screen, Card& card, const Vector2D& tiledOffset);
    void prepareSwipe(const PHLWORKSPACE& target);
    void updateSwipe(double delta);
    void endSwipe();
    void clearSwipe();
    Screen* visualScreenFor(const PHLMONITOR& monitor);
    stage::Settings settingsForSide(bool right) const;
    bool blocked() const;
    bool interactive(const Screen& screen) const;
    bool ownsTransition(const PHLWORKSPACE& workspace) const;
    Screen* screenFor(const PHLMONITOR& monitor);
    std::pair<Screen*, std::optional<std::size_t>> hit(const Vector2D& point);
    void pointer();
    void button(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info);
    void axis(const IPointer::SAxisEvent& event, Event::SCallbackInfo& info);
    void activate(const PHLWORKSPACE& workspace);
    void correctFloating(const PHLMONITOR& monitor, const CBox& desktop);
    void damage(const Screen& screen);
    double cardTop(const Screen& screen, std::size_t index) const;
    std::optional<std::size_t> cardHit(const Screen& screen, const Vector2D& point) const;
    CBox sidebar(const Screen& screen) const;
    CBox desktop(const Screen& screen) const;
    std::string stateJson() const;

    static void recheckThunk(Layout::CSpace* space) {
        auto* self = instance;
        reinterpret_cast<RecheckFn>(self->areaHook->m_original)(space);
        self->afterRecheck(space);
    }
    static void dragThunk(Layout::Supplementary::CDragStateController* drag) { instance->endDrag(drag); }
    static void swipeBeginThunk(CUnifiedWorkspaceSwipeGesture* gesture) {
        auto* self = instance;
        const auto monitor = Desktop::focusState()->monitor();
        if (self->swipe)
            self->clearSwipe();
        self->swipeBeginOriginal(gesture);
        if (monitor && self->ownsTransition(monitor->m_activeWorkspace) && gesture->isGestureInProgress()) {
            self->swipe.emplace();
            self->swipe->native = gesture;
            self->swipe->monitor = monitor;
            self->swipe->origin = monitor->m_activeWorkspace;
        }
    }
    static void workspaceAnimationThunk(PHLWORKSPACE workspace, Animation::Workspace::eAnimationType type, bool left, bool instant,
                                        std::optional<std::string> style) {
        auto* self = instance;
        const bool owned = self->ownsTransition(workspace);
        reinterpret_cast<WorkspaceAnimationFn>(self->workspaceAnimationHook->m_original)(workspace, type, left, instant || owned, std::move(style));
        if (owned)
            workspace->m_renderOffset->setValueAndWarp(Vector2D{});
    }
    static void changeWorkspaceThunk(Monitor::CMonitor* monitor, const PHLWORKSPACE& workspace, bool internal, bool noMouseMove, bool noFocus) {
        auto* self = instance;
        if (self->swipe && self->swipe->monitor == monitor->m_self && (!self->swipe->released || self->swipe->target != workspace))
            self->clearSwipe();
        reinterpret_cast<ChangeWorkspaceFn>(self->changeWorkspaceHook->m_original)(monitor, workspace, internal, noMouseMove, noFocus);
        if (self->enabled && !self->blocked() && !self->syncing) {
            // Finish preparing our transition in the dispatch itself, before a
            // frame can expose the new workspace or a native workspace slide.
            self->forceRefresh = true;
            self->sync();
        }
    }
    static void addPassThunk(Render::IHyprRenderer* renderer, UP<IPassElement>&& element) {
        auto* self = instance;
        if (!self->drawingDecoration) {
            reinterpret_cast<AddPassFn>(self->addPassHook->m_original)(renderer, std::move(element));
            return;
        }
        // Native decorations normally enqueue passes. While composing a stage
        // preview, draw them here instead of modifying the pass being iterated.
        renderer->m_renderData.clipBox = self->decorationClip;
        renderer->draw(element, renderer->m_renderData.damage);
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
    static CRegion surfaceVisibleThunk(CSurfacePassElement* element, bool& cancel) {
        auto* self = instance;
        if (!self->surfaceTransform)
            return reinterpret_cast<SurfaceVisibleFn>(self->surfaceVisibleHook->m_original)(element, cancel);
        // Native visible regions describe the full-size desktop surface. Use
        // the transformed surface bounds, as overview does, before clipping.
        cancel = false;
        return CRegion{surfaceBoxThunk(element).scale(element->m_data.pMonitor->m_scale).round()};
    }
    static void surfaceUVThunk(void* renderer, PHLWINDOW window, SP<CWLSurfaceResource> surface, PHLMONITOR monitor, bool main,
                               const Vector2D& projected, const Vector2D& unscaled, bool misaligned) {
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
        reinterpret_cast<SurfaceUVFn>(self->surfaceUVHook->m_original)(renderer, window, std::move(surface), monitor, main, size, logical, misaligned);
    }
    static void renderWindowThunk(Render::IHyprRenderer* renderer, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time,
                                  bool decorate, Render::eRenderPassMode mode, bool ignorePosition, bool standalone) {
        auto* self = instance;
        const auto* screen = self->visualScreenFor(monitor);
        if (!standalone && !ignorePosition && !self->rendering && !renderer->m_bRenderingSnapshot && self->enabled && !self->blocked() && screen && !screen->covered &&
            !monitor->m_activeSpecialWorkspace && self->flying(*screen, window))
            return;
        reinterpret_cast<RenderWindowFn>(self->renderWindowHook->m_original)(renderer, window, monitor, time, decorate, mode, ignorePosition, standalone);
    }
    static float roundingThunk(Desktop::View::CWindow* window) {
        auto* self = instance;
        // Native decorations query a pre-scale radius; surfaces receive their
        // final displayed radius directly without changing the real window.
        return self->drawingDecoration ? self->decorationRadius : reinterpret_cast<RoundingFn>(self->roundingHook->m_original)(window);
    }
    static PHLWINDOW windowAtThunk(const Desktop::CViewHitTester* tester, const Vector2D& position, uint16_t properties, PHLWINDOW ignore) {
        auto* self = instance;
        // The sidebar is above windows, including floats partially under it.
        // This also prevents native dragEnd from grouping into a hidden deco.
        if (self->hit(position).first)
            return nullptr;
        return reinterpret_cast<WindowAtFn>(self->hitHook->m_original)(tester, position, properties, ignore);
    }
};

bool StageController::Impl::blocked() const {
    return (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) || (overviewSuspended && overviewSuspended());
}

bool StageController::Impl::ownsTransition(const PHLWORKSPACE& workspace) const {
    if (!enabled || !workspace || workspace->m_isSpecialWorkspace || blocked())
        return false;
    const auto monitor = workspace->m_monitor.lock();
    if (!monitor || monitor->m_activeSpecialWorkspace)
        return false;
    const auto screen = std::ranges::find_if(screens, [&](const auto& s) { return s.monitor == monitor; });
    if (screen == screens.end() || !screen->geometry.enabled() || screen->covered || screen->suspended)
        return false;
    const auto targetMode = monitor->m_activeWorkspace ? Fullscreen::controller()->getFullscreenModes(monitor->m_activeWorkspace).internal : Fullscreen::FSMODE_NONE;
    return targetMode != Fullscreen::FSMODE_FULLSCREEN && !(maximizeCover && targetMode == Fullscreen::FSMODE_MAXIMIZED);
}

bool StageController::Impl::interactive(const Screen& screen) const {
    return enabled && !rendering && !blocked() && screen.geometry.enabled() && !screen.covered && !screen.suspended;
}

StageController::Impl::Screen* StageController::Impl::screenFor(const PHLMONITOR& monitor) {
    const auto it = std::ranges::find_if(screens, [&](const auto& s) { return s.monitor == monitor; });
    return it == screens.end() ? nullptr : &*it;
}

StageController::Impl::Screen* StageController::Impl::visualScreenFor(const PHLMONITOR& monitor) {
    if (swipe && swipe->prepared && swipe->monitor == monitor)
        return &swipe->visual;
    return screenFor(monitor);
}

stage::Settings StageController::Impl::settingsForSide(bool right) const {
    auto result = settings;
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
    if (swipe->native) {
        swipe->native->m_workspaceBegin.reset();
        swipe->native->m_initialDirection = 0;
        swipe->native->m_delta = 0;
    }
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
    if (!numberSetting("animations:enabled", 1) || setting("stage_transition_ms", 300) <= 0)
        return;
    auto& visual = swipe->visual;
    visual = *source;
    // A new workspace has no native object until the gesture commits. Build
    // its empty desktop now so outgoing windows still follow the finger.
    visual.active = target ? target->m_id : swipe->requestedTarget;
    visual.right = smartisan ? !source->right : source->right;
    visual.cards.clear();
    for (const auto& workspace : State::workspaceState()->workspaces()) {
        if (workspace->m_isSpecialWorkspace || workspace->m_monitor != monitor || workspace == target ||
            (!setting("stage_show_empty", 1) && workspace->getWindowCount(std::nullopt, false) == 0))
            continue;
        const auto old = std::ranges::find_if(source->cards, [&](const auto& card) { return card.workspace == workspace; });
        visual.cards.push_back(old == source->cards.end() ? Card{.workspace = workspace} : *old);
    }
    std::ranges::sort(visual.cards, [](const Card& a, const Card& b) {
        if ((a.workspace->m_id > 0) != (b.workspace->m_id > 0))
            return a.workspace->m_id > 0;
        return a.workspace->m_id > 0 ? a.workspace->m_id < b.workspace->m_id : a.workspace->m_name < b.workspace->m_name;
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
        if (window->m_monitor == monitor && window->m_isMapped && !window->isHidden() && !window->m_pinned)
            origins.emplace_back(window, setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
                CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()});
    startFlights(visual, origin->m_id, source->cards, source->geometry, source->scroll, source->right, origins, target, false);
    for (auto& flight : visual.flights) {
        const auto window = flight.preview.window.lock();
        if (!window)
            continue;
        if (window->m_workspace == target) {
            flight.to = flight.preview.natural;
            if (!window->m_isFloating)
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
    swipe->prepared = true;
    updatePreviewLiveness();
    armMotion();
}

void StageController::Impl::updateSwipe(double delta) {
    if (!swipe || swipe->released)
        return;
    const auto monitor = swipe->monitor.lock();
    if (!monitor || monitor->m_activeWorkspace != swipe->origin || !ownsTransition(monitor->m_activeWorkspace) || Desktop::focusState()->monitor() != monitor) {
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
    auto id = getWorkspaceIDNameFromString(useR ? (left ? "r-1" : "r+1") : (left ? "m-1" : "m+1")).id;
    const bool create = numberSetting("gestures:workspace_swipe_create_new", 1);
    if (!left && id <= swipe->origin->m_id && create)
        id = getWorkspaceIDNameFromString("r+1").id;
    if ((left && id >= swipe->origin->m_id) || id == WORKSPACE_INVALID || id == swipe->origin->m_id) {
        native->m_delta = 0;
        swipe->progress = 0;
        return;
    }
    swipe->requestedTarget = id;
    const auto target = State::workspaceState()->query().id(id).run();
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
        endSwipe();
        swipeBeginThunk(native);
    }
}

void StageController::Impl::endSwipe() {
    if (!swipe || swipe->released)
        return;
    const auto monitor = swipe->monitor.lock();
    if (!monitor || monitor->m_activeWorkspace != swipe->origin || !ownsTransition(monitor->m_activeWorkspace)) {
        clearSwipe();
        return;
    }
    auto* native = swipe->native;
    const double distance = std::max(1.0, numberSetting("gestures:workspace_swipe_distance", 300));
    const double force = numberSetting("gestures:workspace_swipe_min_speed_to_force", 30);
    const bool commit = !swipe->cancelled && std::abs(native->m_delta) >= 2 &&
        (std::abs(native->m_delta) >= distance * numberSetting("gestures:workspace_swipe_cancel_ratio", 0.5) || (force > 0 && native->m_avgSpeed >= force));
    native->m_workspaceBegin.reset();
    native->m_initialDirection = 0;
    swipe->released = true;
    swipe->releaseFrom = swipe->progress;
    swipe->releaseTo = commit ? 1 : 0;
    swipe->releaseStart = Clock::now();
    swipe->releaseDuration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L) * std::abs(swipe->releaseTo - swipe->releaseFrom);
    if (commit) {
        if (swipe->prepared) {
            for (auto& flight : swipe->visual.flights) {
                const auto box = stage::transitionBoxWithin({flight.from.x, flight.from.y, flight.from.w, flight.from.h},
                    {flight.to.x, flight.to.y, flight.to.w, flight.to.h}, swipe->progress,
                    flightBounds(monitor));
                flight.from = {box.x, box.y, box.width, box.height};
                flight.fromRadius += (flight.toRadius - flight.fromRadius) * stage::transitionProgress(swipe->progress, 1);
            }
            swipe->releaseDuration = std::max(16.0, swipe->releaseDuration);
        }
        auto target = swipe->target.lock();
        if (!target && swipe->requestedTarget != WORKSPACE_INVALID && numberSetting("gestures:workspace_swipe_create_new", 1))
            target = State::workspaceState()->create(swipe->requestedTarget, monitor->m_id);
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
            if (!actual || actual->covered || actual->suspended) {
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
                if (next != actual->flights.end())
                    flight.to = next->to;
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
    if (hooksReady)
        return true;
    if (hookFailure)
        return false;
    const auto find = [&](const char* name, const char* qualified) -> void* {
        for (const auto& match : HyprlandAPI::findFunctionsByName(handle, name)) {
            if (match.demangled.find(qualified) != std::string::npos)
                return match.address;
        }
        error = std::string("stage disabled: missing symbol ") + qualified;
        return nullptr;
    };
    const auto attach = [&](CFunctionHook* hook, const char* name) {
        if (hook && hook->hook())
            return true;
        error = std::string("stage disabled: failed to attach ") + name;
        return false;
    };
    const auto area = find("recheckWorkArea", "Layout::CSpace::recheckWorkArea()");
    const auto drag = find("dragEnd", "Layout::Supplementary::CDragStateController::dragEnd()");
    const auto hit = find("windowAt", "Desktop::CViewHitTester::windowAt(");
    const auto rounding = find("rounding", "Desktop::View::CWindow::rounding()");
    const auto surfaceBox = find("getTexBox", "CSurfacePassElement::getTexBox()");
    const auto surfaceVisible = find("visibleRegion", "CSurfacePassElement::visibleRegion(bool&)");
    const auto surfaceUV = find("calculateUVForSurface", "IElementRenderer::calculateUVForSurface(");
    const auto addPass = find("addPassElement", "IHyprRenderer::addPassElement(");
    const auto workspaceAnimation = find("startAnimation", "Animation::Workspace::startAnimation(");
    const auto changeWorkspace = find("changeWorkspace", "Monitor::CMonitor::changeWorkspace(Hyprutils::Memory::CSharedPointer<CWorkspace> const&");
    renderWindow = reinterpret_cast<RenderWindowFn>(find("renderWindow", "IHyprRenderer::renderWindow("));
    shouldBlur = reinterpret_cast<ShouldBlurFn>(find("shouldBlur", "IHyprRenderer::shouldBlur(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>)"));
    if (area && drag && hit && rounding && surfaceBox && surfaceVisible && surfaceUV && addPass && workspaceAnimation && changeWorkspace && renderWindow && shouldBlur && g_pHyprOpenGL) {
        areaHook = HyprlandAPI::createFunctionHook(handle, area, reinterpret_cast<void*>(&recheckThunk));
        dragHook = HyprlandAPI::createFunctionHook(handle, drag, reinterpret_cast<void*>(&dragThunk));
        hitHook = HyprlandAPI::createFunctionHook(handle, hit, reinterpret_cast<void*>(&windowAtThunk));
        roundingHook = HyprlandAPI::createFunctionHook(handle, rounding, reinterpret_cast<void*>(&roundingThunk));
        renderWindowHook = HyprlandAPI::createFunctionHook(handle, reinterpret_cast<void*>(renderWindow), reinterpret_cast<void*>(&renderWindowThunk));
        surfaceBoxHook = HyprlandAPI::createFunctionHook(handle, surfaceBox, reinterpret_cast<void*>(&surfaceBoxThunk));
        surfaceVisibleHook = HyprlandAPI::createFunctionHook(handle, surfaceVisible, reinterpret_cast<void*>(&surfaceVisibleThunk));
        surfaceUVHook = HyprlandAPI::createFunctionHook(handle, surfaceUV, reinterpret_cast<void*>(&surfaceUVThunk));
        addPassHook = HyprlandAPI::createFunctionHook(handle, addPass, reinterpret_cast<void*>(&addPassThunk));
        workspaceAnimationHook = HyprlandAPI::createFunctionHook(handle, workspaceAnimation, reinterpret_cast<void*>(&workspaceAnimationThunk));
        changeWorkspaceHook = HyprlandAPI::createFunctionHook(handle, changeWorkspace, reinterpret_cast<void*>(&changeWorkspaceThunk));
        hooksReady = attach(areaHook, "work area") && attach(dragHook, "drag end") && attach(hitHook, "window hit test") &&
            attach(roundingHook, "rounding") && attach(renderWindowHook, "window rendering") &&
            (overviewRendering || (attach(surfaceBoxHook, "surface box") && attach(surfaceVisibleHook, "surface visible region") && attach(surfaceUVHook, "surface UV"))) &&
            attach(addPassHook, "decoration pass") && attach(workspaceAnimationHook, "workspace animation") && attach(changeWorkspaceHook, "workspace change");
        if (hooksReady)
            renderWindow = reinterpret_cast<RenderWindowFn>(renderWindowHook->m_original);
    }
    if (!hooksReady) {
        releaseHooks();
        hookFailure = true;
        if (error.empty())
            error = "stage disabled: OpenGL renderer unavailable";
        Log::logger->log(Log::ERR, "[hymission] {}", error);
        HyprlandAPI::addNotification(handle, "[hymission] " + error, CHyprColor(1.0, 0.3, 0.2, 1.0), 8000);
    }
    return hooksReady;
}

void StageController::Impl::releaseHooks() {
    for (auto** hook : {&areaHook, &dragHook, &hitHook, &roundingHook, &renderWindowHook, &surfaceBoxHook, &surfaceVisibleHook, &surfaceUVHook, &addPassHook,
                        &workspaceAnimationHook, &changeWorkspaceHook}) {
        if (!*hook)
            continue;
        (*hook)->unhook();
        HyprlandAPI::removeFunctionHook(handle, *hook);
        *hook = nullptr;
    }
    hooksReady = false;
}

void StageController::Impl::initialize() {
    auto& events = Event::bus()->m_events;
    listeners.emplace_back(events.config.reloaded.listen([this] { reconfigure = true; request(); }));
    listeners.emplace_back(events.config.props_refreshed.listen([this](bool) { request(); }));
    listeners.emplace_back(events.monitor.layoutChanged.listen([this] { request(); }));
    listeners.emplace_back(events.monitor.added.listen([this](PHLMONITOR) { request(); }));
    listeners.emplace_back(events.monitor.removed.listen([this](PHLMONITOR) { request(); }));
    listeners.emplace_back(events.workspace.active.listen([this](PHLWORKSPACE) { request(); }));
    listeners.emplace_back(events.workspace.created.listen([this](PHLWORKSPACEREF) { request(); }));
    listeners.emplace_back(events.workspace.removed.listen([this](PHLWORKSPACEREF) { request(); }));
    listeners.emplace_back(events.workspace.moveToMonitor.listen([this](PHLWORKSPACE, PHLMONITOR) { request(); }));
    listeners.emplace_back(events.workspace.specialActive.listen([this](PHLWORKSPACE, PHLMONITOR) { request(); }));
    listeners.emplace_back(events.window.open.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.close.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.floating.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.fullscreen.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.title.listen([this](PHLWINDOW) { request(); }));
    listeners.emplace_back(events.window.moveToWorkspace.listen([this](PHLWINDOW, PHLWORKSPACE) { request(); }));
    listeners.emplace_back(events.input.mouse.move.listen([this](const Vector2D& position, Event::SCallbackInfo& info) {
        pointer();
        const auto* drag = g_layoutManager->dragController().get();
        if (!info.cancelled && hit(position).first && (!drag || !drag->target()) && !g_pInputManager->hasHeldButtons()) {
            g_pSeatManager->setPointerFocus(nullptr, {});
            info.cancelled = true;
        }
    }));
    listeners.emplace_back(events.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) { const auto copy = event; button(copy, info); }));
    listeners.emplace_back(events.input.mouse.axis.listen([this](const IPointer::SAxisEvent& event, Event::SCallbackInfo& info) { axis(event, info); }));
    listeners.emplace_back(events.gesture.swipe.end.listen([this](const IPointer::SSwipeEndEvent& event, Event::SCallbackInfo&) {
        if (swipe)
            swipe->cancelled = event.cancelled;
    }));
    listeners.emplace_back(events.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& event, Event::SCallbackInfo&) {
        const auto* drag = g_layoutManager->dragController().get();
        if (event.keycode == KEY_ESC && event.state == WL_KEYBOARD_KEY_STATE_PRESSED && drag && drag->mode() == MBIND_MOVE && drag->target()) {
            cancelDrop = true;
            lastDragged = drag->target()->window();
        }
    }));
    listeners.emplace_back(events.render.stage.listen([this](eRenderStage stage) { renderStage(stage); }));
    listeners.emplace_back(events.render.preChecks.listen([this](PHLMONITOR monitor) {
        if (!enabled || rendering)
            return;
        auto* screen = screenFor(monitor);
        if (screen && screen->geometry.enabled() && screen->shown > 0 && !blocked() && !monitor->m_activeSpecialWorkspace)
            monitor->m_solitaryClient.reset(); // per-output: compose the slide before resuming direct scanout
        if (!screen || !sameBox(screen->base, baseArea(monitor)) || screen->scale != monitor->m_scale || screen->transform != static_cast<int>(monitor->m_transform))
            request();
        else if (screen->suspended != (blocked() || !!monitor->m_activeSpecialWorkspace))
            request();
    }));
    if (g_pSessionLockManager) {
        listeners.emplace_back(g_pSessionLockManager->m_events.lock.listen([this] { request(); }));
        listeners.emplace_back(g_pSessionLockManager->m_events.unlock.listen([this] { request(); }));
    }
    request();
}

void StageController::Impl::request(bool dirty) {
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
    if (!enabled || !space)
        return;
    const auto workspace = space->workspace();
    if (!workspace || workspace->m_isSpecialWorkspace)
        return;
    const auto monitor = workspace->m_monitor.lock();
    auto* screen = screenFor(monitor);
    if (!screen || !screen->geometry.enabled())
        return;
    if (maximizeCover && Fullscreen::controller()->getFullscreenModes(workspace).internal == Fullscreen::FSMODE_MAXIMIZED)
        return;
    // Each invocation starts from the original recheckWorkArea output. Retain
    // workspace gaps, including float gaps, and never compound our reservation.
    for (auto* box : {&space->m_workArea, &space->m_floatingWorkArea}) {
        const double amount = std::min(screen->geometry.reservation, std::max(0.0, box->w - 1.0));
        if (!screen->right)
            box->x += amount;
        box->w -= amount;
    }
    if (!syncing && !sameBox(screen->base, baseArea(monitor)))
        request();
}

void StageController::Impl::sync() {
    if (syncing || shuttingDown)
        return;
    if (g_pHyprRenderer && g_pHyprRenderer->m_renderData.pMonitor) {
        request(false);
        return;
    }
    syncing = true;
    const bool wanted = setting("stage_enabled", 0) != 0;
    const bool wasEnabled = enabled;
    enabled = wanted && installHooks();
    if (!enabled) {
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
        Log::logger->log(Log::WARN, "[hymission] normalized invalid stage widths/padding/gaps");
    const bool nextMaximizeCover = setting("stage_maximize_cover_strip", 0) != 0;
    const bool changedPolicy = maximizeCover != nextMaximizeCover;
    maximizeCover = nextMaximizeCover;
    const bool nextSmartisan = setting("stage_smartisan_mode", 0) != 0;
    const bool changedMode = nextSmartisan != smartisan;
    if (changedMode)
        clearSwipe();
    smartisan = nextSmartisan;
    const bool showEmpty = setting("stage_show_empty", 1) != 0;
    auto* drag = g_layoutManager->dragController().get();
    const auto dragged = drag && drag->mode() == MBIND_MOVE && drag->target() ? drag->target()->window() : nullptr;
    if (dragged != lastDragged) {
        cancelDrop = false;
        lastDragged = dragged;
    }

    std::erase_if(screens, [&](const auto& screen) {
        const auto monitor = screen.monitor.lock();
        return !monitor || std::ranges::find(State::monitorState()->monitors(), monitor) == State::monitorState()->monitors().end();
    });
    const auto workspaces = State::workspaceState()->workspacesCopy();
    std::vector<PHLMONITOR> relayout;
    struct PendingFlight {
        PHLMONITOR monitor;
        WORKSPACEID previous;
        std::vector<Card> cards;
        stage::Geometry geometry;
        double scroll;
        bool right;
        std::vector<std::pair<PHLWINDOWREF, CBox>> origins;
    };
    std::vector<PendingFlight> pendingFlights;
    for (const auto& monitor : State::monitorState()->monitors()) {
        auto* screen = screenFor(monitor);
        if (!screen) {
            screens.emplace_back();
            screen = &screens.back();
            screen->monitor = monitor;
        }
        const auto base = baseArea(monitor);
        const auto oldGeometry = screen->geometry;
        const bool switching = screen->active != WORKSPACE_INVALID && monitor->m_activeWorkspace && screen->active != monitor->m_activeWorkspace->m_id;
        const bool changedOutput = !sameBox(base, screen->base) || screen->scale != monitor->m_scale || screen->transform != static_cast<int>(monitor->m_transform);
        if (switching && !changedOutput && !changedPolicy && !reconfigure && !blocked() && !screen->covered && !screen->suspended) {
            pendingFlights.push_back({monitor, screen->active, screen->cards, oldGeometry, screen->scroll, screen->right, {}});
            for (const auto& window : Desktop::windowState()->windows()) {
                if (window->m_monitor == monitor && window->m_isMapped && !window->isHidden() && !window->m_pinned)
                    pendingFlights.back().origins.emplace_back(window, setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() :
                        CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()});
            }
        }
        const bool oldRight = screen->right;
        if (!smartisan || changedMode)
            screen->right = false;
        if (smartisan && switching && !blocked() && !screen->covered && !screen->suspended)
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

        std::vector<PHLWORKSPACE> targets;
        for (const auto& workspace : workspaces) {
            if (workspace->m_isSpecialWorkspace || workspace->m_monitor != monitor)
                continue;
            if (workspace == monitor->m_activeWorkspace)
                continue;
            if (!showEmpty && workspace->getWindowCount(std::nullopt, false) == 0)
                continue;
            targets.emplace_back(workspace);
        }
        std::ranges::sort(targets, [](const auto& a, const auto& b) {
            if ((a->m_id > 0) != (b->m_id > 0))
                return a->m_id > 0;
            return a->m_id > 0 ? a->m_id < b->m_id : a->m_name < b->m_name;
        });
        bool changedCards = targets.size() != screen->cards.size();
        if (!changedCards) {
            for (std::size_t i = 0; i < targets.size(); ++i)
                changedCards |= screen->cards[i].workspace != targets[i];
        }
        std::vector<std::pair<PHLWORKSPACEREF, double>> oldTops;
        for (std::size_t i = 0; i < screen->cards.size(); ++i)
            oldTops.emplace_back(screen->cards[i].workspace, cardTop(*screen, i) + screen->scroll);
        if (changedCards) {
            std::vector<Card> cards;
            for (const auto& workspace : targets) {
                auto it = std::ranges::find_if(screen->cards, [&](const auto& card) { return card.workspace == workspace; });
                if (it != screen->cards.end())
                    cards.emplace_back(std::move(*it));
                else
                    cards.push_back(Card{.workspace = workspace});
            }
            screen->cards = std::move(cards);
        }
        if (dragged && !screen->frozenWidth && oldGeometry.enabled())
            screen->frozenWidth = oldGeometry.cardWidth;
        if (!dragged)
            screen->frozenWidth.reset();
        auto screenSettings = settingsForSide(screen->right);
        screen->geometry = stage::layout(base.w, base.h, targets.size(), screenSettings, screen->frozenWidth, monitor->m_size.x);
        const bool changedGeometry = changedSide || changedOutput || oldGeometry.reservation != screen->geometry.reservation || oldGeometry.cardHeight != screen->geometry.cardHeight ||
            oldGeometry.padding != screen->geometry.padding || oldGeometry.paddingTop != screen->geometry.paddingTop ||
            oldGeometry.paddingBottom != screen->geometry.paddingBottom || oldGeometry.cardGap != screen->geometry.cardGap;
        screen->scroll = screen->geometry.clampScroll(screen->scroll);
        if (changedCards || changedGeometry) {
            for (std::size_t i = 0; i < screen->cards.size(); ++i) {
                auto& card = screen->cards[i];
                const auto old = std::ranges::find_if(oldTops, [&](const auto& entry) { return entry.first == card.workspace; });
                card.shift = !changedOutput && numberSetting("animations:enabled", 1) && setting("stage_transition_ms", 300) > 0 && old != oldTops.end() ?
                    old->second - screen->geometry.cardTop(i, 0) : 0;
                card.shiftStart = Clock::now();
                if (card.shift != 0)
                    armMotion();
            }
        }
        const WORKSPACEID active = monitor->m_activeWorkspace ? monitor->m_activeWorkspace->m_id : WORKSPACE_INVALID;
        const bool changedActive = screen->active != active;
        screen->active = active;
        const auto mode = monitor->m_activeWorkspace ? Fullscreen::controller()->getFullscreenModes(monitor->m_activeWorkspace).internal : Fullscreen::FSMODE_NONE;
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
        const bool changedSuspension = screen->suspended != suspended;
        screen->suspended = suspended;
        if (suspended || cover || changedOutput || reconfigure || (!switching && (changedCards || changedGeometry)))
            screen->flights.clear();
        if (suspended || cover || changedOutput || reconfigure || changedMode || !screen->geometry.enabled()) {
            screen->paneTransition = false;
            screen->departingCards.clear();
        }
        if (changedGeometry || changedPolicy || changedCover || reconfigure)
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
    for (const auto& monitor : relayout) {
        g_layoutManager->invalidateMonitorGeometries(monitor);
        if (auto* screen = screenFor(monitor); screen && screen->geometry.enabled())
            correctFloating(monitor, desktop(*screen));
    }
    for (const auto& pending : pendingFlights) {
        if (auto* screen = screenFor(pending.monitor); screen && !screen->covered && !screen->suspended) {
            // Native layout owns the destination, but Stage owns its motion.
            // Resolve native layout animations once, after saving source boxes,
            // so a second position/size animation cannot continue after a flight.
            for (const auto& window : Desktop::windowState()->windows()) {
                if (window->m_monitor != pending.monitor || !window->m_isMapped || window->isHidden() || window->m_pinned || window->onSpecialWorkspace())
                    continue;
                window->positionAnimation()->warp();
                window->sizeAnimation()->warp();
            }
            startFlights(*screen, pending.previous, pending.cards, pending.geometry, pending.scroll, pending.right, pending.origins);
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
    if (motionTimer || !enabled)
        return;
    motionTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(16), [this](SP<CEventLoopTimer>, void*) {
        stopTimer(motionTimer);
        motion();
    }, nullptr);
    g_pEventLoopManager->addTimer(motionTimer);
}

void StageController::Impl::motion() {
    bool again = false;
    const auto now = Clock::now();
    if (swipe) {
        const auto monitor = swipe->monitor.lock();
        const auto* actual = screenFor(monitor);
        if (!monitor || !actual || blocked() || actual->covered || actual->suspended || (swipe->prepared && !sameBox(actual->base, swipe->visual.base)) ||
            (swipe->committed ? monitor->m_activeWorkspace != swipe->target : monitor->m_activeWorkspace != swipe->origin)) {
            clearSwipe();
        } else {
            if (swipe->released) {
                const double elapsed = std::chrono::duration<double, std::milli>(now - swipe->releaseStart).count();
                const double p = swipe->releaseDuration > 0 ? std::clamp(elapsed / swipe->releaseDuration, 0.0, 1.0) : 1;
                swipe->settleProgress = p;
                swipe->progress = swipe->releaseFrom + (swipe->releaseTo - swipe->releaseFrom) * p;
                if (p >= 1)
                    clearSwipe();
            }
            g_pHyprRenderer->damageMonitor(monitor);
            again = swipe.has_value();
        }
    }
    auto* drag = g_layoutManager->dragController().get();
    const bool dragging = drag && drag->mode() == MBIND_MOVE && drag->target();
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
        if (screen.shown != target) {
            const auto t = std::clamp(std::chrono::duration<double, std::milli>(now - screen.slideStart).count() / 180.0, 0.0, 1.0);
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
    if (again)
        armMotion();
}

std::pair<StageController::Impl::Screen*, std::optional<std::size_t>> StageController::Impl::hit(const Vector2D& point) {
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
        if (point.x >= band.x && point.x < band.x + band.w && point.y >= band.y && point.y < band.y + band.h)
            return {&screen, cardHit(screen, point)};
    }
    return {nullptr, std::nullopt};
}

void StageController::Impl::pointer() {
    if (!enabled || rendering)
        return;
    const auto [hoveredScreen, index] = hit(g_pInputManager->getMouseCoordsInternal());
    for (auto& screen : screens) {
        const auto hovered = &screen == hoveredScreen ? index : std::nullopt;
        if (screen.hovered != hovered) {
            screen.hovered = hovered;
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
}

void StageController::Impl::button(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
    if (rendering)
        return;
    // A consumed press owns its release even if a reload/overview/fullscreen
    // transition happened in between. Never send an unmatched release to a client.
    if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && swallowedButtons.erase(event.button)) {
        const auto workspace = pressed.lock();
        if (event.button == BTN_LEFT)
            pressed.reset();
        const auto [screen, index] = hit(g_pInputManager->getMouseCoordsInternal());
        info.cancelled = true;
        if (event.button == BTN_LEFT && workspace && screen && index && screen->cards[*index].workspace == workspace)
            activate(workspace);
        return;
    }
    // Releases belonging to a press outside the strip (including application
    // file/text drag-and-drop) must reach the existing grab unchanged.
    if (event.state == WL_POINTER_BUTTON_STATE_RELEASED || info.cancelled || g_pInputManager->hasHeldButtons())
        return;
    auto* drag = g_layoutManager->dragController().get();
    if (drag && drag->target())
        return; // native grab must receive its release exactly once
    const auto [screen, index] = hit(g_pInputManager->getMouseCoordsInternal());
    if (!screen)
        return;
    info.cancelled = true;
    swallowedButtons.insert(event.button);
    if (event.button == BTN_LEFT && event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        pressed = index ? screen->cards[*index].workspace : PHLWORKSPACEREF{};
    }
}

void StageController::Impl::axis(const IPointer::SAxisEvent& event, Event::SCallbackInfo& info) {
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

void StageController::Impl::activate(const PHLWORKSPACE& workspace) {
    if (!workspace || workspace->m_isSpecialWorkspace)
        return;
    const auto monitor = workspace->m_monitor.lock();
    if (!monitor)
        return;
    Desktop::focusState()->rawMonitorFocus(monitor);
    monitor->changeWorkspace(workspace, false, true);
    request();
}

void StageController::Impl::endDrag(Layout::Supplementary::CDragStateController* drag) {
    PHLWORKSPACE destination;
    PHLWINDOW window;
    Vector2D dropPoint;
    Vector2D grabOffset;
    if (enabled && !cancelDrop && drag->mode() == MBIND_MOVE && drag->dragThresholdReached() && drag->target()) {
        const auto [screen, index] = hit(g_pInputManager->getMouseCoordsInternal());
        if (screen && index) {
            destination = screen->cards[*index].workspace.lock();
            window = drag->target()->window();
            const auto pointer = g_pInputManager->getMouseCoordsInternal();
            const auto& geometry = screen->geometry;
            const auto band = sidebar(*screen);
            const auto area = desktop(*screen);
            const auto mapped = stage::mapDropPoint(
                {band.x + geometry.padding, screen->base.y + cardTop(*screen, *index), geometry.cardWidth, geometry.cardHeight},
                {area.x, area.y, area.w, area.h}, pointer.x, pointer.y);
            dropPoint = {mapped.first, mapped.second};
            if (window)
                grabOffset = pointer - window->positionAnimation()->value();
        }
    }
    reinterpret_cast<DragEndFn>(dragHook->m_original)(drag);
    // Use the compositor's normal move path after its drag controller has
    // restored tiling/floating and completed the pointer grab.
    if (destination && window && window->m_isMapped && window->m_workspace != destination &&
        State::workspaceState()->query().id(destination->m_id).run() == destination && destination->m_monitor) {
        const auto monitor = destination->m_monitor.lock();
        Desktop::globalWindowController()->moveWindowToWorkspace(window, destination);
        if (const auto target = window->layoutTarget(); target && target->space() == destination->m_space) {
            if (target->floating()) {
                target->setPositionGlobal(CBox{dropPoint - grabOffset, target->position().size()});
            } else {
                // Reinsert through the destination algorithm with a global
                // focal point; never warp the real cursor into a hidden desktop.
                destination->m_space->remove(target);
                destination->m_space->move(target, dropPoint);
            }
        }
        if (auto* screen = screenFor(monitor))
            correctFloating(monitor, desktop(*screen));
        if (setting("stage_drop_follow", 0)) {
            activate(destination);
            Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
        } else if (!destination->isVisible() && Desktop::focusState()->window() == window) {
            const auto focusedMonitor = Desktop::focusState()->monitor();
            const auto current = focusedMonitor ? focusedMonitor->m_activeWorkspace : nullptr;
            auto focus = current ? current->m_lastFocusedWindow.lock() : nullptr;
            if (!focus || focus == window || focus->m_workspace != current)
                focus = current ? current->getFirstWindow() : nullptr;
            Desktop::focusState()->fullWindowFocus(focus, Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
        }
    }
    lastDragged.reset();
    cancelDrop = false;
    request();
}

void StageController::Impl::correctFloating(const PHLMONITOR& monitor, const CBox& desktop) {
    const auto* drag = g_layoutManager->dragController().get();
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->m_isMapped || !window->m_isFloating || window->m_monitor != monitor || window->onSpecialWorkspace() || Fullscreen::controller()->isFullscreen(window) ||
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
    if (!enabled || rendering || blocked())
        return;
    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    auto* screen = visualScreenFor(monitor);
    if (!screen || !screen->geometry.enabled() || monitor->m_activeSpecialWorkspace || screen->shown <= 0)
        return;
    if (stage == RENDER_POST_WINDOWS) {
        const PHLMONITORREF ref = monitor;
        g_pHyprRenderer->m_renderPass.add(makeUnique<StagePassElement>([this, ref] {
            if (const auto mon = ref.lock())
                draw(mon);
        }, screen->flights.empty() && !screen->paneTransition ? sidebar(*screen).translate(-monitor->m_position)
                                  : CBox{{}, monitor->m_size}));
    }
}

void StageController::Impl::draw(const PHLMONITOR& monitor) {
    auto* screen = visualScreenFor(monitor);
    if (!screen || blocked() || rendering || !g_pHyprOpenGL)
        return;
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
    const auto previousClip = g_pHyprRenderer->m_renderData.clipBox;
    const CBox outputClip = physical(CBox{monitor->m_position, monitor->m_size});
    const auto drawPane = [&](const std::vector<Card>& cards, const stage::Geometry& geometry, double scroll, bool right, double offset) {
      const auto area = stage::sidebarArea({screen->base.x, screen->base.y, screen->base.w, screen->base.h}, geometry, right);
      const auto stripClip = physical(CBox{area.x + offset, area.y + geometry.paddingTop, area.width,
                                           area.height - geometry.paddingTop - geometry.paddingBottom}).intersection(outputClip);
      if (stripClip.w <= 0 || stripClip.h <= 0)
          return;
      for (std::size_t i = 0; i < cards.size(); ++i) {
        const auto& card = cards[i];
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - card.shiftStart).count();
        const double shiftProgress = swipe && &swipe->visual == screen ? stage::transitionProgress(swipe->progress, 1) :
            stage::transitionProgress(elapsed, std::clamp(setting("stage_transition_ms", 300), 0L, 2000L));
        const double top = geometry.cardTop(i, scroll) + card.shift * (1 - shiftProgress);
        if (top + geometry.cardHeight <= geometry.paddingTop || top >= screen->base.h - geometry.paddingBottom)
            continue;
        const auto workspace = card.workspace.lock();
        if (!workspace)
            continue;
        const CBox box{area.x + geometry.padding + offset, screen->base.y + top, geometry.cardWidth, geometry.cardHeight};
        if (card.previewsReady) {
            const auto cardClip = physical(box).intersection(stripClip);
            if (cardClip.w <= 0 || cardClip.h <= 0)
                continue;
            g_pHyprRenderer->m_renderData.clipBox = cardClip;
            for (const auto& preview : card.previews) {
                const auto window = preview.window.lock();
                if (!window || !window->m_isMapped || window->isHidden() || window->m_pinned || window->m_workspace != workspace || flying(*screen, window))
                    continue;
                const auto radius = stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1),
                    numberSetting("decoration:rounding", 0), preview.target.w, preview.target.h);
                drawPreview(window, monitor, preview.target.copy().translate(box.pos()), cardClip, radius);
            }
            g_pHyprRenderer->m_renderData.clipBox = stripClip;
        }
      }
    };
    if (screen->paneTransition) {
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - screen->paneStart).count();
        const double t = swipe && &swipe->visual == screen ? swipe->progress :
            screen->paneDuration > 0 ? std::clamp(elapsed / screen->paneDuration, 0.0, 1.0) : 1;
        const double p = t * t * (3 - 2 * t);
        const auto travel = [&](bool right, double width) {
            return right ? monitor->m_position.x + monitor->m_size.x - (screen->base.x + screen->base.w) + width :
                           screen->base.x - monitor->m_position.x + width;
        };
        drawPane(screen->departingCards, screen->departingGeometry, screen->departingScroll, screen->departingRight,
            (screen->departingRight ? 1 : -1) * travel(screen->departingRight, screen->departingGeometry.bandWidth) * p);
        drawPane(screen->cards, screen->geometry, screen->scroll, screen->right,
            (screen->right ? 1 : -1) * travel(screen->right, screen->geometry.bandWidth) * (1 - p));
    } else {
        const double slide = (screen->right ? 1 : -1) * (1 - screen->shown) * screen->geometry.bandWidth;
        drawPane(screen->cards, screen->geometry, screen->scroll, screen->right, slide);
    }
    g_pHyprRenderer->m_renderData.clipBox = previousClip;
    drawFlights(*screen, monitor);
}

CBox StageController::Impl::sidebar(const Screen& screen) const {
    const auto box = stage::sidebarArea({screen.base.x, screen.base.y, screen.base.w, screen.base.h}, screen.geometry, screen.right);
    return {box.x, box.y, box.width, box.height};
}

CBox StageController::Impl::desktop(const Screen& screen) const {
    const auto box = stage::desktopArea({screen.base.x, screen.base.y, screen.base.w, screen.base.h}, screen.geometry, screen.right);
    return {box.x, box.y, box.width, box.height};
}

double StageController::Impl::cardTop(const Screen& screen, std::size_t index) const {
    const auto& card = screen.cards[index];
    const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - card.shiftStart).count();
    const double p = stage::transitionProgress(elapsed, std::clamp(setting("stage_transition_ms", 300), 0L, 2000L));
    return screen.geometry.cardTop(index, screen.scroll) + card.shift * (1 - p);
}

std::optional<std::size_t> StageController::Impl::cardHit(const Screen& screen, const Vector2D& point) const {
    const auto& g = screen.geometry;
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
    return window && !window->m_pinned && std::ranges::any_of(screen.flights, [&](const auto& flight) { return flight.preview.window == window; });
}

void StageController::Impl::startFlights(Screen& screen, WORKSPACEID previous, const std::vector<Card>& oldCards,
                                       const stage::Geometry& oldGeometry, double oldScroll, bool oldRight,
                                       const std::vector<std::pair<PHLWINDOWREF, CBox>>& origins, PHLWORKSPACE visualActive, bool stopNative) {
    const auto monitor = screen.monitor.lock();
    const auto activeWorkspace = visualActive ? visualActive : monitor ? monitor->m_activeWorkspace : nullptr;
    const double duration = std::clamp(setting("stage_transition_ms", 300), 0L, 2000L);
    if (!monitor || duration == 0 || numberSetting("animations:enabled", 1) == 0 || !oldGeometry.enabled() || !screen.geometry.enabled()) {
        screen.flights.clear();
        return;
    }
    const double oldProgress = flightProgress(screen);
    auto previousFlights = std::move(screen.flights);
    std::vector<PHLWORKSPACE> workspaces;
    const auto addWorkspace = [&](const PHLWORKSPACE& workspace) {
        if (workspace && workspace->m_monitor == monitor && !workspace->m_isSpecialWorkspace && std::ranges::find(workspaces, workspace) == workspaces.end())
            workspaces.push_back(workspace);
    };
    addWorkspace(State::workspaceState()->query().id(previous).run());
    addWorkspace(activeWorkspace);
    for (const auto& flight : previousFlights) {
        if (const auto window = flight.preview.window.lock())
            addWorkspace(window->m_workspace);
    }
    const auto interpolate = [&](const CBox& from, const CBox& to, double p) {
        const auto r = stage::transitionBoxWithin({from.x, from.y, from.w, from.h}, {to.x, to.y, to.w, to.h}, p,
            flightBounds(monitor));
        return CBox{r.x, r.y, r.width, r.height};
    };
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
        return stage::previewRounding(numberSetting("plugin:hymission:stage_window_rounding", -1), numberSetting("decoration:rounding", 0), box.w, box.h);
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
            const auto from = old != previousFlights.end() ? std::optional<CBox>{interpolate(old->from, old->to, oldProgress)} :
                workspace->m_id == previous ? std::optional<CBox>{originalBox} : miniature(workspace, originalBox, oldCards, oldGeometry, oldScroll, oldRight, window, true);
            const auto goal = setting("stage_window_decorations", 0) ? preview.natural : CBox{window->positionAnimation()->goal(), window->sizeAnimation()->goal()};
            const auto to = workspace == activeWorkspace ? std::optional<CBox>{preview.natural} :
                miniature(workspace, goal, screen.cards, screen.geometry, screen.scroll, screen.right, window, false);
            if (!from || !to)
                continue;
            const auto nativeRadius = window->rounding();
            const double fromRadius = old != previousFlights.end() ? old->fromRadius + (old->toRadius - old->fromRadius) * stage::transitionProgress(oldProgress, 1) :
                workspace->m_id == previous ? nativeRadius : radius(*from);
            const double toRadius = workspace == activeWorkspace ? nativeRadius : radius(*to);
            screen.flights.push_back({std::move(preview), *from, *to, static_cast<float>(fromRadius), static_cast<float>(toRadius)});
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

void StageController::Impl::drawPreview(const PHLWINDOW& window, const PHLMONITOR& monitor, const CBox& target, const CBox& clip, double radius) {
    const auto root = window->wlSurface()->resource();
    if (!root || target.w <= 0 || target.h <= 0)
        return;
    const CBox source{window->positionAnimation()->value(), window->sizeAnimation()->value()};
    if (source.w <= 0 || source.h <= 0)
        return;
    const bool decorations = setting("stage_window_decorations", 0) != 0;
    const CBox footprint = decorations ? window->getFullWindowBoundingBox() : source;
    if (footprint.w <= 0 || footprint.h <= 0)
        return;
    const double scale = std::min(target.w / footprint.w, target.h / footprint.h);
    const CBox fitted{target.middle() - footprint.size() * scale / 2, footprint.size() * scale};
    radius = std::clamp(radius, 0.0, std::min(source.w, source.h) * scale / 2);
    auto& state = g_pHyprRenderer->m_renderData;
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
            (monitor->m_position - footprint.pos() - offset - window->m_floatingOffset) * monitor->m_scale);
        transform.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, static_cast<float>(scale));
        transform.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, (fitted.pos() - monitor->m_position) * monitor->m_scale);
        state.renderModif = transform;
        state.currentWindow = window;
        drawingDecoration = true;
        decorationRadius = radius / scale;
        decorationClip = clip;
        for (const auto& decoration : window->m_windowDecorations) {
            const auto layer = decoration->getDecorationLayer();
            if (below == (layer == DECORATION_LAYER_BOTTOM || layer == DECORATION_LAYER_UNDER))
                decoration->draw(monitor, 1.F);
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
    data.roundingPower = window->roundingPower();
    data.blur = shouldBlur && shouldBlur(g_pHyprRenderer.get(), window);
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
        g_pHyprRenderer->draw(data, state.damage);
        ++data.surfaceCounter;
    }, nullptr);
    drawDecorations(false);
}

void StageController::Impl::drawFlights(Screen& screen, const PHLMONITOR& monitor) {
    if (screen.flights.empty() || screen.covered || screen.suspended)
        return;
    const auto previousClip = g_pHyprRenderer->m_renderData.clipBox;
    const double p = flightProgress(screen);
    const auto bounds = flightBounds(monitor);
    const CBox clip{(bounds.x - monitor->m_position.x) * monitor->m_scale,
                    (bounds.y - monitor->m_position.y) * monitor->m_scale,
                    bounds.width * monitor->m_scale, bounds.height * monitor->m_scale};
    for (auto& flight : screen.flights) {
        const auto window = flight.preview.window.lock();
        if (!window || !window->m_isMapped || window->isHidden() || window->m_pinned || window->m_monitor != monitor)
            continue;
        if ((!swipe || &swipe->visual != &screen) && window->m_workspace == monitor->m_activeWorkspace)
            flight.to = setting("stage_window_decorations", 0) ? window->getFullWindowBoundingBox() : CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
        const auto box = stage::transitionBoxWithin({flight.from.x, flight.from.y, flight.from.w, flight.from.h},
            {flight.to.x, flight.to.y, flight.to.w, flight.to.h}, p,
            bounds);
        drawPreview(window, monitor, CBox{box.x, box.y, box.width, box.height}, clip,
            flight.fromRadius + (flight.toRadius - flight.fromRadius) * stage::transitionProgress(p, 1));
    }
    g_pHyprRenderer->m_renderData.clipBox = previousClip;
}

void StageController::Impl::refreshPreviews() {
    if (!enabled || blocked() || rendering || g_pHyprRenderer->m_renderData.pMonitor)
        return;
    for (auto& screen : screens) {
        if (!interactive(screen))
            continue;
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
        if (!window || !window->m_isMapped || window->isHidden() || window->m_pinned)
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
            if (!interactive(screen))
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
            window->setSuspended(window->isHidden() || !window->m_workspace || !window->m_workspace->isVisible());
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
        if (!window->m_isMapped || window->isHidden() || window->onSpecialWorkspace() || window->m_pinned)
            continue;
        if (window->m_workspace == workspace) {
            const auto box = decorations ? window->getFullWindowBoundingBox() : CBox{window->positionAnimation()->value(), window->sizeAnimation()->value()};
            if (box.w <= 0 || box.h <= 0)
                continue;
            // A side change moves native tiled geometry. Hidden cards should
            // depict its destination, not bake the intermediate layout slide
            // into a thumbnail that persists until the next refresh.
            const auto offset = (smartisan ? window->positionAnimation()->goal() - window->positionAnimation()->value() : Vector2D{}) +
                (window->m_isFloating ? Vector2D{} : tiledOffset);
            inputs.push_back(WindowInput{.index = windows.size(), .natural = Rect{box.x + offset.x, box.y + offset.y, box.w, box.h}});
            boxes.push_back(box);
            windows.push_back(window);
        }
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
    stopTimer(refreshTimer);
    stopTimer(motionTimer);
    listeners.clear();
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
    nlohmann::json result{{"enabled", enabled}, {"smartisan_mode", smartisan}, {"hooks_ready", hooksReady}, {"preview_renderer", "native_surfaces"},
        {"refresh_interval_ms", std::clamp(setting("stage_refresh_ms", 16), 1L, 16L)}, {"error", error}, {"screens", nlohmann::json::array()}};
    for (const auto& screen : screens) {
        const auto monitor = screen.monitor.lock();
        if (!monitor)
            continue;
        const auto& rendered = swipe && swipe->prepared && swipe->monitor == monitor ? swipe->visual : screen;
        nlohmann::json cards = nlohmann::json::array();
        for (const auto& card : screen.cards) {
            if (const auto workspace = card.workspace.lock())
                cards.push_back({{"workspace", workspace->m_id}, {"name", workspace->m_name}, {"previews_ready", card.previewsReady}});
        }
        result["screens"].push_back({{"monitor", monitor->m_name}, {"card_width", screen.geometry.cardWidth}, {"card_height", screen.geometry.cardHeight},
                                     {"preview_render_fps", Clock::now() - rendered.lastFrame < std::chrono::seconds(1) ? rendered.previewRenderFPS : 0},
                                     {"sidebar_side", screen.right ? "right" : "left"}, {"sidebar_transition", screen.paneTransition},
                                     {"swipe_active", swipe && swipe->monitor == monitor},
                                     {"swipe_progress", swipe && swipe->monitor == monitor ? swipe->progress : 0},
                                     {"transition_windows", rendered.flights.size()}, {"transition_progress", rendered.flights.empty() ? 1.0 : flightProgress(rendered)},
                                     {"desktop_width", screen.geometry.desktopWidth}, {"desktop_height", screen.base.h}, {"reservation", screen.geometry.reservation},
                                     {"scroll", screen.scroll}, {"max_scroll", screen.geometry.maxScroll}, {"covered", screen.covered}, {"suspended", screen.suspended},
                                     {"cards", cards}});
    }
    return result.dump() + "\n";
}

StageController::StageController(HANDLE handle, std::function<bool()> suspended) : m_impl(std::make_unique<Impl>(handle, std::move(suspended))) {}
StageController::~StageController() = default;
void StageController::initialize() { m_impl->initialize(); }
std::string StageController::stateJson() const { return m_impl->stateJson(); }

bool StageController::beginWorkspaceSwipe(void* gesture, void (*original)(void*)) {
    auto* self = Impl::instance;
    const auto monitor = Desktop::focusState()->monitor();
    if (!self || !original || !monitor || !self->ownsTransition(monitor->m_activeWorkspace))
        return false;
    self->swipeBeginOriginal = reinterpret_cast<Impl::SwipeBeginFn>(original);
    Impl::swipeBeginThunk(static_cast<CUnifiedWorkspaceSwipeGesture*>(gesture));
    return true;
}

bool StageController::updateWorkspaceSwipe(void* gesture, double delta) {
    auto* self = Impl::instance;
    if (!self || !self->swipe || self->swipe->native != gesture)
        return false;
    self->updateSwipe(delta);
    return true;
}

bool StageController::endWorkspaceSwipe(void* gesture) {
    auto* self = Impl::instance;
    if (!self || !self->swipe || self->swipe->native != gesture)
        return false;
    self->endSwipe();
    return true;
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

} // namespace hymission
