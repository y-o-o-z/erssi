#!/usr/bin/env python3
"""A restored layout puts every window back in its own main window: after
/LAYOUT SAVE (and /SAVE) and a restart, each main window shows the window
it showed when the layout was saved, the rows stacked and side by side
in the same order, the same main window is active, and the windows have
the numbers they had - with the channels joined again after the connect,
a query, the Notices and Mentions windows and windows stuck to a main
window, the side panels on and off.
Before, a restored layout mixed the windows up: the panels renumber the
windows by their place in the window list whenever one is created, so
the numbers the layout names its windows by named others while it was
being restored.
Usage: test_layout_restore.py <build dir> [<source dir>]"""
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
CHANNELS = ("#one", "#two", "#three")


def window_entries(config_text):
    """{refnum: the text of its entry} of a saved `windows` block (an
    entry is on one line, or up to the line "  };")"""
    m = re.search(r"^windows = \{\n(.*?)^\};", config_text, re.M | re.S)
    assert m, "no windows block saved"
    entries, key, body = {}, None, []
    for line in m.group(1).splitlines():
        start = re.match(r"^  (\d+) = \{(.*)$", line)
        if key is None and start:
            key, rest = int(start.group(1)), start.group(2)
            if rest.rstrip().endswith("};"):  # all on one line
                entries[key], key = rest, None
            else:
                body = [rest]
        elif key is not None:
            if line == "  };":
                entries[key], key = "\n".join(body), None
            else:
                body.append(line)
    return entries


def split_items(text):
    """(the window's own fields, its items) of a `windows` entry"""
    m = re.search(r"items = \((.*?)\);", text, re.S)
    if m is None:
        return text, ""
    return text[:m.start()] + text[m.end():], m.group(1)


def windows_of(config_text):
    """{refnum: what the window is} of a saved `windows` block: its name,
    else its (first) item's name, else ''"""
    out = {}
    for key, text in window_entries(config_text).items():
        own, items = split_items(text)
        name = re.search(r'\bname = "([^"]*)";', own)
        item = re.search(r'\bname = "([^"]*)";', items)
        out[key] = name.group(1) if name else item.group(1) if item else ""
    return out


