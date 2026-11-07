#include "hook_aarch64.hpp"
#if defined(__aarch64__)
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

/*
    Entry patch (16 bytes, displaces 4 instructions):
        LDR X16, #8
        BR  X16
        .quad <destination>
    X16 and X17 are IP0 and IP1: the ABI lets veneers clobber them between a call and the
    callee's entry, so using them at a function entry is legal.

    An entry with a landing pad (BTI, or PACIASP / PACIBSP which act as BTI c) keeps a BTI first
    (20 bytes, 5 displaced instructions), so indirect calls still land on a valid pad. The original
    pad moves to the trampoline: a PACIASP must run only on the path into the original function,
    otherwise LR would be signed twice and the hook's return would fail authentication.

    Jumps out of the trampoline use B / BL within +-128 MiB and X17 otherwise, always through
    RET X17, which is not subject to BTI: the resume address is in the middle of a function, and a
    local function that is only called directly may have no BTI c. A far call sets X30 with ADR
    first: ADR X30, <next> ; LDR X17, =<target> ; RET X17.

    X17 must not be live across those jumps, so an entry whose displaced instructions name X17 is
    refused. At a function entry X17 holds nothing (see above), so only those instructions could
    make it live.

    A displaced branch whose target is inside the displaced bytes is refused. A later branch back
    into them cannot be detected, the same limit as on x86. The patch is written with a plain copy
    while other threads may run the code, and destroy() unmaps the trampoline without waiting for
    calls in flight; Hyprland's CFunctionHook has the same limits on x86. RET X17 would also trip a
    guarded control stack (GCS), which Hyprland does not enable.
*/
namespace hymission::hooks::aarch64 {
namespace {
constexpr uint32_t NOP = 0xD503201F, BTI_C = 0xD503245F, LDR_X16_8 = 0x58000050, BR_X16 = 0xD61F0200;
constexpr uint32_t LDR_X17_LITERAL = 0x58000011;
constexpr size_t MAX_PATCH_WORDS = 5;
#ifndef PROT_BTI
#define PROT_BTI 0x10
#endif

bool isBti(uint32_t insn) { return (insn & 0xFFFFFF3F) == 0xD503241F; }
bool isLandingPad(uint32_t insn) { return isBti(insn) || insn == 0xD503233F || insn == 0xD503237F; }
int64_t signExtend(uint64_t value, unsigned bits) {
    const uint64_t sign = 1ULL << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

// mprotect() without PROT_BTI would silently drop a BTI guard from the mapping, so an
// address whose mapping cannot be read is reported as unknown (-1) and not patched.
int btiGuard(uintptr_t address) {
    std::ifstream smaps("/proc/self/smaps");
    bool inRange = false;
    for (std::string line; std::getline(smaps, line);) {
        if (line.starts_with("VmFlags:")) {
            if (inRange) return line.contains(" bt") ? 1 : 0;
            continue;
        }
        const auto dash = line.find('-'), space = line.find(' ');
        if (dash == std::string::npos || space == std::string::npos || dash > space) continue;
        try {
            const auto start = std::stoull(line.substr(0, dash), nullptr, 16);
            const auto end = std::stoull(line.substr(dash + 1, space - dash - 1), nullptr, 16);
            inRange = address >= start && address < end;
        } catch (const std::exception&) { inRange = false; }
    }
    return -1;
}

// The page stays executable throughout: another thread may be running code on it.
// True once the code is written, even if the page could not be made read-only again.
bool writeCode(void* target, const void* code, size_t size) {
    const auto pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGE_SIZE));
    const auto address = reinterpret_cast<uintptr_t>(target);
    const auto start = address & ~(pageSize - 1);
    const auto length = ((address + size + pageSize - 1) & ~(pageSize - 1)) - start;
    const int guard = btiGuard(address);
    if (guard < 0) return false;
    const int protection = PROT_READ | PROT_EXEC | (guard ? PROT_BTI : 0);
    if (mprotect(reinterpret_cast<void*>(start), length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    std::memcpy(target, code, size);
    if (mprotect(reinterpret_cast<void*>(start), length, protection) != 0)
        std::fprintf(stderr, "[hymission] hook: could not restore code page protection at %p\n", target);
    __builtin___clear_cache(static_cast<char*>(target), static_cast<char*>(target) + size);
    return true;
}

struct Assembler {
    uint64_t base;
    std::vector<uint32_t> code{};
    std::vector<uint64_t> literals{};
    std::vector<std::pair<size_t, size_t>> literalRefs{};

    uint64_t pc() const { return base + code.size() * 4; }
    void emit(uint32_t insn) { code.push_back(insn); }
    // An LDR (literal) whose imm19 is filled in by finish().
    void emitLoad(uint32_t opcode, uint64_t value) {
        literalRefs.emplace_back(code.size(), literals.size());
        literals.push_back(value);
        emit(opcode);
    }
    static bool inBranchRange(uint64_t from, uint64_t target) {
        const auto delta = static_cast<int64_t>(target - from);
        return delta >= -(1LL << 27) && delta < (1LL << 27);
    }
    void emitJump(uint64_t target, bool link) {
        if (inBranchRange(pc(), target)) {
            emit((link ? 0x94000000 : 0x14000000) | (((target - pc()) >> 2) & 0x03FFFFFF));
            return;
        }
        if (link) emit(0x1000007E); // ADR X30, #12: return past the RET below
        emitLoad(LDR_X17_LITERAL, target);
        emit(0xD65F0220); // RET X17
    }
    // <condition> +8 ; B over ; <jump to target>
    void emitConditionalJump(uint32_t branchToPlus8, uint64_t target) {
        const uint32_t jumpWords = inBranchRange(pc() + 8, target) ? 1 : 2;
        emit(branchToPlus8);
        emit(0x14000000 | (jumpWords + 1));
        emitJump(target, false);
    }
    std::vector<uint8_t> finish() {
        if (code.size() % 2) emit(NOP); // 8-byte align the literal pool
        for (const auto& [codeIndex, literalIndex] : literalRefs)
            code[codeIndex] |= ((code.size() + literalIndex * 2 - codeIndex) & 0x7FFFF) << 5;
        std::vector<uint8_t> bytes(code.size() * 4 + literals.size() * 8);
        std::memcpy(bytes.data(), code.data(), code.size() * 4);
        if (!literals.empty()) std::memcpy(bytes.data() + code.size() * 4, literals.data(), literals.size() * 8);
        return bytes;
    }
};

std::vector<uint32_t> entryPatch(uint32_t first, bool landingPad, void* destination) {
    std::vector<uint32_t> patch;
    if (landingPad) patch.push_back(isBti(first) ? first : BTI_C);
    const auto target = reinterpret_cast<uint64_t>(destination);
    patch.insert(patch.end(), {LDR_X16_8, BR_X16, static_cast<uint32_t>(target), static_cast<uint32_t>(target >> 32)});
    return patch;
}

// Conservative: outside the branch / system group, any register field (bits 4:0, 9:5, 14:10,
// 20:16) that reads 17 counts, which also catches status registers (STXR), register pairs
// (CASP, LDP) and base writeback. An immediate that happens to look like 17 only costs a refused
// hook. In the branch / system group only MRS / SYSL and the *1716 PAC hints write X17.
bool mayWriteX17(uint32_t insn) {
    if ((insn & 0xFFE00000) == 0xD5200000) return (insn & 0x1F) == 17; // MRS, SYSL
    if ((insn & 0xFFFFFF1F) == 0xD503211F) return true;                 // PACIA1716 .. AUTIB1716
    if ((insn & 0x1C000000) == 0x14000000) return false;                // branches, exceptions, other system
    for (unsigned shift : {0u, 5u, 10u, 16u})
        if (((insn >> shift) & 0x1F) == 17) return true;
    // CASP names only the even register of each pair: X16 implies X17.
    if ((insn & 0x3F000000) == 0x08000000 && ((insn & 0x1F) == 16 || ((insn >> 16) & 0x1F) == 16)) return true;
    return false;
}

// Moved literals would be executed as code, so never relocate over another hook's patch.
bool isPatched(const uint32_t* source) {
    return (source[0] == LDR_X16_8 && source[1] == BR_X16) || (isBti(source[0]) && source[1] == LDR_X16_8 && source[2] == BR_X16);
}
} // namespace

/*
    Rewritten instructions, everything else is copied verbatim:
        ADR / ADRP Xd, <label>      -> LDR Xd, =<address>
        LDR Wt / Xt / LDRSW <lit>   -> LDR Xt, =<address> ; LDR Wt / Xt / LDRSW Xt, [Xt]
        LDR St / Dt / Qt <lit>      -> LDR X17, =<address> ; LDR St / Dt / Qt, [X17]
        PRFM <lit>                  -> NOP
        B / BL <label>              -> direct B / BL, or through X17
        B.cond / CBZ / CBNZ / TBZ / TBNZ <label> -> <same> +8 ; B over ; jump to <label>
    Refused: a branch target inside the displaced bytes, which now hold the patch; an unconditional
    branch before the last displaced instruction, which means the function is shorter than the
    patch and the bytes after it belong to something else; a literal load into XZR.
*/
std::vector<uint8_t> relocate(const uint32_t* source, size_t count, uint64_t address) {
    Assembler out{.base = address};
    const auto begin = reinterpret_cast<uint64_t>(source), end = begin + count * 4;
    const auto displaced = [&](uint64_t target) { return target >= begin && target < end; };
    for (size_t i = 0; i < count; ++i) {
        const uint32_t insn = source[i];
        const uint64_t pc = reinterpret_cast<uint64_t>(source) + i * 4;
        const bool last = i + 1 == count;
        if (mayWriteX17(insn)) return {};
        if ((insn & 0x7E000000) == 0x74000000) return {}; // FEAT_CMPBR compare and branch: not relocated
        if ((insn & 0x9F000000) == 0x10000000) { // ADR
            const uint64_t imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2);
            const uint64_t target = pc + signExtend(imm, 21);
            if (displaced(target)) return {}; // an address into bytes that now hold the patch
            out.emitLoad(0x58000000 | (insn & 0x1F), target);
        } else if ((insn & 0x9F000000) == 0x90000000) { // ADRP
            const uint64_t imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2);
            out.emitLoad(0x58000000 | (insn & 0x1F), (pc & ~0xFFFULL) + (signExtend(imm, 21) << 12));
        } else if ((insn & 0x3B000000) == 0x18000000) { // LDR, LDRSW, PRFM (literal)
            const uint64_t target = pc + signExtend((insn >> 5) & 0x7FFFF, 19) * 4;
            const uint32_t opc = insn >> 30, rt = insn & 0x1F;
            if (target < end && target + 16 > begin) return {}; // would read the patch, not the original bytes
            if (insn & (1 << 26)) { // SIMD&FP
                if (opc == 3) return {}; // unallocated
                static constexpr uint32_t loads[] = {0xBD400000, 0xFD400000, 0x3DC00000};
                out.emitLoad(LDR_X17_LITERAL, target);
                out.emit(loads[opc] | (17 << 5) | rt);
            } else if (opc == 3) {
                out.emit(NOP); // PRFM is a hint
            } else if (rt == 31) {
                return {}; // a load into XZR, rewritten it would address SP
            } else {
                static constexpr uint32_t loads[] = {0xB9400000, 0xF9400000, 0xB9800000};
                out.emitLoad(0x58000000 | rt, target);
                out.emit(loads[opc] | (rt << 5) | rt);
            }
        } else if ((insn & 0x7C000000) == 0x14000000) { // B / BL
            const bool link = insn & 0x80000000;
            const uint64_t target = pc + signExtend(insn & 0x03FFFFFF, 26) * 4;
            if ((!link && !last) || displaced(target)) return {};
            out.emitJump(target, link);
        } else if ((insn & 0xFF000000) == 0x54000000 || (insn & 0x7E000000) == 0x34000000) { // B.cond, CBZ / CBNZ
            const uint64_t target = pc + signExtend((insn >> 5) & 0x7FFFF, 19) * 4;
            if (displaced(target)) return {};
            out.emitConditionalJump((insn & ~(0x7FFFFu << 5)) | (2 << 5), target);
        } else if ((insn & 0x7E000000) == 0x36000000) { // TBZ / TBNZ
            const uint64_t target = pc + signExtend((insn >> 5) & 0x3FFF, 14) * 4;
            if (displaced(target)) return {};
            out.emitConditionalJump((insn & ~(0x3FFFu << 5)) | (2 << 5), target);
        } else if ((insn & 0xFE000000) == 0xD6000000 && ((insn >> 21) & 7) != 1) { // BR, RET, ERET, not BLR
            if (!last) return {};
            out.emit(insn);
        } else {
            out.emit(insn);
        }
    }
    out.emitJump(reinterpret_cast<uint64_t>(source) + count * 4, false);
    return out.finish();
}

