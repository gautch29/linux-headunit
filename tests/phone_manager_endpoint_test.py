#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual private Unix endpoint/process tests; no manager/phone/appliance I/O."""
import contextlib
import errno
import fcntl
import json
import os
from pathlib import Path
import select
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from filesystem_fixture import change_metadata

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_manager_endpoint import ManagerEndpoint
import phone_manager_endpoint as endpoint_module


HERE = str(Path(__file__).resolve())


def wait_line(process, timeout=2):
    readable, _, _ = select.select([process.stdout], [], [], timeout)
    if not readable:
        raise AssertionError('Private endpoint fixture did not respond before its deadline')
    line = process.stdout.readline(4096)
    if not line:
        raise AssertionError('Private endpoint fixture exited: ' + process.stderr.read(4096).decode(errors='replace'))
    return json.loads(line)


def reply(client):
    raw = bytearray()
    while not raw.endswith(b'\n'):
        chunk = client.recv(65536)
        if not chunk:
            raise AssertionError('Actual endpoint reply closed before its newline')
        raw.extend(chunk)
        if len(raw) > 1_500_000:
            raise AssertionError('Actual endpoint reply exceeded its bound')
    return json.loads(raw)


def command(path, raw=b'{"action":"snapshot"}\n'):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(2)
        client.connect(str(path))
        try:
            client.sendall(raw)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Oversized input is rejected before its entire write completes.
        return reply(client)


class Runtime:
    def __init__(self):
        self.temp = tempfile.TemporaryDirectory(prefix='headunit-manager-', dir='/tmp')
        self.path = Path(self.temp.name).resolve(strict=True)
        self.path.chmod(0o700)

    def close(self):
        self.temp.cleanup()


