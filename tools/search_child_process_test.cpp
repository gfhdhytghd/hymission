#include "search_child_process.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>
extern char** environ;
using namespace std::chrono_literals;
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void testRetirement(int mode, wl_event_loop* loop) {
    int ready[2];
    check(pipe(ready) == 0, "pipe");
    const pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        close(ready[0]);
        if (mode == 1) {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGTERM);
            sigprocmask(SIG_BLOCK, &mask, nullptr);
        } else if (mode == 2) signal(SIGTERM, SIG_IGN);
        const char byte = 'R';
        (void)write(ready[1], &byte, 1);
        close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]);
    char byte;
    check(read(ready[0], &byte, 1) == 1, "child ready");
    close(ready[0]);
    hymission::SearchChildReaper reaper;
    const auto start = std::chrono::steady_clock::now();
    reaper.retire(pid, loop);
    check(std::chrono::steady_clock::now() - start < 100ms, "retire blocked");
    int ticks = 0;
    while (!reaper.empty() && std::chrono::steady_clock::now() - start < 2s) {
        wl_event_loop_dispatch(loop, 5);
        ++ticks;
    }
    if (!reaper.empty()) {
        kill(pid, SIGKILL);
        throw std::runtime_error("child not reaped by deadline");
    }
    check(waitpid(pid, nullptr, WNOHANG) == -1 && errno == ECHILD, "zombie remains");
    if (mode) check(ticks >= 20, "event loop stalled during grace period");
    std::cout << "retirement mode " << mode << ": responsive, child reaped\n";
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--check-signals") {
        sigset_t mask;
        sigprocmask(SIG_SETMASK, nullptr, &mask);
        struct sigaction action{};
        sigaction(SIGTERM, nullptr, &action);
        return sigismember(&mask, SIGTERM) || action.sa_handler != SIG_DFL;
    }
    try {
        sigset_t blocked, original;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTERM);
        sigprocmask(SIG_BLOCK, &blocked, &original);
        struct sigaction ignored{}, previous{};
        ignored.sa_handler = SIG_IGN;
        sigemptyset(&ignored.sa_mask);
        sigaction(SIGTERM, &ignored, &previous);
        posix_spawnattr_t attributes;
        check(posix_spawnattr_init(&attributes) == 0, "spawn attributes");
        check(hymission::configureSearchChildSignals(attributes) == 0, "child signals");
        pid_t pid;
        char option[] = "--check-signals";
        char* args[] = {argv[0], option, nullptr};
        const int error = posix_spawn(&pid, argv[0], nullptr, &attributes, args, environ);
        posix_spawnattr_destroy(&attributes);
        sigprocmask(SIG_SETMASK, &original, nullptr);
        sigaction(SIGTERM, &previous, nullptr);
        check(error == 0, "spawn");
        int status = 0;
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "inherited signal state not reset");
        auto* loop = wl_event_loop_create();
        check(loop != nullptr, "event loop");
        for (int mode : {0, 1, 2}) testRetirement(mode, loop);
        wl_event_loop_destroy(loop);
        std::cout << "child signal reset passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
