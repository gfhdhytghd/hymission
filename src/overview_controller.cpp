#include "overview_controller.hpp"

#include <algorithm>
#include <any>
#include <cmath>
#include <cctype>
#include <fstream>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#define private public
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#undef private

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/history/WorkspaceHistoryTracker.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/ITrackpadGesture.hpp>
#include <hyprland/src/managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Region.hpp>

#include "overview_logic.hpp"

namespace hymission {

namespace {

constexpr double OPEN_DURATION_MS = 180.0;
constexpr double CLOSE_DURATION_MS = 140.0;
constexpr double RELAYOUT_DURATION_MS = 140.0;
constexpr double WORKSPACE_TRANSITION_DURATION_MS = 180.0;
constexpr double CLOSE_SETTLE_TIMEOUT_MS = 80.0;
constexpr auto   NATIVE_ANIMATION_DISABLE_DURATION = std::chrono::milliseconds(320);
constexpr double CLOSE_SETTLE_EPSILON = 0.75;
constexpr std::size_t CLOSE_SETTLE_STABLE_FRAMES = 2;
constexpr double BACKDROP_ALPHA = 0.42;
constexpr double OUTLINE_THICKNESS = 4.0;
constexpr double HOVER_THICKNESS = 2.0;
constexpr double TITLE_PADDING = 12.0;
constexpr auto   MISSION_CONTROL_WORKSPACE_NAME = "Mission Control";
OverviewController* g_controller = nullptr;

enum class GestureDispatcherKind : uint8_t {
    Toggle,
    Open,
};

struct SurfacePassElementMirror {
    void*                             vtable = nullptr;
    CSurfacePassElement::SRenderData  data;
};

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

std::string getConfigString(HANDLE handle, const char* name, std::string fallback) {
    if (const auto* value = HyprlandAPI::getConfigValue(handle, name)) {
        try {
            return std::string(std::any_cast<Hyprlang::STRING>(value->getValue()));
        } catch (const std::bad_any_cast&) {
        }
    }

    return fallback;
}

bool shouldWrapWorkspaceIds(const WORKSPACEID targetId, const WORKSPACEID currentId) {
    static auto PWORKSPACEWRAPAROUND = CConfigValue<Hyprlang::INT>("animations:workspace_wraparound");

    if (!*PWORKSPACEWRAPAROUND)
        return false;

    WORKSPACEID lowestID = INT64_MAX;
    WORKSPACEID highestID = INT64_MIN;

    for (const auto& workspace : g_pCompositor->getWorkspaces()) {
        if (!workspace || workspace->m_id < 0 || workspace->m_isSpecialWorkspace)
            continue;

        lowestID = std::min(lowestID, workspace->m_id);
        highestID = std::max(highestID, workspace->m_id);
    }

    return std::min(targetId, currentId) == lowestID && std::max(targetId, currentId) == highestID;
}

float animationMovePercent(std::string style) {
    std::ranges::transform(style, style.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::istringstream stream(style);
    std::string        token;
    float              movePercent = 100.F;
    while (stream >> token) {
        if (!token.empty() && token.back() == '%') {
            try {
                movePercent = std::stof(token.substr(0, token.size() - 1));
            } catch (...) {
            }
        }
    }

    return movePercent;
}

Vector2D predictedWorkspaceAnimationOffset(HANDLE handle, const PHLMONITOR& monitor, const PHLWORKSPACE& workspace, bool left, bool incoming) {
    if (!monitor || !workspace)
        return {};

    std::string style = workspace->m_renderOffset->getStyle();
    std::ranges::transform(style, style.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    bool vert = style.starts_with("slidevert") || style.starts_with("slidefadevert");
    if (style.find(" top") != std::string::npos) {
        left = false;
        vert = true;
    } else if (style.find(" bottom") != std::string::npos) {
        left = true;
        vert = true;
    } else if (style.find(" left") != std::string::npos) {
        left = false;
        vert = false;
    } else if (style.find(" right") != std::string::npos) {
        left = true;
        vert = false;
    }

    const float movePercent = animationMovePercent(style) / 100.F;

    if (style == "fade")
        return {};

    if (style.starts_with("slidefade")) {
        const double primaryDistance = incoming ? (left ? 1.0 : -1.0) : (left ? -1.0 : 1.0);
        if (vert)
            return Vector2D{0.0, primaryDistance * monitor->m_size.y * movePercent};

        return Vector2D{primaryDistance * monitor->m_size.x * movePercent, 0.0};
    }

    if (vert) {
        const double distance = (static_cast<double>(monitor->m_size.y) + static_cast<double>(getConfigInt(handle, "general:gaps_workspaces", 0))) * movePercent;
        return Vector2D{0.0, incoming ? (left ? distance : -distance) : (left ? -distance : distance)};
    }

    const double distance = (static_cast<double>(monitor->m_size.x) + static_cast<double>(getConfigInt(handle, "general:gaps_workspaces", 0))) * movePercent;
    return Vector2D{incoming ? (left ? distance : -distance) : (left ? -distance : distance), 0.0};
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

Rect translateRect(const Rect& rect, double dx, double dy) {
    return makeRect(rect.x + dx, rect.y + dy, rect.width, rect.height);
}

CBox toBox(const Rect& rect) {
    return {
        rect.x,
        rect.y,
        rect.width,
        rect.height,
    };
}

Vector2D renderedWindowPosition(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    // Hyprland's realPosition is already expressed in global compositor coordinates.
    // Adding workspace render offsets or floating offsets here double-counts them and
    // pushes overview open/close geometry toward off-screen workspace animation space.
    return goal ? window->m_realPosition->goal() : window->m_realPosition->value();
}

Rect surfaceRenderGlobalRectForWindow(const PHLWINDOW& window) {
    if (!window)
        return {};

    Vector2D position = window->m_realPosition->value();
    if (window->m_workspace && !window->m_pinned)
        position += window->m_workspace->m_renderOffset->value();

    return makeRect(position.x, position.y, window->m_realSize->value().x, window->m_realSize->value().y);
}

Layout::Tiled::CScrollingAlgorithm* scrollingAlgorithmForWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space)
        return nullptr;

    const auto algorithm = workspace->m_space->algorithm();
    if (!algorithm || !algorithm->tiledAlgo())
        return nullptr;

    return dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(algorithm->tiledAlgo().get());
}

std::string vectorToString(const Vector2D& value) {
    std::ostringstream out;
    out << value.x << ',' << value.y;
    return out.str();
}

std::string boxToString(const CBox& box) {
    std::ostringstream out;
    out << box.x << ',' << box.y << ' ' << box.width << 'x' << box.height;
    return out.str();
}

std::string rectToString(const Rect& rect) {
    std::ostringstream out;
    out << rect.x << ',' << rect.y << ' ' << rect.width << 'x' << rect.height;
    return out.str();
}

std::string trimCopy(std::string value) {
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !isSpace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !isSpace(ch); }).base(), value.end());
    return value;
}

std::vector<std::string> splitCommaTokens(const std::string& value) {
    std::vector<std::string> tokens;
    std::string              current;
    std::istringstream       stream(value);
    while (std::getline(stream, current, ','))
        tokens.push_back(trimCopy(current));
    return tokens;
}

std::string joinTokens(const std::vector<std::string>& tokens, std::size_t beginIndex) {
    std::ostringstream out;
    for (std::size_t i = beginIndex; i < tokens.size(); ++i) {
        if (i != beginIndex)
            out << ',';
        out << tokens[i];
    }
    return out.str();
}

double normalizedGestureDelta(const IPointer::SSwipeUpdateEvent& event, eTrackpadGestureDirection direction, float deltaScale, bool invertVertical) {
    const double baseDelta = direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? -static_cast<double>(event.delta.x) : -static_cast<double>(event.delta.y);
    const double scaled = baseDelta * static_cast<double>(deltaScale);
    return invertVertical ? -scaled : scaled;
}

class CHymissionTrackpadGesture final : public ITrackpadGesture {
  public:
    CHymissionTrackpadGesture(GestureDispatcherKind dispatcher, OverviewController::ScopeOverride scope, eTrackpadGestureDirection direction, float deltaScale)
        : m_dispatcher(dispatcher), m_scope(scope), m_direction(direction), m_deltaScale(deltaScale) {
    }

    void begin(const STrackpadGestureBegin& e) override {
        m_tracking = false;
        if (!g_controller || !e.swipe ||
            !g_controller->beginTrackpadGesture(m_dispatcher == GestureDispatcherKind::Open, m_scope, m_direction, *e.swipe, m_deltaScale))
            return;

        m_tracking = true;
    }

    void update(const STrackpadGestureUpdate& e) override {
        if (!m_tracking || !g_controller || !e.swipe)
            return;

        g_controller->updateTrackpadGesture(*e.swipe);
    }

    void end(const STrackpadGestureEnd& e) override {
        if (!m_tracking || !g_controller)
            return;

        m_tracking = false;
        g_controller->endTrackpadGesture(e.swipe ? e.swipe->cancelled : true);
    }

  private:
    GestureDispatcherKind             m_dispatcher;
    OverviewController::ScopeOverride m_scope = OverviewController::ScopeOverride::Default;
    eTrackpadGestureDirection         m_direction = TRACKPAD_GESTURE_DIR_VERTICAL;
    float                             m_deltaScale = 1.0F;
    bool                              m_tracking = false;
};

class CHymissionWorkspaceTrackpadGesture final : public ITrackpadGesture {
  public:
    CHymissionWorkspaceTrackpadGesture(eTrackpadGestureDirection direction, float)
        : m_direction(direction) {
    }

    void begin(const STrackpadGestureBegin& e) override {
        m_mode = Mode::Native;

        if (!g_controller || !e.swipe) {
            m_nativeGesture.begin(e);
            return;
        }

        if (g_controller->blocksWorkspaceSwitchInOverviewForGestures()) {
            m_mode = Mode::Blocked;
            return;
        }

        if (g_controller->allowsWorkspaceSwitchInOverviewForGestures()) {
            const auto configuredDirection = e.direction != TRACKPAD_GESTURE_DIR_NONE ? e.direction : m_direction;
            if (!g_controller->beginOverviewWorkspaceSwipeGesture(configuredDirection))
                return;

            ITrackpadGesture::begin(e);
            m_mode = Mode::Overview;
            g_controller->updateOverviewWorkspaceSwipeGesture(distance(e));
            return;
        }

        m_nativeGesture.begin(e);
    }

    void update(const STrackpadGestureUpdate& e) override {
        if (m_mode == Mode::Blocked || !e.swipe)
            return;

        if (m_mode == Mode::Overview) {
            if (g_controller)
                g_controller->updateOverviewWorkspaceSwipeGesture(distance(e));
            return;
        }

        m_nativeGesture.update(e);
    }

    void end(const STrackpadGestureEnd& e) override {
        if (m_mode == Mode::Blocked)
            return;

        if (m_mode == Mode::Overview) {
            if (g_controller)
                g_controller->endOverviewWorkspaceSwipeGesture(e.swipe ? e.swipe->cancelled : true);
            return;
        }

        m_nativeGesture.end(e);
    }

    bool isDirectionSensitive() override {
        return true;
    }

  private:
    enum class Mode {
        Native,
        Overview,
        Blocked,
    };

    CWorkspaceSwipeGesture   m_nativeGesture;
    eTrackpadGestureDirection m_direction = TRACKPAD_GESTURE_DIR_HORIZONTAL;
    Mode                      m_mode = Mode::Native;
};

template <typename T>
bool containsHandle(const std::vector<T>& values, const T& value) {
    return std::ranges::find(values, value) != values.end();
}

bool rectApproxEqual(const Rect& lhs, const Rect& rhs, double epsilon) {
    return std::abs(lhs.x - rhs.x) <= epsilon && std::abs(lhs.y - rhs.y) <= epsilon && std::abs(lhs.width - rhs.width) <= epsilon &&
        std::abs(lhs.height - rhs.height) <= epsilon;
}

std::optional<Vector2D> visiblePointForRectOnMonitor(const Rect& windowRect, const PHLMONITOR& monitor) {
    if (!monitor)
        return std::nullopt;

    const Rect monitorRect = makeRect(monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y);
    const double left = std::max(windowRect.x, monitorRect.x);
    const double top = std::max(windowRect.y, monitorRect.y);
    const double right = std::min(windowRect.x + windowRect.width, monitorRect.x + monitorRect.width);
    const double bottom = std::min(windowRect.y + windowRect.height, monitorRect.y + monitorRect.height);
    if (right <= left || bottom <= top)
        return std::nullopt;

    return Vector2D((left + right) * 0.5, (top + bottom) * 0.5);
}

std::optional<Vector2D> expectedSurfaceSizeForUV(const PHLWINDOW& window, const SP<CWLSurfaceResource>& surface, const PHLMONITOR& monitor, bool main) {
    if (!surface || !monitor)
        return std::nullopt;

    const bool canUseWindow = window && main;
    const bool windowSizeMisalign = canUseWindow && window->getReportedSize() != window->wlSurface()->resource()->m_current.size;

    if (surface->m_current.viewport.hasDestination)
        return (surface->m_current.viewport.destination * monitor->m_scale).round();

    if (surface->m_current.viewport.hasSource)
        return (surface->m_current.viewport.source.size() * monitor->m_scale).round();

    if (windowSizeMisalign)
        return (surface->m_current.size * monitor->m_scale).round();

    if (canUseWindow)
        return (window->getReportedSize() * monitor->m_scale).round();

    return std::nullopt;
}

void focusWindowCompat(const PHLWINDOW& window, bool raw = false, Desktop::eFocusReason reason = Desktop::FOCUS_REASON_OTHER) {
    if (!window)
        return;

    if (raw) {
        Desktop::focusState()->rawWindowFocus(window, reason);
        return;
    }

    Desktop::focusState()->fullWindowFocus(window, reason);
}

CSurfacePassElement::SRenderData* surfaceRenderDataMutable(void* surfacePassThisptr) {
    if (!surfacePassThisptr)
        return nullptr;

    return &reinterpret_cast<SurfacePassElementMirror*>(surfacePassThisptr)->data;
}

CBox hkSurfaceTexBox(void* surfacePassThisptr) {
    if (!g_controller)
        return {};

    return g_controller->surfaceTexBoxHook(surfacePassThisptr);
}

std::optional<CBox> hkSurfaceBoundingBox(void* surfacePassThisptr) {
    if (!g_controller)
        return {};

    return g_controller->surfaceBoundingBoxHook(surfacePassThisptr);
}

CRegion hkSurfaceOpaqueRegion(void* surfacePassThisptr) {
    if (!g_controller)
        return {};

    return g_controller->surfaceOpaqueRegionHook(surfacePassThisptr);
}

CRegion hkSurfaceVisibleRegion(void* surfacePassThisptr, bool& cancel) {
    if (!g_controller)
        return {};

    return g_controller->surfaceVisibleRegionHook(surfacePassThisptr, cancel);
}

void hkBorderDraw(void* borderDecorationThisptr, PHLMONITOR monitor, const float& alpha) {
    if (!g_controller)
        return;

    g_controller->borderDrawHook(borderDecorationThisptr, monitor, alpha);
}

void hkShadowDraw(void* shadowDecorationThisptr, PHLMONITOR monitor, const float& alpha) {
    if (!g_controller)
        return;

    g_controller->shadowDrawHook(shadowDecorationThisptr, monitor, alpha);
}

void hkCalculateUVForSurface(void* rendererThisptr, PHLWINDOW window, SP<CWLSurfaceResource> surface, PHLMONITOR monitor, bool main, const Vector2D& projSize,
                             const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1) {
    if (!g_controller)
        return;

    (void)rendererThisptr;
    g_controller->calculateUVForSurfaceHook(window, std::move(surface), monitor, main, projSize, projSizeUnscaled, fixMisalignedFSV1);
}

void hkSurfaceDraw(void* surfacePassThisptr, const CRegion& damage) {
    if (!g_controller)
        return;

    g_controller->surfaceDrawHook(surfacePassThisptr, damage);
}

bool hkSurfaceNeedsLiveBlur(void* surfacePassThisptr) {
    if (!g_controller)
        return false;

    return g_controller->surfaceNeedsLiveBlurHook(surfacePassThisptr);
}

bool hkSurfaceNeedsPrecomputeBlur(void* surfacePassThisptr) {
    if (!g_controller)
        return false;

    return g_controller->surfaceNeedsPrecomputeBlurHook(surfacePassThisptr);
}

bool hkShouldRenderWindow(void*, PHLWINDOW window, PHLMONITOR monitor) {
    if (!g_controller)
        return false;

    return g_controller->shouldRenderWindowHook(window, monitor);
}

SDispatchResult hkFullscreenActive(std::string args) {
    if (!g_controller)
        return {};

    return g_controller->fullscreenDispatcherHook(std::move(args));
}

SDispatchResult hkFullscreenStateActive(std::string args) {
    if (!g_controller)
        return {};

    return g_controller->fullscreenStateDispatcherHook(std::move(args));
}

SDispatchResult hkChangeWorkspace(std::string args) {
    if (!g_controller)
        return {};

    return g_controller->changeWorkspaceDispatcherHook(std::move(args));
}

SDispatchResult hkFocusWorkspaceOnCurrentMonitor(std::string args) {
    if (!g_controller)
        return {};

    return g_controller->focusWorkspaceOnCurrentMonitorDispatcherHook(std::move(args));
}

void hkWorkspaceSwipeBegin(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e) {
    if (!g_controller)
        return;

    g_controller->workspaceSwipeBeginHook(gestureThisptr, e);
}

void hkWorkspaceSwipeUpdate(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (!g_controller)
        return;

    g_controller->workspaceSwipeUpdateHook(gestureThisptr, e);
}

void hkWorkspaceSwipeEnd(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (!g_controller)
        return;

    g_controller->workspaceSwipeEndHook(gestureThisptr, e);
}

std::optional<std::string> hkHandleGesture(void*, const std::string& keyword, const std::string& value) {
    if (!g_controller)
        return {};

    return g_controller->handleGestureConfigHook(keyword, value);
}

} // namespace

OverviewController::OverviewController(HANDLE handle) : m_handle(handle) {
    g_controller = this;
}

OverviewController::~OverviewController() {
    clearRegisteredTrackpadGestures();
    clearPostCloseForcedFocus();
    clearPostCloseDispatcher();
    restoreWorkspaceNameOverrides();
    g_pHyprRenderer->m_directScanoutBlocked = false;
    setFullscreenRenderOverride(false);
    setInputFollowMouseOverride(false);
    setScrollingFollowFocusOverride(false);
    setAnimationsEnabledOverride(false);
    deactivateHooks();
    if (m_changeWorkspaceHook)
        m_changeWorkspaceHook->unhook();
    if (m_focusWorkspaceOnCurrentMonitorHook)
        m_focusWorkspaceOnCurrentMonitorHook->unhook();
    if (m_workspaceSwipeBeginFunctionHook)
        m_workspaceSwipeBeginFunctionHook->unhook();
    if (m_workspaceSwipeUpdateFunctionHook)
        m_workspaceSwipeUpdateFunctionHook->unhook();
    if (m_workspaceSwipeEndFunctionHook)
        m_workspaceSwipeEndFunctionHook->unhook();

    if (m_surfaceTexBoxHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceTexBoxHook);
    if (m_surfaceBoundingBoxHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceBoundingBoxHook);
    if (m_surfaceOpaqueRegionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceOpaqueRegionHook);
    if (m_surfaceVisibleRegionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceVisibleRegionHook);
    if (m_surfaceDrawHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceDrawHook);
    if (m_surfaceNeedsLiveBlurHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceNeedsLiveBlurHook);
    if (m_surfaceNeedsPrecomputeBlurHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_surfaceNeedsPrecomputeBlurHook);
    if (m_shouldRenderWindowHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_shouldRenderWindowHook);
    if (m_borderDrawHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_borderDrawHook);
    if (m_shadowDrawHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_shadowDrawHook);
    if (m_calculateUVForSurfaceHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_calculateUVForSurfaceHook);
    if (m_fullscreenActiveHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_fullscreenActiveHook);
    if (m_fullscreenStateActiveHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_fullscreenStateActiveHook);
    if (m_changeWorkspaceHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_changeWorkspaceHook);
    if (m_focusWorkspaceOnCurrentMonitorHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_focusWorkspaceOnCurrentMonitorHook);
    if (m_workspaceSwipeBeginFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_workspaceSwipeBeginFunctionHook);
    if (m_workspaceSwipeUpdateFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_workspaceSwipeUpdateFunctionHook);
    if (m_workspaceSwipeEndFunctionHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_workspaceSwipeEndFunctionHook);
    if (m_handleGestureHook)
        HyprlandAPI::removeFunctionHook(m_handle, m_handleGestureHook);

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
    m_windowOpenListener = events.window.open.listen([this](PHLWINDOW window) { handleWindowSetChange(window); });
    m_windowDestroyListener = events.window.destroy.listen([this](PHLWINDOW window) { handleWindowSetChange(window); });
    m_windowCloseListener = events.window.close.listen([this](PHLWINDOW window) { handleWindowSetChange(window); });
    m_windowMoveWorkspaceListener = events.window.moveToWorkspace.listen([this](PHLWINDOW window, PHLWORKSPACE) { handleWindowSetChange(window); });
    m_workspaceActiveListener = events.workspace.active.listen([this](PHLWORKSPACE workspace) { handleWorkspaceChange(workspace); });
    m_monitorRemovedListener = events.monitor.removed.listen([this](PHLMONITOR monitor) { handleMonitorChange(monitor); });
    m_monitorFocusedListener = events.monitor.focused.listen([this](PHLMONITOR) {
        if (isVisible() && shouldHandleInput())
            updateHoveredFromPointer();
    });

    return true;
}

SDispatchResult OverviewController::open(const std::string& args) {
    std::string error;
    const auto requestedScope = parseScopeOverride(args, error);
    if (!requestedScope)
        return {.success = false, .error = error};

    const auto monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor) {
        return {.success = false, .error = "no monitor under cursor"};
    }

    if ((m_state.phase == Phase::Opening || m_state.phase == Phase::Active) && *requestedScope == m_state.collectionPolicy.requestedScope)
        return {};

    beginOpen(monitor, *requestedScope);
    return {};
}

bool OverviewController::allowsWorkspaceSwitchInOverviewForGestures() const {
    return allowsWorkspaceSwitchInOverview();
}

bool OverviewController::blocksWorkspaceSwitchInOverviewForGestures() const {
    return shouldBlockWorkspaceSwitchInOverview();
}

SDispatchResult OverviewController::close() {
    if (m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return {};

    if (m_workspaceTransition.active)
        clearOverviewWorkspaceTransition();

    beginClose();
    return {};
}

SDispatchResult OverviewController::toggle(const std::string& args) {
    if (m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return open(args);

    return close();
}

SDispatchResult OverviewController::debugCurrentLayout() const {
    const auto monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor) {
        return {.success = false, .error = "no monitor under cursor"};
    }

    const State preview = buildState(monitor, ScopeOverride::Default);
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

    if (!m_workspaceTransition.active && (m_state.phase == Phase::Opening || m_state.phase == Phase::Active) &&
        std::any_of(m_state.windows.begin(), m_state.windows.end(), [](const ManagedWindow& managed) { return managed.window && managed.window->m_fadingOut; })) {
        rebuildVisibleState();
    }

    setFullscreenRenderOverride(true);

    if (stage == RENDER_POST_WALLPAPER) {
        updateOverviewWorkspaceTransition();
        updateAnimation();
        renderBackdrop();
    } else if (stage == RENDER_POST_WINDOWS) {
        renderSelectionChrome();
        if ((isAnimating() || m_state.phase == Phase::ClosingSettle || m_state.relayoutActive || m_postOpenRefreshFrames > 0) && !m_deactivatePending) {
            damageOwnedMonitor();
            if (m_postOpenRefreshFrames > 0)
                --m_postOpenRefreshFrames;
        }
        if (m_deactivatePending) {
            if (debugLogsEnabled())
                debugLog("[hymission] post-windows deactivate");
            deactivate();
        }
    }
}

void OverviewController::handleMouseMove() {
    if (m_postCloseForcedFocusLatched && !isVisible()) {
        if (m_ignorePostCloseMouseMoveCount > 0) {
            --m_ignorePostCloseMouseMoveCount;
            return;
        }

        clearPostCloseForcedFocus();
        if (m_restoreInputFollowMouseAfterPostClose) {
            setInputFollowMouseOverride(false);
            m_restoreInputFollowMouseAfterPostClose = false;
        }
    }

    if (!shouldHandleInput())
        return;

    updateHoveredFromPointer();
}

void OverviewController::handleMouseButton(const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
    if (m_postCloseForcedFocusLatched && !isVisible()) {
        clearPostCloseForcedFocus();
        if (m_restoreInputFollowMouseAfterPostClose) {
            setInputFollowMouseOverride(false);
            m_restoreInputFollowMouseAfterPostClose = false;
        }
    }

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
    bool               handled = true;
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
            handled = false;
            break;
    }

    if (handled)
        info.cancelled = true;
}

