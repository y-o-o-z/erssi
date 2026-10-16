#!/usr/bin/env python3
"""Tests of the live-test harness: erssi in a pty, a fake IRC server, a raw
client, TESTRAW, a hermetic environment.
Usage: test_harness.py <build dir> [<source dir>]"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

# set in the harness process; none of them may reach erssi
LEAKY = {"TMUX": "/nonexistent/tmux-sock,1,0", "STY": "1.leak", "TERM_PROGRAM": "leakterm",
         "LC_TERMINAL": "leakterm", "IRCNICK": "leaknick", "XDG_RUNTIME_DIR": "/nonexistent",
         "SSH_AUTH_SOCK": "/nonexistent", "KITTY_PID": "1", "WEZTERM_PANE": "1",
         "GHOSTTY_RESOURCES_DIR": "/nonexistent", "LANGUAGE": "xx"}


def environ_of(pid):
    """The NAME=value strings of process pid's environment: /proc on Linux,
    procstat on FreeBSD (no /proc there); SkipCase when neither is there."""
    path = Path(f"/proc/{pid}/environ")
    if path.exists():
        return [x.decode(errors="replace") for x in path.read_bytes().split(b"\0") if x]
    if shutil.which("procstat"):
        r = subprocess.run(["procstat", "--libxo=json", "-e", str(pid)],
                           capture_output=True, text=True, check=True)
        procs = json.loads(r.stdout)["procstat"]["environment"]
        return procs[str(pid)]["environment"]
    raise harness.SkipCase("neither /proc/<pid>/environ nor procstat: the environment of a process cannot be read")


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-harness."))
    inst = None

    def t_install():
        nonlocal inst
        inst = harness.install(build, work)
        assert inst.erssi.is_file() and inst.moddir.is_dir()

    def t_ircd_client():
        (work / "ircd-only").mkdir()
        with harness.Ircd(work / "ircd-only") as ircd:
            port = ircd.start()
            with harness.Client(port, "carol") as c:
                c.send("PING :x")
                c.expect("PONG")
                c.send("FROBNICATE")
                c.expect(" 421 ")

    def t_hermetic_env():
        e = harness.Erssi(inst, work, "iso", harness.config(1, "iso"), env={"TERM": "vt100", "LIVE_MARK": "1"})
        assert e.home.is_relative_to(work), e.home
        saved = {k: os.environ.get(k) for k in LEAKY}
        os.environ.update(LEAKY)
        try:
            env = e.child_env()
            assert Path(env["HOME"]).is_relative_to(work)
            assert not set(env) & set(LEAKY), set(env) & set(LEAKY)
            assert env["TERM"] == "vt100" and env["LIVE_MARK"] == "1"
            # the real thing: the environment of a running erssi
            (work / "leakcheck").mkdir()
            with harness.Erssi(inst, work / "leakcheck", "leak", harness.config(1, "leak")) as live:
                live.start()
                env = environ_of(live.proc.pid)
                names = {x.split("=", 1)[0] for x in env}
                assert not names & set(LEAKY), names & set(LEAKY)
                assert f"HOME={live.home_dir}" in env, env
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def t_smoke():
        w = Path(tempfile.mkdtemp(prefix="smoke.", dir=work))
        with harness.Ircd(w) as ircd, harness.Erssi(inst, w, "alice", None) as alice:
            port = ircd.start()
            alice.config_text = harness.config(port, "alice")
            alice.start()
            with harness.Client(port, "bob") as bob:
                # the join waits are what makes the test deterministic
                assert alice.wait_file(alice.all_log(), "has joined #test", 15), "alice never joined #test"
                bob.send("JOIN #test")
                assert alice.wait_file(alice.all_log(), "bob [u@127.0.0.1] has joined #test", 10), \
                    "alice did not see bob join"
                bob.send("PRIVMSG #test :smoke hello")
                assert alice.wait_file(alice.home / "logs/E2E/#test.log", "smoke hello", 15), \
                    "channel message not logged"
                # /echo does not expand variables, an alias does: the line must
                # hold the name the active window was just given
                alice.type("/window name livewin")
                alice.type("/alias wn echo active=$winname")
                alice.type("/wn")
                assert alice.wait_file(alice.all_log(), "active=livewin", 5), "$winname not expanded"
                bob.send("TESTRAW alice ::e2e.test NOTICE alice :raw works")
                assert alice.wait_file(alice.all_log(), "raw works", 5), "TESTRAW line not delivered"
            alice.quit()
            assert alice.sanitizer_reports() == [], alice.sanitizer_reports()

    def t_screen():
        # the terminal text of an output stream, no erssi needed
        s = harness.Screen(10, 4)
        s.feed(b"\x1b[?1049h\x1b[H\x1b[2Jtop\x1b[2;3H\x1b[31mab\xe2\x94")
        s.feed(b"\x82c\x1b[0m\x1b]9;osc\x07\x1b[4;1Hlast")  # a character and an OSC cut
        assert s.lines() == ["top       ", "  ab\u2502c    ", "          ", "last      "], s.lines()
        s.feed(b"\x1b[2;4H\x1b[K\x1b[1;4r\x1b[4;1H\n")  # clear to the end, scroll up
        assert s.lines() == ["  a       ", "          ", "last      ", "          "], s.lines()
        s.feed(b"\x1b[1;1H\x1bMx\x1b[r\x1b[3;9Hwide\xe5\xad\x97")  # scroll down, wrap
        # a wide character takes two cells, the second one empty
        assert s.lines() == ["x         ", "  a       ", "        wi", "de\u5b57      "], s.lines()
        assert s.lines(2, 5) == ["   ", "a  ", "   ", "\u5b57 "], s.lines(2, 5)
        s.feed(b"\x1b[r\x1b[1;1H\x1b[2J")
        assert s.lines() == [" " * 10] * 4, s.lines()
        # the style each character was written in
        s.feed(b"\x1b[1;1Ha\x1b[1;38;5;3mb\x1b[22mc\x1b[0;7md\x1b[me\x1b[1;6H\x1b[K")
        assert s.lines()[0] == "abcde     ", s.lines()
        assert s.styles()[0][:6] == [(), (("bold", True), ("fg", "38;5;3")), (("fg", "38;5;3"),),
                                     (("reverse", True),), (), ()], s.styles()[0]
        # a cursor saved before the screen got smaller comes back inside it
        s.feed(b"\x1b[2J\x1b[4;10H\x1b7\x1b[1;1H")
        s.resize(5, 2)
        s.feed(b"\x1b8z")
        assert s.lines() == ["     ", "    z"], s.lines()

    def t_skip_case():
        # a case that raises SkipCase is reported as SKIP, the others still run
        r = subprocess.run([sys.executable, "-c",
                            "import sys; sys.path.insert(0, %r); import harness\n"
                            "def t_skipped(): raise harness.SkipCase('no such feature')\n"
                            "def t_after(): pass\n"
                            "harness.main([t_after, t_skipped, t_after])" % str(Path(harness.__file__).parent)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r
        assert "  SKIP t_skipped: no such feature" in r.stdout, r.stdout
        assert "  ok   t_skipped" not in r.stdout, r.stdout
        assert "result: 2 ok, 0 failed, 1 skipped" in r.stdout, r.stdout

    try:
        harness.main([t_install, t_ircd_client, t_hermetic_env, t_smoke, t_screen, t_skip_case])
    finally:
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
