#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <wayland-server-core.h>

namespace hymission {

// The compositor uses signalfd/Wayland signal sources and blocks SIGTERM.
// A spawned GTK process must not inherit that mask or ignored dispositions.
inline int configureSearchChildSignals(posix_spawnattr_t& attributes) {
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    for (int signal : {SIGTERM, SIGINT, SIGPIPE, SIGCHLD})
        sigaddset(&defaults, signal);
    int error = posix_spawnattr_setsigmask(&attributes, &mask);
    if (!error)
        error = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (!error)
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    return error;
}

class SearchChildReaper {
  public:
    SearchChildReaper() = default;
    SearchChildReaper(const SearchChildReaper&) = delete;
    SearchChildReaper& operator=(const SearchChildReaper&) = delete;

    ~SearchChildReaper() {
        if (m_timer)
            wl_event_source_remove(m_timer);
        for (const auto& child : m_children) {
            if (!reaped(child.pid)) {
                kill(child.pid, SIGKILL);
                (void)reaped(child.pid);
            }
        }
    }

    void retire(pid_t pid, wl_event_loop* loop) {
        if (pid <= 0 || reaped(pid))
            return;
        kill(pid, SIGTERM);
        m_children.push_back({pid, Clock::now() + std::chrono::milliseconds(250)});
        if (!m_timer && loop)
            m_timer = wl_event_loop_add_timer(loop, [](void* data) {
                static_cast<SearchChildReaper*>(data)->poll();
                return 0;
            }, this);
        if (m_timer)
            wl_event_source_timer_update(m_timer, 10);
        else {
            // Failure/unload paths must not block the compositor either.
            kill(pid, SIGKILL);
            poll();
        }
    }

    bool empty() const { return m_children.empty(); }

  private:
    using Clock = std::chrono::steady_clock;
    struct Child { pid_t pid; Clock::time_point deadline; };
    std::vector<Child> m_children;
    wl_event_source* m_timer = nullptr;

    static bool reaped(pid_t pid) {
        const pid_t result = waitpid(pid, nullptr, WNOHANG);
        return result == pid || (result < 0 && errno == ECHILD);
    }

    void poll() {
        for (auto it = m_children.begin(); it != m_children.end();) {
            if (reaped(it->pid)) {
                it = m_children.erase(it);
                continue;
            }
            if (Clock::now() >= it->deadline)
                kill(it->pid, SIGKILL);
            ++it;
        }
        if (m_timer && !m_children.empty())
            wl_event_source_timer_update(m_timer, 10);
    }
};

} // namespace hymission
