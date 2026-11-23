#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Bounded offline audit of original Ultra display reports; no live acquisition.

The private manifest splits extracts at owner/session/host-clock boundaries.
Legacy/default mode checks publisher observations in samples without binding
them to display lines: that manifest binding remains an operator assertion.
Optional strict mode checks producer-reported predecessor/publisher fields in
the declared trusted host/owner scope; it is not cryptographic attestation.
No sender, source-frame, ACK or cross-clock join is manufactured here.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys

MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024
MAX_RECORDS = 8192
MAX_LINE_BYTES = 32768
UINT64_MAX = (1 << 64) - 1
COUNTS = (
    'videoSubmittedWindow', 'videoDecodedWindow', 'videoSupersededWindow',
    'videoDiscardedWindow', 'videoLateWindow', 'videoClockDroppedWindow',
    'videoNonMonotonicWindow', 'videoBackpressureWindow',
)
STATE_COUNTS = (
    'suspensions', 'clockEpoch', 'clockUnavailable', 'ownerGap',
    'feedbackDelay', 'surfaceWithdrawals', 'surfaceRestorations',
)
CORE = ('panel', 'mono_ns', 'scenes', 'decoded', 'videoPresented')
INTEGER_FIELDS = set(CORE + COUNTS + STATE_COUNTS + ('late', 'clockDropped', 'maxStaged'))
RATE_FIELDS = ('fps', 'freshVideoFps', 'videoSubmittedFps')
MEANS = (
    'composeMeanMs', 'completionWaitMeanMs', 'submitToPresentMeanMs',
    'presentToPollMeanMs', 'videoLeadMeanMs',
)
FIELDS = INTEGER_FIELDS | set(RATE_FIELDS + MEANS)
PRODUCER_INTEGERS = {
    'reportSequence', 'previousReportSequence', 'previousReportMonoNs',
    'reportOwnerStartedMonoNs', 'publisherConnectionGeneration',
}
VIDEO_IDENTITY = (
    'videoIdentityObservedMonoNs', 'videoOwnerGeneration', 'videoIngressGeneration',
    'videoOwnerConnectionId', 'videoIngressConnectionId', 'videoAvailable',
    'videoClosed', 'videoOwnerCurrent',
)
VIDEO_WINDOWS = ('videoGenerationChangesWindow', 'videoGenerationDroppedWindow')
PRODUCER_TEXT = {'reportClock', 'publisherNoncePrivate', 'outputRole', 'frameCorrelation'}
PRODUCER_FIELDS = PRODUCER_INTEGERS | PRODUCER_TEXT | set(VIDEO_IDENTITY + VIDEO_WINDOWS)
LABEL = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z')
HEX = re.compile(r'[0-9a-f]{64}\Z')


class EvidenceError(ValueError):
    """Messages are fixed categories, never raw evidence, paths or identities."""


def require(condition, category):
    if not condition:
        raise EvidenceError(category)


def integer(value):
    require(type(value) is int and 0 <= value <= UINT64_MAX, 'invalid_integer')
    return value


def published_integer(value):
    # Existing manager/catalogue JSON encodes uint64 IDs/sequences as decimal
    # strings. Keep their exact integer value; never pass them through float.
    if isinstance(value, str):
        require(re.fullmatch(r'[0-9]{1,20}', value), 'invalid_published_integer')
        return integer(int(value))
    return integer(value)


def object_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate_json_key')
        result[key] = value
    return result


def load_json(raw):
    def constant(_):
        raise EvidenceError('nonfinite_json')
    def number(value):
        parsed = float(value)
        require(math.isfinite(parsed), 'nonfinite_json')
        return parsed
    try:
        return json.loads(raw, object_pairs_hook=object_pairs, parse_constant=constant,
                          parse_float=number)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise EvidenceError('invalid_json') from error


