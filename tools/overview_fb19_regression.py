"""FB19: private compositor pixels for decoration continuity and live strip geometry.

No host IPC/input. Observe mode retains a failing baseline instead of asserting.
"""
import json
from pathlib import Path
import subprocess
import time
from PIL import Image, ImageChops


def run(mode, observe, fp16, request, root, env, runtime, processes, handles):
    results=[]
    def save(): (root/'fb19-results.json').write_text(json.dumps(results,indent=2))
    def check(ok,label,**data):
        results.append(dict(ok=bool(ok),label=label,**data));save()
        if not observe: assert ok,(label,data)
    def ev(code):
        result=request('/eval '+code);assert result.strip()=='ok',(code,result)
    def read(cmd):return json.loads(request(cmd))
    def wait(cmd,pred):
        for _ in range(300):
            value=read(cmd)
            if pred(value):return value
            time.sleep(.02)
        raise AssertionError((cmd,value))
    def capture(name,code,duration=.9):
        ev('hl.plugin.hymission_tiles_test.frame_capture("begin"); '+code)
        time.sleep(duration)
        prefix=root/name;ev('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(prefix))+')')
        frames=json.loads(prefix.with_suffix('.json').read_text());assert frames,(name,'no frames')
        presented=None
        for f in frames:
            p=Path(f['pixels']);im=Image.open(p).convert('RGB')
            if presented is None:presented=im.copy()
            else:
                for rect in f['framebuffer']['damage']:presented.paste(im.crop(tuple(rect)),tuple(rect[:2]))
            q=p.with_suffix('.png');presented.save(q);p.unlink();f['pixels']=str(q)
        prefix.with_suffix('.json').write_text(json.dumps(frames,indent=2))
        if fp16:check(all(f['framebuffer']['drm_format']==0x48344241 for f in frames),name+': FP16 workbuffer verified')
        return frames
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    for kind,name in [('client-header','xdg-shell-client.h'),('private-code','xdg-shell-protocol.c')]:subprocess.run(['wayland-scanner',kind,str(xml),str(root/name)],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'marker';subprocess.run(['cc',str(Path(__file__).with_name('tiles_top_marker_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    ev('hl.monitor({output="",mode="1280x720@60",position="auto",scale="1"}); hl.config({misc={background_color="rgb(6080b0)"},input={follow_mouse=1},general={layout="scrolling",border_size=4,gaps_out=75},scrolling={column_width=.32,fullscreen_on_one_column=false,focus_fit_method=1},animations={enabled=false},decoration={rounding=16,shadow={enabled=true,range=30,color="rgba(000000dd)"}},plugin={hymission={vim_keys=1,debug_logs=1,niri_mode=0,layout_engine="natural2",stage_enabled=0,stage_show_active=0,stage_transition_ms=0,stage_window_decorations=1,workspace_strip_force_show=1,workspace_strip_refresh_ms=500,workspace_change_keeps_overview=1,overview_focus_follows_mouse=1,selected_expand_scale=1,hover_expand_scale=1,backdrop_blur=0,backdrop_color="rgba(00000000)"}}})')
    if mode=='reference':ev('hl.config({plugin={hymission={layout_engine="natural"}}})')
    if fp16:ev('hl.config({render={use_fp16=1,keep_unmodified_copy=1}})')
    if mode not in ('off','reference'):ev('hl.config({plugin={hymission={tiles_enabled=1,tile_min_width=100}}}); hl.plugin.hymission_tiles_test.refresh()')
    count={'off':1,'reference':1,'single':1,'two':2,'three':3}[mode]
    clients={}
    for n in range(count):
        ws=901+n;ev(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))');clients[ws]=[]
        for i in range(3):
            before={c['address'] for c in read('j/clients')}
            log=open(root/f'client-{ws}-{i}.log','w');handles.append(log)
            cenv=child.copy();cenv['HMT_MARKER_COLOR']=('00ff00','ffff00','ff00ff')[i]
            proc=subprocess.Popen([str(binary)],env=cenv,stdout=log,stderr=subprocess.STDOUT);processes.append(proc)
            cs=wait('j/clients',lambda cs:len(cs)==len(before)+1)
            clients[ws].append(next(c['address'] for c in cs if c['address'] not in before))
    ev('hl.dispatch(hl.dsp.focus({workspace=901}))')
    for n in range(1,count):ev(f'hl.plugin.hymission.tile_add({{workspace="{901+n}"}})')
    for stage in (0,1):
        ev('hl.config({animations={enabled=false},plugin={hymission={stage_enabled='+str(stage)+'}}}); hl.plugin.hymission.stage_'+('open' if stage else 'close')+'()')
        time.sleep(.3)
        # Native focus handler really changes the scrolling column width; the
        # frozen overview layout must not be reused for a desktop thumbnail.
        for scope in ('onlycurrentworkspace','forceall'):
            ev('hl.plugin.hymission_tiles_test.pointer("move",10,710); hl.config({animations={enabled=true}})')
            ev('hl.config({gestures={workspace_swipe_distance=300,workspace_swipe_min_speed_to_force=0}})')
            ev('hl.plugin.hymission.gesture({fingers=4,direction="vertical",action="open",scope='+json.dumps(scope)+'})')
            held_open=capture(f'stage{stage}-{scope}-held-open','hl.plugin.hymission_tiles_test.trackpad("begin"); hl.plugin.hymission_tiles_test.trackpad("update",-30)',.15)
            held_open_high=capture(f'stage{stage}-{scope}-held-open-high','hl.plugin.hymission_tiles_test.trackpad("update",-180)',.15)
            ev('hl.plugin.hymission_tiles_test.trackpad("cancel")')
            wait('j/hymission-overview-state',lambda s:not s['active'])
            time.sleep(.4)
            ev('hl.config({input={follow_mouse=1}})')
            opened=capture(f'stage{stage}-{scope}-open',f'hl.plugin.hymission.open("{scope}")')
            wait('j/hymission-overview-state',lambda s:s['phase']=='active');time.sleep(.65)
            # Enter another preview first, so the measured hover cannot be a
            # no-op focus on the window automatically selected at overview entry.
            initial=read('j/hymission-overview-state')
            other=next(w['contentGeometry'] for w in initial['windows'] if w['address']==clients[901][1])
            ev(f'hl.plugin.hymission_tiles_test.pointer("move",{other["x"]+other["width"]/2},{other["y"]+other["height"]/2})')
            time.sleep(.2)
            before=read('j/hymission-overview-state')
            focus_before=read('j/activewindow')
            native_before=next(c for c in read('j/clients') if c['address']==clients[901][0])
            ev('hl.plugin.hymission_tiles_test.focus_resize(true)')
            address=clients[901][0]
            target=next(w['contentGeometry'] for w in before['windows'] if w['address']==address)
            changed=capture(f'stage{stage}-{scope}-focus',f'hl.plugin.hymission_tiles_test.pointer("move",{target["x"]+target["width"]/2},{target["y"]+target["height"]/2})',1.15)
            ev('hl.plugin.hymission_tiles_test.focus_resize(false)')
            after=read('j/hymission-overview-state')
            native_after=next(n for n in changed[-1]['native'] if n['address']==address)
            if mode!='reference':check(abs(native_after['size'][0]-native_before['size'][0])>10,f'{stage}/{scope}: hover actually resizes native scrolling column',before=native_before['size'],after=native_after['size'],focus_before=focus_before.get('address'),focus_after=read('j/activewindow').get('address'),target=address)
            # Pixel oracle: locate the uniquely green native client in its strip
            # entry. Its current compositor geometry maps through the monitor or
            # tile allocation, independently of the overview layout engine.
            samples=[]
            for f in changed[-3:]:
                strips=[s for s in f['overview'].get('strip',[]) if s['workspace']==901]
                if not strips:continue
                s=strips[0];b=s['geometry'];im=Image.open(f['pixels']).convert('RGB')
                native=next(n for n in f['native'] if n['address']==address)
                alloc=next((t for t in f['tiles'].get('tiles',[]) if t['id']==s['tile_id']),dict(x=0,y=0,width=1280,height=720))
                x,y=native['position'];w,h=native['size']
                # Body center, away from CSD/border/tints and other clients.
                yy=int(b['y']+((y+h*.55-alloc['y'])/alloc['height'])*b['height'])
                xs=[]
                if 0<=yy<im.height:
                    for xx in range(max(0,int(b['x']+1)),min(im.width,int(b['x']+b['width']-1))):
                        r,g,bl=im.getpixel((xx,yy))
                        if g>r+40 and g>bl+40:xs.append(xx)
                expected=(max(b['x'],b['x']+(x-alloc['x'])/alloc['width']*b['width']),min(b['x']+b['width'],b['x']+(x+w-alloc['x'])/alloc['width']*b['width']))
                error=max(abs(min(xs)-expected[0]),abs(max(xs)+1-expected[1])) if xs else 999
                samples.append(dict(error=error,expected=expected,observed=[min(xs),max(xs)+1] if xs else [],native=native))
            if mode!='reference':
                check(bool(samples) and max(s['error'] for s in samples)<=4,f'{stage}/{scope}: strip matches current native width/position',samples=samples)
            before_geo={w['address']:w.get('animationToGeometry', w['contentGeometry']) for w in before['windows']}
            after_geo={w['address']:w.get('animationToGeometry', w['contentGeometry']) for w in after['windows']}
            if mode!='reference':check(before_geo==after_geo,f'{stage}/{scope}: focus keeps overview main slots frozen')
            ev('hl.plugin.hymission.gesture({fingers=4,direction="vertical",action="toggle"})')
            held_close_high=capture(f'stage{stage}-{scope}-held-close-high','hl.plugin.hymission_tiles_test.trackpad("begin"); hl.plugin.hymission_tiles_test.trackpad("update",30)',.15)
            held_close=capture(f'stage{stage}-{scope}-held-close','hl.plugin.hymission_tiles_test.trackpad("update",210)',.15)
            ev('hl.plugin.hymission_tiles_test.trackpad("cancel")')
            wait('j/hymission-overview-state',lambda s:s['phase']=='active')
            closed=capture(f'stage{stage}-{scope}-close','hl.plugin.hymission.close()')
            wait('j/hymission-overview-state',lambda s:not s['active'])
            if mode!='reference':
                source=next(n for n in changed[-1]['native'] if n['address']==address)
                actual=next(c for c in read('j/clients') if c['address']==address)
                error=max(abs(x-y) for x,y in zip(source['position']+source['size'],actual['at']+actual['size']))
                check(error<=1,f'{stage}/{scope}: strip source equals actual post-exit desktop',error=error)
            # Capture all transition frames; darkness is measured just outside
            # each content edge, excluding strip/other windows and output edge.
            for name,frames in [('open',opened+held_open+held_open_high),('close',closed+held_close+held_close_high)]:
                darkness=[]
                for f in frames:
                    o=f['overview'];p=o.get('visualProgress')
                    progress_source='ipc'
                    if p is None:
                        # Pre-tiles IPC has no progress field. Estimate openness
                        # from the same linear width interpolation used by its
                        # preview rectangles; retain that evidence distinction.
                        active=opened[-1]['overview'] if name=='open' else after
                        targets={w['address']:w['contentGeometry'] for w in active['windows']}
                        candidates=[]
                        for w in o.get('windows',[]):
                            n=next((n for n in f['native'] if n['address']==w['address']),None)
                            t=targets.get(w['address'])
                            if n and t and abs(t['width']-n['size'][0])>10:
                                delta=t['width']-n['size'][0]
                                candidates.append((abs(delta),(w['contentGeometry']['width']-n['size'][0])/delta))
                        p=max(candidates)[1] if candidates else 0
                        progress_source='geometry-derived'

                    if not .02<p<.98:continue
                    im=Image.open(f['pixels']).convert('RGB');values=[]
                    for win in o.get('windows',[]):
                        b=win['contentGeometry'];xx=int(b['x']+b['width']*.5);yy=int(b['y']+b['height']+8)
                        if not (250<xx<im.width-35 and 40<yy<im.height-35):continue
                        # A local background control removes backdrop/color effects.
                        if any(other['address']!=win['address'] and other['contentGeometry']['x']-35<xx<other['contentGeometry']['x']+other['contentGeometry']['width']+35 and other['contentGeometry']['y']-35<yy<other['contentGeometry']['y']+other['contentGeometry']['height']+35 for other in o['windows']):continue
                        values.append(max(0,im.getpixel((xx,yy+30))[2]-im.getpixel((xx,yy))[2]))
                    if values:darkness.append(dict(progress=p,progress_source=progress_source,darkness=max(values)))
                low=[v['darkness'] for v in darkness if v['progress']<.25];high=[v['darkness'] for v in darkness if v['progress']>.7]
                check(len(low)>=1 and len(high)>=1 and min(low)>=max(5,max(high)*.7),f'{stage}/{scope}/{name}: shadow does not fade near desktop',samples=darkness)
    print('FB19 checks:',sum(r['ok'] for r in results),'/',len(results));save()
