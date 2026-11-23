#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Bounded Ethernet-frame capture of the Pi's CarPlay access-point interface.

The output is a standard PCAP file. Run only on the dedicated AP interface;
captures can contain private traffic and should never be committed.
"""

import argparse
import socket
import struct
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("interface")
    parser.add_argument("output")
    parser.add_argument("--seconds", type=int, default=90)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 300:
        parser.error("duration must be 1..300 seconds")

    capture = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
    capture.bind((args.interface, 0))
    capture.settimeout(0.5)
    end = time.monotonic() + args.seconds
    packets = 0
    with open(args.output, "xb") as output:
        output.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        while time.monotonic() < end:
            try:
                frame = capture.recv(65535)
            except socket.timeout:
                continue
            when = time.time_ns()
            output.write(struct.pack("<IIII", when // 1_000_000_000,
                                     (when // 1_000) % 1_000_000,
                                     len(frame), len(frame)))
            output.write(frame)
            packets += 1
    capture.close()
    print(f"captured {packets} frames on {args.interface}", flush=True)


if __name__ == "__main__":
    main()
