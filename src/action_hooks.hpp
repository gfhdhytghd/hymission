#include "hook_broker.hpp"
#pragma once

#include "overview_controller.hpp"

#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <functional>
#include <sstream>
#include <vector>

namespace hymission {
namespace actionHooks {
using Result = Config::Actions::ActionResult;
using FullscreenFn = Result (*)(Fullscreen::eFullscreenMode, bool, std::optional<PHLWINDOW>);
using FullscreenStateFn = Result (*)(Fullscreen::eFullscreenMode, Fullscreen::eFullscreenMode, bool, std::optional<PHLWINDOW>);
using WorkspaceFn = Result (*)(const std::string&);
inline FullscreenFn fullscreenOriginal = nullptr;
inline FullscreenStateFn fullscreenStateOriginal = nullptr;
inline WorkspaceFn workspaceOriginal = nullptr, workspaceOnMonitorOriginal = nullptr;
inline OverviewController* controller = nullptr;
inline std::vector<hooks::Lease*> hooks;

inline SDispatchResult toDispatch(const Result& result) {
    return result ? SDispatchResult{} : SDispatchResult{.success = false, .error = result.error().message};
}
inline Result toAction(const SDispatchResult& result) {
    return result.success ? Result{} : Config::Actions::actionError(result.error);
}

inline Result fullscreen(Fullscreen::eFullscreenMode mode, bool layoutAware, std::optional<PHLWINDOW> window) {
    if (const auto queued = controller->queueNativeFullscreenAction([=] { return fullscreenOriginal(mode, layoutAware, window); }))
        return *queued;
    return fullscreenOriginal(mode, layoutAware, window);
}
inline Result fullscreenState(Fullscreen::eFullscreenMode internal, Fullscreen::eFullscreenMode client, bool layoutAware, std::optional<PHLWINDOW> window) {
    if (const auto queued = controller->queueNativeFullscreenAction([=] { return fullscreenStateOriginal(internal, client, layoutAware, window); }))
        return *queued;
    return fullscreenStateOriginal(internal, client, layoutAware, window);
}
inline Result workspace(const std::string& value) {
    return toAction(controller->changeWorkspaceDispatcherHook(value));
}
inline Result workspaceOnMonitor(const std::string& value) {
    return toAction(controller->focusWorkspaceOnCurrentMonitorDispatcherHook(value));
}

inline void remove(HANDLE handle) {
    for (auto* hook : hooks)
        hooks::remove(handle, hook);
    hooks.clear();
    controller = nullptr;
    fullscreenOriginal = nullptr;
    fullscreenStateOriginal = nullptr;
    workspaceOriginal = workspaceOnMonitorOriginal = nullptr;
}

inline bool install(HANDLE handle, OverviewController* owner) {
    static_assert(std::is_same_v<FullscreenFn, decltype(&fullscreen)>);
    static_assert(std::is_same_v<FullscreenStateFn, decltype(&fullscreenState)>);
    static_assert(std::is_same_v<WorkspaceFn, decltype(&workspace)>);
    static_assert(std::is_same_v<WorkspaceFn, decltype(&workspaceOnMonitor)>);
    controller = owner;
    const auto attach = [&](const char* name, const char* signature, auto thunk, auto& original) {
        void* address = nullptr;
        for (const auto& match : HyprlandAPI::findFunctionsByName(handle, name)) {
            if (match.demangled != signature)
                continue;
            if (address)
                return false;
            address = match.address;
        }
        if (!address)
            return false;
        auto* hook = hooks::create(handle, address, reinterpret_cast<void*>(thunk));
        if (!hook)
            return false;
        hooks.push_back(hook);
        if (!hook->hook())
            return false;
        original = reinterpret_cast<std::remove_reference_t<decltype(original)>>(hook->m_original);
        return true;
    };
    if (attach("fullscreenWindow", "Config::Actions::fullscreenWindow(Fullscreen::eFullscreenMode, bool, std::optional<Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow> >)", &fullscreen, fullscreenOriginal) &&
        attach("fullscreenWindow", "Config::Actions::fullscreenWindow(Fullscreen::eFullscreenMode, Fullscreen::eFullscreenMode, bool, std::optional<Hyprutils::Memory::CSharedPointer<Desktop::View::CWindow> >)", &fullscreenState, fullscreenStateOriginal) &&
        attach("changeWorkspace", "Config::Actions::changeWorkspace(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)", &workspace, workspaceOriginal) &&
        attach("changeWorkspaceOnCurrentMonitor", "Config::Actions::changeWorkspaceOnCurrentMonitor(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)", &workspaceOnMonitor, workspaceOnMonitorOriginal))
        return true;
    remove(handle);
    return false;
}

// Preserve the plugin's existing string-based Lua adapter and post-close queue.
inline std::function<SDispatchResult(std::string)> dispatcher(const std::string& name) {
    if (name == "workspace")
        return [](std::string args) { return toDispatch(workspaceOriginal(args)); };
    if (name == "focusworkspaceoncurrentmonitor")
        return [](std::string args) { return toDispatch(workspaceOnMonitorOriginal(args)); };
    if (name == "fullscreenstate")
        return [](std::string args) {
            int internal = -1, client = -1;
            std::istringstream values{args};
            values >> internal >> client;
            return toDispatch(fullscreenStateOriginal(static_cast<Fullscreen::eFullscreenMode>(internal), static_cast<Fullscreen::eFullscreenMode>(client), true, std::nullopt));
        };
    if (name != "fullscreen")
        return {};
    return [](std::string args) -> SDispatchResult {
        int mode = 0;
        std::string action = "toggle";
        std::istringstream values{args};
        values >> mode >> action;
        const auto window = Desktop::focusState()->window();
        if (!window)
            return {};
        const auto desired = mode == 1 ? Fullscreen::FSMODE_MAXIMIZED : Fullscreen::FSMODE_FULLSCREEN;
        const bool current = Fullscreen::controller()->isFullscreen(window, desired);
        if ((action == "set" && current) || (action == "unset" && !current))
            return {};
        return toDispatch(fullscreenOriginal(desired, true, std::nullopt));
    };
}
} // namespace actionHooks
} // namespace hymission
