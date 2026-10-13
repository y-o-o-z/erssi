#!/usr/bin/env python3
"""The first-run setup: asked on a fresh home with a terminal, skipped with
"n", EOF, --noconnect or no terminal, asked again with --setup; ^C and
SIGTERM leave without saving; the default-nick hint on a later start.
No test chooses a network (that would connect to the internet): the network
and SASL config changes are covered by the unit test /firstrun/apply.
Usage: test_firstrun.py <build dir> [<source dir>]"""
import os
import shutil
import signal
import stat
import sys
import tempfile
import termios
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 15
ASK = b"Set up now? [Y/n]"


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-firstrun."))
    st = {}

    def t_install():
        st["inst"] = harness.install(build, work)

    def fresh(name, config_text=None, args=()):
        w = Path(tempfile.mkdtemp(prefix=name + ".", dir=work))
        return harness.Erssi(st["inst"], w, name, config_text, args=args)

    def wait_since(e, mark, pattern, timeout=WAIT):
        end = time.time() + timeout
        while True:
            if pattern in e.output_since(mark):
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def answer(e, prompt, text):
        assert e.wait_output(prompt, WAIT), f"no {prompt!r}:\n{e.text()[-2000:]}"
        e.write_raw(text)

    def finish(e):
        e.quit()
        assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def config_of(e):
        return (e.home / "config").read_text(errors="replace")

    def t_wizard_none():
        with fresh("alice") as e:
            e.start()
            answer(e, ASK, b"\r")
            answer(e, b"Network [", b"0\r")
            answer(e, b"Nick [", b"wizuser\r")
            answer(e, b"Theme [", b"\r")
            assert e.wait_output(b"Saved to", WAIT), e.text()[-2000:]
            assert f"Saved to {e.home}/config.".encode() in e.output(), e.text()[-2000:]
            cfg = e.home / "config"
            assert cfg.is_file(), "no config written"
            mode = stat.S_IMODE(cfg.stat().st_mode)
            assert mode == 0o600, oct(mode)
            # the UI starts after the setup, and /quit ends it
            mark = e.mark()
            e.type("/echo ui-is-up")
            assert wait_since(e, mark, b"ui-is-up"), e.text(mark)[-2000:]
            finish(e)
            assert e.proc.returncode == 0, e.proc.returncode
            text = config_of(e)
            assert 'nick = "wizuser";' in text, text
            assert "autoconnect" not in text, text
            assert stat.S_IMODE(cfg.stat().st_mode) == 0o600

    def t_skip_n():
        with fresh("bob") as e:
            e.start()
            answer(e, ASK, b"n\r")
            assert e.wait_output(b"Skipped.", WAIT), e.text()[-2000:]
            assert b"erssi --setup asks again." in e.output()
            time.sleep(1)
            finish(e)
            assert (e.home / "config").is_file(), "skip did not save the config"
        again = harness.Erssi(st["inst"], e.work, "bob")
        with again:
            again.start()
            assert not again.wait_output(ASK, 5), "asked again after a skip"
            finish(again)

    def t_eof():
        with fresh("carol") as e:
            e.start()
            answer(e, ASK, b"\x04")
            assert e.wait_output(b"Skipped.", WAIT), e.text()[-2000:]
            time.sleep(1)
            finish(e)

    def t_no_tty():
        e = fresh("dave")
        out = e.run_detached(5)
        assert ASK not in out, out[-2000:]
        assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def t_noconnect():
        with fresh("erin", args=("--noconnect",)) as e:
            e.start()
            assert not e.wait_output(ASK, 5), "asked with --noconnect"
            finish(e)

    def t_setup_forced():
        """An existing config and --setup: asked; it only adds, and nothing
        the setup prints reaches the log."""
        cfg = harness.config(1, "frank")
        with fresh("frank", cfg, args=("--setup", "--noconnect")) as e:
            e.start()
            answer(e, ASK, b"\r")
            answer(e, b"Network [", b"0\r")
            answer(e, b"Nick [frank]", b"frank2\r")
            answer(e, b"Theme [", b"\r")
            assert e.wait_output(b"Saved to", WAIT), e.text()[-2000:]
            e.type("/echo ui-is-up")
            assert e.wait_file(e.all_log(), "ui-is-up", WAIT), "no log"
            finish(e)
            text = config_of(e)
            assert 'nick = "frank2";' in text, text
            assert 'autoconnect = "yes"' in text, text  # the earlier server keeps it
            log = e.all_log().read_text(errors="replace")
            for s in ("Set up now", "Nick [", "Saved to", "first-run setup"):
                assert s not in log, f"{s!r} in the log"

    def interrupted(name, send):
        with fresh(name) as e:
            e.start()
            answer(e, ASK, b"\r")
            assert e.wait_output(b"Network [", WAIT), e.text()[-2000:]
            send(e)
            try:
                e.proc.wait(5)
            except Exception:
                raise AssertionError("the setup did not end on the signal")
            attrs = termios.tcgetattr(e.master)
            lflag = attrs[3]
            assert lflag & termios.ECHO and lflag & termios.ICANON and lflag & termios.ISIG, \
                "terminal left in raw/no-echo mode"
            assert not (e.home / "config").exists(), "an interrupted setup saved the config"
            assert e.sanitizer_reports() == [], e.sanitizer_reports()
            return e.proc.returncode

    def t_ctrl_c():
        rc = interrupted("gina", lambda e: e.write_raw(b"\x03"))
        assert rc == -signal.SIGINT, rc

    def t_sigterm():
        rc = interrupted("hank", lambda e: os.kill(e.proc.pid, signal.SIGTERM))
        assert rc == -signal.SIGTERM, rc

    def t_hint():
        cfg = harness.config(1, "erssi_user")
        with fresh("ivan", cfg, args=("--noconnect",)) as e:
            e.start()
            assert e.wait_file(e.all_log(), "Your nick is still erssi_user", WAIT), \
                "no default-nick hint"
            assert ASK not in e.output()
            finish(e)
        cfg = harness.config(1, "judy")
        with fresh("judy", cfg, args=("--noconnect",)) as e:
            e.start()
            e.type("/echo ui-is-up")
            assert e.wait_file(e.all_log(), "ui-is-up", WAIT), "no log"
            finish(e)
            assert "Your nick is still" not in e.all_log().read_text(errors="replace")

    try:
        harness.main([t_install, t_wizard_none, t_skip_n, t_eof, t_no_tty, t_noconnect,
                      t_setup_forced, t_ctrl_c, t_sigterm, t_hint])
    finally:
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
