test('version scripts test', find_program('sh'),
  args : [files('test-version-scripts.sh'), meson.current_source_dir() / '..' / '..'])

test('version tree test', find_program('sh'),
  args : [files('test-version-tree.sh'), meson.current_source_dir() / '..' / '..',
          meson.project_version(), erssi_exe])
