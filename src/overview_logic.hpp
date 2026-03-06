#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "mission_layout.hpp"

namespace hymission {

enum class Direction {
    Left,
    Right,
    Up,
    Down,
};

[[nodiscard]] std::optional<std::size_t> hitTest(const std::vector<Rect>& rects, double x, double y);
[[nodiscard]] std::optional<std::size_t> chooseDirectionalNeighbor(const std::vector<Rect>& rects, std::size_t currentIndex, Direction direction);
[[nodiscard]] Rect                       lerpRect(const Rect& from, const Rect& to, double t);
[[nodiscard]] double                     easeOutCubic(double t);
[[nodiscard]] double                     easeInCubic(double t);

} // namespace hymission
