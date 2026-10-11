#!/usr/bin/env bash
# live-interop.sh - the native e2e module against rpe2e.pl, live
#
#   live-interop.sh <meson build dir> <source dir>
#
# Two erssi clients of this build on a local fake IRC server
# (tests/e2e/e2eircd.py, 127.0.0.1 only):
#   alice - the NATIVE module (src/e2e),
#   bob   - the rpe2e.pl script from repartee ($RPE2E_SCRIPT),
#           with the native module loaded as well: it must notice the
#           script and stay out of the way.
# They exchange keys and messages in both directions on a channel and in a
# query: plain text, Polish text, a long message that is chunked, /me,
# "..." and /quote privmsg. Checked: each side reads the other in plain
# text (erssi logs), and the server never saw any of it in plain text.
# In normal mode each side accepts the other once (/e2e accept).
#
# A third client, mallory (native module, auto-accept), plays the outsider
# of the security review: his messages make alice ask him for his key, but
# alice never sends him hers without /e2e accept. Then, on alice: a script
# binding /e2e does not switch the module off, a raw line with CR LF and a
# PRIVMSG sent from a rawlog hook do not leave in clear text, and /upgrade
# with PRIVMSGs still in the flood queue encrypts them. A query with E2E on
# whose nick (carol) is no longer reachable is refused, not sent in clear
# text, until /e2e off in that query. Last, rpe2e.pl
# loads and saves alice's keyring and keeps what only the module writes.
#
# A second fake server plays a second network (chatnet E2E2): alice is on
# both, erin (native module) on E2E2 only. alice's keyring keeps the two
# networks apart ("E2E\x1f#test", "E2E2\x1f#test", each with its own key),
# while rpe2e.pl's (bob's) stays unscoped, and everything on the wire is
# bare. At the very end bob unloads rpe2e.pl and the native module takes
# his old, unscoped keyring over (one network): nothing to do for him.
#
# Each client has its own --home and HOME under a temporary directory and
# runs in a private tmux server (-L e2e-live-<pid>); only that tmux server
# and the fake server started here are stopped at the end.
#
# Needs: tmux, python3, the build with Perl support, and the Perl modules
# of rpe2e.pl (Crypt::NaCl::Sodium, FFI::Platypus - e.g. PERL5LIB pointing
# to a local::lib). Without them the test is skipped (exit 77).
set -u

BUILD=$(cd "${1:?build dir}" && pwd)
SRC=$(cd "${2:?source dir}" && pwd)
SCRIPT=${RPE2E_SCRIPT:-}
IRCD=$SRC/tests/e2e/e2eircd.py

skip() { echo "SKIP: $*"; exit 77; }
command -v tmux >/dev/null 2>&1 || skip "tmux not found"
command -v python3 >/dev/null 2>&1 || skip "python3 not found"
[ -n "$SCRIPT" ] && [ -f "$SCRIPT" ] || skip "set RPE2E_SCRIPT to a copy of rpe2e.pl (from repartee) to run this test"
[ -f "$IRCD" ] || skip "$IRCD not found"
[ -f "$BUILD/src/perl/libperl_core.so" ] || skip "erssi built without Perl"
[ -f "$BUILD/src/e2e/libe2e_core.so" ] || skip "erssi built without e2e"
perl -MCrypt::NaCl::Sodium -MFFI::Platypus -MFFI::CheckLib -e 1 2>/dev/null ||
    skip "Perl modules of rpe2e.pl missing (Crypt::NaCl::Sodium, FFI::Platypus; set PERL5LIB)"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/e2e-live.XXXXXX")
SOCK="e2e-live-$$"
CHAN="#test"
IRCD_PID=
ok=0
fail=0
pass() { printf '  ok   %s\n' "$*"; ok=$((ok + 1)); }
bad()  { printf '  FAIL %s\n' "$*"; fail=$((fail + 1)); }

