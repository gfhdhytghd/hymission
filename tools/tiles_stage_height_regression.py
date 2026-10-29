"""Compare real Stage desktop/window height with tiles off, single, two, three."""
import json
from pathlib import Path
import subprocess
import time
from PIL import Image


def run(case,observe,request,root,env,runtime,processes,handles):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def read(command):return json.loads(request(command))
    def wait(command,predicate):
        for _ in range(300):
            state=read(command)
            if predicate(state):return state
            time.sleep(.02)
        raise AssertionError((command,state))
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    def capture(name):
        path=root/(name+'.png')
        subprocess.run(['grim','-o',read('j/monitors')[0]['name'],str(path)],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        return Image.open(path).convert('RGB')
    try:has_tiles=isinstance(read('j/hymission-tiles-state'),dict)
    except ValueError:has_tiles=False
    assert has_tiles or case=='off','reference has no tile capability'
    evaluate('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1",reserved={top=80,right=0,bottom=24,left=0}}); hl.config({misc={background_color="rgb(6080b0)"},general={layout="scrolling"},scrolling={column_width=.85,fullscreen_on_one_column=true,focus_fit_method=1},animations={enabled=false},plugin={hymission={stage_enabled=0,niri_mode=0,stage_transition_ms=0,stage_show_active=0}}})')
    wait('j/monitors',lambda ms:ms[0]['reserved'][1]==80 and ms[0]['reserved'][3]==24)
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    subprocess.run(['wayland-scanner','client-header',str(xml),str(root/'xdg-shell-client.h')],check=True)
    subprocess.run(['wayland-scanner','private-code',str(xml),str(root/'xdg-shell-protocol.c')],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    for ws in range(1001,1007):
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        log=open(root/f'client-{ws}.log','w');handles.append(log)
        processes.append(subprocess.Popen([str(binary)],env=child,stdout=log,stderr=subprocess.STDOUT))
        wait('j/clients',lambda cs:len(cs)==ws-1000)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1001}))')
    count={'off':0,'single':1,'two':2,'three':3}[case]
    if has_tiles:
        evaluate('hl.config({plugin={hymission={tiles_enabled='+str(int(count>0))+'}}}); hl.plugin.hymission_tiles_test.refresh()')
        wait('j/hymission-tiles-state',lambda s:s['enabled']==bool(count))
        for ws in (1003,1005)[:max(0,count-1)]:evaluate(f'hl.plugin.hymission.tile_add({{workspace="{ws}"}})')
        if count==3:evaluate('hl.dispatch(hl.dsp.focus({workspace=1006})); hl.dispatch(hl.dsp.focus({workspace=1005})); hl.plugin.hymission.tile_focus("left")')
        if count>1:evaluate('hl.dispatch(hl.dsp.focus({workspace=1004})); hl.dispatch(hl.dsp.focus({workspace=1003})); hl.plugin.hymission.tile_focus("left")')
    time.sleep(.2)
    before=read('j/clients');capture('desktop-before')
    evaluate('hl.config({plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission.stage_open()')
    stage=wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1 and len(s['screens'])==max(1,count) and all(sc['cards'] for sc in s['screens']))
    time.sleep(.25);stage=read('j/hymission-stage-state');after=read('j/clients');im=capture('desktop-stage')
    observations=[]
    for sc in stage['screens']:
        ws=sc.get('active_workspace',read('j/activeworkspace')['id'])
        old=next(c for c in before if c['workspace']['id']==ws);new=next(c for c in after if c['address']==old['address'])
        # Probe the old desktop's upper/lower body rows, not just its center:
        # an accidentally centered/shrunken window must fail the raster check.
        x=int(new['at'][0]+new['size'][0]*.5)
        samples=[im.getpixel((x,int(old['at'][1]+dy))) for dy in (64,old['size'][1]-10)]
        monitor=read('j/monitors')[0];left,top,right,bottom=monitor['reserved'];scale=monitor['scale']
        base=sc.get('base',dict(x=monitor['x']+left,y=monitor['y']+top,width=monitor['width']/scale-left-right,height=monitor['height']/scale-top-bottom))
        observations.append(dict(tile_id=sc.get('tile_id',0),workspace=ws,base=base,desktop=sc.get('desktop_geometry',dict(x=base['x']+sc['reservation'],y=base['y'],width=sc['desktop_width'],height=sc['desktop_height'])),card_width=sc['card_width'],card_height=sc['card_height'],sidebar=sc.get('sidebar'),before_at=old['at'],before_size=old['size'],after_at=new['at'],after_size=new['size'],edge_pixels=samples))
    (root/'geometry.json').write_text(json.dumps(dict(case=case,observe=observe,observations=observations,stage=stage,before=before,after=after),indent=2))
    if not observe:
        for o in observations:
            assert abs(o['desktop']['height']-o['base']['height'])<.01 and abs(o['desktop']['y']-o['base']['y'])<.01,('desktop vertically shrunk',o)
            assert abs(o['after_size'][1]-o['before_size'][1])<=1 and abs(o['after_at'][1]-o['before_at'][1])<=1,('window lost native vertical extent',o)
            assert all(g>180 and r<80 and b<80 for r,g,b in o['edge_pixels']),('upper/lower desktop body is blank',o)
        assert max(o['card_height'] for o in observations)-min(o['card_height'] for o in observations)<.01,('unequal card heights',observations)
        if count>1:
            assert max(o['desktop']['width']/o['base']['width'] for o in observations)-min(o['desktop']['width']/o['base']['width'] for o in observations)<.001,('independent tile compression',observations)
            for a,b in zip(observations,observations[1:]):
                assert abs(a['desktop']['x']+a['desktop']['width']-b['desktop']['x'])<.01,('desktop seam',observations)
            assert max(o['card_width']/o['base']['width'] for o in observations)-min(o['card_width']/o['base']['width'] for o in observations)<.001,('column allocation changed',observations)
    evaluate('hl.plugin.hymission.stage_close()')
    wait('j/hymission-stage-state',lambda s:s['visibility_progress']==0)
    time.sleep(.2);capture('desktop-closed')
    if not observe:
        closed={c['address']:c for c in read('j/clients')}
        assert all(abs(closed[c['address']]['at'][1]-c['at'][1])<=1 and abs(closed[c['address']]['size'][1]-c['size'][1])<=1 for c in before),('vertical geometry changed after closing Stage',before,closed)
    print(('OBSERVED' if observe else 'PASS')+': Stage '+case+' '+json.dumps(observations))
