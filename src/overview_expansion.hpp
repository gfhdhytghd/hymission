#pragma once

#include "mission_layout.hpp"
#include "numeric_config.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace hymission::expansion {

// Keep every strict center ordering and every original separating axis.
// These are linear inequalities: interpolation between any two accepted
// expansions preserves them too, including a mid-animation hover retarget.
inline bool preservesPairOrder(const Rect &a, const Rect &b, const Rect &x, const Rect &y) {
    constexpr double epsilon = 1e-7;
    const auto ordered = [&](double before, double after) {
        return before > epsilon ? after >= 0.0 : before < -epsilon ? after <= 0.0 : true;
    };
    if (!ordered(a.centerX() - b.centerX(), x.centerX() - y.centerX()) || !ordered(a.centerY() - b.centerY(), x.centerY() - y.centerY()))
        return false;
    if (a.x + a.width <= b.x + epsilon && x.x + x.width > y.x + epsilon)
        return false;
    if (b.x + b.width <= a.x + epsilon && y.x + y.width > x.x + epsilon)
        return false;
    if (a.y + a.height <= b.y + epsilon && x.y + x.height > y.y + epsilon)
        return false;
    if (b.y + b.height <= a.y + epsilon && y.y + y.height > x.y + epsilon)
        return false;
    return true;
}

