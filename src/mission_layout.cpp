#include "mission_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace hymission {

namespace {

struct PreparedWindow {
    WindowInput input;
    double      weightScale = 1.0;
    double      layoutWidth = 0.0;
    double      layoutHeight = 0.0;
    double      scaledWidth = 0.0;
    double      scaledHeight = 0.0;
};

struct Row {
    std::vector<PreparedWindow> windows;
    double                      fullWidth = 0.0;
    double                      fullHeight = 0.0;
    double                      width = 0.0;
    double                      height = 0.0;
    double                      x = 0.0;
    double                      y = 0.0;
    double                      additionalScale = 1.0;
};

struct LayoutCandidate {
    std::vector<Row> rows;
    std::size_t      maxColumns = 0;
    double           gridWidth = 0.0;
    double           gridHeight = 0.0;
    double           layoutScale = 1.0;
    double           score = -std::numeric_limits<double>::infinity();
};

struct NaturalItem {
    PreparedWindow window;
    double         anchorX = 0.0;
    double         anchorY = 0.0;
    Rect           cell;
    Rect           target;
    double         scale = 1.0;
};

struct NaturalAnchorMap {
    double sourceCenterX = 0.0;
    double sourceCenterY = 0.0;
    double scale = 1.0;
};

struct NaturalOverlapOffset {
    bool   active = false;
    double x = 0.0;
    double y = 0.0;
};

void clampRectToArea(Rect& rect, const Rect& area);

double clampPositive(double value) {
    return std::max(0.0, value);
}

double clampAdditionalScale(double value) {
    return std::clamp(value, 0.0, 1.0);
}

double normalizedMaxPreviewScale(const LayoutConfig& config) {
    return std::max(0.0, config.maxPreviewScale);
}

double normalizedMinSlotScale(const LayoutConfig& config) {
    return std::clamp(config.minSlotScale, 0.0, normalizedMaxPreviewScale(config));
}

double normalizedMinPreviewShortEdge(const LayoutConfig& config, const Rect& area) {
    return std::clamp(config.minPreviewShortEdge, 0.0, std::min(area.width, area.height));
}

double naturalFitScaleForWindow(const PreparedWindow& window, const Rect& area) {
    const double widthFit = area.width / std::max(1.0, window.input.natural.width);
    const double heightFit = area.height / std::max(1.0, window.input.natural.height);
    return std::max(0.0, std::min(widthFit, heightFit));
}

double clampLayoutScale(double value, const LayoutConfig& config) {
    return std::clamp(value, normalizedMinSlotScale(config), normalizedMaxPreviewScale(config));
}

double lerp(double a, double b, double t) {
    const auto clamped = std::clamp(t, 0.0, 1.0);
    return a + (b - a) * clamped;
}

double computeWindowScale(const Rect& naturalForLayout, const Rect& monitorArea, const LayoutConfig& config) {
    const double denom = std::max(1.0, monitorArea.height);
    const double ratio = std::clamp(naturalForLayout.height / denom, 0.0, 1.0);
    return lerp(config.smallWindowBoost, 1.0, ratio);
}

std::pair<double, double> previewSizeWithShortEdgeFloor(const PreparedWindow& window,
                                                        double                scale,
                                                        const Rect&           area,
                                                        const LayoutConfig&   config,
                                                        double                shortEdgeCap = std::numeric_limits<double>::infinity()) {
    double width = std::max(0.0, window.input.natural.width * scale);
    double height = std::max(0.0, window.input.natural.height * scale);
    if (width <= 0.0 || height <= 0.0)
        return {width, height};

    const double floor = std::min(normalizedMinPreviewShortEdge(config, area), std::max(0.0, shortEdgeCap));
    if (floor > 0.0) {
        width = std::max(width, std::min(floor, area.width));
        height = std::max(height, std::min(floor, area.height));
    }

    return {width, height};
}

bool keepSameRow(const Row& row, double candidateWidth, double idealRowWidth) {
    if (row.fullWidth + candidateWidth <= idealRowWidth)
        return true;

    if (idealRowWidth <= 0.0)
        return false;

    const double oldRatio = row.fullWidth / idealRowWidth;
    const double newRatio = (row.fullWidth + candidateWidth) / idealRowWidth;
    return std::abs(1.0 - newRatio) < std::abs(1.0 - oldRatio);
}

Rect insetArea(const Rect& area, const LayoutConfig& config) {
    const double top = std::max(0.0, config.outerPaddingTop);
    const double right = std::max(0.0, config.outerPaddingRight);
    const double bottom = std::max(0.0, config.outerPaddingBottom);
    const double left = std::max(0.0, config.outerPaddingLeft);
    return {
        area.x + left,
        area.y + top,
        std::max(1.0, area.width - left - right),
        std::max(1.0, area.height - top - bottom),
    };
}

std::vector<PreparedWindow> prepareWindows(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& config) {
    std::vector<PreparedWindow> prepared;
    prepared.reserve(windows.size());

    for (const auto& window : windows) {
        PreparedWindow item;
        item.input = window;
        item.layoutWidth = std::max(window.natural.width, config.minWindowLength);
        item.layoutHeight = std::max(window.natural.height, config.minWindowLength);
        const double emphasis = std::max(0.0, window.layoutEmphasis);
        item.weightScale = computeWindowScale({window.natural.x, window.natural.y, item.layoutWidth, item.layoutHeight}, area, config) * emphasis;
        item.scaledWidth = item.layoutWidth * item.weightScale;
        item.scaledHeight = item.layoutHeight * item.weightScale;
        prepared.push_back(item);
    }

    return prepared;
}

void finalizeCandidate(LayoutCandidate& candidate, const Rect& area, const LayoutConfig& config) {
    if (candidate.rows.empty())
        return;

    candidate.maxColumns = 0;
    candidate.gridWidth = 0.0;
    candidate.gridHeight = 0.0;

    for (const auto& row : candidate.rows) {
        candidate.maxColumns = std::max(candidate.maxColumns, row.windows.size());
        candidate.gridWidth = std::max(candidate.gridWidth, row.fullWidth);
        candidate.gridHeight += row.fullHeight;
    }

    const double horizontalSpacing = static_cast<double>(candidate.maxColumns > 0 ? candidate.maxColumns - 1 : 0) * config.columnSpacing;
    const double verticalSpacing = static_cast<double>(candidate.rows.size() > 0 ? candidate.rows.size() - 1 : 0) * config.rowSpacing;

    const double horizontalScale = (area.width - horizontalSpacing) / std::max(1.0, candidate.gridWidth);
    const double verticalScale = (area.height - verticalSpacing) / std::max(1.0, candidate.gridHeight);

    candidate.layoutScale = clampLayoutScale(std::min(horizontalScale, verticalScale), config);

    for (auto& row : candidate.rows) {
        row.width = row.fullWidth * candidate.layoutScale + static_cast<double>(row.windows.size() > 0 ? row.windows.size() - 1 : 0) * config.columnSpacing;
        row.height = row.fullHeight * candidate.layoutScale;
    }

    const double usedWidth = candidate.gridWidth * candidate.layoutScale + horizontalSpacing;
    const double usedHeight = candidate.gridHeight * candidate.layoutScale + verticalSpacing;
    const double areaPixels = std::max(1.0, area.width * area.height);
    const double space = (usedWidth * usedHeight) / areaPixels;

    candidate.score = candidate.layoutScale * config.layoutScaleWeight + space * config.layoutSpaceWeight;
}

LayoutCandidate buildRowCandidate(const std::vector<PreparedWindow>& prepared, std::size_t numRows, const Rect& area, const LayoutConfig& config) {
    LayoutCandidate candidate;

    if (config.forceRowGroups) {
        std::vector<PreparedWindow> sorted = prepared;
        std::stable_sort(sorted.begin(), sorted.end(), [&](const PreparedWindow& a, const PreparedWindow& b) {
            if (a.input.rowGroup != b.input.rowGroup)
                return a.input.rowGroup < b.input.rowGroup;
            if (config.preserveInputOrder)
                return false;
            return a.input.natural.centerX() < b.input.natural.centerX();
        });

        std::optional<std::size_t> currentGroup;
        for (const auto& window : sorted) {
            if (!currentGroup || *currentGroup != window.input.rowGroup) {
                candidate.rows.emplace_back();
                currentGroup = window.input.rowGroup;
            }

            auto& row = candidate.rows.back();
            row.windows.push_back(window);
            row.fullWidth += window.scaledWidth;
            row.fullHeight = std::max(row.fullHeight, window.scaledHeight);
        }

        finalizeCandidate(candidate, area, config);
        return candidate;
    }

    double totalWidth = 0.0;
    for (const auto& window : prepared)
        totalWidth += window.scaledWidth;

    const double idealRowWidth = totalWidth / static_cast<double>(numRows);

    std::vector<PreparedWindow> sorted = prepared;
    if (!config.preserveInputOrder) {
        std::sort(sorted.begin(), sorted.end(), [](const PreparedWindow& a, const PreparedWindow& b) {
            return a.input.natural.centerY() < b.input.natural.centerY();
        });
    }

    std::size_t windowIdx = 0;
    for (std::size_t rowIndex = 0; rowIndex < numRows; ++rowIndex) {
        Row row;
        for (; windowIdx < sorted.size(); ++windowIdx) {
            const auto& window = sorted[windowIdx];

            if (keepSameRow(row, window.scaledWidth, idealRowWidth) || rowIndex + 1 == numRows) {
                row.windows.push_back(window);
                row.fullWidth += window.scaledWidth;
                row.fullHeight = std::max(row.fullHeight, window.scaledHeight);
            } else {
                break;
            }
        }

        if (!row.windows.empty()) {
            if (!config.preserveInputOrder) {
                std::sort(row.windows.begin(), row.windows.end(), [](const PreparedWindow& a, const PreparedWindow& b) {
                    return a.input.natural.centerX() < b.input.natural.centerX();
                });
            }
            candidate.maxColumns = std::max(candidate.maxColumns, row.windows.size());
            candidate.gridWidth = std::max(candidate.gridWidth, row.fullWidth);
            candidate.gridHeight += row.fullHeight;
            candidate.rows.push_back(std::move(row));
        }
    }

    finalizeCandidate(candidate, area, config);
    return candidate;
}

std::vector<WindowSlot> materializeSlots(LayoutCandidate candidate, const Rect& area, const LayoutConfig& config) {
    if (candidate.rows.empty())
        return {};

    double heightWithoutSpacing = 0.0;
    for (const auto& row : candidate.rows)
        heightWithoutSpacing += row.height;

    const double verticalSpacing = static_cast<double>(candidate.rows.size() > 0 ? candidate.rows.size() - 1 : 0) * config.rowSpacing;
    const double additionalVerticalScale = clampAdditionalScale((area.height - verticalSpacing) / std::max(1.0, heightWithoutSpacing));

    double compensation = 0.0;
    double y = 0.0;

    for (auto& row : candidate.rows) {
        const double horizontalSpacing = static_cast<double>(row.windows.size() > 0 ? row.windows.size() - 1 : 0) * config.columnSpacing;
        const double widthWithoutSpacing = std::max(1.0, row.width - horizontalSpacing);
        const double additionalHorizontalScale = clampAdditionalScale((area.width - horizontalSpacing) / widthWithoutSpacing);

        if (additionalHorizontalScale < additionalVerticalScale) {
            row.additionalScale = additionalHorizontalScale;
            compensation += (additionalVerticalScale - additionalHorizontalScale) * row.height;
        } else {
            row.additionalScale = additionalVerticalScale;
        }

        row.x = area.x + clampPositive(area.width - (widthWithoutSpacing * row.additionalScale + horizontalSpacing)) / 2.0;
        row.y = area.y + clampPositive(area.height - (heightWithoutSpacing + verticalSpacing)) / 2.0 + y;
        y += row.height * row.additionalScale + config.rowSpacing;
    }

    compensation /= 2.0;

    std::vector<WindowSlot> slots;
    slots.reserve(candidate.rows.size() * candidate.maxColumns);

    for (const auto& row : candidate.rows) {
        const double rowY = row.y + compensation;
        const double rowHeight = row.height * row.additionalScale;

        double x = row.x;
        for (const auto& window : row.windows) {
            double scale = candidate.layoutScale * window.weightScale * row.additionalScale;
            const double cellWidth = window.layoutWidth * scale;
            const double cellHeight = window.layoutHeight * scale;

            scale = std::clamp(scale, 0.0, normalizedMaxPreviewScale(config));

            const auto [previewWidth, previewHeight] =
                previewSizeWithShortEdgeFloor(window, scale, area, config, std::min(cellWidth, cellHeight));
            const double previewX = std::floor(x + (cellWidth - previewWidth) / 2.0);
            const double previewY = std::floor(candidate.rows.size() == 1 ? rowY + (rowHeight - previewHeight) / 2.0
                                                                           : rowY + rowHeight - cellHeight);

            Rect target{
                previewX,
                previewY,
                previewWidth,
                previewHeight,
            };
            clampRectToArea(target, area);

            slots.push_back({
                .index = window.input.index,
                .natural = window.input.natural,
                .target = target,
                .scale = scale,
            });

            x += cellWidth + config.columnSpacing;
        }
    }

    std::sort(slots.begin(), slots.end(), [](const WindowSlot& a, const WindowSlot& b) {
        return a.index < b.index;
    });

    return slots;
}

void clampRectToArea(Rect& rect, const Rect& area) {
    if (rect.width >= area.width) {
        rect.x = area.x + (area.width - rect.width) / 2.0;
    } else {
        rect.x = std::clamp(rect.x, area.x, area.x + area.width - rect.width);
    }

    if (rect.height >= area.height) {
        rect.y = area.y + (area.height - rect.height) / 2.0;
    } else {
        rect.y = std::clamp(rect.y, area.y, area.y + area.height - rect.height);
    }
}

double overlapAlong(double minA, double maxA, double minB, double maxB, double spacing) {
    return std::min(maxA, maxB) - std::max(minA, minB) + spacing;
}

bool rectsOverlap(const Rect& lhs, const Rect& rhs) {
    return lhs.x < rhs.x + rhs.width && lhs.x + lhs.width > rhs.x && lhs.y < rhs.y + rhs.height && lhs.y + lhs.height > rhs.y;
}

NaturalAnchorMap buildNaturalAnchorMap(const std::vector<PreparedWindow>& prepared, const Rect& area) {
    if (prepared.empty())
        return {.sourceCenterX = area.centerX(), .sourceCenterY = area.centerY(), .scale = 1.0};

    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();

    for (const auto& window : prepared) {
        const Rect& natural = window.input.natural;
        minX = std::min(minX, natural.x);
        minY = std::min(minY, natural.y);
        maxX = std::max(maxX, natural.x + natural.width);
        maxY = std::max(maxY, natural.y + natural.height);
    }

    const double sourceWidth = std::max(1.0, maxX - minX);
    const double sourceHeight = std::max(1.0, maxY - minY);
    const double minSpreadWidth = std::max(sourceWidth, area.width * 0.35);
    const double minSpreadHeight = std::max(sourceHeight, area.height * 0.35);
    const double fitScale = std::min(area.width / minSpreadWidth, area.height / minSpreadHeight);

    return {
        .sourceCenterX = (minX + maxX) / 2.0,
        .sourceCenterY = (minY + maxY) / 2.0,
        .scale = std::min(1.0, fitScale),
    };
}

std::vector<NaturalOverlapOffset> buildNaturalOverlapOffsets(const std::vector<PreparedWindow>& prepared, const Rect& area) {
    std::vector<NaturalOverlapOffset> offsets(prepared.size());
    std::vector<std::size_t>          overlapping;

    for (std::size_t i = 0; i < prepared.size(); ++i) {
        bool overlapsPeer = false;
        for (std::size_t j = 0; j < prepared.size(); ++j) {
            if (i == j)
                continue;
            if (rectsOverlap(prepared[i].input.natural, prepared[j].input.natural)) {
                overlapsPeer = true;
                break;
            }
        }

        if (overlapsPeer)
            overlapping.push_back(i);
    }

    if (overlapping.size() <= 1)
        return offsets;

    auto byX = overlapping;
    auto byY = overlapping;
    std::stable_sort(byX.begin(), byX.end(), [&](std::size_t a, std::size_t b) {
        const double ac = prepared[a].input.natural.centerX();
        const double bc = prepared[b].input.natural.centerX();
        if (std::abs(ac - bc) > 0.5)
            return ac < bc;
        return prepared[a].input.natural.centerY() < prepared[b].input.natural.centerY();
    });
    std::stable_sort(byY.begin(), byY.end(), [&](std::size_t a, std::size_t b) {
        const double ac = prepared[a].input.natural.centerY();
        const double bc = prepared[b].input.natural.centerY();
        if (std::abs(ac - bc) > 0.5)
            return ac < bc;
        return prepared[a].input.natural.centerX() < prepared[b].input.natural.centerX();
    });

    const double denom = std::max(1.0, static_cast<double>(overlapping.size() - 1));
    const double spanX = area.width * 0.34;
    const double spanY = area.height * 0.30;

    for (std::size_t rank = 0; rank < byX.size(); ++rank) {
        auto& offset = offsets[byX[rank]];
        offset.active = true;
        offset.x = (static_cast<double>(rank) / denom - 0.5) * spanX;
    }

    for (std::size_t rank = 0; rank < byY.size(); ++rank) {
        auto& offset = offsets[byY[rank]];
        offset.active = true;
        offset.y = (static_cast<double>(rank) / denom - 0.5) * spanY;
    }

    return offsets;
}

double maxOverlap(const std::vector<NaturalItem>& items, const LayoutConfig& config) {
    const double gapX = std::max(0.0, config.columnSpacing * 0.25);
    const double gapY = std::max(0.0, config.rowSpacing * 0.25);
    double       worst = 0.0;

    for (std::size_t i = 0; i < items.size(); ++i) {
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            const Rect& a = items[i].cell;
            const Rect& b = items[j].cell;
            const double ox = overlapAlong(a.x, a.x + a.width, b.x, b.x + b.width, gapX);
            const double oy = overlapAlong(a.y, a.y + a.height, b.y, b.y + b.height, gapY);
            if (ox > 0.0 && oy > 0.0)
                worst = std::max(worst, std::min(ox, oy));
        }
    }

