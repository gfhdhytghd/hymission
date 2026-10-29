"""Private focus-dependent native resize checks and every-rendered-frame evidence.

The optional fixture listener models a layout resizing a real client on focus.
It invokes native scrolling layout messages, never overview rebuild methods.
"""
import json
from pathlib import Path
import subprocess
import time
from PIL import Image


def run(mode, observe, request, root, env, runtime, processes, handles, expand=False, bisect=False):
    multi_monitor = bisect
    assert not bisect or mode in ('off', 'single'), 'cross-output bisect control requires off or single'
    def evaluate(code):
        result = request('/eval ' + code)
        assert result.strip() == 'ok', (code, result)

    def read(command):
        return json.loads(request(command))

    def wait(command, predicate):
        for _ in range(300):
            state = read(command)
            if predicate(state):
                return state
            time.sleep(.02)
        raise AssertionError((command, state))

    def settled():
        wait('j/hymission-overview-state', lambda s: s['phase'] == 'active' and not s.get('relayout_active', False))
        time.sleep(.25)
        return read('j/hymission-overview-state')

    def geometry(state):
        return {w['address']: w['contentGeometry'] for w in state['windows']}

    def changed(before, after):
        return [a for a, b in before.items() if a not in after or max(abs(b[k]-after[a][k]) for k in b) > .05]

    child = env.copy()
    child.pop('LD_PRELOAD', None)
    child['WAYLAND_DISPLAY'] = next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    scale = 1.18 if expand else 1
    evaluate('hl.config({input={follow_mouse=1},general={layout="scrolling"},scrolling={column_width=.32,fullscreen_on_one_column=false,focus_fit_method=1},'
             'animations={enabled=false},plugin={hymission={niri_mode=0,stage_enabled=0,overview_focus_follows_mouse=1,'
             f'selected_expand_scale={scale},hover_expand_scale={scale},layout_engine="natural",multi_workspace_sort_recent_first=0,workspace_change_keeps_overview=1,vim_keys=1,debug_logs=1,workspace_strip_force_show=1}}}}}})')
    if mode != 'off':
        evaluate('hl.config({plugin={hymission={tiles_enabled=1,tile_min_width=120}}}); hl.plugin.hymission_tiles_test.refresh()')
        wait('j/hymission-tiles-state', lambda s: s['enabled'])
    clients = {}
    workspaces = (901, 902, 903) if mode == 'three' else (901, 902)
    for ws in workspaces:
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        clients[ws] = []
        for n in range(3):
            count = len(read('j/clients'))
            log = open(root/f'client-{ws}-{n}.log', 'w'); handles.append(log)
            proc = subprocess.Popen(['weston-simple-shm'], env=child, stdout=log, stderr=subprocess.STDOUT)
            processes.append(proc)
            state = wait('j/clients', lambda cs: len(cs) == count+1)
            clients[ws].append(next(c['address'] for c in state if c['pid'] == proc.pid))
    evaluate('hl.dispatch(hl.dsp.focus({workspace=901}))')
    if mode in ('two', 'three'):
        evaluate('hl.plugin.hymission.tile_add({workspace="902"}); hl.plugin.hymission.tile_focus("left")')
    if mode == 'three':
        evaluate('hl.plugin.hymission.tile_add({workspace="903"}); hl.plugin.hymission.tile_focus("left"); hl.plugin.hymission.tile_focus("left")')
    if multi_monitor:
        assert request('/output create wayland FOCUS-SECOND').strip() == 'ok'
        wait('j/monitors', lambda ms: len(ms) == 2 and all(m['width'] > 0 for m in ms))
        evaluate('hl.workspace_rule({workspace="904",monitor="FOCUS-SECOND",persistent=true}); hl.dispatch(hl.dsp.focus({workspace=904}))')
        clients[904] = []
        for n in range(3):
            count = len(read('j/clients'))
            log = open(root/f'client-904-{n}.log', 'w'); handles.append(log)
            proc = subprocess.Popen(['weston-simple-shm'], env=child, stdout=log, stderr=subprocess.STDOUT)
            processes.append(proc)
            state = wait('j/clients', lambda cs: len(cs) == count+1)
            clients[904].append(next(c['address'] for c in state if c['pid'] == proc.pid))
        workspaces += (904,)
    observations = []
    # Force-all has identical membership even before tile-scoped overview was
    # introduced, so old bisect candidates retain a comparable scene.
    scopes = ('forceall',) if bisect else ('onlycurrentworkspace', 'forceall')
    for scope in scopes:
        inputs = ('monitor-hover',) if bisect else ('hover', 'keyboard')
        if not bisect and mode in ('two', 'three') and scope == 'onlycurrentworkspace':
            inputs += ('tile-keyboard',)
        for input_kind in inputs:
            control = None
            for resize in (False, True):
                # Identical opening snapshots and selection for paired trials.
                evaluate('hl.plugin.hymission_tiles_test.focus_resize(false)')
                for ws in reversed(workspaces):
                    evaluate(f'hl.plugin.hymission_tiles_test.window("reset_width","{clients[ws][0]}")')
                if mode in ('two', 'three'):
                    primary = next(m['name'] for m in read('j/monitors') if m['name'] != 'FOCUS-SECOND')
                    for _ in range(3):
                        tiles = [t for t in read('j/hymission-tiles-state')['tiles'] if t['monitor'] == primary]
                        if tiles[0]['focused']:
                            break
                        evaluate('hl.plugin.hymission.tile_focus("left")')
                    else:
                        raise AssertionError(('primary tile focus did not reset', tiles))
                evaluate(f'hl.plugin.hymission_tiles_test.window("focus","{clients[901][0]}"); hl.plugin.hymission_tiles_test.pointer("move",10,100)')
                time.sleep(.1)
                evaluate(f'hl.plugin.hymission.open("{scope}")')
                before = settled()
                native_before = read('j/clients')
                selected_before = read('j/activewindow')['address']
                prefix = root/f'{scope}-{input_kind}-resize{int(resize)}'
                prefix.with_suffix('.before.json').write_text(json.dumps(before, indent=2))
                presented = {}
                for monitor in read('j/monitors'):
                    # The private Weston parent may occlude the first child
                    # output. Capture the visible destination output only.
                    if multi_monitor and monitor['name'] != 'FOCUS-SECOND':
                        continue
                    path = str(prefix)+'-'+monitor['name']+'-before.png'
                    subprocess.run(['grim', '-o', monitor['name'], path], env=child, check=True, timeout=5)
                    presented[monitor['name']] = Image.open(path).convert('RGB')
                evaluate('hl.plugin.hymission_tiles_test.focus_resize(' + str(resize).lower() + ')')
                if input_kind in ('hover', 'monitor-hover'):
                    address = clients[904][1] if input_kind == 'monitor-hover' else clients[901][1]
                    b = next(w['contentGeometry'] for w in before['windows'] if w['address'] == address)
                    code = f'hl.plugin.hymission_tiles_test.pointer("move",{b["x"]+b["width"]/2},{b["y"]+b["height"]/2})'
                elif input_kind == 'tile-keyboard':
                    code = 'hl.plugin.hymission.tile_focus("right")'
                else:
                    code = 'hl.plugin.hymission_tiles_test.pointer("tab")'
                destination = ',"FOCUS-SECOND"' if multi_monitor else ''
                evaluate('hl.plugin.hymission_tiles_test.frame_capture("begin"' + destination + '); ' + code)
                time.sleep(.4)
                evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",' + json.dumps(str(prefix)) + ')')
                frames = json.loads(prefix.with_suffix('.json').read_text())
                assert frames, ('no rendered frames', prefix)
                moved_frames = []
                for i, frame in enumerate(frames):
                    image = Image.open(frame['pixels']).convert('RGB')
                    output_image = presented[frame['monitor']]
                    for rect in frame['framebuffer']['damage']:
                        output_image.paste(image.crop(tuple(rect)), tuple(rect[:2]))
                    output = str(prefix)+f'-{i:02}.png'
                    output_image.save(output)
                    Path(frame['pixels']).unlink()
                    frame['pixels'] = output
                    if changed(geometry(before), geometry(frame['overview'])):
                        moved_frames.append(i)
                prefix.with_suffix('.json').write_text(json.dumps(frames, indent=2))
                after = settled()
                prefix.with_suffix('.after.json').write_text(json.dumps(after, indent=2))
                native_after = {c['address']: c for c in read('j/clients')}
                resized = [c['address'] for c in native_before if c['size'] != native_after[c['address']]['size']]
                selected_after = read('j/activewindow')['address']
                if not resize:
                    control = geometry(after)
                result = dict(mode=mode, scope=scope, input=input_kind, expand=expand, resize=resize, resized=len(resized),
                              selection_changed=selected_before != selected_after, frames=len(frames), moved_frames=moved_frames,
                              moved_windows=len(changed(geometry(before), geometry(after))),
                              differs_from_control=len(changed(control, geometry(after))))
                observations.append(result)
                (root/'observations.json').write_text(json.dumps(observations, indent=2))
                assert result['selection_changed'], ('input did not change real focus', result)
                if resize:
                    assert resized, ('native focus resize did not occur', result)
                evaluate('hl.plugin.hymission_tiles_test.focus_resize(false); hl.plugin.hymission.close()')
                wait('j/hymission-overview-state', lambda s: not s['active'])
        if bisect:
            continue
        # Structural controls concern core slots, not the selected-window ripple
        # pose. Keep expansion exercised by the paired focus trials above.
        evaluate('hl.config({plugin={hymission={selected_expand_scale=1,hover_expand_scale=1}}}); hl.plugin.hymission_tiles_test.refresh()')
        evaluate(f'hl.plugin.hymission.open("{scope}")')
        before = settled()
        count = len(read('j/clients'))
        log = open(root/f'structural-{scope}.log', 'w'); handles.append(log)
        proc = subprocess.Popen(['weston-simple-shm'], env=child, stdout=log, stderr=subprocess.STDOUT)
        processes.append(proc)
        wait('j/clients', lambda cs: len(cs) == count+1)
        wait('j/hymission-overview-state', lambda s: len(s['windows']) == len(before['windows'])+1)
        opened = settled()
        assert changed(geometry(before), geometry(opened)), 'new window did not reflow layout'
        proc.terminate(); proc.wait(timeout=5)
        wait('j/hymission-overview-state', lambda s: len(s['windows']) == len(before['windows']))
        settled()
        if mode in ('two', 'three') and not multi_monitor:
            before = settled()
            evaluate('hl.plugin.hymission.tile_resize({delta="+80px"})')
            after = settled()
            if scope == 'onlycurrentworkspace':
                assert changed(geometry(before), geometry(after)), 'tile bounds did not reflow workspace regions'
            else:
                # All overview remains one output-wide region (FB2). A real
                # topology change may reflow the solver, but must neither lose
                # windows nor turn the region into per-tile clusters.
                assert after['window_layout_scope'] == 'all' and not after['tile_groups']
                assert set(geometry(before)) == set(geometry(after))
                monitor = read('j/monitors')[0]
                for rect in geometry(after).values():
                    assert rect['width'] > 0 and rect['height'] > 0
                    assert monitor['x']-1 <= rect['x'] and rect['x']+rect['width'] <= monitor['x']+monitor['width']/monitor['scale']+1
                    assert monitor['y']-1 <= rect['y'] and rect['y']+rect['height'] <= monitor['y']+monitor['height']/monitor['scale']+1
        evaluate('hl.plugin.hymission.close()')
        wait('j/hymission-overview-state', lambda s: not s['active'])
        evaluate('hl.config({plugin={hymission={selected_expand_scale='+str(scale)+',hover_expand_scale='+str(scale)+'}}}); hl.plugin.hymission_tiles_test.refresh()')
    (root/'configerrors.txt').write_text(request('/configerrors'))
    assert not (root/'configerrors.txt').read_text().strip()
    if not observe:
        assert all(not r['differs_from_control'] if expand else not r['moved_frames'] for r in observations), observations
    print(('OBSERVED' if observe else 'PASS') + ': focus/resize overview invariance ' + json.dumps(observations))
