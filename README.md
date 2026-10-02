# erssi — y-o-o-z fork

[![Version](https://img.shields.io/badge/version-1.3.2-f59e0b.svg)](NEWS)
[![Upstream](https://img.shields.io/badge/erssi-1.3.1-30363d.svg)](https://github.com/erssi-org/erssi)
[![irssi](https://img.shields.io/badge/irssi-1.4.5-30363d.svg)](https://github.com/irssi/irssi)
[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](COPYING)

A fork of [erssi](https://github.com/erssi-org/erssi), the next-generation
irssi, tuned for daily use on IRCnet: a polished start theme, rank-colored
nicks, WHOIS that stays out of your channels, a fe-web server that a browser
client can drive exactly like the terminal, and scripts for IRC operators.

`main` is upstream erssi plus the changes below, one commit per change, each
explaining the problem it solves. Everything else — sidepanels, mouse
gestures, credential encryption, image preview, full irssi Perl script
compatibility — comes from erssi unchanged; see the
[upstream README](docs/ERSSI-UPSTREAM-README.md).

## Version

```
$ erssi --version
erssi 1.3.2 (20261002 1302)
```

| | |
|---|---|
| **erssi 1.3.2** | This fork: all of erssi 1.3.1 (erssi-org, 2026-04-06) plus the changes below. Release notes at the top of [NEWS](NEWS). |
| **irssi base** | irssi 1.4.5, the latest irssi release (2023-10-03), plus irssi `master` up to 2025-07-26 as merged by erssi-org. |
| **Perl scripts** | `Irssi::version()` returns the release date (`20261002`), so scripts that require irssi 1.4.5 or newer by date load. `$J` is erssi's own version (`1.3.2`): a script that compares `$J` with `1.4` needs a patch, as the bundled `rpe2e.pl` has. |

## What the fork changes

**Look and feel**

| Change | Why |
|---|---|
| `shellter` is the start theme | A dark theme in the colors of [shellter.me](https://shellter.me). Every line — messages, events, server replies, WHOIS (field labels in the column), notices, private messages — shares one 13-character column and separator, so text always starts in the same place and wrapped lines continue under it. Default `nick_column_width` 13, a matching `nick_hash_colors` palette and hilight colors. |
| `$nickmode` and `nick_mode_color_*` | The mode prefix is drawn in the color of the rank (`@` amber, `+` blue, `%` green, `~`/`&` red) in messages and in the nick list, so ops and voiced users stand out at a glance. |
| 24-bit `nick_hash_colors` | Nick colors can match a 24-bit theme; invalid palette entries are never picked. |
| `fe-text` section in themes | Themes written before 1.3.0 keep their sidepanel and statusbar formats. |
| Sidepanel names cut with `…` | Long names (`#bash.org.pl`) are no longer chopped mid-word by the border; scroll arrows sit outside the nick text. |
| Notices window keeps other windows | A script that creates its window at startup (e.g. *Mentions*) no longer loses it to the Notices window. |
| Quiet network windows, tidy `/NAMES` | Network windows no longer start with three lines of `/WINDOW` output; the `/NAMES` table has no trailing padding, which wrapped into blank lines after a terminal resize. |

**Behaviour**

| Change | Why |
|---|---|
| WHOIS/WHOWAS in the network window (`print_whois_rpl_in_server_window`, on) | Replies go to the status window of the network that asked (IRCnet, IRCnet2), not into the channel you are reading. |
| `anti_floodnet_notices` | Anti-Floodnet messages are local prints that `/ignore` cannot hide; this setting can. |
| No `g_debug` in recode | Every sent line used to show up as `GLib default debug: recode_out: …` in Notices. |
| 256-color fallback for 24-bit colors | erssi's new terminal backend always sent 24-bit color codes; GNU Screen and terminals without truecolor showed the theme without colors. Now, as in irssi 1.4.5, they get the nearest 256-color palette entry. |

**fe-web** (WebSocket server for browser clients)

| Change | Why |
|---|---|
| Commands with a `target` run in that window | `/e2e on`, `/topic`, `/kick` typed in a web channel window act on that channel instead of failing with “not in a channel”. |
| Unknown server tags are rejected | A command meant for a disconnected network is dropped rather than run on another one. |
| RFC 6455 Close handshake; lost TLS peer = plain disconnect | Restarting the web client no longer prints `SSL_ERROR_SSL … unexpected eof` in erssi. |
| React to fe-web settings only | Every unrelated `/set` re-applied fe-web and printed “WebSocket server started/stopped”. |
| `is_highlight` like the terminal | The web marks the same lines as mentions as the terminal does, `/me` included. |

**Bundled scripts** — installed to `<prefix>/share/irssi/scripts`, not loaded
by default (`/script load <name>`; autoload: symlink into
`~/.erssi/scripts/autorun/`). Script messages are in Polish.

| Script | What it does |
|---|---|
| `tk.pl` | Temporary K-lines for IRCnet operators (ircd 2.11 `TKLINE`): nick → WHOIS → mask, never a guessed host; mask types (ident, host, domain), refusal of overly broad masks, `-dry` preview, optional confirmation, JSONL audit log. `/tkl <nick\|user@host> [time] <reason>`, `/untkl`, `/tklist`, `/klist`, `/tk help`. |
| `skaner.pl` | Clones (same host) and IRC operators on a channel, reported in a *skaner* window after join, with alerts when a clone arrives. `/skaner [#channel\|all\|on\|off]`. |
| `mentions.pl` | One *Mentions* window for highlights (also nick mid-sentence), private messages, notices and DCC, mirrored to `~/.erssi/logs/mentions.log`. |
| `webjournal.pl` | Journals every window to `~/.erssi/journal` (JSONL, 0600, rotated) so a web client can show the same history and windows as the terminal. `/webjournal`. |

## Build and install (no root needed)

Dependencies (Debian/Ubuntu names): `meson ninja-build pkg-config gcc
libglib2.0-dev libssl-dev libperl-dev libutf8proc-dev
libgcrypt20-dev libotr5-dev libcurl4-openssl-dev libchafa-dev`.

```sh
git clone https://github.com/y-o-o-z/erssi.git && cd erssi
meson setup Build -Dprefix="$HOME/.local/opt/erssi" \
  -Dwith-perl=yes -Dwith-proxy=yes -Dwith-otr=yes -Dwith-fe-web=yes \
  -Dwith-image-preview=yes -Ddisable-utf8proc=no
ninja -C Build && meson test -C Build && ninja -C Build install
~/.local/opt/erssi/bin/erssi
```

Use `--prefix=/usr/local` and `sudo ninja -C Build install` for a system-wide
install. erssi keeps its configuration in `~/.erssi/`, so it coexists with
irssi.

## Configuration

```
/set theme shellter                       # already the default
/set nick_mode_color_voice %Z79C0FF%_     # rank colors (owner, op, halfop, voice, normal)
/set print_whois_rpl_in_server_window on  # off: WHOIS in the active window, as upstream
/set anti_floodnet_notices off            # hide Anti-Floodnet messages
/set colors_ansi_24bit on                 # default: 24-bit where detected, else 256 colors
```

**Terminals and multiplexers.** 24-bit colors are used when the terminal
announces them (`COLORTERM=truecolor`, kitty, Ghostty, WezTerm, iTerm2) and in
tmux; everywhere else the 256-color palette. In **GNU Screen** (4.x and 5.x)
erssi uses the 256-color palette, because screen drops 24-bit color codes
unless screen 5 runs with `truecolor on`; then `/set term_force_colors on`
switches erssi to 24-bit colors. Tested in GNU Screen 4.09 and 5.0.1 and in
tmux 3.5.

## Web client

fe-web plus [NexusIRC](https://github.com/kofany/nexus) gives a browser view
of the *same* erssi session: channels, queries, history, and erssi's own
windows (Notices, Mentions, script windows), with commands executed by erssi.

```
/set fe_web_password <long random secret>
/set fe_web_bind 127.0.0.1
/set fe_web_enabled on
/script load webjournal
/save
```

The NexusIRC patch series, build steps and reverse-proxy/tunnel setup are in
[contrib/nexusirc](contrib/nexusirc/README.md). Keep fe-web on `127.0.0.1`;
only the web client should be exposed, behind TLS.

## End-to-end encryption

[contrib/rpe2e](contrib/rpe2e/README.md) carries `rpe2e.pl`, the RPE2E v1.0
script from [repartee](https://github.com/outragedevs/repartee) (MIT), adapted
to load on erssi installed without root. It is wire-compatible with repartee
and WeeChat: `/e2e on` in a channel window encrypts that channel
(XChaCha20-Poly1305, per-sender keys, CTCP key exchange). A migration tool
moves an existing repartee identity and keys, so peers see no key change.
Includes an integration test with two real erssi instances.

## Tests

```sh
meson test -C Build                                # C unit tests
python3 contrib/rpe2e/test_rpe2e_from_repartee.py  # repartee key migration
# E2E between two real erssi on a local test server (needs tmux, Perl modules)
contrib/rpe2e/test/run-e2e-test.sh ~/.local/opt/erssi/bin/erssi contrib/rpe2e/rpe2e.pl
```

## Keeping up with upstream

```sh
git remote add upstream https://github.com/erssi-org/erssi.git
git fetch upstream && git merge upstream/main
```

## License

GPL-2.0-or-later, like irssi and erssi. `contrib/rpe2e/rpe2e.pl` is MIT
(repartee authors). Upstream: [erssi-org/erssi](https://github.com/erssi-org/erssi)
by the erssi-org team, built on [irssi](https://irssi.org).
