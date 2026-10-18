#!/usr/bin/env python3
"""A "gui dialog" emitted while erssi starts (a module of autoload_modules
warns while it loads, before "irssi init finished") is printed once.
core queues such texts and sends them again when the start is done; the
front end, which already listened, printed them at once as well, so each
one was shown twice. The proxy module's two warnings (no password, no
ports) are the example.
Usage: test_early_dialog.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
WARNINGS = ("Password not specified, everyone can use this proxy!",
            "No proxy ports specified.")


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-early-dialog."))
    st = {}

    def t_install():
        st["inst"] = harness.install(build, work)
        if not list(st["inst"].moddir.glob("libirc_proxy.so")):
            raise harness.Skip("built without the proxy module (-Dwith-proxy=yes)")

    def t_once():
        cfg = harness.config(1, "alice").replace("core = { ", 'core = { autoload_modules = "irc perl proxy"; ', 1)
        assert "autoload_modules" in cfg
        w = Path(tempfile.mkdtemp(prefix="once.", dir=work))
        with harness.Erssi(st["inst"], w, "alice", cfg, args=("--noconnect",)) as e:
            e.start()
            e.wait_ready()
            e.sync_output()
            for text in WARNINGS:
                assert harness.wait_until(lambda: any(text in ln for ln in e.screen()), WAIT), \
                    f"no '{text}':\n" + "\n".join(e.screen())
                lines = [ln for ln in e.screen() if text in ln]
                assert len(lines) == 1, f"'{text}' {len(lines)} times:\n" + "\n".join(e.screen())
            e.quit()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

    try:
        harness.main([t_install, t_once])
    finally:
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
