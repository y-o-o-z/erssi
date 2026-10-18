# erssi Shellter Edition: changes for erssi main

This file is for the erssi-org team. It lists the changes of the fork
"erssi Shellter Edition" that erssi main can take, and how.

## 1. About the fork

- Repository: https://github.com/y-o-o-z/erssi, branch `main`, based on
  erssi-org main (`c591e6c0`, tag `v1.3.1` plus two erssi-org commits).
- erssi is the work of kofany and the erssi-org team. The fork only adds
  to it.
- Commits of the fork are authored as `y-o-o-z` (GitHub no-reply address)
  and signed with the SSH key in `utils/allowed_signers`.

## 2. Ready to take: pull requests

The neutral bug fixes and hardening are proposed as pull requests on
erssi-org/erssi, one per area. Each is based on erssi-org main, builds
with 0 warnings and passes `meson test` on its own and merged with all
the others (fe-web also under AddressSanitizer and
UndefinedBehaviorSanitizer). They change no default behaviour; the few
visible changes are described in each PR. Commit messages follow the
repository's commitlint rules.

| PR | Area | Commits |
|---|---|---|
| [#4](https://github.com/erssi-org/erssi/pull/4) | core: config file mode 0600, reconnect leak, recode logging, settings checksum UB, image-preview fetches only from public addresses with size limits | 7 |
| [#5](https://github.com/erssi-org/erssi/pull/5) | FreeBSD build, name resolution in Capsicum mode, IPv4 address copy | 4 |
| [#6](https://github.com/erssi-org/erssi/pull/6) | fe-common: old fe-text themes, nick colours, nick expandos, /NAMES padding, CAP LS level, server tags, 344 and NOTICE leaks, secret values in /SET, missing /HELP files | 11 |
| [#7](https://github.com/erssi-org/erssi/pull/7) | anti-floodnet: count senders, not messages; crash; bounded memory; notices can be hidden | 3 |
| [#8](https://github.com/erssi-org/erssi/pull/8) | sidepanels and fe-ansi: network windows, clipping, scroll arrows, themed lines, key input batches, memcpy from NULL | 17 |
| [#9](https://github.com/erssi-org/erssi/pull/9) | fe-web: login, frames, TLS, output queue, Unix socket, use after free and leaks, fuzz targets with corpora | 26 |
| [#10](https://github.com/erssi-org/erssi/pull/10) | build and tests: fe-fuzz paths, tests keep out of HOME, install.sh options | 3 |
| [#3](https://github.com/erssi-org/erssi/pull/3) | a native Mentions window (`fe-mentions.c`, `mentions-log.c`): private messages, actions, notices, DCC CHAT and channel hilights in one immortal window, a 0600 log, `/MENTIONS`, unit tests and a live test (the migration from `mentions.pl` is fork-only) | 4 |
| [#11](https://github.com/erssi-org/erssi/pull/11) | end-to-end encryption: the `e2e` module (RPE2E v1.0 with OpenSSL only, wire-compatible with repartee and `rpe2e.pl`), unit tests, fuzz targets, a live interop test, and three core fixes it needs (flood queue order, `motd_got` over `/upgrade`, the fe-fuzz build) | 5 |

Merge order does not matter. The only overlap: #7 and #9 both add one
line to `docs/help/meson.build` at the same place; keep both.

Not in the pull requests, on purpose: the changes of section 3 (they
change behaviour in C), the defaults of the next subsection (not yet in a
pull request), section 4 (fork-only), the rxvt-unicode 256-colour part
of the scroll-arrow change (it needs the truecolor fallback of the next subsection),
the /HELP CREDENTIAL text (it describes the fork's credential refusal) and
the `~/.erssi/help` lookup order of /HELP.

### Ready to take, not yet in a pull request: defaults back in C

Since 2026.10.0 these changes keep erssi-org's default in C. The Shellter
value is set in the default config (`irssi.conf`) of a new `~/.erssi`; a
config from before 2026.10.0 gets it written once at the first start (the
fork-only `shellter-defaults.c`, section 4). Each row can be taken as it is.

| Commits | Change | Default in C (erssi-org) | Shellter value (irssi.conf) |
|---|---|---|---|
| `b9395ce7 f726253c` + `630cbcda` | 256-colour fallback for 24-bit colours (GNU Screen, Linux console, 8/16-colour terminals); new `term_truecolor auto/on/off` | `colors_ansi_24bit` OFF; with OFF, the fallback gives upstream users 256 colours. v1.3.1 wrote 24-bit codes at start whatever the setting said, so upstream should decide | `"fe-ansi"`: `colors_ansi_24bit` yes |
| `5a80bce2` + `630cbcda` | WHOIS and WHOWAS replies go to the network's status window | `print_whois_rpl_in_server_window` OFF | ON |
| `c92e5631` + `630cbcda` | join prints only the nick count line | `show_names_on_join_limit` 18 | 1 |
| `55aed0a0` (fe-channels.c) + `bd79c1ad` + `630cbcda` | alone on a channel: count line, no names table. `bd79c1ad` replaces the hard-coded rule with the setting `show_names_on_join_alone` | `show_names_on_join_alone` ON, as v1.3.1 | no |
| `8d8aeafa` + `630cbcda` | `$nickmode` expando (mode prefix coloured by rank, new `nick_mode_color_*` settings), theme default, `nick_column_width`, `nick_hash_colors` | `theme` `default`, `nick_column_width` 10, `nick_hash_colors` "g r b m c y G C", `nick_mode_color_*` "". `$nickmode` itself is additive | theme `shellter`, width 13, the shellter palette, rank colours set |
| `c9326d3f` + `630cbcda` | 24-bit hilight colours | `hilight_color` `%Y`, `hilight_act_color` `%M` | `%Zbb9af7`, `%ZFF7B72` |
| `d6cde470` + `630cbcda` | `hidden_settings` default named a script's setting | `hidden_settings` "" (PR #6 already has `""`, checked with `gh pr diff 6`) | `translate_deepl_key` |

`630cbcda` is the take commit for all rows; `86420447` is its test
follow-up (the defaults tests skip when a global `irssi.conf` is
installed). `bd79c1ad` touches `src/fe-common/core/fe-channels.h` and
the setting. To see one feature, `git show 630cbcda -- <file>` (section 6),
for example `git show 630cbcda -- src/fe-common/core/themes.c`.

### Ready to take, not yet in a pull request: features of 2026.10.0

These are new features with their own tests. Off by default, except
the Mentions window (`mentions_window` is "Mentions" in C: every start
creates a background window) and the first-run setup (it asks on a fresh
home when a terminal is attached, and saves a config). Each row can be
taken as it is; the commits are on `dev/2026.10`.

| Commits | Change | Notes |
|---|---|---|
| `0eb382c0 3d104761 4e0d9f86` | `/HELP $name` shows the help of an expando; help texts for `term_truecolor`, `colors_ansi_24bit`, `print_whois_rpl_in_server_window`, `nick_mode_color_*`, `$nickmode`, `show_names_on_join_limit` | The texts of settings that exist only in the fork (`term_truecolor`, `nick_mode_color_*`, `$nickmode`) go with the rows of section 2 that add them |
| `7a637932 405be774` and the Mentions commits (branch `pr3-mentions`: `4c72ebd0 b52076e2 c97952ee 95163304`) | A native Mentions window: private messages, actions, notices, DCC CHAT and channel hilights in one immortal window, a 0600 log, `/MENTIONS`, the `mention` signal, settings `mentions_*`, unit tests and a live test. The branch is the update of pull request #3 | Based on `c591e6c0`; it also carries the Python live-test harness (`tests/live`). Fork-only follow-ups are not in the branch: the queue test before "irssi init finished", the `mentions.pl` migration (section 4) |
| `dce35b88 35677d69 79b16c72` | Terminal notifications: OSC 9 / OSC 777 sequences when a mention or private message arrives while the terminal is not focused (focus reports, tmux pass-through, throttle, control characters removed). New setting `term_notify` | `79b16c72` is neutral: the default of `term_notify` is `off`. The fork's default is a separate commit (`e1ebcbf6`, section 4). Follow-ups: `40a9a14b` (ASCII only on a terminal that is not UTF-8), `e054bdf2` (bidi and line-separator characters removed), `bba0a32c` (tests) |
| `c68686b8 5f5cdc09` | Key input: `gui-mouse.c` dropped the final letter of a CSI sequence (`ESC [ I` Page Up, `ESC [ Z` Shift-Tab, `ESC [ G`) and waited for more after a CSI that ends in a lowercase letter or `@` (rxvt shift-arrows, CSI u) | Bug fixes in code of v1.3.1, with a live test (`tests/live/test_keys.py`). Independent of the rest |
| `09e1ffbb ba922f25 2572f718 d0147b2d c6d57767 6bf30a5d 717d719b 9935c94f 56703955 464ea50b` | First-run setup on a fresh home (network, nick, SASL, theme) and `erssi --setup`. The password is read without echo, ^C and ^Z work, SASL is offered only for a TLS server that is verified. New core helper `settings_get_builtin_str()` | Fork-specific parts: the start-up hint for the nick that comes from the `erssi_user` default of the fork's `irssi.conf`, and the values the unit test expects for the fork's settings (theme `shellter`); both are easy to drop or generalise. The default network offered is "IRCnet" (`firstrun.c`), also a fork preference |
| `8f1e1437 f3092102 52643c1a db95a558 a3885e6b` | `/SEARCH` in the autolog files: a search in a child process with a hit, size and file limit and a 120 s time limit; new setting `search_max_size` | The search is not available in Capsicum mode. Live test: `tests/live/test_search.py` |

## 3. Behind a setting

These change what users see. Upstream may want them only as an opt-in.
Line numbers are at `fe7bffe2` (they may shift by a few lines in later commits).

| Commit | Change in the fork | Where | Upstream default to keep |
|---|---|---|---|
| `03ce4455` (credential part) | credential encryption and the external file refused at runtime | core/credential.c:159, 199 | fix or remove the feature instead |
| `fff42e6f` | `buildtype=debugoptimized`, PIE, stack protector, RELRO/now, `_FORTIFY_SOURCE=2` by default | meson.build:4, 635-644 | meson defaults; hardening behind a meson option. It also replaces a packager's `_FORTIFY_SOURCE=3` with 2 |
| `3a03a593` | no server tag in any window bound to the line's server (also `/WINDOW SERVER -sticky` windows) | fe-common/core/formats.c | listed in 2.2; a setting is possible if upstream wants the tag there |
| `b19f6d84` (fe-help.c) | /HELP reads ~/.erssi/help before help_path | fe-common/core/fe-help.c | listed in 2.2; upstream may prefer it opt-in |

Since 2026.10.0 the fork keeps erssi-org defaults in C (section 2); the rows below still change behaviour in C.

## 4. Fork-only

Not meant for erssi main.

- Branding and version (31): `/VERSION` text, start banner, topic bar,
  `SV` alias, README, NEWS, screenshots, version numbers
  1.3.2 to 1.3.10, calendar versions from 2026.10.0 (`9a6ed0b5 3936782b
  7afb839c bda87f42 1d61b673`). Commits: `2f755752 4c658266 137cf19a e5d1ec5f 295f554b
  18e796f7 36355a5f 81859846 bca533e7 23c82af4 f6d44f8f 6964814a 186c893c
  dd41e167 0ad17cb3 ec182dac 5df34abc 7475d758 f6d162ef 07fd5358 2a1a9791
  98b35ab9 4b665223 98f83298 a3ffd532 fe7bffe2`.
  One neutral idea inside: v1.3.1 `/VERSION` prints a fixed "erssi
  v1.1.0" (fe-common/core/fe-core-commands.c). The fork prints
  `PACKAGE_VERSION` there.
- Shellter values of former defaults (fork-only, 3): `src/fe-common/core/shellter-defaults.c`
  writes them once into an older config (`5cb40073 53b534de`);
  `irssi.conf` carries them for a new `~/.erssi` (`48c40307`).
- Shellter themes (13): `themes/shellter.theme`, `themes/shellter-light.theme`
  and `shellter` as the default theme. They could ship as extra themes
  after cleanup (section 5). Commits: `e4365483 7b39166f 59ebeeb7 02b3e85f
  32f19a96 e9c13dc6 c4f535b6 11701c9c 8d992a73 c3f90edf 36ed3647 7589854a
  66fc60de`.
- Installer, updates, signing, pre-push, CI (18):
  `shellter-install.sh`, `erssi --update` / `--check-update`
  (`src/fe-ansi/irssi.c`, `ERSSI_PREFIX`), signed release tags,
  `utils/allowed_signers`, `utils/pre-push`,
  `.github/workflows/shellter-ci.yml`, and the removal of erssi-org's
  release, pages and packaging workflows. Commits: `c21ae4e3 67159a44
  d1836e0f a2932e97 3d062e72 6dbf4b63 fda5145d 64627a0d c5832501 3b90a145
  7f8f2280 17ccd17d 326aad4c 8c839cdf 9a679bde 7eb407bc 5810ba38 034b957e`.
- ~/.erssi default files updated like package conffiles (2): `a3e00378`
  `65c109e7`, with `src/fe-ansi/default-files-known.h` (hashes of the
  Shellter files) and `utils/default-file-hashes.sh`. Upstream could
  want the mechanism, but behind a setting and with the hash list made
  at build time, not committed.
- Bundled scripts (12): `scripts/mentions.pl` (now only a stub that
  unloads itself in favour of the built-in window), `scripts/trackbar.pl`,
  `scripts/webjournal.pl` with their help files in `scripts/help/`.
  They are separate scripts. Upstream may or may not want them.
  The author's own scripts were removed (`e97edf72`). Commits: `cd3e6246
  1fc4bae4 1a66c953 d57d4d4f eef343d4 393dae3d c33e18e6 23216f09
  e97edf72 1fcedd8b`.
- End-to-end encryption: the `e2e` module (`src/e2e`, `-Dwith-e2e`) speaks
  RPE2E v1.0 natively with OpenSSL only; it replaced the Perl script that
  was in `contrib/rpe2e` (only the repartee migration tool and the MIT
  licence stay there). It is proposed upstream as PR #11
  (branch `upstream/e2e`, based on erssi-org main). Before that, the contrib history: A patch series for a
  third-party web client was added and later removed (`e97edf72`).
  Commits: `e97c5fec 12fa67b5 216037a5 069bedf9`.
- Housekeeping (2): `fe870095` removes erssi-org development files and
  workflows; `d7b01c56` changes example names in fe-web comments.
  If you take `6ceb578a`, `d7b01c56` gives its comment a neutral
  example name.

- Mentions window, shipped defaults and migration: `scripts/mentions.pl`
  became a stub, and the first start of a config that used the script moves
  its settings and log to the built-in window (`bab6476a 6f414985
  fec18987`). `irssi.conf` for a new `~/.erssi` turns `mentions_log` on
  (`0bc04638 a2cedecd`). A queue test for the Mentions window before
  "irssi init finished" stays here too (`79159d94`).
- Shellter values in `irssi.conf` for the new features: `term_notify auto`
  (`e1ebcbf6 5a6c8a2b`), `mentions_log` on, and the nick `erssi_user`
  and theme `shellter` that the first-run setup starts from. A new home gets
  them; a home that exists keeps what it has. Everything in `irssi.conf` is the fork's choice.
- Test and hook housekeeping: `utils/pre-push` runs the tests with a
  throwaway HOME (`b421f94c`); `2307033a` (a warning when the E2E module is
  loaded for one session only).

## 5. Known gaps

Open at `fe7bffe2`. Each item was checked in the code.

1. fe-web, the rest of the per-peer work (login pacing and the login
   queue are fixed, see below: a loop of new connections evicts its own
   oldest pending connection, and a peer that logged in once keeps only
   its oldest one). Outside Linux, or when the kernel cannot tell a
   loopback peer's uid, every loopback peer is one peer (`ip:loopback`,
   all of 127/8 and `::1`) and they share one pacing bucket (fe-web
   prints a hint to use `fe_web_socket` then). A local user who connects
   to a non-loopback address of the machine (its LAN or public IP)
   counts as that address, not as a uid: only loopback peers are looked
   up. A proxy that runs as
   the owner's user (a reverse proxy, socat, an `ssh -L` tunnel other
   users can reach) connects as the owner, so every client behind it
   counts as the owner and is never paced: such a proxy must sign its own
   users in. NexusIRC on the Unix socket does (its own sign-in, the
   fe-web password stored on its side). Residual risk
   that stays: during an attack, a peer that never logged in before and
   sends one wrong password (a typo) from a new address is a suspect, and
   the shared ceiling holds it for up to about 10 minutes, until its
   table entry is forgotten (`FE_WEB_PEER_MEMORY`, 600 s after its last
   wrong password). When the attack fills the table of 256 suspect peers,
   such a peer on a new address shares the overflow bucket with the
   attacker even without a typo. Either way a remote owner may get mostly
   429. For remote access use the Unix socket through an SSH tunnel
   (`ssh -L`), or log in once from the address beforehand.
2. Resolved in 2026.10.0: the unformat code. `fe-ansi/textbuffer-unformat.h`
   holds the one copy (`textbuffer_unformat()`, `textbuffer_read_unichar()`)
   for `textbuffer-view.c` and `sidepanels-text.c`; the include guard of
   `sidepanels-text.h` is `IRSSI_FE_ANSI_SIDEPANELS_TEXT_H`.
3. The fuzz harness includes a .c file: `fe-fuzz/fe-web/fe-web-fuzz.c:15`
   includes `fe-web-server.c` to reach its static functions;
   `fe-web/meson.build` lists that file apart. Upstream may prefer an
   internal header or a test-only static library.
4. Resolved in 2026.10.0: calendar versions, NEWS headers `shellter-v…`
   (`meson.build` is `2026.10.0`; NEWS has `shellter-v1.3.2` to
   `shellter-v1.3.10` and `shellter-v2026.10.0`, no longer erssi-org's own
   future numbers).
5. The stale-socket check on FreeBSD is fixed (see "Fixed since"). Not
   tested on FreeBSD itself.
6. Resolved in 2026.10.0: defaults in C. The Shellter values are in the
   default config (section 2).
7. Credential encryption is disabled at runtime (`core/credential.c:159`,
   `:199`), not fixed.
8. `printtext()` expands `%` codes in its arguments. Strings from a web
   client reach it, e.g. the server tag at `fe-web/fe-web-client.c:144`.
   Use `printtext_string()` or escape `%`. Reported in review, not
   reproduced here.
9. Capsicum: after `/capsicum enter`, a fe-web restart cannot bind or
    write its certificate. Not documented in `/help fe_web`.
10. image-preview: the address filter (`image-preview-fetch.c:194`) does
    not cover Teredo `2001::/32` or the documentation ranges. Terminal
    detection matches loose substrings ("rio", "foot") in
    `image-preview-chafa.c:98`, `:100`.
11. Missing /help text for the `sidepanel_scroll_arrow` format. It is
    described only in README and NEWS. (The settings `term_truecolor`,
    `colors_ansi_24bit`, `print_whois_rpl_in_server_window`,
    `nick_mode_color_*`, `show_names_on_join_limit` and `$nickmode` now
    have /help texts.)
12. Tested since 2026.10.0: the fe-web output queue (partial and refused
    writes, TLS WANT_READ, 32 MB cap), Unix-socket path checks, login
    pacing and eviction, frames, the `/UNLOAD` guard,
    `fe_settings_is_secret()` and /SET, anti-floodnet, truecolor
    detection, the default-file update, the unformat helper and the
    installer. Still not covered: the credential refusal and the Capsicum
    resolver.
13. The shellter themes still carry format sections for scripts erssi
    does not ship, including the author's own (`themes/shellter.theme:753-880`,
    same in `shellter-light.theme`). Remove them before shipping the themes.
14. Commit hygiene: `12144243`, `b71b2ca1` and `1f96101e` are review-fix
    commits that mix several fixes. `55aed0a0` and `03ce4455` mix a fix with
    a behaviour change. Take them per file (section 6).
15. The `"fe-text"` block of `irssi.conf` is ignored since fe-text was
    merged into fe-ansi (`scrollback_lines`, `paste_verify_line_count`,
    `lag_min_show`, `term_force_colors`, `colors`, `actlist_sort`). Moving
    it to `"fe-ansi"` would change two values that users see (2000 lines,
    2), so it is left as is.

Fixed since `fe7bffe2` (the unmasked client frames and `/UNLOAD` of fe-web
from a web client were older than the fork; the rest is fork-only):

- fe-web login pacing is per peer: uid on the Unix socket, address (/64
  for IPv6) on TCP; on TCP loopback (Linux) the peer's uid, asked of the
  kernel for the one socket (NETLINK_SOCK_DIAG), else from /proc/net/tcp
  at most twice a second; elsewhere one shared `ip:loopback`, with a hint
  to use fe_web_socket; your own uid is never paced. A ceiling holds all
  suspect peers (a wrong password in the last 10 minutes) together to one
  check every 2 s once 20 wrong passwords come within a minute, so many
  addresses get about one check a second in all; clean peers and the
  last 32 that logged in are not held by it.
- fe-web's login queue (8 connections that did not log in yet): a new
  connection closes one that is already closing, else the oldest pending
  connection of the busiest peer, not the oldest of anyone, so a loop of
  new connections evicts its own (`fe_web_pending_victim()`). The
  owner's uid is never chosen while another peer is waiting, so many
  addresses with one connection each, which tie with the owner, close
  each other's. A trusted peer (one that logged in) keeps only its
  oldest pending connection, so an address it shares with others (NAT, a
  /64, `ip:loopback`) cannot flood the queue and push the owner out; when
  only kept connections wait, the same rule applies among them.
- fe-web holds a lock on <socket>.lock; an old socket is removed only
  when the lock is free (or, on Linux, nothing listens). The lock is a
  `flock()` the kernel drops when erssi dies, so a lock file left by a
  killed erssi holds no start; a lock file that is a symbolic link, not
  a regular file of the user's, or open to others (mode not 0600) is
  refused, not followed and not changed. Before, an
  `ECONNREFUSED` meant "nobody listens", and FreeBSD (like the other BSDs
  and macOS) also returns it when a listener's queue is full, so a second
  erssi removed the socket of a busy one. Without file locks (some network
  file systems) the BSDs keep an old socket and say so.
- fe-web closes the connection on a client frame that is not masked
  (RFC 6455 5.1), with a close frame of status 1002 (protocol error),
  whatever the opcode (text, binary, continuation, ping, pong, close).
  It is refused on its header, before its payload is waited for; nothing
  of it and nothing after it in the same read is handled (no command, no
  pong, no echo of a close status), and what the client sends until it
  is gone is not read. Before, an unmasked text or binary frame was
  ignored, an unmasked ping was answered and an unmasked close echoed.
  Browsers always mask, so does the `ws` client of NexusIRC.
- fe-web refuses `/UNLOAD` of itself (`/unload web`, `/unload web fe`)
  while it handles a web client's message, with a message in the window
  the command was for. Before, it deinitialised fe-web, which freed the
  client whose frame loop was running the command, and closed fe-web's
  code under it: any logged-in web client could crash erssi. The guard is
  a `command_bind_first("unload")` handler that acts only while
  `fe_web_client_handle_message()` is on the stack (a depth counter), for
  commands and for the other messages (`close_query`, `mark_read`,
  `network_add`, ...), whose effects reach scripts' signal handlers. So
  an alias, `/EVAL`, an abbreviation (`/UNL`), upper case, another
  command character or a script's `signal_emit("command unload")` are
  refused too. Typed in erssi, `/unload web` still runs; `/unload` of
  other modules from a web client is not changed. `/QUIT` and `/UPGRADE`
  from a web client were safe already: they only ask the main loop to
  stop, and the modules are unloaded after it. This fixes the `/UNLOAD`
  path only: a logged-in web client can still run any command (`/exec`,
  `/script exec`, `/set`), so access to fe-web equals access to the
  terminal - guard the fe-web password and socket like a shell login.
- The update of the files erssi ships into `~/.erssi` (themes, startup)
  is its own unit, `fe-ansi/default-files.c`, with tests
  (`tests/fe-ansi/test-default-files.c`); `irssi.c` keeps the walk over
  the shipped files. The hash list `default-files.sha256` is written like
  the files: a temporary file in `~/.erssi` with mode 0600 from the start,
  written out to the disk (`fsync()`), then renamed over the old one.
  Before, `g_file_set_contents()` created it with the umask's mode and a
  `chmod()` set 0600 afterwards. A FIFO swapped in for a file after the
  `lstat()` is opened without blocking and refused, so it cannot hang the
  start; a file's sha256 is computed as it is read, not on a copy in
  memory. The list is read the same way and only up to 64 KiB: a list
  that is a link, a FIFO or larger counts as no record, so the files are
  kept (with a note) and the list written at the end replaces it with a
  regular 0600 file. A file whose replacement fails is not recorded as
  replaced.
- `shellter-install.sh` (fork-only): ASCII spinner and symbols when the
  locale is not UTF-8; the `~/.local/bin` link only for a prefix in your
  home, never as root, never over the binary or over a file; the source
  lock is taken before the venv; FreeBSD's base OpenSSL is enough; the
  source directory and its venv must belong to you, every directory above
  them to you or root, and all be writable only by you (a root-owned sticky directory
  such as /tmp is allowed above the parent of --src, never as its parent),
  otherwise the install is refused with the fix in the message; in a user
  namespace `SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1` counts the overflow uid
  (how the directories of root and of every other unmapped user show there,
  and possibly a mapped user such as rootless podman's nobody) as root's
  above the parent of --src only, so every unmapped user's directories there
  are trusted, not only root's (the namespace is read from
  /proc/self/uid_map; nothing without /proc); a namespace that does not map
  the user's own uid is refused.
- Tests and fuzz harnesses that start the core use their own temporary
  home.

## 6. How to take a change that is not in a pull request

```sh
git remote add shellter https://github.com/y-o-o-z/erssi.git
git fetch shellter main
git cherry-pick <commit>                       # a commit that applies alone
git show <commit> -- src | git apply -3 --index && git commit -c <commit>
                                               # only some paths of a commit
```

A cherry-pick makes a new commit, so the SSH signature does not carry
over.
