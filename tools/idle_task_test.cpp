#include "idle_task.hpp"

#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

// The tests never call the plugin's own event loop accessor.
wl_event_loop* hymission::hyprlandEventLoop() {
    return nullptr;
}

namespace {
bool expect(bool condition, const char* message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

// Hyprland's doLater(): one idle source whose callback first moves the whole
// queue into a local list and then runs it, see EventLoopManager.cpp.
struct DoLaterQueue {
    wl_event_source*                   source = nullptr;
    std::vector<std::function<void()>> fns;

    void post(wl_event_loop* loop, std::function<void()> fn) {
        fns.push_back(std::move(fn));
        if (!source)
            source = wl_event_loop_add_idle(
                loop,
                [](void* data) {
                    auto* queue   = static_cast<DoLaterQueue*>(data);
                    auto  batch   = std::move(queue->fns);
                    queue->source = nullptr;
                    for (auto& fn : batch)
                        if (fn)
                            fn();
                },
                this);
    }
};

// An owner whose replacement deliberately reuses the old address, as a new
// config manager may after the old one was freed.
struct FakeOwner {
    int*                  address = nullptr;
    std::shared_ptr<bool> alive;
    int*                  get() const { return address; }
    explicit              operator bool() const { return alive && *alive; }
};
struct FakeWeak {
    int*                  address = nullptr;
    std::shared_ptr<bool> alive;
    FakeWeak() = default;
    explicit FakeWeak(const FakeOwner& owner) : address(owner.address), alive(owner.alive) {}
    bool expired() const { return !alive || !*alive; }
    int* get() const { return address; }
};

// What PLUGIN_EXIT tears down: every deferred callback the plugin owns.
struct Plugin {
    hymission::IdleTasks                tasks;
    std::unique_ptr<hymission::IdleTask> single;
};
} // namespace

int main() {
    bool ok   = true;
    auto loop = wl_event_loop_create();

    {
        // A config reload running in Hyprland's batch unloads the plugin while
        // plugin callbacks queued after the batch began are still waiting.
        DoLaterQueue hyprland;
        auto         plugin = std::make_unique<Plugin>();
        int          ran    = 0;
        int          staleDoLater = 0;
        hyprland.post(loop, [&] { plugin.reset(); });
        // A raw doLater entry in the same batch cannot be cancelled any more.
        hyprland.post(loop, [&] { ++staleDoLater; });
        plugin->tasks.post(loop, [&] { ++ran; });
        plugin->tasks.post(loop, [&] { ++ran; });
        plugin->single = hymission::IdleTask::later(loop, [&] { ++ran; });
        wl_event_loop_dispatch(loop, 0);
        ok &= expect(!plugin, "the simulated reload unloaded the plugin");
        ok &= expect(ran == 0, "owned idle callbacks never run after their owner was destroyed during another idle callback");
        ok &= expect(staleDoLater == 1, "a doLater entry moved into the running batch still runs after unload (the failure owned tasks avoid)");
    }
    {
        // Normal operation: callbacks run once, in order, and a callback may
        // drop its own task or post another one.
        Plugin                    plugin;
        std::vector<int>          order;
        plugin.tasks.post(loop, [&] { order.push_back(1); });
        plugin.single = hymission::IdleTask::later(loop, [&] {
            plugin.single.reset();
            order.push_back(2);
            plugin.tasks.post(loop, [&] { order.push_back(3); });
        });
        ok &= expect(plugin.tasks.pending() == 1 && plugin.single && plugin.single->pending(), "posted callbacks wait for the next idle dispatch");
        wl_event_loop_dispatch(loop, 0);
        wl_event_loop_dispatch(loop, 0);
        ok &= expect((order == std::vector<int>{1, 2, 3}), "owned callbacks run once in posting order, including ones posted from a callback");
        ok &= expect(!plugin.single && plugin.tasks.pending() == 0, "finished tasks are no longer pending");
    }
    {
        // Dropping a task before the dispatch cancels it.
        int  ran  = 0;
        auto task = hymission::IdleTask::later(loop, [&] { ++ran; });
        hymission::IdleTasks tasks;
        tasks.post(loop, [&] { ++ran; });
        task.reset();
        tasks.clear();
        wl_event_loop_dispatch(loop, 0);
        ok &= expect(ran == 0, "a cancelled task never runs");
        ok &= expect(!hymission::IdleTask::later(nullptr, [] {}) && !hymission::IdleTask::later(loop, {}), "no loop or no callback queues nothing");
    }
    {
        // A full-reset reload destroys the config manager and creates a new
        // one; an ordinary reload keeps it.
        using Manager = Hyprutils::Memory::CUniquePointer<int>;
        hymission::OwnerBinding<Hyprutils::Memory::CWeakPointer<int>> binding;
        Manager manager = Hyprutils::Memory::makeUnique<int>(1);
        ok &= expect(binding.rebind(manager), "the first manager is bound");
        ok &= expect(!binding.rebind(manager) && !binding.rebind(manager), "an ordinary reload with the same manager registers nothing again");
        manager.reset();
        manager = Hyprutils::Memory::makeUnique<int>(2);
        ok &= expect(binding.rebind(manager), "a replaced manager is bound again, even at a reused address");
        ok &= expect(!binding.rebind(manager), "the replacement is then remembered");
        Manager none;
        ok &= expect(!binding.rebind(none), "no manager binds nothing");
        ok &= expect(binding.rebind(manager), "a manager after none is bound again");
    }

    {
        int                                   storage = 0;
        hymission::OwnerBinding<FakeWeak>     binding;
        FakeOwner                             first{&storage, std::make_shared<bool>(true)};
        ok &= expect(binding.rebind(first) && !binding.rebind(first), "the first owner is bound once");
        *first.alive = false;
        FakeOwner second{&storage, std::make_shared<bool>(true)};
        ok &= expect(binding.rebind(second), "a replacement at the very same address is still bound again");
    }

    wl_event_loop_destroy(loop);
    return ok ? 0 : 1;
}
