#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "mission_layout.hpp"
#include "overview_layout_session.hpp"

namespace {

using hymission::LayoutConfig;
using hymission::MissionControlLayout;
using hymission::Rect;
using hymission::WindowInput;
using hymission::WindowSlot;

bool expect(bool condition, const char* message) {
    if (condition)
        return true;

    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool closeEnough(double actual, double expected, double epsilon = 1e-6) {
    return std::abs(actual - expected) <= epsilon;
}

bool rectsOverlap(const Rect& lhs, const Rect& rhs) {
    return lhs.x < rhs.x + rhs.width && lhs.x + lhs.width > rhs.x && lhs.y < rhs.y + rhs.height && lhs.y + lhs.height > rhs.y;
}

bool rectInside(const Rect& rect, const Rect& area) {
    constexpr double epsilon = 0.0001;
    return rect.x + epsilon >= area.x && rect.y + epsilon >= area.y &&
           rect.x + rect.width <= area.x + area.width + epsilon &&
           rect.y + rect.height <= area.y + area.height + epsilon;
}

bool expectSlot(const WindowSlot& actual, const Rect& expectedTarget, double expectedScale, const char* message) {
    return expect(closeEnough(actual.target.x, expectedTarget.x) && closeEnough(actual.target.y, expectedTarget.y) &&
                      closeEnough(actual.target.width, expectedTarget.width) && closeEnough(actual.target.height, expectedTarget.height) &&
                      closeEnough(actual.scale, expectedScale),
                  message);
}

LayoutConfig deterministicConfig() {
    LayoutConfig config;
    config.outerPaddingTop = 0.0;
    config.outerPaddingRight = 0.0;
    config.outerPaddingBottom = 0.0;
    config.outerPaddingLeft = 0.0;
    config.smallWindowBoost = 1.0;
    config.maxPreviewScale = 1.0;
    config.minSlotScale = 0.0;
    config.naturalScaleFlex = 0.0;
    config.preserveInputOrder = true;
    return config;
}

LayoutConfig sortedLayoutConfig() {
    LayoutConfig config = deterministicConfig();
    config.preserveInputOrder = false;
    return config;
}

bool sameRectExactly(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool sameSlotsExactly(const std::vector<WindowSlot>& a, const std::vector<WindowSlot>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].index != b[i].index || a[i].rowGroup != b[i].rowGroup || a[i].scale != b[i].scale ||
            !sameRectExactly(a[i].natural, b[i].natural) || !sameRectExactly(a[i].target, b[i].target))
            return false;
    return true;
}

const WindowSlot* slotFor(const std::vector<WindowSlot>& slots, std::size_t index) {
    const auto it = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) { return slot.index == index; });
    return it == slots.end() ? nullptr : &*it;
}

bool validNatural2Slots(const std::vector<WindowSlot>& slots, const std::vector<WindowInput>& inputs,
                        const Rect& inner, double maxScale, double gapX, double gapY) {
    bool ok = expect(slots.size() == inputs.size(), "Natural2 must retain every input exactly once");
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& slot = slots[i];
        const auto it = std::find_if(inputs.begin(), inputs.end(), [&](const auto& w) { return w.index == slot.index; });
        ok &= expect(it != inputs.end(), "Natural2 must preserve output indices");
        if (it == inputs.end())
            continue;
        const auto& t = slot.target;
        ok &= expect(std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.width) && std::isfinite(t.height) &&
                         t.width > 0 && t.height > 0 && std::isfinite(slot.scale) && slot.scale > 0,
                     "Natural2 must return finite positive previews");
        ok &= expect(rectInside(t, inner), "Natural2 must apply padding to the supplied region exactly once");
        ok &= expect(sameRectExactly(slot.natural, it->natural), "Natural2 must retain current source geometry");
        ok &= expect(slot.rowGroup == it->rowGroup, "Natural2 must return current group ordinals");
        ok &= expect(closeEnough(t.width / it->natural.width, t.height / it->natural.height) &&
                         closeEnough(slot.scale, t.width / it->natural.width) && slot.scale <= maxScale + 1e-6,
                     "Natural2 content must preserve aspect and report its actual bounded scale");
        for (std::size_t j = i + 1; j < slots.size(); ++j) {
            const auto& u = slots[j].target;
            const double gx = std::max(u.x - t.x - t.width, t.x - u.x - u.width);
            const double gy = std::max(u.y - t.y - t.height, t.y - u.y - u.height);
            ok &= expect(slot.index != slots[j].index, "Natural2 must not duplicate a slot");
            ok &= expect(gx >= gapX - 1e-6 || gy >= gapY - 1e-6,
                         "Natural2 must satisfy the reported horizontal or vertical spacing");
        }
    }
    return ok;
}

