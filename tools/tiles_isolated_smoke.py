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
    ap.add_argument("--asan", action="store_true")
    ap.add_argument("--symbol-lookup-failure", action="store_true", help="private child only: force an earlier failed dlsym in update lookup")
    ap.add_argument("--gbm-backend", default="nvidia-drm", help="GBM backend for the isolated parent/child only")
    ap.add_argument("--fixture")
    ap.add_argument("--integration", action="store_true")
    ap.add_argument("--default-off-regression", action="store_true")
    ap.add_argument("--top-clip-regression", action="store_true")
    ap.add_argument("--animation-regression", action="store_true")
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
               GSETTINGS_BACKEND="memory", HYPRLAND_NO_SD_VARS="1", HYPRLAND_NO_SD_TARGET="1", HYPRLAND_NO_SD_NOTIFY="1", HYPRLAND_NO_RT="1",
               AQ_DRM_DEVICES=str(root / "no-drm-device"), LIBSEAT_BACKEND="seatd", SEATD_SOCK=str(root / "no-seatd.sock"),
               LIBGL_ALWAYS_SOFTWARE="0")
    config = root / "hyprland.lua"
    config.write_text('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1"})\n'
                      'hl.config({misc={disable_hyprland_logo=true,disable_splash_rendering=true},animations={enabled=false},xwayland={enabled=false},debug={enable_stdout_logs=true}})\n'
                      f'hl.plugin.load({json.dumps(str(Path(args.plugin).resolve()))})\n')
    if args.fixture:
        with config.open("a") as output:
            output.write(f"hl.plugin.load({json.dumps(str(Path(args.fixture).resolve()))})\n")
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
        if args.symbol_lookup_failure:
            lookup_shim=root/"symbol-lookup-test-shim.so"
            subprocess.run(["cc","-O2","-shared","-fPIC",str(Path(__file__).with_name("tiles_symbol_lookup_test_shim.c")),"-ldl","-o",str(lookup_shim)],check=True)
            env["LD_PRELOAD"]=str(lookup_shim)+":"+env["LD_PRELOAD"]
        if args.asan:
            env["LD_PRELOAD"]=subprocess.check_output(["gcc","-print-file-name=libasan.so"],text=True).strip()+":"+env["LD_PRELOAD"]
            env["ASAN_OPTIONS"]="detect_leaks=0:abort_on_error=1:log_path="+str(root/"asan")
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
                state = json.loads(request("j/hymission-stage-state" if args.default_off_regression else "j/hymission-tiles-state"))
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
        if args.animation_regression:
            from tiles_animation_regression import run
            run(request,root,env,runtime,processes,handles)
            (root/"configerrors.txt").write_text(request("/configerrors"))
            return
        if args.top_clip_regression:
            from tiles_top_clip_regression import run
            run(request,root,env,runtime,processes,handles)
            (root/"configerrors.txt").write_text(request("/configerrors"))
            return
        if args.default_off_regression:
            from tiles_disabled_regression import run
            run(request,root,env,runtime,processes,handles)
            return
        if args.fixture:
            report=root/"update-symbols.json"
            result=request('/eval hl.plugin.hymission_tiles_test.symbol_report("update",'+json.dumps(str(report))+')')
            assert result.strip()=="ok",result
        assert not state["enabled"], state
        result = request("/eval hl.config({plugin={hymission={tiles_enabled=1,tile_min_width=120}}})")
        assert result.strip() == "ok", result
        if args.fixture:
            # With capability enabled but a single tile, native actions and
            # reconcile must leave the real workspace animation running.
            assert request('/eval hl.plugin.hymission_tiles_test.refresh()').strip()=="ok"
            for _ in range(100):
                if json.loads(request("j/hymission-tiles-state"))["enabled"]: break
                time.sleep(0.03)
            else:
                failed=json.loads(request("j/hymission-tiles-state"))
                (root/"tiles-enable-failure.json").write_text(json.dumps(failed,indent=2))
                raise AssertionError(("single-tile capability did not enable",failed))
            code='hl.config({animations={enabled=true}}); hl.dispatch(hl.dsp.focus({workspace=99})); local a=hl.plugin.hymission_tiles_test.workspace_animation("99"); assert(a.animated and math.abs(a.x)>1)'
            assert request('/eval '+code).strip()=="ok"
            time.sleep(0.03)
            assert request('/eval assert(hl.plugin.hymission_tiles_test.workspace_animation("99").animated)').strip()=="ok"
            assert request('/eval hl.dispatch(hl.dsp.focus({workspace=1})); hl.config({animations={enabled=false}})').strip()=="ok"
        for _ in range(2):
            result = request("/eval hl.plugin.hymission.tile_add()")
            assert result.strip() == "ok", (result, request("j/hymission-tiles-state"), request("j/monitors"))
        state = json.loads(request("j/hymission-tiles-state"))
        assert state["enabled"] and not state["initError"] and len(state["tiles"]) == 3, state
        def evaluate(code):
            result=request("/eval " + code)
            assert result.strip()=="ok", (code,result)
        def tiles_state():
            return json.loads(request("j/hymission-tiles-state"))["tiles"]
        original_bindings=[t["activeWorkspace"]["id"] for t in state["tiles"]]
        evaluate('hl.dispatch(hl.dsp.focus({workspace=1}))')
        assert [t["activeWorkspace"]["id"] for t in tiles_state()]==original_bindings
        assert tiles_state()[0]["focused"]
        evaluate('hl.dispatch(hl.dsp.focus({workspace=2,on_current_monitor=true}))')
        assert [t["activeWorkspace"]["id"] for t in tiles_state()]==[2,1,3],tiles_state()
        if args.fixture:
            evaluate('assert(hl.plugin.hymission_tiles_test.change_workspace("98",true)==0); assert(hl.plugin.hymission_tiles_test.change_workspace("2",false)==1)')
            evaluate('hl.config({animations={enabled=true}})')
        evaluate('hl.dispatch(hl.dsp.focus({workspace=4}))')
        assert [t["activeWorkspace"]["id"] for t in tiles_state()]==[4,1,3]
        if args.fixture:
            evaluate('local a=hl.plugin.hymission_tiles_test.workspace_animation("4"); assert(a.animated and math.abs(a.x)>1)')
            assert all(t["renderOffset"]==[0,0] for t in tiles_state()[1:])
            evaluate('hl.config({animations={enabled=false}})')
        evaluate('hl.dispatch(hl.dsp.focus({workspace="previous"}))')
        assert [t["activeWorkspace"]["id"] for t in tiles_state()]==[2,1,3],tiles_state()
        evaluate('hl.plugin.hymission.tile_focus("right")')
        evaluate('hl.plugin.hymission.tile_focus("right")')
        if args.fixture:
            evaluate('hl.config({gestures={workspace_swipe_distance=300,workspace_swipe_min_speed_to_force=0}})')
            prior=tiles_state()
            evaluate('hl.plugin.hymission_tiles_test.swipe("begin")')
            evaluate('hl.plugin.hymission_tiles_test.swipe("update",100)')
            progress=tiles_state()
            assert progress[0]["renderOffset"]==[0,0] and progress[1]["renderOffset"]==[0,0]
            assert progress[2]["renderOffset"][0]<0
            assert abs(progress[2]["renderOffset"][0]+prior[2]["width"]/3)<0.01
            evaluate('hl.plugin.hymission_tiles_test.swipe("cancel")')
            assert [t["activeWorkspace"] for t in tiles_state()]==[t["activeWorkspace"] for t in prior]
            assert all(t["renderOffset"]==[0,0] for t in tiles_state())
            evaluate('hl.plugin.hymission_tiles_test.swipe("begin")')
            evaluate('hl.plugin.hymission_tiles_test.swipe("update",300)')
            evaluate('hl.plugin.hymission_tiles_test.swipe("end")')
            assert [t["activeWorkspace"] for t in tiles_state()][:2]==[t["activeWorkspace"] for t in prior][:2]
            assert tiles_state()[2]["activeWorkspace"]!=prior[2]["activeWorkspace"]
            before_drag=tiles_state()
            boundary=before_drag[1]["x"]
            y=before_drag[0]["y"]+before_drag[0]["height"]*0.5
            evaluate(f'assert(hl.plugin.hymission_tiles_test.pointer("press",{boundary},{y}))')
            evaluate('assert(not hl.plugin.hymission_tiles_test.swipe("try_begin"))')
            evaluate(f'hl.plugin.hymission_tiles_test.pointer("move",{boundary+30},{y})')
            assert abs(tiles_state()[0]["width"]-before_drag[0]["width"]-30)<0.01
            evaluate('assert(hl.plugin.hymission_tiles_test.pointer("escape"))')
            evaluate(f'assert(hl.plugin.hymission_tiles_test.pointer("release",{boundary+30},{y}))')
            assert [t["width"] for t in tiles_state()]==[t["width"] for t in before_drag]
            evaluate('hl.config({gestures={workspace_swipe_forever=1}})')
            before_forever=tiles_state()
            evaluate('hl.plugin.hymission_tiles_test.swipe("begin")')
            evaluate('hl.plugin.hymission_tiles_test.swipe("update",300)')
            assert [t["activeWorkspace"] for t in tiles_state()][:2]==[t["activeWorkspace"] for t in before_forever][:2]
            assert tiles_state()[2]["activeWorkspace"]!=before_forever[2]["activeWorkspace"]
            evaluate('hl.plugin.hymission_tiles_test.swipe("end")')
            evaluate('hl.config({gestures={workspace_swipe_forever=0,workspace_swipe_direction_lock=1,workspace_swipe_direction_lock_threshold=10}})')
            before_lock=tiles_state()
            evaluate('hl.plugin.hymission_tiles_test.swipe("begin")')
            evaluate('hl.plugin.hymission_tiles_test.swipe("update",100)')
            evaluate('hl.plugin.hymission_tiles_test.swipe("update",-100)')
            evaluate('hl.plugin.hymission_tiles_test.swipe("end")')
            assert [t["activeWorkspace"] for t in tiles_state()]==[t["activeWorkspace"] for t in before_lock]
            evaluate('hl.config({gestures={workspace_swipe_direction_lock=0}})')
        state=json.loads(request("j/hymission-tiles-state"))
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
        if args.integration:
            assert args.fixture, "integration needs the private input fixture"
            def wait_json(command, predicate, label):
                for _ in range(100):
                    value=json.loads(request(command))
                    if predicate(value): return value
                    time.sleep(0.05)
                (root / (label + ".json")).write_text(json.dumps(value,indent=2))
                raise AssertionError((label,value))
            evaluate('hl.plugin.hymission.tile_remove()')
            assert len(tiles_state())==1
            # Retired animation leases cannot survive collapse or disabling.
            for collapse in ("remove","disable"):
                evaluate('hl.plugin.hymission.tile_add(); hl.config({animations={enabled=true}}); hl.plugin.hymission_tiles_test.change_workspace("198",false)')
                lease_state=json.loads(request("j/hymission-tiles-state"))
                assert lease_state["monitors"][0]["outgoing_workspace_count"]>0
                if collapse=="remove":
                    evaluate('hl.plugin.hymission.tile_remove()')
                    lease_state=json.loads(request("j/hymission-tiles-state"))
                    assert len(lease_state["tiles"])==1 and lease_state["monitors"][0]["outgoing_workspace_count"]==0
                else:
                    evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
                    wait_json("j/hymission-tiles-state",lambda s:not s["enabled"] and not s["monitors"],"disabled-outgoing-released")
                    evaluate('hl.config({plugin={hymission={tiles_enabled=1}}}); hl.plugin.hymission_tiles_test.refresh()')
                    wait_json("j/hymission-tiles-state",lambda s:s["enabled"] and len(s["tiles"])==1,"outgoing-reenabled")
                evaluate('hl.config({animations={enabled=false}})')
            client_env=env.copy()
            client_env.pop("LD_PRELOAD",None)
            child_displays=[p for p in runtime.glob("wayland-*") if not p.name.endswith(".lock") and p.is_socket()]
            assert len(child_displays)==1, child_displays
            client_env["WAYLAND_DISPLAY"]=child_displays[0].name
            xml=Path(subprocess.check_output(["pkg-config","--variable=pkgdatadir","wayland-protocols"],text=True).strip())/"staging/ext-workspace/ext-workspace-v1.xml"
            subprocess.run(["wayland-scanner","client-header",str(xml),str(root/"ext-workspace-client.h")],check=True)
            subprocess.run(["wayland-scanner","private-code",str(xml),str(root/"ext-workspace-protocol.c")],check=True)
            flags=subprocess.check_output(["pkg-config","--cflags","--libs","wayland-client"],text=True).split()
            ext_client=root/"ext-workspace-client"
            subprocess.run(["cc",str(Path(__file__).with_name("tiles_ext_workspace_client.c")),str(root/"ext-workspace-protocol.c"),"-I"+str(root),*flags,"-o",str(ext_client)],check=True)
            def capture(name):
                try:
                    result=subprocess.run(["grim","-o",monitors[0]["name"],str(root/(name+".png"))],env=client_env,capture_output=True,text=True,timeout=8)
                    if result.returncode: (root/(name+"-capture-error.txt")).write_text(result.stderr)
                except (OSError,subprocess.TimeoutExpired) as error:
                    (root/(name+"-capture-error.txt")).write_text(str(error))
            for _ in range(3):
                client_log=open(root / f"client-{_}.log","w"); handles.append(client_log)
                processes.append(subprocess.Popen(["weston-simple-shm"],env=client_env,stdout=client_log,stderr=subprocess.STDOUT))
                wait_json("j/clients",lambda clients:len(clients)==_+1,"clients")
            evaluate('hl.config({plugin={hymission={stage_enabled=1,stage_transition_ms=0,stage_refresh_ms=16,workspace_strip_force_show=1,workspace_strip_empty_mode="always"}}})')
            evaluate('hl.plugin.hymission.stage_open()')
            def stage_ready(count):
                return wait_json("j/hymission-stage-state",lambda s:s["enabled"] and s["hooks_ready"] and s["visibility_progress"]==1 and len(s["screens"])==count and any(c.get("windows") for sc in s["screens"] for c in sc["cards"]),"stage-ready")
            def pointer(action,x=0,y=0):
                evaluate(f'hl.plugin.hymission_tiles_test.pointer("{action}",{x},{y})')
            def source_preview(state,address=None):
                return next((sc,c,w) for sc in state["screens"] for c in sc["cards"] for w in c.get("windows",[]) if (address is None or w["address"]==address))
            stage=stage_ready(1)
            # Same-column actions retain their original Stage paths. Keep an
            # empty active card beside a populated hidden stage for reordering.
            original_ws=tiles_state()[0]["activeWorkspace"]["id"]
            evaluate(f'hl.workspace_rule({{workspace="{original_ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace=77}})); hl.plugin.hymission_tiles_test.refresh()')
            stage=wait_json("j/hymission-stage-state",lambda s:len(s["screens"])==1 and len(s["screens"][0]["cards"])>=2 and any(c.get("windows") for c in s["screens"][0]["cards"]),"same-column-cards")
            time.sleep(.15)
            stage=json.loads(request("j/hymission-stage-state"))
            (root/"same-column-before.json").write_text(json.dumps(stage,indent=2))
            sc,c,w=source_preview(stage)
            # Stage card activation on a managed single tile must retain the
            # native changeWorkspace animation, just like typed actions.
            sx=w["x"]+w["width"]/2; sy=w["y"]+w["height"]/2
            evaluate(f'hl.config({{animations={{enabled=true}},plugin={{hymission={{stage_transition_ms=220}}}}}}); hl.plugin.hymission_tiles_test.pointer("press",{sx},{sy}); hl.plugin.hymission_tiles_test.pointer("release",{sx},{sy}); local a=hl.plugin.hymission_tiles_test.workspace_animation("{original_ws}"); assert(a.animated and math.abs(a.x)>1)')
            wait_json("j/activeworkspace",lambda ws:ws["id"]==original_ws,"single-tile-stage-native-click")
            time.sleep(.03)
            evaluate(f'assert(hl.plugin.hymission_tiles_test.workspace_animation("{original_ws}").animated)')
            assert all(sc["transition_windows"]==0 for sc in json.loads(request("j/hymission-stage-state"))["screens"])
            evaluate('hl.config({animations={enabled=false},plugin={hymission={stage_transition_ms=0}}}); hl.dispatch(hl.dsp.focus({workspace=77})); hl.plugin.hymission_tiles_test.refresh()')
            stage=wait_json("j/hymission-stage-state",lambda s:len(s["screens"])==1 and len(s["screens"][0]["cards"])>=2 and any(c.get("windows") for c in s["screens"][0]["cards"]),"same-column-after-click")
            time.sleep(.15)
            stage=json.loads(request("j/hymission-stage-state")); sc,c,w=source_preview(stage)
            destination=next(card for card in sc["cards"] if card["workspace"]==77)
            source_members=[v["address"] for v in c["windows"]]
            pointer("press",c["x"]+0.5,c["y"]+0.5)
            assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]==0
            pointer("move",destination["x"]+0.5,destination["y"]+destination["height"]/2)
            reorder=json.loads(request("j/hymission-stage-state"))
            assert reorder["reorder_armed"] and not reorder["tile_transfer_active"]
            pointer("release",destination["x"]+0.5,destination["y"]+destination["height"]/2)
            (root/"same-column-after.json").write_text(request("j/hymission-stage-state"))
            wait_json("j/clients",lambda cs:all(any(v["address"]==address and v["workspace"]["id"]==77 for v in cs) for address in source_members),"same-column-reorder")
            stage=stage_ready(1); sc,c,w=source_preview(stage)
            before_meta=tiles_state()
            evaluate('hl.plugin.hymission_tiles_test.meta(true)')
            sx=w["x"]+w["width"]/2; sy=w["y"]+w["height"]/2
            pointer("press",sx,sy)
            meta_state=json.loads(request("j/hymission-stage-state"))
            assert meta_state["thumbnail_drag_active"] and not meta_state["tile_transfer_active"]
            # Keep Meta thumbnail dragging native even across a split target,
            # including after Meta is released while the thumbnail is held.
            pointer("move",expected_width*.8,300)
            meta_state=json.loads(request("j/hymission-stage-state"))
            assert meta_state["thumbnail_drag_active"] and not meta_state["tile_transfer_active"]
            evaluate('hl.plugin.hymission_tiles_test.meta(false)')
            pointer("move",expected_width*.85,320)
            meta_state=json.loads(request("j/hymission-stage-state"))
            assert meta_state["thumbnail_drag_active"] and not meta_state["tile_transfer_active"]
            pointer("release",expected_width*.85,320)
            target_ws=before_meta[0]["activeWorkspace"]["id"]
            wait_json("j/clients",lambda cs:any(v["address"]==w["address"] and v["workspace"]["id"]==target_ws for v in cs),"meta-native-desktop-drop")
            assert tiles_state()==before_meta
            assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]==0
            stage=stage_ready(1)
            (root / "stage-single.json").write_text(json.dumps(stage,indent=2))
            capture("stage-single")
            def drag_from(sc,c,w,x,y,cancel=False):
                sx=w["x"]+w["width"]/2; sy=w["y"]+w["height"]/2
                pointer("press",sx,sy); pointer("move",x,y)
                assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]!=0
                if cancel:
                    capture("split-preview")
                    pointer("escape")
                pointer("release",x,y)
            sc,c,w=source_preview(stage)
            before=tiles_state()
            sx=w["x"]+w["width"]/2; sy=w["y"]+w["height"]/2
            pointer("press",sx,sy); pointer("move",expected_width*0.8,300)
            assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]!=0
            evaluate('hl.plugin.hymission.open("forceall")')
            wait_json("j/hymission-tiles-state",lambda s:s["interaction_owner"]==0,"overview-cancels-stage-drag")
            pointer("release",expected_width*0.8,300)
            assert tiles_state()==before,(before,tiles_state())
            evaluate('hl.plugin.hymission.close()')
            wait_json("j/hymission-overview-state",lambda s:not s["active"],"close-after-stage-drag")
            stage=stage_ready(1); sc,c,w=source_preview(stage)
            drag_from(sc,c,w,expected_width*0.8,300,True)
            assert tiles_state()==before,(before,tiles_state())
            stage=stage_ready(1); sc,c,w=source_preview(stage)
            sx=w["x"]+w["width"]/2; sy=w["y"]+w["height"]/2
            pointer("press",sx,sy); pointer("move",expected_width*0.8,300); pointer("move",sx,sy); pointer("release",sx,sy)
            assert tiles_state()==before,(before,tiles_state())
            drag_from(sc,c,w,expected_width*0.8,300)
            wait_json("j/hymission-tiles-state",lambda s:len(s["tiles"])==2,"split-two")
            stage=stage_ready(2)
            (root / "stage-two.json").write_text(json.dumps(stage,indent=2))
            # Drag one of the remaining source windows into a new half of its own tile.
            source_tile=next(t for t in tiles_state() if t["id"]==sc["tile_id"])
            remaining=next((a,b,v) for a in stage["screens"] if a["tile_id"]==sc["tile_id"] for b in a["cards"] for v in b.get("windows",[]))
            drag_from(*remaining,source_tile["x"]+source_tile["width"]*0.8,340)
            wait_json("j/hymission-tiles-state",lambda s:len(s["tiles"])==3,"split-three")
            stage=stage_ready(3)
            columns=sorted(stage["screens"],key=lambda sc:next(t["x"] for t in tiles_state() if t["id"]==sc["tile_id"]))
            assert all(sc["sidebar_side"]=="left" for sc in columns)
            assert abs(columns[0]["sidebar"]["x"]-monitors[0]["x"])<1
            for a,b in zip(columns,columns[1:]):
                assert abs(a["sidebar"]["x"]+a["sidebar"]["width"]-b["sidebar"]["x"])<1
            assert columns[-1]["sidebar"]["x"]+columns[-1]["sidebar"]["width"]<expected_width*.4
            (root / "stage-three.json").write_text(json.dumps(stage,indent=2))
            capture("stage-three")
            # Stage's adjacent column must yield divider input to the shared owner.
            before=tiles_state()
            seam=before[0]["x"]+before[0]["width"]
            pointer("press",seam,500); pointer("move",seam+30,500)
            assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]!=0
            pointer("escape"); pointer("release",seam+30,500)
            assert tiles_state()==before,(before,tiles_state())
            # A native directional focus request crosses a one-window tile edge.
            clients=json.loads(request("j/clients"))
            sole=next((i,t,[w for w in clients if w["workspace"]["id"]==t["activeWorkspace"]["id"]][0])
                      for i,t in enumerate(before) if sum(w["workspace"]["id"]==t["activeWorkspace"]["id"] for w in clients)==1)
            i,t,client=sole
            direction="right" if i+1<len(before) else "left"
            neighbor=before[i+(1 if direction=="right" else -1)]
            evaluate(f'hl.plugin.hymission_tiles_test.window("focus","{client["address"]}")')
            evaluate(f'hl.dispatch(hl.dsp.focus({{direction="{direction}"}}))')
            assert next(v["id"] for v in tiles_state() if v["focused"])==neighbor["id"]
            evaluate(f'hl.plugin.hymission_tiles_test.window("focus","{client["address"]}")')
            evaluate(f'hl.plugin.hymission.tile_move_window("{direction}")')
            wait_json("j/clients",lambda cs:any(w["address"]==client["address"] and w["workspace"]["id"]==neighbor["activeWorkspace"]["id"] for w in cs),"api-move-neighbor")
            opposite="left" if direction=="right" else "right"
            evaluate(f'hl.plugin.hymission.tile_move_window("{opposite}")')
            wait_json("j/clients",lambda cs:any(w["address"]==client["address"] and w["workspace"]["id"]==t["activeWorkspace"]["id"] for w in cs),"api-move-return")
            stage=stage_ready(3)
            sc,c,w=source_preview(stage)
            target=next(a for a in stage["screens"] if a["tile_id"]!=sc["tile_id"])
            target_card=next(b for b in target["cards"] if b["workspace"]==target["active_workspace"])
            # Column-to-column move; the source must cease owning the moved client.
            drag_from(sc,c,w,target_card["x"]+target_card["width"]/2,target_card["y"]+target_card["height"]/2)
            clients=wait_json("j/clients",lambda cs:any(v["address"]==w["address"] and v["workspace"]["id"]==target_card["workspace"] for v in cs),"cross-column")
            assert len(tiles_state())==3
            # At the limit a blank part of a stage card moves the entire stage.
            stage=stage_ready(3)
            group_screen=next(a for a in stage["screens"] if any(len(b.get("windows",[]))>1 for b in a["cards"]))
            group_card=next(b for b in group_screen["cards"] if len(b.get("windows",[]))>1)
            group_members=[v["address"] for v in group_card["windows"]]
            dest=next(t for t in tiles_state() if t["id"]!=group_screen["tile_id"])
            pointer("press",group_card["x"]+0.5,group_card["y"]+0.5)
            pointer("move",dest["x"]+dest["width"]*0.7,400)
            assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]!=0
            pointer("release",dest["x"]+dest["width"]*0.7,400)
            wait_json("j/clients",lambda cs:all(any(v["address"]==address and v["workspace"]["id"]==dest["activeWorkspace"]["id"] for v in cs) for address in group_members),"stage-group-move")
            assert len(tiles_state())==3
            # A new native client belongs to the tile focused by the committed drop.
            # Closing a real source client mid-transfer must release ownership.
            close_log=open(root/"client-close-drag.log","w"); handles.append(close_log)
            closing_client=subprocess.Popen(["weston-simple-shm"],env=client_env,stdout=close_log,stderr=subprocess.STDOUT); processes.append(closing_client)
            cs=wait_json("j/clients",lambda cs:len(cs)==4,"close-drag-client")
            close_address=next(v["address"] for v in cs if v["pid"]==closing_client.pid)
            close_stage=wait_json("j/hymission-stage-state",lambda s:any(w["address"]==close_address for sc in s["screens"] for c in sc["cards"] for w in c.get("windows",[])),"close-drag-preview")
            a,b,v=source_preview(close_stage,close_address)
            target=next(t for t in tiles_state() if t["id"]!=a["tile_id"])
            dx=target["x"]+target["width"]*.7
            pointer("press",v["x"]+v["width"]/2,v["y"]+v["height"]/2); pointer("move",dx,400)
            assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]==4
            closing_client.terminate()
            wait_json("j/clients",lambda cs:len(cs)==3,"source-closed")
            wait_json("j/hymission-tiles-state",lambda s:s["interaction_owner"]==0,"source-close-released")
            pointer("release",dx,400)
            expected_ws=next(t["activeWorkspace"]["id"] for t in tiles_state() if t["focused"])
            client_log=open(root/"client-new.log","w"); handles.append(client_log)
            processes.append(subprocess.Popen(["weston-simple-shm"],env=client_env,stdout=client_log,stderr=subprocess.STDOUT))
            clients=wait_json("j/clients",lambda cs:len(cs)==4,"new-client")
            new_client=next(v for v in clients if v["pid"]==processes[-1].pid)
            assert new_client["workspace"]["id"]==expected_ws,(new_client,tiles_state())
            address=new_client["address"]
            # A real ext-workspace protocol client reaches the same monitor
            # change boundary as native actions, including nonfocused tiles.
            before_protocol=tiles_state()
            target=next(t for t in before_protocol if not t["focused"])
            subprocess.run([str(ext_client),target["activeWorkspace"]["name"]],env=client_env,check=True,timeout=5)
            assert next(t["id"] for t in tiles_state() if t["focused"])==target["id"]
            assert [t["activeWorkspace"] for t in tiles_state()]==[t["activeWorkspace"] for t in before_protocol]
            def operation(action): evaluate(f'hl.plugin.hymission_tiles_test.window("{action}","{address}")')
            operation("focus")
            native_bindings=[t["activeWorkspace"] for t in tiles_state()]
            target_tile=next(t for t in tiles_state() if t["activeWorkspace"]["id"]==new_client["workspace"]["id"])
            for mode in ("fullscreen","maximize"):
                operation(mode)
                def inside(cs):
                    w=next(v for v in cs if v["address"]==address)
                    return w["fullscreen"]>0 and w["at"][0]>=target_tile["x"]-1 and w["at"][0]+w["size"][0]<=target_tile["x"]+target_tile["width"]+1 and w["size"][0]>0
                fullscreen_clients=wait_json("j/clients",inside,mode+"-geometry")
                assert [t["activeWorkspace"] for t in tiles_state()]==native_bindings
                (root/(mode+"-clients.json")).write_text(json.dumps(fullscreen_clients,indent=2))
                capture(mode)
                operation("normal")
            evaluate(f'hl.workspace_rule({{workspace="{target_tile["activeWorkspace"]["id"]}",layout="scrolling"}}); hl.plugin.hymission_tiles_test.refresh()')
            wait_json("j/workspaces",lambda workspaces:any(ws.get("id")==target_tile["activeWorkspace"]["id"] and ws["tiledLayout"]=="scrolling" for ws in workspaces),"scrolling-layout")
            for mode in ("fullscreen","maximized"):
                evaluate(f'hl.plugin.hymission.fullscreen({{mode="{mode}",action="set"}})')
                wait_json("j/clients",inside,"scrolling-"+mode)
                operation("normal")
            (root/"scrolling-state.json").write_text(request("j/hymission-tiles-state"))
            operation("pin")
            pinned_before=next(v for v in json.loads(request("j/clients")) if v["address"]==address)
            direction="left" if tiles_state()[-1]["focused"] else "right"
            evaluate(f'hl.plugin.hymission.tile_focus("{direction}")')
            pinned_after=next(v for v in json.loads(request("j/clients")) if v["address"]==address)
            assert pinned_after["pinned"] and pinned_after["at"]==pinned_before["at"]
            operation("unpin"); operation("focus")
            before_special=[t["activeWorkspace"] for t in tiles_state()]
            evaluate('hl.plugin.hymission_tiles_test.special(true)')
            assert [t["activeWorkspace"] for t in tiles_state()]==before_special
            result=request('/eval hl.plugin.hymission.tile_add()')
            assert "special" in result,result
            evaluate('hl.plugin.hymission_tiles_test.special(false)')
            assert [t["activeWorkspace"] for t in tiles_state()]==before_special
            # All overview is unified; only workspace overview confines windows to tiles.
            for finish in ("close","escape"):
                evaluate('hl.plugin.hymission.open("forceall")')
                drag_state=wait_json("j/hymission-overview-state",lambda s:s["active"] and s["phase"]=="active" and not s.get("relayout_active",False) and s["windows"],"overview-drag-ready")
                b=drag_state["windows"][0]["contentGeometry"]
                x=b["x"]+b["width"]/2; y=b["y"]+b["height"]/2
                pointer("press",x,y); pointer("move",x+35,y+35)
                assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]==3
                if finish=="close": evaluate('hl.plugin.hymission.close()')
                else: pointer("escape")
                wait_json("j/hymission-tiles-state",lambda s:s["interaction_owner"]==0,"overview-drag-released")
                pointer("release",x+35,y+35)
                if finish=="escape": evaluate('hl.plugin.hymission.close()')
                wait_json("j/hymission-overview-state",lambda s:not s["active"],"overview-drag-closed")
            for anchor,scope in (("left","all"),("top","forceall")):
                evaluate(f'hl.config({{plugin={{hymission={{workspace_strip_anchor="{anchor}"}}}}}})')
                evaluate(f'hl.plugin.hymission.open("{scope}")')
                ov=wait_json("j/hymission-overview-state",lambda s:s["active"] and s["phase"]=="active" and not s.get("relayout_active",False),"overview")
                assert ov["window_layout_scope"]=="all" and not ov["tile_groups"],ov
                grouped={t["id"]:[e for e in ov["strip"] if e["tile_id"]==t["id"]] for t in tiles_state()}
                previous_right=-1
                for t in tiles_state():
                    entries=grouped[t["id"]]
                    assert entries and len({round(e["geometry"]["x"],3) for e in entries})==1
                    left=min(e["geometry"]["x"] for e in entries); right=max(e["geometry"]["x"]+e["geometry"]["width"] for e in entries)
                    assert left>previous_right and right<(max(t["x"]+t["width"] for t in tiles_state())-min(t["x"] for t in tiles_state()))*.31,(left,right,previous_right,ov["strip"])
                    previous_right=right
                (root / ("overview-"+anchor+".json")).write_text(json.dumps(ov,indent=2))
                capture("overview-"+anchor)
                assert {e["tile_id"] for e in ov["strip"]}=={t["id"] for t in tiles_state()}
                for tile in tiles_state():
                    entries=[e for e in ov["strip"] if e["tile_id"]==tile["id"]]
                    if len(entries)>1: assert len({round(e["geometry"]["y"],2) for e in entries})>1
                by_id={t["id"]:t for t in tiles_state()}
                crossing=[]
                output_left=min(t["x"] for t in tiles_state());output_right=max(t["x"]+t["width"] for t in tiles_state())
                for win in ov["windows"]:
                    t=by_id[win["tile_id"]]; b=win["contentGeometry"]
                    assert b["x"]>=output_left-1 and b["x"]+b["width"]<=output_right+1,(win,t)
                    if b["x"]<t["x"]-1 or b["x"]+b["width"]>t["x"]+t["width"]+1:crossing.append(win)
                assert crossing,(scope,"all layout still confined to home tiles",ov)
                entry=next(e for e in ov["strip"] if not e["active"])
                old={t["id"]:t["activeWorkspace"]["id"] for t in tiles_state()}
                b=entry["geometry"]; pointer("press",b["x"]+b["width"]/2,b["y"]+b["height"]/2); pointer("release",b["x"]+b["width"]/2,b["y"]+b["height"]/2)
                now={t["id"]:t["activeWorkspace"]["id"] for t in tiles_state()}
                assert now[entry["tile_id"]]==entry["workspace"] and all(now[k]==v for k,v in old.items() if k!=entry["tile_id"]),(old,now,entry)
                evaluate('hl.plugin.hymission.close()')
                wait_json("j/hymission-overview-state",lambda s:not s["active"],"overview-closed")
            evaluate('hl.plugin.hymission.open("onlycurrentworkspace")')
            compact=wait_json("j/hymission-overview-state",lambda s:s["active"] and s["phase"]=="active","compact")
            assert compact["window_layout_scope"]=="workspace" and len(compact["tile_groups"])==3
            by_id={t["id"]:t for t in tiles_state()}
            for win in compact["windows"]:
                t=by_id[win["tile_id"]];b=win["contentGeometry"]
                assert b["x"]>=t["x"]-1 and b["x"]+b["width"]<=t["x"]+t["width"]+1,(win,t)
            (root/"overview-workspace.json").write_text(json.dumps(compact,indent=2))
            capture("overview-workspace")
            shown={t["activeWorkspace"]["id"] for t in tiles_state()}
            clients_by_address={v["address"]:v for v in json.loads(request("j/clients"))}
            assert all(clients_by_address[v["address"]]["workspace"]["id"] in shown for v in compact["windows"])
            evaluate('hl.plugin.hymission.close()')
            wait_json("j/hymission-overview-state",lambda s:not s["active"],"compact-closed")
            for client in processes[2:]:
                if client.poll() is None: client.terminate()
            wait_json("j/clients",lambda cs:not cs,"clients-closed")
            evaluate('hl.plugin.hymission.open("onlycurrentworkspace")')
            empty=wait_json("j/hymission-overview-state",lambda s:s["active"] and s["phase"]=="active","empty-strip")
            assert not empty["windows"] and len(empty["tile_groups"])==3 and {e["tile_id"] for e in empty["strip"]}=={t["id"] for t in tiles_state()}
            capture("empty-strip")
            (root/"empty-strip.json").write_text(json.dumps(empty,indent=2))
            evaluate('hl.plugin.hymission.close()')
            wait_json("j/hymission-overview-state",lambda s:not s["active"],"empty-closed")
            # A managed single-tile output must also route requests to a tile
            # on another physical output, including explicit native swaps.
            result=request("/output create wayland TILES-SECOND")
            assert result.strip()=="ok",result
            second_monitors=wait_json("j/monitors",lambda ms:len(ms)==2 and all(m["width"]>0 and m["activeWorkspace"]["id"]>0 for m in ms),"second-output")
            (root/"second-monitors.json").write_text(json.dumps(second_monitors,indent=2))
            topology=wait_json("j/hymission-tiles-state",lambda state:len(state["tiles"])==4,"second-topology")
            primary=[t for t in topology["tiles"] if t["monitor"]==monitors[0]["name"]]
            secondary=next(t for t in topology["tiles"] if t["monitor"]=="TILES-SECOND")
            evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={primary[-1]["activeWorkspace"]["id"]}}}))')
            evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={secondary["activeWorkspace"]["id"]}}}))')
            assert next(t for t in tiles_state() if t["focused"])["monitor"]=="TILES-SECOND"
            old_secondary=secondary["activeWorkspace"]["id"]
            requested=primary[0]
            evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={requested["activeWorkspace"]["id"]},on_current_monitor=true}}))')
            swapped=tiles_state()
            assert next(t for t in swapped if t["monitor"]=="TILES-SECOND")["activeWorkspace"]["id"]==requested["activeWorkspace"]["id"],swapped
            assert next(t for t in swapped if t["monitor"]==requested["monitor"] and t["id"]==requested["id"])["activeWorkspace"]["id"]==old_secondary,swapped
            assert all(next(n for n in swapped if n["monitor"]==t["monitor"] and n["id"]==t["id"])["activeWorkspace"]==t["activeWorkspace"] for t in primary[1:]),swapped
            evaluate('hl.workspace_rule({workspace="name:tiles-bound",monitor="TILES-SECOND",persistent=true})')
            evaluate('hl.dispatch(hl.dsp.focus({monitor="'+monitors[0]["name"]+'"}))')
            evaluate('hl.dispatch(hl.dsp.focus({workspace="name:tiles-bound"}))')
            bound=next(t for t in tiles_state() if t["focused"])
            assert bound["monitor"]=="TILES-SECOND" and bound["activeWorkspace"]["address"]=="tiles-bound",bound
            used={ws["id"] for ws in json.loads(request("j/workspaces")) if "id" in ws}
            restricted=next(i for i in range(1,10000) if i not in used)
            evaluate(f'hl.workspace_rule({{workspace="{restricted}",monitor="{monitors[0]["name"]}"}})')
            primary_before={t["id"]:t["activeWorkspace"] for t in tiles_state() if t["monitor"]==monitors[0]["name"]}
            evaluate('hl.plugin.hymission.tile_add()')
            assert next(t for t in tiles_state() if t["focused"])["activeWorkspace"]["id"]!=restricted
            evaluate('hl.plugin.hymission.tile_add()')
            assert len([t for t in tiles_state() if t["monitor"]=="TILES-SECOND"])==3
            assert "three" in request('/eval hl.plugin.hymission.tile_add()')
            assert {t["id"]:t["activeWorkspace"] for t in tiles_state() if t["monitor"]==monitors[0]["name"]}==primary_before
            (root/"two-output-state.json").write_text(request("j/hymission-tiles-state"))
            remove_log=open(root/"client-remove-drag.log","w"); handles.append(remove_log)
            removing_client=subprocess.Popen(["weston-simple-shm"],env=client_env,stdout=remove_log,stderr=subprocess.STDOUT); processes.append(removing_client)
            cs=wait_json("j/clients",lambda cs:len(cs)==1,"remove-drag-client")
            remove_address=cs[0]["address"]
            remove_stage=wait_json("j/hymission-stage-state",lambda s:any(w["address"]==remove_address for sc in s["screens"] for c in sc["cards"] for w in c.get("windows",[])),"remove-drag-preview")
            a,b,v=source_preview(remove_stage,remove_address)
            assert a["monitor"]=="TILES-SECOND"
            target=next(t for t in tiles_state() if t["monitor"]==monitors[0]["name"])
            dx=target["x"]+target["width"]*.7
            pointer("press",v["x"]+v["width"]/2,v["y"]+v["height"]/2); pointer("move",dx,400)
            assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]==4
            result=request("/output remove TILES-SECOND")
            assert result.strip()=="ok",result
            wait_json("j/hymission-tiles-state",lambda state:len(state["monitors"])==1 and len(state["tiles"])==3,"second-removed")
            assert json.loads(request("j/hymission-tiles-state"))["interaction_owner"]==0
            pointer("release",dx,400)
            removing_client.terminate()
            wait_json("j/clients",lambda cs:not cs,"remove-drag-client-closed")
            evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
            disabled=wait_json("j/hymission-tiles-state",lambda s:not s["enabled"],"tiles-disabled")
            assert disabled["tiles"]==[]
            print("PASS: isolated real clients; Stage cancel/return/split/group/column/desktop; overview groups/grids/click/current/empty; fullscreen/maximize/pin/special; two-output focus/swap/binding; disable")
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
