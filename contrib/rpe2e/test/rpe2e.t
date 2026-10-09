#!/usr/bin/perl
# Unit tests of rpe2e.pl with the irssi API mocked (test/lib/Irssi.pm) and
# the real crypto modules (Crypt::NaCl::Sodium, FFI::Platypus, libsodium).
#
#   prove -I test/lib test/rpe2e.t        (from contrib/rpe2e)
#   (the modules may live in ~/perl5: PERL5LIB=~/perl5/lib/perl5 prove ...)
#   RPE2E_SCRIPT=/path/to/rpe2e.pl prove -I test/lib test/rpe2e.t
#
# Strings handed to the script are BYTE strings, exactly as erssi passes
# them (see test/lib/Irssi.pm). Expected wire values are built here by hand
# from the RPE2E v1.0 layout, not with the script's own helpers, so a bug
# shared by encryption and decryption cannot hide itself.
use strict;
use warnings;
use Test::More;
use File::Temp qw(tempdir);
use File::Basename qw(dirname);
use File::Spec;
use Cwd qw(abs_path);
use JSON::PP qw(decode_json encode_json);
use MIME::Base64 qw(decode_base64);
use Encode qw(encode);
use Irssi;

my $HERE   = dirname(abs_path(__FILE__));
# contrib/rpe2e/test/../rpe2e.pl in the erssi tree, rpe2e/test/../../rpe2e.pl
# in a scripts collection that keeps the script one level up.
my ($SCRIPT) = defined $ENV{RPE2E_SCRIPT} ? ($ENV{RPE2E_SCRIPT})
    : grep { -f } map { File::Spec->catfile($HERE, @$_, 'rpe2e.pl') } ['..'], ['..', '..'];
die "rpe2e.pl not found (set RPE2E_SCRIPT)\n" unless $SCRIPT && -f $SCRIPT;
my $DIR    = tempdir('rpe2e-t-XXXXXX', TMPDIR => 1, CLEANUP => 1);
$Irssi::DIR = $DIR;
my $KEYRING = "$DIR/rpe2e/keyring.json";

{
    package main;
    no warnings 'once';
    local @ARGV;
    my $ok = do $SCRIPT;
    die "loading $SCRIPT failed: " . ($@ || $! || 'false return') unless $ok;
}

# ── helpers ──────────────────────────────────────────────────────────

sub slurp { my ($p) = @_; open my $fh, '<:raw', $p or return undef; local $/; return scalar <$fh> }
sub spew  { my ($p, $d) = @_; open my $fh, '>:raw', $p or die "$p: $!"; print {$fh} $d; close $fh or die $! }

sub fresh_keyring {
    unlink glob("$DIR/rpe2e/*");
    main::ensure_identity();
    my $kr = main::load_keyring();
    return $kr;
}

