#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual private D-Bus/Gio tests with an explicit fake BlueZ radio service.

No system bus, Bluetooth device or phone is used. Dependencies intentionally
fail when absent; the opt-in CMake test must not silently skip its evidence.
"""
import json
import os
from pathlib import Path
import queue
import select
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import warnings
from unittest import mock

warnings.filterwarnings('ignore', category=DeprecationWarning, module=r'gi\.events')
from gi.repository import Gio, GLib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import phone_pairing as pairing


_MANAGER = '''<node><interface name="org.bluez.AgentManager1">
<method name="RegisterAgent"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
<method name="UnregisterAgent"><arg type="o" direction="in"/></method>
<method name="RequestDefaultAgent"><arg type="o" direction="in"/></method>
</interface></node>'''
_PROPERTIES = '''<node><interface name="org.freedesktop.DBus.Properties">
<method name="GetAll"><arg type="s" direction="in"/><arg type="a{sv}" direction="out"/></method>
<method name="Set"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/></method>
<signal name="PropertiesChanged"><arg type="s"/><arg type="a{sv}"/><arg type="as"/></signal>
</interface></node>'''
_DEVICE = '''<node><interface name="org.bluez.Device1"><method name="CancelPairing"/></interface></node>'''
PHONE = '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF'
OTHER = '/org/bluez/hci0/dev_11_22_33_44_55_66'
FOREIGN = '/org/bluez/hci1/dev_AA_BB_CC_DD_EE_FF'


def owned_line(stream, deadline):
    raw = bytearray()
    while len(raw) < 4096:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([stream], [], [], remaining)[0]:
            raise AssertionError('Owned private bus startup line exceeded its deadline')
        byte = os.read(stream.fileno(), 1)
        if not byte:
            raise AssertionError('Owned private bus closed before its startup line')
        raw.extend(byte)
        if byte == b'\n':
            return bytes(raw)
    raise AssertionError('Owned private bus startup line exceeded its size bound')


def variant(value):
    return GLib.Variant('b' if type(value) is bool else 'u' if type(value) is int else 'o' if value.startswith('/org/') else 's', value)


class PrivateBus:
    def __init__(self):
        binary = os.environ.get('HEADUNIT_TEST_DBUS_DAEMON') or shutil.which('dbus-daemon')
        if not binary:
            raise RuntimeError('The explicitly enabled pairing test requires dbus-daemon')
        self.runtime = tempfile.TemporaryDirectory(prefix='headunit-pairing-', dir='/tmp')
        self.folder = Path(self.runtime.name).resolve()
        self.folder.chmod(0o700)
        self.path = self.folder / 'bus'
        try:
            self.process = subprocess.Popen([binary, '--session', '--nofork', '--print-address=1', '--print-pid=1',
                                             '--address=unix:path=' + str(self.path)],
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        except BaseException:
            self.runtime.cleanup()
            raise
        try:
            deadline = time.monotonic() + 2
            self.address = owned_line(self.process.stdout, deadline).decode().strip()
            if int(owned_line(self.process.stdout, deadline)) != self.process.pid:
                raise AssertionError('Private bus PID provenance mismatch')
            info = self.path.lstat()
            if self.process.poll() is not None or not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise AssertionError('Private bus socket provenance mismatch')
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait(timeout=2)
        self.process.stdout.close(); self.process.stderr.close()
        self.runtime.cleanup()


class Call:
    def __init__(self):
        self.event = threading.Event()
        self.error = None
        self.result = None

    def wait(self, timeout=2):
        if not self.event.wait(timeout):
            raise AssertionError('Owned fake BlueZ Agent call exceeded its deadline')
        return self


class FakeBluez:
    """Deliberate radio substitute, served on a real separate Gio connection."""
    def __init__(self, address):
        self.address = address
        self.adapter = {'Powered': True, 'Discoverable': False, 'Pairable': True,
                        'DiscoverableTimeout': 180, 'PairableTimeout': 0, 'Alias': 'Headunit fixture'}
        self.devices = {
            PHONE: {'Adapter': '/org/bluez/hci0', 'Address': 'AA:BB:CC:DD:EE:FF', 'Alias': 'Fixture iPhone', 'Paired': False, 'Trusted': False},
            OTHER: {'Adapter': '/org/bluez/hci0', 'Address': '11:22:33:44:55:66', 'Alias': 'Second fixture', 'Paired': False, 'Trusted': False},
            FOREIGN: {'Adapter': '/org/bluez/hci1', 'Address': 'AA:BB:CC:DD:EE:FF', 'Alias': 'Other adapter', 'Paired': False, 'Trusted': False},
        }
        self.writes, self.methods, self.agent_calls = [], [], []
        self.agent = None
        self.default = False
        self.auto_pairable_agent = False
        self.fail = set()
        self.ignore = set()
        self.drop_on_device_read = False
        self.defer = set()
        self.deferred = []
        self.deferred_ready = threading.Event()
        self.commands = queue.Queue()
        self.ready = threading.Event()
        self.stop = threading.Event()
        self.failure = None
        self.cancellable = Gio.Cancellable.new()
        self.thread = threading.Thread(target=self._run, name='explicit-fake-bluez')
        self.thread.start()
        if not self.ready.wait(2):
            self.close()
            raise AssertionError('Fake BlueZ service did not start')
        if self.failure:
            self.close()
            raise self.failure

    def command(self, function):
        result, done = {}, threading.Event()
        self.commands.put((function, result, done))
        if not done.wait(2):
            raise AssertionError('Fake BlueZ control command exceeded deadline')
        if 'error' in result:
            raise result['error']
        return result.get('value')

    def change(self, path, name, value):
        def change():
            fields = self.adapter if path == '/org/bluez/hci0' else self.devices[path]
            fields[name] = value
            self.bus.emit_signal(None, path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                GLib.Variant('(sa{sv}as)', ('org.bluez.Adapter1' if path == '/org/bluez/hci0' else 'org.bluez.Device1',
                                          {name: variant(value)}, [])))
        self.command(change)

    def release_deferred(self):
        def release():
            for path, interface, key, value, invocation in self.deferred:
                fields = self.adapter if path == '/org/bluez/hci0' else self.devices[path]
                fields[key] = value
                self.bus.emit_signal(None, path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                    GLib.Variant('(sa{sv}as)', (interface, {key: variant(value)}, [])))
                invocation.return_value(None)
            self.deferred.clear()
        self.command(release)

    def invoke(self, method='RequestConfirmation', signature='(ou)', values=(PHONE, 12345), rogue=False):
        result = Call()
        def begin():
            if self.agent is None:
                raise AssertionError('Fixture has no registered pairing agent')
            def completed(connection, response):
                try:
                    result.result = connection.call_finish(response).unpack()
                except Exception as error:
                    result.error = str(error)
                result.event.set()
            connection = self.rogue if rogue else self.bus
            if method == 'Release' and not rogue:
                self.default = False  # Explicit fake daemon unregisters before Release.
            self.agent_calls.append(method)
            connection.call(self.agent[0], self.agent[1], 'org.bluez.Agent1', method, GLib.Variant(signature, values),
                            None, Gio.DBusCallFlags.NONE, 2000, None, completed)
        self.command(begin)
        return result

    def release_name(self):
        return self.command(lambda: self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'ReleaseName', GLib.Variant('(s)', ('org.bluez',)), None, Gio.DBusCallFlags.NONE, 500, None))

    def has_owner(self, name):
        return self.command(lambda: self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'NameHasOwner', GLib.Variant('(s)', (name,)), None, Gio.DBusCallFlags.NONE, 500, None).unpack()[0])

    def _method(self, _, sender, path, interface, method, parameters, invocation):
        try:
            values = parameters.unpack()
            self.methods.append((path, interface, method))
            if interface == 'org.bluez.AgentManager1':
                if method == 'RegisterAgent':
                    if values[1] != 'DisplayYesNo':
                        raise ValueError('Unexpected fixture agent capability')
                    self.agent = (sender, values[0])
                    if self.auto_pairable_agent and not self.adapter['Pairable']:
                        self.adapter['Pairable'] = True
                        self.bus.emit_signal(None, '/org/bluez/hci0', 'org.freedesktop.DBus.Properties',
                            'PropertiesChanged', GLib.Variant('(sa{sv}as)',
                            ('org.bluez.Adapter1', {'Pairable': variant(True)}, [])))
                elif method == 'RequestDefaultAgent':
                    if 'DefaultAgent' in self.fail:
                        raise ValueError('Fixture refused default agent')
                    self.default = True
                elif method == 'UnregisterAgent':
                    if 'UnregisterAgent' in self.fail:
                        raise ValueError('Fixture refused unregister')
                    self.default = False
                invocation.return_value(None)
            elif method == 'GetAll':
                fields = self.adapter if path == '/org/bluez/hci0' else self.devices[path]
                if path in self.devices and self.drop_on_device_read:
                    self.drop_on_device_read = False
                    self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'ReleaseName',
                                      GLib.Variant('(s)', ('org.bluez',)), None, Gio.DBusCallFlags.NONE, 500, None)
                invocation.return_value(GLib.Variant('(a{sv})', ({key: variant(value) for key, value in fields.items()},)))
            elif method == 'Set':
                fields = self.adapter if path == '/org/bluez/hci0' else self.devices[path]
                key, value = values[1], values[2]
                self.writes.append((path, key, value))
                if key in self.fail or (key, value) in self.fail:
                    raise ValueError('Fixture refused ' + key)
                if (key, value) in self.defer:
                    self.deferred.append((path, values[0], key, value, invocation))
                    self.deferred_ready.set()
                    return
                if key not in self.ignore and (key, value) not in self.ignore:
                    fields[key] = value
                    self.bus.emit_signal(None, path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                        GLib.Variant('(sa{sv}as)', (values[0], {key: variant(value)}, [])))
                invocation.return_value(None)
            elif method == 'CancelPairing':
                invocation.return_value(None)
            else:
                raise ValueError('Unknown fixture method')
        except Exception as error:
            invocation.return_dbus_error('org.bluez.Error.Failed', str(error))

    def _run(self):
        context = GLib.MainContext.new()
        context.push_thread_default()
        connections, exports = [], []
        try:
            for _ in range(2):
                connection = Gio.DBusConnection.new_for_address_sync(self.address,
                    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, self.cancellable)
                connection.set_exit_on_close(False)
                connections.append(connection)
            self.bus, self.rogue = connections
            self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
                              GLib.Variant('(su)', ('org.bluez', 4)), None, Gio.DBusCallFlags.NONE, 500, None)
            for path, xml in [('/org/bluez', _MANAGER), ('/org/bluez/hci0', _PROPERTIES),
                              *[(path, _PROPERTIES) for path in self.devices], *[(path, _DEVICE) for path in self.devices]]:
                info = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]
                exports.append(self.bus.register_object(path, info, self._method, None, None))
            self.ready.set()
            while not self.stop.is_set():
                for _ in range(64):
                    if not context.pending():
                        break
                    context.iteration(False)
                try:
                    function, result, done = self.commands.get(timeout=.002)
                except queue.Empty:
                    continue
                try:
                    result['value'] = function()
                except Exception as error:
                    result['error'] = error
                done.set()
        except Exception as error:
            self.failure = error
            self.ready.set()
        finally:
            if connections:
                for export in exports:
                    connections[0].unregister_object(export)
            for connection in connections:
                connection.close_sync(None)
            context.pop_thread_default()

    def close(self):
        self.stop.set()
        self.cancellable.cancel()
        self.thread.join(2)
        if self.thread.is_alive():
            raise AssertionError('Owned fake BlueZ worker did not shut down')


class PairingTests(unittest.TestCase):
    def setUp(self):
        warnings.filterwarnings('ignore', category=DeprecationWarning, module=r'gi\.events')
        self.sessions = []
        self.private = PrivateBus()
        self.addCleanup(self.private.close)
        self.environment = mock.patch.dict(os.environ, {'DBUS_SYSTEM_BUS_ADDRESS': self.private.address})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.bluez = FakeBluez(self.private.address)
        self.addCleanup(self.bluez.close)
        self.addCleanup(self._close_sessions)

    def _close_sessions(self):
        for session in self.sessions:
            session.close()
        for session in self.sessions:
            if session._worker is not None:
                session._worker.join(3)
                self.assertFalse(session._worker.is_alive(), 'Actual pairing worker exceeded cleanup bound')

    def session(self, adapter='hci0'):
        value = pairing.PairingSession(adapter)
        self.sessions.append(value)
        return value

    def phase(self, session, phase, timeout=2):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            value = session.snapshot()
            if value['phase'] == phase:
                return value
            if phase not in ('error', 'idle') and value['phase'] == 'error':
                self.fail('Pairing unexpectedly failed: ' + value['error'])
            time.sleep(.005)
        self.fail(f'Expected pairing {phase}; got {session.snapshot()}')

    def begin(self, session):
        value = session.start()
        self.assertNotEqual(value['phase'], 'paired')
        return self.phase(session, 'discoverable')

    def prompt(self, session, method='RequestConfirmation', signature='(ou)', values=(PHONE, 12345)):
        call = self.bluez.invoke(method, signature, values)
        value = self.phase(session, 'confirmation')
        self.assertFalse(call.event.is_set(), 'Agent automatically accepted an unanswered confirmation')
        return value, call

    def no_trust(self):
        self.assertFalse(any(key == 'Trusted' for _, key, _ in self.bluez.writes))

    def test_constructor_is_inert_snapshot_shape_and_validation(self):
        session = self.session()
        value = session.snapshot()
        self.assertEqual(set(value), {'supported', 'phase', 'session', 'expiresInSeconds', 'adapterName', 'prompt', 'pairedAddress', 'error'})
        self.assertEqual(value['phase'], 'idle')
        self.assertEqual(value['session'], '')
        self.assertIsNone(self.bluez.agent)
        self.assertEqual(self.bluez.methods, [])
        self.assertEqual(self.bluez.writes, [])
        for adapter in ('', 'hci0/other', 'org.bluez', 'hci10000', '../hci0', 0):
            with self.assertRaises(ValueError):
                pairing.PairingSession(adapter)
        session.close()
        with self.assertRaises(ValueError):
            session.start()

    def test_default_pairable_baseline_remains_untouched_and_cancel_cleans(self):
        session = self.session()
        baseline = dict(self.bluez.adapter)
        active = self.begin(session)
        self.assertRegex(active['session'], r'^[0-9a-f]{32}$')
        self.assertEqual(active['adapterName'], 'Headunit fixture')
        self.assertGreater(active['expiresInSeconds'], 0)
        self.assertTrue(self.bluez.default)
        self.assertFalse(any(key in ('Pairable', 'PairableTimeout', 'Powered', 'Alias') for _, key, _ in self.bluez.writes))
        old_owner = self.bluez.agent[0]
        began = time.monotonic(); session.cancel(active['session'])
        self.assertLess(time.monotonic() - began, .03)
        self.phase(session, 'idle')
        self.assertEqual(self.bluez.adapter, baseline)
        self.assertFalse(self.bluez.default)
        self.assertFalse(self.bluez.has_owner(old_owner))
        self.no_trust()

    def test_false_pairability_finite_timeout_precedes_enable_and_restores(self):
        self.bluez.adapter.update(Pairable=False, PairableTimeout=37)
        baseline = dict(self.bluez.adapter)
        session = self.session(); active = self.begin(session)
        writes = [(key, value) for _, key, value in self.bluez.writes]
        timeout_index = next(i for i, item in enumerate(writes) if item[0] == 'PairableTimeout')
        self.assertLess(timeout_index, writes.index(('Pairable', True)))
        self.assertTrue(1 <= writes[timeout_index][1] <= 120)
        session.cancel(active['session']); self.phase(session, 'idle')
        self.assertEqual(self.bluez.adapter, baseline)
        self.no_trust()

    def test_first_agent_auto_pairable_uses_owned_finite_window_and_restores(self):
        self.bluez.adapter.update(Pairable=False, PairableTimeout=37)
        self.bluez.auto_pairable_agent = True
        baseline = dict(self.bluez.adapter)
        session = self.session(); active = self.begin(session)
        self.assertTrue(self.bluez.adapter['Pairable'])
        self.assertTrue(1 <= self.bluez.adapter['PairableTimeout'] <= 120)
        methods = self.bluez.methods
        register = methods.index(('/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent'))
        owned_sets = [i for i, item in enumerate(methods[:register])
                      if item == ('/org/bluez/hci0', 'org.freedesktop.DBus.Properties', 'Set')]
        self.assertEqual(len(owned_sets), 3)
        session.cancel(active['session']); self.phase(session, 'idle')
        self.assertEqual(self.bluez.adapter, baseline)
        self.assertFalse(self.bluez.default)
        self.no_trust()

    def test_explicit_approval_fresh_pair_then_trust_and_verified_completion(self):
        session = self.session(); self.begin(session)
        value, call = self.prompt(session)
        self.assertEqual(value['prompt']['passkey'], '012345')
        self.assertEqual(value['prompt']['address'], 'AA:BB:CC:DD:EE:FF')
        copy = session.snapshot(); copy['prompt']['deviceName'] = 'changed copy'
        self.assertEqual(session.snapshot()['prompt']['deviceName'], 'Fixture iPhone')
        session.confirm(value['session'], value['prompt']['id'], True)
        self.assertIsNone(call.wait().error)
        self.phase(session, 'discoverable')
        self.no_trust()
        self.assertNotEqual(session.snapshot()['phase'], 'paired')
        self.bluez.change(PHONE, 'Paired', True)
        paired = self.phase(session, 'paired')
        self.assertEqual(paired['pairedAddress'], 'AA:BB:CC:DD:EE:FF')
        self.assertTrue(self.bluez.devices[PHONE]['Trusted'])
        self.assertEqual([(path, key, value) for path, key, value in self.bluez.writes if key == 'Trusted'], [(PHONE, 'Trusted', True)])
        self.assertFalse(self.bluez.adapter['Discoverable'])
        self.assertFalse(self.bluez.has_owner(self.bluez.agent[0]))

    def test_authorization_requires_explicit_prompt_and_service_whitelist(self):
        session = self.session(); self.begin(session)
        denied = self.bluez.invoke('AuthorizeService', '(os)', (PHONE, '00000000-deca-fade-deca-deafdecacafe')).wait()
        self.assertIn('Rejected', denied.error)
        value, call = self.prompt(session, 'RequestAuthorization', '(o)', (PHONE,))
        self.assertEqual(value['prompt']['passkey'], '')
        session.confirm(value['session'], value['prompt']['id'], True)
        self.assertIsNone(call.wait().error)
        for uuid in ('00000000-deca-fade-deca-deafdecacafe', '00000000-deca-fade-deca-deafdecacaff'):
            allowed = self.bluez.invoke('AuthorizeService', '(os)', (PHONE, uuid)).wait()
            self.assertIsNone(allowed.error)
        rejected = self.bluez.invoke('AuthorizeService', '(os)', (PHONE, '00000000-0000-0000-0000-000000000001')).wait()
        self.assertIn('Rejected', rejected.error)
        self.no_trust()

    def test_stale_prompt_rejection_denial_and_late_paired_signal_never_trust(self):
        session = self.session(); self.begin(session)
        value, call = self.prompt(session)
        for token, prompt in (('0' * 32, value['prompt']['id']), (value['session'], 'f' * 32)):
            with self.assertRaises(ValueError):
                session.confirm(token, prompt, True)
        with self.assertRaises(ValueError):
            session.confirm(value['session'], value['prompt']['id'], 1)
        session.confirm(value['session'], value['prompt']['id'], False)
        self.assertIn('Rejected', call.wait().error)
        self.phase(session, 'error')
        self.bluez.change(PHONE, 'Paired', True)
        self.assertEqual(session.snapshot()['pairedAddress'], '')
        self.no_trust()

    def test_wrong_sender_other_candidate_adapter_identity_and_prepaired_rejected(self):
        session = self.session(); self.begin(session)
        self.assertIn('Rejected', self.bluez.invoke(rogue=True).wait().error)
        self.assertIsNone(session.snapshot()['prompt'])
        self.assertIn('Rejected', self.bluez.invoke(values=(FOREIGN, 12345)).wait().error)
        self.bluez.devices[PHONE]['Paired'] = True
        self.assertIn('already paired', self.bluez.invoke().wait().error)
        self.bluez.devices[PHONE]['Paired'] = False
        self.bluez.devices[PHONE]['Adapter'] = '/org/bluez/hci1'
        self.assertIn('inconsistent', self.bluez.invoke().wait().error)
        self.bluez.devices[PHONE]['Adapter'] = '/org/bluez/hci0'
        value, pending = self.prompt(session)
        self.assertIn('pending', self.bluez.invoke(values=(OTHER, 1)).wait().error)
        session.confirm(value['session'], value['prompt']['id'], True); pending.wait()
        self.assertIn('Another device', self.bluez.invoke(values=(OTHER, 1)).wait().error)
        self.no_trust()

    def test_pin_entry_methods_unsupported_and_agent_cancel_clears_pending(self):
        session = self.session(); self.begin(session)
        for method, signature, values in (('RequestPinCode', '(o)', (PHONE,)), ('RequestPasskey', '(o)', (PHONE,)),
                                           ('DisplayPinCode', '(os)', (PHONE, '1234')), ('DisplayPasskey', '(ouq)', (PHONE, 12345, 0))):
            self.assertIn('unsupported', self.bluez.invoke(method, signature, values).wait().error)
        _, pending = self.prompt(session)
        self.assertIsNone(self.bluez.invoke('Cancel', '()', ()).wait().error)
        self.assertIn('Canceled', pending.wait().error)
        self.phase(session, 'error')
        self.assertIsNone(session.snapshot()['prompt'])
        self.no_trust()

    def test_fixed_timeout_and_close_are_nonblocking_and_clear_pending(self):
        with mock.patch.object(pairing, 'WINDOW_SECONDS', .2):
            session = self.session(); self.begin(session)
            value, pending = self.prompt(session)
            self.phase(session, 'error')
            self.assertIn('expired', session.snapshot()['error'])
            self.assertIn('Canceled', pending.wait().error)
            with self.assertRaises(ValueError):
                session.confirm(value['session'], value['prompt']['id'], True)
            self.assertFalse(self.bluez.adapter['Discoverable'])
            self.no_trust()
        other = self.session(); self.begin(other)
        _, pending = self.prompt(other)
        began = time.monotonic(); other.close()
        self.assertLess(time.monotonic() - began, .03)
        self.phase(other, 'idle')
        self.assertIn('Canceled', pending.wait().error)

    def test_owner_loss_and_owner_change_during_device_read_never_accept_or_trust(self):
        session = self.session(); self.begin(session)
        value, pending = self.prompt(session)
        self.bluez.command(lambda: setattr(self.bluez, 'drop_on_device_read', True))
        session.confirm(value['session'], value['prompt']['id'], True)
        self.assertIn('Rejected', pending.wait().error)
        error = self.phase(session, 'error')
        self.assertIn('ownership changed', error['error'])
        self.no_trust()

    def test_owner_loss_with_unanswered_prompt_and_release_clear_invocations(self):
        session = self.session(); self.begin(session)
        _, pending = self.prompt(session)
        before = len(self.bluez.writes)
        self.bluez.release_name()
        self.assertIn('Canceled', pending.wait().error)
        self.phase(session, 'error')
        self.assertEqual(len(self.bluez.writes), before)
        self.assertIsNone(session.snapshot()['prompt'])
        self.assertGreater(self.bluez.adapter['DiscoverableTimeout'], 0)
        self.no_trust()

    def test_already_discoverable_is_refused_without_radio_or_agent_writes(self):
        self.bluez.adapter['Discoverable'] = True
        session = self.session(); session.start()
        self.assertIn('already discoverable', self.phase(session, 'error')['error'])
        self.assertEqual(self.bluez.writes, [])
        self.assertIsNone(self.bluez.agent)
        self.no_trust()

    def test_actual_stalled_authentication_expires_and_drains_connection(self):
        path = self.private.folder / 'stalled-auth'
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(path)); listener.listen(1); listener.settimeout(1)
        accepted, disconnected = threading.Event(), threading.Event()
        errors = []
        def stall():
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(1)
                    accepted.set()
                    # Read authentication but deliberately never respond.
                    while connection.recv(4096):
                        pass
                    disconnected.set()
            except Exception as error:
                errors.append(error)
        worker = threading.Thread(target=stall, name='explicit-stalled-auth')
        worker.start()
        try:
            with mock.patch.dict(os.environ, {'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=' + str(path)}), \
                 mock.patch.object(pairing, 'WINDOW_SECONDS', .15):
                session = self.session(); session.start()
                self.assertTrue(accepted.wait(.5))
                error = self.phase(session, 'error', timeout=1)
                self.assertIn('expired', error['error'])
                session.close(); session.wait_closed()
                self.assertTrue(disconnected.wait(.25), 'Retired worker retained its authentication connection')
                self.assertFalse(session._worker.is_alive())
                self.assertEqual(self.bluez.writes, [])
        finally:
            listener.close(); worker.join(1.5)
            self.assertFalse(worker.is_alive())
            if errors:
                raise errors[0]

    def test_power_on_is_explicit_persistent_and_failures_are_honest(self):
        self.bluez.adapter['Powered'] = False
        session = self.session(); active = self.begin(session)
        self.assertTrue(self.bluez.adapter['Powered'])
        session.cancel(active['session']); self.phase(session, 'idle')
        self.assertTrue(self.bluez.adapter['Powered'])
        self.assertEqual([(key, value) for _, key, value in self.bluez.writes if key == 'Powered'], [('Powered', True)])
        self.bluez.adapter['Powered'] = False
        self.bluez.fail.add('Powered')
        failed = self.session(); failed.start()
        self.assertIn('Powered', self.phase(failed, 'error')['error'])
        self.assertFalse(self.bluez.adapter['Discoverable'])
        self.no_trust()
        self.bluez.fail.clear(); self.bluez.ignore.add('Powered')
        ignored = self.session(); ignored.start()
        self.assertIn('did not turn on', self.phase(ignored, 'error')['error'])
        self.assertFalse(self.bluez.adapter['Discoverable'])

    def test_finite_timeout_readback_and_partial_setup_failure_cleanup(self):
        self.bluez.adapter['Pairable'] = False
        self.bluez.fail.add('PairableTimeout')
        session = self.session(); session.start()
        self.phase(session, 'error')
        self.assertFalse(self.bluez.adapter['Discoverable'])
        self.assertFalse(self.bluez.adapter['Pairable'])
        self.assertEqual(self.bluez.adapter['DiscoverableTimeout'], 180)
        self.bluez.fail.clear(); self.bluez.ignore.add('DiscoverableTimeout')
        failed = self.session(); failed.start()
        self.assertIn('did not retain', self.phase(failed, 'error')['error'])
        self.assertFalse(any(key == 'Discoverable' and value is True for _, key, value in self.bluez.writes))
        self.no_trust()

    def test_foreign_changes_survive_cleanup_and_cleanup_failure_is_visible(self):
        session = self.session(); active = self.begin(session)
        self.bluez.change('/org/bluez/hci0', 'DiscoverableTimeout', 17)
        session.cancel(active['session']); self.phase(session, 'idle')
        self.assertEqual(self.bluez.adapter['DiscoverableTimeout'], 17)
        self.bluez.fail.add(('Discoverable', False))
        failed = self.session(); active = self.begin(failed)
        failed.cancel(active['session'])
        error = self.phase(failed, 'error')
        self.assertIn('Discoverable', error['error'])
        self.assertTrue(self.bluez.adapter['Discoverable'])
        self.assertGreater(self.bluez.adapter['DiscoverableTimeout'], 0)
        self.assertLessEqual(len(error['error']), 256)

    def test_cleanup_readback_detects_silently_ignored_restore(self):
        self.bluez.ignore.add(('Discoverable', False))
        session = self.session(); active = self.begin(session)
        session.cancel(active['session'])
        value = self.phase(session, 'error')
        self.assertIn('did not retain restored Discoverable', value['error'])
        self.assertTrue(self.bluez.adapter['Discoverable'])
        self.assertGreater(self.bluez.adapter['DiscoverableTimeout'], 0)

    def test_wait_closed_retains_worker_until_actual_deferred_cleanup_completion(self):
        self.bluez.defer.add(('Discoverable', False))
        session = self.session(); self.begin(session)
        done = threading.Event()
        session.close()
        self.assertTrue(self.bluez.deferred_ready.wait(.5))
        waiter = threading.Thread(target=lambda: (session.wait_closed(), done.set()), name='explicit-retirement-wait')
        waiter.start()
        try:
            self.assertFalse(done.wait(.025))
            self.assertEqual(session.snapshot()['phase'], 'stopping')
            self.assertTrue(self.bluez.adapter['Discoverable'])
            self.bluez.release_deferred()
            self.assertTrue(done.wait(1))
            self.assertEqual(session.snapshot()['phase'], 'idle')
            self.assertFalse(self.bluez.adapter['Discoverable'])
            self.assertFalse(self.bluez.has_owner(self.bluez.agent[0]))
        finally:
            self.bluez.release_deferred()
            waiter.join(1)
            self.assertFalse(waiter.is_alive())

    def test_trust_failure_reports_observed_pair_without_success(self):
        self.bluez.fail.add('Trusted')
        session = self.session(); self.begin(session)
        value, pending = self.prompt(session)
        session.confirm(value['session'], value['prompt']['id'], True)
        self.assertIsNone(pending.wait().error)
        self.bluez.change(PHONE, 'Paired', True)
        value = self.phase(session, 'error')
        self.assertEqual(value['pairedAddress'], 'AA:BB:CC:DD:EE:FF')
        self.assertIn('Trusted', value['error'])
        self.assertFalse(self.bluez.devices[PHONE]['Trusted'])

    def test_silently_ignored_trust_is_not_pairing_success(self):
        self.bluez.ignore.add('Trusted')
        session = self.session(); self.begin(session)
        value, pending = self.prompt(session)
        session.confirm(value['session'], value['prompt']['id'], True)
        self.assertIsNone(pending.wait().error)
        self.bluez.change(PHONE, 'Paired', True)
        value = self.phase(session, 'error')
        self.assertEqual(value['pairedAddress'], 'AA:BB:CC:DD:EE:FF')
        self.assertIn('permission was not retained', value['error'])
        self.assertFalse(self.bluez.devices[PHONE]['Trusted'])


if __name__ == '__main__':
    unittest.main()
