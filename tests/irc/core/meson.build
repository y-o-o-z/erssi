test_test_irc = executable('test-irc',
  files(
    'test-irc.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
    libirc_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'irc/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-irc test', test_test_irc,
  env : test_env,
  args : [
    '--tap',
  ],
  protocol : 'tap')

test_test_channel_events = executable('test-channel-events',
  files(
    'test-channel-events.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
    libirc_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'irc/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-channel-events test', test_test_channel_events,
  env : test_env,
  args : [
    '--tap',
  ],
  protocol : 'tap')

test_test_cmd_queue = executable('test-cmd-queue',
  files(
    'test-cmd-queue.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
    libirc_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'irc/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-cmd-queue test', test_test_cmd_queue,
  env : test_env,
  args : [
    '--tap',
  ],
  protocol : 'tap')

test_test_sasl_upgrade = executable('test-sasl-upgrade',
  files(
    'test-sasl-upgrade.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libirc_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'irc/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
# Leak detection is off only here, as in test-796: irssi core state is not
# freed on deinit.
test('test-sasl-upgrade test', test_test_sasl_upgrade,
  env : environment({'HOME': meson.build_root() / 'test-home', 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'}),
  args : ['--tap'],
  protocol : 'tap')

test_test_scram = executable('test-scram',
  files(
    'test-scram.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libirc_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'irc/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-scram test', test_test_scram,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

# keepnick next to a script that handles the MONITOR numerics. Leak
# detection is off as in test-sasl-upgrade: irssi core state is not freed
# on deinit.
foreach name : ['keepnick-monitor']
  exe = executable('test-' + name,
    files('test-' + name + '.c'),
    link_with : [
      libconfig_a,
      libcore_a,
      libirc_core_a,
    ],
    c_args : [
      '-D' + 'PACKAGE_STRING' + '="' + 'irc/core' + '"',
    ],
    include_directories : rootinc,
    implicit_include_directories : false,
    dependencies : dep
  )
  test('test-' + name + ' test', exe,
    env : environment({'HOME': meson.build_root() / 'test-home', 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'}),
    args : ['--tap'],
    protocol : 'tap')
endforeach
