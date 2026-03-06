#include "mission_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace hymission {

namespace {

struct PreparedWindow {
    WindowInput input;
    double      weightScale = 1.0;
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

double clampPositive(double value) {
    return std::max(0.0, value);
}

double lerp(double a, double b, double t) {
    const auto clamped = std::clamp(t, 0.0, 1.0);
    return a + (b - a) * clamped;
}

double computeWindowScale(const WindowInput& window, const Rect& monitorArea, const LayoutConfig& config) {
    const double denom = std::max(1.0, monitorArea.height);
    const double ratio = std::clamp(window.natural.height / denom, 0.0, 1.0);
    return lerp(config.smallWindowBoost, 1.0, ratio);
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

Rect insetArea(const Rect& area, double padding) {
    const double twice = std::max(0.0, padding * 2.0);
    return {
        area.x + padding,
        area.y + padding,
        std::max(1.0, area.width - twice),
        std::max(1.0, area.height - twice),
    };
}

std::vector<PreparedWindow> prepareWindows(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& config) {
    std::vector<PreparedWindow> prepared;
    prepared.reserve(windows.size());

    for (const auto& window : windows) {
        PreparedWindow item;
        item.input = window;
        item.input.natural.width = std::max(window.natural.width, config.minWindowLength);
        item.input.natural.height = std::max(window.natural.height, config.minWindowLength);
        item.weightScale = computeWindowScale(item.input, area, config);
        item.scaledWidth = item.input.natural.width * item.weightScale;
        item.scaledHeight = item.input.natural.height * item.weightScale;
        prepared.push_back(item);
    }

    return prepared;
}

LayoutCandidate buildCandidate(const std::vector<PreparedWindow>& prepared, std::size_t numRows, const Rect& area, const LayoutConfig& config) {
    LayoutCandidate candidate;

    double totalWidth = 0.0;
    for (const auto& window : prepared)
        totalWidth += window.scaledWidth;

    const double idealRowWidth = totalWidth / static_cast<double>(numRows);

    std::vector<PreparedWindow> sorted = prepared;
    std::sort(sorted.begin(), sorted.end(), [](const PreparedWindow& a, const PreparedWindow& b) {
        return a.input.natural.centerY() < b.input.natural.centerY();
    });

    std::size_t windowIdx = 0;
    for (std::size_t rowIndex = 0; rowIndex < numRows; ++rowIndex) {
        Row row;
        for (; windowIdx < sorted.size(); ++windowIdx) {
            const auto& window = sorted[windowIdx];
            row.fullHeight = std::max(row.fullHeight, window.scaledHeight);

            if (keepSameRow(row, window.scaledWidth, idealRowWidth) || rowIndex + 1 == numRows) {
                row.windows.push_back(window);
                row.fullWidth += window.scaledWidth;
            } else {
                break;
            }
        }

        if (!row.windows.empty()) {
            std::sort(row.windows.begin(), row.windows.end(), [](const PreparedWindow& a, const PreparedWindow& b) {
                return a.input.natural.centerX() < b.input.natural.centerX();
            });
            candidate.maxColumns = std::max(candidate.maxColumns, row.windows.size());
            candidate.gridWidth = std::max(candidate.gridWidth, row.fullWidth);
            candidate.gridHeight += row.fullHeight;
            candidate.rows.push_back(std::move(row));
        }
    }

    if (candidate.rows.empty())
        return candidate;

    const double horizontalSpacing = static_cast<double>(candidate.maxColumns > 0 ? candidate.maxColumns - 1 : 0) * config.columnSpacing;
    const double verticalSpacing = static_cast<double>(candidate.rows.size() > 0 ? candidate.rows.size() - 1 : 0) * config.rowSpacing;

    const double horizontalScale = (area.width - horizontalSpacing) / std::max(1.0, candidate.gridWidth);
    const double verticalScale = (area.height - verticalSpacing) / std::max(1.0, candidate.gridHeight);

    candidate.layoutScale = std::clamp(std::min(horizontalScale, verticalScale), config.minSlotScale, config.maxPreviewScale);

    for (auto& row : candidate.rows) {
        row.width = row.fullWidth * candidate.layoutScale + static_cast<double>(row.windows.size() > 0 ? row.windows.size() - 1 : 0) * config.columnSpacing;
        row.height = row.fullHeight * candidate.layoutScale;
    }

    const double usedWidth = candidate.gridWidth * candidate.layoutScale + horizontalSpacing;
    const double usedHeight = candidate.gridHeight * candidate.layoutScale + verticalSpacing;
    const double areaPixels = std::max(1.0, area.width * area.height);
    const double space = (usedWidth * usedHeight) / areaPixels;

    candidate.score = candidate.layoutScale * config.layoutScaleWeight + space * config.layoutSpaceWeight;
    return candidate;
}

std::vector<WindowSlot> materializeSlots(LayoutCandidate candidate, const Rect& area, const LayoutConfig& config) {
    if (candidate.rows.empty())
        return {};

    double heightWithoutSpacing = 0.0;
    for (const auto& row : candidate.rows)
        heightWithoutSpacing += row.height;

    const double verticalSpacing = static_cast<double>(candidate.rows.size() > 0 ? candidate.rows.size() - 1 : 0) * config.rowSpacing;
    const double additionalVerticalScale = std::min(1.0, (area.height - verticalSpacing) / std::max(1.0, heightWithoutSpacing));

    double compensation = 0.0;
    double y = 0.0;

    for (auto& row : candidate.rows) {
        const double horizontalSpacing = static_cast<double>(row.windows.size() > 0 ? row.windows.size() - 1 : 0) * config.columnSpacing;
        const double widthWithoutSpacing = std::max(1.0, row.width - horizontalSpacing);
        const double additionalHorizontalScale = std::min(1.0, (area.width - horizontalSpacing) / widthWithoutSpacing);

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
            const double cellWidth = window.input.natural.width * scale;
            const double cellHeight = window.input.natural.height * scale;

            scale = std::min(scale, config.maxPreviewScale);

            const double previewWidth = window.input.natural.width * scale;
            const double previewHeight = window.input.natural.height * scale;
            const double previewX = std::floor(x + (cellWidth - previewWidth) / 2.0);
            const double previewY = std::floor(candidate.rows.size() == 1 ? rowY + (rowHeight - previewHeight) / 2.0
                                                                           : rowY + rowHeight - cellHeight);

            slots.push_back({
                .index = window.input.index,
                .natural = window.input.natural,
                .target = {previewX, previewY, previewWidth, previewHeight},
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

} // namespace

std::vector<WindowSlot> MissionControlLayout::compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& config) const {
    if (windows.empty())
        return {};

    const Rect inner = insetArea(area, config.outerPadding);
    const auto prepared = prepareWindows(windows, inner, config);

    std::optional<LayoutCandidate> best;
    for (std::size_t numRows = 1; numRows <= prepared.size(); ++numRows) {
        auto candidate = buildCandidate(prepared, numRows, inner, config);
        if (candidate.rows.empty())
            continue;

        if (!best || candidate.score >= best->score) {
            best = std::move(candidate);
        } else {
            break;
        }
    }

    if (!best)
        return {};

    return materializeSlots(std::move(*best), inner, config);
}

} // namespace hymission
