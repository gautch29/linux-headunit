# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""One restartable private phone-manager endpoint; no phone operations here.

The empty ownership lock is retained while this endpoint lives. An explicitly
inheriting owned launcher may keep it after its manager dies. Linux binds and
changes socket metadata through pinned directory/inode handles. The Darwin
branch exists for portable filesystem/Unix-IPC fixtures, not appliance evidence.
"""
import errno
import fcntl
import json
import math
import os
from pathlib import Path
import select
import socket
import stat
import struct
import sys
import time


MAX_COMMAND_BYTES = 1_500_000


def _check(condition, message):
    if not condition:
        raise ValueError(message)


def _same(left, right):
    return (left.st_dev, left.st_ino) == (right.st_dev, right.st_ino)


def _stable(left, right):
    return _same(left, right) and left.st_ctime_ns == right.st_ctime_ns


def _unique(pairs):
    value = {}
    for key, item in pairs:
        _check(key not in value, 'Duplicate manager command field')
        value[key] = item
    return value


def _real_float(value):
    number = float(value)
    _check(math.isfinite(number), 'Nonfinite manager command value')
    return number


def _constant(_):
    raise ValueError('Nonfinite manager command value')


def _peer_uid(client):
    if sys.platform.startswith('linux'):
        return struct.unpack('=3i', client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[1]
    if sys.platform == 'darwin':
        raw = client.getsockopt(0, socket.LOCAL_PEERCRED, 128)
        version, uid = struct.unpack_from('=II', raw)
        _check(version == 0, 'Unknown manager peer credentials')
        return uid
    raise ValueError('Manager peer verification is unavailable')


class ManagerEndpoint:
    """Own one manager.sock and manager.lock in an existing private directory.

    Constructor acquisition precedes any manager or receiver side effects.
    serve_once calls its callback serially, only for a complete authenticated
    dictionary. The optional small incoming timeout is for bounded IPC fixtures.
    close never explicitly unlocks a deliberately inherited launcher descriptor.
    """
    def __init__(self, directory, owner=None, *, incoming_timeout=3.0):
        self._directory_fd = self._lock_fd = -1
        self._server = None
        self._directory_identity = self._lock_identity = self._socket_identity = None
        self._closed = False
        self._owner = (os.getuid(), os.getgid()) if owner is None else owner
        _check(type(self._owner) in (tuple, list) and len(self._owner) == 2 and
               all(type(item) is int and 0 <= item < (1 << 32) - 1 for item in self._owner),
               'Invalid manager endpoint account')
        self._owner = tuple(self._owner)
        _check(os.geteuid() in (0, self._owner[0]), 'Manager endpoint cannot impersonate another account')
        _check(type(incoming_timeout) in (int, float) and math.isfinite(incoming_timeout) and
               .01 <= incoming_timeout <= 3.0, 'Manager incoming timeout must be .01..3 seconds')
        self._incoming_timeout = float(incoming_timeout)
        raw = os.fspath(directory)
        _check(type(raw) is str and raw.startswith('/') and not raw.endswith('/') and
               all(32 <= ord(c) <= 126 for c in raw) and os.path.normpath(raw) == raw and
               all(part not in ('', '.', '..') for part in raw.split('/')[1:]),
               'Manager directory must be a canonical absolute ASCII path')
        self._directory = Path(raw)
        self.path = self._directory / 'manager.sock'
        _check(len(os.fsencode(self.path)) <= 103, 'Manager Unix socket path exceeds 103 bytes')
        try:
            before = self._directory.lstat()
            _check(self._private(before) and self._directory.resolve(strict=True) == self._directory,
                   'Manager directory must be real, owned and 0700 without redirected ancestors')
            self._directory_fd = os.open(self._directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC |
                                         os.O_NOFOLLOW | os.O_NONBLOCK)
            self._directory_identity = os.fstat(self._directory_fd)
            _check(self._private(self._directory_identity) and _same(before, self._directory_identity),
                   'Manager directory changed during open')
            self._acquire_lock()
            self._reclaim_dead_socket()
            self._bind()
        except BaseException:
            self.close()
            raise

    def _private(self, value):
        return stat.S_ISDIR(value.st_mode) and value.st_uid == self._owner[0] and (value.st_mode & 0o7777) == 0o700

    def _lock(self, value):
        return stat.S_ISREG(value.st_mode) and value.st_uid == self._owner[0] and \
            (value.st_mode & 0o7777) == 0o600 and value.st_nlink == 1 and value.st_size == 0

    def _socket(self, value):
        return stat.S_ISSOCK(value.st_mode) and value.st_uid == self._owner[0] and \
            (value.st_mode & 0o7777) == 0o600 and value.st_nlink == 1

    def _inspect(self, name):
        try:
            return os.stat(name, dir_fd=self._directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            return None

    def _verify_directory(self):
        _check(self._directory_fd >= 0 and self._directory_identity is not None, 'Manager endpoint is closed')
        named, opened = self._directory.lstat(), os.fstat(self._directory_fd)
        _check(self._private(named) and self._private(opened) and _same(named, self._directory_identity) and
               _same(opened, self._directory_identity) and self._directory.resolve(strict=True) == self._directory,
               'Manager directory generation changed or became unsafe')

    def _verify_lock(self):
        self._verify_directory()
        _check(self._lock_fd >= 0 and self._lock_identity is not None, 'Manager ownership lock is unavailable')
        named, opened = self._inspect('manager.lock'), os.fstat(self._lock_fd)
        _check(named is not None and self._lock(named) and self._lock(opened) and
               _stable(named, self._lock_identity) and _stable(opened, self._lock_identity),
               'Manager ownership lock generation changed or became unsafe')

    def _verify_socket(self):
        self._verify_lock()
        named = self._inspect('manager.sock')
        _check(named is not None and self._socket_identity is not None and self._socket(named) and
               _stable(named, self._socket_identity), 'Manager socket generation changed or became unsafe')

    def _acquire_lock(self):
        for _ in range(4):
            self._verify_directory()
            named = self._inspect('manager.lock')
            if named is None:
                try:
                    self._lock_fd = os.open('manager.lock', os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC |
                                            os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=self._directory_fd)
                except FileExistsError:
                    continue
                created = os.fstat(self._lock_fd)
                self._verify_directory()
                current = self._inspect('manager.lock')
                _check(current is not None and _same(created, current) and stat.S_ISREG(created.st_mode) and
                       created.st_uid == os.geteuid() and created.st_nlink == 1 and created.st_size == 0,
                       'New manager lock changed before securing it')
                if os.geteuid() == 0:
                    os.fchown(self._lock_fd, *self._owner)
                os.fchmod(self._lock_fd, 0o600)
            else:
                _check(self._lock(named), 'Manager lock must be empty owned single-link 0600 regular data')
                self._lock_fd = os.open('manager.lock', os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW |
                                        os.O_NONBLOCK, dir_fd=self._directory_fd)
            self._lock_identity = os.fstat(self._lock_fd)
            _check(self._lock(self._lock_identity) and (named is None or _same(named, self._lock_identity)),
                   'Manager lock changed during open')
            try:
                fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise ValueError('Manager endpoint already has a live owner or receiver launcher') from error
            self._verify_lock()
            return
        raise ValueError('Manager lock creation exceeded its retry bound')

    def _address(self):
        if sys.platform.startswith('linux'):
            return f'/proc/self/fd/{self._directory_fd}/manager.sock'
        return str(self.path)

    def _reclaim_dead_socket(self):
        self._verify_lock()
        previous = self._inspect('manager.sock')
        if previous is None:
            return
        _check(self._socket(previous), 'Existing manager endpoint must be owned single-link 0600 socket data')
        metadata = -1
        try:
            if sys.platform.startswith('linux'):
                # Keep the filesystem inode alive across the probe. A bound
                # Unix socket alone does not prevent pathname inode reuse;
                # coarse ctime can otherwise hide a replacement listener.
                metadata = os.open('manager.sock', os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW,
                                   dir_fd=self._directory_fd)
                pinned = os.fstat(metadata)
                _check(self._socket(pinned) and _stable(previous, pinned),
                       'Existing manager socket changed while checking liveness')
            refused = False
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                probe.settimeout(.1)
                try:
                    probe.connect(self._address())
                except OSError as error:
                    _check(error.errno == errno.ECONNREFUSED, 'Existing manager socket liveness is ambiguous')
                    refused = True
            _check(refused, 'Existing manager socket still has a live listener')
            self._verify_lock()
            current = self._inspect('manager.sock')
            _check(current is not None and self._socket(current) and _stable(previous, current),
                   'Existing manager socket changed while checking liveness')
            os.unlink('manager.sock', dir_fd=self._directory_fd)
        finally:
            if metadata >= 0:
                os.close(metadata)

    def _new_socket(self):
        self._verify_lock()
        current = self._inspect('manager.sock')
        _check(current is not None and self._socket_identity is not None and _same(current, self._socket_identity) and
               stat.S_ISSOCK(current.st_mode) and current.st_nlink == 1 and current.st_uid == os.geteuid(),
               'New manager socket changed before securing it')
        return current

    def _bind(self):
        self._verify_lock()
        _check(self._inspect('manager.sock') is None, 'Manager socket appeared before binding')
        self._server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._server.bind(self._address())
        self._socket_identity = self._inspect('manager.sock')
        self._new_socket()
        if sys.platform.startswith('linux'):
            metadata = os.open('manager.sock', os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=self._directory_fd)
            try:
                pinned = os.fstat(metadata)
                _check(stat.S_ISSOCK(pinned.st_mode) and _same(pinned, self._socket_identity),
                       'Manager socket metadata generation changed')
                self._new_socket()
                os.chmod(f'/proc/self/fd/{metadata}', 0o600)
                self._new_socket()
                if os.geteuid() == 0:
                    os.chown(f'/proc/self/fd/{metadata}', *self._owner)
            finally:
                os.close(metadata)
        else:
            os.chmod('manager.sock', 0o600, dir_fd=self._directory_fd, follow_symlinks=False)
            self._new_socket()
            if os.geteuid() == 0:
                os.chown('manager.sock', *self._owner, dir_fd=self._directory_fd, follow_symlinks=False)
        self._verify_lock()
        secured = self._inspect('manager.sock')
        _check(secured is not None and self._socket(secured) and _same(secured, self._socket_identity),
               'Manager socket changed while securing it')
        self._socket_identity = secured
        self._verify_socket()
        self._server.listen(4)
        self._server.setblocking(False)

    @property
    def inherited_lock_fd(self):
        """Only pass explicitly to the owned receiver launcher with pass_fds."""
        self._verify_socket()
        return self._lock_fd

    def _receive(self, client):
        deadline = time.monotonic() + self._incoming_timeout
        raw = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            _check(remaining > 0, 'Manager command deadline expired')
            client.settimeout(remaining)
            chunk = client.recv(min(65536, MAX_COMMAND_BYTES + 1 - len(raw)))
            _check(bool(chunk), 'Manager command is incomplete')
            raw.extend(chunk)
            _check(len(raw) <= MAX_COMMAND_BYTES, 'Manager command exceeds 1,500,000 bytes')
            # Previous chunks contain no delimiter. Scan only the newly received
            # bytes so highly fragmented input stays linear within the deadline.
            if b'\n' in chunk:
                break
        _check(raw.endswith(b'\n') and raw.count(b'\n') == 1, 'Manager command requires exactly one newline')
        value = json.loads(raw.decode('utf-8'), object_pairs_hook=_unique, parse_float=_real_float, parse_constant=_constant)
        _check(type(value) is dict, 'Manager command must be a dictionary')
        _check(time.monotonic() < deadline, 'Manager command deadline expired')
        return value

    def _reply(self, client, value):
        try:
            raw = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(',', ':')).encode('utf-8') + b'\n'
        except (ValueError, TypeError, RecursionError, UnicodeError):
            raw = b'{"ok":false,"error":"Manager callback returned an invalid reply"}\n'
        if len(raw) > MAX_COMMAND_BYTES:
            raw = b'{"ok":false,"error":"Manager reply exceeds its size bound"}\n'
        try:
            deadline = time.monotonic() + self._incoming_timeout
            view = memoryview(raw)
            while view:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return
                client.settimeout(remaining)
                count = client.send(view)
                if count <= 0:
                    return
                view = view[count:]
        except (OSError, ValueError):
            return

    def serve_once(self, callback):
        """Wait at most .1 s for one client; callback execution stays serial."""
        self._verify_socket()
        readable, _, _ = select.select([self._server], [], [], .1)
        if not readable:
            return False
        try:
            client, _ = self._server.accept()
        except BlockingIOError:
            return False
        with client:
            try:
                _check(_peer_uid(client) in (0, self._owner[0]), 'Manager client account is not allowed')
                command = self._receive(client)
                self._verify_socket()
                result = callback(command)
                _check(type(result) is dict, 'Manager callback must return a dictionary')
                self._verify_socket()
                reply = {'ok': True, **result}
            except Exception as error:
                reply = {'ok': False, 'error': str(error)[:300]}
            self._reply(client, reply)
        return True

    def close(self):
        """Release only this generation; keep the lock inode for safe restart."""
        if self._closed:
            return False
        self._closed = True
        removed = False
        try:
            if self._socket_identity is not None:
                try:
                    self._verify_socket()
                except (OSError, ValueError):
                    pass
                else:
                    os.unlink('manager.sock', dir_fd=self._directory_fd)
                    removed = True
        finally:
            if self._server is not None:
                self._server.close()
                self._server = None
            if self._lock_fd >= 0:
                # No LOCK_UN: an owned launcher can inherit this open-file
                # description and retain ownership after its manager exits.
                os.close(self._lock_fd)
                self._lock_fd = -1
            if self._directory_fd >= 0:
                os.close(self._directory_fd)
                self._directory_fd = -1
        return removed

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
