# mentions.pl - okno "Mentions": wszystko, co dotyczy Twojego nicka
#
# Odpowiednik bufora Mentions z repartee. Do okna trafiaja:
#   * wiadomosci i /me na kanalach, ktore erssi oznaczylo jako podswietlenie
#     (Twoj nick gdziekolwiek w zdaniu przy hilight_nick_matches_everywhere ON,
#     albo dowolna regula /hilight),
#   * wiadomosci prywatne, prywatne NOTICE od ludzi, wiadomosci DCC CHAT
#     (bez partyline botnetow z botnet.pl i bez tego, co lapie /ignore).
# Kazdy wpis jest tez dopisywany do pliku (mentions_log_file), wiec to, co
# pisano do Ciebie pod Twoja nieobecnosc, przetrwa restart erssi.
#
# Nick i tresc sa brane z sygnalu wiadomosci, a nie wycinane z gotowej linii
# - dzieki temu dziala z kazdym motywem (kolumna nickow, separatory).
#
# Ustawienia: mentions_window ("Mentions"), mentions_log (ON),
#             mentions_log_file ("~/.erssi/logs/mentions.log")
# Komenda:    /mentions        - przejdz do okna
#             /mentions clear  - wyczysc okno

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

# Ostatnie wiadomosci kanalowe (nick + tresc), dopasowywane potem do
# wypisanej linii. erssi wypisuje linie (sygnal "print text") dopiero PO
# zakonczeniu "message public" - sprawdzone sonda na zywo - wiec zwykla
# zmienna "biezacej wiadomosci" juz by nie istniala. Kolejka per kanal,
# najwyzej 50 wpisow i 30 s.
my %recent;    # lc("$tag\0$kanal") => [ { kind, tag, target, nick, msg, at } ]
my $RECENT_MAX = 50;
my $RECENT_TTL = 30;

# ── okno ─────────────────────────────────────────────────────────────

sub window_name { Irssi::settings_get_str('mentions_window') || 'Mentions' }

# Nowe okno rozpoznajemy po wskazniku (_irssi): erssi aktywuje okno
# "hidden", a chansort przenumerowuje okna w chwili ich tworzenia.
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
    # okno stale: /window close go nie zamknie (zdejmuje: /window immortal off)
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

# ── zapis ────────────────────────────────────────────────────────────

sub log_file {
    my $path = Irssi::settings_get_str('mentions_log_file');
    $path =~ s/\A~/$ENV{HOME}/;
    return $path;
}

sub to_log {
    my ($tag, $where, $nick, $text) = @_;
    return unless Irssi::settings_get_bool('mentions_log');
    my $path = log_file();
    # katalog 0700 i plik od razu 0600 (sysopen) - bez chwili, w ktorej
    # dziennik prywatnych wiadomosci jest czytelny dla innych
    my ($dir) = $path =~ m{\A(.*)/};
    make_path($dir, { mode => 0700 }) if defined $dir && length $dir && !-d $dir;
    sysopen(my $fh, $path, O_WRONLY | O_APPEND | O_CREAT, 0600) or return;
    chmod 0600, $path;
    # irssi oddaje skryptom bajty UTF-8 - zapis bez warstwy kodowania,
    # inaczej polskie znaki bylyby zakodowane drugi raz.
    binmode($fh, ':raw');
    printf {$fh} "%s [%s] %s <%s> %s\n", strftime('%Y-%m-%d %H:%M:%S', localtime), $tag // '-',
        $where // '-', $nick // '?', $text // '';
    close $fh;
}

