#pragma once

#include "stage_edge_framebuffer.hpp"
#include <hyprland/src/render/Framebuffer.hpp>
// Native blur downcasts currentFB to CGLFramebuffer and uses its nonvirtual
// invalidate/getFBID methods. Own the actual native handle, not a second handle
// in an IFramebuffer implementation. Only expose this header's private members;
// its dependencies are already included above (same convention as Stage hooks).
#define private public
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#undef private
#include <hyprland/src/render/Renderer.hpp>
#include <type_traits>

namespace hymission::stage {
class EdgeFramebuffer final : public Render::GL::CGLFramebuffer {
  public:
    explicit EdgeFramebuffer(uint32_t mirrorFormat) : CGLFramebuffer("hymission edge layer"), m_mirrorFormat(mirrorFormat) {}
  protected:
    bool internalAlloc(int w, int h, uint32_t format) override {
        if (!m_tex)
            m_tex = g_pHyprRenderer->createTexture();
        if (!m_stencilTex)
            m_stencilTex = g_pHyprRenderer->createTexture();
        if (m_mirrorFormat && !m_mirrorTex)
            m_mirrorTex = g_pHyprRenderer->createTexture();
        m_tex->allocate({w, h}, format);
        m_stencilTex->allocate({w, h});
        if (m_mirrorTex)
            m_mirrorTex->allocate({w, h}, m_mirrorFormat);
        if (m_fb == GLuint(-1) || m_fb == 0)
            glGenFramebuffers(1, &m_fb);
        if (!allocateEdgeFramebuffer(m_fb, m_tex->m_texID, m_stencilTex->m_texID,
                m_mirrorTex ? m_mirrorTex->m_texID : 0, w, h, format, m_mirrorFormat)) {
            // Native release only deletes an allocated framebuffer. A failed
            // allocation still owns a GL name, so dispose of it here.
            glDeleteFramebuffers(1, &m_fb);
            m_fb = 0;
            m_tex.reset();
            m_stencilTex.reset();
            m_mirrorTex.reset();
            return false;
        }
        m_cleared = false;
        return true;
    }
  private:
    const uint32_t m_mirrorFormat;
};
// Native window blur requires this concrete interface, not just IFramebuffer.
static_assert(std::is_base_of_v<Render::GL::CGLFramebuffer, EdgeFramebuffer>);
} // namespace hymission::stage