void OverviewController::handleWindowSetChange(PHLWINDOW window) {
    if (window && m_postCloseForcedFocusLatched && m_postCloseForcedFocus.lock() == window)
        clearPostCloseForcedFocus();
    if (window && m_pendingLiveFocusWorkspaceChangeTarget.lock() == window)
        m_pendingLiveFocusWorkspaceChangeTarget.reset();

    if (!isVisible() || !window)
        return;

    if (m_workspaceTransition.active) {
        clearOverviewWorkspaceTransition();
        rebuildVisibleState();
        return;
    }

    if (!shouldAutoCloseFor(window))
        return;

    if (m_state.phase == Phase::Opening || m_state.phase == Phase::Active) {
        rebuildVisibleState();
        return;
    }

    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        beginClose(CloseMode::Abort);
}

void OverviewController::handleWorkspaceChange(PHLWORKSPACE workspace) {
    const bool liveFocusWorkspaceChange = matchesPendingLiveFocusWorkspaceChange(workspace);
    const auto action = resolveOverviewWorkspaceChangeAction(isVisible(), m_applyingWorkspaceTransitionCommit, m_workspaceTransition.active,
                                                             m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle,
                                                             liveFocusWorkspaceChange, allowsWorkspaceSwitchInOverview());

    if (action == OverviewWorkspaceChangeAction::Ignore)
        return;

    if (liveFocusWorkspaceChange) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] keep overview open for live focus workspace change";
            if (const auto target = m_pendingLiveFocusWorkspaceChangeTarget.lock())
                out << " target=" << debugWindowLabel(target);
            debugLog(out.str());
        }
        m_pendingLiveFocusWorkspaceChangeTarget.reset();
    }

    if (action == OverviewWorkspaceChangeAction::Rebuild) {
        if (m_workspaceTransition.active)
            clearOverviewWorkspaceTransition();
        rebuildVisibleState();
        return;
    }

    beginClose(CloseMode::Abort);
}

void OverviewController::handleMonitorChange(PHLMONITOR monitor) {
    if (!isVisible() || !monitor || !m_state.ownerMonitor)
        return;

    if (m_workspaceTransition.active) {
        clearOverviewWorkspaceTransition();
        rebuildVisibleState();
    }

    if (monitor == m_state.ownerMonitor) {
        beginClose(CloseMode::Abort);
        return;
    }

    if (ownsMonitor(monitor))
        rebuildVisibleState();
}

bool OverviewController::shouldRenderWindowHook(const PHLWINDOW& window, const PHLMONITOR& monitor) {
    if (!m_shouldRenderWindowOriginal)
        return false;

    if (isVisible() && window && monitor && ownsMonitor(monitor) && hasManagedWindow(window) && previewMonitorForWindow(window) == monitor) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] shouldRenderWindow override " << debugWindowLabel(window) << " monitor=" << monitor->m_name;
            debugLog(out.str());
        }
        return true;
    }

    return m_shouldRenderWindowOriginal(g_pHyprRenderer.get(), window, monitor);
}

void OverviewController::borderDrawHook(void* borderDecorationThisptr, const PHLMONITOR& monitor, const float& alpha) {
    if (!m_borderDrawOriginal) {
        return;
    }

    const auto window = g_pHyprOpenGL->m_renderData.currentWindow.lock();
    if (!window || !monitor || !isVisible() || !ownsMonitor(monitor) || !hasManagedWindow(window) || previewMonitorForWindow(window) != monitor) {
        m_borderDrawOriginal(borderDecorationThisptr, monitor, alpha);
        return;
    }
}

void OverviewController::shadowDrawHook(void* shadowDecorationThisptr, const PHLMONITOR& monitor, const float& alpha) {
    if (!m_shadowDrawOriginal) {
        return;
    }

    const auto window = g_pHyprOpenGL->m_renderData.currentWindow.lock();
    if (!window || !monitor || !isVisible() || !ownsMonitor(monitor) || !hasManagedWindow(window) || previewMonitorForWindow(window) != monitor) {
        m_shadowDrawOriginal(shadowDecorationThisptr, monitor, alpha);
        return;
    }
}

void OverviewController::calculateUVForSurfaceHook(const PHLWINDOW& window, SP<CWLSurfaceResource> surface, const PHLMONITOR& monitor, bool main, const Vector2D& projSize,
                                                   const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1) {
    if (!m_calculateUVForSurfaceOriginal)
        return;

    Vector2D adjustedProjSize = projSize;
    Vector2D adjustedProjSizeUnscaled = projSizeUnscaled;
    bool     adjusted = false;

    if (isVisible() && window && surface && monitor && ownsMonitor(monitor) && hasManagedWindow(window) && previewMonitorForWindow(window) == monitor && !window->m_isX11) {
        const auto expected = expectedSurfaceSizeForUV(window, surface, monitor, main);
        if (expected && (projSize.x + 1.0 < expected->x || projSize.y + 1.0 < expected->y)) {
            adjustedProjSize = *expected;
            if (monitor->m_scale > 0.0)
                adjustedProjSizeUnscaled = *expected / monitor->m_scale;
            adjusted = true;
        }

        if (debugSurfaceLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] uv " << debugWindowLabel(window) << " main=" << main << " proj=" << vectorToString(projSize)
                << " projUnscaled=" << vectorToString(projSizeUnscaled);
            if (expected)
                out << " expected=" << vectorToString(*expected);
            else
                out << " expected=<none>";
            if (adjusted)
                out << " adjustedProj=" << vectorToString(adjustedProjSize) << " adjustedProjUnscaled=" << vectorToString(adjustedProjSizeUnscaled);
            out << " fixMisaligned=" << fixMisalignedFSV1;
            debugSurfaceLog(out.str());
        }
    }

    m_calculateUVForSurfaceOriginal(g_pHyprRenderer.get(), window, std::move(surface), monitor, main, adjustedProjSize, adjustedProjSizeUnscaled, fixMisalignedFSV1);
}

SDispatchResult OverviewController::fullscreenDispatcherHook(std::string args) {
    return runHookedDispatcher(PostCloseDispatcher::Fullscreen, std::move(args));
}

SDispatchResult OverviewController::fullscreenStateDispatcherHook(std::string args) {
    return runHookedDispatcher(PostCloseDispatcher::FullscreenState, std::move(args));
}

SDispatchResult OverviewController::changeWorkspaceDispatcherHook(std::string args) {
    if (!m_changeWorkspaceOriginal)
        return {};

    if (shouldBlockWorkspaceSwitchInOverview()) {
        if (debugLogsEnabled())
            debugLog("[hymission] block changeworkspace during multi-workspace overview");
        return {};
    }

    if (allowsWorkspaceSwitchInOverview()) {
        const auto result = startOverviewWorkspaceTransitionForDispatcher(args, false);
        if (!result.success &&
            (result.error == "overview workspace transition does not support special workspaces" ||
             result.error == "overview workspace transition requires workspace on current monitor"))
            return m_changeWorkspaceOriginal(std::move(args));
        return result;
    }

    return m_changeWorkspaceOriginal(std::move(args));
}

SDispatchResult OverviewController::focusWorkspaceOnCurrentMonitorDispatcherHook(std::string args) {
    if (!m_focusWorkspaceOnCurrentMonitorOriginal)
        return {};

    if (shouldBlockWorkspaceSwitchInOverview()) {
        if (debugLogsEnabled())
            debugLog("[hymission] block focusWorkspaceOnCurrentMonitor during multi-workspace overview");
        return {};
    }

    if (allowsWorkspaceSwitchInOverview()) {
        const auto result = startOverviewWorkspaceTransitionForDispatcher(args, true);
        if (!result.success && result.error == "focusWorkspaceOnCurrentMonitor workspace is on another monitor")
            return m_focusWorkspaceOnCurrentMonitorOriginal(std::move(args));
        return result;
    }

    return m_focusWorkspaceOnCurrentMonitorOriginal(std::move(args));
}

void OverviewController::workspaceSwipeBeginHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureBegin& e) {
    if (!m_workspaceSwipeBeginOriginal)
        return;

    if (shouldBlockWorkspaceSwitchInOverview()) {
        if (debugLogsEnabled())
            debugLog("[hymission] block workspace swipe begin during multi-workspace overview");
        return;
    }

    m_workspaceSwipeBeginOriginal(gestureThisptr, e);
}

void OverviewController::workspaceSwipeUpdateHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (!m_workspaceSwipeUpdateOriginal)
        return;

    if (shouldBlockWorkspaceSwitchInOverview())
        return;

    m_workspaceSwipeUpdateOriginal(gestureThisptr, e);
}

void OverviewController::workspaceSwipeEndHook(void* gestureThisptr, const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (!m_workspaceSwipeEndOriginal)
        return;

    if (shouldBlockWorkspaceSwitchInOverview())
        return;

    m_workspaceSwipeEndOriginal(gestureThisptr, e);
}

void OverviewController::surfaceDrawHook(void* surfacePassThisptr, const CRegion& damage) {
    if (!m_surfaceDrawOriginal) {
        return;
    }

    if (m_surfaceRenderDataTransformDepth > 0) {
        m_surfaceDrawOriginal(surfacePassThisptr, damage);
        return;
    }

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "draw", renderData, monitor, snapshot)) {
        m_surfaceDrawOriginal(surfacePassThisptr, damage);
        return;
    }

    ++m_surfaceRenderDataTransformDepth;
    m_surfaceDrawOriginal(surfacePassThisptr, damage);
    --m_surfaceRenderDataTransformDepth;
    restoreSurfaceRenderData(renderData, snapshot);
}

bool OverviewController::surfaceNeedsLiveBlurHook(void* surfacePassThisptr) {
    if (!m_surfaceNeedsLiveBlurOriginal)
        return false;

    if (shouldSuppressSurfaceBlur(surfacePassThisptr))
        return false;

    auto* renderData = surfaceRenderDataMutable(surfacePassThisptr);
    auto  monitor = renderData ? renderData->pMonitor.lock() : PHLMONITOR{};
    if (!renderData || !renderData->pWindow || !monitor || !isVisible() || !ownsMonitor(monitor) || !hasManagedWindow(renderData->pWindow) ||
        previewMonitorForWindow(renderData->pWindow) != monitor)
        return m_surfaceNeedsLiveBlurOriginal(surfacePassThisptr);

    const float savedAlpha = renderData->alpha;
    renderData->alpha = managedPreviewAlphaFor(renderData->pWindow, savedAlpha);
    const bool needsBlur = m_surfaceNeedsLiveBlurOriginal(surfacePassThisptr);
    renderData->alpha = savedAlpha;
    return needsBlur;
}

bool OverviewController::surfaceNeedsPrecomputeBlurHook(void* surfacePassThisptr) {
    if (!m_surfaceNeedsPrecomputeBlurOriginal)
        return false;

    if (shouldSuppressSurfaceBlur(surfacePassThisptr))
        return false;

    auto* renderData = surfaceRenderDataMutable(surfacePassThisptr);
    auto  monitor = renderData ? renderData->pMonitor.lock() : PHLMONITOR{};
    if (!renderData || !renderData->pWindow || !monitor || !isVisible() || !ownsMonitor(monitor) || !hasManagedWindow(renderData->pWindow) ||
        previewMonitorForWindow(renderData->pWindow) != monitor)
        return m_surfaceNeedsPrecomputeBlurOriginal(surfacePassThisptr);

    const float savedAlpha = renderData->alpha;
    renderData->alpha = managedPreviewAlphaFor(renderData->pWindow, savedAlpha);
    const bool needsBlur = m_surfaceNeedsPrecomputeBlurOriginal(surfacePassThisptr);
    renderData->alpha = savedAlpha;
    return needsBlur;
}

CBox OverviewController::surfaceTexBoxHook(void* surfacePassThisptr) {
    if (!m_surfaceTexBoxOriginal)
        return {};

    if (m_surfaceRenderDataTransformDepth > 0)
        return m_surfaceTexBoxOriginal(surfacePassThisptr);

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "texbox", renderData, monitor, snapshot))
        return m_surfaceTexBoxOriginal(surfacePassThisptr);

    ++m_surfaceRenderDataTransformDepth;
    const CBox box = m_surfaceTexBoxOriginal(surfacePassThisptr);
    --m_surfaceRenderDataTransformDepth;
    restoreSurfaceRenderData(renderData, snapshot);
    return box;
}

std::optional<CBox> OverviewController::surfaceBoundingBoxHook(void* surfacePassThisptr) {
    if (!m_surfaceBoundingBoxOriginal)
        return {};

    if (m_surfaceRenderDataTransformDepth > 0)
        return m_surfaceBoundingBoxOriginal(surfacePassThisptr);

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "boundingBox", renderData, monitor, snapshot))
        return m_surfaceBoundingBoxOriginal(surfacePassThisptr);

    ++m_surfaceRenderDataTransformDepth;
    const auto box = m_surfaceBoundingBoxOriginal(surfacePassThisptr);
    --m_surfaceRenderDataTransformDepth;
    restoreSurfaceRenderData(renderData, snapshot);
    return box;
}

CRegion OverviewController::surfaceOpaqueRegionHook(void* surfacePassThisptr) {
    if (!m_surfaceOpaqueRegionOriginal)
        return {};

    if (m_surfaceRenderDataTransformDepth > 0)
        return m_surfaceOpaqueRegionOriginal(surfacePassThisptr);

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "opaqueRegion", renderData, monitor, snapshot))
        return m_surfaceOpaqueRegionOriginal(surfacePassThisptr);

    // Overview already damages the full monitor while animating, and the transformed preview
    // geometry is temporary. Returning an empty opaque region avoids pass simplification
    // incorrectly occluding lower previews and causing one-frame flashes.
    if (isVisible() && monitor && ownsMonitor(monitor) && hasManagedWindow(renderData->pWindow)) {
        restoreSurfaceRenderData(renderData, snapshot);
        return {};
    }

    ++m_surfaceRenderDataTransformDepth;
    const CRegion region = m_surfaceOpaqueRegionOriginal(surfacePassThisptr);
    --m_surfaceRenderDataTransformDepth;
    restoreSurfaceRenderData(renderData, snapshot);
    return region;
}

CRegion OverviewController::surfaceVisibleRegionHook(void* surfacePassThisptr, bool& cancel) {
    if (!m_surfaceVisibleRegionOriginal)
        return {};

    if (m_surfaceRenderDataTransformDepth > 0)
        return m_surfaceVisibleRegionOriginal(surfacePassThisptr, cancel);

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "visibleRegion", renderData, monitor, snapshot))
        return m_surfaceVisibleRegionOriginal(surfacePassThisptr, cancel);

    ++m_surfaceRenderDataTransformDepth;
    CBox fullBox = m_surfaceTexBoxOriginal(surfacePassThisptr);
    --m_surfaceRenderDataTransformDepth;
    fullBox.scale(monitor->m_scale);
    fullBox.round();
    cancel = fullBox.width <= 0.0 || fullBox.height <= 0.0;
    restoreSurfaceRenderData(renderData, snapshot);
    return cancel ? CRegion{} : CRegion(fullBox);
}

LayoutConfig OverviewController::loadLayoutConfig() const {
    const double outerPadding = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding", 48));
    return {
        .outerPaddingTop = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_top", static_cast<long>(outerPadding))),
        .outerPaddingRight = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_right", static_cast<long>(outerPadding))),
        .outerPaddingBottom = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_bottom", static_cast<long>(outerPadding))),
        .outerPaddingLeft = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_left", static_cast<long>(outerPadding))),
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

OverviewController::CollectionPolicy OverviewController::loadCollectionPolicy(ScopeOverride requestedScope) const {
    if (requestedScope == ScopeOverride::OnlyCurrentWorkspace) {
        return {
            .requestedScope = requestedScope,
            .onlyActiveWorkspace = true,
            .onlyActiveMonitor = true,
            .includeSpecial = false,
        };
    }

    if (requestedScope == ScopeOverride::ForceAll) {
        return {
            .requestedScope = requestedScope,
            .onlyActiveWorkspace = false,
            .onlyActiveMonitor = false,
            .includeSpecial = true,
        };
    }

    return {
        .requestedScope = requestedScope,
        .onlyActiveWorkspace = getConfigInt(m_handle, "plugin:hymission:only_active_workspace", 0) != 0,
        .onlyActiveMonitor = getConfigInt(m_handle, "plugin:hymission:only_active_monitor", 0) != 0,
        .includeSpecial = getConfigInt(m_handle, "plugin:hymission:show_special", 0) != 0,
    };
}

std::optional<OverviewController::ScopeOverride> OverviewController::parseScopeOverride(const std::string& args, std::string& error) const {
    const std::string trimmed = trimCopy(args);
    if (trimmed.empty())
        return ScopeOverride::Default;

    if (trimmed == "onlycurrentworkspace")
        return ScopeOverride::OnlyCurrentWorkspace;
    if (trimmed == "forceall")
        return ScopeOverride::ForceAll;

    error = "invalid overview scope: " + trimmed;
    return std::nullopt;
}

bool OverviewController::focusFollowsMouseEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:overview_focus_follows_mouse", 0) != 0;
}

bool OverviewController::gestureInvertVerticalEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:gesture_invert_vertical", 0) != 0;
}

bool OverviewController::workspaceSwipeInvertEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_invert", 0) != 0;
}

bool OverviewController::workspaceChangeKeepsOverviewEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:workspace_change_keeps_overview", 0) != 0;
}

bool OverviewController::debugLogsEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:debug_logs", 0) != 0;
}

bool OverviewController::debugSurfaceLogsEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:debug_surface_logs", 0) != 0;
}

bool workspaceRowsEnabled(HANDLE handle) {
    return getConfigInt(handle, "plugin:hymission:one_workspace_per_row", 0) != 0;
}

bool OverviewController::isScrollingWorkspace(const PHLWORKSPACE& workspace) const {
    if (!workspace || !workspace->m_space)
        return false;

    const auto algorithm = workspace->m_space->algorithm();
    if (!algorithm || !algorithm->tiledAlgo())
        return false;

    return Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(&typeid(*algorithm->tiledAlgo())) == "scrolling";
}

bool OverviewController::hasScrollingWorkspace() const {
    return std::ranges::any_of(m_state.managedWorkspaces, [this](const PHLWORKSPACE& workspace) { return isScrollingWorkspace(workspace); });
}

double OverviewController::gestureSwipeDistance() const {
    return std::max(1.0, static_cast<double>(getConfigInt(m_handle, "gestures:workspace_swipe_distance", 300)));
}

double OverviewController::gestureForceSpeedThreshold() const {
    return std::max(0.0, static_cast<double>(getConfigInt(m_handle, "gestures:workspace_swipe_min_speed_to_force", 30)));
}

bool OverviewController::gestureSwipeForeverEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_forever", 0) != 0;
}

bool OverviewController::gestureSwipeCreateNewEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_create_new", 0) != 0;
}

bool OverviewController::gestureSwipeUseRelativeEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_use_r", 0) != 0;
}

bool OverviewController::gestureSwipeDirectionLockEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_direction_lock", 0) != 0;
}

double OverviewController::gestureSwipeDirectionLockThreshold() const {
    return std::max(0.0, static_cast<double>(getConfigInt(m_handle, "gestures:workspace_swipe_direction_lock_threshold", 10)));
}

bool OverviewController::allowsWorkspaceSwitchInOverview() const {
    return isVisible() && m_state.collectionPolicy.onlyActiveWorkspace && workspaceChangeKeepsOverviewEnabled();
}

bool OverviewController::shouldBlockWorkspaceSwitchInOverview() const {
    return isVisible() && !m_state.collectionPolicy.onlyActiveWorkspace;
}

bool OverviewController::shouldOverrideWorkspaceNames(const State& state) const {
    return !state.collectionPolicy.onlyActiveWorkspace;
}

bool OverviewController::workspaceSwipeUsesVerticalAxis(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    const auto style = workspace->m_renderOffset->getStyle();
    return style == "slidevert" || style.starts_with("slidefadevert");
}

double OverviewController::workspaceSwipeViewportDistance(const PHLMONITOR& monitor, WorkspaceTransitionAxis axis) const {
    if (!monitor)
        return 1.0;

    const double gaps = static_cast<double>(getConfigInt(m_handle, "general:gaps_workspaces", 0));
    return (axis == WorkspaceTransitionAxis::Vertical ? static_cast<double>(monitor->m_size.y) : static_cast<double>(monitor->m_size.x)) + gaps;
}

int OverviewController::resolveOverviewWorkspaceSwipeStep(eTrackpadGestureDirection direction, double totalDelta, double lastDelta) const {
    if (direction != TRACKPAD_GESTURE_DIR_HORIZONTAL && direction != TRACKPAD_GESTURE_DIR_VERTICAL)
        return 0;

    double adjustedTotal = totalDelta;
    double adjustedLast = lastDelta;
    if (workspaceSwipeInvertEnabled()) {
        adjustedTotal = -adjustedTotal;
        adjustedLast = -adjustedLast;
    }

    const double cancelRatio = std::clamp(getConfigFloat(m_handle, "gestures:workspace_swipe_cancel_ratio", 0.5), 0.0, 1.0);
    const double distanceThreshold = gestureSwipeDistance() * cancelRatio;
    const double speedThreshold = gestureForceSpeedThreshold();
    const double decisive = std::abs(adjustedLast) >= speedThreshold ? adjustedLast : adjustedTotal;

    if (std::abs(decisive) < 0.0001 || (std::abs(adjustedTotal) < distanceThreshold && std::abs(adjustedLast) < speedThreshold))
        return 0;

    return decisive < 0.0 ? -1 : 1;
}

bool OverviewController::resolveOverviewWorkspaceTargetByStep(const PHLMONITOR& monitor, int step, WORKSPACEID& workspaceId, std::string& workspaceName,
                                                              PHLWORKSPACE& workspace, bool& syntheticEmpty) const {
    workspaceId = WORKSPACE_INVALID;
    workspaceName.clear();
    workspace.reset();
    syntheticEmpty = false;

    if (!monitor || step == 0 || !monitor->m_activeWorkspace || monitor->m_activeWorkspace->m_isSpecialWorkspace)
        return false;

    const bool        useRelative = gestureSwipeUseRelativeEnabled();
    const std::string selector = step < 0 ? (useRelative ? "r-1" : "m-1") : (useRelative ? "r+1" : "m+1");
    auto              resolved = getWorkspaceIDNameFromString(selector);

    if (resolved.id == WORKSPACE_INVALID)
        return false;

    workspaceId = resolved.id;
    workspaceName = resolved.name;
    workspace = g_pCompositor->getWorkspaceByID(workspaceId);

    if (step > 0 && gestureSwipeCreateNewEnabled() && (workspaceId <= monitor->m_activeWorkspace->m_id || !workspace)) {
        auto createTarget = getWorkspaceIDNameFromString("r+1");
        if (createTarget.id == WORKSPACE_INVALID)
            return false;

        workspaceId = createTarget.id;
        workspaceName = createTarget.name.empty() ? std::to_string(createTarget.id) : createTarget.name;
        workspace = g_pCompositor->getWorkspaceByID(workspaceId);
        syntheticEmpty = !workspace;
        return true;
    }

    if (workspaceId == monitor->m_activeWorkspace->m_id)
        return false;

    if (!workspace && !gestureSwipeCreateNewEnabled())
        return false;

    syntheticEmpty = !workspace;
    return true;
}

