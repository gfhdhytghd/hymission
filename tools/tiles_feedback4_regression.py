"""Private, every-rendered-frame evidence for tile overview transitions."""
import json
from pathlib import Path
import subprocess
import time
import itertools
from PIL import Image


def run(case,observe,request,root,env,runtime,processes,handles):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def read(command):return json.loads(request(command))
    def wait(command,predicate):
        for _ in range(400):
            state=read(command)
            if predicate(state):return state
            time.sleep(.01)
        raise AssertionError((command,state))
    def overview():return read('j/hymission-overview-state')
    def record(name,code,duration=.55):
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin"); '+code)
        time.sleep(duration)
        prefix=root/name
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(prefix))+')')
        frames=json.loads(prefix.with_suffix('.json').read_text())
        assert len(frames)>=5,('insufficient rendered frames',name,len(frames))
        presented=None
        for i,frame in enumerate(frames):
            image=Image.open(frame['pixels']).convert('RGB');assert image.getextrema()[2][1]>20,('invalid black readback',name,i)
            image.save(root/f'{name}-{i:02}-workbuffer.png')
            # Outside native damage the workbuffer may be undefined/discarded.
            # Compose only damaged rectangles, as the actual output does.
            if presented is None:presented=image.copy()
            else:
                for rect in frame['framebuffer']['damage']:presented.paste(image.crop(tuple(rect)),tuple(rect[:2]))
            presented.save(root/f'{name}-{i:02}.png')
            Path(frame['pixels']).unlink();frame['pixels']=str(root/f'{name}-{i:02}.png')
        prefix.with_suffix('.json').write_text(json.dumps(frames,indent=2))
        subprocess.run(['grim','-o',read('j/monitors')[0]['name'],str(root/(name+'-screencopy.png'))],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        return frames
    def shadow_runs(frames):
        runs=[]
        for i,frame in enumerate(frames):
            if frame['overview']['phase']!='opening':continue
            im=Image.open(frame['pixels']).convert('RGB');best=0
            for window in frame['overview']['windows']:
                b=window['contentGeometry'];y=int(b['y']+b['height']*.5)
                left=max(320,int(b['x']+15));right=min(im.width-10,int(b['x']+b['width']-15))
                if not (90<y<im.height-30 and left<right):continue
                pixels=[im.getpixel((x,y)) for x in range(left,right)]
                dark=[25<p[2]<140 and p[0]<100 and p[1]<150 for p in pixels]
                best=max(best,max([len(list(v)) for key,v in itertools.groupby(dark) if key]+[0]))
            runs.append(best)
        assert len(runs)>=3,('missing intermediate opening frames',runs)
        return max(runs)
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    evaluate('hl.config({misc={background_color="rgb(6080b0)"},general={layout="scrolling"},scrolling={column_width=.85,fullscreen_on_one_column=true,focus_fit_method=1},animations={enabled=false},decoration={shadow={enabled=true,range=20,color="rgba(00000099)"}},plugin={hymission={tiles_enabled=1,tile_min_width=120,niri_mode=0,stage_enabled=0,stage_show_active=0,workspace_strip_force_show=1,workspace_strip_empty_mode="always"}}}); hl.plugin.hymission_tiles_test.refresh()')
    for ws in (901,902,903,904):
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        for _ in range(3):
            count=len(read('j/clients'));log=open(root/f'client-{ws}-{count}.log','w');handles.append(log)
            processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
            wait('j/clients',lambda cs:len(cs)==count+1)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=901})); hl.plugin.hymission.tile_add({workspace="903"}); hl.dispatch(hl.dsp.focus({workspace=904})); hl.dispatch(hl.dsp.focus({workspace=903})); hl.plugin.hymission.tile_focus("left")')
    result=[]
    for stage in (0,1):
        evaluate(f'hl.config({{animations={{enabled=false}},plugin={{hymission={{stage_enabled={stage},stage_transition_ms=0}}}}}}); hl.plugin.hymission.stage_'+('open' if stage else 'close')+'()')
        time.sleep(.2)
        evaluate('hl.config({animations={enabled=true}})')
        for scope in ('onlycurrentworkspace','forceall'):
            opened=record(f'{case}-{scope}-stage{stage}-open',f'hl.plugin.hymission.open("{scope}")')
            wait('j/hymission-overview-state',lambda s:s['phase']=='active')
            if case=='switch' and scope=='onlycurrentworkspace':
                before=overview();neighbors={w['address']:w['contentGeometry'] for w in before['windows'] if w['tile_id']==2}
                assert neighbors
                # Native workspace dispatch while overview is active, rather than a
                # fabricated visual offset; exercises the real transition owner.
                frames=record(f'switch-stage{stage}', 'hl.dispatch(hl.dsp.focus({workspace=902}))',.7)
                moved=[];missing=[]
                for i,f in enumerate(frames):
                    by={w['address']:w['contentGeometry'] for w in f['overview'].get('windows',[])}
                    for address,box in neighbors.items():
                        if address not in by:missing.append(i);continue
                        if max(abs(by[address][k]-v) for k,v in box.items())>1:moved.append(i)
                result.append(dict(stage=stage,scope=scope,moved_frames=sorted(set(moved)),missing_frames=sorted(set(missing))))
            closed=record(f'{case}-{scope}-stage{stage}-close','hl.plugin.hymission.close()')
            wait('j/hymission-overview-state',lambda s:not s['active'])
            if case!='switch':
                missing=[]
                for i,frame in enumerate(closed):
                    im=Image.open(frame['pixels']).convert('RGB')
                    green=sum(1 for r,g,b in im.crop((970,100,1900,1000)).get_flattened_data() if g>180 and r<80 and b<80)
                    if green<10000:missing.append(i)
                result.append(dict(stage=stage,scope=scope,open_frames=len(opened),close_frames=len(closed),shadow_interior_run=shadow_runs(opened),blank_neighbor_frames=missing))
            if case=='switch' and scope=='onlycurrentworkspace':
                evaluate('hl.config({animations={enabled=false}}); hl.dispatch(hl.dsp.focus({workspace=901})); hl.config({animations={enabled=true}})')
    (root/'observations.json').write_text(json.dumps(result,indent=2))
    if not observe and case=='close':assert all(not r['blank_neighbor_frames'] for r in result),result
    if not observe and case=='shadow':assert all(r['shadow_interior_run']<=16 for r in result),result
    if not observe and case=='switch':assert all(not r['moved_frames'] and not r['missing_frames'] for r in result),result
    print(('OBSERVED' if observe else 'PASS')+': every-frame tile overview '+case+' '+json.dumps(result))
