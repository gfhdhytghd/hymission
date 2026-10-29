# Workspace tiles implementation and validation

Date: 2026-10-17. Branch: `feat/workspace-tiles`. Pure plugin; default off;
maximum three tiles per physical monitor. The reviewed design was updated before
implementation of both monitor semantics and overview/Stage collaboration.
The subsequent [review fixes and revalidation](workspace-tiles-review-fixes.md)
record cancellation, native-path, ABI, default-off parity and protocol regressions.
No host plugin load, unload, reload, `hyprpm update`, config edit or push occurred.
The isolated compositor loads only the test build in its own process.

## Implementation phases

| Phase | Local commit | Result |
| --- | --- | --- |
| Reviewed pure-plugin design and three-tile semantics | `65064ea` | Design first |
| Transactional model and randomized invariants | `0fe4a3e` | Geometry, histories, generations, max three |
| Overview/Stage requirement update | `a4005d7` | Design first |
| Native viewports, hook broker and API | `bb2d7b9` | Real workspaces and native surfaces |
| Workspace actions, histories and swipe | `88cc5f5` | Native focus/swap routing |
| Divider interaction | `845d627` | Resize, cancellation and owner exclusion |
| Shared Stage edge rendering | `fd13564` | One capture/effect service |
| Overview/Stage and native lifecycle integration | Commit containing this record | Grouped presentation, transfer/split transactions and regressions |

The last phase also fixes broker continuation calls outside a hook and ownership
of native ABI arguments. Isolated rendering exposed these issues: state-only
assertions had passed while Stage previews were missing. Regression tests now
exercise direct continuations, shared ownership, rvalue references and priority
reordering. Stage and overview hooks use unique full signatures and roll back on
required-hook failure; tiles report the failure and collapse to native behavior.

## Default pkg-config build and tests

Headers: default pkg-config Hyprland git headers, reported version 0.56.0.
Isolated compositor: `/home/wilf/.local/lib/hyprland-hdr/Hyprland`, matching source
revision `e5a03b6a5ce64ec4c85954649bad4df083627574`. This is a local HDR development
build, not evidence of upstream HEAD runtime compatibility. No coverage build.
The linker paths below select the same Aquamarine ABI as that executable.

```sh
cmake -S . -B build-tiles -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug \
  -DHYPRLAND_TEST_EXECUTABLE=/home/wilf/.local/lib/hyprland-hdr/Hyprland \
  -DCMAKE_SHARED_LINKER_FLAGS=-L/home/wilf/data/aquamarine/.prefix/lib \
  -DCMAKE_MODULE_LINKER_FLAGS=-L/home/wilf/data/aquamarine/.prefix/lib \
  -DCMAKE_BUILD_RPATH=/home/wilf/data/aquamarine/.prefix/lib
cmake --build build-tiles -j 6
ctest --test-dir build-tiles --output-on-failure
```

**PASS: 16/16 tests**, including 40,000 seeded model operations and the native
symbol/framebuffer contract probe. Software GL tests cover adjacent viewport
scratch reuse, clipping, blur/fade, fractional coordinates, transforms, damage,
format/mirror switching and poisoned neighboring pixels. These are software
contracts, not HDR display acceptance. An ownership assertion initially checked
the reference count before the call's full expression ended; the corrected test
checks after argument temporaries have been destroyed.

## Isolated runtime integration

```sh
python tools/tiles_isolated_smoke.py \
  --plugin build-tiles/libhymission.so \
  --fixture build-tiles/libhymission-tiles-test.so \
  --integration --output build-tiles/isolated-final
```

**PASS.** The harness uses a private runtime directory and Unix IPC sockets,
Weston headless GL, then a nested Wayland Hyprland. Host display/session/IPC
variables are removed. DRM and libseat targets are deliberately unavailable to
the child; no physical output is acquired. A test-only Weston virtual seat and
Wayland protocol-version shim supply compatible parent facilities. They are
never installed or used by the shipped plugin. Real `weston-simple-shm` clients
exercise native windows and rendering; the private fixture invokes native gesture
sessions and plugin input events. This proves synthetic isolated behavior, not
physical touchpad feel or hardware input coverage.

