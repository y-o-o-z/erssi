#!/usr/bin/env python3
"""The built-in keepnick: /KEEPNICK, /UNKEEPNICK, /LISTNICK; the nick taken
back when its holder quits (ISON, or MONITOR when 005 offers it) or changes
nick (at once); 437 (nick delay) tried at most once per check; a manual
/NICK pauses, a nick the server forces does not; keepnick_auto for a nick
taken at connect; keepnick_quiet; the nicks of ~/.erssi/keepnick (the
keepnick.pl script's file) imported once; the built-in off while a script
named keepnick is loaded.
Usage: test_keepnick.py <build dir> [<source dir>]"""
import os
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
              script_settings=None, net_extra="", core_settings=""):
        cfg = harness.config(port, nick, channels=channels)
        if core_settings:
            assert 'core = { real_name' in cfg, cfg
            cfg = cfg.replace('core = { real_name', 'core = { %s real_name' % core_settings, 1)
        if net_extra:
            assert 'E2E = { type = "IRC"; };' in cfg, cfg
            cfg = cfg.replace('E2E = { type = "IRC"; };', 'E2E = { type = "IRC"; %s };' % net_extra)
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

    # keepnick.pl as far as these tests need it: /keepnick <nick> takes the
    # nick through the script's own redirect and names, the regain line of
    # its own (keepnick.pl's change_nick and sig_redir_keepnick_nick)
    FAKE_SCRIPT = r"""use Irssi; use Irssi::Irc;
sub own { my ($server, $new) = @_; Irssi::signal_stop(); Irssi::print("fake keepnick.pl got $new back"); }
sub redir { Irssi::signal_add_first('message own_nick', \&own); Irssi::signal_emit('event nick', @_);
            Irssi::signal_remove('message own_nick', \&own); }
Irssi::command_bind("keepnick", sub {
    my ($data, $server) = @_;
    Irssi::print("fake keepnick.pl: $data");
    return unless $server && $data;
    $server->redirect_event('keepnick nick', 1, ":$data", -1, '',
                            { "event nick" => "redir keepnick nick", "" => "event empty" });
    $server->send_raw("NICK :$data");
});
Irssi::signal_add('redir keepnick nick', \&redir);
Irssi::Irc::Server::redirect_register('keepnick nick', 0, 0, undef,
    { "event nick" => 0, "event 432" => -1, "event 433" => -1, "event 437" => -1, "event 484" => -1 }, undef);
"""

    def config_has(e, nick):
        return re.search(r'E2E = \{[^}]*keepnick = "%s";' % re.escape(nick), (e.home / "config").read_text())

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
        # keepnick_autosave (ON): in the file at once, no /SAVE
        assert config_has(a, "zed"), (a.home / "config").read_text()
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
        assert 'keepnick = "' not in (a.home / "config").read_text()  # saved at once
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
        # the 730/731 of the nick are not printed as unknown numerics,
        # also not the 730 for our new nick that crosses our MONITOR -
        time.sleep(1)
        out = log_since(a, off)
        assert "730" not in out and "731" not in out, out
        assert "zed!" not in out.split(got_back("zed"))[-1], out
        finish(a)

    def t_monitor_user():
        """the user's own MONITOR: its replies printed, our MONITOR - does
        not remove its entry"""
        port, tr = ircd("monuser", "--nick-rules", "--monitor")
        holder = harness.Client(port, "zed")
        a = erssi("monuser", port, "alice", 'keepnick_auto = "no";')
        off = log_size(a)
        start = len(trace(tr))
        a.type("/quote MONITOR + zed,bob")
        assert wait_log(a, off, "bob"), "the user's 731 for bob not printed:\n" + log_since(a, off)
        a.type("/keepnick zed")
        assert wait_trace(tr, r" in alice MONITOR \+ :?zed$", start), trace(tr)[start:]
        leave(holder)
        assert wait_log(a, off, got_back("zed"), 4), log_since(a, off)
        time.sleep(1)
        assert not sent(tr, r" MONITOR - ", start), sent(tr, r" MONITOR ", start)
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
        a = erssi("refused", port, "alice", 'keepnick_auto = "no"; keepnick_autosave = "no";')
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
        # keepnick_autosave OFF: only /SAVE writes it
        assert not config_has(a, "bad"), (a.home / "config").read_text()
        finish(a)

    def t_invalid():
        """nicks that are not one IRC word, or longer than NICKLEN, are not
        kept: from /KEEPNICK and from the config, said once, nothing sent"""
        port, tr = ircd("invalid", "--nick-rules")
        a = erssi("invalid", port, "alice", 'keepnick_auto = "no";', net_extra='keepnick = "x,y";')
        assert wait_log(a, 0, "Keepnick: x,y cannot be kept on E2E"), log_since(a, 0)
        start = len(trace(tr))
        off = log_size(a)
        # NICKLEN counts bytes: five characters, ten bytes
        for bad in ("bad,nick", ":colon", "#chan", "averyverylongnick", "żżżżż"):
            out = run(a, f"/keepnick {bad}")
            assert f"Keepnick: {bad} cannot be kept on E2E" in out, out
            assert "Now keeping" not in out, out
        time.sleep(CHECK + 1)
        assert not sent(tr, r" (ISON|NICK|MONITOR) ", start), sent(tr, r" (ISON|NICK) ", start)
        assert log_since(a, 0).count("x,y cannot be kept") == 1, log_since(a, 0)
        finish(a)
        # too long for NICKLEN only: listed as refused, not as active
        port, tr = ircd("invalid-len", "--nick-rules")
        b = erssi("invalid-len", port, "alice", 'keepnick_auto = "no";', net_extra='keepnick = "averylongnick";')
        assert wait_log(b, 0, "Keepnick: averylongnick cannot be kept on E2E: longer"), log_since(b, 0)
        out = run(b, "/listnick")
        assert "Keeping averylongnick in [E2E] (refused: longer than the server's NICKLEN)" in out, out
        assert not sent(tr, r" (ISON|NICK) :?averylongnick")
        finish(b)

    def t_autosave_guard():
        """keepnick_autosave writes the file only when irssi's autosave
        would: not over a file changed outside erssi, not with
        settings_autosave OFF - one line each, /SAVE writes it"""
        a = erssi("guard", 1, "alice", args=("--noconnect",), join=False)
        run(a, "/echo started")
        cfg = a.home / "config"
        cfg.write_text(cfg.read_text() + "\n# edited outside erssi\n")
        later = time.time() + 10
        os.utime(cfg, (later, later))
        out = run(a, "/keepnick -net E2E zed")
        assert "Keepnick: Now keeping zed on [E2E]" in out, out
        assert "the config file was changed outside erssi; /SAVE to write the kept nick" in out, out
        text = cfg.read_text()
        assert "# edited outside erssi" in text and 'keepnick = "' not in text, text
        finish(a)

        b = erssi("guard-off", 1, "alice", args=("--noconnect",), join=False,
                  core_settings='settings_autosave = "no";')
        out = run(b, "/keepnick -net E2E zed")
        assert "settings_autosave is OFF; /SAVE to write the kept nick" in out, out
        assert 'keepnick = "' not in (b.home / "config").read_text()
        run(b, "/save")
        assert config_has(b, "zed"), (b.home / "config").read_text()
        b.quit()
        assert b.sanitizer_reports() == [], b.sanitizer_reports()

    def t_bad_bytes():
        """730/731/303 with bytes that are not UTF-8, and rfc1459 case
        variants of the user's own MONITOR target: compared by the
        server's casemapping, byte by byte; the user's still printed"""
        port, tr = ircd("bytes", "--nick-rules", "--monitor", "--casemapping", "rfc1459")
        holder = harness.Client(port, "{zed}")
        a = erssi("bytes", port, "alice", 'keepnick_auto = "no";')
        off = log_size(a)
        a.type("/quote MONITOR + [bob],[zed]")
        assert wait_log(a, off, "[bob]"), log_since(a, off)
        a.type("/keepnick [zed]")
        assert wait_trace(tr, r" in alice MONITOR \+ \[zed\]$", 0), trace(tr)[-1500:]
        with harness.Client(port, "ctl") as ctl:
            for line in (b":e2e.test 731 alice :\xf0", b":e2e.test 730 alice :\xf0!u@h,\xe2\x82",
                         b":e2e.test 303 alice :\xf0", b":e2e.test 731 alice :{BOB},\xf0\xa0"):
                ctl.send(f"TESTRAWHEX alice {line.hex()}")
            ctl.send("PING sync")
            ctl.expect("PONG")
        assert wait_log(a, off, "{BOB}"), "the user's target in another case not printed:\n" + log_since(a, off)
        out = run(a, "/listnick")
        assert "Keeping [zed] in [E2E] (active)" in out, out
        off = log_size(a)
        leave(holder)
        assert wait_log(a, off, got_back("[zed]"), CHECK + 4), log_since(a, off)
        # the 731 of the user's own [zed], as the server spells it: printed
        assert wait_log(a, off, "{zed}"), log_since(a, off)
        finish(a)

    def t_late_484():
        """a 484 while our NICK waits for a reply the redirect no longer
        has (another client's NICK ended it) is not ours: printed, no
        pause"""
        port, tr = ircd("late484", "--nick-rules")
        holder = harness.Client(port, "zed")
        holder.send("JOIN #test")
        holder.expect(" 366 ")
        grab = harness.Client(port, "grab")
        grab.send("JOIN #test")
        grab.expect(" 366 ")
        a = erssi("late484", port, "alice", 'keepnick_auto = "no"; keepnick_check_time = "60s";')
        off = log_size(a)
        start = len(trace(tr))
        a.type("/keepnick zed")
        assert wait_trace(tr, r" in alice ISON :?zed$", start), "no ISON"
        time.sleep(0.5)
        with harness.Client(port, "ctl") as ctl:
            ctl.send("TESTMUTE alice")  # our NICK gets no reply at all
            ctl.send("PING sync")
            ctl.expect("PONG")
            holder.sock.sendall(b"NICK zed2\r\n")
            grab.sock.sendall(b"NICK zed\r\n")
            assert wait_trace(tr, r" in alice NICK :?zed$", start), trace(tr)[start:]
            assert wait_log(a, off, "grab is now known as zed"), log_since(a, off)
            ctl.send("TESTRAW alice ::e2e.test 484 alice :Your connection is restricted!")
            assert wait_log(a, off, "Your connection is restricted"), log_since(a, off)
        out = log_since(a, off)
        assert "deactivated" not in out and "refused" not in out, out
        out = run(a, "/listnick")
        assert "Keeping zed in [E2E] (active)" in out, out
        holder.close()
        grab.close()
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
            (e.home / "keepnick").write_text("e2e\tzold\nnosuchnet\tfoo\nE2E  zed\r\n")

        a = erssi("migrate", 1, "alice", args=("--noconnect",), join=False, before=old_file,
                  script_settings='keepnick_quiet = "yes";')
        assert a.wait_file(a.all_log(), "Keepnick: imported 1 nick", WAIT), a.all_log().read_text()
        assert "does not read the file again" in a.all_log().read_text()
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
        """the Perl script keepnick: the built-in steps back (its commands
        are the script's), a regain by the script through its own redirect
        is processed and said once; loaded again: nothing said or sent;
        another script whose name starts with keepnick does not count;
        unloaded: the built-in is back"""
        if not st["inst"].has_perl:
            raise harness.SkipCase("no Perl in this build")
        port, tr = ircd("script", "--nick-rules")
        holder = harness.Client(port, "zed")
        a = erssi("script", port, "alice", 'keepnick_auto = "no";')
        script = a.home / "scripts" / "keepnick.pl"
        script.write_text(FAKE_SCRIPT)
        helper = a.home / "scripts" / "keepnick_helper.pl"
        helper.write_text("use Irssi;\n")
        off = log_size(a)
        start = len(trace(tr))
        run(a, f"/script load {helper}")
        time.sleep(0.5)
        assert "built-in keepnick is off" not in log_since(a, off), log_since(a, off)
        a.type(f"/script load {script}")
        assert wait_log(a, off, "built-in keepnick is off until it is unloaded"), log_since(a, off)
        run(a, f"/script load {script}")  # again: unloaded and loaded
        time.sleep(1)
        out = log_since(a, off)
        assert out.count("built-in keepnick is off") == 1 and "on again" not in out, out
        assert not sent(tr, r" (ISON|MONITOR|NICK) ", start), sent(tr, r" (ISON|MONITOR|NICK) ", start)
        leave(holder)
        off = log_size(a)
        a.type("/keepnick zed")
        assert wait_log(a, off, "fake keepnick.pl got zed back"), log_since(a, off)
        time.sleep(1)
        out = log_since(a, off)
        assert out.count("fake keepnick.pl got zed back") == 1, out
        assert "Nickstealer" not in out and "now known as" not in out, out
        assert len(sent(tr, r" NICK :?zed$", start)) == 1, sent(tr, r" NICK ", start)
        off = log_size(a)
        a.type("/script unload keepnick")
        assert wait_log(a, off, "built-in keepnick is on again"), log_since(a, off)
        a.type("/nick alice")
        assert wait_log(a, off, "now known as alice"), log_since(a, off)
        out = run(a, "/keepnick zed")
        assert "Keepnick: Now keeping zed on [E2E]" in out, out
        assert wait_log(a, off, got_back("zed"), CHECK + 4), log_since(a, off)
        time.sleep(0.5)
        assert log_since(a, off).count(got_back("zed")) == 1, log_since(a, off)
        finish(a)

    def t_auto_script():
        """keepnick_auto does nothing while the script is loaded, and does
        not start later when it is unloaded"""
        if not st["inst"].has_perl:
            raise harness.SkipCase("no Perl in this build")
        port, tr = ircd("autoscript", "--nick-rules")
        holder = harness.Client(port, "alice")

        def autorun(e):
            (e.home / "scripts" / "autorun").mkdir(parents=True, exist_ok=True)
            (e.home / "scripts" / "autorun" / "keepnick.pl").write_text(FAKE_SCRIPT)

        a = erssi("autoscript", port, "alice", before=autorun)
        assert wait_log(a, 0, "built-in keepnick is off"), log_since(a, 0)
        assert "is in use on" not in log_since(a, 0), log_since(a, 0)
        start = len(trace(tr))
        leave(holder)
        off = log_size(a)
        a.type("/script unload keepnick")
        assert wait_log(a, off, "built-in keepnick is on again"), log_since(a, off)
        time.sleep(CHECK + 1)
        assert not sent(tr, r" (ISON|NICK) ", start), sent(tr, r" (ISON|NICK) ", start)
        finish(a)

    def t_holder_flips():
        """a holder that flips nicks quickly: at most one NICK per check
        interval, the 433s never printed"""
        port, tr = ircd("flip", "--nick-rules")
        holder = harness.Client(port, "zed")
        holder.send("JOIN #test")
        holder.expect(" 366 ")
        a = erssi("flip", port, "alice", 'keepnick_auto = "no"; keepnick_check_time = "60s";')
        off = log_size(a)
        start = len(trace(tr))
        a.type("/keepnick zed")
        assert wait_trace(tr, r" in alice ISON :?zed$", start), "no ISON"
        time.sleep(0.5)
        for _ in range(4):
            holder.sock.sendall(b"NICK zed2\r\nNICK zed\r\n")  # one write: the server takes both first
            time.sleep(0.7)
        time.sleep(1)
        nicks = sent(tr, r" NICK :?zed$", start)
        assert len([n for n in nicks if " in alice " in n]) == 1, nicks
        assert "already in use" not in log_since(a, off), log_since(a, off)
        holder.close()
        finish(a)

    def t_late_reply():
        """another client's NICK onto the nick comes between our NICK and
        its 433: the 433 is still ours, not printed"""
        port, tr = ircd("late", "--nick-rules")
        holder = harness.Client(port, "zed")
        holder.send("JOIN #test")
        holder.expect(" 366 ")
        grab = harness.Client(port, "grab")
        grab.send("JOIN #test")
        grab.expect(" 366 ")
        a = erssi("late", port, "alice", 'keepnick_auto = "no"; keepnick_check_time = "60s";')
        off = log_size(a)
        start = len(trace(tr))
        a.type("/keepnick zed")
        assert wait_trace(tr, r" in alice ISON :?zed$", start), "no ISON"
        time.sleep(0.5)
        holder.sock.sendall(b"NICK zed2\r\n")
        grab.sock.sendall(b"NICK zed\r\n")
        assert wait_trace(tr, r" out alice :\S+ 433 alice zed ", start), trace(tr)[start:]
        time.sleep(1)
        out = log_since(a, off)
        assert "grab is now known as zed" in out, out
        assert "already in use" not in out, out
        holder.close()
        grab.close()
        finish(a)

    def t_two_servers():
        """two connections of one network: the second does not keep the
        first one's nick for itself (keepnick_auto), and takes over the
        network's kept nick when the first goes"""
        port, tr = ircd("twoserv", "--nick-rules")
        holder = harness.Client(port, "zed")
        a = erssi("twoserv", port, "alice")
        off = log_size(a)
        a.type(f"/connect -network e2e 127.0.0.1 {port}")  # the network's name in another case
        assert wait_trace(tr, r" in alice_ ", 0, 15), trace(tr)[-2000:]
        assert wait_log(a, off, "Welcome alice_"), log_since(a, off)
        time.sleep(1)
        assert "is in use on" not in log_since(a, off), log_since(a, off)
        start = len(trace(tr))
        out = run(a, "/keepnick -net E2E zed")
        assert "Keepnick: Now keeping zed on [E2E]" in out, out
        assert wait_trace(tr, r" in alice ISON :?zed$", start), trace(tr)[start:]
        assert not sent(tr, r" in alice_ ISON ", start)
        start = len(trace(tr))
        with harness.Client(port, "ctl") as ctl:
            ctl.send("TESTDROP alice")
            ctl.send("PING sync")
            ctl.expect("PONG")
        assert wait_trace(tr, r" in alice_ ISON :?zed$", start, CHECK + 4), trace(tr)[start:]
        leave(holder)
        assert wait_log(a, off, got_back("zed"), CHECK + 4), log_since(a, off)
        assert wait_trace(tr, r" in alice_ NICK :?zed$", start), trace(tr)[start:]
        finish(a)

    def t_upgrade():
        """/UPGRADE: MONITOR + again (its reply not printed), the nick still
        taken back; a pause stays paused"""
        port, tr = ircd("upgrade", "--nick-rules", "--monitor")
        holder = harness.Client(port, "zed")
        a = erssi("upgrade", port, "alice", 'keepnick_auto = "no";')
        off = log_size(a)
        a.type("/keepnick zed")
        assert wait_trace(tr, r" in alice MONITOR \+ :?zed$", 0), trace(tr)[-2000:]
        start = len(trace(tr))
        a.type("/upgrade")
        assert wait_trace(tr, r" in alice MONITOR \+ :?zed$", start, 15), "no MONITOR + after /UPGRADE"
        time.sleep(1)
        out = run(a, "/listnick")
        assert "Keepnick: Keeping zed in [E2E] (active)" in out, out
        leave(holder)
        assert wait_log(a, off, got_back("zed"), 5), log_since(a, off)
        assert "zed!" not in log_since(a, off), log_since(a, off)
        a.type("/nick bob")
        assert wait_log(a, off, "Nickkeeping deactivated on [E2E]"), log_since(a, off)
        start = len(trace(tr))
        a.type("/upgrade")
        time.sleep(3)
        out = run(a, "/listnick")
        assert "Keepnick: Keeping zed in [E2E] (inactive)" in out, out
        time.sleep(CHECK + 1)
        assert not sent(tr, r" (ISON|NICK|MONITOR \+) ", start), sent(tr, r" (ISON|NICK|MONITOR) ", start)
        finish(a)

    def t_auto_pause():
        """keepnick_auto and a manual /NICK: paused (nothing sent), /NICK
        back reactivates"""
        port, tr = ircd("autopause", "--nick-rules")
        holder = harness.Client(port, "alice")
        a = erssi("autopause", port, "alice")
        assert wait_log(a, 0, "Keepnick: alice is in use on [E2E]"), log_since(a, 0)
        off = log_size(a)
        a.type("/nick bob")
        assert wait_log(a, off, "Keepnick: Nickkeeping deactivated on [E2E]"), log_since(a, off)
        out = run(a, "/listnick")
        assert "Keeping alice in [E2E] (inactive, this connection)" in out, out
        start = len(trace(tr))
        leave(holder)
        time.sleep(CHECK + 1)
        assert not sent(tr, r" (ISON|NICK) ", start), sent(tr, r" (ISON|NICK) ", start)
        a.type("/nick alice")
        assert wait_log(a, off, "Keepnick: Nickkeeping reactivated on [E2E]"), log_since(a, off)
        finish(a)

    def t_casemapping():
        """CASEMAPPING=rfc1459: {zed} holds [zed] - in the ISON and 730/731
        replies it is the same nick"""
        for mode in ("ison", "monitor"):
            flags = ["--nick-rules", "--casemapping", "rfc1459"] + (["--monitor"] if mode == "monitor" else [])
            port, tr = ircd("case-" + mode, *flags)
            holder = harness.Client(port, "{zed}")
            a = erssi("case-" + mode, port, "alice", 'keepnick_auto = "no";')
            off = log_size(a)
            start = len(trace(tr))
            a.type("/keepnick [zed]")
            assert wait_trace(tr, r" in alice (ISON :?|MONITOR \+ )\[zed\]$", start), trace(tr)[start:]
            time.sleep(1.5)
            assert not sent(tr, r" NICK ", start), (mode, sent(tr, r" NICK ", start))
            assert "{zed}" not in log_since(a, off), (mode, log_since(a, off))
            leave(holder)
            assert wait_log(a, off, got_back("[zed]"), CHECK + 4), (mode, log_since(a, off))
            finish(a)

    def cleanup():
        for e in st["erssis"]:
            e.kill()
        for s in st["ircds"]:
            s.stop()

    try:
        harness.main([t_install, t_ison_quit_save_list, t_holder_nick_change, t_holder_flips, t_late_reply,
                      t_late_484, t_monitor, t_monitor_user, t_bad_bytes, t_nick_delay,
                      t_refused, t_invalid, t_autosave_guard,
                      t_manual_pause_forced_not, t_auto, t_auto_pause, t_auto_off, t_two_servers,
                      t_casemapping, t_upgrade, t_migrate, t_script_loaded, t_auto_script])
    finally:
        cleanup()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
