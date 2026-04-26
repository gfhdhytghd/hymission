#include "overview_controller.hpp"

#include <algorithm>
#include <any>
#include <cmath>
#include <cctype>
#include <fstream>
#include <linux/input-event-codes.h>
#include <numeric>
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
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
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
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprutils/math/Region.hpp>

#include "overview_logic.hpp"

namespace hymission {

class OverviewOverlayPassElement final : public IPassElement {
  public:
    OverviewOverlayPassElement(OverviewController* controller, const PHLMONITOR& monitor) : m_controller(controller), m_monitor(monitor) {
    }

    void draw(const CRegion& damage) override {
        (void)damage;

        const auto renderMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
        if (!m_controller || !renderMonitor)
            return;

        const auto expectedMonitor = m_monitor.lock();
        if (!expectedMonitor || renderMonitor != expectedMonitor)
            return;

        m_controller->renderHiddenStripLayerProxies();
        m_controller->renderSelectionChrome();
        m_controller->renderWorkspaceStrip();
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    bool undiscardable() override {
        return true;
    }

    std::optional<CBox> boundingBox() override {
        const auto monitor = m_monitor.lock();
        if (!monitor)
            return std::nullopt;

        return CBox{{}, monitor->m_size};
    }

    CRegion opaqueRegion() override {
        return {};
    }

    const char* passName() override {
        return "OverviewOverlayPassElement";
    }

  private:
    OverviewController* m_controller = nullptr;
    PHLMONITORREF       m_monitor;
};

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
constexpr double STRIP_CARD_PADDING = 0.0;
constexpr double STRIP_THUMB_PADDING = 0.0;
constexpr double STRIP_LABEL_HEIGHT = 0.0;
constexpr double STRIP_MIN_THUMB_LENGTH = 12.0;
constexpr double RECOMMAND_STAGE_TRANSFER = 0.18;
constexpr double SELECTED_WINDOW_LAYOUT_EMPHASIS = 1.18;
constexpr double HOVER_SELECTION_RETARGET_DISTANCE = 18.0;
constexpr auto   HOVER_SELECTION_RETARGET_COOLDOWN = std::chrono::milliseconds(static_cast<int>(RELAYOUT_DURATION_MS + 48.0));
constexpr auto   HOVER_SELECTION_RETARGET_DWELL = std::chrono::milliseconds(48);
constexpr auto   TOGGLE_SWITCH_RELEASE_POLL_INTERVAL = std::chrono::milliseconds(16);
constexpr auto   MISSION_CONTROL_WORKSPACE_NAME = "Mission Control";
constexpr auto   MISSION_CONTROL_HIDDEN_WORKSPACE_PREFIX = "__hymission_hidden__:";
OverviewController* g_controller = nullptr;

enum class GestureDispatcherKind : uint8_t {
    Toggle,
    Open,
};

struct SurfacePassElementMirror {
    void*                             vtable = nullptr;
    CSurfacePassElement::SRenderData  data;
};

class ScopedFlag {
  public:
    explicit ScopedFlag(bool& flag, bool value = true) : m_flag(flag), m_previous(flag) {
        m_flag = value;
    }

    ~ScopedFlag() {
        m_flag = m_previous;
    }

  private:
    bool& m_flag;
    bool  m_previous;
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

std::optional<uint32_t> parseSwitchReleaseKeycode(const std::string& value) {
    if (value.empty())
        return std::nullopt;

    const auto parseCode = [](const std::string& digits) -> std::optional<uint32_t> {
        if (digits.empty() || !std::ranges::all_of(digits, [](unsigned char ch) { return std::isdigit(ch) != 0; }))
            return std::nullopt;

        try {
            return static_cast<uint32_t>(std::stoul(digits));
        } catch (...) {
            return std::nullopt;
        }
    };

    if (value.starts_with("code:"))
        return parseCode(value.substr(5));

    const auto parsed = parseCode(value);
    if (parsed && *parsed > 9)
        return parsed;

    return std::nullopt;
}

std::string asciiLowerCopy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

LayoutEngine parseLayoutEngine(std::string value) {
    value = asciiLowerCopy(std::move(value));
    if (value == "natural" || value == "apple" || value == "apple-like" || value == "expose" || value == "mission-control")
        return LayoutEngine::Natural;

    return LayoutEngine::Grid;
}

std::optional<uint32_t> switchReleaseModifierMask(const std::string& value) {
    const auto lowered = asciiLowerCopy(value);
    if (lowered == "shift" || lowered == "shift_l" || lowered == "shift_r")
        return HL_MODIFIER_SHIFT;
    if (lowered == "ctrl" || lowered == "control" || lowered == "control_l" || lowered == "control_r" || lowered == "ctrl_l" || lowered == "ctrl_r")
        return HL_MODIFIER_CTRL;
    if (lowered == "alt" || lowered == "alt_l" || lowered == "alt_r")
        return HL_MODIFIER_ALT;
    if (lowered == "super" || lowered == "super_l" || lowered == "super_r" || lowered == "meta" || lowered == "meta_l" || lowered == "meta_r")
        return HL_MODIFIER_META;

    return std::nullopt;
}

xkb_keysym_t keysymFromConfiguredSwitchReleaseKey(const std::string& value) {
    if (value.empty())
        return XKB_KEY_NoSymbol;

    return xkb_keysym_from_name(value.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
}

bool keyboardHasPressedKeysym(const SP<IKeyboard>& keyboard, xkb_keysym_t target) {
    if (!keyboard || !keyboard->m_xkbState || target == XKB_KEY_NoSymbol)
        return false;

    xkb_keymap* const keymap = xkb_state_get_keymap(keyboard->m_xkbState);
    if (!keymap)
        return false;

    for (xkb_keycode_t keycode = xkb_keymap_min_keycode(keymap); keycode <= xkb_keymap_max_keycode(keymap); ++keycode) {
        if (!keyboard->getPressed(keycode))
            continue;

        if (xkb_state_key_get_one_sym(keyboard->m_xkbState, keycode) == target)
            return true;
    }

    return false;
}

template <typename Predicate>
bool anyKeyboardWithState(Predicate&& predicate) {
    for (const auto& candidate : g_pInputManager->m_keyboards) {
        if (!candidate || !candidate->m_xkbState)
            continue;

        if (predicate(candidate))
            return true;
    }

    return false;
}

int recommandScopeSign(OverviewController::ScopeOverride scope) {
    return scope == OverviewController::ScopeOverride::ForceAll ? 1 : -1;
}

int signedUnit(double value) {
    if (value > 0.0001)
        return 1;
    if (value < -0.0001)
        return -1;
    return 0;
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

bool hasUsableWindowSize(const Vector2D& size) {
    return size.x >= 1.0 && size.y >= 1.0;
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

Rect scaleRectAroundCenter(const Rect& rect, double scale) {
    const double clampedScale = std::max(0.0, scale);
    const double width = rect.width * clampedScale;
    const double height = rect.height * clampedScale;
    return makeRect(rect.centerX() - width * 0.5, rect.centerY() - height * 0.5, width, height);
}

Rect clampRectInside(const Rect& rect, const Rect& bounds) {
    const double width = std::min(rect.width, bounds.width);
    const double height = std::min(rect.height, bounds.height);
    double x = rect.x;
    double y = rect.y;

    if (x < bounds.x)
        x = bounds.x;
    if (x + width > bounds.x + bounds.width)
        x = bounds.x + bounds.width - width;

    if (y < bounds.y)
        y = bounds.y;
    if (y + height > bounds.y + bounds.height)
        y = bounds.y + bounds.height - height;

    return makeRect(x, y, width, height);
}

bool rectFitsInsideBounds(const Rect& rect, const Rect& bounds, double epsilon = 0.5) {
    return rect.x >= bounds.x - epsilon && rect.y >= bounds.y - epsilon && rect.x + rect.width <= bounds.x + bounds.width + epsilon &&
        rect.y + rect.height <= bounds.y + bounds.height + epsilon;
}

double maxCenteredScaleForBounds(const Rect& rect, const Rect& bounds) {
    if (rect.width <= 1.0 || rect.height <= 1.0)
        return 1.0;

    const double halfWidth = std::max(0.0, std::min(rect.centerX() - bounds.x, bounds.x + bounds.width - rect.centerX()));
    const double halfHeight = std::max(0.0, std::min(rect.centerY() - bounds.y, bounds.y + bounds.height - rect.centerY()));
    const double maxWidth = halfWidth * 2.0;
    const double maxHeight = halfHeight * 2.0;
    return std::max(1.0, std::min(maxWidth / rect.width, maxHeight / rect.height));
}

double maxCenteredScaleForPerSideGrowth(const Rect& rect, double maxGrowXPerSide, double maxGrowYPerSide) {
    if (rect.width <= 1.0 || rect.height <= 1.0)
        return 1.0;

    const double scaleX = 1.0 + std::max(0.0, maxGrowXPerSide) * 2.0 / rect.width;
    const double scaleY = 1.0 + std::max(0.0, maxGrowYPerSide) * 2.0 / rect.height;
    return std::max(1.0, std::min(scaleX, scaleY));
}

Rect inflateRect(const Rect& rect, double amountX, double amountY) {
    return makeRect(rect.x - amountX, rect.y - amountY, rect.width + amountX * 2.0, rect.height + amountY * 2.0);
}

bool rectsOverlap(const Rect& lhs, const Rect& rhs) {
    return lhs.x < rhs.x + rhs.width && lhs.x + lhs.width > rhs.x && lhs.y < rhs.y + rhs.height && lhs.y + lhs.height > rhs.y;
}

std::optional<double> overlapExitDistanceAlongDirection(const Rect& moving, const Rect& obstacle, double dirX, double dirY) {
    if (!rectsOverlap(moving, obstacle))
        return 0.0;

    std::optional<double> exitDistance;
    const auto consider = [&](double candidate) {
        if (candidate < 0.0)
            return;
        candidate += 0.5;
        if (!exitDistance || candidate < *exitDistance)
            exitDistance = candidate;
    };

    if (dirX > 0.001)
        consider((obstacle.x + obstacle.width - moving.x) / dirX);
    else if (dirX < -0.001)
        consider((moving.x + moving.width - obstacle.x) / -dirX);

    if (dirY > 0.001)
        consider((obstacle.y + obstacle.height - moving.y) / dirY);
    else if (dirY < -0.001)
        consider((moving.y + moving.height - obstacle.y) / -dirY);

    return exitDistance;
}

CBox toBox(const Rect& rect) {
    return {
        rect.x,
        rect.y,
        rect.width,
        rect.height,
    };
}

Rect rectToMonitorLocal(const Rect& rect, const PHLMONITOR& monitor) {
    if (!monitor)
        return rect;

    return makeRect(rect.x - monitor->m_position.x, rect.y - monitor->m_position.y, rect.width, rect.height);
}

double renderScaleForMonitor(const PHLMONITOR& monitor) {
    if (!monitor || monitor->m_scale <= 0.0)
        return 1.0;

    return monitor->m_scale;
}

Rect scaleRectForRender(const Rect& rect, const PHLMONITOR& monitor) {
    const double scale = renderScaleForMonitor(monitor);
    return makeRect(rect.x * scale, rect.y * scale, rect.width * scale, rect.height * scale);
}

Rect rectToMonitorRenderLocal(const Rect& rect, const PHLMONITOR& monitor) {
    return scaleRectForRender(rectToMonitorLocal(rect, monitor), monitor);
}

CFramebuffer* layerFramebufferFor(const PHLLS& layer) {
    if (!layer || !g_pHyprOpenGL)
        return nullptr;

    const auto it = std::find_if(g_pHyprOpenGL->m_layerFramebuffers.begin(), g_pHyprOpenGL->m_layerFramebuffers.end(),
                                 [&](const auto& entry) { return entry.first.lock() == layer; });
    return it == g_pHyprOpenGL->m_layerFramebuffers.end() ? nullptr : &it->second;
}

void setTextureLinearFiltering(const SP<CTexture>& texture) {
    if (!texture)
        return;

    texture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

void setFramebufferLinearFiltering(CFramebuffer& framebuffer) {
    setTextureLinearFiltering(framebuffer.getTexture());
}

struct FramebufferBlitRect {
    GLint left = 0;
    GLint bottom = 0;
    GLint right = 0;
    GLint top = 0;
};

std::optional<FramebufferBlitRect> rectToFramebufferBlitRect(const Rect& rect, const Vector2D& framebufferSize) {
    const GLint framebufferWidth = std::max(1, static_cast<int>(std::lround(framebufferSize.x)));
    const GLint framebufferHeight = std::max(1, static_cast<int>(std::lround(framebufferSize.y)));

    const GLint left = std::clamp(static_cast<GLint>(std::floor(rect.x)), 0, framebufferWidth);
    const GLint right = std::clamp(static_cast<GLint>(std::ceil(rect.x + rect.width)), 0, framebufferWidth);
    const GLint topFromTop = std::clamp(static_cast<GLint>(std::floor(rect.y)), 0, framebufferHeight);
    const GLint bottomFromTop = std::clamp(static_cast<GLint>(std::ceil(rect.y + rect.height)), 0, framebufferHeight);
    const GLint bottom = framebufferHeight - bottomFromTop;
    const GLint top = framebufferHeight - topFromTop;

    if (left >= right || bottom >= top)
        return std::nullopt;

    return FramebufferBlitRect{
        .left = left,
        .bottom = bottom,
        .right = right,
        .top = top,
    };
}

bool blitFramebufferRegion(CFramebuffer& sourceFramebuffer, CFramebuffer& targetFramebuffer, const Rect& sourceRect, const Rect& targetRect) {
    if (!sourceFramebuffer.isAllocated() || !targetFramebuffer.isAllocated())
        return false;

    const auto sourceBlitRect = rectToFramebufferBlitRect(sourceRect, sourceFramebuffer.m_size);
    const auto targetBlitRect = rectToFramebufferBlitRect(targetRect, targetFramebuffer.m_size);
    if (!sourceBlitRect || !targetBlitRect)
        return false;

    GLint       previousReadFramebuffer = 0;
    GLint       previousDrawFramebuffer = 0;
    GLfloat     previousClearColor[4] = {};
    const bool  scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer.getFBID());
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer.getFBID());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFramebuffer.getFBID());
    glBlitFramebuffer(sourceBlitRect->left, sourceBlitRect->bottom, sourceBlitRect->right, sourceBlitRect->top, targetBlitRect->left, targetBlitRect->bottom,
                      targetBlitRect->right, targetBlitRect->top, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);

    return glGetError() == GL_NO_ERROR;
}

bool renderTextureIntoFramebuffer(const PHLMONITOR& monitor, CFramebuffer& targetFramebuffer, const SP<CTexture>& texture, const CBox& destinationBox) {
    if (!monitor || !g_pHyprRenderer || !g_pHyprOpenGL || !texture || !targetFramebuffer.isAllocated())
        return false;

    setTextureLinearFiltering(texture);
    setFramebufferLinearFiltering(targetFramebuffer);

    const bool previousBlockScreenShader = g_pHyprOpenGL->m_renderData.blockScreenShader;
    CRegion     fakeDamage{0, 0, targetFramebuffer.m_size.x, targetFramebuffer.m_size.y};
    if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &targetFramebuffer)) {
        g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockScreenShader;
        return false;
    }

    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprOpenGL->clear(CHyprColor{0.0, 0.0, 0.0, 0.0});
    g_pHyprOpenGL->renderTexture(texture, destinationBox, {.a = 1.0F});
    g_pHyprRenderer->endRender();
    g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockScreenShader;
    return true;
}

struct GaussianBlurPipeline {
    GLuint program = 0;
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLint  textureLocation = -1;
    GLint  texelSizeLocation = -1;
    GLint  directionLocation = -1;
    GLint  radiusLocation = -1;
    bool   ready = false;
    bool   failed = false;
};

GaussianBlurPipeline& gaussianBlurPipeline() {
    static GaussianBlurPipeline pipeline;
    return pipeline;
}

GLuint compileShaderStage(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint infoLogLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
    std::string infoLog(std::max(1, infoLogLength), '\0');
    glGetShaderInfoLog(shader, infoLogLength, nullptr, infoLog.data());
    glDeleteShader(shader);
    if (Log::logger)
        Log::logger->log(Log::ERR, "[hymission] gaussian blur shader compile failed: " + infoLog);
    return 0;
}

bool ensureGaussianBlurPipeline() {
    auto& pipeline = gaussianBlurPipeline();
    if (pipeline.ready)
        return true;
    if (pipeline.failed)
        return false;
    if (!g_pHyprRenderer)
        return false;

    g_pHyprRenderer->makeEGLCurrent();

    static constexpr char kVertexSource[] = R"(#version 320 es
precision highp float;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

    static constexpr char kFragmentSource[] = R"(#version 320 es
precision highp float;
in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform vec2 uDirection;
uniform float uRadius;
void main() {
    vec2 stepVec = uTexelSize * uDirection * uRadius;
    vec4 color = texture(uTexture, vTexCoord) * 0.2270270270;
    color += texture(uTexture, vTexCoord + stepVec * 1.3846153846) * 0.3162162162;
    color += texture(uTexture, vTexCoord - stepVec * 1.3846153846) * 0.3162162162;
    color += texture(uTexture, vTexCoord + stepVec * 3.2307692308) * 0.0702702703;
    color += texture(uTexture, vTexCoord - stepVec * 3.2307692308) * 0.0702702703;
    fragColor = color;
}
)";

    pipeline.vertexShader = compileShaderStage(GL_VERTEX_SHADER, kVertexSource);
    pipeline.fragmentShader = compileShaderStage(GL_FRAGMENT_SHADER, kFragmentSource);
    if (!pipeline.vertexShader || !pipeline.fragmentShader) {
        pipeline.failed = true;
        return false;
    }

    pipeline.program = glCreateProgram();
    glAttachShader(pipeline.program, pipeline.vertexShader);
    glAttachShader(pipeline.program, pipeline.fragmentShader);
    glLinkProgram(pipeline.program);

    GLint linked = GL_FALSE;
    glGetProgramiv(pipeline.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint infoLogLength = 0;
        glGetProgramiv(pipeline.program, GL_INFO_LOG_LENGTH, &infoLogLength);
        std::string infoLog(std::max(1, infoLogLength), '\0');
        glGetProgramInfoLog(pipeline.program, infoLogLength, nullptr, infoLog.data());
        if (Log::logger)
            Log::logger->log(Log::ERR, "[hymission] gaussian blur shader link failed: " + infoLog);
        glDeleteProgram(pipeline.program);
        pipeline.program = 0;
        pipeline.failed = true;
        return false;
    }

    static constexpr std::array<float, 16> kQuadVertices = {
        -1.0F, -1.0F, 0.0F, 0.0F,
         1.0F, -1.0F, 1.0F, 0.0F,
        -1.0F,  1.0F, 0.0F, 1.0F,
         1.0F,  1.0F, 1.0F, 1.0F,
    };

    glGenVertexArrays(1, &pipeline.vao);
    glGenBuffers(1, &pipeline.vbo);
    glBindVertexArray(pipeline.vao);
    glBindBuffer(GL_ARRAY_BUFFER, pipeline.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVertices), kQuadVertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    pipeline.textureLocation = glGetUniformLocation(pipeline.program, "uTexture");
    pipeline.texelSizeLocation = glGetUniformLocation(pipeline.program, "uTexelSize");
    pipeline.directionLocation = glGetUniformLocation(pipeline.program, "uDirection");
    pipeline.radiusLocation = glGetUniformLocation(pipeline.program, "uRadius");
    pipeline.ready = pipeline.textureLocation >= 0 && pipeline.texelSizeLocation >= 0 && pipeline.directionLocation >= 0 && pipeline.radiusLocation >= 0;
    pipeline.failed = !pipeline.ready;
    return pipeline.ready;
}

void destroyGaussianBlurPipeline() {
    auto& pipeline = gaussianBlurPipeline();
    if (!pipeline.ready && !pipeline.failed && pipeline.program == 0 && pipeline.vao == 0 && pipeline.vbo == 0 && pipeline.vertexShader == 0 && pipeline.fragmentShader == 0)
        return;

    if (g_pHyprRenderer)
        g_pHyprRenderer->makeEGLCurrent();

    if (pipeline.vbo)
        glDeleteBuffers(1, &pipeline.vbo);
    if (pipeline.vao)
        glDeleteVertexArrays(1, &pipeline.vao);
    if (pipeline.program)
        glDeleteProgram(pipeline.program);
    if (pipeline.vertexShader)
        glDeleteShader(pipeline.vertexShader);
    if (pipeline.fragmentShader)
        glDeleteShader(pipeline.fragmentShader);
    pipeline = {};
}

bool renderGaussianBlurPass(CFramebuffer& sourceFramebuffer, CFramebuffer& targetFramebuffer, const Vector2D& direction, float radius) {
    if (!ensureGaussianBlurPipeline() || !sourceFramebuffer.isAllocated() || !targetFramebuffer.isAllocated())
        return false;

    auto texture = sourceFramebuffer.getTexture();
    if (!texture)
        return false;

    setTextureLinearFiltering(texture);
    setFramebufferLinearFiltering(targetFramebuffer);

    GLint previousFramebuffer = 0;
    GLint previousProgram = 0;
    GLint previousVAO = 0;
    GLint previousArrayBuffer = 0;
    GLint previousActiveTexture = 0;
    GLint previousTexture0 = 0;
    GLint previousViewport[4] = {};
    const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer.getFBID());
    glViewport(0, 0, static_cast<GLsizei>(std::lround(targetFramebuffer.m_size.x)), static_cast<GLsizei>(std::lround(targetFramebuffer.m_size.y)));
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    const auto& pipeline = gaussianBlurPipeline();
    glUseProgram(pipeline.program);
    glBindVertexArray(pipeline.vao);
    texture->bind();
    glUniform1i(pipeline.textureLocation, 0);
    glUniform2f(pipeline.texelSizeLocation, 1.0F / static_cast<float>(std::max(1.0, sourceFramebuffer.m_size.x)),
                1.0F / static_cast<float>(std::max(1.0, sourceFramebuffer.m_size.y)));
    glUniform2f(pipeline.directionLocation, static_cast<float>(direction.x), static_cast<float>(direction.y));
    glUniform1f(pipeline.radiusLocation, radius);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    texture->unbind();

    glBindTexture(GL_TEXTURE_2D, previousTexture0);
    glActiveTexture(previousActiveTexture);
    glBindBuffer(GL_ARRAY_BUFFER, previousArrayBuffer);
    glBindVertexArray(previousVAO);
    glUseProgram(previousProgram);
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    if (blendEnabled)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    return true;
}

bool buildBlurredProxyFramebuffers(const SP<CFramebuffer>& sourceFramebuffer, const std::array<SP<CFramebuffer>, 4>& blurredFramebuffers) {
    if (!g_pHyprRenderer || !sourceFramebuffer || !sourceFramebuffer->isAllocated())
        return false;

    g_pHyprRenderer->makeEGLCurrent();

    const int framebufferWidth = static_cast<int>(std::lround(sourceFramebuffer->m_size.x));
    const int framebufferHeight = static_cast<int>(std::lround(sourceFramebuffer->m_size.y));
    CFramebuffer horizontalFramebuffer;
    CFramebuffer verticalFramebuffer;
    if (!horizontalFramebuffer.alloc(framebufferWidth, framebufferHeight) || !verticalFramebuffer.alloc(framebufferWidth, framebufferHeight))
        return false;
    setFramebufferLinearFiltering(horizontalFramebuffer);
    setFramebufferLinearFiltering(verticalFramebuffer);

    constexpr std::array<int, 4> kBlurIterations = {1, 3, 6, 10};
    constexpr float              kGaussianStepRadius = 1.35F;
    std::size_t                  blurLevelIndex = 0;
    int                          completedIterations = 0;
    CFramebuffer*                currentSource = sourceFramebuffer.get();

    while (blurLevelIndex < blurredFramebuffers.size()) {
        if (!blurredFramebuffers[blurLevelIndex] || !blurredFramebuffers[blurLevelIndex]->isAllocated())
            return false;
        setFramebufferLinearFiltering(*blurredFramebuffers[blurLevelIndex]);

        if (!renderGaussianBlurPass(*currentSource, horizontalFramebuffer, Vector2D{1.0, 0.0}, kGaussianStepRadius))
            return false;
        if (!renderGaussianBlurPass(horizontalFramebuffer, verticalFramebuffer, Vector2D{0.0, 1.0}, kGaussianStepRadius))
            return false;

        ++completedIterations;
        currentSource = &verticalFramebuffer;

        if (completedIterations < kBlurIterations[blurLevelIndex])
            continue;

        if (!blitFramebufferRegion(*currentSource, *blurredFramebuffers[blurLevelIndex], makeRect(0.0, 0.0, currentSource->m_size.x, currentSource->m_size.y),
                                   makeRect(0.0, 0.0, blurredFramebuffers[blurLevelIndex]->m_size.x, blurredFramebuffers[blurLevelIndex]->m_size.y)))
            return false;

        ++blurLevelIndex;
    }

    return true;
}

Rect scaleRectFromAnchor(const Rect& rect, const Rect& contentRect, WorkspaceStripAnchor anchor, double scaleX, double scaleY) {
    double anchorX = contentRect.x + contentRect.width * 0.5;
    double anchorY = contentRect.y + contentRect.height * 0.5;

    switch (anchor) {
        case WorkspaceStripAnchor::Left:
            anchorX = contentRect.x;
            break;
        case WorkspaceStripAnchor::Right:
            anchorX = contentRect.x + contentRect.width;
            break;
        case WorkspaceStripAnchor::Top:
        default:
            anchorY = contentRect.y;
            break;
    }

    const double width = rect.width * scaleX;
    const double height = rect.height * scaleY;
    return makeRect(anchorX - (anchorX - rect.x) * scaleX, anchorY - (anchorY - rect.y) * scaleY, width, height);
}

