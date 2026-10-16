#!/usr/bin/env python3
"""mentions.pl users move to the built-in Mentions window: the script is
unloaded with a notice, its settings move, the stub 3.0.0 does the same.
Usage: test_mentions_migrate.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
NOTICE = "mentions.pl is no longer needed"


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-mentions-migrate."))
    st = {}

    def log_text():
        try:
            return st["alice"].all_log().read_bytes().decode(errors="replace")
        except OSError:
            return ""

    def wait_log(pattern, timeout=WAIT, count=1):
        return bool(harness.wait_until(lambda: len(re.findall(pattern, log_text())) >= count, timeout))

    def file_text(path):
        try:
            return Path(path).read_bytes().decode(errors="replace")
        except OSError:
            return ""

    def t_install():
        inst = harness.install(build, work)
        if not inst.has_perl:
            raise harness.Skip("erssi was built without Perl")
        st["inst"] = inst

    def t_start():
        ircd = harness.Ircd(work)
        st["ircd"] = ircd
        port = ircd.start()
        st["port"] = port
        st["old_log"] = work / "old.log"
        cfg = harness.config(port, "alice")
        marker = '"perl/core" = { perl_use_lib'
        assert marker in cfg, cfg
        cfg = cfg.replace(marker, f'"perl/core/scripts" = {{ mentions_log_file = "{st["old_log"]}"; }};\n  ' + marker)
        alice = harness.Erssi(st["inst"], work, "alice", cfg)
        st["alice"] = alice
        autorun = alice.home / "scripts" / "autorun"
        autorun.mkdir(parents=True)
        fixture = Path(__file__).resolve().parent / "fixtures" / "mentions.pl"
        assert fixture.is_file(), fixture
        (autorun / "mentions.pl").symlink_to(fixture)
        alice.start()
        assert alice.wait_file(alice.all_log(), "has joined #test", 15), "alice never joined #test"
        alice.type_and_wait("/alias wn echo active=$winname")
        st["bob"] = bob = harness.Client(port, "bob")
        bob.send("JOIN #test")
        assert alice.wait_file(alice.all_log(), "bob [u@127.0.0.1] has joined #test", WAIT)

    def t_saved_at_once():
        """The migrated values are in the file without /save or a quit, so
        /RELOAD and settings_autosave OFF keep them."""
        text = file_text(st["alice"].home / "config")
        m = re.search(r'"fe-common/irc"\s*=\s*\{([^}]*)\}', text)
        assert m is not None and "old.log" in m.group(1) and re.search(r'mentions_log\s*=\s*"yes"', m.group(1)), text
        m = re.search(r'"perl/core/scripts"\s*=\s*\{([^}]*)\}', text)
        assert m is None or "mentions_log_file" not in m.group(1), text

    def t_message():
        assert wait_log(re.escape(NOTICE)), "no migration notice:\n" + log_text()
        assert "<irssi dir>/scripts/autorun" in log_text()

    def t_unloaded():
        assert wait_log(r"Unloaded script mentions"), "the script was not unloaded:\n" + log_text()
        alice = st["alice"]
        alice.type_and_wait("/mentions")
        alice.type("/wn")
        assert wait_log(r"active=\S+"), log_text()
        assert re.findall(r"active=(\S*)", log_text())[-1] == "Mentions", log_text()
        assert "OLD SCRIPT RAN" not in log_text()

    def t_settings_moved():
        alice = st["alice"]
        st["bob"].send("PRIVMSG alice :moved hello")
        assert alice.wait_file(st["old_log"], "<bob>", WAIT), "no entry in the log file of the script"
        assert "moved hello" in file_text(st["old_log"])
        alice.type_and_wait("/set mentions_log")
        assert wait_log(r"mentions_log\s+(= )?ON"), "mentions_log is not ON:\n" + log_text()
        alice.type_and_wait("/save")
        cfg = alice.home / "config"
        assert alice.wait_file(cfg, "mentions_log_file", WAIT)
        text = file_text(cfg)
        m = re.search(r'"perl/core/scripts"\s*=\s*\{([^}]*)\}', text)
        assert m is None or "mentions_log_file" not in m.group(1), text
        m = re.search(r'"fe-common/irc"\s*=\s*\{([^}]*)\}', text)
        assert m is not None and "old.log" in m.group(1), text

    def t_no_duplicates():
        st["bob"].send("PRIVMSG alice :only once")
        assert st["alice"].wait_file(st["old_log"], "only once", WAIT)
        # a second entry would be written while the line is handled: done
        # once a channel line bob sends after it is in the all log
        st["bob"].send("PRIVMSG #test :sync after only once")
        assert st["alice"].wait_file(st["alice"].all_log(), "sync after only once", WAIT)
        assert file_text(st["old_log"]).count("only once") == 1, file_text(st["old_log"])

    def t_stub():
        alice = st["alice"]
        alice.type_and_wait(f"/script load {st['inst'].scriptdir}/mentions.pl")
        assert wait_log(re.escape(NOTICE), count=2), "the stub was not unloaded:\n" + log_text()

    def t_broken_script():
        """A mentions.pl that does not compile is destroyed before the idle
        job runs: no notice, no "not loaded" error. The new value of a
        setting is kept, the old node goes."""
        port = st["port"]
        w = work / "broken"
        w.mkdir()
        cfg = harness.config(port, "dave")
        marker = '"perl/core" = { perl_use_lib'
        cfg = cfg.replace(marker, '"perl/core/scripts" = { mentions_log_file = "OLDVALUE"; mentions_window = "OldWin"; };\n'
                          '  "fe-common/irc" = { mentions_log_file = "NEWVALUE"; };\n  ' + marker)
        dave = harness.Erssi(st["inst"], w, "dave", cfg)
        st["dave"] = dave
        autorun = dave.home / "scripts" / "autorun"
        autorun.mkdir(parents=True)
        (autorun / "mentions.pl").symlink_to(Path(__file__).resolve().parent / "fixtures" / "broken" / "mentions.pl")
        dave.start()
        assert dave.wait_file(dave.all_log(), "has joined #test", 15), "dave never joined #test"
        ran = dave.home_dir / "broken.ran"
        harness.wait_until(ran.exists, WAIT)
        assert ran.exists(), "the broken script did not run"
        # a negative check: the idle job that would print the notice runs
        # when erssi is idle, not at a point a test can wait for - a short
        # bounded wait
        time.sleep(1.5)
        text = dave.all_log().read_bytes().decode(errors="replace")
        assert NOTICE not in text, text
        assert "not loaded" not in text, text
        dave.type_and_wait("/save")
        assert dave.wait_file(dave.home / "config", "mentions_log_file", WAIT)
        cfgtext = file_text(dave.home / "config")
        assert "NEWVALUE" in cfgtext and "OLDVALUE" not in cfgtext, cfgtext
        assert "perl/core/scripts" not in cfgtext or "mentions_log_file" not in \
            re.search(r'"perl/core/scripts"\s*=\s*\{([^}]*)\}', cfgtext).group(1), cfgtext
        assert 'mentions_window = "OldWin"' in cfgtext and "OldWin" in re.search(
            r'"fe-common/irc"\s*=\s*\{([^}]*)\}', cfgtext).group(1), cfgtext
        dave.quit()
        assert dave.sanitizer_reports() == [], dave.sanitizer_reports()

    def t_partly_read_config():
        """A config with a parse error is read only in part: the migration
        applies for the run but must not make the file be rewritten (the
        part after the error would be lost)."""
        w = work / "partial"
        w.mkdir()
        cfg = harness.config(st["port"], "erin")
        marker = '"perl/core" = { perl_use_lib'
        cfg = cfg.replace(marker, '"perl/core/scripts" = { mentions_log_file = "OLDVALUE"; };\n  ' + marker)
        cfg += 'aliases = { SENTINEL = "x"; };\ntruncated = { key = "never closed'

        erin = harness.Erssi(st["inst"], w, "erin", cfg)
        st["erin"] = erin
        (erin.home / "scripts" / "autorun").mkdir(parents=True)
        (erin.home / "scripts" / "autorun" / "mentions.pl").symlink_to(
            Path(__file__).resolve().parent / "fixtures" / "mentions.pl")
        # the bytes as the harness wrote them, before erssi is started
        # (start() writes the same text again): a file rewritten during init
        # would differ from these
        erin._prepare()
        before = (erin.home / "config").read_bytes()
        assert before.endswith(b'truncated = { key = "never closed'), before
        erin.start()
        assert erin.wait_file(erin.all_log(), "has joined #test", 15), "erin never joined #test"
        erin.type_and_wait("/set mentions_log_file")
        assert erin.wait_file(erin.all_log(), "OLDVALUE", WAIT), "the migrated value does not apply for the run"
        time.sleep(1)  # a negative check: a rewrite from an idle job - a short bounded wait
        erin.quit()
        after = (erin.home / "config").read_bytes()
        assert after == before, "the partly read config was rewritten:\n" + after.decode(errors="replace")
        assert erin.sanitizer_reports() == [], erin.sanitizer_reports()

    def t_sanitizers():
        alice = st["alice"]
        alice.quit()
        assert alice.sanitizer_reports() == [], alice.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_saved_at_once, t_message, t_unloaded, t_settings_moved, t_no_duplicates, t_stub, t_broken_script,
                      t_partly_read_config, t_sanitizers])
    finally:
        if "bob" in st:
            st["bob"].close()
        if "erin" in st:
            st["erin"].kill()
        if "dave" in st:
            st["dave"].kill()
        if "alice" in st:
            st["alice"].kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
