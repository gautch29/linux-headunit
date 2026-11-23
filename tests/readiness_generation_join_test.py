# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Fresh manager admission with a surviving dated source; no phone/GPU receipt.

The real managers, endpoint locks, native HVO/IPC owners and C++ status publisher
run. BlueZ/validation and the gated receiver executable are explicit fixtures.
The existing publisher's `r` command supplies synthetic renderer eligibility.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import select
import shutil
import subprocess
import sys
import time
import traceback
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from carplay_readiness import MAXIMUM_AGE_NS, clock_ns
from phone_manager import Manager, RETIREMENT_REQUIRED
from phone_manager_endpoint import ManagerEndpoint
from prephone_native import NativePublication

PHONE = 'AA:BB:CC:DD:EE:01'
# This no-descendant launcher generates no status or timestamp. Exec retains
# the actual PID and inherited endpoint lock; stdin subsequently reaches C++.
GATE = ('import os, sys\n'
        'if sys.stdin.buffer.read(1) != b"!":\n'
        '    raise SystemExit(2)\n'
        'os.execv(sys.argv[1], sys.argv[1:])\n')


def require(value, reason):
    if not value:
        raise AssertionError(reason)


def save(path, value):
    with path.open('x') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())


class Phones:
    def phones(self):
        return [{'address': PHONE, 'name': 'Explicit software fixture', 'connected': False}]

    def phone(self, phone):
        require(phone == PHONE, 'Unexpected fixture phone')
        return self.phones()[0]

    def disconnect(self, phone):
        self.phone(phone)

    def media(self, phone):
        self.phone(phone)
        return {'available': False}


