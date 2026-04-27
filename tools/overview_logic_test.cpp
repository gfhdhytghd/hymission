#include <cstdlib>
#include <cmath>
#include <iostream>
#include <optional>
#include <vector>

#include "overview_logic.hpp"

namespace {

using hymission::Direction;
using hymission::GestureAxis;
using hymission::HymissionScrollMode;
using hymission::Rect;
using hymission::RecommandVisibleGestureMode;
using hymission::ScrollingLayoutDirection;
using hymission::WorkspaceStripAnchor;
using hymission::WorkspaceStripEmptyMode;
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
    ok &= expect(!chooseCyclicIndex(0, 0).has_value(), "cyclic selection should be empty for zero windows");
    ok &= expect(!chooseCyclicIndex(1, 0).has_value(), "cyclic selection should be empty for one window");
    ok &= expect(chooseCyclicIndex(4, 0) == std::optional<std::size_t>{1}, "cyclic selection should advance to the next window");
    ok &= expect(chooseCyclicIndex(4, 3) == std::optional<std::size_t>{0}, "cyclic selection should wrap from the end to the start");
    ok &= expect(chooseCyclicIndex(4, 0, -1) == std::optional<std::size_t>{3}, "cyclic selection should support reverse wrapping");

    const Rect middle = lerpRect({0, 0, 100, 100}, {100, 80, 50, 60}, 0.5);
    ok &= expect(middle.x == 50.0 && middle.y == 40.0 && middle.width == 75.0 && middle.height == 80.0, "lerpRect midpoint should be correct");

    ok &= expect(easeOutCubic(0.0) == 0.0, "easeOutCubic(0) should be 0");
    ok &= expect(easeInCubic(1.0) == 1.0, "easeInCubic(1) should be 1");
    ok &= expect(shouldSyncOverviewLiveFocus(true, true, 1), "live focus should sync when overview and Hyprland follow-mouse are enabled");
    ok &= expect(!shouldSyncOverviewLiveFocus(true, true, 0), "live focus should not sync when Hyprland follow-mouse was disabled before opening overview");
    ok &= expect(!shouldSyncOverviewLiveFocus(false, true, 1), "live focus should not sync when overview input handling is inactive");

    ok &= expect(resolveRecommandVisibleGestureMode(1, -1) == RecommandVisibleGestureMode::TransferCapable,
                 "visible forceall swipes toward compact should allow recommand transfer");
    ok &= expect(resolveRecommandVisibleGestureMode(-1, 1) == RecommandVisibleGestureMode::TransferCapable,
                 "visible compact swipes toward forceall should allow recommand transfer");
    ok &= expect(resolveRecommandVisibleGestureMode(1, 1) == RecommandVisibleGestureMode::CloseOnly,
                 "visible forceall swipes in the other direction should only close");
    ok &= expect(resolveRecommandVisibleGestureMode(-1, -1) == RecommandVisibleGestureMode::CloseOnly,
                 "visible compact swipes in the other direction should only close");

    ok &= expect(resolveOverviewGestureCommit(true, 0.6, 0.0, 30.0, false),
                 "opening gestures past halfway should commit");
    ok &= expect(!resolveOverviewGestureCommit(true, 0.4, -40.0, 30.0, false),
                 "reverse velocity should cancel opening gestures");
    ok &= expect(resolveOverviewGestureCommit(false, 0.4, 40.0, 30.0, false),
                 "close gestures past halfway should commit to hidden");
    ok &= expect(!resolveOverviewGestureCommit(false, 0.6, -40.0, 30.0, false),
                 "reverse velocity should cancel close gestures");

    ok &= expect(resolveRecommandGestureCommitDirection(0.6, true, 0.0, 30.0, false) == 1,
                 "positive recommand gestures past halfway should commit to forceall");
    ok &= expect(resolveRecommandGestureCommitDirection(-0.6, true, 0.0, 30.0, false) == -1,
                 "negative recommand gestures past halfway should commit to compact scope");
    ok &= expect(resolveRecommandGestureCommitDirection(0.8, true, -40.0, 30.0, false) == 0,
                 "reverse velocity should close forceall instead of jumping directly to compact scope");
    ok &= expect(resolveRecommandGestureCommitDirection(-0.8, true, 40.0, 30.0, false) == -1,
                 "opening compact with forward velocity should still commit the current recommand side");
    ok &= expect(resolveRecommandGestureCommitDirection(-0.8, false, 40.0, 30.0, false) == 0,
                 "reverse velocity should close compact scope instead of jumping directly to forceall");
    ok &= expect(resolveRecommandGestureCommitDirection(0.8, false, -40.0, 30.0, false) == 1,
                 "reverse reopening velocity should still be able to keep the current visible recommand side");

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
    ok &= expect(parseWorkspaceStripEmptyMode("existing") == WorkspaceStripEmptyMode::Existing, "existing empty-mode should parse");
    ok &= expect(parseWorkspaceStripEmptyMode(" Continuous ") == WorkspaceStripEmptyMode::Continuous,
                 "continuous empty-mode parsing should ignore case and whitespace");
    ok &= expect(parseWorkspaceStripEmptyMode("unexpected") == WorkspaceStripEmptyMode::Existing, "invalid empty-mode should fall back to existing");

