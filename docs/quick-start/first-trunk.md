# Quick Start - Your First Trunk

A trunk connects the node to a carrier, so subscribers can call ordinary phone numbers and
be called on them. This guide connects one carrier to a running node, with the standard
`athenasip.trunks` script and no Lua written: calls to UK numbers go out by the carrier, and
calls to your number come in to `alice`.

You need:

- A running node with a realm and a subscriber who can call, as in
  [One node by hand](one-node.md) or [A server on Linux](linux-server.md). Here the realm
  is `example.com` and the subscriber `alice`.
- An account with a SIP carrier. From the carrier you need its SIP server
  (`sip.acme.example`), the username and password it gave you, your number in E.164
  (`+442071234567`), and the addresses its calls come from, which carriers publish for
  firewalls.
- The node reachable by the carrier: its SIP port open, and `sip.public_address` set when
  it is behind NAT.

## 1. Turn on the trunk script

Make a directory for your scripts with one file in it, `main.lua`:

```
sudo mkdir -p /etc/athenasip/scripts
echo 'authorize, route, on_failure, register = require("athenasip.trunks").hooks()' \
  | sudo tee /etc/athenasip/scripts/main.lua
```

and point the policy at it in the configuration:

```yaml
policy:
  url: "lua://"
  lua:
    path: [/etc/athenasip/scripts]
```

Check it loads, then restart: a change of `policy.url` takes a restart.

```
sudo -u athenasip athenasip --config /etc/athenasip/config.yaml --check
sudo systemctl restart athenasip
```

Everything that worked before still does: the trunk script hands every call that is not
a trunk's to the standard script, which decides as the built-in policy did.

## 2. A user who may manage trunks

Trunks need the `manage-trunks` role, and reloading scripts `manage-cluster`. Give them to
yourself, listing the roles you already hold, then sign in again:

```
curl -X PUT "$ATHENA_API/users/you" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"roles":["manage-admin-users","manage-realms","manage-realm-subscribers",
                "view-cluster-status","manage-cluster","manage-trunks"]}'
```

## 3. Create the trunk

```
curl -X POST "$ATHENA_API/trunks" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "acme",
    "uri": "sip:sip.acme.example",
    "username": "the username the carrier gave you",
    "password": "the password the carrier gave you",
    "register": {"enabled": true, "expires": 300},
    "inbound_addresses": ["203.0.113.0/24"],
    "attributes": {
      "prefixes": ["+44"],
      "country": "44",
      "caller_id": "+442071234567",
      "numbers": {"+442071234567": "sip:alice@example.com"}
    }
  }'
```

| | |
|---|---|
| `uri` | The carrier's SIP server. `;transport=tls` on the end for TLS, which the node verifies against the system's CAs, or `tls_ca` |
| `register` | Most carriers want the node to register so they know where to send calls. Leave it out for a carrier that sends to a fixed address. |
| `inbound_addresses` | Where the carrier's calls come from. A call from anywhere else is not the carrier's. |
| `prefixes` | The numbers this trunk carries, here every UK number |
| `country` | Lets a subscriber dial `020 7123 4567` as well as `+44 20 7123 4567` |
| `caller_id` | The number the people you call see |
| `numbers` | Your numbers, each to the subscriber it rings |

[Scripting](../scripting.md#trunks-without-writing-lua) has every attribute: priorities
between trunks, a number format for carriers that do not take a `+`, and a default for
numbers no list names.

## 4. Check it answers and registered

```
sudo -u athenasip athenasip --config /etc/athenasip/config.yaml --check-trunks
```

sends the carrier an OPTIONS and says what it answered: `ok  trunk acme  udp 203.0.113.10:5060
answered 200 OK in 31 ms`. A challenge (401 or 407) is an answer too. `no answer in 4 s` is
an address or a firewall, and nothing after this will work until it answers.


```
curl "$ATHENA_API/trunks/acme" -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN"
```

`registration.state` is `registered` within a few seconds, and the log says
`Registered to trunk acme for 300 seconds`. `failed` has the reason in
`registration.detail`, and the log says `Cannot register to trunk acme - <reason>`.

## 5. Call

- **Out**: dial `02071234567` or `+442071234567` from Alice. The log says
  `Trunk acme challenged (407) - answering as <username>` if the carrier challenges, which
  the node answers for her.
- **In**: call `+442071234567` from a mobile. Alice's phones ring.

## When it does not work

| What happens | Look for |
|---|---|
| `registration.state` stays `failed` | The `detail`. `no answer` or `no flow to ...` is the carrier's address or a firewall; `the carrier refused the trunk's credentials` is the username or password. |
| A call out is refused 404 | `No trunk carries <number> - 404`: no trunk's `prefixes` match the number as dialled, with `country` applied |
| A call out is refused 403 | `Trunk acme refused this node's credentials - 403` |
| A call in is refused 404 | `Trunk acme brought in <number>, which goes nowhere - 404`: add the number to `numbers`, in the form the carrier sends it |
| A call in is challenged instead | The call came from an address not in `inbound_addresses`. `sip.log_messages: true` shows where it came from. |
| The call connects with no audio | [Troubleshooting](../troubleshooting.md#one-way-or-no-audio), as for any call |

## Next

- A second carrier for failover: another trunk with the same `prefixes` and
  `"priority": 200`; lower goes first, and the default is 100. A busy or wrong number from
  the first is the answer; anything else tries the second.
- Hunt groups, out-of-hours routing and blocklists are a few lines of Lua on top of this:
  [`docs/scripting/examples/`](../scripting/examples/).
- After changing a script, `systemctl reload athenasip` reads it again without a restart
  ([Scripting](../scripting.md#changing-the-scripts)).
