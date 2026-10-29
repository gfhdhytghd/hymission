#pragma once

#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>

namespace hymission {
inline thread_local Render::CRenderContext* currentRenderContext = nullptr;

inline Render::CRenderContext& renderContext() {
    return currentRenderContext ? *currentRenderContext : g_pHyprRenderer->context();
}

// GLES2 contexts (the fork's legacy renderer) have no GLES3 shaders, vertex
// array objects or framebuffer blits. Plugin passes that need them must fall
// back before touching GL state.
inline bool legacyGLES2Renderer() {
    return Render::GL::g_pHyprOpenGL && Render::GL::g_pHyprOpenGL->m_eglContextVersion == Render::GL::CHyprOpenGLImpl::EGL_CONTEXT_GLES_2_0;
}

class RenderContextScope {
  public:
    explicit RenderContextScope(Render::CRenderContext& ctx) : m_previous(currentRenderContext) { currentRenderContext = &ctx; }
    ~RenderContextScope() { currentRenderContext = m_previous; }
  private:
    Render::CRenderContext* m_previous;
};
} // namespace hymission