cleanup() {
    tmux -L "$SOCK" kill-server 2>/dev/null || true
    if [ -n "$IRCD_PID" ]; then kill $IRCD_PID 2>/dev/null || true; fi
    if [ "${KEEP_WORK:-}" = 1 ]; then echo "work dir kept: $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# this build, installed under $WORK (modules and Perl libraries in the
# installed layout); the binary finds the modules through <home>/modules
meson install -C "$BUILD" --destdir "$WORK/inst" --no-rebuild --quiet >"$WORK/install.log" 2>&1 ||
    { cat "$WORK/install.log"; echo "meson install failed"; exit 1; }
ERSSI=$(find "$WORK/inst" -path '*/bin/erssi' -type f | head -n 1)
MODDIR=$(dirname "$(find "$WORK/inst" -name libirc_core.so | head -n 1)")
PERLLIB=$(dirname "$(find "$WORK/inst" -name Irssi.pm | head -n 1)")
[ -x "$ERSSI" ] && [ -d "$MODDIR" ] && [ -d "$PERLLIB" ] || { echo "installed tree incomplete"; exit 1; }

PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
PORT2=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
# E2E compares names in ASCII ([ and { are two channels), E2E2 by Unicode
python3 "$IRCD" --port "$PORT" --wire "$WORK/wire.log" --casemapping ascii &
IRCD_PID=$!
python3 "$IRCD" --port "$PORT2" --wire "$WORK/wire2.log" --network E2E2Test --casemapping utf8 &
IRCD_PID="$IRCD_PID $!"
for port in "$PORT" "$PORT2"; do
    i=0
    while ! python3 -c "import socket; socket.create_connection(('127.0.0.1', $port), 1)" 2>/dev/null; do
        i=$((i + 1)); [ $i -gt 50 ] && { echo "test server did not start"; exit 1; }
        sleep 0.1
    done
done

for nick in alice bob mallory carol erin; do
    home="$WORK/$nick"
    mkdir -p "$home/modules" "$home/logs" "$home/scripts/autorun" "$WORK/home-$nick"
    for so in "$MODDIR"/*.so; do ln -s "$so" "$home/modules/"; done
    [ "$nick" = bob ] && ln -s "$SCRIPT" "$home/scripts/autorun/rpe2e.pl"
    # the theme of this source tree, from --home: without it erssi reads
    # the "shellter" theme from THEMESDIR (the prefix, not the --destdir
    # tree), when one is installed there, and the log lines the long
    # message check reads depend on what this machine has installed
    cp "$SRC/themes/default.theme" "$home/default.theme"
    # alice on both networks, erin on E2E2 only, the others on E2E
    case $nick in
    alice) nets="E2E:$PORT E2E2:$PORT2" ;;
    erin)  nets="E2E2:$PORT2" ;;
    *)     nets="E2E:$PORT" ;;
    esac
    srv= net= chn=
    for n in $nets; do
        srv="$srv${srv:+, }{ address = \"127.0.0.1\"; chatnet = \"${n%%:*}\"; port = \"${n#*:}\"; autoconnect = \"yes\"; }"
        net="$net ${n%%:*} = { type = \"IRC\"; };"
        chn="$chn${chn:+, }{ name = \"$CHAN\"; chatnet = \"${n%%:*}\"; autojoin = \"yes\"; }"
    done
    cat > "$home/config" <<EOF
servers = ( $srv );
chatnets = {$net };
channels = ( $chn );
logs = { "$home/all.log" = { auto_open = "yes"; level = "ALL"; }; };
settings = {
  core = { real_name = "e2e test"; user_name = "$nick"; nick = "$nick"; };
  "fe-common/core" = { theme = "default"; autolog = "yes"; autolog_path = "$home/logs/\$tag/\$0.log"; autolog_level = "ALL"; };
  "perl/core" = { perl_use_lib = "$PERLLIB"; };
};
EOF
    # ASan (a sanitizer build): reports go to files, checked at the end
    tmux -L "$SOCK" new-session -d -s "$nick" -x 220 -y 50 \
        "env -u LC_ALL LANG=C.UTF-8 HOME=$WORK/home-$nick PERL5LIB=${PERL5LIB:-} ASAN_OPTIONS=detect_leaks=0:log_path=$WORK/asan-$nick UBSAN_OPTIONS=print_stacktrace=1:log_path=$WORK/ubsan-$nick $ERSSI --home=$home"
done

say() {  # say <nick> <text>: type one line into that erssi
    tmux -L "$SOCK" send-keys -t "$1" -l -- "$2"
    tmux -L "$SOCK" send-keys -t "$1" Enter
    sleep 1
}

quick() {  # quick <nick> <text>: the same, without waiting
    tmux -L "$SOCK" send-keys -t "$1" -l -- "$2"
    tmux -L "$SOCK" send-keys -t "$1" Enter
    sleep 0.2
}

wait_for() {  # wait_for <file> <fixed string> <seconds>
    local n=0
    while [ $n -lt "$3" ]; do
        grep -qF -- "$2" "$1" 2>/dev/null && return 0
        sleep 1
        n=$((n + 1))
    done
    return 1
}

echo "erssi:    $ERSSI"
echo "rpe2e.pl: $SCRIPT"

n=0
until grep -q "bob.*joined $CHAN" "$WORK/alice/logs/E2E/$CHAN.log" 2>/dev/null ||
      grep -q "alice.*joined $CHAN" "$WORK/bob/logs/E2E/$CHAN.log" 2>/dev/null; do
    n=$((n + 1)); [ $n -gt 40 ] && { bad "clients did not join $CHAN"; exit 1; }
    sleep 1
done
pass "alice and bob on $CHAN"

for nick in alice bob; do
    if wait_for "$WORK/$nick/rpe2e/keyring.json" '"identity"' 15; then
        pass "$nick: identity in rpe2e/keyring.json"
    else
        bad "$nick: no identity in the keyring"
    fi
done
# bob runs rpe2e.pl AND has the native module loaded: the module must
# notice the script (whichever loaded first) and leave everything to it
if grep -qF "Error in script" "$WORK/bob/all.log" 2>/dev/null; then
    bad "bob: rpe2e.pl did not load"; sed -n '/Error in script/,+3p' "$WORK/bob/all.log"; exit 1
fi
say bob "/e2e status"
if wait_for "$WORK/bob/all.log" "(the native e2e module is inactive while the rpe2e.pl script is loaded)" 10 &&
   grep -qE "\[E2E\] identity=[0-9a-f]{32} " "$WORK/bob/all.log"; then
    pass "bob: rpe2e.pl answers /e2e, the native module stays inactive"
else
    bad "bob: rpe2e.pl and the native module are not as expected"
fi
# the warning comes while the modules load, before the log is open: it is
# on bob's screen, in the status window
say bob "/window 1"
if grep -qF "the rpe2e.pl script is loaded: the native e2e module leaves encryption to it" "$WORK/bob/all.log" ||
   tmux -L "$SOCK" capture-pane -p -J -t bob -S -1000 | grep -qF "the rpe2e.pl script is loaded: the native e2e module"; then
    pass "bob: the native module warned about rpe2e.pl"
else
    bad "bob: no warning about rpe2e.pl from the native module"
fi
say alice "/e2e status"
if wait_for "$WORK/alice/all.log" "[E2E] identity=" 10 &&
   ! grep -qF "native e2e module is inactive" "$WORK/alice/all.log"; then
    pass "alice: the native module is active"
else
    bad "alice: the native module is not active"
fi

POLISH="zażółć gęślą jaźń"
LONG_A=$(python3 -c 'print(" ".join("zażółć%d" % i for i in range(1, 41)))')
LONG_B=$(python3 -c 'print(" ".join("gęślą%d" % i for i in range(1, 41)))')

# ---- channel ----
say alice "/join -E2E $CHAN"
say bob "/window goto $CHAN"
say alice "/e2e on"
say bob "/e2e on"
say alice "/e2e handshake bob"
sleep 2
if wait_for "$WORK/bob/logs/E2E/$CHAN.log" "Pending key exchange from alice" 10; then
    pass "bob (rpe2e.pl) got the native KEYREQ"
else
    bad "bob did not get alice's KEYREQ"
fi
say bob "/e2e accept alice"
sleep 2
# bob's key arrived with his answer, but his own request for alice's key
# waits for her decision as well
if wait_for "$WORK/alice/logs/E2E/$CHAN.log" "Pending key exchange from bob" 10; then
    pass "alice (native) got bob's reciprocal KEYREQ and asks before answering"
else
    bad "alice did not ask about bob's reciprocal KEYREQ"
fi
say alice "/e2e accept bob"
sleep 2

say alice "secret message from alice"
say bob "reply from bob"
say alice "$POLISH"
say bob "$POLISH from bob"
say alice "$LONG_A"
say bob "$LONG_B"
say alice "/me waves"
say bob "/me nods"
say alice "...an ellipsis line"
say alice "/quote privmsg $CHAN :quoted in lower case"
say alice "/msg $CHAN sent with msg"
# a % in printed text is shown as it is
say alice "/e2e verify 100%done"
sleep 3

who() {  # who <log under $WORK>: the client it belongs to
    local rel=${1#"$WORK"/}
    echo "${rel%%/*}"
}
check() {  # check <receiver log> <text>
    if wait_for "$1" "$2" 10; then pass "$(who "$1") sees: $2"
    else bad "$(who "$1") does not see: $2"; fi
}
check "$WORK/bob/logs/E2E/$CHAN.log" "secret message from alice"
check "$WORK/alice/logs/E2E/$CHAN.log" "reply from bob"
check "$WORK/bob/logs/E2E/$CHAN.log" "$POLISH"
check "$WORK/alice/logs/E2E/$CHAN.log" "$POLISH from bob"
check "$WORK/bob/logs/E2E/$CHAN.log" "alice waves"
check "$WORK/alice/logs/E2E/$CHAN.log" "bob nods"
check "$WORK/bob/logs/E2E/$CHAN.log" "an ellipsis line"
check "$WORK/bob/logs/E2E/$CHAN.log" "quoted in lower case"
check "$WORK/bob/logs/E2E/$CHAN.log" "sent with msg"
check "$WORK/alice/logs/E2E/$CHAN.log" "cannot resolve handle for 100%done"

# ---- query ----
say alice "/query bob"
say bob "/query alice"
say alice "/e2e on"
say bob "/e2e on"
say alice "/e2e handshake bob"
sleep 2
if wait_for "$WORK/bob/logs/E2E/alice.log" "Pending key exchange from alice" 10; then
    pass "bob (rpe2e.pl) got the native DM KEYREQ"
else
    bad "bob did not get alice's DM KEYREQ"
fi
say bob "/e2e accept alice"
sleep 2
if wait_for "$WORK/alice/logs/E2E/bob.log" "Pending key exchange from bob" 10; then
    pass "alice (native) got bob's reciprocal DM KEYREQ"
else
    bad "alice did not get bob's reciprocal DM KEYREQ"
fi
say alice "/e2e accept bob"
sleep 2
say alice "private from alice $POLISH"
say bob "private from bob $POLISH"
say alice "$LONG_B"
say bob "$LONG_A"
sleep 3
check "$WORK/bob/logs/E2E/alice.log" "private from alice $POLISH"
check "$WORK/alice/logs/E2E/bob.log" "private from bob $POLISH"

# the long messages: several encrypted chunks, each shown as a line; put
# together they are the message
long_check() {  # long_check <receiver log> <sender nick> <text> <what>
    # irssi sends the second line of a long message after its flood delay
    local n=0
    until long_check_once "$1" "$2" "$3"; do
        n=$((n + 1)); [ $n -ge 20 ] && { bad "$4"; return; }
        sleep 1
    done
    pass "$4"
}
long_check_once() {
    python3 - "$1" "$2" "$3" <<'EOF'
import re, sys
log, nick, text = sys.argv[1], sys.argv[2], sys.argv[3]
bodies = []
with open(log, encoding="utf-8", errors="strict") as fh:
    for line in fh:
        # "<alice> text" (default theme) or "alice❯ text" (erssi's)
        m = re.match(r"^\S+\s+(?:<[ @+%~&]?" + re.escape(nick) + r">|[@+%~&]?" +
                     re.escape(nick) + "\u276f) (.*)$", line.rstrip("\n"))
        if m:
            bodies.append(m.group(1))
joined = "".join(bodies).replace(" ", "")
sys.exit(0 if text.replace(" ", "") in joined else 1)
EOF
}
long_check "$WORK/bob/logs/E2E/$CHAN.log" alice "$LONG_A" "channel: bob reads alice's long message (chunked)"
long_check "$WORK/alice/logs/E2E/$CHAN.log" bob "$LONG_B" "channel: alice reads bob's long message (chunked)"
long_check "$WORK/bob/logs/E2E/alice.log" alice "$LONG_B" "query: bob reads alice's long message (chunked)"
long_check "$WORK/alice/logs/E2E/bob.log" bob "$LONG_A" "query: alice reads bob's long message (chunked)"

# ---- what crossed the server ----
python3 - "$WORK/wire.log" "$CHAN" <<'EOF' && pass "server: every message is ciphertext, chunked ones too" || bad "server: plain text or missing chunks"
import sys
log, chan = sys.argv[1], sys.argv[2]
priv = [l.rstrip("\n") for l in open(log, encoding="utf-8") if " PRIVMSG " in l]
targets = {"alice": chan, "bob": chan}
total = cipher = 0
multi = {}
for l in priv:
    sender, _, target, text = l.split(" ", 3)
    if target not in (chan, "alice", "bob"):
        continue
    total += 1
    if text.startswith(":+RPE2E01 "):
        cipher += 1
        msgid, part = text.split()[1], text.split()[3]
        if not part.startswith("1/1"):
            multi.setdefault((sender, target), set()).add(msgid)
print(f"  ({cipher}/{total} PRIVMSGs encrypted; chunked messages: {sum(len(v) for v in multi.values())})")
ok = total >= 20 and cipher == total
# a long message from each side, on the channel and in the query
ok = ok and all(len(multi.get(k, ())) >= 1 for k in (("alice", chan), ("bob", chan), ("alice", "bob"), ("bob", "alice")))
sys.exit(0 if ok else 1)
EOF
leak=0
for word in "secret message" "reply from bob" "waves" "nods" "ellipsis" "lower case" "sent with msg" \
            "private from" "zaż" "gęś" "jaźń" "zażółć1" "gęślą1"; do
    if grep -qiF -- "$word" "$WORK/wire.log"; then bad "plain text crossed the server: $word"; leak=1; fi
done
[ $leak = 0 ] && pass "no plain text crossed the server"
if grep -q "RPEE2E KEYREQ" "$WORK/wire.log" && grep -q "RPEE2E KEYRSP" "$WORK/wire.log"; then
    pass "key exchange via CTCP KEYREQ/KEYRSP"
else
    bad "no KEYREQ/KEYRSP on the server"
fi

# ---- the keyrings: the same format on both sides ----
# alice's (native) entries are kept per network ("E2E\x1f..."), bob's
# (rpe2e.pl) are not; the wire between them names the bare contexts
python3 - "$WORK/alice/rpe2e/keyring.json" "$WORK/bob/rpe2e/keyring.json" <<'EOF' && pass "keyrings: native and rpe2e.pl files hold each other's identity" || bad "keyrings do not match"
import json, sys, stat, os
alice, bob = (json.load(open(p, encoding="utf-8")) for p in sys.argv[1:3])
for p in sys.argv[1:3]:
    assert stat.S_IMODE(os.stat(p).st_mode) == 0o600, p
fa, fb = alice["identity"]["fp"], bob["identity"]["fp"]
assert fb in alice["peers"] and alice["peers"][fb]["status"] == "trusted", "alice lacks bob"
assert fa in bob["peers"] and bob["peers"][fa]["status"] == "trusted", "bob lacks alice"
assert alice["incoming"]["bob@127.0.0.1|E2E\x1f#test"]["status"] == "trusted"
assert bob["incoming"]["alice@127.0.0.1|#test"]["status"] == "trusted"
assert alice["incoming"]["bob@127.0.0.1|E2E\x1f@alice@127.0.0.1"]["status"] == "trusted"
assert bob["incoming"]["alice@127.0.0.1|@bob@127.0.0.1"]["status"] == "trusted"
assert alice["channels"]["E2E\x1f#test"]["enabled"] and alice["channels"]["E2E\x1f@bob@127.0.0.1"]["enabled"]
# nothing unscoped from the module, nothing scoped from rpe2e.pl
for store in ("channels", "outgoing", "incoming", "accepted", "pending", "outgoing_recipients", "pending_inbound"):
    for k in alice.get(store, {}):
        assert "\x1f" in k, (store, k)
assert not any("\x1f" in k for k in bob["channels"]), "rpe2e.pl wrote a scoped entry"
EOF
if grep -aq $'\x1f' "$WORK/wire.log"; then
    bad "a scoped context crossed the server"
else
    pass "server: only bare contexts on the wire"
fi

# ---- mallory: an outsider gets no key without /e2e accept (review C1) ----
say alice "/join -E2E $CHAN"
say mallory "/window goto $CHAN"
say mallory "/e2e mode auto"
say mallory "hello from mallory"
# handshake NOTICEs go through the flood queue: mallory answers bob too
if wait_for "$WORK/alice/logs/E2E/$CHAN.log" "Pending key exchange from mallory" 30; then
    pass "alice: mallory's request for her key waits for /e2e accept"
else
    bad "alice: no pending request from mallory"
fi
if grep -aq "^alice NOTICE mallory :.RPEE2E KEYRSP" "$WORK/wire.log"; then
    bad "alice sent her key (KEYRSP) to mallory without /e2e accept"
else
    pass "alice sent mallory no KEYRSP"
fi
say alice "members only after mallory came"
sleep 3
check "$WORK/bob/logs/E2E/$CHAN.log" "members only after mallory came"
if grep -qF "members only after mallory came" "$WORK/mallory/logs/E2E/$CHAN.log" 2>/dev/null; then
    bad "mallory reads alice's channel messages"
else
    pass "mallory cannot read alice's channel messages"
fi

# ---- a script that binds /e2e is not rpe2e.pl (review M3) ----
cat > "$WORK/alice/scripts/e2ealias.pl" <<'PERL'
use strict;
use Irssi;
our %IRSSI = (name => 'e2ealias');
Irssi::command_bind('e2e', sub { });
PERL
say alice "/script load e2ealias"
say alice "after another script bound e2e"
sleep 2
check "$WORK/bob/logs/E2E/$CHAN.log" "after another script bound e2e"
say alice "/script unload e2ealias"

# ---- a PRIVMSG from a rawlog hook while ciphertext goes out (review M4) ----
cat > "$WORK/alice/scripts/e2erawlog.pl" <<'PERL'
use strict;
use Irssi;
our %IRSSI = (name => 'e2erawlog');
my $done = 0;
Irssi::signal_add('rawlog', sub {
    my ($rawlog, $data) = @_;
    return if $done || $data !~ /^<< PRIVMSG #test :\+RPE2E01 /;
    $done = 1;
    my $server = Irssi::server_find_tag('E2E');
    $server->send_raw_now('PRIVMSG #test :sent from a rawlog hook');
});
PERL
say alice "/script load e2erawlog"
say alice "this line triggers the hook"
sleep 2
check "$WORK/bob/logs/E2E/$CHAN.log" "sent from a rawlog hook"
say alice "/script unload e2erawlog"

# ---- a raw line with CR LF in it (review I2) ----
say alice "/script exec Irssi::active_server->send_raw('PRIVMSG #pub :hello'.chr(13).chr(10).'PRIVMSG $CHAN :raw line with CRLF')"
sleep 1
say alice "/script exec Irssi::active_server->send_raw('PRIVMSG #pub :hello'.chr(13).'PRIVMSG $CHAN :raw line with CR')"
sleep 2

python3 - "$WORK/wire.log" <<'PY' && pass "server: the review checks sent no plain text" || bad "server: plain text from the review checks"
import sys
bad = 0
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    for word in ("hello from mallory", "members only", "another script bound", "triggers the hook",
                 "rawlog hook", "raw line with"):
        if word in l:
            print("  plain text:", l.rstrip())
            bad += 1
sys.exit(1 if bad else 0)
PY

# ---- E2E on in a query, the nick's address no longer known (review 2, I3) ----
say alice "/query carol"
say alice "/e2e on"
say alice "/window close"
say carol "/part $CHAN"
sleep 1
say alice "/join -E2E $CHAN"
say alice "/msg carol unresolved dm secret"
sleep 2
if grep -aqF "unresolved dm secret" "$WORK/wire.log"; then
    bad "a DM to carol (E2E on, address unknown) went out in plain text"
else
    pass "a DM to carol (E2E on, address unknown) is refused"
fi
# turned off in carol's query: clear text is the user's choice now
say alice "/query carol"
say alice "/e2e off"
say alice "/msg carol clear after off"
sleep 2
if grep -aqF "alice PRIVMSG carol :clear after off" "$WORK/wire.log"; then
    pass "/e2e off in the query turns E2E off for carol's last address"
else
    bad "/e2e off in carol's query did not turn E2E off"
fi
say alice "/window close"

# ---- a second network (E2E2): its own keys, its own settings ----
say erin "/window goto $CHAN"
n=0
until grep -qF "erin" "$WORK/alice/logs/E2E2/$CHAN.log" 2>/dev/null ||
      grep -qF "alice" "$WORK/erin/logs/E2E2/$CHAN.log" 2>/dev/null; do
    n=$((n + 1)); [ $n -gt 20 ] && break
    sleep 1
done
# E2E is on for #test on E2E only: the other network's #test is another
# channel and gets clear text
say alice "/join -E2E2 $CHAN"
say alice "net2 plain hello"
sleep 1
if grep -aqF "alice PRIVMSG $CHAN :net2 plain hello" "$WORK/wire2.log"; then
    pass "E2E2: #test there is not encrypted (E2E is on for E2E's #test only)"
else
    bad "E2E2: the clear-text message is not on the second network"
fi
check "$WORK/erin/logs/E2E2/$CHAN.log" "net2 plain hello"
if grep -aqF "net2 plain hello" "$WORK/wire.log"; then
    bad "E2E2's message crossed the first network"
else
    pass "E2E2's message stayed on its network"
fi
# turned on there: a key of its own
say alice "/e2e on"
say alice "net2 secret hello"
sleep 1
python3 - "$WORK/alice/rpe2e/keyring.json" <<'EOF' && pass "alice: E2E and E2E2 #test have their own keys" || bad "alice: the networks share a key or a config"
import json, sys
k = json.load(open(sys.argv[1], encoding="utf-8"))
a, b = k["outgoing"]["E2E\x1f#test"], k["outgoing"]["E2E2\x1f#test"]
assert a["sk"] != b["sk"], "same key"
assert k["channels"]["E2E2\x1f#test"]["enabled"] and k["channels"]["E2E\x1f#test"]["enabled"]
assert "#test" not in k["channels"] and "#test" not in k["outgoing"]
EOF
if grep -aqF "net2 secret hello" "$WORK/wire2.log" "$WORK/wire.log" ||
   ! grep -aq "^alice PRIVMSG $CHAN :+RPE2E01 " "$WORK/wire2.log"; then
    bad "E2E2: the message after /e2e on is not ciphertext"
else
    pass "E2E2: encrypted after /e2e on there"
fi
if grep -aqF "net2 secret hello" "$WORK/erin/logs/E2E2/$CHAN.log" 2>/dev/null; then
    bad "erin reads alice without a key"
else
    pass "erin (no key) cannot read alice on E2E2"
fi
say alice "/join -E2E $CHAN"
say alice "back on the first network"
check "$WORK/bob/logs/E2E/$CHAN.log" "back on the first network"

# ---- CASEMAPPING: names compared as each server compares them ----
# E2E2 (utf8): #Żaba is #żaba to the server, never clear text there
say alice "/join -E2E2 #żaba"
say alice "/e2e on"
say alice "/msg -E2E2 #Żaba unicode case secret"
say alice "/msg -E2E2 #żaba unicode exact secret"
sleep 1
if grep -aqF "unicode case secret" "$WORK/wire2.log" "$WORK/wire.log"; then
    bad "utf8 CASEMAPPING: #Żaba (E2E on for #żaba) went out in plain text"
else
    pass "utf8 CASEMAPPING: #Żaba (E2E on for #żaba) is not sent in plain text"
fi
if wait_for "$WORK/alice/all.log" "the same channel as #Żaba for the server" 5; then
    pass "alice is told #Żaba is the same channel as #żaba"
else
    bad "alice is not told why #Żaba was not sent"
fi
if grep -aq "^alice PRIVMSG #żaba :+RPE2E01 " "$WORK/wire2.log" &&
   ! grep -aqF "unicode exact secret" "$WORK/wire2.log"; then
    pass "utf8 CASEMAPPING: #żaba itself is encrypted"
else
    bad "utf8 CASEMAPPING: #żaba itself is not encrypted"
fi
# R-E11: a name that is not UTF-8 cannot be folded as a utf8 server does
say alice "/script exec Irssi::server_find_tag('E2E2')->command('msg #a'.chr(255).' not utf8 secret')"
sleep 1
if grep -aqF "not utf8 secret" "$WORK/wire2.log" "$WORK/wire.log"; then
    bad "utf8 CASEMAPPING: a channel name that is not UTF-8 went out in plain text"
else
    pass "utf8 CASEMAPPING: a channel name that is not UTF-8 is not sent in plain text"
fi
if wait_for "$WORK/alice/all.log" "is not valid UTF-8: " 5; then
    pass "alice is told why the name that is not UTF-8 was not sent"
else
    bad "alice is not told why the name that is not UTF-8 was not sent"
fi
# E2E (ascii): #a{b is another channel than #a[b, #A[B is the same
say alice "/join -E2E #a[b"
say alice "/e2e on"
say alice "/msg -E2E #a{b bracket plain ok"
say alice "/msg -E2E #A[B bracket upper secret"
sleep 1
if grep -aqF "alice PRIVMSG #a{b :bracket plain ok" "$WORK/wire.log"; then
    pass "ascii CASEMAPPING: #a{b (another channel than #a[b) gets clear text"
else
    bad "ascii CASEMAPPING: #a{b was refused, though the server keeps it apart from #a[b"
fi
if grep -aqF "bracket upper secret" "$WORK/wire.log" "$WORK/wire2.log"; then
    bad "ascii CASEMAPPING: #A[B (E2E on for #a[b) went out in plain text"
else
    pass "ascii CASEMAPPING: #A[B (E2E on for #a[b) is not sent in plain text"
fi
say alice "/join -E2E $CHAN"

# ---- an old keyring on two networks: refused until /e2e adopt ----
# alice's E2E entries become old ones of no network (as rpe2e.pl or erssi
# before the scope wrote them); E2E2's stay. alice uses two networks, so
# erssi cannot know where they belong: never clear text, never another
# network's rows, until /e2e adopt in the window
python3 - "$WORK/alice/rpe2e/keyring.json" "$WORK/alice-scoped.json" <<'PY' || bad "the legacy rewrite of alice's keyring failed"
import json, re, sys, shutil
path, keep = sys.argv[1], sys.argv[2]
shutil.copyfile(path, keep)
k = json.load(open(path, encoding="utf-8"))
bare = lambda t: re.sub(r"(^|\|)E2E\x1f", r"\1", t)
def walk(v):
    if isinstance(v, dict):
        return {bare(key): (bare(x) if key == "channel" and isinstance(x, str) else walk(x)) for key, x in v.items()}
    if isinstance(v, list):
        return [walk(x) for x in v]
    return v
k = walk(k)
assert "#test" in k["channels"] and "E2E2\x1f#test" in k["channels"]
with open(path, "w", encoding="utf-8") as fh:
    json.dump(k, fh)
PY
say alice "/join -E2E $CHAN"
say alice "after legacy rewrite"
sleep 1
if grep -aqF "after legacy rewrite" "$WORK/wire.log" "$WORK/wire2.log"; then
    bad "an old entry on two networks went out in plain text"
else
    pass "an old entry on two networks is not sent in plain text"
fi
if wait_for "$WORK/alice/logs/E2E/$CHAN.log" "/e2e adopt in this window binds it to E2E" 5; then
    pass "alice is told about /e2e adopt"
else
    bad "alice is not told about /e2e adopt"
fi
say alice "/e2e adopt"
if wait_for "$WORK/alice/logs/E2E/$CHAN.log" "adopted $CHAN for E2E" 5; then
    pass "/e2e adopt binds the old #test entries to E2E"
else
    bad "/e2e adopt did not adopt #test"
fi
say alice "after adopt"
check "$WORK/bob/logs/E2E/$CHAN.log" "after adopt"
# in the query: bob's config and his sessions to alice ("@<own>")
say alice "/query bob"
say alice "/e2e adopt"
if wait_for "$WORK/alice/logs/E2E/bob.log" "adopted @bob@127.0.0.1 for E2E" 5; then
    pass "/e2e adopt in the query binds bob's old entries to E2E"
else
    bad "/e2e adopt did not adopt the query"
fi
say alice "dm after adopt"
say bob "/query alice"
say bob "dm back after adopt"
check "$WORK/bob/logs/E2E/alice.log" "dm after adopt"
check "$WORK/alice/logs/E2E/bob.log" "dm back after adopt"
say alice "/window close"
python3 - "$WORK/alice-scoped.json" "$WORK/alice/rpe2e/keyring.json" "$WORK/wire.log" "$WORK/wire2.log" <<'PY' && pass "adopted: E2E's keys as before, E2E2's entries untouched, nothing in clear" || bad "the adoption changed keys or another network's entries"
import json, sys
old, new = (json.load(open(p, encoding="utf-8")) for p in sys.argv[1:3])
s, s2 = "E2E\x1f#test", "E2E2\x1f#test"
assert new["outgoing"][s]["sk"] == old["outgoing"][s]["sk"]
assert new["channels"][s]["adopted"] == "E2E"
assert new["incoming"]["bob@127.0.0.1|" + s]["sk"] == old["incoming"]["bob@127.0.0.1|" + s]["sk"]
assert new["incoming"]["bob@127.0.0.1|E2E\x1f@alice@127.0.0.1"]["sk"] == old["incoming"]["bob@127.0.0.1|E2E\x1f@alice@127.0.0.1"]["sk"]
for store in ("channels", "outgoing"):
    assert new[store][s2] == old[store][s2], store
assert "#test" not in new["channels"] and "#test" not in new["outgoing"]
for w in sys.argv[3:5]:
    for l in open(w, encoding="utf-8", errors="replace"):
        assert "after adopt" not in l and "legacy rewrite" not in l, l
PY
# what the user reads: "<network>:<name>", never the raw separator
say alice "/join -E2E $CHAN"
say alice "/e2e status"
say alice "/e2e list -all"
say alice "/e2e autotrust add $CHAN bob@127.0.0.1"
say alice "/e2e autotrust list"
say alice "/e2e autotrust remove bob@127.0.0.1"
if wait_for "$WORK/alice/all.log" "autotrust add E2E:$CHAN bob@127.0.0.1" 5 &&
   grep -qF "encrypted: " "$WORK/alice/all.log" &&
   grep -F "encrypted: " "$WORK/alice/all.log" | grep -qF "E2E2:$CHAN" &&
   grep -qE "bob@127.0.0.1  E2E:$CHAN  \[trusted\]" "$WORK/alice/all.log" &&
   ! grep -aqE "E2E2?$CHAN" "$WORK/alice/all.log" && ! grep -aq $'\x1f' "$WORK/alice/all.log"; then
    pass "status, list -all and autotrust show <network>:<name>"
else
    bad "status, list -all or autotrust do not show <network>:<name>"
    grep -aE "encrypted:|autotrust|E2E:#|E2E#" "$WORK/alice/all.log" | tail -n 8 | sed 's/^/    /'
fi
# control bytes stored in the keyring (a handle, a nick) are shown as '?',
# never as formatting codes
python3 - "$WORK/alice/rpe2e/keyring.json" <<'PY' || bad "the keyring edit for the display check failed"
import json, sys
p = sys.argv[1]
k = json.load(open(p, encoding="utf-8"))
k["peers"]["e" * 32] = {"pk": "", "last_handle": "evil\x03" "4@h", "last_nick": "evil\x02nick",
                        "status": "pending", "first_seen": 1, "last_seen": 1}
k["incoming"]["evil\x03" "4@h|E2E\x1f#test"] = {"fp": "e" * 32, "sk": "", "status": "pending", "created_at": 1}
json.dump(k, open(p, "w", encoding="utf-8"))
PY
say alice "/e2e list -all"
say alice "/e2e list"
python3 - "$WORK/alice/rpe2e/keyring.json" <<'PY' || bad "the keyring edit for the display check failed"
import json, sys
p = sys.argv[1]
k = json.load(open(p, encoding="utf-8"))
del k["peers"]["e" * 32]
del k["incoming"]["evil\x03" "4@h|E2E\x1f#test"]
json.dump(k, open(p, "w", encoding="utf-8"))
PY
if grep -qF "evil?4@h  [pending]  nick=evil?nick" "$WORK/alice/all.log" &&
   grep -qF "evil?4@h  E2E:$CHAN  [pending]" "$WORK/alice/all.log" &&
   grep -qF "evil?4@h on E2E:$CHAN" "$WORK/alice/all.log"; then
    pass "list and list -all show control bytes of the keyring as '?'"
else
    bad "list or list -all print control bytes of the keyring"
    grep -aF "evil" "$WORK/alice/all.log" | sed 's/^/    /'
fi

# ---- /upgrade with PRIVMSGs in the flood queue (review I1) ----
say alice "/join -E2E $CHAN"
say alice "/set cmds_max_at_once 1"
say alice "/set cmd_queue_speed 8s"
quick alice "upgrade queued one"
quick alice "upgrade queued two"
quick alice "upgrade queued three"
quick alice "/upgrade"
sleep 6
if grep -aqiF "upgrade queued" "$WORK/wire.log"; then
    bad "/upgrade sent queued messages in plain text"
    grep -aF "upgrade queued" "$WORK/wire.log" | sed 's/^/    /'
else
    pass "/upgrade sent no queued message in plain text"
fi
check "$WORK/bob/logs/E2E/$CHAN.log" "upgrade queued three"

# ---- rpe2e.pl keeps what only the module writes ----
# bob's rpe2e.pl loads and saves alice's keyring: "accepted" and
# "seen_rekeys" are the module's own members, rpe2e.pl keeps them
# until now bob's keyring is rpe2e.pl's own (no per-network entries):
# the native module says nothing about it - not in the log, nor on the
# screen of the status window (where it would land while erssi starts)
say bob "/window 1"
if grep -qF "does not know the per-network entries" "$WORK/bob/all.log" ||
   tmux -L "$SOCK" capture-pane -p -J -t bob -S -1000 | grep -qF "does not know the per-network entries"; then
    bad "bob: a downgrade warning on rpe2e.pl's own keyring"
else
    pass "bob: no downgrade warning on rpe2e.pl's own keyring"
fi
cp "$WORK/bob/rpe2e/keyring.json" "$WORK/bob-keyring.json"
cp "$WORK/alice/rpe2e/keyring.json" "$WORK/bob/rpe2e/keyring.json"
say bob "/window goto $CHAN"
# R-E19: rpe2e.pl on a keyring whose #test is set up only in the module's
# per-network entries ("E2E\x1f#test"): the script would send in clear
# text, the inactive module stops it; a bot command still goes out
WIRE_MARK=$(wc -l < "$WORK/wire.log")
say bob "scoped only from bob"
say bob "!seen scopedbot"
# a wire typed (or pasted) by the user is text, not the script's chunk
TYPED_WIRE=$(grep -a "^alice PRIVMSG $CHAN :+RPE2E01 " "$WORK/wire.log" | tail -n 1 | cut -d' ' -f4- | cut -c2-)
say bob "$TYPED_WIRE"
sleep 1
if [ -n "$TYPED_WIRE" ] && ! grep -aqF "bob PRIVMSG $CHAN :$TYPED_WIRE" "$WORK/wire.log"; then
    pass "rpe2e.pl loaded: a wire typed by the user to that channel is not sent"
else
    bad "rpe2e.pl loaded: a wire typed by the user went out (or none to type)"
fi
if grep -aqF "scoped only from bob" "$WORK/wire.log"; then
    bad "rpe2e.pl loaded: a channel only the module encrypts went out in plain text"
else
    pass "rpe2e.pl loaded: a channel only the module encrypts is not sent in plain text"
fi
if wait_for "$WORK/bob/logs/E2E/$CHAN.log" "E2E is on for E2E:$CHAN in the e2e module, which rpe2e.pl does not know" 5; then
    pass "bob is told why it was not delivered"
else
    bad "bob is not told why his message was not delivered"
fi
if grep -aqF "bob PRIVMSG $CHAN :!seen scopedbot" "$WORK/wire.log" &&
   grep -qF "bot-style message to $CHAN sent in CLEARTEXT" "$WORK/bob/logs/E2E/$CHAN.log"; then
    pass "rpe2e.pl loaded: a bot command on that channel still goes out, with the warning"
else
    bad "rpe2e.pl loaded: the bot command was stopped"
fi
say bob "/e2e mode quiet"
# the script decides a raw buffer of several lines by its first target:
# a PRIVMSG behind it to a channel E2E is on for in the script's own
# (bare) entry must not leave in clear either
say bob "/join #multi"
say bob "/e2e on"
say bob "/window goto $CHAN"
say bob "/script exec Irssi::active_server->send_raw('PRIVMSG #pub :multi lead'.chr(13).chr(10).'PRIVMSG #multi :multi secret')"
sleep 1
if grep -aqF "multi secret" "$WORK/wire.log"; then
    bad "rpe2e.pl loaded: a second line of a raw buffer went out in plain text"
else
    pass "rpe2e.pl loaded: a second line of a raw buffer is not sent in plain text"
fi
sleep 1
python3 - "$WORK/alice/rpe2e/keyring.json" "$WORK/bob/rpe2e/keyring.json" "$CHAN" <<'PY' && pass "rpe2e.pl loads and saves the module's keyring, its own members kept" || bad "rpe2e.pl lost what the module stored"
import json, sys
a, b = (json.load(open(p, encoding="utf-8")) for p in sys.argv[1:3])
assert b["channels"][sys.argv[3]]["mode"] == "quiet", "rpe2e.pl did not save"
assert a.get("accepted"), "alice accepted nobody"
assert b.get("accepted") == a["accepted"], "accepted changed"
assert b.get("seen_rekeys") == a.get("seen_rekeys"), "seen_rekeys changed"
assert b["identity"] == a["identity"]
PY
# the downgrade: rpe2e.pl on a keyring with per-network entries cannot
# see them - the native module (inactive) warns
say bob "after copy $LONG_A"
if wait_for "$WORK/bob/all.log" "does not know the per-network entries" 5; then
    pass "bob: the native module warns that rpe2e.pl cannot see the per-network entries"
else
    bad "bob: no downgrade warning with rpe2e.pl on a scoped keyring"
fi
# #test is on in rpe2e.pl's own (bare) entry now: the script encrypts,
# and its chunks pass the module
sleep 2
if tail -n +"$((WIRE_MARK + 1))" "$WORK/wire.log" | python3 -c '
import sys
lines = [l for l in sys.stdin if l.startswith("bob PRIVMSG #test :")]
wires = [l for l in lines if l.startswith("bob PRIVMSG #test :+RPE2E01 ")]
clear = [l for l in lines if "after copy" in l or "zażółć" in l]
sys.exit(0 if len(wires) >= 2 and not clear else 1)'; then
    pass "rpe2e.pl loaded: its own encrypted chunks still go out"
else
    bad "rpe2e.pl loaded: the script's chunks were dropped or plain text went out"
fi

# ---- one network, an old keyring: nothing to do ----
# bob unloads rpe2e.pl, his own (unscoped) keyring comes back, and the
# native module takes it over with the first line he sends (incoming
# traffic never does): he reads alice and she reads him without a new
# key exchange
say bob "/script unload rpe2e"
sleep 1
cp "$WORK/bob-keyring.json" "$WORK/bob/rpe2e/keyring.json"
say bob "/window goto $CHAN"
say bob "the native module on an old keyring"
check "$WORK/alice/logs/E2E/$CHAN.log" "the native module on an old keyring"
say alice "/join -E2E $CHAN"
say alice "alice to the old keyring"
check "$WORK/bob/logs/E2E/$CHAN.log" "alice to the old keyring"
if grep -aqF "old keyring" "$WORK/wire.log"; then
    bad "plain text after the native module took bob's keyring over"
else
    pass "no plain text after the native module took bob's keyring over"
fi
python3 - "$WORK/bob-keyring.json" "$WORK/bob/rpe2e/keyring.json" "$CHAN" <<'PY' && pass "bob: his old #test entries are the network's now, the keys the same" || bad "bob: the old keyring was not taken over as it is"
import json, sys
old, new = (json.load(open(p, encoding="utf-8")) for p in sys.argv[1:3])
c = sys.argv[3]
s = "E2E\x1f" + c
assert new["outgoing"][s]["sk"] == old["outgoing"][c]["sk"], "our key changed"
assert new["incoming"]["alice@127.0.0.1|" + s]["sk"] == old["incoming"]["alice@127.0.0.1|" + c]["sk"]
assert new["channels"][s]["enabled"]
assert new["channels"][s]["adopted"] == "E2E" and new["outgoing"][s]["adopted"] == "E2E"
assert c not in new["channels"] and c not in new["outgoing"]
assert "alice@127.0.0.1|" + c not in new["incoming"]
assert new["identity"] == old["identity"]
PY

# ---- a script of rpe2e.pl's name dies inside its gate ----
# loaded first, then the module (/unload, /load): the module's gate runs
# ahead of the script's, at the same priority, while the module is
# inactive. The script dies there and irssi unloads it at once, so the
# module is active again when its last hook (the rpe2e.pl guard) runs -
# the line has met no gate yet and must not go out in clear text
cat > "$WORK/alice/scripts/rpe2e_die.pl" <<'PERL'
use strict;
use Irssi;
our %IRSSI = (name => 'rpe2e_die');
Irssi::command_bind('e2e', sub { });
Irssi::signal_add_first('server outgoing modify', sub {
    my ($server, $line) = @_;    # $line: a reference to the line
    die "rpe2e_die: dies in its gate\n" if $$line =~ /dies in the gate/;
});
PERL
# the flood queue is slow since the /upgrade check: back to the defaults
say alice "/set -default cmds_max_at_once"
say alice "/set -default cmd_queue_speed"
# the same with a script that hooks in right after the module's gate,
# and no reload: the module decided earlier lines while it was active,
# never this one
cat > "$WORK/alice/scripts/rpe2e_die2.pl" <<'PERL'
use strict;
use Irssi;
our %IRSSI = (name => 'rpe2e_die2');
Irssi::command_bind('e2e', sub { });
# -99: after the module's gate (-100), before every default handler
Irssi::signal_add_priority('server outgoing modify', sub {
    my ($server, $line) = @_;    # $line: a reference to the line
    die "rpe2e_die2: dies after the module's gate\n" if $$line =~ /dies right after/;
}, -99);
PERL
say alice "/join -E2E $CHAN"
say alice "an encrypted line before the script"
say alice "/script load rpe2e_die2"
say alice "a secret line the script dies right after the gate for"
if wait_for "$WORK/alice/all.log" "rpe2e_die2: dies after the module's gate" 15; then
    sleep 1
    pass "the script hooked in after the module's gate died there"
else
    bad "the script hooked in after the module's gate did not die (the check proves nothing)"
fi
if grep -aqF "dies right after the gate for" "$WORK/wire.log"; then
    bad "a line went out in plain text when the script died right after the module's gate"
else
    pass "no plain text when the script dies right after the module's gate"
fi
check "$WORK/bob/logs/E2E/$CHAN.log" "a secret line the script dies right after the gate for"
say alice "/script load rpe2e_die"
say alice "/unload e2e"
say alice "/load e2e"
say alice "/join -E2E $CHAN"
say alice "a secret line the script dies in the gate for"
if wait_for "$WORK/alice/all.log" "rpe2e_die: dies in its gate" 15; then
    sleep 1
    pass "the script of rpe2e.pl's name died in its gate"
else
    bad "the script of rpe2e.pl's name did not die in its gate (the check proves nothing)"
fi
if grep -aqF "dies in the gate for" "$WORK/wire.log"; then
    bad "a line went out in plain text when the script died in its gate"
else
    pass "no plain text when the script dies in its gate"
fi
check "$WORK/bob/logs/E2E/$CHAN.log" "a secret line the script dies in the gate for"

for f in "$WORK"/asan-* "$WORK"/ubsan-*; do
    [ -e "$f" ] || continue
    bad "sanitizer report: $(basename "$f")"
    sed -n '1,30p' "$f"
done

echo "result: $ok ok, $fail failed"
if [ "$fail" -ne 0 ]; then
    for nick in alice bob; do
        echo "--- $nick: last lines"
        tail -n 25 "$WORK/$nick/all.log" 2>/dev/null
    done
fi
[ "$fail" -eq 0 ]
