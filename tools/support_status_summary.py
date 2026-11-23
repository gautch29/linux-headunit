#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Project an explicitly supplied manager status into a new private offline file."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import secrets
import stat
import sys

MAX_BYTES = 1_500_000
MAX_DEPTH = 16
MAX_ITEMS = 256
MAX_STRING = 4096
MAX_NODES = 8192
U64 = (1 << 64) - 1
ROW_IDS = ('fuel_percent', 'range_km', 'battery_voltage', 'ambient_c',
           'coolant_c', 'oil_c', 'oil_pressure_kpa', 'odometer_km')


def invalid():
    raise ValueError('Invalid supplied status')


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            invalid()
        result[key] = value
    return result


def number(text):
    value = float(text)
    if not math.isfinite(value):
        invalid()
    return value


def decode(raw):
    if type(raw) is not bytes or not 0 < len(raw) <= MAX_BYTES:
        invalid()
    value = json.loads(raw.decode('utf-8'), object_pairs_hook=unique,
                       parse_constant=lambda _: invalid(), parse_float=number)
    pending = [(value, 0)]
    count = 0
    while pending:
        item, depth = pending.pop()
        count += 1
        if depth > MAX_DEPTH or count > MAX_NODES:
            invalid()
        if type(item) is dict:
            if len(item) > MAX_ITEMS or any(len(key) > MAX_STRING for key in item):
                invalid()
            pending.extend((child, depth + 1) for child in item.values())
        elif type(item) is list:
            if len(item) > MAX_ITEMS:
                invalid()
            pending.extend((child, depth + 1) for child in item)
        elif type(item) is str:
            if len(item) > MAX_STRING:
                invalid()
        elif type(item) is int and not -U64 <= item <= U64:
            invalid()
    if type(value) is not dict:
        invalid()
    return value


def obj(parent, key):
    value = parent.get(key)
    if value is None:
        return {}
    if type(value) is not dict:
        invalid()
    return value


def field(parent, key, kind, choices=None):
    value = parent.get(key)
    if value is None:
        return None
    if kind == 'decimal':
        if (type(value) is not str or not re.fullmatch(r'0|[1-9][0-9]{0,19}', value)
                or int(value) > U64):
            invalid()
    elif type(value) is not kind:
        invalid()
    if kind is int and not 0 <= value <= U64:
        invalid()
    if choices is not None and value not in choices:
        invalid()
    return value


