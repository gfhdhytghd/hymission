#include "overview_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

namespace hymission {

namespace {

double clampUnit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

Rect clampRectSize(const Rect& rect) {
    return {
        rect.x,
        rect.y,
        std::max(0.0, rect.width),
        std::max(0.0, rect.height),
    };
}

bool contains(const Rect& rect, double x, double y) {
    return x >= rect.x && y >= rect.y && x <= rect.x + rect.width && y <= rect.y + rect.height;
}

double centerDistanceSquared(const Rect& rect, double x, double y) {
    const double dx = rect.centerX() - x;
    const double dy = rect.centerY() - y;
    return dx * dx + dy * dy;
}

std::string_view trimAsciiWhitespace(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);

    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);

    return value;
}

bool equalsAsciiInsensitive(std::string_view value, std::string_view expected) {
    if (value.size() != expected.size())
        return false;

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(expected[i])))
            return false;
    }

    return true;
}

} // namespace

std::optional<std::size_t> hitTest(const std::vector<Rect>& rects, double x, double y) {
    std::optional<std::size_t> bestIndex;
    double                     bestDistance = std::numeric_limits<double>::infinity();

    for (std::size_t index = 0; index < rects.size(); ++index) {
        if (!contains(rects[index], x, y))
            continue;

        const double distance = centerDistanceSquared(rects[index], x, y);
        if (!bestIndex || distance < bestDistance) {
            bestIndex = index;
            bestDistance = distance;
        }
    }

    return bestIndex;
}

std::optional<std::size_t> chooseDirectionalNeighbor(const std::vector<Rect>& rects, std::size_t currentIndex, Direction direction) {
    if (currentIndex >= rects.size())
        return std::nullopt;

    const Rect& current = rects[currentIndex];
    std::optional<std::size_t> bestIndex;
    double                     bestScore = std::numeric_limits<double>::infinity();
    double                     bestDistance = std::numeric_limits<double>::infinity();

    for (std::size_t index = 0; index < rects.size(); ++index) {
        if (index == currentIndex)
            continue;

        const Rect& candidate = rects[index];
        const double dx = candidate.centerX() - current.centerX();
        const double dy = candidate.centerY() - current.centerY();

        double primary = 0.0;
        double secondary = 0.0;

        switch (direction) {
            case Direction::Left:
                primary = -dx;
                secondary = std::abs(dy);
                break;
            case Direction::Right:
                primary = dx;
                secondary = std::abs(dy);
                break;
            case Direction::Up:
                primary = -dy;
                secondary = std::abs(dx);
                break;
            case Direction::Down:
                primary = dy;
                secondary = std::abs(dx);
                break;
        }

        if (primary <= 0.0)
            continue;

        const double score = primary * primary + std::pow(secondary * 1.5, 2.0);
        const double distance = dx * dx + dy * dy;

        if (!bestIndex || score < bestScore || (score == bestScore && distance < bestDistance) || (score == bestScore && distance == bestDistance && index < *bestIndex)) {
            bestIndex = index;
            bestScore = score;
            bestDistance = distance;
        }
    }

    return bestIndex;
}

std::optional<std::size_t> chooseCyclicIndex(std::size_t count, std::size_t currentIndex, int step) {
    if (count < 2 || currentIndex >= count || step == 0)
        return std::nullopt;

    const auto countSigned = static_cast<long long>(count);
    long long  normalized = static_cast<long long>(step) % countSigned;
    if (normalized < 0)
        normalized += countSigned;

    if (normalized == 0)
        return std::nullopt;

    return static_cast<std::size_t>((static_cast<long long>(currentIndex) + normalized) % countSigned);
}

Rect lerpRect(const Rect& from, const Rect& to, double t) {
    const double clamped = clampUnit(t);
    return {
        from.x + (to.x - from.x) * clamped,
        from.y + (to.y - from.y) * clamped,
        from.width + (to.width - from.width) * clamped,
        from.height + (to.height - from.height) * clamped,
    };
}

double easeOutCubic(double t) {
    const double clamped = clampUnit(t);
    const double inverse = 1.0 - clamped;
    return 1.0 - inverse * inverse * inverse;
}

