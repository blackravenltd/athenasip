# Contributing to AthenaSIP

## Branches

- `main` is the last stable state. Nothing is committed to it directly; stable states
  reach it by merging `develop`.
- `develop` is where work happens.
- Work that needs isolation branches off `develop` as `feature/<short-name>` and merges
  back into `develop`.

## Tags and releases

- Tags are exactly `x.y.z` semver: annotated, no `v` prefix, no suffix, cut from `main`.
- The tag, the CMake `project(... VERSION ...)` and `athenasip --version` are the same
  number. CMake configures `src/build_version.h.in`, so the binary follows the build.

A release is: bump the version in `CMakeLists.txt`, merge `develop` into `main`, tag
`x.y.z` on `main`.

## Commits

- Subjects are short imperative prose. No emoji, no ticket prefixes.
- Keep unrelated changes in separate commits: a rename, a bug fix and a feature are
  three commits.
- A commit message ends with its last line of prose. No tool attribution trailers.

## Building and testing

[docs/compiling.md](docs/compiling.md) has the dependencies and presets.
[docs/testing.md](docs/testing.md) has every test layer. The short form:

```
cmake --preset tests && cmake --build build-tests -j8
./build-tests/athenasip_tests
```

Run the test binary, not `ctest`. Everything runs on a developer machine; there is no
hosted CI.

Write tests from the RFC, not from the current behaviour: write the test for what the
standard requires, watch it fail, then fix the code.

## Dependencies

Boost, OpenSSL, yaml-cpp and nghttp2 (only because APNs speaks nothing but HTTP/2), plus
GoogleTest for the test build. Prefer writing
something by hand over adding a library.

## Style

- Formatting is `.clang-format` (Google base, 160 columns): run `clang-format -i` on the
  files you changed. Two-space indent, `#pragma once`, braces on the same line.
- `snake_case` for functions, methods, variables and files; `PascalCase` for types;
  private and protected members prefixed with `_`. Namespaces are
  `athenasip::<subsystem>`.
- Every source file starts with the AthenaSIP header block. Copy it from an existing file.
- No emoji anywhere: code, comments, logs, docs, commit messages.
- Dependency injection over globals: components take their collaborators in their
  constructors. The only process global is `detail::get_global_io_context()`.
- Each component logs through its own `LoggerScoped`.
- Pluggable subsystems register through `register_driver<T>(logger, scheme)` and
  `create_driver(logger, url)`, or from a shared library through
  `ATHENASIP_PLUGIN_MODULE`; see [docs/plugins.md](docs/plugins.md).
- Comments are sparse and explain intent, not mechanics.

## Plan

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Move items across
as they land.
