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
    Model divider({0,0,1800,1000},10,100);
    check(divider.add(20).has_value());
    auto initial=divider.state();
    auto drag=DividerDrag::begin(divider,0,900);
    check(drag.has_value() && drag->update(divider,1000).has_value());
    check(divider.state().tiles[0].width==1000 && divider.state().tiles[1].width==800);
    check(divider.state().focused==initial.focused && divider.state().tiles[0].workspace==10);
    check(drag->update(divider,100000).has_value() && divider.state().tiles[1].width==100);
    check(drag->cancel(divider).has_value() && divider.state().tiles==initial.tiles);
    drag=DividerDrag::begin(divider,0,900);
    check(divider.activate(30).has_value());
    const auto changed=divider.state();
    check(!drag->cancel(divider) && !drag->update(divider,1100) && divider.state()==changed);
    Model split({0,0,1920,1080},1,100);
    check(dropTarget(split,1,100,500).kind==DropKind::SplitLeft);
    check(dropTarget(split,1,1800,500).kind==DropKind::SplitRight);
    check(split.add(2,{},true).has_value() && split.state().tiles[0].workspace==2);
    check(split.add(3).has_value() && split.state().tiles.size()==3);
    check(dropTarget(split,split.state().focused,100,500).kind==DropKind::Move);
    for (bool horizontal:{false,true}) for (size_t count:{1U,2U,7U,30U}) {
        const Box band{20,30,300,160};
        const auto grid=thumbnailGrid(band,count,horizontal,0.7);
        check(grid.size()==count);
        for (size_t i=0;i<grid.size();++i) {
            const auto& a=grid[i];
            check(a.x>=band.x && a.y>=band.y && a.x+a.width<=band.x+band.width+1e-6 && a.y+a.height<=band.y+band.height+1e-6);
            for (size_t j=0;j<i;++j) { const auto& b=grid[j]; check(a.x+a.width<=b.x || b.x+b.width<=a.x || a.y+a.height<=b.y || b.y+b.height<=a.y); }
        }
    }
    Model navigation({0,0,1920,1080},1,100);
    Ownership homes;
    homes.sync(navigation.state());
    check(navigation.add(3).has_value()); homes.sync(navigation.state());
    const auto right=navigation.state().focused;
    homes.claim(7,1); homes.claim(8,right);
    check(homes.owner(navigation.state(),7)==1 && homes.owner(navigation.state(),8)==right);
    check(navigation.activate(8).has_value()); homes.sync(navigation.state());
    check(homes.previous(navigation.state(),right,{1,3,7,8})==3);
    homes.claim(3,1); // FB6 hidden workspace transfer invalidates stale history.
    check(!homes.previous(navigation.state(),right,{1,3,7,8}));
    homes.claim(3,right);
    check(!homes.previous(navigation.state(),right,{1,7,8})); // destroyed history is not recreated
    check(navigation.add(5).has_value()); homes.sync(navigation.state());
    const auto third=navigation.state().focused; homes.claim(9,third);
    check(navigation.remove().has_value()); homes.sync(navigation.state());
    check(homes.owner(navigation.state(),9)==right); // third merges left, not first
    check(navigation.focus(-1).has_value());
    check(navigation.remove().has_value()); homes.sync(navigation.state());
    check(homes.owner(navigation.state(),7)==right); // left edge merges right
    const std::vector<Workspace> owned{-1001,-1000,1,7,12};
    for (const std::string prefix:{"m","e","r",""}) {
        check(relativeTarget(prefix+"+1",1,owned)==7);
        check(relativeTarget(prefix+"-1",7,owned)==1);
        check(relativeTarget(prefix+"+2",1,owned)==12);
    }
    check(relativeTarget("m+1",12,owned)==-1001);
    check(relativeTarget("e-1",-1001,owned)==12);
    check(relativeTarget("m-7",1,owned)==-1001);
    check(relativeTarget("m~99",1,owned)==12 && relativeTarget("e~1",12,owned)==-1001);
    check(relativeTarget("r-1",-1001,owned)==-1001);
    check(relativeTarget("r+1",12,owned,{13,14,17})==15);
    check(relativeTarget("+3",7,owned,{13,14,17})==16);
    check(relativeTarget("r~7",1,owned,{13,14,17})==16);
    check(relativeTarget("next",1,owned)==7);
    check(!relativeTarget("r+4294967296",1,owned));
    check(!relativeTarget("m+garbage",1,owned));
    check(!relativeTarget("r+1",UINT32_MAX,{UINT32_MAX}));
    check(relativeTarget("m+0",7,owned)==7);
    check(!relativeSelector("name:foo") && !relativeSelector("3"));
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
