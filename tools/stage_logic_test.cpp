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
    const Settings defaults;
    const auto small = layout(1920, 1050, 3, defaults); // 1080 screen, 30px bar
    ok &= expect(small.enabled() && near(small.cardWidth, 240), "few workspaces stop at maximum card width");
    ok &= expect(near(small.reservation, 276), "reservation includes card, padding and desktop gap exactly once");
    ok &= expect(near(small.cardWidth / small.cardHeight, small.desktopWidth / 1050), "thumbnail matches reduced desktop, not output aspect");
    ok &= expect(near(small.maxScroll, 0), "few cards do not scroll");

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
    ok &= expect(normalized.minWidth == 8 && normalized.maxWidth == 8 && normalized.padding == 0 && normalized.cardGap == 12, "bad configuration is normalized");
    ok &= expect(!layout(std::numeric_limits<double>::infinity(), 1000, 3, defaults).enabled(), "non-finite output dimensions disable layout");

    ok &= expect(coversStrip(CoverMode::Fullscreen, false) && coversStrip(CoverMode::Fullscreen, true), "true fullscreen always covers strip");
    ok &= expect(!coversStrip(CoverMode::Maximized, false) && coversStrip(CoverMode::Maximized, true), "maximization coverage is configurable");
    ok &= expect(!coversStrip(CoverMode::None, true), "ordinary desktop never covers strip");
    return ok ? 0 : 1;
}
