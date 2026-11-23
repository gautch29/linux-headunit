#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Qualify fixed artwork and observed mappings without claiming car acquisition."""
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    'cluster_bench_profile', ROOT / 'tools/create_cluster_bench_profile.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


def by_path(profile):
    return {'.'.join(item['path']): item for item in profile['signals']}


class ClusterBenchProfileTest(unittest.TestCase):
    def test_greeting_is_session_owned_and_never_fixed_pairing(self):
        values = by_path(generator.profile(session_greeting=True))
        for name, owner in [('uiSceneState', 'scene_state'), ('uiSceneOptions', 'scene_options')]:
            signal = values['uiControl.uiState.' + name]
            self.assertEqual(signal['sessionField'], owner)
            self.assertNotIn('fixedValue', signal)
            self.assertNotIn('vehicleField', signal)
        self.assertEqual(by_path(generator.profile())['uiControl.uiState.uiSceneState']['fixedValue'], 2)

    def test_default_does_not_claim_a_drive_mode(self):
        for extended in (False, True):
            profile = generator.profile(extended=extended)
            self.assertFalse(any(path.startswith('driveState.driveMode.')
                                 for path in by_path(profile)))

    def test_every_authored_fixture_has_exact_zero_based_selection(self):
        for index, identifier in enumerate(generator.DRIVE_MODE_IDENTIFIERS):
            with self.subTest(identifier=identifier):
                profile = generator.profile(extended=True, drive_mode=identifier)
                # Both accepted plist encodings preserve the immutable array,
                # scalar kinds and the exact locally allocated IID map.
                for fmt in (plistlib.FMT_XML, plistlib.FMT_BINARY):
                    decoded = plistlib.loads(plistlib.dumps(profile, fmt=fmt))
                    signals = by_path(decoded)
                    modes = signals['driveState.driveMode.identifiers']
                    self.assertEqual(modes['kind'], 'array')
                    self.assertEqual(modes['fixedValue'],
                                     list(generator.DRIVE_MODE_IDENTIFIERS))
                    self.assertNotIn('vehicleField', modes)
                    self.assertEqual(signals['driveState.driveMode.currentIndex']['fixedValue'], index)
                    self.assertIs(signals['driveState.driveMode.hidden']['fixedValue'], False)
                    self.assertEqual(len({item['iid'] for item in decoded['signals']}),
                                     len(decoded['signals']))

    def test_unknown_fixture_rejected(self):
        for identifier in ('Sport', '', 'normal', 'sport\x00'):
            with self.subTest(identifier=identifier):
                with self.assertRaises(ValueError):
                    generator.profile(drive_mode=identifier)

    def test_observed_transmission_has_no_fixed_or_inferred_state(self):
        paths = by_path(generator.profile(observed_transmission=True))
        selector = paths['driveState.transmissionStatus.transmissionMode']
        gear = paths['driveState.transmissionStatus.gearPosition']
        self.assertEqual(selector['vehicleField'], 'transmission.selector')
        self.assertEqual(gear['vehicleField'], 'transmission.gear')
        self.assertEqual(selector['vehicleStates']['reverse'], 82)
        self.assertEqual(gear['vehicleStates']['3'], 3)
        for item in (selector, gear):
            self.assertNotIn('fixedValue', item)
            self.assertNotIn('vehicleScale', item)
            self.assertNotIn('vehicleOffset', item)
            self.assertNotEqual(item['vehicleField'], 'transmission.gearbox_type')
        # Existing callers retain the explicit artwork fixture unless opted in.
        old = by_path(generator.profile())
        self.assertEqual(old['driveState.transmissionStatus.transmissionMode']['fixedValue'], 77)
        self.assertEqual(old['driveState.transmissionStatus.gearPosition']['fixedValue'], 3)

    def configuration(self):
        return {'schemaVersion': 1, 'modes': [
            {'id': 'comfort', 'assetIdentifier': 'GT'},
            {'id': 'sport', 'assetIdentifier': 'sport'}]}

    def test_observed_mode_has_configuration_but_no_active_measurement(self):
        configuration = self.configuration()
        configuration['modes'].reverse()
        profile = generator.profile(drive_mode_config=configuration)
        for fmt in (plistlib.FMT_XML, plistlib.FMT_BINARY):
            signals = by_path(plistlib.loads(plistlib.dumps(profile, fmt=fmt)))
            modes = signals['driveState.driveMode.identifiers']
            self.assertEqual(modes['fixedValue'], ['sport', 'GT'])
            self.assertNotIn('vehicleField', modes)
            selected = signals['driveState.driveMode.currentIndex']
            self.assertEqual(selected['vehicleField'], 'drive_mode')
            self.assertEqual(selected['vehicleStates'], {'sport': 0, 'comfort': 1})
            self.assertEqual(selected['kind'], 'integer')
            for key in ('fixedValue', 'vehicleScale', 'vehicleOffset'):
                self.assertNotIn(key, selected)
            self.assertIs(signals['driveState.driveMode.hidden']['fixedValue'], False)
        with self.assertRaises(ValueError):
            generator.profile(drive_mode='sport', drive_mode_config=configuration)
        # Configurations may explicitly name another pack's qualified artwork.
        configuration['modes'][0]['assetIdentifier'] = 'custom-unqualified-artwork'
        self.assertEqual(generator.drive_mode_configuration(configuration)[0][0],
                         'custom-unqualified-artwork')

    def test_strict_configuration_identifiers_and_bounds(self):
        bad_documents = [None, [], {}, {'schemaVersion': True, 'modes': []},
                         {'schemaVersion': 2, 'modes': []},
                         {'schemaVersion': 1, 'modes': []},
                         {'schemaVersion': 1, 'modes': {}, 'active': 'sport'}]
        for document in bad_documents:
            with self.subTest(document=document), self.assertRaises(ValueError):
                generator.drive_mode_configuration(document)
        for bad in ('', '1sport', 'sport+', 'sport.mode', 'sport mode', 'é', 's' * 65, 1, True):
            document = self.configuration()
            document['modes'][0]['id'] = bad
            with self.subTest(identifier=bad), self.assertRaises(ValueError):
                generator.drive_mode_configuration(document)
        for bad in ('', 'a' * 129, 'é' * 65, '\x00', 'a\n', '\x7f', '\x80', '\ud800', 1, True):
            document = self.configuration()
            document['modes'][0]['assetIdentifier'] = bad
            with self.subTest(asset=bad), self.assertRaises(ValueError):
                generator.drive_mode_configuration(document)
        for key in ('id', 'assetIdentifier'):
            document = self.configuration()
            document['modes'][1][key] = document['modes'][0][key]
            with self.assertRaises(ValueError):
                generator.drive_mode_configuration(document)
        for extra in ('label', 'index', 'active'):
            document = self.configuration()
            document['modes'][0][extra] = 0
            with self.assertRaises(ValueError):
                generator.drive_mode_configuration(document)
        document = {'schemaVersion': 1, 'modes': [
            {'id': f'm{i}', 'assetIdentifier': f'art{i}'} for i in range(32)]}
        self.assertEqual(len(generator.drive_mode_configuration(document)[0]), 32)
        document['modes'].append({'id': 'extra', 'assetIdentifier': 'extra'})
        with self.assertRaises(ValueError):
            generator.drive_mode_configuration(document)
        document = self.configuration()
        document['modes'][0] = {'id': 's' * 64, 'assetIdentifier': 'é' * 64}
        self.assertEqual(generator.drive_mode_configuration(document)[0][0], 'é' * 64)

    def test_exact_fixed_array_encoded_boundary(self):
        # Eight long strings: one array marker, eight two-byte string prefixes.
        document = {'schemaVersion': 1, 'modes': [
            {'id': f'm{i}', 'assetIdentifier': str(i) + 'x' * (125 if i < 7 else 124)}
            for i in range(8)]}
        self.assertEqual(len(generator.drive_mode_configuration(document)[0]), 8)
        document['modes'][-1]['assetIdentifier'] += 'x'
        with self.assertRaises(ValueError):
            generator.drive_mode_configuration(document)

    def test_json_and_cli_configuration_are_strict_and_private(self):
        with tempfile.TemporaryDirectory() as folder:
            path, output = Path(folder) / 'modes.json', Path(folder) / 'observed.plist'
            for invalid in (b'', b'x' * 16385, b'\xff', b'{',
                            b'{"schemaVersion":1,"schemaVersion":1,"modes":[]}',
                            b'{"schemaVersion":NaN,"modes":[]}',
                            b'{"schemaVersion":1,"modes":[{"id":"sport","id":"comfort","assetIdentifier":"GT"}]}'):
                path.write_bytes(invalid)
                with self.subTest(document=invalid[:80]), self.assertRaises(ValueError):
                    generator.read_drive_mode_configuration(path)
            configuration = self.configuration()
            path.write_text(json.dumps(configuration))
            self.assertEqual(generator.read_drive_mode_configuration(path), configuration)
            command = [sys.executable, str(ROOT / 'tools/create_cluster_bench_profile.py'),
                       '--output', str(output), '--drive-mode-config', str(path)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            signal = by_path(plistlib.loads(output.read_bytes()))['driveState.driveMode.currentIndex']
            self.assertEqual(signal['vehicleStates'], {'comfort': 0, 'sport': 1})
            self.assertNotIn('fixedValue', signal)
            previous = output.read_bytes()
            path.write_text('{}')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), previous)
            result = subprocess.run(command + ['--drive-mode-fixture', 'sport'],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), previous)

    def test_observed_trip_has_only_inspected_paths_and_no_measurement(self):
        self.assertFalse(any(path.startswith('tripComputer.trip.') for path in by_path(generator.profile())))
        profile = generator.profile(observed_trip=True, trip_instance='journeyA', trip_label='Current Trip')
        for fmt in (plistlib.FMT_XML, plistlib.FMT_BINARY):
            signals = by_path(plistlib.loads(plistlib.dumps(profile, fmt=fmt)))
            label = signals['tripComputer.trip.userVisibleLabel']
            consumption = signals['tripComputer.trip.fuelEfficiency']
            self.assertEqual(label['fixedValue'], 'Current Trip')
            self.assertEqual(label['instance'], 'journeyA')
            self.assertEqual(consumption['instance'], label['instance'])
            self.assertEqual(consumption['vehicleField'], 'trip.fuel_efficiency_l_per_100km')
            self.assertEqual(consumption['vehicleScale'], 1000.0)
            self.assertEqual(consumption['kind'], 'real')
            self.assertEqual(consumption['units'], 674)
            self.assertEqual(signals['vehicleUnits.displayUnits.fuelEfficiencyUnit']['fixedValue'], 671)
            for key in ('fixedValue', 'vehicleOffset', 'vehicleStates'):
                self.assertNotIn(key, consumption)
            self.assertEqual({path for path in signals if path.startswith('tripComputer.trip.')},
                             {'tripComputer.trip.userVisibleLabel', 'tripComputer.trip.fuelEfficiency'})
            self.assertFalse(any('heading' in item.get('vehicleField', '') for item in profile['signals']))
            self.assertEqual(signals['driveState.transmissionStatus.gearPosition']['fixedValue'], 3)

    def test_trip_instance_label_are_explicit_and_bounded(self):
        for key in ('trip_instance', 'trip_label'):
            with self.assertRaises(ValueError):
                generator.profile(**{key: 'custom'})
        for instance in ('', 'a.b', 'a/b', 'é', 'x' * 129, 1, True):
            with self.subTest(instance=instance), self.assertRaises(ValueError):
                generator.profile(observed_trip=True, trip_instance=instance)
        for label in ('', '\x00', 'a\n', '\x7f', '\x80', '\ud800', 'é' * 65, 1, True):
            with self.subTest(label=label), self.assertRaises(ValueError):
                generator.profile(observed_trip=True, trip_label=label)
        valid = by_path(generator.profile(observed_trip=True, trip_instance='x' * 128, trip_label='é' * 64))
        self.assertEqual(valid['tripComputer.trip.userVisibleLabel']['fixedValue'], 'é' * 64)
        default = by_path(generator.profile(observed_trip=True))
        self.assertEqual(default['tripComputer.trip.userVisibleLabel']['instance'], 'currentTrip')
        self.assertEqual(default['tripComputer.trip.userVisibleLabel']['fixedValue'], 'Current Trip')

    def test_cli_trip_options_cannot_supply_a_fixed_consumption(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'trip.plist'
            command = [sys.executable, str(ROOT / 'tools/create_cluster_bench_profile.py'),
                       '--output', str(output), '--observed-trip', '--trip-instance', 'tripA', '--trip-label', 'Trip A']
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            signals = by_path(plistlib.loads(output.read_bytes()))
            self.assertEqual(signals['tripComputer.trip.userVisibleLabel']['fixedValue'], 'Trip A')
            self.assertNotIn('fixedValue', signals['tripComputer.trip.fuelEfficiency'])
            previous = output.read_bytes()
            result = subprocess.run([item for item in command if item != '--observed-trip'], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), previous)

    def test_cli_fixture_is_explicit_and_private(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'fixture.plist'
            command = [sys.executable, str(ROOT / 'tools/create_cluster_bench_profile.py'),
                       '--output', str(output), '--drive-mode-fixture', 'sport']
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            signals = by_path(plistlib.loads(output.read_bytes()))
            self.assertEqual(signals['driveState.driveMode.currentIndex']['fixedValue'], 1)
            result = subprocess.run(command[:-1] + ['unknown'], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
