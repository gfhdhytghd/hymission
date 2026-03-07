#include <cstdlib>
#include <iostream>
#include <optional>
#include <vector>

#include "overview_logic.hpp"

namespace {

using hymission::Direction;
using hymission::Rect;

bool expect(bool condition, const char* message) {
    if (condition)
        return true;

    std::cerr << "FAIL: " << message << '\n';
    return false;
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

    ok &= expect(resolveOverviewWorkspaceChangeAction(true, false, false, false, true, false) == OverviewWorkspaceChangeAction::Rebuild,
                 "live focus workspace changes should rebuild overview instead of aborting");
    ok &= expect(resolveOverviewWorkspaceChangeAction(true, false, false, false, false, false) == OverviewWorkspaceChangeAction::Abort,
                 "external workspace changes without overview transitions should abort multi-workspace overview");
    ok &= expect(resolveOverviewWorkspaceChangeAction(true, false, true, false, false, false) == OverviewWorkspaceChangeAction::Rebuild,
                 "overview workspace transitions should continue rebuilding state");
    ok &= expect(resolveOverviewWorkspaceChangeAction(true, false, false, true, true, true) == OverviewWorkspaceChangeAction::Ignore,
                 "closing overview should ignore workspace changes");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
