#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Offline review of recovery content, separate from logical Ready.

No remote operations, image recognition or inferred capture dates. Existing
presentation publications contain no content identity or fresh-video receipt.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys

MAX_LINE = 1_500_000
MAX_ARCHIVE = 256 * 1024 * 1024
MAX_REVIEW = 1024 * 1024
MAX_CAPTURE = 32 * 1024 * 1024
CLASSIFICATIONS = ('content', 'startup', 'logos', 'blank', 'nonpresenting', 'stale_video', 'unknown')


@contextmanager
def bounded_input(path, maximum):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or not 0 <= before.st_size <= maximum:
            raise ValueError('Input must be bounded regular data')
        yield stream
        signature = lambda item: (item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns, item.st_ctime_ns)
        if (signature(before) != signature(os.fstat(fd)) or
                signature(before) != signature(path.lstat())):
            raise ValueError('Input changed while reading')


def file_hash(path, maximum):
    digest = hashlib.sha256()
    with bounded_input(path, maximum) as stream:
        total = 0
        while True:
            raw = stream.read(65536)
            if not raw:
                break
            total += len(raw)
            if total > maximum:
                raise ValueError('Input exceeds bound')
            digest.update(raw)
    return digest.hexdigest()


def content_evidence(logical_ready):
    """An explicit absence of evidence, never a new renderer readiness policy."""
    return {'candidate': None, 'fresh_scene_qualified': None,
            'status': 'unestablished' if logical_ready else 'not_logically_ready',
            'reason': 'No original content/phase/fresh-video presentation observation in this receipt.',
            'visual_review': 'unreviewed', 'optical_first_frame_s': None}


def read_records(path):
    total = 0
    with bounded_input(path, MAX_ARCHIVE) as stream:
        for number in range(1, 1_000_001):
            raw = stream.readline(MAX_LINE + 1)
            if not raw:
                return
            total += len(raw)
            if len(raw) > MAX_LINE or total > MAX_ARCHIVE or not raw.endswith(b'\n'):
                raise ValueError('Incomplete or oversized receipt archive')
            value = json.loads(raw)
            if not isinstance(value, dict):
                raise ValueError('Invalid receipt record')
            yield number, value
        raise ValueError('Receipt record bound exceeded')


def read_review(path):
    if path is None:
        return []
    with bounded_input(path, MAX_REVIEW) as stream:
        raw = stream.read(MAX_REVIEW + 1)
    if len(raw) > MAX_REVIEW:
        raise ValueError('Review exceeds bound')
    value = json.loads(raw)
    if (not isinstance(value, dict) or type(value.get('version')) is not int or value['version'] != 1 or
            not isinstance(value.get('reviews'), list) or len(value['reviews']) > 2000):
        raise ValueError('Invalid review inventory')
    reviews = value['reviews']
    seen = set()
    for review in reviews:
        if not isinstance(review, dict) or set(review) != {
                'sample_line', 'panel', 'classification', 'capture', 'sha256'}:
            raise ValueError('Invalid review fields')
        line = review['sample_line']
        if (type(line) is not int or line <= 0 or review['panel'] not in ('main', 'cluster') or
                review['classification'] not in CLASSIFICATIONS or
                not isinstance(review['sha256'], str) or
                not re.fullmatch('[0-9a-f]{64}', review['sha256'])):
            raise ValueError('Invalid review reference or classification')
        key = line, review['panel']
        if key in seen:
            raise ValueError('Ambiguous duplicate panel review')
        seen.add(key)
        capture = review['capture']
        if (not isinstance(capture, str) or len(capture) > 512 or
                Path(capture).is_absolute() or not Path(capture).parts or
                any(part in ('.', '..') for part in Path(capture).parts)):
            raise ValueError('Capture must be a relative path within the review directory')
    return reviews


def verify_capture(folder, review):
    path = folder / review['capture']
    if path.resolve() != path or not path.is_relative_to(folder):
        raise ValueError('Capture path is redirected')
    # Original PNG bytes are only hashed. They are never decoded, guessed or
    # copied into a source repository by this tool.
    if path.stat().st_size == 0 or file_hash(path, MAX_CAPTURE) != review['sha256']:
        raise ValueError('Capture hash does not match')