double scaleLengthForRender(const PHLMONITOR& monitor, double logicalLength) {
    return logicalLength * renderScaleForMonitor(monitor);
}

int scaleFontSizeForRender(const PHLMONITOR& monitor, int logicalSize) {
    return std::max(1, static_cast<int>(std::lround(scaleLengthForRender(monitor, logicalSize))));
}

void expandRenderDamageToFullMonitor(const PHLMONITOR& monitor) {
    if (!monitor)
        return;

    CRegion fullMonitorDamage{0.0, 0.0, monitor->m_transformedSize.x, monitor->m_transformedSize.y};
    CRegion damage = g_pHyprOpenGL->m_renderData.damage.copy();
    damage.add(fullMonitorDamage);
    CRegion finalDamage = g_pHyprOpenGL->m_renderData.finalDamage.copy();
    finalDamage.add(fullMonitorDamage);
    g_pHyprOpenGL->setDamage(damage, finalDamage);
}

Vector2D renderedWindowPosition(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    // Hyprland's realPosition is already expressed in global compositor coordinates.
    // Adding workspace render offsets or floating offsets here double-counts them and
    // pushes overview open/close geometry toward off-screen workspace animation space.
    return goal ? window->m_realPosition->goal() : window->m_realPosition->value();
}

Rect stateSnapshotGlobalRectForWindow(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    Vector2D position = renderedWindowPosition(window, goal);
    if (window->m_workspace && !window->m_pinned && !window->m_workspace->isVisible()) {
        // For hidden workspaces, keep the workspace render offset in the opening snapshot
        // so forceall windows originate from their off-screen workspace position.
        position += goal ? window->m_workspace->m_renderOffset->goal() : window->m_workspace->m_renderOffset->value();
    }

    const Vector2D size = goal ? window->m_realSize->goal() : window->m_realSize->value();
    return makeRect(position.x, position.y, size.x, size.y);
}

Rect layoutAnchorGlobalRectForWindow(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    const Vector2D position = renderedWindowPosition(window, goal);
    const Vector2D size = goal ? window->m_realSize->goal() : window->m_realSize->value();
    return makeRect(position.x, position.y, size.x, size.y);
}

Rect sceneGlobalRectForWindow(const PHLWINDOW& window, bool goal = false) {
    if (!window)
        return {};

    Vector2D position = renderedWindowPosition(window, goal);
    if (window->m_workspace && !window->m_pinned)
        position += goal ? window->m_workspace->m_renderOffset->goal() : window->m_workspace->m_renderOffset->value();

    const Vector2D size = goal ? window->m_realSize->goal() : window->m_realSize->value();
    return makeRect(position.x, position.y, size.x, size.y);
}

Rect surfaceRenderGlobalRectForWindow(const PHLWINDOW& window) {
    if (!window)
        return {};

    return sceneGlobalRectForWindow(window);
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
    CHymissionTrackpadGesture(GestureDispatcherKind dispatcher, OverviewController::ScopeOverride scope, bool recommand, eTrackpadGestureDirection direction,
                              float deltaScale)
        : m_dispatcher(dispatcher), m_scope(scope), m_recommand(recommand), m_direction(direction), m_deltaScale(deltaScale) {
    }

    void begin(const STrackpadGestureBegin& e) override {
        m_tracking = false;
        if (!g_controller || !e.swipe ||
            !g_controller->beginTrackpadGesture(m_dispatcher == GestureDispatcherKind::Open, m_scope, m_recommand, m_direction, *e.swipe, m_deltaScale))
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
    bool                              m_recommand = false;
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

    if (!canUseWindow)
        return (surface->m_current.size * monitor->m_scale).round();

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

void hkRenderLayer(void* rendererThisptr, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& now, bool popups, bool lockscreen) {
    if (!g_controller)
        return;

    g_controller->renderLayerHook(rendererThisptr, layer, monitor, now, popups, lockscreen);
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
    destroyGaussianBlurPipeline();
    clearToggleSwitchReleasePollTimer();
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
    m_mouseButtonListener = events.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        // Copy the signal payload immediately; forwarding the raw listener arg
        // has produced corrupted button/state values on current Hyprland builds.
        const auto copiedEvent = event;
        if (handleMouseButton(copiedEvent))
            info.cancelled = true;
    });
    m_keyboardListener = events.input.keyboard.key.listen([this](const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) { handleKeyboard(event, info); });
    m_windowOpenListener = events.window.open.listen([this](PHLWINDOW window) { handleWindowSetChange(window, WindowSetChangeKind::General); });
    m_windowDestroyListener = events.window.destroy.listen([this](PHLWINDOW window) {
        pruneWindowActivationHistory(window);
        handleWindowSetChange(window, WindowSetChangeKind::General, true);
    });
    m_windowCloseListener = events.window.close.listen([this](PHLWINDOW window) {
        pruneWindowActivationHistory(window);
        handleWindowSetChange(window, WindowSetChangeKind::General, true);
    });
    m_windowActiveListener = events.window.active.listen([this](PHLWINDOW window, Desktop::eFocusReason) { recordWindowActivation(window); });
    m_windowMoveWorkspaceListener =
        events.window.moveToWorkspace.listen([this](PHLWINDOW window, PHLWORKSPACE) { handleWindowSetChange(window, WindowSetChangeKind::MoveToWorkspace); });
    m_workspaceActiveListener = events.workspace.active.listen([this](PHLWORKSPACE workspace) { handleWorkspaceChange(workspace); });
    m_monitorRemovedListener = events.monitor.removed.listen([this](PHLMONITOR monitor) { handleMonitorChange(monitor); });
    m_monitorFocusedListener = events.monitor.focused.listen([this](PHLMONITOR) {
        if (isVisible() && shouldHandleInput())
            updateHoveredFromPointer(false, false, false, false, "monitor-focused");
    });

    return true;
}

void OverviewController::pruneWindowActivationHistory(const PHLWINDOW& removedWindow) {
    if (removedWindow)
        m_windowMruSerials.erase(removedWindow);

    std::erase_if(m_windowMruSerials, [](const auto& entry) { return !entry.first || !entry.first->m_isMapped; });
}

void OverviewController::recordWindowActivation(const PHLWINDOW& window, bool allowWhileVisible) {
    if (!window || !window->m_isMapped || window->isHidden())
        return;

    if (!allowWhileVisible && isVisible())
        return;

    pruneWindowActivationHistory();
    m_windowMruSerials[window] = m_nextWindowMruSerial++;
}

bool OverviewController::shouldUseRecentWindowOrdering(const State& state) const {
    return !state.collectionPolicy.onlyActiveWorkspace && multiWorkspaceSortRecentFirstEnabled();
}

SP<IKeyboard> OverviewController::inputKeyboardWithState() const {
    for (const auto& candidate : g_pInputManager->m_keyboards) {
        if (candidate && candidate->m_xkbState)
            return candidate;
    }

    return {};
}

bool OverviewController::switchReleaseKeyHeld() const {
    if (!toggleSwitchModeEnabled())
        return false;

    const auto releaseKey = switchReleaseKeyConfig();
    if (releaseKey.empty())
        return false;

    if (const auto modifierMask = switchReleaseModifierMask(releaseKey))
        return anyKeyboardWithState([modifierMask](const auto& keyboard) { return (keyboard->getModifiers() & *modifierMask) != 0; });

    if (const auto configuredKeycode = parseSwitchReleaseKeycode(releaseKey))
        return anyKeyboardWithState([configuredKeycode](const auto& keyboard) { return keyboard->getPressed(*configuredKeycode); });

    const xkb_keysym_t configuredKeysym = keysymFromConfiguredSwitchReleaseKey(releaseKey);
    return anyKeyboardWithState([configuredKeysym](const auto& keyboard) { return keyboardHasPressedKeysym(keyboard, configuredKeysym); });
}

bool OverviewController::isSwitchReleaseEvent(const IKeyboard::SKeyEvent& event, const SP<IKeyboard>& keyboard) const {
    const auto releaseKey = switchReleaseKeyConfig();
    if (!keyboard || !keyboard->m_xkbState || releaseKey.empty())
        return false;

    if (const auto modifierMask = switchReleaseModifierMask(releaseKey)) {
        return !switchReleaseKeyHeld();
    }

    if (const auto configuredKeycode = parseSwitchReleaseKeycode(releaseKey))
        return event.keycode + 8 == *configuredKeycode;

    const xkb_keysym_t configuredKeysym = keysymFromConfiguredSwitchReleaseKey(releaseKey);
    if (configuredKeysym == XKB_KEY_NoSymbol)
        return false;

    const xkb_keysym_t eventKeysym = xkb_state_key_get_one_sym(keyboard->m_xkbState, event.keycode + 8);
    if (eventKeysym == configuredKeysym)
        return true;

    xkb_keymap* const keymap = xkb_state_get_keymap(keyboard->m_xkbState);
    if (!keymap)
        return false;

    const xkb_keycode_t eventKeycode = event.keycode + 8;
    for (int level = 0; level < 8; ++level) {
        const int count = xkb_keymap_key_get_syms_by_level(keymap, eventKeycode, 0, level, nullptr);
        if (count <= 0)
            break;

        const xkb_keysym_t* levelSyms = nullptr;
        const int            resolvedCount = xkb_keymap_key_get_syms_by_level(keymap, eventKeycode, 0, level, &levelSyms);
        for (int index = 0; index < resolvedCount; ++index) {
            if (levelSyms[index] == configuredKeysym)
                return true;
        }
    }

    return false;
}

void OverviewController::updateToggleSwitchSessionReleaseTracking(const char* source) {
    if (!m_toggleSwitchSessionActive || m_beginCloseInProgress || m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing ||
        m_state.phase == Phase::ClosingSettle)
        return;

    const bool releaseKeyHeld = switchReleaseKeyHeld();
    if (!m_toggleSwitchReleaseArmed) {
        if (!releaseKeyHeld)
            return;

        m_toggleSwitchReleaseArmed = true;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] toggle switch release armed source=" << (source ? source : "?") << " key=" << switchReleaseKeyConfig();
            debugLog(out.str());
        }
        return;
    }

    if (releaseKeyHeld)
        return;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] toggle switch release poll close source=" << (source ? source : "?") << " key=" << switchReleaseKeyConfig();
        debugLog(out.str());
    }
    beginClose(CloseMode::ActivateSelection);
}

void OverviewController::scheduleToggleSwitchReleasePoll() {
    if (!g_pEventLoopManager)
        return;

    if (!m_toggleSwitchReleasePollTimer) {
        m_toggleSwitchReleasePollTimer = makeShared<CEventLoopTimer>(
            TOGGLE_SWITCH_RELEASE_POLL_INTERVAL,
            [this](SP<CEventLoopTimer> self, void*) {
                updateToggleSwitchSessionReleaseTracking("poll");

                if (!m_toggleSwitchSessionActive || m_beginCloseInProgress || m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing ||
                    m_state.phase == Phase::ClosingSettle) {
                    self->updateTimeout(std::nullopt);
                    return;
                }

                self->updateTimeout(TOGGLE_SWITCH_RELEASE_POLL_INTERVAL);
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_toggleSwitchReleasePollTimer);
        return;
    }

    m_toggleSwitchReleasePollTimer->updateTimeout(TOGGLE_SWITCH_RELEASE_POLL_INTERVAL);
}

void OverviewController::clearToggleSwitchReleasePollTimer() {
    if (!m_toggleSwitchReleasePollTimer)
        return;

    m_toggleSwitchReleasePollTimer->cancel();
    if (g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_toggleSwitchReleasePollTimer);
    m_toggleSwitchReleasePollTimer.reset();
}

void OverviewController::clearToggleSwitchSession() {
    m_toggleSwitchSessionActive = false;
    m_toggleSwitchReleaseArmed = false;
    clearToggleSwitchReleasePollTimer();
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
    if (m_state.phase == Phase::Inactive || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
        const bool activateSwitchSession = toggleSwitchModeEnabled();
        const auto result = open(args);
        if (!result.success)
            return result;

        if (activateSwitchSession && isVisible()) {
            m_toggleSwitchSessionActive = true;
            m_toggleSwitchReleaseArmed = switchReleaseKeyHeld();
            scheduleToggleSwitchReleasePoll();
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] toggle switch arm autoNext=" << (switchToggleAutoNextEnabled() ? 1 : 0) << " releaseKey=" << switchReleaseKeyConfig()
                    << " releaseArmed=" << (m_toggleSwitchReleaseArmed ? 1 : 0);
                debugLog(out.str());
            }
            if (switchToggleAutoNextEnabled())
                (void)moveSelectionCircular(1, "toggle-switch-open");
        }

        return result;
    }

    if (m_toggleSwitchSessionActive) {
        scheduleToggleSwitchReleasePoll();
        if (debugLogsEnabled())
            debugLog("[hymission] toggle switch cycle");
        (void)moveSelectionCircular(1, "toggle-switch-cycle");
        return {};
    }

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
    if (m_stripSnapshotRenderDepth > 0)
        return;

    if (!isVisible())
        return;

    const auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!monitor || !ownsMonitor(monitor))
        return;

    if (!m_workspaceTransition.active && (m_state.phase == Phase::Opening || m_state.phase == Phase::Active) &&
        std::any_of(m_state.windows.begin(), m_state.windows.end(), [](const ManagedWindow& managed) { return managed.window && managed.window->m_fadingOut; })) {
        scheduleVisibleStateRebuild();
    }

    setFullscreenRenderOverride(true);
    expandRenderDamageToFullMonitor(monitor);

    if (stage == RENDER_POST_WALLPAPER) {
        updateOverviewWorkspaceTransition();
        updateAnimation();
        flushQueuedSelectionRetargetDuringOverview();
        flushQueuedRealFocusDuringOverview();
        renderBackdrop();
    } else if (stage == RENDER_POST_WINDOWS) {
        if (m_deactivatePending) {
            if (debugLogsEnabled())
                debugLog("[hymission] post-windows queue deferred deactivate");
            scheduleDeactivate();
            return;
        }

        g_pHyprRenderer->m_renderPass.add(makeUnique<OverviewOverlayPassElement>(this, monitor));
        if (shouldContinuouslyRefreshWorkspaceStripSnapshots()) {
            m_stripSnapshotsDirty = true;
            scheduleWorkspaceStripSnapshotRefresh();
        }
        if ((isAnimating() || m_state.phase == Phase::ClosingSettle || m_state.relayoutActive || m_postOpenRefreshFrames > 0) && !m_deactivatePending) {
            damageOwnedMonitors();
            if (m_postOpenRefreshFrames > 0)
                --m_postOpenRefreshFrames;
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

    if (m_pressedWindowIndex || m_draggedWindowIndex) {
        updateHoveredFromPointer(false, false, false, false, "mouse-move-drag");

        const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
        if (!m_draggedWindowIndex && m_pressedWindowIndex && *m_pressedWindowIndex < m_state.windows.size()) {
            const double distance = std::hypot(pointer.x - m_pressedWindowPointer.x, pointer.y - m_pressedWindowPointer.y);
            if (distance >= 14.0) {
                const auto& managed = m_state.windows[*m_pressedWindowIndex];
                const Rect  rect = currentPreviewRect(managed);
                m_draggedWindowIndex = m_pressedWindowIndex;
                m_draggedWindowPointerOffset = Vector2D{pointer.x - rect.x, pointer.y - rect.y};
            }
        }

        damageOwnedMonitors();
        return;
    }

    updateHoveredFromPointer(true, true, true, true, "mouse-move");
}

bool OverviewController::handleMouseButton(const IPointer::SButtonEvent& event) {
    if (m_postCloseForcedFocusLatched && !isVisible()) {
        clearPostCloseForcedFocus();
        if (m_restoreInputFollowMouseAfterPostClose) {
            setInputFollowMouseOverride(false);
            m_restoreInputFollowMouseAfterPostClose = false;
        }
    }

    if (!shouldHandleInput())
        return false;

    if (m_state.phase == Phase::Closing)
        return true;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] mouse button event state=" << static_cast<int>(event.state) << " button=" << event.button;
        debugLog(out.str());
    }

    const auto buttonLooksValid = [&](uint32_t button) {
        return button >= BTN_LEFT && button <= BTN_TASK;
    };

    uint32_t effectiveButton = event.button;
    bool     synthesizedButton = false;
    if (!buttonLooksValid(effectiveButton)) {
        effectiveButton = BTN_LEFT;
        synthesizedButton = true;
    }

    wl_pointer_button_state effectiveState = event.state;
    bool                    synthesizedState = false;
    if (effectiveState != WL_POINTER_BUTTON_STATE_PRESSED && effectiveState != WL_POINTER_BUTTON_STATE_RELEASED) {
        effectiveState = m_primaryButtonPressed ? WL_POINTER_BUTTON_STATE_RELEASED : WL_POINTER_BUTTON_STATE_PRESSED;
        synthesizedState = true;
    } else if (synthesizedButton) {
        // Some Hyprland/plugin ABI combinations are delivering a valid callback
        // but a corrupted button code and an unusable edge indicator. Fall back
        // to a minimal local left-button state machine so strip clicks still
        // produce a press edge followed by a release edge.
        effectiveState = m_primaryButtonPressed ? WL_POINTER_BUTTON_STATE_RELEASED : WL_POINTER_BUTTON_STATE_PRESSED;
        synthesizedState = true;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] mouse button resolved rawState=" << static_cast<int>(event.state) << " rawButton=" << event.button
            << " effectiveState=" << static_cast<int>(effectiveState) << " effectiveButton=" << effectiveButton
            << " synthesizedButton=" << (synthesizedButton ? 1 : 0) << " synthesizedState=" << (synthesizedState ? 1 : 0)
            << " primaryDownBefore=" << (m_primaryButtonPressed ? 1 : 0);
        debugLog(out.str());
    }

    if (effectiveButton != BTN_LEFT)
        return true;

    if (effectiveState == WL_POINTER_BUTTON_STATE_PRESSED)
        m_primaryButtonPressed = true;
    else if (effectiveState == WL_POINTER_BUTTON_STATE_RELEASED)
        m_primaryButtonPressed = false;

    const auto cachedHoveredStripIndex = m_state.hoveredStripIndex;
    const auto cachedHoveredIndex = m_state.hoveredIndex;
    const Vector2D pointerBeforeUpdate = g_pInputManager->getMouseCoordsInternal();
    updateHoveredFromPointer(false, false, false, false, "mouse-button-refresh");
    const auto effectiveHoveredStripIndex = m_state.hoveredStripIndex ? m_state.hoveredStripIndex : cachedHoveredStripIndex;
    const auto effectiveHoveredIndex = m_state.hoveredIndex ? m_state.hoveredIndex : cachedHoveredIndex;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] mouse button state=" << static_cast<int>(effectiveState) << " button=" << effectiveButton
            << " ptr=" << vectorToString(pointerBeforeUpdate)
            << " cachedStrip=" << (cachedHoveredStripIndex ? std::to_string(*cachedHoveredStripIndex) : "<null>")
            << " liveStrip=" << (m_state.hoveredStripIndex ? std::to_string(*m_state.hoveredStripIndex) : "<null>")
            << " effectiveStrip=" << (effectiveHoveredStripIndex ? std::to_string(*effectiveHoveredStripIndex) : "<null>")
            << " cachedWindow=" << (cachedHoveredIndex ? std::to_string(*cachedHoveredIndex) : "<null>")
            << " liveWindow=" << (m_state.hoveredIndex ? std::to_string(*m_state.hoveredIndex) : "<null>")
            << " pressedStrip=" << (m_pressedStripIndex ? std::to_string(*m_pressedStripIndex) : "<null>");
        debugLog(out.str());
    }

    if (effectiveState == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (m_draggedWindowIndex && *m_draggedWindowIndex < m_state.windows.size()) {
            const auto window = m_state.windows[*m_draggedWindowIndex].window;
            const auto hoveredStripIndex = m_state.hoveredStripIndex;
            clearStripWindowDragState();

            if (window && hoveredStripIndex && *hoveredStripIndex < m_state.stripEntries.size()) {
                const auto& entry = m_state.stripEntries[*hoveredStripIndex];
                auto        targetWorkspace = entry.workspace ? entry.workspace : g_pCompositor->getWorkspaceByID(entry.workspaceId);
                if (!targetWorkspace && entry.monitor && entry.workspaceId != WORKSPACE_INVALID) {
                    const std::string targetName = entry.workspaceName.empty() ? std::to_string(entry.workspaceId) : entry.workspaceName;
                    targetWorkspace = g_pCompositor->createNewWorkspace(entry.workspaceId, entry.monitor->m_id, targetName);
                }

                if (targetWorkspace && window->m_workspace != targetWorkspace) {
                    g_pCompositor->moveWindowToWorkspaceSafe(window, targetWorkspace);
                    if (g_pAnimationManager)
                        g_pAnimationManager->frameTick();
                    rebuildVisibleState();
                }
            }

            damageOwnedMonitors();
            return true;
        }

        if (m_pressedStripIndex && *m_pressedStripIndex < m_state.stripEntries.size()) {
            const auto pressedStripIndex = *m_pressedStripIndex;
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] mouse release activating strip index=" << pressedStripIndex;
                debugLog(out.str());
            }
            clearStripWindowDragState();
            activateStripTarget(pressedStripIndex);
            return true;
        }

        if (m_pressedWindowIndex && *m_pressedWindowIndex < m_state.windows.size()) {
            m_state.selectedIndex = m_pressedWindowIndex;
            clearStripWindowDragState();
            activateSelection();
            return true;
        }

        clearStripWindowDragState();
        return true;
    }

    if (effectiveState != WL_POINTER_BUTTON_STATE_PRESSED)
        return true;

    if (effectiveHoveredStripIndex && *effectiveHoveredStripIndex < m_state.stripEntries.size()) {
        clearStripWindowDragState();
        m_pressedStripIndex = *effectiveHoveredStripIndex;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] mouse press captured strip index=" << *m_pressedStripIndex;
            debugLog(out.str());
        }
        damageOwnedMonitors();
        return true;
    }

    if (effectiveHoveredIndex) {
        const auto previousSelectedWindow = selectedWindow();
        m_state.selectedIndex = effectiveHoveredIndex;
        m_state.focusDuringOverview = m_state.windows[*effectiveHoveredIndex].window;
        m_queuedOverviewSelectionTarget.reset();
        m_queuedOverviewSelectionSyncScrollingSpot = false;
        m_queuedOverviewLiveFocusTarget.reset();
        m_queuedOverviewLiveFocusSyncScrollingSpot = false;
        m_pressedWindowIndex = effectiveHoveredIndex;
        m_pressedWindowPointer = g_pInputManager->getMouseCoordsInternal();
        latchHoverSelectionAnchor(m_pressedWindowPointer);
        updateSelectedWindowLayout(previousSelectedWindow);
        damageOwnedMonitors();
        return true;
    }

    clearStripWindowDragState();
    if (debugLogsEnabled())
        debugLog("[hymission] mouse press fell through to background close");
    beginClose();
    return true;
}

void OverviewController::handleKeyboard(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
    const auto keyboard = inputKeyboardWithState();
    if (!keyboard || !keyboard->m_xkbState)
        return;

    if (m_toggleSwitchSessionActive)
        updateToggleSwitchSessionReleaseTracking("keyboard");

    if (!shouldHandleInput())
        return;

    if (m_state.phase == Phase::Closing)
        return;

    if (m_toggleSwitchSessionActive && event.state == WL_KEYBOARD_KEY_STATE_RELEASED && isSwitchReleaseEvent(event, keyboard)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] toggle switch release close key=" << switchReleaseKeyConfig() << " keycode=" << event.keycode << " modifiers=" << keyboard->getModifiers();
            debugLog(out.str());
        }
        beginClose(CloseMode::ActivateSelection);
        info.cancelled = true;
        return;
    }

    if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
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

void OverviewController::handleWindowSetChange(PHLWINDOW window, WindowSetChangeKind kind, bool preferDeferredRebuild) {
    if (window && m_postCloseForcedFocusLatched && m_postCloseForcedFocus.lock() == window)
        clearPostCloseForcedFocus();
    if (window && m_pendingLiveFocusWorkspaceChangeTarget.lock() == window)
        m_pendingLiveFocusWorkspaceChangeTarget.reset();
    if (!window) {
        clearPendingWindowGeometryRetry();
        return;
    }

    if (!isVisible())
        return;

    if (kind == WindowSetChangeKind::MoveToWorkspace && window->m_pinned) {
        const auto* managed = managedWindowFor(m_state, window);
        const auto nextMonitor = preferredMonitorForWindow(window, m_state);
        if (managed && windowMatchesOverviewScope(window, m_state, false) && managed->targetMonitor && nextMonitor == managed->targetMonitor) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] ignore pinned workspace-move rebuild target=" << debugWindowLabel(window)
                    << " monitor=" << managed->targetMonitor->m_name;
                if (window->m_workspace)
                    out << " workspace=" << debugWorkspaceLabel(window->m_workspace);
                debugLog(out.str());
            }
            return;
        }
    }

    if (m_applyingWorkspaceTransitionCommit) {
        // Moving pinned windows during a transition commit emits window-set
        // events synchronously. Defer the rebuild until after the commit so we
        // don't clear the transition state out from under the caller.
        m_rebuildVisibleStateAfterWorkspaceTransitionCommit = true;
        return;
    }

    const bool shouldDeferRebuild = preferDeferredRebuild || insideRenderLifecycle();

    if (m_workspaceTransition.active) {
        if (shouldDeferRebuild) {
            scheduleVisibleStateRebuild();
        } else {
            clearOverviewWorkspaceTransition();
            rebuildVisibleState();
            updatePendingWindowGeometryRetry(window);
        }
        return;
    }

    if (!shouldAutoCloseFor(window)) {
        if (m_pendingWindowGeometryRetryTarget.lock() == window)
            clearPendingWindowGeometryRetry();
        return;
    }

    if (m_state.phase == Phase::Opening || m_state.phase == Phase::Active) {
        if (shouldDeferRebuild) {
            scheduleVisibleStateRebuild();
        } else {
            rebuildVisibleState();
            updatePendingWindowGeometryRetry(window);
        }
        return;
    }

    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
        clearPendingWindowGeometryRetry();
        beginClose(CloseMode::Abort);
    }
}

