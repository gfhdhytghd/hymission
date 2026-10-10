import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Hyprland

// Keeps a native Hyprland plugin in step with this Omarchy shell plugin:
// enabled loads the matching build, disabled or removed unloads it.
Item {
  id: root

  property var manifest: null

  readonly property string pluginDir: decodeURIComponent(Qt.resolvedUrl("..").toString().replace(/^file:\/\//, "")).replace(/\/$/, "")
  readonly property string ctl: pluginDir + "/omarchy/plugin"
  readonly property string pluginId: manifest ? manifest.id : ""
  readonly property string title: manifest ? manifest.name : "Plugin"

  property string state: ""
  property bool settingUp: false
  property bool notified: false
  property bool applyPending: false

  function run(proc, verb) {
    if (proc.running) return
    proc.command = [ctl, verb]
    proc.running = true
  }

  function requestApply() {
    if (applier.running) { applyPending = true; return }
    applyPending = false
    run(applier, "apply")
  }

  function openSetup() {
    settingUp = true
    Quickshell.execDetached(["bash", "-lc", `
      if command -v omarchy-launch-floating-terminal-with-presentation >/dev/null; then
        printf -v command '%q setup' "$1"
        exec omarchy-launch-floating-terminal-with-presentation "$command"
      else
        exec xdg-terminal-exec -e bash -c '"$1" setup; read -rp "Done."' bash "$1"
      fi`, "bash", root.ctl])
    poll.start()
  }

  // Critical toasts stay until handled; Omarchy expires the rest within 30 s.
  function notify(body, action, critical) {
    if (notifier.running) return
    // The shell's notification server may still be starting alongside us.
    notifier.command = ["bash", "-c",
      'wait="${OMARCHY_PATH:-/usr/share/omarchy}/bin/omarchy-notification-wait"; [[ ! -x $wait ]] || "$wait" 15 >/dev/null; exec notify-send "$@"',
      "bash", "-a", title, "-i", "system-software-install",
      "-u", critical ? "critical" : "normal", "-t", "30000"]
      .concat(action ? ["-A", "default=" + action] : []).concat([title, body])
    notifier.running = true
  }

  function handle(json) {
    let s
    try { s = JSON.parse(json) } catch (e) { return }
    root.state = s.state
    if (s.state === "loaded" && settingUp && s.outdated) {
      return  // a rebuild is still running; the next poll swaps it in
    } else if (s.state === "loaded") {
      settingUp = false
      poll.stop()
      if (s.outdated && !notified) {
        notified = true
        notify("An update is ready. Click to rebuild.", "Rebuild", false)
      }
    } else if (settingUp) {
      return
    } else if (s.state === "missing") {
      openSetup()
    } else if (s.state === "stale" && !notified) {
      notified = true
      notify("Hyprland was updated. Click to rebuild and load the plugin again.", "Rebuild", true)
    } else if (s.state === "foreign" && !notified) {
      notified = true
      notify("Another copy is already loaded (hyprpm?). Disable it to let Omarchy manage " + title + ".", "", false)
    }
  }

  Process {
    id: starter
    onExited: (exitCode, exitStatus) => {
      if (exitCode === 0) root.requestApply() // also covers events before we listened
      else {
        poll.stop()
        root.settingUp = false
        root.notify("The plugin could not be started. Click to run setup again.", "Rebuild", true)
      }
    }
    stdout: StdioCollector { onStreamFinished: root.handle(text.trim().split("\n").pop()) }
    stderr: StdioCollector { onStreamFinished: if (text.trim()) console.warn(root.pluginId + ": " + text.trim()) }
  }

  Process {
    id: applier
    onExited: (exitCode, exitStatus) => {
      if (root.applyPending) Qt.callLater(() => root.requestApply())
    }
    stderr: StdioCollector { onStreamFinished: if (text.trim()) console.warn(root.pluginId + ": " + text.trim()) }
    stdout: StdioCollector { onStreamFinished: if (text.trim()) console.info(root.pluginId + ": " + text.trim()) }
  }

  Process {
    id: notifier
    stdout: StdioCollector { onStreamFinished: if (text.trim() === "default") root.openSetup() }
  }

  // Setup runs in its own terminal; pick up its result as soon as it lands.
  Timer {
    id: poll
    interval: 2000
    repeat: true
    property int ticks: 0
    onRunningChanged: ticks = 0
    onTriggered: {
      if (++ticks > 900) { stop(); root.settingUp = false; return }
      root.run(starter, "start")
    }
  }

  Connections {
    target: Hyprland
    function onRawEvent(event) {
      if (event.name === "configreloaded") root.requestApply()
    }
  }

  // The shell passes the manifest at creation on startup and assigns it later
  // on hot-load, so handle both.
  Component.onCompleted: if (manifest) run(starter, "start")
  onManifestChanged: if (manifest) run(starter, "start")

  // Reconcile from files after shell teardown settles. Inline so checkout
  // deletion cannot break cleanup; the decision and unload share start's lock.
  Component.onDestruction: {
    if (!pluginId) return
    Quickshell.execDetached(["bash", "-c", `
      set -euo pipefail
      id=$1 cache=$2 checkout=$3 begin=$4 end=$5
      sleep 1.5
      mkdir -p "$cache"
      exec 9>"$cache/.lock"
      flock 9
      config="$HOME/.config/omarchy/shell.json"
      if [[ -d $checkout && -f $config ]] &&
        jq -e --arg id "$id" 'any(.plugins[]?; .id == $id)' "$config" >/dev/null; then
        exit 0
      fi
      menu="$HOME/.config/omarchy/extensions/omarchy-menu.jsonc"
      remove_menu() (
        [[ -f $menu ]] || return 0
        exec 8>"$(dirname "$menu")/.omarchy-menu.lock"
        flock 8
        target=$(readlink -f -- "$menu")
        if ! awk '$0 ~ /^[[:space:]]*\\{[[:space:]]*$/ { found=1 } END { exit !found }' "$target"; then
          echo "$id: menu extension has no standalone { line; leaving it unchanged" >&2
          return 0
        fi
        if ! OMARCHY_BEGIN=$begin OMARCHY_END=$end awk '
          $0 == ENVIRON["OMARCHY_BEGIN"] { if (skip) bad=1; skip=1 }
          $0 == ENVIRON["OMARCHY_END"] { skip=0 }
          END { exit (skip || bad) }
        ' "$target"; then
          echo "$id: incomplete managed menu block; leaving it unchanged" >&2
          return 0
        fi
        tmp=$(mktemp "$(dirname "$target")/.omarchy-menu.XXXXXX")
        trap 'rm -f -- "$tmp"' EXIT
        OMARCHY_BEGIN=$begin OMARCHY_END=$end awk '
          BEGIN { begin=ENVIRON["OMARCHY_BEGIN"]; end=ENVIRON["OMARCHY_END"] }
          $0 == begin { skip=1; next }
          skip { if ($0 == end) skip=0; next }
          { print }
        ' "$target" >"$tmp"
        if ! cmp -s -- "$target" "$tmp"; then
          chmod --reference="$target" "$tmp"
          mv -- "$tmp" "$target"
          omarchy menu refresh >/dev/null 2>&1 || true
        fi
      )
      remove_menu
      pid=$(hyprctl -j instances | jq -r --arg s "$6" '.[] | select(.instance == $s) | .pid')
      [[ -n $pid && -r /proc/$pid/maps ]] || exit 0
      path=$(OMARCHY_CACHE="$cache/" awk '
        { p=substr($0, index($0, "/")); sub(/ \\(deleted\\)$/, "", p)
          if (index(p, ENVIRON["OMARCHY_CACHE"]) == 1 && p ~ /\\.so$/) { print p; exit } }
      ' "/proc/$pid/maps")
      if [[ -n $path ]]; then
        hyprctl plugin unload "$path" >/dev/null
        hyprctl reload >/dev/null
      fi`, "bash", pluginId,
      (Quickshell.env("XDG_CACHE_HOME") || (Quickshell.env("HOME") + "/.cache")) + "/" + pluginId,
      Quickshell.env("HOME") + "/.config/omarchy/plugins/" + pluginId,
      "  // >>> " + pluginId + " (managed by the " + title + " Omarchy plugin; removed when it is disabled)",
      "  // <<< " + pluginId, Quickshell.env("HYPRLAND_INSTANCE_SIGNATURE") || ""])
  }
}
