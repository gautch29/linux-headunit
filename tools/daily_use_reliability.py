#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Repeatable Mac-driven soak, using the existing CM5 manager and owner graph.

Dry runs are synthetic. Raw logs/receipts and reports must remain outside Git.
No phone identity is accepted on the CLI or exported in summary JSON.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import threading
import time

from reliability_probe import OWNERS
from recovery_content_evidence import content_evidence

MAX_AGE_NS = 1_500_000_000
MAX_LINE = 1_500_000
PHONE_ERRORS = {
    'video_frame_timeout': re.compile(r'video.{0,80}frame.{0,80}(?:timed?\s*out|timeout)|'
                                      r'(?:timed?\s*out|timeout).{0,80}video.{0,80}frame', re.I),
    'carplay_error': re.compile(r'(?:carplay|airplay|iap2).{0,120}'
                                r'(?:\berror\b|\bfailed\b|\bfailure\b)', re.I),
}


def integer(value):
    return type(value) is int and value >= 0


def decimal(value):
    if type(value) is not str or not re.fullmatch(r'0|[1-9][0-9]{0,19}', value):
        return None
    number = int(value)
    return number if number < 2 ** 64 else None


def ready(sample, prior=None):
    """Require the manager's admitted state plus its original matching receipt."""
    manager, receipt = sample.get('manager'), sample.get('receipt')
    now = sample.get('remote_monotonic_ns')
    begin, finish = sample.get('host_started_ns'), sample.get('host_finished_ns')
    if not integer(begin) or not integer(finish) or not 0 <= finish - begin <= 5_000_000_000:
        return False
    if (not isinstance(manager, dict) or not isinstance(receipt, dict) or not integer(now) or
            not isinstance(manager.get('phone_home'), dict)):
        return False
    generation = decimal(receipt.get('connectionGeneration'))
    updated = decimal(receipt.get('updatedMonotonicNs'))
    sequence = decimal(receipt.get('sequence'))
    nonce = receipt.get('nonce')
    state = {'state': 'ready', 'returnReady': True}
    if (manager.get('carplay') != state or receipt.get('state') != 'ready' or
            receipt.get('returnReady') is not True or manager.get('active_present') is not True or
            manager.get('error_present') is not False or not integer(manager.get('generation')) or
            receipt.get('version') not in (2, 3) or not generation or not sequence or not updated or
            not isinstance(nonce, str) or not re.fullmatch(r'[0-9a-f]{32}', nonce) or
            not 0 <= now - updated <= MAX_AGE_NS or
            manager.get('active_mode') not in ('standard', 'ultra') or
            receipt.get('negotiatedMode') != manager['active_mode'] or
            (manager.get('phone_home') or {}).get('connection') != str(generation)):
        return False
    if prior and sample.get('boot_id') == prior.get('boot_id'):
        previous = prior.get('receipt') or {}
        if nonce == previous.get('nonce'):
            old_sequence = decimal(previous.get('sequence'))
            old_date = decimal(previous.get('updatedMonotonicNs'))
            old_generation = decimal(previous.get('connectionGeneration'))
            if (old_sequence is None or old_date is None or old_generation is None or
                    sequence < old_sequence or updated < old_date or generation < old_generation or
                    sequence == old_sequence and receipt != previous):
                return False
    return True


def publisher(sample):
    value = sample.get('receipt') or {}
    return sample.get('boot_id'), value.get('nonce'), value.get('connectionGeneration')


def iter_samples(path):
    """Replay bounded receipts from disk; malformed evidence makes counts unavailable."""
    try:
        with path.open('rb') as stream:
            while True:
                raw = stream.readline(MAX_LINE + 1)
                if not raw:
                    break
                if len(raw) > MAX_LINE or not raw.endswith(b'\n'):
                    raise ValueError('Receipt line is incomplete or exceeds bound')
                value = json.loads(raw)
                if not isinstance(value, dict):
                    raise ValueError('Invalid receipt record')
                if value.get('event') == 'sample':
                    yield value
    except (OSError, ValueError):
        yield {}  # No valid owner counters are available for the missing evidence.


