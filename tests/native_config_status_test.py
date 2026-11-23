# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual first-party C++ validation to startup-only manager data; no UI/hardware."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(sys.argv.pop(1)).resolve()
PRESSURE_ONLY = '--pressure-only' in sys.argv
if PRESSURE_ONLY:
    sys.argv.remove('--pressure-only')
sys.path.insert(0, str(ROOT / 'tools'))
import phone_manager
import appliance_release as release
from receiver_configuration import load_receiver_configuration
import appliance_release_test as release_fixture

VALID = b'version=1\nspeed_unit=mph\nspeed_maximum_kmh=160\nrpm_maximum=5000\nrpm_redline=0\nshow_rpm=false\n'
EXPECTED = {'speedUnit': 'mph', 'speedMaximumKmh': 160, 'rpmMaximum': 5000,
            'rpmRedline': 0, 'showRpm': False, 'temperatureUnit': 'celsius', 'distanceUnit': 'kilometers', 'fuelConsumptionUnit': 'liters_per_100km', 'speedPrecision': 'whole', 'pressureUnit': 'kpa', 'temperaturePrecision': 'whole'}
OWNER = (os.getuid(), os.getgid())


class Phones:
    def phones(self):
        return []

    def disconnect(self, phone):
        pass


class NativeConfigStatusTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='native-summary-')
        self.root = Path(self.temporary.name).resolve()
        self.file = self.root / 'native.conf'
        self.file.write_bytes(VALID)
        self.file.chmod(0o600)
        self.bin = self.root / 'build'
        self.bin.mkdir()
        self.checker = self.bin / 'headunit-vehicle-config-check'
        shutil.copy2(CHECKER, self.checker)
        self.checker.chmod(0o755)
        self.settings = {'native_cluster_config': self.file, 'bin_dir': self.bin}

    def tearDown(self):
        self.temporary.cleanup()

    def summary(self, raw=VALID, option='--native-summary'):
        result = subprocess.run([str(CHECKER), option], input=raw, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLessEqual(len(result.stdout), 1024)
        return json.loads(result.stdout)

    def admit(self, settings=None, owner=OWNER, binding=None):
        return phone_manager.native_instrument_configuration(
            self.settings if settings is None else settings, owner, binding)

    def unavailable(self, value, error=None):
        self.assertTrue(value['configured'])
        self.assertEqual(value['state'], 'unavailable')
        self.assertIsNone(value['configuration'])
        self.assertIsNone(value['configurationSha256'])
        self.assertIn(value['error'], ('configuration_unavailable', 'validation_unavailable', 'admission_changed'))
        if error:
            self.assertEqual(value['error'], error)
        self.assertNotIn(str(self.root), json.dumps(value))

    def test_cpp_known_parameters_zero_false_and_lossless_fraction(self):
        self.assertEqual(self.summary(), EXPECTED)
        raw = VALID.replace(b'mph', b'kmh').replace(b'=160\n', b'=160.125\n')
        self.assertEqual(self.summary(raw), dict(EXPECTED, speedUnit='kmh', speedMaximumKmh=160.125))
        result = self.admit()
        self.assertEqual(result['configuration'], EXPECTED)
        self.assertEqual(result['configurationSha256'], hashlib.sha256(VALID).hexdigest())
        self.assertEqual(result['source'], 'source_configuration')
        self.assertEqual(result['state'], 'configured')

    def test_cpp_default_authority_and_original_checker_contract(self):
        self.assertEqual(self.summary(b'', '--native-summary-defaults'), {
            'speedUnit': 'kmh', 'speedMaximumKmh': 260, 'rpmMaximum': 8000,
            'rpmRedline': 6500, 'showRpm': True, 'temperatureUnit': 'celsius', 'distanceUnit': 'kilometers', 'fuelConsumptionUnit': 'liters_per_100km', 'speedPrecision': 'whole', 'pressureUnit': 'kpa', 'temperaturePrecision': 'whole'})
        for args in (['-', '-'], ['-', '-', '-', '--native-cluster', str(self.file)]):
            completed = subprocess.run([str(CHECKER), *args], capture_output=True, timeout=10)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn(b'Vehicle capability files valid:', completed.stdout)
        bad = subprocess.run([str(CHECKER), '--native-summary', 'extra'], capture_output=True, timeout=10)
        self.assertNotEqual(bad.returncode, 0)

    def test_same_cpp_units_authority_independent_of_speed_and_private_profile(self):
        for speed in ('kmh', 'mph'):
            for temperature in (None, 'celsius', 'fahrenheit'):
                for distance in (None, 'kilometers', 'miles'):
                    for consumption in (None, 'liters_per_100km', 'us_mpg', 'imperial_mpg'):
                        for profile in (False, True):
                            with self.subTest(speed=speed, temperature=temperature, distance=distance, profile=profile, consumption=consumption):
                                raw = VALID.replace(b'mph', speed.encode())
                                if temperature is not None:
                                    raw += b'temperature_unit=' + temperature.encode() + b'\n'
                                if distance is not None:
                                    raw += b'distance_unit=' + distance.encode() + b'\n'
                                if consumption is not None:
                                    raw += b'fuel_consumption_unit=' + consumption.encode() + b'\n'
                                if profile:
                                    raw += b'drive_mode_profiles_file=/private/secret-profile-marker.json\n'
                                expected = dict(EXPECTED, speedUnit=speed,
                                                temperatureUnit=temperature or 'celsius',
                                                distanceUnit=distance or 'kilometers',
                                                fuelConsumptionUnit=consumption or 'liters_per_100km')
                                self.assertEqual(self.summary(raw), expected)
                                self.file.write_bytes(raw)
                                manager = self.manager()
                                try:
                                    status = manager.request({'action': 'status'})
                                    value = status['native_instruments']
                                    self.assertEqual(value['configuration'], expected)
                                    self.assertEqual(set(value['configuration']), set(EXPECTED))
                                    self.assertEqual(value['configurationSha256'], hashlib.sha256(raw).hexdigest())
                                    self.assertEqual(value['state'], 'configured')
                                    self.assertNotIn('secret-profile-marker', json.dumps(value))
                                finally:
                                    manager.shutdown()
                                self.assertIsNone(manager.child)

    def test_cpp_consumption_invalid_duplicate_missing_bounds_and_private_exclusion(self):
        optional = b'temperature_unit=fahrenheit\ndistance_unit=miles\nfuel_consumption_unit=us_mpg\ndrive_mode_profiles_file=/private/secret-profile-marker.json\n'
        raw = VALID + optional
        expected = dict(EXPECTED, temperatureUnit='fahrenheit', distanceUnit='miles', fuelConsumptionUnit='us_mpg')
        bounded = raw + b'\n' * (4096 - len(raw))
        self.assertEqual(self.summary(bounded), expected)
        self.file.write_bytes(bounded)
        self.assertEqual(self.admit()['configurationSha256'], hashlib.sha256(bounded).hexdigest())
        invalid = [bounded + b'\n', raw + b'unknown=true\n', raw + b'fuel_consumption_unit=us_mpg\n']
        invalid += [VALID + b'fuel_consumption_unit=' + value + b'\n'
                    for value in (b'mpg', b'USmpg', b'', b'0', b'litres_per_100km')]
        invalid += [raw.replace(line, b'') for line in VALID.splitlines(keepends=True)]
        for value in invalid:
            with self.subTest(raw=value):
                completed = subprocess.run([str(CHECKER), '--native-summary'], input=value,
                                           capture_output=True, timeout=10)
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(completed.stdout, b'')
                self.file.write_bytes(value)
                self.unavailable(self.admit())

    def test_cpp_pressure_independent_units_and_actual_manager_projection(self):
        for speed in ('kmh', 'mph'):
            for temperature in ('celsius', 'fahrenheit'):
                for distance in ('kilometers', 'miles'):
                    for fuel in ('liters_per_100km', 'us_mpg', 'imperial_mpg'):
                        for precision in ('whole', 'one_decimal'):
                            for pressure in (None, 'kpa', 'bar', 'psi'):
                                with self.subTest(speed=speed, temperature=temperature, distance=distance,
                                                  fuel=fuel, precision=precision, pressure=pressure):
                                    raw = (VALID.replace(b'mph', speed.encode()) +
                                           ('temperature_unit=' + temperature + '\n' +
                                            'distance_unit=' + distance + '\n' +
                                            'fuel_consumption_unit=' + fuel + '\n' +
                                            'speed_precision=' + precision + '\n' +
                                            'drive_mode_profiles_file=/private/secret-profile-marker\n').encode())
                                    if pressure is not None:
                                        raw += ('pressure_unit=' + pressure + '\n').encode()
                                    expected = dict(EXPECTED, speedUnit=speed, temperatureUnit=temperature,
                                                    distanceUnit=distance, fuelConsumptionUnit=fuel,
                                                    speedPrecision=precision, pressureUnit=pressure or 'kpa')
                                    self.assertEqual(self.summary(raw), expected)
                                    self.file.write_bytes(raw)
                                    manager = self.manager()
                                    try:
                                        value = manager.request({'action': 'status'})['native_instruments']
                                        self.assertEqual(value['configuration'], expected)
                                        self.assertEqual(set(value['configuration']), set(EXPECTED))
                                        self.assertEqual(value['configurationSha256'], hashlib.sha256(raw).hexdigest())
                                        self.assertNotIn('secret-profile-marker', json.dumps(value))
                                    finally:
                                        manager.shutdown()
                                    self.assertIsNone(manager.child)

    def test_cpp_pressure_twelve_fields_bound_and_rejection(self):
        raw = (VALID + b'temperature_unit=fahrenheit\ndistance_unit=miles\n'
               b'fuel_consumption_unit=us_mpg\nspeed_precision=one_decimal\n'
               b'drive_mode_profiles_file=/private/secret-profile-marker\npressure_unit=bar\n')
        expected = dict(EXPECTED, temperatureUnit='fahrenheit', distanceUnit='miles',
                        fuelConsumptionUnit='us_mpg', speedPrecision='one_decimal', pressureUnit='bar')
        self.assertEqual(len(raw.splitlines()), 12)
        bounded = raw + b'\n' * (4096 - len(raw))
        self.assertEqual(self.summary(bounded), expected)
        self.file.write_bytes(bounded)
        self.assertEqual(self.admit()['configuration'], expected)
        self.assertEqual(self.admit()['configurationSha256'], hashlib.sha256(bounded).hexdigest())
        invalid = [bounded + b'\n', raw + b'unknown=1\n', raw + b'pressure_unit=psi\n']
        invalid += [VALID + b'pressure_unit=' + value + b'\n'
                    for value in (b'', b'kPa', b'Bar', b'PSI', b'0', b'true', b'psi ')]
        invalid += [raw.replace(line, b'') for line in VALID.splitlines(keepends=True)]
        for value in invalid:
            with self.subTest(raw=value):
                result = subprocess.run([str(CHECKER), '--native-summary'], input=value,
                                        capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b'')
                self.file.write_bytes(value)
                self.unavailable(self.admit())

    def test_cpp_precision_independent_units_and_private_profile(self):
        for speed in ('kmh', 'mph'):
            for temperature in ('celsius', 'fahrenheit'):
                for distance in ('kilometers', 'miles'):
                    for consumption in ('liters_per_100km', 'us_mpg', 'imperial_mpg'):
                        for precision in (None, 'whole', 'one_decimal'):
                            with self.subTest(speed=speed, temperature=temperature, distance=distance,
                                              consumption=consumption, precision=precision):
                                raw = (VALID.replace(b'mph', speed.encode()) +
                                       ('temperature_unit=' + temperature + '\n' +
                                        'distance_unit=' + distance + '\n' +
                                        'fuel_consumption_unit=' + consumption + '\n' +
                                        'drive_mode_profiles_file=/private/secret-profile-marker\n').encode())
                                if precision is not None:
                                    raw += ('speed_precision=' + precision + '\n').encode()
                                expected = dict(EXPECTED, speedUnit=speed, temperatureUnit=temperature,
                                                distanceUnit=distance, fuelConsumptionUnit=consumption,
                                                speedPrecision=precision or 'whole')
                                self.assertEqual(self.summary(raw), expected)

    def test_cpp_precision_exact_eleven_fields_byte_bound_and_failure(self):
        raw = (VALID + b'temperature_unit=fahrenheit\ndistance_unit=miles\n'
               b'fuel_consumption_unit=us_mpg\nspeed_precision=one_decimal\n'
               b'drive_mode_profiles_file=/private/secret-profile-marker\n')
        expected = dict(EXPECTED, temperatureUnit='fahrenheit', distanceUnit='miles',
                        fuelConsumptionUnit='us_mpg', speedPrecision='one_decimal')
        bounded = raw + b'\n' * (4096 - len(raw))
        self.assertEqual(len(raw.splitlines()), 11)
        self.assertEqual(self.summary(bounded), expected)
        self.file.write_bytes(bounded)
        admitted = self.admit()
        self.assertEqual(admitted['configuration'], expected)
        self.assertEqual(admitted['configurationSha256'], hashlib.sha256(bounded).hexdigest())
        invalid = [bounded + b'\n', raw + b'unknown=1\n', raw + b'speed_precision=whole\n']
        invalid += [VALID + b'speed_precision=' + value + b'\n'
                    for value in (b'', b'Whole', b'one-decimal', b'decimal', b'0', b'true')]
        invalid += [raw.replace(line, b'') for line in VALID.splitlines(keepends=True)]
        for value in invalid:
            with self.subTest(raw=value):
                result = subprocess.run([str(CHECKER), '--native-summary'], input=value,
                                        capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b'')
                self.file.write_bytes(value)
                self.unavailable(self.admit())

    def test_cpp_temperature_invalid_enum_duplicate_unknown_and_missing_required(self):
        for suffix in (b'temperature_unit=kelvin\n', b'temperature_unit=Fahrenheit\n',
                       b'temperature_unit=\n', b'temperature_unit=0\n',
                       b'temperature_unit=fahrenheit\ntemperature_unit=celsius\n',
                       b'temperature_unit=fahrenheit\nunknown=true\n'):
            with self.subTest(suffix=suffix):
                raw = VALID + suffix
                result = subprocess.run([str(CHECKER), '--native-summary'], input=raw,
                                        capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b'')
                self.file.write_bytes(raw)
                self.unavailable(self.admit(), 'validation_unavailable')
        for line in VALID.splitlines(keepends=True):
            raw = VALID.replace(line, b'') + b'temperature_unit=fahrenheit\n'
            with self.subTest(missing=line):
                result = subprocess.run([str(CHECKER), '--native-summary'], input=raw,
                                        capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b'')

    def test_same_parser_rejects_invalid_duplicate_nonfinite_and_range(self):
        invalid = [b'', VALID + b'version=1\n', VALID + b'unknown=true\n', VALID + b'\0',
                   VALID + b'\r', VALID.replace(b'version=1', b'version=2'),
                   VALID.replace(b'speed_unit=mph', b'speed_unit=knots'),
                   VALID.replace(b'=160\n', b'=0\n'), VALID.replace(b'=160\n', b'=801\n'),
                   VALID.replace(b'=160\n', b'=nan\n'), VALID.replace(b'=5000\n', b'=inf\n'),
                   VALID.replace(b'=5000\n', b'=99\n'), VALID.replace(b'=5000\n', b'=30001\n'),
                   VALID.replace(b'=0\n', b'=-1\n'), VALID.replace(b'=0\n', b'=5001\n'),
                   VALID.replace(b'show_rpm=false', b'show_rpm=0'),
                   VALID.replace(b'=160\n', b'=160kmh\n'), b'version=1\n']
        for raw in invalid:
            with self.subTest(raw=raw):
                completed = subprocess.run([str(CHECKER), '--native-summary'], input=raw,
                                           capture_output=True, timeout=10)
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(completed.stdout, b'')
                self.file.write_bytes(raw)
                self.unavailable(self.admit())

    def test_input_bound_and_safe_allowlist_exclude_private_profile(self):
        safe = VALID + b'drive_mode_profiles_file=/private/secret-profile-marker.json\n'
        self.assertEqual(self.summary(safe), EXPECTED)
        self.file.write_bytes(safe)
        value = self.admit()
        self.assertNotIn('secret-profile-marker', json.dumps(value))
        self.assertEqual(value['configurationSha256'], hashlib.sha256(safe).hexdigest())
        bounded = VALID + b'\n' * (4096 - len(VALID))
        self.assertEqual(self.summary(bounded), EXPECTED)
        completed = subprocess.run([str(CHECKER), '--native-summary'], input=bounded + b'\n',
                                   capture_output=True, timeout=10)
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(completed.stdout, b'')

    def test_missing_unsafe_owner_mode_link_nonregular_and_helper(self):
        self.unavailable(self.admit(owner=(OWNER[0] + 1, OWNER[1])))
        for mode in (0o644, 0o666):
            self.file.chmod(mode)
            self.unavailable(self.admit())
        self.file.chmod(0o600)
        link = self.root / 'link'
        link.symlink_to(self.file)
        pipe = self.root / 'fifo'
        os.mkfifo(pipe, 0o600)
        hard = self.root / 'hardlink'
        os.link(self.file, hard)
        self.unavailable(self.admit())
        hard.unlink()
        for path in (link, pipe, self.root, self.root / 'missing'):
            self.unavailable(self.admit(dict(self.settings, native_cluster_config=path)))
        self.checker.chmod(0o777)
        self.unavailable(self.admit())
        self.checker.unlink()
        self.unavailable(self.admit())

    def manager(self, configured=True):
        config = self.root / 'receiver.json'
        display = self.root / 'displays.conf'
        display.write_text('explicit isolated fixture')
        settings = {'display_config': str(display), 'pair_store': str(self.root / 'pairs')}
        if configured:
            settings.update(native_cluster_config=str(self.file), bin_dir=str(self.bin))
        config.write_text(json.dumps(settings))
        return phone_manager.Manager(config, self.root, display, OWNER, bluez=Phones())

    def test_real_manager_copy_has_no_native_request_io_or_refresh_and_survives_retirement(self):
        self.file.write_bytes(VALID + b'temperature_unit=fahrenheit\ndistance_unit=miles\nfuel_consumption_unit=imperial_mpg\nspeed_precision=one_decimal\npressure_unit=psi\n')
        raw = self.file.read_bytes()
        expected = dict(EXPECTED, temperatureUnit='fahrenheit', distanceUnit='miles', fuelConsumptionUnit='imperial_mpg', speedPrecision='one_decimal', pressureUnit='psi')
        manager = self.manager()
        try:
            first = manager.request({'action': 'status'})
            original = first['native_instruments']
            self.assertEqual(original['configuration'], expected)
            first['native_instruments']['configuration']['speedUnit'] = 'mutated-copy'
            first['native_instruments']['configuration']['temperatureUnit'] = 'celsius'
            first['native_instruments']['configuration']['distanceUnit'] = 'kilometers'
            first['native_instruments']['configuration']['fuelConsumptionUnit'] = 'us_mpg'
            first['native_instruments']['configuration']['speedPrecision'] = 'whole'
            first['native_instruments']['configuration']['pressureUnit'] = 'bar'
            replacement = self.root / 'replacement'
            replacement.write_bytes(VALID.replace(b'mph', b'kmh'))
            os.replace(replacement, self.file)
            self.file.chmod(0o666)
            with patch.object(phone_manager, 'native_instrument_configuration', side_effect=AssertionError('refresh')), \
                    patch.object(release, 'read_configuration_member', side_effect=AssertionError('request native read')), \
                    patch.object(phone_manager.subprocess, 'run', side_effect=AssertionError('request subprocess')):
                again = manager.request({'action': 'status'})
                self.assertEqual(again['native_instruments']['configuration'], expected)
                self.assertEqual(again['native_instruments']['configurationSha256'], hashlib.sha256(raw).hexdigest())
                self.assertEqual((again['active'], again['generation'], again['cluster_formats'], again['camera']),
                                 (first['active'], first['generation'], first['cluster_formats'], first['camera']))
                manager.request({'action': 'disconnect'})
                self.assertEqual(manager.request({'action': 'status'})['native_instruments'], again['native_instruments'])
            self.unavailable(self.admit())
        finally:
            manager.shutdown()
        self.assertIsNone(manager.child)

    def test_omission_and_invalid_configuration_preserve_manager_status(self):
        with patch.object(phone_manager.subprocess, 'run', side_effect=AssertionError('omission helper')):
            manager = self.manager(False)
            try:
                omitted = manager.request({'action': 'status'})
                self.assertEqual(omitted['native_instruments'], {
                    'configured': False, 'state': 'not_configured', 'source': 'compatibility_defaults',
                    'configurationSha256': None, 'configuration': None, 'error': None})
            finally:
                manager.shutdown()
        self.file.write_bytes(b'invalid')
        manager = self.manager()
        try:
            failed = manager.request({'action': 'status'})
            self.unavailable(failed['native_instruments'], 'validation_unavailable')
            self.assertEqual((failed['active'], failed['generation'], failed['cluster_formats'], failed['camera']),
                             (omitted['active'], omitted['generation'], omitted['cluster_formats'], omitted['camera']))
        finally:
            manager.shutdown()

    def test_real_cpp_admission_rejects_replacement_permission_and_helper_races(self):
        original_run = subprocess.run
        for change in ('configuration', 'replacement', 'permission', 'helper'):
            with self.subTest(change=change):
                self.file.write_bytes(VALID)
                self.file.chmod(0o600)
                shutil.copy2(CHECKER, self.checker)
                def run(*args, **kwargs):
                    result = original_run(*args, **kwargs)
                    if change == 'configuration':
                        self.file.write_bytes(VALID.replace(b'mph', b'kmh'))
                    elif change == 'replacement':
                        replacement = self.root / 'new'
                        replacement.write_bytes(VALID)
                        replacement.chmod(0o600)
                        os.replace(replacement, self.file)
                    elif change == 'permission':
                        self.file.chmod(0o644)
                    else:
                        self.checker.chmod(0o777)
                    return result
                with patch.object(phone_manager.subprocess, 'run', run):
                    self.unavailable(self.admit(), 'admission_changed')

    def test_checker_failure_and_malformed_output_are_fixed_errors(self):
        for payload in (b'{}', b'[]', b'x' * 1025,
                        json.dumps(dict(EXPECTED, showRpm=0)).encode(),
                        json.dumps(dict(EXPECTED, speedMaximumKmh=True)).encode(),
                        json.dumps(dict(EXPECTED, speedMaximumKmh=float('nan'))).encode(),
                        b'{"speedUnit":"mph","speedUnit":"kmh"}',
                        json.dumps(dict(EXPECTED, privatePath='/private/secret')).encode()):
            def run(command, **options):
                options['stdout'].write(payload)
            with patch.object(phone_manager.subprocess, 'run', run):
                self.unavailable(self.admit(), 'validation_unavailable')
        for error in (subprocess.CalledProcessError(1, ['private']),
                      subprocess.TimeoutExpired(['private'], 10), OSError('private failure')):
            with patch.object(phone_manager.subprocess, 'run', side_effect=error):
                self.unavailable(self.admit(), 'validation_unavailable')

    def test_summary_admits_exact_ten_safe_fields_and_rejects_every_bad_type(self):
        invalid = [dict(EXPECTED, temperatureUnit=unit)
                   for unit in ('kelvin', 'Fahrenheit', '', None, True, 0, [], {})]
        invalid += [dict(EXPECTED, fuelConsumptionUnit=unit)
                    for unit in ('mpg', 'USmpg', '', None, True, 0, [], {})]
        invalid += [dict(EXPECTED, pressureUnit=unit)
                    for unit in ('kPa', 'Bar', 'PSI', '', None, True, 0, [], {})]
        invalid += [dict(EXPECTED, speedPrecision=unit)
                    for unit in ('Whole', 'decimal', '', None, True, 0, [], {})]
        invalid += [dict(EXPECTED, distanceUnit=unit)
                    for unit in ('mi', 'Miles', '', None, True, 0, [], {})]
        invalid += [dict(EXPECTED, speedUnit=unit) for unit in ('knots', None, True, [], {})]
        invalid += [dict(EXPECTED, showRpm=value) for value in (0, 1, 'false', None, [])]
        for key in ('speedMaximumKmh', 'rpmMaximum', 'rpmRedline'):
            invalid += [dict(EXPECTED, **{key: value})
                        for value in (True, None, '160', [], float('nan'), float('inf'), -float('inf'))]
        invalid += [{key: value for key, value in EXPECTED.items() if key != omitted}
                    for omitted in EXPECTED]
        invalid += [dict(EXPECTED, privatePath='/private/secret'),
                    dict(EXPECTED, acquiredNs='100'), dict(EXPECTED, version=1)]
        payloads = [json.dumps(value).encode() for value in invalid]
        payloads += [json.dumps(EXPECTED).encode()[:-1] + b',"temperatureUnit":"fahrenheit"}',
                     json.dumps(EXPECTED).encode()[:-1] + b',"distanceUnit":"miles"}',
                     json.dumps(EXPECTED).encode()[:-1] + b',"fuelConsumptionUnit":"us_mpg"}',
                     json.dumps(EXPECTED).encode()[:-1] + b',"speedPrecision":"one_decimal"}',
                     json.dumps(EXPECTED).encode()[:-1] + b',"pressureUnit":"bar"}',
                     b'{"temperatureUnit":', b'null', json.dumps(EXPECTED).encode() + b' {}']
        for payload in payloads:
            with self.subTest(payload=payload):
                def run(command, **options):
                    options['stdout'].write(payload)
                with patch.object(phone_manager.subprocess, 'run', run):
                    self.unavailable(self.admit(), 'validation_unavailable')

    def test_actual_startup_timeout_reaps_only_the_summary_child(self):
        marker = self.root / 'helper-pid'
        self.checker.write_text('#!' + sys.executable + '\n'
                                'import os, time\nfrom pathlib import Path\n'
                                'Path(' + repr(str(marker)) + ').write_text(str(os.getpid()))\n'
                                'time.sleep(30)\n')
        unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        try:
            self.unavailable(self.admit(), 'validation_unavailable')
            pid = int(marker.read_text())
            with self.assertRaises(ChildProcessError):
                os.waitpid(pid, os.WNOHANG)
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
            self.assertIsNone(unrelated.poll())
            os.kill(unrelated.pid, 0)
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=5)

    def test_actual_sealed_roles_hashes_owner_helper_and_foreign_paths(self):
        fixture = release_fixture.ReleaseTests(methodName='runTest')
        fixture.setUp()
        try:
            fixture.managed()
            raw = VALID + b'temperature_unit=fahrenheit\ndistance_unit=miles\nfuel_consumption_unit=imperial_mpg\nspeed_precision=one_decimal\npressure_unit=psi\ndrive_mode_profiles_file=/private/secret-profile-marker.json\n'
            expected = dict(EXPECTED, temperatureUnit='fahrenheit', distanceUnit='miles', fuelConsumptionUnit='imperial_mpg', speedPrecision='one_decimal', pressureUnit='psi')
            native = fixture.add_role('native_cluster', raw)
            fixture.settings['native_cluster_config'] = str(native)
            fixture.write_receiver()
            shutil.copy2(CHECKER, fixture.repo / 'build/headunit-vehicle-config-check')
            # Mac executes its actual portable helper. Only that exact fixture
            # binary bypasses the appliance ARM header check; hashes/roles/modes
            # and actual C++ parsing remain production checks. Linux is ARM64.
            original_elf = release.elf_aarch64
            def elf(raw):
                if sys.platform == 'darwin' and raw == CHECKER.read_bytes():
                    return
                original_elf(raw)
            with patch.object(release, 'elf_aarch64', elf):
                binding = fixture.publish('native-summary-one')
                root = Path(binding['path'])
                settings = load_receiver_configuration(root / 'config/receiver.json')
                owner = (fixture.account.pw_uid, fixture.account.pw_gid)
                value = self.admit(settings, owner, binding)
                self.assertEqual(value['configuration'], expected)
                self.assertEqual(value['configurationSha256'], hashlib.sha256(raw).hexdigest())
                self.assertNotIn('secret-profile-marker', json.dumps(value))
                self.unavailable(self.admit(dict(settings, native_cluster_config=native), owner, binding))
                self.unavailable(self.admit(dict(settings, native_cluster_config=root / 'config/receiver.json'), owner, binding))
                self.unavailable(self.admit(dict(settings, bin_dir=self.bin), owner, binding))
                self.unavailable(self.admit(settings, (owner[0] + 1, owner[1]), binding))
                for path in (root / 'config/native-cluster.conf', root / 'build/headunit-vehicle-config-check'):
                    data, mode = path.read_bytes(), path.stat().st_mode & 0o777
                    path.chmod(0o777)
                    self.unavailable(self.admit(settings, owner, binding))
                    path.chmod(0o600)
                    path.write_bytes(data + b'X')
                    path.chmod(mode)
                    self.unavailable(self.admit(settings, owner, binding))
                    path.chmod(0o600)
                    path.write_bytes(data)
                    path.chmod(mode)
                    replacement = path.with_name(path.name + '-old')
                    parent_mode = path.parent.stat().st_mode & 0o777
                    path.parent.chmod(0o700)
                    path.rename(replacement)
                    path.parent.chmod(parent_mode)
                    self.unavailable(self.admit(settings, owner, binding))
                    path.parent.chmod(0o700)
                    replacement.rename(path)
                    path.parent.chmod(parent_mode)
                self.assertEqual(self.admit(settings, owner, binding)['configuration'], expected)
        finally:
            fixture.tearDown()


if __name__ == '__main__':
    # Keep the original workload/deadline intact; new combination cases share
    # this production fixture through a separate bounded CTest invocation.
    names = unittest.defaultTestLoader.getTestCaseNames(NativeConfigStatusTests)
    selected = ['NativeConfigStatusTests.' + name for name in names
                if name.startswith('test_cpp_pressure_') == PRESSURE_ONLY]
    unittest.main(defaultTest=selected)
