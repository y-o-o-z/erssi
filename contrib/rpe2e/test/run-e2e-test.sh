#!/bin/sh
# run-e2e-test.sh - integration test of rpe2e.pl on two real erssi clients
#
#   run-e2e-test.sh [erssi] [rpe2e.pl] [NexusIRC directory]
#
# A local IRC server (e2eircd.py, 127.0.0.1) and two erssi (alice, bob), each
# with its own --home and keyring, in a private tmux server (-L), so nothing
# touches the user's running session. Same scenario as a conversation with
# repartee: /e2e on, key exchange (handshake + accept on both sides), a plain
# message, /me and /msg #channel (the path the web client uses), then:
#   - the receiver sees plain text (erssi autolog),
#   - on the server every PRIVMSG to the channel is +RPE2E01 ciphertext.
# With a built NexusIRC directory alice turns E2E on not from the terminal but
# with a command sent through fe-web the way the web does it (Nexus's
# FeWebSocket, "target" field = channel) - this checks the path
# web -> fe-web -> rpe2e.pl.
set -eu

ERSSI="${1:-$HOME/.local/opt/erssi/bin/erssi}"
SCRIPT=$(realpath "${2:-$HOME/.erssi/scripts/rpe2e.pl}")
NEXUS="${3:-}"
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rpe2e-test.XXXXXX")
PORT=$((20000 + $$ % 10000))
WEBPORT=$((PORT + 1))
SOCK="rpe2e-test-$$"
CHAN="#e2etest"

ok=0
fail=0
pass() { printf '  ok   %s\n' "$*"; ok=$((ok + 1)); }
bad()  { printf '  FAIL %s\n' "$*"; fail=$((fail + 1)); }

cleanup() {
    tmux -L "$SOCK" kill-server 2>/dev/null || true
    if [ -n "${IRCD_PID:-}" ]; then kill "$IRCD_PID" 2>/dev/null || true; fi
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

python3 "$HERE/e2eircd.py" --port "$PORT" --wire "$WORK/wire.log" &
IRCD_PID=$!
# without the server erssi would wait for a reconnect (server_reconnect_time)
i=0
while ! python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), 1)" 2>/dev/null; do
    i=$((i + 1)); [ $i -gt 50 ] && { echo "test server did not start" >&2; exit 1; }
    sleep 0.1
done

for nick in alice bob; do
    home="$WORK/$nick"
    mkdir -p "$home/scripts/autorun" "$home/logs"
    ln -s "$SCRIPT" "$home/scripts/autorun/rpe2e.pl"
    web=""
    if [ "$nick" = alice ] && [ -n "$NEXUS" ]; then
        web="\"fe-web\" = { fe_web_enabled = \"yes\"; fe_web_port = \"$WEBPORT\"; fe_web_password = \"rpe2e-test\"; };"
    fi
    cat > "$home/config" <<EOF
servers = ( { address = "127.0.0.1"; chatnet = "E2E"; port = "$PORT"; autoconnect = "yes"; } );
chatnets = { E2E = { type = "IRC"; }; };
channels = ( { name = "$CHAN"; chatnet = "E2E"; autojoin = "yes"; } );
settings = {
  core = { real_name = "rpe2e test"; user_name = "$nick"; nick = "$nick"; };
  "fe-common/core" = { autolog = "yes"; autolog_path = "$home/logs/\$0.log"; autolog_level = "ALL"; };
  $web
};
EOF
    tmux -L "$SOCK" new-session -d -s "$nick" -x 200 -y 50 \
        "env -u LC_ALL LANG=en_US.UTF-8 $ERSSI --home=$home"
done

say() {  # say <nick> <text> - type a line into erssi
    tmux -L "$SOCK" send-keys -t "$1" -l -- "$2"
    tmux -L "$SOCK" send-keys -t "$1" Enter
    sleep 1
}

