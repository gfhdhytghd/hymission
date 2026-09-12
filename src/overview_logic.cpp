#include "overview_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>

#include <glib.h>

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

struct SpatialPickSlot {
    std::size_t          primaryKeyIndex = 0;
    SpatialPickDirection direction = SpatialPickDirection::Center;
    std::size_t          canonicalSecondaryKeyIndex = 0;
    double               x = 0.0;
    double               y = 0.0;
};

std::vector<std::size_t> minimumCostAssignment(const std::vector<std::vector<double>>& costs) {
    if (costs.empty() || costs.front().empty() || costs.size() > costs.front().size())
        return {};

    const std::size_t rows = costs.size();
    const std::size_t cols = costs.front().size();
    std::vector<double>     u(rows + 1);
    std::vector<double>     v(cols + 1);
    std::vector<std::size_t> p(cols + 1);
    std::vector<std::size_t> way(cols + 1);

    for (std::size_t row = 1; row <= rows; ++row) {
        p[0] = row;
        std::size_t currentColumn = 0;
        std::vector<double> minValue(cols + 1, std::numeric_limits<double>::infinity());
        std::vector<bool>   used(cols + 1, false);

        do {
            used[currentColumn] = true;
            const std::size_t currentRow = p[currentColumn];
            double            delta = std::numeric_limits<double>::infinity();
            std::size_t       nextColumn = 0;

            for (std::size_t column = 1; column <= cols; ++column) {
                if (used[column])
                    continue;

                const double current = costs[currentRow - 1][column - 1] - u[currentRow] - v[column];
                if (current < minValue[column]) {
                    minValue[column] = current;
                    way[column] = currentColumn;
                }

                if (minValue[column] < delta || (minValue[column] == delta && column < nextColumn)) {
                    delta = minValue[column];
                    nextColumn = column;
                }
            }

            for (std::size_t column = 0; column <= cols; ++column) {
                if (used[column]) {
                    u[p[column]] += delta;
                    v[column] -= delta;
                } else {
                    minValue[column] -= delta;
                }
            }

            currentColumn = nextColumn;
        } while (p[currentColumn] != 0);

        do {
            const std::size_t previousColumn = way[currentColumn];
            p[currentColumn] = p[previousColumn];
            currentColumn = previousColumn;
        } while (currentColumn != 0);
    }

    std::vector<std::size_t> assignment(rows, cols);
    for (std::size_t column = 1; column <= cols; ++column) {
        if (p[column] != 0)
            assignment[p[column] - 1] = column - 1;
    }
    return assignment;
}

