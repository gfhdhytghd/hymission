# Stage edge framebuffer crash (2026-10-10)

## Evidence

Three supplied Hyprland crash reports (PIDs 9068, 1946816 and 1962827)
record SIGSEGV at `CGLFramebuffer::internalAlloc + 0xb8`, reached through
`StageEdgeEffect::bufferFor`. The installed Hyprland is 0.56.2,
`efb50993780079460b0cbed1363e2166a2de1d9f`.

Disassembly of the installed `/usr/bin/Hyprland` places that offset at
`mov 0x8(%rax),%edx`, reading the pixel-format record returned by
`Hyprgraphics::Egl::getPixelFormatFromDRM`. There is no null check. The
installed `NFormatUtils::alphaFormat` only handles eight opaque 8/10-bit
formats and returns zero otherwise. Hymission passed its result directly
to the allocator, including when the destination already had alpha. This
produces an invalid format for ABGR8888, ABGR2101010 and ABGR16161616F.

The incident followed a DCI-P3 change. Output XBGR2101010 does not establish
the intermediate render-buffer format. Local Hyprland source (a different
revision from the installed binary) uses ABGR16161616F for FP16 working
buffers and can select FP16 automatically for non-sRGB output. This is a
plausible trigger for the confirmed invalid-format path, not a measurement
of the crash-time format: the supplied reports do not contain that argument.
The fault precedes stencil allocation; the reports do not establish a
stencil or EGL-context fault.

## Fix

Preserve known alpha formats and explicitly convert supported opaque
formats. Validate both color and mirror formats. Stage's two optional
transient layers now use a fallible GL allocator instead of the compositor
allocator, whose incomplete-framebuffer assertion cannot be caught by
checking its boolean return value. Validate dimensions/context, check GL
allocation errors and framebuffer completeness, and use the existing hard
clip on failure. Keep stencil support, color precision and image
descriptions; rebuild the attachment set together for size/format/mirror
changes. Do not switch EGL contexts during drawing.

## Validation and limits

CMake Release plugin build and all five CTest tests passed against the
installed 0.56.2 headers. The surfaceless EGL regression exercises the same
allocation helper used by the plugin: 8-bit, 10-bit, FP16, mixed-format
mirror attachments, resize, mirror removal, invalid main/mirror formats,
missing context, zero/oversize dimensions, and an intentionally incomplete
attachment set followed by recovery. FP16 readback retains a red value of
2.0. Existing edge shader/render tests also pass. Meson configuration, edge-test
build and the edge render test also passed.

This is offline software-GL validation, not acceptance on the LG output or
under live Hyprland. No plugin update/load/unload, compositor restart/reload,
or color configuration change was performed. DCI-P3 remains disabled.
