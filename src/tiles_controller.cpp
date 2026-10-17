#include "tiles_controller.hpp"
#include "hook_broker.hpp"
#include "workspace_key.hpp"
#include "render_context.hpp"
#include "stage_controller.hpp"
#include "state_json.hpp"
#include "tiles_symbols.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/render/scene/SceneSelection.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/workspace/PlacementController.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <linux/input-event-codes.h>
#define private public
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#undef private
#include <hyprland/src/managers/fullscreen/handler/FullscreenHandler.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>

namespace hymission {
namespace {
TilesController* instance = nullptr;
bool locked() { return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked(); }
long integer(const char* name, long fallback) {
    const auto v = Config::mgr()->getConfigValue(std::string("plugin:hymission:") + name);
    return v.dataptr && v.type && *v.type == typeid(Config::INTEGER) ? **reinterpret_cast<Config::INTEGER* const*>(v.dataptr) : fallback;
}
CBox box(tiles::Box b) { return {b.x, b.y, b.width, b.height}; }
tiles::Box geometry(PHLMONITOR m) { return {m->m_position.x, m->m_position.y, m->m_size.x, m->m_size.y}; }
bool pinned(PHLWINDOW w) { return w && static_cast<bool>(w->m_state & Desktop::View::WINDOW_STATE_PINNED); }
}
struct TilesController::Impl {
    struct Output {
        PHLMONITORREF monitor;
        tiles::Model model;
        std::vector<PHLWORKSPACE> held;
        Output(PHLMONITOR m, double minimum) : monitor(m), model(geometry(m), workspaceKey(m->m_activeWorkspace), minimum) {}
    };
    HANDLE handle;
    std::function<bool()> overview;
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<CHyprSignalListener> listeners;
    std::vector<hooks::Lease*> leases;
    UP<SEventLoopDoLaterLock> deferred;
    bool ready = false, applying = false, frameFanout = false, stopping = false;
    std::string initError, edgeFailure;
    bool softEdgeDrawn=false;
    Owner interaction = Owner::None;
    struct Swipe {
        PHLMONITORREF monitor;
        tiles::Id tile = 0;
        uint64_t generation = 0;
        PHLWORKSPACE source, target;
        Vector2D sourceOffset, targetOffset, sourcePreview, targetPreview;
        bool targetForced = false, vertical = false;
        double delta = 0, speed = 0;
        int direction = 0;
        CUnifiedWorkspaceSwipeGesture* native = nullptr;
    };
    std::optional<Swipe> swipe;
    struct Resize { PHLMONITORREF monitor; tiles::DividerDrag drag; double pointerX = 0; };
    std::optional<Resize> resize;
    UP<SEventLoopDoLaterLock> resizeDeferred;
    bool resizeReleasePending = false;
    Impl(HANDLE h, std::function<bool()> o) : handle(h), overview(std::move(o)) {}
    Output* output(PHLMONITOR mon) const {
        for (auto& o : outputs) if (o->monitor == mon) return o.get();
        return nullptr;
    }
    bool active(PHLMONITOR mon) const { const auto* o = output(mon); return ready && o && o->model.state().tiles.size() > 1; }
    std::optional<View> view(PHLWORKSPACE w) const {
        if (!w) return {};
        auto* o = output(w->m_monitor.lock());
        if (!ready || !o || o->model.state().tiles.size() <= 1) return {};
        auto i = o->model.presented(workspaceKey(w));
        if (!i && swipe && swipe->target == w && swipe->monitor == w->m_monitor) i = o->model.index(swipe->tile);
        if (!i) return {};
        const auto& t = o->model.state().tiles[*i];
        return View{o->monitor.lock(), w, t.id, box(o->model.box(*i)), t.id == o->model.state().focused};
    }
    bool ensure();
    bool install();
    void resetHooks() { for (auto* l : leases) hooks::remove(handle, l); leases.clear(); ready = false; }
    void reconcile();
    void request() {
        if (stopping || deferred || !g_pEventLoopManager) return;
        deferred = g_pEventLoopManager->doLaterLock([this] { deferred.reset(); reconcile(); });
    }
    void apply(Output& o, bool focusWindow = false, bool warp = false);
    void collapse();
    void cancelSwipe();
    void flushResize() {
        resizeDeferred.reset();
        if (!resize) return;
        auto* o=output(resize->monitor.lock());
        if (!o || !resize->drag.update(o->model,resize->pointerX)) { resize.reset(); interaction=Owner::None; return; }
        apply(*o);
    }
    void endResize(bool cancel) {
        if (!resize) return;
        resizeDeferred.reset();
        auto r=std::move(*resize); resize.reset();
        if (auto* o=output(r.monitor.lock()); cancel && o && r.drag.cancel(o->model)) apply(*o);
        interaction=Owner::None;
    }
    void pointer(const Vector2D& pos, Event::SCallbackInfo& info) {
        if (!resize) return;
        info.cancelled=true;
        resize->pointerX=pos.x;
        if (!resizeDeferred) resizeDeferred=g_pEventLoopManager->doLaterLock([this] { flushResize(); });
    }
    bool dividerButton(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        if (event.button!=BTN_LEFT) return false;
        if (event.state==WL_POINTER_BUTTON_STATE_RELEASED && resizeReleasePending) {
            flushResize(); endResize(false); resizeReleasePending=false; info.cancelled=true; return true;
        }
        if (info.cancelled || locked() || event.state!=WL_POINTER_BUTTON_STATE_PRESSED || interaction!=Owner::None || (overview && overview()) ||
            g_pInputManager->pointerConstraints().nativeActive() || g_pInputManager->pointerConstraints().blocksAbsoluteMotion()) return false;
        const auto pos=g_pInputManager->getMouseCoordsInternal();
        auto mon=State::monitorState()->query().vec(pos).run();
        if (!active(mon) || mon->m_activeSpecialWorkspace || !mon->logicalBoxMinusReserved().containsPoint(pos)) return false;
        auto* o=output(mon);
        const auto slop=std::clamp(static_cast<double>(integer("tile_divider_hit_width",8)),1.0,o->model.state().minimum/2)/2;
        for (size_t i=0;i+1<o->model.state().tiles.size();++i) {
            const auto b=o->model.box(i);
            if (std::abs(pos.x-b.x-b.width)>slop) continue;
            auto d=tiles::DividerDrag::begin(o->model,i,pos.x);
            if (!d) return false;
            resize=Resize{mon,*d,pos.x}; resizeReleasePending=true; interaction=Owner::Resize;
            g_pSeatManager->setPointerFocus(nullptr,{});
            info.cancelled=true; return true;
        }
        return false;
    }
    void chrome(Event::SRenderStageEvent event) {
        if (!event.context || event.stage!=RENDER_POST_WINDOWS || !active(event.monitor) || locked()) return;
        auto mon=event.monitor;
        auto& ctx=event.context->get();
        const auto* o=output(mon);
        for (size_t i=0;i+1<o->model.state().tiles.size();++i) {
            const auto b=o->model.box(i);
            CRectPassElement::SRectData data;
            data.box=CBox{b.x+b.width-mon->m_position.x-0.5,b.y-mon->m_position.y,1.0,b.height}.scale(mon->m_scale);
            data.color=CHyprColor(0.42,0.64,0.95,interaction==Owner::Resize?0.95:0.4);
            g_pHyprRenderer->addPassElement(ctx,makeUnique<CRectPassElement>(data));
        }
    }
    void notify(Output& o, PHLWORKSPACE old);
    PHLWORKSPACE fresh(PHLMONITOR mon) {
        WorkspaceKey key = 1;
        while (workspaceForKey(key) && key < UINT32_MAX) ++key;
        return key < UINT32_MAX ? createWorkspaceForKey(key, mon->m_id) : nullptr;
    }
    template<class Fn> bool attach(const char* key, Fn fn, hooks::Lease*& lease) {
        const auto sig = tiles::symbols::lookup(key);
        void* source = nullptr;
        size_t count = 0;
        for (const auto& match : HyprlandAPI::findFunctionsByName(handle, std::string(sig.substr(0, sig.find('('))).substr(std::string(sig.substr(0, sig.find('('))).rfind("::") + 2))) if (match.demangled == sig) { source = match.address; ++count; }
        if (count != 1) { initError = std::string("tiles disabled: expected one exact symbol: ") + std::string(sig); return false; }
        lease = hooks::create(handle, source, hooks::target(fn));
        if (lease) leases.push_back(lease);
        if (!lease || !lease->hook()) { initError = std::string("tiles disabled: hook attach failed: ") + std::string(sig); return false; }
        return true;
    }
    inline static Impl* self = nullptr;
    inline static hooks::Lease *areaHook, *hitHook, *focusHook, *changeHook, *visibleHook, *windowHook, *normalHook, *fullHook, *framesHook, *fsSetHook, *fsSyncHook;
    inline static hooks::Lease *typedHook, *typedCurrentHook, *stringHook, *stringCurrentHook, *resolverHook, *previousHook, *previousMonitorHook,
        *directionHook, *moveHook, *placementHook, *swapHook, *swipeBeginHook, *swipeUpdateHook, *swipeEndHook, *swipeCancelHook;