sub enable {
    my ($ctx, $mode) = @_;
    my $kr = main::load_keyring();
    $kr->{channels}{$ctx} = { enabled => 1, mode => $mode // 'normal' };
    main::save_keyring($kr);
}

# Independent oracle for the AAD (RPE2E v1.0, repartee src/e2e/wire.rs):
# the context name goes in as its UTF-8 bytes, ONCE.
sub aad_oracle {
    my ($ctx_bytes, $msgid, $ts, $part, $total) = @_;
    return 'RPE2E01'
        . pack('n', length $ctx_bytes) . $ctx_bytes
        . pack('n', 8) . $msgid
        . pack('n', 8) . pack('q>', $ts)
        . pack('n', 1) . chr($part)
        . pack('n', 1) . chr($total);
}

# Decrypt one "PRIVMSG <target> :+RPE2E01 ..." line with our outgoing key.
sub open_wire {
    my ($line, $ctx_bytes) = @_;
    my ($body) = $line =~ /PRIVMSG \S+ :(.*)\z/s or return undef;
    my $w = main::parse_wire($body) or return undef;
    my $kr = main::load_keyring();
    my $sk = decode_base64($kr->{outgoing}{$ctx_bytes}{sk} // return undef);
    my $aad = aad_oracle($ctx_bytes, $w->{msgid}, $w->{ts}, $w->{part}, $w->{total});
    return main::aead_decrypt($sk, $w->{nonce}, $aad, $w->{ct});
}

# Run one raw line through the outbound gate, as erssi's
# "server outgoing modify" does; returns the line left for the socket.
sub gate {
    my ($server, $line) = @_;
    my $data = $line;
    Irssi::emit('server outgoing modify', $server, \$data, 1);
    return $data;
}

my $SODIUM = Crypt::NaCl::Sodium->new;

# A correctly signed KEYREQ from a peer identity of our own making.
sub peer_keyreq {
    my ($peer, $ctx) = @_;
    my ($eph_sk, $eph_pk) = main::generate_x25519_keypair();
    my $nonce = main::_raw(main::random_bytes(16));
    my $sig = main::ed25519_sign($peer->{sk}, main::_sig_payload_keyreq($ctx, $peer->{pk}, $eph_pk, $nonce));
    return 'RPEE2E KEYREQ v=1 c=' . $ctx
        . ' p=' . main::b64u_encode($peer->{pk})
        . ' e=' . main::b64u_encode($eph_pk)
        . ' n=' . main::b64u_encode($nonce)
        . ' s=' . main::b64u_encode($sig);
}

sub new_peer {
    my ($pk, $sk) = $SODIUM->sign->keypair;
    return { pk => main::_raw($pk), sk => main::_raw($sk) };
}

my $server = Irssi::Test::Server->new(tag => 'net', nick => 'me');
@Irssi::SERVERS = ($server);
my $ZAZOLC = "za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87";   # "zażółć" as UTF-8 bytes

# ── 1. outgoing text is UTF-8 encoded once (CRITICAL) ────────────────

subtest 'non-ASCII outgoing text reaches the peer unchanged' => sub {
    fresh_keyring();
    enable('#chan');
    @{ $server->{sent} } = ();
    my $left = gate($server, "PRIVMSG #chan :$ZAZOLC");
    is($left, '', 'plaintext line dropped');
    is(scalar @{ $server->{sent} }, 1, 'one ciphertext line');
    is(open_wire($server->{sent}[0], '#chan'), $ZAZOLC, 'decrypts to the same UTF-8 bytes');
};

subtest 'the 180-byte chunk budget counts real UTF-8 bytes' => sub {
    fresh_keyring();
    enable('#chan');
    @{ $server->{sent} } = ();
    my $text = "\xc5\xbc" x 90;                       # 90 x "ż" = 180 bytes
    gate($server, "PRIVMSG #chan :$text");
    is(scalar @{ $server->{sent} }, 1, '180 bytes fit one chunk');
    is(open_wire($server->{sent}[0], '#chan'), $text, 'and decrypt unchanged');
};

subtest 'a long non-ASCII /me splits into valid UTF-8 ACTION pieces' => sub {
    fresh_keyring();
    enable('#chan');
    @{ $server->{sent} } = ();
    my $body = "\xc5\xbc" x 120;                      # 240 bytes
    gate($server, "PRIVMSG #chan :\x01ACTION $body\x01");
    is(scalar @{ $server->{sent} }, 2, 'two pieces');
    my $joined = '';
    for my $line (@{ $server->{sent} }) {
        my $pt = open_wire($line, '#chan');
        like($pt, qr/\A\x01ACTION [^\x01]+\x01\z/, 'each piece is a whole ACTION frame');
        my ($piece) = $pt =~ /\A\x01ACTION (.*)\x01\z/s;
        ok(utf8::decode(my $copy = $piece), 'piece is valid UTF-8');
        $joined .= $piece;
    }
    is($joined, $body, 'pieces join to the original text');
};

subtest 'a non-ASCII channel name goes into the AAD as UTF-8 once' => sub {
    fresh_keyring();
    my $chan = "#\xc5\xbcaba";                        # "#żaba"
    enable($chan);
    @{ $server->{sent} } = ();
    gate($server, "PRIVMSG $chan :hello");
    is(open_wire($server->{sent}[0], $chan), 'hello', 'decrypts with the repartee AAD');
};

subtest 'keyring channel names written as real Unicode (repartee export, migration) still match' => sub {
    fresh_keyring();
    my $kr = decode_json(slurp($KEYRING));
    $kr->{channels}{"#\x{17c}aba"} = { enabled => 1, mode => 'normal' };   # "#żaba" as characters
    spew($KEYRING, encode_json($kr));
    @{ $server->{sent} } = ();
    my $left = gate($server, "PRIVMSG #\xc5\xbcaba :secret");
    is($left, '', 'not sent as plaintext');
    like($server->{sent}[0] // '', qr/:\+RPE2E01 /, 'encrypted');
};

# What erssi's C side gets back from signal_continue: SvPV, i.e. the UTF-8
# form of a character string, the bytes of a byte string.
sub as_c_string { my ($s) = @_; return utf8::is_utf8($s) ? encode('UTF-8', $s) : $s }

sub incoming_wire {
    my ($sk, $ctx_bytes, $pt_bytes) = @_;
    my $msgid = main::_raw(main::random_bytes(8));
    my $ts = time;
    my ($nonce, $ct) = main::aead_encrypt($sk, aad_oracle($ctx_bytes, $msgid, $ts, 1, 1), $pt_bytes);
    return main::encode_wire($msgid, $ts, 1, 1, $nonce, $ct);
}

subtest 'incoming non-ASCII text on a non-ASCII channel is routed unchanged' => sub {
    fresh_keyring();
    my $sk = main::_raw(main::random_bytes(32));
    for my $chan ('#chan', "#\xc5\xbcaba") {
        my $kr = main::load_keyring();
        $kr->{incoming}{"peer\@peer.example|$chan"} = { fp => 'cd' x 16, sk => main::b64e($sk), status => 'trusted' };
        main::save_keyring($kr);
        my $wire = incoming_wire($sk, $chan, $ZAZOLC);
        my ($stopped, $cont) = Irssi::emit('event privmsg', $server, "$chan :$wire", 'peer', 'peer@peer.example');
        ok($cont, "$chan: decrypted text re-emitted");
        is(as_c_string($cont->[1] // ''), "$chan :$ZAZOLC", "$chan: target and text reach erssi as sent");
    }
};

# ── 2. the keyring is never replaced after a failed load or write ────

subtest 'an unreadable keyring is not overwritten' => sub {
    plan skip_all => 'running as root, file modes are not enforced' if $> == 0;
    fresh_keyring();
    my $before = slurp($KEYRING);
    chmod 0000, $KEYRING;
    my $chan = Irssi::Test::Item->new(name => '#chan', server => $server);
    $Irssi::COMMANDS{e2e}->('on', $server, $chan);
    eval { main::ensure_identity() };
    chmod 0600, $KEYRING;
    is(slurp($KEYRING), $before, 'keyring file unchanged');
    like(Irssi::printed_text(), qr/cannot read keyring/i, 'the user is told why');
};

subtest 'a corrupt keyring is moved aside before starting fresh' => sub {
    fresh_keyring();
    spew($KEYRING, '{"identity": {"pk": "trunc');
    @Irssi::PRINTED = ();
    main::load_keyring();
    my @aside = glob("$KEYRING.corrupt-*");
    is(scalar @aside, 1, 'one corrupt copy kept');
    is(slurp($aside[0] // ''), '{"identity": {"pk": "trunc', 'with the original content');
    like(Irssi::printed_text(), qr/\Q$aside[0]\E/, 'the user is told where');
    unlink @aside;
};

subtest 'a failed write (disk full) keeps the old keyring' => sub {
    fresh_keyring();
    my $kr = main::load_keyring();
    $kr->{peers}{ sprintf('%032x', $_) } = { pk => 'x' x 44, last_nick => "n$_" } for 1 .. 400;
    main::save_keyring($kr);
    my $before = slurp($KEYRING);
    ok(length($before) > 8192, 'keyring is larger than the limit below');
    # A child process with RLIMIT_FSIZE of 4 KiB: writing more fails with EFBIG.
    my $code = q{
        $SIG{XFSZ} = "IGNORE";
        $Irssi::DIR = shift;
        do shift or die $@;
        my $kr = load_keyring();
        $kr->{peers}{"ff" x 16} = { pk => "y" x 44 };
        save_keyring($kr);
    };
    my $inc = join ' ', map { "-I$_" } grep { !ref } @INC;
    system('sh', '-c', qq{ulimit -f 4; exec "\$0" $inc -e '$code' "\$1" "\$2" 2>/dev/null}, $^X, $DIR, $SCRIPT);
    is(slurp($KEYRING), $before, 'keyring file unchanged');
    is(scalar(() = glob("$DIR/rpe2e/keyring.json.tmp.*")), 0, 'no temporary file left');
};

# ── 3. RPEE2E NOTICEs write the keyring only when something changed ──

subtest 'an invalid KEYREQ does not rewrite the keyring' => sub {
    fresh_keyring();
    enable('#chan');
    my $ino = (stat $KEYRING)[1];
    Irssi::emit('ctcp reply', $server, 'RPEE2E KEYREQ v=1 c=#chan p=AA e=AA n=AA s=AA x=1',
                'mallory', 'm@evil.example', 'me');
    Irssi::emit('ctcp reply', $server, 'RPEE2E KEYRSP v=1 c=#chan p=AA e=AA wn=AA w=AA n=AA s=AA',
                'mallory', 'm@evil.example', 'me');
    is((stat $KEYRING)[1], $ino, 'same file (no rename)');
};

# ── 4. a KEYREQ flood does not turn into a KEYRSP flood ──────────────

subtest 'KEYREQs from one sender are answered at most once per interval' => sub {
    fresh_keyring();
    enable('#auto', 'auto-accept');
    my $eve = new_peer();
    @{ $server->{sent} } = ();
    for (1 .. 5) {
        Irssi::emit('ctcp reply', $server, peer_keyreq($eve, '#auto'), 'eve', 'eve@flood.example', 'me');
    }
    my @rsp = grep { /KEYRSP/ } @{ $server->{sent} };
    is(scalar @rsp, 1, 'one KEYRSP for five KEYREQs');
};

# ── 5. only bot-command-shaped lines bypass encryption ───────────────

subtest 'ellipsis and "!!" lines are encrypted, bot commands are not' => sub {
    fresh_keyring();
    enable('#bots');
    for my $text ('...so what', '!!', '. ', '!?') {
        @{ $server->{sent} } = ();
        my $left = gate($server, "PRIVMSG #bots :$text");
        is($left, '', "'$text' not sent as plaintext");
        like($server->{sent}[0] // '', qr/:\+RPE2E01 /, "'$text' encrypted");
    }
    for my $text ('.op me', '!seen bob') {
        my $line = "PRIVMSG #bots :$text";
        is(gate($server, $line), $line, "'$text' goes out for the bot");
    }
};

# ── 6. /e2e export and import do not expose or lose keys ─────────────

subtest '/e2e export refuses to follow a symlink or overwrite a file' => sub {
    fresh_keyring();
    my $witem = Irssi::Test::Item->new(name => '#chan', server => $server);
    my $victim = "$DIR/victim";
    spew($victim, 'precious');
    symlink $victim, "$DIR/link" or die $!;
    $Irssi::COMMANDS{e2e}->("export $DIR/link", $server, $witem);
    is(slurp($victim), 'precious', 'symlink target untouched');
    $Irssi::COMMANDS{e2e}->("export $victim", $server, $witem);
    is(slurp($victim), 'precious', 'existing file untouched');
    my $old = umask 022;
    $Irssi::COMMANDS{e2e}->("export $DIR/new.json", $server, $witem);
    umask $old;
    ok(-s "$DIR/new.json", 'export to a new file works');
    is((stat "$DIR/new.json")[2] & 07777, 0600, 'and is private');
    unlink $victim, "$DIR/link", "$DIR/new.json";
};

subtest '/e2e import keeps a backup of the replaced keyring' => sub {
    fresh_keyring();
    my $before = slurp($KEYRING);
    my $witem = Irssi::Test::Item->new(name => '#chan', server => $server);
    spew("$DIR/import.json", encode_json({ version => 1, identity => undef, channels => [] }));
    $Irssi::COMMANDS{e2e}->("import $DIR/import.json", $server, $witem);
    my @bak = glob("$KEYRING.bak-*");
    is(scalar @bak, 1, 'one backup');
    is(slurp($bak[0] // ''), $before, 'holding the previous keyring');
    is((stat($bak[0] // ''))[2] & 07777, 0600, 'private');
    unlink @bak, "$DIR/import.json";
};

# ── 7. the gate also sees lower-case and tagged PRIVMSG lines ────────

subtest '/quote privmsg and tagged PRIVMSG are encrypted too' => sub {
    fresh_keyring();
    enable('#chan');
    @{ $server->{sent} } = ();
    is(gate($server, 'privmsg #chan :secret'), '', 'lower-case line dropped');
    like($server->{sent}[0] // '', qr/^PRIVMSG #chan :\+RPE2E01 /, 'and encrypted');
    @{ $server->{sent} } = ();
    is(gate($server, '@+draft/reply=abc PRIVMSG #chan :secret'), '', 'tagged line dropped');
    like($server->{sent}[0] // '', qr/^\@\+draft\/reply=abc PRIVMSG #chan :\+RPE2E01 /,
         'and encrypted with its tags kept');
};

# ── 8. the /e2e guide in a query ─────────────────────────────────────

subtest '/e2e guide in a query lists the peer\'s DM keys' => sub {
    fresh_keyring();
    Irssi::emit('event 311', $server, 'me me myid own.example * :real');
    my $q = Irssi::Test::Item->new(name => 'bob', type => 'QUERY', address => 'bob@bob.example', server => $server);
    my $kr = main::load_keyring();
    $kr->{channels}{'@bob@bob.example'} = { enabled => 1, mode => 'normal' };
    $kr->{incoming}{'bob@bob.example|@myid@own.example'} = { fp => 'ab' x 16, sk => 'A' x 44, status => 'trusted' };
    main::save_keyring($kr);
    @Irssi::PRINTED = ();
    $Irssi::COMMANDS{e2e}->('', $server, $q);
    like(Irssi::printed_text(), qr/Keys from: bob\@bob\.example/, 'trusted DM key shown');
    my $carol = Irssi::Test::Item->new(name => 'carol', type => 'QUERY', server => $server);
    @Irssi::PRINTED = ();
    $Irssi::COMMANDS{e2e}->('', $server, $carol);
    unlike(Irssi::printed_text(), qr/Run \/e2e in a channel or query window/, 'no "run it in a query" in a query');
    like(Irssi::printed_text(), qr/carol/, 'names the peer whose address is unknown');
};

# ── 9. the version gate ──────────────────────────────────────────────

subtest 'version gate: erssi under another binary name loads' => sub {
    local $Irssi::BINARY = '/home/u/bin/irc';
    local %Irssi::SPECIAL = ('$J' => '1.3.10', '$abiversion' => 58);
    ok(eval { main::_require_signal_capable_irssi(); 1 }, 'loads') or diag $@;
};

subtest 'version gate: old irssi still refuses, and names erssi' => sub {
    local $Irssi::BINARY = '/usr/bin/irssi';
    local $Irssi::ABI_DATE = 20200101;
    local %Irssi::SPECIAL = ('$J' => '1.2.3', '$abiversion' => 20);
    ok(!eval { main::_require_signal_capable_irssi(); 1 }, 'refuses');
    like($@, qr/irssi >= 1\.4\.1 or erssi/, 'message mentions erssi');
};

# ── 10. use lib with HOME unset ──────────────────────────────────────

subtest 'loading with HOME unset adds nothing odd to @INC and does not warn' => sub {
    my $code = q{
        $Irssi::DIR = shift; my $s = shift;
        do $s or die $@;
        print grep({ m{^/perl5/} } @INC) ? "BAD-INC\n" : "INC-OK\n";
    };
    my $inc = join ' ', map { "-I$_" } grep { !ref } @INC;
    my $d2 = tempdir('rpe2e-t2-XXXXXX', TMPDIR => 1, CLEANUP => 1);
    my $out = `env -u HOME "$^X" $inc -e '$code' "$d2" "$SCRIPT" 2>&1`;
    like($out, qr/INC-OK/, 'no /perl5/... entry');
    unlike($out, qr/uninitialized/, 'no warning');
};

# ── 11. "%" in printed text is not a theme code ──────────────────────

subtest '"%" in names is printed literally' => sub {
    my $w = Irssi::Test::Item->new(name => '#100%', server => $server);
    @Irssi::PRINTED = ();
    main::_prnt_ok($w, 'enabled on #100%');
    main::_prnt_warn(undef, '50% off');
    main::_prnt_err($w, '%n');
    is_deeply([map { $_->{text} } @Irssi::PRINTED],
              ['[E2E] enabled on #100%%', '[E2E] 50%% off', '[E2E] %%n'], 'escaped');
};

# ── 12. rate-limit maps are pruned ───────────────────────────────────

subtest 'rate-limit stamps expire' => sub {
    my %m;
    no warnings 'redefine';
    local *main::now_unix = sub { 1000 };
    ok(main::_stamp_allow(\%m, "h$_", 30), "first h$_") for 1 .. 300;
    ok(!main::_stamp_allow(\%m, 'h1', 30), 'repeat inside the interval refused');
    local *main::now_unix = sub { 2000 };
    ok(main::_stamp_allow(\%m, 'new', 30), 'new key after the interval');
    is(scalar keys %m, 1, 'expired stamps were dropped');
};

done_testing;
