# FB18: Stage columns always follow workspace number

Baseline: `origin/hyprland-master` at `d27948f`. FB18 intentionally replaces the
saved drag ordering introduced by earlier tile work. Sidebar/card sizes, adaptive
column sizing and desktop compression are unchanged.

## Current ordering contract

Every column, including tiles off and a single tile, has one authority:
`cardsForWorkspace()`. Positive numbered workspace IDs sort ascending. Workspaces
with non-positive named IDs follow the numbered IDs and sort lexicographically
by `displayName()` (case-sensitive, as before). Renaming a numbered workspace
does not change its numeric slot. Special workspaces are excluded from Stage
cards. Synthetic empty slots use their positive IDs and participate in the same
sort. `multi_workspace_sort_recent_first` remains an overview setting; neither
MRU history nor tile focus/ownership history overrides Stage slot order.

- A plain same-column drag never changes order, activation or window workspace
  membership. Its ghost returns to the numbered slot on release, using the
  existing Stage flight clock and renderer. Animations disabled means an
  immediate return. Empty cards have no window ghost to animate.
- A cross-column card drag still transfers the whole workspace through the
  existing ownership transaction (and exchanges visible display slots where
  required). Pointer height and the card under the pointer do not insert before,
  after, at top or at bottom. The destination column's numeric list determines
  the slot. Window workspace identities are preserved.
- A native Super+window drag into tile-column blank space still allocates the
  next available positive ID under the existing rules. Its new card occupies
  that numeric slot. With tiles disabled, arbitrary background still does not
  create a workspace; a configured synthetic slot does. Meta's explicit
  individual-window gesture, desktop drops and Split View remain available.
- Static layout, prepared swipe and all flight goals share the same numbered
  list. Existing passive-card transitions preserve sampled positions and move
  toward the new slots when a card changes column or a workspace is created.

`tileCardOrder`, `orderTileCard()` and their rollback/collapse residue are deleted.
The legacy tiles-off `swapWorkspaces()` content-exchange implementation is also
deleted. Old FB6 source inspection described that policy, but its synthetic
native input did not demonstrate a content swap; this change does not claim an
observed baseline swap where none was recorded. The new explicit contract is no
swap in every mode. Transient drag ownership/hover state remains necessary for
input and the single ghost; it is not a stored ordering preference.

## Animation and new-workspace findings

The existing Stage ghost geometry is shared by drawing and release flights.
Ghost boxes are saved **before** publishing a cross-column ownership change so
layout/column-size changes cannot alter the sampled source. Return flights use
the final numbered card/window slot; passive cards keep the existing transition.
The same ghost/return behavior now covers tiles-off sidebar drags. No additional
animation owner or persistent ordering state is introduced. Release also clears
the original-slot drag highlight immediately.

Enabling animated native blank drops exposed a previously untested problem:
`finishDrop()` looked for the destination card in the focused tile's screen.
When a blank drop created a workspace in another column, membership was correct
but no return flight was produced. The flight lookup now resolves the destination
workspace's tile, and its goal/aperture use the unshifted final slot. This is
verified independently from the plain card-return path.

The active-card variant (`stage_show_active=1`) needs a preview-only return:
its native desktop remains visible while the ghost flies back. Return flights
therefore do not suppress the original native window, and card destinations take
priority over the generic active-workspace desktop retarget. An intermediate
trial was caught by the new test with a 608.681 px desktop/card goal mismatch;
its captures remain in `fb18-active-pilot-off`. Interrupted workspace flights also
start from the real desktop when a preview-only return coexists with it. Overview
origin selection similarly ignores an active workspace's returning card copy.
Off/three-tile captures verify the all-overview source equals the native desktop
within 1 px while a return is actually in flight; overview becomes the sole
render owner.

## Regression protocol

The FB6/FB11 drop and real-device tests now assert numeric position instead of
pointer insertion. The FB17 order harness now attempts contradictory drops
(bottom of the same column, top of another column, bottom back in the original
column) and verifies numeric order in every captured committed/prepared scene.
Single mode adds/removes a second tile before keyboard/swipe checks, proving no
collapsed-tile drag preference survives. Recent-sort 0 and 1 are both tested. Active-card returns additionally inspect
the actual marker body in the native desktop on every captured frame, requiring
more than 70% green coverage inside its unobstructed body ROI.
Named workspaces are deliberately created in non-monotonic name order (middle,
alpha, zeta), distinguishing alphabetical order from creation/ID/MRU order; a real
special workspace is opened to verify Stage suspension and exclusion.

