# Unit tests of the RPE2E protocol code (src/e2e), with fixed vectors from
# libsodium and from the rpe2e.pl script.
foreach name : ['crypto', 'wire', 'keyring', 'proto', 'scope', 'outq', 'state']
  exe = executable('test-e2e-' + name,
    files('test-e2e-' + name + '.c'),
    link_with : libe2e_proto_a,
    include_directories : rootinc,
    implicit_include_directories : false,
    dependencies : dep,
  )
  # scope sweeps every code point and 100k random names (~50 s in ASan),
  # proto floods the request limits with signed KEYREQs (~17 s): run in
  # parts, each its own process (E2E_TEST_PART), so that no single test
  # holds that much work - whole, they ran into the time limit on loaded
  # runners. The parts are a few seconds each alone; the limit leaves room
  # for a runner many times slower.
  parts = {
    'scope' : ['main', 'sweep-0', 'sweep-1', 'sweep-2', 'sweep-3'],
    'proto' : ['proto', 'other'],
  }.get(name, [''])
  foreach part : parts
    test('test-e2e-' + name + (part == '' ? '' : ' ' + part) + ' test', exe,
      env : part == '' ? test_env : environment(test_env_vars + {'E2E_TEST_PART' : part}),
      args : ['--tap'],
      protocol : 'tap',
      timeout : part == '' ? 30 : 120)
  endforeach
endforeach

# native module <-> rpe2e.pl on a local fake IRC server, in a private tmux
# server; skipped without tmux, Perl support or rpe2e.pl's Perl modules.
# It installs this build (meson install), so it depends on what it runs:
# `meson test` rebuilds those first. About 8 min: every encrypted line
# holds the flood queue 4 s and more; CI runs it without a multiplier
live_depends = [erssi_exe, libe2e_core_sm, libirc_core_sm]
if have_perl
  live_depends += [libperl_core_sm, libfe_perl_sm]
endif
test('e2e live interop (native <-> rpe2e.pl)', find_program('live-interop.sh'),
  args : [meson.build_root(), meson.source_root()],
  env : test_env,
  depends : live_depends,
  is_parallel : false,
  timeout : 1500)

# native module <-> repartee itself, on the fake server of tests/live;
# skipped unless REPARTEE_BIN names a repartee binary (and without tmux)
python3_e2e = find_program('python3', required : false)
if python3_e2e.found()
  test('e2e live interop (native <-> repartee)', python3_e2e,
    args : [files('repartee-interop.py'), meson.build_root(), meson.source_root()],
    env : test_env,
    is_parallel : false,
    timeout : 600)
endif