def restart_summary(samples):
    """Aggregate one bounded owner record per service, in a single streaming pass."""
    states = {owner: {'previous': None, 'automatic': 0, 'invocations': 0,
                      'unavailable': False, 'offline_samples': 0} for owner in OWNERS}
    for sample in samples:
        for owner, state in states.items():
            fields = (sample.get('owners') or {}).get(owner)
            if not isinstance(fields, dict):
                if sample.get('expected_offline') is True:
                    state['offline_samples'] += 1
                else:
                    state['unavailable'] = True
                continue
            count = decimal(fields.get('NRestarts'))
            invocation = fields.get('InvocationID')
            if count is None or not invocation or not integer(sample.get('host_finished_ns')):
                state['unavailable'] = True
                continue
            current = sample.get('boot_id'), invocation, count
            previous = state['previous']
            if previous:
                if current[:2] != previous[:2]:
                    state['invocations'] += 1
                if current[0] != previous[0]:
                    state['automatic'] += count
                elif count >= previous[2]:
                    state['automatic'] += count - previous[2]
                else:
                    state['unavailable'] = True
            state['previous'] = current
    return {owner: {'automatic_restarts': None if state['unavailable'] or state['previous'] is None
                    else state['automatic'],
                    'observed_invocation_changes': state['invocations'] if state['previous'] else None,
                    'expected_offline_samples': state['offline_samples']}
            for owner, state in states.items()}


def phone_errors(path, collection):
    counts = dict.fromkeys(PHONE_ERRORS, 0)
    lines = 0
    if not collection.get('complete'):
        return {'available': False, 'counts': dict.fromkeys(PHONE_ERRORS), 'lines': None}
    try:
        with path.open('rb') as stream:
            while True:
                raw = stream.readline(MAX_LINE + 1)
                if not raw:
                    break
                if len(raw) > MAX_LINE:
                    raise ValueError('Phone log line exceeds bound')
                line = raw.decode('utf-8', errors='replace')
                lines += 1
                for name, pattern in PHONE_ERRORS.items():
                    counts[name] += bool(pattern.search(line))
    except (OSError, ValueError):
        return {'available': False, 'counts': dict.fromkeys(PHONE_ERRORS), 'lines': None}
    # An empty or diagnostic-only collector is not evidence of an error-free phone.
    available = lines > 0 and collection.get('phone_lines', 0) > 0
    return {'available': available, 'counts': counts if available else dict.fromkeys(PHONE_ERRORS),
            'lines': lines}


def journal_summary(path, collection):
    lines = 0
    failures = 0
    boot_ids = set()
    if not collection.get('complete'):
        return {'available': False, 'records': None, 'failure_records': None}
    try:
        with path.open('rb') as stream:
            while True:
                raw = stream.readline(MAX_LINE + 1)
                if not raw:
                    break
                if len(raw) > MAX_LINE:
                    raise ValueError('Journal record exceeds bound')
                value = json.loads(raw)
                if not isinstance(value, dict) or not value.get('__CURSOR'):
                    raise ValueError('Invalid journal record')
                boot_ids.add(value.get('_BOOT_ID'))
                lines += 1
                message = value.get('MESSAGE', '')
                if isinstance(message, str) and re.search(r'\b(?:failed|failure|segfault)\b', message, re.I):
                    failures += 1
                if lines > 10000:
                    raise ValueError('Journal excerpt truncated at record bound')
    except (OSError, ValueError, TypeError):
        return {'available': False, 'records': None, 'failure_records': None}
    return {'available': True, 'records': lines, 'failure_records': failures,
            'boot_count': len(boot_ids)}


