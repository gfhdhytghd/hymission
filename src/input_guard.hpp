#pragma once

namespace hymission {

// Shared by every input entry point; null matches Hyprland's existing policy.
template <typename Manager>
bool sessionLocked(Manager* manager) {
    return manager && manager->isSessionLocked();
}

bool sessionLocked();

} // namespace hymission
