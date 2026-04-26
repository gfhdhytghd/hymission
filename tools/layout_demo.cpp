#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "mission_layout.hpp"

namespace {

using hymission::LayoutConfig;
using hymission::LayoutEngine;
using hymission::MissionControlLayout;
using hymission::Rect;
using hymission::WindowInput;
using hymission::WindowSlot;

struct Scene {
    std::string              name;
    Rect                     area;
    std::vector<WindowInput> windows;
};

struct Options {
    std::string              sceneName = "forceall";
    LayoutEngine             engine = LayoutEngine::Natural;
    std::optional<std::string> outputPath;
    std::size_t              stressCases = 0;
    unsigned                 seed = 0xC0FFEE;
    double                   minPreviewShortEdge = 32.0;
    std::optional<double>    naturalScaleFlex;
    bool                     forceRowGroups = false;
    bool                     preserveInputOrder = false;
    bool                     rankScaleByInputOrder = false;
    bool                     listScenes = false;
    bool                     help = false;
};

struct LayoutMetrics {
    double overlapArea = 0.0;
    double outOfBoundsArea = 0.0;
    double minScale = std::numeric_limits<double>::infinity();
    double averageScale = 0.0;
    double minShortEdge = std::numeric_limits<double>::infinity();
    double averageShortEdge = 0.0;
    double targetAreaRatio = 0.0;
    double targetCentroidX = 0.0;
    double targetCentroidY = 0.0;
    double gravityOffset = 0.0;
    double edgeMarginLeft = std::numeric_limits<double>::infinity();
    double edgeMarginRight = std::numeric_limits<double>::infinity();
    double edgeMarginTop = std::numeric_limits<double>::infinity();
    double edgeMarginBottom = std::numeric_limits<double>::infinity();
    double minEdgeMargin = 0.0;
    double edgeBalanceX = 0.0;
    double edgeBalanceY = 0.0;
    double averageMotion = 0.0;
    double maxMotion = 0.0;
    double heatMax = 0.0;
    double heatStdDev = 0.0;
    double heatImbalance = 0.0;
    std::size_t xInversions = 0;
    std::size_t yInversions = 0;
    double score = 0.0;
};

struct StressResult {
    std::size_t              caseIndex = 0;
    Scene                    scene;
    std::vector<WindowSlot>  slots;
    LayoutMetrics            metrics;
};

struct MetricSeries {
    std::vector<double> values;
    double              sum = 0.0;
    double              min = std::numeric_limits<double>::infinity();
    double              max = 0.0;
};

struct StressSummary {
    std::size_t cases = 0;
    std::size_t overlapCases = 0;
    std::size_t outOfBoundsCases = 0;
    std::size_t unreadableShortEdgeCases = 0;
    MetricSeries score;
    MetricSeries gravityOffset;
    MetricSeries edgeMarginLeft;
    MetricSeries edgeMarginRight;
    MetricSeries edgeMarginTop;
    MetricSeries edgeMarginBottom;
    MetricSeries minEdgeMargin;
    MetricSeries edgeBalanceX;
    MetricSeries edgeBalanceY;
    MetricSeries heatMax;
    MetricSeries heatStdDev;
    MetricSeries heatImbalance;
    MetricSeries averageMotion;
    MetricSeries maxMotion;
    MetricSeries minShortEdge;
    MetricSeries averageShortEdge;
    MetricSeries totalInversions;
};

struct StressReport {
    StressResult  worst;
    StressSummary summary;
};

void printSlots(const std::vector<WindowSlot>& slots);
void printMetrics(const LayoutMetrics& metrics);

std::string escapeXml(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped += ch; break;
        }
    }
    return escaped;
}

