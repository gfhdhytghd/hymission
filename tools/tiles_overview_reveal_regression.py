"""Private scrolling overview reveal: pixels, masks and layout-side endpoints."""
import json
import subprocess
import time
from pathlib import Path
from PIL import Image


def validate_exits(root):
    """Validate recorded closing endpoints independently of capture timing."""
    root=Path(root);samples=0;normalized_bands=[]
    for path in root.glob('*-close.json'):
        native=json.loads((root/(path.stem.removesuffix('-close')+'-native.json')).read_text())
        by={c['address']:c for c in native}
        for frame in json.loads(path.read_text()):
            for w in frame['overview'].get('windows',[]):
                client=by.get(w['address']);end=w.get('exitGeometry')
                bounds=next((t for t in frame['tiles'].get('tiles',[]) if t['id']==w.get('tile_id')),dict(x=0,width=1280))
                if not client or client['workspace']['id']%2==0 or not end:continue
                left=client['at'][0]+client['size'][0]<=bounds['x']
                right=client['at'][0]>=bounds['x']+bounds['width']
                if left or right:
                    samples+=1
                    assert abs(end['y']-client['at'][1])<2,(path,w)
                    assert (end['x']+end['width']<=bounds['x']+1 if left else end['x']>=bounds['x']+bounds['width']-1),(path,w)
                if w.get('stageTransitionSharedDecorations'):
                    for width in w['stageTransitionEdges'][:2]:
                        if width>0:
                            normalized_bands.append(width*end['width']/w['contentGeometry']['width'])
    assert samples>0,'no hidden-column closing samples'
    assert normalized_bands and all(abs(width-32)<.1 for width in normalized_bands),normalized_bands
    report=dict(hidden_column_exit_samples=samples,band_samples=len(normalized_bands),
                native_band_range=[min(normalized_bands),max(normalized_bands)],passed=True)
    (root/'exit-validation.json').write_text(json.dumps(report,indent=2))
    return report


