# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual child-exit recovery with explicit radio and validation substitutes."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from phone_manager import Manager

A, B = 'AA:BB:CC:DD:EE:01', 'AA:BB:CC:DD:EE:02'


class Phones:
    def __init__(self):
        self.disconnected = []
        self.forgotten = []

    def phones(self):
        return [{'address': A, 'name': 'Fixture', 'connected': False}]

    def phone(self, phone):
        return {'address': phone}

    def disconnect(self, phone):
        self.disconnected.append(phone)

    def forget(self, phone):
        self.forgotten.append(phone)

    def media(self, phone):
        return {'available': False}


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pm-recovery-', dir='/tmp')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.root.chmod(0o700)
        self.display = self.root / 'standard.conf'
        self.display.write_text('explicit display-validator fixture')
        self.config = self.root / 'receiver.json'
        self.config.write_text(json.dumps({'pair_store': str(self.root / 'pairs'),
                                           'display_config': str(self.display)}))
        self.phones = Phones()
        self.manager = Manager(self.config, self.root, self.display,
                               (os.getuid(), os.getgid()), bluez=self.phones, recovery_attempts=3)
        self.children = []
        self.actual_popen = subprocess.Popen
        self.now = 100.
        self.validation = patch('phone_manager.subprocess.run')
        self.validation.start()
        self.addCleanup(self.validation.stop)
        self.launch = patch('phone_manager.subprocess.Popen', side_effect=self.spawn)
        self.launch_mock = self.launch.start()
        self.addCleanup(self.launch.stop)
        self.clock = patch('phone_manager.time.monotonic', side_effect=lambda: self.now)
        self.clock.start()
        self.addCleanup(self.clock.stop)
        self.addCleanup(self.retire)

    def spawn(self, command, **options):
        # Only the child program and its radio/configuration boundaries are
        # substitutes. Manager start/stop, state files and retirement run as-is.
        child = self.actual_popen([sys.executable, '-c',
                                   'import sys; sys.exit(int(sys.stdin.buffer.read(1)))'],
                                  stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL,
                                  pass_fds=options.get('pass_fds', ()))
        self.children.append(child)
        return child

    def retire(self):
        self.manager.shutdown()
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
            child.stdin.close()

    def exit_child(self, code=7):
        child = self.manager.child
        child.stdin.write(str(code).encode())
        child.stdin.flush()
        child.wait(timeout=5)
        self.manager.tick()

    def publish_ready(self, *, sequence=1, connection=1, state='ready', home_requests=0):
        from phone_state import atomic_json
        self.manager.carplay.clock = lambda: int(self.now * 1e9)
        atomic_json(self.manager.carplay.path,
                    {'version': 3, 'nonce': self.manager.carplay.nonce,
                     'sequence': str(sequence), 'connectionGeneration': str(connection),
                     'updatedMonotonicNs': str(int(self.now * 1e9)),
                     'state': state, 'returnReady': state in ('ready', 'home'),
                     'negotiatedMode': 'ultra', 'phoneHomeRequests': str(home_requests)},
                    self.manager.owner)

    def ready_status_fixture(self):
        from carplay_readiness import clock_ns
        from phone_state import atomic_json
        self.manager.start(A)
        self.publish_ready(home_requests=4)
        self.manager.layout_path = self.root / 'layout'
        style = '00000000-1111-2222-3333-444444444444'
        atomic_json(Path(str(self.manager.layout_path) + '.status'),
                    {'version': 2, 'nonce': self.manager.carplay.nonce,
                     'connectionGeneration': '1', 'sequence': '1',
                     'updatedMonotonicNs': str(clock_ns()),
                     'active': style, 'pending': '', 'ready': True,
                     'transitioning': False, 'error': '',
                     'choices': [{'id': style, 'name': 'Fixture'}]},
                    self.manager.owner)
        status = self.manager.request({'action': 'status'})
        self.assertTrue(status['carplay']['returnReady'])
        self.assertIsNotNone(status['ultra_cluster'])
        self.assertEqual(status['phone_home']['requests'], 4)
        return self.manager.child

    def assert_status_withdrawn(self, status):
        self.assertFalse(status['carplay']['returnReady'])
        self.assertIsNone(status['ultra_cluster'])
        self.assertEqual(status['cluster_formats']['ultra']['choices'], [])
        self.assertEqual(status['media']['controls'], [])
        self.assertFalse(status['recovery']['schedule']['queued'])

    def status_lookup_exit(self, signalled):
        child = self.ready_status_fixture()

        def lookup():
            # Explicit BlueZ substitute; the owned child and readiness reader
            # are real. Death is completed before lookup returns.
            if signalled:
                child.kill()
            else:
                child.stdin.write(b'7')
                child.stdin.flush()
            child.wait(timeout=5)
            return Phones.phones(self.phones)

        with patch.object(self.phones, 'phones', side_effect=lookup):
            status = self.manager.request({'action': 'status'})
        self.assert_status_withdrawn(status)
        self.assertIsNone(status['active_mode'])
        self.assertEqual(status['phone_home']['requests'], 0)
        self.assertEqual(self.manager.retirement_required, signalled)
        self.assertEqual(status['generation'], self.manager.generation)
        return status

    def test_status_lookup_cannot_retain_ready_after_actual_launcher_exit(self):
        self.status_lookup_exit(False)
        self.manager.tick()
        self.assertIsNone(self.manager.child)
        self.assertEqual(self.manager.recovery_at, self.now + 2)
        self.manager.request({'action': 'disconnect', 'phone': A,
                              'generation': self.manager.generation})
        self.now += 100
        self.manager.tick()
        self.assertEqual(len(self.children), 1)

    def test_status_lookup_signal_death_preserves_whole_service_quarantine(self):
        status = self.status_lookup_exit(True)
        self.assertIn('retire the whole manager service', status['error'])
        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
            self.manager.tick()
        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
            self.manager.start(B)
        self.assertEqual(len(self.children), 1)

    def test_status_lookup_cannot_renew_expired_receiver_readiness(self):
        self.ready_status_fixture()
        original = self.manager.carplay.path.read_bytes()

        def lookup():
            self.now += 2  # Explicit injected clock exceeds the original 1.5 s age.
            return Phones.phones(self.phones)

        with patch.object(self.phones, 'phones', side_effect=lookup):
            status = self.manager.request({'action': 'status'})
        self.assert_status_withdrawn(status)
        self.assertEqual(status['carplay']['state'], 'starting')
        self.assertIsNone(status['active_mode'])
        self.assertEqual(status['phone_home']['requests'], 0)
        self.assertEqual(self.manager.carplay.path.read_bytes(), original)
        self.assertIsNotNone(self.manager.child)
        self.assertIsNone(self.manager.recovery_at)

    def status_lookup_connection(self, state):
        self.ready_status_fixture()
        generation = self.manager.generation

        def lookup():
            # A reconnect within the same launcher publishes a different
            # fixture connection; old catalogue bytes remain untouched.
            self.publish_ready(sequence=2, connection=2, state=state, home_requests=1)
            return Phones.phones(self.phones)

        with patch.object(self.phones, 'phones', side_effect=lookup):
            status = self.manager.request({'action': 'status'})
        self.assertEqual(status['carplay'], {'state': state, 'returnReady': state == 'ready'})
        self.assertEqual(status['phone_home']['connection'], '2')
        self.assertEqual(status['phone_home']['requests'], 1)
        self.assertIsNone(status['ultra_cluster'])
        self.assertEqual(status['cluster_formats']['ultra']['choices'], [])
        self.assertEqual(status['generation'], generation)
        self.assertEqual(self.manager.active, A)
        self.assertIsNone(self.manager.recovery_at)

    def test_status_lookup_current_connection_starting_withdraws_old_ready(self):
        self.status_lookup_connection('starting')

    def test_status_lookup_current_connection_ready_rejects_old_catalogue(self):
        self.status_lookup_connection('ready')

    def test_failed_manual_camera_cannot_block_phone_retirement(self):
        from unittest.mock import Mock
        self.manager.start(A)
        camera = Mock()
        camera.request.side_effect = ValueError('failed manual worker')
        self.manager.camera = camera
        self.manager.stop()
        camera.request.assert_called_once_with(False)
        self.assertIsNone(self.manager.child)
        self.assertIsNone(self.manager.active)
        self.assertIsNone(self.manager.carplay.nonce)
        self.manager.camera = None

    def test_signed_camera_is_unavailable_without_an_ultra_render_owner(self):
        from unittest.mock import Mock
        self.manager.base['camera_state_config'] = str(self.root / 'camera-state.json')
        camera = Mock()
        camera.snapshot.return_value = {'configured': True, 'requested': False,
                                        'phase': 'unavailable', 'error': ''}
        self.manager.camera = camera
        self.assertEqual(self.manager.camera_snapshot()['phase'], 'stopped')
        with self.assertRaisesRegex(ValueError, 'Connect CarPlay Ultra'):
            self.manager.request({'action': 'camera', 'enabled': True})
        camera.request.assert_not_called()
        self.manager.start(A)
        self.manager.active_mode = 'standard'
        self.assertFalse(self.manager.camera_snapshot()['configured'])
        self.manager.active_mode = 'ultra'
        self.assertEqual(self.manager.camera_snapshot(), camera.snapshot.return_value)
        self.manager.camera = None

    def test_three_failures_recover_after_retirement_then_budget_exhausts(self):
        self.manager.start(A)
        for delay, remaining in ((2, 3), (4, 2), (8, 1)):
            with self.subTest(delay=delay):
                old_nonce = self.manager.carplay.nonce
                old_generation = self.manager.generation
                self.manager.media.publish({'available': True, 'title': 'Retiring fixture'})
                self.manager.iap_media_path.write_text('{"available":true}')
                self.exit_child()
                self.assertIsNone(self.manager.child)
                self.assertIsNone(self.manager.active)
                self.assertIsNone(self.manager.carplay.nonce)
                self.assertFalse(self.manager.media.last['available'])
                self.assertFalse(self.manager.iap_media_path.exists())
                self.assertEqual(self.manager.state.selected, A)
                self.assertEqual(self.manager.generation, old_generation + 1)
                self.assertEqual(self.manager.recovery_remaining, remaining)
                self.assertEqual(self.manager.recovery_at, self.now + delay)
                launches = len(self.children)
                self.now += delay - .001
                self.manager.tick()
                self.assertEqual(len(self.children), launches)
                self.now += .001
                self.manager.tick()
                self.assertEqual(len(self.children), launches + 1)
                self.assertNotEqual(self.manager.carplay.nonce, old_nonce)
                self.assertEqual(self.manager.active, A)
                self.assertEqual(self.manager.recovery_remaining, remaining - 1)
                self.assertFalse(self.manager.carplay_snapshot()['returnReady'])
        self.exit_child()
        self.assertIsNone(self.manager.recovery_at)
        self.assertIn('choose Connect to retry', self.manager.error)
        self.now += 1000
        self.manager.tick()
        self.assertEqual(len(self.children), 4)

    def test_readiness_never_replenishes_the_failure_budget(self):
        from phone_state import atomic_json
        self.manager.start(A)
        self.exit_child()
        self.now += 2
        self.manager.tick()
        self.manager.carplay.clock = lambda: int(self.now * 1e9)
        atomic_json(self.manager.carplay.path,
                    {'version': 2, 'nonce': self.manager.carplay.nonce, 'sequence': '1',
                     'connectionGeneration': '1', 'updatedMonotonicNs': str(int(self.now * 1e9)),
                     'state': 'ready', 'returnReady': True, 'negotiatedMode': 'ultra'},
                    self.manager.owner)
        self.assertTrue(self.manager.carplay_snapshot()['returnReady'])
        self.manager.tick()
        self.assertEqual(self.manager.recovery_remaining, 2)
        self.exit_child()
        self.assertEqual(self.manager.recovery_remaining, 2)
        self.assertEqual(self.manager.recovery_at, self.now + 4)

    def test_recovery_preserves_the_real_manager_lock(self):
        import fcntl
        from phone_manager_endpoint import ManagerEndpoint
        with ManagerEndpoint(self.root, self.manager.owner) as endpoint:
            self.manager.receiver_lock_fd = endpoint.inherited_lock_fd
            self.manager.start(A)
            self.exit_child()
            with (self.root / 'manager.lock').open('rb') as contender:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(contender.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.now += 2
            self.manager.tick()
            self.assertEqual(self.manager.active, A)
            self.assertEqual(self.launch_mock.call_args.kwargs['pass_fds'], (endpoint.inherited_lock_fd,))
            self.manager.shutdown()
            self.manager.receiver_lock_fd = None

    def test_disconnect_and_shutdown_cancel_delayed_recovery(self):
        for action in ('disconnect', 'shutdown'):
            with self.subTest(action=action):
                self.manager.start(A)
                self.exit_child()
                self.assertIsNotNone(self.manager.recovery_at)
                if action == 'disconnect':
                    status = self.manager.request({'action': 'status'})
                    self.assertIsNone(status['active'])
                    self.assertFalse(status['carplay']['returnReady'])
                    self.assertEqual({key: status['recovery'][key] for key in ('phone', 'remainingAttempts')},
                                     {'phone': A, 'remainingAttempts': 3})
                    self.manager.request({'action': action, 'phone': A, 'generation': status['generation']})
                else:
                    self.manager.shutdown()
                count = len(self.children)
                self.now += 100
                self.manager.tick()
                self.assertEqual(len(self.children), count)
                self.assertIsNone(self.manager.recovery_phone)
                self.assertNotIn('retrying in', self.manager.error)

    def test_delayed_cancel_cannot_stop_a_replacement_launch(self):
        self.manager.start(A)
        self.exit_child()
        generation = self.manager.generation
        self.now += 2
        self.manager.tick()
        with self.assertRaisesRegex(ValueError, 'Session changed'):
            self.manager.request({'action': 'disconnect', 'phone': A, 'generation': generation})
        self.assertIsNotNone(self.manager.child)
        self.assertEqual(self.manager.active, A)

    def test_explicit_connect_supersedes_pending_phone_and_restores_budget(self):
        self.manager.start(A)
        self.exit_child()
        self.manager.request({'action': 'connect', 'phone': B})
        self.assertEqual(self.manager.active, B)
        self.assertEqual(self.manager.recovery_phone, B)
        self.assertEqual(self.manager.recovery_remaining, 3)
        self.assertIsNone(self.manager.recovery_at)
        self.now += 100
        self.manager.tick()
        self.assertEqual(len(self.children), 2)

    def test_remove_or_replaced_selection_cancels_pending_recovery(self):
        for operation in ('forget', 'replace'):
            with self.subTest(operation=operation):
                self.manager.start(A)
                self.exit_child()
                if operation == 'forget':
                    self.manager.request({'action': 'forget', 'phone': A})
                    self.assertIn(A, self.phones.forgotten)
                else:
                    self.manager.state.select(B)
                count = len(self.children)
                self.now += 100
                self.manager.tick()
                self.assertEqual(len(self.children), count)
                self.assertIsNone(self.manager.recovery_at)

    def test_pairing_admission_cancels_pending_recovery(self):
        class Pairing:
            phase = 'idle'
            def snapshot(self):
                return {'phase': self.phase, 'supported': True}
            def start(self):
                self.phase = 'starting'
            def close(self):
                self.phase = 'idle'
            def wait_closed(self):
                pass
        self.manager.pairing = Pairing()
        self.manager.start(A)
        self.exit_child()
        self.manager.request({'action': 'pairing_start'})
        self.now += 100
        self.manager.tick()
        self.assertEqual(len(self.children), 1)
        self.assertIsNone(self.manager.recovery_at)

    def test_changed_pairing_state_refuses_automatic_launch(self):
        class Pairing:
            def snapshot(self):
                return {'phase': 'confirmation'}
        self.manager.start(A)
        self.exit_child()
        self.manager.pairing = Pairing()
        try:
            self.now += 100
            self.manager.tick()
            self.assertEqual(len(self.children), 1)
            self.assertIsNone(self.manager.recovery_at)
        finally:
            self.manager.pairing = None

    def test_preflight_failure_stops_recovery_without_another_child(self):
        self.manager.start(A)
        self.exit_child()
        self.now += 2
        with patch('phone_manager.subprocess.run', side_effect=ValueError('changed inputs')):
            self.manager.tick()
        self.assertEqual(len(self.children), 1)
        self.assertIsNone(self.manager.recovery_at)
        self.assertIn('Receiver recovery failed; choose Connect to retry', self.manager.error)
        self.now += 100
        self.manager.tick()
        self.assertEqual(len(self.children), 1)

    def test_uncertain_child_retirement_never_starts_a_competitor(self):
        self.manager.start(A)
        child = self.manager.child
        with patch.object(child, 'terminate'), patch.object(child, 'wait', side_effect=subprocess.TimeoutExpired('fixture', 15)):
            with self.assertRaisesRegex(RuntimeError, 'still stopping'):
                self.manager.request({'action': 'disconnect'})
        self.assertIs(self.manager.child, child)
        self.assertIsNone(self.manager.recovery_phone)
        self.now += 100
        self.manager.tick()
        self.assertEqual(len(self.children), 1)

    def test_abrupt_launcher_death_does_not_assume_descendant_cleanup(self):
        self.manager.start(A)
        child = self.manager.child
        child.kill()
        child.wait(timeout=5)
        self.assertLess(child.returncode, 0)
        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
            self.manager.tick()
        self.assertIsNone(self.manager.recovery_at)
        self.assertTrue(self.manager.retirement_required)
        self.assert_no_schedule()
        self.now += 100
        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
            self.manager.tick()
        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
            self.manager.start(A)
        self.assertEqual(len(self.children), 1)
        self.assertIn('retire the whole manager service', self.manager.error)

    def test_clean_exit_and_disabled_policy_require_manual_retry(self):
        for attempts, code in ((3, 0), (0, 7)):
            with self.subTest(attempts=attempts, code=code):
                self.manager.recovery_attempts = attempts
                self.manager.start(A)
                self.exit_child(code)
                self.assertIsNone(self.manager.recovery_at)
                self.assert_no_schedule('disabled' if attempts == 0 else 'idle')
                count = len(self.children)
                self.now += 100
                self.manager.tick()
                self.assertEqual(len(self.children), count)
                self.assertIn('choose Connect to retry', self.manager.error)


    def schedule_status(self):
        return self.manager.request({'action': 'status'})['recovery']['schedule']

    def assert_no_schedule(self, state='idle'):
        status = self.manager.request({'action': 'status'})
        self.assertEqual({key: status['recovery'][key] for key in ('phone', 'remainingAttempts')},
                         {'phone': None, 'remainingAttempts': 0})
        schedule = status['recovery']['schedule']
        self.assertEqual(schedule['state'], state)
        self.assertFalse(schedule['queued'])
        self.assertIsNone(schedule['deadlineMonotonicSeconds'])
        self.assertIsNone(schedule['remainingWaitSeconds'])
        return status

    def test_schedule_default_zero_and_strict_maximum_three(self):
        default = Manager(self.config, self.root, self.display, self.manager.owner, bluez=self.phones)
        try:
            status = default.request({'action': 'status'})
            self.assertEqual(status['recovery']['schedule']['configuredMaximum'], 0)
            self.assertEqual(status['recovery']['schedule']['state'], 'disabled')
            self.assertFalse(status['recovery']['schedule']['queued'])
            self.assertIsNone(status['recovery']['schedule']['deadlineMonotonicSeconds'])
            self.assertIsNone(status['recovery']['schedule']['remainingWaitSeconds'])
        finally:
            default.shutdown()
        self.assertEqual(self.schedule_status()['configuredMaximum'], 3)
        for invalid in (-1, 4, True, 3.0, '3', None):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                Manager(self.config, self.root, self.display, self.manager.owner,
                        bluez=self.phones, recovery_attempts=invalid)

    def test_schedule_original_float_deadline_equal_dates_and_copy_isolation(self):
        self.now = 100.123456789876
        self.manager.start(A)
        self.exit_child()
        deadline = self.manager.recovery_at
        generation = self.manager.generation
        first = self.schedule_status()
        self.assertEqual(first, self.schedule_status())
        self.assertEqual(first, {'authority': 'software_scheduling_intent', 'configuredMaximum': 3,
                                 'queued': True, 'state': 'queued',
                                 'clockDomain': 'python-time.monotonic',
                                 'observedMonotonicSeconds': self.now,
                                 'deadlineMonotonicSeconds': deadline,
                                 'remainingWaitSeconds': deadline - self.now})
        first['deadlineMonotonicSeconds'] = -1
        first['queued'] = False
        self.now += .375
        second = self.schedule_status()
        self.assertEqual(second['deadlineMonotonicSeconds'], deadline)
        self.assertEqual(second['observedMonotonicSeconds'], self.now)
        self.assertEqual(second['remainingWaitSeconds'], deadline - self.now)
        self.assertEqual(self.manager.recovery_at, deadline)
        self.assertEqual(self.manager.generation, generation)
        self.assertEqual(self.manager.recovery_remaining, 3)
        self.assertEqual(len(self.children), 1)

    def test_schedule_overdue_query_never_dispatches_or_consumes_queue(self):
        self.manager.start(A)
        self.exit_child()
        deadline = self.manager.recovery_at
        self.now = deadline + 100
        with patch.object(self.manager, 'start', side_effect=AssertionError('status launched')), \
             patch.object(self.manager, 'cancel_recovery', side_effect=AssertionError('status cancelled')), \
             patch.object(self.manager, 'tick', side_effect=AssertionError('status ticked')):
            for _ in range(3):
                snapshot = self.schedule_status()
                self.assertTrue(snapshot['queued'])
                self.assertEqual(snapshot['state'], 'queued')
                self.assertEqual(snapshot['remainingWaitSeconds'], 0.0)
                self.assertEqual(snapshot['deadlineMonotonicSeconds'], deadline)
        self.assertEqual(self.manager.recovery_remaining, 3)
        self.assertEqual(self.manager.recovery_phone, A)
        self.assertEqual(len(self.children), 1)
        old_generation = self.manager.generation
        self.manager.tick()
        self.assertEqual(len(self.children), 2)
        self.assertEqual(self.manager.recovery_remaining, 2)
        self.assertGreater(self.assert_no_schedule()['generation'], old_generation)

    def test_schedule_snapshot_missing_clock_has_no_fabricated_deadline_or_wait(self):
        self.manager.start(A)
        self.exit_child()
        deadline = self.manager.recovery_at
        with patch('phone_manager.time.monotonic', side_effect=AssertionError('snapshot sampled clock')):
            for unknown in (None, True, float('nan'), float('inf'), -1, '100'):
                with self.subTest(unknown=unknown):
                    snapshot = self.manager.recovery_schedule_snapshot(unknown)
                    self.assertTrue(snapshot['queued'])
                    for key in ('clockDomain', 'observedMonotonicSeconds',
                                'deadlineMonotonicSeconds', 'remainingWaitSeconds'):
                        self.assertIsNone(snapshot[key])
        self.assertEqual(self.manager.recovery_at, deadline)
        self.manager.recovery_attempts = 0
        snapshot = self.manager.recovery_schedule_snapshot(self.now)
        self.assertEqual(snapshot['state'], 'disabled')
        self.assertFalse(snapshot['queued'])
        self.assertIsNone(snapshot['deadlineMonotonicSeconds'])
        self.assertIsNone(snapshot['remainingWaitSeconds'])

    def test_schedule_clock_and_fields_are_observed_after_locked_lookup(self):
        self.manager.start(A)
        child = self.manager.child
        original = self.manager.generation
        def lookup():
            self.assertTrue(self.manager.lock._is_owned())
            child.stdin.write(b'7')
            child.stdin.flush()
            child.wait(timeout=5)
            self.now = 400.123456789
            self.manager.tick()  # Explicit lookup fixture mutation by the EXISTING owner.
            self.now += .5
            return Phones.phones(self.phones)
        with patch.object(self.phones, 'phones', side_effect=lookup):
            status = self.manager.request({'action': 'status'})
        snapshot = status['recovery']['schedule']
        self.assertTrue(snapshot['queued'])
        self.assertEqual(snapshot['observedMonotonicSeconds'], self.now)
        self.assertEqual(snapshot['deadlineMonotonicSeconds'], self.now + 1.5)
        self.assertEqual(snapshot['remainingWaitSeconds'], 1.5)
        self.assertEqual(status['generation'], original + 1)
        self.assertEqual(status['recovery']['phone'], A)
        self.assertFalse(status['carplay']['returnReady'])
        with patch('phone_manager.time.monotonic', return_value=self.now) as clock:
            self.schedule_status()
            clock.assert_called_once_with()

    def test_schedule_lookup_cancellation_cannot_leak_old_queue(self):
        self.manager.start(A)
        self.exit_child()
        def lookup():
            self.manager.cancel_recovery()  # Simulated synchronous lookup mutation.
            self.now += 10
            return Phones.phones(self.phones)
        with patch.object(self.phones, 'phones', side_effect=lookup):
            status = self.assert_no_schedule()
        self.assertEqual(status['recovery']['schedule']['observedMonotonicSeconds'], self.now)
        self.assertEqual(len(self.children), 1)

    def test_schedule_existing_cancellation_newstart_and_preflight_failure(self):
        # Connect is last: stopping its live substitute child is signal death,
        # which correctly quarantines this manager for the rest of its lifetime.
        for operation in ('disconnect', 'forget', 'selection', 'shutdown', 'preflight', 'connect'):
            with self.subTest(operation=operation):
                self.manager.start(A)
                self.exit_child()
                before = self.manager.request({'action': 'status'})
                self.assertTrue(before['recovery']['schedule']['queued'])
                if operation == 'shutdown':
                    self.manager.shutdown()
                elif operation == 'selection':
                    self.manager.state.select(B)
                    self.manager.tick()
                elif operation == 'preflight':
                    self.now += 2
                    with patch('phone_manager.subprocess.run', side_effect=ValueError('changed inputs')):
                        self.manager.tick()
                    self.assertIn('Receiver recovery failed', self.manager.error)
                elif operation == 'connect':
                    self.manager.request({'action': 'connect', 'phone': B})
                    self.assertEqual(self.manager.active, B)
                    self.assertNotEqual(self.manager.generation, before['generation'])
                else:
                    self.manager.request({'action': operation, 'phone': A,
                                          'generation': before['generation']})
                self.assert_no_schedule()
                count = len(self.children)
                self.now += 100
                self.manager.tick()
                self.assertEqual(len(self.children), count)
                self.manager.stop()

    def test_schedule_status_actual_private_endpoint_keeps_queue_intent(self):
        from phone_manager_endpoint import ManagerEndpoint
        import select
        self.manager.start(A)
        self.exit_child()
        deadline = self.manager.recovery_at
        script = ('import json,socket,sys\n'
                  'with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:\n'
                  ' s.settimeout(3)\n'
                  ' s.connect(sys.argv[1])\n'
                  ' s.sendall(b\'{"action":"status"}\\n\')\n'
                  ' print("connected",flush=True)\n'
                  ' data=b""\n'
                  ' while not data.endswith(b"\\n"):\n'
                  '  data += s.recv(65536)\n'
                  ' print(data.decode(),end="")\n')
        with ManagerEndpoint(self.root, self.manager.owner) as endpoint:
            client = self.actual_popen([sys.executable, '-B', '-c', script, str(endpoint.path)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                self.assertTrue(select.select([client.stdout], [], [], 3)[0])
                self.assertEqual(client.stdout.readline(), b'connected\n')
                self.assertTrue(endpoint.serve_once(self.manager.request))
                stdout, stderr = client.communicate(timeout=5)
                self.assertEqual(client.returncode, 0, stderr.decode())
                status = json.loads(stdout)
            finally:
                if client.poll() is None:
                    client.kill()
                    client.wait(timeout=5)
                for stream in (client.stdout, client.stderr):
                    stream.close()
        self.assertTrue(status['ok'])
        self.assertEqual(status['recovery']['phone'], A)
        self.assertEqual(status['recovery']['remainingAttempts'], 3)
        self.assertEqual(status['recovery']['schedule']['deadlineMonotonicSeconds'], deadline)
        self.assertEqual(self.manager.recovery_at, deadline)
        self.assertEqual(len(self.children), 1)
        self.assertEqual([record['decision'] for record in status['recovery']['decisions']], ['queued'])
        self.assertEqual(status['recovery']['decisions'][0]['observedMonotonicSeconds'], self.now)
        self.assertEqual(self.decisions(), status['recovery']['decisions'])

    def decisions(self):
        return self.manager.request({'action': 'status'})['recovery']['decisions']

    def test_decisions_empty_default_and_independent_manager_restart(self):
        self.assertEqual(self.decisions(), [])
        self.manager.start(A)
        self.assertEqual(self.decisions(), [])  # Explicit launch is outside recovery.
        self.exit_child()
        self.assertEqual(self.decisions()[-1]['decision'], 'queued')
        other_root = self.root / 'independent'
        other_root.mkdir(mode=0o700)
        other = Manager(self.config, other_root, self.display, self.manager.owner,
                        bluez=Phones())
        try:
            self.assertEqual(other.request({'action': 'status'})['recovery']['decisions'], [])
            self.manager.cancel_recovery()
            self.assertEqual(other.request({'action': 'status'})['recovery']['decisions'], [])
        finally:
            other.shutdown()
        restarted = Manager(self.config, self.root, self.display, self.manager.owner,
                            bluez=Phones(), recovery_attempts=3)
        try:
            self.assertEqual(restarted.request({'action': 'status'})['recovery']['decisions'], [])
        finally:
            restarted.shutdown()

    def test_decisions_exact_original_backoffs_dispatch_and_ring_eviction(self):
        self.now = 100.123456789876
        self.manager.start(A)
        expected = []
        for delay, remaining in ((2, 3), (4, 2), (8, 1)):
            self.exit_child()
            queued = self.decisions()[-1]
            self.assertEqual(queued, {'decision': 'queued', 'generation': self.manager.generation,
                                     'configuredMaximum': 3, 'remainingAttempts': remaining,
                                     'exitCode': 7, 'clockDomain': 'python-time.monotonic',
                                     'observedMonotonicSeconds': self.now})
            expected.append(queued)
            self.assertEqual(self.manager.recovery_at, self.now + delay)
            self.now += delay - .001
            self.manager.tick()
            self.assertEqual(self.decisions(), expected[-8:])
            self.now += .001
            generation = self.manager.generation
            self.manager.tick()
            dispatch, started = self.decisions()[-2:]
            self.assertEqual(dispatch['decision'], 'dispatch')
            self.assertEqual(dispatch['generation'], generation)
            self.assertEqual(dispatch['remainingAttempts'], remaining)
            self.assertEqual(started['decision'], 'child_started')
            self.assertEqual(started['generation'], generation + 1)
            self.assertEqual(started['remainingAttempts'], remaining - 1)
            for record in (dispatch, started):
                self.assertIsNone(record['exitCode'])
                self.assertEqual(record['observedMonotonicSeconds'], self.now)
            expected.extend((dispatch, started))
            self.assertEqual(self.decisions(), expected[-8:])
            self.assertEqual(len(self.children), 4 - remaining + 1)
            self.assertFalse(self.manager.carplay_snapshot()['returnReady'])
        self.exit_child()
        exhausted = self.decisions()[-1]
        self.assertEqual(exhausted['decision'], 'budget_exhausted')
        self.assertEqual(exhausted['remainingAttempts'], 0)
        self.assertEqual(exhausted['exitCode'], 7)
        expected.append(exhausted)
        self.assertEqual(len(expected), 10)
        self.assertEqual(self.decisions(), expected[-8:])
        self.assertEqual(self.manager.recovery_decisions.maxlen, 8)
        self.assertIsNone(self.manager.recovery_at)
        self.now += 1000
        self.manager.tick()
        self.assertEqual(self.decisions(), expected[-8:])
        self.assertEqual(len(self.children), 4)

    def test_decisions_copies_overdue_reads_and_ticks_do_not_duplicate_or_dispatch(self):
        self.manager.start(A)
        self.exit_child()
        original = self.decisions()
        first = self.decisions()
        first[0]['decision'] = 'private arbitrary replacement'
        first.append({'phone': A})
        self.assertEqual(self.decisions(), original)
        self.manager.tick()
        self.manager.tick()
        self.assertEqual(self.decisions(), original)
        self.now = self.manager.recovery_at + 100
        with patch.object(self.manager, 'start', side_effect=AssertionError('status launched')), \
             patch.object(self.manager, 'cancel_recovery', side_effect=AssertionError('status cancelled')), \
             patch.object(self.manager, 'tick', side_effect=AssertionError('status ticked')):
            for _ in range(3):
                self.assertEqual(self.decisions(), original)
        self.assertEqual(self.manager.recovery_remaining, 3)
        self.assertEqual(len(self.children), 1)
        self.manager.tick()
        self.assertEqual([r['decision'] for r in self.decisions()],
                         ['queued', 'dispatch', 'child_started'])
        self.manager.tick()
        self.assertEqual(len(self.decisions()), 3)

    def test_decisions_actual_clean_disabled_and_unselected_exit_are_not_eligible(self):
        for attempts, code, selected in ((3, 0, A), (0, 7, A), (3, 7, B)):
            with self.subTest(attempts=attempts, code=code, selected=selected):
                self.manager.recovery_attempts = attempts
                self.manager.start(A)
                self.manager.state.select(selected)
                self.exit_child(code)
                record = self.decisions()[-1]
                self.assertEqual(record['decision'], 'not_eligible')
                self.assertEqual(record['remainingAttempts'], attempts)
                self.assertEqual(record['configuredMaximum'], attempts)
                self.assertEqual(record['exitCode'], code)
                self.assertIsNone(self.manager.recovery_at)
                history = self.decisions()
                self.now += 100
                self.manager.tick()
                self.assertEqual(self.decisions(), history)

    def test_decisions_cancellation_reasons_only_on_actual_pending_queue(self):
        class Pairing:
            phase = 'idle'
            def snapshot(self):
                return {'phase': self.phase, 'supported': True}
            def start(self):
                self.phase = 'starting'
            def close(self):
                self.phase = 'idle'
            def wait_closed(self):
                pass
        for operation, decision in (('selection', 'cancelled_selection'),
                                    ('pairing', 'cancelled_pairing'),
                                    ('pairing_guard', 'cancelled_pairing'),
                                    ('forget', 'cancelled_removed'),
                                    ('disconnect', 'cancelled'), ('shutdown', 'cancelled')):
            with self.subTest(operation=operation):
                self.manager.start(A)
                self.exit_child()
                generation = self.manager.generation
                if operation == 'selection':
                    self.manager.state.select(B)
                    self.manager.tick()
                elif operation in ('pairing', 'pairing_guard'):
                    self.manager.pairing = Pairing()
                    if operation == 'pairing':
                        self.manager.request({'action': 'pairing_start'})
                    else:
                        self.manager.pairing.phase = 'confirmation'
                        self.manager.tick()
                    self.manager.pairing = None
                elif operation == 'shutdown':
                    self.manager.shutdown()
                else:
                    self.manager.request({'action': operation, 'phone': A, 'generation': generation})
                record = self.decisions()[-1]
                self.assertEqual(record['decision'], decision)
                self.assertEqual(record['generation'], generation)
                self.assertEqual(record['remainingAttempts'], 3)
                self.assertIsNone(record['exitCode'])
                history = self.decisions()
                self.manager.cancel_recovery()
                self.manager.tick()
                self.assertEqual(self.decisions(), history)
                self.assertIsNone(self.manager.recovery_at)

    def test_decisions_due_attempt_failure_is_coarse_private_and_never_started(self):
        for failure in ('phone_manager.subprocess.run', 'phone_manager.subprocess.Popen'):
            with self.subTest(failure=failure):
                self.manager.start(A)
                self.exit_child()
                self.now += 2
                count = len(self.children)
                with patch(failure, side_effect=ValueError('private phone name/address/key details')):
                    self.manager.tick()
                dispatch, failed = self.decisions()[-2:]
                self.assertEqual(dispatch['decision'], 'dispatch')
                self.assertEqual(failed['decision'], 'attempt_failed')
                self.assertEqual(failed['remainingAttempts'], 3)
                self.assertEqual(failed['generation'], dispatch['generation'] +
                                 (1 if failure.endswith('Popen') else 0))
                self.assertIsNone(failed['exitCode'])
                self.assertEqual(len(self.children), count)
                self.assertIsNone(self.manager.recovery_at)
                self.assertNotIn('private', json.dumps(self.decisions()))
                history = self.decisions()
                self.now += 100
                self.manager.tick()
                self.assertEqual(self.decisions(), history)

    def test_decisions_signal_retirement_once_and_no_competing_successor(self):
        self.manager.start(A)
        child = self.manager.child
        child.kill()
        child.wait(timeout=5)
        # Read-only polling observes quarantine but is not a recovery decision.
        self.assertEqual(self.decisions(), [])
        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
            self.manager.tick()
        record = self.decisions()[-1]
        self.assertEqual(record['decision'], 'retirement_required')
        self.assertEqual(record['exitCode'], child.returncode)
        self.assertLess(record['exitCode'], 0)
        for _ in range(2):
            with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
                self.manager.tick()
            with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
                self.manager.start(A)
        self.assertEqual(self.decisions(), [record])
        self.assertEqual(len(self.children), 1)

    def test_decisions_live_actual_child_cancels_queue_without_competitor(self):
        self.manager.start(A)
        self.exit_child()
        # Explicit existing-owner fixture supplies an actual held child; no
        # PID/name is used to infer a role or admit another radio owner.
        child = self.spawn([], pass_fds=())
        self.manager.child = child
        self.now += 100
        self.manager.tick()
        self.assertEqual(self.decisions()[-1]['decision'], 'cancelled_owner')
        self.assertIs(self.manager.child, child)
        self.assertIsNone(child.poll())
        self.assertEqual(len(self.children), 2)
        history = self.decisions()
        with patch.object(child, 'terminate'), \
             patch.object(child, 'wait', side_effect=subprocess.TimeoutExpired('fixture', 15)):
            with self.assertRaisesRegex(RuntimeError, 'still stopping'):
                self.manager.request({'action': 'disconnect'})
        self.assertIs(self.manager.child, child)
        self.assertEqual(self.decisions(), history)
        self.assertEqual(len(self.children), 2)

    def test_decisions_unknown_event_date_is_null_and_status_never_redates_history(self):
        self.manager.start(A)
        self.exit_child()
        def unavailable_observation():
            self.assertTrue(self.manager.lock._is_owned())
            return float('nan')
        with patch('phone_manager.time.monotonic', side_effect=unavailable_observation):
            self.manager.cancel_recovery()
        record = self.decisions()[-1]
        self.assertEqual(record['decision'], 'cancelled')
        self.assertIsNone(record['clockDomain'])
        self.assertIsNone(record['observedMonotonicSeconds'])
        self.now += 500
        self.assertEqual(self.decisions()[-1], record)
        self.assertEqual(set(record), {'decision', 'generation', 'configuredMaximum',
                                      'remainingAttempts', 'exitCode', 'clockDomain',
                                      'observedMonotonicSeconds'})

    def test_decisions_existing_native_cancellation_parser_behavior_is_preserved(self):
        from phone_page_presentation import recovery_phone
        self.manager.start(A)
        self.exit_child()
        recovery = self.manager.request({'action': 'status'})['recovery']
        old = dict(recovery)
        old.pop('decisions')
        self.assertEqual(recovery_phone(recovery), recovery_phone(old))
        self.assertEqual(recovery_phone(old), A)  # declared schedule keeps the core cancel target
        self.assertEqual(recovery_phone({key: recovery[key] for key in ('phone', 'remainingAttempts')}), A)


    def native_endpoint_request(self, endpoint, request):
        import select
        script = ('import json,socket,sys\n'
                  'with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:\n'
                  ' s.settimeout(3)\n'
                  ' s.connect(sys.argv[1])\n'
                  ' s.sendall(sys.argv[2].encode()+b"\\n")\n'
                  ' print("connected",flush=True)\n'
                  ' data=b""\n'
                  ' while not data.endswith(b"\\n"):\n'
                  '  data += s.recv(65536)\n'
                  ' print(data.decode(),end="")\n')
        client = self.actual_popen([sys.executable, '-B', '-c', script, str(endpoint.path),
                                   json.dumps(request)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertTrue(select.select([client.stdout], [], [], 3)[0])
            self.assertEqual(client.stdout.readline(), b'connected\n')
            self.assertTrue(endpoint.serve_once(self.manager.request))
            stdout, stderr = client.communicate(timeout=5)
            self.assertEqual(client.returncode, 0, stderr.decode())
            return json.loads(stdout)
        finally:
            if client.poll() is None:
                client.kill()
                client.wait(timeout=5)
            for stream in (client.stdout, client.stderr):
                stream.close()

    def test_native_cancel_target_from_actual_manager_endpoint_preserves_authority(self):
        from phone_manager_endpoint import ManagerEndpoint
        from phone_page_presentation import recovery_phone
        self.manager.start(A)
        self.exit_child()
        with ManagerEndpoint(self.root, self.manager.owner) as endpoint:
            status = self.native_endpoint_request(endpoint, {'action': 'status'})
            self.assertTrue(status['ok'])
            self.assertEqual(set(status['recovery']), {'phone', 'remainingAttempts', 'schedule', 'decisions'})
            self.assertEqual(recovery_phone(status['recovery']), A)
            self.assertIsNone(status['active'])
            self.assertFalse(status['carplay']['returnReady'])
            deadline = self.manager.recovery_at
            for phone, generation in ((B, status['generation']), (A, status['generation'] + 1)):
                rejected = self.native_endpoint_request(endpoint, {'action': 'disconnect', 'phone': phone,
                                                                   'generation': generation})
                self.assertFalse(rejected['ok'])
                self.assertEqual(self.manager.recovery_at, deadline)
                self.assertEqual(len(self.children), 1)
            cancelled = self.native_endpoint_request(endpoint, {'action': 'disconnect',
                                                                'phone': recovery_phone(status['recovery']),
                                                                'generation': status['generation']})
            self.assertTrue(cancelled['ok'])
            history = self.native_endpoint_request(endpoint, {'action': 'status'})
            self.assertEqual([record['decision'] for record in history['recovery']['decisions']],
                             ['queued', 'cancelled'])
            self.assertIsNone(recovery_phone(history['recovery']))
            self.assertFalse(history['carplay']['returnReady'])
        self.now += 100
        self.manager.tick()
        self.assertEqual(len(self.children), 1)
        self.assertIsNone(self.manager.recovery_at)
        self.assertIsNone(self.manager.child)

    def test_native_endpoint_stale_cancel_cannot_stop_actual_successor(self):
        from phone_manager_endpoint import ManagerEndpoint
        from phone_page_presentation import recovery_phone
        self.manager.start(A)
        self.exit_child()
        with ManagerEndpoint(self.root, self.manager.owner) as endpoint:
            status = self.native_endpoint_request(endpoint, {'action': 'status'})
            target = recovery_phone(status['recovery'])
            self.assertEqual(target, A)
            self.now += 2
            self.manager.tick()
            successor = self.manager.child
            rejected = self.native_endpoint_request(endpoint, {'action': 'disconnect', 'phone': target,
                                                               'generation': status['generation']})
            self.assertFalse(rejected['ok'])
            self.assertIn('Session changed', rejected['error'])
            self.assertIs(self.manager.child, successor)
            self.assertIsNone(successor.poll())
            current = self.native_endpoint_request(endpoint, {'action': 'status'})
            self.assertEqual(current['active'], A)
            self.assertIsNone(recovery_phone(current['recovery']))
            self.assertFalse(current['carplay']['returnReady'])
            self.assertEqual(len(self.children), 2)


if __name__ == '__main__':
    unittest.main()
