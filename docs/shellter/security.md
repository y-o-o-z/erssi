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
- **Other users on the box** — on the Unix socket, and on Linux on
  127.0.0.1 while the kernel tells erssi who connects, your connections
  are told apart from everyone else's. Elsewhere (FreeBSD, macOS, Linux
  without that answer) every local user is the one peer 127.0.0.1: another
  user can still delay your connection or, through the limit of
  connections still logging in, close it while it waits, as in
  2026.10.5. Use the socket (`fe_web_socket`) on a shared box.
- **Limits** — 16 KB and 10 seconds to log in, with a separate limit for
  connections still logging in; at most 8 new connections per peer in 10
  seconds and 200 a second from all peers together, refused before their
  TLS handshake - a peer being a remote address or a local uid other than
  yours (yours are not counted, nor a peer that logged in before against
  the 200); a connection its peer already reset or closed is closed at
  accept. A loopback peer whose uid is not known (every local user then,
  you too: always on FreeBSD and macOS, on Linux when the kernel cannot
  tell) is never refused for its number of connections: its handshakes
  are only spaced out, 50 a second, and the others wait among the
  connections still logging in. That bounds the CPU, but it does not keep
  the users apart: there another local user can still delay your
  connection or, through that limit, close it while it waits, as in
  2026.10.5 - fe_web_socket (a Unix socket) avoids that;
  at most 16 clients; messages of at most 16 MB, also when fragmented (in
  at most 4096 frames). A key fe-web makes is ECDSA P-256, whose
  handshake costs a fraction of the RSA key of older versions (a stored
  pair is kept until its certificate comes within 30 days of its end).
- **WebSocket rules** — a frame that breaks RFC 6455 closes the
  connection with the status the RFC gives: unmasked frames, reserved
  bits, unknown opcodes, a continuation of nothing, a message started
  inside another and oversized or fragmented control frames (1002), a
  text message or close reason that is not UTF-8 (1007), a message that
  is too big (1009). Nothing after it is handled.
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

## Clickable addresses (OSC 8)

The addresses that become links come from other people, so:

- **No hidden targets** — a link is only ever the address as it is
  shown, and its target is that same text; nobody can make words point
  somewhere else. A colour change (also reverse and a reset to the
  defaults) ends an address, so its rest cannot be drawn in the colour of
  the background and still be part of the target; bold, underline and
  italic do not end it. Only http, https, ftp, gopher, gemini, irc and ircs
  addresses become links (no file:, javascript:, data: or mailto:).
- **Nothing ends the sequence early** — the target holds only printable
  ASCII: characters that may not be in a URI are percent-encoded, and
  controls (C0, C1, DEL, also as UTF-8), spaces, invalid UTF-8 and
  Unicode format characters such as bidi overrides end the address. The
  terminal code checks the target again before it writes it, and an
  address longer than 2048 bytes stays plain text. The `id=` of a link is
  made by erssi from the line and a random salt, letters and digits only.
- **Nothing leaks into other text** — a link is open only while the
  address itself is drawn: it is closed before every cursor move, clear
  and scroll, so the side panels, the statusbar, the input line and the
  cleared cells are never part of it.
- **What it does not do** — a link to a look-alike address (another
  domain that reads the same) is still a link to what is shown; check the
  address before you open it, as in a browser.

## What end-to-end encryption protects, and what it does not

erssi's E2E is RPE2E v1.0, the protocol of repartee and its scripts.

- **Protected** — the text of PRIVMSGs and actions (`/me`) in a channel or
  query with E2E on, against the server and the other users: nobody
  without the keys can read it or change the text of a line unnoticed
  (but see below: lines and parts can go missing). Once you have
  verified a person's key (`/e2e verify`, compared over another channel),
  the keys you get from them are really theirs.
