#include "overview_controller.hpp"

#include <algorithm>
#include <any>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "overview_logic.hpp"

namespace hymission {

namespace {

constexpr double OPEN_DURATION_MS = 180.0;
constexpr double CLOSE_DURATION_MS = 140.0;
constexpr double BACKDROP_ALPHA = 0.42;
constexpr double OUTLINE_THICKNESS = 4.0;
constexpr double HOVER_THICKNESS = 2.0;
constexpr double TITLE_PADDING = 12.0;

OverviewController* g_controller = nullptr;

long getConfigInt(HANDLE handle, const char* name, long fallback) {
    if (const auto* value = HyprlandAPI::getConfigValue(handle, name)) {
        try {
            return static_cast<long>(std::any_cast<Hyprlang::INT>(value->getValue()));
        } catch (const std::bad_any_cast&) {
        }
    }

    return fallback;
}

double getConfigFloat(HANDLE handle, const char* name, double fallback) {
    if (const auto* value = HyprlandAPI::getConfigValue(handle, name)) {
        try {
            return static_cast<double>(std::any_cast<Hyprlang::FLOAT>(value->getValue()));
        } catch (const std::bad_any_cast&) {
        }
    }

    return fallback;
}

double clampUnit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

Rect makeRect(double x, double y, double width, double height) {
    return {
        x,
        y,
        std::max(1.0, width),
        std::max(1.0, height),
    };
}

CBox toBox(const Rect& rect) {
    return {
        rect.x,
        rect.y,
        rect.width,
        rect.height,
    };
}

Vector2D renderedWindowPosition(const PHLWINDOW& window) {
    if (!window)
        return {};

    Vector2D position = window->m_realPosition->value();
    if (window->m_workspace && !window->m_pinned)
        position += window->m_workspace->m_renderOffset->value();

    position += window->m_floatingOffset;
    return position;
}

void hkRenderWindow(void* rendererThisptr, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& now, bool decorate, eRenderPassMode passMode, bool ignorePosition,
                    bool standalone) {
    if (g_controller) {
        g_controller->renderWindowHook(rendererThisptr, std::move(window), std::move(monitor), now, decorate, passMode, ignorePosition, standalone);
    }
}

void hkRenderWorkspaceWindows(void* rendererThisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (g_controller) {
        g_controller->renderWorkspaceWindowsHook(rendererThisptr, std::move(monitor), std::move(workspace), now);
    }
}

void hkRenderWorkspaceWindowsFullscreen(void* rendererThisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (g_controller) {
        g_controller->renderWorkspaceWindowsFullscreenHook(rendererThisptr, std::move(monitor), std::move(workspace), now);
    }
}

} // namespace

OverviewController::OverviewController(HANDLE handle) : m_handle(handle) {
    g_controller = this;
}

OverviewController::~OverviewController() {
    deactivateHooks();

    if (m_renderWindowHook) {
        HyprlandAPI::removeFunctionHook(m_handle, m_renderWindowHook);
        m_renderWindowHook = nullptr;
    }

    if (m_renderWorkspaceWindowsHook) {
        HyprlandAPI::removeFunctionHook(m_handle, m_renderWorkspaceWindowsHook);
        m_renderWorkspaceWindowsHook = nullptr;
    }

    if (m_renderWorkspaceWindowsFullscreenHook) {
        HyprlandAPI::removeFunctionHook(m_handle, m_renderWorkspaceWindowsFullscreenHook);
        m_renderWorkspaceWindowsFullscreenHook = nullptr;
    }

    g_controller = nullptr;
}

bool OverviewController::initialize() {
    if (!installHooks())
        return false;

    auto& events = Event::bus()->m_events;

    m_renderStageListener = events.render.stage.listen([this](eRenderStage stage) { renderStage(stage); });
    m_mouseMoveListener = events.input.mouse.move.listen([this](const Vector2D&, Event::SCallbackInfo&) {
        handleMouseMove();
    });
    m_mouseButtonListener = events.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) { handleMouseButton(event, info); });
    m_keyboardListener = events.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) { handleKeyboard(event, info); });
    m_tickListener = events.tick.listen([this]() { handleTick(); });
    m_windowOpenListener = events.window.open.listen([this](PHLWINDOW window) { handleWindowSetChange(window); });
    m_windowDestroyListener = events.window.destroy.listen([this](PHLWINDOW window) { handleWindowSetChange(window); });
    m_windowCloseListener = events.window.close.listen([this](PHLWINDOW window) { handleWindowSetChange(window); });
    m_windowMoveWorkspaceListener = events.window.moveToWorkspace.listen([this](PHLWINDOW window, PHLWORKSPACE) { handleWindowSetChange(window); });
    m_workspaceActiveListener = events.workspace.active.listen([this](PHLWORKSPACE) { handleWorkspaceChange(); });
    m_monitorRemovedListener = events.monitor.removed.listen([this](PHLMONITOR monitor) { handleMonitorChange(monitor); });
    m_monitorFocusedListener = events.monitor.focused.listen([this](PHLMONITOR monitor) {
        if (isVisible() && monitor && m_state.ownerMonitor && monitor != m_state.ownerMonitor)
            beginClose();
    });

    return true;
}

