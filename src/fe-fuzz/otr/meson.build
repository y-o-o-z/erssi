# this file is part of irssi

# Fuzz target for the OTR fragment reassembly (src/otr/otr-reassembly.c):
# the pure string handling that joins the PRIVMSGs a peer splits a long OTR
# message into. It has no libotr type, so the target compiles that source
# directly instead of linking the otr module; it needs only glib, though the
# shared `dep` still carries libotr when OTR is enabled.
#
# With -Dwith-fuzzer=yes each is a libFuzzer binary (otr-<name>-fuzz), and
# `meson test` runs that binary over corpus/<name> with -runs=0: every input
# once, nothing mutated, nothing written. In every other build the target is
# linked with replay.c and `meson test` runs that over corpus/<name>: the
# seeds and the inputs of fixed crashes.

otr_reassembly_src = files('reassembly.c', '../../otr/otr-reassembly.c')

otr_fuzz_targets = ['reassembly']

foreach name : otr_fuzz_targets
  corpus = meson.current_source_dir() / 'corpus' / name
  if want_fuzzer
    fuzz = executable('otr-' + name + '-fuzz',
      otr_reassembly_src,
      link_args : [fuzzer_lib],
      link_language : fuzzer_link_language,
      include_directories : rootinc,
      implicit_include_directories : false,
      install : true,
      dependencies : dep,
    )
    test('otr fuzz corpus ' + name, fuzz,
      env : fuzz_test_env,
      args : ['-runs=0', corpus],
      timeout : 120)
  else
    replay = executable('otr-' + name + '-replay',
      otr_reassembly_src + files('../replay.c'),
      include_directories : rootinc,
      implicit_include_directories : false,
      dependencies : dep,
      build_by_default : false,
    )
    test('otr fuzz corpus ' + name, replay,
      env : fuzz_test_env,
      args : [corpus],
      timeout : 120)
  endif
endforeach

# noinst_headers = files(
#   '../fuzz-target.h',
# )
