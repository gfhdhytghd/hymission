#pragma once

#include "mission_layout.hpp"
#include "overview_logic.hpp"

#include <algorithm>
#include <compare>
#include <map>
#include <string_view>

namespace hymission {

struct ScrollingColumnGeometry {
    std::size_t columnIndex;
    LayoutAxis columnAxis;
};

// The native tape reverses primary coordinates for left/up. Members retain
// their secondary-axis order; only the column's visual index is reversed.
inline std::optional<ScrollingColumnGeometry> scrollingColumnGeometry(std::size_t nativeIndex, std::size_t columnCount,
                                                                     ScrollingLayoutDirection direction) {
    if (nativeIndex >= columnCount)
        return std::nullopt;
    switch (direction) {
        case ScrollingLayoutDirection::Right:
            return ScrollingColumnGeometry{nativeIndex, LayoutAxis::Horizontal};
        case ScrollingLayoutDirection::Left:
            return ScrollingColumnGeometry{columnCount - 1 - nativeIndex, LayoutAxis::Horizontal};
        case ScrollingLayoutDirection::Down:
            return ScrollingColumnGeometry{nativeIndex, LayoutAxis::Vertical};
        case ScrollingLayoutDirection::Up:
            return ScrollingColumnGeometry{columnCount - 1 - nativeIndex, LayoutAxis::Vertical};
    }
    return std::nullopt;
}

// Keep the owner alive for this overview snapshot. A closed window/column's
// address cannot then be recycled into a hidden reservation's identity.
template <typename Owner>
std::optional<std::uint64_t> keepLayoutIdentity(std::vector<Owner>& owners, const Owner& owner) {
    if (!owner)
        return std::nullopt;
    if (std::find(owners.begin(), owners.end(), owner) == owners.end())
        owners.push_back(owner);
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(owner.get()));
}

struct OverviewLayoutRegion {
    std::int64_t monitor = 0;
    std::uint64_t tile = 0;
    // Present for current-workspace regions; changing a tile's workspace
    // starts a new history even if its rectangle has not changed.
    std::optional<std::int64_t> workspace;
    auto operator<=>(const OverviewLayoutRegion&) const = default;
};

// A State owns this value. Speculative State copies never mutate the accepted
// history, and teardown discards the whole session without a global cache.
class OverviewLayoutSession {
  public:
    std::vector<WindowSlot> compute(const OverviewLayoutRegion& region, const std::vector<WindowInput>& inputs, const Rect& area,
                                    const LayoutConfig& config) {
        MissionControlLayout layout;
        if (config.engine != LayoutEngine::Natural) {
            m_contexts.clear();
            return layout.compute(inputs, area, config);
        }
        return layout.compute(inputs, area, config, m_contexts[region]);
    }

    void retainRegions(const std::vector<OverviewLayoutRegion>& regions) {
        std::erase_if(m_contexts, [&](const auto& entry) {
            return std::find(regions.begin(), regions.end(), entry.first) == regions.end();
        });
    }

    const LayoutContext* context(const OverviewLayoutRegion& region) const {
        const auto it = m_contexts.find(region);
        return it == m_contexts.end() ? nullptr : &it->second;
    }

  private:
    std::map<OverviewLayoutRegion, LayoutContext> m_contexts;
};

} // namespace hymission
