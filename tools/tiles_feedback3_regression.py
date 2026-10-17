"""Private native input, ws overview exit and HiDPI scrolling regressions."""
import json
from pathlib import Path
import subprocess
import time
from PIL import Image


def run(case,request,root,env,runtime,processes,handles):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def read(command):return json.loads(request(command))
    def wait(command,predicate):
        for _ in range(200):
            state=read(command)
            if predicate(state):return state
            time.sleep(.025)
        (root/'failure.json').write_text(json.dumps(state,indent=2))
        raise AssertionError((command,state))
    def tiles():return read('j/hymission-tiles-state')['tiles']
    def pointer(action,x=0,y=0):evaluate(f'hl.plugin.hymission_tiles_test.pointer("{action}",{x},{y})')
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    def capture(name):
        monitor=read('j/monitors')[0];path=root/(name+'.png')
        subprocess.run(['grim','-o',monitor['name'],str(path)],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        return Image.open(path).convert('RGB')
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    def spawn(ws):
        evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        before=len(read('j/clients'));log=open(root/f'client-{ws}-{before}.log','w');handles.append(log)
        proc=subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT);processes.append(proc)
        return next(c for c in wait('j/clients',lambda cs:len(cs)==before+1) if c['pid']==proc.pid)
    evaluate('hl.config({animations={enabled=false},plugin={hymission={tiles_enabled=1,tile_min_width=120,stage_enabled=0,vim_keys=1,workspace_strip_force_show=1,workspace_strip_empty_mode="always"}}}); hl.plugin.hymission_tiles_test.refresh()')
    wait('j/hymission-tiles-state',lambda s:s['enabled'])
    for ws in (501,502,503,504):evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}})')
    if case=='hover':
        spawn(501)
        evaluate('hl.plugin.hymission.tile_add({workspace="503"}); hl.plugin.hymission.tile_focus("left")')
        before=tiles();empty=before[1]
        x=empty['x']+empty['width']/2;y=empty['y']+empty['height']/2
        pointer('native',x,y)
        active=read('j/activeworkspace')['id']
        assert active==503,('hover left native routing on wrong tile',active,tiles())
        evaluate('hl.dispatch(hl.dsp.focus({workspace=504}))')
        after=tiles()
        assert after[0]['activeWorkspace']['id']==501 and after[1]['activeWorkspace']['id']==504,(before,after)
        evaluate('hl.plugin.hymission.tile_focus("left"); hl.config({plugin={hymission={stage_enabled=1,stage_transition_ms=0}}}); hl.plugin.hymission.stage_open()')
        stage=wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1 and len(s['screens'])==2)
        b=next(sc['desktop_geometry'] for sc in stage['screens'] if sc['tile_id']==after[1]['id'])
        pointer('native',b['x']+b['width']/2,b['y']+b['height']/2)
        assert read('j/activeworkspace')['id']==504
        evaluate('hl.dispatch(hl.dsp.focus({workspace=503}))')
        assert [t['activeWorkspace']['id'] for t in tiles()]==[501,503]
        print('PASS: hovering empty tile publishes native workspace context before switching, Stage off/on')
        return
    if case=='workspace':
        evaluate('hl.config({general={layout="scrolling"},scrolling={column_width=.85,fullscreen_on_one_column=true,focus_fit_method=1},plugin={hymission={niri_mode=0,stage_transition_ms=0}}})')
        for ws in (501,502,503,504):spawn(ws)
        evaluate('hl.dispatch(hl.dsp.focus({workspace=501})); hl.plugin.hymission.tile_add({workspace="503"}); hl.dispatch(hl.dsp.focus({workspace=504})); hl.dispatch(hl.dsp.focus({workspace=503})); hl.plugin.hymission.tile_focus("left")')
        for stage in (0,1):
            evaluate('hl.config({plugin={hymission={stage_enabled='+str(stage)+'}}}); hl.plugin.hymission.stage_'+('open' if stage else 'close')+'()')
            for side,target in ((0,502),(1,504)):
                original=501 if side==0 else 503
                for finish in ('close','escape'):
                    evaluate(f'hl.plugin.hymission.tile_focus("{"left" if side==0 else "right"}"); hl.dispatch(hl.dsp.focus({{workspace={original}}})); hl.plugin.hymission.open("onlycurrentworkspace")')
                    ov=wait('j/hymission-overview-state',lambda s:s['phase']=='active' and not s.get('relayout_active'))
                    entry=next(e for e in ov['strip'] if e['workspace']==target and e['tile_id']==tiles()[side]['id'])
                    b=entry['geometry'];x=b['x']+b['width']/2;y=b['y']+b['height']/2
                    pointer('press',x,y);pointer('release',x,y)
                    wait('j/hymission-tiles-state',lambda s:s['tiles'][side]['activeWorkspace']['id']==target)
                    wait('j/hymission-overview-state',lambda s:s['phase']=='active' and not s.get('relayout_active'))
                    if finish=='escape':pointer('escape')
                    else:evaluate('hl.plugin.hymission.close()')
                    wait('j/hymission-overview-state',lambda s:not s['active'])
                    time.sleep(.15)
                    image=capture(f'ws-stage-{stage}-side-{side}-{finish}')
                    path=root/f'ws-state-{stage}-{side}-{finish}.json'
                    evaluate('hl.plugin.hymission_tiles_test.animation_report('+json.dumps(str(path))+')')
                    report=json.loads(path.read_text())
                    assert tiles()[side]['activeWorkspace']['id']==target,tiles()
                    assert report[str(target)]['alpha']>.99 and report[str(target)]['value']==[0,0],('blank workspace rendering state',target,report)
                    client=next(c for c in read('j/clients') if c['workspace']['id']==target)
                    monitor=read('j/monitors')[0];scale=monitor['scale']
                    # An actual occupied target must draw after teardown, not
                    # merely report the expected native workspace identity.
                    b={'x':client['at'][0],'y':client['at'][1],'width':client['size'][0],'height':client['size'][1]}
                    tile=tiles()[side]
                    if stage:
                        sc=next(sc for sc in read('j/hymission-stage-state')['screens'] if sc['tile_id']==tile['id'])
                        tile=sc['desktop_geometry']
                    left=max(b['x'],tile['x']);right=min(b['x']+b['width'],tile['x']+tile['width']);top=max(b['y'],tile['y']);bottom=min(b['y']+b['height'],tile['y']+tile['height'])
                    assert right-left>2 and bottom-top>2,('target window outside its visible desktop',client,tile)
                    pixel=image.getpixel((int((left+right)/2*scale),int((top+bottom)/2*scale)))
                    assert pixel[1]>120 and pixel[0]<90,('target workspace is visually blank after ws strip exit',stage,side,finish,pixel,client,tile)
        print('PASS: both tile strip switches survive ws overview teardown with visible rendered clients, Stage off/on and Esc/close')
        return
    if case in ('stage','stage-multi'):
        evaluate('hl.monitor({output="",mode="6144x3456@60",position="auto",scale="2",reserved={top=58,right=0,bottom=0,left=0}}); hl.config({general={layout="scrolling"},scrolling={column_width=.85,fullscreen_on_one_column=true,focus_fit_method=1},plugin={hymission={niri_mode=0,stage_transition_ms=0}}})')
        wait('j/monitors',lambda ms:ms[0]['scale']==2 and ms[0]['width']==6144)
        for ws in (501,502,503,504):
            spawn(ws)
            if case=='stage-multi':
                spawn(ws)
                spawn(ws)
        evaluate('hl.dispatch(hl.dsp.focus({workspace=501})); hl.plugin.hymission.tile_add({workspace="503"}); hl.dispatch(hl.dsp.focus({workspace=504})); hl.dispatch(hl.dsp.focus({workspace=503})); hl.plugin.hymission.tile_focus("left")')
        time.sleep(.15);before_stage=read('j/clients')
        evaluate('hl.config({plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission.stage_open()')
        s=wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1 and len(s['screens'])==2 and all(sc['cards'] and all(c.get('windows') for c in sc['cards']) for sc in s['screens']))
        time.sleep(.2);s=read('j/hymission-stage-state')
        image=capture('stage-hidpi-scrolling')
        if case=='stage-multi':
            (root/'stage-multi-state.json').write_text(json.dumps(s,indent=2))
            capture('stage-multi-scrolling')
            # A workspace card must not expose pieces of scrolling columns
            # beyond its mapped tile viewport in the letterbox margins.
            for sc in s['screens']:
                desk=sc['desktop_geometry']
                for c in sc['cards']:
                    z=min(c['width']/desk['width'],c['height']/desk['height'])
                    left=c['x']+(c['width']-desk['width']*z)/2
                    right=left+desk['width']*z
                    assert any(w['x']+w['width']>left+2 and w['x']<right-2 for w in c['windows']),('occupied Stage workspace completely outside its own viewport',sc['tile_id'],c)
                    visible=max(c['windows'],key=lambda w:max(0,min(right,w['x']+w['width'])-max(left,w['x'])))
                    px=(max(left,visible['x'])+min(right,visible['x']+visible['width']))/2
                    pixel=image.getpixel((int(px*2),int((c['y']+c['height']/2)*2)))
                    assert pixel[1]>180 and pixel[0]<80,('occupied Stage viewport rendered blank',sc['tile_id'],c,pixel)
                    samples=[image.getpixel((int(x*2),int((c['y']+c['height']/2)*2)))
                        for x in (left-12,right+12) if c['x']+2<x<c['x']+c['width']-2]
                    assert all(p[1]<150 for p in samples),('off-viewport scrolling window leaked into card padding',sc['tile_id'],c,samples)
            (root/'stage-multi-native-before.json').write_text(json.dumps(before_stage,indent=2))
            # Repeated open/close must not accumulate camera scaling errors.
            for cycle in range(3):
                evaluate('hl.plugin.hymission.stage_close()')
                wait('j/hymission-stage-state',lambda s:s['visibility_progress']==0)
                time.sleep(.15);closed=read('j/clients')
                for before in before_stage:
                    current=next(c for c in closed if c['address']==before['address'])
                    assert max(abs(x-y) for x,y in zip(before['at']+before['size'],current['at']+current['size']))<=3,('scroll camera drift after Stage cycle',cycle,before,current)
                evaluate('hl.plugin.hymission.stage_open()')
                wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1)
                time.sleep(.15)
            for width,fraction in (('40%',.4),('50%',.5)):
                evaluate('hl.plugin.hymission.tile_focus("left"); hl.plugin.hymission.tile_resize({width="'+width+'"})')
                wait('j/hymission-stage-state',lambda s:len(s['screens'])==2 and abs(s['screens'][0]['base']['width']-3072*fraction)<1)
                time.sleep(.2);resized=read('j/hymission-stage-state');capture('stage-multi-resized-'+width[:-1])
                for sc in resized['screens']:
                    desk=sc['desktop_geometry']
                    for c in sc['cards']:
                        z=min(c['width']/desk['width'],c['height']/desk['height'])
                        left=c['x']+(c['width']-desk['width']*z)/2;right=left+desk['width']*z
                        assert any(w['x']+w['width']>left+2 and w['x']<right-2 for w in c['windows']),('scroll camera lost workspace during tile resize',width,sc,c)
            fresh=read('j/hymission-stage-state')
            sc=next(sc for sc in fresh['screens'] if sc['tile_id']==tiles()[1]['id'])
            card=next(c for c in sc['cards'] if c['workspace']==504)
            candidate=max(card['windows'],key=lambda w:w['x'])
            evaluate('hl.plugin.hymission.open("forceall")')
            ov=wait('j/hymission-overview-state',lambda s:s['phase']=='active' and not s['relayout_active'])
            w=next(w for w in ov['windows'] if w['address']==candidate['address'])
            assert abs(w['animationFromGeometry']['x']-candidate['x'])<3,('right-column Stage origin clipped by tile home instead of output',candidate,w)
            evaluate('hl.plugin.hymission.close()');wait('j/hymission-overview-state',lambda s:not s['active'])
            print('PASS: multi-window Stage visibility/clipping, three camera cycles, tile resize and right-column overview origin')
            return
        for sc in s['screens']:
            for c in sc['cards']:
                ws=c['workspace']
                if len(c.get('windows',[]))!=1:continue
                w=c['windows'][0]
                # A one-window workspace should be centered in its own card,
                # independent of the workspace's global tile origin.
                assert abs(w['x']+w['width']/2-(c['x']+c['width']/2))<c['width']*.12,('preview retains another tile global x offset',sc,c)
                assert c['y']-1<=w['y'] and w['y']+w['height']<=c['y']+c['height']+1,('cropped Stage content',c)
                px=int((w['x']+w['width']/2)*2)
                assert image.getpixel((px,int((w['y']+w['height']/2)*2)))[1]>180,('preview not rendered in its card',c)
                assert any((lambda rgb:rgb[0]>180 and rgb[1]<80)(image.getpixel((px,y))) for y in range(int(c['y']*2),int((w['y']+20)*2))),('CSD header clipped in Stage card',c)
        clients=read('j/clients')
        for sc in s['screens']:
            b=sc['desktop_geometry'];base=sc['base'];zoom=b['width']/base['width']
            # Native non-covering scrolling fullscreen uses logicalBox(), not
            # workArea(). It must see the same scaled monitor as the desktop.
            top=b['y']-base['y']*zoom;height=1728*zoom
            c=next(c for c in clients if c['workspace']['id']==sc['active_workspace'])
            assert c['at'][1]>=top-2 and c['at'][1]+c['size'][1]<=top+height+2,('scrolling fullscreen ignored Stage transform',sc,c)
        (root/'stage-hidpi-scrolling-clients.json').write_text(json.dumps(clients,indent=2))
        (root/'stage-hidpi-scrolling.json').write_text(json.dumps(s,indent=2))
        print('PASS: scale=2 scrolling workspaces centered inside monitor-aspect Stage cards with complete vertical content')
        return
    if case=='drag':
        evaluate('hl.config({binds={drag_threshold=0},general={layout="dwindle"}})')
        a=spawn(501)
        evaluate('hl.plugin.hymission.tile_add({workspace="503"}); hl.plugin.hymission.tile_focus("left")')
        def native_drag(address,x,y,dx,dy):
            evaluate('hl.plugin.hymission_tiles_test.drag("begin",'+json.dumps(address)+f',{x},{y})')
            evaluate(f'hl.plugin.hymission_tiles_test.drag("move",{dx},{dy})')
            time.sleep(.1)
        for stage in (0,1):
            evaluate('hl.config({plugin={hymission={stage_enabled='+str(stage)+'}}}); hl.plugin.hymission.stage_'+('open' if stage else 'close')+'()')
            time.sleep(.2)
            for floating in (False,True):
                evaluate('hl.plugin.hymission_tiles_test.window("'+("float" if floating else "tile")+'",'+json.dumps(a["address"])+')')
                for side in (1,0):
                    c=next(c for c in read('j/clients') if c['address']==a['address'])
                    tile=tiles()[side];b=tile
                    if stage:
                        b=next(sc['desktop_geometry'] for sc in read('j/hymission-stage-state')['screens'] if sc['tile_id']==tile['id'])
                    dx=b['x']+b['width']/2;dy=b['y']+b['height']/2
                    native_drag(a['address'],c['at'][0]+c['size'][0]/2,c['at'][1]+c['size'][1]/2,dx,dy)
                    image=capture(f'drag-stage-{stage}-float-{int(floating)}-side-{side}')
                    (root/f'drag-state-{stage}-{int(floating)}-{side}.json').write_text(json.dumps({'clients':read('j/clients'),'tiles':read('j/hymission-tiles-state'),'stage':read('j/hymission-stage-state')},indent=2))
                    pixel=image.getpixel((int(dx+25),int(dy+25)))
                    assert pixel[1]>180 and pixel[0]<80,('dragged window clipped at its source tile',stage,floating,side,pixel)
                    assert read('j/hymission-tiles-state')['interaction_owner']==0,('native drag acquired another animation owner',tiles())
                    evaluate('hl.plugin.hymission_tiles_test.drag("end")')
                    cs=read('j/clients');c=next(c for c in cs if c['address']==a['address'])
                    assert c['workspace']['id']==tile['activeWorkspace']['id'],('native drag did not transfer tile workspace',stage,floating,side,c,tiles())
                    assert c['floating']==floating,('native drag changed tiling state',c)
                    assert [t['activeWorkspace']['id'] for t in tiles()]==[501,503],tiles()
        # Cancellation must not publish a cross-tile workspace transaction.
        c=next(c for c in read('j/clients') if c['address']==a['address'])
        right=next(sc['desktop_geometry'] for sc in read('j/hymission-stage-state')['screens'] if sc['tile_id']==tiles()[1]['id'])
        native_drag(a['address'],c['at'][0]+c['size'][0]/2,c['at'][1]+c['size'][1]/2,right['x']+right['width']/2,right['y']+right['height']/2)
        pointer('escape');evaluate('hl.plugin.hymission_tiles_test.drag("end")')
        assert next(c for c in read('j/clients') if c['address']==a['address'])['workspace']['id']==501
        # A desktop target must also work in ws overview when it is empty and
        # therefore has no thumbnail hit region.
        evaluate('hl.plugin.hymission.stage_close(); hl.config({plugin={hymission={stage_enabled=0}}})')
        for side in (1,0):
            evaluate('hl.plugin.hymission.open("onlycurrentworkspace")')
            ov=wait('j/hymission-overview-state',lambda s:s['phase']=='active' and not s['relayout_active'] and s['windows'])
            w=next(w for w in ov['windows'] if w['address']==a['address']);b=w['contentGeometry']
            x=b['x']+b['width']/2;y=b['y']+b['height']/2
            tile=tiles()[side];dx=tile['x']+tile['width']/2;dy=tile['y']+tile['height']/2
            pointer('press',x,y);pointer('move',x+40,y+30);pointer('move',dx,dy)
            time.sleep(.12);capture(f'overview-drag-to-empty-{side}')
            pointer('release',dx,dy)
            wait('j/clients',lambda cs:next(c for c in cs if c['address']==a['address'])['workspace']['id']==tile['activeWorkspace']['id'])
            evaluate('hl.plugin.hymission.close()');wait('j/hymission-overview-state',lambda s:not s['active'])
        # An occupied destination uses the same native drag completion path.
        spawn(503);evaluate('hl.plugin.hymission.tile_focus("left")')
        c=next(c for c in read('j/clients') if c['address']==a['address'])
        native_drag(a['address'],c['at'][0]+c['size'][0]/2,c['at'][1]+c['size'][1]/2,1440,540)
        evaluate('hl.plugin.hymission_tiles_test.drag("end")')
        assert next(c for c in read('j/clients') if c['address']==a['address'])['workspace']['id']==503
        print('PASS: native tiled/floating drags both ways, Stage off/on, Esc cancellation, occupied destination and ws overview empty-tile drops')
        return
    raise AssertionError('unimplemented '+case)