void OverviewController::handleWorkspaceChange(PHLWORKSPACE workspace) {
    const bool liveFocusWorkspaceChange = matchesPendingLiveFocusWorkspaceChange(workspace);
    const bool stripWorkspaceChange = matchesPendingStripWorkspaceChange(workspace);
    const auto action = resolveOverviewWorkspaceChangeAction(isVisible(), m_applyingWorkspaceTransitionCommit, m_workspaceTransition.active,
                                                             m_beginCloseInProgress || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle,
                                                             liveFocusWorkspaceChange || stripWorkspaceChange, allowsWorkspaceSwitchInOverview());
    const auto refreshWorkspaceStripActivity = [this]() {
        bool changed = false;
        for (auto& entry : m_state.stripEntries) {
            const bool active = entry.monitor && entry.workspace && entry.monitor->m_activeWorkspace == entry.workspace;
            if (entry.active == active)
                continue;

            entry.active = active;
            changed = true;
        }
        return changed;
    };

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

        if (!m_state.collectionPolicy.onlyActiveWorkspace) {
            const bool stripActivityChanged = refreshWorkspaceStripActivity();
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] skip overview rebuild for live focus workspace change outside active-workspace scope";
                if (workspace)
                    out << " workspace=" << debugWorkspaceLabel(workspace);
                out << " stripChanged=" << (stripActivityChanged ? 1 : 0);
                debugLog(out.str());
            }

            damageOwnedMonitors();
            return;
        }
    }

    if (stripWorkspaceChange) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] keep overview open for strip workspace change";
            if (const auto target = m_pendingStripWorkspaceChangeTarget.lock())
                out << " target=" << debugWorkspaceLabel(target);
            debugLog(out.str());
        }
        clearPendingStripWorkspaceChange();
    }

    if (insideRenderLifecycle()) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] defer workspace change handling until after render action="
                << (action == OverviewWorkspaceChangeAction::Rebuild ? "rebuild" : "abort");
            if (workspace)
                out << " workspace=" << debugWorkspaceLabel(workspace);
            debugLog(out.str());
        }

        scheduleWorkspaceChangeHandling(workspace, action);
        return;
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

bool OverviewController::shouldHideLayerSurface(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (!layer || !monitor || !isVisible() || !workspaceStripEnabled(m_state) || !hideBarsWhenStripShownEnabled() || !ownsMonitor(monitor))
        return false;

    const auto layerMonitor = layer->m_monitor.lock();
    const auto layerResource = layer->m_layerSurface.lock();
    if (!layerMonitor || layerMonitor != monitor || !layerResource || !layer->m_mapped || layer->m_readyToDelete)
        return false;

    return layerResource->m_current.exclusive > 0;
}

void OverviewController::renderLayerHook(void* rendererThisptr, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& now, bool popups, bool lockscreen) {
    if (!m_renderLayerOriginal)
        return;

    if (!lockscreen && shouldHideLayerSurface(layer, monitor)) {
        if (!hideBarAnimationEffectsEnabled())
            return;
        if (shouldRenderHiddenStripLayerProxy(layer, monitor))
            return;
    }

    m_renderLayerOriginal(rendererThisptr, layer, monitor, now, popups, lockscreen);
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

    if (m_surfaceRenderDataTransformDepth > 0) {
        CBox box = m_surfaceTexBoxOriginal(surfacePassThisptr);
        auto* renderData = surfaceRenderDataMutable(surfacePassThisptr);
        auto  monitor = renderData ? renderData->pMonitor.lock() : PHLMONITOR{};
        if (renderData && monitor)
            adjustTransformedSurfaceBoxSize(*renderData, monitor, box);
        return box;
    }

    CSurfacePassElement::SRenderData* renderData = nullptr;
    PHLMONITOR                        monitor;
    SurfaceRenderDataSnapshot        snapshot;
    if (!prepareSurfaceRenderData(surfacePassThisptr, "texbox", renderData, monitor, snapshot))
        return m_surfaceTexBoxOriginal(surfacePassThisptr);

    ++m_surfaceRenderDataTransformDepth;
    CBox box = m_surfaceTexBoxOriginal(surfacePassThisptr);
    adjustTransformedSurfaceBoxSize(*renderData, monitor, box);
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
    adjustTransformedSurfaceBoxSize(*renderData, monitor, fullBox);
    --m_surfaceRenderDataTransformDepth;
    fullBox.scale(monitor->m_scale);
    fullBox.round();
    cancel = fullBox.width <= 0.0 || fullBox.height <= 0.0;
    restoreSurfaceRenderData(renderData, snapshot);
    return cancel ? CRegion{} : CRegion(fullBox);
}

LayoutConfig OverviewController::loadLayoutConfig() const {
    const double outerPadding = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding", 32));
    return {
        .engine = parseLayoutEngine(getConfigString(m_handle, "plugin:hymission:layout_engine", "grid")),
        .outerPaddingTop = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_top", static_cast<long>(outerPadding))),
        .outerPaddingRight = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_right", static_cast<long>(outerPadding))),
        .outerPaddingBottom = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_bottom", static_cast<long>(outerPadding))),
        .outerPaddingLeft = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:outer_padding_left", static_cast<long>(outerPadding))),
        .rowSpacing = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:row_spacing", 32)),
        .columnSpacing = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:column_spacing", 32)),
        .smallWindowBoost = getConfigFloat(m_handle, "plugin:hymission:small_window_boost", 1.35),
        .maxPreviewScale = getConfigFloat(m_handle, "plugin:hymission:max_preview_scale", 0.95),
        .minWindowLength = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:min_window_length", 120)),
        .minPreviewShortEdge = static_cast<double>(getConfigInt(m_handle, "plugin:hymission:min_preview_short_edge", 32)),
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

bool OverviewController::expandSelectedWindowEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:expand_selected_window", 1) != 0;
}

bool OverviewController::focusFollowsMouseEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:overview_focus_follows_mouse", 1) != 0;
}

bool OverviewController::multiWorkspaceSortRecentFirstEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:multi_workspace_sort_recent_first", 1) != 0;
}

bool OverviewController::toggleSwitchModeEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:toggle_switch_mode", 1) != 0;
}

bool OverviewController::switchToggleAutoNextEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:switch_toggle_auto_next", 1) != 0;
}

std::string OverviewController::switchReleaseKeyConfig() const {
    return getConfigString(m_handle, "plugin:hymission:switch_release_key", "Super_L");
}

bool OverviewController::gestureInvertVerticalEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:gesture_invert_vertical", 0) != 0;
}

bool OverviewController::workspaceSwipeInvertEnabled() const {
    return getConfigInt(m_handle, "gestures:workspace_swipe_invert", 0) != 0;
}

bool OverviewController::workspaceChangeKeepsOverviewEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:workspace_change_keeps_overview", 1) != 0;
}

bool OverviewController::hideBarsWhenStripShownEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:hide_bar_when_strip", 1) != 0;
}

bool OverviewController::hideBarAnimationEffectsEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:hide_bar_animation", 1) != 0;
}

bool OverviewController::hideBarAnimationBlurEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:hide_bar_animation_blur", 1) != 0;
}

double OverviewController::hideBarAnimationMoveMultiplier() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:hide_bar_animation_move_multiplier", 0.8), 0.0, 2.0);
}

double OverviewController::hideBarAnimationScaleDivisor() const {
    return std::max(1.0, getConfigFloat(m_handle, "plugin:hymission:hide_bar_animation_scale_divisor", 1.1));
}

double OverviewController::hideBarAnimationAlphaEnd() const {
    return std::clamp(getConfigFloat(m_handle, "plugin:hymission:hide_bar_animation_alpha_end", 0.0), 0.0, 1.0);
}

bool OverviewController::barSingleMissionControlEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:bar_single_mission_control", 0) != 0;
}

bool OverviewController::showFocusIndicatorEnabled() const {
    return getConfigInt(m_handle, "plugin:hymission:show_focus_indicator", 0) != 0;
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
    return barSingleMissionControlEnabled() && !state.collectionPolicy.onlyActiveWorkspace;
}

std::string OverviewController::workspaceStripAnchor() const {
    switch (parseWorkspaceStripAnchor(getConfigString(m_handle, "plugin:hymission:workspace_strip_anchor", "left"))) {
        case WorkspaceStripAnchor::Left:
            return "left";
        case WorkspaceStripAnchor::Right:
            return "right";
        case WorkspaceStripAnchor::Top:
        default:
            return "top";
    }
}

WorkspaceStripEmptyMode OverviewController::workspaceStripEmptyMode() const {
    return parseWorkspaceStripEmptyMode(getConfigString(m_handle, "plugin:hymission:workspace_strip_empty_mode", "existing"));
}

double OverviewController::workspaceStripThickness(const PHLMONITOR& monitor) const {
    const double raw = std::max(64.0, static_cast<double>(getConfigInt(m_handle, "plugin:hymission:workspace_strip_thickness", 160)));
    if (!monitor)
        return raw;

    const bool horizontal = workspaceStripAnchor() == "top";
    const double limit = (horizontal ? static_cast<double>(monitor->m_size.y) : static_cast<double>(monitor->m_size.x)) * 0.35;
    return std::clamp(raw, 64.0, std::max(64.0, limit));
}

double OverviewController::workspaceStripGap() const {
    return std::max(0.0, static_cast<double>(getConfigInt(m_handle, "plugin:hymission:workspace_strip_gap", 24)));
}

bool OverviewController::workspaceStripEnabled(const State& state) const {
    return state.collectionPolicy.onlyActiveWorkspace && !state.suppressWorkspaceStrip;
}

bool OverviewController::isStripOnlyOverviewState(const State& state) const {
    return workspaceStripEnabled(state) && state.windows.empty() && !state.stripEntries.empty();
}

bool OverviewController::shouldContinuouslyRefreshWorkspaceStripSnapshots() const {
    if (!workspaceStripEnabled(m_state))
        return false;

    if ((m_state.phase != Phase::Opening && m_state.phase != Phase::Active) || m_workspaceTransition.active)
        return false;

    // Once overview has no managed previews left, keep the strip visible but
    // stop re-rendering its snapshots every frame. The empty-state strip is
    // stable until a real workspace/window/monitor change rebuilds it.
    return !isStripOnlyOverviewState(m_state);
}

bool OverviewController::isCurrentActiveWorkspaceStripEntry(const WorkspaceStripEntry& entry) const {
    return entry.monitor && entry.workspace && entry.monitor->m_activeWorkspace && entry.workspace == entry.monitor->m_activeWorkspace;
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

    const auto backupAndRename = [&](const PHLWORKSPACE& workspace, const std::string& name) {
        if (!workspace || workspace->m_name == name)
            return;

        const auto alreadyBackedUp = std::ranges::any_of(m_workspaceNameBackups, [&](const WorkspaceNameBackup& backup) { return backup.workspace == workspace; });
        if (!alreadyBackedUp) {
            m_workspaceNameBackups.push_back({
                .workspace = workspace,
                .name = workspace->m_name,
            });
        }

        workspace->rename(name);
    };

    const bool collapseBar = barSingleMissionControlEnabled();
    PHLWORKSPACE primaryWorkspace;
    if (state.ownerMonitor && state.ownerMonitor->m_activeWorkspace && containsHandle(state.managedWorkspaces, state.ownerMonitor->m_activeWorkspace))
        primaryWorkspace = state.ownerMonitor->m_activeWorkspace;

    if (collapseBar && primaryWorkspace) {
        for (const auto& workspace : state.managedWorkspaces) {
            if (!workspace || workspace->m_isSpecialWorkspace || workspace == primaryWorkspace)
                continue;

            backupAndRename(workspace, std::string{MISSION_CONTROL_HIDDEN_WORKSPACE_PREFIX} + std::to_string(workspace->m_id));
        }
    }

    for (const auto& monitor : state.participatingMonitors) {
        if (!monitor || !monitor->m_activeWorkspace)
            continue;

        if (!containsHandle(state.managedWorkspaces, monitor->m_activeWorkspace))
            continue;

        if (collapseBar && primaryWorkspace && monitor->m_activeWorkspace != primaryWorkspace) {
            backupAndRename(monitor->m_activeWorkspace,
                            std::string{MISSION_CONTROL_HIDDEN_WORKSPACE_PREFIX} + std::to_string(monitor->m_activeWorkspace->m_id));
            continue;
        }

        backupAndRename(monitor->m_activeWorkspace, MISSION_CONTROL_WORKSPACE_NAME);
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

    bool         recommand = false;
    ScopeOverride requestedScopeValue = ScopeOverride::Default;
    const auto   trimmedDispatcherArgs = trimCopy(dispatcherArgs);
    if (trimmedDispatcherArgs == "recommand" || trimmedDispatcherArgs == "recommend") {
        if (dispatcherKind != GestureDispatcherKind::Toggle)
            return "gesture recommand is only supported with hymission:toggle";

        recommand = true;
    } else {
        std::string scopeError;
        const auto  requestedScope = parseScopeOverride(dispatcherArgs, scopeError);
        if (!requestedScope)
            return scopeError;

        requestedScopeValue = *requestedScope;
    }

    const bool disableInhibit = flags.contains('p');
    g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
    const auto addResult =
        g_pTrackpadGestures->addGesture(makeUnique<CHymissionTrackpadGesture>(dispatcherKind, requestedScopeValue, recommand, direction, deltaScale), fingerCount, direction,
                                        modMask, deltaScale, disableInhibit);
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
            << " recommand=" << (recommand ? 1 : 0)
            << " scale=" << deltaScale << " modMask=" << modMask << " disableInhibit=" << (disableInhibit ? 1 : 0);
        debugLog(out.str());
    }

    return {};
}

bool OverviewController::beginTrackpadGesture(bool openOnly, ScopeOverride requestedScope, bool recommand, eTrackpadGestureDirection direction,
                                              const IPointer::SSwipeUpdateEvent& event, float deltaScale) {
    if (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return false;

    const bool opening = !isVisible() || openOnly || m_state.phase == Phase::Opening;
    const double initialDelta = normalizedGestureDelta(event, direction, deltaScale, gestureInvertVerticalEnabled());
    const int    initialDirectionSign = signedUnit(initialDelta);

    if (recommand) {
        if (openOnly || initialDirectionSign == 0)
            return false;

        const bool currentlyVisible = isVisible();
        ScopeOverride compactScope = ScopeOverride::OnlyCurrentWorkspace;
        ScopeOverride initialScope = ScopeOverride::Default;
        double       initialSignedProgress = 0.0;
        bool         gestureOpening = true;
        bool         allowTransfer = false;
        int          directionSign = initialDirectionSign;

        if (currentlyVisible) {
            initialScope = m_state.collectionPolicy.requestedScope;
            const int currentSign = recommandScopeSign(initialScope);
            // `recommand` always uses `onlycurrentworkspace` as its compact side,
            // regardless of the config-driven default scope.
            initialSignedProgress = visualProgress() * static_cast<double>(currentSign);
            allowTransfer = resolveRecommandVisibleGestureMode(currentSign, initialDirectionSign) == RecommandVisibleGestureMode::TransferCapable;
            gestureOpening = false;

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
        } else {
            const auto monitor = g_pCompositor->getMonitorFromCursor();
            if (!monitor)
                return false;

            const auto initialScopeTarget = initialDirectionSign > 0 ? ScopeOverride::ForceAll : compactScope;
            initialScope = initialScopeTarget;
            requestedScope = initialScopeTarget;
            directionSign = recommandScopeSign(initialScopeTarget);

            m_suppressInitialHoverUpdate = true;
            beginOpen(monitor, initialScopeTarget);
            m_suppressInitialHoverUpdate = false;
            if (!isVisible())
                return false;
        }

        m_gestureSession = {
            .active = true,
            .recommand = true,
            .startedVisible = currentlyVisible,
            .opening = gestureOpening,
            .allowRecommandTransfer = allowTransfer,
            .requestedScope = currentlyVisible ? initialScope : requestedScope,
            .initialScope = initialScope,
            .compactScope = compactScope,
            .direction = direction,
            .directionSign = directionSign,
            .openness = currentlyVisible ? std::abs(initialSignedProgress) : visualProgress(),
            .signedProgress = initialSignedProgress,
            .lastAlignedSpeed = 0.0,
            .deltaScale = deltaScale,
        };

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] recommand gesture begin openness=" << m_gestureSession.openness << " signed=" << m_gestureSession.signedProgress
                << " transfer=" << (m_gestureSession.allowRecommandTransfer ? 1 : 0) << " scale=" << deltaScale << " dir="
                << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical");
            debugLog(out.str());
        }

        updateTrackpadGesture(event);
        damageOwnedMonitors();
        return true;
    }

    if (openOnly && isVisible())
        return false;

    // Keeping overview open across workspace changes only affects native workspace swipes.
    // The plugin's own toggle gesture must still be able to close the visible overview.
    if (initialDirectionSign == 0 || (opening && initialDelta <= 0.0)) {
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
        .requestedScope = opening ? requestedScope : m_state.collectionPolicy.requestedScope,
        .direction = direction,
        .directionSign = opening ? 1 : initialDirectionSign,
        .openness = visualProgress(),
        .lastAlignedSpeed = 0.0,
        .deltaScale = deltaScale,
    };

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture begin mode=" << (opening ? "open" : "close") << " openness=" << m_gestureSession.openness << " scale=" << deltaScale
            << " dir=" << (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL ? "horizontal" : "vertical");
        debugLog(out.str());
    }

    updateTrackpadGesture(event);
    damageOwnedMonitors();
    return true;
}

void OverviewController::updateTrackpadGesture(const IPointer::SSwipeUpdateEvent& event) {
    if (!m_gestureSession.active)
        return;

    const double delta = normalizedGestureDelta(event, m_gestureSession.direction, m_gestureSession.deltaScale, gestureInvertVerticalEnabled());
    const double alignedDelta = delta * static_cast<double>(m_gestureSession.directionSign);

    if (m_gestureSession.recommand) {
        m_gestureSession.lastAlignedSpeed = alignedDelta;
        const double deltaProgress = alignedDelta / gestureSwipeDistance();
        constexpr double MAX_STAGE_PROGRESS = 1.0 + RECOMMAND_STAGE_TRANSFER;

        const auto syncCurrentScopeProgress = [&] {
            m_gestureSession.openness = clampUnit(std::abs(m_gestureSession.signedProgress));
            m_gestureSession.opening = !(m_gestureSession.startedVisible && m_gestureSession.requestedScope == m_gestureSession.initialScope);
        };

        const auto enterScope = [&](ScopeOverride requestedScope, int sign, double openness) -> bool {
            if (m_state.collectionPolicy.requestedScope != requestedScope) {
                if (!retargetGestureScope(requestedScope))
                    return false;

                m_gestureSession.startedVisible = false;
            }

            m_gestureSession.requestedScope = requestedScope;
            m_gestureSession.hiddenGapProgress = 0.0;
            m_gestureSession.signedProgress = static_cast<double>(sign) * clampUnit(openness);
            m_gestureSession.openness = clampUnit(openness);
            m_gestureSession.opening = true;
            m_gestureSession.allowRecommandTransfer = false;
            m_gestureSession.directionSign = sign;
            return true;
        };

        const int currentSign = recommandScopeSign(m_gestureSession.requestedScope);
        const double sideSpaceDelta = static_cast<double>(m_gestureSession.opening ? currentSign : -currentSign) * deltaProgress;

        // Crossing through the hidden workspace should leave a small transfer
        // gap before the opposite scope starts opening.
        if (std::abs(m_gestureSession.hiddenGapProgress) > 0.0001) {
            const double projectedGap = std::clamp(m_gestureSession.hiddenGapProgress + sideSpaceDelta, -MAX_STAGE_PROGRESS, MAX_STAGE_PROGRESS);
            const int    projectedGapSign = signedUnit(projectedGap);

            if (projectedGapSign == 0) {
                m_gestureSession.hiddenGapProgress = 0.0;
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else if (projectedGapSign == currentSign) {
                m_gestureSession.hiddenGapProgress = 0.0;
                m_gestureSession.signedProgress = static_cast<double>(currentSign) * clampUnit(std::abs(projectedGap));
                syncCurrentScopeProgress();
            } else if (!m_gestureSession.allowRecommandTransfer) {
                m_gestureSession.hiddenGapProgress = 0.0;
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else if (std::abs(projectedGap) < RECOMMAND_STAGE_TRANSFER) {
                m_gestureSession.hiddenGapProgress = projectedGap;
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else {
                const auto targetScope = projectedGapSign > 0 ? ScopeOverride::ForceAll : m_gestureSession.compactScope;
                const double targetOpenness = std::abs(projectedGap) - RECOMMAND_STAGE_TRANSFER;
                if (!enterScope(targetScope, projectedGapSign, targetOpenness))
                    return;
            }
        } else {
            const double projectedProgress = std::clamp(m_gestureSession.signedProgress + sideSpaceDelta, -MAX_STAGE_PROGRESS, MAX_STAGE_PROGRESS);
            const int    projectedSign = signedUnit(projectedProgress);

            if (projectedSign == 0) {
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else if (projectedSign == currentSign) {
                m_gestureSession.signedProgress = static_cast<double>(currentSign) * clampUnit(std::abs(projectedProgress));
                syncCurrentScopeProgress();
            } else if (!m_gestureSession.allowRecommandTransfer) {
                m_gestureSession.hiddenGapProgress = 0.0;
                m_gestureSession.signedProgress = 0.0;
                m_gestureSession.openness = 0.0;
            } else {
                const double overflow = std::abs(projectedProgress);
                if (overflow < RECOMMAND_STAGE_TRANSFER) {
                    m_gestureSession.signedProgress = 0.0;
                    m_gestureSession.hiddenGapProgress = static_cast<double>(projectedSign) * overflow;
                    m_gestureSession.openness = 0.0;
                } else {
                    const auto targetScope = projectedSign > 0 ? ScopeOverride::ForceAll : m_gestureSession.compactScope;
                    const double targetOpenness = overflow - RECOMMAND_STAGE_TRANSFER;
                    if (!enterScope(targetScope, projectedSign, targetOpenness))
                        return;
                }
            }
        }

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] recommand gesture update openness=" << m_gestureSession.openness << " signed=" << m_gestureSession.signedProgress
                << " gap=" << m_gestureSession.hiddenGapProgress << " aligned=" << alignedDelta << " scope=";
            switch (m_gestureSession.requestedScope) {
                case ScopeOverride::ForceAll:
                    out << "forceall";
                    break;
                case ScopeOverride::OnlyCurrentWorkspace:
                    out << "onlycurrentworkspace";
                    break;
                case ScopeOverride::Default:
                default:
                    out << "default";
                    break;
            }
            debugLog(out.str());
        }

        damageOwnedMonitors();
        return;
    }

    m_gestureSession.lastAlignedSpeed = alignedDelta;
    m_gestureSession.openness = clampUnit(m_gestureSession.openness + (m_gestureSession.opening ? alignedDelta : -alignedDelta) / gestureSwipeDistance());

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture update openness=" << m_gestureSession.openness << " aligned=" << alignedDelta;
        debugLog(out.str());
    }

    damageOwnedMonitors();
}

void OverviewController::endTrackpadGesture(bool cancelled) {
    if (!m_gestureSession.active)
        return;

    const GestureSession gesture = m_gestureSession;

    if (gesture.recommand) {
        const double speedThreshold = gestureForceSpeedThreshold();
        const int    commitDirection =
            resolveRecommandGestureCommitDirection(gesture.signedProgress, gesture.opening, gesture.lastAlignedSpeed, speedThreshold, cancelled);

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] recommand gesture end cancelled=" << (cancelled ? 1 : 0) << " target=" << commitDirection << " openness=" << gesture.openness
                << " signed=" << gesture.signedProgress << " lastAligned=" << gesture.lastAlignedSpeed;
            debugLog(out.str());
        }

        if (commitDirection == 0) {
            if (!gesture.opening) {
                m_gestureSession = {};
                beginClose(CloseMode::Normal, gesture.openness, true);
                return;
            }

            m_gestureSession = {};
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
            damageOwnedMonitors();
            return;
        }

        const auto targetScope = commitDirection > 0 ? ScopeOverride::ForceAll : gesture.compactScope;
        if (m_state.collectionPolicy.requestedScope != targetScope && !retargetGestureScope(targetScope)) {
            m_gestureSession = {};
            return;
        }

        m_gestureSession = {};
        m_deactivatePending = false;
        m_state.phase = Phase::Opening;
        m_state.animationProgress = 0.0;
        m_state.animationFromVisual = gesture.openness;
        m_state.animationToVisual = 1.0;
        m_state.animationStart = {};
        damageOwnedMonitors();
        return;
    }

    const double speedThreshold = gestureForceSpeedThreshold();
    const bool   commit = resolveOverviewGestureCommit(gesture.opening, gesture.openness, gesture.lastAlignedSpeed, speedThreshold, cancelled);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] gesture end mode=" << (gesture.opening ? "open" : "close") << " cancelled=" << (cancelled ? 1 : 0) << " commit=" << (commit ? 1 : 0)
            << " openness=" << gesture.openness << " lastAligned=" << gesture.lastAlignedSpeed;
        debugLog(out.str());
    }

    if (!gesture.opening && commit) {
        m_gestureSession = {};
        beginClose(CloseMode::Normal, gesture.openness, true);
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

    damageOwnedMonitors();
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

    damageOwnedMonitors();
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
            damageOwnedMonitors();
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

    damageOwnedMonitors();

    if (gestureSwipeForeverEnabled() && std::abs(m_workspaceTransition.delta) >= m_workspaceTransition.distance - 0.5)
        requestOverviewWorkspaceTransitionCommit(true);
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

    damageOwnedMonitors();
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
        updateHoveredFromPointer(false, false, false, false, "workspace-transition-revert");
        damageOwnedMonitors();
        return;
    }

    requestOverviewWorkspaceTransitionCommit(false);
}