std::vector<SpatialPickSlot> spatialPickSlots() {
    const auto& keys = spatialPickKeys();
    std::vector<SpatialPickSlot> slots;
    slots.reserve(keys.size() * 5);

    for (std::size_t primary = 0; primary < keys.size(); ++primary) {
        slots.push_back({
            .primaryKeyIndex = primary,
            .direction = SpatialPickDirection::Center,
            .canonicalSecondaryKeyIndex = primary,
            .x = keys[primary].x,
            .y = keys[primary].y,
        });
    }

    constexpr SpatialPickDirection DIRECTIONS[] = {
        SpatialPickDirection::Left,
        SpatialPickDirection::Right,
        SpatialPickDirection::Up,
        SpatialPickDirection::Down,
    };

    for (std::size_t primary = 0; primary < keys.size(); ++primary) {
        for (const auto direction : DIRECTIONS) {
            std::optional<std::size_t> canonical;
            double                     bestDistance = std::numeric_limits<double>::infinity();
            for (std::size_t secondary = 0; secondary < keys.size(); ++secondary) {
                if (spatialPickDirectionForKeys(primary, secondary) != direction)
                    continue;

                const double dx = keys[secondary].x - keys[primary].x;
                const double dy = keys[secondary].y - keys[primary].y;
                const double distance = dx * dx + dy * dy;
                if (!canonical || distance < bestDistance || (distance == bestDistance && secondary < *canonical)) {
                    canonical = secondary;
                    bestDistance = distance;
                }
            }

            if (!canonical)
                continue;

            slots.push_back({
                .primaryKeyIndex = primary,
                .direction = direction,
                .canonicalSecondaryKeyIndex = *canonical,
                .x = keys[*canonical].x,
                .y = keys[*canonical].y,
            });
        }
    }

    return slots;
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

std::vector<std::size_t> computePickOrder(const std::vector<Rect>& rects, const std::vector<std::size_t>& monitorRanks) {
    const std::size_t n = rects.size();
    if (n == 0 || monitorRanks.size() != n)
        return {};

    std::size_t maxRank = 0;
    for (const std::size_t rank : monitorRanks)
        maxRank = std::max(maxRank, rank);

    std::vector<std::vector<std::size_t>> byMonitor(maxRank + 1);
    for (std::size_t i = 0; i < n; ++i)
        byMonitor[monitorRanks[i]].push_back(i);

    std::vector<std::size_t> order;
    order.reserve(n);

    for (auto& bucket : byMonitor) {
        std::sort(bucket.begin(), bucket.end(), [&](std::size_t a, std::size_t b) { return rects[a].y < rects[b].y; });

        std::vector<std::vector<std::size_t>> rows;
        for (const std::size_t idx : bucket) {
            if (!rows.empty()) {
                const std::size_t rowRef = rows.back().front();
                if (std::abs(rects[idx].y - rects[rowRef].y) <= rects[rowRef].height * 0.5) {
                    rows.back().push_back(idx);
                    continue;
                }
            }
            rows.push_back({idx});
        }

        for (auto& row : rows) {
            std::sort(row.begin(), row.end(), [&](std::size_t a, std::size_t b) { return rects[a].x < rects[b].x; });
            order.insert(order.end(), row.begin(), row.end());
        }
    }

    return order;
}

std::string computePickLabel(std::size_t orderIndex) {
    if (orderIndex < 9)
        return std::to_string(orderIndex + 1);

    const std::size_t rem = orderIndex - 9;
    const std::size_t groupIndex = rem / 9;
    const std::size_t withinGroup = rem % 9;
    if (groupIndex >= 26)
        return {};

    return std::string(1, static_cast<char>('A' + groupIndex)) + std::to_string(withinGroup + 1);
}

std::size_t computePickOrderIndex(int digit1to9, std::optional<int> letterGroupAtoZ) {
    const auto within = static_cast<std::size_t>(digit1to9 - 1);
    if (!letterGroupAtoZ)
        return within;

    return 9 + static_cast<std::size_t>(*letterGroupAtoZ) * 9 + within;
}

bool pickLetterGroupAvailable(std::size_t windowCount, int letterGroupAtoZ) {
    if (letterGroupAtoZ < 0 || letterGroupAtoZ >= 26)
        return false;

    return computePickOrderIndex(1, letterGroupAtoZ) < windowCount;
}

PickLabelsMode parsePickLabelsMode(std::string_view value) {
    value = trimAsciiWhitespace(value);
    return equalsAsciiInsensitive(value, "spatial") ? PickLabelsMode::Spatial : PickLabelsMode::Sequential;
}

GroupedWindowsPolicy parseGroupedWindowsPolicy(std::string_view value) {
    value = trimAsciiWhitespace(value);
    return equalsAsciiInsensitive(value, "collapsed") ? GroupedWindowsPolicy::Collapsed : GroupedWindowsPolicy::Expanded;
}

std::vector<std::size_t> projectGroupedWindowIndices(const std::vector<GroupProjectionInput>& inputs, GroupedWindowsPolicy policy) {
    std::vector<std::size_t> result;
    result.reserve(inputs.size());
    if (policy == GroupedWindowsPolicy::Expanded) {
        for (std::size_t index = 0; index < inputs.size(); ++index)
            result.push_back(index);
        return result;
    }

    std::unordered_set<std::uintptr_t> emittedGroups;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        if (input.groupId == 0) {
            result.push_back(index);
            continue;
        }
        if (input.current && emittedGroups.insert(input.groupId).second)
            result.push_back(index);
    }
    return result;
}

std::optional<std::size_t> hitTestEqualSegments(const Rect& bounds, std::size_t count, double x, double y) {
    if (count == 0 || bounds.width <= 0.0 || bounds.height <= 0.0 || !contains(bounds, x, y))
        return std::nullopt;

    const double segmentWidth = bounds.width / static_cast<double>(count);
    if (segmentWidth <= 0.0)
        return std::nullopt;

    return std::min(count - 1, static_cast<std::size_t>(std::floor((x - bounds.x) / segmentWidth)));
}

