# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real private Manager constructors and existing sport acquisition owners.

BlueZ is an explicit inert phone boundary. Source, clock, worker, engine,
configuration, status, software resets and retirement are production code.
"""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from phone_manager import Manager
from sport_source import host_now_ns
from sport_telemetry_test import packet


class Phones:
    def phones(self):
        return []


@contextmanager
def private_manager(folder, settings, fanout=None):
    folder = Path(folder).resolve()
    folder.chmod(0o700)
    display = folder / 'display.conf'
    display.write_text('cluster.wayland=fixture-wayland\ncluster.width=1920\ncluster.height=720\n')
    config = folder / 'receiver.json'
    config.write_text(json.dumps(dict(display_config=str(display), pair_store=str(folder / 'pairs'), **settings)))
    config.chmod(0o600)
    manager = Manager(config, folder, display, (os.getuid(), os.getgid()), bluez=Phones(), fanout_config=fanout)
    source = manager.sport
    try:
        yield manager
    finally:
        manager.shutdown()
        if source:
            assert not source.thread.is_alive(), 'Actual sport acquisition thread was not joined'
            retired = source.snapshot()
            assert all(row['value'] is None and row['availability'] in ('unavailable', 'disconnected')
                       for row in retired['inputs'].values()), retired
            print(json.dumps({'receipt': 'actual-sport-retired', 'source': retired['source'],
                              'threadJoined': True, 'inputs': retired['inputs']}))


def status(manager):
    return manager.request({'action': 'status'})['sport']


def wait_for(manager, predicate, limit=4):
    end = time.monotonic() + limit
    while time.monotonic() < end:
        model = status(manager)
        if predicate(model):
            return model
        time.sleep(.01)
    raise AssertionError(f'Actual sport owner condition missing: {model}')


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dated-sport-manager-')
        self.folder = Path(self.temp.name).resolve()
        self.addCleanup(self.temp.cleanup)

    def assert_simulated(self, manager):
        source = manager.sport
        model = status(manager)
        print(json.dumps({'receipt': 'actual-manager-constructor', 'source': model['source'],
                          'simulated': source.simulated, 'threadAlive': source.thread.is_alive(),
                          'startedNs': source.started_ns, 'inputs': model['inputs'], 'trip': model['trip']}))
        self.assertEqual(model['source'], 'simulation')
        self.assertTrue(source.simulated)
        self.assertIsNone(source.path)
        self.assertEqual(source.thread.name, 'sport-observations')
        self.assertTrue(source.thread.is_alive())
        model = wait_for(manager, lambda m: all(row['availability'] == 'available' for row in m['inputs'].values()))
        for row in model['inputs'].values():
            self.assertEqual(row['origin'], 'simulation')
            self.assertGreaterEqual(row['acquiredNs'], source.started_ns)
            self.assertLessEqual(row['acquiredNs'], model['observedNs'])
            self.assertEqual(row['ageNs'], model['observedNs'] - row['acquiredNs'])
            self.assertEqual(row['maximumAgeNs'], 500_000_000)
        return model

    def test_dated_sport_enabled_real_constructor_trip_and_software_reset(self):
        with private_manager(self.folder, {'simulate_dated_vehicle': True, 'sport_telemetry': True}) as manager:
            first = self.assert_simulated(manager)
            source = manager.sport
            source_start = source.started_ns
            session = first['sessionId']
            # Actual dedicated sport curve stays stopped for its first two seconds.
            # Wait for genuine later acquisitions; never advance its clock/start.
            moving = wait_for(manager, lambda m: (m['trip']['distanceKm'] or 0) > 0 and
                              (m['trip']['coveredSeconds'] or 0) > (first['trip']['coveredSeconds'] or 0) and
                              m['inputs']['speed_kmh']['acquiredNs'] > first['inputs']['speed_kmh']['acquiredNs'])
            self.assertGreater(moving['trip']['coveredSeconds'], first['trip']['coveredSeconds'] or 0)
            self.assertGreater(moving['inputs']['speed_kmh']['acquiredNs'], first['inputs']['speed_kmh']['acquiredNs'])
            self.assertEqual(moving['trip']['origin'], 'simulation')
            reset = manager.request({'action': 'trip_reset'})['sport']
            self.assertNotEqual(reset['trip']['journeyId'], moving['trip']['journeyId'])
            self.assertIsNone(reset['trip']['distanceKm'])
            self.assertIsNone(reset['trip']['lastAcquiredNs'])
            self.assertEqual(reset['sessionId'], session)
            self.assertIs(manager.sport, source)
            self.assertEqual(source.started_ns, source_start)
            self.assertEqual(reset['sourceGeneration'], moving['sourceGeneration'])
            self.assertEqual(reset['inputs']['speed_kmh']['origin'], 'simulation')
            recovered = wait_for(manager, lambda m: m['trip']['lastAcquiredNs'] is not None)
            self.assertGreater(recovered['trip']['firstAcquiredNs'], reset['observedNs'])
            print(json.dumps({'receipt': 'actual-trip-software-reset', 'before': moving['trip'],
                              'reset': reset['trip'], 'resetObservedNs': reset['observedNs'],
                              'after': recovered['trip'], 'sourceStartUnchanged': source_start}))

    def test_dated_sport_disabled_creates_no_sport_source(self):
        with private_manager(self.folder, {'simulate_dated_vehicle': True, 'sport_telemetry': False}) as manager:
            self.assertIsNone(manager.sport)
            self.assertEqual(status(manager), {'configured': False})
            for action in ('trip_reset', 'sport_session'):
                with self.assertRaisesRegex(ValueError, 'not configured'):
                    manager.request({'action': action})

    def test_legacy_simulation_keeps_existing_source(self):
        with private_manager(self.folder, {'simulate_vehicle': True, 'sport_telemetry': True}) as manager:
            self.assert_simulated(manager)

    def test_no_source_keeps_unavailable_and_cannot_create_trip_values(self):
        with private_manager(self.folder, {'sport_telemetry': True}) as manager:
            self.assertFalse(manager.sport.simulated)
            self.assertTrue(manager.sport.thread.is_alive())
            model = status(manager)
            self.assertEqual(model['source'], 'unavailable')
            self.assertEqual(model['trip']['phase'], 'waiting')
            self.assertIsNone(model['trip']['distanceKm'])
            self.assertIsNone(model['trip']['coveredSeconds'])
            self.assertTrue(all(row['value'] is None and row['acquiredNs'] is None
                                for row in model['inputs'].values()))

    def test_fanout_observation_adapter_stays_independent_and_expires(self):
        bus = self.folder / 'fanout.json'
        consumers = {name: str(self.folder / (name + '.sock')) for name in ('backup', 'receiver', 'sport')}
        bus.write_text(json.dumps({'version': 1, 'input': str(self.folder / 'input.sock'), 'consumers': consumers}))
        bus.chmod(0o600)
        with private_manager(self.folder, {'sport_telemetry': True, 'vehicle_socket': consumers['receiver'],
                                          'fallback_cluster': True, 'bin_dir': str(self.folder)}, bus) as manager:
            self.assertFalse(manager.sport.simulated)
            self.assertEqual(manager.sport.path, Path(consumers['sport']))
            acquired = host_now_ns()
            # Existing bounded HVO fixture only; this is not physical hardware.
            fields = packet(acquired, generation=37, value='36').split(b' ')
            fields[3] = str(acquired).encode('ascii')  # This fixture producer's actual generation start.
            raw = b' '.join(fields).replace(b',s,', b',m,')
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
                sender.sendto(raw, consumers['sport'])
            model = wait_for(manager, lambda m: m['inputs']['speed_kmh']['availability'] == 'available')
            self.assertEqual(model['source'], 'observations')
            self.assertEqual(model['sourceGeneration'], 37)
            self.assertEqual(model['inputs']['speed_kmh']['value'], 36)
            self.assertEqual(model['inputs']['speed_kmh']['origin'], 'measured')
            self.assertEqual(model['inputs']['speed_kmh']['acquiredNs'], acquired)
            expired = wait_for(manager, lambda m: m['inputs']['speed_kmh']['availability'] == 'stale')
            self.assertIsNone(expired['inputs']['speed_kmh']['value'])
            self.assertEqual(expired['inputs']['speed_kmh']['acquiredNs'], acquired)
            self.assertEqual(expired['trip']['phase'], 'waiting')
        self.assertFalse(Path(consumers['sport']).exists())

    def test_invalid_dated_combinations_reject_actual_constructor(self):
        for extra in ({'simulate_vehicle': True}, {'vehicle_socket': str(self.folder / 'receiver.sock')}):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                with private_manager(self.folder, dict(simulate_dated_vehicle=True, sport_telemetry=True, **extra)):
                    self.fail('Invalid mixed source constructed a worker')
        bus = self.folder / 'fanout.json'
        bus.write_text('{}')
        bus.chmod(0o600)
        with self.assertRaisesRegex(ValueError, 'fan-out'):
            with private_manager(self.folder, {'simulate_dated_vehicle': True, 'sport_telemetry': True,
                                              'fallback_cluster': True, 'bin_dir': str(self.folder)}, bus):
                self.fail('Dated source accepted a fan-out')


if __name__ == '__main__':
    unittest.main()