def project(raw):
    """No clock, live authority, file discovery, raw strings or retained input."""
    status = decode(raw)
    carplay = obj(status, 'carplay')
    recovery = obj(status, 'recovery')
    audio = obj(status, 'audio')
    output = obj(audio, 'output')
    observations = obj(status, 'media_observations')
    pcm = obj(observations, 'pcm')
    vehicle = obj(status, 'prephone_vehicle')
    publisher = obj(vehicle, 'publisher')
    rows = vehicle.get('rows', [])
    if type(rows) is not list:
        invalid()
    safe_rows = []
    seen = set()
    for row in rows:
        if type(row) is not dict:
            invalid()
        identifier = row.get('id')
        if type(identifier) is not str or identifier not in ROW_IDS:
            continue
        if identifier in seen:
            invalid()
        seen.add(identifier)
        safe_rows.append({'id': identifier,
                          'availability': field(row, 'availability', str, (
                              'available', 'missing', 'unavailable', 'stale', 'disconnected')),
                          'acquiredNs': field(row, 'acquiredNs', 'decimal'),
                          'maximumAgeNs': field(row, 'maximumAgeNs', 'decimal'),
                          'origin': field(row, 'origin', str, ('measured', 'calculated', 'simulation'))})
    return {
        'schema_version': 1,
        'evidence': 'offline_supplied_manager_status',
        'current_authority': False,
        'manager': {
            'generation': field(status, 'generation', int),
            'active_mode': field(status, 'active_mode', str, ('standard', 'ultra')),
            'fallback_running': field(status, 'fallback_running', bool),
            'recovery': {'remainingAttempts': field(recovery, 'remainingAttempts', int, range(4))},
            'carplay': {'state': field(carplay, 'state', str, (
                'disconnected', 'starting', 'ready', 'home', 'fault', 'stopped')),
                        'returnReady': field(carplay, 'returnReady', bool)}},
        'audio': {
            'available': field(audio, 'available', bool),
            'pending': field(audio, 'pending', bool),
            'output': {key: field(output, key, 'decimal') for key in (
                'sampledMonotonicNs', 'acceptedBlocks', 'discontinuities', 'underruns')} |
                      {key: field(output, key, bool) for key in ('fresh', 'starved')}},
        'media_observations': {
            'version': field(observations, 'version', int, (1,)),
            'generation': field(observations, 'generation', int),
            'live': field(observations, 'live', bool),
            'pcm': {'state': field(pcm, 'state', str, (
                'unavailable', 'baseline', 'progressing', 'no-advance',
                'unchanged-sample', 'invalid-observation', 'stale'))} |
                   {key: field(pcm, key, 'decimal') for key in (
                       'sampledMonotonicNs', 'acceptedBlocks',
                       'previousSampledMonotonicNs', 'acceptedBlocksDelta')}},
        'prephone_vehicle': {
            'configured': field(vehicle, 'configured', bool),
            'version': field(vehicle, 'version', int, (1,)),
            'state': field(vehicle, 'state', str, (
                'disabled', 'publication_unavailable', 'admitted', 'awaiting_owner', 'awaiting_source',
                'undated_source', 'invalid_snapshot', 'generation_mismatch',
                'disconnected', 'owner_retired', 'publisher_expired',
                'protocol_failure', 'clock_failure')),
            'clockDomain': field(vehicle, 'clockDomain', str, ('host-steady',)),
            **{key: field(vehicle, key, 'decimal') for key in (
                'sourceGeneration', 'sourceStartedNs', 'sourceSampledNs')},
            'publisher': {'live': field(publisher, 'live', bool),
                          'sequence': field(publisher, 'sequence', 'decimal'),
                          'copiedNs': field(publisher, 'copiedNs', 'decimal')},
            'rows': safe_rows}}


def directory(path):
    """Pin only the explicit directory chain; refuse redirected components."""
    if not path.is_absolute() or any(part in ('.', '..') for part in path.parts):
        invalid()
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


def signature(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def read_input(path):
    parent = directory(path.parent)
    try:
        fd = os.open(path.name, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=parent)
        with os.fdopen(fd, 'rb') as stream:
            before = os.fstat(stream.fileno())
            if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                    before.st_uid != os.getuid() or stat.S_IMODE(before.st_mode) != 0o600 or
                    not 0 < before.st_size <= MAX_BYTES):
                invalid()
            raw = stream.read(MAX_BYTES + 1)
            after = os.stat(path.name, dir_fd=parent, follow_symlinks=False)
            if (len(raw) != before.st_size or signature(before) != signature(after) or
                    signature(before) != signature(os.fstat(stream.fileno()))):
                invalid()
            return raw
    finally:
        os.close(parent)


def write_output(path, value):
    parent = directory(path.parent)
    temporary = '.support-' + secrets.token_hex(16)
    created = False
    try:
        info = os.fstat(parent)
        if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
            invalid()
        raw = (json.dumps(value, indent=2, allow_nan=False) + '\n').encode('ascii')
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=parent)
        created = True
        with os.fdopen(fd, 'wb') as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        # link is an atomic no-replace publication, including existing symlinks.
        os.link(temporary, path.name, src_dir_fd=parent, dst_dir_fd=parent, follow_symlinks=False)
    finally:
        if created:
            os.unlink(temporary, dir_fd=parent)
        os.close(parent)


class SafeParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, 'Status summary unavailable: invalid arguments.\n')


def main(argv=None):
    parser = SafeParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        value = project(read_input(args.input))
        write_output(args.output, value)
    except (OSError, ValueError, TypeError, UnicodeError, RecursionError, OverflowError):
        print('Status summary unavailable: unsafe paths or invalid supplied status.', file=sys.stderr)
        return 1
    print('Offline status summary written; current authority remains unknown.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
