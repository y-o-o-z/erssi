# skaner.pl - klony i IRCopi na kanale, raport w osobnym oknie "skaner"
#
# Odpowiednik skaner.lua z repartee_scripts. W irssi/erssi dane sa pelne:
# po wejsciu na kanal klient robi WHO, wiec kazdy nick ma ident@host i
# flage IRC operatora ('*' z WHO -> $nick->{serverop}). Raport powstaje po
# sygnale "channel sync", czyli gdy irssi skonczy WHO i liste trybow.
#
# W oknie "skaner":
#   * klony - osoby z tym samym hostem,
#   * IRCopi obecni na kanale,
#   * alert, gdy na kanal wchodzi klon kogos, kto juz na nim jest.
# Odpowiedniki z scripts.irssi.org: clones.pl, clones_scanner.pl, ircops.pl.
#
# Komendy:
#   /skaner             raport dla biezacego kanalu
#   /skaner #kanal      raport dla kanalu na biezacym serwerze
#   /skaner all         wszystkie kanaly biezacego serwera
#   /skaner on | off    automatyczne raporty i alerty
#   /skaner status | help
#
# Ustawienia: skaner_enabled (ON), skaner_show_clean (ON - jedna linia
# takze dla kanalow bez znalezisk), skaner_window ("skaner").

use strict;
use warnings;

use Irssi;
use Irssi::Irc;

our $VERSION = '1.0.2';
our %IRSSI = (
    authors     => 'yooz',
    contact     => 'https://github.com/y-o-o-z',
    name        => 'skaner',
    description => 'Clones (same host) and IRC operators on a channel - report in the "skaner" window',
    license     => 'MIT',
    url         => 'https://github.com/y-o-o-z/irssi_scripts',
);

my $NICKS_SHOWN = 12;    # ile nickow wypisac w jednej grupie klonow

Irssi::settings_add_bool('skaner', 'skaner_enabled',    1);
Irssi::settings_add_bool('skaner', 'skaner_show_clean', 1);
Irssi::settings_add_str('skaner',  'skaner_window',     'skaner');

Irssi::theme_register([
    'skaner_header',     '%K───%n {channel $0} %K·%n $1 %K·%n users: $2 %K───%n',
    'skaner_clean',      '{channel $0} %K·%n users: $1 %K·%n clones: none %K·%n IRCops: none',
    'skaner_clones',     '  {hilight clones}: $1 users, groups: $0',
    'skaner_clone',      '    %Yx$0%n $1 %K—%n $2',
    'skaner_no_clones',  '  clones: none',
    'skaner_opers',      '  {error IRCops} ($0): $1',
    'skaner_no_opers',   '  IRCops: none',
    'skaner_no_host',    '  no known host: $0 of $1 {comment WHO skipped - channel_max_who_sync?}',
    'skaner_alert',      '%Yclone%n {channel $0}: {nick $1} {nickhost $2} %K—%n already here: $3',
    'skaner_info',       '$0',
    'skaner_title',      '%_$0%_ $1',
    'skaner_empty',      '{channel $0} - no user list yet (the channel is still syncing)',
]);

# ── narzedzia ────────────────────────────────────────────────────────

