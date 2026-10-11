#include "private_debug_log.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool ok, const char* label) { if (!ok) throw std::runtime_error(label); }
int main() {
    char pattern[] = "/tmp/hymission-log-test-XXXXXX";
    const char* temp = mkdtemp(pattern);
    require(temp, "temporary directory");
    const fs::path dir(temp), log = dir / "hymission-debug.log", target = dir / "target";
    try {
        setenv("XDG_RUNTIME_DIR", temp, 1);
        const auto previous = umask(0022);
        hymission::appendPrivateDebugLog("first");
        struct stat info{};
        require(stat(log.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "new log privacy under umask 022");
        chmod(log.c_str(), 0644);
        hymission::appendPrivateDebugLog("second");
        require(stat(log.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "existing permissions tightened");
        std::ifstream input(log); std::string contents((std::istreambuf_iterator<char>(input)), {});
        require(contents == "first\nsecond\n", "append preserves messages");
        input.close(); fs::remove(log);
        { std::ofstream out(target); out << "unchanged"; }
        fs::create_symlink(target, log);
        hymission::appendPrivateDebugLog("secret");
        require(fs::file_size(target) == 9, "symlink target untouched");
        fs::remove(log); fs::create_hard_link(target, log);
        hymission::appendPrivateDebugLog("secret");
        require(fs::file_size(target) == 9, "hard link target untouched");
        fs::remove(log); require(mkfifo(log.c_str(), 0600) == 0, "create FIFO");
        hymission::appendPrivateDebugLog("secret"); fs::remove(log);
        fs::create_directory(log); hymission::appendPrivateDebugLog("secret"); fs::remove(log);
        chmod(dir.c_str(), 0755); hymission::appendPrivateDebugLog("secret");
        require(!fs::exists(log), "public directory rejected"); chmod(dir.c_str(), 0700);
        fs::create_directory_symlink(dir, dir / "alias");
        setenv("XDG_RUNTIME_DIR", (dir / "alias").c_str(), 1); hymission::appendPrivateDebugLog("secret");
        require(!fs::exists(log), "symlink runtime directory rejected");
        unsetenv("XDG_RUNTIME_DIR"); hymission::appendPrivateDebugLog("secret");
        setenv("XDG_RUNTIME_DIR", "relative", 1); hymission::appendPrivateDebugLog("secret");
        require(!fs::exists(log), "missing and relative runtime rejected");
        umask(previous); fs::remove_all(dir);
        std::cout << "PASS: private debug log permissions and unsafe paths\n";
    } catch (const std::exception& e) {
        fs::remove_all(dir); std::cerr << e.what() << '\n'; return 1;
    }
}
