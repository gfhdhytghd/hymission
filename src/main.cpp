#include <memory>
#include <string>

#include <hyprland/src/plugins/PluginAPI.hpp>

#include "overview_controller.hpp"

inline HANDLE g_pluginHandle = nullptr;
inline std::unique_ptr<hymission::OverviewController> g_overviewController;

namespace {
SDispatchResult dispatchToggle(const std::string&) {
    return g_overviewController ? g_overviewController->toggle() : SDispatchResult{.success = false, .error = "overview controller unavailable"};
}

SDispatchResult dispatchOpen(const std::string&) {
    return g_overviewController ? g_overviewController->open() : SDispatchResult{.success = false, .error = "overview controller unavailable"};
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
    CONF("outer_padding", 48L);
    CONF("row_spacing", 32L);
    CONF("column_spacing", 32L);
    CONF("min_window_length", 120L);
    CONF("small_window_boost", 1.35F);
    CONF("max_preview_scale", 0.95F);
    CONF("min_slot_scale", 0.10F);
    CONF("layout_scale_weight", 1.0F);
    CONF("layout_space_weight", 0.10F);
    CONF("overview_focus_follows_mouse", 0L);
#undef CONF

    g_overviewController = std::make_unique<hymission::OverviewController>(g_pluginHandle);
    if (!g_overviewController->initialize()) {
        HyprlandAPI::addNotification(g_pluginHandle, "[hymission] failed to initialize overview controller", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
    }

    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hymission:toggle", dispatchToggle);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hymission:open", dispatchOpen);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hymission:close", dispatchClose);
    HyprlandAPI::addDispatcherV2(g_pluginHandle, "hymission:debug_current_layout", dispatchDebugCurrentLayout);
    HyprlandAPI::reloadConfig();

    return {
        .name = "hymission",
        .description = "Mission Control style overview prototype",
        .author = "wilf",
        .version = "0.1.0",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_overviewController.reset();
}
