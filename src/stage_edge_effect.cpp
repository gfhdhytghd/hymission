#include "stage_edge_effect.hpp"
#include "stage_edge_shader.hpp"

#include <algorithm>
#include <cmath>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

namespace hymission {
namespace {
using Render::GL::g_pHyprOpenGL;

// Guard raw GL work separately from native drawing: native draws also update
// Hyprland's GL state caches. Restore capabilities/scissor through those APIs.
struct GLState {
    GLint program, vao, activeTexture, texture[2], readFB, drawFB, viewport[4], scissor[4];
    GLboolean blend, stencil, scissoring;
    GLState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        for (int i = 0; i < 2; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture[i]);
        }
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        blend = glIsEnabled(GL_BLEND);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        scissoring = glIsEnabled(GL_SCISSOR_TEST);
    }
    ~GLState() {
        glUseProgram(program);
        glBindVertexArray(vao);
        for (int i = 0; i < 2; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, texture[i]);
        }
        glActiveTexture(activeTexture);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFB);
        g_pHyprOpenGL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        g_pHyprOpenGL->blend(blend);
        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, stencil);
        g_pHyprOpenGL->scissor(CBox{scissor[0], scissor[1], scissor[2], scissor[3]}, false);
        if (!scissoring)
            g_pHyprOpenGL->scissor(nullptr);
    }
};

GLuint compile(GLenum type, const char* source, std::string& error) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success)
        return shader;
    char message[1024]{};
    glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
    error = message;
    glDeleteShader(shader);
    return 0;
}
} // namespace

StageEdgeEffect::~StageEdgeEffect() { reset(); }

void StageEdgeEffect::reset() {
    if (g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();
    m_backdrop.reset();
    m_buffers.clear();
    if (m_program)
        glDeleteProgram(m_program);
    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    m_program = m_vao = 0;
    m_error.clear();
}

bool StageEdgeEffect::ensureShader() {
    if (m_program)
        return true;
    if (!m_error.empty())
        return false;
    GLState restore;
    const auto vertex = compile(GL_VERTEX_SHADER, stage::edgeVertexShader, m_error);
    const auto fragment = compile(GL_FRAGMENT_SHADER, stage::edgeFragmentShader, m_error);
    if (vertex && fragment) {
        m_program = glCreateProgram();
        glAttachShader(m_program, vertex);
        glAttachShader(m_program, fragment);
        glLinkProgram(m_program);
        GLint linked = 0;
        glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char message[1024]{};
            glGetProgramInfoLog(m_program, sizeof(message), nullptr, message);
            m_error = message;
            glDeleteProgram(m_program);
            m_program = 0;
        } else
            glGenVertexArrays(1, &m_vao);
    }
    if (vertex)
        glDeleteShader(vertex);
    if (fragment)
        glDeleteShader(fragment);
    if (!m_program) {
        if (m_error.empty())
            m_error = "could not create edge shader";
        Log::logger->log(Log::ERR, "[hymission] Stage edge effect unavailable; retaining hard clip: {}", m_error);
    }
    return m_program != 0;
}

SP<Render::IFramebuffer> StageEdgeEffect::bufferFor(const PHLMONITOR& monitor, const SP<Render::IFramebuffer>& destination, bool horizontal) {
    GLState restore;
    std::erase_if(m_buffers, [](const Buffer& b) { return b.monitor.expired(); });
    auto it = std::ranges::find_if(m_buffers, [&](const Buffer& b) { return b.monitor == monitor; });
    if (it == m_buffers.end()) {
        m_buffers.push_back({monitor, nullptr, nullptr});
        it = std::prev(m_buffers.end());
    }
    // Keep the output's precision and color space, but require an alpha channel.
    auto& fb = horizontal ? it->horizontal : it->framebuffer;
    if (!fb) {
        fb = g_pHyprRenderer->createFB(horizontal ? "hymission horizontal blur" : "hymission scrolling window layer");
        if (fb) {
            auto stencil = g_pHyprRenderer->createTexture();
            glGenTextures(1, &stencil->m_texID);
            fb->addStencil(stencil);
        }
    }
    if (!fb || !fb->alloc(static_cast<int>(monitor->m_pixelSize.x), static_cast<int>(monitor->m_pixelSize.y),
                          NFormatUtils::alphaFormat(destination->m_drmFormat))) {
        if (m_error.empty()) {
            m_error = "could not allocate scrolling window layer";
            Log::logger->log(Log::ERR, "[hymission] {}; retaining hard clip", m_error);
        }
        return nullptr;
    }
    fb->setImageDescription(destination->imageDescription());
    // Preserve the compositor's separate, unmodified capture colors when its
    // work buffer uses a second color attachment (HDR/SDR modifications).
    const auto destinationMirror = destination->getMirrorTexture();
    if (destinationMirror) {
        auto mirror = fb->getMirrorTexture();
        const auto format = NFormatUtils::alphaFormat(destinationMirror->m_drmFormat);
        if (!mirror || mirror->m_size != monitor->m_pixelSize || mirror->m_drmFormat != format) {
            fb->disableMirror();
            mirror = g_pHyprRenderer->createTexture();
            mirror->allocate(monitor->m_pixelSize, format);
            mirror->m_imageDescription = destinationMirror->m_imageDescription;
            fb->enableMirror(mirror);
        }
    } else
        fb->disableMirror();
    fb->bind();
    const GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(destinationMirror ? 2 : 1, attachments);
    return fb;
}

