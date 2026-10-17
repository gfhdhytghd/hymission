#include "hook_broker.hpp"
#include <cassert>
#include <iostream>
namespace h = hymission::hooks;
namespace {
struct Native { void* original; void* bridge; bool attached = false; };
int allocations = 0;
bool fail = false;
int original(int n) { return n * 2; }
h::Lease *a, *b;
int first(int n) { return reinterpret_cast<int (*)(int)>(a->m_original)(n) + 1; }
int second(int n) { return reinterpret_cast<int (*)(int)>(b->m_original)(n + 1) + 10; }
}
namespace hymission::hooks {
void* nativeCreate(void*, void* source, void* destination) { ++allocations; return new Native{source, destination}; }
bool nativeAttach(void* p) { return static_cast<Native*>(p)->attached = !fail; }
void nativeDetach(void* p) { static_cast<Native*>(p)->attached = false; }
void* nativeOriginal(void* p) { return static_cast<Native*>(p)->original; }
void nativeRemove(void*, void* p) { delete static_cast<Native*>(p); --allocations; }
}
int main() {
    a = h::create(nullptr, reinterpret_cast<void*>(&original), h::target(&first));
    b = h::create(nullptr, reinterpret_cast<void*>(&original), h::target(&second));
    assert(a && b && a->group == b->group);
    assert(a->hook() && b->hook() && allocations == 1);
    const auto call = reinterpret_cast<int (*)(int)>(a->group->bridge());
    assert(call(3) == 19);
    b->unhook(); assert(call(3) == 7);
    b->hook(); a->unhook(); assert(call(3) == 18);
    a->hook(); assert(call(3) == 19);
    h::remove(nullptr, b); assert(call(3) == 7);
    a->unhook(); fail = true;
    assert(!a->hook() && !a->enabled);
    h::remove(nullptr, a);
    assert(allocations == 0 && h::groups().empty());
    std::cout << "PASS: one physical patch, ordered continuations, handoff and failed-install rollback\n";
}
