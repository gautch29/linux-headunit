# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual C++ cache/model/binary codec with explicit supplied portable dates.

No portable clock mapping or live-kernel proof is claimed by injected dates.
"""
from copy import deepcopy
import json
from pathlib import Path
import struct
import subprocess
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prephone_indicator_status import IndicatorStatus, decode, PACKET_SIZE, ROWS
from prephone_consumer import AGE_MAX, DATE_MAX

BINARY = sys.argv.pop(1)
NONCE = b'n' * 32


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        raw = subprocess.check_output([BINARY, '--packets'], timeout=5)
        assert len(raw) == 8 * PACKET_SIZE
        cls.packets = [raw[at:at + PACKET_SIZE] for at in range(0, len(raw), PACKET_SIZE)]
        cls.now = struct.unpack_from('<Q', cls.packets[0], 48)[0]

    def ready(self):
        status = IndicatorStatus()
        status.accept(self.packets[0], NONCE, self.now)
        return status

    def test_catalogue_canonical_states_dated_absence_and_legacy_null(self):
        model = self.ready().snapshot(self.now)
        self.assertEqual([(row['id'], row['label'], row['color']) for row in model['rows']], list(ROWS))
        catalogue = json.loads(subprocess.check_output([BINARY, '--catalogue'], timeout=5))
        self.assertEqual([(row['id'], row['label'], row['color']) for row in catalogue['rows']], list(ROWS))
        self.assertEqual([row['value'] for row in model['rows'][:7]],
                         ['FLASHING', 'OFF', None, 'ON', 'OFF', None, None])
        self.assertEqual(model['rows'][5]['availability'], 'missing')
        self.assertEqual(model['rows'][6]['availability'], 'unavailable')
        self.assertEqual(model['rows'][6]['origin'], 'simulation')
        self.assertIsNone(model['rows'][5]['acquiredNs'])
        for index, expected in ((5, 'awaiting_source'), (6, 'invalid_snapshot')):
            status = IndicatorStatus()
            copied = struct.unpack_from('<Q', self.packets[index], 48)[0]
            status.accept(self.packets[index], NONCE, copied)
            result = status.snapshot(copied)
            self.assertEqual(result['state'], expected)
            self.assertIsNone(result['sourceGeneration'])
            self.assertTrue(all(row['value'] is None and row['acquiredNs'] is None for row in result['rows']))
        self.assertIsNone(decode(self.packets[6], NONCE, copied)[2]['clockDomain'])

    def test_copies_phase_independent_original_expiry_and_deepcopies(self):
        status = self.ready()
        first = status.snapshot(self.now)
        status.accept(self.packets[1], NONCE, self.now + 200_000_000)
        later = status.snapshot(self.now + 499_999_999)
        self.assertEqual(later['rows'][0]['value'], 'FLASHING')
        self.assertEqual(later['sourceSampledNs'], first['sourceSampledNs'])
        self.assertEqual(later['rows'][0]['acquiredNs'], first['rows'][0]['acquiredNs'])
        self.assertGreater(int(later['copiedNs']), int(first['copiedNs']))
        expired = status.snapshot(self.now + 500_000_000)
        self.assertEqual(expired['rows'][0]['availability'], 'stale')
        self.assertIsNone(expired['rows'][0]['value'])
        self.assertEqual(expired['rows'][1]['value'], 'OFF')
        self.assertEqual(expired['rows'][0]['ageNs'], '500000000')
        expired['rows'][1]['value'] = 'ON'
        self.assertEqual(status.snapshot(self.now + 500_000_000)['rows'][1]['value'], 'OFF')
        status.accept(self.packets[2], NONCE, self.now + 500_000_000)
        with self.assertRaises(ValueError):
            status.accept(self.packets[1], NONCE, self.now + 500_000_000)

    def test_withdrawal_terminal_new_generation_and_cross_packet_scope(self):
        status = self.ready()
        status.accept(self.packets[3], NONCE, self.now + 600_000_000)
        withdrawn = status.snapshot(self.now + 600_000_000)
        self.assertTrue(all(row['value'] is None for row in withdrawn['rows']))
        self.assertEqual(withdrawn['rows'][1]['acquiredNs'], str(self.now))
        status.accept(self.packets[4], NONCE, self.now + 700_000_000)
        new = status.snapshot(self.now + 700_000_000)
        self.assertEqual(new['sourceGeneration'], '8')
        self.assertTrue(all(row['acquiredNs'] is None for row in new['rows']))
        old = self.ready()
        boundary = old.snapshot(self.now + 200_000_000, new)
        self.assertEqual(boundary['state'], 'generation_mismatch')
        self.assertEqual(boundary['sourceGeneration'], '7')
        self.assertEqual(boundary['rows'][1]['acquiredNs'], str(self.now))
        self.assertTrue(all(row['value'] is None for row in boundary['rows']))
        with self.assertRaises(ValueError):
            old.accept(self.packets[1], NONCE, self.now + 200_000_000)
        old.accept(self.packets[4], NONCE, self.now + 700_000_000)
        terminal = IndicatorStatus()
        terminal.accept(self.packets[7], NONCE, self.now + 700_000_000)
        self.assertEqual(terminal.snapshot(self.now + 700_000_000)['state'], 'disconnected')
        terminal.retire('owner_retired')
        self.assertTrue(all(row['value'] is None for row in terminal.snapshot(self.now + 700_000_000)['rows']))
        with self.assertRaises(ValueError):
            terminal.accept(self.packets[4], NONCE, self.now + 700_000_000)

    def test_all_binary_bounds_types_reserved_clock_nonce_and_atomic_last_row(self):
        changes = [(0, '<B', 0), (40, '<Q', 0), (48, '<Q', self.now + 1),
                   (56, '<Q', 6), (64, '<Q', 0), (72, '<Q', self.now + 1),
                   (80, '<Q', self.now + 1), (88, '<H', 2), (90, '<H', 23),
                   (92, '<I', PACKET_SIZE - 1), (96, '<B', 0), (97, '<B', 1),
                   (104, '<B', 5), (105, '<B', 255), (106, '<B', 3), (107, '<B', 1),
                   (108, '<B', 1), (120, '<Q', self.now - 1), (128, '<Q', 0),
                   (128, '<Q', AGE_MAX + 1), (104 + 23 * 32 + 4, '<B', 1)]
        for offset, fmt, value in changes:
            with self.subTest(offset=offset, value=value):
                bad = bytearray(self.packets[0])
                struct.pack_into(fmt, bad, offset, value)
                status = IndicatorStatus()
                before = deepcopy(status.__dict__)
                with self.assertRaises(ValueError):
                    status.accept(bytes(bad), NONCE, self.now)
                self.assertEqual(status.__dict__, before)
        for raw in (b'', self.packets[0][:-1], self.packets[0] + b'x', bytearray(self.packets[0])):
            with self.assertRaises(ValueError):
                decode(raw, NONCE, self.now)
        for nonce in (b'x' * 32, b'n' * 31, bytearray(NONCE)):
            with self.assertRaises(ValueError):
                decode(self.packets[0], nonce, self.now)
        for now in (0, True, float(self.now), self.now - 1, DATE_MAX + 1):
            with self.assertRaises(ValueError):
                decode(self.packets[0], NONCE, now)

    def test_independent_sequence_original_history_immutable_age_and_atomic_recovery(self):
        for offset, fmt, value in ((136 + 2, '<B', 1), (136 + 1, '<B', 0),
                                   (136 + 24, '<Q', 11_000_000_000),
                                   (136 + 16, '<Q', self.now - 1), (48, '<Q', self.now - 1)):
            status = self.ready()
            before = deepcopy(status.__dict__)
            bad = bytearray(self.packets[1])
            struct.pack_into(fmt, bad, offset, value)
            with self.assertRaises(ValueError):
                status.accept(bytes(bad), NONCE, self.now + 200_000_000)
            self.assertEqual(status.__dict__, before)
            status.accept(self.packets[1], NONCE, self.now + 200_000_000)
        # A later copy cannot reanimate a snapshot already retired by local expiry.
        status = self.ready()
        status.snapshot(self.now + 500_000_000)
        bad = bytearray(self.packets[1])
        with self.assertRaises(ValueError):
            status.accept(bytes(bad), NONCE, self.now + 500_000_000)
        self.assertEqual(status.sequence, 1)
        with self.assertRaises(ValueError):
            status.snapshot(self.now)
        failures = []
        def wrong_owner():
            try:
                status.snapshot(self.now + 500_000_000)
            except ValueError:
                failures.append(True)
        child = threading.Thread(target=wrong_owner)
        child.start()
        child.join()
        self.assertEqual(failures, [True])


if __name__ == '__main__':
    unittest.main()
