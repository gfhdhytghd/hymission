#include "mission_layout.hpp"
#include "natural_layout.hpp"
#include "numeric_config.hpp"

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

void clampRectToArea(Rect& rect, const Rect& area);
std::vector<WindowSlot> computeGridLayout(const std::vector<PreparedWindow>& prepared, const Rect& inner, const LayoutConfig& config);

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
        rect.x = std::clamp(rect.x, area.x, area.x + (area.width - rect.width));
    }

    if (rect.height >= area.height) {
        rect.y = area.y + (area.height - rect.height) / 2.0;
    } else {
        rect.y = std::clamp(rect.y, area.y, area.y + (area.height - rect.height));
    }
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

std::optional<std::vector<WindowSlot>> computeThumbnailLayout(const std::vector<PreparedWindow>& prepared, const Rect& area, const LayoutConfig& config) {
    if (prepared.empty())
        return std::vector<WindowSlot>{};

    std::vector<PreparedWindow> sorted = prepared;
    std::stable_sort(sorted.begin(), sorted.end(), [](const PreparedWindow& a, const PreparedWindow& b) {
        return a.input.rowGroup < b.input.rowGroup;
    });

    std::vector<std::vector<PreparedWindow>> groups;
    for (const auto& window : sorted) {
        if (groups.empty() || groups.back().front().input.rowGroup != window.input.rowGroup)
            groups.emplace_back();
        groups.back().push_back(window);
    }

    if (groups.empty())
        return std::vector<WindowSlot>{};

    const std::size_t numCards = groups.size();
    const double spacing = std::max(0.0, config.rowSpacing);

    struct CardCell {
        std::size_t col;
        std::size_t row;
        std::size_t colSpan = 1;
        std::size_t rowSpan = 1;
    };

    std::size_t gridCols = 1;
    std::size_t gridRows = 1;
    std::vector<CardCell> cells(numCards);

    if (numCards == 1) {
        gridCols = 1; gridRows = 1;
        cells[0] = {0, 0, 1, 1};
    } else if (numCards == 2) {
        gridCols = 2; gridRows = 1;
        cells[0] = {0, 0, 1, 1};
        cells[1] = {1, 0, 1, 1};
    } else if (numCards == 3) {
        gridCols = 2; gridRows = 2;
        cells[0] = {0, 0, 2, 1};
        cells[1] = {0, 1, 1, 1};
        cells[2] = {1, 1, 1, 1};
    } else if (numCards == 4) {
        gridCols = 2; gridRows = 2;
        cells[0] = {0, 0, 1, 1};
        cells[1] = {1, 0, 1, 1};
        cells[2] = {0, 1, 1, 1};
        cells[3] = {1, 1, 1, 1};
    } else if (numCards == 5) {
        gridCols = 3; gridRows = 3;
        cells[0] = {0, 0, 1, 1};
        cells[1] = {2, 0, 1, 1};
        cells[2] = {0, 2, 1, 1};
        cells[3] = {2, 2, 1, 1};
        cells[4] = {1, 1, 1, 1};
    } else {
        gridCols = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(numCards))));
        gridRows = (numCards + gridCols - 1) / gridCols;
        for (std::size_t i = 0; i < numCards; ++i)
            cells[i] = {i % gridCols, i / gridCols, 1, 1};
    }

    const double cellUnitWidth = (area.width - spacing * static_cast<double>(gridCols - 1)) / static_cast<double>(gridCols);
    const double cellUnitHeight = (area.height - spacing * static_cast<double>(gridRows - 1)) / static_cast<double>(gridRows);
    const double areaAspect = area.width / area.height;

    constexpr double cardPadding = 16.0;

    std::vector<WindowSlot> slots;

    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& group = groups[gi];
        const auto& cell = cells[gi];
        const std::size_t groupRowGroup = group.front().input.rowGroup;

        // Available area for this card (accounting for spans)
        const double availWidth = static_cast<double>(cell.colSpan) * cellUnitWidth + static_cast<double>(cell.colSpan - 1) * spacing;
        const double availHeight = static_cast<double>(cell.rowSpan) * cellUnitHeight + static_cast<double>(cell.rowSpan - 1) * spacing;
        const double availX = area.x + static_cast<double>(cell.col) * (cellUnitWidth + spacing);
        const double availY = area.y + static_cast<double>(cell.row) * (cellUnitHeight + spacing);

        // Card maintains monitor aspect ratio, centered in available area
        double cardWidth, cardHeight;
        if (availWidth / availHeight > areaAspect) {
            cardHeight = availHeight;
            cardWidth = cardHeight * areaAspect;
        } else {
            cardWidth = availWidth;
            cardHeight = cardWidth / areaAspect;
        }
        const double cardX = availX + (availWidth - cardWidth) / 2.0;
        const double cardY = availY + (availHeight - cardHeight) / 2.0;

        // Compute bounding box of all windows in this workspace group
        double minX = std::numeric_limits<double>::infinity();
        double minY = std::numeric_limits<double>::infinity();
        double maxX = -std::numeric_limits<double>::infinity();
        double maxY = -std::numeric_limits<double>::infinity();

        for (const auto& window : group) {
            minX = std::min(minX, window.input.natural.x);
            minY = std::min(minY, window.input.natural.y);
            maxX = std::max(maxX, window.input.natural.x + window.input.natural.width);
            maxY = std::max(maxY, window.input.natural.y + window.input.natural.height);
        }

        const double bboxWidth = std::max(1.0, maxX - minX);
        const double bboxHeight = std::max(1.0, maxY - minY);

        const double contentScaleX = (cardWidth - 2.0 * cardPadding) / bboxWidth;
        const double contentScaleY = (cardHeight - 2.0 * cardPadding) / bboxHeight;
        const double contentScale = std::min(contentScaleX, contentScaleY);

        for (const auto& window : group) {
            WindowSlot slot;
            slot.index = window.input.index;
            slot.natural = window.input.natural;
            slot.target = {
                cardX + cardPadding + (window.input.natural.x - minX) * contentScale,
                cardY + cardPadding + (window.input.natural.y - minY) * contentScale,
                std::max(1.0, window.input.natural.width * contentScale),
                std::max(1.0, window.input.natural.height * contentScale),
            };
            slot.scale = contentScale;
            slot.rowGroup = groupRowGroup;
            slots.push_back(slot);
        }
    }

    if (slots.size() != prepared.size())
        return std::nullopt;

    std::sort(slots.begin(), slots.end(), [](const WindowSlot& a, const WindowSlot& b) {
        return a.index < b.index;
    });

    return slots;
}

} // namespace