bool shouldSuppressCollapsedGroupMember(std::uintptr_t itemGroupId, std::uintptr_t windowGroupId, std::uintptr_t boundWindowId, std::uintptr_t windowId) {
    return itemGroupId != 0 && windowGroupId == itemGroupId && boundWindowId != 0 && windowId != 0 && windowId != boundWindowId;
}

float resolveExpandedGroupEffectiveAlpha(float originalAlpha, float previewAlpha, bool overviewVisible, bool rawRenderActive,
                                         bool groupedOverviewItem, bool collapsedOverviewItem) {
    if (!overviewVisible || rawRenderActive || !groupedOverviewItem || collapsedOverviewItem)
        return originalAlpha;

    return std::max(originalAlpha, previewAlpha);
}

bool shouldRefreshDraggedCompositeTexture(bool compositeCapture, bool textureAvailable) {
    return compositeCapture && !textureAvailable;
}

Rect floatingSegmentBarRect(const Rect& preview, std::size_t count, double height, double gap, double minSegmentWidth,
                            double maxSegmentWidth, double horizontalInset) {
    if (count < 2 || preview.width <= 0.0 || preview.height <= 0.0)
        return {};

    height = std::max(1.0, height);
    gap = std::max(0.0, gap);
    horizontalInset = std::max(0.0, horizontalInset);
    minSegmentWidth = std::max(1.0, minSegmentWidth);
    maxSegmentWidth = std::max(minSegmentWidth, maxSegmentWidth);

    const double availableWidth = std::max(1.0, preview.width - horizontalInset * 2.0);
    const double preferredWidth = std::clamp(availableWidth / static_cast<double>(count), minSegmentWidth, maxSegmentWidth) * static_cast<double>(count);
    const double width = std::min(availableWidth, preferredWidth);
    return {
        preview.centerX() - width * 0.5,
        preview.y - gap - height,
        width,
        height,
    };
}

std::vector<Rect> stackedGroupPreviewRects(const std::vector<Rect>& sourceRects, std::size_t frontIndex, double pointerX, double pointerY,
                                           double grabRatioX, double grabRatioY, double scale, double layerOffset, double maxSpread) {
    std::vector<Rect> result(sourceRects.size());
    if (sourceRects.empty() || frontIndex >= sourceRects.size())
        return result;

    scale = std::clamp(scale, 0.01, 1.0);
    grabRatioX = std::clamp(grabRatioX, 0.0, 1.0);
    grabRatioY = std::clamp(grabRatioY, 0.0, 1.0);
    layerOffset = std::max(0.0, layerOffset);
    maxSpread = std::max(0.0, maxSpread);

    const Rect& frontSource = sourceRects[frontIndex];
    const Rect front = {
        pointerX - frontSource.width * scale * grabRatioX,
        pointerY - frontSource.height * scale * grabRatioY,
        std::max(1.0, frontSource.width * scale),
        std::max(1.0, frontSource.height * scale),
    };
    result[frontIndex] = front;

    const std::size_t backCount = sourceRects.size() - 1;
    const double effectiveOffset = backCount == 0 ? 0.0 : std::min(layerOffset, maxSpread / static_cast<double>(backCount));
    std::size_t backLayer = 0;
    for (std::size_t index = 0; index < sourceRects.size(); ++index) {
        if (index == frontIndex)
            continue;

        ++backLayer;
        const Rect& source = sourceRects[index];
        const double width = std::max(1.0, source.width * scale);
        const double height = std::max(1.0, source.height * scale);
        const double offset = effectiveOffset * static_cast<double>(backCount - backLayer + 1);
        result[index] = {
            front.centerX() - width * 0.5 + offset,
            front.centerY() - height * 0.5 + offset,
            width,
            height,
        };
    }

    return result;
}

