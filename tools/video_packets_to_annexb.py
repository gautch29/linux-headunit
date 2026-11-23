#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Convert a private UHVP1 H.264 or HEVC screen capture into an Annex B stream.

This diagnostic tool is independent of the live C++ decoder.
It validates each record and writes only to a newly created owner-only file.
"""

import argparse
import os
import stat
from pathlib import Path

MAX_CAPTURE = 8 * 1024 * 1024 + 1024
START_CODE = b"\x00\x00\x00\x01"


def avcc_parameters(data):
    if len(data) < 7 or data[0] != 1:
        raise ValueError("capture does not contain an AVC configuration")
    nal_length_size = (data[4] & 3) + 1
    pos = 6
    output = bytearray()
    for group_count in (data[5] & 31, None):
        if group_count is None:
            if pos >= len(data):
                raise ValueError("truncated AVC parameter list")
            group_count = data[pos]
            pos += 1
        for _ in range(group_count):
            if len(data) - pos < 2:
                raise ValueError("truncated AVC parameter length")
            length = int.from_bytes(data[pos:pos + 2], "big")
            pos += 2
            if length == 0 or len(data) - pos < length:
                raise ValueError("invalid AVC parameter length")
            output += START_CODE + data[pos:pos + length]
            pos += length
    return nal_length_size, output


def hevc_parameters(data):
    if len(data) < 23 or len(data) > 65536 or data[0] != 1:
        raise ValueError("invalid HEVC configuration size or version")
    if ((data[13] & 0xf0) != 0xf0 or (data[15] & 0xfc) != 0xfc or
            (data[16] & 0xfc) != 0xfc or (data[17] & 0xf8) != 0xf8 or
            (data[18] & 0xf8) != 0xf8 or (data[21] & 3) == 2):
        raise ValueError("invalid HEVC configuration flags")
    nal_length_size = (data[21] & 3) + 1
    pos = 23
    output = bytearray()
    found = set()
    for _ in range(data[22]):
        if len(data) - pos < 3:
            raise ValueError("truncated HEVC parameter array")
        flags = data[pos]
        kind = flags & 63
        count = int.from_bytes(data[pos + 1:pos + 3], "big")
        pos += 3
        if flags & 0x40 or kind in found or kind not in (32, 33, 34, 39, 40):
            raise ValueError("invalid HEVC parameter array type")
        if not 0 < count <= 512:
            raise ValueError("invalid HEVC parameter count")
        for _ in range(count):
            if len(data) - pos < 2:
                raise ValueError("truncated HEVC parameter length")
            length = int.from_bytes(data[pos:pos + 2], "big")
            pos += 2
            if length < 2 or len(data) - pos < length:
                raise ValueError("invalid HEVC parameter length")
            nal = data[pos:pos + length]
            if nal[0] & 0x80 or ((nal[0] >> 1) & 63) != kind or not nal[1] & 7:
                raise ValueError("invalid HEVC parameter NAL header")
            output += START_CODE + nal
            pos += length
        found.add(kind)
    if not {32, 33, 34} <= found or pos != len(data):
        raise ValueError("incomplete HEVC parameters or unexpected suffix")
    return nal_length_size, output


def configuration(data, codec):
    """Accept bare records or the bounded QuickTime forms used by the receiver."""
    if not data or len(data) > 131072:
        raise ValueError("invalid screen configuration size")
    parameters = avcc_parameters if codec == "h264" else hevc_parameters
    if data[0] == 1:
        return parameters(data)
    expected = b"avcC" if codec == "h264" else b"hvcC"
    entries = (b"avc1", b"avc3") if codec == "h264" else (b"hvc1", b"hev1")
    result = None
    count = 0

    def boxes(value, depth):
        nonlocal result, count
        if depth > 2:
            raise ValueError("nested video configuration limit exceeded")
        pos = 0
        while pos < len(value):
            if depth and value[pos:] == b"\x00" * 4:
                break
            count += 1
            if count > 32 or len(value) - pos < 8:
                raise ValueError("invalid video box count or header")
            size = int.from_bytes(value[pos:pos + 4], "big")
            kind = value[pos + 4:pos + 8]
            header = 8
            if size == 1:
                if len(value) - pos < 16:
                    raise ValueError("truncated extended video box")
                size = int.from_bytes(value[pos + 8:pos + 16], "big")
                header = 16
            elif size == 0:
                size = len(value) - pos
            if size < header or size > len(value) - pos:
                raise ValueError("invalid video box length")
            body = value[pos + header:pos + size]
            pos += size
            if kind == expected:
                if result is not None:
                    raise ValueError("multiple video configurations")
                result = parameters(body)
            elif kind in entries:
                if len(body) < 78:
                    raise ValueError("truncated video sample entry")
                boxes(body[78:], depth + 1)
            elif kind not in (b"pasp", b"colr", b"btrt", b"clap", b"fiel"):
                raise ValueError("unsupported video configuration box or codec mismatch")
    boxes(data, 0)
    if result is None:
        raise ValueError("screen configuration contains no codec record")
    return result


def access_unit(data, nal_length_size):
    output = bytearray()
    pos = 0
    while pos < len(data):
        if len(data) - pos < nal_length_size:
            raise ValueError("truncated video NAL length")
        length = int.from_bytes(data[pos:pos + nal_length_size], "big")
        pos += nal_length_size
        if length == 0 or len(data) - pos < length:
            raise ValueError("invalid video NAL length")
        output += START_CODE + data[pos:pos + length]
        pos += length
    return output


def convert(source, destination, codec="h264"):
    if codec not in ("h264", "hevc"):
        raise ValueError("unsupported video codec")
    source_fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(source_fd).st_mode):
            raise ValueError("capture must be a regular file")
        with os.fdopen(source_fd, "rb", closefd=False) as capture:
            raw = capture.read(MAX_CAPTURE + 1)
    finally:
        os.close(source_fd)
    if len(raw) > MAX_CAPTURE or raw[:5] != b"UHVP\x01":
        raise ValueError("invalid or oversized UHVP1 capture")
    cursor = 5
    nal_length_size = None
    config_count = frame_count = 0
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        while cursor < len(raw):
            if len(raw) - cursor < 13:
                raise ValueError("truncated UHVP1 record")
            kind = raw[cursor]
            size = int.from_bytes(raw[cursor + 9:cursor + 13], "little")
            cursor += 13
            if size > MAX_CAPTURE or len(raw) - cursor < size:
                raise ValueError("invalid UHVP1 record size")
            data = raw[cursor:cursor + size]
            cursor += size
            if kind == 1:
                nal_length_size, encoded = configuration(data, codec)
                config_count += 1
            elif kind == 0 and nal_length_size is not None:
                encoded = access_unit(data, nal_length_size)
                frame_count += 1
            else:
                raise ValueError("frame before configuration or unknown record")
            view = memoryview(encoded)
            while view:
                written = os.write(fd, view)
                if written <= 0:
                    raise OSError("video output write made no progress")
                view = view[written:]
        if config_count == 0:
            raise ValueError("capture contains no configuration")
    except BaseException:
        # Retire only the file this invocation created. Preserve a replacement
        # if another owner renamed evidence while conversion was failing.
        try:
            current = destination.lstat()
            created = os.fstat(fd)
            if (current.st_dev, current.st_ino) == (created.st_dev, created.st_ino):
                destination.unlink()
        except FileNotFoundError:
            pass
        raise
    finally:
        os.close(fd)
    return config_count, frame_count


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--codec", choices=("h264", "hevc"), default="h264")
    args = parser.parse_args()
    configs, frames = convert(args.source, args.destination, args.codec)
    print(f"converted {configs} configuration(s), {frames} frame(s)")