SDispatchResult OverviewController::open() {
    const auto monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor) {
        return {.success = false, .error = "no monitor under cursor"};
    }

    if (m_state.phase == Phase::Opening || m_state.phase == Phase::Active)
        return {};

    beginOpen(monitor);
    return {};
}

SDispatchResult OverviewController::close() {
    if (m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing)
        return {};

    beginClose();
    return {};
}

SDispatchResult OverviewController::toggle() {
    if (m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing)
        return open();

    return close();
}

SDispatchResult OverviewController::debugCurrentLayout() const {
    const auto monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor) {
        return {.success = false, .error = "no monitor under cursor"};
    }

    const State preview = buildState(monitor);
    if (preview.windows.empty()) {
        notify(collectionSummary(monitor), CHyprColor(1.0, 0.7, 0.2, 1.0), 5000);
        return {};
    }

    std::ostringstream summary;
    summary << "[hymission] " << preview.windows.size() << " previews";

    const auto limit = std::min<std::size_t>(preview.windows.size(), 3);
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& rect = preview.windows[index].slot.target;
        summary << " | #" << index << ' ' << static_cast<int>(rect.x) << ',' << static_cast<int>(rect.y) << ' ' << static_cast<int>(rect.width) << 'x'
                << static_cast<int>(rect.height);
    }

    notify(summary.str(), CHyprColor(0.3, 0.9, 1.0, 1.0), 4000);
    return {};
}

void OverviewController::renderStage(eRenderStage stage) {
    if (!isVisible())
        return;

    const auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!monitor || !ownsMonitor(monitor))
        return;

    updateAnimation();

    if (stage == RENDER_POST_WALLPAPER) {
        renderBackdrop();
    } else if (stage == RENDER_POST_WINDOWS) {
        renderSelectionChrome();
    }
}

void OverviewController::handleMouseMove() {
    if (!shouldHandleInput())
        return;

    updateHoveredFromPointer();
}

void OverviewController::handleMouseButton(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
    if (!shouldHandleInput())
        return;

    info.cancelled = true;

    if (event.state != WL_POINTER_BUTTON_STATE_PRESSED || m_state.phase == Phase::Closing)
        return;

    if (event.button != BTN_LEFT)
        return;

    updateHoveredFromPointer();

    if (m_state.hoveredIndex) {
        m_state.selectedIndex = m_state.hoveredIndex;
        activateSelection();
        return;
    }

    beginClose();
}