void OverviewController::requestOverviewWorkspaceTransitionCommit(bool followGesture) {
    if (!m_workspaceTransition.active)
        return;

    if (!(g_pHyprOpenGL && g_pHyprOpenGL->m_renderData.pMonitor)) {
        commitOverviewWorkspaceTransition(followGesture);
        return;
    }

    m_pendingWorkspaceTransitionCommitFollowGesture = m_pendingWorkspaceTransitionCommitFollowGesture || followGesture;
    if (m_workspaceTransitionCommitScheduled)
        return;
    if (!g_pEventLoopManager) {
        commitOverviewWorkspaceTransition(m_pendingWorkspaceTransitionCommitFollowGesture);
        return;
    }

    m_workspaceTransitionCommitScheduled = true;
    const auto generation = ++m_workspaceTransitionCommitGeneration;

    if (debugLogsEnabled())
        debugLog("[hymission] defer overview workspace transition commit until after render");

    g_pEventLoopManager->doLater([this, generation] {
        if (g_controller != this || generation != m_workspaceTransitionCommitGeneration)
            return;

        m_workspaceTransitionCommitScheduled = false;
        if (!m_workspaceTransition.active)
            return;

        const bool followGesture = m_pendingWorkspaceTransitionCommitFollowGesture;
        m_pendingWorkspaceTransitionCommitFollowGesture = false;

        // Workspace transition commit mutates live workspace/window ownership.
        // If a frame is still rendering, reschedule instead of tearing the render
        // state mid-frame.
        if (g_pHyprOpenGL && g_pHyprOpenGL->m_renderData.pMonitor) {
            requestOverviewWorkspaceTransitionCommit(followGesture);
            return;
        }

        commitOverviewWorkspaceTransition(followGesture);
    });
}

void OverviewController::commitOverviewWorkspaceTransition(bool followGesture) {
    if (!m_workspaceTransition.active || !m_workspaceTransition.monitor)
        return;

    clearPendingWindowGeometryRetry();

    const auto transitionMonitor = m_workspaceTransition.monitor;
    const auto oldWorkspace = transitionMonitor->m_activeWorkspace;
    const auto targetWorkspaceId = m_workspaceTransition.targetWorkspaceId;
    const bool targetWorkspaceSyntheticEmpty = m_workspaceTransition.targetWorkspaceSyntheticEmpty;
    const auto targetWorkspaceName = m_workspaceTransition.targetWorkspaceName;
    State      next = m_workspaceTransition.targetState;

    auto targetWorkspace = g_pCompositor->getWorkspaceByID(targetWorkspaceId);
    if (!targetWorkspace && targetWorkspaceSyntheticEmpty) {
        targetWorkspace = g_pCompositor->createNewWorkspace(targetWorkspaceId, transitionMonitor->m_id, targetWorkspaceName);
    }
    if (!targetWorkspace) {
        clearOverviewWorkspaceTransition();
        damageOwnedMonitors();
        return;
    }

    const bool temporarilyDisabledAnimations = !m_animationsEnabledOverridden;
    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(true);

    m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
    {
        ScopedFlag applyingWorkspaceTransitionCommit(m_applyingWorkspaceTransitionCommit);

        transitionMonitor->changeWorkspace(targetWorkspace, true, true, true);

        if (oldWorkspace && oldWorkspace != targetWorkspace) {
            for (const auto& window : g_pCompositor->m_windows) {
                if (!window || window->m_workspace != oldWorkspace || !window->m_pinned)
                    continue;

                // Match Hyprland's native changeworkspace ordering: the monitor's
                // active workspace flips first, then pinned windows follow.
                window->layoutTarget()->assignToSpace(targetWorkspace->m_space);
            }
        }

        // `internal=true` skips Hyprland's workspace IN animation, so the target
        // workspace can retain its old off-screen renderOffset (e.g. +/- one
        // monitor height). Normalize it immediately or the new active workspace
        // remains visually shifted after the overview transition commits.
        targetWorkspace->m_renderOffset->setValueAndWarp(Vector2D{});
        targetWorkspace->m_alpha->setValueAndWarp(1.F);
        g_layoutManager->recalculateMonitor(transitionMonitor);
        if (g_pAnimationManager)
            g_pAnimationManager->frameTick();

        if (targetWorkspaceSyntheticEmpty || !containsHandle(next.managedWorkspaces, targetWorkspace) || next.ownerWorkspace != targetWorkspace) {
            const auto rebuildMonitor = m_state.ownerMonitor ? m_state.ownerMonitor : transitionMonitor;
            const std::vector<WorkspaceOverride> overrides = {{
                .monitorId = transitionMonitor->m_id,
                .workspace = targetWorkspace,
                .workspaceId = targetWorkspaceId,
                .workspaceName = targetWorkspaceName,
                .syntheticEmpty = false,
            }};

            if (State rebuilt = buildState(rebuildMonitor, m_state.collectionPolicy.requestedScope, overrides, true); !rebuilt.participatingMonitors.empty())
                next = std::move(rebuilt);
        }

        next.phase = Phase::Active;
        next.focusBeforeOpen = m_state.focusBeforeOpen;
        next.pendingExitFocus = m_state.pendingExitFocus;
        next.closeMode = m_state.closeMode;
        next.relayoutActive = false;
        next.relayoutProgress = 1.0;
        next.relayoutStart = {};

        clearOverviewWorkspaceTransition();
        carryOverWorkspaceStripSnapshots(next, m_state);
        m_state = std::move(next);
        applyWorkspaceNameOverrides(m_state);
        refreshWorkspaceStripSnapshots();
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
    }

    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(false);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview workspace transition commit target=" << targetWorkspace->m_name << " followGesture=" << (followGesture ? 1 : 0);
        debugLog(out.str());
    }

    if (m_rebuildVisibleStateAfterWorkspaceTransitionCommit) {
        m_rebuildVisibleStateAfterWorkspaceTransitionCommit = false;
        rebuildVisibleState();
    } else {
        updateHoveredFromPointer(false, false, false, false, "workspace-transition-commit");
    }
    damageOwnedMonitors();
}

void OverviewController::clearOverviewWorkspaceTransition() {
    clearPendingWindowGeometryRetry();
    m_workspaceTransitionCommitScheduled = false;
    m_pendingWorkspaceTransitionCommitFollowGesture = false;
    ++m_workspaceTransitionCommitGeneration;
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

    if (hookFunction("handleGesture", "CConfigManager::handleGesture(", m_handleGestureHook, reinterpret_cast<void*>(&hkHandleGesture))) {
        if (m_handleGestureHook->hook()) {
            m_handleGestureOriginal = reinterpret_cast<HandleGestureFn>(m_handleGestureHook->m_original);
        } else {
            notify("[hymission] gesture config hook unavailable; dispatcher controls still work", CHyprColor(1.0, 0.65, 0.2, 1.0), 4000);
            HyprlandAPI::removeFunctionHook(m_handle, m_handleGestureHook);
            m_handleGestureHook = nullptr;
            m_handleGestureOriginal = nullptr;
        }
    } else {
        notify("[hymission] gesture config hook not found; dispatcher controls still work", CHyprColor(1.0, 0.65, 0.2, 1.0), 4000);
    }

    if (!hookFunction("shouldRenderWindow",
                      "CHyprRenderer::shouldRenderWindow(Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow>, Hyprutils::Memory::CSharedPointer<CMonitor>)",
                      m_shouldRenderWindowHook, reinterpret_cast<void*>(&hkShouldRenderWindow))) {
        notify("[hymission] failed to hook shouldRenderWindow(window, monitor)", CHyprColor(1.0, 0.2, 0.2, 1.0), 4000);
        return false;
    }

    (void)hookFunction("renderLayer", "CHyprRenderer::renderLayer(", m_renderLayerHook, reinterpret_cast<void*>(&hkRenderLayer));

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
    m_renderLayerOriginal = nullptr;
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
    if (m_renderLayerHook) {
        if (m_renderLayerHook->hook()) {
            m_renderLayerOriginal = reinterpret_cast<RenderLayerFn>(m_renderLayerHook->m_original);
        } else {
            if (debugLogsEnabled())
                debugLog("[hymission] optional hook activation failed: renderLayer");
            HyprlandAPI::removeFunctionHook(m_handle, m_renderLayerHook);
            m_renderLayerHook = nullptr;
            m_renderLayerOriginal = nullptr;
        }
    }
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
    if (m_renderLayerHook)
        m_renderLayerHook->unhook();
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
    m_renderLayerOriginal = nullptr;
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

bool OverviewController::insideRenderLifecycle() const {
    return m_surfaceRenderDataTransformDepth > 0 || m_stripSnapshotRenderDepth > 0 || (g_pHyprOpenGL && g_pHyprOpenGL->m_renderData.pMonitor);
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

bool OverviewController::windowHasUsableStateGeometry(const PHLWINDOW& window) const {
    if (!window)
        return false;

    if (hasUsableWindowSize(window->m_realSize->value()))
        return true;

    return hasUsableWindowSize(window->m_realSize->goal());
}

bool OverviewController::windowMatchesOverviewScope(const PHLWINDOW& window, const State& state, bool requireUsableGeometry) const {
    if (!window)
        return false;

    if (!window->m_isMapped || window->m_fadingOut || window->isHidden())
        return false;

    if (requireUsableGeometry && !windowHasUsableStateGeometry(window))
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

bool OverviewController::shouldAutoCloseFor(const PHLWINDOW& window) const {
    if (!window || !m_state.ownerMonitor)
        return false;

    if (hasManagedWindow(window))
        return true;

    return windowMatchesOverviewScope(window, m_state, false);
}

bool OverviewController::shouldManageWindow(const PHLWINDOW& window, const State& state) const {
    return windowMatchesOverviewScope(window, state, true);
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

        if (!windowHasUsableStateGeometry(window)) {
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

Rect OverviewController::workspaceStripBandRectForMonitor(const PHLMONITOR& monitor, const State& state) const {
    if (!monitor || !workspaceStripEnabled(state))
        return {};

    const auto reservation =
        reserveWorkspaceStripBand(makeRect(monitor->m_position.x, monitor->m_position.y, monitor->m_size.x, monitor->m_size.y),
                                  parseWorkspaceStripAnchor(workspaceStripAnchor()), workspaceStripThickness(monitor), workspaceStripGap());
    return makeRect(reservation.band.x, reservation.band.y, reservation.band.width, reservation.band.height);
}

Rect OverviewController::overviewContentRectForMonitor(const PHLMONITOR& monitor, const State& state) const {
    if (!monitor)
        return {};

    if (!workspaceStripEnabled(state))
        return makeRect(0.0, 0.0, monitor->m_size.x, monitor->m_size.y);

    const auto reservation = reserveWorkspaceStripBand(makeRect(0.0, 0.0, monitor->m_size.x, monitor->m_size.y),
                                                       parseWorkspaceStripAnchor(workspaceStripAnchor()), workspaceStripThickness(monitor), workspaceStripGap());
    return makeRect(reservation.content.x, reservation.content.y, reservation.content.width, reservation.content.height);
}

Vector2D OverviewController::stripThumbnailPreviewOffset(const PHLMONITOR& monitor, const State& state) const {
    if (!monitor || !workspaceStripEnabled(state))
        return {};

    const Rect previewArea = overviewContentRectForMonitor(monitor, state);
    const Rect fullArea = makeRect(0.0, 0.0, monitor->m_size.x, monitor->m_size.y);
    return Vector2D{
        (previewArea.x + previewArea.width * 0.5) - (fullArea.x + fullArea.width * 0.5),
        (previewArea.y + previewArea.height * 0.5) - (fullArea.y + fullArea.height * 0.5),
    };
}

std::vector<Rect> OverviewController::stripRects() const {
    std::vector<Rect> rects;
    rects.reserve(m_state.stripEntries.size());

    for (const auto& entry : m_state.stripEntries)
        rects.push_back(animatedWorkspaceStripRect(entry.rect, entry.monitor));

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
    if (m_stripPreviewContext.active)
        return managedWindowFor(m_stripPreviewContext.state, window, true);

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
    if (m_stripPreviewContext.active) {
        const auto* managed = managedWindowFor(m_stripPreviewContext.state, window, true);
        return managed && managed->targetMonitor ? managed->targetMonitor : PHLMONITOR{};
    }

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

    return sceneGlobalRectForWindow(window);
}

Rect OverviewController::goalGlobalRectForWindow(const PHLWINDOW& window) const {
    if (!window)
        return {};

    return sceneGlobalRectForWindow(window, true);
}

bool OverviewController::shouldUseGoalGeometryForStateSnapshot(const PHLWINDOW& window) const {
    if (!window)
        return false;

    if (window->m_workspace && !window->m_workspace->isVisible())
        return true;

    if (hasUsableWindowSize(window->m_realSize->value()))
        return false;

    return hasUsableWindowSize(window->m_realSize->goal());
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

    Rect current;
    if (m_stripPreviewContext.active) {
        // Strip thumbnails should snapshot the fully-open mini preview, not the
        // main overview's current animation frame. In strip-only openings from
        // an empty workspace, using the opening snapshot geometry leaves hidden
        // workspaces at their off-screen render offsets and the thumbnail falls
        // back to wallpaper-only until another refresh happens.
        current = managed->targetGlobal;
    } else {
        current = workspaceTransitionRectForWindow(window).value_or(currentPreviewRect(*managed));
    }
    const Rect   actual = surfaceRenderGlobalRectForWindow(window);
    const double actualWidth = std::max(1.0, actual.width);
    const double actualHeight = std::max(1.0, actual.height);
    const double uniformScale = std::max(0.0, std::min(current.width / actualWidth, current.height / actualHeight));
    const Rect   fitted = makeRect(current.centerX() - actualWidth * uniformScale * 0.5, current.centerY() - actualHeight * uniformScale * 0.5,
                                   actualWidth * uniformScale, actualHeight * uniformScale);
    return WindowTransform{
        .actualGlobal = actual,
        .targetGlobal = fitted,
        .scaleX = uniformScale,
        .scaleY = uniformScale,
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
    if (!renderData.dontRound && renderData.rounding > 0) {
        double scale = std::max(0.0, std::min(std::abs(transform->scaleX), std::abs(transform->scaleY)));

        // Strip snapshots render the workspace into a smaller framebuffer first.
        // Match window rounding to that extra downscale so mini previews do not
        // keep the full-size corner radius.
        if (m_stripPreviewContext.active) {
            const auto   fbSize = m_stripPreviewContext.framebufferSize;
            const double monitorPixelWidth = std::max(1.0, static_cast<double>(monitor->m_size.x) * renderScaleForMonitor(monitor));
            const double monitorPixelHeight = std::max(1.0, static_cast<double>(monitor->m_size.y) * renderScaleForMonitor(monitor));
            const double fbScale =
                std::clamp(std::min(fbSize.x / monitorPixelWidth, fbSize.y / monitorPixelHeight), 0.0, 1.0);
            scale *= fbScale;
        }

        renderData.rounding = std::max(0, static_cast<int>(std::lround(static_cast<double>(renderData.rounding) * scale)));
        renderData.dontRound = renderData.rounding <= 0;
    }

    // Keep overview previews independent from normal-layout monitor clipping.
    renderData.clipBox = {};

    return true;
}

bool OverviewController::adjustTransformedSurfaceBoxSize(const CSurfacePassElement::SRenderData& renderData, const PHLMONITOR& monitor, CBox& box) const {
    if (renderData.mainSurface)
        return false;

    const auto transform = windowTransformFor(renderData.pWindow, monitor);
    if (!transform)
        return false;

    Vector2D baseSize{box.width, box.height};
    if (renderData.surface) {
        if (renderData.surface->m_current.viewport.hasDestination)
            baseSize = renderData.surface->m_current.viewport.destination;
        else if (renderData.surface->m_current.viewport.hasSource)
            baseSize = renderData.surface->m_current.viewport.source.size();
        else
            baseSize = renderData.surface->m_current.size;
    }

    box.width = std::max(1.0, baseSize.x * transform->scaleX);
    box.height = std::max(1.0, baseSize.y * transform->scaleY);
    return true;
}

double OverviewController::hiddenStripLayerProgress(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (!shouldHideLayerSurface(layer, monitor))
        return 0.0;

    return clampUnit(visualProgress());
}

void OverviewController::clearHiddenStripLayerProxies() {
    m_hiddenStripLayerProxies.clear();
}

OverviewController::HiddenStripLayerProxy* OverviewController::hiddenStripLayerProxyFor(const PHLLS& layer, const PHLMONITOR& monitor) {
    const auto it = std::find_if(m_hiddenStripLayerProxies.begin(), m_hiddenStripLayerProxies.end(),
                                 [&](const HiddenStripLayerProxy& proxy) { return proxy.layer == layer && proxy.monitor == monitor; });
    return it == m_hiddenStripLayerProxies.end() ? nullptr : &*it;
}

const OverviewController::HiddenStripLayerProxy* OverviewController::hiddenStripLayerProxyFor(const PHLLS& layer, const PHLMONITOR& monitor) const {
    const auto it = std::find_if(m_hiddenStripLayerProxies.begin(), m_hiddenStripLayerProxies.end(),
                                 [&](const HiddenStripLayerProxy& proxy) { return proxy.layer == layer && proxy.monitor == monitor; });
    return it == m_hiddenStripLayerProxies.end() ? nullptr : &*it;
}

bool OverviewController::captureHiddenStripLayerProxy(const PHLLS& layer, const PHLMONITOR& monitor) {
    if (!layer || !monitor || !g_pHyprRenderer || !g_pHyprOpenGL || !shouldHideLayerSurface(layer, monitor))
        return false;

    constexpr double kHiddenStripBlurPaddingLogical = 24.0;
    const Rect capturedRectGlobal = makeRect(layer->m_geometry.x, layer->m_geometry.y, layer->m_geometry.w, layer->m_geometry.h);
    const Rect proxyRectGlobal =
        makeRect(capturedRectGlobal.x - kHiddenStripBlurPaddingLogical, capturedRectGlobal.y - kHiddenStripBlurPaddingLogical,
                 capturedRectGlobal.width + kHiddenStripBlurPaddingLogical * 2.0, capturedRectGlobal.height + kHiddenStripBlurPaddingLogical * 2.0);
    if (capturedRectGlobal.width <= 1.0 || capturedRectGlobal.height <= 1.0) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar capture skipped namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                << " captured=" << rectToString(capturedRectGlobal);
            debugLog(out.str());
        }
        return false;
    }

    const int fbWidth = std::max(1, static_cast<int>(std::ceil(proxyRectGlobal.width * renderScaleForMonitor(monitor))));
    const int fbHeight = std::max(1, static_cast<int>(std::ceil(proxyRectGlobal.height * renderScaleForMonitor(monitor))));

    g_pHyprRenderer->makeEGLCurrent();
    g_pHyprRenderer->makeSnapshot(layer);
    auto* sourceFramebuffer = layerFramebufferFor(layer);
    if (!sourceFramebuffer || !sourceFramebuffer->isAllocated() || !sourceFramebuffer->getTexture()) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar capture missing source namespace=" << layer->m_namespace << " monitor=" << monitor->m_name;
            debugLog(out.str());
        }
        return false;
    }

    auto* existing = hiddenStripLayerProxyFor(layer, monitor);
    if (!existing) {
        HiddenStripLayerProxy proxy;
        proxy.layer = layer;
        proxy.monitor = monitor;
        proxy.capturedRectGlobal = capturedRectGlobal;
        proxy.proxyRectGlobal = proxyRectGlobal;
        proxy.snapshotSize = Vector2D{static_cast<double>(fbWidth), static_cast<double>(fbHeight)};
        proxy.framebuffer = makeShared<CFramebuffer>();
        for (auto& blurredFramebuffer : proxy.blurredFramebuffers)
            blurredFramebuffer = makeShared<CFramebuffer>();
        m_hiddenStripLayerProxies.push_back(std::move(proxy));
        existing = &m_hiddenStripLayerProxies.back();
    }

    existing->capturedRectGlobal = capturedRectGlobal;
    existing->proxyRectGlobal = proxyRectGlobal;
    existing->snapshotSize = Vector2D{static_cast<double>(fbWidth), static_cast<double>(fbHeight)};
    if (!existing->framebuffer)
        existing->framebuffer = makeShared<CFramebuffer>();
    for (auto& blurredFramebuffer : existing->blurredFramebuffers) {
        if (!blurredFramebuffer)
            blurredFramebuffer = makeShared<CFramebuffer>();
    }

    if (!existing->framebuffer->isAllocated() || std::abs(existing->framebuffer->m_size.x - fbWidth) > 0.5 || std::abs(existing->framebuffer->m_size.y - fbHeight) > 0.5) {
        existing->framebuffer->release();
        if (!existing->framebuffer->alloc(fbWidth, fbHeight)) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip-bar capture framebuffer alloc failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                    << " fb=(" << fbWidth << "x" << fbHeight << ")";
                debugLog(out.str());
            }
            return false;
        }
        setFramebufferLinearFiltering(*existing->framebuffer);
    }

    for (auto& blurredFramebuffer : existing->blurredFramebuffers) {
        if (!blurredFramebuffer->isAllocated() || std::abs(blurredFramebuffer->m_size.x - fbWidth) > 0.5 || std::abs(blurredFramebuffer->m_size.y - fbHeight) > 0.5) {
            blurredFramebuffer->release();
            if (!blurredFramebuffer->alloc(fbWidth, fbHeight)) {
                if (debugLogsEnabled()) {
                    std::ostringstream out;
                    out << "[hymission] strip-bar capture blur framebuffer alloc failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                        << " fb=(" << fbWidth << "x" << fbHeight << ")";
                    debugLog(out.str());
                }
                return false;
            }
            setFramebufferLinearFiltering(*blurredFramebuffer);
        }
    }

    const double monitorRenderWidth = std::max(1.0, static_cast<double>(monitor->m_size.x) * renderScaleForMonitor(monitor));
    const double monitorRenderHeight = std::max(1.0, static_cast<double>(monitor->m_size.y) * renderScaleForMonitor(monitor));
    const Rect   proxyRectRenderLocal = rectToMonitorRenderLocal(proxyRectGlobal, monitor);
    const Rect   capturedRectRenderLocal = rectToMonitorRenderLocal(capturedRectGlobal, monitor);
    const double targetOffsetX = capturedRectRenderLocal.x - proxyRectRenderLocal.x;
    const double targetOffsetY = capturedRectRenderLocal.y - proxyRectRenderLocal.y;
    constexpr double kSnapshotSizeTolerance = 2.0;
    const bool       sourceMatchesProxy =
        std::abs(sourceFramebuffer->m_size.x - static_cast<double>(fbWidth)) <= kSnapshotSizeTolerance &&
        std::abs(sourceFramebuffer->m_size.y - static_cast<double>(fbHeight)) <= kSnapshotSizeTolerance;
    const bool sourceMatchesCaptured =
        std::abs(sourceFramebuffer->m_size.x - capturedRectRenderLocal.width) <= kSnapshotSizeTolerance &&
        std::abs(sourceFramebuffer->m_size.y - capturedRectRenderLocal.height) <= kSnapshotSizeTolerance;
    const bool sourceMatchesMonitor =
        std::abs(sourceFramebuffer->m_size.x - monitorRenderWidth) <= kSnapshotSizeTolerance &&
        std::abs(sourceFramebuffer->m_size.y - monitorRenderHeight) <= kSnapshotSizeTolerance;

    Rect         sourceRect = makeRect(0.0, 0.0, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y);
    Rect         targetRect = makeRect(targetOffsetX, targetOffsetY, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y);
    CFramebuffer croppedFramebuffer;
    bool         useCroppedFramebuffer = false;
    if (sourceMatchesProxy) {
        targetRect = makeRect(0.0, 0.0, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y);
    } else if (sourceMatchesCaptured) {
        targetRect = makeRect(targetOffsetX, targetOffsetY, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y);
    } else if (sourceMatchesMonitor) {
        const double sourceX = std::clamp(capturedRectRenderLocal.x * sourceFramebuffer->m_size.x / monitorRenderWidth, 0.0,
                                          std::max(0.0, sourceFramebuffer->m_size.x - capturedRectRenderLocal.width));
        const double sourceY = std::clamp(capturedRectRenderLocal.y * sourceFramebuffer->m_size.y / monitorRenderHeight, 0.0,
                                          std::max(0.0, sourceFramebuffer->m_size.y - capturedRectRenderLocal.height));
        const int croppedWidth = std::max(1, static_cast<int>(std::lround(capturedRectRenderLocal.width)));
        const int croppedHeight = std::max(1, static_cast<int>(std::lround(capturedRectRenderLocal.height)));
        if (!croppedFramebuffer.alloc(croppedWidth, croppedHeight)) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip-bar capture cropped framebuffer alloc failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                    << " fb=(" << croppedWidth << "x" << croppedHeight << ")";
                debugLog(out.str());
            }
            return false;
        }
        setFramebufferLinearFiltering(croppedFramebuffer);

        if (!renderTextureIntoFramebuffer(monitor, croppedFramebuffer, sourceFramebuffer->getTexture(),
                                          CBox{-sourceX, -sourceY, sourceFramebuffer->m_size.x, sourceFramebuffer->m_size.y})) {
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] strip-bar capture cropped blit failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                    << " sourceOffset=(" << sourceX << "," << sourceY << ")";
                debugLog(out.str());
            }
            return false;
        }

        sourceRect = makeRect(0.0, 0.0, croppedFramebuffer.m_size.x, croppedFramebuffer.m_size.y);
        targetRect = makeRect(targetOffsetX, targetOffsetY, capturedRectRenderLocal.width, capturedRectRenderLocal.height);
        useCroppedFramebuffer = true;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] strip-bar capture namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
            << " captured=" << rectToString(capturedRectGlobal) << " proxy=" << rectToString(proxyRectGlobal)
            << " capturedRender=" << rectToString(capturedRectRenderLocal) << " proxyRender=" << rectToString(proxyRectRenderLocal)
            << " targetOffset=(" << targetOffsetX << "," << targetOffsetY << ")"
            << " sourceFb=(" << sourceFramebuffer->m_size.x << "x" << sourceFramebuffer->m_size.y << ")"
            << " fb=(" << fbWidth << "x" << fbHeight << ")"
            << " matchProxy=" << sourceMatchesProxy << " matchCaptured=" << sourceMatchesCaptured << " matchMonitor=" << sourceMatchesMonitor
            << " sourceRect=" << rectToString(sourceRect) << " targetRect=" << rectToString(targetRect);
        debugLog(out.str());
    }

    auto& blitSourceFramebuffer = useCroppedFramebuffer ? croppedFramebuffer : *sourceFramebuffer;
    if (!blitFramebufferRegion(blitSourceFramebuffer, *existing->framebuffer, sourceRect, targetRect)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar capture blit failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name
                << " sourceRect=" << rectToString(sourceRect) << " targetRect=" << rectToString(targetRect);
            debugLog(out.str());
        }
        return false;
    }

    if (!buildBlurredProxyFramebuffers(existing->framebuffer, existing->blurredFramebuffers)) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar capture blur build failed namespace=" << layer->m_namespace << " monitor=" << monitor->m_name;
            debugLog(out.str());
        }
        return true;
    }

    return true;
}