def summarize(cases, samples, phone, journals, requested, synthetic=False, interrupted=False):
    owners = restart_summary(samples)
    failures = []
    if len(cases) != requested or interrupted:
        failures.append('run_incomplete')
    if any(case.get('passed') is not True for case in cases):
        failures.append('scenario_failed')
    if not phone.get('available'):
        failures.append('phone_log_unavailable')
    elif any(count for count in phone['counts'].values()):
        failures.append('phone_error_pattern')
    if len(journals) != requested or any(not value.get('available') for value in journals):
        failures.append('journal_unavailable')
    elif any(value.get('failure_records') for value in journals):
        failures.append('journal_failure_pattern')
    for owner, value in owners.items():
        if value['automatic_restarts'] is None:
            failures.append(owner + ':restart_count_unavailable')
        elif value['automatic_restarts']:
            failures.append(owner + ':automatic_restart')
        permitted = sum(case.get('kind') == 'cold-boot' or
                        case.get('kind') == 'restart' and owner == 'headunit-receiver' for case in cases)
        changes = value['observed_invocation_changes']
        if changes is not None and changes > permitted:
            failures.append(owner + ':unexpected_invocation_change')
    return {'schema_version': 2, 'passed': not failures, 'evidence': 'synthetic' if synthetic else 'bench',
            'logical_recovery_passed': len(cases) == requested and not interrupted and
            all(case.get('passed') is True for case in cases),
            'bench_qualified': False, 'requested_cases': requested,
            'completed_cases': len(cases), 'successful_cases': sum(case.get('passed') is True for case in cases),
            'failed_cases': sum(case.get('passed') is not True for case in cases),
            'incomplete_cases': max(0, requested - len(cases)),
            'usable_content': content_evidence(all(case.get('passed') is True for case in cases) and bool(cases)),
            'failures': failures, 'cases': cases, 'owners': owners, 'phone_errors': phone,
            'journals': journals,
            'limits': ['First physical CarPlay frame is unavailable without an optical timing reference.',
                       'PASS covers logical recovery and collection checks; usable visual recovery remains unqualified.',
                       'No original content identity/phase or fresh-video presentation observation is published.',
                       'Readiness timing is a receipt-backed polling upper bound, not a physical frame date.',
                       'Phone patterns are conservative heuristics, not a complete phone error taxonomy.',
                       'NRestarts measures observed automatic restarts; InvocationID tracks observed owner replacements.',
                       'Expected cold-power SSH outages retain unavailable samples; new boot counters are measured independently.']}


class Capture:
    """Drain bounded bytes directly to a private Mac file; retire only our process."""
    def __init__(self, command, path, maximum=256 * 1024 * 1024, stdin=None, phone=False):
        self.complete = False
        self.overflow = False
        self.phone_lines = 0
        self.bytes = 0
        self.stopping = False
        self.drain_error = None
        self.phone = phone
        self.path = path
        self.file = path.open('xb')
        try:
            self.process = subprocess.Popen(command, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            start_new_session=True)
        except BaseException:
            self.file.close()
            raise
        self.maximum = maximum
        self.thread = threading.Thread(target=self.drain, daemon=True)
        self.thread.start()
        if stdin:
            try:
                self.process.stdin.write(stdin)
                self.process.stdin.close()
            except OSError:
                self.stop()
                raise

    def drain(self):
        pending = b''
        try:
            while True:
                raw = self.process.stdout.read1(65536)
                if not raw:
                    break
                self.bytes += len(raw)
                if self.bytes > self.maximum:
                    self.overflow = True
                    self.process.terminate()
                    break
                self.file.write(raw)
                if self.phone:
                    pending += raw
                    rows = pending.split(b'\n')
                    pending = rows.pop()
                    if len(pending) > MAX_LINE:
                        self.overflow = True
                        self.process.terminate()
                        break
                    self.phone_lines += sum(bool(re.match(rb'[A-Z][a-z]{2}\s+\d{1,2}\s+\d\d:', row))
                                            for row in rows)
            self.file.flush()
        except OSError as error:
            self.drain_error = type(error).__name__
            self.process.terminate()
        finally:
            self.file.close()
            self.process.stdout.close()

    def stop(self):
        self.stopping = self.process.poll() is None
        if self.stopping:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            except PermissionError:
                # Darwin can report EPERM for an exited, zombie-only group.
                # Reaping our own exited child proves retirement; a live child
                # still times out and leaves this an error.
                self.process.wait(timeout=3)
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait(timeout=3)
        self.thread.join(timeout=3)
        self.complete = not self.overflow and not self.drain_error and not self.thread.is_alive() and (
            self.stopping if self.phone else self.process.returncode == 0)
        return {'complete': self.complete, 'bytes': self.bytes, 'overflow': self.overflow,
                'exit_code': self.process.returncode, 'phone_lines': self.phone_lines,
                'drain_error': self.drain_error}


