# Live tests in Python: erssi in a pty against a fake IRC server
# (tests/live/harness.py, tests/live/ircd.py). Each test installs this build
# under its own temporary directory (meson install --no-rebuild; `meson test`
# rebuilds first) and gives every erssi instance its own HOME and --home.
python3_prog = find_program('python3', required : false)
if python3_prog.found()
  live_env = environment()
  live_env.set('HOME', meson.build_root() / 'test-home')
  foreach t : ['harness', 'help']
    test('live ' + t, python3_prog, args : [files('test_' + t + '.py'), meson.build_root(), meson.source_root()],
         env : live_env, is_parallel : false, timeout : 300)
  endforeach
endif
