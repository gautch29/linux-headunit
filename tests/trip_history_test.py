#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual private persistence and supervised production-owner regressions.

Acquired values are labelled simulation; no vehicle/phone/UTC timing evidence.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import stat
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from trip_history import HistoryClient, PrivateStore, envelope, restore, validate_history, MAX_BYTES, MAX_IPC_BYTES, CLOCK
from sport_source import SportSource, host_now_ns
from sport_telemetry import SportEngine, Observation, NS

BOOT = '12345678-1234-1234-1234-123456789abc'


def archive():
    engine = SportEngine()
    base = 10 * NS
    for index in range(6):
        stamp = base + index * 200_000_000
        engine.accept(1, {'speed_kmh': Observation(36, stamp, origin='simulation')}, stamp)
    engine.reset_trip(base + NS + 1)
    client = HistoryClient('/unused', (os.getuid(), os.getgid()))
    client.boot = BOOT
    return client.histories(list(engine.trip.history))


def wait(predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.01)
    raise AssertionError('Production owner did not reach expected bounded state')


def service_stop_fixture(folder):
    """Actual owner process; test controller models cgroup delivery to both PIDs."""
    import threading
    stopped = threading.Event()
    def orderly_stop(sig, _):
        stopped.set()
        print(json.dumps({'signal': sig}), flush=True)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, orderly_stop)
    source = SportSource(folder, trip_history=True)
    source.start()
    wait(lambda: source.snapshot()['trip']['storage']['state'] == 'ready')
    with source.lock:
        now = host_now_ns()
        source.engine.accept(1, {'speed_kmh': Observation(36, now - 300_000_000, origin='simulation')}, now)
        source.engine.accept(1, {'speed_kmh': Observation(36, now - 100_000_000, origin='simulation')}, now)
    current = source.snapshot()['trip']
    wait(lambda: not source.snapshot()['trip']['storage']['pending'])
    pid = source.storage.process.pid
    os.kill(pid, signal.SIGSTOP)
    got, status = os.waitpid(pid, os.WUNTRACED)
    assert got == pid and os.WIFSTOPPED(status)
    # Observe the real OS wait calls made by production retirement, not PID absence.
    actual_waitpid = os.waitpid
    exits = []
    def observe_waitpid(*args):
        result = actual_waitpid(*args)
        if result[0] == pid and (os.WIFEXITED(result[1]) or os.WIFSIGNALED(result[1])):
            exits.append(result[1])
        return result
    try:
        from contextlib import ExitStack
        from functools import partial
        with ExitStack() as patches:
            patches.enter_context(patch('os.waitpid', observe_waitpid))
            # Popen.poll keeps a destructor-safe bound OS wait function.
            if hasattr(subprocess, '_del_safe'):
                patches.enter_context(patch.object(subprocess._del_safe, 'waitpid', observe_waitpid))
            else:
                patches.enter_context(patch.object(source.storage.process, '_internal_poll',
                                                   partial(source.storage.process._internal_poll, _waitpid=observe_waitpid)))
            print(json.dumps({'child': pid, 'current': current}), flush=True)
            assert stopped.wait(4)
            source.close()
        try:
            actual_waitpid(pid, os.WNOHANG)
            raise AssertionError('Production owner did not consume child wait status')
        except ChildProcessError:
            pass
        try:
            os.kill(pid, 0)
            raise AssertionError('Storage PID still exists after real wait')
        except ProcessLookupError:
            pass
        print(json.dumps({'storage': source.storage.status(), 'waitStatuses': exits,
                          'sourceAlive': source.thread.is_alive()}), flush=True)
    finally:
        source.close()


class HistoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name).resolve()
        self.folder.chmod(0o700)
        self.owner = (os.getuid(), os.getgid())
        self.clients = []
        self.sources = []

    def tearDown(self):
        for source in self.sources:
            source.close()
            if source.storage and source.storage.process:
                self.assertIsNotNone(source.storage.process.poll())
                self.assertTrue(source.storage.reaped)
        for client in self.clients:
            client.close()
            if client.process:
                self.assertIsNotNone(client.process.poll())
                self.assertTrue(client.reaped)
        self.temporary.cleanup()

    def client(self, folder=None):
        client = HistoryClient(folder or self.folder, self.owner)
        self.clients.append(client)
        client.start()
        return client

    def ready(self, client):
        wait(lambda: (client.tick(), client.state != 'starting')[1])
        self.assertEqual(client.state, 'ready', client.error)

    def save(self, client, records):
        client.offer(records)
        wait(lambda: (client.tick(), client.saved_revision == client.revision)[1])
        self.assertEqual(client.state, 'ready')

    def source(self, simulated=False):
        source = SportSource(self.folder, simulated=simulated, trip_history=True)
        self.sources.append(source)
        source.start()
        wait(lambda: source.snapshot()['trip']['storage']['state'] != 'starting')
        return source

    def feed(self, source):
        # Actual protocol clock; original delayed acquisitions, no clock patch.
        wait(lambda: host_now_ns() - 300_000_000 > source.engine.trip.reset_floor)
        with source.lock:
            now = host_now_ns()
            source.engine.accept(1, {'speed_kmh': Observation(36, now - 300_000_000, origin='simulation')}, now)
            source.engine.accept(1, {'speed_kmh': Observation(36, now - 100_000_000, origin='simulation')}, now)
        return source.snapshot()['trip']

    def test_real_atomic_private_file_restart_and_other_boot_is_history_only(self):
        client = self.client()
        self.ready(client)
        records = archive()
        self.save(client, records)
        path = self.folder / 'trip-history.json'
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(path.stat().st_nlink, 1)
        self.assertEqual(restore(path.read_bytes()), records)
        self.assertAlmostEqual(records[0]['distanceKm'], .01)
        self.assertEqual(records[0]['coveredSeconds'], 1)
        client.close()
        restored = self.source()
        trip = restored.snapshot()['trip']
        self.assertEqual(trip['history'], records)
        self.assertNotEqual(trip['storage']['hostBootId'], BOOT)
        self.assertEqual(trip['storage']['sourceClock'], CLOCK)
        for key in ('firstAcquiredNs', 'lastAcquiredNs', 'distanceKm', 'coveredSeconds', 'peak'):
            self.assertIsNone(trip[key])
        self.assertEqual(restored.engine.trip.acquisition_floor, 0)
        self.assertEqual(restored.engine.trip.reset_floor, 0)
        current = self.feed(restored)
        self.assertAlmostEqual(current['distanceKm'], .002)
        self.assertEqual(current['history'], records)
        current['history'][0]['peak']['speedKmh'] = 99
        self.assertEqual(restored.snapshot()['trip']['history'][0]['peak']['speedKmh'], 36)

    def test_reset_archive_strict_new_acquisition_and_independent_sport_reset(self):
        source = self.source()
        self.feed(source)
        reply = source.request({'action': 'trip_reset'})
        reset = reply['observedNs']
        self.assertIsNone(reply['trip']['distanceKm'])
        self.assertAlmostEqual(reply['trip']['history'][-1]['distanceKm'], .002)
        self.assertEqual(reply['trip']['history'][-1]['classification'], 'HISTORICAL')
        with source.lock:
            now = host_now_ns()
            source.engine.accept(1, {'speed_kmh': Observation(36, reset, origin='simulation')}, now)
        self.assertIsNone(source.snapshot()['trip']['peak'])
        with source.lock:
            now = host_now_ns()
            source.engine.accept(1, {'speed_kmh': Observation(36, now, origin='simulation')}, now)
        before = source.snapshot()['trip']
        source.request({'action': 'sport_session'})
        self.assertEqual(source.snapshot()['trip']['journeyId'], before['journeyId'])
        self.assertEqual(source.snapshot()['trip']['peak'], before['peak'])
        wait(lambda: source.storage.saved_revision > 1)
        persisted = restore((self.folder / 'trip-history.json').read_bytes())
        self.assertEqual(persisted[-1]['lastAcquiredNs'], reply['trip']['history'][-1]['lastAcquiredNs'])

    def test_actual_spawn_masks_pending_soft_signals_preserves_thread_mask_and_closes_fds(self):
        import threading
        import trip_history
        import fcntl
        read_fd, write_fd = os.pipe()
        inherited_fd = fcntl.fcntl(read_fd, fcntl.F_DUPFD, 200)
        os.set_inheritable(inherited_fd, True)
        before_main = signal.pthread_sigmask(signal.SIG_BLOCK, [])
        client = HistoryClient(self.folder, self.owner)
        self.clients.append(client)
        observed = []
        failures = []
        spawn_done = threading.Event()
        owner_release = threading.Event()
        real_spawn = subprocess.Popen
        probe = self.folder / 'startup-probe.json'
        from textwrap import dedent
        script = dedent("""\
            import json
            import os
            import signal
            import sys
            sys.path.insert(0, sys.argv[1])
            import trip_history as history
            os.kill(os.getpid(), signal.SIGSTOP)
            startup_pending = sorted(int(sig) for sig in signal.sigpending())
            write = history.write_frame
            def observe(stream, data):
                if data.get('kind') == 'ready':
                    try:
                        os.fstat(int(sys.argv[7]))
                        fd_closed = False
                    except OSError:
                        fd_closed = True
                    mask = signal.pthread_sigmask(signal.SIG_BLOCK, [])
                    result = {
                        'mask': sorted(int(sig) for sig in mask),
                        'ignored': [int(sig) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
                                    if signal.getsignal(sig) == signal.SIG_IGN],
                        'fdClosed': fd_closed,
                        'startupPending': startup_pending,
                    }
                    with open(sys.argv[2], 'w') as output:
                        json.dump(result, output)
                return write(stream, data)
            history.write_frame = observe
            sys.exit(history.worker(sys.argv[3], (int(sys.argv[4]), int(sys.argv[5])), int(sys.argv[6])))
            """)
        def interrupt_exec(args, **kwargs):
            observed.append(set(signal.pthread_sigmask(signal.SIG_BLOCK, [])))
            # Wrap only observation at production ready; actual worker/IPC/files are unchanged.
            process = real_spawn([sys.executable, '-B', '-c', script,
                                  str(Path(trip_history.__file__).parent), str(probe), *args[-4:], str(inherited_fd)], **kwargs)
            try:
                got, status = os.waitpid(process.pid, os.WUNTRACED)
                self.assertEqual(got, process.pid)
                self.assertTrue(os.WIFSTOPPED(status))
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                    os.kill(process.pid, sig)
                os.kill(process.pid, signal.SIGCONT)
                return process
            except BaseException:
                process.kill()
                process.wait(timeout=2)
                process.stdin.close()
                process.stdout.close()
                raise
        def owner_thread():
            original = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGUSR1, signal.SIGINT})
            expected = signal.pthread_sigmask(signal.SIG_BLOCK, [])
            try:
                with patch('trip_history.subprocess.Popen', interrupt_exec):
                    client.start()
                if signal.pthread_sigmask(signal.SIG_BLOCK, []) != expected:
                    raise AssertionError('Spawn changed the original caller thread mask')
            except BaseException as error:
                failures.append(error)
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, original)
                spawn_done.set()
                # Linux PDEATHSIG watches the actual spawning thread. This
                # test owner must stay alive until its real child is retired.
                if not owner_release.wait(5):
                    failures.append(AssertionError('Fixture spawning owner was not retired'))
        try:
            thread = threading.Thread(target=owner_thread)
            thread.start()
            self.assertTrue(spawn_done.wait(3))
            self.assertTrue(thread.is_alive())
            self.assertFalse(failures, failures)
            self.assertEqual(signal.pthread_sigmask(signal.SIG_BLOCK, []), before_main)
            self.assertEqual(len(observed), 1)
            self.assertTrue({signal.SIGUSR1, signal.SIGINT, signal.SIGTERM, signal.SIGHUP} <= observed[0])
            self.ready(client)
            result = json.loads(probe.read_text())
            self.assertEqual(set(result['mask']), {int(sig) for sig in observed[0]} -
                             {int(signal.SIGINT), int(signal.SIGTERM), int(signal.SIGHUP)})
            self.assertIn(int(signal.SIGUSR1), result['mask'])
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                self.assertIn(int(sig), result['startupPending'])
                self.assertNotIn(int(sig), result['mask'])
                self.assertIn(int(sig), result['ignored'])
                # Future service signals as well as startup pending signals are ignored only by child.
                os.kill(client.process.pid, sig)
            self.assertTrue(result['fdClosed'])
            self.save(client, archive())
            client.close()
            self.assertEqual(client.process.returncode, 0)
            self.assertTrue(client.process.stdin.closed)
            self.assertTrue(client.process.stdout.closed)
            self.assertTrue(client.reaped)
        finally:
            client.close()
            owner_release.set()
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
            self.assertFalse(failures, failures)
            os.close(inherited_fd)
            os.close(read_fd)
            os.close(write_fd)

    def test_failed_spawn_restores_all_original_mask_bits_and_keeps_parent_handlers(self):
        original = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGUSR1, signal.SIGHUP})
        expected = signal.pthread_sigmask(signal.SIG_BLOCK, [])
        handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        client = HistoryClient(self.folder, self.owner)
        real_spawn = subprocess.Popen
        fd_directory = '/proc/self/fd' if sys.platform.startswith('linux') else '/dev/fd'
        descriptors_before = set(os.listdir(fd_directory))
        def failed_spawn(*args, **kwargs):
            self.assertTrue(set(handlers) <= signal.pthread_sigmask(signal.SIG_BLOCK, []))
            return real_spawn(*args, **kwargs)
        try:
            with patch('trip_history.subprocess.Popen', failed_spawn), \
                    patch('trip_history.sys.executable', str(self.folder / 'missing-interpreter')):
                with self.assertRaises(FileNotFoundError):
                    client.start()
            self.assertEqual(set(os.listdir(fd_directory)), descriptors_before)
            self.assertEqual(signal.pthread_sigmask(signal.SIG_BLOCK, []), expected)
            self.assertEqual({sig: signal.getsignal(sig) for sig in handlers}, handlers)
            self.assertIsNone(client.process)
            client.close()
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, original)

    def test_actual_service_group_soft_signals_preserve_final_current_journey(self):
        import select
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            with self.subTest(signal=sig):
                folder = self.folder / str(sig)
                folder.mkdir(mode=0o700)
                parent = subprocess.Popen([sys.executable, '-B', str(Path(__file__).resolve()),
                                           '--service-stop-fixture', str(folder)],
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                pid = None
                try:
                    self.assertTrue(select.select([parent.stdout], [], [], 4)[0])
                    ready = json.loads(parent.stdout.readline())
                    pid = ready['child']
                    os.kill(pid, sig)
                    os.kill(parent.pid, sig)
                    self.assertTrue(select.select([parent.stdout], [], [], 2)[0])
                    self.assertEqual(json.loads(parent.stdout.readline()), {'signal': sig})
                    os.kill(pid, signal.SIGCONT)
                    output, error = parent.communicate(timeout=4)
                    self.assertEqual(parent.returncode, 0, error)
                    retired = json.loads(output)
                    self.assertFalse(retired['sourceAlive'])
                    self.assertTrue(retired['storage']['reaped'])
                    self.assertEqual(len(retired['waitStatuses']), 1)
                    records = restore((folder / 'trip-history.json').read_bytes())
                    print(json.dumps({'serviceSignal': int(sig), 'archivedRecords': len(records),
                                      'retirement': retired}), flush=True)
                    self.assertEqual(len(records), 1, 'Normal service stop lost the final current journey')
                    self.assertEqual(os.waitstatus_to_exitcode(retired['waitStatuses'][0]), 0)
                    self.assertEqual(retired['storage']['state'], 'retired')
                    self.assertEqual(records[-1]['journeyId'], ready['current']['journeyId'])
                    self.assertEqual(records[-1]['lastAcquiredNs'], ready['current']['lastAcquiredNs'])
                    self.assertAlmostEqual(records[-1]['distanceKm'], .002)
                    self.assertEqual(records[-1]['endedReason'], 'Sport owner retired')
                finally:
                    if pid is not None:
                        try:
                            os.kill(pid, signal.SIGCONT)
                        except ProcessLookupError:
                            pass
                    if parent.poll() is None:
                        parent.terminate()
                        try:
                            parent.wait(timeout=4)
                        except subprocess.TimeoutExpired:
                            parent.kill()
                            parent.wait(timeout=2)
                    parent.stdout.close()
                    parent.stderr.close()

    def test_orderly_retirement_archives_once_reaps_and_never_revives(self):
        source = self.source()
        self.feed(source)
        source.close()
        self.assertFalse(source.thread.is_alive())
        self.assertTrue(source.storage.reaped)
        records = restore((self.folder / 'trip-history.json').read_bytes())
        self.assertEqual(records[-1]['endedReason'], 'Sport owner retired')
        self.assertAlmostEqual(records[-1]['distanceKm'], .002)
        source.close()
        self.assertEqual(restore((self.folder / 'trip-history.json').read_bytes()), records)
        with self.assertRaises(ValueError):
            source.request({'action': 'trip_reset'})
        with self.assertRaises(ValueError):
            source.start()
        successor = self.source()
        self.assertEqual(successor.snapshot()['trip']['history'], records)
        self.assertIsNone(successor.snapshot()['trip']['peak'])
        self.assertIsNone(successor.snapshot()['inputs']['speed_kmh']['value'])

    def test_history_bound_keeps_latest_original_journeys_across_restart(self):
        source = self.source()
        for _ in range(22):
            with source.lock:
                now = host_now_ns()
                source.engine.accept(1, {'speed_kmh': Observation(36, now, origin='simulation')}, now)
            source.request({'action': 'trip_reset'})
        source.close()
        records = restore((self.folder / 'trip-history.json').read_bytes())
        self.assertEqual(len(records), 16)
        self.assertEqual(records[0]['journeyId'], 7)
        self.assertEqual(records[-1]['journeyId'], 22)
        self.assertLess((self.folder / 'trip-history.json').stat().st_size, MAX_BYTES)
        successor = self.source()
        self.assertEqual(successor.snapshot()['trip']['history'], records)
        with successor.lock:
            now = host_now_ns()
            successor.engine.accept(1, {'speed_kmh': Observation(72, now, origin='simulation')}, now)
        successor.request({'action': 'trip_reset'})
        successor.close()
        combined = restore((self.folder / 'trip-history.json').read_bytes())
        self.assertEqual(len(combined), 16)
        self.assertEqual(combined[-1]['journeyId'], 1)
        self.assertNotEqual(combined[-1]['ownerId'], records[-1]['ownerId'])
        self.assertEqual(combined[-2], records[-1])

    def test_unknown_covered_interval_gap_and_known_stopped_archive_remain_distinct(self):
        engine = SportEngine()
        base = 10 * NS
        engine.accept(1, {'speed_kmh': Observation(36, base, origin='simulation')}, base)
        engine.accept(1, {'speed_kmh': Observation(36, base + 400_000_000, origin='simulation')}, base + 400_000_000)
        engine.reset_trip(base + 410_000_000)
        engine.accept(1, {'speed_kmh': Observation(0, base + 500_000_000, origin='simulation')}, base + 500_000_000)
        engine.accept(1, {'speed_kmh': Observation(0, base + 700_000_000, origin='simulation')}, base + 700_000_000)
        engine.reset_trip(base + 710_000_000)
        client = self.client()
        self.ready(client)
        records = client.histories(list(engine.trip.history))
        self.save(client, records)
        loaded = restore((self.folder / 'trip-history.json').read_bytes())
        self.assertIsNone(loaded[0]['distanceKm'])
        self.assertIsNone(loaded[0]['coveredSeconds'])
        self.assertEqual(loaded[0]['coverageRatio'], 0)
        self.assertTrue(loaded[0]['partial'])
        self.assertEqual(loaded[1]['distanceKm'], 0)
        self.assertEqual(loaded[1]['averageSpeedKmh'], 0)
        self.assertIsNone(loaded[1]['movingAverageSpeedKmh'])
        self.assertEqual(loaded[1]['coveredSeconds'], .2)

    def test_production_maximum_speed_estimates_round_trip_without_changing_numbers(self):
        client = self.client()
        self.ready(client)
        for count in (3, 5, 17, 51, 201, 501, 1001):
            engine = SportEngine()
            base = 10 * NS
            for index in range(count):
                stamp = base + index * 200_000_000
                engine.accept(1, {'speed_kmh': Observation(300, stamp, origin='simulation')}, stamp)
            engine.reset_trip(stamp + 1)
            records = client.histories(list(engine.trip.history))
            self.assertAlmostEqual(records[0]['averageSpeedKmh'], 300)
            self.save(client, records)
            self.assertEqual(restore((self.folder / 'trip-history.json').read_bytes()), records)

    def test_corrupt_incompatible_hash_duplicate_and_excess_bytes_do_not_overwrite(self):
        valid = envelope(archive())
        body = json.loads(valid)
        incompatible = dict(body, version=2)
        invalid = [b'{', valid.replace(b'36.0', b'37.0', 1), json.dumps(incompatible).encode(),
                   b'{"version":1,"version":1}', b' ' * (MAX_BYTES + 1),
                   b'{"history":NaN}', b'[' * 9 + b']' * 9]
        for raw in invalid:
            with self.subTest(rawKind=len(raw)):
                path = self.folder / 'trip-history.json'
                path.write_bytes(raw)
                path.chmod(0o600)
                source = self.source()
                wait(lambda: source.snapshot()['trip']['storage']['state'] == 'failed')
                current = self.feed(source)
                self.assertAlmostEqual(current['distanceKm'], .002)
                self.assertEqual(current['history'], [])
                source.request({'action': 'trip_reset'})
                source.close()
                self.assertEqual(path.read_bytes(), raw)

    def test_schema_numbers_dates_units_scope_and_coverage_rejected_with_valid_hash(self):
        records = archive()
        mutations = [('coveredSeconds', 2), ('movingSeconds', 2), ('distanceKm', -1),
                     ('averageSpeedKmh', 99), ('averageSpeedKmh', 300.000001),
                     ('movingAverageSpeedKmh', 300.000001), ('coverageRatio', .4), ('uncoveredSeconds', 2),
                     ('origin', 'phone'), ('hostBootId', 'unknown'), ('sourceClock', 'UTC'),
                     ('classification', 'LIVE'), ('journeyId', True), ('sourceGeneration', 0),
                     ('firstAcquiredNs', 0), ('archivedAtNs', 1), ('partial', 1),
                     ('units', {'distance': 'miles'}), ('maximumGapNs', 999),
                     ('movingThresholdKmh', 0), ('distanceKm', float('inf'))]
        for key, value in mutations:
            altered = copy.deepcopy(records)
            altered[0][key] = value
            with self.subTest(field=key, value=value), self.assertRaises(ValueError):
                validate_history(altered)
        with self.assertRaises(ValueError):
            validate_history(records * 2)
        with self.assertRaises(ValueError):
            validate_history(records * 17)
        raw = json.loads(envelope(records))
        raw['history'][0]['coveredSeconds'] = 2
        body = {'version': 1, 'history': raw['history']}
        raw['sha256'] = hashlib.sha256(json.dumps(body, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        with self.assertRaises(ValueError):
            restore(json.dumps(raw).encode())

    def test_link_fifo_shared_modes_and_shared_owner_reject_without_replacing(self):
        for kind in ('symlink', 'hardlink', 'fifo', 'shared-mode'):
            with self.subTest(kind=kind):
                path = self.folder / 'trip-history.json'
                target = self.folder / 'target'
                target.write_bytes(envelope(archive()))
                target.chmod(0o600)
                if kind == 'symlink':
                    path.symlink_to(target)
                elif kind == 'hardlink':
                    os.link(target, path)
                elif kind == 'fifo':
                    os.mkfifo(path, 0o600)
                else:
                    path.write_bytes(target.read_bytes())
                    path.chmod(0o644)
                client = self.client()
                wait(lambda: (client.tick(), client.state == 'failed')[1])
                client.close()
                self.assertTrue(path.is_symlink() if kind == 'symlink' else path.exists())
                path.unlink()
                target.unlink()
        first = self.client()
        self.ready(first)
        self.save(first, archive())
        raw = (self.folder / 'trip-history.json').read_bytes()
        second = self.client()
        wait(lambda: (second.tick(), second.state == 'failed')[1])
        self.assertEqual((self.folder / 'trip-history.json').read_bytes(), raw)
        second.close()
        self.save(first, archive())

    def test_canonical_directory_lock_replacement_and_directory_replacement(self):
        link = self.folder / 'link'
        private = self.folder / 'real'
        private.mkdir(mode=0o700)
        link.symlink_to(private, target_is_directory=True)
        client = self.client(link)
        wait(lambda: (client.tick(), client.state == 'failed')[1])
        client.close()
        self.assertFalse((private / 'trip-history.json').exists())
        client = self.client()
        self.ready(client)
        self.save(client, archive())
        lock = self.folder / '.trip-history.lock'
        lock.rename(self.folder / '.old-lock')
        lock.write_bytes(b'')
        lock.chmod(0o600)
        client.offer(archive())
        wait(lambda: (client.tick(), client.state == 'failed')[1])
        client.close()
        lock.unlink()
        (self.folder / '.old-lock').rename(lock)
        client = self.client()
        self.ready(client)
        old = self.folder / 'original'
        old.mkdir(mode=0o700)
        # Move the pinned directory itself; its old pathname becomes another inode.
        renamed = self.folder.with_name(self.folder.name + '-moved')
        self.folder.rename(renamed)
        self.folder.mkdir(mode=0o700)
        try:
            client.offer(archive())
            wait(lambda: (client.tick(), client.state == 'failed')[1])
            client.close()
            self.assertFalse((self.folder / 'trip-history.json').exists())
        finally:
            self.folder.rmdir()
            renamed.rename(self.folder)

    def test_failed_atomic_write_leaves_original_and_removes_own_temporary(self):
        records = archive()
        store = PrivateStore(self.folder, self.owner)
        try:
            store.save(records)
            original = (self.folder / 'trip-history.json').read_bytes()
            with patch('trip_history.os.fsync', side_effect=OSError('Disk failure')):
                with self.assertRaises(OSError):
                    store.save(archive())
            self.assertEqual((self.folder / 'trip-history.json').read_bytes(), original)
            self.assertEqual(list(self.folder.glob('.trip-history-*')), [])
            # Rename committed but directory sync fails: no successful receipt.
            real_fsync = os.fsync
            def fsync(fd):
                if stat.S_ISDIR(os.fstat(fd).st_mode):
                    raise OSError('Directory sync failure')
                real_fsync(fd)
            with patch('trip_history.os.fsync', side_effect=fsync):
                with self.assertRaises(OSError):
                    store.save(records)
            self.assertEqual(restore((self.folder / 'trip-history.json').read_bytes()), records)
        finally:
            store.close()

    def test_actual_short_file_write_failure_retains_original_and_cleans_temporary(self):
        records = archive()
        store = PrivateStore(self.folder, self.owner)
        store.save(records)
        store.close()
        original = (self.folder / 'trip-history.json').read_bytes()
        larger = records + archive()
        self.assertGreater(len(envelope(larger)), 1024)
        script = ("import sys,os,resource,signal;sys.path.insert(0,sys.argv[1]);"
                  "from trip_history import PrivateStore,restore;"
                  "records=restore(open(sys.argv[3],'rb').read());"
                  "store=PrivateStore(sys.argv[2],(os.getuid(),os.getgid()));"
                  "resource.setrlimit(resource.RLIMIT_FSIZE,(1024,1024));"
                  "signal.signal(signal.SIGXFSZ,signal.SIG_IGN)\n"
                  "try:store.save(records)\n"
                  "except OSError:pass\n"
                  "else:raise AssertionError('Actual file-size write failure not surfaced')\n"
                  "finally:store.close()")
        request = self.folder / 'bounded-write-fixture.json'
        request.write_bytes(envelope(larger))
        result = subprocess.run([sys.executable, '-B', '-c', script,
                                 str(Path(__file__).resolve().parents[1] / 'tools'),
                                 str(self.folder), str(request)], capture_output=True, timeout=4)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual((self.folder / 'trip-history.json').read_bytes(), original)
        self.assertEqual(list(self.folder.glob('.trip-history-*')), [])

    def test_real_file_replacement_during_read_rejects_and_retains_unknown_live_state(self):
        records = archive()
        store = PrivateStore(self.folder, self.owner)
        try:
            store.save(records)
            real_read = os.read
            def read(fd, count):
                raw = real_read(fd, count)
                path = self.folder / 'trip-history.json'
                replacement = self.folder / 'replacement'
                replacement.write_bytes(path.read_bytes())
                replacement.chmod(0o600)
                os.replace(replacement, path)
                return raw
            with patch('trip_history.os.read', side_effect=read):
                with self.assertRaises(ValueError):
                    store.load()
            self.assertEqual(store.load(), records)
        finally:
            store.close()

    def test_actual_failed_child_write_preserves_live_statistics_and_reaps(self):
        source = self.source()
        self.feed(source)
        target = self.folder / 'trip-history.json'
        wait(lambda: target.exists())
        original = target.read_bytes()
        target.unlink()
        os.mkfifo(target, 0o600)
        reply = source.request({'action': 'trip_reset'})
        wait(lambda: source.snapshot()['trip']['storage']['state'] == 'failed')
        self.assertAlmostEqual(reply['trip']['history'][-1]['distanceKm'], .002)
        self.assertEqual(source.snapshot()['trip']['history'][-1]['distanceKm'], reply['trip']['history'][-1]['distanceKm'])
        self.assertTrue(stat.S_ISFIFO(target.stat().st_mode))
        self.assertEqual(restore(original), [])
        self.feed(source)
        self.assertAlmostEqual(source.snapshot()['trip']['distanceKm'], .002)
        source.close()
        self.assertTrue(source.storage.reaped)

    def test_stopped_storage_is_coalesced_and_acquisition_keeps_advancing(self):
        source = self.source(simulated=True)
        pid = source.storage.process.pid
        os.kill(pid, signal.SIGSTOP)
        try:
            first = source.snapshot()['trip']['lastAcquiredNs']
            source.request({'action': 'trip_reset'})
            source.request({'action': 'trip_reset'})
            wait(lambda: source.snapshot()['trip']['lastAcquiredNs'] is not None and source.snapshot()['trip']['lastAcquiredNs'] > first)
            for _ in range(50):
                source.request({'action': 'trip_reset'})
            with source.lock:
                source._storage_tick()
                self.assertIsNotNone(source.storage.inflight)
                self.assertIsNotNone(source.storage.pending)
                self.assertLessEqual(len(source.storage.output), MAX_IPC_BYTES + 4)
            wait(lambda: source.snapshot()['trip']['storage']['state'] == 'failed', timeout=4)
            self.assertIsNotNone(source.snapshot()['trip']['lastAcquiredNs'])
            source.close()
            self.assertTrue(source.storage.reaped)
        finally:
            if source.storage.process.poll() is None:
                os.kill(pid, signal.SIGCONT)

    def test_opt_in_child_retains_existing_sport_line_and_session_schema(self):
        source = self.source()
        source.request({'action': 'sport_line', 'line': [[48, 1.9995], [48, 2.0005]]})
        session = source.snapshot()['sessionId']
        self.feed(source)
        source.request({'action': 'trip_reset'})
        original_trip = source.snapshot()['trip']['history']
        source.request({'action': 'sport_session'})
        source.close()
        sport = json.loads((self.folder / 'sport-history.json').read_bytes())
        self.assertEqual(set(sport), {'version', 'sessionId', 'history', 'line'})
        self.assertGreater(sport['sessionId'], session)
        successor = self.source()
        model = successor.snapshot()
        self.assertEqual(model['laps']['line'], sport['line'])
        self.assertEqual(model['sessionId'], sport['sessionId'] + 1)
        self.assertEqual(model['trip']['history'], original_trip)
        self.assertIsNone(model['trip']['peak'])
        self.assertIsNone(model['inputs']['speed_kmh']['value'])

    def test_actual_manager_constructor_status_actions_and_shutdown(self):
        from phone_manager import Manager
        class Phones:
            def phone(self, phone):
                return {}
            def phones(self):
                return []
            def media(self, phone):
                return {'available': False}
        display = self.folder / 'standard.conf'
        display.write_text('explicit geometry fixture')
        config = self.folder / 'receiver.json'
        config.write_text(json.dumps({'pair_store': str(self.folder / 'pairs'),
                                      'display_config': str(display),
                                      'sport_telemetry': True, 'trip_history': True}))
        manager = Manager(config, self.folder, display, self.owner, bluez=Phones())
        self.sources.append(manager.sport)
        try:
            wait(lambda: manager.request({'action': 'status'})['sport']['trip']['storage']['state'] == 'ready')
            self.feed(manager.sport)
            reply = manager.request({'action': 'trip_reset'})['sport']['trip']
            self.assertAlmostEqual(reply['history'][-1]['distanceKm'], .002)
            self.assertTrue(reply['storage']['pending'])
            for data in ({'action': 'trip_reset', 'phone': 'fixture'},
                         {'action': 'trip_reset', 'line': []}):
                with self.assertRaises(ValueError):
                    manager.request(data)
        finally:
            manager.shutdown()
        self.assertTrue(manager.sport.storage.reaped)
        self.assertEqual(len(restore((self.folder / 'trip-history.json').read_bytes())), 1)

    def test_terminal_acquisition_retires_storage_and_keeps_original_statistics(self):
        from sport_telemetry_test import packet
        source = SportSource(self.folder, socket_path=self.folder / 'speed.sock', trip_history=True)
        self.sources.append(source)
        source.start()
        wait(lambda: source.snapshot()['trip']['storage']['state'] == 'ready')
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
            acquired = host_now_ns()
            sender.sendto(packet(acquired, value='36'), str(source.path))
            wait(lambda: source.snapshot()['trip']['firstAcquiredNs'] == acquired)
            with source.lock:
                source.socket.close()
            source.thread.join(timeout=3)
        self.assertFalse(source.thread.is_alive())
        model = source.snapshot()
        self.assertEqual(model['error'], 'Sport acquisition stopped')
        self.assertEqual(model['trip']['lastAcquiredNs'], acquired)
        self.assertEqual(model['trip']['phase'], 'waiting')
        self.assertTrue(model['trip']['storage']['reaped'])
        self.assertEqual(model['trip']['storage']['state'], 'failed')
        self.assertTrue(model['trip']['storage']['error'])
        self.assertIsNone(model['inputs']['speed_kmh']['value'])
        source.close()
        self.assertFalse(source.path.exists())

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Actual Linux parent-death kernel contract')
    def test_linux_actual_supervisor_death_kills_stopped_child_and_reaps(self):
        import ctypes
        libc = ctypes.CDLL(None, use_errno=True)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)  # Test becomes orphan reaper.
        owner = self.owner
        if os.geteuid() == 0:
            import pwd
            account = pwd.getpwnam('nobody')
            owner = (account.pw_uid, account.pw_gid)
            os.chown(self.folder, *owner)
        script = ("import sys,time;sys.path.insert(0,sys.argv[1]);"
                  "from trip_history import HistoryClient;import os;"
                  "c=HistoryClient(sys.argv[2],(int(sys.argv[3]),int(sys.argv[4])));c.start();"
                  "deadline=time.monotonic()+3\n"
                  "while c.state=='starting' and time.monotonic()<deadline:c.tick();time.sleep(.01)\n"
                  "assert c.state=='ready';print(c.process.pid,flush=True);time.sleep(30)")
        parent = subprocess.Popen([sys.executable, '-B', '-c', script,
                                   str(Path(__file__).resolve().parents[1] / 'tools'), str(self.folder), str(owner[0]), str(owner[1])],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        pid = None
        try:
            import select
            self.assertTrue(select.select([parent.stdout], [], [], 4)[0])
            pid = int(parent.stdout.readline())
            os.kill(pid, signal.SIGSTOP)
            parent.kill()
            parent.wait(timeout=2)
            outcome = []
            def reaped():
                got, status = os.waitpid(pid, os.WNOHANG)
                if got:
                    outcome.append(status)
                    return True
                return False
            wait(reaped)
            self.assertEqual(os.waitstatus_to_exitcode(outcome[0]), -signal.SIGKILL)
            successor = HistoryClient(self.folder, owner)
            self.clients.append(successor)
            successor.start()
            self.ready(successor)  # The real lock retired with the dead display child.
        finally:
            if parent.poll() is None:
                parent.kill()
                parent.wait(timeout=2)
            parent.stdout.close()
            parent.stderr.close()
            if pid is not None:
                try:
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
                except (ProcessLookupError, ChildProcessError):
                    pass
            self.assertEqual(libc.prctl(36, 0, 0, 0, 0), 0)

    @unittest.skipUnless(os.geteuid() == 0, 'Actual root-manager display-account privilege drop')
    def test_actual_root_storage_child_uses_display_identity(self):
        import pwd
        account = pwd.getpwnam('nobody')
        os.chown(self.folder, account.pw_uid, account.pw_gid)
        client = HistoryClient(self.folder, (account.pw_uid, account.pw_gid))
        self.clients.append(client)
        client.start()
        self.ready(client)
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            os.kill(client.process.pid, sig)
        client.offer(archive(), {'version': 1, 'sessionId': 1, 'history': [], 'line': None})
        wait(lambda: (client.tick(), client.saved_revision == client.revision)[1])
        self.assertEqual(client.state, 'ready')
        for name in ('.trip-history.lock', 'trip-history.json', 'sport-history.json'):
            info = (self.folder / name).stat()
            self.assertEqual((info.st_uid, info.st_gid), (account.pw_uid, account.pw_gid))
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
        client.close()

    def test_disabled_inventory_has_no_new_process_or_file(self):
        source = SportSource(self.folder)
        self.sources.append(source)
        self.assertIsNone(source.storage)
        self.assertEqual(source.snapshot()['trip']['persistence'], 'process-memory')
        self.assertNotIn('storage', source.snapshot()['trip'])
        self.assertFalse((self.folder / '.trip-history.lock').exists())


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--service-stop-fixture':
        service_stop_fixture(sys.argv[2])
    else:
        unittest.main()
