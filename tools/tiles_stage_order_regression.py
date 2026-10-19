"""FB17/FB18: all Stage scenes and drag returns use the committed numeric slots."""
import json
from pathlib import Path

KEYS=('x','y','width','height')
def error(a,b):return max(abs(a[k]-b[k]) for k in KEYS)

def numeric_order(state):
    for screen in state['screens']:
        cards=screen['cards']
        numbered=[c['workspace'] for c in cards if c['workspace']>0]
        named=[c['name'] for c in cards if c['workspace']<=0]
        assert numbered==sorted(numbered),('non-numeric Stage order',cards)
        assert named==sorted(named),('named Stage order',cards)
        assert all(c['workspace']>0 for c in cards[:len(numbered)]),cards


def prepare(case,recent,evaluate,read,wait,pointer,root):
    """Try conflicting drop heights through public pointer input, then collapse."""
    import time
    from PIL import Image
    evaluate(f'hl.config({{plugin={{hymission={{multi_workspace_sort_recent_first={recent}}}}}}})')
    if case=='single':evaluate('hl.plugin.hymission.tile_add({workspace="301"})')
    wait('j/hymission-stage-state',lambda s:len(s['screens'])>=(1 if case=='off' else 2))
    results=[]
    def move(ws,column,bottom,name,desktop=False):
        state=read('j/hymission-stage-state');numeric_order(state)
        source,card=next((s,c) for s in state['screens'] for c in s['cards'] if c['workspace']==ws)
        band=state['screens'][column]['sidebar']
        x,y=card['x']+.5,card['y']+.5
        dx=band['x']+band['width']/2;dy=band['y']+(band['height']-3 if bottom else 3)
        before={c['address']:c['workspace']['id'] for c in read('j/clients')}
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin")')
        pointer('press',x,y);pointer('move',dx,dy);time.sleep(.1);pointer('release',dx,dy)
        wait('j/hymission-stage-state',lambda s:any(c['workspace']==ws for c in s['screens'][column]['cards']))
        time.sleep(.9)
        prefix=str(root/name)
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",'+json.dumps(prefix)+')')
        frames=json.loads(Path(prefix+'.json').read_text())
        final=frames[-1]['stage'];numeric_order(final)
        target=next(c for s in final['screens'] for c in s['cards'] if c['workspace']==ws)
        assert before=={c['address']:c['workspace']['id'] for c in read('j/clients')},name
        errors=[];moving=0;desktop_ratios=[]
        for f in frames:
            numeric_order(f['stage'])
            for screen in f['stage']['screens']:
                numeric_order(dict(screens=[dict(cards=screen.get('presentation_cards',screen['cards']))]))
                for flight in screen['flights']:
                    if flight['workspace']==ws:
                        assert len(target['windows'])==1,target
                        errors.append(error(flight['to'],target['windows'][0]))
                        moving+=.01<screen['transition_progress']<.99
            if desktop:
                # A returning active-card ghost must not take over the real
                # desktop. The marker's green body is outside sidebar/ghost.
                pane=next(sc for sc in f['stage']['screens'] if sc['active_workspace']==ws)
                native=next(n for n in f['native'] if n['workspace']==str(ws))
                area=pane['desktop_geometry'];x,y=native['position'];w,h=native['size']
                roi=(int(max(x,area['x'])+8),int(max(y+64,area['y'])+8),int(min(x+w,area['x']+area['width'])-8),int(min(y+h,area['y']+area['height'])-8))
                assert roi[2]>roi[0]+5 and roi[3]>roi[1]+5,(name,roi,native,area)
                with Image.open(f['pixels']) as image:
                    pixels=list(image.convert('RGB').crop(roi).get_flattened_data())
                ratio=sum(g>180 and r<80 and b<80 for r,g,b in pixels)/len(pixels)
                desktop_ratios.append(ratio)
                assert ratio>.7,(name,'active desktop disappeared during ghost return',ratio,roi)
            path=Path(f['pixels']);png=path.with_suffix('.png')
            with Image.open(path) as im:im.save(png)
            path.unlink();f['pixels']=str(png)
        Path(prefix+'.json').write_text(json.dumps(frames,indent=2))
        assert errors and max(errors)<=1 and moving>=2,(name,errors,moving)
        old_cards={c['workspace']:c for sc in state['screens'] for c in sc['cards'] if 'y' in c}
        passive=[]
        for sc in final['screens']:
            for c in sc['cards']:
                key=c['workspace'];old=old_cards.get(key)
                if key==ws or not old or 'y' not in c or abs(c['y']-old['y'])<=1:continue
                ys=[v['y'] for frame in frames for pane in frame['stage']['screens'] for v in pane['cards'] if v['workspace']==key and 'y' in v]
                between=sum(min(old['y'],c['y'])+1<y<max(old['y'],c['y'])-1 for y in ys)
                direction=1 if c['y']>old['y'] else -1
                backwards=max((-(v-u)*direction for u,v in zip(ys,ys[1:])),default=0)
                assert between>=2 and backwards<=1 and abs(ys[-1]-c['y'])<=1,(name,key,between,backwards)
                passive.append(dict(workspace=key,intermediate=between,backwards_jump=max(0,backwards),settled_error=abs(ys[-1]-c['y'])))
        row=dict(name=name,frames=len(frames),moving_frames=moving,endpoint_error=max(errors),order=[c['workspace'] for c in final['screens'][column]['cards']],passive=passive,desktop_green_min=min(desktop_ratios) if desktop_ratios else None)
        results.append(row);(root/'drop-order-results.json').write_text(json.dumps(results,indent=2))
    evaluate('hl.config({plugin={hymission={stage_show_active=1}}})')
    wait('j/hymission-stage-state',lambda s:any(c['workspace']==101 for sc in s['screens'] for c in sc['cards']))
    time.sleep(.75)
    move(101,0,True,'active-card-return',desktop=True)
    # Hand the same active-card return to all-overview before it settles.
    evaluate('hl.config({plugin={hymission={overview_focus_follows_mouse=0,hover_expand_scale=1}}})')
    time.sleep(.1)
    state=read('j/hymission-stage-state');pane=state['screens'][0]
    c=next(c for c in pane['cards'] if c['workspace']==101);band=pane['sidebar']
    pointer('press',c['x']+.5,c['y']+.5)
    pointer('move',band['x']+band['width']/2,band['y']+band['height']-3)
    pointer('release',band['x']+band['width']/2,band['y']+band['height']-3)
    time.sleep(.05)
    held=read('j/hymission-stage-state')
    assert any(0<sc['transition_progress']<1 and any(f['workspace']==101 for f in sc['flights']) for sc in held['screens']),held
    native=next(c for c in read('j/clients') if c['workspace']['id']==101)
    expected=dict(x=native['at'][0],y=native['at'][1],width=native['size'][0],height=native['size'][1])
    evaluate('hl.plugin.hymission.open("forceall")')
    overview=wait('j/hymission-overview-state',lambda s:s['active'] and any(w['address']==native['address'] and 'animationFromGeometry' in w for w in s['windows']))
    origin=next(w['animationFromGeometry'] for w in overview['windows'] if w['address']==native['address'])
    assert error(origin,expected)<=1,('active-card overview used the ghost instead of the desktop',origin,expected)
    wait('j/hymission-overview-state',lambda s:s['phase']=='active')
    owner=read('j/hymission-stage-state')
    assert all(sc['overview_render_owner']=='overview' for sc in owner['screens']),owner
    (root/'active-return-overview.json').write_text(json.dumps(dict(origin=origin,native=expected,error=error(origin,expected),held=held,owner=owner),indent=2))
    evaluate('hl.plugin.hymission.close()')
    wait('j/hymission-overview-state',lambda s:not s['active'])
    evaluate('hl.dispatch(hl.dsp.focus({workspace=101}))')
    time.sleep(.75)
    evaluate('hl.config({plugin={hymission={stage_show_active=0}}})')
    wait('j/hymission-stage-state',lambda s:all(c['workspace']!=sc['active_workspace'] for sc in s['screens'] for c in sc['cards']))
    time.sleep(.75)
    move(100,0,True,'same-column-bottom')
    if case!='off':
        move(100,1,False,'cross-column-top');move(100,0,True,'cross-column-bottom')
    if case=='single':
        evaluate('hl.plugin.hymission.tile_focus("right"); hl.plugin.hymission.tile_remove(); hl.dispatch(hl.dsp.focus({workspace=101}))')
        wait('j/hymission-stage-state',lambda s:len(s['screens'])==1)
        time.sleep(.75)
    state=read('j/hymission-stage-state');numeric_order(state)
    assert [c['workspace'] for c in state['screens'][0]['cards']][:2]==[100,102],state
    (root/'numeric-order.json').write_text(json.dumps(state,indent=2))

