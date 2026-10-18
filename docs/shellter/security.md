# Security in erssi Shellter Edition

Back to the [README](../../README.md).

The network-facing parts were reviewed, fuzzed and tested under
sanitizers.

## Web frontend (fe-web)

Built for a shared shell box.

- **Password** — a web client sends it in an `Authorization: Bearer`
  header. `?password=` in the URL (percent-encoded) is still accepted when
  the request has no `Authorization` header at all, because NexusIRC as
  published sends it there; a URL can end up in the logs of proxies and
  tunnels, so prefer a client that uses the header. One guess per
  connection, compared in constant time.
- **Wrong passwords** — after 5 wrong passwords from one peer, that peer
  gets one check every 2 s. A peer is a uid on the Unix socket (and on a
  loopback port on Linux), otherwise an address; your own uid is never
  paced, and a peer that logged in before is not held up by the wrong
  passwords of others.
- **Nobody locks you out** — on the Unix socket, and on Linux also on
  127.0.0.1, nobody else on the box can lock you out; elsewhere (FreeBSD)
  every local user is the one peer 127.0.0.1, so use the socket
  (`fe_web_socket`) on a shared box.
- **Limits** — 16 KB and 10 seconds to log in, with a separate limit for
  connections still logging in; at most 16 clients; oversized frames and
  oversized or fragmented control frames refused.
- **TLS certificate** — kept in `~/.erssi/fe-web-cert.pem`, so a web client
  can pin exactly that certificate and nothing else listening on the port
  can receive the password. The key is made only once fe-web is enabled,
  kept at mode 0600 and replaced if anyone else could read it.
- **Child processes** — web connections are not inherited by programs that
  scripts start.

