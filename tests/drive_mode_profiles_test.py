#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import json
import os
from pathlib import Path
import sys
import tempfile
import subprocess
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
PROBE = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None
from drive_mode_profiles import DriveModeControl, decode, encode, read, write


def fixture():
    profile = {'layout': '8BC9ABB1-CB20-595E-92BF-D8F099BF3515', 'format': 'sport',
               'palette': 'Blue', 'accent': '99CCFF', 'wallpaper': 'phone', 'sportScreenAvailable': True}
    return {'scope': '44C9FA31EC:985', 'revision': 1, 'selected': None,
            'modes': [{'id': 'sport', 'name': 'Sport', 'base': dict(profile), 'profile': dict(profile)}]}


class Profiles(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name).resolve()
        self.root.chmod(0o700)
        self.path = self.root / 'drive-modes'
        self.owner = (os.getuid(), os.getgid())
        write(self.path, fixture(), self.owner)
        self.control = DriveModeControl(self.path)

    def tearDown(self):
        self.folder.cleanup()

    def test_roundtrip_and_unknown_selection(self):
        self.assertEqual(decode(encode(fixture())), fixture())
        for selected in ('unknown', 0, False):
            value = fixture()
            value['selected'] = selected
            with self.assertRaises(ValueError):
                encode(value)

    def test_atomic_override_selection_reset_and_restart(self):
        self.control.change('drive_mode_profile', {'action': 'drive_mode_profile', 'mode': 'sport',
                            'profile': {'accent': 'FF0000', 'wallpaper': 'red'}, 'revision': 1})
        self.control.change('drive_mode', {'action': 'drive_mode', 'mode': 'sport', 'revision': 2})
        state = DriveModeControl(self.path).snapshot()
        self.assertEqual(state['selected'], 'sport')
        self.assertEqual(state['modes'][0]['profile']['accent'], 'FF0000')
        self.assertEqual(state['modes'][0]['base']['accent'], '99CCFF')
        self.assertIsNone(state['current'])
        self.assertFalse(state['vehicleControlAvailable'])
        self.control.change('drive_mode_profile', {'action': 'drive_mode_profile', 'mode': 'sport',
                            'profile': None, 'revision': 3})
        self.assertEqual(read(self.path, self.owner)['modes'][0]['profile'], fixture()['modes'][0]['base'])
        self.control.change('drive_mode', {'action': 'drive_mode', 'mode': None, 'revision': 4})
        self.assertIsNone(read(self.path, self.owner)['selected'])

    def test_invalid_and_stale_changes_preserve_all_fields(self):
        before = self.path.read_bytes()
        for revision, profile in ((0, {}), (True, {}), (1, {'accent': 'purple'}),
                                  (1, {'sportScreenAvailable': 1}), (1, {'actuator': 'sport'}),
                                  (1, {'wallpaper': '/private/image.png'})):
            with self.assertRaises(ValueError):
                self.control.change('drive_mode_profile', {'action': 'drive_mode_profile', 'mode': 'sport',
                                    'profile': profile, 'revision': revision})
            self.assertEqual(self.path.read_bytes(), before)

    def test_failed_write_does_not_publish_intent(self):
        before = self.path.read_bytes()
        with patch('drive_mode_profiles.os.replace', side_effect=OSError('fixture failed rename')):
            with self.assertRaises(OSError):
                self.control.change('drive_mode', {'action': 'drive_mode', 'mode': 'sport', 'revision': 1})
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(list(self.root.iterdir()), [self.path])

    def test_private_file_rejection(self):
        self.path.chmod(0o644)
        self.assertTrue(self.control.snapshot()['error'])
        self.path.unlink()
        target = self.root / 'target'
        target.write_bytes(encode(fixture()))
        target.chmod(0o600)
        self.path.symlink_to(target)
        self.assertTrue(self.control.snapshot()['error'])
        self.path.unlink()
        os.mkfifo(self.path, 0o600)
        self.assertTrue(self.control.snapshot()['error'])

    def test_stale_foreign_status_never_grants_confirmation(self):
        status = self.root / 'drive-modes.ultra.status'
        for changed in ({'revision': 2}, {'scope': 'foreign:1'}, {'updatedMonotonicNs': 1}):
            from carplay_readiness import clock_ns
            value = dict(version=1, scope='44C9FA31EC:985', revision=1, updatedMonotonicNs=clock_ns(), **{})
            value.update(changed)
            status.write_text(json.dumps(value))
            status.chmod(0o600)
            self.assertIsNone(self.control.snapshot(True)['current'])

    def test_current_status_is_connection_bound_and_expires_original_acquisition(self):
        from carplay_readiness import clock_ns
        publisher = ('a' * 32, '7')
        status_path = self.root / 'drive-modes.ultra.status'
        now = clock_ns()
        status = dict(version=1, scope='44C9FA31EC:985', revision=1, source='vehicle', mode='sport',
                      phase='confirmed', error='', updatedMonotonicNs=now, vehicleGeneration=2,
                      observation={'acquiredSteadyNs': now, 'maximumAgeNs': 250_000_000, 'ageAtPublicationNs': 0,
                                   'clockDomain': 'host-steady', 'origin': 'simulation'},
                      nonce=publisher[0], connectionGeneration=publisher[1], sportScreenAvailable=True,
                      signedWallpaperSupported=False)
        status_path.write_text(json.dumps(status))
        status_path.chmod(0o600)
        self.assertEqual(self.control.snapshot(True, publisher)['current']['source'], 'vehicle')
        self.assertIsNone(self.control.snapshot(True, (publisher[0], '8'))['current'])
        with patch('drive_mode_profiles.clock_ns', return_value=now + 250_000_000):
            self.assertIsNone(self.control.snapshot(True, publisher)['current'])

    @unittest.skipUnless(PROBE, 'C++ status probe is supplied by CTest')
    def test_actual_cpp_status_interoperability_and_acquisition_expiry(self):
        value = fixture()
        value['selected'] = 'sport'
        write(self.path, value, self.owner)
        publisher = ('a' * 32, '7')
        for source in ('user', 'vehicle'):
            value['selected'] = 'sport' if source == 'user' else None
            write(self.path, value, self.owner)
            subprocess.run([str(PROBE), '--publish-status', str(self.path), source], check=True)
            current = self.control.snapshot(True, publisher)['current']
            self.assertIsNotNone(current, 'Actual C++ clock/schema/publisher was not admitted')
            self.assertEqual(current['source'], 'simulation' if source == 'vehicle' else source)
            if source == 'vehicle':
                observation = current['observation']
                self.assertEqual(observation['origin'], 'simulation')
                with patch('drive_mode_profiles.clock_ns', return_value=current['updatedMonotonicNs'] +
                           observation['maximumAgeNs'] - observation['ageAtPublicationNs']):
                    self.assertIsNone(self.control.snapshot(True, publisher)['current'])

    def test_malformed_wire(self):
        raw = encode(fixture())
        for bad in (raw[:-1], raw + raw, raw.replace(b'HUDP1', b'HUDP2'),
                    raw.replace(b'\t1\t', b'\t01\t'), raw.replace(b'\tsport\tSport', b'\tbad mode\tSport'),
                    raw.replace(b'\tBlue\t', b'\tBlue\x00\t'), raw * 100):
            with self.assertRaises((ValueError, UnicodeError)):
                decode(bad)


if __name__ == '__main__':
    unittest.main()
