# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Production native source->kernel IPC->manager data with explicit fixtures."""
import os
from pathlib import Path
import signal
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from prephone_native import NativeModel, NativePublication, decode, LEASE_NS
MODEL_ONLY = '--model' in sys.argv
if MODEL_ONLY:
    sys.argv.remove('--model')
BINARY = str(Path(sys.argv.pop(1)).resolve())


class ModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Bytes from ACTUAL C++ cache/projection/encoder; no Python pseudo-producer.
        cls.raw = subprocess.check_output([BINARY, '--packet'], timeout=5)
        cls.nonce = cls.raw[8:40]
        cls.now = struct.unpack_from('<Q', cls.raw, 48)[0]

    def test_original_acquisition_units_zero_negative_and_copy_independence(self):
        model = NativeModel()
        model.accept(self.raw, self.nonce, self.now)
        first = model.snapshot(self.now)
        self.assertEqual(first['sourceGeneration'], '17')
        self.assertEqual(first['rows'][0]['value'], 0)
        self.assertEqual(first['rows'][3]['value'], -7)
        self.assertEqual(first['rows'][0]['origin'], 'simulation')
        self.assertEqual(first['rows'][1]['availability'], 'missing')
        acquired = first['rows'][0]['acquiredNs']
        first['rows'][0]['value'] = 99
        second = model.snapshot(int(acquired) + 500_000_000)
        self.assertEqual(second['rows'][0]['acquiredNs'], acquired)
        self.assertEqual(second['rows'][0]['availability'], 'stale')
        self.assertIsNone(second['rows'][0]['value'])
        self.assertEqual(second['sourceSampledNs'], acquired)

    def test_replay_nonce_malformed_atomic_and_no_revival(self):
        model = NativeModel()
        model.accept(self.raw, self.nonce, self.now)
        before = model.snapshot(self.now)
        changes = [(40, '<Q', 9), (72, '<Q', 1), (112, '<Q', 0), (96, '<d', float('nan'))]
        for offset, fmt, value in changes:
            bad = bytearray(self.raw)
            struct.pack_into('<Q', bad, 40, 10)
            struct.pack_into(fmt, bad, offset, value)
            with self.assertRaises(ValueError):
                model.accept(bytes(bad), self.nonce, self.now)
            self.assertEqual(model.snapshot(self.now), before)
        with self.assertRaises(ValueError):
            decode(self.raw, b'x' * 32, self.now)
        for bad in (b'', self.raw[:-1], self.raw + b'x'):
            with self.assertRaises(ValueError):
                decode(bad, self.nonce, self.now)
        expired = self.now + 500_000_000
        model.snapshot(expired)
        fresh_copy = bytearray(self.raw)
        struct.pack_into('<QQ', fresh_copy, 40, 10, expired)
        with self.assertRaises(ValueError):
            model.accept(bytes(fresh_copy), self.nonce, expired)
        model.retire('owner_retired')
        self.assertTrue(all(row['value'] is None for row in model.snapshot(expired)['rows']))

    def test_policy_generation_clock_padding_failure(self):
        for offset, fmt, value in [(56, '<Q', 88), (64, '<Q', 0), (48, '<Q', self.now + 1),
                                    (89, '<B', 255), (90, '<B', 1), (112, '<Q', 86_400_000_000_001)]:
            bad = bytearray(self.raw)
            struct.pack_into(fmt, bad, offset, value)
            with self.assertRaises(ValueError):
                decode(bytes(bad), self.nonce, self.now)


