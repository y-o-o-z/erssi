#!/usr/bin/env python3
"""Terminal notifications (term_notify): focus reporting, OSC 9 / OSC 777
for mentions while the terminal is not focused, the throttle, remote text
that tries to break out of the sequence, tmux passthrough, an unknown
terminal, /SET term_notify off and term_notify_text.
Usage: test_notify.py <build dir> [<source dir>]"""
import os
import re
import shutil
import signal
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
FOCUS_ON = b"\x1b[?1004h"
FOCUS_OFF = b"\x1b[?1004l"
FOCUS_IN = b"\x1b[I"
FOCUS_OUT = b"\x1b[O"
OSC9 = b"\x1b]9;"
OSC777 = b"\x1b]777;"
THROTTLE = 3.1


SEQ = re.compile(rb"\x1bPtmux;.*?\x1b\\|\x1b\](?:9|777);[^\x07]*\x07|\x1b\[\?1004[hl]", re.S)


def seqs(out):
    """The notification and focus mode sequences in out (for messages)."""
    return SEQ.findall(out)


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-notify."))
    st = {"instances": []}

    def wait_since(e, mark, pattern, timeout=WAIT):
        end = time.time() + timeout
        while True:
            if pattern in e.output_since(mark):
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def start(name, env):
        """An erssi with term_notify auto in the environment env (on top of
        the harness's allowlist: TERM=xterm-256color, no TMUX/STY/...)."""
        cfg = harness.config(st["port"], name)
        cfg = cfg.replace("settings = {", 'settings = {\n  "fe-ansi" = { term_notify = "auto"; };', 1)
        assert 'term_notify = "auto"' in cfg, cfg
        e = harness.Erssi(st["inst"], work, name, cfg, env=env)
        st["instances"].append(e)
        e.start()
        assert e.wait_file(e.all_log(), "has joined #test", 15), f"{name} never joined #test"
        return e

    def pm(e, text):
        """bob sends a PM to e; returns once e has it in its log."""
        st["bob"].send(f"PRIVMSG {e.name} :{text}")
        shown = text.split("\x1b")[0]
        assert e.wait_file(e.all_log(), shown, WAIT), f"{e.name} did not get {text!r}"

    def log_size(e):
        try:
            return e.all_log().stat().st_size
        except OSError:
            return 0

    def log_since(e, offset):
        return e.all_log().read_bytes()[offset:].decode(errors="replace")

    def bind_pgup(e):
        """PgUp (the key "prior"; ESC [ I is one of its sequences) prints a line."""
        e.type("/bind prior command echo pgup key pressed")
        time.sleep(0.5)

    def focus(e, data):
        e.write_raw(data)
        time.sleep(0.5)  # read by erssi before the PM that follows arrives

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_start():
        ircd = harness.Ircd(work)
        st["ircd"] = ircd
        st["port"] = ircd.start()
        st["bob"] = harness.Client(st["port"], "bob")

    def t_wezterm():
        alice = start("alice", {"TERM_PROGRAM": "WezTerm"})
        st["alice"] = alice
        assert alice.wait_output(FOCUS_ON, WAIT), "focus reporting was not switched on"
        # focused (the state when reporting starts): nothing is sent
        m = alice.mark()
        pm(alice, "hi while focused")
        time.sleep(2)
        assert OSC777 not in alice.output_since(m), seqs(alice.output_since(m))
        # not focused: the notification
        focus(alice, FOCUS_OUT)
        m = alice.mark()
        pm(alice, "hello there")
        want = b"\x1b]777;notify;erssi;PM bob: hello there\x07"
        assert wait_since(alice, m, want), seqs(alice.output_since(m))
        # focused again (and past the throttle): nothing. The reports are
        # not keys: ESC [ I is not PgUp, and ESC [ O would start a key
        # sequence and eat the "/"
        bind_pgup(alice)
        off = log_size(alice)
        focus(alice, FOCUS_IN)
        alice.type("/echo input after focus reports")
        assert alice.wait_file(alice.all_log(), "input after focus reports", WAIT), \
            "the focus reports reached the input line"
        assert "pgup key pressed" not in log_since(alice, off), "ESC [ I was taken as PgUp"
        time.sleep(THROTTLE)
        m = alice.mark()
        pm(alice, "back")
        time.sleep(2)
        assert OSC777 not in alice.output_since(m), seqs(alice.output_since(m))

    def t_throttle():
        alice = st["alice"]
        focus(alice, FOCUS_OUT)
        time.sleep(THROTTLE)
        m = alice.mark()
        pm(alice, "first of two")
        time.sleep(0.5)
        pm(alice, "second of two")
        time.sleep(1)
        out = alice.output_since(m)
        assert out.count(OSC777) == 1, seqs(out)
        assert b"\x1b]777;notify;erssi;PM bob: first of two\x07" in out, seqs(out)

    def t_injection():
        # remote text with its own OSC 52 (clipboard write): ESC and BEL are
        # dropped and ';' becomes ',' inside the OSC 777 fields
        alice = st["alice"]
        time.sleep(THROTTLE)
        m = alice.mark()
        pm(alice, "evil\x1b]52;c;Zm9v\x07end")
        want = b"\x1b]777;notify;erssi;PM bob: evil]52,c,Zm9vend\x07"
        assert wait_since(alice, m, want), seqs(alice.output_since(m))
        assert b"\x1b]52" not in alice.output(), "an OSC 52 reached the terminal"

    def t_text_off():
        alice = st["alice"]
        alice.type("/set term_notify_text off")
        time.sleep(THROTTLE)
        m = alice.mark()
        pm(alice, "secret words")
        want = b"\x1b]777;notify;erssi;PM bob\x07"
        assert wait_since(alice, m, want), seqs(alice.output_since(m))
        out = alice.output_since(m)
        assert out.count(OSC777) == 1, seqs(out)
        assert b"secret" not in out.split(OSC777, 1)[1].split(b"\x07", 1)[0], seqs(out)
        alice.type("/set term_notify_text on")

    def t_suspend():
        # ^Z (term_stop) switches reporting off for the shell and on again
        # after the resume, with the terminal focused. erssi leads its own
        # session here, so the SIGTSTP of an orphaned process group is
        # discarded and it goes on at once.
        alice = st["alice"]
        focus(alice, FOCUS_OUT)
        m = alice.mark()
        alice.write_raw(b"\x1a")
        resumed = wait_since(alice, m, FOCUS_ON)
        if not resumed:
            os.kill(alice.proc.pid, signal.SIGCONT)  # it did stop after all
        out = alice.output_since(m)
        assert resumed and FOCUS_OFF in out, seqs(out)
        assert out.index(FOCUS_OFF) < out.rindex(FOCUS_ON), seqs(out)
        time.sleep(THROTTLE)
        m = alice.mark()
        pm(alice, "after resume")
        time.sleep(2)
        assert OSC777 not in alice.output_since(m), seqs(alice.output_since(m))

    def t_off():
        alice = st["alice"]
        m = alice.mark()
        alice.type("/set term_notify off")
        assert wait_since(alice, m, FOCUS_OFF), "focus reporting was not switched off"
        # on again: reporting comes back, the terminal counts as focused
        m = alice.mark()
        alice.type("/set term_notify auto")
        assert wait_since(alice, m, FOCUS_ON), "focus reporting was not switched on again"
        time.sleep(THROTTLE)
        m = alice.mark()
        pm(alice, "focused after auto")
        time.sleep(2)
        assert OSC777 not in alice.output_since(m), seqs(alice.output_since(m))
        # another setting does not send the mode again
        m = alice.mark()
        alice.type("/set term_notify_text on")
        time.sleep(1)
        assert seqs(alice.output_since(m)) == [], seqs(alice.output_since(m))
        m = alice.mark()
        alice.type("/set term_notify off")
        assert wait_since(alice, m, FOCUS_OFF), "focus reporting was not switched off"
        # off: a focus report is an ordinary key sequence now, nothing is sent
        focus(alice, FOCUS_OUT)
        m = alice.mark()
        pm(alice, "while off")
        time.sleep(2)
        out = alice.output_since(m)
        assert OSC777 not in out and OSC9 not in out, seqs(out)
        # ESC [ O was a key sequence (meta2 waiting for its key): Enter ends
        # it, and the typed line arrives whole. On again for t_sanitizers.
        alice.write_raw(b"\r")
        time.sleep(0.5)
        m = alice.mark()
        alice.type("/set term_notify auto")
        assert wait_since(alice, m, FOCUS_ON), "focus reporting was not switched on again"

    def t_tmux_iterm():
        e = start("alicet", {"TMUX": "/tmp/x,1,0", "TERM_PROGRAM": "tmux", "LC_TERMINAL": "iTerm2"})
        assert e.wait_output(FOCUS_ON, WAIT), "focus reporting was not switched on"
        focus(e, FOCUS_OUT)
        m = e.mark()
        pm(e, "hi")
        want = b"\x1bPtmux;\x1b\x1b]9;erssi: PM bob: hi\x07\x1b\\"
        assert wait_since(e, m, want), seqs(e.output_since(m))

    def t_unknown_terminal():
        e = start("aliceu", {"TERM": "xterm-256color"})
        time.sleep(5)
        assert FOCUS_ON not in e.output(), "focus reporting switched on for an unknown terminal"
        # ESC [ I goes to the key handling as before: it is PgUp, not a
        # focus report (t_wezterm has the opposite with reporting on)
        bind_pgup(e)
        off = log_size(e)
        e.write_raw(FOCUS_IN)
        time.sleep(0.5)
        e.type("/echo after pgup")
        assert e.wait_file(e.all_log(), "after pgup", WAIT), "/echo after ESC [ I not shown"
        assert "pgup key pressed" in log_since(e, off), "ESC [ I is not PgUp:\n" + log_since(e, off)
        assert e.proc.poll() is None, "erssi is gone"
        focus(e, FOCUS_OUT)
        m = e.mark()
        pm(e, "unknown terminal")
        time.sleep(2)
        out = e.output_since(m)
        assert OSC9 not in out and OSC777 not in out, seqs(out)
        assert e.proc.poll() is None, "erssi is gone"

    def t_fresh_default():
        # without a config file the built-in default config (irssi.conf,
        # term_notify auto) is used
        fresh = harness.Erssi(st["inst"], work, "fresh", None, args=("--noconnect",),
                              env={"TERM_PROGRAM": "WezTerm"})
        st["instances"].append(fresh)
        fresh.start()
        time.sleep(3)
        m = fresh.mark()
        fresh.type("/set term_notify")
        end = time.time() + WAIT
        while time.time() < end and "term_notify " not in fresh.text(m) and "term_notify =" not in fresh.text(m):
            time.sleep(0.1)
        out = fresh.text(m)
        raw = fresh.output()
        fresh.kill()
        assert re.search(r"term_notify\s+(= )?auto", out), "term_notify is not auto by default:\n" + out
        assert FOCUS_ON in raw, "no focus reporting with the default config"

    def t_sanitizers():
        # /quit switches focus reporting off: the shell must not get the
        # reports afterwards
        reports = []
        left_on = []
        for e in st["instances"]:
            if e.proc is None or e.proc.poll() is not None:
                reports += e.sanitizer_reports()
                continue
            e.quit()
            modes = [x for x in seqs(e.output()) if x in (FOCUS_ON, FOCUS_OFF)]
            if modes and modes[-1] != FOCUS_OFF:
                left_on.append(e.name)
            reports += e.sanitizer_reports()
        assert left_on == [], "?1004h left on after /quit: " + ", ".join(left_on)
        assert reports == [], reports

    try:
        harness.main([t_install, t_start, t_wezterm, t_throttle, t_injection, t_text_off, t_suspend, t_off,
                      t_tmux_iterm, t_unknown_terminal, t_fresh_default, t_sanitizers])
    finally:
        if "bob" in st:
            st["bob"].close()
        for e in st["instances"]:
            e.kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
