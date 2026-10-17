#include "tiles_logic.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>

using namespace hymission::tiles;
void check(bool b) { if (!b) { std::cerr << "FAIL: tiles invariant\n"; std::abort(); } }
int main() {
    Model m({-1920, 20, 1920, 1080}, 1);
    check(m.add(2).has_value());
    check(m.add(3).has_value());
    auto saved = m.state();
    check(!m.add(4) && m.state() == saved);
    check(m.activate(1).has_value() && m.state().focused == 1);
    check(m.activate(3, Activation::Swap).has_value());
    check(m.state().tiles[0].workspace == 3 && m.state().tiles[2].workspace == 1);
    check(m.previous() == 1);
    check(m.activate(4).has_value() && m.previous() == 3);
    check(m.activate(5).has_value() && m.previous() == 4);
    check(m.activate(4).has_value() && m.previous() == 5);
    const auto edges = m.pixelEdges(1.25);
    check(edges.size() == 4 && edges.front() == 0 && edges.back() == 2400);
    for (size_t i = 0; i < 3; ++i) {
        auto b = m.box(i);
        check(m.at(b.x + b.width / 2, b.y + 1) == i);
        if (i < 2) check(m.at(b.x + b.width, b.y + 1) == i + 1);
    }
    saved = m.state();
    check(!m.resize(std::numeric_limits<double>::quiet_NaN()) && m.state() == saved);
    check(!m.restore(saved, saved.generation - 1) && m.state() == saved);
    check(m.geometry({0, 0, 400, 300}, 240).has_value());
    check(m.state().tiles.size() == 1 && m.state().tiles[0].workspace == 4);
    check(!m.remove());
    check(*length("50%", 1000) == 500 && *length("+80px", 1000, true) == 80);
    check(!length("nan", 1000) && !length("40garbage", 1000) && !length("1e999", 1000) && !length("-4", 1000));
    std::mt19937_64 rng(0x74696c6573);
    Model random({0, 0, 3840, 2160}, 1, 100);
    for (int round = 0; round < 40000; ++round) {
        const auto before = random.state();
        Result result;
        switch (rng() % 8) {
            case 0: result = random.add(1 + rng() % 15); break;
            case 1: result = random.remove(); break;
            case 2: result = random.focus(rng() & 1 ? -1 : 1); break;
            case 3: result = random.activate(1 + rng() % 15); break;
            case 4: result = random.activate(1 + rng() % 15, Activation::Swap); break;
            case 5: result = random.resize(static_cast<double>(rng() % 4000)); break;
            case 6: result = random.geometry({-20, 30, 300.0 + rng() % 4000, 900}, 100); break;
            case 7: result = random.resize(-1); break;
        }
        check(Model::valid(random.state()));
        check(random.state().tiles.size() <= 3);
        if (!result) check(random.state() == before);
        check(random.state().generation >= before.generation);
        const auto e = random.pixelEdges(1.5);
        check(e.front() == 0 && e.back() == std::round(random.state().output.width * 1.5));
        double end = random.state().output.x;
        for (size_t i = 0; i < random.state().tiles.size(); ++i) {
            const auto b = random.box(i);
            check(std::abs(b.x - end) < 1e-6);
            end = b.x + b.width;
        }
        check(std::abs(end - (random.state().output.x + random.state().output.width)) < 1e-6);
    }
    std::cout << "PASS: three-tile transactions, geometry, focus/swap, history and 40000 randomized operations\n";
}
