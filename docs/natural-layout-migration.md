# Natural layout migration

The default `layout_engine` is now `natural`. Its algorithm is the former
natural2 core; `natural2` is an enum/name alias, including scope overrides and
session history. Set `layout_engine = "grid"` for the previous grid layout.

Removed: the old natural anchor/repulsion solver, scale-flex density bias,
profile search, corner-void relief, sparse expansion, input-rank scale gradient,
and natural-to-grid fallback (about 1,250 implementation lines).
`natural_scale_flex` remains registered as a deprecated no-op. The C++ fields
`naturalScaleFlex` and `rankScaleByInputOrder` remain compatibility sinks for
existing callers and the independent evaluator. `layout_scale_weight` and
`layout_space_weight` still control grid, including thumbnail's grid fallback;
they do not affect natural. Grid and thumbnail remain selectable.

The shared name parser covers the base engine and forceall/all/current-workspace
overrides. Natural's controller metadata, per-region contexts, packed scrolling
path, strip predictions and tile regions use the canonical engine value. Stage's
small drag ghost, which constructs a default `LayoutConfig`, also uses natural.
The translated Stage ghost regression and grid-specific fixtures are retained;
old natural-specific scale/repulsion expectations were replaced by the natural
constraints, topology and warm-session suites. Alias tests cover cold equality,
filtered survivors, spelling changes within a live context and deprecated knobs.

## Development worktree validation

- Started by fast-forwarding `design/natural-layout` to `6e0b593`.
- Full Debug CMake build with `-Werror`: passed. Uses the configured local
  development headers `e5a03b6a5ce64ec4c85954649bad4df083627574`, and the matching
  private `build-hdr-release/Hyprland --version` executable for native probes.
  This is offline validation of that revision, not fresh upstream HEAD/Nix or
  host-session acceptance. The existing flake pin was not changed.
- Full CTest: **16/16 passed**, including tiles/Stage logic, software GL rendering,
  workspace identity, snapshot color and native framebuffer contracts.
- Standalone layout suite with `-Wall -Wextra -Werror -D_GLIBCXX_ASSERTIONS`: passed.
- Layout ASan/UBSan with leak detection and assertions: passed.
- `python3 docs/changelog/build.py` and `git diff --check`: passed.
- Independent evaluator v1.1, public only: 57 scenes / 1,074 cases per name,
  corpus `e6ed6e7bfdd0`, configuration `8fb0fd142883`. Both `natural` and
  `natural2` score **91.76116962096039**, session **86.17699412742158**;
  every scene's slots and session score compare exactly. Gate mean 1, no flags.
  Reports: `/tmp/hymission-natural-replace-eval/{natural/natural-replaced,natural2/natural2-alias}/report.json`.
  Evaluator sources were not modified; holdout was not read or executed.

Reproduction (in this worktree):

```sh
cmake -S . -B /tmp/hymission-natural-replace-dev \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-Werror -DBUILD_TESTING=ON \
  -DHYPRLAND_TEST_EXECUTABLE=/home/wilf/data/Hyprland/build-hdr-release/Hyprland
cmake --build /tmp/hymission-natural-replace-dev -j 6
ctest --test-dir /tmp/hymission-natural-replace-dev --output-on-failure
g++ -O2 -std=c++23 -Wall -Wextra -Werror -D_GLIBCXX_ASSERTIONS -Isrc \
  tools/mission_layout_test.cpp src/mission_layout.cpp -o /tmp/hymission-natural-replace-strict
/tmp/hymission-natural-replace-strict
g++ -O1 -g -std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Isrc tools/mission_layout_test.cpp src/mission_layout.cpp \
  -o /tmp/hymission-natural-replace-sanitized
ASAN_OPTIONS=detect_leaks=1 /tmp/hymission-natural-replace-sanitized
```

No host plugin reload, install, push or runtime configuration changes were made.
The focus/hover ordering follow-up is implemented in a separate commit; see
[natural focus stability](natural-focus-stability.md). The core algorithm itself
retains the accepted geometry.
