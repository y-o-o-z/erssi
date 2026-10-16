#!/usr/bin/env python3
"""A minimal IRC server for erssi live tests (tests/live, tests/e2e).

127.0.0.1 only. It handles what erssi and rpe2e.pl need: registration
(CAP/NICK/USER), PING, JOIN with NAMES, MODE, TOPIC, PART, nick changes, WHO,
WHOIS (311 - rpe2e.pl learns its own ident@host from it), PRIVMSG/NOTICE to a
channel and to a nick (CTCP too, which carries KEYREQ/KEYRSP). Every
PRIVMSG/NOTICE line is also written to the --wire file, so the test can check
what really crossed the server; --wire-times writes them once more with the
time they arrived ("<seconds.ms> <nick> <CMD> <target> :<text>"), so the
test can check the pace of the flood queue. --network names the network in 005 NETWORK=,
so two of these can play two networks. --casemapping ascii, rfc1459,
strict-rfc1459 or utf8 sends CASEMAPPING=<value> in 005 and folds channel
and nick names that way (utf8: NFKC, then Python's casefold); without it
no CASEMAPPING is sent and names are folded with str.lower().
--hide-casemapping folds as --casemapping says but names none in 005.
--register-delay N answers USER only after N seconds, so a client stays
connecting that long.
TESTRAW <nick> :<raw line> (sent by any client) writes <raw line> to that nick.
TESTDROP <nick> closes that nick's connection without a word, as a link that
dies does.
ISON is answered (303) with the nicks that are on.
--nick-rules: NICKLEN=9 in 005; a nick in use gets 433 (at registration too: 001 waits for
a nick that is free), QUIT is sent to the clients that share a channel, and
TESTDELAY <nick> <seconds> makes a free nick unavailable for that long
(437, as IRCnet's nick delay); TESTREFUSE <nick> makes NICK <nick> an
erroneous nickname (432); TESTNICK <nick> <new> renames a client from the
server side, as a collision or SVSNICK does.
--monitor: MONITOR=100 in 005; MONITOR + / - / C answered with 730/731,
and the watchers get 730/731 when a monitored nick comes or goes.
"""
import argparse
import asyncio
import time
import unicodedata

SRV = "e2e.test"
NETWORK = "E2ETest"
CASEMAPPING = None
HIDE_CASEMAPPING = False
REGISTER_DELAY = 0  # --register-delay: seconds before the reply to USER
NICK_RULES = False  # --nick-rules: 433, QUIT to channels, TESTDELAY, TESTNICK
MONITOR = False     # --monitor: MONITOR=100 and 730/731
delayed = {}   # nick (folded) -> time until it is free again (437)
erroneous = set()  # nicks (folded) that are erroneous (432)
watchers = {}  # nick (folded) -> set of Client with it in their MONITOR list
TRACE = None   # --trace file: every line in and out, with the time
clients = {}   # nick (folded) -> Client
channels = {}  # channel (folded) -> set(nick folded)

RFC1459_UPPER = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ[]\\~", "abcdefghijklmnopqrstuvwxyz{}|^")
STRICT_UPPER = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ[]\\", "abcdefghijklmnopqrstuvwxyz{}|")
ASCII_UPPER = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def fold(name):
    """a channel or nick name as this server compares it"""
    if CASEMAPPING == "utf8":
        return unicodedata.normalize("NFKC", name).casefold()
    if CASEMAPPING == "rfc1459":
        return name.translate(RFC1459_UPPER)
    if CASEMAPPING == "strict-rfc1459":
        return name.translate(STRICT_UPPER)
    if CASEMAPPING == "ascii":
        return name.translate(ASCII_UPPER)
    return name.lower()


class Client:
    def __init__(self, writer):
        self.writer = writer
        self.nick = "*"
        self.user = "u"
        self.waits_for_nick = False  # USER seen, 001 waits for a free nick
        self.monitor = set()

    @property
    def prefix(self):
        return f"{self.nick}!{self.user}@127.0.0.1"

    def send(self, line):
        trace("out", self.nick, line)
        self.writer.write((line + "\r\n").encode())


