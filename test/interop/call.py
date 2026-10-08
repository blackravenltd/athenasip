#!/usr/bin/env python3
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
"""One whole call between two subscribers, over any transport, against live nodes.

The callee registers and answers; the caller dials, answers the node's challenge, ACKs the
2xx through the Record-Route it was given, and hangs up. Each end checks what RFC 3261
asks of what it receives: the dialog's identifiers, the CSeq of its own requests coming
back in its own space, and the route. Like smoke.py it is small and dependency-free, and
not a SIP stack: no media flows, though the SDP is a real offer.

    test/interop/call.py --caller 1001@10.35.1.20 --callee 1002@10.35.1.20 --password athenaphone

Caller and callee may be on different nodes, and the number dialled need not be the
callee's, which is how a call through a trunk is tested:

    test/interop/call.py --caller 1001@10.35.1.20 --callee 1002@10.44.1.50 \\
        --callee-realm macnessa.athenasip.org --dial sip:+447700900123@10.35.1.20
"""

import argparse
import base64
import hashlib
import os
import queue
import random
import socket
import ssl
import string
import sys
import threading
import time

CRLF = "\r\n"

# Where each transport listens on the nodes, as their configurations have it.
PORTS = {"udp": 5060, "tcp": 5060, "tls": 5061, "ws": 8088, "wss": 8089}
CA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tls", "ca", "snakeca.crt")


def token(length=10):
    return "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(length))


def digest(algorithm, text):
    return (hashlib.sha256 if algorithm.lower().startswith("sha-256") else hashlib.md5)(text.encode()).hexdigest()


class Message:
    """A parsed SIP message: the start line and every header value, in order."""

    def __init__(self, text):
        head, _, self.body = text.partition(CRLF + CRLF)
        lines = head.split(CRLF)
        self.start = lines[0]
        self.headers = []
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                self.headers.append((name.strip().lower(), value.strip()))

    def all(self, name):
        name = name.lower()
        out = []
        for key, value in self.headers:
            if key == name:
                # Record-Route and Via may carry several values in one line.
                out += [part.strip() for part in split_values(value)] if name in ("record-route", "via", "route") else [value]
        return out

    def get(self, name):
        values = self.all(name)
        return values[0] if values else None

    @property
    def is_response(self):
        return self.start.startswith("SIP/2.0")

    @property
    def code(self):
        return int(self.start.split(" ", 2)[1]) if self.is_response else 0

    @property
    def method(self):
        return self.start.split(" ", 1)[0] if not self.is_response else self.get("cseq").split()[1]

    @property
    def cseq(self):
        return int(self.get("cseq").split()[0])


def split_values(value):
    """A header line's comma-separated values, not splitting inside <...> or quotes."""
    parts, depth, quoted, current = [], 0, False, ""
    for c in value:
        if c == '"':
            quoted = not quoted
        elif not quoted and c == "<":
            depth += 1
        elif not quoted and c == ">":
            depth -= 1
        if c == "," and depth == 0 and not quoted:
            parts.append(current)
            current = ""
        else:
            current += c
    parts.append(current)
    return parts


def challenge_of(value):
    parameters = {}
    body = value.split(" ", 1)[1] if " " in value else value
    for part in split_values(body):
        if "=" in part:
            key, raw = part.split("=", 1)
            parameters[key.strip().lower()] = raw.strip().strip('"')
    return parameters


