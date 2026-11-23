# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Strict saved launcher options. Device secrets remain in separate private files."""
import json
import os
import stat
import re
from pathlib import Path

PATHS = {"wifi_config", "identity", "pairing_key", "setup_secret", "pair_store", "mfi_private_key", "mfi_certificate",
         "capture_root", "bin_dir", "ultra_installed_ui", "display_config", "standard_display_config", "media_state", "cluster_page", "vehicle_socket",
         "climate_config", "car_settings_config", "camera_state_config", "native_cluster_config", "audio_control", "audio_state",
         "standard_camera_control_file", "standard_camera_lease_file", "camera_manual_profile",
         "native_page_control_file", "native_page_lease_file", "runtime_status_file", "navigation_socket", "greeting_sequence_file",
         "audio_observation_directory", "prephone_publication_socket"}
STANDARD_CAMERA_PATHS = {"standard_camera_control_file", "standard_camera_lease_file"}
NATIVE_PAGE_PATHS = {"native_page_control_file", "native_page_lease_file"}
STRINGS = {"interface", "preview_sink", "preview_decoder", "audio_sink", "audio_device", "microphone_source", "microphone_device", "x_display", "wayland_display",
           "mfi_device", "mfi_backend", "wifi_country", "route_guidance_name", "auto_connect_phone", "selected_phone", "vehicle_state_profile",
           "vehicle_state_backend", "runtime_status_nonce", "night_mode", "limited_ui"}
INTEGERS = {"route_guidance_component_id", "seconds", "mfi_address", "auto_connect_retry_seconds", "auto_connect_max_attempts", "audio_volume_percent"}
FLAGS = {"route_guidance", "ultra_native_video", "ultra_negotiate", "standard_cluster", "simulate_vehicle", "simulate_dated_vehicle", "fallback_cluster", "direct_media", "vehicle_trace_writes", "audio_muted", "connection_diagnostics", "oem_icon", "multitouch", "pre_phone_gauges", "sport_telemetry", "trip_history", "ambient_light", "prephone_indicators"}
# Non-secret presentation settings; persistent pairing fields are never overridden.
DEVICE_FIELDS = {"device_name": "name", "device_model": "model",
                 "manufacturer": "manufacturer", "serial_number": "serial",
                 "firmware_version": "firmware", "hardware_version": "hardware",
                 "touchscreen_name": "touchscreen_name"}
STRINGS |= DEVICE_FIELDS.keys()


def standard_camera_path(value, setting):
    """Keep private endpoint names intact for the receiver's own preflight."""
    if not isinstance(value, (str, Path)):
        raise ValueError(f"{setting} must be a bounded absolute file path")
    text = str(value)
    if (not text or len(text.encode()) > 4000 or not Path(text).is_absolute() or
            text.endswith('/') or any(ord(c) < 32 or ord(c) == 127 for c in text) or
            any(part in ('.', '..') for part in text.split('/'))):
        raise ValueError(f"{setting} must be a bounded absolute file path without dot components")
    return Path(text)


def validate_standard_camera_settings(values):
    control = values.get("standard_camera_control_file")
    lease = values.get("standard_camera_lease_file")
    for setting, value in (("standard_camera_control_file", control), ("standard_camera_lease_file", lease)):
        if value is not None:
            standard_camera_path(value, setting)
    if (control is None) != (lease is None):
        raise ValueError("standard_camera_control_file and standard_camera_lease_file must be supplied together")
    if control is not None:
        if Path(control) == Path(lease):
            raise ValueError("Standard camera request and lease must be distinct files")
        if values.get("preview_sink", "none") != "wayland-gpu":
            raise ValueError("Standard camera handoff requires preview_sink=wayland-gpu")
        if values.get("ultra_negotiate", False) or values.get("ultra_native_video", False):
            raise ValueError("Standard camera handoff cannot be configured with Ultra")
        if not values.get("display_config"):
            raise ValueError("Standard camera handoff requires an explicit display_config")


