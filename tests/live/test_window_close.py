#!/usr/bin/env python3
"""Closing a window disconnects a server nothing shows any more
(window_close_disconnects): the network window of a network without
channels, a network that keeps a channel, the setting off, a pending
reconnect. Two fake servers: Talk (no channels) and E2E (#test).
Usage: test_window_close.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
QUIT_MSG = "closing time here"
NOTICE = "its last window was closed"
RECONNECT = 12  # server_reconnect_time, seconds


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-window-close."))
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

    def wait_log_re(pattern, offset, timeout=WAIT):
        end = time.time() + timeout
        while True:
            if re.search(pattern, log_since(offset)):
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

    def servers():
        """The /SERVER list: connected, connecting and waiting to reconnect"""
        off = log_size()
        st["alice"].type("/sl")
        assert wait_log_since(off, "sl-done"), "no /SERVER output:\n" + log_since(off)
        return log_since(off)

    def tags():
        return re.findall(r"^\S+ (\S+): 127\.0\.0\.1:", servers(), re.M)

    def wait_tags(want, timeout=WAIT):
        end = time.time() + timeout
        while True:
            got = sorted(tags())
            if got == sorted(want) or time.time() >= end:
                return got
            time.sleep(0.3)

    def windows():
        off = log_size()
        st["alice"].type("/window list")
        st["alice"].type("/echo wl-done")
        assert wait_log_since(off, "wl-done"), "no /WINDOW LIST output"
        return log_since(off)

    def close_window(name):
        """go to the window named NAME and close it; returns the output mark"""
        a = st["alice"]
        a.type(f"/window goto {name}")
        a.type("/alias wn echo active=$winname")
        off = log_size()
        a.type("/wn")
        assert wait_log_since(off, f"active={name}"), f"window {name} not active:\n" + log_since(off)
        mark = a.mark()
        a.type("/window close")
        return mark

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
        cfg = cfg.replace('core = {', f'core = {{ quit_message = "{QUIT_MSG}"; '
                          f'server_reconnect_time = "{RECONNECT}s";', 1)
        assert "server_reconnect_time" in cfg, cfg
        a = harness.Erssi(st["inst"], work, "alice", cfg)
        st["alice"] = a
        a.start()
        assert a.wait_file(a.all_log(), "has joined #test", 15), "alice never joined #test"
        a.type("/alias sl server;echo sl-done")
        got = wait_tags(["E2E", "Talk"])
        assert got == ["E2E", "Talk"], got
        wl = windows()
        assert re.search(r"^\S+ \d+\s+Talk\s", wl, re.M), "no Talk network window:\n" + wl

    def t_close_disconnects():
        """the Talk network window was all of Talk: closing it quits Talk with
        the quit message, says so in the window that becomes active, and
        /CONNECT Talk gets the tag Talk again"""
        a = st["alice"]
        start_e2e = len(trace("e2e"))
        # an empty window without a level whose active server is Talk shows
        # nothing of Talk (the Mentions window is such a window too)
        a.type("/window new hidden")
        a.type("/window name spare")
        a.type("/window server Talk")
        off = log_size()
        a.type("/window")
        assert wait_log_since(off, "Server  : Talk"), "spare window not on Talk:\n" + log_since(off)
        mark = close_window("Talk")
        assert wait_trace("talk", rf" in alice QUIT :{QUIT_MSG}$"), \
            "Talk got no QUIT:\n" + trace("talk")[-2000:]
        assert a.wait_output(NOTICE.encode(), WAIT), "no notice on the screen:\n" + a.text(mark)[-2000:]
        line = [x for x in a.text(mark).splitlines() if NOTICE in x]
        assert line and "Talk" in line[0] and "/CONNECT Talk" in line[0], line
        got = wait_tags(["E2E"])
        assert got == ["E2E"], got
        assert "QUIT" not in trace("e2e")[start_e2e:], "E2E was disconnected too"
        assert not re.search(r"RECON-\d+", servers()), "a reconnect is left"
        off = log_size()
        a.type("/connect Talk")
        assert wait_log_since(off, "Connection to 127.0.0.1 established"), "no new connection"
        got = wait_tags(["E2E", "Talk"])
        assert got == ["E2E", "Talk"], got
        wl = windows()
        assert re.search(r"^\S+ \d+\s+Talk\s", wl, re.M), "no Talk network window:\n" + wl
        assert "Talk2" not in wl, wl

    def t_channel_keeps_server():
        """E2E has #test in another window: closing its network window leaves
        the connection alone"""
        a = st["alice"]
        start = len(trace("e2e"))
        mark = close_window("E2E")
        time.sleep(2)
        assert "QUIT" not in trace("e2e")[start:], "E2E was disconnected:\n" + trace("e2e")[start:]
        assert NOTICE not in a.text(mark), a.text(mark)[-2000:]
        assert sorted(tags()) == ["E2E", "Talk"], tags()

    def t_reconnect_dropped():
        """Talk lost its link and waits to reconnect: closing its window
        drops the reconnect, nothing connects later"""
        a = st["alice"]
        start = len(trace("talk"))
        with harness.Client(st["talk_port"], "dropper") as c:
            c.send("TESTDROP alice")
            assert a.wait_file(a.all_log(), "Connection lost to 127.0.0.1", WAIT), "the link did not drop"
        assert re.search(r"RECON-\d+: 127\.0\.0\.1:\d+ \(Talk\)", servers()), servers()
        dropped = time.time()
        mark = close_window("Talk")
        assert a.wait_output(NOTICE.encode(), WAIT), "no notice on the screen:\n" + a.text(mark)[-2000:]
        out = servers()
        assert not re.search(r"RECON-\d+", out), "the reconnect is still there:\n" + out
        # past the reconnect time nothing came back
        time.sleep(max(0, dropped + RECONNECT + 3 - time.time()))
        assert not re.search(r" in alice NICK ", trace("talk")[start:]), \
            "erssi reconnected:\n" + trace("talk")[start:]
        assert sorted(tags()) == ["E2E"], tags()
        a.type("/connect Talk")
        got = wait_tags(["E2E", "Talk"])
        assert got == ["E2E", "Talk"], got

    def t_setting_off():
        """window_close_disconnects off: the old behaviour - the connection
        stays; /CONNECT Talk then says it is connected (no Talk2)"""
        a = st["alice"]
        off = log_size()
        a.type("/set window_close_disconnects off")
        assert wait_log_re(r"window_close_disconnects\s+(= )?OFF", off), \
            "setting not changed:\n" + log_since(off)
        start = len(trace("talk"))
        close_window("Talk")
        time.sleep(2)
        assert "QUIT" not in trace("talk")[start:], "Talk was disconnected with the setting off"
        off = log_size()
        a.type("/connect Talk")
        assert wait_log_since(off, "Already connected to Talk"), log_since(off)
        assert sorted(tags()) == ["E2E", "Talk"], tags()

    def t_quit():
        a = st["alice"]
        a.quit()
        assert a.sanitizer_reports() == [], a.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_close_disconnects, t_channel_keeps_server,
                      t_reconnect_dropped, t_setting_off, t_quit])
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
