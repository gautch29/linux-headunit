# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Durable preferred phone and strict manager input validation (no radios).

Legacy mode hints stay readable by retained rollback releases, but never
choose the new receiver's capabilities or negotiated CarPlay mode.
"""
import json
import os
from pathlib import Path
import re
import stat
import tempfile

MODES = ('standard', 'ultra')
_MISSING = object()


def _unique_fields(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('Duplicate phone preferences field')
        value[key] = item
    return value


def _file_identity(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_gid, info.st_mode,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _read_preferences(path, owner):
    """Pin the directory and bounded regular file before reading saved state."""
    parent = path.parent.lstat()
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC |
                        os.O_NOFOLLOW | os.O_NONBLOCK)
    fd = -1
    try:
        opened_parent = os.fstat(directory)
        if (parent.st_dev, parent.st_ino) != (opened_parent.st_dev, opened_parent.st_ino):
            raise ValueError('Phone preferences directory changed during open')
        try:
            fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
                         dir_fd=directory)
        except FileNotFoundError:
            return _MISSING
        info = os.fstat(fd)
        expected_uid = owner[0] if owner is not None else os.geteuid()
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
                info.st_uid not in (0, expected_uid) or info.st_mode & 0o7022 or
                not 0 < info.st_size <= 65536):
            raise ValueError('Invalid phone preferences file')
        raw = os.read(fd, 65537)
        after = os.fstat(fd)
        named = os.stat(path.name, dir_fd=directory, follow_symlinks=False)
        final_parent = path.parent.lstat()
        if (len(raw) != info.st_size or _file_identity(info) != _file_identity(after) or
                _file_identity(info) != _file_identity(named) or
                (parent.st_dev, parent.st_ino, parent.st_uid, parent.st_mode) !=
                (final_parent.st_dev, final_parent.st_ino, final_parent.st_uid, final_parent.st_mode)):
            raise ValueError('Phone preferences changed during read')
        try:
            return json.loads(raw, object_pairs_hook=_unique_fields)
        except RecursionError as error:
            raise ValueError('Phone preferences nesting exceeds its schema') from error
    finally:
        if fd >= 0:
            os.close(fd)
        os.close(directory)


def address(value):
    if not isinstance(value, str) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', value):
        raise ValueError('Invalid phone address')
    return value.upper()


def atomic_json(path, value, owner=None):
    path = Path(path)
    payload = json.dumps(value, ensure_ascii=False, indent=2).encode()
    fd, name = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as out:
            os.fchmod(out.fileno(), 0o600)
            if owner is not None:
                os.fchown(out.fileno(), *owner)
            out.write(payload)
            out.flush()
            os.fsync(out.fileno())
        os.replace(name, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


class PhoneState:
    def __init__(self, path, owner=None):
        self.path, self.owner = Path(path), owner
        self.phones = {}
        self.selected = None
        data = _read_preferences(self.path, owner)
        if data is not _MISSING:
            if (not isinstance(data, dict) or set(data) != {'version', 'selected', 'phones'} or
                    type(data['version']) is not int or data['version'] != 1):
                raise ValueError('Invalid phone preferences schema')
            if not isinstance(data['phones'], dict) or len(data['phones']) > 64:
                raise ValueError('Invalid saved phones')
            for key, value in data['phones'].items():
                if address(key) != key or value not in MODES:
                    raise ValueError('Invalid saved phone mode')
            self.phones = data['phones']
            self.selected = address(data['selected']) if data['selected'] is not None else None
            if self.selected and self.selected not in self.phones:
                raise ValueError('Selected phone has no preference')

    def save(self):
        atomic_json(self.path, {'version': 1, 'selected': self.selected, 'phones': self.phones}, self.owner)

    def select(self, phone):
        phone = address(phone)
        if phone not in self.phones and len(self.phones) >= 64:
            raise ValueError('Too many saved phones')
        # This retired hint is required only by older retained releases.
        self.phones.setdefault(phone, 'standard')
        self.selected = phone
        self.save()

    def forget(self, phone):
        phone = address(phone)
        self.phones.pop(phone, None)
        if self.selected == phone:
            self.selected = None
        self.save()


def receiver_settings(base, phone, standard_display, media_state):
    """Offer configured capabilities; the phone chooses the session mode."""
    phone = address(phone)
    result = dict(base)
    # This separate camera owner belongs to the manager, never a phone child.
    result.pop('camera_manual_profile', None)
    result['auto_connect_phone'] = phone
    result['selected_phone'] = phone
    result['pair_store'] = str(Path(base['pair_store']) / ('phone-' + phone.replace(':', '')))
    result['media_state'] = str(media_state)
    page_pair = 'native_page_control_file' in result or 'native_page_lease_file' in result
    if page_pair and not all(key in result for key in ('native_page_control_file', 'native_page_lease_file')):
        raise ValueError('Native page control and surface lease must be supplied together')
    result.update(standard_cluster=True, preview_sink='wayland-gpu' if page_pair else 'wayland-touch',
                  standard_display_config=str(standard_display))
    if result.get('ultra_installed_ui'):
        result.update(ultra_native_video=True, ultra_negotiate=True)
    else:
        result.pop('ultra_installed_ui', None)
        result.pop('climate_config', None)
        result.pop('car_settings_config', None)
        result.pop('camera_state_config', None)
        result.update(vehicle_state_profile='none', vehicle_state_backend='none')
        result.update(ultra_native_video=False, ultra_negotiate=False,
                      display_config=str(standard_display))
    return result
