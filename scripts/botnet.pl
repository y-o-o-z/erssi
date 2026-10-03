# botnet.pl - partyline botnetow (psotnic / pt-pojeby / eggdrop z interfejsem
# IRC) w erssi: jedno polaczenie i jedno okno na botnet
#
# Botnet to w configu zwykla siec IRC (chatnet, np. IRCnetBot) - hub mowi
# protokolem IRC, a cala partyline przychodzi jako rozmowa z nickiem
# "partyline". Bez tego skryptu erssi:
#   * przy kazdym /connect dokladalo KOLEJNE polaczenie do tego samego hubu
#     (IRCnetBot, IRCnetBot2, IRCnetBot3...), kazde z wlasnym oknem sieci
#     i rozmowa partyline - kazda komenda szla wtedy kilka razy,
#   * po zamknieciu okna partyline zostawalo polaczone, wiec nastepna
#     linia od bota otwierala nowe okno (i z window_auto_change zabierala
#     fokus),
#   * gromadzilo zdublowane ponowne polaczenia do botnetow, ktore nie
#     odpowiadaja, i ponawialo je w nieskonczonosc co server_reconnect_time.
#
# Ze skryptem:
#   * jeden botnet = jedno polaczenie: /connect do botnetu, ktory juz dziala
#     albo sie laczy, przelacza na jego okno zamiast laczyc drugi raz
#     (dodatkowe polaczenie, ktore i tak powstanie - np. z ponowienia - jest
#     zamykane od razu),
#   * jeden botnet = jedno okno: okno sieci (z panelu erssi, nazwa = tag)
#     od razu dostaje rozmowe partyline, wiec linie od bota nie otwieraja
#     nowych okien; po zerwaniu i ponownym polaczeniu to samo okno,
#   * zamkniecie okna partyline (/wc, krzyzyk w webie) = rozlaczenie botnetu
#     bez ponawiania,
#   * najwyzej jedno ponowne polaczenie na botnet; po botnet_reconnect_tries
#     nieudanych probach z rzedu skrypt je usuwa i pisze o tym raz,
#   * po zaladowaniu porzadkuje to, co juz jest (nadmiarowe polaczenia,
#     ich okna, zdublowane ponowienia).
#
# Komendy:
#   /bot                     stan botnetow
#   /bot <botnet>            polacz albo przejdz do okna (nazwa bez
#                            rozrozniania wielkosci liter, wystarczy poczatek)
#   /bot close <botnet>      rozlacz, bez ponawiania, zamknij okno
#   /bot cleanup             uporzadkuj teraz (robione tez automatycznie)
#   /bot help
#
# Ustawienia:
#   botnet_chatnets          chatnety botnetow, oddzielone spacja
#                            (domyslnie: IRCnetBot Botnet PT_botnet PTbotZ)
#   botnet_partyline_nick    nick partyline w hubie (partyline)
#   botnet_reconnect_tries   ile nieudanych prob ponowienia z rzedu (3, 0 = bez
#                            limitu)
#   botnet_quit_message      powod przy rozlaczaniu (pusty = domyslny erssi)

use strict;
use warnings;

use Irssi;

our $VERSION = '1.0.0';
our %IRSSI = (
    authors     => 'yooz',
    contact     => 'https://github.com/y-o-o-z',
    name        => 'botnet',
    description => 'Partyline botnetow: jedno polaczenie i jedno okno na botnet, zamkniecie okna = rozlaczenie',
    license     => 'MIT',
    url         => 'https://github.com/y-o-o-z/irssi_scripts',
);

Irssi::settings_add_str('botnet', 'botnet_chatnets',        'IRCnetBot Botnet PT_botnet PTbotZ');
Irssi::settings_add_str('botnet', 'botnet_partyline_nick',  'partyline');
Irssi::settings_add_int('botnet', 'botnet_reconnect_tries', 3);
Irssi::settings_add_str('botnet', 'botnet_quit_message',    '');

