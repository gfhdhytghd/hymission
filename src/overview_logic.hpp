#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mission_layout.hpp"

namespace hymission {

// Shared by gesture geometry prediction and the committed close. Window may be
// a compositor handle or a lightweight handle in the logic tests.
template <typename Window>
[[nodiscard]] Window resolveGestureExitFocus(const Window& original, const Window& preferred, bool restoresFocus, bool originalMapped) {
    if (restoresFocus && original && originalMapped)
        return original;
    return preferred ? preferred : original;
}

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

enum class HymissionScrollMode {
    Layout,
};

enum class GestureAxis {
    Horizontal,
    Vertical,
};

enum class ScrollingLayoutDirection {
    Right,
    Left,
    Down,
    Up,
};

enum class RecommandVisibleGestureMode {
    CloseOnly,
    TransferCapable,
};

enum class HoverRelayoutCurve {
    Linear,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
};

enum class ToggleDirection {
    Forward,
    Reverse,
};

enum class PickLabelsMode {
    Sequential,
    Spatial,
};

enum class GroupedWindowsPolicy {
    Expanded,
    Collapsed,
};

enum class SpatialPickDirection {
    Center,
    Left,
    Right,
    Up,
    Down,
};

struct ToggleArguments {
    std::string scope;
    ToggleDirection direction = ToggleDirection::Forward;
};

struct SpatialPickPoint {
    double x = 0.0;
    double y = 0.0;
};

struct SpatialPickKey {
    char   label = '\0';
    double x = 0.0;
    double y = 0.0;
};

struct SpatialPickRoute {
    std::size_t          windowIndex = 0;
    std::size_t          primaryKeyIndex = 0;
    SpatialPickDirection direction = SpatialPickDirection::Center;
    std::size_t          canonicalSecondaryKeyIndex = 0;
};

struct SpatialPickMap {
    std::vector<SpatialPickRoute>          routes;
    std::vector<std::optional<std::size_t>> nearestWindowByKey;
};

struct WorkspaceStripReservation {
    Rect band;
    Rect content;
};

struct GroupProjectionInput {
    std::uintptr_t groupId = 0;
    bool           current = false;
};

struct WindowExpansionTarget {
    std::size_t index = 0;
    double      scale = 1.0;
};

[[nodiscard]] std::optional<std::size_t> hitTest(const std::vector<Rect>& rects, double x, double y);
[[nodiscard]] std::optional<std::size_t> chooseDirectionalNeighbor(const std::vector<Rect>& rects, std::size_t currentIndex, Direction direction);
[[nodiscard]] std::optional<std::size_t> chooseCyclicIndex(std::size_t count, std::size_t currentIndex, int step = 1);
[[nodiscard]] std::vector<std::size_t>   computePickOrder(const std::vector<Rect>& rects, const std::vector<std::size_t>& monitorRanks);
[[nodiscard]] std::string                computePickLabel(std::size_t orderIndex);
[[nodiscard]] std::size_t                computePickOrderIndex(int digit1to9, std::optional<int> letterGroupAtoZ);
[[nodiscard]] bool                       pickLetterGroupAvailable(std::size_t windowCount, int letterGroupAtoZ);
[[nodiscard]] PickLabelsMode             parsePickLabelsMode(std::string_view value);
[[nodiscard]] GroupedWindowsPolicy       parseGroupedWindowsPolicy(std::string_view value);
[[nodiscard]] std::vector<std::size_t>   projectGroupedWindowIndices(const std::vector<GroupProjectionInput>& inputs, GroupedWindowsPolicy policy);
[[nodiscard]] std::optional<std::size_t> hitTestEqualSegments(const Rect& bounds, std::size_t count, double x, double y);
[[nodiscard]] bool                       shouldSuppressCollapsedGroupMember(std::uintptr_t itemGroupId, std::uintptr_t windowGroupId,
                                                                          std::uintptr_t boundWindowId, std::uintptr_t windowId);
[[nodiscard]] float                      resolveExpandedGroupEffectiveAlpha(float originalAlpha, float previewAlpha, bool overviewVisible,
                                                                           bool rawRenderActive, bool groupedOverviewItem,
                                                                           bool collapsedOverviewItem);
[[nodiscard]] bool                       shouldRefreshDraggedCompositeTexture(bool compositeCapture, bool textureAvailable);
[[nodiscard]] Rect                       floatingSegmentBarRect(const Rect& preview, std::size_t count, double height = 24.0,
                                                              double gap = 7.0, double minSegmentWidth = 56.0,
                                                              double maxSegmentWidth = 128.0, double horizontalInset = 8.0);
[[nodiscard]] std::vector<Rect>           stackedGroupPreviewRects(const std::vector<Rect>& sourceRects, std::size_t frontIndex, double pointerX,
                                                                   double pointerY, double grabRatioX, double grabRatioY, double scale,
                                                                   double layerOffset = 10.0, double maxSpread = 48.0);
[[nodiscard]] const std::vector<SpatialPickKey>& spatialPickKeys();
[[nodiscard]] std::optional<std::size_t> spatialPickKeyIndex(char label);
[[nodiscard]] std::optional<SpatialPickDirection> spatialPickDirectionForKeys(std::size_t primaryKeyIndex, std::size_t secondaryKeyIndex);
[[nodiscard]] SpatialPickMap             computeSpatialPickMap(const std::vector<SpatialPickPoint>& windowCenters);
[[nodiscard]] std::size_t                spatialPickRouteCount(const SpatialPickMap& map, std::size_t primaryKeyIndex);
[[nodiscard]] std::optional<std::size_t> resolveSpatialPickPrimary(const SpatialPickMap& map, std::size_t primaryKeyIndex);
[[nodiscard]] std::optional<std::size_t> resolveSpatialPickChord(const SpatialPickMap& map, std::size_t primaryKeyIndex, std::size_t secondaryKeyIndex);
[[nodiscard]] std::optional<ToggleArguments> parseToggleArguments(std::string_view value);
[[nodiscard]] std::optional<std::string>     legacyFullscreenDispatcherArguments(std::string_view mode, std::string_view action);
[[nodiscard]] Rect                       lerpRect(const Rect& from, const Rect& to, double t);
[[nodiscard]] Rect                       gestureIncomingWorkspaceEndpoint(const Rect& live, double renderOffsetX, double renderOffsetY);
[[nodiscard]] double                     easeOutCubic(double t);
[[nodiscard]] double                     easeInCubic(double t);
[[nodiscard]] double                     easeInOutCubic(double t);
[[nodiscard]] HoverRelayoutCurve         parseHoverRelayoutCurve(std::string_view value);
[[nodiscard]] double                     applyHoverRelayoutCurve(HoverRelayoutCurve curve, double t);
[[nodiscard]] bool                       shouldSyncOverviewLiveFocus(bool handlesInput, bool overviewFocusFollowsMouse, long inputFollowMouseBeforeOpen);
[[nodiscard]] std::vector<WindowExpansionTarget> resolveWindowExpansionTargets(std::optional<std::size_t> selectedIndex,
                                                                                std::optional<std::size_t> hoveredIndex,
                                                                                bool overviewFocusFollowsMouse,
                                                                                double selectedExpandScale,
                                                                                double hoverExpandScale);
[[nodiscard]] bool                       shouldApplyOverviewWindowTransform(bool managedByOverview, bool closePending);
[[nodiscard]] RecommandVisibleGestureMode resolveRecommandVisibleGestureMode(int currentScopeSign, int gestureDirectionSign);
[[nodiscard]] bool                       resolveOverviewGestureCommit(bool opening, double openness, double lastAlignedSpeed, double speedThreshold, bool cancelled);
[[nodiscard]] int                        resolveRecommandGestureCommitDirection(double signedProgress, bool opening, double lastAlignedSpeed, double speedThreshold,
                                                                               bool cancelled);
[[nodiscard]] OverviewWorkspaceChangeAction resolveOverviewWorkspaceChangeAction(bool overviewVisible, bool applyingWorkspaceTransitionCommit,
                                                                                 bool workspaceTransitionActive, bool closing,
                                                                                 bool liveFocusTriggeredWorkspaceChange, bool allowsWorkspaceSwitchInOverview);
[[nodiscard]] WorkspaceStripAnchor parseWorkspaceStripAnchor(std::string_view value);
[[nodiscard]] WorkspaceStripEmptyMode parseWorkspaceStripEmptyMode(std::string_view value);
[[nodiscard]] std::optional<HymissionScrollMode> parseHymissionScrollMode(std::string_view value);
[[nodiscard]] ScrollingLayoutDirection parseScrollingLayoutDirection(std::string_view value);
[[nodiscard]] GestureAxis              axisForScrollingLayoutDirection(ScrollingLayoutDirection direction);
[[nodiscard]] bool                     scrollingLayoutGestureAxisMatches(ScrollingLayoutDirection direction, GestureAxis axis);
[[nodiscard]] double                   scrollingLayoutMoveAmount(ScrollingLayoutDirection direction, double primaryDelta, double sensitivity);
[[nodiscard]] double                   niriScrollingPreviewCellLength(double layoutPrimaryLength, double fallbackPrimaryLength);
[[nodiscard]] double                   niriScrollingPreviewAdvance(double layoutPrimaryLength, double fallbackPrimaryLength, double gap);
[[nodiscard]] double                   niriOverviewPreviewScale(const Rect& previewArea, const Rect& baseArea, double maxPreviewScale, double minSlotScale,
                                                                std::optional<GestureAxis> overflowAxis = std::nullopt);
[[nodiscard]] bool                 isWorkspaceStripHorizontal(WorkspaceStripAnchor anchor);
[[nodiscard]] std::vector<int64_t> expandWorkspaceStripWorkspaceIds(const std::vector<int64_t>& workspaceIds, WorkspaceStripEmptyMode mode);
[[nodiscard]] WorkspaceStripReservation reserveWorkspaceStripBand(const Rect& monitorArea, WorkspaceStripAnchor anchor, double thickness, double gap);
[[nodiscard]] std::vector<Rect>    layoutWorkspaceStripSlots(const Rect& stripBand, WorkspaceStripAnchor anchor, std::size_t slotCount, double gap);
[[nodiscard]] std::vector<Rect>    layoutNiriWorkspaceStripSlots(const Rect& stripBand, WorkspaceStripAnchor anchor, std::size_t slotCount,
                                                                  std::optional<std::size_t> activeIndex, double gap, double padding,
                                                                  double workspaceAspectRatio, double workspaceScale = 1.0);
[[nodiscard]] std::optional<std::size_t> hitTestWorkspaceStrip(const std::vector<Rect>& rects, double x, double y);
[[nodiscard]] std::string normalizedSearchText(std::string_view value);
[[nodiscard]] bool        windowMatchesSearch(std::string_view title, std::string_view windowClass, std::string_view normalizedQuery);
[[nodiscard]] bool        overviewTextInputAllowed(uint32_t modifiers, uint32_t commandModifierMask);

} // namespace hymission
