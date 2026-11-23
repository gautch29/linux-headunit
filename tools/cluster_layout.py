# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Local Ultra style selection. The render owner confirms the phone's transition."""
import argparse
import json
import os
from pathlib import Path
import re
import stat
import tempfile
from carplay_readiness import clock_ns, decimal
from cluster_page import available as native_pages

UUID = re.compile(r'[0-9A-F]{8}(?:-[0-9A-F]{4}){3}-[0-9A-F]{12}')
MAX_PROFILE_BYTES = 8192  # The installed-UI owner and sealed release use the same bound.


def configured_path(profile, key_name='clusterLayoutFile'):
    if not profile:
        return None
    path = Path(profile)
    parent_fd = fd = None
    try:
        # Source profiles historically allow explicit symlinks and relative paths.
        # Pin their resolved target without changing that input policy.
        parent = path.parent.stat()
        parent_identity = parent.st_dev, parent.st_ino
        parent_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NONBLOCK)
        opened_parent = os.fstat(parent_fd)
        if (opened_parent.st_dev, opened_parent.st_ino) != parent_identity:
            return None
        before = os.stat(path.name, dir_fd=parent_fd)
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= MAX_PROFILE_BYTES:
            return None
        fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK, dir_fd=parent_fd)
        def identity(info):
            return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns
        expected = identity(before)
        opened = os.fstat(fd)
        if not stat.S_ISREG(opened.st_mode) or identity(opened) != expected:
            return None
        chunks = []
        remaining = before.st_size
        while remaining:
            chunk = os.read(fd, remaining)
            if not chunk:
                return None
            chunks.append(chunk)
            remaining -= len(chunk)
        if (os.read(fd, 1) or identity(os.fstat(fd)) != expected or
                identity(os.stat(path.name, dir_fd=parent_fd)) != expected):
            return None
        final_parent = path.parent.stat()
        if (final_parent.st_dev, final_parent.st_ino) != parent_identity:
            return None
        lines = b''.join(chunks).decode('utf-8').splitlines()
    except (OSError, UnicodeError):
        return None  # Optional control; receiver startup reports a missing pack/profile separately.
    finally:
        if fd is not None:
            os.close(fd)
        if parent_fd is not None:
            os.close(parent_fd)
    for line in lines:
        key, _, value = line.partition('=')
        if key == key_name:
            path = Path(value)
            if not path.is_absolute():
                raise ValueError(key_name + ' control path must be absolute')
            return path
    return None


def snapshot(path, owner=None, publisher=None):
    if not path:
        return None
    try:
        fd = os.open(str(path) + '.status', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != (owner[0] if owner else os.getuid()) or
                    info.st_mode & 0o077 or info.st_nlink != 1 or not 0 < info.st_size <= 65536):
                return None
            value = json.loads(os.read(fd, 65537))
        finally:
            os.close(fd)
        choices = value['choices']
        if (type(value.get('version')) is not int or value['version'] != 2 or
                type(value.get('nonce')) is not str or not re.fullmatch(r'[0-9a-f]{32}', value['nonce']) or
                not 0 <= clock_ns() - decimal(value['updatedMonotonicNs'], True) <= 3_000_000_000):
            return None
        decimal(value['sequence'], True)
        decimal(value['connectionGeneration'])
        if publisher is not None and (value['nonce'], value['connectionGeneration']) != publisher:
            return None
        if (not isinstance(choices, list) or not 1 <= len(choices) <= 64 or
                any(not isinstance(choice, dict) or set(choice) != {'id', 'name'} or
                    not isinstance(choice['id'], str) or not UUID.fullmatch(choice['id']) or
                    not isinstance(choice['name'], str) or len(choice['name']) > 256 for choice in choices) or
                len({choice['id'] for choice in choices}) != len(choices) or
                value['active'] not in {choice['id'] for choice in choices} or
                value['pending'] not in {'', *(choice['id'] for choice in choices)} or
                type(value['ready']) is not bool or type(value['transitioning']) is not bool or
                not isinstance(value['error'], str) or len(value['error']) > 256):
            return None
        # Preference fields are optional; publisher provenance is mandatory.
        for key in ('confirmed', 'restoring', 'confirmedActive'):
            identifier = value.get(key, '')
            if not isinstance(identifier, str) or identifier and not UUID.fullmatch(identifier):
                return None
        if ('idle' in value and type(value['idle']) is not bool or
                value.get('confirmedActive', '') not in {'', *(choice['id'] for choice in choices)}):
            return None
        preference_error = value.get('preferenceError', '')
        if not isinstance(preference_error, str) or len(preference_error) > 256:
            return None
        palettes = value.get('palettes', [])
        contents = value.get('contents', [])
        palette_id = re.compile(r'[A-Za-z0-9_.-]{1,64}')
        if (not isinstance(palettes, list) or len(palettes) > 64 or
                any(not isinstance(c, dict) or set(c) != {'id', 'name'} or
                    not isinstance(c['id'], str) or not palette_id.fullmatch(c['id']) or
                    not isinstance(c['name'], str) or len(c['name']) > 256 for c in palettes) or
                len({c['id'] for c in palettes}) != len(palettes) or
                value.get('activePalette', '') not in {'', *(c['id'] for c in palettes)} or
                not isinstance(contents, list) or len(contents) > 128 or
                any(not isinstance(c, dict) or set(c) not in (
                    {'id', 'zone', 'name', 'active'}, {'id', 'zone', 'name', 'active', 'available'}) or
                    not isinstance(c['id'], str) or not UUID.fullmatch(c['id']) or
                    not isinstance(c['zone'], str) or not UUID.fullmatch(c['zone']) or
                    not isinstance(c['name'], str) or len(c['name']) > 256 or
                    type(c['active']) is not bool or
                    type(c.get('available', True)) is not bool for c in contents) or
                len({(c['zone'], c['id']) for c in contents}) != len(contents)):
            return None
        return value
    except (OSError, ValueError, KeyError, TypeError, RecursionError):
        return None