bool OverviewController::switchOverviewWorkspaceByStep(int step) {
    PHLMONITOR monitor = Desktop::focusState()->monitor();
    if (!monitor || !containsHandle(m_state.participatingMonitors, monitor))
        monitor = m_state.ownerMonitor;
    if (!monitor)
        return false;

    return startOverviewWorkspaceTransitionByStep(monitor, step, WorkspaceTransitionMode::TimedCommit);
}

void OverviewController::restoreWorkspaceNameOverrides() {
    for (auto it = m_workspaceNameBackups.rbegin(); it != m_workspaceNameBackups.rend(); ++it) {
        if (!it->workspace)
            continue;

        it->workspace->rename(it->name);
    }

    m_workspaceNameBackups.clear();
}

void OverviewController::applyWorkspaceNameOverrides(const State& state) {
    restoreWorkspaceNameOverrides();

    if (!shouldOverrideWorkspaceNames(state))
        return;

    for (const auto& monitor : state.participatingMonitors) {
        if (!monitor || !monitor->m_activeWorkspace)
            continue;

        if (containsHandle(state.managedWorkspaces, monitor->m_activeWorkspace)) {
            m_workspaceNameBackups.push_back({
                .workspace = monitor->m_activeWorkspace,
                .name = monitor->m_activeWorkspace->m_name,
            });
            monitor->m_activeWorkspace->rename(MISSION_CONTROL_WORKSPACE_NAME);
        }
    }
}

void OverviewController::clearRegisteredTrackpadGestures() {
    if (!g_pTrackpadGestures)
        return;

    for (const auto& gesture : m_registeredGestures)
        g_pTrackpadGestures->removeGesture(gesture.fingerCount, gesture.direction, gesture.modMask, gesture.deltaScale, gesture.disableInhibit);

    m_registeredGestures.clear();
}

void OverviewController::rememberRegisteredTrackpadGesture(const GestureRegistration& gesture) {
    std::erase_if(m_registeredGestures, [&](const GestureRegistration& existing) {
        return existing.fingerCount == gesture.fingerCount && existing.direction == gesture.direction && existing.modMask == gesture.modMask &&
            std::abs(existing.deltaScale - gesture.deltaScale) <= 0.0001F && existing.disableInhibit == gesture.disableInhibit;
    });
    m_registeredGestures.push_back(gesture);
}

std::optional<std::string> OverviewController::handleGestureConfigHook(const std::string& keyword, const std::string& value) {
    if (!m_handleGestureOriginal)
        return {};

    const std::string trimmedKeyword = trimCopy(keyword);
    if (!trimmedKeyword.starts_with("gesture"))
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);

    const std::string flags = trimmedKeyword.substr(std::string("gesture").size());
    if (flags.find_first_not_of("p") != std::string::npos)
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);

    const auto tokens = splitCommaTokens(value);
    if (tokens.size() < 3)
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);

    std::size_t fingerCount = 0;
    try {
        fingerCount = static_cast<std::size_t>(std::stoul(tokens[0]));
    } catch (const std::exception&) {
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);
    }

    const auto direction = g_pTrackpadGestures->dirForString(tokens[1]);
    if (direction != TRACKPAD_GESTURE_DIR_VERTICAL && direction != TRACKPAD_GESTURE_DIR_HORIZONTAL)
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);

    uint32_t    modMask = 0;
    float       deltaScale = 1.0F;
    std::size_t actionIndex = 2;
    for (; actionIndex < tokens.size(); ++actionIndex) {
        const auto& token = tokens[actionIndex];
        if (token.starts_with("mod:")) {
            const std::string modValue = trimCopy(token.substr(4));
            modMask = modValue.empty() ? 0 : g_pKeybindManager->stringToModMask(modValue);
            continue;
        }

        if (token.starts_with("scale:")) {
            try {
                deltaScale = std::stof(trimCopy(token.substr(6)));
            } catch (const std::exception&) {
                return std::string{"invalid gesture scale: "} + token;
            }
            continue;
        }

        break;
    }

    if (actionIndex >= tokens.size())
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);

    if (tokens[actionIndex] == "workspace" && actionIndex + 1 == tokens.size()) {
        const bool disableInhibit = flags.contains('p');
        g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
        const auto addResult =
            g_pTrackpadGestures->addGesture(makeUnique<CHymissionWorkspaceTrackpadGesture>(direction, deltaScale), fingerCount, direction, modMask, deltaScale, disableInhibit);
        if (!addResult.has_value())
            return addResult.error();

        rememberRegisteredTrackpadGesture({
            .fingerCount = fingerCount,
            .direction = direction,
            .modMask = modMask,
            .deltaScale = deltaScale,
            .disableInhibit = disableInhibit,
        });

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] register workspace gesture fingers=" << fingerCount << " dir="
                << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical") << " scale=" << deltaScale << " modMask=" << modMask
                << " disableInhibit=" << (disableInhibit ? 1 : 0);
            debugLog(out.str());
        }

        return {};
    }

    if (tokens[actionIndex] != "dispatcher")
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);

    if (actionIndex + 1 >= tokens.size())
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);

    const std::string dispatcher = tokens[actionIndex + 1];
    const std::string dispatcherArgs = joinTokens(tokens, actionIndex + 2);

    GestureDispatcherKind dispatcherKind;
    if (dispatcher == "hymission:toggle") {
        dispatcherKind = GestureDispatcherKind::Toggle;
    } else if (dispatcher == "hymission:open") {
        dispatcherKind = GestureDispatcherKind::Open;
    } else {
        return m_handleGestureOriginal(g_pConfigManager.get(), keyword, value);
    }

    std::string scopeError;
    const auto  requestedScope = parseScopeOverride(dispatcherArgs, scopeError);
    if (!requestedScope)
        return scopeError;

    const bool disableInhibit = flags.contains('p');
    g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
    const auto addResult =
        g_pTrackpadGestures->addGesture(makeUnique<CHymissionTrackpadGesture>(dispatcherKind, *requestedScope, direction, deltaScale), fingerCount, direction, modMask, deltaScale, disableInhibit);
    if (!addResult.has_value())
        return addResult.error();

    rememberRegisteredTrackpadGesture({
        .fingerCount = fingerCount,
        .direction = direction,
        .modMask = modMask,
        .deltaScale = deltaScale,
        .disableInhibit = disableInhibit,
    });

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] register gesture fingers=" << fingerCount << " dir=" << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical")
            << " dispatcher=" << dispatcher << " args=" << dispatcherArgs
            << " scale=" << deltaScale << " modMask=" << modMask << " disableInhibit=" << (disableInhibit ? 1 : 0);
        debugLog(out.str());
    }

    return {};
}

bool OverviewController::beginTrackpadGesture(bool openOnly, ScopeOverride requestedScope, eTrackpadGestureDirection direction, const IPointer::SSwipeUpdateEvent& event,
                                              float deltaScale) {
    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return false;

    if (isVisible() && allowsWorkspaceSwitchInOverview())
        return false;

    if (openOnly && isVisible())
        return false;

    const bool opening = !isVisible() || openOnly || m_state.phase == Phase::Opening;
    const double initialDelta = normalizedGestureDelta(event, direction, deltaScale, gestureInvertVerticalEnabled());
    if ((opening && initialDelta <= 0.0) || (!opening && initialDelta >= 0.0)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] gesture ignored mode=" << (opening ? "open" : "close") << " delta=" << initialDelta
                << " dir=" << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical");
            debugLog(out.str());
        }
        return false;
    }

    if (opening) {
        if (!isVisible()) {
            const auto monitor = g_pCompositor->getMonitorFromCursor();
            if (!monitor)
                return false;

            m_suppressInitialHoverUpdate = true;
            beginOpen(monitor, requestedScope);
            m_suppressInitialHoverUpdate = false;
            if (!isVisible())
                return false;
        }
    } else {
        if (m_state.phase == Phase::Active && m_state.relayoutActive) {
            for (auto& managed : m_state.windows) {
                managed.targetGlobal = currentPreviewRect(managed);
                managed.relayoutFromGlobal = managed.targetGlobal;
            }
            m_state.relayoutActive = false;
            m_state.relayoutProgress = 1.0;
            m_state.relayoutStart = {};
        }

        prepareGestureCloseExitGeometry();
    }

    m_gestureSession = {
        .active = true,
        .opening = opening,
        .requestedScope = requestedScope,
        .direction = direction,
        .openness = visualProgress(),
        .lastSignedSpeed = 0.0,
        .deltaScale = deltaScale,
    };

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture begin mode=" << (opening ? "open" : "close") << " openness=" << m_gestureSession.openness << " scale=" << deltaScale
            << " dir=" << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical");
        debugLog(out.str());
    }

    updateTrackpadGesture(event);
    refreshOwnedMonitors();
    return true;
}

void OverviewController::updateTrackpadGesture(const IPointer::SSwipeUpdateEvent& event) {
    if (!m_gestureSession.active)
        return;

    const double delta = normalizedGestureDelta(event, m_gestureSession.direction, m_gestureSession.deltaScale, gestureInvertVerticalEnabled());

    m_gestureSession.lastSignedSpeed = delta;
    m_gestureSession.openness = clampUnit(m_gestureSession.openness + delta / gestureSwipeDistance());

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture update openness=" << m_gestureSession.openness << " delta=" << delta;
        debugLog(out.str());
    }

    refreshOwnedMonitors();
}

void OverviewController::endTrackpadGesture(bool cancelled) {
    if (!m_gestureSession.active)
        return;

    const GestureSession gesture = m_gestureSession;
    const double         speedThreshold = gestureForceSpeedThreshold();
    const bool           speedCommit = speedThreshold > 0.0 &&
        (gesture.opening ? (gesture.lastSignedSpeed >= speedThreshold) : (gesture.lastSignedSpeed <= -speedThreshold));
    const bool commit = !cancelled && (speedCommit || (gesture.opening ? gesture.openness >= 0.5 : gesture.openness <= 0.5));

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture end mode=" << (gesture.opening ? "open" : "close") << " cancelled=" << (cancelled ? 1 : 0) << " commit=" << (commit ? 1 : 0)
            << " openness=" << gesture.openness << " lastSpeed=" << gesture.lastSignedSpeed;
        debugLog(out.str());
    }

    if (!gesture.opening && commit) {
        beginClose(CloseMode::Normal, std::nullopt, true);
        m_gestureSession = {};
        return;
    }

    m_gestureSession = {};

    m_deactivatePending = false;
    if (gesture.opening) {
        if (commit) {
            m_state.phase = Phase::Opening;
            m_state.animationProgress = 0.0;
            m_state.animationFromVisual = gesture.openness;
            m_state.animationToVisual = 1.0;
            m_state.animationStart = {};
        } else {
            clearPostCloseDispatcher();
            m_state.pendingExitFocus = m_state.focusBeforeOpen;
            m_state.closeMode = m_state.focusBeforeOpen ? CloseMode::Normal : CloseMode::Abort;
            if (m_state.focusBeforeOpen && m_state.focusBeforeOpen->m_isMapped)
                commitOverviewExitFocus(m_state.focusBeforeOpen);
            m_state.phase = Phase::Closing;
            m_state.animationProgress = 0.0;
            m_state.animationFromVisual = gesture.openness;
            m_state.animationToVisual = 0.0;
            m_state.animationStart = {};
        }
    } else {
        m_state.phase = Phase::Opening;
        m_state.animationProgress = 0.0;
        m_state.animationFromVisual = gesture.openness;
        m_state.animationToVisual = 1.0;
        m_state.animationStart = {};
    }

    refreshOwnedMonitors();
}

bool OverviewController::beginOverviewWorkspaceSwipeGesture(eTrackpadGestureDirection direction) {
    if (!isVisible() || !allowsWorkspaceSwitchInOverview() || m_gestureSession.active || m_state.phase != Phase::Active || m_workspaceTransition.active)
        return false;

    PHLMONITOR monitor = Desktop::focusState()->monitor();
    if (!monitor || !containsHandle(m_state.participatingMonitors, monitor))
        monitor = m_state.ownerMonitor;
    if (!monitor)
        return false;

    m_workspaceSwipeGesture = {
        .active = true,
        .monitor = monitor,
        .direction = direction,
    };

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace swipe begin monitor=" << monitor->m_name << " dir="
            << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : direction == TRACKPAD_GESTURE_DIR_VERTICAL ? "vertical" : "other");
        debugLog(out.str());
    }

    return true;
}

bool OverviewController::beginOverviewWorkspaceTransition(const PHLMONITOR& monitor, WORKSPACEID workspaceId, std::string workspaceName, PHLWORKSPACE workspace,
                                                         bool syntheticEmpty, WorkspaceTransitionMode mode) {
    if (!monitor || !isVisible() || m_state.phase != Phase::Active)
        return false;

    if (m_state.relayoutActive) {
        for (auto& managed : m_state.windows) {
            managed.targetGlobal = currentPreviewRect(managed);
            managed.relayoutFromGlobal = managed.targetGlobal;
        }
        m_state.relayoutActive = false;
        m_state.relayoutProgress = 1.0;
        m_state.relayoutStart = {};
    }

    State source = m_state;
    source.phase = Phase::Active;
    source.relayoutActive = false;
    source.relayoutProgress = 1.0;
    source.relayoutStart = {};
    for (auto& managed : source.windows) {
        if (const auto* currentManaged = managedWindowFor(m_state, managed.window, false))
            managed.targetGlobal = currentPreviewRect(*currentManaged);
        managed.relayoutFromGlobal = managed.targetGlobal;
    }

    const auto anchorMonitor = m_state.ownerMonitor ? m_state.ownerMonitor : monitor;
    std::vector<WorkspaceOverride> overrides = {{
        .monitorId = monitor->m_id,
        .workspace = workspace,
        .workspaceId = workspaceId,
        .workspaceName = std::move(workspaceName),
        .syntheticEmpty = syntheticEmpty,
    }};

    State target = buildState(anchorMonitor, m_state.collectionPolicy.requestedScope, overrides, true);
    if (target.participatingMonitors.empty())
        return false;

    if (workspace)
        target.ownerWorkspace = workspace;

    target.phase = Phase::Active;
    target.focusBeforeOpen = m_state.focusBeforeOpen;
    target.closeMode = m_state.closeMode;
    target.pendingExitFocus = m_state.pendingExitFocus;
    target.relayoutActive = false;
    target.relayoutProgress = 1.0;
    target.relayoutStart = {};

    const auto transitionWorkspace = monitor->m_activeWorkspace ? monitor->m_activeWorkspace : source.ownerWorkspace;
    const auto transitionAxis = workspaceSwipeUsesVerticalAxis(transitionWorkspace) ? WorkspaceTransitionAxis::Vertical : WorkspaceTransitionAxis::Horizontal;

    m_workspaceTransition = {
        .active = true,
        .monitor = monitor,
        .gestureDirection = m_workspaceSwipeGesture.direction,
        .axis = transitionAxis,
        .mode = mode,
        .distance = workspaceSwipeViewportDistance(monitor, transitionAxis),
        .delta = 0.0,
        .step = workspaceId > monitor->m_activeWorkspace->m_id ? 1 : -1,
        .initialDirection = 0,
        .avgSpeed = 0.0,
        .speedPoints = 0,
        .targetWorkspaceId = workspaceId,
        .targetWorkspaceName = overrides.front().workspaceName,
        .targetWorkspaceSyntheticEmpty = syntheticEmpty,
        .sourceState = std::move(source),
        .targetState = std::move(target),
        .animationFromDelta = 0.0,
        .animationToDelta = 0.0,
        .animationProgress = 0.0,
        .animationStart = {},
    };

    if (mode == WorkspaceTransitionMode::TimedCommit)
        m_workspaceTransition.animationToDelta = static_cast<double>(m_workspaceTransition.step) * m_workspaceTransition.distance;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace transition begin monitor=" << monitor->m_name << " targetId=" << workspaceId
            << " synthetic=" << (syntheticEmpty ? 1 : 0) << " mode="
            << (mode == WorkspaceTransitionMode::Gesture ? "gesture" : mode == WorkspaceTransitionMode::TimedCommit ? "commit" : "revert")
            << " axis=" << (m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical ? "vertical" : "horizontal");
        debugLog(out.str());
    }

    refreshOwnedMonitors();
    return true;
}

bool OverviewController::startOverviewWorkspaceTransitionByStep(const PHLMONITOR& monitor, int step, WorkspaceTransitionMode mode) {
    if (!allowsWorkspaceSwitchInOverview() || !monitor || step == 0)
        return false;

    WORKSPACEID targetId = WORKSPACE_INVALID;
    std::string targetName;
    PHLWORKSPACE targetWorkspace;
    bool syntheticEmpty = false;
    if (!resolveOverviewWorkspaceTargetByStep(monitor, step, targetId, targetName, targetWorkspace, syntheticEmpty))
        return false;

    if (!beginOverviewWorkspaceTransition(monitor, targetId, std::move(targetName), targetWorkspace, syntheticEmpty, mode))
        return false;

    m_workspaceTransition.step = step < 0 ? -1 : 1;
    if (mode == WorkspaceTransitionMode::TimedCommit)
        m_workspaceTransition.animationToDelta = static_cast<double>(m_workspaceTransition.step) * m_workspaceTransition.distance;
    return true;
}

void OverviewController::updateOverviewWorkspaceSwipeGesture(double delta) {
    if (!m_workspaceSwipeGesture.active || !m_workspaceSwipeGesture.monitor)
        return;

    const double signedDelta = workspaceSwipeInvertEnabled() ? -delta : delta;
    if (std::abs(signedDelta) < 0.0001)
        return;

    const double candidateTotal = (m_workspaceTransition.active ? m_workspaceTransition.delta : 0.0) + signedDelta;
    if (std::abs(candidateTotal) < 0.0001) {
        if (m_workspaceTransition.active) {
            m_workspaceTransition.delta = 0.0;
            refreshOwnedMonitors();
        }
        return;
    }

    const int intendedStep = candidateTotal < 0.0 ? -1 : 1;
    if (!m_workspaceTransition.active || m_workspaceTransition.step != intendedStep) {
        if (!startOverviewWorkspaceTransitionByStep(m_workspaceSwipeGesture.monitor, intendedStep, WorkspaceTransitionMode::Gesture))
            return;
    }

    double nextDelta = candidateTotal;
    if (gestureSwipeDirectionLockEnabled()) {
        if (m_workspaceTransition.initialDirection != 0 && m_workspaceTransition.initialDirection != (nextDelta < 0.0 ? -1 : 1)) {
            nextDelta = 0.0;
        } else if (m_workspaceTransition.initialDirection == 0 && std::abs(nextDelta) > gestureSwipeDirectionLockThreshold()) {
            m_workspaceTransition.initialDirection = nextDelta < 0.0 ? -1 : 1;
        }
    }

    const double previousDelta = m_workspaceTransition.delta;
    m_workspaceTransition.delta = std::clamp(nextDelta, -m_workspaceTransition.distance, m_workspaceTransition.distance);
    const double deltaStep = std::abs(previousDelta - m_workspaceTransition.delta);
    m_workspaceTransition.avgSpeed = (m_workspaceTransition.avgSpeed * static_cast<double>(m_workspaceTransition.speedPoints) + deltaStep) /
        static_cast<double>(m_workspaceTransition.speedPoints + 1);
    ++m_workspaceTransition.speedPoints;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace swipe update delta=" << m_workspaceTransition.delta << " avgSpeed=" << m_workspaceTransition.avgSpeed
            << " step=" << m_workspaceTransition.step;
        debugLog(out.str());
    }

    refreshOwnedMonitors();

    if (gestureSwipeForeverEnabled() && std::abs(m_workspaceTransition.delta) >= m_workspaceTransition.distance - 0.5)
        commitOverviewWorkspaceTransition(true);
}

void OverviewController::endOverviewWorkspaceSwipeGesture(bool cancelled) {
    m_workspaceSwipeGesture.active = false;

    if (!m_workspaceTransition.active)
        return;

    const double cancelRatio = std::clamp(getConfigFloat(m_handle, "gestures:workspace_swipe_cancel_ratio", 0.5), 0.0, 1.0);
    const double speedThreshold = gestureForceSpeedThreshold();
    const bool revert =
        cancelled || ((std::abs(m_workspaceTransition.delta) < m_workspaceTransition.distance * cancelRatio &&
                       (speedThreshold == 0.0 || m_workspaceTransition.avgSpeed < speedThreshold)) ||
                      std::abs(m_workspaceTransition.delta) < 2.0);

    m_workspaceTransition.mode = revert ? WorkspaceTransitionMode::TimedRevert : WorkspaceTransitionMode::TimedCommit;
    m_workspaceTransition.animationFromDelta = m_workspaceTransition.delta;
    m_workspaceTransition.animationToDelta = revert ? 0.0 : static_cast<double>(m_workspaceTransition.step) * m_workspaceTransition.distance;
    m_workspaceTransition.animationProgress = 0.0;
    m_workspaceTransition.animationStart = {};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace swipe end cancelled=" << (cancelled ? 1 : 0) << " revert=" << (revert ? 1 : 0)
            << " delta=" << m_workspaceTransition.delta << " avgSpeed=" << m_workspaceTransition.avgSpeed;
        debugLog(out.str());
    }

    refreshOwnedMonitors();
}

void OverviewController::updateOverviewWorkspaceTransition() {
    if (!m_workspaceTransition.active || m_workspaceTransition.mode == WorkspaceTransitionMode::Gesture)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_workspaceTransition.animationStart == std::chrono::steady_clock::time_point{}) {
        m_workspaceTransition.animationStart = now;
        m_workspaceTransition.animationProgress = 0.0;
        return;
    }

    const double elapsedMs = std::chrono::duration<double, std::milli>(now - m_workspaceTransition.animationStart).count();
    m_workspaceTransition.animationProgress = clampUnit(elapsedMs / WORKSPACE_TRANSITION_DURATION_MS);
    const double eased = easeOutCubic(m_workspaceTransition.animationProgress);
    m_workspaceTransition.delta =
        m_workspaceTransition.animationFromDelta + (m_workspaceTransition.animationToDelta - m_workspaceTransition.animationFromDelta) * eased;

    if (m_workspaceTransition.animationProgress < 1.0)
        return;

    if (m_workspaceTransition.mode == WorkspaceTransitionMode::TimedRevert) {
        clearOverviewWorkspaceTransition();
        updateHoveredFromPointer(focusFollowsMouseEnabled(), false);
        refreshOwnedMonitors();
        return;
    }

    commitOverviewWorkspaceTransition(false);
}

