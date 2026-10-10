// Offline ABI smoke test, preloaded ONLY into `Hyprland --version`. It never
// initializes a compositor, EGL context, plugin loader, or display connection.
#include "stage_edge_native_framebuffer.hpp"
#include <cstdio>
#include <cstdlib>

__attribute__((constructor)) static void checkNativeFramebufferContract() {
    if (!std::getenv("HYMISSION_NATIVE_FRAMEBUFFER_PROBE"))
        return;
    hymission::stage::EdgeFramebuffer framebuffer(0);
    Render::IFramebuffer* interface = &framebuffer;
    auto* native = dynamic_cast<Render::GL::CGLFramebuffer*>(interface);
    if (!native || native->getFBID() != 0 || interface->isAllocated()) {
        std::fputs("FAIL: Stage framebuffer does not satisfy native GL contract\n", stderr);
        std::exit(1);
    }
    // Calls the installed compositor's exact method from the crash stack.
    // Unallocated storage needs no GL context, but requires a valid native this.
    native->invalidate({GL_STENCIL_ATTACHMENT});
    interface->release();
    std::fputs("PASS: Stage native GL cast, getFBID, invalidate and release\n", stderr);
}
