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

  function run(proc, verb) {
    if (proc.running) return
    proc.command = [ctl, verb]
    proc.running = true
  }

  function openSetup() {
    settingUp = true
    Quickshell.execDetached(["bash", "-lc",
      'if command -v omarchy-launch-floating-terminal-with-presentation >/dev/null; then ' +
      'exec omarchy-launch-floating-terminal-with-presentation "$1 setup"; ' +
      'else exec xdg-terminal-exec -e bash -c "$1 setup; read -rp Done."; fi', "bash", root.ctl])
    poll.start()
  }

  function notify(body, action) {
    if (notifier.running) return
    notifier.command = ["notify-send", "-a", title, "-i", "system-software-install"]
      .concat(action ? ["-A", "setup=" + action] : []).concat([title, body])
    notifier.running = true
  }

  function handle(json) {
    let s
    try { s = JSON.parse(json) } catch (e) { return }
    root.state = s.state
    if (s.state === "loaded") {
      settingUp = false
      poll.stop()
      if (s.outdated && !notified) {
        notified = true
        notify("An update was downloaded. Rebuild to use it.", "Rebuild")
      }
    } else if (settingUp) {
      return
    } else if (s.state === "missing") {
      openSetup()
    } else if (s.state === "stale" && !notified) {
      notified = true
      notify("Hyprland was updated. Rebuild the plugin to load it again.", "Rebuild")
    } else if (s.state === "foreign" && !notified) {
      notified = true
      notify("Another copy is already loaded (hyprpm?). Disable it to let Omarchy manage " + title + ".", "")
    }
  }

  Process {
    id: starter
    stdout: StdioCollector { onStreamFinished: root.handle(text.trim().split("\n").pop()) }
    stderr: StdioCollector { onStreamFinished: if (text.trim()) console.warn(root.pluginId + ": " + text.trim()) }
  }

  Process {
    id: applier
    stdout: StdioCollector { onStreamFinished: if (text.trim()) console.info(root.pluginId + ": " + text.trim()) }
  }

  Process {
    id: notifier
    stdout: StdioCollector { onStreamFinished: if (text.trim() === "setup") root.openSetup() }
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
      if (event.name === "configreloaded") root.run(applier, "apply")
    }
  }

  onManifestChanged: if (manifest) run(starter, "start")

  // The shell also tears services down when it restarts, so decide only once
  // it answers again: unload only if this plugin is gone or disabled. Inline,
  // because removal deletes the checkout right after disabling it.
  Component.onDestruction: Quickshell.execDetached(["bash", "-c", `
    id=$1 cache=$2
    for _ in $(seq 50); do omarchy-shell -q shell ping >/dev/null 2>&1 && break; sleep 0.2; done
    list=$(omarchy-plugin-list --json 2>/dev/null) || exit 0
    jq -e --arg id "$id" 'any(.[]; .id == $id and .enabled)' <<<"$list" >/dev/null && exit 0
    pid=$(hyprctl -j instances | jq -r --arg s "$HYPRLAND_INSTANCE_SIGNATURE" '.[] | select(.instance == $s) | .pid')
    path=$(awk -v c="$cache/" 'index($6, c) == 1 && $6 ~ /\\.so$/ { print $6; exit }' "/proc/$pid/maps")
    [ -n "$path" ] || exit 0
    hyprctl plugin unload "$path" >/dev/null && hyprctl reload >/dev/null`,
    "bash", pluginId, (Quickshell.env("XDG_CACHE_HOME") || (Quickshell.env("HOME") + "/.cache")) + "/" + pluginId])
}
