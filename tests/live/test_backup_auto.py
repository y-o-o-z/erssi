#!/usr/bin/env python3
"""Automatic backups (backup_auto, backup_dir, backup_keep, /BACKUP) with the
real installer of this source tree (shellter-install.sh --backup, named by
ERSSI_TEST_UPDATE_INSTALLER) and a real erssi:

- /BACKUP NOW writes backup_dir/erssi-auto-YYYYMMDD-HHMMSS.tar.gz, a tar
  of the home of mode 0600 with the config in it, says so in one line, and
  keeps the newest backup_keep automatic backups: older ones of that name
  go, a file put there by hand and a backup of another name stay;
- backup_auto daily with the last backup two days old: the backup runs by
  itself about a minute after the start, once, and the state file
  (mode 0600) has it; after /UPGRADE no other one runs;
- backup_dir inside the home: turning backup_auto on says why it cannot
  be used, and nothing is written.
Usage: test_backup_auto.py <build dir> [<source dir>]"""
import re
import shutil
import stat
import sys
import tarfile
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 30
NAME = re.compile(r"^erssi-auto-\d{8}-\d{6}\.tar\.gz$")


def main():
    build = Path(sys.argv[1]).resolve()
    source = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).resolve().parents[2]
    installer = source / "shellter-install.sh"
    work = Path(tempfile.mkdtemp(prefix="live-backup-auto.")).resolve()
    st = {}

    def log_text(e):
        try:
            return e.all_log().read_text(errors="replace")
        except OSError:
            return ""

    def wait_log(e, text, timeout=WAIT):
        return bool(harness.wait_until(lambda: text in log_text(e), timeout))

    def autos(d):
        return sorted(p.name for p in d.iterdir() if NAME.match(p.name))

    def erssi(name, settings):
        cfg = harness.config(st["port"], name)
        cfg = cfg.replace('"fe-common/core" = { ', '"fe-common/core" = { ' + settings + ' ')
        return harness.Erssi(st["inst"], work, name, cfg,
                             env={"ERSSI_TEST_UPDATE_INSTALLER": str(installer)})

    def finish(e):
        e.quit()
        assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def t_install():
        st["inst"] = harness.install(build, work)
        st["ircd"] = harness.Ircd(work)
        st["port"] = st["ircd"].start()

    def t_now_and_rotation():
        bk = work / "bk-now"
        bk.mkdir()
        for n in ("erssi-auto-20200101-000000.tar.gz", "erssi-auto-20200102-000000.tar.gz",
                  "erssi-auto-20200103-000000.tar.gz", "erssi-backup-20200101-000000.tar.gz",
                  "mine.tar.gz"):
            (bk / n).write_text("old\n")
        e = erssi("alice", f'backup_dir = "{bk}"; backup_keep = "2";')
        e.start(160, 40)
        e.wait_ready()
        e.type_and_wait("/backup now")
        assert wait_log(e, ".tar.gz ("), log_text(e)[-2000:]
        assert "Backup failed" not in log_text(e), log_text(e)[-2000:]
        names = autos(bk)
        assert len(names) == 2 and names[0] == "erssi-auto-20200103-000000.tar.gz", names
        new = bk / names[1]
        assert f"Backup: {new} (" in log_text(e), log_text(e)[-2000:]
        assert stat.S_IMODE(new.stat().st_mode) == 0o600, oct(new.stat().st_mode)
        with tarfile.open(new) as t:
            members = t.getnames()
        assert any(m.endswith("/home/config") for m in members), members
        assert (bk / "mine.tar.gz").read_text() == "old\n"
        assert (bk / "erssi-backup-20200101-000000.tar.gz").exists()
        # /BACKUP: off, and the one just made
        e.type_and_wait("/backup")
        assert wait_log(e, "Backup: automatic backups are off"), log_text(e)[-2000:]
        assert wait_log(e, f": {new}"), log_text(e)[-2000:]
        finish(e)

    def t_scheduled():
        bk = work / "bk-auto"
        e = erssi("bob", f'backup_dir = "{bk}"; backup_auto = "daily"; backup_keep = "3";')
        e.home.mkdir(parents=True, exist_ok=True)
        old = int(time.time()) - 2 * 24 * 3600
        (e.home / "backup-auto.state").write_text(f"last_success={old}\nlast_attempt={old}\n")
        (e.home / "backup-auto.state").chmod(0o600)
        e.start(160, 40)
        e.wait_ready()
        t0 = time.time()
        assert harness.wait_until(lambda: bk.is_dir() and autos(bk), 90), "no backup in 90 s"
        assert time.time() - t0 > 40, f"backed up after {time.time() - t0:.0f} s, not about a minute"
        assert wait_log(e, ".tar.gz ("), log_text(e)[-2000:]
        assert stat.S_IMODE(bk.stat().st_mode) == 0o700, oct(bk.stat().st_mode)
        state = e.home / "backup-auto.state"
        assert stat.S_IMODE(state.lstat().st_mode) == 0o600
        text = state.read_text()
        assert f"last_file={bk}/" in text and "failures=0" in text, text
        # /UPGRADE: no other backup within the day
        e.type("/upgrade")
        time.sleep(3)
        e.type_and_wait("/echo after-upgrade")
        time.sleep(70)
        e.sync_output()
        assert len(autos(bk)) == 1, autos(bk)
        assert log_text(e).count(".tar.gz (") == 1, log_text(e)[-3000:]
        finish(e)

    def t_inside_home():
        e = erssi("carol", 'backup_dir = "~/.erssi/backups";')
        e.start(160, 40)
        e.wait_ready()
        e.type_and_wait("/set backup_auto daily")
        assert wait_log(e, "is inside the erssi home"), log_text(e)[-2000:]
        e.type_and_wait("/backup now")
        assert wait_log(e, "Backup failed: backup_dir ~/.erssi/backups is inside"), log_text(e)[-2000:]
        assert not (e.home / "backups").exists()
        finish(e)

    try:
        harness.main([t_install, t_now_and_rotation, t_scheduled, t_inside_home])
    finally:
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