void OverviewController::handleKeyboard(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
    if (!shouldHandleInput())
        return;

    info.cancelled = true;

    if (m_state.phase == Phase::Closing || event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;

    SP<IKeyboard> keyboard;
    for (const auto& candidate : g_pInputManager->m_keyboards) {
        if (candidate && candidate->m_xkbState) {
            keyboard = candidate;
            break;
        }
    }

    if (!keyboard || !keyboard->m_xkbState)
        return;

    const xkb_keysym_t keysym = xkb_state_key_get_one_sym(keyboard->m_xkbState, event.keycode + 8);
    switch (keysym) {
        case XKB_KEY_Escape:
            beginClose();
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            activateSelection();
            break;
        case XKB_KEY_Left:
            moveSelection(Direction::Left);
            break;
        case XKB_KEY_Right:
            moveSelection(Direction::Right);
            break;
        case XKB_KEY_Up:
            moveSelection(Direction::Up);
            break;
        case XKB_KEY_Down:
            moveSelection(Direction::Down);
            break;
        default:
            break;
    }
}

void OverviewController::handleTick() {
    if (!isVisible())
        return;

    updateAnimation();
    updateFocusPolicy();

    if (m_state.ownerMonitor) {
        g_pCompositor->scheduleFrameForMonitor(m_state.ownerMonitor);
    }
}

void OverviewController::handleWindowSetChange(PHLWINDOW window) {
    if (!isVisible() || !window)
        return;

    if (shouldAutoCloseFor(window))
        beginClose();
}

void OverviewController::handleWorkspaceChange() {
    if (!isVisible())
        return;

    if (!m_state.ownerMonitor || m_state.ownerMonitor->m_activeWorkspace != m_state.ownerWorkspace)
        beginClose();
}

void OverviewController::handleMonitorChange(PHLMONITOR monitor) {
    if (!isVisible() || !monitor || !m_state.ownerMonitor)
        return;

    if (monitor == m_state.ownerMonitor)
        beginClose();
}

void OverviewController::renderWindowHook(void* rendererThisptr, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& now, bool decorate, eRenderPassMode passMode,
                                          bool ignorePosition, bool standalone) {
    if (!m_renderWindowOriginal) {
        return;
    }

    if (!window || !monitor || !isVisible() || !ownsMonitor(monitor) || !hasManagedWindow(window)) {
        m_renderWindowOriginal(rendererThisptr, std::move(window), std::move(monitor), now, decorate, passMode, ignorePosition, standalone);
        return;
    }

    if (passMode == RENDER_PASS_POPUP)
        return;

    const auto it = std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
    if (it == m_state.windows.end()) {
        m_renderWindowOriginal(rendererThisptr, std::move(window), std::move(monitor), now, decorate, passMode, ignorePosition, standalone);
        return;
    }

    const Rect current = currentPreviewRect(*it);
    const Vector2D renderedPos = renderedWindowPosition(window);
    const Rect actual = makeRect(renderedPos.x, renderedPos.y, window->m_realSize->value().x, window->m_realSize->value().y);
    const double scale = std::clamp(current.width / std::max(1.0, actual.width), 0.05, 10.0);
    const Vector2D translation = {
        current.x - actual.x * scale,
        current.y - actual.y * scale,
    };

    auto& renderModif = g_pHyprOpenGL->m_renderData.renderModif;
    const auto oldModifs = renderModif.modifs;
    const bool oldEnabled = renderModif.enabled;

    renderModif.enabled = true;
    renderModif.modifs = oldModifs;
    renderModif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_SCALE, scale);
    renderModif.modifs.emplace_back(SRenderModifData::RMOD_TYPE_TRANSLATE, translation);

    m_renderWindowOriginal(rendererThisptr, std::move(window), std::move(monitor), now, decorate, passMode, ignorePosition, standalone);

    renderModif.modifs = oldModifs;
    renderModif.enabled = oldEnabled;
}

void OverviewController::renderWorkspaceWindowsHook(void* rendererThisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (!m_renderWorkspaceWindowsOriginal)
        return;

    if (!monitor || !workspace || !isVisible() || !ownsMonitor(monitor) || !ownsWorkspace(workspace)) {
        m_renderWorkspaceWindowsOriginal(rendererThisptr, std::move(monitor), std::move(workspace), now);
        return;
    }

    renderManagedWorkspace(rendererThisptr, monitor, workspace, now);
}

void OverviewController::renderWorkspaceWindowsFullscreenHook(void* rendererThisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (!m_renderWorkspaceWindowsFullscreenOriginal)
        return;

    if (!monitor || !workspace || !isVisible() || !ownsMonitor(monitor) || !ownsWorkspace(workspace)) {
        m_renderWorkspaceWindowsFullscreenOriginal(rendererThisptr, std::move(monitor), std::move(workspace), now);
        return;
    }

    renderManagedWorkspace(rendererThisptr, monitor, workspace, now);
}

