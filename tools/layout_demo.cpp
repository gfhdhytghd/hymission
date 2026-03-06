#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

#include "mission_layout.hpp"

int main() {
    using namespace hymission;

    const Rect area{0, 0, 1728, 1117};
    const std::vector<WindowInput> windows = {
        {.index = 0, .natural = {40, 60, 1440, 900}, .label = "Browser"},
        {.index = 1, .natural = {1200, 120, 560, 820}, .label = "Chat"},
        {.index = 2, .natural = {180, 160, 700, 520}, .label = "Editor"},
        {.index = 3, .natural = {980, 640, 640, 420}, .label = "Terminal"},
        {.index = 4, .natural = {90, 760, 420, 320}, .label = "Music"},
        {.index = 5, .natural = {1480, 740, 320, 240}, .label = "Clock"},
    };

    MissionControlLayout engine;
    const auto slots = engine.compute(windows, area);

    for (const auto& slot : slots) {
        std::cout << '#' << slot.index << ' '
                  << std::setw(4) << static_cast<int>(slot.target.x) << ' '
                  << std::setw(4) << static_cast<int>(slot.target.y) << ' '
                  << std::setw(4) << static_cast<int>(slot.target.width) << 'x'
                  << std::setw(4) << static_cast<int>(slot.target.height)
                  << " scale=" << std::fixed << std::setprecision(3) << slot.scale
                  << '\n';
    }

    return EXIT_SUCCESS;
}
