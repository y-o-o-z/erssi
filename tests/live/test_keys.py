#!/usr/bin/env python3
"""Key sequences from the terminal reach the key bindings: CSI sequences
that end in an upper or lower case letter, in @ or in ~, and the Linux
console's ESC [ [ A; none of them swallows the keys after it. Mouse (SGR)
and bracketed paste still go to their own parsers.
Usage: test_keys.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10

# key name -> the sequences that are bound to it by default (a key name),
# or the sequence itself (meta2-...: no default binding, bound here)
KEYS = {
    "prior": (b"\x1b[I", b"\x1b[5~"),
    "next": (b"\x1b[G", b"\x1b[6~"),
    "stab": (b"\x1b[Z",),
    "home": (b"\x1b[7~",),  # rxvt
    "end": (b"\x1b[8~",),  # rxvt
    "cleft": (b"\x1b[d",),  # rxvt shift-left
    "meta2-a": (b"\x1b[a",),  # rxvt shift-up
    "meta2-@": (b"\x1b[@",),
    "meta2-[A": (b"\x1b[[A",),  # Linux console F1
}
# sequences with no binding: only the line typed after them is checked
UNBOUND = (b"\x1b[97;5u",)  # CSI u (kitty keyboard protocol): ctrl-a


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-keys."))
    st = {}

    def log_size():
        try:
            return st["alice"].all_log().stat().st_size
        except OSError:
            return 0

    def log_since(offset):
        return st["alice"].all_log().read_bytes()[offset:].decode(errors="replace")

    def wait_log_since(offset, text):
        end = time.time() + WAIT
        while True:
            if text in log_since(offset):
                return True
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_start():
        alice = harness.Erssi(st["inst"], work, "alice", harness.config(1, "alice"), args=("--noconnect",))
        st["alice"] = alice
        alice.start()
        alice.wait_ready()
        for name in KEYS:
            off = log_size()
            alice.type_and_wait(f"/bind {name} command echo key-{name} pressed")
            assert wait_log_since(off, f"key-{name} pressed"), f"/bind {name}:\n" + log_since(off)

    def type_keys(text):
        """text one key at a time: keys, not a paste (a paste does not go
        through the escape sequence parser). The gaps between the keys are
        the point: they stay."""
        for ch in text:
            st["alice"].write_raw(ch.encode())
            time.sleep(0.03)

    def line_after(seq, n):
        """The keys typed after seq are not swallowed: ^U clears whatever
        an unbound sequence left in the input line, then a /echo."""
        off = log_size()
        st["alice"].write_raw(seq)
        time.sleep(0.3)  # read apart from what follows (a lone ESC times out)
        type_keys(f"\x15/echo after key {n}\r")
        return off, wait_log_since(off, f"after key {n}")

    def t_keys():
        failed = []
        n = 0
        for name, seqs in KEYS.items():
            for seq in seqs:
                n += 1
                off, ok = line_after(seq, n)
                if not ok:
                    failed.append(f"{seq!r}: the keys after it were lost")
                elif f"key-{name} pressed" not in log_since(off):
                    failed.append(f"{seq!r}: {name} did not fire")
        for seq in UNBOUND:
            n += 1
            if not line_after(seq, n)[1]:
                failed.append(f"{seq!r}: the keys after it were lost")
        assert not failed, "; ".join(failed)

    def t_mouse_and_paste():
        # an SGR click in the text area and a bracketed paste: no bytes of
        # either reach the input line as keys
        alice = st["alice"]
        off = log_size()
        alice.write_raw(b"\x1b[<0;120;20M\x1b[<0;120;20m")
        time.sleep(0.5)  # read apart from the keys: typed at once they are a paste
        type_keys("/echo after mouse\r")
        assert wait_log_since(off, "after mouse"), "the line after the click was lost:\n" + log_since(off)
        assert "<0;" not in log_since(off) and "120;20" not in log_since(off), log_since(off)
        off = log_size()
        alice.write_raw(b"\x1b[200~/echo pasted line\x1b[201~")
        time.sleep(0.5)  # the paste read apart from the Enter after it
        alice.write_raw(b"\r")
        assert wait_log_since(off, "pasted line"), "the pasted line was not sent:\n" + log_since(off)
        text = log_since(off)
        assert "200~" not in text and "201~" not in text and "/echo" not in text, text

    def t_sanitizers():
        alice = st["alice"]
        alice.quit()
        assert alice.sanitizer_reports() == [], alice.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_keys, t_mouse_and_paste, t_sanitizers])
    finally:
        if "alice" in st:
            st["alice"].kill()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
