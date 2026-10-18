"""Stage workspace motion: private real clients, keyboard/swipe input and every-rendered-frame readback."""
import json
import subprocess
import time
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw


def run(case,observe,route_filter,layout,empty_source,request,root,env,runtime,processes,handles):
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
    def record(name,action,swipe=False,flight_owner=True):
        before=stage();(root/(name+'-before.json')).write_text(json.dumps(before,indent=2))
        prefix=str(root/name)
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin")')
        time.sleep(.06)
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
        Path(prefix+'.json').write_text(json.dumps(frames,indent=2))
        (root/(name+'-frames.json')).write_text(json.dumps(frames,indent=2))
        counts=[max((s['transition_windows'] for s in item['stage']['screens']),default=0) for item in samples]
        if flight_owner:
            check(max(counts,default=0)>0,name+': Stage integrated flight exists')
            check(any(0<s['transition_progress']<1 for item in samples for s in item['stage']['screens'] if s['transition_windows']),name+': Stage intermediate flight progress')
        else:
            check(not any(counts),name+': overview owns visible motion without a second Stage animation')
            check(any(f['overview'].get('active') for f in frames),name+': overview frame samples exist')
        return before,samples,frames
    evaluate('hl.monitor({output="",mode="1280x720@60",position="auto",scale="1"}); hl.config({general={layout="dwindle"},animations={enabled=false},input={follow_mouse=0},plugin={hymission={stage_enabled=0,niri_mode=0,stage_show_active=0,stage_empty_slots=0,stage_transition_ms=450}}})')
    if layout=="scrolling":evaluate('hl.config({general={layout="scrolling"},scrolling={column_width=.85,fullscreen_on_one_column=true,focus_fit_method=1}})')
    if empty_source:evaluate('hl.config({plugin={hymission={stage_show_active=1}}})')
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
        if empty_source and ws==101:continue
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}})');focus(ws)
        log=open(root/f'client-{ws}.log','w');handles.append(log)
        client_env=child.copy();client_env["HMT_MARKER_COLOR"]="00ff00" if ws%100==1 else "0000ff"
        processes.append(subprocess.Popen([str(binary)],env=client_env,stdout=log,stderr=subprocess.STDOUT));total+=1
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
    if route_filter in (None,'swipe'):
        evaluate('hl.gesture({fingers=4,direction="vertical",action="workspace"}); hl.config({gestures={workspace_swipe_invert=false,workspace_swipe_create_new=false,workspace_swipe_use_r=false,workspace_swipe_distance=300,workspace_swipe_cancel_ratio=.05,workspace_swipe_min_speed_to_force=0,workspace_swipe_forever=false}})')
    for tile,(source,target) in enumerate(pairs[:count]):
      for route in (('unified',) if route_filter=='unified' else ('key','relative','card','swipe')):
        if route_filter and route!=route_filter:continue
        if has_tiles and count>1:
            active=read('j/hymission-tiles-state')['tiles'][tile]['activeWorkspace']['id'];focus(active)
        focus(source);time.sleep(.7)
        def action():
            if route in ('key','relative'):
                selector=str(target) if route=='key' else 'm+1'
                evaluate(f'if fb10bind then fb10bind:remove() end; fb10bind=hl.bind("F8",hl.dsp.focus({{workspace="{selector}"}})); hl.plugin.hymission_tiles_test.key(66)')
            elif route=='card':
                s=stage();sc,c=next((sc,c) for sc in s['screens'] for c in sc['cards'] if c['workspace']==target)
                if 'x' in c:
                    x=c['x']+sc['card_width']/2;y=c['y']+sc['card_height']/2
                else:
                    addresses={w['address'] for w in read('j/clients') if w['workspace']['id']==target}
                    preview=next(w for w in s['captureWindows'] if w['address'] in addresses)['selectionGeometry']
                    x=preview['x']+preview['width']/2;y=preview['y']+preview['height']/2
                pointer('press',x,y);pointer('release',x,y)
            elif route=='unified':
                evaluate('hl.plugin.hymission_tiles_test.swipe("begin")')
                for n in range(1,5):
                    evaluate(f'hl.plugin.hymission_tiles_test.swipe("update",{n*45})');time.sleep(.04)
                evaluate('hl.plugin.hymission_tiles_test.swipe("end")')
            elif route=='swipe':
                evaluate('hl.plugin.hymission_tiles_test.trackpad("begin")')
                for n in range(1,5):
                    evaluate('hl.plugin.hymission_tiles_test.trackpad("update",45)')
                    (root/f'swipe-held-{tile}-{n}.json').write_text(json.dumps(stage(),indent=2));time.sleep(.04)
                evaluate('hl.plugin.hymission_tiles_test.trackpad("end")')
        name=route if count==1 else f'tile{tile}-{route}'
        cursor_before=read('j/cursorpos')
        before,samples,frames=record(name,action,route=='swipe')
        cursor_after=read('j/cursorpos')
        check(read('j/activeworkspace')['id']==target,name+': commits requested workspace')
        if count>1:
            own=before['screens'][tile]['tile_id']
            def stable(sc):return {k:sc[k] for k in ('active_workspace','desktop_geometry','sidebar','card_width','card_height','cards')}
            neighbors={sc['tile_id']:stable(sc) for sc in before['screens'] if sc['tile_id']!=own}
            check(all(stable(sc)==neighbors[sc['tile_id']] for f in frames for sc in f['stage']['screens'] if sc['tile_id'] in neighbors),name+': neighboring columns and desktops keep geometry')
            check(all(sc['transition_windows']==0 and not sc['swipe_active'] for f in frames for sc in f['stage']['screens'] if sc['tile_id'] in neighbors),name+': neighboring columns do not acquire animation')
            changes=[]
            first=Image.open(frames[0]['pixels']).convert('RGB')
            for frame in frames[1:]:
                diff=ImageChops.difference(first,Image.open(frame['pixels']).convert('RGB'))
                draw=ImageDraw.Draw(diff)
                for point in (cursor_before,cursor_after):draw.rectangle((point['x']-30,point['y']-30,point['x']+30,point['y']+30),fill=(0,0,0))
                for sc in before['screens']:
                    if sc['tile_id'] not in neighbors:continue
                    for region in (sc['desktop_geometry'],sc['sidebar']):
                        x,y=int(region['x'])+3,int(region['y'])+3;w,h=int(region['width'])-6,int(region['height'])-6
                        if w<=0 or h<=0:continue
                        histogram=diff.crop((x,y,x+w,y+h)).convert('L').histogram()
                        changes.append(sum(histogram[9:])/(w*h))
            (root/(name+'-neighbor-pixels.json')).write_text(json.dumps(changes))
            check(max(changes,default=0)<.002,name+': neighboring desktop/sidebar pixels stay still')
        moving=[f for f in frames if any(sc['transition_windows'] for sc in f['stage']['screens'])]
        check(all(abs(v)<.01 for f in moving for w in f['native'] if w['visible'] for v in w['offset']) and
              all(not w['workspace_animated'] and not w['workspace_alpha_animated'] for f in moving for w in f['native'] if w['visible'] or w['workspace'] in (str(source),str(target))),name+': no native slide/fade underneath Stage')
    if route_filter in (None,'swipe'):
      for tile,(source,target) in enumerate(pairs[:count]):
        if has_tiles and count>1:focus(read('j/hymission-tiles-state')['tiles'][tile]['activeWorkspace']['id'])
        focus(source);time.sleep(.55)
        before_bindings=read('j/hymission-tiles-state')['tiles'] if has_tiles else []
        def cancel():
            evaluate('hl.plugin.hymission_tiles_test.trackpad("begin")')
            for _ in range(3):evaluate('hl.plugin.hymission_tiles_test.trackpad("update",45)');time.sleep(.025)
            evaluate('hl.plugin.hymission_tiles_test.trackpad("cancel")')
        record(f'tile{tile}-swipe-cancel',cancel,swipe=True)
        check(read('j/activeworkspace')['id']==source,f'tile{tile}-swipe-cancel: preserves workspace')
        if has_tiles:check(read('j/hymission-tiles-state')['interaction_owner']==0,f'tile{tile}-swipe-cancel: releases single owner')
        if before_bindings:
            check([t['activeWorkspace'] for t in read('j/hymission-tiles-state')['tiles']]==[t['activeWorkspace'] for t in before_bindings],f'tile{tile}-swipe-cancel: preserves all bindings')
    if has_tiles and count>1 and route_filter is None:
      for tile,(source,target) in enumerate(pairs[:count]):
        focus(read('j/hymission-tiles-state')['tiles'][tile]['activeWorkspace']['id']);focus(target);time.sleep(.55)
        expected=target+1
        evaluate('hl.config({gestures={workspace_swipe_create_new=true}})')
        before=read('j/hymission-tiles-state')['tiles']
        def create():
            evaluate('hl.plugin.hymission_tiles_test.trackpad("begin")')
            for _ in range(4):evaluate('hl.plugin.hymission_tiles_test.trackpad("update",50)');time.sleep(.025)
            evaluate('hl.plugin.hymission_tiles_test.trackpad("end")')
        record(f'tile{tile}-swipe-create',create,swipe=True)
        after=read('j/hymission-tiles-state')['tiles']
        check(after[tile]['activeWorkspace']['id']==expected,f'tile{tile}-swipe-create: new workspace belongs to source tile')
        check(all(t['activeWorkspace']==before[i]['activeWorkspace'] for i,t in enumerate(after) if i!=tile),f'tile{tile}-swipe-create: preserves foreign bindings')
        evaluate('hl.config({gestures={workspace_swipe_create_new=false}})')
    if has_tiles and route_filter in (None,'strip'):
      for tile,(source,target) in enumerate(pairs[:count]):
        if has_tiles and count>1:focus(read('j/hymission-tiles-state')['tiles'][tile]['activeWorkspace']['id'])
        focus(source);time.sleep(.55)
        evaluate('hl.config({plugin={hymission={workspace_strip_force_show=1,workspace_strip_empty_mode="always"}}}); hl.plugin.hymission.open("onlycurrentworkspace")')
        ov=wait('j/hymission-overview-state',lambda s:s['phase']=='active' and not s.get('relayout_active'))
        entry=next(e for e in ov['strip'] if e['workspace']==target)
        rect=entry['geometry'];x=rect['x']+rect['width']/2;y=rect['y']+rect['height']/2
        original=read('j/hymission-tiles-state')['tiles'] if has_tiles else []
        def click():pointer('press',x,y);pointer('release',x,y)
        record(f'tile{tile}-strip',click,flight_owner=False)
        wait('j/activeworkspace',lambda s:s['id']==target)
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin"); hl.plugin.hymission.close()')
        wait('j/hymission-overview-state',lambda s:s['phase']=='inactive')
        time.sleep(.1)
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(root/f'tile{tile}-strip-handoff'))+')')
        sc=next(sc for sc in stage()['screens'] if sc['active_workspace']==target)
        check(sc['overview_render_owner']=='stage' and not sc['suspended'],f'tile{tile}-strip: exits onto requested Stage desktop')
        if original:
            after=read('j/hymission-tiles-state')['tiles']
            check(all(a['activeWorkspace']['id']==b['activeWorkspace']['id'] for i,(a,b) in enumerate(zip(original,after)) if i!=tile),f'tile{tile}-strip: foreign bindings unchanged')
    print(('OBSERVE' if observe else 'PASS')+f': Stage {case}, {len(results)} checks, {sum(not x["passed"] for x in results)} failed')


