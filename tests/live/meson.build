# Live tests in Python: erssi in a pty against a fake IRC server
# (tests/live/harness.py, tests/live/ircd.py). Each test installs this build
# under its own temporary directory and gives every erssi instance its own HOME
# and --home. The install step is `meson install` WITHOUT --no-rebuild: a named
# `meson test "live x"` rebuilds only the test's own dependencies, and these
# tests declare none, so the install is what keeps them off a stale binary.
python3_prog = find_program('python3', required : false)
if python3_prog.found()
  live_env = environment()
  live_env.set('HOME', meson.build_root() / 'test-home')
  live_env.set('PYTHONDONTWRITEBYTECODE', '1')
  foreach t : ['harness', 'help', 'mentions', 'mentions_migrate', 'notify', 'keys', 'firstrun', 'search', 'layout', 'window_close', 'query_window', 'connect_existing', 'panel_widths', 'nicklist_bands', 'layout_restore', 'keepnick', 'debug_logs', 'panel_draw', 'e2e_trust', 'e2e_flood', 'credential_warning', 'panel_rows', 'relocatable', 'early_dialog', 'backup', 'update_check', 'hyperlinks']
    # keepnick waits out real check timers and a nick delay (about 170 s),
    # e2e_flood irssi's real flood queue (about 150 s)
    test('live ' + t, python3_prog, args : [files('test_' + t + '.py'), meson.build_root(), meson.source_root()],
         env : live_env, is_parallel : false, timeout : t in ['keepnick', 'e2e_flood'] ? 600 : 300)
  endforeach
endif
