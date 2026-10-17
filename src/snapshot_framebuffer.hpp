#pragma once

#include <hyprland/src/render/Framebuffer.hpp>

namespace hymission {
// Copies and convolution preserve the source encoding. In particular, an HDR
// snapshot is already in the monitor's linear FP16 work space, not client sRGB.
// Set the description AFTER alloc(), which may replace the backing texture.
inline bool allocateSnapshotFramebufferLike(Render::IFramebuffer& target, Render::IFramebuffer& source, int width, int height) {
    if (!target.alloc(width, height, source.m_drmFormat))
        return false;
    target.setImageDescription(source.imageDescription());
    return true;
}
} // namespace hymission
