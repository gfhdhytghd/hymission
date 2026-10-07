#include "stage_edge_shader.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <hyprutils/math/Vector2D.hpp>
#include <hyprutils/math/Box.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
constexpr int W = 128, H = 96;
GLuint compile(GLenum type, const char* source) {
    const auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[2048]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << log << '\n';
        return 0;
    }
    return shader;
}
}

int main() {
    const auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        std::cout << "SKIP: surfaceless EGL unavailable\n";
        return 77;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config{};
    EGLint configs = 0;
    eglChooseConfig(display, configAttrs, &config, 1, &configs);
    const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = configs ? eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs) : EGL_NO_CONTEXT;
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        std::cout << "SKIP: GLES 3 context unavailable\n";
        eglTerminate(display);
        return 77;
    }
    const auto vertex = compile(GL_VERTEX_SHADER, hymission::stage::edgeVertexShader);
    const auto fragment = compile(GL_FRAGMENT_SHADER, hymission::stage::edgeFragmentShader);
    if (!vertex || !fragment)
        return 1;
    const auto program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
        return 1;
    glUseProgram(program);
    GLuint vao, textures[6], framebuffer;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(6, textures);
    for (auto texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, textures[3], 0);
    const GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, attachments);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    glViewport(0, 0, W, H);
    glDisable(GL_BLEND);
    const auto location = [&](const char* name) { return glGetUniformLocation(program, name); };
    glUniform1i(location("uTexture"), 0);
    glUniform1i(location("uMirror"), 1);
    glUniform2f(location("uSize"), W, H);
    using Hyprutils::Math::Vector2D;
    std::vector<unsigned char> source(W * H * 4), pixels(source.size());
    std::vector<unsigned char> mirrorSource(source.size()), mirrorPixels(source.size()), blended(source.size());
    bool ok = true;
    int cases = 0;
    int antiAliasSamples = 0;
    int boundarySamples = 0;
    for (int transform = 0; transform < 8; ++transform) {
      for (const float fraction : {0.F, 0.25F, 0.75F}) {
        const auto tr = static_cast<Hyprutils::Math::eTransform>(transform);
        const auto map = [&](const Vector2D& p) { return Hyprutils::Math::CBox{p.x, p.y, 0, 0}.transform(tr, W, H).pos(); };
        const auto origin = map({0, 0});
        const auto dx = map({1, 0}) - origin;
        const auto dy = map({0, 1}) - origin;
        const bool rotated = transform % 2;
        const float viewX = 16 + fraction, viewY = 12 + fraction, viewW = (rotated ? H : W) - 32, viewH = (rotated ? W : H) - 24;
        const auto inside = [&](const Vector2D& m) { return m.x >= viewX && m.x < viewX + viewW && m.y >= viewY && m.y < viewY + viewH; };
        int minX = W, minY = H, maxX = 0, maxY = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (inside(map({x + 0.5, y + 0.5}))) {
                    minX = std::min(minX, x); minY = std::min(minY, y);
                    maxX = std::max(maxX, x + 1); maxY = std::max(maxY, y + 1);
                }
        glUniform3f(location("uAxisX"), dx.x, dy.x, origin.x);
        glUniform3f(location("uAxisY"), dx.y, dy.y, origin.y);
        glUniform4f(location("uViewport"), viewX, viewY, viewW, viewH);
        glUniform4f(location("uSample"), viewX, viewY, viewW, viewH);
        glUniform4f(location("uSampleFB"), minX, minY, maxX - minX, maxY - minY);
        for (const float scale : {1.F, 1.5F, 2.F}) {
            glUniform1f(location("uMaxRadius"), 12 * scale);
            for (int mode = 0; mode < 4; ++mode) {
                const float left = mode == 1 || mode == 3 ? 16 * scale : 0;
                const float right = mode == 2 || mode == 3 ? 16 * scale : 0;
                glUniform2f(location("uWidths"), left, right);
                for (int pattern = 0; pattern < 3; ++pattern) {
                    const bool hasMirror = pattern == 1;
                    glUniform1i(location("uHasMirror"), hasMirror);
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const auto m = map({x + 0.5, y + 0.5});
                            const int i = (y * W + x) * 4;
                            const int alpha = pattern == 1 ? 128 : 255;
                            const bool bright = pattern != 2 || (static_cast<int>(m.x) + static_cast<int>(m.y)) % 2;
                            source[i] = inside(m) ? 0 : 255; // poison every out-of-viewport sample bright red
                            source[i + 1] = inside(m) && bright ? alpha : 0;
                            source[i + 2] = 0;
                            source[i + 3] = inside(m) ? alpha : 255;
                        }
                    mirrorSource = source;
                    for (std::size_t i = 0; i < source.size(); i += 4)
                        std::swap(mirrorSource[i + 1], mirrorSource[i + 2]);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, textures[2]);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, mirrorSource.data());
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, textures[0]);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, source.data());
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[4], 0);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, textures[5], 0);
                    glUniform1i(location("uPass"), 0);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, textures[3], 0);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, textures[4]);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, textures[5]);
                    glUniform1i(location("uPass"), 1);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                    glReadBuffer(GL_COLOR_ATTACHMENT1);
                    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, mirrorPixels.data());
                    glReadBuffer(GL_COLOR_ATTACHMENT0);
                    // Composite over an actual opaque backdrop: fading must
                    // reveal it, with no dark fringe or doubled window alpha.
                    const GLfloat background[] = {0, 0, 1, 1};
                    glClearBufferfv(GL_COLOR, 0, background);
                    glClearBufferfv(GL_COLOR, 1, background);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, blended.data());
                    glDisable(GL_BLEND);
                    bool clipped = true, clean = true, opaqueCenter = true, mirrored = true, composited = true, noGrid = true, continuousEdge = true;
                    int fading = 0, blurred = 0;
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const auto m = map({x + 0.5, y + 0.5});
                            const int i = (y * W + x) * 4;
                            const int alpha = pattern == 1 ? 128 : 255;
                            mirrored &= mirrorPixels[i] == pixels[i] && mirrorPixels[i + 3] == pixels[i + 3] &&
                                mirrorPixels[i + (hasMirror ? 2 : 1)] == pixels[i + 1] && mirrorPixels[i + (hasMirror ? 1 : 2)] == 0;
                            composited &= blended[i] == 0 && std::abs(blended[i + 1] - pixels[i + 1]) <= 1 &&
                                std::abs(blended[i + 2] - (255 - pixels[i + 3])) <= 1 && blended[i + 3] == 255;
                            if (!inside(m))
                                clipped &= pixels[i] == 0 && pixels[i + 1] == 0 && pixels[i + 2] == 0 && pixels[i + 3] == 0;
                            else {
                                clean &= pixels[i] == 0 && pixels[i + 1] <= pixels[i + 3] && pixels[i + 3] <= alpha;
                                // The first interior pixel must approach transparency,
                                // not jump from zero outside to a half-opaque hard line.
                                if ((left && m.x - viewX <= 0.75) || (right && viewX + viewW - m.x <= 0.75)) {
                                    ++boundarySamples;
                                    continuousEdge &= pixels[i + 3] <= 8;
                                }
                                const bool edge = (left && m.x < viewX + left) || (right && m.x > viewX + viewW - right);
                                if (pattern == 2 && edge) {
                                    const float distance = left && m.x < viewX + left ? m.x - viewX : viewX + viewW - m.x;
                                    const float width = left && m.x < viewX + left ? left : right;
                                    const float t = std::clamp(distance / width, 0.F, 1.F);
                                    const float radius = std::min(12 * scale, width * 0.5F) * (1 - t * t * (3 - 2 * t));
                                    if (radius >= 5 && distance > radius + 2 && m.y > viewY + radius + 2 && m.y < viewY + viewH - radius - 2) {
                                        ++antiAliasSamples;
                                        noGrid &= std::abs(2 * pixels[i + 1] - pixels[i + 3]) <= 8;
                                    }
                                }
                                if (!edge)
                                    opaqueCenter &= pixels[i + 3] == alpha && pixels[i + 1] == source[i + 1];
                                else {
                                    fading += pixels[i + 3] > 0 && pixels[i + 3] < alpha;
                                    blurred += pixels[i + 3] > 10 && pixels[i + 1] > pixels[i + 3] / 5 && pixels[i + 1] < pixels[i + 3] * 4 / 5;
                                }
                            }
                        }
                    const bool passed = continuousEdge && noGrid && clipped && clean && opaqueCenter && mirrored && composited && (mode == 0 || fading > 0) && (pattern != 2 || mode == 0 || blurred > 0);
                    if (!passed)
                        std::cerr << "FAIL transform=" << transform << " scale=" << scale << " mode=" << mode << " pattern=" << pattern
                            << " fraction=" << fraction << " clip=" << clipped << " clean=" << clean << " center=" << opaqueCenter
                            << " continuousEdge=" << continuousEdge << " noGrid=" << noGrid << " mirror=" << mirrored << " composite=" << composited << " fade=" << fading << " blur=" << blurred << '\n';
                    ok &= passed;
                    ++cases;
                }
            }
        }
      }
    }
    ok &= glGetError() == GL_NO_ERROR && antiAliasSamples > 0 && boundarySamples > 0;
    std::cout << cases << " GPU cases; anti-alias samples: " << antiAliasSamples << "; renderer: " << glGetString(GL_RENDERER) << '\n';
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(6, textures);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return ok ? 0 : 1;
}
