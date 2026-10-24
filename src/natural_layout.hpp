#pragma once

// Natural2 overview layout core.
//
// One common content scale for every window (only small_window_boost and
// layoutEmphasis vary it per window), structure-preserving placement, and an
// optional session context that keeps surviving windows in place.
//
//  * Candidate A (geometry): a group whose real windows do not overlap is
//    shrunk as a whole; one shared shrink factor opens the configured spacing.
//  * Candidate B (rows): scrolling columns stay units; units are read in band
//    order (tiled) or strict y order (overlapping/floating), cut into rows by
//    an O(n^2) DP at the largest feasible scale, and placed by isotonic
//    regression towards their real x.
//  * Capacity that cannot hold the configured spacing degrades in a fixed
//    order and reports it through LayoutDegradation.

#include "mission_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace hymission::natural2 {

namespace detail {

constexpr double kPositionFidelity = 0.0;   // share of the real x kept inside a row's slack
constexpr double kGeometryPreference = 0.9; // A wins while its coverage >= this * B's
constexpr double kMinAffineShortEdge = 48.0;
constexpr double kMaxStackGap = 64.0;       // fallback column detection: largest real gap between members
constexpr double kColumnXTolerance = 24.0;  // fallback column detection: x drift (borders, jitter)
constexpr double kColumnWidthTolerance = 2.0;
constexpr double kOverlapShare = 0.02;      // overlap above this share of the smaller window counts
constexpr double kBandOverlap = 0.5;
constexpr double kSplitColumnGain = 1.15;
constexpr double kSpacingFloorShare = 0.25;
constexpr double kSpacingFloor = 4.0;
constexpr double kResizeTrigger = 0.6; // try a local re-placement when a resized window keeps < this share of its accepted area

struct Member {
    std::size_t input = 0; // position in the input vector
    double      w = 0.0;   // layout size incl. boost, before the common scale
    double      h = 0.0;
};

struct Unit {
    std::vector<Member> members; // secondary-axis order
    bool                horizontalStack = false; // members side by side (vertical scrolling)
    Rect                nat;     // union of the members' real rects
    std::size_t         group = 0;
    double              w = 0.0, h = 0.0;   // scalable extent
    double              fixedW = 0.0, fixedH = 0.0; // spacing inside the stack (px, not scaled)
    double              maxBoost = 1.0;
    std::optional<std::uint64_t> columnId;
};

struct Spacing {
    double column = 0.0;
    double row = 0.0;
};

struct Placement {
    std::vector<WindowSlot> slots;      // input order
    std::vector<Rect>       allocations; // input order
    double                  coverage = 0.0;
    double                  scale = 0.0; // common scale S (B) or content scale S*k (A)
    bool                    affine = false;
    std::optional<LayoutAffineFrame> frame;
    std::vector<LayoutHistoryBand> bands;
};

inline Rect insetArea(const Rect& area, const LayoutConfig& c) {
    return {area.x + c.outerPaddingLeft, area.y + c.outerPaddingTop, area.width - c.outerPaddingLeft - c.outerPaddingRight,
            area.height - c.outerPaddingTop - c.outerPaddingBottom};
}

inline double layoutLength(double v, const LayoutConfig& c) {
    return std::max(v, c.minWindowLength);
}

inline double boostFor(const WindowInput& w, const Rect& inner, const LayoutConfig& c) {
    const double ratio = std::clamp(layoutLength(w.natural.height, c) / std::max(1.0, inner.height), 0.0, 1.0);
    const double boost = c.smallWindowBoost + (1.0 - c.smallWindowBoost) * ratio;
    return std::max(1e-3, boost);
}

// layoutEmphasis (hover) never changes the topology: it only grows a single-window unit inside its row's slack
inline double emphasisOf(const WindowInput& w) {
    return std::max(1.0, w.layoutEmphasis);
}

inline std::size_t groupOf(const WindowInput& w, const LayoutConfig& c) {
    return c.forceRowGroups ? w.rowGroup : 0;
}

inline bool sourcesOverlap(const Rect& a, const Rect& b) {
    const double w = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const double h = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    return w > 1.0 && h > 1.0 && w * h > kOverlapShare * std::min(a.width * a.height, b.width * b.height);
}

// strict identity tie-break shared by every sort
inline std::tuple<std::uint64_t, std::size_t> identity(const WindowInput& w) {
    return {w.stableId.value_or(std::numeric_limits<std::uint64_t>::max()), w.index};
}

inline void finalizeUnit(Unit& u, const std::vector<WindowInput>& wins, const Spacing& sp) {
    u.w = u.h = 0.0;
    u.maxBoost = 0.0;
    double minX = std::numeric_limits<double>::infinity(), minY = minX, maxX = -minX, maxY = -minX;
    for (const auto& m : u.members) {
        const auto& r = wins[m.input].natural;
        minX = std::min(minX, r.x);
        minY = std::min(minY, r.y);
        maxX = std::max(maxX, r.x + r.width);
        maxY = std::max(maxY, r.y + r.height);
        if (u.horizontalStack) {
            u.w += m.w;
            u.h = std::max(u.h, m.h);
        } else {
            u.w = std::max(u.w, m.w);
            u.h += m.h;
        }
    }
    const double gaps = static_cast<double>(u.members.size() - 1);
    u.fixedW = u.horizontalStack ? gaps * sp.column : 0.0;
    u.fixedH = u.horizontalStack ? 0.0 : gaps * sp.row;
    u.nat = {minX, minY, maxX - minX, maxY - minY};
}

inline std::vector<Unit> buildUnits(const std::vector<WindowInput>& wins, const std::vector<double>& boost, const LayoutConfig& c, const Spacing& sp,
                                    bool splitColumns) {
    std::vector<Unit> units;
    const auto member = [&](std::size_t i) {
        return Member{i, layoutLength(wins[i].natural.width, c) * boost[i], layoutLength(wins[i].natural.height, c) * boost[i]};
    };
    const auto canStack = [&](const WindowInput& w) { return !splitColumns && w.floating != true; };

    // 1. explicit column metadata
    std::map<std::tuple<std::size_t, std::uint64_t>, std::vector<std::size_t>> byColumn;
    std::vector<std::size_t> rest;
    for (std::size_t i = 0; i < wins.size(); ++i) {
        if (wins[i].columnId && canStack(wins[i]))
            byColumn[{groupOf(wins[i], c), *wins[i].columnId}].push_back(i);
        else
            rest.push_back(i);
    }
    for (auto& [key, idx] : byColumn) {
        std::sort(idx.begin(), idx.end(), [&](auto a, auto b) {
            return std::tuple(wins[a].columnMemberIndex, identity(wins[a])) < std::tuple(wins[b].columnMemberIndex, identity(wins[b]));
        });
        Unit u;
        u.group = std::get<0>(key);
        u.columnId = std::get<1>(key);
        u.horizontalStack = wins[idx.front()].columnAxis == LayoutAxis::Vertical;
        for (auto i : idx)
            u.members.push_back(member(i));
        units.push_back(std::move(u));
    }

    // 2. geometric fallback for callers without metadata: same x/width, vertically adjacent.
    // Columns start at their top member, so x drift between members cannot split them.
    std::sort(rest.begin(), rest.end(), [&](auto a, auto b) {
        const auto& A = wins[a];
        const auto& B = wins[b];
        return std::tuple(groupOf(A, c), A.natural.y, A.natural.x, identity(A)) < std::tuple(groupOf(B, c), B.natural.y, B.natural.x, identity(B));
    });
    std::vector<bool> used(wins.size(), false);
    for (std::size_t a = 0; a < rest.size(); ++a) {
        const std::size_t i = rest[a];
        if (used[i])
            continue;
        used[i] = true;
        Unit u;
        u.group = groupOf(wins[i], c);
        u.members.push_back(member(i));
        if (canStack(wins[i]) && !wins[i].columnId) {
            // greedily extend downwards with the nearest compatible window below the current bottom member
            for (;;) {
                const auto& last = wins[u.members.back().input].natural;
                std::optional<std::size_t> next;
                for (std::size_t b = 0; b < rest.size(); ++b) {
                    const std::size_t j = rest[b];
                    if (used[j] || !canStack(wins[j]) || groupOf(wins[j], c) != u.group || wins[j].groupId != wins[i].groupId ||
                        wins[j].rowGroup != wins[i].rowGroup)
                        continue;
                    const auto& r = wins[j].natural;
                    const double gap = r.y - (last.y + last.height);
                    if (std::abs(r.x - last.x) > kColumnXTolerance || std::abs(r.width - last.width) > kColumnWidthTolerance || gap < -16.0 ||
                        gap > kMaxStackGap)
                        continue;
                    if (!next || std::tuple(r.y, identity(wins[j])) < std::tuple(wins[*next].natural.y, identity(wins[*next])))
                        next = j;
                }
                if (!next)
                    break;
                used[*next] = true;
                u.members.push_back(member(*next));
            }
        }
        units.push_back(std::move(u));
    }
    for (auto& u : units)
        finalizeUnit(u, wins, sp);
    for (auto& u : units)
        for (const auto& m : u.members)
            u.maxBoost = std::max(u.maxBoost, boost[m.input]);
    return units;
}

// reading order of units inside one group
inline std::vector<std::size_t> groupOrder(const std::vector<Unit>& u, const std::vector<std::size_t>& ids, const std::vector<WindowInput>& wins,
                                           bool yOrder) {
    const auto key = [&](std::size_t i) { return identity(wins[u[i].members.front().input]); };
    std::vector<std::size_t> ord = ids;
    // explicit column order wins when every unit of the group carries it
    const bool allColumns = std::all_of(ord.begin(), ord.end(), [&](auto i) { return u[i].columnId.has_value(); });
    if (allColumns) {
        std::sort(ord.begin(), ord.end(), [&](auto a, auto b) {
            return std::tuple(wins[u[a].members.front().input].columnIndex, key(a)) < std::tuple(wins[u[b].members.front().input].columnIndex, key(b));
        });
        return ord;
    }
    std::sort(ord.begin(), ord.end(), [&](auto a, auto b) {
        return std::tuple(u[a].nat.y, u[a].nat.x, key(a)) < std::tuple(u[b].nat.y, u[b].nat.x, key(b));
    });
    if (yOrder)
        return ord;
    std::vector<std::vector<std::size_t>> bands;
    double top = 0.0, bottom = 0.0;
    for (auto i : ord) {
        const auto& r = u[i].nat;
        const bool same = !bands.empty() && std::min(bottom, r.y + r.height) - std::max(top, r.y) > kBandOverlap * std::min(bottom - top, r.height);
        if (same) {
            bands.back().push_back(i);
            top = std::min(top, r.y);
            bottom = std::max(bottom, r.y + r.height);
        } else {
            bands.push_back({i});
            top = r.y;
            bottom = r.y + r.height;
        }
    }
    std::vector<std::size_t> out;
    for (auto& b : bands) {
        std::sort(b.begin(), b.end(), [&](auto a, auto c) { return std::tuple(u[a].nat.centerX(), key(a)) < std::tuple(u[c].nat.centerX(), key(c)); });
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

inline double unitW(const Unit& u, double s) {
    return u.w * s + u.fixedW;
}

inline double unitH(const Unit& u, double s) {
    return u.h * s + u.fixedH;
}

// minimum total height cutting `ord` into rows of width <= W; rows never span groups
inline bool cutRows(const std::vector<Unit>& u, const std::vector<std::size_t>& ord, double s, const Rect& in, const Spacing& sp,
                    std::vector<std::vector<std::size_t>>* rows) {
    const std::size_t   n = ord.size();
    std::vector<double> best(n + 1, std::numeric_limits<double>::infinity());
    std::vector<std::size_t> from(n + 1, 0);
    best[0] = 0.0;
    for (std::size_t j = 1; j <= n; ++j) {
        double width = 0.0, height = 0.0;
        for (std::size_t i = j; i-- > 0;) {
            if (u[ord[i]].group != u[ord[j - 1]].group)
                break;
            width += unitW(u[ord[i]], s) + (i + 1 < j ? sp.column : 0.0);
            if (width > in.width + 1e-6)
                break;
            height = std::max(height, unitH(u[ord[i]], s));
            const double cand = best[i] + height + (i > 0 ? sp.row : 0.0);
            if (cand < best[j] - 1e-9) {
                best[j] = cand;
                from[j] = i;
            }
        }
    }
    if (!(best[n] <= in.height + 1e-6))
        return false;
    if (rows) {
        rows->clear();
        for (std::size_t j = n; j > 0; j = from[j])
            rows->emplace_back(ord.begin() + static_cast<std::ptrdiff_t>(from[j]), ord.begin() + static_cast<std::ptrdiff_t>(j));
        std::reverse(rows->begin(), rows->end());
    }
    return true;
}

// isotonic regression with a box clamp (pool adjacent violators)
inline std::vector<double> isotonic(const std::vector<double>& v, double lo, double hi) {
    struct Block {
        double sum;
        double count;
    };
    std::vector<Block> st;
    for (double x : v) {
        st.push_back({x, 1.0});
        while (st.size() > 1 && st[st.size() - 2].sum / st[st.size() - 2].count > st.back().sum / st.back().count) {
            const auto b = st.back();
            st.pop_back();
            st.back().sum += b.sum;
            st.back().count += b.count;
        }
    }
    std::vector<double> out;
    for (const auto& b : st)
        for (int k = 0; k < static_cast<int>(b.count); ++k)
            out.push_back(hi < lo ? (lo + hi) / 2.0 : std::clamp(b.sum / b.count, lo, hi));
    return out;
}

// centers of items with lengths `len`, kept in order with gap g inside [L,R], nearest to `anchor`
inline std::vector<double> placeInOrder(const std::vector<double>& len, const std::vector<double>& anchor, double L, double R, double g) {
    const std::size_t   n = len.size();
    std::vector<double> offset(n, 0.0), shifted(n, 0.0);
    for (std::size_t i = 1; i < n; ++i)
        offset[i] = offset[i - 1] + len[i - 1] / 2.0 + g + len[i] / 2.0;
    for (std::size_t i = 0; i < n; ++i)
        shifted[i] = anchor[i] - offset[i];
    const auto base = isotonic(shifted, L + len[0] / 2.0, R - len[n - 1] / 2.0 - offset[n - 1]);
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = base[i] + offset[i];
    return out;
}

inline double scaleCap(const std::vector<Unit>& units, const Rect& in, const LayoutConfig& c) {
    double cap = std::numeric_limits<double>::infinity();
    for (const auto& u : units) {
        cap = std::min(cap, c.maxPreviewScale / u.maxBoost);
        cap = std::min(cap, std::max(0.0, in.width - u.fixedW) / std::max(1e-9, u.w));
        cap = std::min(cap, std::max(0.0, in.height - u.fixedH) / std::max(1e-9, u.h));
    }
    return std::max(0.0, cap);
}

// candidate B: rows
inline std::optional<Placement> rowsPlacement(const std::vector<WindowInput>& wins, const std::vector<double>& boost, const Rect& in, const LayoutConfig& c,
                                              const Spacing& sp, bool splitColumns) {
    auto units = buildUnits(wins, boost, c, sp, splitColumns);
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < units.size(); ++i)
        groups[units[i].group].push_back(i);
    std::vector<std::size_t> order;
    std::map<std::size_t, bool> groupYOrder;
    for (const auto& [g, ids] : groups) {
        bool overlapping = false;
        for (std::size_t a = 0; a < ids.size() && !overlapping; ++a)
            for (std::size_t b = a + 1; b < ids.size() && !overlapping; ++b)
                for (const auto& ma : units[ids[a]].members)
                    for (const auto& mb : units[ids[b]].members)
                        overlapping = overlapping || sourcesOverlap(wins[ma.input].natural, wins[mb.input].natural);
        const bool anyFloating = std::any_of(ids.begin(), ids.end(), [&](auto i) {
            return std::any_of(units[i].members.begin(), units[i].members.end(), [&](const Member& m) { return wins[m.input].floating == true; });
        });
        groupYOrder[g] = overlapping || anyFloating;
        const auto o = groupOrder(units, ids, wins, groupYOrder[g]);
        order.insert(order.end(), o.begin(), o.end());
    }

    const double cap = scaleCap(units, in, c);
    double       lo = 0.0, hi = cap;
    if (cutRows(units, order, hi, in, sp, nullptr))
        lo = hi;
    else {
        if (!cutRows(units, order, 0.0, in, sp, nullptr))
            return std::nullopt;
        for (int step = 0; step < 40; ++step) {
            const double mid = (lo + hi) / 2.0;
            (cutRows(units, order, mid, in, sp, nullptr) ? lo : hi) = mid;
        }
    }
    const double s = lo;
    if (!(s > 0.0))
        return std::nullopt;
    std::vector<std::vector<std::size_t>> rows;
    cutRows(units, order, s, in, sp, &rows);
    for (auto& row : rows) {
        if (!groupYOrder[units[row.front()].group])
            continue; // band/column order is already the display order
        std::sort(row.begin(), row.end(), [&](auto a, auto b) {
            return std::tuple(units[a].nat.centerX(), identity(wins[units[a].members.front().input])) <
                std::tuple(units[b].nat.centerX(), identity(wins[units[b].members.front().input]));
        });
    }

    Placement p;
    p.scale = s;
    p.slots.resize(wins.size());
    p.allocations.resize(wins.size());
    std::vector<double> rowH;
    double              total = rows.empty() ? 0.0 : static_cast<double>(rows.size() - 1) * sp.row;
    for (const auto& row : rows) {
        double h = 0.0;
        for (auto i : row)
            h = std::max(h, unitH(units[i], s));
        rowH.push_back(h);
        total += h;
    }
    double y = in.y + (in.height - total) / 2.0;
    double area = 0.0;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto& row = rows[r];
        double      minX = std::numeric_limits<double>::infinity(), maxX = -minX;
        double      rowW = static_cast<double>(row.size() - 1) * sp.column;
        for (auto i : row) {
            minX = std::min(minX, units[i].nat.x);
            maxX = std::max(maxX, units[i].nat.x + units[i].nat.width);
            rowW += unitW(units[i], s);
        }
        std::vector<double> len, anchor;
        double              x = in.x + (in.width - rowW) / 2.0;
        for (auto i : row) {
            const double l = unitW(units[i], s);
            const double centered = x + l / 2.0;
            x += l + sp.column;
            const double mapped = in.x + (units[i].nat.centerX() - minX) / std::max(1.0, maxX - minX) * in.width;
            len.push_back(l);
            anchor.push_back(centered + kPositionFidelity * (mapped - centered));
        }
        auto centers = placeInOrder(len, anchor, in.x, in.x + in.width, sp.column);
        // emphasis: grow within the row height and the row's horizontal slack, neighbours give way by PAVA
        std::vector<double> grow(row.size(), 1.0);
        bool                emphasized = false;
        for (std::size_t k = 0; k < row.size(); ++k) {
            const auto& unit = units[row[k]];
            if (unit.members.size() != 1)
                continue;
            const double e = emphasisOf(wins[unit.members.front().input]);
            if (e <= 1.0)
                continue;
            const double maxScale = c.maxPreviewScale / std::max(1e-9, boost[unit.members.front().input] * s);
            grow[k] = std::max(1.0, std::min({e, rowH[r] / std::max(1e-9, unitH(unit, s)), maxScale}));
            emphasized = emphasized || grow[k] > 1.0;
        }
        if (emphasized) {
            double used = static_cast<double>(row.size() - 1) * sp.column;
            for (std::size_t k = 0; k < row.size(); ++k)
                used += len[k] * grow[k];
            if (used > in.width) { // not enough slack: shrink the growth uniformly back towards 1
                double extra = 0.0;
                for (std::size_t k = 0; k < row.size(); ++k)
                    extra += len[k] * (grow[k] - 1.0);
                const double t = std::max(0.0, (in.width - (used - extra)) / std::max(1e-9, extra));
                for (auto& g : grow)
                    g = 1.0 + (g - 1.0) * t;
            }
            std::vector<double> grownLen(row.size());
            for (std::size_t k = 0; k < row.size(); ++k)
                grownLen[k] = len[k] * grow[k];
            centers = placeInOrder(grownLen, centers, in.x, in.x + in.width, sp.column);
        }

        LayoutHistoryBand band;
        band.rowGroup = wins[units[row.front()].members.front().input].rowGroup;
        band.groupId = wins[units[row.front()].members.front().input].groupId;
        band.allocation = {in.x, y, in.width, rowH[r]};
        for (std::size_t k = 0; k < row.size(); ++k) {
            const auto&  unit = units[row[k]];
            const double cw = unitW(unit, s) * grow[k], ch = unitH(unit, s) * grow[k];
            double       cx = centers[k] - cw / 2.0;
            double       cy = y + (rowH[r] - ch) / 2.0;
            LayoutHistoryUnit hu{.columnId = unit.columnId, .members = {}, .allocation = {cx, cy, cw, ch}};
            for (const auto& m : unit.members) {
                const auto&  w = wins[m.input];
                const double mw = m.w * s * grow[k], mh = m.h * s * grow[k];
                Rect         alloc = unit.horizontalStack ? Rect{cx, cy + (ch - mh) / 2.0, mw, mh} : Rect{cx + (cw - mw) / 2.0, cy, mw, mh};
                const double sc = std::min(c.maxPreviewScale, boost[m.input] * s * grow[k]);
                const Rect   t{alloc.centerX() - w.natural.width * sc / 2.0, alloc.centerY() - w.natural.height * sc / 2.0, w.natural.width * sc,
                             w.natural.height * sc};
                p.slots[m.input] = {.index = w.index, .natural = w.natural, .target = t, .scale = sc, .rowGroup = w.rowGroup};
                p.allocations[m.input] = alloc;
                area += t.width * t.height;
                if (w.stableId)
                    hu.members.push_back(*w.stableId);
                if (unit.horizontalStack)
                    cx += mw + sp.column;
                else
                    cy += mh + sp.row;
            }
            band.units.push_back(std::move(hu));
        }
        p.bands.push_back(std::move(band));
        y += rowH[r] + sp.row;
    }
    p.coverage = area / std::max(1.0, in.width * in.height);
    return p;
}

// candidate A: uniform shrink of the real geometry (single group, no overlaps)
inline std::optional<Placement> affinePlacement(const std::vector<WindowInput>& wins, const Rect& in, const LayoutConfig& c, const Spacing& sp) {
    if (wins.empty())
        return std::nullopt;
    for (std::size_t i = 0; i < wins.size(); ++i) {
        if (wins[i].natural.width <= 0.0 || wins[i].natural.height <= 0.0)
            return std::nullopt;
        for (std::size_t j = i + 1; j < wins.size(); ++j) {
            if (groupOf(wins[i], c) != groupOf(wins[j], c) || sourcesOverlap(wins[i].natural, wins[j].natural))
                return std::nullopt;
        }
    }
    double minX = std::numeric_limits<double>::infinity(), minY = minX, maxX = -minX, maxY = -minX;
    for (const auto& w : wins) {
        minX = std::min(minX, w.natural.x);
        minY = std::min(minY, w.natural.y);
        maxX = std::max(maxX, w.natural.x + w.natural.width);
        maxY = std::max(maxY, w.natural.y + w.natural.height);
    }
    const Rect   bounds{minX, minY, maxX - minX, maxY - minY};
    const double s = std::min(in.width / bounds.width, in.height / bounds.height);
    // largest shared k such that every pair is apart by the column gap horizontally or the row gap vertically
    double k = 1.0;
    for (std::size_t i = 0; i < wins.size(); ++i)
        for (std::size_t j = i + 1; j < wins.size(); ++j) {
            const auto&  a = wins[i].natural;
            const auto&  b = wins[j].natural;
            const double kx = (std::abs(a.centerX() - b.centerX()) * s - sp.column) / ((a.width + b.width) * s / 2.0);
            const double ky = (std::abs(a.centerY() - b.centerY()) * s - sp.row) / ((a.height + b.height) * s / 2.0);
            k = std::min(k, std::max(kx, ky));
        }
    k = std::min(k, c.maxPreviewScale / s);
    if (!(k > 0.0))
        return std::nullopt;
    const double ox = in.x + (in.width - bounds.width * s) / 2.0;
    const double oy = in.y + (in.height - bounds.height * s) / 2.0;
    Placement    p;
    p.affine = true;
    p.scale = s * k;
    p.frame = LayoutAffineFrame{.sourceBounds = bounds, .centerScale = s};
    double area = 0.0;
    for (const auto& w : wins) {
        const double cx = ox + (w.natural.centerX() - minX) * s;
        const double cy = oy + (w.natural.centerY() - minY) * s;
        const Rect   t{cx - w.natural.width * s * k / 2.0, cy - w.natural.height * s * k / 2.0, w.natural.width * s * k, w.natural.height * s * k};
        if (std::min(t.width, t.height) < kMinAffineShortEdge)
            return std::nullopt;
        p.slots.push_back({.index = w.index, .natural = w.natural, .target = t, .scale = s * k, .rowGroup = w.rowGroup});
        p.allocations.push_back(t);
        area += t.width * t.height;
    }
    // emphasis: grow in place as far as the spacing to every other slot and the inner area allow
    for (std::size_t i = 0; i < wins.size(); ++i) {
        const double e = emphasisOf(wins[i]);
        if (e <= 1.0)
            continue;
        const Rect& t = p.slots[i].target;
        double      f = std::min({e, c.maxPreviewScale / p.scale, 2.0 * (t.centerX() - in.x) / t.width, 2.0 * (in.x + in.width - t.centerX()) / t.width,
                                  2.0 * (t.centerY() - in.y) / t.height, 2.0 * (in.y + in.height - t.centerY()) / t.height});
        for (std::size_t j = 0; j < wins.size(); ++j) {
            if (j == i)
                continue;
            const Rect&  o = p.slots[j].target;
            const double fx = (2.0 * (std::abs(t.centerX() - o.centerX()) - sp.column) - o.width) / t.width;
            const double fy = (2.0 * (std::abs(t.centerY() - o.centerY()) - sp.row) - o.height) / t.height;
            f = std::min(f, std::max(fx, fy));
        }
        if (f <= 1.0)
            continue;
        area -= t.width * t.height;
        const Rect g{t.centerX() - t.width * f / 2.0, t.centerY() - t.height * f / 2.0, t.width * f, t.height * f};
        p.slots[i].target = g;
        p.slots[i].scale *= f;
        p.allocations[i] = g;
        area += g.width * g.height;
    }
    p.coverage = area / std::max(1.0, in.width * in.height);
    return p;
}

inline bool meetsQuality(const Placement& p, const LayoutConfig& c, double* minScale = nullptr, double* minShort = nullptr) {
    double lowScale = std::numeric_limits<double>::infinity(), lowShort = lowScale;
    bool   ok = true;
    for (const auto& s : p.slots) {
        const double sc = s.target.width / std::max(1e-9, s.natural.width);
        lowScale = std::min(lowScale, sc);
        lowShort = std::min(lowShort, std::min(s.target.width, s.target.height));
        const bool capped = sc >= c.maxPreviewScale - 1e-6; // shown at max scale: as legible as it gets
        if (!capped && (sc < c.minSlotScale - 1e-9 || std::min(s.target.width, s.target.height) < c.minPreviewShortEdge - 1e-6))
            ok = false;
    }
    if (minScale)
        *minScale = lowScale;
    if (minShort)
        *minShort = lowShort;
    return ok;
}

inline void describe(const Placement& p, const LayoutConfig& c, const Spacing& sp, bool split, bool emergency, LayoutDegradation& d) {
    d = {};
    d.available = true;
    d.feasible = true;
    d.splitColumns = split;
    d.emergencySpacing = emergency;
    d.effectiveColumnSpacing = sp.column;
    d.effectiveRowSpacing = sp.row;
    for (const auto& s : p.slots) {
        const double sc = s.target.width / std::max(1e-9, s.natural.width);
        if (sc < c.minSlotScale - 1e-9)
            d.belowMinSlotScale = true;
        if (std::min(s.target.width, s.target.height) < c.minPreviewShortEdge - 1e-6)
            d.belowMinShortEdge = true;
    }
}

// cold solve with the degradation ladder
inline std::optional<Placement> solveCold(const std::vector<WindowInput>& wins, const Rect& in, const LayoutConfig& c, LayoutDegradation& deg) {
    std::vector<double> boost;
    for (const auto& w : wins)
        boost.push_back(boostFor(w, in, c));
    const Spacing full{c.columnSpacing, c.rowSpacing};

    const auto best = [&](const Spacing& sp, bool split) -> std::optional<Placement> {
        auto B = rowsPlacement(wins, boost, in, c, sp, split);
        auto A = split ? std::nullopt : affinePlacement(wins, in, c, sp);
        if (A && (!B || A->coverage >= kGeometryPreference * B->coverage))
            return A;
        return B;
    };

    // level 0: configured spacing, structure kept
    auto p = best(full, false);
    if (p && meetsQuality(*p, c)) {
        describe(*p, c, full, false, false, deg);
        return p;
    }
    // level 1: split columns when that is required or clearly better
    bool split = false;
    if (auto q = best(full, true); q && (!p || q->scale >= kSplitColumnGain * p->scale || !meetsQuality(*p, c))) {
        if (!p || meetsQuality(*q, c) || q->scale > p->scale) {
            p = std::move(q);
            split = true;
        }
    }
    if (p && meetsQuality(*p, c)) {
        describe(*p, c, full, split, false, deg);
        return p;
    }
    // level 2: shrink both gaps by one factor down to the floor (never above the configured value)
    const double floorShare = [&] {
        const double base = std::max(c.columnSpacing, c.rowSpacing);
        if (base <= 0.0)
            return 1.0;
        return std::min(1.0, std::max(kSpacingFloorShare, kSpacingFloor / base));
    }();
    const auto at = [&](double f) { return Spacing{c.columnSpacing * f, c.rowSpacing * f}; };
    if (auto q = best(at(floorShare), split); q && meetsQuality(*q, c)) {
        double lo = floorShare, hi = 1.0;
        Placement good = std::move(*q);
        for (int step = 0; step < 16; ++step) {
            const double mid = (lo + hi) / 2.0;
            if (auto r = best(at(mid), split); r && meetsQuality(*r, c)) {
                lo = mid;
                good = std::move(*r);
            } else
                hi = mid;
        }
        describe(good, c, at(lo), split, false, deg);
        return good;
    }
    // level 3: keep the floor spacing and accept what fits
    if (auto q = best(at(floorShare), split)) {
        describe(*q, c, at(floorShare), split, false, deg);
        return q;
    }
    // level 4: emergency, shrink the gaps further (to zero) until a positive layout exists
    double lo = 0.0, hi = floorShare;
    std::optional<Placement> good = best(at(0.0), true);
    if (!good) {
        deg = {};
        deg.available = true;
        deg.feasible = false;
        return std::nullopt;
    }
    for (int step = 0; step < 16; ++step) {
        const double mid = (lo + hi) / 2.0;
        if (auto r = best(at(mid), true)) {
            lo = mid;
            good = std::move(r);
        } else
            hi = mid;
    }
    describe(*good, c, at(lo), true, true, deg);
    return good;
}

inline bool sameConstraints(const LayoutConfig& a, const LayoutConfig& b) {
    return a.engine == b.engine && a.outerPaddingTop == b.outerPaddingTop && a.outerPaddingRight == b.outerPaddingRight &&
        a.outerPaddingBottom == b.outerPaddingBottom && a.outerPaddingLeft == b.outerPaddingLeft && a.rowSpacing == b.rowSpacing &&
        a.columnSpacing == b.columnSpacing && a.minWindowLength == b.minWindowLength && a.smallWindowBoost == b.smallWindowBoost &&
        a.maxPreviewScale == b.maxPreviewScale && a.minPreviewShortEdge == b.minPreviewShortEdge && a.minSlotScale == b.minSlotScale &&
        a.forceRowGroups == b.forceRowGroups;
}

inline bool sameRect(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

inline bool trustedIdentities(const std::vector<WindowInput>& wins) {
    std::set<std::uint64_t> seen;
    for (const auto& w : wins)
        if (!w.stableId || !seen.insert(*w.stableId).second)
            return false;
    return true;
}

// the current content of a resized window, fitted into its reservation
inline Rect containIn(const Rect& natural, const Rect& alloc, const LayoutConfig& c) {
    const double sc = std::min({alloc.width / std::max(1e-9, natural.width), alloc.height / std::max(1e-9, natural.height), c.maxPreviewScale});
    return {alloc.centerX() - natural.width * sc / 2.0, alloc.centerY() - natural.height * sc / 2.0, natural.width * sc, natural.height * sc};
}

inline void remember(LayoutContext& ctx, const std::vector<WindowInput>& wins, const Rect& area, const LayoutConfig& c, const Placement& p,
                     const LayoutDegradation& deg, LayoutReflowKind kind) {
    ctx.valid = true;
    ctx.area = area;
    ctx.config = c;
    ctx.affine = p.affine;
    ctx.baseScale = p.scale;
    ctx.affineFrame = p.affine ? p.frame : std::nullopt;
    ctx.bands = p.affine ? std::vector<LayoutHistoryBand>{} : p.bands;
    ctx.windows.clear();
    for (std::size_t i = 0; i < wins.size(); ++i)
        ctx.windows.push_back({.input = wins[i], .allocation = p.allocations[i], .target = p.slots[i].target, .referenceTarget = p.slots[i].target, .rejectedResizeSize = std::nullopt});
    ctx.lastReflowKind = kind;
    ctx.degradation = deg;
}

inline bool sameGroup(const WindowInput& a, const WindowInput& b) {
    return a.groupId || b.groupId ? a.groupId == b.groupId : a.rowGroup == b.rowGroup;
}

inline bool clearOf(const Rect& r, const LayoutContext& ctx, const Rect& in) {
    const double col = ctx.degradation.effectiveColumnSpacing, row = ctx.degradation.effectiveRowSpacing;
    if (r.x < in.x - 1e-6 || r.y < in.y - 1e-6 || r.x + r.width > in.x + in.width + 1e-6 || r.y + r.height > in.y + in.height + 1e-6)
        return false;
    for (const auto& h : ctx.windows) {
        const auto&  o = h.allocation;
        const double gx = std::max(o.x - (r.x + r.width), r.x - (o.x + o.width));
        const double gy = std::max(o.y - (r.y + r.height), r.y - (o.y + o.height));
        if (gx < col - 1e-6 && gy < row - 1e-6)
            return false;
    }
    return true;
}

inline void shiftWindowX(LayoutContext& ctx, std::uint64_t id, double dx) {
    for (auto& h : ctx.windows)
        if (h.input.stableId == id) {
            h.allocation.x += dx;
            h.target.x += dx;
            h.referenceTarget.x += dx;
        }
}

// A: put the newcomer at its affine-mapped centre, else next to an existing reservation, nearest to that centre
inline bool insertAffine(LayoutContext& ctx, const WindowInput& w, const Rect& in, const LayoutConfig& c) {
    if (!ctx.affineFrame || w.columnId)
        return false;
    for (const auto& h : ctx.windows)
        if (!sameGroup(h.input, w))
            return false;
    const auto&  f = *ctx.affineFrame;
    const double s = f.centerScale;
    const double ox = in.x + (in.width - f.sourceBounds.width * s) / 2.0, oy = in.y + (in.height - f.sourceBounds.height * s) / 2.0;
    const double mx = ox + (w.natural.centerX() - f.sourceBounds.x) * s, my = oy + (w.natural.centerY() - f.sourceBounds.y) * s;
    const double tw = w.natural.width * ctx.baseScale, th = w.natural.height * ctx.baseScale;
    const double col = ctx.degradation.effectiveColumnSpacing, row = ctx.degradation.effectiveRowSpacing;
    std::vector<std::pair<double, double>> centres{{mx, my}};
    for (const auto& h : ctx.windows) {
        const auto& o = h.allocation;
        const double ys[] = {o.centerY(), o.y + th / 2.0, o.y + o.height - th / 2.0, my};
        const double xs[] = {o.centerX(), o.x + tw / 2.0, o.x + o.width - tw / 2.0, mx};
        for (double y : ys) {
            centres.push_back({o.x - col - tw / 2.0, y});
            centres.push_back({o.x + o.width + col + tw / 2.0, y});
        }
        for (double x : xs) {
            centres.push_back({x, o.y - row - th / 2.0});
            centres.push_back({x, o.y + o.height + row + th / 2.0});
        }
    }
    std::optional<Rect> best;
    double              bestD = std::numeric_limits<double>::infinity();
    for (const auto& [x, y] : centres) {
        const Rect r{x - tw / 2.0, y - th / 2.0, tw, th};
        const double d = std::hypot(x - mx, y - my);
        if (d < bestD - 1e-9 && clearOf(r, ctx, in)) {
            best = r;
            bestD = d;
        }
    }
    if (!best || std::min(best->width, best->height) < kMinAffineShortEdge)
        return false;
    ctx.windows.push_back({.input = w, .allocation = *best, .target = *best, .referenceTarget = *best, .rejectedResizeSize = std::nullopt});
    (void)c;
    return true;
}

// B: insert the newcomer as a single-window unit into one band of its group at the session scale; only that band moves
inline bool insertIntoBand(LayoutContext& ctx, const WindowInput& w, const Rect& in, const LayoutConfig& c) {
    const double s = ctx.baseScale;
    const double col = ctx.degradation.effectiveColumnSpacing;
    const double b = boostFor(w, in, c);
    const double uw = layoutLength(w.natural.width, c) * b * s, uh = layoutLength(w.natural.height, c) * b * s;
    std::map<std::uint64_t, const LayoutHistoryWindow*> byId;
    for (const auto& h : ctx.windows)
        if (h.input.stableId)
            byId[*h.input.stableId] = &h;
    // a new member of an existing column changes that column's stack: only a cold solve can rebuild it
    if (w.columnId)
        for (const auto& band : ctx.bands)
            for (const auto& u : band.units)
                if (u.columnId == w.columnId)
                    return false;
    const auto columnIndexOf = [&](const LayoutHistoryUnit& u) -> std::optional<std::size_t> {
        if (!u.columnId || u.members.empty())
            return std::nullopt;
        const auto it = byId.find(u.members.front());
        return it == byId.end() ? std::nullopt : std::optional(it->second->input.columnIndex);
    };
    // candidate bands of the group: most vertical overlap with the newcomer's real rect first, later bands on ties
    std::vector<std::pair<double, std::size_t>> candidates;
    for (std::size_t bi = 0; bi < ctx.bands.size(); ++bi) {
        const auto& band = ctx.bands[bi];
        if (band.units.empty() || band.units.front().members.empty())
            continue;
        const auto it = byId.find(band.units.front().members.front());
        if (it == byId.end() || !sameGroup(it->second->input, w) || uh > band.allocation.height + 1e-6)
            continue;
        double overlap = 0.0;
        for (const auto& u : band.units)
            for (auto id : u.members)
                if (auto m = byId.find(id); m != byId.end()) {
                    const auto& r = m->second->input.natural;
                    overlap = std::max(overlap, std::min(r.y + r.height, w.natural.y + w.natural.height) - std::max(r.y, w.natural.y));
                }
        candidates.push_back({-overlap - 1e-9 * static_cast<double>(bi), bi});
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& [_, bi] : candidates) {
        auto& band = ctx.bands[bi];
        const auto naturalX = [&](const LayoutHistoryUnit& u) {
            const auto it = byId.find(u.members.front());
            return it == byId.end() ? 0.0 : it->second->input.natural.centerX();
        };
        std::size_t pos = 0;
        if (w.columnId) {
            // explicit column order, exactly as the cold solve reads it: every unit of the group must carry it,
            // the band keeps ascending order and bands before/after hold only smaller/larger indices
            bool consistent = true;
            for (std::size_t other = 0; other < ctx.bands.size() && consistent; ++other) {
                const auto& ob = ctx.bands[other];
                if (ob.units.empty() || ob.units.front().members.empty())
                    continue;
                const auto first = byId.find(ob.units.front().members.front());
                if (first == byId.end() || !sameGroup(first->second->input, w))
                    continue;
                for (const auto& u : ob.units) {
                    const auto idx = columnIndexOf(u);
                    if (!idx || *idx == w.columnIndex || (other < bi && *idx > w.columnIndex) || (other > bi && *idx < w.columnIndex))
                        consistent = false;
                }
            }
            if (!consistent)
                continue;
            while (pos < band.units.size() && *columnIndexOf(band.units[pos]) < w.columnIndex)
                ++pos;
        } else
            while (pos < band.units.size() && naturalX(band.units[pos]) <= w.natural.centerX())
                ++pos;
        std::vector<double> len, anchor;
        double              used = col * static_cast<double>(band.units.size());
        for (std::size_t k = 0; k <= band.units.size(); ++k) {
            if (k == pos) {
                double a = in.x + in.width / 2.0;
                if (pos > 0 && pos < band.units.size())
                    a = (band.units[pos - 1].allocation.centerX() + band.units[pos].allocation.centerX()) / 2.0;
                else if (pos > 0)
                    a = band.units[pos - 1].allocation.x + band.units[pos - 1].allocation.width + col + uw / 2.0;
                else if (!band.units.empty())
                    a = band.units[0].allocation.x - col - uw / 2.0;
                len.push_back(uw);
                anchor.push_back(a);
                used += uw;
            }
            if (k < band.units.size()) {
                len.push_back(band.units[k].allocation.width);
                anchor.push_back(band.units[k].allocation.centerX());
                used += band.units[k].allocation.width;
            }
        }
        if (used > in.width + 1e-6)
            continue;
        const auto centers = placeInOrder(len, anchor, in.x, in.x + in.width, col);
        for (std::size_t k = 0, u = 0; k < centers.size(); ++k) {
            if (k == pos)
                continue;
            auto&        unit = band.units[u++];
            const double dx = centers[k] - unit.allocation.centerX();
            unit.allocation.x += dx;
            for (auto id : unit.members)
                shiftWindowX(ctx, id, dx);
        }
        const Rect alloc{centers[pos] - uw / 2.0, band.allocation.y + (band.allocation.height - uh) / 2.0, uw, uh};
        const Rect t = containIn(w.natural, alloc, c);
        LayoutHistoryUnit nu{.columnId = w.columnId, .members = {*w.stableId}, .allocation = alloc};
        band.units.insert(band.units.begin() + static_cast<std::ptrdiff_t>(pos), nu);
        ctx.windows.push_back({.input = w, .allocation = alloc, .target = t, .referenceTarget = t, .rejectedResizeSize = std::nullopt});
        return true;
    }
    return false;
}

// a resized window that lost most of its area tries a bigger reservation that only moves its own band (B)
// or nothing else at all (A); committed by the caller only when the window really gets bigger
inline bool reflowResized(LayoutContext& ctx, const WindowInput& w, const Rect& in, const LayoutConfig& c) {
    LayoutHistoryWindow* self = nullptr;
    for (auto& h : ctx.windows)
        if (h.input.stableId == w.stableId)
            self = &h;
    if (!self)
        return false;
    const Rect   kept = containIn(w.natural, self->allocation, c);
    const double keptArea = kept.width * kept.height;
    const double col = ctx.degradation.effectiveColumnSpacing, row = ctx.degradation.effectiveRowSpacing;
    if (ctx.affine) {
        const double want = std::min(c.maxPreviewScale, ctx.baseScale);
        Rect         g{self->allocation.centerX() - w.natural.width * want / 2.0, self->allocation.centerY() - w.natural.height * want / 2.0,
                       w.natural.width * want, w.natural.height * want};
        double f = std::min({1.0, 2.0 * (g.centerX() - in.x) / g.width, 2.0 * (in.x + in.width - g.centerX()) / g.width,
                             2.0 * (g.centerY() - in.y) / g.height, 2.0 * (in.y + in.height - g.centerY()) / g.height});
        for (const auto& h : ctx.windows) {
            if (&h == self)
                continue;
            const auto&  o = h.allocation;
            const double fx = (2.0 * (std::abs(g.centerX() - o.centerX()) - col) - o.width) / g.width;
            const double fy = (2.0 * (std::abs(g.centerY() - o.centerY()) - row) - o.height) / g.height;
            f = std::min(f, std::max(fx, fy));
        }
        g = {g.centerX() - g.width * f / 2.0, g.centerY() - g.height * f / 2.0, g.width * f, g.height * f};
        if (!(f > 0.0) || g.width * g.height <= keptArea * (1.0 + 1e-6))
            return false;
        self->allocation = self->target = self->referenceTarget = g;
        return true;
    }
    for (auto& band : ctx.bands) {
        const auto unitIt = std::find_if(band.units.begin(), band.units.end(), [&](const LayoutHistoryUnit& u) {
            return u.members.size() == 1 && u.members.front() == *w.stableId;
        });
        if (unitIt == band.units.end())
            continue;
        const std::size_t pos = static_cast<std::size_t>(std::distance(band.units.begin(), unitIt));
        const double      b = boostFor(w, in, c);
        double            uw = layoutLength(w.natural.width, c) * b * ctx.baseScale, uh = layoutLength(w.natural.height, c) * b * ctx.baseScale;
        if (uh > band.allocation.height) { // never taller than the band
            uw *= band.allocation.height / uh;
            uh = band.allocation.height;
        }
        std::vector<double> len, anchor;
        double              used = col * static_cast<double>(band.units.size() - 1);
        for (std::size_t k = 0; k < band.units.size(); ++k) {
            len.push_back(k == pos ? uw : band.units[k].allocation.width);
            anchor.push_back(band.units[k].allocation.centerX());
            used += len.back();
        }
        if (used > in.width + 1e-6) { // shrink the request to the band's slack
            const double room = in.width - (used - uw);
            if (room <= self->allocation.width)
                return false;
            uh *= room / uw;
            uw = room;
            len[pos] = uw;
        }
        const Rect alloc{0.0, band.allocation.y + (band.allocation.height - uh) / 2.0, uw, uh};
        const Rect probe = containIn(w.natural, alloc, c);
        if (probe.width * probe.height <= keptArea * (1.0 + 1e-6))
            return false;
        const auto centers = placeInOrder(len, anchor, in.x, in.x + in.width, col);
        for (std::size_t k = 0; k < band.units.size(); ++k) {
            const double dx = centers[k] - band.units[k].allocation.centerX();
            if (k == pos)
                continue;
            band.units[k].allocation.x += dx;
            for (auto id : band.units[k].members)
                shiftWindowX(ctx, id, dx);
        }
        unitIt->allocation = {centers[pos] - uw / 2.0, alloc.y, uw, uh};
        self->allocation = unitIt->allocation;
        self->target = self->referenceTarget = containIn(w.natural, self->allocation, c);
        return true;
    }
    return false;
}

} // namespace detail

inline std::vector<WindowSlot> compute(const std::vector<WindowInput>& wins, const Rect& area, const LayoutConfig& config, LayoutContext* context = nullptr,
                                       LayoutDegradation* degradation = nullptr) {
    using namespace detail;
    LayoutDegradation deg;
    deg.available = true;
    const Rect in = insetArea(area, config);
    const auto finish = [&](std::vector<WindowSlot> slots) {
        std::sort(slots.begin(), slots.end(), [](const WindowSlot& a, const WindowSlot& b) { return a.index < b.index; });
        if (degradation)
            *degradation = deg;
        return slots;
    };
    if (in.width <= 0.0 || in.height <= 0.0 || config.maxPreviewScale <= 0.0) {
        deg.feasible = false;
        if (context)
            context->reset();
        return finish({});
    }

    const bool trusted = context && trustedIdentities(wins);
    if (context && !trusted && !wins.empty()) {
        context->reset();
        context->lastReflowKind = LayoutReflowKind::StatelessFallback;
    }

    // warm path: every window already has a reservation in the same region and configuration
    if (trusted && context->valid && sameConstraints(context->config, config) && sameRect(context->area, area)) {
        if (wins.empty()) {
            context->lastReflowKind = LayoutReflowKind::Retained;
            deg = context->degradation;
            return finish({});
        }
        std::map<std::uint64_t, LayoutHistoryWindow*> previous;
        for (auto& h : context->windows)
            if (h.input.stableId)
                previous[*h.input.stableId] = &h;
        bool                     subset = true;
        std::vector<std::size_t> newcomers;
        for (std::size_t i = 0; i < wins.size(); ++i) {
            const auto& w = wins[i];
            const auto  it = previous.find(*w.stableId);
            if (it == previous.end()) {
                newcomers.push_back(i);
                continue;
            }
            const auto& old = it->second->input;
            const bool  sameGroup = old.groupId || w.groupId ? old.groupId == w.groupId : old.rowGroup == w.rowGroup;
            // a reservation belongs to one place in the topology: its column, member slot, axis and kind must not change
            const bool sameStructure = old.columnId == w.columnId && old.floating == w.floating &&
                (!w.columnId || (old.columnIndex == w.columnIndex && old.columnMemberIndex == w.columnMemberIndex && old.columnAxis == w.columnAxis));
            if (!sameGroup || !sameStructure) {
                subset = false;
                break;
            }
        }
        // workspace rows: a pure renumbering keeps the reservations, a changed relative order does not.
        // The reference is the group order of the accepted solve (its bands), never per-window ordinals
        // that later warm calls may have rewritten for some windows only.
        if (subset && config.forceRowGroups && !context->bands.empty()) {
            std::map<std::uint64_t, std::size_t> rank;
            std::vector<std::tuple<std::optional<std::int64_t>, std::size_t>> seen;
            for (const auto& band : context->bands) {
                const auto key = std::tuple(band.groupId, band.groupId ? std::size_t{0} : band.rowGroup);
                auto it = std::find(seen.begin(), seen.end(), key);
                if (it == seen.end())
                    it = seen.insert(seen.end(), key);
                for (const auto& unit : band.units)
                    for (auto id : unit.members)
                        rank[id] = static_cast<std::size_t>(std::distance(seen.begin(), it));
            }
            const auto order = [](std::size_t x, std::size_t y) { return x < y ? -1 : x > y ? 1 : 0; };
            for (std::size_t i = 0; i < wins.size() && subset; ++i)
                for (std::size_t j = i + 1; j < wins.size() && subset; ++j) {
                    const auto a = rank.find(*wins[i].stableId), b = rank.find(*wins[j].stableId);
                    if (a == rank.end() || b == rank.end())
                        continue;
                    if (order(a->second, b->second) != order(wins[i].rowGroup, wins[j].rowGroup))
                        subset = false;
                }
        }
        LayoutReflowKind kind = LayoutReflowKind::Retained;
        if (subset && !newcomers.empty()) {
            // speculative copy: committed only when every newcomer found room without touching other bands
            LayoutContext trial = *context;
            for (auto i : newcomers) {
                const bool ok = trial.affine ? insertAffine(trial, wins[i], in, config) : insertIntoBand(trial, wins[i], in, config);
                if (!ok) {
                    subset = false;
                    break;
                }
            }
            if (subset) {
                *context = std::move(trial);
                kind = LayoutReflowKind::LocalInsert;
                previous.clear();
                for (auto& h : context->windows)
                    if (h.input.stableId)
                        previous[*h.input.stableId] = &h;
            }
        }
        if (subset) {
            // resized windows that lost too much area: speculative local re-placement, else remember the rejection
            for (const auto& w : wins) {
                auto* h = previous[*w.stableId];
                if (h->input.natural.width == w.natural.width && h->input.natural.height == w.natural.height)
                    continue;
                const auto size = std::pair(w.natural.width, w.natural.height);
                if (h->rejectedResizeSize == size)
                    continue;
                const Rect   kept = containIn(w.natural, h->allocation, config);
                const double ref = h->referenceTarget.width * h->referenceTarget.height;
                if (kept.width * kept.height >= kResizeTrigger * ref)
                    continue;
                LayoutContext trial = *context;
                if (reflowResized(trial, w, in, config)) {
                    *context = std::move(trial);
                    kind = LayoutReflowKind::LocalReflow;
                    previous.clear();
                    for (auto& x : context->windows)
                        if (x.input.stableId)
                            previous[*x.input.stableId] = &x;
                    // the committed target already reflects the new size
                    for (auto& x : context->windows)
                        if (x.input.stableId == w.stableId) {
                            x.input.natural = w.natural;
                            x.rejectedResizeSize.reset();
                        }
                } else
                    h->rejectedResizeSize = size;
            }
            std::vector<WindowSlot> slots;
            for (const auto& w : wins) {
                const auto* h = previous[*w.stableId];
                const bool  sameSize = h->input.natural.width == w.natural.width && h->input.natural.height == w.natural.height;
                const Rect  t = sameSize ? h->target : containIn(w.natural, h->allocation, config);
                slots.push_back({.index = w.index, .natural = w.natural, .target = t, .scale = t.width / std::max(1e-9, w.natural.width),
                                 .rowGroup = w.rowGroup});
            }
            // reservations of windows that left stay in the snapshot so restored search matches return to their place
            for (auto& h : context->windows)
                for (std::size_t i = 0; i < wins.size(); ++i)
                    if (h.input.stableId == wins[i].stableId) {
                        h.input = wins[i];
                        h.target = slots[i].target;
                    }
            context->lastReflowKind = kind;
            // spacing decisions stay from the accepted solve; quality facts describe what is shown now
            deg = context->degradation;
            deg.belowMinSlotScale = deg.belowMinShortEdge = false;
            for (const auto& sl : slots) {
                if (sl.target.width / std::max(1e-9, sl.natural.width) < config.minSlotScale - 1e-9)
                    deg.belowMinSlotScale = true;
                if (std::min(sl.target.width, sl.target.height) < config.minPreviewShortEdge - 1e-6)
                    deg.belowMinShortEdge = true;
            }
            context->degradation = deg;
            return finish(std::move(slots));
        }
    }

    if (wins.empty())
        return finish({});
    const bool hadHistory = trusted && context->valid;
    auto       placement = solveCold(wins, in, config, deg);
    if (!placement) {
        if (context)
            context->reset();
        return finish({});
    }
    if (trusted)
        remember(*context, wins, area, config, *placement, deg, hadHistory ? LayoutReflowKind::ColdFallback : LayoutReflowKind::ColdInitial);
    return finish(std::move(placement->slots));
}

} // namespace hymission::natural2
