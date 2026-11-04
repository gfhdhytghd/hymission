#include "hook_symbols.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

int main(int argc, char** argv) {
    auto require=[](bool ok) { if (!ok) throw std::runtime_error("lookup regression"); };
    require(hymission::hooks::tryLookup([] { return std::vector<int>{42}; }) == std::vector<int>{42});
    require(hymission::hooks::tryLookup([]() -> std::vector<int> { throw std::runtime_error("lookup failed"); }).empty());
    if (argc == 2 && std::string_view(argv[1]) == "--deleted-executable") {
        // Run a disposable copy only. Reproduce the actual native API failure.
        const auto executable = std::filesystem::canonical("/proc/self/exe");
        require(std::filesystem::remove(executable));
        bool threw = false;
        auto result = hymission::hooks::tryLookup([&]() -> std::vector<int> {
            try { (void)std::filesystem::canonical("/proc/self/exe"); }
            catch (const std::filesystem::filesystem_error&) { threw = true; throw; }
            return {1};
        });
        require(threw && result.empty());
    }
    std::cout << "function lookup regression passed\n";
}
