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
