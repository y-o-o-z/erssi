#!/usr/bin/env python3
"""E2E: a peer comes back from another host with the same key.

alice and bob (both with the native e2e module) exchange keys on #test
and read each other. bob then reconnects under another ident (a new
ident@host), with the same keyring, and writes encrypted again. alice must
see a line in place of every message she cannot read, and the warning
that his known key appeared under a new address, with the command that
accepts it; after /e2e reverify <new address> his next message is read.
Nothing of it crosses the server in clear text.
Usage: test_e2e_trust.py <build dir> [<source dir>]"""
import json
import os
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 20


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-e2e-trust."))
    st = {"erssis": [], "ircd": None}

    def erssi(name, port, nick, user=None):
        cfg = harness.config(port, nick)
        if user is not None:
            old = f'user_name = "{nick}";'
            assert old in cfg, cfg
            cfg = cfg.replace(old, f'user_name = "{user}";')
        e = harness.Erssi(st["inst"], work, name, cfg)
        st["erssis"].append(e)
        return e

    def chan_log(e):
        try:
            return (e.home / "logs" / "E2E" / "#test.log").read_text(errors="replace")
        except OSError:
            return ""

    def wait_chan(e, text, start=0, timeout=WAIT):
        return harness.wait_until(lambda: text in chan_log(e)[start:], timeout)

    def keyring(e):
        try:
            return json.loads((e.home / "rpe2e" / "keyring.json").read_text())
        except (OSError, ValueError):
            return {}

    def session(e, handle):
        """the status of e's session from handle on E2E's #test, or None"""
        row = keyring(e).get("incoming", {}).get(f"{handle}|E2E\x1f#test")
        return row.get("status") if row else None

    def log_size(e):
        try:
            return e.all_log().stat().st_size
        except OSError:
            return 0

    def log_since(e, off):
        try:
            return e.all_log().read_bytes()[off:].decode(errors="replace")
        except OSError:
            return ""

    def wait_log(e, off, text, timeout=WAIT):
        return harness.wait_until(lambda: text in log_since(e, off), timeout)

    def up(e):
        """started, the module loaded, on #test with E2E on in its window"""
        off = log_size(e)
        e.start()
        e.wait_ready()
        assert wait_log(e, off, "has joined #test"), f"{e.name} never joined #test"
        e.type_and_wait("/load e2e", "/window goto #test", "/e2e on")

    def say(e, text):
        e.type_and_wait(f"/msg #test {text}")

    def t_install():
        st["inst"] = harness.install(build, work)
        if not list(st["inst"].moddir.glob("libe2e_core.so")):
            raise harness.Skip("erssi built without e2e")
        st["ircd"] = harness.Ircd(work)
        st["port"] = st["ircd"].start()

    def t_first_contact():
        """the first exchange: each one's first message is not read but
        says so and asks for the key; one /e2e accept on each side"""
        a = st["alice"] = erssi("alice", st["port"], "alice")
        k = st["bob"] = erssi("bob", st["port"], "bob")
        up(a)
        up(k)
        say(k, "first words")
        assert wait_chan(a, "[E2E: encrypted message not read - no key from bob "
                            "(bob@127.0.0.1) yet - key requested]"), chan_log(a)[-2000:]
        assert k.wait_file(k.all_log(), "Pending key exchange from alice", WAIT), "bob: no request"
        k.type_and_wait("/window goto #test", "/e2e accept alice")
        assert a.wait_file(a.all_log(), "Pending key exchange from bob", WAIT), "alice: no request"
        a.type_and_wait("/window goto #test", "/e2e accept bob")
        assert harness.wait_until(lambda: session(a, "bob@127.0.0.1") == "trusted", WAIT), keyring(a)
        assert harness.wait_until(lambda: session(k, "alice@127.0.0.1") == "trusted", WAIT), keyring(k)
        say(k, "can you read me")
        assert wait_chan(a, "can you read me"), chan_log(a)[-2000:]
        say(a, "yes I can")
        assert wait_chan(k, "yes I can"), chan_log(k)[-2000:]

    def t_new_address():
        """bob back as bob2@127.0.0.1 with the same key: every
        message of his leaves a line, the blocked exchange is told with
        the command; /e2e status and /e2e list name it"""
        a, k = st["alice"], st["bob"]
        k.quit()
        k.config_text = harness.config(st["port"], "bob").replace(
            'user_name = "bob";', 'user_name = "bob2";')
        start = len(chan_log(a))
        off = log_size(a)
        up(k)
        assert wait_log(a, off, "bob2@127.0.0.1"), "alice did not see bob back"
        say(k, "secret one")
        assert wait_chan(a, "[E2E: encrypted message not read - no key from bob "
                            "(bob2@127.0.0.1) yet - key requested]", start), chan_log(a)[start:]
        assert wait_chan(a, "Notice: the known key", start), chan_log(a)[start:]
        log = chan_log(a)[start:]
        assert "appeared under a new address, bob2@127.0.0.1 (before: bob@127.0.0.1)" in log, log
        assert "/e2e reverify bob2@127.0.0.1" in log, log
        mid = len(chan_log(a))
        say(k, "secret two")
        assert wait_chan(a, "[E2E: encrypted message not read - the known key of bob appeared "
                            "under a new address, bob2@127.0.0.1; if it is the same person: "
                            "/e2e reverify bob2@127.0.0.1]", mid), chan_log(a)[mid:]
        off = log_size(a)
        a.type_and_wait("/e2e status", "/window goto #test", "/e2e list")
        out = log_since(a, off)
        assert out.count("blocked: bob (bob2@127.0.0.1) on E2E:#test") >= 2, out

    def t_shellter_column():
        """in the shellter theme the module's lines are in the column,
        "e2e │ ...", and a long one wraps under its text, not to column 0"""
        a = st["alice"]
        if len(sys.argv) < 3:
            raise harness.SkipCase("no source dir: the shellter theme is not at hand")
        shutil.copy(Path(sys.argv[2]) / "themes" / "shellter.theme", a.home / "shellter.theme")
        a.resize(120, 50)  # the line is longer than the window
        a.type_and_wait("/set theme shellter", "/window goto #test", "/e2e list")

        def found(rows):
            return any("e2e │   blocked: bob (bob2@127.0.0.1)" in r for r in rows)
        rows, ok = a.wait_screen(found, WAIT)
        assert ok, "\n".join(rows)
        i = max(n for n, r in enumerate(rows) if "e2e │   blocked: bob" in r)
        # one column: erssi's own label does not come first
        assert "erssi │" not in rows[i], rows[i]
        text_col = rows[i].index("e2e │ ") + len("e2e │ ")
        # the line starts with its time; the next row continues it, from
        # the window's first column to the text's blank (side panels aside)
        start = re.search(r"\d\d:\d\d", rows[i]).start()
        follow = rows[i + 1]
        assert follow[start:text_col].strip() == "" and follow[text_col] != " ", (rows[i], follow)
        a.type_and_wait("/set theme default")

    def t_reverify():
        """/e2e reverify <new address> moves the key there and asks for it
        again: bob's next message is read"""
        a, k = st["alice"], st["bob"]
        off = log_size(a)
        a.type_and_wait("/window goto #test", "/e2e reverify bob2@127.0.0.1")
        out = log_since(a, off)
        assert "is bob2@127.0.0.1 now (it was bob@127.0.0.1)" in out, out
        # the session of his blocked answer: no new exchange is needed
        assert "their messages on E2E:#test are read again" in out, out
        assert session(a, "bob2@127.0.0.1") == "trusted", keyring(a)
        assert session(a, "bob@127.0.0.1") is None, keyring(a)
        start = len(chan_log(a))
        say(k, "secret three")
        assert wait_chan(a, "secret three", start), chan_log(a)[start:]
        assert "not read" not in chan_log(a)[start:], chan_log(a)[start:]

    def t_bot_lines():
        """a line that looks like a bot command is encrypted like any
        other; with e2e_bot_bypass on it goes out in clear text, warned"""
        a, k = st["alice"], st["bob"]
        start = len(chan_log(k))
        say(a, "!secret bot line")
        assert wait_chan(k, "!secret bot line", start), chan_log(k)[start:]
        assert "!secret bot line" not in (work / "wire.log").read_text(errors="replace")
        off = log_size(a)
        a.type_and_wait("/set e2e_bot_bypass on", "/msg #test !seen clearbot", "/set e2e_bot_bypass off")
        assert harness.wait_until(lambda: "PRIVMSG #test :!seen clearbot" in
                                  (work / "wire.log").read_text(errors="replace"), WAIT)
        assert wait_log(a, off, "bot-style message to #test sent in CLEARTEXT"), log_since(a, off)

    def t_unreadable_keyring():
        """a keyring that cannot be read: /e2e export writes no empty
        export, /e2e rotate claims nothing - both say why"""
        a = st["alice"]
        ring = a.home / "rpe2e" / "keyring.json"
        dest = work / "export.json"
        ring.chmod(0)
        try:
            if os.access(ring, os.R_OK):
                raise harness.SkipCase("the keyring stays readable (run as root?)")
            off = log_size(a)
            a.type_and_wait("/window goto #test", f"/e2e export {dest}", "/e2e rotate")
            out = log_since(a, off)
        finally:
            ring.chmod(0o600)
        assert out.count("cannot be read") == 2, out
        assert "exported keyring" not in out and "rotation scheduled" not in out, out
        assert not dest.exists()

    def t_wire():
        """nothing of it went over the server in clear text; no sanitizer
        reports"""
        wire = (work / "wire.log").read_text(errors="replace")
        for text in ("first words", "can you read me", "yes I can", "secret one", "secret two",
                     "secret three"):
            assert text not in wire, text
        assert "+RPE2E01" in wire
        for e in st["erssis"]:
            e.quit()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def cleanup():
        for e in st["erssis"]:
            e.kill()
        if st["ircd"] is not None:
            st["ircd"].stop()

    try:
        harness.main([t_install, t_first_contact, t_new_address, t_shellter_column, t_reverify,
                      t_bot_lines, t_unreadable_keyring, t_wire])
    finally:
        cleanup()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
