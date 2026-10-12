# E2E over fe-web (protocol version 1)

A web client of fe-web (for example NexusIRC) can show the E2E state of
every channel and query - whether messages there are encrypted, and which
key requests wait for an answer - and accept or decline those requests.
This is the protocol for it. The E2E module (`/help e2e`) does the work;
fe-web only carries its messages.

Everything here is optional for a client: one that does not know these
messages ignores them, as it ignores any other unknown message.

## Server to client: `e2e_state`

```json
{
  "id": "1760000000-0042",
  "type": "e2e_state",
  "server": "IRCnet",
  "channel": "#test",
  "timestamp": 1760000000,
  "e2e": {
    "v": 1,
    "state": "on",
    "network": "IRCnet",
    "context": "#test",
    "mode": "normal",
    "sessions": 2,
    "pending": [
      {
        "nick": "bob",
        "handle": "bob@127.0.0.1",
        "fingerprint": "0123456789abcdef",
        "received_at": 1760000000
      }
    ]
  }
}
```

- `server` is the server tag, `channel` the name of the channel or query
  (the nick, for a query) - the same names as in `channel_join`,
  `nicklist` and `query_opened`.
- `e2e.v` is the version of the object: **1**. A client ignores an
  `e2e_state` whose `v` is not 1, and any field it does not know.
- `e2e.state`:
  - `"off"` - E2E is not on here; messages are sent as they are.
  - `"on"` - E2E is on, and there is a key from at least one person to
    read them.
  - `"waiting"` - E2E is on, but no key from anyone yet.
  - `"blocked"` - E2E is on (or may be), and nothing is sent here: an old
    keyring entry of unknown network, or the query's nick has E2E on at
    another address. erssi says why in the window.
  - `"error"` - the keyring cannot be read (nothing is sent where E2E may
    be on).
- `e2e.network` is the network the keys are kept for (the chatnet, else
  the server tag), `e2e.context` the bare name the keys are for (a
  channel, or `@ident@host` for a query) - `null` when there is none.
- `e2e.mode` is the E2E mode there (`normal`, `auto-accept`, `quiet`),
  or `null`.
- `e2e.sessions` is how many people's keys are there to read messages.
- `e2e.pending` lists the key requests that wait for accept or decline,
  oldest first: the sender's `nick`, its `handle` (`ident@host`, what an
  `e2e_action` names), the first 16 hex digits of its key's
  `fingerprint`, and when it came (`received_at`, Unix time).

All strings are valid UTF-8; quotes, backslashes and control characters
are escaped. They come from IRC (nicks, hosts): show them as text, never
as HTML.

An `e2e_state` is sent:

- in the state dump (`sync_server`), right after each channel's
  `nicklist` and after each `query_opened`;
- on every change: a key request that comes or goes, keys exchanged, E2E
  turned on or off, the keyring changed (also by another program), the
  address of a query's nick learned, a channel joined or a query opened.

Without the e2e module no `e2e_state` is ever sent. While the rpe2e.pl
script is loaded the module is inactive (the script encrypts and owns the
keyring): every channel and query is `"off"` with no requests - the
script's own state is not reported. When the e2e module is unloaded,
every channel and query it told a state to any client is sent `"off"`
once.

## Client to server: `e2e_action`

```json
{
  "type": "e2e_action",
  "id": "r1",
  "server": "IRCnet",
  "target": "#test",
  "action": "accept",
  "handle": "bob@127.0.0.1",
  "fingerprint": "0123456789abcdef"
}
```

- `action`: `"accept"` (as `/e2e accept`: bob gets your key for this
  channel or query) or `"decline"` (as `/e2e decline`: the request goes,
  and his key here is no longer trusted).
- `handle`: the `handle` of a request in this channel's (or query's)
  `pending` list - exactly as it was sent.
- `server` and `target`: the `server` and `channel` of that `e2e_state`.
- `fingerprint` (optional): the `fingerprint` of that request as the
  client showed it - exactly 16 lowercase hex digits. With it, the action
  is taken only when the request that waits has that key; without it, for
  whatever key the request of that handle waits with. **Clients should
  always send it**: it binds the decision to what the user saw (a request
  that came again with another key in the meantime is not accepted).

The answer is a `command_result` with `response_to` set to the `id`:

```json
{"id":"1760000001-0007","type":"command_result","response_to":"r1","success":true,"message":"handed to the e2e module","error_code":null,"timestamp":1760000001}
```

- `success: true` - the action was handed to the e2e module. The effect
  comes as a new `e2e_state` (the request gone from `pending`); what the
  module did, or why it did nothing, is printed in erssi in that window.
- `success: false` with `error_code`:
  - `"e2e_invalid"` - the action is not `accept` or `decline`, the handle
    is not one `ident@host` (3 to 255 bytes, exactly one `@` and not at
    either end, no space, control character, `,` or `!`), the
    fingerprint is there but not 16 lowercase hex digits, the target is
    not one channel or nick, or no server is named;
  - `"e2e_no_server"` - no IRC server with that tag;
  - `"e2e_unavailable"` - the e2e module is not loaded;
  - `"e2e_mismatch"` - the request of that handle waits with another key
    than `fingerprint` (it came again since, or the client's state is
    old): nothing is done; the current one is in the latest `e2e_state`.

Other reasons the module does not act (no such request waits, rpe2e.pl
is loaded, the keyring is lost) are printed in erssi; the answer is then
still `success: true`, and no new `e2e_state` follows.

## Security

`accept` hands out your key: whoever you accept can read what you write
there from then on. The e2e module therefore acts only on a request that
waits there - in that channel or query, with exactly that `handle` - at
the moment it gets the action - and, when the action names a
`fingerprint`, only when that request's key is that one. It never accepts
an address that has not
asked (an `/e2e accept` typed in erssi can trust someone in advance; a web
client cannot), never a request of another channel or query, and never
one of the damaged entries `/e2e list` shows. Anything else is printed in
erssi ("no such request waits - nothing sent") and nothing is sent.

An `e2e_action` is a message of a logged-in web client like any other:
only a client that passed the fe-web login can send one, and an
authenticated web client can run any erssi command anyway. Its fields are
checked one by one and passed on as they are; no command line is built
from them. While the rpe2e.pl script is loaded the module refuses every
action and never writes the keyring.

## For modules and scripts

The e2e module offers the same as signals (docs/signals.txt), which
fe-web uses:

- `"e2e state"` (server, target, json) - the module tells a state; `json`
  is the `e2e` object above.
- `"e2e state request"` (server, target) - answered at once with an
  `"e2e state"` for that channel or query.
- `"e2e action"` (server, target, action, handle, fingerprint) - accept
  or decline, as above; fingerprint may be NULL.
- `"e2e action refused"` (server, target, handle, reason) - emitted by the
  module while it handles an action it does not take; reason is
  `"mismatch"`, `"no_request"`, `"inactive"` (rpe2e.pl loaded), `"lost"`
  (the keyring is lost) or `"not_connected"`.