inline bool preservesOrder(const std::vector<Rect> &reference, const std::vector<Rect> &targets) {
    if (reference.size() != targets.size())
        return false;
    for (std::size_t i = 0; i < reference.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (!preservesPairOrder(reference[i], reference[j], targets[i], targets[j]))
                return false;
    return true;
}

inline double clampUnit(double value) { return finiteConfig(value, 1.0, 0.0, 1.0); }

inline Rect makeRect(double x, double y, double width, double height) {
    return {
        x,
        y,
        std::max(1.0, width),
        std::max(1.0, height),
    };
}

inline Rect translateRect(const Rect &rect, double dx, double dy) { return makeRect(rect.x + dx, rect.y + dy, rect.width, rect.height); }

inline Rect scaleRectAroundCenter(const Rect &rect, double scale) {
    const double clampedScale = std::max(0.0, scale);
    const double width = rect.width * clampedScale;
    const double height = rect.height * clampedScale;
    return makeRect(rect.centerX() - width * 0.5, rect.centerY() - height * 0.5, width, height);
}

inline Rect clampRectInside(const Rect &rect, const Rect &bounds) {
    const double width = std::min(rect.width, bounds.width);
    const double height = std::min(rect.height, bounds.height);
    double x = rect.x;
    double y = rect.y;

    if (x < bounds.x)
        x = bounds.x;
    if (x + width > bounds.x + bounds.width)
        x = bounds.x + bounds.width - width;

    if (y < bounds.y)
        y = bounds.y;
    if (y + height > bounds.y + bounds.height)
        y = bounds.y + bounds.height - height;

    return makeRect(x, y, width, height);
}

inline bool rectFitsInsideBounds(const Rect &rect, const Rect &bounds, double epsilon = 0.5) {
    return rect.x >= bounds.x - epsilon && rect.y >= bounds.y - epsilon && rect.x + rect.width <= bounds.x + bounds.width + epsilon &&
           rect.y + rect.height <= bounds.y + bounds.height + epsilon;
}

inline double maxCenteredScaleForBounds(const Rect &rect, const Rect &bounds) {
    if (rect.width <= 1.0 || rect.height <= 1.0)
        return 1.0;

    const double halfWidth = std::max(0.0, std::min(rect.centerX() - bounds.x, bounds.x + bounds.width - rect.centerX()));
    const double halfHeight = std::max(0.0, std::min(rect.centerY() - bounds.y, bounds.y + bounds.height - rect.centerY()));
    const double maxWidth = halfWidth * 2.0;
    const double maxHeight = halfHeight * 2.0;
    return std::max(1.0, std::min(maxWidth / rect.width, maxHeight / rect.height));
}

inline double maxCenteredScaleForPerSideGrowth(const Rect &rect, double maxGrowXPerSide, double maxGrowYPerSide) {
    if (rect.width <= 1.0 || rect.height <= 1.0)
        return 1.0;

    const double scaleX = 1.0 + std::max(0.0, maxGrowXPerSide) * 2.0 / rect.width;
    const double scaleY = 1.0 + std::max(0.0, maxGrowYPerSide) * 2.0 / rect.height;
    return std::max(1.0, std::min(scaleX, scaleY));
}

struct EdgeMotionAffinity {
    double verticalEdge = 0.0;
    double horizontalEdge = 0.0;
    double corner = 0.0;
};

inline EdgeMotionAffinity edgeMotionAffinityForRect(const Rect &rect, const Rect &bounds) {
    if (bounds.width <= 1.0 || bounds.height <= 1.0)
        return {};

    const double centerX = rect.centerX();
    const double centerY = rect.centerY();
    const double leftDistance = std::max(0.0, centerX - bounds.x);
    const double rightDistance = std::max(0.0, bounds.x + bounds.width - centerX);
    const double topDistance = std::max(0.0, centerY - bounds.y);
    const double bottomDistance = std::max(0.0, bounds.y + bounds.height - centerY);

    EdgeMotionAffinity affinity;
    affinity.verticalEdge = clampUnit(1.0 - std::min(leftDistance, rightDistance) / std::max(1.0, bounds.width * 0.24));
    affinity.horizontalEdge = clampUnit(1.0 - std::min(topDistance, bottomDistance) / std::max(1.0, bounds.height * 0.24));
    affinity.corner = affinity.verticalEdge * affinity.horizontalEdge;
    return affinity;
}

inline double edgeAwareMotionDistance2(const Rect &source, const Rect &target, const Rect &bounds) {
    const EdgeMotionAffinity affinity = edgeMotionAffinityForRect(source, bounds);
    const double dx = target.centerX() - source.centerX();
    const double dy = target.centerY() - source.centerY();
    const double xWeight = 1.0 + affinity.verticalEdge * 7.0 + affinity.corner * 10.0;
    const double yWeight = 1.0 + affinity.horizontalEdge * 7.0 + affinity.corner * 10.0;
    return dx * dx * xWeight + dy * dy * yWeight;
}

inline Rect inflateRect(const Rect &rect, double amountX, double amountY) {
    return makeRect(rect.x - amountX, rect.y - amountY, rect.width + amountX * 2.0, rect.height + amountY * 2.0);
}

inline bool rectsOverlap(const Rect &lhs, const Rect &rhs) {
    return lhs.x < rhs.x + rhs.width && lhs.x + lhs.width > rhs.x && lhs.y < rhs.y + rhs.height && lhs.y + lhs.height > rhs.y;
}

inline std::optional<double> overlapExitDistanceAlongDirection(const Rect &moving, const Rect &obstacle, double dirX, double dirY) {
    if (!rectsOverlap(moving, obstacle))
        return 0.0;

    std::optional<double> exitDistance;
    const auto consider = [&](double candidate) {
        if (candidate < 0.0)
            return;
        candidate += 0.5;
        if (!exitDistance || candidate < *exitDistance)
            exitDistance = candidate;
    };

    if (dirX > 0.001)
        consider((obstacle.x + obstacle.width - moving.x) / dirX);
    else if (dirX < -0.001)
        consider((moving.x + moving.width - obstacle.x) / -dirX);

    if (dirY > 0.001)
        consider((obstacle.y + obstacle.height - moving.y) / dirY);
    else if (dirY < -0.001)
        consider((moving.y + moving.height - obstacle.y) / -dirY);

    return exitDistance;
}

struct Result {
    std::vector<Rect> targets;
    double appliedScale, maxShift, scaleCap, scaleCapByGrowth, scaleCapByBounds, minGapX, minGapY, rippleRadius;
};

// Inputs are frozen slot geometry (orderTargets) and this batch's base targets.
// peerIndices limits pressure to the selected monitor/tile. The second selected
// + hovered expansion can start from the first without accumulating geometry.
inline Result compute(const std::vector<Rect> &baseTargets, const std::vector<Rect> &orderTargets, std::size_t currentManagedIndex,
                      const std::vector<std::size_t> &peerIndices, const Rect &boundsGlobal, const LayoutConfig &layoutConfig,
                      double preferredScale) {
    struct RipplePeer {
        std::size_t index = 0;
        Rect base;
        double distance = 0.0;
    };

    const Rect selectedBase = baseTargets[currentManagedIndex];
    const double minGapX = std::max(0.0, layoutConfig.columnSpacing * 0.25);
    const double minGapY = std::max(0.0, layoutConfig.rowSpacing * 0.25);
    const double maxGrowthXPerSide = std::max(0.0, layoutConfig.columnSpacing * 2.0);
    const double maxGrowthYPerSide = std::max(0.0, layoutConfig.rowSpacing * 2.0);
    const double scaleCapByGrowth = maxCenteredScaleForPerSideGrowth(selectedBase, maxGrowthXPerSide, maxGrowthYPerSide);
    const double scaleCapByBounds = maxCenteredScaleForBounds(selectedBase, boundsGlobal);
    const double scaleCap = std::max(1.0, std::min({preferredScale, scaleCapByGrowth, scaleCapByBounds}));
    const double rippleRadius =
        std::max(std::hypot(selectedBase.width, selectedBase.height) * 2.5, std::hypot(boundsGlobal.width, boundsGlobal.height) * 0.55);
    const double pressureCenterX = selectedBase.centerX();
    const double pressureCenterY = selectedBase.centerY();

    std::vector<RipplePeer> peers;
    peers.reserve(peerIndices.size());
    for (const auto index : peerIndices) {
        if (index == currentManagedIndex || index >= baseTargets.size())
            continue;
        const Rect base = baseTargets[index];
        const double distance = std::hypot(base.centerX() - pressureCenterX, base.centerY() - pressureCenterY);
        peers.push_back({.index = index, .base = base, .distance = distance});
    }

    std::stable_sort(peers.begin(), peers.end(), [](const RipplePeer &lhs, const RipplePeer &rhs) {
        return std::pair(lhs.distance, lhs.index) < std::pair(rhs.distance, rhs.index);
    });

    const auto tryApplyScale = [&](double scale, std::vector<Rect> &outTargets, double &outMaxShift) -> bool {
        outTargets = baseTargets;
        outMaxShift = 0.0;
        if (scale <= 1.0)
            return true; // No growth must not create radial pressure.

        const Rect selectedTarget = scaleRectAroundCenter(selectedBase, scale);
        if (!rectFitsInsideBounds(selectedTarget, boundsGlobal))
            return false;
        outTargets[currentManagedIndex] = selectedTarget;

        const double pressureGap = std::max({1.0, minGapX, minGapY});
        const double radialPressure =
            std::max(pressureGap, std::hypot(selectedTarget.width - selectedBase.width, selectedTarget.height - selectedBase.height) * 0.5 +
                                      pressureGap);

        std::vector<std::size_t> placed;
        placed.reserve(peers.size() + 1);
        placed.push_back(currentManagedIndex);

        const auto overlapsPlaced = [&](const Rect &target) {
            return std::ranges::any_of(placed, [&](std::size_t obstacleIndex) {
                return rectsOverlap(target, inflateRect(outTargets[obstacleIndex], minGapX, minGapY));
            });
        };

        const auto resolveAlongBearing = [&](Rect target, double dirX, double dirY) -> std::optional<Rect> {
            for (std::size_t pass = 0; pass < 6; ++pass) {
                bool changed = false;
                for (const auto obstacleIndex : placed) {
                    const Rect obstacle = inflateRect(outTargets[obstacleIndex], minGapX, minGapY);
                    if (!rectsOverlap(target, obstacle))
                        continue;

                    if (std::abs(dirX) < 0.001 && std::abs(dirY) < 0.001) {
                        dirX = target.centerX() >= pressureCenterX ? 1.0 : -1.0;
                        dirY = target.centerY() >= pressureCenterY ? 1.0 : -1.0;
                        const double length = std::hypot(dirX, dirY);
                        if (length <= 0.001)
                            return std::nullopt;
                        dirX /= length;
                        dirY /= length;
                    }

                    const auto exitDistance = overlapExitDistanceAlongDirection(target, obstacle, dirX, dirY);
                    if (!exitDistance)
                        continue;

                    target = translateRect(target, dirX * *exitDistance, dirY * *exitDistance);
                    target = clampRectInside(target, boundsGlobal);
                    changed = true;
                }

                if (!changed)
                    break;
            }

            if (!rectFitsInsideBounds(target, boundsGlobal) || overlapsPlaced(target))
                return std::nullopt;

            return target;
        };

        const auto motionDirectionsForPeer = [&](const Rect &base, double radialX, double radialY) {
            std::vector<std::pair<double, double>> directions;
            directions.reserve(10);

            const auto appendDirection = [&](double dx, double dy) {
                const double length = std::hypot(dx, dy);
                if (length <= 0.001)
                    return;
                dx /= length;
                dy /= length;
                const auto duplicate = std::ranges::any_of(directions, [&](const auto &existing) {
                    return std::abs(existing.first - dx) <= 0.001 && std::abs(existing.second - dy) <= 0.001;
                });
                if (!duplicate)
                    directions.push_back({dx, dy});
            };

            const EdgeMotionAffinity affinity = edgeMotionAffinityForRect(base, boundsGlobal);
            const double awayX = std::abs(radialX) > 0.001 ? radialX : (base.centerX() >= pressureCenterX ? 1.0 : -1.0);
            const double awayY = std::abs(radialY) > 0.001 ? radialY : (base.centerY() >= pressureCenterY ? 1.0 : -1.0);

            if (affinity.verticalEdge > 0.15) {
                appendDirection(0.0, awayY);
                appendDirection(0.0, 1.0);
                appendDirection(0.0, -1.0);
            }
            if (affinity.horizontalEdge > 0.15) {
                appendDirection(awayX, 0.0);
                appendDirection(1.0, 0.0);
                appendDirection(-1.0, 0.0);
            }

            appendDirection(radialX, radialY);
            appendDirection(radialX, 0.0);
            appendDirection(0.0, radialY);
            appendDirection(1.0, 0.0);
            appendDirection(-1.0, 0.0);
            appendDirection(0.0, 1.0);
            appendDirection(0.0, -1.0);
            return directions;
        };

        for (const auto &peer : peers) {
            const double deltaX = peer.base.centerX() - pressureCenterX;
            const double deltaY = peer.base.centerY() - pressureCenterY;
            double directionX = deltaX;
            double directionY = deltaY;
            const double directionLength = std::hypot(directionX, directionY);
            if (directionLength > 0.001) {
                directionX /= directionLength;
                directionY /= directionLength;
            } else {
                directionX = peer.base.centerX() >= pressureCenterX ? 1.0 : -1.0;
                directionY = peer.base.centerY() >= pressureCenterY ? 1.0 : -1.0;
                const double fallbackLength = std::hypot(directionX, directionY);
                if (fallbackLength <= 0.001)
                    return false;
                directionX /= fallbackLength;
                directionY /= fallbackLength;
            }

            const double influence = clampUnit(1.0 - peer.distance / std::max(1.0, rippleRadius));
            const double easedInfluence = influence * influence;
            Rect target =
                translateRect(peer.base, directionX * radialPressure * easedInfluence, directionY * radialPressure * easedInfluence);
            target = clampRectInside(target, boundsGlobal);

            std::optional<Rect> resolved;
            double resolvedScore = std::numeric_limits<double>::max();
            for (const auto &[candidateDirX, candidateDirY] : motionDirectionsForPeer(peer.base, directionX, directionY)) {
                auto candidate = resolveAlongBearing(target, candidateDirX, candidateDirY);
                if (!candidate)
                    candidate = resolveAlongBearing(clampRectInside(peer.base, boundsGlobal), candidateDirX, candidateDirY);
                if (!candidate)
                    continue;
                if (std::ranges::any_of(placed, [&](std::size_t obstacleIndex) {
                        return !preservesPairOrder(orderTargets[peer.index], orderTargets[obstacleIndex], *candidate,
                                                   outTargets[obstacleIndex]);
                    }))
                    continue;

                const double score = edgeAwareMotionDistance2(peer.base, *candidate, boundsGlobal);
                if (!resolved || score < resolvedScore) {
                    resolved = candidate;
                    resolvedScore = score;
                }
            }
            if (!resolved)
                return false;

            outTargets[peer.index] = *resolved;
            placed.push_back(peer.index);
            outMaxShift =
                std::max(outMaxShift, std::hypot(resolved->centerX() - peer.base.centerX(), resolved->centerY() - peer.base.centerY()));
        }

        return preservesOrder(orderTargets, outTargets);
    };

    std::vector<Rect> bestTargets = baseTargets;
    std::vector<Rect> candidateTargets;
    double bestMaxShift = 0.0;
    double maxShift = 0.0;
    double appliedScale = 1.0;

    if (tryApplyScale(scaleCap, bestTargets, bestMaxShift)) {
        appliedScale = scaleCap;
        maxShift = bestMaxShift;
    } else {
        const bool baseResolved = tryApplyScale(1.0, bestTargets, bestMaxShift);
        maxShift = baseResolved ? bestMaxShift : 0.0;

        double low = 1.0;
        double high = scaleCap;
        for (std::size_t iteration = 0; iteration < 12 && high - low > 0.001; ++iteration) {
            const double mid = (low + high) * 0.5;
            double candidateMaxShift = 0.0;
            if (tryApplyScale(mid, candidateTargets, candidateMaxShift)) {
                appliedScale = mid;
                maxShift = candidateMaxShift;
                bestTargets = candidateTargets;
                low = mid;
            } else {
                high = mid;
            }
        }
    }

    return {std::move(bestTargets), appliedScale, maxShift, scaleCap, scaleCapByGrowth, scaleCapByBounds, minGapX, minGapY, rippleRadius};
}

} // namespace hymission::expansion