double parseDouble(const std::string& value, const char* optionName) {
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        std::cerr << "Invalid number for " << optionName << ": " << value << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::size_t parseSize(const std::string& value, const char* optionName) {
    try {
        return static_cast<std::size_t>(std::stoull(value));
    } catch (const std::exception&) {
        std::cerr << "Invalid integer for " << optionName << ": " << value << '\n';
        std::exit(EXIT_FAILURE);
    }
}

unsigned parseUnsigned(const std::string& value, const char* optionName) {
    try {
        return static_cast<unsigned>(std::stoul(value));
    } catch (const std::exception&) {
        std::cerr << "Invalid integer for " << optionName << ": " << value << '\n';
        std::exit(EXIT_FAILURE);
    }
}

LayoutEngine parseEngine(const std::string& value) {
    if (value == "grid")
        return LayoutEngine::Grid;
    if (value == "natural" || value == "apple" || value == "mission-control" || value == "expose")
        return LayoutEngine::Natural;

    std::cerr << "Unknown engine: " << value << '\n';
    std::exit(EXIT_FAILURE);
}

std::string engineName(LayoutEngine engine) {
    return engine == LayoutEngine::Natural ? "natural" : "grid";
}

std::vector<Scene> scenes() {
    return {
        {
            .name = "forceall",
            .area = {0, 0, 1440, 900},
            .windows =
                {
                    {.index = 0, .natural = {18, 58, 710, 416}, .label = "Ghostty current"},
                    {.index = 1, .natural = {342, 38, 676, 141}, .label = "Dropdown terminal"},
                    {.index = 2, .natural = {1130, 160, 310, 390}, .label = "Dolphin"},
                    {.index = 3, .natural = {380, 560, 600, 415}, .label = "Chrome"},
                    {.index = 4, .natural = {40, 535, 598, 415}, .label = "Ghostty hidden"},
                    {.index = 5, .natural = {1030, 530, 432, 324}, .label = "Ghostty small"},
                },
        },
        {
            .name = "default",
            .area = {0, 0, 1728, 1117},
            .windows =
                {
                    {.index = 0, .natural = {40, 60, 1440, 900}, .label = "Browser"},
                    {.index = 1, .natural = {1200, 120, 560, 820}, .label = "Chat"},
                    {.index = 2, .natural = {180, 160, 700, 520}, .label = "Editor"},
                    {.index = 3, .natural = {980, 640, 640, 420}, .label = "Terminal"},
                    {.index = 4, .natural = {90, 760, 420, 320}, .label = "Music"},
                    {.index = 5, .natural = {1480, 740, 320, 240}, .label = "Clock"},
                },
        },
        {
            .name = "stacked",
            .area = {0, 0, 1400, 900},
            .windows =
                {
                    {.index = 0, .natural = {80, 80, 1200, 700}, .label = "Stacked A"},
                    {.index = 1, .natural = {90, 90, 1200, 700}, .label = "Stacked B"},
                    {.index = 2, .natural = {100, 100, 1200, 700}, .label = "Stacked C"},
                    {.index = 3, .natural = {110, 110, 1200, 700}, .label = "Stacked D"},
                },
        },
        {
            .name = "right-biased",
            .area = {0, 0, 1400, 900},
            .windows =
                {
                    {.index = 0, .natural = {900, 100, 360, 260}, .label = "Right top"},
                    {.index = 1, .natural = {980, 430, 360, 260}, .label = "Right bottom"},
                    {.index = 2, .natural = {1220, 270, 240, 240}, .label = "Far right"},
                },
        },
        {
            .name = "workspace-rows",
            .area = {0, 0, 1440, 900},
            .windows =
                {
                    {.index = 0, .natural = {80, 80, 520, 360}, .label = "WS1 left", .rowGroup = 0},
                    {.index = 1, .natural = {620, 120, 500, 330}, .label = "WS1 right", .rowGroup = 0},
                    {.index = 2, .natural = {90, 520, 560, 330}, .label = "WS2 left", .rowGroup = 1},
                    {.index = 3, .natural = {700, 500, 420, 320}, .label = "WS2 right", .rowGroup = 1},
                },
        },
        {
            .name = "extreme-aspect",
            .area = {0, 0, 1126, 692},
            .windows =
                {
                    {.index = 0, .natural = {-143, 143, 1451, 49}, .label = "wide strip 0"},
                    {.index = 1, .natural = {-186, -1, 52, 656}, .label = "tall strip 1"},
                    {.index = 2, .natural = {589, 672, 1488, 72}, .label = "wide strip 2"},
                    {.index = 3, .natural = {335, 316, 104, 773}, .label = "tall strip 3"},
                    {.index = 4, .natural = {873, 230, 1358, 147}, .label = "wide strip 4"},
                    {.index = 5, .natural = {255, 264, 99, 432}, .label = "tall strip 5"},
                    {.index = 6, .natural = {781, 558, 1412, 149}, .label = "wide strip 6"},
                    {.index = 7, .natural = {879, 50, 102, 765}, .label = "tall strip 7"},
                    {.index = 8, .natural = {1076, 571, 938, 74}, .label = "wide strip 8"},
                    {.index = 9, .natural = {-83, -15, 102, 712}, .label = "tall strip 9"},
                    {.index = 10, .natural = {990, 214, 1346, 56}, .label = "wide strip 10"},
                    {.index = 11, .natural = {427, 339, 125, 484}, .label = "tall strip 11"},
                },
        },
        {
            .name = "many-small-24",
            .area = {0, 0, 1249, 815},
            .windows =
                {
                    {.index = 0, .natural = {697, 362, 65, 141}, .label = "small 0"},
                    {.index = 1, .natural = {1057, 666, 130, 131}, .label = "small 1"},
                    {.index = 2, .natural = {-101, 437, 87, 159}, .label = "small 2"},
                    {.index = 3, .natural = {576, -36, 225, 124}, .label = "small 3"},
                    {.index = 4, .natural = {1025, 271, 183, 103}, .label = "small 4"},
                    {.index = 5, .natural = {644, 533, 51, 125}, .label = "small 5"},
                    {.index = 6, .natural = {667, 95, 228, 189}, .label = "small 6"},
                    {.index = 7, .natural = {595, 53, 226, 36}, .label = "small 7"},
                    {.index = 8, .natural = {1042, 360, 231, 64}, .label = "small 8"},
                    {.index = 9, .natural = {380, 214, 81, 206}, .label = "small 9"},
                    {.index = 10, .natural = {378, 297, 166, 121}, .label = "small 10"},
                    {.index = 11, .natural = {720, 191, 27, 208}, .label = "small 11"},
                    {.index = 12, .natural = {358, -22, 58, 100}, .label = "small 12"},
                    {.index = 13, .natural = {164, 361, 80, 162}, .label = "small 13"},
                    {.index = 14, .natural = {42, 17, 222, 125}, .label = "small 14"},
                    {.index = 15, .natural = {231, 670, 108, 150}, .label = "small 15"},
                    {.index = 16, .natural = {529, 528, 201, 173}, .label = "small 16"},
                    {.index = 17, .natural = {641, -71, 77, 74}, .label = "small 17"},
                    {.index = 18, .natural = {1027, 128, 190, 160}, .label = "small 18"},
                    {.index = 19, .natural = {217, 345, 178, 120}, .label = "small 19"},
                    {.index = 20, .natural = {575, 59, 218, 32}, .label = "small 20"},
                    {.index = 21, .natural = {1168, 94, 36, 208}, .label = "small 21"},
                    {.index = 22, .natural = {1120, 36, 35, 110}, .label = "small 22"},
                    {.index = 23, .natural = {915, 1, 235, 79}, .label = "small 23"},
                },
        },
        {
            .name = "mixed-cluster-16",
            .area = {0, 0, 931, 997},
            .windows =
                {
                    {.index = 0, .natural = {462, 626, 574, 88}, .label = "cluster 0"},
                    {.index = 1, .natural = {388, 518, 239, 1008}, .label = "cluster 1"},
                    {.index = 2, .natural = {194, 604, 350, 158}, .label = "cluster 2"},
                    {.index = 3, .natural = {253, 398, 570, 312}, .label = "cluster 3"},
                    {.index = 4, .natural = {575, 440, 329, 684}, .label = "cluster 4"},
                    {.index = 5, .natural = {335, 493, 247, 181}, .label = "cluster 5"},
                    {.index = 6, .natural = {463, 539, 975, 267}, .label = "cluster 6"},
                    {.index = 7, .natural = {324, 429, 249, 826}, .label = "cluster 7"},
                    {.index = 8, .natural = {530, 408, 204, 255}, .label = "cluster 8"},
                    {.index = 9, .natural = {460, 577, 746, 106}, .label = "cluster 9"},
                    {.index = 10, .natural = {442, 332, 126, 949}, .label = "cluster 10"},
                    {.index = 11, .natural = {520, 580, 282, 175}, .label = "cluster 11"},
                    {.index = 12, .natural = {313, 274, 779, 157}, .label = "cluster 12"},
                    {.index = 13, .natural = {622, 600, 106, 953}, .label = "cluster 13"},
                    {.index = 14, .natural = {585, 448, 216, 141}, .label = "cluster 14"},
                    {.index = 15, .natural = {186, 216, 970, 349}, .label = "cluster 15"},
                },
        },
        {
            .name = "flat-band",
            .area = {0, 0, 2560, 1500},
            .windows =
                {
                    {.index = 0, .natural = {40, 520, 720, 360}, .label = "left wide"},
                    {.index = 1, .natural = {620, 430, 760, 160}, .label = "top strip"},
                    {.index = 2, .natural = {760, 660, 560, 320}, .label = "middle doc"},
                    {.index = 3, .natural = {1120, 560, 460, 420}, .label = "calendar"},
                    {.index = 4, .natural = {1260, 760, 560, 380}, .label = "files"},
                    {.index = 5, .natural = {1680, 740, 220, 430}, .label = "phone"},
                    {.index = 6, .natural = {1960, 620, 560, 330}, .label = "right blank"},
                    {.index = 7, .natural = {420, 720, 560, 320}, .label = "terminal"},
                },
        },
        {
            .name = "live-flat-workspaces",
            .area = {0, 0, 2880, 1800},
            .windows =
                {
                    {.index = 0, .natural = {576, 60, 852, 540}, .label = "calendar ws2"},
                    {.index = 1, .natural = {-977, 60, 1193, 828}, .label = "ghostty ws2"},
                    {.index = 2, .natural = {1442, 60, 1188, 828}, .label = "chrome ws2"},
                    {.index = 3, .natural = {12, 60, 1198, 828}, .label = "ghostty ws1"},
                    {.index = 4, .natural = {12, 60, 1416, 828}, .label = "ghostty ws3"},
                    {.index = 5, .natural = {1224, 60, 1193, 828}, .label = "qq ws1"},
                    {.index = 6, .natural = {2431, 60, 1193, 828}, .label = "wechat ws1"},
                    {.index = 7, .natural = {230, 60, 1198, 828}, .label = "dolphin ws2"},
                },
        },
        {
            .name = "live-right-heavy-11",
            .area = {0, 0, 2880, 1800},
            .windows =
                {
                    {.index = 0, .natural = {576, 60, 852, 540}, .label = "calendar special"},
                    {.index = 1, .natural = {12, 60, 1198, 828}, .label = "chrome ws2"},
                    {.index = 2, .natural = {1224, 60, 1193, 828}, .label = "ghostty hyprv ws1"},
                    {.index = 3, .natural = {180, 60, 1080, 225}, .label = "dropdown special"},
                    {.index = 4, .natural = {1224, 60, 1193, 828}, .label = "ghostty hymission ws2"},
                    {.index = 5, .natural = {1070, 60, 360, 828}, .label = "ai hub special"},
                    {.index = 6, .natural = {2431, 60, 1193, 828}, .label = "dolphin screenshots ws1"},
                    {.index = 7, .natural = {12, 60, 1416, 828}, .label = "discord ws4"},
                    {.index = 8, .natural = {1224, 60, 1193, 828}, .label = "dolphin videos ws3"},
                    {.index = 9, .natural = {12, 60, 1198, 828}, .label = "qq ws3"},
                    {.index = 10, .natural = {12, 60, 1198, 828}, .label = "ghostty active ws1"},
                },
        },
    };
}

std::optional<Scene> findScene(const std::string& name) {
    for (auto scene : scenes()) {
        if (scene.name == name)
            return scene;
    }
    return std::nullopt;
}

LayoutConfig demoConfig(const Options& options) {
    LayoutConfig config;
    config.engine = options.engine;
    config.outerPaddingTop = 92.0;
    config.outerPaddingRight = 32.0;
    config.outerPaddingBottom = 32.0;
    config.outerPaddingLeft = 32.0;
    config.rowSpacing = 32.0;
    config.columnSpacing = 32.0;
    config.minPreviewShortEdge = options.minPreviewShortEdge;
    if (options.naturalScaleFlex)
        config.naturalScaleFlex = *options.naturalScaleFlex;
    config.forceRowGroups = options.forceRowGroups;
    config.preserveInputOrder = options.preserveInputOrder || options.rankScaleByInputOrder;
    config.rankScaleByInputOrder = options.rankScaleByInputOrder;
    return config;
}

Rect insetArea(const Rect& area, const LayoutConfig& config) {
    return {
        area.x + config.outerPaddingLeft,
        area.y + config.outerPaddingTop,
        std::max(1.0, area.width - config.outerPaddingLeft - config.outerPaddingRight),
        std::max(1.0, area.height - config.outerPaddingTop - config.outerPaddingBottom),
    };
}

void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --engine grid|natural       Layout solver to run. Default: natural\n"
              << "  --scene NAME                Built-in scene. Default: forceall\n"
              << "  --output PATH.svg           Render an SVG visual diff\n"
              << "  --width PX --height PX      Override scene monitor size\n"
              << "  --min-preview-short-edge PX Minimum preview short edge. Default: 32\n"
              << "  --natural-scale-flex N      Override natural free-scale range\n"
              << "  --stress COUNT              Run random pathological cases and report the worst one\n"
              << "  --seed N                    Seed for --stress. Default: 12648430\n"
              << "  --force-row-groups          Enable row-group fallback behavior\n"
              << "  --preserve-input-order      Preserve input order in row materialization\n"
              << "  --rank-scale-by-input-order Make earlier input windows larger in natural layout\n"
              << "  --list-scenes               Print built-in scene names\n"
              << "  --help                      Show this help\n";
}

