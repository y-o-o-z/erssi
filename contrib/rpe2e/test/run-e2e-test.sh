#!/bin/sh
# run-e2e-test.sh - test integracyjny rpe2e.pl na dwoch prawdziwych erssi
#
#   run-e2e-test.sh [erssi] [rpe2e.pl] [katalog NexusIRC]
#
# Lokalny serwer IRC (e2eircd.py, 127.0.0.1) i dwa erssi (alice, bob), kazde
# z wlasnym --home i wlasnym keyringiem, w osobnym serwerze tmux (-L), wiec
# nic nie dotyka dzialajacej sesji uzytkownika. Scenariusz jak przy rozmowie
# z repartee: /e2e on, wymiana kluczy (handshake + accept po obu stronach),
# wiadomosc zwykla, /me i /msg #kanal (ta droga wysyla web), a potem:
#   - u odbiorcy tekst jawny (autolog erssi),
#   - na serwerze kazdy PRIVMSG do kanalu to szyfrogram +RPE2E01.
# Z katalogiem NexusIRC (zbudowanym) alice wlacza E2E nie z terminala, tylko
# komenda wyslana przez fe-web tak jak robi to web (FeWebSocket Nexusa, pole
# "target" = kanal) - sprawdza droge: web -> fe-web -> rpe2e.pl.
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
# erssi bez serwera czekalby na ponowne polaczenie (server_reconnect_time)
i=0
while ! python3 -c "import socket; socket.create_connection(('127.0.0.1', $PORT), 1)" 2>/dev/null; do
    i=$((i + 1)); [ $i -gt 50 ] && { echo "serwer testowy nie wystartowal" >&2; exit 1; }
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

say() {  # say <nick> <tekst> - wpisanie linii w erssi
    tmux -L "$SOCK" send-keys -t "$1" -l -- "$2"
    tmux -L "$SOCK" send-keys -t "$1" Enter
    sleep 1
}

wait_for() {  # wait_for <plik> <wzorzec> <sekundy>
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
if wait_for "$WORK/alice/logs/$CHAN.log" "bob" 30 && wait_for "$WORK/bob/logs/$CHAN.log" "$CHAN" 30; then
    pass "alice i bob na $CHAN"
else
    bad "klienci nie weszli na kanal"; exit 1
fi
for nick in alice bob; do
    # rpe2e.pl przy starcie tworzy tozsamosc w <home>/rpe2e/keyring.json
    if wait_for "$WORK/$nick/rpe2e/keyring.json" '"identity"' 10; then
        pass "$nick: rpe2e.pl zaladowany (tozsamosc w keyringu)"
    else
        bad "$nick: rpe2e.pl sie nie zaladowal"
    fi
done

for nick in alice bob; do say "$nick" "/window goto $CHAN"; done
if [ -n "$NEXUS" ]; then
    # alice: /e2e on z "weba"; jej terminal stoi w tym czasie na innym oknie
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
        pass "alice: /e2e on z weba (fe-web, target $CHAN) wlaczylo kanal"
    else
        bad "alice: /e2e on z weba nie zadzialalo"
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

say alice "tajna wiadomosc od alice"
say bob "odpowiedz bob zazolc gesla jazn"
say alice "/me macha"
say alice "/msg $CHAN wyslane jak z weba"
sleep 3

check() {  # check <odbiorca> <tekst>
    if wait_for "$WORK/$1/logs/$CHAN.log" "$2" 10; then pass "$1 widzi: $2"; else bad "$1 nie widzi: $2"; fi
}
check bob "tajna wiadomosc od alice"
check alice "odpowiedz bob zazolc gesla jazn"
check bob "alice macha"
check bob "wyslane jak z weba"

total=$(grep -c " PRIVMSG $CHAN :" "$WORK/wire.log" || true)
cipher=$(grep -c " PRIVMSG $CHAN :+RPE2E01 " "$WORK/wire.log" || true)
if [ "$total" -ge 4 ] && [ "$total" -eq "$cipher" ]; then
    pass "serwer: $cipher/$total wiadomosci kanalu to szyfrogram"
else
    bad "serwer: tylko $cipher/$total wiadomosci kanalu zaszyfrowanych"
fi
if grep -q "tajna wiadomosc\|odpowiedz bob\|macha\|wyslane jak z weba" "$WORK/wire.log"; then
    bad "tekst jawny przeszedl przez serwer"
else
    pass "tekst jawny nie przeszedl przez serwer"
fi
if grep -q "RPEE2E KEYREQ" "$WORK/wire.log" && grep -q "RPEE2E KEYRSP" "$WORK/wire.log"; then
    pass "wymiana kluczy przez CTCP KEYREQ/KEYRSP"
else
    bad "brak KEYREQ/KEYRSP na serwerze"
fi

echo "wynik: $ok ok, $fail bledow"
[ "$fail" -eq 0 ]
