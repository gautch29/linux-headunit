#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Bounded lab CarPlay reconnect manager; never runs on the video/input thread."""
import argparse
import http.client
from dataclasses import dataclass
import ipaddress
import json
import os
from pathlib import Path
import signal
import subprocess
import time

from request_carplay_connection import MAC, endpoint, request_connection


@dataclass
class ReconnectPolicy:
    retry_seconds: float = 15
    max_attempts: int = 3
    target: tuple | None = None
    attempts: int = 0
    next_at: float = 0
    active: bool = False

    def ready(self, now, target, active):
        # Presence loss or a new endpoint starts a new bounded discovery cycle.
        if target != self.target:
            self.target, self.attempts, self.next_at = target, 0, now
        if active:
            self.attempts = 0
            self.next_at = now + self.retry_seconds
        elif self.active:
            self.attempts = 0
            self.next_at = now + 1
        self.active = active
        return bool(target and not active and self.attempts < self.max_attempts and now >= self.next_at)

    def attempted(self, now):
        self.attempts += 1
        self.next_at = now + self.retry_seconds


def command(args, timeout=8):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=timeout).stdout


def paired_phone(phone):
    path = "/org/bluez/hci0/dev_" + phone.upper().replace(":", "_")
    reply = json.loads(command(["busctl", "--json=short", "call", "org.bluez", path,
                               "org.freedesktop.DBus.Properties", "GetAll", "s", "org.bluez.Device1"]))
    properties = reply["data"][0]
    return properties.get("Paired", {}).get("data") is True and \
        properties.get("Blocked", {}).get("data") is False


def discovered_phone(phone):
    output = command(["avahi-browse", "-rtp", "_carplay-ctrl._tcp"])
    endpoints = {result for line in output.splitlines() if (result := endpoint(line, phone)) is not None}
    return next(iter(endpoints)) if len(endpoints) == 1 else None


def remote_host(text):
    host, _, port = text.rpartition(":")
    if not port.isdigit():
        return None
    try:
        address = ipaddress.ip_address(host.strip("[]"))
        return str(address.ipv4_mapped) if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped else str(address)
    except ValueError:
        return None


def has_control_connection(output, host=None):
    # ss -H -tn state established prints queues, local endpoint, peer endpoint.
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        if len(fields) < 4 or remote_host(fields[3]) is None:
            raise ValueError("unrecognized established connection record")
        remote = remote_host(fields[3])
        if host is None or remote == host:
            return True
    return False


def active_phone(host=None):
    return has_control_connection(command(["ss", "-H", "-tn", "state", "established", "( sport = :7000 )"]), host)