def select(path, identifier, owner=None, publisher=None, *, palette=None, zone=None, component=None):
    state = snapshot(path, owner, publisher)
    if not state or not state['ready'] or not state.get('idle', not state['transitioning']):
        raise ValueError('Connect CarPlay Ultra before choosing a cluster style')
    if not isinstance(identifier, str) or identifier not in {choice['id'] for choice in state['choices']}:
        raise ValueError('Choose a style from the installed pack')
    if palette is not None and (not isinstance(palette, str) or
                                palette not in {c['id'] for c in state.get('palettes', [])}):
        raise ValueError('Choose a colour from the installed pack')
    if (zone is not None and not isinstance(zone, str) or
            component is not None and not isinstance(component, str) or
            (zone is None) != (component is None)):
        raise ValueError('Choose centre content from the installed pack')
    if component is not None and (
            identifier != state['active'] or (zone, component) not in
             {(c['zone'], c['id']) for c in state.get('contents', [])}):
        raise ValueError('Choose centre content for the current cluster style')
    if component is not None and any(
            c['zone'] == zone and c['id'] == component and not c.get('available', True)
            for c in state.get('contents', [])):
        raise ValueError('This centre content is unavailable')
    if palette is not None or component is not None:
        request = (f"HUL2 {state['nonce']} {state['connectionGeneration']}\n{identifier}\n"
                   f"{palette or ''}\n{zone or ''}\n{component or ''}\n")
    else:
        request = f"HUL1 {state['nonce']} {state['connectionGeneration']}\n{identifier}\n"
    parent = path.parent.stat()
    if parent.st_uid != (owner[0] if owner else os.getuid()) or parent.st_mode & 0o022:
        raise ValueError('Cluster layout directory must be private')
    fd, temporary = tempfile.mkstemp(prefix='.cluster-layout-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='ascii') as output:
            os.fchmod(output.fileno(), 0o600)
            if owner is not None:
                os.fchown(output.fileno(), *owner)
            output.write(request)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def navigate(path, command, owner=None, publisher=None):
    """One connection-bound, expiring button request; never claims phone execution."""
    if not isinstance(command, str) or command not in ('next', 'previous', 'select', 'back', 'up', 'down',
                                                       'play_pause', 'next_track', 'previous_track',
                                                       'open_maps', 'open_music', 'open_phone'):
        raise ValueError('Choose a cluster navigation button')
    state = snapshot(path, owner, publisher)
    if not state or not state['ready'] or not state.get('idle', not state['transitioning']):
        raise ValueError('Wait for CarPlay Ultra before navigating the cluster')
    directory = temporary_fd = None
    temporary = None
    try:
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
        before = os.fstat(directory)
        named = path.parent.lstat()
        if (before.st_uid != (owner[0] if owner else os.getuid()) or
                stat.S_IMODE(before.st_mode) != 0o700 or
                (before.st_dev, before.st_ino) != (named.st_dev, named.st_ino)):
            raise ValueError('Cluster input directory must be private')
        sampled = clock_ns()
        request = (f"HUI1\n{state['nonce']}\n{state['connectionGeneration']}\n"
                   f"{sampled}\n{sampled}\n{command}\n").encode('ascii')
        # Pin both temporary creation and replacement to the inspected directory.
        import secrets
        temporary = '.cluster-input-' + secrets.token_hex(16)
        temporary_fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                               os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=directory)
        if owner is not None:
            os.fchown(temporary_fd, *owner)
        if os.write(temporary_fd, request) != len(request):
            raise ValueError('Cluster input write was incomplete')
        named = path.parent.lstat()
        if (before.st_dev, before.st_ino) != (named.st_dev, named.st_ino):
            raise ValueError('Cluster input directory changed')
        os.replace(temporary, path.name + '.input', src_dir_fd=directory, dst_dir_fd=directory)
        temporary = None
    finally:
        if temporary_fd is not None:
            os.close(temporary_fd)
        if temporary is not None and directory is not None:
            os.unlink(temporary, dir_fd=directory)
        if directory is not None:
            os.close(directory)