struct Hook {
    uint32_t* source;
    void* destination;
    void* trampoline = nullptr; // one page per hook, kept until destroy()
    size_t trampolineSize = 0;
    size_t words = 0;
    uint32_t saved[MAX_PATCH_WORDS]{};
    bool attached = false;
};

Hook* create(void* source, void* destination) { return new Hook{static_cast<uint32_t*>(source), destination}; }

bool attach(Hook* hook) {
    if (hook->attached) return true;
    const auto* source = hook->source;
    if (reinterpret_cast<uintptr_t>(source) % 4 != 0 || isPatched(source)) return false;
    const bool landingPad = isLandingPad(source[0]);
    const size_t words = landingPad ? 5 : 4;
    if (!hook->trampoline) {
        const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
        void* page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return false;
        const auto code = relocate(source, words, reinterpret_cast<uint64_t>(page));
        if (code.empty() || code.size() > pageSize) { munmap(page, pageSize); return false; }
        std::memcpy(page, code.data(), code.size());
        __builtin___clear_cache(static_cast<char*>(page), static_cast<char*>(page) + code.size());
        if (mprotect(page, pageSize, PROT_READ | PROT_EXEC) != 0) { munmap(page, pageSize); return false; }
        hook->trampoline = page;
        hook->trampolineSize = pageSize;
    }
    std::memcpy(hook->saved, source, words * 4);
    const auto patch = entryPatch(source[0], landingPad, hook->destination);
    if (!writeCode(hook->source, patch.data(), words * 4)) return false;
    hook->words = words;
    hook->attached = true;
    return true;
}

bool detach(Hook* hook) {
    if (hook->attached && writeCode(hook->source, hook->saved, hook->words * 4)) hook->attached = false;
    return !hook->attached;
}

void* original(const Hook* hook) { return hook->attached ? hook->trampoline : nullptr; }

void destroy(Hook* hook) {
    if (!hook) return;
    detach(hook);
    // A source that still jumps here keeps its trampoline mapped.
    if (hook->trampoline && !hook->attached) munmap(hook->trampoline, hook->trampolineSize);
    delete hook;
}
} // namespace hymission::hooks::aarch64
#endif
