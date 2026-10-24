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

test_test_nick_column = executable('test-nick-column',
  files(
    'test-nick-column.c',
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
test('test-nick-column test', test_test_nick_column,
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

# dlsym(RTLD_NEXT): in libdl before glibc 2.34
test_test_firstrun = executable('test-firstrun',
  files(
    'test-firstrun.c',
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
  dependencies : dep + [cc.find_library('dl', required : false)]
)
test('test-firstrun test', test_test_firstrun,
  args : ['--tap'],
  protocol : 'tap')

test_test_log_search = executable('test-log-search',
  files(
    'test-log-search.c',
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
test('test-log-search test', test_test_log_search,
  args : ['--tap'],
  protocol : 'tap')
# the two 1 GiB scans alone; they skip unless ERSSI_TEST_PERF is set:
# ERSSI_TEST_PERF=1 meson test --suite perf
test('test-log-search perf', test_test_log_search,
  args : ['--tap', '-p', '/log_search/perf'],
  protocol : 'tap',
  suite : 'perf',
  timeout : 600)

test_test_shipped_themes = executable('test-shipped-themes',
  files(
    'test-shipped-themes.c',
  ),
  link_with : [
    libconfig_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-shipped-themes test', test_test_shipped_themes,
  env : test_env,
  args : ['--tap'] + shipped_themes,
  protocol : 'tap')

test_test_theme_formats = executable('test-theme-formats',
  files(
    'test-theme-formats.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
    libfe_common_irc_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-common/core' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-theme-formats test', test_test_theme_formats,
  env : test_env,
  args : ['--tap'] + shipped_themes,
  protocol : 'tap')

test_test_credential = executable('test-credential',
  files(
    'test-credential.c',
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
test('test-credential test', test_test_credential,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

# core/credential.c compiled in, with an encryption that can be made to fail
test_test_credential_set = executable('test-credential-set',
  files(
    'test-credential-set.c',
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
test('test-credential-set test', test_test_credential_set,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

# core/network-openssl.c compiled in, for its certificate host name check
test_test_tls_hostname = executable('test-tls-hostname',
  files(
    'test-tls-hostname.c',
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
test('test-tls-hostname test', test_test_tls_hostname,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

# /SAVE with an old encrypted credential in the configuration
test_test_credential_save = executable('test-credential-save',
  files(
    'test-credential-save.c',
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
test('test-credential-save test', test_test_credential_save,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

# the external credentials file: a regular file only, never through a link
test_test_credential_file = executable('test-credential-file',
  files(
    'test-credential-file.c',
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
test('test-credential-file test', test_test_credential_file,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap',
  timeout : 30)

# the startup warning about the old credential encryption
test_test_credential_warning = executable('test-credential-warning',
  files(
    'test-credential-warning.c',
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
test('test-credential-warning test', test_test_credential_warning,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')