    ok &= expect(parseHymissionScrollMode("layout") == std::optional<HymissionScrollMode>{HymissionScrollMode::Layout}, "layout niri scroll mode should parse");
    ok &= expect(!parseHymissionScrollMode(" workspace ").has_value(), "workspace should stay on Hyprland's standard workspace gesture");
    ok &= expect(!parseHymissionScrollMode("Both").has_value(), "combined layout/workspace scroll mode should not parse");
    ok &= expect(!parseHymissionScrollMode("unexpected").has_value(), "invalid niri scroll mode should fail");

    ok &= expect(parseScrollingLayoutDirection("left") == ScrollingLayoutDirection::Left, "left scrolling direction should parse");
    ok &= expect(parseScrollingLayoutDirection("DOWN") == ScrollingLayoutDirection::Down, "down scrolling direction parsing should ignore case");
    ok &= expect(parseScrollingLayoutDirection(" up ") == ScrollingLayoutDirection::Up, "up scrolling direction parsing should ignore whitespace");
    ok &= expect(parseScrollingLayoutDirection("unexpected") == ScrollingLayoutDirection::Right, "invalid scrolling direction should fall back to right");

    ok &= expect(axisForScrollingLayoutDirection(ScrollingLayoutDirection::Right) == GestureAxis::Horizontal, "right scrolling direction should use horizontal gestures");
    ok &= expect(axisForScrollingLayoutDirection(ScrollingLayoutDirection::Left) == GestureAxis::Horizontal, "left scrolling direction should use horizontal gestures");
    ok &= expect(axisForScrollingLayoutDirection(ScrollingLayoutDirection::Down) == GestureAxis::Vertical, "down scrolling direction should use vertical gestures");
    ok &= expect(axisForScrollingLayoutDirection(ScrollingLayoutDirection::Up) == GestureAxis::Vertical, "up scrolling direction should use vertical gestures");
    ok &= expect(scrollingLayoutGestureAxisMatches(ScrollingLayoutDirection::Down, GestureAxis::Vertical),
                 "vertical gestures should match down scrolling layouts");
    ok &= expect(!scrollingLayoutGestureAxisMatches(ScrollingLayoutDirection::Down, GestureAxis::Horizontal),
                 "horizontal gestures should not match down scrolling layouts");
    ok &= expect(closeEnough(scrollingLayoutMoveAmount(ScrollingLayoutDirection::Right, -12.0, 2.0), -24.0),
                 "right scrolling move amount should preserve gesture sign");
    ok &= expect(closeEnough(scrollingLayoutMoveAmount(ScrollingLayoutDirection::Left, -12.0, 2.0), 24.0),
                 "left scrolling move amount should invert gesture sign");
    ok &= expect(closeEnough(scrollingLayoutMoveAmount(ScrollingLayoutDirection::Down, 5.0, 3.0), 15.0),
                 "down scrolling move amount should preserve gesture sign");
    ok &= expect(closeEnough(scrollingLayoutMoveAmount(ScrollingLayoutDirection::Up, 5.0, 3.0), -15.0),
                 "up scrolling move amount should invert gesture sign");
    ok &= expect(closeEnough(scrollingLayoutMoveAmount(ScrollingLayoutDirection::Right, 5.0, -3.0), 0.0),
                 "negative niri scroll sensitivity should clamp to zero");

    ok &= expect(isWorkspaceStripHorizontal(WorkspaceStripAnchor::Top), "top strip should be horizontal");
    ok &= expect(!isWorkspaceStripHorizontal(WorkspaceStripAnchor::Left), "left strip should be vertical");
    ok &= expect(!isWorkspaceStripHorizontal(WorkspaceStripAnchor::Right), "right strip should be vertical");

