#include <algorithm>
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
    bool                     forceRowGroups = false;
    bool                     listScenes = false;
    bool                     help = false;
};

struct LayoutMetrics {
    double overlapArea = 0.0;
    double outOfBoundsArea = 0.0;
    double minScale = std::numeric_limits<double>::infinity();
    double averageScale = 0.0;
    double targetAreaRatio = 0.0;
    double score = 0.0;
};

struct StressResult {
    std::size_t              caseIndex = 0;
    Scene                    scene;
    std::vector<WindowSlot>  slots;
    LayoutMetrics            metrics;
};

void printSlots(const std::vector<WindowSlot>& slots);

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
    config.forceRowGroups = options.forceRowGroups;
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
              << "  --stress COUNT              Run random pathological cases and report the worst one\n"
              << "  --seed N                    Seed for --stress. Default: 12648430\n"
              << "  --force-row-groups          Enable row-group fallback behavior\n"
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
        } else if (arg == "--stress") {
            options.stressCases = parseSize(requireValue("--stress"), "--stress");
        } else if (arg == "--seed") {
            options.seed = parseUnsigned(requireValue("--seed"), "--seed");
        } else if (arg == "--force-row-groups") {
            options.forceRowGroups = true;
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

LayoutMetrics measureLayout(const std::vector<WindowSlot>& slots, const Rect& area) {
    LayoutMetrics metrics;
    double        totalTargetArea = 0.0;

    for (std::size_t i = 0; i < slots.size(); ++i) {
        metrics.minScale = std::min(metrics.minScale, slots[i].scale);
        metrics.averageScale += slots[i].scale;
        totalTargetArea += slots[i].target.width * slots[i].target.height;
        metrics.outOfBoundsArea += outsideArea(slots[i].target, area);

        for (std::size_t j = i + 1; j < slots.size(); ++j)
            metrics.overlapArea += intersectionArea(slots[i].target, slots[j].target);
    }

    if (!slots.empty()) {
        metrics.averageScale /= static_cast<double>(slots.size());
    } else {
        metrics.minScale = 0.0;
    }

    const double areaPixels = std::max(1.0, area.width * area.height);
    metrics.targetAreaRatio = totalTargetArea / areaPixels;
    metrics.score = metrics.overlapArea * 200.0 + metrics.outOfBoundsArea * 200.0;
    if (metrics.minScale < 0.08)
        metrics.score += (0.08 - metrics.minScale) * 1000000.0;
    if (metrics.averageScale < 0.18)
        metrics.score += (0.18 - metrics.averageScale) * 250000.0;
    if (metrics.targetAreaRatio < 0.12 && slots.size() <= 10)
        metrics.score += (0.12 - metrics.targetAreaRatio) * 100000.0;

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

StressResult runStress(const Options& options) {
    std::mt19937          rng(options.seed);
    MissionControlLayout  engine;
    const auto            baseConfig = demoConfig(options);
    StressResult          worst;
    worst.metrics.score = -1.0;

    for (std::size_t i = 0; i < options.stressCases; ++i) {
        auto scene = randomStressScene(rng, i);
        auto slots = engine.compute(scene.windows, scene.area, baseConfig);
        auto metrics = measureLayout(slots, insetArea(scene.area, baseConfig));
        if (metrics.score > worst.metrics.score) {
            worst = {
                .caseIndex = i,
                .scene = std::move(scene),
                .slots = std::move(slots),
                .metrics = metrics,
            };
        }
    }

    return worst;
}

void printStressResult(const StressResult& result) {
    std::cout << "worst case #" << result.caseIndex << " scene=" << result.scene.name << '\n'
              << "windows=" << result.scene.windows.size() << " area="
              << static_cast<int>(result.scene.area.width) << 'x' << static_cast<int>(result.scene.area.height) << '\n'
              << "score=" << std::fixed << std::setprecision(2) << result.metrics.score
              << " overlapArea=" << result.metrics.overlapArea
              << " outOfBoundsArea=" << result.metrics.outOfBoundsArea
              << " minScale=" << std::setprecision(3) << result.metrics.minScale
              << " averageScale=" << result.metrics.averageScale
              << " targetAreaRatio=" << result.metrics.targetAreaRatio << '\n';

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
        auto       worst = runStress(options);
        printStressResult(worst);
        if (options.outputPath) {
            writeSvg(*options.outputPath, worst.scene, config, worst.slots);
            std::cout << "wrote " << *options.outputPath << '\n';
        }
        return EXIT_SUCCESS;
    }

    MissionControlLayout engine;
    const LayoutConfig    config = demoConfig(options);
    const auto            slots = engine.compute(scene.windows, scene.area, config);

    printSlots(slots);
    if (options.outputPath) {
        writeSvg(*options.outputPath, scene, config, slots);
        std::cout << "wrote " << *options.outputPath << '\n';
    }

    return EXIT_SUCCESS;
}
