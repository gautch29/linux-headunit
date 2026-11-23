# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Portable construction/safety checks; not real systemd job qualification."""
from pathlib import Path
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import appliance_boot_systemd_fixture as fixture
from appliance_transaction_test import System


class Tests(unittest.TestCase):
    def test_all_crash_seeds_use_the_existing_journal_and_restoration_owner(self):
        # Real local files and the existing locked transaction owner; command
        # dictionaries here do not qualify Linux parsing or boot jobs.
        for case in fixture.CASES[:8]:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder).resolve()
                units, state = root / 'units', root / 'state'
                units.mkdir(mode=0o700)
                system = System(units)
                def offline(args, **options):
                    return system([args[0], *args[2:]] if args[:2] == ['systemctl', '--root=/'] else args, **options)
                with patch.object(fixture, 'UNITS', units), patch.object(fixture, 'STATE', state), \
                        patch.object(fixture, 'EVENTS', state / 'events.jsonl'), patch.object(fixture, 'guard'), \
                        patch.object(fixture, 'command', offline), patch.object(fixture, 'prepare_boot'), \
                        patch.object(fixture.os, 'execv') as execute:
                    fixture.seed(case)
                execute.assert_called_once_with('/sbin/init', ['/sbin/init', '--unit='+fixture.TARGET])
                journals = list(state.glob('transaction-*/journal.json'))
                if case == 'none':
                    self.assertEqual(journals, [])
                elif case == 'damaged':
                    self.assertEqual(json.loads(journals[0].read_text()), {'version': 999})
                    with fixture.transaction.Transaction(units, state, system) as owner, self.assertRaises(ValueError):
                        owner.recover_pending()
                    continue
                else:
                    self.assertEqual(json.loads(journals[0].read_text())['state'],
                                     'complete' if case == 'complete' else 'rollback_pending' if case == 'rollback-pending' else 'applying')
                with fixture.transaction.Transaction(units, state, system) as owner:
                    owner.recover_pending()
                    selected = fixture.selection(case)[case == 'complete']
                    for name in fixture.transaction.MANAGED:
                        self.assertEqual((units / name).exists(), name in selected)
                        self.assertEqual(owner.enabled(name) == 'enabled', name in selected)
                        if name in selected:
                            self.assertEqual((units / name).read_text(), fixture.actor_unit('candidate' if case == 'complete' else 'previous', name))

    def test_read_only_source_snapshot_is_admissible_with_group_writable_caller_umask(self):
        import os
        import pwd
        import stat
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            source = root / 'source'; source.mkdir(mode=0o700)
            previous = os.umask(0o002)
            try:
                fixture.prepare_mount(source)
            finally:
                os.umask(previous)
            for path in source.rglob('*'):
                self.assertFalse(stat.S_IMODE(path.stat().st_mode) & 0o022)
                self.assertEqual(path.stat().st_uid, os.getuid())
                if path.is_file(): self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o444)
            pool = root / 'helpers'; pool.mkdir(mode=0o755)
            account = pwd.getpwnam('nobody') if os.getuid() == 0 else pwd.getpwuid(os.getuid())
            binding = fixture.release.stage({'version': 4, 'purpose': 'boot-recovery', 'boot_recovery': None,
                'execution': None, 'release_id': 'boot-helper-fixture', 'source_revision': 'a'*40,
                'repo': str(source), 'user': account.pw_name, 'configuration': {}, 'native_camera': None}, pool)
            self.assertEqual(fixture.release.verify(binding)['purpose'], 'boot-recovery')
            # Undo only fixture seal modes for temporary-directory cleanup.
            for path in root.rglob('*'):
                if path.is_dir(): path.chmod(0o700)

    def test_boundary_requires_rootless_container_and_expected_pid1(self):
        good = dict(platform='linux', uid=0, mapping='0 1000 1\n1 100000 65536\n',
                    container='podman', nonce='a'*32, expected='a'*32, pid=9, init_name='systemd')
        fixture.boundary(**good)
        fixture.boundary(**(good | {'boot': True, 'pid': 1, 'init_name': 'python3'}))
        # Actual mapped Podman tuple: destinations name the intermediate
        # rootless parent. Only the exact launcher map admits parent ID0.
        nested = good | {'mapping': '0 0 1\n1 1 65536\n'}
        fixture.boundary(**nested)
        fixture.boundary(**(nested | {'boot': True, 'pid': 1, 'init_name': 'python3'}))
        for bad in ({'platform': 'darwin'}, {'uid': 501}, {'mapping': '0 0 4294967295'},
                    {'mapping': '0 0 1'}, {'mapping': '0 1000 65536'}, {'mapping': 'malformed'},
                    {'mapping': '0 0 1\n1 1 65535'}, {'mapping': '0 0 1\n1 0 65536'},
                    {'container': None}, {'container': 'docker'}, {'nonce': 'b'*32},
                    {'expected': ''}, {'init_name': 'python3'}, {'boot': True}):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                fixture.boundary(**(good | bad))

    def test_host_guard_runs_before_any_inner_mutation(self):
        for callback, arguments in ((fixture.seed, ('optional-remove',)),
                                    (fixture.gate_event, ('queued',)), (fixture.prepare_boot, ()), (fixture.marker, ('previous', fixture.CORE[0], 'owner')),
                                    (fixture.hold, ()), (fixture.observe, ()), (fixture.warm_selection, ())):
            with self.subTest(callback=callback.__name__), patch.object(fixture, 'guard', side_effect=RuntimeError('refused')), \
                    patch.object(fixture, 'command') as command, patch.object(fixture, 'record') as record, \
                    patch.object(fixture.transaction, 'Transaction') as owner:
                with self.assertRaises(RuntimeError):
                    callback(*arguments)
                command.assert_not_called()
                record.assert_not_called()
                owner.assert_not_called()

    def test_container_launcher_is_local_immutable_and_confined(self):
        args = fixture.container_command('sha256:' + 'a'*64, 'private-test', Path('/tmp/source'), 'b'*32, 'none')
        for option in ('--pull=never', '--network=none', '--cgroupns=private', '--systemd=always', '--timeout=90'):
            self.assertIn(option, args)
        self.assertEqual(args[args.index('--volume')+1], '/tmp/source:/fixture:ro,Z')
        for kind in ('uid', 'gid'):
            self.assertIn('--'+kind+'map=0:0:1', args)
            self.assertIn('--'+kind+'map=1:1:65536', args)
        self.assertFalse(any(option.startswith('--userns=') for option in args))
        self.assertFalse(any(option in args for option in ('--privileged', '--pid=host', '--userns=host', '--network=host')))
        for image in ('latest', 'some-image:tag', 'sha256:'+'a'*63, 'sha256:'+'A'*64):
            with self.subTest(image=image), self.assertRaises(ValueError):
                fixture.container_command(image, 'private-test', Path('/tmp/source'), 'b'*32, 'none')
        with patch.object(fixture.sys, 'platform', 'darwin'), patch.object(fixture, 'command') as run, self.assertRaises(RuntimeError):
            fixture.qualify('sha256:'+'a'*64)
        run.assert_not_called()

    def test_outer_host_root_and_non_rootless_manager_never_prepare_a_container(self):
        for uid, rootless in ((0, True), (1000, False)):
            with self.subTest(uid=uid, rootless=rootless), patch.object(fixture.sys, 'platform', 'linux'), \
                    patch.object(fixture.os, 'geteuid', return_value=uid), patch.object(fixture, 'command') as run:
                run.return_value = subprocess.CompletedProcess(['podman', 'info'], 0,
                        stdout=json.dumps({'host': {'security': {'rootless': rootless}}}))
                with self.assertRaises(RuntimeError):
                    fixture.qualify('sha256:'+'a'*64)
                if uid == 0:
                    run.assert_not_called()
                else:
                    run.assert_called_once_with(['podman', 'info', '--format=json'], check=True)

    def test_failed_case_keeps_bounded_streams_logs_before_cleanup_and_no_argv(self):
        calls = []
        nonce = 'a'*32
        def run(args, **_):
            calls.append(args)
            if args[:2] == ['podman', 'info']:
                return subprocess.CompletedProcess(args, 0, stdout='{"host":{"security":{"rootless":true}}}')
            if args[:2] == ['podman', 'run']:
                raise subprocess.CalledProcessError(125, args + ['HOST_PRIVATE=must-not-print'],
                                                    output='prefix'+('x'*5000)+'failed stdout',
                                                    stderr='mapping failure '+nonce)
            if args[:2] == ['podman', 'logs']:
                return subprocess.CompletedProcess(args, 0, stdout='PID1 cause '+nonce, stderr='')
            if args[:2] == ['podman', 'exec']:
                raise subprocess.TimeoutExpired(args, 15, stderr=b'recovery log unavailable')
            self.assertEqual(args[:2], ['podman', 'rm'])
            return subprocess.CompletedProcess(args, 0, stdout='', stderr='')
        output = io.StringIO()
        with patch.object(fixture.sys, 'platform', 'linux'), patch.object(fixture.os, 'geteuid', return_value=1000), \
                patch.object(fixture, 'CASES', ('optional-remove',)), patch.object(fixture, 'command', run), \
                patch.object(fixture.uuid, 'uuid4', return_value=type('UUID', (), {'hex': nonce})()), \
                contextlib.redirect_stderr(output), self.assertRaisesRegex(RuntimeError, 'optional-remove failed'):
            fixture.qualify('sha256:'+'b'*64)
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[0]['returncode'], 125)
        self.assertTrue(rows[0]['stdout'].endswith('failed stdout'))
        self.assertNotIn('prefix', rows[0]['stdout'])
        self.assertIn('mapping failure', rows[0]['stderr'])
        self.assertIn('PID1 cause', rows[1]['stdout'])
        self.assertIn('recovery log unavailable', rows[2]['stderr'])
        self.assertTrue(all(len(row[stream].encode()) <= fixture.DIAGNOSTIC_BYTES for row in rows for stream in ('stdout', 'stderr')))
        self.assertNotIn(nonce, output.getvalue())
        self.assertNotIn('HOST_PRIVATE', output.getvalue())
        self.assertEqual([args[1] for args in calls], ['info', 'run', 'logs', 'exec', 'rm'])
        self.assertEqual(calls[-1][2:4], ['--force', '--ignore'])

    def test_diagnostic_byte_bound_handles_timeout_bytes_and_multibyte_text(self):
        output = io.StringIO()
        error = subprocess.TimeoutExpired(['must-not-print-argv'], 15,
                                          output=('é'*5000).encode(), stderr=b'\xff'*5000+b'timeout cause')
        with contextlib.redirect_stderr(output):
            fixture.failed_output('bounded timeout', error)
        row = json.loads(output.getvalue())
        self.assertLessEqual(len(row['stdout'].encode()), fixture.DIAGNOSTIC_BYTES)
        self.assertLessEqual(len(row['stderr'].encode()), fixture.DIAGNOSTIC_BYTES)
        self.assertTrue(row['stderr'].endswith('timeout cause'))
        self.assertNotIn('must-not-print-argv', output.getvalue())

    def test_every_inert_startup_effect_is_activated_and_gated(self):
        for name in fixture.transaction.MANAGED:
            unit = fixture.actor_unit('previous', name)
            # Old retained units have no gate: production generator must supply it.
            self.assertNotIn('Requires='+fixture.RECOVERY, unit)
            self.assertNotIn('After='+fixture.RECOVERY, unit)
            self.assertLess(unit.index('ExecStartPre='), unit.index('\nExecStart='))
            self.assertIn('WantedBy='+fixture.TARGET, unit)
        units = fixture.auxiliary('lock-conflict')
        self.assertIn('Wants='+fixture.RECOVERY, units[fixture.TARGET])
        self.assertIn('After='+fixture.RECOVERY, units[fixture.TARGET])
        self.assertIn('Requires=fixture-lock.service\nAfter=fixture-lock.service', units[fixture.RECOVERY + '.d/fixture.conf'])
        self.assertIn('Type=notify', units['fixture-lock.service'])
        self.assertIn('--inner finished', units[fixture.RECOVERY + '.d/fixture.conf'])
        self.assertNotIn('ExecStart=', units[fixture.RECOVERY + '.d/fixture.conf'])

    def test_job_parser_rejects_unbounded_and_unowned_shapes(self):
        name = fixture.CORE[0]
        self.assertEqual(fixture.jobs('12 '+name+' start waiting\n'), {name: '12'})
        self.assertEqual(fixture.jobs(''), {})
        for text in ('x '+name+' start waiting', '1 unrelated.service start waiting',
                     '1 '+name+' stop waiting', '1 '+name+' start done', '1 '+name+' start waiting extra',
                     '1 '+name+' start waiting\n2 '+name+' start waiting', ' '*8193):
            with self.subTest(text=text[:80]), self.assertRaises(RuntimeError):
                fixture.jobs(text)

    def test_resolution_requeries_ids_cancels_removed_roles_and_never_waits(self):
        previous, candidate = fixture.selection('optional-remove')
        owner = unittest.mock.Mock()
        owner.enabled.side_effect = lambda name: 'enabled' if name in previous else 'not-found'
        initial = {name: str(index+1) for index, name in enumerate(candidate)}
        # A reload has assigned fresh IDs. No stale numeric ID may be cancelled.
        current = {name: str(index+100) for index, name in enumerate(candidate)}
        after = {name: str(index+200) for index, name in enumerate(previous)}
        with patch.object(fixture, 'pending_jobs', side_effect=[current, after]), patch.object(fixture, 'record') as record:
            self.assertEqual(set(fixture.resolve(owner, initial)), set(previous))
        owner.command.assert_any_call(['systemctl', 'cancel', current['headunit-vehicle.service']])
        owner.command.assert_any_call(['systemctl', '--no-block', 'start', *previous])
        self.assertEqual(owner.command.call_count, 2)
        self.assertEqual(record.call_args.kwargs['discarded'], ['headunit-vehicle.service'])
        self.assertEqual(set(record.call_args.kwargs['admitted']), set(previous)-set(candidate))
        # Jobs already retired by reload are resolved without cancelling stale IDs.
        owner.reset_mock()
        with patch.object(fixture, 'pending_jobs', side_effect=[{}, after]), patch.object(fixture, 'record'):
            fixture.resolve(owner, initial)
        owner.command.assert_called_once_with(['systemctl', '--no-block', 'start', *previous])
        with patch.object(fixture, 'pending_jobs', side_effect=[current, current]), patch.object(fixture, 'record') as record, self.assertRaises(RuntimeError):
            fixture.resolve(owner, initial)
        record.assert_not_called()

    def evidence(self, case):
        previous, candidate = fixture.selection(case)
        selected = candidate if case == 'complete' else previous
        generation = 'candidate' if case == 'complete' else 'previous'
        rows = [{'kind': 'resolved', 'enabled': list(selected),
                 'cancelled': list(set(candidate)-set(selected)),
                 'discarded': list(set(candidate)-set(selected)),
                 'admitted': list(set(selected)-set(candidate))}, {'kind': 'recovered'}]
        for name in selected:
            rows.extend({'kind': phase, 'generation': generation, 'name': name} for phase in ('preflight', 'owner'))
        return rows

    def test_evidence_refuses_candidate_duplicates_missing_early_or_reversed_owners(self):
        for case in fixture.CASES:
            if case in fixture.FAILURES:
                self.assertEqual(fixture.assess(case, [{'kind': 'failed'}]), set())
            else:
                self.assertEqual(fixture.assess(case, self.evidence(case)), set(fixture.selection(case)[case == 'complete']))
        good = self.evidence('optional-remove')
        bad = [good + [good[-1]], good[:-1], good[:2] + [good[2] | {'generation': 'candidate'}] + good[3:],
               [good[2]] + good[:2] + good[3:], good[:2] + [good[3], good[2]] + good[4:],
               [good[0] | {'admitted': []}] + good[1:], [good[0] | {'discarded': []}] + good[1:]]
        for rows in bad:
            with self.subTest(rows=rows), self.assertRaises(AssertionError):
                fixture.assess('optional-remove', rows)
        with self.assertRaises(AssertionError):
            fixture.assess('damaged', [{'kind': 'failed'}, good[-1]])
        with self.assertRaises(AssertionError):
            fixture.assess('lock-conflict', [])


if __name__ == '__main__':
    unittest.main()
