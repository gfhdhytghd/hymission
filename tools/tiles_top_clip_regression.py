"""Check a real CSD header above the layout work area, in native/Stage/overview."""
import json
from pathlib import Path
import subprocess
import time
from PIL import Image


def run(request,root,env,runtime,processes,handles):
    def evaluate(code):
        value=request('/eval '+code)
        assert value.strip()=='ok',(code,value)
    def wait(command,predicate,label):
        for _ in range(150):
            state=json.loads(request(command))
            if predicate(state):return state
            time.sleep(.03)
        (root/(label+'.json')).write_text(json.dumps(state,indent=2))
        raise AssertionError((label,state))
    child=env.copy(); child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'tiles-top-marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    evaluate('hl.config({animations={enabled=false},general={gaps_in=0,gaps_out=0},plugin={hymission={stage_enabled=0,stage_transition_ms=0}}}); hl.plugin.hymission.stage_close(); hl.dispatch(hl.dsp.focus({workspace=401}))')
    log=open(root/'top-marker.log','w');handles.append(log)
    client=subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT);processes.append(client)
    clients=wait('j/clients',lambda cs:len(cs)==1,'top-marker-open')
    address=clients[0]['address']
    def check(name,rect):
        # The red CSD band extends above xdg window geometry. Looking only at
        # client layout coordinates would miss the regression entirely.
        monitor=json.loads(request('j/monitors'))[0]
        time.sleep(.12)
        path=root/(name+'.png')
        subprocess.run(['grim','-o',monitor['name'],str(path)],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        scale=monitor.get('scale',1)
        x,y,w,h=rect
        left=max(0,int((x+w*.25-monitor['x'])*scale));right=min(int(monitor['width']),int((x+w*.75-monitor['x'])*scale))
        top=max(0,int((y-64-monitor['y'])*scale));bottom=min(int(monitor['height']),int((y+min(h*.3,64)-monitor['y'])*scale))
        im=Image.open(path).convert('RGB')
        red=sum(r>120 and r>g*1.8 and r>b*1.8 for r,g,b in im.crop((left,top,right,bottom)).getdata())
        assert red>max(2,(right-left)*.5),(name,rect,red)
    for enabled in (False,True):
        evaluate(f'hl.config({{plugin={{hymission={{tiles_enabled={1 if enabled else 0},stage_enabled=0}}}}}}); hl.plugin.hymission_tiles_test.refresh()')
        wait('j/hymission-tiles-state',lambda s:s['enabled']==enabled,'top-capability')
        evaluate('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1",reserved={top=80,right=0,bottom=24,left=0}})')
        wait('j/monitors',lambda ms:ms[0]['reserved'][1]>=80,'top-reserved')
        if enabled:evaluate('hl.plugin.hymission.tile_add(); hl.plugin.hymission.tile_add(); hl.plugin.hymission.tile_focus("left"); hl.plugin.hymission.tile_focus("left")')
        prefix='top-tiles-on' if enabled else 'top-tiles-off'
        cs=wait('j/clients',lambda cs:cs and cs[0]['size'][0]>0,'top-native-ready');c=cs[0]
        check(prefix+'-native',(*c['at'],*c['size']))
        evaluate('hl.config({plugin={hymission={stage_enabled=1,stage_show_active=1}}}); hl.plugin.hymission.stage_open()')
        stage=wait('j/hymission-stage-state',lambda s:s['captureWindows'] and s['visibility_progress']==1,'top-stage-ready')
        sc=next(sc for sc in stage['screens'] if any(w['address']==address for c in sc['cards'] for w in c.get('windows',[])))
        w=next(w for c in sc['cards'] for w in c.get('windows',[]) if w['address']==address)
        check(prefix+'-stage',(w['x'],w['y'],w['width'],w['height']))
        c=json.loads(request('j/clients'))[0];check(prefix+'-desktop-stage',(*c['at'],*c['size']))
        for scope in ('forceall','onlycurrentworkspace'):
            evaluate(f'hl.plugin.hymission.open("{scope}")')
            ov=wait('j/hymission-overview-state',lambda s:s['active'] and s['phase']=='active' and s['windows'],'top-overview-ready')
            w=next(w for w in ov['windows'] if w['address']==address)['contentGeometry']
            check(prefix+'-'+scope,(w['x'],w['y'],w['width'],w['height']))
            evaluate('hl.plugin.hymission.close()')
            wait('j/hymission-overview-state',lambda s:not s['active'],'top-overview-close')
        evaluate('hl.plugin.hymission.stage_close()')
    client.terminate();wait('j/clients',lambda cs:not cs,'top-client-closed')
    print('PASS: red CSD header retained above reserved layout area in native, Stage and both overview scopes, tiles off/on')
