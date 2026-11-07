#ifdef NDEBUG
#undef NDEBUG
#endif
#include "hook_aarch64.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#if defined(__aarch64__)
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace h = hymission::hooks::aarch64;

// One synthetic int(int) function per relocation case, in asm so the displaced
// instructions (the first 16 or 20 bytes) are exactly what each case needs.
#define FN(name) ".globl " #name "\n.type " #name ", %function\n.balign 4\n" #name ":\n"
asm(R"(
.text
)" FN(hookPlain) R"(
    add w0, w0, #1
    add w0, w0, #2
    add w0, w0, #3
    add w0, w0, #4
    ret
)" FN(hookAdr) R"(
    adr x1, hookData
    ldr x1, [x1]
    add w0, w0, w1
    nop
    ret
)" FN(hookAdrp) R"(
    adrp x1, hookData
    add x1, x1, :lo12:hookData
    ldr x1, [x1]
    add w0, w0, w1
    ret
)" FN(hookLdrX) R"(
    ldr x1, hookData
    add w0, w0, w1
    nop
    nop
    ret
)" FN(hookLdrW) R"(
    nop
    ldr w1, hookData
    add w0, w0, w1
    nop
    ret
)" FN(hookLdrsw) R"(
    nop
    nop
    ldrsw x1, hookData
    add w0, w0, w1
    ret
)" FN(hookLdrD) R"(
    ldr d0, hookData
    fmov x1, d0
    add w0, w0, w1
    nop
    ret
)" FN(hookLdrQ) R"(
    nop
    ldr q0, hookData
    umov x1, v0.d[1]
    add w0, w0, w1
    ret
)" FN(hookPrfm) R"(
    prfm pldl1keep, hookData
    add w0, w0, #5
    nop
    nop
    ret
)" FN(hookBcond) R"(
    cmp w0, #0
    b.lt 1f
    add w0, w0, #1
    ret
1:  sub w0, w0, #1
    ret
)" FN(hookCbz) R"(
    nop
    cbz w0, 1f
    add w0, w0, #1
    ret
1:  mov w0, #100
    ret
)" FN(hookTbz) R"(
    nop
    nop
    tbz w0, #0, 1f
    add w0, w0, #10
    ret
1:  add w0, w0, #20
    ret
)" FN(hookB) R"(
    add w0, w0, #1
    add w0, w0, #2
    add w0, w0, #3
    b 1f
    brk #0
1:  add w0, w0, #4
    ret
)" FN(hookBl) R"(
    stp x29, x30, [sp, #-16]!
    bl hookCallee
    add w0, w0, #1
    ldp x29, x30, [sp], #16
    ret
)" FN(hookCallee) R"(
    add w0, w0, #40
    ret
)" FN(hookBti) R"(
    bti c
    add w0, w0, #1
    add w0, w0, #2
    add w0, w0, #3
    add w0, w0, #4
    ret
)" FN(hookPaciasp) R"(
    paciasp
    add w0, w0, #1
    add w0, w0, #2
    add w0, w0, #3
    add w0, w0, #4
    autiasp
    ret
)" FN(hookTooShort) R"(
    add w0, w0, #7
    ret
)" FN(hookX17) R"(
    mov x17, #3
    add w0, w0, w17
    nop
    nop
    ret
)" FN(hookBranchNotLast) R"(
    add w0, w0, #1
    b 1f
    nop
    nop
1:  ret
.balign 16
hookData:
    .quad 42
    .quad 7
)");

using Fn = int (*)(int);
extern "C" {
int hookPlain(int); int hookAdr(int); int hookAdrp(int); int hookLdrX(int); int hookLdrW(int); int hookLdrsw(int);
int hookLdrD(int); int hookLdrQ(int); int hookPrfm(int); int hookBcond(int); int hookCbz(int); int hookTbz(int);
int hookB(int); int hookBl(int); int hookBti(int); int hookPaciasp(int); int hookTooShort(int); int hookBranchNotLast(int);
int hookX17(int);
}

Fn g_original = nullptr;
int g_runs = 0;
int replacement(int x) {
    ++g_runs;
    return g_original(x) + 1000;
}

void check(Fn fn, const std::vector<int>& inputs) {
    std::vector<int> expected;
    for (int in : inputs) expected.push_back(fn(in));
    auto* hook = h::create(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(replacement));
    assert(!h::original(hook));
    // Attach twice over the hook's life: a re-attach reuses the same trampoline.
    for (int round = 0; round < 2; ++round) {
        assert(h::attach(hook));
        g_original = reinterpret_cast<Fn>(h::original(hook));
        assert(g_original);
        for (size_t i = 0; i < inputs.size(); ++i) {
            g_runs = 0;
            assert(fn(inputs[i]) == expected[i] + 1000);
            assert(g_runs == 1);
            assert(g_original(inputs[i]) == expected[i]);
        }
        h::detach(hook);
        assert(!h::original(hook));
        for (size_t i = 0; i < inputs.size(); ++i) {
            g_runs = 0;
            assert(fn(inputs[i]) == expected[i]);
            assert(g_runs == 0);
        }
    }
    h::destroy(hook);
}

void refuses(Fn fn, int in, int out) {
    auto* hook = h::create(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(replacement));
    assert(!h::attach(hook));
    assert(fn(in) == out);
    h::destroy(hook);
}

void landingPads() {
    const auto entry = [](Fn fn) { return *reinterpret_cast<const uint32_t*>(fn); };
    constexpr uint32_t btiC = 0xD503245F, paciasp = 0xD503233F;
    auto* bti = h::create(reinterpret_cast<void*>(hookBti), reinterpret_cast<void*>(replacement));
    assert(h::attach(bti));
    assert(entry(hookBti) == btiC);
    h::destroy(bti);
    // PACIASP moves to the trampoline, so a hook's return is not authenticated against a signed LR.
    auto* pac = h::create(reinterpret_cast<void*>(hookPaciasp), reinterpret_cast<void*>(replacement));
    assert(h::attach(pac));
    assert(entry(hookPaciasp) == btiC);
    assert(*static_cast<const uint32_t*>(h::original(pac)) == paciasp);
    h::destroy(pac);
    assert(entry(hookPaciasp) == paciasp);
}

void doubleHook() {
    auto* first = h::create(reinterpret_cast<void*>(hookPlain), reinterpret_cast<void*>(replacement));
    auto* second = h::create(reinterpret_cast<void*>(hookPlain), reinterpret_cast<void*>(replacement));
    assert(h::attach(first));
    assert(!h::attach(second));
    h::destroy(second);
    assert(h::original(first));
    h::destroy(first);
    assert(hookPlain(0) == 10);
}

// A trampoline next to its source takes direct branches, a far one goes through X17.
void branchRange() {
    const auto* source = reinterpret_cast<const uint32_t*>(hookPlain);
    const auto near = h::relocate(source, 4, reinterpret_cast<uint64_t>(source) + 4096);
    uint32_t word;
    std::memcpy(&word, near.data() + 16, 4);
    assert((word & 0xFC000000) == 0x14000000);
    const auto far = h::relocate(source, 4, reinterpret_cast<uint64_t>(source) + (1ULL << 40));
    std::memcpy(&word, far.data() + 16, 4);
    assert((word & 0xFF00001F) == 0x58000011);
}

// Runs a function from a page far from any trampoline, covering the X17 path end to end.
void farSource() {
    const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
    void* page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    std::memcpy(page, reinterpret_cast<const void*>(hookPlain), 20);
    __builtin___clear_cache(static_cast<char*>(page), static_cast<char*>(page) + 20);
    assert(mprotect(page, pageSize, PROT_READ | PROT_EXEC) == 0);
    check(reinterpret_cast<Fn>(page), {0, 5});
    munmap(page, pageSize);
}

int main() {
    check(hookPlain, {0, 5, -20});
    check(hookAdr, {0, 5});
    check(hookAdrp, {0, 5});
    check(hookLdrX, {0, 5});
    check(hookLdrW, {0, 5});
    check(hookLdrsw, {0, 5});
    check(hookLdrD, {0, 5});
    check(hookLdrQ, {0, 5});
    check(hookPrfm, {0, 5});
    check(hookBcond, {5, -5});
    check(hookCbz, {0, 5});
    check(hookTbz, {2, 3});
    check(hookB, {0, 5});
    check(hookBl, {0, 5});
    check(hookBti, {0, 5});
    check(hookPaciasp, {0, 5});
    refuses(hookTooShort, 1, 8);
    refuses(hookBranchNotLast, 1, 2);
    refuses(hookX17, 1, 4);
    landingPads();
    doubleHook();
    branchRange();
    farSource();
    std::puts("ok");
}
#else
int main() { std::puts("skipped: not AArch64"); }
#endif
