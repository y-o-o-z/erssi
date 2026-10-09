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
change defaults), section 4 (fork-only), the rxvt-unicode 256-colour part
of the scroll-arrow change (it needs the truecolor fallback of section 3),
the /HELP CREDENTIAL text (it describes the fork's credential refusal) and
the `~/.erssi/help` lookup order of /HELP.

## 3. Behind a setting

These change what users see. Upstream may want them only as an opt-in.
Line numbers are at `4833f418` (they may shift by a few lines in later commits).

| Commit | Change in the fork | Where | Upstream default to keep |
|---|---|---|---|
| `bdeb8084`, `47daae64` | 256-colour fallback for 24-bit colours (GNU Screen, Linux console, 8/16-colour terminals). `colors_ansi_24bit` default ON; new `term_truecolor auto/on/off` | fe-ansi/term-ansi.c:623-624 | `colors_ansi_24bit` OFF. Note: in v1.3.1 the ANSI backend writes 24-bit codes whatever this setting says. Taking the fallback with the default OFF would move every user to 256 colours. Either keep ON with `term_truecolor=auto`, or make the fallback depend on `term_truecolor` alone. |
| `0609e771` | WHOIS and WHOWAS replies go to the network's status window | fe-common/irc/fe-whois.c:453 | `print_whois_rpl_in_server_window` OFF |
| `9e393a2d` | join prints only the nick count line | fe-common/core/fe-channels.c:642 | `show_names_on_join_limit` 18 |
| `92c1a7ac` (fe-channels.c part) | alone on a channel: count line, no names table. Hard-coded, no setting | fe-common/core/fe-channels.c:115 | drop the `size <= 1` rule, or put it behind a setting |
| `f42ff98d` | `$nickmode` expando (mode prefix coloured by rank, new `nick_mode_color_*` settings); also theme default `shellter`, `nick_column_width` 13, shellter `nick_hash_colors` | fe-common/core/fe-expandos.c, fe-messages.c:769-782, themes.c:1477 | `theme` default, `nick_column_width` 10, `nick_hash_colors` "g r b m c y G C", `nick_mode_color_*` empty. `$nickmode` itself is additive |
| `1ea1bd1f` | 24-bit hilight colours | fe-common/core/hilight-text.c:790-791 | `hilight_color` %Y, `hilight_act_color` %M |
| `85e9a8c1` | `hidden_settings` default names a script's setting (`translate_deepl_key`) | fe-common/core/fe-settings.c:471 | "" |
| `69c0526e` (credential part) | credential encryption and the external file refused at runtime | core/credential.c:159, 199 | fix or remove the feature instead |
| `dee9923b` | `buildtype=debugoptimized`, PIE, stack protector, RELRO/now, `_FORTIFY_SOURCE=2` by default | meson.build:4, 635-644 | meson defaults; hardening behind a meson option. It also replaces a packager's `_FORTIFY_SOURCE=3` with 2 |
| `2336acab` | no server tag in any window bound to the line's server (also `/WINDOW SERVER -sticky` windows) | fe-common/core/formats.c | listed in 2.2; a setting is possible if upstream wants the tag there |
| `9559dad6` (fe-help.c) | /HELP reads ~/.erssi/help before help_path | fe-common/core/fe-help.c | listed in 2.2; upstream may prefer it opt-in |

Rule: keep upstream defaults in C. Ship the Shellter values in the
Shellter theme or default config instead.

## 4. Fork-only

Not meant for erssi main. 91 commits.

- Branding and version (26): `/VERSION` text, start banner, topic bar,
  `SV` alias, README, NEWS, screenshots, version numbers
  1.3.2 to 1.3.10. Commits: `c49a9b71 e697af65 8cd5e5a2 9d37292d 49d719ca
  0c66fc9e 15cea05c 33515325 db97f998 226007b5 2a0f363c 68f397e3 af29ea3f
  3ca236a5 cee9c77f cedeaea6 2878fac3 eca79190 6fb0c2b6 3740d250 82f691cf
  f8338ca0 054b1d87 394a075c ead8d021 4833f418`.
  One neutral idea inside: v1.3.1 `/VERSION` prints a fixed "erssi
  v1.1.0" (fe-common/core/fe-core-commands.c). The fork prints
  `PACKAGE_VERSION` there.
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

1. fe-web login pacing is global, not per peer. `fe-web-server.c:442-450`:
   one local user who sends a wrong password every 2 s takes every check
   slot, so the owner mostly gets 429. `fe-web-server.c:957-960`: a loop
   of new connections evicts the owner's pending one. This matters on
   TCP loopback on shared machines. Unix-socket mode
   (`fe_web_socket`) is not affected. Fix: per-uid accounting
   (`SO_PEERCRED` / `getpeereid`) or a strong hint to use the socket.
2. Duplicated code: `fe-ansi/sidepanels-text.c:49` and `:77` copy
   `unformat_24bit_line_color()` and `unformat()` from
   `fe-ansi/textbuffer-view.c:121` and `:165`. Export one helper. The
   include guard in `sidepanels-text.h:1` is still `IRSSI_FE_TEXT_...`.
3. The fuzz harness includes a .c file: `fe-fuzz/fe-web/fe-web-fuzz.c:15`
   includes `fe-web-server.c` to reach its static functions;
   `fe-web/meson.build` lists that file apart. Upstream may prefer an
   internal header or a test-only static library.
4. Version numbers collide with upstream: `meson.build:2` is `1.3.10`, and
   NEWS has `erssi-v1.3.2` to `erssi-v1.3.10`. These are erssi-org's own
   future numbers. (v1.3.1 itself says `1.3.0` in `meson.build`.) The
   fork needs a suffixed or separate scheme.
5. FreeBSD stale-socket check: `fe-web-server.c:1215` treats
   `ECONNREFUSED` as "nobody listens" and removes the socket. FreeBSD
   also returns `ECONNREFUSED` when a listener's queue is full, so a busy
   erssi of the same user can lose its socket. Not tested on FreeBSD.
6. Defaults in C: see section 3. The Shellter values are in C, not in a
   theme or default config.
7. Credential encryption is disabled at runtime (`core/credential.c:159`,
   `:199`), not fixed.
8. `printtext()` expands `%` codes in its arguments. Strings from a web
   client reach it, e.g. the server tag at `fe-web/fe-web-client.c:144`.
   Use `printtext_string()` or escape `%`. Reported in review, not
   reproduced here.
8. WebSocket: unmasked client frames are still accepted
   (`fe-web/fe-web-server.c:563`). RFC 6455 requires closing the
   connection. This is older than the fork.
9. `/UNLOAD fe_web` sent from a web client deinitialises fe-web while
    that client's frame loop runs. Older than the fork.
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
15. Fork-only: the default-file hash list is written with
    `g_file_set_contents()` and then `chmod()` (`fe-ansi/irssi.c:412-413`).
16. Commit hygiene: `18851c12`, `4b56253e` and `d4e22458` are review-fix
    commits that mix several fixes. `92c1a7ac` and `69c0526e` mix a fix with
    a behaviour change. Take them per file (section 6).

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