Options parseOptions(int argc, char** argv, Scene& scene) {
    Options options;
    std::optional<double> widthOverride;
    std::optional<double> heightOverride;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&](const char* optionName) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << optionName << '\n';
                std::exit(EXIT_FAILURE);
            }
            return argv[++i];
        };

        if (arg == "--engine") {
            options.engine = parseEngine(requireValue("--engine"));
        } else if (arg == "--scene") {
            options.sceneName = requireValue("--scene");
        } else if (arg == "--output" || arg == "-o") {
            options.outputPath = requireValue(arg.c_str());
        } else if (arg == "--width") {
            widthOverride = parseDouble(requireValue("--width"), "--width");
        } else if (arg == "--height") {
            heightOverride = parseDouble(requireValue("--height"), "--height");
        } else if (arg == "--min-preview-short-edge") {
            options.minPreviewShortEdge = parseDouble(requireValue("--min-preview-short-edge"), "--min-preview-short-edge");
        } else if (arg == "--natural-scale-flex") {
            options.naturalScaleFlex = parseDouble(requireValue("--natural-scale-flex"), "--natural-scale-flex");
        } else if (arg == "--stress") {
            options.stressCases = parseSize(requireValue("--stress"), "--stress");
        } else if (arg == "--seed") {
            options.seed = parseUnsigned(requireValue("--seed"), "--seed");
        } else if (arg == "--force-row-groups") {
            options.forceRowGroups = true;
        } else if (arg == "--preserve-input-order") {
            options.preserveInputOrder = true;
        } else if (arg == "--rank-scale-by-input-order") {
            options.rankScaleByInputOrder = true;
        } else if (arg == "--list-scenes") {
            options.listScenes = true;
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            std::exit(EXIT_FAILURE);
        }
    }

    if (options.help || options.listScenes || options.stressCases > 0)
        return options;

    auto found = findScene(options.sceneName);
    if (!found) {
        std::cerr << "Unknown scene: " << options.sceneName << "\n\nAvailable scenes:\n";
        for (const auto& available : scenes())
            std::cerr << "  " << available.name << '\n';
        std::exit(EXIT_FAILURE);
    }

    scene = *found;
    if (widthOverride)
        scene.area.width = *widthOverride;
    if (heightOverride)
        scene.area.height = *heightOverride;

    return options;
}

