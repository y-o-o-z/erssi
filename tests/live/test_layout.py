#!/usr/bin/env python3
"""/LAYOUT SAVE and the next start: the main windows come back with the
side panels on (the panel's columns are not part of the saved layout) and
off, a layout saved with the panel's columns in it (as before 2026.10.1)
is restored too, and a layout block that makes no sense never stops erssi
from starting - it falls back to one main window.
The side panels reserve their columns once for the whole screen: any
number of main windows, stacked or side by side, share them, after a
/SET, a terminal resize, a split, /WINDOW HIDE|SHOW|GROW|SHRINK and a
restored layout (before, every main window reserved them again: wrong
columns, overlapping panels, and erssi hung at about 15 main windows).
A `windows` block of the wrong shape is skipped, not fatal at start.
Usage: test_layout.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

block = harness.block

WAIT = 10
PANELS_OFF = '"fe-ansi" = { sidepanel_left = "no"; sidepanel_right = "no"; };'


def cfg(nick, panels=True, extra=""):
    text = harness.config(1, nick, extra=extra)
    if not panels:
        text = text.replace('"irc/core" =', PANELS_OFF + '\n  "irc/core" =', 1)
    return text


def tiling(layout, cols, rows, reserved=15):
    """The main windows tile the main area of a cols x rows terminal with
    `reserved` columns for the left panel: the rows from line 1 down to
    the last line of the main area, without gaps; in each row, windows of
    the same height side by side from column 0 of the main area to its
    right edge, one separator column between two. Returns the problems."""
    width, bad = cols - reserved, []
    bands = {}
    for key, w in layout.items():
        for f in ("first_line", "lines", "first_column", "columns"):
            if not re.fullmatch(r"\d+", w.get(f, "")):
                bad.append(f"{key}: {f} = {w.get(f)!r}")
        if bad:
            return bad
        bands.setdefault(int(w["first_line"]), []).append(w)
    line = 1
    for first in sorted(bands):
        band = sorted(bands[first], key=lambda w: int(w["first_column"]))
        heights = {int(w["lines"]) for w in band}
        if first != line or len(heights) != 1:
            bad.append(f"row at line {first} (expected {line}), heights {heights}")
        col = 0
        for w in band:
            if int(w["first_column"]) != col:
                bad.append(f"row at line {first}: a window at column {w['first_column']}, expected {col}")
            col = int(w["first_column"]) + int(w["columns"]) + 1
        if col - 1 != width:
            bad.append(f"row at line {first} ends at column {col - 1}, expected {width}")
        line = first + max(heights)
    if line - 1 != rows - 3:
        bad.append(f"the rows end at line {line - 1}, expected {rows - 3}")
    return bad


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

    def new(name, text, cols=200, rows=50):
        e = harness.Erssi(st["inst"], work, name, text, args=("--noconnect",))
        st["all"].append(e)
        e.start(cols, rows)
        time.sleep(2)
        started(e)
        return e

    def restart(e, cols=200, rows=50):
        e.quit()
        e.config_text = None  # the config it saved
        e.start(cols, rows)
        time.sleep(2)
        started(e)

    def tiles(e, cols=200, rows=50, what="", reserved=15):
        """The layout erssi saves now tiles the screen; that layout."""
        layout = block(save(e), "mainwindows")
        assert layout, f"{what}: no main window saved"
        bad = tiling(layout, cols, rows, reserved)
        assert not bad, f"{what}: {bad}\n{layout}"
        return layout

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

    # With the left panel on and more than one main window the columns
    # are compared too: the panel's columns are reserved once for the
    # screen, not once for each main window.
    def t_left_panel_split():
        saved, again = save_restart("lsplit", cfg("lsplit"), ["/window new split"])
        layout = same_layout(saved, again)
        assert len(layout) == 2 and not tiling(layout, 200, 50), (tiling(layout, 200, 50), layout)

    def t_left_panel_side_by_side():
        saved, again = save_restart("lright", cfg("lright"), ["/window new -right"])
        layout = same_layout(saved, again)
        assert len(layout) == 2 and not tiling(layout, 200, 50), (tiling(layout, 200, 50), layout)

    # A /SET reads the settings again and lays the panels out again: the
    # columns are not reserved a second time.
    def t_left_panel_three_rows_set():
        e = new("rows3", cfg("rows3"))
        e.type("/window new split")
        e.type("/window new split")
        tiles(e, what="three rows")
        e.type("/set mouse_scroll_chat off")
        e.type("/set sidepanel_left_width 15")
        layout = tiles(e, what="three rows after /set")
        assert len(layout) == 3, layout
        e.quit()

    def t_left_panel_side_by_side_set():
        e = new("side2", cfg("side2"))
        e.type("/window new -right")
        e.type("/set mouse_scroll_chat off")
        layout = tiles(e, what="side by side after /set")
        assert len(layout) == 2 and len(rows(layout)) == 1, layout
        # one left panel: the two windows and their separator share the
        # 185 columns of the main area
        assert sum(int(w["columns"]) for w in layout.values()) == 184, layout
        e.quit()

    def t_left_panel_width_set():
        # a new panel width: the old columns are given back, the new
        # ones reserved, for every row
        e = new("width", cfg("width"))
        e.type("/window new split")
        e.type("/window new -right")
        e.type("/set sidepanel_left_width 25")
        tiles(e, what="width 25", reserved=25)
        e.type("/set sidepanel_left off")
        tiles(e, what="panel off", reserved=0)
        e.type("/set sidepanel_left on")
        e.type("/set sidepanel_left_width 15")
        tiles(e, what="panel back")
        e.quit()

    def t_left_panel_window_commands():
        e = new("wcmds", cfg("wcmds"))
        for c in ["/window new split", "/window new split", "/window new -right", "/window grow 2",
                  "/window shrink 1", "/window grow -right 5", "/window hide", "/set mouse_scroll_chat off",
                  "/window show 2", "/window shrink -right 3", "/window balance",
                  "/set mouse_scroll_chat on"]:
            e.type(c)
            tiles(e, what=c)
        e.quit()

    def t_left_panel_terminal_resize():
        e = new("resize", cfg("resize"))
        e.type("/window new split")
        e.type("/window new -right")
        # 30 columns: no room for the panel (it goes, its columns come back)
        for cols, rows_, reserved in ((120, 40, 15), (200, 50, 15), (90, 30, 15), (30, 20, 0),
                                      (200, 50, 15)):
            e.resize(cols, rows_)
            time.sleep(1)
            started(e)
            tiles(e, cols, rows_, f"{cols}x{rows_}", reserved)
        e.quit()

    def t_both_panels_rows_and_side_by_side():
        # the right panel too is one for the screen, at its right edge
        text = cfg("both2").replace('"irc/core" =',
                                    '"fe-ansi" = { sidepanel_right_auto_hide = "no"; };\n  "irc/core" =', 1)
        e = new("both2", text)
        for c in ["/window new split", "/window new split", "/window new -right",
                  "/set mouse_scroll_chat off"]:
            e.type(c)
            tiles(e, what=c, reserved=30)
        e.type("/set sidepanel_right off")
        tiles(e, what="right panel off", reserved=15)
        e.quit()

    def t_left_panel_fifteen_rows_set():
        # about 15 main windows and a /set hung erssi (the columns reserved
        # 15 times over one row: its last column before its first one)
        e = new("rows15", cfg("rows15"))
        for _ in range(14):
            e.type("/window new split")
        e.type("/set mouse_scroll_chat off")
        started(e)
        layout = tiles(e, what="15 rows")
        assert len(layout) == 15, layout
        e.quit()

    def rows_block(count, lines, columns):
        return "mainwindows = { " + " ".join(
            f'{i + 1} = {{ first_line = "{1 + i * lines}"; lines = "{lines}"; '
            f'first_column = "0"; columns = "{columns}"; }};'
            for i in range(count)) + " };"

    def restored_rows(name, count, lines, cols, rows_):
        e = new(name, cfg(name, True, rows_block(count, lines, cols - 15)), cols, rows_)
        layout = tiles(e, cols, rows_, f"{count} rows restored")
        assert len(layout) == count, layout
        e.quit()

    # a valid layout hung the start with the left panel on
    def t_left_panel_restore_7_rows_80x24():
        restored_rows("r7", 7, 3, 80, 24)

    def t_left_panel_restore_15_rows():
        restored_rows("r15", 15, 3, 200, 50)

    def t_left_panel_save_restart_twice_80x24():
        # a layout erssi saves is one it restores: save, restart, save,
        # restart (it saved negative columns, and the next start dropped
        # the layout). Six splits make rows of 2 lines, the restore makes
        # every row at least 3: all 7 come back, and from then on the
        # layout stays the same.
        e = new("twice", cfg("twice"), 80, 24)
        for _ in range(6):
            e.type("/window new split")
        layouts = [tiles(e, 80, 24, "six splits")]
        for i in range(2):
            restart(e, 80, 24)
            layouts.append(tiles(e, 80, 24, f"restart {i + 1}"))
        e.quit()
        assert [len(lay) for lay in layouts] == [7, 7, 7], layouts
        assert geometry(layouts[1]) == geometry(layouts[2]), layouts

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
            e = new("bad-" + name, cfg("bad" + name, True, BAD[name]))
            layout = block(save(e), "mainwindows")
            assert layout, f"no main window saved: {layout}"
            if name == "tiny":  # as many as fit: 47 lines, 3 each at least
                assert 1 < len(layout) <= 47 // 3, f"{len(layout)} main windows: {layout}"
            else:
                assert len(layout) == 1, f"not one main window: {layout}"
            e.quit()
        t.__name__ = "t_bad_layout_" + name
        return t

    # a `windows` block of the wrong shape: the bad entries are skipped,
    # the good ones next to them restored (the names that must come back,
    # and those that must not). A list has no window numbers: nothing of
    # it is restored, as before.
    WINDOWS_BAD = {
        "scalar": ('windows = "x";', [], []),
        "entry": ('windows = { 1 = "x"; 2 = { name = "two"; }; 4 = { name = "four"; }; };',
                  ["two", "four"], []),
        "list": ('windows = ( "x", { name = "y"; } );', [], ["y"]),
        "badkey": ('windows = { abc = { name = "abc"; }; 0 = { name = "zero"; }; '
                   '3 = { name = "three"; }; 5x = { name = "fivex"; }; };', ["three"], ["abc", "zero", "fivex"]),
        "items": ('windows = { 1 = { name = "one"; items = "x"; }; 2 = { name = "two"; }; };',
                  ["one", "two"], []),
        "item": ('windows = { 1 = { name = "one"; items = ( "x", { type = "CHANNEL"; } ); }; '
                 '2 = { name = "two"; }; };', ["one", "two"], []),
    }

    def bad_windows(name):
        def t():
            text, kept, dropped = WINDOWS_BAD[name]
            e = new("badwin-" + name, cfg("badwin" + name, True, text))
            saved = save(e)
            windows = block(saved, "windows")
            assert windows is not None, saved
            names = [w.get("name") for w in windows.values()]
            assert all(n in names for n in kept), f"{kept} not all restored: {windows}"
            assert not any(n in names for n in dropped), f"{dropped} restored: {windows}"
            e.quit()
        t.__name__ = "t_bad_windows_" + name
        return t

    def t_sanitizers():
        reports = [p for e in st["all"] for p in e.sanitizer_reports()]
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_left_panel_one_main_window, t_both_panels, t_left_panel_split,
                      t_left_panel_side_by_side, t_left_panel_three_rows_set, t_left_panel_side_by_side_set,
                      t_left_panel_width_set, t_left_panel_window_commands, t_left_panel_terminal_resize,
                      t_both_panels_rows_and_side_by_side,
                      t_left_panel_fifteen_rows_set, t_left_panel_restore_7_rows_80x24,
                      t_left_panel_restore_15_rows, t_left_panel_save_restart_twice_80x24,
                      t_panels_off_split, t_panels_off_side_by_side,
                      t_old_layout_with_panel_columns, t_old_layout_split_panels_off]
                     + [bad_layout(n) for n in BAD] + [bad_windows(n) for n in WINDOWS_BAD]
                     + [t_sanitizers])
    finally:
        for e in st["all"]:
            e.kill()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
