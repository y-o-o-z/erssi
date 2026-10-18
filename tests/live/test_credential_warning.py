#!/usr/bin/env python3
"""The warning about the old credential encryption: printed once when erssi
starts with a configuration that still has it on (or holds encrypted
values), not again at a reconnect; a configuration without it gets none.
Usage: test_credential_warning.py <build dir> [<source dir>]"""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 15
WARNING = "Credentials: this configuration still uses the old credential encryption"


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-credential-warning."))
    st = {"ircds": [], "erssis": []}

    def ircd(name):
        d = work / name
        d.mkdir()
        srv = harness.Ircd(d, [])
        st["ircds"].append(srv)
        return srv.start()

    def erssi(name, port, core_settings="", chatnet="", before=None):
        cfg = harness.config(port, "alice")
        if core_settings:
            assert "core = { real_name" in cfg, cfg
            cfg = cfg.replace("core = { real_name", "core = { %s real_name" % core_settings, 1)
        if chatnet:
            assert 'E2E = { type = "IRC"; };' in cfg, cfg
            cfg = cfg.replace('E2E = { type = "IRC"; };', 'E2E = { type = "IRC"; %s };' % chatnet)
        e = harness.Erssi(st["inst"], work, name, cfg)
        st["erssis"].append(e)
        if before is not None:
            e.home.mkdir(parents=True, exist_ok=True)
            before(e)
        e.start()
        assert e.wait_file(e.all_log(), "has joined #test", WAIT), f"{name} never joined #test"
        return e

    def log(e):
        return e.all_log().read_text(errors="replace")

    def t_install():
        st["inst"] = harness.install(build, work)

    def t_once_per_start():
        """credential_config_encrypt on: the warning with what is protected
        and the commands, once; a reconnect does not print it again"""
        a = erssi("encrypt", ircd("encrypt"), 'credential_config_encrypt = "yes";')
        assert a.wait_file(a.all_log(), WARNING, WAIT), log(a)
        a.type_and_wait("/echo started")
        text = log(a)
        assert text.count(WARNING) == 1, text
        assert "is a plain-text file, readable" in text, text
        assert "/CREDENTIAL PASSWD <master password>, /CREDENTIAL DECRYPT, /SAVE." in text, text
        a.type_and_wait("/reconnect")
        assert harness.wait_until(lambda: log(a).count("has joined #test") >= 2, WAIT), log(a)
        a.type_and_wait("/echo reconnected")
        assert log(a).count(WARNING) == 1, log(a)
        a.quit()
        assert a.sanitizer_reports() == [], a.sanitizer_reports()

    def t_encrypted_value():
        """the feature off, a value still encrypted: warned, the password is
        to be entered again"""
        a = erssi("value", ircd("value"),
                  chatnet='sasl_password = "erssi-enc1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";')
        assert a.wait_file(a.all_log(), WARNING, WAIT), log(a)
        assert "enter each encrypted password again" in log(a), log(a)
        a.quit()
        assert a.sanitizer_reports() == [], a.sanitizer_reports()

    def t_plain_no_warning():
        a = erssi("plain", ircd("plain"))
        a.type_and_wait("/echo started")
        assert WARNING not in log(a), log(a)
        a.quit()
        assert a.sanitizer_reports() == [], a.sanitizer_reports()

    def t_percent_in_path():
        """a % in the file name is printed as one %"""
        def directory(e):
            (e.home / "cr%sx").mkdir()

        a = erssi("percent", ircd("percent"),
                  'credential_storage_mode = "external"; credential_external_file = "cr%sx";',
                  before=directory)
        assert a.wait_file(a.all_log(), "cr%sx: a directory", WAIT), log(a)
        assert "%%" not in log(a), log(a)
        a.quit()
        assert a.sanitizer_reports() == [], a.sanitizer_reports()

    def cleanup():
        for e in st["erssis"]:
            e.kill()
        for s in st["ircds"]:
            s.stop()

    try:
        harness.main([t_install, t_once_per_start, t_encrypted_value, t_plain_no_warning,
                      t_percent_in_path])
    finally:
        cleanup()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
