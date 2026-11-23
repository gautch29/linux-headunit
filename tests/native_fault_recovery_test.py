# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Offline qualification auditor tests: explicit fixtures, no system services."""
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import native_fault_recovery as audit


# An explicit fake probe archive, not hardware evidence. Dates, PIDs and hashes
# below identify fixture data only; no actual boot or selected generation claim.
def fixture(owner='headunit-receiver'):
    verification = b'{"fixture":"no selected appliance"}\n'
    scope = {'source_commit': 'a' * 40, 'selected_manifest_sha256': 'b' * 64,
             'helper_manifest_sha256': 'c' * 64,
             'units_sha256': dict.fromkeys(audit.OWNERS, 'd' * 64)}
    plan = {'event': 'plan', 'schema_version': 1, 'case': 'stop-start', 'owner': owner,
            'planned_restarts': 1, 'evidence': 'synthetic', 'scope': scope,
            'verification_sha256': hashlib.sha256(verification).hexdigest()}
    owners = {name: {'Id': name + '.service', 'ActiveState': 'active', 'SubState': 'running',
                    'MainPID': str(100 + index), 'NRestarts': '2', 'InvocationID': str(index + 1) * 32}
              for index, name in enumerate(audit.OWNERS)}
    rows = [plan]
    for index, phase in enumerate(audit.PHASES):
        now = 10_000_000_000 + index * 1_000_000_000
        row = {'event': 'sample', 'phase': phase, 'boot_id': '00000000-0000-0000-0000-000000000001',
               'remote_monotonic_ns': now, 'host_started_ns': now + 900_000_000_000,
               'host_finished_ns': now + 900_000_000_010, 'scope': copy.deepcopy(scope),
               'owners': copy.deepcopy(owners), 'manager': {'active_present': False, 'error_present': False},
               'receipt': None, 'boot_gate': {'Id': 'headunit-boot-recovery.service', 'ActiveState': 'active',
                                             'SubState': 'exited', 'Result': 'success', 'InvocationID': 'f' * 32}}
        if phase == 'retired':
            row['owners'][owner].update(ActiveState='inactive', SubState='dead', MainPID='0', InvocationID='')
            if owner == 'headunit-receiver':
                row['manager'] = None
        if phase == 'restarted':
            row['owners'][owner].update(MainPID='201', InvocationID='e' * 32)
        if phase in ('dispatched', 'restarted'):
            row['dispatch'] = {'operation': 'stop' if phase == 'dispatched' else 'start', 'owner': owner,
                               'boot_id': row['boot_id'],
                               'expected_pid': int(owners[owner]['MainPID']) if phase == 'dispatched' else 0,
                               'expected_invocation_id': owners[owner]['InvocationID'] if phase == 'dispatched' else '',
                               'dispatched_monotonic_ns': now - 500_000_000, 'acknowledged': True}
        rows.append(row)
    return verification, rows


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.archive = self.root / 'receipts.jsonl'
        self.verification, self.rows = fixture()
        self.verification_hash = hashlib.sha256(self.verification).hexdigest()

    def tearDown(self):
        evidence = os.environ.get('HEADUNIT_NATIVE_AUDIT_TEST_EVIDENCE')
        if evidence:
            destination = Path(evidence) / self._testMethodName
            destination.mkdir(mode=0o700)
            for source in self.root.iterdir():
                if stat.S_ISREG(source.lstat().st_mode):
                    shutil.copy2(source, destination / source.name)

    def write(self, rows=None):
        self.archive.write_text(''.join(json.dumps(row) + '\n' for row in (rows or self.rows)))

    def assess(self):
        return audit.assess(self.archive, self.verification_hash, time.monotonic() + 10)

    def reject(self, rows, code):
        self.write(rows)
        with self.assertRaisesRegex(audit.Rejected, '^' + code + '$'):
            self.assess()

    def test_one_case_counts_and_original_clocks_do_not_prove_pixels(self):
        self.write()
        report = self.assess()
        self.assertTrue(report['logical_owner_recovery_observed'])
        self.assertEqual(report['planned_restarts'], 1)
        self.assertEqual(report['automatic_restart_deltas'], dict.fromkeys(audit.OWNERS, 0))
        self.assertEqual(report['phases'][0]['host_started_ns'], 910_000_000_000)
        self.assertEqual(report['phases'][0]['remote_monotonic_ns'], 10_000_000_000)
        self.assertIsNone(audit.content_evidence(True)['candidate'])

    def test_exact_five_owner_allowlist_and_each_isolated_fixture(self):
        for owner in audit.OWNERS:
            self.verification, rows = fixture(owner)
            self.write(rows)
            self.assertEqual(self.assess()['owner'], owner)
        rows[0]['owner'] = 'headunit-receiver; kill -9 1'
        self.reject(rows, 'owner_or_plan_unsupported')

    def test_changed_selected_source_helper_unit_and_boot_gate(self):
        for key in audit.SCOPE:
            rows = copy.deepcopy(self.rows)
            rows[2]['scope'][key] = 'foreign'
            self.reject(rows, 'authority_changed')
        rows = copy.deepcopy(self.rows)
        rows[2]['boot_gate']['InvocationID'] = 'e' * 32
        self.reject(rows, 'boot_gate_changed')
        rows[1]['boot_gate']['Result'] = 'failed'
        self.reject(rows, 'boot_gate_unavailable')

    def test_first_failed_precondition_never_passes_from_successor(self):
        rows = copy.deepcopy(self.rows)
        rows[1]['owners']['headunit-cluster']['ActiveState'] = 'failed'
        rows[-1]['owners']['headunit-cluster']['MainPID'] = '500'
        self.reject(rows, 'owner_not_healthy')
        rows = copy.deepcopy(self.rows)
        rows[1]['manager']['active_present'] = True
        self.reject(rows, 'phonefree_status_unavailable')
        rows[1]['manager'] = None
        self.reject(rows, 'phonefree_status_unavailable')

    def test_changed_dispatched_pid_and_successor_not_signalled(self):
        rows = copy.deepcopy(self.rows)
        rows[2]['owners']['headunit-receiver'].update(MainPID='900', InvocationID='e' * 32)
        self.reject(rows, 'unexpected_owner_replacement')
        rows = copy.deepcopy(self.rows)
        rows[2]['dispatch']['expected_pid'] += 1
        self.reject(rows, 'dispatch_identity_changed')
        rows[2]['dispatch']['acknowledged'] = False
        self.reject(rows, 'dispatch_not_acknowledged')
        rows = copy.deepcopy(self.rows)
        rows[2].pop('dispatch')
        self.reject(rows, 'dispatch_receipt_missing')

    def test_retirement_missing_and_reused_or_other_owner_replacement(self):
        rows = copy.deepcopy(self.rows)
        rows[3]['owners']['headunit-receiver']['MainPID'] = '104'
        self.reject(rows, 'retirement_unproven')
        rows = copy.deepcopy(self.rows)
        rows[-1]['owners']['headunit-receiver']['InvocationID'] = rows[1]['owners']['headunit-receiver']['InvocationID']
        self.reject(rows, 'successor_unproven')
        rows = copy.deepcopy(self.rows)
        rows[-1]['owners']['headunit-cluster']['MainPID'] = '901'
        self.reject(rows, 'unexpected_owner_replacement')

    def test_foreign_retirement_and_original_status_receipt(self):
        rows = copy.deepcopy(self.rows)
        rows[3]['owners']['headunit-receiver']['InvocationID'] = 'e' * 32
        self.reject(rows, 'foreign_retirement')
        rows = copy.deepcopy(self.rows)
        rows[1]['receipt'] = {'state': 'disconnected', 'returnReady': False}
        self.write(rows)
        self.assertTrue(self.assess()['logical_owner_recovery_observed'])
        for receipt in ({'returnReady': False}, {'state': 'ready', 'returnReady': True}, []):
            rows[1]['receipt'] = receipt
            self.reject(rows, 'phone_receipt_present_or_missing')
        rows[1].pop('receipt')
        self.reject(rows, 'status_receipt_missing')

    def test_automatic_restart_and_counter_reset_missing_bool_are_failures(self):
        for count in ('3', '0', None, True, '02'):
            rows = copy.deepcopy(self.rows)
            rows[-1]['owners']['headunit-receiver']['NRestarts'] = count
            self.reject(rows, 'automatic_restart_or_counter_reset' if count in ('3', '0') else 'owner_counter_unavailable')

    def test_foreign_boot_replayed_host_and_source_dates(self):
        rows = copy.deepcopy(self.rows)
        rows[2]['boot_id'] = '00000000-0000-0000-0000-000000000002'
        self.reject(rows, 'foreign_boot')
        for key in ('remote_monotonic_ns', 'host_started_ns'):
            rows = copy.deepcopy(self.rows)
            rows[2][key] = rows[1][key]
            self.reject(rows, 'replayed_or_overlapping_observation')

    def test_lost_partial_retired_extra_duplicate_and_malformed(self):
        self.reject(self.rows[:-1], 'lost_receipt')
        self.reject(self.rows + [self.rows[-1]], 'multiple_cases_or_extra_receipt')
        rows = copy.deepcopy(self.rows)
        rows[3]['phase'] = 'before'
        self.reject(rows, 'phase_missing_or_reordered')
        self.write()
        self.archive.write_bytes(self.archive.read_bytes()[:-1])
        with self.assertRaisesRegex(audit.Rejected, 'partial_or_overbound'):
            self.assess()
        self.archive.write_bytes(b'{"event":"plan","event":"plan"}\n')
        with self.assertRaisesRegex(audit.Rejected, 'duplicate_json_key'):
            self.assess()
        self.archive.write_bytes(b'{bad}\n')
        with self.assertRaises(ValueError):
            self.assess()

    def test_budgets_and_no_unbounded_fifo_symlink_or_changed_input(self):
        self.write()
        with self.assertRaisesRegex(audit.Rejected, 'audit_budget'):
            audit.assess(self.archive, self.verification_hash, 0)
        with patch.object(audit, 'MAX_LINE', 20), self.assertRaisesRegex(audit.Rejected, 'partial_or_overbound'):
            self.assess()
        rows = copy.deepcopy(self.rows)
        for key in ('remote_monotonic_ns', 'host_started_ns', 'host_finished_ns'):
            rows[-1][key] += 90_000_000_000
        self.reject(rows, 'case_budget')
        fifo = self.root / 'fifo'
        os.mkfifo(fifo)
        with self.assertRaises(ValueError):
            audit.retain(fifo, self.root / 'copy', 100, time.monotonic() + 1)
        link = self.root / 'link'
        link.symlink_to(self.archive)
        with self.assertRaises(OSError):
            audit.retain(link, self.root / 'copy', 100, time.monotonic() + 1)
        self.write()
        with self.assertRaises(ValueError):
            with audit.bounded_input(self.archive, audit.MAX_ARCHIVE) as stream:
                stream.read()
                self.archive.write_text('changed')

    def output(self, name):
        # Production CLI permits only the qualification cache. This test owns
        # exactly this temporary folder, including in its isolated Linux HOME.
        base = Path.home() / '.cache/headunit-qualification'
        base.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix='native-audit-test-', dir=base)).resolve()
        self.addCleanup(lambda: shutil.rmtree(folder))
        evidence = os.environ.get('HEADUNIT_NATIVE_AUDIT_TEST_EVIDENCE')
        if evidence:
            destination = Path(evidence) / folder.name
            self.addCleanup(lambda: shutil.copytree(folder, destination))
        return folder / name

    def cli_args(self, output):
        verification = self.root / 'verification.json'
        verification.write_bytes(self.verification)
        return ['--receipts', str(self.archive), '--verification', str(verification), '--output', str(output)]

    def test_actual_cli_private_retention_sanitized_summary_and_unknowns(self):
        self.rows[1]['manager']['private'] = 'phone-address pairing media username'
        self.write()
        output = self.output('passed')
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/native_fault_recovery.py'),
                                 *self.cli_args(output)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((output / 'summary.json').read_text())
        self.assertNotIn('phone-address', json.dumps(report))
        self.assertEqual((output / 'receipts-original.jsonl').read_bytes(), self.archive.read_bytes())
        self.assertTrue(report['audit_passed'])
        self.assertFalse(report['bench_qualified'])
        for key in ('phone_acceptance', 'power_on_ns', 'soft_reboot_ns', 'boot_gate_ready_ns',
                    'first_native_buffer_ns', 'first_native_present_ns', 'optical_first_frame_s'):
            self.assertIsNone(report[key])
        self.assertEqual(output.stat().st_mode & 0o777, 0o700)
        self.assertTrue(all(path.stat().st_mode & 0o777 == 0o600 for path in output.iterdir()))
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/native_fault_recovery.py'),
                                 *self.cli_args(output)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2)

    def test_failure_retains_original_and_verification_mismatch(self):
        self.rows[0]['verification_sha256'] = 'e' * 64
        self.write()
        output = self.output('failure')
        self.assertEqual(audit.main(self.cli_args(output)), 1)
        report = json.loads((output / 'summary.json').read_text())
        self.assertEqual(report['failure'], 'verification_mismatch')
        self.assertFalse(report['audit_passed'])
        self.assertEqual((output / 'receipts-original.jsonl').read_bytes(), self.archive.read_bytes())

    def test_actual_int_term_hup_fail_report_and_child_reaped(self):
        self.write()
        # Explicit fake backend pauses at the parser seam, after original-byte
        # retention and production signal handler setup. Parent sends real OS
        # signals to ONLY its Popen child, then wait() proves actual reaping.
        script = ("import sys,signal; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
                  "import native_fault_recovery as a; "
                  "exec('def paused(*args):\\n Path(sys.argv[2]).write_text(\\\"ready\\\")\\n signal.pause()'); "
                  "a.assess=paused; sys.exit(a.main(sys.argv[3:]))")
        for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            output = self.output('signal')
            marker = self.root / ('signal-' + str(number))
            child = subprocess.Popen([sys.executable, '-B', '-c', script, str(ROOT / 'tools'), str(marker),
                                      *self.cli_args(output)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while not marker.exists() and child.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(marker.exists())
                child.send_signal(number)
                stdout, stderr = child.communicate(timeout=5)
                self.assertEqual(child.returncode, 128 + number, stderr)
                (output / 'caller-retirement.json').write_text(json.dumps({
                    'signal': number, 'child_pid': child.pid, 'exit_code': child.returncode,
                    'wait_completed': True}))
                self.assertEqual(json.loads((output / 'summary.json').read_text())['signal'], number)
                with self.assertRaises(ChildProcessError):
                    os.waitpid(child.pid, os.WNOHANG)
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
                child.stdout.close()
                child.stderr.close()


if __name__ == '__main__':
    unittest.main()
