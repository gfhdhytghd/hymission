#pragma once

#include <hyprland/src/render/Renderer.hpp>

namespace hymission {
inline thread_local Render::CRenderContext* currentRenderContext = nullptr;

inline Render::CRenderContext& renderContext() {
    return currentRenderContext ? *currentRenderContext : g_pHyprRenderer->context();
}

class RenderContextScope {
  public:
    explicit RenderContextScope(Render::CRenderContext& ctx) : m_previous(currentRenderContext) { currentRenderContext = &ctx; }
    ~RenderContextScope() { currentRenderContext = m_previous; }
  private:
    Render::CRenderContext* m_previous;
};
} // namespace hymission
