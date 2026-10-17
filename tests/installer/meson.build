# the release order, in this installer and in that of shellter-v1.3.10
# (skipped without that tag here)
test('installer versions test', find_program('sh'),
  args : [files('test-installer-versions.sh'), meson.current_source_dir() / '..' / '..', 'cur'],
  protocol : 'tap')
test('installer versions test (shellter-v1.3.10)', find_program('sh'),
  args : [files('test-installer-versions.sh'), meson.current_source_dir() / '..' / '..', 'old'],
  protocol : 'tap')

# shellter-install.sh with stand-in tools, a local repository and a temporary
# HOME (test-shellter-install.sh SOURCE_ROOT CASE)
foreach c : ['ascii', 'utf8',
    'link-home', 'link-system-prefix', 'link-root', 'link-same-dir',
    'lock-before-venv', 'lock-held-during-venv',
    'freebsd-base-openssl', 'freebsd-no-openssl',
    'src-foreign', 'src-writable', 'src-shared-parent', 'acl-bsd',
    'userns', 'userns-kernel',
    'venv-meson-only', 'venv-ninja-only', 'venv-no-ninja-wheel', 'venv-both',
    'stale-nexus',
    'release-signature', 'release-newest', 'release-pinned',
    'update', 'update-same-version', 'update-branch']
  test('installer ' + c, find_program('sh'),
    args : [files('test-shellter-install.sh'), meson.current_source_dir() / '..' / '..', c],
    env : test_env, timeout : 60)
endforeach
