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
ctest --preset tests
```

Tests are GoogleTest under `tests/`, mirroring the `src/` layout. Run the `asan` and
`tsan` presets before anything that touches the transaction, channel or media paths.

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
