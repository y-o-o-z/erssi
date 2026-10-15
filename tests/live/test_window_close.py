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
        return re.findall(r"^\S+ (\S+): (?:127\.0\.0\.1|localhost):", servers(), re.M)

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


    def t_range_close():
        """/WINDOW CLOSE <first> <last> over Talk's network window and its
        channel window: one disconnect, one line"""
        a = st["alice"]
        a.type("/window goto Talk")
        a.type("/join #range")
        assert a.wait_file(a.all_log(), "has joined #range", WAIT), "no #range"
        wl = windows()
        net = re.search(r"^\S+ (\d+)\s+Talk\s", wl, re.M)
        chan = re.search(r"^\S+ (\d+)\s+\S*\s*#range\s", wl, re.M)
        assert net and chan, wl
        first, last = sorted((int(net.group(1)), int(chan.group(1))))
        inside = [int(r) for r in re.findall(r"^\S+ (\d+)\s", wl, re.M) if first <= int(r) <= last]
        assert len(inside) == 2, f"windows {first}..{last} are not just Talk and #range:\n" + wl
        start = len(trace("talk"))
        mark = a.mark()
        off = log_size()
        a.type(f"/window close {first} {last}")
        assert wait_trace("talk", rf" in alice QUIT :{QUIT_MSG}$", start), trace("talk")[start:]
        time.sleep(1)
        assert log_since(off).count(NOTICE) == 1, log_since(off)
        assert len(re.findall(r" in alice QUIT ", trace("talk")[start:])) == 1
        assert a.wait_output(NOTICE.encode(), WAIT), a.text(mark)[-2000:]
        assert sorted(tags()) == ["E2E"], tags()
        a.type("/connect Talk")
        got = wait_tags(["E2E", "Talk"])
        assert got == ["E2E", "Talk"], got

    def t_address_comeback():
        """a server of no network: the comeback hint keeps its port"""
        a = st["alice"]
        (work / "bare").mkdir()
        st["bare"] = harness.Ircd(work / "bare")
        port = st["bare"].start()
        before = set(tags())
        # by a name of no setup server (127.0.0.1 would match Talk's setup and
        # join its network)
        a.type(f"/connect -4 localhost {port}")
        end = time.time() + WAIT
        while len(set(tags()) - before) != 1 and time.time() < end:
            time.sleep(0.3)
        new = set(tags()) - before
        assert len(new) == 1, tags()
        tag = new.pop()
        end = time.time() + WAIT
        while not re.search(rf"^\S+ \d+\s+{re.escape(tag)}\s", windows(), re.M) and time.time() < end:
            time.sleep(0.3)
        off = log_size()
        close_window(tag)
        assert wait_log_since(off, NOTICE), log_since(off)
        line = [x for x in log_since(off).splitlines() if NOTICE in x]
        assert f"/CONNECT localhost {port} to come back" in line[0], line
        assert sorted(tags()) == sorted(before), tags()

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
        # no window to switch to: the line says so and names /DISCONNECT
        assert wait_log_since(off, "Already connected to Talk, but no window shows it"), log_since(off)
        assert "/DISCONNECT Talk" in log_since(off), log_since(off)
        assert sorted(tags()) == ["E2E", "Talk"], tags()

    def t_classic_layout():
        """the irssi layout (auto_create_separators off, a status window,
        no network windows): closing the last channel window of a network
        that is not the status window's active server keeps it connected -
        the status window shows every server"""
        alice = st["alice"]
        alice.quit()
        assert alice.sanitizer_reports() == [], alice.sanitizer_reports()
        w = work / "classic"
        w.mkdir()
        cfg = harness.config(0, "carol", servers=[("E2E", st["e2e_port"]), ("Talk", st["talk_port"])],
                             channels=("#classic",))
        cfg = cfg.replace('"fe-common/core" = {', '"fe-ansi" = { auto_create_separators = "no"; };\n'
                          '  "fe-common/core" = {', 1)
        assert "auto_create_separators" in cfg, cfg
        c = harness.Erssi(st["inst"], w, "carol", cfg)
        st["alice"] = c
        c.start()
        end = time.time() + 15
        while log_since(0).count("has joined #classic") < 2 and time.time() < end:
            time.sleep(0.2)
        c.type("/alias sl server;echo sl-done")
        got = wait_tags(["E2E", "Talk"])
        assert got == ["E2E", "Talk"], got
        wl = windows()
        assert "(status)" in wl, wl
        status = re.search(r"^\S+ \d+\s+\(status\)\s+(\S+)", wl, re.M)
        assert status, wl
        other = "Talk" if status.group(1) == "E2E" else "E2E"
        chan = [l for l in wl.splitlines() if "#classic" in l and re.search(rf"\s{other}\s", l)]
        assert chan, f"no #classic window of {other}:\n" + wl
        ref = re.search(r"^\S+ (\d+)\s", chan[0]).group(1)
        name = other.lower()
        start = len(trace(name))
        c.type(f"/window {ref}")
        c.type("/window close")  # what the /WC alias of the shipped config runs
        assert wait_trace(name, r" in carol PART #classic", start), trace(name)[start:]
        time.sleep(2)
        assert "QUIT" not in trace(name)[start:], f"{other} was disconnected:\n" + trace(name)[start:]
        assert sorted(tags()) == ["E2E", "Talk"], tags()

    def t_quit():
        a = st["alice"]
        a.quit()
        assert a.sanitizer_reports() == [], a.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_close_disconnects, t_channel_keeps_server,
                      t_reconnect_dropped, t_range_close, t_address_comeback, t_setting_off, t_classic_layout, t_quit])
    finally:
        if "alice" in st:
            st["alice"].kill()
        for name in ("talk", "e2e", "bare"):
            if name in st:
                st[name].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
