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

bool isWorkspaceStripHorizontal(WorkspaceStripAnchor anchor) {
    return anchor == WorkspaceStripAnchor::Top;
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

std::optional<std::size_t> hitTestWorkspaceStrip(const std::vector<Rect>& rects, double x, double y) {
    return hitTest(rects, x, y);
}

} // namespace hymission
