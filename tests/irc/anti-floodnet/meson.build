test_test_anti_floodnet = executable('test-anti-floodnet',
  files(
    'test-anti-floodnet.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
    libirc_core_a,
    libirc_flood_a,
    libirc_anti_floodnet_a,
    libfe_common_irc_a,
    libfe_irc_dcc_a,
    libfe_irc_notifylist_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'irc/anti-floodnet' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
# Leak detection is off only here, as in test-796: irssi core state is not
# freed on deinit.
test('test-anti-floodnet test', test_test_anti_floodnet,
  env : environment({'HOME': meson.build_root() / 'test-home', 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'}),
  args : ['--tap'],
  protocol : 'tap')
