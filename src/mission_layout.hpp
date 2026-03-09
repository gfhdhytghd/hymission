#pragma once

#include <cstddef>
#include <string>
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

struct WindowInput {
    std::size_t index = 0;
    Rect        natural;
    std::string label;
    std::size_t rowGroup = 0;
    double      layoutEmphasis = 1.0;
};

struct WindowSlot {
    std::size_t index = 0;
    Rect        natural;
    Rect        target;
    double      scale = 1.0;
};

struct LayoutConfig {
    double outerPaddingTop = 48.0;
    double outerPaddingRight = 48.0;
    double outerPaddingBottom = 48.0;
    double outerPaddingLeft = 48.0;
    double rowSpacing = 32.0;
    double columnSpacing = 32.0;
    double smallWindowBoost = 1.35;
    double maxPreviewScale = 0.95;
    double minWindowLength = 120.0;
    double layoutSpaceWeight = 0.10;
    double layoutScaleWeight = 1.0;
    double minSlotScale = 0.10;
    bool   preserveInputOrder = false;
    bool   forceRowGroups = false;
};

class MissionControlLayout {
  public:
    [[nodiscard]] std::vector<WindowSlot> compute(const std::vector<WindowInput>& windows, const Rect& area, const LayoutConfig& config = {}) const;
};

} // namespace hymission
