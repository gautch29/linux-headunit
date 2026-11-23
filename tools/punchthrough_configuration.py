# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Version-5 signed camera graph. Data validation only; no runtime ownership."""
import json
from pathlib import Path


def validate_graph(settings, fields, capture_raw, state_raw, manual_raw):
    from camera_manual import validate_configuration
    from appliance_release import unique
    capture = json.loads(capture_raw, object_pairs_hook=unique)
    state = json.loads(state_raw, object_pairs_hook=unique)
    if type(capture) is not dict or type(state) is not dict:
        raise ValueError('Signed camera profiles must be JSON objects')
    manual = validate_configuration(json.loads(manual_raw, object_pairs_hook=unique))
    if not (settings.get('ultra_negotiate') and settings.get('ultra_native_video') and
            settings.get('ultra_installed_ui') and settings.get('camera_state_config') and
            settings.get('camera_manual_profile')):
        raise ValueError('Signed camera needs explicit Ultra, state and manual owner roles')
    if any(settings.get(key) for key in ('standard_camera_control_file', 'standard_camera_lease_file')):
        raise ValueError('Signed camera graph cannot include a Standard camera owner')
    if any(fields.get(key) for key in ('nativeCameraControlFile', 'nativeCameraLeaseFile')):
        raise ValueError('Signed camera graph cannot include a native camera surface')
    if not fields.get('cameraProfile') or fields.get('cameraControlFile') != manual['requestFile'] or fields.get('cameraStatusFile') != manual['statusFile']:
        raise ValueError('Signed camera and manual owner must share the exact request endpoint')
    if (capture.get('enabled') is not True or capture.get('manualActivation') is not True or
            capture.get('streamIdentifier') != 'camera' or manual['enabled'] is not True):
        raise ValueError('Signed camera graph needs enabled explicit manual camera input')
    if type(state.get('general')) is not dict or type(state['general'].get('on')) is not dict or type(capture.get('capture')) is not dict:
        raise ValueError('Signed camera profiles need explicit dated visibility and capture')
    general = state['general']['on']
    maximum_age = capture['capture'].get('maximumAgeMs')
    if (state.get('version') != 1 or state.get('experimental') is not True or
            state.get('stream_identifier') != capture['streamIdentifier'] or
            general.get('binding') != 'camera.visible' or general.get('initial') is not False or
            general.get('writable') is not False or type(maximum_age) is not int or
            type(general.get('max_age_ms')) is not int or general['max_age_ms'] <= maximum_age):
        raise ValueError('Signed camera state must preserve owner-sourced read-only fresh visibility')
    endpoints = [Path(manual[key]) for key in ('requestFile', 'ownerFile', 'statusFile', 'controlSocket')]
    if manual['statusFile'] + '.owner' in [str(path) for path in endpoints]:
        raise ValueError('Signed camera status owner aliases a manual endpoint')
    if len({path.parent for path in endpoints}) != 1:
        raise ValueError('Signed camera requires one private manual endpoint directory')
    return manual
