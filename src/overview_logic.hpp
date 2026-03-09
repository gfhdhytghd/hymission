#pragma once

#include <cstdint>
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

enum class WorkspaceStripEmptyMode {
    Existing,
    Continuous,
};

enum class RecommandVisibleGestureMode {
    CloseOnly,
    TransferCapable,
};

struct WorkspaceStripReservation {
    Rect band;
    Rect content;
};

[[nodiscard]] std::optional<std::size_t> hitTest(const std::vector<Rect>& rects, double x, double y);
[[nodiscard]] std::optional<std::size_t> chooseDirectionalNeighbor(const std::vector<Rect>& rects, std::size_t currentIndex, Direction direction);
[[nodiscard]] std::optional<std::size_t> chooseCyclicIndex(std::size_t count, std::size_t currentIndex, int step = 1);
[[nodiscard]] Rect                       lerpRect(const Rect& from, const Rect& to, double t);
[[nodiscard]] double                     easeOutCubic(double t);
[[nodiscard]] double                     easeInCubic(double t);
[[nodiscard]] bool                       shouldSyncOverviewLiveFocus(bool handlesInput, bool overviewFocusFollowsMouse, long inputFollowMouseBeforeOpen);
[[nodiscard]] RecommandVisibleGestureMode resolveRecommandVisibleGestureMode(int currentScopeSign, int gestureDirectionSign);
[[nodiscard]] bool                       resolveOverviewGestureCommit(bool opening, double openness, double lastAlignedSpeed, double speedThreshold, bool cancelled);
[[nodiscard]] int                        resolveRecommandGestureCommitDirection(double signedProgress, bool opening, double lastAlignedSpeed, double speedThreshold,
                                                                               bool cancelled);
[[nodiscard]] OverviewWorkspaceChangeAction resolveOverviewWorkspaceChangeAction(bool overviewVisible, bool applyingWorkspaceTransitionCommit,
                                                                                 bool workspaceTransitionActive, bool closing,
                                                                                 bool liveFocusTriggeredWorkspaceChange, bool allowsWorkspaceSwitchInOverview);
[[nodiscard]] WorkspaceStripAnchor parseWorkspaceStripAnchor(std::string_view value);
[[nodiscard]] WorkspaceStripEmptyMode parseWorkspaceStripEmptyMode(std::string_view value);
[[nodiscard]] bool                 isWorkspaceStripHorizontal(WorkspaceStripAnchor anchor);
[[nodiscard]] std::vector<int64_t> expandWorkspaceStripWorkspaceIds(const std::vector<int64_t>& workspaceIds, WorkspaceStripEmptyMode mode);
[[nodiscard]] WorkspaceStripReservation reserveWorkspaceStripBand(const Rect& monitorArea, WorkspaceStripAnchor anchor, double thickness, double gap);
[[nodiscard]] std::vector<Rect>    layoutWorkspaceStripSlots(const Rect& stripBand, WorkspaceStripAnchor anchor, std::size_t slotCount, double gap);
[[nodiscard]] std::optional<std::size_t> hitTestWorkspaceStrip(const std::vector<Rect>& rects, double x, double y);

} // namespace hymission
