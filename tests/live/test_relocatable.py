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
With --tree DIR the same checks run on an installed tree that is already
there, unpacked from a binary package (tests/version/test-build-binary.sh).
Usage: test_relocatable.py <build dir> [<source dir>]
       test_relocatable.py --tree DIR"""
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
        # without the harness's perl_use_lib (a line of a test's own stays)
        text = "\n".join(line for line in self.config_text.splitlines() if "@PERLLIB@" not in line)
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
    tree = None
    if sys.argv[1] == "--tree":
        tree = Path(sys.argv[2]).resolve()
        opts = {"relocatable": True, "prefix": "/"}
    else:
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
            check_set_default(e, ("help_path", "perl_use_lib"), prefix)
            e.quit()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def check_set_default(e, keys, prefix):
        """/SET -default writes no default into the config: the key goes,
        so a default under this release's prefix is never left behind"""
        cfg = e.save_config(*(f"/set {k} /nonexistent/{k}" for k in keys))
        assert all(f"/nonexistent/{k}" in cfg for k in keys), "the values were not saved"
        cfg = e.save_config(*(f"/set -default {k}" for k in keys))
        for k in keys:
            assert k not in cfg, f"/set -default {k} saved it:\n{cfg}"
        if prefix is not None:
            assert str(prefix) not in cfg, f"the prefix in the config:\n{cfg}"

    def t_prefix_a():
        check_prefix("a")

    def t_prefix_b_moved_and_linked():
        check_prefix("b")

    def t_stale_release_paths():
        """help_path and perl_use_lib saved by another release of the
        installer's layout (<p>/releases/0.9/...) are taken as the
        default of this one (releases/1.0), with one warning line each,
        and are gone from the next /SAVE; /help and a Perl script work"""
        if "b" not in st:
            raise harness.SkipCase("not a relocatable build")
        inst, prefix = st["b"]
        old = prefix.parent / "0.9"
        perl_rel = Path(inst.perllib).relative_to(prefix)
        cfg = harness.config(1, "alice").replace(
            'theme = "default";', f'theme = "default"; help_path = "{old}/share/irssi/help";').replace(
            'perl_use_lib = "@PERLLIB@";', f'perl_use_lib = "{old / perl_rel}";')
        assert str(old) in cfg and "@PERLLIB@" not in cfg
        w = Path(tempfile.mkdtemp(prefix="run-stale.", dir=work))
        with HomeOnly(inst, w, "alice", cfg, args=("--noconnect",)) as e:
            e.start()
            e.wait_ready()
            log = e.all_log()
            # the warnings are printed at no message level (not logged):
            # read on the screen. perl_use_lib's at the start, help_path's
            # at the first /help (a short one, so both stay on the screen)
            e.type_and_wait("/help nosuchtopic_x")
            e.sync_output()

            def warned(k):
                return [ln for ln in e.screen() if f"{k} pointed into another release ({old}/" in ln]
            for k in ("help_path", "perl_use_lib"):
                assert harness.wait_until(lambda: warned(k), WAIT), \
                    f"no warning for {k}:\n" + "\n".join(e.screen())
                assert len(warned(k)) == 1, f"{k}: " + "\n".join(warned(k))
            e.type_and_wait(f"/help {HELP_TOPIC}")
            assert e.wait_file(log, HELP_LINE, WAIT), f"/help {HELP_TOPIC} with a stale help_path"
            e.type_and_wait("/script load usercount")
            assert e.wait_file(log, "Loaded script usercount", WAIT), "no Perl script with a stale perl_use_lib"
            # now the defaults of this release
            e.type_and_wait("/set help_path", "/set perl_use_lib")
            assert e.wait_file(log, f"help_path {prefix}/share/irssi/help", WAIT), "help_path is not the default"
            assert e.wait_file(log, f"perl_use_lib {inst.perllib}", WAIT), "perl_use_lib is not the default"
            saved = e.save_config()
            assert str(old) not in saved, f"the old release in the config:\n{saved}"
            e.quit()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

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
            # (its perl_use_lib is the harness's, under this install)
            check_set_default(e, ("help_path",), None)
            e.quit()
            assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def t_unpacked_tree():
        st["tree"] = (harness.Inst(tree), tree)
        check_prefix("tree")

    try:
        if tree is not None:
            harness.main([t_unpacked_tree])
        else:
            harness.main([t_install, t_prefix_a, t_prefix_b_moved_and_linked, t_stale_release_paths,
                          t_not_relocatable])
    finally:
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
