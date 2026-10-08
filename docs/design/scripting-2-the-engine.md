# AthenaSIP - Scripting, part 2: the engine and the scripts

The second of two design documents. [Part 1](scripting-1-the-node.md) says what the node
provides and where the decisions leave the C++. This one is the Lua engine behind the
`lua://` policy driver, the API it gives a script, the standard scripts that reproduce
today's behaviour exactly, and the scripts that do what today's node cannot.

Built in `0.10.0`: [Scripting](../scripting.md) is the reference for what shipped, and
`TODO/COMPLETED.md` records where it differs from this design. Written 2026-10-07 against `0.9.0`.

## The engine

### Lua 5.4, from the system

Lua 5.4 through its C API, taken from the system package the way yaml-cpp is: Debian
trixie ships `liblua5.4-dev` (5.4.7) and Homebrew `lua@5.4`. No binding library (sol2,
LuaBridge): the surface is a few dozen functions and the hand-written binding is the
smaller and clearer dependency, which is the rule this project has for libraries. Not
LuaJIT: it is 5.1, unmaintained in the ways that matter, and a routing decision takes
microseconds in the interpreter.

`src/script/` as it stands (`ScriptEngine`, `LuaScriptEngine`, 311 lines, never built from
`main`) is replaced, not extended: it was an event handler for whole messages, and the
hooks are questions with answers.

### One state per generation

The engine holds one `lua_State` built with `lua_newstate` and a custom allocator that
counts bytes against `policy.lua.memory_limit` (default 64 MB) and refuses beyond it. On
load it opens `base`, `string`, `table`, `math` and `utf8`, and from `os` only `date`,
`time` and `clock`; removes `dofile`, `loadfile` and `load`; installs its own `require` that searches
`policy.lua.path` only; then runs the entry file and `init()`.

A reload (`SIGHUP`, or `POST /api/v1/policy/reload`) builds a second state the same way.
If the entry fails to compile or `init()` raises, the reload is refused with the error and
the old state stays current. Otherwise the new state takes every hook call from then on,
and the old one is closed when the last coroutine still running on it finishes. A
transaction never changes script half way.

The state lives on the Core strand and is touched from nowhere else. Core is single
threaded, so no lock is needed and none is taken.

### A coroutine per hook call

Every hook call is `lua_newthread` on the current state, the hook function and its
arguments pushed, and `lua_resume`. The thread is anchored in the registry until it
finishes, and the C++ side keeps a `HookCall` object holding the thread reference, the
`Handler` to answer, and the deadline.

A primitive that needs the datastore (`store.realm(name)` and the rest) is a C function
that issues the asynchronous datastore call with the Core executor and the `HookCall` as
context, then `lua_yield`s. When the datastore answers on the strand, the engine pushes
the result (or raises the error into the coroutine) and `lua_resume`s. To the script this
is a function that returned. To the node it is an operation that took an executor and a
handler, like every other plugin call, and the strand was never held.

A hook that returns without yielding completes in the same `lua_resume`, and the engine
still posts the handler rather than calling it inline, as the plugin rules require.

### Budgets

- **Instructions:** `lua_sethook` with `LUA_MASKCOUNT` every 10,000 instructions,
  cumulative per hook call, raising an error at `policy.lua.instruction_limit` (default
  1,000,000: a routing decision is a few thousand). Time spent waiting on the store does
  not count; a script cannot spin.
- **Wall clock:** a `HookCall` whose store calls have not come back within
  `policy.lua.timeout_ms` (default 5000, the same order as `sip.connect_timeout_ms`) is
  abandoned: the handler gets an error, the coroutine is dropped, and a late answer from
  the store finds nothing to resume.
- **Memory:** the allocator above. An allocation refused inside a script is a Lua error in
  that script, not a crash.

### Errors

A Lua error in a hook, from `error()`, a bad primitive argument, a budget or a type
mistake, answers the handler with a failure. The node then:

- `authorize`, `route`, `register`: sends 500 Server Internal Error with
  `Reason: SIP;cause=500;text="policy error"` and logs, at error, the hook, the script
  file and line from the traceback, and the message. The log line is the whole of the
  debugging story, so it is complete.
- `on_failure`: behaves as `"next"`, and logs the same way. A failure-handler bug must not
  turn a 486 into a 500.
- `init`: refuses the load. At startup that is exit with the error; at reload it is the
  refusal above.

`athenasip --check` compiles every script on the path and runs `init()` against the
configured datastore, and reports the first error with file and line. `athenasip --check`
without scripts that compile does not pass.

