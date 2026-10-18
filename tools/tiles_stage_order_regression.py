"""FB17: committed Stage slots are the fixed flight goals, including saved drag order."""
import json
from pathlib import Path

KEYS=('x','y','width','height')
def error(a,b):return max(abs(a[k]-b[k]) for k in KEYS)

def prepare(case,recent,evaluate,read,wait,pointer,root):
    """Use the public sidebar drag route; never inject private ordering state."""
    import time
    evaluate(f'hl.config({{plugin={{hymission={{multi_workspace_sort_recent_first={recent}}}}}}})')
    if case=='off':return
    # Build an extra column even for single mode, and retain its real drag order
    # when removing it. This is the user's multi-tile -> single-tile history.
    if case=='single':evaluate('hl.plugin.hymission.tile_add({workspace="301"})')
    wait('j/hymission-stage-state',lambda s:len(s['screens'])>=2)
    def move(ws,column,bottom):
        state=read('j/hymission-stage-state')
        source,card=next((s,c) for s in state['screens'] for c in s['cards'] if c['workspace']==ws)
        band=state['screens'][column]['sidebar']
        x,y=card['x']+.5,card['y']+.5
        dx=band['x']+band['width']/2;dy=band['y']+(band['height']-3 if bottom else 3)
        pointer('press',x,y);pointer('move',dx,dy);pointer('release',dx,dy)
        wait('j/hymission-stage-state',lambda s:any(c['workspace']==ws for c in s['screens'][column]['cards']))
        time.sleep(.75)
    move(100,1,False);move(100,0,True)
    if case=='single':
        evaluate('hl.plugin.hymission.tile_focus("right"); hl.plugin.hymission.tile_remove(); hl.dispatch(hl.dsp.focus({workspace=101}))')
        wait('j/hymission-stage-state',lambda s:len(s['screens'])==1)
        time.sleep(.75)
    state=read('j/hymission-stage-state')
    order=[c['workspace'] for c in state['screens'][0]['cards']]
    assert order.index(102)<order.index(100),('drag order was not retained',order)
    (root/'saved-order.json').write_text(json.dumps(state,indent=2))

def verify(root,observe=False):
    checks=[]
    for result in json.loads((root/'results.json').read_text()):
        tile,route,start,end=(result[k] for k in ('tile','route','start','end'))
        name=f'tile{tile}-{route}-{start}-{end}'
        frames=json.loads((root/(name+'.json')).read_text())
        initial=json.loads((root/(name+'-before.json')).read_text())['screens'][tile]
        final=frames[-1]['stage']['screens'][tile]
        assert not final['flights'] and not final['swipe_active'],name
        # Flight metadata does not contain addresses: the fixture supplies one
        # real marker per workspace, so workspace is an independent identity.
        source=next(c for c in initial['cards'] if c['workspace']==end)['windows'][0]
        goal=next(c for c in final['cards'] if c['workspace']==start)['windows'][0]
        source_errors=[];goal_errors=[];passive={};held=0
        for frame in frames:
            screen=frame['stage']['screens'][tile]
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
