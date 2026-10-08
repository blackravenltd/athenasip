#!/usr/bin/env python3
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
"""Calls to and from a real Android phone running AthenaPhone, driven over adb.

    ATHENA_PROBE_PASSWORD=... test/interop/phone.py --node 10.44.1.50 --realm macnessa.athenasip.org \\
        --phone tom --user 1901

In: a probe subscriber calls the phone's address of record, the script answers from the
incoming-call notification, checks the phone's media came up (AthenaPhone logs a DTLS
connection), and hangs up from the probe's end. Out: the script dials the probe on the
phone's keypad, the probe answers, and the script ends the call on the phone. The probe's
end checks what RFC 3261 asks, as call.py does.

The in-call screen is a window uiautomator's dump does not see, so End call is pressed by
where it sits on the screen. Every path ends any call it started, so a failed run leaves
nothing ringing or connected.

Needs adb connected to one device (adb connect <address>), the app installed and the phone's
account registered on the node.
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import call  # noqa: E402

PACKAGE = "com.athenaphone"

# Where the call screen's End call button sits, as fractions of the screen (AthenaPhone 0.4.0).
END_CALL = (0.5, 0.853)


def adb(*args):
    return subprocess.run(["adb", *args], capture_output=True, text=True, timeout=30).stdout


def screen():
    """The labels on screen, each with its centre: [(label, x, y)], every window included."""
    adb("shell", "uiautomator", "dump", "--windows", "/sdcard/athenasip-ui.xml")
    xml = adb("exec-out", "cat", "/sdcard/athenasip-ui.xml")
    found = []
    for match in re.finditer(r'text="([^"]*)"[^>]*content-desc="([^"]*)"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml):
        label = match.group(1) or match.group(2)
        if label:
            x1, y1, x2, y2 = (int(match.group(i)) for i in range(3, 7))
            found.append((label, (x1 + x2) // 2, (y1 + y2) // 2))
    return found


def find(pattern, labels=None):
    for label, x, y in labels if labels is not None else screen():
        if re.fullmatch(pattern, label, re.IGNORECASE):
            return x, y
    return None


def wait_for(pattern, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        where = find(pattern)
        if where:
            return where
        time.sleep(0.5)
    labels = [label for label, _, _ in screen()]
    raise TimeoutError(f"no '{pattern}' on the phone in {timeout}s; it shows: {', '.join(labels[:15])}")


def tap(where):
    adb("shell", "input", "tap", str(where[0]), str(where[1]))


def end_call():
    size = re.search(r"(\d+)x(\d+)", adb("shell", "wm", "size"))
    width, height = (int(size.group(1)), int(size.group(2))) if size else (720, 1600)
    tap((int(width * END_CALL[0]), int(height * END_CALL[1])))


def media_up(since, timeout=10.0):
    """Whether AthenaPhone has logged its media connected since a logcat time."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        # Quoted: adb joins its arguments into one command line for the phone's shell.
        log = adb("logcat", "-d", "-T", f"'{since}'", "-s", "ReactNativeJS")
        if "dtls=connected" in log:
            return True
        time.sleep(1)
    return False


def logcat_now():
    # Quoted: adb joins its arguments into one command line for the phone's shell.
    return adb("shell", "date", "'+%m-%d %H:%M:%S.000'").strip()


def to_keypad():
    adb("shell", "am", "start", "-n", f"{PACKAGE}/.MainActivity")
    time.sleep(1.5)
    close = find("Close")
    if close:
        tap(close)
        time.sleep(1)
    keypad = find("Keypad")
    if keypad:
        tap(keypad)
        time.sleep(0.5)


def report(name, ok, detail=""):
    print(f"  {'ok' if ok else 'FAILED':6} {name}{'  - ' + detail if detail else ''}", flush=True)
    if not ok:
        raise AssertionError(name)