def verify(root,observe=False):
    checks=[]
    for result in json.loads((root/'results.json').read_text()):
        tile,route,start,end=(result[k] for k in ('tile','route','start','end'))
        name=f'tile{tile}-{route}-{start}-{end}'
        frames=json.loads((root/(name+'.json')).read_text())
        initial=json.loads((root/(name+'-before.json')).read_text())['screens'][tile]
        numeric_order(dict(screens=[initial]))
        final=frames[-1]['stage']['screens'][tile]
        assert not final['flights'] and not final['swipe_active'],name
        # Flight metadata does not contain addresses: the fixture supplies one
        # real marker per workspace, so workspace is an independent identity.
        source=next(c for c in initial['cards'] if c['workspace']==end)['windows'][0]
        goal=next(c for c in final['cards'] if c['workspace']==start)['windows'][0]
        source_errors=[];goal_errors=[];passive={};held=0
        for frame in frames:
            numeric_order(frame['stage'])
            screen=frame['stage']['screens'][tile]
            numeric_order(dict(screens=[dict(cards=screen.get('presentation_cards',screen['cards']))]))
            if screen['swipe_active'] and .05<screen['transition_progress']<.95:held+=1
            for flight in screen['flights']:
                if flight['workspace']==start:goal_errors.append(error(flight['to'],goal))
                # A released gesture legitimately rebases from the held frame.
                if flight['workspace']==end and not source_errors:source_errors.append(error(flight['from'],source))
            for card in screen.get('presentation_cards',screen['cards']):
                ws=card['workspace']
                if ws not in (start,end) and 'y' in card:passive.setdefault(ws,[]).append(card['y'])
        passive_checks=[]
        for ws,ys in passive.items():
            a=next(c for c in initial['cards'] if c['workspace']==ws)
            b=next(c for c in final['cards'] if c['workspace']==ws)
            if abs(a['y']-b['y'])>1:
                between=[y for y in ys if min(a['y'],b['y'])+1<y<max(a['y'],b['y'])-1]
                direction=1 if b['y']>a['y'] else -1
                backwards=max((-(v-u)*direction for u,v in zip(ys,ys[1:])),default=0)
                passive_checks.append(dict(workspace=ws,intermediate=len(between),settled_error=abs(ys[-1]-b['y']),backwards_jump=max(0,backwards)))
                assert backwards<=1,(name,'passive card replayed its old slot',ws,backwards)
                assert len(between)>=2,(name,'passive card teleported',ws,ys)
                assert abs(ys[-1]-b['y'])<=1,(name,ws,ys[-1],b['y'])
        row=dict(name=name,frames=len(frames),held_frames=held,source_error=max(source_errors,default=999),final_slot_error=max(goal_errors,default=999),passive=passive_checks)
        checks.append(row);(root/'order-results.json').write_text(json.dumps(checks,indent=2))
        if not observe:
            assert row['source_error']<=1 and row['final_slot_error']<=1,row
            assert route!='swipe' or held>=2,row
    print(('OBSERVED' if observe else 'PASS')+': Stage source and final slots <=1 px; passive cards move continuously')


