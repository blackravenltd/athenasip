# AthenaSIP - Scripting

Who may call and where a call goes are decided by a **policy**. The default,
`builtin://`, serves the node's own realms and nothing more, and needs no script. With
`lua://` the decisions are Lua 5.4 functions you can read and change.

The node does all the SIP itself. A script is asked a question at each point where the
node has a choice, returns an answer, and the node carries it out: it challenges and
checks passwords, forwards, forks, retries, anchors media and keeps RFC 3261's rules. A
script never touches a transaction, a Via or a Record-Route, and cannot send a message,
so it cannot break a call in a way the standards forbid.

Trunks are built on it: the node registers to a carrier and answers its challenges, and a
script, or the standard `athenasip.trunks` with no Lua written, decides which calls go
where. [Your first trunk](quick-start/first-trunk.md) is the walk-through.

## Turning it on

```yaml
policy:
  url: "lua://"
  lua:
    path: [/etc/athenasip/scripts]   # where your scripts are; searched in order
```

With no `main.lua` in `path`, the standard one runs, which behaves exactly as
`builtin://`. Start from it: copy `main.lua` from the installed scripts
(`/usr/local/share/athenasip/scripts`, or `scripts/` in the source) into your directory
and change the hook you need.

| Key | Default | Meaning |
|---|---|---|
| `path` | none | Directories searched, in order, for the entry script and for `require`. The standard scripts, built into the node, are searched last. |
| `entry` | `main.lua` | The script that defines the hooks |
| `instruction_limit` | `1000000` | Lua instructions one hook call may run. Time waiting on the store does not count. |
| `timeout_ms` | `5000` | How long one hook call may take, waiting included |
| `memory_limit_mb` | `64` | Memory the scripts may hold |

`athenasip --check` loads the scripts and runs their `init()`, and fails with the file
and line of the first error. Changing `policy.url` takes a restart.

## Changing the scripts

Edit the scripts, then have the node read them again, without a restart:

```
systemctl reload athenasip                                  # sends SIGHUP
curl -X POST "$ATHENA_API/policy/reload" -H "Authorization: Bearer $TOKEN"   # role manage-cluster
```

The scripts load into a new state, and calls already being decided finish on the old one.
Scripts that do not load are refused with the file and line, and the ones in force stay:
the log says `The policy did not reload, and the rules in force stay - <error>`, and the API
answers 422 with the same. A reload that worked logs `Policy reloaded: lua 1.0.0, scripts
<fingerprint>`, the fingerprint's first twelve characters.

