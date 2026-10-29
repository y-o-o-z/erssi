test_test_safe_file = executable('test-safe-file',
  files(
    'test-safe-file.c',
  ),
  # libconfig too: on FreeBSD safe-file opens through capsicum.c, which
  # needs it
  link_with : [
    libconfig_a,
    libcore_a,
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)

test('test-safe-file test', test_test_safe_file,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap',
  timeout : 30)

# the calling convention signal_emit() relies on, on the build's machine
test_test_signal_abi = executable('test-signal-abi',
  files(
    'test-signal-abi.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)

test('test-signal-abi test', test_test_signal_abi,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

# config_write() (lib-config) when a write fails at the end
test_test_config_write = executable('test-config-write',
  files(
    'test-config-write.c',
  ),
  link_with : [
    libconfig_a,
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)

test('test-config-write test', test_test_config_write,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap',
  timeout : 30)

# the lock every erssi holds on its home (shellter-install.sh --restore)
test_test_home_lock = executable('test-home-lock',
  files(
    'test-home-lock.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)

test('test-home-lock test', test_test_home_lock,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap',
  timeout : 30)