const WindowInput* findWindow(const std::vector<WindowInput>& windows, std::size_t index) {
    for (const auto& window : windows) {
        if (window.index == index)
            return &window;
    }
    return nullptr;
}

std::string colorFor(std::size_t index) {
    static constexpr std::string_view colors[] = {
        "#5b8def", "#24a36b", "#f59e0b", "#d946ef", "#ef4444", "#14b8a6", "#8b5cf6", "#64748b",
    };
    return std::string(colors[index % std::size(colors)]);
}

double intersectionArea(const Rect& lhs, const Rect& rhs) {
    const double width = std::min(lhs.x + lhs.width, rhs.x + rhs.width) - std::max(lhs.x, rhs.x);
    const double height = std::min(lhs.y + lhs.height, rhs.y + rhs.height) - std::max(lhs.y, rhs.y);
    return std::max(0.0, width) * std::max(0.0, height);
}

double outsideArea(const Rect& rect, const Rect& area) {
    const double insideWidth = std::max(0.0, std::min(rect.x + rect.width, area.x + area.width) - std::max(rect.x, area.x));
    const double insideHeight = std::max(0.0, std::min(rect.y + rect.height, area.y + area.height) - std::max(rect.y, area.y));
    return std::max(0.0, rect.width * rect.height - insideWidth * insideHeight);
}

double centerDistance(const Rect& lhs, const Rect& rhs) {
    return std::hypot(lhs.centerX() - rhs.centerX(), lhs.centerY() - rhs.centerY());
}

std::vector<double> heatCells(const std::vector<WindowSlot>& slots, const Rect& area, int columns = 4, int rows = 3) {
    std::vector<double> heat(static_cast<std::size_t>(columns * rows), 0.0);
    const double        cellWidth = area.width / static_cast<double>(columns);
    const double        cellHeight = area.height / static_cast<double>(rows);
    const double        cellArea = std::max(1.0, cellWidth * cellHeight);

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const Rect cell{
                area.x + static_cast<double>(column) * cellWidth,
                area.y + static_cast<double>(row) * cellHeight,
                cellWidth,
                cellHeight,
            };

            double covered = 0.0;
            for (const auto& slot : slots)
                covered += intersectionArea(slot.target, cell);
            heat[static_cast<std::size_t>(row * columns + column)] = covered / cellArea;
        }
    }

    return heat;
}

