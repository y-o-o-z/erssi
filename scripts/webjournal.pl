# webjournal.pl - a journal of erssi windows for the web client (NexusIRC)
#
# The web client talks to erssi through fe-web and gets events only while it
# is connected. This script writes everything shown in erssi windows to
# disk, so that after a restart the web client can fill the gaps and show
# the same as the terminal:
#
#   * channels and queries - structured entries (time, kind, nick, text)
#     from which the web client rebuilds ordinary messages,
#   * windows without a channel (Notices, Mentions and other named windows,
#     and the network status window named after its tag) - ready text
#     lines; the web client shows them live by reading the end of the file,
#   * client and script messages in channel windows (CLIENT* levels, e.g.
#     "[E2E] ..." from rpe2e.pl) - also as text lines in the channel file;
#     fe-web does not forward them, and without them the web client would
#     not show e.g. a key exchange request.
#
# Netsplits: fe-netjoin stops "message quit" for split quits, "message join"
# for rejoins after a split and "message irc mode" for the ops given back
# then, and prints a summary instead. Its handlers are added again when
# hide_netsplit_quits is turned off and on, and then run before a script's
# handlers at the same priority, so ours are registered at -150 (before
# signal_add_first): the web client still learns who left and came back.
#
# Directory layout (webjournal_dir, ~/.erssi/journal by default):
#   <tag>/<channel|nick>.jsonl   e.g. ircnet/#polska.jsonl
#   <tag>/%status.jsonl          the status window of network <tag>
#   %windows/<window name>.jsonl e.g. %windows/notices.jsonl
# Names: ASCII lower case, characters outside [a-z0-9#&+!._-] as %XX (UTF-8
# bytes). "%status" and "%windows" cannot come from an encoded name.
#
# An entry is one JSON line (UTF-8) with the fields:
#   t   time (seconds, fraction)    k  kind: msg action notice join part
#   n   sender's nick                   quit kick topic mode nick text
#   h   user@host                   x  text (no colour codes for "text")
#   s   1 = own message             hl 1 = highlight (nick in the text)
#   tg  kicked nick (kick)          nn new nick (nick)
#   w   window name (text lines, original case)
# A file larger than webjournal_max_kb is renamed to <file>.1 (the previous
# .1 is removed) and writing starts again - a reader notices that the file
# got smaller.
#
# Settings: webjournal (ON), webjournal_dir, webjournal_max_kb (2048)
# Command:  /webjournal - the journal's state

use strict;
use warnings;

use Irssi;
use JSON::PP ();
use Time::HiRes ();
use File::Path qw(make_path);
use Fcntl qw(O_WRONLY O_APPEND O_CREAT);

our $VERSION = '1.2.5';
our %IRSSI = (
    authors     => 'yooz',
    contact     => 'https://github.com/y-o-o-z',
    name        => 'webjournal',
    description => 'Journal of erssi windows (channels, queries, Notices, Mentions, status) for the web client',
    license     => 'MIT',
    url         => 'https://github.com/y-o-o-z/irssi_scripts',
);

Irssi::settings_add_bool('webjournal', 'webjournal',        1);
Irssi::settings_add_str('webjournal',  'webjournal_dir',    '~/.erssi/journal');
Irssi::settings_add_int('webjournal',  'webjournal_max_kb', 2048);

my $JSON = JSON::PP->new->utf8(1)->canonical(1);
my %stats = (written => 0, rotated => 0, errors => 0, last_error => '');

# Open files (path => handle): long outputs, e.g. /list in the status
# window, are thousands of lines - no open() for each of them. At most
# $FH_MAX at a time; the least recently used are closed first.
my %fh;
my %fh_used;
my $FH_MAX = 64;
my $fh_clock = 0;

# ── file names ───────────────────────────────────────────────────────

