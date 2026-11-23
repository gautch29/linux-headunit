#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Offline single-case phone-free owner receipt audit; no live backend.

Never signals a service, reboots, connects a phone or verifies a release remotely.
Input binding is collector-attributed. Logical owner recovery is not visibility.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import sys
import time

from daily_use_reliability import decimal, evidence_folder
from recovery_content_evidence import bounded_input, content_evidence
from reliability_probe import OWNERS

MAX_ARCHIVE = 8 * 1024 * 1024
MAX_VERIFICATION = 2 * 1024 * 1024
MAX_LINE = 1_500_000
PHASES = ('before', 'dispatched', 'retired', 'restarted')
SCOPE = ('source_commit', 'selected_manifest_sha256', 'helper_manifest_sha256', 'units_sha256')


class Rejected(ValueError):
    """Only fixed public codes may leave the private receipt parser."""


class Interrupted(BaseException):
    def __init__(self, number):
        self.number = number


def require(condition, code):
    if not condition:
        raise Rejected(code)


def exact_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate_json_key')
        result[key] = value
    return result


def digest_matches(value, length):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{' + str(length) + '}', value) is not None


def check_scope(scope):
    require(isinstance(scope, dict) and set(scope) == set(SCOPE), 'scope_missing')
    require(digest_matches(scope['source_commit'], 40), 'source_missing')
    require(all(digest_matches(scope[key], 64) for key in SCOPE[1:3]), 'binding_missing')
    units = scope['units_sha256']
    require(isinstance(units, dict) and set(units) == set(OWNERS) and
            all(digest_matches(value, 64) for value in units.values()), 'units_missing')


def owner_identity(fields, owner, active):
    require(isinstance(fields, dict), 'owner_unavailable')
    require(fields.get('Id') == owner + '.service', 'foreign_owner')
    pid = decimal(fields.get('MainPID'))
    count = decimal(fields.get('NRestarts'))
    invocation = fields.get('InvocationID')
    require(pid is not None and pid < 2 ** 31 and count is not None, 'owner_counter_unavailable')
    if active:
        require(fields.get('ActiveState') == 'active' and fields.get('SubState') == 'running' and
                pid > 1 and digest_matches(invocation, 32), 'owner_not_healthy')
    else:
        require(fields.get('ActiveState') == 'inactive' and fields.get('SubState') == 'dead' and
                pid == 0 and (invocation == '' or digest_matches(invocation, 32)), 'retirement_unproven')
    return pid, invocation, count


def check_sample(value, phase, plan):
    require(value.get('event') == 'sample' and value.get('phase') == phase, 'phase_missing_or_reordered')
    require(value.get('scope') == plan['scope'], 'authority_changed')
    boot = value.get('boot_id')
    require(isinstance(boot, str) and re.fullmatch('[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}', boot),
            'boot_unavailable')
    dates = [value.get(key) for key in ('remote_monotonic_ns', 'host_started_ns', 'host_finished_ns')]
    require(all(type(date) is int and 0 < date < 2 ** 64 for date in dates), 'date_unavailable')
    require(0 <= dates[2] - dates[1] <= 5_000_000_000, 'observation_budget')
    require(isinstance(value.get('owners'), dict) and set(value['owners']) == set(OWNERS), 'fleet_unavailable')
    # These are the existing probe's sanitized manager fields. None during the
    # receiver's verified shutdown is expected, never a native-content receipt.
    manager = value.get('manager')
    if phase == 'retired' and plan['owner'] == 'headunit-receiver':
        require(manager is None, 'retired_manager_still_admitted')
    else:
        require(isinstance(manager, dict) and manager.get('active_present') is False and
                manager.get('error_present') is False, 'phonefree_status_unavailable')
    require('receipt' in value, 'status_receipt_missing')
    receipt = value['receipt']
    require(receipt is None or isinstance(receipt, dict) and
            receipt.get('returnReady') is False and receipt.get('state') in
            ('disconnected', 'stopped', 'starting', 'fault'),
            'phone_receipt_present_or_missing')
    gate = value.get('boot_gate')
    require(isinstance(gate, dict) and gate.get('Id') == 'headunit-boot-recovery.service' and
            gate.get('ActiveState') == 'active' and gate.get('SubState') == 'exited' and
            gate.get('Result') == 'success' and digest_matches(gate.get('InvocationID'), 32),
            'boot_gate_unavailable')
    return dates