void OverviewController::syncHiddenStripLayerProxies() {
    if (!isVisible() || !workspaceStripEnabled(m_state) || !hideBarsWhenStripShownEnabled() || !hideBarAnimationEffectsEnabled()) {
        clearHiddenStripLayerProxies();
        return;
    }

    std::vector<std::pair<PHLLS, PHLMONITOR>> desired;
    for (const auto& layer : g_pCompositor->m_layers) {
        const auto monitor = layer ? layer->m_monitor.lock() : PHLMONITOR{};
        if (!shouldHideLayerSurface(layer, monitor))
            continue;

        desired.emplace_back(layer, monitor);
        const bool captured = captureHiddenStripLayerProxy(layer, monitor);
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar sync namespace=" << layer->m_namespace << " monitor="
                << (monitor ? monitor->m_name : std::string("<null-monitor>")) << " captured=" << (captured ? 1 : 0);
            debugLog(out.str());
        }
        if (!captured) {
            std::erase_if(m_hiddenStripLayerProxies, [&](const HiddenStripLayerProxy& proxy) { return proxy.layer == layer && proxy.monitor == monitor; });
        }
    }

    m_hiddenStripLayerProxies.erase(std::remove_if(m_hiddenStripLayerProxies.begin(), m_hiddenStripLayerProxies.end(),
                                                   [&](const HiddenStripLayerProxy& proxy) {
                                                       return std::none_of(desired.begin(), desired.end(), [&](const auto& entry) {
                                                           return entry.first == proxy.layer && entry.second == proxy.monitor;
                                                       });
                                                   }),
                                    m_hiddenStripLayerProxies.end());
}

Rect OverviewController::hiddenStripLayerProxyRect(const HiddenStripLayerProxy& proxy) const {
    const double hiddenness = hiddenStripLayerProgress(proxy.layer, proxy.monitor);
    const auto   anchor = parseWorkspaceStripAnchor(workspaceStripAnchor());
    const double scaleTarget = 1.0 / hideBarAnimationScaleDivisor();
    const double scale = 1.0 - (1.0 - scaleTarget) * hiddenness;
    const Rect   stripBand = workspaceStripBandRectForMonitor(proxy.monitor, m_state);
    const double moveMultiplier = hideBarAnimationMoveMultiplier();

    Rect rect = scaleRectFromAnchor(proxy.proxyRectGlobal, proxy.capturedRectGlobal, anchor, scale, scale);
    switch (anchor) {
        case WorkspaceStripAnchor::Left:
            rect = translateRect(rect, stripBand.width * hiddenness * moveMultiplier, 0.0);
            break;
        case WorkspaceStripAnchor::Right:
            rect = translateRect(rect, -stripBand.width * hiddenness * moveMultiplier, 0.0);
            break;
        case WorkspaceStripAnchor::Top:
        default:
            rect = translateRect(rect, 0.0, stripBand.height * hiddenness * moveMultiplier);
            break;
    }

    return rect;
}

bool OverviewController::shouldRenderHiddenStripLayerProxy(const PHLLS& layer, const PHLMONITOR& monitor) const {
    if (!shouldHideLayerSurface(layer, monitor))
        return false;

    // Closing completion schedules deferred deactivate before the overlay pass
    // is emitted again. Hand rendering back to the real layer immediately so
    // there is no one-frame gap where both the live bar and proxy are absent.
    if (m_deactivatePending)
        return false;

    const auto* proxy = hiddenStripLayerProxyFor(layer, monitor);
    auto*       framebuffer = proxy && proxy->framebuffer ? proxy->framebuffer.get() : nullptr;
    return proxy && framebuffer && framebuffer->isAllocated() && framebuffer->getTexture();
}

void OverviewController::renderHiddenStripLayerProxies() const {
    if (m_hiddenStripLayerProxies.empty() || !g_pHyprOpenGL || !hideBarAnimationEffectsEnabled())
        return;

    const auto renderMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    for (const auto& proxy : m_hiddenStripLayerProxies) {
        if (!proxy.layer || !proxy.monitor || proxy.monitor != renderMonitor)
            continue;
        if (!shouldHideLayerSurface(proxy.layer, renderMonitor))
            continue;

        auto* sourceFramebuffer = proxy.framebuffer ? proxy.framebuffer.get() : nullptr;
        if (!sourceFramebuffer || !sourceFramebuffer->isAllocated() || !sourceFramebuffer->getTexture())
            continue;

        const double hiddenness = hiddenStripLayerProgress(proxy.layer, renderMonitor);
        const double alphaEnd = hideBarAnimationAlphaEnd();
        const float  proxyAlpha = static_cast<float>(std::clamp(1.0 + (alphaEnd - 1.0) * hiddenness, 0.0, 1.0));
        if (proxyAlpha <= 0.001F)
            continue;

        const float blurStrength =
            hideBarAnimationBlurEnabled() ? static_cast<float>(easeOutCubic(clampUnit((hiddenness - 0.12) / 0.38))) : 0.0F;
        const float sharpAlpha = hideBarAnimationBlurEnabled() ? (proxyAlpha * std::pow(std::max(0.0F, 1.0F - blurStrength), 2.0F)) : proxyAlpha;
        const float blurredAlpha = hideBarAnimationBlurEnabled() ? (proxyAlpha * std::clamp(0.2F + 0.8F * blurStrength, 0.0F, 1.0F)) : 0.0F;
        const Rect  targetRect = rectToMonitorRenderLocal(hiddenStripLayerProxyRect(proxy), renderMonitor);
        if (targetRect.width <= 0.0 || targetRect.height <= 0.0)
            continue;

        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] strip-bar render namespace=" << proxy.layer->m_namespace << " monitor=" << renderMonitor->m_name
                << " hiddenness=" << hiddenness << " proxyAlpha=" << proxyAlpha << " blurStrength=" << blurStrength
                << " proxyRect=" << rectToString(proxy.proxyRectGlobal) << " capturedRect=" << rectToString(proxy.capturedRectGlobal)
                << " targetRect=" << rectToString(targetRect);
            debugLog(out.str());
        }

        if (sharpAlpha > 0.002F)
            g_pHyprOpenGL->renderTexture(sourceFramebuffer->getTexture(), toBox(targetRect), {.a = sharpAlpha});

        if (blurredAlpha <= 0.002F)
            continue;

        constexpr std::size_t blurLevelCount = 4;
        const float           blurLevel = std::clamp(blurStrength * static_cast<float>(blurLevelCount - 1), 0.0F, static_cast<float>(blurLevelCount - 1));
        const auto            lowerIndex = static_cast<std::size_t>(std::floor(blurLevel));
        const auto            upperIndex = std::min(blurLevelCount - 1, lowerIndex + 1);
        const float           upperWeight = std::clamp(blurLevel - static_cast<float>(lowerIndex), 0.0F, 1.0F);
        const float           lowerWeight = 1.0F - upperWeight;

        const auto renderBlurLevel = [&](std::size_t index, float alpha) {
            auto* blurredFramebuffer = index < proxy.blurredFramebuffers.size() && proxy.blurredFramebuffers[index] ? proxy.blurredFramebuffers[index].get() : nullptr;
            if (!blurredFramebuffer || !blurredFramebuffer->isAllocated() || !blurredFramebuffer->getTexture() || alpha <= 0.002F)
                return;
            g_pHyprOpenGL->renderTexture(blurredFramebuffer->getTexture(), toBox(targetRect), {.a = alpha});
        };

        renderBlurLevel(lowerIndex, blurredAlpha * lowerWeight);
        if (upperIndex != lowerIndex)
            renderBlurLevel(upperIndex, blurredAlpha * upperWeight);
    }
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
    if (!renderData || !renderData->pWindow)
        return false;

    monitor = renderData->pMonitor.lock();
    if (!monitor || !isVisible() || !ownsMonitor(monitor) || !hasManagedWindow(renderData->pWindow) || previewMonitorForWindow(renderData->pWindow) != monitor)
        return false;

    snapshot = {
        .pos = renderData->pos,
        .localPos = renderData->localPos,
        .w = renderData->w,
        .h = renderData->h,
        .rounding = renderData->rounding,
        .dontRound = renderData->dontRound,
        .roundingPower = renderData->roundingPower,
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
    renderData->rounding = snapshot.rounding;
    renderData->dontRound = snapshot.dontRound;
    renderData->roundingPower = snapshot.roundingPower;
    renderData->alpha = snapshot.alpha;
    renderData->fadeAlpha = snapshot.fadeAlpha;
    renderData->blur = snapshot.blur;
    renderData->blockBlurOptimization = snapshot.blockBlurOptimization;
    renderData->clipBox = snapshot.clipBox;
}

std::optional<std::size_t> OverviewController::hitTestTarget(double x, double y) const {
    return hitTest(targetRects(), x, y);
}

std::optional<std::size_t> OverviewController::hitTestStripTarget(double x, double y) const {
    return hitTestWorkspaceStrip(stripRects(), x, y);
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

    if ((sourceManaged && sourceManaged->isPinned) || (targetManaged && targetManaged->isPinned)) {
        if (sourceManaged && targetManaged)
            return lerpRect(sourceManaged->targetGlobal, targetManaged->targetGlobal, t);

        return sourceManaged ? sourceManaged->targetGlobal : targetManaged->targetGlobal;
    }

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

double OverviewController::workspaceStripEnterProgress() const {
    return visualProgress();
}

Vector2D OverviewController::workspaceStripEnterOffset(const PHLMONITOR& monitor) const {
    if (!monitor || !workspaceStripEnabled(m_state))
        return {};

    const double progress = workspaceStripEnterProgress();
    if (progress >= 1.0)
        return {};

    const Rect band = workspaceStripBandRectForMonitor(monitor, m_state);
    const double hiddenFraction = 1.0 - progress;

    switch (parseWorkspaceStripAnchor(workspaceStripAnchor())) {
        case WorkspaceStripAnchor::Left:
            return Vector2D{-band.width * hiddenFraction, 0.0};
        case WorkspaceStripAnchor::Right:
            return Vector2D{band.width * hiddenFraction, 0.0};
        case WorkspaceStripAnchor::Top:
        default:
            return Vector2D{0.0, -band.height * hiddenFraction};
    }
}

Rect OverviewController::animatedWorkspaceStripRect(const Rect& rect, const PHLMONITOR& monitor) const {
    const auto offset = workspaceStripEnterOffset(monitor);
    if (offset.x == 0.0 && offset.y == 0.0)
        return rect;

    return translateRect(rect, offset.x, offset.y);
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

    recordWindowActivation(window, true);
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
        logScrollingWorkspaceSpotState("before centerOrFitCol", window->m_workspace, window);
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
        logScrollingWorkspaceSpotState("after centerOrFitCol", window->m_workspace, window);
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
        if (window->m_workspace && isScrollingWorkspace(window->m_workspace))
            logScrollingWorkspaceSpotState("before live focus", window->m_workspace, window);
    }

    const bool temporarilyDisabledAnimations = !m_animationsEnabledOverridden;
    if (temporarilyDisabledAnimations)
        setAnimationsEnabledOverride(true);

    m_pendingLiveFocusWorkspaceChangeTarget = window;
    focusWindowCompat(window, false, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    if (debugLogsEnabled() && window->m_workspace && isScrollingWorkspace(window->m_workspace))
        logScrollingWorkspaceSpotState("after focus before explicit spot sync", window->m_workspace, window);
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

void OverviewController::syncFocusDuringOverviewFromSelection(bool syncScrollingSpot, const char* source) {
    const auto selected = selectedWindow();
    if (!selected)
        return;

    const auto previousSelected = m_state.focusDuringOverview;
    if (m_state.focusDuringOverview != selected && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] overview target " << debugWindowLabel(selected) << " source=" << (source ? source : "?");
        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
            out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        else
            out << " selected=<null>";
        if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
            out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        else
            out << " hovered=<null>";
        out << " pointer=" << vectorToString(g_pInputManager->getMouseCoordsInternal());
        debugLog(out.str());
    }

    m_state.focusDuringOverview = selected;
    latchHoverSelectionAnchor(g_pInputManager->getMouseCoordsInternal());
    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    syncRealFocusDuringOverview(selected, syncScrollingSpot);
    updateSelectedWindowLayout(previousSelected);
}

void OverviewController::queueSelectionRetargetDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot, const char* source) {
    if (!window || !window->m_isMapped || !hasManagedWindow(window))
        return;

    m_queuedOverviewSelectionTarget = window;
    m_queuedOverviewSelectionSyncScrollingSpot = syncScrollingSpot;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] queue selection retarget during overview target=" << debugWindowLabel(window)
            << " source=" << (source ? source : "?")
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0);
        debugLog(out.str());
    }
}

void OverviewController::flushQueuedSelectionRetargetDuringOverview() {
    const auto queuedTarget = m_queuedOverviewSelectionTarget.lock();
    if (!queuedTarget)
        return;

    if (!isVisible() || m_state.phase != Phase::Active || m_gestureSession.active || m_workspaceTransition.active || m_beginCloseInProgress)
        return;

    m_queuedOverviewSelectionTarget.reset();
    const bool syncScrollingSpot = m_queuedOverviewSelectionSyncScrollingSpot;
    m_queuedOverviewSelectionSyncScrollingSpot = false;

    if (!queuedTarget->m_isMapped || !hasManagedWindow(queuedTarget))
        return;

    const auto queuedIt =
        std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == queuedTarget; });
    if (queuedIt == m_state.windows.end())
        return;

    const auto previousSelectedWindow = m_lastLayoutSelectedWindow.lock();
    m_state.selectedIndex = static_cast<std::size_t>(std::distance(m_state.windows.begin(), queuedIt));
    m_state.focusDuringOverview = queuedTarget;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] flush queued selection retarget during overview target=" << debugWindowLabel(queuedTarget)
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0)
            << " previousLayoutSelected=" << debugWindowLabel(previousSelectedWindow);
        debugLog(out.str());
    }

    updateSelectedWindowLayout(previousSelectedWindow);
    queueRealFocusDuringOverview(queuedTarget, syncScrollingSpot, "frame-coalesced");
}

void OverviewController::queueRealFocusDuringOverview(const PHLWINDOW& window, bool syncScrollingSpot, const char* source) {
    if (!window || !window->m_isMapped || !hasManagedWindow(window))
        return;

    m_queuedOverviewLiveFocusTarget = window;
    m_queuedOverviewLiveFocusSyncScrollingSpot = syncScrollingSpot;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] queue real focus during overview target=" << debugWindowLabel(window)
            << " source=" << (source ? source : "?")
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0);
        debugLog(out.str());
    }
}

void OverviewController::flushQueuedRealFocusDuringOverview() {
    const auto queuedTarget = m_queuedOverviewLiveFocusTarget.lock();
    if (!queuedTarget)
        return;

    if (!isVisible() || m_state.phase != Phase::Active || m_gestureSession.active || m_workspaceTransition.active || m_beginCloseInProgress)
        return;

    const bool syncScrollingSpot = m_queuedOverviewLiveFocusSyncScrollingSpot;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;

    if (!queuedTarget->m_isMapped || !hasManagedWindow(queuedTarget))
        return;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] flush queued real focus during overview target=" << debugWindowLabel(queuedTarget)
            << " syncScrollingSpot=" << (syncScrollingSpot ? 1 : 0);
        debugLog(out.str());
    }

    syncRealFocusDuringOverview(queuedTarget, syncScrollingSpot);
}

void OverviewController::updateSelectedWindowLayout(const PHLWINDOW& previousSelectedWindow) {
    if (!expandSelectedWindowEnabled() || !isVisible() || m_state.phase != Phase::Active || m_gestureSession.active || m_workspaceTransition.active)
        return;

    const auto currentSelection = selectedWindow();
    const auto currentSelectedWindow = currentSelection ? currentSelection : Desktop::focusState()->window();
    if (currentSelectedWindow == previousSelectedWindow)
        return;
    m_lastLayoutSelectedWindow = currentSelectedWindow;

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] expand-selected relayout previous=" << debugWindowLabel(previousSelectedWindow)
            << " current=" << debugWindowLabel(currentSelectedWindow);
        if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
            out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        else
            out << " hovered=<null>";
        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
            out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        else
            out << " selected=<null>";
        debugLog(out.str());
        logOverviewLayoutState("before expand-selected relayout", m_state);
    }

    m_hoverSelectionRetargetBlockedUntil = std::chrono::steady_clock::now() + HOVER_SELECTION_RETARGET_COOLDOWN;
    m_hoverSelectionRetargetCandidateIndex.reset();
    m_hoverSelectionRetargetCandidateSince = {};
    m_hoverSelectionRetargetCandidatePrimed = false;

    const auto managedForWindow = [&](const PHLWINDOW& window) -> ManagedWindow* {
        const auto it = std::find_if(m_state.windows.begin(), m_state.windows.end(), [&](const ManagedWindow& managed) { return managed.window == window; });
        return it == m_state.windows.end() ? nullptr : &*it;
    };

    auto* currentManaged = managedForWindow(currentSelectedWindow);
    if (!currentManaged) {
        rebuildVisibleState(currentSelectedWindow, true);
        return;
    }

    const std::size_t currentManagedIndex = static_cast<std::size_t>(currentManaged - m_state.windows.data());
    bool              shouldAnimateRelayout = false;
    std::vector<Rect> baseTargets;
    baseTargets.reserve(m_state.windows.size());
    for (auto& managed : m_state.windows) {
        managed.relayoutFromGlobal = currentPreviewRect(managed);
        if (managed.targetMonitor) {
            managed.targetGlobal =
                makeRect(managed.targetMonitor->m_position.x + managed.slot.target.x, managed.targetMonitor->m_position.y + managed.slot.target.y,
                         managed.slot.target.width, managed.slot.target.height);
        } else {
            managed.targetGlobal = managed.relayoutFromGlobal;
        }
        baseTargets.push_back(managed.targetGlobal);
    }

    if (!currentManaged->targetMonitor) {
        rebuildVisibleState(currentSelectedWindow, true);
        return;
    }

    const Rect boundsLocal = overviewContentRectForMonitor(currentManaged->targetMonitor, m_state);
    const Rect boundsGlobal =
        makeRect(currentManaged->targetMonitor->m_position.x + boundsLocal.x, currentManaged->targetMonitor->m_position.y + boundsLocal.y, boundsLocal.width, boundsLocal.height);
    if (boundsGlobal.width <= 1.0 || boundsGlobal.height <= 1.0) {
        rebuildVisibleState(currentSelectedWindow, true);
        return;
    }

    struct RipplePeer {
        std::size_t index = 0;
        Rect        base;
        double      distance = 0.0;
    };

    const Rect selectedBase = baseTargets[currentManagedIndex];
    const LayoutConfig layoutConfig = loadLayoutConfig();
    const double minGapX = std::max(0.0, layoutConfig.columnSpacing * 0.25);
    const double minGapY = std::max(0.0, layoutConfig.rowSpacing * 0.25);
    const double maxGrowthXPerSide = std::max(0.0, layoutConfig.columnSpacing * 2.0);
    const double maxGrowthYPerSide = std::max(0.0, layoutConfig.rowSpacing * 2.0);
    const double scaleCapByGrowth = maxCenteredScaleForPerSideGrowth(selectedBase, maxGrowthXPerSide, maxGrowthYPerSide);
    const double scaleCapByBounds = maxCenteredScaleForBounds(selectedBase, boundsGlobal);
    const double preferredScale = m_state.windows.size() <= 1 ? 1.0 : SELECTED_WINDOW_LAYOUT_EMPHASIS;
    const double scaleCap = std::max(1.0, std::min({preferredScale, scaleCapByGrowth, scaleCapByBounds}));
    const double rippleRadius =
        std::max(std::hypot(selectedBase.width, selectedBase.height) * 2.5, std::hypot(boundsGlobal.width, boundsGlobal.height) * 0.55);
    const double pressureCenterX = selectedBase.centerX();
    const double pressureCenterY = selectedBase.centerY();

    std::vector<RipplePeer> peers;
    peers.reserve(m_state.windows.size());
    for (std::size_t index = 0; index < m_state.windows.size(); ++index) {
        const auto& managed = m_state.windows[index];
        if (index == currentManagedIndex || managed.targetMonitor != currentManaged->targetMonitor)
            continue;

        const Rect base = baseTargets[index];
        const double distance = std::hypot(base.centerX() - pressureCenterX, base.centerY() - pressureCenterY);
        peers.push_back({.index = index, .base = base, .distance = distance});
    }

    std::stable_sort(peers.begin(), peers.end(), [](const RipplePeer& lhs, const RipplePeer& rhs) { return lhs.distance < rhs.distance; });

    const auto applyTargets = [&](const std::vector<Rect>& targets) {
        for (std::size_t index = 0; index < m_state.windows.size() && index < targets.size(); ++index)
            m_state.windows[index].targetGlobal = targets[index];
    };

    const auto tryApplyScale = [&](double scale, std::vector<Rect>& outTargets, double& outMaxShift) -> bool {
        outTargets = baseTargets;
        outMaxShift = 0.0;

        const Rect selectedTarget = scaleRectAroundCenter(selectedBase, scale);
        if (!rectFitsInsideBounds(selectedTarget, boundsGlobal))
            return false;
        outTargets[currentManagedIndex] = selectedTarget;

        const double pressureGap = std::max({1.0, minGapX, minGapY});
        const double radialPressure =
            std::max(pressureGap, std::hypot(selectedTarget.width - selectedBase.width, selectedTarget.height - selectedBase.height) * 0.5 + pressureGap);

        std::vector<std::size_t> placed;
        placed.reserve(peers.size() + 1);
        placed.push_back(currentManagedIndex);

        const auto overlapsPlaced = [&](const Rect& target) {
            return std::ranges::any_of(placed, [&](std::size_t obstacleIndex) {
                return rectsOverlap(target, inflateRect(outTargets[obstacleIndex], minGapX, minGapY));
            });
        };

        const auto resolveAlongBearing = [&](Rect target, double dirX, double dirY) -> std::optional<Rect> {
            for (std::size_t pass = 0; pass < 6; ++pass) {
                bool changed = false;
                for (const auto obstacleIndex : placed) {
                    const Rect obstacle = inflateRect(outTargets[obstacleIndex], minGapX, minGapY);
                    if (!rectsOverlap(target, obstacle))
                        continue;

                    if (std::abs(dirX) < 0.001 && std::abs(dirY) < 0.001) {
                        dirX = target.centerX() >= pressureCenterX ? 1.0 : -1.0;
                        dirY = target.centerY() >= pressureCenterY ? 1.0 : -1.0;
                        const double length = std::hypot(dirX, dirY);
                        if (length <= 0.001)
                            return std::nullopt;
                        dirX /= length;
                        dirY /= length;
                    }

                    const auto exitDistance = overlapExitDistanceAlongDirection(target, obstacle, dirX, dirY);
                    if (!exitDistance)
                        continue;

                    target = translateRect(target, dirX * *exitDistance, dirY * *exitDistance);
                    target = clampRectInside(target, boundsGlobal);
                    changed = true;
                }

                if (!changed)
                    break;
            }

            if (!rectFitsInsideBounds(target, boundsGlobal) || overlapsPlaced(target))
                return std::nullopt;

            return target;
        };

        for (const auto& peer : peers) {
            const double deltaX = peer.base.centerX() - pressureCenterX;
            const double deltaY = peer.base.centerY() - pressureCenterY;
            double directionX = deltaX;
            double directionY = deltaY;
            const double directionLength = std::hypot(directionX, directionY);
            if (directionLength > 0.001) {
                directionX /= directionLength;
                directionY /= directionLength;
            } else {
                directionX = peer.base.centerX() >= pressureCenterX ? 1.0 : -1.0;
                directionY = peer.base.centerY() >= pressureCenterY ? 1.0 : -1.0;
                const double fallbackLength = std::hypot(directionX, directionY);
                if (fallbackLength <= 0.001)
                    return false;
                directionX /= fallbackLength;
                directionY /= fallbackLength;
            }

            const double influence = clampUnit(1.0 - peer.distance / std::max(1.0, rippleRadius));
            const double easedInfluence = influence * influence;
            Rect target = translateRect(peer.base, directionX * radialPressure * easedInfluence, directionY * radialPressure * easedInfluence);
            target = clampRectInside(target, boundsGlobal);

            auto resolved = resolveAlongBearing(target, directionX, directionY);
            if (!resolved)
                resolved = resolveAlongBearing(clampRectInside(peer.base, boundsGlobal), directionX, directionY);
            if (!resolved)
                return false;

            outTargets[peer.index] = *resolved;
            placed.push_back(peer.index);
            outMaxShift = std::max(outMaxShift, std::hypot(resolved->centerX() - peer.base.centerX(), resolved->centerY() - peer.base.centerY()));
        }

        return true;
    };

    std::vector<Rect> bestTargets = baseTargets;
    std::vector<Rect> candidateTargets;
    double            bestMaxShift = 0.0;
    double            maxShift = 0.0;
    double            appliedScale = 1.0;

    if (tryApplyScale(scaleCap, bestTargets, bestMaxShift)) {
        appliedScale = scaleCap;
        maxShift = bestMaxShift;
    } else {
        const bool baseResolved = tryApplyScale(1.0, bestTargets, bestMaxShift);
        maxShift = baseResolved ? bestMaxShift : 0.0;

        double low = 1.0;
        double high = scaleCap;
        for (std::size_t iteration = 0; iteration < 12 && high - low > 0.001; ++iteration) {
            const double mid = (low + high) * 0.5;
            double       candidateMaxShift = 0.0;
            if (tryApplyScale(mid, candidateTargets, candidateMaxShift)) {
                appliedScale = mid;
                maxShift = candidateMaxShift;
                bestTargets = candidateTargets;
                low = mid;
            } else {
                high = mid;
            }
        }
    }

    applyTargets(bestTargets);

    for (auto& managed : m_state.windows) {
        if (!rectApproxEqual(managed.relayoutFromGlobal, managed.targetGlobal, 0.5))
            shouldAnimateRelayout = true;
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] expand-selected ripple push selected=" << debugWindowLabel(currentSelectedWindow)
            << " peers=" << peers.size()
            << " radius=" << rippleRadius
            << " scale=" << appliedScale
            << " scaleCap=" << scaleCap
            << " growthCap=" << scaleCapByGrowth
            << " boundsCap=" << scaleCapByBounds
            << " minGap=" << minGapX << 'x' << minGapY
            << " maxShift=" << maxShift
            << " selectedTarget=" << rectToString(m_state.windows[currentManagedIndex].targetGlobal);
        debugLog(out.str());
    }

    if (!shouldAnimateRelayout) {
        if (debugLogsEnabled())
            debugLog("[hymission] expand-selected relayout skipped (in-place target unchanged)");
        return;
    }

    m_state.relayoutActive = true;
    m_state.relayoutProgress = 0.0;
    m_state.relayoutStart = {};
    damageOwnedMonitors();
}

