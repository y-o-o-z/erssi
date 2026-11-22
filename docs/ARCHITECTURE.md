# erssi — Architecture Overview

A map of the codebase for people who want to read, extend or audit it. This
is the Shellter Edition tree, but the architecture below is erssi's and, under
that, irssi's: the layering, the signal system and the module pattern are the
same ones you will find upstream. For what this edition adds on top, see
[`../README.md`](../README.md); for how it tracks upstream, see
[`UPSTREAM.md`](UPSTREAM.md).

All paths are relative to the repository root.

---

## 1. The layers

erssi is organised as a stack. Each layer depends only on the ones below it,
and the frontends at the top are interchangeable — the same core drives a
terminal, a web browser or a headless process.

```
┌───────────────────────────────────────────────────────────────┐
│  Frontends          fe-ansi (terminal)   fe-web (browser)       │
│                     fe-none (headless)   fe-fuzz (fuzz harness)  │
├───────────────────────────────────────────────────────────────┤
│  Frontend-agnostic  fe-common/core, fe-common/irc               │
│  UI logic           (windows, printtext, themes, hilight, …)    │
├───────────────────────────────────────────────────────────────┤
│  IRC protocol       irc/core  (+ dcc, flood, anti-floodnet,     │
│                     notifylist, proxy)                          │
├───────────────────────────────────────────────────────────────┤
│  Core               servers, channels, queries, nicklist,       │
│                     settings, signals, commands, modules, …     │
└───────────────────────────────────────────────────────────────┘

   Cross-cutting subsystems used from several layers:
     e2e/            native end-to-end encryption (RPE2E)
     otr/            Off-the-Record messaging
     image-preview/  inline image rendering in the terminal
     perl/           Perl scripting bridge
     lib-config/     config-file parser
```

**Core** (`src/core/`) has no UI at all. It owns the IRC-independent model —
servers, channels, queries, the nick list, settings, the command parser, the
module loader and, above all, the **signal system** that every other layer
talks through. Security-sensitive core code lives here too, e.g.
`src/core/credential.c` (see the note in §6).

**IRC protocol** (`src/irc/core/`) turns bytes on the wire into classified
events and back. Around it sit the IRC feature modules: `dcc/` (file
transfer), `flood/` and `anti-floodnet/` (flood protection), `notifylist/`
(watch list) and `proxy/` (the irssi-proxy server).

**Frontend-agnostic UI** (`src/fe-common/`) is everything a frontend needs
that is *not* tied to one display: window and window-item management,
`printtext` formatting, themes, highlighting, the status bar model. Both the
terminal and the web frontend build on it.

**Frontends** (`src/fe-*`) are the only layer that knows about a screen, a
socket to a browser, or nothing at all:

| Frontend | Entry point | Purpose |
|----------|-------------|---------|
| `fe-ansi` | `src/fe-ansi/irssi.c` (`main`) | The terminal client: pure ANSI/VT100 rendering, sidepanels, mouse, gestures, 24-bit colour. |
| `fe-web` | — (loaded as a module) | A WebSocket server that mirrors the session to a browser. |
| `fe-none` | `src/fe-none/irssi.c` (`main`) | Headless: a core with a main loop and no display, used for scripting/bots. |
| `fe-fuzz` | `src/fe-fuzz/` | A libFuzzer harness that drives the parsers of each subsystem. Built only with `-Dwith-fuzzer=yes`. |

---

## 2. Directory map

