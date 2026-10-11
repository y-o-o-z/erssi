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
| [#3](https://github.com/erssi-org/erssi/pull/3) | an older proposal: a native Mentions window | 3 |

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
| `bdeb8084 47daae64` + `6c8c3adb` | 256-colour fallback for 24-bit colours (GNU Screen, Linux console, 8/16-colour terminals); new `term_truecolor auto/on/off` | `colors_ansi_24bit` OFF; with OFF, the fallback gives upstream users 256 colours. v1.3.1 wrote 24-bit codes at start whatever the setting said, so upstream should decide | `"fe-ansi"`: `colors_ansi_24bit` yes |
| `0609e771` + `6c8c3adb` | WHOIS and WHOWAS replies go to the network's status window | `print_whois_rpl_in_server_window` OFF | ON |
| `9e393a2d` + `6c8c3adb` | join prints only the nick count line | `show_names_on_join_limit` 18 | 1 |
| `92c1a7ac` (fe-channels.c) + `ca68b1df` + `6c8c3adb` | alone on a channel: count line, no names table. `ca68b1df` replaces the hard-coded rule with the setting `show_names_on_join_alone` | `show_names_on_join_alone` ON, as v1.3.1 | no |
| `f42ff98d` + `6c8c3adb` | `$nickmode` expando (mode prefix coloured by rank, new `nick_mode_color_*` settings), theme default, `nick_column_width`, `nick_hash_colors` | `theme` `default`, `nick_column_width` 10, `nick_hash_colors` "g r b m c y G C", `nick_mode_color_*` "". `$nickmode` itself is additive | theme `shellter`, width 13, the shellter palette, rank colours set |
| `1ea1bd1f` + `6c8c3adb` | 24-bit hilight colours | `hilight_color` `%Y`, `hilight_act_color` `%M` | `%Zbb9af7`, `%ZFF7B72` |
| `85e9a8c1` + `6c8c3adb` | `hidden_settings` default named a script's setting | `hidden_settings` "" (PR #6 already has `""`, checked with `gh pr diff 6`) | `translate_deepl_key` |

`6c8c3adb` is the take commit for all rows; `0f11cb35` is its test
follow-up (the defaults tests skip when a global `irssi.conf` is
installed). `ca68b1df` touches `src/fe-common/core/fe-channels.h` and
the setting. To see one feature, `git show 6c8c3adb -- <file>` (section 6),
for example `git show 6c8c3adb -- src/fe-common/core/themes.c`.

## 3. Behind a setting

These change what users see. Upstream may want them only as an opt-in.
Line numbers are at `4833f418` (they may shift by a few lines in later commits).

| Commit | Change in the fork | Where | Upstream default to keep |
|---|---|---|---|
| `69c0526e` (credential part) | credential encryption and the external file refused at runtime | core/credential.c:159, 199 | fix or remove the feature instead |
| `dee9923b` | `buildtype=debugoptimized`, PIE, stack protector, RELRO/now, `_FORTIFY_SOURCE=2` by default | meson.build:4, 635-644 | meson defaults; hardening behind a meson option. It also replaces a packager's `_FORTIFY_SOURCE=3` with 2 |
| `2336acab` | no server tag in any window bound to the line's server (also `/WINDOW SERVER -sticky` windows) | fe-common/core/formats.c | listed in 2.2; a setting is possible if upstream wants the tag there |
| `9559dad6` (fe-help.c) | /HELP reads ~/.erssi/help before help_path | fe-common/core/fe-help.c | listed in 2.2; upstream may prefer it opt-in |

Since 2026.10.0 the fork keeps erssi-org defaults in C (section 2); the rows below still change behaviour in C.

## 4. Fork-only

Not meant for erssi main. 99 commits.

- Branding and version (31): `/VERSION` text, start banner, topic bar,
  `SV` alias, README, NEWS, screenshots, version numbers
  1.3.2 to 1.3.10, calendar versions from 2026.10.0 (`786dd409 0656826a
  f5874039 3aab4339 f5eb5c48`). Commits: `c49a9b71 e697af65 8cd5e5a2 9d37292d 49d719ca
  0c66fc9e 15cea05c 33515325 db97f998 226007b5 2a0f363c 68f397e3 af29ea3f
  3ca236a5 cee9c77f cedeaea6 2878fac3 eca79190 6fb0c2b6 3740d250 82f691cf
  f8338ca0 054b1d87 394a075c ead8d021 4833f418`.
  One neutral idea inside: v1.3.1 `/VERSION` prints a fixed "erssi
  v1.1.0" (fe-common/core/fe-core-commands.c). The fork prints
  `PACKAGE_VERSION` there.
- Shellter values of former defaults (fork-only, 3): `src/fe-common/core/shellter-defaults.c`
  writes them once into an older config (`5ad348a8 e8d46d0e`);
  `irssi.conf` carries them for a new `~/.erssi` (`e33eae52`).
- Shellter themes (13): `themes/shellter.theme`, `themes/shellter-light.theme`
  and `shellter` as the default theme. They could ship as extra themes
  after cleanup (section 5). Commits: `e4365483 7c067efc 1321d58a 640c686b
  777a4f56 0abc2d0b c778ce16 1224aa27 0ef6484a 49f8edad 1a454ea6 3254ed4b
  8c74d6ba`.
- Installer, updates, signing, pre-push, CI (18):
  `shellter-install.sh`, `erssi --update` / `--check-update`
  (`src/fe-ansi/irssi.c`, `ERSSI_PREFIX`), signed release tags,
  `utils/allowed_signers`, `utils/pre-push`,
  `.github/workflows/shellter-ci.yml`, and the removal of erssi-org's
  release, pages and packaging workflows. Commits: `5bd16a53 c5437584
  6360dcfb 0978e48b 8305143f 70234b8f 8c32f909 c9a97110 5fdda940 fd01960b
  d97d9d96 af4295f3 3796e493 09e6bc4a d58cc468 af077c93 42baac67 356d7e06`.
- ~/.erssi default files updated like package conffiles (2): `0b5e9b5d`
  `0a64bda5`, with `src/fe-ansi/default-files-known.h` (hashes of the
  Shellter files) and `utils/default-file-hashes.sh`. Upstream could
  want the mechanism, but behind a setting and with the hash list made
  at build time, not committed.
- Bundled scripts (12): `scripts/mentions.pl`, `scripts/trackbar.pl`,
  `scripts/webjournal.pl` with their help files in `scripts/help/`.
  They are separate scripts. Upstream may or may not want them.
  The author's own scripts were removed (`56f5f294`). Commits: `e98c9986
  7335c86d d022c06b c77acd0f 5ce58555 914e7f09 deec5efa 643d8dca 688d84ca
  ad09be31 56f5f294 e37805dc`.
- End-to-end encryption: the `e2e` module (`src/e2e`, `-Dwith-e2e`) speaks
  RPE2E v1.0 natively with OpenSSL only; it replaced the Perl script that
  was in `contrib/rpe2e` (only the repartee migration tool and the MIT
  licence stay there). Upstream may or may not want it; it could be
  proposed as a separate PR. Before that, the contrib history: A patch series for a
  third-party web client was added and later removed (`56f5f294`).
  Commits: `ec8861b9 2739e24e 60bb924f 02900930 ffa2d3cf 3fc9dead
  1dac3524 6526c1d3 ea42643b 581745f6 8bdd53cd f29f28a7 9605def3 0be4b809
  bdfa60ef 3cc9f4cc 3635ec86 f92cc8f5`.
- Housekeeping (2): `85cbe5eb` removes erssi-org development files and
  workflows; `7fb91b78` changes example names in fe-web comments.
  If you take `6ceb578a`, `7fb91b78` gives its comment a neutral
  example name.

## 5. Known gaps

Open at `4833f418`. Each item was checked in the code.

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
2. Duplicated code: `fe-ansi/sidepanels-text.c:49` and `:77` copy
   `unformat_24bit_line_color()` and `unformat()` from
   `fe-ansi/textbuffer-view.c:121` and `:165`. Export one helper. The
   include guard in `sidepanels-text.h:1` is still `IRSSI_FE_TEXT_...`.
3. The fuzz harness includes a .c file: `fe-fuzz/fe-web/fe-web-fuzz.c:15`
   includes `fe-web-server.c` to reach its static functions;
   `fe-web/meson.build` lists that file apart. Upstream may prefer an
   internal header or a test-only static library.
4. Resolved in 2026.10.0: calendar versions, NEWS headers `shellter-v…`
   (`meson.build` is `2026.10.0`; NEWS has `shellter-v1.3.2` to
   `shellter-v1.3.10` and `shellter-v2026.10.0`, no longer erssi-org's own
   future numbers).
5. Fixed: the stale-socket check on FreeBSD (see below). Not tested on
   FreeBSD itself.
6. Resolved in 2026.10.0: defaults in C. The Shellter values are in the
   default config (section 2).
7. Credential encryption is disabled at runtime (`core/credential.c:159`,
   `:199`), not fixed.
8. `printtext()` expands `%` codes in its arguments. Strings from a web
   client reach it, e.g. the server tag at `fe-web/fe-web-client.c:144`.
   Use `printtext_string()` or escape `%`. Reported in review, not
   reproduced here.
8. Fixed: unmasked client frames (see below). This was older than the
   fork.
9. Fixed: `/UNLOAD` of fe-web sent from a web client (see below). This
    was older than the fork.
10. Capsicum: after `/capsicum enter`, a fe-web restart cannot bind or
    write its certificate. Not documented in `/help fe_web`.
11. image-preview: the address filter (`image-preview-fetch.c:194`) does
    not cover Teredo `2001::/32` or the documentation ranges. Terminal
    detection matches loose substrings ("rio", "foot") in
    `image-preview-chafa.c:98`, `:100`.
12. Missing /help text for `term_truecolor`, `colors_ansi_24bit`,
    `print_whois_rpl_in_server_window`, `nick_mode_color_*`, `$nickmode`,
    `show_names_on_join_limit` and the `sidepanel_scroll_arrow` format.
    They are described only in README and NEWS.
13. Not covered by tests: login pacing and eviction, the output queue
    (partial and WANT_WRITE writes, 32 MB cap), Unix-socket path checks,
    the default-file update, `fe_settings_is_secret()`, the credential
    refusal, the anti-floodnet changes, truecolor detection, the Capsicum
    resolver.
14. The shellter themes still carry format sections for scripts erssi
    does not ship, including the author's own (`themes/shellter.theme:753-880`,
    same in `shellter-light.theme`). Remove them before shipping the themes.
15. Fixed: the default-file hash list is 0600 from the start (see below).
    Fork-only.
16. Commit hygiene: `18851c12`, `4b56253e` and `d4e22458` are review-fix
    commits that mix several fixes. `92c1a7ac` and `69c0526e` mix a fix with
    a behaviour change. Take them per file (section 6).
17. The `"fe-text"` block of `irssi.conf` is ignored since fe-text was
    merged into fe-ansi (`scrollback_lines`, `paste_verify_line_count`,
    `lag_min_show`, `term_force_colors`, `colors`, `actlist_sort`). Moving
    it to `"fe-ansi"` would change two values that users see (2000 lines,
    2), so it is left as is.

Fixed since `4833f418`:

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
  memory.

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
