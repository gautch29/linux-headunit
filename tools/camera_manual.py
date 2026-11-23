#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Opt-in manual camera requests. Native status is not proof of a fresh image."""
import errno
import fcntl
import itertools
import json
import os
from pathlib import Path
import re
import selectors
import socket
import stat
import struct
import sys
import threading
import time


_UINT64 = (1 << 64) - 1
_PHASES = ('idle', 'waiting', 'active', 'fault', 'stopped')
_STATUS = re.compile(rb'HCS1 ([1-9][0-9]{0,19}) ([1-9][0-9]{0,19}) ([1-9][0-9]{0,19}) (idle|waiting|active|fault|stopped)\n')
_REQUEST = re.compile(rb'on ([1-9][0-9]{0,19})\n')


def _check(condition, message):
    if not condition:
        raise ValueError(message)


def _error(value):
    return ''.join(c if 32 <= ord(c) <= 126 else '?' for c in str(value))[:128]


def _unique(pairs):
    result = {}
    for key, value in pairs:
        _check(key not in result, 'Duplicate camera configuration or command field')
        result[key] = value
    return result


def _json(raw):
    return json.loads(raw, object_pairs_hook=_unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite JSON value')))


def _fields(value, keys, message):
    _check(type(value) is dict and set(value) == set(keys), message)


def _integer(value, lower, upper, message):
    _check(type(value) is int and lower <= value <= upper, message)


def _absolute(value):
    _check(type(value) is str and value.startswith('/') and not value.endswith('/') and '\0' not in value and
           len(os.fsencode(value)) <= 4000 and not any(p in ('.', '..') for p in value.split('/')),
           'Camera endpoint requires a bounded absolute path without dot components')
    _check(bool(Path(value).name) and len(os.fsencode(Path(value).name)) <= 255,
           'Camera endpoint must name one bounded file')
    return value


def validate_configuration(value):
    """Validate data only. This never opens, creates or removes an endpoint."""
    _fields(value, ('schemaVersion', 'enabled', 'requestFile', 'ownerFile', 'statusFile', 'controlSocket', 'timing'),
            'Unknown or missing manual camera configuration field')
    _check(type(value['schemaVersion']) is int and value['schemaVersion'] == 1, 'Unsupported manual camera schema')
    _check(type(value['enabled']) is bool, 'Manual camera enabled must be boolean')
    for key in ('requestFile', 'ownerFile', 'statusFile', 'controlSocket'):
        _absolute(value[key])
    control = value['controlSocket']
    _check(all(32 <= ord(c) <= 126 for c in control) and len(control) <= 103 and
           os.path.normpath(control) == control and all(control.split('/')[1:]),
           'Camera control socket requires a canonical ASCII path of at most 103 bytes')
    paths = [value['requestFile'], value['ownerFile'], value['statusFile'], value['statusFile'] + '.owner',
             value['controlSocket'], value['controlSocket'] + '.lock']
    for path in paths:
        _absolute(path)
    _check(len(set(os.path.normpath(p) for p in paths)) == len(paths), 'Manual camera endpoints alias')
    timing = value['timing']
    _fields(timing, ('leaseMs', 'pollMs', 'statusMaximumAgeMs'), 'Unknown or missing manual camera timing field')
    _integer(timing['leaseMs'], 50, 1000, 'Manual camera lease is outside 50..1000 ms')
    _integer(timing['pollMs'], 10, 100, 'Manual camera poll is outside 10..100 ms')
    _integer(timing['statusMaximumAgeMs'], 50, 5000, 'Camera status age is outside 50..5000 ms')
    _check(timing['leaseMs'] >= 3 * timing['pollMs'] and timing['statusMaximumAgeMs'] >= 2 * timing['pollMs'],
           'Manual camera timing has insufficient renewal margin')
    return json.loads(json.dumps(value))


def _account(owner):
    result = (os.getuid(), os.getgid()) if owner is None else owner
    _check(type(result) in (tuple, list) and len(result) == 2 and
           all(type(v) is int and 0 <= v < (1 << 32) - 1 for v in result), 'Invalid camera owner account')
    return tuple(result)


def load_configuration(path, owner=None):
    """Read a bounded real JSON profile; validate-only callers create no files."""
    before = os.stat(path, follow_symlinks=False)
    allowed = {0, os.getuid(), _account(owner)[0]}
    _check(stat.S_ISREG(before.st_mode) and 0 < before.st_size <= 16384 and before.st_uid in allowed and
           not before.st_mode & 0o7022, 'Manual camera profile must be bounded owned regular JSON without unsafe write permissions')
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        opened = os.fstat(fd)
        _check(_stable(before, opened), 'Manual camera profile changed during open')
        raw = _read(fd, 16384)
        _check(len(raw) == opened.st_size and _stable(opened, os.fstat(fd)), 'Manual camera profile changed during read')
    finally:
        os.close(fd)
    return validate_configuration(_json(raw))


def _same(a, b):
    return (a.st_dev, a.st_ino) == (b.st_dev, b.st_ino)


def _content(a, b):
    return _same(a, b) and a.st_size == b.st_size and a.st_mtime_ns == b.st_mtime_ns


def _stable(a, b):
    return _content(a, b) and a.st_ctime_ns == b.st_ctime_ns


def _regular(info, uid, bound, links=(1,)):
    return stat.S_ISREG(info.st_mode) and info.st_uid == uid and stat.S_IMODE(info.st_mode) == 0o600 and \
        info.st_nlink in links and 0 <= info.st_size <= bound


def _read(fd, bound):
    result = bytearray()
    for _ in range(64):
        chunk = os.read(fd, bound + 1 - len(result))
        if not chunk:
            return bytes(result)
        result.extend(chunk)
        _check(len(result) <= bound, 'Camera endpoint exceeds its read bound')
    raise ValueError('Camera endpoint exceeded bounded read turns')


def _request(raw):
    if raw in (b'off\n', b'on\n'):
        return
    match = _REQUEST.fullmatch(raw)
    _check(match is not None and int(match[1]) <= _UINT64, 'Malformed manual camera request')


def _status(raw):
    match = _STATUS.fullmatch(raw)
    _check(match is not None, 'Malformed native camera status')
    fields = tuple(int(match[i]) for i in (1, 2, 3))
    _check(all(v <= _UINT64 for v in fields) and fields[0] <= fields[2], 'Invalid native camera status dates')
    return (*fields, match[4].decode('ascii'))


def unavailable_status_observation(state='not_configured'):
    """Fixed diagnostic schema; no intent, paths, errors or readiness authority."""
    _check(state in ('not_configured', 'disabled', 'suppressed', 'unavailable',
                     'reader_retired', 'manual_owner_unavailable'), 'Invalid camera observation state')
    return {'schemaVersion': 1, 'meaning': 'native_camera_owner_status_only',
            'eligible': False, 'availability': 'unavailable', 'state': state,
            'clockDomain': None, 'maximumAgeMs': None, 'lastObservation': None}


class _PrivatePath:
    def __init__(self, value, uid):
        _absolute(value)
        self.parent, self.name = os.path.dirname(value), os.path.basename(value)
        self.uid, self.fd = uid, -1
        before = os.stat(self.parent, follow_symlinks=False)
        _check(self.private(before), 'Camera parent must be real, owned and 0700')
        self.fd = os.open(self.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
        self.identity = os.fstat(self.fd)
        try:
            _check(self.private(self.identity) and _same(before, self.identity), 'Camera parent changed during open')
        except Exception:
            self.close()
            raise

    def private(self, info):
        return stat.S_ISDIR(info.st_mode) and info.st_uid == self.uid and stat.S_IMODE(info.st_mode) == 0o700

    def verify(self):
        named = os.stat(self.parent, follow_symlinks=False)
        opened = os.fstat(self.fd)
        _check(self.private(named) and self.private(opened) and _same(named, self.identity) and _same(opened, self.identity),
               'Camera parent generation changed')

    def inspect(self, name=None):
        try:
            return os.stat(name or self.name, dir_fd=self.fd, follow_symlinks=False)
        except FileNotFoundError:
            return None

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


def _distinct(paths):
    for i, a in enumerate(paths):
        for b in paths[:i]:
            _check(not (_same(a.identity, b.identity) and a.name == b.name), 'Camera endpoints alias the same path')
            left, right = a.inspect(), b.inspect()
            _check(left is None or right is None or not _same(left, right), 'Camera endpoints alias the same inode')


def _snapshot(path, bound, decode):
    for _ in range(4):
        path.verify()
        named = path.inspect()
        if named is None:
            return None
        _check(_regular(named, path.uid, bound), 'Camera endpoint must be owned single-link 0600 regular data')
        fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=path.fd)
        try:
            opened = os.fstat(fd)
            _check(_regular(opened, path.uid, bound, (0, 1)), 'Camera endpoint became unsafe during open')
            replaced = not _same(named, opened)
            _check(replaced or (_content(named, opened) and (_stable(named, opened) or opened.st_nlink == 0)),
                   'Camera endpoint was modified during open')
            raw = _read(fd, bound)
            after = os.fstat(fd)
            _check(len(raw) == opened.st_size and _regular(after, path.uid, bound, (0, 1)) and _content(opened, after) and
                   (_stable(opened, after) or after.st_nlink == 0), 'Camera endpoint changed during read')
        finally:
            os.close(fd)
        value = decode(raw)  # A newer inode never conceals already observed malformed bytes.
        path.verify()
        current = path.inspect()
        _check(current is not None and _regular(current, path.uid, bound), 'Camera endpoint disappeared or became unsafe')
        if replaced or after.st_nlink == 0 or not _same(after, current):
            _check(not _same(named if replaced else after, current), 'Camera endpoint changed without a safe replacement')
            continue
        _check(_stable(after, current), 'Camera endpoint changed after read')
        return raw, after, value
    raise ValueError('Camera endpoint exceeded bounded replacement retries')


class _Owner:
    def __init__(self, path, owner):
        self.path, self.fd, self.identity = path, -1, None
        for _ in range(64):
            path.verify()
            existing = path.inspect()
            if existing is not None:
                _check(_regular(existing, path.uid, 0), 'Camera owner must be empty owned single-link 0600 data')
                self.fd = os.open(path.name, os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=path.fd)
            else:
                try:
                    self.fd = os.open(path.name, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                                      0o600, dir_fd=path.fd)
                except FileExistsError:
                    continue
                try:
                    if os.geteuid() == 0:
                        os.fchown(self.fd, *owner)
                    os.fchmod(self.fd, 0o600)
                except Exception:
                    self.close()
                    raise
            try:
                self.identity = os.fstat(self.fd)
                _check(_regular(self.identity, path.uid, 0) and (existing is None or _same(existing, self.identity)),
                       'Camera owner changed during open')
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self.verify()
                return
            except Exception:
                self.close()
                raise
        raise ValueError('Camera owner creation exceeded retry bound')

    def verify(self):
        self.path.verify()
        named, opened = self.path.inspect(), os.fstat(self.fd)
        _check(named is not None and _regular(named, self.path.uid, 0) and _regular(opened, self.path.uid, 0) and
               _same(named, self.identity) and _same(opened, self.identity), 'Camera owner generation changed')

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


class _StatusReader:
    def __init__(self, path):
        self.path, self.last, self.last_now, self.live, self.retired = path, None, None, False, False
        self.lock = threading.Lock()

    def phase(self, clock, maximum_age_ms):
        return self.read(clock, maximum_age_ms)[0]

    def read(self, clock, maximum_age_ms):
        """One existing validation and an atomic copy of its original receipt."""
        with self.lock:
            phase = self._phase(clock, maximum_age_ms)
            return phase, self.last, self.live, self.retired

    def _phase(self, clock, maximum_age_ms):
        if self.retired:
            return 'unavailable'
        try:
            snapshot = _snapshot(self.path, 128, _status)
        except (OSError, ValueError):
            snapshot = None
        try:
            now = clock()
            if type(now) is not int or not 0 < now <= _UINT64 or (self.last_now is not None and now < self.last_now):
                self.retired, self.live = True, False
                return 'unavailable'
            self.last_now = now
            if snapshot is None:
                self.live = False
                return 'unavailable'
            value = snapshot[2]
            _check(value[2] <= now and now - value[2] < maximum_age_ms * 1000000, 'Camera status expired or is future-dated')
            if self.last is not None:
                if value == self.last:
                    return value[3] if self.live else 'unavailable'
                _check(value[0] >= self.last[0] and value[2] >= self.last[2] and
                       (value[0] != self.last[0] or value[1] > self.last[1]), 'Camera status was replayed')
            self.last, self.live = value, True
            return value[3]
        except (OSError, ValueError):
            self.live = False
            return 'unavailable'
        except Exception:
            self.retired, self.live = True, False
            return 'unavailable'


def _production_clock():
    _check(sys.platform.startswith('linux'), 'Enabled manual camera control requires Linux CLOCK_MONOTONIC')
    return lambda: time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def _peer_uid(client):
    if sys.platform.startswith('linux'):
        return struct.unpack('=3i', client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[1]
    if sys.platform == 'darwin':  # Filesystem/IPC fixtures only; no production clock claim.
        raw = client.getsockopt(0, socket.LOCAL_PEERCRED, 128)
        version, uid = struct.unpack_from('=II', raw)
        _check(version == 0, 'Unknown local peer credentials')
        return uid
    raise ValueError('Local peer credentials are unsupported')


class _ManualState:
    """One timed manual-request owner and independently responsive local IPC worker."""
    def __init__(self, path, owner=None, *, clock_ns=None):
        self.owner = _account(owner)
        _check(os.geteuid() == 0 or self.owner[0] == os.getuid(), 'Camera writer cannot impersonate another account')
        self.config = load_configuration(path, self.owner)
        self.clock = clock_ns
        self.clock_domain = 'injected_fixture' if clock_ns is not None else None
        self.lock = threading.Lock()
        self.paths, self.owners = [], []
        self.worker, self.server, self.selector = None, None, None
        self.wake_read = self.wake_write = -1
        self.socket_identity = None
        self.desired = self.dirty = self.closing = self.failed = False
        self.phase, self.error = 'unavailable', ''
        self.nonce = itertools.count()
        self.last_clock = None
        if not self.config['enabled']:
            return
        self.clock = clock_ns if clock_ns is not None else _production_clock()
        self.clock_domain = 'injected_fixture' if clock_ns is not None else 'linux_clock_monotonic'
        _check(callable(self.clock), 'Camera clock must be callable')
        try:
            control_parent = Path(self.config['controlSocket']).parent
            _check(control_parent.resolve(strict=True) == control_parent,
                   'Camera control socket requires real canonical parent ancestors')
            values = [self.config[k] for k in ('requestFile', 'ownerFile', 'statusFile', 'controlSocket')]
            values += [self.config['statusFile'] + '.owner', self.config['controlSocket'] + '.lock']
            for value in values:
                self.paths.append(_PrivatePath(value, self.owner[0]))
            _distinct(self.paths)
            self.request_path, owner_path, status_path, self.socket_path, _, socket_lock = self.paths
            self.owners.append(_Owner(owner_path, self.owner))
            self.owners.append(_Owner(socket_lock, self.owner))
            self.status = _StatusReader(status_path)
            self._publish(False)
            self._listen()
            self.wake_read, self.wake_write = os.pipe()
            os.set_blocking(self.wake_read, False)
            os.set_blocking(self.wake_write, False)
            self.worker = threading.Thread(target=self._run, name='headunit-manual-camera', daemon=True)
            self.worker.start()
        except Exception:
            self._cleanup()
            raise

    def _publish(self, enabled):
        owner, path = self.owners[0], self.request_path
        owner.verify()
        path.verify()
        _distinct(self.paths)
        previous = _snapshot(path, 24, _request)
        raw = b'off\n'
        if enabled:
            now = self.clock()
            _check(type(now) is int and 0 < now <= _UINT64 and (self.last_clock is None or now >= self.last_clock),
                   'Manual camera reference clock reversed')
            self.last_clock = now
            deadline = now + self.config['timing']['leaseMs'] * 1000000
            _check(deadline <= _UINT64, 'Manual camera deadline overflow')
            raw = f'on {deadline}\n'.encode('ascii')
        fd, temporary, identity = -1, None, None
        try:
            for _ in range(64):
                temporary = f'.headunit-camera-manual-{os.getpid()}-{next(self.nonce)}.tmp'
                if any(_same(path.identity, p.identity) and temporary == p.name for p in self.paths):
                    continue
                try:
                    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                                 0o600, dir_fd=path.fd)
                    break
                except FileExistsError:
                    continue
            _check(fd >= 0, 'Manual camera temporary creation exceeded retry bound')
            identity = os.fstat(fd)
            if os.geteuid() == 0:
                os.fchown(fd, *self.owner)
            os.fchmod(fd, 0o600)
            count = 0
            for _ in range(64):
                if count == len(raw):
                    break
                written = os.write(fd, raw[count:])
                _check(written > 0, 'Manual camera write failed')
                count += written
            _check(count == len(raw), 'Manual camera write exceeded bounded turns')
            after, named = os.fstat(fd), path.inspect(temporary)
            _check(_regular(after, path.uid, 24) and after.st_size == len(raw) and _same(identity, after) and
                   named is not None and _stable(after, named), 'Manual camera temporary changed')
            owner.verify()
            path.verify()
            _distinct(self.paths)
            current = _snapshot(path, 24, _request)
            _check((current is None) == (previous is None) and
                   (current is None or _stable(previous[1], current[1])), 'Manual request changed during publication')
            os.rename(temporary, path.name, src_dir_fd=path.fd, dst_dir_fd=path.fd)
            temporary = None
            owner.verify()
            path.verify()
            installed = path.inspect()
            _check(installed is not None and _regular(installed, path.uid, 24) and _same(after, installed),
                   'Manual camera installed request changed')
        finally:
            if fd >= 0:
                os.close(fd)
            if temporary is not None and identity is not None:
                current = path.inspect(temporary)
                if current is not None and _same(current, identity):
                    os.unlink(temporary, dir_fd=path.fd)

    def _retire_stale_socket(self, previous):
        # Both cooperating owner locks are held. Linux may retain the pathname
        # after a crash/reboot; never replace a bound or unsafe endpoint.
        path = self.socket_path
        _check(sys.platform.startswith('linux') and stat.S_ISSOCK(previous.st_mode) and
               previous.st_uid == path.uid and stat.S_IMODE(previous.st_mode) == 0o600 and
               previous.st_nlink == 1, 'Camera control socket already exists or is unsafe')
        with open('/proc/net/unix', 'rb') as inventory:
            raw = inventory.read(1024 * 1024 + 1)
        _check(len(raw) <= 1024 * 1024, 'Camera socket inventory exceeds bound')
        for line in raw.splitlines()[1:]:
            fields = line.split(None, 7)
            if len(fields) != 8:
                continue
            address = os.fsdecode(fields[7])
            # bind() uses a pinned /proc FD. The kernel retains that spelling,
            # even after its owner exits; conservatively protect any matching
            # leaf in a live proc-FD endpoint as well as the canonical pathname.
            proc_leaf = address.startswith('/proc/') and '/fd/' in address and address.endswith('/' + path.name)
            _check(address != os.path.join(path.parent, path.name) and not proc_leaf,
                   'Camera control socket already exists with a bound owner')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            try:
                probe.connect(f'/proc/self/fd/{path.fd}/{path.name}')
            except OSError as error:
                _check(error.errno == errno.ECONNREFUSED, 'Camera socket owner cannot be safely determined')
            else:
                raise ValueError('Camera control socket already exists with a live owner')
        self.owners[1].verify()
        path.verify()
        current = path.inspect()
        _check(current is not None and _stable(previous, current), 'Camera socket changed during recovery')
        os.unlink(path.name, dir_fd=path.fd)

    def _listen(self):
        path = self.socket_path
        self.owners[1].verify()
        path.verify()
        previous = path.inspect()
        if previous is not None:
            self._retire_stale_socket(previous)
        _check(path.inspect() is None, 'Camera control socket already exists')
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.setblocking(False)
        # Production binding stays relative to the pinned real directory.
        address = f'/proc/self/fd/{path.fd}/{path.name}' if sys.platform.startswith('linux') else os.path.join(path.parent, path.name)
        self.server.bind(address)
        self.socket_identity = path.inspect()
        _check(self.socket_identity is not None and stat.S_ISSOCK(self.socket_identity.st_mode), 'Camera socket bind changed')
        if sys.platform.startswith('linux'):
            # O_PATH pins the actual filesystem socket inode without opening a
            # socket/device for I/O. Its proc-FD handle works on Python versions
            # without nofollow chmod-at support and never follows the leaf name.
            metadata = os.open(path.name, os.O_PATH | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=path.fd)
            try:
                pinned = os.fstat(metadata)
                _check(stat.S_ISSOCK(pinned.st_mode) and _same(pinned, self.socket_identity), 'Camera socket metadata changed')
                handle = f'/proc/self/fd/{metadata}'
                os.chmod(handle, 0o600)
                if os.geteuid() == 0:
                    os.chown(handle, *self.owner)
            finally:
                os.close(metadata)
        else:
            os.chmod(path.name, 0o600, dir_fd=path.fd, follow_symlinks=False)
            if os.geteuid() == 0:
                os.chown(path.name, *self.owner, dir_fd=path.fd, follow_symlinks=False)
        path.verify()
        current = path.inspect()
        _check(current is not None and _same(current, self.socket_identity) and current.st_uid == path.uid and
               stat.S_IMODE(current.st_mode) == 0o600 and current.st_nlink == 1, 'Camera control socket became unsafe')
        self.socket_identity = current
        self.server.listen(2)

    def _wake(self):
        if self.wake_write >= 0:
            try:
                os.write(self.wake_write, b'w')
            except (BlockingIOError, OSError):
                pass

    def _verify_control(self):
        self.owners[1].verify()
        self.socket_path.verify()
        parent = Path(self.socket_path.parent)
        _check(parent.resolve(strict=True) == parent, 'Camera control socket parent ancestors changed')
        current = self.socket_path.inspect()
        _check(current is not None and self.socket_identity is not None and _same(current, self.socket_identity) and
               stat.S_ISSOCK(current.st_mode) and current.st_uid == self.owner[0] and
               stat.S_IMODE(current.st_mode) == 0o600 and current.st_nlink == 1, 'Camera control socket generation changed')

    def request(self, enabled):
        """Accept only manual intent. This does not acknowledge native visibility."""
        _check(type(enabled) is bool, 'Manual camera request must be boolean')
        with self.lock:
            _check(self.config['enabled'] and not self.closing and not self.failed, 'Manual camera control is unavailable')
            self.desired, self.dirty = enabled, True
        self._wake()
        return self._snapshot(False)

    def _snapshot(self, refresh, include_observation=False):
        if include_observation and hasattr(self, 'status'):
            # Keep validation/retirement and the paired copy atomic. The state
            # lock is always inside the reader lock, never held while joining.
            with self.status.lock:
                return self._snapshot_locked(refresh, include_observation)
        return self._snapshot_locked(refresh, include_observation)

    def _snapshot_locked(self, refresh, include_observation):
        receipt = None
        validated_phase = None
        if refresh and self.config['enabled'] and not self.closing:
            if include_observation:
                phase = self.status._phase(self.clock, self.config['timing']['statusMaximumAgeMs'])
                last, live, retired = self.status.last, self.status.live, self.status.retired
            else:
                phase, last, live, retired = self.status.read(self.clock, self.config['timing']['statusMaximumAgeMs'])
            receipt = last, live, retired
            validated_phase = phase
            with self.lock:
                self.phase = phase
        if include_observation and receipt is None and hasattr(self, 'status'):
            receipt = self.status.last, self.status.live, self.status.retired
        # Reader lock precedes this lock, as in the original refresh/cleanup
        # path. Close/failure wins even if validation completed just beforehand.
        with self.lock:
            camera = {'configured': bool(self.config['enabled'] and not self.closing and not self.failed),
                      'requested': self.desired,
                      'phase': (validated_phase if include_observation and validated_phase is not None else self.phase)
                      if not self.closing else 'unavailable',
                      'error': self.error}
            if not include_observation:
                return camera
            observation = unavailable_status_observation('disabled' if not self.config['enabled'] else 'unavailable')
            observation['clockDomain'] = self.clock_domain
            observation['maximumAgeMs'] = self.config['timing']['statusMaximumAgeMs']
            last, live, retired = receipt if receipt is not None else (None, False, False)
            if last is not None:
                observation['lastObservation'] = {
                    'instanceStartNs': str(last[0]), 'sequence': str(last[1]),
                    'observedNs': str(last[2]), 'phase': last[3]}
            if self.closing or self.failed:
                observation['state'] = 'manual_owner_unavailable'
            elif retired:
                observation['state'] = 'reader_retired'
            elif self.config['enabled']:
                observation['eligible'] = True
                if live:
                    observation['state'] = 'current'
                    observation['availability'] = 'available'
            return camera, observation

    def snapshot(self):
        return self._snapshot(True)

    def snapshot_with_status_observation(self):
        return self._snapshot(True, include_observation=True)

    @staticmethod
    def _reply(client, value):
        raw = json.dumps(value, separators=(',', ':'), ensure_ascii=True).encode('ascii') + b'\n'
        if len(raw) > 256:
            raw = json.dumps({'ok': False, 'error': _error(value.get('error', 'Camera command failed'))[:80]},
                             separators=(',', ':'), ensure_ascii=True).encode('ascii') + b'\n'
        try:
            # This is one bounded nonblocking write. Only a complete canonical
            # success line is an acknowledgement; truncated/failed writes are
            # closed and the strict client must treat them as failed replies.
            return client.send(raw) == len(raw)
        except OSError:
            return False

    def _run(self):
        pending = None
        try:
            self.selector = selectors.DefaultSelector()
            self.selector.register(self.server, selectors.EVENT_READ, 'listen')
            self.selector.register(self.wake_read, selectors.EVENT_READ, 'wake')
            next_renewal = time.monotonic()
            while True:
                with self.lock:
                    desired, dirty, closing = self.desired, self.dirty, self.closing
                    self.dirty = False
                if closing:
                    break
                self._verify_control()
                actual = time.monotonic()
                if dirty or (desired and actual >= next_renewal):
                    self._publish(desired)
                    next_renewal = actual + self.config['timing']['pollMs'] / 1000
                if pending is not None and actual >= pending[2]:
                    self._reply(pending[0], {'ok': False, 'error': 'Camera command deadline expired'})
                    self.selector.unregister(pending[0]); pending[0].close(); pending = None
                timeout = self.config['timing']['pollMs'] / 1000
                if desired:
                    timeout = min(timeout, max(0, next_renewal - actual))
                if pending is not None:
                    timeout = min(timeout, max(0, pending[2] - actual))
                for key, _ in self.selector.select(timeout):
                    if key.data == 'wake':
                        try:
                            os.read(self.wake_read, 4096)
                        except BlockingIOError:
                            pass
                    elif key.data == 'listen':
                        for _ in range(2):
                            try:
                                client, _ = self.server.accept()
                            except BlockingIOError:
                                break
                            client.setblocking(False)
                            try:
                                _check(_peer_uid(client) in (0, self.owner[0]), 'Camera client account is not allowed')
                                _check(pending is None, 'Camera control is busy')
                                pending = [client, bytearray(), time.monotonic() + 0.2]
                                self.selector.register(client, selectors.EVENT_READ, 'command')
                            except (OSError, ValueError) as error:
                                self._reply(client, {'ok': False, 'error': _error(error)})
                                client.close()
                    elif pending is not None and key.fileobj is pending[0]:
                        client, data, _ = pending
                        try:
                            chunk = client.recv(257 - len(data))
                            _check(bool(chunk), 'Incomplete camera command')
                            data.extend(chunk)
                            _check(len(data) <= 256, 'Camera command exceeds its bound')
                            if b'\n' not in data:
                                continue
                            _check(data.endswith(b'\n') and data.count(b'\n') == 1, 'Camera command requires exactly one line')
                            command = _json(data.decode('utf-8'))
                            _fields(command, ('action', 'enabled'), 'Unknown or missing camera command field')
                            _check(command['action'] == 'camera' and type(command['enabled']) is bool, 'Invalid camera command')
                            self.request(command['enabled'])
                            self._reply(client, {'ok': True, 'message': 'Manual camera requested' if command['enabled'] else 'Manual camera cancelled'})
                        except (OSError, ValueError, UnicodeError) as error:
                            self._reply(client, {'ok': False, 'error': _error(error)})
                        self.selector.unregister(client); client.close(); pending = None
        except Exception as error:
            with self.lock:
                self.failed, self.desired, self.error = True, False, _error(error)
        finally:
            if pending is not None:
                pending[0].close()
            try:
                self._publish(False)
            except (OSError, ValueError) as error:
                with self.lock:
                    self.failed, self.error = True, _error(error)
            self._cleanup()

    def _cleanup(self):
        if self.selector is not None:
            self.selector.close()
        if self.server is not None:
            self.server.close()
        if self.socket_identity is not None:
            try:
                self.socket_path.verify()
                current = self.socket_path.inspect()
                if current is not None and _same(current, self.socket_identity):
                    os.unlink(self.socket_path.name, dir_fd=self.socket_path.fd)
            except (OSError, ValueError):
                pass
            self.socket_identity = None
        for fd in (self.wake_read, self.wake_write):
            if fd >= 0:
                os.close(fd)
        self.wake_read = self.wake_write = -1
        status_lock = self.status.lock if hasattr(self, 'status') else threading.Lock()
        with status_lock:
            for owner in self.owners:
                owner.close()
            for path in self.paths:
                path.close()

    def close(self):
        with self.lock:
            self.closing, self.desired = True, False
        self._wake()
        if self.worker is not None and self.worker is not threading.current_thread():
            self.worker.join(timeout=0.3)
            if self.worker.is_alive():
                with self.lock:
                    self.failed, self.error = True, 'Manual camera cleanup is still pending; timed requests will expire'
        # A pending worker retains its pinned resources/locks until it finishes;
        # another generation cannot race its last OFF publication.

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        if hasattr(self, 'lock'):
            try:
                self.close()
            except Exception:
                pass


class ManualCamera:
    """Public owner. Worker state never retains this handle, so disposal cancels."""
    def __init__(self, path, owner=None, *, clock_ns=None):
        self._state = _ManualState(path, owner, clock_ns=clock_ns)

    def request(self, enabled):
        return self._state.request(enabled)

    def snapshot(self):
        return self._state.snapshot()

    def snapshot_with_status_observation(self):
        """Return camera + provenance from the same single status validation."""
        return self._state.snapshot_with_status_observation()

    def close(self):
        self._state.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        if hasattr(self, '_state'):
            try:
                self.close()
            except Exception:
                pass
