#!/usr/bin/env python3
"""The side panels' columns on the rows the panels do not cover.
The panels cover the text rows of the main windows; a statusbar of type
window above or below them belongs to its main window and starts after the
left panel's columns (and ends before the right one's). Nothing drew those
cells on such a row: what the screen had there before the panel came or
moved stayed until a /REDRAW - the statusbar's own start (left panel
shown again), its end (right panel back), a line printed at the start
before there were panels (as after an /UPGRADE), an entry of the panel
from before it moved, or a root statusbar whose row became a main window's
(the panels did not move). They are cleared when a panel is put somewhere
else, or the main windows' outer rows are.
Usage: test_panel_rows.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
LAYOUT_WAIT = 3
COLS, ROWS = 120, 40
PANEL = 15  # the default width of both panels
# a window statusbar on top of the main window and one under it, each with
# an item at its right end; the default info bar (root) goes
BARS = """statusbar = {
  items = { head = "HEADMARK"; tail = "TAILMARK"; };
  default = {
    wtop = { type = "window"; placement = "top"; position = "1"; visible = "always";
      items = { head = { }; tail = { alignment = "right"; }; }; };
    window = { type = "window"; placement = "bottom"; position = "1"; visible = "always";
      items = { time = { }; user = { }; window = { }; window_empty = { }; tail = { alignment = "right"; }; }; };
    info = { disabled = "yes"; };
  };
};
"""
TOP, BOTTOM = 1, ROWS - 2  # the rows of the two window statusbars
STALE = "printed before the panels"


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-panel-rows."))
    st = {"all": []}

    def log_text(e):
        try:
            return e.all_log().read_text(errors="replace")
        except OSError:
            return ""

    def panel_cells(rows, y):
        """(left panel's cells, right panel's cells) of row Y"""
        return rows[y][:PANEL], rows[y][COLS - PANEL:]

    def settled(e):
        e.sync_output()
        return e.screen()

    def assert_clean(e, what):
        rows = settled(e)
        for y in (TOP, BOTTOM):
            left, right = panel_cells(rows, y)
            assert not left.strip() and not right.strip(), \
                f"{what}: row {y} under the panels is {left!r} ... {right!r}:\n{rows[y]}"
        # the statusbars themselves are there, between the panels
        assert "HEADMARK" in rows[TOP] and rows[TOP].rstrip().endswith("TAILMARK"), rows[TOP]
        assert "#test" in rows[BOTTOM] and rows[BOTTOM].rstrip().endswith("TAILMARK"), rows[BOTTOM]

    def stale(e, rows=(TOP, BOTTOM)):
        """What a line printed before the panels were there leaves: text
        in the panels' cells of ROWS, put on the screen the harness reads
        (erssi does not know of it, as it does not after it drew a line
        there and moved the statusbar over it)."""
        e.screen()
        cells = e._screen.cells
        for y in rows:
            for x in list(range(PANEL)) + list(range(COLS - PANEL, COLS)):
                cells[y][x] = ((STALE * 3)[x % len(STALE)], ())

    def t_install():
        st["inst"] = harness.install(build, work)
        st["ircd"] = harness.Ircd(work)
        st["port"] = st["ircd"].start()
        e = harness.Erssi(st["inst"], work, "alice", harness.config(st["port"], "alice", extra=BARS))
        st["all"].append(e)
        st["e"] = e
        e.start(COLS, ROWS)
        e.wait_ready()
        assert harness.wait_until(lambda: "has joined #test" in log_text(e), 15), "alice never joined #test"
        e.type_and_wait("/window goto #test", "/set sidepanel_right_auto_hide off")
        assert_clean(e, "start")

    def t_left_panel_back():
        # without the panel the statusbars start at column 0; with it again
        # they start after it, and their old start was left under it
        e = st["e"]
        e.type_and_wait("/set sidepanel_left off")
        rows = settled(e)
        assert rows[TOP].startswith("HEADMARK"), rows[TOP]
        e.type_and_wait("/set sidepanel_left on")
        assert_clean(e, "left panel back")

    def t_right_panel_back():
        # the item at the right end of the statusbars was left under the
        # right panel
        e = st["e"]
        e.type_and_wait("/set sidepanel_right off")
        rows = settled(e)
        assert rows[BOTTOM].rstrip().endswith("TAILMARK"), rows[BOTTOM]
        e.type_and_wait("/set sidepanel_right on")
        assert_clean(e, "right panel back")

    def t_line_before_panels():
        # the start (and an /UPGRADE): a line drawn over the whole width
        # before there were panels, on a row a statusbar then took
        e = st["e"]
        e.type_and_wait("/set sidepanel_left off", "/set sidepanel_right off")
        settled(e)
        stale(e)
        e.type_and_wait("/set sidepanel_left on", "/set sidepanel_right on")
        assert_clean(e, "panels after a line was printed")

    def t_panel_moved():
        # the top statusbar goes: the panels cover its row too; it comes
        # back: the panels move down, and their entries (the left one's
        # header, the right one's first nick) were left on that row
        e = st["e"]
        e.type_and_wait("/statusbar modify -disable wtop")
        assert harness.wait_until(lambda: "HEADMARK" not in settled(e)[TOP], WAIT), settled(e)[TOP]
        rows = settled(e)
        assert rows[TOP][:PANEL].strip(), f"the left panel does not cover row {TOP}: {rows[TOP]}"
        e.type_and_wait("/statusbar modify -nodisable wtop")
        assert harness.wait_until(lambda: "HEADMARK" in settled(e)[TOP], WAIT), settled(e)[TOP]
        assert_clean(e, "panels moved down")

    def t_root_bar_made_window():
        # the topic bar (root, over the whole width) is made a window
        # statusbar: its row is the main window's now, and starts after
        # the left panel; the panels stay where they are, and the start
        # and the end of the root bar were left under them
        e = st["e"]
        # the channel's sync draws the panels again (about 3 s after the
        # join): over before the modify, or it could do the clear itself
        assert harness.wait_until(lambda: "Join to #test was synced" in log_text(e), 15), \
            "#test never synced"
        e.type_and_wait("/topic TOPICSTART of the channel", "/statusbar additem -alignment right tail topic")
        assert harness.wait_until(lambda: "TOPICSTART" in settled(e)[0], WAIT), settled(e)[0]
        # nothing of the topic change left to draw: the panels are laid
        # out again only for what the modify does
        e.type_and_wait("/redraw")
        rows = settled(e)
        assert rows[0].startswith(" TOPICSTART") and rows[0].rstrip().endswith("TAILMARK"), rows[0]
        assert rows[0][:PANEL].strip() and rows[0][COLS - PANEL:].strip(), rows[0]
        e.type_and_wait("/statusbar modify -type window topic")
        # the panels are laid out a few ms after the bars came; without
        # that nothing clears these cells until some other event (with no
        # traffic, none comes in this time)
        assert harness.wait_until(lambda: not settled(e)[0].startswith(" TOPICSTART"), LAYOUT_WAIT), \
            settled(e)[0]
        rows = settled(e)
        for y in (0, 1):
            left, right = panel_cells(rows, y)
            assert not left.strip() and not right.strip(), \
                f"row {y} under the panels is {left!r} ... {right!r}:\n{rows[y]}"
        assert any("TOPICSTART" in rows[y] for y in (0, 1)), rows[:2]

    def t_quit():
        st["e"].quit()

    def t_sanitizers():
        reports = [p for e in st["all"] for p in e.sanitizer_reports()]
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_left_panel_back, t_right_panel_back, t_line_before_panels,
                      t_panel_moved, t_root_bar_made_window, t_quit, t_sanitizers])
    finally:
        for e in st["all"]:
            e.kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
