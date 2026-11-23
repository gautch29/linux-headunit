#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Print packet-flow metadata from a local PCAP; never print packet payloads."""

import collections
import ipaddress
import struct
import sys


def summarize(path):
    flows = collections.defaultdict(lambda: [0, 0, None, None])
    with open(path, "rb") as capture:
        header = capture.read(24)
        if len(header) != 24 or struct.unpack_from("<I", header)[0] != 0xA1B2C3D4:
            raise ValueError("expected little-endian Ethernet PCAP")
        while record := capture.read(16):
            if len(record) != 16:
                raise ValueError("truncated packet record")
            sec, usec, size, _ = struct.unpack("<IIII", record)
            if size > 65535:
                raise ValueError("oversized packet record")
            frame = capture.read(size)
            if len(frame) != size:
                raise ValueError("truncated packet")
            if len(frame) < 34:
                continue
            ether_type = frame[12:14]
            ip = frame[14:]
            if ether_type == b"\x08\x00":
                header_bytes = (ip[0] & 15) * 4
                if ip[0] >> 4 != 4 or header_bytes < 20 or len(ip) < header_bytes + 4:
                    continue
                protocol = ip[9]
                source = str(ipaddress.IPv4Address(ip[12:16]))
                target = str(ipaddress.IPv4Address(ip[16:20]))
            elif ether_type == b"\x86\xdd":
                if len(ip) < 44 or ip[0] >> 4 != 6:
                    continue
                header_bytes = 40
                protocol = ip[6]
                source = str(ipaddress.IPv6Address(ip[8:24]))
                target = str(ipaddress.IPv6Address(ip[24:40]))
            else:
                continue
            if protocol not in (6, 17):
                continue
            ports = ip[header_bytes:header_bytes + 4]
            src_port, dst_port = struct.unpack("!HH", ports)
            key = ("TCP" if protocol == 6 else "UDP", source, src_port, target, dst_port)
            count, total, first, _ = flows[key]
            when = sec + usec / 1_000_000
            flows[key] = [count + 1, total + size, first or when, when]
    for key, (count, size, first, last) in sorted(
            flows.items(), key=lambda item: item[1][1], reverse=True):
        protocol, source, src_port, target, dst_port = key
        print(f"{protocol} {source}:{src_port} -> {target}:{dst_port} "
              f"packets={count} bytes={size} span={last-first:.1f}s")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: summarize_capture.py FILE.pcap")
    summarize(sys.argv[1])
