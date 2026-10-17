subdir('fe-common')
subdir('irc')
subdir('fe-ansi')
subdir('version')
subdir('installer')
test('help files', find_program('help/check-help.sh'), args : [meson.source_root()])
# the defaults that /HELP pages name agree with settings_add_*() in src/
help_python = find_program('python3', required : false)
if help_python.found()
  test('help defaults', help_python, args : [files('help/check-help-defaults.py'), meson.source_root()],
       protocol : 'tap')
  # every topic under "See also" has a page of its own
  test('help see also', help_python, args : [files('help/check-help-see-also.py'), meson.source_root()],
       protocol : 'tap')
endif
if have_fe_web
  subdir('fe-web')
endif
if have_e2e
  subdir('e2e')
endif
subdir('live')