    return worst;
}

std::vector<NaturalItem> buildNaturalItems(const std::vector<PreparedWindow>& prepared, const Rect& area, double baseScale, const LayoutConfig& config) {
    std::vector<NaturalItem> items;
    items.reserve(prepared.size());

    const double     density = std::clamp((static_cast<double>(prepared.size()) - 6.0) / 24.0, 0.0, 1.0);
    const double     anchorSpread = lerp(0.62, 0.72, density);
    const auto       anchorMap = buildNaturalAnchorMap(prepared, area);
    const auto       overlapOffsets = buildNaturalOverlapOffsets(prepared, area);
    const double     areaCenterX = area.centerX();
    const double     areaCenterY = area.centerY();

    for (std::size_t order = 0; order < prepared.size(); ++order) {
        const auto&  window = prepared[order];
        const double scale = std::clamp(baseScale * window.weightScale, 0.0, std::min(normalizedMaxPreviewScale(config), naturalFitScaleForWindow(window, area)));
        const auto [previewWidth, previewHeight] = previewSizeWithShortEdgeFloor(window, scale, area, config);
        const double cellWidth = std::max({1.0, window.layoutWidth * scale, previewWidth});
        const double cellHeight = std::max({1.0, window.layoutHeight * scale, previewHeight});

        double anchorX = areaCenterX + (window.input.natural.centerX() - anchorMap.sourceCenterX) * anchorSpread * anchorMap.scale;
        double anchorY = areaCenterY + (window.input.natural.centerY() - anchorMap.sourceCenterY) * anchorSpread * anchorMap.scale;
        if (overlapOffsets[order].active) {
            anchorX += overlapOffsets[order].x;
            anchorY += overlapOffsets[order].y;
        }
        anchorX = std::clamp(anchorX, area.x + cellWidth / 2.0, area.x + area.width - cellWidth / 2.0);
        anchorY = std::clamp(anchorY, area.y + cellHeight / 2.0, area.y + area.height - cellHeight / 2.0);

        Rect cell{
            anchorX - cellWidth / 2.0,
            anchorY - cellHeight / 2.0,
            cellWidth,
            cellHeight,
        };
        clampRectToArea(cell, area);

        Rect target{
            cell.centerX() - previewWidth / 2.0,
            cell.centerY() - previewHeight / 2.0,
            previewWidth,
            previewHeight,
        };

        items.push_back({
            .window = window,
            .anchorX = anchorX,
            .anchorY = anchorY,
            .cell = cell,
            .target = target,
            .scale = scale,
        });
    }

    return items;
}

