#!/usr/bin/env python3
"""Side-by-side main windows keep their columns when the side panels come
and go. With sidepanel_right_auto_hide on (the default) the right panel
is shown only while the active window is a channel, so it appears and
goes at every switch between a channel and another window; the left one
goes and comes back with /SET sidepanel_left. Each time every row of
side-by-side windows is laid out again over the new main area; before,
it was laid out from the widths the last time left (already rounded), so
after a few switches a window was a column wider or narrower than the
user had made it. Now the widths a user sets (/WINDOW GROW, SHRINK,
SIZE and BALANCE -right, a split, a window closed, a new terminal size,
a restored layout) are the reference every later layout is made from,
row by row: back to the same panels, the same columns.
Usage: test_panel_widths.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

block = harness.block

WAIT = 10
TRIPS = 6
# the widths of the main windows, set by hand with the right panel off:
# two stacked rows, ones the old layout lost a column of in the first
# round trip (67 117 came back as 68 116, 41 54 88 as 42 53 88)
TARGET = [[67, 117], [41, 54, 88]]


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-panel-widths."))
    st = {}

    def save():
        """/LAYOUT SAVE + /SAVE: the columns of the main windows, row by
        row from the top, each row from the left:
        [[(first_column, columns), ...], ...]."""
        e = st["alice"]
        e.type_and_wait("/layout save", "/save")
        layout = block((e.home / "config").read_text(), "mainwindows")
        assert layout, "no mainwindows block saved"
        st["layout"] = layout
        rows = {}
        for w in layout.values():
            rows.setdefault(int(w["first_line"]), []).append((int(w["first_column"]), int(w["columns"])))
        return [sorted(rows[line]) for line in sorted(rows)]

    def widths(layout):
        return [[w for _, w in row] for row in layout]

    def log_text(off):
        try:
            return st["alice"].all_log().read_bytes()[off:].decode(errors="replace")
        except OSError:
            return ""

    def goto(name):
        e = st["alice"]
        off = e.all_log().stat().st_size
        e.type_and_wait(f"/window goto {name}", "/wn")
        assert f"active={name}" in log_text(off), f"window {name} did not become active"

    def tiles(layout, reserved):
        """Every row fills the main area (the terminal's columns less the
        panels') from its column 0, one separator column between two
        windows."""
        for row in layout:
            col = 0
            for first, width in row:
                assert first == col and width >= 20, (layout, reserved)
                col = first + width + 1
            assert col - 1 == st["cols"] - reserved, (layout, reserved)

    def resize(width, want):
        """/WINDOW GROW|SHRINK -right: the active main window WANT wide"""
        if want > width:
            st["alice"].type_and_wait(f"/window grow -right {want - width}")
        elif want < width:
            st["alice"].type_and_wait(f"/window shrink -right {width - want}")

    def channel():
        goto("#test")

    def other():
        goto("bee")

    def left(on):
        def step():
            st["alice"].type_and_wait(f"/set sidepanel_left {'on' if on else 'off'}")
        return step

    def trips(steps, start, what, seen=None):
        """TRIPS times STEPS [(action, the panels' columns after it)], the
        last one back where the trips start (the columns START): after
        each step the columns are the same in every trip (or SEEN[step])."""
        seen = dict(seen or {})
        for i in range(TRIPS):
            for j, (step, reserved) in enumerate(steps):
                step()
                got = save()
                tiles(got, reserved)
                want = start if j == len(steps) - 1 else seen.get(j)
                assert want is None or got == want, \
                    f"{what}, trip {i + 1}, step {j + 1}: {got} != {want}"
                seen[j] = got
        return seen

    def right_trips(start, what, on=None):
        """the right panel on (#test active) and off (bee active)"""
        return trips([(channel, 30), (other, 15)], start, what, {0: on} if on else None)

    def t_install():
        st["inst"] = harness.install(build, work)

    def started(off=0):
        """alice joined #test (in the log past OFF)"""
        a = st["alice"]
        end = time.time() + 15
        while "has joined #test" not in log_text(off):
            assert time.time() < end, "alice never joined #test"
            time.sleep(0.1)
        a.type_and_wait("/alias wn echo active=$winname$C")

    def t_start():
        st["ircd"] = harness.Ircd(work)
        port = st["ircd"].start()
        a = harness.Erssi(st["inst"], work, "alice", harness.config(port, "alice"))
        st["alice"] = a
        st["cols"] = 200
        a.start(200, 50)
        started()
        # the lower row: aye, bee (no channel: no right panel) and #test;
        # above it top1 and top2; the panels' columns 15 on the left, 15
        # on the right
        goto("#test")
        a.type_and_wait("/window new hide")
        a.type_and_wait("/window name aye")
        a.type_and_wait("/window new -right")
        a.type_and_wait("/window name bee")
        a.type_and_wait("/window show -right #test")
        goto("aye")
        a.type_and_wait("/window new split")
        a.type_and_wait("/window name top1")
        a.type_and_wait("/window new -right")
        a.type_and_wait("/window name top2")
        # uneven widths set by hand
        cols = save()
        assert [len(row) for row in cols] == [2, 3], cols
        goto("top1")
        resize(cols[0][0][1], TARGET[0][0])
        goto("aye")
        resize(save()[1][0][1], TARGET[1][0])
        goto("bee")
        resize(save()[1][1][1], TARGET[1][1])
        st["off"] = save()
        tiles(st["off"], 15)
        assert widths(st["off"]) == TARGET, st["off"]

    def t_right_panel():
        st["on"] = right_trips(st["off"], "right panel")[0]

    def t_left_panel():
        # the right panel off (bee active), the left one goes and comes back
        trips([(left(False), 0), (left(True), 15)], st["off"], "left panel")

    def t_both_panels():
        trips([(channel, 30), (left(False), 15), (other, 0), (left(True), 15)], st["off"],
              "both panels", {0: st["on"]})

    def t_resize_is_new_reference():
        # widths set with the right panel on are the new reference: kept
        # with the panel on, and the same each time it goes
        a = st["alice"]
        channel()
        before = save()
        a.type_and_wait("/window grow -right 5")
        on = save()
        tiles(on, 30)
        # #test is the rightmost: the columns come from bee, next to it;
        # the row above stays as it was
        assert on[1][2][1] == before[1][2][1] + 5 and on[1][1][1] == before[1][1][1] - 5, (on, before)
        assert on[0] == before[0], (on, before)
        other()
        off = save()
        tiles(off, 15)
        right_trips(off, "after /window grow -right", on)

    def t_balance_is_new_reference():
        a = st["alice"]
        other()
        a.type_and_wait("/window balance -right")
        off = save()
        tiles(off, 15)
        assert max(widths(off)[1]) - min(widths(off)[1]) <= 1, off
        right_trips(off, "after /window balance -right")

    def t_split_is_new_reference():
        a = st["alice"]
        other()
        a.type_and_wait("/window new -right")
        a.type_and_wait("/window name dee")
        other()
        off = save()
        tiles(off, 15)
        assert [len(row) for row in off] == [2, 4], off
        right_trips(off, "after a split")

    def t_close_is_new_reference():
        a = st["alice"]
        goto("dee")
        a.type_and_wait("/window hide")
        other()
        off = save()
        tiles(off, 15)
        assert [len(row) for row in off] == [2, 3], off
        right_trips(off, "after a window closed")

    def t_terminal_resize_is_new_reference():
        a = st["alice"]
        for cols in (171, 200):
            st["cols"] = cols
            a.resize(cols, 50)
            # erssi has the new size once the rows end at its right edge
            end = time.time() + WAIT
            while True:
                other()
                off = save()
                if all(row[-1][0] + row[-1][1] == cols - 15 for row in off) or time.time() > end:
                    break
            tiles(off, 15)
            assert [len(row) for row in off] == [2, 3], off
            right_trips(off, f"after the terminal resized to {cols} columns")

    def lines():
        """the rows of the layout last saved: [(first_line, lines), ...]"""
        return sorted({(int(w["first_line"]), int(w["lines"])) for w in st["layout"].values()})

    def t_size_is_new_reference():
        # /WINDOW SIZE -right N: bee's text N columns wide (and one for
        # the separator), 7 more than before, the columns from #test next
        # to it; the widths are the new reference
        a = st["alice"]
        other()
        before = save()
        want = widths(before)[1][1] + 7
        a.type_and_wait(f"/window size -right {want - 1}")
        off = save()
        tiles(off, 15)
        assert widths(off)[1][1] == want and off[0] == before[0], (want, off, before)
        assert widths(off)[1][0] == widths(before)[1][0], (off, before)
        right_trips(off, "after /window size -right")
        # and without -right the lines, as before
        other()
        a.type_and_wait("/window size 10")
        save()
        low = lines()
        a.type_and_wait("/window size 13")
        save()
        high = lines()
        assert len(low) == 2 and high[1][1] == low[1][1] + 3 and high[0][1] == low[0][1] - 3, (low, high)

    def t_restored_layout_is_new_reference():
        # the layout saved (bee active: the right panel off) comes back
        # after a restart, and the panels keep the restored widths
        a = st["alice"]
        other()
        saved = save()
        off = a.all_log().stat().st_size
        a.quit()
        a.config_text = None  # the config it saved
        a.start(200, 50)
        started(off)
        other()
        restored = save()
        tiles(restored, 15)
        # the row of two comes back as it was saved; the row of three
        # with other widths (the restore sets the widths from the right,
        # each against the neighbour it set just before: not what this
        # test is about), so the widths kept are those it restored
        assert restored[0] == saved[0] and len(restored[1]) == 3, f"restored {restored}, saved {saved}"
        right_trips(restored, "after a restored layout")

    def t_sanitizers():
        reports = st["alice"].sanitizer_reports() if "alice" in st else []
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_start, t_right_panel, t_left_panel, t_both_panels,
                      t_resize_is_new_reference, t_balance_is_new_reference, t_split_is_new_reference,
                      t_close_is_new_reference, t_terminal_resize_is_new_reference,
                      t_size_is_new_reference, t_restored_layout_is_new_reference, t_sanitizers])
    finally:
        if "alice" in st:
            st["alice"].kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
