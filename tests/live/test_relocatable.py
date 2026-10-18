#!/usr/bin/env python3
"""A relocatable build (meson -Drelocatable=true) runs from wherever its
installed tree is: installed under two different temporary directories,
one of them moved and started through the binary installer's links
(bin/erssi -> ../current/bin/erssi, current -> releases/<version>), each
erssi prints its own prefix with --print-prefix, copies the themes and the
startup file from it into a new home, shows /help from it, loads a theme
and a Perl script from it, and maps its modules (e2e, fe-web, perl, irc)
and Irssi.so from it - with no help from the home (no module links, no
theme copy, no perl_use_lib in the config, unlike the other live tests).
A build without the option keeps its compiled paths: --print-prefix and
help_path are the compiled prefix also when the tree is installed
somewhere else.
Usage: test_relocatable.py <build dir> [<source dir>]"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
# the Description of a /help page of the fork (docs/help/in/term_truecolor.in)
HELP_TOPIC = "term_truecolor"
HELP_LINE = "Chooses whether erssi sends 24-bit colours to the terminal."


class HomeOnly(harness.Erssi):
    """An erssi whose home has nothing of the installed tree: what it
    needs of that tree it must find from its own prefix."""

    def _prepare(self):
        for d in ("logs", "scripts/autorun"):
            (self.home / d).mkdir(parents=True, exist_ok=True)
        text = "\n".join(line for line in self.config_text.splitlines() if "perl_use_lib" not in line)
        (self.home / "config").write_text(text.replace("@HOME@", str(self.home)) + "\n")


def build_options(build):
    with open(build / "meson-info" / "intro-buildoptions.json") as f:
        return {o["name"]: o["value"] for o in json.load(f)}


def print_prefix(erssi, work):
    home = Path(tempfile.mkdtemp(prefix="print-prefix.", dir=work))
    r = subprocess.run([str(erssi), "--print-prefix"], capture_output=True, text=True, timeout=30,
                       env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home)})
    assert r.returncode == 0 and r.stderr == "", f"--print-prefix: exit {r.returncode}: {r.stderr}"
    assert list(home.iterdir()) == [], "--print-prefix wrote to the home"
    return r.stdout


def mapped_files(pid):
    """The files mapped into process PID (its shared objects)."""
    maps = Path(f"/proc/{pid}/maps")
    if maps.exists():
        text = maps.read_text()
        return {line.split(None, 5)[5] for line in text.splitlines() if len(line.split(None, 5)) == 6}
    if shutil.which("procstat"):  # FreeBSD
        r = subprocess.run(["procstat", "-v", str(pid)], capture_output=True, text=True)
        return {line.split()[-1] for line in r.stdout.splitlines()[1:] if line.split()[-1].startswith("/")}
    raise harness.SkipCase("neither /proc/<pid>/maps nor procstat")


def main():
    build = Path(sys.argv[1]).resolve()
    opts = build_options(build)
    work = Path(tempfile.mkdtemp(prefix="live-relocatable.")).resolve()
    compiled = Path(opts["prefix"])
    st = {}

    def installed(name):
        """This build installed under work/NAME (meson install --destdir),
        and the prefix in it"""
        (work / name).mkdir()
        inst = harness.install(build, work / name)
        prefix = inst.root / compiled.relative_to("/")
        assert inst.erssi == prefix / "bin" / "erssi", inst.erssi
        return inst, prefix

    def t_install():
        if not opts.get("relocatable"):
            st["plain"] = installed("plain")
            return
        # one where meson put it, one moved into the binary installer's layout
        st["a"] = installed("a")
        inst, prefix = installed("b")
        top = work / "b" / "opt" / "erssi"
        release = top / "releases" / "1.0"
        release.parent.mkdir(parents=True)
        prefix.rename(release)
        (top / "current").symlink_to("releases/1.0")
        (top / "bin").mkdir()
        (top / "bin" / "erssi").symlink_to("../current/bin/erssi")
        moved = harness.Inst(release)
        moved.erssi = top / "bin" / "erssi"
        st["b"] = (moved, release)

    def check_prefix(name):
        if name not in st:
            raise harness.SkipCase("not a relocatable build")
        inst, prefix = st[name]
        out = print_prefix(inst.erssi, work)
        assert out == f"{prefix}\n", f"--print-prefix: {out!r}, expected {prefix}"
        share = prefix / "share" / "irssi"
        w = Path(tempfile.mkdtemp(prefix=f"run-{name}.", dir=work))
        with HomeOnly(inst, w, "alice", harness.config(1, "alice"), args=("--noconnect",)) as e:
            e.start()
            e.wait_ready()
            log = e.all_log()
            # first run: the themes and the startup file from the prefix
            themes = sorted(p.name for p in (share / "themes").glob("*.theme"))
            assert themes, f"no themes installed in {share}"
            for t in themes:
                copy = e.home / t
                assert copy.is_file() and copy.read_bytes() == (share / "themes" / t).read_bytes(), \
                    f"{t} not copied from {share}/themes"
            assert (e.home / "startup").read_bytes() == (share / "startup").read_bytes(), \
                "startup not copied from the prefix"
            # a theme that is not in the home is loaded from the prefix
            (e.home / "colorless.theme").unlink()
            e.type_and_wait("/set theme colorless")
            want = f"Now using theme colorless ({share}/themes/colorless.theme)"
            assert e.wait_file(log, want, WAIT), f"no '{want}'"
            e.type_and_wait("/set help_path")
            assert e.wait_file(log, f"help_path {share}/help", WAIT), "help_path is not in the prefix"
            e.type_and_wait(f"/help {HELP_TOPIC}")
            assert e.wait_file(log, HELP_LINE, WAIT), f"/help {HELP_TOPIC}: no '{HELP_LINE}'"
            # a script of the prefix's scripts directory, with its Irssi.pm
            e.type_and_wait("/script load usercount")
            assert e.wait_file(log, "Loaded script usercount", WAIT), "the Perl script was not loaded"
            maps = mapped_files(e.proc.pid)
            for so in ("libe2e_core.so", "libfe_web.so", "libperl_core.so", "libirc_core.so", "Irssi.so"):
                found = [m for m in maps if m.endswith("/" + so)]
                assert found, f"{so} is not loaded"
                assert all(Path(m).is_relative_to(prefix) for m in found), f"{so} from {found}"
            e.quit()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def t_prefix_a():
        check_prefix("a")

    def t_prefix_b_moved_and_linked():
        check_prefix("b")

    def t_not_relocatable():
        if "plain" not in st:
            raise harness.SkipCase("a relocatable build")
        inst, prefix = st["plain"]
        out = print_prefix(inst.erssi, work)
        assert out == f"{compiled}\n", f"--print-prefix: {out!r}, expected {compiled}"
        w = Path(tempfile.mkdtemp(prefix="run-plain.", dir=work))
        with harness.Erssi(inst, w, "alice", harness.config(1, "alice"), args=("--noconnect",)) as e:
            e.start()
            e.wait_ready()
            e.type_and_wait("/set help_path")
            help_dir = compiled / Path(inst.helpdir).relative_to(prefix)
            assert e.wait_file(e.all_log(), f"help_path {help_dir}", WAIT), \
                f"help_path is not the compiled {help_dir}"
            e.quit()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

    try:
        harness.main([t_install, t_prefix_a, t_prefix_b_moved_and_linked, t_not_relocatable])
    finally:
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
