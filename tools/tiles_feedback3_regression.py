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
    evaluate('hl.config({animations={enabled=false},plugin={hymission={tiles_enabled=1,tile_min_width=120,stage_enabled=0,workspace_strip_force_show=1,workspace_strip_empty_mode="always"}}}); hl.plugin.hymission_tiles_test.refresh()')
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
        print('PASS: hovering empty tile publishes native workspace context before switching')
        return
    raise AssertionError('unimplemented '+case)