void OverviewController::commitOverviewWorkspaceTransition(bool followGesture) {
    if (!m_workspaceTransition.active || !m_workspaceTransition.monitor)
        return;

    const auto oldWorkspace = m_workspaceTransition.monitor->m_activeWorkspace;
    auto targetWorkspace = g_pCompositor->getWorkspaceByID(m_workspaceTransition.targetWorkspaceId);
    if (!targetWorkspace && m_workspaceTransition.targetWorkspaceSyntheticEmpty) {
        targetWorkspace = g_pCompositor->createNewWorkspace(m_workspaceTransition.targetWorkspaceId, m_workspaceTransition.monitor->m_id, m_workspaceTransition.targetWorkspaceName);
    }
    if (!targetWorkspace) {
        clearOverviewWorkspaceTransition();
        refreshOwnedMonitors();
        return;
    }

    const bool temporarilyDisabledAnimations = !m_animationsEnabledOverridden;
    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(true);

    m_applyingWorkspaceTransitionCommit = true;
    if (oldWorkspace && oldWorkspace != targetWorkspace) {
        for (const auto& window : g_pCompositor->m_windows) {
            if (!window || window->m_workspace != oldWorkspace || !window->m_pinned)
                continue;

            window->layoutTarget()->assignToSpace(targetWorkspace->m_space);
        }
    }
    m_workspaceTransition.monitor->changeWorkspace(targetWorkspace, true, true, true);
    // `internal=true` skips Hyprland's workspace IN animation, so the target
    // workspace can retain its old off-screen renderOffset (e.g. +/- one
    // monitor height). Normalize it immediately or the new active workspace
    // remains visually shifted after the overview transition commits.
    targetWorkspace->m_renderOffset->setValueAndWarp(Vector2D{});
    targetWorkspace->m_alpha->setValueAndWarp(1.F);
    g_layoutManager->recalculateMonitor(m_workspaceTransition.monitor);
    if (g_pAnimationManager)
        g_pAnimationManager->frameTick();

    State next = std::move(m_workspaceTransition.targetState);
    next.phase = Phase::Active;
    next.focusBeforeOpen = m_state.focusBeforeOpen;
    next.pendingExitFocus = m_state.pendingExitFocus;
    next.closeMode = m_state.closeMode;
    next.relayoutActive = false;
    next.relayoutProgress = 1.0;
    next.relayoutStart = {};

    clearOverviewWorkspaceTransition();
    m_state = std::move(next);
    applyWorkspaceNameOverrides(m_state);
    if (const auto focused = Desktop::focusState()->window()) {
        const auto focusedIt =
            std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == focused; });
        if (focusedIt != m_state.windows.end()) {
            m_state.selectedIndex = static_cast<std::size_t>(std::distance(m_state.windows.begin(), focusedIt));
            m_state.focusDuringOverview = focused;
        }
    }
    if (g_pEventManager) {
        g_pEventManager->postEvent(SHyprIPCEvent{"workspace", targetWorkspace->m_name});
        g_pEventManager->postEvent(SHyprIPCEvent{"workspacev2", std::format("{},{}", targetWorkspace->m_id, targetWorkspace->m_name)});
    }
    Event::bus()->m_events.workspace.active.emit(targetWorkspace);
    m_applyingWorkspaceTransitionCommit = false;

    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(false);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace transition commit target=" << targetWorkspace->m_name << " followGesture=" << (followGesture ? 1 : 0);
        debugLog(out.str());
    }

    updateHoveredFromPointer(focusFollowsMouseEnabled(), true, false);
    refreshOwnedMonitors();
}

void OverviewController::clearOverviewWorkspaceTransition() {
    m_workspaceTransition = {};
}

SDispatchResult OverviewController::startOverviewWorkspaceTransitionForDispatcher(const std::string& args, bool currentMonitorOnly) {
    if (m_workspaceTransition.active)
        return {};

    if (m_state.phase != Phase::Active)
        return {};

    const auto monitor = Desktop::focusState()->monitor();
    if (!monitor || !monitor->m_activeWorkspace)
        return {.success = false, .error = "no active monitor for overview workspace transition"};

    if (!allowsWorkspaceSwitchInOverview())
        return {.success = false, .error = "overview workspace transition unavailable"};

    WORKSPACEID targetId = WORKSPACE_INVALID;
    std::string targetName;
    PHLWORKSPACE targetWorkspace;
    bool syntheticEmpty = false;

    if (currentMonitorOnly) {
        auto [workspaceId, workspaceName, isAutoID] = getWorkspaceIDNameFromString(args);
        if (workspaceId == WORKSPACE_INVALID)
            return {.success = false, .error = "focusWorkspaceOnCurrentMonitor invalid workspace!"};

        targetId = workspaceId;
        targetName = workspaceName;
        targetWorkspace = g_pCompositor->getWorkspaceByID(targetId);
        if (targetWorkspace && targetWorkspace->m_monitor.lock() != monitor)
            return {.success = false, .error = "focusWorkspaceOnCurrentMonitor workspace is on another monitor"};
        syntheticEmpty = !targetWorkspace;
    } else {
        const auto currentWorkspace = monitor->m_activeWorkspace;
        if (!currentWorkspace)
            return {.success = false, .error = "Last monitor not found"};

        auto resolveWorkspaceToChange = [&](std::string value) -> SWorkspaceIDName {
            if (!value.starts_with("previous"))
                return getWorkspaceIDNameFromString(value);

            const bool perMonitor = value.contains("_per_monitor");
            const auto previous = perMonitor ? Desktop::History::workspaceTracker()->previousWorkspaceIDName(currentWorkspace, monitor) :
                                               Desktop::History::workspaceTracker()->previousWorkspaceIDName(currentWorkspace);
            if (previous.id == -1 || previous.id == currentWorkspace->m_id)
                return {.id = WORKSPACE_NOT_CHANGED};

            if (const auto existing = g_pCompositor->getWorkspaceByID(previous.id); existing)
                return {.id = existing->m_id, .name = existing->m_name};

            return {.id = previous.id, .name = previous.name.empty() ? std::to_string(previous.id) : previous.name};
        };

        static auto PBACKANDFORTH = CConfigValue<Hyprlang::INT>("binds:workspace_back_and_forth");
        const bool explicitPrevious = args.contains("previous");
        const auto resolved = resolveWorkspaceToChange(args);
        if (resolved.id == WORKSPACE_INVALID)
            return {.success = false, .error = "Error in changeworkspace, invalid value"};
        if (resolved.id == WORKSPACE_NOT_CHANGED)
            return {};

        const auto previousWorkspace = args.contains("_per_monitor") ? Desktop::History::workspaceTracker()->previousWorkspaceIDName(currentWorkspace, monitor) :
                                                                       Desktop::History::workspaceTracker()->previousWorkspaceIDName(currentWorkspace);
        const bool targetCurrent = resolved.id == currentWorkspace->m_id;
        if (targetCurrent && ((!*PBACKANDFORTH && !explicitPrevious) || previousWorkspace.id == -1))
            return {.success = false, .error = "Previous workspace doesn't exist"};

        targetId = targetCurrent ? previousWorkspace.id : resolved.id;
        targetName = targetCurrent ? (previousWorkspace.name.empty() ? std::to_string(previousWorkspace.id) : previousWorkspace.name) : resolved.name;
        targetWorkspace = g_pCompositor->getWorkspaceByID(targetId);
        if (targetWorkspace && targetWorkspace->m_isSpecialWorkspace)
            return {.success = false, .error = "overview workspace transition does not support special workspaces"};
        if (targetWorkspace && targetWorkspace->m_monitor.lock() != monitor)
            return {.success = false, .error = "overview workspace transition requires workspace on current monitor"};
        syntheticEmpty = !targetWorkspace;
    }

    if (!beginOverviewWorkspaceTransition(monitor, targetId, targetName, targetWorkspace, syntheticEmpty, WorkspaceTransitionMode::TimedCommit))
        return {.success = false, .error = "failed to start overview workspace transition"};

    return {};
}

