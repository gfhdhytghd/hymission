"""Stage drops use private device input and real clients; never host IPC."""
import json
from pathlib import Path
import subprocess
import time


def run(case,observe,request,root,env,runtime,processes,handles,layout="dwindle",hide_active=False):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
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
    def pointer(action,x=0,y=0):evaluate(f'hl.plugin.hymission_tiles_test.pointer("device_{action}",{x},{y})')
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    def capture(name):
        subprocess.run(['grim','-o',read('j/monitors')[0]['name'],str(root/(name+'.png'))],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        (root/(name+'.json')).write_text(json.dumps(dict(stage=read('j/hymission-stage-state'),tiles=read('j/hymission-tiles-state'),clients=read('j/clients')),indent=2))
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker';subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    evaluate('hl.config({general={layout="dwindle"},animations={enabled=false},plugin={hymission={tiles_enabled=1,tile_min_width=120,niri_mode=0,stage_enabled=0,stage_show_active=1,stage_show_empty=1,stage_empty_slots=0,stage_transition_ms=0}}}); hl.plugin.hymission_tiles_test.refresh()')
    if layout=="scrolling":evaluate('hl.config({general={layout="scrolling"},scrolling={column_width=.85,fullscreen_on_one_column=true}})')
    for ws in range(1,7):
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        log=open(root/f'client-{ws}.log','w');handles.append(log)
        processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
        wait('j/clients',lambda cs:len(cs)==ws)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=2}))')
    log=open(root/'second-window.log','w');handles.append(log)
    processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
    wait('j/clients',lambda cs:len(cs)==7)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1}))')
    count={'off':1,'single':1,'two':2,'three':3}[case]
    if case=='off':evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
    if count>=2:evaluate('hl.plugin.hymission.tile_add({workspace="3"}); hl.dispatch(hl.dsp.focus({workspace=4})); hl.dispatch(hl.dsp.focus({workspace=3}))')
    if count==3:evaluate('hl.plugin.hymission.tile_add({workspace="5"}); hl.dispatch(hl.dsp.focus({workspace=6})); hl.dispatch(hl.dsp.focus({workspace=5}))')
    evaluate('hl.config({plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission.stage_open()')
    stage=wait('j/hymission-stage-state',lambda s:len(s['screens'])==count and all(sc['cards'] for sc in s['screens']))
    time.sleep(.4)
    evaluate('hl.bind("SUPER + mouse:272",hl.dsp.window.drag(),{mouse=true}); hl.config({input={follow_mouse=0,sensitivity=0},binds={drag_threshold=0}})')
    results=[]
    def stage():return read('j/hymission-stage-state')
    def card(ws):
        return next((sc,c) for sc in stage()['screens'] for c in sc['cards'] if c['workspace']==ws)
    def membership():return {c['address']:c['workspace']['id'] for c in read('j/clients')}
    def drag(ws,target,name,window=False):
        sc,c=card(ws)
        if window:
            w=c['windows'][0];x=w['x']+w['width']/2;y=w['y']+w['height']/2
        else:x=c['x']+1;y=c['y']+1
        if case=='off':x=c['x']+c['width']/2;y=c['y']+c['height']/2
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin")')
        pointer('move',x,y);pointer('press',x,y);pointer('move',x+12,y+12);pointer('move',*target);time.sleep(.15)
        capture(name+'-held');pointer('release',*target);time.sleep(.25);capture(name+'-released')
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(root/name))+')')
    def meta(on):evaluate('hl.plugin.hymission_tiles_test.meta('+str(on).lower()+')')
    def native_blank(name,synthetic=False,cancel=False):
        sc=stage()['screens'][-1];band=sc['sidebar'];desktop=stage()['screens'][0]['desktop_geometry']
        before=membership();source=stage()['screens'][0]['active_workspace'];address=next(a for a,w in before.items() if w==source)
        x=desktop['x']+desktop['width']/2;y=desktop['y']+desktop['height']/2
        target=(band['x']+band['width']/2,band['y']+band['height']-10)
        if synthetic:
            c=next(c for c in sc['cards'] if c.get('synthetic'))
            first=sc['cards'][0];second=sc['cards'][1]
            index=sc['cards'].index(c)
            target=(first['x']+first['width']/2,first['y']+index*(second['y']-first['y'])+first['height']/2)
        evaluate('hl.config({animations={enabled=true},plugin={hymission={stage_transition_ms=220}}})')
        pointer('move',x,y);meta(True)
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin")')
        # Assert native grab exists: button travelled through InputManager and bindm.
        reply=request(f'/eval assert(hl.plugin.hymission_tiles_test.pointer("device_press",{x},{y}))')
        assert reply.strip()=='ok',reply
        for step in range(1,13):
            pointer('move',x+(target[0]-x)*step/12,y+(target[1]-y)*step/12);time.sleep(.025)
        check(membership()==before,name+': held drag preserves workspace membership')
        capture(name+'-held')
        if cancel:evaluate('hl.plugin.hymission_tiles_test.key(1)')
        pointer('release',*target);meta(False);time.sleep(.55);capture(name+'-released')
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(root/name))+')')
        after=membership();ws=after[address]
        from tiles_stage_order_regression import numeric_order, error
        final=stage();numeric_order(final)
        if cancel:
            check(ws==source,'Esc cancels native blank drop before creation')
        elif case=='off' and not synthetic:
            check(ws==source,'tiles-off arbitrary blank does not create a workspace (baseline)')
        else:
            check(ws not in before.values(),'blank/synthetic drop creates a separate workspace')
            if case!='off':check(card(ws)[0]['tile_id']==sc['tile_id'],'new workspace belongs to destination column')
            check(all(after[a]==w for a,w in before.items() if a!=address),'native drop never merges unrelated windows')
            target_card=next(c for sc in final['screens'] for c in sc['cards'] if c['workspace']==ws)
            target_window=next(w for w in target_card['windows'] if w['address']==address)
            frames=json.loads((root/(name+'.json')).read_text())
            goals=[f['to'] for frame in frames for sc in frame['stage']['screens'] for f in sc['flights'] if f['workspace']==ws]
            check(bool(goals) and max(error(g,target_window) for g in goals)<=1,name+': new workspace flight goal equals numeric static slot')
        evaluate('hl.config({animations={enabled=false},plugin={hymission={stage_transition_ms=0}}})')
        return ws
    if case=='off':
        native_blank('off-arbitrary-blank')
        # Native Stage creation means an explicitly configured synthetic empty slot.
        evaluate('hl.config({plugin={hymission={stage_empty_slots=7}}})')
        time.sleep(.3);native_blank('off-synthetic-empty',True)
    else:
        if hide_active:
            evaluate('hl.config({plugin={hymission={stage_show_active=0}}})');time.sleep(.3)
        native_blank('super-native-blank-cancel',cancel=True)
        native_blank('super-native-blank')
        if hide_active:
            evaluate('hl.config({plugin={hymission={stage_show_active=1}}})');time.sleep(.3)
        if count>1:
            target=stage()['screens'][-1];band=target['sidebar'];before=membership()
            # Deliberately press inside a window preview, the actual FB6 failure.
            drag(2,(band['x']+band['width']/2,band['y']+band['height']-8),'card-preview-to-blank',True)
            check(membership()==before,'card preview press transfers whole workspace identity')
            check(card(2)[0]['tile_id']==target['tile_id'],'whole workspace adopts destination column')
            check([c['workspace'] for c in card(2)[0]['cards']]==sorted(c['workspace'] for c in card(2)[0]['cards']),'blank uses numeric position')
            sc,c=card(3);before=membership()
            existing_source=4 if count==3 else 5
            drag(existing_source,(c['x']+c['width']/2,c['y']+c['height']/2),'card-preview-to-existing',True)
            check(membership()==before,'ordinary card onto existing card never merges contents')
            check(card(existing_source)[0]['tile_id']==sc['tile_id'],'existing card drop changes column ownership')
            ids=[c['workspace'] for c in card(existing_source)[0]['cards']]
            check(existing_source in ids and 3 in ids and ids==sorted(ids),'drop height cannot override numeric order')
            # Meta is the explicit individual-window gesture; keep its old policy.
            sc,c=card(5);before=membership();address=next(a for a,w in before.items() if w==6)
            meta(True);drag(6,(c['x']+c['width']/2,c['y']+c['height']/2),'meta-preview-existing',True);meta(False)
            check(membership()[address]==5,'Meta preview onto existing card moves individual window')
        check(read('j/hymission-tiles-state')['interaction_owner']==0,'shared interaction owner released')
    # Pixel evidence for every rendered frame, compact PNG artifacts.
    from PIL import Image
    for path in root.glob('*.ppm'):
        image=Image.open(path);image.save(path.with_suffix('.png'));path.unlink()
    for path in root.glob('*.json'):
        text=path.read_text()
        if '.ppm' in text:path.write_text(text.replace('.ppm','.png'))
    import hashlib
    frames=[]
    for path in root.glob('*.json'):
        value=json.loads(path.read_text())
        if isinstance(value,list) and value and isinstance(value[0],dict) and 'pixels' in value[0]:
            frames.append(dict(sequence=path.name,count=len(value),sha256=[hashlib.sha256(Path(f['pixels']).read_bytes()).hexdigest() for f in value]))
    check(bool(frames) and all(f['count']>2 for f in frames),'rendered intermediate frames recorded for every drag sequence')
    (root/'frame-manifest.json').write_text(json.dumps(frames,indent=2))
    print(('OBSERVED' if observe else 'PASS')+': real device Stage drops '+case)
