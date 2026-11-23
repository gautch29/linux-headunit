# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Exercise the offline production parser/auditor with explicit synthetic dates.

No phone captures or measured performance claims are fixtures in Git.
"""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import video_attribution as video


def report(date, presented, decoded, scenes, *, first=False, panel=0, **changes):
    fields = dict(panel=panel, mono_ns=date, scenes=scenes, decoded=decoded,
                  videoPresented=presented, late=0, clockDropped=0,
                  **{key: 0 for key in video.STATE_COUNTS})
    if not first:
        fields.update(dict(videoSubmittedWindow=10, videoDecodedWindow=10,
                           videoSupersededWindow=2, videoDiscardedWindow=0,
                           videoLateWindow=0, videoClockDroppedWindow=0,
                           videoNonMonotonicWindow=0, videoBackpressureWindow=0,
                           fps=60, freshVideoFps=1.6, videoSubmittedFps=2))
    fields.update(changes)
    return 'Ultra display diagnostic ' + ' '.join(f'{key}={value}' for key, value in fields.items())


def sample(date, sequence=1, generation=7):
    return {'remote_sampled_ns': date, 'carplay': {'returnReady': True},
            'cluster': {'ready': True, 'nonce': 'synthetic-private-publisher',
                        'connectionGeneration': generation, 'sequence': sequence}}


class AuditTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lines = []
        for index, date in enumerate((1_000_000_000, 6_000_000_000, 11_000_000_000)):
            for panel in (0, 1):
                self.lines.append(report(date, index * 8, index * 10, index * 300,
                                         first=index == 0, panel=panel))
        self.rows = [sample(1_000_000_000), sample(11_000_000_000, 2)]
        self.manifest = {'schema': 1, 'captures': [{
            'label': 'synthetic-owner', 'source_revision': 'a' * 40,
            'clock_domain': 'synthetic-host-epoch', 'owner_scope': 'synthetic-invocation',
            'report_line_range': None,
            'reports': self.write('reports.txt', '\n'.join(self.lines) + '\n'),
            'phases': [{'label': 'moving', 'samples': self.write('samples.json', self.rows)}],
        }]}

    def write(self, name, value):
        raw = value.encode() if isinstance(value, str) else json.dumps(value).encode()
        (self.root / name).write_bytes(raw)
        return {'path': name, 'sha256': hashlib.sha256(raw).hexdigest()}

    def refresh(self):
        capture = self.manifest['captures'][0]
        capture['reports'] = self.write('reports.txt', '\n'.join(self.lines) + '\n')
        capture['phases'][0]['samples'] = self.write('samples.json', self.rows)

    def audit(self):
        self.refresh()
        return video.audit(self.manifest, video.Inputs(self.root))

    def test_original_dates_whole_windows_and_redraw_separation(self):
        result = self.audit()
        self.assertEqual(result['status'], 'consistent_windows')
        for phase in result['phases']:
            self.assertEqual(phase['whole_report_pairs'], 2)
            self.assertEqual(phase['audited_report_span_ns'], 10_000_000_000)
            self.assertEqual(phase['presented_frames'], 16)
            self.assertEqual(phase['weighted_fresh_fps'], 1.6)
            self.assertEqual(phase['windows'][0]['scene_redraws'], 300)
            self.assertEqual(phase['windows'][0]['previous_report_mono_ns'],
                             self.rows[0]['remote_sampled_ns'])
            self.assertIsNone(phase['windows'][0]['producer_window_start_mono_ns'])
        self.assertIsNone(result['attribution']['timeout_cause'])
        self.assertIsNone(result['attribution']['source_frame_to_admission_to_receipt'])
        self.assertEqual(result['attribution']['display_report_to_sample_publisher'],
                         'operator_scope_only_not_embedded_in_report')
        self.assertNotIn('synthetic-private-publisher', json.dumps(result))

    def test_phase_edges_do_not_borrow_nominal_duration(self):
        self.rows[0]['remote_sampled_ns'] += 1
        result = self.audit()
        self.assertEqual([p['whole_report_pairs'] for p in result['phases']], [1, 1])
        self.assertEqual(result['phases'][0]['audited_report_span_ns'], 5_000_000_000)

    def test_missing_report_does_not_join_wrong_window(self):
        del self.lines[2]
        result = self.audit()
        self.assertEqual(result['status'], 'incomplete')
        self.assertEqual(result['phases'][0]['whole_report_pairs'], 0)
        self.assertEqual(result['phases'][0]['rejected_windows'][0]['reason'],
                         'interval_counter_mismatch')
        self.assertEqual(result['phases'][1]['whole_report_pairs'], 2)

    def test_no_interval_counts_does_not_upgrade_cumulative_total(self):
        self.lines[2] = report(6_000_000_000, 8, 10, 300, first=True)
        result = self.audit()
        self.assertEqual(result['status'], 'incomplete')
        self.assertEqual(result['phases'][0]['whole_report_pairs'], 1)

    def test_retirement_split_has_no_cross_capture_join(self):
        self.rows[0]['remote_sampled_ns'] = 6_000_000_000
        capture = self.manifest['captures'][0]
        retired = copy.deepcopy(capture)
        retired['label'] = 'retired-owner'
        retired['reports'] = self.write('retired.txt', self.lines[0] + '\n' + self.lines[1] + '\n')
        retired['phases'][0]['samples'] = self.write(
            'retired-samples.json', [sample(1_000_000_000), sample(5_000_000_000)])
        self.manifest['captures'].insert(0, retired)
        # Refresh manually because audit() assumes the first capture is active.
        capture['phases'][0]['samples'] = self.write('samples.json', self.rows)
        result = video.audit(self.manifest, video.Inputs(self.root))
        self.assertEqual([p['whole_report_pairs'] for p in result['phases']], [0, 0, 1, 1])

    def test_missing_panel_remains_unavailable(self):
        self.lines = self.lines[::2]
        result = self.audit()
        self.assertEqual(result['status'], 'incomplete')
        self.assertIsNone(result['phases'][1]['weighted_fresh_fps'])
        self.assertIsNone(result['phases'][1]['presented_frames'])
        self.assertTrue(all(value is None for value in result['phases'][1]['counts'].values()))

    def test_reordered_duplicate_reset_reports_are_terminal(self):
        original = list(self.lines)
        variants = [original + [original[2]], [original[2], original[0]],
                    [report(1_000_000_000, 8, 10, 300, first=True),
                     report(6_000_000_000, 0, 0, 0, first=True)]]
        for lines in variants:
            with self.subTest(lines=len(lines)):
                self.lines = lines
                with self.assertRaises(video.EvidenceError):
                    self.audit()

    def test_publisher_restart_or_reordered_samples_require_split(self):
        originals = copy.deepcopy(self.rows)
        for mutation in ('generation', 'nonce', 'date', 'sequence', 'ready'):
            with self.subTest(mutation=mutation):
                self.rows = copy.deepcopy(originals)
                if mutation == 'generation':
                    self.rows[1]['cluster']['connectionGeneration'] += 1
                elif mutation == 'nonce':
                    self.rows[1]['cluster']['nonce'] = 'successor'
                elif mutation == 'date':
                    self.rows[1]['remote_sampled_ns'] = self.rows[0]['remote_sampled_ns']
                elif mutation == 'sequence':
                    self.rows[1]['cluster']['sequence'] = 0
                else:
                    self.rows[1]['carplay']['returnReady'] = False
                with self.assertRaises(video.EvidenceError):
                    self.audit()

    def test_counter_and_rate_inconsistency_reject_only_affected_window(self):
        self.lines[2] = report(6_000_000_000, 8, 10, 300, freshVideoFps=60)
        result = self.audit()
        self.assertEqual(result['phases'][0]['whole_report_pairs'], 1)
        self.assertEqual(result['phases'][0]['rejected_windows'][0]['reason'], 'interval_rate_mismatch')

    def test_source_clock_loss_is_separate_from_host_report_time(self):
        self.lines[2] = report(6_000_000_000, 8, 10, 300, clockUnavailable=1)
        self.lines[4] = report(11_000_000_000, 16, 20, 600, clockUnavailable=1)
        result = self.audit()
        window = result['phases'][0]['windows'][0]
        self.assertEqual(window['owner_clock_surface_deltas']['clockUnavailable'], 1)
        self.assertIsNone(result['attribution']['phone_to_host_clock'])

    def test_pipeline_residual_is_not_forced_to_balance(self):
        self.lines[2] = report(6_000_000_000, 8, 10, 300, videoSupersededWindow=3)
        self.assertEqual(self.audit()['phases'][0]['windows'][0]
                         ['decoded_minus_presented_superseded'], -1)

    def test_original_nanosecond_precision_and_numeric_rejection(self):
        date = (1 << 63) + 17
        parsed = video.parse_reports(report(date, 0, 0, 0, first=True).encode())
        self.assertEqual(parsed[0]['mono_ns'], date)
        for extra in (' mono_ns=1', ' freshVideoFps=NaN', ' videoPresented=-1',
                      ' mono_ns=18446744073709551616'):
            with self.subTest(extra=extra), self.assertRaises(video.EvidenceError):
                video.parse_reports((self.lines[0] + extra).encode())

    def test_existing_published_decimal_strings_are_exact(self):
        for row in self.rows:
            for key in ('sequence', 'connectionGeneration'):
                row['cluster'][key] = str(row['cluster'][key])
        self.assertEqual(self.audit()['status'], 'consistent_windows')
        self.assertEqual(video.published_integer('18446744073709551615'), (1 << 64) - 1)
        for value in ('1e3', '1.0', '18446744073709551616', True, 1.0):
            with self.subTest(value=value), self.assertRaises(video.EvidenceError):
                video.published_integer(value)

    def test_concatenated_retired_owner_requires_explicit_original_line_range(self):
        self.lines += [self.lines[0], self.lines[1]]
        with self.assertRaises(video.EvidenceError):
            self.audit()
        self.manifest['captures'][0]['report_line_range'] = [1, 6]
        result = self.audit()
        self.assertEqual([p['whole_report_pairs'] for p in result['phases']], [2, 2])
        self.assertEqual(result['phases'][0]['report_line_range_declared'], [1, 6])
        self.assertEqual(result['phases'][0]['windows'][-1]['end_line'], 5)

    def test_bounds_hash_regular_file_and_mutation(self):
        reference = self.manifest['captures'][0]['reports']
        with self.assertRaises(video.EvidenceError):
            video.Inputs(self.root).read(reference, 1)
        broken = dict(reference, sha256='0' * 64)
        with self.assertRaises(video.EvidenceError):
            video.Inputs(self.root).read(broken)
        (self.root / 'link').symlink_to(self.root / reference['path'])
        with self.assertRaises(OSError):
            video.Inputs(self.root).read(dict(reference, path='link'))
        import os
        os.mkfifo(self.root / 'fifo')
        with self.assertRaises(video.EvidenceError):
            video.Inputs(self.root).read(dict(reference, path='fifo'))
        real_read = os.read
        def changed(fd, size):
            raw = real_read(fd, size)
            (self.root / reference['path']).write_bytes(b'replaced')
            return raw
        with patch.object(video.os, 'read', changed), self.assertRaises(video.EvidenceError):
            video.Inputs(self.root).read(reference)

    def test_json_duplicate_nonfinite_and_phase_overlap(self):
        for raw in (b'{"schema":1,"schema":1}', b'{"value":NaN}', b'{"value":1e999}'):
            with self.assertRaises(video.EvidenceError):
                video.load_json(raw)
        phase = self.manifest['captures'][0]['phases'][0]
        self.manifest['captures'][0]['phases'].append(dict(phase, label='overlap'))
        with self.assertRaises(video.EvidenceError):
            self.audit()

    def test_sequence_cannot_reset_between_phases_of_one_publisher(self):
        next_rows = [sample(12_000_000_000, 1), sample(17_000_000_000, 2)]
        self.manifest['captures'][0]['phases'].append({
            'label': 'later', 'samples': self.write('later.json', next_rows)})
        with self.assertRaises(video.EvidenceError):
            self.audit()

    def test_report_and_total_byte_budgets_are_production_limits(self):
        with self.assertRaises(video.EvidenceError):
            video.parse_reports(b'x' * (video.MAX_LINE_BYTES + 1))
        lines = [report(index + 1, 0, 0, index, first=True)
                 for index in range(video.MAX_RECORDS + 1)]
        with self.assertRaises(video.EvidenceError):
            video.parse_reports('\n'.join(lines).encode())
        reader = video.Inputs(self.root)
        reader.bytes = video.MAX_TOTAL_BYTES
        with self.assertRaises(video.EvidenceError):
            reader.read(self.manifest['captures'][0]['reports'])

    def test_cli_codes_and_sanitized_failure_output(self):
        path = self.root / 'manifest.json'
        command = [sys.executable, '-B', str(Path(video.__file__)), str(path)]
        path.write_text(json.dumps(self.manifest))
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(json.loads(result.stdout)['status'], 'consistent_windows')
        self.lines = self.lines[::2]
        self.refresh()
        path.write_text(json.dumps(self.manifest))
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.manifest['captures'][0]['reports']['path'] = 'PRIVATE-NAME-secret'
        path.write_text(json.dumps(self.manifest))
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn('PRIVATE-NAME', result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)['reason'], 'input_error')


NONCE = '0123456789abcdef0123456789abcdef'


def producer_fields(sequence, date, *, panel=0, previous=None, **changes):
    # Explicit producer fields and synthetic steady dates, NOT inferred by the
    # parser from extract order. Observation deliberately follows report time.
    fields = dict(reportSequence=sequence,
                  previousReportSequence=sequence - 1 if sequence > 1 else 'null',
                  previousReportMonoNs=previous if previous is not None else 'null',
                  reportOwnerStartedMonoNs=100, reportClock='host-steady',
                  publisherNoncePrivate=NONCE, publisherConnectionGeneration=7,
                  outputRole='main' if panel == 0 else 'cluster', frameCorrelation='null',
                  videoIdentityObservedMonoNs=date + 20, videoOwnerGeneration=1,
                  videoIngressGeneration=1, videoOwnerConnectionId=0, videoIngressConnectionId=0,
                  videoAvailable=1, videoClosed=0, videoOwnerCurrent=1,
                  videoGenerationChangesWindow=0, videoGenerationDroppedWindow=0)
    fields.update(changes)
    return fields


def with_fields(line, fields):
    tokens = dict(token.split('=', 1) for token in line.split()[3:])
    tokens.update(fields)
    return 'Ultra display diagnostic ' + ' '.join(f'{key}={value}' for key, value in tokens.items())


class StrictAuditTest(unittest.TestCase):
    write = AuditTest.write
    refresh = AuditTest.refresh

    def setUp(self):
        AuditTest.setUp(self)
        for index, date in enumerate((1_000_000_000, 6_000_000_000, 11_000_000_000)):
            for panel in (0, 1):
                fields = producer_fields(index + 1, date, panel=panel,
                                         previous=date - 5_000_000_000 if index else None)
                self.lines[index * 2 + panel] = with_fields(self.lines[index * 2 + panel], fields)
        for row in self.rows:
            row['cluster']['nonce'] = NONCE

    def audit(self):
        self.refresh()
        return video.audit(self.manifest, video.Inputs(self.root), strict_producer_chain=True)

    def test_exact_producer_chain_separate_snapshot_and_unknown_correlations(self):
        result = self.audit()
        self.assertEqual(result['status'], 'consistent_windows')
        self.assertTrue(all(c['complete_from_first_report'] for c in result['producer_chains']))
        window = result['phases'][0]['windows'][0]
        self.assertEqual(window['producer_window_start_mono_ns'], 1_000_000_000)
        self.assertEqual(window['producer_chain']['previous_sequence'], 1)
        self.assertEqual(window['producer_chain']['sequence'], 2)
        identities = window['video_identity_observations']
        self.assertEqual(identities['current']['videoIdentityObservedMonoNs'], 6_000_000_020)
        self.assertEqual(identities['current']['videoOwnerConnectionId'], 0)
        self.assertIsNone(identities['entire_window_transport_identity'])
        self.assertNotIn(NONCE, json.dumps(result))
        for key, value in result['attribution'].items():
            if key not in ('display_report_to_sample_publisher', 'interval_predecessor'):
                self.assertIsNone(value)

    def test_legacy_mode_ignores_producer_fields_and_preserves_unknowns(self):
        self.refresh()
        result = video.audit(self.manifest, video.Inputs(self.root))
        self.assertEqual(result['status'], 'consistent_windows')
        self.assertNotIn('producer_chains', result)
        self.assertIsNone(result['phases'][0]['windows'][0]['producer_window_start_mono_ns'])
        self.assertEqual(result['attribution']['interval_predecessor'],
                         'extract_adjacency_only_no_producer_sequence')

    def test_strict_on_legacy_missing_fields_fails_closed(self):
        self.lines = [report(1_000_000_000, 0, 0, 0, first=True)]
        with self.assertRaisesRegex(video.EvidenceError, 'missing_producer_fields'):
            self.audit()

    def test_missing_unchanged_counter_report_cannot_hide_in_arithmetic(self):
        # All zero counters/rates make legacy adjacency appear consistent even
        # when the actual producer predecessor is omitted.
        self.lines = []
        for index, date in enumerate((1_000_000_000, 6_000_000_000, 11_000_000_000)):
            for panel in (0, 1):
                line = report(date, 0, 0, 0, panel=panel,
                              videoSubmittedWindow=0, videoDecodedWindow=0,
                              videoSupersededWindow=0, fps=0, freshVideoFps=0, videoSubmittedFps=0)
                self.lines.append(with_fields(line, producer_fields(
                    index + 1, date, panel=panel, previous=date - 5_000_000_000 if index else None)))
        del self.lines[2]
        result = self.audit()
        self.assertEqual(result['status'], 'incomplete')
        self.assertEqual(result['phases'][0]['whole_report_pairs'], 0)
        self.assertEqual(result['phases'][0]['rejected_windows'][0]['reason'],
                         'missing_producer_predecessor')
        self.assertEqual(result['phases'][1]['whole_report_pairs'], 2)
        legacy = video.audit(self.manifest, video.Inputs(self.root))
        self.assertEqual(legacy['status'], 'consistent_windows')

    def test_wrong_predecessor_date_is_not_an_adjacent_window(self):
        self.lines[2] = with_fields(self.lines[2], {'previousReportMonoNs': 1_000_000_001})
        result = self.audit()
        self.assertEqual(result['status'], 'incomplete')
        self.assertEqual(result['phases'][0]['whole_report_pairs'], 1)
        self.assertEqual(result['phases'][0]['rejected_windows'][0]['reason'],
                         'producer_predecessor_date_mismatch')

    def test_missing_initial_reports_remain_incomplete_even_with_later_valid_pairs(self):
        self.lines = self.lines[2:]
        result = self.audit()
        self.assertEqual(result['status'], 'incomplete')
        self.assertEqual([p['whole_report_pairs'] for p in result['phases']], [1, 1])
        self.assertEqual(result['producer_chains'][0]['rejected_joins'][0]['reason'],
                         'missing_initial_producer_reports')

    def test_missing_report_outside_phase_is_still_incomplete(self):
        self.rows[0]['remote_sampled_ns'] = 6_000_000_000
        self.lines = self.lines[2:]
        self.assertEqual(self.audit()['status'], 'incomplete')

    def test_first_nulls_sequence_replay_reset_and_panel_role_rejection(self):
        originals = list(self.lines)
        for index, fields in ((0, {'previousReportSequence': 0}),
                              (0, {'previousReportMonoNs': 0}),
                              (2, {'reportSequence': 1, 'previousReportSequence': 'null',
                                   'previousReportMonoNs': 'null'}),
                              (2, {'reportSequence': 2, 'previousReportSequence': 0}),
                              (2, {'reportSequence': 121, 'previousReportSequence': 120}),
                              (2, {'previousReportMonoNs': 6_000_000_000}),
                              (2, {'outputRole': 'cluster'})):
            with self.subTest(fields=fields):
                self.lines = list(originals)
                self.lines[index] = with_fields(self.lines[index], fields)
                with self.assertRaises(video.EvidenceError):
                    self.audit()

    def test_owner_publisher_scope_and_clock_require_explicit_split(self):
        originals = list(self.lines)
        for fields in ({'reportOwnerStartedMonoNs': 101},
                       {'publisherNoncePrivate': 'f' * 32},
                       {'publisherConnectionGeneration': 8}, {'reportClock': 'phone'},
                       {'publisherNoncePrivate': 'null', 'publisherConnectionGeneration': 'null'},
                       {'reportOwnerStartedMonoNs': 6_000_000_001}):
            with self.subTest(fields=fields):
                self.lines = list(originals)
                self.lines[2] = with_fields(self.lines[2], fields)
                with self.assertRaises(video.EvidenceError):
                    self.audit()
        self.lines = originals
        for key in ('clock_domain', 'owner_scope'):
            previous = self.manifest['captures'][0][key]
            self.manifest['captures'][0][key] = None
            with self.assertRaisesRegex(video.EvidenceError, 'strict_requires_trusted_scope'):
                self.audit()
            self.manifest['captures'][0][key] = previous

    def test_exact_sample_publisher_including_valid_zero_and_uint64_generation(self):
        for generation in (0, video.UINT64_MAX):
            for row in self.rows:
                row['cluster']['connectionGeneration'] = str(generation)
            self.lines = [with_fields(line, {'publisherConnectionGeneration': generation})
                          for line in self.lines]
            self.assertEqual(self.audit()['status'], 'consistent_windows')
        self.rows[0]['cluster']['nonce'] = 'f' * 32
        self.rows[1]['cluster']['nonce'] = 'f' * 32
        with self.assertRaisesRegex(video.EvidenceError, 'producer_sample_publisher_mismatch'):
            self.audit()

    def test_identity_replacement_and_retirement_never_attribute_window_frames(self):
        self.lines[2] = with_fields(self.lines[2], dict(
            videoOwnerGeneration=1, videoIngressGeneration=2,
            videoOwnerConnectionId=0, videoIngressConnectionId=42,
            videoOwnerCurrent=0, videoGenerationChangesWindow=1, videoGenerationDroppedWindow=3))
        self.lines[4] = with_fields(self.lines[4], dict(
            videoOwnerGeneration=2, videoIngressGeneration=3, videoOwnerConnectionId=42,
            videoIngressConnectionId=42, videoAvailable=0, videoClosed=1, videoOwnerCurrent=0))
        result = self.audit()
        self.assertEqual(result['status'], 'consistent_windows')
        windows = result['phases'][0]['windows']
        self.assertEqual(windows[0]['video_identity_observations']['generation_window_counts']
                         ['videoGenerationDroppedWindow'], 3)
        for window in windows:
            self.assertIsNone(window['video_identity_observations']['entire_window_transport_identity'])
        self.assertEqual(windows[1]['video_identity_observations']['current']['videoClosed'], 1)

    def test_missing_native_identity_is_null_not_zero_or_a_frame(self):
        nulls = {key: 'null' for key in video.VIDEO_IDENTITY + video.VIDEO_WINDOWS}
        self.lines = [with_fields(line, nulls) for line in self.lines]
        result = self.audit()
        identity = result['phases'][0]['windows'][0]['video_identity_observations']
        self.assertTrue(all(value is None for value in identity['current'].values()))

    def test_real_no_input_report_shape_keeps_video_unavailable(self):
        nulls = {key: 'null' for key in video.VIDEO_IDENTITY}
        lines = []
        omitted = set(video.VIDEO_WINDOWS + video.COUNTS + ('decoded', 'videoPresented',
                                                          'freshVideoFps', 'videoSubmittedFps'))
        for line in self.lines:
            line = with_fields(line, nulls)
            tokens = [token for token in line.split()[3:] if token.split('=', 1)[0] not in omitted]
            lines.append('Ultra display diagnostic ' + ' '.join(tokens))
        self.lines = lines
        result = self.audit()
        self.assertEqual(result['status'], 'incomplete')
        self.assertTrue(all(c['complete_from_first_report'] for c in result['producer_chains']))
        self.assertTrue(all(p['presented_frames'] is None for p in result['phases']))
        self.assertEqual(result['phases'][0]['rejected_windows'][0]['reason'], 'missing_video_counters')

    def test_partial_or_impossible_video_snapshot_rejected(self):
        original = self.lines[2]
        for fields in ({'videoAvailable': 2}, {'videoClosed': 1},
                       {'videoOwnerCurrent': 0}, {'videoIngressGeneration': 2},
                       {'videoIdentityObservedMonoNs': 99}, {'videoOwnerGeneration': 'null'},
                       {'videoIdentityObservedMonoNs': 'null'}, {'frameCorrelation': 'guessed'}):
            with self.subTest(fields=fields):
                self.lines[2] = with_fields(original, fields)
                with self.assertRaises(video.EvidenceError):
                    self.audit()

    def test_retirement_new_capture_and_clock_domain_have_no_join(self):
        self.refresh()
        retired = copy.deepcopy(self.manifest['captures'][0])
        retired['label'] = 'retired'
        retired['reports'] = self.write('retired.txt', '\n'.join(self.lines) + '\n')
        retired['phases'][0]['samples'] = self.write('retired.json', self.rows)
        successor = self.manifest['captures'][0]
        successor['clock_domain'] = 'separate-synthetic-host'
        successor['owner_scope'] = 'successor-invocation'
        self.lines = [with_fields(line, {'publisherNoncePrivate': 'f' * 32,
                                        'publisherConnectionGeneration': 8,
                                        'reportOwnerStartedMonoNs': 200}) for line in self.lines]
        for row in self.rows:
            row['cluster']['nonce'] = 'f' * 32
            row['cluster']['connectionGeneration'] = 8
        self.refresh()
        self.manifest['captures'].insert(0, retired)
        # Reuse exact dates for an explicitly separate host/owner and genuine
        # successor tuple; never extend the old predecessor into this capture.
        result = video.audit(self.manifest, video.Inputs(self.root), strict_producer_chain=True)
        self.assertEqual(len(result['producer_chains']), 4)
        self.assertTrue(all(p['whole_report_pairs'] == 2 for p in result['phases']))

    def test_source_clock_failure_keeps_original_host_and_snapshot_dates(self):
        self.lines[2] = with_fields(self.lines[2], {'clockUnavailable': 1, 'clockEpoch': 1})
        self.lines[4] = with_fields(self.lines[4], {'clockUnavailable': 1, 'clockEpoch': 1})
        row = self.audit()['phases'][0]['windows'][0]
        self.assertEqual(row['owner_clock_surface_deltas']['clockUnavailable'], 1)
        self.assertEqual(row['producer_window_start_mono_ns'], 1_000_000_000)
        self.assertEqual(row['video_identity_observations']['current']
                         ['videoIdentityObservedMonoNs'], 6_000_000_020)

    def test_exact_uint64_dates_and_producer_parser_failure_boundaries(self):
        date = video.UINT64_MAX - 20
        line = report(date, 0, 0, 0, first=True)
        line = with_fields(line, producer_fields(1, date, reportOwnerStartedMonoNs=date - 1))
        row = video.parse_reports(line.encode(), strict_producer_chain=True)[0]
        self.assertEqual(row['mono_ns'], date)
        self.assertEqual(row['videoIdentityObservedMonoNs'], video.UINT64_MAX)
        for extra in (' reportSequence=1', ' videoAvailable=1', ' reportClock=host-steady'):
            with self.subTest(extra=extra), self.assertRaises(video.EvidenceError):
                video.parse_reports((line + extra).encode(), strict_producer_chain=True)
        for fields in ({'previousReportMonoNs': 'NaN'}, {'publisherConnectionGeneration': -1},
                       {'videoIngressConnectionId': video.UINT64_MAX + 1},
                       {'reportSequence': '1.0'}, {'videoAvailable': 'true'}):
            with self.subTest(fields=fields), self.assertRaises(video.EvidenceError):
                video.parse_reports(with_fields(line, fields).encode(), strict_producer_chain=True)

    def test_missing_panel_has_no_chain_or_fabricated_counts(self):
        self.lines = self.lines[::2]
        result = self.audit()
        self.assertEqual(result['status'], 'incomplete')
        self.assertFalse(result['producer_chains'][1]['complete_from_first_report'])
        self.assertEqual(result['producer_chains'][1]['reports'], 0)
        self.assertIsNone(result['producer_chains'][1]['owner_started_mono_ns'])
        self.assertIsNone(result['phases'][1]['presented_frames'])
        self.assertIsNone(result['phases'][1]['weighted_fresh_fps'])

    def test_delayed_producer_reports_use_exact_dates_not_nominal_five_seconds(self):
        self.lines = []
        dates = (1_000_000_000, 8_000_000_000, 14_000_000_000)
        self.rows[-1]['remote_sampled_ns'] = dates[-1]
        for index, date in enumerate(dates):
            for panel in (0, 1):
                seconds = (date - dates[index - 1]) / 1e9 if index else 1
                line = report(date, index * 8, index * 10, index * 300,
                              first=index == 0, panel=panel,
                              fps=300 / seconds, freshVideoFps=8 / seconds,
                              videoSubmittedFps=10 / seconds)
                self.lines.append(with_fields(line, producer_fields(
                    index + 1, date, panel=panel, previous=dates[index - 1] if index else None)))
        result = self.audit()
        self.assertEqual(result['status'], 'consistent_windows')
        self.assertEqual(result['phases'][0]['audited_report_span_ns'], 13_000_000_000)
        self.assertAlmostEqual(result['phases'][0]['weighted_fresh_fps'], 16 / 13)
        self.assertEqual(result['phases'][0]['windows'][1]['producer_window_start_mono_ns'],
                         dates[1])

    def test_strict_cli_all_codes_sanitizes_nonce_and_original_line_ranges(self):
        self.lines.insert(0, 'unrelated original line')
        self.lines.append(self.lines[1])
        self.manifest['captures'][0]['report_line_range'] = [2, 7]
        self.refresh()
        path = self.root / 'manifest.json'
        command = [sys.executable, '-B', str(Path(video.__file__)), '--strict-producer-chain', str(path)]
        def run():
            path.write_text(json.dumps(self.manifest))
            return subprocess.run(command, capture_output=True, text=True, timeout=5)
        result = run()
        self.assertEqual(result.returncode, 0, result.stdout)
        output = json.loads(result.stdout)
        self.assertEqual(output['phases'][0]['windows'][0]['start_line'], 2)
        self.assertEqual(output['phases'][0]['reports_sha256'],
                         self.manifest['captures'][0]['reports']['sha256'])
        self.assertNotIn(NONCE, result.stdout + result.stderr)
        self.manifest['captures'][0]['report_line_range'] = [4, 7]
        self.assertEqual(run().returncode, 2)
        self.manifest['captures'][0]['report_line_range'] = None
        self.assertEqual(run().returncode, 1)


if __name__ == '__main__':
    unittest.main()
