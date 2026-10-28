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

# Desktop integration (installed by `meson install`; see docs/PACKAGING.md).
app_id = 'org.lunduke.LundukeTerminal'
install_data('data/' + app_id + '.desktop', install_dir : get_option('datadir') / 'applications')
install_data('data/' + app_id + '.metainfo.xml', install_dir : get_option('datadir') / 'metainfo')
install_data('data/icons/hicolor/scalable/apps/' + app_id + '.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
install_data('README.md', 'docs/FONTS.md', 'docs/PACKAGING.md',
  install_dir : get_option('datadir') / 'doc' / 'lunduke-terminal')
desktop_validate = find_program('desktop-file-validate', required : false)
if desktop_validate.found()
  test('desktop-file', desktop_validate, args : [meson.current_source_dir() / 'data' / app_id + '.desktop'])
endif
appstreamcli = find_program('appstreamcli', required : false)
if appstreamcli.found()
  test('metainfo', appstreamcli, args : ['validate', '--no-net', '--explain',
    meson.current_source_dir() / 'data' / app_id + '.metainfo.xml'])
endif

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
test('fixes', executable('fixes_test', 'tests/fixes_test.cpp',
  include_directories : inc, link_with : [vterm_lib, effects_lib], dependencies : [vterm_dep, pangocairo_dep]),
  timeout : 60)
# GTK-level fixes; AddressSanitizer makes a use-after-free fail loudly. Skips without $DISPLAY.
view_test_exe = executable('view_test', 'tests/view_test.cpp', 'src/terminal_view.cpp',
  include_directories : inc, link_with : [effects_lib, gl_lib, vterm_lib],
  dependencies : [gtkmm_dep, pangocairo_dep, vterm_dep, epoxy_dep, threads_dep, dependency('x11')],
  cpp_args : ['-fsanitize=address', '-fno-omit-frame-pointer'], link_args : ['-fsanitize=address'])
test('view', view_test_exe, env : ['ASAN_OPTIONS=detect_leaks=0:abort_on_error=1', 'LIBGL_ALWAYS_SOFTWARE=1'],
  timeout : 60, is_parallel : false)
