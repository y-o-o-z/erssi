#!/usr/bin/env python3
"""OTR fragment reassembly in otr_receive()/enqueue_otr_fragment().

A peer sends a private message in one or more "?OTR:...." fragments; the
last ends with a ".". erssi stitches them back together before handing the
result to libotr. These cases drive that path against the project's
disposable localhost IRC server, with the OTR module loaded by hand (no
established, authenticated OTR session is needed to reach the code):

- an unfinished fragment followed by an empty PRIVMSG must not read before
  the start of the message (the 2026.11 heap over-read at otr.c:641);
- an empty PRIVMSG with nothing pending is harmless;
- a message split across several fragments is reassembled and erssi stays
  responsive;
- a peer that never ends the message cannot grow the queue without bound,
  and erssi recovers and reassembles the next message afterwards.

Every case ends by checking erssi still answers an /echo, and the run ends
with no sanitizer report. Built with -Db_sanitize=address the empty-PRIVMSG
case aborts before the fix; without a sanitizer it is a plain over-read, so
this test is most valuable under ASan.
Usage: test_otr.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 30


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-otr."))
    st = {}

    def log():
        try:
            return st["alice"].all_log().read_text(errors="replace")
        except OSError:
            return ""

    def wait_log(text, start=0, timeout=WAIT):
        return bool(harness.wait_until(lambda: text in log()[start:], timeout))

    def responsive(tag):
        """erssi still runs what is typed: an /echo of a fresh marker shows
        up in the all log. Fails if the client crashed or hung."""
        mark = f"otr-alive-{tag}-{time.time()}"
        off = len(log())
        st["alice"].write_raw(f"/echo {mark}\r".encode())
        assert wait_log(mark, off), f"erssi did not answer after {tag}:\n" + log()[off:]
        assert st["alice"].proc.poll() is None, f"erssi exited after {tag}"

    def t_install():
        st["inst"] = harness.install(build, work)
        if not list(st["inst"].root.rglob("libotr_core.so")):
            raise harness.Skip("this build has no OTR module")

    def t_start():
        ircd = harness.Ircd(work)
        st["ircd"] = ircd
        port = ircd.start()
        st["port"] = port
        e = harness.Erssi(st["inst"], work, "alice", harness.config(port, "alice"))
        st["alice"] = e
        e.start()
        e.wait_ready()
        off = len(log())
        e.type_and_wait("/load otr")
        # the module announces itself; if it refused to load there is no
        # point going on - report it rather than test nothing
        assert harness.wait_until(
            lambda: "Irssi OTR" in log()[off:] or "otr" in log()[off:].lower(), 10), \
            "the OTR module did not load:\n" + log()[off:]
        st["bob"] = harness.Client(port, "bob")

    def t_empty_after_fragment():
        """the over-read case: start a fragment, then an empty PRIVMSG"""
        bob = st["bob"]
        bob.send("PRIVMSG alice :?OTR:AAAA")
        responsive("first-fragment")
        bob.send("PRIVMSG alice :")  # empty trailing, msg_len == 0
        responsive("empty-after-fragment")

    def t_empty_no_buffer():
        """an empty PRIVMSG with nothing pending, then a plain message"""
        bob = st["bob"]
        bob.send("PRIVMSG alice :")
        responsive("empty-no-buffer")
        bob.send("PRIVMSG alice :just a normal line")
        responsive("normal-after-empty")

    def t_multi_fragment():
        """a message split over two fragments is stitched and passed on
        (libotr rejects the bytes, but the reassembly itself must not
        crash and erssi must stay responsive)"""
        bob = st["bob"]
        bob.send("PRIVMSG alice :?OTR:AAAA")
        bob.send("PRIVMSG alice :BBBB.")  # ends with the OTR end tag
        responsive("multi-fragment")

    def t_overflow_then_recover():
        """a peer that never ends the message cannot grow the queue past
        OTR_MSG_MAX_SIZE (512 KiB); erssi drops it, stays alive, and still
        reassembles the next, properly ended message"""
        bob = st["bob"]
        chunk = "A" * 40000
        bob.send("PRIVMSG alice :?OTR:" + chunk)  # begins the queue
        for _ in range(15):  # ~600 KiB of fragments, none ending in "."
            bob.send("PRIVMSG alice :" + chunk)
        responsive("overflow")
        # recovery: a fresh, complete sequence is reassembled as usual
        bob.send("PRIVMSG alice :?OTR:CCCC")
        bob.send("PRIVMSG alice :DDDD.")
        responsive("recover-after-overflow")

    def t_quit():
        st["alice"].quit()
        assert st["alice"].sanitizer_reports() == [], \
            "sanitizer report(s): " + repr(st["alice"].sanitizer_reports())

    try:
        harness.main([t_install, t_start, t_empty_after_fragment, t_empty_no_buffer,
                      t_multi_fragment, t_overflow_then_recover, t_quit])
    finally:
        if "alice" in st:
            st["alice"].kill()
        if "bob" in st:
            st["bob"].close()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
