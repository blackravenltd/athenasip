![AthenaSIP Logo](docs/logos/athenasip_small_white.png)

# AthenaSIP

**Project Status: ALPHA - DO NOT USE**

AthenaSIP is a multi-master, clusterable SIP server built for standards compliance and
for being possible to run without a telecoms background. It ships with everything a
single node needs in-process, and every external piece it can use is a plugin behind one
contract.

## What it is

* **Standards first.** RFC 3261 in full - the four transaction machines, timers A to K,
  section 16 proxy behaviour - plus 3263, 3327, 3581, 4028, 5626, 7118, 8760 and 8866.
  Compliance is proven by a sipp harness rather than asserted.
* **Runs on its own.** The defaults are `memory://`, `local://` and `builtin://`: one
  process, no database, no broker, no media server. Redis, MQTT and rtpengine are what
  it uses when you want them.
* **Pluggable by contract.** `Datastore`, `EventSystem` and `MediaEngine` are plugin
  kinds behind one registry keyed by URL scheme. The contract is versioned and async,
  and the in-tree drivers use exactly the contract an external one would.
* **Browser-ready.** SIP over WebSocket and secure WebSocket (RFC 7118) are here now,
  because a browser is a first-class client rather than a later port.
* **API-driven.** Realms, accounts and registrations are provisioned over a JSON API
  with bearer tokens, described by [an OpenAPI document](docs/api/openapi.yaml). The
  React admin client is [AthenaSIP Admin](https://github.com/blackravenltd/athenasip-admin).

## What it is not, yet

Clustering is designed and not built: one node works, and the second node is Milestone 4
in [`TODO/ACTIVE.md`](TODO/ACTIVE.md). WebRTC needs rtpengine, which is Milestone 3.
Conferencing and presence are further out. The plan is in the repository rather than in
a roadmap page, so it says what is true.

Plaintext UDP and TCP are enabled by default today for evaluation. TLS everywhere is the
intent and `sip.allow_unencrypted` is the switch, but calling it TLS-only would be a
claim the shipped configuration does not support.

## Documentation

* [Quick Start](docs/quick_start.md) - two accounts calling each other, from nothing
* [Installation](docs/installation.md)
* [Compiling](docs/compiling.md)
* [Configuration](docs/configuration.md)
* [Architecture](docs/architecture.md) - the shape, and why it is that shape
* [Design](docs/design.md) - the transport layering
* [Writing a plugin](docs/plugins.md)
* [Events](docs/events.md) - the topic scheme
* [Admin API](docs/api/openapi.yaml)

The plan and the decisions behind it are in [`TODO/ACTIVE.md`](TODO/ACTIVE.md); what has
landed is in [`TODO/COMPLETED.md`](TODO/COMPLETED.md).

## License

AthenaSIP is licensed under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html). Please see the [LICENSE](LICENSE) file.
