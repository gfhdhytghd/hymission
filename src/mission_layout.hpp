#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hymission {

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] double centerX() const {
        return x + width / 2.0;
    }

    [[nodiscard]] double centerY() const {
        return y + height / 2.0;
    }
};

enum class LayoutAxis {
    Horizontal,
    Vertical,
};

struct WindowInput {
    std::size_t index = 0;
    Rect        natural;
    std::string label;
    std::size_t rowGroup = 0;
    double      layoutEmphasis = 1.0;
    // Session-stable identity; index remains the current call's output index.
    std::optional<std::uint64_t> stableId = std::nullopt;
    std::optional<std::int64_t>  groupId = std::nullopt;
    // Optional scrolling structure. IDs are unique across the input region;
    // the indices describe visual order rather than identity.
    std::optional<std::uint64_t> columnId = std::nullopt;
    std::size_t columnIndex = 0;
    std::size_t columnMemberIndex = 0;
    LayoutAxis columnAxis = LayoutAxis::Horizontal;
    // Unknown for legacy callers; known floating windows cannot form columns.
    std::optional<bool> floating = std::nullopt;
};

struct WindowSlot {
    std::size_t index = 0;
    Rect        natural;
    Rect        target;
    double      scale = 1.0;
    std::size_t rowGroup = 0;
};

enum class LayoutEngine {
    Grid,
    Natural,
    Thumbnail,
    Natural2 = Natural, // Compatibility alias; there is only one natural solver.
};

inline LayoutEngine parseOverviewLayoutEngine(std::string_view value) {
    std::string lowered(value);
    for (auto& ch : lowered)
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
    if (lowered == "natural" || lowered == "natural2" || lowered == "apple" || lowered == "apple-like" || lowered == "expose" || lowered == "mission-control")
        return LayoutEngine::Natural;
    if (lowered == "thumbnail" || lowered == "thumbnails")
        return LayoutEngine::Thumbnail;
    return LayoutEngine::Grid;
}

struct LayoutConfig {
    LayoutEngine engine = LayoutEngine::Natural;
    double outerPaddingTop = 48.0;
    double outerPaddingRight = 48.0;
    double outerPaddingBottom = 48.0;
    double outerPaddingLeft = 48.0;
    double rowSpacing = 32.0;
    double columnSpacing = 32.0;
    double smallWindowBoost = 1.35;
    double maxPreviewScale = 0.95;
    double minWindowLength = 120.0;
    double minPreviewShortEdge = 32.0;
    double layoutSpaceWeight = 0.10;
    double layoutScaleWeight = 1.0;
    double minSlotScale = 0.10;
    // Deprecated compatibility field; ignored by every engine.
    double naturalScaleFlex = 0.22;
    bool   preserveInputOrder = false;
    bool   forceRowGroups = false;
    bool   rankScaleByInputOrder = false; // Deprecated; no rank-based size gradient.
};

enum class LayoutReflowKind {
    None,
    ColdInitial,
    Retained,
    LocalInsert,
    LocalReflow,
    ColdFallback,
    StatelessFallback,
};

struct LayoutDegradation {
    // Legacy engines do not self-certify these constraints.
    bool available = false;
    bool feasible = false;
    bool splitColumns = false;
    double effectiveColumnSpacing = 0.0;
    double effectiveRowSpacing = 0.0;
    bool belowMinSlotScale = false;
    bool belowMinShortEdge = false;
    bool emergencySpacing = false;
};

struct LayoutHistoryWindow {
    WindowInput input; // label is diagnostic only, never an identity key
    Rect allocation;
    Rect target;
    // Updated only when a cold/local geometric solve is accepted.
    Rect referenceTarget;
    std::optional<std::pair<double, double>> rejectedResizeSize;
};

struct LayoutHistoryUnit {
    std::optional<std::uint64_t> columnId;
    std::vector<std::uint64_t> members;
    Rect allocation;
};

struct LayoutHistoryBand {
    std::optional<std::int64_t> groupId;
    std::size_t rowGroup = 0;
    std::vector<LayoutHistoryUnit> units;
    Rect allocation;
};

struct LayoutAffineFrame {
    Rect sourceBounds;
    // The center mapping S differs from the actual content scale S*k.
    double centerScale = 0.0;
};

// Value state: speculative layouts must be able to mutate an isolated copy.
// The caller owns the session and resets it before a cold open.
struct LayoutContext {
    bool valid = false;
    Rect area;
    LayoutConfig config;
    bool affine = false;
    double baseScale = 0.0;
    std::optional<LayoutAffineFrame> affineFrame;
    std::vector<LayoutHistoryWindow> windows;
    std::vector<LayoutHistoryBand> bands;
    LayoutReflowKind lastReflowKind = LayoutReflowKind::None;
    LayoutDegradation degradation;

    void reset() {
        *this = {};
    }
};

[[nodiscard]] LayoutConfig normalizeLayoutConfig(LayoutConfig config);

class MissionControlLayout {
  public:
    [[nodiscard]] std::vector<WindowSlot> compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& config = {}) const;
    [[nodiscard]] std::vector<WindowSlot> compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& config,
                                                LayoutDegradation* degradation) const;
    [[nodiscard]] std::vector<WindowSlot> compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& config,
                                                LayoutContext& context, LayoutDegradation* degradation = nullptr) const;
};

} // namespace hymission
