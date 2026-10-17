#!/usr/bin/env python3
"""What the side panels draw stays inside them and fits the terminal.
- The border between a panel and the main windows is the theme's
  {window_border}, drawn once: before, a hard-coded UTF-8 "│" was drawn
  first, also on a terminal that is not UTF-8 (where its bytes 0x94 0x82
  are C1 controls). On such a terminal a border character that is not
  ASCII is drawn as "|".
- A panel is at least 6 columns wide: with /SET sidepanel_left_width 1
  the names were drawn over the first columns of the main window, with
  sidepanel_right_width 1 the nicks wrapped past the last column.
- A theme that sets a side panel format to "" hides those entries: before,
  the raw window name or nick was written instead, unclipped and with the
  control characters a server can put in a channel name (ESC included).
Usage: test_panel_draw.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

block = harness.block

WAIT = 10
COLS, ROWS = 120, 40
BOX = "│"
BOX_UTF8 = BOX.encode()

# the default theme with a box-drawing border
BOXES_THEME = 'abstracts = { window_border = "%B' + BOX + '%N"; };\n'

LEFT_FORMATS = ("sidepanel_header", "sidepanel_item", "sidepanel_item_selected",
                "sidepanel_item_nick_mention", "sidepanel_item_query_msg",
                "sidepanel_item_activity", "sidepanel_item_events", "sidepanel_item_highlight")
RIGHT_FORMATS = tuple(f"sidepanel_nick_{r}_status" for r in ("owner", "admin", "op", "halfop", "voice", "normal"))
# every entry of both panels hidden
HIDDEN_THEME = 'formats = { "fe-text" = { ' + " ".join(f'{f} = "";' for f in LEFT_FORMATS + RIGHT_FORMATS) + " }; };\n"

ESC_CHANNEL = "#esc\x1b]52;c;QUJD\x07x"


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-panel-draw."))
    st = {"all": []}

    def log_text(e):
        try:
            return e.all_log().read_text(errors="replace")
        except OSError:
            return ""

    def new(name, cols=COLS, rows=ROWS, env=None, extra_theme=None):
        e = harness.Erssi(st["inst"], work, name, harness.config(st["port"], name), env=env)
        st["all"].append(e)
        if extra_theme is not None:
            e.home.mkdir(parents=True, exist_ok=True)
            (e.home / "test.theme").write_text(extra_theme)
        e.start(cols, rows)
        e.wait_ready()
        assert harness.wait_until(lambda: "has joined #test" in log_text(e), 15), f"{name} never joined #test"
        e.type_and_wait("/window goto #test")
        if extra_theme is not None:
            e.type_and_wait("/set theme test")
        return e

    def redraw(e):
        """the output of a full redraw (and what came with it)"""
        e.sync_output()
        mark = e.mark()
        e.type_and_wait("/redraw")
        e.sync_output()
        return e.output_since(mark)

    def main_area(e):
        """(first column, columns) of the one main window in the saved
        layout: it leaves out the left panel's columns, so first_column is
        0 and columns is what the panels leave of the terminal."""
        layout = block(e.save_config("/layout save"), "mainwindows")
        (w,) = layout.values()
        return int(w["first_column"]), int(w["columns"])

    def t_install():
        st["inst"] = harness.install(build, work)
        st["ircd"] = harness.Ircd(work)
        st["port"] = st["ircd"].start()

    def t_border_once_from_theme():
        # the default theme's border is "|": no "│" reaches the terminal
        e = new("utf8")
        out = redraw(e)
        assert BOX_UTF8 not in out, "a border not from the theme was drawn"
        rows = e.screen()
        assert rows[2][14] == "|" and rows[2][COLS - 15] == "|", rows[2]
        # a theme with "│": that is the border
        e.home.joinpath("test.theme").write_text(BOXES_THEME)
        e.type_and_wait("/set theme test")
        redraw(e)
        rows = e.screen()
        assert rows[2][14] == BOX and rows[2][COLS - 15] == BOX, rows[2]
        e.quit()

    def t_border_not_utf8():
        # a terminal that is not UTF-8: no UTF-8 box character, the
        # theme's "│" drawn as "|" (before, only its first byte was left
        # of it)
        env = {"LANG": "C", "LC_ALL": "C"}
        e = new("latin1", env=env, extra_theme=BOXES_THEME)
        e.type_and_wait("/set term_charset ISO-8859-1")
        out = redraw(e)
        assert BOX_UTF8 not in out, "UTF-8 box characters sent to a terminal that is not UTF-8"
        rows = e.screen()
        assert rows[2][14] == "|" and rows[2][COLS - 15] == "|", rows[2]
        e.quit()

    def t_width_lower_bound():
        e = new("narrow")
        e.type_and_wait("/set sidepanel_right_auto_hide off")
        assert main_area(e) == (0, COLS - 30), main_area(e)
        for width, want in (("1", 6), ("5", 6), ("6", 6), ("7", 7), ("15", 15)):
            e.type_and_wait(f"/set sidepanel_left_width {width}", f"/set sidepanel_right_width {width}")
            assert harness.wait_until(lambda: main_area(e) == (0, COLS - 2 * want), WAIT, interval=0.2), \
                f"width {width}: main area {main_area(e)}, panels {want} each expected"
            redraw(e)
            rows = e.screen()
            # the borders where the panels end
            assert rows[2][want - 1] == "|" and rows[2][COLS - want] == "|", (width, rows[2])
        # 0 hides the panel, as before
        e.type_and_wait("/set sidepanel_left_width 0")
        assert harness.wait_until(lambda: main_area(e) == (0, COLS - 15), WAIT, interval=0.2), main_area(e)
        e.quit()

    def t_hidden_entries():
        # a theme that hides every entry: the panels stay empty, also
        # for a channel whose name has an escape sequence in it
        e = new("hidden", extra_theme=HIDDEN_THEME)
        e.type_and_wait("/set sidepanel_right_auto_hide off")
        mark = e.mark()
        with harness.Client(st["port"], "srv") as c:
            c.send(f"TESTRAW hidden x :{':hidden!u@127.0.0.1 JOIN :' + ESC_CHANNEL}")
            assert harness.wait_until(lambda: "#esc" in log_text(e), WAIT), "the join never arrived"
        e.type_and_wait("/window goto #test")
        out = e.output_since(mark) + redraw(e)
        assert b"\x1b]52" not in out, "a channel name's escape sequence reached the terminal"
        rows = e.screen()
        left = [r[:14] for r in rows[1:ROWS - 2]]
        right = [r[COLS - 14:] for r in rows[1:ROWS - 2]]
        assert all(not r.strip() for r in left), left
        assert all(not r.strip() for r in right), right
        e.quit()

    def t_sanitizers():
        reports = [p for e in st["all"] for p in e.sanitizer_reports()]
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_border_once_from_theme, t_border_not_utf8, t_width_lower_bound,
                      t_hidden_entries, t_sanitizers])
    finally:
        for e in st["all"]:
            e.kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
