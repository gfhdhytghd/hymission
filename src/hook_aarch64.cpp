#include "hook_aarch64.hpp"
#if defined(__aarch64__)
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

    Jumps out of the trampoline use B / BL within +-128 MiB and X17 otherwise: BLR X17 for calls,
    whose target is a function entry, and RET X17 for jumps, because the resume address is in the
    middle of a function where no landing pad exists and RET is not subject to BTI.

    X17 must not be live across those jumps, so an entry whose displaced instructions name X17 is
    refused. At a function entry X17 holds nothing (see above), so only those instructions could
    make it live.

    A later branch back into the displaced bytes cannot be detected, the same limit as on x86.
    RET X17 would also trip a guarded control stack (GCS), which Hyprland does not enable.
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

// mprotect() without PROT_BTI would silently drop a BTI guard from the mapping.
bool isBtiGuarded(uintptr_t address) {
    std::ifstream smaps("/proc/self/smaps");
    bool inRange = false;
    for (std::string line; std::getline(smaps, line);) {
        if (line.starts_with("VmFlags:")) {
            if (inRange) return line.contains(" bt");
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
    return false;
}

// The page stays executable throughout: another thread may be running code on it.
// True once the code is written, even if the page could not be made read-only again.
bool writeCode(void* target, const void* code, size_t size) {
    const auto pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGE_SIZE));
    const auto address = reinterpret_cast<uintptr_t>(target);
    const auto start = address & ~(pageSize - 1);
    const auto length = ((address + size + pageSize - 1) & ~(pageSize - 1)) - start;
    const int protection = PROT_READ | PROT_EXEC | (isBtiGuarded(address) ? PROT_BTI : 0);
    if (mprotect(reinterpret_cast<void*>(start), length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    std::memcpy(target, code, size);
    mprotect(reinterpret_cast<void*>(start), length, protection);
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
        emitLoad(LDR_X17_LITERAL, target);
        emit(link ? 0xD63F0220 : 0xD65F0220); // BLR X17 / RET X17
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

// Bits 4:0 hold the destination (or a stored register) of every non-branch instruction, and
// bits 14:10 the second register of a load/store pair. Branches use them for offsets.
bool mayWriteX17(uint32_t insn) {
    if ((insn & 0x1C000000) == 0x14000000) return false; // branches, exceptions, system
    return (insn & 0x1F) == 17 || ((insn & 0x3A000000) == 0x28000000 && ((insn >> 10) & 0x1F) == 17);
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
    An unconditional branch before the last displaced instruction means the function is shorter
    than the patch and the bytes after it belong to something else, so that is refused.
*/
std::vector<uint8_t> relocate(const uint32_t* source, size_t count, uint64_t address) {
    Assembler out{.base = address};
    for (size_t i = 0; i < count; ++i) {
        const uint32_t insn = source[i];
        const uint64_t pc = reinterpret_cast<uint64_t>(source) + i * 4;
        const bool last = i + 1 == count;
        if (mayWriteX17(insn)) return {};
        if ((insn & 0x9F000000) == 0x10000000) { // ADR
            const uint64_t imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2);
            out.emitLoad(0x58000000 | (insn & 0x1F), pc + signExtend(imm, 21));
        } else if ((insn & 0x9F000000) == 0x90000000) { // ADRP
            const uint64_t imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2);
            out.emitLoad(0x58000000 | (insn & 0x1F), (pc & ~0xFFFULL) + (signExtend(imm, 21) << 12));
        } else if ((insn & 0x3B000000) == 0x18000000) { // LDR, LDRSW, PRFM (literal)
            const uint64_t target = pc + signExtend((insn >> 5) & 0x7FFFF, 19) * 4;
            const uint32_t opc = insn >> 30, rt = insn & 0x1F;
            if (insn & (1 << 26)) { // SIMD&FP
                if (opc == 3) return {}; // unallocated
                static constexpr uint32_t loads[] = {0xBD400000, 0xFD400000, 0x3DC00000};
                out.emitLoad(LDR_X17_LITERAL, target);
                out.emit(loads[opc] | (17 << 5) | rt);
            } else if (opc == 3) {
                out.emit(NOP); // PRFM is a hint
            } else {
                static constexpr uint32_t loads[] = {0xB9400000, 0xF9400000, 0xB9800000};
                out.emitLoad(0x58000000 | rt, target);
                out.emit(loads[opc] | (rt << 5) | rt);
            }
        } else if ((insn & 0x7C000000) == 0x14000000) { // B / BL
            const bool link = insn & 0x80000000;
            if (!link && !last) return {};
            out.emitJump(pc + signExtend(insn & 0x03FFFFFF, 26) * 4, link);
        } else if ((insn & 0xFF000000) == 0x54000000 || (insn & 0x7E000000) == 0x34000000) { // B.cond, CBZ / CBNZ
            out.emitConditionalJump((insn & ~(0x7FFFFu << 5)) | (2 << 5), pc + signExtend((insn >> 5) & 0x7FFFF, 19) * 4);
        } else if ((insn & 0x7E000000) == 0x36000000) { // TBZ / TBNZ
            out.emitConditionalJump((insn & ~(0x3FFFu << 5)) | (2 << 5), pc + signExtend((insn >> 5) & 0x3FFF, 14) * 4);
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

void detach(Hook* hook) {
    if (!hook->attached) return;
    if (writeCode(hook->source, hook->saved, hook->words * 4)) hook->attached = false;
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
