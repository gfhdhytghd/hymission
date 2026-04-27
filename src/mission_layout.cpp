#include "mission_layout.hpp"

#include <algorithm>
#include <array>
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
    double sourceWidth = 1.0;
    double sourceHeight = 1.0;
    double scale = 1.0;
};

struct NaturalOverlapOffset {
    bool   active = false;
    double x = 0.0;
    double y = 0.0;
};

struct NaturalBandAnchor {
    bool   active = false;
    double x = 0.0;
    double y = 0.0;
};

struct NaturalProfile {
    double visualBias = 0.0;
};

void clampRectToArea(Rect& rect, const Rect& area);
std::vector<WindowSlot> computeGridLayout(const std::vector<PreparedWindow>& prepared, const Rect& inner, const LayoutConfig& config);

double clampPositive(double value) {
    return std::max(0.0, value);
}

double pointToRectDistance(double x, double y, const Rect& rect) {
    const double dx = std::max({rect.x - x, 0.0, x - (rect.x + rect.width)});
    const double dy = std::max({rect.y - y, 0.0, y - (rect.y + rect.height)});
    return std::hypot(dx, dy);
}

double rectDistance(const Rect& lhs, const Rect& rhs) {
    const double dx = std::max({rhs.x - (lhs.x + lhs.width), lhs.x - (rhs.x + rhs.width), 0.0});
    const double dy = std::max({rhs.y - (lhs.y + lhs.height), lhs.y - (rhs.y + rhs.height), 0.0});
    return std::hypot(dx, dy);
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

double slotFitScale(const Rect& natural, const Rect& area, const LayoutConfig& config) {
    const double widthFit = area.width / std::max(1.0, natural.width);
    const double heightFit = area.height / std::max(1.0, natural.height);
    return std::min(normalizedMaxPreviewScale(config), std::max(0.0, std::min(widthFit, heightFit)));
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

Rect insetNaturalEdgeArea(const Rect& area, const LayoutConfig& config) {
    const double horizontal = std::min(area.width * 0.08, std::max(0.0, config.columnSpacing * 0.5));
    const double vertical = std::min(area.height * 0.08, std::max(0.0, config.rowSpacing * 0.5));
    return {
        area.x + horizontal,
        area.y + vertical,
        std::max(1.0, area.width - horizontal * 2.0),
        std::max(1.0, area.height - vertical * 2.0),
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

std::vector<PreparedWindow> applyNaturalScaleFlex(std::vector<PreparedWindow> prepared, const Rect& area, const LayoutConfig& config) {
    if (prepared.size() < 2)
        return prepared;

    if (config.rankScaleByInputOrder)
        return prepared;

    const double countScale = 0.35;
    const double flex = std::clamp(config.naturalScaleFlex, 0.0, 0.25) * countScale;
    if (flex <= 0.0)
        return prepared;

    std::vector<double> bias(prepared.size(), 0.0);
    std::vector<double> density(prepared.size(), 0.0);
    const double        normX = std::max(1.0, area.width);
    const double        normY = std::max(1.0, area.height);
    for (std::size_t i = 0; i < prepared.size(); ++i) {
        for (std::size_t j = 0; j < prepared.size(); ++j) {
            if (i == j)
                continue;

            const double dx = (prepared[i].input.natural.centerX() - prepared[j].input.natural.centerX()) / normX;
            const double dy = (prepared[i].input.natural.centerY() - prepared[j].input.natural.centerY()) / normY;
            const double distanceSq = dx * dx + dy * dy;
            density[i] += std::exp(-distanceSq / 0.10);
        }
    }

    const auto [minIt, maxIt] = std::minmax_element(density.begin(), density.end());
    const double range = *maxIt - *minIt;
    if (range > 0.000001) {
        for (std::size_t i = 0; i < prepared.size(); ++i) {
            bias[i] = std::clamp(((*maxIt + *minIt) * 0.5 - density[i]) / (range * 0.5), -1.0, 1.0);
        }
    }

    for (std::size_t i = 0; i < prepared.size(); ++i) {
        const double multiplier = std::clamp(1.0 + bias[i] * flex, 1.0 - flex, 1.0 + flex);
        prepared[i].weightScale *= multiplier;
        prepared[i].scaledWidth = prepared[i].layoutWidth * prepared[i].weightScale;
        prepared[i].scaledHeight = prepared[i].layoutHeight * prepared[i].weightScale;
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
        return {.sourceCenterX = area.centerX(), .sourceCenterY = area.centerY(), .sourceWidth = 1.0, .sourceHeight = 1.0, .scale = 1.0};

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
        .sourceWidth = sourceWidth,
        .sourceHeight = sourceHeight,
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

std::vector<NaturalBandAnchor> buildNaturalBandAnchors(const std::vector<PreparedWindow>& prepared, const Rect& area, const NaturalProfile& profile) {
    std::vector<NaturalBandAnchor> anchors(prepared.size());
    const std::size_t              count = prepared.size();
    const double                   visualBias = std::clamp(profile.visualBias, 0.0, 1.0);
    if (count < (visualBias > 0.001 ? 4 : 7))
        return anchors;

    const double aspect = area.width / std::max(1.0, area.height);
    const auto   rows = static_cast<std::size_t>(std::clamp(std::round(std::sqrt(static_cast<double>(count) / std::max(1.0, aspect))), 2.0, 4.0));
    const auto   columns = static_cast<std::size_t>(std::ceil(static_cast<double>(count) / static_cast<double>(rows)));
    if (rows < 2 || columns == 0)
        return anchors;

    std::vector<std::size_t> ordered(count);
    for (std::size_t i = 0; i < count; ++i)
        ordered[i] = i;

    if (visualBias < 0.85) {
        std::stable_sort(ordered.begin(), ordered.end(), [&](std::size_t a, std::size_t b) {
            const double ay = prepared[a].input.natural.centerY();
            const double by = prepared[b].input.natural.centerY();
            if (std::abs(ay - by) > 0.5)
                return ay < by;
            return prepared[a].input.natural.centerX() < prepared[b].input.natural.centerX();
        });
    }

    const double legacyUsedWidth = 0.76;
    const double visualUsedWidth = std::clamp(0.74 + static_cast<double>(count) * 0.004, 0.76, 0.84);
    const double legacyUsedHeight = std::clamp(0.58 + static_cast<double>(count) * 0.006, 0.58, 0.74);
    const double visualUsedHeight = std::clamp(0.50 + static_cast<double>(count) * 0.014, 0.56, 0.76);
    const double usedWidth = area.width * lerp(legacyUsedWidth, visualUsedWidth, visualBias);
    const double usedHeight = area.height * lerp(legacyUsedHeight, visualUsedHeight, visualBias);
    const double left = area.centerX() - usedWidth / 2.0;
    const double top = area.centerY() - usedHeight / 2.0;

    for (std::size_t row = 0; row < rows; ++row) {
        const std::size_t begin = row * columns;
        const std::size_t end = std::min(count, begin + columns);
        if (begin >= end)
            break;

        std::vector<std::size_t> band(ordered.begin() + static_cast<std::ptrdiff_t>(begin), ordered.begin() + static_cast<std::ptrdiff_t>(end));
        if (visualBias < 0.85) {
            std::stable_sort(band.begin(), band.end(), [&](std::size_t a, std::size_t b) {
                const double ax = prepared[a].input.natural.centerX();
                const double bx = prepared[b].input.natural.centerX();
                if (std::abs(ax - bx) > 0.5)
                    return ax < bx;
                return prepared[a].input.natural.centerY() < prepared[b].input.natural.centerY();
            });
        }

        const double y = rows == 1 ? area.centerY() : top + usedHeight * (static_cast<double>(row) + 0.5) / static_cast<double>(rows);
        for (std::size_t column = 0; column < band.size(); ++column) {
            const double x = left + usedWidth * (static_cast<double>(column) + 0.5) / static_cast<double>(band.size());
            anchors[band[column]] = {
                .active = true,
                .x = x,
                .y = y,
            };
        }
    }

    return anchors;
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

std::vector<NaturalItem> buildNaturalItems(const std::vector<PreparedWindow>& prepared,
                                           const Rect&                        area,
                                           double                             baseScale,
                                           const LayoutConfig&                config,
                                           const NaturalProfile&              profile) {
    std::vector<NaturalItem> items;
    items.reserve(prepared.size());

    const double     visualBias = std::clamp(profile.visualBias, 0.0, 1.0);
    const double     density = std::clamp((static_cast<double>(prepared.size()) - 6.0) / 24.0, 0.0, 1.0);
    const double     sourceAnchorSpread = lerp(0.62, 0.72, density);
    const double     visualAnchorSpread = lerp(0.34, 0.46, density);
    const double     anchorSpread = lerp(sourceAnchorSpread, visualAnchorSpread, visualBias);
    const auto       anchorMap = buildNaturalAnchorMap(prepared, area);
    const double     sourceAspect = anchorMap.sourceHeight / std::max(1.0, anchorMap.sourceWidth);
    const double     areaAspect = area.height / std::max(1.0, area.width);
    const double     flatness = std::clamp((areaAspect * 0.70 - sourceAspect) / std::max(0.001, areaAspect * 0.35), 0.0, 1.0);
    const double     sourceCountBlend = std::clamp((static_cast<double>(prepared.size()) - 5.0) / 3.0, 0.0, 1.0);
    const double     sourceBandBlend = flatness * sourceCountBlend * lerp(0.82, 0.92, density);
    const double     visualCountBlend = std::clamp((static_cast<double>(prepared.size()) - 3.0) / 5.0, 0.0, 1.0);
    const double     visualUniformBlend = visualCountBlend * lerp(0.46, 0.62, density);
    const double     visualFlatBlend = flatness * visualCountBlend * lerp(0.36, 0.44, density);
    const double     bandBlend = std::clamp(lerp(sourceBandBlend, visualUniformBlend + visualFlatBlend, visualBias), 0.0, 0.94);
    const auto       overlapOffsets = buildNaturalOverlapOffsets(prepared, area);
    const auto       bandAnchors = buildNaturalBandAnchors(prepared, area, profile);
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
        if (bandAnchors[order].active) {
            anchorX = lerp(anchorX, bandAnchors[order].x, bandBlend);
            anchorY = lerp(anchorY, bandAnchors[order].y, bandBlend);
        }
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

    for (const auto& slot : slots) {
        minX = std::min(minX, slot.target.x);
        minY = std::min(minY, slot.target.y);
        maxX = std::max(maxX, slot.target.x + slot.target.width);
        maxY = std::max(maxY, slot.target.y + slot.target.height);
    }

    const double boundsWidth = maxX - minX;
    const double boundsHeight = maxY - minY;

    const double boundsCenterX = (minX + maxX) / 2.0;
    const double boundsCenterY = (minY + maxY) / 2.0;
    const double desiredDx = area.centerX() - boundsCenterX;
    const double desiredDy = area.centerY() - boundsCenterY;
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

void spreadNaturalAxis(std::vector<WindowSlot>& slots, const Rect& area, bool horizontal, double desiredFill, double maxSpreadScale) {
    if (slots.size() < 3)
        return;

    const double areaStart = horizontal ? area.x : area.y;
    const double areaLength = horizontal ? area.width : area.height;
    const double areaEnd = areaStart + areaLength;

    double minEdge = std::numeric_limits<double>::infinity();
    double maxEdge = -std::numeric_limits<double>::infinity();
    double weightedCenter = 0.0;
    double totalArea = 0.0;

    for (const auto& slot : slots) {
        const double start = horizontal ? slot.target.x : slot.target.y;
        const double length = horizontal ? slot.target.width : slot.target.height;
        const double center = start + length * 0.5;
        const double slotArea = slot.target.width * slot.target.height;
        minEdge = std::min(minEdge, start);
        maxEdge = std::max(maxEdge, start + length);
        weightedCenter += center * slotArea;
        totalArea += slotArea;
    }

    const double boundsLength = maxEdge - minEdge;
    const double desiredLength = areaLength * std::clamp(desiredFill, 0.0, 1.0);
    if (boundsLength >= desiredLength || boundsLength <= 1.0)
        return;

    const double origin = totalArea > 0.0 ? weightedCenter / totalArea : (minEdge + maxEdge) * 0.5;
    double       scale = std::min(maxSpreadScale, desiredLength / boundsLength);

    for (const auto& slot : slots) {
        const double start = horizontal ? slot.target.x : slot.target.y;
        const double length = horizontal ? slot.target.width : slot.target.height;
        const double half = length * 0.5;
        const double center = start + half;
        const double delta = center - origin;
        if (std::abs(delta) <= 0.001)
            continue;

        double axisLimit = scale;
        if (delta > 0.0)
            axisLimit = (areaEnd - half - origin) / delta;
        else
            axisLimit = (areaStart + half - origin) / delta;
        if (std::isfinite(axisLimit))
            scale = std::min(scale, axisLimit);
    }

    if (scale <= 1.001)
        return;

    for (auto& slot : slots) {
        const double length = horizontal ? slot.target.width : slot.target.height;
        const double center = (horizontal ? slot.target.x : slot.target.y) + length * 0.5;
        const double targetCenter = origin + (center - origin) * scale;
        const double start = std::floor(targetCenter - length * 0.5);
        if (horizontal)
            slot.target.x = start;
        else
            slot.target.y = start;
        clampRectToArea(slot.target, area);
    }
}

void spreadNaturalTargets(std::vector<WindowSlot>& slots, const Rect& area) {
    const double count = static_cast<double>(slots.size());
    const double verticalFill = std::clamp(0.78 + count * 0.010, 0.80, 0.94);
    const double horizontalFill = std::clamp(0.78 + count * 0.008, 0.80, 0.94);
    spreadNaturalAxis(slots, area, true, horizontalFill, 2.0);
    spreadNaturalAxis(slots, area, false, verticalFill, 2.0);
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

    spreadNaturalTargets(slots, area);
    centerNaturalTargets(slots, area);
    return slots;
}

void shrinkSlotScale(WindowSlot& slot, double scale, const Rect& area, const LayoutConfig& config) {
    if (slot.scale <= 0.0 || scale >= slot.scale)
        return;

    const double naturalShortEdge = std::min(slot.natural.width, slot.natural.height);
    if (naturalShortEdge > 0.0)
        scale = std::max(scale, normalizedMinPreviewShortEdge(config, area) / naturalShortEdge);
    scale = std::max(scale, normalizedMinSlotScale(config));
    if (scale >= slot.scale)
        return;

    const double ratio = std::max(0.0, scale) / slot.scale;
    const double centerX = slot.target.centerX();
    const double centerY = slot.target.centerY();
    slot.target = {
        std::floor(centerX - slot.target.width * ratio / 2.0),
        std::floor(centerY - slot.target.height * ratio / 2.0),
        slot.target.width * ratio,
        slot.target.height * ratio,
    };
    slot.scale = scale;
    clampRectToArea(slot.target, area);
}

void enforceInputOrderScaleGradient(std::vector<WindowSlot>& slots,
                                    const std::vector<PreparedWindow>& prepared,
                                    const Rect&                        area,
                                    const LayoutConfig&                config) {
    if (!config.rankScaleByInputOrder || slots.size() < 2)
        return;

    const double flex = std::clamp(config.naturalScaleFlex, 0.0, 0.25);
    if (flex <= 0.0)
        return;

    std::vector<WindowSlot*> ordered;
    ordered.reserve(prepared.size());
    for (const auto& window : prepared) {
        const auto it = std::find_if(slots.begin(), slots.end(), [&](const WindowSlot& slot) {
            return slot.index == window.input.index;
        });
        if (it != slots.end())
            ordered.push_back(&*it);
    }

    if (ordered.size() < 2)
        return;

    const double denom = std::max(1.0, static_cast<double>(ordered.size() - 1));
    const double rankFlex = flex * 0.55;
    double previousScale = std::numeric_limits<double>::infinity();
    for (std::size_t order = 0; order < ordered.size(); ++order) {
        auto& slot = *ordered[order];
        double targetScale = slot.scale * (1.0 - rankFlex * static_cast<double>(order) / denom);
        if (std::isfinite(previousScale))
            targetScale = std::min(targetScale, previousScale * 0.995);
        shrinkSlotScale(slot, targetScale, area, config);
        previousScale = slot.scale;
    }

    centerNaturalTargets(slots, area);
}

std::array<std::pair<double, double>, 4> areaCorners(const Rect& area) {
    return {{
        {area.x, area.y},
        {area.x + area.width, area.y},
        {area.x, area.y + area.height},
        {area.x + area.width, area.y + area.height},
    }};
}

std::array<double, 4> nearestCornerDistances(const std::vector<WindowSlot>& slots, const Rect& area) {
    const auto corners = areaCorners(area);
    std::array<double, 4> distances{};

    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto& slot : slots)
            nearest = std::min(nearest, pointToRectDistance(corners[corner].first, corners[corner].second, slot.target));
        distances[corner] = std::isfinite(nearest) ? nearest : 0.0;
    }

    return distances;
}

double averageEdgeMargin(const std::vector<WindowSlot>& slots, const Rect& area) {
    if (slots.empty())
        return 0.0;

    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (const auto& slot : slots) {
        minX = std::min(minX, slot.target.x);
        minY = std::min(minY, slot.target.y);
        maxX = std::max(maxX, slot.target.x + slot.target.width);
        maxY = std::max(maxY, slot.target.y + slot.target.height);
    }

    return std::max(1.0, ((minX - area.x) + (area.x + area.width - maxX) + (minY - area.y) + (area.y + area.height - maxY)) / 4.0);
}

double edgeBalancePixels(const std::vector<WindowSlot>& slots, const Rect& area) {
    if (slots.empty())
        return 0.0;

    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (const auto& slot : slots) {
        minX = std::min(minX, slot.target.x);
        minY = std::min(minY, slot.target.y);
        maxX = std::max(maxX, slot.target.x + slot.target.width);
        maxY = std::max(maxY, slot.target.y + slot.target.height);
    }

    const double edgeLeft = minX - area.x;
    const double edgeRight = area.x + area.width - maxX;
    const double edgeTop = minY - area.y;
    const double edgeBottom = area.y + area.height - maxY;
    return std::abs(edgeLeft - edgeRight) + std::abs(edgeTop - edgeBottom);
}

std::pair<double, double> nearestGapMetrics(const std::vector<WindowSlot>& slots) {
    if (slots.size() < 2)
        return {0.0, 0.0};

    double totalNearest = 0.0;
    double maxNearest = 0.0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        double nearest = std::numeric_limits<double>::infinity();
        for (std::size_t j = 0; j < slots.size(); ++j) {
            if (i == j)
                continue;
            nearest = std::min(nearest, rectDistance(slots[i].target, slots[j].target));
        }
        if (std::isfinite(nearest)) {
            totalNearest += nearest;
            maxNearest = std::max(maxNearest, nearest);
        }
    }

    return {totalNearest / static_cast<double>(slots.size()), maxNearest};
}

double cornerVoidCost(const std::vector<WindowSlot>& slots, const Rect& area) {
    if (slots.empty())
        return 0.0;

    const auto distances = nearestCornerDistances(slots, area);
    const auto [minIt, maxIt] = std::minmax_element(distances.begin(), distances.end());
    const auto [averageGap, maxGap] = nearestGapMetrics(slots);
    const double averageEdge = averageEdgeMargin(slots, area);
    const double overflow = std::max(0.0, *maxIt - averageEdge * 5.0);
    const double balance = *maxIt - *minIt;
    return overflow * 18.0 + balance * 1.0 + *maxIt * 0.10 + edgeBalancePixels(slots, area) * 2.8 + averageGap * 1.6 + maxGap * 0.6;
}

bool canPlaceSlotTarget(const std::vector<WindowSlot>& slots, std::size_t movingIndex, const Rect& target, const Rect& area, const LayoutConfig& config) {
    if (target.x < area.x - 0.5 || target.y < area.y - 0.5 || target.x + target.width > area.x + area.width + 0.5 ||
        target.y + target.height > area.y + area.height + 0.5)
        return false;

    const double gapX = std::max(0.0, config.columnSpacing * 0.25);
    const double gapY = std::max(0.0, config.rowSpacing * 0.25);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (i == movingIndex)
            continue;

        const auto& other = slots[i].target;
        if (overlapAlong(target.x, target.x + target.width, other.x, other.x + other.width, gapX) > 0.0 &&
            overlapAlong(target.y, target.y + target.height, other.y, other.y + other.height, gapY) > 0.0)
            return false;
    }

    return true;
}

void relieveCornerVoids(std::vector<WindowSlot>& slots, const Rect& area, const LayoutConfig& config) {
    if (slots.size() < 2)
        return;

    const auto corners = areaCorners(area);
    for (int pass = 0; pass < 9; ++pass) {
        const auto distances = nearestCornerDistances(slots, area);
        const double averageEdge = averageEdgeMargin(slots, area);
        const double limit = averageEdge * 5.0;
        double bestCost = cornerVoidCost(slots, area);
        std::vector<WindowSlot> bestSlots;

        for (std::size_t corner = 0; corner < corners.size(); ++corner) {
            if (distances[corner] <= limit)
                continue;

            std::vector<std::size_t> nearestIndexes;
            nearestIndexes.reserve(slots.size());
            for (std::size_t i = 0; i < slots.size(); ++i) {
                nearestIndexes.push_back(i);
            }
            std::sort(nearestIndexes.begin(), nearestIndexes.end(), [&](std::size_t lhs, std::size_t rhs) {
                const double lhsDistance = pointToRectDistance(corners[corner].first, corners[corner].second, slots[lhs].target);
                const double rhsDistance = pointToRectDistance(corners[corner].first, corners[corner].second, slots[rhs].target);
                if (std::abs(lhsDistance - rhsDistance) > 0.5)
                    return lhsDistance < rhsDistance;
                return lhs < rhs;
            });

            const std::size_t attempts = std::min<std::size_t>(6, nearestIndexes.size());
            for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
                const std::size_t nearestIndex = nearestIndexes[attempt];
                const auto& slot = slots[nearestIndex];
                const bool pullLeft = corners[corner].first <= area.centerX();
                const bool pullTop = corners[corner].second <= area.centerY();
                const double maxX = pullLeft ? slot.target.x - area.x : area.x + area.width - (slot.target.x + slot.target.width);
                const double maxY = pullTop ? slot.target.y - area.y : area.y + area.height - (slot.target.y + slot.target.height);
                const double maxNudge = std::max(24.0, std::min(area.width, area.height) * 0.30);
                const double fullX = std::min(std::max(0.0, maxX), maxNudge);
                const double fullY = std::min(std::max(0.0, maxY), maxNudge);
                if (fullX <= 0.5 && fullY <= 0.5)
                    continue;

                for (const auto [useX, useY] : std::array<std::pair<bool, bool>, 3>{{{true, true}, {true, false}, {false, true}}}) {
                    for (const double fraction : {1.0, 0.75, 0.5, 0.25, 0.125}) {
                        Rect target = slot.target;
                        if (useX)
                            target.x += (pullLeft ? -1.0 : 1.0) * fullX * fraction;
                        if (useY)
                            target.y += (pullTop ? -1.0 : 1.0) * fullY * fraction;
                        clampRectToArea(target, area);
                        if (!canPlaceSlotTarget(slots, nearestIndex, target, area, config))
                            continue;

                        auto candidate = slots;
                        candidate[nearestIndex].target = target;
                        const double cost = cornerVoidCost(candidate, area);
                        if (cost < bestCost - 0.5) {
                            bestCost = cost;
                            bestSlots = std::move(candidate);
                        }
                    }
                }
            }
        }

        if (bestSlots.empty())
            break;

        slots = std::move(bestSlots);
    }
}

double naturalVisualCost(const std::vector<WindowSlot>& slots, const Rect& area, const LayoutConfig& config) {
    if (slots.empty())
        return std::numeric_limits<double>::infinity();

    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double totalArea = 0.0;
    double weightedX = 0.0;
    double weightedY = 0.0;
    double averageScale = 0.0;
    double minShortEdge = std::numeric_limits<double>::infinity();

    for (const auto& slot : slots) {
        minX = std::min(minX, slot.target.x);
        minY = std::min(minY, slot.target.y);
        maxX = std::max(maxX, slot.target.x + slot.target.width);
        maxY = std::max(maxY, slot.target.y + slot.target.height);
        minShortEdge = std::min(minShortEdge, std::min(slot.target.width, slot.target.height));

        const double targetArea = slot.target.width * slot.target.height;
        totalArea += targetArea;
        weightedX += slot.target.centerX() * targetArea;
        weightedY += slot.target.centerY() * targetArea;
        averageScale += slot.scale;
    }

    averageScale /= static_cast<double>(slots.size());

    const double areaPixels = std::max(1.0, area.width * area.height);
    const double targetAreaRatio = totalArea / areaPixels;
    const double centroidX = totalArea > 0.0 ? weightedX / totalArea : area.centerX();
    const double centroidY = totalArea > 0.0 ? weightedY / totalArea : area.centerY();
    const double gravityOffset = std::hypot(centroidX - area.centerX(), centroidY - area.centerY()) / std::max(1.0, std::hypot(area.width, area.height));

    const double edgeLeft = minX - area.x;
    const double edgeRight = area.x + area.width - maxX;
    const double edgeTop = minY - area.y;
    const double edgeBottom = area.y + area.height - maxY;
    const double averageEdge = std::max(1.0, (edgeLeft + edgeRight + edgeTop + edgeBottom) / 4.0);
    const double edgeBalance = std::abs(edgeLeft - edgeRight) / std::max(1.0, area.width) + std::abs(edgeTop - edgeBottom) / std::max(1.0, area.height);
    const double edgeWhitespace = (edgeLeft + edgeRight) / std::max(1.0, area.width) + (edgeTop + edgeBottom) / std::max(1.0, area.height);
    const double boundsFillRatio = ((maxX - minX) * (maxY - minY)) / areaPixels;
    const auto cornerDistances = nearestCornerDistances(slots, area);
    const auto [minCornerIt, maxCornerIt] = std::minmax_element(cornerDistances.begin(), cornerDistances.end());
    const double cornerBalance = (*maxCornerIt - *minCornerIt) / std::max(1.0, std::hypot(area.width, area.height));
    const double cornerWhitespace = *maxCornerIt / std::max(1.0, std::hypot(area.width, area.height));
    const double cornerEdgeOverflow = std::max(0.0, *maxCornerIt - averageEdge * 5.0) / std::max(1.0, std::hypot(area.width, area.height));
    const auto [averageNearestGap, maxNearestGap] = nearestGapMetrics(slots);
    const double gapBasis = std::max(1.0, std::min(area.width, area.height));
    const double averageGapOverflow = std::max(0.0, averageNearestGap / gapBasis - 0.075);
    const double maxGapOverflow = std::max(0.0, maxNearestGap / gapBasis - 0.16);

    constexpr int columns = 4;
    constexpr int rows = 3;
    const double  cellWidth = area.width / static_cast<double>(columns);
    const double  cellHeight = area.height / static_cast<double>(rows);
    const double  cellArea = std::max(1.0, cellWidth * cellHeight);
    std::array<double, columns * rows> heat{};

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const Rect cell{
                area.x + static_cast<double>(column) * cellWidth,
                area.y + static_cast<double>(row) * cellHeight,
                cellWidth,
                cellHeight,
            };

            double covered = 0.0;
            for (const auto& slot : slots) {
                const double width = std::min(slot.target.x + slot.target.width, cell.x + cell.width) - std::max(slot.target.x, cell.x);
                const double height = std::min(slot.target.y + slot.target.height, cell.y + cell.height) - std::max(slot.target.y, cell.y);
                covered += std::max(0.0, width) * std::max(0.0, height);
            }
            heat[static_cast<std::size_t>(row * columns + column)] = covered / cellArea;
        }
    }

    double meanHeat = 0.0;
    double leftHeat = 0.0;
    double rightHeat = 0.0;
    double topHeat = 0.0;
    double bottomHeat = 0.0;
    for (std::size_t i = 0; i < heat.size(); ++i) {
        meanHeat += heat[i];
        const int row = static_cast<int>(i / columns);
        const int column = static_cast<int>(i % columns);
        if (column < columns / 2)
            leftHeat += heat[i];
        else
            rightHeat += heat[i];
        if (row == 0)
            topHeat += heat[i];
        if (row == rows - 1)
            bottomHeat += heat[i];
    }
    meanHeat /= static_cast<double>(heat.size());

    double heatStdDev = 0.0;
    for (const auto value : heat)
        heatStdDev += (value - meanHeat) * (value - meanHeat);
    heatStdDev = std::sqrt(heatStdDev / static_cast<double>(heat.size()));

    const double heatImbalance = std::abs(leftHeat - rightHeat) + std::abs(topHeat - bottomHeat);
    const double sparsePenalty = targetAreaRatio < 0.20 ? (0.20 - targetAreaRatio) : 0.0;
    const double boundsSparsePenalty = boundsFillRatio < 0.82 ? (0.82 - boundsFillRatio) : 0.0;
    const double readableShortEdge = std::max(1.0, normalizedMinPreviewShortEdge(config, area));
    const double readabilityPenalty = minShortEdge < readableShortEdge ? (readableShortEdge - minShortEdge) / readableShortEdge : 0.0;

    return heatStdDev * 2.4 + heatImbalance * 0.42 + gravityOffset * 3.0 + edgeBalance * 2.4 + edgeWhitespace * 3.2 + cornerBalance * 1.5 +
        cornerWhitespace * 0.45 + cornerEdgeOverflow * 8.0 + averageGapOverflow * 5.0 + maxGapOverflow * 3.5 + sparsePenalty * 3.4 +
        boundsSparsePenalty * 4.0 + readabilityPenalty * 14.0 - targetAreaRatio * 1.9 - averageScale * 0.35;
}

Rect scaledSlotTarget(const WindowSlot& slot, double scale, const Rect& area, const LayoutConfig& config, double anchorX, double anchorY) {
    double width = std::max(0.0, slot.natural.width * scale);
    double height = std::max(0.0, slot.natural.height * scale);
    const double floor = normalizedMinPreviewShortEdge(config, area);
    if (floor > 0.0) {
        width = std::max(width, std::min(floor, area.width));
        height = std::max(height, std::min(floor, area.height));
    }
    width = std::max(width, slot.target.width);
    height = std::max(height, slot.target.height);
    anchorX = std::clamp(anchorX, 0.0, 1.0);
    anchorY = std::clamp(anchorY, 0.0, 1.0);
    const double fixedX = slot.target.x + slot.target.width * anchorX;
    const double fixedY = slot.target.y + slot.target.height * anchorY;

    Rect target{
        std::floor(fixedX - width * anchorX),
        std::floor(fixedY - height * anchorY),
        width,
        height,
    };
    clampRectToArea(target, area);
    return target;
}

std::vector<Rect> scaledSlotTargets(const WindowSlot& slot, double scale, const Rect& area, const LayoutConfig& config) {
    const double outwardX = slot.target.centerX() < area.centerX() ? 1.0 : 0.0;
    const double outwardY = slot.target.centerY() < area.centerY() ? 1.0 : 0.0;
    const std::array<std::pair<double, double>, 12> anchors{{
        {0.5, 0.5},
        {outwardX, 0.5},
        {0.5, outwardY},
        {outwardX, outwardY},
        {0.0, 0.5},
        {1.0, 0.5},
        {0.5, 0.0},
        {0.5, 1.0},
        {0.0, 0.0},
        {1.0, 0.0},
        {0.0, 1.0},
        {1.0, 1.0},
    }};

    std::vector<Rect> targets;
    targets.reserve(anchors.size());
    for (const auto& [anchorX, anchorY] : anchors) {
        const Rect target = scaledSlotTarget(slot, scale, area, config, anchorX, anchorY);
        const auto duplicate = std::find_if(targets.begin(), targets.end(), [&](const Rect& existing) {
            return std::abs(existing.x - target.x) < 0.5 && std::abs(existing.y - target.y) < 0.5 && std::abs(existing.width - target.width) < 0.5 &&
                std::abs(existing.height - target.height) < 0.5;
        });
        if (duplicate == targets.end())
            targets.push_back(target);
    }
    return targets;
}

void expandSparseSlots(std::vector<WindowSlot>& slots, const std::vector<PreparedWindow>& prepared, const Rect& area, const LayoutConfig& config) {
    if (slots.size() < 2)
        return;

    std::vector<std::size_t> rankOrder;
    if (config.rankScaleByInputOrder) {
        rankOrder.reserve(prepared.size());
        for (const auto& window : prepared) {
            const auto it = std::find_if(slots.begin(), slots.end(), [&](const WindowSlot& slot) {
                return slot.index == window.input.index;
            });
            if (it != slots.end())
                rankOrder.push_back(static_cast<std::size_t>(std::distance(slots.begin(), it)));
        }
    }

    const double flex = std::clamp(config.naturalScaleFlex, 0.0, 0.25);
    const double maxExtraScale = std::min(0.38, flex * (config.rankScaleByInputOrder ? 1.45 : 1.75));
    if (maxExtraScale <= 0.001)
        return;

    const auto rankCapForSlot = [&](std::size_t slotIndex, const std::vector<WindowSlot>& currentSlots) {
        double cap = normalizedMaxPreviewScale(config);
        if (!config.rankScaleByInputOrder)
            return cap;

        const auto it = std::find(rankOrder.begin(), rankOrder.end(), slotIndex);
        if (it == rankOrder.end())
            return cap;

        const auto position = static_cast<std::size_t>(std::distance(rankOrder.begin(), it));
        if (position > 0)
            cap = std::min(cap, currentSlots[rankOrder[position - 1]].scale * 0.995);
        return cap;
    };

    for (int pass = 0; pass < 5; ++pass) {
        double bestCost = naturalVisualCost(slots, area, config);
        std::vector<WindowSlot> bestSlots;

        for (std::size_t i = 0; i < slots.size(); ++i) {
            const double fitScale = slotFitScale(slots[i].natural, area, config);
            const double maxScale = std::min(fitScale, rankCapForSlot(i, slots));
            if (maxScale <= slots[i].scale + 0.001)
                continue;

            for (const double fraction : {1.0, 0.75, 0.55, 0.35, 0.20}) {
                const double targetScale = std::min(maxScale, slots[i].scale + maxExtraScale * fraction);
                if (targetScale <= slots[i].scale + 0.001)
                    continue;

                for (const auto& target : scaledSlotTargets(slots[i], targetScale, area, config)) {
                    if (!canPlaceSlotTarget(slots, i, target, area, config))
                        continue;

                    auto candidate = slots;
                    candidate[i].target = target;
                    candidate[i].scale = targetScale;
                    const double cost = naturalVisualCost(candidate, area, config);
                    if (cost < bestCost - 0.002) {
                        bestCost = cost;
                        bestSlots = std::move(candidate);
                    }
                }
            }
        }

        if (bestSlots.empty())
            break;
        slots = std::move(bestSlots);
    }

    centerNaturalTargets(slots, area);
}

std::optional<std::vector<WindowSlot>> computeNaturalLayoutWithProfile(const std::vector<PreparedWindow>& prepared,
                                                                        const Rect&                        area,
                                                                        const LayoutConfig&                config,
                                                                        const NaturalProfile&              profile) {
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
        auto         items = buildNaturalItems(prepared, area, baseScale, config, profile);
        if (solveNaturalItems(items, area, config)) {
            lo = baseScale;
            best = std::move(items);
        } else {
            hi = baseScale;
        }
    }

    if (!best && normalizedMinSlotScale(config) <= 0.0) {
        auto items = buildNaturalItems(prepared, area, 0.0, config, profile);
        if (solveNaturalItems(items, area, config))
            best = std::move(items);
    }

    if (!best)
        return std::nullopt;

    return materializeNaturalSlots(std::move(*best), area);
}

