#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real wrapper cleanup with explicitly fake tools, not GPU qualification."""
import contextlib
import io
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock

import standard_surface_handoff_test as wrapper
from generated_h264_fixture import MAXIMUM_ENCODED_BYTES, packets


FAKE_TOOL = r'''
import json, os, pathlib, signal, socket, struct, subprocess, sys, time
role = pathlib.Path(sys.argv[0]).name
runtime = pathlib.Path(os.environ['XDG_RUNTIME_DIR'])
audit = pathlib.Path(os.environ['FAKE_HANDOFF_AUDIT'])
def record(name):
    with audit.open('a') as log:
        log.write(json.dumps({'role': name, 'pid': os.getpid(), 'pgid': os.getpgrp(), 'runtime': str(runtime)}) + '\n')
record(role)
assert runtime.name.startswith('headunit-') and runtime.stat().st_mode & 0o7777 == 0o700
if os.environ.get('FAKE_HANDOFF_DESCENDANTS') == '1':
    descendant = "import json,os,pathlib,signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); p=pathlib.Path(os.environ['FAKE_HANDOFF_AUDIT']); f=p.open('a'); f.write(json.dumps({'role': 'descendant', 'pid': os.getpid(), 'pgid': os.getpgrp(), 'runtime': os.environ['XDG_RUNTIME_DIR']})+'\\n'); f.close(); time.sleep(90)"
    subprocess.Popen([sys.executable, '-c', descendant], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.03)
mode = os.environ.get('FAKE_HANDOFF_MODE', '')
if role == 'ffmpeg':
    if mode == 'encoder-fail': sys.exit(9)
    if mode == 'encoder-hang': time.sleep(90)
    if mode == 'malformed': sys.stdout.buffer.write(b'not H264'); sys.exit(0)
    if mode == 'encoder-overflow':
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        sys.stdout.buffer.write(b'x' * (3 * 1024 * 1024)); sys.stdout.buffer.flush(); time.sleep(90)
    data = bytearray()
    def nal(value): data.extend(b'\x00\x00\x00\x01' + value)
    for i in range(18):
        nal(b'\x09\xf0')
        if i == 0: nal(b'\x67\x42\x00\x1e'); nal(b'\x68\x01')
        nal(bytes([0x65 if i == 0 else 0x41, 0x11]))
    sys.stdout.buffer.write(data); sys.exit(0)
if role == 'weston':
    assert '--backend=headless' in sys.argv and '--renderer=gl' in sys.argv and '--no-config' in sys.argv
    name = next(a.split('=', 1)[1] for a in sys.argv if a.startswith('--socket='))
    path = runtime / name
    if mode == 'hang-startup': signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(90)
    if mode == 'regular-file': path.write_text('not a socket'); time.sleep(90)
    if mode == 'symlink':
        target = runtime / (name + '-target'); target.write_text('not a socket'); path.symlink_to(target); time.sleep(90)
    server = socket.socket(socket.AF_UNIX); server.bind(str(path)); path.chmod(0o600)
    # The socket file exists but nothing ever accepts on it: an exit after
    # listen() would instead race the readiness connect and test a late exit.
    if mode == 'early-exit' and name == 'standard-cluster': sys.exit(8)
    server.listen(); server.settimeout(0.05)
    while True:
        try: connection, _ = server.accept(); connection.close()
        except socket.timeout: pass
if role == 'probe':
    fixture, main, cluster = [pathlib.Path(value) for value in sys.argv[1:]]
    assert len(sys.argv) == 4 and main != cluster and main.parent == cluster.parent == fixture.parent == runtime
    assert main.is_socket() and cluster.is_socket() and not main.is_symlink() and not cluster.is_symlink()
    assert fixture.stat().st_mode & 0o7777 == 0o600
    data = fixture.read_bytes(); assert data[:5] == b'UHVP\x01'
    offset, configs, frames = 5, 0, 0
    while offset < len(data):
        kind, timestamp, count = struct.unpack_from('<BQI', data, offset); offset += 13
        if kind: configs += 1
        else:
            assert timestamp == (900 << 32) + (frames << 32) // 30
            frames += 1
        offset += count
    assert offset == len(data) and configs == 1 and frames == 18
    if mode == 'probe-hang': signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(90)
    if mode == 'probe-fail': sys.exit(7)
    print('explicit fake probe accepted private endpoints and generated UHVP fixture'); sys.exit(0)
raise AssertionError('unknown fake tool')
'''


class WrapperTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='headunit-wrapper-tools-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.root.chmod(0o700)
        self.audit = self.root / 'audit.jsonl'
        for role in ('weston', 'probe', 'ffmpeg'):
            path = self.root / role
            path.write_text('#!' + sys.executable + '\n' + textwrap.dedent(FAKE_TOOL))
            path.chmod(0o700)
        self.limits = wrapper.Limits(encode=1, startup=1, probe=1, terminate=0.15, reap=0.5)

    def records(self):
        if not self.audit.exists():
            return []
        return [json.loads(line) for line in self.audit.read_text().splitlines()]

    def assert_cleanup(self):
        records = self.records()
        self.assertTrue(records, 'fake tools never ran')
        for runtime in {record['runtime'] for record in records}:
            self.assertFalse(Path(runtime).exists(), 'wrapper left its private runtime behind')
        for record in records:
            # An exited orphan may briefly remain as a zombie on a container's
            # init. It is terminal and cannot render, hold sockets or run code.
            deadline = time.monotonic() + 1
            while True:
                state = subprocess.run(['/bin/ps', '-o', 'stat=', '-p', str(record['pid'])],
                                       capture_output=True, text=True, timeout=1).stdout.strip()
                if not state or state.startswith('Z'):
                    break
                if time.monotonic() >= deadline:
                    self.fail(f"owned fake child remains alive: {record}, state={state}")
                time.sleep(0.02)
        leaders = [record for record in records if record['role'] != 'descendant']
        self.assertTrue(all(record['pid'] == record['pgid'] for record in leaders), 'tools did not get their own process groups')
        groups = {record['pgid'] for record in leaders}
        self.assertTrue(all(record['pgid'] in groups for record in records), 'descendant escaped a recorded owned group')

    def run_fake(self, mode='', probe=None):
        environment = {'FAKE_HANDOFF_AUDIT': str(self.audit), 'FAKE_HANDOFF_MODE': mode,
                       'FAKE_HANDOFF_DESCENDANTS': '1'}
        with mock.patch.dict(os.environ, environment), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return wrapper.run(str(self.root / 'weston'), probe or str(self.root / 'probe'),
                               str(self.root / 'ffmpeg'), self.limits)

    def test_success_runs_probe_and_cleans_completed_encoder_descendants(self):
        self.assertEqual(self.run_fake(), 0)
        self.assertEqual(sum(record['role'] == 'weston' for record in self.records()), 2)
        self.assertEqual(sum(record['role'] == 'probe' for record in self.records()), 1)
        self.assertTrue(any(record['role'] == 'descendant' for record in self.records()))
        self.assert_cleanup()

    def test_nonzero_probe_status_is_preserved_and_all_groups_are_cleaned(self):
        self.assertEqual(self.run_fake('probe-fail'), 7)
        self.assert_cleanup()

    def test_probe_timeout_kills_term_ignoring_probe_and_descendants(self):
        with self.assertRaises(TimeoutError):
            self.run_fake('probe-hang')
        self.assert_cleanup()

    def test_encoder_failure_and_invalid_fixture_clean_exited_group_descendants(self):
        for mode, exception in (('encoder-fail', RuntimeError), ('malformed', ValueError)):
            with self.subTest(mode=mode):
                self.audit.unlink(missing_ok=True)
                with self.assertRaises(exception):
                    self.run_fake(mode)
                self.assertFalse(any(record['role'] == 'weston' for record in self.records()))
                self.assert_cleanup()

    def test_encoder_timeout_is_bounded_and_cleans_its_group(self):
        with self.assertRaises(TimeoutError):
            self.run_fake('encoder-hang')
        self.assert_cleanup()

    def test_encoder_overflow_is_kernel_bounded_and_cleans_its_group(self):
        with self.assertRaises(RuntimeError):
            self.run_fake('encoder-overflow')
        self.assertFalse(any(record['role'] == 'weston' for record in self.records()))
        self.assert_cleanup()

    def test_startup_timeout_cleans_both_owned_groups(self):
        with self.assertRaises(TimeoutError):
            self.run_fake('hang-startup')
        self.assert_cleanup()

    def test_socket_existence_without_live_compositor_is_insufficient(self):
        with self.assertRaises(RuntimeError):
            self.run_fake('early-exit')
        self.assertFalse(any(record['role'] == 'probe' for record in self.records()))
        self.assert_cleanup()

    def test_non_socket_and_symlink_are_not_readiness(self):
        for mode in ('regular-file', 'symlink'):
            with self.subTest(mode=mode):
                self.audit.unlink(missing_ok=True)
                with self.assertRaises(RuntimeError):
                    self.run_fake(mode)
                self.assertFalse(any(record['role'] == 'probe' for record in self.records()))
                self.assert_cleanup()

    def test_probe_launch_exception_cleans_already_started_compositors(self):
        with self.assertRaises(FileNotFoundError):
            self.run_fake(probe=str(self.root / 'missing-probe'))
        self.assert_cleanup()

    def test_cli_signal_runs_cleanup_before_exit(self):
        environment = dict(os.environ, FAKE_HANDOFF_AUDIT=str(self.audit), FAKE_HANDOFF_MODE='probe-hang',
                           FAKE_HANDOFF_DESCENDANTS='1')
        command = [sys.executable, '-B', str(Path(wrapper.__file__).resolve()),
                   str(self.root / 'weston'), str(self.root / 'probe'), str(self.root / 'ffmpeg')]
        process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            deadline = time.monotonic() + 4
            while not any(record['role'] == 'probe' for record in self.records()):
                self.assertIsNone(process.poll(), 'CLI wrapper ended before probe')
                self.assertLess(time.monotonic(), deadline, 'CLI wrapper never reached probe')
                time.sleep(0.02)
            process.send_signal(signal.SIGTERM)
            _, errors = process.communicate(timeout=4)
            self.assertEqual(process.returncode, 1, errors.decode(errors='replace'))
            self.assertIn(b'interrupted', errors)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.communicate(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill(); process.communicate(timeout=2)
        self.assert_cleanup()

    def test_deadlines_reject_unbounded_values_without_launching(self):
        for value in (0, -1, 58, float('inf'), float('nan')):
            with self.subTest(value=value), self.assertRaises(ValueError):
                wrapper.run('unused', 'unused', 'unused', wrapper.Limits(probe=value))
        self.assertEqual(self.records(), [])

    def test_permission_denial_for_a_live_group_is_not_ignored(self):
        processes = []
        process = wrapper.launch([sys.executable, '-c', 'import time; time.sleep(90)'],
                                 os.environ.copy(), subprocess.DEVNULL, subprocess.DEVNULL, processes)
        try:
            with mock.patch.object(wrapper.os, 'killpg', side_effect=PermissionError(1, 'explicit live denial')), \
                    mock.patch.object(wrapper.subprocess, 'run') as inspection:
                with self.assertRaises(PermissionError):
                    wrapper.signal_group(process, signal.SIGKILL)
                inspection.assert_not_called()
        finally:
            wrapper.close_owned(processes, self.limits)


class FixtureValidationTest(unittest.TestCase):
    @staticmethod
    def encoded(count=18, changed=None, sps=b'\x67\x42\x00\x1e', pps=b'\x68\x01', extra=b''):
        stream = bytearray()
        def append(nal):
            stream.extend(b'\0\0\0\1' + nal)
        for index in range(count):
            append(b'\x09\xf0')
            if index == 0:
                append(sps); append(pps)
            kind = changed.get(index) if changed and index in changed else (5 if index == 0 else 1)
            if kind is not None:
                append(bytes([0x60 | kind if kind in (5, 7, 8) else kind, 0x11]))
        stream.extend(extra)
        return bytes(stream)

    def test_one_initial_idr_and_seventeen_dependent_frames_are_accepted(self):
        result = packets(self.encoded())
        self.assertEqual(result[:5], b'UHVP\x01')

    def test_extra_idr_missing_frame_and_unrelated_nal_do_not_qualify(self):
        for encoded in (self.encoded(changed={7: 5}), self.encoded(changed={7: None}),
                        self.encoded(changed={7: 6}), self.encoded(changed={0: 1}),
                        self.encoded(count=17), self.encoded(count=19),
                        self.encoded(extra=b'\0\0\0\1\x0c\x11')):
            with self.subTest(encoded=encoded[-16:]), self.assertRaises(ValueError):
                packets(encoded)

    def test_malformed_oversized_and_duplicate_configuration_is_rejected(self):
        for encoded in (self.encoded(sps=b'\x67\x42'), self.encoded(sps=b'\x67\x64\0\x1e'),
                        self.encoded(pps=b'\x68'), self.encoded(sps=b'\x67\x42\0\x1e' + b'x' * 65532),
                        self.encoded(pps=b'\x68' + b'x' * 65535),
                        self.encoded(extra=b'\0\0\0\1\x68\x01'),
                        b'x' * (MAXIMUM_ENCODED_BYTES + 1), b'', b'not Annex B'):
            with self.subTest(size=len(encoded)), self.assertRaises(ValueError):
                packets(encoded)


if __name__ == '__main__':
    unittest.main()
