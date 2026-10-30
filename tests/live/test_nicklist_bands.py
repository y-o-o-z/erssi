#!/usr/bin/env python3
"""The right side panel with stacked main windows: one band of nicks for
every row of main windows, each over the text rows of its own row, with
the nicks of that row's channel (the active main window's window, or else
the one next to the panel). A row whose window is no channel has an empty
band, and with sidepanel_right_auto_hide the panel stays while one band
shows a channel; a band whose window is closed shows the next one. A
nick that joins shows up in its own band only; a click or the wheel in
a band acts on that band's channel. With one main window
the panel is what it was: the active channel's nicks over all its rows.
Before, the panel showed the active main window's channel over all the
main windows, and went away when that window was no channel.
Usage: test_nicklist_bands.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

block = harness.block

WAIT = 30  # a deadline for what comes at once: a loaded machine is slow
COLS, ROWS = 200, 50
# the right panel: its default 15 columns at the right edge, the border
# in the first one, the scroll markers in the last one
PANEL = COLS - 15
# the border: the theme's window_border, or the panel's own line
BORDER = ("|", "\u2502")
UP, DOWN = "\u2191", "\u2193"


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-nicklist-bands."))
    st = {"clients": []}

    def log_text(off):
        try:
            return st["alice"].all_log().read_bytes()[off:].decode(errors="replace")
        except OSError:
            return ""

    def wn():
        """the active window: (name or channel, refnum)"""
        a = st["alice"]
        off = a.all_log().stat().st_size
        a.type_and_wait("/wn")
        m = re.search(r"active=(\S*) refnum=(\d+)", log_text(off))
        assert m, "no answer to /wn:\n" + log_text(off)
        return m.group(1), int(m.group(2))

    def goto(name):
        st["alice"].type_and_wait(f"/window goto {name}")
        got = wn()
        assert got[0] == name, f"window {name} did not become active: {got}"
        return got[1]

    def mainwindows():
        """/LAYOUT SAVE: the main windows from the top, [(first_line,
        lines, refnum of the window it shows)]"""
        a = st["alice"]
        a.type_and_wait("/layout save", "/save")
        layout = block((a.home / "config").read_text(), "mainwindows")
        assert layout, "no mainwindows block saved"
        return sorted((int(w["first_line"]), int(w["lines"]), int(key)) for key, w in layout.items())

    def panel(rows):
        """the right panel's rows: (border, text, marker)"""
        return [(r[0], r[1:-1].strip(), r[-1]) for r in rows]

    def nicks(rows, first, lines):
        """the nicks in the panel over a main window's rows, top down,
        an empty row ''"""
        return [text.lstrip("@+ ") for _, text, _ in panel(rows)[first:first + lines]]

    def listing(want, lines):
        """WANT from the top of a band, the rest of the LINES rows empty"""
        return list(want) + [""] * (lines - len(want))

    def wait_bands(want, what):
        """the panel shows WANT: {(first_line, lines): [nicks]}, every
        row of those main windows with the border - but for the rows of
        the statusbars under the lowest one (st["bars_below"]): the panel
        does not cover them, and its columns there are empty"""
        bottom = max(first + lines for first, lines in want)
        last = bottom - st.get("bars_below", 0)

        def ok(rows):
            p = panel(rows)
            for (first, lines), names in want.items():
                if nicks(rows, first, lines) != listing(names, lines):
                    return False
                if any(b not in BORDER for b, _, _ in p[first:min(first + lines, last)]):
                    return False
            return all(not rows[y].strip() for y in range(last, bottom))
        rows, good = st["alice"].wait_screen(ok, WAIT, PANEL)
        assert good, f"{what}: want {want}, the panel shows:\n" + \
            "\n".join(f"{i:2} {r!r}" for i, r in enumerate(rows))
        return rows

    def rows_of(name):
        """(first_line, lines) of the main window showing window NAME"""
        return st["where"][name]

    def layout_two():
        """the two stacked main windows and the channel each shows"""
        mws = mainwindows()
        assert len(mws) == 2, mws
        by_refnum = {st["refnum"][name]: name for name in st["refnum"]}
        st["where"] = {by_refnum[ref]: (first, lines) for first, lines, ref in mws if ref in by_refnum}
        st["upper"] = by_refnum.get(mws[0][2])
        st["lower"] = by_refnum.get(mws[1][2])
        return mws

    def click(x, y, button=0):
        """a mouse press and release (SGR) at the 0-based cell x, y"""
        st["alice"].write_raw(f"\x1b[<{button};{x + 1};{y + 1}M\x1b[<{button};{x + 1};{y + 1}m".encode())

    def wheel(x, y, down):
        st["alice"].write_raw(f"\x1b[<{65 if down else 64};{x + 1};{y + 1}M".encode())

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_start():
        st["ircd"] = harness.Ircd(work)
        port = st["ircd"].start()
        st["port"] = port
        for nick, chan in (("carol", "#one"), ("dave", "#two")):
            c = harness.Client(port, nick)
            st["clients"].append(c)
            c.send(f"JOIN {chan}")
            c.expect(f" JOIN {chan}")
        a = harness.Erssi(st["inst"], work, "alice",
                          harness.config(port, "alice", channels=("#one", "#two")))
        st["alice"] = a
        a.start(COLS, ROWS)
        end = time.time() + WAIT
        while not all(f"has joined {c}" in log_text(0) for c in ("#one", "#two")):
            assert time.time() < end, "alice never joined #one and #two"
            time.sleep(0.1)
        a.type_and_wait("/alias wn echo active=$winname$C$Q refnum=$winref")
        st["refnum"] = {name: goto(name) for name in ("#two", "#one")}

    def t_one_window():
        # the active channel's nicks over the main window's rows, as before
        mws = mainwindows()
        assert len(mws) == 1, mws
        first, lines, _ = mws[0]
        st["single"] = (first, lines)
        rows = wait_bands({(first, lines): ["alice", "carol"]}, "#one, one main window")
        # the exact rows, to compare with the build before the bands
        print("one main window, #one, panel rows:")
        for i, r in enumerate(rows):
            print(f"  {i:2} {r!r}")
        goto("#two")
        wait_bands({(first, lines): ["alice", "dave"]}, "#two, one main window")
        # the wheel scrolls it as before: down (at most to the last nick)
        # and back up
        wheel(PANEL + 5, first + 1, True)
        rows = wait_bands({(first, lines): ["dave"]}, "#two, scrolled down")
        assert panel(rows)[first][2] == UP, panel(rows)[first]
        wheel(PANEL + 5, first + 1, False)
        rows = wait_bands({(first, lines): ["alice", "dave"]}, "#two, scrolled back")
        assert panel(rows)[first][2] == " ", panel(rows)[first]
        goto("#one")

    def t_two_rows():
        # #one split: the new main window shows #two
        a = st["alice"]
        a.type_and_wait("/window new split")
        goto("#two")
        layout_two()
        assert {st["upper"], st["lower"]} == {"#one", "#two"}, st
        st["names"] = {"#one": ["alice", "carol"], "#two": ["alice", "dave"]}
        bands("two stacked main windows, #two active")

    def t_statusbar_gap():
        # a statusbar under every main window: rows between the bands,
        # the panel's part of them empty. The upper row is 3 lines high
        # and its last line shows a nick before the bar comes; then that
        # line is the bar's, and nothing of the nick stays there.
        a = st["alice"]
        upper = st["upper"]
        c = harness.Client(st["port"], "fay")
        st["clients"].append(c)
        c.send(f"JOIN {upper}")
        c.expect(f" JOIN {upper}")
        names = st["names"][upper] = sorted(st["names"][upper] + ["fay"])
        goto(upper)
        a.type_and_wait("/window size 3")
        layout_two()
        first, lines = rows_of(upper)
        assert lines == 3, st["where"]
        bands("the upper row 3 lines high")
        a.type_and_wait("/statusbar add -type window -placement bottom -visible always gap",
                        "/statusbar additem window gap")
        st["bars_below"] = 1
        low_first, low_lines = rows_of(st["lower"])

        def ok(rows):
            p = panel(rows)
            return (nicks(rows, first, 2) == names[:2] and p[first + 1][2] == DOWN and
                    p[first + 2][1] == "" and
                    nicks(rows, low_first, low_lines - 1) ==
                    listing(st["names"][st["lower"]], low_lines - 1) and
                    p[low_first + low_lines - 1][1] == "")
        rows, good = a.wait_screen(ok, WAIT, PANEL)
        assert good, "a statusbar under each main window, the panel shows:\n" + \
            "\n".join(f"{i:2} {r!r}" for i, r in enumerate(rows))
        # the statusbar is there, in the main window's columns
        assert "[" in a.screen(0, PANEL)[first + 2], a.screen(0, PANEL)[first + 2]
        # the rows as they were, the statusbars stay
        a.type_and_wait("/window balance")
        layout_two()
        bands("balanced, with the statusbars")

    def bands(what):
        want = {rows_of(name): st["names"][name] for name in ("#one", "#two")}
        return wait_bands(want, what)

    def t_active_moves():
        # either row active: each band keeps its own channel
        for name in ("#one", "#two", st["upper"]):
            goto(name)
            bands(f"{name} active")

    def t_join_lower():
        # a nick joins the lower row's channel: in the lower band only
        lower = st["lower"]
        c = harness.Client(st["port"], "erin")
        st["clients"].append(c)
        c.send(f"JOIN {lower}")
        c.expect(f" JOIN {lower}")
        st["names"][lower] = sorted(st["names"][lower] + ["erin"])
        bands(f"erin joined {lower}, {st['upper']} active")

    def t_other_band_untouched():
        # a window change in the upper row draws the upper band again, not
        # the lower one: none of the nicks only the lower band shows is
        # written to the terminal (the screen is the same either way, so
        # this reads what erssi wrote)
        a = st["alice"]
        upper, lower = st["upper"], st["lower"]
        only_lower = set(st["names"][lower]) - set(st["names"][upper])
        assert only_lower, st["names"]
        goto(upper)
        bands("before the window changes")
        mark = a.mark()
        a.type_and_wait("/window new hide", "/window name tmp")
        assert wn()[0] == "tmp"
        wait_bands({rows_of(upper): [], rows_of(lower): st["names"][lower]}, "tmp in the upper row")
        goto(upper)
        bands("the upper channel back")
        written = a.output_since(mark).decode(errors="replace")
        drawn = [n for n in only_lower if n in written]
        assert not drawn, f"the lower band was drawn again: {drawn}"

    def t_wheel_lower():
        # the wheel over the lower band scrolls that band only
        goto(st["upper"])
        first, lines = rows_of(st["lower"])
        wheel(PANEL + 5, first + 1, True)
        want = {rows_of(st["upper"]): st["names"][st["upper"]],
                (first, lines): st["names"][st["lower"]][-1:]}
        rows = wait_bands(want, "the lower band scrolled down")
        assert panel(rows)[first][2] == UP, panel(rows)[first]
        wheel(PANEL + 5, first + 1, False)
        bands("the lower band scrolled back")

    def t_click_lower():
        # a click on erin in the lower band opens a query with erin (the
        # upper row is active, its channel has no erin)
        goto(st["upper"])
        first, _ = rows_of(st["lower"])
        row = first + st["names"][st["lower"]].index("erin")
        click(PANEL + 5, row)
        # the query is in the upper row then: its band is empty, the lower
        # one stays, and the panel too (nothing is typed before: keys
        # right after the click would make a paste of it)
        want = {rows_of(st["upper"]): [], rows_of(st["lower"]): st["names"][st["lower"]]}
        wait_bands(want, "a click on erin in the lower band")
        got = wn()
        assert got[0] == "erin", f"the click on erin opened no query with erin: {got}"
        goto(st["upper"])
        bands("the upper channel back")

    def t_side_by_side():
        # a row of two main windows side by side shows the active one's
        # channel when it is in that row, else the one next to the panel
        a = st["alice"]
        upper, lower = st["upper"], st["lower"]
        goto(lower)
        a.type_and_wait("/window new -right", "/window name bee")
        assert wn()[0] == "bee"
        empty = {rows_of(upper): st["names"][upper], rows_of(lower): []}
        full = {rows_of(upper): st["names"][upper], rows_of(lower): st["names"][lower]}
        wait_bands(empty, "bee active, right of " + lower)
        goto(lower)
        wait_bands(full, lower + " active, bee right of it")
        goto(upper)
        wait_bands(empty, "the upper row active, bee next to the panel")
        goto("bee")
        a.type_and_wait("/window hide")
        wait_bands(full, "bee hidden")

    def t_upper_no_channel():
        # a new window in the upper row is no channel: its band empty, the
        # lower band still lists its nicks with auto-hide on
        a = st["alice"]
        goto(st["upper"])
        a.type_and_wait("/window new hide", "/window name aye")
        assert wn()[0] == "aye"
        upper, lower = rows_of(st["upper"]), rows_of(st["lower"])
        want = {upper: [], lower: st["names"][st["lower"]]}
        wait_bands(want, "aye in the upper row")
        # the lower row active: the same
        goto(st["lower"])
        wait_bands(want, "aye in the upper row, the lower row active")
        # back in the upper row: the same
        goto("aye")
        wait_bands(want, "aye in the upper row, active again")

    def t_window_close():
        # /WINDOW CLOSE of the window a band shows: the window is gone,
        # its main window shows another one and the band that one's nicks
        # (a click and the wheel there never reach the closed window)
        a = st["alice"]
        lower = st["lower"]
        c = harness.Client(st["port"], "gus")
        st["clients"].append(c)
        c.send("JOIN #three")
        c.expect(" JOIN #three")
        goto(lower)
        a.type_and_wait("/window new hide", "/join -window #three")
        end = time.time() + WAIT
        while wn()[0] != "#three":
            assert time.time() < end, "#three did not open in the lower row"
            time.sleep(0.1)
        upper, low = rows_of(st["upper"]), rows_of(lower)
        wait_bands({upper: [], low: ["alice", "gus"]}, "#three in the lower row")
        a.type_and_wait("/window close")
        shown = wn()[0]
        assert shown != "#three", shown
        want = {upper: [], low: st["names"].get(shown, [])}
        wait_bands(want, f"#three closed, {shown} in the lower row")
        first, lines = low
        click(PANEL + 5, first + lines - 2)
        wheel(PANEL + 5, first + 1, True)
        wheel(PANEL + 5, first + 1, False)
        wait_bands(want, "a click and the wheel in the lower band")
        assert a.proc.poll() is None, "erssi exited"
        # the lower channel back in the lower row
        goto(lower)
        wait_bands({upper: [], low: st["names"][lower]}, f"{lower} back in the lower row")

    def t_close_upper():
        # the upper main window goes: one band over the whole main window,
        # nothing left of the empty band or of the statusbar row
        a = st["alice"]
        goto("aye")
        a.type_and_wait("/window hide")
        mws = mainwindows()
        assert len(mws) == 1, mws
        first, lines, _ = mws[0]
        assert (first, lines) == st["single"], (mws, st["single"])
        wait_bands({(first, lines): st["names"][st["lower"]]}, "the upper main window closed")

    def t_move_click():
        # /WINDOW MOVE UP and a click on the band at once: one write, read
        # as keys one by one (no paste detection), so the click comes
        # right after the command, before any redraw. It is on the window
        # that moved, as the panel shows it right after the move
        a = st["alice"]
        shown = st["lower"]
        other = st["upper"]
        goto(shown)
        a.type_and_wait("/window new split")
        goto(other)
        layout_two()
        upper, lower = st["upper"], st["lower"]
        assert {upper, lower} == {shown, other}, st
        bands("the two rows again")
        goto(lower)
        only = sorted(set(st["names"][lower]) - set(st["names"][upper]))
        assert only, st["names"]
        first, _ = rows_of(upper)
        row = first + st["names"][lower].index(only[0])
        a.type_and_wait("/set paste_detect_time 0")
        a.write_raw(f"/window move up\r\x1b[<0;{PANEL + 6};{row + 1}M\x1b[<0;{PANEL + 6};{row + 1}m".encode())
        a.type_and_wait("/set paste_detect_time 5msecs")
        got = wn()
        assert got[0] == only[0], f"the click on {only[0]} right after /window move up: {got}"

    def t_sanitizers():
        reports = st["alice"].sanitizer_reports() if "alice" in st else []
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_start, t_one_window, t_two_rows, t_statusbar_gap, t_active_moves,
                      t_join_lower, t_other_band_untouched, t_wheel_lower, t_click_lower,
                      t_side_by_side, t_upper_no_channel, t_window_close, t_close_upper,
                      t_move_click, t_sanitizers])
    finally:
        if "alice" in st:
            st["alice"].kill()
        for c in st["clients"]:
            c.close()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
