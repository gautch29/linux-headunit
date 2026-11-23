#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Summarize iAP2 headers in a local btmon capture without printing payloads."""

import argparse
import collections
import re
import subprocess


def valid_checksum(data):
    return sum(data) % 256 == 0


def parse_parameters(data):
    fields = []
    cursor = 0
    while cursor < len(data):
        if len(data) - cursor < 4:
            return None
        size = int.from_bytes(data[cursor:cursor + 2], "big")
        kind = int.from_bytes(data[cursor + 2:cursor + 4], "big")
        if size < 4 or cursor + size > len(data):
            return None
        fields.append((kind, bytes(data[cursor + 4:cursor + size])))
        cursor += size
    return fields


def format_layout(fields, message_id, nested=False):
    if fields is None:
        return "unparsed"
    descriptions = []
    for kind, value in fields:
        description = f"0x{kind:04x}:{len(value)}"
        if message_id == 0x1D01 and not nested and kind in (0x0A, 0x11, 0x14, 0x15, 0x18, 0x1E):
            subfields = parse_parameters(value)
            if subfields is not None:
                description += "[" + format_layout(subfields, message_id, True) + "]"
        elif message_id == 0x1D01 and not nested and kind in (0x06, 0x07) and len(value) % 2 == 0:
            ids = (int.from_bytes(value[i:i + 2], "big") for i in range(0, len(value), 2))
            description += "[" + ",".join(f"0x{item:04x}" for item in ids) + "]"
        descriptions.append(description)
    return ",".join(descriptions)


class LinkReader:
    def __init__(self):
        self.pending = bytearray()
        self.counts = collections.Counter()

    def add(self, data, direction):
        self.pending.extend(data)
        result = []
        while self.pending:
            marker = self.pending.find(b"\xff\x5a")
            if marker < 0:
                self.pending = self.pending[-1:] if self.pending[-1] == 0xFF else bytearray()
                break
            del self.pending[:marker]
            if len(self.pending) < 9:
                break
            total = int.from_bytes(self.pending[2:4], "big")
            if not 9 <= total <= 65535 or not valid_checksum(self.pending[:9]):
                del self.pending[0]
                self.counts["bad_header"] += 1
                continue
            if len(self.pending) < total:
                break
            if total > 9 and not valid_checksum(self.pending[9:total]):
                del self.pending[0]
                self.counts["bad_payload"] += 1
                continue
            control, sequence, acknowledgement, session = self.pending[4:8]
            payload_bytes = total - (10 if total > 9 else 9)
            message_id = None
            layout = None
            if (session == 10 and payload_bytes >= 6 and
                    self.pending[9:11] == b"\x40\x40"):
                message_id = int.from_bytes(self.pending[13:15], "big")
                layout = parse_parameters(self.pending[15:total - 1])
            result.append((direction, control, sequence, acknowledgement,
                           session, payload_bytes, message_id, layout))
            self.counts["valid"] += 1
            del self.pending[:total]
        return result


def summarize(path, limit, parameter_layout):
    output = subprocess.run(["btmon", "-r", path], check=True,
                            capture_output=True, text=True).stdout
    readers = {"phone->pi": LinkReader(), "pi->phone": LinkReader()}
    direction = None
    in_uih = False
    dlci = None
    length = None
    credits = False
    hex_bytes = bytearray()
    decoded = []

    def finish():
        nonlocal in_uih, dlci, length, credits, hex_bytes
        if in_uih and dlci == 6 and length is not None and direction:
            data = hex_bytes[1:] if credits else hex_bytes
            if len(data) >= length:
                decoded.extend(readers[direction].add(data[:length], direction))
        in_uih, dlci, length, credits, hex_bytes = False, None, None, False, bytearray()

    for line in output.splitlines():
        if line.startswith("> ACL Data RX:") or line.startswith("< ACL Data TX:"):
            finish()
            direction = "phone->pi" if line.startswith(">") else "pi->phone"
        elif line.startswith(("> ", "< ", "@ ", "= ")):
            finish()
            direction = None
        if "RFCOMM: Unnumbered Info with Header Check" in line:
            in_uih = True
        if not in_uih:
            continue
        if match := re.search(r"\bdlci 0x([0-9a-f]+)", line):
            dlci = int(match.group(1), 16)
        if match := re.search(r"\bLength: (\d+)", line):
            length = int(match.group(1))
        if "Credits:" in line:
            credits = True
        match = re.match(r"^        ([0-9a-f]{2}(?: [0-9a-f]{2}){0,15})\s{2,}", line)
        if match:
            for token in match.group(1).split():
                hex_bytes.append(int(token, 16))
    finish()
    for direction, control, sequence, acknowledgement, session, size, message_id, params in decoded[:limit]:
        message = f" message=0x{message_id:04x}" if message_id is not None else ""
        if parameter_layout and message_id is not None:
            if params is None:
                message += " params=unparsed"
            else:
                message += " params=" + format_layout(params, message_id)
        print(f"{direction} control=0x{control:02x} seq={sequence} "
              f"ack={acknowledgement} session={session} payload={size}{message}")
    print(f"valid={len(decoded)} "
          f"bad_header={sum(r.counts['bad_header'] for r in readers.values())} "
          f"bad_payload={sum(r.counts['bad_payload'] for r in readers.values())}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("capture")
    parser.add_argument("--limit", type=int, default=100)
    parser.add_argument("--parameter-layout", action="store_true",
                        help="print top-level parameter IDs and lengths, never values")
    args = parser.parse_args()
    summarize(args.capture, args.limit, args.parameter_layout)