void OverviewController::setInputFollowMouseOverride(bool disable) {
    const auto* value = HyprlandAPI::getConfigValue(m_handle, "input:follow_mouse");
    if (!value)
        return;

    const auto* data = reinterpret_cast<Hyprlang::INT* const*>(value->getDataStaticPtr());
    if (!data || !*data)
        return;

    if (disable) {
        if (m_inputFollowMouseOverridden)
            return;

        m_inputFollowMouseBackup = static_cast<long>(**data);
        const auto err = g_pConfigManager->parseKeyword("input:follow_mouse", "0");
        if (!err.empty()) {
            notify("[hymission] failed to disable input:follow_mouse", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
            return;
        }

        m_inputFollowMouseOverridden = true;
        return;
    }

    if (!m_inputFollowMouseOverridden)
        return;

    const auto err = g_pConfigManager->parseKeyword("input:follow_mouse", std::to_string(m_inputFollowMouseBackup));
    if (!err.empty()) {
        notify("[hymission] failed to restore input:follow_mouse", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return;
    }

    m_inputFollowMouseOverridden = false;
}

void OverviewController::setScrollingFollowFocusOverride(bool disable) {
    if (!hasScrollingWorkspace())
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

void OverviewController::setAnimationsEnabledOverride(bool disable, std::optional<std::chrono::milliseconds> restoreDelay) {
    const auto* value = HyprlandAPI::getConfigValue(m_handle, "animations:enabled");
    if (!value)
        return;

    const auto* data = reinterpret_cast<Hyprlang::INT* const*>(value->getDataStaticPtr());
    if (!data || !*data)
        return;

    if (disable) {
        if (!m_animationsEnabledOverridden) {
            m_animationsEnabledBackup = static_cast<long>(**data);
            if (m_animationsEnabledBackup == 0)
                return;

            const auto err = g_pConfigManager->parseKeyword("animations:enabled", "0");
            if (!err.empty()) {
                notify("[hymission] failed to disable animations:enabled", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
                return;
            }

            m_animationsEnabledOverridden = true;
            if (debugLogsEnabled())
                debugLog("[hymission] disabled animations:enabled");
        }

        if (m_animationsEnabledOverridden && restoreDelay) {
            if (!m_animationsEnabledRestoreTimer) {
                m_animationsEnabledRestoreTimer = makeShared<CEventLoopTimer>(
                    *restoreDelay,
                    [this](SP<CEventLoopTimer> self, void* data) { setAnimationsEnabledOverride(false); },
                    nullptr);
                g_pEventLoopManager->addTimer(m_animationsEnabledRestoreTimer);
            } else {
                m_animationsEnabledRestoreTimer->updateTimeout(*restoreDelay);
            }

            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] animations restore scheduled in " << restoreDelay->count() << "ms";
                debugLog(out.str());
            }
        }

        return;
    }

    if (m_animationsEnabledRestoreTimer) {
        g_pEventLoopManager->removeTimer(m_animationsEnabledRestoreTimer);
        m_animationsEnabledRestoreTimer.reset();
    }

    if (!m_animationsEnabledOverridden)
        return;

    const auto err = g_pConfigManager->parseKeyword("animations:enabled", std::to_string(m_animationsEnabledBackup));
    if (!err.empty()) {
        notify("[hymission] failed to restore animations:enabled", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return;
    }

    m_animationsEnabledOverridden = false;
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] restored animations:enabled=" << m_animationsEnabledBackup;
        debugLog(out.str());
    }
}

bool OverviewController::installHooks() {
    const auto activateOptionalHook = [&](CFunctionHook*& hook, auto& original, const char* label) {
        if (!hook)
            return;

        if (!hook->hook()) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] optional hook activation failed: " << label;
                debugLog(out.str());
            }
            HyprlandAPI::removeFunctionHook(m_handle, hook);
            hook = nullptr;
            original = nullptr;
            return;
        }

        using OriginalT = std::remove_reference_t<decltype(original)>;
        original = reinterpret_cast<OriginalT>(hook->m_original);
    };

    if (!hookFunction("handleGesture", "CConfigManager::handleGesture(", m_handleGestureHook, reinterpret_cast<void*>(&hkHandleGesture))) {
        notify("[hymission] failed to hook handleGesture", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }
    if (!m_handleGestureHook->hook()) {
        notify("[hymission] failed to activate handleGesture hook", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }
    m_handleGestureOriginal = reinterpret_cast<HandleGestureFn>(m_handleGestureHook->m_original);

    if (!hookFunction("shouldRenderWindow",
                      "CHyprRenderer::shouldRenderWindow(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<CMonitor>)",
                      m_shouldRenderWindowHook, reinterpret_cast<void*>(&hkShouldRenderWindow))) {
        notify("[hymission] failed to hook shouldRenderWindow(window, monitor)", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("getTexBox", "CSurfacePassElement::getTexBox(", m_surfaceTexBoxHook, reinterpret_cast<void*>(&hkSurfaceTexBox))) {
        notify("[hymission] failed to hook getTexBox", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("boundingBox", "CSurfacePassElement::boundingBox(", m_surfaceBoundingBoxHook, reinterpret_cast<void*>(&hkSurfaceBoundingBox))) {
        notify("[hymission] failed to hook boundingBox", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("opaqueRegion", "CSurfacePassElement::opaqueRegion(", m_surfaceOpaqueRegionHook, reinterpret_cast<void*>(&hkSurfaceOpaqueRegion))) {
        notify("[hymission] failed to hook opaqueRegion", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("visibleRegion", "CSurfacePassElement::visibleRegion(", m_surfaceVisibleRegionHook, reinterpret_cast<void*>(&hkSurfaceVisibleRegion))) {
        notify("[hymission] failed to hook visibleRegion", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("draw", "CSurfacePassElement::draw(", m_surfaceDrawHook, reinterpret_cast<void*>(&hkSurfaceDraw))) {
        notify("[hymission] failed to hook surface draw", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("needsLiveBlur", "CSurfacePassElement::needsLiveBlur(", m_surfaceNeedsLiveBlurHook, reinterpret_cast<void*>(&hkSurfaceNeedsLiveBlur))) {
        notify("[hymission] failed to hook surface needsLiveBlur", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("needsPrecomputeBlur", "CSurfacePassElement::needsPrecomputeBlur(", m_surfaceNeedsPrecomputeBlurHook,
                      reinterpret_cast<void*>(&hkSurfaceNeedsPrecomputeBlur))) {
        notify("[hymission] failed to hook surface needsPrecomputeBlur", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("draw", "CHyprBorderDecoration::draw(", m_borderDrawHook, reinterpret_cast<void*>(&hkBorderDraw))) {
        notify("[hymission] failed to hook border decoration draw", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("draw", "CHyprDropShadowDecoration::draw(", m_shadowDrawHook, reinterpret_cast<void*>(&hkShadowDraw))) {
        notify("[hymission] failed to hook shadow decoration draw", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("calculateUVForSurface", "CHyprRenderer::calculateUVForSurface(", m_calculateUVForSurfaceHook, reinterpret_cast<void*>(&hkCalculateUVForSurface))) {
        notify("[hymission] failed to hook calculateUVForSurface", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("fullscreenActive", "CKeybindManager::fullscreenActive(", m_fullscreenActiveHook, reinterpret_cast<void*>(&hkFullscreenActive))) {
        notify("[hymission] failed to hook fullscreenActive", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    if (!hookFunction("fullscreenStateActive", "CKeybindManager::fullscreenStateActive(", m_fullscreenStateActiveHook,
                      reinterpret_cast<void*>(&hkFullscreenStateActive))) {
        notify("[hymission] failed to hook fullscreenStateActive", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    (void)hookFunction("changeworkspace", "CKeybindManager::changeworkspace(", m_changeWorkspaceHook, reinterpret_cast<void*>(&hkChangeWorkspace));
    (void)hookFunction("focusWorkspaceOnCurrentMonitor", "CKeybindManager::focusWorkspaceOnCurrentMonitor(", m_focusWorkspaceOnCurrentMonitorHook,
                       reinterpret_cast<void*>(&hkFocusWorkspaceOnCurrentMonitor));
    (void)hookFunction("begin", "CWorkspaceSwipeGesture::begin(", m_workspaceSwipeBeginFunctionHook, reinterpret_cast<void*>(&hkWorkspaceSwipeBegin));
    (void)hookFunction("update", "CWorkspaceSwipeGesture::update(", m_workspaceSwipeUpdateFunctionHook, reinterpret_cast<void*>(&hkWorkspaceSwipeUpdate));
    (void)hookFunction("end", "CWorkspaceSwipeGesture::end(", m_workspaceSwipeEndFunctionHook, reinterpret_cast<void*>(&hkWorkspaceSwipeEnd));

    m_shouldRenderWindowOriginal = nullptr;
    m_surfaceTexBoxOriginal = nullptr;
    m_surfaceBoundingBoxOriginal = nullptr;
    m_surfaceOpaqueRegionOriginal = nullptr;
    m_surfaceVisibleRegionOriginal = nullptr;
    m_surfaceDrawOriginal = nullptr;
    m_surfaceNeedsLiveBlurOriginal = nullptr;
    m_surfaceNeedsPrecomputeBlurOriginal = nullptr;
    m_borderDrawOriginal = nullptr;
    m_shadowDrawOriginal = nullptr;
    m_calculateUVForSurfaceOriginal = nullptr;
    m_fullscreenActiveOriginal = nullptr;
    m_fullscreenStateActiveOriginal = nullptr;
    m_changeWorkspaceOriginal = nullptr;
    m_focusWorkspaceOnCurrentMonitorOriginal = nullptr;
    m_workspaceSwipeBeginOriginal = nullptr;
    m_workspaceSwipeUpdateOriginal = nullptr;
    m_workspaceSwipeEndOriginal = nullptr;

    activateOptionalHook(m_changeWorkspaceHook, m_changeWorkspaceOriginal, "changeworkspace");
    activateOptionalHook(m_focusWorkspaceOnCurrentMonitorHook, m_focusWorkspaceOnCurrentMonitorOriginal, "focusWorkspaceOnCurrentMonitor");
    activateOptionalHook(m_workspaceSwipeBeginFunctionHook, m_workspaceSwipeBeginOriginal, "workspace swipe begin");
    activateOptionalHook(m_workspaceSwipeUpdateFunctionHook, m_workspaceSwipeUpdateOriginal, "workspace swipe update");
    activateOptionalHook(m_workspaceSwipeEndFunctionHook, m_workspaceSwipeEndOriginal, "workspace swipe end");
    return true;
}

bool OverviewController::activateHooks() {
    if (m_hooksActive)
        return true;

    if (!m_shouldRenderWindowHook || !m_surfaceTexBoxHook || !m_surfaceBoundingBoxHook || !m_surfaceOpaqueRegionHook || !m_surfaceVisibleRegionHook || !m_surfaceDrawHook ||
        !m_surfaceNeedsLiveBlurHook || !m_surfaceNeedsPrecomputeBlurHook || !m_borderDrawHook || !m_shadowDrawHook || !m_calculateUVForSurfaceHook ||
        !m_fullscreenActiveHook || !m_fullscreenStateActiveHook)
        return false;

    const bool hooked = m_shouldRenderWindowHook->hook() && m_surfaceTexBoxHook->hook() && m_surfaceBoundingBoxHook->hook() && m_surfaceOpaqueRegionHook->hook() &&
        m_surfaceVisibleRegionHook->hook() && m_surfaceDrawHook->hook() && m_surfaceNeedsLiveBlurHook->hook() && m_surfaceNeedsPrecomputeBlurHook->hook() &&
        m_borderDrawHook->hook() && m_shadowDrawHook->hook() && m_calculateUVForSurfaceHook->hook() && m_fullscreenActiveHook->hook() &&
        m_fullscreenStateActiveHook->hook();
    if (!hooked) {
        notify("[hymission] surface pass hook attach failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        if (m_shouldRenderWindowHook)
            m_shouldRenderWindowHook->unhook();
        if (m_surfaceTexBoxHook)
            m_surfaceTexBoxHook->unhook();
        if (m_surfaceBoundingBoxHook)
            m_surfaceBoundingBoxHook->unhook();
        if (m_surfaceOpaqueRegionHook)
            m_surfaceOpaqueRegionHook->unhook();
        if (m_surfaceVisibleRegionHook)
            m_surfaceVisibleRegionHook->unhook();
        if (m_surfaceDrawHook)
            m_surfaceDrawHook->unhook();
        if (m_surfaceNeedsLiveBlurHook)
            m_surfaceNeedsLiveBlurHook->unhook();
        if (m_surfaceNeedsPrecomputeBlurHook)
            m_surfaceNeedsPrecomputeBlurHook->unhook();
        if (m_borderDrawHook)
            m_borderDrawHook->unhook();
        if (m_shadowDrawHook)
            m_shadowDrawHook->unhook();
        if (m_calculateUVForSurfaceHook)
            m_calculateUVForSurfaceHook->unhook();
        if (m_fullscreenActiveHook)
            m_fullscreenActiveHook->unhook();
        if (m_fullscreenStateActiveHook)
            m_fullscreenStateActiveHook->unhook();
        return false;
    }

    m_shouldRenderWindowOriginal = reinterpret_cast<ShouldRenderWindowFn>(m_shouldRenderWindowHook->m_original);
    m_surfaceTexBoxOriginal = reinterpret_cast<SurfaceGetTexBoxFn>(m_surfaceTexBoxHook->m_original);
    m_surfaceBoundingBoxOriginal = reinterpret_cast<SurfaceBoundingBoxFn>(m_surfaceBoundingBoxHook->m_original);
    m_surfaceOpaqueRegionOriginal = reinterpret_cast<SurfaceOpaqueRegionFn>(m_surfaceOpaqueRegionHook->m_original);
    m_surfaceVisibleRegionOriginal = reinterpret_cast<SurfaceVisibleRegionFn>(m_surfaceVisibleRegionHook->m_original);
    m_surfaceDrawOriginal = reinterpret_cast<SurfaceDrawFn>(m_surfaceDrawHook->m_original);
    m_surfaceNeedsLiveBlurOriginal = reinterpret_cast<SurfaceBlurNeedsFn>(m_surfaceNeedsLiveBlurHook->m_original);
    m_surfaceNeedsPrecomputeBlurOriginal = reinterpret_cast<SurfaceBlurNeedsFn>(m_surfaceNeedsPrecomputeBlurHook->m_original);
    m_borderDrawOriginal = reinterpret_cast<BorderDrawFn>(m_borderDrawHook->m_original);
    m_shadowDrawOriginal = reinterpret_cast<BorderDrawFn>(m_shadowDrawHook->m_original);
    m_calculateUVForSurfaceOriginal = reinterpret_cast<CalculateUVForSurfaceFn>(m_calculateUVForSurfaceHook->m_original);
    m_fullscreenActiveOriginal = reinterpret_cast<DispatcherFn>(m_fullscreenActiveHook->m_original);
    m_fullscreenStateActiveOriginal = reinterpret_cast<DispatcherFn>(m_fullscreenStateActiveHook->m_original);
    m_hooksActive = true;
    return true;
}

void OverviewController::deactivateHooks() {
    if (!m_hooksActive)
        return;

    if (m_shouldRenderWindowHook)
        m_shouldRenderWindowHook->unhook();
    if (m_surfaceTexBoxHook)
        m_surfaceTexBoxHook->unhook();
    if (m_surfaceBoundingBoxHook)
        m_surfaceBoundingBoxHook->unhook();
    if (m_surfaceOpaqueRegionHook)
        m_surfaceOpaqueRegionHook->unhook();
    if (m_surfaceVisibleRegionHook)
        m_surfaceVisibleRegionHook->unhook();
    if (m_surfaceDrawHook)
        m_surfaceDrawHook->unhook();
    if (m_surfaceNeedsLiveBlurHook)
        m_surfaceNeedsLiveBlurHook->unhook();
    if (m_surfaceNeedsPrecomputeBlurHook)
        m_surfaceNeedsPrecomputeBlurHook->unhook();
    if (m_borderDrawHook)
        m_borderDrawHook->unhook();
    if (m_shadowDrawHook)
        m_shadowDrawHook->unhook();
    if (m_calculateUVForSurfaceHook)
        m_calculateUVForSurfaceHook->unhook();
    if (m_fullscreenActiveHook)
        m_fullscreenActiveHook->unhook();
    if (m_fullscreenStateActiveHook)
        m_fullscreenStateActiveHook->unhook();

    m_shouldRenderWindowOriginal = nullptr;
    m_surfaceTexBoxOriginal = nullptr;
    m_surfaceBoundingBoxOriginal = nullptr;
    m_surfaceOpaqueRegionOriginal = nullptr;
    m_surfaceVisibleRegionOriginal = nullptr;
    m_surfaceDrawOriginal = nullptr;
    m_surfaceNeedsLiveBlurOriginal = nullptr;
    m_surfaceNeedsPrecomputeBlurOriginal = nullptr;
    m_borderDrawOriginal = nullptr;
    m_shadowDrawOriginal = nullptr;
    m_calculateUVForSurfaceOriginal = nullptr;
    m_fullscreenActiveOriginal = nullptr;
    m_fullscreenStateActiveOriginal = nullptr;
    m_surfaceRenderDataTransformDepth = 0;
    m_hooksActive = false;
    g_pHyprRenderer->m_directScanoutBlocked = false;
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
    return m_gestureSession.active || m_workspaceTransition.active || m_state.phase == Phase::Opening || m_state.phase == Phase::Closing;
}

bool OverviewController::isVisible() const {
    return m_state.phase != Phase::Inactive;
}

bool OverviewController::shouldHandleInput() const {
    if (m_gestureSession.active || m_workspaceTransition.active || m_workspaceSwipeGesture.active)
        return false;

    return isVisible() && (m_state.phase == Phase::Opening || m_state.phase == Phase::Active);
}

std::vector<PHLMONITOR> OverviewController::ownedMonitors() const {
    std::vector<PHLMONITOR> monitors;
    const auto append = [&](const PHLMONITOR& monitor) {
        if (!monitor || containsHandle(monitors, monitor))
            return;
        monitors.push_back(monitor);
    };

    for (const auto& monitor : m_state.participatingMonitors)
        append(monitor);

    if (m_workspaceTransition.active) {
        for (const auto& monitor : m_workspaceTransition.sourceState.participatingMonitors)
            append(monitor);
        for (const auto& monitor : m_workspaceTransition.targetState.participatingMonitors)
            append(monitor);
    }

    return monitors;
}

bool OverviewController::shouldSyncRealFocusDuringOverview() const {
    return shouldSyncOverviewLiveFocus(shouldHandleInput(), focusFollowsMouseEnabled(), m_inputFollowMouseBackup);
}

bool OverviewController::ownsMonitor(const PHLMONITOR& monitor) const {
    if (!monitor)
        return false;

    const auto monitors = ownedMonitors();
    return containsHandle(monitors, monitor);
}

bool OverviewController::ownsWorkspace(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    if (containsHandle(m_state.managedWorkspaces, workspace))
        return true;

    return m_workspaceTransition.active &&
        (containsHandle(m_workspaceTransition.sourceState.managedWorkspaces, workspace) || containsHandle(m_workspaceTransition.targetState.managedWorkspaces, workspace));
}

bool OverviewController::hasManagedWindow(const PHLWINDOW& window) const {
    return managedWindowFor(window) != nullptr;
}

bool OverviewController::shouldAutoCloseFor(const PHLWINDOW& window) const {
    if (!window || !m_state.ownerMonitor)
        return false;

    if (hasManagedWindow(window))
        return true;

    return shouldManageWindow(window, m_state);
}

bool OverviewController::shouldManageWindow(const PHLWINDOW& window, const State& state) const {
    if (!window)
        return false;

    if (!window->m_isMapped || window->m_fadingOut || window->isHidden())
        return false;

    const auto size = window->m_realSize->value();
    if (size.x < 1.0 || size.y < 1.0)
        return false;

    if (window->m_pinned) {
        if (window->onSpecialWorkspace())
            return false;
        return std::ranges::any_of(state.participatingMonitors, [&](const PHLMONITOR& monitor) { return monitor && window->visibleOnMonitor(monitor); });
    }

    if (!window->m_workspace)
        return false;

    if (window->m_workspace->m_isSpecialWorkspace)
        return state.collectionPolicy.includeSpecial && containsHandle(state.managedWorkspaces, window->m_workspace) && window->m_workspace->isVisible();

    return containsHandle(state.managedWorkspaces, window->m_workspace);
}

std::string OverviewController::collectionSummary(const PHLMONITOR& monitor) const {
    std::string error;
    const auto requestedScope = parseScopeOverride({}, error);
    const auto policy = loadCollectionPolicy(requestedScope.value_or(ScopeOverride::Default));
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

        if (!window->m_workspace && !window->m_pinned) {
            ++noWorkspace;
            continue;
        }

        if (window->onSpecialWorkspace() && !policy.includeSpecial) {
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

        if (!monitor) {
            ++workspaceMismatch;
            continue;
        }

        std::vector<PHLMONITOR> participatingMonitors;
        if (policy.onlyActiveMonitor) {
            participatingMonitors.push_back(monitor);
        } else {
            for (const auto& candidate : g_pCompositor->m_monitors) {
                if (candidate)
                    participatingMonitors.push_back(candidate);
            }
        }

        if (window->m_pinned) {
            const bool visibleOnAnyMonitor =
                std::ranges::any_of(participatingMonitors, [&](const PHLMONITOR& candidate) { return candidate && window->visibleOnMonitor(candidate); });
            if (window->onSpecialWorkspace() || !visibleOnAnyMonitor) {
                ++workspaceMismatch;
                continue;
            }
        } else {
            bool workspaceAccepted = false;
            if (window->m_workspace) {
                if (window->m_workspace->m_isSpecialWorkspace) {
                    workspaceAccepted = policy.includeSpecial && window->m_workspace->isVisible();
                } else if (policy.onlyActiveWorkspace) {
                    workspaceAccepted = std::ranges::any_of(participatingMonitors, [&](const PHLMONITOR& candidate) {
                        return candidate && candidate->m_activeWorkspace && window->m_workspace == candidate->m_activeWorkspace;
                    });
                } else {
                    const auto workspaceMonitor = window->m_workspace->m_monitor.lock();
                    workspaceAccepted = workspaceMonitor && containsHandle(participatingMonitors, workspaceMonitor);
                }
            }

            if (!workspaceAccepted) {
                ++workspaceMismatch;
                continue;
            }
        }

        const auto size = window->m_realSize->value();
        if (size.x < 1.0 || size.y < 1.0) {
            ++invalidSize;
            continue;
        }

        ++accepted;
    }

    std::ostringstream summary;
    summary << "[hymission] collect scope=";
    switch (policy.requestedScope) {
        case ScopeOverride::Default:
            summary << "default";
            break;
        case ScopeOverride::OnlyCurrentWorkspace:
            summary << "onlycurrentworkspace";
            break;
        case ScopeOverride::ForceAll:
            summary << "forceall";
            break;
    }
    summary << " mon=" << (monitor ? monitor->m_name : "?") << " total=" << total << " ok=" << accepted
            << " mismatch=" << workspaceMismatch << " hidden=" << hidden << " unmapped=" << unmapped << " special=" << specialWorkspace;

    if (fading || invalidSize || noWorkspace)
        summary << " fade=" << fading << " size=" << invalidSize << " nows=" << noWorkspace;

    return summary.str();
}

std::vector<Rect> OverviewController::targetRects() const {
    std::vector<Rect> rects;
    rects.reserve(m_state.windows.size());

    for (const auto& window : m_state.windows)
        rects.push_back(currentPreviewRect(window));

    return rects;
}

const OverviewController::ManagedWindow* OverviewController::managedWindowFor(const State& state, const PHLWINDOW& window, bool includeTransient) const {
    const auto it = std::find_if(state.windows.begin(), state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
    if (it != state.windows.end())
        return &*it;

    if (!includeTransient)
        return nullptr;

    const auto transientIt =
        std::find_if(state.transientClosingWindows.begin(), state.transientClosingWindows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
    return transientIt == state.transientClosingWindows.end() ? nullptr : &*transientIt;
}

const OverviewController::ManagedWindow* OverviewController::managedWindowFor(const PHLWINDOW& window) const {
    if (const auto* managed = managedWindowFor(m_state, window, true); managed)
        return managed;

    if (m_workspaceTransition.active) {
        if (const auto* managed = managedWindowFor(m_workspaceTransition.targetState, window, true); managed)
            return managed;
        if (const auto* managed = managedWindowFor(m_workspaceTransition.sourceState, window, true); managed)
            return managed;
    }

    return nullptr;
}

PHLWINDOW OverviewController::selectedWindow() const {
    if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size())
        return {};

    return m_state.windows[*m_state.selectedIndex].window;
}

float OverviewController::managedPreviewAlphaFor(const PHLWINDOW& window, float fallback) const {
    const auto* managed = managedWindowFor(window);
    return managed ? managed->previewAlpha : fallback;
}

PHLMONITOR OverviewController::preferredMonitorForWindow(const PHLWINDOW& window, const State& state) const {
    if (!window)
        return {};

    if (window->m_pinned) {
        for (const auto& monitor : state.participatingMonitors) {
            if (monitor && window->visibleOnMonitor(monitor))
                return monitor;
        }
    }

    if (const auto monitor = window->m_monitor.lock(); monitor && containsHandle(state.participatingMonitors, monitor))
        return monitor;

    if (window->m_workspace) {
        if (const auto workspaceMonitor = window->m_workspace->m_monitor.lock(); workspaceMonitor && containsHandle(state.participatingMonitors, workspaceMonitor))
            return workspaceMonitor;
    }

    if (const auto monitor = g_pCompositor->getMonitorFromID(window->monitorID()); monitor && containsHandle(state.participatingMonitors, monitor))
        return monitor;

    return state.ownerMonitor;
}

PHLMONITOR OverviewController::previewMonitorForWindow(const PHLWINDOW& window) const {
    if (m_workspaceTransition.active) {
        if (const auto* managed = managedWindowFor(m_workspaceTransition.targetState, window, true); managed && managed->targetMonitor)
            return managed->targetMonitor;
        if (const auto* managed = managedWindowFor(m_workspaceTransition.sourceState, window, true); managed && managed->targetMonitor)
            return managed->targetMonitor;
    }

    const auto* managed = managedWindowFor(window);
    if (!managed || !managed->targetMonitor)
        return {};

    return managed->targetMonitor;
}

const OverviewController::FullscreenWorkspaceBackup* OverviewController::fullscreenBackupForWorkspace(const PHLWORKSPACE& workspace) const {
    const auto it = std::find_if(m_state.fullscreenBackups.begin(), m_state.fullscreenBackups.end(),
                                 [&](const FullscreenWorkspaceBackup& backup) { return backup.workspace == workspace; });
    return it == m_state.fullscreenBackups.end() ? nullptr : &*it;
}

const OverviewController::FullscreenWorkspaceBackup* OverviewController::fullscreenBackupForWindow(const PHLWINDOW& window) const {
    return window ? fullscreenBackupForWorkspace(window->m_workspace) : nullptr;
}

Rect OverviewController::liveGlobalRectForWindow(const PHLWINDOW& window) const {
    if (!window)
        return {};

    const auto renderedPos = renderedWindowPosition(window);
    return makeRect(renderedPos.x, renderedPos.y, window->m_realSize->value().x, window->m_realSize->value().y);
}

Rect OverviewController::goalGlobalRectForWindow(const PHLWINDOW& window) const {
    if (!window)
        return {};

    const auto renderedPos = renderedWindowPosition(window, true);
    return makeRect(renderedPos.x, renderedPos.y, window->m_realSize->goal().x, window->m_realSize->goal().y);
}

bool OverviewController::shouldUseGoalGeometryForStateSnapshot(const PHLWINDOW& window) const {
    return window && window->m_workspace && !window->m_workspace->isVisible();
}

void OverviewController::refreshWorkspaceLayoutSnapshot(const PHLWORKSPACE& workspace) const {
    if (!workspace || !workspace->m_space)
        return;

    const bool shouldRefresh = !workspace->isVisible() || isScrollingWorkspace(workspace);
    if (!shouldRefresh)
        return;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] refresh workspace layout snapshot workspace=" << debugWorkspaceLabel(workspace) << " visible=" << (workspace->isVisible() ? 1 : 0)
            << " scrolling=" << (isScrollingWorkspace(workspace) ? 1 : 0);
        debugLog(out.str());
    }

    // Preserve the current scrolling offset when snapshotting overview geometry.
    // Re-centering a hidden workspace to some remembered focus mutates real layout
    // state and makes overview workspace switches inherit a shifted spot.
    workspace->m_space->recalculate();
}

std::optional<Vector2D> OverviewController::predictedScrollingExitTranslation(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped || !window->m_workspace || !window->m_workspace->m_space || !isScrollingWorkspace(window->m_workspace))
        return std::nullopt;

    const auto target = window->layoutTarget();
    if (!target || target->floating())
        return Vector2D{};

    auto direction = getConfigString(m_handle, "scrolling:direction", "right");
    const auto workspaceRule = g_pConfigManager->getWorkspaceRuleFor(window->m_workspace);
    if (workspaceRule.layoutopts.contains("direction") && !workspaceRule.layoutopts.at("direction").empty())
        direction = workspaceRule.layoutopts.at("direction");

    const bool vertical = direction == "down" || direction == "up";
    const auto workArea = window->m_workspace->m_space->workArea();
    const auto targetBox = target->position();

    const double viewStart = vertical ? workArea.y : workArea.x;
    const double viewEnd = vertical ? (workArea.y + workArea.h) : (workArea.x + workArea.w);
    const double stripStart = vertical ? targetBox.y : targetBox.x;
    const double stripEnd = vertical ? (targetBox.y + targetBox.h) : (targetBox.x + targetBox.w);

    double deltaPrimary = 0.0;
    const bool fullyVisible = stripStart >= viewStart && stripEnd <= viewEnd;
    if (!fullyVisible) {
        if (getConfigInt(m_handle, "scrolling:focus_fit_method", 0) == 1) {
            if (stripStart < viewStart)
                deltaPrimary = viewStart - stripStart;
            else if (stripEnd > viewEnd)
                deltaPrimary = viewEnd - stripEnd;
        } else {
            deltaPrimary = (viewStart + viewEnd) * 0.5 - (stripStart + stripEnd) * 0.5;
        }
    }

    return vertical ? Vector2D{0.0, deltaPrimary} : Vector2D{deltaPrimary, 0.0};
}

void OverviewController::prepareGestureCloseExitGeometry() {
    const auto predictedExitFocus = resolveExitFocus(CloseMode::Normal);
    const auto predictedExitWorkspace = predictedExitFocus ? predictedExitFocus->m_workspace : PHLWORKSPACE{};
    const auto predictedExitMonitor =
        predictedExitWorkspace && predictedExitWorkspace->m_monitor.lock() ? predictedExitWorkspace->m_monitor.lock() :
        (predictedExitFocus ? predictedExitFocus->m_monitor.lock() : PHLMONITOR{});
    const auto currentWorkspaceOnTargetMonitor = predictedExitMonitor ? predictedExitMonitor->m_activeWorkspace : PHLWORKSPACE{};
    const auto scrollingTranslation = predictedScrollingExitTranslation(predictedExitFocus);
    const bool preferGoalGeometry = isScrollingWorkspace(predictedExitWorkspace);
    const bool workspaceSwitchOnExit =
        predictedExitWorkspace && predictedExitMonitor && !predictedExitWorkspace->m_isSpecialWorkspace && currentWorkspaceOnTargetMonitor &&
        predictedExitWorkspace != currentWorkspaceOnTargetMonitor;

    Vector2D incomingWorkspaceOffset;
    Vector2D outgoingWorkspaceOffset;
    if (workspaceSwitchOnExit) {
        const bool animToLeft =
            shouldWrapWorkspaceIds(predictedExitWorkspace->m_id, currentWorkspaceOnTargetMonitor->m_id) ^ (predictedExitWorkspace->m_id > currentWorkspaceOnTargetMonitor->m_id);
        incomingWorkspaceOffset = predictedWorkspaceAnimationOffset(m_handle, predictedExitMonitor, predictedExitWorkspace, animToLeft, true);
        outgoingWorkspaceOffset = predictedWorkspaceAnimationOffset(m_handle, predictedExitMonitor, currentWorkspaceOnTargetMonitor, animToLeft, false);
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] prepare gesture close exit";
        if (predictedExitFocus)
            out << " target=" << debugWindowLabel(predictedExitFocus);
        else
            out << " target=<null>";
        out << " workspaceSwitch=" << (workspaceSwitchOnExit ? 1 : 0);
        if (currentWorkspaceOnTargetMonitor)
            out << " currentWorkspace=" << currentWorkspaceOnTargetMonitor->m_id;
        if (predictedExitWorkspace)
            out << " targetWorkspace=" << predictedExitWorkspace->m_id;
        if (scrollingTranslation)
            out << " scrollingDelta=" << vectorToString(*scrollingTranslation);
        else
            out << " scrollingDelta=<none>";
        if (workspaceSwitchOnExit)
            out << " incomingWsDelta=" << vectorToString(incomingWorkspaceOffset) << " outgoingWsDelta=" << vectorToString(outgoingWorkspaceOffset);
        debugLog(out.str());
    }

    for (auto& managed : m_state.windows) {
        managed.exitGlobal = liveGlobalRectForWindow(managed.window);

        if (workspaceSwitchOnExit && managed.window && managed.window->m_workspace) {
            if (managed.window->m_workspace == predictedExitWorkspace) {
                const auto currentOffset = managed.window->m_workspace->m_renderOffset->value();
                const auto targetOffset = preferGoalGeometry ? Vector2D{} : incomingWorkspaceOffset;
                managed.exitGlobal = translateRect(managed.exitGlobal, targetOffset.x - currentOffset.x, targetOffset.y - currentOffset.y);
            } else if (managed.window->m_workspace == currentWorkspaceOnTargetMonitor) {
                if (preferGoalGeometry) {
                    const auto currentOffset = managed.window->m_workspace->m_renderOffset->value();
                    managed.exitGlobal =
                        translateRect(managed.exitGlobal, outgoingWorkspaceOffset.x - currentOffset.x, outgoingWorkspaceOffset.y - currentOffset.y);
                }
            }
        }

        if (!scrollingTranslation || !predictedExitFocus || !managed.window || !managed.window->m_isMapped)
            continue;

        if (managed.window->m_workspace != predictedExitFocus->m_workspace)
            continue;

        const auto layoutTarget = managed.window->layoutTarget();
        if (!layoutTarget || layoutTarget->floating())
            continue;

        managed.exitGlobal = translateRect(managed.exitGlobal, scrollingTranslation->x, scrollingTranslation->y);
    }
}

std::optional<OverviewController::WindowTransform> OverviewController::windowTransformFor(const PHLWINDOW& window, const PHLMONITOR& monitor) const {
    if (!window || !monitor || !isVisible() || !ownsMonitor(monitor))
        return std::nullopt;

    const auto* managed = managedWindowFor(window);
    if (!managed || !managed->targetMonitor || managed->targetMonitor != monitor)
        return std::nullopt;

    const Rect current = workspaceTransitionRectForWindow(window).value_or(currentPreviewRect(*managed));
    const Rect actual = surfaceRenderGlobalRectForWindow(window);
    const double actualWidth = std::max(1.0, actual.width);
    const double actualHeight = std::max(1.0, actual.height);
    return WindowTransform{
        .actualGlobal = actual,
        .targetGlobal = current,
        .scaleX = current.width / actualWidth,
        .scaleY = current.height / actualHeight,
    };
}

bool OverviewController::transformSurfaceRenderDataForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, CSurfacePassElement::SRenderData& renderData) const {
    const auto transform = windowTransformFor(window, monitor);
    if (!transform)
        return false;

    const Vector2D originalPos = renderData.pos;
    const Vector2D originalLocalPos = renderData.localPos;
    const Vector2D effectiveOrigin = originalPos + originalLocalPos;
    const Vector2D transformedLocalPos = Vector2D(originalLocalPos.x * transform->scaleX, originalLocalPos.y * transform->scaleY);
    const Vector2D transformedEffectiveOrigin = Vector2D(transform->targetGlobal.x + (effectiveOrigin.x - transform->actualGlobal.x) * transform->scaleX,
                                                         transform->targetGlobal.y + (effectiveOrigin.y - transform->actualGlobal.y) * transform->scaleY);

    renderData.pos = transformedEffectiveOrigin - transformedLocalPos;
    renderData.localPos = transformedLocalPos;
    renderData.w = std::max(1.0, renderData.w * transform->scaleX);
    renderData.h = std::max(1.0, renderData.h * transform->scaleY);

    // Keep overview previews independent from normal-layout monitor clipping.
    renderData.clipBox = {};

    return true;
}

bool OverviewController::shouldSuppressSurfaceBlur(void* surfacePassThisptr) const {
    if (!isAnimating())
        return false;

    const auto* renderData = surfaceRenderDataMutable(surfacePassThisptr);
    if (!renderData || !renderData->pWindow || renderData->popup || !renderData->blur)
        return false;

    const auto monitor = renderData->pMonitor.lock();
    if (!monitor || !ownsMonitor(monitor) || !hasManagedWindow(renderData->pWindow) || previewMonitorForWindow(renderData->pWindow) != monitor)
        return false;

    if (debugSurfaceLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] blur suppress " << debugWindowLabel(renderData->pWindow) << " phase="
            << (m_state.phase == Phase::Opening ? "opening" : m_state.phase == Phase::Closing ? "closing" : "active");
        debugSurfaceLog(out.str());
    }

    return true;
}

bool OverviewController::prepareSurfaceRenderData(void* surfacePassThisptr, const char* context, CSurfacePassElement::SRenderData*& renderData, PHLMONITOR& monitor,
                                                  SurfaceRenderDataSnapshot& snapshot) const {
    renderData = surfaceRenderDataMutable(surfacePassThisptr);
    if (!renderData || !renderData->pWindow || renderData->popup)
        return false;

    monitor = renderData->pMonitor.lock();
    if (!monitor || !isVisible() || !ownsMonitor(monitor) || !hasManagedWindow(renderData->pWindow) || previewMonitorForWindow(renderData->pWindow) != monitor)
        return false;

    snapshot = {
        .pos = renderData->pos,
        .localPos = renderData->localPos,
        .w = renderData->w,
        .h = renderData->h,
        .alpha = renderData->alpha,
        .fadeAlpha = renderData->fadeAlpha,
        .blur = renderData->blur,
        .blockBlurOptimization = renderData->blockBlurOptimization,
        .clipBox = renderData->clipBox,
    };

    const bool transformed = transformSurfaceRenderDataForWindow(renderData->pWindow, monitor, *renderData);
    if (transformed) {
        renderData->alpha = managedPreviewAlphaFor(renderData->pWindow, snapshot.alpha);
        if (!renderData->pWindow->m_fadingOut && snapshot.fadeAlpha <= 0.001F)
            renderData->fadeAlpha = 1.0F;
    }

    if (transformed && debugSurfaceLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] surface " << (context ? context : "?") << ' ' << debugWindowLabel(renderData->pWindow) << " main=" << renderData->mainSurface
            << " popup=" << renderData->popup << " monitor=" << monitor->m_name << " pos=" << vectorToString(snapshot.pos) << "->" << vectorToString(renderData->pos)
            << " local=" << vectorToString(snapshot.localPos) << "->" << vectorToString(renderData->localPos) << " size=" << snapshot.w << 'x' << snapshot.h << "->"
            << renderData->w << 'x' << renderData->h << " alpha=" << snapshot.alpha << "->" << renderData->alpha << " fadeAlpha=" << snapshot.fadeAlpha << "->"
            << renderData->fadeAlpha << " clip=" << boxToString(snapshot.clipBox) << "->"
            << boxToString(renderData->clipBox);
        debugSurfaceLog(out.str());
    }

    return transformed;
}

void OverviewController::restoreSurfaceRenderData(CSurfacePassElement::SRenderData* renderData, const SurfaceRenderDataSnapshot& snapshot) const {
    if (!renderData)
        return;

    renderData->pos = snapshot.pos;
    renderData->localPos = snapshot.localPos;
    renderData->w = snapshot.w;
    renderData->h = snapshot.h;
    renderData->alpha = snapshot.alpha;
    renderData->fadeAlpha = snapshot.fadeAlpha;
    renderData->blur = snapshot.blur;
    renderData->blockBlurOptimization = snapshot.blockBlurOptimization;
    renderData->clipBox = snapshot.clipBox;
}

std::optional<std::size_t> OverviewController::hitTestTarget(double x, double y) const {
    return hitTest(targetRects(), x, y);
}

std::optional<Rect> OverviewController::workspaceTransitionRectForWindow(const PHLWINDOW& window) const {
    if (!m_workspaceTransition.active)
        return std::nullopt;

    const auto* sourceManaged = managedWindowFor(m_workspaceTransition.sourceState, window, true);
    const auto* targetManaged = managedWindowFor(m_workspaceTransition.targetState, window, true);
    if (!sourceManaged && !targetManaged)
        return std::nullopt;

    const double clampedDelta = std::clamp(m_workspaceTransition.delta, -m_workspaceTransition.distance, m_workspaceTransition.distance);
    const double sourceOffset = -clampedDelta;
    const double targetOffset = sourceOffset + (clampedDelta < 0.0 ? -m_workspaceTransition.distance : m_workspaceTransition.distance);
    const double t = m_workspaceTransition.distance > 0.0 ? clampUnit(std::abs(clampedDelta) / m_workspaceTransition.distance) : 1.0;

    double sourceDx = 0.0;
    double sourceDy = 0.0;
    double targetDx = 0.0;
    double targetDy = 0.0;
    if (m_workspaceTransition.axis == WorkspaceTransitionAxis::Vertical) {
        sourceDy = sourceOffset;
        targetDy = targetOffset;
    } else {
        sourceDx = sourceOffset;
        targetDx = targetOffset;
    }

    if (sourceManaged && targetManaged) {
        const Rect sourceRect = translateRect(sourceManaged->targetGlobal, sourceDx, sourceDy);
        const Rect targetRect = translateRect(targetManaged->targetGlobal, targetDx, targetDy);
        return lerpRect(sourceRect, targetRect, t);
    }

    if (sourceManaged)
        return translateRect(sourceManaged->targetGlobal, sourceDx, sourceDy);

    return translateRect(targetManaged->targetGlobal, targetDx, targetDy);
}

Rect OverviewController::currentPreviewRect(const ManagedWindow& window) const {
    if (m_workspaceTransition.active) {
        if (const auto rect = workspaceTransitionRectForWindow(window.window); rect)
            return *rect;
    }

    if (m_gestureSession.active) {
        if (m_gestureSession.opening)
            return lerpRect(window.naturalGlobal, window.targetGlobal, visualProgress());
        return lerpRect(window.exitGlobal, window.targetGlobal, visualProgress());
    }

    switch (m_state.phase) {
        case Phase::Opening:
            return lerpRect(window.naturalGlobal, window.targetGlobal, visualProgress());
        case Phase::Active:
            if (m_state.relayoutActive)
                return lerpRect(window.relayoutFromGlobal, window.targetGlobal, relayoutVisualProgress());
            return window.targetGlobal;
        case Phase::ClosingSettle:
            return lerpRect(window.exitGlobal, window.targetGlobal, visualProgress());
        case Phase::Closing:
            return lerpRect(window.exitGlobal, window.targetGlobal, visualProgress());
        case Phase::Inactive:
            return window.naturalGlobal;
    }

    return window.targetGlobal;
}

double OverviewController::visualProgress() const {
    if (m_gestureSession.active)
        return clampUnit(m_gestureSession.openness);

    switch (m_state.phase) {
        case Phase::Opening:
            return std::clamp(m_state.animationFromVisual + (m_state.animationToVisual - m_state.animationFromVisual) * easeOutCubic(m_state.animationProgress), 0.0, 1.0);
        case Phase::ClosingSettle:
            return clampUnit(m_state.animationFromVisual);
        case Phase::Closing:
            return std::clamp(m_state.animationFromVisual + (m_state.animationToVisual - m_state.animationFromVisual) * easeInCubic(m_state.animationProgress), 0.0, 1.0);
        case Phase::Active:
            return 1.0;
        case Phase::Inactive:
            return 0.0;
    }

    return 0.0;
}

double OverviewController::relayoutVisualProgress() const {
    if (!m_state.relayoutActive)
        return 1.0;

    return easeOutCubic(std::clamp(m_state.relayoutProgress, 0.0, 1.0));
}

PHLWINDOW OverviewController::resolveExitFocus(CloseMode mode) const {
    if (mode == CloseMode::Abort)
        return {};

    if (mode == CloseMode::ActivateSelection) {
        const auto selected = selectedWindow();
        if (selected)
            return selected;

        if (m_state.focusDuringOverview && hasManagedWindow(m_state.focusDuringOverview))
            return m_state.focusDuringOverview;
    }

    if (focusFollowsMouseEnabled()) {
        if (m_state.focusDuringOverview && hasManagedWindow(m_state.focusDuringOverview))
            return m_state.focusDuringOverview;

        const auto selected = selectedWindow();
        if (selected)
            return selected;
    }

    return m_state.focusBeforeOpen;
}

bool OverviewController::exitFocusChangedWorkspace(const PHLWINDOW& window) const {
    if (!window || !window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
        return false;

    if (!m_state.focusBeforeOpen || !m_state.focusBeforeOpen->m_workspace || m_state.focusBeforeOpen->m_workspace->m_isSpecialWorkspace)
        return false;

    return window->m_workspace != m_state.focusBeforeOpen->m_workspace;
}

bool OverviewController::shouldPreferGoalExitGeometry(const PHLWINDOW& window) const {
    return window && window->m_workspace && (isScrollingWorkspace(window->m_workspace) || exitFocusChangedWorkspace(window));
}

std::optional<Vector2D> OverviewController::visiblePointForWindowOnMonitor(const PHLWINDOW& window, const PHLMONITOR& monitor, bool preferGoal) const {
    if (!window || !monitor || !window->m_isMapped)
        return std::nullopt;

    if (preferGoal) {
        const auto goalPoint = visiblePointForRectOnMonitor(goalGlobalRectForWindow(window), monitor);
        if (goalPoint)
            return goalPoint;
    }

    return visiblePointForRectOnMonitor(liveGlobalRectForWindow(window), monitor);
}

bool OverviewController::shouldClearWorkspaceFullscreenForExitTarget(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped)
        return false;

    const auto* backup = fullscreenBackupForWorkspace(window->m_workspace);
    if (!backup || !backup->workspace || !backup->hadFullscreenWindow)
        return false;

    if (window->m_workspace != backup->workspace || window->m_fullscreenState.internal != FSMODE_NONE)
        return false;

    PHLWINDOW fullscreenWindow;
    for (const auto& candidate : g_pCompositor->m_windows) {
        if (!candidate || !candidate->m_isMapped || candidate->m_workspace != backup->workspace)
            continue;

        if (candidate->m_fullscreenState.internal != FSMODE_NONE) {
            fullscreenWindow = candidate;
            break;
        }
    }

    return fullscreenWindow && fullscreenWindow != window;
}

bool OverviewController::clearWorkspaceFullscreenForExitTarget(const PHLWINDOW& window) {
    if (!shouldClearWorkspaceFullscreenForExitTarget(window))
        return false;

    auto backupIt = std::find_if(m_state.fullscreenBackups.begin(), m_state.fullscreenBackups.end(),
                                 [&](const FullscreenWorkspaceBackup& backup) { return backup.workspace == window->m_workspace; });
    if (backupIt == m_state.fullscreenBackups.end() || !backupIt->workspace)
        return false;

    PHLWINDOW fullscreenWindow;
    for (const auto& candidate : g_pCompositor->m_windows) {
        if (!candidate || !candidate->m_isMapped || candidate->m_workspace != backupIt->workspace)
            continue;

        if (candidate->m_fullscreenState.internal != FSMODE_NONE) {
            fullscreenWindow = candidate;
            break;
        }
    }

    if (!fullscreenWindow || fullscreenWindow == window)
        return false;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] clear workspace fullscreen source=" << debugWindowLabel(fullscreenWindow) << " target=" << debugWindowLabel(window);
        debugLog(out.str());
    }

    g_pCompositor->setWindowFullscreenInternal(fullscreenWindow, FSMODE_NONE);

    if (backupIt->workspace) {
        backupIt->workspace->m_hasFullscreenWindow = false;
        backupIt->workspace->m_fullscreenMode = FSMODE_NONE;
    }
    if (const auto workspaceMonitor = backupIt->workspace->m_monitor.lock())
        workspaceMonitor->m_solitaryClient.reset();

    backupIt->hadFullscreenWindow = false;
    backupIt->fullscreenMode = FSMODE_NONE;

    return true;
}

void OverviewController::commitOverviewExitFocus(const PHLWINDOW& window) {
    if (!window || !window->m_isMapped)
        return;

    const bool alreadyFocused = Desktop::focusState()->window() == window;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] commit exit focus target=" << debugWindowLabel(window);
        const auto activeBefore = Desktop::focusState()->window();
        if (activeBefore)
            out << " activeBefore=" << debugWindowLabel(activeBefore);
        else
            out << " activeBefore=<null>";
        out << " alreadyFocused=" << (alreadyFocused ? 1 : 0);
        debugLog(out.str());
    }

    if (!alreadyFocused)
        focusWindowCompat(window, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

    (void)syncScrollingWorkspaceSpotOnWindow(window);

    if (m_animationsEnabledOverridden && g_pAnimationManager) {
        // Live focus can switch the real workspace before close starts. Even when the
        // target is already active, force one animation tick so Hyprland flushes the
        // workspace scene that overview was previously masking.
        g_pAnimationManager->frameTick();
        if (debugLogsEnabled())
            debugLog("[hymission] commit exit focus forced animation frameTick");
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] commit exit focus result=";
        const auto activeAfter = Desktop::focusState()->window();
        if (activeAfter)
            out << debugWindowLabel(activeAfter);
        else
            out << "<null>";
        debugLog(out.str());
    }
}

bool OverviewController::syncScrollingWorkspaceSpotOnWindow(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped || !window->m_workspace || !isScrollingWorkspace(window->m_workspace))
        return false;

    const auto target = window->layoutTarget();
    if (!target || target->floating())
        return false;

    auto* scrolling = scrollingAlgorithmForWorkspace(window->m_workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller)
        return false;

    const auto targetData = scrolling->dataFor(target);
    if (!targetData)
        return false;

    const auto column = targetData->column.lock();
    if (!column)
        return false;

    const auto controller = scrolling->m_scrollingData->controller.get();
    const auto columnIndex = scrolling->m_scrollingData->idx(column);
    const auto offsetBefore = controller->getOffset();

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync scrolling workspace spot target=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(window->m_workspace)
            << " live=" << rectToString(liveGlobalRectForWindow(window))
            << " goal=" << rectToString(goalGlobalRectForWindow(window))
            << " col=" << columnIndex
            << " offsetBefore=" << offsetBefore;
        debugLog(out.str());
    }

    column->lastFocusedTarget = targetData;
    scrolling->m_scrollingData->centerOrFitCol(column);
    scrolling->m_scrollingData->recalculate(true);

    if (const auto monitor = window->m_workspace->m_monitor.lock())
        g_layoutManager->recalculateMonitor(monitor);

    if (g_pAnimationManager)
        g_pAnimationManager->frameTick();

    const auto offsetAfter = controller->getOffset();

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync scrolling workspace spot result=" << debugWindowLabel(window)
            << " workspace=" << debugWorkspaceLabel(window->m_workspace)
            << " live=" << rectToString(liveGlobalRectForWindow(window))
            << " goal=" << rectToString(goalGlobalRectForWindow(window))
            << " col=" << columnIndex
            << " offsetAfter=" << offsetAfter;
        debugLog(out.str());
    }

    return true;
}

void OverviewController::refreshExitLayoutForFocus(const PHLWINDOW& window) const {
    if (!window || !window->m_isMapped)
        return;

    std::vector<PHLMONITOR> monitors;
    const auto addMonitor = [&](const PHLMONITOR& monitor) {
        if (!monitor)
            return;
        if (std::ranges::find(monitors, monitor) == monitors.end())
            monitors.push_back(monitor);
    };

    for (const auto& monitor : m_state.participatingMonitors)
        addMonitor(monitor);
    addMonitor(window->m_monitor.lock());
    addMonitor(previewMonitorForWindow(window));

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] refresh exit layout target=" << debugWindowLabel(window);
        if (monitors.empty()) {
            out << " monitors=<none>";
        } else {
            out << " monitors=";
            for (std::size_t i = 0; i < monitors.size(); ++i) {
                out << (i == 0 ? "" : ",") << monitors[i]->m_name;
            }
        }
        debugLog(out.str());
    }

    (void)syncScrollingWorkspaceSpotOnWindow(window);

    for (const auto& monitor : monitors)
        g_layoutManager->recalculateMonitor(monitor);
}

void OverviewController::syncRealFocusDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot) {
    if (!shouldSyncRealFocusDuringOverview() || !window || !window->m_isMapped || !hasManagedWindow(window))
        return;

    if (Desktop::focusState()->window() == window) {
        if (syncScrollingSpot)
            (void)syncScrollingWorkspaceSpotOnWindow(window);
        return;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync real focus during overview target=" << debugWindowLabel(window);
        const auto activeBefore = Desktop::focusState()->window();
        if (activeBefore)
            out << " activeBefore=" << debugWindowLabel(activeBefore);
        else
            out << " activeBefore=<null>";
        if (window->m_workspace)
            out << " targetWorkspace=" << debugWorkspaceLabel(window->m_workspace);
        if (activeBefore && activeBefore->m_workspace)
            out << " activeWorkspaceBefore=" << debugWorkspaceLabel(activeBefore->m_workspace);
        debugLog(out.str());
    }

    const bool temporarilyDisabledAnimations = !m_animationsEnabledOverridden;
    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(true);

    m_pendingLiveFocusWorkspaceChangeTarget = window;
    focusWindowCompat(window, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    if (syncScrollingSpot)
        (void)syncScrollingWorkspaceSpotOnWindow(window);
    if (g_pAnimationManager)
        g_pAnimationManager->frameTick();
    if (m_pendingLiveFocusWorkspaceChangeTarget.lock() == window)
        m_pendingLiveFocusWorkspaceChangeTarget.reset();

    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(false);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] sync real focus result=";
        const auto activeAfter = Desktop::focusState()->window();
        if (activeAfter)
            out << debugWindowLabel(activeAfter);
        else
            out << "<null>";
        if (activeAfter && activeAfter->m_workspace)
            out << " workspace=" << debugWorkspaceLabel(activeAfter->m_workspace);
        if (const auto monitor = Desktop::focusState()->monitor(); monitor)
            out << " activeWorkspaceOnFocusMonitor=" << debugWorkspaceLabel(monitor->m_activeWorkspace);
        debugLog(out.str());
    }
}

