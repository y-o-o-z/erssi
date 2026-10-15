#!/usr/bin/env python3
"""one_window in a network's block: a query of that network opens in the
network window (named after the server tag), never in a window of its own;
closing that window disconnects, /CONNECT brings back one window with the
plain tag. A network without it keeps a window per query. /NETWORK MODIFY
-onewindow / -noonewindow. Two fake servers: Talk (one_window) and E2E.
Usage: test_query_window.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-query-window."))
    st = {}

    def log_size():
        try:
            return st["alice"].all_log().stat().st_size
        except OSError:
            return 0

    def log_since(offset):
        try:
            return st["alice"].all_log().read_bytes()[offset:].decode(errors="replace")
        except OSError:
            return ""

    def wait_log_since(offset, text, timeout=WAIT):
        end = time.time() + timeout
        while True:
            if text in log_since(offset):
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def trace(name):
        try:
            return (work / name / "ircd-trace.log").read_text(errors="replace")
        except OSError:
            return ""

    def wait_trace(name, pattern, start=0, timeout=WAIT):
        end = time.time() + timeout
        while True:
            if re.search(pattern, trace(name)[start:], re.M):
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def tags():
        off = log_size()
        st["alice"].type("/sl")
        assert wait_log_since(off, "sl-done"), "no /SERVER output:\n" + log_since(off)
        return sorted(re.findall(r"^\S+ (\S+): 127\.0\.0\.1:", log_since(off), re.M))

    def wait_tags(want, timeout=WAIT):
        end = time.time() + timeout
        while True:
            got = tags()
            if got == sorted(want) or time.time() >= end:
                return got
            time.sleep(0.3)

    def windows():
        """the /WINDOW LIST lines: (refnum, rest of the line)"""
        off = log_size()
        st["alice"].type("/window list")
        st["alice"].type("/echo wl-done")
        assert wait_log_since(off, "wl-done"), "no /WINDOW LIST output"
        return re.findall(r"^\S+ (\d+)\s+(.*)$", log_since(off).split("wl-done")[0], re.M)

    def where(nick):
        """(window name, active item, refnum) of the window of the query with NICK"""
        a = st["alice"]
        a.type(f"/window goto {nick}")
        off = log_size()
        a.type("/wi")
        assert wait_log_since(off, "at=["), "no /wi output"
        m = re.findall(r"at=\[([^\]]*)\] item=\[([^\]]*)\] ref=(\d+)", log_since(off))
        assert m, log_since(off)
        return m[-1]

    def message(port, nick, text):
        """NICK says TEXT to alice; returns once alice has it"""
        off = log_size()
        with harness.Client(port, nick) as c:
            c.send(f"PRIVMSG alice :{text}")
            assert wait_log_since(off, text), f"alice did not get {text!r}"

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_start():
        for name in ("talk", "e2e"):
            (work / name).mkdir()
            st[name] = harness.Ircd(work / name)
            st[name + "_port"] = st[name].start()
        cfg = harness.config(0, "alice", servers=[("Talk", st["talk_port"]), ("E2E", st["e2e_port"])])
        no_chan = '{ name = "#test"; chatnet = "Talk"; autojoin = "yes"; }, '
        assert no_chan in cfg, cfg
        cfg = cfg.replace(no_chan, "")
        net = 'Talk = { type = "IRC"; };'
        assert net in cfg, cfg
        cfg = cfg.replace(net, 'Talk = { type = "IRC"; one_window = "yes"; };')
        a = harness.Erssi(st["inst"], work, "alice", cfg)
        st["alice"] = a
        a.start()
        assert a.wait_file(a.all_log(), "has joined #test", 15), "alice never joined #test"
        a.type("/alias sl server;echo sl-done")
        a.type("/alias wi echo at=[$winname] item=[$T] ref=$winref")
        got = wait_tags(["E2E", "Talk"])
        assert got == ["E2E", "Talk"], got
        st["windows"] = windows()
        assert [w for w in st["windows"] if w[1].startswith("Talk ")], st["windows"]

    def t_query_in_network_window():
        message(st["talk_port"], "peer", "hello from peer")
        name, item, ref = where("peer")
        assert (name, item) == ("Talk", "peer"), (name, item, ref)
        net = [w for w in st["windows"] if w[1].startswith("Talk ")]
        assert ref == net[0][0], (ref, net)
        assert len(windows()) == len(st["windows"]), windows()

    def t_close_and_connect():
        a = st["alice"]
        start = len(trace("talk"))
        a.type("/window goto Talk")
        a.type("/window close")
        assert wait_trace("talk", r" in alice QUIT ", start), "Talk was not disconnected"
        assert wait_tags(["E2E"]) == ["E2E"]
        a.type("/connect Talk")
        got = wait_tags(["E2E", "Talk"])
        assert got == ["E2E", "Talk"], got
        message(st["talk_port"], "peer", "hello again")
        name, item, _ = where("peer")
        assert (name, item) == ("Talk", "peer"), (name, item)
        wl = windows()
        assert len(wl) == len(st["windows"]), wl
        assert not [w for w in wl if "Talk2" in w[1]], wl

    def t_option_off_own_window():
        before = len(windows())
        message(st["e2e_port"], "peer2", "hello from peer2")
        name, item, _ = where("peer2")
        assert item == "peer2" and name != "E2E", (name, item)
        assert len(windows()) == before + 1, windows()

    def t_network_modify():
        a = st["alice"]
        off = log_size()
        a.type("/network modify -onewindow E2E")
        a.type("/network")
        a.type("/echo nl-done")
        assert wait_log_since(off, "nl-done"), "no /NETWORK output"
        assert re.search(r"^\S+ E2E: .*one_window", log_since(off), re.M), \
            "/NETWORK does not list one_window for E2E:\n" + log_since(off)
        message(st["e2e_port"], "peer3", "hello from peer3")
        name, item, _ = where("peer3")
        assert (name, item) == ("E2E", "peer3"), (name, item)
        a.type("/network modify -noonewindow E2E")
        message(st["e2e_port"], "peer4", "hello from peer4")
        name, item, _ = where("peer4")
        assert item == "peer4" and name != "E2E", (name, item)

    def t_quit():
        a = st["alice"]
        a.quit()
        assert a.sanitizer_reports() == [], a.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_query_in_network_window, t_close_and_connect,
                      t_option_off_own_window, t_network_modify, t_quit])
    finally:
        if "alice" in st:
            st["alice"].kill()
        for name in ("talk", "e2e"):
            if name in st:
                st[name].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