Setting it up: [configuration.md](configuration.md#web-client).

## Image preview

http and https only, public addresses only (also after redirects), a hard
size limit, and no decoding of images too large to handle safely. The debug
log of clicked URLs is off by default. `image_preview_cache_size` is a soft
limit: files of the last minutes stay even when the cache is over it, and
above twice the limit no new download (nor a retry) starts, so the cache
never holds more than twice the limit plus three downloads in progress;
0 means no lasting cache and has no hard limit (`/help image`).

## Configuration

`~/.erssi/config` is written readable only by you, to a temporary file
that replaces it only once every byte is written and synced: a save that
fails on a full disk or quota keeps the old file and says why. The debug
logs of the side panels and of terminal resizes (`/set sidepanel_debug on`,
`/set resize_debug on`) are readable only by you too: they are kept in
`~/.erssi`, never in `/tmp`, and never written through a link.

Files the fork reads or writes at a path that a setting, you or another
user on the box can influence go through one helper (`src/core/safe-file.c`):

- **Reading** — opened once without waiting (a FIFO or a device put there
  cannot hang erssi) and checked on that descriptor: anything but a regular
  file is refused, with a size limit. A symbolic link as the file itself is
  refused where none is expected (the image cache, the web certificate and
  key, `default-files.sha256`, the mentions and debug logs); it is followed
  for files you may well keep as links: help pages in `~/.erssi/help` and
  the help path, the `~/.erssi/keepnick` file of the old script, the themes
  and startup file of the installation, and the external credentials file
  (only to a regular file of your own). Directories above the file are
  always followed, so a log directory on another disk keeps working.
- **Appending** (mentions log, debug logs) — only to a regular file of your
  own with no other hard link, never through a symbolic link, with mode
  0600.
- **Replacing** (files erssi installs into `~/.erssi`) — a new file in the
  same directory with its mode from the start (0600; an installed file
  that is updated keeps its mode), written out to the disk and renamed over
  the old one: a crash leaves the old or the new file, and a link at the
  path is replaced, not followed.
- **The web certificate and key** are replaced the same way when they are
  missing, damaged or about to expire. A link or anything else but a
  regular file in their place is neither used nor replaced: fe-web says so
  and does not start; once it is removed, `/set fe_web_enabled on` (or a
  restart) starts it.
- **The external credentials file** is written where a link at its path
  points, as before, through a temporary file in that file's directory;
  the file there must be a regular file of your own (a link that leads to
  no file is an error, not a missing file). A file that cannot be read or
  written is said, with the reason, and nothing is lost: after a failed
  read the file is not written until `/credential reload` reads it, a
  password moves out of `~/.erssi/config` only once the external file
  holds it, `/credential migrate config` that fails leaves
  `credential_storage_mode` as it was, and `/save` does not write the
  credentials loaded from the external file into the configuration.

The upstream code that reads `~/.erssi/config`, the startup file, themes,
the `/upgrade` session and `/cat` follows links as irssi always has.

## Hardened build

By default: PIE, stack protector, stack clash protection,
`FORTIFY_SOURCE=2`, full RELRO.

## Tested the hard way

Everything fe-web reads from the network (the login request, WebSocket
frames, encrypted frames, client messages, IRC lines turned into web
events) is fuzzed with libFuzzer under AddressSanitizer and
UndefinedBehaviorSanitizer; every finding is replayed by `meson test`. The
test suite also runs under both sanitizers.

## Credential encryption

Switched off: in erssi 1.3.1 it never encrypted the configuration file and
a wrong master password could delete stored credentials. Existing setups
keep working; `/help credential` explains how to return to plain storage.
A value they encrypt is AES-256-GCM, so a changed value or a wrong master
password is refused; values from versions before 2026.10.3 (AES-256-CBC,
which cannot detect a change) are still read. With encryption on,
`/credential passwd` followed by `/save` writes them, like all others, in
plain text. Only a value still in the old form at `/save`, in config mode
and with a master password that a value already in the new form proves
right, is written again in the new form; the external credentials file
is never rewritten that way.

When erssi starts with a configuration that still has the old encryption
or the external file on, or holds encrypted values, it says once in the
status window what is and is not protected - `~/.erssi/config` itself is
plain text, and only values the old feature encrypted are encrypted in it -
and which commands go back to plain storage (`/help credential`).

## Known limitations

### Signal handlers are called through one function type

irssi's signal system (`src/core/signals.c`), kept as it is because every
module and script is built on it, calls each handler through one generic
type, `SIGNAL_FUNC`: a function of six pointer arguments. The handler
itself may declare fewer parameters, or an `int` where the signal passes
`GINT_TO_POINTER()`. Calling a function through an incompatible type is
undefined behaviour in ISO C.

It works on every ABI erssi is built for. On x86-64 (System V), i386
(cdecl), AArch64 and 32-bit Arm (AAPCS), RISC-V, PowerPC and s390x the
caller puts the arguments in registers or in its own stack frame and
removes them itself after the call; the callee reads only the parameters
it declares and never sees the others. An `int` parameter reads the low
part of the register or slot that holds the pointer-sized value, which is
what `GPOINTER_TO_INT()` would give. A handler that declares more
parameters than its signal carries gets `NULL` for them, as the dispatcher
always passes six. `tests/core/test-signal-abi.c` checks all of this on
the machine the build is for, through the real `signal_emit()`.

It would break where the callee removes the arguments (stdcall-style
conventions, such as 32-bit Windows built with `-mrtd`) and where every
indirect call is type-checked: WebAssembly traps, and clang's control-flow
integrity for indirect calls (`-fsanitize=cfi-icall`) stops at the first
signal. The build refuses a WebAssembly target, and `-fsanitize=cfi` in
CFLAGS unless `-fno-sanitize=cfi-icall` follows it.

clang 17 and later include `-fsanitize=function` in
`-fsanitize=undefined`. It reports exactly this pattern, at the first
signal, so a sanitizer build could test nothing else. `meson.build` adds
`-fno-sanitize=function` where the compiler knows the option - clang; GCC
has no such check and does not take the option, so nothing is turned off
there. Every other check of UndefinedBehaviorSanitizer and
AddressSanitizer stays on.

A static check that handlers match their signals is not done: signals are
named by strings, the handlers are cast to `SIGNAL_FUNC` at hundreds of
`signal_add()` calls, so it would need a parser of all of them; the ABI
test above covers what the pattern relies on.
