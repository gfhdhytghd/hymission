#include <cstdlib>
#include <cmath>
#include <iostream>
#include <optional>
#include <vector>

#include "overview_logic.hpp"

namespace {

using hymission::Direction;
using hymission::Rect;
using hymission::WorkspaceStripAnchor;
using hymission::WorkspaceStripReservation;

bool expect(bool condition, const char* message) {
    if (condition)
        return true;

    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool closeEnough(double actual, double expected, double epsilon = 1e-9) {
    return std::abs(actual - expected) <= epsilon;
}

bool expectRect(const Rect& actual, const Rect& expected, const char* message) {
    return expect(closeEnough(actual.x, expected.x) && closeEnough(actual.y, expected.y) && closeEnough(actual.width, expected.width) &&
                      closeEnough(actual.height, expected.height),
                  message);
}

bool expectReservation(const WorkspaceStripReservation& actual, const WorkspaceStripReservation& expected, const char* message) {
    return expectRect(actual.band, expected.band, message) && expectRect(actual.content, expected.content, message);
}

} // namespace

int main() {
    using namespace hymission;

    const std::vector<Rect> rects = {
        {0, 0, 100, 100},
        {140, 0, 100, 100},
        {0, 140, 100, 100},
        {140, 140, 100, 100},
    };

    bool ok = true;

    ok &= expect(hitTest(rects, 50, 50) == std::optional<std::size_t>{0}, "hitTest should find top-left rect");
    ok &= expect(hitTest(rects, 180, 180) == std::optional<std::size_t>{3}, "hitTest should find bottom-right rect");
    ok &= expect(!hitTest(rects, 120, 120).has_value(), "hitTest should miss gap");

    ok &= expect(chooseDirectionalNeighbor(rects, 0, Direction::Right) == std::optional<std::size_t>{1}, "right neighbor from 0 should be 1");
    ok &= expect(chooseDirectionalNeighbor(rects, 0, Direction::Down) == std::optional<std::size_t>{2}, "down neighbor from 0 should be 2");
    ok &= expect(chooseDirectionalNeighbor(rects, 3, Direction::Left) == std::optional<std::size_t>{2}, "left neighbor from 3 should be 2");
    ok &= expect(!chooseDirectionalNeighbor(rects, 0, Direction::Up).has_value(), "up neighbor from top row should be none");

    const Rect middle = lerpRect({0, 0, 100, 100}, {100, 80, 50, 60}, 0.5);
    ok &= expect(middle.x == 50.0 && middle.y == 40.0 && middle.width == 75.0 && middle.height == 80.0, "lerpRect midpoint should be correct");

    ok &= expect(easeOutCubic(0.0) == 0.0, "easeOutCubic(0) should be 0");
    ok &= expect(easeInCubic(1.0) == 1.0, "easeInCubic(1) should be 1");
    ok &= expect(shouldSyncOverviewLiveFocus(true, true, 1), "live focus should sync when overview and Hyprland follow-mouse are enabled");
    ok &= expect(!shouldSyncOverviewLiveFocus(true, true, 0), "live focus should not sync when Hyprland follow-mouse was disabled before opening overview");
    ok &= expect(!shouldSyncOverviewLiveFocus(false, true, 1), "live focus should not sync when overview input handling is inactive");
    ok &= expect(resolveRecommandGestureCommitDirection(0.6, 0.0, 30.0, false) == 1,
                 "positive recommand gestures past halfway should commit to forceall");
    ok &= expect(resolveRecommandGestureCommitDirection(-0.6, 0.0, 30.0, false) == -1,
                 "negative recommand gestures past halfway should commit to compact scope");
    ok &= expect(resolveRecommandGestureCommitDirection(0.8, -40.0, 30.0, false) == 0,
                 "reverse velocity should close forceall instead of jumping directly to compact scope");
    ok &= expect(resolveRecommandGestureCommitDirection(-0.8, 40.0, 30.0, false) == 0,
                 "reverse velocity should close compact scope instead of jumping directly to forceall");
    ok &= expect(resolveRecommandGestureCommitDirection(0.2, 40.0, 30.0, false) == 1,
                 "forward velocity should still be able to commit the current recommand side");

    ok &= expect(resolveOverviewWorkspaceChangeAction(true, false, false, false, true, false) == OverviewWorkspaceChangeAction::Rebuild,
                 "live focus workspace changes should rebuild overview instead of aborting");
    ok &= expect(resolveOverviewWorkspaceChangeAction(true, false, false, false, false, false) == OverviewWorkspaceChangeAction::Abort,
                 "external workspace changes without overview transitions should abort multi-workspace overview");
    ok &= expect(resolveOverviewWorkspaceChangeAction(true, false, true, false, false, false) == OverviewWorkspaceChangeAction::Rebuild,
                 "overview workspace transitions should continue rebuilding state");
    ok &= expect(resolveOverviewWorkspaceChangeAction(true, false, false, true, true, true) == OverviewWorkspaceChangeAction::Ignore,
                 "closing overview should ignore workspace changes");

    ok &= expect(parseWorkspaceStripAnchor("top") == WorkspaceStripAnchor::Top, "top anchor should parse");
    ok &= expect(parseWorkspaceStripAnchor(" LEFT ") == WorkspaceStripAnchor::Left, "left anchor parsing should ignore case and whitespace");
    ok &= expect(parseWorkspaceStripAnchor("Right") == WorkspaceStripAnchor::Right, "right anchor should parse");
    ok &= expect(parseWorkspaceStripAnchor("unexpected") == WorkspaceStripAnchor::Top, "invalid anchors should fall back to top");

    ok &= expect(isWorkspaceStripHorizontal(WorkspaceStripAnchor::Top), "top strip should be horizontal");
    ok &= expect(!isWorkspaceStripHorizontal(WorkspaceStripAnchor::Left), "left strip should be vertical");
    ok &= expect(!isWorkspaceStripHorizontal(WorkspaceStripAnchor::Right), "right strip should be vertical");

    ok &= expectReservation(reserveWorkspaceStripBand({10, 20, 300, 200}, WorkspaceStripAnchor::Top, 40, 12),
                            {
                                {10, 20, 300, 40},
                                {10, 72, 300, 148},
                            },
                            "top strip reservation should reserve band from top edge");
    ok &= expectReservation(reserveWorkspaceStripBand({10, 20, 300, 200}, WorkspaceStripAnchor::Left, 40, 12),
                            {
                                {10, 20, 40, 200},
                                {62, 20, 248, 200},
                            },
                            "left strip reservation should reserve band from left edge");
    ok &= expectReservation(reserveWorkspaceStripBand({10, 20, 300, 200}, WorkspaceStripAnchor::Right, 40, 12),
                            {
                                {270, 20, 40, 200},
                                {10, 20, 248, 200},
                            },
                            "right strip reservation should reserve band from right edge");
    ok &= expectReservation(reserveWorkspaceStripBand({10, 20, 300, 200}, WorkspaceStripAnchor::Top, 0, 24),
                            {
                                {10, 20, 300, 0},
                                {10, 20, 300, 200},
                            },
                            "zero-thickness strips should not reserve content space");
    ok &= expectReservation(reserveWorkspaceStripBand({10, 20, 300, 80}, WorkspaceStripAnchor::Top, 120, 40),
                            {
                                {10, 20, 300, 80},
                                {10, 100, 300, 0},
                            },
                            "strip thickness should clamp to monitor size");

    const auto topSlots = layoutWorkspaceStripSlots({0, 0, 300, 36}, WorkspaceStripAnchor::Top, 3, 15);
    ok &= expect(topSlots.size() == 3, "top strip layout should return one rect per slot");
    ok &= expectRect(topSlots[0], {0, 0, 90, 36}, "top strip first slot should start at left edge");
    ok &= expectRect(topSlots[1], {105, 0, 90, 36}, "top strip second slot should advance along x");
    ok &= expectRect(topSlots[2], {210, 0, 90, 36}, "top strip third slot should end at right edge");

    const auto sideSlots = layoutWorkspaceStripSlots({12, 24, 48, 300}, WorkspaceStripAnchor::Left, 3, 15);
    ok &= expect(sideSlots.size() == 3, "side strip layout should return one rect per slot");
    ok &= expectRect(sideSlots[0], {12, 24, 48, 90}, "side strip first slot should start at top edge");
    ok &= expectRect(sideSlots[1], {12, 129, 48, 90}, "side strip second slot should advance along y");
    ok &= expectRect(sideSlots[2], {12, 234, 48, 90}, "side strip third slot should end at bottom edge");
    ok &= expect(layoutWorkspaceStripSlots({0, 0, 120, 20}, WorkspaceStripAnchor::Top, 0, 10).empty(), "zero-slot strip layout should be empty");

    ok &= expect(hitTestWorkspaceStrip(topSlots, 120, 10) == std::optional<std::size_t>{1}, "strip hit-test should find the matching slot");
    ok &= expect(!hitTestWorkspaceStrip(topSlots, 100, 10).has_value(), "strip hit-test should miss strip gaps");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
