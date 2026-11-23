#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real private files/flock/processes; no renderer, GTK or hardware qualification.

Explicit clocks test renewal arithmetic. Default-clock tests use Python's
CLOCK_MONOTONIC; on Darwin they do not qualify C++ steady_clock interoperability.
"""
import errno
import fcntl
import os
from pathlib import Path
import re
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import main_surface_handoff as m


class HandoffTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='headunit-main-handoff-')
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name).resolve()
        self.folder.chmod(0o700)
        self.control = self.folder / 'request'
        self.lease = self.folder / 'lease'

    def file(self, path, raw=b'', mode=0o600):
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            os.write(fd, raw)
            os.fchmod(fd, mode)
        finally:
            os.close(fd)
        return path

    def handoff(self):
        handoff = m.MainSurfaceHandoff(self.control, self.lease)
        self.addCleanup(handoff.close)
        return handoff

    def deadline(self):
        raw = self.control.read_bytes()
        match = re.fullmatch(rb'on ([1-9][0-9]{0,19})\n', raw)
        self.assertIsNotNone(match)
        self.assertLessEqual(len(raw), 24)
        return int(match[1])

    def assert_blocked(self, fd, kind=fcntl.LOCK_SH):
        with self.assertRaises(OSError) as caught:
            fcntl.flock(fd, kind | fcntl.LOCK_NB)
        self.assertIn(caught.exception.errno, (errno.EAGAIN, errno.EWOULDBLOCK))

    def assert_no_temporary(self):
        self.assertEqual(list(self.folder.glob('.headunit-main-*.tmp')), [])

    def child(self, script, *args):
        child = subprocess.Popen([sys.executable, '-B', '-c', script, str(Path(m.__file__).parent),
                                  *(str(arg) for arg in args)], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        def cleanup():
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=1)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()
        self.addCleanup(cleanup)
        return child

    def line(self, child):
        ready, _, _ = select.select([child.stdout], [], [], 2)
        self.assertTrue(ready, 'Child did not reach its actual lock boundary')
        line = child.stdout.readline().strip()
        self.assertTrue(line, 'Child stopped without publishing its lock boundary')
        return line

    def shared_child(self, path):
        return self.child('''import fcntl,os,sys
fd=os.open(sys.argv[2],os.O_RDWR|os.O_CREAT|os.O_EXCL|os.O_CLOEXEC,0o600)
os.fchmod(fd,0o600)
fcntl.flock(fd,fcntl.LOCK_SH)
print(os.fstat(fd).st_ino,flush=True)
sys.stdin.buffer.read(1)
os.close(fd)
''', path)

    def test_real_shared_and_exclusive_lifetime(self):
        self.file(self.lease)
        original = self.lease.stat().st_ino
        renderer = os.open(self.lease, os.O_RDWR | os.O_CLOEXEC)
        contender = os.open(self.lease, os.O_RDWR | os.O_CLOEXEC)
        try:
            fcntl.flock(renderer, fcntl.LOCK_SH)
            handoff = self.handoff()
            self.assertEqual(self.lease.stat().st_ino, original)
            self.assertEqual(self.control.read_bytes(), b'off\n')
            self.assertFalse(handoff.poll(1_000_000_000))
            handoff.request(1_000_000_000)
            self.assertTrue(handoff.requested)
            self.assertFalse(handoff.acquired)
            self.assertFalse(handoff.poll(1_025_000_000))
            self.assertEqual(self.deadline(), 1_750_000_000)
            fcntl.flock(renderer, fcntl.LOCK_UN)
            self.assertTrue(handoff.poll(1_050_000_000))
            self.assertTrue(handoff.acquired)
            self.assert_blocked(contender)
            handoff.release()
            self.assertEqual(self.control.read_bytes(), b'off\n')
            self.assertFalse(handoff.requested)
            self.assertFalse(handoff.acquired)
            self.assertEqual(handoff.deadline_ns, 0)
            fcntl.flock(contender, fcntl.LOCK_SH | fcntl.LOCK_NB)
            handoff.release()
            handoff.close()
            handoff.close()
            self.assertEqual(self.lease.stat().st_ino, original)
            self.assert_no_temporary()
        finally:
            os.close(renderer)
            os.close(contender)

    def test_renewal_threshold_and_long_foreground(self):
        handoff = self.handoff()
        now = 1_000_000_000
        handoff.request(now)
        self.assertTrue(handoff.poll(now))
        inode = self.control.stat().st_ino
        deadline = handoff.deadline_ns
        handoff.poll(now + 349_999_999)
        self.assertEqual(self.control.stat().st_ino, inode)
        self.assertEqual(handoff.deadline_ns, deadline)
        handoff.poll(now + 350_000_000)
        self.assertNotEqual(self.control.stat().st_ino, inode)
        self.assertEqual(handoff.deadline_ns, now + 350_000_000 + 750_000_000)
        renderer = os.open(self.lease, os.O_RDWR | os.O_CLOEXEC)
        try:
            for tick in range(15, 641):  # More than 15 seconds of 25ms owner turns.
                current = now + tick * 25_000_000
                self.assertTrue(handoff.poll(current))
                self.assertEqual(self.deadline(), handoff.deadline_ns)
                self.assertGreater(handoff.deadline_ns, current)
                self.assertLessEqual(handoff.deadline_ns, current + 750_000_000)
                self.assert_blocked(renderer)
        finally:
            os.close(renderer)
        self.assert_no_temporary()

    def test_default_clock_is_explicit_monotonic(self):
        handoff = self.handoff()
        before = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        handoff.request()
        after = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        self.assertLessEqual(before + 750_000_000, self.deadline())
        self.assertLessEqual(self.deadline(), after + 750_000_000)
        self.assertTrue(handoff.poll())

    def test_clock_reversal_type_overflow_and_equal_turns(self):
        handoff = self.handoff()
        for bad in (True, False, 1.0, '1', 0, -1, (1 << 64) - 750_000_000, 1 << 64):
            with self.subTest(clock=bad), self.assertRaises(ValueError):
                handoff.request(bad)
        handoff.request(1_000_000_000)
        self.assertTrue(handoff.poll(1_000_000_000))
        inode = self.control.stat().st_ino
        with self.assertRaises(ValueError):
            handoff.poll(999_999_999)
        self.assertTrue(handoff.acquired)
        self.assertEqual(self.control.stat().st_ino, inode)
        self.assertTrue(handoff.poll(1_000_000_000))

    def test_real_process_shared_owner_blocks_ack(self):
        child = self.shared_child(self.lease)
        inode = int(self.line(child))
        handoff = self.handoff()
        self.assertEqual(self.lease.stat().st_ino, inode)
        handoff.request(1_000_000_000)
        self.assertFalse(handoff.poll(1_000_000_000))
        child.stdin.write('\n'); child.stdin.flush()
        self.assertEqual(child.wait(timeout=2), 0)
        self.assertTrue(handoff.poll(1_025_000_000))
        self.assertEqual(self.lease.stat().st_ino, inode)

    def test_renderer_wins_actual_lease_creation_race(self):
        original_open = os.open
        raced = []

        def opening(path, flags, *args, **kwargs):
            if path == self.lease.name and flags & os.O_EXCL and not raced:
                child = self.shared_child(self.lease)
                raced.append((child, int(self.line(child))))
            return original_open(path, flags, *args, **kwargs)

        with mock.patch.object(m.os, 'open', side_effect=opening):
            handoff = self.handoff()
        self.assertEqual(len(raced), 1)
        child, inode = raced[0]
        self.assertEqual(self.lease.stat().st_ino, inode)
        handoff.request(1_000_000_000)
        self.assertFalse(handoff.poll(1_000_000_000))
        child.stdin.write('\n'); child.stdin.flush()
        self.assertEqual(child.wait(timeout=2), 0)
        self.assertTrue(handoff.poll(1_000_000_000))

    def test_unsafe_winner_of_creation_race_is_preserved(self):
        sentinel = self.file(self.folder / 'sentinel', b'never truncate')
        original_open = os.open
        raced = False

        def opening(path, flags, *args, **kwargs):
            nonlocal raced
            if path == self.lease.name and flags & os.O_EXCL and not raced:
                raced = True
                self.lease.symlink_to(sentinel)
            return original_open(path, flags, *args, **kwargs)

        with mock.patch.object(m.os, 'open', side_effect=opening), self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease)
        self.assertTrue(raced)
        self.assertTrue(self.lease.is_symlink())
        self.assertEqual(sentinel.read_bytes(), b'never truncate')
        self.assertFalse(self.control.exists())

    def test_actual_crash_releases_ex_and_timed_request_expires(self):
        child = self.child('''import sys,time
sys.path.insert(0,sys.argv[1])
from main_surface_handoff import MainSurfaceHandoff
h=MainSurfaceHandoff(sys.argv[2],sys.argv[3])
h.request()
assert h.poll()
print(h.deadline_ns,flush=True)
while True: time.sleep(1)
''', self.control, self.lease)
        deadline = int(self.line(child))
        renderer = os.open(self.lease, os.O_RDWR | os.O_CLOEXEC)
        try:
            self.assert_blocked(renderer)
            child.send_signal(signal.SIGKILL)
            self.assertEqual(child.wait(timeout=2), -signal.SIGKILL)
            fcntl.flock(renderer, fcntl.LOCK_SH | fcntl.LOCK_NB)
            self.assertEqual(self.deadline(), deadline)
            until = time.monotonic() + 1.2
            while time.clock_gettime_ns(time.CLOCK_MONOTONIC) <= deadline and time.monotonic() < until:
                time.sleep(0.005)
            self.assertGreater(time.clock_gettime_ns(time.CLOCK_MONOTONIC), deadline)
            self.assertEqual(self.deadline(), deadline)  # No continuing background renewal.
        finally:
            os.close(renderer)

    def test_private_shapes_modes_and_safe_constructor_cleanup(self):
        for mode in (0o755, 0o770, 0o1700, 0o4700):
            self.folder.chmod(mode)
            with self.subTest(mode=oct(mode)), self.assertRaises(ValueError):
                m.MainSurfaceHandoff(self.control, self.lease)
            self.assertFalse(self.control.exists())
            self.assertFalse(self.lease.exists())
        self.folder.chmod(0o700)
        with self.assertRaises(FileNotFoundError):
            m.MainSurfaceHandoff(self.folder / 'missing' / 'request', self.folder / 'missing' / 'lease')
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease, owner_uid=os.geteuid() + 1)
        for control, lease in ((self.control, self.control), (self.control, self.folder / 'other' / 'lease'),
                               ('relative', self.lease), (str(self.folder) + '/../request', self.lease),
                               (str(self.control) + '/', self.lease), (str(self.folder) + '//request', self.lease),
                               (str(self.folder / 'é'), self.lease), (str(self.folder / 'bad name'), self.lease)):
            with self.subTest(control=control, lease=lease), self.assertRaises(ValueError):
                m.MainSurfaceHandoff(control, lease)
        alias = self.folder / 'alias'; alias.symlink_to(self.folder, target_is_directory=True)
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(alias / 'request', alias / 'lease')
        self.assertFalse(self.control.exists())
        self.assertFalse(self.lease.exists())

    def test_unsafe_existing_lease_never_truncated_or_removed(self):
        for raw, mode in ((b'leased storage', 0o600), (b'', 0o644), (b'', 0o1600), (b'', 0o4600)):
            self.file(self.lease, raw, mode)
            identity = self.lease.lstat()
            with self.subTest(raw=raw, mode=oct(mode)), self.assertRaises(ValueError):
                m.MainSurfaceHandoff(self.control, self.lease)
            self.assertEqual(self.lease.read_bytes(), raw)
            self.assertEqual(self.lease.lstat().st_ino, identity.st_ino)
            self.assertEqual(stat.S_IMODE(self.lease.lstat().st_mode), mode)
            self.lease.unlink()
        sentinel = self.file(self.folder / 'sentinel', b'unchanged')
        self.lease.symlink_to(sentinel)
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease)
        self.assertEqual(sentinel.read_bytes(), b'unchanged'); self.lease.unlink()
        os.mkfifo(self.lease, 0o600)
        began = time.monotonic()
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease)
        self.assertLess(time.monotonic() - began, 0.3); self.lease.unlink()
        self.lease.mkdir(mode=0o700)
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease)
        self.lease.rmdir()
        self.file(self.lease); os.link(self.lease, self.folder / 'lease-alias')
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease)
        self.assertEqual(self.lease.lstat().st_nlink, 2)
        self.assertFalse(self.control.exists())

    def test_unsafe_existing_request_preserved(self):
        for raw, mode in ((b'on\n', 0o600), (b'on 01\n', 0o600), (b'on 0\n', 0o600),
                          (b'on 18446744073709551616\n', 0o600), (b'off', 0o600), (b'', 0o600),
                          (b'x' * 25, 0o600), (b'off\n', 0o644), (b'off\n', 0o1600), (b'off\n', 0o4600)):
            self.file(self.control, raw, mode)
            identity = self.control.lstat()
            with self.subTest(raw=raw, mode=oct(mode)), self.assertRaises(ValueError):
                m.MainSurfaceHandoff(self.control, self.lease)
            self.assertEqual(self.control.read_bytes(), raw)
            self.assertEqual(self.control.lstat().st_ino, identity.st_ino)
            self.assertFalse(self.lease.exists())
            self.control.unlink()
        sentinel = self.file(self.folder / 'sentinel', b'unchanged')
        self.control.symlink_to(sentinel)
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease)
        self.assertEqual(sentinel.read_bytes(), b'unchanged'); self.control.unlink()
        os.mkfifo(self.control, 0o600)
        began = time.monotonic()
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease)
        self.assertLess(time.monotonic() - began, 0.3); self.control.unlink()
        self.file(self.control, b'off\n'); os.link(self.control, self.folder / 'request-alias')
        with self.assertRaises(ValueError):
            m.MainSurfaceHandoff(self.control, self.lease)
        self.assertFalse(self.lease.exists())

    def test_existing_timed_record_resets_off_and_never_reuses_lease(self):
        self.file(self.control, b'on 18446744073709551615\n')
        self.file(self.lease)
        lease_inode = self.lease.stat().st_ino
        handoff = self.handoff()
        self.assertEqual(self.control.read_bytes(), b'off\n')
        self.assertEqual(self.lease.stat().st_ino, lease_inode)
        self.assertFalse(handoff.requested)
        self.assertFalse(handoff.poll(1_000_000_000))
        handoff.close()
        second = self.handoff()
        self.assertEqual(self.lease.stat().st_ino, lease_inode)
        self.assertEqual(self.control.read_bytes(), b'off\n')
        self.assertFalse(second.acquired)

    def test_replaced_request_fails_closed_retains_ex_until_close(self):
        handoff = self.handoff(); handoff.request(1_000_000_000)
        self.assertTrue(handoff.poll(1_000_000_000))
        replacement = self.file(self.folder / 'replacement', b'on 9000000000\n')
        os.replace(replacement, self.control)
        inode = self.control.stat().st_ino
        renderer = os.open(self.lease, os.O_RDWR | os.O_CLOEXEC)
        try:
            for operation in (handoff.verify, handoff.release, lambda: handoff.poll(1_025_000_000)):
                with self.assertRaisesRegex(ValueError, 'request generation changed'):
                    operation()
                self.assert_blocked(renderer)
                self.assertEqual(self.control.read_bytes(), b'on 9000000000\n')
                self.assertEqual(self.control.stat().st_ino, inode)
            with self.assertRaises(ValueError):
                handoff.close()  # Caller has already retired its surface.
            fcntl.flock(renderer, fcntl.LOCK_SH | fcntl.LOCK_NB)
            self.assertEqual(self.control.read_bytes(), b'on 9000000000\n')
            handoff.close()
        finally:
            os.close(renderer)

    def test_replaced_lease_cannot_ack_a_new_generation(self):
        handoff = self.handoff(); handoff.request(1_000_000_000)
        self.assertTrue(handoff.poll(1_000_000_000))
        old_renderer = os.open(self.lease, os.O_RDWR | os.O_CLOEXEC)
        replacement = self.file(self.folder / 'replacement')
        os.replace(replacement, self.lease)
        inode = self.lease.stat().st_ino
        try:
            with self.assertRaisesRegex(ValueError, 'lease generation changed'):
                handoff.poll(1_025_000_000)
            self.assert_blocked(old_renderer)
            with self.assertRaises(ValueError):
                handoff.close()
            fcntl.flock(old_renderer, fcntl.LOCK_SH | fcntl.LOCK_NB)
            self.assertEqual(self.lease.stat().st_ino, inode)
            self.assertEqual(self.lease.read_bytes(), b'')
        finally:
            os.close(old_renderer)

    def test_parent_replacement_cannot_redirect_requests(self):
        parent = self.folder / 'parent'; parent.mkdir(mode=0o700)
        old = self.folder / 'old'
        handoff = m.MainSurfaceHandoff(parent / 'request', parent / 'lease')
        self.addCleanup(handoff.close)
        handoff.request(1_000_000_000); self.assertTrue(handoff.poll(1_000_000_000))
        parent.rename(old); parent.mkdir(mode=0o700)
        with self.assertRaisesRegex(ValueError, 'parent generation changed'):
            handoff.release()
        self.assertEqual(list(parent.iterdir()), [])
        with self.assertRaises(ValueError):
            handoff.close()
        self.assertEqual(list(parent.iterdir()), [])
        self.assertTrue((old / 'request').read_bytes().startswith(b'on '))

    def test_same_inode_data_and_mode_mutation_are_rejected(self):
        handoff = self.handoff()
        inode = self.control.stat().st_ino
        self.control.write_bytes(b'on 9999999999\n')
        self.assertEqual(self.control.stat().st_ino, inode)
        with self.assertRaisesRegex(ValueError, 'request generation changed'):
            handoff.verify()
        with self.assertRaises(ValueError):
            handoff.close()
        self.control.unlink()
        second = self.handoff()
        self.lease.chmod(0o1600)
        with self.assertRaisesRegex(ValueError, 'lease generation changed'):
            second.verify()
        with self.assertRaises(ValueError):
            second.close()
        self.assertEqual(stat.S_IMODE(self.lease.stat().st_mode), 0o1600)

    def test_failed_atomic_write_and_rename_do_not_unlock(self):
        handoff = self.handoff(); handoff.request(1_000_000_000)
        self.assertTrue(handoff.poll(1_000_000_000))
        raw, inode = self.control.read_bytes(), self.control.stat().st_ino
        renderer = os.open(self.lease, os.O_RDWR | os.O_CLOEXEC)
        try:
            for target, effect in (('write', lambda *_: 0),
                                   ('replace', OSError(errno.EIO, 'Injected atomic rename failure'))):
                with mock.patch.object(m.os, target, side_effect=effect), self.assertRaises((ValueError, OSError)):
                    handoff.release()
                self.assertTrue(handoff.acquired)
                self.assertTrue(handoff.requested)
                self.assert_blocked(renderer)
                self.assertEqual(self.control.read_bytes(), raw)
                self.assertEqual(self.control.stat().st_ino, inode)
                self.assert_no_temporary()
            handoff.release()
            self.assertEqual(self.control.read_bytes(), b'off\n')
            fcntl.flock(renderer, fcntl.LOCK_SH | fcntl.LOCK_NB)
        finally:
            os.close(renderer)

    def test_close_after_failed_off_publication_retires_all_handles(self):
        handoff = self.handoff(); handoff.request(1_000_000_000)
        self.assertTrue(handoff.poll(1_000_000_000))
        renderer = os.open(self.lease, os.O_RDWR | os.O_CLOEXEC)
        try:
            with mock.patch.object(m.os, 'replace', side_effect=OSError(errno.EIO, 'Injected rename failure')):
                with self.assertRaises(OSError):
                    handoff.close()  # Foreground/display already withdrawn.
            self.assertFalse(handoff.acquired)
            self.assertEqual(handoff._control_fd, -1)
            self.assertEqual(handoff._lease_fd, -1)
            self.assertEqual(handoff._directory_fd, -1)
            fcntl.flock(renderer, fcntl.LOCK_SH | fcntl.LOCK_NB)
            self.assertTrue(self.control.read_bytes().startswith(b'on '))
            handoff.close()
        finally:
            os.close(renderer)

    def test_temporary_collision_and_interrupted_lock_bounds(self):
        handoff = self.handoff()
        sentinel = self.file(self.folder / '.headunit-main-collision.tmp', b'preserve')
        with mock.patch.object(m.secrets, 'token_hex', return_value='collision') as random:
            with self.assertRaisesRegex(ValueError, 'temporary creation exceeded'):
                handoff.request(1_000_000_000)
            self.assertEqual(random.call_count, 8)
        self.assertEqual(sentinel.read_bytes(), b'preserve')
        self.assertEqual(self.control.read_bytes(), b'off\n')
        handoff.request(1_000_000_000)
        with mock.patch.object(m.fcntl, 'flock', side_effect=InterruptedError()) as interrupted:
            with self.assertRaisesRegex(ValueError, 'interrupted retry bound'):
                handoff.poll(1_000_000_000)
            self.assertEqual(interrupted.call_count, 8)
        self.assertFalse(handoff.acquired)
        self.assertTrue(handoff.poll(1_000_000_000))

    def test_atomic_replacement_keeps_captured_old_record_immutable(self):
        handoff = self.handoff()
        held = os.open(self.control, os.O_RDONLY | os.O_CLOEXEC)
        try:
            handoff.request(1_000_000_000)
            self.assertEqual(os.read(held, 24), b'off\n')
            self.assertEqual(os.fstat(held).st_nlink, 0)
            self.assertEqual(self.deadline(), 1_750_000_000)
            for path in (self.control, self.lease):
                info = path.lstat()
                self.assertTrue(stat.S_ISREG(info.st_mode))
                self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
                self.assertEqual(info.st_nlink, 1)
                self.assertEqual(info.st_uid, os.geteuid())
            self.assert_no_temporary()
        finally:
            os.close(held)


if __name__ == '__main__':
    unittest.main()
