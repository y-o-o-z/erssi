#!/usr/bin/env python3
"""Smoke test of the live-test harness: erssi in a pty, a fake IRC server,
a raw client, TESTRAW. Usage: test_harness.py <build dir> <source dir>"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402


def run(build, work):
    inst = harness.install(build, work)
    ircd = harness.Ircd(work)
    port = ircd.start()
    alice = harness.Erssi(inst, work, "alice", harness.config(port, "alice"))
    bob = None
    try:
        alice.start()
        bob = harness.Client(port, "bob")
        assert alice.wait_file(alice.all_log(), "has joined #test", 30), "alice never joined #test"
        bob.send("JOIN #test")
        assert alice.wait_file(alice.all_log(), "bob [u@127.0.0.1] has joined #test", 10), \
            "alice did not see bob join"
        bob.send("PRIVMSG #test :smoke hello")
        assert alice.wait_file(alice.home / "logs/E2E/#test.log", "smoke hello", 15), \
            "channel message not logged"
        alice.type("/echo active=$winname")
        assert alice.wait_file(alice.all_log(), "active=", 5), "/echo not logged"
        bob.send("TESTRAW alice ::e2e.test NOTICE alice :raw works")
        assert alice.wait_file(alice.all_log(), "raw works", 5), "TESTRAW line not delivered"
        alice.quit()
        assert alice.sanitizer_reports() == [], alice.sanitizer_reports()
    finally:
        if bob:
            bob.close()
        alice.kill()
        ircd.stop()


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-harness."))

    def t_install():
        inst = harness.install(build, work)
        assert inst.erssi.is_file() and inst.moddir.is_dir()

    def t_ircd_client():
        ircd = harness.Ircd(work)
        try:
            port = ircd.start()
            c = harness.Client(port, "carol")
            c.send("PING :x")
            c.expect("PONG")
            c.close()
        finally:
            ircd.stop()

    def t_home_isolated():
        inst = harness.install(build, work)
        e = harness.Erssi(inst, work, "iso", harness.config(1, "iso"))
        assert e.home.is_relative_to(work), e.home
        assert Path(e.child_env()["HOME"]).is_relative_to(work)

    def t_smoke():
        w = Path(tempfile.mkdtemp(prefix="smoke.", dir=work))
        run(build, w)

    try:
        harness.main([t_install, t_ircd_client, t_home_isolated, t_smoke])
    finally:
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