LayoutMetrics measureLayout(const std::vector<WindowSlot>& slots, const Rect& area, const std::vector<WindowInput>& windows) {
    LayoutMetrics metrics;
    double        totalTargetArea = 0.0;
    double        weightedX = 0.0;
    double        weightedY = 0.0;
    const double  diagonal = std::max(1.0, std::hypot(area.width, area.height));

    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto* window = findWindow(windows, slots[i].index);
        const double slotArea = slots[i].target.width * slots[i].target.height;
        const double shortEdge = std::min(slots[i].target.width, slots[i].target.height);
        metrics.minScale = std::min(metrics.minScale, slots[i].scale);
        metrics.averageScale += slots[i].scale;
        metrics.minShortEdge = std::min(metrics.minShortEdge, shortEdge);
        metrics.averageShortEdge += shortEdge;
        totalTargetArea += slotArea;
        weightedX += slots[i].target.centerX() * slotArea;
        weightedY += slots[i].target.centerY() * slotArea;
        metrics.edgeMarginLeft = std::min(metrics.edgeMarginLeft, slots[i].target.x - area.x);
        metrics.edgeMarginRight = std::min(metrics.edgeMarginRight, area.x + area.width - (slots[i].target.x + slots[i].target.width));
        metrics.edgeMarginTop = std::min(metrics.edgeMarginTop, slots[i].target.y - area.y);
        metrics.edgeMarginBottom = std::min(metrics.edgeMarginBottom, area.y + area.height - (slots[i].target.y + slots[i].target.height));
        metrics.outOfBoundsArea += outsideArea(slots[i].target, area);
        if (window) {
            const double motion = centerDistance(window->natural, slots[i].target) / diagonal;
            metrics.averageMotion += motion;
            metrics.maxMotion = std::max(metrics.maxMotion, motion);
        }

        for (std::size_t j = i + 1; j < slots.size(); ++j) {
            metrics.overlapArea += intersectionArea(slots[i].target, slots[j].target);
            const auto* lhs = findWindow(windows, slots[i].index);
            const auto* rhs = findWindow(windows, slots[j].index);
            if (!lhs || !rhs)
                continue;

            const double sourceDx = lhs->natural.centerX() - rhs->natural.centerX();
            const double targetDx = slots[i].target.centerX() - slots[j].target.centerX();
            const double sourceDy = lhs->natural.centerY() - rhs->natural.centerY();
            const double targetDy = slots[i].target.centerY() - slots[j].target.centerY();
            if (sourceDx * targetDx < 0.0)
                ++metrics.xInversions;
            if (sourceDy * targetDy < 0.0)
                ++metrics.yInversions;
        }
    }

    if (!slots.empty()) {
        metrics.averageScale /= static_cast<double>(slots.size());
        metrics.averageShortEdge /= static_cast<double>(slots.size());
        metrics.averageMotion /= static_cast<double>(slots.size());
        metrics.minEdgeMargin = std::min({metrics.edgeMarginLeft, metrics.edgeMarginRight, metrics.edgeMarginTop, metrics.edgeMarginBottom});
        metrics.edgeBalanceX = std::abs(metrics.edgeMarginLeft - metrics.edgeMarginRight);
        metrics.edgeBalanceY = std::abs(metrics.edgeMarginTop - metrics.edgeMarginBottom);
    } else {
        metrics.minScale = 0.0;
        metrics.minShortEdge = 0.0;
        metrics.edgeMarginLeft = 0.0;
        metrics.edgeMarginRight = 0.0;
        metrics.edgeMarginTop = 0.0;
        metrics.edgeMarginBottom = 0.0;
        metrics.minEdgeMargin = 0.0;
        metrics.edgeBalanceX = 0.0;
        metrics.edgeBalanceY = 0.0;
    }

    const double areaPixels = std::max(1.0, area.width * area.height);
    metrics.targetAreaRatio = totalTargetArea / areaPixels;
    if (totalTargetArea > 0.0) {
        metrics.targetCentroidX = weightedX / totalTargetArea;
        metrics.targetCentroidY = weightedY / totalTargetArea;
        metrics.gravityOffset = std::hypot(metrics.targetCentroidX - area.centerX(), metrics.targetCentroidY - area.centerY()) / diagonal;
    } else {
        metrics.targetCentroidX = area.centerX();
        metrics.targetCentroidY = area.centerY();
    }

    const auto heat = heatCells(slots, area);
    if (!heat.empty()) {
        double mean = 0.0;
        double left = 0.0;
        double right = 0.0;
        double top = 0.0;
        double bottom = 0.0;

        for (std::size_t i = 0; i < heat.size(); ++i) {
            metrics.heatMax = std::max(metrics.heatMax, heat[i]);
            mean += heat[i];
            const int row = static_cast<int>(i / 4);
            const int column = static_cast<int>(i % 4);
            if (column < 2)
                left += heat[i];
            else
                right += heat[i];
            if (row == 0)
                top += heat[i];
            if (row == 2)
                bottom += heat[i];
        }
        mean /= static_cast<double>(heat.size());

        for (const auto value : heat)
            metrics.heatStdDev += (value - mean) * (value - mean);
        metrics.heatStdDev = std::sqrt(metrics.heatStdDev / static_cast<double>(heat.size()));
        metrics.heatImbalance = std::abs(left - right) + std::abs(top - bottom);
    }

    metrics.score = metrics.overlapArea * 200.0 + metrics.outOfBoundsArea * 200.0;
    if (metrics.minScale < 0.08)
        metrics.score += (0.08 - metrics.minScale) * 1000000.0;
    if (metrics.averageScale < 0.18)
        metrics.score += (0.18 - metrics.averageScale) * 250000.0;
    if (metrics.minShortEdge < 24.0)
        metrics.score += (24.0 - metrics.minShortEdge) * 2500.0;
    if (metrics.averageShortEdge < 44.0)
        metrics.score += (44.0 - metrics.averageShortEdge) * 1200.0;
    if (metrics.targetAreaRatio < 0.12 && slots.size() <= 10)
        metrics.score += (0.12 - metrics.targetAreaRatio) * 100000.0;
    metrics.score += metrics.gravityOffset * 40000.0;
    metrics.score += metrics.heatStdDev * 30000.0;
    metrics.score += metrics.heatImbalance * 2500.0;
    metrics.score += static_cast<double>(metrics.xInversions + metrics.yInversions) * 400.0;
    metrics.score += metrics.averageMotion * 3000.0;

    return metrics;
}

double randomDouble(std::mt19937& rng, double minValue, double maxValue) {
    return std::uniform_real_distribution<double>(minValue, maxValue)(rng);
}

int randomInt(std::mt19937& rng, int minValue, int maxValue) {
    return std::uniform_int_distribution<int>(minValue, maxValue)(rng);
}

WindowInput makeWindow(std::size_t index, Rect natural, std::string label) {
    return {
        .index = index,
        .natural = natural,
        .label = std::move(label),
    };
}

Scene randomStressScene(std::mt19937& rng, std::size_t caseIndex) {
    Scene scene;
    scene.name = "stress-" + std::to_string(caseIndex);
    scene.area = {0, 0, randomDouble(rng, 900.0, 2400.0), randomDouble(rng, 600.0, 1500.0)};

    const int mode = static_cast<int>(caseIndex % 7);
    std::size_t count = 0;
    switch (mode) {
        case 0: count = static_cast<std::size_t>(randomInt(rng, 4, 14)); break;
        case 1: count = static_cast<std::size_t>(randomInt(rng, 3, 8)); break;
        case 2: count = static_cast<std::size_t>(randomInt(rng, 8, 30)); break;
        case 3: count = static_cast<std::size_t>(randomInt(rng, 4, 12)); break;
        case 4: count = static_cast<std::size_t>(randomInt(rng, 3, 10)); break;
        case 5: count = static_cast<std::size_t>(randomInt(rng, 6, 18)); break;
        default: count = static_cast<std::size_t>(randomInt(rng, 4, 16)); break;
    }

    scene.windows.reserve(count);
    const double areaW = scene.area.width;
    const double areaH = scene.area.height;
    const double clusterX = randomDouble(rng, -areaW * 0.25, areaW * 1.05);
    const double clusterY = randomDouble(rng, -areaH * 0.25, areaH * 1.05);

    for (std::size_t i = 0; i < count; ++i) {
        Rect        natural;
        std::string label;

        switch (mode) {
            case 0:
                natural = {
                    randomDouble(rng, -areaW * 0.25, areaW * 1.15),
                    randomDouble(rng, -areaH * 0.25, areaH * 1.15),
                    randomDouble(rng, 60.0, areaW * 0.85),
                    randomDouble(rng, 60.0, areaH * 0.85),
                };
                label = "random";
                break;
            case 1:
                natural = {
                    clusterX + randomDouble(rng, -40.0, 40.0),
                    clusterY + randomDouble(rng, -40.0, 40.0),
                    randomDouble(rng, areaW * 0.45, areaW * 0.95),
                    randomDouble(rng, areaH * 0.35, areaH * 0.85),
                };
                label = "stacked";
                break;
            case 2:
                natural = {
                    randomDouble(rng, -areaW * 0.10, areaW * 0.95),
                    randomDouble(rng, -areaH * 0.10, areaH * 0.95),
                    randomDouble(rng, 24.0, 240.0),
                    randomDouble(rng, 24.0, 220.0),
                };
                label = "many-small";
                break;
            case 3:
                natural = {
                    randomDouble(rng, -areaW * 0.20, areaW),
                    randomDouble(rng, -areaH * 0.20, areaH),
                    i % 2 == 0 ? randomDouble(rng, areaW * 0.75, areaW * 1.35) : randomDouble(rng, 40.0, 180.0),
                    i % 2 == 0 ? randomDouble(rng, 40.0, 150.0) : randomDouble(rng, areaH * 0.55, areaH * 1.20),
                };
                label = "extreme-aspect";
                break;
            case 4:
                natural = {
                    i % 2 == 0 ? randomDouble(rng, -areaW * 0.35, 60.0) : randomDouble(rng, areaW * 0.85, areaW * 1.25),
                    randomDouble(rng, -areaH * 0.20, areaH * 1.05),
                    randomDouble(rng, 160.0, areaW * 0.55),
                    randomDouble(rng, 120.0, areaH * 0.65),
                };
                label = "edge";
                break;
            case 5:
                natural = {
                    randomDouble(rng, areaW * 0.25, areaW * 0.65) + randomDouble(rng, -60.0, 60.0),
                    randomDouble(rng, areaH * 0.25, areaH * 0.65) + randomDouble(rng, -60.0, 60.0),
                    i % 3 == 0 ? randomDouble(rng, areaW * 0.55, areaW * 1.10) : randomDouble(rng, 80.0, 360.0),
                    i % 3 == 1 ? randomDouble(rng, areaH * 0.55, areaH * 1.10) : randomDouble(rng, 80.0, 360.0),
                };
                label = "mixed-cluster";
                break;
            default:
                natural = {
                    randomDouble(rng, areaW * 0.70, areaW * 1.35),
                    randomDouble(rng, areaH * 0.60, areaH * 1.35),
                    randomDouble(rng, 120.0, areaW * 0.55),
                    randomDouble(rng, 100.0, areaH * 0.55),
                };
                label = "offscreen-cloud";
                break;
        }

        label += "-" + std::to_string(i);
        scene.windows.push_back(makeWindow(i, natural, label));
    }

    return scene;
}

