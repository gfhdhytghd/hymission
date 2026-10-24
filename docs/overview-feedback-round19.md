# FB19: desktop decorations and live workspace strip

Original baseline: local `hyprland-master` merge `6e0b593` (including natural2).
Before final validation, rebased onto local `hyprland-master` at `4c1f225`,
including the natural replacement and focus/ripple stability changes. Work is
confined to `hymission-tiles`, branch `fix/overview-exit-shadow-strip`; the
concurrent natural2/layout worktree is untouched.

## Shadow continuity

The overview shadow applied an extra `easeInOutCubic(visualProgress())` alpha
multiplier. The window body and border did not. Thus closing reduced the shadow
to zero immediately before native rendering restored it. Opening had the reverse
mismatch. Remove that multiplier; retain the window's managed/native alpha,
shadow rules, native animated color, mapped bounds/cutout, and existing tile and
Stage clip ownership. No independent shadow animation or timing is introduced.

A source-predicate bisect identifies `0128a1164c4335f60521cc68dc8286be90180ce1`
as the introduction. Its parent lacks the multiplier. The original change was
intentional: it addressed a Stage shadow/crop appearance when Stage decorations
were disabled. The pre-tiles reference `552bd339` already has the multiplier;
this is not a new tiles regression. Runtime comparisons with that reference and
the FB19 baseline retain the observed fade. The source bisect is distinct from
a compositor runtime bisect; its full log and predicates are retained.

## Two different geometry contracts

FB7 continues to freeze the **main overview arrangement** across focus changes.
That arrangement is a selection/layout session, not a desktop capture.

The **workspace strip** is a live miniature of the workspace's real desktop.
Ordinary strip snapshots now sample current native window position and size
(and native scrolling camera placement); hidden workspaces use valid goal
geometry by the existing snapshot rule. They no longer use the mini-overview
layout computed by `buildState`, nor run a separate tile overview layout. The
snapshot renders at output resolution, keeps native client/CSD geometry, and
uses the existing tile UV crop when displayed. It has no independent layout
ordering and does not mutate the main overview layout session.

Focus changes queue a deferred strip capture, allowing native focus/layout
handlers to finish before sampling. The existing 500 ms refresh timer continues
to sample subsequent geometry, camera motion and surface changes; refresh=0
retains the every-frame scheduling policy. Snapshots are refreshed, not looked
up through a geometry cache key. Previously the timer could refresh perfectly
regularly and still capture the wrong **overview** geometry. Explicit drag/drop
prediction remains transient, with its existing preview/crossfade behavior.

## Evidence protocol

`tools/overview_fb19_regression.py`, invoked by the private smoke runner through
`--fb19-regression`, uses colored native Wayland clients, real scrolling layout
focus/column resize, private pointer events, and device-level overview gestures.
It retains every rendered frame as PNG plus compositor/native geometry. No host
IPC discovery or physical input is used.

- Strip green-body horizontal boundaries must match the independently mapped
  current native client and tile allocation within 4 thumbnail pixels. The
  same capture checks that main overview target slots remain frozen.
- Timed opening/closing frames are supplemented with held low-openness device
  gesture frames; expensive GPU readback must not silently skip the endpoint
  being tested. Shadow/background samples outside the window compare low/high
  openness, requiring a visible shadow rather than a phase-driven fade.
- Off/single/two/three, ws/all overview and Stage off/on are exercised. FP16
  variants verify the actual workbuffer DRM format. The private fixture reads
  float attachments as float (GLES rejects the previous unsigned-byte read),
  then clamps SDR marker diagnostics for PNG. This does not claim HDR display
  colorimetry or physical session acceptance.

## Validation results

All final runtime captures use the release compositor
`~/.local/lib/hyprland-hdr/Hyprland`, verified commit
`e5a03b6a5ce64ec4c85954649bad4df083627574`, and matching cached release headers.
Development compilation is checked separately through Nix.

- CMake build and complete local CTest: **16/16**, no local skips.
- FB19 acceptance: **8 cases, 304 assertions, 4,033 rendered frames**, covering
  off/single/two/three × RGBA8/FP16, each with ws/all and Stage off/on. The
  smallest measured native focus-dependent width change is **147 px** (so these
  are not no-op focus trials). Maximum strip boundary error is **0.730 px**;
  the sampled strip source and actual post-exit desktop differ by **0 px**.
  Main overview slots remain frozen during these focus changes. Low-openness
  shadow/background contrast is at least **89.5%** of high-openness contrast,
  rather than falling to zero.
