# mentions.pl - a "Mentions" window: everything addressed to your nick
#
# Like the Mentions buffer of repartee. The window gets:
#   * channel messages and /me that erssi marked as a highlight (your nick
#     anywhere in a sentence with hilight_nick_matches_everywhere ON, or any
#     /hilight rule),
#   * private messages, private NOTICEs from people, DCC CHAT messages
#     (not botnet partylines from botnet.pl, not what /ignore catches).
# Every entry is also appended to a file (mentions_log_file), so what was
# written to you while you were away survives an erssi restart.
#
# The nick and the text come from the message signal, not cut out of the
# printed line - so it works with any theme (nick column, separators).
#
# Settings: mentions_window ("Mentions"), mentions_log (ON),
#           mentions_log_file ("~/.erssi/logs/mentions.log")
# Command:  /mentions        - go to the window
#           /mentions clear  - clear the window

use strict;
use warnings;

use Irssi;
use Fcntl qw(O_WRONLY O_APPEND O_CREAT);
use File::Path qw(make_path);
use POSIX qw(strftime);

our $VERSION = '2.1.2';
our %IRSSI = (
    authors     => 'yooz',
    contact     => 'https://github.com/y-o-o-z',
    name        => 'mentions',
    description => 'Mentions window: hilights (also a nick mid-sentence), PMs, NOTICEs and DCC, plus a log file',
    license     => 'MIT',
    url         => 'https://github.com/y-o-o-z/irssi_scripts',
);

Irssi::settings_add_str('mentions',  'mentions_window',   'Mentions');
Irssi::settings_add_bool('mentions', 'mentions_log',      1);
Irssi::settings_add_str('mentions',  'mentions_log_file', '~/.erssi/logs/mentions.log');

Irssi::theme_register([
    'mentions_public',  '{channel $0} {nick $1}%n: $2',
    'mentions_action',  '{channel $0} %_*%_ {nick $1} $2',
    'mentions_private', '%_PM%_ {nick $1}%n: $2',
    'mentions_notice',  '%_NOTICE%_ {nick $1}%n: $2',
    'mentions_dcc',     '%_DCC%_ {nick $1}%n: $2',
    'mentions_info',    '$0',
]);

# Recent channel messages (nick + text), matched later against the printed
# line. erssi prints the line ("print text") only AFTER "message public"
# has finished - checked with a live probe - so a plain "current message"
# variable would be gone by then. One queue per channel, at most 50
# entries and 30 s.
my %recent;    # lc("$tag\0$channel") => [ { kind, tag, target, nick, msg, at } ]
my $RECENT_MAX = 50;
my $RECENT_TTL = 30;

# ── window ────────────────────────────────────────────────────────────

sub window_name { Irssi::settings_get_str('mentions_window') || 'Mentions' }

# The new window is recognised by its pointer (_irssi): erssi activates
# the window "hidden", and chansort renumbers windows as they are created.
sub window {
    my $name = window_name();
    my $window = Irssi::window_find_name($name);
    return $window if $window;
    my $active = Irssi::active_win();
    my $active_ptr = $active ? $active->{_irssi} : undef;
    my %before = map { $_->{_irssi} => 1 } Irssi::windows();
    Irssi::command('window new hidden');
    ($window) = grep { !$before{ $_->{_irssi} } } Irssi::windows();
    $window //= Irssi::active_win();
    $window->set_name($name);
    # immortal: /window close does not close it (undo: /window immortal off)
    $window->command('window immortal on');
    if (defined $active_ptr) {
        my ($previous) = grep { $_->{_irssi} eq $active_ptr } Irssi::windows();
        $previous->set_active() if $previous && $previous->{_irssi} ne $window->{_irssi};
    }
    return $window;
}

sub is_mentions_window {
    my ($win) = @_;
    my $own = Irssi::window_find_name(window_name());
    return $win && $own && $win->{_irssi} eq $own->{_irssi};
}

# ── log ─────────────────────────────────────────────────────────────

sub log_file {
    my $path = Irssi::settings_get_str('mentions_log_file');
    $path =~ s/\A~/$ENV{HOME}/;
    return $path;
}

