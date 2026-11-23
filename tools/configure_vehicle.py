#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Compile one vehicle definition into existing validated launcher/Ultra inputs.

This is offline configuration work. It neither starts services nor acquires
readings. Signed artwork paths and semantic providers remain explicit inputs.
The destination must be new; a selected or sealed release is never modified.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import tempfile

from appliance_release import ui_fields
from create_cluster_bench_profile import drive_mode_configuration
from receiver_configuration import load_receiver_configuration, parse_receiver_configuration


MAX_DEFINITION_BYTES = 65536
CAPABILITIES = {'climate': 'climate_config', 'settings': 'car_settings_config',
                'camera': 'camera_state_config'}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate vehicle definition key')
        result[key] = value
    return result


def finite_json(_):
    raise ValueError('nonfinite vehicle definition number')


def _input_identity(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_gid, info.st_mode,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _bounded_input(path, maximum, description):
    """Pin explicitly supplied input without changing its path/owner policy."""
    path = Path(path)
    before_parent = path.parent.stat()
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NONBLOCK)
    fd = -1
    try:
        opened_parent = os.fstat(directory)
        if (before_parent.st_dev, before_parent.st_ino) != (opened_parent.st_dev, opened_parent.st_ino):
            raise ValueError(description + ' parent changed during open')
        before = os.stat(path.name, dir_fd=directory)
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
            raise ValueError(description + ' must be a bounded regular file')
        # Explicit symlink inputs remain supported. The opened file and final
        # named target must match, even if a link or its target is replaced.
        fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK, dir_fd=directory)
        opened = os.fstat(fd)
        if _input_identity(before) != _input_identity(opened):
            raise ValueError(description + ' changed during open')
        chunks = []
        remaining = opened.st_size
        while remaining:
            chunk = os.read(fd, remaining)
            if not chunk:
                raise ValueError(description + ' changed during read')
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b''.join(chunks)
        extra = os.read(fd, 1)
        after = os.fstat(fd)
        named = os.stat(path.name, dir_fd=directory)
        final_parent = path.parent.stat()
        if (extra or _input_identity(opened) != _input_identity(after) or
                _input_identity(opened) != _input_identity(named) or
                (before_parent.st_dev, before_parent.st_ino) != (final_parent.st_dev, final_parent.st_ino)):
            raise ValueError(description + ' changed during read')
        return raw
    finally:
        if fd >= 0:
            os.close(fd)
        os.close(directory)


def read_definition(path):
    raw = _bounded_input(path, MAX_DEFINITION_BYTES, 'vehicle definition')
    try:
        return json.loads(raw.decode('utf-8'), object_pairs_hook=unique_object,
                          parse_constant=finite_json)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError('invalid vehicle definition JSON') from error


def label(value):
    if not isinstance(value, str) or not value or any(
            ord(c) < 32 or 127 <= ord(c) <= 159 for c in value):
        raise ValueError('invalid vehicle identity label')
    try:
        if len(value.encode('utf-8')) > 128:
            raise ValueError('vehicle identity label exceeds 128 bytes')
    except UnicodeError as error:
        raise ValueError('invalid vehicle identity UTF-8') from error


def compile_profile(document):
    if (not isinstance(document, dict) or
            set(document) - {'schemaVersion', 'vehicle', 'ultraSignals', 'driveModes', 'capabilities',
                             'capabilityBackend', 'nativeCluster'} or
            not {'schemaVersion', 'vehicle', 'ultraSignals'} <= set(document) or
            type(document['schemaVersion']) is not int or document['schemaVersion'] != 1):
        raise ValueError('vehicle definition requires schemaVersion 1, vehicle and ultraSignals')
    identity = document['vehicle']
    if not isinstance(identity, dict) or set(identity) != {'make', 'model', 'year'}:
        raise ValueError('vehicle requires make, model and year only')
    label(identity['make'])
    label(identity['model'])
    if type(identity['year']) is not int or not 1886 <= identity['year'] <= 9999:
        raise ValueError('vehicle year must be 1886..9999')
    signals = copy.deepcopy(document['ultraSignals'])
    if not isinstance(signals, list) or len(signals) > 512:
        raise ValueError('ultraSignals must be an explicit list of at most 512 bindings')
    # The native C++ decoder is the authority for all ordinary signal semantics.
    # Modes have one configuration authority: omission cannot inherit a bench mode.
    for signal in signals:
        if (not isinstance(signal, dict) or 'iid' in signal or
                not isinstance(signal.get('path'), list) or
                signal.get('vehicleField') == 'drive_mode' or
                signal.get('path', [])[:2] == ['driveState', 'driveMode']):
            raise ValueError('signals cannot supply IIDs or bypass driveModes')
    if 'driveModes' in document:
        identifiers, states = drive_mode_configuration(
            {'schemaVersion': 1, 'modes': document['driveModes']})
        signals.extend([
            {'path': ['driveState', 'driveMode', 'identifiers'], 'kind': 'array',
             'maxAgeMs': 250, 'fixedValue': identifiers},
            {'path': ['driveState', 'driveMode', 'currentIndex'], 'kind': 'integer',
             'maxAgeMs': 250, 'vehicleField': 'drive_mode', 'vehicleStates': states},
            {'path': ['driveState', 'driveMode', 'hidden'], 'kind': 'boolean',
             'maxAgeMs': 250, 'fixedValue': False},
        ])
    if len(signals) > 512:
        raise ValueError('driveModes exceeds the 512-signal budget')
    for index, signal in enumerate(signals):
        signal['iid'] = index + 3
    capabilities = document.get('capabilities', {})
    if (not isinstance(capabilities, dict) or set(capabilities) - set(CAPABILITIES) or
            any(not isinstance(value, dict) for value in capabilities.values())):
        raise ValueError('capabilities must contain only explicit climate/settings/camera objects')
    backend = document.get('capabilityBackend', 'none')
    if backend not in ('none', 'simulation') or bool(capabilities) != (backend == 'simulation'):
        raise ValueError('capabilities require explicit capabilityBackend=simulation; no hardware executor is installed')
    if 'nativeCluster' in document:
        native_configuration(document['nativeCluster'])
    return {'schemaVersion': 1, 'mode': 'local-ui-bindings',
            'vehicleIdentity': copy.deepcopy(identity), 'signals': signals}


