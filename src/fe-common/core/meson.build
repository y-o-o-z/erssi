# this file is part of irssi

if have_capsicum
  fe_common_core_capsicum_source = files('fe-capsicum.c')
else
  fe_common_core_capsicum_source = []
endif

fe_common_core_sources = [
  files(
    'chat-completion.c',
    'command-history.c',
    'completion.c',
    'fe-channels.c',
    'fe-common-core.c',
    'fe-core-commands.c',
    'fe-credential.c',
    'fe-exec.c',
    'fe-expandos.c',
    'fe-help.c',
    'fe-ignore-messages.c',
    'fe-ignore.c',
    'fe-log-search.c',
    'fe-log.c',
    'fe-messages.c',
    'fe-modules.c',
    'fe-queries.c',
    'fe-recode.c',
    'fe-server.c',
    'fe-settings.c',
    'fe-tls.c',
    'fe-windows.c',
    'firstrun.c',
    'formats.c',
    'hilight-text.c',
    'keyboard.c',
    'log-search.c',
    'mentions-log.c',
    'module-formats.c',
    'printtext.c',
    'shellter-defaults.c',
    'themes.c',
    'update-check.c',
    'window-activity.c',
    'window-commands.c',
    'window-items.c',
    'windows-layout.c',
  )
  + fe_common_core_capsicum_source
  + [
    default_theme_h,
    irssi_version_h,
  ]
]

libfe_common_core_a = static_library('fe_common_core',
  fe_common_core_sources,
  include_directories : rootinc,
  implicit_include_directories : false,
  c_args : [
    def_datadir,
    def_helpdir,
    def_themesdir,
  ],
  dependencies : dep)

# for the fuzz targets and their corpus replays (src/fe-fuzz): no
# printf() fallback on stdout for text that no window prints
libfuzzer_fe_common_core_a = static_library('fuzzer_fe_common_core',
  fe_common_core_sources,
  include_directories : rootinc,
  implicit_include_directories : false,
  c_args : [
    def_datadir,
    def_helpdir,
    def_themesdir,
    def_suppress_printf_fallback,
  ],
  build_by_default : false,
  dependencies : dep)

install_headers(
  files(
    'chat-completion.h',
    'command-history.h',
    'completion.h',
    'fe-capsicum.h',
    'fe-channels.h',
    'fe-common-core.h',
    'fe-core-commands.h',
    'fe-credential.h',
    'fe-exec.h',
  'fe-expandos.h',
    'fe-log.h',
    'fe-messages.h',
    'fe-queries.h',
    'fe-recode.h',
    'fe-server.h',
    'fe-settings.h',
    'fe-tls.h',
    'fe-windows.h',
    'firstrun.h',
    'formats.h',
    'hilight-text.h',
    'keyboard.h',
    'log-search.h',
    'mentions-log.h',
    'module-formats.h',
    'module.h',
    'printtext.h',
    'shellter-defaults.h',
    'themes.h',
    'update-check.h',
    'window-activity.h',
    'window-items.h',
    'windows-layout.h',
  ),
  subdir : incdir / 'src' / 'fe-common' / 'core')
