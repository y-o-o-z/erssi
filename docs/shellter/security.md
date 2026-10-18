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
- **Nobody locks you out** — on the Unix socket, and on Linux also on
  127.0.0.1, nobody else on the box can lock you out; elsewhere (FreeBSD)
  every local user is the one peer 127.0.0.1, so use the socket
  (`fe_web_socket`) on a shared box.
- **Limits** — 16 KB and 10 seconds to log in, with a separate limit for
  connections still logging in; at most 16 clients; oversized frames and
  oversized or fragmented control frames refused.
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
log of clicked URLs is off by default.

## What end-to-end encryption protects, and what it does not

erssi's E2E is RPE2E v1.0, the protocol of repartee and its scripts.

- **Protected** — the text of PRIVMSGs and actions (`/me`) in a channel or
  query with E2E on, against the server and the other users: nobody
  without the keys can read it or change it unnoticed. Once you have
  verified a person's key (`/e2e verify`, compared over another channel),
  the keys you get from them are really theirs.
- **Not protected** — NOTICEs, CTCPs other than ACTION, and bot commands
  while `e2e_bot_bypass` is on (sent in clear text); nicks, addresses,
  channel names, who talks to whom, when, and how long the messages are;
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
  `~/.erssi/rpe2e/replay.cache` (digests and times only, no text, names
  or keys). Each sender has room for its last 512 parts in each channel
  or query, 16384 in all, so a flood can make only the flooder's own
  older lines repeatable.
- **Files** — keyring.json, the replay cache and `/e2e import` files are
  read only when they are regular files (a symlink is not followed, a
  FIFO or device is refused at once); the keyring and the cache must
  also be yours.
- **Wire compatibility** — forward secrecy, encrypted NOTICEs or signed
  lines would need a new protocol version shared with repartee and its
  scripts; erssi keeps RPE2E v1.0 so that they all understand it.

`/help e2e` has the same in full.

## Configuration

`~/.erssi/config` is written readable only by you. So are the debug logs
of the side panels and of terminal resizes (`/set sidepanel_debug on`,
`/set resize_debug on`): they are kept in `~/.erssi`, never in `/tmp`, and
never written through a link.

## Hardened build

By default: PIE, stack protector, stack clash protection,
`FORTIFY_SOURCE=2`, full RELRO.

## Tested the hard way

Everything fe-web reads from the network (the login request, WebSocket
frames, encrypted frames, client messages, IRC lines turned into web
events) is fuzzed with libFuzzer under AddressSanitizer and
UndefinedBehaviorSanitizer; every finding is replayed by `meson test`. The
test suite also runs under both sanitizers.

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
