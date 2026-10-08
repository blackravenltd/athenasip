# AthenaSIP - Compiling

## Dependencies

CMake 3.23 or newer, a C++20 compiler, Boost 1.87 or newer (`thread`, `json` and
`charconv` are linked; the rest is header-only), OpenSSL, yaml-cpp, nghttp2, which
APNs needs because it speaks only HTTP/2, and Lua 5.4, for [scripting](scripting.md).
CMake finds nghttp2 through pkg-config.
GoogleTest is needed only for the test build.

macOS:

```
brew install cmake pkg-config boost openssl@3 yaml-cpp libnghttp2 lua@5.4 googletest
```

Debian and Ubuntu:

```
apt install build-essential cmake pkg-config libssl-dev libyaml-cpp-dev libnghttp2-dev liblua5.4-dev libgtest-dev
```

Where the distribution's Boost is older than 1.87, build Boost from source; `Dockerfile`
shows how.

## Build

```
cmake --preset debug
cmake --build build -j8
./build/athenasip --version
```

Presets are in `CMakePresets.json`:

| Preset | Build dir | What it is |
| --- | --- | --- |
| `debug` | `build` | Debug, for local work |
| `release` | `build-release` | Release |
| `tests` | `build-tests` | Debug with `ATHENA_BUILD_TESTING=ON` |
| `asan` | `build-asan` | Tests with AddressSanitizer and UndefinedBehaviourSanitizer |
| `tsan` | `build-tsan` | Tests with ThreadSanitizer |

[testing.md](testing.md) covers running the tests and when the sanitizer presets are
used. [installation.md](installation.md) covers installing a release build.

## Docker

```
docker build -t athenasip .
```

The image builds Boost from source and runs one node with `config/config.example.yaml`.
Each release is published as [`tomcully/athenasip`](https://hub.docker.com/r/tomcully/athenasip),
tagged with its version and `latest`, for `linux/amd64` and `linux/arm64`.
