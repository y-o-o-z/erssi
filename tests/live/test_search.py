#!/usr/bin/env python3
"""/SEARCH over the autolog files: the scopes (the window's item, -target,
-network, -all), -max, -regexp and -case, remote text with '%' and '$', the
search running in a child process while erssi goes on, search_max_size,
one search at a time, -stop and /HELP SEARCH.
Usage: test_search.py <build dir> [<source dir>]"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 30  # a deadline for what comes at once: a loaded machine is slow
BIG_MB = 100


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-search."))
    st = {}

    def log_size():
        try:
            return st["alice"].all_log().stat().st_size
        except OSError:
            return 0

    def log_since(offset):
        return st["alice"].all_log().read_bytes()[offset:].decode(errors="replace")

    def wait_log_since(offset, text, timeout=WAIT):
        return bool(harness.wait_until(lambda: text in log_since(offset), timeout))

    def search(args, done="Search for "):
        """Runs /search ARGS; returns the log text it added once DONE is in it."""
        off = log_size()
        st["alice"].type_and_wait("/search " + args)
        assert wait_log_since(off, done), f"/search {args}: no {done!r}:\n" + log_since(off)
        st["alice"].type_and_wait()  # all of what came with DONE is written
        return log_since(off)

    def processes():
        """pid -> (parent pid, state) of every process, from ps (the same on
        Linux and the BSDs; FreeBSD has no /proc), or None when ps failed
        (not known: no answer either way)."""
        if shutil.which("ps") is None:
            raise harness.SkipCase("no ps: the child processes of erssi cannot be listed")
        try:
            r = subprocess.run(["ps", "-A", "-o", "pid=", "-o", "ppid=", "-o", "stat="],
                               capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            return None
        if r.returncode != 0:
            return None
        table = {}
        for line in r.stdout.splitlines():
            f = line.split()
            if len(f) >= 3:
                table[f[0]] = (f[1], f[2])
        return table

    def children():
        """The pids of alice's erssi's child processes, or None (not known)."""
        pid = str(st["alice"].proc.pid)
        table = processes()
        if table is None:
            return None
        return [k for k, (ppid, _) in table.items() if ppid == pid]

    def no_children(timeout=WAIT):
        end = time.time() + timeout
        while True:
            kids = children()
            if kids == []:
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def wait_children(timeout=WAIT):
        """The child processes once there is one (the search started)."""
        end = time.time() + timeout
        while True:
            kids = children()
            if kids:
                return kids
            if time.time() >= end:
                return []
            time.sleep(0.05)

    def gone(pid, timeout):
        """pid has ended (no process, or a zombie of another parent)."""
        end = time.time() + timeout
        while True:
            table = processes()
            if table is not None and (pid not in table or table[pid][1].startswith("Z")):
                return True
            if time.time() >= end:
                return False
            time.sleep(0.05)

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_start():
        ircd = harness.Ircd(work)
        st["ircd"] = ircd
        port = ircd.start()
        alice = harness.Erssi(st["inst"], work, "alice", harness.config(port, "alice"))
        st["alice"] = alice
        alice.start()
        assert alice.wait_file(alice.all_log(), "has joined #test", 15), "alice never joined #test"
        st["bob"] = bob = harness.Client(port, "bob")
        bob.send("JOIN #test")
        assert alice.wait_file(alice.all_log(), "bob [u@127.0.0.1] has joined #test", WAIT), \
            "alice did not see bob join"
        for text in ("needle one", "needle two", "100%needle $0 %K", "other"):
            bob.send(f"PRIVMSG #test :{text}")
        chan_log = alice.home / "logs" / "E2E" / "#test.log"
        assert alice.wait_file(chan_log, "other", WAIT), "bob's lines are not in the autolog"
        st["chan_log"] = chan_log
        alice.type_and_wait("/window goto #test")

    def t_basic():
        out = search("needle")
        for text in ("needle one", "needle two", "100%needle $0 %K"):
            assert text in out, f"{text!r} is not in the hits:\n" + out
        assert "Search for needle: 3 hits" in out, out
        # the label is the target from the file name
        assert re.search(r"#test .*\d+:\d+\s+\S*bob\S* needle one", out), out

    def t_window():
        # the results go to the window that ran the command, not to the
        # one that is active when they arrive
        alice = st["alice"]
        home = alice.home
        alice.type_and_wait("/window new")
        alice.type_and_wait("/window name srch")
        alice.type_and_wait(f"/window log on {home}/win-srch.log")
        alice.type_and_wait("/window goto #test")
        alice.type_and_wait(f"/window log on {home}/win-test.log")
        off = log_size()
        alice.write_raw(b"/window goto srch\r/search -target #test needle two\r/window goto #test\r")
        assert wait_log_since(off, "Search for needle two: 1 hits"), log_since(off)
        alice.type_and_wait()  # the window logs are written with it
        srch = (home / "win-srch.log").read_text(errors="replace")
        test = (home / "win-test.log").read_text(errors="replace") \
            if (home / "win-test.log").exists() else ""
        assert "Search for needle two: 1 hits" in srch, srch
        assert "Search for needle two" not in test, test
        alice.type_and_wait("/window goto srch")
        alice.type_and_wait("/window log off")
        alice.type_and_wait("/window close")
        alice.type_and_wait("/window goto #test")
        st["win_test_log"] = home / "win-test.log"

    def t_bad_scope():
        # glob characters, '/' and . or .. are not a network tag or a target
        for args in ("-network * needle", "-network ../x needle", "-network . needle",
                     "-target .. needle", "-all -target . needle"):
            out = search(args, done="SEARCH:")
            assert "SEARCH: not a network tag or target name" in out, f"{args}: {out}"
            assert "Search for" not in out, f"{args}: {out}"

    def t_max():
        out = search("-max 1 needle")
        assert "Search for needle: 1 hits" in out, out
        assert "100%needle" in out, out
        assert "needle one" not in out and "needle two" not in out, out
        # at most 1000
        out = search("-max 5000 needle")
        assert "Search for needle: 3 hits" in out, out
        out = search("-max 0 needle", done="SEARCH:")
        assert "SEARCH: -max needs a positive number" in out, out

    def t_regexp_case():
        # the log line: the time, the nick (as the theme writes it) and the text
        out = search(r"-regexp -case ^\d+:\d+\s+\S*bob\S* needle t")
        assert re.search(r"Search for .*needle t: 1 hits", out), out
        assert "needle two" in out, out
        out = search("-case NEEDLE")
        assert "Search for NEEDLE: 0 hits" in out, out
        out = search("NEEDLE")
        assert "Search for NEEDLE: 3 hits" in out, out
        # a bad regular expression: an error, nothing is started
        off = log_size()
        st["alice"].type_and_wait("/search -regexp needle(")
        assert wait_log_since(off, "SEARCH:"), log_since(off)
        assert "Search for" not in log_since(off), log_since(off)

    def t_all_network():
        out = search("-all needle")
        assert "Search for needle: 3 hits" in out, out
        # the label says the network when several are searched
        assert "E2E/#test" in out, out
        out = search("-network E2E needle")
        assert "Search for needle: 3 hits" in out, out
        out = search("-network e2e needle")
        assert "Search for needle: 3 hits" in out, out
        out = search("-target #nosuch needle")
        assert "Search for needle: 0 hits in 0 files" in out, out

    def t_no_item():
        alice = st["alice"]
        alice.type_and_wait("/window goto Notices")
        try:
            out = search("needle", done="SEARCH:")
            assert "SEARCH: no channel or query here - use -target, -network or -all" in out, out
            assert "Search for" not in out, out
        finally:
            alice.type_and_wait("/window goto #test")

    def t_stop_idle():
        out = search("-stop", done="Search: nothing is running")
        assert "Search for" not in out, out

    def t_async_and_limit():
        big = st["alice"].home / "logs" / "E2E" / "#big.log"
        st["big"] = big
        # the "y" at the end makes -regexp (x+x+)+y slow on every line
        line = b"12:00 <bob> filler " + b"x" * 60 + b" y\n"
        chunk = line * 10000
        with open(big, "wb") as f:
            f.write(b"--- Log opened Mon Oct 12 12:00:00 2026\n")
            while f.tell() < BIG_MB * 1024 * 1024:
                f.write(chunk)
            f.write(b"12:01 <bob> bigneedle\n")
        alice = st["alice"]
        alice.type_and_wait("/set search_max_size 10M")
        off = log_size()
        # both lines in one read: /echo runs while the search goes on
        alice.write_raw(b"/search -target #big bigneedle\r/echo alive\r")
        assert wait_log_since(off, "Search for bigneedle", 60), log_since(off)
        alice.type_and_wait()
        out = log_since(off)
        assert "alive" in out, out
        assert out.index("alive") < out.index("Search for bigneedle"), out
        summary = [x for x in out.splitlines() if "Search for bigneedle" in x][0]
        assert "1 hits in 1 files" in summary, summary
        assert summary.rstrip().endswith("stopped at search_max_size"), summary
        assert "bigneedle" in out.split("Search for bigneedle")[0], out
        assert no_children(), children()

    def t_busy():
        alice = st["alice"]
        alice.type_and_wait("/set search_max_size 1G")
        # a search that runs for minutes: a backtracking pattern on every line
        slow = b"/search -target #big -regexp (x+x+)+y\r"
        off = log_size()
        alice.write_raw(slow)
        kids = wait_children()
        assert kids, "no search process"
        time.sleep(1.2)  # it runs for longer than that: a time is what is checked
        assert children() == kids, f"the search ended by itself: {children()} {log_since(off)}"
        alice.write_raw(b"/search x\r")
        assert wait_log_since(off, "Search: one is running - /SEARCH -stop first"), log_since(off)
        alice.write_raw(b"/search -stop\r")
        assert wait_log_since(off, "Search stopped"), log_since(off)
        assert all(gone(k, WAIT) for k in kids), f"-stop did not end the search process {kids}"
        assert no_children(WAIT), f"the search process is still there: {children()}"
        # -stop in the same read as the start: the SIGTERM comes right
        # after fork(), before the child has set up its signals
        for _ in range(3):
            off = log_size()
            alice.write_raw(slow + b"/search x\r/search -stop\r")
            assert wait_log_since(off, "Search stopped"), log_since(off)
            out = log_since(off)
            assert "Search: one is running - /SEARCH -stop first" in out, out
            assert out.index("one is running") < out.index("Search stopped"), out
            assert no_children(WAIT), f"the search process is still there: {children()}"
        # a negative check: no late summary of a stopped search (one would
        # come from a child that is gone already) - a short bounded wait
        time.sleep(1)
        assert "Search for" not in log_since(off), log_since(off)
        # a new one runs to the end; the window it was started in is closed
        # meanwhile, so the results go to the active window
        off = log_size()
        alice.write_raw(b"/window new\r/search -max 1 -target #big bigneedle\r/window close\r")
        assert wait_log_since(off, "Search for bigneedle: 1 hits in 1 files", 60), log_since(off)
        alice.type_and_wait()
        out = log_since(off)
        assert "stopped at" not in out, out
        assert alice.proc.poll() is None, "erssi is gone"
        assert no_children(), children()
        test = st["win_test_log"].read_text(errors="replace")
        assert "Search for bigneedle: 1 hits in 1 files" in test, "not in the active window:\n" + test

    def t_child_killed():
        # the search process dies without a summary: the failure says why
        alice = st["alice"]
        off = log_size()
        alice.write_raw(b"/search -target #big -regexp (x+x+)+y\r")
        kids = wait_children()
        assert kids, "no search process"
        time.sleep(0.5)  # the child past its start: it dies while it searches
        for k in kids:
            os.kill(int(k), signal.SIGKILL)
        assert wait_log_since(off, "failed: the search process ended without a result"), \
            log_since(off)
        assert "time limit" not in log_since(off), log_since(off)
        assert no_children(), children()

    def t_help():
        alice = st["alice"]
        alice.type_and_wait(f"/set help_path {st['inst'].helpdir}")
        off = log_size()
        alice.type_and_wait("/help search")
        assert wait_log_since(off, "Searches the autolog files"), log_since(off)
        out = log_since(off)
        assert "SEARCH -stop" in out, out

    def t_quit_running():
        # /quit while a search runs: the child gets SIGTERM and ends
        alice = st["alice"]
        alice.write_raw(b"/search -target #big -regexp (x+x+)+y\r")
        kids = wait_children()
        assert kids, "no search process"
        time.sleep(0.5)  # the child past its start: /quit comes while it searches
        alice.quit()
        assert all(gone(k, WAIT) for k in kids), f"the search process outlived erssi: {kids}"
        st["big"].unlink()
        assert alice.sanitizer_reports() == [], alice.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_basic, t_window, t_bad_scope, t_max, t_regexp_case,
                      t_all_network, t_no_item, t_stop_idle, t_async_and_limit, t_busy, t_child_killed, t_help,
                      t_quit_running])
    finally:
        if "bob" in st:
            st["bob"].close()
        if "alice" in st:
            st["alice"].kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