@unittest.skipIf(MODEL_ONLY, 'Linux owner suite is separately registered')
class OwnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='prephone-')
        self.folder = Path(self.temp.name).resolve()
        self.folder.chmod(0o700)
        self.path = self.folder / 'prephone.sock'
        self.input = self.folder / 'vehicle.sock'
        self.command = [BINARY, str(self.path), str(self.input)]
        self.server = NativePublication(self.path, (os.getuid(), os.getgid()), self.command)
        self.processes = []
        self.child = self.launch()

    def launch(self):
        child = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        self.processes.append(child)
        return child

    def tearDown(self):
        for child in self.processes:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()
        self.server.close()
        self.temp.cleanup()

    def send(self, command):
        self.child.stdin.write(command.encode())
        self.child.stdin.flush()

    def wait_for(self, predicate, limit=3):
        end = time.monotonic() + limit
        while time.monotonic() < end:
            model = self.server.snapshot()
            if predicate(model):
                return model
            if self.child.poll() is not None:
                self.fail(f'Fixture exited: {self.child.stderr.read().decode()}')
            time.sleep(0.01)
        self.fail(f'Publication condition missing: {model}')

    def ready(self):
        self.wait_for(lambda model: model['publisher']['live'])
        self.send('f')
        return self.wait_for(lambda model: model['rows'][0]['availability'] == 'available')

    def test_real_source_expiry_withdrawal_generation_and_new_acquisition(self):
        initial = self.ready()
        acquired = initial['rows'][0]['acquiredNs']
        self.assertEqual(initial['rows'][0]['value'], 0)
        self.assertEqual(initial['rows'][3]['value'], -5)
        self.assertEqual(initial['rows'][1]['origin'], 'calculated')
        expired = self.wait_for(lambda model: model['rows'][0]['availability'] == 'stale')
        self.assertEqual(expired['rows'][0]['acquiredNs'], acquired)
        self.assertEqual(expired['sourceSampledNs'], initial['sourceSampledNs'])
        self.assertEqual(expired['rows'][3]['value'], -5)
        self.send('w')
        withdrawn = self.wait_for(lambda model: model['rows'][3]['availability'] == 'disconnected')
        self.assertEqual(withdrawn['rows'][3]['acquiredNs'], initial['rows'][3]['acquiredNs'])
        self.send('n')
        new = self.wait_for(lambda model: model['sourceGeneration'] == '2')
        self.assertIsNone(new['rows'][0]['value'])
        self.send('f')
        new = self.wait_for(lambda model: model['rows'][0]['availability'] == 'available')
        self.assertGreater(int(new['rows'][0]['acquiredNs']), int(acquired))

    def test_actual_death_clears_fresh_long_age_without_waiting_expiry(self):
        self.ready()
        self.child.kill()
        self.child.wait(timeout=3)
        model = self.server.snapshot()
        self.assertEqual(model['state'], 'owner_retired')
        self.assertFalse(model['publisher']['live'])
        self.assertTrue(all(row['value'] is None for row in model['rows']))

    def test_stalled_live_native_owner_retires_copied_rows(self):
        initial = self.ready()
        self.send('s')
        retired = self.wait_for(lambda model: model['state'] == 'publisher_expired')
        self.assertIsNone(self.child.poll())
        self.assertIsNone(retired['rows'][3]['value'])
        self.assertEqual(retired['rows'][3]['acquiredNs'], initial['rows'][3]['acquiredNs'])

    def test_backpressure_on_production_send_preserves_acquisition_and_recovery(self):
        def source_acquisition():
            self.assertTrue(select.select([self.child.stdout], [], [], 3)[0])
            fields = self.child.stdout.readline().decode().split()
            self.assertEqual(len(fields), 3)
            self.assertEqual(fields[0], 'source')
            return fields[1], fields[2]

        original = self.ready()
        self.assertEqual(source_acquisition()[1], original['sourceSampledNs'])
        old_nonce = self.server.nonce
        self.send('bf')
        generation, blocked_acquired = source_acquisition()
        expired = self.wait_for(lambda model: model['state'] == 'publisher_expired')
        self.assertIsNone(self.child.poll())
        self.assertTrue(all(row['value'] is None for row in expired['rows']))
        # Reconnection and its new challenge are independent of acquisition.
        # Recover the old dated source first; never require a fresh reading to
        # survive the 1s reconnect/200ms copy policy inside its 500ms lifetime.
        self.send('a')
        readmitted = self.wait_for(lambda model: model['publisher']['live'] and
                                  model['sourceSampledNs'] == blocked_acquired)
        self.assertNotEqual(self.server.nonce, old_nonce)
        self.assertEqual(readmitted['sourceGeneration'], generation)
        self.assertEqual(readmitted['sourceStartedNs'], original['sourceStartedNs'])
        self.assertEqual(readmitted['rows'][0]['availability'], 'stale')
        self.assertIsNone(readmitted['rows'][0]['value'])
        for index in (0, 1, 3):
            self.assertEqual(readmitted['rows'][index]['acquiredNs'], blocked_acquired)
            self.assertEqual(readmitted['rows'][index]['origin'], original['rows'][index]['origin'])
            self.assertEqual(readmitted['rows'][index]['maximumAgeNs'],
                             original['rows'][index]['maximumAgeNs'])
        # Preserve the forward fixture's independent original-age/order checks.
        self.assertGreater(int(readmitted['rows'][0]['acquiredNs']),
                           int(original['rows'][0]['acquiredNs']))
        self.assertGreaterEqual(int(readmitted['publisher']['copiedNs']) -
                                int(readmitted['rows'][0]['acquiredNs']),
                                int(readmitted['rows'][0]['maximumAgeNs']))
        self.send('f')
        new_generation, new_acquired = source_acquisition()
        recovered = self.wait_for(lambda model: model['rows'][0]['availability'] == 'available')
        self.assertEqual(new_generation, generation)
        self.assertGreater(int(new_acquired), int(blocked_acquired))
        self.assertEqual(recovered['sourceSampledNs'], new_acquired)
        self.assertEqual(recovered['rows'][0]['acquiredNs'], new_acquired)
        self.assertEqual(recovered['rows'][0]['value'], 0)
        self.assertEqual(recovered['rows'][0]['origin'], 'simulation')

    def test_actual_manager_status_copies_model_and_retires_without_phone_change(self):
        from phone_management_test import Tests
        manager = Tests().quarantine_fixture(self.folder)
        manager.prephone = self.server
        try:
            self.ready()
            status = manager.request({'action': 'status'})
            self.assertEqual(status['prephone_vehicle']['rows'][0]['value'], 0)
            status['prephone_vehicle']['rows'][0]['value'] = 99
            self.assertEqual(manager.request({'action': 'status'})['prephone_vehicle']['rows'][0]['value'], 0)
            self.child.kill()
            self.child.wait(timeout=3)
            status = manager.request({'action': 'status'})
            self.assertEqual(status['prephone_vehicle']['state'], 'owner_retired')
            self.assertFalse(status['prephone_vehicle']['publisher']['live'])
            self.assertEqual(status['active'], manager.active)
            self.assertIsNone(manager.child)
        finally:
            manager.shutdown()

    def test_unconsumed_ipc_does_not_block_source_shutdown(self):
        self.ready()
        # Production sends at most five snapshots/s. Leave manager unpolled;
        # native sample continues and TERM reaps without any disk/thread join.
        time.sleep(0.6)
        self.child.terminate()
        self.assertEqual(self.child.wait(timeout=2), 0)
        self.assertEqual(self.server.snapshot()['state'], 'owner_retired')

    def test_successor_and_old_endpoint_retirement(self):
        self.ready()
        first_nonce = self.server.nonce
        self.child.kill()
        self.child.wait(timeout=3)
        self.server.snapshot()
        self.child = self.launch()
        awaiting = self.wait_for(lambda model: model['publisher']['live'])
        self.assertTrue(all(row['value'] is None for row in awaiting['rows']))
        self.ready()
        self.assertNotEqual(self.server.nonce, first_nonce)
        old_path = self.path.with_name('retired.sock')
        self.path.rename(old_path)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as replacement:
            replacement.bind(str(self.path))
            identity = self.path.lstat().st_ino
            self.server.close()
            self.assertEqual(self.path.lstat().st_ino, identity)
        self.path.unlink()
        old_path.unlink()

    def test_wrong_role_foreign_peer_and_second_owner_are_rejected(self):
        # A same-UID socket client has no sealed native-role authority.
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as rogue:
            rogue.connect(str(self.path))
            self.server.poll()
            self.assertEqual(rogue.recv(32), b'')
        self.ready()
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as rogue:
            rogue.connect(str(self.path))
            self.server.poll()
            self.assertEqual(rogue.recv(32), b'')
        self.assertTrue(self.server.snapshot()['publisher']['live'])
        with self.assertRaises(OSError):
            NativePublication(self.path, (os.getuid(), os.getgid()), self.command)
        self.assertTrue(self.server.snapshot()['publisher']['live'])

    def test_inherited_socket_writer_is_not_native_acquisition_authority(self):
        self.ready()
        self.send('i')
        model = self.wait_for(lambda model: model['state'] == 'protocol_failure')
        self.assertIsNone(self.child.poll())
        self.assertFalse(model['publisher']['live'])
        self.assertTrue(all(row['value'] is None for row in model['rows']))

    def test_copy_during_admission_uses_actual_receipt_time(self):
        original = self.server._admit
        def admit(connection):
            original(connection)
            self.send('f')
            self.assertTrue(select.select([connection], [], [], 1)[0])
        with patch.object(self.server, '_admit', side_effect=admit):
            model = self.wait_for(lambda model: model['sourceGeneration'] == '1')
        self.assertTrue(model['publisher']['live'])
        self.assertEqual(model['rows'][1]['value'], 42)

    def test_second_copy_between_reads_uses_its_actual_receipt_time(self):
        self.ready()
        self.assertTrue(select.select([self.server.connection], [], [], 1)[0])
        connection = self.server.connection
        testcase = self
        class BetweenReads:
            reads = 0
            def fileno(self):
                return connection.fileno()
            def close(self):
                return connection.close()
            def recvmsg(self, *args):
                self.reads += 1
                if self.reads == 2:
                    testcase.send('f')
                    testcase.assertTrue(select.select([connection], [], [], 1)[0])
                return connection.recvmsg(*args)
        self.server.connection = BetweenReads()
        started = time.monotonic_ns()
        self.server.poll()
        model = self.server.snapshot()
        self.assertTrue(model['publisher']['live'], model['state'])
        self.assertGreater(int(model['sourceSampledNs']), started)
        self.assertEqual(model['rows'][1]['value'], 42)

    def test_true_future_copy_and_actual_clock_reversal_retire(self):
        self.ready()
        frontier = self.server.last_now
        with patch('prephone_native.time.monotonic_ns', return_value=frontier - 1):
            self.server.poll()
        self.assertEqual(self.server.model.retirement, 'clock_failure')
        self.assertEqual(self.server.last_now, frontier)
        self.child.kill()
        self.child.wait(timeout=3)
        self.child = self.launch()
        self.ready()
        self.send('t')
        model = self.wait_for(lambda model: model['state'] == 'protocol_failure')
        self.assertTrue(all(row['value'] is None for row in model['rows']))
        self.assertIsNone(self.child.poll())

    def test_clock_failure_during_admission_closes_only_actual_owned_fds(self):
        self.assertTrue(select.select([self.server.server], [], [], 1)[0])
        before = len(list(Path('/proc/self/fd').iterdir()))
        observed = time.monotonic_ns()
        with patch('prephone_native.time.monotonic_ns', side_effect=(observed, observed - 1)):
            self.server.poll()
        self.assertIsNone(self.server.connection)
        self.assertEqual(self.server.pidfd, -1)
        self.assertEqual(self.server.model.retirement, 'clock_failure')
        self.assertEqual(len(list(Path('/proc/self/fd').iterdir())), before)

    def test_int_term_hup_retirement(self):
        for index, sig in enumerate((signal.SIGINT, signal.SIGTERM, signal.SIGHUP)):
            if index:
                self.child = self.launch()
            self.ready()
            self.child.send_signal(sig)
            self.assertEqual(self.child.wait(timeout=2), 0)
            self.assertEqual(self.server.snapshot()['state'], 'owner_retired')


if __name__ == '__main__':
    # Avoid recording artificial skipped owner cases in the portable lane.
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ModelTests if MODEL_ONLY else OwnerTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
