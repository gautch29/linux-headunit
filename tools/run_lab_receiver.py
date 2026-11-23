#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Bounded, independent AP + discovery + receiver test on a Linux bench.

Stop competing receiver and AP processes before running as root. This
launcher uses the project C++ binaries and system hostapd/dnsmasq. It
retires its processes and addresses on exit or SIGTERM.
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import pwd
import secrets
import shutil
import signal
import socket
import sys
import stat
import subprocess
import tempfile
import time

from generate_test_ap import main as generate_ap_config
from receiver_configuration import (load_receiver_configuration, DEVICE_FIELDS,
                                    validate_device_settings, configured_identity,
                                    validate_standard_camera_settings, parse_receiver_configuration,
                                    validate_native_page_settings, validate_runtime_status_settings)

PREVIEW_SINKS = ("none", "x11", "wayland", "wayland-touch", "wayland-gpu", "fake")
WAYLAND_PREVIEW_SINKS = ("wayland", "wayland-touch", "wayland-gpu")
STANDARD_CAMERA_ENVIRONMENT = ("HEADUNIT_STANDARD_CAMERA_CONTROL", "HEADUNIT_STANDARD_CAMERA_LEASE")
NATIVE_PAGE_ENVIRONMENT = ('HEADUNIT_NATIVE_PAGE_CONTROL', 'HEADUNIT_NATIVE_PAGE_SURFACE_LEASE')
PRESENTATION_ENVIRONMENT = ('HEADUNIT_PRESENTATION_STATUS_FILE', 'HEADUNIT_PRESENTATION_STATUS_NONCE')
MANAGED_ARGUMENTS = {'--manager-profile', '--managed-phone', '--manager-lock-fd',
                     '--managed-audio-volume-percent', '--managed-audio-muted', '--managed-runtime-status-nonce'}


def sealed_manager_configuration(config, manager_profile, binding, uid=None, checker=None):
    """Read release-owned inputs; mutable per-phone JSON is never executable input."""
    from appliance_release import config_path, read_configuration_members, ui_fields, unique
    from phone_manager_boot import inspect_settings
    from phone_manager_configuration import validate_manager_settings
    # Capture the related profiles as one graph. Rehashing all executables for
    # every individual configuration read adds serial boot work without a new
    # admission boundary. The batch reader fully verifies before and after.
    manifest, captured = read_configuration_members(binding)
    root = Path(binding['path'])
    if (manifest.get('version') not in (3, 4, 5) or manifest.get('execution', {}).get('receiver') != 'phone-manager' or
            not {'receiver', 'phone_manager', 'standard_displays'} <= set(manifest['configuration'])):
        raise ValueError('Managed receiver execution requires a verified managed phone-manager release')
    if uid is not None and uid != manifest['uid']:
        raise ValueError('Manager display account differs from the bound release')
    expected_profile = config_path(root, 'phone_manager')
    if Path(manager_profile) != expected_profile or Path(config) != config_path(root, 'receiver'):
        raise ValueError('Manager profiles must be the exact bound configuration roles')
    raw = captured['receiver']
    settings = parse_receiver_configuration(raw, Path(config))
    if settings.get('bin_dir') != root / 'build':
        raise ValueError('Receiver binaries must belong to their bound release')
    profile_raw = captured['phone_manager']
    captured_profile = validate_manager_settings(json.loads(profile_raw, object_pairs_hook=unique), settings)
    profile = inspect_settings(manager_profile, settings, manifest['uid'], checker)
    if profile != captured_profile:
        raise ValueError('Manager admission did not describe the captured release profile')
    if not profile['enabled'] or Path(profile['standardDisplayConfig']) != config_path(root, 'standard_displays'):
        raise ValueError('Managed receiver needs its enabled release-owned Standard display profile')
    if manifest['version'] != 5 and any(key in settings for key in ('camera_manual_profile', 'camera_state_config',
                                       'standard_camera_control_file', 'standard_camera_lease_file')):
        raise ValueError('Camera coexistence is not admitted in managed application releases')
    if settings.get('ultra_installed_ui'):
        if ('ultra' not in captured or
                Path(settings['ultra_installed_ui']) != config_path(root, 'ultra')):
            raise ValueError('Ultra profile must be the exact bound configuration role')
        fields = ui_fields(captured['ultra'])
        if manifest['version'] != 5 and any(fields.get(key) for key in ('cameraProfile', 'cameraControlFile', 'nativeCameraControlFile', 'nativeCameraLeaseFile')):
            raise ValueError('Ultra camera coexistence is not admitted in managed application releases')
    # inspect_settings invokes the production checker. Pin both captures again
    # afterward; its file reads must describe the same release generation.
    _, current = read_configuration_members(binding)
    if raw != current['receiver'] or profile_raw != current['phone_manager']:
        raise ValueError('Managed release configuration changed during admission')
    return manifest, settings, captured_profile


