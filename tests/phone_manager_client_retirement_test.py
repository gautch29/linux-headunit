#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual manager/endpoint/child retirement; explicit privilege/radio substitutes.

The real CLI signal handler, event loop, Manager, private Unix endpoint and
inherited launcher lock run as-is. Config validation, BlueZ and the receiver
program are fixtures. No GTK, phone, display, systemd or physical claims.
"""
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
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
HERE = str(Path(__file__).resolve())
sys.path.insert(0, str(ROOT / 'tools'))
from phone_manager_endpoint import ManagerEndpoint, MAX_COMMAND_BYTES

PHONE = 'AA:BB:CC:DD:EE:01'


def line(process, timeout=2):
    if not select.select([process.stdout], [], [], timeout)[0]:
        raise AssertionError('Manager fixture event deadline expired')
    raw = process.stdout.readline()
    if not raw:
        raise AssertionError('Manager fixture exited: ' + process.stderr.read().decode(errors='replace'))
    return json.loads(raw)


def reply(client):
    raw = bytearray()
    while not raw.endswith(b'\n'):
        chunk = client.recv(65536)
        if not chunk:
            raise AssertionError('Manager reply closed before newline')
        raw.extend(chunk)
        if len(raw) > MAX_COMMAND_BYTES:
            raise AssertionError('Manager reply exceeded packet bound')
    return json.loads(raw)


class Fixture:
    def __init__(self, folder, mode='normal'):
        self.folder = folder
        self.process = subprocess.Popen([sys.executable, '-B', HERE, '--fixture', str(folder), mode],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        start_new_session=True, bufsize=0)
        self.events = []
        self.trace = folder / f'trace-{self.process.pid}.jsonl'
        try:
            self.ready = self.event('ready')
            self.path = folder / 'state/manager.sock'
        except BaseException:
            self.close()
            raise

    def event(self, kind):
        deadline = time.monotonic() + 2
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError('Manager fixture event deadline expired: ' + kind)
            value = line(self.process, remaining)
            self.events.append(value)
            if value['event'] == kind:
                return value

    def client(self):
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(4)
        client.connect(str(self.path))
        return client

    def request(self, value):
        with self.client() as client:
            client.sendall(json.dumps(value).encode() + b'\n')
            return reply(client)

    def signal(self, number):
        os.kill(self.process.pid, number)
        acknowledged = self.event('signal')
        if acknowledged['number'] != number:
            raise AssertionError('Wrong signal-handler acknowledgement')

    def finish(self, code=0):
        actual = self.process.wait(timeout=5)
        if actual != code:
            raise AssertionError(f'Manager fixture exit {actual}: ' + self.process.stderr.read().decode(errors='replace'))
        self.events = [json.loads(raw) for raw in self.trace.read_text().splitlines()]
        # CTest retains original events in Testing/JUnit, even after the private
        # runtime directory is cleaned. No receipt or source date is invented.
        print('manager-retirement-evidence ' + json.dumps(self.events))
        return self.events

    def close(self):
        # Release only this fixture's held child before fallback process cleanup.
        (self.folder / 'release-child').touch()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=2)
        for stream in (self.process.stdout, self.process.stderr):
            stream.close()


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pm-client-', dir='/tmp')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name).resolve()
        self.folder.chmod(0o700)

    def fixture(self, mode='normal'):
        fixture = Fixture(self.folder, mode)
        self.addCleanup(fixture.close)
        return fixture

    def assert_retired(self, fixture):
        self.assertFalse(fixture.path.exists())
        retired = next(value for value in reversed(fixture.events) if value['event'] == 'retired')
        self.assertTrue(all(code == 0 for code in retired['children']))
        self.assertIsNone(retired['active'])
        self.assertIsNone(retired['nonce'])
        self.assertFalse(retired['media']['available'])
        with ManagerEndpoint(fixture.path.parent, tuple(fixture.ready['owner'])):
            pass
        self.assertFalse(fixture.path.exists())

    def test_signal_rejects_partial_connect_completed_after_actual_handler(self):
        for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=number):
                fixture = self.fixture()
                with fixture.client() as client:
                    client.sendall(b'{"action":"connect","phone":')
                    fixture.event('receive')
                    fixture.signal(number)
                    client.sendall(json.dumps(PHONE).encode() + b'}\n')
                    value = reply(client)
                events = fixture.finish()
                self.assertFalse(value['ok'], json.dumps(events))
                self.assertIn('stopping', value['error'].lower())
                stopped = next(index for index, event in enumerate(events) if event['event'] == 'signal')
                self.assertFalse(any(event['event'] in ('request', 'spawn', 'tick') for event in events[stopped + 1:]))
                self.assert_retired(fixture)
                fixture.close()

    def test_completed_request_and_closed_reply_preserve_dispatch_without_readiness(self):
        fixture = self.fixture()
        with fixture.client() as client:
            client.sendall(json.dumps({'action': 'connect', 'phone': PHONE}).encode() + b'\n')
            fixture.event('receive')
            client.close()  # Full authenticated request may dispatch without a reply reader.
        fixture.event('spawn')
        status = fixture.request({'action': 'status'})
        self.assertTrue(status['ok'])
        self.assertEqual(status['active'], PHONE)
        self.assertIsNone(status['active_mode'])
        self.assertFalse(status['carplay']['returnReady'])
        self.assertFalse(status['phones'][0]['connected'])
        old_generation = status['generation']
        disconnected = fixture.request({'action': 'disconnect', 'phone': PHONE, 'generation': old_generation})
        self.assertTrue(disconnected['ok'])
        fixture.request({'action': 'connect', 'phone': PHONE})
        stale = fixture.request({'action': 'disconnect', 'phone': PHONE, 'generation': old_generation})
        self.assertFalse(stale['ok'])
        current = fixture.request({'action': 'status'})
        self.assertEqual(current['active'], PHONE)
        self.assertGreater(current['generation'], old_generation)
        self.assertFalse(current['carplay']['returnReady'])
        fixture.signal(signal.SIGTERM)
        fixture.finish()
        self.assert_retired(fixture)

    def test_malformed_oversize_and_eof_never_dispatch_and_keep_status_usable(self):
        fixture = self.fixture()
        for raw in (b'[]\n', b'{"action":"connect","action":"status"}\n',
                    b'{"action":"connect","phone":',
                    b'{"action":"connect","phone":"' + b'x' * MAX_COMMAND_BYTES):
            with self.subTest(bytes=len(raw)):
                with fixture.client() as client:
                    try:
                        client.sendall(raw)
                        client.shutdown(socket.SHUT_WR)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    try:
                        self.assertFalse(reply(client)['ok'])
                    except ConnectionResetError:
                        # An oversize sender may retain unread bytes at close.
                        self.assertGreater(len(raw), MAX_COMMAND_BYTES)
                status = fixture.request({'action': 'status'})
                self.assertTrue(status['ok'])
                self.assertIsNone(status['active'])
                self.assertFalse(status['carplay']['returnReady'])
        fixture.signal(signal.SIGTERM)
        events = fixture.finish()
        self.assertFalse(any(event['event'] == 'spawn' for event in events))
        self.assertTrue(all(event['action'] == 'status' for event in events if event['event'] == 'request'))
        self.assert_retired(fixture)

    def test_missing_newline_uses_existing_three_second_budget(self):
        fixture = self.fixture()
        with fixture.client() as client:
            client.sendall(b'{"action":"connect","phone":"' + PHONE.encode() + b'"}')
            fixture.event('receive')
            began = time.monotonic()
            self.assertFalse(reply(client)['ok'])
            self.assertLess(time.monotonic() - began, 3.4)
        self.assertIsNone(fixture.request({'action': 'status'})['active'])
        fixture.signal(signal.SIGTERM)
        fixture.finish()
        self.assert_retired(fixture)

    def test_shutdown_keeps_held_child_lock_until_real_retirement(self):
        fixture = self.fixture('hold')
        self.assertTrue(fixture.request({'action': 'connect', 'phone': PHONE})['ok'])
        fixture.signal(signal.SIGTERM)
        deadline = time.monotonic() + 2
        while not (self.folder / 'child-retiring').exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        self.assertIsNone(fixture.process.poll())
        self.assertTrue(fixture.path.is_socket())
        with self.assertRaisesRegex(ValueError, 'live owner or receiver launcher'):
            ManagerEndpoint(fixture.path.parent, tuple(fixture.ready['owner']))
        (self.folder / 'release-child').touch()
        fixture.finish()
        self.assert_retired(fixture)
        successor = self.fixture()
        status = successor.request({'action': 'status'})
        self.assertIsNone(status['active'])
        self.assertFalse(status['carplay']['returnReady'])
        successor.signal(signal.SIGTERM)
        successor.finish()
        self.assert_retired(successor)

    def test_queued_old_client_cannot_reach_replacement_endpoint(self):
        old = self.fixture()
        with old.client() as accepted:
            accepted.sendall(b'{"action":"status"')
            old.event('receive')
            with old.client() as queued:
                old.signal(signal.SIGTERM)
                accepted.sendall(b'}\n')
                self.assertFalse(reply(accepted)['ok'])
                old.finish()
                self.assert_retired(old)
                successor = self.fixture()
                try:
                    queued.sendall(json.dumps({'action': 'connect', 'phone': PHONE}).encode() + b'\n')
                except (BrokenPipeError, ConnectionResetError):
                    pass
                try:
                    self.assertEqual(queued.recv(4096), b'')
                except ConnectionResetError:
                    pass
                status = successor.request({'action': 'status'})
                self.assertIsNone(status['active'])
                self.assertFalse(status['carplay']['returnReady'])
                old.close()  # Repeated old cleanup must preserve the successor path.
                self.assertTrue(successor.path.is_socket())
                successor.signal(signal.SIGTERM)
                events = successor.finish()
                self.assertFalse(any(event['event'] == 'spawn' for event in events))
                self.assert_retired(successor)

    def test_loop_exception_retires_owned_child_and_endpoint(self):
        fixture = self.fixture('tick-error')
        value = fixture.request({'action': 'connect', 'phone': PHONE})
        self.assertTrue(value['ok'])
        fixture.finish(code=1)
        self.assert_retired(fixture)


def child_main(folder, mode, descriptor):
    # The real inherited descriptor stays held through this tiny child's exit.
    value = os.fstat(int(descriptor))
    if value.st_size != 0:
        raise ValueError('Fixture did not inherit the empty manager lock')
    stopping = False
    def stop(*_):
        nonlocal stopping
        stopping = True
        if mode == 'hold':
            (folder / 'child-retiring').touch()
    signal.signal(signal.SIGTERM, stop)
    print('ready', flush=True)
    while not stopping or mode == 'hold' and not (folder / 'release-child').exists():
        time.sleep(.01)
    return 0


def fixture_main(folder, mode):
    import phone_manager as manager_module
    import phone_manager_endpoint as endpoint_module
    folder = Path(folder)
    trace = folder / f'trace-{os.getpid()}.jsonl'
    def record(event, **values):
        value = {'event': event, **values}
        with trace.open('a') as stream:
            stream.write(json.dumps(value) + '\n')
        print(json.dumps(value), flush=True)

    class PrivilegeBoundary:
        def __getattr__(self, name):
            return getattr(os, name)
        def geteuid(self):
            return 0
    manager_module.os = PrivilegeBoundary()
    uid = os.getuid() or 1
    manager_module.pwd = types.SimpleNamespace(getpwuid=lambda _: types.SimpleNamespace(pw_gid=os.getgid()))
    actual_signal = signal.signal
    def witnessed_signal(number, handler):
        def witnessed(*arguments):
            handler(*arguments)  # Delegate the actual CLI handler exactly once.
            record('signal', number=int(number))
        return actual_signal(number, witnessed)
    manager_module.signal = types.SimpleNamespace(signal=witnessed_signal, SIGINT=signal.SIGINT,
                                                   SIGTERM=signal.SIGTERM, SIGHUP=signal.SIGHUP)
    actual_receive = endpoint_module.ManagerEndpoint._receive
    def receive(endpoint, client):
        record('receive')  # Accepted actual socket, before its original bounded read.
        return actual_receive(endpoint, client)
    endpoint_module.ManagerEndpoint._receive = receive
    class Phones:
        def phones(self):
            return [{'address': PHONE, 'name': 'Synthetic phone', 'connected': False}]
        def phone(self, phone):
            return {'address': phone, 'connected': False}
        def disconnect(self, phone):
            record('bluez-disconnect', phone=phone)
        def media(self, phone):
            return {'available': False}
    actual_manager = manager_module.Manager
    actual_popen = subprocess.Popen
    children = []
    def spawn(command, **options):
        descriptor, = options['pass_fds']
        child = actual_popen([sys.executable, '-B', HERE, '--child', str(folder), mode, str(descriptor)],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             pass_fds=(descriptor,), bufsize=0)
        children.append(child)
        if not select.select([child.stdout], [], [], 2)[0] or child.stdout.readline() != b'ready\n':
            raise AssertionError('Owned child did not acknowledge startup')
        child.stdout.close()
        record('spawn', pid=child.pid)
        return child
    manager_module.subprocess.run = lambda *_, **__: types.SimpleNamespace(returncode=0)
    manager_module.subprocess.Popen = spawn
    def construct(*args, **options):
        manager = actual_manager(*args, **options, bluez=Phones())
        actual_request, actual_tick, actual_shutdown = manager.request, manager.tick, manager.shutdown
        def request(value):
            record('request', action=value.get('action'))
            return actual_request(value)
        def tick():
            record('tick')
            actual_tick()
            if mode == 'tick-error' and manager.child is not None:
                raise RuntimeError('Explicit fixture loop exception')
        def shutdown():
            record('shutdown')
            try:
                actual_shutdown()
            finally:
                record('retired', children=[child.poll() for child in children], active=manager.active,
                       nonce=manager.carplay.nonce, media=manager.media.last)
                for child in children:
                    if child.poll() is None:
                        child.kill()
                    child.wait(timeout=2)
                    child.stderr.close()
        manager.request, manager.tick, manager.shutdown = request, tick, shutdown
        record('ready', owner=list(manager.owner))
        return manager
    manager_module.Manager = construct
    display = folder / 'standard.conf'
    display.write_text('explicit display-validation fixture\n')
    config = folder / 'receiver.json'
    config.write_text(json.dumps({'pair_store': str(folder / 'pairs'), 'display_config': str(display)}))
    sys.argv = ['phone_manager.py', '--config', str(config), '--directory', str(folder / 'state'),
                '--standard-display', str(display), '--uid', str(uid)]
    manager_module.main()
    return 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--fixture':
        raise SystemExit(fixture_main(*sys.argv[2:]))
    if len(sys.argv) > 1 and sys.argv[1] == '--child':
        raise SystemExit(child_main(Path(sys.argv[2]), *sys.argv[3:]))
    unittest.main()
