// Isolated-test fixture only. Not installed and never loaded in the host session.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <linux/input-event-codes.h>
#include <hyprland/src/desktop/state/FocusState.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace {
int swipe(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    if (action=="begin" || action=="try_begin") {
        const bool started=g_pUnifiedWorkspaceSwipe->begin(Desktop::focusState()->monitor());
        const bool active=started && g_pUnifiedWorkspaceSwipe->isGestureInProgress();
        if (!active && action=="begin") return luaL_error(L,"native swipe did not begin");
        lua_pushboolean(L,active); return 1;
    }
    if (action=="update") g_pUnifiedWorkspaceSwipe->update(luaL_checknumber(L,2));
    else if (action=="end") g_pUnifiedWorkspaceSwipe->end();
    else if (action=="cancel") g_pUnifiedWorkspaceSwipe->cancel();
    else return luaL_error(L,"unknown test swipe action");
    return 0;
}
int pointer(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    Event::SCallbackInfo info;
    if (action=="escape") {
        IKeyboard::SKeyEvent e{.keycode=KEY_ESC,.state=WL_KEYBOARD_KEY_STATE_PRESSED};
        Event::bus()->m_events.input.keyboard.key.emit(e,info);
    } else {
        const Vector2D pos{luaL_checknumber(L,2),luaL_checknumber(L,3)};
        Pointer::pointerController()->warpTo(pos);
        if (action=="move") Event::bus()->m_events.input.mouse.move.emit(pos,info);
        else {
            IPointer::SButtonEvent e{.button=BTN_LEFT,.state=action=="press"?WL_POINTER_BUTTON_STATE_PRESSED:WL_POINTER_BUTTON_STATE_RELEASED};
            Event::bus()->m_events.input.mouse.button.emit(e,info);
        }
    }
    lua_pushboolean(L,info.cancelled); return 1;
}
}
APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (!HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","swipe",swipe) ||
        !HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","pointer",pointer)) throw std::runtime_error("test fixture Lua registration failed");
    return {"hymission_tiles_test","private native gesture/input test fixture","hymission","1"};
}
APICALL EXPORT void PLUGIN_EXIT() { g_pUnifiedWorkspaceSwipe->cancel(); }
