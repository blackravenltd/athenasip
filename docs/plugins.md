# AthenaSIP - Writing a Plugin

Almost everything AthenaSIP talks to is a plugin: the datastore that holds realms,
subscribers and bindings; the event system it publishes to; the media engine that
anchors RTP. Routing policy and others follow. They all register through one contract,
and the ones that ship in the tree use exactly the contract an external plugin uses.

That is deliberate. `memory` + `redis`, `local` + `mqtt` and `builtin` + `rtpengine` are
what AthenaSIP tests and ships, but the architecture privileges none of them. DynamoDB,
NATS, Kafka or an SFU nobody has written yet are plugins someone can write, not roadmap
items the core has to carry.

## The shape

A plugin is a class deriving from `athenasip::plugins::Plugin` (`src/plugins/plugin.h`)
and from the interface for its kind:

| Kind | Interface | Ships in tree |
|---|---|---|
| `datastore` | `athenasip::datastores::Datastore` | `memory://`, `redis://` |
| `events` | `athenasip::events::EventSystem` | `local://`, `mqtt://` |
| `media` | `athenasip::media::MediaEngine` | `builtin://` |

A kind is a string, not an enum, so a plugin can introduce a kind the core was not built
knowing about.

`Plugin` is what every plugin has whatever it plugs into:

```cpp
std::string kind() const;           // "datastore", "events", "media", or your own
std::string name() const;           // "redis" - the implementation, not the scheme
std::string version() const;        // the driver's version, not the server's
std::uint32_t api_version() const;  // the contract it was built against
bool configure(const YAML::Node& own_root, const Config& system);
bool health() const;
```

`name()` is the implementation, not the URL scheme it was reached by. `RedisDatastore`
answers `redis` whether it was selected as `redis://`, `rediss://` or `redis+ssl://`.

## Registering

Registration is by `(kind, scheme)`:

```cpp
Datastore::register_driver<MyDatastore>(logger, "mystore");
```

which is a typed way of writing

```cpp
PluginRegistry::instance().add<MyDatastore>(logger, kinds::datastore, "mystore");
```

The key is the pair, not the scheme alone: `memory://` is a datastore and, separately,
an event system, and the registry has to keep them apart. Registering the same
`(kind, scheme)` twice replaces the driver, so a plugin can deliberately override a
built-in.

Your constructor takes the logger and the parsed URL:

```cpp
MyDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
```

## Configuration

The URL is the selector and stays a one-liner:

```yaml
datastore:
  url: "memory://"
```

Anything a URL cannot reasonably express comes from the section named after the driver,
inside the section for its kind:

```yaml
media:
  url: "rtpengine://10.0.0.5:22222"
  rtpengine:
    pool:
      - "10.0.0.5:22222"
      - "10.0.0.6:22222"
    health_check_interval: 5
```

`configure(own_root, system)` is called once, before `connect()`. `own_root` is that
section and is empty when there is none, so a driver that needs nothing else can ignore
it. `system` is the whole server configuration, for what a driver cannot be told twice:
the node id, the SIP timers. Returning `false` refuses the configuration and stops
startup, which is the right answer when a driver has been handed something that cannot
work.

The section is keyed on `name()`, not the scheme, so a driver reachable under several
schemes has one section rather than three.

## Async is the contract

`Datastore` operations do not return values. Every one of them takes the caller's
executor and a handler:

```cpp
void realm_get_by_name(plugins::Executor on, std::string realm_name,
                       plugins::Handler<std::shared_ptr<types::Realm>> handler);
void realm_create(plugins::Executor on, std::shared_ptr<types::Realm> realm,
                  plugins::StatusHandler handler);
```

This is not decoration. `Core` runs on a single strand, so a blocking read there stops
every call on the node rather than only the one that asked. It is version 1 of the
contract because it could not be added later: making a returning interface async breaks
every plugin written against it.

Two rules follow, and a driver that breaks either is broken:

- **The handler runs on `on`, the executor the caller passed.** Never on your own I/O
  thread. `Plugin::_complete` does this for you.
- **The handler is never called inline.** Even when you already know the answer, post
  it. `MemoryDatastore` is a few hash maps and could answer immediately; it posts
  anyway, because code written against the fast driver has to work against the slow one.

Results distinguish two things that are not the same:

```cpp
struct Status { bool ok; std::string error; };
template <typename T> struct Result { bool ok; std::string error; T value; };
```

A read that finds nothing **succeeds** with an empty value. A read that could not happen
**fails** with an error. "No such subscriber" is a 404 and "Redis is unreachable" is a
500, and a driver that reports them the same way makes that distinction impossible
upstream.

With no blocking primitive left, an operation that needs several round trips chains
them: each step starts the next from its own completion. `RedisDatastore::realm_create`
is `EXISTS`, then `SET`, then `SADD`, written as three nested handlers, and
`location_list` walks the index one key at a time. It reads longer than the blocking
version did. That is what the blocking version was hiding.

## Lifecycle

```
construct(logger, url) -> configure(own_root, system) -> connect(on, handler) -> ... -> close()
```

`connect()` is a round trip and is async. `close()` is teardown: synchronous, and safe
to call twice. `health()` reports what the driver already knows and does not go and ask
the far end.

Startup waits for `connect()` on the main thread, which is fine: main is not the Core
strand. Nothing else in the server ever waits.

## Versioning

`API_VERSION` in `src/plugins/plugin.h` is the contract version. Every plugin reports
the version it was built against, and the registry refuses to construct one that does
not match:

```
Plugin acme 1.0.0 was built against contract version 1, this server speaks 2
```

For a plugin compiled into the server that check can never fail. It exists for what
comes next: plugins loaded from shared libraries, where a mismatch is a crash rather
than a warning. The version is bumped whenever `Plugin`, or any interface derived from
it, changes shape.

## Testing a plugin

Tests are GoogleTest under `tests/`, mirroring `src/`. `tests/plugins/plugin_registry_test.cpp`
covers the registry itself.

A test is not on the Core strand, so it may wait where production code may not.
`tests/helpers/sync_datastore_helper.h` is a blocking view of a datastore for exactly
that: it turns each async call back into a return value so an assertion can be a
statement about what the store holds. Anything testing the async behaviour itself -
that the handler runs on the executor it was given, that a failure is reported rather
than swallowed - calls the driver directly instead.

## What is not here yet

- **`MediaEngine` is still synchronous.** It moves to the same async shape before the
  contract is declared stable; rtpengine is a network round trip and cannot stay on the
  strand either.
- **Plugins are compiled in.** Shared-library loading with `plugins.path`, an
  `extern "C"` entry point and `athenasip plugins list` is Milestone 5. The contract
  here is the one that loader will use, which is why it is worth getting right now.
