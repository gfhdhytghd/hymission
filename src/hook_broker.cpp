#include "hook_broker.hpp"
#if defined(__aarch64__)
#include "hook_aarch64.hpp"
namespace hymission::hooks {
// Hyprland's CFunctionHook has no AArch64 implementation, so the plugin patches entries itself.
void* nativeCreate(void*, void* source, void* destination) { return aarch64::create(source, destination); }
bool nativeAttach(void* hook) { return aarch64::attach(static_cast<aarch64::Hook*>(hook)); }
bool nativeDetach(void* hook) { return aarch64::detach(static_cast<aarch64::Hook*>(hook)); }
void* nativeOriginal(void* hook) { return aarch64::original(static_cast<aarch64::Hook*>(hook)); }
void nativeRemove(void*, void* hook) { aarch64::destroy(static_cast<aarch64::Hook*>(hook)); }
}
#else
#include "hook_entry.hpp"
#include <unordered_set>
#include <hyprland/src/helpers/memory/Memory.hpp>
// Confine access to the native probe to this adapter, like the private layout
// adapters in the controllers. All broker users still use the public leases.
#define private public
#include <hyprland/src/plugins/HookSystem.hpp>
#undef private
#include <hyprland/src/plugins/PluginAPI.hpp>
namespace hymission::hooks {
void* nativeCreate(void* owner, void* source, void* destination) { return HyprlandAPI::createFunctionHook(owner, source, destination); }
bool nativeAttach(void* hook) {
    auto* native=static_cast<CFunctionHook*>(hook);
    // Probe before patching. Reuse the exact instruction boundaries Hyprland
    // uses, and retain its relocated RIP-memory operands and final return jump.
    std::vector<size_t> lengths;
    try { lengths=native->probeMinimumJumpSize(native->m_source,5).insSizes; }
    catch (const std::exception&) { return false; }
    if (!native->hook()) return false;
    const auto landing=static_cast<const unsigned char*>(native->m_original);
    const auto fixed=relocateEntryBranches(native->m_originalBytes,{landing,native->m_hookLen+5},lengths,
        reinterpret_cast<uintptr_t>(native->m_source),reinterpret_cast<uintptr_t>(landing),HOOK_TRAMPOLINE_MAX_SIZE);
    if (!fixed) { native->unhook(); return false; }
    // Native HookSystem owns RWX trampoline slots. Complete relocation before
    // returning to the event loop; detach continues to use its original bytes.
    std::memcpy(native->m_original,fixed->data(),fixed->size());
    return true;
}
bool nativeDetach(void* hook) { static_cast<CFunctionHook*>(hook)->unhook(); return true; }
void* nativeOriginal(void* hook) { return static_cast<CFunctionHook*>(hook)->m_original; }
void nativeRemove(void* owner, void* hook) { HyprlandAPI::removeFunctionHook(owner, static_cast<CFunctionHook*>(hook)); }
}
#endif
