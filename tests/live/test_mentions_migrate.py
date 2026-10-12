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
        end = time.time() + timeout
        while True:
            if len(re.findall(pattern, log_text())) >= count:
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

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
        alice.type("/alias wn echo active=$winname")
        st["bob"] = bob = harness.Client(port, "bob")
        bob.send("JOIN #test")
        assert alice.wait_file(alice.all_log(), "bob [u@127.0.0.1] has joined #test", WAIT)

    def t_message():
        assert wait_log(re.escape(NOTICE)), "no migration notice:\n" + log_text()
        assert "<irssi dir>/scripts/autorun" in log_text()

    def t_unloaded():
        assert wait_log(r"Unloaded script|Unloaded"), "the script was not unloaded:\n" + log_text()
        alice = st["alice"]
        alice.type("/mentions")
        time.sleep(0.5)
        alice.type("/wn")
        assert wait_log(r"active=\S+"), log_text()
        assert re.findall(r"active=(\S*)", log_text())[-1] == "Mentions", log_text()
        assert "OLD SCRIPT RAN" not in log_text()

    def t_settings_moved():
        alice = st["alice"]
        st["bob"].send("PRIVMSG alice :moved hello")
        assert alice.wait_file(st["old_log"], "<bob>", WAIT), "no entry in the log file of the script"
        assert "moved hello" in file_text(st["old_log"])
        alice.type("/set mentions_log")
        assert wait_log(r"mentions_log\s+(= )?ON"), "mentions_log is not ON:\n" + log_text()
        alice.type("/save")
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
        time.sleep(1.5)
        assert file_text(st["old_log"]).count("only once") == 1, file_text(st["old_log"])

    def t_stub():
        alice = st["alice"]
        alice.type(f"/script load {st['inst'].scriptdir}/mentions.pl")
        assert wait_log(re.escape(NOTICE), count=2), "the stub was not unloaded:\n" + log_text()

    def t_sanitizers():
        alice = st["alice"]
        alice.quit()
        assert alice.sanitizer_reports() == [], alice.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_message, t_unloaded, t_settings_moved, t_no_duplicates, t_stub,
                      t_sanitizers])
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
