test('version scripts test', find_program('sh'),
  args : [files('test-version-scripts.sh'), meson.current_source_dir() / '..' / '..'])

test('version tree test', find_program('sh'),
  args : [files('test-version-tree.sh'), meson.current_source_dir() / '..' / '..',
          meson.project_version(), erssi_exe])

# utils/pre-push: real pushes to throwaway repositories with a throwaway
# signing key (test-pre-push.sh SOURCE_ROOT)
test('pre-push test', find_program('sh'),
  args : [files('test-pre-push.sh'), meson.current_source_dir() / '..' / '..'],
  timeout : 300)

# utils/resign-history.sh: a real git filter-repo rewrite of a synthetic
# history, re-signed with a throwaway key and pushed through utils/pre-push
# (skipped without git filter-repo)
test('resign-history test', find_program('sh'),
  args : [files('test-resign-history.sh'), meson.current_source_dir() / '..' / '..'],
  timeout : 120)

# utils/build-binary.sh with a throwaway repository of this tree and a
# throwaway signing key: the tags it refuses and SHA256SUMS (verify), a
# package of this tree and how it runs (package), no package when a test
# fails (tests-fail). The last two build the tree from scratch with every
# core, a few minutes each: run alone, so that the tests beside them are
# not starved of CPU into a timeout.
foreach c : ['verify', 'package', 'tests-fail']
  test('build-binary ' + c, find_program('sh'),
    args : [files('test-build-binary.sh'), meson.current_source_dir() / '..' / '..', c],
    is_parallel : c == 'verify', timeout : c == 'verify' ? 120 : 1800)
endforeach