class Bench:
    def __init__(self, args, folder):
        self.args, self.folder = args, folder
        self.collector = None
        self.script = Path(__file__).with_name('reliability_probe.py').read_bytes()
        self.ssh = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=6',
                    '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2']
        if args.ssh_key:
            self.ssh.extend(['-i', str(args.ssh_key)])
        self.ssh.append(args.host)

    def command(self, operation, cursor=''):
        return [*self.ssh, shlex.join(['python3', '-B', '-E', '-s', '-', operation,
                                     self.args.state_directory, cursor])]

    def probe(self, operation):
        if self.collector and (self.collector.process.poll() is not None or
                               self.collector.overflow or self.collector.drain_error):
            raise ValueError('phone_collector_stopped')
        begin = time.monotonic_ns()
        try:
            result = subprocess.run(self.command(operation), input=self.script,
                                    capture_output=True, timeout=50, check=True)
            if len(result.stdout) > MAX_LINE:
                raise ValueError('Probe output exceeds bound')
            value = json.loads(result.stdout)
            if not isinstance(value, dict) or value.get('probe_error'):
                raise ValueError('Invalid probe result')
        except (subprocess.SubprocessError, OSError, ValueError):
            value = {'probe_error': 'unavailable'}
        return {**value, 'host_started_ns': begin, 'host_finished_ns': time.monotonic_ns()}

    def journal(self, index, cursor):
        if not cursor:
            return {'available': False, 'records': None, 'failure_records': None}
        path = self.folder / f'journal-{index:04d}.jsonl'
        capture = Capture(self.command('journal', cursor), path, 32 * 1024 * 1024, stdin=self.script)
        try:
            capture.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pass
        collection = capture.stop()
        return journal_summary(path, collection)


class Simulation:
    def __init__(self, failure=False):
        self.ns = 10_000_000_000
        self.boot = 1
        self.generation = 1
        self.connected = True
        self.invocation = 1
        self.failure = failure

    def probe(self, operation):
        self.ns += 250_000_000
        if operation == 'connect':
            self.generation += 1
            self.connected = not self.failure
        elif operation == 'disconnect':
            self.connected = False
        elif operation == 'restart':
            self.invocation += 1
            self.generation += 1
            self.connected = not self.failure
        state = 'ready' if self.connected else 'disconnected'
        value = {'boot_id': str(self.boot), 'remote_monotonic_ns': self.ns,
                 'host_started_ns': self.ns, 'host_finished_ns': self.ns + 1000000}
        if operation == 'cursor':
            value['cursor'] = 'synthetic'
        elif operation == 'sample':
            nonce = f'{self.generation:032x}'
            value.update(manager={'generation': self.generation, 'active_mode': 'ultra',
                                  'active_present': self.connected, 'error_present': False,
                                  'phone_home': {'connection': str(self.generation)},
                                  'carplay': {'state': state, 'returnReady': self.connected}},
                         receipt={'version': 3, 'nonce': nonce, 'sequence': str(self.ns),
                                  'connectionGeneration': str(self.generation),
                                  'updatedMonotonicNs': str(self.ns), 'state': state,
                                  'returnReady': self.connected, 'negotiatedMode': 'ultra',
                                  'phoneHomeRequests': '0'},
                         owners={owner: {'NRestarts': '0', 'InvocationID':
                                         f'{self.boot}-{self.invocation if owner == "headunit-receiver" else 1}',
                                         'ActiveState': 'active', 'SubState': 'running', 'MainPID': '1'}
                                 for owner in OWNERS})
        else:
            value['acknowledged'] = True
        return value

    def journal(self, index, cursor):
        return {'available': True, 'records': 0, 'failure_records': 0, 'boot_count': 0}


