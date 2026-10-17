#pragma once
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace hymission::hooks {
inline std::string normalizedSignature(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (unsigned char c:input) if (!std::isspace(c)) result+=static_cast<char>(c);
    return result;
}

// Aliases at the same address are one symbol. Existing controllers may retain
// their historical substring lookup; new Tiles hooks require full signatures.
template<class Matches> void* findSymbol(const Matches& matches,std::string_view signature,bool strict=false) {
    const auto needle=normalizedSignature(signature);
    auto select=[&](auto predicate) -> std::pair<void*,bool> {
        void* address=nullptr;
        for (const auto& match:matches) if (match.address && predicate(normalizedSignature(match.demangled))) {
            if (address && address!=match.address) return {nullptr,true};
            address=match.address;
        }
        return {address,false};
    };
    const auto exact=select([&](const auto& name) { return name==needle; });
    if (exact.first || exact.second || strict) return exact.first;
    const auto loose=select([&](const auto& name) { return name.find(needle)!=std::string::npos; });
    if (loose.first || loose.second) return loose.first;
    const auto paren=needle.find('(');
    if (paren==std::string::npos) return nullptr;
    const auto legacy=needle.substr(0,paren+1);
    return select([&](const auto& name) { return name.find(legacy)!=std::string::npos; }).first;
}
}
