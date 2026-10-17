#include "hook_broker.hpp"
#include <cassert>
#include <iostream>
namespace h = hymission::hooks;
namespace {
struct Native { void* source; void* bridge; bool attached = false; void* original=nullptr; };
int allocations = 0;
bool fail = false;
int original(int n) { return n * 2; }
h::Lease *a, *b;
h::Lease *ownedA, *ownedB;
int consume(std::shared_ptr<int> value, std::unique_ptr<int>&& moved) { return *value + *moved; }
int wrapOwnedA(std::shared_ptr<int> value, std::unique_ptr<int>&& moved) {
    return reinterpret_cast<decltype(&consume)>(ownedA->m_original)(value,std::move(moved))+1;
}
int wrapOwnedB(std::shared_ptr<int> value, std::unique_ptr<int>&& moved) {
    return reinterpret_cast<decltype(&consume)>(ownedB->m_original)(value,std::move(moved))+2;
}
int first(int n) { return reinterpret_cast<int (*)(int)>(a->m_original)(n) + 1; }
int second(int n) { return reinterpret_cast<int (*)(int)>(b->m_original)(n + 1) + 10; }
}
namespace hymission::hooks {
void* nativeCreate(void*, void* source, void* destination) { ++allocations; return new Native{source, destination}; }
bool nativeAttach(void* p) { auto* n=static_cast<Native*>(p); n->original=fail?nullptr:n->source; return n->attached = !fail; }
void nativeDetach(void* p) { auto* n=static_cast<Native*>(p); n->attached=false; n->original=nullptr; }
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
    assert(reinterpret_cast<int (*)(int)>(a->m_original)(3)==6);
    assert(reinterpret_cast<int (*)(int)>(b->m_original)(3)==7);
    assert(first(3)==7); // direct helper must not run its own transform twice
    b->unhook(); assert(call(3) == 7);
    b->hook(); a->unhook(); assert(call(3) == 18);
    a->hook(); assert(call(3) == 19);
    h::remove(nullptr, b); assert(call(3) == 7);
    const auto cached=reinterpret_cast<int (*)(int)>(a->m_original);
    a->unhook();
    assert(!a->m_original && call(3)==6 && cached(3)==6);
    assert(a->hook() && a->m_original && call(3)==7);
    a->unhook(); fail = true;
    assert(!a->hook() && !a->enabled);
    h::remove(nullptr, a);
    assert(cached(3)==0 && call(3)==0); // removed lease/group cannot be dereferenced
    assert(allocations == 0 && h::groups().empty());
    fail=false;
    ownedA=h::create(nullptr,reinterpret_cast<void*>(&consume),h::target(&wrapOwnedA));
    ownedB=h::create(nullptr,reinterpret_cast<void*>(&consume),h::target(&wrapOwnedB));
    assert(ownedA->hook() && ownedB->hook());
    auto owner=std::make_shared<int>(3);
    auto moved=std::make_unique<int>(7);
    const auto ownedCall=reinterpret_cast<decltype(&consume)>(ownedA->group->bridge());
    for (int i=0;i<1000;++i) { assert(ownedCall(owner,std::move(moved))==13); assert(owner.use_count()==1 && moved); }
    ownedA->priority=10;
    ownedA->unhook(); assert(ownedA->hook());
    assert(ownedCall(owner,std::move(moved))==13);
    assert(owner.use_count()==1 && moved);
    h::remove(nullptr,ownedA); h::remove(nullptr,ownedB);
    assert(allocations==0 && h::groups().empty());
    std::cout << "PASS: one physical patch, ordered continuations, handoff and failed-install rollback\n";
}
