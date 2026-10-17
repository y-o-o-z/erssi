#!/usr/bin/env python3
"""The native e2e module against repartee itself, live (optional).

Skipped (77) unless REPARTEE_BIN names a repartee binary (built from
https://github.com/outragedevs/repartee, e.g. `cargo build --release`).
alice is erssi (this build, tests/live/harness.py), bob is repartee in a
private tmux server, both on the fake server tests/live/ircd.py (127.0.0.1
only; USERHOST answered, so repartee learns its own address).

Checked: the first contact (bob's first encrypted line leaves a line in
alice's channel, one /e2e accept on each side, both read each other);
bob comes back under another ident with the same keys - every line of his
says why it is not read and names /e2e reverify, which alone makes his
next line read; nothing crosses the server in clear text.
Usage: repartee-interop.py <build dir> <source dir>"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SRC = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parents[2])
sys.path.insert(0, str(SRC / "tests" / "live"))
import harness  # noqa: E402

WAIT = 30

RP_CONFIG = """[general]
nick = "{nick}"
username = "{user}"
realname = "repartee test"
flood_protection = false

[image_preview]
enabled = false

[web]
enabled = false

[emotes]
enabled = false

[spellcheck]
enabled = false

[shrink]
enabled = false

[translate]
enabled = false

[e2e]
enabled = true
default_mode = "normal"

[servers.test]
label = "Test"
address = "127.0.0.1"
port = {port}
tls = false
autoconnect = true
channels = ["#test"]
auto_reconnect = false
"""


def main():
    build = Path(sys.argv[1]).resolve()
    rp_bin = os.environ.get("REPARTEE_BIN", "")
    if not rp_bin or not os.access(rp_bin, os.X_OK):
        print("SKIP: set REPARTEE_BIN to a repartee binary to run this test")
        sys.exit(77)
    if shutil.which("tmux") is None:
        print("SKIP: tmux not found")
        sys.exit(77)
    work = Path(tempfile.mkdtemp(prefix="e2e-repartee."))
    sock = f"e2e-repartee-{os.getpid()}"
    st = {"ircd": None, "alice": None, "rp_pids": []}

    def tmux(*args, check=True):
        return subprocess.run(["tmux", "-L", sock] + list(args), capture_output=True, text=True,
                              check=check)

    def rp_screen():
        return tmux("capture-pane", "-p", "-t", "bob", "-S", "-200", check=False).stdout

    def rp_type(text):
        tmux("send-keys", "-t", "bob", "-l", "--", text)
        tmux("send-keys", "-t", "bob", "Enter")
        time.sleep(1)

    def rp_wait(text, timeout=WAIT):
        return harness.wait_until(lambda: text in rp_screen(), timeout, 0.3)

    def rp_start(user):
        home = work / "home-bob"
        (home / ".repartee").mkdir(parents=True, exist_ok=True)
        os.chmod(home, 0o700)
        os.chmod(home / ".repartee", 0o700)
        (home / ".repartee" / "config.toml").write_text(
            RP_CONFIG.format(nick="bob", user=user, port=st["port"]))
        tmux("new-session", "-d", "-s", "bob", "-x", "200", "-y", "50",
             f"env -i PATH=/usr/bin:/bin HOME={home} TERM=xterm-256color LANG=C.UTF-8 "
             f"LC_ALL=C.UTF-8 {rp_bin}")
        assert rp_wait("has joined #test"), rp_screen()[-2000:]
        time.sleep(1)
        st["rp_pids"] += [int(p.stem) for p in (home / ".repartee" / "sessions").glob("*.sock")]

    def rp_stop():
        rp_type("/quit")
        for pid in st["rp_pids"]:
            for _ in range(40):
                if not Path(f"/proc/{pid}").exists():
                    break
                time.sleep(0.25)
            else:
                try:
                    os.kill(pid, 15)
                except ProcessLookupError:
                    pass
        st["rp_pids"] = []
        tmux("kill-session", "-t", "bob", check=False)

    def chan_log():
        try:
            return (st["alice"].home / "logs" / "E2E" / "#test.log").read_text(errors="replace")
        except OSError:
            return ""

    def wait_chan(text, start=0, timeout=WAIT):
        return harness.wait_until(lambda: text in chan_log()[start:], timeout)

    def say(text):
        st["alice"].type_and_wait(f"/msg #test {text}")

    def t_install():
        st["inst"] = harness.install(build, work)
        if not list(st["inst"].moddir.glob("libe2e_core.so")):
            raise harness.Skip("erssi built without e2e")
        st["ircd"] = harness.Ircd(work, ["--nick-rules"])
        st["port"] = st["ircd"].start()

    def t_first_contact():
        a = st["alice"] = harness.Erssi(st["inst"], work, "alice", harness.config(st["port"], "alice"))
        a.start()
        a.wait_ready()
        assert a.wait_file(a.all_log(), "has joined #test", WAIT)
        a.type_and_wait("/load e2e", "/window goto #test", "/e2e on")
        rp_start("bob")
        rp_type("/e2e on")
        rp_type("first words from repartee")
        assert wait_chan("[E2E: encrypted message not read - no key from bob (bob@127.0.0.1) yet"), \
            chan_log()[-2000:]
        assert rp_wait("Pending key exchange from alice"), rp_screen()[-2000:]
        rp_type("/e2e accept alice")
        assert a.wait_file(a.all_log(), "Pending key exchange from bob", WAIT), "no request from bob"
        a.type_and_wait("/window goto #test", "/e2e accept bob")
        time.sleep(2)
        rp_type("can you read me")
        assert wait_chan("can you read me"), chan_log()[-2000:]
        say("yes I can read you")
        assert rp_wait("yes I can read you"), rp_screen()[-2000:]

    def t_new_address():
        rp_stop()
        start = len(chan_log())
        rp_start("bobnew")
        rp_type("after the reconnect")
        assert wait_chan("/e2e reverify bobnew@127.0.0.1", start), chan_log()[start:]
        rp_type("second after the reconnect")
        assert wait_chan("now comes from a new address, bobnew@127.0.0.1", start), chan_log()[start:]
        off = len(st["alice"].all_log().read_text(errors="replace"))
        st["alice"].type_and_wait("/window goto #test", "/e2e reverify bobnew@127.0.0.1")
        out = st["alice"].all_log().read_text(errors="replace")[off:]
        assert "is bobnew@127.0.0.1 now" in out, out
        mid = len(chan_log())
        rp_type("read after reverify")
        assert wait_chan("read after reverify", mid), chan_log()[mid:]
        assert "not read" not in chan_log()[mid:], chan_log()[mid:]

    def t_wire():
        wire = (work / "wire.log").read_text(errors="replace")
        for text in ("first words", "can you read me", "yes I can", "after the reconnect",
                     "read after reverify"):
            assert text not in wire, text
        assert "+RPE2E01" in wire

    def cleanup():
        if st["rp_pids"]:
            try:
                rp_stop()
            except Exception:
                for pid in st["rp_pids"]:
                    try:
                        os.kill(pid, 15)
                    except ProcessLookupError:
                        pass
        tmux("kill-server", check=False)
        if st["alice"] is not None:
            st["alice"].kill()
        if st["ircd"] is not None:
            st["ircd"].stop()

    try:
        harness.main([t_install, t_first_contact, t_new_address, t_wire])
    finally:
        cleanup()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
