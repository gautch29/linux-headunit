# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Read-only native phone-page admission and one private process owner.

Importing this module opens no display, bus, radio or runtime endpoint. The
retained page.lock is separate from manager.lock and is never deleted. A page
claims it before importing GTK or loading a display library.
"""
import fcntl
import json
import os
from pathlib import Path
import re
import stat


_FIELDS = {'schemaVersion', 'enabled', 'managerSocket', 'waylandDisplay', 'output', 'homeButton'}
_SURFACE_FIELDS = {'surfaceControlFile', 'surfaceLeaseFile'}
_PATH = re.compile(r'/[A-Za-z0-9_./-]+\Z')
_NAME = re.compile(r'[A-Za-z0-9._-]{1,127}\Z')
_BOUND = 16384


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _absolute(value, field, maximum=4000):
    _require(type(value) is str and len(value) <= maximum and _PATH.fullmatch(value) and
             not value.endswith('/') and all(part and part not in ('.', '..') and len(part) <= 255
                                            for part in value.split('/')[1:]),
             f'{field} must be a bounded canonical absolute path without service syntax')
    return value


def _uid(value):
    _require(value is None or (type(value) is int and 0 <= value < (1 << 32) - 1),
             'Invalid configured native-page owner UID')


def validate_configuration(value):
    """Validate only explicit static values; do not inspect a runtime socket."""
    _require(type(value) is dict and type(value.get('schemaVersion')) is int and value['schemaVersion'] in (1, 2),
             'Unsupported native-page schema')
    _require(set(value) == _FIELDS | (_SURFACE_FIELDS if value['schemaVersion'] == 2 else set()),
             'Unknown or missing native-page profile field')
    _require(type(value['enabled']) is bool, 'Native-page enabled must be boolean')
    _require(not value['enabled'] or value['schemaVersion'] == 2,
             'Enabled native page requires schemaVersion 2 and surface handoff')
    _absolute(value['managerSocket'], 'managerSocket', 103)
    if value['schemaVersion'] == 2:
        parent = Path(value['managerSocket']).parent
        for field, filename in (('surfaceControlFile', 'home-surface.request'), ('surfaceLeaseFile', 'home-surface.lease')):
            _absolute(value[field], field)
            _require(Path(value[field]) == parent / filename, 'Native page needs fixed handoff filenames beside manager.sock')
    for key in ('waylandDisplay', 'output'):
        _require(type(value[key]) is str and _NAME.fullmatch(value[key]) and value[key] not in ('.', '..'),
                 f'{key} must be an explicit bounded connector name')
    home = value['homeButton']
    _require(type(home) is dict and {'corner', 'margin', 'size'} <= set(home) <= {'corner', 'margin', 'size', 'enabled'},
             'homeButton needs corner, margin, size and optional enabled')
    _require('enabled' not in home or type(home['enabled']) is bool,
             'Home-button enabled must be boolean')
    _require(type(home['corner']) is str and home['corner'] in
             ('bottom-right', 'bottom-left', 'top-right', 'top-left'), 'Unsupported home-button corner')
    _require(type(home['margin']) is int and 0 <= home['margin'] <= 96,
             'Home-button margin must be an integer from 0 to 96')
    _require(type(home['size']) is int and 48 <= home['size'] <= 96,
             'Home-button size must be an integer from 48 to 96')
    return dict(value, homeButton=dict(home))


def _unique(pairs):
    value = {}
    for key, item in pairs:
        _require(key not in value, 'Duplicate native-page profile field')
        value[key] = item
    return value


def _constant(_):
    raise ValueError('Nonfinite native-page profile value')


def _same(left, right):
    return (left.st_dev, left.st_ino) == (right.st_dev, right.st_ino)


def _signature(value):
    return (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns,
            value.st_ctime_ns, value.st_uid, value.st_mode, value.st_nlink)


def _trusted(value, owners, maximum=_BOUND):
    return stat.S_ISREG(value.st_mode) and value.st_uid in owners and value.st_nlink == 1 and \
        not value.st_mode & 0o7022 and 0 < value.st_size <= maximum


def _read_static(path, uid=None, maximum=_BOUND):
    """Read stable single-link data without interpreting it or creating state."""
    _uid(uid)
    path = Path(_absolute(os.fspath(path), 'Native-page profile path'))
    owners = {0, os.getuid()}
    if uid is not None:
        owners.add(uid)
    before = path.lstat()
    _require(path.resolve(strict=True) == path and _trusted(before, owners, maximum),
             'Native-page profile must be bounded real trusted single-link data without unsafe modes')
    parent_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    fd = -1
    try:
        parent = os.fstat(parent_fd)
        _require(_same(parent, path.parent.lstat()) and path.parent.resolve(strict=True) == path.parent,
                 'Native-page profile parent changed during open')
        fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent_fd)
        opened = os.fstat(fd)
        _require(_trusted(opened, owners, maximum) and _signature(before) == _signature(opened),
                 'Native-page profile changed during open')
        raw = bytearray()
        complete = False
        for _ in range(64):
            chunk = os.read(fd, maximum + 1 - len(raw))
            if not chunk:
                complete = True
                break
            raw.extend(chunk)
            _require(len(raw) <= maximum, 'Native-page profile exceeds its read bound')
        after = os.fstat(fd)
        current = os.stat(path.name, dir_fd=parent_fd, follow_symlinks=False)
        _require(complete and len(raw) == opened.st_size and _trusted(after, owners, maximum) and
                 _signature(opened) == _signature(after) == _signature(current) and
                 _same(parent, path.parent.lstat()) and path.resolve(strict=True) == path,
                 'Native-page profile or its parent changed during read')
        _require(_signature(path.lstat()) == _signature(current), 'Native-page profile is no longer current')
    finally:
        if fd >= 0:
            os.close(fd)
        os.close(parent_fd)
    return bytes(raw), _signature(after)


def load_configuration(path, uid=None):
    """Read one stable owned native-page JSON profile; contact no services."""
    raw, _ = _read_static(path, uid)
    return validate_configuration(json.loads(raw, object_pairs_hook=_unique, parse_constant=_constant))


def _static_snapshot(path, uid, maximum=_BOUND):
    # Metadata alone can match after a same-size rewrite on a coarse filesystem.
    # Retain bounded stable bytes as well as ownership/identity for admission.
    return _read_static(path, uid, maximum)


def inspect_settings(profile_path, manager_profile_path, settings, uid, checker=None):
    """Admit an explicitly installed page on both production main routes.

    Uses the actual manager admission and C++ display parser; validates static
    data only. Missing runtime directories, sockets and displays are permitted.
    """
    from phone_manager_boot import inspect_settings as inspect_manager, _displays
    from phone_manager_configuration import load_configuration as load_manager
    _uid(uid)
    page_before = _static_snapshot(profile_path, uid)
    manager_before = _static_snapshot(manager_profile_path, uid)
    page = load_configuration(profile_path, uid)
    _require(page['enabled'], 'An explicitly installed native page must be enabled')
    # Pin both files before manager admission, so its cluster coherence and our
    # page-route checks cannot accidentally validate different generations.
    static_manager = load_manager(manager_profile_path, uid)
    _require(type(settings) is dict and settings.get('display_config'),
             'Native-page admission needs receiver display_config')
    paths = (settings['display_config'], static_manager['standardDisplayConfig'])
    before = [_static_snapshot(path, uid) for path in paths]
    manager = inspect_manager(manager_profile_path, settings, uid, checker)
    _require(manager['enabled'], 'The native page requires an enabled supervised phone manager')
    _require(page['managerSocket'] == manager['stateDirectory'] + '/manager.sock',
             'Native-page managerSocket must equal the supervised manager endpoint')
    validate_surface_settings(page, manager, settings)
    checker = checker or Path(settings['bin_dir']) / 'headunit-display-config-check'
    for path in paths:
        main = _displays(path, uid, checker)[0]
        _require((page['waylandDisplay'], page['output']) == (main['wayland'], main['output']),
                 'Native page must target the main output in both Standard and Ultra')
    _require([_static_snapshot(path, uid) for path in paths] == before and
             _static_snapshot(profile_path, uid) == page_before and
             _static_snapshot(manager_profile_path, uid) == manager_before,
             'Native-page startup configuration changed during validation')
    return page


def validate_surface_settings(page, manager, settings):
    """Tie ready Home to the exact receiver endpoints, without runtime writes."""
    from receiver_configuration import validate_native_page_settings
    validate_native_page_settings(settings)
    _require(page['enabled'] and page['schemaVersion'] == 2, 'Ready native page requires enabled schemaVersion 2')
    folder = Path(manager['stateDirectory'])
    for page_field, setting, filename in (('surfaceControlFile', 'native_page_control_file', 'home-surface.request'),
                                         ('surfaceLeaseFile', 'native_page_lease_file', 'home-surface.lease')):
        _require(page.get(page_field) == str(folder / filename) and settings.get(setting) is not None and
                 Path(settings[setting]) == folder / filename, 'Page and receiver must share the exact fixed surface handoff pair')
    reserved = {'manager.sock', 'page.lock', 'manager.lock', 'media.json', 'media.json.iap', 'media.json.iap.art',
                'phones.json', 'audio-control.json', 'audio-state.json', 'audio-preferences.json'}
    _require(all(Path(page[field]) not in {folder / name for name in reserved} for field in _SURFACE_FIELDS),
             'Native page surface endpoints collide with managed state')


def inspect_boot(profile_path, manager_profile_path, receiver_config_path, uid=None, checker=None):
    """Admit the optional camera-free resident page from real static files.

    All native/signed camera routes are deliberately excluded: snapshot intent
    alone cannot arbitrate an independently activated reverse-camera surface.
    This function opens neither a runtime endpoint nor a display or radio.
    """
    from receiver_configuration import parse_receiver_configuration
    _uid(uid)
    uid = os.getuid() if uid is None else uid
    page_before = _static_snapshot(profile_path, uid)
    manager_before = _static_snapshot(manager_profile_path, uid)
    raw, receiver_before = _read_static(receiver_config_path, uid)
    receiver_before = (raw, receiver_before)
    settings = parse_receiver_configuration(raw, Path(receiver_config_path))
    _require(not any(key in settings for key in ('standard_camera_control_file',
                                               'standard_camera_lease_file')),
             'Resident phone page cannot combine configured manual or Standard camera routes')
    if settings.get('camera_manual_profile'):
        _require(settings.get('camera_state_config') and settings.get('ultra_negotiate') and settings.get('ultra_native_video'),
                 'Resident camera requires its signed Ultra state owner')
    ui_path = settings.get('ultra_installed_ui')
    ui_before = None
    if ui_path is not None:
        ui_raw, ui_before = _read_static(ui_path, uid, 65536)
        ui_before = (ui_raw, ui_before)
        text = ui_raw.decode('utf-8')
        _require(not any(ord(c) < 32 and c != '\n' or ord(c) == 127 for c in text),
                 'Unsupported installed-UI profile characters')
        fields = {}
        for line in text.splitlines():
            if not line or line.startswith('#'):
                continue
            key, separator, value = line.partition('=')
            _require(separator and key and key not in fields, 'Invalid or duplicate installed-UI profile field')
            fields[key] = value
        _require(not any(fields.get(key) for key in ('nativeCameraControlFile', 'nativeCameraLeaseFile')),
                 'Resident phone page cannot combine installed-UI native or signed camera routes')
        if any(fields.get(key) for key in ('cameraProfile', 'cameraControlFile')):
            _require(fields.get('cameraProfile') and fields.get('cameraControlFile') and settings.get('camera_state_config'),
                     'Resident phone page requires complete signed camera routes')
        page_fields = ('nativePageControlFile', 'nativePageLeaseFile')
        if any(key in fields for key in page_fields):
            _require(all(key in fields for key in page_fields) and
                     fields[page_fields[0]] == str(settings.get('native_page_control_file', '')) and
                     fields[page_fields[1]] == str(settings.get('native_page_lease_file', '')),
                     'Installed UI page handoff must match the exact receiver pair')
    configuration = inspect_settings(profile_path, manager_profile_path, settings, uid, checker)
    _require(_static_snapshot(profile_path, uid) == page_before and
             _static_snapshot(manager_profile_path, uid) == manager_before and
             _static_snapshot(receiver_config_path, uid) == receiver_before and
             (ui_path is None or _static_snapshot(ui_path, uid, 65536) == ui_before),
             'Native-page boot configuration changed during validation')
    return configuration


class PageOwner:
    """Own one retained empty page.lock in an existing euid-owned 0700 directory.

    No socket, deletion, inherited-launcher API or explicit LOCK_UN. Every
    descriptor is close-on-exec. verify() rejects later pathname generations;
    close() simply releases our handles and never touches a replacement inode.
    """
    def __init__(self, directory):
        self._directory_fd = self._lock_fd = -1
        self._directory_identity = self._lock_identity = None
        self.directory = Path(_absolute(os.fspath(directory), 'Native-page private directory'))
        self.path = self.directory / 'page.lock'
        try:
            before = self.directory.lstat()
            _require(self._private(before) and self.directory.resolve(strict=True) == self.directory,
                     'Native-page directory must be existing real owned 0700 data without redirected ancestors')
            self._directory_fd = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC |
                                         os.O_NOFOLLOW | os.O_NONBLOCK)
            self._directory_identity = os.fstat(self._directory_fd)
            _require(self._private(self._directory_identity) and _same(before, self._directory_identity),
                     'Native-page directory changed during open')
            self._acquire()
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _private(info):
        return stat.S_ISDIR(info.st_mode) and info.st_uid == os.geteuid() and (info.st_mode & 0o7777) == 0o700

    @staticmethod
    def _lock(info):
        return stat.S_ISREG(info.st_mode) and info.st_uid == os.geteuid() and (info.st_mode & 0o7777) == 0o600 and \
            info.st_nlink == 1 and info.st_size == 0

    def _inspect(self):
        try:
            return os.stat('page.lock', dir_fd=self._directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            return None

    def _verify_directory(self):
        _require(self._directory_fd >= 0 and self._directory_identity is not None, 'Native-page owner is closed')
        opened, named = os.fstat(self._directory_fd), self.directory.lstat()
        _require(self._private(opened) and self._private(named) and _same(opened, self._directory_identity) and
                 _same(named, self._directory_identity) and self.directory.resolve(strict=True) == self.directory,
                 'Native-page private directory generation changed or became unsafe')

    def verify(self):
        self._verify_directory()
        _require(self._lock_fd >= 0 and self._lock_identity is not None, 'Native-page lock is unavailable')
        opened, named = os.fstat(self._lock_fd), self._inspect()
        _require(named is not None and self._lock(opened) and self._lock(named) and
                 _signature(opened) == _signature(named) == _signature(self._lock_identity),
                 'Native-page lock generation changed or became unsafe')

    def _acquire(self):
        for _ in range(4):
            self._verify_directory()
            named = self._inspect()
            if named is None:
                try:
                    self._lock_fd = os.open('page.lock', os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC |
                                            os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=self._directory_fd)
                except FileExistsError:
                    continue
                created = os.fstat(self._lock_fd)
                self._verify_directory()
                current = self._inspect()
                _require(current is not None and _same(created, current) and stat.S_ISREG(created.st_mode) and
                         created.st_uid == os.geteuid() and created.st_nlink == 1 and created.st_size == 0,
                         'New native-page lock changed before securing it')
                # Secure only the new pinned inode; never rewrite a stale file.
                os.fchmod(self._lock_fd, 0o600)
            else:
                _require(self._lock(named), 'Native-page lock must be empty owned single-link 0600 regular data')
                self._lock_fd = os.open('page.lock', os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
                                        dir_fd=self._directory_fd)
            self._lock_identity = os.fstat(self._lock_fd)
            _require(self._lock(self._lock_identity) and (named is None or _signature(named) == _signature(self._lock_identity)),
                     'Native-page lock changed during open')
            try:
                fcntl.flock(self._lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise ValueError('Native phone page already has a live owner') from error
            self.verify()
            return
        raise ValueError('Native-page lock creation exceeded its retry bound')

    def close(self):
        if self._lock_fd >= 0:
            os.close(self._lock_fd)
            self._lock_fd = -1
        if self._directory_fd >= 0:
            os.close(self._directory_fd)
            self._directory_fd = -1

    def __enter__(self):
        self.verify()
        return self

    def __exit__(self, *_):
        self.close()
