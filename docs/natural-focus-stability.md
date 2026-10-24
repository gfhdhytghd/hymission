# Natural overview focus stability

The natural replacement was committed separately as `931b195` on
`design/natural-layout` and ported as `78fb2d0` on `port/natural2-master`.
This follow-up fixes focus-related layout movement on both lines without
changing `natural_layout.hpp` (SHA256
`5695fea9d2c702db2fbed8ad5be99843dd4d936b2da4a98b7ddd40f587dac369`).

## Findings and changes

1. **Native resize / column changes.** The controller passed current native
   geometry into its copied `LayoutContext`, while explicitly excluding natural
   from `freezeExistingLayout`. The core deliberately supports resize reflow:
   content below 60% of its accepted reference area may enter `LocalReflow`;
   column topology changes can cause a cold fallback. That general-purpose API
   behavior conflicts with the overview's frozen-layout contract.

   `OverviewLayoutSession` now remembers the visible input snapshot per region.
   Identical stable identities and workspace membership reuse source rectangles,
   column metadata and allocation geometry, regardless of native focus resizes,
   floating/column transients, input renumbering or subpixel viewport roundoff
   (up to 0.001 logical pixels). The adapter always supplies emphasis 1. Opening,
   closing, filtering or replacing windows, changing workspace/region, and real
   viewport/configuration changes can still update layout. The raw core keeps
   its legitimate resize API and its existing regression coverage.

   A separate size-only probe keeps identities and metadata unchanged: nine
   360×220 floating windows at a 400×270 source grid, region (100,200,1200,800),
   zero padding and spacing 24. Doubling only window 4's width makes the
   no-freeze control enter `LocalReflow` and change that preview's target;
   the fixed adapter stays `Retained` with zero target changes. At width factors
   1.2/1.5 the control remains `Retained` but changes the content rectangle;
   factors 2/3/4/8 enter `LocalReflow`. This isolates the conditional resize
   trigger from metadata corruption; it does not itself prove a host swap.
   Probe source/results: `resize-only.cpp` and `resize-only-{old,new}.log` in
   the control directory below.

2. **Ripple displacement.** The original solver checked bounds and overlap but
   not relative ordering. An otherwise feasible fallback bearing could move a
   peer across another window's center ordering. The extracted pure solver in
   `overview_expansion.hpp` now accepts only candidates preserving the frozen
   center ordering and original separating axes (numerical tolerance 1e-7).
   It retains radial pressure and directional escape search, reducing growth
   when the requested enlargement is infeasible. Scale 1 produces no pressure.

   The negative control disables only these acceptance checks. Seed 0's first
   selection/hover batch (selected index 1, hovered index 2) already fails.
   For example, pair 6/0 changes center-X difference from -17 to +1.70062;
   pair 5/3 changes center-Y difference from -1 to +12.0682. These are measured
   changes of relative order, not evidence of a full host-session window swap.

3. **Hover retarget / passive rebuild timing.** Each expansion starts from the
   frozen slots; an independent hover expansion starts from the selected pass's
   targets within the same synchronous batch. Core emphasis is not compounded.
   Natural passive rebuilds now preserve both the current ripple endpoints and
   its original interpolation start/animation clock, instead of discarding the
   ripple or snapping to a newly computed base. `currentPreviewRect` uses the
   same bounded interpolation progress for all previews. Keeping the same
   separating axes is a set of linear inequalities, so transitions between
   accepted endpoint sets, including mid-animation retargets, cannot cross.

4. **Ties / floating point.** There is no evidence here of random core sorting
   or a non-strict comparator. The old ripple used a strict distance comparator
   with input-order ties; its peer ordering is now the explicit strict tuple
   `(distance, index)`. Frozen geometry removes frame-dependent source sorting
   and topology changes; small viewport roundoff cannot reset the context.
   The conditional feasibility/resize thresholds and unguarded directional
   escape explain layout-dependent risk. The user's particular host frame
   has not been captured, so no unique live cause is claimed.

The controller changes are in `updateSelectedWindowLayout`, its extracted
geometry helpers, and `rebuildVisibleState`. The shared session adapter changed;
strip rendering/snapshots/FB19 code and the `hymission-tiles` worktree were not edited.
The deprecated layout-demo rank flag also no longer implicitly changes grid
input ordering: its documented no-op behavior is now consistent.

## Offline evidence

- 64 fixed seeds × 48 native geometry/metadata frames = **3,072 frozen-input
  rebuilds**, with renumbered/shuffled inputs, changing column axes/IDs and
  source sizes, simulated emphasis and viewport roundoff. Source/target
  rectangles remain exact and the reflow kind stays `Retained`.
- Membership replacement/filtering and isolated viewport reflow still work;
  existing multi-monitor/tile/workspace ownership and speculative-copy tests pass.
- 128 seeds × 24 selection/independent-hover batches = **3,072 retargets**.
  Exact repeatability, real enlargement and peer movement are checked; each
  batch samples nine interpolation positions, including unfinished animation
  retargeting, and checks order plus non-overlap.
- Removing only snapshot freezing makes the seeded geometry test fail.
  Removing only ripple order checks makes seed 0 / retarget 0 fail.
  Control sources, executables and logs are in
  `/tmp/hymission-natural-focus-controls/`.
- Both full Debug plugin builds pass **`-Werror`**. Development snapshot
  `e5a03b6a5ce64ec4c85954649bad4df083627574`: **16/16 CTests**.
  Stable `v0.56.2` / `efb50993780079460b0cbed1363e2166a2de1d9f`, using the
  requested `pc056`: **13/13 CTests**. Stage/tiles logic and native/software-GL
  probes registered on each branch pass; no skipped tests.
- Both standalone layout suites pass `-Wall -Wextra -Werror` and assertions;
  both ASan/UBSan layout suites pass with assertions and leak detection.
  Logs use `/tmp/hymission-natural-focus-{dev,master}-{build,ctest}.log`,
  `/tmp/hymission-natural-focus-{strict,sanitized}.log` and
  `/tmp/hymission-natural-focus-master-{strict,sanitized}.log`.
- The independent public scores remain the migration's **91.76117 / 86.17699
  session**, identical for natural and natural2. This follow-up changes the
  controller adapter/ripple, not the raw core evaluated by that harness.
  No evaluator changes or holdout access were made.

No host reload, install, push or runtime configuration edits were performed.
Actual hover/focus visual acceptance, especially focus-resizing scrolling layouts
and multiple live tiles, remains pending a safe user-run reload.