def trace(direction, nick, line):
    if TRACE:
        with open(TRACE, "a", encoding="utf-8") as fh:
            fh.write(f"{time.time():.3f} {direction} {nick} {line}\n")


def members(chan):
    return [clients[n] for n in channels.get(fold(chan), ()) if n in clients]


def neighbours(me):
    """the other clients that share a channel with me"""
    seen = set()
    for nicks in channels.values():
        if fold(me.nick) in nicks:
            seen.update(clients[n] for n in nicks if n in clients)
    seen.discard(me)
    return seen


def nick_online(client):
    if MONITOR:
        for w in watchers.get(fold(client.nick), ()):
            w.send(f":{SRV} 730 {w.nick} :{client.prefix}")


def nick_offline(nick):
    if MONITOR:
        for w in watchers.get(fold(nick), ()):
            w.send(f":{SRV} 731 {w.nick} :{nick}")


def nick_refused(me, new):
    """the numeric that refuses NEW for me, or None"""
    if not NICK_RULES:
        return None
    if fold(new) in erroneous:
        return f":{SRV} 432 {me.nick} {new} :Erroneous Nickname"
    holder = clients.get(fold(new))
    if holder is not None and holder is not me:
        return f":{SRV} 433 {me.nick} {new} :Nickname is already in use"
    if delayed.get(fold(new), 0) > time.time() and (holder is None or holder is not me):
        return f":{SRV} 437 {me.nick} {new} :Nick/channel is temporarily unavailable"
    return None


def rename(me, new):
    """me is new from now on: told to me and to the clients in its channels"""
    old = me.nick
    out = f":{me.prefix} NICK :{new}"
    for c in neighbours(me) | {me}:
        c.send(out)
    for nicks in channels.values():
        if fold(old) in nicks:
            nicks.discard(fold(old))
            nicks.add(fold(new))
    clients.pop(fold(old), None)
    me.nick = new
    clients[fold(new)] = me
    if fold(old) != fold(new):
        nick_offline(old)
        nick_online(me)


def welcome(me):
    for num, msg in (("001", f"Welcome {me.nick}"), ("002", f"Your host is {SRV}"), ("003", "today"),
                     ("004", f"{SRV} e2e io biklmnopstv")):
        me.send(f":{SRV} {num} {me.nick} :{msg}")
    cm = f" CASEMAPPING={CASEMAPPING}" if CASEMAPPING and not HIDE_CASEMAPPING else ""
    mon = " MONITOR=100" if MONITOR else ""
    nl = " NICKLEN=9" if NICK_RULES else ""
    me.send(f":{SRV} 005 {me.nick} CHANTYPES=# PREFIX=(ov)@+ NETWORK={NETWORK}{cm}{mon}{nl} :are supported")
    me.send(f":{SRV} 376 {me.nick} :End of MOTD")
    nick_online(me)


