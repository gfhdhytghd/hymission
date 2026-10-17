"""Sample native/tile workspace animations and swipe settlement between frames."""
import json
import subprocess
import time


def run(request,root,env,runtime,processes,handles):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def wait(command,predicate):
        for _ in range(200):
            state=json.loads(request(command))
            if predicate(state):return state
            time.sleep(.02)
        raise AssertionError((command,state))
    def sample(path):
        evaluate('hl.plugin.hymission_tiles_test.animation_report('+json.dumps(str(path))+')')
        return json.loads(path.read_text())
    def record(name):
        frames=[]
        for i in range(26):
            frames.append(sample(root/(name+f'-{i:02}.json')))
            time.sleep(.02)
        (root/(name+'.json')).write_text(json.dumps(frames,indent=2))
        return frames
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    evaluate('hl.curve("tilesFeedback",{type="bezier",points={{.25,.1},{.25,1}}}); hl.config({animations={enabled=false,workspace_wraparound=false},general={gaps_workspaces=17}})')
    for ws in (501,502,503,504,505):evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}})')
    for ws in (501,502,503):
        evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        log=open(root/f'animation-client-{ws}.log','w');handles.append(log)
        processes.append(subprocess.Popen(['weston-simple-shm'],env=child,stdout=log,stderr=subprocess.STDOUT))
        wait('j/clients',lambda cs:len(cs)==ws-500)
    styles=('slide','slidevert','slidefade 55%','fade')
    native={}
    def configure(style):
        evaluate('hl.config({animations={enabled=false}}); hl.dispatch(hl.dsp.focus({workspace=501}))')
        time.sleep(.15)
        evaluate('hl.animation({leaf="workspaces",enabled=true,speed=4,bezier="tilesFeedback",style='+json.dumps(style)+'}); hl.config({animations={enabled=true}})')
    for style in styles:
        configure(style)
        evaluate('hl.dispatch(hl.dsp.focus({workspace=502}))')
        native[style]=record('native-'+style.split()[0])
        if style=='slide':
            evaluate('hl.dispatch(hl.dsp.focus({workspace=501}))')
            native['reverse']=record('native-slide-reverse')
    configure('slide')
    evaluate('hl.config({animations={workspace_wraparound=true}}); hl.dispatch(hl.dsp.focus({workspace=505}))')
    native['wrap']=record('native-slide-wrap')
    evaluate('hl.config({animations={workspace_wraparound=false}})')
    evaluate('hl.config({animations={enabled=false},plugin={hymission={tiles_enabled=1,tile_min_width=120}}}); hl.dispatch(hl.dsp.focus({workspace=501})); hl.plugin.hymission_tiles_test.refresh()')
    wait('j/hymission-tiles-state',lambda s:s['enabled'])
    evaluate('hl.plugin.hymission.tile_add({workspace="503"}); hl.plugin.hymission.tile_add({workspace="504"}); hl.plugin.hymission.tile_focus("left"); hl.plugin.hymission.tile_focus("left"); hl.config({plugin={hymission={stage_enabled=1,stage_transition_ms=220,stage_show_active=1}}}); hl.plugin.hymission.stage_open()')
    wait('j/hymission-stage-state',lambda s:s['enabled'] and s['visibility_progress']==1 and len(s['screens'])==3)
    tiles=json.loads(request('j/hymission-tiles-state'))['tiles']; focused=next(t for t in tiles if t['focused'])
    stage=json.loads(request('j/hymission-stage-state'))
    screen=next(sc for sc in stage['screens'] if sc['tile_id']==focused['id'])
    scene_scale=screen['desktop_width']/screen['base']['width']
    width=focused['width']*scene_scale;height=focused['height']*scene_scale
    for style in styles:
        configure(style)
        neighbors=sample(root/'neighbors.json')
        evaluate('hl.dispatch(hl.dsp.focus({workspace=502}))')
        frames=record('tile-'+style.split()[0])
        monitor=json.loads(request('j/monitors'))[0]
        for ws in ('501','502'):
            reference=native[style][0][ws];actual=frames[0][ws]
            gap=0 if style.startswith('slidefade') else 17
            for axis,extent in ((0,width),(1,height)):
                native_extent=monitor['width' if axis==0 else 'height']/monitor.get('scale',1)
                ratio=(extent+gap*scene_scale)/(native_extent+gap)
                for field in ('begun','goal'):
                    assert abs(actual[field][axis]-reference[field][axis]*ratio)<.1,(style,ws,field,actual,reference)
        for ws in ('503','504'):
            assert neighbors[ws]['value']==[0,0] and abs(neighbors[ws]['alpha']-1)<.001,neighbors[ws]
        axis=1 if style=='slidevert' else 0
        moving=[f['502'] for f in frames if f['502']['animated'] and (abs(f['502']['value'][axis])>1 or .02<f['502']['alpha']<.98)]
        assert len(moving)>=4,(style,'animation snapped',frames)
        for f in frames:
            for ws in ('501','502'):
                a=f[ws]
                assert a['curve']==native[style][0][ws]['curve']=='tilesFeedback' and a['speed']==4,(style,a)
                assert abs(a['value'][0])<=width+18 and abs(a['value'][1])<=height+18,(style,a)
                d=a['goal'][axis]-a['begun'][axis]
                if abs(d)>1 and .05<a['percent']<.95:
                    assert abs((a['value'][axis]-a['begun'][axis])/d-a['curve_value'])<.16,(style,a)
            for ws in ('503','504'):
                assert f[ws]['value']==neighbors[ws]['value'] and abs(f[ws]['alpha']-neighbors[ws]['alpha'])<.001,(style,ws,f[ws])
        stage=json.loads(request('j/hymission-stage-state'))
        assert all(s['transition_windows']==0 for s in stage['screens']),stage
    configure('slide')
    evaluate('hl.dispatch(hl.dsp.focus({workspace=502}))');time.sleep(.6)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=501}))')
    reverse=record('tile-slide-reverse')
    assert reverse[0]['501']['begun'][0]<0 and native['reverse'][0]['501']['begun'][0]<0
    assert reverse[0]['502']['goal'][0]>0
    evaluate('hl.config({animations={workspace_wraparound=true}}); hl.dispatch(hl.dsp.focus({workspace=505}))')
    wrap=record('tile-slide-wrap')
    assert wrap[0]['505']['begun'][0]<0 and native['wrap'][0]['505']['begun'][0]<0
    evaluate('hl.config({animations={workspace_wraparound=false}})')
    configure('slide')
    evaluate('hl.config({gestures={workspace_swipe_distance=300,workspace_swipe_min_speed_to_force=0,workspace_swipe_cancel_ratio=.5}})')
    for delta,commit in ((180,True),(60,False)):
        evaluate('hl.config({animations={enabled=false}}); hl.dispatch(hl.dsp.focus({workspace=501})); hl.config({animations={enabled=true}})')
        time.sleep(.15)
        before=root/('swipe-commit-before.json' if commit else 'swipe-revert-before.json')
        after=root/('swipe-commit-after.json' if commit else 'swipe-revert-after.json')
        evaluate('hl.plugin.hymission_tiles_test.swipe("begin"); hl.plugin.hymission_tiles_test.swipe("update",'+str(delta)+'); hl.plugin.hymission_tiles_test.animation_report('+json.dumps(str(before))+'); hl.plugin.hymission_tiles_test.swipe("end"); hl.plugin.hymission_tiles_test.animation_report('+json.dumps(str(after))+')')
        a=json.loads(before.read_text());b=json.loads(after.read_text())
        for ws in ('501','502'):
            assert all(abs(x-y)<.01 for x,y in zip(a[ws]['value'],b[ws]['value'])),(delta,ws,a[ws],b[ws])
            assert abs(b[ws]['alpha']-1)<.001,(delta,ws,b[ws])
        record('swipe-commit' if commit else 'swipe-revert')
        active=json.loads(request('j/activeworkspace'))['id']
        assert active==(502 if commit else 501),(delta,active)
    print('PASS: native/tile animation middle frames, curve/speed/style, stable neighbors and continuous swipe commit/revert')
