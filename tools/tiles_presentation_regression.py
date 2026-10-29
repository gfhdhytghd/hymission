"""Private real-client fullscreen/float/drag scene and input regressions (FB8)."""
import json
from pathlib import Path
import subprocess
import time
from PIL import Image


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
    results=[]
    def check(ok,label):
        results.append(dict(check=label,passed=bool(ok)));(root/'results.json').write_text(json.dumps(results,indent=2))
        if not observe:assert ok,label
    def pointer(action,x=0,y=0):evaluate(f'hl.plugin.hymission_tiles_test.pointer("{action}",{x},{y})')
    def window(action,address,*args):evaluate('hl.plugin.hymission_tiles_test.window('+','.join([json.dumps(action),json.dumps(address),*map(str,args)])+')')
    def client(address):return next(c for c in read('j/clients') if c['address']==address)
    def tiles():return read('j/hymission-tiles-state')['tiles']
    child=env.copy();child.pop('LD_PRELOAD',None);child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    def capture(name):
        path=root/(name+'.png');subprocess.run(['grim','-o',read('j/monitors')[0]['name'],str(path)],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        (root/(name+'.json')).write_text(json.dumps(dict(clients=read('j/clients'),tiles=read('j/hymission-tiles-state'),stage=read('j/hymission-stage-state'),overview=read('j/hymission-overview-state')),indent=2))
        return Image.open(path).convert('RGB')
    def green(im,x,y):
        r,g,b=im.getpixel((int(x),int(y)));return g>150 and r<100 and b<100
    def record(name,code):
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin"); '+code);time.sleep(.7)
        prefix=root/name;evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(prefix))+')')
        frames=json.loads(prefix.with_suffix('.json').read_text())
        for f in frames:
            path=Path(f['pixels']);im=Image.open(path);target=path.with_suffix('.png');im.save(target);path.unlink();f['pixels']=str(target)
        prefix.with_suffix('.json').write_text(json.dumps(frames,indent=2));check(len(frames)>=3,name+' has intermediate frames');return frames
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True);subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split();binary=root/'marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    evaluate('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1",reserved={top=40,bottom=20,left=0,right=0}}); hl.config({general={layout="dwindle"},animations={enabled=false},binds={drag_threshold=0},plugin={hymission={tiles_enabled=1,tile_min_width=120,niri_mode=0,stage_enabled=0,stage_transition_ms=0,stage_show_active=1,overview_focus_follows_mouse=0,hover_expand_scale=1}}}); hl.plugin.hymission_tiles_test.refresh()')
    clients={}
    for ws in range(1201,1207):
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        color=child.copy();color['HMT_MARKER_COLOR']='00ff00' if ws in (1201,1202) else '2030d0'
        log=open(root/f'client-{ws}.log','w');handles.append(log);proc=subprocess.Popen([str(binary)],env=color,stdout=log,stderr=subprocess.STDOUT);processes.append(proc)
        clients[ws]=next(c['address'] for c in wait('j/clients',lambda cs:len(cs)==ws-1200) if c['pid']==proc.pid)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1201})); hl.plugin.hymission.tile_add({workspace="1203"})')
    if case=='three':evaluate('hl.plugin.hymission.tile_add({workspace="1205"})')
    address=clients[1201];window('focus',address)
    for layout in ('dwindle','scrolling'):
        evaluate(f'hl.workspace_rule({{workspace="1201",layout="{layout}"}}); hl.plugin.hymission_tiles_test.refresh()')
        time.sleep(.15)
        for stage in (0,1):
            evaluate('hl.plugin.hymission.stage_'+('open' if stage else 'close')+'()');time.sleep(.15)
            before=client(address);bindings=[t['activeWorkspace']['id'] for t in tiles()]
            evaluate('hl.config({animations={enabled=true}})')
            frames=record(f'{layout}-stage{stage}-enter',f'hl.plugin.hymission_tiles_test.window("fullscreen","{address}")')
            c=client(address);check(c['at']==[0,0] and c['size']==[1920,1080],f'{layout}/stage{stage}: true fullscreen covers physical monitor')
            sizes=[n['size'][0] for f in frames for n in f['native'] if n.get('address')==address]
            check(any(before['size'][0]+2<w<1918 for w in sizes),f'{layout}/stage{stage}: fullscreen enter animates through intermediate width')
            im=capture(f'{layout}-stage{stage}-fullscreen')
            check(all(green(im,x,500) for x in (5,500,1000,1500,1915)),f'{layout}/stage{stage}: fullscreen covers all tiles and sidebar pixels')
            if not observe:
                for x in (10,950,1910):window('assert-hit',address,x,500)
            check([t['activeWorkspace']['id'] for t in tiles()]==bindings,'fullscreen keeps tile workspace bindings')
            evaluate('hl.plugin.hymission.tile_focus("right")');time.sleep(.1)
            check(client(address)['size']==[1920,1080],'other-tile focus does not shrink fullscreen owner')
            check(read('j/activewindow')['address']==address,'physical fullscreen retains keyboard focus')
            window('focus',address)
            # Overview may temporarily override presentation; real fullscreen must survive it.
            evaluate('hl.plugin.hymission.open("forceall")');wait('j/hymission-overview-state',lambda s:s['phase']=='active');capture(f'{layout}-stage{stage}-overview')
            evaluate('hl.plugin.hymission.close()');wait('j/hymission-overview-state',lambda s:not s['active']);time.sleep(.1)
            check(client(address)['fullscreen']==2,'overview exit retains fullscreen mode')
            frames=record(f'{layout}-stage{stage}-exit',f'hl.plugin.hymission_tiles_test.window("normal","{address}")')
            c=client(address);check(c['workspace']['id']==1201 and c['size'][0]<1920,'fullscreen exit restores source workspace and tile geometry')
            evaluate('hl.config({animations={enabled=false}})')
            window('layout-fullscreen',address);time.sleep(.15);c=client(address)
            check(c['at']==[0,0] and c['size']==[1920,1080],f'{layout}/stage{stage}: layout-aware explicit fullscreen uses physical monitor')
            capture(f'{layout}-stage{stage}-layout-aware-fullscreen');window('normal',address)
            for mode in ('maximize','internal-fullscreen','layout-internal-fullscreen'):
                window(mode,address);time.sleep(.15);c=client(address);tile=next(t for t in tiles() if t['activeWorkspace']['id']==1201)
                check(c['size'][0]<=tile['width']+1,mode+' remains bounded by home tile')
                if 'internal' in mode:check(c['fullscreen']==2 and c['fullscreenClient']==0,mode+' has real internal-only fullscreen state')
                capture(f'{layout}-stage{stage}-{mode}');window('normal',address)
    evaluate('hl.plugin.hymission.stage_close(); hl.config({animations={enabled=false}})');window('focus',address);window('float',address)
    window('place',address,200,300,400,360);time.sleep(.1)
    floating_before=client(address)
    evaluate('hl.config({animations={enabled=true}})')
    record('floating-fullscreen-enter',f'hl.plugin.hymission_tiles_test.window("fullscreen","{address}")')
    check(client(address)['size']==[1920,1080],'floating fullscreen covers the complete physical monitor')
    record('floating-fullscreen-exit',f'hl.plugin.hymission_tiles_test.window("normal","{address}")')
    floating_after=client(address)
    check(floating_after['workspace']['id']==1201 and floating_after['size']==floating_before['size'] and floating_after['at']==floating_before['at'],'floating fullscreen exit preserves native restore animation, original tile and geometry')
    evaluate('hl.config({animations={enabled=false}})')
    # Center-controlled ownership independent of pointer/native drag, including resize.
    boundary=tiles()[0]['x']+tiles()[0]['width'];destination=tiles()[1]['activeWorkspace']['id']
    window('place',address,boundary-250,300,400,360);time.sleep(.2)
    im=capture('float-straddling-left-center')
    check(green(im,boundary+80,500),'float is fully visible beyond source tile edge')
    check(client(address)['workspace']['id']==1201,'floating center left retains source workspace')
    window('place',address,boundary-100,300,400,360);time.sleep(.3)
    check(client(address)['workspace']['id']==destination,'floating center crossing changes workspace to destination tile')
    check(read('j/activeworkspace')['id']==destination and read('j/activewindow')['address']==address,'focused floating rehome follows tile without stealing keyboard focus')
    window('focus',address)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1204}))');time.sleep(.2)
    im=capture('float-hidden-with-owner-workspace')
    check(not green(im,boundary+100,500),'floating client hides with its owning workspace')
    evaluate('hl.dispatch(hl.dsp.focus({workspace='+str(destination)+'}))');time.sleep(.2)
    im=capture('float-owner-workspace-restored')
    check(green(im,boundary+100,500),'floating client reappears with its owning workspace')
    window('focus',address)
    window('place',address,boundary-100,300,100,360);time.sleep(.3)
    check(client(address)['workspace']['id']==1201,'resizing across center boundary updates workspace ownership')
    check(read('j/activeworkspace')['id']==1201,'focused float resize restores the source tile as workspace context')
    # Opaque blue neighbor must never cover the green dragged client.
    for stage in (0,1):
        evaluate('hl.plugin.hymission.stage_'+('open' if stage else 'close')+'()');time.sleep(.15)
        window('focus',address);c=client(address);sx=c['at'][0]+c['size'][0]/2;sy=c['at'][1]+c['size'][1]/2
        evaluate(f'hl.plugin.hymission_tiles_test.drag("begin","{address}",{sx},{sy}); hl.plugin.hymission_tiles_test.drag("move",1500,550)');time.sleep(.2)
        c=client(address);im=capture(f'native-drag-stage{stage}')
        check(green(im,1530,580),'native drag above occupied destination, Stage '+str(stage))
        if stage:
            evaluate('hl.plugin.hymission_tiles_test.drag("move",100,550)');time.sleep(.15);im=capture('native-drag-over-sidebar')
            check(green(im,130,580),'native drag above Stage sidebar')
        evaluate('hl.plugin.hymission_tiles_test.drag("end")')
        # Restore left for the next interaction without changing native float policy.
        window('workspace',address,1201);window('focus',address)
        window('place',address,200,300,400,360);time.sleep(.2)
    evaluate('hl.plugin.hymission.stage_close(); hl.plugin.hymission.open("onlycurrentworkspace")');ov=wait('j/hymission-overview-state',lambda s:s['phase']=='active')
    time.sleep(.35);ov=read('j/hymission-overview-state')
    w=next(w for w in ov['windows'] if w['address']==address);b=w['contentGeometry'];sx=b['x']+b['width']/2;sy=b['y']+b['height']/2
    pointer('move',sx,sy);time.sleep(.25)
    ov=read('j/hymission-overview-state');w=next(w for w in ov['windows'] if w['address']==address);b=w['contentGeometry'];sx=b['x']+b['width']/2;sy=b['y']+b['height']/2
    pointer('press',sx,sy);pointer('move',sx+30,sy+30);pointer('move',1500,550);time.sleep(.2);im=capture('overview-drag')
    check(green(im,1488,560),'overview ghost above destination tile contents')
    pointer('move',200,550);time.sleep(.15);im=capture('overview-drag-over-strip')
    check(green(im,188,560),'overview ghost above strip');pointer('escape');pointer('release',1500,550)
    evaluate('hl.plugin.hymission.close()');wait('j/hymission-overview-state',lambda s:not s['active'])
    evaluate('hl.plugin.hymission.stage_open()');time.sleep(.2)
    # Use the hidden green workspace to exercise the Stage-owned transfer ghost.
    stage=read('j/hymission-stage-state');sc,c,w=next((sc,c,w) for sc in stage['screens'] for c in sc['cards'] for w in c.get('windows',[]) if w['address']==clients[1202])
    sx=w['x']+w['width']/2;sy=w['y']+w['height']/2
    pointer('press',sx,sy);pointer('move',1500,550);time.sleep(.2);im=capture('stage-drag')
    check(green(im,1530,580),'Stage ghost above destination tile contents')
    pointer('move',200,550);time.sleep(.15);im=capture('stage-drag-over-sidebar')
    check(green(im,230,580),'Stage ghost above all sidebar columns');pointer('escape');pointer('release',1500,550)
    check(read('j/hymission-tiles-state')['interaction_owner']==0,'all drag paths release shared owner')
    window('focus',address)
    stage=read('j/hymission-stage-state');sc,c,w=next((sc,c,w) for sc in stage['screens'] for c in sc['cards'] for w in c.get('windows',[]) if w['address']==clients[1202])
    sx=w['x']+w['width']/2;sy=w['y']+w['height']/2
    pointer('press',sx,sy);pointer('move',1500,550);time.sleep(.1)
    window('fullscreen',address);time.sleep(.2)
    check(read('j/hymission-tiles-state')['interaction_owner']==0,'physical fullscreen cancels a held Stage transfer')
    im=capture('fullscreen-interrupts-stage-drag');check(green(im,1500,500),'fullscreen interrupt renders without a stale drag ghost')
    pointer('release',1500,550);check(client(clients[1202])['workspace']['id']==1202,'fullscreen interrupt never commits hidden Stage drop')
    window('normal',address)
    evaluate('hl.plugin.hymission.stage_close()')
    window('pin',address);window('place',address,500,300,400,360)
    blue=clients[1203];window('float',blue);window('focus',blue);window('place',blue,500,300,400,360);time.sleep(.25)
    im=capture('pinned-above-ordinary-float')
    check(green(im,700,500),'native pinned plane remains above ordinary floating stack')
    window('unpin',address)
    print(('OBSERVED' if observe else 'PASS')+': '+case+' physical fullscreen/maximize/internal FS; float center; native/overview/Stage drag pixels')