const std::vector<SpatialPickKey>& spatialPickKeys() {
    static const std::vector<SpatialPickKey> KEYS = {
        // Physical ANSI positions, including punctuation. Labels are layout-aware
        // at render time, while routing remains tied to physical keycodes.
        {'`', 0.5 / 15.0, 0.5 / 4.0}, {'1', 1.5 / 15.0, 0.5 / 4.0}, {'2', 2.5 / 15.0, 0.5 / 4.0},
        {'3', 3.5 / 15.0, 0.5 / 4.0}, {'4', 4.5 / 15.0, 0.5 / 4.0}, {'5', 5.5 / 15.0, 0.5 / 4.0},
        {'6', 6.5 / 15.0, 0.5 / 4.0}, {'7', 7.5 / 15.0, 0.5 / 4.0}, {'8', 8.5 / 15.0, 0.5 / 4.0},
        {'9', 9.5 / 15.0, 0.5 / 4.0}, {'0', 10.5 / 15.0, 0.5 / 4.0}, {'-', 11.5 / 15.0, 0.5 / 4.0},
        {'=', 12.5 / 15.0, 0.5 / 4.0},
        {'Q', 2.0 / 15.0, 1.5 / 4.0}, {'W', 3.0 / 15.0, 1.5 / 4.0}, {'E', 4.0 / 15.0, 1.5 / 4.0},
        {'R', 5.0 / 15.0, 1.5 / 4.0}, {'T', 6.0 / 15.0, 1.5 / 4.0}, {'Y', 7.0 / 15.0, 1.5 / 4.0},
        {'U', 8.0 / 15.0, 1.5 / 4.0}, {'I', 9.0 / 15.0, 1.5 / 4.0}, {'O', 10.0 / 15.0, 1.5 / 4.0},
        {'P', 11.0 / 15.0, 1.5 / 4.0}, {'[', 12.0 / 15.0, 1.5 / 4.0}, {']', 13.0 / 15.0, 1.5 / 4.0},
        {'\\', 14.0 / 15.0, 1.5 / 4.0},
        {'A', 2.25 / 15.0, 2.5 / 4.0}, {'S', 3.25 / 15.0, 2.5 / 4.0}, {'D', 4.25 / 15.0, 2.5 / 4.0},
        {'F', 5.25 / 15.0, 2.5 / 4.0}, {'G', 6.25 / 15.0, 2.5 / 4.0}, {'H', 7.25 / 15.0, 2.5 / 4.0},
        {'J', 8.25 / 15.0, 2.5 / 4.0}, {'K', 9.25 / 15.0, 2.5 / 4.0}, {'L', 10.25 / 15.0, 2.5 / 4.0},
        {';', 11.25 / 15.0, 2.5 / 4.0}, {'\'', 12.25 / 15.0, 2.5 / 4.0},
        {'Z', 2.75 / 15.0, 3.5 / 4.0}, {'X', 3.75 / 15.0, 3.5 / 4.0}, {'C', 4.75 / 15.0, 3.5 / 4.0},
        {'V', 5.75 / 15.0, 3.5 / 4.0}, {'B', 6.75 / 15.0, 3.5 / 4.0}, {'N', 7.75 / 15.0, 3.5 / 4.0},
        {'M', 8.75 / 15.0, 3.5 / 4.0}, {',', 9.75 / 15.0, 3.5 / 4.0}, {'.', 10.75 / 15.0, 3.5 / 4.0},
        {'/', 11.75 / 15.0, 3.5 / 4.0},
    };
    return KEYS;
}

std::optional<std::size_t> spatialPickKeyIndex(char label) {
    label = static_cast<char>(std::toupper(static_cast<unsigned char>(label)));
    const auto& keys = spatialPickKeys();
    const auto  it = std::find_if(keys.begin(), keys.end(), [&](const SpatialPickKey& key) { return key.label == label; });
    if (it == keys.end())
        return std::nullopt;
    return static_cast<std::size_t>(std::distance(keys.begin(), it));
}

std::optional<SpatialPickDirection> spatialPickDirectionForKeys(std::size_t primaryKeyIndex, std::size_t secondaryKeyIndex) {
    const auto& keys = spatialPickKeys();
    if (primaryKeyIndex >= keys.size() || secondaryKeyIndex >= keys.size())
        return std::nullopt;
    if (primaryKeyIndex == secondaryKeyIndex)
        return SpatialPickDirection::Center;

    const double dx = keys[secondaryKeyIndex].x - keys[primaryKeyIndex].x;
    const double dy = keys[secondaryKeyIndex].y - keys[primaryKeyIndex].y;
    constexpr double EPSILON = 1e-9;
    constexpr double KEY_X = 1.0 / 15.0;
    constexpr double KEY_Y = 1.0 / 4.0;

    if (std::abs(dy) <= EPSILON && std::abs(std::abs(dx) - KEY_X) <= EPSILON)
        return dx < 0.0 ? SpatialPickDirection::Left : SpatialPickDirection::Right;

    if (std::abs(std::abs(dy) - KEY_Y) <= EPSILON && std::abs(dx) <= 0.75 / 15.0 + EPSILON)
        return dy < 0.0 ? SpatialPickDirection::Up : SpatialPickDirection::Down;

    return std::nullopt;
}

