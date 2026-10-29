# Scrolling overview edge reveal (FB13)

## Baseline and root causes

Requested baseline: `2feb179`. The supplied screenshot is read-only and is not committed. Before committing, this worktree was rebased onto `origin/hyprland-master` at `1041461`, preserving FB12's output-wide Stage flight ordering. The `hymission-tiles-fb7` worktree is untouched.

Three paths were involved:

- `captureWindowPasses()` explicitly extracted `OverviewShadowPassElement` from the cropped/blurred content batch. Its shadow exclusion was correctly mapped after FB4, but the outer shadow then rendered across the output independently of the reveal mask. Fixing the cutout alone cannot make a decoration follow the content's moving edge.
- `overviewRevealFrame()` expanded the mapped viewport towards the output while reducing edge widths to zero. That is the old card/unfold design, not the requested desktop band translating past a window. The desktop and sidebar card cases now carry distinct policy through `EdgeViewport::revealDecorations`; cards retain their original fixed-outer-edge behavior.
- A completely clipped desktop column failed `overviewEndpointVisible()` and was routed through the generic offscreen-workspace endpoint. That code could choose vertical direction from the overview slot or workspace animation. Desktop tape endpoints now retain their actual layout positions, including wholly hidden columns; remaining offscreen scrolling endpoint selection uses the owning viewport's left/right side and layout y.

The Stage-disabled control exposed an additional rendering gap: there was no Stage render-window hook in the all-overview path, so computed edge state could be reported without being applied to pixels. Overview now leases just the shared native render-window hook and optional edge-blur hook independently of Stage's input/layout hooks. Stage later reuses the render lease. Native rendering outside overview remains a passthrough.

## Resulting behavior

Desktop reveal translates the blur/alpha band outwards at its window-relative width, rather than shrinking it in place. The fully open endpoint leaves the entire window clear. Closing traverses the reverse path; interruption samples keep their edge state and decoration ownership. Contents, borders and native cutout shadows are captured in one layer and receive one mask, avoiding a separate early shadow or duplicate edge animation.

For ws overview, the owner clip moves between the Stage-compressed desktop tile and the final overview tile allocation. The tile wrapper respects that intermediate clip and keeps the actual viewport coordinate system, instead of rebuilding it from the clip rectangle. Native shadow clipping uses the same intermediate owner region. All overview retains the physical-output scene. Empty Stage columns participate in the shared desktop geometry and handoff even when they have no sidebar cards.

Tiles-off, single-tile and multi-tile desktop sources share the same reveal policy. Stage-hidden sources derive their native desktop viewport without enabling Stage input/layout behavior. Stage card reveals and FB12 workspace-activation flights retain their separate policies.

## Evidence and scope of conclusions

`tools/tiles_overview_reveal_regression.py`, reached through `--overview-reveal-regression`, creates native Wayland marker clients in scrolling layout and focuses a middle column, leaving columns wholly outside both sides. It records every rendered framebuffer plus native, Stage and overview state for entry and exit, all/ws scope, Stage on/off, tiles off/single/two/three. PNGs and JSON remain ignored under `build-tiles`.

The private fixture samples overview state **before** synchronous GPU readback, matching the existing Stage phase sampling. Sampling it afterwards advances the clock and can associate a later aperture with earlier pixels. This correction changes test instrumentation only.

Pixel evidence additionally holds a real private-device overview gesture at two intermediate phases (12 and 60 input units), captures shadows enabled/disabled at identical window geometry, and differences pixels beyond the reveal band. This isolates shadow pixels from the strip panel and overlapping green clients; a dark-background heuristic alone can mistake the strip for a shadow. Every phase pair checks identical geometry. Zero sampled pixels are reported as such and are not treated as proof that the old visual defect was absent.

The exact `2feb179` baseline reproduces the empty-frame defect in Stage-on ws overview: the 60-unit held pair changes **2,574 of 3,432** tested pixels beyond the content mask. Its Stage-on all/ws direction checks also fail for wholly hidden columns. Other controlled baseline cases did not produce the same detectable shadow artifact; this is not generalized into a claim that every tiles-off or all-overview configuration reproduced it.

The pre-tiles `552bd339` reference is independently loaded into the matching private release compositor (`fb13-reference-final`). Its source uses the same card reveal/unfold math, shadow-pass extraction and generic offscreen endpoint fallback. The original card behavior is preserved. That reference does not export explicit animation-from endpoints; the exact new direction assertion is therefore unavailable there. Its paired pixel run has no qualifying outside-mask samples in this setup, so it does **not** establish either a reproduced or absent pre-tiles empty-frame defect. Native frames and source comparison are retained rather than inventing a positive reproduction.

## Validation

Final logs and artifacts use the `build-tiles/fb13-verified-*` prefix. The source, binary, compositor and frame index identities are recorded in `fb13-evidence-identity.json`.

- Complete local CTest **16/16**, no skips; tiles-disabled observations exactly match `feedback3-disabled-reference/default-off-observations.json`; release integration smoke passes.
- New all/ws reveal regression: tiles off/single/two/three each pass **48/48** checks, with Stage on/off, intermediate entry/exit frames and held shadow controls. Every paired shadow check reports zero leaked pixels. Cases with no qualifying pixels remain explicitly recorded with zero samples.
- Independent closing-frame validation passes **517** hidden-column exit samples. Across **488** recorded band samples, width normalized back to native window coordinates remains 32 pixels (floating-point range 31.999999999999996–32.00000000000001). This checks that the band translates rather than collapsing as the window scales. Each exit returns to its actual side and native layout y.
- FB4 shadow exclusion/interior regression, top/reserved-area regression and FB12 three-column scrolling flight clip regression pass (`fb13-verified-required.log`).
- Pure desktop-band tests sample 101 phases on both sides, asserting outward motion, invariant window-relative band width, shared decoration ownership and a fully clear final window. Original card/fixed-edge and interruption tests remain.
- Refreshed development flake targets upstream main `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`; Nix build passes using the writable local store (`fb13-verified-nix.log`). The two EGL tests unavailable in Nix are covered by the local complete CTest run.

Release runtime is `~/.local/lib/hyprland-hdr/Hyprland`, revision `e5a03b6a5ce64ec4c85954649bad4df083627574`. Development HEAD is compile/test evidence, not a claim that the release compositor is HEAD or that a host plugin reload was tested. The archived reference build's `stage_logic.cpp`, `stage_controller.cpp` and `overview_controller.cpp` were checked byte-for-byte against `552bd339`.

These are isolated compositor and raster results, not physical-session acceptance. No push, host reload, hyprpm update, or host plugin load/unload is performed.