def verify_manager_lock(descriptor, directory, owner_uid):
    """Prove this inherited open-file description holds the exact manager lock."""
    if type(descriptor) is not int or descriptor < 3:
        raise ValueError('Managed launcher needs an inherited manager ownership descriptor')
    directory = Path(directory)
    before = directory.lstat()
    if (directory.resolve(strict=True) != directory or not stat.S_ISDIR(before.st_mode) or
            before.st_uid != owner_uid or (before.st_mode & 0o7777) != 0o700):
        raise ValueError('Manager ownership directory must be real, owned and 0700')
    parent = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    contender = -1
    try:
        def valid_lock(info):
            return (stat.S_ISREG(info.st_mode) and info.st_uid == owner_uid and
                    (info.st_mode & 0o7777) == 0o600 and info.st_nlink == 1 and info.st_size == 0)
        def same(a, b):
            return (a.st_dev, a.st_ino) == (b.st_dev, b.st_ino)
        opened_parent = os.fstat(parent)
        if not same(before, opened_parent):
            raise ValueError('Manager ownership directory changed during open')
        named = os.stat('manager.lock', dir_fd=parent, follow_symlinks=False)
        inherited = os.fstat(descriptor)
        if not valid_lock(named) or not valid_lock(inherited) or not same(named, inherited):
            raise ValueError('Inherited descriptor is not the exact private manager.lock')
        contender = os.open('manager.lock', os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
        if not same(os.fstat(contender), inherited):
            raise ValueError('Manager lock changed during ownership proof')
        try:
            fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            raise ValueError('Manager ownership lock is not held')
        # A different descriptor held by another process cannot satisfy this.
        # No LOCK_UN is used: the manager and launcher share this description.
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('Inherited descriptor does not own the manager lock') from error
        current_parent = directory.lstat()
        current = os.stat('manager.lock', dir_fd=parent, follow_symlinks=False)
        if (not same(current_parent, before) or directory.resolve(strict=True) != directory or
                current_parent.st_uid != owner_uid or (current_parent.st_mode & 0o7777) != 0o700 or
                not valid_lock(current) or not same(current, inherited)):
            raise ValueError('Manager lock generation changed during ownership proof')
    finally:
        if contender >= 0:
            os.close(contender)
        os.close(parent)
    return descriptor


def managed_receiver_settings(base, profile, phone, volume=None, muted=None, runtime_nonce=None,
                              allow_signed_camera=False):
    """Only phone identity and desired head-unit gain vary a sealed receiver."""
    from phone_state import address, receiver_settings
    if address(phone) != phone:
        raise ValueError('Managed phone must be a canonical bounded selector')
    if (volume is None) != (muted is None):
        raise ValueError('Managed audio gain and mute must be supplied together')
    if volume is not None and (type(volume) is not int or not 0 <= volume <= 100 or type(muted) is not bool):
        raise ValueError('Managed audio preference must be gain 0..100 and boolean mute')
    if (any(key in base for key in ('standard_camera_control_file', 'standard_camera_lease_file')) or
            (not allow_signed_camera and any(key in base for key in ('camera_manual_profile', 'camera_state_config')))):
        raise ValueError('Managed release camera configuration is not admitted')
    folder = Path(profile['stateDirectory'])
    if any(key in base for key in ('runtime_status_file', 'runtime_status_nonce')):
        raise ValueError('Sealed receiver readiness is derived per launch, never stored in the release')
    result = receiver_settings(base, phone, profile['standardDisplayConfig'], folder / 'media.json')
    result['direct_media'] = False  # The manager is the single media publisher.
    if base.get('audio_sink', 'none') != 'none':
        result['audio_control'], result['audio_state'] = folder / 'audio-control.json', folder / 'audio-state.json'
        if volume is not None:
            result['audio_volume_percent'], result['audio_muted'] = volume, muted
    elif volume is not None:
        raise ValueError('Managed audio preferences require a sealed audio sink')
    if runtime_nonce is not None:
        result['runtime_status_file'] = folder / 'carplay-status.json'
        result['runtime_status_nonce'] = runtime_nonce
        validate_runtime_status_settings(result)
    return result


def presentation_environment(environment, settings):
    """Never inherit another manager's status target or launch nonce."""
    validate_runtime_status_settings(settings)
    result = dict(environment)
    for key in PRESENTATION_ENVIRONMENT:
        result.pop(key, None)
    if settings.get('runtime_status_file') is not None:
        result[PRESENTATION_ENVIRONMENT[0]] = str(settings['runtime_status_file'])
        result[PRESENTATION_ENVIRONMENT[1]] = settings['runtime_status_nonce']
    return result


def navigation_environment(environment, settings):
    """Optional HUD subscription target; never inherit another launch's path."""
    result = dict(environment)
    result.pop('HEADUNIT_NAVIGATION_SOCKET', None)
    if settings.get('navigation_socket') is not None:
        validate_device_settings(settings)
        result['HEADUNIT_NAVIGATION_SOCKET'] = str(settings['navigation_socket'])
    return result


def vehicle_capability_environment(environment, settings):
    """Only explicit capability files reach the receiver; scrub inherited paths."""
    result = dict(environment)
    for key in ('HEADUNIT_VEHICLE_STATE_PROFILE', 'HEADUNIT_CLIMATE_CONFIG',
                'HEADUNIT_CAR_SETTINGS_CONFIG', 'HEADUNIT_CAMERA_STATE_CONFIG',
                'HEADUNIT_NATIVE_CLUSTER_CONFIG', 'HEADUNIT_GREETING_SEQUENCE_FILE',
                'HEADUNIT_VEHICLE_STATE_BACKEND', 'HEADUNIT_VEHICLE_TRACE_WRITES'):
        result.pop(key, None)
    for field, variable in (('climate_config', 'HEADUNIT_CLIMATE_CONFIG'),
                            ('car_settings_config', 'HEADUNIT_CAR_SETTINGS_CONFIG'),
                            ('camera_state_config', 'HEADUNIT_CAMERA_STATE_CONFIG'),
                            ('native_cluster_config', 'HEADUNIT_NATIVE_CLUSTER_CONFIG'),
                            ('greeting_sequence_file', 'HEADUNIT_GREETING_SEQUENCE_FILE')):
        if settings.get(field):
            result[variable] = str(settings[field])
    if settings.get('vehicle_state_backend', 'none') != 'none':
        result['HEADUNIT_VEHICLE_STATE_BACKEND'] = settings['vehicle_state_backend']
    if settings.get('vehicle_trace_writes'):
        result['HEADUNIT_VEHICLE_TRACE_WRITES'] = '1'
    if settings.get('vehicle_state_profile', 'none') != 'none':
        result['HEADUNIT_VEHICLE_STATE_PROFILE'] = settings['vehicle_state_profile']
    return result


def check_vehicle_capabilities(settings, owner):
    """Check display-owned endpoints as the admitted receiver, even under a root launcher."""
    if not any(settings.get(k) for k in ('climate_config', 'car_settings_config',
                                       'camera_state_config', 'native_cluster_config')):
        return
    identity = {}
    if os.geteuid() == 0:
        identity = {'user': owner[0], 'group': owner[1], 'extra_groups': ()}
    command = [str(Path(settings['bin_dir']) / 'headunit-vehicle-config-check'),
               *(str(settings.get(k) or '-') for k in ('climate_config', 'car_settings_config',
                                                       'camera_state_config'))]
    if settings.get('native_cluster_config'):
        command += ['--native-cluster', str(settings['native_cluster_config'])]
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=10, **identity)


def native_page_environment(environment, settings, display_fields):
    """Use only configured handoff endpoints; never inherit another owner."""
    validate_native_page_settings(settings)
    result = dict(environment)
    for key in NATIVE_PAGE_ENVIRONMENT:
        result.pop(key, None)
    if settings.get('native_page_control_file') is not None:
        if not display_fields.get('main.wayland') or not display_fields.get('main.output'):
            raise ValueError('Native page needs an explicit named main Wayland route')
        if not settings.get('ultra_negotiate', False) or settings.get('standard_display_config'):
            for setting, variable in zip(('native_page_control_file', 'native_page_lease_file'), NATIVE_PAGE_ENVIRONMENT):
                result[variable] = str(settings[setting])
    return result


def native_page_ui(raw, settings):
    """Bounded derived UI bytes; the sealed/original profile remains untouched."""
    validate_native_page_settings(settings)
    if len(raw) > 8192:
        raise ValueError('Installed UI profile exceeds its read bound')
    from appliance_release import ui_fields
    fields = ui_fields(raw)
    if settings.get('native_page_control_file') is None:
        if any(key in fields for key in ('nativePageControlFile', 'nativePageLeaseFile')):
            raise ValueError('Installed UI page handoff requires the explicit receiver pair')
        return raw
    if any(fields.get(key) for key in ('nativeCameraControlFile', 'nativeCameraLeaseFile')):
        raise ValueError('Native page handoff cannot combine installed UI camera owners')
    if any(key in fields for key in ('cameraProfile', 'cameraControlFile')) and not (fields.get('cameraProfile') and fields.get('cameraControlFile') and settings.get('camera_state_config')):
        raise ValueError('Native page signed camera requires its complete source/state/request graph')
    pair = {'nativePageControlFile': str(settings['native_page_control_file']),
            'nativePageLeaseFile': str(settings['native_page_lease_file'])}
    if any(key in fields for key in pair) and any(fields.get(key) != value for key, value in pair.items()):
        raise ValueError('Installed UI handoff pair differs from the receiver configuration')
    fields.update(pair)
    result = ''.join(key + '=' + value + '\n' for key, value in fields.items()).encode()
    if len(result) > 8192:
        raise ValueError('Derived installed UI profile exceeds its read bound')
    return result