SpatialPickMap computeSpatialPickMap(const std::vector<SpatialPickPoint>& windowCenters) {
    SpatialPickMap result;
    const auto&    keys = spatialPickKeys();
    result.nearestWindowByKey.resize(keys.size());
    if (windowCenters.empty())
        return result;

    for (std::size_t keyIndex = 0; keyIndex < keys.size(); ++keyIndex) {
        std::size_t nearest = 0;
        double      nearestDistance = std::numeric_limits<double>::infinity();
        for (std::size_t windowIndex = 0; windowIndex < windowCenters.size(); ++windowIndex) {
            const double dx = windowCenters[windowIndex].x - keys[keyIndex].x;
            const double dy = windowCenters[windowIndex].y - keys[keyIndex].y;
            const double distance = dx * dx + dy * dy;
            if (distance < nearestDistance) {
                nearest = windowIndex;
                nearestDistance = distance;
            }
        }
        result.nearestWindowByKey[keyIndex] = nearest;
    }

    const auto slots = spatialPickSlots();
    if (slots.empty())
        return result;

    const auto routeCost = [&](std::size_t windowIndex, std::size_t slotIndex) {
        const double dx = windowCenters[windowIndex].x - slots[slotIndex].x;
        const double dy = windowCenters[windowIndex].y - slots[slotIndex].y;
        const double directionPenalty = slots[slotIndex].direction == SpatialPickDirection::Center ? 0.0 : 4.0;
        return directionPenalty + dx * dx + dy * dy + static_cast<double>(slotIndex) * 1e-10 + static_cast<double>(windowIndex) * 1e-12;
    };

    std::vector<std::optional<std::size_t>> slotForWindow(windowCenters.size());
    if (windowCenters.size() <= slots.size()) {
        std::vector<std::vector<double>> costs(windowCenters.size(), std::vector<double>(slots.size()));
        for (std::size_t windowIndex = 0; windowIndex < windowCenters.size(); ++windowIndex) {
            for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex)
                costs[windowIndex][slotIndex] = routeCost(windowIndex, slotIndex);
        }

        const auto assignment = minimumCostAssignment(costs);
        for (std::size_t windowIndex = 0; windowIndex < assignment.size(); ++windowIndex)
            slotForWindow[windowIndex] = assignment[windowIndex];
    } else {
        std::vector<std::vector<double>> costs(slots.size(), std::vector<double>(windowCenters.size()));
        for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
            for (std::size_t windowIndex = 0; windowIndex < windowCenters.size(); ++windowIndex)
                costs[slotIndex][windowIndex] = routeCost(windowIndex, slotIndex);
        }

        const auto assignment = minimumCostAssignment(costs);
        for (std::size_t slotIndex = 0; slotIndex < assignment.size(); ++slotIndex)
            slotForWindow[assignment[slotIndex]] = slotIndex;
    }

    for (std::size_t windowIndex = 0; windowIndex < slotForWindow.size(); ++windowIndex) {
        if (!slotForWindow[windowIndex])
            continue;
        const auto& slot = slots[*slotForWindow[windowIndex]];
        result.routes.push_back({
            .windowIndex = windowIndex,
            .primaryKeyIndex = slot.primaryKeyIndex,
            .direction = slot.direction,
            .canonicalSecondaryKeyIndex = slot.canonicalSecondaryKeyIndex,
        });
    }

    return result;
}

std::size_t spatialPickRouteCount(const SpatialPickMap& map, std::size_t primaryKeyIndex) {
    return static_cast<std::size_t>(
        std::ranges::count_if(map.routes, [&](const SpatialPickRoute& route) { return route.primaryKeyIndex == primaryKeyIndex; }));
}

