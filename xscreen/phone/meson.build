project('xscreen', 'c',
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=release'])

add_project_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers',
                      language: 'c')

gio = dependency('gio-2.0')
pipewire = dependency('libpipewire-0.3')
egl = dependency('egl')
glesv2 = dependency('glesv2')
libdrm = dependency('libdrm')
threads = dependency('threads')

cc = meson.get_compiler('c')
libm = cc.find_library('m', required: false)

core_sources = files(
  'src/xs-capture.c',
  'src/xs-damage.c',
  'src/xs-enc.c',
  'src/xs-gpu.c',
)

executable('xs-probe',
  core_sources + files('src/xs-probe.c'),
  dependencies: [gio, pipewire, egl, glesv2, libdrm, threads])

executable('xs-gpubench',
  core_sources + files('src/xs-gpubench.c'),
  dependencies: [gio, pipewire, egl, glesv2, libdrm, threads])

executable('xscreend',
  core_sources + files('src/xscreend.c', 'src/xs-input.c', 'src/xs-layout.c',
                       'src/xs-scrcpy.c', 'src/xs-clipboard.c', 'src/xs-audio.c',
                       'src/xs-files.c'),
  dependencies: [gio, dependency('gio-unix-2.0'), dependency('libei-1.0'), pipewire, egl,
                 glesv2, libdrm, threads, libm],
  install: true)

executable('xs-gputest',
  files('src/xs-gputest.c', 'src/xs-gpu.c', 'src/xs-damage.c'),
  dependencies: [egl, glesv2, libdrm, libm])

test('damage', executable('test-damage',
  files('tests/test-damage.c', 'src/xs-damage.c'),
  include_directories: include_directories('src')))

test('scrcpy', executable('test-scrcpy',
  files('tests/test-scrcpy.c', 'src/xs-scrcpy.c'),
  dependencies: [dependency('glib-2.0'), libm]))
