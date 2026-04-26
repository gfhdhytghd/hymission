#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "mission_layout.hpp"

namespace {

using hymission::LayoutConfig;
using hymission::MissionControlLayout;
using hymission::Rect;
using hymission::WindowInput;
using hymission::WindowSlot;

bool expect(bool condition, const char* message) {
    if (condition)
        return true;

    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool closeEnough(double actual, double expected, double epsilon = 1e-6) {
    return std::abs(actual - expected) <= epsilon;
}

bool rectsOverlap(const Rect& lhs, const Rect& rhs) {
    return lhs.x < rhs.x + rhs.width && lhs.x + lhs.width > rhs.x && lhs.y < rhs.y + rhs.height && lhs.y + lhs.height > rhs.y;
}

bool rectInside(const Rect& rect, const Rect& area) {
    constexpr double epsilon = 0.0001;
    return rect.x + epsilon >= area.x && rect.y + epsilon >= area.y &&
           rect.x + rect.width <= area.x + area.width + epsilon &&
           rect.y + rect.height <= area.y + area.height + epsilon;
}

bool expectSlot(const WindowSlot& actual, const Rect& expectedTarget, double expectedScale, const char* message) {
    return expect(closeEnough(actual.target.x, expectedTarget.x) && closeEnough(actual.target.y, expectedTarget.y) &&
                      closeEnough(actual.target.width, expectedTarget.width) && closeEnough(actual.target.height, expectedTarget.height) &&
                      closeEnough(actual.scale, expectedScale),
                  message);
}

LayoutConfig deterministicConfig() {
    LayoutConfig config;
    config.outerPaddingTop = 0.0;
    config.outerPaddingRight = 0.0;
    config.outerPaddingBottom = 0.0;
    config.outerPaddingLeft = 0.0;
    config.smallWindowBoost = 1.0;
    config.maxPreviewScale = 1.0;
    config.minSlotScale = 0.0;
    config.preserveInputOrder = true;
    return config;
}

LayoutConfig sortedLayoutConfig() {
    LayoutConfig config = deterministicConfig();
    config.preserveInputOrder = false;
    return config;
}

} // namespace

int main() {
    using namespace hymission;

    MissionControlLayout engine;
    bool                 ok = true;

    {
        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 0, 692, 173}, .label = "wide-short"},
            {.index = 1, .natural = {0, 220, 198, 719}, .label = "narrow-tall"},
            {.index = 2, .natural = {0, 460, 546, 622}, .label = "wide-tall"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1000, 800}, deterministicConfig());
        ok &= expect(slots.size() == 3, "row-height case should keep all windows");
        ok &= expectSlot(slots[0], {202.0, 0.0, 595.8026905829597, 148.95067264573993}, 0.8609865470852018,
                         "rejected windows must not contaminate the previous row height");
        ok &= expectSlot(slots[1], {163.0, 180.0, 170.47533632286996, 619.0493273542601}, 0.8609865470852018,
                         "second slot should stay in the lower row after the row-height fix");
    }

    {
        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 0, 186, 501}, .label = "a"},
            {.index = 1, .natural = {0, 200, 210, 733}, .label = "b"},
            {.index = 2, .natural = {0, 400, 1035, 100}, .label = "c"},
            {.index = 3, .natural = {0, 600, 240, 483}, .label = "d"},
            {.index = 4, .natural = {0, 800, 529, 1150}, .label = "e"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1000, 800}, deterministicConfig());
        ok &= expect(slots.size() == 5, "row-search case should keep all windows");
        ok &= expect(slots[2].target.x < 150.0 && slots[2].target.y > 700.0 && slots[2].target.width > 400.0,
                     "layout search must continue past the first local score drop");
        ok &= expectSlot(slots[3], {554.0, 603.0, 97.88635156664897, 196.99628252788105}, 0.4078597981943707,
                         "later row-count candidates should be allowed to win");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.rowSpacing = 200.0;
        config.columnSpacing = 0.0;
        config.forceRowGroups = true;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 0, 80, 80}, .label = "a", .rowGroup = 0},
            {.index = 1, .natural = {0, 0, 80, 80}, .label = "b", .rowGroup = 1},
        };

        const auto slots = engine.compute(windows, {0, 0, 120, 120}, config);
        ok &= expect(slots.size() == 2, "spacing clamp case should keep row-grouped windows");
        ok &= expect(slots[0].scale == 0.0 && slots[1].scale == 0.0, "oversized spacing should clamp scales to zero instead of going negative");
        ok &= expect(slots[0].target.width == 0.0 && slots[0].target.height == 0.0 && slots[1].target.width == 0.0 && slots[1].target.height == 0.0,
                     "oversized spacing should never produce negative preview sizes");
    }

    {
        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {260, 0, 50, 500}, .label = "thin-middle"},
            {.index = 1, .natural = {0, 0, 180, 180}, .label = "left"},
            {.index = 2, .natural = {420, 0, 180, 180}, .label = "right"},
        };

        const auto slots = engine.compute(windows, {0, 0, 600, 600}, sortedLayoutConfig());
        ok &= expect(slots.size() == 3, "default-order case should keep all windows");
        ok &= expect(closeEnough(slots[0].natural.width, 50.0) && closeEnough(slots[0].natural.height, 500.0),
                     "layout must preserve the original natural geometry");
        ok &= expect(slots[0].target.height > 0.0 && closeEnough(slots[0].target.width / slots[0].target.height, 50.0 / 500.0),
                     "layout previews must preserve the original aspect ratio");
        ok &= expect(slots[1].target.x < slots[0].target.x && slots[0].target.x < slots[2].target.x,
                     "default ordering should reorder slots by geometry instead of preserving input order");
    }

    {
        const std::vector<WindowInput> baseline = {
            {.index = 0, .natural = {0, 0, 200, 160}, .label = "selected"},
            {.index = 1, .natural = {240, 0, 200, 160}, .label = "neighbor"},
        };
        const std::vector<WindowInput> emphasized = {
            {.index = 0, .natural = {0, 0, 200, 160}, .label = "selected", .layoutEmphasis = 1.18},
            {.index = 1, .natural = {240, 0, 200, 160}, .label = "neighbor"},
        };

        const auto baselineSlots = engine.compute(baseline, {0, 0, 420, 220}, deterministicConfig());
        const auto emphasizedSlots = engine.compute(emphasized, {0, 0, 420, 220}, deterministicConfig());
        ok &= expect(baselineSlots.size() == 2 && emphasizedSlots.size() == 2, "selected emphasis case should keep both windows");
        ok &= expect(emphasizedSlots[0].target.width > baselineSlots[0].target.width,
                     "selected emphasis should enlarge the selected preview");
        ok &= expect(emphasizedSlots[1].target.width < baselineSlots[1].target.width || emphasizedSlots[1].target.x > baselineSlots[1].target.x,
                     "selected emphasis should push neighboring previews away");
        ok &= expect(!rectsOverlap(emphasizedSlots[0].target, emphasizedSlots[1].target),
                     "selected emphasis should keep previews non-overlapping");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {40, 40, 520, 360}, .label = "top-left"},
            {.index = 1, .natural = {520, 60, 440, 330}, .label = "top-right"},
            {.index = 2, .natural = {90, 380, 500, 300}, .label = "bottom-left"},
            {.index = 3, .natural = {570, 360, 410, 320}, .label = "bottom-right"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1000, 700}, config);
        ok &= expect(slots.size() == 4, "natural engine should keep all windows");
        for (std::size_t i = 0; i < slots.size(); ++i) {
            for (std::size_t j = i + 1; j < slots.size(); ++j)
                ok &= expect(!rectsOverlap(slots[i].target, slots[j].target), "natural engine should keep previews non-overlapping");
        }
        ok &= expect(slots[0].target.centerX() < slots[1].target.centerX() && slots[2].target.centerX() < slots[3].target.centerX(),
                     "natural engine should preserve left/right spatial memory");
        ok &= expect(slots[0].target.centerY() < slots[2].target.centerY() && slots[1].target.centerY() < slots[3].target.centerY(),
                     "natural engine should preserve top/bottom spatial memory");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {320, 780, 520, 360}, .label = "lower-left"},
            {.index = 1, .natural = {870, 790, 440, 330}, .label = "lower-right"},
            {.index = 2, .natural = {420, 1180, 500, 300}, .label = "bottom-left"},
            {.index = 3, .natural = {980, 1160, 410, 320}, .label = "bottom-right"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1400, 900}, config);
        ok &= expect(slots.size() == 4, "natural engine should keep off-center source windows");

        double centerY = 0.0;
        for (const auto& slot : slots)
            centerY += slot.target.centerY();
        centerY /= static_cast<double>(slots.size());

        ok &= expect(centerY > 330.0 && centerY < 570.0, "natural engine should recenter an off-screen-biased window cloud");
        ok &= expect(slots[0].target.centerX() < slots[1].target.centerX() && slots[2].target.centerX() < slots[3].target.centerX(),
                     "recentered natural engine should preserve left/right spatial memory");
        ok &= expect(slots[0].target.centerY() < slots[2].target.centerY() && slots[1].target.centerY() < slots[3].target.centerY(),
                     "recentered natural engine should preserve top/bottom spatial memory");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {80, 80, 1200, 700}, .label = "stacked-a"},
            {.index = 1, .natural = {90, 90, 1200, 700}, .label = "stacked-b"},
            {.index = 2, .natural = {100, 100, 1200, 700}, .label = "stacked-c"},
            {.index = 3, .natural = {110, 110, 1200, 700}, .label = "stacked-d"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1400, 900}, config);
        ok &= expect(slots.size() == 4, "natural engine should keep heavily overlapping windows");

        double averageScale = 0.0;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            averageScale += slots[i].scale;
            for (std::size_t j = i + 1; j < slots.size(); ++j)
                ok &= expect(!rectsOverlap(slots[i].target, slots[j].target), "overlap jitter should still produce non-overlapping previews");
        }
        averageScale /= static_cast<double>(slots.size());

        ok &= expect(averageScale > 0.22, "overlap jitter should avoid collapsing heavily stacked windows into tiny previews");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.minPreviewShortEdge = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 40, 1450, 48}, .label = "thin-wide"},
            {.index = 1, .natural = {120, 140, 56, 680}, .label = "thin-tall"},
            {.index = 2, .natural = {260, 260, 1300, 64}, .label = "thin-wide-2"},
            {.index = 3, .natural = {560, 70, 64, 720}, .label = "thin-tall-2"},
            {.index = 4, .natural = {460, 420, 28, 26}, .label = "tiny"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1100, 700}, config);
        ok &= expect(slots.size() == 5, "natural engine should keep extreme-aspect windows");
        for (const auto& slot : slots) {
            ok &= expect(rectInside(slot.target, {0, 0, 1100, 700}), "extreme-aspect previews should stay inside the monitor");
            ok &= expect(std::min(slot.target.width, slot.target.height) >= 31.9,
                         "natural engine should enforce a readable short edge for extreme-aspect previews");
        }
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {900, 100, 360, 260}, .label = "right-top"},
            {.index = 1, .natural = {980, 430, 360, 260}, .label = "right-bottom"},
            {.index = 2, .natural = {1220, 270, 240, 240}, .label = "far-right"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1400, 900}, config);
        ok &= expect(slots.size() == 3, "natural engine should keep right-biased windows");

        double minX = 1400.0;
        double maxX = 0.0;
        for (const auto& slot : slots) {
            minX = std::min(minX, slot.target.x);
            maxX = std::max(maxX, slot.target.x + slot.target.width);
        }

        const double centerX = (minX + maxX) / 2.0;
        ok &= expect(centerX > 560.0 && centerX < 840.0, "natural engine should center the final target cloud");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const Rect area{0, 0, 1440, 900};
        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 60, 3800, 150}, .label = "ultra-wide-a"},
            {.index = 1, .natural = {-600, 355, 4200, 140}, .label = "ultra-wide-b"},
            {.index = 2, .natural = {900, 120, 2900, 160}, .label = "ultra-wide-c"},
            {.index = 3, .natural = {80, -900, 180, 3600}, .label = "ultra-tall-a"},
            {.index = 4, .natural = {560, 20, 160, 2800}, .label = "ultra-tall-b"},
        };

        const auto slots = engine.compute(windows, area, config);
        ok &= expect(slots.size() == windows.size(), "natural engine should keep extreme-aspect windows");
        for (const auto& slot : slots)
            ok &= expect(rectInside(slot.target, area), "natural engine should fit extreme-aspect targets inside the area");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 8.0;
        config.columnSpacing = 8.0;
        config.minSlotScale = 0.05;

        const Rect area{0, 0, 1440, 900};
        std::vector<WindowInput> windows;
        for (std::size_t i = 0; i < 120; ++i) {
            const double x = static_cast<double>((i * 73) % 1320);
            const double y = static_cast<double>((i * 47) % 780);
            const double width = 60.0 + static_cast<double>((i * 19) % 160);
            const double height = 50.0 + static_cast<double>((i * 23) % 130);
            windows.push_back({.index = i, .natural = {x, y, width, height}, .label = "dense"});
        }

        const auto slots = engine.compute(windows, area, config);
        ok &= expect(slots.size() == windows.size(), "dense natural requests should fall back without dropping windows");
        for (const auto& slot : slots)
            ok &= expect(rectInside(slot.target, area), "dense fallback should keep targets inside the area");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.forceRowGroups = true;
        config.rowSpacing = 40.0;
        config.columnSpacing = 24.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 0, 240, 180}, .label = "a", .rowGroup = 0},
            {.index = 1, .natural = {300, 20, 240, 180}, .label = "b", .rowGroup = 0},
            {.index = 2, .natural = {0, 260, 240, 180}, .label = "c", .rowGroup = 1},
            {.index = 3, .natural = {300, 280, 240, 180}, .label = "d", .rowGroup = 1},
        };

        const Rect area{0, 0, 700, 500};
        const auto slots = engine.compute(windows, area, config);
        ok &= expect(slots.size() == windows.size(), "row-group natural layout should preserve cardinality");
        for (const auto& slot : slots)
            ok &= expect(rectInside(slot.target, area), "row-group natural layout should stay inside the area");
        ok &= expect(slots[0].target.centerY() < slots[2].target.centerY() && slots[1].target.centerY() < slots[3].target.centerY(),
                     "row-group natural layout should keep workspace bands ordered");
        ok &= expect(slots[0].target.centerX() < slots[1].target.centerX() && slots[2].target.centerX() < slots[3].target.centerX(),
                     "row-group natural layout should preserve within-band left/right order");
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
