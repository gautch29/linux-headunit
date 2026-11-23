# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Linux actual C++ source owner -> private admitted IPC -> manager endpoint.

All acquisitions are explicit local simulations; no phone, display or actuator.
"""
import copy
import json
import os
from pathlib import Path
import select
import signal
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
from prephone_native import NativePublication
from ambient_light_status import encode_settings
from phone_manager_endpoint import ManagerEndpoint
from phone_management_test import Tests
BINARY = str(Path(sys.argv.pop(1)).resolve())


class OwnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ambient-')
        self.folder = Path(self.temp.name).resolve()
        self.folder.chmod(0o700)
        self.command = [BINARY, str(self.folder / 'prephone.sock'), str(self.folder / 'vehicle.sock'), '--ambient']
        self.server = NativePublication(self.folder / 'prephone.sock', (os.getuid(), os.getgid()),
                                        self.command, True)
        self.manager = Tests().quarantine_fixture(self.folder)
        self.manager.prephone = self.server
        self.manager.base['ambient_light'] = True
        self.endpoint = ManagerEndpoint(self.folder, (os.getuid(), os.getgid()))
        self.children = []
        self.child = self.launch()
        self.wait_for(lambda status: status['available'])

    def launch(self):
        child = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
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
                stream.close()
        self.temp.cleanup()

    def request(self, data):
        # Actual strict manager endpoint, JSON framing/credentials and callback.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(2)
            client.connect(str(self.endpoint.path))
            client.sendall(json.dumps(data).encode() + b'\n')
            self.assertTrue(self.endpoint.serve_once(self.manager.request))
            raw = b''
            while not raw.endswith(b'\n'):
                raw += client.recv(65536)
            return json.loads(raw)

    def status(self):
        reply = self.request({'action': 'status'})
        self.assertTrue(reply['ok'], reply)
        self.assertEqual(reply['prephone_vehicle']['publisher'], reply['ambient_light']['publisher'])
        self.assertIsNone(reply['ambient_light']['current'])
        self.assertIsNone(reply['ambient_light']['command'])
        return reply['ambient_light']

    def wait_for(self, condition, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.status()
            if condition(status):
                return status
            time.sleep(.01)
        self.fail('Ambient condition missing: ' + repr(status))

    def send(self, command):
        self.child.stdin.write(command.encode())
        self.child.stdin.flush()

    def stall(self):
        self.send('s')
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            self.assertTrue(select.select([self.child.stdout], [], [], max(0, deadline - time.monotonic()))[0])
            if self.child.stdout.readline() == b'stalled\n':
                return
        self.fail('Native fixture did not confirm its stall')

    def published_packets(self):
        self.send('q')
        self.assertTrue(select.select([self.child.stdout], [], [], 2)[0])
        line = self.child.stdout.readline()
        self.assertTrue(line.startswith(b'packets '), line)
        return int(line.split()[1])

    def policy(self, **values):
        settings = copy.deepcopy(self.status()['settings'])
        settings.update(values)
        return settings

    def change(self, policy):
        return self.request({'action': 'ambient_light_settings', 'revision': self.status()['revision'],
                             'settings': policy})

    def test_status_one_poll_same_clock_and_independent_deep_copies(self):
        self.send('f')
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            reply = self.request({'action': 'status'})
            if any(row['value'] is not None for row in reply['prephone_vehicle']['rows']):
                break
            time.sleep(.01)
        self.assertTrue(any(row['value'] is not None for row in reply['prephone_vehicle']['rows']))
        with patch.object(self.server, 'poll', wraps=self.server.poll) as poll, \
                patch.object(self.server.model, 'snapshot', wraps=self.server.model.snapshot) as vehicle, \
                patch.object(self.server.ambient, 'snapshot', wraps=self.server.ambient.snapshot) as ambient:
            reply = self.request({'action': 'status'})
        self.assertEqual(poll.call_count, 1)
        self.assertEqual(vehicle.call_args.args, ambient.call_args.args)
        self.assertEqual(reply['prephone_vehicle']['publisher'], reply['ambient_light']['publisher'])
        pair = self.server.snapshots()
        pair[0]['rows'][0]['value'] = 999
        pair[0]['publisher']['live'] = False
        pair[1]['settings']['fixed_rgbw'][0] = 255
        self.assertTrue(pair[1]['publisher']['live'])
        fresh = self.server.snapshots()
        self.assertNotEqual(fresh[0]['rows'][0]['value'], 999)
        self.assertNotEqual(fresh[1]['settings']['fixed_rgbw'][0], 255)

    def test_status_real_retirement_during_projection_withdraws_both(self):
        policy = self.policy(enabled=True, selection='fixed', night_policy='off')
        self.assertTrue(self.change(policy)['ok'])
        self.wait_for(lambda status: status['requested'] is not None)
        self.send('f')
        # Preserve genuine source identity after death; no invented observation.
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            before = self.request({'action': 'status'})
            if any(row['value'] is not None for row in before['prephone_vehicle']['rows']):
                break
            time.sleep(.01)
        self.assertTrue(any(row['value'] is not None for row in before['prephone_vehicle']['rows']))
        original = self.server.model.snapshot
        killed = False

        def death_after_actual_vehicle_copy(now):
            nonlocal killed
            result = original(now)
            if not killed:
                killed = True
                self.child.send_signal(signal.SIGTERM)
                self.assertEqual(self.child.wait(timeout=3), 0)
            return result

        with patch.object(self.server.model, 'snapshot', side_effect=death_after_actual_vehicle_copy), \
                patch.object(self.server, 'poll', wraps=self.server.poll) as poll:
            after = self.request({'action': 'status'})
        vehicle, ambient = after['prephone_vehicle'], after['ambient_light']
        self.assertEqual(vehicle['publisher'], ambient['publisher'])
        self.assertFalse(vehicle['publisher']['live'])
        self.assertTrue(all(row['value'] is None for row in vehicle['rows']))
        self.assertFalse(ambient['available'])
        for key in ('requested', 'settings', 'current', 'command', 'revision'):
            self.assertIsNone(ambient[key])
        for key in ('sourceGeneration', 'sourceStartedNs', 'sourceSampledNs'):
            self.assertEqual(vehicle[key], before['prephone_vehicle'][key])
        self.assertEqual(poll.call_count, 1)

    def test_status_saturated_real_native_socket_keeps_original_receive_budget(self):
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

        # Actual production 200 ms cadence queues both original packet families.
        # No fixture clock, injected receipt, cadence or socket size adjustment.
        self.stall()
        self.request({'action': 'status'})
        baseline = self.published_packets()
        self.send('r')
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.published_packets() - baseline >= 18:
                break
            time.sleep(.02)
        else:
            self.fail('Actual native publisher did not queue 18 successful sends')
        self.stall()
        counter = CountingSocket(self.server.connection)
        self.server.connection = counter
        with patch.object(self.server, 'poll', wraps=self.server.poll) as poll:
            reply = self.request({'action': 'status'})
        self.assertEqual(counter.received, 16)
        self.assertEqual(poll.call_count, 1)
        self.assertEqual(reply['prephone_vehicle']['publisher'], reply['ambient_light']['publisher'])
        # Saturation leaves genuine queued data for the next bounded observation.
        previous = counter.received
        self.request({'action': 'status'})
        self.assertGreater(counter.received, previous)

    def test_joint_status_disabled_graph_keeps_one_vehicle_poll(self):
        self.child.terminate()
        self.child.wait(timeout=3)
        self.server.close()
        self.command = self.command[:-1]
        self.server = NativePublication(self.folder / 'prephone.sock', (os.getuid(), os.getgid()),
                                        self.command)
        self.manager.prephone = self.server
        self.manager.base['ambient_light'] = False
        self.child = self.launch()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            reply = self.request({'action': 'status'})
            if reply['prephone_vehicle']['publisher']['live']:
                break
            time.sleep(.01)
        self.assertTrue(reply['prephone_vehicle']['publisher']['live'])
        self.assertIsNone(self.server.ambient)
        with patch.object(self.server, 'poll', wraps=self.server.poll) as poll:
            reply = self.request({'action': 'status'})
        self.assertEqual(poll.call_count, 1)
        self.assertEqual(reply['ambient_light']['state'], 'disabled')
        self.assertFalse(reply['ambient_light']['configured'])
        self.assertIsNone(reply['ambient_light']['settings'])

    def test_standalone_snapshot_contracts_poll_once_and_keep_original_dates(self):
        for method in (self.server.snapshot, self.server.ambient_snapshot, self.manager.ambient_snapshot):
            with self.subTest(method=method.__name__), \
                    patch.object(self.server, 'poll', wraps=self.server.poll) as poll:
                result = method()
                self.assertEqual(poll.call_count, 1)
                self.assertTrue(result['publisher']['live'])
        self.stall()
        before = self.request({'action': 'status'})
        after = self.request({'action': 'status'})
        self.assertEqual(after['prephone_vehicle']['publisher'], after['ambient_light']['publisher'])
        self.assertEqual(after['prephone_vehicle']['publisher'], before['prephone_vehicle']['publisher'])
        self.assertEqual(after['ambient_light']['sampled_ns'], before['ambient_light']['sampled_ns'])

    def test_manager_revision_bound_policy_separate_from_physical_and_phone_state(self):
        before = self.request({'action': 'status'})
        old_revision = before['ambient_light']['revision']
        policy = self.policy(enabled=True, selection='fixed', fixed_rgbw=[9, 37, 83, 0], night_policy='off')
        reply = self.change(policy)
        self.assertTrue(reply['ok'])
        self.assertFalse(reply['confirmed'])
        updated = self.wait_for(lambda status: status['software_action'] and
                               status['software_action']['status'] == 'software_updated')
        self.assertEqual(updated['settings'], policy)
        self.assertEqual(updated['requested']['value']['rgbw'], [9, 37, 83, 0])
        self.assertIsNone(updated['current'])
        self.assertIsNone(updated['command'])
        self.assertFalse(updated['provider_available'])
        stale = self.request({'action': 'ambient_light_settings', 'revision': old_revision, 'settings': policy})
        self.assertFalse(stale['ok'])
        changed_copy = self.status()
        changed_copy['settings']['fixed_rgbw'][0] = 255
        self.assertEqual(self.status()['settings']['fixed_rgbw'][0], 9)
        after = self.request({'action': 'status'})
        for key in ('active', 'generation', 'active_mode', 'camera', 'carplay'):
            self.assertEqual(after[key], before[key])
        self.assertEqual(self.manager.bluez.commands, 0)
        disabled = self.policy(enabled=False)
        self.assertTrue(self.change(disabled)['ok'])
        off = self.wait_for(lambda status: status['settings']['enabled'] is False)
        self.assertEqual(off['requested']['source'], 'disabled')
        self.assertIsNone(off['current'])

    def test_missing_invalid_concurrent_action_and_model_expiry(self):
        self.assertFalse(self.request({'action': 'ambient_light_settings', 'settings': {}})['ok'])
        bad = self.policy(day_brightness_permille=True)
        self.assertFalse(self.change(bad)['ok'])
        self.assertFalse(self.request({'action': 'status', 'settings': {}})['ok'])
        policy = self.policy(enabled=True, selection='screen')
        self.stall()
        self.assertTrue(self.change(policy)['ok'])
        self.assertFalse(self.change(policy)['ok'])
        expired = self.wait_for(lambda status: not status['available'])
        self.assertEqual(expired['state'], 'model_expired')
        self.assertIsNone(expired['requested'])
        self.assertIsNone(expired['settings'])
        timeout = self.wait_for(lambda status: status['software_action']['status'] == 'timed_out')
        self.assertFalse(timeout['software_action']['physical_confirmed'])
        self.assertFalse(self.change(policy)['ok'])
        # An expired command is rejected by the genuine resumed C++ handler.
        self.send('r')
        resumed = self.wait_for(lambda status: status['available'])
        self.assertFalse(resumed['settings']['enabled'])
        self.assertEqual(resumed['software_action']['status'], 'timed_out')

    def test_actual_handler_failure_preserves_policy_and_reports_no_physical_completion(self):
        now = self.server._observe()
        original = self.server.ambient.snapshot(now)
        data = {'action': 'ambient_light_settings', 'revision': original['revision'], 'settings': original['settings']}
        raw, pending = self.server.ambient.prepare(data, now, self.server.nonce)
        corrupt = bytearray(raw)
        struct.pack_into('<H', corrupt, 64 + 8, 1001)
        self.assertEqual(self.server.connection.send(corrupt), len(corrupt))
        self.server.ambient.sent(pending)
        failed = self.wait_for(lambda status: status['software_action'] and status['software_action']['status'] == 'failed')
        self.assertEqual(failed['software_action']['reason'], 'invalid_settings')
        self.assertEqual(failed['settings'], original['settings'])
        self.assertEqual(failed['revision'], original['revision'])
        self.assertIsNone(failed['current'])

    def test_original_dated_mode_headlights_delta_and_expiry_no_saved_profile_observation(self):
        policy = self.policy(enabled=True, selection='drive_mode', night_policy='headlights',
                             mode_colours=[{'id': 'sport', 'rgbw': [240, 31, 9, 0]}])
        self.assertTrue(self.change(policy)['ok'])
        self.wait_for(lambda status: status['settings']['enabled'])
        self.assertIsNone(self.status()['requested'])
        self.send('m')
        active = self.wait_for(lambda status: status['requested'] is not None)
        request = active['requested']
        self.assertEqual(request['source'], 'drive_mode')
        self.assertEqual(request['night_source'], 'headlights')
        self.assertEqual(request['value']['brightness_permille'], policy['night_brightness_permille'])
        self.assertEqual(request['colour_observation']['origin'], 'simulation')
        acquired = request['colour_observation']['acquired_ns']
        # An unrelated original fuel delta does not redraft mode/headlight dates.
        self.send('f')
        later = self.status()
        if later['requested']:
            self.assertEqual(later['requested']['colour_observation']['acquired_ns'], acquired)
        expired = self.wait_for(lambda status: status['requested'] is None)
        self.assertTrue(expired['available'])
        self.assertIsNone(expired['current'])
        self.send('m')
        renewed = self.wait_for(lambda status: status['requested'] is not None)
        self.assertGreater(int(renewed['requested']['colour_observation']['acquired_ns']), int(acquired))
        self.send('w')
        withdrawn = self.wait_for(lambda status: status['requested'] is None)
        self.assertTrue(withdrawn['available'])

    def test_real_retirement_signals_successor_nonce_old_revision_and_second_owner(self):
        old = self.status()['revision']
        second = self.launch()
        deadline = time.monotonic() + .3
        while time.monotonic() < deadline:
            self.status()
            time.sleep(.01)
        self.assertTrue(self.status()['available'])
        second.terminate()
        second.wait(timeout=3)
        for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=number):
                self.child.send_signal(number)
                self.assertEqual(self.child.wait(timeout=3), 0)
                retired = self.status()
                self.assertFalse(retired['available'])
                self.assertIsNone(retired['requested'])
                self.assertIsNone(retired['current'])
                self.assertFalse(self.request({'action': 'ambient_light_settings', 'revision': old,
                                              'settings': {}})['ok'])
                self.child = self.launch()
                current = self.wait_for(lambda status: status['available'])
                self.assertNotEqual(current['revision'], old)
                self.assertFalse(current['settings']['enabled'])
                old = current['revision']

    def test_shutdown_pending_action_endpoint_retired_and_fresh_successor(self):
        policy = self.policy(enabled=True, selection='fixed', night_policy='off')
        self.stall()
        self.assertTrue(self.change(policy)['ok'])
        self.manager.shutdown()
        retired = self.server.ambient.snapshot(self.server.last_now)
        self.assertFalse(retired['available'])
        self.assertEqual(retired['software_action']['status'], 'failed')
        self.assertEqual(retired['software_action']['reason'], 'owner_retired')
        self.assertFalse((self.folder / 'prephone.sock').exists())
        self.assertEqual(self.server.pidfd, -1)


if __name__ == '__main__':
    unittest.main()
