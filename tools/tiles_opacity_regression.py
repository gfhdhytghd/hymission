"""Private before/after pixel observations for native and overview transparency."""
import json
import subprocess
import time
from pathlib import Path
from PIL import Image
import numpy as np


def run(request, root, env, runtime, processes, handles, observe=False, fp16=False, tiles=False, stage=False, cm="auto", niri=False, hover=False, user=False, session=False):
    user = user or session
    hover = hover or session
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def read(command):return json.loads(request(command))
    def wait(command,predicate):
        for _ in range(200):
            value=read(command)
            if predicate(value):return value
            time.sleep(.025)
        (root/'failure.json').write_text(json.dumps(value,indent=2))
        raise AssertionError((command,value))
    child=env.copy();child.pop('LD_PRELOAD',None)
    child['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml=Path(subprocess.check_output(['pkg-config','--variable=pkgdatadir','wayland-protocols'],text=True).strip())/'stable/xdg-shell/xdg-shell.xml'
    for kind,name in [('client-header','xdg-shell-client.h'),('private-code','xdg-shell-protocol.c')]:
        subprocess.run(['wayland-scanner',kind,str(xml),str(root/name)],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    binary=root/'opacity-client'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_opacity_client.c')),str(root/'xdg-shell-protocol.c'),'-I'+str(root),*flags,'-o',str(binary)],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','gtk4','gtk4-layer-shell-0'],text=True).split()
    background=root/'background'
    subprocess.run(['cc',str(Path(__file__).with_name('tiles_opacity_background.c')),*flags,'-o',str(background)],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','gtk4','gtk4-layer-shell-0'],text=True).split()
    bar=root/'bar';subprocess.run(['cc',str(Path(__file__).with_name('search_reserved_test_bar.c')),*flags,'-o',str(bar)],check=True)
    def spawn(binary,name,alpha=255,terminal=False):
        log=open(root/(name+'.log'),'w');handles.append(log)
        current=child.copy();current.update(FB20_ID=name,FB20_ALPHA=str(alpha))
        command = ['kitty','--config','NONE','--class',name,'--title',name,
                   '-o','background=#f0a040','-o','foreground=#000000','-o','background_opacity=0.5',
                   '-o','hide_window_decorations=yes','-o','shell_integration=disabled',
                   '-o','confirm_os_window_close=0','-o','cursor_blink_interval=0',
                   '/bin/sh','-c','printf "FB20 transparent terminal\\nwindowrule / client alpha + blur\\n"; exec sleep 3600'] if terminal else [str(binary)]
        p=subprocess.Popen(command,env=current,stdout=log,stderr=subprocess.STDOUT);processes.append(p);return p
    def capture(name):
        if '-hover-' not in name:
            evaluate('hl.plugin.hymission_tiles_test.pointer("move",0,0)')
        time.sleep(.25)
        subprocess.run(['grim',str(root/(name+'.png'))],env=child,check=True,capture_output=True)
        state=dict(overview=read('j/hymission-overview-state'),clients=read('j/clients'),stage=read('j/hymission-stage-state'),tiles=read('j/hymission-tiles-state'),monitors=read('j/monitors'))
        (root/(name+'.json')).write_text(json.dumps(state,indent=2))
        return state
    evaluate('hl.config({general={layout="scrolling",gaps_in=10,gaps_out=20},animations={enabled=false},decoration={rounding=0,shadow={enabled=false},blur={enabled=false},active_opacity=1.0,inactive_opacity=1.0},misc={background_color=0xff808080},plugin={hymission={vim_keys=0,pick_labels_enabled=1,pick_labels_show=0,overview_focus_follows_mouse=1,selected_expand_scale=1.0,hover_expand_scale=1.0,workspace_strip_force_show=0,workspace_overview_max_preview_scale=0.5,workspace_strip_empty_mode="existing",workspace_strip_refresh_ms=100,backdrop_color=0x00000000,backdrop_blur=0,stage_transition_ms=0,stage_enabled=0,tiles_enabled='+str(int(tiles))+'}}})')
    if fp16:evaluate('hl.config({render={use_fp16=1,keep_unmodified_copy=1}})')
    if cm!='auto':evaluate('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1",cm="'+cm+'"})')
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1})); hl.plugin.hymission_tiles_test.refresh()')
    spawn(background,'background')
    wait('j/layers',lambda s:'fb20-background' in json.dumps(s))
    if user:
        spawn(bar,'bar');wait('j/monitors',lambda s:s[0]['reserved'][1]==48)
    for i,(name,opacity,alpha) in enumerate([('fb20-rule','0.8 override 0.8 override',255),('fb20-active','0.85 override 0.4 override',255),('fb20-client','1.0 override 1.0 override',128),('fb20-opaque','1.0 override 1.0 override',255)]):
        evaluate('hl.window_rule({name="'+name+'",match={class="^'+name+'$"},opacity="'+opacity+'"})')
        if user and not session and name=='fb20-client':
            evaluate('hl.window_rule({name="fb20-float",match={class="^fb20-client$"},float=true,size="600 400",move="500 300"})')
        spawn(binary,name,alpha,terminal=session and name=='fb20-client')
        wait('j/clients',lambda cs:len(cs)==i+1)
    if session:
        for workspace in (3,2):
            evaluate(f'hl.workspace_rule({{workspace="{workspace}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={workspace}}}))')
            spawn(binary,'fb20-other-'+str(workspace))
            wait('j/clients',lambda cs:len(cs)==(5 if workspace==3 else 6))
        evaluate('hl.dispatch(hl.dsp.focus({workspace=1}))')
        evaluate('hl.config({plugin={hymission={selected_expand_scale=1.2,hover_expand_scale=1.2}}})')
    if tiles:
        evaluate('hl.workspace_rule({workspace="2",persistent=true}); hl.plugin.hymission.tile_add({workspace="2"})')
        wait('j/hymission-tiles-state',lambda s:len(s['tiles'])==2)
    if stage:
        evaluate('hl.config({plugin={hymission={stage_enabled=1,stage_show_active=0}}}); hl.plugin.hymission.stage_open()')
        wait('j/hymission-stage-state',lambda s:s['visibility_progress']==1)
    if not user:
        evaluate('hl.config({general={layout="dwindle"},plugin={hymission={overview_focus_follows_mouse='+str(int(hover))+',workspace_overview_max_preview_scale=0.95}}})')
    results=[]
    for blur in (False,True):
        evaluate('hl.config({decoration={blur={enabled='+str(blur).lower()+',size=8,passes=2,ignore_opacity='+str(user).lower()+',special=true,new_optimizations=true,xray=false}},plugin={hymission={niri_mode='+str(int(niri))+',layout_engine="'+('thumbnail' if niri else 'natural')+'"}}})')
        for focus in ('fb20-opaque','fb20-active'):
            target=next(c for c in read('j/clients') if c['class']==focus)
            evaluate('hl.dispatch(hl.dsp.focus({window="address:'+target['address']+'"}))')
            name='blur-'+str(int(blur))+'-'+focus
            capture(name+'-desktop')
            for scope in ('forceall','onlycurrentworkspace'):
                evaluate('hl.dispatch(hl.dsp.focus({window="address:'+target['address']+'"}))')
                time.sleep(.2)
                label=name+'-'+scope
                evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin")');evaluate('hl.plugin.hymission.open("'+scope+'")')
                wait('j/hymission-overview-state',lambda s:s['phase']=='active' and not s['relayout_active'])
                evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(root/(label+'-opening')))+')')
                data=capture(label)
                results.append(dict(name=label,state=data))
                if hover:
                    for selected in (('fb20-rule','fb20-client','fb20-active','fb20-opaque') if session else ('fb20-active','fb20-opaque')):
                        current=read('j/hymission-overview-state')
                        window=next(w for w in current['windows'] if w['class']==selected)
                        box=window['contentGeometry'];x=box['x']+box['width']/2;y=box['y']+box['height']/2
                        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin")')
                        evaluate(f'hl.plugin.hymission_tiles_test.pointer("move",{x},{y})')
                        time.sleep(.35)
                        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(root/(label+'-hover-'+selected+'-transition')))+')')
                        capture(label+'-hover-'+selected)
                        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin")');time.sleep(.06)
                        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(root/(label+'-hover-'+selected+'-frames')))+')')

                evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin"); hl.plugin.hymission.close()')
                wait('j/hymission-overview-state',lambda s:not s['active'])
                evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(str(root/(label+'-closing')))+')')
    (root/'observations.json').write_text(json.dumps(dict(fp16=fp16,tiles=tiles,stage=stage,cm=cm,niri=niri,user=user,session=session,cases=results),indent=2))
    (root/'configerrors.txt').write_text(request('/configerrors'))
    if not observe:
        assert (not user or session) and not niri, 'Legacy scene observations require --opacity-observe'
        from tiles_opacity_pixels import verify
        verify(root, fp16)
    print(('OBSERVED' if observe else 'PASS')+': main preview opacity rule, active/inactive, client alpha; blur off/on; all/workspace and animation pixels (desktop/strip observations retained)')