void OverviewController::syncFocusDuringOverviewFromSelection(bool syncScrollingSpot) {
    const auto selected = selectedWindow();
    if (!selected)
        return;

    if (m_state.focusDuringOverview != selected && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview target " << debugWindowLabel(selected);
        debugLog(out.str());
    }

    m_state.focusDuringOverview = selected;
    syncRealFocusDuringOverview(selected, syncScrollingSpot);
}

bool OverviewController::matchesPendingLiveFocusWorkspaceChange(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    const auto pendingTarget = m_pendingLiveFocusWorkspaceChangeTarget.lock();
    return pendingTarget && pendingTarget->m_workspace && pendingTarget->m_workspace == workspace;
}

void OverviewController::clearPostCloseForcedFocus() {
    const auto forcedTarget = m_postCloseForcedFocus.lock();
    if (forcedTarget && g_pInputManager->m_forcedFocus.lock() == forcedTarget)
        g_pInputManager->m_forcedFocus.reset();

    m_postCloseForcedFocus.reset();
    m_postCloseForcedFocusLatched = false;
    m_ignorePostCloseMouseMoveCount = 0;
}

void OverviewController::clearPostCloseDispatcher() {
    m_postCloseDispatcher = PostCloseDispatcher::None;
    m_postCloseDispatcherArgs.clear();
}

void OverviewController::queuePostCloseDispatcher(PostCloseDispatcher dispatcher, std::string args) {
    m_postCloseDispatcher = dispatcher;
    m_postCloseDispatcherArgs = std::move(args);
}

SDispatchResult OverviewController::runHookedDispatcher(PostCloseDispatcher dispatcher, std::string args) {
    DispatcherFn original = nullptr;
    const char*  label = nullptr;
    switch (dispatcher) {
        case PostCloseDispatcher::Fullscreen:
            original = m_fullscreenActiveOriginal;
            label = "fullscreen";
            break;
        case PostCloseDispatcher::FullscreenState:
            original = m_fullscreenStateActiveOriginal;
            label = "fullscreenstate";
            break;
        case PostCloseDispatcher::None:
            return {};
    }

    if (!original)
        return {.success = false, .error = "fullscreen dispatcher hook unavailable"};

    if (!isVisible())
        return original(std::move(args));

    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return {};

    if (!selectedWindow())
        return {.success = false, .error = "no selected window in overview"};

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] queue post-close dispatcher " << label << " args=" << args;
        debugLog(out.str());
    }

    queuePostCloseDispatcher(dispatcher, std::move(args));
    beginClose(CloseMode::ActivateSelection);
    return {};
}

void OverviewController::setFullscreenRenderOverride(bool suppress) {
    if (suppress) {
        if (!m_state.fullscreenOverrideActive)
            m_state.fullscreenOverrideActive = true;

        for (const auto& backup : m_state.fullscreenBackups) {
            if (!backup.workspace)
                continue;

            backup.workspace->m_hasFullscreenWindow = false;
            backup.workspace->m_fullscreenMode = FSMODE_NONE;
            if (const auto workspaceMonitor = backup.workspace->m_monitor.lock())
                workspaceMonitor->m_solitaryClient.reset();
        }

        return;
    }

    if (!m_state.fullscreenOverrideActive)
        return;

    for (const auto& backup : m_state.fullscreenBackups) {
        if (!backup.workspace)
            continue;

        backup.workspace->m_hasFullscreenWindow = backup.hadFullscreenWindow;
        backup.workspace->m_fullscreenMode = backup.fullscreenMode;
        if (const auto workspaceMonitor = backup.workspace->m_monitor.lock())
            workspaceMonitor->m_solitaryClient.reset();
    }

    m_state.fullscreenOverrideActive = false;
}

bool OverviewController::transformBoxForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, CBox& box, bool scaled) const {
    const auto transform = windowTransformFor(window, monitor);
    if (!transform)
        return false;

    const double monitorScale = scaled ? monitor->m_scale : 1.0;
    const Rect actual = scaled ? makeRect((transform->actualGlobal.x - monitor->m_position.x) * monitorScale, (transform->actualGlobal.y - monitor->m_position.y) * monitorScale,
                                          transform->actualGlobal.width * monitorScale, transform->actualGlobal.height * monitorScale)
                               : transform->actualGlobal;
    const Rect target = scaled ? makeRect((transform->targetGlobal.x - monitor->m_position.x) * monitorScale, (transform->targetGlobal.y - monitor->m_position.y) * monitorScale,
                                          transform->targetGlobal.width * monitorScale, transform->targetGlobal.height * monitorScale)
                               : transform->targetGlobal;

    box.x = target.x + (box.x - actual.x) * transform->scaleX;
    box.y = target.y + (box.y - actual.y) * transform->scaleY;
    box.width = std::max(1.0, box.width * transform->scaleX);
    box.height = std::max(1.0, box.height * transform->scaleY);
    return true;
}

CRegion OverviewController::transformRegionForWindow(const PHLWINDOW& window, const PHLMONITOR& monitor, const CRegion& region, bool scaled) const {
    CRegion transformed;
    bool    changed = false;

    region.forEachRect([&](const pixman_box32_t& rect) {
        CBox box{
            static_cast<double>(rect.x1),
            static_cast<double>(rect.y1),
            static_cast<double>(rect.x2 - rect.x1),
            static_cast<double>(rect.y2 - rect.y1),
        };

        if (transformBoxForWindow(window, monitor, box, scaled))
            changed = true;

        transformed.add(box);
    });

    return changed ? transformed : region.copy();
}

void OverviewController::beginOpen(const PHLMONITOR& monitor, ScopeOverride requestedScope) {
    setAnimationsEnabledOverride(false);

    const auto buildStart = std::chrono::steady_clock::now();
    const double fromVisual = isVisible() ? visualProgress() : 0.0;
    clearOverviewWorkspaceTransition();
    m_workspaceSwipeGesture = {};
    State next = buildState(monitor, requestedScope);
    if (next.windows.empty()) {
        notify(collectionSummary(monitor), CHyprColor(1.0, 0.7, 0.2, 1.0), 5000);
        return;
    }

    if (!activateHooks())
        return;

    clearPostCloseForcedFocus();
    clearPostCloseDispatcher();
    m_pendingLiveFocusWorkspaceChangeTarget.reset();
    next.phase = Phase::Opening;
    next.animationProgress = 0.0;
    next.animationFromVisual = fromVisual;
    next.animationToVisual = 1.0;
    next.animationStart = {};
    m_deactivatePending = false;
    m_state = std::move(next);
    applyWorkspaceNameOverrides(m_state);
    setInputFollowMouseOverride(true);
    setScrollingFollowFocusOverride(true);
    setFullscreenRenderOverride(true);
    g_pHyprRenderer->m_directScanoutBlocked = true;
    m_postOpenRefreshFrames = 3;
    if (!m_suppressInitialHoverUpdate)
        updateHoveredFromPointer();

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] beginOpen monitor=" << m_state.ownerMonitor->m_name << " windows=" << m_state.windows.size() << " fromVisual=" << fromVisual
            << " buildMs=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count()
            << " scope=";
        switch (m_state.collectionPolicy.requestedScope) {
            case ScopeOverride::Default:
                out << "default";
                break;
            case ScopeOverride::OnlyCurrentWorkspace:
                out << "onlycurrentworkspace";
                break;
            case ScopeOverride::ForceAll:
                out << "forceall";
                break;
        }
        out << " monitors=" << m_state.participatingMonitors.size() << " workspaces=" << m_state.managedWorkspaces.size() << " fullscreenBackups="
            << m_state.fullscreenBackups.size();
        debugLog(out.str());
    }

    refreshOwnedMonitors();
}