### What the node hands a script

Values cross as Lua userdata with metatables whose `__index` is a C function, so a
script sees `request.from.user` and the node builds nothing it did not ask for. Tables
are returned where a value is a list or a plain record (headers, attributes, locations).
Nothing a script is handed outlives the hook call: a `request` kept in a global is a
dangling reference the metatable refuses with an error on next use.

The `request` object:

| Field | Type | |
|---|---|---|
| `method` | string | |
| `uri` | uri | the Request-URI |
| `from`, `to` | address | `.uri`, `.display`, `.tag` |
| `call_id` | string | |
| `cseq` | integer | |
| `headers` | headers | `:get(name)` a list of values, `:first(name)`, `:has(name)` |
| `body`, `content_type` | string or nil | |
| `source` | source | `.transport`, `.address`, `.port`, `.flow_id`, `.reliable`, `.peer_node`, `.authenticated_as` |
| `in_dialog` | boolean | To tag and a known dialog |
| `has_flow_token` | boolean | a token this node sealed was present |
| `has_route` | boolean | a Route remains after this node's were stripped |
| `relay` | boolean | a REGISTER being forwarded (`authorize` only) |
| `identity` | identity or nil | after `authorize`: `.kind` (`subscriber`, `trunk`, `peer`, `address`), `.realm`, `.aor`, `.trunk` |
| `vars` | table | the script's own, kept across the hooks of one transaction |

`uri` has `scheme`, `user`, `host`, `port`, `transport`, `params` (table), `tostring()`,
and `with{user=, host=, ...}` returning a copy. `address` the same plus `display` and
`tag`. `sip.uri(string)` parses one; `sip.uri{...}` builds one.

Edits, on the copy the node forwards (part 1, item 6): `request:set_header(name, value)`,
`:add_header`, `:remove_header`, `:set_request_user(user)`, `:set_from{display=, user=,
host=}`. The guard list is enforced here with an error naming the header.

### The library

Everything is under one global, `athenasip`, which the standard scripts bind to `a`.
Functions marked "yields" go to the store.

```
a.log.debug|info|warn|error(fmt, ...)
a.node.id                                 -- sip.node_id
a.node.names(host, port)                  -- is this host:port one of this node's addresses
a.node.public_address
a.node.nodes()                            -- the directory: {id, status, stale, cluster_reachable, ...}
a.config.get("behaviour.media_profile")   -- any setting, read-only
a.config.behaviour                        -- {media_anchor, media_profile, qualify_interval, rewrite_contact}

a.store.realm(name)                  -- yields; realm or nil
a.store.realms()                     -- yields; list
a.store.subscriber(aor)              -- yields; subscriber or nil; aor is a uri or a string
a.store.locations(subscriber)        -- yields; list of bindings
a.store.trunk(name)                  -- yields; trunk or nil
a.store.trunks()                     -- yields; list
a.store.call(id)                     -- yields
a.store.counter.incr(key, ttl)       -- yields; the new value
a.store.counter.get(key)             -- yields

a.auth.digest{realm = name, from_must_match = true, trust_flow = true}
                                          -- realm "*": any served realm. trust_flow: a reliable flow a REGISTER
                                          -- authenticated as the From AOR is not challenged (proxy.cpp:282)
a.auth.trusted{kind = "trunk", name = "acme"}
a.auth.accept()
a.auth.reject(code, reason)

a.route.subscriber(subscriber, opts)      -- opts: ring_timeout, reoffer
a.route.uri(uri, opts)                    -- opts: next_hop, trunk, ring_timeout, reoffer
a.route.trunk(name, opts)                 -- sugar: a.route.uri(trunk.uri:with{user = opts.user}, {trunk = name})
a.route.forward(targets, opts)            -- opts: media = {anchor=, profile=}, rewrite_contact
a.route.reply(code, reason, headers)
a.route.reject(code, reason)              -- the same as reply, read differently

a.register.accept{realm = realm, max_expires = n, min_expires = n, qualify = n}
a.register.forward()
a.register.reject(code, reason)

a.sip.uri(s) / a.sip.uri{...}
a.sip.number.digits(s)                    -- strip everything but digits and a leading +
a.sip.number.e164(s, {country = "44", trunk_prefix = "0"})
a.sip.cidr.contains(cidr_or_list, address)
a.sip.realm_of(request)                   -- lower(request.from.uri.host)
```

