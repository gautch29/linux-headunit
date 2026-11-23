# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""First-party synthetic status fixtures, never phone/car/installed evidence.

Fixed 1,000,000,000 ns acquisitions belong to an explicitly simulated host-steady
domain, not to today's clock. The actual Manager fixture substitutes BlueZ and
uses no receiver child; the actual consumer supplies expiry/retirement models.
"""
from copy import deepcopy
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import support_status_summary as summary
from media_state import MediaObservations
from prephone_consumer_test import admitted, packet, START, AGE
import phone_management_test as manager_fixture

PRIVATE = 'PRIVATE-identity-path-credential-pairing-artwork-RAW'


def raw(value):
    return json.dumps(value, allow_nan=False).encode()


def fixture():
    return {
        'generation': 7, 'active_mode': 'ultra', 'fallback_running': False,
        'active': PRIVATE, 'phones': [{'name': PRIVATE, 'address': PRIVATE}],
        'error': PRIVATE, 'fallback_error': PRIVATE,
        'recovery': {'phone': PRIVATE, 'remainingAttempts': 0},
        'carplay': {'state': 'home', 'returnReady': True, 'nonce': PRIVATE},
        'audio': {'available': True, 'pending': True, 'route': PRIVATE,
                  'error': PRIVATE, 'requestedVolumePercent': 100, 'volumePercent': 0,
                  'output': {'sampledMonotonicNs': '1000000100', 'acceptedBlocks': '0',
                             'discontinuities': '0', 'underruns': '2', 'fresh': True,
                             'starved': False, 'private': PRIVATE}},
        'media_observations': {'version': 1, 'generation': 7, 'live': True,
                               'action': {'state': 'dispatched', 'error': PRIVATE},
                               'transportReport': {'source': PRIVATE},
                               'pcm': {'state': 'no-advance', 'route': PRIVATE,
                                       'sampledMonotonicNs': '1000000100', 'acceptedBlocks': '0',
                                       'previousSampledMonotonicNs': '1000000000',
                                       'acceptedBlocksDelta': '0'}},
        'prephone_vehicle': {
            'configured': True, 'version': 1, 'state': 'admitted', 'clockDomain': 'host-steady',
            'sourceGeneration': '3', 'sourceStartedNs': '999999999',
            'sourceSampledNs': '1000000100',
            'publisher': {'live': True, 'sequence': '9', 'copiedNs': '1000000200',
                          'hostBootId': PRIVATE, 'error': PRIVATE},
            'rows': [{'id': 'fuel_percent', 'availability': 'available', 'value': 0,
                      'label': PRIVATE, 'unit': PRIVATE, 'acquiredNs': '1000000000',
                      'maximumAgeNs': '500000000', 'origin': 'simulation'}]},
        'release': {'path': PRIVATE, 'release_id': PRIVATE}, 'future': PRIVATE}


# Independently specified public contract: no generation/date synthesis, no raw
# status copy. All actual numeric zeroes here remain zero rather than missing.
GOLDEN = {
    'schema_version': 1, 'evidence': 'offline_supplied_manager_status', 'current_authority': False,
    'manager': {'generation': 7, 'active_mode': 'ultra', 'fallback_running': False,
                'recovery': {'remainingAttempts': 0}, 'carplay': {'state': 'home', 'returnReady': True}},
    'audio': {'available': True, 'pending': True,
              'output': {'sampledMonotonicNs': '1000000100', 'acceptedBlocks': '0',
                         'discontinuities': '0', 'underruns': '2', 'fresh': True, 'starved': False}},
    'media_observations': {'version': 1, 'generation': 7, 'live': True,
                           'pcm': {'state': 'no-advance', 'sampledMonotonicNs': '1000000100',
                                   'acceptedBlocks': '0', 'previousSampledMonotonicNs': '1000000000',
                                   'acceptedBlocksDelta': '0'}},
    'prephone_vehicle': {'configured': True, 'version': 1, 'state': 'admitted',
                         'clockDomain': 'host-steady', 'sourceGeneration': '3',
                         'sourceStartedNs': '999999999', 'sourceSampledNs': '1000000100',
                         'publisher': {'live': True, 'sequence': '9', 'copiedNs': '1000000200'},
                         'rows': [{'id': 'fuel_percent', 'availability': 'available',
                                   'acquiredNs': '1000000000', 'maximumAgeNs': '500000000',
                                   'origin': 'simulation'}]}}


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name).resolve()
        self.folder.chmod(0o700)
        self.input = self.folder / 'input.json'
        self.output = self.folder / 'output.json'
        self.input.write_bytes(raw(fixture()))
        self.input.chmod(0o600)

    def cli(self, *args):
        return subprocess.run([sys.executable, '-B', str(ROOT / 'tools/support_status_summary.py'),
                               *map(str, args)], capture_output=True, timeout=5)

    def test_independent_golden(self):
        self.assertEqual(summary.project(raw(fixture())), GOLDEN)
        result = self.cli('--input', self.input, '--output', self.output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.output.read_bytes()), GOLDEN)
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o600)
        self.assertEqual(self.output.stat().st_nlink, 1)
        self.assertNotIn(PRIVATE.encode(), result.stdout + result.stderr + self.output.read_bytes())

    def test_missing_is_null_not_zero(self):
        value = summary.project(b'{}')
        self.assertIsNone(value['manager']['generation'])
        self.assertIsNone(value['manager']['recovery']['remainingAttempts'])
        self.assertIsNone(value['audio']['available'])
        self.assertTrue(all(v is None for v in value['audio']['output'].values()))
        self.assertEqual(value['prephone_vehicle']['rows'], [])

    def test_actual_manager_status_and_shutdown(self):
        manager = manager_fixture.Tests().quarantine_fixture(self.folder)
        try:
            initial = manager.request({'action': 'status'})
            before = summary.project(raw(initial))
            self.assertEqual(before['manager']['generation'], initial['generation'])
            self.assertEqual(before['manager']['carplay'], {'state': 'disconnected', 'returnReady': False})
            self.assertEqual(before['prephone_vehicle']['state'], 'disabled')
            manager.shutdown()
            after = summary.project(raw(manager.request({'action': 'status'})))
            self.assertFalse(after['manager']['carplay']['returnReady'])
            self.assertFalse(after['current_authority'])
        finally:
            manager.shutdown()

    def test_actual_consumer_original_dates_expiry_retirement(self):
        consumer = admitted(packet(values={'fuel_percent': (0, START, 'simulation')}))
        current = consumer.snapshot(START + 100)
        expired = consumer.snapshot(START + AGE)
        consumer.retire('existing-owner-A', 1, START + AGE + 1)
        retired = consumer.snapshot(START + AGE + 1)
        for model, state in ((current, 'available'), (expired, 'stale'), (retired, 'disconnected')):
            result = summary.project(raw({'prephone_vehicle': model}))['prephone_vehicle']
            self.assertEqual(result['sourceSampledNs'], str(START + 100))
            self.assertEqual(result['rows'][0]['acquiredNs'], str(START))
            self.assertEqual(result['rows'][0]['origin'], 'simulation')
            self.assertEqual(result['rows'][0]['availability'], state)
            self.assertNotIn('value', result['rows'][0])
        self.assertEqual(current['rows'][0]['value'], 0)
        self.assertIsNone(expired['rows'][0]['value'])

    def test_actual_media_owner_progress_stale_and_retirement(self):
        model = MediaObservations()
        audio = fixture()['audio']
        audio['route'] = 'a' * 32
        first = model.snapshot(7, True, {}, audio, 1000, 1000000100)
        audio = deepcopy(audio)
        audio['output'].update(sampledMonotonicNs='1000000200', acceptedBlocks='5')
        second = model.snapshot(7, True, {}, audio, 1000, 1000000200)
        audio['output']['fresh'] = False
        stale = model.snapshot(7, True, {}, audio, 3000, 3000000200)
        retired = model.snapshot(8, False, {}, audio, 3000, 3000000300)
        for original, state in ((first, 'baseline'), (second, 'progressing'),
                                (stale, 'stale'), (retired, 'unavailable')):
            result = summary.project(raw({'media_observations': original}))
            self.assertEqual(result['media_observations']['pcm']['state'], state)
            self.assertFalse(result['current_authority'])
        self.assertEqual(second['pcm']['acceptedBlocksDelta'], '5')

    def test_unknown_and_sensitive_fields_never_export(self):
        value = fixture()
        value['future'] = {'key': PRIVATE, 'path': '/private/' + PRIVATE, 'raw': [PRIVATE]}
        value['prephone_vehicle']['rows'].append({'id': PRIVATE, 'value': PRIVATE})
        self.assertEqual(summary.project(raw(value)), GOLDEN)
        for branch in ('manager', 'audio', 'media_observations', 'prephone_vehicle'):
            self.assertNotIn(PRIVATE, json.dumps(summary.project(raw(value))[branch]))

    def test_invalid_numeric_types_and_enums(self):
        for path, bad in ((('generation',), True), (('generation',), -1),
                          (('generation',), 1.0), (('active_mode',), PRIVATE),
                          (('carplay', 'returnReady'), 1),
                          (('recovery', 'remainingAttempts'), 4),
                          (('audio', 'output', 'acceptedBlocks'), True),
                          (('audio', 'output', 'acceptedBlocks'), '01'),
                          (('audio', 'output', 'underruns'), str(2**64)),
                          (('prephone_vehicle', 'clockDomain'), PRIVATE)):
            value = fixture()
            target = value
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = bad
            with self.subTest(path=path, bad=bad), self.assertRaises(ValueError):
                summary.project(raw(value))

    def test_malformed_duplicate_nonfinite_and_structural_bounds(self):
        cases = [b'', b'[]', b'{', b'\xff', b'{"generation":0,"generation":1}',
                 b'{"future":{"secret":0,"secret":1}}', b'{"future":NaN}',
                 b'{"future":Infinity}', b'{"future":-Infinity}', b'{"future":1e999}',
                 b'{"future":18446744073709551616}',
                 raw({'future': 'x' * 4097}), raw({'future': [None] * 257}),
                 raw({'future': {str(i): 0 for i in range(257)}}),
                 b'{"future":' + b'[' * 17 + b'0' + b']' * 17 + b'}',
                 b' ' * (summary.MAX_BYTES + 1),
                 raw({'future': [[0] * 256 for _ in range(33)]})]
        for case in cases:
            with self.subTest(size=len(case)), self.assertRaises((ValueError, UnicodeError)):
                summary.project(case)
        self.assertIsNotNone(summary.project(raw({'future': 'x' * 4096})))

    def test_wrong_shapes_duplicate_rows_and_unknown_versions(self):
        for value in ({'carplay': []}, {'audio': {'output': []}},
                      {'prephone_vehicle': {'rows': {}}},
                      {'prephone_vehicle': {'rows': [False]}},
                      {'media_observations': {'version': 2}},
                      {'prephone_vehicle': {'version': True}}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                summary.project(raw(value))
        value = fixture()
        value['prephone_vehicle']['rows'] *= 2
        with self.assertRaises(ValueError):
            summary.project(raw(value))

    def test_failure_atomic_and_private_errors(self):
        self.input.write_bytes(b'{"secret":"' + PRIVATE.encode() + b'","generation":true}')
        result = self.cli('--input', self.input, '--output', self.output)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.output.exists())
        self.assertEqual(sorted(p.name for p in self.folder.iterdir()), ['input.json'])
        self.assertNotIn(PRIVATE.encode(), result.stdout + result.stderr)
        result = self.cli('--input', self.folder / PRIVATE, '--output', self.output)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(PRIVATE.encode(), result.stdout + result.stderr)
        result = self.cli('--unknown-' + PRIVATE)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn(PRIVATE.encode(), result.stdout + result.stderr)

    def test_no_overwrite_original_existing_or_symlink(self):
        original = self.input.read_bytes()
        for output in (self.input, self.output):
            if output == self.output:
                output.symlink_to(self.input)
            result = self.cli('--input', self.input, '--output', output)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(self.input.read_bytes(), original)
        self.assertTrue(self.output.is_symlink())
        self.output.unlink()
        self.output.write_bytes(b'original')
        result = self.cli('--input', self.input, '--output', self.output)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.output.read_bytes(), b'original')
        self.assertFalse(list(self.folder.glob('.support-*')))

    def test_unsafe_file_types_links_permissions_and_parent(self):
        original = self.input.read_bytes()
        self.input.chmod(0o644)
        self.assertEqual(self.cli('--input', self.input, '--output', self.output).returncode, 1)
        self.input.chmod(0o600)
        linked = self.folder / 'linked'
        os.link(self.input, linked)
        self.assertEqual(self.cli('--input', self.input, '--output', self.output).returncode, 1)
        linked.unlink()
        linked.symlink_to(self.input)
        self.assertEqual(self.cli('--input', linked, '--output', self.output).returncode, 1)
        linked.unlink()
        os.mkfifo(linked)
        self.assertEqual(self.cli('--input', linked, '--output', self.output).returncode, 1)
        linked.unlink()
        self.folder.chmod(0o755)
        self.assertEqual(self.cli('--input', self.input, '--output', self.output).returncode, 1)
        self.folder.chmod(0o700)
        linked.symlink_to(self.folder, target_is_directory=True)
        self.assertEqual(self.cli('--input', self.input, '--output', linked / 'out').returncode, 1)
        self.assertEqual(self.input.read_bytes(), original)
        self.assertFalse(self.output.exists())

    def test_owner_and_inplace_mutation_refused(self):
        with patch('support_status_summary.os.getuid', return_value=os.getuid() + 1):
            with self.assertRaises(ValueError):
                summary.read_input(self.input)
            with self.assertRaises(ValueError):
                summary.write_output(self.output, GOLDEN)
        actual_stat = os.stat

        def changed(*args, **kwargs):
            self.input.write_bytes(b'{}')
            return actual_stat(*args, **kwargs)

        with patch('support_status_summary.os.stat', side_effect=changed):
            with self.assertRaises(ValueError):
                summary.read_input(self.input)
        self.assertFalse(self.output.exists())

    def test_write_failure_leaves_no_published_or_temporary_file(self):
        with patch('support_status_summary.os.fsync', side_effect=OSError(PRIVATE)):
            self.assertEqual(summary.main(['--input', str(self.input), '--output', str(self.output)]), 1)
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.folder.glob('.support-*')))


if __name__ == '__main__':
    unittest.main()
