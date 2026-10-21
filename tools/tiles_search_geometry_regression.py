"""Private search/strip geometry across disabled, single and split outputs."""
import json
from pathlib import Path
import subprocess
import time


def run(request, root, env, runtime, processes, handles):
    def evaluate(code):
        result = request('/eval ' + code)
        assert result.strip() == 'ok', (code, result)

    def read(command):
        return json.loads(request(command))

    def wait(command, predicate):
        for _ in range(200):
            value = read(command)
            if predicate(value):
                return value
            time.sleep(.025)
        (root / 'failure.json').write_text(json.dumps(value, indent=2))
        raise AssertionError((command, value))

    child = env.copy()
    child.pop('LD_PRELOAD', None)
    displays = [p.name for p in runtime.glob('wayland-*') if p.is_socket()]
    assert len(displays) == 1
    child['WAYLAND_DISPLAY'] = displays[0]
    xml = Path(subprocess.check_output(['pkg-config', '--variable=pkgdatadir', 'wayland-protocols'], text=True).strip()) / 'stable/xdg-shell/xdg-shell.xml'
    for kind, name in [('client-header', 'xdg-shell-client.h'), ('private-code', 'xdg-shell-protocol.c')]:
        subprocess.run(['wayland-scanner', kind, str(xml), str(root / name)], check=True)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'wayland-client'], text=True).split()
    marker = root / 'marker'
    subprocess.run(['cc', str(Path(__file__).with_name('tiles_top_marker_client.c')), str(root / 'xdg-shell-protocol.c'), '-I' + str(root), *flags, '-o', str(marker)], check=True)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gtk4', 'gtk4-layer-shell-0'], text=True).split()
    bar = root / 'exclusive-bar'
    subprocess.run(['cc', str(Path(__file__).with_name('search_reserved_test_bar.c')), *flags, '-o', str(bar)], check=True)

    def spawn(binary, name):
        log = open(root / (name + '.log'), 'w')
        handles.append(log)
        process = subprocess.Popen([str(binary)], env=child, stdout=log, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def overview():
        return wait('j/hymission-overview-state', lambda s: s['phase'] == 'active' and not s['relayout_active'])

    def search_layer():
        return [layer for monitor in read('j/layers').values() for level in monitor['levels'].values()
                for layer in level if layer['namespace'] == 'hymission-search']

    def capture(name):
        subprocess.run(['grim', '-o', read('j/monitors')[0]['name'], str(root / (name + '.png'))], env=child, check=True, capture_output=True)
        (root / (name + '.json')).write_text(json.dumps(dict(overview=overview(), tiles=read('j/hymission-tiles-state'), monitors=read('j/monitors'), layers=read('j/layers')), indent=2))

    def intersects(a, b):
        return min(a['x']+a['width'], b['x']+b['width']) > max(a['x'], b['x']) + .5 and min(a['y']+a['height'], b['y']+b['height']) > max(a['y'], b['y']) + .5

    evaluate('hl.config({animations={enabled=false},plugin={hymission={tiles_enabled=1,tile_min_width=120,pick_labels_enabled=1,vim_keys=0,workspace_strip_empty_mode="always",stage_enabled=0}}}); hl.plugin.hymission_tiles_test.refresh()')
    for ws in (1, 2, 3):
        evaluate(f'hl.workspace_rule({{workspace="{ws}",persistent=true}}); hl.dispatch(hl.dsp.focus({{workspace={ws}}}))')
        spawn(marker, 'marker-' + str(ws))
        wait('j/clients', lambda cs: len(cs) == ws)
    evaluate('hl.dispatch(hl.dsp.focus({workspace=1}))')
    checks = []
    for count in (0, 1, 2, 3):
        evaluate('hl.config({plugin={hymission={tiles_enabled=' + str(int(count > 0)) + '}}}); hl.plugin.hymission_tiles_test.refresh()')
        if count > 1:
            evaluate(f'hl.plugin.hymission.tile_add({{workspace="{count}"}})')
        if count:
            wait('j/hymission-tiles-state', lambda s: len(s['tiles']) == count)
        for reserved in (False, True):
            bar_process = None
            if reserved:
                # Exercise global/local conversions on a nonzero output origin.
                evaluate('hl.monitor({output="",mode="1920x1080@60",position="1920x300",scale="1"}); hl.plugin.hymission_tiles_test.refresh()')
                wait('j/monitors', lambda ms: ms[0]['x'] == 1920 and ms[0]['y'] == 300)
                bar_process = spawn(bar, f'bar-{count}')
                wait('j/monitors', lambda ms: ms[0]['reserved'][1] == 48)
            for strip in (False, True):
                for scope in ('forceall', 'onlycurrentworkspace'):
                    # Workspace scope enables its strip by design; suppressing
                    # force_show only disables the all-overview strip.
                    actual_strip = strip or scope == 'onlycurrentworkspace'
                    name = f'tiles-{count}-bar-{int(reserved)}-strip-{int(strip)}-{scope}'
                    evaluate('hl.config({plugin={hymission={workspace_strip_force_show=' + str(int(strip)) + ',workspace_strip_anchor="top"}}}); hl.plugin.hymission.open("' + scope + '")')
                    before = overview()
                    assert 'visualProgress' in before and 'relayout_active' in before
                    assert not search_layer()
                    capture(name + '-hidden')
                    subprocess.run(['wtype', '-k', 'slash'], env=child, check=True)
                    for _ in range(100):
                        if search_layer():
                            break
                        time.sleep(.025)
                    assert len(search_layer()) == 1
                    time.sleep(.3)  # Wait for layer-shell exclusive keyboard focus.
                    capture(name + '-empty-search')
                    subprocess.run(['wtype', 'tiles'], env=child, check=True)
                    time.sleep(.2)
                    after = overview()
                    # All marker titles match. Search must not add another
                    # reservation or alter any per-tile preview layout.
                    rectangles = lambda state: {w['address']: w['contentGeometry'] for w in state['windows']}
                    assert rectangles(before) == rectangles(after), (name, before, after)
                    monitor = read('j/monitors')[0]
                    layer = search_layer()[0]
                    search = dict(x=layer['x'], y=layer['y'], width=layer['w'], height=layer['h'])
                    strip_height = 184 if actual_strip and count < 2 else 0
                    expected_y = monitor['y'] + monitor['reserved'][1] + strip_height + round(max(0, (64 - search['height']) / 2))
                    assert abs(search['y'] - expected_y) <= 1, (name, search, expected_y)
                    assert all(not intersects(search, e['geometry']) for e in after['strip']), (name, search, after['strip'])
                    assert all(not intersects(search, w['contentGeometry']) for w in after['windows']), (name, search, after['windows'])
                    if count > 1 and actual_strip:
                        assert {e['tile_id'] for e in after['strip']} == {t['id'] for t in read('j/hymission-tiles-state')['tiles']}
                    if count and scope == 'onlycurrentworkspace':
                        assert len(after['tile_groups']) == count
                        groups = {t['tile_id']: t['geometry'] for t in after['tile_groups']}
                        for window in after['windows']:
                            rect, group = window['contentGeometry'], groups[window['tile_id']]
                            assert rect['x'] >= group['x'] - 1 and rect['x'] + rect['width'] <= group['x'] + group['width'] + 1
                    capture(name + '-shown')
                    checks.append(dict(scene=name, search=search, windows=len(after['windows']), tile_groups=len(after['tile_groups'])))
                    evaluate('hl.plugin.hymission.close()')
                    wait('j/hymission-overview-state', lambda s: not s['active'])
            if bar_process:
                bar_process.terminate()
                bar_process.wait(timeout=5)
                wait('j/monitors', lambda ms: ms[0]['reserved'][1] == 0)
                evaluate('hl.monitor({output="",mode="1920x1080@60",position="0x0",scale="1"}); hl.plugin.hymission_tiles_test.refresh()')
                wait('j/monitors', lambda ms: ms[0]['x'] == 0 and ms[0]['y'] == 0)
    (root / 'search-checks.json').write_text(json.dumps(checks, indent=2))
    (root / 'configerrors.txt').write_text(request('/configerrors'))
    print(f'PASS: {len(checks)} search/strip scenes, stable typing, split workspace layouts, reserved areas and nonzero output origin')