def run_cases(backend, args, record, cases, journals):
    last = None
    expected_offline = False
    case_index = 0

    def sample():
        nonlocal last
        value = backend.probe('sample')
        value.update(expected_offline=expected_offline, case_index=case_index)
        record('sample', value)
        last = value
        return value

    def action(operation):
        value = backend.probe(operation)
        record(operation, value)
        if value.get('acknowledged') is not True:
            raise ValueError(operation + '_not_acknowledged')
        return value

    def wait(predicate, reason):
        end = time.monotonic() + args.timeout
        # Simulated failures stay bounded without sleeping through real deadlines.
        attempts = 4 if args.dry_run else None
        while time.monotonic() < end:
            old = last
            value = sample()
            if time.monotonic() <= end and predicate(value, old):
                return value
            if attempts is not None:
                attempts -= 1
                if attempts == 0:
                    break
            else:
                time.sleep(args.poll_seconds)
        raise ValueError(reason)

    def disconnected(value, _):
        manager = value.get('manager') or {}
        return (manager.get('carplay') == {'state': 'disconnected', 'returnReady': False} and
                manager.get('active_present') is False)

    def establish():
        value = sample()
        if ready(value):
            return value
        action('connect')
        return wait(lambda current, old: ready(current, old), 'initial_connect_timeout')

    schedule = (['cycle'] * args.cycles + ['connect-again'] * args.burst_connects +
                ['link-loss'] * args.link_loss_cycles + ['restart'] * args.service_restarts +
                ['cold-boot'] * args.cold_boots)
    sample()
    for index, kind in enumerate(schedule, 1):
        case_index = index
        expected_offline = False
        print(f'Case {index}/{len(schedule)}: {kind}', flush=True)
        case = {'index': index, 'kind': kind, 'passed': False, 'failure': None,
                'start_to_first_frame_s': None, 'start_to_first_frame_upper_bound_s': None,
                'start_to_readiness_upper_bound_s': None,
                'reconnect_upper_bound_s': None, 'poll_interval_s': args.poll_seconds,
                'poll_transport_duration_s': None,
                'intentional_service_restart': kind == 'restart',
                'physical_power_on_date_available': False,
                'usable_content': content_evidence(False)}
        cases.append(case)
        cursor = backend.probe('cursor')
        record('cursor', cursor)
        start = None
        baseline = None
        try:
            if kind == 'cycle':
                baseline = sample()
                if (baseline.get('manager') or {}).get('active_present'):
                    action('disconnect')
                wait(disconnected, 'disconnect_retirement_timeout')
                start = action('connect')
            elif kind == 'connect-again':
                baseline = establish()
                start = action('connect')
            elif kind == 'restart':
                baseline = establish()
                start = action('restart')
            elif kind == 'link-loss':
                baseline = establish()
                if args.dry_run:
                    backend.connected = False
                else:
                    input('Disable the phone wireless link (keep USB logging), then press Enter. ')
                # A usable not-ready manager report is required; SSH/receipt loss alone is not link loss.
                wait(lambda value, _: isinstance(value.get('manager'), dict) and
                     value['manager'].get('carplay', {}).get('returnReady') is False,
                     'link_loss_not_observed')
                if args.dry_run:
                    backend.generation += 1
                    backend.connected = not args.dry_run_failure
                    start = backend.probe('marker')
                else:
                    input('Restore the phone wireless link; press Enter immediately after restoring it. ')
                    start = {'host_started_ns': time.monotonic_ns()}
                case['recovery_method'] = args.link_loss_reconnect
                if args.link_loss_reconnect == 'manager':
                    action('connect')
            else:
                baseline = establish()
                if args.dry_run:
                    backend.boot += 1
                    backend.generation += 1
                    backend.connected = not args.dry_run_failure
                    start = backend.probe('marker')
                else:
                    input('Cold-power-cycle the CM5; press Enter immediately after applying power. ')
                    start = {'host_started_ns': time.monotonic_ns()}
                case['timing_origin'] = 'operator_confirmation_after_power_on'
                expected_offline = True
            record('start', {'case_index': index, **start})
            first = wait(lambda value, old: ready(value, old) and
                         (baseline is None or publisher(value) != publisher(baseline)) and
                         (kind != 'cold-boot' or value.get('boot_id') != baseline.get('boot_id')),
                         'fresh_readiness_timeout')
            elapsed = (first['host_finished_ns'] - start['host_started_ns']) / 1e9
            if elapsed < 0:
                raise ValueError('host_clock_reversed')
            case['start_to_readiness_upper_bound_s'] = elapsed
            # Ready can precede useful phone content (V84 blank main/LOGOS).
            # Leave both first-frame fields unavailable, rather than copying a
            # logically admitted publication date into a visual timing field.
            case['usable_content'] = content_evidence(True)
            if kind in ('cycle', 'connect-again', 'link-loss'):
                case['reconnect_upper_bound_s'] = elapsed
            case['first_ready_receipt'] = first.get('receipt')
            case['boot_id'] = first.get('boot_id')
            case['poll_transport_duration_s'] = (first['host_finished_ns'] - first['host_started_ns']) / 1e9
            case['timing_origin'] = case.get('timing_origin', 'host_dispatch_before_operation')
            expected_offline = False
            end = time.monotonic() + args.hold_seconds
            confirmed = False
            while args.dry_run or time.monotonic() < end or not confirmed:
                if not args.dry_run:
                    time.sleep(args.poll_seconds)
                old = last
                value = sample()
                if not ready(value, old) or publisher(value) != publisher(first):
                    raise ValueError('readiness_lost_during_hold')
                owners = value.get('owners') or {}
                if any((owners.get(owner) or {}).get('ActiveState') != 'active' for owner in OWNERS):
                    raise ValueError('owner_not_active')
                confirmed = decimal(value['receipt']['sequence']) > decimal(first['receipt']['sequence'])
                if args.dry_run or confirmed and time.monotonic() >= end:
                    break
                if time.monotonic() > end + args.timeout:
                    raise ValueError('receipt_did_not_advance')
            if kind in ('restart', 'cold-boot'):
                expected = OWNERS if kind == 'cold-boot' else ('headunit-receiver',)
                if any((first.get('owners', {}).get(owner) or {}).get('InvocationID') ==
                       (baseline.get('owners', {}).get(owner) or {}).get('InvocationID')
                       for owner in expected):
                    raise ValueError('expected_owner_replacement_not_observed')
            case['passed'] = confirmed
            if not confirmed:
                case['failure'] = 'receipt_did_not_advance'
        except ValueError as error:
            case['failure'] = str(error)
        finally:
            # Retain final owners even after a failure, then bound journal archival.
            try:
                sample()
            except ValueError as error:
                case['passed'] = False
                case['failure'] = str(error)
            journals.append(backend.journal(index, cursor.get('cursor')))
            case['logical_recovery_passed'] = case['passed']
            case['usable_content'] = content_evidence(case['passed'])
            record('case_result', case)
        if not case['passed']:
            break  # Preserve the failed state for the bench owner; no retry hides a failure.