def launch_settings(selected, arguments):
    """Pure static admission, apart from proving an already inherited lock."""
    binding = None
    runtime_nonce = getattr(selected, 'managed_runtime_status_nonce', None)
    selectors = (selected.manager_profile, selected.managed_phone, selected.manager_lock_fd)
    managed = any(value is not None for value in selectors) or runtime_nonce is not None
    audio = selected.managed_audio_volume_percent is not None or selected.managed_audio_muted is not None
    if bool(selected.release_root) != bool(selected.release_manifest_sha256):
        raise ValueError('A release root and manifest digest must be supplied together')
    if not selected.release_root:
        if managed or audio:
            raise ValueError('Managed selectors require a bound application release')
        return (load_receiver_configuration(selected.config) if selected.config else {}), None
    from appliance_release import verify, config_path, read_configuration_member
    binding = {'path': str(selected.release_root), 'sha256': selected.release_manifest_sha256}
    manifest = verify(binding)
    if manifest['version'] == 2:
        raise ValueError('Retained version 2 execution must use its own sealed launcher')
    allowed = {'--config', '--release-root', '--release-manifest-sha256', '--dry-run'}
    if manifest.get('version') in (3, 4, 5):
        allowed |= MANAGED_ARGUMENTS
    seen = set()
    for argument in arguments:
        if argument.startswith('--'):
            flag = argument.split('=', 1)[0]
            if flag not in allowed or flag in seen:
                raise ValueError('Sealed release settings cannot be overridden or repeated on the command line')
            seen.add(flag)
    if not selected.config:
        raise ValueError('Receiver configuration must belong to its bound release')
    raw = read_configuration_member(selected.config, binding, 'receiver')
    saved = parse_receiver_configuration(raw, selected.config)
    if any(key in saved for key in ('runtime_status_file', 'runtime_status_nonce')):
        raise ValueError('Bound receiver readiness is derived per launch, never stored in the release')
    if saved.get('bin_dir') != selected.release_root / 'build':
        raise ValueError('Receiver binaries must belong to their bound release')
    if manifest.get('version') not in (3, 4, 5):
        if managed or audio:
            raise ValueError('Managed selectors require a verified managed phone-manager release')
        return saved, binding
    profile_path = selected.manager_profile or config_path(selected.release_root, 'phone_manager')
    manifest, saved, profile = sealed_manager_configuration(selected.config, profile_path, binding)
    if not managed:
        if audio or '--dry-run' not in seen:
            raise ValueError('Managed receiver execution requires the complete managed selector; base preflight is dry-run only')
        return saved, binding
    if any(value is None for value in selectors) or runtime_nonce is None:
        raise ValueError('Supply manager profile, phone, ownership descriptor and fresh readiness nonce together')
    verify_manager_lock(selected.manager_lock_fd, profile['stateDirectory'], manifest['uid'])
    if selected.managed_audio_muted is not None and selected.managed_audio_muted not in ('true', 'false'):
        raise ValueError('Managed mute must be true or false')
    return managed_receiver_settings(saved, profile, selected.managed_phone,
                                     selected.managed_audio_volume_percent,
                                     None if selected.managed_audio_muted is None else selected.managed_audio_muted == 'true', runtime_nonce, allow_signed_camera=manifest['version'] == 5), binding


def wayland_environment(environment, owner_uid, display, runtime_root=Path("/run/user")):
    """Connect only to the selected user's existing Wayland socket."""
    runtime = runtime_root / str(owner_uid)
    runtime_info = runtime.stat()
    if not stat.S_ISDIR(runtime_info.st_mode) or runtime_info.st_uid != owner_uid:
        raise RuntimeError("Wayland runtime directory has an unexpected owner")
    if Path(display).name != display or display in ("", ".", ".."):
        raise ValueError("Wayland display must be a socket name")
    display_info = (runtime / display).stat()
    if not stat.S_ISSOCK(display_info.st_mode) or display_info.st_uid != owner_uid:
        raise RuntimeError("Wayland display is not an owned socket")
    result = dict(environment)
    result["XDG_RUNTIME_DIR"] = str(runtime)
    result["WAYLAND_DISPLAY"] = display
    return result


def wait_for_wayland(owner, display):
    """Radio/AP setup may precede graphics; wait boundedly before consuming it."""
    uid, gid = owner
    environment = dict(os.environ, XDG_RUNTIME_DIR=f"/run/user/{uid}", WAYLAND_DISPLAY=display)
    subprocess.run(python_helper_command(Path(__file__).with_name("wait_wayland.py"),
                                        "--socket", display, "--timeout", "15"),
                   env=environment, user=uid, group=gid, extra_groups=(), check=True, timeout=17,
                   capture_output=True, text=True)


def standard_preview_environment(environment, settings, display_fields, owner_uid):
    """Preflight opt-in Standard GPU routing without creating private files."""
    validate_standard_camera_settings(settings)
    result = dict(environment)
    for variable in STANDARD_CAMERA_ENVIRONMENT:
        result.pop(variable, None)
    if settings.get("preview_sink", "none") == "wayland-gpu":
        if (settings.get("ultra_negotiate", False) or settings.get("ultra_native_video", False)) and not settings.get('standard_display_config'):
            raise ValueError("wayland-gpu is a Standard CarPlay preview backend, without Ultra")
        if not settings.get("display_config") or not display_fields.get("main.wayland") or not display_fields.get("main.output"):
            raise ValueError("wayland-gpu requires display_config with explicit main.wayland and main.output")
    control, lease = (settings.get("standard_camera_control_file"), settings.get("standard_camera_lease_file"))
    if control is None:
        return result
    endpoints = []
    for value, variable, is_lease in ((control, STANDARD_CAMERA_ENVIRONMENT[0], False),
                                     (lease, STANDARD_CAMERA_ENVIRONMENT[1], True)):
        path = Path(value)
        parent_info = path.parent.lstat()
        if (not stat.S_ISDIR(parent_info.st_mode) or parent_info.st_uid != owner_uid or
                stat.S_IMODE(parent_info.st_mode) != 0o700):
            raise ValueError("Standard camera endpoint requires an owned real 0700 parent")
        try:
            info = path.lstat()
        except FileNotFoundError:
            info = None
        if info is not None:
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != owner_uid or
                    stat.S_IMODE(info.st_mode) != 0o600 or
                    (is_lease and (info.st_size != 0 or info.st_nlink != 1)) or
                    (not is_lease and (info.st_nlink != 1 or not 0 <= info.st_size <= 24))):
                raise ValueError("Standard camera endpoint must be an owned single-link 0600 regular file (request: at most 24 bytes; lease: empty)")
        endpoints.append((parent_info.st_dev, parent_info.st_ino, path.name, info))
        result[variable] = str(path)
    first, second = endpoints
    if first[:3] == second[:3] or (first[3] is not None and second[3] is not None and
            (first[3].st_dev, first[3].st_ino) == (second[3].st_dev, second[3].st_ino)):
        raise ValueError("Standard camera request and lease must be distinct files")
    return result


