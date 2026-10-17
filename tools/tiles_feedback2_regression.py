"""Private input/render regressions for the second tile feedback round."""
import json
import subprocess
import time
from PIL import Image


def run(case,request,root,env,runtime,processes,handles):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def wait(command,predicate):
        for _ in range(200):
            state=json.loads(request(command))
            if predicate(state):return state
            time.sleep(.02)
        raise AssertionError((command,state))
    def state():return json.loads(request('j/hymission-tiles-state'))
    def overview():return json.loads(request('j/hymission-overview-state'))
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    def capture(name):
        monitor=json.loads(request('j/monitors'))[0]
        path=root/(name+'.png')
        subprocess.run(['grim','-o',monitor['name'],str(path)],env=child,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        return Image.open(path).convert('RGB')
    evaluate('hl.config({animations={enabled=false},plugin={hymission={tiles_enabled=1,tile_min_width=120,workspace_strip_force_show=1,workspace_strip_empty_mode="always"}}}); hl.plugin.hymission_tiles_test.refresh()')
    wait('j/hymission-tiles-state',lambda s:s['enabled'])
    if case=='panel':
        evaluate('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1",reserved={top=80,right=0,bottom=24,left=0}})')
        wait('j/monitors',lambda ms:ms[0]['reserved'][1]>=80)
        evaluate('hl.plugin.hymission.tile_add(); hl.plugin.hymission.tile_add(); hl.plugin.hymission.open("forceall")')
        ov=wait('j/hymission-overview-state',lambda s:s['phase']=='active')
        (root/'panel-state.json').write_text(json.dumps(ov,indent=2))
        time.sleep(.1)
        image=capture('shared-panel')
        groups={t['id']:[e['geometry'] for e in ov['strip'] if e['tile_id']==t['id']] for t in state()['tiles']}
        centers=sorted(sum(b['x']+b['width']/2 for b in entries)/len(entries) for entries in groups.values())
        # No thumbnails or focus outline in the reserved header: the entire
        # glass panel must continue through it, including across column seams.
        for x in [int(v) for v in centers]+[int((a+b)/2) for a,b in zip(centers,centers[1:])]:
            a=image.getpixel((x,30));b=image.getpixel((x,90+500))
            assert max(abs(u-v) for u,v in zip(a,b))<8,('header/background discontinuity',x,a,b)
        for a,b in zip(centers,centers[1:]):
            seam=int((a+b)/2)
            row=[image.getpixel((x,30)) for x in range(seam-6,seam+7)]
            assert max(max(p[c] for p in row)-min(p[c] for p in row) for c in range(3))<3,('column seam',row)
        print('PASS: one continuous strip glass panel across columns and reserved header')
        return
    raise AssertionError('case not implemented: '+case)
