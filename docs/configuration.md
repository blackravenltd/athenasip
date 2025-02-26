# AthenaSIP - Configuration

AthenaSIP uses a YAML configuration file. The server will search, in order:

1. ~/.athenasip/config.yaml
2. /usr/etc/athenasip/config.yaml

## Configuration

```yaml
sip:
  realm: "sip.athenasip.org"
  nonce_secret: "your_nonce_secret_here"
  allow_unencrypted: true

tcp:
  enable: true
  address: 0.0.0.0
  port: 5060

tls:
  enable: true
  address: 0.0.0.0
  port: 5061
  cert_pem_filename: "../tls/snakeoil.cer"
  key_pem_filename: "../tls/snakeoil.key"

rtprelay:
  enable: true
  address: 0.0.0.0
  public_address: 0.0.0.0
  port_min: 22000
  port_max: 23000

db:
  url: "mysql://root@localhost/athenasip"
```

### `sip` Section

This section configures the server itself.

#### `realm`

The default SIP realm of the server.

#### `nonce_secret`

The secret for constucting authentication nonces. 

#### `allow_unencrypted`

If `true`, the server will reject SIP `INVITE` requests that do not describe encrypted media.
This setting can be `true` even if the TCP server is enabled - in which case the SIP flow will
be unencrypted, but the server will still reject attempts to initate unencrypted calls.

### `tcp` Section

This section configures TCP listener for the server. TCP is one possible transport
for SIP.

**WARNING** The TCP transport is unencrypted.

#### `enabled`

Whether to enable the TCP server, defaults to `true` if the section is present.

#### `address`

The address to bind to, e.g. `0.0.0.0`

#### `port`

The port to listen on, defaults to `5061`.

### `tls` Section

This section configures Transport Layer Security for the server.

#### `enabled`

Whether to enable the TLS server, defaults to `true` if the section is present.

#### `cert_pem_filename`

The filename for the PEM format server certificate.

#### `key_pem_filename`

The filename for the PEM format server key.

### `rtprelay`

Configures the built-in UDP/RDP relay. The relay exposes pairs of ports and relays UDP packets
between them, sending outgoing packets to the source endpoint of the first packet received. 

#### `enable`

Whether to enable the UDP/RDP relay, defaults to `true` if the section is present.
  
#### `address`

The address to bind to, e.g. `0.0.0.0`

#### `public_address`

The IP address to advertise (and replace in SDP) - this should be a real public IP address.

#### `port_min`

The first port in the available port range for the relay, e.g. `22000`

#### `port_max`

The last port in the available port range for the relay, e.g. `23000`

### `db` Section

This section configures the database.

#### `url`

The URL to access the database, in the format: `scheme://user[:password]@host[:port]/databasename`. AthenaSIP ships with support for the following:

| Database                                  | Scheme     | Example URL                           |
|:------------------------------------------|:-----------|:--------------------------------------|
|[MySQL](https://www.mysql.com/)            | `mysql`    | `mysql://root@localhost/athenasip`    |
|[MariaDB](https://mariadb.org/)            | `mariadb`  | `mariadb://root@localhost/athenasip`  |
|[PostgreSQL](https://www.postgresql.org/)  | `postgres` | `postgres://root@localhost/athenasip` |