def assess(archive, verification_sha256, deadline):
    """Consume exactly one plan and four original observations, fail at first guard."""
    digest = hashlib.sha256()
    total = 0
    phases = []
    previous = None
    before = None
    with bounded_input(archive, MAX_ARCHIVE) as stream:
        for index in range(6):
            require(time.monotonic() <= deadline, 'audit_budget')
            raw = stream.readline(MAX_LINE + 1)
            if not raw:
                require(index == 5, 'lost_receipt')
                break
            total += len(raw)
            require(total <= MAX_ARCHIVE and len(raw) <= MAX_LINE and raw.endswith(b'\n'), 'partial_or_overbound')
            digest.update(raw)
            value = json.loads(raw, object_pairs_hook=exact_object,
                               parse_constant=lambda _: require(False, 'nonfinite_json'))
            require(isinstance(value, dict), 'record_unavailable')
            if index == 0:
                plan = value
                require(set(plan) == {'event', 'schema_version', 'case', 'owner', 'planned_restarts',
                                      'evidence', 'verification_sha256', 'scope'}, 'plan_unavailable')
                require(plan['event'] == 'plan' and type(plan['schema_version']) is int and
                        plan['schema_version'] == 1 and plan['case'] == 'stop-start', 'case_unsupported')
                require(plan['owner'] in OWNERS and type(plan['planned_restarts']) is int and
                        plan['planned_restarts'] == 1, 'owner_or_plan_unsupported')
                require(plan['evidence'] in ('synthetic', 'operator_collected'), 'evidence_scope_unavailable')
                require(plan['verification_sha256'] == verification_sha256, 'verification_mismatch')
                check_scope(plan['scope'])
                continue
            require(index <= 4, 'multiple_cases_or_extra_receipt')
            phase = PHASES[index - 1]
            dates = check_sample(value, phase, plan)
            current = {owner: owner_identity(value['owners'][owner], owner,
                       not (phase == 'retired' and owner == plan['owner'])) for owner in OWNERS}
            if before is None:
                before = value
                original = current
                require(len({item[0] for item in current.values()}) == len(OWNERS), 'duplicate_pid')
            else:
                require(value['boot_id'] == before['boot_id'], 'foreign_boot')
                require(value['boot_gate'] == before['boot_gate'], 'boot_gate_changed')
                require(dates[0] > previous['remote_monotonic_ns'] and
                        dates[1] >= previous['host_finished_ns'], 'replayed_or_overlapping_observation')
                require(dates[0] - before['remote_monotonic_ns'] <= 90_000_000_000 and
                        dates[2] - before['host_started_ns'] <= 90_000_000_000, 'case_budget')
                for owner, identity in current.items():
                    require(identity[2] == original[owner][2], 'automatic_restart_or_counter_reset')
                    if owner != plan['owner'] or phase == 'dispatched':
                        require(identity == original[owner], 'unexpected_owner_replacement')
                    elif phase == 'retired':
                        require(identity[1] in ('', original[owner][1]), 'foreign_retirement')
                    elif phase == 'restarted':
                        require(identity[0] != original[owner][0] and identity[1] != original[owner][1],
                                'successor_unproven')
                require(len({item[0] for item in current.values() if item[0]}) ==
                        sum(item[0] != 0 for item in current.values()), 'duplicate_pid')
            if phase in ('dispatched', 'restarted'):
                dispatch = value.get('dispatch')
                require(isinstance(dispatch, dict) and set(dispatch) == {
                        'operation', 'owner', 'boot_id', 'expected_pid', 'expected_invocation_id',
                        'dispatched_monotonic_ns', 'acknowledged'}, 'dispatch_receipt_missing')
                require(dispatch['operation'] == ('stop' if phase == 'dispatched' else 'start') and
                        dispatch['owner'] == plan['owner'] and dispatch['boot_id'] == before['boot_id'] and
                        dispatch['acknowledged'] is True, 'dispatch_not_acknowledged')
                expected = original[plan['owner']] if phase == 'dispatched' else (0, '', 0)
                require(type(dispatch['expected_pid']) is int and dispatch['expected_pid'] == expected[0] and
                        dispatch['expected_invocation_id'] == expected[1], 'dispatch_identity_changed')
                dispatch_date = dispatch['dispatched_monotonic_ns']
                require(type(dispatch_date) is int and
                        previous['remote_monotonic_ns'] < dispatch_date <= dates[0], 'dispatch_date_unavailable')
            phases.append({'phase': phase, 'boot_id': value['boot_id'],
                           'remote_monotonic_ns': dates[0], 'host_started_ns': dates[1],
                           'host_finished_ns': dates[2],
                           'owners': {owner: {'pid': item[0], 'invocation_id': item[1],
                                             'NRestarts': item[2]} for owner, item in current.items()}})
            previous = value
    require(len(phases) == 4, 'lost_receipt')
    return {'logical_owner_recovery_observed': True, 'evidence': plan['evidence'],
            'case': plan['case'], 'owner': plan['owner'], 'scope': plan['scope'],
            'planned_restarts': 1, 'observed_planned_replacements': 1,
            'automatic_restart_deltas': dict.fromkeys(OWNERS, 0), 'phases': phases,
            'boot_id': before['boot_id'], 'receipts_sha256': digest.hexdigest(),
            'logical_service_return_poll_upper_bound_s':
            (phases[-1]['host_finished_ns'] - phases[1]['host_started_ns']) / 1e9}