def call_in(args):
    """The probe calls the phone; the phone answers from its notification; the probe hangs up."""
    a = call.Agent("probe", args.user, args.node, args.realm, args.password, call.local_ip_towards(args.node), args.transport)
    dial = f"sip:{args.phone}@{args.realm}"
    call_id, tag = call.token(16), call.token(8)
    state = {"cseq": 1, "branch": None, "answered": None}
    sender = f"From: <sip:{a.user}@{a.realm}>;tag={tag}"

    def invite(cseq, authorization):
        state["branch"] = "z9hG4bK" + call.token()
        offer = call.sdp(a.local.split(":")[0], 30000)
        lines = [f"INVITE {dial} SIP/2.0", f"Via: {a.via};branch={state['branch']};rport", "Max-Forwards: 70", sender, f"To: <{dial}>",
                 f"Call-ID: {call_id}", f"CSeq: {cseq} INVITE", f"Contact: {a.contact}", "Content-Type: application/sdp"]
        if authorization:
            lines.append(f"Proxy-Authorization: {authorization}")
        return call.CRLF.join(lines + [f"Content-Length: {len(offer)}", "", offer])

    def ack_for(to, cseq, branch, target=dial, route=()):
        lines = [f"ACK {target} SIP/2.0", f"Via: {a.via};branch={branch};rport", "Max-Forwards: 70"] + [f"Route: {r}" for r in route]
        return call.CRLF.join(lines + [sender, f"To: {to}", f"Call-ID: {call_id}", f"CSeq: {cseq} ACK", "Content-Length: 0", "", ""])

    def any_for(cseq):
        return lambda m: m.is_response and m.get("call-id") == call_id and m.method == "INVITE" and m.cseq == cseq

    def final_for(cseq):
        return lambda m: any_for(cseq)(m) and m.code >= 200

    try:
        a.register()
        to_keypad()
        print(f"In: {args.user} calls {dial} over {args.transport}", flush=True)

        text = invite(1, None)
        a.send(text)
        response, _ = a.wait(final_for(1), timeout=15, resend=text, until=any_for(1))
        if response.code == 407:
            a.send(ack_for(response.get("to"), 1, state["branch"]))
            state["cseq"] = 2
            text = invite(2, a.credentials(call.challenge_of(response.get("proxy-authenticate")), "INVITE", dial, ""))
            a.send(text)
            a.wait(any_for(2), timeout=15, resend=text)

        since = logcat_now()
        answer = wait_for("Answer|Accept", timeout=20)
        report("the phone rang", True)
        tap(answer)

        response, _ = a.wait(final_for(state["cseq"]), timeout=20)
        report("the phone answered 200", response.code == 200, response.start)
        state["answered"] = response

        route = list(reversed(response.all("record-route")))
        target = response.get("contact").strip("<>").split(">")[0]
        ack = ack_for(response.get("to"), state["cseq"], "z9hG4bK" + call.token(), target, route)
        a.acks[call_id] = ack
        a.send(ack)

        report("the phone's media came up", media_up(since))
        time.sleep(args.hold)
    finally:
        try:
            response = state["answered"]
            if response is not None:
                route = list(reversed(response.all("record-route")))
                target = response.get("contact").strip("<>").split(">")[0]
                lines = [f"BYE {target} SIP/2.0", f"Via: {a.via};branch=z9hG4bK{call.token()};rport", "Max-Forwards: 70"] + [f"Route: {r}" for r in route]
                bye = call.CRLF.join(lines + [sender, f"To: {response.get('to')}", f"Call-ID: {call_id}", f"CSeq: {state['cseq'] + 1} BYE",
                                              "Content-Length: 0", "", ""])
                a.send(bye)
                answer, _ = a.wait(lambda m: m.is_response and m.get("call-id") == call_id and m.method == "BYE" and m.code >= 200, timeout=15,
                                   resend=bye)
                report("the phone answered the BYE 200", answer.code == 200, answer.start)
            elif state["branch"]:
                # Never answered: a CANCEL, so the phone stops ringing.
                a.send(call.CRLF.join([f"CANCEL {dial} SIP/2.0", f"Via: {a.via};branch={state['branch']};rport", "Max-Forwards: 70", sender,
                                       f"To: <{dial}>", f"Call-ID: {call_id}", f"CSeq: {state['cseq']} CANCEL", "Content-Length: 0", "", ""]))
        finally:
            a.close()
            time.sleep(1)
            to_keypad()


def call_out(args):
    """The phone dials the probe; the probe answers; the phone hangs up."""
    probe = call.Agent("probe", args.user, args.node, args.realm, args.password, call.local_ip_towards(args.node), args.transport)
    answering = None
    try:
        probe.register()
        answering = call.Callee(probe)
        answering.start()

        to_keypad()
        labels = screen()
        for digit in args.user:
            key = find(f"Dial {re.escape(digit)}", labels)
            report(f"the keypad has {digit}", key is not None)
            tap(key)
            time.sleep(0.2)
        print(f"Out: the phone dials {args.user}, answered over {args.transport}", flush=True)
        since = logcat_now()
        tap(wait_for("Start audio call"))

        deadline = time.time() + 20
        while time.time() < deadline and len(answering.seen) < 2 and not answering.error:
            time.sleep(0.2)
        report("the probe was called and its 200 ACKed", len(answering.seen) >= 2, str(answering.error or ""))
        invite = answering.seen[0]
        offered = [line for line in invite.body.split(call.CRLF) if line.startswith("m=")]
        if answering.refused:
            print(f"  (the probe refused {len(answering.refused)} offer(s) it cannot take, and took: {', '.join(offered)})", flush=True)
        report("the call is from the phone's account", f"sip:{args.phone}@" in (invite.get("from") or ""), invite.get("from"))
        report("the phone's media came up", media_up(since))

        time.sleep(args.hold)
        end_call()
        answering.join(timeout=15)
        report("the phone hung up with a BYE", len(answering.seen) == 3 and answering.seen[2].method == "BYE",
               ", ".join(m.method for m in answering.seen))
        report("its BYE is above its INVITE", answering.seen[2].cseq > invite.cseq, f"INVITE {invite.cseq}, BYE {answering.seen[2].cseq}")
    finally:
        # Whatever happened, nothing is left up on the phone.
        if answering is not None and answering.is_alive():
            end_call()
        probe.close()
        time.sleep(1)
        to_keypad()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--node", required=True)
    parser.add_argument("--realm", required=True)
    parser.add_argument("--phone", required=True, help="the user part of the phone's address of record")
    parser.add_argument("--user", required=True, help="the probe subscriber")
    parser.add_argument("--password", default=os.environ.get("ATHENA_PROBE_PASSWORD", ""))
    parser.add_argument("--transport", default="udp", choices=sorted(call.PORTS))
    parser.add_argument("--hold", type=float, default=3.0)
    parser.add_argument("--only", choices=["in", "out"])
    args = parser.parse_args()

    failed = 0
    for direction, run in (("in", call_in), ("out", call_out)):
        if args.only and args.only != direction:
            continue
        try:
            run(args)
        except (AssertionError, TimeoutError, RuntimeError, OSError) as error:
            print(f"  FAILED {direction}: {error}")
            failed += 1
    print("\nThe phone's calls went through." if not failed else f"\n{failed} of the phone's calls failed.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
