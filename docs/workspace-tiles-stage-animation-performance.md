# Stage animation performance investigation (2026-10-17)

## Scope and measurement

- No host reload, input injection, hyprpm update, plugin load/unload, or push. All rendering/input tests ran in private nested compositors.
- Task baseline: `4a439a521d1ec463712889528f755a46009b8168`. Pre-fix local HEAD: `54a7adf03c62b5eb9f600a9cc37ef62b7db551f1` (includes the eleven local follow-ups). Read-only hyprpm cache inspection found 54a7adf, rather than the older hash stated in the task; cache metadata is not proof of which artifact was active when the user observed stutter.
- Release compositor: `~/.local/lib/hyprland-hdr/Hyprland`, e5a03b6a5ce64ec4c85954649bad4df083627574, matching hyprpm headers. All four performance plugins use the same RelWithDebInfo `-O2` configuration. The ordinary Debug build is excluded from timing comparisons.
- Renderer: NVIDIA RTX PRO 6000 Blackwell Workstation Edition. Private output 6144x3456 @60 Hz, scale 2, top reservation 58; scrolling columns 0.85, six persistent workspaces, three real Wayland clients each. Four open/close cycles, 500 ms each. Separate compositor per off/single/two/three case, tiles installed before Stage; no concurrent builds or GPU tests during these timing runs.
- Forced native `render:use_fp16=1`, `render:keep_unmodified_copy=1`. Every phase asserts an actual ABGR16161616F framebuffer (`0x48344241`) plus native mirror attachment (XRGB8888 on this nested SDR output). This exercises FP16/MRT bandwidth, but does **not** establish physical HDR/PQ, 10-bit mirror behavior, or user-session acceptance.
- CPU render time is RENDER_PRE through RENDER_POST. CPU work below also includes timed native output-layout calls made outside that interval, without double counting calls inside it. This is not a whole-event-loop/presentation-latency measurement. GPU time is asynchronous EXT_disjoint_timer_query elapsed time from RENDER_BEGIN through RENDER_POST; all queries resolved. glFinish is only used after each phase to drain results, outside its sampled frames. GPU elapsed time can include scheduling/dependency waits, and is not GPU busy utilization. Interval is consecutive RENDER_PRE timestamps, excluding the first frame of each phase.
- GL counters are a private child-only LD_PRELOAD shim: uniform lookups, glGetIntegerv, allocations, glClearBufferfv attachment clears/scissored pixel areas, glDrawArrays and program links. They are not a count of every possible native GPU operation. Instrumentation is identical between builds. The fixture spy hooks are installed only when profiling starts.
- `origin/master` targets another stable ABI; it is not a valid loadable same-ABI reference. The pre-tiles development reference is 552bd339fe4809e0a031d277ca97f4dac6d0a5da. Default-off interaction observations match that reference exactly; its timings are included below.

## Findings and fixes

1. **Hook-order-dependent nested capture in 4a439a5.** With tiles installed first, a Stage preview re-entered tile window effects and repeated offscreen capture/blur. Reversing installation order reduced the same three-tile SDR open frame from median 20 attachment clears/247 uniform lookups to 11/130. In the canonical FP16 tiles-first run, open/close were 39/37 clears; HEAD was 12/11. Existing local d8d74c7 already puts the tile hook outside Stage, guards capture re-entry/attachment aliasing and culls off-viewport previews. This task retains those fixes, rather than attributing them to the new optimization.
2. **Whole-output layout repeated per tile.** `applyVisibility()` visited every Stage Screen and invalidated its physical monitor each time. Thus one progress sample performed one/two/three identical monitor layouts, window-animation warps and output damages. It now updates every column first, then invalidates/warps/damages once per physical output. Common Stage scale/translation and adaptive max-workspace-count sizing remain unchanged.
3. **GL queries on every cache hit and blur draw.** `bufferFor()` saved/restored GL state and queried GL_MAX_TEXTURE_SIZE even when cached attachments already matched. Bindings/limits are now read only when allocating/replacing attachments or deleting expired buffers. Image-description metadata still refreshes on every hit. The twelve uniform locations are cached after linking and reset with the program. Format, mirror, size/context checks, soft-edge shaders, blur radius/pass count, clipping, colors and animation duration are unchanged.
4. **Negative findings.** Stable animation frames allocate zero textures/framebuffers, link zero shaders and do zero symbol lookups in all measured versions. These are not the cause of steady-frame stutter. First-open hook installation/shader work is separate from warm frames. No new whole-screen blur pass is introduced; the existing two-pass soft edge is retained.