    static void area(Layout::CSpace* space) {
        reinterpret_cast<decltype(&area)>(areaHook->m_original)(space);
        const auto v = self->view(space->workspace());
        if (!v) return;
        const auto base = v->monitor->logicalBoxMinusReserved();
        const auto target = v->allocation.intersection(base);
        for (auto* b : {&space->m_workArea, &space->m_floatingWorkArea}) {
            const double left = std::max(0.0, b->x - base.x), top = std::max(0.0, b->y - base.y);
            const double right = std::max(0.0, base.x + base.w - b->x - b->w), bottom = std::max(0.0, base.y + base.h - b->y - b->h);
            *b = CBox{target.x + left, target.y + top, std::max(1.0, target.w - left - right), std::max(1.0, target.h - top - bottom)};
        }
    }
    static PHLWINDOW hit(const Desktop::CViewHitTester* tester, const Vector2D& pos, uint16_t flags, PHLWINDOW ignore) {
        const auto original = reinterpret_cast<decltype(&hit)>(hitHook->m_original);
        if (locked() || (self->overview && self->overview())) return original(tester, pos, flags, ignore);
        const auto mon = State::monitorState()->query().vec(pos).run();
        auto* o = self->output(mon);
        if (!self->active(mon) || mon->m_activeSpecialWorkspace) return original(tester, pos, flags, ignore);
        const auto candidate = original(tester, pos, flags, ignore);
        if (pinned(candidate)) return candidate;
        const auto i = o->model.at(pos.x, pos.y);
        return i ? tester->windowAtWorkspace(pos, workspaceForKey(o->model.state().tiles[*i].workspace), flags, ignore) : nullptr;
    }
    static void rawFocus(Desktop::CFocusState* state, PHLWINDOW window, Desktop::eFocusReason reason, SP<CWLSurfaceResource> surface) {
        if (!locked() && !self->applying && !pinned(window)) {
            if (const auto v = self->view(window ? window->m_workspace : nullptr)) {
                auto* o = self->output(v->monitor);
                if (!v->focused) { if (o->model.activate(workspaceKey(v->workspace))) self->apply(*o); }
            }
        }
        reinterpret_cast<decltype(&rawFocus)>(focusHook->m_original)(state, std::move(window), reason, std::move(surface));
    }
    static void change(Monitor::CMonitor* mon, const PHLWORKSPACE& ws, bool internal, bool noMouseMove, bool noFocus) {
        auto monitor = mon->m_self.lock();
        if (!self->active(monitor) || locked() || self->applying || !ws || ws->type() == Workspace::eWorkspaceType::SPECIAL) {
            reinterpret_cast<decltype(&change)>(changeHook->m_original)(mon, ws, internal, noMouseMove, noFocus); return;
        }
        if (ws->m_monitor != monitor) return; // placement is handled before this boundary
        auto* o = self->output(monitor);
        if (o->model.activate(workspaceKey(ws))) self->apply(*o, !internal && !noFocus, !noMouseMove);
    }
    static bool visible(const Workspace::CHLWorkspace* ws) {
        if (!locked() && self->view(const_cast<Workspace::CHLWorkspace*>(ws)->m_self.lock())) return !ws->m_monitor->m_activeSpecialWorkspace;
        return reinterpret_cast<decltype(&visible)>(visibleHook->m_original)(ws);
    }
    static void window(Render::IHyprRenderer* r, Render::CRenderContext& ctx, PHLWINDOW w, PHLMONITOR mon,
                       const Render::SWindowRenderPresentation& p, const Time::steady_tp& now, bool decorate, Render::eRenderPassMode mode, bool ignore, bool standalone) {
        const auto original = reinterpret_cast<decltype(&window)>(windowHook->m_original);
        const auto v = self->view(w ? w->m_workspace : nullptr);
        if (!v || v->monitor != mon || pinned(w) || ignore || standalone || ctx.m_renderingSnapshot || (self->overview && self->overview())) {
            original(r, ctx, std::move(w), std::move(mon), p, now, decorate, mode, ignore, standalone); return;
        }
        RenderContextScope scope{ctx};
        const Rect clip{v->allocation.x, v->allocation.y, v->allocation.w, v->allocation.h};
        auto edge=stage::edgeViewport(clip,0,0);
        const auto algorithm=w->m_workspace->space()->algorithm();
        const bool scrolling=algorithm && algorithm->tiledAlgo() && Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(&typeid(*algorithm->tiledAlgo()))=="scrolling";
        if (scrolling && !w->isFloating() && !Fullscreen::controller()->isFullscreen(w)) {
            const double width=std::max(0L,integer("stage_scrolling_desktop_edge_width",32));
            if (width>0 && StageController::ensureEdgeService()) {
                const auto pos=w->positionAnimation()->value()+w->m_workspace->m_renderOffset->value()+w->presentation().floatingOffset();
                const auto size=w->sizeAnimation()->value();
                edge=stage::edgeViewportForWindow(stage::edgeViewport(clip,width,width,width,width),{pos.x,pos.y,size.x,size.y});
            } else if (width>0) self->edgeFailure="native blur backdrop hook unavailable; hard clip retained";
        }
        StageController::renderInViewport(mon, edge, clip, [&] { original(r, ctx, w, mon, p, now, decorate, mode, ignore, standalone); });
    }
    using SceneFn = void (*)(Render::IHyprRenderer*, Render::CRenderContext&, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, Render::eSceneMode);
    static void scenes(bool full, Render::IHyprRenderer* r, Render::CRenderContext& ctx, PHLMONITOR mon, PHLWORKSPACE ws, const Time::steady_tp& now, Render::eSceneMode mode) {
        auto original = reinterpret_cast<SceneFn>((full ? fullHook : normalHook)->m_original);
        if (!self->active(mon) || !ws || ws->type() == Workspace::eWorkspaceType::SPECIAL || mode != Render::eSceneMode::MONITOR || (self->overview && self->overview())) {
            original(r, ctx, mon, ws, now, mode); return;
        }
        for (const auto& t : self->output(mon)->model.state().tiles) {
            auto w = workspaceForKey(t.workspace);
            if (!w) continue;
            const auto fn = reinterpret_cast<SceneFn>((Fullscreen::controller()->hasFullscreen(w) ? fullHook : normalHook)->m_original);
            fn(r, ctx, mon, w, now, Render::eSceneMode::WORKSPACE_WINDOWS);
        }
        if (self->swipe && self->swipe->monitor == mon && self->swipe->target) {
            auto w = self->swipe->target;
            const auto fn = reinterpret_cast<SceneFn>((Fullscreen::controller()->hasFullscreen(w) ? fullHook : normalHook)->m_original);
            fn(r, ctx, mon, w, now, Render::eSceneMode::WORKSPACE_WINDOWS);
        }
    }
    static void normal(Render::IHyprRenderer* r, Render::CRenderContext& c, PHLMONITOR m, PHLWORKSPACE w, const Time::steady_tp& t, Render::eSceneMode mode) { scenes(false, r, c, m, w, t, mode); }
    static void full(Render::IHyprRenderer* r, Render::CRenderContext& c, PHLMONITOR m, PHLWORKSPACE w, const Time::steady_tp& t, Render::eSceneMode mode) { scenes(true, r, c, m, w, t, mode); }
    static void frames(Render::IHyprRenderer* r, PHLMONITOR mon, PHLWORKSPACE ws, const Time::steady_tp& now) {
        const auto fn = reinterpret_cast<decltype(&frames)>(framesHook->m_original);
        if (!self->active(mon) || self->frameFanout || ws != mon->m_activeWorkspace) { fn(r, mon, ws, now); return; }
        self->frameFanout = true;
        Hyprutils::Utils::CScopeGuard reset{[] { self->frameFanout = false; }};
        for (const auto& t : self->output(mon)->model.state().tiles) if (auto w = workspaceForKey(t.workspace)) fn(r, mon, w, now);
    }
    static void clampFullscreen(Fullscreen::IFullscreenHandler* handler, const SP<Layout::ITarget>& target) {
        if (!target) return;
        const auto v = self->view(target->workspace());
        const auto mode = handler->getFullscreenModes(target).internal;
        if (!v || mode == Fullscreen::FSMODE_NONE) return;
        target->setPositionGlobal(mode == Fullscreen::FSMODE_FULLSCREEN ? v->allocation : target->space()->workArea(target->floating()));
    }
    static void fsSet(Fullscreen::IFullscreenHandler* handler, SP<Layout::ITarget> target) {
        reinterpret_cast<decltype(&fsSet)>(fsSetHook->m_original)(handler, target); clampFullscreen(handler, target);
    }
    static void fsSync(Fullscreen::IFullscreenHandler* handler) {
        reinterpret_cast<decltype(&fsSync)>(fsSyncHook->m_original)(handler); clampFullscreen(handler, handler->getFullscreen());
    }

