#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Generate local Ultra gauge bindings for an explicitly simulated bench review.

IIDs are allocated locally; these are not the phone's Vehicle Data Protocol IDs.
With --extended-readings, fuel/coolant/ambient/range/odometer values use the
existing VehicleSource boundary. Thresholds, units and transmission are explicit
bench configuration; this tool does not acquire measurements from a car.
--drive-mode-fixture explicitly adds an immutable authored state list and a
fixed selection for offline artwork review, not a live vehicle drive-mode input.
--observed-transmission replaces the fixed M/3 artwork fixture with typed
selector/engaged-gear bindings. It supplies no measurement or inferred gear.
--drive-mode-config reads {"schemaVersion":1,"modes":[{"id":"comfort",
"assetIdentifier":"GT"},{"id":"sport","assetIdentifier":"sport"}]}.
The ordered immutable mapping supplies authored artwork identifiers and timed
zero-based indices only. Active mode measurements must come from VehicleSource.
--observed-trip adds only the inspected repeated tripComputer.trip consumption
input, converting whole-trip mean L/100km to mL/100km. Its instance and label are
explicit bench configuration; no counters or consumption are fabricated.
"""
import argparse
import json
from pathlib import Path
import plistlib
import re


DRIVE_MODE_IDENTIFIERS = ('GT', 'sport', 'individual', 'terrain', 'track', 'wet')
MAX_DRIVE_MODE_CONFIG_BYTES = 16 * 1024
DRIVE_MODE_ID = re.compile(r'[A-Za-z][A-Za-z0-9_-]{0,63}\Z')
TRIP_INSTANCE = re.compile(r'[A-Za-z0-9_-]{1,128}\Z')


def drive_mode_configuration(document):
    if (not isinstance(document, dict) or set(document) != {'schemaVersion', 'modes'}
            or type(document['schemaVersion']) is not int or document['schemaVersion'] != 1):
        raise ValueError('drive-mode configuration needs schemaVersion 1 and modes')
    modes = document['modes']
    if not isinstance(modes, list) or not 1 <= len(modes) <= 32:
        raise ValueError('drive-mode configuration needs 1..32 ordered entries')
    identifiers, states = [], {}
    for entry in modes:
        if not isinstance(entry, dict) or set(entry) != {'id', 'assetIdentifier'}:
            raise ValueError('drive-mode entry needs id and assetIdentifier only')
        identifier, asset = entry['id'], entry['assetIdentifier']
        if not isinstance(identifier, str) or not DRIVE_MODE_ID.fullmatch(identifier):
            raise ValueError('invalid semantic drive-mode identifier')
        if (not isinstance(asset, str) or not asset or
                any(ord(character) < 32 or 127 <= ord(character) <= 159 for character in asset)):
            raise ValueError('invalid authored drive-mode identifier')
        try:
            size = len(asset.encode('utf-8'))
        except UnicodeError as error:
            raise ValueError('invalid authored drive-mode UTF-8 identifier') from error
        if size > 128:
            raise ValueError('authored drive-mode identifier exceeds 128 bytes')
        if identifier in states or asset in identifiers:
            raise ValueError('duplicate semantic or authored drive-mode identifier')
        states[identifier] = len(identifiers)
        identifiers.append(asset)
    # The C++ profile decoder caps every fixed scalar array at 1 KiB encoded.
    # Match OPACK's array marker/terminator and string length prefixes exactly.
    lengths = [len(identifier.encode('utf-8')) for identifier in identifiers]
    encoded_size = (1 if len(identifiers) < 15 else 2) + sum(
        size + (1 if size <= 32 else 2) for size in lengths)
    if encoded_size > 1024:
        raise ValueError('authored drive-mode identifiers array exceeds 1 KiB')
    return identifiers, states


def read_drive_mode_configuration(path):
    with path.open('rb') as stream:
        data = stream.read(MAX_DRIVE_MODE_CONFIG_BYTES + 1)
    if not data or len(data) > MAX_DRIVE_MODE_CONFIG_BYTES:
        raise ValueError('drive-mode configuration must be 1..16384 bytes')

    def unique_object(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate drive-mode configuration key')
            result[key] = value
        return result

    def nonfinite_number(_):
        raise ValueError('nonfinite JSON number')

    try:
        document = json.loads(data.decode('utf-8'), object_pairs_hook=unique_object,
                              parse_constant=nonfinite_number)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError('invalid drive-mode configuration JSON') from error
    drive_mode_configuration(document)
    return document


def profile(night=True, extended=False, drive_mode=None, observed_transmission=False, drive_mode_config=None,
            observed_trip=False, trip_instance=None, trip_label=None, session_greeting=False):
    if drive_mode is not None and drive_mode not in DRIVE_MODE_IDENTIFIERS:
        raise ValueError('unknown authored drive-mode fixture')
    if drive_mode is not None and drive_mode_config is not None:
        raise ValueError('drive-mode configuration and artwork fixture are mutually exclusive')
    configured_modes = drive_mode_configuration(drive_mode_config) if drive_mode_config is not None else None
    if not observed_trip and (trip_instance is not None or trip_label is not None):
        raise ValueError('trip instance/label requires --observed-trip')
    if observed_trip:
        trip_instance = 'currentTrip' if trip_instance is None else trip_instance
        trip_label = 'Current Trip' if trip_label is None else trip_label
        if not isinstance(trip_instance, str) or not TRIP_INSTANCE.fullmatch(trip_instance):
            raise ValueError('invalid local trip instance')
        if (not isinstance(trip_label, str) or not trip_label or
                any(ord(character) < 32 or 127 <= ord(character) <= 159 for character in trip_label)):
            raise ValueError('invalid local trip label')
        try:
            label_size = len(trip_label.encode('utf-8'))
        except UnicodeError as error:
            raise ValueError('invalid local trip UTF-8 label') from error
        if label_size > 128:
            raise ValueError('local trip label exceeds 128 bytes')
    signals = []

    def signal(path, kind, *, unit=None, fixed=None, field=None, scale=1.0, states=None, instance=None):
        value = {'iid': len(signals) + 3, 'path': path.split('.'), 'kind': kind, 'maxAgeMs': 250}
        if unit is not None:
            value['units'] = unit
        if instance is not None:
            value['instance'] = instance
        if fixed is not None:
            value['fixedValue'] = fixed
        if field:
            value['vehicleField'] = field
            if scale != 1.0:
                value['vehicleScale'] = scale
        if states is not None:
            value['vehicleStates'] = states
        signals.append(value)

    signal('vehicleMotion.displayedSpeed.speed', 'real', unit=1034)
    signal('vehicleMotion.displayedSpeed.speedKMH', 'real', unit=1032)
    signal('vehicleMotion.displayedSpeed.speedMPH', 'real', unit=1033)
    signal('vehicleMotion.displayedSpeed.speedMaxKMH', 'real', unit=1032, fixed=260.0)
    signal('vehicleMotion.displayedSpeed.speedMaxMPH', 'real', unit=1033, fixed=160.0)
    signal('vehicleMotion.displayedSpeed.showSecondarySpeed', 'boolean', fixed=False)
    signal('vehicleUnits.displayUnits.speedUnit', 'integer', fixed=1032)
    signal('internalCombustionEngine.engineRPM.rotationalSpeed', 'real')
    signal('internalCombustionEngine.engineRPM.rotationalSpeedMax', 'real', fixed=8000.0)
    signal('internalCombustionEngine.engineRPM.rotationalSpeedMarkerRedline', 'real', fixed=6500.0)
    signal('internalCombustionEngine.engineRPM.rotationalSpeedState', 'integer')
    # Some primary layouts construct a target-speed controller even while
    # cruise control is inactive. Declare the inputs, with inactive fixtures.
    signal('driverAssistance.targetSpeed.speedKMH', 'real', unit=1032, fixed=80.0)
    signal('driverAssistance.targetSpeed.speedMPH', 'real', unit=1033, fixed=80.0 / 1.609344)
    signal('driverAssistance.targetSpeed.targetSpeedState', 'integer', fixed=0)
    if observed_transmission:
        # Explicit display mappings for the inspected reference family only.
        # Gearbox type is deliberately not a source for transmissionMode.
        signal('driveState.transmissionStatus.transmissionMode', 'integer',
               field='transmission.selector', states={
                   'park': 80, 'reverse': 82, 'neutral': 78, 'drive': 68,
                   'low': 76, 'manual': 77, 'sport': 83})
        signal('driveState.transmissionStatus.gearPosition', 'integer',
               field='transmission.gear', states={
                   'neutral': 0, 'reverse': 0,
                   **{str(gear): gear for gear in range(1, 33)}})
    else:
        signal('driveState.transmissionStatus.transmissionMode', 'integer', fixed=77)  # M operating mode, fixture only.
        signal('driveState.transmissionStatus.gearPosition', 'integer', fixed=3)
    if drive_mode is not None:
        signal('driveState.driveMode.identifiers', 'array', fixed=list(DRIVE_MODE_IDENTIFIERS))
        signal('driveState.driveMode.currentIndex', 'integer', fixed=DRIVE_MODE_IDENTIFIERS.index(drive_mode))
        signal('driveState.driveMode.hidden', 'boolean', fixed=False)
    elif configured_modes is not None:
        identifiers, states = configured_modes
        signal('driveState.driveMode.identifiers', 'array', fixed=identifiers)
        signal('driveState.driveMode.currentIndex', 'integer', field='drive_mode', states=states)
        signal('driveState.driveMode.hidden', 'boolean', fixed=False)
    if extended:
        signal('fuel.fuelLevel.fuelLevel', 'real', field='fuel_percent')
        signal('fuel.fuelLevel.fuelLevelState', 'integer', fixed=0)
        signal('fuel.fuelLevel.fuelLevelMarkerLow', 'real', fixed=15.0)
        # Range labels require a configured port-side controller, even when
        # no arrow is wanted. NONE=0 is the inspected pack's explicit fixture.
        signal('fuel.fuelLevel.portSideIndicator', 'integer', fixed=0)
        signal('internalCombustionEngine.engineTemperature.temperature', 'real', unit=1082, field='coolant_c')
        signal('internalCombustionEngine.engineTemperature.temperatureMin', 'real', unit=1082, fixed=50.0)
        signal('internalCombustionEngine.engineTemperature.temperatureMax', 'real', unit=1082, fixed=130.0)
        signal('internalCombustionEngine.engineTemperature.temperatureMarkerCold', 'real', unit=1082, fixed=60.0)
        signal('internalCombustionEngine.engineTemperature.temperatureMarkerHot', 'real', unit=1082, fixed=115.0)
        signal('internalCombustionEngine.engineTemperature.temperatureState', 'integer', fixed=0)
        signal('environmentalConditions.exteriorConditions.temperature', 'real', unit=1082, field='ambient_c')
        signal('vehicleUnits.displayUnits.temperatureUnit', 'integer', fixed=1082)
        # The asset consumes paired kilometre/mile values indexed by their
        # units. Both come from one timestamped physical field, not separate
        # independently refreshed measurements or fixed display strings.
        for path, field in [('fuel.fuelRemainingRange', 'range_km'),
                            ('tripComputer.remainingRange', 'range_km'),
                            ('tripComputer.odometer', 'odometer_km')]:
            signal(path + '.distanceKM', 'real', unit=151, field=field)
            signal(path + '.distanceMiles', 'real', unit=164, field=field, scale=1.0 / 1.609344)
        signal('fuel.fuelRemainingRange.hidden', 'boolean', fixed=False)
        signal('tripComputer.remainingRange.hidden', 'boolean', fixed=False)
        signal('vehicleUnits.displayUnits.distanceUnit', 'integer', fixed=151)
    if observed_trip:
        # Pack-inspected units, not protocol IDs or physical car encodings.
        signal('tripComputer.trip.userVisibleLabel', 'string', fixed=trip_label, instance=trip_instance)
        signal('tripComputer.trip.fuelEfficiency', 'real', unit=674,
               field='trip.fuel_efficiency_l_per_100km', scale=1000.0, instance=trip_instance)
        signal('vehicleUnits.displayUnits.fuelEfficiencyUnit', 'integer', fixed=671)
    if session_greeting:
        signal('uiControl.uiState.uiSceneState', 'integer')
        signals[-1]['sessionField'] = 'scene_state'
        signal('uiControl.uiState.uiSceneOptions', 'array')
        signals[-1]['sessionField'] = 'scene_options'
    else:
        signal('uiControl.uiState.uiSceneState', 'integer', fixed=2)
    signal('uiControl.uiAppearance.nightMode', 'boolean', fixed=night)
    return {'schemaVersion': 1, 'mode': 'local-ui-bindings', 'signals': signals}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--day', action='store_true')
    parser.add_argument('--extended-readings', action='store_true', help='Typed fuel, temperatures, range and odometer; offline-qualified, pending live check')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--drive-mode-fixture', choices=DRIVE_MODE_IDENTIFIERS,
                        help='Explicit fixed authored drive-mode artwork fixture; never a measured car value')
    mode.add_argument('--drive-mode-config', type=Path,
                      help='Ordered semantic ID -> authored artwork JSON configuration; active mode remains unavailable until observed')
    parser.add_argument('--observed-transmission', action='store_true',
                        help='Timed typed selector/engaged-gear bindings; no fixed or inferred transmission value')
    parser.add_argument('--observed-trip', action='store_true',
                        help='Timed whole-trip consumption input only; no invented trip readings')
    parser.add_argument('--trip-instance',
                        help='Explicit local repeated trip instance (default: currentTrip); requires --observed-trip')
    parser.add_argument('--trip-label',
                        help='Explicit fixed authored trip label (default: Current Trip); requires --observed-trip')
    parser.add_argument('--session-greeting', action='store_true',
                        help='Timed authenticated-session STARTUP to DRIVE; optional pairing requires verified new pairing')
    args = parser.parse_args()
    try:
        configuration = read_drive_mode_configuration(args.drive_mode_config) if args.drive_mode_config is not None else None
        output = profile(not args.day, args.extended_readings, args.drive_mode_fixture,
                         args.observed_transmission, configuration,
                         args.observed_trip, args.trip_instance, args.trip_label, args.session_greeting)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    args.output.write_bytes(plistlib.dumps(output))
    args.output.chmod(0o600)
