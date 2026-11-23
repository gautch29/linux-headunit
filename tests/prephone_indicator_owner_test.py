# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual C++ native source -> Linux kernel admission -> real Manager endpoint.

All car observations are explicitly acquired simulation fixtures. Hooks only
count real calls or terminate/wait for an actual owned child between copies.
"""
import copy
import ctypes
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from prephone_native import NativePublication
from phone_manager_endpoint import ManagerEndpoint
from phone_management_test import Tests as ManagerFixtures

BINARY = str(Path(sys.argv.pop(1)).resolve())


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='indicators-')
        self.folder = Path(self.temp.name).resolve()
        self.folder.chmod(0o700)
        self.command = [BINARY, str(self.folder / 'prephone.sock'), str(self.folder / 'vehicle.sock'),
                        '--ambient', '--prephone-indicators']
        self.server = NativePublication(self.folder / 'prephone.sock', (os.getuid(), os.getgid()),
                                        self.command, True, indicators_enabled=True)
        self.manager = ManagerFixtures().quarantine_fixture(self.folder)
        self.manager.prephone = self.server
        self.manager.base.update(ambient_light=True, prephone_indicators=True,
                                 prephone_publication_socket=self.folder / 'prephone.sock')
        self.endpoint = ManagerEndpoint(self.folder, (os.getuid(), os.getgid()))
        self.children = []
        self.child = self.launch()
        self.wait_for(lambda result: result['ambient_light']['available'])

    def launch(self):
        child = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, bufsize=0)
        self.children.append(child)
        return child

    def tearDown(self):
        self.endpoint.close()
        self.manager.shutdown()
        for child in self.children:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
            for stream in (child.stdin, child.stdout, child.stderr):
                if stream:
                    stream.close()
        self.temp.cleanup()

    def request(self, data):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(2)
            client.connect(str(self.endpoint.path))
            client.sendall(json.dumps(data).encode() + b'\n')
            self.assertTrue(self.endpoint.serve_once(self.manager.request))
            raw = b''
            while not raw.endswith(b'\n'):
                raw += client.recv(65536)
            result = json.loads(raw)
        self.assertTrue(result['ok'], result)
        return result

    def status(self):
        result = self.request({'action': 'status'})
        for key in ('ambient_light', 'prephone_indicators'):
            self.assertEqual(result[key]['publisher'], result['prephone_vehicle']['publisher'])
        self.assertIsNone(result['ambient_light']['current'])
        self.assertIsNone(result['ambient_light']['command'])
        return result

    def wait_for(self, condition, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.status()
            if condition(result):
                return result
            self.assertIsNone(self.child.poll())
            time.sleep(.01)
        self.fail('Actual publication condition missing: ' + repr(result))

    def send(self, command):
        self.child.stdin.write(command.encode())
        self.child.stdin.flush()

    def line(self, prefix):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            self.assertTrue(select.select([self.child.stdout], [], [], max(0, deadline - time.monotonic()))[0])
            line = self.child.stdout.readline()
            if line.startswith(prefix):
                return line
        self.fail('Actual native fixture barrier missing')

    def stall(self):
        self.send('s')
        self.line(b'stalled')

    def count(self):
        self.send('q')
        return int(self.line(b'packets ').split()[1])

    def queue(self, records):
        baseline = self.count()
        self.send('r')
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.count() - baseline >= records:
                self.stall()
                return self.count() - baseline
            time.sleep(.01)
        self.fail('Actual native successful sends did not queue requested records')

    def lamps(self):
        self.send('fl')
        return self.wait_for(lambda result: result['prephone_indicators']['rows'][1]['value'] == 'OFF' and
                             result['prephone_vehicle']['rows'][0]['value'] == 0)

    def policy(self):
        result = self.status()
        settings = copy.deepcopy(result['ambient_light']['settings'])
        settings.update(enabled=True, selection='fixed', fixed_rgbw=[9, 37, 83, 0], night_policy='off')
        self.request({'action': 'ambient_light_settings', 'revision': result['ambient_light']['revision'],
                      'settings': settings})
        return self.wait_for(lambda result: result['ambient_light']['software_action'] and
                             result['ambient_light']['software_action']['status'] == 'software_updated')

    def test_same_source_copy_one_poll_same_clock_canonical_values_and_deepcopies(self):
        before = self.lamps()
        with patch.object(self.server, 'poll', wraps=self.server.poll) as poll, \
                patch.object(self.server.model, 'snapshot', wraps=self.server.model.snapshot) as vehicle, \
                patch.object(self.server.ambient, 'snapshot', wraps=self.server.ambient.snapshot) as ambient, \
                patch.object(self.server.indicators, 'snapshot', wraps=self.server.indicators.snapshot) as lamps:
            result = self.status()
        self.assertEqual(poll.call_count, 1)
        self.assertEqual(vehicle.call_args.args[0], ambient.call_args.args[0])
        self.assertEqual(vehicle.call_args.args[0], lamps.call_args.args[0])
        model = result['prephone_indicators']
        self.assertEqual(model['rows'][0]['value'], 'FLASHING')
        self.assertEqual(model['rows'][1]['value'], 'OFF')
        self.assertEqual(model['rows'][5]['availability'], 'missing')
        self.assertEqual(model['rows'][6]['availability'], 'unavailable')
        self.assertEqual(model['rows'][1]['origin'], 'simulation')
        self.assertEqual(model['copiedNs'], result['prephone_vehicle']['publisher']['copiedNs'])
        self.assertEqual(model['copiedNs'], result['ambient_light']['sampled_ns'])
        self.assertEqual(model['sourceSampledNs'], result['prephone_vehicle']['sourceSampledNs'])
        self.assertGreaterEqual(int(model['renderedNs']), int(model['copiedNs']))
        triple = self.server.observations()
        triple[0]['publisher']['live'] = False
        triple[1]['settings']['fixed_rgbw'][0] = 255
        triple[2]['rows'][1]['value'] = 'ON'
        self.assertTrue(triple[2]['publisher']['live'])
        fresh = self.server.observations()
        self.assertTrue(fresh[0]['publisher']['live'])
        self.assertNotEqual(fresh[1]['settings']['fixed_rgbw'][0], 255)
        self.assertEqual(fresh[2]['rows'][1]['value'], 'OFF')
        self.assertEqual(len(self.server.snapshots()), 2)
        for method in (self.server.snapshot, self.server.ambient_snapshot, self.server.indicator_snapshot):
            with patch.object(self.server, 'poll', wraps=self.server.poll) as poll:
                method()
            self.assertEqual(poll.call_count, 1)
        self.assertEqual(result['active'], before['active'])

    def test_original_ttl_expiry_withdrawal_heartbeat_and_genuine_generation(self):
        before = self.lamps()['prephone_indicators']
        acquired = before['rows'][0]['acquiredNs']
        self.send('h')
        copied = self.wait_for(lambda result: int(result['prephone_indicators']['publicationSequence']) >
                               int(before['publicationSequence']))['prephone_indicators']
        self.assertEqual(copied['rows'][0]['acquiredNs'], acquired)
        expired = self.wait_for(lambda result: result['prephone_indicators']['rows'][0]['availability'] == 'stale')
        lamp = expired['prephone_indicators']['rows'][0]
        self.assertEqual(lamp['acquiredNs'], acquired)
        self.assertGreaterEqual(int(lamp['ageNs']), int(lamp['maximumAgeNs']))
        self.assertEqual(expired['prephone_indicators']['rows'][1]['value'], 'OFF')
        self.send('w')
        withdrawn = self.wait_for(lambda result: result['prephone_indicators']['rows'][1]['availability'] == 'disconnected')
        self.assertEqual(withdrawn['prephone_indicators']['rows'][1]['acquiredNs'], before['rows'][1]['acquiredNs'])
        self.send('n')
        new = self.wait_for(lambda result: result['prephone_indicators']['sourceGeneration'] == '2')
        self.assertTrue(all(row['value'] is None and row['acquiredNs'] is None for row in new['prephone_indicators']['rows']))
        self.send('l')
        recovered = self.wait_for(lambda result: result['prephone_indicators']['rows'][1]['value'] == 'OFF')
        self.assertGreater(int(recovered['prephone_indicators']['rows'][1]['acquiredNs']), int(before['rows'][1]['acquiredNs']))

    def test_real_term_wait_between_each_projection_withdraws_all(self):
        for model_name in ('model', 'ambient', 'indicators'):
            with self.subTest(after=model_name):
                self.policy()
                before = self.lamps()
                model = getattr(self.server, model_name)
                original = model.snapshot
                killed = False
                def death_after_actual_copy(*args):
                    nonlocal killed
                    copied = original(*args)
                    if not killed:
                        killed = True
                        self.child.send_signal(signal.SIGTERM)
                        self.assertEqual(self.child.wait(timeout=3), 0)
                    return copied
                with patch.object(model, 'snapshot', side_effect=death_after_actual_copy), \
                        patch.object(self.server, 'poll', wraps=self.server.poll) as poll:
                    after = self.status()
                self.assertEqual(poll.call_count, 1)
                self.assertFalse(after['prephone_vehicle']['publisher']['live'])
                for key in ('prephone_vehicle', 'prephone_indicators'):
                    self.assertTrue(all(row['value'] is None for row in after[key]['rows']))
                    for date in ('sourceGeneration', 'sourceStartedNs', 'sourceSampledNs'):
                        self.assertEqual(after[key][date], before[key][date])
                self.assertFalse(after['ambient_light']['available'])
                self.assertIsNone(after['ambient_light']['requested'])
                self.child = self.launch()
                self.wait_for(lambda result: result['ambient_light']['available'])

    def test_blocked_new_indicator_record_cannot_reanimate_retired_source_generation(self):
        before = self.lamps()['prephone_indicators']
        self.assertEqual(before['rows'][1]['maximumAgeNs'], '10000000000')
        self.send('pn')
        boundary = self.wait_for(lambda result: result['prephone_vehicle']['sourceGeneration'] == '2')
        old = boundary['prephone_indicators']
        self.assertEqual(old['state'], 'generation_mismatch')
        self.assertEqual(old['sourceGeneration'], '1')
        self.assertEqual(old['sourceStartedNs'], before['sourceStartedNs'])
        self.assertEqual(old['rows'][1]['acquiredNs'], before['rows'][1]['acquiredNs'])
        self.assertLess(int(old['rows'][1]['ageNs']), int(old['rows'][1]['maximumAgeNs']))
        self.assertTrue(all(row['value'] is None for row in old['rows']))
        self.send('o')
        new = self.wait_for(lambda result: result['prephone_indicators']['sourceGeneration'] == '2')
        self.assertTrue(all(row['acquiredNs'] is None for row in new['prephone_indicators']['rows']))
        self.send('l')
        self.wait_for(lambda result: result['prephone_indicators']['rows'][1]['value'] == 'OFF')

    def test_genuinely_queued_over16_and_new_generation_on_record16(self):
        self.lamps()
        self.stall()
        self.status()  # Drain original records before counting actual sends.
        self.assertEqual(self.queue(15), 15)
        self.send('n')
        self.line(b'source ')
        self.assertEqual(self.queue(3), 3)
        class CountingSocket:
            def __init__(self, connection):
                self.connection = connection
                self.received = 0
            def __getattr__(self, name):
                return getattr(self.connection, name)
            def recvmsg(self, *args):
                result = self.connection.recvmsg(*args)
                self.received += 1
                return result
        counter = CountingSocket(self.server.connection)
        self.server.connection = counter
        with patch.object(self.server, 'poll', wraps=self.server.poll) as poll:
            boundary = self.status()
        self.assertEqual(counter.received, 16)
        self.assertEqual(poll.call_count, 1)
        self.assertEqual(boundary['prephone_vehicle']['sourceGeneration'], '2')
        lamps = boundary['prephone_indicators']
        self.assertEqual(lamps['sourceGeneration'], '1')
        self.assertEqual(lamps['state'], 'generation_mismatch')
        self.assertTrue(all(row['value'] is None for row in lamps['rows']))
        self.assertLess(int(lamps['rows'][1]['ageNs']), int(lamps['rows'][1]['maximumAgeNs']))
        after = self.status()
        self.assertGreater(counter.received, 16)
        self.assertEqual(after['prephone_indicators']['sourceGeneration'], '2')

    def test_malformed_truncated_oversized_replayed_indicator_records_retire_all(self):
        for command in ('x', 'y', 'z', 'j'):
            with self.subTest(command=command):
                self.lamps()
                self.send(command)
                retired = self.wait_for(lambda result: result['prephone_indicators']['state'] == 'protocol_failure')
                self.assertFalse(retired['prephone_indicators']['publisher']['live'])
                self.assertEqual(retired['prephone_indicators']['error'], 'protocol_failure')
                self.assertTrue(all(row['value'] is None for row in retired['prephone_vehicle']['rows']))
                self.assertTrue(all(row['value'] is None for row in retired['prephone_indicators']['rows']))
                self.assertFalse(retired['ambient_light']['available'])
                self.child.terminate()
                self.assertEqual(self.child.wait(timeout=3), 0)
                self.child = self.launch()
                self.wait_for(lambda result: result['ambient_light']['available'])

    def test_simultaneous_ambient_action_and_indicator_copies_preserve_volatile_policy(self):
        before = self.lamps()
        old = before['ambient_light']['revision']
        enabled = self.policy()
        self.assertIsNone(enabled['ambient_light']['current'])
        self.assertEqual(enabled['prephone_indicators']['rows'][1]['value'], 'OFF')
        self.assertFalse(enabled['ambient_light']['provider_available'])
        self.child.terminate()
        self.assertEqual(self.child.wait(timeout=3), 0)
        self.status()
        self.child = self.launch()
        new = self.wait_for(lambda result: result['ambient_light']['available'])
        self.assertFalse(new['ambient_light']['settings']['enabled'])
        self.assertNotEqual(new['ambient_light']['revision'], old)
        self.assertTrue(all(row['value'] is None for row in new['prephone_indicators']['rows']))
        for key in ('active', 'generation', 'active_mode', 'camera', 'carplay'):
            self.assertEqual(new[key], before[key])
        self.assertEqual(self.manager.bluez.commands, 0)

    def test_actual_signals_and_manager_ownerclose_retire_every_projection(self):
        for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            self.lamps()
            self.child.send_signal(number)
            self.assertEqual(self.child.wait(timeout=3), 0)
            retired = self.status()
            self.assertTrue(all(row['value'] is None for row in retired['prephone_indicators']['rows']))
            self.assertFalse(retired['ambient_light']['available'])
            self.child = self.launch()
            self.wait_for(lambda result: result['ambient_light']['available'])
        self.policy()
        self.lamps()
        self.server.close()
        retired = self.server.observations()
        self.assertFalse(retired[0]['publisher']['live'])
        self.assertTrue(all(row['value'] is None for row in retired[2]['rows']))
        self.assertFalse((self.folder / 'prephone.sock').exists())
        self.assertEqual(self.server.pidfd, -1)
        self.server = NativePublication(self.folder / 'prephone.sock', (os.getuid(), os.getgid()),
                                        self.command, True, indicators_enabled=True)
        self.manager.prephone = self.server
        new = self.wait_for(lambda result: result['ambient_light']['available'])
        self.assertFalse(new['ambient_light']['settings']['enabled'])

    def test_real_lease_long_lamps_and_explicit_invalid_wire_clock(self):
        before = self.lamps()
        self.stall()
        self.status()
        expired = self.wait_for(lambda result: result['prephone_indicators']['state'] == 'publisher_expired')
        self.assertIsNone(self.child.poll())
        self.assertTrue(all(row['value'] is None for row in expired['prephone_indicators']['rows']))
        self.assertEqual(expired['prephone_indicators']['rows'][1]['acquiredNs'],
                         before['prephone_indicators']['rows'][1]['acquiredNs'])
        self.assertFalse(expired['ambient_light']['available'])
        self.child.terminate()
        self.child.wait(timeout=3)
        self.child = self.launch()
        self.wait_for(lambda result: result['ambient_light']['available'])
        self.lamps()
        # Explicit malformed-wire injection, not observed kernel clock reversal.
        self.send('t')
        invalid = self.wait_for(lambda result: result['prephone_indicators']['state'] == 'protocol_failure')
        self.assertTrue(all(row['value'] is None for row in invalid['prephone_indicators']['rows']))
        self.assertFalse(invalid['ambient_light']['available'])

    def test_actual_inherited_writer_and_second_role_admission_clear_all(self):
        self.lamps()
        second = self.launch()
        self.status()
        self.assertTrue(self.status()['prephone_indicators']['publisher']['live'])
        second.terminate()
        second.wait(timeout=3)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as foreign:
            foreign.settimeout(2)
            foreign.connect(str(self.folder / 'prephone.sock'))
            self.server.poll()
            self.assertEqual(foreign.recv(32), b'')
        self.assertTrue(self.status()['prephone_indicators']['publisher']['live'])
        self.send('i')  # Real fork/inherited socket; actual parent waitpid.
        retired = self.wait_for(lambda result: result['prephone_indicators']['state'] == 'protocol_failure')
        self.assertTrue(all(row['value'] is None for row in retired['prephone_indicators']['rows']))
        self.assertTrue(all(row['value'] is None for row in retired['prephone_vehicle']['rows']))
        self.assertFalse(retired['ambient_light']['available'])

    def test_real_kernel_parent_death_and_actual_adopted_waitpid(self):
        # The fixture explicitly requests PDEATHSIG. This verifies a real Linux
        # retirement path, not an appliance parent-death configuration claim.
        self.child.terminate()
        self.assertEqual(self.child.wait(timeout=3), 0)
        self.status()
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        supervisor = None
        pid = None
        reaped = False
        try:
            script = ('import subprocess,sys,signal\n'
                      'child=subprocess.Popen(sys.argv[1:],stdin=sys.stdin,stdout=subprocess.DEVNULL)\n'
                      'print(child.pid,flush=True)\nsignal.pause()\n')
            supervisor = subprocess.Popen([sys.executable, '-B', '-c', script, *self.command],
                                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.children.append(supervisor)
            self.assertTrue(select.select([supervisor.stdout], [], [], 2)[0])
            pid = int(supervisor.stdout.readline())
            # Main fixture is exited, so use genuine status directly here.
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and not self.status()['ambient_light']['available']:
                time.sleep(.01)
            self.assertTrue(self.status()['ambient_light']['available'])
            # This root-held writer stays open after supervisor death, so EOF
            # cannot account for the child's observed PDEATHSIG retirement.
            self.assertFalse(supervisor.stdin.closed)
            supervisor.terminate()
            self.assertEqual(supervisor.wait(timeout=3), -signal.SIGTERM)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                actual, state = os.waitpid(pid, os.WNOHANG)
                if actual:
                    reaped = True
                    self.assertEqual(os.waitstatus_to_exitcode(state), 0)
                    break
                time.sleep(.01)
            self.assertTrue(reaped)
            retired = self.status()
            self.assertFalse(retired['prephone_indicators']['publisher']['live'])
            self.assertTrue(all(row['value'] is None for row in retired['prephone_indicators']['rows']))
            self.assertFalse((self.folder / 'vehicle.sock').exists())
        finally:
            if supervisor and supervisor.poll() is None:
                supervisor.terminate()
                supervisor.wait(timeout=3)
            if pid and not reaped:
                try:
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
                except ProcessLookupError:
                    pass
            self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    def test_legacy_actual_simulator_is_undated_null_and_exact_argv_required(self):
        with self.assertRaises(ValueError):
            NativePublication(self.folder / 'unused.sock', (os.getuid(), os.getgid()), self.command)
        with self.assertRaises(ValueError):
            NativePublication(self.folder / 'unused.sock', (os.getuid(), os.getgid()), self.command[:-1],
                              indicators_enabled=True)
        self.child.terminate()
        self.child.wait(timeout=3)
        self.server.close()
        self.command.append('--legacy')
        self.server = NativePublication(self.folder / 'prephone.sock', (os.getuid(), os.getgid()),
                                        self.command, True, indicators_enabled=True)
        self.manager.prephone = self.server
        self.child = self.launch()
        undated = self.wait_for(lambda result: result['prephone_indicators']['state'] == 'undated_source')
        self.assertIsNone(undated['prephone_indicators']['sourceGeneration'])
        self.assertTrue(all(row['value'] is None and row['acquiredNs'] is None for row in undated['prephone_indicators']['rows']))
        self.assertTrue(all(row['value'] is None for row in undated['prephone_vehicle']['rows']))

    def test_disabled_native_graph_has_no_indicator_module_or_extra_poll(self):
        self.child.terminate()
        self.child.wait(timeout=3)
        self.server.close()
        self.command.remove('--prephone-indicators')
        previous = sys.modules.pop('prephone_indicator_status', None)
        try:
            self.server = NativePublication(self.folder / 'prephone.sock', (os.getuid(), os.getgid()),
                                            self.command, True)
            self.manager.prephone = self.server
            self.manager.base['prephone_indicators'] = False
            self.child = self.launch()
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                result = self.request({'action': 'status'})
                if result['ambient_light']['available']:
                    break
                time.sleep(.01)
            self.assertTrue(result['ambient_light']['available'])
            self.assertIsNone(self.server.indicators)
            self.assertNotIn('prephone_indicator_status', sys.modules)
            with patch.object(self.server, 'poll', wraps=self.server.poll) as poll:
                result = self.request({'action': 'status'})
            self.assertEqual(poll.call_count, 1)
            self.assertFalse(result['prephone_indicators']['configured'])
            self.assertEqual(result['prephone_indicators']['state'], 'disabled')
        finally:
            if previous is not None:
                sys.modules['prephone_indicator_status'] = previous


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Tests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
