# Unit tests of fe-web (src/fe-web), on the harness of the fe-web fuzz
# targets (src/fe-fuzz/fe-web): web clients on socketpairs, driven by the
# real code of fe-web-server.c, with a stand-in TLS layer and clock.
foreach name : ['output', 'socket', 'pacing', 'login', 'frames', 'unload', 'e2e',
                'password', 'settings', 'tls']
  exe = executable('test-fe-web-' + name,
    files('test-fe-web-' + name + '.c', 'fe-web-test.c'),
    link_with : fe_web_fuzz_link_with,
    include_directories : rootinc,
    implicit_include_directories : false,
    dependencies : [dep, openssl_dep],
  )
  test('test-fe-web-' + name + ' test', exe,
    env : fuzz_test_env,
    args : ['--tap'],
    protocol : 'tap',
    timeout : 60)
endforeach

# fe-web with the e2e module's e2e-web.c (src/e2e), the rest of the module
# stood in for by the test
if have_e2e
  exe = executable('test-fe-web-e2e-module',
    files('test-fe-web-e2e-module.c', 'fe-web-test.c', '../../src/e2e/e2e-web.c'),
    link_with : fe_web_fuzz_link_with + [libe2e_proto_a],
    include_directories : rootinc,
    implicit_include_directories : false,
    dependencies : [dep, openssl_dep],
  )
  test('test-fe-web-e2e-module test', exe,
    env : fuzz_test_env,
    args : ['--tap'],
    protocol : 'tap',
    timeout : 60)
endif
