# this file is part of irssi

# irssi's fuzz targets: what the server sends, text formats, theme files
# and IRC parameters; and the image decoder of the image preview. With
# -Dwith-fuzzer=yes each is a libFuzzer binary (<name>-fuzz), and
# `meson test` runs it over corpus/<name> with -runs=0: every input once,
# nothing mutated, nothing written. In every other build the target is
# linked with replay.c and `meson test` replays corpus/<name> with that:
# the seeds and the inputs of fixed crashes. (fe-web/ and e2e/ have their
# own meson.build files.)

fuzz_formats = files('null-logger.c', '../fe-ansi/module-formats.c')

# [name, sources, link_with, dependencies]
fuzz_targets = [
  ['irssi', files('irssi.c') + fuzz_formats,
   [libconfig_a, libcore_a, libfuzzer_fe_common_core_a], [dep]],
  ['server', files('server.c') + fuzz_formats,
   [libconfig_a, libcore_a, libfuzzer_fe_common_core_a, libirc_core_a,
    libfe_common_irc_a, libfe_irc_dcc_a, libfe_irc_notifylist_a], [dep]],
  ['event-get-params', files('irc/core/event-get-params.c') + fuzz_formats,
   [libconfig_a, libcore_a, libirc_core_a, libfuzzer_fe_common_core_a], [dep]],
  ['theme-load', files('fe-common/core/theme-load.c') + fuzz_formats,
   [libconfig_a, libcore_a, libfuzzer_fe_common_core_a], [dep]],
]
if have_image_preview
  fuzz_targets += [
    ['image-decode', files('image-preview/decode.c', '../image-preview/image-decode.c'),
     [], [dep, math_dep]],
  ]
endif

foreach t : fuzz_targets
  name = t[0]
  corpus = meson.current_source_dir() / 'corpus' / name
  if want_fuzzer
    fuzz = executable(name + '-fuzz',
      t[1],
      link_with : t[2],
      link_args : [fuzzer_lib],
      link_language : fuzzer_link_language,
      include_directories : rootinc,
      implicit_include_directories : false,
      install : true,
      dependencies : t[3],
    )
    test('fuzz corpus ' + name, fuzz,
      env : fuzz_test_env,
      args : ['-runs=0', corpus],
      timeout : 120)
  else
    replay = executable(name + '-replay',
      t[1] + files('replay.c'),
      link_with : t[2],
      include_directories : rootinc,
      implicit_include_directories : false,
      dependencies : t[3],
      build_by_default : false,
    )
    test('fuzz corpus ' + name, replay,
      env : fuzz_test_env,
      args : [corpus],
      timeout : 120)
  endif
endforeach

# noinst_headers = files(
#   'fuzz-target.h',
#   'null-logger.h',
#   '../fe-ansi/module-formats.h',
# )