bool StageEdgeEffect::draw(const PHLMONITOR& monitor, const stage::EdgeViewport& viewport, const CBox& clip,
                           const std::function<void(const CBox&)>& drawWindow) {
    auto& state = g_pHyprRenderer->m_renderData;
    const auto& v = viewport.box;
    const CBox view = CBox{v.x - monitor->m_position.x, v.y - monitor->m_position.y, v.width, v.height}.scale(monitor->m_scale);
    auto hardClip = clip.intersection(view).intersection(CBox{{}, monitor->m_transformedSize});
    // Native scissors truncate floats. Round inwards so fractional output
    // scales cannot expose even a partial pixel outside the logical viewport.
    const auto hardEnd = Vector2D{std::floor(hardClip.x + hardClip.w), std::floor(hardClip.y + hardClip.h)};
    hardClip.x = std::ceil(hardClip.x);
    hardClip.y = std::ceil(hardClip.y);
    hardClip.w = std::max(0.0, hardEnd.x - hardClip.x);
    hardClip.h = std::max(0.0, hardEnd.y - hardClip.y);
    CRegion outputDamage = state.damage.copy().intersect(hardClip);
    if (hardClip.empty() || outputDamage.empty())
        return false;
    const auto previousDamage = state.damage;
    const auto previousClip = state.clipBox;
    Hyprutils::Utils::CScopeGuard restore{[&] { state.damage = previousDamage; state.clipBox = previousClip; }};
    const double left = viewport.left * monitor->m_scale;
    const double right = viewport.right * monitor->m_scale;
    const double top = viewport.top * monitor->m_scale;
    const double bottom = viewport.bottom * monitor->m_scale;
    const CBox affected = outputDamage.getExtents();
    const bool soft = (left > 0 && affected.x < view.x + left) || (right > 0 && affected.x + affected.w > view.x + view.w - right) ||
        (top > 0 && affected.y < view.y + top) || (bottom > 0 && affected.y + affected.h > view.y + view.h - bottom);
    const auto destination = state.currentFB;
    const auto fb = soft && destination && ensureShader() ? bufferFor(monitor, destination) : nullptr;
    const auto horizontal = fb ? bufferFor(monitor, destination, true) : nullptr;
    if (!fb || !horizontal) {
        // Some native texture paths use the clip rectangle instead of damage.
        // Keep disjoint damage disjoint, or transparent windows would be drawn
        // over their previous pixels in the undamaged gaps between rectangles.
        outputDamage.forEachRect([&](const auto& r) {
            const CBox drawClip{r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1};
            state.clipBox = drawClip;
            state.damage = CRegion{drawClip};
            drawWindow(drawClip);
        });
        return true;
    }
    // The outer native pass expands damage for live blur. Stay inside that
    // expansion even when system blur size/passes are configured very small.
    static auto blurSize = CConfigValue<Config::INTEGER>("decoration:blur:size");
    static auto blurPasses = CConfigValue<Config::INTEGER>("decoration:blur:passes");
    const double nativeSupport = std::clamp(*blurSize, int64_t{1}, int64_t{40}) * std::pow(2, std::clamp(*blurPasses, int64_t{1}, int64_t{8}));
    const double maxRadius = std::min({12.0 * monitor->m_scale, nativeSupport - 1, 32.0});
    const double support = std::ceil(std::min(maxRadius, std::max({left, right, top, bottom}) / 2)) + 1;
    const CBox sample = CBox{affected.x - support, affected.y - support, affected.w + support * 2, affected.h + support * 2}.intersection(hardClip);
    {
        GLState restoreGL;
        fb->bind();
        g_pHyprOpenGL->scissor(sample, true);
        const GLfloat transparent[4] = {0, 0, 0, 0};
        glClearBufferfv(GL_COLOR, 0, transparent);
        if (fb->getMirrorTexture())
            glClearBufferfv(GL_COLOR, 1, transparent);
    }
    {
        // The blurMainFramebuffer hook reads this backdrop instead of blurring
        // the transparent scratch layer. Native UV/feedback/blur stay intact.
        const auto previousBackdrop = m_backdrop;
        m_backdrop = destination;
        g_pHyprRenderer->bindFB(fb);
        Hyprutils::Utils::CScopeGuard restoreFB{[&] {
            m_backdrop = previousBackdrop;
            g_pHyprRenderer->bindFB(destination);
        }};
        state.damage = CRegion{sample};
        state.clipBox = sample;
        drawWindow(sample);
    }
    GLState restoreGL;
    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);
    fb->getTexture()->bind();
    const auto mirror = fb->getMirrorTexture();
    if (mirror) {
        glActiveTexture(GL_TEXTURE1);
        mirror->bind();
    }
    const auto uniform = [&](const char* name) { return glGetUniformLocation(m_program, name); };
    glUniform1i(uniform("uTexture"), 0);
    glUniform1i(uniform("uMirror"), 1);
    glUniform1i(uniform("uHasMirror"), mirror ? 1 : 0);
    glUniform2f(uniform("uSize"), monitor->m_pixelSize.x, monitor->m_pixelSize.y);
    const auto transform = Math::wlTransformToHyprutils(monitor->m_transform);
    // CBox's dimensions are the input extent, matching native damage/scissor
    // transforms. Vector2D::transform uses a different size convention.
    const auto map = [&](double x, double y) {
        return CBox{x, y, 0, 0}.transform(transform, monitor->m_pixelSize.x, monitor->m_pixelSize.y).pos();
    };
    const auto origin = map(0, 0);
    const auto x = map(1, 0) - origin;
    const auto y = map(0, 1) - origin;
    glUniform3f(uniform("uAxisX"), x.x, y.x, origin.x);
    glUniform3f(uniform("uAxisY"), x.y, y.y, origin.y);
    glUniform4f(uniform("uViewport"), view.x, view.y, view.w, view.h);
    glUniform4f(uniform("uSample"), sample.x, sample.y, sample.w, sample.h);
    auto sampleFB = sample.copy().transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)),
        monitor->m_transformedSize.x, monitor->m_transformedSize.y);
    const auto end = Vector2D{std::floor(sampleFB.x + sampleFB.w), std::floor(sampleFB.y + sampleFB.h)};
    sampleFB.x = std::ceil(sampleFB.x);
    sampleFB.y = std::ceil(sampleFB.y);
    sampleFB.w = std::max(1.0, end.x - sampleFB.x);
    sampleFB.h = std::max(1.0, end.y - sampleFB.y);
    glUniform4f(uniform("uSampleFB"), sampleFB.x, sampleFB.y, sampleFB.w, sampleFB.h);
    glUniform4f(uniform("uWidths"), left, right, top, bottom);
    glUniform1f(uniform("uMaxRadius"), maxRadius);
    // First pass fills the padded sample rectangle without opacity. The
    // second samples only that fresh rectangle and composites over the output.
    g_pHyprRenderer->bindFB(horizontal);
    g_pHyprOpenGL->blend(false);
    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
    glUniform1i(uniform("uPass"), 0);
    g_pHyprOpenGL->scissor(sample, true);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g_pHyprRenderer->bindFB(destination);
    glActiveTexture(GL_TEXTURE0);
    horizontal->getTexture()->bind();
    if (mirror) {
        glActiveTexture(GL_TEXTURE1);
        horizontal->getMirrorTexture()->bind();
    }
    glUniform1i(uniform("uPass"), 1);
    g_pHyprOpenGL->blend(true);
    outputDamage.forEachRect([&](const auto& rect) {
        g_pHyprOpenGL->scissor(&rect, true);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });
    return true;
}
} // namespace hymission
