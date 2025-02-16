# AthenaSIP - Configuration

AthenaSIP uses a YAML configuration file. The server will search, in order:

1. ~/.athenasip/config.yaml
2. /usr/etc/athenasip/config.yaml

## Configuration

```yaml
sip:
  address: 127.0.0.1
  port: 5061
  realm: "sip.athena.org"
  nonce_secret: "your_nonce_secret_here"

tls:
  cert_pem_filename: "../tls/snakeoil.cer"
  key_pem_filename: "../tls/snakeoil.key"

db:
  url: "mysql://root@localhost/athenasip"
```

### `sip` Section

This section configures the server itself.

#### `address`

The address to bind to, e.g. `127.0.0.1`

#### `port`

The port to listen on, defaults to `5061`.

#### `realm`

The default SIP realm of the server.

#### `nonce_secret`

The secret for constucting authentication nonces. 

### `tls` Section

This section configures Transport Layer Security for the server.

#### `cert_pem_filename`

The filename for the PEM format server certificate.

#### `key_pem_filename`

The filename for the PEM format server key.

### `db` Section

This section configures the database.

#### `url`

The URL to access the database, in the format: `scheme://user[:password]@host[:port]/databasename`. AthenaSIP ships with support for the following:

| Database                                  | Scheme     | Example URL                           |
|:------------------------------------------|:-----------|:--------------------------------------|
|[MySQL](https://www.mysql.com/)            | `mysql`    | `mysql://root@localhost/athenasip`    |
|[MariaDB](https://mariadb.org/)            | `mariadb`  | `mariadb://root@localhost/athenasip`  |
|[PostgreSQL](https://www.postgresql.org/)  | `postgres` | `postgres://root@localhost/athenasip` |


