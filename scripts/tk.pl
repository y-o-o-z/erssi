use strict;
use warnings;

use Fcntl qw(O_APPEND O_CREAT O_WRONLY LOCK_EX);
use JSON::PP ();
use Socket qw(AF_INET AF_INET6 inet_pton inet_ntop);
use Irssi;    # Irssi::Irc nie jest wymagane - obiekty serwera i tak sa Irssi::Irc::Server

use vars qw($VERSION %IRSSI);

$VERSION = '2.1.2';
%IRSSI = (
    authors     => 'yooz',
    contact     => 'https://github.com/y-o-o-z',
    name        => 'tk',
    description => 'TKLINE dla ircd 2.11 (IRCnet): WHOIS -> maska -> TKLINE, z kontrola zakresu maski, podgladem i audytem',
    license     => 'GPL-3.0-or-later',
);

# =====================================================================
#  tk.pl - nadawanie czasowych K-linii (TKLINE) z poziomu irssi
#
#  Protokol (ircd 2.11, ircd/s_conf.c):
#     TKLINE <czas> <user@host> :<powod>      (3 parametry wymagane)
#     UNTKLINE <user@host>                    (dopasowanie DOKLADNE,
#                                              bez wildcardow - musi byc
#                                              taka sama maska jak nadana)
#     STATS k -> lista tklinii, STATS K -> lista stalych K-linii
#
#  <czas> parsuje serwerowy wdhms2sec(): 30s / 10m / 2h / 1d / 1w.
#  Licznik BEZ jednostki to u serwera MINUTY (TKLINE_MULTIPLIER = 60, od
#  2.11.1; zweryfikowane w irc2.11.2p3 z 42.pl/ircd) - skrypt liczy tak
#  samo i wysyla czas w postaci kanonicznej (60 -> 1h).
#  i kombinacje typu 1d12h. Serwer moze dodatkowo obcinac czas do
#  TKLINE_MAXTIME, a powod do TOPICLEN.
#
#  Skrypt NIGDY nie zgaduje hosta: dla nicka robi WHOIS przez mechanizm
#  przekierowan irssi (redirect_event), a TKLINE wysyla dopiero po
#  otrzymaniu numerika 311. Odpowiedz WHOIS jest wyciszona - w oknie
#  widac tylko wynik dzialania skryptu.
#
#  /tk help          - pelna pomoc
# =====================================================================

# ---- stale protokolu ircd 2.11 --------------------------------------
my $USERLEN      = 10;    # ircd 2.11: struct.h
my $HOSTLEN      = 63;
my $MAX_RAW      = 480;   # 512 z CRLF, zostawiamy zapas
my $DEFAULT_TOPICLEN = 160;
my @MASK_TYPES   = qw(ident host domain);
my %DURATION_FACTOR = (w => 604800, d => 86400, h => 3600, m => 60, s => 1);

# ---- stan runtime ---------------------------------------------------
# Obiektow Irssi nie wolno przechowywac miedzy zdarzeniami (perl.txt:
# "Storing and later using any Irssi object may result in use-after-free"),
# wiec trzymamy wylacznie tag serwera i odzyskujemy obiekt przez
# Irssi::server_find_tag().
my %pending;         # "tag\x1enick" => zadanie czekajace na WHOIS
my %confirm;         # tag          => zadanie czekajace na /tk yes
my %expect;          # tag          => co ostatnio wyslalismy (do diagnozy bledow)
my @recent;          # historia sesji
my $expect_window    = 15;   # sekundy, w ktorych wiazemy blad serwera z nasza komenda
my $confirm_window   = 60;

