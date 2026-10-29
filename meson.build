project('hymission', 'cpp',
  version: '0.12.0',
  default_options: ['buildtype=release'],
)

cpp_compiler = meson.get_compiler('cpp')
if cpp_compiler.has_argument('-std=c++23')
  add_global_arguments('-std=c++23', language: 'cpp')
elif cpp_compiler.has_argument('-std=c++2b')
  add_global_arguments('-std=c++2b', language: 'cpp')
else
  error('Could not configure current C++ compiler with C++23 support')
endif

add_project_arguments(
  [
    '-DWLR_USE_UNSTABLE',
    '-g',
    '-Wall',
    '-Wextra',
    '-Wno-unused-parameter',
    '-Wno-unused-value',
    '-Wno-missing-field-initializers',
    '-Wno-narrowing',
    '-Wno-pointer-arith',
  ],
  language: 'cpp')

src = [
  'src/main.cpp',
  'src/mission_layout.cpp',
  'src/overview_logic.cpp',
  'src/overview_controller.cpp',
  'src/stage_logic.cpp',
  'src/stage_controller.cpp',
  'src/stage_edge_effect.cpp',
  'src/tiles_controller.cpp',
  'src/hook_broker.cpp',
  'src/tiles_logic.cpp',
]

glib_dep = dependency('glib-2.0')

broker_test = executable('hymission-hook-broker-test', 'tools/hook_broker_test.cpp', include_directories: include_directories('src'), install: false)
test('hymission-hook-broker-test', broker_test)

tiles_logic_test = executable('hymission-tiles-logic-test',
  ['tools/tiles_logic_test.cpp', 'src/tiles_logic.cpp'],
  include_directories: include_directories('src'), install: false)
test('hymission-tiles-logic-test', tiles_logic_test)

shared_module(meson.project_name(), src,
  dependencies: [
    dependency('hyprland'),
    dependency('lua'),
    glib_dep,
  ],
  install: true,
)

executable('hymission-search-input', 'src/search_input.cpp',
  dependencies: [dependency('gtk4'), dependency('gtk4-layer-shell-0'), dependency('libadwaita-1')],
  install: true,
  install_dir: get_option('libexecdir'),
)

executable('hymission-layout-demo',
  ['tools/layout_demo.cpp', 'src/mission_layout.cpp'],
  include_directories: include_directories('src'),
  install: false,
)

mission_layout_test = executable('hymission-mission-layout-test',
  ['tools/mission_layout_test.cpp', 'src/mission_layout.cpp'],
  cpp_args: ['-D_GLIBCXX_ASSERTIONS'],
  include_directories: include_directories('src'),
  install: false,
)

test('hymission-mission-layout-test', mission_layout_test)

stage_logic_test = executable('hymission-stage-logic-test',
  ['tools/stage_logic_test.cpp', 'src/stage_logic.cpp', 'src/mission_layout.cpp'],
  include_directories: include_directories('src'),
  install: false,
)
test('hymission-stage-logic-test', stage_logic_test)

edge_test_egl = dependency('egl', required: false)
edge_test_gles = dependency('glesv2', required: false)
if edge_test_egl.found() and edge_test_gles.found()
  stage_edge_render_test = executable('hymission-stage-edge-render-test',
    'tools/stage_edge_render_test.cpp',
    include_directories: include_directories('src'),
    dependencies: [edge_test_egl, edge_test_gles, dependency('hyprutils'), dependency('hyprgraphics'), dependency('libdrm')],
    install: false,
  )
  test('hymission-stage-edge-render-test', stage_edge_render_test, env: ['LIBGL_ALWAYS_SOFTWARE=1'])
endif

overview_logic_test = executable('hymission-overview-logic-test',
  ['tools/overview_logic_test.cpp', 'src/overview_logic.cpp'],
  include_directories: include_directories('src'),
  dependencies: [dependency('hyprland'), glib_dep],
  install: false,
)

test('hymission-overview-logic-test', overview_logic_test, timeout: 10)

search_child_process_test = executable('hymission-search-child-process-test',
  'tools/search_child_process_test.cpp',
  include_directories: include_directories('src'),
  dependencies: dependency('wayland-server'),
  install: false,
)
test('search-child-process-test', search_child_process_test, timeout: 10)

private_debug_log_test = executable('hymission-private-debug-log-test',
  'tools/private_debug_log_test.cpp',
  include_directories: include_directories('src'),
  install: false,
)
test('hymission-private-debug-log-test', private_debug_log_test, timeout: 10)

input_guard_test = executable('hymission-input-guard-test',
  'tools/input_guard_test.cpp',
  include_directories: include_directories('src'),
  install: false,
)
test('hymission-input-guard-test', input_guard_test)

config_command_test = executable('hymission-config-command-test',
  'tools/config_command_test.cpp',
  include_directories: include_directories('src'),
  install: false,
)
test('hymission-config-command-test', config_command_test)

deferred_snapshot_test = executable('hymission-deferred-snapshot-test',
  'tools/deferred_snapshot_test.cpp',
  include_directories: include_directories('src'),
  install: false,
)
test('hymission-deferred-snapshot-test', deferred_snapshot_test)

search_focus_test = executable('hymission-search-focus-test',
  'tools/search_focus_test.cpp',
  include_directories: include_directories('src'),
  install: false,
)
test('hymission-search-focus-test', search_focus_test, timeout: 5)

state_json_test = executable('hymission-state-json-test',
  'tools/state_json_test.cpp',
  include_directories: include_directories('src'),
  install: false,
)
test('hymission-state-json-test', state_json_test)