std::optional<std::size_t> resolveSpatialPickPrimary(const SpatialPickMap& map, std::size_t primaryKeyIndex) {
    const SpatialPickRoute* route = nullptr;
    std::size_t             routeCount = 0;
    for (const auto& candidate : map.routes) {
        if (candidate.primaryKeyIndex != primaryKeyIndex)
            continue;
        route = &candidate;
        ++routeCount;
    }

    if (routeCount == 1)
        return route->windowIndex;
    if (routeCount > 1)
        return std::nullopt;
    if (primaryKeyIndex < map.nearestWindowByKey.size())
        return map.nearestWindowByKey[primaryKeyIndex];
    return std::nullopt;
}

std::optional<std::size_t> resolveSpatialPickChord(const SpatialPickMap& map, std::size_t primaryKeyIndex, std::size_t secondaryKeyIndex) {
    const auto direction = spatialPickDirectionForKeys(primaryKeyIndex, secondaryKeyIndex);
    if (!direction)
        return std::nullopt;

    const auto route = std::find_if(map.routes.begin(), map.routes.end(), [&](const SpatialPickRoute& candidate) {
        return candidate.primaryKeyIndex == primaryKeyIndex && candidate.direction == *direction;
    });
    if (route == map.routes.end())
        return std::nullopt;
    return route->windowIndex;
}

std::optional<ToggleArguments> parseToggleArguments(std::string_view value) {
    ToggleArguments result;
    value = trimAsciiWhitespace(value);
    if (value.empty())
        return result;

    while (true) {
        const std::size_t separator = value.find(',');
        const std::string_view token = trimAsciiWhitespace(value.substr(0, separator));
        if (token.empty())
            return std::nullopt;

        if (token == "reverse") {
            if (result.direction == ToggleDirection::Reverse)
                return std::nullopt;
            result.direction = ToggleDirection::Reverse;
        } else if (token == "onlycurrentworkspace" || token == "forceall") {
            if (!result.scope.empty())
                return std::nullopt;
            result.scope = std::string(token);
        } else {
            return std::nullopt;
        }

        if (separator == std::string_view::npos)
            break;
        value.remove_prefix(separator + 1);
    }

    return result;
}

std::optional<std::string> legacyFullscreenDispatcherArguments(std::string_view mode, std::string_view action) {
    mode = trimAsciiWhitespace(mode);
    action = trimAsciiWhitespace(action);

    const char* legacyMode = nullptr;
    if (mode.empty() || equalsAsciiInsensitive(mode, "fullscreen") || mode == "0")
        legacyMode = "0";
    else if (equalsAsciiInsensitive(mode, "maximized") || mode == "1")
        legacyMode = "1";
    else
        return std::nullopt;

    const char* legacyAction = nullptr;
    if (action.empty() || equalsAsciiInsensitive(action, "toggle"))
        legacyAction = "toggle";
    else if (equalsAsciiInsensitive(action, "set"))
        legacyAction = "set";
    else if (equalsAsciiInsensitive(action, "unset"))
        legacyAction = "unset";
    else
        return std::nullopt;

    return std::string{legacyMode} + " " + legacyAction;
}

