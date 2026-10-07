#!/usr/bin/env python3
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
"""A callee that is only reachable down the connection it registered over.

    flow_callee.py --host 172.32.0.11 --transport tcp --user bob --password bob-secret
    flow_callee.py --host 172.32.0.11 --transport ws  --user bob --password bob-secret

It registers over TCP or a WebSocket, keeps that connection, and answers one call on it:
200 to the INVITE, then it waits for the ACK and the BYE. Nothing listens at its Contact,
so a call reaches it down the flow or not at all, like a browser or a phone behind NAT.
sipp cannot do this: it registers in one run and listens in another.

Exit status 0 when the whole call was seen, 1 otherwise. Standard library only; the
transports are test/interop/smoke.py's.
"""

import argparse
import os
import sys
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "interop"))

from smoke import CRLF, Stream, authorization_for, connect, header, parse_challenge, register_message, status_of  # noqa: E402


class BodyStream(Stream):
    """Reads the body Content-Length declares; smoke.py's Stream frames by the blank line."""

    def receive(self):
        while True:
            # A CRLF keep-alive answer (RFC 5626 4.4.1) is not the start of a message.
            self._buffer = self._buffer.lstrip(b"\r\n")

            if b"\r\n\r\n" in self._buffer:
                head, rest = self._buffer.split(b"\r\n\r\n", 1)
                text = head.decode(errors="replace") + CRLF + CRLF
                length = int(header(text, "Content-Length") or header(text, "l") or 0)

                if len(rest) >= length:
                    self._buffer = rest[length:]
                    return text + rest[:length].decode(errors="replace")

            chunk = self._socket.recv(65535)
            if not chunk:
                raise ConnectionError("closed before a whole message arrived")
            self._buffer += chunk


def headers_named(message, name):
    """Every value of a header, in order."""
    values = []
    for line in message.split(CRLF):
        if not line:
            break
        if line.lower().startswith(name.lower() + ":"):
            values.append(line.split(":", 1)[1].strip())
    return values


def method_of(message):
    first = message.split(CRLF, 1)[0]
    return "" if first.startswith("SIP/2.0") else first.split(" ", 1)[0]


def response_to(request, code, reason, contact=None, body=""):
    lines = [f"SIP/2.0 {code} {reason}"]
    lines += [f"Via: {value}" for value in headers_named(request, "Via")]
    lines += [f"Record-Route: {value}" for value in headers_named(request, "Record-Route")]
    lines.append(f"From: {header(request, 'From')}")

    to = header(request, "To")
    lines.append(f"To: {to}" if "tag=" in to else f"To: {to};tag=flow-callee")

    lines.append(f"Call-ID: {header(request, 'Call-ID')}")
    lines.append(f"CSeq: {header(request, 'CSeq')}")
    if contact:
        lines.append(f"Contact: <{contact}>")
    if body:
        lines.append("Content-Type: application/sdp")
    lines += [f"Content-Length: {len(body)}", "", body]
    return CRLF.join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--transport", choices=["tcp", "ws"], required=True)
    parser.add_argument("--realm", default="example.com")
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--sip-port", type=int, default=5060)
    parser.add_argument("--ws-port", type=int, default=9500)
    parser.add_argument("--ws-path", default="/")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()

    stream = connect(args, args.transport)
    if args.transport == "tcp":
        stream = BodyStream(stream._socket, "tcp")

    # A WebSocket client's Contact names nothing reachable, on purpose (RFC 7118 section 5).
    local = stream.local() if args.transport == "tcp" else f"{uuid.uuid4().hex[:12]}.invalid"
    contact = f"sip:{args.user}@{local};transport={args.transport}"

    call_id = f"flow-callee-{uuid.uuid4().hex[:12]}"
    uri = f"sip:{args.realm}"

    stream.send(register_message(args, args.transport, f"z9hG4bK{uuid.uuid4().hex[:12]}", call_id, 1, local))
    challenged = stream.receive()
    if status_of(challenged) != 401:
        print(f"REGISTER was answered {status_of(challenged)}, not challenged", flush=True)
        return 1

    authorization = authorization_for(args, parse_challenge(header(challenged, "WWW-Authenticate")), uri)
    stream.send(register_message(args, args.transport, f"z9hG4bK{uuid.uuid4().hex[:12]}", call_id, 2, local, authorization))

    registered = stream.receive()
    if status_of(registered) != 200:
        print(f"REGISTER was refused: {status_of(registered)}", flush=True)
        return 1

    print(f"registered over {args.transport} from {stream.local()}", flush=True)

    seen = []
    while True:
        message = stream.receive()
        method = method_of(message)
        if not method:
            continue

        seen.append(method)
        print(f"< {message.split(CRLF, 1)[0]}", flush=True)

        if method == "INVITE":
            # Answer in kind when there was an offer. No media is sent: this tests signalling.
            host = stream.local().rsplit(":", 1)[0]
            answer = ""
            if "application/sdp" in (header(message, "Content-Type") or ""):
                answer = CRLF.join(
                    ["v=0", f"o=bob 1 1 IN IP4 {host}", "s=-", f"c=IN IP4 {host}", "t=0 0", "m=audio 40000 RTP/AVP 8", "a=rtpmap:8 PCMA/8000", ""]
                )
            stream.send(response_to(message, 200, "OK", contact, answer))
        elif method == "BYE":
            stream.send(response_to(message, 200, "OK"))
            break
        elif method not in ("ACK", "CANCEL"):
            stream.send(response_to(message, 200, "OK"))

    stream.close()

    if seen[:1] == ["INVITE"] and "ACK" in seen and seen[-1] == "BYE":
        print("the call arrived down the flow: INVITE, ACK, BYE", flush=True)
        return 0

    print(f"not the call that was expected: {seen}", flush=True)
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - whatever it was, the harness wants to read it
        print(f"failed: {error}", flush=True)
        sys.exit(1)
