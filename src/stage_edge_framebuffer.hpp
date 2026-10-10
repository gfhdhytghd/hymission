#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <hyprgraphics/egl/Egl.hpp>

namespace hymission::stage {
// alphaFormat() in Hyprland only converts opaque formats and returns INVALID
// for formats that already have alpha (including the CM FP16 intermediate).
inline uint32_t edgeAlphaFormat(uint32_t format) {
    const auto* info = Hyprgraphics::Egl::getPixelFormatFromDRM(format);
    if (!info)
        return DRM_FORMAT_INVALID;
    if (info->withAlpha)
        return format;
    switch (format) {
        case DRM_FORMAT_XRGB8888: return DRM_FORMAT_ARGB8888;
        case DRM_FORMAT_XBGR8888: return DRM_FORMAT_ABGR8888;
        case DRM_FORMAT_RGBX8888: return DRM_FORMAT_RGBA8888;
        case DRM_FORMAT_BGRX8888: return DRM_FORMAT_BGRA8888;
        case DRM_FORMAT_XRGB2101010: return DRM_FORMAT_ARGB2101010;
        case DRM_FORMAT_XBGR2101010: return DRM_FORMAT_ABGR2101010;
        case DRM_FORMAT_RGBX1010102: return DRM_FORMAT_RGBA1010102;
        case DRM_FORMAT_BGRX1010102: return DRM_FORMAT_BGRA1010102;
        case DRM_FORMAT_XBGR16161616F: return DRM_FORMAT_ABGR16161616F;
        case DRM_FORMAT_XRGB16161616F: return DRM_FORMAT_ARGB16161616F;
        default: return DRM_FORMAT_INVALID;
    }
}

// Caller owns the objects and restores GL bindings. Never call the compositor's
// allocator here: it dereferences unknown formats and asserts on incomplete FBOs.
inline bool allocateEdgeFramebuffer(GLuint fb, GLuint color, GLuint stencil, GLuint mirror,
                                    int width, int height, uint32_t format, uint32_t mirrorFormat) {
    if (eglGetCurrentContext() == EGL_NO_CONTEXT || !fb || !color || !stencil || width <= 0 || height <= 0)
        return false;
    const auto* info = Hyprgraphics::Egl::getPixelFormatFromDRM(format);
    const auto* mirrorInfo = mirror ? Hyprgraphics::Egl::getPixelFormatFromDRM(mirrorFormat) : nullptr;
    if (!info || !info->withAlpha || (mirror && (!mirrorInfo || !mirrorInfo->withAlpha)))
        return false;
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (width > maxSize || height > maxSize)
        return false;
    // Do not attribute an earlier draw's GL error to this allocation.
    while (glGetError() != GL_NO_ERROR) {}
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    const auto allocate = [&](GLuint texture, GLenum attachment, GLint internal, GLenum external, GLenum type) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, external, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, 0);
        return glGetError() == GL_NO_ERROR;
    };
    const auto allocateColor = [&](GLuint texture, GLenum attachment, const Hyprgraphics::Egl::SPixelFormat* f) {
        return allocate(texture, attachment, f->glInternalFormat ? f->glInternalFormat : f->glFormat, f->glFormat, f->glType);
    };
    if (!allocateColor(color, GL_COLOR_ATTACHMENT0, info) ||
        !allocate(stencil, GL_DEPTH_STENCIL_ATTACHMENT, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8) ||
        (mirror && !allocateColor(mirror, GL_COLOR_ATTACHMENT1, mirrorInfo)))
        return false;
    if (!mirror)
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    const GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(mirror ? 2 : 1, attachments);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
}
} // namespace hymission::stage
