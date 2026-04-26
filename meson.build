project('hymission', 'cpp',
  version: '0.2.1',
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
]

shared_module(meson.project_name(), src,
  dependencies: [
    dependency('hyprland'),
  ],
  install: true,
)

executable('hymission-layout-demo',
  ['tools/layout_demo.cpp', 'src/mission_layout.cpp'],
  include_directories: include_directories('src'),
  install: false,
)

mission_layout_test = executable('hymission-mission-layout-test',
  ['tools/mission_layout_test.cpp', 'src/mission_layout.cpp'],
  include_directories: include_directories('src'),
  install: false,
)

test('hymission-mission-layout-test', mission_layout_test)

overview_logic_test = executable('hymission-overview-logic-test',
  ['tools/overview_logic_test.cpp', 'src/overview_logic.cpp'],
  include_directories: include_directories('src'),
  install: false,
)

test('hymission-overview-logic-test', overview_logic_test)
