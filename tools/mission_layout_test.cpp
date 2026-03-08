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

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