def assess(archive, reviews, review_folder):
    from daily_use_reliability import ready, publisher, decimal
    # At most 2000 caller-selected anchors, never the full receipt archive.
    wanted = {review['sample_line'] for review in reviews}
    anchors = {}
    cases = {}
    first_publishers = {}
    prior = None
    for line, value in read_records(archive):
        if line in wanted:
            if value.get('event') != 'sample':
                raise ValueError('Review must reference an original sample line')
            if not ready(value, prior):
                raise ValueError('Review anchor is stale, replayed or not logically Ready')
            anchors[line] = value
        if value.get('event') == 'sample':
            prior = value
        if value.get('event') == 'case_result':
            index = value.get('index')
            if type(index) is not int or not 1 <= index <= 1000 or index in cases:
                raise ValueError('Invalid or duplicate case result')
            cases[index] = {'index': index, 'logical_passed': value.get('passed') is True,
                            'logical_failure': value.get('failure'),
                            'logical_ready_upper_bound_s': value.get('start_to_readiness_upper_bound_s'),
                            'usable_content': content_evidence(value.get('passed') is True),
                            'reviewed_samples': []}
            first_publishers[index] = value.get('boot_id'), value.get('first_ready_receipt')
    if not cases:
        raise ValueError('No completed case results in original archive')
    if set(anchors) != wanted:
        raise ValueError('Review reference is outside the original archive')
    grouped = {}
    for review in reviews:
        verify_capture(review_folder, review)
        sample = anchors[review['sample_line']]
        case = cases.get(sample.get('case_index'))
        receipt = sample.get('receipt')
        if case is None or not isinstance(receipt, dict):
            raise ValueError('Review lacks original case or publisher context')
        # Use the production logical predicate, including freshness/transport
        # guards. A reviewed image does not repair a rejected logical receipt.
        identity = publisher(sample)
        if not isinstance(identity[0], str) or not identity[0]:
            raise ValueError('Missing original boot identity')
        boot, first = first_publishers[sample['case_index']]
        if (not isinstance(first, dict) or identity !=
                (boot, first.get('nonce'), first.get('connectionGeneration')) or
                decimal(first.get('sequence')) is None or
                decimal(first.get('updatedMonotonicNs')) is None or
                decimal(receipt['sequence']) < decimal(first['sequence']) or
                decimal(receipt['updatedMonotonicNs']) < decimal(first['updatedMonotonicNs'])):
            raise ValueError('Review anchor precedes recovery or belongs to a replaced publisher')
        key = sample['case_index'], review['sample_line']
        if key not in grouped:
            grouped[key] = {'sample_line': review['sample_line'],
                            'publisher': {'boot_id': identity[0], 'nonce': identity[1],
                                          'connection_generation': identity[2]},
                            'receipt_sequence': receipt['sequence'],
                            'receipt_updated_monotonic_ns': receipt['updatedMonotonicNs'],
                            'sample_host_started_ns': sample['host_started_ns'],
                            'sample_host_finished_ns': sample['host_finished_ns'],
                            'capture_date_ns': None, 'capture_binding': 'reviewer_attributed',
                            'panels': {}, 'candidate': None, 'fresh_scene_qualified': None}
        grouped[key]['panels'][review['panel']] = {
            'classification': review['classification'], 'sha256': review['sha256']}
    for (index, _), observation in sorted(grouped.items()):
        panels = observation['panels']
        classifications = [panel['classification'] for panel in panels.values()]
        if any(value not in ('content', 'unknown') for value in classifications):
            observation['candidate'] = False
        elif len(panels) == 2 and classifications == ['content', 'content']:
            observation['candidate'] = True
        cases[index]['reviewed_samples'].append(observation)
    return {'schema_version': 1, 'evidence': 'offline_review', 'bench_qualified': False,
            'logical_result_source': 'original_case_result',
            'completed_cases': len(cases),
            'logical_successful_cases': sum(case['logical_passed'] for case in cases.values()),
            'logical_failed_cases': sum(not case['logical_passed'] for case in cases.values()),
            'reviewed_candidate_samples': sum(value['candidate'] is True for value in grouped.values()),
            'reviewed_nonusable_samples': sum(value['candidate'] is False for value in grouped.values()),
            'fresh_scene_qualified_cases': None, 'optical_first_frame_s': None,
            'cases': list(cases.values()),
            'limits': ['Classifications are explicit reviewer observations, not pixel recognition.',
                       'Sample dates belong to the original receipt; capture dates remain unavailable.',
                       'A reviewer-attributed sample is not an original capture-to-publisher binding.',
                       'Content pixels do not establish current fresh phone video or optical timing.',
                       'Startup/logos/blank/stale/nonpresenting reviews remain separate from logical Ready.']}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipts', required=True, type=Path)
    parser.add_argument('--review', type=Path, help='Private version-1 review manifest, optional')
    parser.add_argument('--output', required=True, help='New private qualification-cache directory')
    args = parser.parse_args(argv)
    from daily_use_reliability import evidence_folder, source_commit
    os.umask(0o077)
    try:
        hashes = {'receipts': file_hash(args.receipts, MAX_ARCHIVE),
                  'review': file_hash(args.review, MAX_REVIEW) if args.review else None}
        reviews = read_review(args.review)
        report = assess(args.receipts, reviews, args.review.parent.resolve() if args.review else None)
        report['source_commit'] = source_commit()
        report['source_sha256'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                   for path in (Path(__file__), Path(__file__).with_name('daily_use_reliability.py'))}
        if hashes != {'receipts': file_hash(args.receipts, MAX_ARCHIVE),
                      'review': file_hash(args.review, MAX_REVIEW) if args.review else None}:
            raise ValueError('Inputs changed during review')
        report['inputs_sha256'] = hashes
        folder = evidence_folder(args.output)
        (folder / 'content-review.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    except (OSError, ValueError, TypeError, RecursionError):
        print('Content evidence unavailable: invalid, incomplete or changed input.', file=sys.stderr)
        return 1
    print(f'Offline review: {report["logical_successful_cases"]}/{report["completed_cases"]} logical; '
          f'{report["reviewed_candidate_samples"]} reviewed candidate samples; fresh-scene/optical unknown; '
          f'{folder / "content-review.json"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
