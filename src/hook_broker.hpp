#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace hymission::hooks {
// These five operations are the only dependency on Hyprland's patching API.
void* nativeCreate(void* owner, void* source, void* destination);
bool nativeAttach(void* hook);
void nativeDetach(void* hook);
void* nativeOriginal(void* hook);
void nativeRemove(void* owner, void* hook);
struct Group;
struct Lease {
    Group* group = nullptr;
    void* destination = nullptr;
    void* m_original = nullptr;
    bool enabled = false;
    bool hook();
    bool unhook();
};
struct Group {
    void* owner = nullptr;
    void* source = nullptr;
    void* native = nullptr;
    bool attached = false;
    std::vector<Lease*> leases;
    virtual ~Group() { if (native) nativeRemove(owner, native); }
    virtual void* bridge() = 0;
    virtual void* continuation() = 0;
    Lease* add(void* destination) {
        auto* l = new Lease{this, destination, continuation()};
        leases.push_back(l);
        return l;
    }
};
inline std::map<void*, std::unique_ptr<Group>>& groups() { static std::map<void*, std::unique_ptr<Group>> data; return data; }
inline bool Lease::hook() {
    if (enabled) return true;
    if (!group->native) group->native = nativeCreate(group->owner, group->source, group->bridge());
    if (!group->native) return false;
    if (!group->attached) {
        if (!nativeAttach(group->native)) return false;
        group->attached = true;
    }
    enabled = true;
    return true;
}
inline bool Lease::unhook() {
    enabled = false;
    if (group->attached && std::ranges::none_of(group->leases, [](auto* l) { return l->enabled; })) {
        nativeDetach(group->native);
        group->attached = false;
    }
    return true;
}
inline void remove(void*, Lease* lease) {
    if (!lease) return;
    auto* g = lease->group;
    lease->unhook();
    std::erase(g->leases, lease);
    delete lease;
    if (g->leases.empty()) groups().erase(g->source);
}

// Each source is patched once. Independent controller leases can be enabled,
// disabled or rolled back without changing anybody else's original pointer.
// Reentrant native calls restart at the head; an original call continues after
// the current subscriber. Newest subscriber is outermost, like nested wrappers.
template<class> struct TypedGroup;
template<class R, class... A> struct TypedGroup<R (*)(A...)> final : Group {
    using Fn = R (*)(A...);
    static constexpr size_t capacity = 16;
    inline static std::array<TypedGroup*, capacity> channels{};
    inline static thread_local std::array<size_t, capacity> cursors{};
    size_t channel = capacity;
    TypedGroup() {
        for (size_t i = 0; i < capacity; ++i) if (!channels[i]) { channel = i; channels[i] = this; break; }
    }
    ~TypedGroup() override { if (channel != capacity) channels[channel] = nullptr; }
    R call(size_t from, A... args) {
        const auto saved = cursors[channel];
        struct Restore { size_t& value; size_t saved; ~Restore() { value = saved; } } restore{cursors[channel], saved};
        for (size_t n = from; n < leases.size(); ++n) {
            auto* l = leases[leases.size() - n - 1];
            if (!l->enabled) continue;
            cursors[channel] = n + 1;
            return reinterpret_cast<Fn>(l->destination)(std::forward<A>(args)...);
        }
        return reinterpret_cast<Fn>(nativeOriginal(native))(std::forward<A>(args)...);
    }
    template<size_t I> static R entry(A... args) { return channels[I]->call(0, std::forward<A>(args)...); }
    template<size_t I> static R next(A... args) { return channels[I]->call(cursors[I], std::forward<A>(args)...); }
    template<size_t... I> static auto entries(std::index_sequence<I...>) { return std::array<Fn, capacity>{&entry<I>...}; }
    template<size_t... I> static auto continuations(std::index_sequence<I...>) { return std::array<Fn, capacity>{&next<I>...}; }
    void* bridge() override { return channel == capacity ? nullptr : reinterpret_cast<void*>(entries(std::make_index_sequence<capacity>{})[channel]); }
    void* continuation() override { return channel == capacity ? nullptr : reinterpret_cast<void*>(continuations(std::make_index_sequence<capacity>{})[channel]); }
};
using Factory = std::function<std::unique_ptr<Group>()>;
inline auto& factories() { static std::map<void*, Factory> data; return data; }
template<class Fn> void* target(Fn function) {
    const auto key = reinterpret_cast<void*>(function);
    factories().try_emplace(key, [] { return std::make_unique<TypedGroup<Fn>>(); });
    return key;
}
inline Lease* create(void* owner, void* source, void* destination) {
    if (!source || !destination) return nullptr;
    auto it = groups().find(source);
    if (it == groups().end()) {
        auto f = factories().find(destination);
        if (f == factories().end()) return nullptr;
        auto g = f->second();
        if (!g->bridge() || !g->continuation()) return nullptr;
        g->owner = owner; g->source = source;
        it = groups().emplace(source, std::move(g)).first;
    }
    return it->second->add(destination);
}
} // namespace hymission::hooks
