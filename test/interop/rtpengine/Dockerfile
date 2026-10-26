#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# rtpengine from Debian's own package, for a host the published drachtio/rtpengine image does not run on: it is
# amd64 only, and Debian builds rtpengine for arm64 too. Userspace forwarding only; no kernel module.
#
#   ATHENA_INTEROP_RTPENGINE_IMAGE=athenasip-rtpengine test/interop/up.sh --rtpengine
#
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends rtpengine-daemon iproute2 \
    && rm -rf /var/lib/apt/lists/*
ENTRYPOINT []
