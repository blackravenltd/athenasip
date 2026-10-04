# AthenaSIP - Writing a Plugin

The datastore, the event system and the media engine are plugins. The drivers in the tree
use the same contract an external one does, and none is privileged.

| Kind | Interface | In tree |
|---|---|---|
| `datastore` | `datastores::Datastore` (`src/datastores/datastore.h`) | `memory://`, `redis://` |
| `events` | `events::EventSystem` (`src/events/event_system.h`) | `local://`, `mqtt://` |
| `media` | `media::MediaEngine` (`src/media/media_engine.h`) | `builtin://`, `rtpengine://` |

A kind is a string, so a plugin can introduce a new one. Plugins are compiled into the
server; there is no shared-library loader.

## The base contract

Every plugin derives from `plugins::Plugin` (`src/plugins/plugin.h`) through the interface
for its kind:

```cpp
std::string kind() const;           // "datastore", "events", "media"; set by the interface
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
`src/datastores/datastore_drivers.h`, `src/events/event_system_drivers.h` and
`src/media/media_engine_drivers.h`; add yours there.

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

Where a rule like these matters, it is stated at the operation's declaration: read the
comment, not only the signature.

## Versioning

`plugins::API_VERSION` in `src/plugins/plugin.h` is the contract version, bumped whenever
`Plugin` or an interface derived from it changes shape. The registry refuses to construct
a plugin whose `api_version()` differs:

```
Plugin acme 1.0.0 was built against contract version 1, this server speaks 2
```

An operation added to an interface is given a failing default rather than made pure
virtual, so existing drivers keep compiling.

## Testing

Tests are GoogleTest under `tests/`, mirroring `src/`; see [Testing](testing.md).

| Helper | Use |
|---|---|
| `tests/helpers/sync_datastore_helper.h`, `sync_event_system_helper.h`, `sync_media_engine_helper.h` | Blocking views of a driver, for asserting on what it holds |
| `tests/plugins/plugin_registry_test.cpp` | The registry's own tests |

Test the async rules themselves (handler on the given executor, never inline, failures
reported) by calling the driver directly.