bool overviewLayoutSessionRegression() {
    using namespace hymission;
    bool ok = true;
    struct DirectionCase {
        ScrollingLayoutDirection direction;
        std::array<std::size_t, 3> visualIndices;
        LayoutAxis axis;
    };
    const DirectionCase directions[]{
        {ScrollingLayoutDirection::Right, {0, 1, 2}, LayoutAxis::Horizontal},
        {ScrollingLayoutDirection::Left, {2, 1, 0}, LayoutAxis::Horizontal},
        {ScrollingLayoutDirection::Down, {0, 1, 2}, LayoutAxis::Vertical},
        {ScrollingLayoutDirection::Up, {2, 1, 0}, LayoutAxis::Vertical},
    };
    for (const auto& expected : directions) {
        for (std::size_t nativeIndex = 0; nativeIndex < 3; ++nativeIndex) {
            const auto mapped = scrollingColumnGeometry(nativeIndex, 3, expected.direction);
            ok &= expect(mapped && mapped->columnIndex == expected.visualIndices[nativeIndex] && mapped->columnAxis == expected.axis,
                         "all four scrolling directions must map first/middle/last columns into the correct visual order and axis");
        }
        const auto single = scrollingColumnGeometry(0, 1, expected.direction);
        ok &= expect(single && single->columnIndex == 0 && single->columnAxis == expected.axis,
                     "a single column must retain its index while still mapping the scrolling axis");
        ok &= expect(!scrollingColumnGeometry(0, 0, expected.direction) && !scrollingColumnGeometry(3, 3, expected.direction) &&
                         !scrollingColumnGeometry(std::numeric_limits<std::size_t>::max(), 3, expected.direction),
                     "empty and out-of-range column metadata must be rejected without unsigned wraparound");
    }
    ok &= expect(!scrollingColumnGeometry(0, 1, static_cast<ScrollingLayoutDirection>(99)),
                 "an unknown scrolling direction must not manufacture column metadata");
    for (const auto name : {"natural2", "NATURAL2", "Natural2"})
        ok &= expect(parseOverviewLayoutEngine(name) == LayoutEngine::Natural2, "controller parsing must select Natural2 explicitly");
    for (const auto name : {"natural", "apple", "apple-like", "expose", "mission-control"})
        ok &= expect(parseOverviewLayoutEngine(name) == LayoutEngine::Natural, "legacy Natural aliases must retain their meaning");
    ok &= expect(parseOverviewLayoutEngine("THUMBNAILS") == LayoutEngine::Thumbnail &&
                     parseOverviewLayoutEngine("grid") == LayoutEngine::Grid && parseOverviewLayoutEngine("unknown") == LayoutEngine::Grid,
                 "legacy parsing and the default engine must remain compatible");

    // Hidden reservations retain their owners; a new object cannot recycle
    // the address of a closed window/column during the same overview session.
    std::vector<std::shared_ptr<int>> owners;
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<int> weak = owner;
    const auto id = keepLayoutIdentity(owners, owner);
    ok &= expect(id && keepLayoutIdentity(owners, owner) == id && owners.size() == 1 &&
                     !keepLayoutIdentity(owners, std::shared_ptr<int>{}),
                 "stable identity must use the owner, deduplicate it and reject null handles");
    auto trialOwners = owners;
    owner.reset();
    owners.clear();
    const auto newOwner = std::make_shared<int>(2);
    ok &= expect(!weak.expired() && keepLayoutIdentity(owners, newOwner) != id,
                 "a copied snapshot must keep hidden identities alive without sharing mutable identity tables");
    trialOwners.clear();
    ok &= expect(weak.expired(), "teardown must release identity owners once all snapshots have gone");

    auto config = deterministicConfig();
    config.engine = LayoutEngine::Natural2;
    const Rect area{100, 200, 1000, 800};
    const OverviewLayoutRegion a{1, 11, -1001}, b{1, 12, -1001}, otherMonitor{2, 11, -1001}, changedWorkspace{1, 11, 42};
    const std::vector<WindowInput> inputs{
        {.index = 4, .natural = {0, 0, 600, 800}, .label = "same", .stableId = 10, .groupId = -1001},
        {.index = 8, .natural = {620, 0, 600, 390}, .label = "same", .stableId = 20, .groupId = -1001},
        {.index = 12, .natural = {620, 410, 600, 390}, .label = "same", .stableId = 30, .groupId = -1001},
    };
    OverviewLayoutSession accepted;
    const auto first = accepted.compute(a, inputs, area, config);
    ok &= expect(accepted.context(a) && accepted.context(a)->lastReflowKind == LayoutReflowKind::ColdInitial,
                 "a fresh overview region must start with a cold context");
    (void)accepted.compute(b, inputs, area, config);
    (void)accepted.compute(otherMonitor, inputs, area, config);
    ok &= expect(accepted.context(b) && accepted.context(b)->lastReflowKind == LayoutReflowKind::ColdInitial &&
                     accepted.context(otherMonitor) && accepted.context(otherMonitor)->lastReflowKind == LayoutReflowKind::ColdInitial,
                 "tiles and monitors must own independent histories even for the same workspace and source geometry");
    auto trial = accepted;
    Rect shifted = area;
    shifted.x += 200;
    (void)trial.compute(a, inputs, shifted, config);
    ok &= expect(trial.context(a) && sameRectExactly(trial.context(a)->area, shifted) &&
                     accepted.context(a) && sameRectExactly(accepted.context(a)->area, area),
                 "transition and drop predictions must never mutate the accepted region history");
    auto filtered = std::vector<WindowInput>{inputs.back()};
    filtered[0].index = 0;
    const auto warm = accepted.compute(a, filtered, area, config);
    const auto* expected = slotFor(first, 12);
    const auto* survivor = slotFor(warm, 0);
    ok &= expect(expected && survivor && sameRectExactly(expected->target, survivor->target),
                 "a controller region must retain filtered survivors by stable identity after renumbering");
    ok &= expect(accepted.compute(a, {}, area, config).empty() && accepted.context(a) && accepted.context(a)->valid &&
                     sameSlotsExactly(first, accepted.compute(a, inputs, area, config)),
                 "an empty search and restoration must preserve the region reservations");
    (void)accepted.compute(changedWorkspace, inputs, area, config);
    ok &= expect(accepted.context(changedWorkspace) && accepted.context(changedWorkspace)->lastReflowKind == LayoutReflowKind::ColdInitial,
                 "assigning a different workspace to the same tile must start a fresh history");
    accepted.retainRegions({b, changedWorkspace});
    ok &= expect(!accepted.context(a) && !accepted.context(otherMonitor) && accepted.context(b) && accepted.context(changedWorkspace),
                 "removed monitor and tile regions must be pruned without clearing surviving regions");
    auto legacy = config;
    legacy.engine = LayoutEngine::Grid;
    (void)accepted.compute(b, {}, area, legacy);
    ok &= expect(!accepted.context(b) && !accepted.context(changedWorkspace), "switching engines must discard all Natural2 histories, even for empty inputs");
    (void)accepted.compute(b, inputs, area, config);
    ok &= expect(accepted.context(b) && accepted.context(b)->lastReflowKind == LayoutReflowKind::ColdInitial,
                 "switching back to Natural2 must not resurrect the old session");
    accepted = {};
    ok &= expect(!accepted.context(b), "closing overview must discard all region contexts");
    return ok;
}

bool natural2WorkspaceAndRowRegression(const MissionControlLayout& engine) {
    using namespace hymission;
    bool ok = true;
    auto config = deterministicConfig();
    config.engine = LayoutEngine::Natural2;
    config.maxPreviewScale = .95;
    config.minPreviewShortEdge = 0;
    config.forceRowGroups = false;
    const Rect area{0, 0, 2200, 1600};
    // Another overlapping window forces rows. Geometrically adjacent windows
    // still form a stack within one workspace, even with forceRowGroups off.
    // Either differing stable workspace IDs or differing fallback row groups
    // must prevent an accidental cross-workspace stack.
    for (int mode = 0; mode < 4; ++mode) {
        const std::vector<WindowInput> windows{
            {.index = 0, .natural = {0, 0, 400, 200}, .label = "top", .stableId = 10, .groupId = 100},
            {.index = 1, .natural = {0, 212, 400, 200}, .label = "bottom", .rowGroup = mode & 1 ? 1U : 0U,
             .stableId = 20, .groupId = mode & 2 ? 200 : 100},
            {.index = 2, .natural = {100, 0, 200, 200}, .label = "overlap", .stableId = 30, .groupId = 100, .floating = true},
        };
        LayoutContext history;
        const auto result = engine.compute(windows, area, config, history);
        bool stacked = false;
        for (const auto& band : history.bands)
            for (const auto& unit : band.units)
                stacked |= std::find(unit.members.begin(), unit.members.end(), 10) != unit.members.end() &&
                           std::find(unit.members.begin(), unit.members.end(), 20) != unit.members.end();
        ok &= expect(!history.affine && !history.bands.empty() && stacked == (mode == 0),
                     "geometric fallback must stack within one workspace and never merge across workspace metadata");
        ok &= validNatural2Slots(result, windows, area, config.maxPreviewScale, config.columnSpacing, config.rowSpacing);
    }

    // Unequal-width overlapping windows have opposite left-edge and center
    // orders. Center order must win, with the row tightly packed and centered.
    const std::vector<WindowInput> floating{
        {.index = 0, .natural = {0, 0, 1000, 200}, .label = "wide", .stableId = 10, .groupId = 100, .floating = true},
        {.index = 1, .natural = {100, 0, 200, 200}, .label = "narrow", .stableId = 20, .groupId = 100, .floating = true},
    };
    LayoutContext history;
    const auto result = engine.compute(floating, area, config, history);
    const auto* wide = slotFor(result, 0);
    const auto* narrow = slotFor(result, 1);
    ok &= expect(history.bands.size() == 1 && wide && narrow && narrow->target.centerX() < wide->target.centerX(),
                 "row display order must use source centers rather than source left edges");
    if (wide && narrow) {
        ok &= expect(closeEnough(wide->target.x - narrow->target.x - narrow->target.width, config.columnSpacing) &&
                         closeEnough(narrow->target.x - area.x, area.x + area.width - wide->target.x - wide->target.width),
                     "row slack must stay around a centered tight row instead of creating nonuniform internal gaps");
    }
    ok &= validNatural2Slots(result, floating, area, config.maxPreviewScale, config.columnSpacing, config.rowSpacing);
    return ok;
}