void updateNaturalTargets(std::vector<NaturalItem>& items) {
    for (auto& item : items) {
        item.target = {
            std::floor(item.cell.centerX() - item.target.width / 2.0),
            std::floor(item.cell.centerY() - item.target.height / 2.0),
            item.target.width,
            item.target.height,
        };
    }
}

bool solveNaturalItems(std::vector<NaturalItem>& items, const Rect& area, const LayoutConfig& config) {
    if (items.empty())
        return true;

    const double gapX = std::max(0.0, config.columnSpacing * 0.25);
    const double gapY = std::max(0.0, config.rowSpacing * 0.25);

    for (int iteration = 0; iteration < 160; ++iteration) {
        double maxMove = 0.0;

        for (std::size_t i = 0; i < items.size(); ++i) {
            for (std::size_t j = i + 1; j < items.size(); ++j) {
                Rect& a = items[i].cell;
                Rect& b = items[j].cell;

                const double ox = overlapAlong(a.x, a.x + a.width, b.x, b.x + b.width, gapX);
                const double oy = overlapAlong(a.y, a.y + a.height, b.y, b.y + b.height, gapY);
                if (ox <= 0.0 || oy <= 0.0)
                    continue;

                double directionX = items[j].anchorX - items[i].anchorX;
                double directionY = items[j].anchorY - items[i].anchorY;
                if (std::abs(directionX) < 0.001 && std::abs(directionY) < 0.001) {
                    directionX = static_cast<double>(j) - static_cast<double>(i);
                    directionY = 0.0;
                }

                double moveX = 0.0;
                double moveY = 0.0;
                if (ox < oy) {
                    const double dir = directionX < 0.0 ? -1.0 : 1.0;
                    moveX = dir * (ox / 2.0 + 0.5);
                } else {
                    const double dir = directionY < 0.0 ? -1.0 : 1.0;
                    moveY = dir * (oy / 2.0 + 0.5);
                }

                a.x -= moveX;
                a.y -= moveY;
                b.x += moveX;
                b.y += moveY;
                maxMove = std::max(maxMove, std::hypot(moveX, moveY));
            }
        }

        for (auto& item : items) {
            const double spring = iteration < 80 ? 0.010 : 0.004;
            const double dx = (item.anchorX - item.cell.centerX()) * spring;
            const double dy = (item.anchorY - item.cell.centerY()) * spring;
            item.cell.x += dx;
            item.cell.y += dy;
            maxMove = std::max(maxMove, std::hypot(dx, dy));

            const double beforeX = item.cell.x;
            const double beforeY = item.cell.y;
            clampRectToArea(item.cell, area);
            maxMove = std::max(maxMove, std::hypot(item.cell.x - beforeX, item.cell.y - beforeY));
        }

        if (maxMove < 0.05 && maxOverlap(items, config) < 0.5)
            break;
    }

    updateNaturalTargets(items);
    return maxOverlap(items, config) < 1.0;
}

