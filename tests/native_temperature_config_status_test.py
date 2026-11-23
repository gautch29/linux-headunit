# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Separate bounded temperature precision cases; preserve both old status workloads."""
import itertools
import hashlib
import json
import subprocess
import unittest
from unittest.mock import patch

# Reuse the original actual-helper fixture without selecting its old test cases.
# It consumes the checker argv exactly as the original CTest entry point does.
import native_config_status_test as base
from configure_vehicle import native_configuration


class TemperaturePrecisionTests(unittest.TestCase):
    setUp = base.NativeConfigStatusTests.setUp
    tearDown = base.NativeConfigStatusTests.tearDown
    summary = base.NativeConfigStatusTests.summary
    admit = base.NativeConfigStatusTests.admit
    unavailable = base.NativeConfigStatusTests.unavailable
    manager = base.NativeConfigStatusTests.manager

    def test_independent_enums_compiler_cpp_manager_and_omitted_bytes(self):
        dimensions = [('kmh', 'mph'), ('celsius', 'fahrenheit'),
                      ('kilometers', 'miles'), ('liters_per_100km', 'us_mpg', 'imperial_mpg'),
                      ('whole', 'one_decimal'), ('kpa', 'bar', 'psi')]
        for speed, temperature, distance, fuel, precision, pressure in itertools.product(*dimensions):
            native = dict(speedUnit=speed, speedMaximumKmh=160, rpmMaximum=5000,
                          rpmRedline=0, showRpm=False, temperatureUnit=temperature,
                          distanceUnit=distance, fuelConsumptionUnit=fuel,
                          speedPrecision=precision, pressureUnit=pressure)
            old = native_configuration(native)
            self.assertNotIn(b'temperature_precision', old)
            # Same exact reviewed compiler independently reconstructs old bytes.
            expected_old = (base.VALID.replace(b'mph', speed.encode()) +
                            ('temperature_unit=' + temperature + '\n' +
                             'distance_unit=' + distance + '\n' +
                             'fuel_consumption_unit=' + fuel + '\n' +
                             'speed_precision=' + precision + '\n' +
                             'pressure_unit=' + pressure + '\n').encode())
            self.assertEqual(old, expected_old)
            for chosen in ('whole', 'one_decimal'):
                raw = native_configuration(dict(native, temperaturePrecision=chosen))
                self.assertEqual(raw, old + ('temperature_precision=' + chosen + '\n').encode())
                raw += b'drive_mode_profiles_file=/private/secret-profile-marker\n'
                expected = dict(base.EXPECTED, speedUnit=speed, temperatureUnit=temperature,
                                distanceUnit=distance, fuelConsumptionUnit=fuel,
                                speedPrecision=precision, pressureUnit=pressure, temperaturePrecision=chosen)
                self.assertEqual(self.summary(raw), expected)
                self.file.write_bytes(raw)
                manager = self.manager()
                try:
                    value = manager.request({'action': 'status'})['native_instruments']
                    self.assertEqual(value['configuration'], expected)
                    self.assertEqual(len(value['configuration']), 11)
                    self.assertEqual(value['configurationSha256'], hashlib.sha256(raw).hexdigest())
                    self.assertNotIn('secret-profile-marker', json.dumps(value))
                finally:
                    manager.shutdown()
                self.assertIsNone(manager.child)

    def test_thirteen_fields_bounds_strict_types_and_missing_required(self):
        raw = (base.VALID + b'temperature_unit=fahrenheit\ndistance_unit=miles\n'
               b'fuel_consumption_unit=us_mpg\nspeed_precision=one_decimal\npressure_unit=bar\n'
               b'drive_mode_profiles_file=/private/secret-profile-marker\ntemperature_precision=one_decimal\n')
        expected = dict(base.EXPECTED, temperatureUnit='fahrenheit', distanceUnit='miles',
                        fuelConsumptionUnit='us_mpg', speedPrecision='one_decimal',
                        pressureUnit='bar', temperaturePrecision='one_decimal')
        self.assertEqual(len(raw.splitlines()), 13)
        bounded = raw + b'\n' * (4096 - len(raw))
        self.assertEqual(self.summary(bounded), expected)
        self.file.write_bytes(bounded)
        self.assertEqual(self.admit()['configurationSha256'], hashlib.sha256(bounded).hexdigest())
        bad = [bounded + b'\n', raw + b'unknown=1\n', raw + b'temperature_precision=whole\n']
        bad += [base.VALID + b'temperature_precision=' + value + b'\n'
                for value in (b'', b'Whole', b'one-decimal', b'0', b'true', b'one_decimal ')]
        bad += [raw.replace(line, b'') for line in base.VALID.splitlines(keepends=True)]
        for value in bad:
            result = subprocess.run([str(base.CHECKER), '--native-summary'], input=value,
                                    capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')
            self.file.write_bytes(value)
            self.unavailable(self.admit())
        native = dict(speedUnit='kmh', speedMaximumKmh=260, rpmMaximum=8000,
                      rpmRedline=6500, showRpm=True)
        for value in ('Whole', 'one-decimal', '', None, True, 0, 1., [], {}):
            with self.assertRaises(ValueError):
                native_configuration(dict(native, temperaturePrecision=value))
        for key in native:
            missing = dict(native, temperaturePrecision='one_decimal')
            del missing[key]
            with self.assertRaises(ValueError):
                native_configuration(missing)
        self.file.write_bytes(raw)
        invalid = [dict(expected, temperaturePrecision=value)
                   for value in ('Whole', 'one-decimal', '', None, True, 0, 1., [], {})]
        invalid += [{k: v for k, v in expected.items() if k != 'temperaturePrecision'}]
        payloads = [json.dumps(value).encode() for value in invalid]
        payloads += [json.dumps(expected).encode()[:-1] + b',"temperaturePrecision":"whole"}']
        for payload in payloads:
            def run(command, **options):
                options['stdout'].write(payload)
            with patch.object(base.phone_manager.subprocess, 'run', run):
                self.unavailable(self.admit(), 'validation_unavailable')

    def test_immutable_copy_without_refresh_or_authority(self):
        raw = base.VALID + b'temperature_precision=one_decimal\n'
        self.file.write_bytes(raw)
        manager = self.manager()
        try:
            original = manager.request({'action': 'status'})['native_instruments']
            self.assertEqual(original['configuration']['temperaturePrecision'], 'one_decimal')
            original['configuration']['temperaturePrecision'] = 'whole'
            self.file.write_bytes(base.VALID + b'temperature_precision=whole\n')
            with patch.object(base.phone_manager, 'native_instrument_configuration', side_effect=AssertionError('refresh')), \
                    patch.object(base.release, 'read_configuration_member', side_effect=AssertionError('request read')), \
                    patch.object(base.phone_manager.subprocess, 'run', side_effect=AssertionError('request helper')):
                current = manager.request({'action': 'status'})['native_instruments']
                self.assertEqual(current['configuration']['temperaturePrecision'], 'one_decimal')
                self.assertEqual(current['configurationSha256'], hashlib.sha256(raw).hexdigest())
                manager.request({'action': 'disconnect'})
                self.assertEqual(manager.request({'action': 'status'})['native_instruments'], current)
        finally:
            manager.shutdown()
        self.assertIsNone(manager.child)


if __name__ == '__main__':
    unittest.main()
