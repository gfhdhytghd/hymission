#include <memory>
#include <sstream>
#include <string>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

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

int luaDispatchResult(lua_State* L, const SDispatchResult& result) {
    if (result.success)
        return 0;

    lua_pushstring(L, result.error.empty() ? "hymission function failed" : result.error.c_str());
    return lua_error(L);
}

std::string luaOptionalString(lua_State* L, int index) {
    if (lua_gettop(L) < index || lua_isnil(L, index))
        return {};

    return luaL_checkstring(L, index);
}

std::string luaTableStringField(lua_State* L, const char* field, const std::string& fallback = {}) {
    lua_getfield(L, 1, field);
    std::string result = fallback;
    if (!lua_isnil(L, -1))
        result = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return result;
}

std::string luaRequiredTableStringField(lua_State* L, const char* field) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "hl.plugin.hymission.gesture: missing field \"%s\"", field);
        return {};
    }

    std::string result = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return result;
}

int luaRequiredTableIntegerField(lua_State* L, const char* field) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "hl.plugin.hymission.gesture: missing field \"%s\"", field);
        return 0;
    }

    const int result = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    return result;
}

bool luaTableBoolField(lua_State* L, const char* field, bool fallback = false) {
    lua_getfield(L, 1, field);
    const bool result = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return result;
}

bool luaTableHasField(lua_State* L, const char* field) {
    lua_getfield(L, 1, field);
    const bool result = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return result;
}

std::string normalizeHymissionDispatcher(std::string dispatcher) {
    if (dispatcher == "toggle" || dispatcher == "hymission.toggle")
        return "hymission:toggle";
    if (dispatcher == "open" || dispatcher == "hymission.open")
        return "hymission:open";
    if (dispatcher == "close" || dispatcher == "hymission.close")
        return "hymission:close";
    if (dispatcher == "debug_current_layout" || dispatcher == "debugCurrentLayout" || dispatcher == "hymission.debug_current_layout" ||
        dispatcher == "hymission.debugCurrentLayout")
        return "hymission:debug_current_layout";
    if (dispatcher == "scroll" || dispatcher == "hymission.scroll")
        return "hymission:scroll";
    return dispatcher;
}

int luaToggle(lua_State* L) {
    return luaDispatchResult(L, dispatchToggle(luaOptionalString(L, 1)));
}

int luaOpen(lua_State* L) {
    return luaDispatchResult(L, dispatchOpen(luaOptionalString(L, 1)));
}

int luaClose(lua_State* L) {
    return luaDispatchResult(L, dispatchClose(""));
}

int luaDebugCurrentLayout(lua_State* L) {
    return luaDispatchResult(L, dispatchDebugCurrentLayout(""));
}

int luaDispatch(lua_State* L) {
    const std::string dispatcher = normalizeHymissionDispatcher(luaL_checkstring(L, 1));
    const std::string args       = luaOptionalString(L, 2);

    if (dispatcher == "hymission:toggle")
        return luaDispatchResult(L, dispatchToggle(args));
    if (dispatcher == "hymission:open")
        return luaDispatchResult(L, dispatchOpen(args));
    if (dispatcher == "hymission:close")
        return luaDispatchResult(L, dispatchClose(args));
    if (dispatcher == "hymission:debug_current_layout")
        return luaDispatchResult(L, dispatchDebugCurrentLayout(args));

    lua_pushstring(L, ("unknown hymission dispatcher: " + dispatcher).c_str());
    return lua_error(L);
}

std::string luaGestureValueFromTable(lua_State* L) {
    const int         fingers = luaRequiredTableIntegerField(L, "fingers");
    const std::string direction = luaRequiredTableStringField(L, "direction");

    std::string action = luaTableStringField(L, "dispatcher");
    if (action.empty())
        action = luaRequiredTableStringField(L, "action");

    std::ostringstream value;
    value << fingers << ", " << direction;

    const std::string mods = luaTableStringField(L, "mods", luaTableStringField(L, "mod"));
    if (!mods.empty())
        value << ", mod:" << mods;

    if (luaTableHasField(L, "scale")) {
        lua_getfield(L, 1, "scale");
        value << ", scale:" << luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }

    if (action == "workspace") {
        value << ", workspace";
        return value.str();
    }

    const std::string dispatcher = normalizeHymissionDispatcher(action);
    std::string       args       = luaTableStringField(L, "args");
    if (args.empty())
        args = luaTableStringField(L, "scope");
    if (args.empty())
        args = luaTableStringField(L, "mode");
    if (args.empty() && (luaTableBoolField(L, "recommand") || luaTableBoolField(L, "recommend")))
        args = "recommand";
    if (args.empty() && dispatcher == "hymission:scroll")
        args = "layout";

    value << ", dispatcher, " << dispatcher;
    if (!args.empty())
        value << "," << args;

    return value.str();
}

int luaGesture(lua_State* L) {
    std::string keyword = "gesture";
    std::string value;

    if (lua_istable(L, 1)) {
        if (luaTableBoolField(L, "disable_inhibit") || luaTableBoolField(L, "disableInhibit"))
            keyword = "gesturep";
        value = luaGestureValueFromTable(L);
    } else {
        value = luaL_checkstring(L, 1);
        if (lua_gettop(L) >= 2 && lua_isboolean(L, 2) && lua_toboolean(L, 2))
            keyword = "gesturep";
    }

    if (!g_overviewController) {
        lua_pushstring(L, "overview controller unavailable");
        return lua_error(L);
    }

    const auto error = g_overviewController->handleGestureConfigHook(keyword, value);
    if (error) {
        lua_pushstring(L, error->empty() ? "failed to register hymission gesture" : error->c_str());
        return lua_error(L);
    }

    return 0;
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
    FLOAT_CONF("workspace_overview_max_preview_scale", 0.95F);
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

    if (Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA) {
        const auto registerLuaFunction = [&](const char* name, PLUGIN_LUA_FN fn) {
            if (!HyprlandAPI::addLuaFunction(g_pluginHandle, "hymission", name, fn)) {
                HyprlandAPI::addNotification(
                    g_pluginHandle,
                    std::string("[hymission] failed to register lua function hl.plugin.hymission.") + name,
                    CHyprColor(1.0, 0.2, 0.2, 1.0),
                    5000);
            }
        };

        registerLuaFunction("toggle", luaToggle);
        registerLuaFunction("open", luaOpen);
        registerLuaFunction("close", luaClose);
        registerLuaFunction("debug_current_layout", luaDebugCurrentLayout);
        registerLuaFunction("dispatch", luaDispatch);
        registerLuaFunction("gesture", luaGesture);
    }

    if (!HyprlandAPI::reloadConfig()) {
        HyprlandAPI::addNotification(g_pluginHandle, "[hymission] reloadConfig failed", CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
    }

    return {
        .name = "hymission",
        .description = "Mission Control style overview prototype",
        .author = "wilf",
        .version = "0.3.3",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_overviewController.reset();
}