## Warm animation timings

Each cell is **median / p95 / worst**, milliseconds; cycles 1–3 (cycle 0 reported separately below). No samples discarded for being slow.

| Build | Tiles | Direction | Frames | CPU work ms | GPU elapsed ms | Frame interval ms |
|---|---|---|---:|---|---|---|
| 4a439a5 | off | close | 86 | 0.964 / 1.385 / 2.798 | 6.407 / 16.419 / 16.763 | 16.642 / 31.574 / 39.732 |
| 4a439a5 | off | open | 76 | 1.252 / 1.964 / 3.491 | 7.686 / 16.492 / 16.646 | 18.017 / 38.930 / 61.649 |
| 4a439a5 | single | close | 80 | 1.101 / 1.586 / 2.972 | 6.769 / 16.432 / 16.821 | 17.737 / 35.618 / 37.136 |
| 4a439a5 | single | open | 79 | 1.380 / 2.136 / 3.929 | 5.940 / 14.584 / 16.528 | 20.188 / 35.943 / 38.060 |
| 4a439a5 | three | close | 90 | 1.864 / 2.207 / 3.729 | 5.374 / 10.963 / 16.800 | 17.543 / 22.133 / 26.813 |
| 4a439a5 | three | open | 90 | 1.959 / 2.465 / 3.806 | 4.311 / 7.581 / 16.165 | 16.667 / 19.545 / 25.769 |
| 4a439a5 | two | close | 74 | 1.472 / 1.791 / 3.445 | 4.457 / 12.317 / 16.788 | 20.450 / 35.232 / 52.721 |
| 4a439a5 | two | open | 87 | 1.536 / 2.007 / 3.335 | 4.670 / 8.805 / 11.636 | 16.800 / 24.040 / 29.284 |
| 54a7adf | off | close | 80 | 0.937 / 1.323 / 2.986 | 5.787 / 16.439 / 16.943 | 17.898 / 34.007 / 36.720 |
| 54a7adf | off | open | 80 | 1.297 / 1.933 / 3.624 | 6.846 / 16.455 / 16.923 | 18.025 / 36.535 / 45.127 |
| 54a7adf | single | close | 87 | 0.968 / 1.355 / 1.538 | 7.395 / 16.547 / 16.770 | 16.862 / 30.779 / 45.624 |
| 54a7adf | single | open | 75 | 1.208 / 2.324 / 5.448 | 5.881 / 16.503 / 22.739 | 17.771 / 39.708 / 68.381 |
| 54a7adf | three | close | 89 | 1.391 / 1.731 / 3.477 | 3.934 / 8.338 / 17.740 | 17.105 / 23.417 / 32.582 |
| 54a7adf | three | open | 89 | 1.499 / 1.766 / 3.221 | 3.170 / 8.189 / 9.959 | 16.728 / 20.683 / 22.214 |
| 54a7adf | two | close | 64 | 1.151 / 1.571 / 7.718 | 2.839 / 9.465 / 13.935 | 25.443 / 38.245 / 56.682 |
| 54a7adf | two | open | 79 | 1.228 / 1.673 / 3.741 | 2.532 / 8.268 / 9.527 | 19.297 / 30.497 / 35.360 |
| fixed | off | close | 82 | 0.920 / 1.444 / 3.086 | 6.773 / 16.542 / 17.962 | 17.144 / 35.158 / 38.240 |
| fixed | off | open | 80 | 1.244 / 1.891 / 3.591 | 4.678 / 16.483 / 17.421 | 17.799 / 30.601 / 38.246 |
| fixed | single | close | 83 | 0.963 / 1.416 / 2.927 | 7.994 / 16.553 / 17.050 | 16.697 / 32.674 / 38.276 |
| fixed | single | open | 81 | 1.241 / 1.819 / 3.100 | 6.668 / 16.557 / 17.373 | 16.816 / 34.591 / 42.492 |
| fixed | three | close | 88 | 1.324 / 1.552 / 3.033 | 4.244 / 8.627 / 16.311 | 17.516 / 24.978 / 27.148 |
| fixed | three | open | 89 | 1.377 / 1.656 / 3.542 | 3.681 / 7.205 / 14.494 | 16.689 / 19.775 / 23.739 |
| fixed | two | close | 69 | 1.129 / 1.559 / 3.280 | 1.970 / 9.177 / 15.729 | 23.332 / 47.087 / 53.626 |
| fixed | two | open | 80 | 1.247 / 1.574 / 3.203 | 2.991 / 7.619 / 10.835 | 19.236 / 30.183 / 32.103 |
| pre-tiles reference | off | close | 79 | 0.984 / 1.465 / 2.353 | 5.681 / 16.509 / 16.551 | 17.930 / 37.137 / 48.166 |
| pre-tiles reference | off | open | 74 | 1.185 / 1.954 / 5.869 | 5.854 / 14.851 / 16.545 | 20.660 / 38.419 / 42.229 |

