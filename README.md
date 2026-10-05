![AthenaSIP Logo](docs/logos/athenasip_small_white.png)

# AthenaSIP

**Project Status: ALPHA - DO NOT USE**

AthenaSIP is a multi-master, clusterable SIP server built for standards compliance and
for running without a telecoms background. A single node needs nothing but itself, and
every external piece it can use is a plugin behind one contract.

* **Standards first.** RFC 3261 in full - the four transaction machines, timers A to K,
  section 16 proxy behaviour - plus 3263, 3327, 3581, 4028, 5626, 6026, 7118, 8760 and
  8866, proven by a sipp harness.
* **Runs on its own.** The defaults are `memory://`, `local://` and `builtin://`: one
  process, no database, no broker, no media server. Redis, MQTT and rtpengine are there
  when you want them.
* **Pluggable by contract.** `Datastore`, `EventSystem` and `MediaEngine` are plugin
  kinds in a registry keyed by URL scheme. The contract is versioned and async, and the
  in-tree drivers use the same contract an external one would.
* **Browser-ready.** SIP over WebSocket and secure WebSocket (RFC 7118), with WebRTC
  media through rtpengine.
* **API-driven.** Realms, subscribers and administrators are provisioned over a JSON API
  described by [an OpenAPI document](docs/api/openapi.yaml). Administrators sign in for a
  session token and hold roles. There are no configured tokens: the first administrator
  is made with `athenasip --add-user` on the host. The React admin client is
  [AthenaSIP Admin](https://github.com/blackravenltd/athenasip-admin).

## Try it

```
docker/up.sh
```

AthenaSIP with Redis, Mosquitto, rtpengine and coturn, a realm, two subscribers and an
administrator. [Quick Start](docs/quick_start.md) covers that and the same thing by hand
on one process.

## Limits

Clustering works between two nodes, including a node dying and its clients registering
again through the other; larger clusters are untested.
Conferencing and presence are not built. The example configuration enables plaintext UDP
and TCP for evaluation; `sip.allow_unencrypted: false` limits a node to TLS and WSS.

## Documentation

* [Quick Start](docs/quick_start.md) - two subscribers calling each other
* [Installation](docs/installation.md) - the binary, the configuration and the systemd unit
* [Compiling](docs/compiling.md) - dependencies and build presets
* [Configuration](docs/configuration.md)
* [How a call works](docs/how-a-call-works.md) - one call through a node, step by step
* [Media](docs/media.md) - the builtin relay and rtpengine, ports and addresses
* [Running a cluster](docs/clustering.md) - two nodes, step by step
* [Troubleshooting](docs/troubleshooting.md) - from the symptom to the cause
* [Behaviour](docs/behaviour.md) - the choices SIP servers differ on
* [Authentication](docs/authentication.md) - SIP Digest and the admin plane
* [Certificates](docs/certificates.md) - TLS for clients and the cluster's certificate authority
* [Architecture](docs/architecture.md)
* [Writing a plugin](docs/plugins.md)
* [Events](docs/events.md) - the topic scheme
* [Glossary](docs/glossary.md)
* [Testing](docs/testing.md)
* [Admin API](docs/api/openapi.yaml)
* [Interop fixture](test/interop/README.md) - a node to point a real SIP client at
* [Contributing](CONTRIBUTING.md)

## License

AthenaSIP is licensed under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html). See [LICENSE](LICENSE).
