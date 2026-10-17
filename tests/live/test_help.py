#!/usr/bin/env python3
"""/HELP for the fork's own settings and for $nickmode.
Usage: test_help.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

# topic -> first line of its Description section
TOPICS = {
    "term_truecolor": "Chooses whether erssi sends 24-bit colours to the terminal.",
    "colors_ansi_24bit": "Sends 24-bit colours to the terminal where term_truecolor allows them.",
    "print_whois_rpl_in_server_window": "Prints WHOIS and WHOWAS replies in the network's status window.",
    "show_names_on_join_limit": "Limits the nick list that is printed when you join a channel.",
    "nick_mode_color": "Colours the mode prefix that $nickmode puts before a nick.",
    "nick_mode_color_owner": "Sets the colour of the owner prefix (~ or &) that $nickmode prints.",
    "nick_mode_color_op": "Sets the colour of the op prefix (@) that $nickmode prints.",
    "nick_mode_color_halfop": "Sets the colour of the halfop prefix (%) that $nickmode prints.",
    "nick_mode_color_voice": "Sets the colour of the voice prefix (+) that $nickmode prints.",
    "nick_mode_color_normal": "Sets the colour of the blank $nickmode prints for a nick without a mode.",
    "nickmode": "$nickmode in a message format is the sender's mode prefix",
    "term_notify": "Shows a desktop notification through the terminal for every mention",
    "sidepanel_debug": "Writes a debug log of the side panels and the mouse",
    "resize_debug": "Writes a debug log of every terminal resize",
    "sidepanel_left_width": "The width in columns of the left side panel",
    "sidepanel_right_width": "The width in columns of the right side panel",
}


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-help."))
    inst = None

    def t_install():
        nonlocal inst
        inst = harness.install(build, work)

    def t_help():
        w = Path(tempfile.mkdtemp(prefix="help.", dir=work))
        with harness.Erssi(inst, w, "alice", harness.config(1, "alice"), args=("--noconnect",)) as alice:
            alice.start()
            alice.wait_ready()
            log = alice.all_log()
            # the compiled-in help path is the real prefix, not this install
            alice.type_and_wait(f"/set help_path {inst.helpdir}")
            assert alice.wait_file(log, str(inst.helpdir), 10), "help_path not set"
            for topic, line in TOPICS.items():
                alice.type_and_wait(f"/help {topic}")
                assert alice.wait_file(log, line, 10), f"/help {topic}: no '{line}'"
            alice.type_and_wait("/help $nickmode")
            assert alice.wait_file(log, "$nickmode in a message format", 10), "/help $nickmode"
            alice.type_and_wait("/help nosuchtopic_x")
            assert alice.wait_file(log, "No help for nosuchtopic_x", 10), "unknown topic"
            alice.quit()
            assert alice.sanitizer_reports() == [], alice.sanitizer_reports()

    try:
        harness.main([t_install, t_help])
    finally:
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
