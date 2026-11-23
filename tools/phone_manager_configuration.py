# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Strict static phone-manager profile. Validation never starts a manager."""
import json
import os
from pathlib import Path
import re
import stat


_FIELDS = {'schemaVersion', 'enabled', 'stateDirectory', 'standardDisplayConfig', 'autoConnectSelected'}
_OPTIONAL = {'pairing', 'receiverRecoveryAttempts'}
_PROFILE_BOUND = 16384
_SAFE_PATH = re.compile(r'/[A-Za-z0-9_./-]+')


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _service_path(value, field):
    _require(type(value) is str and len(value) <= 4000 and _SAFE_PATH.fullmatch(value) is not None and
             not value.endswith('/') and all(part and part not in ('.', '..') and len(part) <= 255
                                            for part in value.split('/')[1:]),
             f'{field} must be a bounded absolute canonical path without service syntax')
    return value


def _real_state_ancestors(value):
    """Missing state directories are allowed; existing components must be real."""
    current = Path('/')
    for part in value.split('/')[1:]:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            # No remaining descendant can exist below a missing component.
            return
        _require(stat.S_ISDIR(info.st_mode), 'State directory and its existing ancestors must be real directories')
        _require(current.resolve(strict=True) == current, 'State directory must not have redirected ancestors')


def validate_configuration(value):
    """Validate static data and existing state ancestors; create no endpoints."""
    _require(type(value) is dict and _FIELDS <= set(value) <= _FIELDS | _OPTIONAL,
             'Unknown or missing phone-manager profile field')
    _require(type(value['schemaVersion']) is int and value['schemaVersion'] == 1, 'Unsupported phone-manager schema')
    for field in ('enabled', 'autoConnectSelected'):
        _require(type(value[field]) is bool, f'{field} must be boolean')
    attempts = value.get('receiverRecoveryAttempts', 0)
    _require(type(attempts) is int and 0 <= attempts <= 3,
             'receiverRecoveryAttempts must be an integer from zero to three')
    state = _service_path(value['stateDirectory'], 'stateDirectory')
    _service_path(value['standardDisplayConfig'], 'standardDisplayConfig')
    _require(len(state + '/manager.sock') <= 103, 'Manager socket path exceeds 103 printable ASCII bytes')
    _real_state_ancestors(state)
    if 'pairing' in value:
        pairing = value['pairing']
        _require(type(pairing) is dict and set(pairing) == {'enabled', 'adapter'},
                 'Pairing settings need enabled and adapter only')
        _require(type(pairing['enabled']) is bool, 'Pairing enabled must be boolean')
        _require(type(pairing['adapter']) is str and re.fullmatch(r'hci(?:0|[1-9][0-9]{0,2})', pairing['adapter']),
                 'Pairing adapter must be a bounded hci controller name')
    # Strings remain exactly as configured; normalization could conceal a
    # redirected path or make the native socket client use another endpoint.
    return dict(value, **({'pairing': dict(value['pairing'])} if 'pairing' in value else {}))


def _unique(pairs):
    value = {}
    for key, field in pairs:
        _require(key not in value, 'Duplicate phone-manager profile field')
        value[key] = field
    return value


def _nonfinite(_):
    raise ValueError('Nonfinite JSON value in phone-manager profile')


def _same(a, b):
    return (a.st_dev, a.st_ino) == (b.st_dev, b.st_ino)


def _stable(a, b):
    return _same(a, b) and a.st_size == b.st_size and a.st_mtime_ns == b.st_mtime_ns and a.st_ctime_ns == b.st_ctime_ns


def _trusted(info, owners):
    return stat.S_ISREG(info.st_mode) and info.st_uid in owners and info.st_nlink == 1 and \
        not info.st_mode & 0o7022 and 0 < info.st_size <= _PROFILE_BOUND


def load_configuration(path, owner_uid=None):
    """Read one stable owned profile, admitting root and a configured account."""
    _require(owner_uid is None or (type(owner_uid) is int and 0 <= owner_uid < (1 << 32) - 1),
             'Invalid configured phone-manager owner UID')
    owners = {0, os.getuid()}
    if owner_uid is not None:
        owners.add(owner_uid)
    before = os.stat(path, follow_symlinks=False)
    _require(_trusted(before, owners), 'Phone-manager profile must be bounded owned single-link regular data without unsafe modes')
    # Never open a known symlink, FIFO, device, directory or oversized endpoint.
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        opened = os.fstat(fd)
        _require(_trusted(opened, owners) and _stable(before, opened), 'Phone-manager profile changed during open')
        raw = bytearray()
        complete = False
        for _ in range(64):
            chunk = os.read(fd, _PROFILE_BOUND + 1 - len(raw))
            if not chunk:
                complete = True
                break
            raw.extend(chunk)
            _require(len(raw) <= _PROFILE_BOUND, 'Phone-manager profile exceeds its read bound')
        after = os.fstat(fd)
        _require(complete and len(raw) == opened.st_size and _trusted(after, owners) and _stable(opened, after),
                 'Phone-manager profile changed or exceeded bounded read turns')
        current = os.stat(path, follow_symlinks=False)
        _require(_trusted(current, owners) and _stable(after, current), 'Phone-manager profile is no longer current')
    finally:
        os.close(fd)
    return validate_configuration(json.loads(raw, object_pairs_hook=_unique, parse_constant=_nonfinite))


def validate_manager_settings(profile, settings):
    """Require a persistent receiver and one externally supervised boot cluster."""
    profile = validate_configuration(profile)
    _require(type(settings) is dict, 'Phone-manager receiver settings must be an object')
    _require(type(settings.get('seconds')) is int and settings['seconds'] == 0,
             'Phone-manager receiver requires seconds=0')
    fallback = settings.get('fallback_cluster', False)
    _require(type(fallback) is bool and not fallback, 'Phone manager must not supervise the externally managed boot cluster')
    media = settings.get('media_state')
    _require(isinstance(media, (str, Path)) and str(media) == profile['stateDirectory'] + '/media.json',
             'Receiver media_state must equal the manager stateDirectory/media.json')
    greeting = settings.get('greeting_sequence_file')
    _require(greeting is None or str(greeting) == profile['stateDirectory'] + '/greeting-sequence',
             'Greeting preference must equal manager stateDirectory/greeting-sequence')
    publication = settings.get('prephone_publication_socket')
    _require(publication is None or str(publication) == profile['stateDirectory'] + '/prephone.sock',
             'Native publication must equal manager stateDirectory/prephone.sock')
    pairing = profile.get('pairing', {})
    _require(not pairing.get('enabled') or pairing['adapter'] == 'hci0',
             'The current receiver uses hci0; enabled pairing must use the same controller')
    return profile
