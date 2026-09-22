#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Regenerates the snakeoil CA and server certificate in this directory.
#
#   tls/generate.sh
#
# These exist so that TLS and WSS can be tried without a certificate authority. They
# are not for production: the private keys are in the repository, so anyone can be this
# node.
#
# What they are not is sloppy. A client that verifies properly has to be able to verify
# these, or "try TLS" means "turn verification off", and the first thing anybody learns
# about the project's TLS is how to ignore it. The CA carries keyUsage with keyCertSign
# (RFC 5280 section 4.2.1.3 requires it to be asserted where the extension is present,
# and strict verifiers refuse a CA without it), and the server certificate carries
# extendedKeyUsage serverAuth and names every address a local test reaches it on.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CA_DIR="$HERE/ca"

DAYS_CA="${DAYS_CA:-3650}"
DAYS_CERT="${DAYS_CERT:-3650}"
SUBJECT_BASE="/C=NZ/ST=Wellington/L=Wellington/O=AthenaSIP"

mkdir -p "$CA_DIR"

echo "Certificate authority..."
openssl req -x509 -newkey rsa:4096 -sha256 -days "$DAYS_CA" -nodes \
  -keyout "$CA_DIR/snakeca.key" -out "$CA_DIR/snakeca.crt" \
  -subj "${SUBJECT_BASE}/OU=CA/CN=AthenaSIP Snakeoil CA" \
  -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -addext "subjectKeyIdentifier=hash" 2>/dev/null

echo "Server key and request..."
openssl req -newkey rsa:4096 -sha256 -nodes \
  -keyout "$HERE/snakeoil.key" -out "$HERE/snakeoil.csr" \
  -subj "${SUBJECT_BASE}/CN=localhost" 2>/dev/null

# Every name a local test reaches this node by. A certificate that does not name what
# the client dialled fails verification however good the chain is.
cat > "$HERE/snakeoil.v3.ext" <<EXT
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
subjectAltName = DNS:localhost, DNS:athenasip.org, DNS:sip.athenasip.org, IP:127.0.0.1, IP:0:0:0:0:0:0:0:1
EXT

echo "Signing..."
openssl x509 -req -in "$HERE/snakeoil.csr" -sha256 -days "$DAYS_CERT" \
  -CA "$CA_DIR/snakeca.crt" -CAkey "$CA_DIR/snakeca.key" -CAcreateserial \
  -extfile "$HERE/snakeoil.v3.ext" -out "$HERE/snakeoil.cer" 2>/dev/null

echo
openssl verify -CAfile "$CA_DIR/snakeca.crt" "$HERE/snakeoil.cer"
openssl x509 -in "$HERE/snakeoil.cer" -noout -dates -ext subjectAltName
