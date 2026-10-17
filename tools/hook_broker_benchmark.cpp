#include "hook_broker.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cassert>
namespace h=hymission::hooks;
namespace {
struct Native { void* source; void* bridge; };
struct Box { double x,y,w,h; };
struct Context {
    inline static int copies=0;
    uint64_t draws=0;
    Context()=default;
    Context(const Context& other):draws(other.draws) { ++copies; }
};
struct Element { double x=1; };
h::Lease *boxLease,*drawLease,*valueLease;
struct Value {
    inline static int copies=0;
    int n=7;
    Value()=default;
    Value(const Value& v):n(v.n) { ++copies; }
    Value(Value&& v) noexcept:n(v.n) {}
};
[[gnu::noinline]] Box texBox(Element* e) { return {e->x,2,3,4}; }
[[gnu::noinline]] void draw(Element*,Context& context) { ++context.draws; }
[[gnu::noinline]] int consume(Value value) { return value.n; }
Box boxHook(Element* e) { return reinterpret_cast<decltype(&texBox)>(boxLease->m_original)(e); }
void drawHook(Element* e,Context& c) { reinterpret_cast<decltype(&draw)>(drawLease->m_original)(e,c); }
int valueHook(Value v) { return reinterpret_cast<decltype(&consume)>(valueLease->m_original)(std::move(v)); }
template<class F> double measure(F fn) {
    constexpr int iterations=2000000;
    const auto start=std::chrono::steady_clock::now();
    for (int i=0;i<iterations;++i) { fn(); asm volatile("" ::: "memory"); }
    return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
}
}
namespace hymission::hooks {
void* nativeCreate(void*,void* source,void* target) { return new Native{source,target}; }
bool nativeAttach(void*) { return true; }
void nativeDetach(void*) {}
void* nativeOriginal(void* p) { return static_cast<Native*>(p)->source; }
void nativeRemove(void*,void* p) { delete static_cast<Native*>(p); }
}
int main() {
    boxLease=h::create(nullptr,reinterpret_cast<void*>(&texBox),h::target(&boxHook));
    drawLease=h::create(nullptr,reinterpret_cast<void*>(&draw),h::target(&drawHook));
    valueLease=h::create(nullptr,reinterpret_cast<void*>(&consume),h::target(&valueHook));
    assert(boxLease->hook() && drawLease->hook() && valueLease->hook());
    const auto boxCall=reinterpret_cast<decltype(&texBox)>(boxLease->group->bridge());
    const auto drawCall=reinterpret_cast<decltype(&draw)>(drawLease->group->bridge());
    const auto valueCall=reinterpret_cast<decltype(&consume)>(valueLease->group->bridge());
    Element e; Context c; double sum=0;
    Value value;
    Value::copies=0; assert(valueCall(value)==7);
    std::printf("by_value_copies=%d (includes one caller copy)\n",Value::copies);
    assert(Value::copies==3); // caller, hook and native each receive an owned ABI value
    for (int sample=0;sample<5;++sample) {
        const auto directBox=measure([&] { sum+=texBox(&e).x; });
        const auto brokerBox=measure([&] { sum+=boxCall(&e).x; });
        const auto directDraw=measure([&] { draw(&e,c); });
        const auto brokerDraw=measure([&] { drawCall(&e,c); });
        std::printf("sample=%d getTexBox_direct_ns=%.2f getTexBox_broker_ns=%.2f draw_direct_ns=%.2f draw_broker_ns=%.2f\n",sample,directBox,brokerBox,directDraw,brokerDraw);
    }
    assert(sum>0 && c.draws>0);
    assert(Context::copies==0);
    std::printf("getTexBox/draw argument object copies=0\n");
    h::remove(nullptr,boxLease); h::remove(nullptr,drawLease); h::remove(nullptr,valueLease);
}