    using Action = Config::Actions::ActionResult;
    Action route(PHLWORKSPACE ws, bool current) {
        auto mon = Desktop::focusState()->monitor();
        auto* o = output(mon);
        if (!active(mon) || !ws || ws->type() == Workspace::eWorkspaceType::SPECIAL || applying || locked()) {
            const auto fn = reinterpret_cast<Action (*)(PHLWORKSPACE)>((current ? typedCurrentHook : typedHook)->m_original);
            return fn(std::move(ws));
        }
        if (resize) endResize(true);
        if (interaction != Owner::None) cancelSwipe();
        for (auto& other : outputs) if (const auto i = other->model.presented(workspaceKey(ws))) {
            if (other->monitor != mon && current) {
                (void)other->model.activate(workspaceKey(ws));
                apply(*other);
                State::Workspace::placementController()->swapActiveWorkspaces(mon,other->monitor.lock());
                apply(*o,true);
            } else if (other->monitor == mon && current) {
                if (!o->model.activate(workspaceKey(ws), tiles::Activation::Swap)) return Config::Actions::actionError("tile swap failed");
                apply(*o, true);
            } else {
                Desktop::focusState()->rawMonitorFocus(other->monitor.lock());
                if (!other->model.activate(workspaceKey(ws))) return Config::Actions::actionError("tile focus failed");
                static auto warp = CConfigValue<Config::INTEGER>("cursor:warp_on_monitor_change");
                apply(*other, true, *warp > 0);
            }
            return {};
        }
        if (ws->m_monitor != mon) {
            const auto fn = reinterpret_cast<Action (*)(PHLWORKSPACE)>((current ? typedCurrentHook : typedHook)->m_original);
            return fn(std::move(ws));
        }
        if (mon->m_activeSpecialWorkspace) {
            static auto hide = CConfigValue<Config::INTEGER>("binds:hide_special_on_workspace_change");
            if (!*hide) return Config::Actions::actionError("special workspace blocks tile activation");
            mon->setSpecialWorkspace(nullptr);
        }
        if (!o->model.activate(workspaceKey(ws))) return Config::Actions::actionError("tile activation failed");
        static auto warp = CConfigValue<Config::INTEGER>("cursor:warp_on_change_workspace");
        apply(*o, true, *warp > 0);
        return {};
    }
    static Action typed(PHLWORKSPACE ws) { return self->route(std::move(ws), false); }
    static Action typedCurrent(PHLWORKSPACE ws) { return self->route(std::move(ws), true); }
    static Action actionString(const std::string& selector, bool current) {
        auto mon = Desktop::focusState()->monitor();
        if (!self->active(mon) || locked() || self->applying || (self->overview && self->overview()))
            return reinterpret_cast<Action (*)(const std::string&)>((current ? stringCurrentHook : stringHook)->m_original)(selector);
        auto target = State::Workspace::resolver()->getWorkspaceTargetFromString(selector, mon);
        if (!target.valid()) return Config::Actions::actionError("invalid tile workspace selector");
        auto ws = State::workspaceState()->find(target);
        static auto back = CConfigValue<Config::INTEGER>("binds:workspace_back_and_forth");
        if (ws == mon->m_activeWorkspace && *back) {
            if (const auto previous = self->output(mon)->model.previous()) target = workspaceTarget(*previous);
            ws = State::workspaceState()->find(target);
        }
        if (!ws) ws = State::workspaceState()->create(target, mon);
        return self->route(std::move(ws), current);
    }
    static Action stringAction(const std::string& s) { return actionString(s, false); }
    static Action stringCurrent(const std::string& s) { return actionString(s, true); }
    static State::Workspace::STarget resolve(State::Workspace::CWorkspaceResolver* resolver, const std::string& selector, std::optional<PHLMONITOR> base) {
        const auto mon = base.value_or(Desktop::focusState()->monitor());
        if (self->active(mon) && (selector == "prev" || selector == "previous" || selector == "previous_per_monitor")) {
            if (const auto prev = self->output(mon)->model.previous()) return workspaceTarget(*prev);
            return {};
        }
        return reinterpret_cast<decltype(&resolve)>(resolverHook->m_original)(resolver, selector, base);
    }
    using History = Desktop::History::CWorkspaceHistoryTracker;
    static std::optional<History::SHistoryEntry> history(PHLWORKSPACE ws) {
        if (const auto v = self->view(ws)) {
            const auto* o = self->output(v->monitor);
            const auto i = o->model.index(v->id);
            if (i) for (const auto key : o->model.state().tiles[*i].history) if (key != workspaceKey(ws)) return History::SHistoryEntry{workspaceForKey(key), v->monitor, workspaceTarget(key)};
            return History::SHistoryEntry{};
        }
        return {};
    }
    static const History::SHistoryEntry previous(History* tracker, PHLWORKSPACE ws) {
        if (const auto h = history(ws)) return *h;
        return reinterpret_cast<decltype(&previous)>(previousHook->m_original)(tracker, ws);
    }
    static const History::SHistoryEntry previousMonitor(History* tracker, PHLWORKSPACE ws, PHLMONITOR mon) {
        if (ws && ws->m_monitor == mon) if (const auto h = history(ws)) return *h;
        return reinterpret_cast<decltype(&previousMonitor)>(previousMonitorHook->m_original)(tracker, ws, mon);
    }
    static Action direction(Math::eDirection dir) {
        const auto mon = Desktop::focusState()->monitor();
        const auto w = Desktop::focusState()->window();
        if (self->active(mon) && !mon->m_activeSpecialWorkspace && (dir == Math::DIRECTION_LEFT || dir == Math::DIRECTION_RIGHT) &&
            (!w || !Desktop::windowState()->query().inDirection(w, dir))) {
            static auto fallback = CConfigValue<Config::INTEGER>("binds:window_direction_monitor_fallback");
            auto* o = self->output(mon);
            const auto before = o->model.state().focused;
            if (*fallback && o->model.focus(dir == Math::DIRECTION_LEFT ? -1 : 1) && o->model.state().focused != before) { self->apply(*o, true, true); return {}; }
        }
        return reinterpret_cast<decltype(&direction)>(directionHook->m_original)(dir);
    }
    static Action moveDirection(Math::eDirection dir, std::optional<PHLWINDOW> window) {
        const auto w = window.value_or(Desktop::focusState()->window());
        const auto mon = w ? w->m_monitor.lock() : nullptr;
        if (w && self->active(mon) && !mon->m_activeSpecialWorkspace && !Fullscreen::controller()->isFullscreen(w) &&
            (dir == Math::DIRECTION_LEFT || dir == Math::DIRECTION_RIGHT) && !Desktop::windowState()->query().inDirection(w, dir)) {
            auto* o = self->output(mon);
            const int i = static_cast<int>(*o->model.index(o->model.state().focused)) + (dir == Math::DIRECTION_LEFT ? -1 : 1);
            if (i >= 0 && i < static_cast<int>(o->model.state().tiles.size()) && instance->moveWindow(w, mon, o->model.state().tiles[i].id)) return {};
        }
        return reinterpret_cast<decltype(&moveDirection)>(moveHook->m_original)(dir, window);
    }
    static void placement(const State::Workspace::CPlacementController* p, PHLWORKSPACE ws, PHLMONITOR mon, bool noWarp, bool carryFocus) {
        if (!ws || !mon || self->applying || locked() || ws->type() == Workspace::eWorkspaceType::SPECIAL) {
            reinterpret_cast<decltype(&placement)>(placementHook->m_original)(p, ws, mon, noWarp, carryFocus); return;
        }
        if (const auto v = self->view(ws); v && v->monitor != mon) {
            auto* o = self->output(v->monitor);
            const auto originalFocus=o->model.state().focused;
            if (auto replacement = self->fresh(v->monitor)) {
                auto state = o->model.state(); state.focused = v->id;
                (void)o->model.restore(state, o->model.state().generation);
                (void)o->model.activate(workspaceKey(replacement));
                o->held.push_back(replacement);
                state=o->model.state(); state.focused=originalFocus;
                (void)o->model.restore(state,o->model.state().generation);
                self->apply(*o);
            }
        }
        reinterpret_cast<decltype(&placement)>(placementHook->m_original)(p, ws, mon, noWarp, carryFocus);
        if (carryFocus && self->active(mon)) { auto* o=self->output(mon); (void)o->model.activate(workspaceKey(ws)); self->apply(*o, true, !noWarp); }
        self->request();
    }
    static void swapPhysical(const State::Workspace::CPlacementController* p, PHLMONITOR a, PHLMONITOR b) {
        reinterpret_cast<decltype(&swapPhysical)>(swapHook->m_original)(p,a,b);
        for (const auto& mon : {a,b}) if (self->active(mon) && mon->m_activeWorkspace) { auto* o=self->output(mon); (void)o->model.activate(workspaceKey(mon->m_activeWorkspace)); self->apply(*o); }
    }
    PHLWORKSPACE swipeTarget(Output& o, int step) {
        const auto mon = o.monitor.lock();
        std::vector<PHLWORKSPACE> candidates;
        rememberWorkspaceKeys();
        for (const auto& w : State::workspaceState()->workspacesCopy()) if (w->m_monitor == mon && w->type() != Workspace::eWorkspaceType::SPECIAL && (!o.model.presented(workspaceKey(w)) || w == swipe->source)) candidates.push_back(w);
        std::ranges::sort(candidates, {}, [](const auto& w) { return workspaceKey(w); });
        const auto it=std::ranges::find(candidates,swipe->source);
        const int i=it==candidates.end()?0:static_cast<int>(it-candidates.begin());
        const int j=i+step;
        if (j>=0 && j<static_cast<int>(candidates.size())) return candidates[j];
        static auto create=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_create_new");
        if (*create && step>0) {
            WorkspaceKey id=1;
            for (const auto& w:State::workspaceState()->workspacesCopy()) if (w->numberedID()) id=std::max(id,workspaceKey(w)+1);
            return id<=UINT32_MAX?createWorkspaceForKey(id,mon->m_id):nullptr;
        }
        if (candidates.size()>1) return step>0?candidates.front():candidates.back();
        return nullptr;
    }
    static bool swipeBegin(CUnifiedWorkspaceSwipeGesture* gesture, PHLMONITOR mon) {
        if (!self->active(mon) || locked() || (self->overview && self->overview())) return reinterpret_cast<decltype(&swipeBegin)>(swipeBeginHook->m_original)(gesture,mon);
        if (self->interaction != Owner::None || mon->m_activeSpecialWorkspace) return false;
        const auto native=reinterpret_cast<decltype(&swipeBegin)>(hooks::nativeOriginal(swipeBeginHook->group->native));
        if (!native(gesture,mon)) return false;
        auto* o=self->output(mon);
        self->interaction=Owner::Swipe;
        self->swipe.emplace();
        auto& s=*self->swipe;
        s.monitor=mon; s.tile=o->model.state().focused; s.generation=o->model.state().generation;
        s.source=mon->m_activeWorkspace; s.sourceOffset=s.source->m_renderOffset->value(); s.native=gesture;
        const auto style=s.source->m_renderOffset->getStyle();
        s.vertical=style=="slidevert" || style.starts_with("slidefadevert");
        return true;
    }
    static void swipeUpdate(CUnifiedWorkspaceSwipeGesture* gesture, double delta) {
        if (!self->swipe || self->swipe->native!=gesture) { reinterpret_cast<decltype(&swipeUpdate)>(swipeUpdateHook->m_original)(gesture,delta); return; }
        auto& s=*self->swipe;
        auto mon=s.monitor.lock(); auto* o=self->output(mon);
        if (!o || o->model.state().generation!=s.generation || locked()) { self->cancelSwipe(); return; }
        const int direction=delta>=0?1:-1;
        if (s.direction!=direction) {
            if (s.target) { s.target->m_renderOffset->setValueAndWarp(s.targetOffset); s.target->m_forceRendering=s.targetForced; }
            s.direction=direction; s.target=self->swipeTarget(*o,direction);
            if (s.target) { s.targetOffset=s.target->m_renderOffset->value(); s.targetForced=s.target->m_forceRendering; s.target->m_forceRendering=true; s.target->space()->recalculate(); }
        }
        static auto distance=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        s.speed=std::abs(delta-s.delta); s.delta=delta; gesture->m_delta=delta;
        const double progress=s.target?std::clamp(std::abs(delta)/std::max(1.0,static_cast<double>(*distance)),0.0,1.0):0;
        const auto b=o->model.box(*o->model.index(s.tile));
        const double travel=s.vertical?b.height:b.width;
        s.sourcePreview=s.vertical?Vector2D{0.0,-direction*progress*travel}:Vector2D{-direction*progress*travel,0.0};
        s.source->m_renderOffset->setValueAndWarp(s.sourcePreview);
        if (s.target) { s.targetPreview=s.vertical?Vector2D{0.0,direction*(1-progress)*travel}:Vector2D{direction*(1-progress)*travel,0.0}; s.target->m_renderOffset->setValueAndWarp(s.targetPreview); }
        g_pHyprRenderer->damageMonitor(mon);
    }
    static void swipeEnd(CUnifiedWorkspaceSwipeGesture* gesture) {
        if (!self->swipe || self->swipe->native!=gesture) { reinterpret_cast<decltype(&swipeEnd)>(swipeEndHook->m_original)(gesture); return; }
        static auto distance=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        static auto ratio=CConfigValue<Config::FLOAT>("gestures:workspace_swipe_cancel_ratio");
        auto target=self->swipe->target; auto mon=self->swipe->monitor.lock();
        static auto force=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_min_speed_to_force");
        const bool commit=target && std::abs(self->swipe->delta)>=2 && (std::abs(self->swipe->delta)>=std::max(1.0,static_cast<double>(*distance))*std::clamp(static_cast<double>(*ratio),0.0,1.0) || (*force>0 && self->swipe->speed>=*force));
        self->cancelSwipe();
        if (commit) if (auto* o=self->output(mon); o && o->model.activate(workspaceKey(target))) self->apply(*o,true);
    }
    static void swipeCancel(CUnifiedWorkspaceSwipeGesture* gesture) {
        if (self->swipe && self->swipe->native==gesture) self->cancelSwipe();
        else reinterpret_cast<decltype(&swipeCancel)>(swipeCancelHook->m_original)(gesture);
    }
};

bool TilesController::Impl::install() {
    return attach("recheckWorkArea", &area, areaHook) && attach("windowAt", &hit, hitHook) &&
        attach("rawWindowFocus", &rawFocus, focusHook) && attach("changeWorkspaceMonitor", &change, changeHook) &&
        attach("isVisibleNotCovered", &visible, visibleHook) && attach("renderWindow", &window, windowHook) &&
        attach("renderWorkspaceWindows", &normal, normalHook) && attach("renderWorkspaceWindowsFullscreen", &full, fullHook) &&
        attach("sendFrameEventsToWorkspace", &frames, framesHook) && attach("setTargetSizeAndPosition", &fsSet, fsSetHook) && attach("syncTargetSizeAndPosition", &fsSync, fsSyncHook) &&
        attach("changeWorkspaceTyped", &typed, typedHook) && attach("changeWorkspaceCurrentTyped", &typedCurrent, typedCurrentHook) &&
        attach("changeWorkspaceString", &stringAction, stringHook) && attach("changeWorkspaceCurrentString", &stringCurrent, stringCurrentHook) &&
        attach("getWorkspaceTargetFromString", &resolve, resolverHook) && attach("previousWorkspace", &previous, previousHook) && attach("previousWorkspaceMonitor", &previousMonitor, previousMonitorHook) &&
        attach("moveFocus", &direction, directionHook) && attach("moveInDirection", &moveDirection, moveHook) && attach("moveWorkspaceToMonitor", &placement, placementHook) && attach("swapActiveWorkspaces", &swapPhysical, swapHook) &&
        attach("begin", &swipeBegin, swipeBeginHook) && attach("update", &swipeUpdate, swipeUpdateHook) && attach("end", &swipeEnd, swipeEndHook) && attach("cancel", &swipeCancel, swipeCancelHook);
}
void TilesController::Impl::cancelSwipe() {
    if (!swipe) return;
    auto session=std::move(*swipe); swipe.reset();
    if (session.source && session.source->m_renderOffset->value()==session.sourcePreview) session.source->m_renderOffset->setValueAndWarp(session.sourceOffset);
    if (session.target) {
        if (session.target->m_renderOffset->value()==session.targetPreview) session.target->m_renderOffset->setValueAndWarp(session.targetOffset);
        session.target->m_forceRendering=session.targetForced;
    }
    interaction=Owner::None;
    if (session.native) reinterpret_cast<decltype(&swipeCancel)>(hooks::nativeOriginal(swipeCancelHook->group->native))(session.native);
    if (auto mon=session.monitor.lock()) g_pHyprRenderer->damageMonitor(mon);
}
bool TilesController::Impl::ensure() {
    if (!integer("tiles_enabled", 0)) return false;
    if (ready) return true;
    if (!initError.empty()) return false;
    if (!install()) { resetHooks(); return false; }
    ready = true;
    return true;
}
void TilesController::Impl::notify(Output& o, PHLWORKSPACE old) {
    auto mon = o.monitor.lock();
    if (!mon) return;
    auto ws = mon->m_activeWorkspace;
    if (ws != old && ws) {
        if (old) old->m_events.activeChanged.emit();
        ws->m_events.activeChanged.emit();
        IPC::Socket2::sock()->postEvent({"workspace", ws->addressableName()});
        IPC::Socket2::sock()->postEvent({"workspacev2", ws->addressableName() + "," + ws->displayName()});
        Event::bus()->m_events.workspace.active.emit(ws);
    }
    IPC::Socket2::sock()->postEvent({"hymission-tiles", std::to_string(o.model.state().generation)});
}
void TilesController::Impl::apply(Output& o, bool focusWindow, bool warp) {
    const auto mon = o.monitor.lock();
    if (!mon || applying) return;
    applying = true;
    Hyprutils::Utils::CScopeGuard reset{[this] { applying = false; }};
    const auto old = mon->m_activeWorkspace;
    auto held = std::move(o.held);
    for (const auto& t : o.model.state().tiles) if (auto ws = workspaceForKey(t.workspace)) {
        o.held.push_back(ws); ws->setVisible(true); ws->m_alpha->setValueAndWarp(1.F); ws->m_renderOffset->setValueAndWarp(Vector2D{});
        if (t.id == o.model.state().focused) mon->m_activeWorkspace = ws;
    }
    for (const auto& ws : held) if (!o.model.presented(workspaceKey(ws)) && ws->m_monitor == mon) { ws->setVisible(false); ws->m_forceRendering = false; }
    if (old && !o.model.presented(workspaceKey(old)) && old->m_monitor == mon) old->setVisible(false);
    g_layoutManager->invalidateMonitorGeometries(mon);
    Desktop::globalWindowController()->updateSuspendedStates();
    if (focusWindow && !mon->m_activeSpecialWorkspace) {
        Desktop::focusState()->rawMonitorFocus(mon);
        Desktop::focusState()->fullWindowFocus(mon->m_activeWorkspace->getFocusCandidate(), Desktop::FOCUS_REASON_WORKSPACE_CHANGE);
    }
    if (warp) {
        const auto b = box(o.model.box(*o.model.index(o.model.state().focused)));
        Pointer::pointerController()->warpTo(b.pos() + b.size() / 2);
    }
    g_pHyprRenderer->damageMonitor(mon);
    notify(o, old);
}
void TilesController::Impl::collapse() {
    endResize(true);
    cancelSwipe();
    for (auto& o : outputs) if (auto mon = o->monitor.lock()) {
        auto keep = mon->m_activeWorkspace;
        for (const auto& w : o->held) if (w->m_monitor == mon) w->setVisible(w == keep);
    }
    outputs.clear();
    for (const auto& mon : State::monitorState()->monitors()) { g_layoutManager->invalidateMonitorGeometries(mon); g_pHyprRenderer->damageMonitor(mon); }
    Desktop::globalWindowController()->updateSuspendedStates();
}
void TilesController::Impl::reconcile() {
    if (locked() || applying || stopping) return;
    if (!integer("tiles_enabled", 0)) { if (ready) { collapse(); resetHooks(); } return; }
    if (!ensure()) return;
    std::erase_if(outputs, [](const auto& o) { return o->monitor.expired() || !o->monitor->m_enabled; });
    for (const auto& mon : State::monitorState()->monitors()) {
        if (!mon->m_enabled || mon->isMirror() || !mon->m_activeWorkspace || !std::isfinite(mon->m_size.x) || !std::isfinite(mon->m_size.y) || mon->m_size.x <= 0 || mon->m_size.y <= 0) continue;
        auto* o = output(mon);
        if (!o) { outputs.push_back(std::make_unique<Output>(mon, std::max(1L, integer("tile_min_width", 240)))); continue; }
        const auto generation = o->model.state().generation;
        if (!o->model.geometry(geometry(mon), std::max(1L, integer("tile_min_width", 240)))) continue;
        bool changed = generation != o->model.state().generation;
        if (o->model.state().tiles.size()==1 && mon->m_activeWorkspace && workspaceKey(mon->m_activeWorkspace)!=o->model.state().tiles[0].workspace) {
            (void)o->model.activate(workspaceKey(mon->m_activeWorkspace)); changed=true;
        }
        for (const auto& t : o->model.state().tiles) {
            auto w = workspaceForKey(t.workspace);
            if (!w || w->m_monitor != mon) {
                const auto id = t.id;
                const auto current = o->model.state().focused;
                // Remove invalid binding by replacing it, keeping other regions intact.
                auto next = fresh(mon);
                if (next) {
                    if (auto old = workspaceForKey(t.workspace)) (void)o->model.activate(workspaceKey(old));
                    auto state = o->model.state(); state.focused = id;
                    (void)o->model.restore(state, o->model.state().generation);
                    (void)o->model.activate(workspaceKey(next));
                    state = o->model.state(); state.focused = current;
                    (void)o->model.restore(state, o->model.state().generation);
                    o->held.push_back(next);
                    changed = true;
                }
                break;
            }
            if (!w->visible()) changed = true;
        }
        if (changed) apply(*o);
    }
}

TilesController::TilesController(HANDLE h, std::function<bool()> o) : m(std::make_unique<Impl>(h, std::move(o))) { instance = this; Impl::self = m.get(); }
TilesController::~TilesController() { m->stopping = true; m->deferred.reset(); m->resizeDeferred.reset(); m->listeners.clear(); if (m->ready) m->collapse(); m->resetHooks(); Impl::self = nullptr; instance = nullptr; }
TilesController* TilesController::get() { return instance; }
void TilesController::initialize() {
    auto& e = Event::bus()->m_events;
    m->listeners.emplace_back(e.config.reloaded.listen([this] { m->request(); }));
    m->listeners.emplace_back(e.monitor.layoutChanged.listen([this] { m->request(); }));
    m->listeners.emplace_back(e.workspace.active.listen([this](PHLWORKSPACE) { m->request(); }));
    m->listeners.emplace_back(e.workspace.removed.listen([this](PHLWORKSPACEREF) { m->request(); }));
    m->listeners.emplace_back(e.workspace.moveToMonitor.listen([this](PHLWORKSPACE, PHLMONITOR) { m->request(); }));
    m->listeners.emplace_back(e.render.preChecks.listen([this](PHLMONITOR mon) { if (m->active(mon)) mon->m_solitaryClient.reset(); }));
    m->listeners.emplace_back(e.render.stage.listen([this](Event::SRenderStageEvent event) { m->chrome(event); }));
    m->listeners.emplace_back(e.input.mouse.move.listen([this](const Vector2D& pos, Event::SCallbackInfo& info) { m->pointer(pos,info); }));
    m->listeners.emplace_back(e.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
        if (m->resize && event.keycode==KEY_ESC && event.state==WL_KEYBOARD_KEY_STATE_PRESSED) { m->endResize(true); info.cancelled=true; }
    }));
    m->listeners.emplace_back(e.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        if (m->dividerButton(event,info) || info.cancelled) return;
        if (locked() || event.state != WL_POINTER_BUTTON_STATE_PRESSED || m->interaction != Owner::None || (m->overview && m->overview())) return;
        const auto pos = g_pInputManager->getMouseCoordsInternal();
        auto mon = State::monitorState()->query().vec(pos).run();
        if (m->active(mon) && !mon->m_activeSpecialWorkspace) { const auto i = m->output(mon)->model.at(pos.x, pos.y); if (i) focus(mon, m->output(mon)->model.state().tiles[*i].id); }
    }));
    if (g_pSessionLockManager) m->listeners.emplace_back(g_pSessionLockManager->m_events.lock.listen([this] { m->endResize(true); m->cancelSwipe(); }));
    m->request();
}
std::vector<TilesController::View> TilesController::views(const PHLMONITOR& mon) const {
    std::vector<View> out;
    auto* o = m->output(mon);
    if (m->active(mon)) for (size_t i = 0; i < o->model.state().tiles.size(); ++i) {
        const auto& t = o->model.state().tiles[i];
        out.push_back({mon, workspaceForKey(t.workspace), t.id, box(o->model.box(i)), t.id == o->model.state().focused});
    }
    return out;
}
bool TilesController::enabled(const PHLMONITOR& mon) const { return m->active(mon); }
std::optional<TilesController::View> TilesController::view(const PHLWORKSPACE& w) const { return m->view(w); }
bool TilesController::presented(const PHLWORKSPACE& w) const { return m->view(w).has_value(); }
bool TilesController::focus(const PHLMONITOR& mon, tiles::Id id, bool warp) {
    auto* o = m->output(mon);
    if (!o || !m->ready || locked()) return false;
    const auto i = o->model.index(id);
    if (!i) return false;
    const auto generation = o->model.state().generation;
    if (!o->model.activate(o->model.state().tiles[*i].workspace)) return false;
    if (generation != o->model.state().generation) m->apply(*o, true, warp);
    return true;
}
bool TilesController::activate(const PHLMONITOR& mon, tiles::Id id, const PHLWORKSPACE& ws, bool swap) {
    auto* o = m->output(mon);
    if (!o || !ws || ws->m_monitor != mon || ws->type() == Workspace::eWorkspaceType::SPECIAL || locked() || !o->model.index(id)) return false;
    auto before = o->model.state();
    auto state = before; state.focused = id;
    if (!o->model.restore(state, before.generation) || !o->model.activate(workspaceKey(ws), swap ? tiles::Activation::Swap : tiles::Activation::Focus)) return false;
    m->apply(*o, true);
    return true;
}
bool TilesController::moveWindow(const PHLWINDOW& w, const PHLMONITOR& mon, tiles::Id id) {
    auto* o = m->output(mon);
    if (!o || !w || !w->mapped() || pinned(w) || locked()) return false;
    const auto i = o->model.index(id);
    auto ws = i ? workspaceForKey(o->model.state().tiles[*i].workspace) : nullptr;
    if (!ws) return false;
    w->layoutTarget()->assignToSpace(ws->space());
    focus(mon, id);
    Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_KEYBIND);
    return true;
}
bool TilesController::busy() const { return m->interaction != Owner::None; }
bool TilesController::acquire(Owner owner) { if (locked() || owner == Owner::None || busy()) return false; m->interaction = owner; return true; }
void TilesController::release(Owner owner) { if (m->interaction == owner) m->interaction = Owner::None; }
TilesController::Owner TilesController::owner() const { return m->interaction; }
void TilesController::edgeError(std::string e) { if (!e.empty()) { m->edgeFailure=std::move(e); m->softEdgeDrawn=false; } }
void TilesController::edgeActive() { m->softEdgeDrawn=true; m->edgeFailure.clear(); }
SDispatchResult TilesController::dispatch(const std::string& action, const std::string& args) {
    if (locked()) return {.success=false, .error="tiles unavailable while locked"};
    if (!m->ensure()) return {.success=false, .error=m->initError.empty() ? "tiles_enabled is 0" : m->initError};
    m->reconcile();
    const auto mon = Desktop::focusState()->monitor();
    auto* o = m->output(mon);
    if (!o || mon->m_activeSpecialWorkspace || busy()) return {.success=false, .error="tiles unavailable during special workspace or another interaction"};
    std::map<std::string,std::string> fields;
    std::istringstream stream(args);
    for (std::string word; stream >> word;) {
        const auto equal = word.find('=');
        if (equal == std::string::npos || !fields.emplace(word.substr(0,equal), word.substr(equal+1)).second) {
            if ((action == "tile_focus" || action == "tile_move_window") && (word == "left" || word == "right") && fields.empty()) fields["direction"] = word;
            else return {.success=false,.error="invalid or duplicate tile argument"};
        }
    }
    for (const auto& [k,v] : fields) if (!((action=="tile_add" && (k=="workspace"||k=="width")) || (action=="tile_resize" && (k=="width"||k=="delta"||k=="neighbor")) || ((action=="tile_focus"||action=="tile_move_window") && k=="direction"))) return {.success=false,.error="unknown tile argument: "+k};
    tiles::Result result;
    PHLWORKSPACE pendingWorkspace; // native state stores weak refs; retain through publication
    if (action == "tile_add") {
        if (o->model.state().tiles.size() >= tiles::maxTiles) return {.success=false,.error="at most three tiles per monitor"};
        std::optional<double> width;
        if (fields.contains("width")) { auto n = tiles::length(fields["width"], mon->m_size.x); if (!n) return {.success=false,.error=n.error()}; width=*n; }
        PHLWORKSPACE ws;
        if (fields.contains("workspace")) {
            const auto target = State::Workspace::resolver()->getWorkspaceTargetFromString(fields["workspace"], mon);
            if (!target.valid() || target.type == Workspace::eWorkspaceType::SPECIAL) return {.success=false,.error="expected normal workspace"};
            ws = State::workspaceState()->find(target);
            if (!ws) ws = State::workspaceState()->create(target, mon, true);
        } else ws = m->fresh(mon);
        if (!ws || ws->m_monitor != mon || ws->visible()) return {.success=false,.error="tile_add requires an unpresented workspace on this monitor"};
        result = o->model.add(workspaceKey(ws), width);
        pendingWorkspace = ws;
    } else if (action == "tile_remove") result = o->model.remove();
    else if (action == "tile_resize") {
        if (fields.contains("width") == fields.contains("delta")) return {.success=false,.error="specify width or delta"};
        const bool delta = fields.contains("delta");
        auto n = tiles::length(fields[delta ? "delta" : "width"], mon->m_size.x, delta);
        if (!n) return {.success=false,.error=n.error()};
        int neighbor = 0;
        if (fields.contains("neighbor")) { if (fields["neighbor"]=="left") neighbor=-1; else if (fields["neighbor"]=="right") neighbor=1; else return {.success=false,.error="neighbor must be left or right"}; }
        result = o->model.resize(*n + (delta ? o->model.state().tiles[*o->model.index(o->model.state().focused)].width : 0), neighbor);
    } else if (action == "tile_focus" || action == "tile_move_window") {
        const auto direction = fields["direction"];
        if (direction!="left" && direction!="right") return {.success=false,.error="direction must be left or right"};
        if (action=="tile_move_window") {
            const int j=static_cast<int>(*o->model.index(o->model.state().focused))+(direction=="left"?-1:1);
            if (j<0 || j>=static_cast<int>(o->model.state().tiles.size())) return {.success=false,.error="no adjacent tile"};
            return moveWindow(Desktop::focusState()->window(),mon,o->model.state().tiles[j].id) ? SDispatchResult{} : SDispatchResult{.success=false,.error="window move failed"};
        }
        result = o->model.focus(direction=="left"?-1:1);
    } else return {.success=false,.error="unknown tile action"};
    if (!result) return {.success=false,.error=result.error()};
    m->apply(*o,true);
    return {};
}
std::string TilesController::stateJson() const {
    nlohmann::json monitors=nlohmann::json::array(), tiles=nlohmann::json::array();
    for (const auto& o : m->outputs) if (auto mon=o->monitor.lock()) {
        nlohmann::json entries=nlohmann::json::array();
        for (size_t i=0;i<o->model.state().tiles.size();++i) {
            const auto& t=o->model.state().tiles[i]; const auto b=o->model.box(i); const auto ws=workspaceForKey(t.workspace);
            auto entry=nlohmann::json{{"id",t.id},{"name",mon->m_name+"/tile-"+std::to_string(t.id)},{"monitor",mon->m_name},{"x",b.x},{"y",b.y},{"width",b.width},{"height",b.height},
                {"focused", t.id==o->model.state().focused && Desktop::focusState()->monitor()==mon}, {"activeWorkspace",{{"id",t.workspace},{"name",ws?ws->displayName():""},{"address",ws?ws->addressableName():""}}},{"history",t.history},{"renderOffset",ws?nlohmann::json::array({ws->m_renderOffset->value().x,ws->m_renderOffset->value().y}):nlohmann::json::array({0,0})}};
            entries.push_back(entry); tiles.push_back(entry);
        }
        monitors.push_back({{"name",mon->m_name},{"generation",o->model.state().generation},{"tiles",entries}});
    }
    return dumpStateJson({{"version",1},{"backend","plugin"},{"enabled",m->ready},{"max_tiles",tiles::maxTiles},{"initError",m->initError},{"interaction_owner",static_cast<int>(m->interaction)},
        {"edge_effect",m->softEdgeDrawn?"blur_fade":"hard_clip"},{"edge_error",m->edgeFailure},{"monitors",monitors},{"tiles",tiles}})+"\n";
}
} // namespace hymission
