#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One node, built from this tree. docker-compose.test.yml uses it for the sipp harness.

FROM debian:trixie-slim AS build

ARG BOOST_VERSION=1.89.0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        liblua5.4-dev \
        libnghttp2-dev \
        libssl-dev \
        libyaml-cpp-dev \
        pkg-config \
        wget \
    && rm -rf /var/lib/apt/lists/*

# Debian's Boost is older than the 1.87 that boost::redis and boost::mqtt5 need, so it
# is built here: only the compiled libraries the server links, plus the headers. Its own
# layer, because it is slow and changes only with BOOST_VERSION.
RUN set -eux; \
    version_underscored="$(echo "${BOOST_VERSION}" | tr '.' '_')"; \
    wget -q "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${version_underscored}.tar.gz"; \
    tar xzf "boost_${version_underscored}.tar.gz"; \
    cd "boost_${version_underscored}"; \
    ./bootstrap.sh --with-libraries=thread,json,charconv; \
    ./b2 -j"$(nproc)" --with-thread --with-json --with-charconv install; \
    cd ..; \
    rm -rf "boost_${version_underscored}" "boost_${version_underscored}.tar.gz"

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY scripts ./scripts

# CMake needs packaging/ and config/ at configure time for its install rules, though
# the image copies the binary out rather than installing.
COPY packaging ./packaging
COPY config ./config

# One compiler per 1.5 GB of available memory, at most one per core: a release build of the
# HTTPS and JSON code takes about a gigabyte per job, and a Docker VM is often 8 GB.
RUN jobs=$(awk '/MemAvailable/ { print int($2 / 1500000) }' /proc/meminfo); \
    [ "${jobs}" -ge 1 ] || jobs=1; \
    [ "${jobs}" -le "$(nproc)" ] || jobs=$(nproc); \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"${jobs}"

FROM debian:trixie-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        liblua5.4-0 \
        libnghttp2-14 \
        libssl3 \
        libyaml-cpp0.8 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/lib/libboost_*.so* /usr/local/lib/
RUN ldconfig

COPY --from=build /src/build/athenasip /usr/local/bin/athenasip

# The server's configuration directory. Mount one over this.
RUN mkdir -p /root/.athenasip
COPY config/config.example.yaml /root/.athenasip/config.yaml

# SIP over UDP and TCP, WebSocket, and the admin API.
EXPOSE 5060/udp 5060/tcp 9500/tcp 8080/tcp

# The RTP range the builtin relay allocates from.
EXPOSE 22000-23000/udp

ENTRYPOINT ["/usr/local/bin/athenasip"]