LayoutConfig OverviewController::loadLayoutConfig() const {
    return {
        .outerPadding = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding", 48)),
        .rowSpacing = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:row_spacing", 32)),
        .columnSpacing = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:column_spacing", 32)),
        .smallWindowBoost = getConfigFloat(m_handle, "plugin:hymission:small_window_boost", 1.35),
        .maxPreviewScale = getConfigFloat(m_handle, "plugin:hymission:max_preview_scale", 0.95),
        .minWindowLength = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:min_window_length", 120)),
        .layoutSpaceWeight = getConfigFloat(m_handle, "plugin:hymission:layout_space_weight", 0.10),
        .layoutScaleWeight = getConfigFloat(m_handle, "plugin:hymission:layout_scale_weight", 1.0),
        .minSlotScale = getConfigFloat(m_handle, "plugin:hymission:min_slot_scale", 0.10),
    };
}

bool OverviewController::focusFollowsMouseEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:overview_focus_follows_mouse", 0) != 0;
}

bool OverviewController::isScrollingWorkspace(const PHLWORKSPACE& workspace) const {
    if (!workspace || !workspace->m_space)
        return false;

    const auto algorithm = workspace->m_space->algorithm();
    if (!algorithm || !algorithm->tiledAlgo())
        return false;

    return Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(&typeid(*algorithm->tiledAlgo())) == "scrolling";
}