double easeInCubic(double t) {
    const double clamped = clampUnit(t);
    return clamped * clamped * clamped;
}

bool shouldSyncOverviewLiveFocus(bool handlesInput, bool overviewFocusFollowsMouse, long inputFollowMouseBeforeOpen) {
    return handlesInput && overviewFocusFollowsMouse && inputFollowMouseBeforeOpen != 0;
}

RecommandVisibleGestureMode resolveRecommandVisibleGestureMode(int currentScopeSign, int gestureDirectionSign) {
    if (currentScopeSign != 0 && gestureDirectionSign == -currentScopeSign)
        return RecommandVisibleGestureMode::TransferCapable;

    return RecommandVisibleGestureMode::CloseOnly;
}

bool resolveOverviewGestureCommit(bool opening, double openness, double lastAlignedSpeed, double speedThreshold, bool cancelled) {
    if (cancelled)
        return false;

    const bool speedForward = speedThreshold > 0.0 && lastAlignedSpeed >= speedThreshold;
    const bool speedReverse = speedThreshold > 0.0 && lastAlignedSpeed <= -speedThreshold;
    if (speedReverse)
        return false;

    return speedForward || (opening ? openness >= 0.5 : openness <= 0.5);
}

int resolveRecommandGestureCommitDirection(double signedProgress, bool opening, double lastAlignedSpeed, double speedThreshold, bool cancelled) {
    if (cancelled)
        return 0;

    const int sign = signedProgress > 0.0001 ? 1 : signedProgress < -0.0001 ? -1 : 0;
    if (sign == 0)
        return 0;

    const bool speedTowardCurrentSide = speedThreshold > 0.0 && (opening ? lastAlignedSpeed >= speedThreshold : lastAlignedSpeed <= -speedThreshold);
    const bool speedTowardHidden = speedThreshold > 0.0 && (opening ? lastAlignedSpeed <= -speedThreshold : lastAlignedSpeed >= speedThreshold);
    if (speedTowardHidden)
        return 0;

    return (speedTowardCurrentSide || std::abs(signedProgress) >= 0.5) ? sign : 0;
}

OverviewWorkspaceChangeAction resolveOverviewWorkspaceChangeAction(bool overviewVisible, bool applyingWorkspaceTransitionCommit, bool workspaceTransitionActive,
                                                                   bool closing, bool liveFocusTriggeredWorkspaceChange,
                                                                   bool allowsWorkspaceSwitchInOverview) {
    if (!overviewVisible || applyingWorkspaceTransitionCommit || closing)
        return OverviewWorkspaceChangeAction::Ignore;

    if (workspaceTransitionActive || liveFocusTriggeredWorkspaceChange || allowsWorkspaceSwitchInOverview)
        return OverviewWorkspaceChangeAction::Rebuild;

    return OverviewWorkspaceChangeAction::Abort;
}

WorkspaceStripAnchor parseWorkspaceStripAnchor(std::string_view value) {
    value = trimAsciiWhitespace(value);

    if (equalsAsciiInsensitive(value, "left"))
        return WorkspaceStripAnchor::Left;
    if (equalsAsciiInsensitive(value, "right"))
        return WorkspaceStripAnchor::Right;

    return WorkspaceStripAnchor::Top;
}

WorkspaceStripEmptyMode parseWorkspaceStripEmptyMode(std::string_view value) {
    value = trimAsciiWhitespace(value);

    if (equalsAsciiInsensitive(value, "continuous"))
        return WorkspaceStripEmptyMode::Continuous;

    return WorkspaceStripEmptyMode::Existing;
}

std::optional<HymissionScrollMode> parseHymissionScrollMode(std::string_view value) {
    value = trimAsciiWhitespace(value);

    if (equalsAsciiInsensitive(value, "layout"))
        return HymissionScrollMode::Layout;

    return std::nullopt;
}