Rect gestureIncomingWorkspaceEndpoint(const Rect& live, double renderOffsetX, double renderOffsetY) {
    // The gesture reaches the settled desktop, not the start of a second
    // workspace animation. Remove any in-flight native workspace translation.
    return {live.x - renderOffsetX, live.y - renderOffsetY, live.width, live.height};
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

double easeInOutCubic(double t) {
    const double clamped = clampUnit(t);
    if (clamped < 0.5)
        return 4.0 * clamped * clamped * clamped;

    const double shifted = -2.0 * clamped + 2.0;
    return 1.0 - shifted * shifted * shifted * 0.5;
}

HoverRelayoutCurve parseHoverRelayoutCurve(std::string_view value) {
    value = trimAsciiWhitespace(value);

    if (equalsAsciiInsensitive(value, "linear"))
        return HoverRelayoutCurve::Linear;
    if (equalsAsciiInsensitive(value, "ease_in_cubic") || equalsAsciiInsensitive(value, "ease-in-cubic"))
        return HoverRelayoutCurve::EaseInCubic;
    if (equalsAsciiInsensitive(value, "ease_out_cubic") || equalsAsciiInsensitive(value, "ease-out-cubic") || value.empty())
        return HoverRelayoutCurve::EaseOutCubic;
    if (equalsAsciiInsensitive(value, "ease_in_out_cubic") || equalsAsciiInsensitive(value, "ease-in-out-cubic"))
        return HoverRelayoutCurve::EaseInOutCubic;

    return HoverRelayoutCurve::EaseOutCubic;
}

double applyHoverRelayoutCurve(HoverRelayoutCurve curve, double t) {
    switch (curve) {
        case HoverRelayoutCurve::Linear:
            return clampUnit(t);
        case HoverRelayoutCurve::EaseInCubic:
            return easeInCubic(t);
        case HoverRelayoutCurve::EaseOutCubic:
            return easeOutCubic(t);
        case HoverRelayoutCurve::EaseInOutCubic:
            return easeInOutCubic(t);
    }

    return easeOutCubic(t);
}

std::vector<WindowExpansionTarget> resolveWindowExpansionTargets(std::optional<std::size_t> selectedIndex,
                                                                 std::optional<std::size_t> hoveredIndex,
                                                                 bool overviewFocusFollowsMouse,
                                                                 double selectedExpandScale,
                                                                 double hoverExpandScale) {
    std::vector<WindowExpansionTarget> targets;
    targets.reserve(2);

    if (selectedIndex)
        targets.push_back({.index = *selectedIndex, .scale = std::max(1.0, selectedExpandScale)});

    if (!overviewFocusFollowsMouse && hoveredIndex && hoveredIndex != selectedIndex)
        targets.push_back({.index = *hoveredIndex, .scale = std::max(1.0, hoverExpandScale)});

    return targets;
}

bool shouldSyncOverviewLiveFocus(bool handlesInput, bool overviewFocusFollowsMouse, long inputFollowMouseBeforeOpen) {
    return handlesInput && overviewFocusFollowsMouse && inputFollowMouseBeforeOpen != 0;
}

bool shouldApplyOverviewWindowTransform(bool managedByOverview, bool closePending) {
    return managedByOverview && !closePending;
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

double niriScrollingPreviewCellLength(double layoutPrimaryLength, double fallbackPrimaryLength) {
    return std::max({1.0, layoutPrimaryLength, fallbackPrimaryLength});
}

double niriScrollingPreviewAdvance(double layoutPrimaryLength, double fallbackPrimaryLength, double gap) {
    return niriScrollingPreviewCellLength(layoutPrimaryLength, fallbackPrimaryLength) + std::max(0.0, gap);
}

double niriOverviewPreviewScale(const Rect& previewArea, const Rect& baseArea, double maxPreviewScale, double minSlotScale, std::optional<GestureAxis> overflowAxis) {
    if (previewArea.width <= 1.0 || previewArea.height <= 1.0 || baseArea.width <= 1.0 || baseArea.height <= 1.0)
        return 0.0;

    double fitScale = std::min(previewArea.width / baseArea.width, previewArea.height / baseArea.height);
    if (overflowAxis == GestureAxis::Horizontal)
        fitScale = previewArea.height / baseArea.height;
    else if (overflowAxis == GestureAxis::Vertical)
        fitScale = previewArea.width / baseArea.width;

    const double maxScale = std::max(minSlotScale, maxPreviewScale);
    return std::max(minSlotScale, std::min(fitScale, maxScale));
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

std::string normalizedSearchText(std::string_view value) {
    if (value.empty())
        return {};

    gchar* normalized = g_utf8_normalize(value.data(), static_cast<gssize>(value.size()), G_NORMALIZE_ALL_COMPOSE);
    if (!normalized)
        return {};
    gchar* folded = g_utf8_casefold(normalized, -1);
    g_free(normalized);
    if (!folded)
        return {};
    std::string result = folded;
    g_free(folded);
    return result;
}

bool windowMatchesSearch(std::string_view title, std::string_view windowClass, std::string_view normalizedQuery) {
    if (normalizedQuery.empty())
        return true;
    return normalizedSearchText(title).contains(normalizedQuery) || normalizedSearchText(windowClass).contains(normalizedQuery);
}

bool overviewTextInputAllowed(uint32_t modifiers, uint32_t commandModifierMask) {
    return (modifiers & commandModifierMask) == 0;
}

} // namespace hymission
