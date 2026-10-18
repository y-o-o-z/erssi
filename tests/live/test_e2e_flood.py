#!/usr/bin/env python3
"""E2E under a flood of strangers, with irssi's real send queue.

alice and bob (native e2e module) read each other on #test. alice gets
irssi's default flood rule (cmd_queue_speed 2200 ms, 5 at once, through
/reconnect). 60 raw clients in #test post junk +RPE2E01 lines every 5 s:
alice asks each of them for a key (strangers, normal mode). Meanwhile
alice types a long line (several encrypted chunks) and short ones: all
of them must leave within the bound (the strangers' NOTICEs never wait
in irssi's queue ahead of them), and bob reads them. Then /e2e rotate
under the flood: the REKEY reaches bob and he reads the next line.
Usage: test_e2e_flood.py <build dir> [<source dir>]"""
import base64
import json
import os
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 30
BOUND = 30  # seconds: a typed line's chunks are all out by then


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-e2e-flood."))
    st = {"erssis": [], "ircd": None, "clones": [], "stop": threading.Event()}
    times = work / "wire-times.log"

    def chan_log(e):
        try:
            return (e.home / "logs" / "E2E" / "#test.log").read_text(errors="replace")
        except OSError:
            return ""

    def wait_chan(e, text, timeout=WAIT):
        return harness.wait_until(lambda: text in chan_log(e), timeout)

    def lines_since(t0, nick, cmd):
        out = []
        try:
            for ln in times.read_text(errors="replace").splitlines():
                f = ln.split(" ", 4)
                if len(f) >= 4 and float(f[0]) >= t0 and f[1] == nick and f[2] == cmd:
                    out.append((float(f[0]), ln))
        except OSError:
            pass
        return out

    def up(name, nick, cfg_extra=""):
        cfg = harness.config(st["port"], nick)
        e = harness.Erssi(st["inst"], work, name, cfg)
        st["erssis"].append(e)
        e.start()
        e.wait_ready()
        assert e.wait_file(e.all_log(), "has joined #test", WAIT), f"{name} never joined"
        e.type_and_wait("/load e2e", "/window goto #test", "/e2e mode auto")
        return e

    def junk():
        return "+RPE2E01 %s %d 1/1 %s:%s" % (os.urandom(8).hex(), int(time.time()),
                                            base64.b64encode(os.urandom(24)).decode(),
                                            base64.b64encode(os.urandom(40)).decode())

    def flood():
        while not st["stop"].is_set():
            for c in st["clones"]:
                try:
                    c.send(f"PRIVMSG #test :{junk()}")
                except OSError:
                    pass
                time.sleep(5 / max(1, len(st["clones"])))

    def t_install():
        st["inst"] = harness.install(build, work)
        if not list(st["inst"].moddir.glob("libe2e_core.so")):
            raise harness.Skip("erssi built without e2e")
        st["ircd"] = harness.Ircd(work, ["--wire-times", str(times)])
        st["port"] = st["ircd"].start()

    def t_pair():
        a = st["alice"] = up("alice", "alice")
        b = st["bob"] = up("bob", "bob")
        # the first line of each asks the other for the key
        b.type_and_wait("/msg #test first from bob")
        a.type_and_wait("/msg #test first from alice")

        def paired():
            try:
                ka = json.loads((a.home / "rpe2e" / "keyring.json").read_text())
                kb = json.loads((b.home / "rpe2e" / "keyring.json").read_text())
            except (OSError, ValueError):
                return False
            return ka["incoming"].get("bob@127.0.0.1|E2E\x1f#test", {}).get("status") == "trusted" and \
                kb["incoming"].get("alice@127.0.0.1|E2E\x1f#test", {}).get("status") == "trusted"
        assert harness.wait_until(paired, WAIT), "no key exchange"
        b.type_and_wait("/msg #test hello alice")
        a.type_and_wait("/msg #test hello bob")
        assert harness.wait_until(lambda: "hello alice" in chan_log(a) and "hello bob" in chan_log(b), WAIT), \
            (chan_log(a)[-1500:], chan_log(b)[-1500:])
        # strangers ask in normal mode; irssi's own flood rule
        off = a.all_log().stat().st_size
        a.type_and_wait("/e2e mode normal", "/set cmd_queue_speed 2200msec", "/set cmds_max_at_once 5")
        a.type_and_wait("/reconnect")
        assert harness.wait_until(lambda: "has joined #test" in a.all_log().read_bytes()[off:].decode(errors="replace"),
                                  WAIT)
        time.sleep(5)

    def t_flood_lines():
        """the user's lines leave within the bound while 60 strangers
        flood the channel with junk ciphertext"""
        a, b = st["alice"], st["bob"]
        a.type_and_wait("/set e2e_debug on")
        off = a.all_log().stat().st_size
        start = time.time()
        for i in range(60):
            c = harness.Client(st["port"], f"clone{i}", f"c{i}")
            c.send("JOIN #test")
            st["clones"].append(c)
        threading.Thread(target=flood, daemon=True).start()
        time.sleep(15)
        assert lines_since(start, "alice", "NOTICE"), "alice asked no clone (no flood)"
        long_line = "long " + " ".join(f"word{i:03d}" for i in range(55))  # about 450 bytes
        t0 = time.time()
        a.type_and_wait("/window goto #test", f"/msg #test {long_line}")
        for i in range(3):
            a.type_and_wait(f"/msg #test short line {i}")
            time.sleep(1)
        for text in ("word054", "short line 0", "short line 1", "short line 2"):
            assert harness.wait_until(lambda: text in chan_log(b), BOUND + 5), (text, chan_log(b)[-2000:])
        sent = lines_since(t0, "alice", "PRIVMSG")
        assert sent, "alice sent nothing"
        last = max(t for t, _ in sent)
        assert last - t0 <= BOUND, f"alice's lines took {last - t0:.1f} s"
        # and no stranger's NOTICE went out while her lines waited
        firsts, lasts = min(t for t, _ in sent), last
        between = [t for t, ln in lines_since(firsts, "alice", "NOTICE") if t < lasts and " clone" in ln]
        assert not between, f"{len(between)} NOTICEs to clones between alice's chunks"
        # "TX" only for what left at once; the rest says it waits
        a.type_and_wait("/set e2e_debug off")
        dbg = a.all_log().read_bytes()[off:].decode(errors="replace")
        tx = dbg.count("TX auto-KEYREQ to clone")
        wire = [ln for _, ln in lines_since(start, "alice", "NOTICE") if " clone" in ln and "KEYREQ" in ln]
        assert "auto-KEYREQ to clone" in dbg and "queued behind your own lines" in dbg, dbg[-2000:]
        assert tx <= len(wire), (tx, len(wire))

    def t_rotate_under_flood():
        """/e2e rotate under the flood: the REKEY reaches bob, he reads"""
        a, b = st["alice"], st["bob"]
        t0 = time.time()
        a.type_and_wait("/window goto #test", "/e2e rotate")
        a.type_and_wait("/msg #test after the rotation")
        assert wait_chan(b, "after the rotation", BOUND + 5), chan_log(b)[-2000:]
        rekeys = [ln for _, ln in lines_since(t0, "alice", "NOTICE") if " bob :" in ln and "REKEY" in ln]
        assert rekeys, "no REKEY to bob"

    def t_stop():
        st["stop"].set()
        for c in st["clones"]:
            c.close()
        for e in st["erssis"]:
            e.quit()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def cleanup():
        st["stop"].set()
        for c in st["clones"]:
            c.close()
        for e in st["erssis"]:
            e.kill()
        if st["ircd"] is not None:
            st["ircd"].stop()

    try:
        harness.main([t_install, t_pair, t_flood_lines, t_rotate_under_flood, t_stop])
    finally:
        cleanup()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