std::optional<std::vector<WindowSlot>> computeNaturalLayout(const std::vector<PreparedWindow>& prepared, const Rect& area, const LayoutConfig& config) {
    if (prepared.empty())
        return std::vector<WindowSlot>{};

    const std::array<NaturalProfile, 3> profiles{{
        {.visualBias = 0.0},
        {.visualBias = 0.55},
        {.visualBias = 1.0},
    }};

    std::optional<std::vector<WindowSlot>> bestSlots;
    double                                 bestCost = std::numeric_limits<double>::infinity();

    const auto consider = [&](std::vector<WindowSlot> candidate, double costBias = 0.0) {
        if (candidate.empty())
            return;

        enforceInputOrderScaleGradient(candidate, prepared, area, config);
        relieveCornerVoids(candidate, area, config);
        expandSparseSlots(candidate, prepared, area, config);
        relieveCornerVoids(candidate, area, config);
        const double cost = naturalVisualCost(candidate, area, config) + costBias;
        if (!bestSlots || cost < bestCost) {
            bestCost = cost;
            bestSlots = std::move(candidate);
        }
    };

    for (const auto& profile : profiles) {
        auto candidate = computeNaturalLayoutWithProfile(prepared, area, config, profile);
        if (!candidate)
            continue;

        consider(std::move(*candidate));
    }

    if (!config.forceRowGroups && prepared.size() >= 4) {
        LayoutConfig visualPackConfig = config;
        visualPackConfig.forceRowGroups = false;
        visualPackConfig.preserveInputOrder = config.preserveInputOrder;
        visualPackConfig.layoutSpaceWeight = std::max(visualPackConfig.layoutSpaceWeight, 0.18);

        auto visualPack = computeGridLayout(prepared, area, visualPackConfig);
        centerNaturalTargets(visualPack, area);
        consider(std::move(visualPack), -0.08);
    }

    return bestSlots;
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
    auto       prepared = prepareWindows(windows, inner, config);

    if (config.engine == LayoutEngine::Natural) {
        const Rect naturalInner = insetNaturalEdgeArea(inner, config);
        prepared = applyNaturalScaleFlex(std::move(prepared), naturalInner, config);
        if (config.forceRowGroups) {
            if (auto groupedNatural = computeNaturalRowGroupLayout(prepared, naturalInner, config))
                return *groupedNatural;
        } else {
            if (auto natural = computeNaturalLayout(prepared, naturalInner, config))
                return *natural;
        }
        return computeGridLayout(prepared, naturalInner, config);
    }

    return computeGridLayout(prepared, inner, config);
}

} // namespace hymission
