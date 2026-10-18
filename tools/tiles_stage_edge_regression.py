"""Private fixed-geometry desktop edge pixel controls."""
import json,subprocess,time,math,os
from pathlib import Path
from PIL import Image

def verify_flight_pixels(root,count,observe=False,prefix="switch"):
    root=Path(root);samples=[]
    if count>1:
        ws=(902 if prefix=="switch" else 901)+2*(count-1)
        for frame in json.loads((root/f'{prefix}-{count-1}.json').read_text()):
            sc=frame['stage']['screens'][count-1]
            if not .85<sc['transition_progress']<1:continue
            image=Image.open(frame['pixels']).convert('RGB')
            for flight in sc['flights']:
                if flight['workspace']!=ws or not flight.get('viewport'):continue
                v=flight['viewport'];b=flight['frame'];edge=v['x']+v['width']
                x=math.floor(edge)-2;y=round(b['y']+b['height']/2)
                if not (b['x']+50<edge<b['x']+b['width']-20 and 36<x<image.width and 0<y<image.height):continue
                pixel=image.getpixel((x,y));clear=image.getpixel((x-36,y))
                if clear[1]<240 or clear[2]>10:continue
                samples.append(dict(frame=frame['pixels'],phase=sc['transition_progress'],point=[x,y],edge=pixel,clear=clear,right_band=flight['viewport_edges'][1]))
        passed=bool(samples) and all(r['edge'][2]>5 and r['edge'][1]<245 for r in samples)
        if not observe:assert passed,('incoming right edge raster is a hard cut',samples)
    else:passed=True
    result=dict(passed=passed,samples=samples)
    (root/('flight-edge-pixels.json' if prefix=='switch' else 'swipe-edge-pixels.json')).write_text(json.dumps(result,indent=2));return result

