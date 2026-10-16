# What erssi Shellter Edition adds

Back to the [README](../../README.md). Every command below has its own
page in `/help`.

- [Nicks, networks and windows](#nicks-networks-and-windows)
- [Mentions, notifications and search](#mentions-notifications-and-search)
- [Look and feel](#look-and-feel)
- [Help for every command](#help-for-every-command)
- [Bundled scripts](#bundled-scripts)
- [Reliability](#reliability)

The [security review](security.md) and
[end-to-end encryption](configuration.md#end-to-end-encryption) have their
own pages.

## Nicks, networks and windows

New in 2026.10.1 and 2026.10.2.

- **Keepnick built in** — `/keepnick` keeps your nick on a network and
  takes it back as soon as it is free: through MONITOR when the server
  offers it, otherwise ISON every 12 seconds, and at once when the holder
  quits or changes nick in a channel you share. With `keepnick_auto` (on)
  erssi also takes back the nick a connection asked for when it had to
  start on another one. The nick is saved with the network; the nicks of
  the keepnick.pl script are imported once. `/help keepnick`.
- **Side panels for split windows** — windows split above each other or
  side by side keep their columns when the panels hide and show, and every
  row of windows gets its own part of the nick list. `/window size`,
  `grow` and `shrink` work in both directions and say when a number is out
  of range.
- **Layouts that come back as saved** — after `/layout save` or `/save`
  and a restart every window returns to its place, with the same active
  window in each part of the screen.
- **One connection per network** — `/connect IRCnet` while IRCnet is
  connected switches to its window instead of opening a second
  connection; closing a network's last window disconnects it; the network
  option `one_window` keeps all its queries in the network window.

## Mentions, notifications and search

New in 2026.10.0.

- **Mentions window** — private messages and `/me` actions, notices from
  people, DCC CHAT and channel hilights collected in one background window
  (`/mentions`), mirrored to a log file only you can read (`mentions_log`,
  on by default in a new home). `/ignore` is respected, and
  `mentions_exclude_networks` leaves networks out. Built in: no script to
  load. `/help mentions`.
- **Desktop notifications** — a mention while the terminal is not focused
  pops up a notification through the terminal (iTerm2, kitty, WezTerm,
  Ghostty, foot; also inside tmux). `term_notify` is `auto` in a new home;
  in a config from 1.3.x turn it on with `/set term_notify auto`.
  `/set term_notify_text off` leaves the message text out. `/help term_notify`.
- **Search the logs** — `/search needle` looks through the autolog files
  of the channel or query you are in (autolog is on in a new home; if yours
  is off, `/set autolog on`); `-target`, `-network` and `-all` widen it,
  `-regexp` and `-case` change the match, `-max` sets the number of hits
  (50). It runs in a separate process, so erssi goes on meanwhile
  (`/search -stop` ends it), and stops at `search_max_size` (512 MiB) read.
  `/help search`.

## Look and feel

- **`shellter` theme by default** — a dark theme in the colours of
  [shellter.me](https://shellter.me). Messages, events, server replies,
  WHOIS, notices and private messages share one column and one separator,
  so text always starts in the same place and wrapped lines continue under
  it. Joining a channel prints one summary line
  (`#chan: nicks 70 · ops 29 · voiced 4 · regular 37`) instead of a nick table.
- **`shellter-light`** — for terminals with a light background: the same
  layout in light colours with every text colour at 4.5:1 or more on white
  ([screenshot](../images/erssi-shellter-light.png)): `/set theme shellter-light`,
  with the matching nick colours listed at the top of `themes/shellter-light.theme`.
- **Rank colours** — `$nickmode` and `nick_mode_color_*` draw `@`, `+`, `%`
  and `~`/`&` in their own colours in messages and in the nick list;
  `nick_hash_colors` accepts 24-bit colours.
- **Read marker in the column** — the bundled trackbar branches off the
  separator (`├────`) instead of cutting through timestamps and nicks.
- **Activity that means something** — a quit or nick change marks only the
  windows where that nick is, not every window of the network.
- **Clean windows** — network windows open without `/WINDOW` chatter and
  without server tags on every line; WHOIS replies go to the network window
  (`print_whois_rpl_in_server_window`), not into the channel you are reading.
- **Themeable statusbar** — the colours of the statusbar items (time,
  nick, window, activity, lag, prompt) come from the theme (`sb_*`
  abstracts), with one continuous bar background.
- **Every terminal** — 24-bit colour where the terminal shows it, the
  nearest 256-colour entry where it does not (GNU Screen, Linux console,
  rxvt-unicode, 8/16-colour terminals). `/set term_truecolor auto|on|off`
  overrides it.

## Help for every command

`/help` covers every command, including the erssi commands that had no help
(`credential`, `fe_web`, `floodnet`, `foreach`, `image`, `nickhash`, …) and
the commands of the bundled scripts.
`/help $nickmode` shows the help of an expando.
Help files in `~/.erssi/help/` are read first — drop in a file named after a
command to document your own scripts.

## Bundled scripts

Installed in `<prefix>/share/irssi/scripts` next to the scripts erssi ships,
loaded on demand (`/script load <name>`, or a symlink in
`~/.erssi/scripts/autorun/`).

| Script | What it does |
|---|---|
| `trackbar.pl` | The read marker of trackbar 2.9.2, drawn as `├────` from the theme's separator column when the theme has one. `/mark`, `/trackbar`. |
| `webjournal.pl` | Journals every window to `~/.erssi/journal` (one JSON line per message, files only you can read), for a web client that reads the journal to show history after a restart. |
| `mentions.pl` | Only a stub now: the Mentions window is built in, and erssi moves the script's settings to it at start. Loaded, the stub is unloaded again with a notice; remove it from `~/.erssi/scripts/autorun/`. |

## Reliability

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
- **Slow web clients** — output to a web client that reads slowly (a
  phone on mobile data, a large state dump) is queued in order instead of
  dropped; a client that never reads is disconnected at 32 MB.
- **FreeBSD** — tested on FreeBSD 14.5: builds with the base system's
  OpenSSL and passes the test suite; `/connect` works inside Capsicum
  capability mode (`/search` does not run there). See
  [install.md](install.md#freebsd).
