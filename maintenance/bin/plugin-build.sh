#!/bin/bash
# plugin-build.sh REPO_DIR HYPRLAND_PREFIX [LABEL]
# Configures, builds and runs the offline tests of a plugin checkout against one Hyprland prefix, inside the
# sandbox (no network, no home, no live compositor). Output: REPO_DIR/build-LABEL and build-LABEL.log.
# Exit code 0 = configure, build and ctest all passed. Pass HYPRLAND_PREFIX=system to use the system headers.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(readlink -f "$1"); prefix=$2; label=${3:-check}
build=$repo/build-$label; log=$repo/build-$label.log
cat > "$repo/.plugin-build-inner.sh" <<'INNER'
set -uo pipefail
repo=$1; prefix=$2; build=$3; flags=()
if [ "$prefix" != system ]; then
  export PKG_CONFIG_PATH=$prefix/share/pkgconfig:$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
  export LD_LIBRARY_PATH=$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
  flags+=(-DHYPRLAND_TEST_EXECUTABLE="$prefix/bin/Hyprland")
fi
echo "== hyprland $(pkg-config --modversion hyprland 2>/dev/null || echo none) from $(pkg-config --variable=pcfiledir hyprland 2>/dev/null)"
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON "${flags[@]}" || { echo "== CONFIGURE FAILED"; exit 2; }
ninja -C "$build" -k 0 || { echo "== BUILD FAILED"; exit 3; }
if grep -q "add_test" "$build/CTestTestfile.cmake" 2>/dev/null || ls "$build"/*/CTestTestfile.cmake >/dev/null 2>&1; then
  ctest --test-dir "$build" --output-on-failure --timeout 600 || { echo "== TESTS FAILED"; exit 4; }
else
  echo "== no ctest targets"
fi
echo "== OK"
INNER
ro=(); [ "$prefix" != system ] && ro=(--ro "$prefix")
gpu=(); for n in /dev/dri/renderD* /dev/nvidiactl /dev/nvidia0; do [ -e "$n" ] && gpu+=(--dev "$n"); done
"$here/sandbox.sh" --rw "$repo" "${ro[@]}" "${gpu[@]}" --env NINJA_STATUS="[%f/%t] " -- bash "$repo/.plugin-build-inner.sh" "$repo" "$prefix" "$build" > "$log" 2>&1
code=$?; rm -f "$repo/.plugin-build-inner.sh"
grep -E "^== |error:|FAILED|Failed|tests passed|Total Test time" "$log" | tail -n 25
echo "log: $log"; exit $code
