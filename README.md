# erssi Shellter Edition

**Your shell, your shelter — on IRCnet.**

[![Release](https://img.shields.io/badge/release-2026.10.3-f59e0b.svg)](https://github.com/y-o-o-z/erssi/releases)
[![erssi](https://img.shields.io/badge/based_on-erssi_1.3.1-30363d.svg)](https://github.com/erssi-org/erssi)
[![irssi](https://img.shields.io/badge/core-irssi_1.4.5-30363d.svg)](https://github.com/irssi/irssi)
[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](COPYING)

Website: [y-o-o-z.github.io/erssi](https://y-o-o-z.github.io/erssi/)

erssi Shellter Edition is an edition of
[erssi](https://github.com/erssi-org/erssi), the modern irssi created by
Jerzy “kofany” Dąbrowski and the erssi-org team. It keeps everything erssi
brings — sidepanels, mouse support, 24-bit colour, image preview, a web
frontend and full irssi Perl script compatibility — and tunes it for people
who live on IRCnet: a polished default theme, help for every command, a
built-in Mentions window, end-to-end encryption, a signed installer that
also updates, and a security review of the parts that face the network.

![erssi Shellter Edition 2026.10.3 in #shellter, shellter theme, yooz and kofany answering questions about the release: the window list on the left, the channel with the topic, the join summary line, messages under one separator, the nick list with ranks on the right, and the statusbar](docs/images/erssi-shellter.png)

## Install

One command, no root, about two minutes on a shell box:

```sh
curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
```

- The installer checks the system first and lists the packages erssi needs
  (as root it installs them once you confirm; on a user account it names
  them for the administrator and puts a missing `meson` or `ninja` into a
  venv).
- It builds the newest release recorded in the installer, after checking
  that its tag points to the recorded commit and carries a valid signature
  by the release key, runs the test suite and installs to
  `~/.local/opt/erssi`, linked as `~/.local/bin/erssi`.
- The downloaded script itself is trusted as it is: it carries the release
  key and the recorded commits that everything after it is checked
  against. Without ssh-keygen (or with git before 2.34) only the recorded
  commit is checked, and any other `--ref` is built unverified, with a
  warning. How to check the script first, and what each mode verifies:
  [What the installer verifies](docs/shellter/install.md#what-the-installer-verifies).
- Works on Linux and FreeBSD. Options: `--prefix`, `--ref`, `--src`,
  `--no-test`, `--clean`, `--yes`.
- erssi keeps its configuration in `~/.erssi/`, so it runs side by side
  with irssi. Start it inside tmux so the session survives logging out.

Details (package lists, options, FreeBSD and Capsicum, rootless containers,
the manual build): [docs/shellter/install.md](docs/shellter/install.md).

### Update

```sh
erssi --check-update     # is a newer release available?
erssi --update           # build and install it
```

`--update` takes the newest release on GitHub that carries a valid
signature by the release key of the installed copy and is a newer version
(the rules of the 2026.10.3 installer: from 2026.10.2 or older, run the
install command once instead to get them), builds, tests and installs it
into the same prefix; `/upgrade` then loads it in a running erssi, keeping
your windows and channels. Connections over plain text stay up; those over
TLS cannot be handed to the new process, so they reconnect once ("[TLS]
Client upgrade"). Coming from 1.3.9 or older (no `--update`): run the
install command again; your `~/.erssi/config` is kept and the Shellter look
stays.

Details (signature check by hand, themes and `startup` updated like system
config files, what changes when upgrading from 1.3.x):
[docs/shellter/install.md](docs/shellter/install.md#update).

## Version

| Component | Version |
|---|---|
| **erssi Shellter Edition 2026.10.3** | erssi 1.3.1 (erssi-org, 2026-04-06) with the changes below. Versions are calendar versions YEAR.MONTH.PATCH (2026.10.0 is the first; 1.3.2 to 1.3.10 came before). Release notes: [NEWS](NEWS). |
| **irssi core** | irssi 1.4.5 plus later changes from irssi `master`, as merged by erssi-org. |
| **Perl scripts** | `Irssi::version()` returns the release date, so scripts that require irssi 1.4.5 or newer load. `$J` is erssi Shellter Edition's own calendar version (for example `2026.10.3`). |

## What this edition adds

Every command has a page in `/help`. The full lists:
[docs/shellter/features.md](docs/shellter/features.md).

**Encryption you can rely on** (new in 2026.10.3)

- **Nothing disappears** — an encrypted message that cannot be read leaves a line in its place that says why and what to run; a known key from a new address gets a warning with the exact `/e2e reverify` command.
- **Plain text is marked** — a line in an encrypted channel or query that came in unencrypted shows `[plain]`, also one under the peer's nick from another address or under your own nick; bot-style lines are encrypted too unless you turn `e2e_bot_bypass` on.
- **Tested against repartee** — `meson test` includes live tests against repartee and `rpe2e.pl` (run when they are installed), covering key changes, a reset and a flood of key requests.

**Nicks, networks and windows** (new in 2026.10.1 and 2026.10.2)

- **Keepnick built in** — `/keepnick` takes your nick back as soon as it is free (MONITOR or ISON). `/help keepnick`.
- **Side panels for split windows** — every row of split windows gets its own part of the nick list.
- **Layouts that come back as saved** — after `/layout save` or `/save` every window returns to its place.
- **One connection per network** — `/connect IRCnet` while connected switches to its window.

**Mentions, notifications and search** (new in 2026.10.0)

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

- **Web frontend (fe-web)** — the password in a header (the URL form NexusIRC sends still works); wrong passwords paced per peer; limits on login, clients and frames.
- **Image preview** — http and https only, public addresses only, a hard size limit.
- **Configuration** — `~/.erssi/config` is written readable only by you.
- **Hardened build** — PIE, stack protector, stack clash protection, `FORTIFY_SOURCE=2`, full RELRO.
- **Fuzzing** — what fe-web reads from the network is fuzzed with libFuzzer under ASan and UBSan.
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
pull requests to erssi-org are built and tested on the Shellter team's own
server: the hardened build with `meson test`, the ASan/UBSan build, live
E2E interop with `rpe2e.pl`, libFuzzer runs on the fe-web and E2E parsers,
the installer test and shellcheck. Each job is a commit status (the check
mark next to a commit) with a one-line result. Pull requests from other
contributors are tested after they have been reviewed.
`.github/workflows/shellter-ci.yml` describes a subset of these jobs for
GitHub Actions (the hardened build, the ASan/UBSan build and the installer
checks), which stay switched off on this repository.

## Credits and license

- **erssi** — Jerzy “kofany” Dąbrowski and the erssi-org team,
  [erssi-org/erssi](https://github.com/erssi-org/erssi).
- **irssi** — the irssi developers, [irssi.org](https://irssi.org).
- **RPE2E** — the repartee authors (MIT, [contrib/rpe2e/LICENSE](contrib/rpe2e/LICENSE)).
- **Shellter Edition** — the [Shellter team](https://shellter.me),
  [y-o-o-z/erssi](https://github.com/y-o-o-z/erssi).

GPL-2.0-or-later, like irssi and erssi. The e2e module is a port of the
MIT-licensed `rpe2e.pl` (licence in `contrib/rpe2e/LICENSE`).