def run(case, observe, request, root, env, runtime, processes, handles):
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
    configuration='hl.monitor({output="",mode="1280x720@60",position="auto",scale="1"}); hl.config({misc={background_color="rgb(6080b0)"},input={follow_mouse=0},general={layout="scrolling",border_size=5},scrolling={column_width=.55,fullscreen_on_one_column=false,focus_fit_method=1},animations={enabled=false},decoration={rounding=20,shadow={enabled=true,range=24,color="rgba(000000cc)"}},plugin={hymission={tiles_enabled=1,tile_min_width=100,niri_mode=0,stage_enabled=0,stage_show_active=0,stage_transition_ms=0,workspace_strip_force_show=0,overview_focus_follows_mouse=0,hover_expand_scale=1}}}); hl.plugin.hymission_tiles_test.refresh()'
    if case=='reference':configuration=configuration.replace('tiles_enabled=1,tile_min_width=100,','')
    evaluate(configuration)
    count={'reference':1,'off':1,'single':1,'two':2,'three':3}[case]
    centers=[]
    for tile in range(count):
        for ws,n in ((901+tile*2,5),(902+tile*2,1)):
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
    observations=[]
    def record(name,code,duration=.65):
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin"); '+code)
        time.sleep(duration)
        prefix=root/name
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(prefix))+')')
        frames=json.loads(prefix.with_suffix('.json').read_text())
        for frame in frames:
            p=Path(frame['pixels']);q=p.with_suffix('.png');Image.open(p).save(q);p.unlink();frame['pixels']=str(q)
        prefix.with_suffix('.json').write_text(json.dumps(frames,indent=2))
        check(len(frames)>=(4 if duration>.2 else 1),name+': framebuffer samples exist')
        return frames
    for stage in (0,1):
        evaluate('hl.config({animations={enabled=false},plugin={hymission={stage_enabled=%d}}}); hl.plugin.hymission.stage_'%stage+('open' if stage else 'close')+'()')
        time.sleep(.2)
        for address in centers:evaluate(f'hl.plugin.hymission_tiles_test.window("focus","{address}")')
        time.sleep(.2)
        evaluate('hl.config({animations={enabled=true}})')
        for scope in ('forceall','onlycurrentworkspace'):
            native=read('j/clients');(root/f'{scope}-stage{stage}-native.json').write_text(json.dumps(native,indent=2))
            opened=record(f'{scope}-stage{stage}-open',f'hl.plugin.hymission.open("{scope}")')
            wait('j/hymission-overview-state',lambda s:s['phase']=='active')
            closed=record(f'{scope}-stage{stage}-close','hl.plugin.hymission.close()')
            wait('j/hymission-overview-state',lambda s:not s['active'])
            dark=0;tested=0;horizontal=[];bands=[]
            for frame in opened:
                if frame['overview']['phase']!='opening':continue
                image=Image.open(frame['pixels']).convert('RGB')
                # Solid private background is the most common non-client color.
                colors=image.getcolors(image.width*image.height)
                bg=max(((n,c) for n,c in colors if c[2]>c[0] and c[2]>c[1]),default=(0,(96,128,176)))[1]
                for w in frame['overview']['windows']:
                    client=next((c for c in native if c['address']==w['address']),None)
                    if not client or client['workspace']['id']%2==0:continue
                    b=w['contentGeometry'];clip=w.get('stageTransitionClip')
                    start=w.get('animationFromGeometry',{})
                    if clip:
                        edges=w['stageTransitionEdges'];bands.append(dict(address=w['address'],window=b,clip=clip,edges=edges))
                        # Scan the unrevealed rounded left side, including its
                        # shadow support. Content/other clients are green/red;
                        # an early shadow is a dark background-colored frame.
                        for x in range(max(1,int(b['x']-16)),min(image.width-1,int(b['x']+8),int(clip['x']-2))):
                            for y in range(max(50,int(b['y']+b['height']*.2)),min(image.height-20,int(b['y']+b['height']*.8)),4):
                                covered=False
                                for other in frame['overview']['windows']:
                                    if other['address']==w['address']:continue
                                    ob=other['contentGeometry'];oc=other.get('stageTransitionClip')
                                    owner=next((c for c in native if c['address']==other['address']),None)
                                    if oc and owner and owner['workspace']['id']%2 and not (oc['x']<=x<oc['x']+oc['width'] and oc['y']<=y<oc['y']+oc['height']):continue
                                    if ob['x']-30<=x<ob['x']+ob['width']+30 and ob['y']-40<=y<ob['y']+ob['height']+30:covered=True;break
                                if covered:continue
                                r,g,blue=image.getpixel((x,y));tested+=1
                                if blue<bg[2]*.85 and r<bg[0]*.9 and g<bg[1]*.9 and blue>g and blue>r:dark+=1
                    if start and 'exitGeometry' in w:
                        bounds=next((t for t in frame['tiles'].get('tiles',[]) if t['id']==w.get('tile_id')),dict(x=0,width=1280))
                        actual=client['at'];size=client['size']
                        if actual[0]+size[0]<=bounds['x'] or actual[0]>=bounds['x']+bounds['width']:
                            side=-1 if actual[0]+size[0]<=bounds['x'] else 1
                            horizontal.append(dict(address=w['address'],side=side,start=start,native_y=actual[1],bounds=bounds))
            observations.append(dict(stage=stage,scope=scope,unrevealed_shadow_pixels=dark,tested_pixels=tested,hidden_columns=horizontal,bands=bands,open_frames=len(opened),close_frames=len(closed)))
            # Single-frame darkness is diagnostic only: strip panels and other
            # clients may cover the same pixels. Paired held-phase captures below
            # isolate actual shadow pixels without masking background panels.
            if case!='reference':
                check(bool(horizontal),f'{scope}/Stage{stage}: fully hidden scrolling columns exercised')
                check(all(abs(h['start']['y']-h['native_y'])<2 and (h['start']['x']+h['start']['width']<=h['bounds']['x']+1 if h['side']<0 else h['start']['x']>=h['bounds']['x']+h['bounds']['width']-1) for h in horizontal),f'{scope}/Stage{stage}: hidden columns start on their layout side and y')
            (root/'observations.json').write_text(json.dumps(observations,indent=2))
            for held_delta in (12,60):
                evaluate('hl.plugin.hymission.gesture({fingers=4,direction="vertical",action="open",args="'+scope+'"})')
                evaluate('hl.plugin.hymission_tiles_test.trackpad("begin"); hl.plugin.hymission_tiles_test.trackpad("update",'+str(held_delta)+')')
                held=read('j/hymission-overview-state')
                if not held['active']:
                    evaluate('hl.plugin.hymission_tiles_test.trackpad("cancel"); hl.plugin.hymission_tiles_test.trackpad("begin"); hl.plugin.hymission_tiles_test.trackpad("update",'+str(-held_delta)+')')
                wait('j/hymission-overview-state',lambda s:s['active'])
                on=record(f'{scope}-stage{stage}-held{held_delta}-shadow-on','',.12)[-1]
                evaluate('hl.config({decoration={shadow={enabled=false}}})')
                off=record(f'{scope}-stage{stage}-held{held_delta}-shadow-off','',.12)[-1]
                evaluate('hl.config({decoration={shadow={enabled=true}}})')
                evaluate('hl.plugin.hymission_tiles_test.trackpad("cancel")')
                wait('j/hymission-overview-state',lambda s:not s['active'])
                a=Image.open(on['pixels']).convert('RGB');bim=Image.open(off['pixels']).convert('RGB')
                difference=0;sampled=0
                windows=on['overview']['windows'];other={w['address']:w for w in off['overview']['windows']}
                stable=all(w['address'] in other and max(abs(w['contentGeometry'][k]-other[w['address']]['contentGeometry'][k]) for k in ('x','y','width','height'))<.1 for w in windows)
                check(stable,f'{scope}/Stage{stage}: paired shadow control has identical held geometry')
                for w in windows:
                    client=next((c for c in native if c['address']==w['address']),None)
                    if not client or client['workspace']['id']%2==0:continue
                    box=w['contentGeometry'];clip=w.get('stageTransitionClip')
                    if not clip:continue
                    for x in range(max(1,int(box['x']-20)),min(a.width-1,int(box['x']+4),int(clip['x']-2))):
                        for y in range(max(40,int(box['y']+box['height']*.2)),min(a.height-20,int(box['y']+box['height']*.8)),2):
                            covered=False
                            for v in windows:
                                if v['address']==w['address']:continue
                                vb=v['contentGeometry'];vc=v.get('stageTransitionClip');owner=next((c for c in native if c['address']==v['address']),None)
                                if vc and owner and owner['workspace']['id']%2 and not (vc['x']<=x<vc['x']+vc['width'] and vc['y']<=y<vc['y']+vc['height']):continue
                                if vb['x']-30<=x<vb['x']+vb['width']+30 and vb['y']-40<=y<vb['y']+vb['height']+30:covered=True;break
                            if covered:continue
                            sampled+=1
                            if max(abs(u-v) for u,v in zip(a.getpixel((x,y)),bim.getpixel((x,y))))>3:difference+=1
                observations[-1].setdefault('paired_phases',[]).append(dict(delta=held_delta,shadow_pixels_outside_mask=difference,tested_pixels=sampled))
                check(difference==0,f'{scope}/Stage{stage}: shadow is absent beyond the moving band')
                (root/'observations.json').write_text(json.dumps(observations,indent=2))
    if not observe:validate_exits(root)
    print(('OBSERVED' if observe else 'PASS')+': scrolling overview moving edge band '+case)


if __name__=='__main__':
    import sys
    for directory in sys.argv[1:]:print(directory,json.dumps(validate_exits(directory)))
