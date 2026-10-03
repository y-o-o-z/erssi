# webjournal.pl - dziennik okien erssi dla klienta web (NexusIRC)
#
# Web laczy sie z erssi przez fe-web i dostaje zdarzenia tylko wtedy, gdy
# jest polaczony. Ten skrypt zapisuje na dysk wszystko, co widac w oknach
# erssi, zeby web po restarcie uzupelnil dziury i pokazal to samo co konsola:
#
#   * kanaly i rozmowy - wpisy strukturalne (czas, rodzaj, nick, tresc),
#     z ktorych web odtwarza zwykle wiadomosci,
#   * okna bez kanalu (Notices, Mentions, skaner i inne nazwane okna oraz
#     okno statusu sieci nazwane jak jej tag) - gotowe linie tekstu; web
#     pokazuje je na zywo, czytajac koncowke pliku,
#   * komunikaty klienta i skryptow w oknach kanalow (poziomy CLIENT*, np.
#     "[E2E] ..." z rpe2e.pl) - tez jako linie tekstu w pliku kanalu; fe-web
#     ich nie przesyla, a bez nich w webie nie byloby widac np. prosby
#     o wymiane kluczy.
#
# Uklad katalogu (webjournal_dir, domyslnie ~/.erssi/journal):
#   <tag>/<kanal|nick>.jsonl     np. ircnet/#polska.jsonl
#   <tag>/%status.jsonl          okno statusu sieci <tag>
#   %windows/<nazwa okna>.jsonl  np. %windows/notices.jsonl
# Nazwy: male litery ASCII, znaki spoza [a-z0-9#&+!._-] jako %XX (bajty
# UTF-8). "%status" i "%windows" nie moga powstac z zakodowanej nazwy.
#
# Wpis to jedna linia JSON (UTF-8), pola:
#   t   czas (sekundy, ulamek)      k  rodzaj: msg action notice join part
#   n   nick nadawcy                    quit kick topic mode nick text
#   h   user@host                   x  tresc (bez kodow kolorow dla "text")
#   s   1 = wlasna wiadomosc        hl 1 = podswietlenie (nick w tresci)
#   tg  nick wyrzucony (kick)       nn nowy nick (nick)
#   w   nazwa okna (linie tekstu, z oryginalna wielkoscia liter)
# Plik wiekszy niz webjournal_max_kb przechodzi w <plik>.1 (poprzedni .1
# znika), a zapis idzie od nowa - czytajacy wykrywa to po zmniejszeniu pliku.
#
# Ustawienia: webjournal (ON), webjournal_dir, webjournal_max_kb (2048)
# Komenda:    /webjournal - stan dziennika

use strict;
use warnings;

use Irssi;
use JSON::PP ();
use Time::HiRes ();
use File::Path qw(make_path);
use Fcntl qw(O_WRONLY O_APPEND O_CREAT);

our $VERSION = '1.2.3';
our %IRSSI = (
    authors     => 'yooz',
    contact     => 'https://github.com/y-o-o-z',
    name        => 'webjournal',
    description => 'Dziennik okien erssi (kanaly, rozmowy, Notices, Mentions, status) dla klienta web',
    license     => 'MIT',
    url         => 'https://github.com/y-o-o-z/irssi_scripts',
);

Irssi::settings_add_bool('webjournal', 'webjournal',        1);
Irssi::settings_add_str('webjournal',  'webjournal_dir',    '~/.erssi/journal');
Irssi::settings_add_int('webjournal',  'webjournal_max_kb', 2048);

my $JSON = JSON::PP->new->utf8(1)->canonical(1);
my %stats = (written => 0, rotated => 0, errors => 0, last_error => '');

# Otwarte pliki (sciezka => uchwyt): dlugie wyjscia, np. /list w oknie
# statusu, to tysiace linii - bez otwierania pliku na kazda z nich. Najwyzej
# $FH_MAX naraz; nadmiar zamykany od najdawniej uzywanego.
my %fh;
my %fh_used;
my $FH_MAX = 64;
my $fh_clock = 0;

# ── nazwy plikow ─────────────────────────────────────────────────────

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

# ── zapis ────────────────────────────────────────────────────────────

# Tekst z irssi bywa ciagiem znakow (flaga UTF-8) albo surowymi bajtami.
# Ujednolicamy na znaki, zeby JSON nie zakodowal bajtow UTF-8 drugi raz.
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