# Poziom HILIGHT: wpis zaznacza okno w panelu kolorem wzmianki.
sub record {
    my ($format, $tag, $where, $nick, $text) = @_;
    $text = '' unless defined $text;
    $text =~ s/\A\s+|\s+\z//g;
    window()->printformat(MSGLEVEL_HILIGHT, $format, $where // '', $nick // '?', $text);
    to_log($tag, $where, $nick, $text);
}

# ── sygnaly ──────────────────────────────────────────────────────────

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

# Nick w czesci linii przed trescia: caly albo przyciety przez kolumne
# nickow erssi (nick_column_width: poczatek nicka + "+").
sub nick_in_prefix {
    my ($prefix, $nick) = @_;
    return 0 unless defined $nick && length $nick;
    return 1 if index($prefix, $nick) >= 0;
    for my $keep (reverse 1 .. length($nick) - 1) {
        return 1 if index($prefix, substr($nick, 0, $keep) . '+') >= 0;
    }
    return 0;
}

# Wiadomosc z kolejki, ktora jest KONCEM wypisanej linii, a jej nadawca
# stoi w linii przed trescia. Od najnowszej: tresc zawarta w srodku
# linii albo taka sama tresc od kogos innego nie przypisze zdania
# niewlasciwej osobie.
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
    # tylko kanaly - PM, prywatne /me i NOTICE maja wlasne handlery
    return unless $target =~ /\A[#&!+]/;
    my $tag = $dest->{server} ? $dest->{server}{tag} : undef;
    my $line = $stripped // $text // '';
    if (my $msg = take_recent($tag, $target, $line)) {
        my $format = $msg->{kind} eq 'action' ? 'mentions_action' : 'mentions_public';
        record($format, $msg->{tag}, $target, $msg->{nick}, Irssi::strip_codes($msg->{msg}));
        return;
    }
    # Podswietlenie spoza wiadomosci (np. inny skrypt) - tekst linii bez nicka.
    record('mentions_public', $tag, $target, '?', $line);
}

# Partyline botnetu (botnet.pl): hub mowi jak serwer IRC, a cala
# partyline przychodzi jako rozmowa - to nie sa wzmianki. Botnety bierzemy
# z ustawien botnet.pl tylko, gdy jest zaladowany (odczyt ustawienia,
# ktorego nikt nie zarejestrowal, erssi zglasza jako blad).
sub is_botnet {
    my ($server) = @_;
    return 0 unless $server && Irssi::Script::botnet->can('partyline_nick');
    my $chatnet = lc($server->{chatnet} // '');
    return 0 unless length $chatnet;
    return (grep { lc $_ eq $chatnet } split /[\s,]+/, Irssi::settings_get_str('botnet_chatnets')) ? 1 : 0;
}

# /ignore: PRIVMSG filtruje samo erssi (zatrzymuje "message private" przed
# skryptami), NOTICE i /me - nie, wiec sprawdzamy sami.
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

# Prywatne /me (cel = nasz nick) - kanalowe obsluguje sig_print_text.
sub sig_private_action {
    my ($server, $msg, $nick, $address, $target) = @_;
    return unless $server && defined $target && lc $target eq lc($server->{nick} // '');
    return if is_botnet($server);
    return if ignored($server, $nick, $address, $target, $msg, MSGLEVEL_ACTIONS);
    record('mentions_action', $server->{tag}, 'PM', $nick, $msg);
}

sub sig_notice {
    my ($server, $msg, $nick, $address, $target) = @_;
    return if !defined $nick || !length $nick || index($nick, '.') >= 0;   # serwery
    return if defined $target && $target =~ /\A[#&!+]/;                     # NOTICE na kanal
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

# Okno tworzymy dopiero po starcie erssi: przy "irssi init finished" erssi
# nadaje oknu nr 1 nazwe "Notices", a wbudowane sortowanie moze wczesniej
# postawic nasze okno na pozycji 1 - nazwa "Mentions" zostalaby nadpisana.
# Timer odpala sie juz w petli glownej (takze przy recznym /script load).
Irssi::timeout_add_once(10, sub { window() }, undef);

# Gdyby okno jednak zniknelo (np. /window immortal off + /window close),
# powstaje od nowa - wzmianki nie gina w pustce.
Irssi::signal_add('window destroyed', sub {
    my ($destroyed) = @_;
    return unless lc($destroyed->{name} // '') eq lc window_name();
    Irssi::timeout_add_once(10, sub { window() }, undef);
});

1;
