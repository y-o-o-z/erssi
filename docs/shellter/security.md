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
which cannot detect a change) are still read, and `/SAVE` writes them in
the new form once a value already in the new form confirms the master
password.
