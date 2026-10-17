#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace hymission::hooks {
// Hyprland fixes RIP-relative memory operands, but copies relative branches
// verbatim. Keep its instruction offsets and RIP fixes intact. Short branches
// use nearby jump islands after the native return jump; near branches are
// adjusted in place. No flags, registers or original instruction sizes change.
inline std::optional<std::vector<unsigned char>> relocateEntryBranches(
    std::span<const unsigned char> original, std::span<const unsigned char> native,
    std::span<const size_t> lengths, uintptr_t source, uintptr_t landing, size_t capacity) {
    if (native.size()!=original.size()+5 || native.size()>capacity) return {};
    std::vector<unsigned char> result(native.begin(),native.end());
    auto relative32=[&](size_t operand, uintptr_t target, uintptr_t next) {
        const auto delta=static_cast<int64_t>(target)-static_cast<int64_t>(next);
        if (delta<std::numeric_limits<int32_t>::min() || delta>std::numeric_limits<int32_t>::max()) return false;
        const auto value=static_cast<int32_t>(delta);
        std::memcpy(result.data()+operand,&value,sizeof(value));
        return true;
    };
    auto mapped=[&](uintptr_t target) -> std::optional<uintptr_t> {
        if (target<source || target>=source+original.size()) return target;
        size_t offset=0;
        for (auto length:lengths) {
            if (source+offset==target) return landing+offset;
            offset+=length;
        }
        return {}; // branch into the middle of a copied instruction
    };
    size_t offset=0;
    for (const auto length:lengths) {
        if (!length || offset+length>original.size()) return {};
        const auto code=original.subspan(offset,length);
        size_t op=0;
        while (op<length && (code[op]==0x66 || code[op]==0x67 || code[op]==0xf2 || code[op]==0xf3 ||
               code[op]==0x2e || code[op]==0x3e || code[op]==0x26 || code[op]==0x36 || code[op]==0x64 || code[op]==0x65 ||
               (code[op]>=0x40 && code[op]<=0x4f))) ++op;
        if (op==length) return {};
        const bool shortBranch=code[op]==0xeb || (code[op]>=0x70 && code[op]<=0x7f) || (code[op]>=0xe0 && code[op]<=0xe3);
        const bool nearBranch=code[op]==0xe8 || code[op]==0xe9 || (code[op]==0x0f && op+1<length && code[op+1]>=0x80 && code[op+1]<=0x8f);
        if (shortBranch || nearBranch) {
            const size_t operand=op+((code[op]==0x0f)?2:1);
            const size_t width=shortBranch?1:4;
            if (operand+width!=length) return {};
            int64_t delta;
            if (shortBranch) { int8_t v; std::memcpy(&v,code.data()+operand,1); delta=v; }
            else { int32_t v; std::memcpy(&v,code.data()+operand,4); delta=v; }
            const auto target=mapped(source+offset+length+delta);
            if (!target) return {};
            if (shortBranch) {
                const auto island=result.size();
                const auto distance=static_cast<int64_t>(island)-static_cast<int64_t>(offset+length);
                if (island+5>capacity || distance>127 || distance< -128) return {};
                result[offset+operand]=static_cast<unsigned char>(static_cast<int8_t>(distance));
                result.resize(island+5); result[island]=0xe9;
                if (!relative32(island+1,*target,landing+island+5)) return {};
            } else if (!relative32(offset+operand,*target,landing+offset+length)) return {};
        }
        offset+=length;
    }
    if (offset!=original.size()) return {};
    return result;
}
}
