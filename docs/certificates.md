# AthenaSIP - Certificates

A node uses certificates in two places:

- **For clients**: phones on TLS, browsers on secure WebSocket, and the admin listener on
  HTTPS. These need a certificate the clients already trust, from a public authority.
- **For the cluster**: nodes talk SIP to each other over mutual TLS, under an authority
  the cluster makes for itself.

## Certificates for clients

| Listener | Keys | Without its own |
|---|---|---|
| SIP over TLS | `tls.cert_pem_filename`, `tls.key_pem_filename` | Required when `tls` is enabled |
| Secure WebSocket, `websocket.tls: true` | `websocket.cert_pem_filename`, `websocket.key_pem_filename` | Required |
| Secure WebSocket, `websocket.secure_port` | `websocket.cert_pem_filename`, `websocket.key_pem_filename` | The `tls` section's |
| Admin API and console over HTTPS (`http.tls.enable`) | `http.tls.cert_pem_filename`, `http.tls.key_pem_filename` | The `tls` section's |

Each is a PEM file. The certificate file may hold the whole chain, the server certificate
first, and must name the host name clients connect to.

### From a public authority

Clients, and browsers especially, accept only a certificate from an authority they
already trust. [Let's Encrypt](https://letsencrypt.org/) issues them free. With
[certbot](https://certbot.eff.org/), for `sip.example.com`:

```sh
certbot certonly --standalone -d sip.example.com
```

```yaml
tls:
  enable: true
  port: 5061
  cert_pem_filename: /etc/athenasip/tls/fullchain.pem
  key_pem_filename: /etc/athenasip/tls/privkey.pem

websocket:
  port: 9443
  tls: true
  cert_pem_filename: /etc/athenasip/tls/fullchain.pem
  key_pem_filename: /etc/athenasip/tls/privkey.pem

http:
  port: 8080
  tls:
    enable: true      # HTTPS on 8443, with the tls section's files
```

Use `fullchain.pem`, not `cert.pem`: without the intermediate certificate many clients
refuse the connection. certbot keeps its files readable by root only, and the systemd unit
runs the node as the `athenasip` user, so copy the two files somewhere that user can read
them, as above, from a certbot deploy hook. The node reads certificates when it starts:
restart it after each renewal.

`athenasip --check` loads every certificate a listener needs and reports each.

### The snakeoil certificate

`tls/` holds a certificate and a certificate authority for trying TLS and WSS on a test
machine. Their private keys are in the repository, so anyone can impersonate a node using
them: never use them on a node others can reach. Clients refuse them unless told to
trust `tls/ca/snakeca.crt`. `tls/generate.sh` makes them again.

## Certificates for a cluster

Each node holds a certificate signed by the cluster's own certificate authority and
trusts only what that authority signed. AthenaSIP makes both; no `openssl` commands are
needed. [Running a cluster](clustering.md) is the whole setup.

### Make the authority, once

```sh
athenasip --ca-init
```

This writes `ca.key` and `ca.crt` to `~/.athenasip/ca`, or to `--ca-dir PATH`. Anyone
holding `ca.key` can make a certificate every node will trust: keep it on one machine,
backed up. It is written readable by its owner only. `--ca-init` refuses to overwrite an
existing authority, because that would orphan every certificate it signed.

### Issue each node its certificate

```sh
athenasip --ca-node node-a --san 10.35.1.20 --san node-a.example.com
```

This writes `node-a.key` and `node-a.crt` beside the authority. Give a `--san` for every
address and host name another node reaches this one by. `--replace` issues again over an
existing certificate, to renew it or change its names.

Copy `ca.crt`, `node-a.crt` and `node-a.key` to node A. `ca.key` stays where it is.

### Use them

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

### What is made

| | Authority | Node |
|---|---|---|
| Key | EC P-256 | EC P-256 |
| Lifetime | ten years | two years |
| Basic constraints | `CA:TRUE`, path length 0 | `CA:FALSE` |
| Usage | certificate and CRL signing | digital signature, key agreement; server and client authentication |