- **Not protected** — NOTICEs, CTCPs other than ACTION, and bot commands
  while `e2e_bot_bypass` is on (sent in clear text); nicks, addresses,
  channel names, who talks to whom, when, and how long the messages are;
  that every message arrives, and in order: the server can drop whole
  messages, drop some parts of a long one (each part is shown on its
  own, nothing says one is missing) or reorder messages, as long as each
  arrives within 5 minutes of its timestamp;
  which of a channel's readers wrote a line (a sender's key there is
  shared with everyone they let read, so one of them, with the server's
  help, could pass a line off as the sender's); everything on your own
  machine, where messages are decrypted: logs, the Mentions window,
  terminal notifications and web clients get clear text. Lines that
  arrive in clear text are shown with `[plain]` in red - in a query also
  one from another address under the peer's nick (`[plain, from
  <ident@host>]`), and one under your own nick that you did not send
  encrypted.
- **No forward secrecy** — session keys stay in
  `~/.erssi/rpe2e/keyring.json` while they are used, and a new key (a
  rotation, REKEY) is sent wrapped to the recipient's long-term identity
  key, as in repartee. Whoever recorded the traffic and later gets a
  recipient's identity private key (or keyring.json) can read those
  sessions; rotating keys does not undo that. Keep keyring.json private
  (erssi writes it, and its exports, with mode 0600). After a suspected
  compromise run `/e2e reset -force` (a new identity), remove the old
  `keyring.json.bak-*` copies and have people verify your new key.
- **Replays** — a message the server delivers again is dropped while its
  timestamp is still accepted (5 minutes either way), also after a
  restart: the cache of what was read is kept in
  `~/.erssi/rpe2e/replay.cache` (only hashes and times). Each sender has
  room for its last 512 parts in each channel or query, 16384 in all, so
  a flood can make only the flooder's own older lines repeatable - unless
  many thousands of senders write at once, when the cap takes from
  whoever holds the most. Two erssi processes that share one home
  overwrite each other's cache file (the last one to save wins), which
  only weakens the protection after a restart.
- **Files** — keyring.json, the replay cache and `/e2e import` files are
  read only when they are regular files (a symlink is not followed, a
  FIFO or device is refused at once); the keyring and the cache must
  also be yours.
- **Wire compatibility** — forward secrecy, encrypted NOTICEs or signed
  lines would need a new protocol version shared with repartee and its
  scripts; erssi keeps RPE2E v1.0 so that they all understand it.

`/help e2e` has the same in full.

## Update notice

`update_check` (off by default) runs the installer installed with erssi,
`<prefix>/share/irssi/shellter-install.sh --update --check --porcelain --summary`,
once a day. It is the check of `erssi --check-update`: the newest release
tag counts only with a valid signature by the release key that installer
carries ([What the installer verifies](install.md#what-the-installer-verifies));
erssi does not look for releases itself. The check downloads the list
of release tags and tag objects with their commits, no code, and nothing
is installed or run from them.

The list of tags is not authenticated: it comes over HTTPS from the
repository, and whoever controls that repository, the server or the
connection can leave a release out of it, or serve an older tag object.
The check then says "current" (a moved tag of the installed release is
reported), so a new release can be withheld from you. It can never make
a tag look signed that the release key did not sign, and nothing it
fetches is installed.

The installer runs in a session of its own, with no terminal (stdin is
`/dev/null`, git may not ask for a password), and is stopped with its
children after two minutes. erssi keeps at most 64 KB of its output and
takes only one line in the expected form, with version numbers made of
digits and dots, and after "update" at most three "summary" lines;
anything else is an error. What erssi shows of an error is printable
ASCII, at most 200 bytes. The summary comes from the message of the
release tag whose signature was checked; erssi removes control and
format characters from it (escape codes, bidi and zero-width marks) and
keeps at most 200 bytes of valid UTF-8 per line, so nothing from the
network reaches the terminal as a control sequence.
`~/.erssi/update-check.state` holds only times, version numbers, that
reason and the summary; it is written as a whole (mode 0600) and read
only when it is a regular file of your own, and the summary is cleaned
again when it is read.

`/update install` is the same installer with `--update`, the same
signature checks as `erssi --update`, started only by that command. It
runs without a terminal in a session of its own and is stopped after an
hour. Its output goes to `~/.erssi/update-install.log`, created mode 0600
(a link there is not followed) and locked while it runs; erssi shows only
the installer's step, warning and error lines, cleaned as the summary.
An install whose conf names another updater (`UPDATE_COMMAND`) is
refused rather than running that command.

## Configuration

`~/.erssi/config` is written readable only by you, to a temporary file
that replaces it only once every byte is written and synced: a save that
fails on a full disk or quota keeps the old file and says why. The debug
logs of the side panels and of terminal resizes (`/set sidepanel_debug on`,
`/set resize_debug on`) are readable only by you too: they are kept in
`~/.erssi`, never in `/tmp`, and never written through a link.

Files the fork reads or writes at a path that a setting, you or another
user on the box can influence go through one helper (`src/core/safe-file.c`);
the E2E keyring and replay memory, whose buffers must also be wiped, and
the fe-web socket lock open their files the same way in their own code:

- **Reading** — opened once without waiting (a FIFO or a device put there
  cannot hang erssi) and checked on that descriptor: anything but a regular
  file is refused, and the readers of whole files set a size limit. A symbolic link as the file itself is
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
  credentials loaded from the external file into the configuration -
  except while that file cannot be read or written: then a network or
  server you change keeps its passwords in `~/.erssi/config` until
  `/credential reload` succeeds.

The upstream code that reads `~/.erssi/config`, the startup file, themes,
the `/upgrade` session and `/cat` follows links as irssi always has.

## Backups

`erssi --backup` writes a file that holds everything an attacker needs to
be you: the E2E identity key and session keys (`rpe2e/keyring.json`), the
OTR keys, the fe-web key, and the passwords of your networks, SASL
accounts and fe-web (in `config` and `.credentials`, in plain text). It is
not encrypted. It is written with mode 0600 from its first byte (a file of
its own under another name, renamed only once complete), and the output
says it must be kept private. Keep it so: not in a web directory, not in a
shared or synced folder others can read, not on a box whose root you do not
trust; copy it with `scp`/`sftp`, and remove it once the move is done. To
keep it for longer, encrypt it, e.g. `age -p` or `gpg -c`. The automatic
backups (`backup_auto`) are the same files, kept in `backup_dir` (made
with mode 0700): choose a directory only you can read. Someone who got
a copy should be treated as having your keyring: `/e2e reset -force` and
new passwords.

`erssi --restore` takes an archive as untrusted input: it works on a copy
of its own, read once (the file you named may change meanwhile), refuses links,
hard links, devices, FIFOs, absolute names and `..` before anything is
unpacked, unpacks into a new directory (never over the home), checks the
result against its `MANIFEST`, makes only links that stay in the home
(each path once, never inside another link, checked again once made),
gives every file and directory your ownership and mode 0600/0700, and
only then swaps the home. Anyone who can give you an archive can still
give you their config, scripts and keys: restore only backups you made.

## Hardened build

By default: PIE, stack protector, stack clash protection,
`FORTIFY_SOURCE=2`, full RELRO.

## Tested the hard way

Everything fe-web reads from the network (the login request, WebSocket
frames, encrypted frames, client messages, IRC lines turned into web
events) is fuzzed with libFuzzer under AddressSanitizer and
UndefinedBehaviorSanitizer; every finding is replayed by `meson test`. The
image decoder of the image preview and irssi's parsers of server lines,
text formats, theme files and IRC parameters have fuzz targets too, and
`meson test` replays their seed corpora in every build. The test suite
also runs under both sanitizers.

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