void OverviewController::setScrollingFollowFocusOverride(bool disable) {
    if (!isScrollingWorkspace(m_state.ownerWorkspace))
        return;

    const auto* value = HyprlandAPI::getConfigValue(m_handle, "scrolling:follow_focus");
    if (!value)
        return;

    const auto* data = reinterpret_cast<Hyprlang::INT* const*>(value->getDataStaticPtr());
    if (!data || !*data)
        return;

    if (disable) {
        if (m_scrollingFollowFocusOverridden)
            return;

        m_scrollingFollowFocusBackup = static_cast<long>(**data);
        const auto err = g_pConfigManager->parseKeyword("scrolling:follow_focus", "0");
        if (!err.empty()) {
            notify("[hymission] failed to disable scrolling:follow_focus", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
            return;
        }

        m_scrollingFollowFocusOverridden = true;
        return;
    }

    if (!m_scrollingFollowFocusOverridden)
        return;

    const auto err = g_pConfigManager->parseKeyword("scrolling:follow_focus", std::to_string(m_scrollingFollowFocusBackup));
    if (!err.empty()) {
        notify("[hymission] failed to restore scrolling:follow_focus", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return;
    }

    m_scrollingFollowFocusOverridden = false;
}

bool OverviewController::installHooks() {
    if (!hookFunction("renderWindow", "CHyprRenderer::renderWindow(", m_renderWindowHook, reinterpret_cast<void*>(&hkRenderWindow))) {
        notify("[hymission] failed to hook renderWindow", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    m_renderWindowOriginal = reinterpret_cast<RenderWindowFn>(m_renderWindowHook->m_original);

    if (!hookFunction("renderWorkspaceWindows", "CHyprRenderer::renderWorkspaceWindows(", m_renderWorkspaceWindowsHook, reinterpret_cast<void*>(&hkRenderWorkspaceWindows))) {
        notify("[hymission] failed to hook workspace renderer", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    m_renderWorkspaceWindowsOriginal = reinterpret_cast<RenderWorkspaceWindowsFn>(m_renderWorkspaceWindowsHook->m_original);

    if (!hookFunction("renderWorkspaceWindowsFullscreen", "CHyprRenderer::renderWorkspaceWindowsFullscreen(", m_renderWorkspaceWindowsFullscreenHook,
                      reinterpret_cast<void*>(&hkRenderWorkspaceWindowsFullscreen))) {
        notify("[hymission] failed to hook fullscreen workspace renderer", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    m_renderWorkspaceWindowsFullscreenOriginal = reinterpret_cast<RenderWorkspaceWindowsFn>(m_renderWorkspaceWindowsFullscreenHook->m_original);
    return true;
}

bool OverviewController::activateHooks() {
    if (m_hooksActive)
        return true;

    if (!m_renderWindowHook || !m_renderWorkspaceWindowsHook || !m_renderWorkspaceWindowsFullscreenHook)
        return false;

    if (!m_renderWindowHook->hook()) {
        notify("[hymission] renderWindow hook attach failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!m_renderWorkspaceWindowsHook->hook()) {
        m_renderWindowHook->unhook();
        notify("[hymission] workspace hook attach failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!m_renderWorkspaceWindowsFullscreenHook->hook()) {
        m_renderWorkspaceWindowsHook->unhook();
        m_renderWindowHook->unhook();
        notify("[hymission] fullscreen workspace hook attach failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    m_hooksActive = true;
    return true;
}

void OverviewController::deactivateHooks() {
    if (!m_hooksActive)
        return;

    if (m_renderWorkspaceWindowsFullscreenHook)
        m_renderWorkspaceWindowsFullscreenHook->unhook();

    if (m_renderWorkspaceWindowsHook)
        m_renderWorkspaceWindowsHook->unhook();

    if (m_renderWindowHook)
        m_renderWindowHook->unhook();

    m_hooksActive = false;
}

bool OverviewController::hookFunction(const std::string& symbolName, const std::string& demangledNeedle, CFunctionHook*& hook, void* destination) {
    void* source = findFunction(symbolName, demangledNeedle);
    if (!source)
        return false;

    hook = HyprlandAPI::createFunctionHook(m_handle, source, destination);
    return hook != nullptr;
}

void* OverviewController::findFunction(const std::string& symbolName, const std::string& demangledNeedle) const {
    const auto matches = HyprlandAPI::findFunctionsByName(m_handle, symbolName);
    const auto it = std::find_if(matches.begin(), matches.end(), [&](const SFunctionMatch& match) {
        return match.demangled.find(demangledNeedle) != std::string::npos;
    });

    if (it != matches.end())
        return it->address;

    return matches.empty() ? nullptr : matches.front().address;
}

bool OverviewController::isAnimating() const {
    return m_state.phase == Phase::Opening || m_state.phase == Phase::Closing;
}

bool OverviewController::isVisible() const {
    return m_state.phase != Phase::Inactive;
}

bool OverviewController::shouldHandleInput() const {
    return isVisible();
}

bool OverviewController::ownsMonitor(const PHLMONITOR& monitor) const {
    return m_state.ownerMonitor && monitor && monitor == m_state.ownerMonitor;
}

bool OverviewController::ownsWorkspace(const PHLWORKSPACE& workspace) const {
    return m_state.ownerWorkspace && workspace && workspace == m_state.ownerWorkspace;
}

bool OverviewController::hasManagedWindow(const PHLWINDOW& window) const {
    return std::any_of(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
}

bool OverviewController::shouldAutoCloseFor(const PHLWINDOW& window) const {
    if (!window || !m_state.ownerMonitor || !m_state.ownerWorkspace)
        return false;

    if (hasManagedWindow(window))
        return true;

    return shouldManageWindow(window, m_state.ownerMonitor);
}

bool OverviewController::shouldManageWindow(const PHLWINDOW& window, const PHLMONITOR& monitor) const {
    if (!window || !monitor || !monitor->m_activeWorkspace)
        return false;

    if (!window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
        return false;

    if (!window->m_isMapped || window->isHidden() || window->m_fadingOut)
        return false;

    if (window->m_workspace != monitor->m_activeWorkspace)
        return false;

    const auto size = window->m_realSize->value();
    return size.x >= 1.0 && size.y >= 1.0;
}

std::string OverviewController::collectionSummary(const PHLMONITOR& monitor) const {
    std::size_t total = 0;
    std::size_t accepted = 0;
    std::size_t noWorkspace = 0;
    std::size_t specialWorkspace = 0;
    std::size_t unmapped = 0;
    std::size_t hidden = 0;
    std::size_t fading = 0;
    std::size_t workspaceMismatch = 0;
    std::size_t invalidSize = 0;

    for (const auto& window : g_pCompositor->m_windows) {
        if (!window)
            continue;

        ++total;

        if (!window->m_workspace) {
            ++noWorkspace;
            continue;
        }

        if (window->m_workspace->m_isSpecialWorkspace) {
            ++specialWorkspace;
            continue;
        }

        if (!window->m_isMapped) {
            ++unmapped;
            continue;
        }

        if (window->isHidden()) {
            ++hidden;
            continue;
        }

        if (window->m_fadingOut) {
            ++fading;
            continue;
        }

        if (!monitor || !monitor->m_activeWorkspace || window->m_workspace != monitor->m_activeWorkspace) {
            ++workspaceMismatch;
            continue;
        }

        const auto size = window->m_realSize->value();
        if (size.x < 1.0 || size.y < 1.0) {
            ++invalidSize;
            continue;
        }

        ++accepted;
    }

    std::ostringstream summary;
    summary << "[hymission] collect mon=" << (monitor ? monitor->m_name : "?") << " ws="
            << ((monitor && monitor->m_activeWorkspace) ? monitor->m_activeWorkspace->m_name : "?") << " total=" << total << " ok=" << accepted
            << " mismatch=" << workspaceMismatch << " hidden=" << hidden << " unmapped=" << unmapped << " special=" << specialWorkspace;

    if (fading || invalidSize || noWorkspace)
        summary << " fade=" << fading << " size=" << invalidSize << " nows=" << noWorkspace;

    return summary.str();
}

std::vector<Rect> OverviewController::targetRects() const {
    std::vector<Rect> rects;
    rects.reserve(m_state.windows.size());

    for (const auto& window : m_state.windows)
        rects.push_back(window.targetGlobal);

    return rects;
}

std::optional<std::size_t> OverviewController::hitTestTarget(double x, double y) const {
    return hitTest(targetRects(), x, y);
}

Rect OverviewController::currentPreviewRect(const ManagedWindow& window) const {
    return lerpRect(window.naturalGlobal, window.targetGlobal, visualProgress());
}

double OverviewController::visualProgress() const {
    switch (m_state.phase) {
        case Phase::Opening:
            return easeOutCubic(m_state.animationProgress);
        case Phase::Closing:
            return 1.0 - easeInCubic(m_state.animationProgress);
        case Phase::Active:
            return 1.0;
        case Phase::Inactive:
            return 0.0;
    }

    return 0.0;
}

void OverviewController::beginOpen(const PHLMONITOR& monitor) {
    State next = buildState(monitor);
    if (next.windows.empty()) {
        notify(collectionSummary(monitor), CHyprColor(1.0, 0.7, 0.2, 1.0), 5000);
        return;
    }

    if (!activateHooks())
        return;

    next.phase = Phase::Opening;
    next.animationProgress = 0.0;
    next.animationStart = std::chrono::steady_clock::now();
    m_state = std::move(next);
    setScrollingFollowFocusOverride(true);

    refreshScene(m_state.ownerMonitor, m_state.windows);
    g_pCompositor->scheduleFrameForMonitor(m_state.ownerMonitor);
}

void OverviewController::beginClose() {
    if (!isVisible())
        return;

    deactivate();
}

void OverviewController::deactivate() {
    const auto monitor = m_state.ownerMonitor;
    const auto windows = m_state.windows;
    setScrollingFollowFocusOverride(false);
    deactivateHooks();
    m_state = {};
    refreshScene(monitor, windows);
}

void OverviewController::refreshScene(const PHLMONITOR& monitor, const std::vector<ManagedWindow>& windows) const {
    if (!monitor)
        return;

    for (const auto& managed : windows) {
        if (managed.window)
            g_pHyprRenderer->damageWindow(managed.window);
    }

    g_pHyprRenderer->damageMonitor(monitor);
    g_layoutManager->recalculateMonitor(monitor);
    g_pCompositor->scheduleFrameForMonitor(monitor);
}

void OverviewController::updateAnimation() {
    if (!isAnimating())
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double, std::milli>(now - m_state.animationStart).count();
    const double duration = m_state.phase == Phase::Opening ? OPEN_DURATION_MS : CLOSE_DURATION_MS;

    m_state.animationProgress = clampUnit(elapsed / duration);
    if (m_state.animationProgress < 1.0)
        return;

    if (m_state.phase == Phase::Opening) {
        m_state.phase = Phase::Active;
        m_state.animationProgress = 1.0;
    } else if (m_state.phase == Phase::Closing) {
        deactivate();
    }
}

void OverviewController::updateFocusPolicy() {
    if (!isVisible() || m_state.phase == Phase::Closing)
        return;

    const auto desiredFocus = focusFollowsMouseEnabled() ? m_state.focusDuringOverview : m_state.focusBeforeOpen;
    if (desiredFocus && Desktop::focusState()->window() != desiredFocus) {
        Desktop::focusState()->rawWindowFocus(desiredFocus, Desktop::FOCUS_REASON_OTHER);
    }
}

void OverviewController::updateHoveredFromPointer() {
    if (!isVisible())
        return;

    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    m_state.hoveredIndex = hitTestTarget(pointer.x, pointer.y);
    if (m_state.hoveredIndex) {
        m_state.selectedIndex = m_state.hoveredIndex;
        if (focusFollowsMouseEnabled()) {
            m_state.focusDuringOverview = m_state.windows[*m_state.hoveredIndex].window;
            updateFocusPolicy();
        }
    }
}

void OverviewController::moveSelection(Direction direction) {
    if (m_state.windows.empty())
        return;

    if (!m_state.selectedIndex) {
        m_state.selectedIndex = 0;
        return;
    }

    if (const auto next = chooseDirectionalNeighbor(targetRects(), *m_state.selectedIndex, direction)) {
        m_state.selectedIndex = *next;
    }
}

void OverviewController::activateSelection() {
    if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size())
        return;

    Desktop::focusState()->fullWindowFocus(m_state.windows[*m_state.selectedIndex].window, Desktop::FOCUS_REASON_CLICK);
    beginClose();
}

void OverviewController::notify(const std::string& message, const CHyprColor& color, float durationMs) const {
    HyprlandAPI::addNotification(m_handle, message, color, durationMs);
}

void OverviewController::renderBackdrop() const {
    if (!m_state.ownerMonitor)
        return;

    const double alpha = BACKDROP_ALPHA * visualProgress();
    if (alpha <= 0.0)
        return;

    g_pHyprOpenGL->renderRect(
        CBox{
            m_state.ownerMonitor->m_position.x,
            m_state.ownerMonitor->m_position.y,
            m_state.ownerMonitor->m_transformedSize.x,
            m_state.ownerMonitor->m_transformedSize.y,
        },
        CHyprColor(0.05, 0.06, 0.08, alpha),
        {});
}

void OverviewController::renderSelectionChrome() const {
    const double progress = visualProgress();
    if (progress <= 0.0)
        return;

    if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size()) {
        renderOutline(currentPreviewRect(m_state.windows[*m_state.hoveredIndex]), CHyprColor(0.95, 0.97, 1.0, 0.55 * progress), HOVER_THICKNESS);
    }

    if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size()) {
        const auto& window = m_state.windows[*m_state.selectedIndex];
        const Rect  rect = currentPreviewRect(window);
        renderOutline(rect, CHyprColor(0.24, 0.78, 1.0, 0.95 * progress), OUTLINE_THICKNESS);

        auto texture = g_pHyprOpenGL->renderText(window.title, CHyprColor(1.0, 1.0, 1.0, std::min(1.0, progress)), 16, false, "", static_cast<int>(rect.width));
        if (!texture)
            return;

        const Rect titleRect = makeRect(rect.x, std::max(m_state.ownerMonitor->m_position.y + 8.0, rect.y - texture->m_size.y - TITLE_PADDING), texture->m_size.x, texture->m_size.y);
        g_pHyprOpenGL->renderTexture(texture, toBox(titleRect), {});
    }
}

void OverviewController::renderOutline(const Rect& rect, const CHyprColor& color, double thickness) const {
    const Rect top = makeRect(rect.x - thickness, rect.y - thickness, rect.width + thickness * 2.0, thickness);
    const Rect bottom = makeRect(rect.x - thickness, rect.y + rect.height, rect.width + thickness * 2.0, thickness);
    const Rect left = makeRect(rect.x - thickness, rect.y, thickness, rect.height);
    const Rect right = makeRect(rect.x + rect.width, rect.y, thickness, rect.height);

    g_pHyprOpenGL->renderRect(toBox(top), color, {});
    g_pHyprOpenGL->renderRect(toBox(bottom), color, {});
    g_pHyprOpenGL->renderRect(toBox(left), color, {});
    g_pHyprOpenGL->renderRect(toBox(right), color, {});
}

void OverviewController::renderManagedWorkspace(void* rendererThisptr, const PHLMONITOR& monitor, const PHLWORKSPACE& workspace, const Time::steady_tp& now) {
    if (!monitor || !workspace)
        return;

    const auto focused = Desktop::focusState()->window();

    for (const auto& managed : m_state.windows) {
        if (!managed.window || managed.isFloating || managed.isPinned || managed.window == focused)
            continue;

        renderWindowHook(rendererThisptr, managed.window, monitor, now, true, RENDER_PASS_MAIN, false, false);
    }

    for (const auto& managed : m_state.windows) {
        if (!managed.window || managed.isFloating || managed.isPinned || managed.window != focused)
            continue;

        renderWindowHook(rendererThisptr, managed.window, monitor, now, true, RENDER_PASS_MAIN, false, false);
    }

    for (const auto& managed : m_state.windows) {
        if (!managed.window || !managed.isFloating || managed.isPinned)
            continue;

        renderWindowHook(rendererThisptr, managed.window, monitor, now, true, RENDER_PASS_ALL, false, false);
    }
}

OverviewController::State OverviewController::buildState(const PHLMONITOR& monitor) const {
    State state;
    if (!monitor || !monitor->m_activeWorkspace)
        return state;

    state.ownerMonitor = monitor;
    state.ownerWorkspace = monitor->m_activeWorkspace;
    state.focusBeforeOpen = Desktop::focusState()->window();
    state.focusDuringOverview = state.focusBeforeOpen;

    std::vector<WindowInput> inputs;
    inputs.reserve(g_pCompositor->m_windows.size());

    for (const auto& window : g_pCompositor->m_windows) {
        if (!shouldManageWindow(window, monitor))
            continue;

        inputs.push_back({
            .index = state.windows.size(),
            .natural =
                {
                    renderedWindowPosition(window).x - monitor->m_position.x,
                    renderedWindowPosition(window).y - monitor->m_position.y,
                    std::max(1.0, window->m_realSize->value().x),
                    std::max(1.0, window->m_realSize->value().y),
                },
            .label = window->m_title,
        });

        const auto renderedPos = renderedWindowPosition(window);
        state.windows.push_back({
            .window = window,
            .title = window->m_title,
            .naturalGlobal = makeRect(renderedPos.x, renderedPos.y, window->m_realSize->value().x, window->m_realSize->value().y),
            .isFloating = window->m_isFloating,
            .isPinned = window->m_pinned,
        });
    }

    if (state.windows.empty())
        return state;

    MissionControlLayout engine;
    state.slots = engine.compute(inputs,
                                 {
                                     0.0,
                                     0.0,
                                     static_cast<double>(monitor->m_transformedSize.x),
                                     static_cast<double>(monitor->m_transformedSize.y),
                                 },
                                 loadLayoutConfig());

    for (std::size_t index = 0; index < state.windows.size() && index < state.slots.size(); ++index) {
        state.windows[index].slot = state.slots[index];
        state.windows[index].targetGlobal =
            makeRect(monitor->m_position.x + state.slots[index].target.x, monitor->m_position.y + state.slots[index].target.y, state.slots[index].target.width,
                     state.slots[index].target.height);
    }

    const auto focused = Desktop::focusState()->window();
    for (std::size_t index = 0; index < state.windows.size(); ++index) {
        if (state.windows[index].window == focused) {
            state.selectedIndex = index;
            break;
        }
    }

    if (!state.selectedIndex && !state.windows.empty())
        state.selectedIndex = 0;

    return state;
}

} // namespace hymission
