# AthenaSIP - Certificates for a cluster

Nodes in a cluster talk SIP to each other over mutual TLS. Each node holds a certificate
signed by the cluster's own certificate authority and trusts only what that authority
signed. AthenaSIP makes both; no `openssl` commands are needed.

## Make the authority, once

```sh
athenasip --ca-init
```

This writes `ca.key` and `ca.crt` to `~/.athenasip/ca`, or to `--ca-dir PATH`. Anyone
holding `ca.key` can make a certificate every node will trust: keep it on one machine,
backed up. It is written readable by its owner only. `--ca-init` refuses to overwrite an
existing authority, because that would orphan every certificate it signed.

## Issue each node its certificate

```sh
athenasip --ca-node node-a --san 10.35.1.20 --san node-a.example.com
```

This writes `node-a.key` and `node-a.crt` beside the authority. Give a `--san` for every
address and host name another node reaches this one by. `--replace` issues again over an
existing certificate, to renew it or change its names.

Copy `ca.crt`, `node-a.crt` and `node-a.key` to node A. `ca.key` stays where it is.

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

This is the inter-node listener. A peer must present a certificate the cluster CA
signed. The same files are used for the connections this node opens to its peers, where
the peer's certificate must also name the address that was dialled. A node with
`cluster.enable` set and any of the three files missing refuses to start.

`advertise` is the name or address this node tells its peers to dial. It must be one the
node's certificate carries (`--san`). It defaults to `address`, or to
`sip.public_address` where `address` is `0.0.0.0`.

`athenasip --check` verifies the certificates and tries the other nodes.

## What is made

| | Authority | Node |
|---|---|---|
| Key | EC P-256 | EC P-256 |
| Lifetime | ten years | two years |
| Basic constraints | `CA:TRUE`, path length 0 | `CA:FALSE` |
| Usage | certificate and CRL signing | digital signature, key agreement; server and client authentication |
