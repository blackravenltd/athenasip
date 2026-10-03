#!/usr/bin/env python3
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
"""What rtpengine did with the media, asked of rtpengine.

    test/interop/media-stats.py              every call it is holding
    test/interop/media-stats.py --watch      the same, once a second
    test/interop/media-stats.py <call-id>    one call

The engine's own counters are the only thing that says media moved rather than that a
call was signalled: a description the engine declined travels on untouched and the two
endpoints reach each other directly, which looks identical from the outside. This is
the thing to read after a manual call, and the answer that goes in the interop matrix.

It speaks rtpengine's ng protocol over UDP and needs nothing installed. The control
port is published on loopback by the fixture's rtpengine overlay.
"""

import argparse
import signal
import socket
import sys
import time


# Bencode, which is all the ng protocol is: a cookie, a space, and one bencoded dict.
def encode(value):
    if isinstance(value, str):
        value = value.encode()
    if isinstance(value, bytes):
        return str(len(value)).encode() + b":" + value
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, dict):
        return b"d" + b"".join(encode(k) + encode(v) for k, v in value.items()) + b"e"
    if isinstance(value, (list, tuple)):
        return b"l" + b"".join(encode(item) for item in value) + b"e"
    raise TypeError(f"cannot bencode {type(value).__name__}")


def decode(data, at=0):
    kind = data[at:at + 1]

    if kind == b"i":
        end = data.index(b"e", at)
        return int(data[at + 1:end]), end + 1

    if kind == b"l":
        at += 1
        items = []
        while data[at:at + 1] != b"e":
            item, at = decode(data, at)
            items.append(item)
        return items, at + 1

    if kind == b"d":
        at += 1
        out = {}
        while data[at:at + 1] != b"e":
            key, at = decode(data, at)
            value, at = decode(data, at)
            out[key.decode(errors="replace") if isinstance(key, bytes) else key] = value
        return out, at + 1

    separator = data.index(b":", at)
    length = int(data[at:separator])
    start = separator + 1
    return data[start:start + length], start + length


class Engine:
    def __init__(self, host, port, timeout):
        self._address = (host, port)
        self._timeout = timeout
        self._cookie = 0

    def command(self, **fields):
        self._cookie += 1
        cookie = f"media-stats-{self._cookie}".encode()

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(self._timeout)

        try:
            sock.sendto(cookie + b" " + encode(fields), self._address)
            reply = sock.recv(262144)
        finally:
            sock.close()

        body = reply.split(b" ", 1)[1]
        answer, _ = decode(body)

        if answer.get("result") == b"error":
            raise RuntimeError(answer.get("error-reason", b"unknown").decode(errors="replace"))

        return answer


def text(value):
    return value.decode(errors="replace") if isinstance(value, bytes) else str(value)


def report(call_id, answer):
    """One call, as few lines as say whether its media worked."""
    totals = answer.get("totals", {}).get("RTP", {})

    print(f"{call_id}")
    print(f"  relayed  {totals.get('packets', 0)} packets, {totals.get('bytes', 0)} bytes, {totals.get('errors', 0)} errors")

    for tag, leg in sorted(answer.get("tags", {}).items()):
        for media in leg.get("medias", []):
            flags = [text(flag) for flag in media.get("flags", [])]

            for stream in media.get("streams", []):
                stats = stream.get("stats", {})
                if not stats.get("packets"):
                    continue

                stream_flags = [text(flag) for flag in stream.get("flags", [])]
                endpoint = stream.get("endpoint", {})

                print(
                    f"  {tag:<12} port {stream.get('local port')}"
                    f"  {text(media.get('protocol', b'?'))}"
                    f"  {text(stream.get('crypto suite', b'none'))}"
                )
                print(
                    f"               from {text(endpoint.get('address', b'?'))}:{endpoint.get('port', 0)}"
                    f"  {stats.get('packets')} packets, {stats.get('bytes')} bytes, {stats.get('errors')} errors"
                )

                interesting = [f for f in stream_flags + flags if f in (
                    "DTLS fingerprint verified", "ICE", "rtcp-mux", "DTLS-SRTP", "confirmed", "kernelized")]
                if interesting:
                    print(f"               {', '.join(dict.fromkeys(interesting))}")

    # A call with no relayed packet is the failure this tool exists to make visible.
    if not totals.get("packets"):
        print("  NOTHING WAS RELAYED - the engine was asked but no media reached it")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("call_id", nargs="?", help="one call, rather than every call it is holding")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=22222, help="the ng control port (ATHENA_INTEROP_NG_PORT)")
    parser.add_argument("--watch", action="store_true", help="repeat once a second until interrupted")
    parser.add_argument("--timeout", type=float, default=5.0)

    args = parser.parse_args()

    # Watching ends by being stopped, and being stopped has to leave the output behind:
    # SIGTERM raises rather than killing the process where it stands.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))

    engine = Engine(args.host, args.port, args.timeout)

    def once():
        if args.call_id:
            calls = [args.call_id]
        else:
            listed = engine.command(command="list")
            calls = [text(call) for call in listed.get("calls", [])]

        if not calls:
            print("No calls. The engine is up and holding nothing.")
            return

        for call_id in calls:
            report(call_id, engine.command(command="query", **{"call-id": call_id}))

    try:
        if not args.watch:
            once()
            return 0

        # Flushed every time round. Watching is the mode whose output is read while it
        # is still being written - in a terminal beside a call, or into a file that is
        # read after the call ended - and a block-buffered stdout loses all of it.
        while True:
            print(f"--- {time.strftime('%H:%M:%S')}", flush=True)
            once()
            sys.stdout.flush()
            time.sleep(1)
    except KeyboardInterrupt:
        return 0
    except (socket.timeout, TimeoutError):
        print(f"No answer from rtpengine at {args.host}:{args.port}.", file=sys.stderr)
        print("Is the fixture up with --rtpengine? The control port is published on loopback only.", file=sys.stderr)
        return 1
    except (RuntimeError, OSError) as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
