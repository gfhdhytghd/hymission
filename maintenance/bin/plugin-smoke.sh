#!/bin/bash
# plugin-smoke.sh HYPRLAND_BINARY PLUGIN.so [DISPATCHER ARGS]...
# Starts a private headless Hyprland of the exact target build inside the sandbox, loads the plugin,
# runs each dispatcher (one argument each, e.g. "hymission:toggle"), checks the compositor survived,
# unloads the plugin and exits. Output is the evidence; exit code 0 means every step passed.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
hypr=$(readlink -f "$1"); so=$(readlink -f "$2"); shift 2
libdir=$(dirname "$(dirname "$hypr")")/lib; [ -d "$libdir" ] || libdir=$(dirname "$hypr")
work=$(mktemp -d "${TMPDIR:-/tmp}/plugin-smoke.XXXXXX")
logs=${SMOKE_LOGS:-$work/logs}; mkdir -p "$logs"; echo "logs: $logs"
printf 'monitor = , 1280x720@60, auto, 1\nmisc {\n  disable_hyprland_logo = true\n}\ndebug {\n  enable_stdout_logs = true\n  disable_logs = false\n}\nxwayland {\n  enabled = false\n}\n' > "$work/smoke.conf"
cp "$here/wayland-parent-shim.c" "$here/weston-test-seat.c" "$work/"
cat > "$work/inner.sh" <<'INNER'
set -u
hypr=$1; so=$2; conf=$3; shift 3
[ -d "$(dirname "$(dirname "$hypr")")/lib" ] && export LD_LIBRARY_PATH="$(dirname "$(dirname "$hypr")")/lib"
export XDG_RUNTIME_DIR; mkdir -p -m 700 "$XDG_RUNTIME_DIR"
export GBM_BACKEND=${GBM_BACKEND:-nvidia-drm}
export XDG_CACHE_HOME="$LOGS/cache"; mkdir -p "$XDG_CACHE_HOME"
cc -O2 -Wno-attributes -shared -fPIC "$(dirname "$conf")/weston-test-seat.c" $(pkg-config --cflags --libs libweston-15) -o "$XDG_RUNTIME_DIR/test-seat.so" || { echo "SMOKE FAIL: seat module build"; exit 1; }
weston --backend=headless --renderer=gl --width=1280 --height=720 --socket=smoke-parent --idle-time=0 --no-config --modules="$XDG_RUNTIME_DIR/test-seat.so" > "$LOGS/weston.log" 2>&1 & wpid=$!
for _ in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/smoke-parent" ] && break; kill -0 $wpid 2>/dev/null || break; sleep 0.2; done
[ -S "$XDG_RUNTIME_DIR/smoke-parent" ] || { echo "SMOKE FAIL: headless parent (weston) did not start"; tail -n 20 "$LOGS/weston.log"; exit 1; }
export WAYLAND_DISPLAY=smoke-parent
cc -O2 -shared -fPIC "$(dirname "$conf")/wayland-parent-shim.c" -ldl -o "$XDG_RUNTIME_DIR/parent-shim.so" || { echo "SMOKE FAIL: shim build"; exit 1; }
LD_PRELOAD="$XDG_RUNTIME_DIR/parent-shim.so" "$hypr" -c "$conf" > "$LOGS/hyprland.log" 2>&1 & pid=$!
for _ in $(seq 100); do sock=$(ls "$XDG_RUNTIME_DIR"/hypr/*/.socket.sock 2>/dev/null | head -1); [ -n "$sock" ] && break; kill -0 $pid 2>/dev/null || break; sleep 0.2; done
if [ -z "${sock:-}" ]; then echo "SMOKE FAIL: compositor did not start"; tail -n 30 "$LOGS/hyprland.log"; exit 1; fi
export HYPRLAND_INSTANCE_SIGNATURE=$(basename "$(dirname "$sock")")
sleep 1
echo "compositor: $(hyprctl version | head -1)"
for _ in $(seq 50); do hyprctl monitors 2>/dev/null | grep -q '^Monitor' && break; sleep 0.2; done
echo "monitors: $(hyprctl monitors | grep -oE '^Monitor [^ ]+' | cut -d' ' -f2 | tr '\n' ' ')"
status=0
out=$(hyprctl plugin load "$so"); echo "load: $out"; case "$out" in *ok*|*"loaded"*) ;; *) status=1 ;; esac
echo "list:"; hyprctl plugin list | sed 's/^/  /'
hyprctl plugin list | grep -q "$(basename "$so" .so)\|Plugin " || status=1
for d in "$@"; do
  out=$(hyprctl dispatch $d)
  # Lua config providers expose plugin dispatchers as hl.plugin.<plugin>.<name>(args) instead of plugin:name args.
  case "$out" in *hl.dispatch*) name=${d%% *}; rest=${d#"$name"}; rest=${rest# }
    lua="hl.plugin.${name%%:*}.${name#*:}"; [ -n "$rest" ] && lua="function() $lua(\"$rest\") end"
    out=$(hyprctl dispatch "$lua"); d="$lua" ;; esac
  echo "dispatch $d: $out"; case "$out" in *error*|*"Invalid dispatcher"*) status=1 ;; esac; sleep 1
  kill -0 $pid 2>/dev/null || { echo "SMOKE FAIL: compositor died after dispatch $d"; tail -n 40 "$LOGS/hyprland.log"; exit 1; }
done
out=$(hyprctl plugin unload "$so"); echo "unload: $out"; case "$out" in *ok*|*unloaded*) ;; *) status=1 ;; esac
kill -0 $pid 2>/dev/null || { echo "SMOKE FAIL: compositor died"; status=1; }
hyprctl dispatch exit >/dev/null 2>&1; sleep 1; kill $pid 2>/dev/null; wait $pid 2>/dev/null; kill $wpid 2>/dev/null; wait $wpid 2>/dev/null
grep -iE "crash|segfault|abort|CRITICAL|initError" "$LOGS/hyprland.log" | head -5
ls "$XDG_CACHE_HOME"/hyprland/hyprlandCrashReport* 2>/dev/null | head -1 | xargs -r -I{} sh -c 'echo "crash report: {}"; head -n 40 {}' 
[ $status -eq 0 ] && echo "SMOKE PASS" || echo "SMOKE FAIL"
exit $status
INNER
gpu=(); for n in /dev/dri/renderD* /dev/nvidiactl /dev/nvidia0; do [ -e "$n" ] && gpu+=(--dev "$n"); done
"$here/sandbox.sh" "${gpu[@]}" --rw "$logs" --env LOGS="$logs" --ro "$work" --ro "$(dirname "$hypr")" --ro "$libdir" --ro "$(dirname "$so")" -- \
  bash "$work/inner.sh" "$hypr" "$so" "$work/smoke.conf" "$@"
code=$?; [ -n "${SMOKE_LOGS:-}" ] && rm -rf "$work"; exit $code
