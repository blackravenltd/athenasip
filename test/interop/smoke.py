#!/usr/bin/env python3
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
"""Register against the interop fixture on every transport it exposes.

This is the thing to run before blaming a client. It is a deliberately small,
dependency-free SIP client: one REGISTER, the 401 that answers it, the Digest that
answers that, and the 200 that ends it. If this passes and a real client does not, the
difference is in the client; if this fails, the fixture is not serving what it claims.

    test/interop/smoke.py
    test/interop/smoke.py --sip-port 15060 --ws-port 18088 --tls-port 15061

It speaks enough of RFC 3261 section 22.4 to authenticate and enough of RFC 7118 to
open a WebSocket, and nothing else. It is not a SIP stack.
"""

import argparse
import base64
import hashlib
import os
import socket
import ssl
import sys
import uuid

CRLF = "\r\n"


def digest_response(username, password, realm, nonce, method, uri, algorithm):
    """RFC 3261 22.4 and RFC 7616, for the two algorithms this node challenges with."""
    digest = hashlib.sha256 if algorithm.lower().startswith("sha-256") else hashlib.md5

    ha1 = digest(f"{username}:{realm}:{password}".encode()).hexdigest()
    ha2 = digest(f"{method}:{uri}".encode()).hexdigest()

    return digest(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()


def header(message, name):
    """The first value of a header, case-insensitively, or None."""
    for line in message.split(CRLF):
        if not line:
            break
        if line.lower().startswith(name.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return None


def parse_challenge(value):
    """The parameters of a WWW-Authenticate value, unquoted."""
    parameters = {}
    body = value.split(" ", 1)[1] if " " in value else value

    for part in body.split(","):
        if "=" not in part:
            continue
        key, raw = part.split("=", 1)
        parameters[key.strip().lower()] = raw.strip().strip('"')

    return parameters


def status_of(message):
    first = message.split(CRLF, 1)[0]
    parts = first.split(" ", 2)
    return int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0


def register_message(args, transport, branch, call_id, cseq, local, authorization=None):
    via_transport = {"udp": "UDP", "tcp": "TCP", "tls": "TLS", "ws": "WS"}[transport]
    contact_transport = "" if transport == "udp" else f";transport={transport}"

    lines = [
        f"REGISTER sip:{args.realm} SIP/2.0",
        f"Via: SIP/2.0/{via_transport} {local};branch={branch}",
        f"From: <sip:{args.user}@{args.realm}>;tag=smoke-{branch[-8:]}",
        f"To: <sip:{args.user}@{args.realm}>",
        f"Call-ID: {call_id}",
        f"CSeq: {cseq} REGISTER",
        f"Contact: <sip:{args.user}@{local}{contact_transport}>",
        "Max-Forwards: 70",
        "Expires: 60",
    ]

    if authorization:
        lines.append(f"Authorization: {authorization}")

    lines += ["Content-Length: 0", "", ""]
    return CRLF.join(lines)


def authorization_for(args, challenge, uri):
    algorithm = challenge.get("algorithm", "MD5")
    response = digest_response(
        args.user, args.password, challenge["realm"], challenge["nonce"], "REGISTER", uri, algorithm
    )

    return (
        f'Digest username="{args.user}", realm="{challenge["realm"]}", '
        f'nonce="{challenge["nonce"]}", uri="{uri}", response="{response}", algorithm={algorithm}'
    )


class Stream:
    """A SIP connection, whatever is underneath it."""

    def __init__(self, sock, transport):
        self._socket = sock
        self._transport = transport
        self._buffer = b""

    def local(self):
        host, port = self._socket.getsockname()[:2]
        return f"{host}:{port}"

    def send(self, text):
        self._socket.sendall(text.encode())

    def receive(self):
        """One SIP message. Datagram transports get one per read; streams are framed by
        the blank line, because nothing here has a body."""
        while True:
            if b"\r\n\r\n" in self._buffer:
                message, self._buffer = self._buffer.split(b"\r\n\r\n", 1)
                return message.decode(errors="replace") + CRLF + CRLF

            chunk = self._socket.recv(65535)
            if not chunk:
                raise ConnectionError("closed before a whole message arrived")

            self._buffer += chunk

    def close(self):
        try:
            self._socket.close()
        except OSError:
            pass


class WebSocketStream(Stream):
    """RFC 6455 framing, and RFC 7118's "sip" subprotocol. Client frames are masked."""

    def send(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))

        frame = bytearray([0x81])  # FIN, text
        if len(payload) < 126:
            frame.append(0x80 | len(payload))
        elif len(payload) < 65536:
            frame.append(0x80 | 126)
            frame += len(payload).to_bytes(2, "big")
        else:
            frame.append(0x80 | 127)
            frame += len(payload).to_bytes(8, "big")

        frame += mask + masked
        self._socket.sendall(bytes(frame))

    def receive(self):
        while True:
            head = self._read_exactly(2)
            length = head[1] & 0x7F

            if length == 126:
                length = int.from_bytes(self._read_exactly(2), "big")
            elif length == 127:
                length = int.from_bytes(self._read_exactly(8), "big")

            payload = self._read_exactly(length) if length else b""

            # A server frame is never masked, so there is no key to strip.
            if head[0] & 0x0F in (0x1, 0x2):
                return payload.decode(errors="replace")

    def _read_exactly(self, count):
        while len(self._buffer) < count:
            chunk = self._socket.recv(65535)
            if not chunk:
                raise ConnectionError("closed mid-frame")
            self._buffer += chunk

        taken, self._buffer = self._buffer[:count], self._buffer[count:]
        return taken


def connect(args, transport):
    if transport == "udp":
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(args.timeout)
        sock.connect((args.host, args.sip_port))
        return Stream(sock, transport)

    if transport == "tcp":
        sock = socket.create_connection((args.host, args.sip_port), args.timeout)
        sock.settimeout(args.timeout)
        return Stream(sock, transport)

    if transport == "tls":
        context = ssl.create_default_context(cafile=args.ca)
        # The fixture's certificate names 127.0.0.1, so the name checked is the address.
        raw = socket.create_connection((args.host, args.tls_port), args.timeout)
        raw.settimeout(args.timeout)
        return Stream(context.wrap_socket(raw, server_hostname=args.host), transport)

    if transport == "ws":
        sock = socket.create_connection((args.host, args.ws_port), args.timeout)
        sock.settimeout(args.timeout)

        key = base64.b64encode(os.urandom(16)).decode()
        request = CRLF.join(
            [
                f"GET {args.ws_path} HTTP/1.1",
                f"Host: {args.host}:{args.ws_port}",
                "Upgrade: websocket",
                "Connection: Upgrade",
                f"Sec-WebSocket-Key: {key}",
                "Sec-WebSocket-Version: 13",
                "Sec-WebSocket-Protocol: sip",
                "",
                "",
            ]
        )
        sock.sendall(request.encode())

        stream = WebSocketStream(sock, transport)
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = sock.recv(65535)
            if not chunk:
                raise ConnectionError("closed during the upgrade")
            response += chunk

        head, rest = response.split(b"\r\n\r\n", 1)
        stream._buffer = rest

        text = head.decode(errors="replace")
        if "101" not in text.split(CRLF, 1)[0]:
            raise ConnectionError("the upgrade was refused: " + text.split(CRLF, 1)[0])

        # RFC 7118 section 4: a server that does not name the subprotocol back is a
        # server a client is entitled to conclude does not speak SIP.
        if "sip" not in (header(text, "Sec-WebSocket-Protocol") or ""):
            raise ConnectionError("the server did not name the sip subprotocol")

        return stream

    raise ValueError(transport)


def register(args, transport):
    """Returns the final status code, or raises."""
    stream = connect(args, transport)

    try:
        call_id = f"smoke-{uuid.uuid4().hex[:12]}"
        uri = f"sip:{args.realm}"
        local = stream.local()

        stream.send(register_message(args, transport, f"z9hG4bK{uuid.uuid4().hex[:12]}", call_id, 1, local))
        challenged = stream.receive()

        if status_of(challenged) != 401:
            return status_of(challenged)

        value = header(challenged, "WWW-Authenticate")
        if not value:
            raise ConnectionError("401 with no challenge in it")

        authorization = authorization_for(args, parse_challenge(value), uri)

        stream.send(
            register_message(args, transport, f"z9hG4bK{uuid.uuid4().hex[:12]}", call_id, 2, local, authorization)
        )

        return status_of(stream.receive())
    finally:
        stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--sip-port", type=int, default=5060)
    parser.add_argument("--tls-port", type=int, default=5061)
    parser.add_argument("--ws-port", type=int, default=8088)
    parser.add_argument("--ws-path", default="/ws")
    parser.add_argument("--user", default="1001")
    parser.add_argument("--password", default="athenaphone")
    # The realm is named for the address a client dialled, so it follows the host unless
    # it is given. A fixture off loopback - rtpengine's, which advertises this machine's
    # own address - would otherwise be registered against the wrong realm.
    parser.add_argument("--realm", default=None)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument(
        "--ca",
        default=os.path.join(os.path.dirname(__file__), "..", "..", "tls", "ca", "snakeca.crt"),
        help="the CA that vouches for the fixture's TLS certificate",
    )
    parser.add_argument("--transports", default="udp,tcp,tls,ws")

    args = parser.parse_args()

    if args.realm is None:
        args.realm = args.host

    failures = 0

    for transport in args.transports.split(","):
        transport = transport.strip()
        if not transport:
            continue

        try:
            code = register(args, transport)
            ok = code == 200
            print(f"  {transport:<4} {'ok' if ok else 'FAILED'}  {code}")
            failures += 0 if ok else 1
        except Exception as error:  # noqa: BLE001 - the report is the point
            print(f"  {transport:<4} FAILED  {type(error).__name__}: {error}")
            failures += 1

    if failures:
        print(f"\n{failures} transport(s) did not register.")
        return 1

    print("\nEvery transport registered.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
