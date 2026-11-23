#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual-GPU Standard handoff probe in two exclusively owned test compositors.

Usage: standard_surface_handoff_test.py WESTON PROBE FFMPEG
All process groups, sockets and generated samples belong to this invocation.
No appliance compositor, service, profile or private phone capture is used.
"""
from dataclasses import dataclass
import os
from pathlib import Path
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

from generated_h264_fixture import FFMPEG_ARGUMENTS, MAXIMUM_ENCODED_BYTES, packets

_interruption_requested = False


def check_interrupted():
    if _interruption_requested:
        raise InterruptedError('owned Standard handoff test interrupted')


@dataclass(frozen=True)
class Limits:
    encode: float = 15
    startup: float = 10
    probe: float = 56
    terminate: float = 2
    reap: float = 2

    def validate(self):
        if any(not 0 < value < 58 for value in (self.encode, self.startup, self.probe, self.terminate, self.reap)):
            raise ValueError('every owned child deadline must be positive and below 58 seconds')


def observed_status(process):
    # Do not reap during observation. Keeping the exited leader's PID reserved
    # makes killpg safe even when that tool left descendants in its own group.
    result = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
    if result is None:
        return None
    return result.si_status if result.si_code == os.CLD_EXITED else -result.si_status


def wait_owned(process, timeout, label, output_bound=None):
    deadline = time.monotonic() + timeout
    while True:
        check_interrupted()
        if output_bound and output_bound[0].stat().st_size > output_bound[1]:
            raise RuntimeError(label + ' output exceeded its bound during execution')
        status = observed_status(process)
        if status is not None:
            return status
        if time.monotonic() >= deadline:
            raise TimeoutError(label + ' exceeded its bounded test deadline')
        time.sleep(0.01)


def signal_group(process, requested):
    try:
        os.killpg(process.pid, requested)
    except ProcessLookupError:
        pass  # No living members; its unreaped leader still reserves this PID.
    except PermissionError:
        # Darwin denies signals to a group containing only unreaped zombies.
        # A terminal leader alone cannot prove this: descendants may be alive.
        # Inspect just this reserved group and reject every uncertain/live case.
        if sys.platform != 'darwin' or observed_status(process) is None:
            raise
        result = subprocess.run(['/bin/ps', '-o', 'pid=,pgid=,stat=,uid=', '-g', str(process.pid)],
                                capture_output=True, text=True, timeout=1)
        rows = [line.split() for line in result.stdout.splitlines()]
        valid = result.returncode == 0 and len(result.stdout) < 65536 and bool(rows)
        leader = False
        for row in rows:
            if len(row) != 4:
                valid = False; break
            try:
                pid, group, uid = int(row[0]), int(row[1]), int(row[3])
            except ValueError:
                valid = False; break
            if group != process.pid or uid != os.getuid() or not row[2].startswith('Z'):
                valid = False; break
            leader = leader or pid == process.pid
        if not valid or not leader:
            raise


def close_owned(processes, limits):
    # Signal every group before reaping any leader, including completed tools.
    # No lookup of other sessions or shutdown of an appliance occurs here.
    errors = []
    for process in reversed(processes):
        try:
            signal_group(process, signal.SIGTERM)
        except OSError as error:
            errors.append(RuntimeError(f'TERM group {process.pid} ({process.args[0]}): {error}'))
    deadline = time.monotonic() + limits.terminate
    while time.monotonic() < deadline:
        if all(observed_status(process) is not None for process in processes):
            break
        time.sleep(0.01)
    for process in reversed(processes):
        try:
            signal_group(process, signal.SIGKILL)
        except OSError as error:
            errors.append(RuntimeError(f'KILL group {process.pid} ({process.args[0]}): {error}'))
    for process in reversed(processes):
        try:
            process.wait(timeout=limits.reap)
        except (OSError, subprocess.TimeoutExpired) as error:
            errors.append(error)
    if errors:
        raise RuntimeError('owned test process cleanup did not complete: ' + str(errors[0])) from errors[0]


def launch(command, environment, stdout, stderr, processes):
    process = subprocess.Popen(command, env=environment, stdin=subprocess.DEVNULL,
                               stdout=stdout, stderr=stderr, start_new_session=True)
    processes.append(process)
    # Signal handlers only set a flag. In particular they cannot interrupt
    # Popen after fork but before the owned handle has entered this list.
    check_interrupted()
    return process


def socket_ready(path, process, parent_identity):
    if observed_status(process) is not None:
        raise RuntimeError('owned test compositor exited before or during socket qualification')
    parent = path.parent.lstat()
    if (not stat.S_ISDIR(parent.st_mode) or parent.st_uid != os.getuid() or stat.S_IMODE(parent.st_mode) != 0o700 or
            (parent.st_dev, parent.st_ino) != parent_identity):
        raise RuntimeError('owned compositor runtime identity or permissions changed')
    try:
        before = path.lstat()
    except FileNotFoundError:
        return False
    if not stat.S_ISSOCK(before.st_mode) or before.st_uid != os.getuid():
        raise RuntimeError('test Wayland endpoint must be a real owned socket')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(0.1)
        try:
            connection.connect(str(path))
        except (ConnectionRefusedError, FileNotFoundError, socket.timeout):
            return False
        if hasattr(socket, 'SO_PEERCRED'):
            pid, uid, _ = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if pid != process.pid or uid != os.getuid():
                raise RuntimeError('Wayland socket peer is not the owned compositor process')
    after = path.lstat()
    if (after.st_dev, after.st_ino) != (before.st_dev, before.st_ino) or not stat.S_ISSOCK(after.st_mode):
        raise RuntimeError('test Wayland socket changed during qualification')
    if observed_status(process) is not None:
        raise RuntimeError('owned test compositor exited during socket qualification')
    return True


def tail(path, maximum=8192):
    try:
        with path.open('rb') as content:
            content.seek(0, os.SEEK_END)
            content.seek(max(0, content.tell() - maximum))
            return content.read(maximum).decode(errors='replace')
    except FileNotFoundError:
        return ''


def run(weston, probe, ffmpeg, limits=Limits()):
    limits.validate()
    # /tmp keeps both absolute AF_UNIX paths below macOS's 104-byte limit even
    # when an inherited TMPDIR points deep into a desktop application's cache.
    with tempfile.TemporaryDirectory(prefix='headunit-standard-handoff-', dir='/tmp') as folder:
        runtime = Path(folder)
        runtime.chmod(0o700)
        initial = runtime.lstat()
        parent_identity = (initial.st_dev, initial.st_ino)
        environment = dict(os.environ, XDG_RUNTIME_DIR=folder, WAYLAND_DISPLAY='standard-main')
        names = ('standard-main', 'standard-cluster')
        processes, logs = [], []
        status = 1
        try:
            encoded, encoder_log = runtime / 'generated.h264', runtime / 'encoder.log'
            logs.append(encoder_log)
            with encoded.open('xb') as output, encoder_log.open('xb') as error:
                encoded.chmod(0o600); encoder_log.chmod(0o600)
                encoder = launch([sys.executable, '-B', str(Path(__file__).resolve()), '--bounded-encoder',
                                  ffmpeg, *FFMPEG_ARGUMENTS], environment, output, error, processes)
                if wait_owned(encoder, limits.encode, 'generated H.264 encoder', (encoded, MAXIMUM_ENCODED_BYTES)) != 0:
                    raise RuntimeError('generated H.264 encoder failed')
            if encoded.stat().st_size > MAXIMUM_ENCODED_BYTES:
                raise RuntimeError('generated H.264 encoder output exceeded its bound')
            fixture = runtime / 'synthetic.packets'
            fixture.write_bytes(packets(encoded.read_bytes())); fixture.chmod(0o600)
            compositors = []
            for name in names:
                log = runtime / (name + '.log'); logs.append(log)
                compositors.append(launch([
                    weston, '--backend=headless', '--renderer=gl', '--width=640', '--height=360',
                    '--idle-time=0', '--no-config', '--socket=' + name, '--log=' + str(log),
                    '--shell=desktop-shell.so',
                ], environment, subprocess.DEVNULL, subprocess.DEVNULL, processes))
            deadline = time.monotonic() + limits.startup
            while True:
                check_interrupted()
                readiness = [socket_ready(runtime / name, process, parent_identity)
                             for name, process in zip(names, compositors)]
                if all(readiness):
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError('owned headless GL compositors did not start')
                time.sleep(0.02)
            output_path, error_path = runtime / 'probe.log', runtime / 'probe.err'
            logs.extend((output_path, error_path))
            with output_path.open('xb') as output, error_path.open('xb') as error:
                output_path.chmod(0o600); error_path.chmod(0o600)
                child = launch([probe, str(fixture), *(str(runtime / name) for name in names)],
                               environment, output, error, processes)
                status = wait_owned(child, limits.probe, 'actual-GPU Standard handoff probe')
            if any(observed_status(process) is not None for process in compositors):
                raise RuntimeError('owned test compositor exited during the handoff probe')
            if status == 0:
                print(tail(output_path).strip())
            check_interrupted()
            return status if status >= 0 else 1
        finally:
            try:
                close_owned(processes, limits)
            finally:
                if status:
                    for log in logs:
                        text = tail(log)
                        if text:
                            print(text, file=sys.stderr)
            check_interrupted()


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--bounded-encoder':
        # Kernel enforcement also bounds a writer that exceeds its limit in
        # less than the parent owner's observation interval. Exec keeps PID
        # and group identity; no preexec callback or additional child exists.
        import resource
        resource.setrlimit(resource.RLIMIT_FSIZE, (MAXIMUM_ENCODED_BYTES, MAXIMUM_ENCODED_BYTES))
        os.execvp(sys.argv[2], sys.argv[2:])
    if len(sys.argv) != 4:
        raise ValueError('usage: standard_surface_handoff_test.py WESTON PROBE FFMPEG')
    def interrupted(_number, _frame):
        global _interruption_requested
        _interruption_requested = True
        for requested in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(requested, signal.SIG_IGN)
    for requested in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(requested, interrupted)
    return run(*sys.argv[1:])


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
