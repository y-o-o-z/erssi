#!/usr/bin/env python3
"""The debug logs of the side panels (sidepanel_debug) and of terminal
resizes (resize_debug) are files of the user's own in the erssi home
(--home), made readable by the user only, and both follow /SET at once:
on opens the log, off closes it. Before, the side panel log was the fixed
/tmp/irssi_sidepanels.log (any user of the box could plant a link there
or read the channels and nicks in it) and resize_debug worked only after
a restart. The side panel log has no line for every band of every panel
redraw, and the resize log names a main window by the number of its
active window, not by a pointer cut to an int.
Usage: test_debug_logs.py <build dir> [<source dir>]"""
import re
import shutil
import stat
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-debug-logs."))
    st = {}

    def log_text(path):
        try:
            return path.read_text(errors="replace")
        except OSError:
            return ""

    def private(path):
        mode = stat.S_IMODE(path.lstat().st_mode)
        assert stat.S_ISREG(path.lstat().st_mode) and mode == 0o600, f"{path}: mode {mode:o}"

    def resize_logs():
        return sorted(st["alice"].home.glob("resize-*.log"))

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_start():
        st["ircd"] = harness.Ircd(work)
        port = st["ircd"].start()
        st["port"] = port
        a = harness.Erssi(st["inst"], work, "alice", harness.config(port, "alice"))
        st["alice"] = a
        a.start(120, 40)
        a.wait_ready()
        assert harness.wait_until(lambda: "has joined #test" in log_text(a.all_log()), 15), \
            "alice never joined #test"
        a.type_and_wait("/window goto #test")

    def t_sidepanel_debug_on():
        a = st["alice"]
        path = a.home / "sidepanels.log"
        assert not path.exists(), "a side panel log before sidepanel_debug"
        a.type_and_wait("/set sidepanel_debug on")
        # panel activity: a nick joins and talks, the window changes
        with harness.Client(st["port"], "bob") as bob:
            bob.send("JOIN #test")
            bob.expect("366")
            bob.send("PRIVMSG #test :hello alice")
            a.type_and_wait("/window 1", "/window goto #test")
            assert harness.wait_until(lambda: any("bob" in r for r in a.screen()), WAIT), "bob never in the nick list"
            a.sync_output()
        assert harness.wait_until(lambda: path.exists() and log_text(path), WAIT), \
            f"no side panel log in {a.home}"
        private(path)
        st["sp_log"] = path
        # no trace of every band of every redraw (bob's join and line
        # redrew the nick list)
        text = log_text(path)
        assert "DEBUG draw_right" not in text, "per-redraw traces in the log:\n" + text[-2000:]

    def t_sidepanel_debug_off():
        a = st["alice"]
        path = st["sp_log"]
        a.type_and_wait("/set sidepanel_debug off")
        size = path.stat().st_size
        a.type_and_wait("/window 1", "/window goto #test")
        a.sync_output()
        assert path.stat().st_size == size, "the side panel log still written after off"

    def t_resize_debug_on_without_restart():
        a = st["alice"]
        assert resize_logs() == [], resize_logs()
        a.type_and_wait("/set resize_debug on")
        assert harness.wait_until(lambda: resize_logs(), WAIT), f"no resize log in {a.home}"
        (path,) = resize_logs()
        private(path)
        a.resize(100, 30)
        # a line typed wakes erssi up for the SIGWINCH
        a.type_and_wait()
        assert harness.wait_until(lambda: "RESIZE SEQUENCE" in log_text(path), WAIT), \
            "the resize was not logged:\n" + log_text(path)[-2000:]
        st["rs_log"] = path

    def t_resize_log_names_windows():
        # "... cache for the main window of window N": N is the number of
        # its active window, not a pointer cut to an int
        def named():
            return [int(m) for m in re.findall(r"cache for the main window of window (-?\d+)",
                                               log_text(st["rs_log"]))]
        # logged when the main window is redrawn, after the resize itself
        assert harness.wait_until(named, WAIT), "no main window named:\n" + log_text(st["rs_log"])[-2000:]
        ids = named()
        assert all(1 <= i < 100 for i in ids), f"not window numbers: {sorted(set(ids))}"

    def t_resize_debug_off():
        a = st["alice"]
        path = st["rs_log"]
        a.type_and_wait("/set resize_debug off")
        size = path.stat().st_size
        a.resize(120, 40)
        a.sync_output()
        time.sleep(0.5)
        a.sync_output()
        assert path.stat().st_size == size, "the resize log still written after off"

    def t_sanitizers():
        reports = st["alice"].sanitizer_reports() if "alice" in st else []
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_start, t_sidepanel_debug_on, t_sidepanel_debug_off,
                      t_resize_debug_on_without_restart, t_resize_log_names_windows,
                      t_resize_debug_off, t_sanitizers])
    finally:
        if "alice" in st:
            st["alice"].kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
