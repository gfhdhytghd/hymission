#pragma once

#include <hyprland/src/render/decorations/AnimatedDecorationGradient.hpp>

#include <optional>
#include <type_traits>

namespace hymission {
// Native decorations own their gradients now. Sample their public render state
// during a dry draw/query rather than duplicating focus, group and rule colors.
inline thread_local std::optional<CAnimatedDecorationGradient::SRenderState>* sampledDecorationGradient = nullptr;
using DecorationGradientFn = CAnimatedDecorationGradient::SRenderState (*)(const CAnimatedDecorationGradient*);
inline DecorationGradientFn decorationGradientOriginal = nullptr;

inline CAnimatedDecorationGradient::SRenderState hookDecorationGradient(const CAnimatedDecorationGradient* gradient) {
    auto state = decorationGradientOriginal(gradient);
    if (sampledDecorationGradient)
        *sampledDecorationGradient = state;
    return state;
}

static_assert(std::is_same_v<DecorationGradientFn, decltype(&hookDecorationGradient)>);

class DecorationGradientScope {
  public:
    explicit DecorationGradientScope(std::optional<CAnimatedDecorationGradient::SRenderState>& state) : m_previous(sampledDecorationGradient) {
        sampledDecorationGradient = &state;
    }
    ~DecorationGradientScope() { sampledDecorationGradient = m_previous; }
  private:
    std::optional<CAnimatedDecorationGradient::SRenderState>* m_previous;
};
} // namespace hymission