my %failures;      # lc chatnet -> nieudane proby z rzedu
my %connecting;    # tag -> lc chatnet (polaczenia w toku, jeszcze nie w Irssi::servers)
my %closing;       # tag -> 1: rozlaczamy sami, zamykanie rozmowy nie jest sygnalem uzytkownika
my $quitting = 0;

# ── botnety z ustawien i configu ─────────────────────────────────────

sub botnets {
    my %set;
    $set{ lc $_ } = $_ for grep { length } split /[\s,]+/, Irssi::settings_get_str('botnet_chatnets');
    return %set;    # lc -> nazwa jak w ustawieniu
}

sub botnet_of_chatnet {
    my ($chatnet) = @_;
    return undef unless defined $chatnet && length $chatnet;
    my %set = botnets();
    return exists $set{ lc $chatnet } ? lc $chatnet : undef;
}

sub partyline_nick { Irssi::settings_get_str('botnet_partyline_nick') || 'partyline' }

sub server_botnet {
    my ($server) = @_;
    return undef unless $server;
    return botnet_of_chatnet($server->{chatnet});
}

# adres:port -> botnet, z sekcji servers{} configu (do /connect <adres>)
sub botnet_of_address {
    my ($address, $port) = @_;
    my $file = Irssi::get_irssi_config();
    open(my $fh, '<', $file) or return undef;
    local $/;
    my $cfg = <$fh>;
    close $fh;
    while ($cfg =~ /\{([^{}]*?)\}/gs) {
        my $block = $1;
        my ($addr) = $block =~ /\baddress\s*=\s*"([^"]*)"/;
        my ($net)  = $block =~ /\bchatnet\s*=\s*"([^"]*)"/;
        my ($p)    = $block =~ /\bport\s*=\s*"?(\d+)"?/;
        next unless defined $addr && defined $net && lc $addr eq lc $address;
        next if defined $port && length $port && defined $p && $p ne $port;
        my $b = botnet_of_chatnet($net);
        return $b if $b;
    }
    return undef;
}

sub display_name {
    my ($botnet) = @_;
    my %set = botnets();
    return $set{$botnet} // $botnet;
}

