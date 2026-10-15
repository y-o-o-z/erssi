#!/usr/bin/env python3
"""/LAYOUT SAVE and the next start: the main windows come back with the
side panels on (the panel's columns are not part of the saved layout) and
off, a layout saved with the panel's columns in it (as before 2026.10.1)
is restored too, and a layout block that makes no sense never stops erssi
from starting - it falls back to one main window.
Usage: test_layout.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
PANELS_OFF = '"fe-ansi" = { sidepanel_left = "no"; sidepanel_right = "no"; };'


def cfg(nick, panels=True, extra=""):
    text = harness.config(1, nick, extra=extra)
    if not panels:
        text = text.replace('"irc/core" =', PANELS_OFF + '\n  "irc/core" =', 1)
    return text


def block(config_text, name):
    """{key: {field: value}} of a top level block of a saved config, None
    when the config has no such block."""
    m = re.search(r"^" + name + r" = \{\n(.*?)^\};", config_text, re.M | re.S)
    if m is None:
        return None
    out = {}
    for key, body in re.findall(r"^  (\S+) = \{(.*?)\};", m.group(1), re.M | re.S):
        out[key] = dict(re.findall(r'(\w+) = "([^"]*)";', body))
    return out


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-layout."))
    st = {"all": []}

    def t_install():
        st["inst"] = harness.install(build, work)

    def started(e):
        """erssi is running and its input line works: a /echo reaches the log."""
        assert e.proc.poll() is None, f"{e.name}: erssi exited at start ({e.proc.returncode})"
        mark = f"layout-alive-{e.name}-{time.time()}"
        e.type(f"/echo {mark}")
        assert e.wait_file(e.all_log(), mark, WAIT), \
            f"{e.name}: erssi does not answer (exited: {e.proc.poll()})"

    def save(e):
        """/LAYOUT SAVE + /SAVE; the saved config text."""
        conf = e.home / "config"
        before = conf.stat().st_mtime_ns if conf.exists() else 0
        e.type("/layout save")
        e.type("/save")
        end = time.time() + WAIT
        while conf.stat().st_mtime_ns == before and time.time() < end:
            time.sleep(0.1)
        return conf.read_text()

    def new(name, text):
        e = harness.Erssi(st["inst"], work, name, text, args=("--noconnect",))
        st["all"].append(e)
        e.start()
        time.sleep(2)
        started(e)
        return e

    def restart(e):
        e.quit()
        e.config_text = None  # the config it saved
        e.start()
        time.sleep(2)
        started(e)

    def save_restart(name, text, cmds):
        """cmds, /LAYOUT SAVE, restart: (the saved config, the layout saved
        again after the restart)."""
        e = new(name, text)
        for c in cmds:
            e.type(c)
        saved = save(e)
        assert block(saved, "mainwindows"), f"no mainwindows block saved:\n{saved}"
        restart(e)
        again = save(e)
        e.quit()
        return saved, again

    # The geometry only, not the keys: a key is the refnum of the window
    # active in that main window, and that one is not always the same
    # after a start (as before this test).
    def geometry(layout):
        return sorted((int(w["first_line"]), int(w["lines"]), int(w["first_column"]), int(w["columns"]))
                      for w in layout.values())

    def rows(layout):
        return sorted({(int(w["first_line"]), int(w["lines"])) for w in layout.values()})

    def same_layout(saved, again):
        layout, restored = block(saved, "mainwindows"), block(again, "mainwindows")
        assert restored and geometry(restored) == geometry(layout), \
            f"restored layout differs:\nsaved    {layout}\nrestored {restored}"
        return layout

    def t_left_panel_one_main_window():
        # the crash: one main window after the left panel, a hidden window
        saved, again = save_restart("lone", cfg("lone"), ["/window new hide", "/window name second"])
        layout = same_layout(saved, again)
        assert len(layout) == 1, layout
        # columns of the main area: the panel's are not saved
        (w,) = layout.values()
        assert w["first_column"] == "0" and w["columns"] == "185", layout
        assert "second" in [v.get("name") for v in block(again, "windows").values()], again

    def t_both_panels():
        # the right panel shown in any window, not only in a channel
        text = cfg("both").replace('"irc/core" =',
                                   '"fe-ansi" = { sidepanel_right_auto_hide = "no"; };\n  "irc/core" =', 1)
        saved, again = save_restart("both", text, ["/window new hide"])
        layout = same_layout(saved, again)
        (w,) = layout.values()
        assert w["first_column"] == "0" and w["columns"] == "170", layout

    # With the left panel on and more than one main window only the rows
    # are compared: the panel's columns are then reserved once for each
    # main window at the first setup change (sidepanels-layout.c, not the
    # layout) and the columns come out wrong, with or without a layout.
    def t_left_panel_split():
        saved, again = save_restart("lsplit", cfg("lsplit"), ["/window new split"])
        layout = block(saved, "mainwindows")
        assert len(layout) == 2 and all(w["first_column"] == "0" for w in layout.values()), layout
        restored = block(again, "mainwindows")
        assert len(restored) == 2 and rows(restored) == rows(layout), again

    def t_left_panel_side_by_side():
        saved, again = save_restart("lright", cfg("lright"), ["/window new -right"])
        layout = block(saved, "mainwindows")
        assert sorted(int(w["first_column"]) for w in layout.values())[0] == 0, layout
        restored = block(again, "mainwindows")
        assert len(restored) == 2 and rows(restored) == rows(layout), again

    def t_panels_off_split():
        layout = same_layout(*save_restart("nosplit", cfg("nosplit", False), ["/window new split"]))
        assert len(layout) == 2, layout

    def t_panels_off_side_by_side():
        layout = same_layout(*save_restart("noright", cfg("noright", False), ["/window new -right"]))
        assert len(layout) == 2 and len({w["first_line"] for w in layout.values()}) == 1, layout

    def t_old_layout_with_panel_columns():
        # as erssi saved it before, with the left panel (15 columns) on
        old = """mainwindows = {
  3 = { first_line = "1"; lines = "47"; first_column = "15"; columns = "185"; };
};
windows = {
  1 = { immortal = "yes"; name = "Notices"; servertag = "*"; level = "NOTICES CLIENTNOTICES CLIENTCRAP CLIENTERRORS"; };
  2 = { immortal = "yes"; name = "Mentions"; };
  3 = { name = "second"; };
};
"""
        e = new("old", cfg("old", True, old))
        again = save(e)
        e.quit()
        assert geometry(block(again, "mainwindows")) == [(1, 47, 0, 185)], again
        assert block(again, "windows")["3"].get("name") == "second", again

    def t_old_layout_split_panels_off():
        old = """mainwindows = {
  1 = { first_line = "25"; lines = "23"; first_column = "15"; columns = "185"; };
  3 = { first_line = "1"; lines = "24"; first_column = "15"; columns = "185"; };
};
"""
        e = new("oldsplit", cfg("oldsplit", False, old))
        layout = block(save(e), "mainwindows")
        e.quit()
        assert geometry(layout) == [(1, 24, 0, 200), (25, 23, 0, 200)], layout

    BAD = {
        # no main window at column 0 and a band with no height: the crash
        "nolines": """mainwindows = { 1 = { first_line = "1"; lines = "0"; first_column = "15"; columns = "185"; }; };""",
        "negative": """mainwindows = { 1 = { first_line = "-5"; lines = "-3"; first_column = "-1"; columns = "-9"; }; };""",
        "notablock": """mainwindows = { 1 = "x"; 2 = { first_line = "1"; lines = "47"; }; };""",
        "scalar": """mainwindows = "garbage";""",
        "badkey": """mainwindows = { 0 = { first_line = "1"; lines = "47"; }; abc = { first_line = "1"; lines = "47"; }; };""",
        "huge": """mainwindows = { 1 = { first_line = "1"; lines = "2147483647"; first_column = "0"; columns = "2147483647"; };
  2 = { first_line = "2147483000"; lines = "2147483647"; first_column = "0"; columns = "2147483647"; }; };""",
        # more bands than the screen has room for, each one line high
        "tiny": "mainwindows = { " + " ".join(
            f'{i} = {{ first_line = "{i}"; lines = "1"; first_column = "0"; columns = "200"; }};'
            for i in range(1, 41)) + " };",
        # two bands that overlap
        "overlap": """mainwindows = { 1 = { first_line = "1"; lines = "30"; first_column = "0"; columns = "200"; };
  2 = { first_line = "10"; lines = "30"; first_column = "0"; columns = "200"; }; };""",
    }

    def bad_layout(name):
        def t():
            # "tiny" with the panels off: 15 main windows with the left
            # panel on hang erssi in the side panels (a layout or not)
            e = new("bad-" + name, cfg("bad" + name, name != "tiny", BAD[name]))
            layout = block(save(e), "mainwindows")
            assert layout, f"no main window saved: {layout}"
            if name == "tiny":  # as many as fit: 47 lines, 3 each at least
                assert 1 < len(layout) <= 47 // 3, f"{len(layout)} main windows: {layout}"
            else:
                assert len(layout) == 1, f"not one main window: {layout}"
            e.quit()
        t.__name__ = "t_bad_layout_" + name
        return t

    def t_sanitizers():
        reports = [p for e in st["all"] for p in e.sanitizer_reports()]
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_left_panel_one_main_window, t_both_panels, t_left_panel_split,
                      t_left_panel_side_by_side, t_panels_off_split, t_panels_off_side_by_side,
                      t_old_layout_with_panel_columns, t_old_layout_split_panels_off]
                     + [bad_layout(n) for n in BAD] + [t_sanitizers])
    finally:
        for e in st["all"]:
            e.kill()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
