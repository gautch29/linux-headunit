# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual synthetic C++ acquisition -> kernel role admission -> manager rows."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from prephone_native import NativePublication
BINARY = str(Path(sys.argv.pop(1)).resolve())


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dated-sim-')
        self.folder = Path(self.temp.name).resolve()
        self.folder.chmod(0o700)
        self.children = []
        self.server = None

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=3)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()
        if self.server:
            self.server.close()
        self.temp.cleanup()

    def launch(self, mode='dated'):
        path = self.folder / 'prephone.sock'
        command = [BINARY, str(path), mode]
        self.server = NativePublication(path, (os.getuid(), os.getgid()), command)
        return self.child(command)

    def child(self, command):
        child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        self.children.append(child)
        self.assertTrue(select.select([child.stdout], [], [], 3)[0])
        first, steady, last = map(int, child.stdout.readline().split())
        self.assertLessEqual(first, steady)
        self.assertLessEqual(steady, last)
        self.assertLessEqual(last, time.monotonic_ns())
        return child

    def command(self, child, value):
        child.stdin.write(value.encode())
        child.stdin.flush()
        self.assertTrue(select.select([child.stdout], [], [], 3)[0])
        self.assertEqual(child.stdout.readline().decode().strip(), value)

    def wait_for(self, predicate, seconds=3):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            model = self.server.snapshot()
            if predicate(model):
                return model
            time.sleep(.01)
        self.fail(f'Expected source state missing: {model}')

    def ready(self):
        return self.wait_for(lambda m: all(r['availability'] == 'available' for r in m['rows']))

    def test_legacy_actual_source_is_undated_and_null(self):
        self.launch('legacy')
        model = self.wait_for(lambda m: m['state'] == 'undated_source')
        self.assertIsNone(model['sourceGeneration'])
        self.assertTrue(all(r['value'] is None and r['origin'] is None and
                            r['acquiredNs'] is None for r in model['rows']))

    def test_generated_values_provenance_expiry_original_dates_and_terminal_generation(self):
        child = self.launch()
        first = self.ready()
        self.assertEqual(first['sourceGeneration'], '1')
        self.assertTrue(all(r['origin'] == 'simulation' for r in first['rows']))
        self.assertTrue(all(int(first['sourceStartedNs']) <= int(r['acquiredNs']) <=
                            int(first['sourceSampledNs']) for r in first['rows']))
        self.assertEqual(first['rows'][3]['value'], 18)
        self.assertEqual(first['rows'][1]['value'], 420)
        next_model = self.wait_for(lambda m: int(m['rows'][0]['acquiredNs'] or 0) >
                                   int(first['rows'][0]['acquiredNs']))
        self.assertNotEqual(first['rows'][0]['value'], next_model['rows'][0]['value'])
        self.command(child, 'p')
        stale = self.wait_for(lambda m: all(r['availability'] == 'stale' for r in m['rows']))
        sequence = stale['publisher']['sequence']
        heartbeat = self.wait_for(lambda m: m['publisher']['sequence'] > sequence)
        self.assertEqual(heartbeat['sourceSampledNs'], stale['sourceSampledNs'])
        self.assertEqual([r['acquiredNs'] for r in heartbeat['rows']],
                         [r['acquiredNs'] for r in stale['rows']])
        self.assertTrue(all(r['value'] is None and r['origin'] == 'simulation' for r in heartbeat['rows']))
        self.command(child, 'r')
        self.ready()
        self.command(child, 'w')
        retired = self.wait_for(lambda m: all(r['availability'] == 'disconnected' for r in m['rows']))
        self.assertTrue(all(r['origin'] == 'simulation' and r['value'] is None for r in retired['rows']))
        sequence = retired['publisher']['sequence']
        again = self.wait_for(lambda m: m['publisher']['sequence'] > sequence)
        self.assertEqual([r['acquiredNs'] for r in again['rows']],
                         [r['acquiredNs'] for r in retired['rows']])
        self.assertTrue(all(r['value'] is None for r in again['rows']))
        self.command(child, 'n')
        successor = self.ready()
        self.assertEqual(successor['sourceGeneration'], '2')
        self.assertGreater(int(successor['sourceStartedNs']), int(retired['sourceSampledNs']))
        self.assertTrue(all(int(r['acquiredNs']) >= int(successor['sourceStartedNs']) for r in successor['rows']))

    def test_actual_manager_copy_and_process_loss(self):
        from phone_management_test import Tests as ManagerTests
        child = self.launch()
        manager = ManagerTests().quarantine_fixture(self.folder)
        manager.prephone = self.server
        try:
            self.ready()
            status = manager.request({'action': 'status'})
            self.assertTrue(all(r['origin'] == 'simulation' for r in status['prephone_vehicle']['rows']))
            status['prephone_vehicle']['rows'][0]['value'] = -999
            self.assertGreater(manager.request({'action': 'status'})['prephone_vehicle']['rows'][0]['value'], 0)
            child.kill()
            child.wait(timeout=3)
            retired = manager.request({'action': 'status'})['prephone_vehicle']
            self.assertEqual(retired['state'], 'owner_retired')
            self.assertTrue(all(r['value'] is None for r in retired['rows']))
            self.assertEqual(status['active'], manager.active)
            self.assertIsNone(manager.child)
        finally:
            manager.shutdown()

    def test_real_manager_preserves_independent_native_and_sport_sources(self):
        from dated_simulation_sport_manager_test import private_manager, wait_for
        child = self.launch()
        native = self.ready()
        with private_manager(self.folder, {'simulate_dated_vehicle': True, 'sport_telemetry': True}) as manager:
            manager.prephone = self.server
            sport = wait_for(manager, lambda m: m['inputs']['speed_kmh']['availability'] == 'available')
            original_start = manager.sport.started_ns
            self.assertEqual(sport['source'], 'simulation')
            self.assertLess(int(native['sourceStartedNs']), original_start)
            combined = manager.request({'action': 'status'})
            self.assertEqual(combined['sport']['inputs']['speed_kmh']['origin'], 'simulation')
            self.assertTrue(all(r['origin'] == 'simulation' for r in combined['prephone_vehicle']['rows']))
            self.assertEqual(combined['prephone_vehicle']['sourceStartedNs'], native['sourceStartedNs'])
            self.assertEqual(combined['prephone_vehicle']['rows'][1]['value'], 420)
            reset = manager.request({'action': 'trip_reset'})['sport']
            self.assertIsNone(reset['trip']['distanceKm'])
            self.assertEqual(reset['sourceGeneration'], sport['sourceGeneration'])
            after = manager.request({'action': 'status'})
            self.assertEqual(after['prephone_vehicle']['sourceGeneration'], native['sourceGeneration'])
            self.assertEqual(after['prephone_vehicle']['sourceStartedNs'], native['sourceStartedNs'])
            self.assertEqual(after['prephone_vehicle']['rows'][1]['value'], 420)
            self.assertEqual(manager.sport.started_ns, original_start)
            child.terminate()
            child.wait(timeout=3)
            retired = self.wait_for(lambda m: m['state'] == 'owner_retired')
            self.assertTrue(all(r['value'] is None for r in retired['rows']))
            surviving = wait_for(manager, lambda m: (m['inputs']['speed_kmh']['acquiredNs'] or 0) >
                                 sport['inputs']['speed_kmh']['acquiredNs'])
            self.assertEqual(surviving['source'], 'simulation')
            self.assertEqual(surviving['inputs']['speed_kmh']['availability'], 'available')
            print('Actual native owner retirement preserves independent sport acquisition/start/reset authority')

    def test_exact_argv_mode_admission_and_signals(self):
        path = self.folder / 'prephone.sock'
        command = [BINARY, str(path), 'dated']
        self.server = NativePublication(path, (os.getuid(), os.getgid()), command)
        foreign = self.child([BINARY, str(path), 'legacy'])
        rejected = self.wait_for(lambda m: m['publisher']['error'] is not None)
        self.assertFalse(rejected['publisher']['live'])
        self.assertTrue(all(r['value'] is None for r in rejected['rows']))
        foreign.terminate()
        foreign.wait(timeout=3)
        child = self.child(command)
        self.ready()
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            child.send_signal(signum)
            self.assertEqual(child.wait(timeout=3), 0)
            retired = self.wait_for(lambda m: m['state'] == 'owner_retired')
            self.assertTrue(all(r['value'] is None for r in retired['rows']))
            child = self.child(self.server.command)
            self.ready()
        self.command(child, 's')
        expired = self.wait_for(lambda m: m['state'] == 'publisher_expired')
        self.assertTrue(all(r['value'] is None for r in expired['rows']))
        self.assertIsNone(child.poll())


if __name__ == '__main__':
    unittest.main()