def compare(reference, candidates, output):
    """Compare rendered blue-client geometry at each recorded Stage phase, not wall-clock frame indices."""
    def blue_box(path):
        im=Image.open(path).convert('RGB');r,g,b=im.split()
        mask=ImageChops.multiply(b.point(lambda v:255 if v>180 else 0),r.point(lambda v:255 if v<60 else 0))
        mask=ImageChops.multiply(mask,g.point(lambda v:255 if v<60 else 0))
        box=mask.getbbox()
        assert box,('missing blue client',path)
        return (box[0],box[1],box[2]-box[0],box[3]-box[1])
    reports={}
    for directory in [reference,*candidates]:
        routes={}
        for route in ('key','relative','card','swipe'):
            frames=json.loads((directory/(route+'-frames.json')).read_text())
            first,last=blue_box(frames[0]['pixels']),blue_box(frames[-1]['pixels'])
            samples=[]
            for frame in frames:
                sc=frame['stage']['screens'][0]
                if not sc['transition_windows']:continue
                if route=='swipe' and any(w['workspace']=='102' and w['visible'] for w in frame['native']):continue
                p=sc['transition_progress']
                if not .04<p<.92:continue
                box=blue_box(frame['pixels'])
                q=(box[2]-first[2])/(last[2]-first[2])
                expected=[a+(b-a)*q for a,b in zip(first,last)]
                samples.append(dict(phase=p,q=q,phase_error=abs(q-(1-(1-p)**3)),pixel_error=max(abs(a-b) for a,b in zip(box,expected))))
            assert len(samples)>=3,(directory,route,'insufficient rendered middle frames',samples)
            report=dict(first=first,last=last,samples=len(samples),max_phase_error=max(x['phase_error'] for x in samples),max_pixel_error=max(x['pixel_error'] for x in samples))
            routes[route]=report
            assert report['max_phase_error']<.04,(directory,route,report)
            assert report['max_pixel_error']<4,(directory,route,report)
        reports[str(directory)]=routes
    original=reports[str(reference)]
    for directory in candidates:
        for route in original:
            for endpoint in ('first','last'):
                assert max(abs(a-b) for a,b in zip(original[route][endpoint],reports[str(directory)][route][endpoint]))<=2,(directory,route,endpoint,original[route],reports[str(directory)][route])
    output.write_text(json.dumps(reports,indent=2))
    print('PASS: pretiles/off/single rendered endpoints and per-frame Stage cubic geometry; four-finger held phases included')


if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reference',type=Path,required=True)
    ap.add_argument('--candidate',type=Path,action='append',required=True)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();compare(args.reference,args.candidate,args.output)
