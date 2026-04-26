#include <memory>
#include <string>

#include <hyprland/src/plugins/PluginAPI.hpp>

#include "overview_controller.hpp"

inline HANDLE g_pluginHandle = nullptr;
inline std::unique_ptr<hymission::OverviewController> g_overviewController;

namespace {
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

#define CONF(name, value) HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hymission:" name, {value})
    CONF("outer_padding", 32L);
    CONF("outer_padding_top", 92L);
    CONF("outer_padding_right", 32L);
    CONF("outer_padding_bottom", 32L);
    CONF("outer_padding_left", 32L);
    CONF("row_spacing", 32L);
    CONF("column_spacing", 32L);
    CONF("min_window_length", 120L);
    CONF("min_preview_short_edge", 32L);
    CONF("small_window_boost", 1.35F);
    CONF("max_preview_scale", 0.95F);
    CONF("min_slot_scale", 0.10F);
    CONF("layout_scale_weight", 1.0F);
    CONF("layout_space_weight", 0.10F);
    CONF("expand_selected_window", 1L);
    CONF("overview_focus_follows_mouse", 1L);
    CONF("multi_workspace_sort_recent_first", 1L);
    CONF("gesture_invert_vertical", 0L);
    CONF("one_workspace_per_row", 0L);
    CONF("only_active_workspace", 0L);
    CONF("only_active_monitor", 0L);
    CONF("show_special", 0L);
    CONF("toggle_switch_mode", 1L);
    CONF("switch_toggle_auto_next", 1L);
    CONF("workspace_change_keeps_overview", 1L);
    CONF("workspace_strip_thickness", 160L);
    CONF("workspace_strip_gap", 24L);
    CONF("hide_bar_when_strip", 1L);
    CONF("hide_bar_animation", 1L);
    CONF("hide_bar_animation_blur", 1L);
    CONF("hide_bar_animation_move_multiplier", 0.8F);
    CONF("hide_bar_animation_scale_divisor", 1.1F);
    CONF("hide_bar_animation_alpha_end", 0.0F);
    CONF("bar_single_mission_control", 0L);
    CONF("show_focus_indicator", 0L);
    CONF("debug_logs", 0L);
    CONF("debug_surface_logs", 0L);
#undef CONF
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hymission:layout_engine", Hyprlang::STRING{"grid"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hymission:workspace_strip_anchor", Hyprlang::STRING{"left"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hymission:workspace_strip_empty_mode", Hyprlang::STRING{"existing"});
    HyprlandAPI::addConfigValue(g_pluginHandle, "plugin:hymission:switch_release_key", Hyprlang::STRING{"Super_L"});

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
        .version = "0.2.1",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_overviewController.reset();
}
