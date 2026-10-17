"""Private input/render regressions for the second tile feedback round."""
import json
import subprocess
import time
from pathlib import Path
from PIL import Image


def run(case,request,root,env,runtime,processes,handles):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def wait(command,predicate):
        for _ in range(200):
            state=json.loads(request(command))
            if predicate(state):return state
            time.sleep(.02)
        raise AssertionError((command,state))
    def state():return json.loads(request('j/hymission-tiles-state'))
    def overview():return json.loads(request('j/hymission-overview-state'))
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    def capture(name):
        monitor=json.loads(request('j/monitors'))[0]
        path=root/(name+'.png')
        subprocess.run(['grim','-o',monitor['name'],str(path)],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        return Image.open(path).convert('RGB')
    evaluate('hl.config({animations={enabled=false},plugin={hymission={tiles_enabled=1,tile_min_width=120,workspace_strip_force_show=1,workspace_strip_empty_mode="always"}}}); hl.plugin.hymission_tiles_test.refresh()')
    wait('j/hymission-tiles-state',lambda s:s['enabled'])
    if case=='panel':
        evaluate('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1",reserved={top=80,right=0,bottom=24,left=0}})')
        wait('j/monitors',lambda ms:ms[0]['reserved'][1]>=80)
        evaluate('hl.plugin.hymission.tile_add(); hl.plugin.hymission.tile_add(); hl.plugin.hymission.open("onlycurrentworkspace")')
        ov=wait('j/hymission-overview-state',lambda s:s['phase']=='active')
        (root/'panel-state.json').write_text(json.dumps(ov,indent=2))
        time.sleep(.1)
        image=capture('shared-panel')
        groups={t['id']:[e['geometry'] for e in ov['strip'] if e['tile_id']==t['id']] for t in state()['tiles']}
        centers=sorted(sum(b['x']+b['width']/2 for b in entries)/len(entries) for entries in groups.values())
        # No thumbnails or focus outline in the reserved header: the entire
        # glass panel must continue through it, including across column seams.
        for x in [int(v) for v in centers]+[int((a+b)/2) for a,b in zip(centers,centers[1:])]:
            a=image.getpixel((x,30));b=image.getpixel((x,90+500))
            assert max(abs(u-v) for u,v in zip(a,b))<8,('header/background discontinuity',x,a,b)
        for a,b in zip(centers,centers[1:]):
            seam=int((a+b)/2)
            row=[image.getpixel((x,30)) for x in range(seam-6,seam+7)]
            assert max(max(p[c] for p in row)-min(p[c] for p in row) for c in range(3))<3,('column seam',row)
        # Empty background below the thumbnails must have no focused-column
        # blue frame either (the old frame ran at band.x + 3).
        for entries in groups.values():
            x=int(min(b['x'] for b in entries)-8+3)
            pixel=image.getpixel((x,700))
            assert max(pixel)-min(pixel)<12,('focused tile column outline remains',x,pixel)
        print('PASS: continuous ws-only strip glass panel, no seams/header line/focused-column outline')
        return
    if case in ('gesture','overview','stage'):
        xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
        subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
        subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
        flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
        binary=root/'tiles-marker'
        subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    if case=='gesture':
        evaluate('hl.gesture({fingers=4,direction="vertical",action="workspace"}); hl.animation({leaf="workspaces",enabled=true,speed=8,bezier="default",style="slidevert"}); hl.config({gestures={workspace_swipe_invert=false,workspace_swipe_distance=300,workspace_swipe_cancel_ratio=.05,workspace_swipe_min_speed_to_force=0,workspace_swipe_forever=false}}); hl.plugin.hymission_tiles_test.refresh()')
        for ws in (601,602,603,604):evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}})')
        for ws in (601,602,603):
            evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
            log=open(root/f'gesture-client-{ws}.log','w');handles.append(log)
            processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
            wait('j/clients',lambda cs:len(cs)==ws-600)
        for capability in (0,1):
            evaluate(f'hl.config({{plugin={{hymission={{tiles_enabled={capability},stage_enabled=1,stage_transition_ms=0}}}}}}); hl.plugin.hymission_tiles_test.refresh(); hl.dispatch(hl.dsp.focus({{workspace=601}})); hl.plugin.hymission.stage_open()')
            wait('j/hymission-stage-state',lambda s:s['enabled'] and s['visibility_progress']==1)
            evaluate('hl.plugin.hymission_tiles_test.trackpad("begin"); hl.plugin.hymission_tiles_test.trackpad("update",40)')
            baseline=json.loads(request('j/hymission-stage-state'))
            assert baseline['screens'][0]['swipe_active'] and baseline['screens'][0]['swipe_progress']>0,baseline
            (root/f'gesture-single-{capability}.json').write_text(json.dumps(baseline,indent=2))
            evaluate('hl.plugin.hymission_tiles_test.trackpad("cancel")')
            wait('j/hymission-stage-state',lambda s:not s['screens'][0]['swipe_active'])
        evaluate('hl.plugin.hymission.stage_close(); hl.config({plugin={hymission={stage_enabled=0}}})')
        evaluate('hl.dispatch(hl.dsp.focus({workspace=601})); hl.plugin.hymission.tile_add({workspace="603"}); hl.plugin.hymission.tile_add({workspace="604"}); hl.plugin.hymission.tile_focus("left"); hl.plugin.hymission.tile_focus("left"); hl.config({plugin={hymission={stage_enabled=1,stage_transition_ms=0,stage_show_active=1}}}); hl.plugin.hymission.stage_open()')
        wait('j/hymission-stage-state',lambda s:s['enabled'] and len(s['screens'])==3 and s['visibility_progress']==1)
        evaluate('hl.config({animations={enabled=true}})')
        snapshots=[]
        stage=json.loads(request('j/hymission-stage-state'))
        focused=next(t for t in state()['tiles'] if t['focused'])
        screen=next(sc for sc in stage['screens'] if sc['tile_id']==focused['id'])
        desk=screen.get('desktop_geometry',{'y':0,'height':1080})
        evaluate('hl.plugin.hymission_tiles_test.trackpad("begin")')
        for i in range(1,6):
            evaluate('hl.plugin.hymission_tiles_test.trackpad("update",40)')
            path=root/f'trackpad-{i}.json'
            evaluate('hl.plugin.hymission_tiles_test.animation_report('+json.dumps(str(path))+')')
            frame=json.loads(path.read_text());snapshots.append(frame)
            assert state()['interaction_owner']==2,state()
            assert abs(frame['601']['value'][0])<.01 and abs(frame['601']['value'][1]+i*40/300*desk['height'])<.1,('vertical input not following fingers',i,frame)
            assert abs(frame['602']['alpha']-1)<.01
            for ws in ('603','604'):assert frame[ws]['value']==[0,0] and abs(frame[ws]['alpha']-1)<.01,frame
            image=capture(f'trackpad-{i}')
            focused=next(t for t in state()['tiles'] if t['focused'])
            x=int(desk.get('x',focused['x'])+desk.get('width',focused['width'])*.8)
            expected=desk['y']+(1-i*40/300)*desk['height']
            red=[y for y in range(max(80,int(expected-70)),image.height) if (lambda p:p[0]>120 and p[1]<70 and p[2]<70)(image.getpixel((x,y)))]
            assert red,('offset state changed but no incoming header in the rendered middle frame',i,x)
            assert abs(min(red)-expected)<80,('rendered header did not follow input',i,min(red),expected)
            outside=list(range(80,max(80,int(desk['y'])-4)))+list(range(min(image.height,int(desk['y']+desk['height'])+4),image.height))
            assert not any((lambda p:p[0]>120 or p[1]>120)(image.getpixel((x,y))) for y in outside),('swipe painted outside the common Stage scene',i)
            time.sleep(.02)
        evaluate('hl.plugin.hymission_tiles_test.trackpad("end")')
        wait('j/hymission-tiles-state',lambda s:s['interaction_owner']==0)
        wait('j/activeworkspace',lambda s:s['id']==602)
        (root/'trackpad-samples.json').write_text(json.dumps(snapshots,indent=2))
        print('PASS: four-finger vertical InputManager -> TrackpadGestures -> Hymission wrapper -> unified tile swipe, five following-finger middle frames and stable neighbors')
        return
    if case=='overview':
        for ws in (701,702,703):
            evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
            log=open(root/f'overview-client-{ws}.log','w');handles.append(log)
            processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
            wait('j/clients',lambda cs:len(cs)==ws-700)
        evaluate('hl.dispatch(hl.dsp.focus({workspace=701})); hl.plugin.hymission.tile_add({workspace="702"}); hl.plugin.hymission.tile_add({workspace="703"}); hl.plugin.hymission.tile_focus("left"); hl.plugin.hymission.tile_focus("left")')
        for stage in (0,1):
            evaluate(f'hl.config({{animations={{enabled=false}},plugin={{hymission={{stage_enabled={stage},stage_transition_ms=0}}}}}}); hl.plugin.hymission.stage_'+('open' if stage else 'close')+'()')
            time.sleep(.2)
            before={c['address']:{'x':c['at'][0],'y':c['at'][1],'width':c['size'][0],'height':c['size'][1]} for c in json.loads(request('j/clients'))}
            capture(f'overview-before-stage-{stage}')
            evaluate('hl.config({animations={enabled=true}}); hl.plugin.hymission.open("forceall")')
            first=overview()
            for w in first['windows']:
                origin=w.get('animationFromGeometry',w['contentGeometry']);expected=before[w['address']]
                assert max(abs(origin[k]-expected[k]) for k in expected)<30,('visible tile starts offscreen',stage,w,expected)
            frames=[]
            for i in range(40):
                frame=overview();frames.append(frame)
                for w in frame['windows']:
                    if 'workspaceAlpha' in w:assert abs(w['workspaceAlpha']-1)<.01,w
                    if frame['phase']=='opening' and 'animationToGeometry' in w:
                        for axis in ('x','y'):
                            b=before[w['address']][axis];g=w['animationToGeometry'][axis];v=w['contentGeometry'][axis]
                            assert min(b,g)-2<=v<=max(b,g)+2,(stage,w,axis)
                time.sleep(.007)
            assert sum(f['phase']=='opening' for f in frames)>=3,frames
            capture(f'overview-active-stage-{stage}')
            evaluate('hl.plugin.hymission.close()')
            closing=overview()
            for w in closing['windows']:
                if 'exitGeometry' in w:
                    assert max(abs(w['exitGeometry'][k]-before[w['address']][k]) for k in before[w['address']])<30,('visible tile exits offscreen',w,before)
            for i in range(35):
                frame=overview();frames.append(frame)
                for w in frame['windows']:
                    if 'workspaceAlpha' in w:assert abs(w['workspaceAlpha']-1)<.01,w
                time.sleep(.007)
            wait('j/hymission-overview-state',lambda s:not s['active'])
            (root/f'overview-stage-{stage}-frames.json').write_text(json.dumps(frames,indent=2))
            capture(f'overview-after-stage-{stage}')
            after={c['address']:{'x':c['at'][0],'y':c['at'][1],'width':c['size'][0],'height':c['size'][1]} for c in json.loads(request('j/clients'))}
            assert before==after,(before,after)
        print('PASS: all overview visible tile origins/endpoints, continuous middle frames, alpha and desktop restoration, Stage off/on')
        return
    if case=='stage':
        evaluate('hl.config({general={gaps_in=0,gaps_out=0},plugin={hymission={stage_enabled=0,stage_show_active=1,stage_transition_ms=400}}})')
        for ws in (801,802,803):
            evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
            log=open(root/f'stage-client-{ws}.log','w');handles.append(log)
            processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
            wait('j/clients',lambda cs:len(cs)==ws-800)
        evaluate('hl.dispatch(hl.dsp.focus({workspace=801})); hl.plugin.hymission.tile_add({workspace="802",width=600}); hl.plugin.hymission.tile_add({workspace="803",width=300}); hl.plugin.hymission.tile_focus("left"); hl.plugin.hymission.tile_focus("left")')
        time.sleep(.2);before=json.loads(request('j/clients'));capture('stage-global-before')
        evaluate('hl.config({animations={enabled=true},plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission.stage_open()')
        frames=[]
        for i in range(65):
            s=json.loads(request('j/hymission-stage-state'));frames.append(s)
            screens=s['screens']
            if len(screens)==3 and all(sc['card_width'] for sc in screens):
                ratios=[sc['card_width']/sc['card_height'] for sc in screens]
                assert all(abs(r-1920/1080)<.001 for r in ratios),('card ratio uses tile aspect',ratios)
                assert max(sc['card_height'] for sc in screens)-min(sc['card_height'] for sc in screens)<.01,('unequal card heights',screens)
                if all('desktop_geometry' in sc for sc in screens):
                    scales=[sc['desktop_geometry']['width']/sc['base']['width'] for sc in screens]
                    assert max(scales)-min(scales)<.001,('only one tile compressed',scales)
                    assert all(abs(sc['desktop_geometry']['height']/sc['base']['height']-scales[0])<.001 for sc in screens),screens
            time.sleep(.01)
        s=wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1 and len(s['screens'])==3)
        assert sum(0<f['visibility_progress']<1 for f in frames)>3,frames
        time.sleep(.4);after=json.loads(request('j/clients'));image=capture('stage-global-open')
        for sc in s['screens']:
            desk=sc['desktop_geometry'];original=next(c for c in before if c['workspace']['id']==sc['active_workspace']);current=next(c for c in after if c['address']==original['address'])
            ratio=desk['width']/sc['base']['width']
            assert ratio<.8 and abs(current['size'][0]/original['size'][0]-ratio)<.03 and abs(current['size'][1]/original['size'][1]-ratio)<.04,('real desktop not compressed on both axes',original,current,sc)
            x=desk['x']+desk['width']/2;y=desk['y']+desk['height']/2
            evaluate(f'hl.plugin.hymission_tiles_test.pointer("native",{x},{y})')
            assert json.loads(request('j/activewindow'))['address']==current['address'],('mapped input hit wrong tile',sc)
        (root/'stage-global-frames.json').write_text(json.dumps(frames,indent=2))
        (root/'stage-global-state.json').write_text(json.dumps(s,indent=2))
        # An empty focused tile with empty cards hidden must still use the
        # same scene transform as the columns which contain real windows.
        last=next(c for c in after if c['workspace']['id']==803)
        next(p for p in processes if p.pid==last['pid']).terminate()
        wait('j/clients',lambda cs:len(cs)==2)
        evaluate('hl.config({plugin={hymission={stage_show_empty=0}}}); hl.plugin.hymission_tiles_test.refresh()')
        empty=wait('j/hymission-stage-state',lambda s:len(s['screens'])==3 and any(sc['active_workspace']==803 and not sc['cards'] for sc in s['screens']))
        scales=[sc['desktop_geometry']['width']/sc['base']['width'] for sc in empty['screens']]
        assert max(scales)-min(scales)<.001,('empty focused tile lost the common scene',empty)
        capture('stage-global-empty-focused')
        evaluate('hl.plugin.hymission.stage_close()')
        wait('j/hymission-stage-state',lambda s:s['visibility_progress']==0)
        time.sleep(.5);capture('stage-global-closed')
        print('PASS: monitor-aspect equal-height Stage cards; uniform whole-output tile scaling in middle frames, real windows and mapped input')
        return
    raise AssertionError('case not implemented: '+case)