class Fixture:
    def __init__(self, folder, mode='normal', timeout=.4):
        self.child = None
        self.process = subprocess.Popen([sys.executable, HERE, '--fixture', str(folder), mode, str(timeout)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        start_new_session=True)
        try:
            self.ready = wait_line(self.process)
            if self.ready.get('ok') is not True:
                raise AssertionError('Endpoint fixture startup failed: ' + str(self.ready))
            self.path = Path(self.ready['path'])
            self.child = self.ready.get('child')
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.process is None:
            return
        # Every process group was created by this fixture. No ambient services
        # or sessions are targeted; inherited children stay in this group.
        try:
            os.killpg(self.process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except PermissionError:
            # Darwin can return EPERM for a zombie-only group. A reaped direct
            # child and no known live inheritor are the only harmless case.
            if self.process.poll() is None or self.child is not None:
                raise
        self.process.wait(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()
        self.process = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


@contextlib.contextmanager
def legacy(folder, listening=True):
    path = folder / 'manager.sock'
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(path))
    path.chmod(0o600)
    if listening:
        listener.listen(2)
    else:
        listener.close()
    try:
        yield path, listener
    finally:
        listener.close()
        path.unlink(missing_ok=True)


class EndpointTests(unittest.TestCase):
    def setUp(self):
        self.runtime = Runtime()
        self.folder = self.runtime.path
        self.addCleanup(self.runtime.close)

    def test_private_generation_context_and_no_global_umask_change(self):
        observed = os.umask(0o077)
        os.umask(observed)
        with ManagerEndpoint(self.folder) as endpoint:
            self.assertEqual(endpoint.path, self.folder / 'manager.sock')
            self.assertEqual(stat.S_IMODE(endpoint.path.lstat().st_mode), 0o600)
            lock = self.folder / 'manager.lock'
            self.assertEqual(stat.S_IMODE(lock.lstat().st_mode), 0o600)
            self.assertEqual(lock.read_bytes(), b'')
            self.assertEqual(lock.lstat().st_nlink, 1)
            self.assertFalse(os.get_inheritable(endpoint.inherited_lock_fd))
            actual = os.umask(observed)
            self.assertEqual(actual, observed)
        self.assertFalse(endpoint.path.exists())
        self.assertTrue(lock.exists())
        self.assertFalse(endpoint.close())
        with self.assertRaises(ValueError):
            _ = endpoint.inherited_lock_fd

    def test_actual_partial_io_strict_commands_and_callback_errors(self):
        with Fixture(self.folder) as fixture:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(2)
                client.connect(str(fixture.path))
                client.sendall(b'{"action":')
                client.sendall(b'"snapshot","value":{"nested":true}}\n')
                value = reply(client)
                self.assertTrue(value['ok'])
                self.assertEqual(value['echo']['value'], {'nested': True})
                self.assertEqual(value['count'], 1)
            for raw in (b'[]\n', b'{"action":"snapshot","action":"other"}\n',
                        b'{"action":"snapshot","v":{"x":1,"x":2}}\n',
                        b'{"action":"snapshot","v":NaN}\n', b'{"action":"snapshot","v":1e999}\n',
                        b'{"action":"snapshot"}\n{}\n', b'\xff\n'):
                self.assertFalse(command(fixture.path, raw)['ok'])
            self.assertEqual(command(fixture.path)['count'], 2)
            failed = command(fixture.path, b'{"action":"fail"}\n')
            self.assertFalse(failed['ok'])
            self.assertEqual(failed['error'], 'Explicit fixture callback failure')
            self.assertEqual(command(fixture.path)['count'], 4)

    def test_one_incoming_request_at_a_time_without_background_execution(self):
        with Fixture(self.folder) as fixture:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as first, socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as second:
                for client in (first, second):
                    client.settimeout(2)
                    client.connect(str(fixture.path))
                first.sendall(b'{"action":')
                second.sendall(b'{"action":"second"}\n')
                self.assertFalse(select.select([second], [], [], .025)[0])
                first.sendall(b'"first"}\n')
                self.assertEqual(reply(first)['count'], 1)
                self.assertEqual(reply(second)['count'], 2)

    def test_size_bounds_for_real_incoming_and_outgoing_bytes(self):
        with Fixture(self.folder, timeout=1) as fixture:
            big = json.dumps({'action': 'snapshot', 'value': 'x' * 1_499_800}, separators=(',', ':')).encode() + b'\n'
            accepted = command(fixture.path, big)
            self.assertTrue(accepted['ok'])
            self.assertEqual(len(accepted['echo']['value']), 1_499_800)
            oversized = b'{"action":"snapshot","value":"' + b'x' * 1_500_001 + b'"}\n'
            self.assertFalse(command(fixture.path, oversized)['ok'])
            self.assertEqual(command(fixture.path)['count'], 2)
            too_large_reply = command(fixture.path, b'{"action":"huge-reply"}\n')
            self.assertFalse(too_large_reply['ok'])
            self.assertIn('size bound', too_large_reply['error'])

    def test_fragmented_large_command_preserves_framing_and_deadline(self):
        raw = b'{"action":"snapshot","value":"' + b'x' * 131_072 + b'"}\n'
        class Fragments:
            offset = 0
            def settimeout(client, remaining):
                self.assertGreater(remaining, 0)
                self.assertLessEqual(remaining, 3)
            def recv(client, maximum):
                count = min(17, maximum)
                chunk = raw[client.offset:client.offset + count]
                client.offset += len(chunk)
                return chunk
        with ManagerEndpoint(self.folder) as endpoint:
            result = endpoint._receive(Fragments())
            self.assertEqual(result, {'action': 'snapshot', 'value': 'x' * 131_072})

    def test_total_incoming_deadline_does_not_renew_on_progress(self):
        # Progress at 0.2 s would renew a 0.3 s deadline to 0.5 s. The bound
        # leaves 0.15 s for scheduler jitter on hosted runners on either side.
        with Fixture(self.folder, timeout=.3) as fixture:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(2)
                client.connect(str(fixture.path))
                began = time.monotonic()
                client.sendall(b'{"action":')
                self.assertFalse(select.select([client], [], [], .2)[0])
                client.sendall(b'"snapshot"')
                value = reply(client)
                self.assertFalse(value['ok'])
                self.assertLess(time.monotonic() - began, .45)
            self.assertEqual(command(fixture.path)['count'], 1)

    def test_sigkill_stale_socket_is_reclaimed_and_restart_serves(self):
        with Fixture(self.folder) as fixture:
            self.assertTrue(command(fixture.path)['ok'])
            old = fixture.path.lstat()
            os.kill(fixture.process.pid, signal.SIGKILL)
            fixture.process.wait(timeout=2)
            self.assertEqual(fixture.path.lstat().st_ino, old.st_ino)
            with Fixture(self.folder) as replacement:
                new = replacement.path.lstat()
                # ext4/overlayfs may reuse the freed inode number at once; the
                # change time still identifies a newly created socket.
                self.assertNotEqual((old.st_dev, old.st_ino, old.st_ctime_ns),
                                    (new.st_dev, new.st_ino, new.st_ctime_ns))
                self.assertTrue(command(replacement.path)['ok'])

    def test_nonblocking_concurrent_owner_refusal_before_callback_side_effects(self):
        with Fixture(self.folder) as fixture:
            previous = fixture.path.lstat()
            began = time.monotonic()
            result = subprocess.run([sys.executable, HERE, '--fixture', str(self.folder), 'normal', '.1'],
                                    capture_output=True, timeout=2)
            self.assertEqual(result.returncode, 2)
            self.assertIn('live owner', json.loads(result.stdout)['error'])
            self.assertLess(time.monotonic() - began, 1)
            self.assertEqual(fixture.path.lstat().st_ino, previous.st_ino)
            self.assertTrue(command(fixture.path)['ok'])

    def test_live_legacy_listener_without_lock_is_not_reclaimed(self):
        with legacy(self.folder) as (path, listener):
            before = path.lstat()
            with self.assertRaisesRegex(ValueError, 'live listener'):
                ManagerEndpoint(self.folder)
            self.assertEqual(path.lstat().st_ino, before.st_ino)
            client, _ = listener.accept()  # The refusal probe reached this real live listener.
            client.close()

    def test_real_refused_socket_replacement_during_probe_is_not_unlinked(self):
        with legacy(self.folder, listening=False) as (path, _):
            real_socket = socket.socket
            replacement = None
            class RacingProbe:
                def __enter__(probe):
                    probe.connection = real_socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    return probe
                def __exit__(probe, *_):
                    probe.connection.close()
                def settimeout(probe, value):
                    probe.connection.settimeout(value)
                def connect(probe, address):
                    nonlocal replacement
                    try:
                        probe.connection.connect(address)
                    except OSError as error:
                        if error.errno != errno.ECONNREFUSED:
                            raise
                        # A real kernel refusal is followed by a deliberately
                        # synchronized new listener at the same pathname.
                        path.unlink()
                        replacement = real_socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        replacement.bind(str(path)); path.chmod(0o600); replacement.listen(1)
                        raise
            try:
                with mock.patch.object(endpoint_module.socket, 'socket', side_effect=lambda *_: RacingProbe()):
                    with self.assertRaisesRegex(ValueError, 'changed while checking'):
                        ManagerEndpoint(self.folder)
                self.assertTrue(stat.S_ISSOCK(path.lstat().st_mode))
                with real_socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.settimeout(.2); client.connect(str(path))
                accepted, _ = replacement.accept(); accepted.close()
            finally:
                if replacement is not None:
                    replacement.close()

    def test_unsafe_directory_lock_and_endpoint_refusal(self):
        for path in ('relative', str(self.folder) + '/.', str(self.folder) + '/../elsewhere'):
            with self.assertRaises(ValueError):
                ManagerEndpoint(path)
        for mode in (0o755, 0o1700):
            self.folder.chmod(mode)
            with self.assertRaises(ValueError):
                ManagerEndpoint(self.folder)
            self.folder.chmod(0o700)
        alias = self.folder / 'alias'
        alias.symlink_to(self.folder, target_is_directory=True)
        with self.assertRaises(ValueError):
            ManagerEndpoint(alias)
        alias.unlink()
        lock = self.folder / 'manager.lock'
        for data, mode in ((b'owned', 0o600), (b'', 0o644)):
            lock.write_bytes(data)
            lock.chmod(mode)
            with self.assertRaises(ValueError):
                ManagerEndpoint(self.folder)
            self.assertEqual(lock.read_bytes(), data)
            lock.unlink()
        target = self.folder / 'elsewhere'
        target.write_bytes(b'never alter')
        target.chmod(0o600)
        lock.touch(mode=0o600)
        linked_lock = self.folder / 'linked-lock'
        os.link(lock, linked_lock)
        with self.assertRaises(ValueError):
            ManagerEndpoint(self.folder)
        self.assertEqual(linked_lock.lstat().st_nlink, 2)
        lock.unlink(); linked_lock.unlink()
        lock.symlink_to(target)
        with self.assertRaises(ValueError):
            ManagerEndpoint(self.folder)
        lock.unlink()
        endpoint_path = self.folder / 'manager.sock'
        for kind in ('regular', 'symlink', 'unsafe-socket'):
            if kind == 'regular':
                endpoint_path.write_bytes(b'keep regular')
                endpoint_path.chmod(0o600)
            elif kind == 'symlink':
                endpoint_path.symlink_to(target)
            else:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.bind(str(endpoint_path)); sock.close()
                endpoint_path.chmod(0o644)
            before = endpoint_path.lstat()
            with self.assertRaises(ValueError):
                ManagerEndpoint(self.folder)
            self.assertEqual(endpoint_path.lstat().st_ino, before.st_ino)
            self.assertEqual(target.read_bytes(), b'never alter')
            endpoint_path.unlink()
        with legacy(self.folder, listening=False) as (path, _):
            linked_socket = self.folder / 'linked-socket'
            os.link(path, linked_socket)
            try:
                with self.assertRaises(ValueError):
                    ManagerEndpoint(self.folder)
                self.assertEqual(path.lstat().st_nlink, 2)
            finally:
                linked_socket.unlink()

    def test_old_owner_never_cleans_replaced_socket_lock_or_parent(self):
        for replacement in ('socket', 'lock', 'parent'):
            runtime = Runtime()
            self.addCleanup(runtime.close)
            endpoint = ManagerEndpoint(runtime.path)
            self.addCleanup(endpoint.close)
            if replacement == 'socket':
                endpoint.path.unlink()
                endpoint.path.write_bytes(b'new endpoint generation')
                endpoint.path.chmod(0o600)
                preserved = endpoint.path
            elif replacement == 'lock':
                lock = runtime.path / 'manager.lock'
                lock.unlink(); lock.touch(mode=0o600)
                preserved = endpoint.path
            else:
                moved = runtime.path.with_name(runtime.path.name + '-moved')
                runtime.path.rename(moved)
                self.addCleanup(lambda path=moved: __import__('shutil').rmtree(path))
                runtime.path.mkdir(mode=0o700)
                preserved = runtime.path / 'manager.sock'
                preserved.write_bytes(b'new directory generation')
                preserved.chmod(0o600)
            before = preserved.lstat()
            with self.assertRaises((ValueError, OSError)):
                _ = endpoint.inherited_lock_fd
            self.assertFalse(endpoint.close())
            self.assertEqual(preserved.lstat().st_ino, before.st_ino)

    def test_restored_metadata_does_not_hide_a_changed_generation(self):
        for name in ('manager.sock', 'manager.lock'):
            runtime = Runtime()
            self.addCleanup(runtime.close)
            endpoint = ManagerEndpoint(runtime.path)
            self.addCleanup(endpoint.close)
            path = runtime.path / name
            before = path.lstat()
            change_metadata(path, lambda: path.chmod(0o644), before)
            path.chmod(0o600)
            self.assertEqual(path.lstat().st_ino, before.st_ino)
            with self.assertRaisesRegex(ValueError, 'generation changed'):
                _ = endpoint.inherited_lock_fd
            self.assertFalse(endpoint.close())
            self.assertTrue(endpoint.path.exists())

    def test_inherited_launcher_retains_lock_after_parent_close(self):
        endpoint = ManagerEndpoint(self.folder)
        self.addCleanup(endpoint.close)
        descriptor = endpoint.inherited_lock_fd
        child = subprocess.Popen([sys.executable, HERE, '--hold-fd', str(descriptor)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 pass_fds=(descriptor,))
        try:
            self.assertTrue(wait_line(child)['ok'])
            endpoint.close()
            with self.assertRaisesRegex(ValueError, 'live owner'):
                ManagerEndpoint(self.folder)
            child.stdin.write(b'exit\n'); child.stdin.flush()
            self.assertEqual(child.wait(timeout=2), 0)
            with ManagerEndpoint(self.folder):
                pass
        finally:
            if child.poll() is None:
                child.kill(); child.wait(timeout=2)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()

    def test_inherited_launcher_retains_lock_after_sigkill(self):
        with Fixture(self.folder, mode='inherit') as fixture:
            os.kill(fixture.process.pid, signal.SIGKILL)
            fixture.process.wait(timeout=2)
            with self.assertRaisesRegex(ValueError, 'live owner'):
                ManagerEndpoint(self.folder)
            os.kill(fixture.child, signal.SIGKILL)
            until = time.monotonic() + 2
            while True:
                try:
                    endpoint = ManagerEndpoint(self.folder)
                except ValueError:
                    if time.monotonic() >= until:
                        raise AssertionError('Inherited launcher lock did not release after its exit')
                    select.select([], [], [], .005)
                else:
                    endpoint.close()
                    break
            fixture.child = None


def fixture_main(folder, mode, timeout):
    child = None
    try:
        with ManagerEndpoint(folder, incoming_timeout=float(timeout)) as endpoint:
            if mode == 'inherit':
                child = subprocess.Popen([sys.executable, HERE, '--hold-fd', str(endpoint.inherited_lock_fd)],
                                         stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                         pass_fds=(endpoint.inherited_lock_fd,))
            print(json.dumps({'ok': True, 'path': str(endpoint.path), 'child': child.pid if child else None}), flush=True)
            calls = 0
            def callback(value):
                nonlocal calls
                calls += 1
                if value.get('action') == 'fail':
                    raise ValueError('Explicit fixture callback failure')
                if value.get('action') == 'huge-reply':
                    return {'value': 'x' * 1_500_001}
                return {'echo': value, 'count': calls}
            while not select.select([sys.stdin], [], [], 0)[0]:
                endpoint.serve_once(callback)
    except (OSError, ValueError) as error:
        print(json.dumps({'ok': False, 'error': str(error)}), flush=True)
        return 2
    finally:
        if child is not None and child.poll() is None:
            child.kill(); child.wait(timeout=2)
    return 0


def hold_fd(descriptor):
    value = os.fstat(int(descriptor))
    if not stat.S_ISREG(value.st_mode) or value.st_size != 0:
        raise ValueError('Inherited fixture descriptor is not the real empty ownership lock')
    print('{"ok":true}', flush=True)
    select.select([sys.stdin], [], [], 5)
    return 0


if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == '--fixture':
        raise SystemExit(fixture_main(*sys.argv[2:]))
    if len(sys.argv) == 3 and sys.argv[1] == '--hold-fd':
        raise SystemExit(hold_fd(sys.argv[2]))
    unittest.main()