# Plik tworzony od razu z prawami 0600 (sysopen), katalog 0700; zapis bez
# buforowania, zeby czytajacy (Nexus) widzial cale linie od razu.
sub handle_for {
    my ($file) = @_;
    $fh_used{$file} = ++$fh_clock;
    if (my $open = $fh{$file}) {
        # Plik skasowany albo podmieniony z zewnatrz (rm, logrotate) przy
        # otwartym uchwycie: zapis szedlby do niewidocznego juz pliku.
        # Ten sam plik = to samo urzadzenie i i-wezel; inaczej otwieramy od nowa.
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

# ── kanaly i rozmowy ────────────────────────────────────────────────

sub is_channel {
    my ($server, $target) = @_;
    return 0 unless defined $target && length $target;
    my $ok = eval { $server->ischannel($target) };
    return $ok if defined $ok;
    return $target =~ /^[#&!+]/ ? 1 : 0;
}

# Jak hilight_nick_matches_everywhere: nick jako osobne slowo w tresci.
sub mentions_me {
    my ($server, $text) = @_;
    my $nick = $server->{nick};
    return 0 unless defined $nick && length $nick && defined $text;
    return $text =~ /(?<![\w\[\]\\`^{|}-])\Q$nick\E(?![\w\[\]\\`^{|}-])/i ? 1 : 0;
}

sub ignored {
    my ($server, $nick, $address, $target, $text, $level) = @_;
    # brak adresu (wiadomosc od serwera) albo celu to celowo NULL dla irssi
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

# Cel z prefiksem statusu (@#kanal, +#kanal - wiadomosc tylko dla opow lub
# voice) zapisujemy w pliku kanalu.
sub channel_of {
    my ($server, $target) = @_;
    return $target if is_channel($server, $target);
    (my $bare = $target // '') =~ s/^[@+%~]+//;
    return length $bare && $bare ne $target && is_channel($server, $bare) ? $bare : undef;
}

# NOTICE na kanal - do kanalu; prywatny do otwartej rozmowy z nadawca (erssi
# pokazuje go wtedy w jej oknie), bez rozmowy - trafia do okna Notices i do
# dziennika jako linia tekstu.
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

# PRIVMSG @#kanal: erssi wysyla go jako "message irc op_public", nie "message public".
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

# QUIT i NICK nie maja kanalu - zapisujemy je we wszystkich kanalach, na
# ktorych jest nick, oraz w otwartej rozmowie z nim. Sygnaly "message ..."
# niosa tekst juz przekodowany (recode), tak jak widzi go erssi:
#   message quit - lista nickow jeszcze z odchodzacym (fe-messages tez z niej
#                  korzysta, zeby wypisac quit na kanalach),
#   message nick - lista nickow juz z NOWYM nickiem, rozmowa bywa pod
#                  nowym albo starym.
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

# ── okna bez kanalu ──────────────────────────────────────────────────

sub window_has_items {
    my ($window) = @_;
    my @items = eval { $window->items };
    return scalar @items;
}

sub sig_print_text {
    my ($dest, $text, $stripped) = @_;
    my $window = ref $dest ? $dest->{window} : undef;
    return unless $window;
    # MSGLEVEL_NEVER: linie tylko na ekran (np. kreska trackbar.pl)
    return if ($dest->{level} // 0) & MSGLEVEL_NEVER;

    $stripped = Irssi::strip_codes($text // '') unless defined $stripped;
    # wciecie kolumny z motywu (np. "        erssi │ ...") nie ma sensu w webie
    $stripped =~ s/^\s+//;
    $stripped =~ s/\s+$//;
    return unless length $stripped;
    my $hl = ($dest->{level} // 0) & MSGLEVEL_HILIGHT ? 1 : 0;

    if (window_has_items($window)) {
        # Okno kanalu/rozmowy: wiadomosci ida wpisami strukturalnymi, tu tylko
        # komunikaty klienta i skryptow.
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

# ── komenda ──────────────────────────────────────────────────────────

sub cmd_webjournal {
    my $state = Irssi::settings_get_bool('webjournal') ? 'wlaczony' : 'WYLACZONY (/set webjournal on)';
    Irssi::print("webjournal $VERSION: $state, katalog " . base_dir()
        . ", zapisanych wpisow $stats{written}, rotacji $stats{rotated}, bledow $stats{errors}"
        . ($stats{errors} ? " (ostatni: $stats{last_error})" : ''), MSGLEVEL_CLIENTCRAP);
}

Irssi::signal_add('message public',        \&sig_public);
Irssi::signal_add('message own_public',    \&sig_own_public);
Irssi::signal_add('message private',       \&sig_private);
Irssi::signal_add('message own_private',   \&sig_own_private);
Irssi::signal_add('message irc action',    \&sig_action);
Irssi::signal_add('message irc own_action', \&sig_own_action);
Irssi::signal_add('message irc notice',    \&sig_notice);
Irssi::signal_add('message irc own_notice', \&sig_own_notice);
Irssi::signal_add('message join',          \&sig_join);
Irssi::signal_add('message part',          \&sig_part);
Irssi::signal_add('message kick',          \&sig_kick);
Irssi::signal_add('message topic',         \&sig_topic);
Irssi::signal_add('message irc mode',      \&sig_mode);
Irssi::signal_add('message irc op_public', \&sig_op_public);
Irssi::signal_add('message quit',          \&sig_quit);
Irssi::signal_add('message nick',          \&sig_nick);
Irssi::signal_add('message own_nick',      \&sig_own_nick);
Irssi::signal_add('print text',            \&sig_print_text);
Irssi::command_bind('webjournal',          \&cmd_webjournal);
