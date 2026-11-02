#!/usr/bin/env python3
"""The disk space warning (disk_warn): a few seconds after the start erssi
says in the Notices window how little room is left for its home, of the
file system or of the user's quota, and nothing when there is enough or
the warning is off. ERSSI_TEST_DISK_LEFT stands in for what is left.
Usage: test_disk_space.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 30  # the first look is 5 s after the start; a loaded machine is slow


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-disk-space."))
    st = {"all": []}

    def started(name, left, warn=None):
        cfg = harness.config(1, name)
        if warn is not None:
            old = 'theme = "default";'
            assert old in cfg, cfg
            cfg = cfg.replace(old, f'{old} disk_warn = "{warn}";')
        e = harness.Erssi(st["inst"], work, name, cfg,
                          args=("--noconnect",), env={"ERSSI_TEST_DISK_LEFT": left})
        st["all"].append(e)
        e.start(120, 40)
        e.wait_ready()
        return e

    def log(e):
        """the all log; GLib's sizes have a no-break space before the unit"""
        try:
            return e.all_log().read_text(errors="replace").replace("\u00a0", " ")
        except OSError:
            return ""

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_low_disk():
        e = started("disk", "1048576")
        assert harness.wait_until(lambda: "Disk: only 1.0 MB left on the disk of" in log(e), WAIT), log(e)[-2000:]
        assert str(e.home) in log(e), "the home is not named"
        e.quit()

    def t_low_quota():
        e = started("quota", "300000:quota")
        assert harness.wait_until(lambda: "Disk: only 300.0 kB left of your disk quota" in log(e), WAIT), \
            log(e)[-2000:]
        e.quit()

    def t_enough_or_off():
        # plenty left, and the warning off: nothing said in the same time
        a = started("plenty", "1073741824")
        b = started("off", "1024", warn="0")
        time.sleep(8)
        for e in (a, b):
            assert "Disk: only" not in log(e), (e.name, log(e)[-2000:])
            e.quit()

    def t_sanitizers():
        reports = [p for e in st["all"] for p in e.sanitizer_reports()]
        assert reports == [], reports

    try:
        harness.main([t_install, t_low_disk, t_low_quota, t_enough_or_off, t_sanitizers])
    finally:
        for e in st["all"]:
            e.kill()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
