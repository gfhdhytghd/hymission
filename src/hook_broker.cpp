#include "hook_broker.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
namespace hymission::hooks {
void* nativeCreate(void* owner, void* source, void* destination) { return HyprlandAPI::createFunctionHook(owner, source, destination); }
bool nativeAttach(void* hook) { return static_cast<CFunctionHook*>(hook)->hook(); }
void nativeDetach(void* hook) { static_cast<CFunctionHook*>(hook)->unhook(); }
void* nativeOriginal(void* hook) { return static_cast<CFunctionHook*>(hook)->m_original; }
void nativeRemove(void* owner, void* hook) { HyprlandAPI::removeFunctionHook(owner, static_cast<CFunctionHook*>(hook)); }
}
