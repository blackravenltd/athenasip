# Contributing to AthenaSIP

## Branches

- `main` is the last stable state. Nothing is committed to it directly.
- `develop` is where work happens. Branch from it, merge back into it.
- Feature work that needs isolation branches off `develop` as `feature/<short-name>`
  and merges back into `develop`.
- Stable states reach `main` by merging `develop` into it.
- There is no `master`. If one ever appears, rename it to `main`.

## Tags

- Tags are exactly `x.y.z` semver: annotated, no `v` prefix, no suffix.
- Tags are cut from `main`.
- The tag, the CMake `project(... VERSION ...)` and what the binary reports are the
  same number. `src/build_version.h.in` is configured by CMake into the build tree, so
  the binary cannot drift from the build system.

A release is: bump the version in `CMakeLists.txt`, merge `develop` into `main`, tag
`x.y.z` on `main`.

## Commits

- Subjects are short imperative prose. No emoji, no ticket prefixes.
- Keep unrelated changes in separate commits: a rename, a bug fix and a feature are
  three commits, not one.
- A commit message ends with its last line of prose. No trailers announcing the tool
  that wrote it.

## Building

```
cmake --preset debug && cmake --build build -j8
```

Presets are defined in `CMakePresets.json`:

| Preset | Build dir | What it is |
| --- | --- | --- |
| `debug` | `build` | Debug, the default for local work |
| `release` | `build-release` | Release |
| `tests` | `build-tests` | Debug with `ATHENA_BUILD_TESTING=ON` |
| `asan` | `build-asan` | AddressSanitizer and UndefinedBehaviourSanitizer |
| `tsan` | `build-tsan` | ThreadSanitizer |

## Testing

```
cmake --preset tests && cmake --build build-tests -j8
./build-tests/athenasip_tests
```

Tests are GoogleTest under `tests/`, mirroring the `src/` layout. Run the binary
directly rather than through ctest where you can: ctest starts a process per test and
takes about eleven minutes to do what the binary does in eleven seconds. `ctest
--preset tests` is still there for a per-test report.

Two suites skip themselves unless pointed at something real, so a machine without them
still runs green and a machine with them tests the canonical drivers:

```
redis-server --port 6399 --save '' --daemonize yes
mosquitto -p 1883 -d

ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399 \
ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1883 ./build-tests/athenasip_tests
```

Run the `asan` and `tsan` presets before anything that touches the transaction, channel
or media paths.

### End to end

```
test/e2e/run.sh                 the sipp scenarios, in-process relay
test/e2e/run.sh --rtpengine     the same, with a real rtpengine on the media path
```

Principle 2 says compliance is proven rather than asserted, and this is where that
happens. The rtpengine run also asserts that the engine relayed the media rather than
declining it, which the built-in run cannot: a declined description travels on untouched
and the two ends reach each other directly, so the call completes either way.

### Interop

```
test/interop/up.sh              a node for a real SIP client to be pointed at
test/interop/smoke.py           registers on UDP, TCP, TLS and WS
```

Run the smoke test before blaming a client. If it passes and the client does not, the
difference is in the client; if it fails, the fixture is not serving what it claims and
nothing else is worth debugging. `test/interop/README.md` has the rest.

## Style

- Formatting is `.clang-format` (Google base, 160 columns), applied by `clang-format -i`
  on the files you changed. Two-space indent, `#pragma once`, braces on the same line.
- `snake_case` for functions, methods, variables and files. `PascalCase` for types.
  Private and protected members are prefixed with `_`. Namespaces are
  `athenasip::<subsystem>`.
- Every source file starts with the AthenaSIP header block: name, copyright, GPLv3 link.
  Copy it from any existing file.
- No emoji anywhere: code, comments, logs, docs, commit messages.
- Dependency injection over globals. Components take their collaborators in their
  constructors. The only process global is `detail::get_global_io_context()`.
- Comments are sparse and explain intent, not mechanics.

## Plan

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Move items across
as they land and keep the `file:line` references current.