bool natural2WarmTopologyRegression(const MissionControlLayout& engine) {
    using namespace hymission;
    bool ok = true;
    auto config = deterministicConfig();
    config.engine = LayoutEngine::Natural2;
    config.maxPreviewScale = .95;
    config.minPreviewShortEdge = 0;
    config.forceRowGroups = true;
    const Rect area{0, 0, 1000, 1000};

    // The first window of another workspace cannot join a single-group affine layout.
    {
        std::vector<WindowInput> windows{
            {.index = 0, .natural = {0, 0, 200, 200}, .label = "old", .stableId = 1, .groupId = 10},
        };
        LayoutContext history;
        (void)engine.compute(windows, area, config, history);
        ok &= expect(history.affine, "workspace insertion fixture must start with an affine layout");
        windows.push_back({.index = 1, .natural = {300, 0, 100, 100}, .label = "new workspace", .rowGroup = 1,
                           .stableId = 2, .groupId = 20});
        const auto result = engine.compute(windows, area, config, history);
        const auto* first = slotFor(result, 0);
        const auto* second = slotFor(result, 1);
        ok &= expect(history.lastReflowKind == LayoutReflowKind::ColdFallback && !history.affine && history.bands.size() == 2 &&
                         first && second && first->target.y + first->target.height + config.rowSpacing <= second->target.y + 1e-6,
                     "a new workspace must rebuild an affine layout into ordered separate bands");
        ok &= validNatural2Slots(result, windows, area, config.maxPreviewScale, config.columnSpacing, config.rowSpacing);
    }

    const std::vector<WindowInput> columns{
        {.index = 0, .natural = {0, 0, 200, 200}, .label = "col10", .stableId = 1, .groupId = 10, .columnId = 10, .floating = false},
        {.index = 1, .natural = {0, 0, 200, 200}, .label = "col20", .stableId = 2, .groupId = 10, .columnId = 20,
         .columnIndex = 1, .floating = false},
    };
    LayoutContext columnHistory;
    (void)engine.compute(columns, area, config, columnHistory);
    ok &= expect(!columnHistory.affine && columnHistory.bands.size() == 1, "column insertion fixture must start with a row layout");

    // New members of a known column require a rebuilt stack, including columns
    // first encountered earlier in the same speculative batch of newcomers.
    for (bool newColumn : {false, true}) {
        auto windows = columns;
        auto history = columnHistory;
        if (newColumn) {
            windows.push_back({.index = 2, .natural = {900, 0, 100, 100}, .label = "new column first", .stableId = 3, .groupId = 10,
                               .columnId = 30, .columnIndex = 2, .floating = false});
            windows.push_back({.index = 3, .natural = {900, 112, 100, 100}, .label = "new column second", .stableId = 4, .groupId = 10,
                               .columnId = 30, .columnIndex = 2, .columnMemberIndex = 1, .floating = false});
        } else {
            windows.push_back({.index = 2, .natural = {0, 212, 200, 100}, .label = "second col10 member", .stableId = 3, .groupId = 10,
                               .columnId = 10, .columnMemberIndex = 1, .floating = false});
        }
        const auto result = engine.compute(windows, area, config, history);
        const auto* top = slotFor(result, newColumn ? 2 : 0);
        const auto* bottom = slotFor(result, newColumn ? 3 : 2);
        ok &= expect(history.lastReflowKind == LayoutReflowKind::ColdFallback && history.windows.size() == windows.size() && top && bottom &&
                         closeEnough(top->target.centerX(), bottom->target.centerX()) &&
                         top->target.y + top->target.height + config.rowSpacing <= bottom->target.y + 1e-6,
                     "new column members must rebuild one ordered stack rather than become separate horizontal units");
        ok &= validNatural2Slots(result, windows, area, config.maxPreviewScale, config.columnSpacing, config.rowSpacing);
    }

    // Keep explicit column order even when natural x disagrees; another workspace
    // forces B, without source overlaps that could select a different reading order.
    {
        std::vector<WindowInput> windows{
            {.index = 0, .natural = {0, 0, 200, 200}, .label = "col10", .stableId = 1, .groupId = 10, .columnId = 10, .floating = false},
            {.index = 1, .natural = {300, 0, 200, 200}, .label = "col30", .stableId = 2, .groupId = 10, .columnId = 30,
             .columnIndex = 2, .floating = false},
            {.index = 3, .natural = {0, 0, 200, 200}, .label = "other workspace", .rowGroup = 1, .stableId = 4, .groupId = 20,
             .columnId = 40, .floating = false},
        };
        LayoutContext history;
        const auto before = engine.compute(windows, area, config, history);
        windows.push_back({.index = 2, .natural = {900, 0, 100, 100}, .label = "col20", .stableId = 3, .groupId = 10,
                           .columnId = 20, .columnIndex = 1, .floating = false});
        const auto result = engine.compute(windows, area, config, history);
        const auto* left = slotFor(result, 0);
        const auto* middle = slotFor(result, 2);
        const auto* right = slotFor(result, 1);
        const auto* otherBefore = slotFor(before, 3);
        const auto* otherAfter = slotFor(result, 3);
        ok &= expect(history.lastReflowKind == LayoutReflowKind::LocalInsert && left && middle && right &&
                         left->target.centerX() < middle->target.centerX() && middle->target.centerX() < right->target.centerX(),
                     "warm insertion of an explicit column must follow columnIndex rather than source x");
        ok &= expect(otherBefore && otherAfter && sameRectExactly(otherBefore->target, otherAfter->target),
                     "local insertion must leave the other workspace unchanged in both axes");
        ok &= validNatural2Slots(result, windows, area, config.maxPreviewScale, config.columnSpacing, config.rowSpacing);
        ok &= expect(sameSlotsExactly(result, engine.compute(windows, area, config, history)),
                     "the accepted local insertion must be stable on the next identical call");
    }
    return ok;
}

