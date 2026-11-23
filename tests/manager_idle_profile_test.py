# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Finite profiling recipe with actual Manager/IPC/sport lifecycle, no hardware."""
import copy
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/profile_manager_idle.py'
sys.path.insert(0, str(ROOT / 'tools'))
import profile_manager_idle as profiler
EVIDENCE = None
if '--evidence' in sys.argv:
    at = sys.argv.index('--evidence')
    EVIDENCE = Path(sys.argv[at + 1]).resolve()
    del sys.argv[at:at + 2]
NATIVE = None
if '--native-fixture' in sys.argv:
    at = sys.argv.index('--native-fixture')
    NATIVE = Path(sys.argv[at + 1]).resolve()
    del sys.argv[at:at + 2]


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='idle-recipe-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = EVIDENCE / self._testMethodName if EVIDENCE else Path(self.temp.name)
        if EVIDENCE:
            self.root.mkdir(parents=True, exist_ok=False)
        self.children = []
        self.addCleanup(self.retire)

    def retire(self):
        for child in self.children:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=12)

    def launch(self, output, *options):
        argv = [sys.executable, '-B', str(TOOL), '--output', str(output), *options]
        if NATIVE:
            argv += ['--native-fixture', str(NATIVE)]
        child = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.children.append(child)
        return child

    def verify_retirement(self, output):
        receipt = json.loads((output / 'receipt.json').read_text())
        worker = json.loads((output / 'worker.json').read_text())
        self.assertTrue(receipt['fixtureWaited'])
        self.assertTrue(receipt['fixtureDirectoryAbsent'])
        self.assertTrue(worker['retirement']['managerSocketAbsent'])
        self.assertTrue(worker['retirement']['nativeSocketAbsent'])
        if worker['workload'] == 'configured':
            self.assertTrue(worker['retirement']['sportJoined'])
        if NATIVE:
            self.assertTrue(worker['retirement']['nativeWaited'])
            self.assertTrue(worker['retirement']['nativeValuesWithdrawn'])
        pid = json.loads((output / 'ready.json').read_text())['pid']
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
        return receipt, worker

    def verify_mapping(self, worker):
        owners = worker['threadOwners']
        self.assertIn('no equivalence to V87 TID 980', owners['scope'])
        self.assertIn('not vehicle acquisition', owners['clock'])
        main = owners['main']
        self.assertTrue(profiler.same_owner(main['before'], main['after']))
        self.assertEqual(main['independentCpu']['cpuNs'], worker['mainThreadCpuNs'])
        self.assertEqual(main['independentCpu']['observedStartNs'], worker['observedStartNs'])
        for role in ('main', 'sport'):
            mapping = owners[role]
            if role == 'sport' and worker['workload'] == 'sparse':
                self.assertIsNone(mapping['before']['nativeId'])
                self.assertIsNone(mapping['independentCpu'])
                self.assertIsNone(mapping['kernelCpuInterval'])
                continue
            self.assertTrue(profiler.same_owner(mapping['before'], mapping['after']))
            if worker['procThreadsBefore'] is None:
                self.assertIsNone(mapping['kernelCpuBefore'])
                self.assertIsNone(mapping['kernelCpuInterval'])
            else:
                self.assertIsNotNone(mapping['kernelCpuInterval'])
                for stage, counters in (('before', 'procThreadsBefore'), ('after', 'procThreadsAfter')):
                    identity = mapping[stage]['kernelIdentity']
                    self.assertEqual(identity['processId'], worker['pid'])
                    row = worker[counters]['threads'][str(mapping[stage]['nativeId'])]
                    self.assertEqual(identity['threadStartTicks'], row['startTicks'])
                    self.assertEqual(row, mapping['kernelCpu' + stage.title()])
            if role == 'sport':
                lifetime = mapping['independentCpu']
                self.assertTrue(lifetime['actualSourceThread'])
                self.assertEqual(lifetime['ownerBefore']['objectId'], mapping['before']['objectId'])
                self.assertEqual(lifetime['ownerBefore']['nativeId'], mapping['before']['nativeId'])
                self.assertEqual(lifetime['threadCpuNs'], worker['sportWorker']['threadCpuNs'])
                self.assertIn('including retirement', lifetime['scope'])
                self.assertLessEqual(lifetime['observedStartNs'], worker['observedStartNs'])
                self.assertGreaterEqual(lifetime['observedEndNs'], worker['observedEndNs'])
                self.assertFalse(mapping['afterShutdown']['alive'])
                self.assertIsNone(mapping['afterShutdown']['nativeId'])
                self.assertIsNone(mapping['afterShutdown']['kernelIdentity'])

    def test_real_workloads_independent_cpu_and_profile_evidence(self):
        for workload, mode in (('sparse', 'cpu'), ('configured', 'cpu'), ('configured', 'profile')):
            with self.subTest(workload=workload, mode=mode):
                output = self.root / (workload + '-' + mode)
                child = self.launch(output, '--seconds', '1', '--workload', workload,
                                    '--mode', mode)
                _, error = child.communicate(timeout=15)
                self.assertEqual(child.returncode, 0, error.decode())
                receipt, worker = self.verify_retirement(output)
                self.assertTrue(receipt['ownerRefused'])
                self.assertTrue(receipt['invalidActionRefused'])
                self.assertGreaterEqual(receipt['statusReplies'], 1)
                self.assertGreater(worker['wallNs'], 0)
                self.assertGreater(worker['mainThreadCpuNs'], 0)
                self.assertGreater(worker['processCpuNs'], 0)
                self.assertTrue(worker['completed'])
                self.verify_mapping(worker)
                self.assertTrue(worker['sourceUnchanged'])
                self.assertIn('tools/phone_manager.py', worker['sourceSha256'])
                self.assertEqual((output / 'workload.prof').exists(), mode == 'profile')
                if mode == 'profile':
                    self.assertIn('no thread isolation', worker['profileScope'])
                if NATIVE:
                    self.assertTrue(worker['native']['live'])
                    self.assertGreater(int(worker['native']['sequence']), 0)

    def test_int_term_hup_repeated_signals_wait_and_withdraw_actual_owners(self):
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=sig):
                output = self.root / ('signal-' + str(sig))
                child = self.launch(output, '--seconds', '30')
                deadline = time.monotonic() + 10
                while not (output / 'ready.json').exists():
                    self.assertIsNone(child.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.01)
                # Allow a genuine serving/tick interval; no artificial owner clock.
                time.sleep(.15)
                child.send_signal(sig)
                child.send_signal(sig)
                _, error = child.communicate(timeout=12)
                self.assertEqual(child.returncode, 128 + sig, error.decode())
                receipt, worker = self.verify_retirement(output)
                self.assertEqual(receipt['interruptedSignal'], sig)
                self.assertTrue(worker['interrupted'])
                self.verify_mapping(worker)

    def test_actual_main_identity_copies_and_platform_missing_kernel(self):
        thread = threading.current_thread()
        original = profiler.owner_observation(thread)
        self.assertEqual(original['objectId'], id(thread))
        self.assertEqual(original['nativeId'], threading.get_native_id())
        self.assertTrue(original['alive'])
        absent = profiler.owner_observation(None)
        self.assertIsNone(absent['objectId'])
        self.assertIsNone(absent['nativeId'])
        self.assertIsNone(profiler.joined_cpu(absent, profiler.proc_threads()))
        with patch.object(profiler, 'kernel_identity', return_value=None):
            missing = profiler.owner_observation(thread)
        self.assertEqual(missing['nativeId'], original['nativeId'])
        self.assertIsNone(profiler.joined_cpu(missing, profiler.proc_threads()))
        with patch.object(threading, 'get_native_id', None):
            missing_native = profiler.owner_observation(thread)
        self.assertIsNone(missing_native['nativeId'])
        self.assertIsNone(missing_native['kernelIdentity'])
        with patch.object(Path, 'read_text', side_effect=PermissionError('explicit fixture denial')):
            self.assertIsNone(profiler.kernel_identity(original['nativeId']))
            self.assertIsNone(profiler.proc_threads())
        self.assertEqual(original['nativeId'], threading.get_native_id())

    def test_actual_sport_thread_lifecycle_death_join_and_historical_copy(self):
        from sport_source import SportSource
        source = SportSource(Path(self.temp.name), simulated=True)
        self.addCleanup(source.close)
        unstarted = profiler.owner_observation(source.thread)
        self.assertIsNone(unstarted['nativeId'])
        source.start()
        actual = source.thread
        before = profiler.owner_observation(actual)
        first = profiler.proc_threads()
        after = profiler.owner_observation(actual)
        last = profiler.proc_threads()
        preserved = copy.deepcopy(before)
        source.close()
        self.assertFalse(actual.is_alive())
        self.assertIsNotNone(actual.native_id)  # Python keeps this historical ID.
        dead = profiler.owner_observation(actual)
        self.assertIsNone(dead['nativeId'])
        self.assertIsNone(dead['kernelIdentity'])
        self.assertIsNone(profiler.joined_cpu(dead, last))
        result = profiler.owner_mapping(before, after, dead, first, last)
        self.assertEqual(before, preserved)
        before['nativeId'] = None
        after['objectId'] = None
        self.assertEqual(result['before'], preserved)
        self.assertIsNotNone(result['after']['objectId'])
        self.assertFalse(result['afterShutdown']['alive'])

    @unittest.skipUnless(sys.platform == 'linux', 'Linux kernel task/start-tick join only')
    def test_original_kernel_records_reject_id_reuse_foreign_missing_and_reversed_cpu(self):
        # Actual current owner and actual OS rows first. Mutations below are
        # explicitly mismatch fixtures, never evidence of real kernel ID reuse.
        before = profiler.owner_observation(threading.current_thread())
        first = profiler.proc_threads()
        after = profiler.owner_observation(threading.current_thread())
        last = profiler.proc_threads()
        mapping = profiler.owner_mapping(before, after, after, first, last)
        self.assertIsNotNone(mapping['kernelCpuInterval'])
        tid = str(before['nativeId'])
        for field in ('processId', 'processStartTicks'):
            changed = copy.deepcopy(first)
            changed[field] += 1
            self.assertIsNone(profiler.joined_cpu(before, changed))
        for field in ('nativeId', 'startTicks'):
            changed = copy.deepcopy(first)
            changed['threads'][tid][field] += 1
            self.assertIsNone(profiler.joined_cpu(before, changed))
        missing = copy.deepcopy(first)
        del missing['threads'][tid]
        self.assertIsNone(profiler.joined_cpu(before, missing))
        historical = copy.deepcopy(first)
        historical['threads'][tid]['observedMonotonicNs'] = before['observedMonotonicNs'] - 1
        self.assertIsNone(profiler.joined_cpu(before, historical))
        foreign = copy.deepcopy(after)
        foreign['objectId'] += 1
        self.assertIsNone(profiler.owner_mapping(before, foreign, after, first, last)['kernelCpuInterval'])
        reused = copy.deepcopy(after)
        newer = copy.deepcopy(last)
        reused['kernelIdentity']['threadStartTicks'] += 1
        newer['threads'][tid]['startTicks'] += 1
        self.assertIsNotNone(profiler.joined_cpu(reused, newer))
        self.assertIsNone(profiler.owner_mapping(before, reused, after, first, newer)['kernelCpuInterval'])
        for field in ('userTicks', 'systemTicks'):
            reversed_cpu = copy.deepcopy(last)
            reversed_cpu['threads'][tid][field] = first['threads'][tid][field] - 1
            self.assertIsNone(profiler.owner_mapping(before, after, after, first, reversed_cpu)['kernelCpuInterval'])
        first['threads'][tid]['userTicks'] = -1
        self.assertGreaterEqual(mapping['kernelCpuBefore']['userTicks'], 0)

    def test_actual_owner_failure_preserves_null_interval_and_retires_sources(self):
        from phone_manager import Manager
        folder = Path(self.temp.name).resolve()
        output = self.root / 'failure'
        output.mkdir()
        (folder / 'begin').touch()
        with patch.object(Manager, 'tick', side_effect=ValueError('explicit fixture workload failure')):
            with self.assertRaisesRegex(ValueError, 'explicit fixture workload failure'):
                profiler.worker(folder, output, 'configured', 'cpu', NATIVE)
        worker = json.loads((output / 'worker.json').read_text())
        self.assertFalse(worker['completed'])
        self.assertTrue(worker['retirement']['sportJoined'])
        self.assertTrue(worker['retirement']['managerSocketAbsent'])
        self.assertIsNone(worker['threadOwners']['main']['after'])
        self.assertIsNone(worker['threadOwners']['main']['independentCpu'])
        self.assertIsNone(worker['threadOwners']['sport']['kernelCpuInterval'])
        self.assertFalse(worker['threadOwners']['sport']['afterShutdown']['alive'])
        if NATIVE:
            self.assertTrue(worker['retirement']['nativeWaited'])
            self.assertTrue(worker['retirement']['nativeValuesWithdrawn'])

    def test_bad_bounds_and_existing_output_never_launch_an_owner(self):
        for seconds in ('nan', 'inf', '0', '61'):
            output = self.root / ('bound-' + seconds)
            child = self.launch(output, '--seconds', seconds)
            child.communicate(timeout=3)
            self.assertNotEqual(child.returncode, 0)
            self.assertFalse(output.exists())
        output = self.root / 'existing'
        output.mkdir()
        sentinel = output / 'receipt.json'
        sentinel.write_text('retained prior evidence')
        child = self.launch(output, '--seconds', '1')
        child.communicate(timeout=3)
        self.assertNotEqual(child.returncode, 0)
        self.assertEqual(sentinel.read_text(), 'retained prior evidence')
        self.assertFalse((output / 'ready.json').exists())


if __name__ == '__main__':
    unittest.main()