Three-tile warm CPU medians improve 29.7% open / 29.0% close versus 4a439a5; versus the already-fixed local HEAD, the additional changes improve 8.1% / 4.8%. Fixed three-tile CPU p95 is 1.66 / 1.55 ms, comparable to tiles-off 1.89 / 1.44 ms. Fixed three-tile GPU medians are 3.68 / 4.24 ms, versus tiles-off 4.68 / 6.77 ms; the GPU medians vary with nested compositor waits, so these are not claimed as a causal GPU speedup. Intervals show the isolated case does not reproduce the severity of the user’s host-session stutter.

## Cold-start timings

Cycle 0 includes initial hook/shader setup; cells retain median / p95 / worst. These costs are not hidden in warm-only claims.

| Build | Tiles | Direction | Frames | CPU work ms | GPU elapsed ms |
|---|---|---|---:|---|---|
| 4a439a5 | off | close | 30 | 0.893 / 1.344 / 2.783 | 8.118 / 16.540 / 16.571 |
| 4a439a5 | off | open | 19 | 1.542 / 38.652 / 38.652 | 4.000 / 16.580 / 16.580 |
| 4a439a5 | single | close | 31 | 0.844 / 1.302 / 3.010 | 6.265 / 16.653 / 16.677 |
| 4a439a5 | single | open | 24 | 1.743 / 3.721 / 33.335 | 7.154 / 14.189 / 14.487 |
| 4a439a5 | three | close | 30 | 1.856 / 2.208 / 3.252 | 6.193 / 8.804 / 9.676 |
| 4a439a5 | three | open | 30 | 2.008 / 6.560 / 8.935 | 5.280 / 8.684 / 9.246 |
| 4a439a5 | two | close | 26 | 1.514 / 1.793 / 5.765 | 3.918 / 10.080 / 15.219 |
| 4a439a5 | two | open | 28 | 1.683 / 3.375 / 4.604 | 5.907 / 8.016 / 8.615 |
| 54a7adf | off | close | 26 | 0.989 / 1.553 / 6.431 | 6.864 / 15.081 / 16.419 |
| 54a7adf | off | open | 21 | 1.254 / 1.999 / 31.619 | 6.069 / 16.489 / 16.672 |
| 54a7adf | single | close | 27 | 1.055 / 1.940 / 2.835 | 5.887 / 16.434 / 16.550 |
| 54a7adf | single | open | 27 | 1.424 / 3.702 / 38.869 | 5.182 / 16.403 / 16.767 |
| 54a7adf | three | close | 30 | 1.459 / 1.803 / 3.254 | 4.747 / 8.005 / 13.581 |
| 54a7adf | three | open | 30 | 1.584 / 3.075 / 3.978 | 2.488 / 7.015 / 9.589 |
| 54a7adf | two | close | 26 | 1.089 / 1.548 / 1.560 | 4.391 / 13.502 / 15.087 |
| 54a7adf | two | open | 29 | 1.293 / 4.221 / 5.716 | 4.367 / 10.463 / 10.858 |
| fixed | off | close | 26 | 1.029 / 2.758 / 6.069 | 4.816 / 15.060 / 21.105 |
| fixed | off | open | 22 | 1.337 / 1.807 / 39.178 | 5.504 / 11.163 / 12.628 |
| fixed | single | close | 25 | 1.133 / 1.507 / 1.897 | 4.913 / 14.762 / 16.054 |
| fixed | single | open | 24 | 1.312 / 1.856 / 38.858 | 9.278 / 16.546 / 16.565 |
| fixed | three | close | 29 | 1.378 / 1.461 / 2.986 | 3.299 / 7.289 / 8.743 |
| fixed | three | open | 31 | 1.298 / 3.932 / 5.896 | 1.922 / 12.154 / 13.908 |
| fixed | two | close | 25 | 1.153 / 1.419 / 1.442 | 1.905 / 10.274 / 10.349 |
| fixed | two | open | 31 | 1.182 / 3.370 / 5.715 | 3.595 / 8.263 / 10.222 |
| pre-tiles reference | off | close | 29 | 0.956 / 1.558 / 2.692 | 5.707 / 16.415 / 16.626 |
| pre-tiles reference | off | open | 22 | 1.290 / 3.724 / 38.611 | 5.797 / 15.012 / 16.248 |

