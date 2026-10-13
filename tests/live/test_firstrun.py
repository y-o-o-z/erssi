#!/usr/bin/env python3
"""The first-run setup: asked on a fresh home with a terminal, skipped with
"n", EOF, --noconnect, TERM=dumb or no terminal on stdin or stdout, asked
again with --setup (which needs a terminal); ^C (also at the password
prompt) and SIGTERM leave without saving; the default-nick hint on a later
start. No test chooses a network of the default config (that would connect
to the internet): the network and SASL config changes are covered by the
unit test /firstrun/apply. Only t_ctrl_c_password chooses a network, a
127.0.0.1 one, with --noconnect, and quits at the password prompt.
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
        _, out = e.run_detached(5)
        assert ASK not in out, out[-2000:]
        assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def t_stdout_pipe():
        """stdin a terminal, stdout a pipe: not asked"""
        e = fresh("dora")
        _, out = e.run_detached(5, stdin_tty=True)
        assert ASK not in out, out[-2000:]
        assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def t_setup_no_tty():
        """--setup without a terminal: one line on stderr, exit status 1"""
        e = fresh("dina", args=("--setup",))
        rc, out = e.run_detached(5)
        assert rc == 1, (rc, out[-2000:])
        assert out == b"erssi: --setup needs a terminal\n", out[-2000:]
        assert not (e.home / "config").exists()

    def t_term_dumb():
        e = harness.Erssi(st["inst"], Path(tempfile.mkdtemp(prefix="dumb.", dir=work)), "dumb",
                          env={"TERM": "dumb"})
        with e:
            e.start()
            assert not e.wait_output(ASK, 5), "asked with TERM=dumb"
            e.kill()
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

    def lflag(e):
        """the local modes of erssi's terminal (the pty master shares them)"""
        return termios.tcgetattr(e.master)[3]

    def cooked(e):
        f = lflag(e)
        return bool(f & termios.ECHO and f & termios.ICANON and f & termios.ISIG)

    def ended(e):
        try:
            e.proc.wait(5)
        except Exception:
            raise AssertionError("the setup did not end on the signal")

    def interrupted(name, send):
        with fresh(name) as e:
            e.start()
            answer(e, ASK, b"\r")
            assert e.wait_output(b"Network [", WAIT), e.text()[-2000:]
            send(e)
            ended(e)
            assert cooked(e), "terminal left in raw/no-echo mode"
            assert not (e.home / "config").exists(), "an interrupted setup saved the config"
            assert e.sanitizer_reports() == [], e.sanitizer_reports()
            return e.proc.returncode

    def t_ctrl_c():
        rc = interrupted("gina", lambda e: e.write_raw(b"\x03"))
        assert rc == -signal.SIGINT, rc

    def t_sigterm():
        rc = interrupted("hank", lambda e: os.kill(e.proc.pid, signal.SIGTERM))
        assert rc == -signal.SIGTERM, rc

    def t_ctrl_c_password():
        """^C at the SASL password prompt (no echo, ISIG off) quits without
        saving and gives the terminal back. The network is 127.0.0.1 with
        verified TLS; --noconnect and the ^C keep erssi from connecting."""
        port = harness.free_port()
        cfg = (f'servers = ( {{ address = "127.0.0.1"; chatnet = "Local"; port = "{port}"; '
               f'use_tls = "yes"; tls_verify = "yes"; }} );\n'
               'chatnets = { Local = { type = "IRC"; }; };\n'
               'settings = { core = { nick = "kim"; user_name = "kim"; }; };\n')
        with fresh("kim", cfg, args=("--setup", "--noconnect")) as e:
            e.start()
            path = e.home / "config"
            assert e.wait_output(ASK, WAIT), e.text()[-2000:]
            # taken at the first question: the start may migrate the config before
            before = (path.read_bytes(), path.stat().st_mtime_ns)
            answer(e, ASK, b"\r")
            answer(e, b"Network [", b"1\r")
            answer(e, b"Nick [kim]", b"\r")
            answer(e, b"SASL account on Local", b"alice\r")
            assert e.wait_output(b"SASL password: ", WAIT), e.text()[-2000:]
            # the check below can fail: here the terminal is raw and silent
            end = time.time() + WAIT
            while cooked(e) and time.time() < end:
                time.sleep(0.05)
            assert not lflag(e) & termios.ECHO, "no echo-off at the password prompt"
            # ^Z: erssi's process group is orphaned here (its own session), so the
            # stop is discarded and the read starts again at once, with the prompt
            e.write_raw(b"x\x1a")
            end = time.time() + WAIT
            while e.output().count(b"SASL password: ") < 2 and time.time() < end:
                time.sleep(0.05)
            assert e.output().count(b"SASL password: ") == 2, e.text()[-2000:]
            while cooked(e) and time.time() < end:
                time.sleep(0.05)
            e.write_raw(b"hun\x03")
            ended(e)
            assert e.proc.returncode == -signal.SIGINT, e.proc.returncode
            assert cooked(e), "terminal left in raw/no-echo mode"
            assert (path.read_bytes(), path.stat().st_mtime_ns) == before, "config changed"
            typed = e.output().split(b"SASL password: ", 1)[1]
            assert b"hun" not in typed and b"x" not in typed, "the password was echoed"
            assert b"Skipped" not in e.output() and b"Saved to" not in e.output()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

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
        harness.main([t_install, t_wizard_none, t_skip_n, t_eof, t_no_tty, t_stdout_pipe,
                      t_setup_no_tty, t_term_dumb, t_noconnect, t_setup_forced, t_ctrl_c,
                      t_sigterm, t_ctrl_c_password, t_hint])
    finally:
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
