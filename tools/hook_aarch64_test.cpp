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
)" FN(hookIntra) R"(
    cbz w0, 1f
    add w0, w0, #1
1:  add w0, w0, #2
    add w0, w0, #3
    add w0, w0, #4
    ret
)" FN(hookLiteralInPatch) R"(
    ldr w1, 1f
    mov w0, w1
1:  nop
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
int hookX17(int); int hookIntra(int); int hookLiteralInPatch(int);
}

Fn g_original = nullptr;
int g_runs = 0;
int replacement(int x) {
    ++g_runs;
    return g_original(x) + 1000;
}

bool near(const void* a, const void* b) {
    const auto delta = reinterpret_cast<intptr_t>(a) - reinterpret_cast<intptr_t>(b);
    return delta > -(1LL << 27) && delta < (1LL << 27);
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
        assert(near(reinterpret_cast<const void*>(g_original), reinterpret_cast<const void*>(fn))); // direct branches
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
    // A far call must not use BLR, which BTI checks: ADR X30 ; LDR X17 ; RET X17.
    const auto* call = reinterpret_cast<const uint32_t*>(hookBl);
    const auto farCall = h::relocate(call, 4, reinterpret_cast<uint64_t>(call) + (1ULL << 40));
    uint32_t words[3];
    std::memcpy(words, farCall.data() + 4, sizeof words);
    assert(words[0] == 0x1000007E && (words[1] & 0xFF00001F) == 0x58000011 && words[2] == 0xD65F0220);
}

// A source on an anonymous page, away from the binary, gets a trampoline near it as well.
void mappedSource() {
    const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
    void* page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    std::memcpy(page, reinterpret_cast<const void*>(hookPlain), 20);
    __builtin___clear_cache(static_cast<char*>(page), static_cast<char*>(page) + 20);
    assert(mprotect(page, pageSize, PROT_READ | PROT_EXEC) == 0);
    check(reinterpret_cast<Fn>(page), {0, 5});
    munmap(page, pageSize);
}

