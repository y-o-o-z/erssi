test('version scripts test', find_program('sh'),
  args : [files('test-version-scripts.sh'), meson.current_source_dir() / '..' / '..'])

test('version tree test', find_program('sh'),
  args : [files('test-version-tree.sh'), meson.current_source_dir() / '..' / '..',
          meson.project_version(), erssi_exe])

# utils/pre-push: real pushes to throwaway repositories with a throwaway
# signing key (test-pre-push.sh SOURCE_ROOT)
test('pre-push test', find_program('sh'),
  args : [files('test-pre-push.sh'), meson.current_source_dir() / '..' / '..'],
  timeout : 60)

# utils/resign-history.sh: a real git filter-repo rewrite of a synthetic
# history, re-signed with a throwaway key and pushed through utils/pre-push
# (skipped without git filter-repo)
test('resign-history test', find_program('sh'),
  args : [files('test-resign-history.sh'), meson.current_source_dir() / '..' / '..'],
  timeout : 120)
