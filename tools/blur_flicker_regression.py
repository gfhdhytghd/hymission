"""Closed-overview, partial-damage pixel oracle for the private nested harness.

Run tiles_isolated_smoke.py --blur-flicker-regression CASE with its matching
--hyprland, --plugin and --fixture. CASE reference omits Hymission entirely.
The fixture reads completed output buffers without requesting full damage.
Identical client buffers with disjoint small damage plus pointer motion must
match full-damage frames of the same scene. Artifacts include every frame and
its actual damage, so a full-repaint-only test cannot silently pass.
"""
import json
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image


def run(case, request, root, env, runtime, processes, handles, opacity='client'):
    def evaluate(code):
        reply = request('/eval ' + code)
        assert reply.strip() == 'ok', (code, reply)

    def read(command):
        return json.loads(request(command))

    def wait(command, predicate):
        for _ in range(250):
            state = read(command)
            if predicate(state):
                return state
            time.sleep(.02)
        raise AssertionError((command, state))

    child = env.copy()
    child.pop('LD_PRELOAD', None)
    child['WAYLAND_DISPLAY'] = next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
    xml = Path(subprocess.check_output(['pkg-config', '--variable=pkgdatadir', 'wayland-protocols'], text=True).strip()) / 'stable/xdg-shell/xdg-shell.xml'
    for kind, name in [('client-header', 'xdg-shell-client.h'), ('private-code', 'xdg-shell-protocol.c')]:
        subprocess.run(['wayland-scanner', kind, str(xml), str(root / name)], check=True)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'wayland-client'], text=True).split()
    client = root / 'client'
    subprocess.run(['cc', str(Path(__file__).with_name('tiles_opacity_client.c')), str(root / 'xdg-shell-protocol.c'), '-I' + str(root), *flags, '-o', str(client)], check=True)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gtk4', 'gtk4-layer-shell-0'], text=True).split()
    background = root / 'background'
    subprocess.run(['cc', str(Path(__file__).with_name('tiles_opacity_background.c')), *flags, '-o', str(background)], check=True)

    def spawn(binary, name, **variables):
        log = open(root / (name + '.log'), 'w')
        handles.append(log)
        current = child.copy()
        current.update(FB20_ID=name, **variables)
        processes.append(subprocess.Popen([str(binary)], env=current, stdout=log, stderr=subprocess.STDOUT))

    evaluate('hl.monitor({output="",mode="1280x720@60",position="auto",scale="1"}); hl.config({general={layout="scrolling",gaps_in=10,gaps_out=20},scrolling={column_width=.55,fullscreen_on_one_column=false,focus_fit_method=1},input={follow_mouse=0},animations={enabled=false},decoration={rounding=0,shadow={enabled=false},blur={enabled=true,size=8,passes=2,new_optimizations=true,xray=false},active_opacity=1.0,inactive_opacity=1.0},cursor={no_hardware_cursors=1}})')
    if case != 'reference':
        evaluate('hl.config({plugin={hymission={stage_enabled=0,tiles_enabled=0,stage_transition_ms=0,workspace_strip_refresh_ms=100}}})')
    rule_opacity = opacity == 'rule'
    if rule_opacity:
        evaluate('hl.window_rule({name="blur-flicker-opacity",match={class="^blur-flicker"},opacity="0.8 override 0.8 override"})')
    spawn(background, 'background')
    wait('j/layers', lambda s: 'fb20-background' in json.dumps(s))
    evaluate('hl.dispatch(hl.dsp.focus({workspace=2}))')
    spawn(client, 'other-workspace')
    wait('j/clients', lambda s: len(s) == 1)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1}))')
    for i in range(3):
        spawn(client, 'blur-flicker' + str(i), FB20_ALPHA='255' if rule_opacity else '128', FB20_SMALL_DAMAGE='1', FB20_PATTERN='1')
        wait('j/clients', lambda s: len(s) == i + 2)
    target = next(c for c in read('j/clients') if c['class'] == 'blur-flicker1')
    evaluate('hl.dispatch(hl.dsp.focus({window="address:' + target['address'] + '"}))')
    if case in ('stage', 'stage-tiles'):
        evaluate('hl.config({plugin={hymission={stage_enabled=1}}}); hl.plugin.hymission.stage_open()')
        wait('j/hymission-stage-state', lambda s: s['visibility_progress'] == 1)
    if case in ('tiles', 'stage-tiles'):
        evaluate('hl.config({plugin={hymission={tiles_enabled=1}}}); hl.workspace_rule({workspace="2",persistent=true}); hl.plugin.hymission.tile_add({workspace="2"})')
        wait('j/hymission-tiles-state', lambda s: len(s['tiles']) == 2)
    if case not in ('reference', 'never'):
        evaluate('hl.plugin.hymission.open("onlycurrentworkspace")')
        wait('j/hymission-overview-state', lambda s: s['phase'] == 'active' and not s['relayout_active'])
        evaluate('hl.plugin.hymission.close()')
        wait('j/hymission-overview-state', lambda s: not s['active'])
    time.sleep(.5)
    monitors = read('j/monitors')
    mw, mh = monitors[0]['width'], monitors[0]['height']
    def visible_area(c):
        x, y = c['at']; w, h = c['size']
        return max(0, min(mw, x + w) - max(0, x)) * max(0, min(mh, y + h) - max(0, y))
    window = max((c for c in read('j/clients') if c['class'].startswith('blur-flicker')), key=visible_area)
    (root / 'scene.json').write_text(json.dumps(dict(case=case, rule_opacity=rule_opacity, window=window, monitors=monitors), indent=2))
    x, y = window['at']
    w, h = window['size']
    pointer_y = int(y + h / 2)
    pointer_left, pointer_right = max(0, x) + 40, min(mw, x + w) - 40
    evaluate(f'hl.plugin.hymission_tiles_test.pointer("move",{int((pointer_left+pointer_right)/2)},{pointer_y})')

    def capture(label, partial=False):
        action = 'begin_partial' if partial else 'begin'
        evaluate(f'hl.plugin.hymission_tiles_test.frame_capture("{action}","",48,true)')
        if partial:
            for i in range(60):
                px = int(pointer_left + (i * 31) % max(1, int(pointer_right - pointer_left)))
                evaluate(f'hl.plugin.hymission_tiles_test.pointer("move",{px},{pointer_y})')
                time.sleep(.02)
        else:
            time.sleep(.15)
        evaluate('hl.plugin.hymission_tiles_test.frame_capture("end",' + json.dumps(str(root / label)) + ')')
        frames = json.loads((root / (label + '.json')).read_text())
        assert frames, ('no captured frames', label)
        for frame in frames:
            path = Path(frame['pixels'])
            png = path.with_suffix('.png')
            Image.open(path).save(png)
            path.unlink()
            frame['pixels'] = str(png)
        (root / (label + '.json')).write_text(json.dumps(frames, indent=2))
        return frames

    full = capture('full-before')
    partial = capture('partial', True)
    after = capture('full-after')
    assert all(f['overview'] is None or not f['overview']['active'] for f in partial), 'overview must remain closed'
    reference = np.array(Image.open(full[-1]['pixels']).convert('RGB')).astype(int)
    mask = np.ones(reference.shape[:2], dtype=bool)
    # Compare the whole composed scene, including adjacent columns and viewport
    # edges. Only the moving software cursor band is excluded.
    mask[max(0, pointer_y - 48):pointer_y + 48, :] = False
    assert mask.sum() > 10000, ('insufficient visible content', window)
    observations = []
    for frame in partial + after:
        pixels = np.array(Image.open(frame['pixels']).convert('RGB')).astype(int)
        diff = np.max(np.abs(pixels - reference), axis=2)
        area = sum((r[2] - r[0]) * (r[3] - r[1]) for r in frame['framebuffer']['damage'])
        observations.append(dict(pixels=frame['pixels'], changed=int((diff[mask] > 1).sum()), max_error=int(diff[mask].max()), damage_area=area))
    partial_frames = sum(o['damage_area'] < mask.size * .8 for o in observations[:len(partial)])
    report = dict(case=case, sampled_pixels=int(mask.sum()), frames=len(partial), partial_frames=partial_frames, observations=observations)
    (root / 'pixel-checks.json').write_text(json.dumps(report, indent=2))
    (root / 'configerrors.txt').write_text(request('/configerrors'))
    assert not (root / 'configerrors.txt').read_text().strip(), root / 'configerrors.txt'
    assert len(partial) >= 20 and partial_frames >= 10, ('partial damage coverage missing', report)
    assert all(o['changed'] == 0 for o in observations), ('partial/full render mismatch', root / 'pixel-checks.json', max(o['changed'] for o in observations))
    print(f'PASS blur-flicker {case}: {len(partial)} consecutive frames, {partial_frames} partial, {mask.sum()} pixels/frame')
