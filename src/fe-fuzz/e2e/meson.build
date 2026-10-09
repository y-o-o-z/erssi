# this file is part of irssi

# Fuzz targets for what the e2e module parses: RPE2E wire messages, the
# key exchange CTCPs and IRC lines (wire), keyring.json and /e2e import
# files (keyring). With -Dwith-fuzzer=yes each is a libFuzzer binary
# (e2e-<name>-fuzz); in every build the same target is linked with
# replay.c and `meson test` runs it over corpus/<name>: the seeds and the
# inputs of fixed crashes.

e2e_fuzz_targets = ['wire', 'keyring']

foreach name : e2e_fuzz_targets
  if want_fuzzer
    executable('e2e-' + name + '-fuzz',
      files(name + '.c'),
      link_with : libe2e_proto_a,
      link_args : [fuzzer_lib],
      link_language : fuzzer_link_language,
      include_directories : rootinc,
      implicit_include_directories : false,
      install : true,
      dependencies : dep,
    )
  endif

  replay = executable('e2e-' + name + '-replay',
    files(name + '.c', 'replay.c'),
    link_with : libe2e_proto_a,
    include_directories : rootinc,
    implicit_include_directories : false,
    dependencies : dep,
    build_by_default : false,
  )
  test('e2e fuzz corpus ' + name, replay,
    env : fuzz_test_env,
    args : [meson.current_source_dir() / 'corpus' / name],
    timeout : 120)
endforeach

# noinst_headers = files(
#   'e2e-fuzz.h',
# )
