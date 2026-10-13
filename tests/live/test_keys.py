#!/usr/bin/env python3
"""Key sequences from the terminal reach the key bindings: CSI sequences
that end in a letter (ESC [ I, ESC [ G, ESC [ Z) and ones that end in ~.
Usage: test_keys.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 10

# key name -> the sequences that are bound to it by default
KEYS = {
    "prior": (b"\x1b[I", b"\x1b[5~"),
    "next": (b"\x1b[G", b"\x1b[6~"),
    "stab": (b"\x1b[Z",),
}


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
        time.sleep(2)
        for name in KEYS:
            off = log_size()
            alice.type(f"/bind {name} command echo key-{name} pressed")
            assert wait_log_since(off, f"key-{name} pressed"), f"/bind {name}:\n" + log_since(off)

    def t_keys():
        alice = st["alice"]
        failed = []
        for name, seqs in KEYS.items():
            for seq in seqs:
                off = log_size()
                alice.write_raw(seq)
                time.sleep(0.3)
                # the next line must not be eaten by a sequence left half done
                alice.type(f"/echo after {seq[1:]!r}")
                if not wait_log_since(off, f"after {seq[1:]!r}"):
                    failed.append(f"{seq!r}: the /echo after it was lost")
                elif f"key-{name} pressed" not in log_since(off):
                    failed.append(f"{seq!r}: {name} did not fire")
        assert not failed, "; ".join(failed)

    def t_sanitizers():
        alice = st["alice"]
        alice.quit()
        assert alice.sanitizer_reports() == [], alice.sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_keys, t_sanitizers])
    finally:
        if "alice" in st:
            st["alice"].kill()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
