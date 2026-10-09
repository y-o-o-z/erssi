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
    if [ -n "$IRCD_PID" ]; then kill "$IRCD_PID" 2>/dev/null || true; fi
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
python3 "$IRCD" --port "$PORT" --wire "$WORK/wire.log" &
IRCD_PID=$!
i=0
while ! python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), 1)" 2>/dev/null; do
    i=$((i + 1)); [ $i -gt 50 ] && { echo "test server did not start"; exit 1; }
    sleep 0.1
done

for nick in alice bob mallory carol; do
    home="$WORK/$nick"
    mkdir -p "$home/modules" "$home/logs" "$home/scripts/autorun" "$WORK/home-$nick"
    for so in "$MODDIR"/*.so; do ln -s "$so" "$home/modules/"; done
    [ "$nick" = bob ] && ln -s "$SCRIPT" "$home/scripts/autorun/rpe2e.pl"
    cat > "$home/config" <<EOF
servers = ( { address = "127.0.0.1"; chatnet = "E2E"; port = "$PORT"; autoconnect = "yes"; } );
chatnets = { E2E = { type = "IRC"; }; };
channels = ( { name = "$CHAN"; chatnet = "E2E"; autojoin = "yes"; } );
logs = { "$home/all.log" = { auto_open = "yes"; level = "ALL"; }; };
settings = {
  core = { real_name = "e2e test"; user_name = "$nick"; nick = "$nick"; };
  "fe-common/core" = { autolog = "yes"; autolog_path = "$home/logs/\$0.log"; autolog_level = "ALL"; };
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
until grep -q "bob.*joined $CHAN" "$WORK/alice/logs/$CHAN.log" 2>/dev/null ||
      grep -q "alice.*joined $CHAN" "$WORK/bob/logs/$CHAN.log" 2>/dev/null; do
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
for nick in alice bob; do say "$nick" "/window goto $CHAN"; done
say alice "/e2e on"
say bob "/e2e on"
say alice "/e2e handshake bob"
sleep 2
if wait_for "$WORK/bob/logs/$CHAN.log" "Pending key exchange from alice" 10; then
    pass "bob (rpe2e.pl) got the native KEYREQ"
else
    bad "bob did not get alice's KEYREQ"
fi
say bob "/e2e accept alice"
sleep 2
# bob's key arrived with his answer, but his own request for alice's key
# waits for her decision as well
if wait_for "$WORK/alice/logs/$CHAN.log" "Pending key exchange from bob" 10; then
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

check() {  # check <receiver log> <text>
    if wait_for "$1" "$2" 10; then pass "$(basename "$(dirname "$(dirname "$1")")") sees: $2"
    else bad "$(basename "$(dirname "$(dirname "$1")")") does not see: $2"; fi
}
check "$WORK/bob/logs/$CHAN.log" "secret message from alice"
check "$WORK/alice/logs/$CHAN.log" "reply from bob"
check "$WORK/bob/logs/$CHAN.log" "$POLISH"
check "$WORK/alice/logs/$CHAN.log" "$POLISH from bob"
check "$WORK/bob/logs/$CHAN.log" "alice waves"
check "$WORK/alice/logs/$CHAN.log" "bob nods"
check "$WORK/bob/logs/$CHAN.log" "an ellipsis line"
check "$WORK/bob/logs/$CHAN.log" "quoted in lower case"
check "$WORK/bob/logs/$CHAN.log" "sent with msg"
check "$WORK/alice/logs/$CHAN.log" "cannot resolve handle for 100%done"

# ---- query ----
say alice "/query bob"
say bob "/query alice"
say alice "/e2e on"
say bob "/e2e on"
say alice "/e2e handshake bob"
sleep 2
if wait_for "$WORK/bob/logs/alice.log" "Pending key exchange from alice" 10; then
    pass "bob (rpe2e.pl) got the native DM KEYREQ"
else
    bad "bob did not get alice's DM KEYREQ"
fi
say bob "/e2e accept alice"
sleep 2
if wait_for "$WORK/alice/logs/bob.log" "Pending key exchange from bob" 10; then
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
check "$WORK/bob/logs/alice.log" "private from alice $POLISH"
check "$WORK/alice/logs/bob.log" "private from bob $POLISH"

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
long_check "$WORK/bob/logs/$CHAN.log" alice "$LONG_A" "channel: bob reads alice's long message (chunked)"
long_check "$WORK/alice/logs/$CHAN.log" bob "$LONG_B" "channel: alice reads bob's long message (chunked)"
long_check "$WORK/bob/logs/alice.log" alice "$LONG_B" "query: bob reads alice's long message (chunked)"
long_check "$WORK/alice/logs/bob.log" bob "$LONG_A" "query: alice reads bob's long message (chunked)"

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
python3 - "$WORK/alice/rpe2e/keyring.json" "$WORK/bob/rpe2e/keyring.json" <<'EOF' && pass "keyrings: native and rpe2e.pl files hold each other's identity" || bad "keyrings do not match"
import json, sys, stat, os
alice, bob = (json.load(open(p, encoding="utf-8")) for p in sys.argv[1:3])
for p in sys.argv[1:3]:
    assert stat.S_IMODE(os.stat(p).st_mode) == 0o600, p
fa, fb = alice["identity"]["fp"], bob["identity"]["fp"]
assert fb in alice["peers"] and alice["peers"][fb]["status"] == "trusted", "alice lacks bob"
assert fa in bob["peers"] and bob["peers"][fa]["status"] == "trusted", "bob lacks alice"
assert alice["incoming"]["bob@127.0.0.1|#test"]["status"] == "trusted"
assert bob["incoming"]["alice@127.0.0.1|#test"]["status"] == "trusted"
assert alice["incoming"]["bob@127.0.0.1|@alice@127.0.0.1"]["status"] == "trusted"
assert bob["incoming"]["alice@127.0.0.1|@bob@127.0.0.1"]["status"] == "trusted"
EOF

# ---- mallory: an outsider gets no key without /e2e accept (review C1) ----
say alice "/window goto $CHAN"
say mallory "/window goto $CHAN"
say mallory "/e2e mode auto"
say mallory "hello from mallory"
# handshake NOTICEs go through the flood queue: mallory answers bob too
if wait_for "$WORK/alice/logs/$CHAN.log" "Pending key exchange from mallory" 30; then
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
check "$WORK/bob/logs/$CHAN.log" "members only after mallory came"
if grep -qF "members only after mallory came" "$WORK/mallory/logs/$CHAN.log" 2>/dev/null; then
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
check "$WORK/bob/logs/$CHAN.log" "after another script bound e2e"
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
    my ($server) = Irssi::servers();
    $server->send_raw_now('PRIVMSG #test :sent from a rawlog hook');
});
PERL
say alice "/script load e2erawlog"
say alice "this line triggers the hook"
sleep 2
check "$WORK/bob/logs/$CHAN.log" "sent from a rawlog hook"
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
say alice "/window goto $CHAN"
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

# ---- /upgrade with PRIVMSGs in the flood queue (review I1) ----
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
check "$WORK/bob/logs/$CHAN.log" "upgrade queued three"

# ---- rpe2e.pl keeps what only the module writes ----
# bob's rpe2e.pl loads and saves alice's keyring: "accepted" and
# "seen_rekeys" are the module's own members, rpe2e.pl keeps them
cp "$WORK/alice/rpe2e/keyring.json" "$WORK/bob/rpe2e/keyring.json"
say bob "/window goto $CHAN"
say bob "/e2e mode quiet"
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