class Inputs:
    def __init__(self, directory):
        self.directory = directory
        self.bytes = 0

    def read(self, reference, limit=MAX_FILE_BYTES):
        require(isinstance(reference, dict) and set(reference) == {'path', 'sha256'},
                'invalid_file_reference')
        require(isinstance(reference['path'], str) and 0 < len(reference['path']) <= 4096,
                'invalid_path')
        require(isinstance(reference['sha256'], str) and HEX.fullmatch(reference['sha256']),
                'invalid_hash')
        path = self.directory / reference['path']
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        try:
            before = os.fstat(fd)
            require(stat.S_ISREG(before.st_mode) and 0 < before.st_size <= limit,
                    'file_bound_or_type')
            require(self.bytes + before.st_size <= MAX_TOTAL_BYTES, 'total_byte_bound')
            chunks = []
            remaining = before.st_size + 1
            while remaining:
                chunk = os.read(fd, min(remaining, 65536))
                if not chunk:
                    break
                chunks.append(chunk)
                remaining -= len(chunk)
            raw = b''.join(chunks)
            after = os.fstat(fd)
            named = path.lstat()
            signature = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
            # Metadata is used only to detect mutation, never to date a frame.
            require(signature(before) == signature(after) == signature(named) and
                    len(raw) == before.st_size, 'file_changed')
            require(hashlib.sha256(raw).hexdigest() == reference['sha256'], 'hash_mismatch')
            self.bytes += len(raw)
            return raw
        finally:
            os.close(fd)


def parse_reports(raw, line_range=None, *, strict_producer_chain=False):
    require(line_range is None or isinstance(line_range, list) and len(line_range) == 2 and
            all(type(value) is int for value in line_range) and
            1 <= line_range[0] <= line_range[1] <= MAX_FILE_BYTES, 'invalid_line_range')
    reports = []
    fields = FIELDS | PRODUCER_FIELDS if strict_producer_chain else FIELDS
    previous = {}
    for number, line in enumerate(raw.splitlines(), 1):
        require(len(line) <= MAX_LINE_BYTES, 'line_byte_bound')
        if line_range is not None and not line_range[0] <= number <= line_range[1]:
            continue
        marker = b'Ultra display diagnostic '
        if marker not in line:
            continue
        require(line.count(marker) == 1, 'ambiguous_report_line')
        row = {'original_line': number}
        try:
            tokens = line.split(marker, 1)[1].decode('ascii').split()
        except UnicodeError as error:
            raise EvidenceError('nonascii_report') from error
        for token in tokens:
            key, separator, value = token.partition('=')
            if not separator or key not in fields:
                continue
            require(key not in row, 'duplicate_report_field')
            if key in PRODUCER_FIELDS and key not in PRODUCER_TEXT:
                require(value == 'null' or re.fullmatch(r'[0-9]{1,20}', value),
                        'invalid_producer_integer')
                row[key] = None if value == 'null' else integer(int(value))
            elif key in PRODUCER_TEXT:
                row[key] = None if value == 'null' else value
            elif key in INTEGER_FIELDS:
                require(re.fullmatch(r'[0-9]{1,20}', value), 'invalid_report_integer')
                row[key] = integer(int(value))
            else:
                require(re.fullmatch(r'-?[0-9]+(?:\.[0-9]+)?(?:e[+-]?[0-9]+)?', value)
                        and len(value) <= 32, 'invalid_report_number')
                row[key] = float(value)
                require(math.isfinite(row[key]) and
                        (key == 'videoLeadMeanMs' or row[key] >= 0), 'invalid_report_number')
        core = CORE
        # The producer can report a scene with no NativeInput; it emits null
        # identity and omits video counters. Keep that chain, not zero frames.
        if strict_producer_chain and row.get('videoIdentityObservedMonoNs') is None:
            core = ('panel', 'mono_ns', 'scenes')
        require(all(key in row for key in core) and row['panel'] in (0, 1),
                'missing_report_identity')
        if strict_producer_chain:
            validate_producer_report(row)
        prior = previous.get(row['panel'])
        if prior:
            require(row['mono_ns'] > prior['mono_ns'], 'report_reordered_or_duplicate')
            for key in ('scenes', 'decoded', 'videoPresented', 'late', 'clockDropped') + STATE_COUNTS:
                if key in row and key in prior:
                    require(row[key] >= prior[key], 'counter_reset_split_capture')
        previous[row['panel']] = row
        reports.append(row)
        require(len(reports) <= MAX_RECORDS, 'report_count_bound')
    require(reports, 'no_display_reports')
    return reports


