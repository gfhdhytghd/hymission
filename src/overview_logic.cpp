#include "overview_logic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hymission {

namespace {

double clampUnit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

bool contains(const Rect& rect, double x, double y) {
    return x >= rect.x && y >= rect.y && x <= rect.x + rect.width && y <= rect.y + rect.height;
}

double centerDistanceSquared(const Rect& rect, double x, double y) {
    const double dx = rect.centerX() - x;
    const double dy = rect.centerY() - y;
    return dx * dx + dy * dy;
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

} // namespace hymission
