#ifdef NDEBUG
#undef NDEBUG
#endif
#include "hook_broker.hpp"
#include "hook_attempt.hpp"
#include "hook_symbols.hpp"
#include "hook_entry.hpp"
#include <cassert>
#include <iostream>
namespace h = hymission::hooks;
namespace {
struct Native { void* source; void* bridge; bool attached = false; void* original=nullptr; };
int allocations = 0;
bool fail = false;
int original(int n) { return n * 2; }
h::Lease *a, *b;
struct ActionError { std::string message; explicit ActionError(std::string m):message(std::move(m)) {} };
using ActionResult=std::expected<void,ActionError>;
ActionResult action() { return {}; }
ActionResult actionHook() { return {}; }
struct DispatchResult { bool success=true; std::string error; };
DispatchResult dispatch() { return {}; }
DispatchResult dispatchHook() { return {}; }
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
    // Actual release end() and scrolling update() prologues from both host cores.
    const std::array<unsigned char,6> tail{0xe9,0x6b,0xe3,0xff,0xff,0x90};
    std::vector<unsigned char> copied(tail.begin(),tail.end()); copied.resize(11); copied[6]=0xe9;
    const auto relocated=h::relocateEntryBranches(tail,copied,std::array<size_t,2>{5,1},0xc94650,0x1062320,32);
    assert(relocated);
    int32_t displacement;
    std::memcpy(&displacement,relocated->data()+1,4);
    assert(0x1062320+5+displacement==0xc929c0);
    const std::array<unsigned char,6> conditional{0x80,0x7f,0x10,0,0x75,0x0a};
    copied.assign(conditional.begin(),conditional.end()); copied.resize(11); copied[6]=0xe9;
    const auto branch=h::relocateEntryBranches(conditional,copied,std::array<size_t,2>{4,2},0xca24d0,0x1062400,32);
    assert(branch && branch->size()==16 && (*branch)[5]==5 && (*branch)[11]==0xe9);
    std::memcpy(&displacement,branch->data()+12,4);
    assert(0x1062400+16+displacement==0xca24e0);
    assert(std::equal(branch->begin(),branch->begin()+4,conditional.begin()));
    assert(!h::relocateEntryBranches(conditional,copied,std::array<size_t,2>{4,2},0xca24d0,0x1062400,15));
    const std::array<unsigned char,6> back{0x90,0x90,0x90,0x90,0x75,0xfa};
    const auto internal=h::relocateEntryBranches(back,copied,std::array<size_t,5>{1,1,1,1,2},0x1000,0x2000,32);
    assert(internal);
    std::memcpy(&displacement,internal->data()+12,4);
    assert(0x2000+16+displacement==0x2000);
    const std::array<unsigned char,7> ripMemory{0x48,0x8b,0x05,0x11,0,0,0};
    copied.assign(ripMemory.begin(),ripMemory.end()); copied[3]=0x22; copied.resize(12); copied[7]=0xe9;
    const auto rip=h::relocateEntryBranches(ripMemory,copied,std::array<size_t,1>{7},0x1000,0x2000,32);
    assert(rip && *rip==copied); // retain native RIP relocation and return jump
    assert(!h::relocateEntryBranches(tail,std::vector<unsigned char>(11),std::array<size_t,2>{5,1},0xc94650,0x10000000000,32));
    struct Symbol { std::string signature,demangled; void* address; };
    void* p=reinterpret_cast<void*>(&original);
    std::vector<Symbol> aliases{{"_ZN1A1fESt8optionalI1XI1YEE","unrelated API name",p},{"_ZN1A1fESt8optionalI1XI1YEE","another shifted API name",p}};
    assert(h::normalizedSignature("A::f( std::optional<X<Y>> )")==h::normalizedSignature("A::f(std::optional<X<Y> >)"));
    assert(h::findSymbol(aliases,"A::f(std::optional<X<Y> >)",true)==p);
    assert(h::findSymbol(aliases,"A::f(OldType)")==p); // historical qualified prefix
    assert(!h::findSymbol(aliases,"A::f(OldType)",true));
    aliases.push_back({"_ZN1A1fEi","mispaired",reinterpret_cast<void*>(&first)});
    assert(!h::findSymbol(aliases,"A::f(OldType)")); // distinct overloads remain ambiguous
    std::vector<Symbol> shifted{
        {"_ZN29CUnifiedWorkspaceSwipeGesture6updateEd","unrelated preceding function",p},
        {"_ZN1A1fEi","CUnifiedWorkspaceSwipeGesture::update(double)",reinterpret_cast<void*>(&first)},
        {"_Zinvalid","CUnifiedWorkspaceSwipeGesture::update(double)",reinterpret_cast<void*>(&second)}};
    assert(h::findSymbol(shifted,"CUnifiedWorkspaceSwipeGesture::update(double)",true)==p);
    shifted.erase(shifted.begin());
    assert(!h::findSymbol(shifted,"CUnifiedWorkspaceSwipeGesture::update(double)",true));
    assert(!h::findSymbol(shifted,"CUnifiedWorkspaceSwipeGesture::update(double)"));
    assert(h::canonicalSymbol("_Zinvalid").empty());
    assert(h::canonicalSymbol("plain_c_symbol")=="plain_c_symbol");
    h::AttemptCache optionalHook;
    int probes=0;
    for (int frame=0;frame<10000;++frame) assert(!optionalHook.run([&] { ++probes; return false; }));
    optionalHook.release();
    assert(!optionalHook.run([&] { ++probes; return true; }) && probes==1);
    optionalHook.reset(); // config reload, the capability can be retried
    assert(optionalHook.run([&] { ++probes; return true; }) && probes==2);
    optionalHook.release();
    assert(optionalHook.run([&] { ++probes; return true; }) && probes==3);
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
    auto* actionLease=h::create(nullptr,reinterpret_cast<void*>(&action),h::target(&actionHook));
    assert(actionLease && actionLease->hook());
    const auto actionCall=reinterpret_cast<decltype(&action)>(actionLease->group->bridge());
    const auto actionNext=reinterpret_cast<decltype(&action)>(actionLease->m_original);
    assert(actionCall() && actionNext());
    actionLease->unhook(); assert(actionCall() && actionNext());
    h::remove(nullptr,actionLease);
    assert(!actionCall() && !actionNext());
    assert(actionCall().error().message=="hook continuation unavailable");
    assert(actionNext().error().message=="hook continuation unavailable");
    auto* dispatchLease=h::create(nullptr,reinterpret_cast<void*>(&dispatch),h::target(&dispatchHook));
    assert(dispatchLease && dispatchLease->hook());
    const auto dispatchNext=reinterpret_cast<decltype(&dispatch)>(dispatchLease->m_original);
    h::remove(nullptr,dispatchLease);
    assert(!dispatchNext().success && !dispatchNext().error.empty());
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
