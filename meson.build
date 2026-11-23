project('paint', 'c',
  version: '0.3.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

gnome = import('gnome')
i18n = import('i18n')

gtk4_dep = dependency('gtk4', version: '>= 4.12')
adw_dep = dependency('libadwaita-1', version: '>= 1.6')
cairo_dep = dependency('cairo')
pangocairo_dep = dependency('pangocairo')
math_dep = meson.get_compiler('c').find_library('m', required: false)
webp_dep = dependency('libwebp', required: false)

common_c_args = [
  '-DGETTEXT_PACKAGE="paint"',
  '-DLOCALEDIR="' + (get_option('prefix') / get_option('localedir')) + '"',
]
if webp_dep.found()
  common_c_args += ['-DHAVE_LIBWEBP=1']
endif

resources = gnome.compile_resources('paint-resources',
  'data/paint.gresource.xml',
  source_dir: 'data',
)

core_sources = files(
  'src/paint-tools.c',
  'src/paint-document.c',
  'src/paint-io.c',
  'src/paint-filters.c',
  'src/paint-canvas.c',
)

app_sources = core_sources + files(
  'src/main.c',
  'src/paint-application.c',
  'src/paint-window.c',
  'src/paint-canvas-page.c',
  'src/paint-color-picker.c',
  'src/paint-layers-panel.c',
  'src/paint-session.c',
  'src/paint-dialogs.c',
  'src/paint-preferences.c',
) + [resources]

executable('paint',
  app_sources,
  c_args: common_c_args,
  dependencies: [gtk4_dep, adw_dep, cairo_dep, pangocairo_dep, math_dep, webp_dep],
  include_directories: include_directories('src'),
  install: true,
)

install_data('data/io.github.spondle.Paint.desktop',
  install_dir: get_option('datadir') / 'applications',
)

install_data('data/io.github.spondle.Paint.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

install_data('data/icons/scalable/apps/io.github.spondle.Paint.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps',
)

install_data('data/icons/symbolic/apps/io.github.spondle.Paint-symbolic.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/symbolic/apps',
)

if find_program('msgfmt', required: false).found()
  subdir('po')
endif

test_canvas = executable('test-canvas',
  core_sources + files('tests/test-canvas.c'),
  c_args: common_c_args,
  dependencies: [gtk4_dep, cairo_dep, pangocairo_dep, math_dep, webp_dep],
  include_directories: include_directories('src'),
)

test('canvas-core', test_canvas)

test_keybinds = executable('test-keybinds',
  files('src/paint-preferences.c', 'tests/test-keybinds.c'),
  c_args: common_c_args,
  dependencies: [gtk4_dep, adw_dep, cairo_dep, math_dep],
  include_directories: include_directories('src'),
)

test('keybinds', test_keybinds)
