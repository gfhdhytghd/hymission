"""Identical private-session observations for reference and tiles-disabled builds."""
import json
import subprocess
import time


def run(request,root,env,runtime,processes,handles):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def wait(command,predicate,label):
        for _ in range(150):
            state=json.loads(request(command))
            if predicate(state): return state
            time.sleep(.03)
        raise AssertionError((label,state))
    def pointer(action,x,y):
        evaluate(f'hl.plugin.hymission_tiles_test.pointer("{action}",{x},{y})')
    child_env=env.copy(); child_env.pop('LD_PRELOAD',None)
    displays=[p for p in runtime.glob('wayland-*') if p.is_socket()]
    assert len(displays)==1
    child_env['WAYLAND_DISPLAY']=displays[0].name
    for ws in (1,2):
        evaluate(f'hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        log=open(root/f'default-client-{ws}.log','w'); handles.append(log)
        processes.append(subprocess.Popen(['weston-simple-shm'],env=child_env,stdout=log,stderr=subprocess.STDOUT))
        wait('j/clients',lambda cs:len(cs)==ws,'clients')
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1}))')
    evaluate('hl.config({plugin={hymission={stage_enabled=1,stage_card_min_width=16,stage_transition_ms=0}}}); hl.plugin.hymission.stage_open()')
    observations=[]
    for reserved in (False,True):
        if reserved:
            evaluate('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1",reserved={0,70,0,240}}); hl.plugin.hymission_tiles_test.refresh()')
        state=wait('j/hymission-stage-state',lambda s:s['enabled'] and s['hooks_ready'] and s['visibility_progress']==1 and s['captureWindows'] and s['screens'][0]['cards'],'Stage ready')
        # Wait for output configuration and preview geometry to settle.
        time.sleep(.15)
        state=json.loads(request('j/hymission-stage-state'))
        screen=state['screens'][0]
        geometry={k:screen[k] for k in ('card_width','card_height','desktop_width','desktop_height','reservation','sidebar_side')}
        client_by_address={c['address']:c for c in json.loads(request('j/clients'))}
        preview=state['captureWindows'][0]
        target_ws=client_by_address[preview['address']]['workspace']['id']
        rect=preview['selectionGeometry']; x=rect['x']+rect['width']/2; y=rect['y']+rect['height']/2
        pointer('press',x,y); pointer('release',x,y)
        active=wait('j/activeworkspace',lambda s:s['id']==target_ws,'card click')['id']
        # The monitor-edge extension must click the same card, including inside
        # a left reserved region (cardHit's activation range includes it).
        state=wait('j/hymission-stage-state',lambda s:s['captureWindows'] and all(c['workspace']!=active for c in s['screens'][0]['cards']) and
                   all(client_by_address[w['address']]['workspace']['id']!=active for w in s['captureWindows']),'post click')
        preview=state['captureWindows'][0]; rect=preview['selectionGeometry']
        client_by_address={c['address']:c for c in json.loads(request('j/clients'))}
        edge_target=client_by_address[preview['address']]['workspace']['id']
        pointer('press',1,rect['y']+rect['height']/2); pointer('release',1,rect['y']+rect['height']/2)
        wait('j/activeworkspace',lambda s:s['id']==edge_target,'edge click')
        evaluate('hl.config({animations={enabled=true},plugin={hymission={stage_transition_ms=220}}}); hl.plugin.hymission.open("forceall")')
        opening=wait('j/hymission-overview-state',lambda s:s['active'],'overview opening')['phase']
        overview=wait('j/hymission-overview-state',lambda s:s['active'] and s['phase']=='active','overview active')
        window_geometries=sorted((w['contentGeometry'] for w in overview['windows']),key=lambda b:(b['x'],b['y']))
        # Exit towards the other workspace using the public activation action.
        evaluate('hl.plugin.hymission.close()')
        closing=wait('j/hymission-overview-state',lambda s:s['phase'] in ('closing','closing-settle','inactive'),'overview closing')['phase']
        wait('j/hymission-overview-state',lambda s:not s['active'],'overview closed')
        stage=wait('j/hymission-stage-state',lambda s:s['screens'] and s['screens'][0]['overview_render_owner']=='stage','Stage restored')
        assert stage['hooks_ready'] and not stage['error']
        evaluate('hl.config({animations={enabled=false},plugin={hymission={stage_transition_ms=0}}})')
        observations.append({'reserved':reserved,'geometry':geometry,'card_click':active,'edge_click':edge_target,
                             'overview_windows':window_geometries,'opening_observed':opening!='inactive','closing_observed':closing in ('closing','closing-settle'),
                             'stage_restored':True})
    (root/'default-off-observations.json').write_text(json.dumps(observations,indent=2))
    (root/'configerrors.txt').write_text(request('/configerrors'))
    print('PASS: tiles-disabled Stage dimensions, card/monitor-edge clicks, overview entry/exit and Stage restoration')
