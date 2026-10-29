#!/usr/bin/env python3
"""Clickable addresses (OSC 8, term_hyperlinks) in a real erssi with its side
panels:
- auto: on in a terminal known to show them (kitty here), off in one that
  is not known (xterm-256color) and under GNU screen (STY).
- A long address that wraps over several lines of the main window is one
  link: every piece is opened with the same id and the whole address, and
  closed before the cursor moves, so nothing else (the side panels, the
  statusbar, the input line, cleared cells) is ever part of it. Only SGR
  (colours) comes between an open and its close.
- The same after /redraw, after a resize and after /clear.
- /set term_hyperlinks off: none at all.
- An address someone sends with an escape sequence in it cannot bring its
  own sequence.
Usage: test_hyperlinks.py <build dir> [<source dir>]"""
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10
COLS, ROWS = 90, 30
URL = "https://example.org/" + "a" * 50 + "/" + "b" * 50 + "?q=1&r=(x)"
KITTY = {"TERM": "xterm-kitty", "KITTY_WINDOW_ID": "1"}

TOKEN = re.compile(rb"\x1b\]8;([^;\x07\x1b]*);([^\x07\x1b]*)\x1b\\"   # OSC 8
                   rb"|\x1b\[[0-9;?]*[ -/]*[@-~]"                       # CSI
                   rb"|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)"               # another OSC
                   rb"|\x1b[^\[\]]"                                      # other ESC
                   rb"|[^\x1b]+", re.S)


def link_runs(out):
    """[(id, uri, text)] for every link in the output, after checking that
    each is closed before anything but text and SGR, and that no link is
    open at the end."""
    runs, cur = [], None
    for m in TOKEN.finditer(out):
        tok = m.group(0)
        if tok.startswith(b"\x1b]8;"):
            params, uri = m.group(1), m.group(2)
            if uri:
                assert cur is None, f"a link opened inside another: {tok!r}"
                assert params.startswith(b"id=") and re.fullmatch(rb"id=[A-Za-z0-9]+", params), params
                assert re.fullmatch(rb"[\x21-\x7e]+", uri), uri
                cur = [params[3:].decode(), uri.decode(), b""]
            else:
                assert cur is not None, "a link closed that was not open"
                runs.append((cur[0], cur[1], cur[2].decode(errors="replace")))
                cur = None
        elif tok.startswith(b"\x1b"):
            if cur is not None:
                assert tok.startswith(b"\x1b[") and tok.endswith(b"m"), \
                    f"{tok!r} inside the link {cur[1]!r} after {cur[2]!r}"
        elif cur is not None:
            text = tok.decode(errors="replace")
            assert "\x07" not in text and not any(0x80 <= ord(c) <= 0x9f for c in text), tok
            cur[2] += tok
    assert cur is None, f"the link {cur[1]!r} left open after {cur[2]!r}"
    return runs


