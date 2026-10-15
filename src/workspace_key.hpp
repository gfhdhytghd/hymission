#pragma once

#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/desktop/history/WorkspaceHistoryTracker.hpp>
#include <hyprland/src/workspace/RegularWorkspace.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace hymission {

// Keep numeric slots and the existing negative-key ordering for unnumbered
// workspaces. These keys are plugin-local; Hyprland identity is (id, address,
// type), and a display-name change must never change the key or lookup target.
using WorkspaceKey = int64_t;
constexpr WorkspaceKey WORKSPACE_INVALID = -1;
constexpr WorkspaceKey WORKSPACE_NOT_CHANGED = -101;

struct WorkspaceKeyName {
    WorkspaceKey id = WORKSPACE_INVALID;
    std::string name;
    bool autoID = false;
};

inline std::vector<State::Workspace::STarget> unnumberedWorkspaceTargets;

inline WorkspaceKey workspaceKey(const State::Workspace::STarget& target) {
    if (!target.valid())
        return WORKSPACE_INVALID;
    if (const auto numbered = std::get_if<Workspace::SWorkspaceNumberedID>(&*target.id))
        return numbered->value;
    for (size_t i = 0; i < unnumberedWorkspaceTargets.size(); ++i) {
        const auto& existing = unnumberedWorkspaceTargets[i];
        if (existing.id == target.id && existing.address == target.address && existing.type == target.type) {
            unnumberedWorkspaceTargets[i].displayName = target.displayName;
            return -1000 - static_cast<WorkspaceKey>(i);
        }
    }
    unnumberedWorkspaceTargets.push_back(target);
    return -1000 - static_cast<WorkspaceKey>(unnumberedWorkspaceTargets.size() - 1);
}

inline void rememberWorkspaceKeys() {
    // The native relative resolver orders named workspaces in reverse state
    // order, ahead of numbered ones. Seed that order before a sort comparator
    // can observe a named key for the first time.
    for (const auto& workspace : State::workspaceState()->workspacesCopy()) {
        if (!workspace->numberedID())
            (void)workspaceKey(State::Workspace::STarget{workspace->id(), workspace->addressableName(), workspace->displayName(), workspace->type()});
    }
}

inline WorkspaceKey workspaceKey(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return WORKSPACE_INVALID;
    if (!workspace->numberedID() && std::ranges::none_of(unnumberedWorkspaceTargets, [&](const auto& target) {
            return target.id == workspace->id() && target.address == workspace->addressableName() && target.type == workspace->type();
        }))
        rememberWorkspaceKeys();
    return workspaceKey(State::Workspace::STarget{workspace->id(), workspace->addressableName(), workspace->displayName(), workspace->type()});
}

inline WorkspaceKey workspaceKey(const PHLWORKSPACEREF& workspace) {
    return workspaceKey(workspace.lock());
}

inline State::Workspace::STarget workspaceTarget(WorkspaceKey key) {
    if (key > 0 && key <= std::numeric_limits<Workspace::WorkspaceIDContainer>::max())
        return {Workspace::SWorkspaceNumberedID{static_cast<Workspace::WorkspaceIDContainer>(key)}, std::to_string(key), {}, Workspace::eWorkspaceType::NORMAL};
    if (key <= -1000) {
        const auto index = static_cast<size_t>(-(key + 1000));
        if (index < unnumberedWorkspaceTargets.size())
            return unnumberedWorkspaceTargets[index];
    }
    return {};
}

inline PHLWORKSPACE workspaceForKey(WorkspaceKey key) {
    const auto target = workspaceTarget(key);
    return target.valid() ? State::workspaceState()->find(target) : nullptr;
}

inline PHLWORKSPACE createWorkspaceForKey(WorkspaceKey key, MONITORID monitorID, const std::string& displayName = {}, bool isEmpty = true) {
    auto target = workspaceTarget(key);
    if (!target.valid())
        return nullptr;
    if (!displayName.empty())
        target.displayName = displayName;
    return State::workspaceState()->create(target, State::monitorState()->query().id(monitorID).run(), isEmpty);
}

inline WorkspaceKeyName resolveWorkspaceKey(const std::string& selector) {
    rememberWorkspaceKeys();
    const auto target = State::Workspace::resolver()->getWorkspaceTargetFromString(selector);
    return {workspaceKey(target), target.displayName.empty() ? target.address : target.displayName, false};
}

inline WorkspaceKeyName previousWorkspaceKey(const PHLWORKSPACE& workspace, PHLMONITOR monitor = nullptr) {
    const auto previous = monitor ? Desktop::History::workspaceTracker()->previousWorkspace(workspace, monitor) :
                                    Desktop::History::workspaceTracker()->previousWorkspace(workspace);
    return {workspaceKey(previous.target), previous.target.displayName, false};
}

} // namespace hymission
