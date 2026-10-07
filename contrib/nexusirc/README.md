# Web client for erssi: NexusIRC + patches

[NexusIRC](https://github.com/kofany/nexus) (a fork of The Lounge) talks to
erssi through the fe-web module. With these patches the browser shows what the
terminal shows: the same session, channels, queries, history and erssi's own
windows (Notices, Mentions, script windows). The series is made against
`kofany/nexus` **ba3fb21** and is applied as a whole, in order (`git am`).

| Patches | What they add |
|---|---|
| 0001 | A *Recent mentions* window for erssi (fe-web `is_highlight`). |
| 0002, 0003, 0013 | The **shellter** theme (shellter.me colors, light and dark following the system; fonts since 0023: Source Sans 3 and Source Code Pro) and a sign-in page with a map of IRCnet. |
| 0004, 0005, 0007, 0008, 0010–0012 | The erssi window journal (`scripts/webjournal.pl`): history without gaps across web restarts, an “erssi” group with windows that are not channels, script messages live. |
| 0006 | Closing the fe-web connection with a handshake (no TLS errors in erssi). |
| 0009 | Commands typed in a channel window run in that channel. |
| 0014 | Sender rank (`@`, `+`) on messages, plain wording of events. |
| 0015 | Networks that finished connecting before Nexus attached still reach the browser. |
| 0016 | The message view follows the terminal: sender column as wide as the longest nick in the window, time without brackets, erssi lines with their source in the sender column; shellter polish (readable hover, dark inputs, sentence-case buttons, mIRC colors readable on both backgrounds). |
| 0017 | Page head: document language, pinch zoom on phones, font preload. |
| 0018 | The whole interface in Polish (chat, events with Polish plural forms, settings, network manager, help, menus, notifications, server feedback, dates) and one brand, erssi@tahio. |
| 0019 | Trust only erssi's own fe-web certificate (`~/.erssi/fe-web-cert.pem`, kept by erssi since 1.3.2) instead of any certificate on the port. |
| 0020 | Phones: the input line stays above the on-screen keyboard and the browser bar (visible-viewport height, 16px input so iOS does not zoom, 40px buttons, safe area). |
| 0021 | Sessions and sign-out work with erssi: a reload keeps its session instead of creating a new never-expiring one, the session list and "Sign out" / "Sign out all other sessions" work, a sign-out button in the sidebar, sessions unused for 30 days expire. |
| 0022 | The fe-web password goes in an `Authorization` header, never in the URL or a log; the settings test uses the certificate pin too, an unreadable `caFile` stops the connection, the certificate is re-read on every reconnect; sign-out waits for the server, sessions expire while Nexus runs and end on a password change; `irssiConnection.journalDir` per user. Needs erssi Shellter Edition 1.3.3. |
| 0023 | Source Sans 3 (text, interface, 15px) and Source Code Pro (time, nick column, nick list, code) instead of Geist: Adobe's Source family, variable fonts, latin + latin-ext, hosted with the theme (OFL 1.1); a monospaced nick list tells I from l and 0 from O; no ligatures in IRC text. |
| 0024 | Connect to erssi over its Unix socket (`irssiConnection.socketPath`, erssi `fe_web_socket`): no TCP port other users of the box can reach; TLS, the certificate pin and the password header stay. |
| 0025 | The encrypted message storage key is derived the same way after saving the erssi settings as at login (it used a different key). |
| 0026 | Help: the changelog is the list of local patches in this checkout and no update is announced (the upstream check asked The Lounge's releases, another project); the missing handler for the changelog request is added. |

Patches 0002, 0003 and 0018 brand the web client as **erssi@tahio** and put
the interface in Polish. For your own brand or language, change the texts in
`client/components/` (and `client/js/helpers/contextMenu.ts`,
`client/nexusirc.webmanifest`, `client/index.html.tpl`) and the icons in
`client/img/`; the series has no i18n layer, texts are inline.

## Install

**1. erssi: fe-web and the journal** (y-o-o-z/erssi ≥ 1.3.3)

```
/set fe_web_password <long random secret, e.g. from: openssl rand -base64 32>
/set fe_web_bind 127.0.0.1
/set fe_web_port 9001
/set fe_web_enabled on
/script load webjournal
/save
```

`webjournal.pl` is installed with erssi (`<prefix>/share/irssi/scripts`), so
`/script load webjournal` works right away. To load it at startup:
`ln -s <prefix>/share/irssi/scripts/webjournal.pl ~/.erssi/scripts/autorun/`.

**2. NexusIRC with the patches** (Node.js ≥ 24)

```sh
git clone https://github.com/kofany/nexus.git && cd nexus
git checkout -b local ba3fb21
git am /path/to/erssi/contrib/nexusirc/0*.patch
corepack enable && yarn install
NODE_ENV=production yarn build
```

**3. Configuration** — `~/.nexusirc/config.js` (created on first start):

```js
host: "127.0.0.1",   // local only; expose it through a reverse proxy or tunnel
port: 19000,
reverseProxy: true,  // when a proxy or tunnel (e.g. Cloudflare) is in front
public: false,
theme: "shellter",
```

**4. User and the erssi connection**

```sh
node index.mjs add <user>   # sets the web sign-in password
node index.mjs start        # http://127.0.0.1:19000
```

After signing in: *Settings → irssi connection*: host `127.0.0.1`, port
`9001`, the `fe_web_password`, **Test**, then **Save** (Nexus stores it
encrypted).

**5. Access from outside** — a reverse proxy with TLS, or a tunnel (e.g.
Cloudflare Tunnel: *Public hostname* → `HTTP 127.0.0.1:19000`). Nexus and
fe-web listen on `127.0.0.1` only; the fe-web password never leaves the
machine.

## Maintenance

- Stop Nexus gracefully (SIGTERM, Ctrl-C): it closes the fe-web connection
  and flushes its message store.
- `/webjournal` in erssi shows the journal state; files live in
  `~/.erssi/journal` (0600, rotated at `webjournal_max_kb`). For another
  directory set `webjournal_dir` in erssi and `ERSSI_JOURNAL_DIR` for Nexus.
- Journal tests on the Nexus side: `test/tests/erssiJournal.ts` (mocha).
