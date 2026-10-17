#pragma once
#include <cctype>
#include <cstdlib>
#include <cxxabi.h>
#include <memory>
#include <vector>
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

// SFunctionMatch::demangled is paired by an external nm line counter in
// Hyprland. Failed dlsym entries can shift it onto an unrelated symbol. Decode
// the actual lookup key instead: it is the name that produced match.address.
inline std::string canonicalSymbol(const std::string& mangled) {
    if (!mangled.starts_with("_Z")) return mangled; // unmangled C symbol
    int status=0;
    const std::unique_ptr<char,decltype(&std::free)> name{abi::__cxa_demangle(mangled.c_str(),nullptr,nullptr,&status),&std::free};
    return status==0 && name?std::string{name.get()}:std::string{};
}

// Aliases at the same address are one symbol. Existing controllers may retain
// their historical substring lookup; new Tiles hooks require full signatures.
template<class Matches> void* findSymbol(const Matches& matches,std::string_view signature,bool strict=false) {
    const auto needle=normalizedSignature(signature);
    std::vector<std::pair<void*,std::string>> candidates;
    for (const auto& match:matches) if (match.address) {
        auto name=normalizedSignature(canonicalSymbol(match.signature));
        if (!name.empty()) candidates.emplace_back(match.address,std::move(name));
    }
    auto select=[&](auto predicate) -> std::pair<void*,bool> {
        void* address=nullptr;
        for (const auto& [candidate,name]:candidates) if (predicate(name)) {
            if (address && address!=candidate) return {nullptr,true};
            address=candidate;
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