sub to_log {
    my ($tag, $where, $nick, $text) = @_;
    return unless Irssi::settings_get_bool('mentions_log');
    my $path = log_file();
    # directory 0700 and the file 0600 from the start (sysopen) - never a
    # moment when the log of private messages is readable by others
    my ($dir) = $path =~ m{\A(.*)/};
    make_path($dir, { mode => 0700 }) if defined $dir && length $dir && !-d $dir;
    sysopen(my $fh, $path, O_WRONLY | O_APPEND | O_CREAT, 0600) or return;
    chmod 0600, $path;
    # irssi hands scripts UTF-8 bytes - written without an encoding layer,
    # otherwise non-ASCII characters would be encoded a second time.
    binmode($fh, ':raw');
    printf {$fh} "%s [%s] %s <%s> %s\n", strftime('%Y-%m-%d %H:%M:%S', localtime), $tag // '-',
        $where // '-', $nick // '?', $text // '';
    close $fh;
}

# Level HILIGHT: an entry marks the window in the panel in the mention colour.
sub record {
    my ($format, $tag, $where, $nick, $text) = @_;
    $text = '' unless defined $text;
    $text =~ s/\A\s+|\s+\z//g;
    window()->printformat(MSGLEVEL_HILIGHT, $format, $where // '', $nick // '?', $text);
    to_log($tag, $where, $nick, $text);
}

# ── signals ─────────────────────────────────────────────────────────

sub recent_key { lc(($_[0] // '') . "\0" . ($_[1] // '')) }

sub remember {
    my ($kind) = @_;
    return sub {
        my ($server, $msg, $nick, $address, $target) = @_;
        return unless $server && defined $target && $target =~ /\A[#&!+]/;
        my $queue = $recent{ recent_key($server->{tag}, $target) } //= [];
        my $now = time;
        @$queue = grep { $now - $_->{at} <= $RECENT_TTL } @$queue;
        push @$queue, { kind => $kind, tag => $server->{tag}, target => $target,
            nick => $nick, msg => $msg, at => $now };
        shift @$queue while @$queue > $RECENT_MAX;
    };
}

# The nick in the part of the line before the text: whole, or cut by
# erssi's nick column (nick_column_width: the start of the nick + "+").
sub nick_in_prefix {
    my ($prefix, $nick) = @_;
    return 0 unless defined $nick && length $nick;
    return 1 if index($prefix, $nick) >= 0;
    for my $keep (reverse 1 .. length($nick) - 1) {
        return 1 if index($prefix, substr($nick, 0, $keep) . '+') >= 0;
    }
    return 0;
}

# The queued message that is the END of the printed line, with its sender
# in the line before the text. Newest first: text found in the middle of
# the line, or the same text from someone else, does not attribute the
# sentence to the wrong person.
sub take_recent {
    my ($tag, $target, $line) = @_;
    my $queue = $recent{ recent_key($tag, $target) } or return;
    $line =~ s/\s+\z//;
    for my $i (reverse 0 .. $#$queue) {
        my $plain = Irssi::strip_codes($queue->[$i]{msg} // '');
        $plain =~ s/\s+\z//;
        next unless length $plain && length($line) >= length($plain);
        next unless substr($line, -length $plain) eq $plain;
        next unless nick_in_prefix(substr($line, 0, length($line) - length($plain)), $queue->[$i]{nick});
        return splice(@$queue, $i, 1);
    }
    return;
}

sub sig_print_text {
    my ($dest, $text, $stripped) = @_;
    my $level = $dest->{level};
    return unless $level & MSGLEVEL_HILIGHT;
    return if $level & MSGLEVEL_NOHILIGHT;
    return unless $level & (MSGLEVEL_PUBLIC | MSGLEVEL_ACTIONS);
    return if is_mentions_window($dest->{window});

    my $target = $dest->{target} // '';
    # channels only - PMs, private /me and NOTICEs have their own handlers
    return unless $target =~ /\A[#&!+]/;
    my $tag = $dest->{server} ? $dest->{server}{tag} : undef;
    my $line = $stripped // $text // '';
    if (my $msg = take_recent($tag, $target, $line)) {
        my $format = $msg->{kind} eq 'action' ? 'mentions_action' : 'mentions_public';
        record($format, $msg->{tag}, $target, $msg->{nick}, Irssi::strip_codes($msg->{msg}));
        return;
    }
    # A highlight not from a message (e.g. another script) - the line text without a nick.
    record('mentions_public', $tag, $target, '?', $line);
}

# A botnet partyline (botnet.pl): the hub talks like an IRC server and the
# whole partyline arrives as a conversation - these are not mentions. The
# botnets come from botnet.pl's settings only when it is loaded (erssi
# reports reading a setting nobody registered as an error).
sub is_botnet {
    my ($server) = @_;
    return 0 unless $server && Irssi::Script::botnet->can('partyline_nick');
    my $chatnet = lc($server->{chatnet} // '');
    return 0 unless length $chatnet;
    return (grep { lc $_ eq $chatnet } split /[\s,]+/, Irssi::settings_get_str('botnet_chatnets')) ? 1 : 0;
}

# /ignore: erssi filters PRIVMSG itself (it stops "message private" before
# scripts), but not NOTICE and /me, so they are checked here.
sub ignored {
    my ($server, $nick, $address, $target, $msg, $level) = @_;
    my $hit = eval { $server->ignore_check($nick // '', $address // '', $target // '', $msg // '', $level) };
    return $hit ? 1 : 0;
}

sub sig_private {
    my ($server, $msg, $nick, $address) = @_;
    return if is_botnet($server);
    record('mentions_private', $server->{tag}, 'PM', $nick, $msg);
}

# Private /me (the target is our nick) - channel ones go through sig_print_text.
sub sig_private_action {
    my ($server, $msg, $nick, $address, $target) = @_;
    return unless $server && defined $target && lc $target eq lc($server->{nick} // '');
    return if is_botnet($server);
    return if ignored($server, $nick, $address, $target, $msg, MSGLEVEL_ACTIONS);
    record('mentions_action', $server->{tag}, 'PM', $nick, $msg);
}

sub sig_notice {
    my ($server, $msg, $nick, $address, $target) = @_;
    return if !defined $nick || !length $nick || index($nick, '.') >= 0;   # servers
    return if defined $target && $target =~ /\A[#&!+]/;                     # NOTICE to a channel
    return if is_botnet($server);
    return if ignored($server, $nick, $address, $target, $msg, MSGLEVEL_NOTICES);
    record('mentions_notice', $server->{tag}, 'NOTICE', $nick, $msg);
}

sub sig_dcc {
    my ($dcc, $msg) = @_;
    record('mentions_dcc', 'DCC', 'DCC', $dcc->{nick} || 'DCC', $msg);
}

sub cmd_mentions {
    my ($data) = @_;
    my $window = window();
    if (($data // '') =~ /\A\s*clear\s*\z/i) {
        $window->command('clear');
        return;
    }
    $window->set_active();
}

Irssi::signal_add_first('message public',     remember('msg'));
Irssi::signal_add_first('message irc action', remember('action'));
Irssi::signal_add('print text',               \&sig_print_text);
Irssi::signal_add('message private',          \&sig_private);
Irssi::signal_add('message irc action',       \&sig_private_action);
Irssi::signal_add('message irc notice',       \&sig_notice);
Irssi::signal_add('dcc chat message',         \&sig_dcc);
Irssi::command_bind('mentions', \&cmd_mentions);

# The window is made only after erssi has started: at "irssi init finished"
# erssi names window 1 "Notices", and the built-in sorting could have put
# our window at position 1 before that - "Mentions" would be overwritten.
# The timer fires in the main loop (also on a manual /script load).
Irssi::timeout_add_once(10, sub { window() }, undef);

# Should the window disappear anyway (e.g. /window immortal off +
# /window close), it is made again - mentions do not vanish.
Irssi::signal_add('window destroyed', sub {
    my ($destroyed) = @_;
    return unless lc($destroyed->{name} // '') eq lc window_name();
    Irssi::timeout_add_once(10, sub { window() }, undef);
});

1;
