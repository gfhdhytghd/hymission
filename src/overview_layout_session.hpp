#pragma once

#include "mission_layout.hpp"
#include "overview_logic.hpp"

#include <algorithm>
#include <cmath>
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
template <typename Owner> std::optional<std::uint64_t> keepLayoutIdentity(std::vector<Owner> &owners, const Owner &owner) {
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
    auto operator<=>(const OverviewLayoutRegion &) const = default;
};

// A State owns this value. Speculative State copies never mutate the accepted
// history, and teardown discards the whole session without a global cache.
class OverviewLayoutSession {
  public:
    std::vector<WindowSlot> compute(const OverviewLayoutRegion &region, const std::vector<WindowInput> &inputs, const Rect &area,
                                    const LayoutConfig &config) {
        MissionControlLayout layout;
        if (config.engine != LayoutEngine::Natural) {
            m_contexts.clear();
            return layout.compute(inputs, area, config);
        }
        auto &frame = m_contexts[region];
        auto baseInputs = inputs;
        auto solveArea = area;
        // Controller expansion is a separate rendering pass, never core emphasis.
        for (auto &input : baseInputs)
            input.layoutEmphasis = 1.0;
        if (frame.context.valid && sameRegionConstraints(frame, area, config)) {
            // Native focus can resize/reorder scrolling columns between frames.
            // With identical membership, reuse the complete source snapshot so
            // neither a resize nor column metadata can trigger LocalReflow.
            auto frozen = frozenInputs(frame.inputs, baseInputs);
            if (frozen) {
                baseInputs = std::move(*frozen);
                solveArea = frame.area; // Ignore subpixel viewport roundoff too.
            }
        }
        auto result = layout.compute(baseInputs, solveArea, config, frame.context);
        frame.inputs = std::move(baseInputs);
        frame.area = solveArea;
        frame.config = normalizeLayoutConfig(config);
        return result;
    }

    void retainRegions(const std::vector<OverviewLayoutRegion> &regions) {
        std::erase_if(m_contexts,
                      [&](const auto &entry) { return std::find(regions.begin(), regions.end(), entry.first) == regions.end(); });
    }

    const LayoutContext *context(const OverviewLayoutRegion &region) const {
        const auto it = m_contexts.find(region);
        return it == m_contexts.end() ? nullptr : &it->second.context;
    }

    bool allRegionsRetained() const {
        return !m_contexts.empty() && std::ranges::all_of(m_contexts, [](const auto &entry) {
            return entry.second.context.lastReflowKind == LayoutReflowKind::Retained;
        });
    }

  private:
    struct Frame {
        LayoutContext context;
        std::vector<WindowInput> inputs;
        Rect area;
        LayoutConfig config;
    };

    static bool sameRegionConstraints(const Frame &frame, const Rect &area, const LayoutConfig &rawConfig) {
        const auto b = normalizeLayoutConfig(rawConfig);
        const auto &a = frame.config;
        const auto near = [](double x, double y) { return std::abs(x - y) <= 0.001; };
        return near(frame.area.x, area.x) && near(frame.area.y, area.y) && near(frame.area.width, area.width) &&
               near(frame.area.height, area.height) && a.engine == b.engine && a.outerPaddingTop == b.outerPaddingTop &&
               a.outerPaddingRight == b.outerPaddingRight && a.outerPaddingBottom == b.outerPaddingBottom &&
               a.outerPaddingLeft == b.outerPaddingLeft && a.rowSpacing == b.rowSpacing && a.columnSpacing == b.columnSpacing &&
               a.minWindowLength == b.minWindowLength && a.smallWindowBoost == b.smallWindowBoost &&
               a.maxPreviewScale == b.maxPreviewScale && a.minPreviewShortEdge == b.minPreviewShortEdge &&
               a.minSlotScale == b.minSlotScale && a.forceRowGroups == b.forceRowGroups;
    }

    static std::optional<std::vector<WindowInput>> frozenInputs(const std::vector<WindowInput> &previous,
                                                                const std::vector<WindowInput> &current) {
        if (previous.size() != current.size())
            return std::nullopt;
        std::map<std::uint64_t, const WindowInput *> byId;
        for (const auto &input : previous)
            if (!input.stableId || !byId.emplace(*input.stableId, &input).second)
                return std::nullopt;
        std::vector<WindowInput> frozen;
        for (const auto &input : current) {
            if (!input.stableId)
                return std::nullopt;
            const auto it = byId.find(*input.stableId);
            if (it == byId.end() || it->second->groupId != input.groupId || (!input.groupId && it->second->rowGroup != input.rowGroup))
                return std::nullopt;
            auto copy = *it->second;
            copy.index = input.index;
            copy.label = input.label;
            frozen.push_back(std::move(copy));
            byId.erase(it); // Reject duplicate current identities.
        }
        return frozen;
    }

    std::map<OverviewLayoutRegion, Frame> m_contexts;
};

} // namespace hymission
