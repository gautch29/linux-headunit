#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real Python manual-owner / C++ control-boundary interoperability.

Usage: camera_manual_interop_test.py CAMERA_MANUAL_INTEROP_HELPER
Linux exercises the enabled production CLOCK_MONOTONIC adapter. Darwin first
proves an explicit CLOCK_MONOTONIC_RAW fixture clock against the real C++
steady_clock; enabled production remains rejected. No camera, GPU, vehicle,
phone manager service or hardware is opened. The C++ HCS1 publisher is a status
fixture; an active phase here does not qualify actual surface visibility.
"""
import json
import os
from pathlib import Path
import re
import selectors
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from camera_manual import ManualCamera, load_configuration

HELPER = None
WIRE_CLOCK = None
FIXTURE_CLOCK = not sys.platform.startswith('linux')
ON = re.compile(rb'on ([1-9][0-9]{0,19})\n')


def private_json(path, value):
    path.write_text(json.dumps(value), encoding='utf-8')
    path.chmod(0o600)


def clock_candidate():
    if sys.platform.startswith('linux'):
        return lambda: time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    if sys.platform == 'darwin' and hasattr(time, 'CLOCK_MONOTONIC_RAW'):
        # This is an explicitly injected fixture, never a platform fallback in
        # ManualCamera. Its domain is measured against the actual C++ helper.
        return lambda: time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    raise RuntimeError('No supported interop fixture clock for this platform')


def invoke(*arguments, check=True):
    result = subprocess.run([str(HELPER), *map(str, arguments)], capture_output=True,
                            text=True, timeout=2, check=False)
    if len(result.stdout) > 4096 or len(result.stderr) > 4096:
        raise RuntimeError('C++ interop helper output exceeded its bound')
    if check and result.returncode:
        raise RuntimeError('C++ interop helper failed: ' + result.stderr)
    return result


def qualify_clock():
    clock = clock_candidate()
    for _ in range(5):
        before = clock()
        value = int(invoke('clock').stdout.strip())
        after = clock()
        if not 0 < before <= value <= after:
            raise RuntimeError('Python fixture clock and actual C++ steady_clock domains disagree')
    return clock


def wait_for(predicate, message, timeout=1.5):
    until = time.monotonic() + timeout
    while True:
        result = predicate()
        if result:
            return result
        if time.monotonic() >= until:
            raise AssertionError(message)
        time.sleep(0.01)


class Child:
    """Bounded fixture pipes and exact owned subprocess cleanup; no services."""
    def __init__(self, arguments):
        self.error = tempfile.TemporaryFile()
        self.process = subprocess.Popen(arguments, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.error)
        self.pending = bytearray()
        self.was_killed = False

    def line(self, timeout=1.5):
        until = time.monotonic() + timeout
        with selectors.DefaultSelector() as selected:
            selected.register(self.process.stdout, selectors.EVENT_READ)
            while b'\n' not in self.pending:
                remaining = until - time.monotonic()
                if remaining <= 0 or not selected.select(max(0, remaining)):
                    raise AssertionError('Fixture child reply exceeded its deadline: ' + self.errors())
                chunk = os.read(self.process.stdout.fileno(), 4097)
                if not chunk:
                    raise AssertionError('Fixture child exited before reply: ' + self.errors())
                self.pending.extend(chunk)
                if len(self.pending) > 4096:
                    raise AssertionError('Fixture child reply exceeded its bound')
        line, _, self.pending = self.pending.partition(b'\n')
        return json.loads(line)

    def send(self, command):
        self.process.stdin.write(command.encode('ascii') + b'\n')
        self.process.stdin.flush()

    def command(self, command):
        self.send(command)
        return self.line()

    def errors(self):
        self.error.seek(0)
        raw = self.error.read(4097)
        if len(raw) > 4096:
            raise AssertionError('Fixture child errors exceeded their bound')
        return raw.decode('utf-8', errors='replace')

    def kill(self):
        if self.process.poll() is not None:
            raise AssertionError('Fixture child exited before deliberate SIGKILL: ' + self.errors())
        self.process.send_signal(signal.SIGKILL)
        self.process.wait(timeout=2)
        self.was_killed = True
        if self.process.returncode != -signal.SIGKILL:
            raise AssertionError('Fixture child did not terminate from deliberate SIGKILL')

    def close(self):
        cleanup_error = None
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.kill()
                cleanup_error = 'Fixture child required forced cleanup after EOF'
        errors = self.errors()
        if errors or (not self.was_killed and self.process.returncode != 0):
            cleanup_error = 'Fixture child failed during its actual lifecycle: ' + errors
        for stream in (self.process.stdin, self.process.stdout):
            if not stream.closed:
                stream.close()
        self.error.close()
        if cleanup_error:
            raise AssertionError(cleanup_error)


class Fixture:
    def __init__(self, enabled=True):
        # Resolve Darwin's /tmp symlink before giving canonical paths to C++.
        self.temporary = tempfile.TemporaryDirectory(prefix='hu-manual-', dir='/tmp')
        self.folder = Path(self.temporary.name).resolve()
        self.folder.chmod(0o700)
        self.profile = self.folder / 'profile.json'
        self.request = self.folder / 'manual.request'
        self.owner = self.folder / 'manual.owner'
        self.status = self.folder / 'native.status'
        self.socket = self.folder / 'manual.sock'
        self.configuration = {
            'schemaVersion': 1, 'enabled': enabled, 'requestFile': str(self.request),
            'ownerFile': str(self.owner), 'statusFile': str(self.status),
            'controlSocket': str(self.socket),
            'timing': {'leaseMs': 300, 'pollMs': 10, 'statusMaximumAgeMs': 150},
        }
        private_json(self.profile, self.configuration)
        self.children = []
        self.camera = None

    def start(self):
        options = {'clock_ns': WIRE_CLOCK} if FIXTURE_CLOCK else {}
        self.camera = ManualCamera(str(self.profile), (os.getuid(), os.getgid()), **options)
        return self.camera

    def publisher(self):
        child = Child([str(HELPER), 'status-publish', str(self.status), '5000'])
        self.children.append(child)
        first = child.line()
        if first['phase'] != 'idle' or not 0 < first['instance'] <= first['observed'] or first['sequence'] != 1:
            raise AssertionError('C++ status fixture did not publish its genuine initial observation')
        return child

    def owner_process(self):
        arguments = [sys.executable, str(Path(__file__).resolve()), '--manual-worker', str(self.profile)]
        if FIXTURE_CLOCK:
            arguments.append('--fixture-clock')
        child = Child(arguments)
        self.children.append(child)
        first = child.line()
        if not first['configured'] or first['requested']:
            raise AssertionError('Manual worker did not start configured and OFF')
        return child

    def actual_request(self):
        return invoke('request', self.request).stdout.strip()

    def timed_on(self):
        raw = self.request.read_bytes()
        match = ON.fullmatch(raw)
        if match is None or len(raw) > 24:
            raise AssertionError('Python owner emitted an indefinite or malformed ON')
        now, deadline = WIRE_CLOCK(), int(match[1])
        if not now < deadline <= now + self.configuration['timing']['leaseMs'] * 1_000_000:
            raise AssertionError('Python owner emitted an expired or excessive lease')
        return deadline

    def private_endpoint(self, path, kind='regular'):
        info = path.lstat()
        expected = stat.S_ISSOCK(info.st_mode) if kind == 'socket' else stat.S_ISREG(info.st_mode)
        if not expected or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1:
            raise AssertionError('Interop endpoint is not owned private single-link data')

    def close(self):
        errors = []
        try:
            if self.camera is not None:
                self.camera.close()
        except Exception as error:
            errors.append(error)
        for child in reversed(self.children):
            try:
                child.close()
            except Exception as error:
                errors.append(error)
        self.temporary.cleanup()
        if errors:
            raise AssertionError('Interop fixture cleanup failed: ' + str(errors[0]))


class ManualInterop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global WIRE_CLOCK
        WIRE_CLOCK = qualify_clock()

    def setUp(self):
        self.fixture = Fixture()

    def tearDown(self):
        self.fixture.close()

    def test_01_actual_clock_domain_is_bracketed(self):
        self.assertGreater(WIRE_CLOCK(), 0)
        expected = 'production Linux CLOCK_MONOTONIC' if not FIXTURE_CLOCK else 'injected Darwin CLOCK_MONOTONIC_RAW fixture'
        print('Clock qualification: actual C++ steady_clock brackets ' + expected, flush=True)

    def test_02_enabled_platform_guard_and_disabled_no_endpoints(self):
        f = self.fixture
        self.assertEqual(load_configuration(f.profile), f.configuration)
        self.assertEqual(set(f.folder.iterdir()), {f.profile})
        if FIXTURE_CLOCK:
            with self.assertRaisesRegex(ValueError, 'Linux CLOCK_MONOTONIC'):
                ManualCamera(str(f.profile), (os.getuid(), os.getgid()))
            self.assertEqual(set(f.folder.iterdir()), {f.profile})
        f.configuration['enabled'] = False
        private_json(f.profile, f.configuration)
        camera = ManualCamera(str(f.profile), (os.getuid(), os.getgid()))
        try:
            self.assertFalse(camera.snapshot()['configured'])
            self.assertFalse(camera.snapshot()['requested'])
            self.assertEqual(set(f.folder.iterdir()), {f.profile})
        finally:
            camera.close()

    def test_03_real_reader_and_immutable_atomic_renewal(self):
        f = self.fixture
        camera = f.start()
        self.assertEqual(f.actual_request(), 'off')
        with f.request.open('rb') as retained:
            self.assertTrue(camera.request(True)['requested'])
            wait_for(lambda: f.actual_request() == 'on', 'C++ reader did not accept Python timed request')
            first = f.timed_on()
            wait_for(lambda: f.timed_on() > first, 'Manual worker did not renew its real deadline')
            self.assertEqual(retained.read(), b'off\n')
            self.assertEqual(os.fstat(retained.fileno()).st_nlink, 0)
        for path in (f.request, f.owner, Path(str(f.socket) + '.lock')):
            f.private_endpoint(path)
        f.private_endpoint(f.socket, 'socket')
        camera.request(False)
        wait_for(lambda: f.actual_request() == 'off', 'Manual OFF did not reach actual C++ reader')
        self.assertEqual(f.request.read_bytes(), b'off\n')

    def test_04_genuine_hcs1_phase_and_original_expiry_are_independent(self):
        f = self.fixture
        peer = f.publisher()
        camera = f.start()
        wait_for(lambda: camera.snapshot()['phase'] == 'idle', 'Python did not read actual C++ HCS1 idle')
        active = peer.command('active')
        self.assertGreater(active['sequence'], 1)
        wait_for(lambda: camera.snapshot()['phase'] == 'active', 'Python did not read actual C++ HCS1 active')
        self.assertFalse(camera.snapshot()['requested'])
        self.assertEqual(f.actual_request(), 'off')
        camera.request(True)
        wait_for(lambda: f.actual_request() == 'on', 'Manual request did not activate its own lease')
        peer.kill()
        original = f.status.read_bytes()
        wait_for(lambda: camera.snapshot()['phase'] == 'unavailable', 'Original HCS1 heartbeat never expired')
        self.assertEqual(f.status.read_bytes(), original)
        self.assertTrue(camera.snapshot()['requested'])
        self.assertEqual(f.actual_request(), 'on')
        # The managed camera controller independently requires a fresh owner;
        # this UI snapshot neither creates an ACK nor invents a presentation.

    def test_05_native_close_ack_reaches_real_off_without_visibility_claim(self):
        f = self.fixture
        peer = f.publisher()
        camera = f.start()
        peer.command('active')
        wait_for(lambda: camera.snapshot()['phase'] == 'active', 'HCS1 active fixture missing')
        camera.request(True)
        wait_for(lambda: f.actual_request() == 'on', 'Native Close fixture did not start ON')
        reply = json.loads(invoke('manual-client', f.socket).stdout)
        self.assertEqual(reply['state'], 'accepted')
        self.assertGreater(reply['sequence'], 0)
        wait_for(lambda: f.actual_request() == 'off', 'Accepted native Close did not produce actual OFF')
        self.assertEqual(f.request.read_bytes(), b'off\n')
        self.assertFalse(camera.snapshot()['requested'])
        self.assertEqual(camera.snapshot()['phase'], 'active')

    def test_06_sigkill_owner_leaves_only_original_expiring_lease(self):
        f = self.fixture
        owner = f.owner_process()
        self.assertTrue(owner.command('on')['requested'])
        wait_for(lambda: f.actual_request() == 'on', 'Worker request never reached actual C++ reader')
        owner.kill()
        original = f.request.read_bytes()
        self.assertIsNotNone(ON.fullmatch(original))
        f.timed_on()
        wait_for(lambda: f.actual_request() == 'off', 'SIGKILL left an indefinite active manual request')
        self.assertEqual(f.request.read_bytes(), original)

    def test_07_native_close_while_owner_command_loop_is_blocked(self):
        f = self.fixture
        owner = f.owner_process()
        owner.command('on')
        wait_for(lambda: f.actual_request() == 'on', 'Blocked-owner fixture never became ON')
        first = f.timed_on()
        owner.send('block')
        wait_for(lambda: f.timed_on() > first, 'Manual renewal depends on the fixture owner command loop')
        reply = json.loads(invoke('manual-client', f.socket).stdout)
        self.assertEqual(reply['state'], 'accepted')
        wait_for(lambda: f.actual_request() == 'off', 'Independent native Close worker did not write OFF')
        self.assertFalse(owner.line()['requested'])

    def test_08_close_withdraws_and_removes_only_owned_socket(self):
        f = self.fixture
        camera = f.start()
        camera.request(True)
        wait_for(lambda: f.actual_request() == 'on', 'Close fixture never became ON')
        camera.close()
        self.assertEqual(f.actual_request(), 'off')
        self.assertEqual(f.request.read_bytes(), b'off\n')
        self.assertFalse(f.socket.exists())
        self.assertFalse(camera.snapshot()['configured'])
        self.assertFalse(camera.snapshot()['requested'])

    def test_09_original_cpp_receipt_survives_same_read_and_expiry(self):
        f = self.fixture
        peer = f.publisher()
        camera = f.start()
        peer.command('active')
        peer.kill()
        raw = f.status.read_bytes()
        fields = raw.decode('ascii').split()
        self.assertEqual(fields[0], 'HCS1')
        legacy, observation = camera.snapshot_with_status_observation()
        receipt = {'instanceStartNs': fields[1], 'sequence': fields[2],
                   'observedNs': fields[3], 'phase': fields[4]}
        self.assertEqual(legacy['phase'], 'active')
        self.assertFalse(legacy['requested'])
        self.assertEqual(observation['lastObservation'], receipt)
        self.assertEqual(observation['clockDomain'],
                         'injected_fixture' if FIXTURE_CLOCK else 'linux_clock_monotonic')
        self.assertEqual(camera.snapshot_with_status_observation()[1]['lastObservation'], receipt)
        wait_for(lambda: camera.snapshot_with_status_observation()[1]['availability'] == 'unavailable',
                 'Original C++ owner receipt did not expire')
        stale = camera.snapshot_with_status_observation()[1]
        self.assertEqual(stale['lastObservation'], receipt)
        self.assertEqual(f.status.read_bytes(), raw)
        self.assertEqual(stale['availability'], 'unavailable')
        camera.close()
        self.assertFalse(camera.snapshot_with_status_observation()[1]['eligible'])


def manual_worker(profile, fixture_clock):
    """Test-owned process embedding the actual module, never a manager service."""
    options = {'clock_ns': clock_candidate()} if fixture_clock else {}
    camera = ManualCamera(profile, (os.getuid(), os.getgid()), **options)
    def reply(value):
        print(json.dumps(value, separators=(',', ':')), flush=True)
    try:
        reply(camera.snapshot())
        until = time.monotonic() + 5
        with selectors.DefaultSelector() as selected:
            selected.register(sys.stdin, selectors.EVENT_READ)
            pending = bytearray()
            while time.monotonic() < until:
                if not selected.select(0.1):
                    continue
                chunk = os.read(sys.stdin.fileno(), 65)
                if not chunk:
                    return
                pending.extend(chunk)
                if len(pending) > 64:
                    raise ValueError('Manual owner fixture commands exceeded bound')
                while b'\n' in pending:
                    raw, _, pending = pending.partition(b'\n')
                    command = raw.decode('ascii')
                    if command in ('on', 'off'):
                        reply(camera.request(command == 'on'))
                    elif command == 'snapshot':
                        reply(camera.snapshot())
                    elif command == 'block':
                        # Only the fixture's owner command loop is blocked. The
                        # production module owns renewal and native IPC itself.
                        time.sleep(0.5)
                        reply(camera.snapshot())
                    else:
                        raise ValueError('Unknown manual owner fixture command')
    finally:
        camera.close()


if __name__ == '__main__':
    if len(sys.argv) in (3, 4) and sys.argv[1] == '--manual-worker':
        if len(sys.argv) == 4 and sys.argv[3] != '--fixture-clock':
            raise SystemExit('Unknown worker fixture option')
        manual_worker(sys.argv[2], len(sys.argv) == 4)
    else:
        if len(sys.argv) != 2:
            raise SystemExit('Usage: camera_manual_interop_test.py CAMERA_MANUAL_INTEROP_HELPER')
        HELPER = Path(sys.argv.pop()).resolve(strict=True)
        unittest.main(verbosity=2)