void OverviewController::clearPendingWindowGeometryRetry() {
    m_pendingWindowGeometryRetryTarget.reset();
    m_pendingWindowGeometryRetryRemaining = 0;
    m_pendingWindowGeometryRetryScheduled = false;
    ++m_pendingWindowGeometryRetryGeneration;
}

void OverviewController::scheduleVisibleStateRebuild() {
    if (m_visibleStateRebuildScheduled)
        return;

    if (!g_pEventLoopManager) {
        rebuildVisibleState();
        return;
    }

    m_visibleStateRebuildScheduled = true;
    const auto generation = ++m_visibleStateRebuildGeneration;
    g_pEventLoopManager->doLater([this, generation] {
        if (g_controller != this || generation != m_visibleStateRebuildGeneration)
            return;

        m_visibleStateRebuildScheduled = false;
        if (!isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
            return;

        rebuildVisibleState();
    });
}

void OverviewController::scheduleWorkspaceChangeHandling(const PHLWORKSPACE& workspace, OverviewWorkspaceChangeAction action) {
    m_pendingWorkspaceChange = workspace;
    m_pendingWorkspaceChangeAction = action;

    if (m_workspaceChangeHandlingScheduled)
        return;

    if (!g_pEventLoopManager) {
        if (!insideRenderLifecycle()) {
            if (action == OverviewWorkspaceChangeAction::Rebuild) {
                if (m_workspaceTransition.active)
                    clearOverviewWorkspaceTransition();
                rebuildVisibleState();
            } else {
                beginClose(CloseMode::Abort);
            }
            m_pendingWorkspaceChange.reset();
            m_pendingWorkspaceChangeAction.reset();
        }
        return;
    }

    m_workspaceChangeHandlingScheduled = true;
    const auto generation = ++m_workspaceChangeHandlingGeneration;
    g_pEventLoopManager->doLater([this, generation] {
        if (g_controller != this || generation != m_workspaceChangeHandlingGeneration)
            return;

        m_workspaceChangeHandlingScheduled = false;
        const auto workspace = m_pendingWorkspaceChange.lock();
        if (!workspace || !m_pendingWorkspaceChangeAction.has_value())
            return;
        const auto action = *m_pendingWorkspaceChangeAction;
        m_pendingWorkspaceChangeAction.reset();

        if (insideRenderLifecycle()) {
            scheduleWorkspaceChangeHandling(workspace, action);
            return;
        }

        if (action == OverviewWorkspaceChangeAction::Rebuild) {
            if (m_workspaceTransition.active)
                clearOverviewWorkspaceTransition();
            rebuildVisibleState();
            return;
        }

        beginClose(CloseMode::Abort);
    });
}

void OverviewController::schedulePendingWindowGeometryRetry(const PHLWINDOW& window) {
    if (!window || !g_pEventLoopManager || !isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle)
        return;

    const auto pendingTarget = m_pendingWindowGeometryRetryTarget.lock();
    if (pendingTarget != window)
        m_pendingWindowGeometryRetryRemaining = 2;
    else
        m_pendingWindowGeometryRetryRemaining = std::max<std::size_t>(m_pendingWindowGeometryRetryRemaining, 2);
    m_pendingWindowGeometryRetryTarget = window;

    if (m_pendingWindowGeometryRetryScheduled)
        return;

    m_pendingWindowGeometryRetryScheduled = true;
    const auto generation = ++m_pendingWindowGeometryRetryGeneration;
    g_pEventLoopManager->doLater([this, generation] {
        if (g_controller != this || generation != m_pendingWindowGeometryRetryGeneration)
            return;

        m_pendingWindowGeometryRetryScheduled = false;

        if (!isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
            clearPendingWindowGeometryRetry();
            return;
        }

        const auto window = m_pendingWindowGeometryRetryTarget.lock();
        if (!window || !windowMatchesOverviewScope(window, m_state, false)) {
            clearPendingWindowGeometryRetry();
            return;
        }

        if (hasManagedWindow(window)) {
            clearPendingWindowGeometryRetry();
            return;
        }

        if (m_pendingWindowGeometryRetryRemaining == 0) {
            clearPendingWindowGeometryRetry();
            return;
        }

        --m_pendingWindowGeometryRetryRemaining;
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] retry overview rebuild for pending window geometry target=" << debugWindowLabel(window)
                << " retriesLeft=" << m_pendingWindowGeometryRetryRemaining;
            debugLog(out.str());
        }

        rebuildVisibleState();
        if (!isVisible()) {
            clearPendingWindowGeometryRetry();
            return;
        }

        if (!windowMatchesOverviewScope(window, m_state, false) || hasManagedWindow(window) || windowHasUsableStateGeometry(window) ||
            m_pendingWindowGeometryRetryRemaining == 0) {
            clearPendingWindowGeometryRetry();
            return;
        }

        schedulePendingWindowGeometryRetry(window);
    });
}

void OverviewController::updatePendingWindowGeometryRetry(const PHLWINDOW& window) {
    if (!window)
        return;

    const auto pendingTarget = m_pendingWindowGeometryRetryTarget.lock();
    if (!isVisible() || m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle) {
        if (pendingTarget == window)
            clearPendingWindowGeometryRetry();
        return;
    }

    if (!windowMatchesOverviewScope(window, m_state, false) || hasManagedWindow(window) || windowHasUsableStateGeometry(window)) {
        if (pendingTarget == window)
            clearPendingWindowGeometryRetry();
        return;
    }

    schedulePendingWindowGeometryRetry(window);
}

bool OverviewController::matchesPendingLiveFocusWorkspaceChange(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    const auto pendingTarget = m_pendingLiveFocusWorkspaceChangeTarget.lock();
    return pendingTarget && pendingTarget->m_workspace && pendingTarget->m_workspace == workspace;
}

void OverviewController::clearPendingStripWorkspaceChange() {
    m_pendingStripWorkspaceChangeTarget.reset();
}

bool OverviewController::matchesPendingStripWorkspaceChange(const PHLWORKSPACE& workspace) const {
    if (!workspace)
        return false;

    const auto pendingTarget = m_pendingStripWorkspaceChangeTarget.lock();
    return pendingTarget && pendingTarget == workspace;
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
    clearToggleSwitchSession();

    const auto buildStart = std::chrono::steady_clock::now();
    const double fromVisual = isVisible() ? visualProgress() : 0.0;
    clearOverviewWorkspaceTransition();
    m_workspaceSwipeGesture = {};
    recordWindowActivation(Desktop::focusState()->window());
    const auto preferredSelectedWindow = expandSelectedWindowEnabled() ? Desktop::focusState()->window() : PHLWINDOW{};
    State next = buildState(monitor, requestedScope, {}, false, false, preferredSelectedWindow);
    if (next.windows.empty() && next.stripEntries.empty()) {
        notify(collectionSummary(monitor), CHyprColor(1.0, 0.7, 0.2, 1.0), 5000);
        return;
    }

    if (!activateHooks())
        return;

    clearPendingWindowGeometryRetry();
    m_visibleStateRebuildScheduled = false;
    ++m_visibleStateRebuildGeneration;
    clearPostCloseForcedFocus();
    clearPostCloseDispatcher();
    m_lastLayoutSelectedWindow.reset();
    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_pendingLiveFocusWorkspaceChangeTarget.reset();
    m_pendingWorkspaceChange.reset();
    m_pendingWorkspaceChangeAction.reset();
    m_workspaceChangeHandlingScheduled = false;
    ++m_workspaceChangeHandlingGeneration;
    clearPendingStripWorkspaceChange();
    clearStripWindowDragState();
    m_primaryButtonPressed = false;
    next.phase = Phase::Opening;
    next.animationProgress = 0.0;
    next.animationFromVisual = fromVisual;
    next.animationToVisual = 1.0;
    next.animationStart = {};
    m_deactivatePending = false;
    carryOverWorkspaceStripSnapshots(next, m_state);
    m_state = std::move(next);
    m_hoverSelectionAnchorValid = false;
    m_hoverSelectionRetargetBlockedUntil = {};
    m_hoverSelectionRetargetCandidateIndex.reset();
    m_hoverSelectionRetargetCandidateSince = {};
    m_hoverSelectionRetargetCandidatePrimed = false;
    if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
        latchHoverSelectionAnchor(g_pInputManager->getMouseCoordsInternal());
    applyWorkspaceNameOverrides(m_state);
    clearHiddenStripLayerProxies();
    syncHiddenStripLayerProxies();
    setInputFollowMouseOverride(true);
    setScrollingFollowFocusOverride(true);
    setFullscreenRenderOverride(true);
    refreshWorkspaceStripSnapshots();
    g_pHyprRenderer->m_directScanoutBlocked = true;
    m_postOpenRefreshFrames = 3;
    if (!m_suppressInitialHoverUpdate)
        updateHoveredFromPointer(false, false, false, false, "opening-complete");

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
        logOverviewLayoutState("beginOpen", m_state);
        if (const auto selected = selectedWindow(); selected && selected->m_workspace && isScrollingWorkspace(selected->m_workspace))
            logScrollingWorkspaceSpotState("beginOpen", selected->m_workspace, selected);
    }

    damageOwnedMonitors();
}

bool OverviewController::retargetGestureScope(ScopeOverride requestedScope) {
    PHLMONITOR monitor = m_state.ownerMonitor;
    if (!monitor)
        monitor = g_pCompositor->getMonitorFromCursor();
    if (!monitor)
        return false;

    beginOpen(monitor, requestedScope);
    return isVisible() && m_state.collectionPolicy.requestedScope == requestedScope;
}

void OverviewController::beginClose(CloseMode mode, std::optional<double> fromVisualOverride, bool deferFullscreenMutations) {
    if (!isVisible())
        return;

    if (mode != CloseMode::Abort && (m_state.phase == Phase::Closing || m_state.phase == Phase::ClosingSettle))
        return;

    if (mode == CloseMode::Abort && m_state.phase == Phase::Closing)
        return;

    const ScopedFlag beginCloseGuard(m_beginCloseInProgress);
    clearToggleSwitchSession();

    clearPendingWindowGeometryRetry();
    m_visibleStateRebuildScheduled = false;
    ++m_visibleStateRebuildGeneration;

    if (mode == CloseMode::Abort)
        clearPostCloseDispatcher();

    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;

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
    clearHiddenStripLayerProxies();
    syncHiddenStripLayerProxies();

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

    damageOwnedMonitors();
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
    clearToggleSwitchSession();
    m_primaryButtonPressed = false;
    m_hoverSelectionAnchorValid = false;
    m_hoverSelectionRetargetBlockedUntil = {};
    m_hoverSelectionRetargetCandidateIndex.reset();
    m_hoverSelectionRetargetCandidateSince = {};
    m_hoverSelectionRetargetCandidatePrimed = false;
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
    m_lastLayoutSelectedWindow.reset();
    m_queuedOverviewSelectionTarget.reset();
    m_queuedOverviewSelectionSyncScrollingSpot = false;
    m_queuedOverviewLiveFocusTarget.reset();
    m_queuedOverviewLiveFocusSyncScrollingSpot = false;
    m_pendingLiveFocusWorkspaceChangeTarget.reset();
    m_pendingWorkspaceChange.reset();
    m_pendingWorkspaceChangeAction.reset();
    m_workspaceChangeHandlingScheduled = false;
    ++m_workspaceChangeHandlingGeneration;
    clearPendingStripWorkspaceChange();
    clearStripWindowDragState();
    clearHiddenStripLayerProxies();
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
    m_deactivateScheduled = false;
    m_visibleStateRebuildScheduled = false;
    ++m_visibleStateRebuildGeneration;
    m_postOpenRefreshFrames = 0;
    clearPendingWindowGeometryRetry();
    clearOverviewWorkspaceTransition();
    m_workspaceSwipeGesture = {};
    m_stripSnapshotsDirty = false;
    m_stripSnapshotRefreshScheduled = false;
    m_state = {};
    for (const auto& ownedMonitor : ownedMonitors) {
        g_pHyprRenderer->damageMonitor(ownedMonitor);
        g_pCompositor->scheduleFrameForMonitor(ownedMonitor);
    }
    if (monitor) {
        g_pHyprRenderer->damageMonitor(monitor);
        g_pCompositor->scheduleFrameForMonitor(monitor);
    }

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

void OverviewController::scheduleDeactivate() {
    if (m_deactivateScheduled || !g_pEventLoopManager)
        return;

    m_deactivateScheduled = true;
    g_pEventLoopManager->doLater([this] {
        if (g_controller != this)
            return;

        m_deactivateScheduled = false;
        if (!m_deactivatePending || !isVisible())
            return;

        if (g_pHyprOpenGL && g_pHyprOpenGL->m_renderData.pMonitor) {
            scheduleDeactivate();
            return;
        }

        if (debugLogsEnabled())
            debugLog("[hymission] deferred deactivate");
        deactivate();
    });
}

void OverviewController::damageOwnedMonitors() const {
    for (const auto& monitor : ownedMonitors()) {
        g_pHyprRenderer->damageMonitor(monitor);
        g_pCompositor->scheduleFrameForMonitor(monitor);
    }
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
                damageOwnedMonitors();
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
            latchHoverSelectionAnchor(g_pInputManager->getMouseCoordsInternal());
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
        updateSelectedWindowLayout({});
        updateHoveredFromPointer(false, false, false, false, "begin-open");
        damageOwnedMonitors();
    } else if (m_state.phase == Phase::Closing) {
        m_state.animationProgress = 1.0;
        m_state.animationFromVisual = 0.0;
        m_state.animationToVisual = 0.0;
        if (!m_deactivatePending) {
            if (debugLogsEnabled())
                debugLog("[hymission] anim closing complete, queue deferred deactivate");
            m_deactivatePending = true;
            scheduleDeactivate();
        }
    }
}

void OverviewController::updateHoveredFromPointer(bool syncSelection, bool syncRealFocus, bool syncScrollingSpot, bool allowSelectionRetarget, const char* source) {
    if (!isVisible())
        return;

    const Vector2D pointer = g_pInputManager->getMouseCoordsInternal();
    const bool draggingWindow = m_draggedWindowIndex.has_value();
    const auto previousHoveredStrip = m_state.hoveredStripIndex;
    const auto previousHovered = m_state.hoveredIndex;
    const auto previousSelected = m_state.selectedIndex;
    const auto previousFocus = m_state.focusDuringOverview;
    const auto now = std::chrono::steady_clock::now();

    m_state.hoveredStripIndex = hitTestStripTarget(pointer.x, pointer.y);
    m_state.hoveredIndex = (draggingWindow || m_state.hoveredStripIndex) ? std::optional<std::size_t>{} : hitTestTarget(pointer.x, pointer.y);

    const bool wantsSelectionRetarget =
        !draggingWindow && syncSelection && m_state.hoveredIndex && focusFollowsMouseEnabled() && allowSelectionRetarget &&
        (!m_state.selectedIndex || *m_state.hoveredIndex != *m_state.selectedIndex);
    const bool immediateRetarget = wantsSelectionRetarget && syncRealFocus;
    const bool retargetBlockedByRelayout = expandSelectedWindowEnabled() && m_state.relayoutActive && !immediateRetarget;
    const bool retargetBlockedByCooldown = expandSelectedWindowEnabled() && now < m_hoverSelectionRetargetBlockedUntil && !immediateRetarget;
    bool retargetLocked = false;
    if (wantsSelectionRetarget && !immediateRetarget && !retargetBlockedByRelayout && !retargetBlockedByCooldown)
        retargetLocked = hoverSelectionRetargetLocked(pointer, m_state.hoveredIndex);

    const bool retargetBlocked = retargetBlockedByRelayout || retargetBlockedByCooldown || retargetLocked;
    const bool canRetargetSelection = wantsSelectionRetarget && !retargetBlocked;

    if (!wantsSelectionRetarget || immediateRetarget) {
        m_hoverSelectionRetargetCandidateIndex.reset();
        m_hoverSelectionRetargetCandidateSince = {};
        m_hoverSelectionRetargetCandidatePrimed = false;
    } else if (retargetBlocked) {
        m_hoverSelectionRetargetCandidateIndex = m_state.hoveredIndex;
        m_hoverSelectionRetargetCandidateSince = {};
        m_hoverSelectionRetargetCandidatePrimed = false;
    }

    if (immediateRetarget && m_state.hoveredIndex) {
        m_state.selectedIndex = m_state.hoveredIndex;
        const auto nextSelectedWindow = selectedWindow();
        if (nextSelectedWindow) {
            m_state.focusDuringOverview = nextSelectedWindow;
            queueSelectionRetargetDuringOverview(nextSelectedWindow, syncScrollingSpot, source);
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] hover retarget immediate queued pointer=" << pointer.x << ',' << pointer.y;
                out << " source=" << (source ? source : "?");
                out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(nextSelectedWindow);
                const auto activeWindow = Desktop::focusState()->window();
                if (activeWindow)
                    out << " active=" << debugWindowLabel(activeWindow);
                else
                    out << " active=<null>";
                debugLog(out.str());
            }
        }
    } else if (canRetargetSelection) {
        const bool candidateNeedsPriming = m_hoverSelectionRetargetCandidateIndex != m_state.hoveredIndex || !m_hoverSelectionRetargetCandidatePrimed ||
            m_hoverSelectionRetargetCandidateSince == std::chrono::steady_clock::time_point{};
        if (candidateNeedsPriming) {
            m_hoverSelectionRetargetCandidateIndex = m_state.hoveredIndex;
            m_hoverSelectionRetargetCandidateSince = now;
            m_hoverSelectionRetargetCandidatePrimed = true;
            if (debugLogsEnabled()) {
                std::ostringstream out;
                out << "[hymission] hover retarget dwell start pointer=" << pointer.x << ',' << pointer.y;
                out << " source=" << (source ? source : "?");
                out << " candidate=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
                if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
                    out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
                else
                    out << " selected=<null>";
                out << " dwellMs=" << HOVER_SELECTION_RETARGET_DWELL.count();
                debugLog(out.str());
            }
        } else if (now - m_hoverSelectionRetargetCandidateSince >= HOVER_SELECTION_RETARGET_DWELL) {
            const bool selectionChanged = m_state.selectedIndex != m_state.hoveredIndex;
            m_state.selectedIndex = m_state.hoveredIndex;
            m_hoverSelectionRetargetCandidateIndex.reset();
            m_hoverSelectionRetargetCandidateSince = {};
            m_hoverSelectionRetargetCandidatePrimed = false;
            if (syncRealFocus)
                syncFocusDuringOverviewFromSelection(syncScrollingSpot, source);
            else
                m_state.focusDuringOverview = m_state.windows[*m_state.hoveredIndex].window;
            if (selectionChanged && !syncRealFocus)
                latchHoverSelectionAnchor(pointer);
        } else if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] hover retarget dwelling pointer=" << pointer.x << ',' << pointer.y;
            out << " source=" << (source ? source : "?");
            out << " candidate=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
            if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
                out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
            else
                out << " selected=<null>";
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_hoverSelectionRetargetCandidateSince).count();
            out << " elapsedMs=" << elapsed;
            out << " dwellMs=" << HOVER_SELECTION_RETARGET_DWELL.count();
            debugLog(out.str());
        }
    } else if (retargetBlockedByCooldown && allowSelectionRetarget && debugLogsEnabled() && m_state.hoveredIndex && m_state.selectedIndex &&
               *m_state.hoveredIndex != *m_state.selectedIndex) {
        std::ostringstream out;
        out << "[hymission] hover retarget cooling down pointer=" << pointer.x << ',' << pointer.y;
        out << " source=" << (source ? source : "?");
        out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_hoverSelectionRetargetBlockedUntil - now).count();
        out << " remainingMs=" << std::max<long long>(remaining, 0);
        debugLog(out.str());
    } else if (retargetBlockedByRelayout && allowSelectionRetarget && debugLogsEnabled() && m_state.hoveredIndex && m_state.selectedIndex &&
               *m_state.hoveredIndex != *m_state.selectedIndex) {
        std::ostringstream out;
        out << "[hymission] hover retarget deferred during relayout pointer=" << pointer.x << ',' << pointer.y;
        out << " source=" << (source ? source : "?");
        out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        out << " relayoutT=" << m_state.relayoutProgress;
        debugLog(out.str());
    } else if (retargetLocked && debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] hover retarget locked pointer=" << pointer.x << ',' << pointer.y
            << " source=" << (source ? source : "?")
            << " anchor=" << vectorToString(m_hoverSelectionAnchorPointer)
            << " threshold=" << HOVER_SELECTION_RETARGET_DISTANCE;
        if (m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size())
            out << " hovered=" << *m_state.hoveredIndex << ":" << debugWindowLabel(m_state.windows[*m_state.hoveredIndex].window);
        else
            out << " hovered=<null>";
        if (m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size())
            out << " selected=" << *m_state.selectedIndex << ":" << debugWindowLabel(m_state.windows[*m_state.selectedIndex].window);
        else
            out << " selected=<null>";
        debugLog(out.str());
    }

    if (previousHoveredStrip != m_state.hoveredStripIndex || previousHovered != m_state.hoveredIndex || previousSelected != m_state.selectedIndex ||
        previousFocus != m_state.focusDuringOverview) {
        if (debugLogsEnabled()) {
            std::ostringstream out;
            out << "[hymission] hover pointer=" << pointer.x << ',' << pointer.y;
            out << " source=" << (source ? source : "?");
            if (m_state.hoveredStripIndex && *m_state.hoveredStripIndex < m_state.stripEntries.size()) {
                const auto& entry = m_state.stripEntries[*m_state.hoveredStripIndex];
                out << " strip=" << *m_state.hoveredStripIndex << ':' << entry.workspaceId;
                if (entry.workspace)
                    out << ':' << entry.workspace->m_name;
                else if (!entry.workspaceName.empty())
                    out << ':' << entry.workspaceName;
                if (entry.newWorkspaceSlot)
                    out << ":new";
            } else {
                out << " strip=<null>";
            }
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
        damageOwnedMonitors();
    }
}

