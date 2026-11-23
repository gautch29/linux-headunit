#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Request one CarPlay session from an explicitly selected phone on the lab AP.

Run after Bluetooth/MFi/Wi-Fi handoff, with the receiver already listening.
This bounded lab utility is not an automatic pairing/reconnection manager.
"""
import argparse
import http.client
import ipaddress
from pathlib import Path
import re
import shlex
import subprocess


MAC = re.compile(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}")


def endpoint(line, phone):
    fields = line.rstrip().split(";", 9)
    if len(fields) != 10 or fields[:3] != ["=", "wlan0", "IPv4"] or \
            fields[4:6] != ["_carplay-ctrl._tcp", "local"]:
        return None
    try:
        address = ipaddress.IPv4Address(fields[7])
        port = int(fields[8])
        records = dict(item.split("=", 1) for item in shlex.split(fields[9]) if "=" in item)
    except (ValueError, TypeError):
        return None
    network = ipaddress.IPv4Network("10.10.0.0/24")
    if records.get("id", "").lower() != phone.lower() or address not in network or \
            address in (network.network_address, network.broadcast_address, ipaddress.IPv4Address("10.10.0.1")) or \
            not 1 <= port <= 65535:
        return None
    return str(address), port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phone-address", required=True, help="explicitly selected phone's Bluetooth MAC")
    parser.add_argument("--identity", required=True, type=Path)
    args = parser.parse_args()
    if not MAC.fullmatch(args.phone_address):
        parser.error("invalid phone Bluetooth address")
    fields = dict(line.split("=", 1) for line in args.identity.read_text().splitlines())
    device_id = fields.get("deviceid", "")
    if not MAC.fullmatch(device_id):
        parser.error("invalid receiver device ID")
    result = subprocess.run(["avahi-browse", "-rtp", "_carplay-ctrl._tcp"],
                            capture_output=True, text=True, check=True, timeout=8)
    endpoints = {value for line in result.stdout.splitlines()
                 if (value := endpoint(line, args.phone_address)) is not None}
    if len(endpoints) != 1:
        raise RuntimeError("expected one selected phone endpoint on the lab AP")
    host, port = endpoints.pop()
    status = request_connection((host, port), device_id, fields.get("srcvers", "950.7.1"))
    if status != 200:
        raise RuntimeError(f"phone rejected connection request: HTTP {status}")
    print("Phone accepted CarPlay connection request; verify video in receiver logs")


def request_connection(target, device_id, source_version):
    connection = http.client.HTTPConnection(*target, timeout=5)
    try:
        connection.request("GET", "/ctrl-int/1/connect", headers={
            "AirPlay-Receiver-Device-ID": str(int(device_id.replace(":", ""), 16)),
            "User-Agent": "AirPlay/" + source_version,
        })
        return connection.getresponse().status
    finally:
        connection.close()


if __name__ == "__main__":
    main()
