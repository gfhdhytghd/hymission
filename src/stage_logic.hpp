#pragma once

#include <cstddef>
#include <optional>
#include "mission_layout.hpp"

namespace hymission::stage {

// All dimensions are logical pixels. Width/height have already had the bar's
// reserved area removed; desktop gaps belong to the native layout, not here.
struct Settings {
    double minWidth = 120;
    double maxWidth = 0; // 0: one fifth of the output's logical width
    double padding = 12;
    double cardGap = 12;
    double desktopGap = 12;
};

struct Geometry {
    double cardWidth = 0;
    double cardHeight = 0;
    double bandWidth = 0;
    double reservation = 0;
    double desktopWidth = 0;
    double height = 0;
    double padding = 0;
    double cardGap = 0;
    double maxScroll = 0;
    std::size_t count = 0;

    bool enabled() const { return count && reservation > 0; }
    double cardTop(std::size_t index, double scroll) const;
    double clampScroll(double scroll) const;
    double reveal(std::size_t index, double scroll) const;
    std::optional<std::size_t> hit(double x, double y, double scroll) const;
};

Settings normalize(Settings settings);
Geometry layout(double width, double height, std::size_t count, Settings settings,
                std::optional<double> frozenCardWidth = std::nullopt, double outputWidth = 0);
std::vector<WindowSlot> arrangeWindows(const std::vector<WindowInput>& windows, const Geometry& geometry, const Rect& desktop);
double previewRounding(double configured, double system, double width, double height);

// Keep this policy independent of compositor enum values for state tests.
enum class CoverMode { None, Maximized, Fullscreen };
bool coversStrip(CoverMode mode, bool maximizeCover);

} // namespace hymission::stage