bool natural2Regression(const MissionControlLayout& engine) {
    using namespace hymission;
    bool ok = true;
    LayoutConfig config;
    config.engine = LayoutEngine::Natural2;
    config.outerPaddingTop = 31;
    config.outerPaddingRight = 17;
    config.outerPaddingBottom = 43;
    config.outerPaddingLeft = 23;
    config.forceRowGroups = true;
    const Rect area{113, -71, 1400, 1000};
    const Rect inner{136, -40, 1360, 926};
    const std::vector<WindowInput> windows{
        {.index = 37, .natural = {0, 0, 620, 900}, .label = "same title", .rowGroup = 3, .stableId = 0, .groupId = -1001},
        {.index = 5, .natural = {632, 0, 620, 444}, .label = "same title", .rowGroup = 3, .stableId = 11, .groupId = -1001},
        {.index = 900, .natural = {632, 456, 620, 444}, .label = "same title", .rowGroup = 3, .stableId = 12, .groupId = -1001},
    };
    LayoutDegradation diagnostic;
    const auto plain = engine.compute(windows, area, config);
    const auto reported = engine.compute(windows, area, config, &diagnostic);
    LayoutContext history;
    const auto first = engine.compute(windows, area, config, history);
    ok &= expect(diagnostic.available && diagnostic.feasible, "Natural2 must dispatch to the new core and report feasibility");
    ok &= expect(sameSlotsExactly(plain, reported) && sameSlotsExactly(plain, first),
                 "Natural2 stateless, diagnostic and cold context overloads must agree exactly");
    ok &= expect(history.valid && history.lastReflowKind == LayoutReflowKind::ColdInitial,
                 "a stable ID of zero and negative workspace key must support history");
    ok &= validNatural2Slots(first, windows, inner, config.maxPreviewScale, config.columnSpacing, config.rowSpacing);
    auto permuted = windows;
    std::reverse(permuted.begin(), permuted.end());
    ok &= expect(sameSlotsExactly(plain, engine.compute(permuted, area, config)),
                 "stateless layouts with stable identities must not depend on input permutation");
    auto grid = config;
    grid.engine = LayoutEngine::Grid;
    ok &= expect(!sameSlotsExactly(plain, engine.compute(windows, area, grid)), "Natural2 must not silently dispatch to Grid");

    for (const auto& gaps : {std::pair{16.0, 48.0}, std::pair{64.0, 12.0}}) {
        auto asymmetric = config;
        asymmetric.columnSpacing = gaps.first;
        asymmetric.rowSpacing = gaps.second;
        const auto slots = engine.compute(windows, area, asymmetric, &diagnostic);
        ok &= validNatural2Slots(slots, windows, inner, asymmetric.maxPreviewScale, gaps.first, gaps.second);
    }

    // Renumbering, duplicate/changed titles and controller-derived flags must
    // not turn a warm subset into a new spatial layout.
    auto subset = std::vector<WindowInput>{windows[2], windows[1]};
    for (std::size_t i = 0; i < subset.size(); ++i) {
        subset[i].index = i;
        subset[i].label = "a different shared title";
        subset[i].rowGroup = 0;
    }
    auto warmConfig = config;
    warmConfig.preserveInputOrder = true;
    warmConfig.rankScaleByInputOrder = true;
    warmConfig.layoutSpaceWeight = 90;
    warmConfig.naturalScaleFlex = .1;
    const auto retained = engine.compute(subset, area, warmConfig, history, &history.degradation);
    ok &= expect(history.lastReflowKind == LayoutReflowKind::Retained && history.degradation.available && history.degradation.feasible,
                 "warm history and an aliased diagnostic output must remain intact");
    for (const auto& w : subset) {
        const auto old = std::find_if(windows.begin(), windows.end(), [&](const auto& v) { return v.stableId == w.stableId; });
        const auto* before = slotFor(first, old->index);
        const auto* after = slotFor(retained, w.index);
        ok &= expect(before && after && sameRectExactly(before->target, after->target),
                     "warm removal must match stable identity rather than title, ordinal or input index");
    }
    ok &= expect(engine.compute({}, area, warmConfig, history).empty() && history.valid &&
                     history.lastReflowKind == LayoutReflowKind::Retained,
                 "empty search results must preserve the session reservations");
    const auto restored = engine.compute(windows, area, warmConfig, history);
    ok &= expect(sameSlotsExactly(first, restored), "restoring a search result must recover its exact original slot");

    auto speculative = history;
    Rect shifted = area;
    shifted.x += 200;
    const auto alternative = engine.compute(windows, shifted, config, speculative);
    ok &= expect(speculative.lastReflowKind == LayoutReflowKind::ColdFallback && !alternative.empty() &&
                     sameRectExactly(history.area, area) && sameSlotsExactly(first, engine.compute(windows, area, config, history)),
                 "speculative context copies and changed areas must not mutate the accepted state");

    for (const auto legacy : {LayoutEngine::Grid, LayoutEngine::Natural, LayoutEngine::Thumbnail}) {
        auto legacyConfig = config;
        legacyConfig.engine = legacy;
        auto stale = history;
        diagnostic.available = diagnostic.feasible = true;
        const auto original = engine.compute(windows, area, legacyConfig);
        const auto throughContext = engine.compute(windows, area, legacyConfig, stale, &diagnostic);
        ok &= expect(sameSlotsExactly(original, throughContext) && !stale.valid && stale.windows.empty() && !diagnostic.available,
                     "legacy context overloads must preserve results and clear new-engine history and diagnostics");
        diagnostic.available = true;
        ok &= expect(engine.compute({}, area, legacyConfig, &diagnostic).empty() && !diagnostic.available,
                     "legacy empty calls must also clear stale diagnostics");
        stale = history;
        ok &= expect(engine.compute({}, area, legacyConfig, stale, &diagnostic).empty() && !stale.valid &&
                         stale.lastReflowKind == LayoutReflowKind::None && !diagnostic.available,
                     "an empty call through a legacy engine must end the previous Natural2 session");
    }

    auto duplicateIds = windows;
    duplicateIds[1].stableId = duplicateIds[0].stableId;
    auto invalidHistory = history;
    const auto invalid = engine.compute(duplicateIds, area, config, invalidHistory);
    ok &= expect(!invalidHistory.valid && invalidHistory.lastReflowKind == LayoutReflowKind::StatelessFallback &&
                     sameSlotsExactly(invalid, engine.compute(duplicateIds, area, config)),
                 "duplicate stable IDs must fall back safely without dropping windows");
    duplicateIds[1].stableId = std::nullopt;
    invalidHistory = history;
    const auto missing = engine.compute(duplicateIds, area, config, invalidHistory);
    ok &= expect(!invalidHistory.valid && invalidHistory.lastReflowKind == LayoutReflowKind::StatelessFallback &&
                     sameSlotsExactly(missing, engine.compute(duplicateIds, area, config)),
                 "missing stable IDs must disable history rather than guessing identity from labels");

    // Public dispatch must normalize before entering the core.
    auto raw = config;
    raw.smallWindowBoost = std::numeric_limits<double>::quiet_NaN();
    raw.maxPreviewScale = std::numeric_limits<double>::infinity();
    raw.columnSpacing = std::numeric_limits<double>::quiet_NaN();
    const auto normalized = normalizeLayoutConfig(raw);
    ok &= expect(sameSlotsExactly(engine.compute(windows, area, raw), engine.compute(windows, area, normalized)),
                 "Natural2 must receive normalized finite configuration through every public entry");
    LayoutContext normalizedHistory;
    ok &= expect(sameSlotsExactly(engine.compute(windows, area, raw, &diagnostic),
                                  engine.compute(windows, area, normalized)) &&
                     sameSlotsExactly(engine.compute(windows, area, raw, normalizedHistory),
                                      engine.compute(windows, area, normalized)),
                 "diagnostic and context overloads must normalize the configuration too");

    auto compact = deterministicConfig();
    compact.engine = LayoutEngine::Natural2;
    compact.maxPreviewScale = .95;
    compact.minSlotScale = .1;
    compact.minPreviewShortEdge = 32;
    const std::vector<WindowInput> pair{
        {.index = 7, .natural = {0, 0, 120, 120}, .label = "a", .stableId = 1},
        {.index = 2, .natural = {200, 0, 120, 120}, .label = "b", .stableId = 2},
    };
    const auto tiny = engine.compute(pair, {0, 0, 7, 7}, compact, &diagnostic);
    ok &= expect(diagnostic.feasible && diagnostic.emergencySpacing && diagnostic.belowMinSlotScale &&
                     diagnostic.effectiveColumnSpacing <= compact.columnSpacing && diagnostic.effectiveRowSpacing <= compact.rowSpacing,
                 "tiny regions must visibly degrade spacing instead of losing windows");
    ok &= validNatural2Slots(tiny, pair, {0, 0, 7, 7}, compact.maxPreviewScale,
                             diagnostic.effectiveColumnSpacing, diagnostic.effectiveRowSpacing);
    for (double gap : {0.0, 1.0, 2.0}) {
        compact.columnSpacing = compact.rowSpacing = gap;
        const auto slots = engine.compute(pair, {0, 0, 500, 300}, compact, &diagnostic);
        ok &= expect(diagnostic.effectiveColumnSpacing <= gap && diagnostic.effectiveRowSpacing <= gap,
                     "the spacing floor must never raise a configured small or zero spacing");
        ok &= validNatural2Slots(slots, pair, {0, 0, 500, 300}, compact.maxPreviewScale,
                                 diagnostic.effectiveColumnSpacing, diagnostic.effectiveRowSpacing);
    }
    auto impossible = history;
    diagnostic.feasible = true;
    ok &= expect(engine.compute(pair, {0, 0, 0, 100}, compact, impossible, &diagnostic).empty() &&
                     !diagnostic.feasible && !impossible.valid,
                 "unusable regions must be reported as infeasible and invalidate history");
    compact.maxPreviewScale = 0;
    ok &= expect(engine.compute(pair, {0, 0, 500, 300}, compact, &diagnostic).empty() && !diagnostic.feasible,
                 "a zero maximum scale must not be reported as a positive-area layout");

    // Geometry changes must update diagnostic facts, even when allocations stay fixed.
    auto resizeConfig = deterministicConfig();
    resizeConfig.engine = LayoutEngine::Natural2;
    resizeConfig.maxPreviewScale = .95;
    resizeConfig.minSlotScale = .9;
    resizeConfig.minPreviewShortEdge = 60;
    std::vector<WindowInput> resized{
        {.index = 0, .natural = {0, 0, 80, 80}, .label = "a", .stableId = 1, .floating = true},
        {.index = 1, .natural = {0, 0, 80, 80}, .label = "b", .stableId = 2, .floating = true},
    };
    LayoutContext resizeHistory;
    const auto resizeBase = engine.compute(resized, {0, 0, 1800, 1000}, resizeConfig, resizeHistory);
    resized[0].natural = {0, 0, 140, 60};
    const auto resizeResult = engine.compute(resized, {0, 0, 1800, 1000}, resizeConfig, resizeHistory, &diagnostic);
    ok &= expect(resizeBase.size() == 2 && resizeResult.size() == 2 && diagnostic.belowMinSlotScale && diagnostic.belowMinShortEdge,
                 "retained resize diagnostics must describe current content, not the cold snapshot");

    // Contain uses the live source aspect, not minWindowLength padded dimensions.
    resizeConfig.minSlotScale = resizeConfig.minPreviewShortEdge = 0;
    resized[0].natural = {0, 0, 1000, 1000};
    resized[1].natural = {100, 100, 40, 80};
    resizeHistory.reset();
    (void)engine.compute(resized, {0, 0, 400, 300}, resizeConfig, resizeHistory);
    const auto it = std::find_if(resizeHistory.windows.begin(), resizeHistory.windows.end(), [](const auto& w) { return w.input.stableId == 2; });
    ok &= expect(it != resizeHistory.windows.end(), "small source must have a reservation");
    if (it != resizeHistory.windows.end()) {
        const auto allocation = it->allocation;
        resized[1].natural.width = 30;
        resized[1].natural.height = 60;
        const double fit = std::min({allocation.width / 30, allocation.height / 60, resizeConfig.maxPreviewScale});
        const auto result = engine.compute(resized, {0, 0, 400, 300}, resizeConfig, resizeHistory);
        const auto* small = slotFor(result, 1);
        ok &= expect(small && closeEnough(small->scale, fit) && closeEnough(small->target.centerX(), allocation.centerX()) &&
                         closeEnough(small->target.centerY(), allocation.centerY()),
                     "resized content must fit its reservation using actual source dimensions");
    }

    // A group ordinal can change harmlessly, but reversing real group order cannot.
    auto groupedConfig = deterministicConfig();
    groupedConfig.engine = LayoutEngine::Natural2;
    groupedConfig.forceRowGroups = true;
    std::vector<WindowInput> groups{
        {.index = 0, .natural = {0, 0, 300, 200}, .label = "a", .rowGroup = 0, .stableId = 1, .groupId = -1001},
        {.index = 1, .natural = {0, 0, 300, 200}, .label = "b", .rowGroup = 1, .stableId = 2, .groupId = 42},
    };
    LayoutContext groupHistory;
    (void)engine.compute(groups, {0, 0, 1000, 800}, groupedConfig, groupHistory);
    groups[0].rowGroup = 1;
    groups[1].rowGroup = 0;
    const auto reordered = engine.compute(groups, {0, 0, 1000, 800}, groupedConfig, groupHistory);
    const auto* lower = slotFor(reordered, 0);
    const auto* upper = slotFor(reordered, 1);
    ok &= expect(lower && upper && upper->target.y + upper->target.height <= lower->target.y,
                 "a real workspace reorder must invalidate incompatible warm band order");

    // Search can temporarily remove a whole workspace and renumber the
    // survivors. A resized survivor must not jump when that group returns.
    std::vector<WindowInput> searchGroups{
        {.index = 0, .natural = {0, 0, 300, 200}, .label = "a", .rowGroup = 0, .stableId = 1, .groupId = -1001},
        {.index = 1, .natural = {0, 0, 300, 200}, .label = "b", .rowGroup = 1, .stableId = 2, .groupId = 42},
        {.index = 2, .natural = {0, 0, 300, 200}, .label = "c", .rowGroup = 2, .stableId = 3, .groupId = 100},
    };
    LayoutContext searchHistory;
    const auto searchBase = engine.compute(searchGroups, {0, 0, 1000, 800}, groupedConfig, searchHistory);
    auto filteredGroups = std::vector<WindowInput>{searchGroups[0], searchGroups[2]};
    filteredGroups[1].index = filteredGroups[1].rowGroup = 1;
    filteredGroups[1].natural = {0, 0, 600, 100};
    const auto filteredSlots = engine.compute(filteredGroups, {0, 0, 1000, 800}, groupedConfig, searchHistory);
    searchGroups[2].natural = filteredGroups[1].natural;
    const auto unfiltered = engine.compute(searchGroups, {0, 0, 1000, 800}, groupedConfig, searchHistory);
    const auto* expectedC = slotFor(filteredSlots, 1);
    const auto* actualC = slotFor(unfiltered, 2);
    const auto* expectedB = slotFor(searchBase, 1);
    const auto* actualB = slotFor(unfiltered, 1);
    ok &= expect(searchHistory.lastReflowKind == LayoutReflowKind::Retained && expectedC && actualC && expectedB && actualB &&
                     sameRectExactly(expectedC->target, actualC->target) && sameRectExactly(expectedB->target, actualB->target),
                 "restoring a hidden workspace must compare one canonical group order, not mixed-generation ordinals");

    std::vector<WindowInput> column{
        {.index = 0, .natural = {0, 0, 400, 300}, .label = "a", .stableId = 10, .groupId = -1001, .columnId = 99,
         .columnMemberIndex = 0, .floating = false},
        {.index = 1, .natural = {0, 0, 400, 300}, .label = "b", .stableId = 20, .groupId = -1001, .columnId = 99,
         .columnMemberIndex = 1, .floating = false},
    };
    LayoutContext columnHistory;
    const auto vertical = engine.compute(column, {0, 0, 1000, 800}, groupedConfig, columnHistory);
    const auto* top = slotFor(vertical, 0);
    const auto* bottom = slotFor(vertical, 1);
    ok &= expect(top && bottom && closeEnough(top->target.centerX(), bottom->target.centerX()) &&
                     top->target.y + top->target.height + groupedConfig.rowSpacing <= bottom->target.y + 1e-6,
                 "explicit column members must preserve their full secondary-axis spacing");
    std::swap(column[0].columnMemberIndex, column[1].columnMemberIndex);
    const auto swapped = engine.compute(column, {0, 0, 1000, 800}, groupedConfig, columnHistory);
    top = slotFor(swapped, 1);
    bottom = slotFor(swapped, 0);
    ok &= expect(top && bottom && top->target.y + top->target.height <= bottom->target.y,
                 "changing explicit member order must not reuse the old column topology");
    for (auto& w : column)
        w.columnAxis = LayoutAxis::Vertical;
    const auto horizontal = engine.compute(column, {0, 0, 1000, 800}, groupedConfig, columnHistory);
    const auto* left = slotFor(horizontal, 1);
    const auto* right = slotFor(horizontal, 0);
    ok &= expect(left && right && closeEnough(left->target.centerY(), right->target.centerY()) &&
                     left->target.x + left->target.width + groupedConfig.columnSpacing <= right->target.x + 1e-6,
                 "vertical-scrolling column metadata must place members side by side");
    return ok;
}

} // namespace

