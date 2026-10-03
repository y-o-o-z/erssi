# erssi Shellter Edition

**Your shell, your shelter — on IRCnet.**

[![Release](https://img.shields.io/badge/release-1.3.2-f59e0b.svg)](https://github.com/y-o-o-z/erssi/releases)
[![erssi](https://img.shields.io/badge/based_on-erssi_1.3.1-30363d.svg)](https://github.com/erssi-org/erssi)
[![irssi](https://img.shields.io/badge/core-irssi_1.4.5-30363d.svg)](https://github.com/irssi/irssi)
[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](COPYING)

erssi Shellter Edition is yooz's edition of
[erssi](https://github.com/erssi-org/erssi), the modern irssi created by
Jerzy “kofany” Dąbrowski and the erssi-org team. It keeps everything erssi
brings — sidepanels, mouse support, 24-bit colour, image preview, a web
frontend and full irssi Perl script compatibility — and tunes it for people
who live on IRCnet: a polished default theme, help for every command,
operator and botnet tools, and a security review of the parts that face the
network.

![erssi Shellter Edition: window list, a channel in the shellter theme, nick list by rank and the botnet status in the statusbar](docs/images/erssi-shellter.png)

## Install

One command, no root, about two minutes on a shell box:

```sh
curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
```

The installer checks the build dependencies and prints the exact package
command for your system when something is missing, installs `meson` and
`ninja` into a private Python environment if the system has none, builds the
latest release, runs the test suite, installs to `~/.local/opt/erssi` and
links `~/.local/bin/erssi`. Run it again to update. Options: `--prefix`,
`--ref <tag|branch>`, `--no-test`.

erssi keeps its configuration in `~/.erssi/`, so it runs side by side with
irssi. Start it inside tmux so the session survives logging out.

<details>
<summary>Manual build</summary>

Debian / Ubuntu:

```sh
sudo apt install git gcc meson ninja-build pkgconf libglib2.0-dev libssl-dev \
  libperl-dev libutf8proc-dev libgcrypt20-dev libotr5-dev \
  libcurl4-openssl-dev libchafa-dev
```

FreeBSD: `pkg install git meson ninja pkgconf glib perl5 utf8proc libgcrypt libotr curl chafa`

```sh
git clone https://github.com/y-o-o-z/erssi.git && cd erssi
meson setup Build -Dprefix="$HOME/.local/opt/erssi" -Dwith-proxy=yes
ninja -C Build && meson test -C Build && ninja -C Build install
```

Only GLib (2.32 or newer) and OpenSSL are required. Perl, utf8proc,
libcurl + chafa (image preview) and libotr + libgcrypt (OTR) are detected
and used when present. For a system-wide install use
`-Dprefix=/usr/local` and `sudo ninja -C Build install`.
</details>

## Version

| | |
|---|---|
| **erssi Shellter Edition 1.3.2** | erssi 1.3.1 (erssi-org, 2026-04-06) with the changes below. Release notes: [NEWS](NEWS). |
| **irssi core** | irssi 1.4.5 plus irssi `master` up to 2025-07-26, as merged by erssi-org. |
| **Perl scripts** | `Irssi::version()` returns the release date, so scripts that require irssi 1.4.5 or newer load. `$J` is erssi's own version (`1.3.2`). |

## What this edition adds

### Look and feel

- **`shellter` theme by default** — a dark theme in the colours of
  [shellter.me](https://shellter.me). Messages, events, server replies,
  WHOIS, notices and private messages share one column and one separator,
  so text always starts in the same place and wrapped lines continue under
  it. Joining a channel prints one summary line
  (`#chan: 70 nicks · 29 ops · 4 voiced · 37 regular`) instead of a nick table.
- **Rank colours** — `$nickmode` and `nick_mode_color_*` draw `@`, `+`, `%`
  and `~`/`&` in their own colours in messages and in the nick list;
  `nick_hash_colors` accepts 24-bit colours.
- **Read marker in the column** — the trackbar branches off the separator
  (`├────`) instead of cutting through timestamps and nicks.
- **Activity that means something** — a quit or nick change marks only the
  windows where that nick is, not every window of the network.
- **Clean windows** — network windows open without `/WINDOW` chatter and
  without server tags on every line; WHOIS replies go to the network window
  (`print_whois_rpl_in_server_window`), not into the channel you are reading.
- **Every terminal** — 24-bit colour where the terminal shows it, the
  nearest 256-colour entry where it does not (GNU Screen, Linux console,
  8/16-colour terminals). `/set term_truecolor auto|on|off` overrides it.

### Help for every command

`/help` covers every command: the erssi commands that had no help
(`credential`, `fe_web`, `floodnet`, `foreach`, `image`, `nickhash`, …) and
every command of the bundled scripts. Help files in `~/.erssi/help/` are read
first — drop in a file named after a command to document your own scripts.

### IRCnet tools

Bundled in `<prefix>/share/irssi/scripts` and loaded on demand
(`/script load <name>`, or a symlink in `~/.erssi/scripts/autorun/`). Their
messages and help are in Polish.

| Script | What it does |
|---|---|
| `botnet.pl` | Botnet partylines that speak IRC (psotnic, pt-pojeby, eggdrop): one connection per botnet, a network window with the partyline window under it like a channel, a statusbar item with the state of every botnet and unread partyline lines, a retry limit for hubs that are down. `/bot`, `/bot <botnet>`, `/bot close`. |
| `tk.pl` | Temporary K-lines for IRCnet operators (ircd 2.11 `TKLINE`): nick → WHOIS → mask, never a guessed host; refuses overly broad masks; `-dry` preview; JSONL audit log. `/tkl`, `/untkl`, `/tklist`, `/klist`. |
| `skaner.pl` | Clones and IRC operators on a channel, reported after join, with an alert when a clone arrives. `/skaner`. |
| `mentions.pl` | One *Mentions* window for highlights, private messages, notices and DCC, mirrored to a log file. |
| `trackbar.pl` | The read marker of trackbar 2.9, drawn as `├────` from the theme's separator column. `/mark`, `/trackbar`. |
| `webjournal.pl` | Journals every window so the web client shows the same history and windows as the terminal. |

### Reliability

- **Anti-floodnet without collateral damage** — counts different senders,
  so one person pasting lines is no longer a flood; never filters people you
  have a query with; ends protection on its own; bounded memory; no crash on
  messages from servers.
- **Sidepanels** — turning a panel off or changing its width gives the space
  back; a terminal narrower than the panels (a phone over ssh) keeps a
  usable window; stale scroll arrows, a leak on every panel hide and a crash
  path on very tall terminals are fixed.
- **Input line** — control characters (bold, colour) are visible and the
  cursor stays on the text.
- **Message formats** — messages to `@#channel` show the right nick.

### Security

The network-facing parts were reviewed for this release.

- **Web frontend (fe-web)** — built for a shared shell box: one password
  guess per connection, compared in constant time; 16 KB and 10 seconds to
  log in; at most 16 clients; oversized frames refused. The TLS certificate
  is kept in `~/.erssi/fe-web-cert.pem` and the web client trusts exactly that
  certificate, so nothing else listening on the port can receive the
  password.
- **Image preview** — http and https only, public addresses only (also after
  redirects), a hard size limit, and no decoding of images too large to
  handle safely. The debug log of clicked URLs is off by default.
- **Configuration** — `~/.erssi/config` is written readable only by you.
- **Credential encryption** — switched off: in erssi 1.3.1 it never
  encrypted the configuration file and a wrong master password could delete
  stored credentials. Existing setups keep working; `/help credential`
  explains how to return to plain storage.

## Configuration

```
/set theme shellter                       # the default
/set nick_mode_color_voice %Z79C0FF%_     # rank colours: _owner, _op, _halfop, _voice, _normal
/set print_whois_rpl_in_server_window on  # off: WHOIS in the active window
/set anti_floodnet_notices off            # hide Anti-Floodnet notices
/set term_truecolor auto                  # on / off to override terminal detection
/statusbar info add -after act botnet     # botnet states in the statusbar
```

In tmux, enable RGB colour with `set -as terminal-features ',*:RGB'`.

## Web client

fe-web together with [NexusIRC](https://github.com/kofany/nexus) by kofany
gives a browser view of the same erssi session — channels, queries, history
and erssi's own windows — with every command executed by erssi.

```
/set fe_web_password <long random secret>
/set fe_web_bind 127.0.0.1
/set fe_web_enabled on
/script load webjournal
/save
```

[contrib/nexusirc](contrib/nexusirc/README.md) holds the NexusIRC patch series
(Polish interface, terminal-like view, certificate pinning) with build steps
and reverse-proxy notes. Keep fe-web on `127.0.0.1`; expose only the web
client, behind TLS.

## End-to-end encryption

[contrib/rpe2e](contrib/rpe2e/README.md) carries `rpe2e.pl`, the RPE2E v1.0
script from [repartee](https://github.com/outragedevs/repartee), adapted to
erssi. It is wire-compatible with repartee and WeeChat: `/e2e on` in a
channel encrypts it (XChaCha20-Poly1305, per-sender keys, CTCP key exchange).
A migration tool moves an existing repartee identity, so peers see no key
change.

## Development

```sh
meson test -C Build                               # unit tests
ln -sf ../../utils/pre-push .git/hooks/pre-push   # build, tests and history checks before every push
git remote add upstream https://github.com/erssi-org/erssi.git
git fetch upstream && git merge upstream/main     # follow erssi
```

Each change in this edition is a separate commit that explains the problem
it solves, so upstream merges stay reviewable.

## Credits and license

- **erssi** — Jerzy “kofany” Dąbrowski and the erssi-org team,
  [erssi-org/erssi](https://github.com/erssi-org/erssi).
- **irssi** — the irssi developers, [irssi.org](https://irssi.org).
- **NexusIRC** — kofany, [kofany/nexus](https://github.com/kofany/nexus).
- **RPE2E** — the repartee authors (MIT).
- **Shellter Edition** — yooz, [y-o-o-z/erssi](https://github.com/y-o-o-z/erssi).

GPL-2.0-or-later, like irssi and erssi. `contrib/rpe2e/rpe2e.pl` is MIT.