sub setting_int {
    my ($name, $minimum, $maximum) = @_;
    my $value = int(Irssi::settings_get_int($name) // 0);
    $value = $minimum if defined $minimum && $value < $minimum;
    $value = $maximum if defined $maximum && $value > $maximum;
    return $value;
}

sub register_settings {
    Irssi::settings_add_str('tk',  'tk_mask_type',       'ident');
    Irssi::settings_add_str('tk',  'tk_default_time',    '30m');
    Irssi::settings_add_str('tk',  'tk_max_time',        '30d');
    Irssi::settings_add_int('tk',  'tk_whois_timeout_ms', 10_000);
    Irssi::settings_add_int('tk',  'tk_reason_max',      0);      # 0 = z ISUPPORT TOPICLEN
    Irssi::settings_add_bool('tk', 'tk_require_oper',    1);
    Irssi::settings_add_bool('tk', 'tk_allow_broad',     0);
    Irssi::settings_add_bool('tk', 'tk_confirm',         0);
    Irssi::settings_add_bool('tk', 'tk_audit',           1);
}

# =====================================================================
#  Wyjscie na ekran
# =====================================================================

sub register_formats {
    Irssi::theme_register([
        'tk_loaded', '{line_start}%_TK%_ v$0 zaladowany - /tk help',
        'tk_info',   '{line_start}%_TK%_ $0',
        'tk_warn',   '{line_start}%_TK%_ %Yuwaga%n $0',
        'tk_error',  '{line_start}%_TK%_ %Rblad%n $0',
        'tk_sent',   '{line_start}%_TK%_ %G>>%n $0',
        'tk_dry',    '{line_start}%_TK%_ %Ypodglad%n $0',
    ]);
}

# Tekst od usera (nick, maska, powod, komunikat serwera) idzie wylacznie
# jako argument printformat - erssi wstawia argumenty doslownie (bez kodow
# %), wiec % zostaje jak jest (podwojenie dawaloby na ekranie "%%").
# Zdejmujemy tylko znaki sterujace (kolory mIRC, CR/LF).
sub esc {
    my ($text) = @_;
    return '' unless defined $text;
    $text =~ s/[\x00-\x08\x0a-\x1f]//g;
    return $text;
}

sub say_info  { Irssi::printformat(Irssi::MSGLEVEL_CLIENTCRAP(),  'tk_info',  esc($_[0])) }
sub say_warn  { Irssi::printformat(Irssi::MSGLEVEL_CLIENTCRAP(),  'tk_warn',  esc($_[0])) }
# poziom CLIENTCRAP: przy CLIENTERROR erssi dokleja przed linia wlasna etykiete
sub say_error { Irssi::printformat(Irssi::MSGLEVEL_CLIENTCRAP(),  'tk_error', esc($_[0])) }

sub say_command {
    my ($format, $req, $mask) = @_;
    Irssi::printformat(Irssi::MSGLEVEL_CLIENTCRAP(), $format, esc(raw_line($req, $mask)));
}

# =====================================================================
#  Audyt
# =====================================================================

sub audit_path {
    my $dir = Irssi::get_irssi_dir();
    return '' unless defined $dir && length $dir;
    return $dir . '/tk_audit.jsonl';
}

sub audit_event {
    my ($event, %fields) = @_;
    return unless Irssi::settings_get_bool('tk_audit');
    my $path = audit_path();
    return unless defined $path && length $path;

    my $line = eval {
        JSON::PP->new->canonical(1)->encode({
            timestamp => time(),
            event     => $event,
            %fields,
        });
    };
    return unless defined $line;

    if (sysopen(my $fh, $path, O_WRONLY | O_APPEND | O_CREAT, 0600)) {
        chmod 0600, $path;
        print {$fh} $line, "\n" if flock($fh, LOCK_EX);
        close $fh;
    }
    return;
}

# =====================================================================
#  Czas, powod, maski - czysta logika (testowalna bez irssi)
# =====================================================================

# Zgodnie z wdhms2sec() z ircd 2.11: ciag <liczba><jednostka>, jednostki
# w/d/h/m/s; licznik bez jednostki (tylko na koncu) to MINUTY
# (TKLINE_MULTIPLIER = 60). Zwraca liczbe sekund.
my $BARE_UNIT = 60;
sub duration_seconds {
    my ($value) = @_;
    return unless defined $value && length $value && length($value) <= 20;

    my $rest  = lc $value;
    my $total = 0;
    my $seen  = 0;

    while (length $rest) {
        last unless $rest =~ s/\A([0-9]{1,9})([wdhms]?)//;
        my ($count, $unit) = ($1, $2);
        return if $unit eq '' && length $rest;   # cyfry bez jednostki tylko na koncu
        $total += $count * ($unit eq '' ? $BARE_UNIT : $DURATION_FACTOR{$unit});
        $seen = 1;
        return if $total > 3650 * 86_400;        # absurd, pewnie literowka
    }
    return if length $rest;
    return unless $seen && $total > 0;
    return $total;
}

# Do serwera zawsze postac kanoniczna z jednostkami ("60" -> "1h",
# "10m5" -> "15m"): podglad pokazuje dokladnie to, co policzy ircd.
sub normalize_duration {
    my ($value) = @_;
    my $seconds = duration_seconds($value);
    return defined $seconds ? human_duration($seconds) : undef;
}

sub human_duration {
    my ($seconds) = @_;
    return '0s' unless defined $seconds && $seconds > 0;
    my @out;
    for my $unit (qw(w d h m s)) {
        my $factor = $DURATION_FACTOR{$unit};
        next if $seconds < $factor;
        my $count = int($seconds / $factor);
        $seconds -= $count * $factor;
        push @out, "$count$unit";
    }
    return join('', @out);
}

sub clean_reason {
    my ($reason, $limit) = @_;
    return '' unless defined $reason;
    $limit = $DEFAULT_TOPICLEN unless defined $limit && $limit > 0;
    $reason =~ s/[\x00-\x1f]/ /g;    # CR/LF = wstrzykniecie komendy do socketu
    $reason =~ s/\A\s+//;
    $reason =~ s/\s+\z//;
    $reason =~ s/\s{2,}/ /g;
    $reason =~ s/\A://;
    $reason = substr($reason, 0, $limit) if length($reason) > $limit;
    $reason =~ s/\s+\z//;
    return $reason;
}

sub valid_nick {
    my ($nick) = @_;
    return 0 unless defined $nick;
    return $nick =~ /\A[A-Za-z\[\]\\`^{}_|][A-Za-z0-9\[\]\\`^{}_|-]{0,31}\z/ ? 1 : 0;
}

sub split_mask {
    my ($mask) = @_;
    return unless defined $mask;
    my $at = index($mask, '@');
    return if $at < 0;
    return (substr($mask, 0, $at), substr($mask, $at + 1));
}

# Powtorzenie kontroli, ktore robi prep_kline() w ircd 2.11 - lepiej
# odrzucic lokalnie niz dostac "T/KLINE: Incorrect format".
sub mask_problem {
    my ($mask) = @_;
    return 'maska jest pusta' unless defined $mask && length $mask;
    return 'maska zawiera bialy znak lub znak sterujacy' if $mask =~ /[\s\x00-\x1f]/;
    my ($user, $host) = split_mask($mask);
    return 'maska musi miec postac user@host' unless defined $host;
    return 'maska musi miec postac user@host' if index($host, '@') >= 0;
    return 'maska bez czesci user lub host (ircd to odrzuca)'
        if !length($user) || !length($host) || $mask eq '@' || $mask eq '@*' || $mask eq '*@';
    my $bare = $user;
    $bare =~ s/\A=//;    # '=' = wariant "otherkill", ircd go zdejmuje
    return 'czesc user jest za dluga (ircd 2.11: ' . $USERLEN . ' znakow)'
        if length($bare) > $USERLEN;
    return 'czesc host jest za dluga (ircd 2.11: ' . $HOSTLEN . ' znakow)'
        if length($host) > $HOSTLEN;
    return cidr_problem($host) if index($host, '/') >= 0;
    return;
}

# Host z '/' serwer sprawdza jako ip/prefiks (match_ipmask); zly zapis
# daje "Incorrect format", hostname z '/' nigdy nie przejdzie.
sub cidr_problem {
    my ($host) = @_;
    my ($addr, $bits) = $host =~ m{\A([^/]+)/([0-9]{1,3})\z}
        or return "maska z '/' musi miec postac ip/prefiks, np. 1.2.3.0/24";
    if ($addr =~ /\A([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)\z/) {
        return 'nieprawidlowy adres IPv4 w masce' if grep { $_ > 255 } ($1, $2, $3, $4);
        return 'prefiks IPv4 musi byc z zakresu 0-32' if $bits > 32;
        return;
    }
    if (index($addr, ':') >= 0 && $addr =~ /\A[0-9A-Fa-f:]+\z/) {
        return 'prefiks IPv6 musi byc z zakresu 0-128' if $bits > 128;
        return;
    }
    return "ip/prefiks tylko dla adresow IP - serwer odrzuca hostname z '/'";
}

# Maska, ktora zdejmie pol sieci. Serwer jej nie odrzuci, wiec musimy my.
sub mask_is_broad {
    my ($mask) = @_;
    my ($user, $host) = split_mask($mask);
    return 1 unless defined $host;
    return 1 if $host !~ /[A-Za-z0-9]/;              # *, *.*, ?.? itd.
    my $literal = $host;
    $literal =~ s/[*?]//g;
    return 1 if length($literal) < 4;                # np. *.pl, *.co
    return 1 if $host =~ /\A[*?]+\.?[^.]*\z/;        # *pl, *.pl - jeden czlon
    return 0;
}

sub host_is_ipv4 { return defined $_[0] && $_[0] =~ /\A[0-9]{1,3}(?:\.[0-9]{1,3}){3}\z/ ? 1 : 0 }

# Domeny drugiego poziomu, przy ktorych *.<2ld>.<tld> byloby za szerokie.
my %SECOND_LEVEL = map { $_ => 1 } qw(com net org edu gov mil co ac biz info waw);

# IPv6 -> siec /64 jako ip/prefiks (2001:db8:1:2::/64): glob na zapisie
# tekstowym nie dziala, bo ten sam adres ma wiele zapisow ("::" skraca
# zera). ::ffff:a.b.c.d to IPv4 - maska jak dla IPv4. Niepoprawny adres:
# undef (maska sie nie zbuduje, nic nie wychodzi).
sub ipv6_mask_host {
    my ($host) = @_;
    my $packed = inet_pton(AF_INET6, $host) or return;
    if (substr($packed, 0, 12) eq ("\0" x 10) . "\xff\xff") {
        my $ipv4 = inet_ntop(AF_INET, substr($packed, 12, 4));
        return domain_mask_host($ipv4);
    }
    my $net = substr($packed, 0, 8) . ("\0" x 8);
    return inet_ntop(AF_INET6, $net) . '/64';
}

sub domain_mask_host {
    my ($host) = @_;
    return $host unless defined $host && length $host;

    if (host_is_ipv4($host)) {
        my @octets = split /\./, $host;
        $octets[3] = '*';
        return join('.', @octets);
    }
    return ipv6_mask_host($host) if index($host, ':') >= 0;

    my @labels = split /\./, $host;
    return $host if @labels <= 2;
    my $keep = 2;
    $keep = 3 if @labels >= 3
        && length($labels[-1]) <= 3
        && $SECOND_LEVEL{lc $labels[-2]};
    return $host if @labels <= $keep;
    return '*.' . join('.', @labels[-$keep .. -1]);
}

sub mask_type_valid { my $t = lc($_[0] // ''); return (grep { $_ eq $t } @MASK_TYPES) ? 1 : 0 }

sub build_mask {
    my ($user, $host, $type) = @_;
    return unless defined $user && defined $host && length $host;
    $type = 'ident' unless mask_type_valid($type);

    $user =~ s/\A~/*/;                               # brak identd - '~' bywa niestabilny
    $user = '*' unless length $user;
    $user = substr($user, 0, $USERLEN) if length($user) > $USERLEN;

    return "$user\@$host"                if $type eq 'ident';
    return '*@' . $host                  if $type eq 'host';
    my $domain = domain_mask_host($host);
    return defined $domain ? '*@' . $domain : undef;
}

sub glob_matches {
    my ($glob, $text) = @_;
    my $regex = quotemeta lc $glob;
    $regex =~ s/\\\*/.*/g;
    $regex =~ s/\\\?/./g;
    return lc($text) =~ /\A$regex\z/ ? 1 : 0;
}

# adres IP (v4 albo v6) w ip/prefiks
sub ip_in_cidr {
    my ($ip, $cidr) = @_;
    my ($net, $bits) = $cidr =~ m{\A([^/]+)/([0-9]{1,3})\z} or return 0;
    for my $family (AF_INET, AF_INET6) {
        my $addr = inet_pton($family, $ip) // next;
        my $netp = inet_pton($family, $net) // return 0;
        return 0 if $bits > 8 * length $netp;
        my $mask = pack('B*', ('1' x $bits) . ('0' x (8 * length($netp) - $bits)));
        return (($addr & $mask) eq ($netp & $mask)) ? 1 : 0;
    }
    return 0;
}

sub mask_matches {
    my ($mask, $userhost) = @_;
    return 0 unless defined $mask && defined $userhost;
    my ($muser, $mhost) = split_mask($mask);
    my ($uuser, $uhost) = split_mask($userhost);
    if (defined $mhost && defined $uhost && index($mhost, '/') >= 0) {
        return glob_matches($muser, $uuser) && ip_in_cidr($uhost, $mhost) ? 1 : 0;
    }
    return glob_matches($mask, $userhost);
}

# Maska typu ident@dokladny.host: ident bez wildcardow (dopuszczalny
# '*' zamiast '~' na poczatku, jak z build_mask) i host bez * ? oraz '/'.
sub mask_is_exact {
    my ($mask) = @_;
    my ($user, $host) = split_mask($mask);
    return 0 unless defined $host && length $host;
    return 0 if $host =~ m{[*?/]};
    return $user =~ /\A=?\*?[^*?]+\z/ ? 1 : 0;
}

# =====================================================================
#  Serwer
# =====================================================================

sub resolve_server {
    my ($tag, $server) = @_;
    if (defined $tag && length $tag) {
        my $found = Irssi::server_find_tag($tag);
        return (undef, "brak polaczenia o tagu $tag") unless $found;
        $server = $found;
    }
    $server = Irssi::active_server() unless $server;
    return (undef, 'brak aktywnego polaczenia IRC') unless $server;
    return (undef, 'serwer ' . ($server->{tag} // '?') . ' nie jest polaczony')
        unless $server->{connected};
    return ($server, undef);
}

sub reason_limit {
    my ($server) = @_;
    my $configured = setting_int('tk_reason_max', 0, 400);
    return $configured if $configured > 0;
    my $topiclen = eval { $server->isupport('TOPICLEN') };
    return $topiclen if defined $topiclen && $topiclen =~ /\A[0-9]{1,3}\z/ && $topiclen > 0;
    return $DEFAULT_TOPICLEN;
}

sub max_seconds {
    my $raw = Irssi::settings_get_str('tk_max_time') // '';
    $raw =~ s/\A\s+|\s+\z//g;
    return 0 if !length($raw) || $raw eq '0';
    my $seconds = duration_seconds($raw);
    return defined $seconds ? $seconds : 30 * 86_400;
}

sub pending_key {
    my ($tag, $nick) = @_;
    return lc(($tag // '') . "\x1e" . ($nick // ''));
}

# =====================================================================
#  Wysylka
# =====================================================================

sub raw_line {
    my ($req, $mask) = @_;
    return "$req->{command} $req->{duration} $mask :$req->{reason}"
        if $req->{command} eq 'TKLINE';
    return "$req->{command} $mask";
}

sub deliver {
    my ($server, $req, $mask) = @_;

    my $line = raw_line($req, $mask);
    if (length($line) > $MAX_RAW && $req->{command} eq 'TKLINE') {
        my $overflow = length($line) - $MAX_RAW;
        $req->{reason} = clean_reason(substr($req->{reason}, 0, length($req->{reason}) - $overflow));
        say_warn('powod przyciety do limitu dlugosci linii IRC');
        $line = raw_line($req, $mask);
    }

    $server->send_raw($line);
    $expect{$server->{tag}} = { time => time(), command => $req->{command}, mask => $mask };

    say_command('tk_sent', $req, $mask);
    unshift @recent, {
        time    => time(),
        tag     => $server->{tag},
        command => $req->{command},
        mask    => $mask,
        target  => $req->{target},
        duration => $req->{duration},
        reason  => $req->{reason},
    };
    pop @recent while @recent > 20;

    audit_event(lc($req->{command}) . '_sent',
        tag      => $server->{tag},
        network  => ($server->{chatnet} // ''),
        target   => $req->{target},
        mask     => $mask,
        duration => $req->{duration},
        reason   => $req->{reason},
    );
    return 1;
}

# Ostatni etap: znamy juz konkretna maske. Tu zapadaja decyzje o
# bezpieczenstwie, bo maska z WHOIS-a moze byc inna niz ta wpisana recznie.
sub finish_request {
    my ($server, $req, $mask) = @_;

    if (my $problem = mask_problem($mask)) {
        say_error("$problem ($mask) - nie wysylam");
        audit_event('rejected', reason => $problem, mask => $mask, target => $req->{target});
        return;
    }

    if ($req->{command} eq 'TKLINE') {
        my $me = $server->{userhost};
        # erssi zna nasz user@host dopiero po wejsciu na kanal (albo 396) -
        # bez niego nie sprawdzimy samobanu, wiec tylko maska waska
        if ((!defined $me || !length $me) && !mask_is_exact($mask)) {
            say_error("nie znam Twojego user\@host (serwer go nie podal - wejdz na dowolny kanal), "
                . "wiec nie sprawdze, czy maska $mask nie lapie Ciebie - nie wysylam; "
                . 'bez tego dozwolona tylko maska ident@dokladny.host (-mask ident)');
            audit_event('rejected', reason => 'self_unknown', mask => $mask);
            return;
        }
        if (defined $me && length $me && mask_matches($mask, $me)) {
            say_error("maska $mask lapie Ciebie ($me) - nie wysylam");
            audit_event('rejected', reason => 'self_match', mask => $mask);
            return;
        }
        if (mask_is_broad($mask) && !$req->{force} && !Irssi::settings_get_bool('tk_allow_broad')) {
            say_error("maska $mask jest zbyt szeroka - powtorz z -force albo zawez (-mask ident)");
            audit_event('rejected', reason => 'broad_mask', mask => $mask);
            return;
        }
    }

    if ($req->{dry}) {
        say_command('tk_dry', $req, $mask);
        return;
    }

    if ($req->{command} eq 'TKLINE' && Irssi::settings_get_bool('tk_confirm') && !$req->{confirmed}) {
        $confirm{$server->{tag}} = { %$req, mask => $mask, expires => time() + $confirm_window };
        say_command('tk_dry', $req, $mask);
        say_info("potwierdz w ciagu ${confirm_window}s: /tk yes (anulowanie: /tk no)");
        return;
    }

    return deliver($server, $req, $mask);
}

# =====================================================================
#  WHOIS -> maska
# =====================================================================

sub start_whois {
    my ($server, $req) = @_;

    my $key = pending_key($server->{tag}, $req->{target});
    if (exists $pending{$key}) {
        say_error("WHOIS dla $req->{target} jest juz w toku");
        return;
    }

    $req->{tag}     = $server->{tag};
    $req->{started} = time();
    $pending{$key}  = $req;
    $pending{$key}{timeout} = Irssi::timeout_add_once(
        setting_int('tk_whois_timeout_ms', 2000, 60_000), 'whois_timed_out', $key);

    # Cala odpowiedz WHOIS trafia do skryptu; '' => 'event empty' wycisza
    # numeryki, ktorych nie obslugujemy, zeby nie zasmiecac okna.
    $server->redirect_event('whois', 1, $req->{target}, -1, '', {
        'event 311' => 'redir tk whois',       # RPL_WHOISUSER
        'event 318' => 'redir tk whois end',   # RPL_ENDOFWHOIS
        'event 401' => 'redir tk whois none',  # ERR_NOSUCHNICK
        'event 402' => 'redir tk whois none',  # ERR_NOSUCHSERVER
        ''          => 'event empty',
    });
    $server->send_raw('WHOIS ' . $req->{target});
    say_info("WHOIS $req->{target} - czekam na ident\@host");
    return;
}

sub take_pending {
    my ($tag, $nick) = @_;
    my $key = pending_key($tag, $nick);
    my $req = delete $pending{$key} or return;
    Irssi::timeout_remove($req->{timeout}) if $req->{timeout};
    return $req;
}

sub whois_timed_out {
    my ($key) = @_;
    my $req = delete $pending{$key} or return;
    say_error("WHOIS dla $req->{target} bez odpowiedzi - $req->{command} NIE zostal wyslany");
    audit_event('whois_timeout', tag => $req->{tag}, target => $req->{target});
    return;
}

# 311: <mynick> <nick> <user> <host> * :<realname>
sub sig_whois {
    my ($server, $data) = @_;
    return unless $server;
    my (undef, $nick, $user, $host) = split / +/, ($data // '');
    return unless defined $nick && defined $user && defined $host;

    my $req = take_pending($server->{tag}, $nick) or return;
    $req->{whois} = "$user\@$host";

    if ($req->{command} eq 'MASK') {
        say_info("$nick = $user\@$host");
        say_info(sprintf('  %-6s %s', $_, build_mask($user, $host, $_) // '?'))
            for @MASK_TYPES;
        return;
    }

    my $mask = build_mask($user, $host, $req->{mask_type});
    unless (defined $mask) {
        say_error("WHOIS dla $nick zwrocil nieuzyteczny host - nie wysylam");
        return;
    }
    say_info("$nick = $user\@$host -> $mask");
    return finish_request($server, $req, $mask);
}

# 318 konczy WHOIS - jesli zadanie wciaz czeka, 311 nie przyszlo.
sub sig_whois_end {
    my ($server, $data) = @_;
    return unless $server;
    my (undef, $nick) = split / +/, ($data // '');
    return unless defined $nick;
    my $req = take_pending($server->{tag}, $nick) or return;
    say_error("nie znaleziono $nick - $req->{command} NIE zostal wyslany");
    audit_event('whois_missing', tag => $req->{tag}, target => $req->{target});
    return;
}

# 401/402 potrafia przyjsc z tokenem, ktory nie jest naszym nickiem
# (np. nazwa serwera przy 402) - wtedy bierzemy jedyne zadanie tego tagu.
sub sig_whois_none {
    my ($server, $data) = @_;
    return unless $server;
    my (undef, $token) = split / +/, ($data // '');
    my $req = defined $token ? take_pending($server->{tag}, $token) : undef;
    $req = take_single_pending($server->{tag}) unless $req;
    return unless $req;
    say_error("nie znaleziono $req->{target} - $req->{command} NIE zostal wyslany");
    audit_event('whois_missing', tag => $req->{tag}, target => $req->{target});
    return;
}

sub take_single_pending {
    my ($tag) = @_;
    my @keys = grep { ($pending{$_}{tag} // '') eq ($tag // '') } keys %pending;
    return unless @keys == 1;
    my $req = delete $pending{$keys[0]};
    Irssi::timeout_remove($req->{timeout}) if $req->{timeout};
    return $req;
}

# =====================================================================
#  Diagnostyka odpowiedzi serwera
# =====================================================================

sub expected_recently {
    my ($server) = @_;
    my $rec = $expect{$server->{tag}} or return;
    return if time() - $rec->{time} > $expect_window;
    return $rec;
}

sub sig_notice {
    my ($server, $data) = @_;
    return unless $server;
    my $rec = expected_recently($server) or return;
    return unless ($data // '') =~ /(T?KLINE|UNTKLINE): Incorrect format/i;
    say_error("serwer odrzucil $rec->{command} $rec->{mask} - niepoprawny format maski");
    audit_event('server_rejected', tag => $server->{tag}, mask => $rec->{mask});
    return;
}

sub sig_no_privileges {
    my ($server) = @_;
    return unless $server;
    my $rec = expected_recently($server) or return;
    say_error("brak uprawnien do $rec->{command} (O-line bez flagi kline/tkline)");
    audit_event('no_privileges', tag => $server->{tag}, mask => $rec->{mask});
    return;
}

# 421: <mynick> <command> :Unknown command
sub sig_unknown_command {
    my ($server, $data) = @_;
    return unless $server;
    my (undef, $command) = split / +/, ($data // '');
    return unless defined $command && $command =~ /\A(?:UN)?TKLINE\z/i;
    say_error("serwer nie zna komendy $command - ircd zbudowany bez #define TKLINE");
    audit_event('unknown_command', tag => $server->{tag}, command => uc $command);
    return;
}

# =====================================================================
#  Komendy
# =====================================================================

sub parse_options {
    my ($command, $data) = @_;
    my ($options, $rest) = Irssi::command_parse_options($command, $data);
    return unless defined $options;    # irssi sam zglosil bledna opcje
    return ($options, defined $rest ? $rest : '');
}

sub prepare_request {
    my ($command, $options, $needs_oper, $window_server) = @_;

    my ($server, $error) = resolve_server($options->{server}, $window_server);
    unless ($server) {
        say_error($error);
        return;
    }
    if ($needs_oper && !$server->{server_operator} && Irssi::settings_get_bool('tk_require_oper')) {
        say_error('nie masz +o na serwerze (/oper) - ustaw tk_require_oper OFF aby wysylac mimo to');
        return;
    }

    my $mask_type = $options->{mask} // Irssi::settings_get_str('tk_mask_type');
    unless (mask_type_valid($mask_type)) {
        say_error('tk_mask_type / -mask musi byc: ' . join(', ', @MASK_TYPES));
        return;
    }

    return ($server, {
        command   => $command,
        mask_type => lc $mask_type,
        # flaga bez argumentu przychodzi z erssi jako '' (falsz!) - liczy sie obecnosc
        force     => exists $options->{force} ? 1 : 0,
        dry       => exists $options->{dry} ? 1 : 0,
    });
}

sub cmd_tkl {
    my ($data, $window_server) = @_;

    my ($options, $rest) = parse_options('tkl', $data) or return;
    my ($server, $req) = prepare_request('TKLINE', $options, 1, $window_server);
    return unless $server;

    my ($target, $tail) = ($rest // '') =~ /\A\s*(\S+)\s+(.+?)\s*\z/;
    unless (defined $tail) {
        usage_tkl();
        return;
    }

    # Drugi wyraz to czas tylko wtedy, gdy naprawde nim jest - inaczej
    # calosc jest powodem, a czas bierzemy z tk_default_time.
    my ($maybe_time, $reason_rest) = $tail =~ /\A(\S+)\s+(.+)\z/;
    my ($duration_raw, $reason);
    if (defined $maybe_time && defined duration_seconds($maybe_time)) {
        ($duration_raw, $reason) = ($maybe_time, $reason_rest);
    }
    elsif (!defined $maybe_time && defined duration_seconds($tail)) {
        say_error('brak powodu - sam czas nie wystarczy');
        usage_tkl();
        return;
    }
    else {
        $duration_raw = Irssi::settings_get_str('tk_default_time');
        $reason       = $tail;
    }

    my $seconds = duration_seconds($duration_raw);
    unless (defined $seconds) {
        say_error("nieprawidlowy czas '$duration_raw' - uzyj 30s, 10m, 2h, 1d, 1w lub 1d12h");
        return;
    }
    my $limit = max_seconds();
    if ($limit && $seconds > $limit) {
        say_error('czas ' . human_duration($seconds) . ' przekracza tk_max_time ('
            . human_duration($limit) . ')');
        return;
    }

    $reason = clean_reason($reason, reason_limit($server));
    unless (length $reason) {
        say_error('powod nie moze byc pusty');
        return;
    }

    $req->{target}   = $target;
    $req->{duration} = normalize_duration($duration_raw);
    $req->{reason}   = $reason;

    if (index($target, '@') >= 0) {
        return finish_request($server, $req, $target);
    }
    unless (valid_nick($target)) {
        say_error("'$target' to ani poprawny nick, ani maska user\@host");
        return;
    }
    if (lc($target) eq lc($server->{nick} // '')) {
        say_error('to Twoj wlasny nick');
        return;
    }
    return start_whois($server, $req);
}

sub cmd_untkl {
    my ($data, $window_server) = @_;

    my ($options, $rest) = parse_options('untkl', $data) or return;
    my ($server, $req) = prepare_request('UNTKLINE', $options, 1, $window_server);
    return unless $server;

    my ($target) = ($rest // '') =~ /\A\s*(\S+)\s*\z/;
    unless (defined $target) {
        say_info('uzycie: /untkl <user@host albo nick>');
        say_info('UNTKLINE dopasowuje maski doslownie - sprawdz /tklist');
        return;
    }

    $req->{target}   = $target;
    $req->{duration} = '';
    $req->{reason}   = '';

    return finish_request($server, $req, $target) if index($target, '@') >= 0;
    unless (valid_nick($target)) {
        say_error("'$target' to ani poprawny nick, ani maska user\@host");
        return;
    }
    say_warn('UNTKLINE wymaga maski identycznej z nadana - maska z WHOIS moze sie nie zgadzac');
    return start_whois($server, $req);
}

sub cmd_stats {
    my ($letter, $data, $server) = @_;
    my $error;
    ($server, $error) = resolve_server(undef, $server);
    unless ($server) {
        say_error($error);
        return;
    }
    my ($argument) = ($data // '') =~ /\A\s*([^\s\x00-\x1f]{1,64})\s*\z/;
    $server->send_raw('STATS ' . $letter . (defined $argument ? " $argument" : ''));
    return;
}

sub cmd_tklist { return cmd_stats('k', $_[0], $_[1]) }   # tkliny (ircd 2.11)
sub cmd_klist  { return cmd_stats('K', $_[0], $_[1]) }   # stale K-linie

sub cmd_tk_yes {
    my ($data, $server) = @_;
    my ($resolved, $error) = resolve_server(undef, $server);
    unless ($resolved) {
        say_error($error);
        return;
    }
    my $req = delete $confirm{$resolved->{tag}};
    unless ($req && $req->{expires} >= time()) {
        say_error('nie ma nic do potwierdzenia');
        return;
    }
    $req->{confirmed} = 1;
    return deliver($resolved, $req, $req->{mask});
}

sub cmd_tk_no {
    my ($data, $server) = @_;
    my ($resolved) = resolve_server(undef, $server);
    my $req = $resolved ? delete $confirm{$resolved->{tag}} : undef;
    say_info($req ? "anulowano $req->{command} $req->{mask}" : 'nie ma nic do anulowania');
    return;
}

sub cmd_tk_pending {
    my @keys = sort keys %pending;
    unless (@keys) {
        say_info('brak zadan czekajacych na WHOIS');
        return;
    }
    for my $key (@keys) {
        my $req = $pending{$key};
        say_info(sprintf('%s %s (%s) od %ds', $req->{command}, $req->{target},
            $req->{tag}, time() - $req->{started}));
    }
    return;
}

sub cmd_tk_recent {
    unless (@recent) {
        say_info('w tej sesji skrypt nic nie wyslal');
        return;
    }
    for my $entry (@recent) {
        my @when = localtime $entry->{time};
        say_info(sprintf('%02d:%02d:%02d %s %s %s %s',
            $when[2], $when[1], $when[0], $entry->{tag},
            $entry->{command}, $entry->{mask},
            $entry->{command} eq 'TKLINE' ? "$entry->{duration} :$entry->{reason}" : ''));
    }
    return;
}

sub cmd_tk_mask {
    my ($data, $window_server) = @_;
    my ($options, $rest) = parse_options('tk mask', $data) or return;
    my ($resolved, $req) = prepare_request('MASK', $options, 0, $window_server);
    return unless $resolved;

    my ($target) = ($rest // '') =~ /\A\s*(\S+)\s*\z/;
    unless (defined $target && valid_nick($target)) {
        say_info('uzycie: /tk mask [-mask ident|host|domain] <nick>');
        return;
    }
    $req->{target}   = $target;
    $req->{duration} = '';
    $req->{reason}   = '';
    $req->{dry}      = 1;
    return start_whois($resolved, $req);
}

sub usage_tkl {
    say_info('uzycie: /tkl [-server <tag>] [-mask ident|host|domain] [-force] [-dry] <nick|user@host> [czas] <powod>');
    say_info('przyklad: /tkl Spammer 30m flood reklamowy');
    return;
}

# plik pomocy (~/.erssi/help albo help_path): jest = /help pokazuje go jak
# kazda inna komende; brak (zwykle irssi) = pomoc wbudowana w skrypt
sub help_file {
    my ($name) = @_;
    for my $dir (Irssi::get_irssi_dir() . '/help', split /:/, Irssi::settings_get_str('help_path')) {
        return 1 if -f "$dir/" . lc $name;
    }
    return 0;
}

sub cmd_tk_help {
    if (help_file('tk')) { Irssi::command('help tk'); return }
    tk_help_text();
    return;
}

sub tk_help_text {
    say_info("tk.pl v$VERSION - czasowe K-linie (TKLINE) dla ircd 2.11 / IRCnet");
    say_info(' ');
    usage_tkl();
    say_info('  -mask ident  = ident@host (domyslnie, ~ident -> *ident)');
    say_info('  -mask host   = *@pelny.host');
    say_info('  -mask domain = *@*.domena, *@1.2.3.* dla IPv4, *@siec::/64 dla IPv6');
    say_info('  -dry         = pokaz komende, nie wysylaj');
    say_info('  -force       = pozwol na szeroka maske');
    say_info('  czas opcjonalny: 30s 10m 2h 1d 1w 1d12h; bez niego tk_default_time');
    say_info(' ');
    say_info('/untkl <user@host|nick>  - zdejmij tkline (dopasowanie doslowne)');
    say_info('/tklist [maska]          - STATS k, lista tklinii');
    say_info('/klist [maska]           - STATS K, lista stalych K-linii');
    say_info('/tk mask <nick>          - pokaz jaka maske zbudowalby skrypt');
    say_info('/tk pending | /tk recent | /tk yes | /tk no | /tk help');
    say_info(' ');
    say_info('ustawienia: tk_mask_type tk_default_time tk_max_time tk_confirm');
    say_info('            tk_allow_broad tk_require_oper tk_reason_max');
    say_info('            tk_whois_timeout_ms tk_audit');
    say_info('audyt: ' . audit_path());
    return;
}

my %SUBCOMMANDS = map { $_ => 1 } qw(help mask pending recent yes no list klist);

sub cmd_tk {
    my ($data, $server, $witem) = @_;
    my ($first) = ($data // '') =~ /\A\s*(\S+)/;
    if (defined $first && $SUBCOMMANDS{lc $first}) {
        Irssi::command_runsub('tk', $data, $server, $witem);
        return;
    }
    return cmd_tkl($data, $server, $witem);    # zgodnosc: /tk dzialalo jak /tkl
}

sub cmd_help {
    my ($data) = @_;
    return unless ($data // '') =~ /\A\s*(tk|tkl|untkl|tklist|klist)\s*\z/i;
    return if help_file($1);
    tk_help_text();
    Irssi::signal_stop();
    return;
}

# --- haki testowe (t/tk.t) -------------------------------------------
sub _test_guard {
    die 'funkcje _test_* sa dostepne tylko w testach' unless $ENV{TK_TESTING};
    return 1;
}

sub _test_reset_state {
    _test_guard();
    %pending = ();
    %confirm = ();
    %expect  = ();
    @recent  = ();
    return 1;
}

sub _test_pending_count { _test_guard(); return scalar keys %pending }
sub _test_recent_count  { _test_guard(); return scalar @recent }

sub UNLOAD {
    for my $req (values %pending) {
        Irssi::timeout_remove($req->{timeout}) if $req->{timeout};
    }
    %pending = ();
    %confirm = ();
    audit_event('script_unloaded');
    return;
}

# =====================================================================
#  Rejestracja
# =====================================================================

register_settings();
register_formats();

Irssi::command_bind('tkl',         'cmd_tkl',      'TKLINE');
Irssi::command_bind('tk',          'cmd_tk',       'TKLINE');
Irssi::command_bind('untkl',       'cmd_untkl',    'TKLINE');
Irssi::command_bind('tklist',      'cmd_tklist',   'TKLINE');
Irssi::command_bind('klist',       'cmd_klist',    'TKLINE');
Irssi::command_bind('tkhelp',      'cmd_tk_help',  'TKLINE');
Irssi::command_bind('tk help',     'cmd_tk_help',  'TKLINE');
Irssi::command_bind('tk mask',     'cmd_tk_mask',  'TKLINE');
Irssi::command_bind('tk pending',  'cmd_tk_pending', 'TKLINE');
Irssi::command_bind('tk recent',   'cmd_tk_recent',  'TKLINE');
Irssi::command_bind('tk yes',      'cmd_tk_yes',   'TKLINE');
Irssi::command_bind('tk no',       'cmd_tk_no',    'TKLINE');
Irssi::command_bind('tk list',     'cmd_tklist',   'TKLINE');
Irssi::command_bind('tk klist',    'cmd_klist',    'TKLINE');
Irssi::command_bind_first('help',  'cmd_help');

# Opcje dopiero po command_bind: dla komendy, ktorej jeszcze nie ma, erssi
# odrzuca command_set_options ("default critical") i -dry/-force/-mask/
# -server byly nieznanymi opcjami.
Irssi::command_set_options('tkl',     'force dry +mask +server');
Irssi::command_set_options('untkl',   'force dry +mask +server');
Irssi::command_set_options('tk mask', '+mask +server');

Irssi::signal_add('redir tk whois',      'sig_whois');
Irssi::signal_add('redir tk whois end',  'sig_whois_end');
Irssi::signal_add('redir tk whois none', 'sig_whois_none');
Irssi::signal_add('event notice',        'sig_notice');
Irssi::signal_add('event 481',           'sig_no_privileges');
Irssi::signal_add('event 421',           'sig_unknown_command');

audit_event('script_loaded', version => $VERSION);
Irssi::printformat(Irssi::MSGLEVEL_CLIENTCRAP(), 'tk_loaded', $VERSION);

1;