Each cross-column drop also checks intermediate passive-card positions, no
backwards replay and settled error ≤1 px. Each ordinary drag return retains
every rendered frame and requires moving
intermediate frames plus a flight goal within 1 px of the actual settled static
window slot. Inputs are synthetic events on private pointer, keyboard and trackpad devices
through the real compositor input/gesture path; no physical device or host IPC
is used. Keyboard and four-finger gesture paths retain the same FB17
source/goal and passive-card checks. Native Super+blank drops now enable Stage
animation, require a return flight, preserve every unrelated workspace identity,
and compare its goal with the new workspace's static slot (≤1 px).

The disabled regression retains its existing reference observations unchanged
and adds a separate `disabled-numeric-drag.json` assertion for the intentional
FB18 rule. This avoids silently substituting the new contract for a pre-tiles
reference result.

## Evidence and validation

The unmodified `d27948f` control fails the strengthened numeric oracle after a
same-column bottom drop: workspace 102 precedes 100. The raw control is retained
at `build-tiles/fb18-baseline-order`. Initial repaired off/single/two pilots pass;
the new native blank animation test first reproduced the missing foreign-column
flight and passes after the destination lookup repair. Failed pilots are retained.

Final validation was run after fetching/rebasing onto latest
`origin/hyprland-master`, still `d27948f`. Only this worktree is modified.

- Full local CTest: **16/16**, including EGL rendering tests, no skips.
- Full legacy/order matrix: **85 scenarios run**, initially 84 passed and one
  legacy shadow assertion failed; see the controlled follow-up below.
- Final rebuilt artifact: full release integration smoke and
  `tiles_disabled_regression` pass. The original disabled observations exactly
  match the retained pre-tiles reference. The separate new numeric-drag contract
  passes. Two/three-tile presentation/pinned/floating regressions also pass.
- Off/single/two/three, recent-sort 0/1: **56 keyboard/swipe switches / 3,441
  rendered frames**, maximum source/goal error **0 px**. **28 card returns /
  1,991 frames** have maximum goal error **0 px**, including eight active-card
  returns whose native desktop green-body ROI coverage stays **100%** throughout.
  Cross-column passive-card paths contain intermediate frames with no backwards
  replay and settle within 1 px. Native blank/synthetic creation drops require
  an animated flight and a static-slot error ≤1 px. Named/special checks pass.
- Active-return → all-overview handoff: off/three both measure **0 px** source
  error and transfer rendering ownership to overview. Corresponding frame/state
  sequences and `active-return-overview.json` are retained.
- Existing scrolling off/single/two/three and dwindle two/three handoff pixel /
  aperture checks pass. Eight Stage profile configurations cover both hook
  orders with **1,185 frames**; GPU samples are complete and all unchanged warm
  cache/layout/symbol/GL gates pass.
- The refreshed flake updates Hyprland to
  `12f242511a9c0fe8a884a9d90511bbbbe044f067` (2026-10-19), with its upstream
  dependency lock updates. Final `nix build -L .#hymission` passes against that
  development revision. The two EGL tests skip inside Nix and pass locally.
- Isolated runtime remains the specified release binary
  `~/.local/lib/hyprland-hdr/Hyprland`, verified commit
  `e5a03b6a5ce64ec4c85954649bad4df083627574`, with matching cached release headers.
  Development compilation and isolated release runtime are distinct evidence.

### Preserved legacy shadow outliers

`--feedback4-regression shadow --feedback4-three` initially records a 23-pixel
run in Stage-on/forceall (limit 16), and one later candidate replay records 24.
An earlier candidate replay passed. No overview/shadow source or threshold was
changed to obtain a green result.

A fixed three-pair serial follow-up retains **all** baseline/candidate outcomes:

| Pair | Unmodified d27948f Stage-on/forceall run | Candidate run | Outcome |
|---|---:|---:|---|
| 0 | 1 | 1 | Both pass |
| 1 | 2 | 12 | Both pass |
| 2 | 24 | 8 | Baseline fails, candidate passes |

The same assertion therefore reproduces on the unmodified baseline. This is
an intermittent existing gate, not a claim that every historical run passed or
that its underlying pixel cause is established. The candidate's three final
controls pass, as do the separate paired-mask overview reveal regressions; the
raw failure is not erased or relabeled as a pass. All PNGs, state samples,
observations and input hashes remain under the ignored `build-tiles/fb18-*`
directories. `fb18-evidence-identity.json` indexes the artifacts and gate results.

No push, host reload, `hyprpm update`, host plugin load/unload or physical-device
input was performed. These are isolated compositor/build results, not physical
session acceptance.