def evidence_folder(raw):
    path = Path(raw).expanduser().absolute()
    cache = (Path.home() / '.cache/headunit-qualification').resolve()
    if not path.is_relative_to(cache) or path.resolve() != path:
        raise ValueError('Evidence must use a new real directory under ~/.cache/headunit-qualification/')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.mkdir(mode=0o700)
    return path


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, help='New private qualification-cache directory')
    parser.add_argument('--dry-run', action='store_true', help='Synthetic run; no SSH, phone or services')
    parser.add_argument('--dry-run-failure', action='store_true', help='Simulate missing reconnect readiness')
    parser.add_argument('--host', help='SSH destination for the appliance')
    parser.add_argument('--ssh-key', type=Path)
    parser.add_argument('--state-directory', help='Absolute native Home state path on the appliance')
    parser.add_argument('--cycles', type=int, default=10)
    parser.add_argument('--burst-connects', type=int, default=10, help='Connect again without a preceding Disconnect')
    parser.add_argument('--link-loss-cycles', type=int, default=0, help='Operator-controlled wireless-loss cases')
    parser.add_argument('--link-loss-reconnect', choices=('automatic', 'manager'), default='automatic')
    parser.add_argument('--service-restarts', type=int, default=0, help='Restart only existing receiver/manager service')
    parser.add_argument('--cold-boots', type=int, default=0, help='Operator-controlled cold-power cases')
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--poll-seconds', type=float, default=.25)
    parser.add_argument('--hold-seconds', type=float, default=2)
    args = parser.parse_args(argv)
    counts = (args.cycles, args.burst_connects, args.link_loss_cycles, args.service_restarts, args.cold_boots)
    if any(not 0 <= count <= 1000 for count in counts) or not 1 <= sum(counts) <= 1000:
        parser.error('Request 1..1000 total cases, with nonnegative counts')
    for name, lower, upper in (('timeout', 1, 600), ('poll_seconds', .1, 5), ('hold_seconds', 0, 1800)):
        value = getattr(args, name)
        if not math.isfinite(value) or not lower <= value <= upper:
            parser.error(name + f' must be {lower}..{upper}')
    if args.dry_run_failure and not args.dry_run:
        parser.error('--dry-run-failure requires --dry-run')
    if not args.dry_run and (not args.host or not args.state_directory):
        parser.error('Live runs require --host and --state-directory')
    if args.host is not None and (args.host.startswith('-') or not re.fullmatch(r'[A-Za-z0-9_.@-]+', args.host)):
        parser.error('Invalid SSH host')
    if args.state_directory is not None and not Path(args.state_directory).is_absolute():
        parser.error('State directory must be absolute')
    return args