- `6e0b593` before-fix control: all four native resize trials really resize;
  four strip checks and eight shadow checks fail. Maximum strip mismatch is
  **15.969 px**. At openness 0.1 the measured shadow contrast falls to 0–1.
- `552bd339` is built from an archive and run in the same private compositor.
  It also shows contrast around 3 at openness 0.2 and 0–1 at openness 0.1,
  confirming the fade predates tiles. Its older IPC lacks `visualProgress`;
  these observations explicitly label openness as **geometry-derived**, using
  the measured active/native width endpoints of the linear preview transform.
  This reference uses its original `natural` engine, not the later natural2
  solver. It is a decoration comparison, not a claim of layout parity.
- Full **85-case legacy matrix** is run again after the rebase, including
  `tiles_disabled_regression`, release integration smoke, native gesture /
  lifecycle / animation paths, top clipping, overview reveal, presentation,
  Stage geometry, ordering, native drops, edge effects and handoffs. The first
  pass has 82 successes and three stale-oracle failures; the corrections and
  successful targeted replays are detailed below. Original failed runs remain.
- Additional search/header coverage: **32 scenes** pass, including reserved
  bars and a nonzero output origin. Focus-expansion paired trials still run at
  1.18; structural/header-only controls use scale 1 to isolate their contract.
- Latest refreshed Hyprland flake revision:
  **`a1c2428f79b3b05c3fd36c27378e3538c8c6a014`**. `nix build -L .#hymission`
  passes. Its Stage EGL and snapshot-color tests skip inside Nix; both pass in
  the complete local CTest run and FP16 is independently checked at runtime.

### Regression oracle corrections retained with the evidence

The old FB4 shadow check sampled the entire *uncropped* moving window rectangle.
In the failing frame, its 20-pixel segment begins at x=860, while that window's
actual Stage transition clip starts at x=960. It was measuring a gap outside the
window's visible viewport. The corrected oracle intersects the content interior
with `stageTransitionClip` and excludes its explicit fade edges; the **16-pixel
threshold is unchanged**. Replaying the same retained frames now reports maxima
`[1, 1, 0, 1]` across the four Stage/scope combinations. A separate, clearly
labeled synthetic 24-pixel hole planted inside an actually green visible body
must still fail the gate. Every shadow run verifies that negative control.
Three fixed serial baseline/candidate pairs (unmodified `4c1f225` versus FB19)
all pass with maximum legitimate interior run 1; all synthetic holes are detected.
The stronger existing paired-mask overview reveal checks also pass.

The old focus structural test required tile resizing to move all-overview
windows. That contradicts its single output-wide region and the new focus-stable
layout policy. Workspace-scoped regions must still react to tile resizing.
All-overview structural checks now require unchanged membership, no tile groups,
and positive rectangles within the physical output, allowing the solver to
respond to a genuine topology change without requiring either movement or
immobility. Focus/resize paired trials retain their original strict invariance
checks, including expansion 1.18. The corrected two-tile and expansion suites
pass. Search/header tests similarly disable deliberate hover expansion while
checking reservations; the original expansion-dependent mismatch is also
reproduced on unmodified `4c1f225` and retained.

Legacy drop/integration fixtures explicitly set `stage_drag_reorder=0` for the
FB18 fixed-slot identity contract they assert. The newer default renumbering
feature is preserved and remains covered by its own tests and disabled suite.
No product defaults or real user configuration are changed by these fixtures.

An early FP16 recorder trial failed because GLES rejected unsigned-byte reads
from a float attachment. The private recorder now uses float readback for FP16;
this is a test fixture correction, not a compositor crash fix. A second pilot
missed the timed closing high-openness frame under synchronous readback load.
Final tests hold **both** low and high openness through real device gesture
updates, while still retaining the complete timed opening/closing sequences.
Failed pilots and original assertion failures are preserved, not relabeled.

The ignored `build-tiles/fb19-evidence-identity.json` indexes input hashes, logs,
frame/state reports, the bisect log, controls and final gates. Acceptance data is
under `build-tiles/fb19-accepted-*`; the full rebased matrix is under
`build-tiles/fb19-rebased-reg-*` with targeted corrected replays alongside it.
Only this worktree was edited. No push, host reload, `hyprpm update`, live plugin
load/unload or physical-device input was performed. Isolated readback/build
results do not substitute for the user's physical session acceptance.
