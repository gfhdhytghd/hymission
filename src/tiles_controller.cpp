#include "tiles_controller.hpp"
#include "hook_broker.hpp"
#include "workspace_key.hpp"
#include "render_context.hpp"
#include "stage_controller.hpp"
#include "state_json.hpp"
#include "tiles_symbols.hpp"
#include "hook_symbols.hpp"
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
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
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/PointerConstraintController.hpp>
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
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#undef private
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollTapeController.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingFullscreenHandler.hpp>
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
        std::vector<View> outgoing;
        std::map<tiles::Id,PHLWORKSPACE> bindings;
        uint64_t notifiedGeneration=0;
        bool removing=false;
        Output(PHLMONITOR m, double minimum) : monitor(m), model(geometry(m), workspaceKey(m->m_activeWorkspace), minimum),bindings{{model.state().focused,m->m_activeWorkspace}} {}
    };
    HANDLE handle;
    std::function<bool()> overview;
    std::function<bool()> overviewGrouped;
    std::function<void()> topologyChanged;
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<CHyprSignalListener> listeners;
    std::vector<hooks::Lease*> leases;
    UP<SEventLoopDoLaterLock> deferred;
    bool ready = false, applying = false, frameFanout = false, stopping = false;
    std::string initError, edgeFailure;
    bool softEdgeDrawn=false, nativeDropCancelled=false, collapsing=false;
    std::map<WorkspaceKey,std::pair<PHLMONITORREF,tiles::Id>> homes;
    Owner interaction = Owner::None;
    struct Swipe {
        PHLMONITORREF monitor;
        tiles::Id tile = 0;
        uint64_t generation = 0;
        PHLWORKSPACE source, target;
        Vector2D sourceOffset, targetOffset, sourcePreview, targetPreview;
        Vector2D sourceGoal;
        bool sourceAnimated=false;
        float sourceAlpha=1,sourceAlphaGoal=1,targetAlpha=1,targetAlphaGoal=1;
        bool sourceAlphaAnimated=false,targetAlphaAnimated=false;
        bool targetForced = false, vertical = false;
        double delta = 0, speed = 0;
        int direction = 0, initialDirection=0;
        uint64_t speedPoints=0;
        bool createdEmpty=false;
        CUnifiedWorkspaceSwipeGesture* native = nullptr;
    };
    std::optional<Swipe> swipe;
    struct Resize { PHLMONITORREF monitor; tiles::DividerDrag drag; double pointerX = 0; };
    std::optional<Resize> resize;
    UP<SEventLoopDoLaterLock> resizeDeferred;
    bool resizeReleasePending = false;
    struct Transfer {
        PHLMONITORREF sourceMonitor, targetMonitor;
        tiles::Id source=0;
        uint64_t sourceGeneration=0, targetGeneration=0;
        std::vector<PHLWINDOWREF> windows;
        PHLWORKSPACE sourceWorkspace, targetWorkspace;
        CBox origin;
        Vector2D press, pointer;
        tiles::Drop drop;
        bool dragging=false;
    };
    std::optional<Transfer> transfer;
    PHLWORKSPACE pointerWorkspace, layoutWorkspace;
    Impl(HANDLE h, std::function<bool()> o, std::function<void()> changed, std::function<bool()> grouped) : handle(h), overview(std::move(o)), overviewGrouped(std::move(grouped)),topologyChanged(std::move(changed)) {}
    Output* output(PHLMONITOR mon) const {
        for (auto& o : outputs) if (o->monitor == mon) return o.get();
        return nullptr;
    }
    bool active(PHLMONITOR mon) const { const auto* o = output(mon); return ready && mon && mon->m_enabled && o && !o->removing && o->model.state().tiles.size() > 1; }
    std::optional<View> view(PHLWORKSPACE w) const {
        if (!w) return {};
        auto* o = output(w->m_monitor.lock());
        if (!ready || !o || o->removing) return {};
        auto i = o->model.presented(workspaceKey(w));
        if (!i && swipe && swipe->target == w && swipe->monitor == w->m_monitor) i = o->model.index(swipe->tile);
        if (!i) return {};
        const auto& t = o->model.state().tiles[*i];
        return View{o->monitor.lock(), w, t.id, box(o->model.box(*i)), t.id == o->model.state().focused};
    }
    Vector2D inputPosition(PHLMONITOR mon,const Vector2D& point) const {
        const CBox output{mon->m_position,mon->m_size};
        if (const auto mapped=StageController::tilePresentation(mon,output)) {
            const double scale=mapped->w/output.w;
            return output.pos()+(point-mapped->pos())/scale;
        }
        return point;
    }
    std::optional<size_t> at(Output& output,const Vector2D& point) const {
        const auto pos=inputPosition(output.monitor.lock(),point);
        return output.model.at(pos.x,pos.y);
    }
    bool ensure();
    bool install();
    void resetHooks() { for (auto* l : leases) hooks::remove(handle, l); leases.clear(); ready = false; }
    void reconcile();
    std::optional<View> outgoingView(const PHLWORKSPACE& ws) const {
        if (!ws || !(ws->m_renderOffset->isBeingAnimated() || ws->m_alpha->isBeingAnimated())) return {};
        if (auto* o=output(ws->m_monitor.lock())) for (const auto& v:o->outgoing) if (v.workspace==ws) return v;
        return {};
    }
    void request() {
        if (stopping || deferred || !g_pEventLoopManager) return;
        deferred = g_pEventLoopManager->doLaterLock([this] { deferred.reset(); reconcile(); });
    }
    void apply(Output& o, bool focusWindow = false, bool warp = false, bool internal = false);
    bool publish(Output& o, tiles::Model draft, const std::vector<PHLWINDOW>& windows={}, PHLWORKSPACE destination=nullptr, bool focus=true, bool warp=false) {
        if (applying) return false;
        std::vector<PHLWORKSPACE> origins;
        for (const auto& w:windows) {
            if (!w || !w->mapped() || pinned(w) || !w->m_workspace || !destination) return false;
            origins.push_back(w->m_workspace);
        }
        applying=true;
        for (const auto& w:windows) Desktop::globalWindowController()->moveWindowToWorkspace(w,destination);
        bool moved=true;
        for (const auto& w:windows) moved &= w->mapped() && w->m_workspace==destination;
        if (!moved) {
            for (size_t i=0;i<windows.size();++i) if (windows[i]->mapped()) Desktop::globalWindowController()->moveWindowToWorkspace(windows[i],origins[i]);
            applying=false; return false;
        }
        o.model=std::move(draft);
        applying=false;
        apply(o,focus,warp);
        return true;
    }
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
        resize->pointerX=inputPosition(resize->monitor.lock(),pos).x;
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
        const auto input=inputPosition(mon,pos);
        const auto slop=std::clamp(static_cast<double>(integer("tile_divider_hit_width",8)),1.0,o->model.state().minimum/2)/2;
        for (size_t i=0;i+1<o->model.state().tiles.size();++i) {
            const auto b=o->model.box(i);
            if (std::abs(input.x-b.x-b.width)>slop) continue;
            auto d=tiles::DividerDrag::begin(o->model,i,input.x);
            if (!d) return false;
            resize=Resize{mon,*d,input.x}; resizeReleasePending=true; interaction=Owner::Resize;
            g_pSeatManager->setPointerFocus(nullptr,{});
            info.cancelled=true; return true;
        }
        return false;
    }
    void chrome(Event::SRenderStageEvent event) {
        if (!event.context || event.stage!=RENDER_POST_WINDOWS || (!active(event.monitor) && !(transfer && transfer->dragging && transfer->targetMonitor==event.monitor)) || locked()) return;
        auto mon=event.monitor;
        auto& ctx=event.context->get();
        if (transfer && transfer->dragging && transfer->targetMonitor==mon && transfer->drop.kind!=tiles::DropKind::Cancel) {
            CRectPassElement::SRectData data;
            data.box=box(transfer->drop.preview).translate(-mon->m_position).scale(mon->m_scale);
            const bool valid=transfer->drop.kind!=tiles::DropKind::Invalid;
            data.color=valid?CHyprColor(0.25,0.5,0.95,0.22):CHyprColor(0.9,0.2,0.15,0.2);
            g_pHyprRenderer->addPassElement(ctx,makeUnique<CRectPassElement>(data));
            if (transfer->drop.divider) {
                data.box=CBox{transfer->drop.divider-mon->m_position.x-1,transfer->drop.preview.y-mon->m_position.y,2.0,transfer->drop.preview.height}.scale(mon->m_scale);
                data.color=CHyprColor(0.4,0.7,1.0,0.85);
                g_pHyprRenderer->addPassElement(ctx,makeUnique<CRectPassElement>(data));
            }
        }
    }
    void notify(Output& o, PHLWORKSPACE old);
    PHLWORKSPACE fresh(PHLMONITOR mon) {
        unsigned candidates=0;
        for (WorkspaceKey key=1;key<UINT32_MAX;++key) {
            if (workspaceForKey(key)) continue;
            if (++candidates>256) return nullptr;
            const auto bound=Config::workspaceRuleMgr()->getBoundMonitorForWS(Workspace::SWorkspaceNumberedID{static_cast<uint32_t>(key)},Workspace::eWorkspaceType::NORMAL,std::to_string(key));
            if (bound && *bound!=mon) continue;
            auto ws=createWorkspaceForKey(key,mon->m_id);
            return ws && ws->m_monitor==mon?ws:nullptr;
        }
        return nullptr;
    }
    template<class Fn> bool attach(const char* key, Fn fn, hooks::Lease*& lease, int priority=0) {
        const auto sig = tiles::symbols::lookup(key);
        const auto source=hooks::findSymbol(HyprlandAPI::findFunctionsByName(handle, std::string(sig.substr(0, sig.find('('))).substr(std::string(sig.substr(0, sig.find('('))).rfind("::") + 2)),sig,true);
        if (!source) { initError = std::string("tiles disabled: expected one exact symbol: ") + std::string(sig); return false; }
        lease = hooks::create(handle, source, hooks::target(fn));
        if (lease) { lease->priority=priority; leases.push_back(lease); }
        if (!lease || !lease->hook()) { initError = std::string("tiles disabled: hook attach failed: ") + std::string(sig); return false; }
        return true;
    }
    inline static Impl* self = nullptr;
    inline static hooks::Lease *areaHook, *hitHook, *focusHook, *changeHook, *visibleHook, *windowHook, *normalHook, *fullHook, *framesHook, *fsSetHook, *fsSyncHook;
    inline static hooks::Lease *typedHook, *typedCurrentHook, *stringHook, *stringCurrentHook, *resolverHook, *previousHook, *previousMonitorHook,
        *directionHook, *moveHook, *placementHook, *swapHook, *swipeBeginHook, *swipeUpdateHook, *swipeEndHook, *swipeCancelHook;
    inline static hooks::Lease *pointerHook, *fsHasHook, *fsModesHook, *fsWindowHook, *fsFadeHook, *layerHook, *layerHitHook;
    inline static hooks::Lease *cameraAreaHook, *dragEndHook, *logicalBoxHook, *scrollRecalculateHook, *scrollFocusHook, *scrollRequestHook, *scrollColumnHook;

    struct LayoutScope {
        PHLWORKSPACE previous;
        explicit LayoutScope(PHLWORKSPACE ws):previous(self->layoutWorkspace) { self->layoutWorkspace=ws; }
        ~LayoutScope() { self->layoutWorkspace=previous; }
    };
    struct AlgoAccess:Layout::IModeAlgorithm {
        static PHLWORKSPACE workspace(Layout::IModeAlgorithm* algorithm) {
            const auto parent=algorithm->*&AlgoAccess::m_parent;
            return parent && parent->space()?parent->space()->workspace():nullptr;
        }
    };
    struct HandlerAccess:Fullscreen::IFullscreenHandler {
        static SP<Layout::CSpace> space(const Fullscreen::IFullscreenHandler* handler) {
            return (handler->*&HandlerAccess::getSpace)();
        }
    };
    static CBox logicalBox(const Monitor::CMonitor* mon) {
        const auto ws=self->layoutWorkspace;
        if (self->ready && ws && ws->type()!=Workspace::eWorkspaceType::SPECIAL && ws->m_monitor.get()==mon) {
            auto view=self->view(ws);
            if (view) return StageController::tilePresentation(view->monitor,view->allocation).value_or(view->allocation);
            const auto id=instance->preferredTile(ws,ws->m_monitor.lock());
            for (const auto& candidate:instance->views(ws->m_monitor.lock())) if (candidate.id==id)
                return StageController::tilePresentation(candidate.monitor,candidate.allocation).value_or(candidate.allocation);
        }
        return reinterpret_cast<decltype(&logicalBox)>(logicalBoxHook->m_original)(mon);
    }
    static void scrollRecalculate(Layout::Tiled::SScrollingData* data,bool force) {
        const auto algorithm=data->algorithm;
        const auto ws=algorithm?AlgoAccess::workspace(algorithm):nullptr;
        LayoutScope scope{ws};
        reinterpret_cast<decltype(&scrollRecalculate)>(scrollRecalculateHook->m_original)(data,force);
        // This layout overrides the generic fullscreen geometry setters with
        // no-ops; its camera can place a column after the fullscreen request.
        if (ws && self->active(ws->m_monitor.lock())) {
            const auto handler=algorithm->getFSHandler();
            if (handler) for (const auto& window:Desktop::windowState()->windows())
                if (window->mapped() && window->m_workspace==ws && !pinned(window) && Fullscreen::controller()->layoutManagedFS(window))
                    clampFullscreen(handler.get(),window->layoutTarget());
        }
    }
    static void scrollFocus(Layout::Tiled::CScrollingAlgorithm* algorithm,SP<Layout::ITarget> target,Layout::Tiled::CScrollingAlgorithm::eInputMode input) {
        LayoutScope scope{target?target->workspace():nullptr};
        reinterpret_cast<decltype(&scrollFocus)>(scrollFocusHook->m_original)(algorithm,target,input);
    }
    using ScrollFS=Fullscreen::ScrollingFullscreenHandler::CScrollingFullscreenHandler;
    static Fullscreen::eFullscreenRequestResult scrollRequest(ScrollFS* handler,const Fullscreen::SFullscreenRequest& request) {
        LayoutScope scope{request.target?request.target->workspace():nullptr};
        const auto result=reinterpret_cast<decltype(&scrollRequest)>(scrollRequestHook->m_original)(handler,request);
        if (request.target && self->active(request.target->workspace()?request.target->workspace()->m_monitor.lock():nullptr)) clampFullscreen(handler,request.target);
        return result;
    }
    static float scrollColumn(const ScrollFS* handler) {
        const auto space=HandlerAccess::space(handler);
        LayoutScope scope{space?space->workspace():nullptr};
        return reinterpret_cast<decltype(&scrollColumn)>(scrollColumnHook->m_original)(handler);
    }
    static void cameraArea(Layout::CSpace* space) {
        const auto ws=space?space->workspace():nullptr;
        const auto mon=ws?ws->m_monitor.lock():nullptr;
        auto* scrolling=ws && ws->type()!=Workspace::eWorkspaceType::SPECIAL && space->algorithm() ?
            dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(space->algorithm()->tiledAlgo().get()):nullptr;
        auto* controller=scrolling && scrolling->m_scrollingData?scrolling->m_scrollingData->controller.get():nullptr;
        const bool remap=controller && self->ready && (self->active(mon) || self->applying || self->collapsing);
        const bool horizontal=controller && controller->isPrimaryHorizontal();
        const auto before=space->workArea();
        reinterpret_cast<decltype(&cameraArea)>(cameraAreaHook->m_original)(space);
        if (!remap) return;
        const auto after=space->workArea();
        const double from=horizontal?before.w:before.h,to=horizontal?after.w:after.h;
        if (from<=1 || to<=1 || std::abs(from-to)<.001) return;
        // Scroll offsets are distances in the old viewport, not column indices.
        // Follow the final work area after both Tiles and Stage reservations.
        const double ratio=to/from;
        const double offset=controller->getOffset()*ratio;
        controller->getScrollInhibitor().offsetWhenInhibited*=ratio;
        controller->setOffset(offset);
    }
    static void area(Layout::CSpace* space) {
        // Rebuild native monitor gaps/reserved areas in physical coordinates.
        // Stage transforms the resulting tile area once, after this subscriber.
        LayoutScope physicalMonitor{nullptr};
        reinterpret_cast<decltype(&area)>(areaHook->m_original)(space);
        auto v = self->view(space->workspace());
        if (!v && self->ready && space->workspace() && space->workspace()->type()!=Workspace::eWorkspaceType::SPECIAL) {
            const auto mon=space->workspace()->m_monitor.lock();
            const auto id=instance->preferredTile(space->workspace(),mon);
            for (const auto& candidate:instance->views(mon)) if (candidate.id==id) { v=candidate; break; }
        }
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
        const auto i = self->at(*o,pos);
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
        if (o->model.activate(workspaceKey(ws))) self->apply(*o, !internal && !noFocus, !noMouseMove, internal);
    }
    static bool visible(const Workspace::CHLWorkspace* ws) {
        if (!locked() && self->view(const_cast<Workspace::CHLWorkspace*>(ws)->m_self.lock())) return !ws->m_monitor->m_activeSpecialWorkspace;
        return reinterpret_cast<decltype(&visible)>(visibleHook->m_original)(ws);
    }
    static void window(Render::IHyprRenderer* r, Render::CRenderContext& ctx, PHLWINDOW w, PHLMONITOR mon,
                       const Render::SWindowRenderPresentation& p, const Time::steady_tp& now, bool decorate, Render::eRenderPassMode mode, bool ignore, bool standalone) {
        const auto original = reinterpret_cast<decltype(&window)>(windowHook->m_original);
        if (!ignore && !standalone && !ctx.m_renderingSnapshot && instance->transferMember(w) && instance->transferDragging()) return;
        const bool overview=self->overview && self->overview();
        // Unified all/forceall previews own their physical-output clipping.
        if (overview && (!self->overviewGrouped || !self->overviewGrouped())) {
            original(r, ctx, std::move(w), std::move(mon), p, now, decorate, mode, ignore, standalone); return;
        }
        auto v = self->view(w ? w->m_workspace : nullptr);
        if (!v && !overview) v=self->outgoingView(w?w->m_workspace:nullptr);
        if (!v && overview && w && w->m_workspace) if (const auto id=instance->preferredTile(w->m_workspace,mon)) {
            for (const auto& candidate:instance->views(mon)) if (candidate.id==id) { v=candidate; v->workspace=w->m_workspace; break; }
        }
        const auto* drag=g_layoutManager->dragController().get();
        const bool nativeDrag=!overview && drag && drag->mode()==MBIND_MOVE && drag->target() && drag->target()->window()==w;
        if (nativeDrag || (!self->active(mon) && !overview) || !v || v->monitor != mon || pinned(w) || ignore || standalone || ctx.m_renderingSnapshot || StageController::renderingPreview()) {
            original(r, ctx, std::move(w), std::move(mon), p, now, decorate, mode, ignore, standalone); return;
        }
        RenderContextScope scope{ctx};
        auto clipBox=v->allocation;
        if (!overview) if (const auto mapped=StageController::tilePresentation(mon,clipBox)) {
            clipBox=*mapped;
            // Keep the native vertical-shadow protection: the common scene
            // moves content, but reserved layout bounds must not trim CSD.
            if (!w->m_workspace->m_renderOffset->isBeingAnimated() && w->m_workspace->m_renderOffset->value()==Vector2D{}) {
                clipBox.y=v->allocation.y; clipBox.h=v->allocation.h;
            }
        }
        const auto presentation=overview?StageController::overviewPresentation(w,mon):std::nullopt;
        if (presentation) clipBox=clipBox.intersection(CBox{presentation->clip.x,presentation->clip.y,presentation->clip.width,presentation->clip.height});
        else if (!overview && !w->isFloating() && !Fullscreen::controller()->isFullscreen(w)) if (const auto desktop=StageController::nativeDesktop(w->m_workspace)) {
            // Reapply 21fdcdd's native vertical-shadow rule at the outer tile
            // pass too: layout reserved/gap bounds must not trim CSD/decorations.
            const auto clip=stage::nativeTileClip({clipBox.x,clipBox.y,clipBox.w,clipBox.h},*desktop);
            clipBox=CBox{clip.x,clip.y,clip.width,clip.height};
        }
        if (clipBox.empty()) return;
        const Rect clip{clipBox.x,clipBox.y,clipBox.w,clipBox.h};
        auto edge=stage::edgeViewport(clip,0,0);
        if (presentation) edge=stage::edgeViewport(clip,presentation->viewport.left,presentation->viewport.right,presentation->viewport.top,presentation->viewport.bottom);
        const auto algorithm=w->m_workspace->space()->algorithm();
        const bool scrolling=algorithm && algorithm->tiledAlgo() && Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(&typeid(*algorithm->tiledAlgo()))=="scrolling";
        if (!overview && scrolling && !w->isFloating() && !Fullscreen::controller()->isFullscreen(w)) {
            const double width=std::max(0L,integer("stage_scrolling_desktop_edge_width",32));
            if (width>0 && StageController::ensureEdgeService()) {
                const auto pos=w->positionAnimation()->value()+w->m_workspace->m_renderOffset->value()+w->presentation().floatingOffset();
                const auto size=w->sizeAnimation()->value();
                edge=stage::edgeViewportForWindow(stage::edgeViewport(clip,width,width,0,0),{pos.x,pos.y,size.x,size.y});
            } else if (width>0) self->edgeFailure="native blur backdrop hook unavailable; hard clip retained";
        }
        // WORKSPACE_WINDOWS selects only this workspace, but its presentation
        // intentionally strips workspace motion/alpha for snapshots. This is a
        // live tile scene: retain the native MONITOR presentation inside our
        // fixed tile clip, including warped offsets while fingers are down.
        const auto livePresentation=overview?p:w->presentation().renderPresentation(Render::eSceneMode::MONITOR);
        StageController::renderInViewport(mon, edge, clip, [&] { original(r, ctx, w, mon, livePresentation, now, decorate, mode, ignore, standalone); });
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
        auto* o=self->output(mon);
        std::erase_if(o->outgoing,[&](const auto& v) { return !self->outgoingView(v.workspace) || o->model.presented(workspaceKey(v.workspace)); });
        for (const auto& v:o->outgoing) {
            const auto fn=reinterpret_cast<SceneFn>((Fullscreen::controller()->hasFullscreen(v.workspace)?fullHook:normalHook)->m_original);
            fn(r,ctx,mon,v.workspace,now,Render::eSceneMode::WORKSPACE_WINDOWS);
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
        for (const auto& v:self->output(mon)->outgoing) if (self->outgoingView(v.workspace)) fn(r,mon,v.workspace,now);
        if (self->swipe && self->swipe->monitor==mon && self->swipe->target) fn(r,mon,self->swipe->target,now);
    }
    static void clampFullscreen(Fullscreen::IFullscreenHandler* handler, const SP<Layout::ITarget>& target) {
        if (!target) return;
        const auto v = self->view(target->workspace());
        const auto mode = handler->getFullscreenModes(target).internal;
        if (!v || mode == Fullscreen::FSMODE_NONE) return;
        const auto layoutTarget=target->window()?target->window()->layoutTarget():target;
        const auto allocation=StageController::tilePresentation(v->monitor,v->allocation).value_or(v->allocation);
        const auto desired=mode==Fullscreen::FSMODE_FULLSCREEN?allocation:target->space()->workArea(target->floating());
        if (const auto window=target->window()) {
            auto expected=desired.copy().round();
            if (mode==Fullscreen::FSMODE_MAXIMIZED) {
                const auto reserved=window->getFullWindowReservedArea();
                expected.translate(reserved.topLeft); expected.w-=reserved.topLeft.x+reserved.bottomRight.x; expected.h-=reserved.topLeft.y+reserved.bottomRight.y;
            }
            if (window->positionAnimation()->goal()==expected.pos() && window->sizeAnimation()->goal()==expected.size()) return;
        }
        const bool queued=Fullscreen::controller()->m_windowPosSettingQueued;
        Fullscreen::controller()->m_windowPosSettingQueued=true;
        Hyprutils::Utils::CScopeGuard restore{[&] { Fullscreen::controller()->m_windowPosSettingQueued=queued; }};
        layoutTarget->setPositionGlobal(desired);
    }
    static void fsSet(Fullscreen::IFullscreenHandler* handler, SP<Layout::ITarget> target) {
        if (target && self->view(target->workspace()) && handler->getFullscreenModes(target).internal!=Fullscreen::FSMODE_NONE) {
            clampFullscreen(handler,target); return;
        }
        reinterpret_cast<decltype(&fsSet)>(fsSetHook->m_original)(handler, target); clampFullscreen(handler, target);
    }
    static void fsSync(Fullscreen::IFullscreenHandler* handler) {
        const auto target=handler->getFullscreen();
        if (target && self->view(target->workspace()) && handler->getFullscreenModes(target).internal!=Fullscreen::FSMODE_NONE) {
            clampFullscreen(handler,target); return;
        }
        reinterpret_cast<decltype(&fsSync)>(fsSyncHook->m_original)(handler); clampFullscreen(handler, handler->getFullscreen());
    }

    static PHLWORKSPACE pointerTarget(PHLWORKSPACE ws) {
        if (self->pointerWorkspace && ws && ws->m_monitor==self->pointerWorkspace->m_monitor && ws==ws->m_monitor->m_activeWorkspace) return self->pointerWorkspace;
        return ws;
    }
    static bool fsHas(Fullscreen::CFullscreenController* c,PHLWORKSPACE ws,std::optional<bool> covering) {
        ws=pointerTarget(ws);
        LayoutScope scope{ws && self->active(ws->m_monitor.lock())?ws:nullptr};
        return reinterpret_cast<decltype(&fsHas)>(fsHasHook->m_original)(c,ws,covering);
    }
    static Fullscreen::SFullscreenMode fsModes(Fullscreen::CFullscreenController* c,PHLWORKSPACE ws,std::optional<bool> covering) {
        ws=pointerTarget(ws);
        LayoutScope scope{ws && self->active(ws->m_monitor.lock())?ws:nullptr};
        return reinterpret_cast<decltype(&fsModes)>(fsModesHook->m_original)(c,ws,covering);
    }
    static PHLWINDOW fsWindow(Fullscreen::CFullscreenController* c,PHLWORKSPACE ws,std::optional<bool> covering) {
        ws=pointerTarget(ws);
        LayoutScope scope{ws && self->active(ws->m_monitor.lock())?ws:nullptr};
        return reinterpret_cast<decltype(&fsWindow)>(fsWindowHook->m_original)(c,ws,covering);
    }
    static bool dragEnd(Layout::Supplementary::CDragStateController* drag) {
        const auto original=reinterpret_cast<decltype(&dragEnd)>(dragEndHook->m_original);
        PHLWINDOW window;
        PHLWORKSPACE source,destination;
        PHLMONITOR monitor;
        tiles::Id tile=0;
        const auto point=g_pInputManager->getMouseCoordsInternal();
        static auto threshold=CConfigValue<Config::INTEGER>("binds:drag_threshold");
        if (!locked() && !self->nativeDropCancelled && self->interaction==Owner::None &&
            !(self->overview && self->overview()) && drag->mode()==MBIND_MOVE &&
            (*threshold<=0 || drag->dragThresholdReached()) && drag->target()) {
            window=drag->target()->window();
            monitor=State::monitorState()->query().vec(point).run();
            if (window && window->mapped() && !pinned(window) && !window->onSpecialWorkspace() &&
                self->active(monitor) && !monitor->m_activeSpecialWorkspace) {
                if (auto* o=self->output(monitor)) if (const auto i=self->at(*o,point)) {
                    const auto& target=o->model.state().tiles[*i];
                    tile=target.id;
                    destination=workspaceForKey(target.workspace);
                    source=window->m_workspace;
                }
            }
        }
        self->nativeDropCancelled=false;
        // Keep the native drag as the sole motion owner. It restores tiling,
        // handles grouping and lets Stage consume sidebar drops before we
        // commit a desktop transfer through the regular activation transaction.
        const bool ended=original(drag);
        if (ended && window && window->mapped() && source && source!=destination && window->m_workspace==source) {
            const auto* o=self->output(monitor);
            const auto i=o?o->model.index(tile):std::nullopt;
            if (i && workspaceForKey(o->model.state().tiles[*i].workspace)==destination)
                instance->moveWindow(window,monitor,tile);
        }
        return ended;
    }
    static void pointerInput(CInputManager* input,uint32_t time,bool refocus,bool mouse,std::optional<Vector2D> position,bool pointerOnly) {
        const auto previous=self->pointerWorkspace;
        self->pointerWorkspace.reset();
        Hyprutils::Utils::CScopeGuard reset{[&] { self->pointerWorkspace=previous; }};
        const auto pos=position.value_or(input->getMouseCoordsInternal());
        const auto mon=State::monitorState()->query().vec(pos).run();
        if (!self->applying && !locked() && self->active(mon) && !mon->m_activeSpecialWorkspace && !(self->overview && self->overview())) {
            auto* o=self->output(mon);
            if (const auto i=self->at(*o,pos)) {
                self->pointerWorkspace=workspaceForKey(o->model.state().tiles[*i].workspace);
                static auto focusMonitor=CConfigValue<Config::INTEGER>("misc:mouse_move_focuses_monitor");
                // Empty virtual outputs have no window focus event to publish
                // their active workspace. Match native monitor hover routing.
                const auto* drag=g_layoutManager->dragController().get();
                if (!pointerOnly && (*focusMonitor || refocus) && self->interaction==Owner::None &&
                    (!drag || !drag->target()) && o->model.state().focused!=o->model.state().tiles[*i].id &&
                    mon->logicalBoxMinusReserved().containsPoint(pos)) {
                    (void)o->model.activate(workspaceKey(self->pointerWorkspace));
                    self->apply(*o,true,false);
                }
            }
        }
        reinterpret_cast<decltype(&pointerInput)>(pointerHook->m_original)(input,time,refocus,mouse,position,pointerOnly);
    }
    static void fsFade(PHLWORKSPACE ws,Animation::Workspace::eAnimationType type) {
        reinterpret_cast<decltype(&fsFade)>(fsFadeHook->m_original)(ws,type);
        const auto mon=ws?ws->m_monitor.lock():nullptr;
        if (self->active(mon) && !mon->m_activeSpecialWorkspace && ws->type()!=Workspace::eWorkspaceType::SPECIAL) {
            // Top-layer visibility is now clipped per tile; keep its monitor-wide
            // alpha available for the other desktops.
            for (const auto& ls:mon->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) if (ls) *ls->alpha()[Desktop::View::LS_ALPHA_FADE]=1.F;
        }
    }
    static bool covered(PHLWORKSPACE ws) {
        LayoutScope scope{ws && self->active(ws->m_monitor.lock())?ws:nullptr};
        return ws && reinterpret_cast<decltype(&fsModes)>(fsModesHook->m_original)(Fullscreen::controller().get(),ws,true).internal==Fullscreen::FSMODE_FULLSCREEN;
    }
    static void layer(Render::IHyprRenderer* r,Render::CRenderContext& ctx,PHLLS ls,PHLMONITOR mon,const Time::steady_tp& now,bool popups,bool lockscreen) {
        const auto original=reinterpret_cast<decltype(&layer)>(layerHook->m_original);
        if (!self->active(mon) || locked() || lockscreen || !ls || ls->m_layer!=ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
            (ls->m_flags & Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN) || mon->m_activeSpecialWorkspace || ctx.m_renderingSnapshot || (self->overview && self->overview())) {
            original(r,ctx,ls,mon,now,popups,lockscreen); return;
        }
        RenderContextScope scope{ctx};
        for (const auto& view:instance->views(mon)) if (!covered(view.workspace)) {
            const Rect clip{view.allocation.x,view.allocation.y,view.allocation.w,view.allocation.h};
            StageController::renderInViewport(mon,stage::edgeViewport(clip,0,0),clip,[&] { original(r,ctx,ls,mon,now,popups,lockscreen); });
        }
    }
    static SP<CWLSurfaceResource> layerHit(const Desktop::CViewHitTester* tester,const Vector2D& pos,std::vector<PHLLSREF>* layers,Vector2D* coords,PHLLS* found,bool aboveLock) {
        const auto original=reinterpret_cast<decltype(&layerHit)>(layerHitHook->m_original);
        const auto mon=State::monitorState()->query().vec(pos).run();
        if (!layers || !self->active(mon) || locked() || aboveLock || mon->m_activeSpecialWorkspace || (self->overview && self->overview())) return original(tester,pos,layers,coords,found,aboveLock);
        auto* o=self->output(mon); const auto i=self->at(*o,pos);
        if (!i || !covered(workspaceForKey(o->model.state().tiles[*i].workspace))) return original(tester,pos,layers,coords,found,aboveLock);
        auto filtered=*layers;
        std::erase_if(filtered,[](const auto& ls) { return ls && ls->m_layer==ZWLR_LAYER_SHELL_V1_LAYER_TOP && !(ls->m_flags & Desktop::View::LAYER_FLAG_ABOVE_FULLSCREEN); });
        return original(tester,pos,&filtered,coords,found,aboveLock);
    }

    using Action = Config::Actions::ActionResult;
    Action route(PHLWORKSPACE ws, bool current) {
        auto mon = Desktop::focusState()->monitor();
        auto* o = output(mon);
        if (!active(mon) || !ws || ws->type() == Workspace::eWorkspaceType::SPECIAL || applying || locked()) {
            // Native on-current-monitor recognizes visible workspaces through
            // the physical output's compatibility active pointer.
            if (!applying && !locked() && mon && current && ws && ws->m_monitor!=mon && active(ws->m_monitor.lock())) if (const auto v=view(ws);v && !v->focused) {
                auto* target=output(v->monitor);
                if (target->model.activate(workspaceKey(ws))) apply(*target);
            }
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
        // Keep native pointer constraints, workspace_center_on and cursor policy;
        // the monitor boundary below only substitutes tile publication/geometry.
        return reinterpret_cast<Action (*)(PHLWORKSPACE)>((current?typedCurrentHook:typedHook)->m_original)(ws);
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
        if (!ws || !self->active(ws->m_monitor.lock())) return {};
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
            static auto groupFirst=CConfigValue<Config::INTEGER>("binds:movefocus_cycles_groupfirst");
            static auto fullscreenCycle=CConfigValue<Config::INTEGER>("binds:movefocus_cycles_fullscreen");
            if (*fullscreenCycle && w && Fullscreen::controller()->isFullscreen(w) && !Fullscreen::controller()->layoutManagedFS(w) &&
                Desktop::windowState()->query().cycle(w,{.focusableOnly=true,.previous=dir==Math::DIRECTION_LEFT,.allowFullscreenBlocked=true}))
                return reinterpret_cast<decltype(&direction)>(directionHook->m_original)(dir);
            if (*groupFirst && w && w->grouping().group()) {
                const auto group=w->grouping().group();
                if ((dir==Math::DIRECTION_LEFT && w!=group->head()) || (dir==Math::DIRECTION_RIGHT && w!=group->tail()))
                    return reinterpret_cast<decltype(&direction)>(directionHook->m_original)(dir);
            }
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
        for (const auto& mon : {a,b}) if (self->ready && self->output(mon) && mon->m_activeWorkspace) { auto* o=self->output(mon); (void)o->model.activate(workspaceKey(mon->m_activeWorkspace)); self->apply(*o); }
    }
    PHLWORKSPACE swipeTarget(Output& o, int step) {
        const auto mon = o.monitor.lock();
        std::vector<PHLWORKSPACE> candidates;
        static auto useRelative=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_use_r");
        if (*useRelative) {
            const auto count=State::workspaceState()->workspacesCopy().size()+tiles::maxTiles+1;
            for (size_t n=1;n<=count;++n) {
                const auto selector=std::string("r")+(step>0?"+":"-")+std::to_string(n);
                const auto target=State::Workspace::resolver()->getWorkspaceTargetFromString(selector,mon);
                if (!target.valid() || target.type==Workspace::eWorkspaceType::SPECIAL) return nullptr;
                auto ws=State::workspaceState()->find(target);
                if (ws && (ws==swipe->source || o.model.presented(workspaceKey(ws)) || ws->m_monitor!=mon)) continue;
                if (!ws) {
                    if (swipe->createdEmpty) return nullptr;
                    ws=State::workspaceState()->create(target,mon); swipe->createdEmpty=!!ws;
                }
                return ws && ws->m_monitor==mon?ws:nullptr;
            }
            return nullptr;
        }
        rememberWorkspaceKeys();
        for (const auto& w : State::workspaceState()->workspacesCopy()) if (w->m_monitor == mon && w->type() != Workspace::eWorkspaceType::SPECIAL && (!o.model.presented(workspaceKey(w)) || w == swipe->source)) candidates.push_back(w);
        std::ranges::sort(candidates, {}, [](const auto& w) { return workspaceKey(w); });
        const auto it=std::ranges::find(candidates,swipe->source);
        const int i=it==candidates.end()?0:static_cast<int>(it-candidates.begin());
        const int j=i+step;
        if (j>=0 && j<static_cast<int>(candidates.size())) return candidates[j];
        static auto create=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_create_new");
        if (*create && step>0 && !swipe->createdEmpty) {
            WorkspaceKey id=1;
            for (const auto& w:State::workspaceState()->workspacesCopy()) if (w->numberedID()) id=std::max(id,workspaceKey(w)+1);
            auto ws=id<=UINT32_MAX?createWorkspaceForKey(id,mon->m_id):nullptr;
            swipe->createdEmpty=!!ws; return ws && ws->m_monitor==mon?ws:nullptr;
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
        s.sourceGoal=s.source->m_renderOffset->goal(); s.sourceAnimated=s.source->m_renderOffset->isBeingAnimated();
        s.sourceAlpha=s.source->m_alpha->value(); s.sourceAlphaGoal=s.source->m_alpha->goal(); s.sourceAlphaAnimated=s.source->m_alpha->isBeingAnimated();
        const auto style=s.source->m_renderOffset->getStyle();
        s.vertical=style=="slidevert" || style.starts_with("slidefadevert");
        return true;
    }
    static void swipeUpdate(CUnifiedWorkspaceSwipeGesture* gesture, double delta) {
        if (!self->swipe || self->swipe->native!=gesture) { reinterpret_cast<decltype(&swipeUpdate)>(swipeUpdateHook->m_original)(gesture,delta); return; }
        auto& s=*self->swipe;
        auto mon=s.monitor.lock(); auto* o=self->output(mon);
        if (!o || o->model.state().generation!=s.generation || locked()) { self->cancelSwipe(); return; }
        static auto lock=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_direction_lock");
        static auto threshold=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_direction_lock_threshold");
        s.speed=(s.speed*s.speedPoints+std::abs(delta-s.delta))/(s.speedPoints+1); ++s.speedPoints;
        if (*lock) {
            const int sign=delta>=0?1:-1;
            if (s.initialDirection && sign!=s.initialDirection) delta=0;
            else if (!s.initialDirection && std::abs(delta)>*threshold) s.initialDirection=sign;
        }
        const int direction=delta>=0?1:-1;
        if (s.direction!=direction) {
            if (s.target) {
                s.target->m_renderOffset->setValueAndWarp(s.targetOffset); s.target->m_forceRendering=s.targetForced;
                s.target->m_alpha->setValueAndWarp(s.targetAlpha);
                if (s.targetAlphaAnimated) *s.target->m_alpha=s.targetAlphaGoal;
            }
            s.direction=direction; s.target=self->swipeTarget(*o,direction);
            if (s.target) {
                s.targetOffset=s.target->m_renderOffset->value(); s.targetForced=s.target->m_forceRendering;
                s.targetAlpha=s.target->m_alpha->value(); s.targetAlphaGoal=s.target->m_alpha->goal(); s.targetAlphaAnimated=s.target->m_alpha->isBeingAnimated();
                s.target->m_forceRendering=true; s.target->space()->recalculate();
            }
        }
        static auto distance=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        delta=std::clamp(delta,-std::max(1.0,static_cast<double>(*distance)),std::max(1.0,static_cast<double>(*distance)));
        s.delta=delta; gesture->m_delta=delta;
        const double progress=s.target?std::clamp(std::abs(delta)/std::max(1.0,static_cast<double>(*distance)),0.0,1.0):0;
        const auto b=o->model.box(*o->model.index(s.tile));
        static auto gap=CConfigValue<Config::INTEGER>("general:gaps_workspaces");
        const auto allocation=StageController::tilePresentation(mon,box(b)).value_or(box(b));
        const double scale=allocation.w/b.width;
        const double travel=(s.vertical?allocation.h:allocation.w)+*gap*scale;
        s.source->m_alpha->setValueAndWarp(1.F);
        if (s.target) s.target->m_alpha->setValueAndWarp(1.F);
        s.sourcePreview=s.vertical?Vector2D{0.0,-direction*progress*travel}:Vector2D{-direction*progress*travel,0.0};
        s.source->m_renderOffset->setValueAndWarp(s.sourcePreview);
        if (s.target) { s.targetPreview=s.vertical?Vector2D{0.0,direction*(1-progress)*travel}:Vector2D{direction*(1-progress)*travel,0.0}; s.target->m_renderOffset->setValueAndWarp(s.targetPreview); }
        g_pHyprRenderer->damageMonitor(mon);
        static auto forever=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_forever");
        if (*forever && progress>=1) {
            const auto tile=s.tile; const auto session=gesture->m_sessionID; const auto serial=gesture->m_sessionSerial; const bool created=s.createdEmpty;
            swipeEnd(gesture);
            auto* current=self->output(mon);
            if (current && current->model.state().focused==tile && !gesture->isGestureInProgress() && gesture->m_sessionSerial==serial && swipeBegin(gesture,mon)) {
                gesture->m_sessionID=session; self->swipe->createdEmpty=created;
            }
        }
    }
    static void swipeEnd(CUnifiedWorkspaceSwipeGesture* gesture) {
        if (!self->swipe || self->swipe->native!=gesture) { reinterpret_cast<decltype(&swipeEnd)>(swipeEndHook->m_original)(gesture); return; }
        static auto distance=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        static auto ratio=CConfigValue<Config::FLOAT>("gestures:workspace_swipe_cancel_ratio");
        auto target=self->swipe->target; auto mon=self->swipe->monitor.lock();
        static auto force=CConfigValue<Config::INTEGER>("gestures:workspace_swipe_min_speed_to_force");
        const bool commit=target && std::abs(self->swipe->delta)>=2 && (std::abs(self->swipe->delta)>=std::max(1.0,static_cast<double>(*distance))*std::clamp(static_cast<double>(*ratio),0.0,1.0) || (*force>0 && self->swipe->speed>=*force));
        const auto session=*self->swipe;
        self->cancelSwipe();
        auto* o=self->output(mon);
        if (!o || !target || std::abs(session.delta)<2) return;
        if (commit) {
            auto draft=o->model;
            if (!draft.activate(workspaceKey(target)) || !self->publish(*o,std::move(draft))) return;
        }
        const auto logicalAllocation=box(o->model.box(*o->model.index(session.tile)));
        const auto allocation=StageController::tilePresentation(mon,logicalAllocation).value_or(logicalAllocation);
        static auto gap=CConfigValue<Config::INTEGER>("general:gaps_workspaces");
        const double travel=(session.vertical?allocation.h:allocation.w)+*gap*allocation.w/logicalAllocation.w;
        const auto end=session.vertical?Vector2D{0.0,session.direction*travel}:Vector2D{session.direction*travel,0.0};
        session.source->m_renderOffset->setValue(session.sourcePreview);
        target->m_renderOffset->setValue(session.targetPreview);
        *session.source->m_renderOffset=commit?-end:Vector2D{};
        *target->m_renderOffset=commit?Vector2D{}:end;
        session.source->m_alpha->setValueAndWarp(1.F); target->m_alpha->setValueAndWarp(1.F);
        if (!commit) {
            std::erase_if(o->outgoing,[&](const auto& v) { return v.workspace==target; });
            o->outgoing.push_back({mon,target,session.tile,logicalAllocation,false});
        } else g_pInputManager->unconstrainMouse();
        g_pHyprRenderer->damageMonitor(mon);
    }
    static void swipeCancel(CUnifiedWorkspaceSwipeGesture* gesture) {
        if (self->swipe && self->swipe->native==gesture) self->cancelSwipe();
        else reinterpret_cast<decltype(&swipeCancel)>(swipeCancelHook->m_original)(gesture);
    }
};

bool TilesController::Impl::install() {
    return attach("recheckWorkArea", &cameraArea, cameraAreaHook,20) && attach("dragEnd", &dragEnd, dragEndHook,20) && attach("logicalBox", &logicalBox, logicalBoxHook) && attach("scrollRecalculate", &scrollRecalculate, scrollRecalculateHook) &&
        attach("scrollFocus", &scrollFocus, scrollFocusHook) && attach("scrollFullscreenRequest", &scrollRequest, scrollRequestHook) && attach("scrollFullscreenColumn", &scrollColumn, scrollColumnHook) &&
        attach("mouseMoveUnified", &pointerInput, pointerHook) && attach("hasFullscreenWorkspace", &fsHas, fsHasHook) &&
        attach("fullscreenModesWorkspace", &fsModes, fsModesHook) && attach("fullscreenWindowWorkspace", &fsWindow, fsWindowHook) &&
        attach("setFullscreenFadeAnimation", &fsFade, fsFadeHook) && attach("renderLayer", &layer, layerHook) && attach("layerSurfaceAt", &layerHit, layerHitHook) &&
        attach("recheckWorkArea", &area, areaHook) && attach("windowAt", &hit, hitHook) &&
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
    if (session.source) {
        session.source->m_alpha->setValueAndWarp(session.sourceAlpha);
        if (session.sourceAlphaAnimated) *session.source->m_alpha=session.sourceAlphaGoal;
    }
    if (session.target) {
        session.target->m_alpha->setValueAndWarp(session.targetAlpha);
        if (session.targetAlphaAnimated) *session.target->m_alpha=session.targetAlphaGoal;
    }
    interaction=Owner::None;
    if (session.native) reinterpret_cast<decltype(&swipeCancel)>(hooks::nativeOriginal(swipeCancelHook->group->native))(session.native);
    if (session.source && session.sourceAnimated) {
        session.source->m_renderOffset->setValue(session.sourceOffset);
        *session.source->m_renderOffset=session.sourceGoal;
    }
    if (auto mon=session.monitor.lock()) g_pHyprRenderer->damageMonitor(mon);
}
bool TilesController::Impl::ensure() {
    if (!integer("tiles_enabled", 0)) return false;
    if (!initError.empty()) return false;
    if (ready) return true;
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
    if (o.notifiedGeneration!=o.model.state().generation) {
        o.notifiedGeneration=o.model.state().generation;
        if (topologyChanged) topologyChanged();
    }
}
void TilesController::Impl::apply(Output& o, bool focusWindow, bool warp, bool internal) {
    const auto mon = o.monitor.lock();
    if (!mon || applying) return;
    applying = true;
    Hyprutils::Utils::CScopeGuard reset{[this] { applying = false; }};
    if (!active(mon)) o.outgoing.clear();
    const auto old = mon->m_activeWorkspace;
    auto held = std::move(o.held);
    auto previous=std::move(o.bindings);
    auto constraintGuard=g_pInputManager->pointerConstraints().suspend();
    for (const auto& t : o.model.state().tiles) if (auto ws = workspaceForKey(t.workspace)) {
        homes[t.workspace]={mon,t.id};
        o.bindings[t.id]=ws;
        o.held.push_back(ws); ws->setVisible(true);
        const auto prior=previous.find(t.id);
        if (active(mon) && (prior==previous.end() ||
            (prior->second!=ws && overview && overview()))) {
            ws->m_renderOffset->setValueAndWarp(Vector2D{});
            ws->m_alpha->setValueAndWarp(1.F);
        }
        if (!internal && active(mon) && prior!=previous.end() && prior->second!=ws && !o.model.presented(workspaceKey(prior->second)) && !(overview && overview())) {
            const auto exiting=prior->second;
            bool left=false;
            if (ws->numberedID() && exiting->numberedID()) {
                static auto wrap=CConfigValue<Config::INTEGER>("animations:workspace_wraparound");
                auto low=workspaceKey(ws),high=low;
                for (const auto& candidate:State::workspaceState()->workspacesCopy()) if (candidate->numberedID()) {
                    low=std::min(low,workspaceKey(candidate)); high=std::max(high,workspaceKey(candidate));
                }
                const bool wraps=*wrap && std::min(workspaceKey(ws),workspaceKey(exiting))==low && std::max(workspaceKey(ws),workspaceKey(exiting))==high;
                left=wraps ^ (*ws->numberedID()>*exiting->numberedID());
            }
            const auto logicalAllocation=box(o.model.box(*o.model.index(t.id)));
            const auto allocation=StageController::tilePresentation(mon,logicalAllocation).value_or(logicalAllocation);
            std::erase_if(o.outgoing,[&](const auto& v) { return v.workspace==exiting; });
            o.outgoing.push_back({mon,exiting,t.id,logicalAllocation,false});
            if (!internal) g_pInputManager->releaseAllMouseButtons();
            for (const auto& [w,type]:{std::pair{exiting,Animation::Workspace::ANIMATION_TYPE_OUT},std::pair{ws,Animation::Workspace::ANIMATION_TYPE_IN}}) {
                static auto animations=CConfigValue<Config::INTEGER>("animations:enabled");
                const bool instant=!*animations;
                Animation::Workspace::startAnimation(w,type,left,instant,ws->m_animationStyle);
                // Preserve native style/config/callbacks and scale only the
                // horizontal displacement into this workspace's viewport.
                const auto value=w->m_renderOffset->value(),goal=w->m_renderOffset->goal();
                static auto workspaceGap=CConfigValue<Config::INTEGER>("general:gaps_workspaces");
                const auto style=ws->m_animationStyle.value_or(w->m_renderOffset->getStyle());
                const double gap=style.starts_with("slidefade")?0.0:static_cast<double>(*workspaceGap);
                const double tileGap=gap*allocation.w/logicalAllocation.w;
                const double sx=(allocation.w+tileGap)/std::max(1.0,mon->m_size.x+gap);
                const double sy=(allocation.h+tileGap)/std::max(1.0,mon->m_size.y+gap);
                w->m_renderOffset->setValue({value.x*sx,value.y*sy});
                *w->m_renderOffset=Vector2D{goal.x*sx,goal.y*sy};
                if (instant) w->m_renderOffset->warp();
            }
        }
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
        const auto allocation=box(o.model.box(*o.model.index(o.model.state().focused)));
        const auto b=StageController::tilePresentation(mon,allocation).value_or(allocation);
        Pointer::pointerController()->warpTo(b.pos() + b.size() / 2);
    }
    g_pHyprRenderer->damageMonitor(mon);
    notify(o, old);
    if (transfer && ((transfer->sourceMonitor==mon && transfer->sourceGeneration!=o.model.state().generation) ||
        (transfer->targetMonitor==mon && transfer->targetGeneration!=o.model.state().generation))) instance->finishTransfer(false);
}
void TilesController::Impl::collapse() {
    collapsing=true;
    Hyprutils::Utils::CScopeGuard restoreCameraScope{[this] { collapsing=false; }};
    if (transfer) { transfer.reset(); interaction=Owner::None; }
    endResize(true);
    cancelSwipe();
    for (auto& o : outputs) o->outgoing.clear();
    for (auto& o : outputs) if (auto mon = o->monitor.lock()) {
        auto keep = mon->m_activeWorkspace;
        for (const auto& w : o->held) if (w->m_monitor == mon) w->setVisible(w == keep);
    }
    outputs.clear();
    if (topologyChanged) topologyChanged();
    pointerWorkspace.reset();
    for (const auto& mon : State::monitorState()->monitors()) if (mon->m_activeWorkspace)
        Animation::Workspace::setFullscreenFadeAnimation(mon->m_activeWorkspace,Fullscreen::controller()->hasFullscreen(mon->m_activeWorkspace)?Animation::Workspace::ANIMATION_TYPE_IN:Animation::Workspace::ANIMATION_TYPE_OUT);
    for (const auto& mon : State::monitorState()->monitors()) { g_layoutManager->invalidateMonitorGeometries(mon); g_pHyprRenderer->damageMonitor(mon); }
    Desktop::globalWindowController()->updateSuspendedStates();
}
void TilesController::Impl::reconcile() {
    if (!integer("tiles_enabled",0)) for (auto& o:outputs) o->outgoing.clear();
    if (locked() || applying || stopping) return;
    if (!integer("tiles_enabled", 0) || !initError.empty()) { if (ready) { collapse(); resetHooks(); } return; }
    if (!ensure()) return;
    const auto removed=std::erase_if(outputs, [](const auto& o) { return o->monitor.expired() || !o->monitor->m_enabled || o->removing; });
    if (removed && topologyChanged) topologyChanged();
    for (const auto& mon : State::monitorState()->monitors()) {
        if (!mon->m_enabled || mon->isMirror() || !mon->m_activeWorkspace || !std::isfinite(mon->m_size.x) || !std::isfinite(mon->m_size.y) || mon->m_size.x <= 0 || mon->m_size.y <= 0) continue;
        auto* o = output(mon);
        if (!o) { outputs.push_back(std::make_unique<Output>(mon, std::max(1L, integer("tile_min_width", 240)))); continue; }
        if (!active(mon)) o->outgoing.clear();
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

TilesController::TilesController(HANDLE h, std::function<bool()> o,std::function<void()> changed,std::function<bool()> grouped) : m(std::make_unique<Impl>(h, std::move(o),std::move(changed),std::move(grouped))) { instance = this; Impl::self = m.get(); }
TilesController::~TilesController() { m->stopping = true; m->deferred.reset(); m->resizeDeferred.reset(); m->listeners.clear(); if (m->ready) m->collapse(); m->resetHooks(); Impl::self = nullptr; instance = nullptr; }
TilesController* TilesController::get() { return instance; }
void TilesController::initialize() {
    auto& e = Event::bus()->m_events;
    m->listeners.emplace_back(e.config.reloaded.listen([this] { m->request(); }));
    m->listeners.emplace_back(e.monitor.layoutChanged.listen([this] { finishTransfer(false); m->request(); }));
    m->listeners.emplace_back(e.monitor.focused.listen([this](PHLMONITOR) { if (m->ready && m->topologyChanged) m->topologyChanged(); }));
    m->listeners.emplace_back(e.monitor.added.listen([this](PHLMONITOR) { m->request(); }));
    m->listeners.emplace_back(e.monitor.preRemoved.listen([this](PHLMONITOR mon) {
        if (auto* o=m->output(mon)) o->removing=true;
        m->endResize(true); m->cancelSwipe(); finishTransfer(false); m->request();
    }));
    m->listeners.emplace_back(e.monitor.removed.listen([this](PHLMONITOR) { finishTransfer(false); m->request(); }));
    m->listeners.emplace_back(e.workspace.created.listen([this](PHLWORKSPACEREF) { m->request(); }));
    m->listeners.emplace_back(e.workspace.active.listen([this](PHLWORKSPACE) { m->request(); }));
    m->listeners.emplace_back(e.workspace.removed.listen([this](PHLWORKSPACEREF) { finishTransfer(false); m->request(); }));
    m->listeners.emplace_back(e.workspace.moveToMonitor.listen([this](PHLWORKSPACE, PHLMONITOR) { finishTransfer(false); m->request(); }));
    m->listeners.emplace_back(e.window.close.listen([this](PHLWINDOW w) { if (transferMember(w)) finishTransfer(false); }));
    m->listeners.emplace_back(e.window.destroy.listen([this](const PHLWINDOWREF& ref) {
        if (m->transfer && std::ranges::find(m->transfer->windows,ref)!=m->transfer->windows.end()) finishTransfer(false);
    }));
    m->listeners.emplace_back(e.window.moveToWorkspace.listen([this](PHLWINDOW w,PHLWORKSPACE) { if (transferMember(w) && m->transfer && w->m_workspace!=m->transfer->sourceWorkspace) finishTransfer(false); }));
    m->listeners.emplace_back(e.render.preChecks.listen([this](PHLMONITOR mon) { if (m->active(mon)) mon->m_solitaryClient.reset(); }));
    m->listeners.emplace_back(e.render.stage.listen([this](Event::SRenderStageEvent event) { m->chrome(event); }));
    m->listeners.emplace_back(e.input.mouse.move.listen([this](const Vector2D& pos, Event::SCallbackInfo& info) { m->pointer(pos,info); }));
    m->listeners.emplace_back(e.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
        const auto* drag=g_layoutManager->dragController().get();
        if (m->ready && event.keycode==KEY_ESC && event.state==WL_KEYBOARD_KEY_STATE_PRESSED && drag && drag->mode()==MBIND_MOVE && drag->target())
            m->nativeDropCancelled=true;
        if (m->resize && event.keycode==KEY_ESC && event.state==WL_KEYBOARD_KEY_STATE_PRESSED) { m->endResize(true); info.cancelled=true; }
    }));
    m->listeners.emplace_back(e.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        if (m->dividerButton(event,info) || info.cancelled) return;
        if (locked() || event.state != WL_POINTER_BUTTON_STATE_PRESSED || m->interaction != Owner::None || (m->overview && m->overview())) return;
        const auto pos = g_pInputManager->getMouseCoordsInternal();
        auto mon = State::monitorState()->query().vec(pos).run();
        if (m->active(mon) && !mon->m_activeSpecialWorkspace) { const auto i = m->at(*m->output(mon),pos); if (i) focus(mon, m->output(mon)->model.state().tiles[*i].id); }
    }));
    if (g_pSessionLockManager) m->listeners.emplace_back(g_pSessionLockManager->m_events.lock.listen([this] { m->endResize(true); m->cancelSwipe(); finishTransfer(false); }));
    if (g_pSessionLockManager) m->listeners.emplace_back(g_pSessionLockManager->m_events.unlock.listen([this] { m->request(); }));
    m->request();
}
std::vector<TilesController::View> TilesController::views(const PHLMONITOR& mon) const {
    std::vector<View> out;
    auto* o = m->output(mon);
    if (m->ready && o) for (size_t i = 0; i < o->model.state().tiles.size(); ++i) {
        const auto& t = o->model.state().tiles[i];
        out.push_back({mon, workspaceForKey(t.workspace), t.id, box(o->model.box(i)), t.id == o->model.state().focused});
    }
    return out;
}
bool TilesController::enabled(const PHLMONITOR& mon) const { return m->active(mon); }
bool TilesController::available() const { return m->ready && m->initError.empty(); }
bool TilesController::handleDivider(const IPointer::SButtonEvent& event,Event::SCallbackInfo& info) { return m->dividerButton(event,info); }
void TilesController::fail(std::string error) { m->initError="tiles disabled: "+std::move(error); m->request(); }
tiles::Id TilesController::preferredTile(const PHLWORKSPACE& ws, const PHLMONITOR& mon) const {
    auto* o=m->output(mon);
    if (!m->ready || !o) return 0;
    if (ws) {
        if (const auto i=o->model.presented(workspaceKey(ws))) return o->model.state().tiles[*i].id;
        if (const auto it=m->homes.find(workspaceKey(ws));it!=m->homes.end() && it->second.first==mon && o->model.index(it->second.second)) return it->second.second;
        size_t rank=SIZE_MAX; tiles::Id id=0;
        for (const auto& t:o->model.state().tiles) {
            auto it=std::ranges::find(t.history,workspaceKey(ws));
            if (it!=t.history.end() && static_cast<size_t>(it-t.history.begin())<rank) { rank=it-t.history.begin(); id=t.id; }
        }
        if (id) return id;
    }
    return o->model.state().tiles.front().id;
}
std::optional<TilesController::View> TilesController::view(const PHLWORKSPACE& w) const { return m->view(w); }
bool TilesController::presented(const PHLWORKSPACE& w) const { return m->view(w).has_value(); }
bool TilesController::focus(const PHLMONITOR& mon, tiles::Id id, bool warp) {
    auto* o = m->output(mon);
    if (!o || !m->ready || locked()) return false;
    const auto i = o->model.index(id);
    if (!i) return false;
    auto draft=o->model;
    if (!draft.activate(draft.state().tiles[*i].workspace)) return false;
    return m->publish(*o,std::move(draft),{},nullptr,true,warp);
}
bool TilesController::activate(const PHLMONITOR& mon, tiles::Id id, const PHLWORKSPACE& ws, bool swap) {
    auto* o = m->output(mon);
    if (!o || !ws || ws->m_monitor != mon || ws->type() == Workspace::eWorkspaceType::SPECIAL || locked() || !o->model.index(id)) return false;
    auto draft=o->model;
    auto state=draft.state(); state.focused=id;
    if (!draft.restore(state,state.generation) || !draft.activate(workspaceKey(ws),swap?tiles::Activation::Swap:tiles::Activation::Focus)) return false;
    return m->publish(*o,std::move(draft));
}
bool TilesController::moveWindow(const PHLWINDOW& w, const PHLMONITOR& mon, tiles::Id id) {
    auto* o = m->output(mon);
    if (!o || !w || !w->mapped() || pinned(w) || locked()) return false;
    const auto i = o->model.index(id);
    auto ws = i ? workspaceForKey(o->model.state().tiles[*i].workspace) : nullptr;
    if (!ws) return false;
    auto draft=o->model;
    auto state=draft.state(); state.focused=id;
    if (!draft.restore(state,draft.state().generation) || !m->publish(*o,std::move(draft),{w},ws)) return false;
    Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_KEYBIND);
    return true;
}
bool TilesController::moveWindows(const std::vector<PHLWINDOW>& windows,const PHLMONITOR& mon,tiles::Id id,const PHLWORKSPACE& destination) {
    auto* o=m->output(mon);
    if (!m->ready || !o || windows.empty() || !o->model.index(id) || !destination || destination->m_monitor!=mon || destination->type()==Workspace::eWorkspaceType::SPECIAL || locked()) return false;
    const auto key=workspaceKey(destination);
    const auto previous=m->homes.find(key);
    const auto saved=previous==m->homes.end()?std::optional<std::pair<PHLMONITORREF,tiles::Id>>{}:previous->second;
    m->homes[key]={mon,id};
    if (m->publish(*o,o->model,windows,destination,false)) return true;
    if (saved) m->homes[key]=*saved; else m->homes.erase(key);
    return false;
}
bool TilesController::busy() const { return m->interaction != Owner::None; }
bool TilesController::acquire(Owner owner) { if (!m->ready) return true; if (locked() || owner == Owner::None || busy()) return false; m->interaction = owner; return true; }
void TilesController::release(Owner owner) { if (m->interaction == owner) m->interaction = Owner::None; }
TilesController::Owner TilesController::owner() const { return m->interaction; }
void TilesController::edgeError(std::string e) { if (!e.empty()) { m->edgeFailure=std::move(e); m->softEdgeDrawn=false; } }
void TilesController::edgeActive() { m->softEdgeDrawn=true; m->edgeFailure.clear(); }
bool TilesController::beginTransfer(const std::vector<PHLWINDOW>& windows,const PHLMONITOR& mon,tiles::Id id,const CBox& origin,const Vector2D& press) {
    auto* o=m->output(mon);
    if (!m->ready || !o || !o->model.index(id) || windows.empty() || !acquire(Owner::StageDrag)) return false;
    Impl::Transfer transfer;
    transfer.sourceMonitor=mon; transfer.source=id; transfer.sourceGeneration=o->model.state().generation;
    transfer.origin=origin; transfer.press=transfer.pointer=press;
    for (const auto& w:windows) {
        if (!w || !w->mapped() || pinned(w) || !w->m_workspace || w->onSpecialWorkspace()) { release(Owner::StageDrag); return false; }
        if (transfer.sourceWorkspace && transfer.sourceWorkspace!=w->m_workspace) { release(Owner::StageDrag); return false; }
        transfer.windows.push_back(w);
        if (!transfer.sourceWorkspace) transfer.sourceWorkspace=w->m_workspace;
    }
    m->transfer=std::move(transfer);
    return true;
}
bool TilesController::transferDestination(const PHLMONITOR& source,tiles::Id tile,const Vector2D& point,const PHLMONITOR& columnMonitor,tiles::Id column) const {
    const auto mon=columnMonitor?columnMonitor:State::monitorState()->query().vec(point).run();
    const auto* o=m->output(mon);
    if (!m->ready || !o || !mon || mon->m_activeSpecialWorkspace || locked()) return false;
    if (column) return mon!=source || column!=tile;
    const auto input=m->inputPosition(mon,point);
    const auto target=tiles::dropTarget(o->model,mon==source?tile:0,input.x,input.y,false);
    return target.kind==tiles::DropKind::SplitLeft || target.kind==tiles::DropKind::SplitRight ||
        (target.kind==tiles::DropKind::Move && (mon!=source || target.target!=tile));
}
bool TilesController::updateTransfer(const Vector2D& point,const PHLMONITOR& columnMonitor,tiles::Id column,const PHLWORKSPACE& target) {
    if (!m->transfer) return false;
    auto& t=*m->transfer;
    auto* source=m->output(t.sourceMonitor.lock());
    if (!source || source->model.state().generation!=t.sourceGeneration || locked()) { finishTransfer(false); return false; }
    t.pointer=point;
    t.dragging |= point.distanceSq(t.press)>64;
    if (!t.dragging) return true;
    auto previous=t.targetMonitor.lock();
    auto mon=columnMonitor?columnMonitor:State::monitorState()->query().vec(point).run();
    auto* o=m->output(mon);
    t.targetMonitor=mon; t.targetWorkspace=target; t.drop={};
    if (o && !mon->m_activeSpecialWorkspace) {
        t.targetGeneration=o->model.state().generation;
        if (!t.origin.containsPoint(point)) {
            const auto input=m->inputPosition(mon,point);
            t.drop=tiles::dropTarget(o->model,mon==t.sourceMonitor?t.source:0,input.x,input.y,column!=0);
            if (column && o->model.index(column)) {
                t.drop.target=column;
                t.drop.preview=o->model.box(*o->model.index(column));
                t.drop.kind=tiles::DropKind::Move;
            }
            if (const auto mapped=StageController::tilePresentation(mon,box(t.drop.preview))) {
                if (t.drop.divider) t.drop.divider=StageController::tilePresentation(mon,CBox{t.drop.divider,t.drop.preview.y,1.0,1.0})->x;
                t.drop.preview={mapped->x,mapped->y,mapped->w,mapped->h};
            }
        }
    }
    if (previous) g_pHyprRenderer->damageMonitor(previous);
    if (mon) g_pHyprRenderer->damageMonitor(mon);
    return true;
}
bool TilesController::finishTransfer(bool commit) {
    if (!m->transfer) return false;
    auto t=std::move(*m->transfer); m->transfer.reset();
    release(Owner::StageDrag);
    if (auto mon=t.sourceMonitor.lock()) g_pHyprRenderer->damageMonitor(mon);
    if (auto mon=t.targetMonitor.lock()) g_pHyprRenderer->damageMonitor(mon);
    if (!commit || !t.dragging || t.drop.kind==tiles::DropKind::Cancel || t.drop.kind==tiles::DropKind::Invalid || locked()) return false;
    auto* source=m->output(t.sourceMonitor.lock()); auto* target=m->output(t.targetMonitor.lock());
    if (!source || !target || source->model.state().generation!=t.sourceGeneration || target->model.state().generation!=t.targetGeneration || !target->model.index(t.drop.target)) return false;
    std::vector<PHLWINDOW> windows;
    for (const auto& ref:t.windows) { auto w=ref.lock(); if (!w || !w->mapped() || w->m_workspace!=t.sourceWorkspace) return false; windows.push_back(w); }
    auto draft=target->model;
    auto state=draft.state(); state.focused=t.drop.target;
    if (!draft.restore(state,draft.state().generation)) return false;
    const bool split=t.drop.kind==tiles::DropKind::SplitLeft || t.drop.kind==tiles::DropKind::SplitRight;
    PHLWORKSPACE destination;
    if (split) {
        if (draft.state().tiles.size()>=tiles::maxTiles) return false;
        destination=m->fresh(t.targetMonitor.lock());
        if (!destination || destination->m_monitor!=t.targetMonitor || !draft.add(workspaceKey(destination),{},t.drop.kind==tiles::DropKind::SplitLeft)) return false;
    } else {
        destination=t.targetWorkspace?t.targetWorkspace:workspaceForKey(draft.state().tiles[*draft.index(t.drop.target)].workspace);
        if (!destination || destination->m_monitor!=t.targetMonitor || !draft.activate(workspaceKey(destination))) return false;
    }
    return m->publish(*target,std::move(draft),windows,destination);
}
bool TilesController::transferDragging() const { return m->transfer && m->transfer->dragging; }
bool TilesController::transferMember(const PHLWINDOW& w) const { return m->transfer && std::ranges::find(m->transfer->windows,w)!=m->transfer->windows.end(); }
std::vector<PHLWINDOW> TilesController::transferMembers() const { std::vector<PHLWINDOW> out; if (m->transfer) for (auto& w:m->transfer->windows) if (auto p=w.lock()) out.push_back(p); return out; }
Vector2D TilesController::transferPoint() const { return m->transfer?m->transfer->pointer:Vector2D{}; }
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
    auto draft=o->model;
    tiles::Result result;
    PHLWORKSPACE pendingWorkspace; // native state stores weak refs; retain through publication
    if (action == "tile_add") {
        if (draft.state().tiles.size() >= tiles::maxTiles) return {.success=false,.error="at most three tiles per monitor"};
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
        result = draft.add(workspaceKey(ws), width);
        pendingWorkspace = ws;
    } else if (action == "tile_remove") result = draft.remove();
    else if (action == "tile_resize") {
        if (fields.contains("width") == fields.contains("delta")) return {.success=false,.error="specify width or delta"};
        const bool delta = fields.contains("delta");
        auto n = tiles::length(fields[delta ? "delta" : "width"], mon->m_size.x, delta);
        if (!n) return {.success=false,.error=n.error()};
        int neighbor = 0;
        if (fields.contains("neighbor")) { if (fields["neighbor"]=="left") neighbor=-1; else if (fields["neighbor"]=="right") neighbor=1; else return {.success=false,.error="neighbor must be left or right"}; }
        result = draft.resize(*n + (delta ? draft.state().tiles[*draft.index(draft.state().focused)].width : 0), neighbor);
    } else if (action == "tile_focus" || action == "tile_move_window") {
        const auto direction = fields["direction"];
        if (direction!="left" && direction!="right") return {.success=false,.error="direction must be left or right"};
        if (action=="tile_move_window") {
            const int j=static_cast<int>(*draft.index(draft.state().focused))+(direction=="left"?-1:1);
            if (j<0 || j>=static_cast<int>(draft.state().tiles.size())) return {.success=false,.error="no adjacent tile"};
            return moveWindow(Desktop::focusState()->window(),mon,draft.state().tiles[j].id) ? SDispatchResult{} : SDispatchResult{.success=false,.error="window move failed"};
        }
        result = draft.focus(direction=="left"?-1:1);
    } else return {.success=false,.error="unknown tile action"};
    if (!result) return {.success=false,.error=result.error()};
    if (!m->publish(*o,std::move(draft))) return {.success=false,.error="tile activation transaction failed"};
    return {};
}
std::string TilesController::stateJson() const {
    nlohmann::json monitors=nlohmann::json::array(), tiles=nlohmann::json::array();
    for (const auto& o : m->outputs) if (auto mon=o->monitor.lock()) {
        nlohmann::json entries=nlohmann::json::array();
        for (size_t i=0;i<o->model.state().tiles.size();++i) {
            const auto& t=o->model.state().tiles[i]; const auto b=o->model.box(i); const auto ws=workspaceForKey(t.workspace);
            auto entry=nlohmann::json{{"id",t.id},{"name",mon->m_name+"/tile-"+std::to_string(t.id)},{"monitor",mon->m_name},{"x",b.x},{"y",b.y},{"width",b.width},{"height",b.height},
                {"scale",mon->m_scale},{"transform",static_cast<int>(mon->m_transform)},{"focused", t.id==o->model.state().focused && Desktop::focusState()->monitor()==mon}, {"activeWorkspace",{{"id",t.workspace},{"name",ws?ws->displayName():""},{"address",ws?ws->addressableName():""},{"selector",ws?(ws->numberedID()?ws->addressableName():"name:"+ws->addressableName()):""}}},{"history",t.history},{"renderOffset",ws?nlohmann::json::array({ws->m_renderOffset->value().x,ws->m_renderOffset->value().y}):nlohmann::json::array({0,0})}};
            entries.push_back(entry); tiles.push_back(entry);
        }
        monitors.push_back({{"name",mon->m_name},{"generation",o->model.state().generation},{"outgoing_workspace_count",o->outgoing.size()},{"tiles",entries}});
    }
    return dumpStateJson({{"version",1},{"backend","plugin"},{"enabled",m->ready},{"max_tiles",tiles::maxTiles},{"initError",m->initError},{"interaction_owner",static_cast<int>(m->interaction)},
        {"edge_effect",m->softEdgeDrawn?"blur_fade":"hard_clip"},{"edge_error",m->edgeFailure},{"monitors",monitors},{"tiles",tiles}})+"\n";
}
} // namespace hymission
