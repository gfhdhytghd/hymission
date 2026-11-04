#pragma once
#include "hook_symbols.hpp"
#include <array>
#include <string>
#include <unistd.h>
#include <vector>
#include <hyprland/src/plugins/PluginAPI.hpp>

namespace hymission::hooks {
// Hyprland resolves symbols by reading its own binary from disk. After a package
// update that file is gone, and findFunctionsByName terminates the compositor from
// std::filesystem::canonical("/proc/self/exe"). Report no match instead: hooks that
// are already attached keep working, and new ones fail through their usual path.
inline std::vector<SFunctionMatch> findFunctions(HANDLE handle, const std::string& name) {
    std::array<char, 4096> link{};
    const auto size = readlink("/proc/self/exe", link.data(), link.size());
    if (size <= 0 || isReplacedBinaryLink({link.data(), static_cast<size_t>(size)})) return {};
    return HyprlandAPI::findFunctionsByName(handle, name);
}
}
