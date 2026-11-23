#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Exercise the generated files through the production C++ validators."""
import copy
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from configure_vehicle import _bounded_input, compile_configuration, compile_profile, read_definition, native_configuration
from receiver_configuration import load_receiver_configuration

CHECKER = Path(sys.argv.pop(1)).resolve()


class ConfigurationTest(unittest.TestCase):
    def test_combined_optional_units_compile_and_original_profile_parser_coexists(self):
        for speed in ('kmh', 'mph'):
            for temperature in (None, 'celsius', 'fahrenheit'):
                for distance in (None, 'kilometers', 'miles'):
                    with self.subTest(speed=speed, temperature=temperature, distance=distance), tempfile.TemporaryDirectory() as directory:
                        root = Path(directory)
                        definition = self.example('generic-modes')
                        native = definition['nativeCluster']
                        native['speedUnit'] = speed
                        if temperature is not None:
                            native['temperatureUnit'] = temperature
                        if distance is not None:
                            native['distanceUnit'] = distance
                        generated = native_configuration(native)
                        output = compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                        self.assertEqual((output / 'native-cluster.conf').read_bytes(), generated)
                        for profile in (False, True):
                            raw = generated
                            if profile:
                                raw += b'drive_mode_profiles_file=/private/secret-profile-marker.json\n'
                            completed = subprocess.run([str(CHECKER), '--native-summary'], input=raw,
                                                       capture_output=True, timeout=3)
                            self.assertEqual(completed.returncode, 0, completed.stderr)
                            summary = json.loads(completed.stdout)
                            self.assertEqual(summary['speedUnit'], speed)
                            self.assertEqual(summary['temperatureUnit'], temperature or 'celsius')
                            self.assertEqual(summary['distanceUnit'], distance or 'kilometers')
                            self.assertEqual(len(summary), 11)
                            self.assertNotIn(b'secret-profile-marker', completed.stdout)

    def test_consumption_compiler_independent_unit_combinations_and_exact_summary(self):
        for speed in ('kmh', 'mph'):
            for temperature in (None, 'celsius', 'fahrenheit'):
                for distance in (None, 'kilometers', 'miles'):
                    for consumption in (None, 'liters_per_100km', 'us_mpg', 'imperial_mpg'):
                        with self.subTest(speed=speed, temperature=temperature, distance=distance,
                                          consumption=consumption), tempfile.TemporaryDirectory() as directory:
                            root = Path(directory)
                            definition = self.example('generic-modes')
                            native = definition['nativeCluster']
                            native['speedUnit'] = speed
                            if temperature is not None:
                                native['temperatureUnit'] = temperature
                            if distance is not None:
                                native['distanceUnit'] = distance
                            before = native_configuration(native)
                            if consumption is not None:
                                native['fuelConsumptionUnit'] = consumption
                            generated = native_configuration(native)
                            suffix = b'' if consumption is None else ('fuel_consumption_unit=' + consumption + '\n').encode()
                            self.assertEqual(generated, before + suffix)
                            output = compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                            self.assertEqual((output / 'native-cluster.conf').read_bytes(), generated)
                            for profile in (False, True):
                                raw = generated + (b'drive_mode_profiles_file=/private/secret-profile-marker.json\n' if profile else b'')
                                completed = subprocess.run([str(CHECKER), '--native-summary'], input=raw,
                                                           capture_output=True, timeout=3)
                                self.assertEqual(completed.returncode, 0, completed.stderr)
                                summary = json.loads(completed.stdout)
                                self.assertEqual(summary['fuelConsumptionUnit'], consumption or 'liters_per_100km')
                                self.assertEqual(summary['speedUnit'], speed)
                                self.assertEqual(summary['temperatureUnit'], temperature or 'celsius')
                                self.assertEqual(summary['distanceUnit'], distance or 'kilometers')
                                self.assertEqual(set(summary), {'speedUnit', 'temperatureUnit', 'distanceUnit', 'fuelConsumptionUnit', 'speedPrecision', 'pressureUnit', 'temperaturePrecision',
                                                               'speedMaximumKmh', 'rpmMaximum', 'rpmRedline', 'showRpm'})
                                self.assertNotIn(b'secret-profile-marker', completed.stdout)

    def test_invalid_consumption_never_publishes_and_actual_checker_retires_staging(self):
        for value in (None, False, 1, {}, [], '', 'mpg', 'USmpg', 'us_mpg\nunknown=1'):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                definition = self.example('generic-modes')
                definition['nativeCluster']['fuelConsumptionUnit'] = value
                with self.assertRaises(ValueError):
                    compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))
        fields = self.example('generic-modes')['nativeCluster']
        for required in fields:
            missing = dict(fields, fuelConsumptionUnit='us_mpg')
            del missing[required]
            with self.subTest(required=required), self.assertRaises(ValueError):
                native_configuration(missing)
        original = subprocess.run
        for suffix in ('fuel_consumption_unit=invalid\n', 'fuel_consumption_unit=us_mpg\nfuel_consumption_unit=us_mpg\n'):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                base = self.base(root)
                def corrupt_then_check(args, **kwargs):
                    path = Path(args[args.index('--native-cluster') + 1])
                    path.write_bytes(path.read_bytes() + suffix.encode())
                    return original(args, **kwargs)
                with patch('configure_vehicle.subprocess.run', side_effect=corrupt_then_check):
                    with self.assertRaises(ValueError):
                        compile_configuration(self.example('generic-modes'), base, root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))

    def test_pressure_compiler_preserves_old_bytes_and_all_independent_units(self):
        for pressure in (None, 'kpa', 'bar', 'psi'):
            with self.subTest(pressure=pressure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                definition = self.example('generic-modes')
                native = definition['nativeCluster']
                original = native_configuration(native)
                if pressure is not None:
                    native['pressureUnit'] = pressure
                generated = native_configuration(native)
                suffix = b'' if pressure is None else ('pressure_unit=' + pressure + '\n').encode()
                self.assertEqual(generated, original + suffix)
                output = compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                self.assertEqual((output / 'native-cluster.conf').read_bytes(), generated)
                for speed in ('kmh', 'mph'):
                    for temperature in ('celsius', 'fahrenheit'):
                        for distance in ('kilometers', 'miles'):
                            for fuel in ('liters_per_100km', 'us_mpg', 'imperial_mpg'):
                                for precision in ('whole', 'one_decimal'):
                                    combined = dict(native, speedUnit=speed, temperatureUnit=temperature,
                                                    distanceUnit=distance, fuelConsumptionUnit=fuel,
                                                    speedPrecision=precision)
                                    raw = native_configuration(combined) + b'drive_mode_profiles_file=/private/secret-profile-marker\n'
                                    result = subprocess.run([str(CHECKER), '--native-summary'], input=raw,
                                                            capture_output=True, timeout=10)
                                    self.assertEqual(result.returncode, 0, result.stderr)
                                    summary = json.loads(result.stdout)
                                    self.assertEqual(len(summary), 11)
                                    for key in ('speedUnit', 'temperatureUnit', 'distanceUnit', 'fuelConsumptionUnit', 'speedPrecision'):
                                        self.assertEqual(summary[key], combined[key])
                                    self.assertEqual(summary['pressureUnit'], pressure or 'kpa')
                                    self.assertNotIn(b'secret-profile-marker', result.stdout)

    def test_invalid_pressure_never_publishes_and_cpp_rejects_staging(self):
        for value in (None, False, True, 1, {}, [], '', 'kPa', 'Bar', 'PSI', 'psi\nunknown=1'):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                definition = self.example('generic-modes')
                definition['nativeCluster']['pressureUnit'] = value
                with self.assertRaises(ValueError):
                    compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))
        fields = self.example('generic-modes')['nativeCluster']
        for required in fields:
            missing = dict(fields, pressureUnit='bar')
            del missing[required]
            with self.subTest(required=required), self.assertRaises(ValueError):
                native_configuration(missing)
        original = subprocess.run
        for suffix in ('pressure_unit=invalid\n', 'pressure_unit=bar\npressure_unit=psi\n'):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                base = self.base(root)
                def corrupt_then_check(args, **kwargs):
                    path = Path(args[args.index('--native-cluster') + 1])
                    path.write_bytes(path.read_bytes() + suffix.encode())
                    return original(args, **kwargs)
                with patch('configure_vehicle.subprocess.run', side_effect=corrupt_then_check):
                    with self.assertRaises(ValueError):
                        compile_configuration(self.example('generic-modes'), base, root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))

    def test_precision_compiler_exact_bytes_and_actual_cpp_units(self):
        for speed in ('kmh', 'mph'):
            for precision in (None, 'whole', 'one_decimal'):
                with self.subTest(speed=speed, precision=precision), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    definition = self.example('generic-modes')
                    native = definition['nativeCluster']
                    native['speedUnit'] = speed
                    before = native_configuration(native)
                    if precision is not None:
                        native['speedPrecision'] = precision
                    generated = native_configuration(native)
                    suffix = b'' if precision is None else ('speed_precision=' + precision + '\n').encode()
                    self.assertEqual(generated, before + suffix)
                    output = compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                    self.assertEqual((output / 'native-cluster.conf').read_bytes(), generated)
                    for temperature in ('celsius', 'fahrenheit'):
                        for distance in ('kilometers', 'miles'):
                            for consumption in ('liters_per_100km', 'us_mpg', 'imperial_mpg'):
                                combined = dict(native, temperatureUnit=temperature, distanceUnit=distance,
                                                fuelConsumptionUnit=consumption)
                                raw = native_configuration(combined) + b'drive_mode_profiles_file=/private/secret-profile-marker\n'
                                result = subprocess.run([str(CHECKER), '--native-summary'], input=raw,
                                                        capture_output=True, timeout=10)
                                self.assertEqual(result.returncode, 0, result.stderr)
                                summary = json.loads(result.stdout)
                                self.assertEqual(len(summary), 11)
                                self.assertEqual(summary['speedPrecision'], precision or 'whole')
                                self.assertEqual(summary['speedUnit'], speed)
                                self.assertEqual(summary['temperatureUnit'], temperature)
                                self.assertEqual(summary['distanceUnit'], distance)
                                self.assertEqual(summary['fuelConsumptionUnit'], consumption)
                                self.assertNotIn(b'secret-profile-marker', result.stdout)

    def test_invalid_precision_never_publishes_and_cpp_rejects_staging(self):
        for value in (None, False, True, 1, {}, [], '', 'Whole', 'one-decimal', 'one_decimal\nunknown=1'):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                definition = self.example('generic-modes')
                definition['nativeCluster']['speedPrecision'] = value
                with self.assertRaises(ValueError):
                    compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))
        fields = self.example('generic-modes')['nativeCluster']
        for required in fields:
            missing = dict(fields, speedPrecision='one_decimal')
            del missing[required]
            with self.subTest(required=required), self.assertRaises(ValueError):
                native_configuration(missing)
        original = subprocess.run
        for suffix in ('speed_precision=invalid\n', 'speed_precision=one_decimal\nspeed_precision=whole\n'):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                base = self.base(root)
                def corrupt_then_check(args, **kwargs):
                    path = Path(args[args.index('--native-cluster') + 1])
                    path.write_bytes(path.read_bytes() + suffix.encode())
                    return original(args, **kwargs)
                with patch('configure_vehicle.subprocess.run', side_effect=corrupt_then_check):
                    with self.assertRaises(ValueError):
                        compile_configuration(self.example('generic-modes'), base, root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))

    def test_distance_option_preserves_original_bytes_and_independent_speed(self):
        expected = (b'version=1\nspeed_unit=kmh\nspeed_maximum_kmh=260\n'
                    b'rpm_maximum=8000\nrpm_redline=6500\nshow_rpm=true\n')
        fields = {'speedUnit': 'kmh', 'speedMaximumKmh': 260, 'rpmMaximum': 8000,
                  'rpmRedline': 6500, 'showRpm': True}
        self.assertEqual(native_configuration(fields), expected)
        for speed in ('kmh', 'mph'):
            for distance in (None, 'kilometers', 'miles'):
                with self.subTest(speed=speed, distance=distance), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    definition = self.example('generic-modes')
                    definition['nativeCluster'] = dict(fields, speedUnit=speed)
                    if distance is not None:
                        definition['nativeCluster']['distanceUnit'] = distance
                    output = compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                    suffix = b'' if distance is None else ('distance_unit=' + distance + '\n').encode()
                    self.assertEqual((output / 'native-cluster.conf').read_bytes(),
                                     expected.replace(b'speed_unit=kmh', ('speed_unit=' + speed).encode()) + suffix)

    def test_invalid_distance_optional_or_required_fields_never_publish(self):
        for value in (None, False, 1, {}, [], '', 'mi', 'Miles', 'kilometers\nunknown=1'):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                definition = self.example('generic-modes')
                definition['nativeCluster']['distanceUnit'] = value
                with self.assertRaises(ValueError):
                    compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))
        fields = self.example('generic-modes')['nativeCluster']
        for required in fields:
            missing = dict(fields, distanceUnit='miles')
            del missing[required]
            with self.subTest(required=required), self.assertRaises(ValueError):
                native_configuration(missing)
        with self.assertRaises(ValueError):
            native_configuration(dict(fields, distanceUnit='miles', unknown='value'))

    def test_actual_checker_distance_rejection_retires_bad_staging(self):
        original = subprocess.run
        for suffix in ('distance_unit=invalid\n', 'distance_unit=miles\ndistance_unit=miles\n'):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                base = self.base(root)
                def corrupt_then_check(args, **kwargs):
                    path = Path(args[args.index('--native-cluster') + 1])
                    path.write_bytes(path.read_bytes() + suffix.encode())
                    return original(args, **kwargs)
                with patch('configure_vehicle.subprocess.run', side_effect=corrupt_then_check):
                    with self.assertRaises(ValueError):
                        compile_configuration(self.example('generic-modes'), base, root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))

    def test_fifo_inputs_reject_without_blocking_or_publishing(self):
        script = ('import sys\n'
                  'from pathlib import Path\n'
                  'sys.path.insert(0, sys.argv[1])\n'
                  'from configure_vehicle import compile_configuration, read_definition\n'
                  'root = Path(sys.argv[2])\n'
                  'try:\n'
                  '    if sys.argv[3] == "definition":\n'
                  '        read_definition(root / "definition.json")\n'
                  '    else:\n'
                  '        definition = {"schemaVersion": 1, "vehicle": {"make": "Fixture", "model": "Fixture", "year": 2020}, "ultraSignals": []}\n'
                  '        compile_configuration(definition, root / "base-receiver.json", root / "new", root / "unused-checker")\n'
                  'except ValueError:\n'
                  '    pass\n'
                  'else:\n'
                  '    raise AssertionError("FIFO input was admitted")\n')
        for kind in ('definition', 'installed-ui'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.base(root)
                fifo = root / ('definition.json' if kind == 'definition' else 'base-ui.conf')
                fifo.unlink(missing_ok=True)
                os.mkfifo(fifo, 0o600)
                completed = subprocess.run([sys.executable, '-B', '-c', script,
                                            str(ROOT / 'tools'), str(root), kind],
                                           timeout=3, capture_output=True)
                self.assertEqual(completed.returncode, 0, completed.stderr.decode())
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))

    def test_pinned_inputs_preserve_explicit_links_and_reject_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'definition.json'
            raw = json.dumps(self.example('defender-1998')).encode()
            path.write_bytes(raw)
            link = root / 'linked.json'
            link.symlink_to(path)
            self.assertEqual(read_definition(link), self.example('defender-1998'))
            with self.assertRaises(ValueError):
                _bounded_input(root, 65536, 'fixture')
            original_read = os.read
            for replace in (False, True):
                path.write_bytes(raw)
                def read_then_change(fd, maximum):
                    content = original_read(fd, maximum)
                    if replace:
                        updated = root / 'replacement'
                        updated.write_bytes(raw)
                        os.replace(updated, path)
                    else:
                        path.write_bytes(raw + b' ')
                    return content
                with patch('configure_vehicle.os.read', side_effect=read_then_change):
                    with self.assertRaises(ValueError):
                        read_definition(link)

    def test_regular_inputs_complete_legal_partial_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            definition = self.example('defender-1998')
            path = root / 'definition.json'
            path.write_text(json.dumps(definition))
            ui = root / 'base-ui.conf'
            raw = b'assetDirectory=/private/signed-fixture\n'
            ui.write_bytes(raw)
            original_read = os.read
            with patch('configure_vehicle.os.read', side_effect=lambda fd, maximum: original_read(fd, min(maximum, 3))):
                self.assertEqual(read_definition(path), definition)
                self.assertEqual(_bounded_input(ui, 8192, 'installed-UI profile'), raw)

    def base(self, root):
        ui = root / 'base-ui.conf'
        ui.write_text('assetDirectory=/private/signed-fixture\nassetIdentifier=fixture\n'
                      'assetVersion=1\nvehicleProfile=/private/old-profile\nsimulateGauges=true\n')
        receiver = root / 'base-receiver.json'
        receiver.write_text(json.dumps({'display_config': 'displays.conf',
            'ultra_installed_ui': str(ui), 'ultra_negotiate': True, 'ultra_native_video': True,
            'device_model': 'IndependentHeadunit', 'manufacturer': 'Independent',
            'simulate_vehicle': True, 'vehicle_state_profile': 'bench'}))
        return receiver

    def example(self, name):
        return read_definition(ROOT / 'config' / ('vehicle-' + name + '.example.json'))

    def test_defender_omits_all_undeclared_features_and_preserves_device_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = self.base(root)
            original = base.read_bytes()
            output = compile_configuration(self.example('defender-1998'), base, root / 'new', CHECKER)
            profile = plistlib.loads((output / 'vehicle-profile.plist').read_bytes())
            self.assertEqual(profile['vehicleIdentity'],
                             {'make': 'Land Rover', 'model': 'Defender 110', 'year': 1998})
            self.assertEqual(profile['signals'], [])
            receiver = load_receiver_configuration(output / 'receiver.json')
            self.assertEqual(receiver['manufacturer'], 'Independent')
            self.assertEqual(receiver['device_model'], 'IndependentHeadunit')
            self.assertEqual(receiver['bin_dir'], CHECKER.parent)
            self.assertEqual(receiver['vehicle_state_backend'], 'none')
            self.assertEqual(receiver['vehicle_state_profile'], 'none')
            self.assertFalse(receiver['simulate_vehicle'])
            self.assertIn('simulateGauges=false\n', (output / 'installed-ui.conf').read_text())
            self.assertFalse(any(key in receiver for key in ('climate_config', 'car_settings_config', 'camera_state_config')))
            self.assertEqual(base.read_bytes(), original)
            self.assertEqual(os.stat(output).st_mode & 0o777, 0o700)
            for file in output.iterdir():
                self.assertEqual(os.stat(file).st_mode & 0o777, 0o600)
            with self.assertRaises(ValueError):
                compile_configuration(self.example('defender-1998'), base, output, CHECKER)
            with self.assertRaises(ValueError):
                compile_configuration(self.example('defender-1998'), base, root / 'bad\nname', CHECKER)
            self.assertFalse((root / 'bad\nname').exists())

    def test_modes_capabilities_and_output_graph_use_the_new_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            definition = self.example('generic-modes')
            definition['driveModes'].reverse()
            output = compile_configuration(definition, self.base(root), root / 'new', CHECKER)
            receiver = load_receiver_configuration(output / 'receiver.json')
            self.assertEqual(receiver['climate_config'], output / 'climate.json')
            self.assertEqual(receiver['vehicle_state_backend'], 'simulation')
            self.assertEqual(receiver['native_cluster_config'], output / 'native-cluster.conf')
            self.assertEqual(receiver['ultra_installed_ui'], output / 'installed-ui.conf')
            self.assertIn('vehicleProfile=' + str(output / 'vehicle-profile.plist'),
                          (output / 'installed-ui.conf').read_text())
            signals = plistlib.loads((output / 'vehicle-profile.plist').read_bytes())['signals']
            index = next(s for s in signals if s.get('vehicleField') == 'drive_mode')
            self.assertNotIn('fixedValue', index)
            self.assertEqual(index['vehicleStates'], {'sport': 0, 'comfort': 1})
            identifiers = next(s for s in signals if s['path'][-1] == 'identifiers')
            self.assertEqual(identifiers['fixedValue'], ['sport', 'GT'])
            self.assertEqual([s['iid'] for s in signals], list(range(3, 8)))

    def test_optional_native_temperature_units_use_existing_checker_and_graph(self):
        for unit in (None, 'celsius', 'fahrenheit'):
            with self.subTest(unit=unit), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                definition = self.example('generic-modes')
                native = definition['nativeCluster']
                if unit is not None:
                    native['temperatureUnit'] = unit
                output = compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                expected = ('version=1\nspeed_unit=' + native['speedUnit'] +
                            '\nspeed_maximum_kmh=' + str(native['speedMaximumKmh']) +
                            '\nrpm_maximum=' + str(native['rpmMaximum']) +
                            '\nrpm_redline=' + str(native['rpmRedline']) +
                            '\nshow_rpm=' + str(native['showRpm']).lower() + '\n')
                if unit is not None:
                    expected += 'temperature_unit=' + unit + '\n'
                self.assertEqual((output / 'native-cluster.conf').read_text(), expected)
                self.assertEqual(read_definition(output / 'vehicle-definition.json'), definition)
                config = output / 'native-cluster.conf'
                for extra in ('temperature_unit=kelvin\n',
                              'temperature_unit=celsius\ntemperature_unit=fahrenheit\n',
                              'unknown_unit=fahrenheit\n'):
                    config.write_text(expected + extra)
                    result = subprocess.run([str(CHECKER), '-', '-', '-', '--native-cluster', str(config)],
                                            capture_output=True, timeout=3)
                    self.assertNotEqual(result.returncode, 0)

    def test_invalid_temperature_units_never_publish_or_leave_staging(self):
        for unit in ('F', 'C', 'Fahrenheit', 'kelvin', '', 'fahrenheit ', None, True, 1, [], {}):
            with self.subTest(unit=unit), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                definition = self.example('generic-modes')
                definition['nativeCluster']['temperatureUnit'] = unit
                with self.assertRaises(ValueError):
                    compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'definition.json'
            path.write_text('{"nativeCluster":{"temperatureUnit":"celsius","temperatureUnit":"fahrenheit"}}')
            with self.assertRaises(ValueError):
                read_definition(path)
        definition = self.example('generic-modes')
        definition['nativeCluster']['temperatureUnit'] = 'fahrenheit'
        for key in ('speedUnit', 'speedMaximumKmh', 'rpmMaximum', 'rpmRedline', 'showRpm'):
            incomplete = copy.deepcopy(definition)
            del incomplete['nativeCluster'][key]
            with self.assertRaises(ValueError):
                compile_profile(incomplete)

    def test_native_parser_rejects_invalid_signals_and_capabilities_before_publish(self):
        for change in ('unknown-field', 'fixed-reading', 'duplicate-path', 'invalid-climate', 'invalid-native'):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                definition = self.example('generic-modes')
                if change == 'unknown-field':
                    definition['ultraSignals'][1]['vehicleField'] = 'invented_provider'
                elif change == 'fixed-reading':
                    definition['ultraSignals'][1]['fixedValue'] = 0
                elif change == 'duplicate-path':
                    definition['ultraSignals'].append(copy.deepcopy(definition['ultraSignals'][0]))
                elif change == 'invalid-climate':
                    definition['capabilities']['climate']['zones'][0]['temperature']['maximum'] = 10
                else:
                    definition['nativeCluster']['rpmRedline'] = 8000
                with self.assertRaises(ValueError):
                    compile_configuration(definition, self.base(root), root / 'new', CHECKER)
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))

    def test_rejects_ambiguous_or_unbounded_configuration(self):
        good = self.example('defender-1998')
        for change in ({'schemaVersion': True}, {'driveModes': []}, {'driveModes': None},
                       {'vehicle': {'make': 'Land Rover', 'model': 'Defender', 'year': True}},
                       {'capabilities': {'climate': {'version': 1}}},
                       {'capabilityBackend': 'hardware'}, {'unknown': 1},
                       {'ultraSignals': [{'path': ['driveState', 'driveMode', 'currentIndex'], 'kind': 'integer'}]},
                       {'ultraSignals': [{'iid': 3, 'path': ['a', 'b', 'c']}]},
                       {'ultraSignals': [{'path': None}]},
                       {'ultraSignals': [{}] * 513}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                compile_profile(dict(good, **change))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'definition.json'
            for raw in (b'{"schemaVersion":1,"schemaVersion":1}', b'{"value":NaN}', b'\xff', b' ' * 65537):
                path.write_bytes(raw)
                with self.assertRaises(ValueError):
                    read_definition(path)


if __name__ == '__main__':
    unittest.main()
