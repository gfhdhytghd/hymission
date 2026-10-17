#include "tiles_logic.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace hymission::tiles {
namespace {
bool finiteBox(Box b) { return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.width) && std::isfinite(b.height) && b.width > 0 && b.height > 0; }
void remember(Tile& t, Workspace w) {
    std::erase(t.history, w);
    t.history.insert(t.history.begin(), w);
    if (t.history.size() > 64) t.history.resize(64);
}
}
bool Box::contains(double px, double py) const { return px >= x && px < x + width && py >= y && py < y + height; }
Model::Model(Box output, Workspace initial, double minimum) {
    m_state = {output, minimum, {{1, initial, output.width, {}}}, 1, 2, 1};
    if (!valid(m_state)) throw std::invalid_argument("invalid tile geometry or workspace");
}
bool Model::valid(const State& s) {
    if (!finiteBox(s.output) || !std::isfinite(s.minimum) || s.minimum <= 0 || s.tiles.empty() || s.tiles.size() > maxTiles) return false;
    std::unordered_set<Id> ids;
    std::unordered_set<Workspace> workspaces;
    double sum = 0;
    bool focus = false;
    for (const auto& t : s.tiles) {
        if (!t.id || t.id >= s.nextId || !t.workspace || !std::isfinite(t.width) || t.width <= 0 ||
            (s.tiles.size() > 1 && t.width + 1e-7 < s.minimum) || !ids.insert(t.id).second || !workspaces.insert(t.workspace).second) return false;
        sum += t.width;
        focus |= t.id == s.focused;
    }
    return focus && std::abs(sum - s.output.width) <= std::max(1e-7, s.output.width * 1e-10);
}
Result Model::publish(State s) {
    if (!valid(s)) return std::unexpected("invalid tile transaction");
    if (s == m_state) return {};
    if (m_state.generation == UINT64_MAX) return std::unexpected("tile generation exhausted");
    s.generation = m_state.generation + 1;
    m_state = std::move(s);
    return {};
}
std::optional<size_t> Model::index(Id id) const {
    for (size_t i = 0; i < m_state.tiles.size(); ++i) if (m_state.tiles[i].id == id) return i;
    return {};
}
std::optional<size_t> Model::presented(Workspace w) const {
    for (size_t i = 0; i < m_state.tiles.size(); ++i) if (m_state.tiles[i].workspace == w) return i;
    return {};
}
Result Model::add(Workspace w, std::optional<double> width, bool before) {
    if (m_state.tiles.size() >= maxTiles) return std::unexpected("at most three tiles per monitor");
    if (!w || presented(w)) return std::unexpected("new tile requires an unpresented workspace");
    auto s = m_state;
    const auto i = *index(s.focused);
    const auto amount = width.value_or(s.tiles[i].width / 2);
    if (!std::isfinite(amount) || amount < s.minimum || s.tiles[i].width - amount < s.minimum) return std::unexpected("not enough width for two tiles");
    if (s.nextId == UINT64_MAX) return std::unexpected("tile IDs exhausted");
    s.tiles[i].width -= amount;
    s.focused = s.nextId++;
    s.tiles.insert(s.tiles.begin() + i + (before ? 0 : 1), Tile{s.focused, w, amount, {}});
    return publish(std::move(s));
}
Result Model::resize(double width, int neighbor) {
    if (m_state.tiles.size() == 1) return std::unexpected("a single tile occupies the full monitor");
    auto s = m_state;
    const auto i = *index(s.focused);
    const int j = static_cast<int>(i) + (neighbor ? neighbor : (i + 1 < s.tiles.size() ? 1 : -1));
    if (j < 0 || j >= static_cast<int>(s.tiles.size()) || std::abs(j - static_cast<int>(i)) != 1) return std::unexpected("no adjacent tile");
    const auto total = s.tiles[i].width + s.tiles[j].width;
    if (!std::isfinite(width) || width < s.minimum || total - width < s.minimum) return std::unexpected("tile minimum width exceeded");
    s.tiles[i].width = width;
    s.tiles[j].width = total - width;
    return publish(std::move(s));
}
Result Model::remove() {
    if (m_state.tiles.size() == 1) return std::unexpected("cannot remove the last tile");
    auto s = m_state;
    const auto i = *index(s.focused), j = i ? i - 1 : i + 1;
    s.tiles[j].width += s.tiles[i].width;
    s.focused = s.tiles[j].id;
    s.tiles.erase(s.tiles.begin() + i);
    return publish(std::move(s));
}
Result Model::resizePair(size_t left, double width) {
    if (left + 1 >= m_state.tiles.size() || left >= m_state.tiles.size()) return std::unexpected("invalid divider");
    auto s=m_state;
    const double total=s.tiles[left].width+s.tiles[left+1].width;
    if (!std::isfinite(width) || width<s.minimum || total-width<s.minimum) return std::unexpected("tile minimum width exceeded");
    s.tiles[left].width=width;
    s.tiles[left+1].width=total-width;
    return publish(std::move(s));
}
Result Model::focus(int step) {
    if (step != -1 && step != 1) return std::unexpected("direction must be left or right");
    const auto i = static_cast<int>(*index(m_state.focused)) + step;
    if (i < 0 || i >= static_cast<int>(m_state.tiles.size())) return {};
    auto s = m_state;
    s.focused = s.tiles[i].id;
    return publish(std::move(s));
}
Result Model::activate(Workspace w, Activation policy) {
    if (!w) return std::unexpected("invalid workspace");
    auto s = m_state;
    const auto i = *index(s.focused);
    if (s.tiles[i].workspace == w) return {};
    if (const auto other = presented(w)) {
        if (policy == Activation::Focus) s.focused = s.tiles[*other].id;
        else {
            remember(s.tiles[i], s.tiles[i].workspace);
            remember(s.tiles[*other], w);
            std::swap(s.tiles[i].workspace, s.tiles[*other].workspace);
        }
    } else {
        remember(s.tiles[i], s.tiles[i].workspace);
        s.tiles[i].workspace = w;
    }
    return publish(std::move(s));
}
Result Model::geometry(Box output, double minimum) {
    if (!finiteBox(output) || !std::isfinite(minimum) || minimum <= 0) return std::unexpected("invalid output geometry");
    if (output == m_state.output && minimum == m_state.minimum) return {};
    auto s = m_state;
    // Keep focused content if the output shrinks below the minimum partition size.
    while (s.tiles.size() > 1 && output.width < s.tiles.size() * minimum) {
        const auto it = std::ranges::find_if(s.tiles, [&](const Tile& t) { return t.id != s.focused; });
        s.tiles.erase(it);
    }
    const auto oldWidth = [&] { double n = 0; for (auto& t : s.tiles) n += t.width; return n; }();
    double available = output.width - s.tiles.size() * minimum;
    if (s.tiles.size() == 1) s.tiles[0].width = output.width;
    else {
        // Allocate the minimum first; distribute the remainder by previous weights.
        double remaining = output.width;
        for (size_t i = 0; i < s.tiles.size(); ++i) {
            auto& t = s.tiles[i];
            t.width = i + 1 == s.tiles.size() ? remaining : minimum + available * t.width / oldWidth;
            remaining -= t.width;
        }
    }
    s.output = output;
    s.minimum = minimum;
    return publish(std::move(s));
}
Result Model::restore(const State& snapshot, uint64_t expectedGeneration) {
    if (expectedGeneration != m_state.generation || snapshot.nextId != m_state.nextId) return std::unexpected("stale tile transaction");
    auto s = snapshot;
    s.generation = m_state.generation;
    return publish(std::move(s));
}
Box Model::box(size_t i) const {
    if (i >= m_state.tiles.size()) return {};
    auto b = m_state.output;
    for (size_t j = 0; j < i; ++j) b.x += m_state.tiles[j].width;
    b.width = m_state.tiles[i].width;
    return b;
}
std::optional<size_t> Model::at(double x, double y) const {
    for (size_t i = 0; i < m_state.tiles.size(); ++i) if (box(i).contains(x, y)) return i;
    return {};
}
std::vector<int> Model::pixelEdges(double scale) const {
    if (!std::isfinite(scale) || scale <= 0 || m_state.output.width * scale > std::numeric_limits<int>::max()) return {};
    std::vector<int> edges{0};
    double x = 0;
    for (const auto& t : m_state.tiles) { x += t.width; edges.push_back(static_cast<int>(std::round(x * scale))); }
    edges.back() = static_cast<int>(std::round(m_state.output.width * scale));
    return edges;
}
std::optional<Workspace> Model::previous() const {
    for (const auto w : m_state.tiles[*index(m_state.focused)].history) if (w != m_state.tiles[*index(m_state.focused)].workspace) return w;
    return {};
}
std::expected<double, std::string> length(const std::string& s, double total, bool delta) {
    if (s.empty() || !std::isfinite(total) || total <= 0) return std::unexpected("invalid length");
    auto end = s.data() + s.size();
    bool percent = s.ends_with('%');
    if (percent) --end;
    else if (s.ends_with("px")) end -= 2;
    double n = 0;
    auto begin = s.data();
    if (begin != end && *begin == '+') ++begin;
    const auto [p, ec] = std::from_chars(begin, end, n);
    if (ec != std::errc{} || p != end || !std::isfinite(n) || (!delta && n <= 0)) return std::unexpected("invalid length");
    const auto result = percent ? n * total / 100 : n;
    if (!std::isfinite(result)) return std::unexpected("length overflow");
    return result;
}
std::expected<DividerDrag, std::string> DividerDrag::begin(const Model& model, size_t left, double pointerX) {
    if (left>=model.state().tiles.size() || left+1>=model.state().tiles.size() || !std::isfinite(pointerX)) return std::unexpected("invalid divider start");
    DividerDrag d;
    d.m_initial=model.state(); d.m_generation=d.m_initial.generation; d.m_left=left; d.m_x=pointerX;
    return d;
}
Result DividerDrag::update(Model& model, double pointerX) {
    if (model.state().generation!=m_generation || !std::isfinite(pointerX)) return std::unexpected("stale divider interaction");
    const auto& a=m_initial.tiles[m_left]; const auto& b=m_initial.tiles[m_left+1];
    const auto width=std::clamp(a.width+pointerX-m_x,m_initial.minimum,a.width+b.width-m_initial.minimum);
    auto r=model.resizePair(m_left,width);
    if (r) m_generation=model.state().generation;
    return r;
}
Result DividerDrag::cancel(Model& model) {
    auto r=model.restore(m_initial,m_generation);
    if (r) m_generation=model.state().generation;
    return r;
}
std::vector<Box> thumbnailGrid(Box area, size_t count, bool horizontal, double aspect) {
    std::vector<Box> out;
    if (!finiteBox(area) || !count || !std::isfinite(aspect) || aspect<=0) return out;
    const size_t cross=std::min(size_t{2},count);
    const size_t rows=horizontal?cross:(count+cross-1)/cross;
    const size_t columns=horizontal?(count+cross-1)/cross:cross;
    const double cw=area.width/columns, ch=area.height/rows;
    const double gap=std::min({8.0,cw/8,ch/8});
    const double width=std::max(1e-6,std::min(cw-gap,(ch-gap)*aspect));
    const double height=width/aspect;
    for (size_t i=0;i<count;++i) {
        const size_t row=horizontal?i/columns:i%rows, column=horizontal?i%columns:i/rows;
        out.push_back({area.x+column*cw+(cw-width)/2,area.y+row*ch+(ch-height)/2,width,height});
    }
    return out;
}
Drop dropTarget(const Model& model, Id source, double x, double y, bool sidebar) {
    const auto i=model.at(x,y);
    if (!i) return {};
    const auto& s=model.state();
    const auto& t=s.tiles[*i]; const auto b=model.box(*i);
    const bool same=source==t.id;
    if (sidebar) return {DropKind::Move,t.id,b,0};
    const bool split=source && s.tiles.size()<maxTiles && (s.tiles.size()==1 || same || x<b.x+b.width*0.2 || x>b.x+b.width*0.8);
    if (!split) return {DropKind::Move,t.id,b,0};
    if (b.width<s.minimum*2) return {DropKind::Invalid,t.id,b,0};
    const bool left=x<b.x+b.width/2;
    auto half=b; half.width/=2; if (!left) half.x+=half.width;
    return {left?DropKind::SplitLeft:DropKind::SplitRight,t.id,half,b.x+b.width/2};
}
} // namespace hymission::tiles