def audio_environment(environment, settings):
    """Apply explicit mixer configuration, clearing every inherited audio flag."""
    validate_device_settings(settings)
    result = dict(environment)
    for key in ('HEADUNIT_AUDIO_SINK', 'HEADUNIT_AUDIO_DEVICE', 'HEADUNIT_AUDIO_CONTROL_FILE',
                'HEADUNIT_AUDIO_STATE_FILE', 'HEADUNIT_AUDIO_VOLUME_PERCENT', 'HEADUNIT_AUDIO_MUTED',
                'HEADUNIT_MICROPHONE_SOURCE', 'HEADUNIT_MICROPHONE_DEVICE',
                'HEADUNIT_AUDIO_OBSERVATION_FILE'):
        result.pop(key, None)
    if settings.get('audio_observation_directory') is not None:
        # Only name a new destination here. The receiver owns exclusive creation,
        # private permissions and all observed dates; launcher preflight does no I/O.
        directory = Path(settings['audio_observation_directory'])
        result['HEADUNIT_AUDIO_OBSERVATION_FILE'] = str(
            directory / ('pcm-' + secrets.token_hex(16) + '.jsonl'))
    if settings.get('audio_sink', 'none') != 'none':
        result['HEADUNIT_AUDIO_SINK'] = settings['audio_sink']
        if settings.get('audio_device'):
            result['HEADUNIT_AUDIO_DEVICE'] = settings['audio_device']
        if settings.get('audio_control'):
            for field, variable in (('audio_control', 'HEADUNIT_AUDIO_CONTROL_FILE'), ('audio_state', 'HEADUNIT_AUDIO_STATE_FILE')):
                path = Path(settings[field]).expanduser()
                result[variable] = str(path.parent.resolve() / path.name)
        if settings.get('audio_volume_percent') is not None:
            result['HEADUNIT_AUDIO_VOLUME_PERCENT'] = str(settings['audio_volume_percent'])
        if settings.get('audio_muted') is not None:
            result['HEADUNIT_AUDIO_MUTED'] = '1' if settings['audio_muted'] else '0'
    if settings.get('microphone_source', 'none') != 'none':
        result['HEADUNIT_MICROPHONE_SOURCE'] = settings['microphone_source']
        if settings.get('microphone_device'):
            result['HEADUNIT_MICROPHONE_DEVICE'] = settings['microphone_device']
    return result


def private_file(path, owner, release_binding=None, role=None):
    if release_binding:
        from appliance_release import configuration_member
        if configuration_member(path, release_binding, role):
            return
        raise ValueError('Configuration is not a member of its verified release')
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
        raise ValueError(f"{path.name} must be a private regular file")
    if info.st_uid != owner:
        raise ValueError(f"{path.name} has an unexpected owner")


def private_directory(path, owner_uid, owner_gid):
    created = not path.exists()
    path.mkdir(mode=0o700, exist_ok=True)
    if created and os.geteuid() == 0:
        os.chown(path, owner_uid, owner_gid)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != owner_uid or \
            info.st_mode & 0o077:
        raise ValueError(f"{path.name} must be an owner-only directory")


def connection_diagnostic_environment(environment, enabled):
    result = dict(environment)
    result.pop("HEADUNIT_CONNECTION_DIAGNOSTICS", None)
    if enabled:
        result["HEADUNIT_CONNECTION_DIAGNOSTICS"] = "1"
    return result


def mfi_environment(environment, settings):
    """One explicit provider; inherited experiments cannot replace the default chip."""
    validate_device_settings(settings)
    result = dict(environment)
    for key in ('HEADUNIT_MFI_BACKEND', 'HEADUNIT_MFI_PRIVATE_KEY', 'HEADUNIT_MFI_CERTIFICATE',
                'HEADUNIT_MFI_DEVICE', 'HEADUNIT_MFI_ADDRESS'):
        result.pop(key, None)
    backend = settings.get('mfi_backend', 'hardware')
    result['HEADUNIT_MFI_BACKEND'] = backend
    if backend == 'software':
        result['HEADUNIT_MFI_PRIVATE_KEY'] = str(settings['mfi_private_key'])
        result['HEADUNIT_MFI_CERTIFICATE'] = str(settings['mfi_certificate'])
    else:
        result['HEADUNIT_MFI_DEVICE'] = settings.get('mfi_device', '/dev/i2c-1')
        result['HEADUNIT_MFI_ADDRESS'] = str(settings.get('mfi_address', 16))
    return result


def check_software_mfi_identity(settings, owner):
    if settings.get('mfi_backend', 'hardware') != 'software':
        return
    options = {'user': owner[0], 'group': owner[1]} if os.geteuid() == 0 else {}
    subprocess.run([str(Path(settings['bin_dir']) / 'headunit-control-probe'),
                    '--check-software-mfi', str(settings['mfi_private_key']),
                    str(settings['mfi_certificate'])],
                   check=True, capture_output=True, text=True, timeout=5, **options)


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True)


def enable_wifi_radio(cancelled):
    """Wait for the existing NetworkManager, then enable Wi-Fi before AP work.

    Managed static admission can overlap NetworkManager startup. No daemon is
    started here; readiness waiting is bounded and never substitutes for the
    subsequent command's success. Bluetooth/AP ownership remains serial.
    """
    deadline = time.monotonic() + 6
    while True:
        if cancelled():
            raise RuntimeError('Wi-Fi preparation cancelled')
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError('NetworkManager did not become ready')
        try:
            result = subprocess.run(['busctl', 'get-property', 'org.freedesktop.NetworkManager',
                                     '/org/freedesktop/NetworkManager', 'org.freedesktop.NetworkManager', 'Version'],
                                    capture_output=True, text=True, timeout=min(1, remaining))
            if result.returncode == 0:
                break
        except subprocess.TimeoutExpired:
            pass
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(.1, remaining))
    # A late property reply cannot bypass the wait budget or cancellation.
    if cancelled():
        raise RuntimeError('Wi-Fi preparation cancelled')
    if time.monotonic() >= deadline:
        raise RuntimeError('NetworkManager did not become ready')
    subprocess.run(['nmcli', 'radio', 'wifi', 'on'], check=True,
                   capture_output=True, text=True, timeout=5)
    if cancelled():
        raise RuntimeError('Wi-Fi preparation cancelled')


def prepare_radios(managed, diagnostics, cancelled):
    """One launcher keeps radio preparation after admission and before AP work."""
    if managed:
        startup_diagnostic(diagnostics, 'wifi-wait-begin')
        enable_wifi_radio(cancelled)
        startup_diagnostic(diagnostics, 'wifi-ready')
    if cancelled():
        raise RuntimeError('Radio preparation cancelled')
    startup_diagnostic(diagnostics, 'bluetooth-begin')
    enable_bluetooth_connections()
    startup_diagnostic(diagnostics, 'bluetooth-ready')


def enable_bluetooth_connections():
    # A powered adapter may still reject every incoming BR/EDR connection.
    # Saved phones need connectability, independently of discovery/pairing mode.
    adapter = ("org.bluez", "/org/bluez/hci0", "org.bluez.Adapter1")
    subprocess.run(["busctl", "set-property", *adapter, "Powered", "b", "true"],
                   check=True, capture_output=True, text=True, timeout=5)
    current = subprocess.run(["busctl", "get-property", *adapter, "Connectable"],
                             capture_output=True, text=True, timeout=5)
    if current.returncode == 0 and current.stdout.strip() == "b true":
        return
    result = subprocess.run(["busctl", "set-property", *adapter,
                             "Connectable", "b", "true"],
                            capture_output=True, text=True, timeout=5)
    if result.returncode:
        # Older BlueZ releases lack Adapter1.Connectable; use the controller's
        # management API without altering discoverability, bonds or trust.
        if not shutil.which("btmgmt"):
            raise RuntimeError("Bluetooth connectability needs Adapter1.Connectable or btmgmt")
        # This launcher/profile currently uses the first controller (hci0).
        # The CM5 btmgmt's numeric --index 0 mode can remain in its event loop;
        # its default-controller command completes normally on this bench.
        subprocess.run(["btmgmt", "connectable", "on"],
                       check=True, capture_output=True, text=True, timeout=5)


def addresses(interface):
    result = json.loads(run("ip", "-json", "address", "show", "dev", interface).stdout)
    if len(result) != 1:
        raise RuntimeError("Wi-Fi interface is unavailable")
    return result[0]