# polaczenia botnetu: [polaczone serwery (najstarsze pierwsze)], [tagi w toku]
sub live_servers {
    my ($botnet) = @_;
    my @srv = sort { ($a->{connect_time} // 0) <=> ($b->{connect_time} // 0) }
              grep { (server_botnet($_) // '') eq $botnet } Irssi::servers();
    my @pending = grep { $connecting{$_} eq $botnet && !Irssi::server_find_tag($_) } keys %connecting;
    return (\@srv, \@pending);
}

sub reconnects_of {
    my ($botnet) = @_;
    return sort { ($a->{next_connect} // 0) <=> ($b->{next_connect} // 0) }
           grep { (botnet_of_chatnet($_->{chatnet}) // '') eq $botnet } Irssi::reconnects();
}

# zdarzenia w tle (rozlaczenia, wstrzymane ponowienia) - okno Notices
sub say_info {
    my ($text) = @_;
    Irssi::print("botnet: $text", MSGLEVEL_CLIENTNOTICE);
}

# odpowiedzi na komendy - tam, gdzie je wpisano
sub say_here {
    my ($text) = @_;
    Irssi::active_win()->print($text, MSGLEVEL_CLIENTCRAP);
}

# ── okno botnetu ─────────────────────────────────────────────────────

sub find_query {
    my ($server) = @_;
    return $server->query_find(partyline_nick());
}

sub window_of {
    my ($server) = @_;
    my $botnet = server_botnet($server);
    # okno sieci z panelu erssi (nazwa = tag) albo okno nazwane jak botnet
    for my $name ($server->{tag}, display_name($botnet)) {
        my $win = Irssi::window_find_name($name);
        return $win if $win;
    }
    my $query = find_query($server);
    return $query ? $query->window() : undef;
}

# Rozmowa partyline w oknie botnetu, zanim przyjdzie pierwsza linia od bota -
# inaczej erssi otwiera dla niej nowe okno.
sub ensure_window {
    my ($server) = @_;
    return unless $server && $server->{connected} && server_botnet($server);
    my $win = window_of($server);
    my $query = find_query($server);
    my $nick = partyline_nick();

    if (!$win) {
        # bez okna sieci (panele wylaczone): okno o nazwie botnetu
        my $prev = Irssi::active_win();
        Irssi::command('window new hidden');
        $win = Irssi::active_win();
        $win->set_name(display_name(server_botnet($server)));
        $win->change_server($server);
        $prev->set_active() if $prev && $prev->{refnum} != $win->{refnum};
    }

    # okno bez nazwy (np. sama rozmowa sprzed skryptu) dostaje nazwe botnetu
    my $label = display_name(server_botnet($server));
    $win->set_name($label) if !defined $win->{name} && !Irssi::window_find_name($label);

    if (!$query) {
        $win->command("query -window $nick");
        return;
    }
    my $qwin = $query->window();
    return if $qwin && $qwin->{refnum} == $win->{refnum};
    # rozmowa w innym oknie (np. sprzed skryptu): przenies, puste okno zamknij
    $win->command("window item move $nick");
    $qwin->destroy() if $qwin && !$qwin->items() && !defined $qwin->{name};
}

# ── porzadki ─────────────────────────────────────────────────────────

sub close_window_of_tag {
    my ($tag) = @_;
    my $win = Irssi::window_find_name($tag);
    $win->destroy() if $win && !$win->items();
}

sub disconnect {
    my ($server, $reason) = @_;
    my $tag = $server->{tag};
    $closing{$tag} = 1;
    my $msg = Irssi::settings_get_str('botnet_quit_message');
    Irssi::command("disconnect $tag" . (length $msg ? " $msg" : ''));
    # rozmowa partyline zostaje bez serwera - zamknij ja i jej okno
    for my $q (Irssi::queries()) {
        next unless lc($q->{name}) eq lc partyline_nick() && ($q->{server_tag} // '') eq $tag;
        my $w = $q->window();
        $q->destroy();
        $w->destroy() if $w && !$w->items() && !defined $w->{name};
    }
    close_window_of_tag($tag);
    delete $closing{$tag};
}

sub remove_reconnect {
    my ($rec) = @_;
    Irssi::command("disconnect RECON-$rec->{tag}");
}

# jedno polaczenie i najwyzej jedno ponowienie na botnet
sub cleanup {
    my ($quiet) = @_;
    my %set = botnets();
    my $changed = 0;
    for my $botnet (sort keys %set) {
        my ($srv, $pending) = live_servers($botnet);
        my @srv = @$srv;
        if (@srv > 1) {
            my ($keep, @extra) = @srv;
            disconnect($_) for @extra;
            say_info(sprintf('%s: zamknieto %d nadmiarowe polaczenie(a) (%s), zostaje %s',
                display_name($botnet), scalar @extra, join(', ', map { $_->{tag} } @extra), $keep->{tag}));
            $changed = 1;
        }
        ensure_window($srv[0]) if @srv;

        my @rec = reconnects_of($botnet);
        my @drop = (@srv || @$pending) ? @rec : @rec[1 .. $#rec];
        if (@drop) {
            remove_reconnect($_) for @drop;
            say_info(sprintf('%s: usunieto %d zdublowane ponowienie(a)', display_name($botnet), scalar @drop))
                unless $quiet;
            $changed = 1;
        }
    }
    return $changed;
}

# ── sygnaly ──────────────────────────────────────────────────────────

# /connect do botnetu, ktory juz dziala: przejdz do jego okna
sub cmd_connect {
    my ($args) = @_;
    my @words = grep { !/^-/ } split ' ', ($args // '');
    return unless @words;
    my $botnet = botnet_of_chatnet($words[0]) // botnet_of_address($words[0], $words[1]);
    return unless $botnet;

    my ($srv, $pending) = live_servers($botnet);
    if (@$srv) {
        Irssi::signal_stop();
        ensure_window($srv->[0]);
        my $win = window_of($srv->[0]);
        $win->set_active() if $win;
        say_here(display_name($botnet) . " jest juz polaczony ($srv->[0]{tag}) - bez drugiego polaczenia");
        return;
    }
    if (@$pending) {
        Irssi::signal_stop();
        say_here(display_name($botnet) . ' juz sie laczy - poczekaj');
        return;
    }
    # reczne polaczenie zastepuje czekajace ponowienia
    remove_reconnect($_) for reconnects_of($botnet);
    delete $failures{$botnet};
}

sub sig_server_looking {
    my ($server) = @_;
    my $botnet = server_botnet($server) or return;
    $connecting{ $server->{tag} } = $botnet;
}

sub sig_server_connected {
    my ($server) = @_;
    my $botnet = server_botnet($server) or return;
    delete $connecting{ $server->{tag} };
    delete $failures{$botnet};

    my ($srv) = live_servers($botnet);
    my @older = grep { $_->{tag} ne $server->{tag} } @$srv;
    if (@older) {
        # drugie polaczenie (np. ponowienie, gdy pierwsze jeszcze zylo) - zbedne
        say_info(display_name($botnet) . ": juz polaczony jako $older[0]{tag}, zamykam $server->{tag}");
        disconnect($server);
        return;
    }
    remove_reconnect($_) for reconnects_of($botnet);
}

# Po powitaniu (001): serwer jest zarejestrowany, a pierwsza linia od bota
# dopiero przyjdzie. Na "server connected" (samo gniazdo) jeszcze za wczesnie.
sub sig_event_connected {
    my ($server) = @_;
    ensure_window($server) if server_botnet($server);
}

sub sig_connect_failed {
    my ($server) = @_;
    my $botnet = server_botnet($server) or return;
    delete $connecting{ $server->{tag} };
    my $tries = Irssi::settings_get_int('botnet_reconnect_tries');
    $failures{$botnet}++;
    # ponowienie zapisuje sie po tym sygnale - sprawdz za chwile
    Irssi::timeout_add_once(100, sub {
        my @rec = reconnects_of($botnet);
        if ($tries > 0 && $failures{$botnet} >= $tries) {
            remove_reconnect($_) for @rec;
            say_info(sprintf('%s nie odpowiada (%d proby) - wstrzymano ponawianie; /bot %s, aby sprobowac znow',
                display_name($botnet), $failures{$botnet}, display_name($botnet))) if @rec;
            return;
        }
        remove_reconnect($_) for @rec[1 .. $#rec];
    }, undef);
}

sub sig_server_disconnected {
    my ($server) = @_;
    my $botnet = server_botnet($server) or return;
    delete $connecting{ $server->{tag} };
    Irssi::timeout_add_once(100, sub { cleanup(1) }, undef);
}

# zamkniecie okna partyline przez uzytkownika = rozlaczenie botnetu
sub sig_query_destroyed {
    my ($query) = @_;
    return if $quitting;
    return unless lc($query->{name} // '') eq lc partyline_nick();
    my $server = $query->{server};
    return unless $server && $server->{connected} && server_botnet($server);
    return if $closing{ $server->{tag} };
    my $botnet = server_botnet($server);
    Irssi::timeout_add_once(10, sub {
        my $s = Irssi::server_find_tag($server->{tag}) or return;
        disconnect($s);
        remove_reconnect($_) for reconnects_of($botnet);
        say_info(display_name($botnet) . ' rozlaczony (zamknieto okno partyline)');
    }, undef);
}

# ── /bot ─────────────────────────────────────────────────────────────

sub status_line {
    my ($botnet) = @_;
    my ($srv, $pending) = live_servers($botnet);
    my @rec = reconnects_of($botnet);
    my $name = display_name($botnet);
    if (@$srv) {
        my $s = $srv->[0];
        my $win = window_of($s);
        my $since = int((time - ($s->{connect_time} || time)) / 60);
        return sprintf('%-11s polaczony jako %s od %d min%s%s', $name, $s->{tag}, $since,
            $win ? ", okno $win->{refnum}" : '', @$srv > 1 ? sprintf(' (+%d nadmiarowe!)', @$srv - 1) : '');
    }
    return sprintf('%-11s laczy sie', $name) if @$pending;
    if (@rec) {
        return sprintf('%-11s ponowi za %ds%s', $name, ($rec[0]{next_connect} // time) - time,
            @rec > 1 ? sprintf(' (zdublowane: %d)', scalar @rec) : '');
    }
    my $f = $failures{$botnet};
    return sprintf('%-11s nie odpowiada (%d proby), ponawianie wstrzymane', $name, $f) if $f;
    return sprintf('%-11s rozlaczony', $name);
}

sub find_botnet_arg {
    my ($arg) = @_;
    my %set = botnets();
    return lc $arg if exists $set{ lc $arg };
    my @m = grep { index($_, lc $arg) == 0 } sort keys %set;
    return @m == 1 ? $m[0] : undef;
}

sub help {
    say_here($_) for (
        "botnet.pl $VERSION - partyline botnetow, jedno polaczenie i jedno okno na botnet",
        '  /bot                 stan botnetow',
        '  /bot <botnet>        polacz albo przejdz do okna',
        '  /bot close <botnet>  rozlacz bez ponawiania, zamknij okno',
        '  /bot cleanup         uporzadkuj polaczenia i ponowienia teraz',
        '  zamkniecie okna partyline (/wc) tez rozlacza botnet',
        '  botnety: ' . join(' ', map { display_name($_) } sort keys %{ { botnets() } }) . '  (/set botnet_chatnets)',
    );
}

sub cmd_bot {
    my ($data, $server, $item) = @_;
    my @a = split ' ', ($data // '');
    my %set = botnets();

    if (!@a) {
        say_here('botnety:');
        say_here('  ' . status_line($_)) for sort keys %set;
        return;
    }
    my $sub = lc $a[0];
    if ($sub eq 'help') { help(); return }
    if ($sub eq 'cleanup') {
        say_here('botnet: wszystko w porzadku - nic do sprzatania') unless cleanup(0);
        return;
    }
    if ($sub eq 'close' || $sub eq 'zamknij') {
        my $botnet = find_botnet_arg($a[1] // '');
        unless ($botnet) { say_here('botnet: ktory botnet? /bot close <botnet>'); return }
        my ($srv) = live_servers($botnet);
        remove_reconnect($_) for reconnects_of($botnet);
        delete $failures{$botnet};
        disconnect($_) for @$srv;
        say_here('botnet: ' . display_name($botnet) . (@$srv ? ' rozlaczony' : ' nie byl polaczony - ponowienia usuniete'));
        return;
    }
    my $botnet = find_botnet_arg($a[0]);
    unless ($botnet) {
        say_here("botnet: nie znam botnetu '$a[0]' - /bot pokazuje liste");
        return;
    }
    Irssi::command('connect ' . display_name($botnet));
}

# ── start ────────────────────────────────────────────────────────────

Irssi::signal_add_first('command connect', \&cmd_connect);
Irssi::signal_add_first('command quit', sub { $quitting = 1 });
Irssi::signal_add('server looking', \&sig_server_looking);
Irssi::signal_add_last('server connected', \&sig_server_connected);
Irssi::signal_add_last('event connected', \&sig_event_connected);
Irssi::signal_add_last('server connect failed', \&sig_connect_failed);
Irssi::signal_add_last('server disconnected', \&sig_server_disconnected);
Irssi::signal_add('query destroyed', \&sig_query_destroyed);
Irssi::command_bind('bot', \&cmd_bot);

# po zaladowaniu: porzadek w tym, co juz jest (po starcie erssi - z timera)
Irssi::timeout_add_once(500, sub { cleanup(0) }, undef);