ScrollingLayoutDirection parseScrollingLayoutDirection(std::string_view value) {
    value = trimAsciiWhitespace(value);

    if (equalsAsciiInsensitive(value, "left"))
        return ScrollingLayoutDirection::Left;
    if (equalsAsciiInsensitive(value, "down"))
        return ScrollingLayoutDirection::Down;
    if (equalsAsciiInsensitive(value, "up"))
        return ScrollingLayoutDirection::Up;

    return ScrollingLayoutDirection::Right;
}

GestureAxis axisForScrollingLayoutDirection(ScrollingLayoutDirection direction) {
    switch (direction) {
        case ScrollingLayoutDirection::Down:
        case ScrollingLayoutDirection::Up:
            return GestureAxis::Vertical;
        case ScrollingLayoutDirection::Right:
        case ScrollingLayoutDirection::Left:
        default:
            return GestureAxis::Horizontal;
    }
}

bool scrollingLayoutGestureAxisMatches(ScrollingLayoutDirection direction, GestureAxis axis) {
    return axisForScrollingLayoutDirection(direction) == axis;
}

double scrollingLayoutMoveAmount(ScrollingLayoutDirection direction, double primaryDelta, double sensitivity) {
    const double sign = (direction == ScrollingLayoutDirection::Left || direction == ScrollingLayoutDirection::Up) ? -1.0 : 1.0;
    return primaryDelta * sign * std::max(0.0, sensitivity);
}

bool isWorkspaceStripHorizontal(WorkspaceStripAnchor anchor) {
    return anchor == WorkspaceStripAnchor::Top;
}

std::vector<int64_t> expandWorkspaceStripWorkspaceIds(const std::vector<int64_t>& workspaceIds, WorkspaceStripEmptyMode mode) {
    std::vector<int64_t> sortedIds = workspaceIds;
    std::sort(sortedIds.begin(), sortedIds.end());
    sortedIds.erase(std::unique(sortedIds.begin(), sortedIds.end()), sortedIds.end());

    if (mode == WorkspaceStripEmptyMode::Existing || sortedIds.empty())
        return sortedIds;

    std::vector<int64_t> expanded;
    expanded.reserve(sortedIds.size() * 2);
    for (std::size_t index = 0; index < sortedIds.size(); ++index) {
        const int64_t workspaceId = sortedIds[index];
        expanded.push_back(workspaceId);

        if (index + 1 >= sortedIds.size())
            continue;

        const int64_t nextWorkspaceId = sortedIds[index + 1];
        if (workspaceId < 1 || nextWorkspaceId <= workspaceId + 1)
            continue;

        expanded.push_back(workspaceId + 1);
    }

    return expanded;
}

WorkspaceStripReservation reserveWorkspaceStripBand(const Rect& monitorArea, WorkspaceStripAnchor anchor, double thickness, double gap) {
    const Rect monitor = clampRectSize(monitorArea);
    const bool horizontal = isWorkspaceStripHorizontal(anchor);
    const double mainLength = horizontal ? monitor.height : monitor.width;
    const double bandThickness = std::clamp(thickness, 0.0, mainLength);
    const double bandGap = bandThickness > 0.0 ? std::clamp(gap, 0.0, mainLength - bandThickness) : 0.0;
    const double contentLength = std::max(0.0, mainLength - bandThickness - bandGap);

    WorkspaceStripReservation reservation = {
        .band = monitor,
        .content = monitor,
    };

    switch (anchor) {
        case WorkspaceStripAnchor::Left:
            reservation.band.width = bandThickness;
            reservation.content.x = monitor.x + bandThickness + bandGap;
            reservation.content.width = contentLength;
            break;
        case WorkspaceStripAnchor::Right:
            reservation.band.x = monitor.x + monitor.width - bandThickness;
            reservation.band.width = bandThickness;
            reservation.content.width = contentLength;
            break;
        case WorkspaceStripAnchor::Top:
            reservation.band.height = bandThickness;
            reservation.content.y = monitor.y + bandThickness + bandGap;
            reservation.content.height = contentLength;
            break;
    }

    return reservation;
}

