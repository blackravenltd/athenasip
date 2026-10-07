# Quick Start - Connecting Phones

What to put into a desk phone or a softphone ([Linphone](https://www.linphone.org/),
[Zoiper](https://www.zoiper.com/), [AthenaPhone](https://github.com/blackravenltd/athenaphone)
or any other RFC 3261 client), and what the node needs for phones that are not on its own
network. It assumes a running node with a realm and a subscriber, from
[One node by hand](one-node.md) or [Try it in Docker](docker.md).

## The account

| Client setting | Value |
|---|---|
| User name | The subscriber's user, `alice` |
| Domain, or SIP server | The realm, `example.com`. The node finds the realm from this, so it must be the realm's exact name. |
| Authentication user name | The same as the user name, or empty |
| Password | The subscriber's password |
| Outbound proxy | The node's address, when the domain does not resolve to the node. Many clients call this the proxy or the registrar. |
| Transport and port | One of the listeners below |

The domain and the node's address can differ. A realm named `example.com` is served by a
node at `sip.example.com` when the client's outbound proxy is set to
`sip.example.com`, or when DNS for `example.com` has SRV records pointing there
(`_sip._udp`, `_sip._tcp`, `_sips._tcp`).

## The transports

| Transport | Listener | Default port | Notes |
|---|---|---|---|
| UDP | `udp` | 5060 | What most desk phones use first |
| TCP | `tcp` | 5060 | |
| TLS | `tls` | 5061 | Encrypted signalling. The client must trust the node's certificate. |
| WebSocket | `websocket` | 9500 | Browsers and WebRTC clients; see [Calling from a browser](browser.md) |

A subscriber can register on any of them, and two subscribers on different transports can
call each other. `sip.allow_unencrypted: false` limits the node to TLS and secure
WebSocket.

For TLS the certificate must name the address the client connects to and come from an
authority the client trusts ([Certificates](../certificates.md)). To test with the
snakeoil certificate in `tls/`, import `tls/ca/snakeca.crt` into the client as a trusted
authority; never use it on a node others can reach.

## Phones on other networks

Phones away from the node's network reach it through NAT, theirs and often the node's.
Three settings cover nearly every case:

```yaml
sip:
  public_address: 203.0.113.5     # the address phones reach the node on
  localnet: ["192.168.0.0/16"]    # the node's own network, which is given the local address

media:
  builtin:
    public_address: 203.0.113.5   # the address phones send media to

behaviour:
  qualify_interval: 30            # an OPTIONS every 30 seconds keeps each phone's NAT open
```

Forward or open, on the node's router and firewall:

- the SIP ports the phones use (UDP and TCP 5060, TCP 5061);
- UDP `media.builtin.port_min` to `port_max`, 22000 to 23000 by default. A call can use
  any port in the range, so forward all of it.

Media is relayed through the node by default (`behaviour.media_anchor`), so two phones that
cannot reach each other still hear each other. [Media](../media.md) explains the relay and
when to use rtpengine instead.

Where a phone offers TCP or TLS, prefer it: the node keeps answering down the connection
the phone opened, which stays open through NAT, where a UDP mapping can close between
calls without anyone noticing.

## Check it

```
curl "$ATHENA_API/registrations" -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN"
```

Each entry is one registered device, with the address it registered from. A subscriber
with two devices has two entries, and a call to it rings them one at a time, newest first.

`test/interop/smoke.py` registers on every transport without a phone, which tells a node
problem from a client problem:

```
test/interop/smoke.py --host 203.0.113.5 --realm example.com --user alice \
  --password alice-secret --transports udp,tcp
```

Add `tls` with `--ca` naming the authority to trust, and `ws` with `--ws-port`.

When something does not work, [Troubleshooting](../troubleshooting.md) goes from the
response code or log line to the fix: [Cannot register](../troubleshooting.md#cannot-register),
[Calls fail](../troubleshooting.md#calls-fail) and
[One-way or no audio](../troubleshooting.md#one-way-or-no-audio).
