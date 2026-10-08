![AthenaSIP Logo](docs/logos/athenasip_small_white.png)

# AthenaSIP

AthenaSIP is a multi-master, clusterable SIP server built for standards compliance and
for running without a telecoms background. A single node needs nothing but itself, and
every external piece it can use is a plugin behind one contract.

* **Standards first.** RFC 3261 in full - the four transaction machines, timers A to K,
  section 16 proxy behaviour - plus 3263, 3327, 3581, 4028, 5626, 6026, 7118, 8760 and
  8866, proven by a sipp harness.
* **Runs on its own.** The defaults are `memory://`, `local://` and `builtin://`: one
  process, no database, no broker, no media server. Redis, MQTT and rtpengine are there
  when you want them.
* **Clusters.** Nodes share subscribers through Redis, find each other over MQTT and
  forward calls to each other over mutual TLS, with a certificate authority the node makes
  for itself. When a node dies its clients register again through another.
* **Browsers and phones alike.** SIP over WebSocket and secure WebSocket (RFC 7118), with
  WebRTC media through rtpengine, which also converts between a browser and a desk phone.
  TURN credentials are minted per client.
* **Mobile push.** Phones that sleep are woken for a call through Apple Push, Firebase
  Cloud Messaging or Web Push (RFC 8599).
* **Routing you can script.** Who may call and where a call goes is a policy: the
  built-in one serves the node's own realms, and `lua://` makes it Lua scripts that can do
  anything else. The standard scripts behave exactly as the built-in policy, and the tests
  hold them to it.
* **Trunks.** Carriers and PBXs are records over the API. The node registers to them,
  answers their challenges and opens TLS to them itself; `athenasip.trunks` routes numbers
  out by prefix and in by number with no Lua written ([Scripting](docs/scripting.md)).
* **Pluggable by contract.** The datastore, event bus, media engine, push services and
  routing policy are plugin kinds in a registry keyed by URL scheme. The contract is versioned and async, the
  in-tree drivers use the same contract an external one would, and a plugin can be a
  shared library loaded at start.
* **API-driven.** Realms, subscribers and administrators are provisioned over a JSON API
  described by [an OpenAPI document](docs/api/openapi.yaml). Administrators sign in for a
  session token and hold roles; there are no configured tokens to leak. The React admin
  console is [AthenaSIP Admin](https://github.com/blackravenltd/athenasip-admin).
* **Easy to configure.** One annotated YAML file. `athenasip --check` tries every
  dependency and certificate before you start, a misspelt key is named with the one you
  meant, and `athenasip --print-schema` gives your editor a schema to check the file as you
  type.

## Get started

```
docker/up.sh
```

AthenaSIP with Redis, Mosquitto, rtpengine and coturn, a realm, two subscribers and an
administrator, in one command. The [Quick Start](docs/quick_start.md) has a guide for each
next step:

* [Try it in Docker](docs/quick-start/docker.md)
* [One node by hand](docs/quick-start/one-node.md)
* [A server on Linux](docs/quick-start/linux-server.md)
* [Connecting phones](docs/quick-start/phones.md)
* [Calling from a browser](docs/quick-start/browser.md)
* [Your first trunk](docs/quick-start/first-trunk.md)
* [Running a cluster](docs/clustering.md)

## Companion projects

* [AthenaSIP Admin](https://github.com/blackravenltd/athenasip-admin) - the administration
  console, with a phone in the browser. A static React bundle the node serves beside its
  admin API (`docker/up.sh --console`). Alpha.
* [AthenaPhone](https://github.com/blackravenltd/athenaphone) - an open source SIP
  softphone for Android, audio and video over UDP, TCP, TLS or WebSocket with WebRTC
  media, for AthenaSIP or any standards-compliant SIP server. Working, not yet released.

## Limits

* Clustering is tested with two nodes, including one dying and its clients registering
  again through the other. A call in progress on a node that dies ends with it.
* Push is tested against local stand-ins for Apple, Firebase and Web Push, not yet
  against the real services.
* Trunks are tested against local stand-ins for a carrier, not yet against a real one.
* Conferencing and presence are not built yet.
* The example configuration enables plaintext UDP and TCP so a first call is easy;
  `sip.allow_unencrypted: false` limits a node to TLS and secure WebSocket.

## Documentation

* [Quick Start](docs/quick_start.md) - a guide for each way in
* [Installation](docs/installation.md) - the binary, the configuration and the systemd unit
* [Compiling](docs/compiling.md) - dependencies and build presets
* [Configuration](docs/configuration.md) and its [reference](docs/configuration-reference.md)
* [How a call works](docs/how-a-call-works.md) - one call through a node, step by step
* [Media](docs/media.md) - the builtin relay and rtpengine, ports and addresses
* [Running a cluster](docs/clustering.md) - two nodes, step by step
* [Troubleshooting](docs/troubleshooting.md) - from the symptom to the cause
* [Behaviour](docs/behaviour.md) - the choices SIP servers differ on
* [Scripting](docs/scripting.md) - routing in Lua, and trunks
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
