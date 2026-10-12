test_test_formats = executable('test-formats',
  files(
    'test-formats.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-formats test', test_test_formats,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_nick_palette = executable('test-nick-palette',
  files(
    'test-nick-palette.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-nick-palette test', test_test_nick_palette,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_join_names = executable('test-join-names',
  files(
    'test-join-names.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-join-names test', test_test_join_names,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_shellter_defaults = executable('test-shellter-defaults',
  files(
    'test-shellter-defaults.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
    libfe_common_irc_a,
    libirc_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
    def_sysconfdir,
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-shellter-defaults test', test_test_shellter_defaults,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_fe_settings = executable('test-fe-settings',
  files(
    'test-fe-settings.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-fe-settings test', test_test_fe_settings,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_cmd_cat = executable('test-cmd-cat',
  files(
    'test-cmd-cat.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-cmd-cat test', test_test_cmd_cat,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_mentions_log = executable('test-mentions-log',
  files(
    'test-mentions-log.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-mentions-log test', test_test_mentions_log,
  args : ['--tap'],
  protocol : 'tap')