void recordMetric(MetricSeries& series, double value) {
    series.values.push_back(value);
    series.sum += value;
    series.min = std::min(series.min, value);
    series.max = std::max(series.max, value);
}

double metricAverage(const MetricSeries& series) {
    if (series.values.empty())
        return 0.0;
    return series.sum / static_cast<double>(series.values.size());
}

double metricPercentile(MetricSeries series, double percentile) {
    if (series.values.empty())
        return 0.0;

    std::sort(series.values.begin(), series.values.end());
    const double rank = std::clamp(percentile, 0.0, 1.0) * static_cast<double>(series.values.size() - 1);
    const auto   lower = static_cast<std::size_t>(std::floor(rank));
    const auto   upper = static_cast<std::size_t>(std::ceil(rank));
    if (lower == upper)
        return series.values[lower];

    const double t = rank - static_cast<double>(lower);
    return series.values[lower] * (1.0 - t) + series.values[upper] * t;
}

void recordStressMetrics(StressSummary& summary, const LayoutMetrics& metrics) {
    ++summary.cases;
    if (metrics.overlapArea > 0.5)
        ++summary.overlapCases;
    if (metrics.outOfBoundsArea > 0.5)
        ++summary.outOfBoundsCases;
    if (metrics.minShortEdge < 24.0)
        ++summary.unreadableShortEdgeCases;

    recordMetric(summary.score, metrics.score);
    recordMetric(summary.gravityOffset, metrics.gravityOffset);
    recordMetric(summary.edgeMarginLeft, metrics.edgeMarginLeft);
    recordMetric(summary.edgeMarginRight, metrics.edgeMarginRight);
    recordMetric(summary.edgeMarginTop, metrics.edgeMarginTop);
    recordMetric(summary.edgeMarginBottom, metrics.edgeMarginBottom);
    recordMetric(summary.minEdgeMargin, metrics.minEdgeMargin);
    recordMetric(summary.edgeBalanceX, metrics.edgeBalanceX);
    recordMetric(summary.edgeBalanceY, metrics.edgeBalanceY);
    recordMetric(summary.heatMax, metrics.heatMax);
    recordMetric(summary.heatStdDev, metrics.heatStdDev);
    recordMetric(summary.heatImbalance, metrics.heatImbalance);
    recordMetric(summary.averageMotion, metrics.averageMotion);
    recordMetric(summary.maxMotion, metrics.maxMotion);
    recordMetric(summary.minShortEdge, metrics.minShortEdge);
    recordMetric(summary.averageShortEdge, metrics.averageShortEdge);
    recordMetric(summary.totalInversions, static_cast<double>(metrics.xInversions + metrics.yInversions));
}

StressReport runStress(const Options& options) {
    std::mt19937          rng(options.seed);
    MissionControlLayout  engine;
    const auto            baseConfig = demoConfig(options);
    StressReport          report;
    report.worst.metrics.score = -1.0;

    for (std::size_t i = 0; i < options.stressCases; ++i) {
        auto scene = randomStressScene(rng, i);
        auto slots = engine.compute(scene.windows, scene.area, baseConfig);
        auto metrics = measureLayout(slots, insetArea(scene.area, baseConfig), scene.windows);
        recordStressMetrics(report.summary, metrics);
        if (metrics.score > report.worst.metrics.score) {
            report.worst = {
                .caseIndex = i,
                .scene = std::move(scene),
                .slots = std::move(slots),
                .metrics = metrics,
            };
        }
    }

    return report;
}

void printMetricSeries(std::string_view name, const MetricSeries& series) {
    std::cout << "summary " << name
              << " min=" << std::fixed << std::setprecision(4) << (series.values.empty() ? 0.0 : series.min)
              << " p01=" << metricPercentile(series, 0.01)
              << " p05=" << metricPercentile(series, 0.05)
              << " avg=" << metricAverage(series)
              << " p50=" << metricPercentile(series, 0.50)
              << " p95=" << metricPercentile(series, 0.95)
              << " p99=" << metricPercentile(series, 0.99)
              << " max=" << series.max
              << '\n';
}

