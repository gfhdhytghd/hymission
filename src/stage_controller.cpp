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
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
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
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

// CSpace has no plugin work-area contributor API. Change only its freshly
// computed boxes after the original function, never monitor/bar reservations.
#define private public
#include <hyprland/src/layout/space/Space.hpp>
#undef private

#include "vendor/nlohmann/json.hpp"

namespace hymission {
namespace {
using Render::GL::g_pHyprOpenGL;
using Clock = std::chrono::steady_clock;

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
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    bool undiscardable() override { return true; }
    bool disableSimplification() override { return true; }
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
    struct Card {
        PHLWORKSPACEREF workspace;
        SP<Render::IFramebuffer> snapshot;
        SP<Render::ITexture> label;
        std::string labelText;
        bool dirty = true;
        bool snapshotReady = false;
    };
    struct Screen {
        PHLMONITORREF monitor;
        CBox base;
        stage::Geometry geometry;
        std::vector<Card> cards;
        SP<Render::IFramebuffer> scratch;
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
    };

    using RecheckFn = void (*)(Layout::CSpace*);
    using DragEndFn = void (*)(Layout::Supplementary::CDragStateController*);
    using WindowAtFn = PHLWINDOW (*)(const Desktop::CViewHitTester*, const Vector2D&, uint16_t, PHLWINDOW);
    using RenderWindowFn = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
    inline static Impl* instance = nullptr;

    HANDLE handle;
    std::function<bool()> overviewSuspended;
    std::vector<Screen> screens;
    std::vector<CHyprSignalListener> listeners;
    CFunctionHook* areaHook = nullptr;
    CFunctionHook* dragHook = nullptr;
    CFunctionHook* hitHook = nullptr;
    RenderWindowFn renderWindow = nullptr;
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
    void snapshots();
    bool snapshot(Screen& screen, Card& card);
    bool blocked() const;
    bool interactive(const Screen& screen) const;
    Screen* screenFor(const PHLMONITOR& monitor);
    std::pair<Screen*, std::optional<std::size_t>> hit(const Vector2D& point);
    void pointer();
    void button(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info);
    void axis(const IPointer::SAxisEvent& event, Event::SCallbackInfo& info);
    void activate(const PHLWORKSPACE& workspace);
    void correctFloating(const PHLMONITOR& monitor, const CBox& desktop);
    void damage(const Screen& screen);
    std::string stateJson() const;

