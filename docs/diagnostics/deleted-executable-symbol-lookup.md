# Deleted compositor executable during symbol lookup

The 2026-11-03 22:00:55 EST crash (PID 6411) ran Hyprland
`e5a03b6a5ce64ec4c85954649bad4df083627574` and Hymission 0.11.0 from
`~/.local/lib/hymission-fix/hymission.so`.

The matching old Hyprland executable recovered this stack from the core:

```
updateTrackpadGesture -> retargetGestureScope -> beginOpen
-> refreshWorkspaceStripSnapshots -> renderWorkspaceStripSnapshot
-> findFunction -> HyprlandAPI::findFunctionsByName
-> std::filesystem::canonical -> __cxa_throw -> terminate -> abort
```

The core contained:

```
filesystem error: cannot make canonical path: No such file or directory [/proc/self/exe]
```

The executable mapping was marked `(deleted)`. The native API canonicalizes
`/proc/self/exe` on each call, even when its symbol lists are already cached.
Replacing the executable while the process remains alive therefore makes later
lookups throw. The system coredump summary used a replacement executable and
misidentified a crash-reporter frame as animation reset; use the matching binary.

## Repair

- Resolve the overview strip renderer during controller initialization and keep
  its address on the controller. Rendering does not perform this lazy lookup.
- Route all Hymission native symbol queries through `hooks::findFunctions`.
  Standard exceptions return an empty match list. Existing missing-symbol paths
  disable the hook or skip the snapshot, rather than unwinding the event loop.
- Do not resolve against the replacement binary or reuse addresses across plugin
  instances.

## Offline regression

`hymission-function-lookup-test` checks successful lookup and standard-exception
fallback. `hymission-deleted-executable-lookup-test` runs a temporary executable
copy, unlinks that copy, and calls `canonical("/proc/self/exe")`. It verifies that
this exact filesystem error is caught and returns an empty result.

Both tests are registered with CMake and Meson. This is an offline regression,
not live compositor acceptance. No plugin reload is part of the test.
