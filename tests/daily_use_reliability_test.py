#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Synthetic parsing, summary and process-retirement checks; no phone/CM5 I/O."""
import copy
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import daily_use_reliability as harness
import reliability_probe as probe
import recovery_content_evidence as content


class ParsingTests(unittest.TestCase):
    def test_original_journal_cursor_with_empty_display_preamble(self):
        cursor = 's=fixture;i=12;b=fixture;m=34;t=56;x=78'
        self.assertEqual(probe.journal_cursor('-- No entries --\n-- cursor: ' + cursor + '\n'), cursor)
        self.assertEqual(probe.journal_cursor('-- cursor: ' + cursor + '\n'), cursor)
        for output in ('-- No entries --\n', '-- cursor: \n',
                       '-- cursor: ' + cursor + '\n-- cursor: ' + cursor,
                       '-- cursor: invalid cursor\n', '-- cursor: ' + 'x' * 4097):
            with self.subTest(output=output[:60]), self.assertRaises(ValueError):
                probe.journal_cursor(output)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name).resolve()
        self.sim = harness.Simulation()

    def test_current_ready_and_missing_stale_future_foreign(self):
        sample = self.sim.probe('sample')
        self.assertTrue(harness.ready(sample))
        for key, value in (('updatedMonotonicNs', '1'), ('updatedMonotonicNs', str(10 ** 15)),
                           ('connectionGeneration', '0'), ('returnReady', False),
                           ('negotiatedMode', 'standard'), ('sequence', '01'), ('nonce', 'bad')):
            broken = copy.deepcopy(sample)
            broken['receipt'][key] = value
            self.assertFalse(harness.ready(broken), key)
        for key in ('manager', 'receipt', 'remote_monotonic_ns', 'host_started_ns'):
            broken = copy.deepcopy(sample)
            broken.pop(key)
            self.assertFalse(harness.ready(broken), key)
        sample['manager']['phone_home']['connection'] = '99'
        self.assertFalse(harness.ready(sample))

    def test_replayed_sequence_and_stalled_transport(self):
        previous = self.sim.probe('sample')
        current = self.sim.probe('sample')
        self.assertTrue(harness.ready(current, previous))
        current['receipt']['sequence'] = previous['receipt']['sequence']
        self.assertFalse(harness.ready(current, previous))
        current = self.sim.probe('sample')
        current['host_finished_ns'] = current['host_started_ns'] + 6_000_000_000
        self.assertFalse(harness.ready(current))

    def test_phone_errors_empty_failed_and_oversized(self):
        path = self.path / 'phone.log'
        path.write_text('Oct 13 20:12:01 phone CarPlay Video frame timed out\n'
                        'Oct 13 20:12:02 phone AirPlay failed to start\n'
                        'Oct 13 20:12:03 phone normal frame accepted\n')
        collection = {'complete': True, 'phone_lines': 3}
        result = harness.phone_errors(path, collection)
        self.assertTrue(result['available'])
        self.assertEqual(result['counts'], {'video_frame_timeout': 1, 'carplay_error': 1})
        self.assertFalse(harness.phone_errors(path, {'complete': False})['available'])
        path.write_text('')
        result = harness.phone_errors(path, collection)
        self.assertFalse(result['available'])
        self.assertIsNone(result['counts']['video_frame_timeout'])
        path.write_bytes(b'x' * (harness.MAX_LINE + 1))
        self.assertFalse(harness.phone_errors(path, collection)['available'])

    def test_journal_malformed_truncated_empty_and_failure(self):
        path = self.path / 'journal.jsonl'
        path.write_text(json.dumps({'__CURSOR': 'a', '_BOOT_ID': 'b',
                                    'MESSAGE': 'receiver failed'}) + '\n')
        result = harness.journal_summary(path, {'complete': True})
        self.assertTrue(result['available'])
        self.assertEqual(result['failure_records'], 1)
        self.assertFalse(harness.journal_summary(path, {'complete': False})['available'])
        path.write_text('{bad\n')
        self.assertFalse(harness.journal_summary(path, {'complete': True})['available'])
        path.write_text('')
        self.assertEqual(harness.journal_summary(path, {'complete': True})['records'], 0)
        path.write_text((json.dumps({'__CURSOR': 'a'}) + '\n') * 10001)
        self.assertFalse(harness.journal_summary(path, {'complete': True})['available'])

    def test_restart_counts_do_not_double_count_invocation_change(self):
        samples = [self.sim.probe('sample') for _ in range(4)]
        owner = 'headunit-receiver'
        for index, sample in enumerate(samples):
            sample['owners'][owner].update(NRestarts=str(min(index, 2)), InvocationID=str(index))
        result = harness.restart_summary(samples)[owner]
        self.assertEqual(result['automatic_restarts'], 2)
        self.assertEqual(result['observed_invocation_changes'], 3)
        samples[-1]['owners'][owner]['NRestarts'] = '0'
        self.assertIsNone(harness.restart_summary(samples)[owner]['automatic_restarts'])
        samples[-1]['owners'].pop(owner)
        self.assertIsNone(harness.restart_summary(samples)[owner]['automatic_restarts'])

    def test_summary_missing_readings_and_phone_failure_cannot_pass(self):
        samples = [self.sim.probe('sample')]
        cases = [{'kind': 'cycle', 'passed': True}]
        phone = {'available': True, 'counts': {'video_frame_timeout': 0}}
        journal = [{'available': True, 'failure_records': 0}]
        self.assertTrue(harness.summarize(cases, samples, phone, journal, 1)['passed'])
        phone['counts']['video_frame_timeout'] = 1
        result = harness.summarize(cases, samples, phone, journal, 1)
        self.assertFalse(result['passed'])
        self.assertTrue(result['logical_recovery_passed'])
        self.assertEqual(result['phone_errors']['counts']['video_frame_timeout'], 1)
        phone['available'] = False
        result = harness.summarize(cases, samples, phone, [], 2, interrupted=True)
        self.assertFalse(result['passed'])
        self.assertIn('run_incomplete', result['failures'])
        self.assertIn('journal_unavailable', result['failures'])
        self.assertFalse(harness.summarize(cases, [], phone, journal, 1)['passed'])

    def test_unexpected_owner_replacement_fails_but_declared_restart_is_allowed(self):
        first = self.sim.probe('sample')
        self.sim.probe('restart')
        last = self.sim.probe('sample')
        phone = {'available': True, 'counts': {'video_frame_timeout': 0}}
        journal = [{'available': True, 'failure_records': 0}]
        self.assertTrue(harness.summarize([{'kind': 'restart', 'passed': True}],
                                        [first, last], phone, journal, 1)['passed'])
        result = harness.summarize([{'kind': 'cycle', 'passed': True}],
                                   [first, last], phone, journal, 1)
        self.assertIn('headunit-receiver:unexpected_invocation_change', result['failures'])

    def test_all_scenarios_and_failed_connect_stop_without_recovery(self):
        args = harness.arguments(['--output', 'unused', '--dry-run', '--cycles', '2',
                                  '--burst-connects', '2', '--link-loss-cycles', '1',
                                  '--service-restarts', '1', '--cold-boots', '1'])
        cases, journals, events = [], [], []
        harness.run_cases(self.sim, args, lambda event, value: events.append((event, value)), cases, journals)
        self.assertEqual(len(cases), 7)
        self.assertTrue(all(case['passed'] for case in cases))
        self.assertTrue(all(case['start_to_first_frame_s'] is None for case in cases))
        self.assertTrue(all(case['start_to_readiness_upper_bound_s'] > 0 for case in cases))
        events.clear()
        cases.clear()
        journals.clear()
        args.dry_run_failure = True
        harness.run_cases(harness.Simulation(True), args, lambda event, value: events.append((event, value)), cases, journals)
        self.assertEqual(len(cases), 1)
        self.assertFalse(cases[0]['passed'])
        self.assertEqual(sum(event == 'connect' for event, _ in events), 1)

    def test_private_receipt_rejects_redirects_and_wrong_permissions(self):
        folder = self.path / 'state'
        folder.mkdir(mode=0o700)
        path = folder / 'carplay-status.json'
        path.write_text(json.dumps(self.sim.probe('sample')['receipt']))
        path.chmod(0o600)
        self.assertEqual(probe.receipt(folder)['state'], 'ready')
        path.chmod(0o644)
        with self.assertRaises(ValueError):
            probe.receipt(folder)
        path.unlink()
        path.symlink_to(self.path / 'other')
        with self.assertRaises(OSError):
            probe.receipt(folder)

    def test_real_capture_retirement_overflow_and_early_phone_exit(self):
        capture = harness.Capture([sys.executable, '-c', 'import time; print("ready", flush=True); time.sleep(30)'],
                                  self.path / 'capture.log')
        collection = capture.stop()
        self.assertIsNotNone(capture.process.poll())
        self.assertFalse(collection['complete'])  # An interrupted journal is incomplete.
        self.assertFalse(capture.thread.is_alive())
        capture = harness.Capture([sys.executable, '-c', 'print("x" * 200000)'],
                                  self.path / 'overflow.log', maximum=100)
        capture.process.wait(timeout=3)
        collection = capture.stop()
        self.assertTrue(collection['overflow'])
        self.assertFalse(collection['complete'])
        capture = harness.Capture([sys.executable, '-c', 'print("Oct 13 20:12:01 normal")'],
                                  self.path / 'phone-exit.log', phone=True)
        capture.process.wait(timeout=3)
        self.assertFalse(capture.stop()['complete'])

    def test_manager_selected_phone_stays_inside_remote_helper(self):
        # A real Unix IPC fixture verifies framing and rejection, no phone identity needed.
        path = self.path / 'manager.sock'
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(str(path))
            server.listen(1)
            commands = []
            def reply():
                client, _ = server.accept()
                with client:
                    commands.append(json.loads(client.recv(1024)))
                    client.sendall(b'{"ok":true,"generation":7}\n')
            thread = threading.Thread(target=reply)
            thread.start()
            self.assertEqual(probe.request(self.path, {'action': 'status'})['generation'], 7)
            thread.join(timeout=2)
            self.assertEqual(commands, [{'action': 'status'}])

    def test_malformed_manager_objects_and_expected_offline_counters(self):
        value = self.sim.probe('sample')
        for malformed in (None, [], 'bad'):
            broken = copy.deepcopy(value)
            broken['manager']['phone_home'] = malformed
            self.assertFalse(harness.ready(broken))
        offline = {'probe_error': 'unavailable', 'expected_offline': True}
        self.sim.boot += 1
        resumed = self.sim.probe('sample')
        result = harness.restart_summary([value, offline, resumed])
        for owner in probe.OWNERS:
            self.assertEqual(result[owner]['automatic_restarts'], 0)
            self.assertEqual(result[owner]['expected_offline_samples'], 1)
        offline['expected_offline'] = False
        self.assertIsNone(harness.restart_summary([value, offline, resumed])['headunit-receiver']['automatic_restarts'])

    def test_capture_write_failure_marks_evidence_unavailable_and_retires_process(self):
        file = mock.Mock()
        file.write.side_effect = OSError('fixture storage failure')
        with mock.patch.object(Path, 'open', return_value=file):
            capture = harness.Capture([sys.executable, '-c', 'print("ready", flush=True)'],
                                      self.path / 'failed-write.log')
        capture.thread.join(timeout=3)
        result = capture.stop()
        self.assertEqual(result['drain_error'], 'OSError')
        self.assertFalse(result['complete'])
        self.assertIsNotNone(capture.process.poll())

    def test_remote_probe_sandwich_rejects_publisher_switch(self):
        first = self.sim.probe('sample')
        other = copy.deepcopy(first['receipt'])
        other['nonce'] = 'f' * 32
        with mock.patch.object(sys, 'argv', ['probe', 'sample', str(self.path), '']), \
                mock.patch.object(Path, 'read_text', return_value='fixture-boot'), \
                mock.patch.object(probe, 'request', return_value=first['manager']), \
                mock.patch.object(probe, 'receipt', side_effect=[first['receipt'], other]), \
                mock.patch.object(probe, 'services', return_value=first['owners']), \
                mock.patch.object(sys, 'stdout', new_callable=io.StringIO) as output:
            probe.main()
            self.assertIsNone(json.loads(output.getvalue())['receipt'])

    def test_remote_connect_uses_selected_paired_listing_without_exporting_identity(self):
        identity = 'PRIVATE-FIXTURE-PHONE'
        status = {'ok': True, 'phones': [{'selected': True, 'address': identity}]}
        with mock.patch.object(sys, 'argv', ['probe', 'connect', str(self.path), '']), \
                mock.patch.object(Path, 'read_text', return_value='fixture-boot'), \
                mock.patch.object(probe, 'request', side_effect=[status, {'ok': True}]) as request, \
                mock.patch.object(sys, 'stdout', new_callable=io.StringIO) as output:
            probe.main()
            self.assertNotIn(identity, output.getvalue())
            self.assertTrue(json.loads(output.getvalue())['acknowledged'])
            self.assertEqual(request.call_args_list[1].args[1], {'action': 'connect', 'phone': identity})

    def test_interrupted_live_driver_writes_failed_report_and_retires_collector(self):
        collector = mock.Mock(phone_lines=1)
        collector.process.poll.return_value = None
        collection = {'complete': True, 'phone_lines': 1, 'bytes': 30}
        collector.stop.return_value = collection
        def capture(_command, path, **_options):
            path.write_text('Oct 13 20:12:01 normal phone line\n')
            return collector
        with mock.patch.object(harness, 'evidence_folder', return_value=self.path), \
                mock.patch.object(harness, 'Capture', side_effect=capture), \
                mock.patch.object(harness, 'Bench'), \
                mock.patch.object(harness, 'run_cases', side_effect=KeyboardInterrupt), \
                mock.patch.object(sys, 'stdout', new_callable=io.StringIO):
            before = os.umask(0o077)
            try:
                result = harness.main(['--output', 'unused', '--cycles', '1', '--burst-connects', '0',
                                       '--host', 'bench.example.invalid', '--state-directory', '/tmp/state'])
            finally:
                os.umask(before)
        collector.stop.assert_called_once()
        self.assertEqual(result, 1)
        summary = json.loads((self.path / 'summary.json').read_text())
        self.assertTrue(summary['interrupted'])
        self.assertFalse(summary['passed'])
        self.assertEqual(summary['completed_cases'], 0)
        self.assertIn('run_incomplete', summary['failures'])
        self.assertTrue((self.path / 'evidence-sha256.json').exists())

    def test_streamed_receipts_match_in_memory_counts_and_reject_partial_records(self):
        path = self.path / 'receipts.jsonl'
        values = [self.sim.probe('sample') for _ in range(3)]
        path.write_text(''.join(json.dumps({'event': 'sample', **value}) + '\n' for value in values))
        self.assertEqual(harness.restart_summary(harness.iter_samples(path)), harness.restart_summary(values))
        with path.open('a') as stream:
            stream.write('{incomplete')
        result = harness.restart_summary(harness.iter_samples(path))
        self.assertIsNone(result['headunit-receiver']['automatic_restarts'])

    def test_dead_phone_collector_blocks_live_manager_actions(self):
        args = harness.arguments(['--output', 'unused', '--host', 'bench.example.invalid',
                                  '--state-directory', '/tmp/state'])
        backend = harness.Bench(args, self.path)
        backend.collector = mock.Mock()
        backend.collector.process.poll.return_value = 1
        with mock.patch.object(subprocess, 'run', side_effect=AssertionError('No remote action after collector loss')):
            with self.assertRaisesRegex(ValueError, 'phone_collector_stopped'):
                backend.probe('connect')

    def test_dry_run_backend_never_invokes_external_tools(self):
        args = harness.arguments(['--dry-run', '--output', 'unused', '--cycles', '1', '--burst-connects', '0'])
        with mock.patch.object(subprocess, 'run', side_effect=AssertionError('No hardware subprocess permitted')), \
                mock.patch.object(subprocess, 'Popen', side_effect=AssertionError('No collector permitted')):
            cases, journals = [], []
            harness.run_cases(self.sim, args, lambda *_: None, cases, journals)
        self.assertTrue(cases[0]['passed'])

    def test_logical_ready_does_not_supply_first_visible_frame(self):
        args = harness.arguments(['--dry-run', '--output', 'unused', '--cycles', '1', '--burst-connects', '0'])
        cases, journals = [], []
        harness.run_cases(self.sim, args, lambda *_: None, cases, journals)
        case = cases[0]
        self.assertTrue(case['passed'])
        self.assertGreater(case['start_to_readiness_upper_bound_s'], 0)
        self.assertIsNone(case['start_to_first_frame_upper_bound_s'])
        self.assertIsNone(case['start_to_first_frame_s'])
        self.assertIsNone(case['usable_content']['candidate'])
        summary = harness.summarize(cases, [self.sim.probe('sample')],
                                    {'available': True, 'counts': {'video_frame_timeout': 0}},
                                    journals, 1)
        self.assertTrue(summary['passed'])
        self.assertFalse(summary['bench_qualified'])
        self.assertIsNone(summary['usable_content']['fresh_scene_qualified'])
        self.assertEqual(summary['failed_cases'], 0)
        self.assertEqual(summary['incomplete_cases'], 0)

    def test_failed_and_incomplete_counts_are_distinct(self):
        result = harness.summarize([{'passed': False}], [self.sim.probe('sample')],
                                   {'available': True, 'counts': {'video_frame_timeout': 0}}, [], 10)
        self.assertEqual(result['failed_cases'], 1)
        self.assertEqual(result['incomplete_cases'], 9)
        self.assertIsNone(result['usable_content']['candidate'])

    def test_supervised_fallback_is_separate_from_optional_manager_child(self):
        sample = self.sim.probe('sample')
        status = {**sample['manager'], 'fallback_running': False, 'cluster_page': 'trip'}
        with mock.patch.object(sys, 'argv', ['probe', 'sample', str(self.path), '']), \
                mock.patch.object(Path, 'read_text', return_value='fixture-boot'), \
                mock.patch.object(probe, 'request', return_value=status), \
                mock.patch.object(probe, 'receipt', return_value=sample['receipt']), \
                mock.patch.object(probe, 'services', return_value=sample['owners']), \
                mock.patch.object(sys, 'stdout', new_callable=io.StringIO) as output:
            probe.main()
        value = json.loads(output.getvalue())
        self.assertFalse(value['fallback']['optional_manager_child_running'])
        self.assertEqual(value['owners']['headunit-cluster']['ActiveState'], 'active')
        self.assertEqual(value['fallback']['manager_native_page'], 'trip')
        self.assertIsNone(value['fallback']['visible'])
        # Optional/missing child status is unavailable, never a manufactured zero.
        status.pop('fallback_running')
        status.pop('cluster_page')
        with mock.patch.object(sys, 'argv', ['probe', 'sample', str(self.path), '']), \
                mock.patch.object(Path, 'read_text', return_value='fixture-boot'), \
                mock.patch.object(probe, 'request', return_value=status), \
                mock.patch.object(probe, 'receipt', return_value=sample['receipt']), \
                mock.patch.object(probe, 'services', return_value=sample['owners']), \
                mock.patch.object(sys, 'stdout', new_callable=io.StringIO) as output:
            probe.main()
        value = json.loads(output.getvalue())
        self.assertIsNone(value['fallback']['optional_manager_child_running'])
        self.assertIsNone(value['fallback']['manager_native_page'])

    def test_current_state_preconditions_reject_missing_or_ambiguous_phone_before_action(self):
        for operation, status in (
                ('connect', {'phones': []}),
                ('connect', {'phones': [{'selected': True}, {'selected': True}]}),
                ('disconnect', {'active': None, 'generation': 19})):
            with self.subTest(operation=operation, status=status), \
                    mock.patch.object(sys, 'argv', ['probe', operation, str(self.path), '']), \
                    mock.patch.object(Path, 'read_text', return_value='fixture-boot'), \
                    mock.patch.object(probe, 'request', return_value=status) as request:
                with self.assertRaises(ValueError):
                    probe.main()
                self.assertEqual(request.call_count, 1)  # Read-only status, no guessed action.


class ContentReviewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name).resolve()
        self.archive = self.folder / 'receipts.jsonl'
        self.sim = harness.Simulation()
        args = harness.arguments(['--dry-run', '--output', 'unused', '--cycles', '1', '--burst-connects', '0'])
        events, cases, journals = [], [], []
        harness.run_cases(self.sim, args, lambda event, value: events.append(
            copy.deepcopy({'event': event, **value})),
                          cases, journals)
        self.events = events
        self.line = next(number for number, event in enumerate(events, 1)
                         if event.get('event') == 'sample' and event.get('case_index') == 1 and
                         event.get('receipt', {}).get('nonce') == cases[0]['first_ready_receipt']['nonce'] and
                         harness.ready(event))
        self.write_archive()
        # Synthetic bytes, explicitly not a real panel screenshot.
        capture = self.folder / 'fixture.png'
        capture.write_bytes(b'synthetic capture fixture')
        self.digest = content.file_hash(capture, content.MAX_CAPTURE)

    def write_archive(self):
        self.archive.write_text(''.join(json.dumps(event) + '\n' for event in self.events))

    def reviews(self, main='blank', cluster='logos'):
        return [{'sample_line': self.line, 'panel': panel, 'classification': classification,
                 'capture': 'fixture.png', 'sha256': self.digest}
                for panel, classification in (('main', main), ('cluster', cluster))]

    def test_v84_shaped_blank_logos_and_settled_content_never_prove_fresh_scene(self):
        for main, cluster, candidate in (('blank', 'logos', False), ('startup', 'content', False),
                                         ('nonpresenting', 'content', False),
                                         ('stale_video', 'content', False),
                                         ('content', 'content', True), ('unknown', 'content', None)):
            with self.subTest(main=main, cluster=cluster):
                report = content.assess(self.archive, self.reviews(main, cluster), self.folder)
                self.assertEqual(report['logical_successful_cases'], 1)
                observation = report['cases'][0]['reviewed_samples'][0]
                self.assertIs(observation['candidate'], candidate)
                self.assertIsNone(observation['capture_date_ns'])
                self.assertIsNone(observation['fresh_scene_qualified'])
                self.assertEqual(observation['receipt_sequence'],
                                 self.events[self.line - 1]['receipt']['sequence'])
                self.assertFalse(report['bench_qualified'])
                self.assertIsNone(report['optical_first_frame_s'])
        report = content.assess(self.archive, self.reviews('content', 'content')[:1], self.folder)
        self.assertIsNone(report['cases'][0]['reviewed_samples'][0]['candidate'])

    def test_no_review_remains_unknown(self):
        report = content.assess(self.archive, [], None)
        self.assertEqual(report['reviewed_candidate_samples'], 0)
        self.assertEqual(report['cases'][0]['reviewed_samples'], [])
        self.assertIsNone(report['cases'][0]['usable_content']['candidate'])

    def test_replacement_retirement_replay_and_missing_source_context_rejected(self):
        original = copy.deepcopy(self.events)
        for mutation in ('old_generation', 'stale', 'missing_boot', 'before_recovery',
                         'date_reversal', 'retired'):
            self.events = copy.deepcopy(original)
            sample = self.events[self.line - 1]
            if mutation == 'old_generation':
                sample['receipt']['nonce'] = 'f' * 32
            elif mutation == 'stale':
                sample['receipt']['updatedMonotonicNs'] = '1'
            elif mutation == 'missing_boot':
                sample.pop('boot_id')
            elif mutation == 'before_recovery':
                sample['receipt']['sequence'] = '1'
            elif mutation == 'date_reversal':
                sample['receipt']['updatedMonotonicNs'] = str(sample['remote_monotonic_ns'] - 1)
            else:
                sample['receipt']['returnReady'] = False
            self.write_archive()
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                content.assess(self.archive, self.reviews(), self.folder)

    def test_tampered_missing_redirected_and_oversized_captures_fail_closed(self):
        reviews = self.reviews()
        reviews[0]['sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            content.assess(self.archive, reviews, self.folder)
        reviews[0]['capture'] = 'missing.png'
        with self.assertRaises(OSError):
            content.assess(self.archive, reviews, self.folder)
        (self.folder / 'redirect.png').symlink_to(self.folder / 'fixture.png')
        reviews[0]['capture'] = 'redirect.png'
        with self.assertRaises(ValueError):
            content.assess(self.archive, reviews, self.folder)
        with mock.patch.object(content, 'MAX_CAPTURE', 1), self.assertRaises(ValueError):
            content.assess(self.archive, self.reviews(), self.folder)

    def test_review_schema_bounds_duplicate_and_unsafe_paths(self):
        path = self.folder / 'review.json'
        path.write_text(json.dumps({'version': 1, 'reviews': self.reviews()}))
        self.assertEqual(content.read_review(path), self.reviews())
        for reviews in (self.reviews() * 2, [{**self.reviews()[0], 'capture': '../elsewhere.png'}],
                        [{**self.reviews()[0], 'capture': '/tmp/elsewhere.png'}],
                        [{**self.reviews()[0], 'classification': 'guessed_pixels'}],
                        [{**self.reviews()[0], 'sample_line': True}], self.reviews() * 1001):
            path.write_text(json.dumps({'version': 1, 'reviews': reviews}))
            with self.assertRaises(ValueError):
                content.read_review(path)
        path.write_text('x' * (content.MAX_REVIEW + 1))
        with self.assertRaises(ValueError):
            content.read_review(path)

    def test_archive_bounds_partial_duplicate_and_missing_anchor(self):
        with self.archive.open('a') as stream:
            stream.write('{partial')
        with self.assertRaises(ValueError):
            content.assess(self.archive, [], None)
        self.write_archive()
        reviews = self.reviews()
        reviews[0]['sample_line'] = len(self.events) + 1
        with self.assertRaises(ValueError):
            content.assess(self.archive, reviews, self.folder)
        self.events.append(copy.deepcopy(self.events[-1]))
        self.write_archive()
        with self.assertRaises(ValueError):
            content.assess(self.archive, [], None)
        with mock.patch.object(content, 'MAX_ARCHIVE', 1), self.assertRaises(ValueError):
            content.assess(self.archive, [], None)

    def test_offline_driver_has_no_hardware_calls_and_records_source_and_unknowns(self):
        output = self.folder / 'report'
        output.mkdir()
        with mock.patch.object(harness, 'evidence_folder', return_value=output), \
                mock.patch.object(harness, 'source_commit', return_value='f' * 40), \
                mock.patch.object(subprocess, 'run', side_effect=AssertionError('No hardware calls')), \
                mock.patch.object(subprocess, 'Popen', side_effect=AssertionError('No hardware calls')), \
                mock.patch.object(sys, 'stdout', new_callable=io.StringIO):
            self.assertEqual(content.main(['--receipts', str(self.archive), '--output', 'unused']), 0)
        report = json.loads((output / 'content-review.json').read_text())
        self.assertEqual(report['source_commit'], 'f' * 40)
        self.assertEqual(report['inputs_sha256']['receipts'], content.file_hash(self.archive, content.MAX_ARCHIVE))
        self.assertIsNone(report['inputs_sha256']['review'])
        self.assertIsNone(report['fresh_scene_qualified_cases'])
        self.assertFalse(report['bench_qualified'])

    def test_nonregular_archive_is_rejected_without_blocking(self):
        fifo = self.folder / 'fifo'
        os.mkfifo(fifo)
        with self.assertRaises(ValueError):
            content.assess(fifo, [], None)


if __name__ == '__main__':
    unittest.main()