int main() {
    using namespace hymission;

    MissionControlLayout engine;
    bool                 ok = true;

    {
        // A moving Stage ghost can almost fill its translated viewport. Adding
        // the origin before subtracting its height inverted clamp's bounds by
        // one ULP and aborted the compositor (deterministic seed 201617, #389).
        LayoutConfig config;
        config.minWindowLength=24;
        config.outerPaddingTop=config.outerPaddingBottom=config.outerPaddingLeft=config.outerPaddingRight=4;
        const Rect area{411.88280266653652,868.43682263198446,220,160};
        const auto slots=engine.compute({
            {.index=0,.natural={557.71705576944532,894.87708891008037,612.48928419155095,241.24218032492627},.label="ghost-a"},
            {.index=1,.natural={443.219496780074,2.0962340675506965,1467.5476798410521,579.16306941458242},.label="ghost-b"}},area,config);
        ok &= expect(slots.size()==2,"translated Stage ghost must retain all members");
        for (const auto& slot:slots) ok &= expect(rectInside(slot.target,area),"translated Stage ghost must have ordered clamp bounds and stay inside its viewport");
    }

    for (const auto layoutEngine : {LayoutEngine::Grid, LayoutEngine::Natural, LayoutEngine::Thumbnail}) {
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), static_cast<double>(std::numeric_limits<long>::max())}) {
            auto config = deterministicConfig();
            config.engine = layoutEngine;
            config.smallWindowBoost = invalid;
            config.maxPreviewScale = invalid;
            config.naturalScaleFlex = invalid;
            config.minSlotScale = invalid;
            config.layoutSpaceWeight = invalid;
            config.layoutScaleWeight = invalid;
            config.minWindowLength = invalid;
            config.minPreviewShortEdge = invalid;
            config.outerPaddingTop = invalid;
            config.rowSpacing = invalid;
            config.columnSpacing = invalid;
            const auto normalized = normalizeLayoutConfig(config);
            ok &= expect(std::isfinite(normalized.smallWindowBoost) && normalized.smallWindowBoost <= 4 &&
                         normalized.naturalScaleFlex <= 0.25 && normalized.minWindowLength <= 4096,
                         "layout input must be finite and bounded before entering the solvers");
            const auto slots = engine.compute({{.index = 0, .natural = {0, 0, 640, 480}, .label = "invalid-a"},
                                               {.index = 1, .natural = {640, 0, 800, 600}, .label = "invalid-b"}}, {0, 0, 1920, 1080}, config);
            ok &= expect(slots.size() == 2, "invalid layout configuration must retain every window");
            for (const auto& slot : slots)
                ok &= expect(std::isfinite(slot.target.x) && std::isfinite(slot.target.y) && std::isfinite(slot.target.width) &&
                             std::isfinite(slot.target.height) && std::isfinite(slot.scale), "invalid layout configuration must produce finite geometry");
        }
    }

    {
        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 0, 692, 173}, .label = "wide-short"},
            {.index = 1, .natural = {0, 220, 198, 719}, .label = "narrow-tall"},
            {.index = 2, .natural = {0, 460, 546, 622}, .label = "wide-tall"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1000, 800}, deterministicConfig());
        ok &= expect(slots.size() == 3, "row-height case should keep all windows");
        ok &= expectSlot(slots[0], {202.0, 0.0, 595.8026905829597, 148.95067264573993}, 0.8609865470852018,
                         "rejected windows must not contaminate the previous row height");
        ok &= expectSlot(slots[1], {163.0, 180.0, 170.47533632286996, 619.0493273542601}, 0.8609865470852018,
                         "second slot should stay in the lower row after the row-height fix");
    }

    {
        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 0, 186, 501}, .label = "a"},
            {.index = 1, .natural = {0, 200, 210, 733}, .label = "b"},
            {.index = 2, .natural = {0, 400, 1035, 100}, .label = "c"},
            {.index = 3, .natural = {0, 600, 240, 483}, .label = "d"},
            {.index = 4, .natural = {0, 800, 529, 1150}, .label = "e"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1000, 800}, deterministicConfig());
        ok &= expect(slots.size() == 5, "row-search case should keep all windows");
        ok &= expect(slots[2].target.x < 150.0 && slots[2].target.y > 700.0 && slots[2].target.width > 400.0,
                     "layout search must continue past the first local score drop");
        ok &= expectSlot(slots[3], {554.0, 603.0, 97.88635156664897, 196.99628252788105}, 0.4078597981943707,
                         "later row-count candidates should be allowed to win");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.rowSpacing = 200.0;
        config.columnSpacing = 0.0;
        config.forceRowGroups = true;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 0, 80, 80}, .label = "a", .rowGroup = 0},
            {.index = 1, .natural = {0, 0, 80, 80}, .label = "b", .rowGroup = 1},
        };

        const auto slots = engine.compute(windows, {0, 0, 120, 120}, config);
        ok &= expect(slots.size() == 2, "spacing clamp case should keep row-grouped windows");
        ok &= expect(slots[0].scale == 0.0 && slots[1].scale == 0.0, "oversized spacing should clamp scales to zero instead of going negative");
        ok &= expect(slots[0].target.width == 0.0 && slots[0].target.height == 0.0 && slots[1].target.width == 0.0 && slots[1].target.height == 0.0,
                     "oversized spacing should never produce negative preview sizes");
    }

    {
        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {260, 0, 50, 500}, .label = "thin-middle"},
            {.index = 1, .natural = {0, 0, 180, 180}, .label = "left"},
            {.index = 2, .natural = {420, 0, 180, 180}, .label = "right"},
        };

        const auto slots = engine.compute(windows, {0, 0, 600, 600}, sortedLayoutConfig());
        ok &= expect(slots.size() == 3, "default-order case should keep all windows");
        ok &= expect(closeEnough(slots[0].natural.width, 50.0) && closeEnough(slots[0].natural.height, 500.0),
                     "layout must preserve the original natural geometry");
        ok &= expect(slots[0].target.height > 0.0 && closeEnough(slots[0].target.width / slots[0].target.height, 50.0 / 500.0),
                     "layout previews must preserve the original aspect ratio");
        ok &= expect(slots[1].target.x < slots[0].target.x && slots[0].target.x < slots[2].target.x,
                     "default ordering should reorder slots by geometry instead of preserving input order");
    }

    {
        const std::vector<WindowInput> baseline = {
            {.index = 0, .natural = {0, 0, 200, 160}, .label = "selected"},
            {.index = 1, .natural = {240, 0, 200, 160}, .label = "neighbor"},
        };
        const std::vector<WindowInput> emphasized = {
            {.index = 0, .natural = {0, 0, 200, 160}, .label = "selected", .layoutEmphasis = 1.18},
            {.index = 1, .natural = {240, 0, 200, 160}, .label = "neighbor"},
        };
        const std::vector<WindowInput> lightlyEmphasized = {
            {.index = 0, .natural = {0, 0, 200, 160}, .label = "selected", .layoutEmphasis = 1.05},
            {.index = 1, .natural = {240, 0, 200, 160}, .label = "neighbor"},
        };

        const auto baselineSlots = engine.compute(baseline, {0, 0, 420, 220}, deterministicConfig());
        const auto lightlyEmphasizedSlots = engine.compute(lightlyEmphasized, {0, 0, 420, 220}, deterministicConfig());
        const auto emphasizedSlots = engine.compute(emphasized, {0, 0, 420, 220}, deterministicConfig());
        ok &= expect(baselineSlots.size() == 2 && lightlyEmphasizedSlots.size() == 2 && emphasizedSlots.size() == 2,
                     "selected emphasis case should keep both windows");
        ok &= expect(emphasizedSlots[0].target.width > baselineSlots[0].target.width,
                     "selected emphasis should enlarge the selected preview");
        ok &= expect(emphasizedSlots[0].target.width > lightlyEmphasizedSlots[0].target.width,
                     "larger selected emphasis should enlarge the selected preview more until layout caps apply");
        ok &= expect(emphasizedSlots[1].target.width < baselineSlots[1].target.width || emphasizedSlots[1].target.x > baselineSlots[1].target.x,
                     "selected emphasis should push neighboring previews away");
        ok &= expect(!rectsOverlap(emphasizedSlots[0].target, emphasizedSlots[1].target),
                     "selected emphasis should keep previews non-overlapping");
        ok &= expect(!rectsOverlap(lightlyEmphasizedSlots[0].target, lightlyEmphasizedSlots[1].target),
                     "lighter selected emphasis should keep previews non-overlapping");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {40, 40, 520, 360}, .label = "top-left"},
            {.index = 1, .natural = {520, 60, 440, 330}, .label = "top-right"},
            {.index = 2, .natural = {90, 380, 500, 300}, .label = "bottom-left"},
            {.index = 3, .natural = {570, 360, 410, 320}, .label = "bottom-right"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1000, 700}, config);
        ok &= expect(slots.size() == 4, "natural engine should keep all windows");
        for (std::size_t i = 0; i < slots.size(); ++i) {
            for (std::size_t j = i + 1; j < slots.size(); ++j)
                ok &= expect(!rectsOverlap(slots[i].target, slots[j].target), "natural engine should keep previews non-overlapping");
        }
        ok &= expect(slots[0].target.centerX() < slots[1].target.centerX() && slots[2].target.centerX() < slots[3].target.centerX(),
                     "natural engine should preserve left/right spatial memory");
        ok &= expect(slots[0].target.centerY() < slots[2].target.centerY() && slots[1].target.centerY() < slots[3].target.centerY(),
                     "natural engine should preserve top/bottom spatial memory");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {320, 780, 520, 360}, .label = "lower-left"},
            {.index = 1, .natural = {870, 790, 440, 330}, .label = "lower-right"},
            {.index = 2, .natural = {420, 1180, 500, 300}, .label = "bottom-left"},
            {.index = 3, .natural = {980, 1160, 410, 320}, .label = "bottom-right"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1400, 900}, config);
        ok &= expect(slots.size() == 4, "natural engine should keep off-center source windows");

        double centerY = 0.0;
        for (const auto& slot : slots)
            centerY += slot.target.centerY();
        centerY /= static_cast<double>(slots.size());

        ok &= expect(centerY > 330.0 && centerY < 570.0, "natural engine should recenter an off-screen-biased window cloud");
        ok &= expect(slots[0].target.centerX() < slots[1].target.centerX() && slots[2].target.centerX() < slots[3].target.centerX(),
                     "recentered natural engine should preserve left/right spatial memory");
        ok &= expect(slots[0].target.centerY() < slots[2].target.centerY() && slots[1].target.centerY() < slots[3].target.centerY(),
                     "recentered natural engine should preserve top/bottom spatial memory");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {80, 80, 1200, 700}, .label = "stacked-a"},
            {.index = 1, .natural = {90, 90, 1200, 700}, .label = "stacked-b"},
            {.index = 2, .natural = {100, 100, 1200, 700}, .label = "stacked-c"},
            {.index = 3, .natural = {110, 110, 1200, 700}, .label = "stacked-d"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1400, 900}, config);
        ok &= expect(slots.size() == 4, "natural engine should keep heavily overlapping windows");

        double averageScale = 0.0;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            averageScale += slots[i].scale;
            for (std::size_t j = i + 1; j < slots.size(); ++j)
                ok &= expect(!rectsOverlap(slots[i].target, slots[j].target), "overlap jitter should still produce non-overlapping previews");
        }
        averageScale /= static_cast<double>(slots.size());

        ok &= expect(averageScale > 0.22, "overlap jitter should avoid collapsing heavily stacked windows into tiny previews");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.minPreviewShortEdge = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 40, 1450, 48}, .label = "thin-wide"},
            {.index = 1, .natural = {120, 140, 56, 680}, .label = "thin-tall"},
            {.index = 2, .natural = {260, 260, 1300, 64}, .label = "thin-wide-2"},
            {.index = 3, .natural = {560, 70, 64, 720}, .label = "thin-tall-2"},
            {.index = 4, .natural = {460, 420, 28, 26}, .label = "tiny"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1100, 700}, config);
        ok &= expect(slots.size() == 5, "natural engine should keep extreme-aspect windows");
        for (const auto& slot : slots) {
            ok &= expect(rectInside(slot.target, {0, 0, 1100, 700}), "extreme-aspect previews should stay inside the monitor");
            ok &= expect(std::min(slot.target.width, slot.target.height) >= 31.9,
                         "natural engine should enforce a readable short edge for extreme-aspect previews");
        }
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {900, 100, 360, 260}, .label = "right-top"},
            {.index = 1, .natural = {980, 430, 360, 260}, .label = "right-bottom"},
            {.index = 2, .natural = {1220, 270, 240, 240}, .label = "far-right"},
        };

        const auto slots = engine.compute(windows, {0, 0, 1400, 900}, config);
        ok &= expect(slots.size() == 3, "natural engine should keep right-biased windows");

        double minX = 1400.0;
        double maxX = 0.0;
        for (const auto& slot : slots) {
            minX = std::min(minX, slot.target.x);
            maxX = std::max(maxX, slot.target.x + slot.target.width);
        }

        const double centerX = (minX + maxX) / 2.0;
        ok &= expect(centerX > 560.0 && centerX < 840.0, "natural engine should center the final target cloud");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 32.0;
        config.columnSpacing = 32.0;

        const Rect area{0, 0, 1440, 900};
        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 60, 3800, 150}, .label = "ultra-wide-a"},
            {.index = 1, .natural = {-600, 355, 4200, 140}, .label = "ultra-wide-b"},
            {.index = 2, .natural = {900, 120, 2900, 160}, .label = "ultra-wide-c"},
            {.index = 3, .natural = {80, -900, 180, 3600}, .label = "ultra-tall-a"},
            {.index = 4, .natural = {560, 20, 160, 2800}, .label = "ultra-tall-b"},
        };

        const auto slots = engine.compute(windows, area, config);
        ok &= expect(slots.size() == windows.size(), "natural engine should keep extreme-aspect windows");
        for (const auto& slot : slots)
            ok &= expect(rectInside(slot.target, area), "natural engine should fit extreme-aspect targets inside the area");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.rowSpacing = 8.0;
        config.columnSpacing = 8.0;
        config.minSlotScale = 0.05;

        const Rect area{0, 0, 1440, 900};
        std::vector<WindowInput> windows;
        for (std::size_t i = 0; i < 120; ++i) {
            const double x = static_cast<double>((i * 73) % 1320);
            const double y = static_cast<double>((i * 47) % 780);
            const double width = 60.0 + static_cast<double>((i * 19) % 160);
            const double height = 50.0 + static_cast<double>((i * 23) % 130);
            windows.push_back({.index = i, .natural = {x, y, width, height}, .label = "dense"});
        }

        const auto slots = engine.compute(windows, area, config);
        ok &= expect(slots.size() == windows.size(), "dense natural requests should fall back without dropping windows");
        for (const auto& slot : slots)
            ok &= expect(rectInside(slot.target, area), "dense fallback should keep targets inside the area");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.naturalScaleFlex = 0.16;
        config.rankScaleByInputOrder = true;
        config.preserveInputOrder = true;
        config.rowSpacing = 24.0;
        config.columnSpacing = 24.0;

        std::vector<WindowInput> windows;
        for (std::size_t i = 0; i < 6; ++i)
            windows.push_back({.index = i, .natural = {static_cast<double>(i) * 40.0, 60.0, 360.0, 260.0}, .label = "recent"});

        const Rect area{0, 0, 1300, 760};
        const auto slots = engine.compute(windows, area, config);
        ok &= expect(slots.size() == windows.size(), "recent-rank natural scaling should keep all windows");
        for (std::size_t i = 1; i < slots.size(); ++i)
            ok &= expect(slots[i - 1].scale > slots[i].scale, "recent-rank natural scaling should decrease scale by input order");
        for (const auto& slot : slots)
            ok &= expect(rectInside(slot.target, area), "recent-rank natural scaling should keep targets inside the area");
    }

    {
        LayoutConfig config = deterministicConfig();
        config.engine = LayoutEngine::Natural;
        config.forceRowGroups = true;
        config.rowSpacing = 40.0;
        config.columnSpacing = 24.0;

        const std::vector<WindowInput> windows = {
            {.index = 0, .natural = {0, 0, 240, 180}, .label = "a", .rowGroup = 0},
            {.index = 1, .natural = {300, 20, 240, 180}, .label = "b", .rowGroup = 0},
            {.index = 2, .natural = {0, 260, 240, 180}, .label = "c", .rowGroup = 1},
            {.index = 3, .natural = {300, 280, 240, 180}, .label = "d", .rowGroup = 1},
        };

        const Rect area{0, 0, 700, 500};
        const auto slots = engine.compute(windows, area, config);
        ok &= expect(slots.size() == windows.size(), "row-group natural layout should preserve cardinality");
        for (const auto& slot : slots)
            ok &= expect(rectInside(slot.target, area), "row-group natural layout should stay inside the area");
        ok &= expect(slots[0].target.centerY() < slots[2].target.centerY() && slots[1].target.centerY() < slots[3].target.centerY(),
                     "row-group natural layout should keep workspace bands ordered");
        ok &= expect(slots[0].target.centerX() < slots[1].target.centerX() && slots[2].target.centerX() < slots[3].target.centerX(),
                     "row-group natural layout should preserve within-band left/right order");
    }

    ok &= natural2Regression(engine);
    ok &= natural2WarmTopologyRegression(engine);
    ok &= overviewLayoutSessionRegression();
    ok &= natural2WorkspaceAndRowRegression(engine);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
