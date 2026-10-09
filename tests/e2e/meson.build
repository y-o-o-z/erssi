# Unit tests of the RPE2E protocol code (src/e2e), with fixed vectors from
# libsodium and from the rpe2e.pl script.
foreach name : ['crypto', 'wire', 'keyring', 'proto']
  exe = executable('test-e2e-' + name,
    files('test-e2e-' + name + '.c'),
    link_with : libe2e_proto_a,
    include_directories : rootinc,
    implicit_include_directories : false,
    dependencies : dep,
  )
  test('test-e2e-' + name + ' test', exe,
    env : test_env,
    args : ['--tap'],
    protocol : 'tap')
endforeach

# native module <-> rpe2e.pl on a local fake IRC server, in a private tmux
# server; skipped without tmux, Perl support or rpe2e.pl's Perl modules
test('e2e live interop (native <-> rpe2e.pl)', find_program('live-interop.sh'),
  args : [meson.build_root(), meson.source_root()],
  env : test_env,
  is_parallel : false,
  timeout : 400)