NATIVE_NAMES = {'map': 'Map', 'media': 'Media', 'vehicle': 'Vehicle', 'trip': 'Trip',
                'minimal': 'Minimal', 'sport': 'Sport', 'gauges': 'Full Gauges'}
# These are presentation labels for declared pack layouts, not new signed layouts.
# Match both ID and authored name; unfamiliar catalogues retain their own names.
SIGNED_FORMATS = {
    ('1B54747B-5609-52E0-A1DD-9CC8EF0B11BE', 'DigiBand'): ('minimal', 'Minimal · DigiBand'),
    ('8BC9ABB1-CB20-595E-92BF-D8F099BF3515', 'DigiRing'): ('sport', 'Sport · DigiRing'),
    ('0D38B526-1271-575D-8A92-393A86B718F9', 'Modular'): ('gauges', 'Full Gauges · Modular'),
    ('DB7C6944-5DFF-58DF-94E9-91006781FA93', 'MapsMedia'): ('maps_media', 'Large Map / Media'),
}


def format_snapshot(native_page, map_available, ultra):
    """Accept only the already validated fresh cluster_layout snapshot (or None)."""
    native = [{'id': page, 'name': NATIVE_NAMES[page], 'page': page}
              for page in native_pages(map_available)] if native_page is not None else []
    signed = []
    if ultra:
        for choice in ultra['choices']:
            identifier, name = SIGNED_FORMATS.get((choice['id'], choice['name']),
                                                  (choice['id'], choice['name']))
            signed.append({'id': identifier, 'name': name, 'layout': choice['id'],
                           'authoredName': choice['name']})
    def format_id(layout):
        return next((choice['id'] for choice in signed if choice['layout'] == layout), None)
    return {
        'native': {'choices': native, 'selected': native_page, 'selectionKind': 'saved_preference'},
        'ultra': {'choices': signed,
                  'selectable': bool(ultra and ultra['ready'] and
                                     ultra.get('idle', not ultra['transitioning'])),
                  'active': format_id(ultra.get('active')) if ultra else None,
                  'confirmed': format_id(ultra.get('confirmedActive')) if ultra else None,
                  'pending': format_id(ultra.get('pending')) if ultra else None,
                  'error': ultra.get('error', '') if ultra else ''}}


def format_choice(model, renderer, identifier):
    if renderer not in ('native', 'ultra') or not isinstance(identifier, str):
        raise ValueError('Choose a native or Ultra format from the current catalogue')
    if renderer == 'ultra' and not model['ultra']['selectable']:
        raise ValueError('Wait for the current CarPlay Ultra scene before choosing a format')
    for item in model[renderer]['choices']:
        if item['id'] == identifier:
            return item
    raise ValueError('Cluster format is unavailable; refresh the catalogue')