void centerNaturalTargets(std::vector<WindowSlot>& slots, const Rect& area) {
    if (slots.empty())
        return;

    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double weightedX = 0.0;
    double weightedY = 0.0;
    double totalArea = 0.0;

    for (const auto& slot : slots) {
        minX = std::min(minX, slot.target.x);
        minY = std::min(minY, slot.target.y);
        maxX = std::max(maxX, slot.target.x + slot.target.width);
        maxY = std::max(maxY, slot.target.y + slot.target.height);
        const double area = slot.target.width * slot.target.height;
        weightedX += slot.target.centerX() * area;
        weightedY += slot.target.centerY() * area;
        totalArea += area;
    }

    const double boundsWidth = maxX - minX;
    const double boundsHeight = maxY - minY;

    const double boundsCenterX = (minX + maxX) / 2.0;
    const double boundsCenterY = (minY + maxY) / 2.0;
    const double visualCenterX = totalArea > 0.0 ? weightedX / totalArea : boundsCenterX;
    const double visualCenterY = totalArea > 0.0 ? weightedY / totalArea : boundsCenterY;
    const double desiredDx = area.centerX() - (visualCenterX * 0.65 + boundsCenterX * 0.35);
    const double desiredDy = area.centerY() - (visualCenterY * 0.65 + boundsCenterY * 0.35);
    const double minDx = area.x - minX;
    const double maxDx = area.x + area.width - maxX;
    const double minDy = area.y - minY;
    const double maxDy = area.y + area.height - maxY;
    const double dx = boundsWidth < area.width ? std::clamp(desiredDx, minDx, maxDx) : 0.0;
    const double dy = boundsHeight < area.height ? std::clamp(desiredDy, minDy, maxDy) : 0.0;

    for (auto& slot : slots) {
        slot.target.x = std::floor(slot.target.x + dx);
        slot.target.y = std::floor(slot.target.y + dy);
        clampRectToArea(slot.target, area);
    }
}