def run(native_binary, publisher_binary):
    runtime = Path('/tmp') / ('hu-rg-' + secrets.token_hex(3))
    runtime.mkdir(mode=0o700)
    reports = os.environ.get('HEADUNIT_READINESS_JOIN_REPORT_DIR')
    report_dir = Path(reports) if reports else None
    if report_dir:
        save(report_dir / 'fixture.json', {'kind': 'software-only', 'runtime': str(runtime)})
        (report_dir / 'original-runtime.txt').write_text(str(runtime) + '\n')
    trace = []
    children = []
    streams = []
    manager = endpoint = native = None
    failure = None
    owner = os.getuid(), os.getgid()
    display = runtime / 'standard.conf'
    display.write_text('Explicit dry-run display-validator substitute\n')
    config = runtime / 'receiver.json'
    config.write_text(json.dumps({'pair_store': str(runtime / 'pairs'),
                                  'display_config': str(display)}))
    command = [str(native_binary), str(runtime / 'prephone.sock'), str(runtime / 'vehicle.sock')]
    actual_popen = subprocess.Popen

    def event(name, **fields):
        trace.append({'event': name, **fields})
        with (runtime / 'journey.log').open('a') as stream:
            stream.write(json.dumps(trace[-1], allow_nan=False) + '\n')

    def spawn(_command, **options):
        derived = json.loads((runtime / 'managed-receiver.json').read_text())
        stderr = (runtime / f'child-{len(children) + 1}.stderr').open('wb')
        streams.append(stderr)
        child = actual_popen([sys.executable, '-B', '-c', GATE, str(publisher_binary),
                              '--interop', str(runtime), derived['runtime_status_nonce']],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                             pass_fds=options.get('pass_fds', ()))
        children.append(child)
        event('receiver-created-gated', pid=child.pid, nonce=derived['runtime_status_nonce'])
        return child

    def line(child, expected, filename):
        require(select.select([child.stdout], [], [], 3)[0], 'Fixture response missing')
        raw = child.stdout.readline()
        with (runtime / filename).open('ab') as stream:
            stream.write(raw)
        require(raw.decode().strip() == expected, 'Unexpected fixture response')

    def receiver_command(child, value, expected, index):
        child.stdin.write(value.encode())
        child.stdin.flush()
        line(child, expected, f'child-{index}.log')

    def status(name):
        value = manager.request({'action': 'status'})
        event(name, status=value)
        return value

    def wait_native(predicate):
        limit = time.monotonic() + 3
        while True:
            value = manager.request({'action': 'status'})['prephone_vehicle']
            if predicate(value):
                return value
            require(native.poll() is None, 'Native fixture exited during admission')
            require(time.monotonic() < limit, 'Native admission condition missing')
            time.sleep(.01)

    def open_manager():
        current_endpoint = ManagerEndpoint(runtime, owner)
        try:
            current = Manager(config, runtime, display, owner, bluez=Phones(),
                              receiver_lock_fd=current_endpoint.inherited_lock_fd)
            current.prephone = NativePublication(runtime / 'prephone.sock', owner, command)
        except BaseException:
            current_endpoint.close()
            raise
        return current, current_endpoint

    try:
        # Prepare all configuration/imports/gate text before Ready A starts the
        # original 1.5s window. Never modify or replace a clock for this test.
        manager, endpoint = open_manager()
        stderr = (runtime / 'native.stderr').open('wb')
        streams.append(stderr)
        native = actual_popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr)
        wait_native(lambda value: value['publisher']['live'])
        native.stdin.write(b'f')
        native.stdin.flush()
        require(select.select([native.stdout], [], [], 3)[0], 'Original HVO acquisition missing')
        original_acquisition = native.stdout.readline().decode().strip()
        original = wait_native(lambda value: value['rows'][0]['availability'] == 'available')
        require(original['rows'][0]['value'] == 0, 'Original zero was unavailable')
        require(original['rows'][3]['value'] == -5, 'Original negative value lost')
        old_ipc_nonce = manager.prephone.nonce
        event('original-native', model=original, source_line=original_acquisition, pid=native.pid)
        with patch('phone_manager.subprocess.run') as validation, \
                patch('phone_manager.subprocess.Popen', side_effect=spawn):
            manager.start(PHONE)
            first = manager.child
            receiver_command(first, '!', 'initial', 1)
            receiver_command(first, 'r', 'r', 1)
            require(status('ready-a')['carplay']['returnReady'], 'Fixture A did not qualify wire status')
            old_ready = manager.carplay.path.read_bytes()
            (runtime / 'original-ready.json').write_bytes(old_ready)
            old_wire = json.loads(old_ready)
            first.kill()
            require(first.wait(timeout=3) < 0, 'Fixture death did not reap')
            require(not status('dead-before-tick')['carplay']['returnReady'],
                    'Dead receiver retained readiness before manager tick')
            require(manager.retirement_required, 'Signal death lost existing retirement guard')
            try:
                manager.require_launcher_retired()
            except RuntimeError as error:
                require(str(error) == RETIREMENT_REQUIRED, 'Retirement policy changed')
            else:
                raise AssertionError('Old manager admitted a replacement without retirement')
            old_publication = manager.prephone
            manager.shutdown()
            endpoint.close()
            manager = endpoint = None
            require(native.poll() is None, 'Independent native source died with manager')
            require(old_publication.connection is None, 'Old native IPC connection survived shutdown')
            manager, endpoint = open_manager()
            blank = manager.prephone.model.snapshot(time.monotonic_ns())
            require(blank['sourceGeneration'] is None, 'Fresh consumer inherited source identity')
            require(all(row['value'] is None for row in blank['rows']), 'Fresh consumer inherited values')
            manager.start(PHONE)
            second = manager.child
            require(second.poll() is None, 'Replacement gated launcher is not live')
            require(manager.carplay.nonce != old_wire['nonce'], 'Replacement reused receiver nonce')
            require(manager.carplay.path.read_bytes() == old_ready, 'Original Ready bytes were replaced')
            before = clock_ns()
            foreign = status('fresh-manager-old-ready')
            after = clock_ns()
            ages = [instant - int(old_wire['updatedMonotonicNs']) for instant in (before, after)]
            event('original-ready-age-guard', before_after_age_ns=ages, maximum_ns=MAXIMUM_AGE_NS)
            require(all(0 <= age <= MAXIMUM_AGE_NS for age in ages),
                    'INCONCLUSIVE: missed original 1.5s guard; stop without retry or clock/date changes')
            require(not foreign['carplay']['returnReady'], 'Old launch authorized fresh manager')
            require(manager.carplay.last is None, 'Foreign Ready was accepted as current history')
            rejoined = wait_native(lambda value: value['publisher']['live'] and
                                   value['sourceGeneration'] == original['sourceGeneration'])
            observed = manager.prephone.last_now
            require(native.poll() is None, 'Surviving native owner exited')
            require(manager.prephone.peer[0] == native.pid, 'Native source process changed')
            require(manager.prephone.nonce != old_ipc_nonce, 'Native IPC challenge was reused')
            for key in ('sourceGeneration', 'sourceStartedNs', 'sourceSampledNs', 'clockDomain'):
                require(rejoined[key] == original[key], 'Original source identity/date changed: ' + key)
            require(int(rejoined['publisher']['copiedNs']) > int(original['publisher']['copiedNs']),
                    'Rejoined model did not come from a new actual native copy')
            for old, current in zip(original['rows'], rejoined['rows'], strict=True):
                for key in ('id', 'acquiredNs', 'maximumAgeNs', 'origin'):
                    require(current[key] == old[key], 'Original row metadata changed: ' + key)
                if old['acquiredNs'] is None:
                    require(current['availability'] == 'missing' and current['value'] is None,
                            'Missing original row became a reading')
                else:
                    expired = observed - int(old['acquiredNs']) >= int(old['maximumAgeNs'])
                    require(current['availability'] == ('stale' if expired else 'available'),
                            'Row freshness did not use original acquisition')
                    require(current['value'] == (None if expired else old['value']),
                            'Copy refreshed/replaced an original value')
            event('same-source-rejoined', model=rejoined, observed_ns=observed, native_pid=native.pid)
            require(not status('native-cannot-authorize-phone')['carplay']['returnReady'],
                    'Native re-admission authorized an unpublished receiver')
            receiver_command(second, '!', 'initial', 2)
            require(not status('replacement-initial')['carplay']['returnReady'],
                    'New publisher initial state inherited Ready')
            require(manager.carplay.last['nonce'] == manager.carplay.nonce, 'New initial publication rejected')
            receiver_command(second, 'r', 'r', 2)
            final = status('replacement-synthetic-ready')
            require(final['carplay']['returnReady'], 'New owned fixture publication did not qualify wire status')
            require(final['active_mode'] is None, 'Synthetic fixture invented negotiated phone mode')
            require(manager.carplay.last['connectionGeneration'] == old_wire['connectionGeneration'],
                    'Expected equal numeric generations in distinct receiver launches')
            require(validation.call_count == 2, 'Unexpected launch/preflight count')
            receiver_command(second, 's', 's', 2)
            require(second.wait(timeout=3) == 0, 'Replacement publisher did not retire normally')
    except BaseException:
        failure = traceback.format_exc()
        raise
    finally:
        # Evidence stays outside the build. The reviewed lane owner copies and
        # verifies both complete report trees before retiring its container.
        cleanup_errors = []
        if manager:
            try:
                manager.shutdown()
            except BaseException:
                cleanup_errors.append(traceback.format_exc())
        if endpoint:
            try:
                endpoint.close()
            except BaseException:
                cleanup_errors.append(traceback.format_exc())
        for child in [*children, *([native] if native else [])]:
            try:
                if child.poll() is None:
                    child.terminate()
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                cleanup_errors.append('Fixture required forced retirement: ' + str(child.pid))
                child.kill()
                child.wait(timeout=3)
            for stream in (child.stdin, child.stdout):
                stream.close()
        for stream in streams:
            stream.close()
        save(runtime / 'result.json', {'passed': failure is None and not cleanup_errors,
                                       'failure': failure, 'cleanup_errors': cleanup_errors,
                                       'children': [{'pid': child.pid, 'returncode': child.returncode}
                                                    for child in children],
                                       'native_pid': native.pid if native else None,
                                       'native_returncode': native.returncode if native else None,
                                       'renderer_readiness': 'explicit synthetic interop r input',
                                       'phone_gpu_systemd_physical_evidence': False})
        save(runtime / 'trace.json', trace)
        if report_dir:
            for index in (1, 2):
                shutil.copytree(runtime, report_dir / f'runtime-{index}')
        print('Original software join evidence retained:', runtime, flush=True)
        require(not cleanup_errors, 'Fixture cleanup failed; original failure/evidence retained')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native_fixture', type=Path)
    parser.add_argument('publisher_fixture', type=Path)
    arguments = parser.parse_args()
    run(arguments.native_fixture.resolve(strict=True), arguments.publisher_fixture.resolve(strict=True))
    print('Fresh manager/native original-date and receiver-launch admission passed; synthetic Ready only.')
