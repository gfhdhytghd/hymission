#pragma once

#include "mission_layout.hpp"
#include <algorithm>

namespace hymission {
// Reserve this band even when the search helper is hidden. User padding is
// applied to content afterwards, so opening or typing never moves previews.
inline constexpr double SEARCH_HEADER_HEIGHT = 64.0;
struct SearchHeaderReservation {
    Rect header;
    Rect content;
};
inline SearchHeaderReservation reserveSearchHeader(const Rect& available) {
    const double height = std::min(SEARCH_HEADER_HEIGHT, std::max(0.0, available.height - 1.0));
    return {{available.x, available.y, available.width, height},
            {available.x, available.y + height, available.width, std::max(1.0, available.height - height)}};
}
} // namespace hymission
