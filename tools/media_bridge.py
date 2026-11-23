#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Publish bounded iAP now-playing snapshots for the standalone CM5 display."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import stat
import time

from media_state import MediaState, validate_media


def read_private(path, maximum):
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1 or
                info.st_mode & 0o7022 or not 0 < info.st_size <= maximum):
            raise ValueError('Invalid iAP media file')
        raw = os.read(fd, maximum + 1)
        after = os.fstat(fd)
        if (len(raw) != info.st_size or
                (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_mode, info.st_nlink) !=
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_mode, after.st_nlink)):
            raise ValueError('Incomplete iAP media file')
        return raw, info.st_mtime
    finally:
        os.close(fd)


def snapshot(source):
    raw, modified = read_private(source, 8192)
    if not 0 <= time.time() - modified <= 3:
        raise ValueError('Stale iAP media file')
    try:
        value = json.loads(raw)
    except RecursionError as error:
        raise ValueError('iAP media nesting exceeds its schema') from error
    if not isinstance(value, dict):
        raise ValueError('Invalid iAP media snapshot schema')
    digest = value.pop('iap_artwork_sha256', '')
    if not isinstance(digest, str) or digest and not re.fullmatch('[0-9a-f]{64}', digest):
        raise ValueError('Invalid iAP artwork hash')
    value = validate_media(value)
    value['source'] = 'iap'
    return value, digest


def run(source, destination, phone_address=None, bluez=None):
    media = MediaState(destination)
    media.publish({'available': False})
    if phone_address and bluez is None:
        from bluez_phones import BluezPhones
        bluez = BluezPhones()
    stopping = False
    def stop(_number, _frame):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    artwork_hash = ''
    next_write = 0.
    next_bluez_read = 0.
    bluez_state = {'available': False}
    last_state = None
    while not stopping:
        now = time.monotonic()
        try:
            state, digest = snapshot(source)
        except (OSError, ValueError, TypeError, json.JSONDecodeError):
            state, digest = {'available': False}, ''
        if not state['available'] and bluez:
            if now >= next_bluez_read:
                try:
                    bluez_state = bluez.media(phone_address)
                except Exception:
                    bluez_state = {'available': False}
                next_bluez_read = now + 1
            state = bluez_state
        if state != last_state or now >= next_write:
            cover = None
            if state['available'] and digest and digest != artwork_hash:
                try:
                    raw, _ = read_private(Path(str(source) + '.art'), 1048576)
                    if hashlib.sha256(raw).hexdigest() == digest:
                        cover = base64.b64encode(raw).decode('ascii')
                except (OSError, ValueError):
                    pass
            try:
                media.publish(state, artwork=cover,
                              keep_artwork=cover is None and bool(digest) and digest == artwork_hash)
            except ValueError:
                try:
                    media.publish(state)
                except ValueError:
                    media.publish({'available': False})
            artwork_hash = digest if state['available'] and (cover or (media.last or {}).get('artwork')) else ''
            last_state = state
            next_write = now + 1
        time.sleep(.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--phone-address')
    args = parser.parse_args()
    if args.input == args.output or not args.output.parent.is_dir():
        parser.error('Invalid media paths')
    run(args.input, args.output, args.phone_address)


if __name__ == '__main__':
    main()
