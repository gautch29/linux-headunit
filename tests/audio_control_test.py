# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Offline private audio sidecars, route replacement and native action contracts."""
import ast
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from audio_control import AudioControl, MAX_SEQUENCE, validate_state

ROUTE, REPLACEMENT = 'a' * 32, 'b' * 32
NOW = 10_000_000_000


def state(**changes):
    value = {'version': 1, 'updatedMonotonicNs': str(NOW), 'available': True, 'route': ROUTE,
            'acceptedSequence': '0', 'appliedSequence': '0', 'requestedVolumePercent': 100,
            'requestedMuted': False, 'volumePercent': 100, 'muted': False, 'error': ''} | changes
    if 'volumePercent' in changes and 'requestedVolumePercent' not in changes:
        value['requestedVolumePercent'] = changes['volumePercent']
    if 'muted' in changes and 'requestedMuted' not in changes:
        value['requestedMuted'] = changes['muted']
    return value


def write(path, value):
    path.write_text(json.dumps(value)); path.chmod(0o600)


class AudioControlTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.owner = (os.getuid(), os.getgid())
        self.now = NOW
        self.audio = AudioControl(self.folder, self.owner, clock=lambda: self.now)

    def tearDown(self):
        self.temp.cleanup()

    def test_output_progress_keeps_owner_date_and_independent_freshness(self):
        observation = {'sampledMonotonicNs': str(NOW - 1_000_000_000),
                       'acceptedBlocks': str(MAX_SEQUENCE), 'discontinuities': '3',
                       'underruns': '2', 'starved': True}
        write(self.audio.state_path, state(output=observation))
        value = self.audio.snapshot()
        self.assertTrue(value['available'])
        self.assertTrue(value['output']['fresh'])
        self.assertEqual(value['output']['sampledMonotonicNs'], observation['sampledMonotonicNs'])
        self.now += 1
        value = self.audio.snapshot()
        self.assertTrue(value['available'])
        self.assertFalse(value['output']['fresh'])
        # A new receiver/file heartbeat cannot refresh an old worker observation.
        write(self.audio.state_path, state(updatedMonotonicNs=str(self.now), output=observation))
        self.assertFalse(self.audio.snapshot()['output']['fresh'])
        for changes in ({'sampledMonotonicNs': str(self.now + 1)}, {'sampledMonotonicNs': '0'},
                        {'acceptedBlocks': 1}, {'acceptedBlocks': str(MAX_SEQUENCE + 1)},
                        {'underruns': '01'}, {'starved': 1}, {'extra': 0}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate_state(state(updatedMonotonicNs=str(self.now),
                                     output=observation | changes), self.now)
        with self.assertRaises(ValueError):
            validate_state(state(available=False, route='', output=observation), NOW)
        with self.assertRaises(ValueError):
            validate_state(state(output={}), NOW)
        self.assertNotIn('output', validate_state(state(), NOW))

    def test_optional_timing_is_bounded_numeric_and_keeps_owner_freshness(self):
        timing = {'scheduledAheadNs': '70000000', 'maximumOwnerGapNs': '50000000',
                  'lastUnderrunOwnerGapNs': '40000000', 'lastUnderrunLateNs': '1000000',
                  'queuedBytes': '1920', 'lastUnderrunQueuedBytes': '3840'}
        observation = {'sampledMonotonicNs': str(NOW - 1_000_000_001),
                       'acceptedBlocks': '12', 'discontinuities': '1',
                       'underruns': '0', 'starved': False, 'timing': timing}
        value = validate_state(state(output=observation), NOW)
        self.assertEqual(value['output']['timing'], timing)
        self.assertFalse(value['output']['fresh'])
        for bad in (None, {}, timing | {'extra': '0'}, timing | {'scheduledAheadNs': 1},
                    timing | {'maximumOwnerGapNs': '01'}, timing | {'queuedBytes': -1},
                    timing | {'lastUnderrunLateNs': str(MAX_SEQUENCE + 1)}):
            with self.subTest(timing=bad), self.assertRaises(ValueError):
                validate_state(state(output=observation | {'timing': bad}), NOW)

    def test_pending_is_not_applied_and_original_dates_remain(self):
        write(self.audio.state_path, state(acceptedSequence='8', appliedSequence='8', volumePercent=65))
        request = self.audio.request(ROUTE, 70, True)
        self.assertEqual(request, {'version': 1, 'route': ROUTE, 'sequence': '9', 'volumePercent': 70, 'muted': True})
        value = self.audio.snapshot()
        self.assertTrue(value['pending']); self.assertEqual(value['volumePercent'], 65)
        self.assertFalse(value['muted']); self.assertEqual(value['acceptedSequence'], '8')
        self.assertEqual(value['pendingVolumePercent'], 70); self.assertTrue(value['pendingMuted'])
        self.assertEqual(value['updatedMonotonicNs'], str(NOW))
        write(self.audio.state_path, state(acceptedSequence='9', appliedSequence='8', volumePercent=65,
                                          requestedVolumePercent=70, requestedMuted=True))
        self.assertTrue(self.audio.snapshot()['pending'])
        write(self.audio.state_path, state(acceptedSequence='9', appliedSequence='9', volumePercent=70,
                                          muted=True, requestedVolumePercent=70, requestedMuted=True))
        applied = self.audio.snapshot()
        self.assertFalse(applied['pending']); self.assertEqual(applied['volumePercent'], 70)
        self.assertTrue(applied['muted']); self.assertEqual(applied['updatedMonotonicNs'], str(NOW))

    def test_requests_are_serial_and_monotonically_greater_than_local_and_receiver(self):
        write(self.audio.state_path, state(acceptedSequence='41', appliedSequence='41'))
        requests, errors = [], []
        def send():
            try:
                requests.append(self.audio.request(ROUTE, 30, False)['sequence'])
            except Exception as error:
                errors.append(error)
        workers = [threading.Thread(target=send) for _ in range(8)]
        for worker in workers: worker.start()
        for worker in workers: worker.join()
        self.assertFalse(errors)
        self.assertEqual(sorted(map(int, requests)), list(range(42, 50)))
        write(self.audio.state_path, state(acceptedSequence='55', appliedSequence='49'))
        self.assertEqual(self.audio.request(ROUTE, 31, True)['sequence'], '56')
        self.assertEqual(self.audio.control_path.stat().st_mode & 0o777, 0o600)

    def test_route_replacement_never_accepts_old_command_and_resets_pending(self):
        write(self.audio.state_path, state())
        self.audio.request(ROUTE, 35, True)
        previous = self.audio.control_path.read_bytes()
        write(self.audio.state_path, state(route=REPLACEMENT, volumePercent=100))
        with self.assertRaises(ValueError): self.audio.request(ROUTE, 90, False)
        self.assertEqual(self.audio.control_path.read_bytes(), previous)
        self.assertFalse(self.audio.snapshot()['pending'])
        self.assertEqual(self.audio.request(REPLACEMENT, 40, False)['sequence'], '1')

    def test_freshness_uses_monotonic_state_not_recent_file_modification(self):
        write(self.audio.state_path, state(updatedMonotonicNs=str(NOW - 3_000_000_001)))
        self.assertFalse(self.audio.snapshot()['available'])
        with self.assertRaises(ValueError): self.audio.request(ROUTE, 30, False)
        write(self.audio.state_path, state(updatedMonotonicNs=str(NOW + 1)))
        self.assertFalse(self.audio.snapshot()['available'])
        write(self.audio.state_path, state(updatedMonotonicNs=str(NOW - 3_000_000_000)))
        self.assertTrue(self.audio.snapshot()['available'])

    def test_unavailability_and_reset_do_not_delete_desired_preferences(self):
        write(self.audio.state_path, state())
        self.audio.request(ROUTE, 27, True)
        write(self.audio.state_path, state(available=False, route=''))
        self.assertFalse(self.audio.snapshot()['available'])
        write(self.audio.state_path, state())
        self.assertEqual(self.audio.request(ROUTE, 27, True)['sequence'], '2')
        self.audio.reset()
        self.assertFalse(self.audio.control_path.exists()); self.assertFalse(self.audio.state_path.exists())
        loaded = AudioControl(self.folder, self.owner)
        self.assertEqual(loaded.configuration()['audio_volume_percent'], 27)
        self.assertTrue(loaded.configuration()['audio_muted'])
        self.assertFalse(loaded.snapshot()['available'])

    def test_exact_json_schema_numbers_and_printable_bounded_errors(self):
        for changes in ({'version': True}, {'version': 2}, {'route': ROUTE.upper()}, {'route': ROUTE[:-1]},
                        {'acceptedSequence': 3}, {'acceptedSequence': '01'}, {'acceptedSequence': '-1'},
                        {'acceptedSequence': str(MAX_SEQUENCE + 1)}, {'appliedSequence': '1'},
                        {'updatedMonotonicNs': '0'}, {'available': 1}, {'volumePercent': True},
                        {'volumePercent': 101}, {'requestedVolumePercent': -1}, {'muted': 0},
                        {'requestedMuted': 'false'}, {'error': 'bad\nmessage'}, {'error': 'é'},
                        {'error': 'x' * 129}, {'extra': 'unknown'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate_state(state(**changes), NOW)
        with self.assertRaises(ValueError): validate_state(state(requestedVolumePercent=90), NOW)
        with self.assertRaises(ValueError): validate_state(state(requestedMuted=True), NOW)
        invalid = state(); invalid.pop('muted')
        with self.assertRaises(ValueError): validate_state(invalid, NOW)
        value = state(error='x' * 128)
        self.assertEqual(validate_state(value, NOW), value)

    def test_duplicate_unknown_oversize_and_private_file_guards(self):
        path = self.audio.state_path
        text = json.dumps(state())
        path.write_text(text[:-1] + ', "route": "' + REPLACEMENT + '"}'); path.chmod(0o600)
        self.assertFalse(self.audio.snapshot()['available'])
        path.write_text('x' * 2049)
        self.assertFalse(self.audio.snapshot()['available'])
        path.write_text('[' * 1000 + '0' + ']' * 1000)
        self.assertFalse(self.audio.snapshot()['available'])
        write(path, state()); path.chmod(0o644)
        self.assertFalse(self.audio.snapshot()['available'])
        path.chmod(0o400); self.assertFalse(self.audio.snapshot()['available'])
        path.chmod(0o600)
        write(path, state())
        self.folder.chmod(0o755)
        self.assertFalse(self.audio.snapshot()['available'])
        with self.assertRaises(ValueError): self.audio.request(ROUTE, 50, False)
        self.folder.chmod(0o700)
        self.assertTrue(self.audio.snapshot()['available'])
        linked = self.folder / 'linked.json'; os.link(path, linked)
        self.assertFalse(self.audio.snapshot()['available'])
        linked.unlink(); path.unlink()
        target = self.folder / 'target.json'; write(target, state()); path.symlink_to(target)
        self.assertFalse(self.audio.snapshot()['available'])
        with self.assertRaises(ValueError): self.audio.request(ROUTE, 50, False)
        path.unlink(); os.mkfifo(path, 0o600)
        self.assertFalse(self.audio.snapshot()['available']) # Nonblocking open cannot hang on a FIFO.

    def test_invalid_preferences_owner_and_sequence_exhaustion(self):
        self.folder.chmod(0o755)
        with self.assertRaises(ValueError): AudioControl(self.folder, self.owner)
        self.folder.chmod(0o700)
        with self.assertRaises(ValueError): AudioControl(self.folder, None)
        with self.assertRaises(ValueError): AudioControl(self.folder, (os.getuid() + 1, os.getgid()))
        write(self.audio.preference_path, {'version': 1, 'volumePercent': 5, 'muted': False, 'extra': 1})
        with self.assertRaises(ValueError): AudioControl(self.folder, self.owner)
        self.audio.preference_path.unlink()
        write(self.audio.state_path, state(acceptedSequence=str(MAX_SEQUENCE)))
        with self.assertRaises(ValueError): self.audio.request(ROUTE, 50, False)
        self.assertFalse(self.audio.control_path.exists())

    def test_native_action_is_separate_from_bluetooth_and_retains_route(self):
        # Exercise the real method without importing GTK or contacting radios.
        module = ast.parse((Path(__file__).resolve().parents[1] / 'tools' / 'phone_page.py').read_text())
        definition = next(n for n in module.body if isinstance(n, ast.ClassDef) and n.name == 'PhonePage')
        method = next(n for n in definition.body if isinstance(n, ast.FunctionDef) and n.name == 'audio_action')
        scope = {}; exec(compile(ast.Module(body=[method], type_ignores=[]), 'phone_page.py', 'exec'), scope)
        class Page:
            status = {}
            def __init__(self): self.calls = []
            def action(self, value): self.calls.append(value)
        page = Page(); scope['audio_action'](page, 5)
        self.assertFalse(page.calls)
        page.status = {'active': 'AA:BB:CC:DD:EE:01', 'generation': 7,
                       'audio': state(volumePercent=60) | {'pending': True, 'pendingVolumePercent': 65, 'pendingMuted': True}}
        scope['audio_action'](page, 5)
        self.assertEqual(page.calls[-1], {'action': 'audio', 'phone': page.status['active'],
                                          'generation': 7, 'route': ROUTE, 'volumePercent': 70, 'muted': True})
        scope['audio_action'](page, toggle=True)
        self.assertFalse(page.calls[-1]['muted']); self.assertEqual(page.calls[-1]['volumePercent'], 65)
        page.status['audio'] = {'available': False}; scope['audio_action'](page, 5)
        self.assertEqual(len(page.calls), 2)


if __name__ == '__main__':
    unittest.main()