def launch(command, logfile, owner=None, log_owner=None, environment=None):
    output = logfile.open("wb")
    try:
        options = {}
        if log_owner is not None:
            os.fchown(output.fileno(), *log_owner)
        if owner is not None:
            uid, gid = owner
            os.fchown(output.fileno(), uid, gid)
            account = pwd.getpwuid(uid)
            name = account.pw_name
            options = {"user": uid, "group": gid,
                       "extra_groups": os.getgrouplist(name, gid)}
        child_environment = dict(os.environ if environment is None else environment)
        if owner is not None:
            # Dropping uid does not update sudo's inherited home/cache paths.
            child_environment.update(HOME=account.pw_dir, USER=name, LOGNAME=name)
            child_environment["XDG_CACHE_HOME"] = str(Path(account.pw_dir) / ".cache")
            child_environment["XDG_CONFIG_HOME"] = str(Path(account.pw_dir) / ".config")
            child_environment["XDG_DATA_HOME"] = str(Path(account.pw_dir) / ".local/share")
        options["env"] = child_environment
        return subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                stdout=output, stderr=subprocess.STDOUT,
                                start_new_session=True, **options)
    finally:
        output.close()


def stop_process(process):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=3)


def python_helper_command(script, *arguments):
    """New interpreters do not inherit the parent interpreter's -B/-E/-s."""
    return [sys.executable, '-B', '-E', '-s', '-u', str(script), *arguments]


def receiver_owner(identity_info, release_binding=None):
    if not release_binding:
        return identity_info.st_uid, identity_info.st_gid
    from appliance_release import verify
    account = verify(release_binding)
    if identity_info.st_uid != account['uid']:
        raise ValueError('Persistent receiver identity must belong to the release display account')
    # A private identity may legitimately have another file group. It is not
    # the authority for the sealed configuration's runtime read group.
    return account['uid'], account['gid']


def startup_diagnostic(enabled, stage):
    """Fixed launcher boundaries, with real dates and no private input values.

    Optional diagnostics cannot authorize connection/display readiness or make
    a failed launch succeed. There are at most nine calls per launcher attempt.
    """
    if not enabled:
        return
    try:
        date = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        print(f'Headunit startup diagnostic mono_ns={date} stage={stage}', flush=True)
    except Exception:
        pass


def wait_for_ap(process, logfile):
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("hostapd exited before enabling the AP")
        if "AP-ENABLED" in logfile.read_text(errors="replace"):
            return
        time.sleep(0.1)
    raise RuntimeError("hostapd did not report an enabled AP")


def check_identity_configuration(text, checker, owner):
    # Check the exact resolved file as the receiver user before changing radios.
    with tempfile.TemporaryDirectory(prefix="headunit-identity-check-") as folder:
        directory = Path(folder)
        path = directory / "identity.conf"
        path.write_text(text)
        path.chmod(0o600)
        options = {}
        if os.geteuid() == 0:
            os.chown(directory, *owner)
            os.chown(path, *owner)
            options = {"user": owner[0], "group": owner[1]}
        subprocess.run([str(checker), str(path)], check=True,
                       capture_output=True, text=True, **options)


