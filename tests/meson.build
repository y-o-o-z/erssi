subdir('fe-common')
subdir('irc')
subdir('fe-ansi')
subdir('version')
subdir('installer')
test('help files', find_program('help/check-help.sh'), args : [meson.source_root()])
if have_fe_web
  subdir('fe-web')
endif
if have_e2e
  subdir('e2e')
endif
subdir('live')
