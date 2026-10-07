# Quick Start - A Server on Linux

A node that stays up: built from source on Debian 13 or Ubuntu, run by systemd as its own
user, keeping its data in Redis so it survives a restart, with TLS for phones and HTTPS
for the admin API. About half an hour, most of it compiling.

[Installation](../installation.md) is the reference for each step.

## 1. Build and install

The build dependencies:

```
apt install build-essential cmake pkg-config git wget ca-certificates \
  libssl-dev libyaml-cpp-dev libnghttp2-dev
```

AthenaSIP needs Boost 1.87 or newer. Debian 13 ships 1.83, so build the three libraries it
links and install them with the headers under `/usr/local`:

```
wget https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.gz
tar xzf boost_1_89_0.tar.gz && cd boost_1_89_0
./bootstrap.sh --with-libraries=thread,json,charconv
sudo ./b2 -j"$(nproc)" --with-thread --with-json --with-charconv install
sudo ldconfig
cd ..
```

Then AthenaSIP itself:

```
git clone https://github.com/blackravenltd/athenasip.git
cd athenasip
git checkout "$(git tag --sort=-v:refname | head -1)"   # the latest release
cmake --preset release
cmake --build build-release -j"$(nproc)"
sudo cmake --install build-release --prefix /usr/local
athenasip --version
```

That installs the binary, a systemd unit and `/etc/athenasip/config.yaml`, copied from
the annotated example.

## 2. The service account

```
sudo useradd --system --no-create-home --shell /usr/sbin/nologin athenasip
sudo chown -R root:athenasip /etc/athenasip
sudo chmod 0750 /etc/athenasip
sudo chmod 0640 /etc/athenasip/config.yaml
```

## 3. Redis

```
sudo apt install redis-server
```

Debian's Redis listens on loopback only, which is what a single node wants.

## 4. A certificate

Phones on TLS and the admin API on HTTPS need a certificate naming the host, here
`sip.example.com`. [Certificates](../certificates.md#from-a-public-authority) has the
Let's Encrypt steps; below, the files are in `/etc/athenasip/tls/`, readable by the
`athenasip` group.

## 5. Configure

Edit `/etc/athenasip/config.yaml`. These are the settings that differ from the example;
leave the rest as they are.

```yaml
sip:
  node_id: sip-1
  public_address: sip.example.com      # the name or address phones reach this host on
  # localnet: ["192.168.0.0/16"]       # the host's own network, when it is behind NAT

tls:
  enable: true
  port: 5061
  cert_pem_filename: /etc/athenasip/tls/fullchain.pem
  key_pem_filename: /etc/athenasip/tls/privkey.pem

datastore:
  url: "redis://127.0.0.1:6379/0"

media:
  url: "builtin://"
  builtin:
    public_address: sip.example.com    # where phones send media

log:
  level: info

http:
  port: 8080
  tls:
    enable: true                       # HTTPS on 8443, with the tls section's certificate
  files:
    enable: false                      # until a console is installed for it to serve
```

The admin API answers on plain HTTP on 8080 too. Bind `http.address` to `127.0.0.1` to
keep that on the host and use 8443 from elsewhere. `sip.allow_unencrypted: false`, with
the `udp`, `tcp` and plain `websocket` sections removed, limits phones to TLS.

Check the file before starting anything:

```
sudo -u athenasip athenasip --config /etc/athenasip/config.yaml --check
```

It runs as the service user, so it also proves that user can read the file and the
certificate.

Each line is `ok` or `FAIL`, with the reason. A misspelt key is reported as a warning
naming the key that was probably meant. For checking as you type,
[Configuration](../configuration.md) shows how to give an editor the schema.

## 6. The first administrator, then start it

```
sudo -u athenasip athenasip --config /etc/athenasip/config.yaml --add-user admin \
  --role manage-admin-users --role manage-realms \
  --role manage-realm-subscribers --role view-cluster-status

sudo systemctl daemon-reload
sudo systemctl enable --now athenasip
journalctl -u athenasip -f
```

`--add-user` prompts for the password, writes the user to Redis and exits.

```
curl https://sip.example.com:8443/api/v1/health
```

## 7. Open the ports

| Port | For |
|---|---|
| UDP and TCP 5060 | SIP, plain |
| TCP 5061 | SIP over TLS |
| TCP 9500 | SIP over WebSocket, if browsers will use it |
| TCP 8443 | The admin API over HTTPS |
| UDP 22000 to 23000 | Media through the builtin relay |

Behind NAT, forward the same ports from the router to the host, and list the host's own
network in `sip.localnet`. On a host shared with other services, check nothing else holds
these ports (`ss -lntup`) and bind each listener to one address rather than `0.0.0.0`.

## 8. Add subscribers

Sign in and create a realm and its subscribers, as in
[One node by hand](one-node.md#2-sign-in), against `https://sip.example.com:8443/api/v1`.
Name the realm for the domain phones register to. [Connecting phones](phones.md) has the
client settings.

## Afterwards

- **Upgrading**: check out the new tag, build, `cmake --install` again and restart. The
  configuration is never overwritten; `/usr/local/share/doc/athenasip/config.example.yaml`
  is replaced with the new example, for comparison.
- **Browsers**: [Calling from a browser](browser.md) adds rtpengine, secure WebSocket and TURN.
- **A second node**: [Running a cluster](../clustering.md).
- **Logs and health**: `journalctl -u athenasip`, `GET /api/v1/health`, and `GET /metrics`
  in the Prometheus format.
- **Something wrong**: [Troubleshooting](../troubleshooting.md).