Assertions cover:

- Default off; creation up to three; fourth add errors without state mutation;
  resize, remove, previous, visible-target focus and explicit swap.
- Native swipe tile-width displacement, cancel/commit, continuous segments,
  direction lock and resize/swipe exclusion.
- Stage Esc/return-to-source cancellation; single-to-two and two-to-three split;
  column-to-column and desktop drops; entire-stage transfer at the limit;
  shared ownership; divider/Esc while Stage is enabled.
- Native directional focus across a one-window tile boundary, adjacent-window
  API move and return; a new client's placement in the focused tile.
- Native fullscreen/maximize and layout-aware scrolling fullscreen/maximize
  within tile geometry, with other bindings unchanged; monitor-wide pinned and
  special behavior.
- Vertical and horizontal overview strip groups, two-column/two-row layouts,
  per-tile main window bounds, group-specific workspace clicks,
  `onlycurrentworkspace` collecting all visible tiles, and empty strip-only state.
- Two nested outputs; visible workspace focus; explicit on-current-monitor swap
  from a single-tile output to a nonfocused tile on another output; named monitor
  binding; automatic workspace creation skipping an ID bound to the other output;
  three-tile limits on both outputs; output removal and disabling tiles.

JSON snapshots, screenshots and logs remain in `build-tiles/isolated-final/`;
the directory is ignored. Screenshots are validation artifacts only. Normal tiles
and Stage continue to render native surfaces. `stage-three.png`,
`split-preview.png`, `overview-left.png` and `overview-top.png` document the
grouped presentation; automated assertions additionally inspect geometry and
actual client membership. Native headless output creation in this environment
returned 0×0, so the second-output test uses a second nested Wayland output.

The same full integration run also **PASSed with AddressSanitizer on the plugin**
(`build-tiles-asan/libhymission.so`, harness `--asan`, artifacts in
`build-tiles/isolated-asan/`). No ASan error was reported. The native compositor
and graphics driver were not instrumented; leak detection was disabled. This is
an additional plugin memory-access check, not a full compositor memory audit.

## Fresh upstream development build

The flake input was refreshed to Hyprland upstream `main`
`8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`, and the lock is committed with this
implementation. The system Nix store is not writable; the repository's existing
local-store procedure is used:

```sh
nix --extra-experimental-features 'nix-command flakes' \
  --store /home/wilf/.cache/hymission-nix flake update hyprland
nix --extra-experimental-features 'nix-command flakes' \
  --store /home/wilf/.cache/hymission-nix build -L .#hymission --no-link
```

**PASS: full plugin build; 14 tests passed and 2 GL tests skipped** in the Nix
check phase (`hymission-stage-edge-render-test`, `hymission-snapshot-color-test`,
EGL/GLES-unavailable return status). Both GL tests passed in the default
pkg-config build above. Full details are in the
[compatibility record](compatibility/hyprland-master.md). This check does not
load the resulting plugin into the host or establish upstream HEAD runtime
acceptance.

## Remaining acceptance boundaries

**PENDING:** physical touchpad/touch/tablet and pointer constraints; mixed HDR/SDR
visuals and metadata, 10-bit/FP16 hardware paths, mirror capture on hardware,
physical fractional-scale/rotated outputs and disconnect/reconnect, VRR/tearing,
lockscreen runtime interaction. A real ext-workspace client now passes isolated
visible-workspace activation; see the review record. Native mouse
fullscreen-query scoping and lock cancellation have code paths, but these are
not a substitute for device-level acceptance. Workspace display-name identity has
model coverage; numeric workspace-ID mutation is not a runtime acceptance claim.

**PENDING:** fixed-load CPU/GPU p50/p95/p99 frame times, missed frames and VRAM
comparisons for one/two/three tiles. No performance or real HDR acceptance is
inferred from build success, GL pixels or nested screenshots. No pure-plugin
impossibility was established for the implemented interactions; private ABI
drift and compositor paths outside the exercised cases remain compatibility risks.