def native_page_path(value, field):
    text = str(value) if isinstance(value, (str, Path)) else ''
    if (not re.fullmatch(r'/[A-Za-z0-9_./-]{1,3999}', text) or text.endswith('/') or
            any(not part or part in ('.', '..') or len(part) > 255 for part in text.split('/')[1:])):
        raise ValueError(f'{field} requires a bounded canonical absolute path without service syntax')
    return Path(text)


def validate_native_page_settings(values):
    present = {key for key in NATIVE_PAGE_PATHS if values.get(key) is not None}
    if not present:
        return
    if present != NATIVE_PAGE_PATHS:
        raise ValueError('Native page control and surface lease must be supplied together')
    control, lease = [native_page_path(values[key], key) for key in ('native_page_control_file', 'native_page_lease_file')]
    if control == lease or control.parent != lease.parent:
        raise ValueError('Native page control and lease need distinct files in the same private directory')
    if any(values.get(key) is not None for key in ('standard_camera_control_file', 'standard_camera_lease_file')):
        raise ValueError('Native page and camera surface ownership cannot be combined')
    if (values.get('camera_state_config') or values.get('camera_manual_profile')) and not (values.get('ultra_negotiate') and values.get('ultra_native_video') and values.get('ultra_installed_ui')):
        raise ValueError('Native page camera coexistence requires the signed Ultra owner')
    if not values.get('display_config'):
        raise ValueError('Native page handoff needs explicit display_config')
    if values.get('ultra_negotiate', False) or values.get('ultra_native_video', False):
        if not (values.get('ultra_negotiate') and values.get('ultra_native_video') and values.get('ultra_installed_ui')):
            raise ValueError('Native page Ultra handoff requires native installed UI negotiation')
    elif values.get('preview_sink') != 'wayland-gpu':
        raise ValueError('Native page Standard handoff requires preview_sink=wayland-gpu')
    for field in PATHS - NATIVE_PAGE_PATHS:
        if values.get(field) is not None and Path(values[field]) in (control, lease):
            raise ValueError('Native page endpoints collide with another receiver path: ' + field)


