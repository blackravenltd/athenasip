#!/usr/bin/env python3
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
"""Checks a live deployment: every node's health, registration on every transport, and the
calls the deployment is for. Run it after every deploy; it prints one line per check and
exits non-zero if any failed.

    test/interop/verify.py test/interop/live.json
    test/interop/verify.py test/interop/live.json --publish mqtt://10.35.1.10:1883

The deployment is described in JSON:

    {
      "nodes": {"fi-1": {"host": "10.35.1.20", "realm": "10.35.1.20", "http": 8080}},
      "password_file": "/root/.athenasip-probe-password",
      "registrations": [{"node": "fi-1", "user": "1001"}],
      "calls": [
        {"name": "local over tls", "caller": "1001@fi-1", "callee": "1002@fi-1", "transports": "tls:tls"}
      ]
    }

A call may name "dial" (the Request-URI, for a number that leaves by a trunk) and
"callee_hangs_up". Nodes are named in the file; a caller or callee is user@node.

--publish posts the result, retained, as athenasip/probes/<name>/calls on an MQTT broker, for
a monitor; it needs mosquitto_pub.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))


def health(node):
    url = f"http://{node['host']}:{node.get('http', 8080)}/api/v1/health"
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            body = json.load(response)
        return body.get("status") == "ok", f"{body.get('status')} {body.get('version', '')}".strip()
    except Exception as error:  # noqa: BLE001 - reported as the check's detail
        return False, str(error)


def run(command, timeout):
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, f"no result in {timeout}s"
    lines = [line for line in done.stdout.splitlines() if line.strip()]
    failed = [line.strip() for line in lines if "FAILED" in line]
    detail = failed[0] if failed else (lines[-1] if lines else done.stderr.strip()[-200:])
    return done.returncode == 0, detail


def endpoint(spec, nodes):
    user, name = spec.split("@")
    node = nodes[name]
    return user, node["host"], node.get("realm", node["host"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("deployment", help="the JSON description of the deployment")
    parser.add_argument("--publish", help="an mqtt:// broker to publish the result to")
    parser.add_argument("--name", default=socket.gethostname().split(".")[0], help="this probe's name, for --publish")
    args = parser.parse_args()

    with open(args.deployment) as file:
        deployment = json.load(file)
    nodes = deployment["nodes"]

    # The probe's password is never in the file: from the environment, else a file on the probe's host.
    password = os.environ.get("ATHENA_PROBE_PASSWORD")
    if not password and os.path.exists(deployment.get("password_file", "")):
        with open(deployment["password_file"]) as file:
            password = file.read().strip()
    if not password:
        print("No password: set ATHENA_PROBE_PASSWORD, or put it in the deployment's password_file.")
        return 2
    results = []

    def record(check, ok, detail):
        results.append({"check": check, "ok": ok, "detail": detail})
        print(f"  {'ok' if ok else 'FAILED':6} {check:46} {detail}", flush=True)

    for name, node in nodes.items():
        record(f"{name} health", *health(node))

    for registration in deployment.get("registrations", []):
        node = nodes[registration["node"]]
        transports = registration.get("transports", "udp,tcp,tls,ws")
        ok, detail = run([sys.executable, os.path.join(HERE, "smoke.py"), "--host", node["host"], "--realm", node.get("realm", node["host"]),
                          "--user", registration["user"], "--password", password, "--transports", transports], 60)
        record(f"{registration['user']}@{registration['node']} registers ({transports})", ok, detail)

    for call in deployment.get("calls", []):
        caller_user, caller_host, caller_realm = endpoint(call["caller"], nodes)
        callee_user, callee_host, callee_realm = endpoint(call["callee"], nodes)
        caller_transport, callee_transport = call.get("transports", "udp:udp").split(":")
        command = [sys.executable, os.path.join(HERE, "call.py"), "--caller", f"{caller_user}@{caller_host}", "--caller-realm", caller_realm,
                   "--callee", f"{callee_user}@{callee_host}", "--callee-realm", callee_realm, "--password", password,
                   "--caller-transport", caller_transport, "--callee-transport", callee_transport, "--hold", str(call.get("hold", 0.5))]
        if "dial" in call:
            command += ["--dial", call["dial"]]
        if call.get("callee_hangs_up"):
            command.append("--callee-hangs-up")
        record(call["name"], *run(command, 90))

    failed = [result for result in results if not result["ok"]]
    print(f"\n{len(results) - len(failed)} of {len(results)} checks passed.")

    if args.publish:
        broker = args.publish.removeprefix("mqtt://")
        host, _, port = broker.partition(":")
        report = {"probe": args.name, "ok": not failed, "passed": len(results) - len(failed), "checks": len(results),
                  "failed": [f"{result['check']}: {result['detail']}" for result in failed], "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        subprocess.run(["mosquitto_pub", "-h", host, "-p", port or "1883", "-r", "-t", f"athenasip/probes/{args.name}/calls", "-m", json.dumps(report)],
                       check=False, timeout=10)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
