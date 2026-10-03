# AthenaSIP - Installation

A node is one static binary, one configuration file and one systemd unit. Nothing else
is required: the defaults use the in-process datastore, event system and media relay, so
a working node needs no Redis, no broker and no media server.

## From source

```bash
cmake --preset release
cmake --build build-release -j"$(nproc)"
sudo cmake --install build-release --prefix /usr/local
```

That puts:

| | |
|---|---|
| `/usr/local/bin/athenasip` | the binary |
| `/etc/athenasip/config.yaml` | the live configuration, **only if there is not one already** |
| `/usr/local/share/doc/athenasip/config.example.yaml` | the annotated original, always |
| `/lib/systemd/system/athenasip.service` | the unit |

Installing over an existing node keeps its configuration. An install that replaced it
would take the node down at the worst possible moment, so the example is installed
beside it instead and the live file is left alone.

`--prefix` somewhere other than `/usr` or `/usr/local` makes the install
self-contained: the configuration goes to `<prefix>/etc/athenasip` and the unit to
`<prefix>/lib/systemd/system`. `DESTDIR=` stages the whole tree for packaging without
touching the host.

Build dependencies are CMake, a C++20 compiler, Boost 1.87 or newer, OpenSSL and
yaml-cpp. Debian ships Boost 1.83, which is too old; `Dockerfile` builds it from source
for exactly that reason.

## The service account

The unit runs as `athenasip`, which is not created for you - a package would, and a
`cmake --install` deliberately does not create users on your host:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin athenasip
sudo chown -R root:athenasip /etc/athenasip
sudo chmod 0750 /etc/athenasip
sudo chmod 0640 /etc/athenasip/config.yaml
```

The configuration is readable by the group and writable only by root, because it holds
the admin API tokens.

## Running it

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now athenasip
systemctl status athenasip
journalctl -u athenasip -f
```

`athenasip --version` prints what the tag, CMake and the binary all agree on.
`athenasip --config PATH` reads a particular file; with nothing given, the first of
`$ATHENASIP_CONFIG`, `/etc/athenasip/config.yaml` and `~/.athenasip/config.yaml` that
exists is used - which is what lets a package and a checkout both work untold.

## What the unit assumes

A SIP server is a thing on a network that strangers can send bytes to, so the unit
assumes it will one day be the way in. It runs as its own unprivileged user with no
capabilities at all, can write to `/var/lib/athenasip` and `/var/log/athenasip` and
nothing else, can open `AF_INET` and `AF_INET6` sockets and no other address family,
and has `MemoryMax`, `TasksMax` and `LimitNOFILE` set so that a node under attack is
not the reason a shared host falls over.

Nothing needs a privileged port: SIP is 5060 and 5061, both above 1024. If you want
443, put a proxy in front rather than giving this process the ability to bind it.

Override anything with `systemctl edit athenasip` rather than editing the installed
unit, which an upgrade replaces.

## On a shared host

Check what is already listening before you start, because the defaults are the obvious
numbers and somebody else may have them:

```bash
ss -lntup | grep -E ':(5060|5061|8080|8088)\b'
```

Every listener's address and port is in the configuration. Bind to one address rather
than `0.0.0.0` where the host has several, and set `sip.public_address` to the address
clients actually reach the node on - a node writes its own address into Via and
Record-Route, so one that advertises the wrong one is a node whose replies go nowhere.

## Configuring it

`/usr/local/share/doc/athenasip/config.example.yaml` is annotated throughout and
`docs/configuration.md` explains every setting. The three that matter first:

- `sip.public_address` - what clients reach this node on.
- There are no API tokens to set. Make the first administrator with `athenasip --add-user`
  on this host; the admin API, which provisions realms and subscribers, is signed in to as
  that user.
- `datastore.url` and `events.url` - `memory://` and `local://` for a single node;
  `redis://` and `mqtt://` for a cluster.

Realms and subscribers are not configured here. They are provisioned over the admin API,
because a cluster shares them and a file on one node does not:

```bash
curl -fsS -X POST http://<node>:8080/api/v1/realms \
  -H 'Authorization: Bearer <admin token>' -H 'Content-Type: application/json' \
  -d '{"name":"example.com","registration_timeout":3600}'
```

## Uninstalling

```bash
sudo systemctl disable --now athenasip
sudo rm -f /lib/systemd/system/athenasip.service /usr/local/bin/athenasip
sudo rm -rf /usr/local/share/doc/athenasip
sudo systemctl daemon-reload
```

`/etc/athenasip` and `/var/lib/athenasip` are left for you to remove, because they hold
configuration and state that an uninstall has no business deciding about.
