project('lunduke-terminal', 'cpp',
  version : '0.0.1',
  license : 'GPL-3.0-or-later',
  meson_version : '>=0.56',
  default_options : ['cpp_std=c++17', 'warning_level=2', 'buildtype=release'])

gtkmm_dep = dependency('gtkmm-3.0', version : '>=3.24')
pangocairo_dep = dependency('pangocairo')
vterm_dep = dependency('vterm', version : '>=0.3')
epoxy_dep = dependency('epoxy')
egl_dep = dependency('egl')
threads_dep = dependency('threads')
fontconfig_dep = dependency('fontconfig')
util_dep = meson.get_compiler('cpp').find_library('util')

inc = include_directories('src')

effects_lib = static_library('lterm-effects',
  'src/effects.cpp', 'src/cpu_effects.cpp', 'src/thread_pool.cpp',
  'src/grid_painter.cpp', 'src/synthetic_frame.cpp', 'src/profiles.cpp',
  'src/font_resolve.cpp',
  dependencies : [pangocairo_dep, threads_dep, fontconfig_dep])

gl_lib = static_library('lterm-gl', 'src/gl_effects.cpp', 'src/gl_pipeline.cpp',
  link_with : effects_lib, dependencies : [epoxy_dep, egl_dep])

vterm_lib = static_library('lterm-vterm', 'src/vterm_session.cpp', 'src/selection.cpp',
  link_with : effects_lib, dependencies : [vterm_dep, util_dep, pangocairo_dep])

executable('lunduke-terminal',
  'src/main.cpp', 'src/terminal_view.cpp',
  link_with : [effects_lib, gl_lib, vterm_lib],
  dependencies : [gtkmm_dep, pangocairo_dep, vterm_dep, epoxy_dep, threads_dep],
  install : true)

executable('lterm-m0-bench', 'tools/m0_bench.cpp',
  include_directories : inc,
  link_with : [effects_lib, gl_lib],
  dependencies : [pangocairo_dep, epoxy_dep, threads_dep])

executable('lterm-m1-bench', 'tools/m1_bench.cpp',
  include_directories : inc,
  link_with : [effects_lib, gl_lib],
  dependencies : [pangocairo_dep, epoxy_dep, threads_dep])
test('effects', executable('effects_test', 'tests/effects_test.cpp',
  include_directories : inc, link_with : effects_lib, dependencies : [pangocairo_dep, threads_dep]))
test('profiles', executable('profiles_test', 'tests/profiles_test.cpp',
  include_directories : inc, link_with : effects_lib, dependencies : [pangocairo_dep, threads_dep]))
test('painter', executable('painter_test', 'tests/painter_test.cpp',
  include_directories : inc, link_with : effects_lib, dependencies : [pangocairo_dep, threads_dep]))
test('fonts', executable('fonts_test', 'tests/fonts_test.cpp',
  include_directories : inc, link_with : effects_lib, dependencies : [pangocairo_dep, threads_dep, fontconfig_dep]))
test('vterm', executable('vterm_test', 'tests/vterm_test.cpp',
  include_directories : inc, link_with : [vterm_lib, effects_lib], dependencies : [vterm_dep, pangocairo_dep]))
test('gl-match', executable('gl_match_test', 'tests/gl_match_test.cpp',
  include_directories : inc, link_with : [effects_lib, gl_lib],
  dependencies : [pangocairo_dep, epoxy_dep, threads_dep]),
  env : ['LIBGL_ALWAYS_SOFTWARE=1'], timeout : 120)
test('session', executable('session_test', 'tests/session_test.cpp',
  include_directories : inc, link_with : [vterm_lib, effects_lib], dependencies : [vterm_dep, pangocairo_dep]),
  timeout : 60)