void OverviewController::beginClose(CloseMode mode, std::optional<double> fromVisualOverride, bool deferFullscreenMutations) {
    if (!isVisible())
        return;

    if (mode != CloseMode::Abort && (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle))
        return;

    if (mode == CloseMode::Abort && m_state.phase == Phase::Closing)
        return;

    if (mode == CloseMode::Abort)
        clearPostCloseDispatcher();

    if (m_state.phase == Phase::Active && m_state.relayoutActive) {
        for (auto& managed : m_state.windows) {
            managed.targetGlobal = currentPreviewRect(managed);
            managed.relayoutFromGlobal = managed.targetGlobal;
        }
        m_state.relayoutActive = false;
        m_state.relayoutProgress = 1.0;
        m_state.relayoutStart = {};
    }

    const double fromVisual = fromVisualOverride.value_or(visualProgress());
    m_state.pendingExitFocus = resolveExitFocus(mode);
    m_state.closeMode = mode;
    m_state.settleStableFrames = 0;
    m_state.settleHasSample = false;
    m_state.settleStart = {};
    m_state.exitFullscreenReapplied = false;
    m_state.deferredFullscreenWorkspaceClear = false;
    m_state.deferredHiddenFullscreenReapply = false;
    m_deactivatePending = false;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] beginClose monitor=" << (m_state.ownerMonitor ? m_state.ownerMonitor->m_name : "?") << " fromVisual=" << fromVisual << " mode=";
        switch (mode) {
            case CloseMode::Normal:
                out << "normal";
                break;
            case CloseMode::ActivateSelection:
                out << "activate";
                break;
            case CloseMode::Abort:
                out << "abort";
                break;
        }
        if (m_state.pendingExitFocus)
            out << " pendingExitFocus=" << debugWindowLabel(m_state.pendingExitFocus);
        else
            out << " pendingExitFocus=<null>";
        if (m_state.focusDuringOverview)
            out << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview);
        else
            out << " focusDuringOverview=<null>";
        if (m_state.focusBeforeOpen)
            out << " focusBeforeOpen=" << debugWindowLabel(m_state.focusBeforeOpen);
        else
            out << " focusBeforeOpen=<null>";
        if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
            out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        else
            out << " hovered=<null>";
        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
            out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        else
            out << " selected=<null>";
        const auto activeWindow = Desktop::focusState()->window();
        if (activeWindow)
            out << " activeBeforeClose=" << debugWindowLabel(activeWindow);
        else
            out << " activeBeforeClose=<null>";
        if (m_state.pendingExitFocus && m_state.pendingExitFocus->m_workspace)
            out << " pendingExitWorkspace=" << debugWorkspaceLabel(m_state.pendingExitFocus->m_workspace);
        else
            out << " pendingExitWorkspace=<null>";
        if (m_state.focusBeforeOpen && m_state.focusBeforeOpen->m_workspace)
            out << " focusBeforeOpenWorkspace=" << debugWorkspaceLabel(m_state.focusBeforeOpen->m_workspace);
        else
            out << " focusBeforeOpenWorkspace=<null>";
        out << " ownerWorkspace=" << debugWorkspaceLabel(m_state.ownerWorkspace);
        debugLog(out.str());
    }

    if (mode != CloseMode::Abort)
        setAnimationsEnabledOverride(true);
    else
        setAnimationsEnabledOverride(false);

    const bool needsDeferredFullscreenClear =
        mode != CloseMode::Abort && deferFullscreenMutations && shouldClearWorkspaceFullscreenForExitTarget(m_state.pendingExitFocus);
    const bool clearedFullscreen =
        mode != CloseMode::Abort && !deferFullscreenMutations && clearWorkspaceFullscreenForExitTarget(m_state.pendingExitFocus);
    const auto* pendingFullscreenBackup = fullscreenBackupForWindow(m_state.pendingExitFocus);
    const bool shouldReapplyOriginalFullscreen = mode != CloseMode::Abort && m_state.pendingExitFocus && pendingFullscreenBackup &&
        m_state.pendingExitFocus == pendingFullscreenBackup->originalFullscreenWindow && pendingFullscreenBackup->originalFullscreenMode != FSMODE_NONE;
    const bool needsDeferredFullscreenReapply = shouldReapplyOriginalFullscreen && deferFullscreenMutations;
    if (needsDeferredFullscreenClear)
        m_state.deferredFullscreenWorkspaceClear = true;
    if (needsDeferredFullscreenReapply)
        m_state.deferredHiddenFullscreenReapply = true;
    if (shouldReapplyOriginalFullscreen && !deferFullscreenMutations) {
        commitOverviewExitFocus(m_state.pendingExitFocus);
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] beginClose hidden fullscreen reapply " << debugWindowLabel(m_state.pendingExitFocus) << " mode="
                << static_cast<int>(pendingFullscreenBackup->originalFullscreenMode);
            debugLog(out.str());
        }
        if (m_state.pendingExitFocus->m_fullscreenState.internal != FSMODE_NONE)
            g_pCompositor->setWindowFullscreenInternal(m_state.pendingExitFocus, FSMODE_NONE);
        g_pCompositor->setWindowFullscreenInternal(m_state.pendingExitFocus, pendingFullscreenBackup->originalFullscreenMode);
        m_state.exitFullscreenReapplied = true;
    } else if ((needsDeferredFullscreenClear || needsDeferredFullscreenReapply) && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] beginClose defer fullscreen mutation clear=" << (needsDeferredFullscreenClear ? 1 : 0)
            << " reapply=" << (needsDeferredFullscreenReapply ? 1 : 0);
        debugLog(out.str());
    }
    const bool preferGoalGeometry = shouldPreferGoalExitGeometry(m_state.pendingExitFocus);
    const bool shouldSettle = mode != CloseMode::Abort && m_state.pendingExitFocus &&
        (preferGoalGeometry || clearedFullscreen || m_state.exitFullscreenReapplied || m_state.deferredFullscreenWorkspaceClear ||
         m_state.deferredHiddenFullscreenReapply);
    if (debugLogsEnabled() && m_state.pendingExitFocus) {
        std::ostringstream out;
        out << "[hymission] beginClose geometry preferGoal=" << (preferGoalGeometry ? 1 : 0) << " shouldSettle=" << (shouldSettle ? 1 : 0)
            << " exitFocusChangedWorkspace=" << (exitFocusChangedWorkspace(m_state.pendingExitFocus) ? 1 : 0);
        if (const auto exitMonitor = m_state.pendingExitFocus->m_monitor.lock(); exitMonitor) {
            out << " exitMonitor=" << exitMonitor->m_name;
            out << " activeWorkspaceOnExitMonitor=" << debugWorkspaceLabel(exitMonitor->m_activeWorkspace);
        } else {
            out << " exitMonitor=<null>";
        }
        out << " live=" << rectToString(liveGlobalRectForWindow(m_state.pendingExitFocus));
        out << " goal=" << rectToString(goalGlobalRectForWindow(m_state.pendingExitFocus));
        if (const auto* pendingManaged = managedWindowFor(m_state.pendingExitFocus); pendingManaged)
            out << " preview=" << rectToString(currentPreviewRect(*pendingManaged));
        debugLog(out.str());
    }
    if (shouldSettle) {
        setScrollingFollowFocusOverride(false);
        if (!m_state.exitFullscreenReapplied)
            commitOverviewExitFocus(m_state.pendingExitFocus);
        if (preferGoalGeometry)
            refreshExitLayoutForFocus(m_state.pendingExitFocus);
        for (auto& managed : m_state.windows) {
            if (!managed.window || !managed.window->m_isMapped)
                continue;

            managed.exitGlobal = preferGoalGeometry ? goalGlobalRectForWindow(managed.window) : liveGlobalRectForWindow(managed.window);
        }
        if (debugLogsEnabled() && m_state.pendingExitFocus) {
            if (const auto* pendingManaged = managedWindowFor(m_state.pendingExitFocus)) {
                std::ostringstream out;
                out << "[hymission] beginClose settle target=" << debugWindowLabel(m_state.pendingExitFocus)
                    << " exitGlobal=" << rectToString(pendingManaged->exitGlobal)
                    << " preview=" << rectToString(currentPreviewRect(*pendingManaged));
                debugLog(out.str());
            }
        }
        m_state.phase = Phase::ClosingSettle;
        m_state.animationProgress = 0.0;
        m_state.animationFromVisual = fromVisual;
        m_state.animationToVisual = 0.0;
        m_state.animationStart = {};
        if (debugLogsEnabled())
            debugLog("[hymission] beginClose settle start");
    } else {
        if (mode != CloseMode::Abort)
            commitOverviewExitFocus(m_state.pendingExitFocus);
        m_state.phase = Phase::Closing;
        m_state.animationProgress = 0.0;
        m_state.animationFromVisual = fromVisual;
        m_state.animationToVisual = 0.0;
        m_state.animationStart = {};
        for (auto& managed : m_state.windows)
            managed.exitGlobal = liveGlobalRectForWindow(managed.window);
        if (debugLogsEnabled() && m_state.pendingExitFocus) {
            if (const auto* pendingManaged = managedWindowFor(m_state.pendingExitFocus)) {
                std::ostringstream out;
                out << "[hymission] beginClose immediate target=" << debugWindowLabel(m_state.pendingExitFocus)
                    << " exitGlobal=" << rectToString(pendingManaged->exitGlobal)
                    << " preview=" << rectToString(currentPreviewRect(*pendingManaged));
                debugLog(out.str());
            }
        }
    }

    refreshOwnedMonitors();
}

void OverviewController::deactivate() {
    const auto monitor = m_state.ownerMonitor;
    const auto ownedMonitors = m_state.participatingMonitors;
    const auto fullscreenActiveOriginal = m_fullscreenActiveOriginal;
    const auto fullscreenStateActiveOriginal = m_fullscreenStateActiveOriginal;
    const auto* desiredFullscreenBackup = fullscreenBackupForWindow(m_state.pendingExitFocus);
    const auto originalFullscreenWindow = desiredFullscreenBackup ? desiredFullscreenBackup->originalFullscreenWindow : PHLWINDOW{};
    const auto originalFullscreenMode = desiredFullscreenBackup ? desiredFullscreenBackup->originalFullscreenMode : FSMODE_NONE;
    const auto desiredFocus = m_state.closeMode != CloseMode::Abort && m_state.pendingExitFocus && m_state.pendingExitFocus->m_isMapped ? m_state.pendingExitFocus : PHLWINDOW{};
    const auto postCloseDispatcher = desiredFocus ? m_postCloseDispatcher : PostCloseDispatcher::None;
    const auto postCloseDispatcherArgs = desiredFocus ? m_postCloseDispatcherArgs : std::string{};
    const bool shouldDelayRestoreNativeAnimations = m_animationsEnabledOverridden && m_state.closeMode != CloseMode::Abort;
    const bool shouldPreserveExitFocus = desiredFocus && m_inputFollowMouseOverridden && m_inputFollowMouseBackup != 0;
    const bool preferGoalVisiblePoint = shouldPreserveExitFocus && shouldPreferGoalExitGeometry(desiredFocus);
    const auto focusMonitor = desiredFocus ? (previewMonitorForWindow(desiredFocus) ? previewMonitorForWindow(desiredFocus) : desiredFocus->m_monitor.lock()) : PHLMONITOR{};
    const auto visiblePoint = shouldPreserveExitFocus ? visiblePointForWindowOnMonitor(desiredFocus, focusMonitor, preferGoalVisiblePoint) : std::nullopt;
    const bool shouldWarpCursorForExitFocus = visiblePoint && desiredFocus != m_state.focusBeforeOpen;
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] deactivate monitor=" << (monitor ? monitor->m_name : "?");
        if (m_state.pendingExitFocus)
            out << " pendingExitFocus=" << debugWindowLabel(m_state.pendingExitFocus);
        else
            out << " pendingExitFocus=<null>";
        out << " closeMode=";
        switch (m_state.closeMode) {
            case CloseMode::Normal:
                out << "normal";
                break;
            case CloseMode::ActivateSelection:
                out << "activate";
                break;
            case CloseMode::Abort:
                out << "abort";
                break;
        }
        if (desiredFocus)
            out << " desiredFocus=" << debugWindowLabel(desiredFocus);
        else
            out << " desiredFocus=<null>";
        if (desiredFocus && desiredFocus->m_workspace)
            out << " desiredWorkspace=" << debugWorkspaceLabel(desiredFocus->m_workspace);
        else
            out << " desiredWorkspace=<null>";
        if (originalFullscreenWindow)
            out << " originalFullscreen=" << debugWindowLabel(originalFullscreenWindow) << " mode=" << static_cast<int>(originalFullscreenMode);
        else
            out << " originalFullscreen=<null>";
        out << " exitFullscreenReapplied=" << (m_state.exitFullscreenReapplied ? 1 : 0);
        out << " shouldPreserveFocus=" << (shouldPreserveExitFocus ? 1 : 0);
        out << " preferGoalVisiblePoint=" << (preferGoalVisiblePoint ? 1 : 0);
        out << " shouldWarpCursor=" << (shouldWarpCursorForExitFocus ? 1 : 0);
        if (desiredFocus) {
            out << " desiredLive=" << rectToString(liveGlobalRectForWindow(desiredFocus));
            out << " desiredGoal=" << rectToString(goalGlobalRectForWindow(desiredFocus));
        }
        const auto activeBefore = Desktop::focusState()->window();
        if (activeBefore)
            out << " activeBeforeDeactivate=" << debugWindowLabel(activeBefore);
        else
            out << " activeBeforeDeactivate=<null>";
        debugLog(out.str());
    }

    clearPostCloseForcedFocus();
    m_pendingLiveFocusWorkspaceChangeTarget.reset();
    deactivateHooks();
    setFullscreenRenderOverride(false);
    restoreWorkspaceNameOverrides();
    setScrollingFollowFocusOverride(false);
    g_pHyprRenderer->m_directScanoutBlocked = false;

    if (shouldPreserveExitFocus) {
        g_pInputManager->m_forcedFocus = desiredFocus;
        m_postCloseForcedFocus = desiredFocus;
        m_postCloseForcedFocusLatched = true;
        if (shouldWarpCursorForExitFocus) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] warp cursor to visible exit focus " << debugWindowLabel(desiredFocus) << " point=" << vectorToString(*visiblePoint);
                debugLog(out.str());
            }
            g_pCompositor->warpCursorTo(*visiblePoint);
        }
    }

    if (!shouldPreserveExitFocus) {
        setInputFollowMouseOverride(false);
        m_restoreInputFollowMouseAfterPostClose = false;
    } else {
        m_restoreInputFollowMouseAfterPostClose = true;
    }
    if (desiredFocus && Desktop::focusState()->window() != desiredFocus)
        focusWindowCompat(desiredFocus);
    if (!m_state.exitFullscreenReapplied && desiredFocus && desiredFocus == originalFullscreenWindow && originalFullscreenMode != FSMODE_NONE && desiredFocus->m_isMapped) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] fullscreen restore check " << debugWindowLabel(desiredFocus) << " mode=" << static_cast<int>(originalFullscreenMode)
                << " internal=" << static_cast<int>(desiredFocus->m_fullscreenState.internal) << " needsRestore=1";
            debugLog(out.str());
        }
        if (desiredFocus->m_fullscreenState.internal != FSMODE_NONE)
            g_pCompositor->setWindowFullscreenInternal(desiredFocus, FSMODE_NONE);
        g_pCompositor->setWindowFullscreenInternal(desiredFocus, originalFullscreenMode);
    }
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] deactivate result active=";
        const auto activeAfter = Desktop::focusState()->window();
        if (activeAfter)
            out << debugWindowLabel(activeAfter);
        else
            out << "<null>";
        switch (postCloseDispatcher) {
            case PostCloseDispatcher::None:
                out << " postCloseDispatcher=<none>";
                break;
            case PostCloseDispatcher::Fullscreen:
                out << " postCloseDispatcher=fullscreen";
                break;
            case PostCloseDispatcher::FullscreenState:
                out << " postCloseDispatcher=fullscreenstate";
                break;
        }
        debugLog(out.str());
    }
    clearPostCloseDispatcher();
    m_deactivatePending = false;
    m_postOpenRefreshFrames = 0;
    clearOverviewWorkspaceTransition();
    m_workspaceSwipeGesture = {};
    m_state = {};
    for (const auto& ownedMonitor : ownedMonitors)
        refreshScene(ownedMonitor);
    refreshScene(monitor);

    switch (postCloseDispatcher) {
        case PostCloseDispatcher::Fullscreen:
            if (fullscreenActiveOriginal)
                fullscreenActiveOriginal(postCloseDispatcherArgs);
            break;
        case PostCloseDispatcher::FullscreenState:
            if (fullscreenStateActiveOriginal)
                fullscreenStateActiveOriginal(postCloseDispatcherArgs);
            break;
        case PostCloseDispatcher::None:
            break;
    }

    if (shouldDelayRestoreNativeAnimations)
        setAnimationsEnabledOverride(true, NATIVE_ANIMATION_DISABLE_DURATION);
    else
        setAnimationsEnabledOverride(false);
}

void OverviewController::refreshScene(const PHLMONITOR& monitor) const {
    if (!monitor)
        return;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] refreshScene monitor=" << monitor->m_name;
        debugLog(out.str());
    }

    g_pHyprRenderer->damageMonitor(monitor);
    g_pCompositor->scheduleFrameForMonitor(monitor);
}

void OverviewController::refreshOwnedMonitors() const {
    for (const auto& monitor : ownedMonitors())
        refreshScene(monitor);
}

void OverviewController::damageOwnedMonitor() const {
    refreshOwnedMonitors();
}

void OverviewController::updateAnimation() {
    if (m_state.phase == Phase::ClosingSettle) {
        const auto now = std::chrono::steady_clock::now();
        if (m_state.settleStart == std::chrono::steady_clock::time_point{}) {
            m_state.settleStart = now;
            if (debugLogsEnabled())
                debugLog("[hymission] close settle start");
        }

        if (m_state.deferredFullscreenWorkspaceClear || m_state.deferredHiddenFullscreenReapply) {
            bool appliedDeferredFullscreenMutation = false;

            if (m_state.deferredFullscreenWorkspaceClear) {
                m_state.deferredFullscreenWorkspaceClear = false;
                if (clearWorkspaceFullscreenForExitTarget(m_state.pendingExitFocus)) {
                    appliedDeferredFullscreenMutation = true;
                    if (debugLogsEnabled())
                        debugLog("[hymission] close settle applied deferred fullscreen clear");
                }
            }

            if (m_state.deferredHiddenFullscreenReapply) {
                m_state.deferredHiddenFullscreenReapply = false;
                const auto* pendingFullscreenBackup = fullscreenBackupForWindow(m_state.pendingExitFocus);
                const bool shouldReapplyOriginalFullscreen = m_state.pendingExitFocus && m_state.pendingExitFocus->m_isMapped && pendingFullscreenBackup &&
                    m_state.pendingExitFocus == pendingFullscreenBackup->originalFullscreenWindow && pendingFullscreenBackup->originalFullscreenMode != FSMODE_NONE;
                if (shouldReapplyOriginalFullscreen) {
                    if (debugLogsEnabled()) {
                        std::ostringstream out;
                        out << "[hymission] close settle apply deferred fullscreen reapply " << debugWindowLabel(m_state.pendingExitFocus) << " mode="
                            << static_cast<int>(pendingFullscreenBackup->originalFullscreenMode);
                        debugLog(out.str());
                    }
                    if (m_state.pendingExitFocus->m_fullscreenState.internal != FSMODE_NONE)
                        g_pCompositor->setWindowFullscreenInternal(m_state.pendingExitFocus, FSMODE_NONE);
                    g_pCompositor->setWindowFullscreenInternal(m_state.pendingExitFocus, pendingFullscreenBackup->originalFullscreenMode);
                    m_state.exitFullscreenReapplied = true;
                    appliedDeferredFullscreenMutation = true;
                }
            }

            if (appliedDeferredFullscreenMutation) {
                m_state.settleHasSample = false;
                m_state.settleStableFrames = 0;
                refreshOwnedMonitors();
                return;
            }
        }

        const bool preferGoalGeometry = shouldPreferGoalExitGeometry(m_state.pendingExitFocus);
        bool stable = m_state.settleHasSample;
        for (auto& managed : m_state.windows) {
            if (!managed.window || !managed.window->m_isMapped) {
                beginClose(CloseMode::Abort);
                return;
            }

            const Rect sampledGlobal = preferGoalGeometry ? goalGlobalRectForWindow(managed.window) : liveGlobalRectForWindow(managed.window);
            if (m_state.settleHasSample && !rectApproxEqual(managed.exitGlobal, sampledGlobal, CLOSE_SETTLE_EPSILON))
                stable = false;
            managed.exitGlobal = sampledGlobal;
        }

        if (debugLogsEnabled() && m_state.pendingExitFocus) {
            const auto* pendingManaged = managedWindowFor(m_state.pendingExitFocus);
            std::ostringstream out;
            out << "[hymission] close settle sample preferGoal=" << (preferGoalGeometry ? 1 : 0);
            if (pendingManaged) {
                const Rect sampledGlobal = preferGoalGeometry ? goalGlobalRectForWindow(m_state.pendingExitFocus) : liveGlobalRectForWindow(m_state.pendingExitFocus);
                out << " target=" << debugWindowLabel(m_state.pendingExitFocus)
                    << " storedExit=" << rectToString(pendingManaged->exitGlobal)
                    << " sampled=" << rectToString(sampledGlobal)
                    << " preview=" << rectToString(currentPreviewRect(*pendingManaged));
            }
            if (m_state.pendingExitFocus->m_workspace) {
                out << " pendingWorkspace=" << debugWorkspaceLabel(m_state.pendingExitFocus->m_workspace)
                    << " wsRender=" << vectorToString(m_state.pendingExitFocus->m_workspace->m_renderOffset->value())
                    << " wsGoal=" << vectorToString(m_state.pendingExitFocus->m_workspace->m_renderOffset->goal());
            }
            if (const auto monitor = m_state.pendingExitFocus->m_monitor.lock(); monitor)
                out << " activeWorkspaceOnMonitor=" << debugWorkspaceLabel(monitor->m_activeWorkspace);
            debugLog(out.str());
        }

        if (!m_state.settleHasSample) {
            m_state.settleHasSample = true;
            m_state.settleStableFrames = 0;
            return;
        }

        m_state.settleStableFrames = stable ? (m_state.settleStableFrames + 1) : 0;
        const double settleElapsedMs = std::chrono::duration<double, std::milli>(now - m_state.settleStart).count();
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] close settle stable=" << (stable ? 1 : 0) << " frames=" << m_state.settleStableFrames << " elapsedMs=" << settleElapsedMs;
            debugLog(out.str());
        }

        if (m_state.settleStableFrames >= CLOSE_SETTLE_STABLE_FRAMES || settleElapsedMs >= CLOSE_SETTLE_TIMEOUT_MS) {
            m_state.phase = Phase::Closing;
            m_state.animationProgress = 0.0;
            m_state.animationFromVisual = clampUnit(m_state.animationFromVisual);
            m_state.animationToVisual = 0.0;
            m_state.animationStart = {};
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] close settle complete frames=" << m_state.settleStableFrames << " elapsedMs=" << settleElapsedMs;
                debugLog(out.str());
            }
        }
        return;
    }

    if (m_gestureSession.active)
        return;

    if (m_state.phase == Phase::Active && m_state.relayoutActive) {
        const auto now = std::chrono::steady_clock::now();
        if (m_state.relayoutStart == std::chrono::steady_clock::time_point{}) {
            m_state.relayoutStart = now;
            m_state.relayoutProgress = 0.0;
            if (debugLogsEnabled())
                debugLog("[hymission] relayout anim start");
            return;
        }

        const auto elapsed = std::chrono::duration<double, std::milli>(now - m_state.relayoutStart).count();
        m_state.relayoutProgress = clampUnit(elapsed / RELAYOUT_DURATION_MS);
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] relayout anim t=" << m_state.relayoutProgress;
            debugLog(out.str());
        }

        if (m_state.relayoutProgress >= 1.0) {
            m_state.relayoutProgress = 1.0;
            m_state.relayoutActive = false;
            m_state.relayoutStart = {};
            if (debugLogsEnabled())
                debugLog("[hymission] relayout anim complete");
        }
        return;
    }

    if (!isAnimating())
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_state.animationStart == std::chrono::steady_clock::time_point{}) {
        m_state.animationStart = now;
        m_state.animationProgress = 0.0;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] anim start phase=" << (m_state.phase == Phase::Opening ? "opening" : "closing") << " visual=" << visualProgress();
            debugLog(out.str());
        }
        return;
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(now - m_state.animationStart).count();
    const double duration = m_state.phase == Phase::Opening ? OPEN_DURATION_MS : CLOSE_DURATION_MS;

    m_state.animationProgress = clampUnit(elapsed / duration);
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] anim phase=" << (m_state.phase == Phase::Opening ? "opening" : "closing") << " t=" << m_state.animationProgress
            << " visual=" << visualProgress();
        debugLog(out.str());
    }

    if (m_state.animationProgress < 1.0)
        return;

    if (m_state.phase == Phase::Opening) {
        m_state.phase = Phase::Active;
        m_state.animationProgress = 1.0;
        m_state.animationFromVisual = 1.0;
        m_state.animationToVisual = 1.0;
        m_postOpenRefreshFrames = std::max<std::size_t>(m_postOpenRefreshFrames, 3);
        if (debugLogsEnabled())
            debugLog("[hymission] anim opening complete");
        updateHoveredFromPointer();
        refreshOwnedMonitors();
    } else if (m_state.phase == Phase::Closing) {
        m_state.animationProgress = 1.0;
        m_state.animationFromVisual = 0.0;
        m_state.animationToVisual = 0.0;
        if (!m_deactivatePending && debugLogsEnabled())
            debugLog("[hymission] anim closing complete, waiting for post-windows deactivate");
        m_deactivatePending = true;
    }
}