std::vector<Rect> layoutWorkspaceStripSlots(const Rect& stripBand, WorkspaceStripAnchor anchor, std::size_t slotCount, double gap) {
    std::vector<Rect> slots;
    slots.reserve(slotCount);

    const Rect band = clampRectSize(stripBand);
    if (slotCount == 0 || band.width <= 0.0 || band.height <= 0.0)
        return slots;

    const bool horizontal = isWorkspaceStripHorizontal(anchor);
    const double mainLength = horizontal ? band.width : band.height;
    const double maxGap = slotCount > 1 ? mainLength / static_cast<double>(slotCount - 1) : 0.0;
    const double slotGap = slotCount > 1 ? std::clamp(gap, 0.0, maxGap) : 0.0;
    const double totalGap = slotGap * static_cast<double>(slotCount - 1);
    const double slotLength = std::max(0.0, (mainLength - totalGap) / static_cast<double>(slotCount));
    double cursor = horizontal ? band.x : band.y;

    for (std::size_t index = 0; index < slotCount; ++index) {
        if (horizontal) {
            slots.push_back({
                cursor,
                band.y,
                slotLength,
                band.height,
            });
        } else {
            slots.push_back({
                band.x,
                cursor,
                band.width,
                slotLength,
            });
        }

        cursor += slotLength + slotGap;
    }

    return slots;
}

std::vector<Rect> layoutNiriWorkspaceStripSlots(const Rect& stripBand, WorkspaceStripAnchor anchor, std::size_t slotCount, std::optional<std::size_t> activeIndex,
                                                double gap, double padding, double workspaceAspectRatio, double workspaceScale) {
    std::vector<Rect> slots;
    slots.reserve(slotCount);

    const Rect band = clampRectSize(stripBand);
    if (slotCount == 0 || band.width <= 0.0 || band.height <= 0.0)
        return slots;

    const bool   horizontal = isWorkspaceStripHorizontal(anchor);
    const double mainStart = horizontal ? band.x : band.y;
    const double mainLength = horizontal ? band.width : band.height;
    const double crossStart = horizontal ? band.y : band.x;
    const double crossLength = horizontal ? band.height : band.width;
    const double safePadding = std::clamp(padding, 0.0, std::min(mainLength, crossLength) * 0.45);
    const double safeGap = std::max(0.0, gap);
    const double availableMain = std::max(1.0, mainLength - safePadding * 2.0);
    const double availableCross = std::max(1.0, crossLength - safePadding * 2.0);
    const double aspect = std::max(0.05, workspaceAspectRatio);
    const double scale = std::clamp(workspaceScale, 0.05, 1.0);

    const double slotCross = availableCross * scale;
    const double slotMain = horizontal ? slotCross * aspect : slotCross / aspect;
    const double effectiveGap = slotCount > 1 ? safeGap : 0.0;
    const double contentMain = slotMain * static_cast<double>(slotCount) + effectiveGap * static_cast<double>(slotCount - 1);

    const double minStart = mainStart + safePadding;
    const double maxStart = mainStart + mainLength - safePadding - contentMain;
    double       cursor = minStart + std::max(0.0, (availableMain - contentMain) * 0.5);
    if (activeIndex && *activeIndex < slotCount) {
        const double activeCenterInContent = static_cast<double>(*activeIndex) * (slotMain + effectiveGap) + slotMain * 0.5;
        cursor = mainStart + mainLength * 0.5 - activeCenterInContent;
        if (contentMain <= availableMain)
            cursor = std::clamp(cursor, minStart, std::max(minStart, maxStart));
    } else if (contentMain > availableMain) {
        cursor = mainStart + (mainLength - contentMain) * 0.5;
    }

    const double cross = crossStart + (crossLength - slotCross) * 0.5;
    for (std::size_t index = 0; index < slotCount; ++index) {
        if (horizontal)
            slots.push_back({cursor, cross, slotMain, slotCross});
        else
            slots.push_back({cross, cursor, slotCross, slotMain});
        cursor += slotMain + effectiveGap;
    }

    return slots;
}

std::optional<std::size_t> hitTestWorkspaceStrip(const std::vector<Rect>& rects, double x, double y) {
    return hitTest(rects, x, y);
}

} // namespace hymission