sub encode_name {
    my ($name) = @_;
    $name = '' unless defined $name;
    utf8::encode($name) if utf8::is_utf8($name);
    $name =~ tr/A-Z/a-z/;
    $name =~ s/([^a-z0-9#&+!._-])/sprintf('%%%02X', ord $1)/ge;
    return $name;
}

sub base_dir {
    my $dir = Irssi::settings_get_str('webjournal_dir') || '~/.erssi/journal';
    $dir =~ s/^~(?=\/|$)/$ENV{HOME}/;
    return $dir;
}

sub target_file { base_dir() . '/' . encode_name($_[0]) . '/' . encode_name($_[1]) . '.jsonl' }
sub status_file { base_dir() . '/' . encode_name($_[0]) . '/%status.jsonl' }
sub window_file { base_dir() . '/%windows/' . encode_name($_[0]) . '.jsonl' }

# ── writing ──────────────────────────────────────────────────────────

# Text from irssi is either a character string (UTF-8 flag) or raw bytes.
# Turn it into characters, so that JSON does not encode UTF-8 bytes twice.
sub chars {
    my ($s) = @_;
    return undef unless defined $s;
    utf8::decode($s) unless utf8::is_utf8($s);
    return $s;
}

sub append {
    my ($file, %entry) = @_;
    return unless Irssi::settings_get_bool('webjournal');
    my %clean = (t => sprintf('%.3f', Time::HiRes::time()) + 0);
    for my $key (keys %entry) {
        next unless defined $entry{$key};
        $clean{$key} = $key =~ /^(?:s|hl)$/ ? ($entry{$key} ? 1 : 0) : chars($entry{$key});
    }
    delete $clean{$_} for grep { $_ =~ /^(?:s|hl)$/ && !$clean{$_} } keys %clean;

    my $ok = eval {
        rotate($file);
        my $fh = handle_for($file);
        print {$fh} $JSON->encode(\%clean), "\n" or die "$file: $!\n";
        1;
    };
    close_handle($file) unless $ok;
    if ($ok) {
        $stats{written}++;
    } else {
        $stats{errors}++;
        $stats{last_error} = $@;
        $stats{last_error} =~ s/\s+$//;
    }
}

# The file is created with mode 0600 right away (sysopen), the directory
# 0700; writes are unbuffered, so a reader (Nexus) sees whole lines at once.
sub handle_for {
    my ($file) = @_;
    $fh_used{$file} = ++$fh_clock;
    if (my $open = $fh{$file}) {
        # The file was removed or replaced from outside (rm, logrotate)
        # while open: writes would go to a file nobody can see any more.
        # Same file = same device and inode; otherwise open it again.
        my @disk = stat $file;
        my @fd   = stat $open;
        return $open if @disk && @fd && $disk[0] == $fd[0] && $disk[1] == $fd[1];
        close_handle($file);
        $fh_used{$file} = $fh_clock;
    }
    my ($dir) = $file =~ m{^(.*)/};
    make_path($dir, { mode => 0700 }) unless -d $dir;
    sysopen(my $fh, $file, O_WRONLY | O_APPEND | O_CREAT, 0600) or die "$file: $!\n";
    binmode($fh, ':raw');
    $fh->autoflush(1);
    $fh{$file} = $fh;
    if (keys %fh > $FH_MAX) {
        my ($oldest) = sort { $fh_used{$a} <=> $fh_used{$b} } grep { $_ ne $file } keys %fh;
        close_handle($oldest);
    }
    return $fh;
}

sub close_handle {
    my ($file) = @_;
    my $fh = delete $fh{$file};
    delete $fh_used{$file};
    close($fh) if $fh;
}

sub rotate {
    my ($file) = @_;
    my $max = Irssi::settings_get_int('webjournal_max_kb');
    $max = 2048 if $max <= 0;
    my $size = -s $file;
    return unless $size && $size > $max * 1024;
    close_handle($file);
    rename($file, "$file.1") or die "$file.1: $!\n";
    $stats{rotated}++;
}

# ── channels and queries ────────────────────────────────────────────

sub is_channel {
    my ($server, $target) = @_;
    return 0 unless defined $target && length $target;
    my $ok = eval { $server->ischannel($target) };
    return $ok if defined $ok;
    return $target =~ /^[#&!+]/ ? 1 : 0;
}

# Like hilight_nick_matches_everywhere: the nick as a separate word in the text.
sub mentions_me {
    my ($server, $text) = @_;
    my $nick = $server->{nick};
    return 0 unless defined $nick && length $nick && defined $text;
    return $text =~ /(?<![\w\[\]\\`^{|}-])\Q$nick\E(?![\w\[\]\\`^{|}-])/i ? 1 : 0;
}

sub ignored {
    my ($server, $nick, $address, $target, $text, $level) = @_;
    # no address (a message from the server) or no target is a deliberate NULL for irssi
    no warnings 'uninitialized';
    my $hit = eval { $server->ignore_check($nick, $address, $target, $text, $level) };
    return $hit ? 1 : 0;
}

sub log_target {
    my ($server, $target, %entry) = @_;
    return unless $server && defined $target && length $target;
    append(target_file($server->{tag}, $target), %entry);
}

sub sig_public {
    my ($server, $msg, $nick, $address, $target) = @_;
    return if ignored($server, $nick, $address, $target, $msg, MSGLEVEL_PUBLIC);
    log_target($server, $target, k => 'msg', n => $nick, h => $address, x => $msg, hl => mentions_me($server, $msg));
}

sub sig_own_public {
    my ($server, $msg, $target) = @_;
    log_target($server, $target, k => 'msg', n => $server->{nick}, x => $msg, s => 1);
}

sub sig_private {
    my ($server, $msg, $nick, $address) = @_;
    return if ignored($server, $nick, $address, $nick, $msg, MSGLEVEL_MSGS);
    log_target($server, $nick, k => 'msg', n => $nick, h => $address, x => $msg, hl => 1);
}

sub sig_own_private {
    my ($server, $msg, $target) = @_;
    log_target($server, $target, k => 'msg', n => $server->{nick}, x => $msg, s => 1);
}

sub sig_action {
    my ($server, $msg, $nick, $address, $target) = @_;
    my $public = is_channel($server, $target);
    return if ignored($server, $nick, $address, $public ? $target : $nick, $msg, MSGLEVEL_ACTIONS);
    log_target($server, $public ? $target : $nick, k => 'action', n => $nick, h => $address, x => $msg,
        hl => $public ? mentions_me($server, $msg) : 1);
}

sub sig_own_action {
    my ($server, $msg, $target) = @_;
    log_target($server, $target, k => 'action', n => $server->{nick}, x => $msg, s => 1);
}

# A target with a status prefix (@#channel, +#channel - a message only for
# ops or voiced users) goes to the channel's file.
sub channel_of {
    my ($server, $target) = @_;
    return $target if is_channel($server, $target);
    (my $bare = $target // '') =~ s/^[@+%~]+//;
    return length $bare && $bare ne $target && is_channel($server, $bare) ? $bare : undef;
}

# A channel NOTICE goes to the channel; a private one to an open query with
# the sender (erssi then shows it in that window); without a query it goes
# to the Notices window and into the journal as a text line.
sub sig_notice {
    my ($server, $msg, $nick, $address, $target) = @_;
    my $channel = channel_of($server, $target);
    my $where = $channel // (eval { $server->query_find($nick) } ? $nick : undef);
    return unless defined $where;
    return if ignored($server, $nick, $address, $where, $msg, MSGLEVEL_NOTICES);
    log_target($server, $where, k => 'notice', n => $nick, h => $address, x => $msg);
}

sub sig_own_notice {
    my ($server, $msg, $target) = @_;
    my $channel = channel_of($server, $target);
    my $where = $channel // (eval { $server->query_find($target) } ? $target : undef);
    return unless defined $where;
    log_target($server, $where, k => 'notice', n => $server->{nick}, x => $msg, s => 1);
}

# PRIVMSG @#channel: erssi emits it as "message irc op_public", not "message public".
sub sig_op_public {
    my ($server, $msg, $nick, $address, $target) = @_;
    my $channel = channel_of($server, $target) // return;
    return if ignored($server, $nick, $address, $channel, $msg, MSGLEVEL_PUBLIC);
    log_target($server, $channel, k => 'msg', n => $nick, h => $address, x => $msg, hl => mentions_me($server, $msg));
}

sub sig_join {
    my ($server, $channel, $nick, $address) = @_;
    log_target($server, $channel, k => 'join', n => $nick, h => $address, s => $nick eq ($server->{nick} // '') ? 1 : 0);
}

sub sig_part {
    my ($server, $channel, $nick, $address, $reason) = @_;
    log_target($server, $channel, k => 'part', n => $nick, h => $address, x => $reason);
}

sub sig_kick {
    my ($server, $channel, $nick, $kicker, $address, $reason) = @_;
    log_target($server, $channel, k => 'kick', n => $kicker, h => $address, tg => $nick, x => $reason);
}

sub sig_topic {
    my ($server, $channel, $topic, $nick, $address) = @_;
    log_target($server, $channel, k => 'topic', n => $nick, h => $address, x => $topic);
}

sub sig_mode {
    my ($server, $channel, $nick, $address, $mode) = @_;
    return unless is_channel($server, $channel);
    log_target($server, $channel, k => 'mode', n => $nick, h => $address, x => $mode);
}

# QUIT and NICK have no channel - they are written to every channel the
# nick is on and to an open query with it. The "message ..." signals carry
# text that is already recoded, as erssi shows it:
#   message quit - the nick list still has the leaving nick (fe-messages
#                  uses it too, to print the quit in the channels),
#   message nick - the nick list already has the NEW nick; the query may be
#                  under the new or the old one.
sub windows_with_nick {
    my ($server, @nicks) = @_;
    my %seen;
    my @targets;
    for my $channel ($server->channels) {
        next unless grep { eval { $channel->nick_find($_) } } @nicks;
        push @targets, $channel->{name} unless $seen{ lc $channel->{name} }++;
    }
    for my $nick (@nicks) {
        my $query = eval { $server->query_find($nick) } or next;
        push @targets, $query->{name} unless $seen{ lc $query->{name} }++;
    }
    return @targets;
}

sub sig_quit {
    my ($server, $nick, $address, $reason) = @_;
    log_target($server, $_, k => 'quit', n => $nick, h => $address, x => $reason) for windows_with_nick($server, $nick);
}

sub sig_nick {
    my ($server, $new, $old, $address) = @_;
    log_target($server, $_, k => 'nick', n => $old, h => $address, nn => $new) for windows_with_nick($server, $new, $old);
}

sub sig_own_nick {
    my ($server, $new, $old, $address) = @_;
    log_target($server, $_, k => 'nick', n => $old, h => $address, nn => $new, s => 1) for windows_with_nick($server, $new, $old);
}

# ── windows without a channel ────────────────────────────────────────

sub window_has_items {
    my ($window) = @_;
    my @items = eval { $window->items };
    return scalar @items;
}

sub sig_print_text {
    my ($dest, $text, $stripped) = @_;
    my $window = ref $dest ? $dest->{window} : undef;
    return unless $window;
    # MSGLEVEL_NEVER: lines for the screen only (e.g. the trackbar.pl line)
    return if ($dest->{level} // 0) & MSGLEVEL_NEVER;

    $stripped = Irssi::strip_codes($text // '') unless defined $stripped;
    # the theme's column indent (e.g. "        erssi │ ...") makes no sense on the web
    $stripped =~ s/^\s+//;
    $stripped =~ s/\s+$//;
    return unless length $stripped;
    my $hl = ($dest->{level} // 0) & MSGLEVEL_HILIGHT ? 1 : 0;

    if (window_has_items($window)) {
        # A channel/query window: messages go as structured entries, here
        # only client and script messages.
        return unless ($dest->{level} // 0) & (MSGLEVEL_CLIENTNOTICE | MSGLEVEL_CLIENTCRAP | MSGLEVEL_CLIENTERROR);
        my $server = $dest->{server};
        my $target = $dest->{target};
        unless ($server && defined $target && length $target) {
            my $item = $window->{active};
            $server = $item ? $item->{server} : undef;
            $target = $item ? $item->{name} : undef;
        }
        return unless $server && defined $target && length $target;
        log_target($server, $target, k => 'text', x => $stripped, hl => $hl);
        return;
    }

    my $name = $window->{name};
    return unless defined $name && length $name;

    my $server = Irssi::server_find_tag($name);
    my $file = $server && $server->{tag} eq $name ? status_file($name) : window_file($name);
    append($file, k => 'text', w => $name, x => $stripped, hl => $hl);
}

# ── command ──────────────────────────────────────────────────────────

sub cmd_webjournal {
    my $state = Irssi::settings_get_bool('webjournal') ? 'enabled' : 'DISABLED (/set webjournal on)';
    my $text = "webjournal $VERSION: $state, directory " . base_dir()
        . ", entries written: $stats{written}, rotations: $stats{rotated}, errors: $stats{errors}"
        . ($stats{errors} ? " (last: $stats{last_error})" : '');
    $text =~ s/%/%%/g;    # Irssi::print reads % as colour codes
    Irssi::print($text, MSGLEVEL_CLIENTCRAP);
}

Irssi::signal_add('message public',        \&sig_public);
Irssi::signal_add('message own_public',    \&sig_own_public);
Irssi::signal_add('message private',       \&sig_private);
Irssi::signal_add('message own_private',   \&sig_own_private);
Irssi::signal_add('message irc action',    \&sig_action);
Irssi::signal_add('message irc own_action', \&sig_own_action);
Irssi::signal_add('message irc notice',    \&sig_notice);
Irssi::signal_add('message irc own_notice', \&sig_own_notice);
# -150: always before fe-netjoin, which stops these for netsplits and netjoins
Irssi::signal_add_priority('message join',     \&sig_join, -150);
Irssi::signal_add('message part',          \&sig_part);
Irssi::signal_add('message kick',          \&sig_kick);
Irssi::signal_add('message topic',         \&sig_topic);
Irssi::signal_add_priority('message irc mode', \&sig_mode, -150);
Irssi::signal_add('message irc op_public', \&sig_op_public);
Irssi::signal_add_priority('message quit',     \&sig_quit, -150);
Irssi::signal_add('message nick',          \&sig_nick);
Irssi::signal_add('message own_nick',      \&sig_own_nick);
Irssi::signal_add('print text',            \&sig_print_text);
Irssi::command_bind('webjournal',          \&cmd_webjournal);
