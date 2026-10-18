#pragma once

#include "stage_logic.hpp"
#include <functional>
#include <string>
#include <vector>
#include <hyprland/src/defines.hpp>
#include <hyprland/src/render/Framebuffer.hpp>

namespace hymission {
// A transient native window layer, never a cached client snapshot. Rendering
// stays in the current compositor pass; no begin/end or client state changes.
class StageEdgeEffect {
  public:
    struct Metrics { uint64_t calls=0,softCalls=0,aliasCalls=0,softPixels=0; };
    const Metrics& metrics() const { return m_metrics; }
    ~StageEdgeEffect();
    bool draw(const PHLMONITOR& monitor, const stage::EdgeViewport& viewport, const CBox& clip,
              const std::function<void(const CBox&)>& drawWindow);
    SP<Render::IFramebuffer> backdrop() const { return m_backdrop; }
    void reset();
    const std::string& error() const { return m_error; }
  private:
    Metrics m_metrics;
    struct Buffer {
        PHLMONITORREF monitor;
        SP<Render::IFramebuffer> framebuffer;
        SP<Render::IFramebuffer> horizontal;
    };
    std::vector<Buffer> m_buffers;
    SP<Render::IFramebuffer> m_backdrop;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    struct Uniforms {
        GLint texture=-1,mirror=-1,hasMirror=-1,size=-1,axisX=-1,axisY=-1,viewport=-1,
            sample=-1,sampleFB=-1,widths=-1,maxRadius=-1,pass=-1;
    } m_uniforms;
    std::string m_error;
    bool ensureShader();
    SP<Render::IFramebuffer> bufferFor(const PHLMONITOR& monitor, const SP<Render::IFramebuffer>& destination, bool horizontal = false);
};
} // namespace hymission