void OverviewController::rebuildVisibleState(PHLWINDOW preferredSelectedWindow, bool forceRelayout) {
    if (!isVisible() || !m_state.ownerMonitor || !m_state.ownerWorkspace)
        return;

    if (m_workspaceTransition.active)
        clearOverviewWorkspaceTransition();

    const auto monitor = m_state.ownerMonitor;
    const auto previousOwnerWorkspace = m_state.ownerWorkspace;
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

    const auto layoutSelectedWindow =
        expandSelectedWindowEnabled() ? (preferredSelectedWindow ? preferredSelectedWindow : (selectedWindow() ? selectedWindow() : Desktop::focusState()->window())) : PHLWINDOW{};
    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] rebuild request forceRelayout=" << (forceRelayout ? 1 : 0)
            << " preferredSelected=" << debugWindowLabel(preferredSelectedWindow)
            << " layoutSelected=" << debugWindowLabel(layoutSelectedWindow);
        if (const auto activeWindow = Desktop::focusState()->window(); activeWindow)
            out << " active=" << debugWindowLabel(activeWindow);
        else
            out << " active=<null>";
        debugLog(out.str());
    }
    State next = buildState(monitor, requestedScope, {}, false, false, layoutSelectedWindow);
    if (next.windows.empty() && next.stripEntries.empty()) {
        beginClose(CloseMode::Abort);
        return;
    }

    if (!m_state.collectionPolicy.onlyActiveWorkspace && previousOwnerWorkspace)
        next.ownerWorkspace = previousOwnerWorkspace;

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
    const bool selectionRelayoutForced = forceRelayout && expandSelectedWindowEnabled();

    bool shouldAnimateRelayout = false;
    if (sameWindowSet && sameMonitorSet && !selectionRelayoutForced) {
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
    } else if (previousPhase == Phase::Active || selectionRelayoutForced) {
        for (auto& window : next.windows) {
            if (const auto* previousManaged = previousManagedForWindow(window.window); previousManaged)
                window.exitGlobal = previousManaged->exitGlobal;

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

    if (sameWindowSet && sameMonitorSet && !selectionRelayoutForced) {
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
        out << " forcedSelectionRelayout=" << (selectionRelayoutForced ? 1 : 0);
        debugLog(out.str());
        if (forceRelayout || selectionRelayoutForced) {
            logOverviewLayoutState("rebuild-before", m_state);
            logOverviewLayoutState("rebuild-next", next);
        }
    }

    clearStripWindowDragState();
    carryOverWorkspaceStripSnapshots(next, m_state);
    m_state = std::move(next);
    applyWorkspaceNameOverrides(m_state);
    syncHiddenStripLayerProxies();
    refreshWorkspaceStripSnapshots();
    if (selectionRelayoutForced) {
        m_state.hoveredIndex = m_state.selectedIndex;
    } else {
        updateHoveredFromPointer(false, false, false, false, forceRelayout ? "rebuild-forced" : "rebuild-passive");
    }
    if (!selectionRelayoutForced && m_state.phase == Phase::Active && (!sameWindowSet || !sameMonitorSet))
        updateSelectedWindowLayout({});
    if (debugLogsEnabled() && (forceRelayout || selectionRelayoutForced))
        logOverviewLayoutState("rebuild-after-hover-refresh", m_state);
    damageOwnedMonitors();
}

void OverviewController::moveSelection(Direction direction) {
    if (m_state.windows.empty())
        return;

    if (!m_state.selectedIndex) {
        m_state.selectedIndex = 0;
        syncFocusDuringOverviewFromSelection(true, "keyboard-init");
        damageOwnedMonitors();
        return;
    }

    if (const auto next = chooseDirectionalNeighbor(targetRects(), *m_state.selectedIndex, direction)) {
        if (*next == *m_state.selectedIndex)
            return;
        m_state.selectedIndex = *next;
        syncFocusDuringOverviewFromSelection(true, "keyboard-nav");
        damageOwnedMonitors();
    }
}

bool OverviewController::moveSelectionCircular(int step, const char* source) {
    if (m_state.windows.size() < 2)
        return false;

    if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size())
        m_state.selectedIndex = 0;

    const auto next = chooseCyclicIndex(m_state.windows.size(), *m_state.selectedIndex, step);
    if (!next || *next == *m_state.selectedIndex)
        return false;

    m_state.selectedIndex = *next;
    syncFocusDuringOverviewFromSelection(true, source);
    damageOwnedMonitors();
    return true;
}

void OverviewController::activateSelection() {
    if (!m_state.selectedIndex || *m_state.selectedIndex >= m_state.windows.size())
        return;

    beginClose(CloseMode::ActivateSelection);
}

void OverviewController::clearStripWindowDragState() {
    m_pressedStripIndex.reset();
    m_pressedWindowIndex.reset();
    m_draggedWindowIndex.reset();
    m_pressedWindowPointer = {};
    m_draggedWindowPointerOffset = {};
}

void OverviewController::activateStripTarget(std::size_t index) {
    if (index >= m_state.stripEntries.size())
        return;

    clearStripWindowDragState();

    const auto& entry = m_state.stripEntries[index];
    if (!entry.monitor || entry.workspaceId == WORKSPACE_INVALID)
        return;

    auto targetWorkspace = entry.workspace ? entry.workspace : g_pCompositor->getWorkspaceByID(entry.workspaceId);
    if (targetWorkspace && targetWorkspace->m_isSpecialWorkspace)
        return;

    if (targetWorkspace && entry.monitor->m_activeWorkspace == targetWorkspace) {
        m_state.hoveredStripIndex = index;
        damageOwnedMonitors();
        return;
    }

    const std::string targetName = entry.workspaceName.empty() ? std::to_string(entry.workspaceId) : entry.workspaceName;
    const bool        syntheticTarget = !targetWorkspace && (entry.syntheticEmpty || entry.newWorkspaceSlot);

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] activate strip target monitor=" << entry.monitor->m_name;
        if (targetWorkspace)
            out << " workspace=" << debugWorkspaceLabel(targetWorkspace);
        else
            out << " workspace=<synthetic:" << entry.workspaceId << '>';
        out << " phase=" << static_cast<int>(m_state.phase)
            << " synthetic=" << (entry.syntheticEmpty ? 1 : 0) << " new=" << (entry.newWorkspaceSlot ? 1 : 0);
        debugLog(out.str());
    }

    if (!beginOverviewWorkspaceTransition(entry.monitor, entry.workspaceId, targetName, targetWorkspace, syntheticTarget, WorkspaceTransitionMode::TimedCommit)) {
        if (debugLogsEnabled())
            debugLog("[hymission] strip target transition begin failed");
        damageOwnedMonitors();
    }
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

void OverviewController::latchHoverSelectionAnchor(const Vector2D& pointer) {
    m_hoverSelectionAnchorPointer = pointer;
    m_hoverSelectionAnchorValid = true;
}

bool OverviewController::hoverSelectionRetargetLocked(const Vector2D& pointer, const std::optional<std::size_t>& hoveredIndex) const {
    if (!expandSelectedWindowEnabled() || !m_hoverSelectionAnchorValid || !m_state.selectedIndex || !hoveredIndex || *hoveredIndex == *m_state.selectedIndex)
        return false;

    const double distance = std::hypot(pointer.x - m_hoverSelectionAnchorPointer.x, pointer.y - m_hoverSelectionAnchorPointer.y);
    return distance < HOVER_SELECTION_RETARGET_DISTANCE;
}

void OverviewController::logOverviewLayoutState(const char* context, const State& state) const {
    if (!debugLogsEnabled())
        return;

    std::ostringstream summary;
    summary << "[hymission] layout dump context=" << (context ? context : "?") << " phase=";
    switch (state.phase) {
        case Phase::Inactive:
            summary << "inactive";
            break;
        case Phase::Opening:
            summary << "opening";
            break;
        case Phase::Active:
            summary << "active";
            break;
        case Phase::ClosingSettle:
            summary << "closing-settle";
            break;
        case Phase::Closing:
            summary << "closing";
            break;
    }
    summary << " ownerMonitor=" << (state.ownerMonitor ? state.ownerMonitor->m_name : "?")
            << " ownerWorkspace=" << debugWorkspaceLabel(state.ownerWorkspace)
            << " windows=" << state.windows.size();
    if (state.selectedIndex && *state.selectedIndex < state.windows.size())
        summary << " selected=" << *state.selectedIndex << ":" << debugWindowLabel(state.windows[*state.selectedIndex].window);
    else
        summary << " selected=<null>";
    if (state.hoveredIndex && *state.hoveredIndex < state.windows.size())
        summary << " hovered=" << *state.hoveredIndex << ":" << debugWindowLabel(state.windows[*state.hoveredIndex].window);
    else
        summary << " hovered=<null>";
    if (state.focusDuringOverview)
        summary << " focusDuringOverview=" << debugWindowLabel(state.focusDuringOverview);
    else
        summary << " focusDuringOverview=<null>";
    debugLog(summary.str());

    if (state.windows.empty())
        return;

    std::ostringstream stateOrder;
    stateOrder << "[hymission] layout dump state-order context=" << (context ? context : "?");
    for (std::size_t index = 0; index < state.windows.size(); ++index) {
        const auto& managed = state.windows[index];
        stateOrder << " | #" << index << ' ' << debugWindowLabel(managed.window);
        if (managed.window && managed.window->m_workspace)
            stateOrder << " ws=" << debugWorkspaceLabel(managed.window->m_workspace);
        stateOrder << " mon=" << (managed.targetMonitor ? managed.targetMonitor->m_name : "?")
                   << " slot=" << rectToString(managed.slot.target)
                   << " target=" << rectToString(managed.targetGlobal);
    }
    debugLog(stateOrder.str());

    std::vector<std::size_t> visualOrder(state.windows.size());
    std::iota(visualOrder.begin(), visualOrder.end(), 0);
    std::stable_sort(visualOrder.begin(), visualOrder.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto& left = state.windows[lhs];
        const auto& right = state.windows[rhs];
        const auto leftMonitorId = left.targetMonitor ? left.targetMonitor->m_id : MONITOR_INVALID;
        const auto rightMonitorId = right.targetMonitor ? right.targetMonitor->m_id : MONITOR_INVALID;
        if (leftMonitorId != rightMonitorId)
            return leftMonitorId < rightMonitorId;
        if (std::abs(left.targetGlobal.y - right.targetGlobal.y) > 0.5)
            return left.targetGlobal.y < right.targetGlobal.y;
        if (std::abs(left.targetGlobal.x - right.targetGlobal.x) > 0.5)
            return left.targetGlobal.x < right.targetGlobal.x;
        return lhs < rhs;
    });

    std::ostringstream visual;
    visual << "[hymission] layout dump visual-order context=" << (context ? context : "?");
    for (std::size_t visualIndex = 0; visualIndex < visualOrder.size(); ++visualIndex) {
        const auto stateIndex = visualOrder[visualIndex];
        const auto& managed = state.windows[stateIndex];
        visual << " | @" << visualIndex << " #" << stateIndex << ' ' << debugWindowLabel(managed.window)
               << " target=" << rectToString(managed.targetGlobal);
    }
    debugLog(visual.str());
}

void OverviewController::logScrollingWorkspaceSpotState(const char* context, const PHLWORKSPACE& workspace, const PHLWINDOW& focusWindow) const {
    if (!debugLogsEnabled() || !workspace)
        return;

    auto* scrolling = scrollingAlgorithmForWorkspace(workspace);
    if (!scrolling || !scrolling->m_scrollingData || !scrolling->m_scrollingData->controller) {
        std::ostringstream out;
        out << "[hymission] scrolling spot dump context=" << (context ? context : "?")
            << " workspace=" << debugWorkspaceLabel(workspace) << " unavailable=1";
        debugLog(out.str());
        return;
    }

    const auto& data = scrolling->m_scrollingData;
    std::ostringstream summary;
    summary << "[hymission] scrolling spot dump context=" << (context ? context : "?")
            << " workspace=" << debugWorkspaceLabel(workspace)
            << " visible=" << (workspace->isVisible() ? 1 : 0)
            << " columns=" << data->columns.size()
            << " offset=" << data->controller->getOffset();
    if (data->lockedCameraOffset)
        summary << " lockedOffset=" << *data->lockedCameraOffset;
    else
        summary << " lockedOffset=<null>";
    if (focusWindow)
        summary << " focus=" << debugWindowLabel(focusWindow);
    const auto activeWindow = Desktop::focusState()->window();
    if (activeWindow)
        summary << " active=" << debugWindowLabel(activeWindow);
    else
        summary << " active=<null>";
    debugLog(summary.str());

    std::ostringstream columns;
    columns << "[hymission] scrolling spot columns context=" << (context ? context : "?");
    for (std::size_t columnIndex = 0; columnIndex < data->columns.size(); ++columnIndex) {
        const auto& column = data->columns[columnIndex];
        if (!column) {
            columns << " | col#" << columnIndex << " <null-column>";
            continue;
        }

        const auto lastFocusedTarget = column->lastFocusedTarget.lock();
        columns << " | col#" << columnIndex << " width=" << column->getColumnWidth() << " targets=" << column->targetDatas.size();
        if (lastFocusedTarget) {
            const auto lastTarget = lastFocusedTarget->target.lock();
            columns << " lastFocused=" << debugWindowLabel(lastTarget ? lastTarget->window() : PHLWINDOW{});
        } else {
            columns << " lastFocused=<null>";
        }

        for (std::size_t targetIndex = 0; targetIndex < column->targetDatas.size(); ++targetIndex) {
            const auto& targetData = column->targetDatas[targetIndex];
            const auto target = targetData ? targetData->target.lock() : SP<Layout::ITarget>{};
            const auto window = target ? target->window() : PHLWINDOW{};

            columns << " [" << targetIndex << ']';
            if (targetData && targetData == lastFocusedTarget)
                columns << '*';
            if (window == focusWindow)
                columns << '!';
            columns << debugWindowLabel(window);
            if (target)
                columns << " pos=" << boxToString(target->position());
            if (targetData)
                columns << " layout=" << boxToString(targetData->layoutBox);
        }
    }
    debugLog(columns.str());
}

bool OverviewController::workspaceStripEntriesMatchForSnapshot(const WorkspaceStripEntry& lhs, const WorkspaceStripEntry& rhs) const {
    const auto lhsMonitorId = lhs.monitor ? lhs.monitor->m_id : MONITOR_INVALID;
    const auto rhsMonitorId = rhs.monitor ? rhs.monitor->m_id : MONITOR_INVALID;
    if (lhsMonitorId != rhsMonitorId)
        return false;

    if (lhs.newWorkspaceSlot != rhs.newWorkspaceSlot)
        return false;

    if (lhs.workspaceId != rhs.workspaceId)
        return false;

    if (lhs.newWorkspaceSlot)
        return lhs.workspaceId == rhs.workspaceId;

    if (lhs.workspace && rhs.workspace)
        return lhs.workspace == rhs.workspace;

    return true;
}

void OverviewController::carryOverWorkspaceStripSnapshots(State& next, const State& previous) const {
    if (next.stripEntries.empty() || previous.stripEntries.empty())
        return;

    // Keep the previous strip textures alive until the async refresh repaints
    // the new state. Otherwise workspace commits briefly render only the card
    // background because the replacement snapshot is deferred to doLater().
    for (auto& nextEntry : next.stripEntries) {
        if (nextEntry.snapshot || nextEntry.newWorkspaceSlot)
            continue;

        const auto previousIt = std::find_if(previous.stripEntries.begin(), previous.stripEntries.end(), [&](const WorkspaceStripEntry& previousEntry) {
            return previousEntry.snapshot && workspaceStripEntriesMatchForSnapshot(nextEntry, previousEntry);
        });
        if (previousIt == previous.stripEntries.end())
            continue;

        nextEntry.snapshot = previousIt->snapshot;
    }
}

void OverviewController::renderBackdrop() const {
    const double alpha = BACKDROP_ALPHA * visualProgress();
    if (alpha <= 0.0)
        return;

    const auto monitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!monitor)
        return;

    g_pHyprOpenGL->renderRect(
        CBox{
            0.0,
            0.0,
            monitor->m_transformedSize.x,
            monitor->m_transformedSize.y,
        },
        CHyprColor(0.05, 0.06, 0.08, alpha),
        {});
}

void OverviewController::renderSelectionChrome() const {
    const double progress = visualProgress();
    if (progress <= 0.0)
        return;

    const auto renderMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    const bool showFocusIndicator = showFocusIndicatorEnabled();

    if (showFocusIndicator && m_state.hoveredIndex && *m_state.hoveredIndex < m_state.windows.size() &&
        m_state.windows[*m_state.hoveredIndex].targetMonitor == renderMonitor) {
        renderOutline(currentPreviewRect(m_state.windows[*m_state.hoveredIndex]), CHyprColor(0.95, 0.97, 1.0, 0.55 * progress), HOVER_THICKNESS);
    }

    if (showFocusIndicator && m_state.selectedIndex && *m_state.selectedIndex < m_state.windows.size() &&
        m_state.windows[*m_state.selectedIndex].targetMonitor == renderMonitor) {
        const auto& window = m_state.windows[*m_state.selectedIndex];
        const Rect  rectGlobal = currentPreviewRect(window);
        const Rect  rect = rectToMonitorRenderLocal(rectGlobal, renderMonitor);
        renderOutline(rectGlobal, CHyprColor(0.24, 0.78, 1.0, 0.95 * progress), OUTLINE_THICKNESS);

        auto texture =
            g_pHyprOpenGL->renderText(window.title, CHyprColor(1.0, 1.0, 1.0, std::min(1.0, progress)), scaleFontSizeForRender(renderMonitor, 16), false, "",
                                      static_cast<int>(rect.width));
        if (texture) {
            const Rect titleRect =
                makeRect(rect.x, std::max(scaleLengthForRender(renderMonitor, 8.0), rect.y - texture->m_size.y - scaleLengthForRender(renderMonitor, TITLE_PADDING)),
                         texture->m_size.x, texture->m_size.y);
            g_pHyprOpenGL->renderTexture(texture, toBox(titleRect), {});
        }
    }

    if (m_draggedWindowIndex && *m_draggedWindowIndex < m_state.windows.size() && m_state.windows[*m_draggedWindowIndex].targetMonitor == renderMonitor) {
        const auto& window = m_state.windows[*m_draggedWindowIndex];
        const Rect  preview = currentPreviewRect(window);
        const auto  pointer = g_pInputManager->getMouseCoordsInternal();
        const Rect  ghostGlobal = makeRect(pointer.x - m_draggedWindowPointerOffset.x, pointer.y - m_draggedWindowPointerOffset.y, preview.width, preview.height);
        const Rect  ghost = rectToMonitorRenderLocal(ghostGlobal, renderMonitor);
        g_pHyprOpenGL->renderRect(toBox(ghost), CHyprColor(0.16, 0.20, 0.24, 0.28 * progress), {});
        renderOutline(ghostGlobal, CHyprColor(0.95, 0.97, 1.0, 0.82 * progress), 2.0);
    }
}

void OverviewController::renderOutline(const Rect& rect, const CHyprColor& color, double thickness) const {
    const auto renderMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    const Rect local = rectToMonitorRenderLocal(rect, renderMonitor);
    const double scaledThickness = scaleLengthForRender(renderMonitor, thickness);
    const Rect top = makeRect(local.x - scaledThickness, local.y - scaledThickness, local.width + scaledThickness * 2.0, scaledThickness);
    const Rect bottom = makeRect(local.x - scaledThickness, local.y + local.height, local.width + scaledThickness * 2.0, scaledThickness);
    const Rect left = makeRect(local.x - scaledThickness, local.y, scaledThickness, local.height);
    const Rect right = makeRect(local.x + local.width, local.y, scaledThickness, local.height);

    g_pHyprOpenGL->renderRect(toBox(top), color, {});
    g_pHyprOpenGL->renderRect(toBox(bottom), color, {});
    g_pHyprOpenGL->renderRect(toBox(left), color, {});
    g_pHyprOpenGL->renderRect(toBox(right), color, {});
}

Rect OverviewController::workspaceStripThumbRect(const WorkspaceStripEntry& entry, const PHLMONITOR& monitor) const {
    if (!monitor)
        return {};

    const Rect outer = rectToMonitorLocal(entry.rect, monitor);
    return makeRect(outer.x, outer.y, outer.width, outer.height);
}

void OverviewController::renderWorkspaceStripSnapshot(WorkspaceStripEntry& entry) {
    entry.snapshot.reset();

    if (!entry.monitor || entry.newWorkspaceSlot)
        return;

    const Rect thumb = workspaceStripThumbRect(entry, entry.monitor);
    if (thumb.width < 4.0 || thumb.height < 4.0)
        return;

    const int fbWidth = std::max(1, static_cast<int>(std::ceil(thumb.width * entry.monitor->m_scale)));
    const int fbHeight = std::max(1, static_cast<int>(std::ceil(thumb.height * entry.monitor->m_scale)));
    using RenderWorkspaceFn = void (*)(CHyprRenderer*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);

    static RenderWorkspaceFn renderWorkspaceFn = nullptr;
    static bool renderWorkspaceResolved = false;
    if (!renderWorkspaceResolved) {
        renderWorkspaceResolved = true;
        renderWorkspaceFn = reinterpret_cast<RenderWorkspaceFn>(findFunction("renderWorkspace", "CHyprRenderer::renderWorkspace"));
        if (!renderWorkspaceFn)
            debugLog("[hymission] failed to resolve CHyprRenderer::renderWorkspace for strip snapshots");
    }

    const auto monitor = entry.monitor;
    const auto targetWorkspace = entry.workspace ? entry.workspace : g_pCompositor->getWorkspaceByID(entry.workspaceId);
    if (!targetWorkspace && !entry.syntheticEmpty)
        return;
    auto snapshot = std::make_shared<WorkspaceStripEntry::Snapshot>();
    if (!snapshot->framebuffer.alloc(fbWidth, fbHeight))
        return;
    setFramebufferLinearFiltering(snapshot->framebuffer);

    State previewState;
    bool  renderWorkspaceContents = false;
    if (targetWorkspace) {
        const std::vector<WorkspaceOverride> workspaceOverrides = {{
            .monitorId = monitor->m_id,
            .workspace = targetWorkspace,
            .workspaceId = entry.workspaceId,
            .workspaceName = entry.workspaceName,
            .syntheticEmpty = false,
        }};
        previewState = buildState(monitor, ScopeOverride::OnlyCurrentWorkspace, workspaceOverrides, true, true);
        previewState.phase = Phase::Active;
        previewState.animationProgress = 1.0;
        previewState.animationFromVisual = 1.0;
        previewState.animationToVisual = 1.0;
        previewState.relayoutProgress = 1.0;
        previewState.relayoutActive = false;

        const Vector2D previewOffset = stripThumbnailPreviewOffset(monitor, previewState);
        if (previewOffset.x != 0.0 || previewOffset.y != 0.0) {
            for (auto& managed : previewState.windows) {
                managed.targetGlobal = translateRect(managed.targetGlobal, -previewOffset.x, -previewOffset.y);
                managed.relayoutFromGlobal = translateRect(managed.relayoutFromGlobal, -previewOffset.x, -previewOffset.y);
                managed.exitGlobal = translateRect(managed.exitGlobal, -previewOffset.x, -previewOffset.y);
                managed.slot.target =
                    makeRect(managed.slot.target.x - previewOffset.x, managed.slot.target.y - previewOffset.y, managed.slot.target.width, managed.slot.target.height);
            }
        }

        // Decide from the thumbnail's own preview build, not the outer strip's
        // cached bookkeeping, whether this workspace still has content to draw.
        renderWorkspaceContents = !previewState.windows.empty();
    }

    // Once a real workspace has no non-fading window previews left, snapshot it
    // as background-only so the strip does not freeze a close-animation frame.
    if (renderWorkspaceContents && !renderWorkspaceFn)
        return;
    const bool targetIsCurrentWorkspace = isCurrentActiveWorkspaceStripEntry(entry);

    const auto previousWorkspace = monitor->m_activeWorkspace;
    const auto previousSpecialWorkspace = monitor->m_activeSpecialWorkspace;
    const bool previousBlockSurfaceFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const bool previousBlockScreenShader = g_pHyprOpenGL->m_renderData.blockScreenShader;
    bool targetVisibilityChanged = false;
    bool previousVisibilityChanged = false;
    bool targetAnimationActivated = false;
    bool previousAnimationActivated = false;

    const auto applyFullscreenOverrideForState = [](State& state, bool suppress) {
        if (suppress) {
            if (!state.fullscreenOverrideActive)
                state.fullscreenOverrideActive = true;

            for (const auto& backup : state.fullscreenBackups) {
                if (!backup.workspace)
                    continue;

                backup.workspace->m_hasFullscreenWindow = false;
                backup.workspace->m_fullscreenMode = FSMODE_NONE;
                if (const auto workspaceMonitor = backup.workspace->m_monitor.lock())
                    workspaceMonitor->m_solitaryClient.reset();
            }

            return;
        }

        if (!state.fullscreenOverrideActive)
            return;

        for (const auto& backup : state.fullscreenBackups) {
            if (!backup.workspace)
                continue;

            backup.workspace->m_hasFullscreenWindow = backup.hadFullscreenWindow;
            backup.workspace->m_fullscreenMode = backup.fullscreenMode;
            if (const auto workspaceMonitor = backup.workspace->m_monitor.lock())
                workspaceMonitor->m_solitaryClient.reset();
        }

        state.fullscreenOverrideActive = false;
    };

    const auto renderBackgroundLayers = [&](const Time::steady_tp& now) {
        if (!m_renderLayerOriginal)
            return;

        for (const auto layerKind : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
            for (const auto& layer : g_pCompositor->m_layers) {
                if (!layer || !layer->m_mapped || layer->m_readyToDelete)
                    continue;

                const auto layerMonitor = layer->m_monitor.lock();
                if (!layerMonitor || layerMonitor != monitor || layer->m_layer != static_cast<int>(layerKind))
                    continue;

                m_renderLayerOriginal(g_pHyprRenderer.get(), layer, monitor, now, false, false);
            }
        }
    };

    const auto renderBackgroundLayersIntoFramebuffer = [&](CFramebuffer& targetFramebuffer, const Time::steady_tp& now) -> bool {
        const bool previousBlockScreenShaderLocal = g_pHyprOpenGL->m_renderData.blockScreenShader;
        CRegion     fakeDamage{0, 0, static_cast<int>(std::lround(targetFramebuffer.m_size.x)), static_cast<int>(std::lround(targetFramebuffer.m_size.y))};
        if (!g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &targetFramebuffer)) {
            g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockScreenShaderLocal;
            return false;
        }

        g_pHyprOpenGL->m_renderData.blockScreenShader = true;
        g_pHyprOpenGL->clear(CHyprColor{0.05, 0.06, 0.08, 1.0});
        renderBackgroundLayers(now);
        g_pHyprRenderer->endRender();
        g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockScreenShaderLocal;
        return true;
    };

    ++m_stripSnapshotRenderDepth;
    g_pHyprRenderer->makeEGLCurrent();
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    if (renderWorkspaceContents) {
        m_stripPreviewContext.active = true;
        m_stripPreviewContext.monitor = monitor;
        m_stripPreviewContext.state = std::move(previewState);
        m_stripPreviewContext.framebufferSize = Vector2D{static_cast<double>(fbWidth), static_cast<double>(fbHeight)};
        applyFullscreenOverrideForState(m_stripPreviewContext.state, true);
    }

    // Rendering the current active workspace into a thumbnail can reuse the
    // live monitor state directly. Avoid flipping active workspace / special
    // workspace / desktop animation state for that path.
    if (renderWorkspaceContents && targetWorkspace && previousWorkspace && !targetIsCurrentWorkspace && previousWorkspace->m_visible) {
        previousWorkspace->m_visible = false;
        previousVisibilityChanged = true;
        g_pDesktopAnimationManager->startAnimation(previousWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);
        previousAnimationActivated = true;
    }

    if (renderWorkspaceContents && targetWorkspace && !targetIsCurrentWorkspace)
        monitor->m_activeSpecialWorkspace.reset();

    if (renderWorkspaceContents && targetWorkspace && !targetIsCurrentWorkspace) {
        monitor->m_activeWorkspace = targetWorkspace;
        if (!targetWorkspace->m_visible) {
            targetWorkspace->m_visible = true;
            targetVisibilityChanged = true;
        }
        g_pDesktopAnimationManager->startAnimation(targetWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
        targetAnimationActivated = true;
    }

    const auto renderNow = Time::steadyNow();
    bool       renderedScaledBackgroundOnly = false;
    if (!renderWorkspaceContents) {
        const int backgroundFbWidth = std::max(1, static_cast<int>(std::ceil(static_cast<double>(monitor->m_size.x) * renderScaleForMonitor(monitor))));
        const int backgroundFbHeight = std::max(1, static_cast<int>(std::ceil(static_cast<double>(monitor->m_size.y) * renderScaleForMonitor(monitor))));
        CFramebuffer backgroundFramebuffer;
        if (backgroundFramebuffer.alloc(backgroundFbWidth, backgroundFbHeight)) {
            setFramebufferLinearFiltering(backgroundFramebuffer);
            renderedScaledBackgroundOnly =
                renderBackgroundLayersIntoFramebuffer(backgroundFramebuffer, renderNow) &&
                blitFramebufferRegion(backgroundFramebuffer, snapshot->framebuffer, makeRect(0.0, 0.0, backgroundFramebuffer.m_size.x, backgroundFramebuffer.m_size.y),
                                     makeRect(0.0, 0.0, snapshot->framebuffer.m_size.x, snapshot->framebuffer.m_size.y));
        }
    }

    if (!renderedScaledBackgroundOnly) {
        CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->beginRender(monitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &snapshot->framebuffer);
        g_pHyprOpenGL->clear(CHyprColor{0.05, 0.06, 0.08, 1.0});
        renderBackgroundLayers(renderNow);
        if (renderWorkspaceContents && targetWorkspace && renderWorkspaceFn)
            renderWorkspaceFn(g_pHyprRenderer.get(), monitor, targetWorkspace, renderNow, CBox{0.0, 0.0, static_cast<double>(fbWidth), static_cast<double>(fbHeight)});
        g_pHyprOpenGL->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
        g_pHyprOpenGL->m_renderData.blockScreenShader = previousBlockScreenShader;
    }
    if (renderWorkspaceContents) {
        applyFullscreenOverrideForState(m_stripPreviewContext.state, false);
        m_stripPreviewContext.state = {};
        m_stripPreviewContext.framebufferSize = {};
        m_stripPreviewContext.monitor.reset();
        m_stripPreviewContext.active = false;
    }

    if (targetAnimationActivated)
        g_pDesktopAnimationManager->startAnimation(targetWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);
    if (targetVisibilityChanged && targetWorkspace)
        targetWorkspace->m_visible = false;

    if (renderWorkspaceContents && !targetIsCurrentWorkspace) {
        monitor->m_activeSpecialWorkspace = previousSpecialWorkspace;
        monitor->m_activeWorkspace = previousWorkspace;
    }

    if (previousVisibilityChanged && previousWorkspace)
        previousWorkspace->m_visible = true;
    if (previousAnimationActivated && previousWorkspace)
        g_pDesktopAnimationManager->startAnimation(previousWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);

    g_pHyprRenderer->m_bBlockSurfaceFeedback = previousBlockSurfaceFeedback;
    --m_stripSnapshotRenderDepth;

    entry.snapshot = std::move(snapshot);
}

