#pragma once
#include "hook_symbols.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace hymission::hooks {
inline auto findFunctions(HANDLE handle, const std::string& name) {
    return tryLookup([&] { return HyprlandAPI::findFunctionsByName(handle, name); });
}
}
