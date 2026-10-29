#!/usr/bin/env python3
"""erssi --backup and --restore (shellter-install.sh --backup/--restore,
which erssi runs with its home) against a real erssi: a running erssi
holds a shared lock on <home>/erssi.lock (mode 0600) for as long as it
runs, so a restore onto its home is refused and changes nothing, also
when run by the installer directly; a backup taken while it runs is a
new file of mode 0600 without the lock, and a restore into a new home
gives the same files, which an erssi then starts with. erssi --help
names the options, and --with-logs or --dry-run alone are refused. The
lock survives /upgrade: the new erssi holds it, once.
erssi runs the installer of its own installation (PKGDATADIR): in a
build that is not relocatable that is the compiled prefix, not this
test's install, so those steps run only in a relocatable build.
Usage: test_backup.py <build dir> [<source dir>]"""
import fcntl
import os
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402


def main():
    build = Path(sys.argv[1]).resolve()
    source = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).resolve().parents[2]
    installer = source / "shellter-install.sh"
    work = Path(tempfile.mkdtemp(prefix="live-backup.")).resolve()
    st = {}

    def run(e, *args, timeout=60):
        """erssi with --home of E and ARGS, without a terminal and without
        the harness preparing the home (run_detached would)"""
        e.home_dir.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([str(st["inst"].erssi), f"--home={e.home}", *args], capture_output=True,
                           text=True, env=e.child_env(), cwd=str(e.home_dir), timeout=timeout,
                           stdin=subprocess.DEVNULL)
        return r.returncode, r.stdout + r.stderr

    def run_installer(e, *args):
        r = subprocess.run(["sh", str(installer), *args], capture_output=True, text=True,
                           env=e.child_env(), cwd=str(e.home_dir), timeout=60)
        return r.returncode, r.stdout + r.stderr

    def files(home):
        """path -> (kind, mode, bytes or link target) below HOME"""
        res = {}
        for p in sorted(home.rglob("*")):
            rel = str(p.relative_to(home))
            s = p.lstat()
            if stat.S_ISLNK(s.st_mode):
                res[rel] = ("l", 0, os.readlink(p))
            elif stat.S_ISDIR(s.st_mode):
                res[rel] = ("d", stat.S_IMODE(s.st_mode), None)
            else:
                res[rel] = ("f", stat.S_IMODE(s.st_mode), p.read_bytes())
        return res

    def archived(path):
        """path -> (kind, bytes or link target) of what a backup holds:
        its files and directories, and the links its MANIFEST names"""
        res = {}
        with tarfile.open(path) as t:
            top = t.getmembers()[0].name.split("/")[0]
            for m in t.getmembers():
                if not m.name.startswith(top + "/home/"):
                    continue
                rel = m.name[len(top + "/home/"):].rstrip("/")
                res[rel] = ("d", None) if m.isdir() else ("f", t.extractfile(m).read())
            manifest = t.extractfile(top + "/MANIFEST").read().decode()
        for line in manifest.splitlines():
            f = line.split("\t")
            if f[0] == "l":
                res[f[1]] = ("l", f[2])
        return res

    def relocatable():
        inst = st["inst"]
        r = subprocess.run([str(inst.erssi), "--print-prefix"], capture_output=True, text=True,
                           env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(work)})
        mine = Path(r.stdout.strip()) / "share" / "irssi" / "shellter-install.sh"
        if not mine.is_file() or mine.read_bytes() != installer.read_bytes():
            raise harness.SkipCase("not a relocatable build: erssi runs the installer of its compiled prefix")

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_help():
        a = harness.Erssi(st["inst"], work, "help")
        rc, out = run(a, "--help")
        assert rc == 0, f"--help: exit {rc}: {out}"
        for opt in ("--backup", "--restore=FILE", "--with-logs", "--dry-run"):
            assert opt in out, f"--help does not name {opt}:\n{out}"
        for opt in ("--with-logs", "--dry-run"):
            rc, out = run(a, opt)
            assert rc != 0 and "goes with --backup or --restore" in out, f"{opt} alone: exit {rc}: {out}"

    def t_start():
        a = harness.Erssi(st["inst"], work, "alice", harness.config(1, "alice"), args=("--noconnect",))
        st["alice"] = a
        # a script with its autorun link, as a user has them
        (a.home / "scripts" / "autorun").mkdir(parents=True, exist_ok=True)
        (a.home / "scripts" / "hello.pl").write_text("# hello\n")
        (a.home / "scripts" / "autorun" / "hello.pl").symlink_to("../hello.pl")
        a.start(120, 40)
        a.wait_ready()
        lock = a.home / "erssi.lock"
        assert lock.is_file() and not lock.is_symlink(), "no erssi.lock in the home"
        assert stat.S_IMODE(lock.stat().st_mode) == 0o600, f"erssi.lock mode {stat.S_IMODE(lock.stat().st_mode):o}"
        # what --restore asks for: an exclusive lock, refused while erssi runs
        with open(lock, "r+") as f:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                raise AssertionError("erssi.lock is not locked while erssi runs")
            except BlockingIOError:
                pass

    def t_upgrade_keeps_lock():
        """/UPGRADE runs a new erssi in the same process: the lock is let go
        with the exec (close-on-exec) and taken again by the new one, once"""
        a = st["alice"]
        pid = a.proc.pid
        lock = a.home / "erssi.lock"
        a.type("/upgrade")

        def upgraded():
            r = subprocess.run(["ps", "-p", str(pid), "-o", "args="], capture_output=True, text=True)
            return "--session" in r.stdout
        assert harness.wait_until(upgraded, 20), "erssi did not run again with --session after /upgrade"

        def held():
            with open(lock, "r+") as f:
                try:
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    return False
                except BlockingIOError:
                    return True
        assert harness.wait_until(held, 10), "erssi.lock is not locked after /upgrade"
        fds = Path(f"/proc/{pid}/fd")
        if fds.is_dir():
            ino = lock.stat().st_ino
            n = 0
            for fd in fds.iterdir():
                try:
                    if os.stat(fd).st_ino == ino:
                        n += 1
                except OSError:
                    pass
            assert n == 1, f"erssi holds erssi.lock {n} times after /upgrade (the old descriptor was inherited?)"
        a.wait_ready()

    def t_backup_while_running():
        relocatable()
        a = st["alice"]
        out_file = work / "alice.tar.gz"
        rc, out = run(a, "--backup", str(out_file))
        assert rc == 0, f"--backup: exit {rc}:\n{out}"
        assert "Keep this file private" in out, out
        assert stat.S_IMODE(out_file.stat().st_mode) == 0o600, "the backup is not 0600"
        names = subprocess.run(["tar", "-tzf", str(out_file)], capture_output=True, text=True).stdout
        assert "/home/config" in names, names
        assert "erssi.lock" not in names, "the lock is in the backup"
        st["backup"] = out_file
        rc, out = run(a, "--backup", str(out_file))
        assert rc != 0 and "refusing to overwrite" in out, f"a second --backup to the same file: exit {rc}: {out}"

    def t_restore_refused_while_running():
        a = st["alice"]
        before = files(a.home)
        # a backup of another home, to restore onto alice's
        b = harness.Erssi(st["inst"], work, "bob")
        b.home.mkdir(parents=True)
        (b.home / "config").write_text("settings = { };\n")
        rc, out = run_installer(b, "--backup", str(work / "bob.tar.gz"), "--home", str(b.home))
        assert rc == 0, f"backup of bob: exit {rc}:\n{out}"
        rc, out = run_installer(a, "--restore", str(work / "bob.tar.gz"), "--home", str(a.home))
        assert rc != 0 and "an erssi is running on" in out, f"--restore onto a running erssi: exit {rc}:\n{out}"
        assert "nothing was changed" in out, out
        assert files(a.home).keys() == before.keys(), "the home changed"
        assert not list(work.glob("home-alice/.erssi.before-restore-*")), "the home was moved aside"
        assert not list(work.glob("home-alice/.erssi-restore.*")), "a temporary directory is left"
        if "backup" in st:
            rc, out = run(a, "--restore", str(st["backup"]))
            assert rc != 0 and "an erssi is running on" in out, f"erssi --restore onto itself: exit {rc}:\n{out}"

    def t_restore_new_home():
        relocatable()
        a = st["alice"]
        a.quit()
        c = harness.Erssi(st["inst"], work, "carol", harness.config(1, "carol"), args=("--noconnect",))
        st["carol"] = c
        rc, out = run(c, "--restore", str(st["backup"]), "--dry-run")
        assert rc == 0 and "Dry run" in out and not c.home.exists(), f"--restore --dry-run: exit {rc}:\n{out}"
        rc, out = run(c, "--restore", str(st["backup"]))
        assert rc == 0, f"--restore: exit {rc}:\n{out}"
        want = archived(st["backup"])
        got = files(c.home)
        # the lock the restore held while it swapped the home in
        assert got.pop("erssi.lock", (None,))[0] == "f", "no erssi.lock in the restored home"
        assert sorted(got) == sorted(want), f"other files: {sorted(set(got) ^ set(want))}"
        for k, (kind, mode, data) in got.items():
            assert (kind, data) == want[k], f"{k} differs"
            if kind == "d":
                assert mode == 0o700, f"{k}: mode {mode:o}"
            elif kind == "f":
                assert mode in (0o600, 0o700), f"{k}: mode {mode:o}"
        assert got["scripts/autorun/hello.pl"] == ("l", 0, "../hello.pl"), got["scripts/autorun/hello.pl"]
        for k in ("config", "scripts/hello.pl"):
            assert got[k][2] == (a.home / k).read_bytes(), f"{k} is not alice's"
        # an erssi starts on the restored home. The harness config names
        # alice's home (the all log): carol's from here on, and start()
        # writes no config of its own
        cfg = c.home / "config"
        cfg.write_text(cfg.read_text().replace(str(a.home), str(c.home)))
        c.config_text = None
        c.start(120, 40)
        c.wait_ready()
        c.quit()

    try:
        harness.main([t_install, t_help, t_start, t_upgrade_keeps_lock, t_backup_while_running,
                      t_restore_refused_while_running, t_restore_new_home])
    finally:
        for name in ("alice", "carol"):
            if name in st:
                st[name].kill()
        if not harness.keep_work():
            subprocess.run(["rm", "-rf", str(work)])


if __name__ == "__main__":
    main()