def validate_device_settings(values):
    if type(values.get('trip_history', False)) is not bool:
        raise ValueError('Trip history must be boolean')
    if values.get('trip_history', False) and not values.get('sport_telemetry', False):
        raise ValueError('Trip history requires sport telemetry')
    if type(values.get('simulate_dated_vehicle', False)) is not bool:
        raise ValueError('simulate_dated_vehicle must be an explicit boolean')
    if values.get('simulate_dated_vehicle', False) and (
            values.get('simulate_vehicle', False) or values.get('vehicle_socket')):
        raise ValueError('Dated simulation excludes legacy simulation and vehicle socket')
    if values.get('audio_observation_directory') is not None:
        native_page_path(values['audio_observation_directory'], 'audio_observation_directory')
    publication = values.get('prephone_publication_socket')
    if values.get('prephone_indicators', False) and publication is None:
        raise ValueError('Pre-phone indicators require native publication')
    if values.get('ambient_light', False) and publication is None:
        raise ValueError('Ambient software model requires native publication')
    if publication is not None:
        endpoint = native_page_path(publication, 'prephone_publication_socket')
        if endpoint.name != 'prephone.sock' or len(str(endpoint).encode()) > 103:
            raise ValueError('Native publication requires bounded manager prephone.sock')
        for key in PATHS - {'prephone_publication_socket'}:
            if values.get(key) is not None and Path(values[key]) in (endpoint, Path(str(endpoint) + '.lock')):
                raise ValueError('Native publication endpoint collides with another receiver path')
    backend = values.get('mfi_backend', 'hardware')
    if backend not in ('hardware', 'software'):
        raise ValueError('mfi_backend must be hardware or software')
    credentials = [values.get(key) for key in ('mfi_private_key', 'mfi_certificate')]
    if backend == 'software':
        if any(value is None for value in credentials):
            raise ValueError('Software MFi requires both mfi_private_key and mfi_certificate')
        for key in ('mfi_private_key', 'mfi_certificate'):
            native_page_path(values[key], key)
        if Path(credentials[0]) == Path(credentials[1]):
            raise ValueError('Software MFi key and certificate must be distinct files')
        for key in PATHS - {'mfi_private_key', 'mfi_certificate'}:
            if values.get(key) is not None and Path(values[key]) in map(Path, credentials):
                raise ValueError('Software MFi input collides with another receiver path: ' + key)
    elif any(value is not None for value in credentials):
        raise ValueError('Software MFi inputs require explicit mfi_backend=software')
    navigation = values.get('navigation_socket')
    if navigation is not None:
        endpoint = native_page_path(navigation, 'navigation_socket')
        if len(str(endpoint).encode()) > 103:
            raise ValueError('navigation_socket must fit a local Unix socket path (103 bytes)')
        if not values.get('route_guidance', False):
            raise ValueError('navigation_socket requires route_guidance')
        for key in PATHS - {'navigation_socket'}:
            if values.get(key) is not None and Path(values[key]) in (
                    endpoint, Path(str(endpoint) + '.lock'), Path(str(endpoint) + '.publisher.lock')):
                raise ValueError('Navigation endpoint collides with another receiver path: ' + key)
    if values.get('standard_display_config') is not None:
        native_page_path(values['standard_display_config'], 'standard_display_config')
        if not values.get('display_config'):
            raise ValueError('Phone-selected modes require explicit display geometry')
    validate_standard_camera_settings(values)
    validate_native_page_settings(values)
    validate_runtime_status_settings(values)
    if values.get("audio_sink", "none") not in ("none", "fakesink", "alsasink", "pulsesink"):
        raise ValueError("unknown audio_sink")
    control, state = values.get("audio_control"), values.get("audio_state")
    for key, value in (("audio_control", control), ("audio_state", state)):
        if value is not None and (not isinstance(value, (str, Path)) or not str(value) or '\0' in str(value)):
            raise ValueError(f"{key} must be a nonempty file path")
    if bool(control) != bool(state):
        raise ValueError("audio_control and audio_state must be supplied together")
    if control and Path(control).resolve() == Path(state).resolve():
        raise ValueError("audio_control and audio_state must be distinct")
    volume, muted = values.get("audio_volume_percent"), values.get("audio_muted")
    if volume is not None and (type(volume) is not int or not 0 <= volume <= 100):
        raise ValueError("audio_volume_percent must be an integer in 0..100")
    if muted is not None and type(muted) is not bool:
        raise ValueError("audio_muted must be boolean")
    if (control or volume is not None or muted is not None) and values.get("audio_sink", "none") not in ("fakesink", "alsasink", "pulsesink"):
        raise ValueError("audio controls and initial volume/mute require a configured audio_sink")
    audio_device = values.get("audio_device")
    if audio_device is not None:
        if (not isinstance(audio_device, str) or not 1 <= len(audio_device) <= 255 or
                any(ord(c) < 32 or ord(c) > 126 for c in audio_device)):
            raise ValueError("audio_device must contain 1..255 printable ASCII characters")
        if values.get("audio_sink", "none") not in ("alsasink", "pulsesink"):
            raise ValueError("audio_device requires alsasink or pulsesink")
    microphone_source = values.get("microphone_source", "none")
    if microphone_source not in ("none", "alsasrc", "pulsesrc"):
        raise ValueError("unknown microphone_source")
    if microphone_source != "none" and values.get("audio_sink", "none") == "none":
        raise ValueError("microphone capture requires configured audio output")
    microphone_device = values.get("microphone_device")
    if microphone_device is not None:
        if (not isinstance(microphone_device, str) or not 1 <= len(microphone_device) <= 255 or
                any(ord(c) < 32 or ord(c) > 126 for c in microphone_device)):
            raise ValueError("microphone_device must contain 1..255 printable ASCII characters")
        if microphone_source == "none":
            raise ValueError("microphone_device requires a capture source")
    if values.get("vehicle_state_profile", "none") not in ("none", "empty", "climate-bench", "settings-bench", "bench"):
        raise ValueError("unknown vehicle_state_profile")
    camera_state = values.get("camera_state_config")
    capabilities = bool(values.get("climate_config") or values.get("car_settings_config") or camera_state)
    backend = values.get("vehicle_state_backend", "none")
    if backend not in ("none", "simulation"):
        raise ValueError("vehicle_state_backend must be none or simulation; no hardware provider is installed")
    if capabilities and values.get("vehicle_state_profile", "none") != "none":
        raise ValueError("choose capability files or a legacy vehicle_state_profile")
    if capabilities != (backend == "simulation"):
        raise ValueError("capability files require the explicit simulation backend; backend requires capability files")
    if camera_state and not (values.get("ultra_negotiate") and values.get("ultra_native_video") and values.get("ultra_installed_ui")):
        raise ValueError("camera state requires native installed UI with explicit Ultra negotiation")
    for key, low, high in (("auto_connect_retry_seconds", 1, 600), ("auto_connect_max_attempts", 1, 10)):
        value = values.get(key)
        if value is not None and (type(value) is not int or not low <= value <= high):
            raise ValueError(f"{key} must be an integer in {low}..{high}")
    value = values.get("night_mode")
    if value is not None and (not isinstance(value, str) or not re.fullmatch(
            r"off|on|headlights|schedule|schedule:(?:[01][0-9]|2[0-3]):[0-5][0-9]-(?:[01][0-9]|2[0-3]):[0-5][0-9]", value)
            or (value.startswith("schedule:") and value[9:14] == value[15:20])):
        raise ValueError("night_mode must be off, on, headlights, schedule or schedule:HH:MM-HH:MM")
    if values.get("pre_phone_gauges") and not values.get("ultra_installed_ui"):
        raise ValueError("pre_phone_gauges requires a verified local Ultra profile")
    value = values.get("limited_ui")
    if value is not None and value != "speed":
        raise ValueError("limited_ui must be speed")
    value=values.get("route_guidance_component_id")
    if value is not None and (type(value) is not int or not 3 <= value <= 65535):
        raise ValueError("route_guidance_component_id must be 3..65535")
    value=values.get("route_guidance_name")
    if value is not None and (not isinstance(value,str) or not 1 <= len(value) <= 127 or any(ord(c)<32 or ord(c)>126 for c in value)):
        raise ValueError("route_guidance_name must be printable ASCII, 1..127 characters")
    phone = values.get("selected_phone")
    if phone is not None and (not isinstance(phone, str) or not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", phone)):
        raise ValueError("selected_phone must be a Bluetooth address")
    phone = values.get("auto_connect_phone")
    if phone is not None and (not isinstance(phone, str) or
                             not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", phone)):
        raise ValueError("auto_connect_phone must be a selected phone Bluetooth address")
    for key in DEVICE_FIELDS:
        value = values.get(key)
        if value is None:
            continue
        limit = 63 if key == "device_name" else 127
        if not isinstance(value, str) or not 1 <= len(value) <= limit or any(
                ord(c) < 32 or ord(c) > 126 for c in value):
            raise ValueError(f"{key} must contain 1..{limit} printable ASCII characters")


def validate_runtime_status_settings(values):
    """A per-launch nonce binds ephemeral receiver observations to their owner."""
    path, nonce = values.get('runtime_status_file'), values.get('runtime_status_nonce')
    if (path is None) != (nonce is None):
        raise ValueError('runtime_status_file and runtime_status_nonce must be supplied together')
    if path is None:
        return
    endpoint = native_page_path(path, 'runtime_status_file')
    if not isinstance(nonce, str) or not re.fullmatch(r'[0-9a-f]{32}', nonce):
        raise ValueError('runtime_status_nonce must contain 32 lowercase hexadecimal characters')
    for key in PATHS - {'runtime_status_file'}:
        value = values.get(key)
        if value is not None and Path(value) in (endpoint, Path(str(endpoint) + '.owner')):
            raise ValueError('Runtime presentation endpoint collides with another receiver path: ' + key)


def configured_identity(source, settings):
    """Overlay device labels only; preserve keys, UUIDs and negotiated capabilities."""
    validate_device_settings(settings)
    fields = {}
    for line in source.splitlines():
        key, separator, value = line.partition("=")
        if not separator or not key or key in fields:
            raise ValueError("invalid or duplicate persistent identity field")
        fields[key] = value
    for setting, field in DEVICE_FIELDS.items():
        if settings.get(setting) is not None:
            fields[field] = settings[setting]
    result = "".join(f"{key}={value}\n" for key, value in fields.items())
    if len(result.encode()) > 4096:
        raise ValueError("configured identity is too large")
    return result



def _input_identity(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_gid, info.st_mode,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def load_receiver_configuration(path):
    """Capture a bounded stable regular input; source links/permissions stay allowed.

    Sealed release callers retain their separate owner/link/manifest admission.
    Pin the resolved origin too: relative settings belong to the captured file,
    even if the caller's symlink is changed after this read.
    """
    path = Path(path).absolute()
    parent = path.parent.stat()
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NONBLOCK)
    fd = -1
    try:
        opened_parent = os.fstat(directory)
        if (parent.st_dev, parent.st_ino) != (opened_parent.st_dev, opened_parent.st_ino):
            raise ValueError("receiver configuration parent changed during open")
        before = os.stat(path.name, dir_fd=directory)
        origin = path.resolve(strict=True)
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= 16384:
            raise ValueError("receiver configuration must be a bounded regular file")
        fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK, dir_fd=directory)
        opened = os.fstat(fd)
        if _input_identity(before) != _input_identity(opened):
            raise ValueError("receiver configuration changed during open")
        chunks = []
        remaining = opened.st_size
        while remaining:
            chunk = os.read(fd, remaining)
            if not chunk:
                raise ValueError("receiver configuration changed during read")
            chunks.append(chunk)
            remaining -= len(chunk)
        extra = os.read(fd, 1)
        after = os.fstat(fd)
        named = os.stat(path.name, dir_fd=directory)
        final_parent = path.parent.stat()
        if (extra or _input_identity(opened) != _input_identity(after) or
                _input_identity(opened) != _input_identity(named) or
                (parent.st_dev, parent.st_ino) != (final_parent.st_dev, final_parent.st_ino) or
                path.resolve(strict=True) != origin):
            raise ValueError("receiver configuration changed during read")
        raw = b''.join(chunks)
    finally:
        if fd >= 0:
            os.close(fd)
        os.close(directory)
    return parse_receiver_configuration(raw, origin, source_parent=origin.parent)


def parse_receiver_configuration(raw, path, *, source_parent=None):
    """Parse captured bytes, with an optional already pinned source directory."""
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate receiver setting: {key}")
            result[key] = value
        return result

    if len(raw) > 16384:
        raise ValueError("receiver configuration is too large")
    values = json.loads(raw, object_pairs_hook=unique)
    if not isinstance(values, dict):
        raise ValueError("receiver configuration must be an object")
    for key, value in values.items():
        expected = str if key in PATHS | STRINGS else int if key in INTEGERS else bool if key in FLAGS else None
        if expected is None:
            raise ValueError(f"unknown receiver setting: {key}")
        if type(value) is not expected or (expected is str and (not value or '\0' in value)):
            raise ValueError(f"invalid receiver setting: {key}")
        if key in PATHS:
            if key in NATIVE_PAGE_PATHS or key in ('runtime_status_file', 'navigation_socket', 'greeting_sequence_file',
                                                 'audio_observation_directory'):
                values[key] = native_page_path(value, key)
                continue
            if key in STANDARD_CAMERA_PATHS:
                # Private endpoints must already be absolute. Resolving a
                # symlink here would hide it from the runtime ownership check.
                values[key] = standard_camera_path(value, key)
                continue
            resolved = Path(value).expanduser()
            if not resolved.is_absolute():
                resolved = (source_parent if source_parent is not None else path.resolve().parent) / resolved
            # Keep the final audio filename intact: the receiver must reject
            # a linked sidecar itself, rather than silently redirecting it.
            values[key] = (resolved.parent.resolve() / resolved.name if key in ('audio_control', 'audio_state', 'camera_manual_profile', 'mfi_private_key', 'mfi_certificate')
                           else resolved.resolve())
    if "display_config" not in values:
        raise ValueError("saved receiver configuration requires display_config")
    validate_device_settings(values)
    return values