    ok &= expect(expandWorkspaceStripWorkspaceIds({1, 5, 9}, WorkspaceStripEmptyMode::Existing) == std::vector<int64_t>({1, 5, 9}),
                 "existing empty-mode should keep sparse workspace ids");
    ok &= expect(expandWorkspaceStripWorkspaceIds({9, 1, 5, 5}, WorkspaceStripEmptyMode::Continuous) == std::vector<int64_t>({1, 2, 5, 6, 9}),
                 "continuous empty-mode should progressively expose numeric gaps");
    ok &= expect(expandWorkspaceStripWorkspaceIds({1, 1000}, WorkspaceStripEmptyMode::Continuous) == std::vector<int64_t>({1, 2, 1000}),
                 "continuous empty-mode should not expand large numeric spans into one slot per id");
    ok &= expect(expandWorkspaceStripWorkspaceIds({-1337, 1}, WorkspaceStripEmptyMode::Continuous) == std::vector<int64_t>({-1337, 1}),
                 "continuous empty-mode should not expand named workspace ids");
    ok &= expect(expandWorkspaceStripWorkspaceIds({}, WorkspaceStripEmptyMode::Continuous).empty(),
                 "empty workspace id sets should stay empty");

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

    const auto niriTopSlots = layoutNiriWorkspaceStripSlots({0, 0, 500, 80}, WorkspaceStripAnchor::Top, 3, std::optional<std::size_t>{1}, 10, 8, 2.0);
    ok &= expect(niriTopSlots.size() == 3, "niri top strip layout should return one rect per slot");
    ok &= expectRect(niriTopSlots[0], {48, 8, 128, 64}, "niri top strip should keep active slot centered");
    ok &= expectRect(niriTopSlots[1], {186, 8, 128, 64}, "niri top strip active slot should be centered in the band");
    ok &= expectRect(niriTopSlots[2], {324, 8, 128, 64}, "niri top strip should advance by workspace aspect plus gap");

    const auto niriSideSlots = layoutNiriWorkspaceStripSlots({0, 0, 100, 500}, WorkspaceStripAnchor::Left, 3, std::optional<std::size_t>{1}, 10, 8, 2.0);
    ok &= expect(niriSideSlots.size() == 3, "niri side strip layout should return one rect per slot");
    ok &= expectRect(niriSideSlots[0], {8, 177, 84, 42}, "niri side strip should use monitor aspect thumbnails");
    ok &= expectRect(niriSideSlots[1], {8, 229, 84, 42}, "niri side strip active slot should be centered in the band");
    ok &= expectRect(niriSideSlots[2], {8, 281, 84, 42}, "niri side strip should advance vertically by thumbnail height plus gap");

    const auto niriScaledSideSlots =
        layoutNiriWorkspaceStripSlots({0, 0, 1000, 1000}, WorkspaceStripAnchor::Left, 3, std::optional<std::size_t>{1}, 50, 0, 16.0 / 9.0, 0.5);
    ok &= expect(niriScaledSideSlots.size() == 3, "niri scaled strip layout should return one rect per slot");
    ok &= expectRect(niriScaledSideSlots[0], {250, 28.125, 500, 281.25}, "niri scaled strip should keep monitor ratio at configured strip scale");
    ok &= expectRect(niriScaledSideSlots[1], {250, 359.375, 500, 281.25}, "niri scaled strip should center active workspace without fitting all slots");

    const auto niriOverflowSlots =
        layoutNiriWorkspaceStripSlots({0, 0, 500, 80}, WorkspaceStripAnchor::Top, 6, std::optional<std::size_t>{3}, 10, 8, 2.0);
    ok &= expect(niriOverflowSlots.size() == 6, "niri overflowing strip layout should keep all slots");
    ok &= expectRect(niriOverflowSlots[0], {-228, 8, 128, 64}, "niri overflowing strip should allow slots before the screen");
    ok &= expectRect(niriOverflowSlots[3], {186, 8, 128, 64}, "niri overflowing strip should keep active slot centered");
    ok &= expectRect(niriOverflowSlots[5], {462, 8, 128, 64}, "niri overflowing strip should allow slots after the screen");

    ok &= expect(hitTestWorkspaceStrip(topSlots, 120, 10) == std::optional<std::size_t>{1}, "strip hit-test should find the matching slot");
    ok &= expect(!hitTestWorkspaceStrip(topSlots, 100, 10).has_value(), "strip hit-test should miss strip gaps");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
