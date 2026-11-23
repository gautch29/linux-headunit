#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Wait briefly for an owned Wayland socket and named physical outputs."""
import argparse
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import time


def ready(runtime, name, outputs):
    path = runtime / name
    info = path.lstat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError('Wayland socket owner or type changed')
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(1)
        connection.connect(str(path))
    if not outputs:
        return True
    result = subprocess.run(['wlr-randr', '--json'], check=True, capture_output=True,
                            text=True, timeout=2)
    found = json.loads(result.stdout)
    return all(any(item.get('name') == wanted and item.get('enabled')
                   for item in found) for wanted in outputs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', default='wayland-0')
    parser.add_argument('--output', action='append', default=[])
    parser.add_argument('--timeout', type=int, default=30)
    args = parser.parse_args()
    if args.socket in ('', '.', '..') or '/' in args.socket or not 1 <= args.timeout <= 60:
        parser.error('Invalid Wayland socket or wait duration')
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        try:
            info = runtime.stat()
        except FileNotFoundError:
            time.sleep(.1)
            continue
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            parser.error('Wayland runtime directory has an unexpected owner')
        try:
            if ready(runtime, args.socket, args.output):
                return
        except (OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError):
            pass
        time.sleep(.1)
    parser.error('Wayland socket or requested output did not become ready')


if __name__ == '__main__':
    main()