def receiver_listening():
    return bool(command(["ss", "-H", "-ltn", "sport = :7000"]).strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phone-address", required=True)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--retry-seconds", type=int, default=15)
    parser.add_argument("--max-attempts", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.retry_seconds <= 600 or not 1 <= args.max_attempts <= 10:
        parser.error("invalid reconnect retry bounds")
    if not MAC.fullmatch(args.phone_address) or not 0 <= args.seconds <= 3600:
        parser.error("invalid selected phone or duration")
    fields = dict(line.split("=", 1) for line in args.identity.read_text().splitlines())
    device_id, source_version = fields.get("deviceid", ""), fields.get("srcvers", "")
    if not MAC.fullmatch(device_id) or not source_version or any(ord(c) < 32 or ord(c) > 126 for c in source_version):
        parser.error("invalid receiver identity")
    stopping = False

    def stop(_signal, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    policy = ReconnectPolicy(args.retry_seconds, args.max_attempts)
    diagnostics = os.environ.get('HEADUNIT_CONNECTION_DIAGNOSTICS') == '1'
    diagnostic_reports = 0

    diagnostic_begins = {}
    diagnostic_previous = {}

    def diagnostic(stage, **facts):
        nonlocal diagnostic_reports
        if not diagnostics or diagnostic_reports >= 64:
            return
        try:
            # Capture the real date at the boundary, even when an unchanged
            # empty scan is suppressed. Retain only each operation's latest
            # begin, so discovery appearing later still has its own duration.
            try: date = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            except Exception: date = 'none'
            if stage.endswith('-begin'):
                diagnostic_begins[stage[:-6]] = date
                return
            operation = stage.rsplit('-', 1)[0] if stage.endswith(('-end', '-failed')) else stage
            signature = (policy.attempts, tuple(sorted(facts.items())))
            if diagnostic_previous.get(stage) == signature:
                diagnostic_begins.pop(operation, None)
                return
            diagnostic_previous[stage] = signature
            begin = diagnostic_begins.pop(operation, None)
            rows = [(operation + '-begin', begin, {})] if begin is not None else []
            rows.append((stage, date, facts))
            for name, observed, values in rows:
                if diagnostic_reports >= 64:
                    break
                diagnostic_reports += 1
                fields = ' '.join(f'{key}={value}' for key, value in values.items())
                print(f'CarPlay connection diagnostic mono_ns={observed} stage={name} attempt={policy.attempts}'
                      + (' ' + fields if fields else ''), flush=True)
        except Exception:
            pass  # Optional timing/output must not change a connection request.

    deadline = time.monotonic() + args.seconds if args.seconds else float('inf')
    previous = None
    print("Automatic CarPlay connection manager ready for configured paired phone", flush=True)
    while not stopping and time.monotonic() < deadline:
        cycle_started = time.monotonic()
        active = False
        stage = 'control-check'
        try:
            # iOS can withdraw its discovery service while CarPlay is active.
            # Any receiver control connection occupies this single-phone bench.
            diagnostic('control-check-begin')
            active = active_phone()
            diagnostic('control-check-end', active=int(active))
            if active:
                target = policy.target
            else:
                stage = 'inspection'
                diagnostic('inspection-begin')
                paired = paired_phone(args.phone_address)
                diagnostic('inspection-end', paired=int(paired))
                target = None
                if paired:
                    stage = 'discovery'
                    diagnostic('discovery-begin')
                    target = discovered_phone(args.phone_address)
                    diagnostic('discovery-end', targetPresent=int(target is not None))
            now = time.monotonic()
            ready = policy.ready(now, target, active)
            state = ("active" if active else "waiting for selected phone" if not target else
                     "retry limit" if policy.attempts >= policy.max_attempts else "waiting for CarPlay")
            if state != previous:
                print("CarPlay connection state: " + state, flush=True)
                diagnostic('state-change', active=int(active), targetPresent=int(target is not None))
                previous = state
            if ready and receiver_listening() and not stopping and time.monotonic() < deadline:
                # Recheck immediately before requesting; discovery can take seconds.
                if not active_phone():
                    policy.attempted(time.monotonic())
                    stage = 'request'
                    diagnostic('request-begin')
                    status = request_connection(target, device_id, source_version)
                    diagnostic('request-end', httpStatus=status)
                    print(f"CarPlay connect attempt={policy.attempts} HTTP={status}; waiting for receiver session", flush=True)
            if target and not active and policy.attempts == policy.max_attempts:
                if previous != "retry limit":
                    print("CarPlay reconnect retry limit reached; awaiting presence or session change", flush=True)
                    previous = "retry limit"
        except (OSError, ValueError, KeyError, IndexError, TypeError, http.client.HTTPException, subprocess.SubprocessError) as error:
            diagnostic(stage + '-failed', error=type(error).__name__)
            # Failure to inspect state never authorizes a connection request.
            if previous != "inspection unavailable":
                print("CarPlay connection inspection unavailable: " + type(error).__name__, flush=True)
                previous = "inspection unavailable"
        # Discovery itself can take a second. Count that work toward the
        # inactive cadence instead of adding a full pause after every scan.
        # An established session only needs the existing quiet presence poll.
        wait_until = min(deadline, cycle_started + (2 if active else 0.5))
        while not stopping and time.monotonic() < wait_until:
            time.sleep(0.1)
    print("Automatic CarPlay connection manager stopped", flush=True)


if __name__ == "__main__":
    main()
