"""Stage layout viewport/column/camera measurements, private compositor only."""
import json
import subprocess
import time
from pathlib import Path
from PIL import Image


def validate_camera(root):
    root=Path(root)
    before=json.loads((root/'before-state.json').read_text())
    opened=json.loads((root/'opened-state.json').read_text())
    samples=0;max_error=0.;min_visible=1.
    for screen in opened['stage']['screens']:
        ws=screen['active_workspace']
        initial=next(w for w in before['layout'] if w['workspace']==ws)
        if 'offset' not in initial:continue
        area=initial['work_area']
        candidates=[c for c in before['clients'] if c['workspace']['id']==ws]
        focused=min(candidates,key=lambda c:abs(c['at'][0]+c['size'][0]/2-area['x']-area['width']/2))['address']
        for name in ('opening','closing'):
            for frame in json.loads((root/(name+'.json')).read_text()):
                state=next((w for w in frame.get('layout',[]) if w['workspace']==ws),None)
                stage=next((s for s in frame['stage']['screens'] if s['active_workspace']==ws),None)
                window=next((w for w in frame['native'] if w['address']==focused),None)
                if not state or not stage or not window:continue
                error=abs(state['offset']-initial['offset']*state['work_area']['width']/area['width'])
                max_error=max(max_error,error);samples+=1
                desk=stage['desktop_geometry'];x=window['position'][0];width=window['size'][0]
                visible=max(0,min(x+width,desk['x']+desk['width'])-max(x,desk['x']))/max(1,width)
                min_visible=min(min_visible,visible)
    assert samples and max_error<3,(samples,max_error)
    assert min_visible>.98,min_visible
    result=dict(samples=samples,max_camera_error=max_error,min_focused_column_visible=min_visible,passed=True)
    (root/'camera-validation.json').write_text(json.dumps(result,indent=2));return result


