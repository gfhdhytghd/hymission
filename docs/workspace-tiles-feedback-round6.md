# Stage drop targets and workspace identity (FB6)

> FB18 supersedes the ordering policy in this historical report: every Stage
> column uses numeric/name order; sidebar drag height never inserts/reorders cards
> or swaps workspace contents, including with tiles disabled. Ownership transfer
> and FB17's shared preview/static/flight authority remain. See
> [the current contract](workspace-tiles-feedback-round18.md).

The tile adapter must resolve a Stage drop from the pointer, never from the physical monitor's global active workspace. This change is confined to tiles-enabled paths.

## Native baseline and requested semantics

With tiles disabled, `button()` uses `reorderTargetIndex()` for inter-card gaps and swaps the *contents* of two workspace slots through `swapWorkspaces()`. Padding outside those bounds cancels. Native desktop drags onto a card target its workspace. The disabled regression compares the original reference observations independently.

The user's supplemental rules deliberately replace the content-swap behavior when tiles are enabled:

| Source and destination | Result |
| --- | --- |
| Card or preview to its own sidebar column, including padding and gaps | Reorder the containing workspace card, retaining workspace/window identity |
| Workspace card to another sidebar column, including empty column/slot/padding | Transfer the workspace's tile ownership and insert at the pointer's position; never merge its windows into another workspace |
| Individual window/group to another column's card | Move only that window/group to the card's workspace |
| Individual window/group to another column's empty area | Move only that window/group to that column's currently displayed workspace |
| Card/preview to a tile desktop | Use the existing split/move preview and activation transaction, resolving the tile from the pointer |
| Single tile card/preview to desktop | Split in place, with the dragged members in the new tile |
| Outside any legal destination, Esc, or return to source | Cancel; no global-active fallback |

If an already displayed workspace changes columns, its display slot is exchanged with the destination slot, preserving both workspaces and their windows. Hidden workspaces only change their home/history. Sidebar order is session-local and does not rename workspace IDs. A sidebar point resolves to the nearest visible column within the shared panel, including padding; a point outside the panel is never treated as a sidebar target.

## Root cause and change

The previous candidate detector promoted cross-column card drags into window transfer transactions before sidebar reorder could run. It flattened workspace cards into lists of windows. Empty-column handling then selected a desktop workspace for these lists, losing workspace identity. The new path defers promotion until the pointer actually leaves the sidebar, handles a return to the sidebar before releasing the transaction, and commits sidebar reorder/ownership separately. Whole-workspace ownership uses the same validated tile model publication mechanism; failures restore the previous home.

## Reproduction and evidence

`tools/tiles_stage_drop_regression.py`, invoked through the private nested compositor smoke runner, creates six real Wayland clients and drives pointer presses, motion, releases and Esc. It records screenshots and Stage/tile/client snapshots before release and afterwards. Tests cover one, two and three tiles, sidebar padding, whole-workspace cross-column ownership/order, individual-window empty-column targeting, desktop targets, split creation, invalid targets and shared-owner cleanup.

Baseline binary: `build-tiles/fb6-before.so` (374ce6b). `build-tiles/fb6-before-two/results.json` reproduces failed reorder and whole-workspace identity checks. Fixed observations and screenshots are in `build-tiles/fb6-{single,two,three}/`. Screenshots and local build outputs are ignored and not committed.

Full release smoke now asserts that tiles-enabled same-column reorder retains workspace identity, replacing its superseded expectation that contents move to workspace 77. The disabled path retains its separate original behavior checks.

## Validation

- CMake build and all 16 CTest tests pass (`build-tiles/fb6-checks.log`).
- Strict single/two/three drop regressions pass; the archived two-tile baseline fails the new ownership/order checks.
- `tiles_disabled_regression` observations exactly equal `feedback3-disabled-reference/default-off-observations.json`.
- Full smoke with the installed release compositor passes (`build-tiles/fb6-release.log`).
- The high-resolution Stage performance/bounds/empty-column test passes (`build-tiles/fb6-stage.log`). The empty column must remain a target even though native Stage's `interactive()` rejects a column with no cards; target resolution now explicitly includes it. Its pointer test selects the visible intersection of a scrolling preview with its card, rather than an off-camera window center.
- Native disabled pointer observations are retained in `fb6-native-off/native-reorder-observation.json`; the padding drop preserves membership. The native content-swap policy above is established by source inspection; the synthetic input sequence did not exercise a content swap. The independent disabled-reference comparison is the behavioral regression gate.

All runtime checks use isolated Weston/Hyprland, real Wayland clients, and the release binary. This is not host-session or physical HDR acceptance. No push or live reload was performed.
