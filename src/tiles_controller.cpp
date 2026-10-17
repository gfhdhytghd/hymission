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
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/render/scene/SceneSelection.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#define private public
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#undef private
#include <hyprland/src/managers/fullscreen/handler/FullscreenHandler.hpp>

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
    Owner interaction = Owner::None;
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
        const auto i = o->model.presented(workspaceKey(w));
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
        StageController::renderInViewport(mon, stage::edgeViewport(clip, 0, 0), clip, [&] { original(r, ctx, w, mon, p, now, decorate, mode, ignore, standalone); });
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
};

bool TilesController::Impl::install() {
    return attach("recheckWorkArea", &area, areaHook) && attach("windowAt", &hit, hitHook) &&
        attach("rawWindowFocus", &rawFocus, focusHook) && attach("changeWorkspaceMonitor", &change, changeHook) &&
        attach("isVisibleNotCovered", &visible, visibleHook) && attach("renderWindow", &window, windowHook) &&
        attach("renderWorkspaceWindows", &normal, normalHook) && attach("renderWorkspaceWindowsFullscreen", &full, fullHook) &&
        attach("sendFrameEventsToWorkspace", &frames, framesHook) && attach("setTargetSizeAndPosition", &fsSet, fsSetHook) && attach("syncTargetSizeAndPosition", &fsSync, fsSyncHook);
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
TilesController::~TilesController() { m->stopping = true; m->deferred.reset(); m->listeners.clear(); if (m->ready) m->collapse(); m->resetHooks(); Impl::self = nullptr; instance = nullptr; }
TilesController* TilesController::get() { return instance; }
void TilesController::initialize() {
    auto& e = Event::bus()->m_events;
    m->listeners.emplace_back(e.config.reloaded.listen([this] { m->request(); }));
    m->listeners.emplace_back(e.monitor.layoutChanged.listen([this] { m->request(); }));
    m->listeners.emplace_back(e.workspace.removed.listen([this](PHLWORKSPACEREF) { m->request(); }));
    m->listeners.emplace_back(e.workspace.moveToMonitor.listen([this](PHLWORKSPACE, PHLMONITOR) { m->request(); }));
    m->listeners.emplace_back(e.render.preChecks.listen([this](PHLMONITOR mon) { if (m->active(mon)) mon->m_solitaryClient.reset(); }));
    m->listeners.emplace_back(e.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo&) {
        if (locked() || event.state != WL_POINTER_BUTTON_STATE_PRESSED || m->interaction != Owner::None || (m->overview && m->overview())) return;
        const auto pos = g_pInputManager->getMouseCoordsInternal();
        auto mon = State::monitorState()->query().vec(pos).run();
        if (m->active(mon) && !mon->m_activeSpecialWorkspace) { const auto i = m->output(mon)->model.at(pos.x, pos.y); if (i) focus(mon, m->output(mon)->model.state().tiles[*i].id); }
    }));
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
void TilesController::edgeError(std::string e) { m->edgeFailure = std::move(e); }
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
                {"focused", t.id==o->model.state().focused && Desktop::focusState()->monitor()==mon}, {"activeWorkspace",{{"id",t.workspace},{"name",ws?ws->displayName():""},{"address",ws?ws->addressableName():""}}},{"history",t.history}};
            entries.push_back(entry); tiles.push_back(entry);
        }
        monitors.push_back({{"name",mon->m_name},{"generation",o->model.state().generation},{"tiles",entries}});
    }
    return dumpStateJson({{"version",1},{"backend","plugin"},{"enabled",m->ready},{"max_tiles",tiles::maxTiles},{"initError",m->initError},{"interaction_owner",static_cast<int>(m->interaction)},
        {"edge_effect",m->edgeFailure.empty()?"hard_clip":"hard_clip"},{"edge_error",m->edgeFailure},{"monitors",monitors},{"tiles",tiles}})+"\n";
}
} // namespace hymission
