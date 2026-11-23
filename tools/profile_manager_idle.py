#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Finite local Manager workload; never accepts a bench PID/config/socket.

Actual Manager/ManagerEndpoint/SportSource, explicit fake BlueZ and no display,
receiver, pairing, signed assets or hardware. Optional existing Linux C++ native
fixture exercises real source/publication admission, not sealed startup admission.
CPU, cProfile and syscall modes are separate runs, never causal bench evidence.
"""
import argparse
import cProfile
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
TICK_SECONDS = 1
STATUS_SECONDS = 2


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def command(path, value):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(3)
        client.connect(str(path))
        client.sendall(json.dumps(value).encode() + b'\n')
        raw = bytearray()
        while not raw.endswith(b'\n'):
            chunk = client.recv(65536)
            if not chunk or len(raw) + len(chunk) > 1_500_000:
                raise ValueError('Incomplete/oversized fixture reply')
            raw.extend(chunk)
        return json.loads(raw)


class FakeBluez:
    """Explicit empty phone inventory; no real BlueZ import/operation invoked."""
    def phones(self):
        return []

    def media(self, phone):
        raise AssertionError('Disconnected workload attempted a phone operation')


def source_identity():
    files = set((ROOT / 'tools').glob('*.py'))
    files.add(Path(__file__).resolve())
    for module in tuple(sys.modules.values()):
        name = getattr(module, '__file__', None)
        if name:
            path = Path(name).resolve()
            if path.is_relative_to(ROOT) and path.suffix == '.py':
                files.add(path)
    return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(files)}


def proc_child(pid):
    """Only called with the actual Popen child owned by this fixture."""
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return {'observedMonotonicNs': time.monotonic_ns(),
            'ticksPerSecond': os.sysconf('SC_CLK_TCK'),
            'userTicks': int(fields[11]), 'systemTicks': int(fields[12])}


def proc_stat(path):
    """Original numeric /proc observation; never reads another process."""
    raw = path.read_text()
    identifier = int(raw.split(' ', 1)[0])
    fields = raw.rsplit(')', 1)[1].split()
    return {'nativeId': identifier, 'startTicks': int(fields[19]),
            'userTicks': int(fields[11]), 'systemTicks': int(fields[12]),
            'observedMonotonicNs': time.monotonic_ns()}


def kernel_identity(native_id):
    if native_id is None:
        return None
    try:
        process = proc_stat(Path('/proc/self/stat'))
        task = proc_stat(Path(f'/proc/self/task/{native_id}/stat'))
        if process['nativeId'] != os.getpid() or task['nativeId'] != native_id:
            return None
        return {'processId': process['nativeId'], 'processStartTicks': process['startTicks'],
                'nativeId': task['nativeId'], 'threadStartTicks': task['startTicks'],
                'processObservedMonotonicNs': process['observedMonotonicNs'],
                'observedMonotonicNs': task['observedMonotonicNs']}
    except (OSError, ValueError, IndexError):
        return None


def owner_observation(thread):
    """Copies only an explicitly supplied owned Thread, never a name/CPU guess.

    Callers retain the two actual objects through retirement, preventing object-ID
    reuse. Python retains native_id after death; that historical property is not
    current task identity. Earlier observations keep their original dates/IDs.
    """
    alive = thread.is_alive() if thread is not None else None
    native_id = getattr(thread, 'native_id', None) if alive else None
    if alive and thread is threading.current_thread():
        get_native_id = getattr(threading, 'get_native_id', None)
        if get_native_id is None or get_native_id() != native_id:
            native_id = None
    kernel = kernel_identity(native_id)
    # A death during /proc I/O cannot create a live owner join.
    if alive and not thread.is_alive():
        alive = False
        native_id = kernel = None
    return {'observedMonotonicNs': time.monotonic_ns(),
            'objectId': id(thread) if thread is not None else None,
            'alive': alive, 'nativeId': native_id, 'kernelIdentity': kernel}


def proc_threads():
    """Kernel cumulative counters only for this fixture process, when available."""
    folder = Path('/proc/self/task')
    if not folder.is_dir():
        return None
    rows = {}
    try:
        process = proc_stat(Path('/proc/self/stat'))
        for path in folder.iterdir():
            try:
                rows[path.name] = proc_stat(path / 'stat')
            except (OSError, ValueError, IndexError):
                continue
    except (OSError, ValueError, IndexError):
        return None
    return {'observedMonotonicNs': time.monotonic_ns(),
            'processId': process['nativeId'], 'processStartTicks': process['startTicks'],
            'ticksPerSecond': os.sysconf('SC_CLK_TCK'), 'threads': rows}


def joined_cpu(owner, counters):
    """Independent CPU row only with same-process and original task-start proof."""
    if owner is None:
        return None
    identity = owner['kernelIdentity']
    if not owner['alive'] or owner['nativeId'] is None or identity is None or counters is None:
        return None
    row = counters['threads'].get(str(owner['nativeId']))
    if (row is None or identity['processId'] != os.getpid() or
            counters['processId'] != identity['processId'] or
            counters['processStartTicks'] != identity['processStartTicks'] or
            row['nativeId'] != owner['nativeId'] or identity['nativeId'] != owner['nativeId'] or
            row['startTicks'] != identity['threadStartTicks'] or
            row['observedMonotonicNs'] < owner['observedMonotonicNs']):
        return None
    return copy.deepcopy(row)


def same_owner(before, after):
    return (before is not None and after is not None and
            before['alive'] is True and after['alive'] is True and
            before['objectId'] is not None and before['objectId'] == after['objectId'] and
            before['nativeId'] is not None and before['nativeId'] == after['nativeId'])


def owner_mapping(before, after, retired, counters_before, counters_after):
    first = joined_cpu(before, counters_before)
    last = joined_cpu(after, counters_after)
    interval = None
    if (same_owner(before, after) and first is not None and last is not None and
            first['startTicks'] == last['startTicks'] and
            before['kernelIdentity']['processStartTicks'] == after['kernelIdentity']['processStartTicks'] and
            counters_before['ticksPerSecond'] == counters_after['ticksPerSecond'] and
            last['observedMonotonicNs'] > first['observedMonotonicNs'] and
            last['userTicks'] >= first['userTicks'] and last['systemTicks'] >= first['systemTicks']):
        interval = {'scope': 'independent same-process kernel task observation interval; excludes retirement',
                    'ticksPerSecond': counters_before['ticksPerSecond'],
                    'before': first, 'after': last,
                    'userTicks': last['userTicks'] - first['userTicks'],
                    'systemTicks': last['systemTicks'] - first['systemTicks']}
    return copy.deepcopy({'before': before, 'after': after, 'afterShutdown': retired,
                          'kernelCpuBefore': first, 'kernelCpuAfter': last,
                          'kernelCpuInterval': interval})


def worker(folder, output, workload, mode, native_fixture):
    from phone_manager import Manager
    from phone_manager_endpoint import ManagerEndpoint
    from sport_source import SportSource

    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)
    manager = endpoint = native = None
    started_wall = started_cpu = started_thread = None
    before_threads = after_threads = None
    # Fixed two known owners. Retain these objects until the report is written;
    # neither enumerate Python threads nor assign a role to unknown kernel tasks.
    owners = {'main': threading.current_thread(), 'sport': None}
    owners_before = owners_after = {'main': None, 'sport': None}
    sport_cpu = {}
    original_run = SportSource._run

    def measured_sport(source):
        actual_thread = threading.current_thread()
        original_identity = owner_observation(actual_thread)
        started = time.monotonic_ns()
        begin = time.thread_time_ns()
        try:
            original_run(source)
        finally:
            cpu = time.thread_time_ns() - begin
            ended = time.monotonic_ns()
            sport_cpu.update(threadCpuNs=cpu, observedStartNs=started, observedEndNs=ended,
                             ownerBefore=original_identity, ownerAfter=owner_observation(actual_thread),
                             actualSourceThread=source.thread is actual_thread,
                             scope='existing SportSource worker lifetime including retirement')

    # Instrument only this owned fixture's existing worker. No runtime source edit
    # or additional measurement thread; unprofiled mode uses independent OS clocks.
    SportSource._run = measured_sport
    main_profile = cProfile.Profile() if mode == 'profile' else None
    ticks = served = 0
    native_retired = None
    report = {'scope': 'local disconnected source fixture, not V87 CPU reproduction',
              'workload': workload, 'mode': mode, 'completed': False, 'pid': os.getpid(),
              'profileScope': 'aggregate callbacks; no thread isolation or CPU attribution'
                              if mode == 'profile' else None,
              'platform': platform.platform(), 'python': platform.python_version(),
              'cadence': {'tickSeconds': TICK_SECONDS, 'statusSeconds': STATUS_SECONDS},
              'boundaries': ['fake empty BlueZ', 'no receiver/display/pairing/hardware',
                             'unsealed constructor; sealed input admission not measured'],
              'nativeFixtureSha256': hashlib.sha256(native_fixture.read_bytes()).hexdigest()
                                     if native_fixture else None}
    try:
        display = folder / 'display.conf'
        display.write_text('explicit unused display boundary\n')
        config = folder / 'receiver.json'
        settings = {'display_config': str(display), 'pair_store': str(folder / 'pairs')}
        if workload == 'configured':
            settings.update(sport_telemetry=True, simulate_vehicle=True, audio_sink='fakesink',
                            cluster_page=str(folder / 'cluster-page'),
                            greeting_sequence_file=str(folder / 'greeting-sequence'))
        write_json(config, settings)
        config.chmod(0o600)
        endpoint = ManagerEndpoint(folder)
        manager = Manager(config, folder, display, (os.getuid(), os.getgid()),
                          bluez=FakeBluez(), receiver_lock_fd=endpoint.inherited_lock_fd)
        owners['sport'] = manager.sport.thread if manager.sport else None
        if native_fixture:
            from prephone_native import NativePublication
            argv = [str(native_fixture), str(folder / 'prephone.sock'), str(folder / 'vehicle.sock')]
            manager.prephone = NativePublication(folder / 'prephone.sock', manager.owner, argv)
            native = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.PIPE)
            # First-party cache/source/projection/encoder; only acquisitions are
            # explicit labelled fixture input. Preserve real original source dates.
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and manager.prephone.connection is None and not stopping:
                manager.prephone.poll()
                time.sleep(.01)
            if manager.prephone.connection is None:
                raise ValueError('Actual native fixture admission failed')
            native.stdin.write(b'f')
            native.stdin.flush()
        write_json(output / 'ready.json', {'pid': os.getpid(), 'nativePid': native.pid if native else None})
        # Parent starts the bounded interval after actual socket/worker readiness.
        while not (folder / 'begin').exists() and not stopping:
            time.sleep(.01)
        report['sourceSha256Before'] = source_identity()
        report['nativeProcBefore'] = proc_child(native.pid) if native else None
        owners_before = {role: owner_observation(thread) for role, thread in owners.items()}
        started_wall = time.monotonic_ns()
        started_cpu = time.process_time_ns()
        started_thread = time.thread_time_ns()
        before_threads = proc_threads()
        if main_profile:
            main_profile.enable()
        next_tick = 0
        # Scheduling matches main's six-line loop; business methods/IPC are actual.
        # This fixture does not execute the privileged appliance CLI/preflight.
        while not stopping and not (folder / 'end').exists():
            served += endpoint.serve_once(manager.request)
            if time.monotonic() >= next_tick:
                manager.tick()
                ticks += 1
                if native:
                    if native.poll() is not None:
                        raise ValueError('Native source fixture exited during workload')
                    native.stdin.write(b'f')
                    native.stdin.flush()
                next_tick = time.monotonic() + TICK_SECONDS
        if main_profile:
            main_profile.disable()
        ended_wall = time.monotonic_ns()
        ended_cpu = time.process_time_ns()
        ended_thread = time.thread_time_ns()
        owners_after = {role: owner_observation(thread) for role, thread in owners.items()}
        after_threads = proc_threads()
        report['nativeProcAfter'] = proc_child(native.pid) if native and native.poll() is None else None
        wall = ended_wall - started_wall
        report.update(wallNs=wall, processCpuNs=ended_cpu - started_cpu,
                      mainThreadCpuNs=ended_thread - started_thread,
                      processCpuPercentOneCore=100 * (ended_cpu - started_cpu) / wall,
                      mainThreadCpuPercentOneCore=100 * (ended_thread - started_thread) / wall,
                      observedStartNs=started_wall, observedEndNs=ended_wall,
                      ticks=ticks, served=served, procThreadsBefore=before_threads,
                      procThreadsAfter=after_threads)
        if native:
            state = manager.prephone.snapshot()
            report['native'] = {'live': state['publisher']['live'],
                                'sequence': state['publisher']['sequence'],
                                'state': state['state'], 'sourceGeneration': state['sourceGeneration'],
                                'sourceSampledNs': state['sourceSampledNs'],
                                'copiedNs': state['publisher']['copiedNs']}
        report['completed'] = not stopping
        report['interrupted'] = stopping
    finally:
        # Cleanup stays inside the owning process and retains the socket lock until
        # the genuine source is waited. Repeated signals set a flag only.
        if main_profile:
            main_profile.disable()
        if native:
            if native.poll() is None:
                native.terminate()
            try:
                native.wait(timeout=3)
            except subprocess.TimeoutExpired:
                native.kill()
                native.wait(timeout=3)
            native.stdin.close()
            native.stderr.close()
            if manager and manager.prephone:
                retired = manager.prephone.snapshot()
                native_retired = not retired['publisher']['live'] and all(
                    row['value'] is None for row in retired['rows'])
        if manager:
            manager.shutdown()
        if endpoint:
            endpoint.close()
        mappings = {role: owner_mapping(owners_before[role], owners_after[role],
                                       owner_observation(thread), before_threads, after_threads)
                    for role, thread in owners.items()}
        mappings['main']['independentCpu'] = {
            'scope': 'calling main Thread CPU over the workload interval; excludes retirement',
            'cpuNs': report['mainThreadCpuNs'], 'observedStartNs': report['observedStartNs'],
            'observedEndNs': report['observedEndNs']
        } if same_owner(owners_before['main'], owners_after['main']) else None
        mappings['sport']['independentCpu'] = copy.deepcopy(sport_cpu) if (
            sport_cpu.get('actualSourceThread') and
            sport_cpu['ownerBefore']['objectId'] == id(owners['sport']) and
            same_owner(sport_cpu['ownerBefore'], sport_cpu['ownerAfter'])) else None
        report.update(interrupted=stopping, sportWorker=sport_cpu,
                      threadOwners={'scope': 'local fixture only; no equivalence to V87 TID 980',
                                    'clock': 'Python time.monotonic_ns; host profiler observations, not vehicle acquisition',
                                    'objectIdScope': 'retained Python Thread objects in this worker process only',
                                    'main': mappings['main'], 'sport': mappings['sport']},
                      retirement={'nativeWaited': native.poll() is not None if native else None,
                                  'nativeValuesWithdrawn': native_retired,
                                  'sportJoined': not manager.sport.thread.is_alive() if manager and manager.sport else None,
                                  'managerSocketAbsent': not (folder / 'manager.sock').exists(),
                                  'nativeSocketAbsent': not (folder / 'prephone.sock').exists()},
                      sourceSha256=source_identity())
        report['sourceUnchanged'] = all(report['sourceSha256'].get(name) == digest
                                        for name, digest in report.get('sourceSha256Before', {}).items())
        if main_profile:
            main_profile.dump_stats(str(output / 'workload.prof'))
        write_json(output / 'worker.json', report)
        SportSource._run = original_run


def run(args):
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    process = None
    interrupted = None

    def stop(sig, _):
        nonlocal interrupted
        interrupted = sig
        if process and process.poll() is None:
            # Only our new session; strace and its fixture/source descendants.
            os.killpg(process.pid, sig)

    previous = {sig: signal.signal(sig, stop) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    receipt = {'requestedSeconds': args.seconds, 'mode': args.mode, 'workload': args.workload,
               'statusReplies': 0, 'ownerRefused': False, 'invalidActionRefused': False}
    temporary = tempfile.TemporaryDirectory(prefix='pm-idle-', dir='/tmp')
    try:
        folder = Path(temporary.name).resolve()
        folder.chmod(0o700)
        argv = [sys.executable, '-B', str(Path(__file__).resolve()), '--worker', str(folder),
                '--output', str(output), '--workload', args.workload, '--mode', args.mode]
        if args.native_fixture:
            argv += ['--native-fixture', str(args.native_fixture.resolve())]
        if args.mode == 'syscalls':
            # Aggregate counts, never paths/payloads. Trace only our fresh child.
            argv = ['strace', '-f', '-c', '-o', str(output / 'syscalls.txt'), *argv]
        with (output / 'worker.log').open('wb') as log:
            process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic() + 10
            while not (output / 'ready.json').exists() and not interrupted:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise ValueError('Fixture startup failed; retained worker.log')
                time.sleep(.01)
            if not interrupted:
                from phone_manager_endpoint import ManagerEndpoint
                try:
                    contender = ManagerEndpoint(folder)
                except (ValueError, BlockingIOError):
                    receipt['ownerRefused'] = True
                else:
                    contender.close()
                    raise ValueError('Competing fixture owner unexpectedly admitted')
                (folder / 'begin').touch()
                receipt['invalidActionRefused'] = not command(folder / 'manager.sock', {'action': 'unknown'})['ok']
                started = time.monotonic()
                next_status = started
                while time.monotonic() - started < args.seconds and not interrupted:
                    if time.monotonic() >= next_status:
                        status = command(folder / 'manager.sock', {'action': 'status'})
                        if not status['ok'] or status['active'] is not None or status['carplay']['returnReady']:
                            raise ValueError('Disconnected fixture authority changed')
                        receipt['statusReplies'] += 1
                        next_status = time.monotonic() + STATUS_SECONDS
                    time.sleep(.01)
                receipt['clientActualSeconds'] = time.monotonic() - started
            (folder / 'end').touch()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                raise ValueError('Owned fixture failed bounded retirement')
            receipt['exitCode'] = process.returncode
            if process.returncode != 0 and not interrupted:
                raise ValueError('Fixture failed; retained worker.log/worker.json')
    except BaseException:
        if not interrupted:
            raise
    finally:
        # Preserve failed/signal evidence before TemporaryDirectory retires data.
        # No other process, build, container or input is touched.
        if process and process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=3)
        temporary.cleanup()
        receipt.update(interruptedSignal=interrupted,
                       fixtureDirectoryAbsent=not Path(temporary.name).exists(),
                       fixtureWaited=process is not None and process.poll() is not None)
        write_json(output / 'receipt.json', receipt)
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    return 128 + interrupted if interrupted else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--output', required=True, type=Path, help='new retained report directory outside build trees')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--workload', choices=('sparse', 'configured'), default='configured')
    parser.add_argument('--mode', choices=('cpu', 'profile', 'syscalls'), default='cpu')
    parser.add_argument('--native-fixture', type=Path, help='own built headunit-prephone-native-fixture (Linux only)')
    parser.add_argument('--worker', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or not 1 <= args.seconds <= 60:
        parser.error('--seconds must be finite and between 1 and 60')
    if args.native_fixture and (sys.platform != 'linux' or not args.native_fixture.resolve().is_file()):
        parser.error('--native-fixture requires a real Linux fixture executable')
    if args.mode == 'syscalls' and sys.platform != 'linux':
        parser.error('syscalls mode requires Linux strace')
    if args.worker:
        worker(args.worker, args.output, args.workload, args.mode,
               args.native_fixture.resolve() if args.native_fixture else None)
        return 0
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
