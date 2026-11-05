#!/usr/bin/env python3
"""/UPGRADE with a space in the home (and so in the config's path): the
new erssi gets each path as one argument and runs in the same process.
The command line was once one string split at every space, so the exec
failed after the session was saved and the servers let go - every
connection lost.
Usage: test_upgrade_paths.py <build dir>"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-upgrade-paths.")).resolve()
    st = {}

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_space_in_home():
        e = harness.Erssi(st["inst"], work, "with space", harness.config(1, "spacey"),
                          args=("--noconnect",))
        st["e"] = e
        assert " " in str(e.home), e.home
        e.start(120, 40)
        e.wait_ready()
        pid = e.proc.pid
        e.upgrade()
        assert e.proc.pid == pid and e.proc.poll() is None, "erssi is gone after /upgrade"
        e.type_and_wait("/echo after the upgrade")
        assert "after the upgrade" in e.all_log().read_text(errors="replace")
        e.quit()
        assert e.sanitizer_reports() == [], e.sanitizer_reports()

    try:
        harness.main([t_install, t_space_in_home])
    finally:
        if "e" in st:
            st["e"].kill()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
