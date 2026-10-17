// Preload only into the matching Hyprland --version. Use its real framebuffer
// bookkeeping and CM descriptions with private, surfaceless GL storage.
// No compositor, plugin loader, Wayland connection or live output is touched.
#include "snapshot_framebuffer.hpp"
#include <hyprland/src/protocols/types/Buffer.hpp>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <hyprgraphics/egl/Egl.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

class TestTexture final : public Render::ITexture {
  public:
    TestTexture() { glGenTextures(1, &m_texID); }
    ~TestTexture() override { glDeleteTextures(1, &m_texID); }
    void setTexParameter(GLenum name, GLint value) override { bind(); glTexParameteri(GL_TEXTURE_2D, name, value); }
    void allocate(const Vector2D& size, uint32_t format) override {
        const auto* info = Hyprgraphics::Egl::getPixelFormatFromDRM(format);
        check(info, "known snapshot pixel format");
        bind();
        glTexImage2D(GL_TEXTURE_2D, 0, info->glInternalFormat ? info->glInternalFormat : info->glFormat,
                     size.x, size.y, 0, info->glFormat, info->glType, nullptr);
        setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    void update(uint32_t, uint8_t*, uint32_t, const CRegion&) override {}
    void bind() override { glBindTexture(GL_TEXTURE_2D, m_texID); }
};

class TestFramebuffer final : public Render::IFramebuffer {
  public:
    bool failAllocation = false;
    ~TestFramebuffer() override { release(); }
    void bind() override { glBindFramebuffer(GL_FRAMEBUFFER, fb); }
    bool readPixels(CHLBufferReference, uint32_t, uint32_t, uint32_t, uint32_t) override { return false; }
    void addStencil(SP<Render::ITexture>) override {}
    void release() override {
        glDeleteFramebuffers(1, &fb);
        fb = 0;
        m_tex.reset();
        m_fbAllocated = false;
    }
    void blitTo(TestFramebuffer& target) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.fb);
        glBlitFramebuffer(0, 0, m_size.x, m_size.y, 0, 0, target.m_size.x, target.m_size.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    std::array<float, 4> pixel() {
        bind();
        std::array<float, 4> result{};
        if (m_drmFormat == DRM_FORMAT_ABGR16161616F)
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, result.data());
        else {
            std::array<unsigned char, 4> bytes{};
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
            for (size_t i = 0; i < 4; ++i)
                result[i] = bytes[i] / 255.F;
        }
        check(glGetError() == GL_NO_ERROR, "GL blit and readback");
        return result;
    }
  protected:
    bool internalAlloc(int w, int h, uint32_t format) override {
        if (failAllocation)
            return false;
        if (!fb)
            glGenFramebuffers(1, &fb);
        m_tex = makeShared<TestTexture>();
        m_tex->allocate({w, h}, format);
        bind();
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex->m_texID, 0);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
  private:
    GLuint fb = 0;
};

void runTests() {
    using namespace NColorManagement;
    // A preload constructor can precede the binary's inline global constants.
    const auto sdr = CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_SRGB, .primariesNameSet = true,
        .primariesNamed = CM_PRIMARIES_SRGB, .primaries = NColorPrimaries::BT709});
    const auto hdrWork = CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_EXT_LINEAR, .primariesNameSet = true,
        .primariesNamed = CM_PRIMARIES_SRGB, .primaries = NColorPrimaries::BT709,
        .luminances = {.min = 0.005F, .max = 1000, .reference = 173}});
    for (bool hdr : {false, true}) {
        const auto description = hdr ? hdrWork : sdr;
        const uint32_t format = hdr ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_ABGR8888;
        TestFramebuffer source;
        check(source.alloc(8, 4, format), "source allocation");
        source.setImageDescription(description);
        const std::array<float, 4> color = hdr ? std::array<float, 4>{2.5F, 0.5F, -0.125F, 0.75F} :
                                               std::array<float, 4>{0.6F, 0.3F, 0.15F, 0.75F};
        source.bind();
        glClearBufferfv(GL_COLOR, 0, color.data());
        const auto expected = source.pixel();

        // Rotated normalization, crop, padded proxy, H/V blur storage and the
        // final blur level all carry the same encoding, even across resizes.
        TestFramebuffer target;
        for (int stage = 0; stage < 7; ++stage) {
            check(hymission::allocateSnapshotFramebufferLike(target, source, 3 + stage, 2 + stage), "derived allocation");
            check(target.m_drmFormat == format, "preserve source precision");
            check(target.imageDescription() == description && target.getTexture()->m_imageDescription == description,
                  "tag replacement texture after allocation");
            check(!target.getTexture()->m_imageDescription->needsCM(description), "work-space copy must skip repeated CM");
            source.blitTo(target);
            const auto actual = target.pixel();
            for (size_t channel = 0; channel < 4; ++channel)
                check(std::abs(actual[channel] - expected[channel]) < 0.002F, "preserve RGB range and premultiplied alpha");
        }
        if (hdr) {
            // Negative control: the old default allocation clips HDR and has
            // no description, causing renderToFBInternal's sRGB fallback.
            TestFramebuffer old;
            check(old.alloc(8, 4), "old default allocation");
            source.blitTo(old);
            check(!old.getTexture()->m_imageDescription && old.pixel()[0] == 1.F, "reproduce old clipping and lost CM tag");
            check(sdr->needsCM(description), "old fallback triggers another conversion");
        }
        target.failAllocation = true;
        check(!hymission::allocateSnapshotFramebufferLike(target, source, 100, 100), "allocation failure propagates");
    }
}
} // namespace

__attribute__((constructor)) static void snapshotColorTest() {
    if (!std::getenv("HYMISSION_SNAPSHOT_COLOR_TEST"))
        return;
    const auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
        std::exit(77);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    eglChooseConfig(display, attrs, &config, 1, &count);
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const auto context = count ? eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttrs) : EGL_NO_CONTEXT;
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        std::exit(77);
    runTests();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    std::puts("PASS: snapshot HDR range, CM identity, SDR pixels, resize and failure propagation");
}
