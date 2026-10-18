# the image preview cache, compiled into the test with the few irssi
# functions it calls stubbed out
test_test_image_cache = executable('test-image-cache',
  files(
    'test-image-cache.c',
    '../../src/core/safe-file.c',
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

# the download retries, with no network request: failures are reported
# by hand
test_test_image_fetch = executable('test-image-fetch',
  files(
    'test-image-fetch.c',
    '../../src/core/safe-file.c',
  ),
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'image-preview' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : [dep, curl_dep]
)
test('test-image-fetch test', test_test_image_fetch,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')
