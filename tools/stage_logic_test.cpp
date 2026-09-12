#include "stage_logic.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }
}

int main() {
    using namespace hymission::stage;
    bool ok = true;
    const Settings defaults{.maxWidth = 240}; // explicit pixel override remains supported
    const auto automatic = layout(1920, 1050, 2, Settings{});
    ok &= expect(near(automatic.cardWidth, 384), "default maximum is one fifth of output width");
    const auto reserved = layout(1800, 1050, 2, Settings{}, std::nullopt, 1920);
    ok &= expect(near(reserved.cardWidth, 384), "side bar reservations do not change output-relative maximum");
    const auto small = layout(1920, 1050, 3, defaults); // 1080 screen, 30px bar
    ok &= expect(small.enabled() && near(small.cardWidth, 240), "few workspaces stop at maximum card width");
    ok &= expect(near(small.reservation, 276), "reservation includes card, padding and desktop gap exactly once");
    ok &= expect(near(small.cardWidth / small.cardHeight, small.desktopWidth / 1050), "thumbnail matches reduced desktop, not output aspect");
    ok &= expect(near(small.maxScroll, 0), "few cards do not scroll");
    const Settings asymmetric{.maxWidth = 240, .padding = 10, .cardGap = 10, .desktopGap = 0,
        .paddingTop = 0, .paddingRight = 0, .paddingBottom = 10};
    const auto edges = layout(1920, 1050, 30, asymmetric);
    ok &= expect(near(edges.cardTop(0, 0), 0) && near(edges.reservation, edges.cardWidth + 10), "native zero top and separate horizontal gaps are preserved");
    ok &= expect(edges.hit(10, 0, 0) == 0 && !edges.hit(9, 0, 0), "hit testing uses distinct top and left margins");
    ok &= expect(near(edges.cardTop(29, edges.maxScroll) + edges.cardHeight, 1040), "scrolling preserves the native bottom margin");

    for (const double width : {640.0, 1080.0, 1536.0, 1920.0, 2560.0}) {
        for (const double height : {480.0, 864.0, 1050.0, 1920.0}) {
            for (std::size_t count = 1; count <= 50; ++count) {
                const auto g = layout(width, height, count, defaults);
                ok &= expect(g.enabled() && g.desktopWidth >= 64, "normal dimensions retain a usable desktop");
                ok &= expect(near(g.cardWidth / g.cardHeight, g.desktopWidth / height), "aspect invariant for horizontal, vertical and fractional-scale logical sizes");
                ok &= expect(g.cardWidth >= 120 && g.cardWidth <= 240, "normal cards stay within configured bounds");
                ok &= expect(near(width, g.reservation + g.desktopWidth), "reservation and desktop partition available width");
                if (g.cardWidth > 120 + 1e-7)
                    ok &= expect(g.maxScroll < 1e-7, "scrolling starts only at minimum width");
                if (g.cardWidth < 240 - 1e-7 && g.cardWidth > 120 + 1e-7)
                    ok &= expect(near(g.cardTop(count - 1, 0) + g.cardHeight + g.padding, height), "adaptive width fills available height");
            }
        }
    }

    const auto many = layout(1920, 1050, 30, defaults);
    ok &= expect(near(many.cardWidth, 120) && many.maxScroll > 0, "minimum width creates vertical overflow");
    const auto lastScroll = many.reveal(29, 0);
    ok &= expect(near(lastScroll, many.maxScroll), "activating last card reveals it");
    ok &= expect(near(many.reveal(0, lastScroll), 0), "activating first card returns to top");
    ok &= expect(many.hit(20, many.cardTop(29, lastScroll) + 10, lastScroll) == 29, "hit test includes scroll offset");
    ok &= expect(!many.hit(20, many.padding + many.cardHeight + 2, 0), "gap is not a workspace");
    ok &= expect(!many.hit(2, 20, 0) && !many.hit(20, 2, 0), "padding is not a workspace");
    ok &= expect(!many.hit(20, 1051, 0), "cards outside viewport cannot be selected");
    ok &= expect(near(many.clampScroll(-100), 0) && near(many.clampScroll(1e8), many.maxScroll), "scroll bounds clamp");

    const auto frozen = layout(1920, 1050, 30, defaults, small.cardWidth);
    ok &= expect(near(frozen.cardWidth, small.cardWidth) && frozen.maxScroll > 0, "drag freezes width while workspaces change");
    const auto empty = layout(1920, 1050, 0, defaults);
    ok &= expect(!empty.enabled() && near(empty.reservation, 0) && near(empty.desktopWidth, 1920), "no visible workspaces releases reservation");
    const auto narrow = layout(150, 400, 3, defaults);
    ok &= expect(narrow.enabled() && narrow.cardWidth < 120 && narrow.desktopWidth >= 64, "tiny output may break minimum card width");
    ok &= expect(!layout(90, 400, 3, defaults).enabled(), "unusable output suspends stage");
    ok &= expect(!layout(1920, 10, 3, defaults).enabled(), "unusable height suspends stage");
    auto invalid = defaults;
    invalid.minWidth = -5;
    invalid.maxWidth = -10;
    invalid.padding = -2;
    invalid.cardGap = std::numeric_limits<double>::quiet_NaN();
    const auto normalized = normalize(invalid);
    ok &= expect(normalized.minWidth == 8 && normalized.maxWidth == 0 && normalized.padding == 0 && normalized.cardGap == 12, "bad configuration is normalized");
    ok &= expect(!layout(std::numeric_limits<double>::infinity(), 1000, 3, defaults).enabled(), "non-finite output dimensions disable layout");

    ok &= expect(coversStrip(CoverMode::Fullscreen, false) && coversStrip(CoverMode::Fullscreen, true), "true fullscreen always covers strip");
    ok &= expect(!coversStrip(CoverMode::Maximized, false) && coversStrip(CoverMode::Maximized, true), "maximization coverage is configurable");
    ok &= expect(!coversStrip(CoverMode::None, true), "ordinary desktop never covers strip");
    ok &= expect(near(previewRounding(-1, 12, 100, 80), 6), "default preview rounding is half system rounding");
    ok &= expect(near(previewRounding(0, 12, 100, 80), 0), "zero explicitly disables rounding");
    ok &= expect(near(previewRounding(9, 12, 100, 80), 9), "explicit preview rounding overrides system value");
    ok &= expect(near(previewRounding(50, 12, 20, 10), 5), "radius is bounded by miniature dimensions");
    const hymission::Rect desktop{2300, 40, 1600, 1000};
    const std::vector<hymission::WindowInput> windows{{.index=0, .natural={2300,40,1200,800}}, {.index=1, .natural={2500,140,600,700}}, {.index=2, .natural={2200,500,900,600}}};
    const auto arranged = arrangeWindows(windows, automatic, desktop);
    ok &= expect(arranged.size() == windows.size(), "all workspace windows receive spatial slots");
    for (std::size_t i = 0; i < arranged.size(); ++i) {
        const auto& slot = arranged[i];
        const auto& box = slot.target;
        ok &= expect(near(box.width / box.height, windows[slot.index].natural.width / windows[slot.index].natural.height), "individual windows retain their own aspect");
        ok &= expect(slot.index == i && near(slot.scale, arranged.front().scale), "stacking order and common scale are preserved");
        ok &= expect(near(box.x - arranged[0].target.x, (windows[i].natural.x - windows[0].natural.x) * slot.scale) &&
                     near(box.y - arranged[0].target.y, (windows[i].natural.y - windows[0].natural.y) * slot.scale), "relative placement survives monitor offset and reserved area");
    }
    ok &= expect(arranged[1].target.x < arranged[0].target.x + arranged[0].target.width, "overlapping windows stay overlapping");
    ok &= expect(arranged[2].target.x < arranged[0].target.x, "off-desktop window is not repositioned to fit");
    ok &= expect(arrangeWindows(windows, automatic, {0, 0, 0, 1000}).empty(), "invalid desktop cannot produce a transform");
    ok &= expect(near(transitionProgress(0, 300), 0) && near(transitionProgress(300, 300), 1), "transition has exact endpoints");
    ok &= expect(near(transitionProgress(150, 300), 0.875), "both flight directions use cubic ease-out");
    ok &= expect(transitionProgress(75, 300) > transitionProgress(150, 300) - transitionProgress(75, 300), "equal time intervals decelerate");
    ok &= expect(near(transitionProgress(-10, 300), 0) && near(transitionProgress(999, 300), 1) && near(transitionProgress(0, 0), 1), "disabled and out-of-range timings are bounded");
    const hymission::Rect miniature{10, 250, 160, 100}, full{320, 10, 1600, 1000};
    const double p = 0.5;
    const auto growing = transitionBox(miniature, full, p);
    const auto shrinking = transitionBox(full, miniature, p);
    const auto cx = [](const hymission::Rect& r) { return r.x + r.width / 2; };
    const auto cy = [](const hymission::Rect& r) { return r.y + r.height / 2; };
    ok &= expect(near((cx(growing) - cx(miniature)) / (cx(full) - cx(miniature)), 0.875) &&
                 near((cy(shrinking) - cy(full)) / (cy(miniature) - cy(full)), 0.5), "growing translation shares scale easing while shrinking retains smoothstep");
    ok &= expect(near((growing.width - miniature.width) / (full.width - miniature.width), 0.875) &&
                 near((shrinking.height - full.height) / (miniature.height - full.height), 0.875), "both scales ease out independently of translation");
    for (int step = 0; step <= 100; ++step) {
        const auto box = transitionBox(miniature, full, step / 100.0);
        for (int x : {0, 1}) for (int y : {0, 1}) {
            const double dx = full.x + x * full.width - miniature.x - x * miniature.width;
            const double dy = full.y + y * full.height - miniature.y - y * miniature.height;
            const double px = box.x + x * box.width - miniature.x - x * miniature.width;
            const double py = box.y + y * box.height - miniature.y - y * miniature.height;
            ok &= expect(std::abs(px * dy - py * dx) < 1e-6, "every growing corner stays on its straight start-to-end segment");
        }
    }
    const hymission::Rect centeredSmall{450, 350, 100, 100}, centeredLarge{0, 0, 1000, 800};
    const auto centered = transitionBox(centeredSmall, centeredLarge, 0.3);
    ok &= expect(near(cx(centered), 500) && near(cy(centered), 400), "scaling about a stationary center does not introduce translation");
    ok &= expect(near(growing.width / growing.height, full.width / full.height) && near(shrinking.width / shrinking.height, full.width / full.height), "flight preserves aspect ratio");
    const auto retargeted = transitionBox(growing, miniature, 0);
    ok &= expect(near(retargeted.x, growing.x) && near(retargeted.width, growing.width), "interrupted transition resumes from its current box without a jump");
    const hymission::Rect flightOutput{-3072, 0, 3072, 1728};
    const hymission::Rect rightCard{-360, 500, 340, 210}, flightDesktop{-3060, 60, 2670, 1650};
    const auto overflowing = transitionBox(rightCard, flightDesktop, 0.25);
    ok &= expect(overflowing.x + overflowing.width <= 0, "straight corner growth no longer overtakes the output edge");
    for (int step = 0; step <= 100; ++step) {
        for (bool incoming : {false, true}) {
            const auto bounded = transitionBoxWithin(incoming ? rightCard : flightDesktop, incoming ? flightDesktop : rightCard, step / 100.0, flightOutput);
            ok &= expect(bounded.x >= flightOutput.x - 1e-6 && bounded.y >= flightOutput.y - 1e-6 &&
                bounded.x + bounded.width <= flightOutput.x + flightOutput.width + 1e-6 &&
                bounded.y + bounded.height <= flightOutput.y + flightOutput.height + 1e-6, "incoming and outgoing edges stay inside their monitor throughout the flight");
        }
    }
    const auto interrupted = transitionBoxWithin(rightCard, flightDesktop, 0.25, flightOutput);
    const hymission::Rect gapBounds{flightOutput.x + 17, flightOutput.y + 23,
        flightOutput.width - 17 - 31, flightOutput.height - 23 - 47};
    for (int step = 0; step <= 100; ++step) {
        for (bool incoming : {false, true}) {
            const auto box = transitionBoxWithin(incoming ? rightCard : flightDesktop,
                incoming ? flightDesktop : rightCard, step / 100.0, gapBounds);
            ok &= expect(box.x >= gapBounds.x - 1e-6 && box.y >= gapBounds.y - 1e-6 &&
                box.x + box.width <= gapBounds.x + gapBounds.width + 1e-6 &&
                box.y + box.height <= gapBounds.y + gapBounds.height + 1e-6,
                "both flight directions preserve asymmetric outer gaps throughout the animation");
            const auto resumed = transitionBoxWithin(box, rightCard, 0, gapBounds);
            ok &= expect(near(box.x, resumed.x) && near(box.y, resumed.y) &&
                near(box.width, resumed.width) && near(box.height, resumed.height),
                "swipe release and retarget retain the gap-constrained frame");
        }
    }
    const auto boundedRetarget = transitionBoxWithin(interrupted, rightCard, 0, flightOutput);
    ok &= expect(near(interrupted.x, boundedRetarget.x) && near(interrupted.y, boundedRetarget.y) &&
        near(interrupted.width, boundedRetarget.width) && near(interrupted.height, boundedRetarget.height), "retargeting preserves an edge-constrained frame");
    const auto leftCard = hymission::Rect{flightOutput.x + 20, 500, 340, 210};
    const auto rightFlightDesktop = hymission::Rect{-2682, 60, 2670, 1650};
    for (int step = 0; step <= 100; ++step) {
        const auto bounded = transitionBoxWithin(leftCard, rightFlightDesktop, step / 100.0, flightOutput);
        ok &= expect(bounded.x >= flightOutput.x - 1e-6 && bounded.x + bounded.width <= 1e-6,
            "mirrored incoming flight also stays inside the left and right output edges");
    }
    const auto oversized = transitionBoxWithin({-4000, -1000, 6000, 4000}, flightDesktop, 0, flightOutput);
    ok &= expect(near(oversized.width / oversized.height, 1.5), "oversized floating flight fits the output without distortion");
    const hymission::Rect dropCard{-1900, 180, 200, 100}, dropDesktop{-1650, 40, 1600, 800};
    const auto drop = mapDropPoint(dropCard, dropDesktop, -1850, 255);
    ok &= expect(near(drop.first, -1250) && near(drop.second, 640), "drop focal point maps both axes across monitor offsets and reserved areas");
    const auto shiftedDrop = mapDropPoint({-1900, 150, 200, 100}, dropDesktop, -1850, 225);
    ok &= expect(near(drop.first, shiftedDrop.first) && near(drop.second, shiftedDrop.second), "animated or scrolled card position does not change the relative drop location");
    const hymission::Rect outputBase{-1920, 40, 1920, 1040};
    const auto leftBand = sidebarArea(outputBase, small, false);
    const auto rightBand = sidebarArea(outputBase, small, true);
    const auto leftDesktop = desktopArea(outputBase, small, true);
    const auto rightDesktop = desktopArea(outputBase, small, false);
    ok &= expect(near(leftBand.x, -1920) && near(rightBand.x + rightBand.width, 0), "both sidebar sides are relative to their own output");
    ok &= expect(near(leftDesktop.x, -1920) && near(rightDesktop.x + rightDesktop.width, 0), "desktop reservation mirrors with the sidebar");
    ok &= expect(leftDesktop.x + leftDesktop.width <= rightBand.x && rightDesktop.x >= leftBand.x + leftBand.width, "desktop and sidebar do not overlap on either side");
    const auto rightDrop = mapDropPoint(rightBand, leftDesktop, rightBand.x + rightBand.width / 2, rightBand.y + rightBand.height / 2);
    ok &= expect(near(rightDrop.first, leftDesktop.x + leftDesktop.width / 2) && near(rightDrop.second, leftDesktop.y + leftDesktop.height / 2), "right sidebar drops map into the left desktop on a negative-coordinate monitor");
    return ok ? 0 : 1;
}
