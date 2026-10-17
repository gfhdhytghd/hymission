#!/usr/bin/env python3
"""Run only a private nested compositor. Never discover or contact the host IPC.

Requires weston headless, a matching non-coverage Hyprland, and a built plugin.
Logs and the private config stay in --output for reproducibility.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import signal
import subprocess
import tempfile
import shutil
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hyprland", default=str(Path.home() / ".local/lib/hyprland-hdr/Hyprland"))
    ap.add_argument("--debugger", action="store_true")
    ap.add_argument("--gbm-backend", default="nvidia-drm", help="GBM backend for the isolated parent/child only")
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="hmt-"))
    env = os.environ.copy()
    for key in ("HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "DBUS_SESSION_BUS_ADDRESS", "LD_PRELOAD"):
        env.pop(key, None)
    env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / "config"), XDG_CACHE_HOME=str(root / "cache"),
               HYPRLAND_NO_SD_VARS="1", HYPRLAND_NO_SD_TARGET="1", HYPRLAND_NO_SD_NOTIFY="1", HYPRLAND_NO_RT="1",
               AQ_DRM_DEVICES=str(root / "no-drm-device"), LIBSEAT_BACKEND="seatd", SEATD_SOCK=str(root / "no-seatd.sock"),
               LIBGL_ALWAYS_SOFTWARE="0")
    config = root / "hyprland.lua"
    config.write_text('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1"})\n'
                      'hl.config({misc={disable_hyprland_logo=true,disable_splash_rendering=true},animations={enabled=false},xwayland={enabled=false}})\n'
                      f'hl.plugin.load({json.dumps(str(Path(args.plugin).resolve()))})\n')
    processes = []
    handles = []
    try:
        seat = root / "test-seat.so"
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "libweston-15"], text=True).split()
        subprocess.run(["cc", "-shared", "-fPIC", str(Path(__file__).with_name("tiles_weston_test_seat.c")), *flags, "-o", str(seat)], check=True)
        env["GBM_BACKEND"] = args.gbm_backend
        parent_log = open(root / "weston.log", "w")
        handles.append(parent_log)
        parent = subprocess.Popen(["weston", "--backend=headless", "--renderer=gl", "--width=1920", "--height=1080",
                                   "--socket=tiles-parent", "--idle-time=0", "--no-config", "--modules=" + str(seat)], env=env, stdout=parent_log, stderr=subprocess.STDOUT)
        processes.append(parent)
        for _ in range(100):
            if (runtime / "tiles-parent").exists():
                break
            if parent.poll() is not None:
                raise RuntimeError("headless parent failed; see weston.log")
            time.sleep(0.1)
        env["WAYLAND_DISPLAY"] = "tiles-parent"
        shim = root / "parent-protocol-shim.so"
        flags = subprocess.check_output(["pkg-config", "--cflags", "wayland-client"], text=True).split()
        subprocess.run(["cc", "-shared", "-fPIC", str(Path(__file__).with_name("tiles_wayland_test_shim.c")), *flags, "-ldl", "-o", str(shim)], check=True)
        env["LD_PRELOAD"] = str(shim)
        log = open(root / "hyprland.log", "w")
        handles.append(log)
        command = [args.hyprland, "--config", str(config)]
        if args.debugger:
            command = ["gdb", "-q", "-batch", "-ex", "set startup-with-shell off", "-ex", "handle SIGUSR1 stop print nopass", "-ex", "run", "-ex", "thread apply all bt 50", "--args", *command]
        child = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        processes.append(child)
        ipc = None
        for _ in range(200):
            sockets = list((runtime / "hypr").glob("*/.socket.sock"))
            if sockets:
                ipc = sockets[0]
                break
            if child.poll() is not None:
                raise RuntimeError("isolated Hyprland failed; see hyprland.log")
            time.sleep(0.1)
        if ipc is None:
            raise RuntimeError("private IPC did not appear")
        assert ipc.resolve().is_relative_to(runtime.resolve())

        def request(command):
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(5)
                sock.connect(str(ipc))
                sock.sendall(command.encode())
                data = b""
                try:
                    while chunk := sock.recv(65536):
                        data += chunk
                except TimeoutError:
                    if args.debugger:
                        descendants = Path(f"/proc/{child.pid}/task/{child.pid}/children").read_text().split()
                        for pid in descendants: os.kill(int(pid), signal.SIGUSR1)
                        child.wait(timeout=15)
                    raise
            return data.decode()

        for _ in range(100):
            try:
                state = json.loads(request("j/hymission-tiles-state"))
                break
            except (ValueError, OSError):
                time.sleep(0.1)
        else:
            raise RuntimeError("plugin not ready")
        for _ in range(100):
            monitors = json.loads(request("j/monitors"))
            if monitors and all(m.get("width",0)>0 and m.get("height",0)>0 for m in monitors):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("nested output did not appear")
        expected_width = monitors[0]["width"] / monitors[0].get("scale", 1)
        assert not state["enabled"], state
        result = request("/eval hl.config({plugin={hymission={tiles_enabled=1,tile_min_width=120}}})")
        assert result.strip() == "ok", result
        for _ in range(2):
            result = request("/eval hl.plugin.hymission.tile_add()")
            assert result.strip() == "ok", (result, request("j/hymission-tiles-state"), request("j/monitors"))
        state = json.loads(request("j/hymission-tiles-state"))
        assert state["enabled"] and not state["initError"] and len(state["tiles"]) == 3, state
        before = state["tiles"]
        result = request("/eval hl.plugin.hymission.tile_add()")
        assert "three" in result, result
        assert json.loads(request("j/hymission-tiles-state"))["tiles"] == before
        result = request('/eval hl.plugin.hymission.tile_focus("left")')
        assert result.strip() == "ok", result
        result = request('/eval hl.plugin.hymission.tile_resize({delta="+20px"})')
        assert result.strip() == "ok", result
        state = json.loads(request("j/hymission-tiles-state"))
        assert len(state["tiles"]) == 3
        assert abs(sum(t["width"] for t in state["tiles"]) - expected_width) < 1e-5, state
        result = request('/eval hl.plugin.hymission.tile_remove()')
        assert result.strip() == "ok", result
        assert len(json.loads(request("j/hymission-tiles-state"))["tiles"]) == 2
        (root / "state.json").write_text(request("j/hymission-tiles-state"))
        (root / "configerrors.txt").write_text(request("/configerrors"))
        print("PASS: private nested compositor, default-off, add/max-three/focus/resize/remove and state geometry")
    finally:
        for proc in reversed(processes):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
        for handle in handles:
            handle.close()
        if (runtime / "hypr").exists():
            for source in (runtime / "hypr").rglob("*"):
                if source.is_file():
                    destination = root / "ipc-logs" / source.relative_to(runtime / "hypr")
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)
        shutil.rmtree(runtime)


if __name__ == "__main__":
    main()
