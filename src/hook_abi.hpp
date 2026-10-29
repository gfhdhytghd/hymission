#pragma once

#include <type_traits>

namespace hymission::hook_abi {

// The member function a hook thunk `R (*)(This, Args...)` replaces.
template <class Fn, class Class>
struct Member;

template <class R, class This, class... Args, class Class>
struct Member<R (*)(This, Args...), Class> {
    using Type = std::conditional_t<std::is_const_v<std::remove_pointer_t<This>>, R (Class::*)(Args...) const, R (Class::*)(Args...)>;
};

} // namespace hymission::hook_abi

// Hooks resolve the native function by name or signature string, so the thunk
// type itself is never compared with the native member. Selecting the native
// member through the thunk's member type fails to compile when a parameter is
// added or changed. `Access` may be a subclass that re-exposes a protected member.
#define HYMISSION_NATIVE_HOOK_ABI(Fn, Class, Access, method)                                                                                       \
    static_assert(std::is_member_function_pointer_v<decltype(static_cast<typename ::hymission::hook_abi::Member<Fn, Class>::Type>(&Access::method))>, \
                  #Class "::" #method " no longer matches its hook thunk")
