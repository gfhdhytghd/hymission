#include <memory>
#include <string>

#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include "overview_controller.hpp"

inline HANDLE g_pluginHandle = nullptr;
inline std::unique_ptr<hymission::OverviewController> g_overviewController;

namespace {
bool addConfigValue(SP<Config::Values::IValue> value) {
    const std::string name = value->name();

    if (HyprlandAPI::addConfigValueV2(g_pluginHandle, value))
        return true;

    HyprlandAPI::addNotification(
        g_pluginHandle,
        "[hymission] failed to register config value " + name,
        CHyprColor(1.0, 0.2, 0.2, 1.0),
        5000);
    return false;
}

bool addIntConfig(const char* name, Config::INTEGER fallback) {
    return addConfigValue(makeShared<Config::Values::CIntValue>(name, "", fallback));
}

bool addFloatConfig(const char* name, Config::FLOAT fallback) {
    return addConfigValue(makeShared<Config::Values::CFloatValue>(name, "", fallback));
}

bool addStringConfig(const char* name, Config::STRING fallback) {
    return addConfigValue(makeShared<Config::Values::CStringValue>(name, "", fallback));
}

SDispatchResult dispatchToggle(const std::string& args) {
    return g_overviewController ? g_overviewController->toggle(args) : SDispatchResult{.success = false, .error = "overview controller unavailable"};
}

SDispatchResult dispatchOpen(const std::string& args) {
    return g_overviewController ? g_overviewController->open(args) : SDispatchResult{.success = false, .error = "overview controller unavailable"};
}

SDispatchResult dispatchClose(const std::string&) {
    return g_overviewController ? g_overviewController->close() : SDispatchResult{.success = false, .error = "overview controller unavailable"};
}

SDispatchResult dispatchDebugCurrentLayout(const std::string&) {
    return g_overviewController ? g_overviewController->debugCurrentLayout() : SDispatchResult{.success = false, .error = "overview controller unavailable"};
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;

#define INT_CONF(name, value) addIntConfig("plugin:hymission:" name, Config::INTEGER{value})
#define FLOAT_CONF(name, value) addFloatConfig("plugin:hymission:" name, Config::FLOAT{value})
#define STRING_CONF(name, value) addStringConfig("plugin:hymission:" name, Config::STRING{value})
    INT_CONF("outer_padding", 32);
    INT_CONF("outer_padding_top", 92);
    INT_CONF("outer_padding_right", 32);
    INT_CONF("outer_padding_bottom", 32);
    INT_CONF("outer_padding_left", 32);
    INT_CONF("row_spacing", 32);
    INT_CONF("column_spacing", 32);
    INT_CONF("min_window_length", 120);
    INT_CONF("min_preview_short_edge", 32);
    FLOAT_CONF("small_window_boost", 1.35F);
    FLOAT_CONF("max_preview_scale", 0.95F);
    FLOAT_CONF("min_slot_scale", 0.10F);
    FLOAT_CONF("natural_scale_flex", 0.22F);
    FLOAT_CONF("layout_scale_weight", 1.0F);
    FLOAT_CONF("layout_space_weight", 0.10F);
    INT_CONF("expand_selected_window", 1);
    INT_CONF("overview_focus_follows_mouse", 1);
    INT_CONF("multi_workspace_sort_recent_first", 1);
    INT_CONF("niri_mode", 0);
    FLOAT_CONF("niri_scroll_pixels_per_delta", 1.0F);
    FLOAT_CONF("niri_workspace_scale", 1.0F);
    INT_CONF("gesture_invert_vertical", 0);
    INT_CONF("one_workspace_per_row", 0);
    INT_CONF("only_active_workspace", 0);
    INT_CONF("only_active_monitor", 0);
    INT_CONF("show_special", 0);
    INT_CONF("toggle_switch_mode", 1);
    INT_CONF("switch_toggle_auto_next", 1);
    INT_CONF("workspace_change_keeps_overview", 1);
    INT_CONF("workspace_strip_thickness", 160);
    INT_CONF("workspace_strip_gap", 24);
    INT_CONF("hide_bar_when_strip", 1);
    INT_CONF("hide_bar_animation", 1);
    INT_CONF("hide_bar_animation_blur", 1);
    FLOAT_CONF("hide_bar_animation_move_multiplier", 0.8F);
    FLOAT_CONF("hide_bar_animation_scale_divisor", 1.1F);
    FLOAT_CONF("hide_bar_animation_alpha_end", 0.0F);
    INT_CONF("bar_single_mission_control", 0);
    INT_CONF("show_focus_indicator", 0);
    INT_CONF("debug_logs", 0);
    INT_CONF("debug_surface_logs", 0);
    STRING_CONF("layout_engine", "grid");
    STRING_CONF("workspace_strip_anchor", "left");
    STRING_CONF("workspace_strip_empty_mode", "existing");
    STRING_CONF("switch_release_key", "Super_L");
#undef STRING_CONF
#undef FLOAT_CONF
#undef INT_CONF

    g_overviewController = std::make_unique<hymission::OverviewController>(g_pluginHandle);
    if (!g_overviewController->initialize()) {
        HyprlandAPI::addNotification(g_pluginHandle, "[hymission] failed to initialize overview controller", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
    }

    const auto registerDispatcher = [&](const char* name, auto handler) {
        if (!HyprlandAPI::addDispatcherV2(g_pluginHandle, name, handler)) {
            HyprlandAPI::addNotification(g_pluginHandle, std::string("[hymission] failed to register dispatcher ") + name, CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
        }
    };

    registerDispatcher("hymission:toggle", dispatchToggle);
    registerDispatcher("hymission:open", dispatchOpen);
    registerDispatcher("hymission:close", dispatchClose);
    registerDispatcher("hymission:debug_current_layout", dispatchDebugCurrentLayout);

    if (!HyprlandAPI::reloadConfig()) {
        HyprlandAPI::addNotification(g_pluginHandle, "[hymission] reloadConfig failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
    }

    return {
        .name = "hymission",
        .description = "Mission Control style overview prototype",
        .author = "wilf",
        .version = "0.3.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_overviewController.reset();
}
