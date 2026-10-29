# Stage reserves the scrolling layout viewport (FB14)

## Baseline diagnosis

Baseline: `origin/hyprland-master` at `7cdc3ed`. Native layout measurements distinguish a reduced layout viewport from a framebuffer crop or scale.

Stage originally preserved the sizing width of a multi-column scrolling tape (`a40df9b`), translating its work area while clipping the overflow behind the sidebar. `d7ed354` added the single-column exception. Thus, in the requested baseline, tiles disabled and one enabled tile still kept the original scrolling work-area width for a multi-column tape.

FB5 (`4a439a5`) changed the multi-tile branch to apply `scaleDesktopRect()` to `CSpace::m_workArea` itself. That already reduced the native `CScrollingAlgorithm::usableArea()` and recomputed native column boxes. It was not merely a raster scale. The two-tile baseline control confirms this: each work area changes from 600 to 461.25 and each 0.55-width column from 330 to 253.6875. This path already satisfies the requested viewport behavior; no multi-tile reproduction of an unchanged native width is claimed.

In contrast, the tiles-disabled baseline remains 1240 wide throughout Stage opening and closing, with 682-wide logical columns. The sidebar occupies space but the sizing viewport does not shrink. Its lack of intermediate viewport widths is recorded in `fb14-before-off-final`; the single/two/three baseline controls are retained separately.

## Implementation

- Remove the scrolling column-count exception: every native layout work area reserves the Stage sidebar width. Scrolling's normalized column fractions resolve against the narrower usable area. The single-tile and disabled-tile cases now agree with the multi-tile policy.
- Preserve camera position in column coordinates by scaling the pixel offset, including the inhibited-scroll offset, by the viewport-size ratio. Stage handles the tiles-disabled path. The existing outer Tiles camera hook handles enabled tiles, now including a single tile when Stage reserves its desktop. It observes the final work area once, avoiding a second rebase.
- Retain the existing common multi-tile work-area mapping, full vertical work area, coupled sidebar/card layout and equal card heights. Native surfaces are not given an additional desktop raster scale. Non-scrolling layouts retain their previous reservation logic.
- Stage visibility progress still drives the original open/close animation and warps the native position/size animations after each layout update, so the native layout follows Stage without a second lagging animation.

## Native measurements and images

The private test uses a 1280×720 output, five scrolling columns per active workspace, normalized width 0.55, native gaps and borders. Full work areas include the native gap policy; they are smaller than the geometric desktop allocation. In multi-tile mode, the existing common mapping scales those gap margins along with the work-area rectangle. The three-tile case deliberately uses allocations 640/320/320.

| Mode / workspace | Native work area closed → open → closed | Logical column closed → open | Geometric open desktop width |
| --- | --- | --- | --- |
| Tiles off / 901 | 1240 → 964 → 1240 | 682 → 530.2 | 1004 |
| Single tile / 901 | 1240 → 964 → 1240 | 682 → 530.2 | 1004 |
| Two tiles / 901, 903 | 600 → 461.25 → 600 each | 330 → 253.6875 | 492 each |
| Three tiles / 901 | 600 → 451.875 → 600 | 330 → 248.53125 | 482 |
| Three tiles / 903, 905 | 280 → 210.875 → 280 each | 154 → 115.98125 | 241 each |

The test fixture reads native work areas, `usableArea()`, controller offsets, normalized column sizes and logical target boxes. Each before/opened/closed state also records native client geometry and an actual screenshot. Every-rendered-frame recordings include those layout values before GPU readback; they do not infer layout changes from resized pixels.

Artifacts are ignored under `build-tiles/fb14-{off,single,two-scrolling,three-scrolling}` and the four `*-dwindle` directories. Each has `before.png`, `opened.png`, `closed.png`, state JSON, `measurements.json`, and `opening/closing` frame indices with PNGs. The two- and three-tile desktop allocations remain proportional and all native work-area heights stay constant throughout both transitions.

Across 772 scrolling animation samples, the selected column remains fully visible. The maximum pixel-camera rebase error is below 0.0011 pixels; normalized column fractions are unchanged, actual logical column widths match the reduced viewport ratio, and closing restores the original width. Opening and closing both contain multiple native intermediate widths and are checked for a one-frame endpoint jump. `camera-validation.json` provides the independent per-frame camera/visibility checks.

## Regression coverage

The complete local CTest suite passes (16/16, no skips). Tiles-disabled observations exactly match the retained default-off reference, and the complete release integration smoke passes. The refreshed development flake remains at Hyprland `8010b4e5c66f97aa82cd5b3332b4ec613779b9a7`; `nix build -L .#hymission` passes. Its two EGL-dependent tests skip in the Nix builder and pass in the local suite. The runtime matrix additionally covers prior Stage switch/drop/height, real device drop, native workspace animation, real gesture lifecycle, relative navigation, presentation/fullscreen/drag, FB2/FB3 feedback, FB4 shadow/close/switch, focus reflow, FB12 flight clipping, FB13 reveal, and both hook orders in Stage animation profiling. All eight scrolling/dwindle viewport cases pass. Detailed run names and outcomes are indexed in `build-tiles/fb14-evidence-identity.json`.

The old FB3 performance test initially fails on the unmodified baseline too: it disables and recreates Tiles but expects workspace 504 to retain its previous column without an explicit transfer. It then expects a plain card-body drag to merge a window into the active workspace, contradicting FB11. The fixture now explicitly transfers the card to prepare the desired column after hook reinstallation, and checks whole-workspace identity on the final empty-column drop. Bounded effect work, active-card exclusion, alias/nested-capture checks and the inverse hook-order case remain intact. The corrected performance test passes; production ownership and drag semantics are unchanged by this test repair.

The eight serial animation profiles (off/single/two/three, Tiles-first and Stage-first hook installation) pass the unchanged warm-cache gates: complete GPU timing samples, at most two layout calls per warm frame with median at most one, zero symbol lookups and no warm GL resource allocations or shader/uniform cache misses. An earlier concurrent profiling run exceeded the layout-call gate (worst three); it is retained separately, not counted as a pass. Serial baseline and current controls both pass. High-resolution timing depends on nested-compositor/client scheduling; these checks establish bounded work and correct animation state, not physical-display frame-rate acceptance.

Before committing, fetch/rebase confirms `origin/hyprland-master` remains `7cdc3ed`, with no source changes to integrate.

All runtime runs use private nested compositors with the matching e5a03b6 release binary. Images, frame indices and binary/source identities remain build artifacts. No host reload, hyprpm update, host plugin load/unload or push is performed. These measurements and raster checks do not claim physical-session acceptance.