The fingerprint is a SHA-256 of every script the node loaded. It is in the node status
(`policy.fingerprint`, [Events](events.md#node-status)), so the nodes of a cluster can be
seen to run the same rules. Each node reads its own scripts:
copy them to every node and reload each.

## The hooks

The entry script defines up to five global functions.

| Hook | Asked when | Returns |
|---|---|---|
| `authorize(request)` | A request that starts something reaches the node, or a REGISTER for another domain is to be forwarded | How the caller is proved: `athenasip.auth.accept()`, `.digest{}`, `.trusted{}` or `.reject()` |
| `route(request)` | An authorised request has no Route and no flow token to follow | `athenasip.route.forward()` with targets, `athenasip.route.reply()`, a bare list of targets, or `nil` (480) |
| `on_failure(request, response, state)` | A branch ended in 3xx to 5xx and there may be more to try | `"next"`, `"stop"`, or a list of targets to try first. Optional: without it, `"next"`. |
| `register(request)` | A REGISTER arrives, before it is challenged | `athenasip.register.accept{}`, `.forward()` or `.reject()` |
| `init()` | The scripts load | Nothing. Optional. Cannot read the store. |

Requests inside a dialog, ACK, CANCEL, and anything carrying a Route or a flow token are
routed by the node without asking: RFC 3261 fixes their path. A 2xx or a 6xx ends a fork
without `on_failure` being asked.

`authorize` returns a requirement, not a verdict. Returning
`athenasip.auth.digest{realm = realm}` makes the node challenge if there are no
credentials and check them if there are; `route` runs only once they pass, and sees the
retried request as it saw the first.

## The request

| Field | |
|---|---|
| `method` | `"INVITE"`, `"REGISTER"`, ... |
| `uri` | The Request-URI, a [URI](#uris) |
| `from`, `to` | `{uri =, display =, tag =}`, or `nil` when the header is missing |
| `call_id` | |
| `in_dialog` | A To tag inside a dialog this node knows. A tag alone is not enough. |
| `from_peer` | Came from another node of the cluster, proved by its certificate |
| `has_flow_token` | Carries a token this node sealed in its own Record-Route or Path |
| `has_route` | A Route header remains after this node's own were taken off |
| `relay` | A REGISTER for a domain not served here (`authorize` only) |
| `trunk` | After `authorize`: the trunk `athenasip.auth.trusted` named, or `nil` |
| `source` | `{transport =, address =, port =, reliable =, authenticated =, flow =}` |
| `request:header(name)` | The first value of a header, as text, or `nil` |
| `request:headers(name)` | Every value of a header, as a list |
| `request:set_header(name, value)` | In `route`: every value of a header replaced, on what the node forwards |
| `request:add_header(name, value)`, `request:remove_header(name)` | The same, adding a value or removing them all |
| `request:set_from{display =, user =, host =}` | The From the far end sees, for caller ID. Its tag stays: it names the dialog. |

The edits apply to every copy the node forwards, never to the request as it arrived, and a
script cannot touch the headers that carry the transaction, the dialog or the route: Via,
Route, Record-Route, Path, CSeq, Call-ID, Max-Forwards, Content-Length, Content-Type,
Contact, To, and From except through `set_from`. Trying is an error naming the header.

`on_failure` also gets `response` (`code`, `reason`, `:header()`, `:headers()`) and
`state` (`tried`, `remaining`, `best`).

A request belongs to its call. Kept in a global and used later, it raises an error.

### URIs

`scheme`, `user`, `host`, `port` (`nil` when absent), `transport`, `uri:param(name)`,
`uri:with{user =, host =, port =}` (a changed copy) and `tostring(uri)`. Two URIs compare equal by RFC 3261 19.1.4. `athenasip.sip.uri(text)`
parses one; anywhere a URI is taken, its text works too.

## The library

Everything is under the global `athenasip`.

| | |
|---|---|
| `athenasip.store.realm(name)` | The realm, or `nil` |
| `athenasip.store.subscriber(aor)` | The subscriber for an address of record (a URI or its text), or `nil` |
| `athenasip.store.locations(subscriber)` | The subscriber's bindings: `#bindings`, and `bindings[i]` with `contact`, `node_id`, `flow_id`, `registered_at`, `expires_at`, `instance`, `reg_id`, `push` |
| `athenasip.store.trunk(name)` | The [trunk](#trunks), or `nil` |
| `athenasip.store.trunks()` | Every trunk, in name order |
| `athenasip.node.id`, `.public_address` | |
| `athenasip.node.names(host, port)` | Whether an address is this node's own |
| `athenasip.config.get("sip.forward_register")` | Any setting, by its dotted name; its default when the file does not set it |
| `athenasip.config.behaviour` | The server's `{media_anchor, media_profile, qualify_interval, rewrite_contact}` |
| `athenasip.log.debug/info/warn/error(text)` | To the node's log, with the script's file and line. `print` logs at info. |
| `athenasip.sip.uri(text)` | Parses a URI |
| `athenasip.sip.in_range(range, address)` | Whether an address is in a CIDR range, or in any of a list of them |

A realm has `name`, `id`, `registration_timeout`, `registration_minimum`, `behaviour`
(what it set itself), `behaviour_effective` (over the server's) and `attributes`. A
subscriber has `id`, `aor`, `uri`, `realm`, `media_profile` and `attributes`.

`attributes` is a free-form object set over the API on a realm, a subscriber or a trunk
(`"attributes": {"forward_to": "sip:bob@example.com"}`), which a script reads as plain Lua
tables. The node reads none of it, so it is where a deployment keeps what its scripts need:
a forwarding number, a caller ID, a blocklist.

### Trunks

A trunk is a carrier or a PBX, kept over the API (`/api/v1/trunks`, role
`manage-trunks`, described in the [API](api/openapi.yaml)):

| Field | |
|---|---|
| `name` | Letters, digits, `.`, `_` and `-`, up to 64 |
| `uri` | Where calls and the REGISTER go, located by RFC 3263: `sip:sip.acme.example;transport=tls` |
| `proxy` | An outbound proxy, where they are sent instead of where `uri` says |
| `username`, `password` | For the carrier's challenges. The password is written, never returned. |
| `register` | `{enabled, expires, contact_user}`: whether the node registers to the carrier, for how long (60 to 86400 seconds), and the user part of the Contact it registers, where the carrier sends calls; the username when not set |
| `inbound_addresses` | The addresses or CIDR ranges the carrier's calls come from |
| `tls_ca` | A CA file for TLS to the carrier; empty is the system's store |
| `attributes` | Anything a script reads; the node reads none of it |

One node of a cluster registers each trunk that asks to be, holding a lease on it in the
datastore; when that node stops, another takes over within two minutes. What it last
reported is the trunk's `registration` in the API (`registering`, `registered` or
`failed`, with the reason), and is published, retained, as `trunks/<name>/status`. A
changed trunk is registered again; a removed one, or one no longer registering, is
unregistered.

A script sees `name`, `uri`, `username`, `registers`, `contact_user` (the user part of the
Contact registered, which is the username when the trunk does not set one),
`inbound_addresses` and `attributes`, the free-form object set over the API, as plain Lua
tables. `trunk:admits(address)` says whether an address is in its inbound ranges. Its
password is the node's, for answering the carrier's challenges, and no script reads it.

When a carrier answers a call routed to its trunk with 401 or 407, the node answers the
challenge itself with the trunk's credentials, in a new request with the next CSeq (RFC
3261 22.2), and the caller never sees it. For the rest of that call the node keeps each end
in its own CSeq space: requests from the caller go to the carrier raised by one for each
challenge answered, and responses come back lowered. A challenge the node cannot answer,
or one that refuses its answer, reaches the caller as 403.

The store lookups wait for the datastore without holding up the node: each hook call runs
in its own coroutine, which sleeps until the answer comes. A store that fails raises an
error, which `pcall` catches like any other.

### Answers

| | |
|---|---|
| `athenasip.auth.accept()` | |
| `athenasip.auth.trusted{trunk = name}` | A request from a trunk, proved by where it came from; `route` sees `request.trunk` |
| `athenasip.auth.digest{realm = realm, from_must_match = true}` | `realm = "*"` is any realm here, with a challenge for each. `from_must_match = false` accepts any subscriber's credentials, and any connection a subscriber registered over. |
| `athenasip.auth.reject(code, reason)` | |
| `athenasip.route.subscriber(subscriber, {bindings = b})` | Every device of a subscriber, in the node's order, by push or through another node as needed |
| `{ring_timeout = seconds}` | On any target: how long an INVITE may ring there before the node moves on, with a CANCEL if it rang. Unset is timer C, three minutes. |
| `athenasip.route.uri(uri, {next_hop = uri, trunk = name})` | One URI, located by RFC 3263; with `trunk`, the node answers that trunk's challenges |
| `athenasip.route.trunk(trunk, {user = number})` | Out by a trunk: its URI with the number as the user part |
| `athenasip.route.forward(targets, {media = m, rewrite_contact = r})` | `media` as `config.behaviour` and `behaviour_effective` give it |
| `athenasip.route.reply(code, reason)` | Also `athenasip.route.reject` |
| `athenasip.register.accept{realm =, max_expires =, min_expires =, qualify =}` | |
| `athenasip.register.forward()` | RFC 3261 10.3 step 1 |
| `athenasip.register.reject(code, reason)` | |

A reason left out is the RFC's phrase for the code.

## The standard scripts

`main.lua` binds the hooks to `athenasip.standard`, which is `builtin://` written in Lua,
line for line. To change one decision, keep the rest:

```lua
local std = require "athenasip.standard"
authorize, on_failure, register = std.authorize, std.on_failure, std.register

function route(request)
  if request.uri.user == "reception" then
    return athenasip.route.forward({athenasip.route.uri("sip:desk@192.0.2.40")})
  end
  return std.route(request)
end
```

The node's tests run against both `builtin://` and the standard scripts, and fail if they
answer any request differently.

## Trunks without writing Lua

`athenasip.trunks` runs calls in and out through trunks from nothing but the trunks kept
over the API. Its `main.lua` is one line:

```lua
authorize, route, on_failure, register = require("athenasip.trunks").hooks()
```

and a trunk for a UK carrier, created with `POST /api/v1/trunks`, looks like this:

```json
{
  "name": "acme",
  "uri": "sip:sip.acme.example;transport=tls",
  "username": "4420xxxx",
  "password": "...",
  "register": {"enabled": true, "expires": 300, "contact_user": "4420xxxx"},
  "inbound_addresses": ["203.0.113.0/24"],
  "attributes": {
    "prefixes": ["+44"],
    "country": "44",
    "caller_id": "+442071234567",
    "numbers": {"+442071234567": "sip:reception@example.com"}
  }
}
```

| Attribute | Meaning |
|---|---|
| `prefixes` | The numbers it carries, in E.164. The longest prefix that matches wins. |
| `priority` | Among trunks with the same match, lower first (default 100) |
| `country` | The country code, for reading a national number: `020 7123 4567` is `+442071234567` in `44` |
| `dial_format` | `e164` (default) or `digits`, without the `+`, as the carrier wants the number |
| `caller_id` | The number presented calling out, set as the From and asserted in `P-Asserted-Identity` |
| `from_domain` | The host of that From, for a carrier that checks the From is its own |
| `numbers` | The numbers it brings in, each to an address of record |
| `default` | Where a number in no list goes; without it such a call is refused |

What it does:

- **In**: a request from a trunk's `inbound_addresses` is that trunk's. The number dialled,
  from the Request-URI or, when that is the Contact the node registered, from the To, names a
  subscriber through `numbers`. Anything else is refused 404.
- **Out**: a caller in one of the node's realms dialling a number that is no subscriber goes
  out by every trunk that carries it, best first. 404, 410, 484, 486, 600, 603 or 604 from a
  trunk is the answer; any other failure tries the next trunk.
- Everything else is `athenasip.standard`.

`docs/scripting/examples/` has scripts built on it: a hunt group, out-of-hours routing to an
on-call mobile, a blocklist, emergency numbers, and following a redirect. The tests load
every one.

## What a script cannot do

- No `io`, `debug` or `package`; of `os`, only `date`, `time` and `clock`; no `load`,
  `loadfile` or `dofile`. `require` reads only from `path` and the standard scripts.
- A hook call that runs past its instruction limit, its time limit or the memory limit is
  stopped.

When a hook raises or is stopped, the caller gets 500 and the log has the hook, the
script, the line and a traceback. A failing `on_failure` is taken as `"next"`, so a mistake
there cannot turn a busy signal into a server error.
