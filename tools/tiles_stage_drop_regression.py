"""Stage drops use private device input and real clients; never host IPC."""
import json
from pathlib import Path
import subprocess
import time


def run(case,observe,request,root,env,runtime,processes,handles):
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
    def pointer(action,x=0,y=0):evaluate(f'hl.plugin.hymission_tiles_test.pointer("{action}",{x},{y})')
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
    for ws in range(1101,1107):
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        log=open(root/f'client-{ws}.log','w');handles.append(log)
        processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
        wait('j/clients',lambda cs:len(cs)==ws-1100)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1101}))')
    count={'off':1,'single':1,'two':2,'three':3}[case]
    if case=='off':evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
    if count>=2:evaluate('hl.plugin.hymission.tile_add({workspace="1103"}); hl.dispatch(hl.dsp.focus({workspace=1104})); hl.dispatch(hl.dsp.focus({workspace=1103}))')
    if count==3:evaluate('hl.plugin.hymission.tile_add({workspace="1105"}); hl.dispatch(hl.dsp.focus({workspace=1106})); hl.dispatch(hl.dsp.focus({workspace=1105}))')
    evaluate('hl.config({plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission.stage_open()')
    stage=wait('j/hymission-stage-state',lambda s:len(s['screens'])==count and all(sc['cards'] for sc in s['screens']))
    time.sleep(.4)
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
        pointer('press',x,y);pointer('move',x+12,y+12);pointer('move',*target);time.sleep(.15)
        capture(name+'-held');pointer('release',*target);time.sleep(.25);capture(name+'-released')
    if case=='off':
        before=membership();sc,c=card(1103)
        drag(1102,(c['x']+c['width']/2,c['y']+c['height']/2),'native-reorder')
        check(membership()==before,'tiles-off card drag never swaps workspace contents')
        check([c['workspace'] for c in stage()['screens'][0]['cards']]==sorted(c['workspace'] for c in stage()['screens'][0]['cards']),'tiles-off card order remains numeric')
        (root/'native-reorder-observation.json').write_text(json.dumps(dict(before=before,after=membership(),stage=stage()),indent=2))
        before=membership();sc,c=card(1102);band=sc['sidebar']
        drag(1102,(band['x']+band['width']/2,band['y']+band['height']-2),'native-outside-padding')
        check(membership()==before,'native drop outside reorder bounds cancels')
        print('PASS: tiles-disabled native reorder baseline');return
    # Same-column lower padding returns to the numeric slot, never a content swap.
    sc,c=card(1102);band=sc['sidebar'];before=membership()
    drag(1102,(band['x']+band['width']/2,band['y']+band['height']-2),'same-column-empty')
    check(membership()==before,'same-column blank preserves every workspace identity')
    check([c['workspace'] for c in card(1102)[0]['cards']]==sorted(c['workspace'] for c in card(1102)[0]['cards']),'same-column blank retains numeric order')
    if count>1:
        target=stage()['screens'][-1];band=target['sidebar'];destination=target['active_workspace'];before=membership()
        drag(1102,(band['x']+band['width']/2,band['y']+band['height']-2),'workspace-cross-column-empty')
        check(membership()==before,'whole-workspace transfer never merges windows')
        check(card(1102)[0]['tile_id']==target['tile_id'],'workspace card adopts destination column')
        check([c['workspace'] for c in card(1102)[0]['cards']]==sorted(c['workspace'] for c in card(1102)[0]['cards']),'cross-column blank uses numeric position')
        # Meta explicitly moves an individual preview into an existing card.
        target=stage()["screens"][-1];band=target["sidebar"];destination=target["active_workspace"]
        source=1101;before=membership();address=next(a for a,w in before.items() if w==source)
        _,c=card(destination)
        evaluate('hl.plugin.hymission_tiles_test.meta(true)')
        drag(source,(c['x']+c['width']/2,c['y']+c['height']/2),'meta-window-cross-column-card',True)
        evaluate('hl.plugin.hymission_tiles_test.meta(false)')
        check(membership()[address]==destination,'explicit Meta window gesture targets destination card')
        # Workspace card onto desktop carries its members, with pointer-derived target.
        sc,c=card(1104);desktop=stage()['screens'][0]['desktop_geometry']
        before=membership();members=[a for a,w in before.items() if w==1104]
        drag(1104,(desktop['x']+desktop['width']/2,desktop['y']+desktop['height']/2),'workspace-to-desktop')
        target_ws=read('j/hymission-tiles-state')['tiles'][0]['activeWorkspace']['id']
        check(all(membership()[a]==target_ws for a in members),'desktop drop uses pointer tile workspace')
    else:
        desktop=stage()['screens'][0]['desktop_geometry']
        before=membership();address=next(a for a,w in before.items() if w==1102)
        drag(1102,(desktop['x']+desktop['width']*.75,desktop['y']+desktop['height']/2),'single-desktop-split',True)
        tiles=read('j/hymission-tiles-state')['tiles']
        check(len(tiles)==2,'single tile desktop drop creates split')
        check(membership()[address]!=1101,'split window never falls back to global active')
    # Invalid target and Esc must preserve every membership and release the owner.
    sc,c=card(1106);before=membership()
    drag(1106,(-100,-100),'invalid-outside')
    check(membership()==before,'invalid outside target cancels without active-workspace fallback')
    sc,c=card(1106);x=c['x']+1;y=c['y']+1
    pointer('press',x,y);pointer('move',x+20,y+20);pointer('escape');pointer('release',x+20,y+20)
    check(membership()==before,'Esc cancels without workspace mutation')
    check(read('j/hymission-tiles-state')['interaction_owner']==0,'drop releases shared interaction owner')
    print(('OBSERVED' if observe else 'PASS')+': Stage '+case+' column identity/order/desktop/split drops')
