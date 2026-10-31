# erssi Shellter Edition

**Your shell, your shelter — on IRCnet.**

[![Release](https://img.shields.io/badge/release-2026.10.9-f59e0b.svg)](https://github.com/y-o-o-z/erssi/releases)
[![erssi](https://img.shields.io/badge/based_on-erssi_1.3.1-30363d.svg)](https://github.com/erssi-org/erssi)
[![irssi](https://img.shields.io/badge/core-irssi_1.4.5-30363d.svg)](https://github.com/irssi/irssi)
[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](COPYING)

Website: [erssi.shellter.me](https://erssi.shellter.me/)

erssi Shellter Edition is an edition of
[erssi](https://github.com/erssi-org/erssi), the modern irssi created by
Jerzy “kofany” Dąbrowski and the erssi-org team. It keeps everything erssi
brings — sidepanels, mouse support, 24-bit colour, image preview, a web
frontend and full irssi Perl script compatibility — and tunes it for people
who live on IRCnet: a polished default theme, help for every command, a
built-in Mentions window, end-to-end encryption, a signed installer that
also updates, and a security review of the parts that face the network.

<picture>
  <source media="(prefers-color-scheme: light)" srcset="docs/images/erssi-shellter-light.png">
  <img src="docs/images/erssi-shellter.png" alt="erssi Shellter Edition 2026.10.9 in #shellter on IRCnet, shellter theme: the window list on the left; in the middle yooz (yooz@staff.shellter.me) and kofany (kofany@irc.al) join and get op, then people on the channel talk about the release under one separator; the nick list by rank on the right; the statusbar below">
</picture>

<sub>A real erssi with the real nicks of #shellter; the lines they say were written for the picture.</sub>

## Install

One command, no root:

```sh
curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
```

- **Debian 12 and 13, Ubuntu 24.04, FreeBSD 14 and 15 (amd64): a ready-made binary, in seconds.**
  The installer downloads the release's package and installs it only if
  the release's `SHA256SUMS` carries a valid signature by the release key,
  the package matches its checksum there, and its manifest names the
  release, the commit the installer records, your platform and your Perl.
  The libraries it needs must be there; if one is missing, the installer
  names the `apt` or `pkg` command and builds from source instead. The
  release goes into `~/.local/opt/erssi/releases/<version>`, `current` is
  switched to it in one step, and the previous release stays for
  `--rollback`.
- **Elsewhere, or with `--source`: built from source**, about two minutes
  on a shell box. The installer checks the system first and lists the
  packages erssi needs (as root it installs them once you confirm; on a
  user account it names them for the administrator and puts a missing
  `meson` or `ninja` into a venv). It builds the newest release recorded
  in the installer, after checking that its tag points to the recorded
  commit and carries a valid signature by the release key, and runs the
  test suite.
- Either way erssi goes to `~/.local/opt/erssi`, linked as
  `~/.local/bin/erssi`. The downloaded script itself is trusted as it is:
  it carries the release key and the recorded commits that everything
  after it is checked against. Without ssh-keygen (or with git before
  2.34) the source build checks only the recorded commit, and any other
  `--ref` is built unverified, with a warning. How to check the script and
  a package first, and what each mode verifies:
  [What the installer verifies](docs/shellter/install.md#what-the-installer-verifies).
- Works on Linux and FreeBSD; a build from source needs GLib 2.70 or
  newer. Options: `--prefix`, `--ref`, `--src`,
  `--no-test`, `--clean`, `--yes`, `--binary`/`--source`, `--keep`,
  `--dry-run`, `--rollback`, `--uninstall`, `--backup`, `--restore`.
- Moving to another box: `erssi --backup` puts your home (config,
  scripts, E2E keyring, keys) into one private file, `erssi --restore FILE`
  puts it back
  ([details](docs/shellter/install.md#moving-to-another-box)).
- erssi keeps its configuration in `~/.erssi/`, so it runs side by side
  with irssi. Start it inside tmux so the session survives logging out.

Details (binary packages, package lists, options, FreeBSD and Capsicum,
rootless containers, the manual build):
[docs/shellter/install.md](docs/shellter/install.md).

### Update

```sh
erssi --check-update     # is a newer release available?
erssi --update           # install it
```

`--update` takes the newest release on GitHub that is newer than yours and
carries a valid signature by the release key of the installed copy, and
installs it into the same prefix: from its package where there is one for
your system (checked as above, and its commit must be the one of the
signed tag), otherwise built and tested from source. `/upgrade` then loads
the new version in a running erssi and keeps your windows and channels.
Connections over plain text stay up; those over TLS cannot be handed to the
new process, so they reconnect once ("[TLS] Client upgrade").

In erssi, `/set update_check on` checks once a day in the background and
says when a new signed release is out, with a short summary; it installs
nothing by itself. `/update install` runs the same update in the
background from inside erssi, and `/upgrade` loads it
([Update notice](docs/shellter/configuration.md#update-notice)).

From 2026.10.2 or older, and from 1.3.9 or older (which have no
`--update`), run the install command once instead; your `~/.erssi/config`
is kept and the Shellter look stays.

Details (signature check by hand, rollback, themes and `startup` updated
like system config files, what changes when upgrading from 1.3.x):
[docs/shellter/install.md](docs/shellter/install.md#update).

## Version

| Component | Version |
|---|---|
| **erssi Shellter Edition 2026.10.9** | erssi 1.3.1 (erssi-org, 2026-04-06) with the changes below. Versions are calendar versions YEAR.MONTH.PATCH (2026.10.0 is the first; 1.3.2 to 1.3.10 came before). Release notes: [NEWS](NEWS). |
| **irssi core** | irssi 1.4.5 plus later changes from irssi `master`, as merged by erssi-org. |
| **Perl scripts** | `Irssi::version()` returns the release date, so scripts that require irssi 1.4.5 or newer load. `$J` is erssi Shellter Edition's own calendar version (for example `2026.10.6`). |

## What this edition adds

Every command has a page in `/help`. The full lists:
[docs/shellter/features.md](docs/shellter/features.md); what each release
changed: [NEWS](NEWS).

**New in 2026.10.7**

- **Ready-made binaries for more systems** — Debian 12 and Ubuntu 24.04 join Debian 13 and FreeBSD 14 and 15.
- **Packages you can rebuild yourself** — the packages are reproducible: `sh utils/build-binary.sh --verify PACKAGE` builds one again from its signed tag and says whether it is the same, byte for byte.
- **Told when a release is out** — `/set update_check on` checks once a day, in the background, for a newer signed release and says so in the Notices window, with a short summary; nothing is installed by itself, and `/update install` installs it from inside erssi.
- **Clickable links** — addresses in messages are OSC 8 links in terminals that support them, also when they wrap over several lines; the link is always the text you see.
- **Moving to another box** — `erssi --backup` puts your whole setup, E2E keyring included, into one private file; `erssi --restore` checks it and puts it back.
- **Installs that do not stop for nothing** — a source install runs the tests of erssi, not those of the release tools, and names what to do when a test fails; a box without `ssh-keygen` is told which one package gives the ready-made binary.

**Ready-made binaries**

- **Installed in seconds** — on Debian 12 and 13, Ubuntu 24.04, FreeBSD 14 and 15 (amd64) the installer and `erssi --update` install the release's signed package instead of building it.
- **Checked before anything changes** — the signature on `SHA256SUMS`, the package's checksum and its manifest must all match, or nothing is installed.
- **Releases side by side** — each release in its own directory, `current` switched in one step, `--rollback` to the previous one.

**Encryption you can rely on**

- **Nothing disappears** — an encrypted message that cannot be read leaves a line in its place that says why and what to run; a known key from a new address gets a warning with the exact `/e2e reverify` command.
- **Plain text is marked** — a line in an encrypted channel or query that came in unencrypted shows `[plain]`, also one under the peer's nick from another address or under your own nick; bot-style lines are encrypted too unless you turn `e2e_bot_bypass` on.
- **Tested against repartee** — `meson test` includes live tests against repartee and `rpe2e.pl` (run when they are installed), covering key changes, a reset and a flood of key requests.

**Nicks, networks and windows**

- **Keepnick built in** — `/keepnick` takes your nick back as soon as it is free (MONITOR or ISON). `/help keepnick`.
- **Side panels for split windows** — every row of split windows gets its own part of the nick list.
- **Layouts that come back as saved** — after `/layout save` or `/save` every window returns to its place.
- **One connection per network** — `/connect IRCnet` while connected switches to its window.

**Mentions, notifications and search**

- **Mentions window** — private messages, notices from people and hilights in one window, with a log. `/help mentions`.
- **Desktop notifications** — through the terminal, also inside tmux. `/help term_notify`.
- **Search the logs** — `/search needle` in the autolog files, in a separate process. `/help search`.

**Look and feel**

- **`shellter` theme by default** — messages, events and replies share one column and one separator; `shellter-light` for light terminals.
- **Rank colours** — `@`, `+`, `%` and `~`/`&` in their own colours, in messages and in the nick list.
- **Clean windows** — WHOIS replies go to the network window; a quit or nick change marks only the windows where that nick is.
- **Every terminal** — 24-bit colour where the terminal shows it, the nearest 256-colour entry where it does not.

**Help, scripts, reliability**

- **Help for every command** — including the erssi commands that had no help; your own files in `~/.erssi/help/` are read first.
- **Bundled scripts** — `trackbar.pl` (a read marker in the separator column) and `webjournal.pl` (history for a web client).
- **Reliability** — anti-floodnet that counts senders, sidepanels on narrow terminals, slow web clients queued instead of dropped.

## Configuration

The first start in a new home asks for a network, your nick, a SASL
account (when the network has TLS with a verified certificate) and the
theme; Enter keeps the default. `erssi --setup` asks again. A few
settings:

```
/set theme shellter                       # the default
/set term_notify auto                     # desktop notifications (off in a config from 1.3.x)
/set mentions_log on                      # log of the Mentions window
```

Details (the first start questions, more settings, tmux):
[docs/shellter/configuration.md](docs/shellter/configuration.md).

## Web client

fe-web lets a web client show the same erssi session in a browser, with
every command executed by erssi;
[NexusIRC](https://github.com/kofany/nexus) by kofany is one. Set
`fe_web_password`, `fe_web_bind 127.0.0.1` and `fe_web_enabled on`, or
`fe_web_socket` for a Unix socket on a shared box. Keep fe-web on
`127.0.0.1`; expose only the web client, behind TLS.

Details: [docs/shellter/configuration.md](docs/shellter/configuration.md#web-client).

## End-to-end encryption

erssi speaks RPE2E v1.0, the end-to-end encryption of
[repartee](https://github.com/outragedevs/repartee), natively, and is
wire-compatible with repartee and its WeeChat (`rpe2e.py`) and irssi
(`rpe2e.pl`) scripts. `/e2e on` in a channel or query encrypts it; a key
request waits for `/e2e accept`. A message that cannot be read is never
hidden: its line says why and names the command that fixes it. `/help e2e`
has the commands.

What it protects and what it does not (metadata, NOTICEs, no forward secrecy): [docs/shellter/security.md](docs/shellter/security.md#what-end-to-end-encryption-protects-and-what-it-does-not).

Details and moving from `rpe2e.pl`:
[docs/shellter/configuration.md](docs/shellter/configuration.md#end-to-end-encryption).

## Security

The network-facing parts were reviewed, fuzzed and tested under
sanitizers.

- **Terminal output** — every C0, DEL and C1 control character that comes from the network is drawn as `?`, on UTF-8, 8-bit and Big5 terminals.
- **Web frontend (fe-web)** — the password in a header (the URL form NexusIRC sends still works); wrong passwords paced per peer; new connections limited before TLS; limits on login, clients and frames; password copies wiped from memory.
- **Image preview** — http and https only, public addresses only, a hard size limit.
- **Configuration** — `~/.erssi/config` is written readable only by you.
- **Hardened build** — PIE, stack protector, stack clash protection, `FORTIFY_SOURCE=2`, full RELRO.
- **Fuzzing** — what fe-web and the E2E module read from the network, irssi's own parsers and the image decoder are fuzzed with libFuzzer under ASan and UBSan; every `meson test` replays their seed corpora.
- **Credential encryption** — switched off; `/help credential` explains why.

Details: [docs/shellter/security.md](docs/shellter/security.md).

## Development

How to report a bug or send a change: [CONTRIBUTING.md](CONTRIBUTING.md);
security problems: [SECURITY.md](SECURITY.md).

```sh
meson setup Build
meson test -C Build                               # unit tests and fuzz corpus replays
meson setup Build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson test -C Build-asan                          # the same tests under ASan and UBSan
ERSSI_TEST_PERF=1 meson test -C Build --suite perf # log search speed (2 x 1 GiB in TMPDIR)
```

- Tests run with `HOME` inside the build directory, so building and testing
  never writes into your own `~/.erssi`.
- With clang, UBSan's `-fsanitize=function` check is left out: irssi's
  signals call every handler through one generic function type by design.
  A sanitizer given in `CFLAGS` instead of `-Db_sanitize` needs
  `-fno-sanitize=function` at the end of `CFLAGS` (meson warns about it).
- Changes are kept in topic commits that explain the problem they solve, so
  upstream merges stay reviewable.
- `utils/pre-push` is the release hook of the Shellter team (it enforces
  the authorship and signing rules of releases); contributors do not need it.
- Most fixes are offered to erssi-org as pull requests (#3 to #11);
  [docs/UPSTREAM.md](docs/UPSTREAM.md) lists them.

### Continuous integration

Every commit on `main`, every release tag and the branches of the
pull requests to erssi-org are built and tested by erssi-ci, the CI on the
Shellter team's own server. Each job is a commit status (the check mark
next to a commit) with a one-line result:

| Status | What it runs |
|---|---|
| `erssi-ci/build` | the hardened build and the full `meson test`, the package builds of the packager included |
| `erssi-ci/sanitizers` | the same tests in an ASan/UBSan build |
| `erssi-ci/shellcheck` | shellcheck on the shell scripts |
| `erssi-ci/installer` | the installer's tests, and that every release it records points to its commit and is signed |
| `erssi-ci/e2e-live` | live E2E interop with `rpe2e.pl` |
| `erssi-ci/fuzz` | libFuzzer runs on the fe-web and E2E parsers |

A status shows "pending" until its job has run, which after a history
rewrite, when every release tag is tested again, can take hours. Pull
requests from other contributors are tested after they have been
reviewed. GitHub Actions are not used: the repository has no workflow.

## Credits and license

- **erssi** — Jerzy “kofany” Dąbrowski and the erssi-org team,
  [erssi-org/erssi](https://github.com/erssi-org/erssi).
- **irssi** — the irssi developers, [irssi.org](https://irssi.org).
- **RPE2E** — the repartee authors (MIT, [contrib/rpe2e/LICENSE](contrib/rpe2e/LICENSE)).
- **Shellter Edition** — the [Shellter team](https://shellter.me),
  [y-o-o-z/erssi](https://github.com/y-o-o-z/erssi).

GPL-2.0-or-later, like irssi and erssi. The e2e module is a port of the
MIT-licensed `rpe2e.pl` (licence in `contrib/rpe2e/LICENSE`).
