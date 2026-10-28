#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <wayland-server-core.h>

namespace hymission {

// The compositor's wayland event loop; defined by the plugin entry point.
wl_event_loop* hyprlandEventLoop();

// A deferred callback owned by the plugin. Hyprland's doLater() moves its whole
// queue into a local list before running it, so an entry cannot be cancelled
// once that batch has started: a config reload in the batch can unload the
// plugin, and the next entry then jumps into unmapped code. Each IdleTask is a
// wayland idle source of its own instead. Destroying the task removes the
// source, and libwayland never runs a removed idle source, even while it is
// dispatching another one.
class IdleTask {
  public:
    static std::unique_ptr<IdleTask> later(wl_event_loop* loop, std::function<void()> fn) {
        if (!loop || !fn)
            return nullptr;
        auto task = std::unique_ptr<IdleTask>(new IdleTask(std::move(fn)));
        task->m_source = wl_event_loop_add_idle(loop, &IdleTask::dispatch, task.get());
        return task->m_source ? std::move(task) : nullptr;
    }
    static std::unique_ptr<IdleTask> later(std::function<void()> fn) { return later(hyprlandEventLoop(), std::move(fn)); }

    ~IdleTask() {
        if (m_source)
            wl_event_source_remove(m_source);
    }
    IdleTask(const IdleTask&)            = delete;
    IdleTask& operator=(const IdleTask&) = delete;

    bool pending() const { return m_source != nullptr; }

  private:
    explicit IdleTask(std::function<void()> fn) : m_fn(std::move(fn)) {}

    // libwayland removes the source once this returns. The callback may destroy
    // its own task, so nothing touches the task after the callback starts.
    static void dispatch(void* data) {
        auto* task     = static_cast<IdleTask*>(data);
        task->m_source = nullptr;
        const auto fn  = std::move(task->m_fn);
        fn();
    }

    wl_event_source*      m_source = nullptr;
    std::function<void()> m_fn;
};

// Fire-and-forget callbacks of one owner. Destroying the set cancels them.
class IdleTasks {
  public:
    void post(wl_event_loop* loop, std::function<void()> fn) {
        std::erase_if(m_tasks, [](const auto& task) { return !task->pending(); });
        if (auto task = IdleTask::later(loop, std::move(fn)))
            m_tasks.push_back(std::move(task));
    }
    void post(std::function<void()> fn) { post(hyprlandEventLoop(), std::move(fn)); }
    void clear() { m_tasks.clear(); }
    std::size_t pending() const { return std::ranges::count_if(m_tasks, [](const auto& task) { return task->pending(); }); }

  private:
    std::vector<std::unique_ptr<IdleTask>> m_tasks;
};

// Remembers which instance of a replaceable owner (e.g. the config manager)
// something was registered with. rebind() is true for the first instance and
// again after it was replaced, even when the new one reuses the old address.
template <class Weak>
class OwnerBinding {
  public:
    template <class Owner>
    bool rebind(const Owner& current) {
        if (!current) {
            m_bound = {};
            return false;
        }
        if (!m_bound.expired() && m_bound.get() == current.get())
            return false;
        m_bound = Weak(current);
        return true;
    }

  private:
    Weak m_bound;
};

} // namespace hymission
