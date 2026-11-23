#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Synthetic captures only: codec conversion, framing rejection and private output."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import video_packets_to_annexb as converter

AVC = bytes.fromhex("0142001effe100046742001e0100026880")
HEVC = bytearray(23)
HEVC[0] = 1
HEVC[13] = 0xf0
HEVC[15] = HEVC[16] = 0xfc
HEVC[17] = HEVC[18] = 0xf8
HEVC[21] = 3
HEVC[22] = 3
for kind in (32, 33, 34):
    HEVC += bytes((0x80 | kind, 0, 1, 0, 2, kind << 1, 1))
HEVC = bytes(HEVC)


def box(kind, body, extended=False):
    if extended:
        return struct.pack(">I4sQ", 1, kind, len(body) + 16) + body
    return struct.pack(">I4s", len(body) + 8, kind) + body


def record(kind, data):
    return struct.pack("<BQI", kind, 123456, len(data)) + data


class CaptureConversion(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "capture.packets"
        self.output = self.root / "private.annexb"

    def tearDown(self):
        self.temporary.cleanup()

    def convert(self, config, frame, codec):
        self.source.write_bytes(b"UHVP\x01" + record(1, config) + record(0, frame))
        return converter.convert(self.source, self.output, codec)

    def test_both_codecs_convert_parameters_and_two_nals(self):
        for codec, config, parameters, nal in (
            ("h264", AVC, [b"\x67\x42\x00\x1e", b"\x68\x80"], b"\x65\x80"),
            ("hevc", HEVC, [b"\x40\x01", b"\x42\x01", b"\x44\x01"], b"\x26\x01"),
        ):
            for width in (1, 2, 4):
                with self.subTest(codec=codec, width=width):
                    config = bytearray(config)
                    config[4 if codec == "h264" else 21] = (config[4 if codec == "h264" else 21] & 0xfc) | (width - 1)
                    frame = (len(nal).to_bytes(width, "big") + nal) * 2
                    self.assertEqual(self.convert(bytes(config), frame, codec), (1, 1))
                    expected = b"".join(converter.START_CODE + n for n in parameters + [nal, nal])
                    self.assertEqual(self.output.read_bytes(), expected)
                    self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)
                    self.output.unlink()

    def test_quicktime_forms_and_reserved_zero_size(self):
        for codec, config, kind, entry in (("h264", AVC, b"avcC", b"avc1"), ("hevc", HEVC, b"hvcC", b"hvc1")):
            expected = converter.configuration(config, codec)
            direct = box(kind, config)
            zero = b"\x00" * 4 + direct[4:]
            sample = box(entry, bytes(78) + direct + box(b"pasp", bytes(8)) + bytes(4))
            for value in (direct, zero, box(kind, config, True), sample):
                self.assertEqual(converter.configuration(value, codec), expected)
            for value in (direct + direct, direct + bytes(4), sample[:-1], box(entry, bytes(77)), box(b"junk", direct)):
                with self.assertRaises(ValueError):
                    converter.configuration(value, codec)
        with self.assertRaises(ValueError):
            converter.configuration(box(b"hvcC", HEVC), "h264")

    def test_quicktime_box_count_and_depth_are_bounded(self):
        codec = box(b"hvcC", HEVC)
        with self.assertRaises(ValueError):
            converter.configuration(box(b"pasp", b"") * 32 + codec, "hevc")
        for _ in range(3):
            codec = box(b"hvc1", bytes(78) + codec)
        with self.assertRaises(ValueError):
            converter.configuration(codec, "hevc")

    def test_hevc_rejects_truncation_flags_duplicate_and_bad_nals(self):
        for size in range(len(HEVC)):
            with self.assertRaises(ValueError):
                converter.hevc_parameters(HEVC[:size])
        for index, value in ((13, 0), (21, 2), (22, 0), (23, 0x60), (24, 255), (28, 0), (29, 0)):
            malformed = bytearray(HEVC)
            malformed[index] = value
            with self.assertRaises(ValueError):
                converter.hevc_parameters(malformed)
        duplicate = bytearray(HEVC)
        duplicate[30] = duplicate[23]
        with self.assertRaises(ValueError):
            converter.hevc_parameters(duplicate)
        with self.assertRaises(ValueError):
            converter.hevc_parameters(HEVC + b"suffix")

    def test_rejected_capture_retires_its_partial_output(self):
        valid = b"UHVP\x01" + record(1, AVC)
        for raw in (valid + b"x", valid + record(0, b"\x00\x00\x00\x04x"),
                    b"UHVP\x01" + record(0, b"x"), valid + record(2, b"x"), b"UHVP\x01"):
            self.source.write_bytes(raw)
            with self.assertRaises(ValueError):
                converter.convert(self.source, self.output)
            self.assertFalse(self.output.exists())

    def test_existing_output_is_preserved(self):
        self.output.write_bytes(b"retained private evidence")
        self.source.write_bytes(b"UHVP\x01" + record(1, AVC))
        with self.assertRaises(FileExistsError):
            converter.convert(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"retained private evidence")

    def test_failure_does_not_remove_replaced_evidence(self):
        self.source.write_bytes(b"UHVP\x01" + record(1, AVC))

        def fail_write(fd, data):
            self.output.unlink()
            self.output.write_bytes(b"replacement evidence")
            raise OSError("synthetic output failure")

        with patch.object(converter.os, "write", side_effect=fail_write):
            with self.assertRaises(OSError):
                converter.convert(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"replacement evidence")

    def test_source_is_bounded_regular_and_not_linked(self):
        with self.source.open("wb") as output:
            output.write(b"UHVP\x01")
            output.truncate(converter.MAX_CAPTURE + 1)
        with self.assertRaises(ValueError):
            converter.convert(self.source, self.output)
        self.assertFalse(self.output.exists())
        linked = self.root / "linked"
        linked.symlink_to(self.source)
        with self.assertRaises(OSError):
            converter.convert(linked, self.output)
        fifo = self.root / "fifo"
        os.mkfifo(fifo)
        with self.assertRaises(ValueError):
            converter.convert(fifo, self.output)


if __name__ == "__main__":
    unittest.main()