def run(case,observe,layout,request,root,env,runtime,processes,handles,warmup=False):
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
    configuration='hl.monitor({output="",mode="1280x720@60",position="auto",scale="1"}); hl.config({misc={background_color="rgb(6080b0)"},input={follow_mouse=0},general={layout="scrolling",border_size=5},scrolling={column_width=.55,fullscreen_on_one_column=false,focus_fit_method=1},animations={enabled=false},decoration={rounding=20,shadow={enabled=true,range=24,color="rgba(000000cc)"}},plugin={hymission={tiles_enabled=1,tile_min_width=100,niri_mode=0,stage_enabled=0,stage_show_active=0,stage_transition_ms=450,workspace_strip_force_show=0,overview_focus_follows_mouse=0,hover_expand_scale=1}}}); hl.plugin.hymission_tiles_test.refresh()'
    if layout=="dwindle":configuration=configuration.replace('layout="scrolling"','layout="dwindle"')
    evaluate(configuration)
    count={'off':1,'single':1,'two':2,'three':3}[case]
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

    def snapshot(name):
        path=root/(name+'-layout.json')
        evaluate('hl.plugin.hymission_tiles_test.layout_state('+json.dumps(str(path))+')')
        value=dict(layout=json.loads(path.read_text()),stage=read('j/hymission-stage-state'),clients=read('j/clients'),tiles=read('j/hymission-tiles-state'))
        (root/(name+'-state.json')).write_text(json.dumps(value,indent=2))
        subprocess.run(['grim','-o',read('j/monitors')[0]['name'],str(root/(name+'.png'))],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        return value
    def record(name,code):
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin"); '+code)
        time.sleep(.95)
        prefix=root/name
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(prefix))+')')
        frames=json.loads(prefix.with_suffix('.json').read_text())
        for frame in frames:
            p=Path(frame['pixels']);q=p.with_suffix('.png');Image.open(p).save(q);p.unlink();frame['pixels']=str(q)
        prefix.with_suffix('.json').write_text(json.dumps(frames,indent=2));return frames
    for address in centers:evaluate(f'hl.plugin.hymission_tiles_test.window("focus","{address}")')
    time.sleep(.2)
    if warmup:
        # Explicit steady-state control; the default still records cold startup.
        evaluate('hl.plugin.hymission.stage_open()')
        wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1)
        evaluate('hl.plugin.hymission.stage_close()')
        wait('j/hymission-stage-state',lambda s:s['visibility_progress']==0)
        time.sleep(.2)
    before=snapshot('before')
    evaluate('hl.config({animations={enabled=true}})')
    opened=record('opening','hl.plugin.hymission.stage_open()')
    wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1)
    after=snapshot('opened')
    closed=record('closing','hl.plugin.hymission.stage_close()')
    wait('j/hymission-stage-state',lambda s:s['visibility_progress']==0)
    restored=snapshot('closed')
    by=lambda value,ws:next(v for v in value['layout'] if v['workspace']==ws)
    values=[]
    for tile in range(count):
        ws=901+tile*2;a=by(before,ws);b=by(after,ws);c=by(restored,ws)
        screen=next(s for s in after['stage']['screens'] if s['active_workspace']==ws)
        usable=b.get('usable_area',b['work_area']);initial=a.get('usable_area',a['work_area'])
        reservation=screen['reservation'];base=screen['base'];desk=screen['desktop_geometry']
        # One Stage desktop with several scrolling columns: the tape keeps its
        # sizing width and moves by the reservation; nothing is resized.
        pushed=layout=='scrolling' and count==1 and len(a.get('columns',[]))>1
        expected=initial['width']*(desk['width']/base['width']) if count>1 else initial['width'] if pushed else initial['width']-reservation
        check(abs(usable['width']-expected)<2,f'tile {tile}: viewport keeps its width (pushed) or shrinks to allocated desktop width')
        if pushed:
            check(abs(abs(b['work_area']['x']-a['work_area']['x'])-reservation)<2,f'tile {tile}: tape moves by the Stage reservation')
            check(abs(c['work_area']['x']-a['work_area']['x'])<1,f'tile {tile}: closing restores position')
        check(abs(c['work_area']['width']-a['work_area']['width'])<1,f'tile {tile}: closing restores width')
        check(abs(b['work_area']['height']-a['work_area']['height'])<1,f'tile {tile}: vertical work area unchanged')
        if layout=='scrolling':
            ratio=usable['width']/initial['width']
            check(all(abs(y['fraction']-x['fraction'])<.0001 for x,y in zip(a['columns'],b['columns'])),f'tile {tile}: normalized column fractions preserved')
            check(all(abs(y['targets'][0]['width']-x['targets'][0]['width']*ratio)<2 for x,y in zip(a['columns'],b['columns'])),f'tile {tile}: native column boxes relaid out at viewport ratio')
            check(abs(b['offset']-a['offset']*ratio)<3,f'tile {tile}: camera remains anchored in column coordinates')
        for name,frames in [('open',opened),('close',closed)]:
            samples=[next((v for v in f.get('layout',[]) if v['workspace']==ws),None) for f in frames]
            # A pushed tape animates its position; a resized one its width.
            axis='x' if pushed else 'width'
            widths=[v['work_area'][axis] for v in samples if v]
            distinct={round(w,1) for w in widths if min(a['work_area'][axis],b['work_area'][axis])+.5<w<max(a['work_area'][axis],b['work_area'][axis])-.5}
            check(len(distinct)>=3,f'tile {tile}: {name} has native intermediate viewport {axis} values')
            check(all(abs(v['work_area']['height']-a['work_area']['height'])<1 for v in samples if v),f'tile {tile}: {name} preserves height throughout')
            if pushed:
                check(all(abs(v['work_area']['width']-a['work_area']['width'])<1 for v in samples if v),f'tile {tile}: {name} never resizes the pushed tape')
            check(max((abs(y-x) for x,y in zip(widths,widths[1:])),default=0)<abs(a['work_area'][axis]-b['work_area'][axis])*.55+2,f'tile {tile}: {name} has no one-frame {axis} jump')
        values.append(dict(workspace=ws,before=a,opened=b,closed=c,desktop=desk,expected_width=expected))
    (root/'measurements.json').write_text(json.dumps(values,indent=2))
    if not observe and layout=='scrolling':validate_camera(root)
    print(('OBSERVED' if observe else 'PASS')+': native Stage viewport '+case+' '+layout)


if __name__=='__main__':
    import sys
    for root in sys.argv[1:]:print(root,json.dumps(validate_camera(root)))