def retain(path, destination, maximum, deadline):
    digest = hashlib.sha256()
    total = 0
    with bounded_input(path, maximum) as stream, destination.open('xb') as output:
        os.chmod(destination, 0o600)
        while True:
            require(time.monotonic() <= deadline, 'audit_budget')
            raw = stream.read(65536)
            if not raw:
                break
            total += len(raw)
            require(total <= maximum, 'retention_budget')
            output.write(raw)
            digest.update(raw)
        output.flush()
        os.fsync(output.fileno())
    return digest.hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipts', required=True, type=Path)
    parser.add_argument('--verification', required=True, type=Path,
                        help='Original sole-owner verification record; hash-bound, never inferred')
    parser.add_argument('--output', required=True)
    args = parser.parse_args(argv)
    try:
        folder = evidence_folder(args.output)
    except (OSError, ValueError):
        print('Invalid private evidence destination.', file=sys.stderr)
        return 2
    report = {'schema_version': 1, 'audit_passed': False,
              'logical_owner_recovery_observed': False, 'bench_qualified': False,
              'binding': 'collector_attributed_not_independently_verified',
              'phone_acceptance': None, 'usable_content': content_evidence(False),
              'power_on_ns': None, 'soft_reboot_ns': None, 'boot_gate_ready_ns': None,
              'first_native_buffer_ns': None, 'first_native_present_ns': None,
              'optical_first_frame_s': None, 'failure': None,
              'planned_restarts': None, 'observed_planned_replacements': None,
              'automatic_restart_deltas': dict.fromkeys(OWNERS)}
    old_handlers = {}

    def stop(number, _frame):
        raise Interrupted(number)

    result = 1
    copied = set()
    try:
        for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            old_handlers[number] = signal.signal(number, stop)
        deadline = time.monotonic() + 10
        verification_hash = retain(args.verification, folder / 'verification-original.json', MAX_VERIFICATION, deadline)
        copied.add('verification-original.json')
        receipt_hash = retain(args.receipts, folder / 'receipts-original.jsonl', MAX_ARCHIVE, deadline)
        copied.add('receipts-original.jsonl')
        # Audit the retained original bytes, not a path which a collector can replace.
        observed = assess(folder / 'receipts-original.jsonl', verification_hash, deadline)
        require(observed['receipts_sha256'] == receipt_hash, 'retained_input_changed')
        report.update(observed)
        report.update(audit_passed=True, usable_content=content_evidence(True),
                      verification_sha256=verification_hash)
        result = 0
    except Interrupted as error:
        report['failure'] = 'interrupted'
        report['signal'] = error.number
        result = 128 + error.number
    except Rejected as error:
        report['failure'] = str(error)
    except (OSError, ValueError, TypeError, RecursionError):
        report['failure'] = 'invalid_or_unavailable_evidence'
    finally:
        # The tool has no child/remote process to clean up. Keep failure receipts
        # before restoring handlers, including actual HUP/TERM/INT interruptions.
        for number in old_handlers:
            signal.signal(number, signal.SIG_IGN)
        try:
            report['retained_files'] = {}
            for name in ('receipts-original.jsonl', 'verification-original.json'):
                path = folder / name
                if path.exists():
                    raw = path.read_bytes()
                    report['retained_files'][name] = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(),
                                                    'copy_complete': name in copied}
            report['tool_sha256'] = {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                                     for name in ('native_fault_recovery.py', 'reliability_probe.py',
                                                  'daily_use_reliability.py', 'recovery_content_evidence.py')}
            with (folder / 'summary.json').open('x') as output:
                os.chmod(folder / 'summary.json', 0o600)
                output.write(json.dumps(report, indent=2, allow_nan=False) + '\n')
                output.flush()
                os.fsync(output.fileno())
        finally:
            for number, handler in old_handlers.items():
                signal.signal(number, handler)
    print('Offline owner audit: ' + ('PASS' if result == 0 else 'FAIL') + '; visibility/phone acceptance unknown.')
    return result


if __name__ == '__main__':
    sys.exit(main())