wait_for() {  # wait_for <file> <pattern> <seconds>
    i=0
    while [ $i -lt "$3" ]; do
        grep -q -- "$2" "$1" 2>/dev/null && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

echo "rpe2e.pl: $SCRIPT"
echo "erssi:    $ERSSI"
both_joined() {  # whoever joined second shows up as "joined" in the other's log
    i=0
    while [ $i -lt 30 ]; do
        grep -q -- "bob.*joined $CHAN" "$WORK/alice/logs/$CHAN.log" 2>/dev/null && return 0
        grep -q -- "alice.*joined $CHAN" "$WORK/bob/logs/$CHAN.log" 2>/dev/null && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}
if both_joined; then
    pass "alice and bob on $CHAN"
else
    bad "clients did not join the channel"; exit 1
fi
for nick in alice bob; do
    # on start rpe2e.pl creates an identity in <home>/rpe2e/keyring.json
    if wait_for "$WORK/$nick/rpe2e/keyring.json" '"identity"' 10; then
        pass "$nick: rpe2e.pl loaded (identity in keyring)"
    else
        bad "$nick: rpe2e.pl did not load"
    fi
done

for nick in alice bob; do say "$nick" "/window goto $CHAN"; done
if [ -n "$NEXUS" ]; then
    # alice: /e2e on from the "web"; her terminal shows another window meanwhile
    say alice "/window 1"
    cat > "$WORK/web-command.mjs" <<EOF
import {FeWebSocket} from "$NEXUS/dist/server/feWebClient/feWebSocket.js";
const ws = new FeWebSocket({host: "127.0.0.1", port: $WEBPORT, password: "rpe2e-test", encryption: true,
    useTLS: true, rejectUnauthorized: false, reconnect: false});
await ws.connect();
for (let i = 0; i < 50 && !ws.isConnected(); i++) await new Promise((r) => setTimeout(r, 100));
ws.executeCommand("/e2e on", "E2E", "$CHAN");
await new Promise((r) => setTimeout(r, 1500));
await ws.disconnect();
EOF
    if (cd "$NEXUS" && LOG_LEVEL=error node "$WORK/web-command.mjs") > "$WORK/web-command.log" 2>&1 &&
        wait_for "$WORK/alice/rpe2e/keyring.json" '"enabled":1' 10; then
        pass "alice: /e2e on from the web (fe-web, target $CHAN) enabled the channel"
    else
        bad "alice: /e2e on from the web did not work"
        sed 's/^/       /' "$WORK/web-command.log"
    fi
    say alice "/window goto $CHAN"
else
    say alice "/e2e on"
fi
say bob "/e2e on"
say alice "/e2e handshake bob"
sleep 2
say bob "/e2e accept alice"
sleep 2
say alice "/e2e accept bob"
sleep 2

say alice "secret message from alice"
say bob "reply from bob"
say alice "/me waves"
say alice "/msg $CHAN sent like from the web"
sleep 3

check() {  # check <receiver> <text>
    if wait_for "$WORK/$1/logs/$CHAN.log" "$2" 10; then pass "$1 sees: $2"; else bad "$1 does not see: $2"; fi
}
check bob "secret message from alice"
check alice "reply from bob"
check bob "alice waves"
check bob "sent like from the web"

total=$(grep -c " PRIVMSG $CHAN :" "$WORK/wire.log" || true)
cipher=$(grep -c " PRIVMSG $CHAN :+RPE2E01 " "$WORK/wire.log" || true)
if [ "$total" -ge 4 ] && [ "$total" -eq "$cipher" ]; then
    pass "server: $cipher/$total channel messages are ciphertext"
else
    bad "server: only $cipher/$total channel messages encrypted"
fi
if grep -q "secret message\|reply from bob\|waves\|sent like from the web" "$WORK/wire.log"; then
    bad "plain text crossed the server"
else
    pass "no plain text crossed the server"
fi
if grep -q "RPEE2E KEYREQ" "$WORK/wire.log" && grep -q "RPEE2E KEYRSP" "$WORK/wire.log"; then
    pass "key exchange via CTCP KEYREQ/KEYRSP"
else
    bad "no KEYREQ/KEYRSP on the server"
fi

echo "result: $ok ok, $fail failed"
[ "$fail" -eq 0 ]