def sticky_of(config_text):
    """{window: the window active in its main window} of the sticky
    windows of a saved `windows` block (itself when it is that one)"""
    wins = windows_of(config_text)
    out = {}
    for key, text in window_entries(config_text).items():
        own = split_items(text)[0]
        if re.search(r'\bsticky = "yes";', own):
            parent = re.search(r'\bparent = "(\d+)";', own)
            out[wins[key]] = wins[int(parent.group(1))] if parent else wins[key]
    return out


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-layout-restore."))
    st = {"clients": []}

    def log_text(off):
        try:
            return st["alice"].all_log().read_bytes()[off:].decode(errors="replace")
        except OSError:
            return ""

    def wn():
        """the active window: its name, channel or query"""
        a = st["alice"]
        off = a.all_log().stat().st_size
        a.type_and_wait("/wn")
        m = re.search(r"active=(\S*) refnum=(\d+)", log_text(off))
        assert m, "no answer to /wn:\n" + log_text(off)
        return m.group(1)

    def goto(name):
        st["alice"].type_and_wait(f"/window goto {name}")
        got = wn()
        assert got == name, f"window {name} did not become active: {got}"

    def state():
        """/LAYOUT SAVE + /SAVE: (the rows of main windows from the top,
        each from the left, as the windows they show; {window: refnum};
        the sticky windows; the active window)"""
        a = st["alice"]
        a.type_and_wait("/layout save", "/save")
        text = (a.home / "config").read_text()
        wins = windows_of(text)
        layout = block(text, "mainwindows")
        assert layout, "no mainwindows block saved"
        rows = {}
        for key, w in layout.items():
            rows.setdefault(int(w["first_line"]), []).append((int(w["first_column"]), wins.get(int(key), key)))
        shown = [[name for _, name in sorted(rows[line])] for line in sorted(rows)]
        numbers = {name: ref for ref, name in wins.items() if name}
        return shown, numbers, sticky_of(text), wn()

    def joined(off):
        end = time.time() + 15
        while not all(f"has joined {c}" in log_text(off) for c in CHANNELS):
            assert time.time() < end, "alice did not join " + " ".join(CHANNELS) + ":\n" + log_text(off)[-2000:]
            time.sleep(0.1)

    def restart():
        a = st["alice"]
        off = a.all_log().stat().st_size
        a.quit()
        a.config_text = None  # the config it saved
        a.start(200, 50)
        joined(off)
        a.type_and_wait("/alias wn echo active=$winname$C$Q refnum=$winref")

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_start():
        st["ircd"] = harness.Ircd(work)
        port = st["ircd"].start()
        st["port"] = port
        c = harness.Client(port, "bob")
        st["clients"].append(c)
        a = harness.Erssi(st["inst"], work, "alice", harness.config(port, "alice", channels=CHANNELS))
        st["alice"] = a
        a.start(200, 50)
        joined(0)
        a.type_and_wait("/alias wn echo active=$winname$C$Q refnum=$winref")

    def t_build_layout():
        # two stacked rows of three side-by-side windows each:
        # #two | Mentions | the query with bob, above #one | Notices | #three
        a = st["alice"]
        goto("#one")
        a.type_and_wait("/query bob")
        goto("#one")
        a.type_and_wait("/window show #two")
        goto("#two")
        a.type_and_wait("/window show -right Mentions")
        goto("Mentions")
        a.type_and_wait("/window show -right bob")
        goto("#one")
        a.type_and_wait("/window show -right Notices")
        goto("Notices")
        a.type_and_wait("/window show -right #three")
        # #one's main window has #one and E2E (the server window) for its
        # own (sticky), #one in front
        e2e = state()[1]["E2E"]
        goto("#one")
        a.type_and_wait("/window stick", f"/window stick {e2e}")
        goto("Mentions")
        st["want"] = state()
        shown, _, sticky, active = st["want"]
        assert sticky == {"#one": "#one", "E2E": "#one"}, sticky
        print("saved:", st["want"])
        assert sorted(map(sorted, shown)) == sorted(map(sorted, [["#two", "Mentions", "bob"],
                                                                 ["#one", "Notices", "#three"]])), shown
        assert active == "Mentions", active
        # the left panel's selected row: Mentions, after the window change
        rows = left_rows()
        st["selected"] = rows["Mentions"][1]
        assert [w for w, (_, style) in rows.items() if style == st["selected"]] == ["Mentions"], rows

    def left_rows():
        """{window: (its number, the style of its row)} of the left panel
        (its 15 columns, the border in the last one; a row "N. name")"""
        a = st["alice"]
        rows, styles = a.screen(0, 14), a.screen_styles(0, 14)
        out = {}
        for y, text in enumerate(rows):
            m = re.match(r"\s*(\d+)\. (\S+)", text)
            if m:
                out[m.group(2)] = (int(m.group(1)), styles[y][m.start(1)])
        return out

    def wait_selected(name, what):
        """the left panel shows NAME's row, and only that one, in the style
        of the selected row"""
        def ok(_):
            rows = left_rows()
            return (name in rows and
                    [w for w, (_, style) in rows.items() if style == st["selected"]] == [name])
        _, good = st["alice"].wait_screen(ok, WAIT)
        assert good, f"{what}: {name} is not the one selected row: {left_rows()}"
        return left_rows()[name][0]

    def check(what):
        got = state()
        print(what + ":", got)
        assert got == st["want"], f"{what}: {got} != {st['want']}"

    def t_layout_save_restart():
        # /LAYOUT SAVE and /SAVE, a restart
        restart()
        check("after a restart")

    def t_selected_after_restart():
        # the left panel selects the active window after the start (the
        # restore made it active before the panel was there: row 1 was
        # selected)
        wait_selected("Mentions", "after a restart")

    def t_save_restart():
        # a second restart, from the layout a restored session saved
        # (check() saved it with /LAYOUT SAVE), with /SAVE once more
        st["alice"].type_and_wait("/save")
        restart()
        check("after /SAVE and a restart")

    def t_panels_off():
        a = st["alice"]
        a.type_and_wait("/set sidepanel_left off", "/set sidepanel_right off")
        check("the panels off")
        restart()
        check("the panels off, after a restart")

    def t_selected_after_renumber():
        # a new query sorts before Mentions: the windows are renumbered,
        # Mentions has the next number and is still the active window,
        # and the selected row is still Mentions'
        a = st["alice"]
        a.type_and_wait("/set sidepanel_left on", "/set sidepanel_right on")
        before = wait_selected("Mentions", "the panels on again")
        c = harness.Client(st["port"], "cid")
        st["clients"].append(c)
        c.send("PRIVMSG alice :hello")
        _, good = a.wait_screen(lambda _: "cid" in left_rows(), WAIT)
        assert good, f"no window for the query with cid: {left_rows()}"
        assert wn() == "Mentions"
        after = wait_selected("Mentions", "after the windows were renumbered")
        assert after == before + 1, (before, after)

    def t_sanitizers():
        reports = st["alice"].sanitizer_reports() if "alice" in st else []
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_start, t_build_layout, t_layout_save_restart,
                      t_selected_after_restart, t_save_restart, t_panels_off,
                      t_selected_after_renumber, t_sanitizers])
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
