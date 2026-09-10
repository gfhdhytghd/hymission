#include "stage_logic.hpp"

#include <algorithm>
#include <cmath>

namespace hymission::stage {
namespace {
double sane(double value, double fallback) { return std::isfinite(value) ? value : fallback; }
}

Settings normalize(Settings s) {
    s.minWidth = std::max(8.0, sane(s.minWidth, 120));
    s.maxWidth = sane(s.maxWidth, 0);
    s.maxWidth = s.maxWidth <= 0 ? 0 : std::max(s.minWidth, s.maxWidth);
    s.padding = std::max(0.0, sane(s.padding, 12));
    s.cardGap = std::max(0.0, sane(s.cardGap, 12));
    s.desktopGap = std::max(0.0, sane(s.desktopGap, 12));
    return s;
}

Geometry layout(double width, double height, std::size_t count, Settings settings, std::optional<double> frozenCardWidth, double outputWidth) {
    Geometry result;
    result.desktopWidth = std::max(0.0, sane(width, 0));
    result.height = std::max(0.0, sane(height, 0));
    const auto s = normalize(settings);
    // A 64px usable desktop and an 8px card are hard floors only on tiny outputs.
    const double availableWidth = width - 2 * s.padding - s.desktopGap;
    const double maximum = s.maxWidth > 0 ? s.maxWidth : (outputWidth > 0 ? outputWidth : width) / 5;
    const double upper = std::min(maximum, availableWidth - 64);
    if (!count || !std::isfinite(width) || !std::isfinite(height) || upper < 8 || height <= 2 * s.padding + 8)
        return result;

    const double lower = std::min(s.minWidth, upper);
    const double availableHeight = height - 2 * s.padding - static_cast<double>(count - 1) * s.cardGap;
    // Solve N*c*H/(W-2p-g-c) <= H-2p-(N-1)*gap directly. The desktop
    // width and the card width are coupled; fitting against the full monitor
    // aspect first would produce the wrong ratio (and sometimes oscillate).
    const double fit = availableHeight > 0 ? availableHeight * availableWidth / (static_cast<double>(count) * height + availableHeight) : lower;
    const double c = std::clamp(frozenCardWidth ? sane(*frozenCardWidth, fit) : fit, lower, upper);
    result.cardWidth = c;
    result.bandWidth = c + 2 * s.padding;
    result.reservation = result.bandWidth + s.desktopGap;
    result.desktopWidth = width - result.reservation;
    result.cardHeight = c * height / result.desktopWidth;
    result.padding = s.padding;
    result.cardGap = s.cardGap;
    result.count = count;
    result.maxScroll = std::max(0.0, static_cast<double>(count) * result.cardHeight + static_cast<double>(count - 1) * s.cardGap + 2 * s.padding - height);
    return result;
}

double Geometry::clampScroll(double scroll) const { return std::clamp(sane(scroll, 0), 0.0, maxScroll); }
double Geometry::cardTop(std::size_t index, double scroll) const { return padding + static_cast<double>(index) * (cardHeight + cardGap) - clampScroll(scroll); }

double Geometry::reveal(std::size_t index, double scroll) const {
    scroll = clampScroll(scroll);
    if (index >= count)
        return scroll;
    const auto top = cardTop(index, scroll);
    if (top < padding)
        return clampScroll(scroll + top - padding);
    if (top + cardHeight > height - padding)
        return clampScroll(scroll + top + cardHeight - height + padding);
    return scroll;
}

std::optional<std::size_t> Geometry::hit(double x, double y, double scroll) const {
    if (!enabled() || !std::isfinite(x) || !std::isfinite(y) || x < padding || x >= padding + cardWidth || y < padding || y >= height - padding)
        return std::nullopt;
    const double relative = y + clampScroll(scroll) - padding;
    if (relative < 0)
        return std::nullopt;
    const auto index = static_cast<std::size_t>(relative / (cardHeight + cardGap));
    if (index >= count || relative - static_cast<double>(index) * (cardHeight + cardGap) >= cardHeight)
        return std::nullopt;
    return index;
}

bool coversStrip(CoverMode mode, bool maximizeCover) {
    return mode == CoverMode::Fullscreen || (mode == CoverMode::Maximized && maximizeCover);
}

std::vector<WindowSlot> arrangeWindows(const std::vector<WindowInput>& windows, const Geometry& g, const Rect& desktop) {
    std::vector<WindowSlot> slots;
    if (desktop.width <= 0 || desktop.height <= 0 || g.cardWidth <= 0 || g.cardHeight <= 0)
        return slots;
    const double scale = std::min(g.cardWidth / desktop.width, g.cardHeight / desktop.height);
    const double x = (g.cardWidth - desktop.width * scale) / 2;
    const double y = (g.cardHeight - desktop.height * scale) / 2;
    // One transform for the entire workspace preserves placement, overlap and
    // stacking order. Off-desktop portions are clipped by the card framebuffer.
    for (const auto& window : windows) {
        const auto& box = window.natural;
        slots.push_back(WindowSlot{.index = window.index, .natural = box,
            .target = {x + (box.x - desktop.x) * scale, y + (box.y - desktop.y) * scale, box.width * scale, box.height * scale}, .scale = scale});
    }
    return slots;
}

double previewRounding(double configured, double system, double width, double height) {
    const double radius = configured < 0 ? std::max(0.0, sane(system, 0)) / 2 : sane(configured, 0);
    return std::clamp(radius, 0.0, std::max(0.0, std::min(width, height) / 2));
}
} // namespace hymission::stage
