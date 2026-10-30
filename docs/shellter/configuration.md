# Configuring erssi Shellter Edition

Back to the [README](../../README.md).

- [First start](#first-start)
- [Settings](#settings)
- [tmux](#tmux)
- [Update notice](#update-notice)
- [Web client](#web-client)
- [End-to-end encryption](#end-to-end-encryption)

## First start

The first start in a new home, on a terminal, asks a few questions before
the screen opens:

1. a network (or none; the chosen one connects at every start),
2. your nick,
3. a SASL account, when that network has TLS with a verified certificate,
4. the theme,
5. whether erssi checks once a day for a new release (`update_check`,
   see [Update notice](#update-notice); asked only when the installer
   installed erssi, and Enter keeps it off).

Enter keeps the default shown in brackets. The answers go to
`~/.erssi/config` (mode 0600; a SASL password is stored there in plain
text, as with `/network add -sasl_password`).

- `n`, Ctrl-D or five minutes without an answer skip the questions; Ctrl-C
  quits without saving.
- `erssi --setup` asks again, also with an existing config, and only adds
  to it (without a terminal it says so and exits).
- Nothing is asked without a terminal, with `TERM=dumb`, or with
  `--config`, `--connect`, `--noconnect` or `--nick`.
- While the nick is still `erssi_user`, every start reminds you after the
  banner.
- Ctrl-Z at the password prompt suspends erssi; after `fg` the prompt is
  shown again.

## Settings

```
/set theme shellter                       # the default
/set nick_mode_color_voice %Z79C0FF%_     # rank colours: _owner, _op, _halfop, _voice, _normal
/set print_whois_rpl_in_server_window on  # off: WHOIS in the active window
/set anti_floodnet_notices off            # hide Anti-Floodnet notices
/set term_truecolor auto                  # on / off to override terminal detection
/set term_hyperlinks auto                 # clickable addresses (OSC 8); over ssh auto is off: /set term_hyperlinks on yourself if your terminal shows links
/set term_notify auto                     # desktop notifications (off in a config from 1.3.x)
/set mentions_log on                      # log of the Mentions window
```

The Shellter look (theme, nick and rank colours, WHOIS in the network
window, the join count line, 24-bit colours) comes from the default config
(`irssi.conf`) of a new `~/.erssi`. `/set -default <name>` (and `/reset`)
gives erssi's built-in value.

## tmux

In tmux, enable RGB colour with `set -as terminal-features ',*:RGB'`, for
notifications `set -g focus-events on` and `set -g allow-passthrough on`,
and for clickable addresses (tmux 3.4 or later) the `hyperlinks` feature
for each outer terminal that shows links, by its TERM. Not for `*`: tmux
would send the links to every client, also one attached from a console or
a terminal that prints them, and erssi inside tmux cannot tell.
In `~/.tmux.conf`:

```
set -as terminal-features ',*:RGB'                  # RGB colour
set -g focus-events on                              # for notifications
set -g allow-passthrough on                         # for notifications
set -as terminal-features ',xterm-kitty:hyperlinks' # clickable addresses in kitty
```

## Update notice

erssi can tell you when a new signed release is out, so an erssi that
lives in tmux for months does not stay on an old release. It is off by
default, because it contacts the erssi repository (GitHub, unless you
installed from another `--repo`):

```
/set update_check on                     # check once a day
/update                                  # what the last check found
/update check                            # check now
/update install                          # install the newest release, in the background
/update install -upgrade                 # ... and /upgrade once it is installed
/statusbar additem -after lag update info   # show [update 2026.11.1] while there is one
```

- **What runs** — the installer that was installed with erssi
  (`shellter-install.sh --update --check --porcelain`, the same check as
  `erssi --check-update`), in the background: erssi goes on while it runs,
  one check runs at a time, one that takes longer than two minutes is
  stopped. The installer finds the newest release tag with a valid
  signature by the release key it carries, exactly as for an update
  ([What the installer verifies](install.md#what-the-installer-verifies)).
  It downloads the list of release tags and one tag object with the
  commit it names, never the files of a release: about 6 KB received
  and 2 KB sent (measured against GitHub, plus TLS); a newer tag skipped
  as not signed by the release key costs one more such fetch (five at
  most).
- **When** — a minute after the start, then every 24 hours. The time of
  the last check is kept in `~/.erssi/update-check.state` (mode 0600), so
  a restart or `/upgrade` does not check again before the day is over;
  several erssi with the same home share it.
- **What it says** — one line, once for each new release, in the
  window of client messages, the Notices window:
  `erssi 2026.11.1 is available (signed). Install it: /update install
  (or erssi --update in a shell), then /upgrade`, and under it up to
  three lines of what the release brings, from its signed tag (releases
  from 2026.11.0 on; control characters are removed before anything is
  shown). The summary is kept in the state file, and `/update` shows it
  again. The shipped config marks no activity for
  client messages, so the line is easy to miss from another window: the
  `update` statusbar item shows the new version while there is one and
  takes no space otherwise. It is not in the shipped statusbars; the
  command above adds it. A failed
  check (no network, GitHub down) is not shown; after three in a row, one
  line says so, and `/update` shows why the last one failed. A release tag that moved to another commit is shown at
  once: report it.
- **What it does not do** — install anything by itself. Only
  `/update install`, given by you, installs (below).
- **`/update install`** — checks first, and when a newer signed release
  is there runs the same installer with `--update` (what `erssi --update`
  runs) in the background, with no terminal, so it asks nothing; erssi
  goes on meanwhile. The Notices window shows its steps, warnings and
  error; everything it prints goes to `~/.erssi/update-install.log`
  (mode 0600), named in the messages. A ready-made package installs in
  seconds, a source build can take 20 minutes or more; after an hour it
  is stopped. At the end erssi says which release is installed and that
  `/upgrade` loads it (TLS connections reconnect once); when erssi was
  started from another path than the install's `bin/erssi`, it names the
  binary to give `/upgrade`. `-upgrade` runs `/upgrade` itself once the
  install succeeded. Nothing is installed when erssi is the newest
  release; one install runs at a time (the installer's lock on the
  prefix, and the log is locked against another erssi with the same
  home); an install whose conf names another updater (`UPDATE_COMMAND`)
  is refused - update it that way. As root, missing system packages stop
  it (from a shell, `erssi --update` asks). `/quit` or `/upgrade` while
  it runs stops it; the release in use stays as it was.
- **Where it does nothing** — an erssi that the installer did not install
  (a distribution package, a build of your own: no `shellter-install.sh`
  installed with it, or no `shellter-install.conf` in its prefix), one
  that another user installed (a system install made by root: root
  updates it). Turning the setting on says why. For an install whose conf
  names another tool (`UPDATE_COMMAND`) the installer says so, and erssi
  says it once; it still asks the installer every day, which answers
  from the conf without contacting anything, so the check finds releases
  again once the conf names no other tool. In Capsicum capability mode
  erssi cannot start programs, so there is no check and no install: one
  that runs when the mode is entered is stopped first, and erssi says why
  there is none from then on; `erssi --check-update` and `erssi --update`
  from a shell still work.

## Web client

fe-web lets a web client show the same erssi session in a browser, with
every command executed by erssi;
[NexusIRC](https://github.com/kofany/nexus) by kofany is one.

```
/set fe_web_password <long random secret>
/set fe_web_bind 127.0.0.1
/set fe_web_enabled on
/save
```

On a shared box, erssi can listen on a Unix socket instead of the loopback
port, which every user of the box can connect to (the web client has to
support connecting to a socket):

```
/set fe_web_socket ~/.erssi/fe-web.sock
```

The socket is readable and writable only by you (0600, in a directory only
you can write to) and connections from other users' processes are refused;
TLS and the password work as on TCP. `/help fe_web` has the details; the
E2E part of the protocol is in [docs/fe-web-e2e.md](../fe-web-e2e.md). The
limits fe-web sets are in [security.md](security.md#web-frontend-fe-web).

Keep fe-web on `127.0.0.1`; expose only the web client, behind TLS.

## End-to-end encryption

erssi speaks RPE2E v1.0, the end-to-end encryption of
[repartee](https://github.com/outragedevs/repartee), natively: the `e2e`
module is built in and loaded at start (`-Dwith-e2e=no` leaves it out).

- `/e2e on` in a channel or query encrypts it (XChaCha20-Poly1305,
  per-sender keys, CTCP key exchange), wire-compatible with repartee and
  its WeeChat (`rpe2e.py`) and irssi (`rpe2e.pl`) scripts.
- A key request waits for `/e2e accept` (`/e2e pending` lists them), and
  your key goes only to people you accepted.
- An encrypted message that cannot be read leaves a line in its place
  (`[E2E: encrypted message not read - ...]`, `[E2E rejected: ...]`)
  that says why and what to do.
- When someone comes back from another host with the same key, or their
  key changes, their key exchange is blocked until you decide: you are
  told so in the window, and `/e2e reverify <ident@host>` accepts it.
- Keys are kept per network, so `#test` on two networks has two keys.
- The `e2e` statusbar item shows the state of the active channel or query.
- `/help e2e` has the commands.

### Moving from rpe2e.pl

Unload the script (`/script unload rpe2e`, and remove it from
`scripts/autorun`); while it is loaded the module stays inactive. The
keyring is the same file (`~/.erssi/rpe2e/keyring.json`), so there are no
new keys; people asking for your key again need one `/e2e accept`.

If you use more than one network, the old keys stay blocked (nothing is
sent or decrypted with them) until `/e2e adopt` in that channel or query
binds them to its network, or `/e2e off` turns E2E off there.

[contrib/rpe2e](../../contrib/rpe2e/README.md) holds
`rpe2e-from-repartee`, which moves an existing repartee identity, so peers
see no key change.
