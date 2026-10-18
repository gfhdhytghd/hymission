"""Stage workspace motion: private real clients, keyboard/swipe input and every-rendered-frame readback."""
import json
import subprocess
import time
from pathlib import Path
from PIL import Image


def run(case,observe,route_filter,layout,request,root,env,runtime,processes,handles):
    results=[]
    def evaluate(code):
        reply=request('/eval '+code)
        assert reply.strip()=='ok',(code,reply)
    def read(cmd):return json.loads(request(cmd))
    def wait(cmd,predicate):
        for _ in range(300):
            value=read(cmd)
            if predicate(value):return value
            time.sleep(.02)
        raise AssertionError((cmd,value))
    def check(ok,message):
        results.append(dict(check=message,passed=bool(ok)))
        (root/'results.json').write_text(json.dumps(results,indent=2))
        if not observe:assert ok,message
    def focus(ws):evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={json.dumps(str(ws))}}}))')
    def stage():return read('j/hymission-stage-state')
    def pointer(action,x,y):evaluate(f'hl.plugin.hymission_tiles_test.pointer("{action}",{x},{y})')
    def record(name,action,swipe=False):
        before=stage();(root/(name+'-before.json')).write_text(json.dumps(before,indent=2))
        prefix=str(root/name)
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin")')
        samples=[];start=time.monotonic();action()
        for n in range(30):
            samples.append(dict(ms=(time.monotonic()-start)*1000,stage=stage()))
            time.sleep(.018)
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(prefix)+')')
        frames=json.loads(Path(prefix+'.json').read_text())
        for f in frames:
            p=Path(f['pixels']) if 'pixels' in f else None
            if p and p.exists():
                im=Image.open(p);q=p.with_suffix('.png');im.save(q);p.unlink();f['pixels']=str(q)
        (root/(name+'-samples.json')).write_text(json.dumps(samples,indent=2))
        (root/(name+'-frames.json')).write_text(json.dumps(frames,indent=2))
        counts=[max((s['transition_windows'] for s in item['stage']['screens']),default=0) for item in samples]
        check(max(counts,default=0)>0,name+': Stage integrated flight exists')
        check(any(0<s['transition_progress']<1 for item in samples for s in item['stage']['screens'] if s['transition_windows']),name+': Stage intermediate flight progress')
        return before,samples,frames
    evaluate('hl.monitor({output="",mode="1280x720@60",position="auto",scale="1"}); hl.config({general={layout="dwindle"},animations={enabled=false},input={follow_mouse=0},plugin={hymission={stage_enabled=0,niri_mode=0,stage_show_active=0,stage_empty_slots=0,stage_transition_ms=450}}})')
    if layout=="scrolling":evaluate('hl.config({general={layout="scrolling"},scrolling={column_width=.85,fullscreen_on_one_column=true,focus_fit_method=1}})')
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker';subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    pairs=[(101,102),(201,202),(301,302)]
    count={'off':1,'single':1,'two':2,'three':3}[case]
    total=0
    for ws in [ws for pair in pairs[:count] for ws in pair]:
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}})');focus(ws)
        log=open(root/f'client-{ws}.log','w');handles.append(log)
        processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT));total+=1
        wait('j/clients',lambda cs:len(cs)==total)
    focus(101)
    try:has_tiles=isinstance(read('j/hymission-tiles-state'),dict)
    except (ValueError,KeyError):has_tiles=False
    if has_tiles:
        evaluate(f'hl.config({{plugin={{hymission={{tiles_enabled={0 if case=="off" else 1},tile_min_width=120}}}}}}); hl.plugin.hymission_tiles_test.refresh()')
        for a,b in pairs[1:count]:evaluate(f'hl.plugin.hymission.tile_add({{workspace="{a}"}})');focus(b);focus(a)
        focus(101)
    evaluate('hl.config({animations={enabled=true},plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission.stage_open()')
    wait('j/hymission-stage-state',lambda s:len(s['screens'])==count and s['visibility_progress']==1)
    time.sleep(.7)
    for route in ('key','relative','card','swipe'):
        if route_filter and route!=route_filter:continue
        focus(101);time.sleep(.7)
        def action():
            if route in ('key','relative'):
                selector='102' if route=='key' else 'm+1'
                evaluate(f'if fb10bind then fb10bind:remove() end; fb10bind=hl.bind("F8",hl.dsp.focus({{workspace="{selector}"}})); hl.plugin.hymission_tiles_test.key(66)')
            elif route=='card':
                s=stage();sc,c=next((sc,c) for sc in s['screens'] for c in sc['cards'] if c['workspace']==102)
                x=c.get('x',10)+sc['card_width']/2;y=c.get('y',10)+sc['card_height']/2
                pointer('press',x,y);pointer('release',x,y)
            elif route=='swipe':
                evaluate('hl.gesture({fingers=4,direction="vertical",action="workspace"}); hl.config({gestures={workspace_swipe_invert=false,workspace_swipe_create_new=false,workspace_swipe_use_r=false,workspace_swipe_distance=300,workspace_swipe_cancel_ratio=.05,workspace_swipe_min_speed_to_force=0,workspace_swipe_forever=false}})')
                evaluate('hl.plugin.hymission_tiles_test.trackpad("begin")')
                for n in range(1,5):
                    evaluate('hl.plugin.hymission_tiles_test.trackpad("update",45)')
                    (root/f'swipe-held-{n}.json').write_text(json.dumps(stage(),indent=2));time.sleep(.04)
                evaluate('hl.plugin.hymission_tiles_test.trackpad("end")')
        record(route,action,route=='swipe')
    print(('OBSERVE' if observe else 'PASS')+f': Stage {case}, {len(results)} checks, {sum(not x["passed"] for x in results)} failed')
