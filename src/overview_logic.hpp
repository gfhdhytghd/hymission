#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "mission_layout.hpp"

namespace hymission {

enum class Direction {
    Left,
    Right,
    Up,
    Down,
};

enum class OverviewWorkspaceChangeAction {
    Ignore,
    Rebuild,
    Abort,
};

enum class WorkspaceStripAnchor {
    Top,
    Left,
    Right,
};

struct WorkspaceStripReservation {
    Rect band;
    Rect content;
};

[[nodiscard]] std::optional<std::size_t> hitTest(const std::vector<Rect>& rects, double x, double y);
[[nodiscard]] std::optional<std::size_t> chooseDirectionalNeighbor(const std::vector<Rect>& rects, std::size_t currentIndex, Direction direction);
[[nodiscard]] Rect                       lerpRect(const Rect& from, const Rect& to, double t);
[[nodiscard]] double                     easeOutCubic(double t);
[[nodiscard]] double                     easeInCubic(double t);
[[nodiscard]] bool                       shouldSyncOverviewLiveFocus(bool handlesInput, bool overviewFocusFollowsMouse, long inputFollowMouseBeforeOpen);
[[nodiscard]] int                        resolveRecommandGestureCommitDirection(double signedProgress, double lastSignedSpeed, double speedThreshold, bool cancelled);
[[nodiscard]] OverviewWorkspaceChangeAction resolveOverviewWorkspaceChangeAction(bool overviewVisible, bool applyingWorkspaceTransitionCommit,
                                                                                 bool workspaceTransitionActive, bool closing,
                                                                                 bool liveFocusTriggeredWorkspaceChange, bool allowsWorkspaceSwitchInOverview);
[[nodiscard]] WorkspaceStripAnchor parseWorkspaceStripAnchor(std::string_view value);
[[nodiscard]] bool                 isWorkspaceStripHorizontal(WorkspaceStripAnchor anchor);
[[nodiscard]] WorkspaceStripReservation reserveWorkspaceStripBand(const Rect& monitorArea, WorkspaceStripAnchor anchor, double thickness, double gap);
[[nodiscard]] std::vector<Rect>    layoutWorkspaceStripSlots(const Rect& stripBand, WorkspaceStripAnchor anchor, std::size_t slotCount, double gap);
[[nodiscard]] std::optional<std::size_t> hitTestWorkspaceStrip(const std::vector<Rect>& rects, double x, double y);

} // namespace hymission
