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
            evaluate('hl.monitor({output="",mode="1920x1080@60",position="auto",scale="1",reserved={top=0,right=70,bottom=0,left=240}}); hl.plugin.hymission_tiles_test.refresh()')
            wait('j/monitors',lambda ms:ms and sum(ms[0]['reserved'])>=310,'reserved areas applied')
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
        # Exit to a window on the other workspace: the endpoint must use the
        # pending activeWorkspace, even while Stage still has its old snapshot.
        active_ws=json.loads(request('j/activeworkspace'))['id']
        chosen=next(w for w in overview['windows'] if client_by_address[w['address']]['workspace']['id']!=active_ws)
        exit_ws=client_by_address[chosen['address']]['workspace']['id']
        b=chosen['contentGeometry']; px=b['x']+b['width']/2; py=b['y']+b['height']/2
        pointer('press',px,py); pointer('release',px,py)
        closing=wait('j/hymission-overview-state',lambda s:s['phase'] in ('closing','closing-settle','inactive'),'overview closing')['phase']
        wait('j/hymission-overview-state',lambda s:not s['active'],'overview closed')
        wait('j/activeworkspace',lambda s:s['id']==exit_ws,'overview selected workspace')
        stage=wait('j/hymission-stage-state',lambda s:s['screens'] and s['screens'][0]['overview_render_owner']=='stage','Stage restored')
        assert stage['hooks_ready'] and not stage['error']
        evaluate('hl.config({animations={enabled=false},plugin={hymission={stage_transition_ms=0}}})')
        observations.append({'reserved':reserved,'geometry':geometry,'card_click':active,'edge_click':edge_target,
                             'overview_windows':window_geometries,'overview_exit_workspace':exit_ws,'opening_observed':opening!='inactive','closing_observed':closing in ('closing','closing-settle'),
                             'stage_restored':True})
    # A Stage press whose release arrives during capture input suppression must be
    # left alone. Once suppression ends, Stage still owns its original release.
    stage=wait('j/hymission-stage-state',lambda s:s['captureWindows'],'suppressed-release-ready')
    r=stage['captureWindows'][0]['selectionGeometry']; px=r['x']+r['width']/2; py=r['y']+r['height']/2
    evaluate(f'assert(hl.plugin.hymission_tiles_test.pointer("press",{px},{py}))')
    assert request('/hymission-capture-input begin tiles-review').strip()=='ok'
    evaluate(f'assert(not hl.plugin.hymission_tiles_test.pointer("release",{px},{py}))')
    assert request('/hymission-capture-input end tiles-review').strip()=='ok'
    evaluate(f'assert(hl.plugin.hymission_tiles_test.pointer("release",{px},{py}))')
    assert request('/hymission-capture-input begin tiles-review').strip()=='ok'
    evaluate(f'assert(not hl.plugin.hymission_tiles_test.pointer("press",{px},{py}))')
    assert request('/hymission-capture-input end tiles-review').strip()=='ok'
    evaluate(f'assert(not hl.plugin.hymission_tiles_test.pointer("release",{px},{py}))')
    # A press while suppressed must not leave stale release ownership either.
    evaluate('hl.plugin.hymission.open("forceall")')
    overview=wait('j/hymission-overview-state',lambda s:s['active'] and s['phase']=='active' and s['windows'],'escape-drag-overview')
    before={w['address']:w['workspace']['id'] for w in json.loads(request('j/clients'))}
    r=overview['windows'][0]['contentGeometry']; px=r['x']+r['width']/2; py=r['y']+r['height']/2
    pointer('press',px,py); pointer('move',px+35,py+35)
    evaluate('assert(hl.plugin.hymission_tiles_test.pointer("escape"))')
    wait('j/hymission-overview-state',lambda s:not s['active'],'escape-drag-closes-overview')
    pointer('release',px+35,py+35)
    assert before=={w['address']:w['workspace']['id'] for w in json.loads(request('j/clients'))}
    observations.append({'suppressed_release_retained':True,'overview_drag_escape_closes':True,'escape_keeps_workspaces':True})
    # Real trackpad event entry, including the replaced native gesture wrapper.
    evaluate('hl.gesture({fingers=4,direction="vertical",action="workspace"}); hl.config({gestures={workspace_swipe_invert=false,workspace_swipe_distance=300}}); hl.plugin.hymission_tiles_test.refresh(); hl.dispatch(hl.dsp.focus({workspace=1}))')
    wait('j/activeworkspace',lambda s:s['id']==1,'gesture-origin'); time.sleep(.1)
    evaluate('hl.plugin.hymission_tiles_test.trackpad("begin"); hl.plugin.hymission_tiles_test.trackpad("update",40)')
    gesture=json.loads(request('j/hymission-stage-state'))['screens'][0]
    assert gesture['swipe_active'] and gesture['swipe_progress']>0,gesture
    observations.append({'vertical_stage_swipe_active':gesture['swipe_active'],'vertical_stage_swipe_progress':gesture['swipe_progress']})
    evaluate('hl.plugin.hymission_tiles_test.trackpad("cancel")')
    wait('j/hymission-stage-state',lambda s:not s['screens'][0]['swipe_active'],'gesture-cancelled')
    (root/'default-off-observations.json').write_text(json.dumps(observations,indent=2))
    # FB18 deliberately supersedes tiles-off content swapping. Keep this new
    # contract separate from the unchanged pre-tiles reference observations.
    def drag_first_to_last():
        state=wait('j/hymission-stage-state',lambda s:len(s['screens'][0]['cards'])>=2,'numeric-drag-ready')
        before={c['address']:c['workspace']['id'] for c in json.loads(request('j/clients'))}
        screen=state['screens'][0];source=screen['cards'][0];target=screen['cards'][-1]
        slots=[c['workspace'] for c in screen['cards'] if c['workspace']>0]
        pointer('press',source['x']+1,source['y']+1)
        pointer('move',target['x']+target['width']/2,target['y']+target['height']/2)
        pointer('release',target['x']+target['width']/2,target['y']+target['height']/2)
        time.sleep(.15)
        after={c['address']:c['workspace']['id'] for c in json.loads(request('j/clients'))}
        state=json.loads(request('j/hymission-stage-state'))
        ids=[c['workspace'] for c in state['screens'][0]['cards']]
        return before,after,ids,slots
    # stage_drag_reorder=0 keeps FB18: a drag never changes numbers or contents.
    evaluate('hl.config({animations={enabled=false},plugin={hymission={stage_transition_ms=0,stage_show_active=1,niri_mode=0,stage_drag_reorder=0}}})')
    before,after,ids,_=drag_first_to_last()
    assert before==after and ids==sorted(ids),('FB18 disabled numeric drag',before,after,ids)
    (root/'disabled-numeric-drag.json').write_text(json.dumps(dict(before=before,after=after,order=ids),indent=2))
    # Default drag reorder: the first card's workspace takes the last slot's
    # number and the others move up one slot. Windows keep their workspace
    # objects; only the ids change, and the cards stay in numeric order.
    evaluate('hl.config({plugin={hymission={stage_drag_reorder=1}}})')
    before,after,ids,slots=drag_first_to_last()
    renumber={slots[0]:slots[-1],**{slots[k]:slots[k-1] for k in range(1,len(slots))}}
    assert after=={a:renumber.get(ws,ws) for a,ws in before.items()} and ids==sorted(ids),('drag reorder',before,after,ids,slots)
    (root/'disabled-drag-reorder.json').write_text(json.dumps(dict(before=before,after=after,order=ids,slots=slots),indent=2))
    (root/'configerrors.txt').write_text(request('/configerrors'))
    print('PASS: tiles-disabled Stage dimensions, card/monitor-edge clicks, overview entry/exit and Stage restoration')


if __name__ == "__main__":
    import sys
    from tiles_isolated_smoke import main

    sys.argv.insert(1, "--default-off-regression")
    main()
