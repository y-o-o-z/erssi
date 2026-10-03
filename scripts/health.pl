# health.pl - stan erssi dla czujek: co minute zapisuje ~/.erssi/health.json
#
# Plik (0600, zapis atomowy) mowi, czy erssi zyje i co widzi:
#   {"time":..., "pid":..., "version":..., "servers":[{"tag","chatnet",
#    "connected","nick","lag_ms","channels":[...]}], "autojoin":{chatnet:[...]}}
# "autojoin" to kanaly z autojoinem z konfiguracji - czujka porownuje je
# z "channels", wiec widzi kanal, z ktorego erssi wylecialo (ban, kick).
# Czyta go `ircnet health`; czujka na homelabie alarmuje, gdy plik jest
# stary (erssi zawieszone) albo czegos brakuje.
#
# Ustawienia: health_file (domyslnie <katalog erssi>/health.json),
#             health_interval (sekundy, 60)

use strict;
use warnings;
use Irssi;
use JSON::PP ();
use Fcntl qw(O_WRONLY O_CREAT O_TRUNC);

our $VERSION = '1.0.0';
our %IRSSI = (
    authors     => 'yooz',
    contact     => 'https://github.com/y-o-o-z',
    name        => 'health',
    description => 'Writes erssi state (servers, channels, lag) to a JSON file every minute for watchdogs',
    license     => 'MIT',
    url         => 'https://github.com/y-o-o-z/irssi_scripts',
);

Irssi::settings_add_str('health', 'health_file', '');
Irssi::settings_add_int('health', 'health_interval', 60);

my $timer;

sub health_file {
    my $f = Irssi::settings_get_str('health_file');
    return length $f ? $f : Irssi::get_irssi_dir() . '/health.json';
}

# kanaly z autojoinem z pliku konfiguracji (sekcja channels)
sub autojoin_channels {
    my %aj;
    my $file = Irssi::get_irssi_config();
    open(my $fh, '<', $file) or return \%aj;
    local $/;
    my $cfg = <$fh>;
    close $fh;
    my ($section) = $cfg =~ /^channels\s*=\s*\((.*?)^\);/ms or return \%aj;
    while ($section =~ /\{([^{}]*)\}/g) {
        my $block = $1;
        next unless $block =~ /\bautojoin\s*=\s*"?(yes|on|1)"?/i;
        my ($name) = $block =~ /\bname\s*=\s*"([^"]+)"/;
        my ($net)  = $block =~ /\bchatnet\s*=\s*"([^"]+)"/;
        push @{ $aj{$net} }, $name if defined $name && defined $net;
    }
    return \%aj;
}

sub write_health {
    my @servers;
    for my $s (Irssi::servers()) {
        push @servers, {
            tag       => $s->{tag},
            chatnet   => $s->{chatnet} // '',
            connected => $s->{connected} ? JSON::PP::true : JSON::PP::false,
            nick      => $s->{nick} // '',
            lag_ms    => $s->{lag} // 0,
            channels  => [ map { $_->{name} } $s->channels() ],
        };
    }
    my $data = {
        time     => time,
        pid      => $$,
        version  => Irssi::parse_special('$J'),
        servers  => \@servers,
        autojoin => autojoin_channels(),
    };
    my $file = health_file();
    my $tmp  = "$file.tmp";
    sysopen(my $fh, $tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600) or return;
    print $fh JSON::PP->new->canonical->encode($data);
    close $fh or return;
    rename $tmp, $file;
}

sub restart_timer {
    Irssi::timeout_remove($timer) if $timer;
    my $sec = Irssi::settings_get_int('health_interval');
    $sec = 60 if $sec < 10;
    $timer = Irssi::timeout_add($sec * 1000, \&write_health, '');
}

Irssi::signal_add('setup changed', \&restart_timer);
for my $sig ('server connected', 'server disconnected', 'channel joined', 'channel destroyed') {
    Irssi::signal_add_last($sig, sub { Irssi::timeout_add_once(500, \&write_health, '') });
}
restart_timer();
write_health();

sub UNLOAD {
    Irssi::timeout_remove($timer) if $timer;
    unlink health_file();
}

1;