void printStressSummary(const StressSummary& summary) {
    std::cout << "summary cases=" << summary.cases
              << " overlapCases=" << summary.overlapCases
              << " outOfBoundsCases=" << summary.outOfBoundsCases
              << " unreadableShortEdgeCases=" << summary.unreadableShortEdgeCases
              << '\n';
    printMetricSeries("score", summary.score);
    printMetricSeries("gravityOffset", summary.gravityOffset);
    printMetricSeries("edgeMarginLeft", summary.edgeMarginLeft);
    printMetricSeries("edgeMarginRight", summary.edgeMarginRight);
    printMetricSeries("edgeMarginTop", summary.edgeMarginTop);
    printMetricSeries("edgeMarginBottom", summary.edgeMarginBottom);
    printMetricSeries("minEdgeMargin", summary.minEdgeMargin);
    printMetricSeries("edgeBalanceX", summary.edgeBalanceX);
    printMetricSeries("edgeBalanceY", summary.edgeBalanceY);
    printMetricSeries("heatMax", summary.heatMax);
    printMetricSeries("heatStdDev", summary.heatStdDev);
    printMetricSeries("heatImbalance", summary.heatImbalance);
    printMetricSeries("averageMotion", summary.averageMotion);
    printMetricSeries("maxMotion", summary.maxMotion);
    printMetricSeries("minShortEdge", summary.minShortEdge);
    printMetricSeries("averageShortEdge", summary.averageShortEdge);
    printMetricSeries("totalInversions", summary.totalInversions);
}

void printStressResult(const StressResult& result) {
    std::cout << "worst case #" << result.caseIndex << " scene=" << result.scene.name << '\n'
              << "windows=" << result.scene.windows.size() << " area="
              << static_cast<int>(result.scene.area.width) << 'x' << static_cast<int>(result.scene.area.height) << '\n'
              << std::fixed << std::setprecision(3);
    printMetrics(result.metrics);

    for (const auto& window : result.scene.windows) {
        std::cout << "input #" << window.index << ' '
                  << static_cast<int>(window.natural.x) << ','
                  << static_cast<int>(window.natural.y) << ' '
                  << static_cast<int>(window.natural.width) << 'x'
                  << static_cast<int>(window.natural.height) << ' '
                  << window.label << '\n';
    }
    printSlots(result.slots);
}

void writeSvg(const std::string& path, const Scene& scene, const LayoutConfig& config, const std::vector<WindowSlot>& slots) {
    std::ofstream out(path);
    if (!out) {
        std::cerr << "Failed to open output: " << path << '\n';
        std::exit(EXIT_FAILURE);
    }

    const Rect inner = insetArea(scene.area, config);
    out << std::fixed << std::setprecision(2);
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << scene.area.width << "\" height=\"" << scene.area.height << "\" viewBox=\""
        << scene.area.x << ' ' << scene.area.y << ' ' << scene.area.width << ' ' << scene.area.height << "\">\n";
    out << "<defs>\n"
        << "  <filter id=\"shadow\" x=\"-20%\" y=\"-20%\" width=\"140%\" height=\"140%\">\n"
        << "    <feDropShadow dx=\"0\" dy=\"8\" stdDeviation=\"10\" flood-color=\"#0f172a\" flood-opacity=\"0.18\"/>\n"
        << "  </filter>\n"
        << "</defs>\n";
    out << "<rect x=\"" << scene.area.x << "\" y=\"" << scene.area.y << "\" width=\"" << scene.area.width << "\" height=\"" << scene.area.height
        << "\" fill=\"#0f172a\"/>\n";
    out << "<rect x=\"" << scene.area.x << "\" y=\"" << scene.area.y << "\" width=\"" << scene.area.width << "\" height=\"" << scene.area.height
        << "\" fill=\"#e0f2fe\" opacity=\"0.92\"/>\n";
    out << "<rect x=\"" << inner.x << "\" y=\"" << inner.y << "\" width=\"" << inner.width << "\" height=\"" << inner.height
        << "\" fill=\"none\" stroke=\"#0284c7\" stroke-width=\"2\" stroke-dasharray=\"10 8\" opacity=\"0.7\"/>\n";
    out << "<text x=\"" << inner.x << "\" y=\"" << (inner.y - 18) << "\" font-family=\"monospace\" font-size=\"18\" fill=\"#0f172a\">"
        << escapeXml(scene.name) << " / " << engineName(config.engine) << "</text>\n";

    const auto metrics = measureLayout(slots, inner, scene.windows);
    const auto heat = heatCells(slots, inner);
    const double cellWidth = inner.width / 4.0;
    const double cellHeight = inner.height / 3.0;
    for (std::size_t i = 0; i < heat.size(); ++i) {
        const int row = static_cast<int>(i / 4);
        const int column = static_cast<int>(i % 4);
        const double opacity = std::clamp(heat[i] * 0.55, 0.0, 0.32);
        if (opacity <= 0.001)
            continue;
        out << "<rect x=\"" << (inner.x + static_cast<double>(column) * cellWidth) << "\" y=\""
            << (inner.y + static_cast<double>(row) * cellHeight) << "\" width=\"" << cellWidth << "\" height=\"" << cellHeight
            << "\" fill=\"#ef4444\" opacity=\"" << opacity << "\"/>\n";
    }
    out << "<line x1=\"" << inner.centerX() - 14.0 << "\" y1=\"" << inner.centerY() << "\" x2=\"" << inner.centerX() + 14.0 << "\" y2=\""
        << inner.centerY() << "\" stroke=\"#0f172a\" stroke-width=\"2\" opacity=\"0.45\"/>\n";
    out << "<line x1=\"" << inner.centerX() << "\" y1=\"" << inner.centerY() - 14.0 << "\" x2=\"" << inner.centerX() << "\" y2=\""
        << inner.centerY() + 14.0 << "\" stroke=\"#0f172a\" stroke-width=\"2\" opacity=\"0.45\"/>\n";
    out << "<circle cx=\"" << metrics.targetCentroidX << "\" cy=\"" << metrics.targetCentroidY << "\" r=\"9\" fill=\"#f97316\" stroke=\"#7c2d12\" stroke-width=\"2\"/>\n";

    for (const auto& window : scene.windows) {
        const auto color = colorFor(window.index);
        out << "<rect x=\"" << window.natural.x << "\" y=\"" << window.natural.y << "\" width=\"" << window.natural.width << "\" height=\""
            << window.natural.height << "\" rx=\"8\" fill=\"" << color << "\" fill-opacity=\"0.08\" stroke=\"" << color
            << "\" stroke-width=\"2\" stroke-dasharray=\"8 6\"/>\n";
    }

    for (const auto& slot : slots) {
        const auto* window = findWindow(scene.windows, slot.index);
        if (!window)
            continue;

        const auto color = colorFor(slot.index);
        out << "<line x1=\"" << window->natural.centerX() << "\" y1=\"" << window->natural.centerY() << "\" x2=\"" << slot.target.centerX()
            << "\" y2=\"" << slot.target.centerY() << "\" stroke=\"" << color << "\" stroke-width=\"2\" opacity=\"0.35\"/>\n";
    }

    for (const auto& slot : slots) {
        const auto* window = findWindow(scene.windows, slot.index);
        if (!window)
            continue;

        const auto color = colorFor(slot.index);
        out << "<g filter=\"url(#shadow)\">\n";
        out << "  <rect x=\"" << slot.target.x << "\" y=\"" << slot.target.y << "\" width=\"" << slot.target.width << "\" height=\""
            << slot.target.height << "\" rx=\"10\" fill=\"#f8fafc\" stroke=\"" << color << "\" stroke-width=\"3\"/>\n";
        out << "  <rect x=\"" << slot.target.x << "\" y=\"" << slot.target.y << "\" width=\"" << slot.target.width << "\" height=\"26\" rx=\"10\" fill=\""
            << color << "\" opacity=\"0.18\"/>\n";
        out << "  <text x=\"" << (slot.target.x + 10) << "\" y=\"" << (slot.target.y + 19)
            << "\" font-family=\"monospace\" font-size=\"14\" fill=\"#0f172a\">#" << slot.index << " "
            << escapeXml(window->label) << "</text>\n";
        out << "  <text x=\"" << (slot.target.x + 10) << "\" y=\"" << (slot.target.y + slot.target.height - 12)
            << "\" font-family=\"monospace\" font-size=\"13\" fill=\"#334155\">scale=" << std::setprecision(3) << slot.scale
            << std::setprecision(2) << " target=" << static_cast<int>(slot.target.x) << "," << static_cast<int>(slot.target.y) << " "
            << static_cast<int>(slot.target.width) << "x" << static_cast<int>(slot.target.height) << "</text>\n";
        out << "</g>\n";
    }

    out << "<text x=\"" << (inner.x + inner.width - 520) << "\" y=\"" << (inner.y - 18)
        << "\" font-family=\"monospace\" font-size=\"14\" fill=\"#334155\">dashed = source window geometry, solid = solved overview target</text>\n";
    out << "<text x=\"" << inner.x << "\" y=\"" << (inner.y + inner.height + 22)
        << "\" font-family=\"monospace\" font-size=\"14\" fill=\"#334155\">gravity=" << std::setprecision(3) << metrics.gravityOffset
        << " heatMax=" << metrics.heatMax << " heatStdDev=" << metrics.heatStdDev << " heatImbalance=" << metrics.heatImbalance
        << " short(min/avg)=" << std::setprecision(1) << metrics.minShortEdge << "/" << metrics.averageShortEdge
        << " edge(l/r/t/b)=" << metrics.edgeMarginLeft << "/" << metrics.edgeMarginRight << "/" << metrics.edgeMarginTop << "/" << metrics.edgeMarginBottom
        << " edgeBalance(x/y)=" << metrics.edgeBalanceX << "/" << metrics.edgeBalanceY
        << " motion(avg/max)=" << metrics.averageMotion << "/" << metrics.maxMotion << " inv(x/y)=" << metrics.xInversions << "/" << metrics.yInversions
        << "</text>\n";
    out << "</svg>\n";
}

