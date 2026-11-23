# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Explicit incoming BlueZ SSP pairing, with one independently owned worker.

Uses BlueZ Agent1/Adapter1 APIs. Construction is radio/agent inert. An explicit
start may turn the selected adapter on; that intentional power change persists.
Discoverability and any pairability enabled by us are temporary. Baseline
Pairable=true is left alone, including its existing timeout. No scanning,
adapter renaming, arbitrary device selection or auto-approval. A freshly bonded,
explicitly approved candidate is trusted for future authorized reconnections.
"""
from collections import deque
import copy
import math
import re
import secrets
import threading
import time


WINDOW_SECONDS = 120
_ACTIVE = ('starting', 'discoverable', 'confirmation', 'stopping')
_ADAPTER = re.compile(r'hci[0-9]{1,4}\Z')
_ADDRESS = re.compile(r'(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\Z')
_SERVICES = {'00000000-deca-fade-deca-deafdecacafe', '00000000-deca-fade-deca-deafdecacaff'} | {
    f'0000{number}-0000-1000-8000-00805f9b34fb'
    for number in ('1108', '110a', '110b', '110c', '110d', '110e', '110f', '1112', '111e', '111f')
}
_AGENT_XML = '''<node><interface name="org.bluez.Agent1">
<method name="Release"/>
<method name="RequestPinCode"><arg type="o" direction="in"/><arg type="s" direction="out"/></method>
<method name="DisplayPinCode"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
<method name="RequestPasskey"><arg type="o" direction="in"/><arg type="u" direction="out"/></method>
<method name="DisplayPasskey"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="q" direction="in"/></method>
<method name="RequestConfirmation"><arg type="o" direction="in"/><arg type="u" direction="in"/></method>
<method name="RequestAuthorization"><arg type="o" direction="in"/></method>
<method name="AuthorizeService"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
<method name="Cancel"/>
</interface></node>'''


def _check(condition, message):
    if not condition:
        raise ValueError(message)


def _text(value, limit=128):
    return ''.join(c for c in str(value) if ord(c) >= 32 and ord(c) != 127)[:limit]


class PairingSession:
    def __init__(self, adapter='hci0'):
        _check(type(adapter) is str and _ADAPTER.fullmatch(adapter), 'Invalid pairing adapter; use hciN')
        self.adapter = adapter
        self._lock = threading.Lock()
        self._worker = None
        self._cancellable = None
        self._closed = False
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._commands = deque()
        self._deadline = 0
        self._confirmation_queued = False
        self._state = self._initial('idle', '')

    @staticmethod
    def _initial(phase, session):
        return {'supported': True, 'phase': phase, 'session': session, 'expiresInSeconds': 0,
                'adapterName': '', 'prompt': None, 'pairedAddress': '', 'error': ''}

    def snapshot(self):
        with self._lock:
            value = copy.deepcopy(self._state)
            if value['phase'] in _ACTIVE and value['phase'] != 'stopping':
                value['expiresInSeconds'] = max(0, min(120, math.ceil(self._deadline - time.monotonic())))
                if not value['expiresInSeconds']:
                    value['prompt'] = None
            return value

    def start(self):
        with self._lock:
            _check(not self._closed, 'Pairing control is closed')
            _check(not self._worker or not self._worker.is_alive(), 'A pairing session or its cleanup is already running')
            session = secrets.token_hex(16)
            self._state = self._initial('starting', session)
            self._deadline = time.monotonic() + WINDOW_SECONDS
            self._stop = threading.Event()
            self._wake.clear()
            self._commands.clear()
            self._confirmation_queued = False
            # Non-daemon: process shutdown waits for bounded radio cleanup.
            self._worker = threading.Thread(target=self._run, args=(session, self._deadline), name='headunit-pairing')
            try:
                self._worker.start()
            except Exception as error:
                self._worker = None
                self._state.update(phase='error', error=_text(error, 256))
                raise ValueError('Cannot start Bluetooth pairing worker') from error
        return self.snapshot()

    def _request_stop(self, session=None, close=False):
        with self._lock:
            if close:
                self._closed = True
            else:
                _check(type(session) is str and session == self._state['session'] and self._state['phase'] in _ACTIVE,
                       'Pairing session is stale or no longer active')
            if self._worker and self._worker.is_alive():
                self._state['phase'] = 'stopping'
                self._state['prompt'] = None
                self._stop.set()
            cancellable = self._cancellable
        self._wake.set()
        if cancellable is not None:
            cancellable.cancel()

    def cancel(self, session):
        self._request_stop(session)
        return self.snapshot()

    def confirm(self, session, prompt, accepted):
        _check(type(accepted) is bool, 'Pairing confirmation must be boolean')
        with self._lock:
            value = self._state
            _check(type(session) is str and session == value['session'] and value['phase'] == 'confirmation' and
                   value['prompt'] is not None and type(prompt) is str and prompt == value['prompt']['id'] and
                   time.monotonic() < self._deadline and not self._confirmation_queued,
                   'Pairing confirmation is stale or already answered')
            self._commands.append((prompt, accepted))
            self._confirmation_queued = True
        self._wake.set()
        return self.snapshot()

    def close(self):
        """Request asynchronous cleanup; no main-thread bus calls or joins."""
        self._request_stop(close=True)

    def wait_closed(self):
        """Final shutdown only: retain manager ownership until actual retirement.

        Call close() first. Never call this on a UI/request callback.
        """
        with self._lock:
            worker = self._worker
        if worker is not None:
            _check(worker is not threading.current_thread(), 'Pairing worker cannot join itself')
            worker.join()

    def _publish(self, **fields):
        with self._lock:
            # A public cancellation cannot be undone by a late startup reply.
            if self._stop.is_set() and fields.get('phase') in ('discoverable', 'confirmation'):
                return
            self._state.update(fields)

    def _run(self, session, deadline):
        try:
            from gi.repository import Gio, GLib
            engine = _Agent(self, Gio, GLib, session, deadline)
            engine.run()
        except Exception as error:
            self._publish(phase='error', prompt=None, expiresInSeconds=0, error=_text(error, 256))
        finally:
            with self._lock:
                self._cancellable = None


class _Agent:
    def __init__(self, public, Gio, GLib, session, deadline):
        self.public, self.Gio, self.GLib = public, Gio, GLib
        self.session, self.deadline = session, deadline
        self.path = '/org/bluez/' + public.adapter
        self.agent_path = '/org/headunit/pairing/' + session
        self.context = GLib.MainContext.new()
        self.bus = None
        self.owner = ''
        self.registration = 0
        self.subscriptions = []
        self.registered = False
        self.writes = []
        self.foreign = set()
        self.pending = None
        self.candidate = None
        self.confirmed = False
        self.error = ''
        self.paired = ''
        self.observed_paired = ''
        self.cancellable = Gio.Cancellable.new()
        with public._lock:
            public._cancellable = self.cancellable
            if public._stop.is_set():
                self.cancellable.cancel()

    def _due(self):
        _check(not self.public._stop.is_set(), 'Pairing was cancelled')
        _check(time.monotonic() < self.deadline, 'Pairing window expired')

    def _call(self, destination, path, interface, method, signature=None, values=(), cleanup=False):
        if not cleanup:
            self._due()
        try:
            value = self.bus.call_sync(destination, path, interface, method,
                                       self.GLib.Variant(signature, values) if signature else None, None,
                                       self.Gio.DBusCallFlags.NONE, 250 if cleanup else
                                       max(1, min(750, math.ceil((self.deadline - time.monotonic()) * 1000))),
                                       None if cleanup else self.cancellable)
        except Exception:
            if not cleanup:
                self._due()
            raise
        if not cleanup:
            self._due()
        return value.unpack()

    def _current_owner(self, cleanup=False):
        return self._call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                          'GetNameOwner', '(s)', ('org.bluez',), cleanup)[0]

    def _guard_owner(self, cleanup=False):
        try:
            _check(self.owner and self._current_owner(cleanup) == self.owner, 'Bluetooth daemon ownership changed')
        except Exception as error:
            if not cleanup:
                self._due()
            raise ValueError('Bluetooth daemon is unavailable or ownership changed') from error

    def _connect(self, address):
        # Unlike the synchronous constructor, authentication cannot stall this
        # worker's deadline processing. Completion is kept on our own context.
        completed = []
        def connected(_, result):
            try:
                completed.append((self.Gio.DBusConnection.new_for_address_finish(result), None))
            except Exception as error:
                completed.append((None, error))
        self.Gio.DBusConnection.new_for_address(address,
            self.Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | self.Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None, self.cancellable, connected)
        failure = None
        while not completed:
            for _ in range(64):
                if not self.context.pending():
                    break
                self.context.iteration(False)
            if failure is None:
                try:
                    self._due()
                except Exception as error:
                    failure = error
                    self.cancellable.cancel()
                    self.public._publish(phase='stopping', prompt=None, expiresInSeconds=0)
            if not completed:
                self.public._wake.wait(.005)
                self.public._wake.clear()
        self.bus, error = completed[0]
        if self.bus is not None:
            self.bus.set_exit_on_close(False)
        if failure is not None:
            raise failure
        if error is not None:
            raise error

    def _properties(self, path, interface, cleanup=False):
        self._guard_owner(cleanup)
        values = self._call(self.owner, path, 'org.freedesktop.DBus.Properties', 'GetAll', '(s)', (interface,), cleanup)[0]
        self._guard_owner(cleanup)
        return values

    def _set(self, name, value, previous):
        current = self._properties(self.path, 'org.bluez.Adapter1')
        _check(name in current and type(current[name]) is type(previous) and current[name] == previous,
               'Bluetooth adapter changed before pairing could take ownership')
        if value is True:
            _check(current.get('Powered') is True, 'Bluetooth adapter is not powered')
            required = (name + 'Timeout',)
            if name == 'Discoverable' and any(item[0] == 'PairableTimeout' for item in self.writes):
                required += ('PairableTimeout',)
            for timeout_name in required:
                expected = next((item[2] for item in self.writes if item[0] == timeout_name), None)
                _check(type(expected) is int and 1 <= expected <= 120 and type(current.get(timeout_name)) is int and
                       current[timeout_name] == expected, 'Finite Bluetooth timeout did not read back before enabling pairing')
            _check(current.get('Discoverable') is False if name == 'Pairable' else current.get('Pairable') is True,
                   'Bluetooth pairing policy changed before enabling discovery')
        self.writes.append((name, previous, value))  # Includes an ambiguous Set failure for safe cleanup.
        self._call(self.owner, self.path, 'org.freedesktop.DBus.Properties', 'Set', '(ssv)',
                   ('org.bluez.Adapter1', name, self.GLib.Variant('b' if type(value) is bool else 'u', value)))
        readback = self._properties(self.path, 'org.bluez.Adapter1')
        _check(type(readback.get(name)) is type(value) and readback[name] == value,
               'Bluetooth did not retain its requested ' + name + ' setting')

    def _stop(self, error=''):
        if error and not self.error:
            self.error = _text(error, 256)
        self.public._stop.set()
        self.public._wake.set()
        self.public._publish(phase='stopping', prompt=None, expiresInSeconds=0)

    def _device(self, path, allow_paired=False, cleanup=False):
        prefix = self.path + '/dev_'
        _check(type(path) is str and path.startswith(prefix), 'Pairing device belongs to a different adapter')
        address = path[len(prefix):].replace('_', ':')
        _check(_ADDRESS.fullmatch(address) and path == prefix + address.replace(':', '_'), 'Invalid pairing device path')
        values = self._properties(path, 'org.bluez.Device1', cleanup)
        actual = values.get('Address')
        _check(type(actual) is str and _ADDRESS.fullmatch(actual) and actual.upper() == address.upper() and
               values.get('Adapter') == self.path and type(values.get('Paired')) is bool,
               'Pairing device identity is inconsistent')
        _check(allow_paired or values['Paired'] is False, 'This device is already paired')
        _check(self.candidate is None or self.candidate['path'] == path, 'Another device already owns this pairing session')
        return {'path': path, 'address': actual.upper(), 'name': _text(values.get('Alias', values.get('Name', 'Phone'))),
                'paired': values['Paired'], 'trusted': values.get('Trusted')}

    def _method(self, _, sender, __, ___, method, parameters, invocation):
        try:
            _check(sender == self.owner, 'Pairing agent caller is not the Bluetooth daemon')
            self._guard_owner()
            values = parameters.unpack()
            if method == 'Release':
                self.registered = False
                invocation.return_value(None)
                self._stop('Bluetooth pairing agent was released')
            elif method == 'Cancel':
                invocation.return_value(None)
                self._stop('Bluetooth pairing was cancelled by the phone')
            elif method in ('RequestConfirmation', 'RequestAuthorization'):
                _check(self.pending is None, 'A pairing confirmation is already pending')
                device = self._device(values[0])
                passkey = ''
                if method == 'RequestConfirmation':
                    _check(type(values[1]) is int and 0 <= values[1] <= 999999, 'Invalid pairing comparison code')
                    passkey = f'{values[1]:06d}'
                self.candidate = device
                prompt = {'id': secrets.token_hex(16), 'deviceName': device['name'], 'address': device['address'], 'passkey': passkey}
                self.pending = (invocation, prompt['id'])
                with self.public._lock:
                    self.public._confirmation_queued = False
                self.public._publish(phase='confirmation', prompt=prompt)
            elif method == 'AuthorizeService':
                device = self._device(values[0], allow_paired=True)
                _check(self.confirmed and self.candidate and device['path'] == self.candidate['path'] and
                       type(values[1]) is str and values[1].lower() in _SERVICES,
                       'This service has no explicit pairing authorization')
                self._guard_owner()
                invocation.return_value(None)
            else:
                raise ValueError('PIN/passkey entry and display-only pairing are unsupported; use numeric comparison')
        except Exception as error:
            invocation.return_dbus_error('org.bluez.Error.Rejected', _text(error, 256))

    def _signal(self, _, sender, path, interface, member, parameters):
        if interface == 'org.freedesktop.DBus' and member == 'NameOwnerChanged':
            name, old, new = parameters.unpack()
            if name == 'org.bluez' and old == self.owner and new != self.owner:
                self._stop('Bluetooth daemon ownership changed')
            return
        if sender != self.owner:
            return
        changed_interface, changed, invalidated = parameters.unpack()
        if path == self.path and changed_interface == 'org.bluez.Adapter1':
            for name, _, value in self.writes:
                if name in invalidated or (name in changed and (type(changed[name]) is not type(value) or changed[name] != value)):
                    self.foreign.add(name)
            if changed.get('Powered') is False:
                self._stop('Bluetooth adapter was powered off')
        self.public._wake.set()

    def _setup(self):
        self._due()
        address = self.Gio.dbus_address_get_for_bus_sync(self.Gio.BusType.SYSTEM, self.cancellable)
        self._connect(address)
        self.owner = self._current_owner()
        properties = self._properties(self.path, 'org.bluez.Adapter1')
        for key in ('Powered', 'Discoverable', 'Pairable'):
            _check(type(properties.get(key)) is bool, 'Bluetooth adapter has an invalid ' + key + ' state')
        for key in ('DiscoverableTimeout', 'PairableTimeout'):
            _check(type(properties.get(key)) is int and 0 <= properties[key] <= 0xffffffff,
                   'Bluetooth adapter has an invalid timeout')
        _check(not properties['Discoverable'], 'Bluetooth is already discoverable; close the other pairing window first')
        self.public._publish(adapterName=_text(properties.get('Alias', properties.get('Name', self.public.adapter))))
        if not properties['Powered']:
            self._guard_owner()
            self._call(self.owner, self.path, 'org.freedesktop.DBus.Properties', 'Set', '(ssv)',
                       ('org.bluez.Adapter1', 'Powered', self.GLib.Variant('b', True)))
            current = self._properties(self.path, 'org.bluez.Adapter1')
            _check(current.get('Powered') is True, 'Bluetooth did not turn on; check rfkill and adapter power')
        info = self.Gio.DBusNodeInfo.new_for_xml(_AGENT_XML).interfaces[0]
        self.registration = self.bus.register_object(self.agent_path, info, self._method, None, None)
        self.subscriptions.append(self.bus.signal_subscribe('org.freedesktop.DBus', 'org.freedesktop.DBus',
            'NameOwnerChanged', '/org/freedesktop/DBus', 'org.bluez', self.Gio.DBusSignalFlags.NONE, self._signal))
        self.subscriptions.append(self.bus.signal_subscribe(self.owner, 'org.freedesktop.DBus.Properties',
            'PropertiesChanged', None, None, self.Gio.DBusSignalFlags.NONE, self._signal))
        # With BlueZ AlwaysPairable=false, installing the first default agent
        # enables Pairable itself. Admit our finite timeout and owned enable
        # first, while still undiscoverable, so that daemon side effect cannot
        # race a stale baseline or open an unbounded pairing window.
        timeout = max(1, min(120, math.floor(self.deadline - time.monotonic())))
        self._set('DiscoverableTimeout', timeout, properties['DiscoverableTimeout'])
        if not properties['Pairable']:
            self._set('PairableTimeout', timeout, properties['PairableTimeout'])
            self._set('Pairable', True, False)
        self._guard_owner()
        self._call(self.owner, '/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent', '(os)',
                   (self.agent_path, 'DisplayYesNo'))
        self.registered = True
        self._call(self.owner, '/org/bluez', 'org.bluez.AgentManager1', 'RequestDefaultAgent', '(o)', (self.agent_path,))
        self._set('Discoverable', True, False)
        current = self._properties(self.path, 'org.bluez.Adapter1')
        _check(current.get('Powered') is True and current.get('Discoverable') is True and current.get('Pairable') is True,
               'Bluetooth did not enable the pairing window')
        self.public._publish(phase='discoverable')

    def _commands(self):
        with self.public._lock:
            commands = list(self.public._commands)
            self.public._commands.clear()
        for prompt, accepted in commands:
            self._due()
            _check(self.pending is not None and prompt == self.pending[1], 'Pairing confirmation expired')
            invocation, _ = self.pending
            self.pending = None
            if not accepted:
                invocation.return_dbus_error('org.bluez.Error.Rejected', 'Pairing was declined')
                self._stop('Pairing was declined')
                return
            try:
                self._device(self.candidate['path'])
                self._guard_owner()
                self._due()
            except Exception as error:
                invocation.return_dbus_error('org.bluez.Error.Rejected', _text(error, 256))
                raise
            self.confirmed = True
            invocation.return_value(None)
            self.public._publish(phase='discoverable', prompt=None)

    def _cleanup(self):
        self.public._publish(phase='stopping', prompt=None, expiresInSeconds=0)
        issues = []
        if self.pending is not None:
            invocation, _ = self.pending
            self.pending = None
            try:
                invocation.return_dbus_error('org.bluez.Error.Canceled', 'Pairing session ended')
            except Exception as error:
                issues.append(_text(error, 100))
        if self.bus is not None:
            try:
                self._guard_owner(True)
            except Exception as error:
                # Never restore properties or unregister on a new daemon.
                if self.owner and (self.writes or self.registered):
                    issues.append(_text(error, 100))
            else:
                if self.candidate is not None and not self.paired:
                    try:
                        candidate = self._device(self.candidate['path'], allow_paired=True, cleanup=True)
                        if not candidate['paired']:
                            self._call(self.owner, candidate['path'], 'org.bluez.Device1', 'CancelPairing', cleanup=True)
                    except Exception as error:
                        if 'DoesNotExist' not in str(error) and 'NotInProgress' not in str(error):
                            issues.append(_text(error, 100))
                for name, previous, value in reversed(self.writes):
                    try:
                        current = self._properties(self.path, 'org.bluez.Adapter1', True)
                        if name in self.foreign or type(current.get(name)) is not type(value) or current[name] != value:
                            continue
                        if name.endswith('Timeout') and current.get(name.removesuffix('Timeout')) is not False:
                            issues.append('An enabled adapter setting kept its finite timeout')
                            continue
                        self._call(self.owner, self.path, 'org.freedesktop.DBus.Properties', 'Set', '(ssv)',
                                   ('org.bluez.Adapter1', name, self.GLib.Variant('b' if type(previous) is bool else 'u', previous)), True)
                        restored = self._properties(self.path, 'org.bluez.Adapter1', True)
                        _check(type(restored.get(name)) is type(previous) and restored[name] == previous,
                               'Bluetooth cleanup did not retain restored ' + name)
                    except Exception as error:
                        issues.append(_text(error, 100))
                if self.registered:
                    try:
                        self._guard_owner(True)
                        self._call(self.owner, '/org/bluez', 'org.bluez.AgentManager1', 'UnregisterAgent',
                                   '(o)', (self.agent_path,), True)
                    except Exception as error:
                        issues.append(_text(error, 100))
            for subscription in self.subscriptions:
                try:
                    self.bus.signal_unsubscribe(subscription)
                except Exception as error:
                    issues.append(_text(error, 100))
            if self.registration:
                try:
                    self.bus.unregister_object(self.registration)
                except Exception as error:
                    issues.append(_text(error, 100))
            try:
                self.bus.close_sync(None)
            except Exception as error:
                issues.append(_text(error, 100))
        if issues:
            self.error = _text('; '.join(filter(None, (self.error, *issues))), 256)
        self.public._publish(phase='paired' if self.paired else ('error' if self.error else 'idle'),
                             pairedAddress=self.paired or self.observed_paired, prompt=None, expiresInSeconds=0, error=self.error)

    def run(self):
        self.context.push_thread_default()
        try:
            self._setup()
            next_poll = 0
            while not self.public._stop.is_set():
                self._due()
                for _ in range(64):
                    if not self.context.pending():
                        break
                    self.context.iteration(False)
                if self.public._stop.is_set():
                    break
                self._commands()
                if self.public._stop.is_set():
                    break
                now = time.monotonic()
                if now >= next_poll:
                    current = self._properties(self.path, 'org.bluez.Adapter1')
                    _check(current.get('Powered') is True and current.get('Discoverable') is True and current.get('Pairable') is True,
                           'Bluetooth pairing window was closed externally')
                    if self.confirmed and self.candidate is not None:
                        device = self._device(self.candidate['path'], allow_paired=True)
                        if device['paired']:
                            self.observed_paired = device['address']
                            _check(type(device['trusted']) is bool, 'Bluetooth did not report candidate trust state')
                            if not device['trusted']:
                                self._guard_owner()
                                self._call(self.owner, device['path'], 'org.freedesktop.DBus.Properties', 'Set', '(ssv)',
                                           ('org.bluez.Device1', 'Trusted', self.GLib.Variant('b', True)))
                            verified = self._device(device['path'], allow_paired=True)
                            _check(verified['paired'] and verified['trusted'] is True,
                                   'Phone paired, but Bluetooth reconnection permission was not retained')
                            self._guard_owner()
                            self.paired = device['address']
                            break
                    next_poll = now + .1
                self.public._wake.wait(.01)
                self.public._wake.clear()
        except Exception as error:
            if not self.public._stop.is_set():
                self.error = _text(error, 256)
        finally:
            try:
                self._cleanup()
            finally:
                self.context.pop_thread_default()