```
src/
├── core/              IRC-independent model + the signal system
│   ├── servers.c, channels.c, queries.c, nicklist.c
│   ├── settings.c            settings/configuration
│   ├── signals.c             signal/hook system (the backbone)
│   ├── commands.c            /command parsing and dispatch
│   ├── modules.c, modules.h  module loader; MODULE_DATA macros
│   ├── credential.c          credential handling (see §6)
│   └── ...
├── irc/
│   ├── core/          IRC protocol: parsing, events, modes, CTCP
│   │   ├── irc.c             raw-line parsing and event classification
│   │   ├── irc-servers.c     IRC server records
│   │   ├── channel-events.c, modes.c, ctcp.c
│   │   └── ...
│   ├── dcc/           file transfer
│   ├── flood/         simple per-nick flood detection (small reference module)
│   ├── anti-floodnet/ netsplit/CTCP/nick-change flood protection (multi-file module)
│   ├── notifylist/    watch list
│   └── proxy/         irssi-proxy server
├── fe-common/
│   ├── core/          windows, printtext, themes, hilight, statusbar model
│   └── irc/           IRC-specific display helpers
├── fe-ansi/           terminal frontend
│   ├── irssi.c              main(); textui_init()/textui_finish_init()
│   ├── mainwindows.c        split-window layout
│   ├── gui-printtext.c      text rendering
│   ├── gui-readline.c       input handling
│   ├── gui-entry.c          input line editing
│   ├── gui-mouse.c          mouse protocol (SGR)
│   ├── gui-gestures.c       mouse-gesture navigation
│   ├── sidepanels-*.c       window-list / nick-list side panels
│   └── term-ansi.c          ANSI/VT100 terminal abstraction
├── fe-web/            WebSocket web frontend
│   ├── fe-web.c             coordinator
│   ├── fe-web-server.c      TCP/WebSocket listener
│   ├── fe-web-client.c      per-client state
│   ├── fe-web-signals.c     forwards IRC events to clients (a large file)
│   ├── fe-web-websocket.c   RFC 6455 framing
│   ├── fe-web-json.c        JSON serialisation
│   ├── fe-web-crypto.c      transport encryption
│   └── fe-web-ssl.c         TLS
├── fe-none/           headless frontend
├── fe-fuzz/           libFuzzer harness + corpus (per-subsystem targets)
├── e2e/               native end-to-end encryption (RPE2E) — see §7
├── otr/               Off-the-Record messaging — see §7
├── image-preview/     inline image preview (libcurl + chafa) — see §7
├── lib-config/        config-file parser
└── perl/              Perl scripting bridge

tests/                 unit tests, run via `meson test` (see §8)
themes/                colour themes (incl. the shellter theme)
docs/                  this file, QUICK_REFERENCE.md, docs/shellter/, help/
meson.build, meson_options.txt
```

---

## 3. The signal system — the backbone

Nearly all communication between layers goes through a gtk-like signal bus
(`src/core/signals.c`). A layer **emits** a named signal; any number of
handlers, registered by other layers or by Perl scripts, run in turn and may
stop propagation. This is why the frontends can be swapped out: core and the
IRC layer only ever emit signals, they never call a frontend directly.

```c
/* register a handler (usually in a module's *_init) */
signal_add("event privmsg", (SIGNAL_FUNC) my_privmsg_handler);

static void my_privmsg_handler(IRC_SERVER_REC *server, const char *data,
                               const char *nick, const char *addr) {
    /* ... */
}

/* emit from the event source */
signal_emit("event privmsg", 4, server, args, nick, address);

/* remove when the module unloads */
signal_remove("event privmsg", (SIGNAL_FUNC) my_privmsg_handler);
```

Ordering matters. Use `signal_add_first()` for a handler that must run
**before** the default one (e.g. a filter that may stop the event), and
`signal_add_last()` for one that must run **after** (e.g. a frontend that
displays the final result). `signal_stop()` halts propagation to later
handlers.

Signals come in levels of abstraction:

- **Rawest** — `"server incoming"` (every line from the server),
  `"server event"` (after prefix/tag parsing).
- **IRC protocol events** — `"event privmsg"`, `"event notice"`,
  `"event join"`, `"ctcp msg"`, numeric replies `"event 311"` … `"event 999"`.
- **Message events** (frontend-agnostic, higher level) — `"message public"`,
  `"message private"`, `"message join"`, `"message kick"`, `"message topic"`, …
- **Lifecycle / config** — `"server connected"`, `"server disconnected"`,
  `"server destroyed"`, `"setup changed"`, `"module loaded"`.

The handler signature must match the emit exactly (argument count and types);
a mismatch is the most common cause of a crash in a new module.
`docs/QUICK_REFERENCE.md` lists the signal names and signatures you will reach
for most often, and `docs/signals.txt` is the full upstream reference.

---

## 4. Message flow: wire → display

Tracing one inbound PRIVMSG shows how the layers chain through signals. All
of these functions live in `src/irc/core/irc.c` unless noted.

