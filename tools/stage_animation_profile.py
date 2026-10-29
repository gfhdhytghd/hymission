"""Private nested-compositor Stage animation CPU/GPU frame profiles."""
import json
from pathlib import Path
import subprocess
import time


def run(request,root,env,runtime,processes,handles,case_filter="off",order="tiles-first",fp16_mirror=False):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def read(command):return json.loads(request(command))
    def wait(command,predicate):
        for _ in range(500):
            state=read(command)
            if predicate(state):return state
            time.sleep(.01)
        raise AssertionError((command,state))
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    if fp16_mirror:evaluate('hl.config({render={use_fp16=1,keep_unmodified_copy=1}})')
    evaluate('hl.monitor({output="",mode="6144x3456@60",position="auto",scale="2",reserved={top=58,right=0,bottom=0,left=0}}); hl.config({general={layout="scrolling"},scrolling={column_width=.85,fullscreen_on_one_column=true,focus_fit_method=1},animations={enabled=false},plugin={hymission={stage_enabled=0,niri_mode=0,stage_show_active=0,stage_transition_ms=500}}})')
    wait('j/monitors',lambda ms:ms[0]['width']==6144 and ms[0]['scale']==2)
    for ws in range(501,507):
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        for _ in range(3):
            count=len(read('j/clients'));log=open(root/f'client-{ws}-{count}.log','w');handles.append(log)
            processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
            wait('j/clients',lambda cs:len(cs)==count+1)
    evaluate('hl.plugin.hymission.stage_close()')
    wait('j/hymission-stage-state',lambda s:s['visibility_progress']==0)
    evaluate('hl.config({animations={enabled=true}})')
    try: has_tiles=isinstance(read('j/hymission-tiles-state'),dict)
    except (ValueError,KeyError): has_tiles=False
    cases=[('off',0,1),('single',1,1),('two',1,2),('three',1,3)] if has_tiles else [('off',0,1)]
    for label,enabled,count in cases:
        if label!=case_filter:continue
        evaluate('hl.plugin.hymission.stage_close(); hl.config({animations={enabled=false},plugin={hymission={stage_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
        wait('j/hymission-stage-state',lambda s:not s['enabled'])
        if order=='stage-first':evaluate('hl.plugin.hymission.stage_open(); hl.plugin.hymission.stage_close()')
        if has_tiles:
            evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
            wait('j/hymission-tiles-state',lambda s:not s['enabled'])
        evaluate('hl.dispatch(hl.dsp.focus({workspace=501}))')
        if has_tiles:evaluate(f'hl.config({{plugin={{hymission={{tiles_enabled={enabled}}}}}}}); hl.plugin.hymission_tiles_test.refresh()')
        if enabled:
            wait('j/hymission-tiles-state',lambda s:s['enabled'])
            for ws in (503,505)[:count-1]:evaluate(f'hl.plugin.hymission.tile_add({{workspace="{ws}"}})')
            if count>=2:evaluate('hl.plugin.hymission.tile_focus("left"); hl.plugin.hymission.tile_focus("right"); hl.dispatch(hl.dsp.focus({workspace=504})); hl.dispatch(hl.dsp.focus({workspace=503}))')
            if count==3:evaluate('hl.plugin.hymission.tile_focus("right"); hl.dispatch(hl.dsp.focus({workspace=506})); hl.dispatch(hl.dsp.focus({workspace=505}))')
        evaluate('hl.config({animations={enabled=true},plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission_tiles_test.refresh()')
        wait('j/hymission-stage-state',lambda s:s['visibility_progress']==0)
        time.sleep(.3)
        for cycle in range(4):
            for direction,target in (('open',1),('close',0)):
                before=read('j/hymission-stage-state')
                path=root/f'{label}-{direction}-{cycle}.json'
                start=time.monotonic()
                evaluate(f'hl.plugin.hymission_tiles_test.profile("begin"); hl.plugin.hymission.stage_{direction}()')
                last=wait('j/hymission-stage-state',lambda s:abs(s['visibility_progress']-target)<.0001)
                evaluate('hl.plugin.hymission_tiles_test.profile("end",'+json.dumps(str(path))+')')
                profile=json.loads(path.read_text());profile.update(case=label,order=order,fp16_mirror=fp16_mirror,direction=direction,cycle=cycle,wall_ms=(time.monotonic()-start)*1000,stage_before=before,stage_after=last)
                path.write_text(json.dumps(profile,indent=2))
                if fp16_mirror:assert profile['mirror_format'] and profile['drm_format']==0x48344241,('FP16/MRT not active',profile)
                assert len(profile['frames'])>=3,(label,direction,cycle,profile)
                time.sleep(.15)
        print('PROFILED',label,flush=True)
    print('PASS: Stage '+case_filter+' open/close frames, CPU render/layout, GL workload counters and async GPU queries; FP16/MRT='+str(fp16_mirror))


def summarize(directory):
    """Keep cold starts and warm cycles separate; never substitute missing GPU data."""
    import math
    import statistics
    groups={}
    for path in sorted(Path(directory).glob('*/*-*.json')):
        profile=json.loads(path.read_text())
        if 'frames' not in profile:continue
        assert not profile['pending'],('unresolved GPU queries',path)
        key=(profile['case'],profile['direction'],'cold' if profile['cycle']==0 else 'warm')
        groups.setdefault(key,[]).extend(profile['frames'])
    rows=[]
    for (case,direction,temperature),frames in sorted(groups.items()):
        def stats(values):
            values=sorted(values)
            return dict(median=statistics.median(values),p95=values[math.ceil(.95*len(values))-1],worst=max(values)) if values else None
        row=dict(case=case,direction=direction,cycles=temperature,frames=len(frames))
        for field in ('cpu_work_ms','cpu_render_ms','gpu_ms','interval_ms','layout_ms','layout_calls','symbol_calls'):
            row[field]=stats([f[field] for f in frames if field in f and (field!='interval_ms' or f[field]>0)])
        row['gpu_samples']=sum('gpu_ms' in f for f in frames)
        row['gl']={name:stats([f['gl'][name] for f in frames]) for name in frames[0].get('gl',{})}
        rows.append(row)
    assert rows,('no profiles',directory)
    return rows


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories',nargs='+',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--check-cache',action='store_true',help='fixed build: assert bounded warm-frame layout work and cached GL resources')
    args=parser.parse_args()
    results={str(p):summarize(p) for p in args.directories}
    if args.check_cache:
        for rows in results.values():
            for row in rows:
                if row['cycles']!='warm':continue
                assert row['gpu_samples']==row['frames'],('missing GPU timing',row)
                assert row['layout_calls']['median']<=1 and row['layout_calls']['worst']<=2,('repeated output layout',row)
                assert row['symbol_calls']['worst']==0,('per-frame symbol lookup',row)
                for key in ('uniform_lookups','texture_allocations','framebuffer_allocations','shader_links'):
                    assert row['gl'][key]['worst']==0,('warm GL cache miss',key,row)
    args.output.write_text(json.dumps(results,indent=2)+'\n')