## Per-frame workload (warm medians)

| Build | Tiles | Direction | Output layouts | Uniform lookups | glGetIntegerv | Attachment clears | Cleared pixels | DrawArrays |
|---|---|---|---:|---:|---:|---:|---:|---:|
| 4a439a5 | off | close | 1 | 78 | 228 | 13 | 6.34727e+07 | 75 |
| 4a439a5 | off | open | 1 | 78 | 228 | 13 | 6.35475e+07 | 105 |
| 4a439a5 | single | close | 1 | 91 | 266 | 15 | 6.3465e+07 | 81 |
| 4a439a5 | single | open | 1 | 91 | 266 | 15 | 6.35659e+07 | 117 |
| 4a439a5 | three | close | 3 | 234 | 684 | 37 | 2.29637e+08 | 146 |
| 4a439a5 | three | open | 3 | 247 | 722 | 39 | 1.67473e+08 | 140 |
| 4a439a5 | two | close | 2 | 182 | 532 | 29 | 2.25939e+08 | 93 |
| 4a439a5 | two | open | 2 | 208 | 608 | 33 | 2.02461e+08 | 85 |
| 54a7adf | off | close | 1 | 78 | 228 | 13 | 6.35265e+07 | 75 |
| 54a7adf | off | open | 1 | 78 | 228 | 13 | 6.34649e+07 | 105 |
| 54a7adf | single | close | 1 | 78 | 228 | 13 | 6.34909e+07 | 73 |
| 54a7adf | single | open | 1 | 78 | 228 | 13 | 6.34981e+07 | 103 |
| 54a7adf | three | close | 3 | 65 | 190 | 11 | 6.31175e+07 | 127 |
| 54a7adf | three | open | 3 | 65 | 190 | 12 | 5.66666e+07 | 135 |
| 54a7adf | two | close | 2 | 39 | 114 | 7 | 6.27864e+07 | 97 |
| 54a7adf | two | open | 2 | 78 | 228 | 13 | 5.81725e+07 | 121 |
| fixed | off | close | 1 | 0 | 108 | 13 | 6.35267e+07 | 75 |
| fixed | off | open | 1 | 0 | 108 | 13 | 6.35061e+07 | 105 |
| fixed | single | close | 1 | 0 | 108 | 13 | 6.35005e+07 | 73 |
| fixed | single | open | 1 | 0 | 108 | 13 | 6.3454e+07 | 103 |
| fixed | three | close | 1 | 0 | 90 | 11 | 6.32447e+07 | 127 |
| fixed | three | open | 1 | 0 | 90 | 11 | 5.67357e+07 | 133 |
| fixed | two | close | 1 | 0 | 54 | 7 | 6.29372e+07 | 97 |
| fixed | two | open | 1 | 0 | 81 | 10.5 | 5.81434e+07 | 118 |
| pre-tiles reference | off | close | 1 | 78 | 228 | 13 | 6.34391e+07 | 75 |
| pre-tiles reference | off | open | 1 | 78 | 228 | 13 | 6.34472e+07 | 105 |

