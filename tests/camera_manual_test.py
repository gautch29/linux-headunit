#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual private files/processes/IPC; injected Python clocks are not C++ claims."""
import copy
import gc
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import camera_manual as m


class Clock:
    def __init__(self, now=1000000000):
        self.now = now

    def __call__(self):
        return self.now


class ManualTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='headunit-manual-')
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name).resolve()
        self.profile = self.folder / 'profile.json'
        self.config = {'schemaVersion': 1, 'enabled': True,
                       'requestFile': str(self.folder / 'request'), 'ownerFile': str(self.folder / 'owner'),
                       'statusFile': str(self.folder / 'status'), 'controlSocket': str(self.folder / 'control'),
                       'timing': {'leaseMs': 250, 'pollMs': 50, 'statusMaximumAgeMs': 500}}
        self.save()

    def save(self):
        self.profile.write_text(json.dumps(self.config))
        self.profile.chmod(0o600)

    def camera(self, clock=None):
        # This tests Python's own filesystem/IPC domain, not Darwin libc++.
        camera = m.ManualCamera(self.profile, clock_ns=clock or time.monotonic_ns)
        self.addCleanup(camera.close)
        return camera

    def endpoint(self, name, raw, mode=0o600):
        path = self.folder / name
        if path.exists():
            path.chmod(0o600)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        try:
            os.write(fd, raw)
            os.fchmod(fd, mode)
        finally:
            os.close(fd)
        return path

    def wait(self, predicate, message='Expected asynchronous update was not observed'):
        until = time.monotonic() + 0.7
        while time.monotonic() < until:
            if predicate():
                return
            time.sleep(0.003)
        self.fail(message)

    def request(self):
        return (self.folder / 'request').read_bytes()

    def deadline(self):
        match = re.fullmatch(rb'on ([1-9][0-9]{0,19})\n', self.request())
        self.assertIsNotNone(match)
        return int(match[1])

    def command(self, raw):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(0.6)
            client.connect(self.config['controlSocket'])
            client.sendall(raw)
            reply = bytearray()
            while not reply.endswith(b'\n') and len(reply) <= 256:
                chunk = client.recv(256)
                if not chunk:
                    break
                reply.extend(chunk)
            self.assertLessEqual(len(reply), 256)
            return json.loads(reply)

    def test_disabled_and_validate_only_create_no_endpoints(self):
        self.config['enabled'] = False
        for key in ('requestFile', 'ownerFile', 'statusFile', 'controlSocket'):
            self.config[key] = str(self.folder / 'missing' / key)
        self.save()
        self.assertEqual(m.load_configuration(self.profile), self.config)
        with m.ManualCamera(self.profile) as camera:
            self.assertEqual(camera.snapshot(), {'configured': False, 'requested': False, 'phase': 'unavailable', 'error': ''})
            self.assertIsNone(camera._state.worker)
            with self.assertRaises(ValueError):
                camera.request(True)
        self.assertFalse((self.folder / 'missing').exists())

    def test_strict_configuration(self):
        changes = [({'schemaVersion': True}), {'schemaVersion': 2}, {'enabled': 1}, {'extra': 1},
                   {'requestFile': 'relative'}, {'requestFile': str(self.folder / 'request') + '/'},
                   {'requestFile': str(self.folder) + '/../request'}, {'requestFile': '/tmp/a\0b'},
                   {'ownerFile': self.config['requestFile']}, {'controlSocket': self.config['statusFile'] + '.owner'},
                   {'controlSocket': '/tmp/' + 'x' * 100}, {'controlSocket': '/tmp/caméra'}, {'controlSocket': '/tmp//camera'},
                   {'controlSocket': '//tmp/camera'}, {'controlSocket': '/tmp/camera\n'}]
        for change in changes:
            value = copy.deepcopy(self.config); value.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                m.validate_configuration(value)
        for timing in ({'leaseMs': 49}, {'leaseMs': 1001}, {'pollMs': True}, {'pollMs': 101},
                       {'leaseMs': 100, 'pollMs': 50}, {'statusMaximumAgeMs': 49}, {'extra': 1}):
            value = copy.deepcopy(self.config); value['timing'].update(timing)
            with self.subTest(timing=timing), self.assertRaises(ValueError):
                m.validate_configuration(value)
        for raw in ('{"schemaVersion":1,"schemaVersion":1}', '{"schemaVersion":NaN}'):
            self.profile.write_text(raw)
            with self.assertRaises(ValueError):
                m.load_configuration(self.profile)
        self.profile.unlink(); os.mkfifo(self.profile, 0o600)
        with self.assertRaises(ValueError):
            m.load_configuration(self.profile)

    def test_configuration_permissions_and_special_modes(self):
        for mode in (0o666, 0o620, 0o602, 0o4600, 0o1600):
            self.profile.chmod(mode)
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                m.load_configuration(self.profile)
        self.profile.chmod(0o600)
        self.assertEqual(m.load_configuration(self.profile), self.config)
        # A root manager still validates its explicitly configured desktop
        # owner's profile, rather than treating all non-root owners as unsafe.
        with mock.patch.object(m.os, 'getuid', return_value=0):
            self.assertEqual(m.load_configuration(self.profile, (self.profile.stat().st_uid, os.getgid())), self.config)
            if self.profile.stat().st_uid != 0:
                with self.assertRaises(ValueError):
                    m.load_configuration(self.profile)

    def test_linux_clock_guard_and_owner_validation(self):
        with mock.patch.object(m.sys, 'platform', 'darwin'), self.assertRaisesRegex(ValueError, 'Linux CLOCK_MONOTONIC'):
            m.ManualCamera(self.profile)
        self.assertFalse((self.folder / 'owner').exists())
        for owner in ((True, 1), (-1, 1), (1,), (), 'owner'):
            with self.subTest(owner=owner), self.assertRaises(ValueError):
                m.ManualCamera(self.profile, owner, clock_ns=time.monotonic_ns)

    def test_constructor_cancel_renewal_and_disposal(self):
        clock = Clock(); camera = m.ManualCamera(self.profile, clock_ns=clock)
        self.assertEqual(self.request(), b'off\n')
        held = os.open(self.config['requestFile'], os.O_RDONLY)
        try:
            state = camera.request(True)
            self.assertTrue(state['requested']); self.assertEqual(state['phase'], 'unavailable')
            self.wait(lambda: self.request().startswith(b'on '))
            self.assertEqual(self.deadline(), clock.now + 250000000)
            self.assertEqual(os.read(held, 24), b'off\n')
            self.assertEqual(os.fstat(held).st_nlink, 0)
            clock.now += 100000000
            self.wait(lambda: self.deadline() == clock.now + 250000000)
            camera.request(False); self.wait(lambda: self.request() == b'off\n')
            camera.request(True); self.wait(lambda: self.request().startswith(b'on '))
            # The public handle is not retained by the daemon worker.
            del camera; gc.collect()
            self.wait(lambda: self.request() == b'off\n')
            self.assertFalse((self.folder / 'control').exists())
        finally:
            os.close(held)

    def test_owner_lock_across_process_and_no_truncation(self):
        camera = self.camera(); camera.request(True); self.wait(lambda: self.request().startswith(b'on '))
        script = 'import sys,time;sys.path.insert(0,sys.argv[1]);from camera_manual import ManualCamera;ManualCamera(sys.argv[2],clock_ns=time.monotonic_ns)'
        other = subprocess.run([sys.executable, '-c', script, str(Path(m.__file__).parent), str(self.profile)],
                               capture_output=True, timeout=2)
        self.assertNotEqual(other.returncode, 0)
        self.assertTrue(self.request().startswith(b'on '))
        for name in ('owner', 'control.lock', 'request'):
            info = (self.folder / name).lstat()
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
            self.assertEqual(info.st_nlink, 1)
            self.assertEqual(info.st_uid, os.getuid())
        camera.close()
        self.assertEqual(self.request(), b'off\n')
        resumed = self.camera(); self.assertTrue(resumed.snapshot()['configured'])

    def test_unsafe_requests_are_preserved_without_dangerous_open(self):
        path = self.folder / 'request'
        for raw, mode in ((b'on 01\n', 0o600), (b'off\n', 0o644), (b'x' * 25, 0o600)):
            self.endpoint('request', raw, mode)
            with self.subTest(raw=raw, mode=mode), self.assertRaises(ValueError):
                m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
            self.assertEqual(path.read_bytes(), raw)
        path.unlink(); sentinel = self.endpoint('sentinel', b'sentinel')
        path.symlink_to(sentinel)
        with self.assertRaises(ValueError):
            m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
        self.assertEqual(sentinel.read_bytes(), b'sentinel'); path.unlink()
        os.mkfifo(path, 0o600)
        began = time.monotonic()
        with self.assertRaises(ValueError):
            m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
        self.assertLess(time.monotonic() - began, 0.3); path.unlink()
        self.endpoint('request', b'off\n'); os.link(path, self.folder / 'request-alias')
        with self.assertRaises(ValueError):
            m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)

    def test_stale_socket_and_owner_shapes_preserved(self):
        path = self.folder / 'control'
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(path)); path.chmod(0o600)
            identity = path.lstat()
            with self.assertRaisesRegex(ValueError, 'already exists'):
                m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
            self.assertEqual(path.lstat().st_ino, identity.st_ino)
        path.unlink()
        for name in ('owner', 'control.lock'):
            self.endpoint(name, b'not empty')
            with self.assertRaises(ValueError):
                m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
            self.assertEqual((self.folder / name).read_bytes(), b'not empty')
            (self.folder / name).unlink()

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux kernel socket retirement')
    def test_crashed_camera_owner_restarts_with_camera_off(self):
        script = ('import sys,time;sys.path.insert(0,sys.argv[1]);'
                  'from camera_manual import ManualCamera;'
                  'c=ManualCamera(sys.argv[2]);c.request(True);'
                  'print("ready",flush=True);time.sleep(30)')
        child = subprocess.Popen([sys.executable, '-c', script, str(Path(m.__file__).parent), str(self.profile)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), 'ready')
            self.wait(lambda: self.request().startswith(b'on '))
            child.kill()
            child.wait(timeout=2)
            self.assertTrue((self.folder / 'control').exists())
            camera = self.camera()
            self.assertEqual(self.request(), b'off\n')
            self.assertFalse(camera.snapshot()['requested'])
            self.assertTrue(self.command(b'{"action":"camera","enabled":false}\n')['ok'])
        finally:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=2)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux kernel socket retirement')
    def test_bound_pinned_socket_without_listener_is_preserved(self):
        path = self.folder / 'control'
        directory = os.open(self.folder, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(f'/proc/self/fd/{directory}/control')
                path.chmod(0o600)
                identity = path.lstat()
                # A bound owner need not have reached listen(). ECONNREFUSED
                # alone cannot establish abandonment; the kernel inventory
                # must protect the pinned binding too.
                with self.assertRaisesRegex(ValueError, 'bound owner'):
                    m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
                self.assertEqual(path.lstat().st_ino, identity.st_ino)
        finally:
            os.close(directory)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux kernel socket retirement')
    def test_unsafe_abandoned_socket_is_preserved(self):
        path = self.folder / 'control'
        for mode, hard_link in ((0o644, False), (0o600, True)):
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(str(path))
            path.chmod(mode)
            alias = self.folder / 'socket-alias'
            if hard_link:
                os.link(path, alias)
            identity = path.lstat()
            with self.subTest(mode=mode, hard_link=hard_link), self.assertRaisesRegex(ValueError, 'unsafe'):
                m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
            self.assertEqual(path.lstat().st_ino, identity.st_ino)
            self.assertEqual(stat.S_IMODE(path.lstat().st_mode), mode)
            if hard_link:
                alias.unlink()
            path.unlink()

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux kernel socket retirement')
    def test_unavailable_kernel_inventory_preserves_abandoned_socket(self):
        path = self.folder / 'control'
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(path))
        path.chmod(0o600)
        identity = path.lstat()
        # Only the inventory uses Python open(); profile and endpoint readers
        # use bounded pinned OS descriptors. No production hooks are added.
        with mock.patch('builtins.open', side_effect=PermissionError('fixture inventory denied')):
            with self.assertRaises(PermissionError):
                m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
        self.assertEqual(path.lstat().st_ino, identity.st_ino)
        resumed = self.camera()
        self.assertFalse(resumed.snapshot()['requested'])
        self.assertEqual(self.request(), b'off\n')

    def test_control_socket_redirected_ancestor_is_rejected(self):
        alias = self.folder / 'alias'; alias.symlink_to(self.folder, target_is_directory=True)
        self.config['controlSocket'] = str(alias / 'control')
        self.save()
        with self.assertRaisesRegex(ValueError, 'canonical parent'):
            m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
        self.assertFalse((self.folder / 'owner').exists())

    def test_socket_commands_are_typed_bounded_and_independent(self):
        camera = self.camera()
        self.assertTrue(self.command(b'{"action":"camera","enabled":true}\n')['ok'])
        self.wait(lambda: self.request().startswith(b'on '))
        for raw in (b'{"action":"camera","enabled":1}\n', b'{"action":"camera","enabled":true,"phone":"x"}\n',
                    b'{"action":"camera","enabled":false,"enabled":true}\n', b'{"action":"phone","enabled":true}\n',
                    b'{}\n', b'x' * 257, b'{}\n{}\n', b'\xff\n'):
            with self.subTest(raw=raw):
                self.assertFalse(self.command(raw)['ok'])
                self.assertTrue(camera.snapshot()['requested'])
        self.assertTrue(self.command(b'{"action":"camera","enabled":false}\n')['ok'])
        self.wait(lambda: self.request() == b'off\n')

    def test_partial_client_cannot_stop_renewal(self):
        camera = self.camera(); camera.request(True); self.wait(lambda: self.request().startswith(b'on '))
        before = self.deadline()
        with socket.socket(socket.AF_UNIX) as partial:
            partial.settimeout(0.6); partial.connect(self.config['controlSocket']); partial.sendall(b'{"action":')
            self.wait(lambda: self.deadline() > before + 100000000)
            reply = json.loads(partial.recv(256))
            self.assertFalse(reply['ok']); self.assertIn('deadline', reply['error'])
        self.assertTrue(self.command(b'{"action":"camera","enabled":false}\n')['ok'])
        self.wait(lambda: self.request() == b'off\n')

    def test_status_is_original_native_state_not_manual_intent(self):
        clock = Clock(); camera = self.camera(clock)
        self.endpoint('status', b'HCS1 100 1 1000000000 active\n')
        state = camera.snapshot(); self.assertFalse(state['requested']); self.assertEqual(state['phase'], 'active')
        camera.request(True)
        for seq, phase in enumerate(('waiting', 'fault', 'stopped', 'idle'), 2):
            self.endpoint('status', f'HCS1 100 {seq} {clock.now} {phase}\n'.encode())
            self.assertEqual(camera.snapshot()['phase'], phase)
            self.assertTrue(camera.snapshot()['requested'])
        clock.now += 500000000
        self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        self.endpoint('status', f'HCS1 100 6 {clock.now} active\n'.encode())
        self.assertEqual(camera.snapshot()['phase'], 'active')

    def test_status_loss_replay_clock_reversal_and_malformed_bounds(self):
        clock = Clock(); camera = self.camera(clock)
        self.endpoint('status', b'HCS1 100 10 1000000000 active\n')
        self.assertEqual(camera.snapshot()['phase'], 'active')
        (self.folder / 'status').unlink(); self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        self.endpoint('status', b'HCS1 100 10 1000000000 active\n')
        self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        for bad in (b'HCS1 99 99 1000000000 active\n', b'HCS1 100 9 1000000000 active\n',
                    b'HCS1 100 11 1000000001 active\n', b'HCS1 100 11 1000000000 Active\n',
                    b'HCS1 100 11 1000000000 active\n\n', b'HCS1 100 18446744073709551616 1000000000 active\n',
                    b'HCS1 0100 11 1000000000 active\n', b'x' * 129, b'HCS1 100 11 1000000000 active\x00\n'):
            self.endpoint('status', bad)
            self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        self.endpoint('status', b'HCS1 101 1 1000000000 active\n'); self.assertEqual(camera.snapshot()['phase'], 'active')
        clock.now -= 1; self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        clock.now += 2; self.endpoint('status', f'HCS1 102 1 {clock.now} active\n'.encode())
        self.assertEqual(camera.snapshot()['phase'], 'unavailable')

    def test_status_unsafe_mode_and_link_loss_latches(self):
        clock = Clock(); camera = self.camera(clock)
        path = self.endpoint('status', b'HCS1 100 1 1000000000 active\n')
        self.assertEqual(camera.snapshot()['phase'], 'active')
        path.chmod(0o644); self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        path.chmod(0o600); self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        self.endpoint('status', b'HCS1 100 2 1000000000 active\n'); self.assertEqual(camera.snapshot()['phase'], 'active')
        alias = self.folder / 'status-link'; os.link(path, alias)
        self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        alias.unlink(); self.assertEqual(camera.snapshot()['phase'], 'unavailable')

    def test_snapshot_retries_only_safe_atomic_replacement_with_four_attempt_bound(self):
        clock = Clock(); camera = self.camera(clock)
        self.endpoint('status', b'HCS1 100 1 1000000000 active\n')
        self.assertEqual(camera.snapshot()['phase'], 'active')
        original_read = m._read
        calls = []

        def replacement(fd, bound):
            raw = original_read(fd, bound)
            if bound == 128:
                calls.append(raw)
                if len(calls) == 1:
                    new = self.endpoint('next-status', b'HCS1 100 2 1000000000 active\n')
                    os.replace(new, self.folder / 'status')
            return raw

        # Test-only syscall boundary injection; all files/rename/read operations
        # remain actual OS calls. No hooks are added to the production module.
        with mock.patch.object(m, '_read', side_effect=replacement):
            self.assertEqual(camera.snapshot()['phase'], 'active')
        self.assertEqual(len(calls), 2)
        calls.clear()

        def continuous(fd, bound):
            raw = original_read(fd, bound)
            if bound == 128:
                calls.append(raw)
                new = self.endpoint('next-status', f'HCS1 100 {len(calls)+2} 1000000000 active\n'.encode())
                os.replace(new, self.folder / 'status')
            return raw

        with mock.patch.object(m, '_read', side_effect=continuous):
            self.assertEqual(camera.snapshot()['phase'], 'unavailable')
        self.assertEqual(len(calls), 4)
        self.assertEqual(camera.snapshot()['phase'], 'active')

    def test_observed_malformed_or_inplace_mutation_is_not_hidden_by_retry(self):
        clock = Clock(); camera = self.camera(clock)
        original_read = m._read
        for source, in_place, mode in ((b'malformed\n', False, 0o600),
                                       (b'HCS1 100 1 1000000000 active\n', True, 0o600),
                                       (b'HCS1 100 1 1000000000 active\n', False, 0o644)):
            self.endpoint('status', source)
            calls = []

            def mutate(fd, bound):
                raw = original_read(fd, bound)
                if bound == 128:
                    calls.append(raw)
                    if in_place:
                        self.endpoint('status', b'HCS1 100 2 1000000000 fault\n')
                    else:
                        new = self.endpoint('next-status', b'HCS1 100 2 1000000000 active\n', mode)
                        os.replace(new, self.folder / 'status')
                return raw

            with self.subTest(source=source, in_place=in_place, mode=mode), mock.patch.object(m, '_read', side_effect=mutate):
                self.assertEqual(camera.snapshot()['phase'], 'unavailable')
            self.assertEqual(len(calls), 1)

    def test_invalid_parent_or_owner_generation_never_clobbers_replacement(self):
        camera = self.camera(); camera.request(True); self.wait(lambda: self.request().startswith(b'on '))
        request = self.request()
        (self.folder / 'owner').unlink(); self.endpoint('owner', b'')
        self.wait(lambda: not camera.snapshot()['configured'])
        self.assertEqual(self.request(), request)
        self.assertTrue(camera.snapshot()['error']); self.assertLessEqual(len(camera.snapshot()['error']), 128)
        camera.close()
        self.assertEqual(self.request(), request)

    def test_sigkill_owner_request_retains_original_deadline_and_expires(self):
        script = ('import sys,time;sys.path.insert(0,sys.argv[1]);from camera_manual import ManualCamera;'
                  'camera=ManualCamera(sys.argv[2],clock_ns=time.monotonic_ns);camera.request(True);'
                  'time.sleep(.1);print("ready",flush=True);time.sleep(30)')
        process = subprocess.Popen([sys.executable, '-c', script, str(Path(m.__file__).parent), str(self.profile)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        self.assertEqual(process.stdout.readline().strip(), 'ready')
        process.kill(); process.wait(timeout=1)
        raw, deadline = self.request(), self.deadline()
        time.sleep(0.3)
        self.assertGreaterEqual(time.monotonic_ns(), deadline)
        self.assertEqual(self.request(), raw)
        # Linux recovery verifies locks, kernel bindings, refusal and inode
        # stability before retiring the abandoned endpoint. Other platforms
        # still require explicit cleanup.
        if not sys.platform.startswith('linux'):
            with self.assertRaisesRegex(ValueError, 'already exists'):
                m.ManualCamera(self.profile, clock_ns=time.monotonic_ns)
            (self.folder / 'control').unlink()
        camera = self.camera(); self.assertEqual(self.request(), b'off\n'); camera.close()
        process.stdout.close(); process.stderr.close()

    def test_status_provenance_preserves_original_receipt_and_single_read(self):
        clock = Clock()
        camera = self.camera(clock)
        self.endpoint('status', b'HCS1 100 1 1000000000 active\n')
        reads = []
        original = m._snapshot

        def counted(path, bound, decode):
            if bound == 128:
                reads.append(path)
            return original(path, bound, decode)

        with mock.patch.object(m, '_snapshot', side_effect=counted):
            legacy, observation = camera.snapshot_with_status_observation()
        self.assertEqual(len(reads), 1)
        self.assertEqual(legacy, {'configured': True, 'requested': False, 'phase': 'active', 'error': ''})
        receipt = {'instanceStartNs': '100', 'sequence': '1', 'observedNs': '1000000000', 'phase': 'active'}
        self.assertEqual(observation['lastObservation'], receipt)
        self.assertEqual(observation['meaning'], 'native_camera_owner_status_only')
        self.assertEqual(observation['clockDomain'], 'injected_fixture')
        self.assertEqual(observation['availability'], 'available')
        self.assertTrue(observation['eligible'])
        observation['lastObservation']['sequence'] = '999'
        observation['availability'] = 'unavailable'
        clock.now += 1
        self.assertEqual(camera.snapshot_with_status_observation()[1]['lastObservation'], receipt)
        self.assertEqual(set(camera.snapshot()), {'configured', 'requested', 'phase', 'error'})
        self.assertEqual(set(camera.request(False)), {'configured', 'requested', 'phase', 'error'})

    def test_status_provenance_failure_and_recovery_never_redates_last_receipt(self):
        clock = Clock()
        camera = self.camera(clock)
        self.endpoint('status', b'HCS1 100 10 1000000000 active\n')
        original = camera.snapshot_with_status_observation()[1]['lastObservation']
        for raw in (b'HCS1 100 9 1000000000 idle\n',
                    b'HCS1 100 11 1000000001 fault\n', b'malformed\n',
                    b'HCS1 99 99 1000000000 waiting\n'):
            with self.subTest(raw=raw):
                self.endpoint('status', raw)
                legacy, observation = camera.snapshot_with_status_observation()
                self.assertEqual(legacy['phase'], 'unavailable')
                self.assertEqual(observation['availability'], 'unavailable')
                self.assertEqual(observation['lastObservation'], original)
        self.endpoint('status', b'HCS1 100 10 1000000000 active\n')
        self.assertEqual(camera.snapshot_with_status_observation()[1]['availability'], 'unavailable')
        self.endpoint('status', b'HCS1 100 11 1000000000 idle\n')
        observation = camera.snapshot_with_status_observation()[1]
        self.assertEqual(observation['availability'], 'available')
        self.assertEqual(observation['lastObservation']['sequence'], '11')
        self.endpoint('status', b'HCS1 101 1 1000000000 waiting\n')
        receipt = camera.snapshot_with_status_observation()[1]['lastObservation']
        self.assertEqual(receipt['instanceStartNs'], '101')
        clock.now += 500000000
        for _ in range(2):
            legacy, observation = camera.snapshot_with_status_observation()
            self.assertEqual(legacy['phase'], 'unavailable')
            self.assertEqual(observation['availability'], 'unavailable')
            self.assertEqual(observation['lastObservation'], receipt)
        self.endpoint('status', f'HCS1 101 2 {clock.now} fault\n'.encode())
        self.assertEqual(camera.snapshot_with_status_observation()[1]['availability'], 'available')
        clock.now -= 1
        legacy, observation = camera.snapshot_with_status_observation()
        self.assertEqual(observation['state'], 'reader_retired')
        self.assertFalse(observation['eligible'])
        retired = observation['lastObservation']
        clock.now += 2
        self.endpoint('status', f'HCS1 102 1 {clock.now} active\n'.encode())
        observation = camera.snapshot_with_status_observation()[1]
        self.assertEqual(observation['lastObservation'], retired)
        self.assertEqual(observation['availability'], 'unavailable')

    def test_status_provenance_unsafe_file_and_owner_retirement(self):
        clock = Clock()
        camera = self.camera(clock)
        path = self.endpoint('status', b'HCS1 100 1 1000000000 active\n')
        receipt = camera.snapshot_with_status_observation()[1]['lastObservation']
        path.chmod(0o644)
        self.assertEqual(camera.snapshot_with_status_observation()[1]['availability'], 'unavailable')
        path.chmod(0o600)
        self.assertEqual(camera.snapshot_with_status_observation()[1]['availability'], 'unavailable')
        self.endpoint('status', b'HCS1 101 1 1000000000 idle\n')
        self.assertEqual(camera.snapshot_with_status_observation()[1]['availability'], 'available')
        camera.request(True)
        self.wait(lambda: self.request().startswith(b'on '))
        (self.folder / 'owner').unlink()
        self.endpoint('owner', b'')
        self.wait(lambda: not camera.snapshot()['configured'])
        legacy, observation = camera.snapshot_with_status_observation()
        self.assertEqual(observation['state'], 'manual_owner_unavailable')
        self.assertFalse(observation['eligible'])
        self.assertEqual(observation['availability'], 'unavailable')
        self.assertEqual(observation['lastObservation']['instanceStartNs'], '101')
        self.assertNotIn('error', observation)
        self.assertNotIn(str(self.folder), json.dumps(observation))
        self.assertEqual(receipt['observedNs'], '1000000000')

    def test_status_provenance_disabled_missing_close_and_uint64_identity(self):
        clock = Clock(m._UINT64)
        camera = self.camera(clock)
        self.assertIsNone(camera.snapshot_with_status_observation()[1]['lastObservation'])
        self.endpoint('status', f'HCS1 {m._UINT64} {m._UINT64} {m._UINT64} stopped\n'.encode())
        receipt = camera.snapshot_with_status_observation()[1]['lastObservation']
        self.assertEqual(receipt['sequence'], str(m._UINT64))
        camera.close()
        with mock.patch.object(m, '_snapshot', side_effect=AssertionError('Closed diagnostics reopened a file')):
            legacy, observation = camera.snapshot_with_status_observation()
        self.assertFalse(legacy['configured'])
        self.assertFalse(observation['eligible'])
        self.assertEqual(observation['lastObservation'], receipt)
        self.assertEqual(observation['availability'], 'unavailable')
        self.config['enabled'] = False
        self.save()
        disabled = m.ManualCamera(self.profile)
        self.addCleanup(disabled.close)
        legacy, observation = disabled.snapshot_with_status_observation()
        self.assertEqual(observation['state'], 'disabled')
        self.assertIsNone(observation['clockDomain'])
        self.assertIsNone(observation['lastObservation'])
        self.assertIsNone(disabled._state.worker)

    def test_paired_status_concurrent_validation_and_close_are_independent_copies(self):
        import threading
        clock = Clock()
        camera = self.camera(clock)
        self.endpoint('status', b'HCS1 100 1 1000000000 active\n')
        entered, release = threading.Event(), threading.Event()
        original = camera._state.status._phase
        failures = []
        result = []

        def pause_after_copy(*args):
            copied = original(*args)
            entered.set()
            if not release.wait(.7):
                raise AssertionError('Test did not release copied status')
            return copied

        def snapshot():
            try:
                result.append(camera.snapshot_with_status_observation())
            except Exception as error:
                failures.append(error)

        with mock.patch.object(camera._state.status, '_phase', side_effect=pause_after_copy):
            thread = threading.Thread(target=snapshot)
            thread.start()
            try:
                self.assertTrue(entered.wait(.7))
                camera.close()
            finally:
                release.set()
                thread.join(.7)
        self.assertFalse(thread.is_alive())
        self.assertFalse(failures)
        legacy, observation = result[0]
        self.assertEqual(legacy['phase'], 'unavailable')
        self.assertEqual(observation['lastObservation']['instanceStartNs'], '100')
        self.assertEqual(observation['availability'], 'unavailable')
        self.assertFalse(observation['eligible'])

    def test_paired_copy_finishes_before_another_reader_can_retire_its_clock(self):
        import threading
        clock = Clock()
        camera = self.camera(clock)
        self.endpoint('status', b'HCS1 100 1 1000000000 active\n')
        first_read, rollback_read = threading.Event(), threading.Event()
        original = camera._state.status._phase
        results, failures = [], []

        def observed(*args):
            phase = original(*args)
            (rollback_read if camera._state.status.retired else first_read).set()
            return phase

        def read(paired):
            try:
                results.append(camera.snapshot_with_status_observation() if paired else camera.snapshot())
            except Exception as error:
                failures.append(error)

        with mock.patch.object(camera._state.status, '_phase', side_effect=observed):
            paired = threading.Thread(target=read, args=(True,))
            rollback = threading.Thread(target=read, args=(False,))
            try:
                with camera._state.lock:
                    paired.start()
                    self.assertTrue(first_read.wait(.7))
                    clock.now -= 1
                    rollback.start()
                    # The paired copy is waiting for the state lock. A later read
                    # must not retire it and then let that old copy appear current.
                    self.assertFalse(rollback_read.wait(.025))
            finally:
                if paired.ident is not None:
                    paired.join(.7)
                if rollback.ident is not None:
                    rollback.join(.7)
        self.assertFalse(paired.is_alive())
        self.assertFalse(rollback.is_alive())
        self.assertFalse(failures)
        self.assertTrue(rollback_read.is_set())
        self.assertEqual(len(results), 2)
        observation = camera.snapshot_with_status_observation()[1]
        self.assertEqual(observation['state'], 'reader_retired')
        self.assertEqual(observation['availability'], 'unavailable')
        self.assertFalse(observation['eligible'])
        self.assertEqual(observation['lastObservation']['observedNs'], '1000000000')

    def test_manual_ipc_canonical_reply_bytes_stay_exact(self):
        self.camera()
        for enabled, message in ((True, 'Manual camera requested'), (False, 'Manual camera cancelled')):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(.6)
                client.connect(self.config['controlSocket'])
                client.sendall(json.dumps({'action': 'camera', 'enabled': enabled}).encode() + b'\n')
                raw = bytearray()
                while True:
                    chunk = client.recv(257)
                    if not chunk:
                        break
                    raw.extend(chunk)
                self.assertEqual(raw, ('{"ok":true,"message":"' + message + '"}\n').encode())
                self.assertLessEqual(len(raw), 256)


if __name__ == '__main__':
    unittest.main()
