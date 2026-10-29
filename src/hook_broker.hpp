#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <expected>
#include <string>
#include <map>
#include <memory>
#include <type_traits>
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
    int priority = 0;
    size_t continuationIndex = 0;
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
    virtual void* continuation(Lease*) = 0;
    virtual void releaseContinuation(Lease*) = 0;
    Lease* add(void* destination) {
        auto* l = new Lease{this, destination};
        l->m_original=continuation(l);
        if (!l->m_original) { delete l; return nullptr; }
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
        for (auto* lease:group->leases) lease->m_original=group->continuation(lease);
    }
    enabled = true;
    std::stable_sort(group->leases.begin(),group->leases.end(),[](const auto* a,const auto* b) { return a->priority<b->priority; });
    return true;
}
inline bool Lease::unhook() {
    enabled = false;
    if (group->attached && std::ranges::none_of(group->leases, [](auto* l) { return l->enabled; })) {
        nativeDetach(group->native);
        group->attached = false;
        for (auto* lease:group->leases) lease->m_original=nullptr;
    }
    return true;
}
inline void remove(void*, Lease* lease) {
    if (!lease) return;
    auto* g = lease->group;
    lease->unhook();
    std::erase(g->leases, lease);
    g->releaseContinuation(lease);
    delete lease;
    if (g->leases.empty()) groups().erase(g->source);
}

// Each source is patched once. Independent controller leases can be enabled,
// disabled or rolled back without changing anybody else's original pointer.
// Reentrant native calls restart at the head. Every lease has its own original
// continuation, also valid when called outside its hook (preview helpers do so).
// Newest subscriber is outermost, with explicit priority for work-area ordering.
template<class> struct TypedGroup;
template<class R, class... A> struct TypedGroup<R (*)(A...)> final : Group {
    using Fn = R (*)(A...);
    static constexpr size_t capacity = 16;
    static constexpr size_t continuationCapacity = 64;
    inline static std::array<TypedGroup*, capacity> channels{};
    inline static std::array<Lease*, continuationCapacity> originals{};
    size_t channel = capacity;
    static R unavailable() {
        if constexpr (std::is_void_v<R>) return;
        else if constexpr (std::is_reference_v<R>) {
            static std::remove_reference_t<R> empty{};
            return static_cast<R>(empty);
        } else if constexpr (requires { typename R::error_type; typename R::value_type; }) {
            // ActionResult/expected default construction means success. A
            // removed continuation must fail explicitly instead of claiming
            // that an action it never called completed successfully.
            using Error=typename R::error_type;
            if constexpr (std::is_constructible_v<Error,std::string>)
                return R{std::unexpect,std::string{"hook continuation unavailable"}};
            else return R{std::unexpect};
        } else if constexpr (requires(R value) { value.success=false; value.error=std::string{}; }) {
            R value{};
            value.success=false;
            value.error="hook continuation unavailable";
            return value;
        } else return R{};
    }
    TypedGroup() {
        for (size_t i = 0; i < capacity; ++i) if (!channels[i]) { channel = i; channels[i] = this; break; }
    }
    ~TypedGroup() override { if (channel != capacity) channels[channel] = nullptr; }
    // Entry owns the native ABI arguments. Borrow them through the broker;
    // only an actual subscriber/native call receives its by-value arguments.
    // Pointer/scalar arguments and references on getTexBox/draw need no object
    // copies. Non-trivial native ABI values retain an owned copy per callee.
    template<class T> static decltype(auto) pass(std::remove_reference_t<T>& value) {
        if constexpr (std::is_reference_v<T>) return std::forward<T>(value);
        else if constexpr (std::is_trivially_copyable_v<T>) return std::move(value);
        else return std::as_const(value);
    }
    R call(size_t from, A&... args) {
        // unhook() destroys Hyprland's trampoline. The restored source is the
        // only valid native entry after detach; never retain that trampoline.
        if (!attached) return source?reinterpret_cast<Fn>(source)(pass<A>(args)...):unavailable();
        for (size_t n = from; n < leases.size(); ++n) {
            auto* l = leases[leases.size() - n - 1];
            if (!l->enabled) continue;
            return reinterpret_cast<Fn>(l->destination)(pass<A>(args)...);
        }
        const auto original=native?nativeOriginal(native):nullptr;
        return original?reinterpret_cast<Fn>(original)(pass<A>(args)...):unavailable();
    }
    template<size_t I> static R entry(A... args) { return channels[I]?channels[I]->call(0, args...):unavailable(); }
    template<size_t I> static R next(A... args) {
        auto* lease=originals[I];
        if (!lease) return unavailable();
        auto* group=static_cast<TypedGroup*>(lease->group);
        const auto it=std::ranges::find(group->leases,lease);
        const auto from=it==group->leases.end()?group->leases.size():group->leases.size()-static_cast<size_t>(it-group->leases.begin());
        return group->call(from,args...);
    }
    template<size_t... I> static auto entries(std::index_sequence<I...>) { return std::array<Fn, capacity>{&entry<I>...}; }
    template<size_t... I> static auto continuations(std::index_sequence<I...>) { return std::array<Fn, sizeof...(I)>{&next<I>...}; }
    void* bridge() override { return channel == capacity ? nullptr : reinterpret_cast<void*>(entries(std::make_index_sequence<capacity>{})[channel]); }
    void* continuation(Lease* lease) override {
        for (size_t i=0;i<continuationCapacity;++i) if (originals[i]==lease)
            return reinterpret_cast<void*>(continuations(std::make_index_sequence<continuationCapacity>{})[i]);
        for (size_t i=0;i<continuationCapacity;++i) if (!originals[i]) {
            originals[i]=lease; lease->continuationIndex=i;
            return reinterpret_cast<void*>(continuations(std::make_index_sequence<continuationCapacity>{})[i]);
        }
        return nullptr;
    }
    void releaseContinuation(Lease* lease) override { originals[lease->continuationIndex]=nullptr; }
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
        if (!g->bridge()) return nullptr;
        g->owner = owner; g->source = source;
        it = groups().emplace(source, std::move(g)).first;
    }
    return it->second->add(destination);
}
} // namespace hymission::hooks
