#!/bin/bash
# hyprland-prefix.sh SLUG REF [--aquamarine SLUG REF]
# Builds the exact Hyprland revision a plugin branch targets into a cached prefix and prints its path:
#   <prefix>/bin/Hyprland, <prefix>/lib (hypr* libraries newer than the system's), <prefix>/share/pkgconfig
# Dependencies are rebuilt only when the system package is older than Hyprland's minimum. The cache key is
# the resolved commit, so a ref like "main" is re-resolved every call and only rebuilt when it moved.
set -euo pipefail
STATE=${HYPRLAND_PREFIX_STATE:-/home/wilf/.local/state/hyprland-plugins-maintenance/hyprland}
REFERENCE=/home/wilf/data/Hyprland
slug=$1; ref=$2; shift 2
aq_slug=hyprwm/aquamarine; aq_ref=
while [ $# -gt 0 ]; do case "$1" in --aquamarine) aq_slug=$2; aq_ref=$3; shift 3 ;; *) echo "unknown option $1" >&2; exit 2 ;; esac; done

resolve() { # resolve SLUG REF -> commit sha (tags are peeled)
  local out; out=$(git ls-remote "https://github.com/$1.git" "refs/tags/$2^{}" "refs/tags/$2" "refs/heads/$2" | head -1 | cut -f1)
  if [ -z "$out" ] && [[ $2 =~ ^[0-9a-f]{7,40}$ ]]; then out=$2; fi
  [ -n "$out" ] || { echo "cannot resolve $1 $2" >&2; exit 1; }; echo "$out"; }
sha=$(resolve "$slug" "$ref")
key=$sha; [ -n "$aq_ref" ] && key="$sha-aq$(resolve "$aq_slug" "$aq_ref" | cut -c1-12)"
dir=$STATE/$key
if [ -f "$dir/.ok" ]; then echo "$dir"; exit 0; fi
mkdir -p "$STATE"; work=$(mktemp -d "$STATE/build.XXXXXX"); log=$work/build.log
# On failure keep the log next to the cache so the caller can quote it; the partial build itself is discarded.
trap 'code=$?; if [ $code -ne 0 ]; then cp "$log" "$STATE/failed-$key.log" 2>/dev/null; echo "hyprland-prefix.sh failed (exit $code); log: $STATE/failed-$key.log" >&3; rm -rf "$dir"; fi; rm -rf "$work"' EXIT
exec 3>&1; exec >"$log" 2>&1
rm -rf "$dir"; prefix=$dir; mkdir -p "$prefix"
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig:$prefix/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
export LD_LIBRARY_PATH=$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
jobs=$(( $(nproc) * 3 / 4 )); [ $jobs -ge 1 ] || jobs=1

clone() { # clone SLUG SHA DIR
  local ref_opt=(); [ -d "$REFERENCE/.git" ] && ref_opt=(--reference-if-able "$REFERENCE" --dissociate)
  git clone -q "${ref_opt[@]}" "https://github.com/$1.git" "$3"; git -C "$3" checkout -q "$2"; git -C "$3" submodule update -q --init --recursive; }
build_dep() { # build_dep NAME SLUG REF
  local name=$1 dslug=$2 dref=$3 src=$work/$1
  echo "== building $name $dref from $dslug"; clone "$dslug" "$(resolve "$dslug" "$dref")" "$src"
  cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_TESTING=OFF -DINSTALL_TESTS=OFF
  ninja -C "$src/build" -j "$jobs" && cmake --install "$src/build"; }
vergte() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }

src=$work/Hyprland; echo "== cloning $slug $sha"; clone "$slug" "$sha" "$src"
cm=$src/CMakeLists.txt
need() { { grep -oE "$1_MINIMUM_VERSION [0-9.]+" "$cm" || true; } | head -1 | awk '{print $2}'; }
for pair in hyprutils:HYPRUTILS hyprlang:HYPRLANG hyprcursor:HYPRCURSOR hyprgraphics:HYPRGRAPHICS hyprwire:HYPRWIRE; do
  pkg=${pair%%:*}; min=$(need "${pair#*:}"); [ -n "$min" ] || continue
  have=$(pkg-config --modversion "$pkg" 2>/dev/null || echo 0)
  vergte "$have" "$min" && { echo "$pkg: system $have satisfies >= $min"; continue; }
  build_dep "$pkg" "hyprwm/$pkg" "v$min"; done
min=$({ grep -oE "hyprland-protocols>=[0-9.]+" "$cm" || true; } | head -1 | cut -d= -f2); have=$(pkg-config --modversion hyprland-protocols 2>/dev/null || echo 0)
if [ -n "$min" ] && ! vergte "$have" "$min"; then build_dep hyprland-protocols hyprwm/hyprland-protocols "v$min"; fi
min=$(need AQUAMARINE); have=$(pkg-config --modversion aquamarine 2>/dev/null || echo 0)
if [ -n "$aq_ref" ]; then build_dep aquamarine "$aq_slug" "$aq_ref"
elif [ -n "$min" ] && ! vergte "$have" "$min"; then build_dep aquamarine hyprwm/aquamarine "v$min"; fi

echo "== building Hyprland"
cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DNO_XWAYLAND=ON
ninja -C "$src/build" -j "$jobs"
cmake --install "$src/build"
# Headers + hyprland.pc: the install target does not ship them on every version.
[ -f "$prefix/share/pkgconfig/hyprland.pc" ] || make -C "$src" installheaders PREFIX="$prefix"
PKG_CONFIG_PATH=$prefix/share/pkgconfig:$prefix/lib/pkgconfig pkg-config --modversion hyprland
"$prefix/bin/Hyprland" --version | head -3
ln -sfn Hyprland "$prefix/bin/hyprland"
printf '%s\n' "$slug $ref $sha" > "$prefix/.source"
cp "$log" "$dir/build.log" && touch "$dir/.ok"
exec 1>&3; echo "$dir"