async def handle(reader, writer, wire_path, times_path):
    me = Client(writer)
    try:
        while line := await reader.readline():
            text = line.decode(errors="replace").rstrip("\r\n")
            if not text:
                continue
            trace("in", me.nick, text)
            parts = text.split(" ")
            cmd, args = parts[0].upper(), parts[1:]
            trailing = text.split(" :", 1)[1] if " :" in text else (args[-1] if args else "")
            if cmd == "CAP" and args and args[0] == "LS":
                me.send(f":{SRV} CAP * LS :")
            elif cmd == "NICK":
                new = args[0].lstrip(":")
                refused = nick_refused(me, new)
                if refused:
                    me.send(refused)
                elif me.nick != "*" and not me.waits_for_nick:
                    rename(me, new)
                else:
                    me.nick = new
                    clients[fold(me.nick)] = me
                    if me.waits_for_nick:
                        me.waits_for_nick = False
                        welcome(me)
            elif cmd == "USER":
                me.user = args[0]
                if REGISTER_DELAY:
                    await asyncio.sleep(REGISTER_DELAY)
                if NICK_RULES and me.nick == "*":
                    me.waits_for_nick = True  # its NICK was refused: 001 once one is free
                else:
                    welcome(me)
            elif cmd == "PING":
                me.send(f":{SRV} PONG {SRV} :{args[0].lstrip(':') if args else ''}")
            elif cmd == "JOIN":
                for chan in filter(None, args[0].lstrip(":").split(",")):
                    channels.setdefault(fold(chan), set()).add(fold(me.nick))
                    for other in members(chan):
                        other.send(f":{me.prefix} JOIN {chan}")
                    names = " ".join(c.nick for c in members(chan))
                    me.send(f":{SRV} 331 {me.nick} {chan} :No topic is set")
                    me.send(f":{SRV} 353 {me.nick} = {chan} :{names}")
                    me.send(f":{SRV} 366 {me.nick} {chan} :End of NAMES")
            elif cmd == "MODE" and args and args[0].startswith("#"):
                if len(args) == 1:
                    me.send(f":{SRV} 324 {me.nick} {args[0]} +nt")
                    me.send(f":{SRV} 329 {me.nick} {args[0]} {int(time.time())}")
                elif len(args) == 2 and args[1].lstrip("+") in ("b", "e", "I"):
                    end = {"b": "368", "e": "349", "I": "347"}[args[1].lstrip("+")]
                    me.send(f":{SRV} {end} {me.nick} {args[0]} :End of list")
                else:
                    for c in members(args[0]):
                        c.send(f":{me.prefix} MODE {' '.join(args)}")
            elif cmd == "TOPIC" and len(args) >= 2:
                for c in members(args[0]):
                    c.send(f":{me.prefix} TOPIC {args[0]} :{trailing}")
            elif cmd == "PART" and args:
                for chan in args[0].split(","):
                    for c in members(chan):
                        c.send(f":{me.prefix} PART {chan} :{trailing if len(args) > 1 else ''}")
                    channels.get(fold(chan), set()).discard(fold(me.nick))
            elif cmd == "WHO" and args:
                for c in members(args[0]):
                    me.send(f":{SRV} 352 {me.nick} {args[0]} {c.user} 127.0.0.1 {SRV} {c.nick} H :0 {c.nick}")
                me.send(f":{SRV} 315 {me.nick} {args[0]} :End of WHO")
            elif cmd == "WHOIS" and args:
                target = clients.get(fold(args[-1]))
                if target:
                    me.send(f":{SRV} 311 {me.nick} {target.nick} {target.user} 127.0.0.1 * :{target.nick}")
                me.send(f":{SRV} 318 {me.nick} {args[-1]} :End of WHOIS")
            elif cmd in ("PRIVMSG", "NOTICE") and len(args) >= 2:
                target = args[0]
                with open(wire_path, "a", encoding="utf-8") as fh:
                    fh.write(f"{me.nick} {cmd} {target} :{trailing}\n")
                if times_path:
                    with open(times_path, "a", encoding="utf-8") as fh:
                        fh.write(f"{time.time():.3f} {me.nick} {cmd} {target} :{trailing}\n")
                out = f":{me.prefix} {cmd} {target} :{trailing}"
                if target.startswith("#"):
                    for c in members(target):
                        if c is not me:
                            c.send(out)
                elif fold(target) in clients:
                    clients[fold(target)].send(out)
            elif cmd == "TESTRAW" and len(args) >= 2:
                target = clients.get(fold(args[0]))
                if target:
                    target.send(trailing)
            elif cmd == "TESTDROP" and args:
                target = clients.get(fold(args[0]))
                if target:
                    target.writer.close()
            elif cmd == "TESTDELAY" and len(args) >= 2 and NICK_RULES:
                delayed[fold(args[0])] = time.time() + float(args[1])
            elif cmd == "TESTREFUSE" and args and NICK_RULES:
                erroneous.add(fold(args[0]))
            elif cmd == "TESTNICK" and len(args) >= 2 and NICK_RULES:
                target = clients.get(fold(args[0]))
                if target:
                    rename(target, args[1])
            elif cmd == "ISON":
                on = [clients[fold(n)].nick for n in " ".join(args).lstrip(":").split() if fold(n) in clients]
                me.send(f":{SRV} 303 {me.nick} :{' '.join(on)}")
            elif cmd == "MONITOR" and MONITOR and args:
                op = args[0]
                targets = [t for t in (args[1].lstrip(":").split(",") if len(args) > 1 else []) if t]
                if op == "+":
                    for t in targets:
                        me.monitor.add(fold(t))
                        watchers.setdefault(fold(t), set()).add(me)
                    on = [clients[fold(t)].prefix for t in targets if fold(t) in clients]
                    off = [t for t in targets if fold(t) not in clients]
                    if on:
                        me.send(f":{SRV} 730 {me.nick} :{','.join(on)}")
                    if off:
                        me.send(f":{SRV} 731 {me.nick} :{','.join(off)}")
                elif op == "-":
                    for t in targets:
                        me.monitor.discard(fold(t))
                        watchers.get(fold(t), set()).discard(me)
                elif op.upper() == "C":
                    for t in me.monitor:
                        watchers.get(t, set()).discard(me)
                    me.monitor.clear()
            elif cmd == "QUIT":
                if NICK_RULES:
                    for c in neighbours(me):
                        c.send(f":{me.prefix} QUIT :{trailing if args else ''}")
                break
            elif cmd not in ("CAP", "PONG", "USERHOST", "AWAY", "MODE", "TOPIC"):
                me.send(f":{SRV} 421 {me.nick} {cmd} :Unknown command")
            await writer.drain()
    except ConnectionError:
        pass  # the client reset its connection: it is gone, as with QUIT
    finally:
        gone = clients.get(fold(me.nick)) is me
        clients.pop(fold(me.nick), None)
        for chan in channels.values():
            chan.discard(fold(me.nick))
        for t in me.monitor:
            watchers.get(t, set()).discard(me)
        if gone:
            nick_offline(me.nick)
        writer.close()


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--wire", required=True)
    parser.add_argument("--wire-times", help="every PRIVMSG/NOTICE again, with the time it arrived")
    parser.add_argument("--trace", help="log every line in and out with the time (off by default)")
    parser.add_argument("--network", default="E2ETest")
    parser.add_argument("--casemapping", choices=("ascii", "rfc1459", "strict-rfc1459", "utf8"))
    parser.add_argument("--hide-casemapping", action="store_true",
                        help="fold as --casemapping says, but name no CASEMAPPING in 005")
    parser.add_argument("--register-delay", type=float, default=0,
                        help="wait this many seconds before answering USER (a slow registration)")
    parser.add_argument("--nick-rules", action="store_true",
                        help="433 for a nick in use, QUIT to the channels, TESTDELAY (437), TESTNICK")
    parser.add_argument("--monitor", action="store_true", help="MONITOR=100 in 005, 730/731")
    args = parser.parse_args()
    global NETWORK, CASEMAPPING, HIDE_CASEMAPPING, TRACE, REGISTER_DELAY, NICK_RULES, MONITOR
    REGISTER_DELAY = args.register_delay
    NICK_RULES = args.nick_rules
    MONITOR = args.monitor
    TRACE = args.trace
    NETWORK = args.network
    CASEMAPPING = args.casemapping
    HIDE_CASEMAPPING = args.hide_casemapping
    server = await asyncio.start_server(lambda r, w: handle(r, w, args.wire, args.wire_times), "127.0.0.1", args.port)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
