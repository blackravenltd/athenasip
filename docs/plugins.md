# AthenaSIP - Writing a Plugin

The datastore, the event system, the media engine and the push services are plugins. The
drivers in the tree use the same contract an external one does, and none is privileged.

| Kind | Interface | In tree |
|---|---|---|
| `datastore` | `datastores::Datastore` (`src/datastores/datastore.h`) | `memory://`, `redis://` |
| `events` | `events::EventSystem` (`src/events/event_system.h`) | `local://`, `mqtt://` |
| `media` | `media::MediaEngine` (`src/media/media_engine.h`) | `builtin://`, `rtpengine://` |
| `push` | `push::PushService` (`src/push/push_service.h`) | `apns://`, `fcm://`, `webpush://` |

A kind is a string, so a plugin can introduce a new one. A plugin is either compiled into
the server or built as a module (a shared library) the server loads at start: see
[Building a module](#building-a-module).

## The base contract

Every plugin derives from `plugins::Plugin` (`src/plugins/plugin.h`) through the interface
for its kind:

```cpp
std::string kind() const;           // "datastore", "events", "media", "push"; set by the interface
std::string name() const;           // the implementation: "redis", whatever scheme selected it
std::string version() const;        // the driver's version, not the server's
std::uint32_t api_version() const;  // the contract it was built against; do not override
bool configure(const YAML::Node& own_root, const Config& system);  // optional
bool health() const;                // what the driver already knows; no round trip
```

The constructor takes the logger and the parsed URL:

```cpp
MyDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
```

## Registering

```cpp
Datastore::register_driver<MyDatastore>(logger, "mystore");
```

The key is `(kind, scheme)`, so one scheme can name a driver of each kind. Registering a
pair again replaces the driver. A driver may be registered under several schemes. The
built-ins are registered from `main()` by the `register_builtin_*` functions in
`src/datastores/datastore_drivers.h`, `src/events/event_system_drivers.h`,
`src/media/media_engine_drivers.h` and `src/push/push_service_drivers.h`; add yours there,
or build it as a module.

`Datastore::create_driver(logger, url)` constructs the driver for a URL.

## Configuration

The URL selects the driver. Anything a URL cannot express goes in a section named after
the driver's `name()`, inside the section for its kind:

```yaml
media:
  url: "builtin://"
  builtin:
    public_address: 203.0.113.5
    port_min: 22000
    port_max: 23000
```

`configure(own_root, system)` is called once, before `connect()`. `own_root` is that
section, undefined when absent. `system` is the whole server configuration (node id,
timers). Returning `false` stops startup.

A driver can describe its section with a static `settings()`. The registry picks it up
when the driver registers, compiled in or from a module, and the section then joins the
[reference](configuration-reference.md), the editor schema from `athenasip --print-schema`,
and the warning for a misspelt key. A module's section is checked once the module has
loaded; `--print-schema` loads no modules, so it describes the built-in drivers only. Keys start at the section's own name; the registry
puts them under the kind's section:

```cpp
static plugins::Settings settings() {
  using namespace plugins::define;
  return {
      section("acme", "The acme:// media engine."),
      required(text("acme.api_key_file", "", "The key the Acme console issues.")),
      integer("acme.timeout_ms", "500", "How long to wait for an answer, in milliseconds.", 1),
  };
}
```

Without it the section is still handed to the driver, and the node leaves its keys alone.

## Lifecycle

```
construct(logger, url) -> configure(own_root, system) -> connect(on, handler) -> ... -> close()
```

`connect()` is asynchronous and must make a real round trip to the far end. `close()` is
synchronous and safe to call twice.

## Async rules

`Core` runs on one strand, so a blocking plugin call would stall every call on the node.
Every operation therefore takes the caller's executor and a handler:

```cpp
void realm_get_by_name(plugins::Executor on, std::string realm_name,
                       plugins::Handler<std::shared_ptr<types::Realm>> handler);
void realm_create(plugins::Executor on, std::shared_ptr<types::Realm> realm,
                  plugins::StatusHandler handler);
```

- Run the handler on `on`, never on your own I/O thread.
- Never call the handler inline, even when the answer is already known. Post it.
  `Plugin::_complete(on, handler, value)` does both.
- An operation needing several round trips chains them from each completion.

Results are `plugins::Status {ok, error}` and `plugins::Result<T> {ok, error, value}`.
A read that finds nothing succeeds with an empty value; a read that could not happen
fails with an error. The API maps the first to 404 and the second to a 5xx, so a driver
must not conflate them.

The one fire-and-forget operation is `EventSystem::publish(event_name, message)`: the bus
is never on the call path, so callers do not wait on it. Use
`publish(on, event_name, message, handler)` to hear the outcome.

## Kind-specific notes

### Datastore

- `user_*` and `session_*` default to a failure such as `memory does not support
  user_get`. Implement them if the store should hold admin users
  ([Authentication](authentication.md)).
- `realm_create`, `user_create` and `subscriber_create` fail if the record exists;
  the matching `*_update` fails if it does not. The API tells "already exists" from
  "not found" by which failed.
- `session_delete` succeeds whether or not the hash was held.
- `realm_delete` also deletes the realm's subscribers and their bindings.
- A session's absolute expiry and a binding's `expires_seconds` are the store's to
  enforce.

### EventSystem

- `publish_state` publishes a retained message; `will_set` registers a message for the
  broker to publish if the node vanishes, and is called before `connect()`. Both have
  defaults for a bus with neither concept.
- `subscribe` accepts MQTT-style filters (`+`, `#`); `TopicFilter` implements the
  matching. [Events](events.md) lists the topics.

### MediaEngine

- `capabilities()` reports `bridge`, `conference`, `record` and `transcode`;
  `produces(profile)` reports which media profiles the engine can generate.
- `offer`, `answer`, `release` and `query` are required. An engine that cannot carry an
  offer returns a failed `media::Result`; the SDP then passes through untouched.
- `start_recording`, `stop_recording`, `join`, `leave` and `roster` default to a failure
  or an empty answer.
- `packets_relayed()` feeds metrics and may return nothing.

### PushService

- A node runs several, one per [RFC 8599](https://www.rfc-editor.org/rfc/rfc8599) push
  service, and `name()` is the service's `pn-provider` value: `apns`, `fcm`, `webpush`.
- `accepts(notification)` says whether a Contact carries what the service needs (RFC 8599
  sections 10 to 12). The registrar answers 555 when it does not.
- `capabilities()` adds indicators beside `+sip.pns` in the REGISTER's 2xx, such as
  `+sip.vapid`.
- `send` succeeds when the service took the push, not when the client woke. The proxy
  waits for the client to register again either way.
- `connect` and `close` default to nothing; credentials are read in `configure`.

Where a rule like these matters, it is stated at the operation's declaration: read the
comment, not only the signature.

## Building a module

A module is a shared library (`.so`, `.dylib` or `.dll`) in a directory named by
[`plugins.path`](configuration.md#plugins). At start the server loads each one, registers
what it declares beside the built-in drivers, and refuses, with the reason in the log, a file
that is not a module or one built against another contract version.

```cpp
#include "plugins/plugin_module.h"
#include "datastores/datastore.h"

class AcmeDatastore : public athenasip::datastores::Datastore { /* ... */ };

void register_acme(athenasip::plugins::ModuleHost& host) {
  host.add<AcmeDatastore>(athenasip::plugins::kinds::datastore, "acme");
}

ATHENASIP_PLUGIN_MODULE("acme", register_acme)
```

The kinds are named in `plugins::kinds` (`datastore`, `events`, `media`), and the push
service's in `push::kind`.

Rules:

- Build against the server's `src/` headers with the same compiler, standard library and
  Boost, yaml-cpp and OpenSSL versions, because `std::shared_ptr`, `YAML::Node` and Boost
  executors cross the boundary.
- Do not link `athena_core`: the module would get a registry of its own. Leave the
  server's symbols undefined and let them resolve against the running server, which
  exports them (`-undefined dynamic_lookup` on macOS; the default on Linux).
- The contract version is exported by `ATHENASIP_PLUGIN_MODULE`; a module built against
  another is refused before any of its code runs.
- A loaded module stays loaded for the life of the process.

`athenasip --list-plugins` loads the modules, says which loaded and why any did not, and
lists every driver by kind and scheme. `tests/modules/sample_module.cpp` is a complete
module, built and loaded by the test suite.

## Versioning

`plugins::API_VERSION` in `src/plugins/plugin.h` is the contract version, bumped whenever
`Plugin` or an interface derived from it changes shape. The registry refuses to construct
a plugin whose `api_version()` differs:

```
Plugin acme 1.0.0 was built against contract version 1, this server speaks 2
```

The version tracks shape only. A change of meaning under the same signatures, such as an
operation that now succeeds where it failed, does not move it, and is stated at the
declaration instead.

An operation added to an interface is given a failing default rather than made pure
virtual, so existing drivers keep compiling.

## Testing

Tests are GoogleTest under `tests/`, mirroring `src/`; see [Testing](testing.md).

| Helper | Use |
|---|---|
| `tests/helpers/sync_datastore_helper.h`, `sync_event_system_helper.h`, `sync_media_engine_helper.h` | Blocking views of a driver, for asserting on what it holds |
| `tests/helpers/fake_push_service_helper.h` | A push service that records what it was asked to send |
| `tests/plugins/plugin_registry_test.cpp`, `module_loader_test.cpp` | The registry's and the module loader's own tests |

Test the async rules themselves (handler on the given executor, never inline, failures
reported) by calling the driver directly.