std::vector<WindowSlot> materializeNaturalSlots(std::vector<NaturalItem> items, const Rect& area) {
    std::vector<WindowSlot> slots;
    slots.reserve(items.size());

    for (const auto& item : items) {
        slots.push_back({
            .index = item.window.input.index,
            .natural = item.window.input.natural,
            .target = item.target,
            .scale = item.scale,
        });
    }

    std::sort(slots.begin(), slots.end(), [](const WindowSlot& a, const WindowSlot& b) {
        return a.index < b.index;
    });

    centerNaturalTargets(slots, area);
    return slots;
}

std::optional<std::vector<WindowSlot>> computeNaturalLayout(const std::vector<PreparedWindow>& prepared, const Rect& area, const LayoutConfig& config) {
    if (prepared.empty())
        return std::vector<WindowSlot>{};

    double maxWeight = 1.0;
    double fitLimitedBaseScale = normalizedMaxPreviewScale(config);
    for (const auto& window : prepared) {
        maxWeight = std::max(maxWeight, window.weightScale);
        fitLimitedBaseScale = std::min(fitLimitedBaseScale, naturalFitScaleForWindow(window, area) / std::max(0.000001, window.weightScale));
    }

    double lo = 0.0;
    double hi = std::min(normalizedMaxPreviewScale(config) / maxWeight, fitLimitedBaseScale);
    std::optional<std::vector<NaturalItem>> best;

    for (int step = 0; step < 24; ++step) {
        const double baseScale = (lo + hi) / 2.0;
        auto         items = buildNaturalItems(prepared, area, baseScale, config);
        if (solveNaturalItems(items, area, config)) {
            lo = baseScale;
            best = std::move(items);
        } else {
            hi = baseScale;
        }
    }

    if (!best && normalizedMinSlotScale(config) <= 0.0) {
        auto items = buildNaturalItems(prepared, area, 0.0, config);
        if (solveNaturalItems(items, area, config))
            best = std::move(items);
    }

    if (!best)
        return std::nullopt;

    return materializeNaturalSlots(std::move(*best), area);
}

