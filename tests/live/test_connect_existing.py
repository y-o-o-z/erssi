#!/usr/bin/env python3
"""/CONNECT <network> while a server of that network is connected,
connecting or waiting to reconnect opens no second connection: it makes
the server's window active and says so. /CONNECT <address> still opens a
second connection. Fake servers: Talk, and Slow (registration delayed).
Usage: test_connect_existing.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
NOTICE = "/CONNECT with a server address"
SLOW = 6  # seconds the Slow server takes to answer USER


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-connect-existing."))
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

    def registrations(name):
        """the USER lines erssi sent to that server: one per connection"""
        return len(re.findall(r" in \S+ USER alice ", trace(name)))

    def servers():
        off = log_size()
        st["alice"].type("/sl")
        assert wait_log_since(off, "sl-done"), "no /SERVER output:\n" + log_since(off)
        return log_since(off).split("sl-done")[0]

    def tags():
        return sorted(re.findall(r"^\S+ (\S+): 127\.0\.0\.1:", servers(), re.M))

    def wait_tags(want, timeout=WAIT):
        end = time.time() + timeout
        while True:
            got = tags()
            if got == sorted(want) or time.time() >= end:
                return got
            time.sleep(0.3)

    def active():
        off = log_size()
        st["alice"].type("/wn")
        assert wait_log_since(off, "active=["), "no /wn output"
        return re.findall(r"active=\[([^\]]*)\]", log_since(off))[-1]

    def connect_again(name):
        """/CONNECT NAME from the Notices window; returns the output mark"""
        a = st["alice"]
        a.type("/window goto Notices")
        assert active() == "Notices"
        off = log_size()
        mark = a.mark()
        a.type(f"/connect {name}")
        assert wait_log_since(off, NOTICE), "no notice:\n" + log_since(off)
        line = [x for x in log_since(off).splitlines() if NOTICE in x]
        assert name in line[0], line
        assert a.wait_output(NOTICE.encode(), WAIT), "notice not on the screen:\n" + a.text(mark)[-2000:]
        return mark

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_start():
        for name, extra in (("talk", []), ("slow", ["--register-delay", str(SLOW)])):
            (work / name).mkdir()
            st[name] = harness.Ircd(work / name, extra)
            st[name + "_port"] = st[name].start()
        cfg = harness.config(0, "alice", servers=[("Talk", st["talk_port"]), ("Slow", st["slow_port"])],
                             channels=())
        cfg = cfg.replace(f'chatnet = "Slow"; port = "{st["slow_port"]}"; autoconnect = "yes";',
                          f'chatnet = "Slow"; port = "{st["slow_port"]}"; autoconnect = "no";')
        assert 'autoconnect = "no"' in cfg, cfg
        a = harness.Erssi(st["inst"], work, "alice", cfg)
        st["alice"] = a
        a.start()
        a.type("/alias sl server;echo sl-done")
        a.type("/alias wn echo active=[$winname]")
        got = wait_tags(["Talk"], 15)
        assert got == ["Talk"], got

    def t_connected():
        """Talk is connected: /CONNECT Talk makes its window active, says
        so, and connects nothing"""
        connect_again("Talk")
        assert active() == "Talk", active()
        time.sleep(2)
        assert tags() == ["Talk"], tags()
        assert registrations("talk") == 1, trace("talk")

    def t_connecting():
        """Slow is still registering: a second /CONNECT Slow waits for it"""
        a = st["alice"]
        a.type("/connect Slow")
        assert st["alice"].wait_file(a.all_log(), "Waiting for CAP LS response", WAIT)
        time.sleep(0.5)
        connect_again("Slow")
        got = wait_tags(["Slow", "Talk"], SLOW + WAIT)
        assert got == ["Slow", "Talk"], got
        time.sleep(1)
        assert registrations("slow") == 1, trace("slow")
        assert "Slow2" not in servers()

    def t_reconnect_waiting():
        """Talk lost its link and waits to reconnect (5 minutes): /CONNECT
        Talk does not open another connection"""
        a = st["alice"]
        before = registrations("talk")
        with harness.Client(st["talk_port"], "dropper") as c:
            c.send("TESTDROP alice")
            assert a.wait_file(a.all_log(), "Connection lost to 127.0.0.1", WAIT), "the link did not drop"
        assert re.search(r"RECON-\d+: 127\.0\.0\.1:\d+ \(Talk\)", servers()), servers()
        connect_again("Talk")
        assert active() == "Talk", active()  # its window, by its own name
        time.sleep(2)
        assert registrations("talk") == before, trace("talk")
        # with the reconnect removed, /CONNECT Talk connects again as Talk
        a.type("/rmreconns")
        a.type("/connect Talk")
        got = wait_tags(["Slow", "Talk"])
        assert got == ["Slow", "Talk"], got

    def t_address_second_connection():
        """/CONNECT <address> <port> opens a second connection, as before"""
        a = st["alice"]
        before = registrations("talk")
        a.type(f"/connect 127.0.0.1 {st['talk_port']} - alice2")
        end = time.time() + WAIT
        while registrations("talk") == before and time.time() < end:
            time.sleep(0.2)
        assert registrations("talk") == before + 1, trace("talk")
        end = time.time() + WAIT
        while len(tags()) != 3 and time.time() < end:
            time.sleep(0.3)
        assert len(tags()) == 3, tags()

    def t_quit():
        a = st["alice"]
        a.quit()
        assert a.sanitizer_reports() == [], a.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_connected, t_connecting, t_reconnect_waiting,
                      t_address_second_connection, t_quit])
    finally:
        if "alice" in st:
            st["alice"].kill()
        for name in ("talk", "slow"):
            if name in st:
                st[name].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