class ContentControl:
    """Manager-owned, connection-bound Remote UI requests; never an active highlight."""
    def __init__(self, settings_profile, state_directory, owner=None):
        self.path = None
        self.choices = []
        self.owner = owner
        self.sequence = 0
        self.selected = None
        self.publisher = None
        self.next_refresh = 0
        if not settings_profile:
            return
        fd = os.open(settings_profile, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= 65536 or
                    info.st_mode & 0o022 or info.st_uid not in (0, owner[0] if owner else os.getuid())):
                raise ValueError('Unsafe vehicle-content capability file')
            raw = os.read(fd, 65537)
            after = os.fstat(fd)
            named = os.stat(settings_profile)
            def identity(value):
                return (value.st_dev, value.st_ino, value.st_size, value.st_mode,
                        value.st_uid, value.st_mtime_ns, value.st_ctime_ns)
            if len(raw) != info.st_size or identity(info) != identity(after) or identity(info) != identity(named):
                raise ValueError('Vehicle-content capability file changed during read')
        finally:
            os.close(fd)
        document = json.loads(raw)
        if not isinstance(document, dict):
            raise ValueError('Vehicle-content capability root must be an object')
        content = document.get('cluster_content', {})
        if not isinstance(content, dict):
            raise ValueError('Vehicle-content capabilities must be an object')
        path = content.get('request_file')
        if not path:
            return
        if document.get('version') != 1 or document.get('experimental') is not True:
            raise ValueError('Vehicle-content requests require explicit experimental capabilities')
        pages = content.get('pages', [])
        names = {'body': 'Seats and doors', 'tyres': 'Tyre status', 'charging': 'Charging'}
        if (not isinstance(path, str) or len(path.encode()) > 128 or
                not isinstance(pages, list) or not 1 <= len(pages) <= 16):
            raise ValueError('Invalid vehicle-content request configuration')
        endpoint = Path(path)
        if (not endpoint.is_absolute() or endpoint.parent != Path(state_directory) or
                any(part in ('.', '..') for part in path.split('/')) or
                endpoint.name != 'cluster-content.request' or
                any(ord(c) < 32 or ord(c) == 127 for c in path)):
            raise ValueError('Vehicle-content request must be a private manager-state endpoint')
        for page in pages:
            if (not isinstance(page, dict) or set(page) - {'id', 'widget', 'panel', 'zone', 'dismissible'} or
                    not isinstance(page.get('id'), str) or
                    not re.fullmatch(r'[A-Za-z][A-Za-z0-9_.-]{0,95}', page['id']) or
                    page.get('widget') not in names or page.get('panel') != 'Cluster_Display' or
                    page.get('zone') not in ('zone1', 'zone2') or
                    type(page.get('dismissible', True)) is not bool or
                    page.get('dismissible', True) is not True):
                raise ValueError('Vehicle-content page has no established cluster contract')
            self.choices.append({'id': page['id'], 'name': names[page['widget']]})
        if len({c['id'] for c in self.choices}) != len(self.choices):
            raise ValueError('Duplicate vehicle-content page identifier')
        if len({(p['panel'], p['zone']) for p in pages}) != 1:
            raise ValueError('Vehicle-content requests must use one display zone')
        self.path = endpoint

    def _write(self, identifier, publisher):
        if not self.path:
            raise ValueError('Vehicle-content requests are not configured')
        nonce, generation = publisher
        if (not isinstance(nonce, str) or not re.fullmatch(r'[0-9a-f]{32}', nonce) or
                decimal(generation, True) == 0):
            raise ValueError('Vehicle-content request needs the current connection publisher')
        parent = self.path.parent.lstat()
        if (not stat.S_ISDIR(parent.st_mode) or parent.st_mode & 0o077 or
                parent.st_uid != (self.owner[0] if self.owner else os.getuid())):
            raise ValueError('Vehicle-content request directory must be private')
        renewed = clock_ns() // 1000000
        request = f'HUC1 {nonce} {generation} {self.sequence} {renewed} {renewed + 2000}\n{identifier or "-"}\n'
        fd, temporary = tempfile.mkstemp(prefix='.cluster-content-', dir=self.path.parent)
        try:
            with os.fdopen(fd, 'w', encoding='ascii') as output:
                os.fchmod(output.fileno(), 0o600)
                if self.owner is not None:
                    os.fchown(output.fileno(), *self.owner)
                output.write(request)
            final_parent = self.path.parent.lstat()
            if (parent.st_dev, parent.st_ino) != (final_parent.st_dev, final_parent.st_ino):
                raise ValueError('Vehicle-content directory changed during publication')
            os.replace(temporary, self.path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
        self.next_refresh = renewed + 500

    def request(self, identifier, publisher):
        if identifier is not None and (type(identifier) is not str or identifier not in {c['id'] for c in self.choices}):
            raise ValueError('Choose vehicle content from the configured pages')
        if self.sequence >= 2**64 - 1:
            raise ValueError('Vehicle-content command sequence exhausted')
        self.sequence += 1
        self._write(identifier, publisher)
        self.selected, self.publisher = identifier, publisher

    def clear(self):
        previous = self.publisher
        self.selected = None
        self.publisher = None
        if previous:
            self.request(None, previous)
            self.publisher = None

    def maintain(self, publisher):
        if self.publisher and self.publisher != publisher:
            self.clear()
        if self.selected and publisher == self.publisher and clock_ns() // 1000000 >= self.next_refresh:
            self._write(self.selected, publisher)

    def snapshot(self):
        return {'configured': bool(self.path), 'choices': self.choices,
                'requested': self.selected, 'confirmed': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('layout', nargs='?', help='Layout UUID; omit to list available styles')
    args = parser.parse_args()
    path = configured_path(args.profile)
    if args.layout:
        try:
            select(path, args.layout)
        except ValueError as error:
            parser.error(str(error))
    else:
        print(json.dumps(snapshot(path), indent=2))
