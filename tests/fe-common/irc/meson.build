test_test_mentions_queue = executable('test-mentions-queue',
  files(
    'test-mentions-queue.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
    libfe_common_irc_a,
    libirc_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/irc' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-mentions-queue test', test_test_mentions_queue,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')
