test('installer versions test', find_program('sh'),
  args : [files('test-installer-versions.sh'), meson.current_source_dir() / '..' / '..'])

# shellter-install.sh with stand-in tools, a local repository and a temporary
# HOME (test-shellter-install.sh SOURCE_ROOT CASE)
foreach c : ['ascii', 'utf8',
    'link-home', 'link-system-prefix', 'link-root', 'link-same-dir',
    'lock-before-venv', 'lock-held-during-venv',
    'freebsd-base-openssl', 'freebsd-no-openssl',
    'src-foreign', 'src-writable', 'src-shared-parent']
  test('installer ' + c, find_program('sh'),
    args : [files('test-shellter-install.sh'), meson.current_source_dir() / '..' / '..', c],
    env : test_env, timeout : 60)
endforeach
