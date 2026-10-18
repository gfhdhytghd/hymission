"""Owned relative navigation through real private keyboard and four-finger device events."""
import json
import subprocess
import time
from pathlib import Path


def run(case,observe,request,root,env,runtime,processes,handles):
    results=[]
    (root/"keyboard.jsonl").write_text("")
    def evaluate(code):
        answer=request('/eval '+code)
        assert answer.strip()=='ok',(code,answer)
    def read(command):return json.loads(request(command))
    def state():return read('j/hymission-tiles-state')
    def wait(command,predicate):
        for _ in range(250):
            value=read(command)
            if predicate(value):return value
            time.sleep(.02)
        raise AssertionError((command,value))
    def check(ok,message):
        results.append(dict(check=message,passed=bool(ok)))
        (root/'results.json').write_text(json.dumps(results,indent=2))
        if not observe:assert ok,message
    def focus(ws):evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={json.dumps(str(ws))}}}))')
    def current():return read('j/activeworkspace')['id']
    def bindings():return {t['id']:t['activeWorkspace']['id'] for t in state()['tiles']}
    def select(selector,expected,label):
        before=state();tile=next(t['id'] for t in before['tiles'] if t['focused']);neighbors={i:w for i,w in bindings().items() if i!=tile}
        # Rebind a real keyboard key, then send press/release through IKeyboard -> InputManager.
        evaluate(f'if fb9bind then fb9bind:remove() end; fb9bind=hl.bind("F8",hl.dsp.focus({{workspace={json.dumps(selector)}}})); hl.plugin.hymission_tiles_test.key(66)')
        with (root/'keyboard.jsonl').open('a') as log:log.write(json.dumps(dict(selector=selector,expected=expected,actual=current(),state=state()))+'\n')
        check(current()==expected,label+': workspace')
        check(all(bindings().get(i)==w for i,w in neighbors.items()),label+': neighbors unchanged')
        check(next(t['id'] for t in state()['tiles'] if t['focused'])==tile,label+': tile focus unchanged')
    evaluate('hl.config({general={layout="dwindle"},animations={enabled=false},input={follow_mouse=0},binds={workspace_back_and_forth=false},plugin={hymission={tiles_enabled=1,tile_min_width=120,stage_enabled=0,niri_mode=0}}}); hl.plugin.hymission_tiles_test.refresh()')
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker';subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    count=3 if case=='three' else 2
    total=0
    setup=[]
    pairs=[(1,7),(3,8),(5,9)][:count]
    for ws in [ws for pair in pairs for ws in pair]:
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}})');focus(ws)
        log=open(root/f'client-{ws}.log','w');handles.append(log)
        processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT));total+=1
        wait('j/clients',lambda cs:len(cs)==total)
    if case in ('off','single'):
        if case=='off':evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
        control=[]
        for base,selector in ((1,'m+1'),(7,'m-1'),(1,'e-1'),(1,'r+1'),(1,'+2'),(7,'next')):
            focus(base)
            evaluate(f'if fb9bind then fb9bind:remove() end; fb9bind=hl.bind("F8",hl.dsp.focus({{workspace={json.dumps(selector)}}})); hl.plugin.hymission_tiles_test.key(66)')
            control.append(dict(base=base,selector=selector,result=current()))
        focus(1);focus(7)
        evaluate('if fb9bind then fb9bind:remove() end; fb9bind=hl.bind("F8",hl.dsp.focus({workspace="previous"})); hl.plugin.hymission_tiles_test.key(66)')
        control.append(dict(base=7,selector='previous',result=current()))
        (root/'control.json').write_text(json.dumps(control,indent=2))
        print('OBSERVE: native relative selector control '+case);return
    focus(1)
    for i,pair in enumerate(pairs):
        if i:
            evaluate(f'hl.plugin.hymission.tile_add({{workspace="{pair[0]}"}})')
            focus(pair[1]);focus(pair[0])
        setup.append(state()); (root/"setup.json").write_text(json.dumps(setup,indent=2))
    ids=[t['id'] for t in state()['tiles']]
    def home_focus(ws):
        index=next(i for i,pair in enumerate(pairs) if ws in pair)
        focus(bindings()[ids[index]]);focus(ws)
    def groups():
        evaluate('hl.config({plugin={hymission={stage_enabled=1,stage_transition_ms=0,stage_show_active=1,stage_show_empty=1,stage_empty_slots=0}}}); hl.plugin.hymission.stage_open()')
        stage=wait('j/hymission-stage-state',lambda s:len(s['screens'])==count and all(sc['cards'] for sc in s['screens']))
        (root/'stage-columns.json').write_text(json.dumps(stage,indent=2))
        result={c['workspace']:sc['tile_id'] for sc in stage['screens'] for c in sc['cards']}
        evaluate('hl.plugin.hymission.stage_close(); hl.config({plugin={hymission={stage_enabled=0}}})')
        return result
    homes=groups()
    for i,pair in enumerate(pairs):check(all(homes.get(ws)==ids[i] for ws in pair),'Stage column matches home '+str(pair))
    for a,b in pairs:
        focus(a)
        for prefix in ('m','r','e',''):
            select(prefix+'+1',b,prefix+' forward skips foreign homes')
            select(prefix+'-1',a,prefix+' backward skips foreign homes')
        select('m-1',b,'m backward wrap')
        select('e+1',a,'e forward wrap')
        select('m+3',b,'multi-step modulo')
        select('m~1',a,'absolute existing index')
        select('next',b,'next follows owned order')
        select('previous',a,'tile history previous')
        select('previous_per_monitor',b,'tile monitor history previous')
        evaluate('hl.config({binds={workspace_back_and_forth=true}})')
        select(str(b),a,'back-and-forth uses local history')
        evaluate('hl.config({binds={workspace_back_and_forth=false}})')
        select('r-1',a,'r negative boundary clamps locally')
    # Name a hidden foreign workspace explicitly: it adopts the focused tile.
    focus(7);focus(1);focus(bindings()[ids[-1]]);focus(7)
    check(next(t['id'] for t in state()['tiles'] if t['focused'])==ids[-1],'explicit hidden target adopts caller tile')
    homes=groups();check(homes.get(7)==ids[-1],'Stage reflects explicit adoption')
    focus(1)
    previous_path=root/'previous-target.json'
    evaluate('hl.plugin.hymission_tiles_test.resolve_workspace("previous",'+json.dumps(str(previous_path))+')')
    check(json.loads(previous_path.read_text())==-1 and current()==1,'stale transferred history ignored without creating a workspace')
    # Restore ownership using explicit activation, then test overview strip.
    focus(7);focus(pairs[-1][1]);focus(1);focus(7);focus(1)
    evaluate('hl.config({plugin={hymission={workspace_strip_force_show=1,workspace_strip_empty_mode="always"}}}); hl.plugin.hymission.open("onlycurrentworkspace")')
    overview=wait('j/hymission-overview-state',lambda s:s['phase']=='active')
    (root/'strip-columns.json').write_text(json.dumps(overview,indent=2))
    strip={e['workspace']:e['tile_id'] for e in overview['strip'] if isinstance(e.get('workspace'),int)}
    for i,pair in enumerate(pairs):check(all(strip.get(ws)==ids[i] for ws in pair),'strip column matches home '+str(pair))
    evaluate('hl.plugin.hymission.close()');wait('j/hymission-overview-state',lambda s:s['phase']=='inactive')
    # Native four-finger event path, both m and r candidate policy, Stage off/on.
    evaluate('hl.gesture({fingers=4,direction="vertical",action="workspace"}); hl.animation({leaf="workspaces",enabled=true,speed=8,bezier="default",style="slidevert"}); hl.config({gestures={workspace_swipe_invert=false,workspace_swipe_distance=300,workspace_swipe_cancel_ratio=.05,workspace_swipe_min_speed_to_force=0,workspace_swipe_forever=false,workspace_swipe_create_new=false}})')
    for stage in (0,1):
        evaluate('hl.config({plugin={hymission={stage_enabled=%d,stage_transition_ms=%d}}}); hl.plugin.hymission.stage_%s()' % (stage,250 if stage else 0,'open' if stage else 'close'))
        if stage:wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1)
        for use_r in (False,True):
            evaluate(f'hl.config({{gestures={{workspace_swipe_use_r={str(use_r).lower()}}}}})')
            for a,b in pairs:
                home_focus(a);time.sleep(.35);evaluate('hl.config({animations={enabled=true}})')
                neighbors={i:w for i,w in bindings().items() if w!=a};samples=[]
                evaluate('hl.plugin.hymission_tiles_test.trackpad("begin")')
                for n in range(1,5):
                    evaluate('hl.plugin.hymission_tiles_test.trackpad("update",45)');time.sleep(.02)
                    sample=state();samples.append(sample)
                    if n==2:
                        path=root/f'swipe-{stage}-{use_r}-{a}-native.json'
                        evaluate('hl.plugin.hymission_tiles_test.animation_report('+json.dumps(str(path))+')')
                        native=json.loads(path.read_text())
                        if stage:
                            visual=read('j/hymission-stage-state')
                            check(any(f['workspace']==b and abs(f['frame']['width']-f['from']['width'])>1 for sc in visual['screens'] for f in sc.get('flights',[])),'four-finger Stage flight previews owned target')
                        else:check(str(b) in native and abs(native[str(b)]['value'][1])>1,'four-finger previews owned target')
                        subprocess.run(['grim','-o',read('j/monitors')[0]['name'],str(path.with_suffix('.png'))],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
                (root/f'swipe-{stage}-{use_r}-{a}.json').write_text(json.dumps(samples,indent=2))
                if stage:
                    visual=read('j/hymission-stage-state')
                    check(any(sc['swipe_active'] and 0<sc['swipe_progress']<1 and sc['transition_windows']>0 for sc in visual['screens']),'four-finger Stage integrated intermediate motion')
                    check(all(t['renderOffset']==[0,0] for t in state()['tiles']),'Stage owns motion without native tile slide')
                else:check(any(any(abs(t['renderOffset'][1])>1 for t in s['tiles'] if t['activeWorkspace']['id']==a) for s in samples),'four-finger vertical intermediate motion')
                check(all(all(abs(t['renderOffset'][1])<.01 for t in s['tiles'] if t['id'] in neighbors) for s in samples),'four-finger stable neighbors')
                evaluate('hl.plugin.hymission_tiles_test.trackpad("end")');time.sleep(.4)
                (root/f'swipe-{stage}-{use_r}-{a}-after.json').write_text(json.dumps(dict(current=current(),tiles=state()),indent=2))
                check(current()==b,'four-finger selects only owned next')
                check(all(bindings().get(i)==w for i,w in neighbors.items()),'four-finger preserves neighbors')
                evaluate('hl.config({animations={enabled=false}})')
    evaluate('hl.plugin.hymission.stage_close(); hl.config({plugin={hymission={stage_enabled=0}},gestures={workspace_swipe_create_new=true,workspace_swipe_use_r=true}})')
    # Creating past an owned boundary skips other tiles' numbered slots.
    home_focus(7);select('r+1',10 if count==3 else 9,'r creates after owned end skipping foreign IDs')
    created=current();homes=groups();check(homes.get(created)==ids[0],'created workspace home is current tile')
    select('m-1',7,'created workspace joins local sorted ring')
    focus(created)
    evaluate('hl.plugin.hymission_tiles_test.trackpad("begin"); hl.plugin.hymission_tiles_test.trackpad("update",200); hl.plugin.hymission_tiles_test.trackpad("end")')
    time.sleep(.3)
    check(current()==created+1,'four-finger right boundary creates in current tile')
    homes=groups();check(homes.get(created+1)==ids[0],'swipe-created workspace home is current tile')
    # Remove last tile: hidden ownership merges to adjacent tile rather than first.
    if count==3:
        home_focus(5);evaluate('hl.plugin.hymission.tile_remove()');count=2
        homes=groups();check(homes.get(9)==ids[1],'removed tile hidden homes merge into adjacent tile')
    failed=sum(not item['passed'] for item in results)
    print(f'{"OBSERVE" if observe else "PASS"}: {case} owned relative keyboard selectors, history, columns, and four-finger gestures ({len(results)} checks, {failed} failed)')
