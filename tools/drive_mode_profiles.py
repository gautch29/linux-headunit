# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Atomic display profiles, separate from dated car observations and actuators.

The sole durable HUDP1 file contains defaults, per-mode overrides and an optional
user selection. Native/signed owners resolve vehicle observations independently.
"""
import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import re
import secrets
import stat
from carplay_readiness import clock_ns
from private_state import unique_object

MAXIMUM = 32768
FIELDS = ('layout', 'format', 'palette', 'accent', 'wallpaper', 'sportScreenAvailable')
IDENTIFIER = re.compile(r'[A-Za-z][A-Za-z0-9_-]{0,63}')
TOKEN = re.compile(r'[A-Za-z0-9_.:-]{1,64}')
UUID = re.compile(r'[0-9A-F]{8}(?:-[0-9A-F]{4}){3}-[0-9A-F]{12}')
FORMATS = ('minimal', 'sport', 'gauges', 'maps_media', 'map', 'media', 'vehicle', 'trip')
WALLPAPERS = ('phone', 'black', 'blue', 'red')


def require(valid, message='Invalid drive-mode profile'):
    if not valid:
        raise ValueError(message)


def validate_profile(value):
    require(type(value) is dict and set(value) == set(FIELDS))
    require(type(value['layout']) is str and (not value['layout'] or UUID.fullmatch(value['layout'])))
    require(type(value['format']) is str and value['format'] in FORMATS)
    require(type(value['palette']) is str and (not value['palette'] or re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', value['palette'])))
    require(type(value['accent']) is str and re.fullmatch(r'[0-9A-F]{6}', value['accent']))
    require(value['wallpaper'] in WALLPAPERS and type(value['sportScreenAvailable']) is bool)
    return dict(value)


def validate(document):
    require(type(document) is dict and set(document) == {'scope', 'revision', 'selected', 'modes'})
    require(type(document['scope']) is str and re.fullmatch(r'[A-Za-z0-9_.:-]{1,160}', document['scope']))
    require(type(document['revision']) is int and 1 <= document['revision'] < 2**64)
    require(document['selected'] is None or type(document['selected']) is str and IDENTIFIER.fullmatch(document['selected']))
    require(type(document['modes']) is list and 1 <= len(document['modes']) <= 32)
    ids = set()
    for mode in document['modes']:
        require(type(mode) is dict and set(mode) == {'id', 'name', 'base', 'profile'})
        require(type(mode['id']) is str and IDENTIFIER.fullmatch(mode['id']) and mode['id'] not in ids)
        ids.add(mode['id'])
        require(type(mode['name']) is str and 1 <= len(mode['name']) <= 64 and all(32 <= ord(c) <= 126 for c in mode['name']))
        validate_profile(mode['base'])
        validate_profile(mode['profile'])
    require(document['selected'] is None or document['selected'] in ids)
    return deepcopy(document)


def encode(document):
    value = validate(document)
    lines = ['\t'.join(('HUDP1', value['scope'], str(value['revision']), value['selected'] or '-'))]
    for mode in value['modes']:
        for tag, key in (('mode', 'profile'), ('base', 'base')):
            profile = mode[key]
            fields = [tag, mode['id'], mode['name'], *(profile[k] for k in FIELDS[:-1]),
                      'true' if profile['sportScreenAvailable'] else 'false']
            lines.append('\t'.join(fields))
    raw = ('\n'.join(lines) + '\n').encode('ascii')
    require(len(raw) <= MAXIMUM)
    return raw


def decode(raw):
    require(type(raw) is bytes and 0 < len(raw) <= MAXIMUM and raw.endswith(b'\n') and b'\r' not in raw and b'\0' not in raw)
    lines = raw.decode('ascii').splitlines()
    require(3 <= len(lines) <= 65)
    header = lines[0].split('\t')
    require(len(header) == 4 and header[0] == 'HUDP1' and re.fullmatch(r'[1-9][0-9]{0,19}', header[2]))
    value = {'scope': header[1], 'revision': int(header[2]), 'selected': None if header[3] == '-' else header[3], 'modes': []}
    bases = []
    for line in lines[1:]:
        row = line.split('\t')
        require(len(row) == 9 and row[0] in ('mode', 'base') and row[-1] in ('true', 'false'))
        profile = dict(zip(FIELDS, [*row[3:8], row[8] == 'true']))
        target = value['modes'] if row[0] == 'mode' else bases
        target.append({'id': row[1], 'name': row[2], 'profile': profile})
    require(len(value['modes']) == len(bases))
    for mode, base in zip(value['modes'], bases):
        require((mode['id'], mode['name']) == (base['id'], base['name']))
        mode['base'] = base['profile']
    return validate(value)


def identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mode, info.st_uid,
            info.st_gid, info.st_nlink, info.st_mtime_ns, info.st_ctime_ns)


def directory(path, owner):
    parent = path.parent.lstat()
    require(path.parent.resolve(strict=True) == path.parent and stat.S_ISDIR(parent.st_mode) and
            parent.st_uid == owner[0] and stat.S_IMODE(parent.st_mode) == 0o700,
            'Drive-mode directory must be real, owned and 0700')
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    if (os.fstat(fd).st_dev, os.fstat(fd).st_ino) != (parent.st_dev, parent.st_ino):
        os.close(fd)
        raise ValueError('Drive-mode directory changed')
    return fd, parent


def read(path, owner):
    folder, parent = directory(path, owner)
    fd = None
    try:
        fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=folder)
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_uid == owner[0] and stat.S_IMODE(before.st_mode) == 0o600 and
                before.st_nlink == 1 and 0 < before.st_size <= MAXIMUM, 'Drive-mode file must be private and bounded')
        raw = os.read(fd, MAXIMUM + 1)
        named = os.stat(path.name, dir_fd=folder, follow_symlinks=False)
        require(len(raw) == before.st_size and identity(before) == identity(os.fstat(fd)) == identity(named))
        final = path.parent.lstat()
        require((parent.st_dev, parent.st_ino, parent.st_uid, parent.st_mode) ==
                (final.st_dev, final.st_ino, final.st_uid, final.st_mode))
        return decode(raw)
    finally:
        if fd is not None:
            os.close(fd)
        os.close(folder)


def write(path, document, owner):
    raw = encode(document)
    folder, parent = directory(path, owner)
    name = '.drive-modes-' + secrets.token_hex(16)
    fd = None
    try:
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=folder)
        os.fchown(fd, *owner)
        offset = 0
        while offset < len(raw):
            count = os.write(fd, raw[offset:])
            require(count > 0, 'Drive-mode write incomplete')
            offset += count
        os.fsync(fd)
        final = path.parent.lstat()
        require((parent.st_dev, parent.st_ino, parent.st_uid, parent.st_mode) ==
                (final.st_dev, final.st_ino, final.st_uid, final.st_mode))
        os.replace(name, path.name, src_dir_fd=folder, dst_dir_fd=folder)
        os.fsync(folder)
    finally:
        if fd is not None:
            os.close(fd)
        try:
            os.unlink(name, dir_fd=folder)
        except FileNotFoundError:
            pass
        os.close(folder)


class DriveModeControl:
    def __init__(self, path, owner=None):
        self.path = Path(path) if path else None
        self.owner = owner or (os.getuid(), os.getgid())

    def change(self, action, data):
        require(self.path is not None, 'Drive modes are not configured')
        value = read(self.path, self.owner)
        require(type(data.get('revision')) is int and data['revision'] == value['revision'], 'Drive-mode profiles changed; refresh first')
        if action == 'drive_mode':
            require(set(data) == {'action', 'mode', 'revision'})
            value['selected'] = data['mode']
        else:
            require(action == 'drive_mode_profile' and set(data) == {'action', 'mode', 'profile', 'revision'})
            matches = [m for m in value['modes'] if m['id'] == data['mode']]
            require(len(matches) == 1, 'Choose a configured drive mode')
            patch = data['profile']
            require(patch is None or type(patch) is dict and set(patch) <= set(FIELDS))
            matches[0]['profile'] = dict(matches[0]['base']) if patch is None else dict(matches[0]['profile'], **patch)
        value['revision'] += 1
        write(self.path, value, self.owner)
        return {'message': 'Display profile saved', 'revision': value['revision'], 'vehicleModeChanged': False}

    def snapshot(self, ultra=False, publisher=None):
        empty = {'configured': bool(self.path), 'modes': [], 'current': None, 'error': '', 'vehicleControlAvailable': False}
        if not self.path:
            return empty
        try:
            value = read(self.path, self.owner)
            current = None
            status_path = Path(str(self.path) + ('.ultra.status' if ultra else '.native.status'))
            try:
                from private_state import read_private
                status = read_private(status_path, self.owner, 4096)
                now = clock_ns()
                require(type(status) is dict and set(status) == {
                    'version', 'scope', 'revision', 'source', 'mode', 'phase', 'error', 'updatedMonotonicNs',
                    'vehicleGeneration', 'observation', 'nonce', 'connectionGeneration',
                    'sportScreenAvailable', 'signedWallpaperSupported'})
                require(type(status['version']) is int and status['version'] == 1 and
                        status['scope'] == value['scope'] and type(status['revision']) is int and
                        status['revision'] == value['revision'] and type(status['updatedMonotonicNs']) is int and
                        0 <= now - status['updatedMonotonicNs'] <= 3_000_000_000)
                require(status['source'] in ('vehicle', 'simulation', 'user', 'unavailable', 'unmapped') and
                        status['mode'] in (None, *(m['id'] for m in value['modes'])) and
                        status['phase'] in ('native-profile', 'waiting', 'pending', 'confirmed', 'partial',
                                            'unavailable', 'unsupported', 'error', 'changed-on-phone', 'local-profile') and
                        type(status['error']) is str and len(status['error']) <= 256 and
                        type(status['sportScreenAvailable']) is bool and status['signedWallpaperSupported'] is False and
                        type(status['vehicleGeneration']) is int and status['vehicleGeneration'] >= 0 and
                        type(status['nonce']) is str and type(status['connectionGeneration']) is str)
                observation = status['observation']
                if status['source'] in ('vehicle', 'simulation', 'unmapped'):
                    require(type(observation) is dict and set(observation) == {
                        'acquiredSteadyNs', 'maximumAgeNs', 'ageAtPublicationNs', 'clockDomain', 'origin'} and
                            type(observation['acquiredSteadyNs']) is int and observation['acquiredSteadyNs'] > 0 and
                            type(observation['maximumAgeNs']) is int and type(observation['ageAtPublicationNs']) is int and
                            observation['clockDomain'] == 'host-steady' and
                            0 < observation['maximumAgeNs'] <= 86_400_000_000_000 and
                            0 <= observation['ageAtPublicationNs'] and
                            observation['ageAtPublicationNs'] + now - status['updatedMonotonicNs'] < observation['maximumAgeNs'] and
                            observation['origin'] in ('measured', 'calculated', 'simulation'))
                else:
                    require(observation is None and status['vehicleGeneration'] == 0)
                if ultra:
                    require(publisher is not None and (status.get('nonce'), status.get('connectionGeneration')) == publisher)
                current = status
            except (OSError, ValueError, TypeError):
                pass
            return dict(empty, **value, current=current)
        except (OSError, ValueError, UnicodeError) as error:
            return dict(empty, error=str(error)[:200])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--initialize', type=Path, required=True, help='Bounded JSON defaults; no car selection inferred')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(args.output.is_absolute())
    require(not args.output.exists(), 'Initialization must not replace user overrides')
    with args.initialize.open('rb') as source:
        raw = source.read(MAXIMUM + 1)
    require(len(raw) <= MAXIMUM)
    value = json.loads(raw, object_pairs_hook=unique_object)
    require(type(value) is dict and set(value) == {'scope', 'modes'})
    document = {'scope': value['scope'], 'revision': 1, 'selected': None,
                'modes': [dict(m, base=deepcopy(m['profile'])) for m in value['modes']]}
    write(args.output, document, (os.getuid(), os.getgid()))


if __name__ == '__main__':
    main()