Cached GL queries drop from 228 to 108 per tiles-off/single frame, and 190 to 90 per three-tile frame compared with local HEAD. Uniform lookups go to zero. Warm allocation/link/symbol counters remain zero; the optional `--check-cache` assertion verifies all fixed cases, one median output layout and at most two per frame (settle/input updates may share a frame).

## Reproduction

Build each archived revision with matching release headers and identical flags:

```sh
PKG_CONFIG_PATH=/var/cache/hyprpm/wilf/headersRoot/share/pkgconfig cmake -S SOURCE -B BUILD -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build BUILD --target hymission -j4
# Repeat in fresh private compositors for off, single, two, three:
python -B tools/tiles_isolated_smoke.py --plugin BUILD/libhymission.so --fixture build-tiles/libhymission-tiles-test.so --stage-animation-profile --stage-profile-fp16-mirror --stage-profile-case three --output build-tiles/stage-perf/BUILD/three
# stage-first is an independent installation-order control:
# add --stage-profile-order stage-first
python -B tools/stage_animation_profile.py build-tiles/stage-perf/fixed-fp16 --check-cache --output build-tiles/stage-perf/fixed-cache-check.json
```

Raw per-frame profiles/logs: `build-tiles/stage-perf/{baseline-fp16,head-fp16,reference-fp16,fixed-fp16}/CASE/CASE-{open,close}-{0,1,2,3}.json`; combined `summary.json`. Earlier failed multi-case-in-one-process attempts are retained as diagnostic logs and excluded from the final tables; Stage intentionally retains hooks when closed, so a fresh compositor per case avoids order contamination. Cold and warm rows are aggregated independently by the committed helper.

Measured optimized plugin SHA256:

- baseline: `628758741028f2e01b04bd7b419b45ba73833b5943e38d51c9e7928d3a6b5047`
- head: `e7fd02d4010f64636d28f6389788900a468649fccfcdbb73cb1896dc551c63ee`
- reference: `50c72bb2fe94ce61cd6987c6c5cfca98747aa8fd3551ad615cfd1221679ca0c1`
- fixed: `cb58a0816dbc573ffb84f273fc6e30d0f39120c7f20685c1dd7da169d0f79551`

## Validation

- Full local CTest: 16/16 PASS, no skips (`ctest.log`).
- Release-compositor full integration smoke PASS (`regression-integration.log`).
- tiles_disabled_regression PASS, JSON exactly equal to the pre-tiles reference (`regression-disabled.log`).
- Stage maximum-workspace-count adaptive layout, three-window scrolling preview/camera/clip, and reserved top/CSD pixel checks PASS (`regression-stage-adaptive.log`, `regression-stage-multi.log`, `regression-top-clip.log`).
- Settled screenshot comparison against optimized 54a7adf: 6144x3456 frames, 2–70 changed pixels across four views (maximum 0.00033% pixels; mean absolute channel difference ≤0.000418/255). This is near-equality, not a claim of bit-identical screenshots. The same occupied-viewport/no-margin-leak pixel assertions pass both builds. No layout/shader/blur parameters changed in this task.
- Additional hook-order/bounded capture/empty-column transfer regression and fresh upstream build: final results below.

Physical HDR-session severe stutter and live acceptance remain **UNAVAILABLE**: no user-session reload or input was performed. The measured repeated rendering/layout/query costs are fixed; these data do not prove every cause of the user’s host-session symptom has been eliminated.

Final checks:

- Both hook installation orders, bounded soft-edge work, zero nested/aliased capture and empty-column transfer PASS (`regression-stage-perf.log`).
- Fixed warm-frame cache/layout assertions PASS for all four configurations (`fixed-cache-check.json`); the same assertions intentionally fail on 4a439a5 (`cache-negative-control.log`).
- Refreshed the flake Hyprland input; upstream main remains 8010b4e5c66f97aa82cd5b3332b4ec613779b9a7, so `flake.lock` is unchanged. `nix build -L .#hymission` PASS (`nix-update.log`, `nix-build.log`); 14 tests pass and two GL tests skip because EGL is unavailable inside the Nix check. Both skipped tests pass in the local 16/16 suite.
- Final isolated configuration-error files are empty. `git diff --check` PASS.
