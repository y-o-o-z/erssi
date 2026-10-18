test_test_safe_file = executable('test-safe-file',
  files(
    'test-safe-file.c',
  ),
  link_with : [
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

