test_test_paste_join_multiline = executable('test-paste-join-multiline',
  files(
    '../../src/fe-ansi/gui-entry.c',
    '../../src/fe-ansi/gui-gestures.c',
    '../../src/fe-ansi/gui-mouse.c',
    '../../src/fe-ansi/gui-printtext.c',
    '../../src/fe-ansi/gui-windows.c',
    '../../src/fe-ansi/mainwindows.c',
    '../../src/fe-ansi/term-ansi.c',
    '../../src/fe-ansi/sidepanels-core.c',
    '../../src/fe-ansi/sidepanels-render.c',
    '../../src/fe-ansi/sidepanels-text.c',
    '../../src/fe-ansi/sidepanels-activity.c',
    '../../src/fe-ansi/sidepanels-signals.c',
    '../../src/fe-ansi/sidepanels-layout.c',
    '../../src/fe-ansi/textbuffer-formats.c',
    '../../src/fe-ansi/textbuffer-view.c',
    '../../src/fe-ansi/textbuffer.c',
    '../../src/fe-ansi/resize-debug.c',
    'mock-irssi.c',
    'test-paste-join-multiline.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-ansi' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep,
)

test('test-paste-join-multiline test', test_test_paste_join_multiline,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_sidepanel_text = executable('test-sidepanel-text',
  files(
    '../../src/fe-ansi/sidepanels-text.c',
    'test-sidepanel-text.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-ansi' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep,
)

test('test-sidepanel-text test', test_test_sidepanel_text,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_term_defaults = executable('test-term-defaults',
  files(
    '../../src/fe-ansi/gui-entry.c',
    '../../src/fe-ansi/gui-gestures.c',
    '../../src/fe-ansi/gui-mouse.c',
    '../../src/fe-ansi/gui-printtext.c',
    '../../src/fe-ansi/gui-windows.c',
    '../../src/fe-ansi/mainwindows.c',
    '../../src/fe-ansi/term-ansi.c',
    '../../src/fe-ansi/sidepanels-core.c',
    '../../src/fe-ansi/sidepanels-render.c',
    '../../src/fe-ansi/sidepanels-text.c',
    '../../src/fe-ansi/sidepanels-activity.c',
    '../../src/fe-ansi/sidepanels-signals.c',
    '../../src/fe-ansi/sidepanels-layout.c',
    '../../src/fe-ansi/textbuffer-formats.c',
    '../../src/fe-ansi/textbuffer-view.c',
    '../../src/fe-ansi/textbuffer.c',
    '../../src/fe-ansi/resize-debug.c',
    'mock-irssi.c',
    'test-term-defaults.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-ansi' + '"',
    def_sysconfdir,
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep,
)

test('test-term-defaults test', test_test_term_defaults,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_term_colors = executable('test-term-colors',
  files(
    '../../src/fe-ansi/gui-entry.c',
    '../../src/fe-ansi/gui-gestures.c',
    '../../src/fe-ansi/gui-mouse.c',
    '../../src/fe-ansi/gui-printtext.c',
    '../../src/fe-ansi/gui-windows.c',
    '../../src/fe-ansi/mainwindows.c',
    '../../src/fe-ansi/term-ansi.c',
    '../../src/fe-ansi/sidepanels-core.c',
    '../../src/fe-ansi/sidepanels-render.c',
    '../../src/fe-ansi/sidepanels-text.c',
    '../../src/fe-ansi/sidepanels-activity.c',
    '../../src/fe-ansi/sidepanels-signals.c',
    '../../src/fe-ansi/sidepanels-layout.c',
    '../../src/fe-ansi/textbuffer-formats.c',
    '../../src/fe-ansi/textbuffer-view.c',
    '../../src/fe-ansi/textbuffer.c',
    '../../src/fe-ansi/resize-debug.c',
    'mock-irssi.c',
    'test-term-colors.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
    libfe_common_core_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-ansi' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep,
)

test('test-term-colors test', test_test_term_colors,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_default_files = executable('test-default-files',
  files(
    '../../src/fe-ansi/default-files.c',
    'test-default-files.c',
  ),
  link_with : [
    libconfig_a,
    libcore_a,
  ],
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-ansi' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep,
)

# under AddressSanitizer with leak detection, whatever the rest of the
# suite uses: a leak here (a note, a record) fails the test
test_default_files_env = environment()
test_default_files_env.set('HOME', meson.build_root() / 'test-home')
test_default_files_env.set('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')

test('test-default-files test', test_test_default_files,
  env : test_default_files_env,
  args : ['--tap'],
  protocol : 'tap')

test_test_term_notify = executable('test-term-notify',
  files(
    '../../src/fe-ansi/term-notify.c',
    'test-term-notify.c',
  ),
  c_args : [
    '-D' + 'PACKAGE_STRING' + '="' + 'fe-ansi' + '"',
  ],
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep,
)

test('test-term-notify test', test_test_term_notify,
  env : test_env,
  args : ['--tap'],
  protocol : 'tap')
