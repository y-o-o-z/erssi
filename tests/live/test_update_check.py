#!/usr/bin/env python3
"""The update notice (update_check, /UPDATE) against a stand-in installer
(ERSSI_TEST_UPDATE_INSTALLER) that answers as shellter-install.sh
--update --check --porcelain does and logs every run:

- /UPDATE CHECK: the line "erssi X is available (signed). Update from a
  shell: ..." and the "update" statusbar item; the state file is the
  user's own, mode 0600; an error and "current" are said too;
- update_check on: the first check about a minute after the start, the
  line once per version, the statusbar item at once from the state
  file at the next start, and no check again after /UPGRADE within the
  day;
- an installer that is not there: turning the setting on says why, and
  /UPDATE CHECK runs nothing.
Usage: test_update_check.py <build dir> [<source dir>]"""
import shutil
import stat
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 15
AVAILABLE = "erssi 2099.1.0 is available (signed). Update from a shell: erssi --update, then /upgrade"


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-update-check."))
    st = {}

    def installer(name):
        """a stand-in installer in work/<name>: answers with the line in
        <name>.answer and the code in <name>.code, logs each run to
        <name>.runs"""
        d = work / name
        d.mkdir()
        script = d / "shellter-install.sh"
        script.write_text("#!/bin/sh\n"
                          f'echo "$*" >> "{d}/runs"\n'
                          f'cat "{d}/answer"\n'
                          f'exit "$(cat "{d}/code")"\n')
        answer(d, "update 2026.11.0 2099.1.0", 0)
        return d

    def answer(d, line, code):
        (d / "answer").write_text(line + "\n")
        (d / "code").write_text(f"{code}\n")

    def runs(d):
        try:
            return (d / "runs").read_text().splitlines()
        except OSError:
            return []

    def log_text(e):
        try:
            return e.all_log().read_text(errors="replace")
        except OSError:
            return ""

    def wait_log(e, text, timeout=WAIT):
        return bool(harness.wait_until(lambda: text in log_text(e), timeout))

    def on_screen(e, text, timeout=WAIT):
        return e.wait_screen(lambda rows: any(text in r for r in rows), timeout)[1]

    def erssi(name, d, extra_settings=""):
        cfg = harness.config(st["port"], name)
        if extra_settings:
            cfg = cfg.replace('"fe-common/core" = { ', '"fe-common/core" = { ' + extra_settings + ' ')
        return harness.Erssi(st["inst"], work, name, cfg,
                             env={"ERSSI_TEST_UPDATE_INSTALLER": str(d / "shellter-install.sh")})

    def finish(e):
        e.quit()
        assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def t_install():
        st["inst"] = harness.install(build, work)
        st["ircd"] = harness.Ircd(work)
        st["port"] = st["ircd"].start()

    def t_check_command():
        d = installer("manual")
        e = erssi("alice", d)
        st["alice"] = e
        e.start(160, 40)
        e.wait_ready()
        e.type_and_wait("/statusbar additem -after lag update info")
        assert not any("update 2099" in r for r in e.screen()), "the item shows before a check"
        e.type_and_wait("/update check")
        assert wait_log(e, AVAILABLE), log_text(e)[-2000:]
        assert on_screen(e, "update 2099.1.0"), "\n".join(e.screen())
        # the line is in the Notices window, erssi's status window
        e.type_and_wait("/window goto Notices")
        assert on_screen(e, "erssi 2099.1.0 is available (signed). Update from a shell:"), \
            "\n".join(e.screen())
        e.type_and_wait("/window goto #test")
        assert len(runs(d)) == 1 and runs(d)[0].startswith("--update --check --porcelain --prefix /"), runs(d)
        state = e.home / "update-check.state"
        mode = stat.S_IMODE(state.lstat().st_mode)
        assert stat.S_ISREG(state.lstat().st_mode) and mode == 0o600, oct(mode)
        text = state.read_text()
        assert "notified=2099.1.0" in text and "available=2099.1.0" in text, text
        # /UPDATE: the setting is off, and what the last check found
        e.type_and_wait("/update")
        assert wait_log(e, "Update check is off: /SET update_check on checks once a day"), log_text(e)[-2000:]
        # an error is said when asked for
        answer(d, "error cannot reach https://github.com/y-o-o-z/erssi.git", 1)
        e.type_and_wait("/update check")
        assert wait_log(e, "Update check failed: cannot reach https://github.com/y-o-o-z/erssi.git"), \
            log_text(e)[-2000:]
        e.type_and_wait("/update")
        assert wait_log(e, "failed: cannot reach https://github.com/y-o-o-z/erssi.git (1 in a row)"), \
            log_text(e)[-2000:]
        # a failure is no debug line in the window
        assert "GLib" not in log_text(e), log_text(e)[-2000:]
        # garbage is an error, not an update
        answer(d, "Update available: 2026.11.0 -> 2099.2.0", 0)
        e.type_and_wait("/update check")
        assert wait_log(e, "Update check failed: unexpected answer from the installer (exit code 0)"), \
            log_text(e)[-2000:]
        assert "2099.2.0 is available" not in log_text(e)
        # up to date: the item goes
        answer(d, "current 2099.1.0", 0)
        e.type_and_wait("/update check")
        assert wait_log(e, "erssi 2099.1.0 is the newest signed release"), log_text(e)[-2000:]
        assert e.wait_screen(lambda rows: not any("update 2099" in r for r in rows), WAIT)[1], \
            "\n".join(e.screen())
        finish(e)

    def t_daily():
        """the first check a minute after the start, once per version;
        after /UPGRADE no new check within the day"""
        d = installer("daily")
        e = erssi("bob", d, 'update_check = "yes";')
        e.start(160, 40)
        e.wait_ready()
        e.type_and_wait("/statusbar additem -after lag update info")
        t0 = time.time()
        assert harness.wait_until(lambda: runs(d), 90), "no check in 90 s"
        assert time.time() - t0 > 40, f"checked after {time.time() - t0:.0f} s, not about a minute"
        assert wait_log(e, AVAILABLE), log_text(e)[-2000:]
        assert on_screen(e, "update 2099.1.0"), "\n".join(e.screen())
        assert log_text(e).count("2099.1.0 is available") == 1
        # /UPGRADE: the item is back at once, from the state file; no new
        # check before the day is over
        e.type("/upgrade")
        time.sleep(3)
        e.type_and_wait("/echo after-upgrade")
        assert on_screen(e, "update 2099.1.0"), "\n".join(e.screen())
        time.sleep(75)
        e.sync_output()
        assert len(runs(d)) == 1, runs(d)
        assert log_text(e).count("2099.1.0 is available") == 1, log_text(e)[-3000:]
        finish(e)

    def t_no_installer():
        d = work / "none"
        d.mkdir()
        e = erssi("carol", d)
        e.start(160, 40)
        e.wait_ready()
        e.type_and_wait("/set update_check on")
        assert wait_log(e, "Update check: this erssi was not installed with shellter-install.sh"), \
            log_text(e)[-2000:]
        e.type_and_wait("/update check")
        assert log_text(e).count("not installed with shellter-install.sh") == 2, log_text(e)[-2000:]
        assert runs(d) == []
        finish(e)

    try:
        harness.main([t_install, t_check_command, t_daily, t_no_installer])
    finally:
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