A realm from the store is a table: `name`, `registration_timeout`, `registration_minimum`,
`behaviour` (what it set, `nil` for inherited), `behaviour_effective` (over the server's),
`attributes`. A subscriber: `id`, `aor`, `realm`, `media_profile`, `attributes`. A
binding: `contact`, `node_id`, `flow_id`, `registered_at`, `expires_at`, `instance`,
`reg_id`, `push`. A trunk: part 1's record, without the password.

`route` returns one of `a.route.forward`, `a.route.reply` or `nil`. A `nil` means the node
does what it does with no targets: 480. A target list may
also be returned bare and is wrapped in `a.route.forward`.

`on_failure(request, response, state)` gets `response.code`, `response.reason`,
`response.headers`, and `state.target` (the one that failed: `.kind`, `.uri`, `.trunk`),
`state.tried` (how many), `state.remaining` (how many are left), `state.best` (the best
code so far). It returns `"next"`, `"stop"`, or a list of targets to try before the
remaining ones.

## The standard scripts

Installed to `${CMAKE_INSTALL_DATADIR}/athenasip/scripts`, always on the path, and the
default `main.lua` is one line of them. With `policy.url: lua://` and nothing else, a node
behaves as it does under `builtin://`. A deployment that wants something else writes its
own `main.lua` on `policy.lua.path`, requires the standard module, and overrides the
hooks it cares about, calling the standard function for everything else.

### `main.lua`

```lua
local std = require "athenasip.standard"
authorize, route, on_failure, register = std.authorize, std.route, std.on_failure, std.register
```

### `athenasip/standard.lua`

This reproduces `proxy.cpp:211-271` (`_authorize`), `:274-355` (`_authenticate`, the
parts that are policy), `:457-601` (`_determine_targets`), `:932` and
`registrar.cpp:145-188` and `:482-518`. Each function has the line it replaces beside
it, and the differential run in part 1's migration is what proves the two agree.

```lua
local a = athenasip
local std = {}

-- proxy.cpp:211-271. An out-of-dialog request, or a REGISTER being relayed.
function std.authorize(request)
  if request.method == "ACK" or request.method == "CANCEL" then return a.auth.accept() end
  if request.source.peer_node then return a.auth.accept() end            -- a cluster peer: :222
  if request.in_dialog then return a.auth.accept() end                   -- :225
  if request.has_flow_token then return a.auth.accept() end              -- :229

  if request.relay then                                                  -- :1888 _authorize_relay
    if request.source.reliable and request.source.authenticated_as then return a.auth.accept() end
    return a.auth.digest{realm = "*", from_must_match = false}           -- one challenge per realm: :1960
  end

  if not request.from then return a.auth.reject(400, "Bad Request") end  -- :232

  local realm = a.store.realm(a.sip.realm_of(request))                   -- :241
  if realm then return a.auth.digest{realm = realm.name} end             -- :249; the user must match: :343
  if request.has_route then return a.auth.reject(403, "Forbidden") end  -- routed off this node: :252

  local called = a.store.realm(request.uri.host:lower())                 -- :260
  if called then return a.auth.accept() end                              -- anybody may call in: :265
  return a.auth.reject(403, "Forbidden")                                 -- :268
end

-- proxy.cpp:457-601. An authorised initial request with no Route and no flow token. An OPTIONS for the node
-- itself never gets here: the node answers it (RFC 3261 11.2).
function std.route(request)

  local realm = a.store.realm(request.uri.host:lower())                  -- :527
  if not realm then                                                      -- :539-547: forwarded as it is
    return a.route.forward({a.route.uri(request.uri)}, {
      media = a.config.behaviour,                                        -- part 1, item 11
      rewrite_contact = a.config.behaviour.rewrite_contact,
    })
  end

  local subscriber = a.store.subscriber(request.uri)                     -- :560; the whole URI is the AOR
  if not subscriber then return a.route.reject(404, "Not Found") end     -- :572
  local bindings = a.store.locations(subscriber)                         -- :580
  if #bindings == 0 then return a.route.reject(480, "Temporarily Unavailable") end  -- :591

  return a.route.forward({a.route.subscriber(subscriber)}, {             -- order and expansion: _add_targets :958
    media = realm.behaviour_effective,                                   -- :550
    rewrite_contact = realm.behaviour_effective.rewrite_contact,         -- :551
  })
end

-- proxy.cpp:932. Every 3xx to 5xx moves to the next target; 2xx and 6xx never reach here.
function std.on_failure(request, response, state)
  return "next"
end

-- registrar.cpp:145-188 and :482-518.
function std.register(request)
  local realm = a.store.realm(request.to.uri.host:lower())               -- :145
  if not realm then                                                      -- _on_unserved :160-188
    if a.node.names(request.uri.host, request.uri.port) or a.store.realm(request.uri.host:lower()) then
      return a.register.reject(404, "Not Found")                         -- :172
    end
    if a.config.get("sip.forward_register") == "subscribers" then return a.register.forward() end  -- :181
    return a.register.reject(403, "Forbidden")
  end
  return a.register.accept{
    realm = realm,
    max_expires = realm.registration_timeout,                            -- :507
    min_expires = realm.registration_minimum,                            -- :512
    qualify = realm.behaviour_effective.qualify_interval,                -- :307
  }
end

return std
```

What is not here, and why: the push, outbound and Path handling of a REGISTER, the 555 and
439 and 423-for-push answers, the expansion of a subscriber into bindings in the order
`_add_targets` sorts them, the 488 re-offer, the 503 and 430 rules, timer C, the
challenge-and-verify of a Digest. All of it is mechanism and runs the same whatever
`route` returns. The script above is every line of policy the node has, and it fits on a
page, which is the measure of whether the seam is in the right place.

### Proof

Three checks, all in the tree, and all kept: `builtin://` stays the default driver, so the
standard scripts and the C++ must go on agreeing, and a change to either without the
other fails the suite.

1. The unit suite runs the proxy and registrar tests against `lua://` with these scripts.
   Where a test today sets `config->behaviour` or a realm's behaviour, nothing changes:
   the scripts read the same values.
2. The three sipp harnesses (12, 12 and 15 scenarios) pass with `policy.url: lua://`.
3. A differential fixture in `tests/policy/` feeds the same recorded requests to both
   drivers and asserts identical decisions, including the 500 paths: a store that fails
   must give the same code from both.

## Scripts that do more

These are the scripts a deployment writes, or copies from `docs/scripting/examples/`,
once part 1's mechanism exists. Each is complete: it requires the standard module and
overrides what it needs.

### Trunks: outbound by prefix, inbound by number

The common case, and what the standard trunk module `athenasip.trunks` provides so that
a deployment with trunks records them over the API, sets each trunk's `attributes`, and
writes no Lua at all. The record for a carrier:

```json
{
  "name": "acme",
  "uri": "sip:sip.acme.example;transport=tls",
  "auth": {"username": "4420xxxx", "password": "..."},
  "register": {"enabled": true, "expires": 300, "contact_user": "4420xxxx"},
  "inbound_addresses": ["203.0.113.0/24"],
  "attributes": {
    "prefixes": ["+44", "+1"],
    "country": "44",
    "numbers": {"+442012345678": "sip:reception@example.com", "+442012345679": "sip:alice@example.com"},
    "caller_id": "+442012345678",
    "priority": 10
  }
}
```

```lua
local a = athenasip
local std = require "athenasip.standard"
local trunks = {}

-- Which trunks carry a number, best first.
local function trunks_for(number)
  local out = {}
  for _, t in ipairs(a.store.trunks()) do
    for _, p in ipairs(t.attributes.prefixes or {}) do
      if number:sub(1, #p) == p then out[#out + 1] = t; break end
    end
  end
  table.sort(out, function(x, y) return (x.attributes.priority or 100) < (y.attributes.priority or 100) end)
  return out
end

local function trunk_from(request)
  for _, t in ipairs(a.store.trunks()) do
    if a.sip.cidr.contains(t.inbound_addresses, request.source.address) then return t end
  end
end

function trunks.authorize(request)
  local t = trunk_from(request)
  if t and not request.in_dialog then return a.auth.trusted{kind = "trunk", name = t.name} end
  return std.authorize(request)
end

function trunks.route(request)
  -- Inbound from a carrier: the dialled number names a subscriber.
  if request.identity and request.identity.kind == "trunk" then
    local t = a.store.trunk(request.identity.trunk)
    local number = a.sip.number.e164(request.uri.user, {country = t.attributes.country})
    local aor = (t.attributes.numbers or {})[number]
    if not aor then return a.route.reject(404, "Not Found") end
    local subscriber = a.store.subscriber(aor)
    if not subscriber then return a.route.reject(404, "Not Found") end
    request:remove_header("P-Asserted-Identity")       -- the carrier's assertion is not for the phone
    request:set_header("P-Asserted-Identity", "<" .. request.from.uri:tostring() .. ">")
    return a.route.forward({a.route.subscriber(subscriber)}, {media = a.store.realm(subscriber.realm).behaviour_effective})
  end

  -- A subscriber dialling a number: out by the best trunk, the rest as failover.
  if request.identity and request.identity.kind == "subscriber" and request.uri.user and request.uri.user:match("^%+?%d+$") then
    local realm = a.store.realm(request.identity.realm)
    local number = a.sip.number.e164(request.uri.user, {country = realm.attributes.country or "44"})
    local candidates = trunks_for(number)
    if #candidates == 0 then return a.route.reject(404, "No route to number") end

    local caller = a.store.subscriber(request.identity.aor)
    local cli = caller.attributes.caller_id or candidates[1].attributes.caller_id
    if cli then
      request:set_from{user = cli}
      request:set_header("P-Asserted-Identity", "<sip:" .. cli .. "@" .. realm.name .. ">")
    end
    if caller.attributes.withhold_number then request:set_header("Privacy", "id") end

    local targets = {}
    for _, t in ipairs(candidates) do targets[#targets + 1] = a.route.trunk(t.name, {user = number}) end
    return a.route.forward(targets, {media = realm.behaviour_effective})
  end

  return std.route(request)
end

-- A carrier that answers 503 or 480 is skipped for the next one; a busy or declined number is final.
function trunks.on_failure(request, response, state)
  if state.target.trunk then
    if response.code == 486 or response.code == 603 or response.code == 404 then return "stop" end
    return "next"
  end
  return std.on_failure(request, response, state)
end

trunks.register = std.register
return trunks
```

The node answers the carrier's 407 on each INVITE and keeps the trunk registered; the
script never sees either. `main.lua` for this deployment is the standard one with
`athenasip.trunks` in place of `athenasip.standard`.

### Peering with another PBX by address

An Asterisk on the LAN that sends calls for some extensions and takes calls for others,
with no registration and no credentials either way: a trunk with `inbound_addresses`
set, `register.enabled` false and no `auth`. The trunks module above handles it with no
change; `prefixes` of `["2"]` sends every number starting 2 to it, and its `numbers` map
brings its extensions to our subscribers.

### Out of hours

Calls to the main number after six go to the on-call phone's mobile, through the trunk.

```lua
local a = athenasip
local trunks = require "athenasip.trunks"
local m = {}

local function out_of_hours()
  local h = tonumber(os.date("%H"))
  return h < 9 or h >= 18
end

function m.route(request)
  if request.identity and request.identity.kind == "trunk" and out_of_hours() then
    local realm = a.store.realm("example.com")
    local oncall = realm.attributes.oncall_mobile
    if oncall then return a.route.forward({a.route.trunk("acme", {user = oncall})}, {media = realm.behaviour_effective}) end
  end
  return trunks.route(request)
end

setmetatable(m, {__index = trunks})
return m
```

### A hunt sequence with a timeout

Ring the subscriber for twenty seconds, then a colleague, then the mobile. Without
`ring_timeout` the first branch would ring for timer C's four minutes.

```lua
function m.route(request)
  local target = a.store.subscriber(request.uri)
  if target and target.attributes.hunt then
    local realm = a.store.realm(target.realm)
    local targets = {a.route.subscriber(target, {ring_timeout = 20})}
    for _, aor in ipairs(target.attributes.hunt) do
      local s = a.store.subscriber(aor)
      if s then targets[#targets + 1] = a.route.subscriber(s, {ring_timeout = 20}) end
    end
    if target.attributes.mobile then targets[#targets + 1] = a.route.trunk("acme", {user = target.attributes.mobile}) end
    return a.route.forward(targets, {media = realm.behaviour_effective})
  end
  return trunks.route(request)
end
```

Each `ring_timeout` ends its branch with a 408 to `on_failure`, which says `"next"`. The
caller hears ringing throughout: the node forwards each branch's 180.

### Call forwarding a subscriber set themselves

`PUT /api/v1/realms/example.com/subscribers/alice` with
`{"attributes": {"forward_to": "sip:bob@example.com"}}`, by the subscriber's own
credentials on `/api/v1/subscriber/{realm}/...` once that route exists, or by an
administrator. The script:

```lua
function m.route(request)
  local target = a.store.subscriber(request.uri)
  if target and target.attributes.forward_to then
    local to = a.store.subscriber(target.attributes.forward_to)
    if to then
      request.vars.forwarded_from = target.aor
      request:set_header("Diversion", "<" .. target.aor .. ">;reason=unconditional")   -- RFC 5806
      return a.route.forward({a.route.subscriber(to)}, {media = a.store.realm(to.realm).behaviour_effective})
    end
  end
  return trunks.route(request)
end
```

### Limits

Thirty calls a minute from any one source address, and ten concurrent calls over a
trunk. The counters are shared, so the limit holds across the cluster.

```lua
function m.authorize(request)
  if request.method == "INVITE" and not request.in_dialog then
    local n = a.store.counter.incr("invites:" .. request.source.address, 60)
    if n > 30 then return a.auth.reject(503, "Service Unavailable") end   -- 503 so a UA backs off (RFC 3261 21.5.4)
  end
  return trunks.authorize(request)
end
```

Concurrent calls need a counter that falls when a call ends, which is a `on_call_ended`
hook the design does not have yet; the per-minute limit is what the counters give today,
and a concurrency cap is the first candidate for a sixth hook.

### A blocklist

```lua
function m.authorize(request)
  local realm = a.store.realm(a.sip.realm_of(request))
  if realm and realm.attributes.blocked then
    for _, pattern in ipairs(realm.attributes.blocked) do
      if request.from.uri.user:match(pattern) then return a.auth.reject(603, "Decline") end
    end
  end
  return trunks.authorize(request)
end
```

### Emergency numbers first

Whatever else the script decides, a call to an emergency number goes out, by the trunk
marked for it, with the caller's registered address as its identity.

```lua
local EMERGENCY = {["999"] = true, ["112"] = true, ["911"] = true}

function m.route(request)
  if EMERGENCY[request.uri.user or ""] then
    for _, t in ipairs(a.store.trunks()) do
      if t.attributes.emergency then
        local caller = request.identity and request.identity.aor and a.store.subscriber(request.identity.aor)
        if caller and caller.attributes.caller_id then request:set_from{user = caller.attributes.caller_id} end
        return a.route.forward({a.route.trunk(t.name, {user = request.uri.user})}, {media = {anchor = true, profile = "rtp"}})
      end
    end
  end
  return trunks.route(request)
end
```

### Following a redirect

The proxy does not recurse on a 3xx (RFC 3261 16.7 leaves it optional): the response
competes as the best failure. A script that wants the Contact followed does it in
`on_failure`, which can hand back new targets.

```lua
function m.on_failure(request, response, state)
  if response.code >= 300 and response.code < 400 and not request.vars.redirected then
    request.vars.redirected = true
    local targets = {}
    for _, contact in ipairs(response.headers:get("Contact")) do
      targets[#targets + 1] = a.route.uri(a.sip.uri(contact))
    end
    if #targets > 0 then return targets end
  end
  return trunks.on_failure(request, response, state)
end
```

## What the examples ask for that the design did not list

Writing them turned up two small things, each worth adding before the examples ship:

- A `Diversion` or `History-Info` header on a forwarded call, which is a header edit the
  guard list already permits; it is listed so the examples are checked against RFC 5806
  and RFC 7044 and not against habit.
- A hook when a call ends, for concurrency counters. It is a notification rather than a
  decision, and the simplest version is a `on_call_ended(call)` the engine runs with no
  answer expected.

## Testing scripts

- `tests/script/` tests the engine itself: coroutine yield and resume across a store
  call, the three budgets, an error in each hook and what the node sends, reload with a
  broken script, reload under load, the guard list, the dangling-reference refusal.
- `tests/policy/` holds the differential fixture from part 1 and the standard scripts'
  own tests: each line of `standard.lua` has a request that reaches it.
- The trunk examples run under the sipp harness with sipp as the carrier: a registrar
  that challenges and grants 300 seconds, a UAS that answers the INVITE with 407 and then
  180 and 200, an inbound INVITE from the carrier's address to a mapped number, and a
  carrier that answers 503 so the second trunk is tried.
- `athenasip --check` compiles the scripts and runs `init()`. `athenasip --explain
  <request.txt>` feeds a request from a file through `authorize` and `route` against the
  live store and prints each decision, which is how an operator sees why a call went
  where it did without placing it.

## Documentation

`docs/scripting.md` becomes the reference: the hooks, the `request` object, the library
with every function and what it yields on, the standard scripts annotated, and the
examples. `docs/configuration.md` gains the `policy` section. The README's "Pluggable by
contract" gains the sentence that routing is a script, and the Linux quick start gains the
step that installs the standard scripts, which `cmake --install` does.