def validate_producer_report(row):
    required = (PRODUCER_INTEGERS | PRODUCER_TEXT | set(VIDEO_IDENTITY))
    require(required <= row.keys(), 'missing_producer_fields')
    require(row['reportClock'] == 'host-steady', 'unsupported_producer_clock')
    require(row['outputRole'] == ('main' if row['panel'] == 0 else 'cluster'),
            'producer_panel_role_mismatch')
    require(row['frameCorrelation'] is None, 'unsupported_frame_correlation')
    nonce = row['publisherNoncePrivate']
    require(isinstance(nonce, str) and re.fullmatch(r'[0-9a-f]{32}', nonce) and
            row['publisherConnectionGeneration'] is not None, 'missing_producer_publisher')
    sequence = row['reportSequence']
    started = row['reportOwnerStartedMonoNs']
    require(sequence is not None and 1 <= sequence <= 120 and started is not None and
            started <= row['mono_ns'], 'invalid_producer_identity')
    prior_sequence, prior_date = row['previousReportSequence'], row['previousReportMonoNs']
    if sequence == 1:
        require(prior_sequence is None and prior_date is None, 'invalid_first_predecessor')
    else:
        require(prior_sequence == sequence - 1 and prior_date is not None and
                started <= prior_date < row['mono_ns'], 'invalid_producer_predecessor')
    observation = row['videoIdentityObservedMonoNs']
    if observation is None:
        require(all(row[key] is None for key in VIDEO_IDENTITY + VIDEO_WINDOWS
                    if key in row), 'partial_video_identity')
        return
    require(all(row[key] is not None for key in
                ('videoOwnerGeneration', 'videoIngressGeneration', 'videoAvailable',
                 'videoClosed', 'videoOwnerCurrent')) and
            all(key in row and row[key] is not None for key in VIDEO_WINDOWS),
            'partial_video_identity')
    require(observation >= started, 'video_identity_before_owner')
    for key in ('videoAvailable', 'videoClosed', 'videoOwnerCurrent'):
        require(row[key] in (0, 1), 'invalid_video_identity_flag')
    require(not (row['videoAvailable'] and row['videoClosed']), 'invalid_video_availability')
    current = bool(row['videoAvailable'] and not row['videoClosed'] and
                   row['videoOwnerGeneration'] == row['videoIngressGeneration'])
    require(bool(row['videoOwnerCurrent']) == current, 'invalid_video_owner_current')


def producer_chains(reports, capture):
    # Dates identify a local owner only within the explicitly declared trusted
    # host/invocation. Neither labels nor launch nonces attest to the producer.
    require(capture['clock_domain'] is not None and capture['owner_scope'] is not None,
            'strict_requires_trusted_scope')
    identities = {(row['reportOwnerStartedMonoNs'], row['publisherNoncePrivate'],
                   row['publisherConnectionGeneration']) for row in reports}
    require(len(identities) == 1, 'producer_owner_or_publisher_changed_split_capture')
    summaries = []
    for panel in (0, 1):
        lane = [row for row in reports if row['panel'] == panel]
        rejected = []
        if lane and lane[0]['reportSequence'] != 1:
            rejected.append({'end_line': lane[0]['original_line'],
                             'reason': 'missing_initial_producer_reports'})
        for before, current in zip(lane, lane[1:]):
            require(current['reportSequence'] > before['reportSequence'],
                    'producer_sequence_reordered_or_reset')
            reason = producer_join(before, current)
            if reason:
                rejected.append({'end_line': current['original_line'], 'reason': reason})
        summaries.append({
            'capture': capture['label'], 'panel': panel, 'reports': len(lane),
            'owner_started_mono_ns': lane[0]['reportOwnerStartedMonoNs'] if lane else None,
            'first_sequence': lane[0]['reportSequence'] if lane else None,
            'last_sequence': lane[-1]['reportSequence'] if lane else None,
            'complete_from_first_report': bool(lane) and not rejected,
            'rejected_joins': rejected,
        })
    return summaries


