# the image preview cache, compiled into the test with the few irssi
# functions it calls stubbed out
test_test_image_cache = executable('test-image-cache',
  files(
    'test-image-cache.c',
  ),
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'image-preview' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep
)
test('test-image-cache test', test_test_image_cache,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')
