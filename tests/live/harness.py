"""Harness for erssi live tests: the build installed under a temporary
directory, a fake IRC server (ircd.py), raw IRC clients and erssi instances
running in a pty, each with its own HOME and --home.

A test script calls main([test, ...]); it prints one line per test and exits
0 (all ok), 1 (a failure) or 77 (Skip raised: the reason is printed)."""
import codecs
import fcntl
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
import traceback
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
ANSI = re.compile(r"\x1b(?:\[[0-9;?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\)|[()][0-9A-Za-z]|[=>78])")


class Skip(Exception):
    """The test cannot run here (missing tool or feature)."""


class SkipCase(Exception):
    """One test case cannot run here; the rest go on. Reported as SKIP."""


# an empty cell of the Screen: a space in the default style
BLANK = (" ", ())


class Screen:
    """What a terminal of COLS x ROWS shows after an output stream: the
    part of xterm that erssi (term-ansi.c) uses - cursor moves, clearing
    the screen and to the end of a line, scroll regions with LF and ESC M,
    lines and characters inserted or deleted - and the style (SGR: colours,
    bold, reverse, ...) each character was written in. Modes, OSC and DCS
    strings are read and dropped. A wide character takes two cells, the
    second one empty. A cell is (character, style)."""

    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.cells = [[BLANK] * cols for _ in range(rows)]
        self.x = self.y = 0
        self.top, self.bottom = 0, rows - 1
        self.wrap = False
        self.saved = (0, 0)
        self.last = " "
        self.style = ()
        self._attrs = {}
        self._dec = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self._rest = ""

    def resize(self, cols, rows):
        self.cells = [(row + [BLANK] * cols)[:cols] for row in self.cells[:rows]]
        self.cells += [[BLANK] * cols for _ in range(rows - len(self.cells))]
        self.cols, self.rows = cols, rows
        self.top, self.bottom = 0, rows - 1
        self.x, self.y = min(self.x, cols - 1), min(self.y, rows - 1)
        self.saved = (min(self.saved[0], cols - 1), min(self.saved[1], rows - 1))
        self.wrap = False

    def lines(self, first=0, end=None):
        """Every row as text, columns FIRST to END - 1 (the cells, not the
        characters: a wide character counts two)."""
        return ["".join(c for c, _ in row[first:end]) for row in self.cells]

    def styles(self, first=0, end=None):
        """Every row's cells' styles, columns FIRST to END - 1: what the
        SGR sequences before a cell's character set (see _sgr), () for
        the terminal's default"""
        return [[style for _, style in row[first:end]] for row in self.cells]

    def _blank(self):
        return [BLANK] * self.cols

    def _scroll(self, n, top=None):
        """n lines up (n < 0: down) inside the scroll region, or from
        row TOP to its bottom"""
        top = self.top if top is None else top
        for _ in range(abs(n)):
            if n > 0:
                del self.cells[top]
                self.cells.insert(self.bottom, self._blank())
            else:
                del self.cells[self.bottom]
                self.cells.insert(top, self._blank())

    def _linefeed(self):
        if self.y == self.bottom:
            self._scroll(1)
        elif self.y < self.rows - 1:
            self.y += 1

    def _put(self, ch):
        if unicodedata.combining(ch) or ch in "‍️":
            return
        w = 2 if unicodedata.east_asian_width(ch) in "WF" else 1
        if self.wrap or self.x + w > self.cols:
            self.x, self.wrap = 0, False
            self._linefeed()
        self.cells[self.y][self.x] = (ch, self.style)
        if w == 2 and self.x + 1 < self.cols:
            self.cells[self.y][self.x + 1] = ("", self.style)
        self.last = ch
        self.x += w
        if self.x >= self.cols:
            self.x, self.wrap = self.cols - 1, True

    # SGR codes that set one attribute on, and the code that turns it off
    _ON = {1: ("bold", 22), 2: ("dim", 22), 3: ("italic", 23), 4: ("underline", 24),
           5: ("blink", 25), 7: ("reverse", 27)}

    def _sgr(self, codes):
        """The style after SGR CODES: a sorted tuple of (attribute, value),
        the colours as their codes ("38;5;3", "31")"""
        attrs = self._attrs
        i = 0
        while i < len(codes):
            c = codes[i]
            if c == 0:
                attrs.clear()
            elif c in self._ON:
                attrs[self._ON[c][0]] = True
            elif c in (22, 23, 24, 25, 27):
                for name, off in self._ON.values():
                    if off == c:
                        attrs.pop(name, None)
            elif 30 <= c <= 37 or 90 <= c <= 97:
                attrs["fg"] = str(c)
            elif 40 <= c <= 47 or 100 <= c <= 107:
                attrs["bg"] = str(c)
            elif c == 39:
                attrs.pop("fg", None)
            elif c == 49:
                attrs.pop("bg", None)
            elif c in (38, 48) and i + 1 < len(codes):
                n = 3 if codes[i + 1] == 5 else 5 if codes[i + 1] == 2 else 1
                attrs["fg" if c == 38 else "bg"] = ";".join(str(v) for v in codes[i:i + n])
                i += n - 1
            i += 1
        self.style = tuple(sorted(attrs.items()))

    def feed(self, data):
        text = self._rest + self._dec.decode(data)
        self._rest = ""
        i, n = 0, len(text)
        while i < n:
            ch = text[i]
            if ch == "\x1b":
                end = self._escape(text, i)
                if end is None:  # cut in the middle: the rest comes later
                    self._rest = text[i:]
                    return
                i = end
                continue
            i += 1
            if ch == "\r":
                self.x, self.wrap = 0, False
            elif ch in "\n\x0b\x0c":
                self.wrap = False
                self._linefeed()
            elif ch == "\b":
                self.x, self.wrap = max(0, self.x - 1), False
            elif ch == "\t":
                self.x = min(self.cols - 1, (self.x // 8 + 1) * 8)
            elif ch >= " " and ch != "\x7f":
                self._put(ch)

    def _escape(self, text, i):
        """Handle the sequence at text[i] (ESC); the index after it, None
        when it is not complete yet."""
        if i + 1 >= len(text):
            return None
        kind = text[i + 1]
        if kind == "[":
            j = i + 2
            while j < len(text) and not "\x40" <= text[j] <= "\x7e":
                j += 1
            if j >= len(text):
                return None
            self._csi(text[i + 2:j], text[j])
            return j + 1
        if kind in "]PX^_":  # a string, to BEL or ST
            j = i + 2
            while j < len(text):
                if text[j] == "\x07" and kind == "]":
                    return j + 1
                if text[j] == "\x1b" and j + 1 < len(text) and text[j + 1] == "\\":
                    return j + 2
                j += 1
            return None
        if kind in "()*+#":
            return None if i + 2 >= len(text) else i + 3
        self.wrap = False
        if kind == "M":
            if self.y == self.top:
                self._scroll(-1)
            elif self.y > 0:
                self.y -= 1
        elif kind == "D":
            self._linefeed()
        elif kind == "E":
            self.x = 0
            self._linefeed()
        elif kind == "7":
            self.saved = (self.x, self.y)
        elif kind == "8":
            self.x, self.y = self.saved
        elif kind == "c":
            self.__init__(self.cols, self.rows)
        return i + 2

    def _csi(self, params, final):
        private = params[:1] in ("?", "<", ">", "=")
        nums = [int(p) if p.isdigit() else None for p in params.lstrip("?<>=").split(";")]

        def arg(k, default=1):
            v = nums[k] if k < len(nums) else None
            return default if v is None or (v == 0 and default == 1) else v

        if private:
            if final in "hl" and any(v in (47, 1047, 1049) for v in nums):
                self.cells = [self._blank() for _ in range(self.rows)]
            return
        if final == "m":
            self._sgr([0 if v is None else v for v in nums])
            return
        if final not in "mb":
            self.wrap = False
        if final in "Hf":
            self.y = min(self.rows - 1, arg(0) - 1)
            self.x = min(self.cols - 1, arg(1) - 1)
        elif final == "A":
            self.y = max(0, self.y - arg(0))
        elif final == "B":
            self.y = min(self.rows - 1, self.y + arg(0))
        elif final == "C":
            self.x = min(self.cols - 1, self.x + arg(0))
        elif final == "D":
            self.x = max(0, self.x - arg(0))
        elif final == "G":
            self.x = min(self.cols - 1, arg(0) - 1)
        elif final == "d":
            self.y = min(self.rows - 1, arg(0) - 1)
        elif final == "J":
            mode = arg(0, 0)
            row = self.cells[self.y]
            if mode == 0:
                row[self.x:] = [BLANK] * (self.cols - self.x)
                rows = range(self.y + 1, self.rows)
            elif mode == 1:
                row[:self.x + 1] = [BLANK] * (self.x + 1)
                rows = range(0, self.y)
            else:
                rows = range(self.rows)
            for r in rows:
                self.cells[r] = self._blank()
        elif final == "K":
            mode = arg(0, 0)
            row = self.cells[self.y]
            if mode == 0:
                row[self.x:] = [BLANK] * (self.cols - self.x)
            elif mode == 1:
                row[:self.x + 1] = [BLANK] * (self.x + 1)
            else:
                self.cells[self.y] = self._blank()
        elif final == "X":
            row = self.cells[self.y]
            n = min(arg(0), self.cols - self.x)
            row[self.x:self.x + n] = [BLANK] * n
        elif final == "@":
            row = self.cells[self.y]
            n = min(arg(0), self.cols - self.x)
            row[self.x:self.x] = [BLANK] * n
            del row[self.cols:]
        elif final == "P":
            row = self.cells[self.y]
            n = min(arg(0), self.cols - self.x)
            del row[self.x:self.x + n]
            row += [BLANK] * n
        elif final in "LM" and self.top <= self.y <= self.bottom:
            n = min(arg(0), self.bottom - self.y + 1)
            self._scroll(n if final == "M" else -n, top=self.y)
        elif final in "ST":
            self._scroll(arg(0) if final == "S" else -arg(0))
        elif final == "r":
            top, bottom = arg(0) - 1, arg(1, self.rows) - 1
            if 0 <= top < bottom < self.rows:
                self.top, self.bottom = top, bottom
            else:
                self.top, self.bottom = 0, self.rows - 1
            self.x = self.y = 0
        elif final == "s":
            self.saved = (self.x, self.y)
        elif final == "u":
            self.x, self.y = self.saved
        elif final == "b":
            for _ in range(arg(0)):
                self._put(self.last)


def keep_work():
    return os.environ.get("KEEP_WORK") == "1"


def main(tests):
    ok = failed = skipped = 0
    try:
        for test in tests:
            name = getattr(test, "__name__", str(test))
            try:
                test()
                print(f"  ok   {name}", flush=True)
                ok += 1
            except Skip:
                raise
            except SkipCase as exc:
                print(f"  SKIP {name}: {exc}", flush=True)
                skipped += 1
            except Exception as exc:  # AssertionError included
                if not isinstance(exc, AssertionError):
                    traceback.print_exc()
                print(f"  FAIL {name}: {exc or repr(exc)}", flush=True)
                failed += 1
    except Skip as exc:
        print(f"SKIP: {exc}", flush=True)
        if failed:  # a failure before the skip must not turn into a SKIP
            print(f"result: {ok} ok, {failed} failed", flush=True)
            sys.exit(1)
        sys.exit(77)
    print(f"result: {ok} ok, {failed} failed" + (f", {skipped} skipped" if skipped else ""),
          flush=True)
    sys.exit(1 if failed else 0)


class Inst:
    """This build installed under <work>/inst (the installed layout)."""

    def __init__(self, root):
        found = list(root.rglob("bin/erssi"))
        mods = list(root.rglob("libirc_core.so"))
        if not found or not mods:
            raise Skip("installed tree incomplete (no bin/erssi or libirc_core.so)")
        self.root = root
        self.erssi = found[0]
        self.moddir = mods[0].parent
        irssi_pm = list(root.rglob("Irssi.pm"))
        self.perllib = irssi_pm[0].parent if irssi_pm else None
        self.has_perl = self.perllib is not None
        share = self.erssi.parent.parent / "share" / "irssi"
        self.scriptdir = share / "scripts"
        self.helpdir = share / "help"
        self.themedir = share / "themes"


def install(build, work):
    """meson install --destdir <work>/inst. The install step rebuilds what is
    out of date first: `meson test <name>` rebuilds only the test's own
    dependencies, so without that a named live test could run a stale binary."""
    build, work = Path(build), Path(work)
    if shutil.which("meson") is None:
        raise Skip("meson not found")
    dest = work / "inst"
    if not dest.exists():
        tmp = work / "inst.tmp"  # an interrupted install is never reused
        shutil.rmtree(tmp, ignore_errors=True)
        r = subprocess.run(["meson", "install", "-C", str(build), "--destdir", str(tmp),
                            "--quiet"], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError("meson install failed:\n" + r.stdout + r.stderr)
        tmp.rename(dest)
    return Inst(dest)


def wait_until(check, timeout, interval=0.05):
    """Call check() until it returns something true or TIMEOUT seconds
    have passed (it is called once more at the end); its last value. For
    waiting on a condition instead of a fixed time."""
    end = time.time() + timeout
    while True:
        value = check()
        if value or time.time() >= end:
            return value
        time.sleep(interval)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Ircd:
    """tests/live/ircd.py as a child process; the wire log is work/wire.log,
    every line in and out (with the time) is in work/ircd-trace.log.
    Usable as a context manager (stop() on exit)."""

    def __init__(self, work, extra_args=()):
        self.work = Path(work)
        self.extra = list(extra_args)
        self.proc = None
        self.wire = self.work / "wire.log"
        self.trace = self.work / "ircd-trace.log"

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()

    def start(self, timeout=60):
        """Start the server on a port the system picks when it binds (no
        race with another program for a port chosen before); it writes the
        port to a file once it listens. Returns the port. The deadline is
        for a loaded machine: an idle one takes a fraction of a second."""
        port_file = self.work / "ircd.port"
        try:
            port_file.unlink()
        except FileNotFoundError:
            pass
        self.proc = subprocess.Popen(
            [sys.executable, str(HERE / "ircd.py"), "--port", "0", "--port-file", str(port_file),
             "--wire", str(self.wire), "--trace", str(self.trace)] + self.extra)

        def listening():
            try:
                return int(port_file.read_text())
            except (OSError, ValueError):
                return self.proc.poll() is not None and -1
        port = wait_until(listening, timeout)
        if not port or port < 0:
            self.stop()
            raise AssertionError(f"fake IRC server did not start in {timeout} s (exit {self.proc.poll()})")
        return port

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


class Client:
    """A bare IRC connection to the fake server, registered (001 seen)."""

    def __init__(self, port, nick, user="u"):
        self.nick = nick
        self.sock = socket.create_connection(("127.0.0.1", port), 30)
        self.buf = b""
        try:
            self.send(f"NICK {nick}")
            self.send(f"USER {user} 0 * :{nick}")
            self.expect(" 001 ")
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def send(self, line):
        self.sock.sendall(line.encode() + b"\r\n")

    def expect(self, substr, timeout=30):
        end = time.time() + timeout
        while True:
            while b"\r\n" in self.buf:
                line, self.buf = self.buf.split(b"\r\n", 1)
                text = line.decode(errors="replace")
                if text.startswith("PING"):
                    self.send("PONG" + text[4:])
                if substr in text:
                    return text
            left = end - time.time()
            if left <= 0:
                raise AssertionError(f"{self.nick}: did not see {substr!r}")
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                raise AssertionError(f"{self.nick}: connection closed waiting for {substr!r}")
            self.buf += data

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def config(port, nick, chatnet="E2E", channels=("#test",), extra="", servers=None):
    """An erssi config for the fake server. "@HOME@" and "@PERLLIB@" are
    replaced by Erssi.start() with the instance's --home and Perl library.
    servers: list of (chatnet, port); default [(chatnet, port)].
    cmd_queue_speed is 5 ms: with the default 2200 ms the post-connect
    commands (MODE, JOIN, MODE #chan, WHO) are paced one per 500 ms tick
    and a join takes ~5 s."""
    servers = servers or [(chatnet, port)]
    srv = ", ".join(f'{{ address = "127.0.0.1"; chatnet = "{c}"; port = "{p}"; autoconnect = "yes"; }}'
                    for c, p in servers)
    net = " ".join(f'{c} = {{ type = "IRC"; }};' for c, _ in servers)
    chn = ", ".join(f'{{ name = "{ch}"; chatnet = "{c}"; autojoin = "yes"; }}'
                    for c, _ in servers for ch in channels)
    return f"""servers = ( {srv} );
chatnets = {{ {net} }};
channels = ( {chn} );
logs = {{ "@HOME@/all.log" = {{ auto_open = "yes"; level = "ALL"; }}; }};
settings = {{
  core = {{ real_name = "live test"; user_name = "{nick}"; nick = "{nick}"; }};
  "fe-common/core" = {{ theme = "default"; autolog = "yes"; autolog_path = "@HOME@/logs/$tag/$0.log"; autolog_level = "ALL"; }};
  "irc/core" = {{ cmd_queue_speed = "5msec"; }};
  "perl/core" = {{ perl_use_lib = "@PERLLIB@"; }};
}};
{extra}
"""


def block(config_text, name):
    """{key: {field: value}} of a top level block of a saved config, None
    when the config has no such block."""
    m = re.search(r"^" + name + r" = \{\n(.*?)^\};", config_text, re.M | re.S)
    if m is None:
        return None
    out = {}
    for key, body in re.findall(r"^  (\S+) = \{(.*?)\};", m.group(1), re.M | re.S):
        out[key] = dict(re.findall(r'(\w+) = "([^"]*)";', body))
    return out


class Erssi:
    """One erssi in a pty. HOME is work/home-<name>, --home is its .erssi."""

    def __init__(self, inst, work, name, config_text=None, args=(), env=None):
        self.inst = inst
        self.work = Path(work).resolve()
        self.name = name
        self.config_text = config_text
        self.args = list(args)
        self.extra_env = dict(env or {})
        self.home_dir = self.work / f"home-{name}"
        self.home = self.home_dir / ".erssi"
        self.proc = None
        self.master = None
        for a in self.args:
            if a in ("-h",) or a.startswith("--home"):
                raise ValueError("the harness sets --home itself")
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._thread = None
        self._marks = 0
        self._screen = None
        self._screen_off = 0
        self._log_start = 0  # size of the all log when this run started

    def child_env(self):
        """The environment of the child, built from an allowlist: nothing
        else of the caller's (TMUX, STY, TERM_PROGRAM, IRC*, XDG_*, SSH_*,
        KITTY_*, ...) reaches erssi. The test's env= overrides come last."""
        env = {k: os.environ[k] for k in ("PATH", "PERL5LIB", "LD_LIBRARY_PATH", "LLVM_SYMBOLIZER_PATH",
                                          "ASAN_SYMBOLIZER_PATH") if k in os.environ}
        env.update({
            "HOME": str(self.home_dir),
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
            "TERM": "xterm-256color",
            "ASAN_OPTIONS": f"detect_leaks=0:log_path={self.work}/asan-{self.name}",
            "UBSAN_OPTIONS": f"print_stacktrace=1:log_path={self.work}/ubsan-{self.name}",
            # never the installer of an erssi installed on this machine
            # (update_check, /UPDATE CHECK would contact GitHub): a test
            # that wants one names its stand-in
            "ERSSI_TEST_UPDATE_INSTALLER": str(self.work / "no-update-installer"),
        })
        env.update(self.extra_env)
        # the real HOME must never reach a child
        home = Path(env["HOME"]).resolve()
        if not (home == self.work or self.work in home.parents):
            raise AssertionError(f"HOME outside the work dir: {home}")
        return env

    def _prepare(self):
        home = self.home
        for d in ("modules", "logs", "scripts/autorun"):
            (home / d).mkdir(parents=True, exist_ok=True)
        for so in self.inst.moddir.glob("*.so"):
            link = home / "modules" / so.name
            if not link.exists():
                link.symlink_to(so)
        # the theme of this build, from --home: otherwise erssi reads the
        # theme directory of the installation prefix
        theme = self.inst.themedir / "default.theme"
        if theme.is_file():
            shutil.copy(theme, home / "default.theme")
        if self.config_text is not None:
            text = self.config_text.replace("@HOME@", str(home)).replace("@PERLLIB@", str(self.inst.perllib or ""))
            (home / "config").write_text(text)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.kill()

    def start(self, cols=200, rows=50):
        self._prepare()
        env = self.child_env()
        try:
            self._log_start = self.all_log().stat().st_size
        except OSError:
            self._log_start = 0
        master, slave = os.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

        def ctty():
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        try:
            self.proc = subprocess.Popen([str(self.inst.erssi), f"--home={self.home}"] + self.args,
                                         stdin=slave, stdout=slave, stderr=slave, env=env,
                                         cwd=str(self.home_dir), start_new_session=True, preexec_fn=ctty)
        except BaseException:
            os.close(master)
            raise
        finally:
            os.close(slave)
        self.master = master
        with self._lock:
            # a restart of this object: nothing of the previous run (the
            # reader of that one was joined in kill()) may match a wait
            self._buf.clear()
            self._screen = Screen(cols, rows)
            self._screen_off = 0
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def resize(self, cols, rows):
        """A new terminal size: the kernel sends SIGWINCH to erssi."""
        self.screen()  # what came before is drawn at the old size
        self._screen.resize(cols, rows)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def screen(self, first=0, end=None):
        """The rows of the terminal now as text, columns FIRST to END - 1
        (see Screen)."""
        with self._lock:
            data = bytes(self._buf[self._screen_off:])
            self._screen_off = len(self._buf)
        self._screen.feed(data)
        return self._screen.lines(first, end)

    def screen_styles(self, first=0, end=None):
        """The styles of the cells of the terminal now (Screen.styles)"""
        self.screen()
        return self._screen.styles(first, end)

    def screen_and_styles(self, first=0, end=None):
        """(screen(), screen_styles()) of ONE snapshot: two calls would
        feed the output that came between them"""
        rows = self.screen(first, end)
        return rows, self._screen.styles(first, end)

    def wait_screen(self, check, timeout, first=0, end=None):
        """Poll the screen (columns FIRST to END - 1) until check(rows) is
        true; the last rows read, and whether it was."""
        end_time = time.time() + timeout
        while True:
            rows = self.screen(first, end)
            if check(rows):
                return rows, True
            if time.time() >= end_time:
                return rows, False
            time.sleep(0.1)

    def run_detached(self, timeout, stdin_tty=False):
        """erssi without a terminal on stdout: stdout and stderr one pipe,
        stdin /dev/null (or, with stdin_tty, the slave of a new pty), the
        same environment, HOME and --home as start(). Killed after TIMEOUT
        seconds if it is still running. Returns (exit status, output bytes);
        the status is negative for a signal, as in subprocess."""
        self._prepare()
        env = self.child_env()
        master = slave = None
        if stdin_tty:
            master, slave = os.openpty()
        try:
            proc = subprocess.Popen([str(self.inst.erssi), f"--home={self.home}"] + self.args,
                                    stdin=slave if stdin_tty else subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                                    cwd=str(self.home_dir), start_new_session=True)
        except BaseException:
            if master is not None:
                os.close(master)
            raise
        finally:
            if slave is not None:
                os.close(slave)
        try:
            out, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                proc.kill()
            out, _ = proc.communicate()
        finally:
            if master is not None:
                os.close(master)
        return proc.returncode, out

    def _reader(self):
        while True:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                return
            if not data:
                return
            with self._lock:
                self._buf += data

    def type(self, line):
        """Type LINE and give erssi 0.3 s: for a line that cannot be
        followed by type_and_wait's /echo (no all log, a prompt that would
        take the /echo as its answer). Prefer type_and_wait."""
        self.write_raw(line.encode() + b"\r")
        time.sleep(0.3)

    def write_raw(self, data):
        os.write(self.master, data)

    def wait_terminal(self, timeout=30):
        """After start(): wait until erssi has set the terminal up (raw
        mode, then the alternate screen): what is typed from then on is
        read as keys. For an erssi without an all log (no config)."""
        if not self.wait_output(b"\x1b[?1049h", timeout):
            raise AssertionError(f"{self.name}: the terminal was not set up in {timeout} s"
                                 f" (exited: {self.proc.poll()})")

    def wait_ready(self, timeout=30):
        """After start(): wait until erssi runs what is typed - its all log
        has new lines (it is open), then an /echo of a new marker
        reaches it. The /echo is typed again after 2, 4 and 8 s in case
        the first one came before the log was open: at most four lines,
        never a paste of paste_verify_line_count (5) lines."""
        def fail(what):
            return AssertionError(f"{self.name}: {what} in {timeout} s (exited: {self.proc.poll()})")
        end = time.time() + timeout
        self.wait_terminal(timeout)

        def log_open():
            try:
                return self.proc.poll() is not None or self.all_log().stat().st_size > self._log_start
            except OSError:
                return False
        if not wait_until(log_open, timeout) or self.proc.poll() is not None:
            raise fail("no all log")
        wait = 2
        while True:
            self._marks += 1
            mark = f"ready-{self.name}-{self._marks}-{time.time()}"
            self.write_raw(f"/echo {mark}\r".encode())
            if self.wait_file(self.all_log(), mark, min(wait, max(0.1, end - time.time()))):
                return
            if self.proc.poll() is not None or time.time() >= end:
                raise fail("erssi does not answer")
            wait *= 2

    def sync_output(self, timeout=20):
        """Wait until erssi has written to the terminal everything it was
        going to for what it read so far: an /echo of a new marker typed
        now shows up in its output (one stream, in order). For a check
        that something was NOT written, instead of a fixed time. The line
        must reach a command (no prompt or half key sequence waiting)."""
        self._marks += 1
        mark = f"sync{self._marks}x{int(time.time() * 1000) % 100000}"
        self.write_raw(f"/echo {mark}\r".encode())
        if not self.wait_output(mark.encode(), timeout):
            raise AssertionError(f"{self.name}: the /echo {mark} was not drawn in {timeout} s"
                                 f" (exited: {self.proc.poll()})")

    def upgrade(self, timeout=60):
        """/UPGRADE, then wait until the new erssi runs what is typed: it
        has opened the all log again (one more "--- Log opened" line: the
        old process does not close its logs before the exec), and an /echo
        reaches it (wait_ready). The session file is no sign: /UPGRADE
        first writes a probe under its name and removes it again."""
        def opened():
            try:
                return self.all_log().read_bytes().count(b"--- Log opened")
            except OSError:
                return 0
        before = opened()
        self.write_raw(b"/upgrade\r")
        if not wait_until(lambda: opened() > before or self.proc.poll() is not None, timeout) \
                or self.proc.poll() is not None:
            raise AssertionError(f"{self.name}: no new erssi after /upgrade in {timeout} s"
                                 f" (exited: {self.proc.poll()})")
        self.wait_ready(timeout)

    def save_config(self, *lines, timeout=20):
        """Type LINES (e.g. "/layout save") and /SAVE, wait until erssi has
        run them; the config file's text then. /SAVE writes the file before
        it returns, so once the /echo typed after it is in the log the file
        is complete - a better sign than its mtime or contents, which can
        stay the same (the same layout saved again, in the same tick)."""
        self.type_and_wait(*lines, "/save", timeout=timeout)
        return (self.home / "config").read_text()

    def type_and_wait(self, *lines, timeout=20):
        """Type LINES and wait (no fixed time) until erssi has run them
        all: an /echo of a new marker typed after them reaches the all log
        (the config must log level ALL to all.log, as config() does).
        Lines read at once are a paste, run as typed below
        paste_verify_line_count (5): at most 3 lines and the marker."""
        if len(lines) > 3:
            raise ValueError("type_and_wait: at most 3 lines at a time")
        self._marks += 1
        mark = f"type-and-wait-{self.name}-{self._marks}-{time.time()}"
        for line in lines + (f"/echo {mark}",):
            self.write_raw(line.encode() + b"\r")
        if not self.wait_file(self.all_log(), mark, timeout):
            raise AssertionError(f"{self.name}: erssi did not run {lines!r} in {timeout} s"
                                 f" (exited: {self.proc.poll()})")

    def output(self):
        with self._lock:
            return bytes(self._buf)

    def mark(self):
        with self._lock:
            return len(self._buf)

    def output_since(self, mark):
        with self._lock:
            return bytes(self._buf[mark:])

    def text(self, since=0):
        """The output since a mark() as text, ANSI escape sequences removed."""
        with self._lock:
            raw = bytes(self._buf[since:])
        return ANSI.sub("", raw.decode(errors="replace"))

    def _has(self, pattern):
        with self._lock:
            return pattern in self._buf

    def wait_output(self, pattern, timeout):
        return bool(wait_until(lambda: self._has(pattern), timeout))

    def all_log(self):
        return self.home / "all.log"

    def wait_file(self, path, text, timeout):
        def found():
            try:
                return text in Path(path).read_text(errors="replace")
            except OSError:
                return False
        return bool(wait_until(found, timeout))

    def quit(self):
        if not self.proc:
            return
        if self.proc.poll() is None:
            try:
                self.write_raw(b"/quit\r")
                self.proc.wait(10)
            except (subprocess.TimeoutExpired, OSError):
                pass
        self.kill()

    def kill(self):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except OSError:
                self.proc.kill()
            self.proc.wait()
        # the slave side is closed now: the reader gets EIO and ends. Join it
        # before closing the fd it reads (the number could be reused).
        if self._thread is not None:
            self._thread.join(2)
            self._thread = None
        if self.master is not None:
            try:
                os.close(self.master)
            except OSError:
                pass
            self.master = None

    def sanitizer_reports(self):
        return sorted(p for pat in (f"asan-{self.name}.*", f"ubsan-{self.name}.*") for p in self.work.glob(pat))