// Runs relocated code from a page that is certainly more than 128 MiB from its source: the
// resume jump, a call and a conditional branch all go through X17, and the X17 guard applies.
void farTrampoline() {
    const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
    const auto source = reinterpret_cast<uintptr_t>(hookPlain) & ~(pageSize - 1);
    void* page = MAP_FAILED;
    for (uintptr_t distance = 1ULL << 30; page == MAP_FAILED && distance < (1ULL << 40); distance *= 2) {
        page = mmap(reinterpret_cast<void*>(source + distance), pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (page != MAP_FAILED && page != reinterpret_cast<void*>(source + distance)) { munmap(page, pageSize); page = MAP_FAILED; }
    }
    assert(page != MAP_FAILED && !near(page, reinterpret_cast<const void*>(hookPlain)));
    const auto run = [&](Fn fn, int in) {
        const auto code = h::relocate(reinterpret_cast<const uint32_t*>(fn), 4, reinterpret_cast<uint64_t>(page));
        assert(!code.empty());
        assert(mprotect(page, pageSize, PROT_READ | PROT_WRITE) == 0);
        std::memcpy(page, code.data(), code.size());
        __builtin___clear_cache(static_cast<char*>(page), static_cast<char*>(page) + code.size());
        assert(mprotect(page, pageSize, PROT_READ | PROT_EXEC) == 0);
        assert(reinterpret_cast<Fn>(page)(in) == fn(in));
    };
    run(hookPlain, 5);
    run(hookBl, 5);  // ADR X30 ; LDR X17 ; RET X17 returns past the sequence
    run(hookCbz, 0); // taken: the far jump after the skip
    run(hookCbz, 5); // not taken: the B over the far jump
    assert(h::relocate(reinterpret_cast<const uint32_t*>(hookX17), 4, reinterpret_cast<uint64_t>(page)).empty());
    munmap(page, pageSize);
}

// One instruction followed by three NOPs, relocated next to itself and far away.
bool accepted(uint32_t word, uint64_t distance) {
    const uint32_t code[] = {word, 0xD503201Fu, 0xD503201Fu, 0xD503201Fu};
    return !h::relocate(code, 4, reinterpret_cast<uint64_t>(code) + distance).empty();
}

int main() {
    constexpr uint64_t NEAR = 4096, FAR = 1ULL << 40;
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
    check(hookX17, {0, 5}); // X17 is only reserved when a far jump clobbers it
    refuses(hookTooShort, 1, 8);
    refuses(hookBranchNotLast, 1, 2);
    refuses(hookIntra, 1, 11);
    refuses(hookIntra, 0, 9);
    refuses(hookLiteralInPatch, 1, 0xD503201F);
    // Writers of X17 outside bits 4:0 or outside the data processing group: accepted next to the
    // source, refused when the trampoline is far.
    for (uint32_t word : {
             0xD53BD051u, // mrs x17, tpidr_el0
             0xD5280011u, // sysl x17, #0, c0, c0, #0
             0xD5780010u, // mrrs x16, x17, midr_el1
             0xD503211Fu, // pacia1716
             0x528000B1u, // mov w17, #5
             0x9A810011u, // csel x17, x0, x1, eq
             0x9E660011u, // fmov x17, d0
             0x1E18FC11u, // fcvtzs w17, s0, #1
             0x4E183C11u, // mov x17, v0.d[1]
             0x88117C01u, // stxr w17, w1, [x0]
             0x48307C02u, // casp x16, x17, x2, x3, [x0]
             0xC85FFC11u, // ldaxr x17, [x0]
             0xF8408620u, // ldr x0, [x17], #8
             0xF8008E20u, // str x0, [x17, #8]!
             0xA9404401u, // ldp x1, x17, [x0]
             0x4CDF7A20u, // ld1 {v0.4s}, [x17], #16
             0xF8200411u, // ldraa x17, [x0]
             0xD9400011u, // ldapur x17, [x0]
             0xD9411811u, // ldiapp x17, x1, [x0]
             0xF83FD010u, // ld64b x16, [x0]
         })
        assert(accepted(word, NEAR) && !accepted(word, FAR));
    // Immediates and vector registers whose bits read 17, and X17 as a source: never refused.
    for (uint32_t word : {
             0xF9404420u, // ldr x0, [x1, #136]
             0x3DC04400u, // ldr q0, [x0, #272]
             0x91404420u, // add x0, x1, #0x11, lsl #12
             0x52800220u, // mov w0, #17
             0xFA510801u, // ccmp x0, #17, #1, eq
             0x1E610411u, // fccmpe d0, d1, #1, eq
             0xF8510420u, // ldr x0, [x1], #-240
             0xD9510020u, // ldapur x0, [x1, #-240]
             0xA9B97BFDu, // stp x29, x30, [sp, #-112]!
             0x9E670011u, // fmov d17, x0
             0xFD400011u, // ldr d17, [x0]
             0x9E620220u, // scvtf d0, x17
             0x1E03FE20u, // ucvtf s0, w17, #1
             0x4E040E20u, // dup v0.4s, w17
             0x4E181E20u, // mov v0.d[1], x17
             0x6E0C6420u, // mov v0.s[1], v1.s[3]
             0x9B024420u, // madd x0, x1, x2, x17
         })
        assert(accepted(word, NEAR) && accepted(word, FAR));
    // FEAT_CMPBR compare-and-branch (CBGT, CBNE #imm) is not relocated.
    for (uint32_t word : {0x740100C0u, 0xF5E080C0u}) assert(!accepted(word, NEAR));
    landingPads();
    doubleHook();
    branchRange();
    mappedSource();
    farTrampoline();
    std::puts("ok");
}
#else
int main() {
    std::puts("skipped: not AArch64");
    return 77; // reported as skipped by CTest (SKIP_RETURN_CODE) and Meson
}
#endif