def native_configuration(document):
    keys = {'speedUnit', 'speedMaximumKmh', 'rpmMaximum', 'rpmRedline', 'showRpm'}
    if (not isinstance(document, dict) or set(document) - keys - {'temperatureUnit', 'distanceUnit', 'fuelConsumptionUnit', 'speedPrecision', 'pressureUnit', 'temperaturePrecision'} or
            not keys <= set(document)):
        raise ValueError('nativeCluster requires explicit speed unit/range, RPM range/redline and visibility')
    if document['speedUnit'] not in ('kmh', 'mph') or type(document['showRpm']) is not bool:
        raise ValueError('invalid nativeCluster units or visibility')
    if 'pressureUnit' in document and (type(document['pressureUnit']) is not str or
            document['pressureUnit'] not in ('kpa', 'bar', 'psi')):
        raise ValueError('invalid nativeCluster pressure unit')
    if 'temperaturePrecision' in document and (type(document['temperaturePrecision']) is not str or
            document['temperaturePrecision'] not in ('whole', 'one_decimal')):
        raise ValueError('invalid nativeCluster temperature precision')
    if 'speedPrecision' in document and (type(document['speedPrecision']) is not str or
            document['speedPrecision'] not in ('whole', 'one_decimal')):
        raise ValueError('invalid nativeCluster speed precision')
    if 'fuelConsumptionUnit' in document and document['fuelConsumptionUnit'] not in ('liters_per_100km', 'us_mpg', 'imperial_mpg'):
        raise ValueError('invalid nativeCluster fuel consumption unit')
    if 'distanceUnit' in document and document['distanceUnit'] not in ('kilometers', 'miles'):
        raise ValueError('invalid nativeCluster distance unit')
    for key in ('speedMaximumKmh', 'rpmMaximum', 'rpmRedline'):
        if type(document[key]) not in (int, float):
            raise ValueError('nativeCluster ranges must be numeric')
    if 'temperatureUnit' in document and document['temperatureUnit'] not in ('celsius', 'fahrenheit'):
        raise ValueError('invalid nativeCluster temperature unit')
    # The bounded C++ parser validates the numeric domain before publication.
    return ('version=1\nspeed_unit=' + document['speedUnit'] +
            '\nspeed_maximum_kmh=' + str(document['speedMaximumKmh']) +
            '\nrpm_maximum=' + str(document['rpmMaximum']) +
            '\nrpm_redline=' + str(document['rpmRedline']) +
            '\nshow_rpm=' + str(document['showRpm']).lower() + '\n' +
            ('temperature_unit=' + document['temperatureUnit'] + '\n'
             if 'temperatureUnit' in document else '') +
            ('distance_unit=' + document['distanceUnit'] + '\n'
             if 'distanceUnit' in document else '') +
            ('fuel_consumption_unit=' + document['fuelConsumptionUnit'] + '\n'
             if 'fuelConsumptionUnit' in document else '') +
            ('speed_precision=' + document['speedPrecision'] + '\n'
             if 'speedPrecision' in document else '') +
            ('pressure_unit=' + document['pressureUnit'] + '\n'
             if 'pressureUnit' in document else '') +
            ('temperature_precision=' + document['temperaturePrecision'] + '\n'
             if 'temperaturePrecision' in document else '')).encode()


