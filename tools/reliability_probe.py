# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""SSH-stdin helper for daily_use_reliability.py; never installed in a release.

Only the explicit connect/disconnect/restart operations mutate the appliance.
Phone addresses stay inside this process. No configurations or pair stores read.
"""
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import time

OWNERS = ('headunit-compositor', 'headunit-outputs', 'headunit-phone-page',
          'headunit-cluster', 'headunit-receiver')
BOUND = 1_500_000


def journal_cursor(output):
    # journalctl -n0 may put '-- No entries --' before its original cursor.
    # Retain exactly that cursor, never the whole display output or a new date.
    cursors = [line.removeprefix('-- cursor: ') for line in output.splitlines()
               if line.startswith('-- cursor: ')]
    if len(cursors) != 1 or not 0 < len(cursors[0]) <= 4096 or any(
            character.isspace() for character in cursors[0]):
        raise ValueError('Journal cursor unavailable or ambiguous')
    return cursors[0]


def request(folder, command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(44)
        client.connect(str(folder / 'manager.sock'))
        client.sendall(json.dumps(command).encode() + b'\n')
        with client.makefile('rb') as stream:
            raw = stream.readline(BOUND + 1)
    if len(raw) > BOUND or not raw.endswith(b'\n'):
        raise ValueError('Manager reply exceeds bound or is incomplete')
    value = json.loads(raw)
    if not isinstance(value, dict) or value.get('ok') is not True:
        raise ValueError('Manager rejected request')
    return value


def receipt(folder):
    """Retain original dated bytes; do not repair malformed/unavailable state."""
    path = folder / 'carplay-status.json'
    before = folder.lstat()
    if (not stat.S_ISDIR(before.st_mode) or stat.S_IMODE(before.st_mode) != 0o700 or
            before.st_uid != os.getuid() or folder.resolve() != folder):
        raise ValueError('Unsafe receipt directory')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or
                not 0 < info.st_size <= 512):
            raise ValueError('Unsafe receipt')
        raw = os.read(fd, 513)
        after = os.fstat(fd)
        named = path.lstat()
        parent = folder.lstat()
        signature = lambda item: (item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns,
                                  item.st_ctime_ns, item.st_uid, item.st_mode, item.st_nlink)
        if (signature(info) != signature(after) or signature(info) != signature(named) or
                (before.st_dev, before.st_ino) != (parent.st_dev, parent.st_ino) or
                len(raw) != info.st_size):
            raise ValueError('Receipt changed while reading')
        return json.loads(raw)
    finally:
        os.close(fd)


def services():
    result = subprocess.run(['systemctl', 'show', *OWNERS, '-p', 'Id', '-p', 'ActiveState',
                             '-p', 'SubState', '-p', 'MainPID', '-p', 'NRestarts',
                             '-p', 'InvocationID'], capture_output=True, text=True,
                            check=True, timeout=5)
    value = {}
    for block in result.stdout.strip().split('\n\n'):
        fields = dict(line.split('=', 1) for line in block.splitlines() if '=' in line)
        name = fields.get('Id', '').removesuffix('.service')
        if name in OWNERS:
            value[name] = fields
    return value


def main():
    operation, raw_folder, cursor = sys.argv[1:4]
    folder = Path(raw_folder)
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if operation == 'journal':
        command = ['journalctl', '--no-pager', '-o', 'json', '-n', '10001']
        for owner in OWNERS:
            command.extend(['-u', owner])
        command.append('--after-cursor=' + cursor)
        # Stream to the Mac; its collector enforces a byte bound.
        sys.exit(subprocess.call(command))
    value = {'boot_id': boot, 'remote_monotonic_ns': time.monotonic_ns()}
    if operation == 'cursor':
        result = subprocess.run(['journalctl', '--no-pager', '-n', '0', '--show-cursor'],
                                capture_output=True, text=True, check=True, timeout=5)
        value['cursor'] = journal_cursor(result.stdout)
    elif operation == 'restart':
        subprocess.run(['sudo', '-n', 'systemctl', 'restart', 'headunit-receiver.service'],
                       check=True, timeout=40, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        value['acknowledged'] = True
    elif operation in ('connect', 'disconnect'):
        status = request(folder, {'action': 'status'})
        if operation == 'connect':
            phones = [phone for phone in status.get('phones', [])
                      if phone.get('selected') is True]
            if len(phones) != 1:
                raise ValueError('Exactly one selected paired phone is required')
            command = {'action': 'connect', 'phone': phones[0]['address']}
        else:
            if not status.get('active'):
                raise ValueError('No current active phone to disconnect')
            command = {'action': 'disconnect', 'phone': status['active'],
                       'generation': status['generation']}
        # Date actual dispatch on CM5, before the manager's potentially slow work.
        value['remote_monotonic_ns'] = time.monotonic_ns()
        request(folder, command)
        value['acknowledged'] = True
    elif operation == 'sample':
        try:
            before_receipt = receipt(folder)
        except (OSError, ValueError, TypeError):
            before_receipt = None
        try:
            status = request(folder, {'action': 'status'})
            value['manager'] = {key: status.get(key) for key in
                                ('generation', 'active_mode', 'carplay', 'phone_home')}
            value['manager']['active_present'] = bool(status.get('active'))
            value['manager']['error_present'] = bool(status.get('error'))
            # This flag describes only the optional manager-owned subprocess.
            # The supervised headunit-cluster service has its own owner record;
            # neither process state establishes native pixels or source data.
            child = status.get('fallback_running')
            page = status.get('cluster_page')
            value['fallback'] = {
                'optional_manager_child_running': child if type(child) is bool else None,
                'manager_native_page': page if page in
                ('map', 'media', 'vehicle', 'trip', 'minimal', 'sport', 'gauges') else None,
                'visible': None}
        except (OSError, ValueError, TypeError):
            value['manager'] = None
            value['fallback'] = None
        try:
            value['receipt'] = receipt(folder)
        except (OSError, ValueError, TypeError):
            value['receipt'] = None
        after_receipt = value['receipt']
        # Sandwich the manager read with the original publication, so a different
        # launch/connection cannot be associated with that manager admission.
        if not before_receipt or not after_receipt or any(
                before_receipt.get(key) != after_receipt.get(key)
                for key in ('nonce', 'connectionGeneration', 'state', 'returnReady', 'negotiatedMode')):
            value['receipt'] = None
        value['owners'] = services()
        value['remote_monotonic_ns'] = time.monotonic_ns()
    else:
        raise ValueError('Unknown probe operation')
    print(json.dumps(value, allow_nan=False))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # Arbitrary subprocess/manager errors can include identities. Retain type only.
        print(json.dumps({'probe_error': type(error).__name__}))
        sys.exit(1)