sub irc_lc {
    my ($s) = @_;
    $s = lc($s // '');
    $s =~ tr/[]\\~/{}|^/;
    return $s;
}

sub host_part {
    my ($address) = @_;
    return undef unless defined $address && length $address;
    my $at = rindex($address, '@');
    my $host = $at >= 0 ? substr($address, $at + 1) : $address;
    return length $host ? $host : undef;
}

sub prefixed {
    my ($nick) = @_;
    my $prefix = $nick->{op} ? '@' : $nick->{halfop} ? '%' : $nick->{voice} ? '+' : '';
    return $prefix . $nick->{nick};
}

# Okno "skaner": szukane po nazwie, tworzone w tle bez zmiany aktywnego
# okna. Powstaje juz przy zaladowaniu skryptu, jak bufor *skaner* w repartee.
sub window {
    my $name = Irssi::settings_get_str('skaner_window') || 'skaner';
    my $window = Irssi::window_find_name($name);
    return $window if $window;
    # Nowe okno rozpoznajemy po wskazniku (_irssi), nie po numerze ani po
    # aktywnym oknie: erssi aktywuje okno "hidden", a skrypty sortujace
    # (chansort) przenumerowuja okna w chwili ich tworzenia.
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

sub out {
    my ($format, @args) = @_;
    window()->printformat(MSGLEVEL_CLIENTNOTICE, $format, @args);
}

sub alert {
    my (@args) = @_;
    # Alert o klonie zaznacza aktywnosc okna (poziom NOTICES).
    window()->printformat(MSGLEVEL_NOTICES, 'skaner_alert', @args);
}

sub nick_list {
    my (@nicks) = @_;
    my @names = map { prefixed($_) } @nicks[0 .. ($#nicks < $NICKS_SHOWN - 1 ? $#nicks : $NICKS_SHOWN - 1)];
    push @names, '+' . (@nicks - $NICKS_SHOWN) if @nicks > $NICKS_SHOWN;
    return join(', ', @names);
}

# ── analiza ──────────────────────────────────────────────────────────

sub analyse {
    my ($channel) = @_;
    my @nicks = sort { irc_lc($a->{nick}) cmp irc_lc($b->{nick}) } $channel->nicks();
    my (%groups, @order, @opers);
    my $no_host = 0;
    for my $nick (@nicks) {
        push @opers, $nick if $nick->{serverop};
        my $host = host_part($nick->{host});
        if (!defined $host) {
            $no_host++;
            next;
        }
        my $key = irc_lc($host);
        push @order, $key unless $groups{$key};
        $groups{$key} ||= { host => $host, nicks => [] };
        push @{ $groups{$key}{nicks} }, $nick;
    }
    my @clones = sort {
        @{ $b->{nicks} } <=> @{ $a->{nicks} } || irc_lc($a->{host}) cmp irc_lc($b->{host})
    } grep { @{ $_->{nicks} } > 1 } map { $groups{$_} } @order;
    return { total => scalar @nicks, clones => \@clones, opers => \@opers, no_host => $no_host };
}

# $auto: raport po wejsciu (kanal bez znalezisk = jedna linia).
sub report {
    my ($channel, $auto) = @_;
    my $result = analyse($channel);
    my $name = $channel->{name};
    my $net  = $channel->{server}{chatnet} || $channel->{server}{tag};

    if (!$result->{total}) {
        out('skaner_empty', $name);
        return;
    }
    if ($auto && !@{ $result->{clones} } && !@{ $result->{opers} } && !$result->{no_host}) {
        out('skaner_clean', $name, $net, $result->{total}) if Irssi::settings_get_bool('skaner_show_clean');
        return;
    }

    out('skaner_header', $name, $net, $result->{total});
    if (@{ $result->{clones} }) {
        my $people = 0;
        $people += @{ $_->{nicks} } for @{ $result->{clones} };
        out('skaner_clones', scalar @{ $result->{clones} }, $people);
        out('skaner_clone', scalar @{ $_->{nicks} }, $_->{host}, nick_list(@{ $_->{nicks} }))
            for @{ $result->{clones} };
    } else {
        out('skaner_no_clones');
    }
    if (@{ $result->{opers} }) {
        my @shown = map { prefixed($_) . ($_->{host} ? " ($_->{host})" : '') } @{ $result->{opers} };
        out('skaner_opers', scalar @shown, join(', ', @shown));
    } else {
        out('skaner_no_opers');
    }
    out('skaner_no_host', $result->{no_host}, $result->{total}) if $result->{no_host};
}

# ── indeks hostow ────────────────────────────────────────────────────

# Alert przy wejsciu pyta "kto jeszcze ma ten host". Przegladanie calej
# listy nickow przy kazdym JOIN (zalew wejsc na duzym kanale = tysiace
# porownan na kazde wejscie) zastepuje indeks na kanal: host -> nicki,
# budowany przy synchronizacji (albo przy pierwszym wejsciu po /script
# load). Poprawiaja go sygnaly listy nickow erssi (nicklist new / remove /
# changed / host changed), a nie "message part/quit/nick" - te /ignore
# zatrzymuje przed skryptami i indeks rozjechalby sie z kanalem.
# Przy wylaczonym skanerze indeksu nie ma - powstaje od nowa po wlaczeniu.
my %index;    # "tag\0kanal" => { hosts => { host_lc => { nick_lc => nick } }, nicks => { nick_lc => host_lc } }

sub index_key { ($_[0] // '') . "\0" . irc_lc($_[1]) }

sub index_remove {
    my ($idx, $nick) = @_;
    my $n = irc_lc($nick);
    my $host = delete $idx->{nicks}{$n} // return;
    delete $idx->{hosts}{$host}{$n};
    delete $idx->{hosts}{$host} unless %{ $idx->{hosts}{$host} };
}

sub index_add {
    my ($idx, $nick, $address) = @_;
    index_remove($idx, $nick);
    my $host = host_part($address) // return;
    my ($n, $h) = (irc_lc($nick), irc_lc($host));
    $idx->{hosts}{$h}{$n} = $nick;
    $idx->{nicks}{$n} = $h;
}

sub index_build {
    my ($channel) = @_;
    my $idx = { hosts => {}, nicks => {} };
    index_add($idx, $_->{nick}, $_->{host}) for $channel->nicks();
    return $index{ index_key($channel->{server}{tag}, $channel->{name}) } = $idx;
}

sub index_find {
    my ($server, $channel_name) = @_;
    return $index{ index_key($server->{tag}, $channel_name) };
}

sub enabled {
    return 1 if Irssi::settings_get_bool('skaner_enabled');
    %index = ();
    return 0;
}

# ── sygnaly ──────────────────────────────────────────────────────────

# Koniec synchronizacji kanalu po naszym wejsciu: hosty i flagi z WHO sa juz.
sub sig_channel_sync {
    my ($channel) = @_;
    return unless enabled();
    index_build($channel);
    report($channel, 1);
}

sub sig_message_join {
    my ($server, $channel_name, $nick, $address) = @_;
    return unless enabled();
    return if irc_lc($nick) eq irc_lc($server->{nick});
    my $channel = $server->channel_find($channel_name) or return;
    return unless $channel->{synced};
    my $idx = index_find($server, $channel_name) // index_build($channel);
    my $host = host_part($address);
    my %same = defined $host ? %{ $idx->{hosts}{ irc_lc($host) } || {} } : ();
    delete $same{ irc_lc($nick) };
    index_add($idx, $nick, $address);
    return unless %same;
    # obiekty nickow (prefiksy @ % +) tylko dla tych kilku z tym samym hostem
    my @same = sort { irc_lc($a->{nick}) cmp irc_lc($b->{nick}) }
        grep { defined } map { $channel->nick_find($_) } values %same;
    return unless @same;
    alert($channel->{name}, $nick, $address, nick_list(@same));
}

# Sygnaly listy nickow: (kanal, nick[, stary nick]). Indeks istnieje tylko
# dla zsynchronizowanych kanalow - w trakcie WHO nic nie robimy.
sub channel_index {
    my ($channel) = @_;
    return unless $channel && $channel->{server};
    return index_find($channel->{server}, $channel->{name});
}

sub sig_nicklist_new {
    my ($channel, $nick) = @_;
    my $idx = channel_index($channel) or return;
    index_add($idx, $nick->{nick}, $nick->{host});
}

sub sig_nicklist_remove {
    my ($channel, $nick) = @_;
    my $idx = channel_index($channel) or return;
    index_remove($idx, $nick->{nick});
}

sub sig_nicklist_changed {
    my ($channel, $nick, $oldnick) = @_;
    my $idx = channel_index($channel) or return;
    index_remove($idx, $oldnick);
    index_add($idx, $nick->{nick}, $nick->{host});
}

sub sig_channel_destroyed {
    my ($channel) = @_;
    return unless $channel && $channel->{server};
    delete $index{ index_key($channel->{server}{tag}, $channel->{name}) };
}

# ── komenda ──────────────────────────────────────────────────────────

sub help {
    out('skaner_title', 'skaner', '- clones and IRC operators on a channel');
    out('skaner_info', '  /skaner             report for the current channel');
    out('skaner_info', '  /skaner #channel    report for a channel on this server');
    out('skaner_info', '  /skaner all         all channels of this server');
    out('skaner_info', '  /skaner on | off    automatic reports and clone alerts');
    out('skaner_info', '  /skaner status | help');
}

sub cmd_skaner {
    my ($data, $server, $witem) = @_;
    my ($arg) = split / +/, ($data // '');
    $arg //= '';
    my $sub = lc $arg;

    if ($sub eq 'on' || $sub eq 'off') {
        Irssi::settings_set_bool('skaner_enabled', $sub eq 'on' ? 1 : 0);
        out('skaner_info', 'automatic reports: ' . ($sub eq 'on' ? 'on' : 'off'));
        return;
    }
    if ($sub eq 'status') {
        my $count = 0;
        for my $srv (Irssi::servers()) {
            my @channels = $srv->channels();
            $count += @channels;
        }
        out('skaner_title', "skaner v$VERSION", sprintf('· %s · channels: %d',
            Irssi::settings_get_bool('skaner_enabled') ? 'enabled' : 'disabled', $count));
        return;
    }
    if ($sub eq 'help') {
        help();
        return;
    }
    if (!$server) {
        out('skaner_info', 'no active server - switch to an IRC network window');
        return;
    }
    if ($sub eq 'all') {
        my @channels = sort { irc_lc($a->{name}) cmp irc_lc($b->{name}) } $server->channels();
        out('skaner_title', 'report:', scalar(@channels) . (@channels == 1 ? ' channel' : ' channels'));
        report($_, 0) for @channels;
        return;
    }
    my $channel;
    if (length $arg) {
        $channel = $server->channel_find($arg);
        if (!$channel) {
            out('skaner_info', "you are not on channel $arg");
            return;
        }
    } elsif ($witem && $witem->{type} eq 'CHANNEL') {
        $channel = $witem;
    } else {
        out('skaner_info', 'specify a channel: /skaner #channel (or run the command in a channel window)');
        return;
    }
    report($channel, 0);
}

Irssi::signal_add_last('channel sync', \&sig_channel_sync);
Irssi::signal_add_last('message join', \&sig_message_join);
Irssi::signal_add('nicklist new', \&sig_nicklist_new);
Irssi::signal_add('nicklist host changed', \&sig_nicklist_new);
Irssi::signal_add('nicklist remove', \&sig_nicklist_remove);
Irssi::signal_add('nicklist changed', \&sig_nicklist_changed);
Irssi::signal_add('channel destroyed', \&sig_channel_destroyed);
Irssi::command_bind('skaner', \&cmd_skaner);

# Okno tworzymy dopiero po starcie erssi: przy "irssi init finished" erssi
# nadaje oknu nr 1 nazwe "Notices", a wbudowane sortowanie moze wczesniej
# postawic nasze okno na pozycji 1 - nazwa "skaner" zostalaby nadpisana.
# Timer odpala sie juz w petli glownej (takze przy recznym /script load).
Irssi::timeout_add_once(10, sub { window() }, undef);

# Gdyby okno jednak zniknelo (np. /window immortal off + /window close),
# powstaje od nowa - wzmianki nie gina w pustce.
Irssi::signal_add('window destroyed', sub {
    my ($destroyed) = @_;
    return unless lc($destroyed->{name} // '') eq lc (Irssi::settings_get_str('skaner_window') || 'skaner');
    Irssi::timeout_add_once(10, sub { window() }, undef);
});

1;
