#!/usr/bin/env python3
"""The built-in keepnick: /KEEPNICK, /UNKEEPNICK, /LISTNICK; the nick taken
back when its holder quits (ISON, or MONITOR when 005 offers it) or changes
nick (at once); 437 (nick delay) tried at most once per check; a manual
/NICK pauses, a nick the server forces does not; keepnick_auto for a nick
taken at connect; keepnick_quiet; the nicks of ~/.erssi/keepnick (the
keepnick.pl script's file) imported once; the built-in off while a script
named keepnick is loaded.
Usage: test_keepnick.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
CHECK = 5  # keepnick_check_time in seconds (its minimum)
QUEUE = '"irc/core" = { cmd_queue_speed = "5msec"; };'


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-keepnick."))
    st = {"ircds": [], "erssis": []}

    def ircd(name, *flags):
        d = work / name
        d.mkdir()
        srv = harness.Ircd(d, flags)
        st["ircds"].append(srv)
        return srv.start(), d / "ircd-trace.log"

    def trace(path):
        try:
            return path.read_text(errors="replace")
        except OSError:
            return ""

    def wait_trace(path, pattern, start=0, timeout=WAIT):
        end = time.time() + timeout
        while True:
            if re.search(pattern, trace(path)[start:], re.M):
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def sent(path, pattern, start=0):
        """the lines erssi sent (the trace's "in" lines of any nick) that match"""
        return [ln for ln in trace(path)[start:].splitlines()
                if re.match(r"^\S+ in ", ln) and re.search(pattern, ln)]

    def erssi(name, port, nick, settings="", channels=("#test",), args=(), join=True, before=None,
              script_settings=None):
        cfg = harness.config(port, nick, channels=channels)
        if script_settings is not None:
            cfg = cfg.replace('"perl/core" = {', '"perl/core/scripts" = { %s };\n  "perl/core" = {'
                              % script_settings, 1)
            assert '"perl/core/scripts"' in cfg, cfg
        assert QUEUE in cfg, cfg
        cfg = cfg.replace(QUEUE, '"irc/core" = { cmd_queue_speed = "5msec"; keepnick_check_time = "%ds"; %s };'
                          % (CHECK, settings))
        e = harness.Erssi(st["inst"], work, name, cfg, args=args)
        st["erssis"].append(e)
        if before is not None:
            before(e)
        e.start()
        if join:
            assert e.wait_file(e.all_log(), "has joined #test", 15), f"{name} never joined #test"
        return e

    def log_size(e):
        try:
            return e.all_log().stat().st_size
        except OSError:
            return 0

    def log_since(e, off):
        try:
            return e.all_log().read_bytes()[off:].decode(errors="replace")
        except OSError:
            return ""

    def wait_log(e, off, text, timeout=WAIT):
        end = time.time() + timeout
        while True:
            if text in log_since(e, off):
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def run(e, cmd, done="cmd-done"):
        """the log lines that CMD printed"""
        off = log_size(e)
        e.type(cmd)
        e.type(f"/echo {done}")
        assert wait_log(e, off, done), f"no output of {cmd}"
        return log_since(e, off).split(done)[0]

    def finish(e):
        e.quit()
        assert e.sanitizer_reports() == [], e.sanitizer_reports()

    def leave(c):
        """c quits and the server has seen it (closing at once can reset the
        connection before the server reads what was sent)"""
        c.send("PING sync")
        c.expect("PONG")
        c.send("QUIT :bye")
        try:
            c.expect("\x00never", 5)  # until the server closes the link
        except AssertionError:
            pass
        c.close()

    def got_back(nick, net="E2E"):
        return f"Keepnick: Nickstealer left [{net}], got {nick} back"

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_ison_quit_save_list():
        """ISON every check; the holder quits (no shared channel): the nick
        is taken within one check, said in each channel window; /SAVE
        writes keepnick = "zed" in the network's block; /LISTNICK,
        /UNKEEPNICK"""
        port, tr = ircd("ison", "--nick-rules")
        holder = harness.Client(port, "zed")
        a = erssi("ison", port, "alice", 'keepnick_auto = "no";', channels=("#test", "#test2"))
        assert a.wait_file(a.all_log(), "has joined #test2", WAIT), "no #test2"
        off = log_size(a)
        start = len(trace(tr))
        a.type("/keepnick zed")
        assert wait_log(a, off, "Keepnick: Now keeping zed on [E2E]"), log_since(a, off)
        assert wait_trace(tr, r" in alice ISON :?zed$", start), "no ISON for zed:\n" + trace(tr)[start:]
        time.sleep(1)
        assert not sent(tr, r" NICK :?zed$", start), "NICK zed sent while zed is on"
        leave(holder)
        assert wait_log(a, off, got_back("zed"), CHECK + 4), "zed not regained:\n" + log_since(a, off)
        out = log_since(a, off)
        assert out.count(got_back("zed")) == 2, out  # #test and #test2
        assert "now known as" not in out, out
        assert "already in use" not in out, out
        assert not sent(tr, r" MONITOR ", start), "MONITOR without MONITOR in 005"
        # nothing more is asked once the nick is ours
        n = len(sent(tr, r" ISON ", start))
        time.sleep(CHECK + 1)
        assert len(sent(tr, r" ISON ", start)) == n, "ISON goes on after the nick was taken"
        run(a, "/save")
        cfg = (a.home / "config").read_text()
        assert re.search(r'E2E = \{[^}]*keepnick = "zed";', cfg), cfg
        out = run(a, "/listnick")
        assert "Keepnick: Keeping zed in [E2E] (active)" in out, out
        out = run(a, "/unkeepnick")
        assert "Keepnick: Stopped trying to keep zed on [E2E]" in out, out
        out = run(a, "/listnick")
        assert "Keepnick: No nicks in keep list" in out, out
        out = run(a, "/unkeepnick E2E")
        assert "Keepnick: No nick is kept on E2E" in out, out
        out = run(a, "/unkeepnick nosuchnet")
        assert "Keepnick: Unknown chat network: nosuchnet" in out, out
        out = run(a, "/keepnick -net nosuchnet bob")
        assert "Keepnick: Unknown chat network: nosuchnet" in out, out
        run(a, "/save")
        cfg = (a.home / "config").read_text()
        assert 'keepnick = "' not in cfg, cfg
        finish(a)

    def t_holder_nick_change():
        """the holder changes nick in a shared channel: taken at once, not
        at the next ISON"""
        port, tr = ircd("nickchange", "--nick-rules")
        holder = harness.Client(port, "zed")
        holder.send("JOIN #test")
        holder.expect(" 366 ")
        a = erssi("nickchange", port, "alice", 'keepnick_auto = "no"; keepnick_check_time = "60s";')
        off = log_size(a)
        start = len(trace(tr))
        a.type("/keepnick zed")
        assert wait_trace(tr, r" in alice ISON :?zed$", start), "no ISON"
        time.sleep(0.5)
        t0 = time.time()
        holder.send("NICK zed2")
        assert wait_log(a, off, got_back("zed"), 3), "zed not taken at once:\n" + log_since(a, off)
        assert time.time() - t0 < 3
        assert len(sent(tr, r" ISON ", start)) == 1, sent(tr, r" ISON ", start)
        holder.close()
        finish(a)

    def t_monitor():
        """MONITOR=100 in 005: MONITOR + zed, no ISON; 731 when the holder
        quits takes it at once; MONITOR - after. keepnick_quiet: said once,
        in the network window, not in the channel"""
        port, tr = ircd("monitor", "--nick-rules", "--monitor")
        holder = harness.Client(port, "zed")
        a = erssi("monitor", port, "alice", 'keepnick_auto = "no"; keepnick_quiet = "yes";',
                  channels=("#test", "#test2"))
        assert a.wait_file(a.all_log(), "has joined #test2", WAIT), "no #test2"
        off = log_size(a)
        start = len(trace(tr))
        a.type("/keepnick zed")
        assert wait_trace(tr, r" in alice MONITOR \+ :?zed$", start), "no MONITOR +:\n" + trace(tr)[start:]
        assert wait_trace(tr, r" out alice :\S+ 730 alice :zed!", start), "no 730"
        time.sleep(0.5)
        t0 = time.time()
        leave(holder)
        assert wait_log(a, off, got_back("zed"), 3), "zed not taken at 731:\n" + log_since(a, off)
        assert time.time() - t0 < 3
        assert wait_trace(tr, r" in zed MONITOR - :?zed$", start), "no MONITOR -"
        assert not sent(tr, r" ISON ", start), sent(tr, r" ISON ", start)
        out = log_since(a, off)
        assert out.count(got_back("zed")) == 1, out
        chan = (a.home / "logs" / "E2E" / "#test.log").read_text(errors="replace")
        assert got_back("zed") not in chan, chan
        # the 730/731 of the nick are not printed as unknown numerics
        assert "730" not in out and "731" not in out, out
        finish(a)

    def t_nick_delay():
        """437 (nick delay): at most one NICK per check, never printed;
        taken once the delay is over"""
        port, tr = ircd("delay", "--nick-rules")
        holder = harness.Client(port, "zed")
        holder.send("TESTDELAY zed 13")
        leave(holder)
        a = erssi("delay", port, "alice", 'keepnick_auto = "no";')
        off = log_size(a)
        start = len(trace(tr))
        a.type("/keepnick zed")
        assert wait_log(a, off, got_back("zed"), 13 + CHECK + 4), "zed never taken:\n" + log_since(a, off)
        times = [float(ln.split()[0]) for ln in sent(tr, r" NICK :?zed$", start)]
        assert 2 <= len(times) <= 4, times
        gaps = [b - a_ for a_, b in zip(times, times[1:])]
        assert all(g >= CHECK - 1 for g in gaps), gaps
        out = log_since(a, off)
        assert "temporarily unavailable" not in out, out
        finish(a)

    def t_refused():
        """432 (erroneous nickname): one message, keeping paused, nothing
        more sent, the 432 not printed as an error"""
        port, tr = ircd("refused", "--nick-rules")
        a = erssi("refused", port, "alice", 'keepnick_auto = "no";')
        with harness.Client(port, "ctl") as ctl:
            ctl.send("TESTREFUSE bad")
            ctl.send("PING sync")
            ctl.expect("PONG")
        off = log_size(a)
        start = len(trace(tr))
        a.type("/keepnick bad")
        assert wait_log(a, off, "Keepnick: The server refused bad on [E2E] (erroneous nickname), "
                        "nickkeeping deactivated on [E2E]"), log_since(a, off)
        time.sleep(CHECK + 1)
        assert len(sent(tr, r" NICK :?bad$", start)) == 1, sent(tr, r" NICK ", start)
        assert len(sent(tr, r" ISON ", start)) == 1, sent(tr, r" ISON ", start)
        out = log_since(a, off)
        assert out.count("Keepnick:") == 2 and "Erroneous" not in out, out
        out = run(a, "/listnick")
        assert "Keepnick: Keeping bad in [E2E] (inactive)" in out, out
        finish(a)

    def t_manual_pause_forced_not():
        """a manual /NICK away pauses (nothing sent), /NICK back and
        /KEEPNICK resume; a nick the server forces does not pause"""
        port, tr = ircd("pause", "--nick-rules")
        a = erssi("pause", port, "alice", 'keepnick_auto = "no";')
        off = log_size(a)
        a.type("/keepnick")
        assert wait_log(a, off, "Keepnick: Now keeping alice on [E2E]"), log_since(a, off)
        a.type("/nick bob")
        assert wait_log(a, off, "Keepnick: Nickkeeping deactivated on [E2E]"), log_since(a, off)
        out = run(a, "/listnick")
        assert "Keepnick: Keeping alice in [E2E] (inactive)" in out, out
        start = len(trace(tr))
        time.sleep(CHECK + 1)
        assert not sent(tr, r" (ISON|NICK) ", start), sent(tr, r" (ISON|NICK) ", start)
        off = log_size(a)
        a.type("/nick alice")
        assert wait_log(a, off, "Keepnick: Nickkeeping reactivated on [E2E]"), log_since(a, off)
        a.type("/nick bob")
        assert wait_log(a, off, "Keepnick: Nickkeeping deactivated on [E2E]"), log_since(a, off)
        off = log_size(a)
        a.type("/keepnick alice")
        assert wait_log(a, off, "Keepnick: Nickkeeping reactivated on [E2E]"), log_since(a, off)
        assert wait_log(a, off, got_back("alice"), 3), log_since(a, off)
        # the server renames us (a collision, SVSNICK): kept trying
        off = log_size(a)
        with harness.Client(port, "ctl") as ctl:
            ctl.send("TESTNICK alice 0PNAAAAAA")
            assert wait_log(a, off, "0PNAAAAAA"), log_since(a, off)
            assert wait_log(a, off, got_back("alice"), CHECK + 4), log_since(a, off)
        assert "deactivated" not in log_since(a, off), log_since(a, off)
        finish(a)

    def t_auto():
        """keepnick_auto (default ON): connected on alice_ because alice is
        taken - alice is taken back when its holder leaves; not saved"""
        port, tr = ircd("auto", "--nick-rules")
        holder = harness.Client(port, "alice")
        a = erssi("auto", port, "alice")
        off = 0
        assert wait_log(a, off, "Keepnick: alice is in use on [E2E]"), log_since(a, off)
        out = run(a, "/listnick")
        assert re.search(r"Keepnick: Keeping alice in \[E2E\] \(active, this connection\)", out), out
        leave(holder)
        assert wait_log(a, off, got_back("alice"), CHECK + 4), log_since(a, off)
        run(a, "/save")
        assert 'keepnick = "' not in (a.home / "config").read_text()
        finish(a)

    def t_auto_off():
        """keepnick_auto OFF: the alternate nick stays"""
        port, tr = ircd("autooff", "--nick-rules")
        holder = harness.Client(port, "alice")
        a = erssi("autooff", port, "alice", 'keepnick_auto = "no";')
        start = len(trace(tr))
        leave(holder)
        time.sleep(CHECK + 2)
        assert not sent(tr, r" NICK :?alice$", start), sent(tr, r" NICK ", start)
        assert not sent(tr, r" ISON ", start), sent(tr, r" ISON ", start)
        out = run(a, "/listnick")
        assert "Keepnick: No nicks in keep list" in out, out
        finish(a)

    def t_migrate():
        """~/.erssi/keepnick (net<TAB>nick, the script's file): imported once
        into the network's keepnick, said in one line, the file left; an
        unknown network is skipped; not imported again after /UNKEEPNICK.
        The script's keepnick_quiet comes along"""
        def old_file(e):
            e.home.mkdir(parents=True, exist_ok=True)
            (e.home / "keepnick").write_text("e2e\tzed\nnosuchnet\tfoo\n")

        a = erssi("migrate", 1, "alice", args=("--noconnect",), join=False, before=old_file,
                  script_settings='keepnick_quiet = "yes";')
        assert a.wait_file(a.all_log(), "Keepnick: imported 1 nick", WAIT), a.all_log().read_text()
        out = run(a, "/listnick")
        assert "Keepnick: Keeping zed in [E2E] (active)" in out, out
        cfg = (a.home / "config").read_text()
        assert re.search(r'E2E = \{[^}]*keepnick = "zed";', cfg), cfg  # saved at start
        assert (a.home / "keepnick").exists()
        out = run(a, "/set keepnick_quiet")
        assert re.search(r"keepnick_quiet\s*=?\s*ON\b", out), out
        run(a, "/unkeepnick E2E")
        run(a, "/save")
        finish(a)
        log = a.all_log()
        log.unlink()
        a.config_text = None  # keep the saved config
        a.start()
        out = run(a, "/listnick")
        assert "Keepnick: No nicks in keep list" in out, out
        assert "imported" not in log.read_text(), log.read_text()
        finish(a)

    def t_script_loaded():
        """a Perl script named keepnick: the built-in steps back (its
        commands are the script's) and comes back when it is unloaded"""
        if not st["inst"].has_perl:
            raise harness.SkipCase("no Perl in this build")
        port, tr = ircd("script", "--nick-rules")
        holder = harness.Client(port, "zed")
        a = erssi("script", port, "alice", 'keepnick_auto = "no";')
        script = a.home / "scripts" / "keepnick.pl"
        script.write_text('use Irssi;\n'
                          'Irssi::command_bind("keepnick", sub { Irssi::print("fake keepnick.pl: $_[0]"); });\n')
        off = log_size(a)
        a.type(f"/script load {script}")
        assert wait_log(a, off, "built-in keepnick is off until it is unloaded"), log_since(a, off)
        start = len(trace(tr))
        out = run(a, "/keepnick zed")
        assert "fake keepnick.pl: zed" in out, out
        assert "Now keeping" not in out, out
        time.sleep(1)
        assert not sent(tr, r" (ISON|MONITOR|NICK) ", start), sent(tr, r" (ISON|NICK) ", start)
        off = log_size(a)
        a.type("/script unload keepnick")
        assert wait_log(a, off, "built-in keepnick is on again"), log_since(a, off)
        out = run(a, "/keepnick zed")
        assert "Keepnick: Now keeping zed on [E2E]" in out, out
        assert wait_trace(tr, r" in alice ISON :?zed$", start), "no ISON after the unload"
        holder.close()
        finish(a)

    def cleanup():
        for e in st["erssis"]:
            e.kill()
        for s in st["ircds"]:
            s.stop()

    try:
        harness.main([t_install, t_ison_quit_save_list, t_holder_nick_change, t_monitor, t_nick_delay, t_refused,
                      t_manual_pause_forced_not, t_auto, t_auto_off, t_migrate, t_script_loaded])
    finally:
        cleanup()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
