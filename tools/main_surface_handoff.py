# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""A private timed request and real shared/exclusive main-window handoff.

The caller must own page.lock before construction. Map a foreground page only
after poll() returns True; that EX lock acknowledges that every main presenter
has released its SH window lease. Before release(), lower/unmap the page and
complete the caller's GDK synchronization. close() must follow surface teardown.

There is no GTK, IPC or background worker here. Default deadlines use explicit
CLOCK_MONOTONIC, the Linux renderer's wire clock. Darwin flock tests qualify
ownership only: Darwin C++ steady_clock has a different clock origin.

Requests are ephemeral leases, not durable state. Atomic replacement provides
visibility; deliberately avoid fsync on the GUI thread or the eMMC every renewal.
"""
import errno
import fcntl
import os
from pathlib import Path
import re
import secrets
import stat
import time


_UINT64 = (1 << 64) - 1
_LEASE_NS = 750_000_000
_RENEW_REMAINING_NS = 400_000_000
_PATH = re.compile(r'/[A-Za-z0-9_./-]+\Z')
_REQUEST = re.compile(rb'on ([1-9][0-9]{0,19})\n\Z')


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _path(value):
    value = os.fspath(value)
    _require(type(value) is str and len(value) <= 4000 and _PATH.fullmatch(value) and
             not value.endswith('/') and all(part and part not in ('.', '..') and len(part) <= 255
                                            for part in value.split('/')[1:]),
             'Main handoff paths must be bounded canonical absolute ASCII paths')
    return Path(value)


def _identity(info):
    return info.st_dev, info.st_ino


def _signature(info):
    return (_identity(info), info.st_uid, info.st_mode, info.st_nlink,
            info.st_size, info.st_mtime_ns, info.st_ctime_ns)


class MainSurfaceHandoff:
    """Renew a 750ms request; expose real, nonblocking EX ownership as an ACK."""
    def __init__(self, control_path, lease_path, *, owner_uid=None):
        self._directory_fd = self._lease_fd = self._control_fd = -1
        self._closed = False
        self._requested = self._acquired = False
        self._deadline_ns = 0
        self._last_now = None
        self._control_info = None
        self._payload = None
        self.control_path, self.lease_path = _path(control_path), _path(lease_path)
        self._owner_uid = os.geteuid() if owner_uid is None else owner_uid
        _require(type(self._owner_uid) is int and 0 <= self._owner_uid < (1 << 32) - 1 and
                 self._owner_uid == os.geteuid(), 'Main handoff must run as its file owner')
        _require(self.control_path.parent == self.lease_path.parent and self.control_path != self.lease_path,
                 'Main handoff request and lease must be distinct files in one private parent')
        self._parent = self.control_path.parent
        try:
            before = self._parent.lstat()
            _require(self._private_parent(before) and self._parent.resolve(strict=True) == self._parent,
                     'Main handoff parent must be real, owned and 0700')
            self._directory_fd = os.open(self._parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC |
                                         os.O_NOFOLLOW | os.O_NONBLOCK)
            self._parent_info = os.fstat(self._directory_fd)
            _require(self._private_parent(self._parent_info) and _identity(before) == _identity(self._parent_info),
                     'Main handoff parent changed during open')
            self._open_control()
            self._open_lease()
            self.verify()
            self._publish(b'off\n')
        except BaseException:
            self._closed = True
            self._close_handles()
            raise

    @property
    def requested(self):
        return self._requested

    @property
    def acquired(self):
        return self._acquired

    @property
    def deadline_ns(self):
        return self._deadline_ns

    def _private_parent(self, info):
        return stat.S_ISDIR(info.st_mode) and info.st_uid == self._owner_uid and stat.S_IMODE(info.st_mode) == 0o700

    def _private_file(self, info, maximum, empty=False):
        return (stat.S_ISREG(info.st_mode) and info.st_uid == self._owner_uid and
                stat.S_IMODE(info.st_mode) == 0o600 and info.st_nlink == 1 and
                (info.st_size == 0 if empty else 0 < info.st_size <= maximum))

    def _named(self, name):
        try:
            return os.stat(name, dir_fd=self._directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            return None

    def _verify_parent(self):
        named, opened = self._parent.lstat(), os.fstat(self._directory_fd)
        _require(self._private_parent(named) and self._private_parent(opened) and
                 _identity(named) == _identity(opened) == _identity(self._parent_info) and
                 self._parent.resolve(strict=True) == self._parent, 'Main handoff parent generation changed')

    def _open_control(self):
        named = self._named(self.control_path.name)
        if named is None:
            return
        _require(self._private_file(named, 24), 'Main request must be bounded owned single-link 0600 regular data')
        fd = os.open(self.control_path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
                     dir_fd=self._directory_fd)
        try:
            opened = os.fstat(fd)
            _require(self._private_file(opened, 24) and _signature(named) == _signature(opened),
                     'Main request changed during open')
            raw = os.read(fd, 25)
            after, current = os.fstat(fd), self._named(self.control_path.name)
            _require(len(raw) == opened.st_size and current is not None and
                     _signature(opened) == _signature(after) == _signature(current), 'Main request changed during read')
            match = _REQUEST.fullmatch(raw)
            _require(raw == b'off\n' or (match is not None and int(match[1]) <= _UINT64),
                     'Main request has malformed timed content')
            self._control_fd, self._control_info, self._payload = fd, after, raw
            fd = -1
        finally:
            if fd >= 0:
                os.close(fd)

    def _open_lease(self):
        for _ in range(8):
            self._verify_parent()
            named = self._named(self.lease_path.name)
            if named is not None:
                _require(self._private_file(named, 0, True), 'Main lease must be empty owned single-link 0600 regular data')
            try:
                flags = os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK
                if named is None:
                    flags |= os.O_CREAT | os.O_EXCL
                fd = os.open(self.lease_path.name, flags, 0o600, dir_fd=self._directory_fd)
            except FileExistsError:
                continue  # A renderer can safely create this same empty lease.
            try:
                if named is None:
                    os.fchmod(fd, 0o600)
                opened, current = os.fstat(fd), self._named(self.lease_path.name)
                _require(self._private_file(opened, 0, True) and current is not None and
                         self._private_file(current, 0, True) and _identity(current) == _identity(opened) and
                         (named is None or _identity(named) == _identity(opened)), 'Main lease generation changed during open')
                self._lease_fd, self._lease_info = fd, opened
                fd = -1
                return
            finally:
                if fd >= 0:
                    os.close(fd)
        raise ValueError('Main lease creation exceeded its retry bound')

    def verify(self):
        _require(not self._closed, 'Main handoff is closed')
        self._verify_parent()
        named, opened = self._named(self.lease_path.name), os.fstat(self._lease_fd)
        _require(named is not None and self._private_file(named, 0, True) and self._private_file(opened, 0, True) and
                 _identity(named) == _identity(opened) == _identity(self._lease_info), 'Main lease generation changed')
        named = self._named(self.control_path.name)
        if self._control_fd < 0:
            _require(named is None, 'Main request appeared during ownership admission')
        else:
            opened = os.fstat(self._control_fd)
            _require(named is not None and self._private_file(named, 24) and self._private_file(opened, 24) and
                     _signature(named) == _signature(opened) == _signature(self._control_info), 'Main request generation changed')

    def _publish(self, raw):
        self.verify()
        fd, temporary, created = -1, None, None
        try:
            for _ in range(8):
                temporary = '.headunit-main-' + secrets.token_hex(12) + '.tmp'
                try:
                    fd = os.open(temporary, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC |
                                 os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=self._directory_fd)
                    break
                except FileExistsError:
                    continue
            _require(fd >= 0, 'Main request temporary creation exceeded its retry bound')
            created = os.fstat(fd)
            os.fchmod(fd, 0o600)
            written = 0
            for _ in range(24):
                if written == len(raw):
                    break
                count = os.write(fd, raw[written:])
                _require(count > 0, 'Main request write did not progress')
                written += count
            _require(written == len(raw), 'Main request exceeded its bounded write turns')
            info, named = os.fstat(fd), self._named(temporary)
            _require(self._private_file(info, 24) and info.st_size == len(raw) and named is not None and
                     _identity(info) == _identity(created) and _signature(info) == _signature(named),
                     'Main request temporary changed')
            self.verify()
            os.replace(temporary, self.control_path.name, src_dir_fd=self._directory_fd, dst_dir_fd=self._directory_fd)
            temporary = None
            self._verify_parent()
            installed, info = self._named(self.control_path.name), os.fstat(fd)
            _require(installed is not None and self._private_file(installed, 24) and
                     _signature(installed) == _signature(info), 'Installed main request changed')
            previous = self._control_fd
            self._control_fd, self._control_info, self._payload = fd, info, raw
            fd = -1
            if previous >= 0:
                os.close(previous)
        finally:
            if fd >= 0:
                os.close(fd)
            if temporary is not None and created is not None:
                named = self._named(temporary)
                if named is not None and _identity(named) == _identity(created):
                    os.unlink(temporary, dir_fd=self._directory_fd)

    def _now(self, selected):
        now = time.clock_gettime_ns(time.CLOCK_MONOTONIC) if selected is None else selected
        _require(type(now) is int and 0 < now <= _UINT64 - _LEASE_NS and
                 (self._last_now is None or now >= self._last_now), 'Main handoff reference clock reversed or overflowed')
        self._last_now = now
        return now

    def request(self, now_ns=None):
        now = self._now(now_ns)
        self.verify()
        if not self._requested or self._deadline_ns - now <= _RENEW_REMAINING_NS:
            deadline = now + _LEASE_NS
            self._publish(f'on {deadline}\n'.encode('ascii'))
            self._deadline_ns, self._requested = deadline, True

    def poll(self, now_ns=None):
        now = self._now(now_ns)
        self.verify()
        if not self._requested:
            return False
        self.request(now)
        if not self._acquired:
            for _ in range(8):
                try:
                    fcntl.flock(self._lease_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    self._acquired = True
                    break
                except InterruptedError:
                    continue
                except OSError as error:
                    if error.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                        return False
                    raise
            _require(self._acquired, 'Main lease acquisition exceeded interrupted retry bound')
        self.verify()
        return True

    def release(self):
        """Caller must retire/synchronize its foreground surface first."""
        self.verify()
        if self._payload != b'off\n':
            self._publish(b'off\n')
        # Do not unlock on a failed OFF publication: keep exclusion until the
        # caller retires its GDK connection and calls close().
        if self._acquired:
            fcntl.flock(self._lease_fd, fcntl.LOCK_UN)
        self._requested = self._acquired = False
        self._deadline_ns = 0

    def _close_handles(self):
        first_error = None
        for field in ('_control_fd', '_lease_fd', '_directory_fd'):
            fd = getattr(self, field, -1)
            if fd >= 0:
                setattr(self, field, -1)
                try:
                    os.close(fd)
                except OSError as error:
                    # Still close the lease after a different handle's failed
                    # close. Never retry close on a potentially reused fd.
                    if first_error is None:
                        first_error = error
        self._requested = self._acquired = False
        self._deadline_ns = 0
        if first_error is not None:
            raise first_error

    def close(self):
        """Close only after page/display retirement; preserve replacement files."""
        if self._closed:
            return
        try:
            self.release()
        finally:
            self._closed = True
            self._close_handles()

    def __del__(self):
        try:
            self._close_handles()
        except Exception:
            pass
