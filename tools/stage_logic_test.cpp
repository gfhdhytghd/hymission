#include "stage_logic.hpp"
#include "dispatcher_names.hpp"

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
    const std::vector<hymission::Rect> clickCards{{20, 20, 160, 100}, {20, 140, 160, 100}};
    const hymission::Rect clickStrip{0, 0, 200, 260};
    ok &= expect(activationCardAt(clickCards, clickStrip, 0, 60) == 0 && activationCardAt(clickCards, clickStrip, 199, 180) == 1,
        "card activation extends across strip padding to both horizontal edges");
    ok &= expect(activationCardAt(clickCards, clickStrip, 0, 129.9) == 0 && activationCardAt(clickCards, clickStrip, 0, 130) == 1,
        "card gaps divide at their midpoint without a dead zone");
    ok &= expect(activationCardAt(clickCards, clickStrip, 30, 0) == 0 && activationCardAt(clickCards, clickStrip, 30, 259) == 1,
        "outer vertical strip padding activates the nearest card");
    ok &= expect(!activationCardAt(clickCards, clickStrip, 200, 60) && !activationCardAt(clickCards, clickStrip, -1, 60),
        "activation never leaks into the desktop or another output");
    ok &= expect(activationCardAt({{20, -150, 160, 100}, {20, -20, 160, 100}}, clickStrip, 0, 0) == 1,
        "offscreen cards cannot steal clicks from a partly visible card");
    ok &= expect(activationCardAt({{20, 20, 160, 100}, {20, 70, 160, 100}}, clickStrip, 0, 80) == 1 &&
        !activationCardAt({}, clickStrip, 0, 80), "overlaps retain paint order and empty strips have no target");
    const hymission::Rect focusDesktop{500, 100, 1000, 800};
    ok &= expect(near(visibleAxisFraction({450, 100, 190, 400}, focusDesktop, true), 0.14),
        "follow threshold excludes Stage-covered content from intersection and denominator");
    ok &= expect(near(visibleAxisFraction({1360, 100, 190, 400}, focusDesktop, true), 0.14),
        "right-side viewport uses the same follow threshold");
    ok &= expect(near(visibleAxisFraction({500, 50, 400, 162}, focusDesktop, false), 0.14),
        "vertical follow threshold uses desktop height and top boundary");
    ok &= expect(near(visibleAxisFraction({500, 788, 400, 200}, focusDesktop, false), 0.14),
        "vertical bottom overlap uses desktop height");
    ok &= expect(visibleAxisFraction({0, 100, 200, 200}, focusDesktop, true) == 0 &&
        visibleAxisFraction({1600, 100, 200, 200}, focusDesktop, true) == 0,
        "fully offscreen columns never count negative overlap as visible");
    ok &= expect(visibleAxisFraction({0, 0, 3000, 2000}, focusDesktop, true) == 1 &&
        visibleAxisFraction(focusDesktop, {0, 0, 0, 0}, false) == 0,
        "oversized targets saturate and empty viewports reject visibility");
    ok &= expect(near(visibleAxisFraction({-1550, -700, 190, 400}, {-1500, -700, 1000, 800}, true), 0.14),
        "global monitor offsets do not change follow percentage");
    {
        const hymission::Rect output{-1000, -200, 1000, 800};
        const hymission::Rect work{-980, -170, 950, 740};
        const auto left = holdStrip(output, work, 120, false);
        const auto right = holdStrip(output, work, 120, true);
        ok &= expect(near(left.x, -1000) && near(left.y, -200) && near(left.width, 140) && near(left.height, 800),
                     "left hold strip includes actual Stage width, gap and output reservations");
        ok &= expect(near(right.x, -150) && near(right.width, 150) && near(right.height, 800),
                     "right hold strip reaches the global logical output edge");
        ok &= expect(near(holdStrip(output, work, 200, false).width, 220), "hold width follows dynamic Stage geometry");
        ok &= expect(near(holdStrip(output, work, 9999, false).width, 970), "hold reservation clamps to available work area");
        ok &= expect(hymission::normalizeHymissionDispatcher("stage_open") == "hymission:stage_open" &&
                     hymission::normalizeHymissionDispatcher("hymission.stage_close") == "hymission:stage_close",
                     "idempotent Stage aliases resolve in Lua dispatch routing");
    }
    {
        const hymission::Rect full{-1920, 30, 1920, 1050};
        const hymission::Rect inset{-1680, 30, 1680, 1050};
        const auto hidden = visibilityDesktop(full, inset, 0);
        const auto half = visibilityDesktop(full, inset, 0.5);
        const auto shown = visibilityDesktop(full, inset, 1);
        ok &= expect(near(hidden.x, full.x) && near(hidden.width, full.width), "hidden Stage restores full desktop");
        ok &= expect(near(half.x, -1800) && near(half.width, 1800) && near(half.x + half.width, 0), "finger progress pushes desktop right without moving its right edge");
        ok &= expect(near(shown.x, inset.x) && near(shown.width, inset.width), "fully visible Stage reserves its strip");
        ok &= expect(near(visibilityGestureProgress(0, 150, 300), 0.5) && near(visibilityGestureProgress(1, -150, 300), 0.5), "open and close share continuous progress");
        ok &= expect(near(visibilityGestureProgress(0, -300, 300), 0) && near(visibilityGestureProgress(1, 600, 300), 1), "gesture endpoints clamp without overshoot");
        ok &= expect(near(visibilityGestureProgress(0, 60, 300), 0.2), "reversing accumulated travel reverses the desktop");
    }
    ok &= expect(hymission::normalizeHymissionDispatcher("stage_toggle") == "hymission:stage_toggle" &&
                 hymission::normalizeHymissionDispatcher("hymission.stage_toggle") == "hymission:stage_toggle" &&
                 hymission::normalizeHymissionDispatcher("hymission:stage_toggle") == "hymission:stage_toggle",
                 "Lua Stage action names route to the continuous Stage gesture dispatcher");
    for (const double progress : {0.0, 0.25, 0.5, 1.0, 0.5, 0.0}) {
        const hymission::Rect native{-1440, 30, 1440, 842};
        const auto scrolling = reservedWorkArea(native, 240, progress, false, true);
        ok &= expect(near(scrolling.width, native.width) && near(scrolling.height, native.height) &&
                     near(scrolling.x, native.x + 240 * progress), "scrolling tape translates on advance and reversal without changing its sizing viewport");
        const auto tiled = reservedWorkArea(native, 240, progress, false, false);
        ok &= expect(near(tiled.x + tiled.width, native.x + native.width), "ordinary tiled layout retains its right desktop edge");
        const auto right = reservedWorkArea(native, 240, progress, true, true);
        ok &= expect(near(right.x, native.x) && near(right.width, native.width), "right sidebar clips scrolling columns without resizing them");
    }
    // Lifecycle regression: A's old sidebar must never cover B's predicted
    // sidebar during the zero-progress frame or deferred overview teardown.
    ok &= expect(overviewRenderOwner(OverviewPhase::Inactive, false) == OverviewRenderOwner::Stage,
                 "resting desktop uses live Stage cards");
    for (const bool prepared : {false, true}) {
        ok &= expect(overviewRenderOwner(OverviewPhase::Active, prepared) == OverviewRenderOwner::Overview,
                     "overview owns all gesture frames including zero and reversal");
    }
    ok &= expect(overviewRenderOwner(OverviewPhase::Releasing, false) == OverviewRenderOwner::Overview,
                 "missing prepared exit must not reveal old live Stage cards");
    ok &= expect(overviewRenderOwner(OverviewPhase::Releasing, true) == OverviewRenderOwner::PreparedStage,
                 "deferred teardown shows the same prepared scene as exit endpoints");
    ok &= expect(overviewRenderOwner(OverviewPhase::Inactive, true) == OverviewRenderOwner::Stage,
                 "after synchronous publication live Stage resumes ownership");
    const hymission::Rect viewportBox{200, 50, 800, 500};
    const auto hard = edgeViewport(viewportBox, 0, 0);
    ok &= expect(near(edgeOpacity(hard, 200, 50), 1) && near(edgeOpacity(hard, 999.9, 549.9), 1), "zero widths retain the entire interior");
    for (const auto& point : {std::pair{199.9, 100.0}, {1000.0, 100.0}, {500.0, 49.9}, {500.0, 550.0}})
        ok &= expect(near(edgeOpacity(hard, point.first, point.second), 0), "zero widths NEVER disable hard clipping");
    const auto shadowClip = desktopShadowViewport(hard, {0, 0, 1200, 600});
    ok &= expect(near(edgeOpacity(shadowClip, 500, 45), 1) && near(edgeOpacity(shadowClip, 500, 555), 1),
        "desktop shadows survive above and below work area");
    ok &= expect(near(edgeOpacity(shadowClip, 199, 45), 0) && near(edgeOpacity(shadowClip, 1001, 555), 0) &&
        near(edgeOpacity(shadowClip, 500, -1), 0) && near(edgeOpacity(shadowClip, 500, 601), 0),
        "desktop shadow clip retains horizontal tape and output boundaries");
    ok &= expect(near(hard.box.y, 50) && near(hard.box.height, 500), "shadow clip does not alter flight mapping viewport");
    const auto invalidEdges = edgeViewport(viewportBox, -10, std::numeric_limits<double>::quiet_NaN());
    ok &= expect(near(invalidEdges.left, 0) && near(invalidEdges.right, 0), "invalid effect widths become hard clips");
    const auto wideEdges = edgeViewport(viewportBox, 1e8, 1e8);
    ok &= expect(near(wideEdges.left, 400) && near(wideEdges.right, 400), "large bilateral bands do not overlap");
    const auto soft = edgeViewport(viewportBox, 64, 0);
    const auto mirrored = edgeViewport(viewportBox, 0, 64);
    ok &= expect(near(edgeOpacity(soft, 200, 100), 0) && near(edgeOpacity(soft, 216, 100), 0.5) && near(edgeOpacity(soft, 232, 100), 1) &&
        near(edgeOpacity(soft, 264, 100), 1) && near(edgeOpacity(soft, 999, 100), 1), "desktop fades only at the sidebar boundary");
    for (int d = 1; d < 800; ++d)
        ok &= expect(near(edgeOpacity(soft, 200 + d, 100), edgeOpacity(mirrored, 1000 - d, 100)), "right sidebar mirrors the desktop edge");
    const auto complete = edgeViewportForWindow(soft, {200, 50, 400, 300});
    const auto inset = edgeViewportForWindow(soft, {210, 50, 400, 300});
    const auto clipped = edgeViewportForWindow(soft, {190, 50, 400, 300});
    ok &= expect(complete.left == 0 && inset.left == 0 && clipped.left == soft.left,
        "only crossing windows blur; flush and fully visible window edges stay sharp");
    const auto rightClipped = edgeViewportForWindow(mirrored, {800, 50, 220, 300});
    ok &= expect(rightClipped.right == mirrored.right && edgeViewportForWindow(mirrored, {800, 50, 200, 300}).right == 0,
        "crossing classification mirrors with the sidebar");
    const auto fourEdges = edgeViewport(viewportBox, 64, 64, 40, 40);
    const auto topOnly = edgeViewportForWindow(fourEdges, {250, 40, 300, 200});
    const auto bottomOnly = edgeViewportForWindow(fourEdges, {250, 450, 300, 120});
    const auto corner = edgeViewportForWindow(fourEdges, {190, 40, 200, 200});
    ok &= expect(topOnly.top == 40 && topOnly.bottom == 0 && topOnly.left == 0 && topOnly.right == 0,
        "top overflow enables only the top gradient");
    ok &= expect(bottomOnly.bottom == 40 && bottomOnly.top == 0, "bottom overflow enables only the bottom gradient");
    ok &= expect(corner.top == 40 && corner.left == 64, "corner overflow retains both gradients");
    ok &= expect(edgeViewportForWindow(fourEdges, viewportBox).top == 0 && edgeViewportForWindow(fourEdges, viewportBox).bottom == 0,
        "fully visible vertical edges stay sharp");
    ok &= expect(near(edgeOpacity(topOnly, 500, 50), 0) && near(edgeOpacity(topOnly, 500, 60), 0.5) &&
        near(edgeOpacity(topOnly, 500, 70), 1) && near(edgeOpacity(bottomOnly, 500, 540), 0.5), "vertical opacity ramps mirror");
    const auto tallBands = edgeViewport(viewportBox, 0, 0, 1000, 1000);
    ok &= expect(near(tallBands.top + tallBands.bottom, viewportBox.height), "vertical bands fit viewport height");
    const auto verticalFlight = scrollingFlightFrame(viewportBox, viewportBox, fourEdges, hard, 0.5);
    ok &= expect(near(verticalFlight.viewport.top, 5) && near(verticalFlight.viewport.bottom, 5), "flights ease out vertical bands with window geometry");
    const auto verticalMix = interpolateEdgeFrame({fourEdges, viewportBox}, {hard, viewportBox}, 0.5);
    ok &= expect(near(verticalMix.viewport.top, 20) && near(verticalMix.viewport.bottom, 20), "overview frames interpolate vertical bands");
    const OverviewEndpoint verticalEndpoint{viewportBox, fourEdges, viewportBox};
    const auto verticalReveal = overviewRevealFrame(verticalEndpoint, viewportBox, viewportBox, 1);
    ok &= expect(verticalReveal.viewport.top == 0 && verticalReveal.viewport.bottom == 0, "overview reveal removes vertical bands at completion");
    const auto cardView = edgeViewport({20, 300, 160, 100}, 16, 16);
    const hymission::Rect oversizedDrag{0, 0, 1800, 1000};
    const auto desktopDrag = dragTargetBox(oversizedDrag, viewportBox, 210, 60, 0.2, true);
    ok &= expect(near(desktopDrag.width, 1800) && near(desktopDrag.height, 1000) &&
        near(desktopDrag.centerX(), 210) && near(desktopDrag.centerY(), 60) && desktopDrag.x < viewportBox.x && desktopDrag.y < viewportBox.y,
        "desktop drag retains oversized native geometry centered on the pointer");
    const auto desktopArrival = transitionBox({30, 320, 160, 90}, desktopDrag, 1);
    ok &= expect(near(desktopArrival.x, desktopDrag.x) && near(desktopArrival.y, desktopDrag.y) &&
        near(desktopArrival.width, desktopDrag.width) && near(desktopArrival.height, desktopDrag.height),
        "desktop drag animation ends exactly at the unclamped native handoff geometry");
    const auto cardDrag = dragTargetBox(oversizedDrag, cardView.box, -100, 1000, 0.2, false);
    ok &= expect(cardDrag.x >= cardView.box.x && cardDrag.y >= cardView.box.y &&
        cardDrag.x + cardDrag.width <= cardView.box.x + cardView.box.width + 1e-7 &&
        cardDrag.y + cardDrag.height <= cardView.box.y + cardView.box.height + 1e-7,
        "Stage drag target still fits inside its card");
    // Drag transfers use two stationary clips, with no visible scaling between them.
    const auto sameRect = [](const hymission::Rect& a, const hymission::Rect& b) {
        return near(a.x, b.x) && near(a.y, b.y) && near(a.width, b.width) && near(a.height, b.height);
    };
    const auto visible = [](const ScrollingFlightFrame& f) {
        const auto& w = f.window;
        const auto& v = f.viewport.box;
        return w.x < v.x + v.width && w.x + w.width > v.x && w.y < v.y + v.height && w.y + w.height > v.y;
    };
    for (const bool right : {false, true}) {
        const auto desktop = edgeViewport({200, 50, 800, 500}, right ? 0 : 64, right ? 64 : 0);
        const auto card = edgeViewport({right ? 1020.0 : 20.0, 300, 160, 100}, 16, 16);
        const hymission::Rect large{350, 150, 300, 200};
        const hymission::Rect small{card.box.x + 30, 320, 60, 40};
        for (const bool returning : {false, true}) {
            const auto from = returning ? small : large;
            const auto to = returning ? large : small;
            const auto source = returning ? card : desktop;
            const auto destination = returning ? desktop : card;
            ok &= expect(sameRect(edgeTransferFrame(from, to, source, destination, 0).window, from), "drag starts at the exact source geometry");
            ok &= expect(sameRect(edgeTransferFrame(from, to, source, destination, 1).window, to), "drag finishes at the exact destination geometry");
            ok &= expect(!visible(edgeTransferFrame(from, to, source, destination, 0.5)), "drag changes scale only while fully outside the clip");
            for (int step = 0; step <= 100; ++step) {
                const double p = step / 100.0;
                const auto frame = edgeTransferFrame(from, to, source, destination, p);
                const auto& expected = p < 0.5 ? source : destination;
                const auto& size = p < 0.5 ? from : to;
                ok &= expect(sameRect(frame.viewport.box, expected.box) && near(frame.viewport.left, expected.left) &&
                    near(frame.viewport.right, expected.right), "drag clipping and edge effects stay fixed in their own region");
                ok &= expect(near(frame.window.width, size.width) && near(frame.window.height, size.height), "each drag phase translates without resizing");
                const auto reverse = edgeTransferFrame(frame.window, from, frame.viewport, source, 0);
                ok &= expect(sameRect(reverse.window, frame.window) && sameRect(reverse.viewport.box, frame.viewport.box), "mid-drag reversal retains the sampled geometry and clip");
            }
            const auto exit = edgeTransferFrame(from, to, source, destination, 0.25);
            const auto enter = edgeTransferFrame(from, to, source, destination, 0.75);
            const bool toRight = destination.box.centerX() > source.box.centerX();
            ok &= expect(toRight ? exit.window.x > from.x && enter.window.x < to.x : exit.window.x < from.x && enter.window.x > to.x,
                "both sidebar sides exit and enter through the facing edges");
        }
    }
    for (int step = 0; step <= 100; ++step) {
        const auto hidden = edgeTransferFrame({1100, 100, 200, 100}, {200, 320, 40, 20}, soft, cardView, step / 100.0);
        ok &= expect(!visible(hidden), "drag transfer never exposes hidden scrolling columns");
    }
    const auto sameCard = edgeTransferFrame({30, 310, 30, 30}, {100, 340, 30, 30}, cardView, cardView, 0.5);
    ok &= expect(visible(sameCard) && sameRect(sameCard.viewport.box, cardView.box), "settling within one card does not exit it again");
    const auto lowerCard = edgeViewport({20, 450, 160, 100}, 0, 0);
    const auto cardExit = edgeTransferFrame({30, 310, 30, 30}, {30, 460, 30, 30}, cardView, lowerCard, 0.25);
    const auto cardEnter = edgeTransferFrame({30, 310, 30, 30}, {30, 460, 30, 30}, cardView, lowerCard, 0.75);
    ok &= expect(cardExit.window.y > 310 && cardEnter.window.y < 460, "vertical card transfers use the facing top and bottom edges");
    const auto hardTransfer = edgeTransferFrame({350, 150, 300, 200}, {50, 320, 60, 40}, hard, edgeViewport(cardView.box, 0, 0), 0.75);
    ok &= expect(near(hardTransfer.viewport.left, 0) && near(hardTransfer.viewport.right, 0) && sameRect(hardTransfer.viewport.box, cardView.box),
        "zero blur width preserves the stationary hard clip during drag transfer");
    for (const double offset : {-1.5, -0.3, 0.0, 0.6, 1.1}) {
        const hymission::Rect from{viewportBox.x + offset * viewportBox.width, viewportBox.y + 100, 200, 100};
        const hymission::Rect to{cardView.box.x + offset * cardView.box.width, cardView.box.y + 20, 40, 20};
        for (int step = 0; step <= 100; ++step) {
            const auto frame = scrollingFlightFrame(from, to, soft, cardView, step / 100.0);
            ok &= expect(near((frame.window.x - frame.viewport.box.x) / frame.viewport.box.width, offset), "scrolling flights preserve viewport-relative positions");
            if (offset < -0.25 || offset >= 1)
                ok &= expect(frame.window.x + frame.window.width <= frame.viewport.box.x || frame.window.x >= frame.viewport.box.x + frame.viewport.box.width,
                    "fully offscreen columns never get fitted back into a flying viewport");
            const auto restart = scrollingFlightFrame(frame.window, to, frame.viewport, cardView, 0);
            ok &= expect(near(restart.window.x, frame.window.x) && near(restart.window.width, frame.window.width) &&
                near(restart.viewport.box.x, frame.viewport.box.x) && near(restart.viewport.left, frame.viewport.left), "retarget preserves the displayed window, clip and effect widths");
        }
    }
    ok &= expect(overviewSidebarSlides(true, OverviewPhase::Active), "compact overview retains its independent sliding sidebar");
    ok &= expect(!overviewSidebarSlides(false, OverviewPhase::Active), "forceall never duplicates overview-owned sidebar windows");
    ok &= expect(!overviewSidebarSlides(true, OverviewPhase::Releasing) && !overviewSidebarSlides(true, OverviewPhase::Inactive),
        "sidebar slide yields to the prepared and normal desktop scenes on release");
    for (const bool scrolling : {false, true}) {
        ok &= expect(desktopEdgeApplies(false, true, scrolling, false, false), "unpinned floats clip in every layout");
        ok &= expect(!desktopEdgeApplies(true, true, scrolling, false, false), "pinned floats remain above the strip without clipping");
        ok &= expect(!desktopEdgeApplies(false, true, scrolling, true, false) && !desktopEdgeApplies(false, true, scrolling, false, true),
            "special and fullscreen windows retain native policy");
    }
    ok &= expect(!desktopEdgeApplies(false, false, false, false, false), "non-scrolling tiled windows retain native rendering");
    // Overview starts with exactly the Stage pixels, including a partially
    // clipped tape window and a card clipped by the scrolling sidebar itself.
    const hymission::Rect revealOutput{-1920, 0, 1920, 1080};
    const OverviewEndpoint reveal{{-1940, 100, 200, 160}, edgeViewport({-1900, 80, 240, 200}, 16, 16), {-1900, 120, 240, 160}};
    ok &= expect(overviewEndpointVisible(reveal), "partially clipped Stage windows get a reveal");
    auto invisible = reveal;
    invisible.window.x = -2100;
    invisible.window.width = 200;
    ok &= expect(!overviewEndpointVisible(invisible), "touching the clip edge without pixels is not visible");
    invisible = reveal;
    invisible.clip.height = 0;
    ok &= expect(!overviewEndpointVisible(invisible), "fully scrolled-out cards cannot seed a reveal");
    const auto initial = overviewRevealFrame(reveal, reveal.window, revealOutput, 0);
    ok &= expect(near(initial.viewport.box.x, reveal.viewport.box.x) && near(initial.viewport.left, 16) &&
        near(initial.clip.y, 120) && near(initial.clip.height, 160), "first overview frame retains card and strip clipping and gradient");
    const hymission::Rect fullPreview{-1200, 200, 500, 400};
    const auto opened = overviewRevealFrame(reveal, fullPreview, revealOutput, 1);
    ok &= expect(near(opened.clip.x, revealOutput.x) && near(opened.clip.width, revealOutput.width) &&
        near(opened.viewport.left, 0) && near(opened.viewport.right, 0), "fully open overview reveals the complete window without a gradient");
    const auto almost = overviewRevealFrame(reveal, reveal.window, revealOutput, 0.00001);
    ok &= expect(std::abs(almost.clip.x - initial.clip.x) < 0.001 && std::abs(almost.clip.y - initial.clip.y) < 0.001,
        "opening does not instantly release the cropped window edges");
    for (int step = 0; step <= 100; ++step) {
        const double p = step / 100.0;
        const hymission::Rect current{reveal.window.x + (fullPreview.x - reveal.window.x) * p,
            reveal.window.y + (fullPreview.y - reveal.window.y) * p,
            reveal.window.width + (fullPreview.width - reveal.window.width) * p,
            reveal.window.height + (fullPreview.height - reveal.window.height) * p};
        const auto frame = overviewRevealFrame(reveal, current, revealOutput, p);
        const auto reverse = overviewRevealFrame(reveal, current, revealOutput, 1 - (100 - step) / 100.0);
        ok &= expect(near(frame.clip.x, reverse.clip.x) && near(frame.viewport.left, reverse.viewport.left),
            "gesture reversal follows the same reveal instead of resetting the crop");
        ok &= expect(std::isfinite(frame.clip.x) && frame.clip.width > 0 && frame.clip.height > 0,
            "reveal remains valid throughout scaling on a negatively positioned monitor");
    }
    // The card's outside fade stays at its original x as the window unfolds.
    // Vertical motion and the opposite (desktop seam) edge keep their path.
    for (const bool right : {false, true}) {
        auto baseline = reveal;
        auto preview = fullPreview;
        if (right) {
            const auto mirror = [&](hymission::Rect& r) { r.x = 2 * revealOutput.x + revealOutput.width - r.x - r.width; };
            mirror(baseline.window);
            mirror(baseline.viewport.box);
            mirror(baseline.clip);
            mirror(preview);
        }
        auto anchored = baseline;
        anchored.viewport.fixedOuterEdge = right ? FixedOuterEdge::Right : FixedOuterEdge::Left;
        anchored.viewport.fixedOuterX = right ? baseline.viewport.box.x + baseline.viewport.box.width : baseline.viewport.box.x;
        for (int step = 0; step <= 100; ++step) {
            const double p = step / 100.0;
            const auto moving = overviewRevealFrame(baseline, preview, revealOutput, p);
            const auto fixed = overviewRevealFrame(anchored, preview, revealOutput, p);
            const auto outerX = [right](const hymission::Rect& r) { return right ? r.x + r.width : r.x; };
            const auto seamX = [right](const hymission::Rect& r) { return right ? r.x : r.x + r.width; };
            ok &= expect(near(outerX(fixed.viewport.box), anchored.viewport.fixedOuterX) &&
                near(outerX(fixed.clip), anchored.viewport.fixedOuterX), "outer fade and clip retain card x on both sides");
            ok &= expect(near(seamX(fixed.viewport.box), seamX(moving.viewport.box)) && near(fixed.clip.y, moving.clip.y) &&
                near(fixed.clip.height, moving.clip.height), "pinning outer x preserves seam and vertical reveal");
            const EdgeFrame full{edgeViewport(revealOutput, 0, 0), revealOutput};
            const auto released = interpolateEdgeFrame(fixed, full, p);
            const auto reversed = interpolateEdgeFrame(full, fixed, 1 - p);
            ok &= expect(near(outerX(released.viewport.box), anchored.viewport.fixedOuterX) &&
                near(outerX(reversed.viewport.box), anchored.viewport.fixedOuterX), "release and reverse retain the fixed outer x");
        }
    }
    // Interrupt an opening at 37%, then close to a DIFFERENT workspace card.
    // The first frame must keep the sampled crop, not the new destination crop.
    const auto sampled = overviewRevealFrame(reveal, fullPreview, revealOutput, 0.37);
    const OverviewEndpoint newDestination{{-400, 600, 160, 100}, edgeViewport({-440, 580, 240, 140}, 0, 16), {-440, 600, 200, 100}};
    const auto destinationFrame = overviewRevealFrame(newDestination, fullPreview, revealOutput, 0);
    const auto interruptedReveal = interpolateEdgeFrame(sampled, destinationFrame, overviewTransitionProgress(0.37, 0.37, false));
    ok &= expect(near(interruptedReveal.clip.x, sampled.clip.x) && near(interruptedReveal.clip.width, sampled.clip.width) &&
        near(interruptedReveal.viewport.left, sampled.viewport.left), "interrupt keeps sampled crop when closing destination changes");
    const OverviewEndpoint released{fullPreview, sampled.viewport, sampled.clip};
    const auto releaseStart = overviewRevealFrame(released, fullPreview, revealOutput, 0);
    for (const auto& settleTarget : {destinationFrame, EdgeFrame{edgeViewport(revealOutput, 0, 0), revealOutput}}) {
        const auto first = interpolateEdgeFrame(releaseStart, settleTarget, overviewTransitionProgress(0.37, 0.37, false));
        ok &= expect(near(first.clip.x, sampled.clip.x) && near(first.clip.y, sampled.clip.y) &&
            near(first.clip.width, sampled.clip.width) && near(first.clip.height, sampled.clip.height) &&
            near(first.viewport.box.x, sampled.viewport.box.x) && near(first.viewport.box.y, sampled.viewport.box.y) &&
            near(first.viewport.box.width, sampled.viewport.box.width) && near(first.viewport.box.height, sampled.viewport.box.height) &&
            near(first.viewport.left, sampled.viewport.left) && near(first.viewport.right, sampled.viewport.right),
            "release and layout-settle retain the sampled crop and gradient for both desktop and card destinations");
    }
    const auto finished = interpolateEdgeFrame(sampled, destinationFrame, overviewTransitionProgress(0.37, 0, false));
    ok &= expect(near(finished.clip.x, destinationFrame.clip.x) && near(finished.viewport.right, destinationFrame.viewport.right),
        "interrupted close reaches the new workspace crop");
    ok &= expect(near(overviewTransitionProgress(0.37, 0.37, true), 0) && near(overviewTransitionProgress(0.37, 1, true), 1) &&
        near(overviewTransitionProgress(0.37, 0.685, true), 0.5), "reopening uses only the remaining progress");
    ok &= expect(near(overviewTransitionProgress(0, 0, false), 1) && near(overviewTransitionProgress(1, 1, true), 1),
        "zero-length transitions finish without division by zero");
    // A native close snapshot covers the output, but classification must use
    // each animated body independently, even when several snapshots coexist.
    for (int step = 0; step <= 100; ++step) {
        const double shrink = 1 - step / 200.0;
        const auto closingVisible = edgeViewportForWindow(soft, {300, 100, 200 * shrink, 200 * shrink});
        const auto closingCrossed = edgeViewportForWindow(soft, {190, 100, 200 * shrink, 200 * shrink});
        ok &= expect(near(closingVisible.left, 0) && near(closingCrossed.left, soft.left),
            "simultaneous closing windows retain independent edge classification while shrinking");
    }
    const auto hardFlight = scrollingFlightFrame({0, 50, 100, 100}, {0, 300, 20, 20}, hard, edgeViewport(cardView.box, 0, 0), 0.5);
    ok &= expect(near(hardFlight.viewport.left, 0) && near(hardFlight.viewport.right, 0), "hard clipping survives flight interpolation");
    ok &= expect(previewLayer(false, false, false, false) == PreviewLayer::Tiled,
        "ordinary tiled windows remain visible");
    ok &= expect(previewLayer(false, false, true, false) == PreviewLayer::Floating && PreviewLayer::Tiled < PreviewLayer::Floating,
        "floating previews stay above tiled windows regardless of global list order");
    for (const bool floating : {false, true}) {
        ok &= expect(previewLayer(true, true, floating, false) == PreviewLayer::Fullscreen,
            "both tiled and floating fullscreen owners remain visible");
        ok &= expect(previewLayer(true, false, floating, false) == PreviewLayer::Hidden,
            "fullscreen occlusion excludes underlying surfaces and their blur");
    }
    ok &= expect(previewLayer(true, false, false, true) == PreviewLayer::Hidden,
        "over-fullscreen permission does not raise a tiled sibling");
    ok &= expect(previewLayer(true, false, true, true) == PreviewLayer::Floating && PreviewLayer::Fullscreen < PreviewLayer::Floating,
        "permitted floating windows remain above the fullscreen owner");
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
    // Restore from the bounded sidebar frame to the actual native position,
    // including negative coordinates and destinations larger than the output.
    const auto restoreStart = transitionBoxWithin(rightCard, rightCard, 0, gapBounds);
    for (const hymission::Rect goal : {hymission::Rect{-3300, -90, 800, 600}, hymission::Rect{-200, 1400, 900, 700},
                                      hymission::Rect{-3500, -200, 4000, 2200}}) {
        const auto finalFrame = transitionBox(restoreStart, goal, 1);
        const auto nearEnd = transitionBox(restoreStart, goal, 0.999);
        ok &= expect(sameRect(finalFrame, goal), "floating restore hands off at the native target instead of a fitted edge");
        ok &= expect(std::abs(nearEnd.x - goal.x) < 0.001 && std::abs(nearEnd.y - goal.y) < 0.001 &&
            std::abs(nearEnd.width - goal.width) < 0.001 && std::abs(nearEnd.height - goal.height) < 0.001,
            "floating restore approaches its offscreen target continuously before handoff");
        const auto sample = transitionBox(restoreStart, goal, 0.8);
        const auto restart = transitionBox(sample, goal, 0);
        ok &= expect(sameRect(sample, restart), "retargeting an offscreen floating restore does not fit it back inside the output");
    }
    const hymission::Rect dropCard{-1900, 180, 200, 100}, dropDesktop{-1650, 40, 1600, 800};
    const auto drop = mapDropPoint(dropCard, dropDesktop, -1850, 255);
    for (const auto& card : {dropCard, hymission::Rect{-1900, 180, 200, 150}}) {
        const double scale = std::min(card.width / dropDesktop.width, card.height / dropDesktop.height);
        for (const auto& center : {std::pair{-1850.0, 255.0}, std::pair{-1920.0, 165.0}}) {
            const auto mapped = mapPreviewCenter(card, dropDesktop, center.first, center.second);
            const auto x = card.x + (card.width - dropDesktop.width * scale) / 2 + (mapped.first - dropDesktop.x) * scale;
            const auto y = card.y + (card.height - dropDesktop.height * scale) / 2 + (mapped.second - dropDesktop.y) * scale;
            ok &= expect(near(x, center.first) && near(y, center.second),
                "floating drop preserves preview center including letterboxing and early release outside card");
        }
    }
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
    ok &= expect(staggeredProgress(0.2, 0, 3, false) > 0 && near(staggeredProgress(0.2, 2, 3, false), 0), "top-down reveal starts the top card first");
    ok &= expect(staggeredProgress(0.2, 2, 3, true) > 0 && near(staggeredProgress(0.2, 0, 3, true), 0), "bottom-up reveal reverses the order");
    for (std::size_t i = 0; i < 5; ++i)
        ok &= expect(near(staggeredProgress(0, i, 5, false), 0) && near(staggeredProgress(1, i, 5, false), 1), "all cards share the transition endpoints");
    ok &= expect(near(staggeredProgress(0.5, 0, 1, false), transitionProgress(0.5, 1)), "one visible card uses the normal transition");
    for (std::size_t i = 0; i < 5; ++i) {
        const double held = staggeredProgress(0.6, i, 5, true);
        ok &= expect(staggeredProgress(0.3, i, 5, true) <= held && staggeredProgress(0.9, i, 5, true) >= held,
            "finger reveal advances and reverses with displacement for every card");
    }
    const hymission::Rect hiddenCard{-180, 300, 160, 90};
    const hymission::Rect desktopWindow{240, 120, 1280, 720};
    const auto entering = transitionBox(hiddenCard, desktopWindow, 0);
    const auto entered = transitionBox(hiddenCard, desktopWindow, 1);
    const auto leaving = transitionBox(desktopWindow, hiddenCard, 1);
    ok &= expect(near(entering.x, -180) && near(entering.width, 160) && near(entered.width, 1280) &&
        near(leaving.x, -180) && near(leaving.width, 160), "hidden sidebar endpoints retain thumbnail size and remain outside the output");
    return ok ? 0 : 1;
}
