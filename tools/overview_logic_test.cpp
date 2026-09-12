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

    // Different workspaces and scrolling positions make choosing the hovered
    // target for prediction but the original target for commit visibly wrong.
    struct WindowFixture { int workspace; int scrollingX; };
    WindowFixture original{2, 0}, hovered{5, 1200};
    WindowFixture* missing = nullptr;
    WindowFixture selected{2, 2400};
    bool confirmedFocusOk = true;
    confirmedFocusOk &= expect(resolveConfirmedExitFocus(&selected, &hovered, &original, true) == &selected,
                               "explicit activation must use the selected offscreen window, not stale hover focus");
    confirmedFocusOk &= expect(resolveConfirmedExitFocus(&original, &hovered, &original, true) == &original,
                               "explicitly selecting the original window must override a different hover target");
    confirmedFocusOk &= expect(resolveConfirmedExitFocus(&selected, &hovered, &original, false) == &hovered,
                               "normal close must retain preferred hover focus");
    confirmedFocusOk &= expect(resolveConfirmedExitFocus(missing, &hovered, &original, true) == &hovered,
                               "missing selection must fall back to preferred focus");
    confirmedFocusOk &= expect(resolveConfirmedExitFocus(missing, missing, &original, true) == &original,
                               "missing selection and hover must fall back to original focus");
    confirmedFocusOk &= expect(resolveConfirmedExitFocus(missing, missing, missing, true) == nullptr,
                               "empty overview must not invent an activation target");
    if (!confirmedFocusOk)
        return EXIT_FAILURE;

    bool gestureFocusOk = true;
    const auto restored = resolveGestureExitFocus(&original, &hovered, true, true);
    gestureFocusOk &= expect(restored == &original && restored->workspace == 2 && restored->scrollingX == 0,
                             "gesture exit geometry and focus must target the original workspace and scrolling position");
    gestureFocusOk &= expect(resolveGestureExitFocus(&original, &hovered, false, true) == &hovered,
                             "disabled restoration must preserve the hovered target");
    gestureFocusOk &= expect(resolveGestureExitFocus(&original, &hovered, true, false) == &hovered,
                             "unmapped original must fall back to the preferred target");
    gestureFocusOk &= expect(resolveGestureExitFocus(missing, &hovered, true, false) == &hovered,
                             "missing original must fall back to the preferred target");
    gestureFocusOk &= expect(resolveGestureExitFocus(&original, missing, false, true) == &original,
                             "missing preferred target must retain the normal original fallback");
    gestureFocusOk &= expect(resolveGestureExitFocus(missing, missing, true, false) == nullptr,
                             "empty overview must have no exit target");
    if (!gestureFocusOk)
        return EXIT_FAILURE;

    // Regression: ws4 -> ws5 on a 1728px-high monitor used to place the
    // incoming window below the screen until gesture release snapped it back.
    const Rect desktop{391, 60, 2669, 1656};
    bool gestureGeometryOk = expectRect(gestureIncomingWorkspaceEndpoint(desktop, 0, 0), desktop,
                                       "cross-workspace gesture must end on-screen without an incoming slide offset");
    gestureGeometryOk &= expectRect(gestureIncomingWorkspaceEndpoint({391, 1788, 2669, 1656}, 0, 1728), desktop,
                                    "downward native workspace animation must be removed from the gesture endpoint");
    gestureGeometryOk &= expectRect(gestureIncomingWorkspaceEndpoint({391, -1668, 2669, 1656}, 0, -1728), desktop,
                                    "upward native workspace animation must be removed from the gesture endpoint");
    gestureGeometryOk &= expectRect(gestureIncomingWorkspaceEndpoint({3463, 60, 2669, 1656}, 3072, 0), desktop,
                                    "horizontal workspace animation must also end at the settled desktop");
    if (!gestureGeometryOk)
        return EXIT_FAILURE;

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

    ok &= expect(shouldApplyOverviewWindowTransform(true, false), "managed live windows should receive overview transforms");
    ok &= expect(!shouldApplyOverviewWindowTransform(true, true), "closing windows should leave overview transforms before close snapshots are captured");
    ok &= expect(!shouldApplyOverviewWindowTransform(false, false), "unmanaged windows should not receive overview transforms");

    {
        const auto followsMouse = resolveWindowExpansionTargets(1, 2, true, 1.25, 1.5);
        ok &= expect(followsMouse.size() == 1 && followsMouse[0].index == 1 && closeEnough(followsMouse[0].scale, 1.25),
                     "focus-follows-mouse should ignore hover expansion and use selected expansion only");

        const auto independent = resolveWindowExpansionTargets(1, 2, false, 1.25, 1.5);
        ok &= expect(independent.size() == 2 && independent[0].index == 1 && closeEnough(independent[0].scale, 1.25) &&
                         independent[1].index == 2 && closeEnough(independent[1].scale, 1.5),
                     "independent hover should expand selected and hovered windows separately");

        const auto sameWindow = resolveWindowExpansionTargets(1, 1, false, 1.25, 1.5);
        ok &= expect(sameWindow.size() == 1 && sameWindow[0].index == 1 && closeEnough(sameWindow[0].scale, 1.25),
                     "the same selected and hovered window should use selected expansion without stacking scales");

        const auto hoverOnly = resolveWindowExpansionTargets(std::nullopt, 2, false, 1.25, 1.5);
        ok &= expect(hoverOnly.size() == 1 && hoverOnly[0].index == 2 && closeEnough(hoverOnly[0].scale, 1.5),
                     "independent hover should work without a selected target");
    }

    ok &= expect(computePickOrder(rects, {0, 0, 0, 0}) == std::vector<std::size_t>({0, 1, 2, 3}),
                 "pick order should read a single grid in row-major order");
    {
        const std::vector<Rect>        twoMonitorRects = {
            {500, 0, 100, 100}, // monitor 1, row 0
            {0, 0, 100, 100},   // monitor 0, row 0
            {0, 140, 100, 100}, // monitor 0, row 1
        };
        const std::vector<std::size_t> ranks = {1, 0, 0};
        ok &= expect(computePickOrder(twoMonitorRects, ranks) == std::vector<std::size_t>({1, 2, 0}),
                     "pick order should exhaust the lower-ranked monitor before moving to the next");
    }
    {
        const std::vector<Rect>        staggeredRow = {
            {0, 0, 100, 100},
            {140, 40, 100, 100}, // within half-height tolerance of row 0 -> same row
            {0, 260, 100, 100},  // clearly a new row
        };
        const std::vector<std::size_t> ranks = {0, 0, 0};
        ok &= expect(computePickOrder(staggeredRow, ranks) == std::vector<std::size_t>({0, 1, 2}),
                     "pick order should cluster near-aligned previews into the same row before sorting by x");
    }
    ok &= expect(computePickOrder({}, {}).empty(), "pick order should be empty for no windows");

    ok &= expect(computePickLabel(0) == "1", "pick label 0 should be \"1\"");
    ok &= expect(computePickLabel(8) == "9", "pick label 8 should be \"9\"");
    ok &= expect(computePickLabel(9) == "A1", "pick label 9 should roll over to \"A1\"");
    ok &= expect(computePickLabel(17) == "A9", "pick label 17 should be \"A9\"");
    ok &= expect(computePickLabel(18) == "B1", "pick label 18 should roll over to \"B1\"");
    ok &= expect(computePickLabel(9 + 26 * 9 - 1) == "Z9", "pick label at the last addressable slot should be \"Z9\"");
    ok &= expect(computePickLabel(9 + 26 * 9).empty(), "pick label beyond the addressable range should be empty");

    ok &= expect(computePickOrderIndex(1, std::nullopt) == 0, "digit-only pick order index 1 should map to 0");
    ok &= expect(computePickOrderIndex(9, std::nullopt) == 8, "digit-only pick order index 9 should map to 8");
    ok &= expect(computePickOrderIndex(1, 0) == 9, "letter group A digit 1 should map to order index 9");
    ok &= expect(computePickOrderIndex(9, 0) == 17, "letter group A digit 9 should map to order index 17");
    ok &= expect(computePickOrderIndex(1, 1) == 18, "letter group B digit 1 should map to order index 18");
    ok &= expect(!pickLetterGroupAvailable(9, 0), "letter group A should not be armed before an A label exists");
    ok &= expect(pickLetterGroupAvailable(10, 0), "letter group A should be armed when A1 exists");
    ok &= expect(!pickLetterGroupAvailable(18, 1), "letter group B should not be armed before B1 exists");
    ok &= expect(pickLetterGroupAvailable(19, 1), "letter group B should be armed when B1 exists");
    ok &= expect(!pickLetterGroupAvailable(243, 26), "letter groups outside A-Z should be rejected");

    ok &= expect(parsePickLabelsMode("spatial") == PickLabelsMode::Spatial, "spatial pick-label mode should parse");
    ok &= expect(parsePickLabelsMode("SPATIAL") == PickLabelsMode::Spatial, "spatial pick-label mode should be case-insensitive");
    ok &= expect(parsePickLabelsMode("unknown") == PickLabelsMode::Sequential, "unknown pick-label modes should preserve sequential behavior");
    ok &= expect(parseGroupedWindowsPolicy("expanded") == GroupedWindowsPolicy::Expanded, "expanded grouped-window policy should parse");

    const auto composedQuery = normalizedSearchText("ÉDITEUR");
    ok &= expect(windowMatchesSearch("e\xCC\x81" "diteur de texte", "org.example.Editor", composedQuery),
                 "search should normalize canonically equivalent Unicode and case-fold it");
    ok &= expect(windowMatchesSearch("Terminal", "org.gnome.Console", normalizedSearchText("CONSOLE")),
                 "search should match window class case-insensitively");
    ok &= expect(windowMatchesSearch("Terminal", "org.gnome.Console", {}), "an empty search should match every window");
    ok &= expect(!windowMatchesSearch("Terminal", "org.gnome.Console", normalizedSearchText("browser")),
                 "an unrelated query should produce no match");

    constexpr uint32_t SHIFT = 1U << 0;
    constexpr uint32_t CAPS = 1U << 1;
    constexpr uint32_t CTRL = 1U << 2;
    constexpr uint32_t ALT = 1U << 3;
    constexpr uint32_t SUPER = 1U << 6;
    constexpr uint32_t COMMAND_MODIFIERS = CTRL | ALT | SUPER;
    ok &= expect(overviewTextInputAllowed(0, COMMAND_MODIFIERS), "plain keys should be eligible for label/search input");
    ok &= expect(overviewTextInputAllowed(SHIFT | CAPS, COMMAND_MODIFIERS), "Shift and Caps Lock should remain valid text modifiers");
    ok &= expect(!overviewTextInputAllowed(CTRL, COMMAND_MODIFIERS), "Ctrl shortcuts must pass through overview label handling");
    ok &= expect(!overviewTextInputAllowed(ALT, COMMAND_MODIFIERS), "Alt shortcuts must pass through overview label handling");
    ok &= expect(!overviewTextInputAllowed(SUPER, COMMAND_MODIFIERS), "Super shortcuts must pass through overview label handling");
    ok &= expect(parseGroupedWindowsPolicy(" COLLAPSED ") == GroupedWindowsPolicy::Collapsed, "collapsed grouped-window policy should trim and ignore case");
    ok &= expect(parseGroupedWindowsPolicy("invalid") == GroupedWindowsPolicy::Expanded, "invalid grouped-window policy should fall back to expanded");
    {
        const std::vector<GroupProjectionInput> grouped = {{0, false}, {11, false}, {11, true}, {22, true}, {22, false}, {0, false}};
        ok &= expect(projectGroupedWindowIndices(grouped, GroupedWindowsPolicy::Expanded) == std::vector<std::size_t>({0, 1, 2, 3, 4, 5}),
                     "expanded grouped-window projection should preserve every candidate");
        ok &= expect(projectGroupedWindowIndices(grouped, GroupedWindowsPolicy::Collapsed) == std::vector<std::size_t>({0, 2, 3, 5}),
                     "collapsed grouped-window projection should keep standalone windows and one current member per group");
    }
    ok &= expect(hitTestEqualSegments({0, 0, 300, 30}, 3, 150, 15) == std::optional<std::size_t>{1}, "equal-segment hit testing should select the middle tab");
    ok &= expect(!hitTestEqualSegments({0, 0, 300, 30}, 3, 301, 15), "equal-segment hit testing should reject points outside the bar");
    ok &= expect(shouldSuppressCollapsedGroupMember(11, 11, 101, 102), "collapsed group siblings should stay out of the normal render pass");
    ok &= expect(!shouldSuppressCollapsedGroupMember(11, 11, 101, 101), "the collapsed group member bound to the overview item should render");
    ok &= expect(!shouldSuppressCollapsedGroupMember(11, 22, 101, 102), "members of another group should remain unaffected");
    ok &= expect(closeEnough(resolveExpandedGroupEffectiveAlpha(0.0F, 0.72F, true, false, true, false), 0.72, 1e-6),
                 "expanded grouped windows should bypass Hyprland's zero effective-alpha render gate");
    ok &= expect(closeEnough(resolveExpandedGroupEffectiveAlpha(0.0F, 0.72F, true, false, true, true), 0.0, 1e-6),
                 "collapsed grouped windows should retain Hyprland's effective alpha");
    ok &= expect(closeEnough(resolveExpandedGroupEffectiveAlpha(0.0F, 0.72F, true, true, true, false), 0.0, 1e-6),
                 "raw snapshots should retain Hyprland's effective alpha");
    ok &= expect(closeEnough(resolveExpandedGroupEffectiveAlpha(0.35F, 0.72F, true, false, false, false), 0.35, 1e-6),
                 "ordinary overview windows should retain Hyprland's effective alpha");
    ok &= expect(closeEnough(resolveExpandedGroupEffectiveAlpha(0.35F, 0.72F, false, false, true, false), 0.35, 1e-6),
                 "normal desktop rendering should remain unchanged");
    ok &= expect(shouldRefreshDraggedCompositeTexture(true, false), "multi-surface drag should capture its first composited frame");
    ok &= expect(!shouldRefreshDraggedCompositeTexture(true, true), "multi-surface drag should keep its first composite rigid while moving");
    ok &= expect(!shouldRefreshDraggedCompositeTexture(false, false), "single-surface drag should not enter the composite capture path");
    {
        const Rect preview = {100, 100, 900, 500};
        const Rect labels = floatingSegmentBarRect(preview, 2);
        ok &= expect(closeEnough(labels.centerX(), preview.centerX()) && closeEnough(labels.width, 256),
                     "collapsed group labels should use a compact centered width");
        ok &= expect(labels.y + labels.height <= preview.y, "collapsed group labels should not cover the preview decoration");
        const Rect crowded = floatingSegmentBarRect({0, 100, 240, 120}, 8);
        ok &= expect(crowded.width <= 224 && crowded.x >= 8, "crowded collapsed group labels should stay inside the preview width");
    }
    {
        const std::vector<Rect> groupRects = {{0, 0, 200, 100}, {300, 0, 100, 200}, {0, 300, 160, 120}};
        const auto stack = stackedGroupPreviewRects(groupRects, 1, 500, 400, 0.5, 0.5, 0.65, 10.0, 12.0);
        ok &= expect(stack.size() == 3 && closeEnough(stack[1].centerX(), 500) && closeEnough(stack[1].centerY(), 400),
                     "group drag stack should keep the grabbed member under the pointer");
        ok &= expect(closeEnough(stack[0].width, 130) && closeEnough(stack[2].height, 78), "group drag stack should scale every member uniformly");
        ok &= expect(stack[0].x - (stack[1].centerX() - stack[0].width * 0.5) <= 12.0 &&
                         stack[2].x - (stack[1].centerX() - stack[2].width * 0.5) <= 12.0,
                     "group drag stack spread should stay within the configured cap");
    }
    ok &= expect(spatialPickKeys().size() == 47, "spatial pick mode should expose the full physical ANSI alphanumeric and punctuation area");

    const auto spatialF = spatialPickKeyIndex('F');
    const auto spatialR = spatialPickKeyIndex('R');
    const auto spatialT = spatialPickKeyIndex('T');
    const auto spatialC = spatialPickKeyIndex('C');
    const auto spatialV = spatialPickKeyIndex('V');
    const auto spatialD = spatialPickKeyIndex('D');
    const auto spatialG = spatialPickKeyIndex('G');
    const auto spatialSlash = spatialPickKeyIndex('/');
    const auto spatialBracket = spatialPickKeyIndex('[');
    ok &= expect(spatialF && spatialR && spatialT && spatialC && spatialV && spatialD && spatialG && spatialSlash && spatialBracket,
                 "spatial key lookup should find QWERTY and punctuation keys");
    ok &= expect(spatialPickDirectionForKeys(*spatialF, *spatialF) == SpatialPickDirection::Center, "repeating F should select the center route");
    ok &= expect(spatialPickDirectionForKeys(*spatialF, *spatialR) == SpatialPickDirection::Up, "FR should be an upward route");
    ok &= expect(spatialPickDirectionForKeys(*spatialF, *spatialT) == SpatialPickDirection::Up, "FT should be an alternate upward neighbor");
    ok &= expect(spatialPickDirectionForKeys(*spatialF, *spatialC) == SpatialPickDirection::Down, "FC should be a downward route");
    ok &= expect(spatialPickDirectionForKeys(*spatialF, *spatialV) == SpatialPickDirection::Down, "FV should be an alternate downward neighbor");
    ok &= expect(spatialPickDirectionForKeys(*spatialF, *spatialD) == SpatialPickDirection::Left, "FD should be a left route");
    ok &= expect(spatialPickDirectionForKeys(*spatialF, *spatialG) == SpatialPickDirection::Right, "FG should be a right route");
    ok &= expect(!spatialPickDirectionForKeys(*spatialF, *spatialPickKeyIndex('1')), "non-adjacent second keys should not resolve a direction");

    const auto twoWindowSpatial = computeSpatialPickMap({{0.25, 0.5}, {0.75, 0.5}});
    std::string leftSpatialKeys;
    std::string rightSpatialKeys;
    for (std::size_t keyIndex = 0; keyIndex < spatialPickKeys().size(); ++keyIndex) {
        const auto windowIndex = resolveSpatialPickPrimary(twoWindowSpatial, keyIndex);
        if (!windowIndex)
            continue;
        (*windowIndex == 0 ? leftSpatialKeys : rightSpatialKeys).push_back(spatialPickKeys()[keyIndex].label);
    }
    ok &= expect(leftSpatialKeys.find('1') != std::string::npos && rightSpatialKeys.find('/') != std::string::npos,
                 "two horizontal windows should map left and right physical-key areas, including punctuation");
    ok &= expect(leftSpatialKeys.size() + rightSpatialKeys.size() == spatialPickKeys().size(),
                 "two horizontal windows should assign every physical punctuation and alphanumeric key exactly once");
    ok &= expect(twoWindowSpatial.routes.size() == 2 && twoWindowSpatial.routes[0].direction == SpatialPickDirection::Center &&
                     twoWindowSpatial.routes[1].direction == SpatialPickDirection::Center &&
                     twoWindowSpatial.routes[0].primaryKeyIndex != twoWindowSpatial.routes[1].primaryKeyIndex,
                 "two windows should receive distinct single-key center routes");

    std::vector<SpatialPickPoint> allKeyCenters;
    for (const auto& key : spatialPickKeys())
        allKeyCenters.push_back({key.x, key.y});
    const auto fullSingleKeySpatial = computeSpatialPickMap(allKeyCenters);
    std::vector<bool> uniquePrimary(spatialPickKeys().size(), false);
    bool              allCenterRoutes = fullSingleKeySpatial.routes.size() == spatialPickKeys().size();
    for (const auto& route : fullSingleKeySpatial.routes) {
        allCenterRoutes &= route.direction == SpatialPickDirection::Center && route.primaryKeyIndex < uniquePrimary.size() && !uniquePrimary[route.primaryKeyIndex];
        if (route.primaryKeyIndex < uniquePrimary.size())
            uniquePrimary[route.primaryKeyIndex] = true;
    }
    ok &= expect(allCenterRoutes, "up to 47 windows should each receive a unique single-key route");

    SpatialPickMap sharedSpatial;
    sharedSpatial.routes = {
        {.windowIndex = 0, .primaryKeyIndex = *spatialF, .direction = SpatialPickDirection::Center, .canonicalSecondaryKeyIndex = *spatialF},
        {.windowIndex = 1, .primaryKeyIndex = *spatialF, .direction = SpatialPickDirection::Up, .canonicalSecondaryKeyIndex = *spatialR},
    };
    ok &= expect(spatialPickRouteCount(sharedSpatial, *spatialF) == 2, "a shared primary should expose both assigned routes");
    ok &= expect(resolveSpatialPickChord(sharedSpatial, *spatialF, *spatialF) == std::optional<std::size_t>{0}, "FF should resolve the center window");
    ok &= expect(resolveSpatialPickChord(sharedSpatial, *spatialF, *spatialR) == std::optional<std::size_t>{1}, "FR should resolve the upper window");
    ok &= expect(resolveSpatialPickChord(sharedSpatial, *spatialF, *spatialT) == std::optional<std::size_t>{1},
                 "an alternate adjacent upper key should resolve the FR route");
    ok &= expect(!resolveSpatialPickChord(sharedSpatial, *spatialF, *spatialG), "an unassigned adjacent direction should not resolve");

    std::vector<SpatialPickPoint> overflowSpatialCenters(300, {0.5, 0.5});
    const auto                    overflowSpatial = computeSpatialPickMap(overflowSpatialCenters);
    ok &= expect(overflowSpatial.routes.size() >= 47 && overflowSpatial.routes.size() < overflowSpatialCenters.size(),
                 "windows beyond the available spatial routes should remain unassigned without losing the base key map");

    const auto defaultToggle = parseToggleArguments("");
    ok &= expect(defaultToggle && defaultToggle->scope.empty() && defaultToggle->direction == ToggleDirection::Forward,
                 "toggle arguments should default to forward config scope");
    const auto reverseToggle = parseToggleArguments("reverse");
    ok &= expect(reverseToggle && reverseToggle->scope.empty() && reverseToggle->direction == ToggleDirection::Reverse,
                 "toggle arguments should support reverse direction");
    const auto scopedReverseToggle = parseToggleArguments("forceall, reverse");
    ok &= expect(scopedReverseToggle && scopedReverseToggle->scope == "forceall" && scopedReverseToggle->direction == ToggleDirection::Reverse,
                 "toggle arguments should combine a scope with reverse direction");
    ok &= expect(!parseToggleArguments("unknown").has_value(), "toggle arguments should reject unknown values");
    ok &= expect(!parseToggleArguments("reverse,reverse").has_value(), "toggle arguments should reject duplicate direction values");
    ok &= expect(!parseToggleArguments("forceall,").has_value(), "toggle arguments should reject empty trailing values");
    ok &= expect(legacyFullscreenDispatcherArguments("", "") == std::optional<std::string>{"0 toggle"},
                 "Lua fullscreen arguments should default to fullscreen toggle");
    ok &= expect(legacyFullscreenDispatcherArguments("maximized", "toggle") == std::optional<std::string>{"1 toggle"},
                 "Lua fullscreen arguments should map maximized toggle to the legacy dispatcher");
    ok &= expect(legacyFullscreenDispatcherArguments("FULLSCREEN", "set") == std::optional<std::string>{"0 set"},
                 "Lua fullscreen arguments should accept case-insensitive mode and actions");
    ok &= expect(legacyFullscreenDispatcherArguments("1", "unset") == std::optional<std::string>{"1 unset"},
                 "Lua fullscreen arguments should accept numeric mode aliases");
    ok &= expect(!legacyFullscreenDispatcherArguments("invalid", "toggle").has_value(),
                 "Lua fullscreen arguments should reject unknown modes");
    ok &= expect(!legacyFullscreenDispatcherArguments("fullscreen", "invalid").has_value(),
                 "Lua fullscreen arguments should reject unknown actions");

    const Rect middle = lerpRect({0, 0, 100, 100}, {100, 80, 50, 60}, 0.5);
    ok &= expect(middle.x == 50.0 && middle.y == 40.0 && middle.width == 75.0 && middle.height == 80.0, "lerpRect midpoint should be correct");

    ok &= expect(easeOutCubic(0.0) == 0.0, "easeOutCubic(0) should be 0");
    ok &= expect(easeInCubic(1.0) == 1.0, "easeInCubic(1) should be 1");
    ok &= expect(easeInOutCubic(0.0) == 0.0, "easeInOutCubic(0) should be 0");
    ok &= expect(easeInOutCubic(0.5) == 0.5, "easeInOutCubic midpoint should be 0.5");
    ok &= expect(easeInOutCubic(1.0) == 1.0, "easeInOutCubic(1) should be 1");
    ok &= expect(parseHoverRelayoutCurve("linear") == HoverRelayoutCurve::Linear, "hover curve parser should accept linear");
    ok &= expect(parseHoverRelayoutCurve("EASE-IN-CUBIC") == HoverRelayoutCurve::EaseInCubic, "hover curve parser should accept case-insensitive dash form");
    ok &= expect(parseHoverRelayoutCurve("ease_in_out_cubic") == HoverRelayoutCurve::EaseInOutCubic, "hover curve parser should accept ease_in_out_cubic");
    ok &= expect(parseHoverRelayoutCurve("unknown") == HoverRelayoutCurve::EaseOutCubic, "hover curve parser should fall back to ease_out_cubic");
    ok &= expect(applyHoverRelayoutCurve(HoverRelayoutCurve::Linear, 0.25) == 0.25, "linear hover curve should preserve progress");
    ok &= expect(applyHoverRelayoutCurve(HoverRelayoutCurve::EaseInCubic, 0.5) < 0.5, "ease-in hover curve should start slowly");
    ok &= expect(applyHoverRelayoutCurve(HoverRelayoutCurve::EaseOutCubic, 0.5) > 0.5, "ease-out hover curve should start quickly");
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
    ok &= expect(closeEnough(niriScrollingPreviewCellLength(320.0, 200.0), 320.0),
                 "niri scrolling preview cell should use the larger primary length");
    ok &= expect(closeEnough(niriScrollingPreviewCellLength(120.0, 200.0), 200.0),
                 "niri scrolling preview cell should preserve full fallback width");
    ok &= expect(closeEnough(niriScrollingPreviewAdvance(320.0, 200.0, 24.0), 344.0),
                 "niri scrolling preview advance should add configured gap");
    ok &= expect(closeEnough(niriScrollingPreviewAdvance(320.0, 200.0, -24.0), 320.0),
                 "negative niri scrolling preview gap should clamp to zero");
    ok &= expect(closeEnough(niriOverviewPreviewScale({0, 0, 900, 500}, {0, 0, 3000, 500}, 0.95, 0.10, GestureAxis::Horizontal), 0.95),
                 "horizontal niri overview should ignore scrolling-tape width when fitting scale");
    ok &= expect(closeEnough(niriOverviewPreviewScale({0, 0, 900, 500}, {0, 0, 3000, 500}, 0.95, 0.10), 0.30),
                 "normal niri overview fitting should still account for both axes");
    ok &= expect(closeEnough(niriOverviewPreviewScale({0, 0, 900, 500}, {0, 0, 900, 2400}, 0.95, 0.10, GestureAxis::Vertical), 0.95),
                 "vertical niri overview should ignore scrolling-tape height when fitting scale");
    ok &= expect(closeEnough(niriOverviewPreviewScale({0, 0, 900, 360}, {0, 0, 900, 500}, 0.95, 0.10, GestureAxis::Horizontal), 0.72),
                 "niri overview should still shrink when the cross axis cannot fit the configured max scale");

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