```
IRC server sends a raw line
        ↓
irc_parse_incoming()                    reads the socket
        ↓   emit "server incoming" (server, raw_line)
irc_parse_incoming_line()               splits @tags :prefix command args
        ↓   emit "server event" (parsed components)
irc_server_event()                      classifies: PRIVMSG, NOTICE, …
        ↓   emit "event privmsg" (server, args, nick, address)
   ┌────────────────┬─────────────────────┬──────────────────────┐
   ↓                ↓                     ↓                      ↓
flood/            ctcp handlers       fe-common/irc          fe-web
anti-floodnet     (irc/core)          builds the display      (if loaded)
(may ignore)                          "message public" →      mirrors to
                                      fe-ansi renders         browser clients
```

The split at the bottom is the whole point of the architecture: a flood
module, the terminal renderer and the web frontend all see the *same* event,
each at the level it cares about, without knowing the others exist.

---

## 5. Module pattern and per-server state

An IRC feature module is a small unit with an `_init`/`_deinit` pair that
registers its signals and settings on load and unregisters them on unload:

```c
void irc_flood_init(void) {
    settings_add_int("flood", "flood_timecheck", 8);
    settings_add_int("flood", "flood_max_msgs", 4);

    read_settings();
    signal_add("setup changed",    (SIGNAL_FUNC) read_settings);
    signal_add_first("server connected",  (SIGNAL_FUNC) flood_init_server);
    signal_add("server destroyed", (SIGNAL_FUNC) flood_deinit_server);

    module_register("flood", "irc");
}

void irc_flood_deinit(void) {
    /* remove everything _init added, in reverse */
}
```

Because core creates and destroys server records, a module cannot keep its
per-server state in a global. Instead it hangs its own opaque record off each
`SERVER_REC` with the `MODULE_DATA` macros (`src/core/modules.h`):

```c
typedef struct { GHashTable *floodlist; } MODULE_SERVER_REC;

static void flood_init_server(IRC_SERVER_REC *server) {
    MODULE_SERVER_REC *rec = g_new0(MODULE_SERVER_REC, 1);
    rec->floodlist = g_hash_table_new((GHashFunc) i_istr_hash,
                                      (GCompareFunc) i_istr_equal);
    MODULE_DATA_SET(server, rec);          /* attach */
}

MODULE_SERVER_REC *rec = MODULE_DATA(server);   /* retrieve, anywhere */

static void flood_deinit_server(IRC_SERVER_REC *server) {
    MODULE_SERVER_REC *rec = MODULE_DATA(server);
    if (rec) { g_hash_table_destroy(rec->floodlist); g_free(rec); }
    MODULE_DATA_UNSET(server);             /* detach on "server destroyed" */
}
```

Two reference modules are worth reading before writing a new one:

- **`src/irc/flood/`** — the smallest complete example: detect rapid messages
  from one nick, emit a `"flood"` signal that `autoignore.c` acts on.
- **`src/irc/anti-floodnet/`** — a multi-file module split by concern
  (`floodnet-ctcp.c`, `floodnet-nick.c`, `floodnet-stats.c`) with its own
  status commands. Read this for how a larger module is organised.

A module's build entry is a `meson.build` in its directory, pulled in by a
`subdir(...)` line in the parent `meson.build`.

---

## 6. A note on `src/core/credential.c`

The credential-encryption subsystem is present in the tree but **disabled**:
interactive attempts to enable it are refused (`CREDENTIAL_DISABLED_MSG`).
Do not treat it as a live feature or re-enable it without a decision to do so;
it is kept for config-compatibility, not as a supported path. Code that
touches plaintext there wipes it before freeing (`credential_wipe_str` /
`OPENSSL_cleanse`).

---

## 7. Shellter subsystems

These are the parts this edition leans on most. Each is optional at build time
(`meson_options.txt`) and `auto`-detected by default.