LayoutConfig normalizeLayoutConfig(LayoutConfig config) {
    const LayoutConfig defaults;
    for (auto field : {&LayoutConfig::outerPaddingTop, &LayoutConfig::outerPaddingRight, &LayoutConfig::outerPaddingBottom,
                       &LayoutConfig::outerPaddingLeft, &LayoutConfig::rowSpacing, &LayoutConfig::columnSpacing,
                       &LayoutConfig::minWindowLength, &LayoutConfig::minPreviewShortEdge})
        config.*field = finiteConfig(config.*field, defaults.*field, 0.0, 4096.0);
    config.smallWindowBoost = finiteConfig(config.smallWindowBoost, defaults.smallWindowBoost, 0.1, 4.0);
    config.maxPreviewScale = finiteConfig(config.maxPreviewScale, defaults.maxPreviewScale, 0.0, 4.0);
    config.minSlotScale = finiteConfig(config.minSlotScale, defaults.minSlotScale, 0.0, config.maxPreviewScale);
    config.layoutSpaceWeight = finiteConfig(config.layoutSpaceWeight, defaults.layoutSpaceWeight, 0.0, 100.0);
    config.layoutScaleWeight = finiteConfig(config.layoutScaleWeight, defaults.layoutScaleWeight, 0.0, 100.0);
    return config;
}

std::vector<WindowSlot> MissionControlLayout::compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& rawConfig) const {
    return compute(windows, area, rawConfig, static_cast<LayoutDegradation*>(nullptr));
}

std::vector<WindowSlot> MissionControlLayout::compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& rawConfig,
                                                    LayoutContext& context, LayoutDegradation* degradation) const {
    const auto config = normalizeLayoutConfig(rawConfig);
    if (config.engine == LayoutEngine::Natural)
        return natural2::compute(windows, area, config, &context, degradation);

    // An engine switch ends the natural history, even for an empty request.
    context.reset();
    return compute(windows, area, config, degradation);
}

std::vector<WindowSlot> MissionControlLayout::compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& rawConfig,
                                                    LayoutDegradation* degradation) const {
    const auto config = normalizeLayoutConfig(rawConfig);
    if (config.engine == LayoutEngine::Natural)
        return natural2::compute(windows, area, config, nullptr, degradation);

    if (degradation)
        *degradation = {};
    if (windows.empty())
        return {};

    const Rect inner = insetArea(area, config);
    auto       prepared = prepareWindows(windows, inner, config);

    if (config.engine == LayoutEngine::Thumbnail) {
        if (auto thumbnail = computeThumbnailLayout(prepared, inner, config))
            return *thumbnail;
        return computeGridLayout(prepared, inner, config);
    }

    return computeGridLayout(prepared, inner, config);
}

} // namespace hymission
