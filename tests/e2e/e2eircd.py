#!/usr/bin/env python3
"""A minimal IRC server for testing rpe2e.pl between two erssi clients.

127.0.0.1 only. It handles what erssi and rpe2e.pl need: registration
(CAP/NICK/USER), PING, JOIN with NAMES, MODE, TOPIC, PART, nick changes, WHO,
WHOIS (311 - rpe2e.pl learns its own ident@host from it), PRIVMSG/NOTICE to a
channel and to a nick (CTCP too, which carries KEYREQ/KEYRSP). Every
PRIVMSG/NOTICE line is also written to the --wire file, so the test can check
what really crossed the server.
"""
import argparse
import asyncio

SRV = "e2e.test"
clients = {}   # nick (lower) -> Client
channels = {}  # channel (lower) -> set(nick lower)


class Client:
    def __init__(self, writer):
        self.writer = writer
        self.nick = "*"
        self.user = "u"

    @property
    def prefix(self):
        return f"{self.nick}!{self.user}@127.0.0.1"

    def send(self, line):
        self.writer.write((line + "\r\n").encode())


def members(chan):
    return [clients[n] for n in channels.get(chan.lower(), ()) if n in clients]


async def handle(reader, writer, wire_path):
    me = Client(writer)
    try:
        while line := await reader.readline():
            text = line.decode(errors="replace").rstrip("\r\n")
            if not text:
                continue
            parts = text.split(" ")
            cmd, args = parts[0].upper(), parts[1:]
            trailing = text.split(" :", 1)[1] if " :" in text else (args[-1] if args else "")
            if cmd == "CAP" and args and args[0] == "LS":
                me.send(f":{SRV} CAP * LS :")
            elif cmd == "NICK":
                new = args[0].lstrip(":")
                if me.nick != "*":
                    seen = {me}
                    for chan, nicks in channels.items():
                        if me.nick.lower() in nicks:
                            nicks.discard(me.nick.lower())
                            nicks.add(new.lower())
                            for other in [clients[n] for n in nicks if n in clients]:
                                seen.add(other)
                    for c in seen:
                        c.send(f":{me.prefix} NICK :{new}")
                    clients.pop(me.nick.lower(), None)
                me.nick = new
                clients[me.nick.lower()] = me
            elif cmd == "USER":
                me.user = args[0]
                for num, msg in (("001", f"Welcome {me.nick}"), ("002", f"Your host is {SRV}"), ("003", "today"),
                                 ("004", f"{SRV} e2e io biklmnopstv")):
                    me.send(f":{SRV} {num} {me.nick} :{msg}")
                me.send(f":{SRV} 005 {me.nick} CHANTYPES=# PREFIX=(ov)@+ NETWORK=E2ETest :are supported")
                me.send(f":{SRV} 376 {me.nick} :End of MOTD")
            elif cmd == "PING":
                me.send(f":{SRV} PONG {SRV} :{args[0].lstrip(':') if args else ''}")
            elif cmd == "JOIN":
                for chan in args[0].split(","):
                    channels.setdefault(chan.lower(), set()).add(me.nick.lower())
                    for other in members(chan):
                        other.send(f":{me.prefix} JOIN {chan}")
                    names = " ".join(c.nick for c in members(chan))
                    me.send(f":{SRV} 353 {me.nick} = {chan} :{names}")
                    me.send(f":{SRV} 366 {me.nick} {chan} :End of NAMES")
            elif cmd == "MODE" and args and args[0].startswith("#"):
                if len(args) == 1:
                    me.send(f":{SRV} 324 {me.nick} {args[0]} +nt")
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
                    channels.get(chan.lower(), set()).discard(me.nick.lower())
            elif cmd == "WHO" and args:
                for c in members(args[0]):
                    me.send(f":{SRV} 352 {me.nick} {args[0]} {c.user} 127.0.0.1 {SRV} {c.nick} H :0 {c.nick}")
                me.send(f":{SRV} 315 {me.nick} {args[0]} :End of WHO")
            elif cmd == "WHOIS" and args:
                target = clients.get(args[-1].lower())
                if target:
                    me.send(f":{SRV} 311 {me.nick} {target.nick} {target.user} 127.0.0.1 * :{target.nick}")
                me.send(f":{SRV} 318 {me.nick} {args[-1]} :End of WHOIS")
            elif cmd in ("PRIVMSG", "NOTICE") and len(args) >= 2:
                target = args[0]
                with open(wire_path, "a", encoding="utf-8") as fh:
                    fh.write(f"{me.nick} {cmd} {target} :{trailing}\n")
                out = f":{me.prefix} {cmd} {target} :{trailing}"
                if target.startswith("#"):
                    for c in members(target):
                        if c is not me:
                            c.send(out)
                elif target.lower() in clients:
                    clients[target.lower()].send(out)
            elif cmd == "QUIT":
                break
            await writer.drain()
    finally:
        clients.pop(me.nick.lower(), None)
        for chan in channels.values():
            chan.discard(me.nick.lower())
        writer.close()


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--wire", required=True)
    args = parser.parse_args()
    server = await asyncio.start_server(lambda r, w: handle(r, w, args.wire), "127.0.0.1", args.port)
    async with server:
        await server.serve_forever()


asyncio.run(main())