def producer_join(before, current):
    if current['previousReportSequence'] != before['reportSequence']:
        return 'missing_producer_predecessor'
    if current['previousReportMonoNs'] != before['mono_ns']:
        return 'producer_predecessor_date_mismatch'
    return None


def add_producer_window(row, before, current):
    row['producer_window_start_mono_ns'] = current['previousReportMonoNs']
    row['producer_chain'] = {
        'sequence': current['reportSequence'],
        'previous_sequence': current['previousReportSequence'],
        'owner_started_mono_ns': current['reportOwnerStartedMonoNs'],
        'clock': current['reportClock'], 'configured_output_role': current['outputRole'],
        'sample_publisher_matches_in_declared_scope': True,
    }
    # These snapshots are separately dated, possibly AFTER report/counter time.
    # Even equal endpoint IDs and zero adoption counters do not enumerate every
    # ingress replacement, so no entire-window generation attribution is made.
    row['video_identity_observations'] = {
        'before': {key: before[key] for key in VIDEO_IDENTITY},
        'current': {key: current[key] for key in VIDEO_IDENTITY},
        'generation_window_counts': {key: current.get(key) for key in VIDEO_WINDOWS},
        'entire_window_transport_identity': None,
    }


def phase_samples(raw):
    rows = load_json(raw)
    require(isinstance(rows, list) and 2 <= len(rows) <= MAX_RECORDS, 'sample_count_bound')
    previous_date = None
    previous_sequence = None
    first_sequence = None
    publisher = None
    for row in rows:
        require(isinstance(row, dict), 'invalid_sample')
        date = integer(row.get('remote_sampled_ns'))
        require(previous_date is None or date > previous_date, 'sample_reordered_or_duplicate')
        previous_date = date
        carplay, cluster = row.get('carplay'), row.get('cluster')
        require(isinstance(carplay, dict) and isinstance(cluster, dict) and
                carplay.get('returnReady') is True and cluster.get('ready') is True,
                'sample_not_both_ready')
        nonce = cluster.get('nonce')
        require(isinstance(nonce, str) and 0 < len(nonce) <= 256, 'missing_sample_publisher')
        generation = published_integer(cluster.get('connectionGeneration'))
        current = (nonce, generation)
        require(publisher is None or publisher == current, 'publisher_changed_split_capture')
        publisher = current
        sequence = published_integer(cluster.get('sequence'))
        require(previous_sequence is None or sequence >= previous_sequence,
                'sample_sequence_reordered')
        previous_sequence = sequence
        if first_sequence is None:
            first_sequence = sequence
    return (rows[0]['remote_sampled_ns'], rows[-1]['remote_sampled_ns'], publisher, len(rows),
            first_sequence, previous_sequence)


def window(before, current):
    if any(key not in row for row in (before, current) for key in ('decoded', 'videoPresented')):
        return None, 'missing_video_counters'
    if any(key not in current for key in COUNTS + RATE_FIELDS):
        return None, 'missing_interval_fields'
    duration = current['mono_ns'] - before['mono_ns']
    presented = current['videoPresented'] - before['videoPresented']
    scenes = current['scenes'] - before['scenes']
    seconds = duration / 1e9
    # Window counts refer to the last emitted report, not to any adjacent row
    # in an incomplete extract. A missed predecessor must not borrow its date.
    for cumulative, interval in (('decoded', 'videoDecodedWindow'),
                                 ('late', 'videoLateWindow'),
                                 ('clockDropped', 'videoClockDroppedWindow')):
        if cumulative in before and cumulative in current:
            if current[cumulative] - before[cumulative] != current[interval]:
                return None, 'interval_counter_mismatch'
    for count, rate in ((presented, 'freshVideoFps'), (scenes, 'fps'),
                        (current['videoSubmittedWindow'], 'videoSubmittedFps')):
        # Production currently prints six significant digits; 0.001 fps covers
        # rounding for the measured <=60 fps reports, not arbitrary high rates.
        if abs(count / seconds - current[rate]) > 0.001:
            return None, 'interval_rate_mismatch'
    return {
        'previous_report_mono_ns': before['mono_ns'], 'report_mono_ns': current['mono_ns'],
        'producer_window_start_mono_ns': None,
        'adjacent_report_span_ns': duration, 'start_line': before['original_line'],
        'end_line': current['original_line'], 'presented_frames': presented,
        'scene_redraws': scenes, 'counts': {key: current[key] for key in COUNTS},
        'reported_rates': {key: current[key] for key in RATE_FIELDS},
        'reported_means_ms': {key: current[key] for key in MEANS if key in current},
        'owner_clock_surface_deltas': {
            key: current[key] - before[key] if key in current and key in before else None
            for key in STATE_COUNTS},
        'decoded_minus_presented_superseded':
            current['videoDecodedWindow'] - presented - current['videoSupersededWindow'],
    }, None