    static void recheckThunk(Layout::CSpace* space) {
        auto* self = instance;
        reinterpret_cast<RecheckFn>(self->areaHook->m_original)(space);
        self->afterRecheck(space);
    }
    static void dragThunk(Layout::Supplementary::CDragStateController* drag) { instance->endDrag(drag); }
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

bool StageController::Impl::interactive(const Screen& screen) const {
    return enabled && !rendering && !blocked() && screen.geometry.enabled() && !screen.covered && !screen.suspended;
}

StageController::Impl::Screen* StageController::Impl::screenFor(const PHLMONITOR& monitor) {
    const auto it = std::ranges::find_if(screens, [&](const auto& s) { return s.monitor == monitor; });
    return it == screens.end() ? nullptr : &*it;
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
        return nullptr;
    };
    const auto area = find("recheckWorkArea", "Layout::CSpace::recheckWorkArea()");
    const auto drag = find("dragEnd", "Layout::Supplementary::CDragStateController::dragEnd()");
    const auto hit = find("windowAt", "Desktop::CViewHitTester::windowAt(");
    renderWindow = reinterpret_cast<RenderWindowFn>(find("renderWindow", "IHyprRenderer::renderWindow("));
    if (area && drag && hit && renderWindow && g_pHyprOpenGL) {
        areaHook = HyprlandAPI::createFunctionHook(handle, area, reinterpret_cast<void*>(&recheckThunk));
        dragHook = HyprlandAPI::createFunctionHook(handle, drag, reinterpret_cast<void*>(&dragThunk));
        hitHook = HyprlandAPI::createFunctionHook(handle, hit, reinterpret_cast<void*>(&windowAtThunk));
        hooksReady = areaHook && dragHook && hitHook && areaHook->hook() && dragHook->hook() && hitHook->hook();
    }
    if (!hooksReady) {
        releaseHooks();
        hookFailure = true;
        error = "stage disabled: work-area / native-drag / hit-test / window-render hook or OpenGL renderer unavailable";
        Log::logger->log(Log::ERR, "[hymission] {}", error);
        HyprlandAPI::addNotification(handle, "[hymission] " + error, CHyprColor(1.0, 0.3, 0.2, 1.0), 8000);
    }
    return hooksReady;
}

void StageController::Impl::releaseHooks() {
    for (auto** hook : {&areaHook, &dragHook, &hitHook}) {
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
    if (g_pHyprOpenGL && !screens.empty())
        g_pHyprOpenGL->makeEGLCurrent(); // card/output removal may destroy GL resources
    syncing = true;
    const bool wanted = setting("stage_enabled", 0) != 0;
    const bool wasEnabled = enabled;
    enabled = wanted && installHooks();
    if (!enabled) {
        stopTimer(refreshTimer);
        stopTimer(motionTimer);
        auto previous = std::move(screens);
        screens.clear();
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

    const stage::Settings raw{static_cast<double>(setting("stage_card_min_width", 120)), static_cast<double>(setting("stage_card_max_width", 240)),
                              static_cast<double>(setting("stage_padding", 12)), static_cast<double>(setting("stage_card_gap", 12)),
                              static_cast<double>(setting("stage_desktop_gap", 12))};
    settings = stage::normalize(raw);
    if ((reconfigure || !wasEnabled) && (raw.minWidth != settings.minWidth || raw.maxWidth != settings.maxWidth || raw.padding != settings.padding || raw.cardGap != settings.cardGap || raw.desktopGap != settings.desktopGap))
        Log::logger->log(Log::WARN, "[hymission] normalized invalid stage widths/padding/gaps");
    const bool nextMaximizeCover = setting("stage_maximize_cover_strip", 0) != 0;
    const bool changedPolicy = maximizeCover != nextMaximizeCover;
    maximizeCover = nextMaximizeCover;
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
    for (const auto& monitor : State::monitorState()->monitors()) {
        auto* screen = screenFor(monitor);
        if (!screen) {
            screens.emplace_back();
            screen = &screens.back();
            screen->monitor = monitor;
        }
        const auto base = baseArea(monitor);
        const auto oldGeometry = screen->geometry;
        const bool changedOutput = !sameBox(base, screen->base) || screen->scale != monitor->m_scale || screen->transform != static_cast<int>(monitor->m_transform);
        screen->base = base;
        screen->scale = monitor->m_scale;
        screen->transform = static_cast<int>(monitor->m_transform);

        std::vector<PHLWORKSPACE> targets;
        for (const auto& workspace : workspaces) {
            if (workspace->m_isSpecialWorkspace || workspace->m_monitor != monitor)
                continue;
            if (!showEmpty && workspace->getWindowCount() == 0)
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
        screen->geometry = stage::layout(base.w, base.h, targets.size(), settings, screen->frozenWidth);
        const bool changedGeometry = changedOutput || oldGeometry.reservation != screen->geometry.reservation || oldGeometry.cardHeight != screen->geometry.cardHeight;
        screen->scroll = screen->geometry.clampScroll(screen->scroll);
        const WORKSPACEID active = monitor->m_activeWorkspace ? monitor->m_activeWorkspace->m_id : WORKSPACE_INVALID;
        if ((!dragged && (changedCards || changedGeometry)) || active != screen->active) {
            for (std::size_t i = 0; i < targets.size(); ++i) {
                if (targets[i]->m_id == active)
                    screen->scroll = screen->geometry.reveal(i, screen->scroll);
            }
        }
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
        if (changedGeometry || changedPolicy || changedCover || reconfigure)
            relayout.push_back(monitor);
        if (changedOutput)
            screen->scratch.reset();
        for (auto& card : screen->cards) {
            if (changedGeometry) {
                card.snapshot.reset();
                card.snapshotReady = false;
                card.label.reset();
            }
            card.dirty |= forceRefresh || changedGeometry || changedCards || changedActive || changedSuspension || changedCover;
        }
        if (changedGeometry || changedCards || changedActive || changedCover || changedSuspension)
            damage(*screen);
    }
    for (const auto& monitor : relayout) {
        g_layoutManager->invalidateMonitorGeometries(monitor);
        if (auto* screen = screenFor(monitor); screen && screen->geometry.enabled())
            correctFloating(monitor, CBox{screen->base.x + screen->geometry.reservation, screen->base.y, screen->geometry.desktopWidth, screen->base.h});
    }
    forceRefresh = false;
    reconfigure = false;
    syncing = false;
    snapshots();
    armRefresh();
}

void StageController::Impl::armRefresh() {
    stopTimer(refreshTimer);
    if (!enabled || !g_pEventLoopManager)
        return;
    const auto interval = std::max(16L, setting("stage_refresh_ms", 500));
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
    auto* drag = g_layoutManager->dragController().get();
    const bool dragging = drag && drag->mode() == MBIND_MOVE && drag->target();
    const auto point = g_pInputManager->getMouseCoordsInternal();
    for (auto& screen : screens) {
        const double target = screen.covered ? 0 : 1;
        if (screen.shown != target) {
            const auto t = std::clamp(std::chrono::duration<double, std::milli>(now - screen.slideStart).count() / 180.0, 0.0, 1.0);
            const auto eased = 1 - std::pow(1 - t, 3);
            screen.shown = t == 1 ? target : screen.slideFrom + (target - screen.slideFrom) * eased;
            damage(screen);
            again |= t < 1;
        }
        const auto& geometry = screen.geometry;
        if (!dragging || !interactive(screen) || point.x < screen.base.x || point.x >= screen.base.x + geometry.bandWidth ||
            point.y < screen.base.y || point.y >= screen.base.y + screen.base.h)
            continue;
        const double y = point.y - screen.base.y;
        const double delta = y < 32 ? -8 : y > screen.base.h - 32 ? 8 : 0;
        const double scroll = geometry.clampScroll(screen.scroll + delta);
        if (scroll != screen.scroll) {
            screen.scroll = scroll;
            screen.hovered = geometry.hit(point.x - screen.base.x, y, scroll);
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
        if (point.x >= screen.base.x && point.x < screen.base.x + screen.geometry.bandWidth && point.y >= screen.base.y && point.y < screen.base.y + screen.base.h)
            return {&screen, screen.geometry.hit(point.x - screen.base.x, point.y - screen.base.y, screen.scroll)};
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
    if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;
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
    if (enabled && !cancelDrop && drag->mode() == MBIND_MOVE && drag->dragThresholdReached() && drag->target()) {
        const auto [screen, index] = hit(g_pInputManager->getMouseCoordsInternal());
        if (screen && index) {
            destination = screen->cards[*index].workspace.lock();
            window = drag->target()->window();
        }
    }
    reinterpret_cast<DragEndFn>(dragHook->m_original)(drag);
    // Use the compositor's normal move path after its drag controller has
    // restored tiling/floating and completed the pointer grab.
    if (destination && window && window->m_isMapped && window->m_workspace != destination &&
        State::workspaceState()->query().id(destination->m_id).run() == destination && destination->m_monitor) {
        const auto monitor = destination->m_monitor.lock();
        Desktop::globalWindowController()->moveWindowToWorkspace(window, destination);
        if (auto* screen = screenFor(monitor))
            correctFloating(monitor, CBox{screen->base.x + screen->geometry.reservation, screen->base.y, screen->geometry.desktopWidth, screen->base.h});
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
    auto* screen = screenFor(monitor);
    if (!screen || !screen->geometry.enabled() || monitor->m_activeSpecialWorkspace || screen->shown <= 0)
        return;
    if (stage == RENDER_POST_WINDOWS) {
        const PHLMONITORREF ref = monitor;
        g_pHyprRenderer->m_renderPass.add(makeUnique<StagePassElement>([this, ref] {
            if (const auto mon = ref.lock())
                draw(mon);
        }, CBox{screen->base.x - monitor->m_position.x, screen->base.y - monitor->m_position.y, screen->geometry.bandWidth, screen->base.h}));
    }
}

void StageController::Impl::draw(const PHLMONITOR& monitor) {
    auto* screen = screenFor(monitor);
    if (!screen || blocked() || rendering || !g_pHyprOpenGL)
        return;
    const auto& geometry = screen->geometry;
    const double slide = -(1 - screen->shown) * geometry.bandWidth;
    const auto physical = [&](CBox box) { return box.translate(-monitor->m_position).scale(monitor->m_scale); };
    const auto previousClip = g_pHyprRenderer->m_renderData.clipBox;
    g_pHyprRenderer->m_renderData.clipBox = physical(CBox{screen->base.x, screen->base.y, geometry.bandWidth, screen->base.h});
    g_pHyprOpenGL->renderRect(physical(CBox{screen->base.x + slide, screen->base.y, geometry.bandWidth, screen->base.h}), CHyprColor(0.035, 0.045, 0.06, 1.0), {});
    g_pHyprRenderer->m_renderData.clipBox = physical(CBox{screen->base.x, screen->base.y + geometry.padding, geometry.bandWidth, screen->base.h - 2 * geometry.padding});
    for (std::size_t i = 0; i < screen->cards.size(); ++i) {
        auto& card = screen->cards[i];
        const double top = geometry.cardTop(i, screen->scroll);
        if (top + geometry.cardHeight <= geometry.padding || top >= screen->base.h - geometry.padding)
            continue;
        const auto workspace = card.workspace.lock();
        if (!workspace)
            continue;
        const CBox box{screen->base.x + geometry.padding + slide, screen->base.y + top, geometry.cardWidth, geometry.cardHeight};
        const bool active = workspace->m_id == screen->active;
        const bool hover = screen->hovered == i && !screen->covered;
        g_pHyprOpenGL->renderRect(physical(box), active ? CHyprColor(0.13, 0.19, 0.27, 1.0) : CHyprColor(0.075, 0.085, 0.10, 1.0), {});
        if (card.snapshotReady && card.snapshot)
            g_pHyprOpenGL->renderTexture(card.snapshot->getTexture(), physical(box), {});
        if (hover)
            g_pHyprOpenGL->renderRect(physical(box), CHyprColor(0.4, 0.65, 1.0, 0.15), {});
        if (active || hover) {
            const CHyprColor color = hover ? CHyprColor(0.6, 0.82, 1.0, 1.0) : CHyprColor(0.3, 0.6, 1.0, 1.0);
            for (const auto& edge : {CBox{box.x, box.y, box.w, 2}, CBox{box.x, box.y + box.h - 2, box.w, 2},
                                    CBox{box.x, box.y, 2, box.h}, CBox{box.x + box.w - 2, box.y, 2, box.h}})
                g_pHyprOpenGL->renderRect(physical(edge), color, {});
        }
        if (card.label) {
            const double labelHeight = std::min(geometry.cardHeight, card.label->m_size.y / monitor->m_scale);
            const double labelWidth = std::min(geometry.cardWidth, card.label->m_size.x / monitor->m_scale);
            const CBox labelBox{box.x + 4, box.y + 3, std::max(1.0, labelWidth), labelHeight};
            g_pHyprOpenGL->renderRect(physical(CBox{box.x + 2, box.y + 2, std::min(box.w - 4, labelWidth + 4), labelHeight + 2}), CHyprColor(0, 0, 0, 0.65), {});
            g_pHyprOpenGL->renderTexture(card.label, physical(labelBox), {});
        }
    }
    g_pHyprRenderer->m_renderData.clipBox = previousClip;
}

void StageController::Impl::snapshots() {
    if (!enabled || blocked() || rendering || g_pHyprRenderer->m_renderData.pMonitor)
        return;
    for (auto& screen : screens) {
        if (!interactive(screen))
            continue;
        bool changed = false;
        for (std::size_t i = 0; i < screen.cards.size(); ++i) {
            auto& card = screen.cards[i];
            const auto top = screen.geometry.cardTop(i, screen.scroll);
            if (!card.dirty || top + screen.geometry.cardHeight <= screen.geometry.padding || top >= screen.base.h - screen.geometry.padding)
                continue;
            if (snapshot(screen, card)) {
                card.dirty = false;
                changed = true;
            }
        }
        if (changed)
            damage(screen);
    }
}

bool StageController::Impl::snapshot(Screen& screen, Card& card) {
    card.snapshotReady = false;
    const auto monitor = screen.monitor.lock();
    const auto workspace = card.workspace.lock();
    if (!monitor || !workspace || !g_pHyprOpenGL || !renderWindow)
        return false;
    g_pHyprOpenGL->makeEGLCurrent();
    const int width = std::max(1, static_cast<int>(std::lround(monitor->m_transformedSize.x)));
    const int height = std::max(1, static_cast<int>(std::lround(monitor->m_transformedSize.y)));
    if (!screen.scratch)
        screen.scratch = g_pHyprRenderer->createFB("hymission stage window scratch");
    if (!card.snapshot)
        card.snapshot = g_pHyprRenderer->createFB("hymission stage card");
    const int thumbWidth = std::max(1, static_cast<int>(std::lround(screen.geometry.cardWidth * monitor->m_scale)));
    const int thumbHeight = std::max(1, static_cast<int>(std::lround(screen.geometry.cardHeight * monitor->m_scale)));
    if (!screen.scratch || !card.snapshot || !screen.scratch->alloc(width, height) || !card.snapshot->alloc(thumbWidth, thumbHeight))
        return false;
    for (const auto& fb : {screen.scratch, card.snapshot}) {
        fb->setImageDescription(monitor->workBufferImageDescription());
        fb->getTexture()->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        fb->getTexture()->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // Restore every temporary render override before returning to the event
    // loop. In particular, never switch the monitor's real active workspace.
    const auto oldOffset = workspace->m_renderOffset->value();
    const auto oldOffsetGoal = workspace->m_renderOffset->goal();
    const auto oldAlpha = workspace->m_alpha->value();
    const auto oldAlphaGoal = workspace->m_alpha->goal();
    const bool oldVisible = workspace->m_visible;
    const bool oldForce = workspace->m_forceRendering;
    const bool oldSnapshot = g_pHyprRenderer->m_bRenderingSnapshot;
    const bool oldFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool oldShader = g_pHyprRenderer->m_renderData.blockScreenShader;
    rendering = true;
    workspace->m_renderOffset->setValueAndWarp(Vector2D{});
    workspace->m_alpha->setValueAndWarp(1.F);
    workspace->m_visible = true;
    workspace->m_forceRendering = true;
    g_pHyprRenderer->m_bRenderingSnapshot = true;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    auto restore = std::make_unique<Hyprutils::Utils::CScopeGuard>([&] {
        workspace->m_visible = oldVisible;
        workspace->m_forceRendering = oldForce;
        workspace->m_renderOffset->setValueAndWarp(oldOffset);
        if (oldOffset != oldOffsetGoal)
            *workspace->m_renderOffset = oldOffsetGoal;
        workspace->m_alpha->setValueAndWarp(oldAlpha);
        if (oldAlpha != oldAlphaGoal)
            *workspace->m_alpha = oldAlphaGoal;
        g_pHyprRenderer->m_bRenderingSnapshot = oldSnapshot;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = oldFeedback;
        g_pHyprRenderer->m_renderData.blockScreenShader = oldShader;
        rendering = false;
    });

    CRegion damage{0, 0, width, height};
    if (!g_pHyprRenderer->beginFullFakeRender(monitor, damage, screen.scratch)) {
        return false;
    }
    g_pHyprRenderer->setViewport(0, 0, width, height);
    g_pHyprRenderer->m_renderData.fbSize = screen.scratch->m_size;
    g_pHyprRenderer->m_renderData.transformDamage = false;
    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    g_pHyprRenderer->draw(CClearPassElement::SClearData{.color = CHyprColor(0, 0, 0, 0)}, damage);

    std::vector<PHLWINDOW> windows;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->m_isMapped || window->isHidden() || window->onSpecialWorkspace())
            continue;
        if (window->m_workspace == workspace || (window->m_pinned && window->m_monitor == monitor))
            windows.push_back(window);
    }
    const auto rank = [&](const PHLWINDOW& w) {
        if (w->m_pinned)
            return 5;
        if (Fullscreen::controller()->isFullscreen(w))
            return 3;
        if (w->m_isFloating)
            return w->shouldRenderOverFullscreen() ? 4 : 2;
        return workspace->m_lastFocusedWindow == w ? 1 : 0;
    };
    std::stable_sort(windows.begin(), windows.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    const auto now = Time::steadyNow();
    for (const auto& window : windows)
        renderWindow(g_pHyprRenderer.get(), window, monitor, now, true, Render::RENDER_PASS_ALL, false, false);
    g_pHyprRenderer->endRender();
    restore.reset();

    // Crop the desktop (excluding the bar and our band) while reducing it into
    // the card. Both are axis-aligned logical-orientation export framebuffers.
    auto* source = dynamic_cast<Render::GL::CGLFramebuffer*>(screen.scratch.get());
    auto* target = dynamic_cast<Render::GL::CGLFramebuffer*>(card.snapshot.get());
    if (!source || !target)
        return false;
    const CBox crop = CBox{screen.base.x + screen.geometry.reservation - monitor->m_position.x,
                          screen.base.y - monitor->m_position.y, screen.geometry.desktopWidth, screen.base.h}.scale(monitor->m_scale);
    GLint readFB = 0, drawFB = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source->getFBID());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->getFBID());
    glBlitFramebuffer(static_cast<int>(std::lround(crop.x)), height - static_cast<int>(std::lround(crop.y + crop.h)),
                      static_cast<int>(std::lround(crop.x + crop.w)), height - static_cast<int>(std::lround(crop.y)),
                      0, 0, thumbWidth, thumbHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFB);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    if (glGetError() != GL_NO_ERROR)
        return false;

    const auto label = workspace->m_name.empty() ? std::to_string(workspace->m_id) : workspace->m_name;
    if (!card.label || card.labelText != label) {
        card.labelText = label;
        card.label = g_pHyprRenderer->renderText(label, CHyprColor(0.95, 0.96, 0.98, 1.0), std::max(1, static_cast<int>(12 * monitor->m_scale)), false, "sans",
                                                std::max(1, thumbWidth - static_cast<int>(12 * monitor->m_scale)));
    }
    card.snapshotReady = true;
    return true;
}

StageController::Impl::~Impl() {
    shuttingDown = true;
    enabled = false;
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
    if (g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();
    screens.clear();
    instance = nullptr;
}

std::string StageController::Impl::stateJson() const {
    nlohmann::json result{{"enabled", enabled}, {"hooks_ready", hooksReady}, {"error", error}, {"screens", nlohmann::json::array()}};
    for (const auto& screen : screens) {
        const auto monitor = screen.monitor.lock();
        if (!monitor)
            continue;
        nlohmann::json cards = nlohmann::json::array();
        for (const auto& card : screen.cards) {
            if (const auto workspace = card.workspace.lock())
                cards.push_back({{"workspace", workspace->m_id}, {"name", workspace->m_name}, {"snapshot", card.snapshotReady}});
        }
        result["screens"].push_back({{"monitor", monitor->m_name}, {"card_width", screen.geometry.cardWidth}, {"card_height", screen.geometry.cardHeight},
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

} // namespace hymission
