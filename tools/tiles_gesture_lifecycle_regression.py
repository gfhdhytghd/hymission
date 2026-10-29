"""Exercise the real device gesture end path, including native fallbacks."""
import json
import time


def run(request,root):
    def evaluate(code):
        result=request('/eval '+code)
        assert result.strip()=='ok',(code,result)
    def wait(command,predicate):
        for _ in range(150):
            state=json.loads(request(command))
            if predicate(state):return state
            time.sleep(.02)
        raise AssertionError((command,state))
    evaluate('hl.config({animations={enabled=true},plugin={hymission={stage_enabled=0,tiles_enabled=0}},gestures={workspace_swipe_invert=false,workspace_swipe_distance=300,workspace_swipe_cancel_ratio=.5,workspace_swipe_min_speed_to_force=0,workspace_swipe_forever=false}}); hl.gesture({fingers=4,direction="vertical",action="workspace"}); hl.animation({leaf="workspaces",enabled=true,speed=8,bezier="default",style="slidevert"})')
    for ws in (901,902,903):evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}})')
    samples=[]
    def gesture(label,delta=200,finish='end',horizontal=False):
        evaluate('hl.plugin.hymission_tiles_test.trackpad("begin")')
        for _ in range(5):
            evaluate(f'hl.plugin.hymission_tiles_test.trackpad("update",{0 if horizontal else delta/5},{delta/5 if horizontal else 0})')
            time.sleep(.01)
        evaluate(f'hl.plugin.hymission_tiles_test.trackpad("{finish}")')
        time.sleep(.35)
        samples.append({'case':label,'workspace':json.loads(request('j/activeworkspace'))['id'],
                        'tiles':json.loads(request('j/hymission-tiles-state'))})
        (root/'gesture-lifecycle.json').write_text(json.dumps(samples,indent=2))
        # A second IPC round trip detects a signal raised after the callback.
        assert json.loads(request('j/monitors'))
    for enabled in (0,1):
        evaluate(f'hl.config({{plugin={{hymission={{tiles_enabled={enabled}}}}}}}); hl.plugin.hymission_tiles_test.refresh(); hl.dispatch(hl.dsp.focus({{workspace=901}}))')
        wait('j/hymission-tiles-state',lambda s:s['enabled']==bool(enabled))
        gesture(f'single-enabled-{enabled}')
        assert samples[-1]['workspace']==902,samples[-1]
        gesture(f'single-reverse-{enabled}',-200)
        assert samples[-1]['workspace']==901,samples[-1]
        gesture(f'single-cancel-{enabled}',30,'cancel')
        assert samples[-1]['workspace']==901,samples[-1]
        gesture(f'single-no-movement-{enabled}',0)
    evaluate('hl.plugin.hymission.tile_add({workspace="903"}); hl.plugin.hymission.tile_focus("left")')
    wait('j/hymission-tiles-state',lambda s:len(s['tiles'])==2)
    gesture('multi-tile')
    assert samples[-1]['workspace']==902,samples[-1]
    evaluate('hl.plugin.hymission.tile_remove()')
    wait('j/hymission-tiles-state',lambda s:len(s['tiles'])==1)
    gesture('collapsed-native',-200)
    evaluate('hl.config({plugin={hymission={tiles_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
    wait('j/hymission-tiles-state',lambda s:not s['enabled'])
    gesture('disabled-after-collapse')
    evaluate('hl.gesture({fingers=4,direction="horizontal",action="scroll_move"})')
    for layout in ('dwindle','scrolling'):
        evaluate('hl.config({general={layout="'+layout+'"}})')
        gesture('native-scroll-'+layout,50,horizontal=True)
        gesture('native-scroll-cancel-'+layout,-50,'cancel',horizontal=True)
    print('PASS: real four-finger native end path, disabled/single/multi/collapsed/disabled-again, reverse/cancel/no-op; native scroll conditional branch in dwindle and scrolling')