def audit(manifest, inputs, *, strict_producer_chain=False):
    require(isinstance(manifest, dict) and set(manifest) == {'schema', 'captures'} and
            type(manifest['schema']) is int and manifest['schema'] == 1,
            'invalid_manifest')
    captures = manifest['captures']
    require(isinstance(captures, list) and 1 <= len(captures) <= 16, 'capture_count_bound')
    output = []
    labels = set()
    incomplete = False
    chains = []
    for capture in captures:
        require(isinstance(capture, dict) and set(capture) ==
                {'label', 'source_revision', 'clock_domain', 'owner_scope', 'reports',
                 'report_line_range', 'phases'},
                'invalid_capture')
        label = capture['label']
        require(isinstance(label, str) and LABEL.fullmatch(label) and label not in labels,
                'invalid_capture_label')
        labels.add(label)
        revision = capture['source_revision']
        require(isinstance(revision, str) and re.fullmatch(r'[0-9a-f]{40}', revision),
                'invalid_source_revision')
        for key in ('clock_domain', 'owner_scope'):
            value = capture[key]
            require(value is None or isinstance(value, str) and LABEL.fullmatch(value),
                    'invalid_scope')
        reports = parse_reports(inputs.read(capture['reports']), capture['report_line_range'],
                                strict_producer_chain=strict_producer_chain)
        if strict_producer_chain:
            summaries = producer_chains(reports, capture)
            chains.extend(summaries)
            incomplete |= any(not row['complete_from_first_report'] for row in summaries)
        phases = capture['phases']
        require(isinstance(phases, list) and 1 <= len(phases) <= 16, 'phase_count_bound')
        prior_end = None
        publisher = None
        prior_sequence = None
        phase_labels = set()
        for phase in phases:
            require(isinstance(phase, dict) and set(phase) == {'label', 'samples'}, 'invalid_phase')
            name = phase['label']
            require(isinstance(name, str) and LABEL.fullmatch(name) and name not in phase_labels,
                    'invalid_phase_label')
            phase_labels.add(name)
            start, end, current_publisher, sample_count, first_sequence, last_sequence = phase_samples(
                inputs.read(phase['samples'], 2 * 1024 * 1024))
            require(prior_end is None or start > prior_end, 'phase_overlap_or_reordering')
            require(publisher is None or publisher == current_publisher,
                    'publisher_changed_split_capture')
            require(prior_sequence is None or first_sequence >= prior_sequence,
                    'sample_sequence_reordered')
            if strict_producer_chain:
                require(all((row['publisherNoncePrivate'], row['publisherConnectionGeneration']) ==
                            current_publisher for row in reports), 'producer_sample_publisher_mismatch')
            prior_end, publisher = end, current_publisher
            prior_sequence = last_sequence
            for panel in (0, 1):
                lane = [row for row in reports if row['panel'] == panel]
                windows, rejected = [], []
                for before, current in zip(lane, lane[1:]):
                    if before['mono_ns'] < start or current['mono_ns'] > end:
                        continue
                    reason = producer_join(before, current) if strict_producer_chain else None
                    row = None
                    if reason is None:
                        row, reason = window(before, current)
                    if reason:
                        rejected.append({'end_line': current['original_line'], 'reason': reason})
                    else:
                        if strict_producer_chain:
                            add_producer_window(row, before, current)
                        windows.append(row)
                incomplete |= bool(rejected) or not windows
                span = sum(row['adjacent_report_span_ns'] for row in windows)
                frames = sum(row['presented_frames'] for row in windows)
                output.append({
                    'capture': label, 'phase': name, 'panel': panel,
                    'source_revision_declared': revision,
                    'clock_domain_declared': capture['clock_domain'],
                    'owner_scope_declared': capture['owner_scope'],
                    'sample_publisher_unchanged': True, 'both_ready_samples': sample_count,
                    'sample_start_mono_ns': start, 'sample_end_mono_ns': end,
                    'reports_sha256': capture['reports']['sha256'],
                    'report_line_range_declared': capture['report_line_range'],
                    'samples_sha256': phase['samples']['sha256'],
                    'whole_report_pairs': len(windows), 'audited_report_span_ns': span,
                    'presented_frames': frames if windows else None,
                    'weighted_fresh_fps': frames * 1e9 / span if span else None,
                    'reported_fresh_fps_mean': sum(row['reported_rates']['freshVideoFps']
                                                  for row in windows) / len(windows)
                    if windows else None,
                    'counts': {key: sum(row['counts'][key] for row in windows) if windows else None
                               for key in COUNTS},
                    'windows': windows, 'rejected_windows': rejected,
                })
    result = {
        'schema': 1, 'status': 'incomplete' if incomplete else 'consistent_windows',
        'input_bytes_read': inputs.bytes,
        'attribution': {
            'display_report_to_sample_publisher': 'operator_scope_only_not_embedded_in_report',
            'interval_predecessor': 'extract_adjacency_only_no_producer_sequence',
            'source_frame_to_admission_to_receipt': None,
            'authenticated_transport_generation': None,
            'sender_to_panel': None, 'phone_to_host_clock': None,
            'phone_local_encoder_ack_date': None, 'timeout_cause': None,
        },
        'limits': ['sample polls are not video frames',
                   'fresh counts are accepted source tokens, not distinct picture content',
                   'missing reports with unchanged counters can be undetectable',
                   'no per-frame balance, p95, optical, deployment or causal qualification'],
        'phases': output,
    }

    if strict_producer_chain:
        result['strict_producer_chain'] = True
        result['producer_chains'] = chains
        result['attribution'].update({
            'display_report_to_sample_publisher':
                'producer_reported_tuple_matches_in_declared_trusted_scope_not_attestation',
            'interval_predecessor': 'exact_producer_sequence_and_date',
        })
        result['limits'][2] = 'missing producer reports leave incomplete chains; no gap filling'
        result['limits'].append('video identity snapshots do not attribute whole windows or frames')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path, help='private schema-1 manifest; output JSON to stdout')
    parser.add_argument('--strict-producer-chain', action='store_true',
                        help='require producer identities in declared trusted host/owner scope')
    args = parser.parse_args(argv)
    try:
        path = args.manifest
        # The manifest itself is bounded and never contributes a frame date.
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        try:
            info = os.fstat(fd)
            require(stat.S_ISREG(info.st_mode) and 0 < info.st_size <= 128 * 1024,
                    'manifest_byte_bound_or_type')
            with os.fdopen(fd, 'rb', closefd=False) as stream:
                raw = stream.read(128 * 1024 + 1)
        finally:
            os.close(fd)
        require(0 < len(raw) <= 128 * 1024, 'manifest_byte_bound')
        result = audit(load_json(raw), Inputs(path.parent),
                       strict_producer_chain=args.strict_producer_chain)
        result['manifest_sha256'] = hashlib.sha256(raw).hexdigest()
        print(json.dumps(result, allow_nan=False, indent=2))
        return 2 if result['status'] == 'incomplete' else 0
    except (OSError, ValueError, TypeError, OverflowError, RecursionError) as error:
        category = str(error) if isinstance(error, EvidenceError) else 'input_error'
        print(json.dumps({'schema': 1, 'status': 'invalid', 'reason': category}))
        return 1


if __name__ == '__main__':
    sys.exit(main())