class Connection:
    """One flow to a node: a UDP socket, or a TCP, TLS, WS or WSS connection read by a thread. Messages arrive on
    `inbox` whole: a stream is framed by Content-Length (RFC 3261 18.3), a WebSocket by its frames (RFC 7118)."""

    def __init__(self, transport, node, local_ip, inbox):
        self.transport, self.node, self.inbox = transport, node, inbox
        self.running = True
        port = PORTS[transport]

        if transport == "udp":
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.socket.bind((local_ip, 0))
        else:
            raw = socket.create_connection((node, port), 5)
            if transport in ("tls", "wss"):
                context = ssl.create_default_context(cafile=CA)
                raw = context.wrap_socket(raw, server_hostname=node)
            self.socket = raw
            if transport in ("ws", "wss"):
                self._upgrade(port)
        self.socket.settimeout(0.2)
        host, local_port = self.socket.getsockname()[:2]
        self.local = f"{host}:{local_port}"
        self.buffer = b""
        threading.Thread(target=self._read, daemon=True).start()

    def _upgrade(self, port):
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall(CRLF.join([f"GET /ws HTTP/1.1", f"Host: {self.node}:{port}", "Upgrade: websocket", "Connection: Upgrade",
                                       f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13", "Sec-WebSocket-Protocol: sip", "", ""]).encode())
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.socket.recv(65535)
            if not chunk:
                raise ConnectionError("closed during the WebSocket upgrade")
            response += chunk
        head, self.pending = response.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ConnectionError("the WebSocket upgrade was refused")

    def send(self, text, to=None):
        data = text.encode()
        if self.transport == "udp":
            self.socket.sendto(data, to or (self.node, PORTS["udp"]))
        elif self.transport in ("ws", "wss"):
            mask = os.urandom(4)
            frame = bytearray([0x81])
            if len(data) < 126:
                frame.append(0x80 | len(data))
            else:
                frame.append(0x80 | 126)
                frame += len(data).to_bytes(2, "big")
            frame += mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            self.socket.sendall(bytes(frame))
        else:
            self.socket.sendall(data)

    def _read(self):
        if self.transport in ("ws", "wss"):
            self.buffer = getattr(self, "pending", b"")
        while self.running:
            try:
                if self.transport == "udp":
                    data, source = self.socket.recvfrom(65535)
                    if data.strip():
                        self.inbox.put((Message(data.decode(errors="replace")), source))
                    continue
                chunk = self.socket.recv(65535)
                if not chunk:
                    return
                self.buffer += chunk
                self._frame()
            except (socket.timeout, ssl.SSLWantReadError):
                continue
            except OSError:
                return

    def _frame(self):
        while True:
            if self.transport in ("ws", "wss"):
                if len(self.buffer) < 2:
                    return
                length, offset = self.buffer[1] & 0x7F, 2
                if length == 126:
                    if len(self.buffer) < 4:
                        return
                    length, offset = int.from_bytes(self.buffer[2:4], "big"), 4
                elif length == 127:
                    if len(self.buffer) < 10:
                        return
                    length, offset = int.from_bytes(self.buffer[2:10], "big"), 10
                if len(self.buffer) < offset + length:
                    return
                opcode, payload = self.buffer[0] & 0x0F, self.buffer[offset:offset + length]
                self.buffer = self.buffer[offset + length:]
                if opcode == 1 and payload.strip():
                    self.inbox.put((Message(payload.decode(errors="replace")), None))
                continue

            # RFC 5626 4.4.1 keep-alives are CRLFs between messages.
            self.buffer = self.buffer.lstrip(b"\r\n")
            end = self.buffer.find(b"\r\n\r\n")
            if end < 0:
                return
            head = self.buffer[:end].decode(errors="replace")
            length = 0
            for line in head.split(CRLF):
                name, _, value = line.partition(":")
                if name.strip().lower() in ("content-length", "l"):
                    length = int(value.strip() or 0)
            total = end + 4 + length
            if len(self.buffer) < total:
                return
            text, self.buffer = self.buffer[:total].decode(errors="replace"), self.buffer[total:]
            self.inbox.put((Message(text), None))

    def close(self):
        self.running = False
        try:
            self.socket.close()
        except OSError:
            pass


class Agent:
    """One end: a flow to its node and what it has heard down it."""

    def __init__(self, name, user, node, realm, password, local_ip, transport="udp"):
        self.name, self.user, self.node, self.realm, self.password = name, user, node, realm, password
        self.transport = transport
        self.inbox = queue.Queue()
        self.connection = Connection(transport, node, local_ip, self.inbox)
        self.local = self.connection.local

    @property
    def via(self):
        return f"SIP/2.0/{self.transport.upper()} {self.local}"

    @property
    def contact(self):
        # RFC 7118 5: a WebSocket client's Contact names nothing reachable; the node uses the flow.
        if self.transport in ("ws", "wss"):
            return f"<sip:{self.user}@{token(8)}.invalid;transport={self.transport}>"
        return f"<sip:{self.user}@{self.local}" + ("" if self.transport == "udp" else f";transport={self.transport}") + ">"

    def send(self, text, to=None):
        self.connection.send(text, to)

    def wait(self, accept, timeout=10.0):
        """The next message accept() takes; others are answered or dropped as a UA would."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                message, source = self.inbox.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                break
            if accept(message):
                return message, source
            if not message.is_response and message.method == "OPTIONS":
                self.send(reply(message, 200, "OK", self.user), source)
        raise TimeoutError(f"{self.name}: nothing arrived in {timeout}s")

    def credentials(self, challenge, method, uri, header_value):
        algorithm = challenge.get("algorithm", "MD5")
        ha1 = digest(algorithm, f"{self.user}:{challenge['realm']}:{self.password}")
        ha2 = digest(algorithm, f"{method}:{uri}")
        if "qop" in challenge:
            nc, cnonce = "00000001", token(8)
            response = digest(algorithm, f"{ha1}:{challenge['nonce']}:{nc}:{cnonce}:auth:{ha2}")
            extra = f", qop=auth, nc={nc}, cnonce=\"{cnonce}\""
        else:
            response = digest(algorithm, f"{ha1}:{challenge['nonce']}:{ha2}")
            extra = ""
        return (f'Digest username="{self.user}", realm="{challenge["realm"]}", nonce="{challenge["nonce"]}", '
                f'uri="{uri}", response="{response}", algorithm={algorithm}{extra}')

    def register(self):
        call_id, tag = token(16), token(8)
        uri = f"sip:{self.realm}"
        authorization = None
        for cseq in (1, 2):
            branch = "z9hG4bK" + token()
            lines = [f"REGISTER {uri} SIP/2.0", f"Via: {self.via};branch={branch};rport", "Max-Forwards: 70",
                     f"From: <sip:{self.user}@{self.realm}>;tag={tag}", f"To: <sip:{self.user}@{self.realm}>", f"Call-ID: {call_id}",
                     f"CSeq: {cseq} REGISTER", f"Contact: {self.contact}", "Expires: 120"]
            if authorization:
                lines.append(f"Authorization: {authorization}")
            self.send(CRLF.join(lines + ["Content-Length: 0", "", ""]))
            response, _ = self.wait(lambda m: m.is_response and m.get("call-id") == call_id and m.cseq == cseq and m.code >= 200)
            if response.code == 200:
                return
            if response.code != 401 or authorization:
                raise RuntimeError(f"{self.name}: REGISTER answered {response.start}")
            authorization = self.credentials(challenge_of(response.get("www-authenticate")), "REGISTER", uri, "Authorization")

    def close(self):
        self.connection.close()


def reply(request, code, reason, user, extra=None, body=""):
    lines = [f"SIP/2.0 {code} {reason}"]
    lines += [f"Via: {v}" for v in request.all("via")]
    lines += [f"Record-Route: {r}" for r in request.all("record-route")]
    to = request.get("to")
    if code > 100 and "tag=" not in to:
        to += ";tag=" + user + "x"
    lines += [f"From: {request.get('from')}", f"To: {to}", f"Call-ID: {request.get('call-id')}", f"CSeq: {request.get('cseq')}"]
    lines += extra or []
    lines += [f"Content-Length: {len(body)}", "", body]
    return CRLF.join(lines)


def sdp(address, port):
    return CRLF.join(["v=0", f"o=- {random.randint(1, 10**9)} 1 IN IP4 {address}", "s=-", f"c=IN IP4 {address}", "t=0 0",
                      f"m=audio {port} RTP/AVP 0", "a=rtpmap:0 PCMU/8000", ""])


class Callee(threading.Thread):
    """Answers the first INVITE after ringing, then the BYE, or hangs up itself. Records what it was sent."""

    def __init__(self, agent, hangs_up=False, hold=1.0):
        super().__init__(daemon=True)
        self.agent, self.seen, self.error = agent, [], None
        self.hangs_up, self.hold, self.bye_answer = hangs_up, hold, None

    def run(self):
        a = self.agent
        try:
            invite, source = a.wait(lambda m: not m.is_response and m.method == "INVITE", timeout=30)
            self.seen.append(invite)
            a.send(reply(invite, 180, "Ringing", a.user), source)
            time.sleep(0.5)
            ip, port = a.local.split(":")
            a.send(reply(invite, 200, "OK", a.user, [f"Contact: {a.contact}", "Content-Type: application/sdp"], sdp(ip, 40000)), source)
            ack, _ = a.wait(lambda m: not m.is_response and m.method == "ACK", timeout=10)
            self.seen.append(ack)
            if self.hangs_up:
                time.sleep(self.hold)
                return self._hang_up(invite, source)
            bye, source = a.wait(lambda m: not m.is_response and m.method == "BYE", timeout=20)
            self.seen.append(bye)
            a.send(reply(bye, 200, "OK", a.user), source)
        except Exception as error:  # noqa: BLE001 - reported by the caller
            self.error = error

    def _hang_up(self, invite, source):
        """RFC 3261 15.1.1, from the callee: its own CSeq space, the route set as recorded, the caller's Contact."""
        a = self.agent
        to_tag = a.user + "x"
        target = invite.get("contact").strip("<>").split(">")[0]
        lines = [f"BYE {target} SIP/2.0", f"Via: {a.via};branch=z9hG4bK{token()};rport", "Max-Forwards: 70"]
        lines += [f"Route: {r}" for r in invite.all("record-route")]
        lines += [f"From: {invite.get('to')};tag={to_tag}", f"To: {invite.get('from')}", f"Call-ID: {invite.get('call-id')}", "CSeq: 1 BYE"]
        a.send(CRLF.join(lines + ["Content-Length: 0", "", ""]), source)
        self.bye_answer, _ = a.wait(lambda m: m.is_response and m.method == "BYE" and m.code >= 200, timeout=15)


def call(caller, callee_thread, dial, hold):
    """Places the call and returns a list of (step, ok, detail)."""
    steps = []

    def step(name, ok, detail=""):
        steps.append((name, ok, detail))
        print(f"  {'ok' if ok else 'FAILED':6} {name}{'  - ' + detail if detail else ''}", flush=True)
        if not ok:
            raise AssertionError(name)

    a = caller
    call_id, tag = token(16), token(8)
    ip, port = a.local.split(":")
    offer = sdp(ip, 30000)
    cseq = 1
    authorization = None

    def invite_text(branch, cseq, authorization):
        lines = [f"INVITE {dial} SIP/2.0", f"Via: {a.via};branch={branch};rport", "Max-Forwards: 70",
                 f"From: <sip:{a.user}@{a.realm}>;tag={tag}", f"To: <{dial}>", f"Call-ID: {call_id}", f"CSeq: {cseq} INVITE",
                 f"Contact: {a.contact}", "Content-Type: application/sdp"]
        if authorization:
            lines.append(f"Proxy-Authorization: {authorization}")
        return CRLF.join(lines + [f"Content-Length: {len(offer)}", "", offer])

    # A final answer to this INVITE, not a retransmission of one to the INVITE before it.
    def final_for(number):
        return lambda m: m.is_response and m.get("call-id") == call_id and m.method == "INVITE" and m.cseq == number and m.code >= 200

    branch = "z9hG4bK" + token()
    a.send(invite_text(branch, cseq, None))
    response, _ = a.wait(final_for(cseq), timeout=15)
    if response.code == 407:
        step("challenged 407", True)
        # 17.1.1.3: the 407 is ACKed on its own transaction, same branch.
        a.send(CRLF.join([f"ACK {dial} SIP/2.0", f"Via: {a.via};branch={branch};rport", "Max-Forwards: 70",
                          f"From: <sip:{a.user}@{a.realm}>;tag={tag}", f"To: {response.get('to')}", f"Call-ID: {call_id}",
                          f"CSeq: {cseq} ACK", "Content-Length: 0", "", ""]))
        authorization = a.credentials(challenge_of(response.get("proxy-authenticate")), "INVITE", dial, "Proxy-Authorization")
        cseq += 1
        branch = "z9hG4bK" + token()
        a.send(invite_text(branch, cseq, authorization))
        response, _ = a.wait(final_for(cseq), timeout=30)

    step("answered 200", response.code == 200, response.start)
    step("200 is for the caller's own INVITE", response.cseq == cseq, f"CSeq {response.cseq}, sent {cseq}")
    step("200 carries an answer", "m=audio" in response.body)

    route = list(reversed(response.all("record-route")))
    to = response.get("to")
    target = response.get("contact").strip("<>").split(">")[0]

    def in_dialog(method, number, branch):
        lines = [f"{method} {target} SIP/2.0", f"Via: {a.via};branch={branch};rport", "Max-Forwards: 70"]
        lines += [f"Route: {r}" for r in route]
        lines += [f"From: <sip:{a.user}@{a.realm}>;tag={tag}", f"To: {to}", f"Call-ID: {call_id}", f"CSeq: {number} {method}"]
        return CRLF.join(lines + ["Content-Length: 0", "", ""])

    a.send(in_dialog("ACK", cseq, "z9hG4bK" + token()))

    if callee_thread.hangs_up:
        bye, source = a.wait(lambda m: not m.is_response and m.method == "BYE", timeout=20)
        step("callee's BYE reached the caller", True, f"CSeq {bye.cseq}")
        a.send(reply(bye, 200, "OK", a.user), source)
        callee_thread.join(timeout=5)
        if callee_thread.error:
            step("callee's BYE was answered", False, str(callee_thread.error))
        answer = callee_thread.bye_answer
        step("callee's BYE answered 200 in its own CSeq space", answer is not None and answer.code == 200 and answer.cseq == 1,
             answer.start if answer else "nothing")
        step("caller saw the callee's own CSeq", bye.cseq == 1, f"CSeq {bye.cseq}")
        return steps

    time.sleep(hold)

    bye_branch = "z9hG4bK" + token()
    a.send(in_dialog("BYE", cseq + 1, bye_branch))
    bye_answer, _ = a.wait(lambda m: m.is_response and m.get("call-id") == call_id and m.method == "BYE" and m.code >= 200, timeout=15)
    step("BYE answered 200", bye_answer.code == 200, bye_answer.start)
    step("BYE answer in the caller's CSeq space", bye_answer.cseq == cseq + 1, f"CSeq {bye_answer.cseq}")

    callee_thread.join(timeout=5)
    if callee_thread.error:
        step("callee saw the whole call", False, str(callee_thread.error))
    seen = callee_thread.seen
    methods = [m.method for m in seen]
    step("callee saw INVITE, ACK, BYE", methods == ["INVITE", "ACK", "BYE"], ", ".join(methods))
    invite, ack, bye = seen
    step("callee's ACK has its INVITE's CSeq", ack.cseq == invite.cseq, f"INVITE {invite.cseq}, ACK {ack.cseq}")
    step("callee's BYE is above its INVITE", bye.cseq > invite.cseq, f"INVITE {invite.cseq}, BYE {bye.cseq}")
    step("one Call-ID end to end", invite.get("call-id") == call_id)
    return steps


def local_ip_towards(host):
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((host, 5060))
    address = probe.getsockname()[0]
    probe.close()
    return address


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--caller", required=True, help="user@node")
    parser.add_argument("--callee", required=True, help="user@node")
    parser.add_argument("--caller-realm")
    parser.add_argument("--callee-realm")
    parser.add_argument("--password", default="athenaphone")
    parser.add_argument("--dial", help="the Request-URI dialled; the callee's address of record by default")
    parser.add_argument("--hold", type=float, default=1.0, help="seconds between the ACK and the BYE")
    parser.add_argument("--callee-hangs-up", action="store_true", help="the callee sends the BYE")
    parser.add_argument("--caller-transport", default="udp", choices=sorted(PORTS))
    parser.add_argument("--callee-transport", default="udp", choices=sorted(PORTS))
    args = parser.parse_args()

    caller_user, caller_node = args.caller.split("@")
    callee_user, callee_node = args.callee.split("@")
    caller_realm = args.caller_realm or caller_node
    callee_realm = args.callee_realm or callee_node
    dial = args.dial or f"sip:{callee_user}@{callee_realm}"

    caller = Agent("caller", caller_user, caller_node, caller_realm, args.password, local_ip_towards(caller_node), args.caller_transport)
    callee = Agent("callee", callee_user, callee_node, callee_realm, args.password, local_ip_towards(callee_node), args.callee_transport)
    try:
        callee.register()
        caller.register()
        print(f"Calling {dial} from {caller_user}@{caller_realm} over {args.caller_transport}, answered by {callee_user}@{callee_realm} "
              f"over {args.callee_transport}", flush=True)
        thread = Callee(callee, args.callee_hangs_up, args.hold)
        thread.start()
        call(caller, thread, dial, args.hold)
        print("\nThe call went through.")
        return 0
    except (AssertionError, TimeoutError, RuntimeError, OSError) as error:
        print(f"\nThe call failed: {error}")
        return 1
    finally:
        caller.close()
        callee.close()


if __name__ == "__main__":
    sys.exit(main())