def json_bytes(document):
    return (json.dumps(document, indent=2, ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')


def compile_configuration(definition, receiver_path, destination, checker):
    """Validate a complete generated graph before atomically publishing a new directory."""
    profile = compile_profile(definition)
    receiver = load_receiver_configuration(Path(receiver_path))
    if not (receiver.get('ultra_negotiate') and receiver.get('ultra_native_video') and
            receiver.get('ultra_installed_ui')):
        raise ValueError('vehicle compilation requires an explicit native installed Ultra profile')
    destination = Path(destination).absolute()
    destination_text = str(destination / 'vehicle-definition.json')
    if len(destination_text.encode('utf-8')) > 4000 or any(ord(c) < 32 or ord(c) == 127 for c in destination_text):
        raise ValueError('vehicle configuration destination must fit a bounded single-line path')
    if destination.exists() or destination.is_symlink():
        raise ValueError('vehicle configuration destination must be new')
    if not destination.parent.is_dir():
        raise ValueError('vehicle configuration destination parent must exist')
    destination = destination.parent.resolve() / destination.name
    ui_path = receiver['ultra_installed_ui']
    raw_ui = _bounded_input(ui_path, 8192, 'installed UI configuration')
    ui = ui_fields(raw_ui)
    for key in ('cameraProfile',):
        ui.pop(key, None)
    capabilities = definition.get('capabilities', {})
    if 'camera' not in capabilities and any(key in ui for key in (
            'cameraControlFile', 'nativeCameraControlFile', 'nativeCameraLeaseFile')):
        raise ValueError('camera ownership endpoints require explicit camera capabilities')
    files = {'vehicle-profile.plist': plistlib.dumps(profile, fmt=plistlib.FMT_BINARY)}
    if len(files['vehicle-profile.plist']) > 65536:
        raise ValueError('generated local profile exceeds the 65536-byte release bound')
    receiver['vehicle_state_profile'] = 'none'
    # Generated metadata/options require the same build that validated them;
    # an older inherited executable directory can reject this schema at launch.
    receiver['bin_dir'] = Path(checker).resolve().parent
    receiver['vehicle_state_backend'] = definition.get('capabilityBackend', 'none')
    receiver['simulate_vehicle'] = False
    receiver.pop('native_cluster_config', None)
    if 'nativeCluster' in definition:
        files['native-cluster.conf'] = native_configuration(definition['nativeCluster'])
        receiver['native_cluster_config'] = destination / 'native-cluster.conf'
    for capability, field in CAPABILITIES.items():
        receiver.pop(field, None)
        if capability in capabilities:
            filename = {'settings': 'car-settings.json'}.get(capability, capability + '.json')
            data = json_bytes(capabilities[capability])
            if len(data) > (8192 if capability == 'camera' else 65536):
                raise ValueError('generated capability exceeds its release bound')
            files[filename] = data
            receiver[field] = destination / filename
    ui['vehicleProfile'] = str(destination / 'vehicle-profile.plist')
    ui['simulateGauges'] = 'false'
    if 'camera' in capabilities:
        ui['cameraProfile'] = str(destination / 'camera.json')
    receiver['ultra_installed_ui'] = destination / 'installed-ui.conf'
    files['installed-ui.conf'] = ''.join(key + '=' + value + '\n' for key, value in ui.items()).encode()
    if len(files['installed-ui.conf']) > 8192:
        raise ValueError('generated installed UI configuration exceeds its release bound')
    files['receiver.json'] = json_bytes({key: str(value) if isinstance(value, Path) else value
                                         for key, value in receiver.items()})
    parse_receiver_configuration(files['receiver.json'], destination / 'receiver.json')
    # The definition remains the edit source, not another mutable runtime input.
    files['vehicle-definition.json'] = json_bytes(definition)
    staging = Path(tempfile.mkdtemp(prefix='.vehicle-config-', dir=destination.parent))
    try:
        for name, data in files.items():
            target = staging / name
            target.write_bytes(data)
            target.chmod(0o600)
        args = [str(Path(checker).absolute())]
        args.extend(str(staging / name) if name in files else '-'
                    for name in ('climate.json', 'car-settings.json', 'camera.json'))
        args.extend(['--local-profile', str(staging / 'vehicle-profile.plist')])
        if 'native-cluster.conf' in files:
            args.extend(['--native-cluster', str(staging / 'native-cluster.conf')])
        completed = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   timeout=30, check=False)
        if completed.returncode:
            # Do not echo arbitrary capability names, paths or provider values.
            raise ValueError('native vehicle configuration validation failed')
        # A second existence check prevents replacing a destination created during
        # validation. This tool is for a private single-writer configuration parent.
        if destination.exists() or destination.is_symlink():
            raise ValueError('vehicle configuration destination appeared during validation')
        os.rename(staging, destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--definition', required=True, type=Path)
    parser.add_argument('--receiver', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--checker', required=True, type=Path,
                        help='Matching build/headunit-vehicle-config-check executable')
    args = parser.parse_args()
    try:
        compile_configuration(read_definition(args.definition), args.receiver, args.output, args.checker)
    except (OSError, ValueError, TypeError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print('Vehicle configuration validated and written; no services started')


if __name__ == '__main__':
    main()