def finish(evaluate,read,wait,root):
    """Named workspaces follow numbered ones; special workspaces stay outside."""
    evaluate('hl.dispatch(hl.dsp.focus({workspace=101}))')
    for name in ['fb18-middle','fb18-alpha','fb18-zeta']:
        evaluate('hl.workspace_rule({workspace="name:'+name+'",persistent=true}); hl.dispatch(hl.dsp.focus({workspace="name:'+name+'"}))')
    evaluate('hl.dispatch(hl.dsp.focus({workspace=101}))')
    state=wait('j/hymission-stage-state',lambda s:all(any(c['name']==n for sc in s['screens'] for c in sc['cards']) for n in ['fb18-alpha','fb18-middle','fb18-zeta']))
    numeric_order(state)
    evaluate('hl.dispatch(hl.dsp.workspace.toggle_special("fb18-order"))')
    special=wait('j/hymission-stage-state',lambda s:s['screens'] and all(sc['suspended'] for sc in s['screens']))
    assert not any(c['name']=='special:fb18-order' or c['name']=='fb18-order' for sc in special['screens'] for c in sc['cards']),special
    evaluate('hl.dispatch(hl.dsp.workspace.toggle_special("fb18-order"))')
    (root/'named-special-order.json').write_text(json.dumps(dict(named=state,special=special),indent=2))
