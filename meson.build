project('retrolunar', 'c',
  version : '2.0.0',
  default_options : ['c_std=gnu11', 'warning_level=3'],
)

cc = meson.get_compiler('c')
if not cc.has_function('realpath', prefix : '#include <stdlib.h>')
  error('retrolunar needs realpath()')
endif

add_project_arguments(
  '-DRETROLUNAR_DEFAULT_SYSTEM="' + get_option('default_system') + '"',
  language : 'c',
)

# Vendored, two files: see vendor/tomlc17/README.md for the revision.
tomlc17 = static_library('tomlc17', 'vendor/tomlc17/tomlc17.c')
tomlc17_dep = declare_dependency(
  link_with : tomlc17,
  include_directories : include_directories('vendor/tomlc17'),
)

retrolunar_sources = files(
  'src/main.c',
  'src/cli.c',
  'src/util.c',
  'src/hash.c',
  'src/tree.c',
  'src/toml_read.c',
  'src/system.c',
  'src/package.c',
  'src/manifest.c',
  'src/resolve.c',
  'src/fingerprint.c',
  'src/env.c',
  'src/emit.c',
  'src/cmd_search.c',
  'src/cmd_deps.c',
  'src/cmd_install.c',
  'src/cmd_generate.c',
  'src/cmd_env.c',
  'src/cmd_export.c',
  'src/cmd_shell.c',
  'src/cmd_build.c',
  'src/cmd_gc.c',
  'src/repos.c',
  'src/lock.c',
  'src/bin.c',
)

executable('retrolunar',
  retrolunar_sources,
  dependencies : tomlc17_dep,
  install : true,
)