def source_commit():
    """Source hashes still identify an exported snapshot without Git metadata."""
    try:
        result = subprocess.run(
            ['git', '-C', str(Path(__file__).resolve().parents[1]), 'rev-parse', 'HEAD'],
            capture_output=True, text=True, timeout=5, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    revision = result.stdout.strip()
    if result.returncode == 0 and re.fullmatch(r'[0-9a-f]{40}', revision):
        return revision
    return None


def main(argv=None):
    args = arguments(argv)
    os.umask(0o077)
    try:
        folder = evidence_folder(args.output)
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2
    cases, journals = [], []
    receipt_bytes = 0
    requested = args.cycles + args.burst_connects + args.link_loss_cycles + args.service_restarts + args.cold_boots
    collector = None
    phone = {'available': False, 'counts': dict.fromkeys(PHONE_ERRORS), 'lines': None}
    interrupted = False
    with (folder / 'receipts.jsonl').open('x') as receipts:
        def record(event, value):
            nonlocal receipt_bytes
            raw = json.dumps({'event': event, **value}, allow_nan=False) + '\n'
            receipt_bytes += len(raw.encode('utf-8'))
            if receipt_bytes > 256 * 1024 * 1024:
                raise ValueError('receipt_archive_byte_bound')
            receipts.write(raw)
            receipts.flush()

        def terminate(_signum, _frame):
            raise KeyboardInterrupt

        previous = signal.signal(signal.SIGTERM, terminate)
        try:
            if args.dry_run:
                backend = Simulation(args.dry_run_failure)
                phone = {'available': True, 'counts': dict.fromkeys(PHONE_ERRORS, 0), 'lines': 1}
            else:
                backend = Bench(args, folder)
                collector = Capture(['idevicesyslog'], folder / 'phone-syslog.log', phone=True)
                backend.collector = collector
                # Confirm actual phone lines before any live operation.
                deadline = time.monotonic() + 10
                while collector.phone_lines == 0 and collector.process.poll() is None and time.monotonic() < deadline:
                    time.sleep(.1)
                if collector.phone_lines == 0 or collector.process.poll() is not None:
                    raise ValueError('phone_collector_not_ready')
            run_cases(backend, args, record, cases, journals)
        except (KeyboardInterrupt, EOFError):
            interrupted = True
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            if receipt_bytes <= 256 * 1024 * 1024:
                record('run_error', {'error_type': type(error).__name__})
            interrupted = True
        finally:
            # A second signal must not interrupt retirement/report generation.
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            old_int = signal.signal(signal.SIGINT, signal.SIG_IGN)
            if collector:
                collection = collector.stop()
                (folder / 'phone-collector.json').write_text(json.dumps(collection, indent=2) + '\n')
                phone = phone_errors(folder / 'phone-syslog.log', collection)
            signal.signal(signal.SIGTERM, previous)
            signal.signal(signal.SIGINT, old_int)
    summary = summarize(cases, iter_samples(folder / 'receipts.jsonl'), phone, journals,
                        requested, args.dry_run, interrupted)
    summary['created_utc'] = datetime.now(timezone.utc).isoformat()
    summary['source_commit'] = source_commit()
    summary['harness_sha256'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in
                                (Path(__file__), Path(__file__).with_name('reliability_probe.py'),
                                 Path(__file__).with_name('recovery_content_evidence.py'))}
    summary['interrupted'] = interrupted
    summary['parameters'] = {key: getattr(args, key) for key in
                             ('cycles', 'burst_connects', 'link_loss_cycles', 'link_loss_reconnect',
                              'service_restarts', 'cold_boots', 'timeout', 'poll_seconds', 'hold_seconds')}
    (folder / 'summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False) + '\n')
    hashes = {}
    for path in folder.iterdir():
        if path.is_file():
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(65536), b''):
                    digest.update(block)
            hashes[path.name] = digest.hexdigest()
    (folder / 'evidence-sha256.json').write_text(json.dumps(hashes, indent=2) + '\n')
    print(f'{"PASS" if summary["passed"] else "FAIL"} logical/collection checks ({summary["evidence"]}): '
          f'{summary["successful_cases"]}/{requested} cases; {summary["failed_cases"]} failed, '
          f'{summary["incomplete_cases"]} incomplete; usable/fresh-scene/optical unknown; '
          f'{folder / "summary.json"}')
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
