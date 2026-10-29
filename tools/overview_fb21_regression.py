"""Private device-level all -> desktop -> workspace gesture observations (FB21)."""
import json
import subprocess
import time
from pathlib import Path
from PIL import Image


def run(mode, observe, request, root, env, runtime, processes, handles, layout="dwindle", fp16=False, opacity=False, hover_delay=.6, same_workspace=False, native_animations=False):
    def ev(code):
        result=request('/eval '+code);assert result.strip()=='ok',(code,result)
    def read(cmd):return json.loads(request(cmd))
    def wait(cmd,pred):
        for _ in range(300):
            data=read(cmd)
            if pred(data):return data
            time.sleep(.02)
        raise AssertionError((cmd,data))
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    for kind,name in [('client-header','xdg-shell-client.h'),('private-code','xdg-shell-protocol.c')]:
        subprocess.run(['wayland-scanner',kind,str(xml),str(root/name)],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    ev('hl.config({input={follow_mouse=1},general={layout="'+layout+'"},animations={enabled='+str(native_animations).lower()+'},decoration={rounding=0,shadow={enabled=false},blur={enabled=false}},misc={background_color=0xff202020},plugin={hymission={stage_enabled=0,tiles_enabled='+str(int(mode!='off'))+',layout_engine="natural",niri_mode=0,overview_focus_follows_mouse=1,selected_expand_scale=1.18,hover_expand_scale=1.18,workspace_change_keeps_overview=1,toggle_switch_mode=1,workspace_strip_anchor="left",gesture_close_restores_focus=1,pick_labels_enabled=1,pick_labels_show=0,vim_keys=0,debug_logs=1,backdrop_color=0x00000000,backdrop_blur=0}},gestures={workspace_swipe_distance=300,workspace_swipe_min_speed_to_force=0,workspace_swipe_cancel_ratio=.5}})')
    if fp16:ev('hl.config({render={use_fp16=1,keep_unmodified_copy=1}})')
    if opacity:ev('hl.config({decoration={active_opacity=.85,inactive_opacity=.4,dim_inactive=true,dim_strength=.15}})')
    clients=[]
    for ws,color in [(1,'0000ff'),(1,'ff00ff'),(2,'00ff00'),(2,'ffff00'),(3,'00ffff')]:
        ev(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        log=open(root/f'client-{len(clients)}.log','w');handles.append(log)
        current=child.copy();current['HMT_MARKER_COLOR']=color
        p=subprocess.Popen([str(binary)],env=current,stdout=log,stderr=subprocess.STDOUT);processes.append(p)
        cs=wait('j/clients',lambda cs:any(c['pid']==p.pid for c in cs))
        clients.append(next(c['address'] for c in cs if c['pid']==p.pid))
    ev('hl.dispatch(hl.dsp.focus({workspace=1})); hl.plugin.hymission_tiles_test.refresh()')
    if mode=='two':ev('hl.plugin.hymission.tile_add({workspace="3"}); hl.plugin.hymission.tile_focus("left")')
    ev('hl.plugin.hymission.gesture({fingers=4,direction="vertical",action="toggle",recommand=true})')
    target_index=1 if same_workspace else 2
    (root/"fb21-clients.json").write_text(json.dumps(clients))
    (root/"fb21-settings.json").write_text(json.dumps(dict(mode=mode,layout=layout,fp16=fp16,opacity=opacity,hover_delay=hover_delay,target_index=target_index,follow_mouse=1,native_animations=native_animations)))
    observations=[]
    def shot(label):
        subprocess.run(['grim',str(root/(label+'.png'))],env=child,check=True,capture_output=True)
        d=dict(overview=read('j/hymission-overview-state'),clients=read('j/clients'),focus=read('j/activewindow'),tiles=read('j/hymission-tiles-state'))
        (root/(label+'.json')).write_text(json.dumps(d,indent=2));return d
    def record_begin():ev('hl.plugin.hymission_tiles_test.frame_capture("begin")')
    def record_end(label):
        ev('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(root/label))+')')
        meta=root/(label+'.json');frames=json.loads(meta.read_text())
        for f in frames:
            p=Path(f['pixels']);q=p.with_suffix('.png')
            with Image.open(p) as im:im.save(q,compress_level=1)
            p.unlink();f['pixels']=str(q)
        meta.write_text(json.dumps(frames,indent=2));return frames
    def updates(n):
        for _ in range(n):ev('hl.plugin.hymission_tiles_test.trackpad("update",20)');time.sleep(.025)
    for name in ("continuous", "split", "continuous-cancel"):
        split=name=="split"
        cancelled=name=="continuous-cancel"
        ev('hl.plugin.hymission_tiles_test.window("focus","'+clients[0]+'"); hl.plugin.hymission_tiles_test.pointer("move",0,0); hl.plugin.hymission.open("forceall")')
        before=wait('j/hymission-overview-state',lambda s:s['phase']=='active' and not s['relayout_active'])
        time.sleep(.5)
        b=next(w['contentGeometry'] for w in before['windows'] if w['address']==clients[target_index])
        ev(f'hl.plugin.hymission_tiles_test.pointer("move",{b["x"]+b["width"]/2},{b["y"]+b["height"]/2})')
        time.sleep(hover_delay);hover=shot(name+'-hover')
        record_begin();ev('hl.plugin.hymission_tiles_test.trackpad("begin")');updates(15)
        desktop=shot(name+'-desktop')
        if split:
            ev('hl.plugin.hymission_tiles_test.trackpad("end")');wait('j/hymission-overview-state',lambda s:not s['active']);time.sleep(.25)
            ev('hl.plugin.hymission_tiles_test.trackpad("begin")')
        updates(9 if cancelled else 24);held=shot(name+'-held');frames=record_end(name+'-frames')
        record_begin()
        ev('hl.plugin.hymission_tiles_test.trackpad("'+('cancel' if cancelled else 'end')+'")')
        wait('j/hymission-overview-state',lambda s:not s['active'] if cancelled else s['phase']=='active' and not s['relayout_active'])
        time.sleep(.4);settled=shot(name+'-settled');release_frames=record_end(name+'-release')
        observations.append(dict(case=name,opening=before,hover=hover,desktop=desktop,held=held,settled=settled,frames=len(frames),release_frames=len(release_frames)))
        ev('hl.plugin.hymission.close()');wait('j/hymission-overview-state',lambda s:not s['active']);time.sleep(.4)
        shot(name+'-closed')
    (root/'fb21-observations.json').write_text(json.dumps(observations,indent=2))
    errors=request('/configerrors');(root/'configerrors.txt').write_text(errors)
    assert not errors.strip(),errors
    if not observe:
        from overview_fb21_pixels import verify
        verify(root,clients,fp16,opacity)
    print(('OBSERVED' if observe else 'PASS')+' FB21:',[(o['case'],o['settled']['overview'].get('window_layout_scope'),o['frames']) for o in observations])
