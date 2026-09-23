#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Regenerates the snakeoil CA and server certificate in this directory.
#
#   tls/generate.sh                                     the CA and the server certificate
#   tls/generate.sh --server-only --out DIR 10.0.0.5    a server certificate for an
#                                                       address the checked-in one does
#                                                       not name, signed by the same CA
#
# The second form is for a fixture that has moved off loopback: a certificate that does
# not name what the client dialled fails verification however good the chain is, and the
# CA stays put so a client already trusting tls/ca/snakeca.crt needs no second CA.
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
OUT_DIR="$HERE"
SERVER_ONLY="no"
EXTRA_NAMES=()

while (( $# > 0 )); do
  case "$1" in
    --server-only) SERVER_ONLY="yes" ;;
    --out) shift; OUT_DIR="$1" ;;
    -*) echo "Unknown option: $1" >&2; exit 1 ;;
    *) EXTRA_NAMES+=("$1") ;;
  esac
  shift
done

DAYS_CA="${DAYS_CA:-3650}"
DAYS_CERT="${DAYS_CERT:-3650}"
SUBJECT_BASE="/C=NZ/ST=Wellington/L=Wellington/O=AthenaSIP"

mkdir -p "$CA_DIR" "$OUT_DIR"

if [[ "$SERVER_ONLY" == "no" ]]; then
  echo "Certificate authority..."
  openssl req -x509 -newkey rsa:4096 -sha256 -days "$DAYS_CA" -nodes \
    -keyout "$CA_DIR/snakeca.key" -out "$CA_DIR/snakeca.crt" \
    -subj "${SUBJECT_BASE}/OU=CA/CN=AthenaSIP Snakeoil CA" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash" 2>/dev/null
fi

echo "Server key and request..."
openssl req -newkey rsa:4096 -sha256 -nodes \
  -keyout "$OUT_DIR/snakeoil.key" -out "$OUT_DIR/snakeoil.csr" \
  -subj "${SUBJECT_BASE}/CN=localhost" 2>/dev/null

# Every name a local test reaches this node by, plus anything asked for on the command
# line. A certificate that does not name what the client dialled fails verification
# however good the chain is, and a fixture published on this machine's own address is
# dialled by that address.
names="DNS:localhost, DNS:athenasip.org, DNS:sip.athenasip.org, IP:127.0.0.1, IP:0:0:0:0:0:0:0:1"

for name in ${EXTRA_NAMES+"${EXTRA_NAMES[@]}"}; do
  # An address goes in as IP: and anything else as DNS:, because a verifier matches the
  # two against different fields and will not take one for the other.
  if [[ "$name" =~ ^[0-9]+(\.[0-9]+){3}$ || "$name" == *:* ]]; then
    names="${names}, IP:${name}"
  else
    names="${names}, DNS:${name}"
  fi
done

cat > "$OUT_DIR/snakeoil.v3.ext" <<EXT
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
subjectAltName = ${names}
EXT

echo "Signing..."

# The CA's serial file is a checked-in artefact, and a fixture signing a certificate for
# its own address must not rewrite it every time it starts. A random serial is unique
# without a file to keep it in, which is all a snakeoil CA with no revocation needs.
if [[ "$SERVER_ONLY" == "yes" ]]; then
  serial=(-set_serial "0x$(openssl rand -hex 8)")
else
  serial=(-CAcreateserial)
fi

openssl x509 -req -in "$OUT_DIR/snakeoil.csr" -sha256 -days "$DAYS_CERT" \
  -CA "$CA_DIR/snakeca.crt" -CAkey "$CA_DIR/snakeca.key" "${serial[@]}" \
  -extfile "$OUT_DIR/snakeoil.v3.ext" -out "$OUT_DIR/snakeoil.cer" 2>/dev/null

echo
openssl verify -CAfile "$CA_DIR/snakeca.crt" "$OUT_DIR/snakeoil.cer"
openssl x509 -in "$OUT_DIR/snakeoil.cer" -noout -dates -ext subjectAltName
