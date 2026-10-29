#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hymission::tiles {
using Workspace = int64_t;
using Id = uint64_t;
constexpr size_t maxTiles = 3;
struct Box {
    double x = 0, y = 0, width = 0, height = 0;
    bool contains(double px, double py) const;
    bool operator==(const Box&) const = default;
};
struct Tile {
    Id id = 0;
    Workspace workspace = 0;
    double width = 0;
    std::vector<Workspace> history;
    bool operator==(const Tile&) const = default;
};
struct State {
    Box output;
    double minimum = 240;
    std::vector<Tile> tiles;
    Id focused = 0, nextId = 1;
    uint64_t generation = 0;
    bool operator==(const State&) const = default;
};
enum class Activation { Focus, Swap };
using Result = std::expected<void, std::string>;
// Workspace id changes applied all at once (old id -> new id).
using Renumbering = std::vector<std::pair<Workspace, Workspace>>;
// Displayed workspaces and histories follow renumbered workspaces.
State renumbered(State state, const Renumbering& changes);

// One physical output. All mutations validate a draft and publish it once.
// Workspace identity belongs to the adapter; display names never participate.
class Model {
  public:
    explicit Model(Box output, Workspace initial, double minimum = 240);
    const State& state() const { return m_state; }
    Result add(Workspace workspace, std::optional<double> width = {}, bool before = false);
    Result resize(double width, int neighbor = 0);
    Result resizePair(size_t left, double leftWidth);
    Result remove();
    Result focus(int step);
    Result activate(Workspace workspace, Activation policy = Activation::Focus);
    Result geometry(Box output, double minimum);
    Result restore(const State& snapshot, uint64_t expectedGeneration);
    std::optional<size_t> index(Id id) const;
    std::optional<size_t> presented(Workspace workspace) const;
    std::optional<size_t> at(double x, double y) const;
    Box box(size_t index) const;
    std::vector<int> pixelEdges(double scale) const;
    std::optional<Workspace> previous() const;
    static bool valid(const State& state);
  private:
    State m_state;
    Result publish(State draft);
};
// One ownership table per physical output, shared by presentation and navigation.
class Ownership {
  public:
    Id owner(const State&, Workspace) const;
    void claim(Workspace workspace, Id tile) { m_homes[workspace] = tile; }
    void forget(Workspace workspace) { m_homes.erase(workspace); }
    void renumber(const Renumbering& changes);
    void sync(const State&);
    std::optional<Workspace> previous(const State&, Id, const std::vector<Workspace>& alive) const;
  private:
    std::map<Workspace, Id> m_homes;
    std::vector<Id> m_order;
};
bool relativeSelector(const std::string&);
// Ordered existing owned workspaces; unavailable also includes physical monitor rules.
std::optional<Workspace> relativeTarget(const std::string&, Workspace current, const std::vector<Workspace>& ordered,
                                       const std::set<Workspace>& unavailable = {});
class DividerDrag {
  public:
    static std::expected<DividerDrag, std::string> begin(const Model&, size_t left, double pointerX);
    Result update(Model&, double pointerX);
    Result cancel(Model&);
    uint64_t generation() const { return m_generation; }
  private:
    State m_initial;
    size_t m_left = 0;
    double m_x = 0;
    uint64_t m_generation = 0;
};
std::expected<double, std::string> length(const std::string& value, double total, bool delta = false);
std::vector<Box> thumbnailGrid(Box area, size_t count, bool horizontal, double aspect);
enum class DropKind { Cancel, Invalid, Move, SplitLeft, SplitRight };
struct Drop {
    DropKind kind = DropKind::Cancel;
    Id target = 0;
    Box preview;
    double divider = 0;
};
Drop dropTarget(const Model&, Id source, double x, double y, bool sidebar = false);
} // namespace hymission::tiles
