# AthenaSIP - Compiling

## Linux - apt

```
apt install cmake build-essential
```

## MacOSX

* Brew Dependencies:

```
brew install cmake boost openssl@3 lua yaml-cpp tinyxml2 googletest
```

* Build

```
cmake --preset debug
cmake --build build -j8
```

Presets are listed in `CONTRIBUTING.md`.