void OverviewController::refreshWorkspaceStripSnapshots() {
    if (!workspaceStripEnabled(m_state) || m_state.stripEntries.empty()) {
        for (auto& entry : m_state.stripEntries)
            entry.snapshot.reset();
        m_stripSnapshotsDirty = false;
        return;
    }

    if (m_stripSnapshotRenderDepth > 0 || (g_pHyprOpenGL && g_pHyprOpenGL->m_renderData.pMonitor)) {
        m_stripSnapshotsDirty = true;
        scheduleWorkspaceStripSnapshotRefresh();
        return;
    }

    m_stripSnapshotsDirty = false;
    for (auto& entry : m_state.stripEntries)
        renderWorkspaceStripSnapshot(entry);
}

void OverviewController::scheduleWorkspaceStripSnapshotRefresh() {
    if (m_stripSnapshotRefreshScheduled || !g_pEventLoopManager)
        return;

    m_stripSnapshotRefreshScheduled = true;
    g_pEventLoopManager->doLater([this] {
        if (g_controller != this)
            return;

        m_stripSnapshotRefreshScheduled = false;
        if (!m_stripSnapshotsDirty)
            return;

        refreshWorkspaceStripSnapshots();
    });
}

void OverviewController::renderWorkspaceStrip() const {
    const double progress = visualProgress();
    if (progress <= 0.0 || m_state.stripEntries.empty())
        return;

    const auto renderMonitor = g_pHyprOpenGL->m_renderData.pMonitor.lock();
    if (!renderMonitor)
        return;

    if (debugLogsEnabled()) {
        static int debugRenderSamples = 0;
        if (debugRenderSamples < 4) {
            std::ostringstream out;
            out << "[hymission] strip render progress=" << progress << " entries=" << m_state.stripEntries.size();
            if (!m_state.stripEntries.empty())
                out << " firstRect=" << rectToString(m_state.stripEntries.front().rect);
            debugLog(out.str());
            ++debugRenderSamples;
        }
    }

    const Rect bandGlobal = workspaceStripBandRectForMonitor(renderMonitor, m_state);
    if (bandGlobal.width > 0.0 && bandGlobal.height > 0.0) {
        const Rect band = rectToMonitorRenderLocal(animatedWorkspaceStripRect(bandGlobal, renderMonitor), renderMonitor);
        g_pHyprOpenGL->renderRect(toBox(band), CHyprColor(0.03, 0.07, 0.14, 0.24 * progress), {.blur = true, .blurA = 1.0F});
    }

    for (std::size_t index = 0; index < m_state.stripEntries.size(); ++index) {
        const auto& entry = m_state.stripEntries[index];
        if (!entry.monitor || entry.monitor != renderMonitor)
            continue;

        const bool hovered = m_state.hoveredStripIndex && *m_state.hoveredStripIndex == index;
        const Rect outerGlobal = animatedWorkspaceStripRect(entry.rect, renderMonitor);
        const Rect outer = rectToMonitorLocal(outerGlobal, renderMonitor);
        if (outer.width <= 0.0 || outer.height <= 0.0)
            continue;

        const Rect thumb = rectToMonitorLocal(outerGlobal, renderMonitor);
        const Rect thumbRender = scaleRectForRender(thumb, renderMonitor);

        const CHyprColor cardColor = entry.newWorkspaceSlot ? CHyprColor(0.11, 0.16, 0.23, 0.26 * progress) :
                                      entry.syntheticEmpty ? CHyprColor(0.06, 0.10, 0.16, 0.18 * progress) :
                                      entry.active ? CHyprColor(0.10, 0.18, 0.32, 0.24 * progress) :
                                                     CHyprColor(0.05, 0.09, 0.15, 0.18 * progress);
        const CHyprColor stateOverlayColor =
            hovered ? CHyprColor(1.0, 1.0, 1.0, 0.06 * progress) : entry.active ? CHyprColor(0.34, 0.58, 0.95, 0.10 * progress) : CHyprColor(0.0, 0.0, 0.0, 0.0);

        g_pHyprOpenGL->renderRect(toBox(thumbRender), cardColor, {.blur = true, .blurA = 1.0F});

        if (!entry.newWorkspaceSlot && entry.snapshot && entry.snapshot->framebuffer.isAllocated() && entry.snapshot->framebuffer.getTexture()) {
            g_pHyprOpenGL->renderTexture(entry.snapshot->framebuffer.getTexture(), toBox(thumbRender), {.a = static_cast<float>(std::clamp(progress, 0.0, 1.0))});
        }

        if (stateOverlayColor.a > 0.0) {
            g_pHyprOpenGL->renderRect(toBox(thumbRender), stateOverlayColor, {});
        }

        if (entry.newWorkspaceSlot) {
            const double plusArmLength = std::min(thumb.width, thumb.height) * 0.28;
            const double plusThickness = std::max(2.0, std::round(std::min(thumb.width, thumb.height) * 0.03));
            const Rect plusHorizontal =
                makeRect(thumb.centerX() - plusArmLength * 0.5, thumb.centerY() - plusThickness * 0.5, plusArmLength, plusThickness);
            const Rect plusVertical =
                makeRect(thumb.centerX() - plusThickness * 0.5, thumb.centerY() - plusArmLength * 0.5, plusThickness, plusArmLength);
            g_pHyprOpenGL->renderRect(toBox(scaleRectForRender(plusHorizontal, renderMonitor)), CHyprColor(0.97, 0.985, 1.0, 0.88 * progress), {});
            g_pHyprOpenGL->renderRect(toBox(scaleRectForRender(plusVertical, renderMonitor)), CHyprColor(0.97, 0.985, 1.0, 0.88 * progress), {});
        }
    }
}

void OverviewController::buildWorkspaceStripEntries(State& state) const {
    state.stripEntries.clear();
    state.hoveredStripIndex.reset();

    if (!workspaceStripEnabled(state))
        return;

    const std::string            anchor = workspaceStripAnchor();
    const WorkspaceStripEmptyMode emptyMode = workspaceStripEmptyMode();
    const bool                   horizontal = anchor == "top";
    const double                 stripGap = std::clamp(workspaceStripGap() * 0.5, 8.0, 24.0);
    const double                 padding = 12.0;

    for (const auto& monitor : state.participatingMonitors) {
        if (!monitor)
            continue;

        std::vector<PHLWORKSPACE> normalWorkspaces;
        const auto allWorkspaces = g_pCompositor->getWorkspacesCopy();
        normalWorkspaces.reserve(allWorkspaces.size());
        for (const auto& workspace : allWorkspaces) {
            if (!workspace || workspace->m_isSpecialWorkspace)
                continue;

            const auto workspaceMonitor = workspace->m_monitor.lock();
            if (workspaceMonitor == monitor)
                normalWorkspaces.push_back(workspace);
        }

        if (monitor->m_activeWorkspace && !monitor->m_activeWorkspace->m_isSpecialWorkspace && !containsHandle(normalWorkspaces, monitor->m_activeWorkspace))
            normalWorkspaces.push_back(monitor->m_activeWorkspace);

        std::stable_sort(normalWorkspaces.begin(), normalWorkspaces.end(), [](const PHLWORKSPACE& lhs, const PHLWORKSPACE& rhs) {
            if (!lhs || !rhs)
                return static_cast<bool>(lhs);
            return lhs->m_id < rhs->m_id;
        });

        std::unordered_map<WORKSPACEID, PHLWORKSPACE> normalById;
        for (const auto& workspace : normalWorkspaces) {
            if (workspace)
                normalById.emplace(workspace->m_id, workspace);
        }

        std::vector<int64_t> workspaceIds;
        workspaceIds.reserve(normalWorkspaces.size());
        for (const auto& workspace : normalWorkspaces) {
            if (workspace)
                workspaceIds.push_back(workspace->m_id);
        }

        const auto stripWorkspaceIds = expandWorkspaceStripWorkspaceIds(workspaceIds, emptyMode);

        std::vector<WorkspaceStripEntry> monitorEntries;
        for (const auto rawWorkspaceId : stripWorkspaceIds) {
            const auto workspaceId = static_cast<WORKSPACEID>(rawWorkspaceId);
            const auto it = normalById.find(workspaceId);
            if (it != normalById.end()) {
                monitorEntries.push_back({
                    .monitor = monitor,
                    .workspace = it->second,
                    .workspaceId = workspaceId,
                    .workspaceName = it->second->m_name,
                    .syntheticEmpty = false,
                    .newWorkspaceSlot = false,
                    .active = monitor->m_activeWorkspace == it->second,
                });
            } else {
                monitorEntries.push_back({
                    .monitor = monitor,
                    .workspace = {},
                    .workspaceId = workspaceId,
                    .workspaceName = std::to_string(workspaceId),
                    .syntheticEmpty = true,
                    .newWorkspaceSlot = false,
                    .active = false,
                });
            }
        }

        WORKSPACEID nextWorkspaceId = stripWorkspaceIds.empty() ? 1 : static_cast<WORKSPACEID>(std::max<int64_t>(stripWorkspaceIds.back(), 0) + 1);
        while (g_pCompositor->getWorkspaceByID(nextWorkspaceId))
            ++nextWorkspaceId;

        monitorEntries.push_back({
            .monitor = monitor,
            .workspace = {},
            .workspaceId = nextWorkspaceId,
            .workspaceName = std::to_string(nextWorkspaceId),
            .syntheticEmpty = true,
            .newWorkspaceSlot = true,
            .active = false,
        });

        if (monitorEntries.empty())
            continue;

        const Rect band = workspaceStripBandRectForMonitor(monitor, state);
        const double innerWidth = std::max(1.0, band.width - padding * 2.0);
        const double innerHeight = std::max(1.0, band.height - padding * 2.0);
        double scale = horizontal ? innerHeight / static_cast<double>(monitor->m_size.y) : innerWidth / static_cast<double>(monitor->m_size.x);
        double entryWidth = std::max(24.0, static_cast<double>(monitor->m_size.x) * scale);
        double entryHeight = std::max(24.0, static_cast<double>(monitor->m_size.y) * scale);

        if (horizontal) {
            const double totalWidth = entryWidth * monitorEntries.size() + stripGap * std::max<std::size_t>(monitorEntries.size() - 1, 0);
            if (totalWidth > innerWidth) {
                const double fitScale = innerWidth / std::max(1.0, totalWidth);
                entryWidth *= fitScale;
                entryHeight *= fitScale;
            }

            const double effectiveGap = monitorEntries.size() > 1 ? std::min(stripGap, std::max(4.0, (innerWidth - entryWidth * monitorEntries.size()) / (monitorEntries.size() - 1))) : 0.0;
            const double contentWidth = entryWidth * monitorEntries.size() + effectiveGap * std::max<std::size_t>(monitorEntries.size() - 1, 0);
            double cursorX = band.x + padding + std::max(0.0, (innerWidth - contentWidth) * 0.5);
            const double y = band.y + (band.height - entryHeight) * 0.5;
            for (auto& entry : monitorEntries) {
                entry.rect = makeRect(cursorX, y, entryWidth, entryHeight);
                cursorX += entryWidth + effectiveGap;
            }
        } else {
            const double totalHeight = entryHeight * monitorEntries.size() + stripGap * std::max<std::size_t>(monitorEntries.size() - 1, 0);
            if (totalHeight > innerHeight) {
                const double fitScale = innerHeight / std::max(1.0, totalHeight);
                entryWidth *= fitScale;
                entryHeight *= fitScale;
            }

            const double effectiveGap = monitorEntries.size() > 1 ? std::min(stripGap, std::max(4.0, (innerHeight - entryHeight * monitorEntries.size()) / (monitorEntries.size() - 1))) : 0.0;
            const double contentHeight = entryHeight * monitorEntries.size() + effectiveGap * std::max<std::size_t>(monitorEntries.size() - 1, 0);
            const double x = band.x + (band.width - entryWidth) * 0.5;
            double cursorY = band.y + padding + std::max(0.0, (innerHeight - contentHeight) * 0.5);
            for (auto& entry : monitorEntries) {
                entry.rect = makeRect(x, cursorY, entryWidth, entryHeight);
                cursorY += entryHeight + effectiveGap;
            }
        }

        state.stripEntries.insert(state.stripEntries.end(), monitorEntries.begin(), monitorEntries.end());
    }

    const auto focusWindow = state.focusDuringOverview ? state.focusDuringOverview : Desktop::focusState()->window();
    for (const auto& window : g_pCompositor->m_windows) {
        if (!window || !window->m_isMapped || window->m_fadingOut || window->isHidden())
            continue;

        if (!windowHasUsableStateGeometry(window))
            continue;

        const bool useGoalGeometry = shouldUseGoalGeometryForStateSnapshot(window);
        const auto naturalGlobal = stateSnapshotGlobalRectForWindow(window, useGoalGeometry);
        const auto previewAlpha = std::clamp(window->m_activeInactiveAlpha->value(), 0.0F, 1.0F);
        const auto targetMonitor = preferredMonitorForWindow(window, state);

        for (auto& entry : state.stripEntries) {
            if (!entry.monitor)
                continue;

            if (window->m_pinned) {
                if (entry.monitor == targetMonitor && entry.workspace == entry.monitor->m_activeWorkspace) {
                    entry.windows.push_back({
                        .naturalGlobal = naturalGlobal,
                        .alpha = previewAlpha,
                        .focused = window == focusWindow,
                    });
                }
                continue;
            }

            if (window->m_workspace && entry.workspace == window->m_workspace) {
                entry.windows.push_back({
                    .naturalGlobal = naturalGlobal,
                    .alpha = previewAlpha,
                    .focused = window == focusWindow,
                });
            }
        }
    }

    if (debugLogsEnabled()) {
        std::ostringstream out;
        out << "[hymission] strip build entries=" << state.stripEntries.size() << " monitors=" << state.participatingMonitors.size() << " anchor=" << anchor;
        for (std::size_t i = 0; i < state.stripEntries.size() && i < 8; ++i) {
            const auto& entry = state.stripEntries[i];
            out << " | #" << i << " mon=" << (entry.monitor ? entry.monitor->m_name : "?") << " ws=" << entry.workspaceId
                << " rect=" << rectToString(entry.rect) << " windows=" << entry.windows.size() << " active=" << (entry.active ? 1 : 0)
                << " new=" << (entry.newWorkspaceSlot ? 1 : 0);
        }
        debugLog(out.str());
    }
}

OverviewController::State OverviewController::buildState(const PHLMONITOR& monitor, ScopeOverride requestedScope, const std::vector<WorkspaceOverride>& workspaceOverrides,
                                                         bool keepEmptyParticipatingMonitors, bool suppressWorkspaceStrip,
                                                         PHLWINDOW preferredSelectedWindow) const {
    State state;
    if (!monitor || !monitor->m_activeWorkspace)
        return state;

    const bool preserveExistingOrder =
        workspaceOverrides.empty() && isVisible() && requestedScope == m_state.collectionPolicy.requestedScope && (!m_state.ownerMonitor || monitor == m_state.ownerMonitor);

    state.ownerMonitor = monitor;
    state.ownerWorkspace = monitor->m_activeWorkspace;
    state.collectionPolicy = loadCollectionPolicy(requestedScope);
    state.suppressWorkspaceStrip = suppressWorkspaceStrip;
    const auto focusedWindow = Desktop::focusState()->window();
    state.focusBeforeOpen = focusedWindow;
    state.focusDuringOverview = preferredSelectedWindow ? preferredSelectedWindow : focusedWindow;

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

        std::vector<std::size_t> visibleOrder;
        visibleOrder.reserve(m_state.windows.size());
        for (std::size_t i = 0; i < m_state.windows.size(); ++i) {
            if (m_state.windows[i].window)
                visibleOrder.push_back(i);
        }

        // Preserve the currently visible preview order rather than the raw
        // storage order. Selection-emphasis relayouts should only resize and
        // nudge neighbors, not reshuffle previews that are already on screen.
        std::stable_sort(visibleOrder.begin(), visibleOrder.end(), [&](std::size_t lhsIndex, std::size_t rhsIndex) {
            const auto& lhs = m_state.windows[lhsIndex];
            const auto& rhs = m_state.windows[rhsIndex];
            const auto lhsMonitorId = lhs.targetMonitor ? lhs.targetMonitor->m_id : MONITOR_INVALID;
            const auto rhsMonitorId = rhs.targetMonitor ? rhs.targetMonitor->m_id : MONITOR_INVALID;
            if (lhsMonitorId != rhsMonitorId)
                return lhsMonitorId < rhsMonitorId;

            const Rect lhsRect = currentPreviewRect(lhs);
            const Rect rhsRect = currentPreviewRect(rhs);
            if (std::abs(lhsRect.y - rhsRect.y) > 0.5)
                return lhsRect.y < rhsRect.y;
            if (std::abs(lhsRect.x - rhsRect.x) > 0.5)
                return lhsRect.x < rhsRect.x;
            return lhsIndex < rhsIndex;
        });

        for (std::size_t order = 0; order < visibleOrder.size(); ++order) {
            const auto& managed = m_state.windows[visibleOrder[order]];
            if (managed.window)
                previousOrder.emplace(managed.window, order);
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

    const bool orderByRecentUse = !preserveExistingOrder && shouldUseRecentWindowOrdering(state);
    if (orderByRecentUse && !m_windowMruSerials.empty()) {
        std::stable_sort(candidates.begin(), candidates.end(), [&](const PHLWINDOW& lhs, const PHLWINDOW& rhs) {
            const auto lhsIt = m_windowMruSerials.find(lhs);
            const auto rhsIt = m_windowMruSerials.find(rhs);
            const bool lhsKnown = lhsIt != m_windowMruSerials.end();
            const bool rhsKnown = rhsIt != m_windowMruSerials.end();

            if (lhsKnown != rhsKnown)
                return lhsKnown;
            if (!lhsKnown)
                return false;
            if (lhsIt->second != rhsIt->second)
                return lhsIt->second > rhsIt->second;

            return false;
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
        const Rect naturalGlobal = stateSnapshotGlobalRectForWindow(window, useGoalGeometry);
        const Rect layoutGlobal = layoutAnchorGlobalRectForWindow(window, useGoalGeometry);
        const std::size_t windowIndex = state.windows.size();

        inputsByMonitor[targetMonitor->m_id].push_back({
            .index = windowIndex,
            .natural =
                {
                    layoutGlobal.x - targetMonitor->m_position.x,
                    layoutGlobal.y - targetMonitor->m_position.y,
                    layoutGlobal.width,
                    layoutGlobal.height,
                },
            .label = window->m_title,
            .rowGroup = rowGroupForWindow(window),
            .layoutEmphasis = 1.0,
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

    std::vector<PHLMONITOR> activeParticipatingMonitors;
    MissionControlLayout engine;
    LayoutConfig config = loadLayoutConfig();
    config.preserveInputOrder = preserveExistingOrder || orderByRecentUse;
    config.forceRowGroups = useWorkspaceRows;
    for (const auto& candidateMonitor : state.participatingMonitors) {
        if (!candidateMonitor)
            continue;

        const auto inputsIt = inputsByMonitor.find(candidateMonitor->m_id);
        const bool hasStripWorkspace = workspaceStripEnabled(state) && static_cast<bool>(candidateMonitor->m_activeWorkspace);
        const bool keepMonitor = keepEmptyParticipatingMonitors && overrideForMonitor(candidateMonitor);
        if ((inputsIt == inputsByMonitor.end() || inputsIt->second.empty()) && !keepMonitor && !hasStripWorkspace)
            continue;

        activeParticipatingMonitors.push_back(candidateMonitor);
        if (inputsIt == inputsByMonitor.end() || inputsIt->second.empty())
            continue;

        const Rect previewArea = overviewContentRectForMonitor(candidateMonitor, state);
        const auto slots =
            engine.compute(inputsIt->second,
                           previewArea,
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
    buildWorkspaceStripEntries(state);

    const auto selectionTarget = preferredSelectedWindow ? preferredSelectedWindow : focusedWindow;
    for (std::size_t index = 0; index < state.windows.size(); ++index) {
        if (state.windows[index].window == selectionTarget) {
            state.selectedIndex = index;
            break;
        }
    }

    if (!state.selectedIndex) {
        for (std::size_t index = 0; index < state.windows.size(); ++index) {
            if (state.windows[index].window == focusedWindow) {
                state.selectedIndex = index;
                break;
            }
        }
    }

    if (!state.selectedIndex && !state.windows.empty())
        state.selectedIndex = 0;

    if (state.selectedIndex && *state.selectedIndex < state.windows.size())
        state.focusDuringOverview = state.windows[*state.selectedIndex].window;
    else
        state.focusDuringOverview = focusedWindow;

    return state;
}

} // namespace hymission
