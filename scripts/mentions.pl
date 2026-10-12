# mentions.pl - replaced by the built-in Mentions window of erssi
#
# The window, its settings (mentions_window, mentions_log, mentions_log_file)
# and its log file are part of erssi now (/HELP MENTIONS). This file is only
# a stub: when it is loaded, erssi unloads it and prints a notice. Remove it
# from <irssi dir>/scripts/autorun to stop the notice.

use strict;
use warnings;

use Irssi;

our $VERSION = '3.0.0';
our %IRSSI = (
    authors     => 'yooz',
    contact     => 'https://github.com/y-o-o-z',
    name        => 'mentions',
    description => 'Stub: the Mentions window is built into erssi now',
    license     => 'MIT',
    url         => 'https://github.com/y-o-o-z/irssi_scripts',
);
