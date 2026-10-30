#!/usr/bin/env python3
"""Side-by-side main windows keep their columns when the side panels come
and go. With sidepanel_right_auto_hide on (the default) the right panel
is shown only while a row of main windows shows a channel (here only
#test, in the lower row), so it appears and goes at every switch between
#test and another window of that row; the left one
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

WAIT = 30  # a deadline for what comes at once: a loaded machine is slow
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
        end = time.time() + WAIT
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
        # uneven widths set by hand, the right panel off (with top1
        # active it shows the lower row's #test)
        a.type_and_wait("/set sidepanel_right off")
        cols = save()
        assert [len(row) for row in cols] == [2, 3], cols
        goto("top1")
        resize(cols[0][0][1], TARGET[0][0])
        goto("aye")
        resize(save()[1][0][1], TARGET[1][0])
        goto("bee")
        resize(save()[1][1][1], TARGET[1][1])
        # bee active: no row shows a channel, the panel stays hidden
        a.type_and_wait("/set sidepanel_right on")
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

    def t_grow_shrink_one_column():
        # /WINDOW GROW|SHRINK -right without a number: one column, as the
        # forms without -right are one line; the widths the new reference
        a = st["alice"]
        other()
        before = save()
        a.type_and_wait("/window grow -right")
        grown = save()
        tiles(grown, 15)
        assert widths(grown)[1][1] == widths(before)[1][1] + 1 and grown[0] == before[0], (grown, before)
        assert widths(grown)[1][2] == widths(before)[1][2] - 1, (grown, before)
        right_trips(grown, "after /window grow -right")
        a.type_and_wait("/window shrink -right")
        back = save()
        assert back == before, (back, before)
        right_trips(back, "after /window shrink -right")
        # and the lines, as before
        other()
        low = lines()
        a.type_and_wait("/window grow")
        save()
        high = lines()
        assert high[1][1] == low[1][1] + 1 and high[0][1] == low[0][1] - 1, (low, high)
        a.type_and_wait("/window shrink")
        save()
        assert lines() == low, (low, lines())

    OUT_OF_RANGE = "Not a number of lines or columns from 1 to"

    def t_numbers_out_of_range():
        # a number past the terminal's size, INT_MIN, a negative one,
        # 0 and garbage: refused with an error, nothing resized (before,
        # atoi() overflowed or a negated INT_MIN did)
        a = st["alice"]
        other()
        before = save()
        for cmd in ("grow", "shrink", "size"):
            for right in ("", "-right "):
                for num in ("99999999999", "201", "-- -2147483648", "-- -5", "0", "abc"):
                    off = a.all_log().stat().st_size
                    a.type_and_wait(f"/window {cmd} {right}{num}")
                    out = log_text(off)
                    assert OUT_OF_RANGE in out, f"/window {cmd} {right}{num}: no error:\n{out}"
                    assert save() == before, f"/window {cmd} {right}{num} resized"
        # a number in range still works
        a.type_and_wait("/window grow -right 2")
        assert widths(save())[1][1] == widths(before)[1][1] + 2
        a.type_and_wait("/window shrink -right 2")
        assert save() == before

    def t_trailing_space_and_missing_size():
        # "/window grow -right 2 " (an alias that ends in a space) is the
        # number 2 again, and /window size without a number says so
        a = st["alice"]
        other()
        before = save()
        a.type_and_wait("/window grow -right 2 ")
        assert widths(save())[1][1] == widths(before)[1][1] + 2, "trailing space refused"
        a.type_and_wait("/window shrink -right 2  ")
        assert save() == before
        for right in ("", "-right "):
            for tail in ("", " "):
                off = a.all_log().stat().st_size
                a.type_and_wait(f"/window size {right}{tail}")
                out = log_text(off)
                assert "Give the number of lines or columns" in out, f"no clear error:\n{out}"
                assert OUT_OF_RANGE not in out and save() == before, out

    def t_restored_layout_is_new_reference():
        # the layout saved (the right panel off) comes back after a
        # restart, and the panels keep the restored widths. The panel is
        # switched off, not auto-hidden: the restart does not put every
        # window back in its main window, and the panel shows while a
        # row shows a channel
        a = st["alice"]
        other()
        a.type_and_wait("/set sidepanel_right off")
        saved = save()
        off = a.all_log().stat().st_size
        a.quit()
        a.config_text = None  # the config it saved
        a.start(200, 50)
        started(off)
        restored = save()
        tiles(restored, 15)
        assert restored == saved, f"restored {restored}, saved {saved}"

        def panel_on():
            a.type_and_wait("/set sidepanel_right on")
            channel()

        def panel_off():
            a.type_and_wait("/set sidepanel_right off")
        trips([(panel_on, 30), (panel_off, 15)], restored, "after a restored layout")

    def t_sanitizers():
        reports = st["alice"].sanitizer_reports() if "alice" in st else []
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_start, t_right_panel, t_left_panel, t_both_panels,
                      t_resize_is_new_reference, t_balance_is_new_reference, t_split_is_new_reference,
                      t_close_is_new_reference, t_terminal_resize_is_new_reference,
                      t_size_is_new_reference, t_grow_shrink_one_column, t_numbers_out_of_range, t_trailing_space_and_missing_size,
                      t_restored_layout_is_new_reference,
                      t_sanitizers])
    finally:
        if "alice" in st:
            st["alice"].kill()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