- **E2E — native end-to-end encryption** (`src/e2e/`, `-Dwith-e2e`). The RPE2E
  protocol implemented directly against OpenSSL (no extra crypto library).
  Split into protocol (`e2e-proto.c`), crypto primitives (`e2e-crypto.c`),
  the keyring (`e2e-keyring.c`), wire framing (`e2e-wire.c`), the outbound
  queue (`e2e-outq.c`), scope/state, the status bar, the JSON view and
  `e2e-web.c` for E2E over the web frontend (see `docs/fe-web-e2e.md`).
  `e2e_core_init()` registers the subsystem. Its parsers have dedicated fuzz
  targets under `src/fe-fuzz/e2e/`.
- **OTR — Off-the-Record** (`src/otr/`, `-Dwith-otr`). The classic OTR messaging
  support (libotr), with its own frontend formats and commands.
- **Image preview** (`src/image-preview/`, `-Dwith-image-preview`). Fetches an
  image over libcurl (`image-preview-fetch.c`), decodes it (`image-decode.c` /
  `stb_image.h`), caches it (`image-preview-cache.c`) and renders it to the
  terminal with chafa (`image-preview-chafa.c`). The fetch path is
  network-facing; its SSRF address checks are the kind of thing an audit
  should look at first.
- **fe-web** (`src/fe-web/`, `-Dwith-fe-web`). The WebSocket frontend; see §1/§2.
- **Perl** (`src/perl/`, `-Dwith-perl`). The scripting bridge; irssi Perl
  scripts load unmodified.

---

## 8. Building and testing

```sh
# configure (options are -D<name>, see meson_options.txt)
meson setup Build -Dwith-perl=yes -Dwith-otr=yes -Dwith-e2e=yes \
                  -Dwith-fe-web=yes -Dwith-image-preview=yes

# compile
ninja -C Build

# run the test suite
meson test -C Build
```

Run the suite through **`meson test`**, not by launching the test binaries by
hand. The tests instantiate a real client, which reads and writes a real
`~/.erssi`; `meson test` runs each one with an isolated, throwaway `HOME` so it
cannot touch your own configuration. Setting `ERSSI_TEST_PACKAGER=1` additionally
exercises the packaging/reproducibility path (this is what a release tag runs
in CI; a per-commit build skips it — see `docs/shellter/releasing.md`).

Tests live under `tests/`, mirroring the source layout (e.g.
`tests/irc/core/`). Fuzz targets live under `src/fe-fuzz/` and are built with
`-Dwith-fuzzer=yes`.

---

## 9. Memory management

erssi uses GLib allocation throughout. The rule is simple and absolute:
**anything allocated with a `g_*` allocator is freed with `g_free()`** (or the
matching `g_*_free`/`g_*_destroy`). Mixing `malloc()` with `g_free()`, or the
reverse, crashes.

```c
char   *s    = g_strdup("hello");
char   *fmt  = g_strdup_printf("x=%d", n);
STRUCT *rec  = g_new0(STRUCT, 1);
GSList *list = g_slist_append(list, item);

g_free(s); g_free(fmt); g_free(rec);
g_slist_free_full(list, g_free);     /* frees the list and every item */
g_hash_table_destroy(hash);          /* runs the destroy callbacks */
```

For buffers that held secrets (keys, passwords), wipe before freeing — see §6.

---

## 10. Where to look

| You want to… | Start at |
|---|---|
| Follow an inbound IRC line | `src/irc/core/irc.c` (`irc_parse_incoming` → `irc_server_event`) |
| See how events become signals | `src/core/signals.c`; signal names in `docs/signals.txt` |
| Read a small reference module | `src/irc/flood/flood.c` |
| Read a larger reference module | `src/irc/anti-floodnet/` |
| Understand settings | `src/core/settings.c` |
| Understand the terminal init order | `src/fe-ansi/irssi.c` (`textui_finish_init`) |
| Work on the web frontend | `src/fe-web/fe-web-signals.c`, `fe-web-server.c` |
| Work on encryption | `src/e2e/` (and `docs/fe-web-e2e.md`), `src/otr/` |
| Audit the network-facing fetch | `src/image-preview/image-preview-fetch.c` |
| Add or run tests | `tests/`, run with `meson test -C Build` (see §8) |
| A terser developer cheatsheet | `docs/QUICK_REFERENCE.md` |

---

*erssi is the modern irssi by Jerzy “kofany” Dąbrowski and the erssi-org team;
the Shellter Edition is maintained by the Shellter team. This document
describes the tree as of the 2026.11.x series.*