def check_url_drawn(out, url):
    """The pieces of url drawn in out: one id, the whole address as the
    target of each, and their text the address (a draw may draw it more
    than once)."""
    runs = [r for r in link_runs(out) if r[1] == url]
    assert runs, f"no link to {url!r} in {out[-2000:]!r}"
    ids = {r[0] for r in runs}
    assert len(ids) == 1, ids
    text = "".join(r[2] for r in runs)
    assert url in text, (text, url)
    assert len(runs) >= 2, f"the address did not wrap: {runs}"
    for r in runs:
        assert r[2] and r[2] in url, r
    return runs


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-hyperlinks."))
    st = {"all": []}

    def log_text(e):
        try:
            return e.all_log().read_text(errors="replace")
        except OSError:
            return ""

    def new(name, env):
        e = harness.Erssi(st["inst"], work, name, harness.config(st["port"], name), env=env)
        st["all"].append(e)
        e.start(COLS, ROWS)
        e.wait_ready()
        assert harness.wait_until(lambda: "has joined #test" in log_text(e), 15), f"{name} never joined #test"
        e.type_and_wait("/window goto #test")
        return e

    def say(e, text):
        """bob says text in #test; returns the output it drew"""
        e.sync_output()
        mark = e.mark()
        st["bob"].send(f"PRIVMSG #test :{text}")
        shown = text.split("\x1b")[0][:20]
        assert harness.wait_until(lambda: shown in log_text(e), WAIT), f"{e.name} did not get {text!r}"
        e.sync_output()
        return e.output_since(mark)

    def redraw(e):
        e.sync_output()
        mark = e.mark()
        e.type_and_wait("/redraw")
        e.sync_output()
        return e.output_since(mark)

    def t_install():
        st["inst"] = harness.install(build, work)
        st["ircd"] = harness.Ircd(work)
        st["port"] = st["ircd"].start()
        st["bob"] = harness.Client(st["port"], "bob")
        st["bob"].send("JOIN #test")
        st["bob"].expect("JOIN")

    def t_auto_on_kitty():
        e = new("kitty", KITTY)
        st["kitty"] = e
        out = say(e, "look " + URL + ".")
        check_url_drawn(out, URL)
        # the sentence's full stop is not part of it
        assert all(not r[1].endswith(".") for r in link_runs(out))
        # the panels and everything else: never inside a link
        out = redraw(e)
        check_url_drawn(out, URL)
        rows = e.screen()
        assert any("look" in r for r in rows), rows

    def t_resize_and_clear():
        e = st["kitty"]
        e.sync_output()
        mark = e.mark()
        e.resize(COLS - 17, ROWS - 4)
        assert harness.wait_until(lambda: URL[-10:].encode() in e.output_since(mark), WAIT)
        e.sync_output()
        check_url_drawn(e.output_since(mark), URL)
        e.sync_output()
        mark = e.mark()
        e.type_and_wait("/clear")
        e.sync_output()
        link_runs(e.output_since(mark))
        out = redraw(e)
        assert [r for r in link_runs(out) if r[1] == URL] == [], "a cleared line is still drawn"

    def t_evil():
        e = st["kitty"]
        out = say(e, "x https://e.example/\x1b]8;;https://evil.example/\x1b\\y\x07 z")
        runs = link_runs(out)
        assert runs, out
        for _, uri, _ in runs:
            assert "\x1b" not in uri and "\x07" not in uri, uri
        assert b"\x1b]8;;https://evil" not in out, out
        assert b"\x07" not in out, out

    def t_setting_off():
        e = st["kitty"]
        e.type_and_wait("/clear")
        say(e, URL)
        e.type_and_wait("/set term_hyperlinks off")
        out = redraw(e)
        assert b"\x1b]8;" not in out, out
        # still drawn, as plain text
        text = harness.ANSI.sub("", out.decode(errors="replace"))
        assert "https://example.org/" in text and "?q=1&r=(x)" in text, text
        e.type_and_wait("/set term_hyperlinks on")
        check_url_drawn(redraw(e), URL)
        e.quit()

    def t_auto_off_unknown():
        e = new("xterm", {})
        out = say(e, URL) + redraw(e)
        assert b"\x1b]8;" not in out, out
        e.type_and_wait("/set term_hyperlinks on")
        check_url_drawn(redraw(e), URL)
        e.quit()

    def t_auto_off_screen():
        env = dict(KITTY)
        env["STY"] = "1.pts-0.host"
        e = new("screen", env)
        out = say(e, URL) + redraw(e)
        assert b"\x1b]8;" not in out, out
        e.quit()

    def t_sanitizers():
        reports = [p for e in st["all"] for p in e.sanitizer_reports()]
        for p in reports:
            print(p.read_text(errors="replace")[:4000], file=sys.stderr)
        assert reports == [], reports

    try:
        harness.main([t_install, t_auto_on_kitty, t_resize_and_clear, t_evil, t_setting_off,
                      t_auto_off_unknown, t_auto_off_screen, t_sanitizers])
    finally:
        for e in st["all"]:
            e.kill()
        if "bob" in st:
            st["bob"].close()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
