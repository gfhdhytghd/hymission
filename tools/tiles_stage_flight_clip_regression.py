"""FB12: native frame pixels must cover each moving Stage window across foreign tiles.

The independent oracle samples solid client interiors, subtracting only windows
above them in the native Stage flight order. Endpoints and all flight geometry
are retained alongside every captured frame, including held four-finger motion.
"""
import json
import math
from pathlib import Path
import subprocess
import time
from PIL import Image, ImageChops, ImageDraw

MARKER_COLORS = ('00ff00', '0000ff', 'ffff00', 'ff00ff', '00ffff', 'ff8000')


def verify_existing(root, observe=False):
    """Audit coverage and restored endpoint clips from the retained raw frames."""
    checks = []
    for result in json.loads((root/'results.json').read_text()):
        tile,route,start,end = (result[k] for k in ('tile','route','start','end'))
        name = f'tile{tile}-{route}-{start}-{end}'
        frames = json.loads((root/(name+'.json')).read_text())
        samples = json.loads((root/(name+'-pixels.json')).read_text())
        multiple = len(frames[-1]['stage']['screens']) > 1
        crossing = {role:sum(s['foreign_pixels']>=30 for s in samples if s['workspace']==ws)
                    for role,ws in (('entering_right',end),('leaving_left',start))}
        held = sum(f['stage']['screens'][tile]['swipe_active'] and
                   .06<f['stage']['screens'][tile]['transition_progress']<.94 for f in frames)
        image = Image.open(frames[-1]['pixels']).convert('RGB')
        foreign = Image.new('L',image.size)
        draw = ImageDraw.Draw(foreign)
        for i,screen in enumerate(frames[-1]['stage']['screens']):
            if i==tile:continue
            for b in (screen['desktop_geometry'],screen['sidebar']):
                draw.rectangle((math.ceil(b['x']+6),math.ceil(b['y']+6),
                                math.floor(b['x']+b['width']-6),math.floor(b['y']+b['height']-6)),fill=255)
        own_colors = Image.new('L',image.size)
        for ws in (start,end):
            color = tuple(bytes.fromhex(MARKER_COLORS[2*(ws//100-1)+ws%100-1]))
            mask = Image.new('L',image.size,255)
            for channel,expected in zip(image.split(),color):
                mask = ImageChops.multiply(mask,channel.point(lambda v,e=expected:255 if abs(v-e)<45 else 0))
            own_colors = ImageChops.lighter(own_colors,mask)
        leaked = sum(ImageChops.multiply(foreign,own_colors).histogram()[1:])
        settled = not any(s['transition_windows'] or s['swipe_active'] for s in frames[-1]['stage']['screens'])
        row = dict(name=name,frames=len(frames),crossing=crossing,held_frames=held,settled=settled,endpoint_foreign_pixels=leaked)
        checks.append(row)
        assert settled and (route!='swipe' or held>=2),row
        assert not multiple or all(crossing.values()),row
        if not observe:
            assert result['worst_missing_fraction']<.025 and leaked<=8,row
    (root/'coverage.json').write_text(json.dumps(checks,indent=2))
    print(('OBSERVED' if observe else 'PASS')+': incoming/outgoing foreign-region coverage, held swipe and restored endpoint clips')


def run(case, observe, route_filter, layout, request, root, env, runtime, processes, handles):
    def evaluate(code):
        value = request('/eval '+code)
        assert value.strip() == 'ok', (code, value)
    def read(cmd): return json.loads(request(cmd))
    def wait(cmd, predicate):
        for _ in range(400):
            state = read(cmd)
            if predicate(state): return state
            time.sleep(.015)
        raise AssertionError((cmd, state))
    def focus(ws): evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
    def stage(): return read('j/hymission-stage-state')
    results = []
    colors = MARKER_COLORS
    count = {'off':1, 'single':1, 'two':2, 'three':3}[case]
    pairs = [(101+100*i, 102+100*i) for i in range(count)]
    color_by_ws = {ws:tuple(bytes.fromhex(colors[2*i+j])) for i,pair in enumerate(pairs) for j,ws in enumerate(pair)}
    child = env.copy(); child.pop('LD_PRELOAD', None)
    child['WAYLAND_DISPLAY'] = next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml = Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags = subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary = root/'marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    evaluate('hl.monitor({output="",mode="1280x720@60",position="auto",scale="1"}); hl.config({general={layout="'+layout+'"},scrolling={column_width=.85,fullscreen_on_one_column=false,focus_fit_method=1},animations={enabled=false},input={follow_mouse=0},decoration={shadow={enabled=false}},plugin={hymission={stage_enabled=0,niri_mode=0,stage_show_active=0,stage_empty_slots=0,stage_transition_ms=650,stage_window_rounding=0,stage_scrolling_preview_edge_width=0,stage_scrolling_desktop_edge_width=0}}})')
    total = 0
    for i,pair in enumerate(pairs):
        for j,ws in enumerate(pair):
            evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}})'); focus(ws)
            log = open(root/f'client-{ws}.log','w'); handles.append(log)
            client_env = child.copy(); client_env['HMT_MARKER_COLOR'] = colors[2*i+j]
            processes.append(subprocess.Popen([str(binary)],env=client_env,stdout=log,stderr=subprocess.STDOUT))
            total += 1
            wait('j/clients', lambda cs:len(cs)==total)
    focus(101)
    if case != 'off':
        evaluate('hl.config({plugin={hymission={tiles_enabled=1,tile_min_width=120}}}); hl.plugin.hymission_tiles_test.refresh()')
        wait('j/hymission-tiles-state',lambda s:s['enabled'])
        for source,target in pairs[1:]:
            evaluate(f'hl.plugin.hymission.tile_add({{workspace="{source}"}})'); focus(target); focus(source)
        focus(101)
    evaluate('hl.config({animations={enabled=true},plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission.stage_open(); hl.gesture({fingers=4,direction="vertical",action="workspace"}); hl.config({gestures={workspace_swipe_invert=false,workspace_swipe_create_new=false,workspace_swipe_use_r=false,workspace_swipe_distance=300,workspace_swipe_cancel_ratio=.05,workspace_swipe_min_speed_to_force=0,workspace_swipe_forever=false}})')
    wait('j/hymission-stage-state',lambda s:len(s['screens'])==count and s['visibility_progress']==1)
    time.sleep(.7)

    def rect_mask(size, box, inset=6):
        x,y,w,h = (box[k] for k in ('x','y','width','height'))
        mask = Image.new('L',size)
        left,top,right,bottom = math.ceil(x+inset),math.ceil(y+inset),math.floor(x+w-inset),math.floor(y+h-inset)
        if right>left and bottom>top: ImageDraw.Draw(mask).rectangle((left,top,right-1,bottom-1),fill=255)
        return mask
    def color_mask(image, color):
        result = Image.new('L',image.size,255)
        for channel,expected in zip(image.split(),color):
            result = ImageChops.multiply(result,channel.point(lambda v,e=expected:255 if abs(v-e)<45 else 0))
        return result
    def pixels(mask): return sum(mask.histogram()[1:])
    def analyze(frames, own):
        samples=[]
        for index,frame in enumerate(frames):
            sc=frame['stage']['screens'][own]
            if not .06<sc['transition_progress']<.94: continue
            flights=sc['flights']
            image=Image.open(frame['pixels']).convert('RGB')
            for rank,flight in enumerate(flights):
                box=flight['frame'];ws=flight['workspace']
                if ws not in color_by_ws or min(box['width'],box['height'])<25:continue
                mask=rect_mask(image.size,box)
                # Scrolling Stage retains its native moving tape aperture.
                # This is independent of all stationary tile/column scissors.
                if flight.get('viewport'):
                    mask=ImageChops.multiply(mask,rect_mask(image.size,flight['viewport'],2))
                native=next(n for n in frame['native'] if n['workspace']==str(ws))
                # This marker has a 32 px red CSD header. Sample its solid body,
                # excluding the scaled header instead of calling it a hole.
                header=math.ceil(32*box['height']/max(1,native['size'][1]))+8
                ImageDraw.Draw(mask).rectangle((0,0,image.width,math.ceil(box['y']+header)),fill=0)
                # Native Stage paints the incoming workspace above the outgoing
                # one. Include the front client's CSD header in its occlusion.
                for front in flights[rank+1:]:
                    b=front['frame'];draw=ImageDraw.Draw(mask)
                    draw.rectangle((math.floor(b['x']-4),math.floor(b['y']-40),math.ceil(b['x']+b['width']+4),math.ceil(b['y']+b['height']+4)),fill=0)
                tested=pixels(mask)
                if tested<30:continue
                visible=ImageChops.multiply(mask,color_mask(image,color_by_ws[ws]))
                foreign=Image.new('L',image.size)
                for peer_index,peer in enumerate(frame['stage']['screens']):
                    if peer_index==own:continue
                    for region in (peer['desktop_geometry'],peer['sidebar']):
                        foreign=ImageChops.lighter(foreign,rect_mask(image.size,region,0))
                witness=ImageChops.multiply(mask,foreign);crossing=pixels(witness)
                samples.append(dict(frame=index,workspace=ws,phase=sc['transition_progress'],tested_pixels=tested,missing_pixels=tested-pixels(visible),foreign_pixels=crossing,missing_foreign_pixels=crossing-pixels(ImageChops.multiply(visible,foreign))))
        return samples

    for tile,(source,target) in enumerate(pairs):
        for route in ((route_filter,) if route_filter else ('key','swipe')):
            for start,end in ((source,target),(target,source)):
                if count>1:focus(read('j/hymission-tiles-state')['tiles'][tile]['activeWorkspace']['id'])
                focus(start);time.sleep(.75)
                before=stage();name=f'tile{tile}-{route}-{start}-{end}'
                (root/(name+'-before.json')).write_text(json.dumps(before,indent=2))
                evaluate('hl.plugin.hymission_tiles_test.pointer("move",1270,710); hl.plugin.hymission_tiles_test.frame_capture("begin")')
                time.sleep(.04)
                if route=='key':
                    evaluate(f'if fb12bind then fb12bind:remove() end; fb12bind=hl.bind("F8",hl.dsp.focus({{workspace="{end}"}})); hl.plugin.hymission_tiles_test.key(66)')
                else:
                    sign=1 if end>start else -1
                    evaluate('hl.plugin.hymission_tiles_test.trackpad("begin")')
                    for n in range(4):
                        evaluate(f'hl.plugin.hymission_tiles_test.trackpad("update",{sign*45})');time.sleep(.065)
                        (root/f'{name}-held-{n}.json').write_text(json.dumps(stage(),indent=2))
                    evaluate('hl.plugin.hymission_tiles_test.trackpad("end")')
                time.sleep(.85)
                prefix=str(root/name)
                evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(prefix)+')')
                frames=json.loads(Path(prefix+'.json').read_text())
                for frame in frames:
                    path=Path(frame['pixels']);target_path=path.with_suffix('.png')
                    with Image.open(path) as im:im.save(target_path)
                    path.unlink();frame['pixels']=str(target_path)
                Path(prefix+'.json').write_text(json.dumps(frames,indent=2))
                samples=analyze(frames,tile)
                (root/(name+'-pixels.json')).write_text(json.dumps(samples,indent=2))
                changed=[]
                for frame in frames:
                    for i,(a,b) in enumerate(zip(before['screens'],frame['stage']['screens'])):
                        if i!=tile and any(a[k]!=b[k] for k in ('active_workspace','desktop_geometry','sidebar','cards')):changed.append(i)
                worst=max((s['missing_pixels']/s['tested_pixels'] for s in samples),default=1)
                foreign=[s for s in samples if s['foreign_pixels']>=30]
                result=dict(tile=tile,route=route,start=start,end=end,frames=len(frames),sampled_windows=len(samples),crossing_samples=len(foreign),worst_missing_fraction=worst,missing_foreign_pixels=max((s['missing_foreign_pixels'] for s in foreign),default=0),neighbor_geometry_changes=changed,committed=read('j/activeworkspace')['id']==end)
                results.append(result);(root/'results.json').write_text(json.dumps(results,indent=2))
                assert samples and result['committed'],result
                assert not changed,result
                if not observe:assert worst<.025,result
    verify_existing(root,observe)
    print(('OBSERVED' if observe else 'PASS')+': unclipped Stage flights '+json.dumps(results))


if __name__ == '__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-existing',type=Path,required=True)
    parser.add_argument('--observe',action='store_true')
    args=parser.parse_args()
    verify_existing(args.verify_existing,args.observe)
