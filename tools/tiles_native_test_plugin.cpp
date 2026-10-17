// Isolated-test fixture only. Not installed and never loaded in the host session.
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <linux/input-event-codes.h>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace {
HANDLE fixtureHandle=nullptr;
CFunctionHook* releaseHook=nullptr;
int mouseReleases=0;
void releaseButtons(CInputManager* input) {
    ++mouseReleases;
    reinterpret_cast<decltype(&releaseButtons)>(releaseHook->m_original)(input);
}
int workspaceChange(lua_State* L) {
    if (!releaseHook) {
        for (const auto& symbol:HyprlandAPI::findFunctionsByName(fixtureHandle,"releaseAllMouseButtons"))
            if (symbol.demangled=="CInputManager::releaseAllMouseButtons()") {
                releaseHook=HyprlandAPI::createFunctionHook(fixtureHandle,symbol.address,reinterpret_cast<void*>(&releaseButtons));
                break;
            }
        if (!releaseHook || !releaseHook->hook()) return luaL_error(L,"test release spy unavailable");
    }
    const auto mon=Desktop::focusState()->monitor();
    const auto target=State::Workspace::resolver()->getWorkspaceTargetFromString(luaL_checkstring(L,1),mon);
    auto ws=State::workspaceState()->find(target);
    if (!ws) ws=State::workspaceState()->create(target,mon,true);
    const int before=mouseReleases;
    mon->changeWorkspace(ws,lua_toboolean(L,2),true);
    lua_pushinteger(L,mouseReleases-before);
    return 1;
}
int meta(lua_State* L) {
    const auto keyboard=g_pSeatManager->m_keyboard.lock();
    if (!keyboard || !keyboard->m_xkbKeymap || !keyboard->m_xkbState) return luaL_error(L,"isolated keyboard unavailable");
    const auto index=xkb_keymap_mod_get_index(keyboard->m_xkbKeymap,XKB_MOD_NAME_LOGO);
    if (index==XKB_MOD_INVALID) return luaL_error(L,"isolated Meta modifier unavailable");
    const auto mask=lua_toboolean(L,1)?1u<<index:0;
    keyboard->m_modifiersState.depressed=mask;
    xkb_state_update_mask(keyboard->m_xkbState,mask,0,0,0,0,0);
    g_pInputManager->onKeyboardMod(keyboard);
    return 0;
}
int workspaceAnimation(lua_State* L) {
    const auto target=State::Workspace::resolver()->getWorkspaceTargetFromString(luaL_checkstring(L,1),Desktop::focusState()->monitor());
    const auto ws=State::workspaceState()->find(target);
    if (!ws) return luaL_error(L,"test workspace not found");
    lua_createtable(L,0,5);
    lua_pushnumber(L,ws->m_renderOffset->value().x); lua_setfield(L,-2,"x");
    lua_pushnumber(L,ws->m_renderOffset->goal().x); lua_setfield(L,-2,"goal_x");
    lua_pushnumber(L,ws->m_alpha->value()); lua_setfield(L,-2,"alpha");
    lua_pushboolean(L,ws->m_renderOffset->isBeingAnimated() || ws->m_alpha->isBeingAnimated()); lua_setfield(L,-2,"animated");
    return 1;
}
int refresh(lua_State*) { Event::bus()->m_events.config.reloaded.emit(); return 0; }
PHLWORKSPACE specialOrigin;
PHLWINDOW specialWindow;
int special(lua_State* L) {
    auto mon=Desktop::focusState()->monitor();
    if (lua_toboolean(L,1)) {
        specialWindow=Desktop::focusState()->window();
        if (!specialWindow) return luaL_error(L,"no focused test window");
        specialOrigin=specialWindow->m_workspace;
        const auto target=State::Workspace::resolver()->getWorkspaceTargetFromString("special:tiles-test",mon);
        auto ws=State::workspaceState()->find(target);
        if (!ws) ws=State::workspaceState()->create(target,mon,true);
        Desktop::globalWindowController()->moveWindowToWorkspace(specialWindow,ws);
        mon->setSpecialWorkspace(ws);
    } else {
        mon->setSpecialWorkspace(nullptr);
        if (specialWindow && specialOrigin) Desktop::globalWindowController()->moveWindowToWorkspace(specialWindow,specialOrigin);
        specialWindow.reset(); specialOrigin.reset();
    }
    return 0;
}
int window(lua_State* L) {
    const std::string action=luaL_checkstring(L,1);
    const std::string address=luaL_optstring(L,2,"");
    PHLWINDOW window;
    for (const auto& w:Desktop::windowState()->windows()) if (std::format("0x{:x}",reinterpret_cast<uintptr_t>(w.get()))==address) window=w;
    if (!window) return luaL_error(L,"test window not found");
    if (action=="focus") Desktop::focusState()->fullWindowFocus(window,Desktop::FOCUS_REASON_KEYBIND);
    else if (action=="fullscreen" || action=="maximize" || action=="normal") {
        const auto mode=action=="fullscreen"?Fullscreen::FSMODE_FULLSCREEN:action=="maximize"?Fullscreen::FSMODE_MAXIMIZED:Fullscreen::FSMODE_NONE;
        Fullscreen::controller()->setFullscreenMode(window,mode,mode,false);
    } else if (action=="pin" || action=="unpin") {
        auto result=Config::Actions::floatWindow(Config::Actions::TOGGLE_ACTION_ENABLE,window);
        if (result) result=Config::Actions::pinWindow(action=="pin"?Config::Actions::TOGGLE_ACTION_ENABLE:Config::Actions::TOGGLE_ACTION_DISABLE,window);
        if (!result) return luaL_error(L,"native pin operation failed");
    } else return luaL_error(L,"unknown test window action");
    return 0;
}
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
        if (action=="native") g_pInputManager->refocus(pos);
        else if (action=="move") Event::bus()->m_events.input.mouse.move.emit(pos,info);
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
    fixtureHandle=handle;
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","change_workspace",workspaceChange);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","meta",meta);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","workspace_animation",workspaceAnimation);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","refresh",refresh);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","window",window);
    HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","special",special);
    if (!HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","swipe",swipe) ||
        !HyprlandAPI::addLuaFunction(handle,"hymission_tiles_test","pointer",pointer)) throw std::runtime_error("test fixture Lua registration failed");
    return {"hymission_tiles_test","private native gesture/input test fixture","hymission","1"};
}
APICALL EXPORT void PLUGIN_EXIT() {
    g_pUnifiedWorkspaceSwipe->cancel();
    if (releaseHook) HyprlandAPI::removeFunctionHook(fixtureHandle,releaseHook);
    releaseHook=nullptr;
}
