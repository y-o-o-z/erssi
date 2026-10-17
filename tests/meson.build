subdir('fe-common')
subdir('irc')
subdir('fe-ansi')
subdir('version')
subdir('installer')
test('help files', find_program('help/check-help.sh'), args : [meson.source_root()])
python3_prog = find_program('python3', required : false)
if python3_prog.found()
  # the defaults that /HELP pages name agree with settings_add_*() in src/
  test('help defaults', python3_prog, args : [files('help/check-help-defaults.py'), meson.source_root()],
       protocol : 'tap')
  # every topic under "See also" has a page of its own
  test('help see also', python3_prog, args : [files('help/check-help-see-also.py'), meson.source_root()],
       protocol : 'tap')
  # contrib/rpe2e/rpe2e-from-repartee rewrites long-term E2E identity keys;
  # its tests skip (77) without libsodium
  test('rpe2e-from-repartee', python3_prog,
       args : [files('../contrib/rpe2e/test_rpe2e_from_repartee.py')],
       env : ['PYTHONDONTWRITEBYTECODE=1'], timeout : 60)
endif
if have_fe_web
  subdir('fe-web')
endif
if have_e2e
  subdir('e2e')
endif
subdir('live')
