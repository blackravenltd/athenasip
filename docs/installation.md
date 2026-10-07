# AthenaSIP - Installation

A node is one binary, one configuration file and one systemd unit. The defaults use the
in-process datastore, event system and media relay, so a working node needs no Redis, no
broker and no media server.

## From source

Dependencies are in [compiling.md](compiling.md).

```bash
cmake --preset release
cmake --build build-release -j"$(nproc)"
sudo cmake --install build-release --prefix /usr/local
```

That installs:

| | |
|---|---|
| `/usr/local/bin/athenasip` | the binary |
| `/etc/athenasip/config.yaml` | the live configuration, only if there is not one already |
| `/usr/local/share/doc/athenasip/config.example.yaml` | the annotated example, always |
| `/lib/systemd/system/athenasip.service` | the unit (Linux only) |

Installing over an existing node keeps its configuration.

With a `--prefix` other than `/usr` or `/usr/local` the install is self-contained: the
configuration goes to `<prefix>/etc/athenasip` and the unit to
`<prefix>/lib/systemd/system`. `-DATHENA_CONFIG_INSTALL_DIR=` and
`-DATHENA_SYSTEMD_UNIT_DIR=` at configure time move either. `DESTDIR=` stages the whole
tree for packaging without touching the host.

## The service account

The unit runs as `athenasip`, which the install does not create:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin athenasip
sudo chown -R root:athenasip /etc/athenasip
sudo chmod 0750 /etc/athenasip
sudo chmod 0640 /etc/athenasip/config.yaml
```

Keep the configuration readable by the group and writable only by root: it can hold
secrets such as `http.api.turn_shared_secret`.

## Running it

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now athenasip
systemctl status athenasip
journalctl -u athenasip -f
```

| | |
|---|---|
| `athenasip --version` | print the version |
| `athenasip --config PATH` | read a particular configuration |
| `athenasip --print-config` | print the effective configuration, defaults resolved, and exit |
| `athenasip --print-schema` | print every setting as a JSON Schema for an editor, or with `=markdown` as the reference, and exit |
| `athenasip --check` | try the datastore, event bus, media engine, certificates and cluster peers, report, and exit |
| `athenasip --list-plugins` | load the plugin modules in `plugins.path`, say which loaded, list every driver, and exit |
| `athenasip --help` | every option |

With no `--config`, the node reads `$ATHENASIP_CONFIG` when it is set, otherwise
`/etc/athenasip/config.yaml` when it exists, otherwise `~/.athenasip/config.yaml`. The unit passes
`--config /etc/athenasip/config.yaml`.

## Configuring it

The installed `config.example.yaml` is annotated throughout and
[configuration.md](configuration.md) explains every setting. Set these first:

- `sip.public_address` - the address clients reach this node on. The node writes it into
  Via and Record-Route, so a wrong one sends replies nowhere.
- `media.builtin.public_address` - the address clients send media to, when using the
  built-in relay.
- `datastore.url` and `events.url` - `memory://` and `local://` for a single node;
  `redis://` and `mqtt://` for a cluster or for data that survives a restart.

Every listener's address and port is in the configuration. On a shared host, check the
defaults are free and bind to one address rather than `0.0.0.0`:

```bash
ss -lntup | grep -E ':(5060|5061|5062|8080|8443|9500)\b'
```

The builtin media relay also needs its UDP range, 22000 to 23000 by default, free and open
in the firewall ([Media](media.md#ports-to-open)).

## The first administrator

There are no API tokens to configure. Make the first administrator on the host, against
the configured datastore:

```bash
sudo -u athenasip athenasip --config /etc/athenasip/config.yaml --add-user admin \
  --role manage-admin-users --role manage-realms \
  --role manage-realm-subscribers --role view-cluster-status
```

It prompts for the password without echoing it, or reads it from standard input when
that is not a terminal. Without `--role` the user holds `manage-admin-users` only.
`--reset-password NAME` sets a new password for an existing user and ends its sessions.

With `memory://` nothing outlives the process, so `--add-user` creates the user and then
carries on as the node. Use `redis://` for a node run by systemd.

Realms and subscribers are provisioned over the admin API, signed in as that user:
[One node by hand](quick-start/one-node.md#2-sign-in) has the calls and
[authentication.md](authentication.md) the model. [A server on Linux](quick-start/linux-server.md)
is this page as one walk-through.

## What the unit restricts

The unit runs unprivileged with no capabilities, may write only to `/var/lib/athenasip`
and `/var/log/athenasip`, may open only `AF_INET` and `AF_INET6` sockets, and sets
`MemoryMax=512M`, `TasksMax=256` and `LimitNOFILE=16384`.

No default port is privileged. For a port below 1024, put a proxy in front rather than
granting `CAP_NET_BIND_SERVICE`.

Override settings with `systemctl edit athenasip`, not by editing the installed unit,
which an upgrade replaces.

## Uninstalling

```bash
sudo systemctl disable --now athenasip
sudo rm -f /lib/systemd/system/athenasip.service /usr/local/bin/athenasip
sudo rm -rf /usr/local/share/doc/athenasip
sudo systemctl daemon-reload
```

`/etc/athenasip` and `/var/lib/athenasip` hold configuration and state and are left for
you to remove.
