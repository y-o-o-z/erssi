#!/usr/bin/env python3
"""What the side panels draw stays inside them and fits the terminal.
- A panel is at least 6 columns wide: with /SET sidepanel_left_width 1
  the names were drawn over the first columns of the main window, with
  sidepanel_right_width 1 the nicks wrapped past the last column.
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

    def t_sanitizers():
        reports = [p for e in st["all"] for p in e.sanitizer_reports()]
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_width_lower_bound, t_sanitizers])
    finally:
        for e in st["all"]:
            e.kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
