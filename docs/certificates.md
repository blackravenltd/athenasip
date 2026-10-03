# Certificates for a cluster

Nodes in a cluster talk SIP to each other over mutual TLS. Each node proves who it is with a
certificate signed by the cluster's own certificate authority, and trusts what that
authority signed and nothing else. AthenaSIP makes both, so nothing here needs OpenSSL's
command line.

## Make the authority, once

```sh
athenasip --ca-init
```

This writes `ca.key` and `ca.crt` to `~/.athenasip/ca` (or to `--ca-dir PATH`). The key is
the cluster: anyone holding it can make a certificate every node will trust. Keep it on one
machine, readable only by you, and somewhere it is backed up. AthenaSIP writes it readable
by its owner only, and refuses to make a second authority over the first, because that
would orphan every node certificate the first one signed.

## Issue each node its certificate

```sh
athenasip --ca-node node-a --san 10.35.1.20 --san node-a.example.com
```

This writes `node-a.key` and `node-a.crt` beside the authority. The certificate names the
node id and every address and host name given with `--san`, which should be every way
another node reaches it. It is good for both ends of a TLS connection, which mutual TLS
needs. A node certificate lasts two years. Issue it again with `--replace` to renew it or to
change its names. The authority lasts ten years.

Copy `ca.crt`, `node-a.crt` and `node-a.key` to node A. Only `ca.key` stays behind.

## Use them

```yaml
cluster:
  enable: true
  address: 0.0.0.0
  port: 5062
  advertise: node-a.internal
  ca: /etc/athenasip/cluster/ca.crt
  cert: /etc/athenasip/cluster/node-a.crt
  key: /etc/athenasip/cluster/node-a.key
```

This is the inter-node listener. A peer has to show a certificate the cluster CA signed,
and the node records which node it is from that certificate. The same certificates are used
for the TLS connections this node opens to its peers, where the peer's certificate has to
be signed by the cluster CA and name the address that was dialled. A `cluster` section
enabled without all three files stops the node at start-up, because a cluster listener
that cannot tell a node from anybody else would be open to anyone.

`advertise` is what this node tells its peers to dial, in its status on the event bus. It
has to be a name or an address the node's certificate carries (`--san`), because a peer
checks the certificate against what it dialled. Left out, it is `address`, and where that
is `0.0.0.0`, `sip.public_address`.

## What it makes

Keys are EC P-256. The authority is `CA:TRUE` with a path length of zero, so it signs node
certificates and cannot sign another authority. Node certificates are `CA:FALSE`, for
digital signature and key agreement, and both server and client authentication.
