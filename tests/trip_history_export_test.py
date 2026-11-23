#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real offline CLI on owned synthetic archives; faults never touch user state."""
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from trip_history import MAX_BYTES, envelope, restore
from trip_history_test import archive

TOOL = Path(__file__).resolve().parents[1] / 'tools/trip_history_export.py'


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.root.chmod(0o700)
        self.input = self.root / 'trip-history.json'
        self.output = self.root / 'export'
        self.records = archive()
        self.save(self.records)
        self.original = self.input.read_bytes()
        self.original_mark = self.input.stat()

    def tearDown(self):
        self.temporary.cleanup()

    def save(self, records):
        self.input.write_bytes(envelope(records))
        self.input.chmod(0o600)

    def command(self, hook=None):
        args = ['--input', str(self.input), '--output-directory', str(self.output)]
        if hook is None:
            return [sys.executable, '-B', str(TOOL), *args]
        # Private process-only injected syscall failure/gate; no production test switch.
        bootstrap = self.root / 'bootstrap.py'
        descriptor_check = ('import atexit, fcntl\n'
                            'def descriptors():\n'
                            ' result=[]\n'
                            ' for fd in range(256):\n'
                            '  try: fcntl.fcntl(fd,fcntl.F_GETFD); result.append(fd)\n'
                            '  except OSError: pass\n'
                            ' return result\n'
                            'initial_descriptors=descriptors()\n'
                            'def verify_descriptors():\n'
                            ' assert descriptors()==initial_descriptors, "Exporter descriptor leak"\n'
                            'atexit.register(verify_descriptors)\n')
        bootstrap.write_text('import os, sys, runpy, stat, time, signal\nfrom pathlib import Path\n' + descriptor_check +
                             'root=Path(' + repr(str(self.root)) + ')\n' + hook + '\n' +
                             'sys.argv=[' + repr(str(TOOL)) + ']+sys.argv[1:]\n' +
                             'runpy.run_path(' + repr(str(TOOL)) + ',run_name="__main__")\n')
        return [sys.executable, '-B', str(bootstrap), *args]

    def run_cli(self, hook=None):
        return subprocess.run(self.command(hook), capture_output=True, text=True, timeout=5,
                              env={**os.environ, 'PYTHONPATH': str(TOOL.parent)})

    def refused(self, result):
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr, 'Trip history export refused or failed; durability unknown.\n')

    def unchanged(self):
        self.assertEqual(self.input.read_bytes(), self.original)
        info = self.input.stat()
        for key in ('st_dev', 'st_ino', 'st_mode', 'st_uid', 'st_gid', 'st_nlink', 'st_size', 'st_mtime_ns', 'st_ctime_ns'):
            self.assertEqual(getattr(info, key), getattr(self.original_mark, key))

    def exported(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        report = json.loads(result.stdout)
        self.assertEqual(set(report), {'recordCount', 'inputSha256', 'exportSha256', 'path'})
        path = self.output / 'trip-history-export.json'
        self.assertEqual(report['path'], str(path))
        self.assertEqual(report['inputSha256'], hashlib.sha256(self.input.read_bytes()).hexdigest())
        self.assertEqual(report['exportSha256'], hashlib.sha256(path.read_bytes()).hexdigest())
        from trip_history_export import MAX_EXPORT_BYTES
        self.assertLessEqual(path.stat().st_size, MAX_EXPORT_BYTES)
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(path.stat().st_nlink, 1)
        self.assertEqual(list(self.output.iterdir()), [path])
        value = json.loads(path.read_bytes())
        self.assertEqual(set(value), {'version', 'kind', 'classification', 'sourceSchema', 'inputSha256', 'recordCount', 'history'})
        self.assertEqual(value['version'], 1)
        self.assertEqual(value['classification'], 'HISTORICAL')
        self.assertEqual(value['sourceSchema'], 'ultra-headunit-trip-history-v1')
        self.assertEqual(value['history'], restore(self.input.read_bytes()))
        self.assertEqual(report['recordCount'], len(value['history']))
        self.assertEqual(value['inputSha256'], report['inputSha256'])
        self.assertNotIn('journeyId', result.stdout)
        return value

    def test_valid_archive_exact_values_and_live_store_unchanged(self):
        value = self.exported()
        self.assertEqual(value['history'], self.records)
        self.unchanged()

    def test_success_closes_descriptors_in_running_process(self):
        result = self.run_cli('')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        self.unchanged()

    def test_empty_history_is_explicit_zero_records(self):
        self.save([])
        self.assertEqual(self.exported()['history'], [])

    def test_maximum_records_mixed_boot_clock_origin_and_exact_64bit_dates(self):
        records = []
        for index in range(16):
            item = copy.deepcopy(self.records[0])
            item['ownerId'] = '%032x' % (index + 1)
            item['hostBootId'] = '12345678-1234-1234-1234-%012x' % (index + 1)
            item['journeyId'] = 2**53 - 1 - index
            item['sourceGeneration'] = 2**64 - 1 - index
            item['sourceClock'] = 'CLOCK_MONOTONIC_RAW' if index % 2 else 'CLOCK_MONOTONIC'
            item['origin'] = ('simulation', 'measured', 'calculated')[index % 3]
            item['peak']['origin'] = item['origin']
            delta = 2**63 - 1 - item['archivedAtNs'] - index
            for key in ('firstAcquiredNs', 'lastAcquiredNs', 'archivedAtNs'):
                item[key] += delta
            item['peak']['acquiredNs'] += delta
            records.append(item)
        self.save(records)
        self.assertEqual(self.exported()['history'], records)

    def test_null_single_acquisition_and_partial_coverage_are_preserved(self):
        item = self.records[0]
        item['lastAcquiredNs'] = item['firstAcquiredNs']
        item['archivedAtNs'] = item['firstAcquiredNs'] + 1
        for key in ('distanceKm', 'coveredSeconds', 'movingSeconds', 'movingDistanceKm',
                    'averageSpeedKmh', 'movingAverageSpeedKmh', 'coverageRatio'):
            item[key] = None
        item['acquiredSpanSeconds'] = item['uncoveredSeconds'] = 0
        self.save([item])
        self.assertEqual(self.exported()['history'], [item])
        # Separate valid gap case, no inferred distance/average correction.
        self.output = self.root / 'partial'
        item['lastAcquiredNs'] += 10**9
        item['archivedAtNs'] = item['lastAcquiredNs'] + 1
        item['acquiredSpanSeconds'] = item['uncoveredSeconds'] = 1
        item['coverageRatio'] = 0
        item['partial'] = True
        self.save([item])
        self.assertEqual(self.exported()['history'], [item])

    def test_schema_unknown_duplicate_corrupt_truncated_oversized_and_record_bounds(self):
        bad = []
        value = json.loads(self.original)
        value['extra'] = 1
        bad.append(json.dumps(value).encode())
        item = copy.deepcopy(self.records[0])
        item['futureField'] = 1
        body = {'version': 1, 'history': [item]}
        from trip_history import encode
        bad.append(encode({**body, 'sha256': hashlib.sha256(encode(body)).hexdigest()}))
        bad.extend([self.original[:-1], b'{}', b'[]', b'{"version":1,"version":1}', b'x' * (MAX_BYTES + 1), b''])
        value = json.loads(self.original)
        value['history'][0]['distanceKm'] += 1
        bad.append(json.dumps(value).encode())
        for raw in bad:
            with self.subTest(rawSize=len(raw)):
                self.input.write_bytes(raw)
                self.refused(self.run_cli())
                self.assertFalse(self.output.exists())
        records = [copy.deepcopy(self.records[0]) for _ in range(17)]
        for index, item in enumerate(records):
            item['ownerId'] = '%032x' % index
        body = {'version': 1, 'history': records}
        self.input.write_bytes(encode({**body, 'sha256': hashlib.sha256(encode(body)).hexdigest()}))
        self.refused(self.run_cli())

    def test_modes_links_symlinks_special_files_and_noncanonical_parents(self):
        for mode in (0o644, 0o400, 0o660):
            with self.subTest(mode=mode):
                self.input.chmod(mode)
                self.refused(self.run_cli())
        self.input.chmod(0o600)
        link = self.root / 'link'
        os.link(self.input, link)
        self.refused(self.run_cli())
        link.unlink()
        self.input.rename(link)
        self.input.symlink_to(link)
        self.refused(self.run_cli())
        self.input.unlink()
        os.mkfifo(self.input, 0o600)
        self.refused(self.run_cli())
        self.input.unlink()
        link.rename(self.input)
        self.root.chmod(0o755)
        self.refused(self.run_cli())
        self.root.chmod(0o700)
        alias = self.root / 'alias'
        alias.symlink_to(self.root, target_is_directory=True)
        self.input = alias / 'trip-history.json'
        self.refused(self.run_cli())

    @unittest.skipUnless(os.geteuid() == 0, 'Actual foreign UID/GID requires Linux privileged fixture')
    def test_foreign_input_owner_and_group_rejected(self):
        for uid, gid in ((1, os.getgid()), (os.getuid(), 1)):
            with self.subTest(uid=uid, gid=gid):
                os.chown(self.input, uid, gid)
                self.refused(self.run_cli())
        os.chown(self.input, os.getuid(), os.getgid())

    def test_input_replacement_mutation_and_late_change_refuse(self):
        hooks = [
            "real=os.read\ndone=False\ndef changed(fd,n):\n global done\n data=real(fd,n)\n if not done:\n  done=True\n  p=root/'trip-history.json'\n  q=root/'replacement'\n  q.write_bytes(p.read_bytes()); q.chmod(0o600); q.replace(p)\n return data\nos.read=changed",
            "real=os.read\ndone=False\ndef changed(fd,n):\n global done\n data=real(fd,n)\n if not done:\n  done=True\n  with (root/'trip-history.json').open('ab') as out: out.write(b' ')\n return data\nos.read=changed",
            "real=os.fsync\ndone=False\ndef changed(fd):\n global done\n real(fd)\n if not done:\n  done=True\n  p=root/'trip-history.json'; q=root/'replacement'\n  q.write_bytes(p.read_bytes()); q.chmod(0o600); q.replace(p)\nos.fsync=changed"]
        for hook in hooks:
            with self.subTest(hook=hook):
                self.save(self.records)
                self.refused(self.run_cli(hook))
                self.assertFalse(self.output.exists())

    def test_existing_directory_file_symlink_and_atomic_destination_race_refused(self):
        self.output.mkdir(mode=0o700)
        self.refused(self.run_cli())
        self.assertEqual(list(self.output.iterdir()), [])
        self.output.rmdir()
        self.output.write_text('foreign')
        self.refused(self.run_cli())
        self.assertEqual(self.output.read_text(), 'foreign')
        self.output.unlink()
        self.output.symlink_to(self.root, target_is_directory=True)
        self.refused(self.run_cli())
        self.output.unlink()
        hook = "real=os.link\ndef raced(src,dst,**kw):\n fd=os.open(dst,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600,dir_fd=kw['dst_dir_fd'])\n os.write(fd,b'foreign'); os.close(fd)\n return real(src,dst,**kw)\nos.link=raced"
        self.refused(self.run_cli(hook))
        self.assertEqual((self.output / 'trip-history-export.json').read_bytes(), b'foreign')
        self.assertEqual(len(list(self.output.iterdir())), 1)
        self.unchanged()

    def test_short_writes_zero_write_write_sync_permission_failures_and_atomic_visibility(self):
        hook = "real=os.write\ndef short(fd,data): return real(fd,data[:7])\nos.write=short"
        result = self.run_cli(hook)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.output / 'trip-history-export.json').read_bytes())['history'], self.records)
        import shutil
        shutil.rmtree(self.output)
        hooks = ["def fail(fd,data): return 0\nos.write=fail",
                 "real=os.write\ndef fail(fd,data):\n real(fd,data[:5]); raise OSError('private data')\nos.write=fail",
                 "def fail(fd,mode): raise PermissionError('private data')\nos.fchmod=fail"]
        for stage in (1, 2, 3):
            hooks.append("real=os.fsync\ncount=0\ndef fail(fd):\n global count\n count+=1\n if count==" + str(stage) + ": raise OSError('private data')\n return real(fd)\nos.fsync=fail")
        for hook in hooks:
            with self.subTest(hook=hook):
                self.refused(self.run_cli(hook))
                self.assertFalse(self.output.exists())
                self.unchanged()
        # At publication, the only visible candidate must already be complete JSON.
        hook = "real=os.link\ndef checked(src,dst,**kw):\n assert not (root/'export'/dst).exists()\n import json\n assert json.loads((root/'export'/src).read_bytes())['recordCount']==1\n return real(src,dst,**kw)\nos.link=checked"
        self.assertEqual(self.run_cli(hook).returncode, 0)

    def test_output_open_and_directory_permission_failures_cleanup(self):
        hooks = [
            "real=os.open\ndef fail(path,flags,*args,**kw):\n if str(path).startswith('.trip-export-'): raise PermissionError('private')\n return real(path,flags,*args,**kw)\nos.open=fail",
            "real=os.open\ndef fail(path,flags,*args,**kw):\n if path=='export': raise PermissionError('private')\n return real(path,flags,*args,**kw)\nos.open=fail"]
        for hook in hooks:
            with self.subTest(hook=hook):
                self.refused(self.run_cli(hook))
                self.assertFalse(self.output.exists())
                self.unchanged()

    def test_directory_replacement_preserves_foreign_and_cleans_own_descriptor(self):
        hook = "real=os.fsync\ndone=False\ndef changed(fd):\n global done\n real(fd)\n if not done:\n  done=True\n  p=root/'export'; p.rename(root/'retired-own')\n  p.mkdir(mode=0o700); (p/'foreign').write_text('keep')\nos.fsync=changed"
        self.refused(self.run_cli(hook))
        self.assertEqual(list((self.root / 'retired-own').iterdir()), [])
        self.assertEqual((self.output / 'foreign').read_text(), 'keep')
        self.unchanged()

    def test_signal_at_input_constructor_return_closes_registered_descriptors(self):
        hook = "def returned(frame,event,arg):\n if event=='return' and frame.f_code.co_name=='__init__' and frame.f_code.co_filename.endswith('/trip_history_export.py'):\n  sys.setprofile(None)\n  os.kill(os.getpid(),signal.SIGTERM)\nsys.setprofile(returned)"
        result = self.run_cli(hook)
        self.assertEqual(result.returncode, 143, result.stderr)
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr, 'Trip history export interrupted; durability unknown.\n')
        self.assertFalse(self.output.exists())
        self.unchanged()

    def test_int_term_hup_cleanup_at_read_write_and_after_atomic_link(self):
        for operation in ('read', 'write', 'link'):
            for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                with self.subTest(operation=operation, signal=signum):
                    marker = self.root / 'ready'
                    gate = self.root / 'continue'
                    hook = "real=os." + operation + "\ndone=False\ndef gated(*args,**kw):\n global done\n result=real(*args,**kw)\n if not done:\n  done=True\n  (root/'ready').write_text('ready')\n  while not (root/'continue').exists(): time.sleep(.005)\n return result\nos." + operation + "=gated"
                    process = subprocess.Popen(self.command(hook), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                               text=True, env={**os.environ, 'PYTHONPATH': str(TOOL.parent)})
                    try:
                        deadline = time.monotonic() + 2
                        while not marker.exists() and process.poll() is None and time.monotonic() < deadline:
                            time.sleep(.005)
                        self.assertTrue(marker.exists())
                        process.send_signal(signum)
                        # Link ownership acquisition masks signals until registration.
                        gate.touch()
                        out, err = process.communicate(timeout=2)
                        self.assertEqual(process.returncode, 128 + signum, err)
                        self.assertEqual(out, '')
                        self.assertEqual(err, 'Trip history export interrupted; durability unknown.\n')
                        self.assertFalse(self.output.exists())
                        self.unchanged()
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait()
                        marker.unlink(missing_ok=True)
                        gate.unlink(missing_ok=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