void printSlots(const std::vector<WindowSlot>& slots) {
    for (const auto& slot : slots) {
        std::cout << '#' << slot.index << ' '
                  << std::setw(4) << static_cast<int>(slot.target.x) << ' '
                  << std::setw(4) << static_cast<int>(slot.target.y) << ' '
                  << std::setw(4) << static_cast<int>(slot.target.width) << 'x'
                  << std::setw(4) << static_cast<int>(slot.target.height)
                  << " scale=" << std::fixed << std::setprecision(3) << slot.scale
                  << '\n';
    }
}

void printMetrics(const LayoutMetrics& metrics) {
    std::cout << "metrics"
              << " score=" << std::fixed << std::setprecision(2) << metrics.score
              << " overlapArea=" << metrics.overlapArea
              << " outOfBoundsArea=" << metrics.outOfBoundsArea
              << " minScale=" << std::setprecision(3) << metrics.minScale
              << " averageScale=" << metrics.averageScale
              << " minShortEdge=" << std::setprecision(1) << metrics.minShortEdge
              << " averageShortEdge=" << metrics.averageShortEdge
              << " edgeMargins(l/r/t/b)=" << metrics.edgeMarginLeft << "/" << metrics.edgeMarginRight << "/" << metrics.edgeMarginTop << "/" << metrics.edgeMarginBottom
              << " minEdgeMargin=" << metrics.minEdgeMargin
              << " edgeBalance(x/y)=" << metrics.edgeBalanceX << "/" << metrics.edgeBalanceY
              << " targetAreaRatio=" << metrics.targetAreaRatio
              << " gravityOffset=" << metrics.gravityOffset
              << " centroid=" << static_cast<int>(metrics.targetCentroidX) << "," << static_cast<int>(metrics.targetCentroidY)
              << " heatMax=" << metrics.heatMax
              << " heatStdDev=" << metrics.heatStdDev
              << " heatImbalance=" << metrics.heatImbalance
              << " motion(avg/max)=" << metrics.averageMotion << "/" << metrics.maxMotion
              << " inversions(x/y)=" << metrics.xInversions << "/" << metrics.yInversions
              << '\n';
}

} // namespace

int main(int argc, char** argv) {
    Scene scene;
    const Options options = parseOptions(argc, argv, scene);

    if (options.help) {
        printUsage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (options.listScenes) {
        for (const auto& available : scenes())
            std::cout << available.name << '\n';
        return EXIT_SUCCESS;
    }

    if (options.stressCases > 0) {
        const auto config = demoConfig(options);
        auto       report = runStress(options);
        printStressSummary(report.summary);
        printStressResult(report.worst);
        if (options.outputPath) {
            writeSvg(*options.outputPath, report.worst.scene, config, report.worst.slots);
            std::cout << "wrote " << *options.outputPath << '\n';
        }
        return EXIT_SUCCESS;
    }

    MissionControlLayout engine;
    const LayoutConfig    config = demoConfig(options);
    const auto            slots = engine.compute(scene.windows, scene.area, config);

    printSlots(slots);
    printMetrics(measureLayout(slots, insetArea(scene.area, config), scene.windows));
    if (options.outputPath) {
        writeSvg(*options.outputPath, scene, config, slots);
        std::cout << "wrote " << *options.outputPath << '\n';
    }

    return EXIT_SUCCESS;
}
