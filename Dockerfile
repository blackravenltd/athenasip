#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One node, built from this tree. Used by docker-compose.test.yml for the sipp harness
# and by anyone who wants to run a node without installing a toolchain.

FROM debian:trixie-slim AS build

ARG BOOST_VERSION=1.89.0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libssl-dev \
        libyaml-cpp-dev \
        wget \
    && rm -rf /var/lib/apt/lists/*

# Debian ships Boost 1.83 and this needs 1.87 or newer for boost::redis and
# boost::mqtt5, so Boost is built here. Only the three compiled libraries the server
# links; everything else it uses - asio, beast, json's header-only mode - comes from the
# headers this installs alongside them. Its own layer, because it is the slow one and it
# changes only when BOOST_VERSION does.
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

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"

FROM debian:trixie-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libssl3 \
        libyaml-cpp0.8 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/lib/libboost_*.so* /usr/local/lib/
RUN ldconfig

COPY --from=build /src/build/athenasip /usr/local/bin/athenasip

# Where the server looks for its configuration. Mount one over this.
RUN mkdir -p /root/.athenasip
COPY config/config.example.yaml /root/.athenasip/config.yaml

# SIP over UDP and TCP, WebSocket, and the admin API.
EXPOSE 5060/udp 5060/tcp 9500/tcp 8080/tcp

# The RTP range the builtin relay allocates from.
EXPOSE 22000-23000/udp

ENTRYPOINT ["/usr/local/bin/athenasip"]