void OverviewController::updateHoveredFromPointer(bool syncSelection, bool syncRealFocus, bool syncScrollingSpot) {
    if (!isVisible())
        return;

    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    const auto previousHovered = m_state.hoveredIndex;
    const auto previousSelected = m_state.selectedIndex;
    const auto previousFocus = m_state.focusDuringOverview;

    m_state.hoveredIndex = hitTestTarget(pointer.x, pointer.y);
    // Passive refreshes (workspace rebuild/commit) should still align overview
    // selection with the stationary pointer, but can skip the real-focus sync
    // that would otherwise mutate scrolling spot state.
    if (syncSelection && m_state.hoveredIndex && focusFollowsMouseEnabled()) {
        m_state.selectedIndex = m_state.hoveredIndex;
        if (syncRealFocus)
            syncFocusDuringOverviewFromSelection(syncScrollingSpot);
        else
            m_state.focusDuringOverview = m_state.windows[*m_state.hoveredIndex].window;
    }

    if (previousHovered != m_state.hoveredIndex || previousSelected != m_state.selectedIndex || previousFocus != m_state.focusDuringOverview) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] hover pointer=" << pointer.x << ',' << pointer.y;
            if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
                out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
            else
                out << " hovered=<null>";
            if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
                out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
            else
                out << " selected=<null>";
            if (m_state.focusDuringOverview)
                out << " focusDuringOverview=" << debugWindowLabel(m_state.focusDuringOverview);
            else
                out << " focusDuringOverview=<null>";
            const auto activeWindow = Desktop::focusState()->window();
            if (activeWindow)
                out << " active=" << debugWindowLabel(activeWindow);
            else
                out << " active=<null>";
            debugLog(out.str());
        }
        damageOwnedMonitor();
    }
}

void OverviewController::rebuildVisibleState() {
    if (!isVisible() || !m_state.ownerMonitor || !m_state.ownerWorkspace)
        return;

    if (m_workspaceTransition.active)
        clearOverviewWorkspaceTransition();

    const auto monitor = m_state.ownerMonitor;
    const auto requestedScope = m_state.collectionPolicy.requestedScope;
    const auto previousPhase = m_state.phase;
    const auto previousAnimationProgress = m_state.animationProgress;
    const auto previousAnimationFromVisual = m_state.animationFromVisual;
    const auto previousAnimationToVisual = m_state.animationToVisual;
    const auto previousAnimationStart = m_state.animationStart;
    const auto previousSettleStableFrames = m_state.settleStableFrames;
    const auto previousSettleHasSample = m_state.settleHasSample;
    const auto previousSettleStart = m_state.settleStart;
    const auto previousRelayoutProgress = m_state.relayoutProgress;
    const bool previousRelayoutActive = m_state.relayoutActive;
    const auto previousRelayoutStart = m_state.relayoutStart;
    const auto previousFocusBeforeOpen = m_state.focusBeforeOpen;
    const auto previousPendingExitFocus = m_state.pendingExitFocus;
    const auto previousCloseMode = m_state.closeMode;
    const bool previousExitFullscreenReapplied = m_state.exitFullscreenReapplied;
    const auto previousFullscreenBackups = m_state.fullscreenBackups;
    const bool previousFullscreenOverrideActive = m_state.fullscreenOverrideActive;
    std::vector<std::pair<PHLWINDOW, Rect>> previousPreviewRects;
    previousPreviewRects.reserve(m_state.windows.size() + m_state.transientClosingWindows.size());
    for (const auto& window : m_state.windows)
        previousPreviewRects.emplace_back(window.window, currentPreviewRect(window));
    for (const auto& window : m_state.transientClosingWindows)
        previousPreviewRects.emplace_back(window.window, currentPreviewRect(window));

    State next = buildState(monitor, requestedScope);
    if (next.windows.empty()) {
        beginClose(CloseMode::Abort);
        return;
    }

    next.phase = previousPhase;
    next.animationProgress = previousAnimationProgress;
    next.animationFromVisual = previousAnimationFromVisual;
    next.animationToVisual = previousAnimationToVisual;
    next.animationStart = previousAnimationStart;
    next.settleStableFrames = previousSettleStableFrames;
    next.settleHasSample = previousSettleHasSample;
    next.settleStart = previousSettleStart;
    next.relayoutProgress = previousRelayoutProgress;
    next.relayoutActive = previousRelayoutActive;
    next.relayoutStart = previousRelayoutStart;
    next.focusBeforeOpen = previousFocusBeforeOpen;
    next.closeMode = previousCloseMode;
    next.exitFullscreenReapplied = previousExitFullscreenReapplied;
    next.fullscreenOverrideActive = previousFullscreenOverrideActive;
    for (auto& backup : next.fullscreenBackups) {
        const auto previousIt = std::find_if(previousFullscreenBackups.begin(), previousFullscreenBackups.end(),
                                             [&](const FullscreenWorkspaceBackup& previous) { return previous.workspace == backup.workspace; });
        if (previousIt == previousFullscreenBackups.end())
            continue;

        backup.hadFullscreenWindow = previousIt->hadFullscreenWindow;
        backup.fullscreenMode = previousIt->fullscreenMode;
        backup.originalFullscreenWindow = previousIt->originalFullscreenWindow;
        backup.originalFullscreenMode = previousIt->originalFullscreenMode;
    }

    auto previousRectForWindow = [&](const PHLWINDOW& window) -> Rect {
        const auto it = std::find_if(previousPreviewRects.begin(), previousPreviewRects.end(), [&](const auto& previous) { return previous.first == window; });
        return it != previousPreviewRects.end() ? it->second : liveGlobalRectForWindow(window);
    };

    const auto previousManagedForWindow = [&](const PHLWINDOW& window) -> const ManagedWindow* {
        const auto it = std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
        return it == m_state.windows.end() ? nullptr : &*it;
    };

    if (previousPendingExitFocus && std::any_of(next.windows.begin(), next.windows.end(), [&](const ManagedWindow& managed) { return managed.window == previousPendingExitFocus; }))
        next.pendingExitFocus = previousPendingExitFocus;
    else
        next.pendingExitFocus = {};

    const bool sameWindowSet = next.windows.size() == m_state.windows.size() &&
        std::ranges::all_of(next.windows, [&](const ManagedWindow& managed) { return managed.window && previousManagedForWindow(managed.window) != nullptr; });
    const bool sameMonitorSet = next.participatingMonitors.size() == m_state.participatingMonitors.size() &&
        std::ranges::all_of(next.participatingMonitors, [&](const PHLMONITOR& monitor) { return containsHandle(m_state.participatingMonitors, monitor); });

    bool shouldAnimateRelayout = false;
    if (sameWindowSet && sameMonitorSet) {
        for (auto& window : next.windows) {
            const auto* previousManaged = previousManagedForWindow(window.window);
            if (!previousManaged)
                continue;

            window.targetMonitor = previousManaged->targetMonitor;
            window.slot = previousManaged->slot;
            window.targetGlobal = previousManaged->targetGlobal;
            window.relayoutFromGlobal = previousManaged->targetGlobal;
            window.exitGlobal = previousManaged->exitGlobal;
        }
    } else if (previousPhase == Phase::Active) {
        for (auto& window : next.windows) {
            const auto it = std::find_if(previousPreviewRects.begin(), previousPreviewRects.end(), [&](const auto& previous) { return previous.first == window.window; });
            if (it != previousPreviewRects.end())
                window.relayoutFromGlobal = it->second;
            else
                window.relayoutFromGlobal = window.naturalGlobal;

            if (!rectApproxEqual(window.relayoutFromGlobal, window.targetGlobal, 0.5))
                shouldAnimateRelayout = true;
        }
    }

    auto appendTransientClosingWindow = [&](const ManagedWindow& source) {
        if (!source.window || !source.window->m_isMapped || !source.window->m_fadingOut)
            return;
        if (std::any_of(next.windows.begin(), next.windows.end(), [&](const ManagedWindow& managed) { return managed.window == source.window; }))
            return;
        if (std::any_of(next.transientClosingWindows.begin(), next.transientClosingWindows.end(), [&](const ManagedWindow& managed) { return managed.window == source.window; }))
            return;

        const Rect previewRect = previousRectForWindow(source.window);
        next.transientClosingWindows.push_back({
            .window = source.window,
            .targetMonitor = source.targetMonitor,
            .title = source.title,
            .naturalGlobal = previewRect,
            .exitGlobal = previewRect,
            .relayoutFromGlobal = previewRect,
            .targetGlobal = previewRect,
            .slot = source.slot,
            .previewAlpha = source.previewAlpha,
            .isFloating = source.isFloating,
            .isPinned = source.isPinned,
        });
    };

    for (const auto& window : m_state.windows)
        appendTransientClosingWindow(window);
    for (const auto& window : m_state.transientClosingWindows)
        appendTransientClosingWindow(window);

    if (sameWindowSet && sameMonitorSet) {
        next.relayoutActive = false;
        next.relayoutProgress = 1.0;
        next.relayoutStart = {};
    } else if (shouldAnimateRelayout) {
        next.relayoutActive = true;
        next.relayoutProgress = 0.0;
        next.relayoutStart = {};
    } else {
        next.relayoutActive = false;
        next.relayoutProgress = 1.0;
        next.relayoutStart = {};
        for (auto& window : next.windows)
            window.relayoutFromGlobal = window.targetGlobal;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] rebuild overview state windows=" << next.windows.size() << " phase=";
        switch (next.phase) {
            case Phase::Inactive:
                out << "inactive";
                break;
            case Phase::Opening:
                out << "opening";
                break;
            case Phase::Active:
                out << "active";
                break;
            case Phase::ClosingSettle:
                out << "closing-settle";
                break;
            case Phase::Closing:
                out << "closing";
                break;
        }
        out << " relayout=" << (shouldAnimateRelayout ? 1 : 0);
        out << " frozenLayout=" << ((sameWindowSet && sameMonitorSet) ? 1 : 0);
        debugLog(out.str());
    }

    m_state = std::move(next);
    applyWorkspaceNameOverrides(m_state);
    updateHoveredFromPointer(focusFollowsMouseEnabled(), false);
    refreshOwnedMonitors();
}

void OverviewController::moveSelection(Direction direction) {
    if (m_state.windows.empty())
        return;

    if (!m_state.selectedIndex) {
        m_state.selectedIndex = 0;
        syncFocusDuringOverviewFromSelection();
        damageOwnedMonitor();
        return;
    }

    if (const auto next = chooseDirectionalNeighbor(targetRects(), *m_state.selectedIndex, direction)) {
        if (*next == *m_state.selectedIndex)
            return;
        m_state.selectedIndex = *next;
        syncFocusDuringOverviewFromSelection();
        damageOwnedMonitor();
    }
}

void OverviewController::activateSelection() {
    if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size())
        return;

    beginClose(CloseMode::ActivateSelection);
}

void OverviewController::notify(const std::string& message, const CHyprColor& color, float durationMs) const {
    HyprlandAPI::addNotification(m_handle, message, color, durationMs);
}

void OverviewController::debugLog(const std::string& message) const {
    if (!debugLogsEnabled())
        return;

    if (Log::logger)
        Log::logger->log(Log::DEBUG, message);

    std::ofstream out("/tmp/hymission-debug.log", std::ios::app);
    if (!out.is_open())
        return;

    out << message << '\n';
}

void OverviewController::debugSurfaceLog(const std::string& message) const {
    if (!debugSurfaceLogsEnabled())
        return;

    if (Log::logger)
        Log::logger->log(Log::DEBUG, message);

    std::ofstream out("/tmp/hymission-debug.log", std::ios::app);
    if (!out.is_open())
        return;

    out << message << '\n';
}

std::string OverviewController::debugWorkspaceLabel(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return "<null-workspace>";

    std::ostringstream out;
    out << workspace->m_id << ':' << workspace->m_name;
    if (workspace->m_isSpecialWorkspace)
        out << " special";
    if (const auto monitor = workspace->m_monitor.lock())
        out << "@" << monitor->m_name;
    else
        out << "@<no-monitor>";
    return out.str();
}

std::string OverviewController::debugWindowLabel(const PHLWINDOW& window) const {
    if (!window)
        return "<null-window>";

    std::ostringstream out;
    out << window->m_class << "::" << window->m_title << '@' << std::hex << reinterpret_cast<uintptr_t>(window.get()) << std::dec
        << (window->m_isX11 ? " x11" : " wayland");
    return out.str();
}

void OverviewController::renderBackdrop() const {
    const double alpha = BACKDROP_ALPHA * visualProgress();
    if (alpha <= 0.0)
        return;

    for (const auto& monitor : ownedMonitors()) {
        if (!monitor)
            continue;

        g_pHyprOpenGL->renderRect(
            CBox{
                monitor->m_position.x,
                monitor->m_position.y,
                monitor->m_size.x,
                monitor->m_size.y,
            },
            CHyprColor(0.05, 0.06, 0.08, alpha),
            {});
    }
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

        const auto titleMonitor = window.targetMonitor ? window.targetMonitor : m_state.ownerMonitor;
        const double titleTop = titleMonitor ? titleMonitor->m_position.y + 8.0 : rect.y;
        const Rect titleRect = makeRect(rect.x, std::max(titleTop, rect.y - texture->m_size.y - TITLE_PADDING), texture->m_size.x, texture->m_size.y);
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

OverviewController::State OverviewController::buildState(const PHLMONITOR& monitor, ScopeOverride requestedScope, const std::vector<WorkspaceOverride>& workspaceOverrides,
                                                         bool keepEmptyParticipatingMonitors) const {
    State state;
    if (!monitor || !monitor->m_activeWorkspace)
        return state;

    const bool preserveExistingOrder =
        workspaceOverrides.empty() && isVisible() && requestedScope == m_state.collectionPolicy.requestedScope && (!m_state.ownerMonitor || monitor == m_state.ownerMonitor);

    state.ownerMonitor = monitor;
    state.ownerWorkspace = monitor->m_activeWorkspace;
    state.collectionPolicy = loadCollectionPolicy(requestedScope);
    state.focusBeforeOpen = Desktop::focusState()->window();
    state.focusDuringOverview = state.focusBeforeOpen;

    const auto addMonitor = [&](const PHLMONITOR& candidate) {
        if (!candidate || containsHandle(state.participatingMonitors, candidate))
            return;
        state.participatingMonitors.push_back(candidate);
    };

    if (state.collectionPolicy.onlyActiveMonitor) {
        addMonitor(monitor);
    } else {
        for (const auto& candidate : g_pCompositor->m_monitors)
            addMonitor(candidate);
    }

    const auto addWorkspace = [&](const PHLWORKSPACE& workspace) {
        if (!workspace || containsHandle(state.managedWorkspaces, workspace))
            return;
        state.managedWorkspaces.push_back(workspace);
    };

    const auto overrideForMonitor = [&](const PHLMONITOR& candidateMonitor) -> const WorkspaceOverride* {
        const auto it = std::find_if(workspaceOverrides.begin(), workspaceOverrides.end(),
                                     [&](const WorkspaceOverride& override) { return candidateMonitor && override.monitorId == candidateMonitor->m_id; });
        return it == workspaceOverrides.end() ? nullptr : &*it;
    };

    if (state.collectionPolicy.onlyActiveWorkspace) {
        for (const auto& candidateMonitor : state.participatingMonitors) {
            if (!candidateMonitor)
                continue;

            if (const auto* override = overrideForMonitor(candidateMonitor); override) {
                if (override->workspace)
                    addWorkspace(override->workspace);
                continue;
            }

            if (candidateMonitor->m_activeWorkspace)
                addWorkspace(candidateMonitor->m_activeWorkspace);
        }
    } else {
        for (const auto& workspace : g_pCompositor->getWorkspacesCopy()) {
            if (!workspace || workspace->m_isSpecialWorkspace)
                continue;

            const auto workspaceMonitor = workspace->m_monitor.lock();
            if (workspaceMonitor && containsHandle(state.participatingMonitors, workspaceMonitor))
                addWorkspace(workspace);
        }
    }

    if (state.collectionPolicy.includeSpecial) {
        for (const auto& candidateMonitor : state.participatingMonitors) {
            if (candidateMonitor && candidateMonitor->m_activeSpecialWorkspace)
                addWorkspace(candidateMonitor->m_activeSpecialWorkspace);
        }
    }

    if (workspaceRowsEnabled(m_handle)) {
        std::stable_sort(state.managedWorkspaces.begin(), state.managedWorkspaces.end(), [](const PHLWORKSPACE& lhs, const PHLWORKSPACE& rhs) {
            if (!lhs || !rhs)
                return static_cast<bool>(lhs);

            if (lhs->m_isSpecialWorkspace != rhs->m_isSpecialWorkspace)
                return !lhs->m_isSpecialWorkspace;

            return lhs->m_id < rhs->m_id;
        });
    }

    for (const auto& workspace : state.managedWorkspaces) {
        if (!workspace)
            continue;

        FullscreenWorkspaceBackup backup;
        backup.workspace = workspace;
        backup.hadFullscreenWindow = workspace->m_hasFullscreenWindow;
        backup.fullscreenMode = workspace->m_hasFullscreenWindow ? workspace->m_fullscreenMode : FSMODE_NONE;
        if (workspace->m_hasFullscreenWindow) {
            backup.originalFullscreenWindow = workspace->getFullscreenWindow();
            backup.originalFullscreenMode = workspace->m_fullscreenMode;
        }

        if (!backup.originalFullscreenWindow || backup.originalFullscreenMode == FSMODE_NONE) {
            for (const auto& window : g_pCompositor->m_windows) {
                if (!window || !window->m_isMapped || window->m_workspace != workspace || window->m_fullscreenState.internal == FSMODE_NONE)
                    continue;

                backup.originalFullscreenWindow = window;
                backup.originalFullscreenMode = window->m_fullscreenState.internal;
                backup.hadFullscreenWindow = true;
                backup.fullscreenMode = window->m_fullscreenState.internal;
                break;
            }
        }

        state.fullscreenBackups.push_back(backup);
    }

    std::vector<PHLWINDOW> candidates;
    candidates.reserve(g_pCompositor->m_windows.size());

    const auto appendCandidate = [&](const PHLWINDOW& window) {
        if (!window || containsHandle(candidates, window))
            return;
        candidates.push_back(window);
    };

    for (const auto& workspace : state.managedWorkspaces) {
        if (!workspace || !workspace->m_space)
            continue;

        for (const auto& targetRef : workspace->m_space->targets()) {
            const auto target = targetRef.lock();
            if (!target)
                continue;

            appendCandidate(target->window());
        }
    }

    for (const auto& window : g_pCompositor->m_windows) {
        if (!window)
            continue;

        appendCandidate(window);
    }

    if (preserveExistingOrder && !m_state.windows.empty()) {
        std::unordered_map<PHLWINDOW, std::size_t> previousOrder;
        previousOrder.reserve(m_state.windows.size());
        for (std::size_t i = 0; i < m_state.windows.size(); ++i) {
            if (m_state.windows[i].window)
                previousOrder.emplace(m_state.windows[i].window, i);
        }

        std::stable_sort(candidates.begin(), candidates.end(), [&](const PHLWINDOW& lhs, const PHLWINDOW& rhs) {
            const auto lhsIt = previousOrder.find(lhs);
            const auto rhsIt = previousOrder.find(rhs);
            const bool lhsKnown = lhsIt != previousOrder.end();
            const bool rhsKnown = rhsIt != previousOrder.end();

            if (lhsKnown != rhsKnown)
                return lhsKnown;
            if (!lhsKnown)
                return false;

            return lhsIt->second < rhsIt->second;
        });
    }

    std::unordered_map<MONITORID, std::vector<WindowInput>> inputsByMonitor;
    std::unordered_map<MONITORID, std::vector<std::size_t>> indexesByMonitor;
    const bool useWorkspaceRows = workspaceRowsEnabled(m_handle);
    const auto rowGroupForWindow = [&](const PHLWINDOW& window) -> std::size_t {
        if (!useWorkspaceRows)
            return 0;

        const auto workspace = window && window->m_workspace ? window->m_workspace : state.ownerWorkspace;
        const auto it = std::find(state.managedWorkspaces.begin(), state.managedWorkspaces.end(), workspace);
        if (it != state.managedWorkspaces.end())
            return static_cast<std::size_t>(std::distance(state.managedWorkspaces.begin(), it));

        if (state.ownerWorkspace) {
            const auto ownerIt = std::find(state.managedWorkspaces.begin(), state.managedWorkspaces.end(), state.ownerWorkspace);
            if (ownerIt != state.managedWorkspaces.end())
                return static_cast<std::size_t>(std::distance(state.managedWorkspaces.begin(), ownerIt));
        }

        return state.managedWorkspaces.size();
    };

    for (const auto& workspace : state.managedWorkspaces)
        refreshWorkspaceLayoutSnapshot(workspace);

    for (const auto& window : candidates) {
        if (!shouldManageWindow(window, state))
            continue;

        const auto targetMonitor = preferredMonitorForWindow(window, state);
        if (!targetMonitor)
            continue;

        const bool useGoalGeometry = shouldUseGoalGeometryForStateSnapshot(window);
        const auto renderedPos = renderedWindowPosition(window, useGoalGeometry);
        const auto renderedSize = useGoalGeometry ? window->m_realSize->goal() : window->m_realSize->value();
        const Rect naturalGlobal = makeRect(renderedPos.x, renderedPos.y, renderedSize.x, renderedSize.y);
        const std::size_t windowIndex = state.windows.size();

        inputsByMonitor[targetMonitor->m_id].push_back({
            .index = windowIndex,
            .natural =
                {
                    naturalGlobal.x - targetMonitor->m_position.x,
                    naturalGlobal.y - targetMonitor->m_position.y,
                    naturalGlobal.width,
                    naturalGlobal.height,
                },
            .label = window->m_title,
            .rowGroup = rowGroupForWindow(window),
        });
        indexesByMonitor[targetMonitor->m_id].push_back(windowIndex);

        state.windows.push_back({
            .window = window,
            .targetMonitor = targetMonitor,
            .title = window->m_title,
            .naturalGlobal = naturalGlobal,
            .exitGlobal = naturalGlobal,
            .previewAlpha = std::clamp(window->m_activeInactiveAlpha->value(), 0.0F, 1.0F),
            .isFloating = window->m_isFloating,
            .isPinned = window->m_pinned,
        });
    }

    if (state.windows.empty())
        return state;

    std::vector<PHLMONITOR> activeParticipatingMonitors;
    MissionControlLayout engine;
    LayoutConfig config = loadLayoutConfig();
    config.preserveInputOrder = preserveExistingOrder;
    config.forceRowGroups = useWorkspaceRows;
    for (const auto& candidateMonitor : state.participatingMonitors) {
        if (!candidateMonitor)
            continue;

        const auto inputsIt = inputsByMonitor.find(candidateMonitor->m_id);
        const bool keepMonitor = keepEmptyParticipatingMonitors && overrideForMonitor(candidateMonitor);
        if ((inputsIt == inputsByMonitor.end() || inputsIt->second.empty()) && !keepMonitor)
            continue;

        activeParticipatingMonitors.push_back(candidateMonitor);
        if (inputsIt == inputsByMonitor.end() || inputsIt->second.empty())
            continue;

        const auto slots =
            engine.compute(inputsIt->second,
                           {
                               0.0,
                               0.0,
                               static_cast<double>(candidateMonitor->m_size.x),
                               static_cast<double>(candidateMonitor->m_size.y),
                           },
                           config);
        const auto& indexes = indexesByMonitor[candidateMonitor->m_id];
        for (std::size_t slotIndex = 0; slotIndex < slots.size() && slotIndex < indexes.size(); ++slotIndex) {
            const std::size_t windowIndex = indexes[slotIndex];
            state.windows[windowIndex].slot = slots[slotIndex];
            state.windows[windowIndex].targetGlobal =
                makeRect(candidateMonitor->m_position.x + slots[slotIndex].target.x, candidateMonitor->m_position.y + slots[slotIndex].target.y, slots[slotIndex].target.width,
                         slots[slotIndex].target.height);
            state.windows[windowIndex].relayoutFromGlobal = state.windows[windowIndex].targetGlobal;
            state.slots.push_back(slots[slotIndex]);
        }
    }
    state.participatingMonitors = std::move(activeParticipatingMonitors);

    const auto focused = Desktop::focusState()->window();
    for (std::size_t index = 0; index < state.windows.size(); ++index) {
        if (state.windows[index].window == focused) {
            state.selectedIndex = index;
            break;
        }
    }

    if (!state.selectedIndex && !state.windows.empty())
        state.selectedIndex = 0;

    if (state.selectedIndex && *state.selectedIndex < state.windows.size())
        state.focusDuringOverview = state.windows[*state.selectedIndex].window;

    return state;
}

} // namespace hymission