std::vector<WindowSlot> computeGridLayout(const std::vector<PreparedWindow>& prepared, const Rect& inner, const LayoutConfig& config) {
    if (config.forceRowGroups)
        return materializeSlots(buildRowCandidate(prepared, prepared.size(), inner, config), inner, config);

    std::optional<LayoutCandidate> best;
    for (std::size_t numRows = 1; numRows <= prepared.size(); ++numRows) {
        auto candidate = buildRowCandidate(prepared, numRows, inner, config);
        if (candidate.rows.empty())
            continue;

        if (!best || candidate.score >= best->score)
            best = std::move(candidate);
    }

    if (!best)
        return {};

    return materializeSlots(std::move(*best), inner, config);
}

std::optional<std::vector<WindowSlot>> computeNaturalRowGroupLayout(const std::vector<PreparedWindow>& prepared, const Rect& area, const LayoutConfig& config) {
    if (prepared.empty())
        return std::vector<WindowSlot>{};

    std::vector<PreparedWindow> sorted = prepared;
    std::stable_sort(sorted.begin(), sorted.end(), [&](const PreparedWindow& a, const PreparedWindow& b) {
        if (a.input.rowGroup != b.input.rowGroup)
            return a.input.rowGroup < b.input.rowGroup;
        if (config.preserveInputOrder)
            return false;
        if (std::abs(a.input.natural.centerY() - b.input.natural.centerY()) > 0.5)
            return a.input.natural.centerY() < b.input.natural.centerY();
        return a.input.natural.centerX() < b.input.natural.centerX();
    });

    std::vector<std::vector<PreparedWindow>> groups;
    for (const auto& window : sorted) {
        if (groups.empty() || groups.back().front().input.rowGroup != window.input.rowGroup)
            groups.emplace_back();
        groups.back().push_back(window);
    }

    if (groups.empty())
        return std::vector<WindowSlot>{};

    LayoutConfig groupConfig = config;
    groupConfig.forceRowGroups = false;

    const double spacing = groups.size() > 1 ? std::max(0.0, config.rowSpacing) : 0.0;
    const double totalSpacing = spacing * static_cast<double>(groups.size() - 1);
    const double bandHeight = std::max(1.0, (area.height - totalSpacing) / static_cast<double>(groups.size()));

    std::vector<WindowSlot> slots;
    double                  y = area.y;
    for (const auto& group : groups) {
        const Rect band{
            area.x,
            y,
            area.width,
            bandHeight,
        };

        std::vector<WindowSlot> groupSlots;
        if (auto natural = computeNaturalLayout(group, band, groupConfig))
            groupSlots = std::move(*natural);

        if (groupSlots.empty())
            groupSlots = computeGridLayout(group, band, groupConfig);

        slots.insert(slots.end(), groupSlots.begin(), groupSlots.end());
        y += bandHeight + spacing;
    }

    if (slots.size() != prepared.size())
        return std::nullopt;

    std::sort(slots.begin(), slots.end(), [](const WindowSlot& a, const WindowSlot& b) {
        return a.index < b.index;
    });
    return slots;
}

} // namespace

std::vector<WindowSlot> MissionControlLayout::compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& config) const {
    if (windows.empty())
        return {};

    const Rect inner = insetArea(area, config);
    const auto prepared = prepareWindows(windows, inner, config);

    if (config.engine == LayoutEngine::Natural) {
        if (config.forceRowGroups) {
            if (auto groupedNatural = computeNaturalRowGroupLayout(prepared, inner, config))
                return *groupedNatural;
        } else {
            if (auto natural = computeNaturalLayout(prepared, inner, config))
                return *natural;
        }
    }

    return computeGridLayout(prepared, inner, config);
}

} // namespace hymission
