# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual C++ Controller software owner/codec, portable original-date consumer.

Darwin evaluates the actual C++ date; it claims no cross-language clock mapping.
Invalid dates/bytes below are explicit test injections, never production inputs.
"""
import copy
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from ambient_light_status import AmbientStatus, encode_settings, decode, PACKET_SIZE
BINARY = sys.argv.pop(1)
NONCE = b'n' * 32


def settings(**values):
    original = json.loads((ROOT / 'config/ambient-light.example.json').read_text())
    original.update(values)
    return original


class SoftwareOwnerTests(unittest.TestCase):
    def setUp(self):
        self.child = subprocess.Popen([BINARY], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE)
        self.raw = self.read()
        self.sampled = struct.unpack_from('<Q', self.raw, 48)[0]
        self.consumer = AmbientStatus()
        self.consumer.accept(self.raw, NONCE, self.sampled)
        self.identifier = 0

    def read(self):
        raw = b''
        deadline = time.monotonic() + 5
        while len(raw) < PACKET_SIZE:
            self.assertTrue(select.select([self.child.stdout], [], [], max(0, deadline - time.monotonic()))[0],
                            'Actual C++ owner did not publish')
            part = os.read(self.child.stdout.fileno(), PACKET_SIZE - len(raw))
            self.assertTrue(part, 'Actual C++ owner retired before publication')
            raw += part
        return raw

    def tearDown(self):
        self.child.stdin.close()
        try:
            self.assertEqual(self.child.wait(timeout=5), 0, self.child.stderr.read().decode())
        finally:
            if self.child.poll() is None:
                self.child.kill()
                self.child.wait(timeout=5)
            self.child.stdout.close()
            self.child.stderr.close()

    def change(self, policy, revision=None, date=None, mutation=None):
        self.identifier += 1
        revision = self.consumer.revision if revision is None else revision
        raw = bytearray(b'HUALS01\0' + NONCE + struct.pack('<3Q', revision, self.identifier,
                            self.sampled if date is None else date) + encode_settings(policy))
        if mutation:
            mutation(raw)
        self.child.stdin.write(raw)
        self.child.stdin.flush()
        packet = self.read()
        self.sampled = struct.unpack_from('<Q', packet, 48)[0]
        self.consumer.accept(packet, NONCE, self.sampled)
        return self.consumer.snapshot(self.sampled), packet

    def test_existing_controller_settings_preview_off_and_no_observed_authority(self):
        original = self.consumer.snapshot(self.sampled)
        self.assertTrue(original['available'])
        self.assertFalse(original['provider_available'])
        self.assertEqual(original['requested']['source'], 'disabled')
        policy = settings(enabled=True, selection='fixed', fixed_rgbw=[7, 19, 41, 0], night_policy='on')
        result, _ = self.change(policy)
        self.assertEqual(result['settings'], policy)
        self.assertEqual(result['requested']['value']['rgbw'], [7, 19, 41, 0])
        self.assertEqual(result['requested']['value']['brightness_permille'], 60)
        self.assertEqual(result['requested']['night_source'], 'policy')
        self.assertIsNone(result['current'])
        self.assertIsNone(result['command'])
        # No screen, saved profile, speed or generation is invented by this owner.
        for selection in ('screen', 'drive_mode', 'automatic'):
            result, _ = self.change(settings(enabled=True, selection=selection))
            self.assertIsNone(result['requested'])
        result, _ = self.change(settings(enabled=False))
        self.assertEqual(result['requested']['source'], 'disabled')
        self.assertIsNone(result['current'])

    def test_real_handler_invalid_stale_expired_future_atomic_and_replay_retirement(self):
        original = self.consumer.snapshot(self.sampled)['settings']
        result, packet = self.change(settings(), mutation=lambda raw: struct.pack_into('<H', raw, 64 + 8, 1001))
        self.assertEqual(self.consumer.result, 'invalid_settings')
        self.assertEqual(result['settings'], original)
        self.assertEqual(self.consumer.revision, 1)
        for date in (self.sampled - 1_000_000_000, self.sampled + 60_000_000_000):
            self.change(settings(), date=date)
            self.assertEqual(self.consumer.result, 'expired_action')
            self.assertEqual(self.consumer.revision, 1)
        self.change(settings(), revision=44)
        self.assertEqual(self.consumer.result, 'stale_revision')
        self.assertEqual(self.consumer.revision, 1)
        action = b'HUALS01\0' + NONCE + struct.pack('<3Q', 1, self.identifier, self.sampled) + encode_settings(settings())
        self.child.stdin.write(action)
        self.child.stdin.flush()
        self.assertEqual(os.read(self.child.stdout.fileno(), PACKET_SIZE), b'')
        self.assertEqual(self.child.wait(timeout=3), 0)

    def test_consumer_original_dates_expiry_replay_malformed_and_retirement(self):
        before = self.consumer.snapshot(self.sampled)
        for raw in (self.raw, self.raw[:-1], self.raw + b'x'):
            with self.assertRaises(ValueError):
                self.consumer.accept(raw, NONCE, self.sampled)
            self.assertEqual(self.consumer.snapshot(self.sampled), before)
        for offset, value in ((48, self.sampled + 1), (56, 0), (80, 2), (72, 20)):
            raw = bytearray(self.raw)
            struct.pack_into('<Q', raw, offset, value)
            with self.assertRaises(ValueError):
                decode(bytes(raw), NONCE, self.sampled)
        expired = self.consumer.snapshot(self.sampled + 1_000_000_000)
        self.assertFalse(expired['available'])
        self.assertIsNone(expired['settings'])
        self.assertIsNone(expired['requested'])
        self.assertEqual(expired['sampled_ns'], str(self.sampled))
        self.consumer.retire('owner_retired')
        self.assertEqual(self.consumer.snapshot(self.sampled)['state'], 'owner_retired')
        with self.assertRaises(ValueError):
            self.consumer.accept(self.raw, NONCE, self.sampled)

    def test_full_bound_settings_roundtrip_original_controller_and_strict_action_shape(self):
        maximum = settings(night_policy='schedule:21:37-06:23', mode_colours=[
            {'id': 'm' + str(i).zfill(63), 'rgbw': [i, 0, 0, 255]} for i in range(32)])
        result, _ = self.change(maximum)
        self.assertEqual(result['settings'], maximum)
        data = {'action': 'ambient_light_settings', 'revision': result['revision'], 'settings': maximum}
        raw, pending = self.consumer.prepare(data, self.sampled, NONCE)
        self.assertEqual(len(raw), 2432)
        self.consumer.sent(pending)
        with self.assertRaises(ValueError):
            self.consumer.prepare(data, self.sampled, NONCE)
        self.assertEqual(self.consumer.snapshot(self.sampled + 1_000_000_000)['software_action']['status'], 'timed_out')
        for invalid in (dict(maximum, extra=True), dict(maximum, enabled=1),
                        dict(maximum, mode_colours=[maximum['mode_colours'][0]] * 2),
                        dict(maximum, fixed_rgbw=[True, 0, 0, 0])):
            with self.assertRaises(ValueError):
                encode_settings(invalid)


if __name__ == '__main__':
    unittest.main()