def main():
    os.umask(0o077)
    bootstrap = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    bootstrap.add_argument("--config", type=Path)
    bootstrap.add_argument('--release-root', type=Path)
    bootstrap.add_argument('--release-manifest-sha256')
    bootstrap.add_argument('--manager-profile', type=Path)
    bootstrap.add_argument('--managed-phone')
    bootstrap.add_argument('--manager-lock-fd', type=int)
    bootstrap.add_argument('--managed-audio-volume-percent', type=int)
    bootstrap.add_argument('--managed-audio-muted')
    bootstrap.add_argument('--managed-runtime-status-nonce')
    selected, _ = bootstrap.parse_known_args()
    release_binding = None
    try:
        saved, release_binding = launch_settings(selected, sys.argv[1:])
    except (OSError, ValueError) as error:
        bootstrap.error(str(error))
    parser = argparse.ArgumentParser(description=__doc__, parents=[bootstrap], allow_abbrev=False)
    parser.add_argument("--wifi-config", type=Path)
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--pairing-key", type=Path)
    parser.add_argument("--setup-secret", type=Path)
    parser.add_argument("--pair-store", type=Path)
    parser.add_argument("--capture-root", type=Path)
    parser.add_argument("--bin-dir", type=Path)
    parser.add_argument("--interface", default="wlan0")
    parser.add_argument("--wifi-country", default="FR")
    parser.add_argument("--selected-phone", help="accept iAP only from this phone")
    parser.add_argument("--route-guidance", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--night-mode", help="phone day/night appearance: off, on, schedule[:HH:MM-HH:MM]")
    parser.add_argument("--limited-ui", choices=("speed",),
                        help="driving restrictions from fresh vehicle speed")
    parser.add_argument("--pre-phone-gauges", action=argparse.BooleanOptionalAction, default=False,
                        help="appliance backup uses verified local gauges before phone connection")
    parser.add_argument("--multitouch", action=argparse.BooleanOptionalAction, default=False,
                        help="declare a two-finger touchscreen (pinch/rotate) to the phone")
    parser.add_argument("--oem-icon", action=argparse.BooleanOptionalAction, default=False,
                        help="show the phone's Home (car) icon; tapping it opens native Home")
    parser.add_argument("--route-guidance-component-id", type=int, default=42)
    parser.add_argument("--route-guidance-name", default="Instrument cluster")
    parser.add_argument("--navigation-socket", type=Path, help="private native HUD route-guidance consumer")
    parser.add_argument("--standard-cluster", action=argparse.BooleanOptionalAction, default=False)
    # The phone manager owns this independent window across receiver restarts.
    # Accept the saved field here so mode-specific dry runs use one schema.
    parser.add_argument("--fallback-cluster", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--simulate-vehicle", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--simulate-dated-vehicle", action=argparse.BooleanOptionalAction, default=False,
                        help="explicit dated synthetic source in the existing native cluster owner only")
    parser.add_argument("--vehicle-socket", type=Path)
    parser.add_argument("--vehicle-state-profile", default="none",
                        choices=("none", "empty", "climate-bench", "settings-bench", "bench"),
                        help="opt-in memory-only Apple vehicle-state research profile")
    parser.add_argument("--climate-config", type=Path, help="semantic Climate capability JSON")
    parser.add_argument("--car-settings-config", type=Path, help="semantic car-settings capability JSON")
    parser.add_argument("--camera-state-config", type=Path, help="experimental read-only Ultra exterior-camera state JSON")
    parser.add_argument("--native-cluster-config", type=Path, help="explicit native gauge units, ranges and RPM visibility")
    parser.add_argument("--vehicle-state-backend", default="none", choices=("none", "simulation"),
                        help="explicit capability value provider; physical hardware provider is not installed")
    parser.add_argument("--vehicle-trace-writes", action=argparse.BooleanOptionalAction, default=False,
                        help="log numeric vehicle requests, replies and binding names")
    parser.add_argument("--media-state", type=Path)
    parser.add_argument("--cluster-page", type=Path)
    parser.add_argument("--direct-media", action=argparse.BooleanOptionalAction, default=False,
                        help="publish iAP media for a standalone appliance without the phone manager")
    parser.add_argument("--audio-sink", choices=("none", "fakesink", "alsasink", "pulsesink"),
                        default="none", help="optional 48 kHz AAC media downlink")
    parser.add_argument("--audio-device", help="explicit ALSA device or PulseAudio sink name")
    parser.add_argument("--microphone-source", choices=("none", "alsasrc", "pulsesrc"), default="none")
    parser.add_argument("--microphone-device", help="explicit ALSA capture device or PulseAudio source")
    parser.add_argument("--audio-control", type=Path, help="private route-scoped head-unit volume request file")
    parser.add_argument("--audio-state", type=Path, help="private confirmed head-unit mixer state file")
    parser.add_argument("--audio-observation-directory", type=Path,
                        help="opt-in private directory for one fresh numeric PCM trace per receiver launch")
    parser.add_argument("--audio-volume-percent", type=int, help="initial head-unit mixer gain (0..100)")
    parser.add_argument("--audio-muted", action=argparse.BooleanOptionalAction, default=None,
                        help="initial head-unit mute, independent of phone Bluetooth volume")
    parser.add_argument("--auto-connect-phone", help="paired phone Bluetooth address for bounded automatic requests")
    parser.add_argument("--auto-connect-retry-seconds", type=int, default=15)
    parser.add_argument("--auto-connect-max-attempts", type=int, default=3)
    parser.add_argument("--connection-diagnostics", action=argparse.BooleanOptionalAction, default=False,
                        help="bounded content-free reconnect timing diagnostics")
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--preview-sink", choices=PREVIEW_SINKS,
                        default="none")
    parser.add_argument("--standard-camera-control-file",
                        help="opt-in private absolute Standard camera request path; requires wayland-gpu")
    parser.add_argument("--standard-camera-lease-file",
                        help="private absolute main-surface lease path paired with the Standard camera request")
    parser.add_argument('--native-page-control-file', dest='native_page_control_file')
    parser.add_argument('--native-page-surface-lease-file', dest='native_page_lease_file')
    parser.add_argument('--runtime-status-file')
    parser.add_argument('--runtime-status-nonce')
    parser.add_argument("--preview-decoder", choices=("v4l2h264dec", "avdec_h264"),
                        default="v4l2h264dec",
                        help="Pi 4 hardware H.264 or explicit software H.264 (CM5 bring-up)")
    parser.add_argument("--x-display", default=":10")
    parser.add_argument("--main-width", type=int, default=1280)
    parser.add_argument("--main-height", type=int, default=720)
    parser.add_argument("--wayland-display", default="wayland-0")
    parser.add_argument("--ultra-installed-ui", type=Path,
                        help="experimental verified installed UI consumer; does not advertise Ultra features")
    parser.add_argument("--ultra-native-video", action=argparse.BooleanOptionalAction, default=False,
                        help="route video into installed theme; requires native DMA-buf decoder and source timing")
    parser.add_argument("--ultra-negotiate", action=argparse.BooleanOptionalAction, default=False, help="experimental installed-asset Ultra/HEVC negotiation")
    parser.add_argument("--display-config", type=Path, help="shared main/cluster geometry and output routing")
    parser.add_argument("--standard-display-config", type=Path, help="Standard geometry for phone-selected sessions")
    parser.add_argument("--mfi-device", default="/dev/i2c-1")
    parser.add_argument("--mfi-address", type=int, default=16, help="7-bit I2C address in decimal")
    # Retain explicit research configurations without offering the unfinished
    # software provider in the normal launcher interface.
    parser.add_argument("--mfi-backend", choices=('hardware', 'software'), default='hardware',
                        help=argparse.SUPPRESS)
    parser.add_argument("--mfi-private-key", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--mfi-certificate", type=Path, help=argparse.SUPPRESS)
    for setting in DEVICE_FIELDS:
        parser.add_argument("--" + setting.replace("_", "-"))
    parser.add_argument("--dry-run", action="store_true")
    parser.set_defaults(**saved)
    args = parser.parse_args()
    try:
        validate_device_settings(vars(args))
    except ValueError as error:
        parser.error(str(error))
    for key in ("wifi_config", "identity", "pairing_key", "setup_secret", "pair_store", "capture_root", "bin_dir"):
        if getattr(args, key) is None:
            parser.error(f"missing --{key.replace('_', '-')} or saved setting")
    for key, choices in (("audio_sink", ("none", "fakesink", "alsasink", "pulsesink")),
                         ("preview_sink", PREVIEW_SINKS),
                         ("preview_decoder", ("v4l2h264dec", "avdec_h264"))):
        if getattr(args, key) not in choices:
            parser.error(f"invalid {key}")
    if not 8 <= args.mfi_address <= 119 or not args.mfi_device.startswith("/dev/i2c-"):
        parser.error("invalid MFi bus configuration")
    if args.display_config:
        if any(option.split('=')[0] in ("--main-width", "--main-height") for option in sys.argv[1:]):
            parser.error("edit display_config instead of overriding its dimensions")
        run(str(args.bin_dir / "headunit-display-config-check"), str(args.display_config),
            *(["--ultra"] if args.ultra_negotiate else []))
    if args.standard_cluster and (((args.ultra_negotiate or args.ultra_native_video) and not args.standard_display_config) or args.preview_sink not in ("wayland-touch", "wayland-gpu") or not args.display_config):
        parser.error("native Standard cluster requires Wayland touch and explicit Standard geometry")
    if args.standard_display_config:
        run(str(args.bin_dir / "headunit-display-config-check"), str(args.standard_display_config))
    if args.simulate_dated_vehicle and (args.simulate_vehicle or args.vehicle_socket):
        parser.error("dated simulation excludes legacy simulation and vehicle socket")
    if args.simulate_vehicle and args.vehicle_socket:
        parser.error("choose simulation or a vehicle socket")
    if (args.vehicle_state_profile not in ("none", "empty") or args.climate_config or
            args.car_settings_config or args.camera_state_config) and not args.ultra_negotiate:
        parser.error("Vehicle capabilities require native Ultra negotiation")
    if args.ultra_negotiate:
        if not args.display_config:
            parser.error("Ultra negotiation requires --display-config with physical panel dimensions")
        if not args.ultra_native_video:
            parser.error("Ultra negotiation requires --ultra-native-video")
    if args.ultra_native_video and not args.ultra_installed_ui:
        parser.error("native Ultra video requires --ultra-installed-ui")
    if not 0 <= args.seconds <= 3600:
        parser.error("duration must be 0..3600 seconds (0 runs until stopped)")
    if args.direct_media and not args.media_state:
        parser.error("direct media publishing needs --media-state")
    if not 320 <= args.main_width <= 8192 or not 240 <= args.main_height <= 4320:
        parser.error("main display must be 320..8192 by 240..4320 pixels")
    identity_info = args.identity.lstat()
    try:
        owner = receiver_owner(identity_info, release_binding)
    except ValueError as error:
        parser.error(str(error))
    display_fields = {}
    try:
        check_vehicle_capabilities(vars(args), owner)
    except subprocess.CalledProcessError as error:
        parser.error("vehicle capability files: " + error.stderr.strip())
    except (OSError, subprocess.TimeoutExpired) as error:
        parser.error("vehicle capability checker: " + str(error))
    if args.display_config:
        display_fields = dict(line.strip().split("=", 1) for line in args.display_config.read_text().splitlines() if line.strip() and not line.lstrip().startswith("#"))
    if args.standard_display_config:
        standard_fields = dict(line.strip().split("=", 1) for line in args.standard_display_config.read_text().splitlines() if line.strip() and not line.lstrip().startswith("#"))
        shared = ('main.wayland', 'main.output', 'cluster.wayland', 'cluster.output', 'cluster.width', 'cluster.height')
        if any(standard_fields.get(key) != display_fields.get(key) for key in shared):
            parser.error('Phone-selected modes must share main routing and the native cluster canvas')
    try:
        standard_preview_environment({}, vars(args), display_fields, owner[0])
        native_page_environment({}, vars(args), display_fields)
        presentation_environment({}, vars(args))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    for path in (args.wifi_config, args.identity, args.pairing_key,
                 args.setup_secret):
        private_file(path, owner[0])
    if args.native_cluster_config:
        private_file(args.native_cluster_config, owner[0], release_binding, 'native_cluster')
    try:
        check_software_mfi_identity(vars(args), owner)
    except (OSError, subprocess.SubprocessError):
        parser.error('Software MFi identity preflight failed; check owned 0600 files and matching P-256 inputs')
    if args.ultra_installed_ui:
        private_file(args.ultra_installed_ui, owner[0], release_binding, 'ultra')
    page_ui = None
    if args.ultra_installed_ui:
        if release_binding:
            from appliance_release import read_configuration_member
            source_ui = read_configuration_member(args.ultra_installed_ui, release_binding, 'ultra')
        else:
            from appliance_release import read_file
            source_ui, _ = read_file(args.ultra_installed_ui, 8192, {owner[0]}, private=True)
        configured_ui = native_page_ui(source_ui, vars(args))
        if args.native_page_control_file is not None:
            page_ui = configured_ui
    if args.interface != "wlan0":
        parser.error("the current AP generator supports wlan0 only")
    identity_text = configured_identity(args.identity.read_text(), vars(args))
    check_identity_configuration(identity_text, args.bin_dir / "headunit-identity-config-check", owner)
    with tempfile.TemporaryDirectory(prefix="headunit-ap-check-") as temp:
        generate_ap_config(str(args.wifi_config), temp, args.wifi_country)
    if args.auto_connect_phone:
        for name in ("avahi-browse", "ss"):
            if not shutil.which(name):
                parser.error(f"automatic connection needs {name}")
    if args.dry_run:
        print("Standalone receiver configuration valid; no services started")
        return
    if os.geteuid() != 0:
        parser.error("the lab launcher needs root for Wi-Fi AP configuration")
    for name in ("ip", "hostapd", "dnsmasq", "pgrep", "busctl"):
        if not shutil.which(name):
            parser.error(f"missing system tool: {name}")
    control = args.bin_dir / "headunit-control-probe"
    advertiser = args.bin_dir / "headunit-airplay-advertiser"
    if not control.is_file() or not advertiser.is_file():
        parser.error("build the control receiver and advertiser first")
    if subprocess.run(["pgrep", "-x", "hostapd"],
                      capture_output=True, check=False).returncode == 0:
        parser.error("another hostapd already owns the Wi-Fi interface")
    if subprocess.run(["pgrep", "-x", "dnsmasq"],
                      capture_output=True, check=False).returncode == 0:
        parser.error("another dnsmasq is running")
    if any("dev " + args.interface in line for line in
           run("ip", "route", "show", "default").stdout.splitlines()):
        parser.error("Wi-Fi is the host's default route; use another management link")
    with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        try:
            probe.bind(("::", 7000))
        except OSError:
            parser.error("TCP 7000 is already owned by another receiver")

    private_directory(args.pair_store, *owner)
    private_directory(args.capture_root, *owner)
    session = args.capture_root / ("session-" + time.strftime("%Y%m%d-%H%M%S"))
    session.mkdir(mode=0o700)
    os.chown(session, *owner)
    private_directory(session, *owner)
    # Each process receives the same resolved labels. The persistent identity is
    # untouched, so changing a name never regenerates pairing keys or UUIDs.
    effective_identity = session / "identity.conf"
    effective_identity.write_text(identity_text)
    effective_identity.chmod(0o600)
    os.chown(effective_identity, *owner)
    effective_ui = args.ultra_installed_ui
    if page_ui is not None:
        effective_ui = session / 'installed-ui.conf'
        fd = os.open(effective_ui, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'wb') as output:
            os.fchown(output.fileno(), *owner)
            output.write(page_ui)
    processes = []
    reconnect_process = None
    reconnect_exit_reported = False
    added_ipv4 = False
    added_ipv6 = False
    raised_link = False
    stopping = False

    def on_signal(_number, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGHUP, on_signal)
    try:
        prepare_radios(args.manager_profile is not None, args.connection_diagnostics, lambda: stopping)
        if args.device_name is not None:
            # This is persistent device configuration, not temporary pairing state.
            run("busctl", "set-property", "org.bluez", "/org/bluez/hci0",
                "org.bluez.Adapter1", "Alias", "s", args.device_name)
        generate_ap_config(str(args.wifi_config), str(session / "ap"), args.wifi_country)
        current = addresses(args.interface)
        if "UP" not in current.get("flags", []):
            run("ip", "link", "set", args.interface, "up")
            raised_link = True
        current = addresses(args.interface)
        ipv4 = [entry["local"] for entry in current.get("addr_info", [])
                if entry["family"] == "inet"]
        if ipv4 and ipv4 != ["10.10.0.1"]:
            raise RuntimeError("Wi-Fi interface has an unrelated IPv4 address")
        if not ipv4:
            run("ip", "address", "add", "10.10.0.1/24", "dev", args.interface)
            added_ipv4 = True
        ipv6_link = [entry["local"] for entry in current.get("addr_info", [])
                     if entry["family"] == "inet6" and entry.get("scope") == "link"]
        if not ipv6_link:
            run("ip", "-6", "address", "add", "fe80::1/64", "dev", args.interface)
            added_ipv6 = True

        ap = session / "ap"
        processes.append(launch(["hostapd", str(ap / "hostapd.conf")],
                                session / "hostapd.log", log_owner=owner))
        startup_diagnostic(args.connection_diagnostics, 'ap-started')
        wait_for_ap(processes[-1], session / "hostapd.log")
        startup_diagnostic(args.connection_diagnostics, 'ap-enabled')
        processes.append(launch(["dnsmasq", "--keep-in-foreground",
                                 "--conf-file=" + str(ap / "dnsmasq.conf")],
                                session / "dnsmasq.log", log_owner=owner))
        processes.append(launch([str(advertiser), str(args.seconds),
                                 str(effective_identity), args.interface],
                                session / "advertiser.log", owner))
        control_environment = connection_diagnostic_environment(os.environ, args.connection_diagnostics)
        control_environment = navigation_environment(control_environment, vars(args))
        for key in ("HEADUNIT_ROUTE_GUIDANCE", "HEADUNIT_ROUTE_COMPONENT_ID", "HEADUNIT_ROUTE_COMPONENT_NAME", "HEADUNIT_SELECTED_DEVICE", "HEADUNIT_STANDARD_CLUSTER_DISPLAY", "HEADUNIT_STANDARD_CLUSTER_OUTPUT", "HEADUNIT_MAIN_OUTPUT", "HEADUNIT_MEDIA_STATE", "HEADUNIT_IAP_MEDIA_STATE", "HEADUNIT_VEHICLE_SOCKET", "HEADUNIT_SIMULATE_VEHICLE", "HEADUNIT_NIGHT_MODE", "HEADUNIT_OEM_ICON", "HEADUNIT_MULTITOUCH", "HEADUNIT_LIMITED_UI"):
            control_environment.pop(key, None)
        if display_fields:
            control_environment["HEADUNIT_MAIN_OUTPUT"] = display_fields["main.output"]
        control_environment = standard_preview_environment(control_environment, vars(args), display_fields, owner[0])
        control_environment = native_page_environment(control_environment, vars(args), display_fields)
        control_environment = presentation_environment(control_environment, vars(args))
        control_environment = audio_environment(control_environment, vars(args))
        if args.night_mode:
            control_environment["HEADUNIT_NIGHT_MODE"] = args.night_mode
        if args.oem_icon:
            control_environment["HEADUNIT_OEM_ICON"] = "1"
        if args.multitouch:
            control_environment["HEADUNIT_MULTITOUCH"] = "1"
        if args.limited_ui:
            control_environment["HEADUNIT_LIMITED_UI"] = args.limited_ui
        if args.route_guidance:
            control_environment["HEADUNIT_ROUTE_GUIDANCE"]="1"
            control_environment["HEADUNIT_ROUTE_COMPONENT_ID"]=str(args.route_guidance_component_id)
            control_environment["HEADUNIT_ROUTE_COMPONENT_NAME"]=args.route_guidance_name
        if args.selected_phone:
            control_environment["HEADUNIT_SELECTED_DEVICE"] = "/org/bluez/hci0/dev_" + args.selected_phone.upper().replace(":", "_")
        if args.standard_cluster:
            output = display_fields["cluster.output"]
            if output == display_fields["main.output"] and display_fields["cluster.wayland"] == display_fields["main.wayland"]:
                raise ValueError("Standard native cluster needs a separate compositor output")
            control_environment["HEADUNIT_STANDARD_CLUSTER_DISPLAY"] = display_fields["cluster.wayland"]
            control_environment["HEADUNIT_STANDARD_CLUSTER_OUTPUT"] = output
        if args.media_state:
            control_environment["HEADUNIT_MEDIA_STATE"] = str(args.media_state)
            control_environment["HEADUNIT_IAP_MEDIA_STATE"] = str(args.media_state) + ".iap"
        if args.cluster_page:
            control_environment["HEADUNIT_CLUSTER_PAGE"] = str(args.cluster_page)
        if args.vehicle_socket:
            control_environment["HEADUNIT_VEHICLE_SOCKET"] = str(args.vehicle_socket)
        control_environment = vehicle_capability_environment(control_environment, vars(args))
        if args.simulate_vehicle:
            control_environment["HEADUNIT_SIMULATE_VEHICLE"] = "1"
        control_environment["HEADUNIT_PREVIEW_SINK"] = args.preview_sink
        control_environment["HEADUNIT_PREVIEW_DECODER"] = args.preview_decoder
        control_environment.pop("HEADUNIT_DISPLAY_CONFIG", None)
        control_environment.pop("HEADUNIT_STANDARD_DISPLAY_CONFIG", None)
        if args.display_config:
            control_environment["HEADUNIT_DISPLAY_CONFIG"] = str(args.display_config.resolve())
        if args.standard_display_config:
            control_environment["HEADUNIT_STANDARD_DISPLAY_CONFIG"] = str(args.standard_display_config.resolve())
        control_environment = mfi_environment(control_environment, vars(args))
        control_environment["HEADUNIT_MAIN_WIDTH"] = str(args.main_width)
        control_environment["HEADUNIT_MAIN_HEIGHT"] = str(args.main_height)
        control_environment.pop("HEADUNIT_ULTRA_INSTALLED_UI", None)
        control_environment.pop("HEADUNIT_ULTRA_NATIVE_VIDEO", None)
        control_environment.pop("HEADUNIT_ULTRA_NEGOTIATE", None)
        if args.ultra_negotiate:
            control_environment["HEADUNIT_ULTRA_NEGOTIATE"] = "1"
        if args.ultra_native_video:
            control_environment["HEADUNIT_ULTRA_NATIVE_VIDEO"] = "1"
        if args.ultra_installed_ui:
            control_environment["HEADUNIT_ULTRA_INSTALLED_UI"] = str(effective_ui.resolve())
        if args.preview_sink == "x11":
            control_environment["DISPLAY"] = args.x_display
            control_environment["XAUTHORITY"] = str(
                Path(pwd.getpwuid(owner[0]).pw_dir) / ".Xauthority")
        elif args.preview_sink in WAYLAND_PREVIEW_SINKS or args.ultra_native_video:
            selected_display = display_fields["main.wayland"] if args.preview_sink == "wayland-gpu" else args.wayland_display
            startup_diagnostic(args.connection_diagnostics, 'wayland-wait-begin')
            wait_for_wayland(owner, selected_display)
            control_environment = wayland_environment(control_environment, owner[0], selected_display)
            startup_diagnostic(args.connection_diagnostics, 'wayland-ready')
        processes.append(launch([str(control), "7000", str(args.seconds),
                                 str(session), str(effective_identity),
                                 str(args.setup_secret), str(args.pairing_key),
                                 str(args.pair_store), "--mfi-auth",
                                 "--iap2-wifi-config", str(args.wifi_config)],
                                session / "receiver.log", owner,
                                environment=control_environment))
        startup_diagnostic(args.connection_diagnostics, 'receiver-spawned')
        if args.direct_media:
            bridge = Path(__file__).resolve().with_name("media_bridge.py")
            command = python_helper_command(bridge, "--input", str(args.media_state) + ".iap",
                                            "--output", str(args.media_state))
            if args.auto_connect_phone:
                command.extend(("--phone-address", args.auto_connect_phone))
            processes.append(launch(command,
                                    session / "media-bridge.log", owner))
        if args.auto_connect_phone:
            reconnect = Path(__file__).resolve().with_name("carplay_reconnect.py")
            reconnect_environment = connection_diagnostic_environment(os.environ, args.connection_diagnostics)
            reconnect_process = launch(python_helper_command(reconnect,
                                        "--phone-address", args.auto_connect_phone,
                                        "--identity", str(effective_identity),
                                        "--seconds", str(args.seconds),
                                        "--retry-seconds", str(args.auto_connect_retry_seconds),
                                        "--max-attempts", str(args.auto_connect_max_attempts)),
                                       session / "reconnect.log", owner, environment=reconnect_environment)
            processes.append(reconnect_process)
        deadline = time.monotonic() + args.seconds if args.seconds else float('inf')
        print("Independent lab receiver started; private logs:", session)
        while not stopping and time.monotonic() < deadline:
            for process in processes:
                if process.poll() is not None:
                    if process is reconnect_process:
                        if not reconnect_exit_reported:
                            print("Automatic reconnect helper stopped; receiver remains running; inspect reconnect.log")
                            reconnect_exit_reported = True
                        continue
                    raise RuntimeError(
                        f"lab component {Path(process.args[0]).name} exited "
                        f"with status {process.returncode}; inspect private logs")
            time.sleep(0.25)
    finally:
        for process in reversed(processes):
            stop_process(process)
        cleanup_commands = []
        if added_ipv6:
            cleanup_commands.append(("ip", "-6", "address", "del",
                                     "fe80::1/64", "dev", args.interface))
        if added_ipv4:
            cleanup_commands.append(("ip", "address", "del",
                                     "10.10.0.1/24", "dev", args.interface))
        if raised_link:
            cleanup_commands.append(("ip", "link", "set", args.interface, "down"))
        cleanup_failed = False
        for command in cleanup_commands:
            try:
                run(*command)
            except subprocess.CalledProcessError:
                cleanup_failed = True
        if cleanup_failed:
            print("Independent lab receiver stopped; inspect Wi-Fi address and link state")
        else:
            print("Independent lab receiver stopped; network changes restored")


if __name__ == "__main__":
    main()