def run(case,observe,request,root,env,runtime,processes,handles):
    results=[]
    def evaluate(code):
        reply=request('/eval '+code)
        assert reply.strip()=='ok',(code,reply)
    def read(cmd):return json.loads(request(cmd))
    def check(ok,label):
        results.append(dict(check=label,passed=bool(ok)))
        (root/'results.json').write_text(json.dumps(results,indent=2))
        if not observe:assert ok,label
    def wait(cmd,predicate):
        for _ in range(300):
            value=read(cmd)
            if predicate(value):return value
            time.sleep(.02)
        raise AssertionError((cmd,value))
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    configuration='hl.monitor({output="",mode="1280x720@60",position="auto",scale="1"}); hl.config({misc={background_color="rgb(6080b0)"},input={follow_mouse=0},general={layout="scrolling",border_size=5},scrolling={column_width=.55,fullscreen_on_one_column=false,focus_fit_method=0},animations={enabled=false},decoration={rounding=20,shadow={enabled=true,range=24,color="rgba(000000cc)"}},plugin={hymission={tiles_enabled=1,tile_min_width=100,niri_mode=0,stage_enabled=0,stage_show_active=0,stage_transition_ms=450,workspace_strip_force_show=0,overview_focus_follows_mouse=0,hover_expand_scale=1}}}); hl.plugin.hymission_tiles_test.refresh()'

    first=os.getenv('HYMISSION_EDGE_STAGE_FIRST')=='1'
    if first:configuration=configuration.replace('tiles_enabled=1','tiles_enabled=0').replace('stage_enabled=0','stage_enabled=1')
    evaluate(configuration)
    if first:evaluate('hl.plugin.hymission.stage_open(); hl.config({plugin={hymission={tiles_enabled=1}}}); hl.plugin.hymission_tiles_test.refresh()')
    count={'off':1,'single':1,'two':2,'three':3}[case]
    centers=[]
    for tile in range(count):
        for ws,n in ((901+tile*2,5),(902+tile*2,5)):
            evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
            addresses=[]
            for i in range(n):
                before={c['address'] for c in read('j/clients')}
                log=open(root/f'client-{ws}-{i}.log','w');handles.append(log)
                processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
                clients=wait('j/clients',lambda cs:len(cs)==len(before)+1)
                addresses.append(next(c['address'] for c in clients if c['address'] not in before))
            if n==5:centers.append(addresses[2])
    evaluate('hl.dispatch(hl.dsp.focus({workspace=901}))')
    for tile in range(1,count):evaluate(f'hl.plugin.hymission.tile_add({{workspace="{901+tile*2}"}})')
    if case=='off':evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')


    def record(name,code='',duration=.8):
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin"); '+code)
        time.sleep(duration)
        prefix=root/name
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(prefix))+')')
        frames=json.loads(prefix.with_suffix('.json').read_text())
        for f in frames:
            p=Path(f['pixels']);q=p.with_suffix('.png');Image.open(p).save(q);p.unlink();f['pixels']=str(q)
        prefix.with_suffix('.json').write_text(json.dumps(frames,indent=2));return frames
    evaluate('hl.plugin.hymission.stage_open()')
    time.sleep(.2)
    stage=read('j/hymission-stage-state')
    (root/'stage.json').write_text(json.dumps(stage,indent=2))
    for screen in stage['screens']:
        desktop=screen['desktop_geometry']; ws=screen['active_workspace']
        clients=[c for c in read('j/clients') if c['workspace']['id']==ws]
        # Fixed native geometry isolates the raster gradient from layout/camera
        # policy. The second phase below uses unmodified native scrolling.
        for i,c in enumerate(clients):
            x=desktop['x']-100 if i==0 else desktop['x']+desktop['width']-100 if i==1 else -4000-i*300
            evaluate(f'hl.plugin.hymission_tiles_test.window("place","{c["address"]}",{x},150,200,400)')
    time.sleep(.15)
    images={}
    for width in (32,0):
        evaluate('hl.config({plugin={hymission={stage_scrolling_desktop_edge_width='+str(width)+'}}})')
        images[width]=Image.open(record('static-'+str(width),duration=.14)[-1]['pixels']).convert('RGB')
    static=[]
    for sc in stage['screens']:
        d=sc['desktop_geometry']
        for side in ('left','right'):
            # Off/single Stage only softens its sidebar-facing (left) edge.
            if count==1 and side=='right':continue
            x=math.ceil(d['x'])+2 if side=='left' else math.floor(d['x']+d['width'])-3
            soft=images[32].getpixel((x,350));hard=images[0].getpixel((x,350))
            changed=sum(abs(a-b) for a,b in zip(soft,hard))
            bx=math.ceil(d['x'])+8 if side=='left' else math.floor(d['x']+d['width'])-9
            def mixed_rows(image):
                result=[]
                for y in range(180,210):
                    r,g,b=image.getpixel((bx,y))
                    # Remove the known blue wallpaper contribution. A real
                    # vertical filter mixes the marker's sharp red/green edge;
                    # alpha alone only mixes either marker color with wallpaper.
                    if min(r-b*96/176,g-b*128/176)>20:result.append(y)
                return result
            blurred=mixed_rows(images[32]);sharp=mixed_rows(images[0])
            check(len(blurred)>=4 and not sharp,f'static tile {sc["tile_id"]} {side}: Gaussian mixes sharp client header, hard control does not')
            static.append(dict(tile=sc['tile_id'],side=side,soft=soft,hard=hard,difference=changed,blurred_rows=blurred,hard_mixed_rows=sharp))
            check(changed>80,f'static tile {sc["tile_id"]} {side}: real alpha/blur band differs from hard clip')
    (root/'static-pixels.json').write_text(json.dumps(static,indent=2))
    evaluate('hl.config({plugin={hymission={stage_scrolling_desktop_edge_width=32}}}); hl.plugin.hymission.stage_close()')
    if case!='off':
        evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
        wait('j/hymission-tiles-state',lambda s:not s['enabled'])
        evaluate('hl.dispatch(hl.dsp.focus({workspace=901}))')
        evaluate('hl.config({plugin={hymission={tiles_enabled=1}}}); hl.plugin.hymission_tiles_test.refresh()')
        wait('j/hymission-tiles-state',lambda s:s['enabled'])
        for tile in range(1,count):evaluate(f'hl.plugin.hymission.tile_add({{workspace="{901+tile*2}"}})')
    for tile in range(count):
        source=901+tile*2
        evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={source}}})); hl.dispatch(hl.dsp.focus({{workspace={source+1}}})); hl.plugin.hymission_tiles_test.window("focus","{centers[2*tile+1]}"); hl.dispatch(hl.dsp.focus({{workspace={source}}})); hl.plugin.hymission_tiles_test.window("focus","{centers[2*tile]}")')
    evaluate('hl.plugin.hymission.stage_open()')
    time.sleep(.2)
    evaluate('hl.config({animations={enabled=true}})')
    closing=record('closing','hl.plugin.hymission.stage_close()')
    opening=record('opening','hl.plugin.hymission.stage_open()')
    animation_pixels=[]
    for name,frames in (('opening',opening),('closing',closing)):
        for frame in frames:
            if not .02<frame['stage']['visibility_progress']<.98:continue
            image=Image.open(frame['pixels']).convert('RGB')
            for sc in frame['stage']['screens']:
                d=sc['desktop_geometry']
                for side in ('left','right'):
                    if count==1 and side=='right':continue
                    boundary=d['x'] if side=='left' else d['x']+d['width']
                    for w in frame['native']:
                        if w['workspace']!=str(sc['active_workspace']):continue
                        x,y=w['position'];width,height=w['size']
                        crossing=x<boundary-4 and x+width>boundary+32 if side=='left' else x<boundary-32 and x+width>boundary+4
                        if not crossing:continue
                        px=math.ceil(boundary)+2 if side=='left' else math.floor(boundary)-3
                        iy=round(y+height*.5)
                        inside=px+32 if side=='left' else px-32
                        if not (0<=px<image.width and 0<=inside<image.width and 0<=iy<image.height):continue
                        rgb=image.getpixel((px,iy));clear=image.getpixel((inside,iy))
                        if clear[1]<240 or clear[2]>10:continue
                        animation_pixels.append(dict(direction=name,tile=sc['tile_id'],side=side,progress=frame['stage']['visibility_progress'],edge=rgb,clear=clear,frame=frame['pixels']))
    (root/'animation-pixels.json').write_text(json.dumps(animation_pixels,indent=2))
    if count>1:
        for name in ('opening','closing'):
            for side in ('left','right'):
                rows=[r for r in animation_pixels if r['direction']==name and r['side']==side]
                check(bool(rows),f'{name}/{side}: actual overflowing desktop pixel samples')
                check(all(r['edge'][2]>5 and r['edge'][1]<245 for r in rows),f'{name}/{side}: gradient retained in native desktop middle frames')
    for tile in range(count):
        ws=901+tile*2
        evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        time.sleep(.6)
        frames=record(f'switch-{tile}',f'hl.dispatch(hl.dsp.focus({{workspace={ws+1}}}))')
        late=[]
        for frame in frames:
            sc=next(s for s in frame['stage']['screens'] if s['active_workspace']==ws+1)
            if not .85<sc['transition_progress']<1:continue
            for flight in sc['flights']:
                if flight['workspace']!=ws+1 or not flight.get('viewport_edges'):continue
                late.append(flight['viewport_edges'])
        if count>1:
            check(bool(late),f'tile {tile}: late incoming scrolling flight samples exist')
            check(all(e[0]>25 and e[1]>25 for e in late),f'tile {tile}: both incoming desktop edges retain their native band')
    verify_flight_pixels(root,count,observe)
    evaluate('hl.gesture({fingers=4,direction="vertical",action="workspace"}); hl.config({gestures={workspace_swipe_invert=false,workspace_swipe_create_new=false,workspace_swipe_use_r=false,workspace_swipe_distance=300,workspace_swipe_cancel_ratio=.05,workspace_swipe_min_speed_to_force=0,workspace_swipe_forever=false}})')
    for tile in range(count):
        source=901+tile*2
        evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={source+1}}}))')
        time.sleep(.6)
        evaluate('hl.plugin.hymission_tiles_test.trackpad("begin"); hl.plugin.hymission_tiles_test.trackpad("update",-270)')
        held=record(f'swipe-{tile}',duration=.16)
        bands=[flight['viewport_edges'] for frame in held for flight in frame['stage']['screens'][tile]['flights'] if flight['workspace']==source and flight.get('viewport_edges')]
        check(bool(bands),f'tile {tile}: real held workspace gesture has incoming flights')
        if count>1:check(all(e[0]>25 and e[1]>25 for e in bands),f'tile {tile}: held gesture retains both desktop bands')
        evaluate('hl.plugin.hymission_tiles_test.trackpad("end")')
        time.sleep(.6)
    verify_flight_pixels(root,count,observe,prefix="swipe")
    (root/'final.json').write_text(json.dumps(read('j/hymission-stage-state'),indent=2))
    print(('OBSERVED' if observe else 'PASS')+': desktop static edges, open/close and workspace flights '+case)

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root',type=Path);parser.add_argument('--tiles',type=int,required=True);parser.add_argument('--observe',action='store_true');parser.add_argument('--swipe',action='store_true')
    args=parser.parse_args();print(json.dumps(verify_flight_pixels(args.root,args.tiles,args.observe,prefix="swipe" if args.swipe else "switch")))
