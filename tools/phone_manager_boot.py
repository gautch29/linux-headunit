# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Read-only admission for the optional supervised native phone manager."""
import json
import os
from pathlib import Path
import stat
import subprocess

from phone_manager_configuration import load_configuration, validate_manager_settings
from receiver_configuration import load_receiver_configuration


def _signature(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns,
            info.st_ctime_ns, info.st_uid, info.st_mode, info.st_nlink)


def _static_file(path, uid, maximum=16384):
    path = Path(path)
    info = path.lstat()
    if (path.resolve(strict=True) != path or not stat.S_ISREG(info.st_mode) or
            info.st_uid not in (0, os.getuid(), uid) or info.st_mode & 0o7022 or
            info.st_nlink != 1 or not 0 < info.st_size <= maximum):
        raise ValueError('Manager startup settings require bounded real trusted static files')
    return _signature(info)


def _displays(path, uid, checker):
    before = _static_file(path, uid)
    result = subprocess.run([str(checker), str(path), '--json'], check=True,
                            capture_output=True, text=True, timeout=5)
    if _signature(Path(path).lstat()) != before:
        raise ValueError('Manager display configuration changed during validation')
    value = json.loads(result.stdout)
    if not isinstance(value, list) or len(value) != 2:
        raise ValueError('Display checker did not return both manager panels')
    for panel in value:
        if (not isinstance(panel, dict) or
                any(type(panel.get(key)) is not int for key in ('width', 'height')) or
                any(not isinstance(panel.get(key), str) or not panel[key]
                    for key in ('wayland', 'output'))):
            raise ValueError('Manager panels need explicit valid Wayland sockets and outputs')
    return value


def inspect_settings(profile_path, settings, uid, checker=None):
    """Validate without starting radios, a manager, a child or runtime endpoints."""
    before = _static_file(profile_path, uid)
    profile = validate_manager_settings(load_configuration(profile_path, uid), settings)
    if _signature(Path(profile_path).lstat()) != before:
        raise ValueError('Manager startup profile changed during validation')
    if not settings.get('display_config') or not settings.get('bin_dir'):
        raise ValueError('Supervised phone manager needs display_config and bin_dir')
    checker = checker or Path(settings['bin_dir']) / 'headunit-display-config-check'
    current = _displays(settings['display_config'], uid, checker)
    standard = _displays(profile['standardDisplayConfig'], uid, checker)
    def route(panel):
        return panel['wayland'], panel['output']
    if route(current[0]) != route(standard[0]):
        raise ValueError('Standard and Ultra main views must use the same supervised display')
    if ((route(current[1]), current[1]['width'], current[1]['height']) !=
            (route(standard[1]), standard[1]['width'], standard[1]['height'])):
        raise ValueError('Standard and Ultra clusters must share the backup output and canvas')
    if route(current[0]) == route(current[1]):
        raise ValueError('Supervised main and cluster outputs must be distinct')
    return profile


def inspect_configuration(config_path, profile_path, uid, checker=None):
    before = _static_file(config_path, uid)
    settings = load_receiver_configuration(Path(config_path))
    if _signature(Path(config_path).lstat()) != before:
        raise ValueError('Manager receiver configuration changed during validation')
    return inspect_settings(profile_path, settings, uid, checker)


def prepare_directory(directory, owner):
    """Create only the selected private state directory; never take over an owner."""
    directory = Path(directory)
    if not directory.is_absolute() or directory.resolve() != directory:
        raise ValueError('Manager directory needs a real canonical absolute path')
    try:
        directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    except FileExistsError:
        pass
    else:
        if os.geteuid() == 0:
            os.chown(directory, *owner, follow_symlinks=False)
    info = directory.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != owner[0] or
            stat.S_IMODE(info.st_mode) != 0o700 or directory.resolve() != directory):
        raise ValueError('Manager directory must be real, owned and 0700')
    return directory
