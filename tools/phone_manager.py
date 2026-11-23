#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Native phone page backend. Local Unix socket only; owns its receiver child."""
import argparse
import base64
import copy
from collections import deque
import hashlib
import json
import math
import os
from pathlib import Path
import pwd
import secrets
import shutil
import signal
import stat
import subprocess
import tempfile
import sys
import threading
import time

from phone_state import PhoneState, address, atomic_json, receiver_settings
from receiver_configuration import load_receiver_configuration
from bluez_phones import BluezPhones
from media_state import MediaState, MediaObservations, COMMANDS, validate_media, media_fresh, validate_iap_observation

# Native transport buttons mapped to the Ultra media HID device. The device has
# one Play/Pause toggle, so both buttons request it; the phone decides the state.
HID_MEDIA_CONTROLS = {'play': 'play_pause', 'pause': 'play_pause',
                      'next': 'next_track', 'previous': 'previous_track'}
from cluster_page import adopt as cluster_page_adopt, available as cluster_pages, read as cluster_page_read, select as cluster_page_select
from cluster_layout import configured_path as layout_path, snapshot as layout_snapshot, select as layout_select, navigate as layout_navigate, ContentControl, format_snapshot, format_choice
from vehicle_fanout import vehicle_arguments
from drive_mode_profiles import DriveModeControl
from audio_control import AudioControl, unavailable as audio_unavailable
from carplay_readiness import CarplayReadiness
from camera_manual import ManualCamera, unavailable_status_observation
from phone_manager_boot import inspect_configuration, prepare_directory
from phone_manager_endpoint import ManagerEndpoint
from run_lab_receiver import sealed_manager_configuration, verify_manager_lock

RETIREMENT_REQUIRED = 'Receiver launcher died before cleanup; retire the whole manager service before retrying'


def cluster_display(path):
    fields = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, separator, value = line.partition('=')
        if not separator or not key or key in fields:
            raise ValueError('Invalid fallback display configuration')
        fields[key] = value
    name = fields.get('cluster.wayland', '')
    if name in ('', '.', '..') or len(name) > 100 or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in name):
        raise ValueError('Invalid fallback Wayland socket name')
    try:
        width = int(fields['cluster.width'])
        height = int(fields['cluster.height'])
    except (KeyError, ValueError) as error:
        raise ValueError('Invalid fallback cluster size') from error
    if not 640 <= width <= 4096 or not 240 <= height <= 2160:
        raise ValueError('Fallback cluster size outside supported range')
    return name, (width, height)


def cluster_wayland_name(path):
    return cluster_display(path)[0]


def open_owned_log(path, owner):
    """A malformed log endpoint cannot block launch or redirect append writes."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_CLOEXEC |
                 os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
                info.st_uid not in (0, owner[0]) or stat.S_IMODE(info.st_mode) != 0o600):
            raise ValueError('Manager log must be an owned single-link 0600 regular file')
        os.fchown(fd, *owner)
        return fd
    except BaseException:
        os.close(fd)
        raise


# Keep in step with headunit::theme::display_response_policy.
DISPLAY_RESPONSE_MODES = ('smooth', 'balanced', 'fast')
GREETING_VARIANTS = ('off', 'brand', 'minimal')
GREETING_CLOCK = time.CLOCK_MONOTONIC_RAW if sys.platform == 'darwin' else time.CLOCK_MONOTONIC


def display_response_read(path, owner=None, choices=DISPLAY_RESPONSE_MODES):
    """Return the saved mode, or None when absent or not a valid private choice."""
    if not path:
        return None
    try:
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or not 0 < info.st_size <= 16 or
                info.st_uid != (owner[0] if owner else os.getuid()) or info.st_mode & 0o077):
            return None
        content = os.read(fd, 17)
        if len(content) != info.st_size:
            return None
        value = content.decode('ascii')
    except (OSError, UnicodeError):
        return None
    finally:
        os.close(fd)
    value = value[:-1] if value.endswith('\n') else value
    return value if value in choices else None


def display_response_write(path, mode, owner=None, choices=DISPLAY_RESPONSE_MODES):
    """Atomically replace the choice; `owner` (uid, gid) is the reading display account.

    The renderer accepts only a 0600 regular file owned by its own account.
    """
    if mode not in choices:
        raise ValueError('Invalid display response')
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.display-response-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='ascii') as output:
            os.fchmod(output.fileno(), 0o600)
            if owner is not None:
                os.fchown(output.fileno(), *owner)
            output.write(mode + '\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def startup_warning_default(profile):
    path = layout_path(profile, 'warningOverlayProfile')
    if not path:
        return None
    try:
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
                    info.st_mode & 0o022 or info.st_uid not in (0, os.getuid()) or
                    not 0 < info.st_size <= 65536):
                return None
            content = os.read(fd, 65537)
            if len(content) != info.st_size:
                return None
            value = json.loads(content)
        finally:
            os.close(fd)
        hide = value.get('hideDuringStartup', True)
        return hide if value.get('enabled') is True and type(hide) is bool else None
    except (OSError, ValueError, AttributeError, RecursionError):
        return None


def native_instrument_configuration(settings, owner, binding=None):
    """Admit original configuration once at startup; never an applied receipt.

    Only the existing C++ parser interprets native configuration. No profile
    path/content or acquisition/phone/display authority enters this projection.
    Omission leaves explicit configuration null, with documented C++ bench defaults.
    """
    path = settings.get('native_cluster_config')
    result = {'configured': bool(path), 'state': 'not_configured',
              'source': 'source_configuration' if path else 'compatibility_defaults',
              'configurationSha256': None, 'configuration': None, 'error': None}
    if not path:
        return result
    result.update(state='unavailable', error='configuration_unavailable')
    try:
        from appliance_release import read_file, read_configuration_member, verify
        path = Path(path)
        checker = Path(settings['bin_dir']) / 'headunit-vehicle-config-check'

        def identity(info):
            return (info.st_dev, info.st_ino, info.st_uid, info.st_gid, info.st_mode,
                    info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)

        def capture():
            if binding:
                manifest = verify(binding)
                if (owner != (manifest['uid'], manifest['gid']) or
                        checker != Path(binding['path']) / 'build/headunit-vehicle-config-check'):
                    raise ValueError('Native summary differs from its sealed owner/helper role')
                raw = read_configuration_member(path, binding, 'native_cluster')
            else:
                if os.geteuid() not in (0, owner[0]):
                    raise ValueError('Native summary account mismatch')
                raw, _ = read_file(path, 4096, {owner[0]}, private=True)
            before = checker.lstat()
            binary, info = read_file(checker, 128 * 1024 * 1024,
                                     {manifest['administrator_uid']} if binding else {owner[0]})
            if (identity(before) != identity(info) or not info.st_mode & 0o111 or
                    info.st_mode & 0o7000):
                raise ValueError('Unsafe native summary helper')
            return raw, hashlib.sha256(binary).digest(), identity(info)

        # Also pin each named entry across parsing; identical replacement files
        # do not turn this startup admission into a refresh mechanism.
        original_named = identity(path.lstat())
        raw, helper_hash, helper_identity = capture()
        options = {'user': owner[0], 'group': owner[1], 'extra_groups': ()} if os.geteuid() == 0 else {}
        result['error'] = 'validation_unavailable'
        with tempfile.TemporaryFile() as output:
            subprocess.run([str(checker), '--native-summary'], input=raw, stdout=output,
                           stderr=subprocess.DEVNULL, check=True, timeout=10, **options)
            output.seek(0)
            summary_raw = output.read(1025)
        if len(summary_raw) > 1024:
            raise ValueError('Oversized native summary')
        def unique(pairs):
            value = {}
            for key, item in pairs:
                if key in value:
                    raise ValueError('Duplicate native summary field')
                value[key] = item
            return value
        summary = json.loads(summary_raw, object_pairs_hook=unique)
        if (not isinstance(summary, dict) or
                set(summary) != {'speedUnit', 'temperatureUnit', 'distanceUnit', 'fuelConsumptionUnit', 'speedPrecision', 'pressureUnit', 'temperaturePrecision', 'speedMaximumKmh', 'rpmMaximum', 'rpmRedline', 'showRpm'} or
                type(summary['speedUnit']) is not str or summary['speedUnit'] not in ('kmh', 'mph') or
                type(summary['temperatureUnit']) is not str or summary['temperatureUnit'] not in ('celsius', 'fahrenheit') or
                type(summary['distanceUnit']) is not str or summary['distanceUnit'] not in ('kilometers', 'miles') or
                type(summary['fuelConsumptionUnit']) is not str or summary['fuelConsumptionUnit'] not in ('liters_per_100km', 'us_mpg', 'imperial_mpg') or
                type(summary['pressureUnit']) is not str or summary['pressureUnit'] not in ('kpa', 'bar', 'psi') or
                type(summary['temperaturePrecision']) is not str or summary['temperaturePrecision'] not in ('whole', 'one_decimal') or
                type(summary['speedPrecision']) is not str or summary['speedPrecision'] not in ('whole', 'one_decimal') or
                type(summary['showRpm']) is not bool or
                any(type(summary[key]) not in (int, float) or not math.isfinite(summary[key])
                    for key in ('speedMaximumKmh', 'rpmMaximum', 'rpmRedline'))):
            raise ValueError('Malformed native summary')
        result['error'] = 'admission_changed'
        if (capture() != (raw, helper_hash, helper_identity) or
                original_named != identity(path.lstat()) or helper_identity != identity(checker.lstat())):
            raise ValueError('Native configuration/helper changed during admission')
        result.update(state='configured', configurationSha256=hashlib.sha256(raw).hexdigest(),
                      configuration=summary, error=None)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, subprocess.SubprocessError, RecursionError):
        pass  # Optional read-only data failure cannot retire working owners.
    return result


class Manager:
    def __init__(self, config, folder, standard_display, owner, bluez=None, fanout_config=None,
                 receiver_lock_fd=None, pairing=None, release_binding=None, manager_profile=None,
                 recovery_attempts=0):
        if type(recovery_attempts) is not int or not 0 <= recovery_attempts <= 3:
            raise ValueError('Receiver recovery attempts must be an integer from zero to three')
        self.recovery_attempts = recovery_attempts
        self.recovery_phone = None
        self.recovery_remaining = 0
        self.recovery_at = None
        self.folder, self.owner = Path(folder), owner
        self.page_adopted = False
        if receiver_lock_fd is not None and (type(receiver_lock_fd) is not int or receiver_lock_fd < 0):
            raise ValueError('Receiver owner descriptor must be an actual open file descriptor')
        self.receiver_lock_fd = receiver_lock_fd
        self.pairing = pairing
        self.release_binding = dict(release_binding) if release_binding else None
        self.manager_profile = Path(manager_profile) if manager_profile else None
        self.config_path = Path(config)
        if self.release_binding:
            if not self.manager_profile or not owner:
                raise ValueError('Sealed manager needs its profile and display account')
            manifest, self.base, self.profile = sealed_manager_configuration(
                self.config_path, self.manager_profile, self.release_binding, owner[0])
            if (tuple(owner) != (manifest['uid'], manifest['gid']) or
                    Path(folder) != Path(self.profile['stateDirectory']) or
                    Path(standard_display) != Path(self.profile['standardDisplayConfig'])):
                raise ValueError('Manager runtime paths/account differ from the sealed profile')
            if recovery_attempts != self.profile.get('receiverRecoveryAttempts', 0):
                raise ValueError('Receiver recovery policy differs from the sealed profile')
            verify_manager_lock(receiver_lock_fd, folder, owner[0])
        else:
            self.base = load_receiver_configuration(self.config_path)
        self.native_instruments = native_instrument_configuration(self.base, self.owner, self.release_binding)
        self.greeting_path = self.base.get('greeting_sequence_file')
        if self.greeting_path and Path(self.greeting_path) != self.folder / 'greeting-sequence':
            raise ValueError('Greeting preference must be in the manager state directory')
        self.layout_path = layout_path(self.base.get('ultra_installed_ui'))
        self.drive_modes = DriveModeControl(layout_path(self.base.get('ultra_installed_ui'), 'driveModeProfilesFile') or
                                            layout_path(self.base.get('native_cluster_config'), 'drive_mode_profiles_file'), self.owner)
        self.display_response_path = layout_path(self.base.get('ultra_installed_ui'), 'displayResponseFile')
        self.startup_warning_lights_path = layout_path(self.base.get('ultra_installed_ui'), 'startupWarningLightsFile')
        self.startup_warning_hide_default = startup_warning_default(self.base.get('ultra_installed_ui'))
        self.content = ContentControl(self.base.get('car_settings_config'), folder, owner)
        self.standard_display = Path(standard_display).resolve()
        if not self.standard_display.is_file():
            raise ValueError('Standard display configuration is missing')
        self.state = PhoneState(self.folder / 'phones.json', owner)
        self.bluez = bluez or BluezPhones()
        # Preserve the preferred phone; session mode belongs to the phone.
        previous = self.base.get('auto_connect_phone')
        if not self.state.path.exists() and previous:
            try:
                self.bluez.phone(previous)
            except ValueError:
                pass
            else:
                self.state.select(previous)
        self.media = MediaState(self.folder / 'media.json', owner)
        self.media_observations = MediaObservations()
        self.iap_media_path = self.media.path.with_name(self.media.path.name + '.iap')
        self.iap_art_path = self.iap_media_path.with_name(self.iap_media_path.name + '.art')
        self.iap_art_hash = ''
        self.media.publish({'available': False})
        self.child = None
        self.retirement_required = False
        self.carplay = CarplayReadiness(self.folder.resolve(), owner)
        self.active = None
        self.active_mode = None
        self.error = ''
        self.lock = threading.RLock()
        self.recovery_decisions = deque(maxlen=8)
        self.generation = 0
        self.audio = (AudioControl(self.folder, owner, self.base.get('audio_volume_percent', 100),
                                   self.base.get('audio_muted', False))
                      if self.base.get('audio_sink', 'none') != 'none' else None)
        if self.audio:
            self.audio.reset()
        self.external_until = 0
        self.fallback = None
        self.fallback_error = ''
        self.fallback_retry_at = 0
        self.fallback_wayland, self.fallback_size = ('', None)
        self.fallback_vehicle_args = []
        if self.base.get('fallback_cluster'):
            self.fallback_wayland, self.fallback_size = cluster_display(self.base['display_config'])
            if (self.fallback_wayland, self.fallback_size) != cluster_display(self.standard_display):
                raise ValueError('Standard and Ultra cluster displays must match for fallback')
            self.fallback_vehicle_args = vehicle_arguments(self.base, owner[0] if owner else os.getuid(), fanout_config)
        if self.fallback_wayland and not self.base.get('bin_dir'):
            raise ValueError('Backup cluster needs the receiver binary directory')
        # Created last: constructor failures above cannot orphan its worker.
        # Its independent small-lock worker never waits on phone operations.
        if self.base.get('camera_manual_profile'):
            from camera_manual import load_configuration
            manual = load_configuration(self.base['camera_manual_profile'], owner)
            prepare_directory(Path(manual['requestFile']).parent, owner or (os.getuid(), os.getgid()))
        self.camera = ManualCamera(self.base['camera_manual_profile'], owner) if self.base.get('camera_manual_profile') else None
        self.prephone = None
        self.prephone_error = None
        self.open_prephone_publication(fanout_config)
        self.sport = None
        if self.base.get('sport_telemetry', False):
            try:
                from sport_source import SportSource
                sport_socket = None
                if fanout_config:
                    from vehicle_fanout import load_configuration
                    sport_socket = load_configuration(fanout_config, owner[0] if owner else os.getuid(),
                                                      self.release_binding)['consumers'].get('sport')
                    if not sport_socket:
                        raise ValueError('Sport telemetry needs its own fan-out sport consumer')
                self.sport = SportSource(folder, bool(self.base.get('simulate_vehicle', False) or
                                                      self.base.get('simulate_dated_vehicle', False)), sport_socket, owner,
                                         trip_history=self.base.get('trip_history', False))
                self.sport.start()
            except BaseException:
                if self.prephone:
                    self.prephone.close()
                if self.camera:
                    self.camera.close()
                raise

    def open_prephone_publication(self, fanout_config):
        if not self.base.get('prephone_publication_socket'):
            return
        try:
            if not self.release_binding:
                raise ValueError('Native publication requires exact sealed role admission')
            from install_cm5_appliance import cluster_configuration, native_cluster_command
            from prephone_native import NativePublication
            root = Path(self.release_binding['path'])
            cluster = cluster_configuration(self.base, root / 'build/headunit-display-config-check')
            arguments = vehicle_arguments(self.base, self.owner[0], fanout_config, self.release_binding)
            command = native_cluster_command(root, self.base, cluster, arguments)
            self.prephone = NativePublication(self.base['prephone_publication_socket'], self.owner, command,
                                              self.base.get('ambient_light', False),
                                              indicators_enabled=self.base.get('prephone_indicators', False))
        except (OSError, ValueError, subprocess.SubprocessError):
            # Optional data publication cannot retire working camera/radio/phone.
            self.prephone_error = 'publication_unavailable'

    def unavailable_indicators(self):
        # Disabled graphs never import/read/poll an optional indicator module.
        configured = bool(self.base.get('prephone_indicators', False))
        return {'configured': configured, 'version': 1,
                'state': 'publication_unavailable' if configured else 'disabled',
                'clockDomain': None, 'sourceGeneration': None, 'sourceStartedNs': None,
                'sourceSampledNs': None, 'renderedNs': None, 'copiedNs': None,
                'publicationSequence': None, 'rows': [], 'publisher': None,
                'error': 'publication_unavailable' if configured else None}

    def ambient_snapshot(self):
        if self.base.get('ambient_light', False) and getattr(self, 'prephone', None):
            return self.prephone.ambient_snapshot()
        # Keep the disabled graph independent of optional ambient imports.
        return {'configured': bool(self.base.get('ambient_light', False)),
                'state': 'publication_unavailable' if self.base.get('ambient_light', False) else 'disabled',
                'available': False, 'provider_available': False, 'settings': None,
                'requested': None, 'current': None, 'command': None, 'revision': None}

    def maintain_fallback(self):
        if not self.fallback_wayland:
            return
        if self.fallback and self.fallback.poll() is None:
            return
        if self.fallback:
            self.fallback_error = f'Backup cluster stopped ({self.fallback.returncode})'
            self.fallback = None
            self.fallback_retry_at = time.monotonic() + 2
        if time.monotonic() < self.fallback_retry_at:
            return
        self.require_launcher_retired()
        if not self.owner:
            raise RuntimeError('Backup cluster needs an owner account')
        uid, gid = self.owner
        runtime = Path('/run/user') / str(uid)
        socket_path = runtime / self.fallback_wayland
        try:
            runtime_info, socket_info = runtime.lstat(), socket_path.lstat()
            if not stat.S_ISDIR(runtime_info.st_mode) or runtime_info.st_uid != uid or runtime_info.st_mode & 0o077 or \
                    socket_info.st_uid != uid or not stat.S_ISSOCK(socket_info.st_mode):
                raise ValueError('Fallback Wayland socket has an unexpected owner or type')
            binary = Path(self.base['bin_dir']) / 'headunit-cluster-wayland'
            if not binary.is_file():
                raise ValueError('Build the independent backup cluster first')
            command = self.fallback_command(binary)
            log_fd = open_owned_log(self.folder / 'fallback-cluster.log', (uid, gid))
            try:
                with os.fdopen(log_fd, 'ab', buffering=0) as log:
                    log_fd = -1
                    account = pwd.getpwuid(uid)
                    env = dict(os.environ, HOME=account.pw_dir, USER=account.pw_name, LOGNAME=account.pw_name,
                               XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY=self.fallback_wayland)
                    self.fallback = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                                     stderr=subprocess.STDOUT, env=env, user=uid, group=gid)
            finally:
                if log_fd >= 0:
                    os.close(log_fd)
            self.fallback_error = ''
        except (OSError, ValueError) as error:
            self.fallback_error = str(error)[:200]
            self.fallback_retry_at = time.monotonic() + 2

    def fallback_command(self, binary):
        if not self.fallback_size:
            raise ValueError('Backup cluster is not configured')
        command = [str(binary), '--size', *(str(size) for size in self.fallback_size)]
        command.extend(('--media-file', str(self.media.path)))
        if self.base.get('cluster_page'):
            command.extend(('--page-file', str(self.base['cluster_page'])))
        if self.greeting_path:
            command.extend(('--greeting-sequence-file', str(self.greeting_path)))
        if self.base.get('native_cluster_config'):
            command.extend(('--native-cluster-config', str(self.base['native_cluster_config'])))
        command.extend(self.fallback_vehicle_args)
        return command

    def shutdown(self):
        if getattr(self, 'prephone', None):
            self.prephone.close()
        if getattr(self, 'sport', None):
            self.sport.close()
        if self.pairing:
            self.pairing.close() # Reject pending pairing before slow phone cleanup.
        try:
            try:
                if self.camera:
                    self.camera.close() # Withdraw manual intent before slow receiver stop.
            finally:
                self.stop()
        finally:
            try:
                if self.fallback and self.fallback.poll() is None:
                    self.fallback.terminate()
                    try:
                        self.fallback.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.fallback.kill()
                        self.fallback.wait(timeout=5)
                self.fallback = None
            finally:
                if self.pairing:
                    # Final shutdown retains endpoint ownership until the
                    # temporary radio/default-agent owner has retired.
                    self.pairing.wait_closed()

    def stop(self):
        if getattr(self, 'camera', None):
            try:
                self.camera.request(False)
            except (OSError, ValueError):
                # A failed manual worker's timed intent expires independently.
                # It must never prevent retirement of the phone/display owners.
                pass
        if getattr(self, 'content', None):
            try:
                self.content.clear()
            except (OSError, ValueError):
                # Its short lease withdraws independently; failed publication
                # cannot block complete phone/display retirement.
                pass
        self.cancel_recovery()
        if self.child:
            self.observe_launcher_exit()
            self.child.terminate()
            try:
                code = self.child.wait(timeout=15)
            except subprocess.TimeoutExpired:
                # The launcher owns cleanup of AP and its process groups. Do not
                # start a competing receiver if cleanup has not completed.
                raise RuntimeError('Receiver is still stopping; try again shortly')
            if type(code) is int and code < 0:
                self.retirement_required = True
            self.child = None
        self.active = self.active_mode = None
        self.carplay.reset()
        self.generation += 1
        self.external_until = 0
        self.media.publish({'available': False})
        self.media_observations.reset()
        self.iap_media_path.unlink(missing_ok=True)
        self.iap_art_path.unlink(missing_ok=True)
        self.iap_art_hash = ''
        if self.audio:
            self.audio.reset()
        if self.retirement_required:
            self.error = RETIREMENT_REQUIRED

    def launcher_exit_code(self):
        child = self.child
        code = child.poll() if child else None
        if type(code) is int and code < 0:
            self.retirement_required = True
        if self.retirement_required:
            self.error = RETIREMENT_REQUIRED
        return code

    def observe_launcher_exit(self):
        self.launcher_exit_code()
        return self.retirement_required

    def require_launcher_retired(self):
        if self.observe_launcher_exit():
            raise RuntimeError(RETIREMENT_REQUIRED)

    def _record_recovery_decision(self, decision, remaining, *, exit_code=None, observed=None):
        """Record an existing owner decision, never a phone or presentation receipt."""
        with self.lock:
            if observed is None:
                observed = time.monotonic()
            clock_known = type(observed) is float and math.isfinite(observed) and observed >= 0
            self.recovery_decisions.append({
                'decision': decision,
                'generation': self.generation,
                'configuredMaximum': self.recovery_attempts,
                'remainingAttempts': remaining,
                'exitCode': exit_code,
                'clockDomain': 'python-time.monotonic' if clock_known else None,
                'observedMonotonicSeconds': observed if clock_known else None})

    def cancel_recovery(self, decision='cancelled'):
        with self.lock:
            if self.recovery_at is not None:
                if decision is not None:
                    self._record_recovery_decision(decision, self.recovery_remaining)
                self.error = ''
            self.recovery_phone = None
            self.recovery_remaining = 0
            self.recovery_at = None

    def recovery_schedule_snapshot(self, observed):
        """Copy scheduling intent using the caller's existing locked status date."""
        queued = self.recovery_attempts > 0 and self.recovery_at is not None
        clock_known = type(observed) is float and math.isfinite(observed) and observed >= 0
        deadline = self.recovery_at
        deadline_known = (queued and clock_known and type(deadline) is float and
                          math.isfinite(deadline) and deadline >= 0)
        return {'authority': 'software_scheduling_intent',
                'configuredMaximum': self.recovery_attempts,
                'queued': queued,
                'state': 'disabled' if self.recovery_attempts == 0 else 'queued' if queued else 'idle',
                'clockDomain': 'python-time.monotonic' if clock_known else None,
                'observedMonotonicSeconds': observed if clock_known else None,
                'deadlineMonotonicSeconds': deadline if deadline_known else None,
                'remainingWaitSeconds': max(0.0, deadline - observed) if deadline_known else None}

    def start(self, phone, *, recovery_remaining=None):
        self.require_launcher_retired()
        self.require_pairing_idle()
        phone = address(phone)
        self.bluez.phone(phone)
        launch_nonce = secrets.token_hex(16)
        if self.release_binding:
            # Re-read the pinned release before every phone change. Only
            # bounded selectors travel to the child; writable derived JSON is
            # never a source of privileged program or endpoint configuration.
            manifest, base, profile = sealed_manager_configuration(
                self.config_path, self.manager_profile, self.release_binding, self.owner[0])
            if base != self.base or profile != self.profile or (manifest['uid'], manifest['gid']) != tuple(self.owner):
                raise ValueError('Sealed manager configuration/account changed')
            verify_manager_lock(self.receiver_lock_fd, self.folder, self.owner[0])
            command = ['/usr/bin/python3', '-B', '-E', '-s', '-u',
                       str(Path(self.release_binding['path']) / 'tools/run_lab_receiver.py'),
                       '--release-root', self.release_binding['path'], '--release-manifest-sha256', self.release_binding['sha256'],
                       '--config', str(self.config_path), '--manager-profile', str(self.manager_profile),
                       '--managed-phone', phone, '--manager-lock-fd', str(self.receiver_lock_fd),
                       '--managed-runtime-status-nonce', launch_nonce]
            if self.audio:
                audio = self.audio.configuration()
                command.extend(('--managed-audio-volume-percent', str(audio['audio_volume_percent']),
                                '--managed-audio-muted', 'true' if audio['audio_muted'] else 'false'))
            validation_inheritance = {'pass_fds': (self.receiver_lock_fd,)}
        else:
            config = receiver_settings(self.base, phone, self.standard_display, self.media.path)
            config.update(runtime_status_file=str(self.carplay.path), runtime_status_nonce=launch_nonce)
            if self.audio:
                config.update(self.audio.configuration())
            config = {k: str(v) if isinstance(v, Path) else v for k, v in config.items()}
            effective = self.folder / 'managed-receiver.json'
            atomic_json(effective, config, self.owner)
            command = ['/usr/bin/python3', str(Path(__file__).with_name('run_lab_receiver.py')),
                       '--config', str(effective)]
            validation_inheritance = {}
        # Validate prior to interrupting a current session.
        env = dict(os.environ, SUDO_UID=str(self.owner[0]), SUDO_GID=str(self.owner[1]))
        subprocess.run([*command, '--dry-run'], env=env, check=True,
                       capture_output=True, text=True, timeout=15, **validation_inheritance)
        self.stop()
        self.require_launcher_retired()
        self.bluez.disconnect(phone)
        self.state.select(phone)
        log_fd = open_owned_log(self.folder / 'receiver-manager.log', self.owner)
        log = os.fdopen(log_fd, 'ab', buffering=0)
        env = dict(os.environ, SUDO_UID=str(self.owner[0]), SUDO_GID=str(self.owner[1]))
        try:
            inheritance = {'pass_fds': (self.receiver_lock_fd,)} if self.receiver_lock_fd is not None else {}
            self.child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                          stdin=subprocess.DEVNULL, env=env, **inheritance)
        finally:
            log.close()
        self.active, self.active_mode = phone, None
        self.carplay.reset(launch_nonce)
        self.error = ''
        self.recovery_phone = phone
        self.recovery_remaining = (self.recovery_attempts if recovery_remaining is None
                                   else recovery_remaining)
        self.recovery_at = None
        if recovery_remaining is not None:
            self._record_recovery_decision('child_started', self.recovery_remaining)
        # The bounded reconnect helper performs the actual request after AP setup.

    def restore_selected(self):
        """One explicitly configured boot attempt; selection is not connection proof."""
        if not self.state.selected:
            return False
        try:
            self.start(self.state.selected)
            return True
        except Exception as error:
            self.error = ('Preferred phone could not start; choose Connect to retry: ' + str(error))[:300]
            return False

    def tick(self):
        with self.lock:
            self.observe_launcher_exit()
            content = getattr(self, 'content', None)
            if content and content.path:
                status = self.carplay_snapshot()
                publisher = ((self.carplay.nonce, self.carplay.last['connectionGeneration'])
                             if self.active_mode == 'ultra' and status['returnReady'] else None)
                try:
                    content.maintain(publisher)
                except (OSError, ValueError):
                    content.selected = None
                    content.publisher = None
            if self.child and self.launcher_exit_code() is not None:
                code = self.child.returncode
                # Use the same complete retirement as explicit Disconnect;
                # an adapter hold cannot keep a dead receiver's media alive.
                phone, remaining = self.recovery_phone, self.recovery_remaining
                self.stop()
                # Signal death can bypass launcher cleanup and leave radio/display
                # children alive. Leave that case to whole-service retirement.
                if self.retirement_required:
                    # The supervisor must retire the entire existing control
                    # group before a new manager can admit radio/display owners.
                    self._record_recovery_decision('retirement_required', remaining, exit_code=code)
                    raise RuntimeError(RETIREMENT_REQUIRED)
                if code > 0 and remaining and phone == self.state.selected:
                    delay = 2 << (self.recovery_attempts - remaining)
                    self.recovery_phone, self.recovery_remaining = phone, remaining
                    observed = time.monotonic()
                    self.recovery_at = observed + delay
                    self._record_recovery_decision('queued', remaining, exit_code=code, observed=observed)
                    self.error = f'Receiver stopped ({code}); retrying in {delay} seconds'
                else:
                    decision = ('budget_exhausted' if code > 0 and self.recovery_attempts and
                                not remaining and phone == self.state.selected else 'not_eligible')
                    self._record_recovery_decision(decision, remaining, exit_code=code)
                    self.error = f'Receiver stopped ({code}); choose Connect to retry'
                return
            self.require_launcher_retired()
            if getattr(self, 'prephone', None):
                self.prephone.poll()
            self.maintain_fallback()
            if self.recovery_at is not None:
                phone, remaining = self.recovery_phone, self.recovery_remaining
                if self.child or phone != self.state.selected:
                    self.cancel_recovery('cancelled_owner' if self.child else 'cancelled_selection')
                else:
                    try:
                        self.require_pairing_idle()
                    except ValueError:
                        self.cancel_recovery('cancelled_pairing')
                        return
                    if time.monotonic() >= self.recovery_at:
                        # A failed preflight/launch stops recovery; only an actual
                        # owned-child failure consumes another automatic attempt.
                        self._record_recovery_decision('dispatch', remaining)
                        self.cancel_recovery(None)
                        try:
                            self.start(phone, recovery_remaining=remaining - 1)
                        except Exception as error:
                            self.cancel_recovery()
                            self._record_recovery_decision('attempt_failed', remaining)
                            self.error = ('Receiver recovery failed; choose Connect to retry: ' + str(error))[:300]
                        return
            if time.monotonic() < self.external_until:
                return
            media_phone, media_child, media_generation = self.active, self.child, self.generation

            def publish_media(value, artwork=None, keep_artwork=False):
                # BlueZ lookup can outlive the receiver or reenter this RLock.
                # A late copy must neither revive a dead owner in media.json
                # nor overwrite the successor's useful metadata/artwork.
                if (self.active != media_phone or self.child is not media_child or
                        self.generation != media_generation):
                    return False
                live = bool(media_phone and media_child and self.launcher_exit_code() is None)
                if not live:
                    value = {'available': False}
                    artwork = None
                    keep_artwork = False
                self.media.publish(value, artwork, keep_artwork=keep_artwork)
                return live

            if self.active and self.child and self.launcher_exit_code() is None:
                try:
                    fd = os.open(self.iap_media_path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
                    try:
                        info = os.fstat(fd)
                        if (not stat.S_ISREG(info.st_mode) or not self.owner or
                                info.st_uid != self.owner[0] or info.st_nlink != 1 or
                                info.st_mode & 0o7022 or not 0 < info.st_size <= 8192 or
                                not 0 <= time.time() - info.st_mtime <= 3):
                            raise ValueError('Stale or invalid iAP media snapshot')
                        content = os.read(fd, 8193)
                        after = os.fstat(fd)
                        if ((info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_uid,
                             info.st_mode, info.st_nlink) !=
                                (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_uid,
                                 after.st_mode, after.st_nlink)):
                            raise ValueError('iAP media snapshot changed during read')
                    finally:
                        os.close(fd)
                    if len(content) == info.st_size:
                        raw = json.loads(content)
                        if not isinstance(raw, dict):
                            raise ValueError('Invalid iAP media snapshot schema')
                        art_hash = raw.pop('iap_artwork_sha256', '')
                        if (not isinstance(art_hash, str) or
                                art_hash and (len(art_hash) != 64 or
                                              any(c not in '0123456789abcdef' for c in art_hash))):
                            raise ValueError('Invalid iAP artwork hash')
                        # Observation is optional: malformed/foreign scope never
                        # hides otherwise usable legacy metadata or capabilities.
                        observation = raw.pop('iap_observation', None)
                        value = validate_media(raw)
                        try:
                            observation = validate_iap_observation(observation)
                            if observation and observation['launchNonce'] == self.carplay.nonce:
                                value['iap_observation'] = observation
                        except ValueError:
                            pass
                        if value['available']:
                            value['source'] = 'iap'
                            if (self.media.last or {}).get('source') != 'iap':
                                self.iap_art_hash = ''
                            # iAP supplies the better track data. BlueZ may
                            # independently expose AVRCP commands for this
                            # same selected phone; use only capabilities it
                            # actually reports, never inferred commands.
                            try:
                                bluetooth = validate_media(self.bluez.media(self.active))
                                if bluetooth['available']:
                                    value['controls'] = bluetooth['controls']
                                    if 'volume' in bluetooth:
                                        value['volume'] = bluetooth['volume']
                            except (OSError, ValueError, TypeError):
                                pass
                            cover = None
                            if art_hash and art_hash != self.iap_art_hash:
                                try:
                                    art_fd = os.open(self.iap_art_path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
                                    try:
                                        art_info = os.fstat(art_fd)
                                        if (not stat.S_ISREG(art_info.st_mode) or
                                                art_info.st_uid != self.owner[0] or art_info.st_nlink != 1 or
                                                art_info.st_mode & 0o7022 or
                                                not 24 <= art_info.st_size <= 1048576):
                                            raise ValueError('Invalid iAP artwork file')
                                        art_bytes = os.read(art_fd, 1048577)
                                    finally:
                                        os.close(art_fd)
                                    if (len(art_bytes) != art_info.st_size or
                                            hashlib.sha256(art_bytes).hexdigest() != art_hash):
                                        raise ValueError('iAP artwork file changed')
                                    cover = base64.b64encode(art_bytes).decode('ascii')
                                except (OSError, ValueError):
                                    art_hash = ''
                            try:
                                if not publish_media(value, cover,
                                                     keep_artwork=bool(art_hash) and cover is None and
                                                     art_hash == self.iap_art_hash):
                                    return
                            except ValueError:
                                # A malformed image must not hide valid track
                                # and playback information.
                                art_hash = ''
                                if not publish_media(value):
                                    return
                            self.iap_art_hash = art_hash
                            return
                except (OSError, ValueError, TypeError, RecursionError):
                    pass
            try:
                value = self.bluez.media(self.active) if self.active else {'available': False}
                publish_media(value)
            except Exception:
                publish_media({'available': False})

    def audio_snapshot(self):
        if (not self.audio or not self.active or self.state.selected != self.active or
                not self.child or self.launcher_exit_code() is not None):
            return audio_unavailable()
        return self.audio.snapshot()

    def carplay_snapshot(self):
        value = self.carplay.snapshot(bool(self.active and self.state.selected == self.active and
                                           self.child and self.launcher_exit_code() is None))
        self.active_mode = self.carplay.negotiated_mode
        return value

    def require_pairing_idle(self):
        if self.pairing and self.pairing.snapshot()['phase'] in ('starting', 'discoverable', 'confirmation', 'stopping'):
            raise ValueError('Finish or cancel adding a phone before changing the receiver')

    def camera_snapshot(self):
        if not self.camera:
            return {'configured': False, 'requested': False, 'phase': 'unavailable', 'error': ''}
        if self.base.get('camera_state_config') and (
                self.active_mode == 'standard' or self.child is None and not self.retirement_required):
            # The signed camera has no separate display owner. A fully retired
            # launcher, or negotiated Standard session, cannot retain its pixels.
            return {'configured': False, 'requested': False, 'phase': 'stopped', 'error': ''}
        return self.camera.snapshot()

    def camera_snapshot_with_status_observation(self):
        if not self.camera:
            return self.camera_snapshot(), unavailable_status_observation()
        if self.base.get('camera_state_config') and (
                self.active_mode == 'standard' or self.child is None and not self.retirement_required):
            return self.camera_snapshot(), unavailable_status_observation('suppressed')
        camera, observation = self.camera.snapshot_with_status_observation()
        if self.base.get('camera_state_config') and self.retirement_required:
            observation = unavailable_status_observation('suppressed')
        return camera, observation

    def request(self, data):
        if not isinstance(data, dict) or set(data) - {'action', 'phone', 'command', 'generation', 'media', 'artwork', 'page', 'layout', 'publisher', 'palette', 'zone', 'component', 'route', 'volumePercent', 'muted', 'enabled', 'session', 'prompt', 'accepted', 'mode', 'hide', 'app', 'renderer', 'format', 'variant', 'phase', 'profile', 'revision', 'line', 'settings'}:
            raise ValueError('Invalid manager request')
        action = data.get('action')
        if action != 'ambient_light_settings' and 'settings' in data:
            raise ValueError('Ambient settings require an ambient software operation')
        if action != 'cluster_format' and {'renderer', 'format'} & set(data):
            raise ValueError('Cluster format fields require a cluster format action')
        if action in ('sport_line', 'sport_session', 'trip_reset'):
            if not getattr(self, 'sport', None):
                raise ValueError('Sport telemetry is not configured')
            return {'sport': self.sport.request(data)}
        if 'line' in data:
            raise ValueError('Sport line is not a phone operation')
        self.observe_launcher_exit()
        if action not in ('cluster_layout', 'cluster_format', 'cluster_content', 'cluster_input', 'phone_app') and {'publisher', 'palette', 'zone', 'component'} & set(data):
            raise ValueError('Cluster options are not a phone or media operation')
        if action != 'phone_app' and 'app' in data:
            raise ValueError('Invalid manager request')
        pairing_fields = {'session', 'prompt', 'accepted'}
        if action not in ('pairing_start', 'pairing_cancel', 'pairing_confirm') and pairing_fields & set(data):
            raise ValueError('Pairing confirmation is not a phone or media operation')
        if action in ('pairing_start', 'pairing_cancel', 'pairing_confirm'):
            fields = {'pairing_start': {'action'}, 'pairing_cancel': {'action', 'session'},
                      'pairing_confirm': {'action', 'session', 'prompt', 'accepted'}}[action]
            if set(data) != fields or (action == 'pairing_confirm' and type(data['accepted']) is not bool):
                raise ValueError('Pairing commands require their complete current session fields')
            if not self.pairing:
                raise ValueError('Adding phones is not configured')
            if action == 'pairing_start':
                with self.lock:
                    self.require_launcher_retired()
                    if self.child and self.launcher_exit_code() is None:
                        raise ValueError('Disconnect CarPlay before adding another phone')
                    self.require_launcher_retired()
                    self.pairing.start()
                    self.cancel_recovery('cancelled_pairing')
                return {'message': 'Preparing Bluetooth pairing; follow the phone page'}
            if action == 'pairing_cancel':
                self.pairing.cancel(data['session'])
                return {'message': 'Closing pairing'}
            self.pairing.confirm(data['session'], data['prompt'], data['accepted'])
            return {'message': 'Code confirmed; waiting for Bluetooth pairing' if data['accepted'] else 'Pairing rejected'}
        if action not in ('greeting_sequence', 'greeting_preview') and {'variant', 'phase'} & set(data):
            raise ValueError('Greeting options are not a phone operation')
        if action in ('greeting_sequence', 'greeting_preview'):
            if not self.greeting_path:
                raise ValueError('Greeting sequence is not configured')
            if action == 'greeting_sequence':
                if set(data) != {'action', 'variant'} or data['variant'] not in GREETING_VARIANTS:
                    raise ValueError('Choose Off, Linux Head Unit or Minimal')
                display_response_write(self.greeting_path, data['variant'], self.owner, GREETING_VARIANTS)
                directory = os.open(Path(self.greeting_path).parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
                return {'message': 'Greeting preference saved; applies to the next sequence'}
            if set(data) != {'action', 'phase'} or data['phase'] not in ('startup', 'goodbye'):
                raise ValueError('Choose a startup or goodbye preview')
            # Cosmetic intent acquired now, not an ignition observation or a
            # confirmed display/power command. Receivers expire it independently.
            event = data['phase'] + ' ' + str(time.clock_gettime_ns(GREETING_CLOCK))
            display_response_write(str(self.greeting_path) + '.event', event, self.owner, (event,))
            return {'message': 'Greeting preview requested', 'confirmed': False,
                    'provenance': 'native-preview'}
        if action == 'ambient_light_settings':
            with self.lock:
                if not self.base.get('ambient_light', False) or not getattr(self, 'prephone', None):
                    raise ValueError('Ambient software settings are not configured')
                return self.prephone.ambient_change(data)
        if action in ('drive_mode', 'drive_mode_profile'):
            with self.lock:
                return self.drive_modes.change(action, data)
        if {'profile', 'revision'} & set(data):
            raise ValueError('Drive-mode fields require a drive-mode operation')
        if action != 'display_response' and 'mode' in data:
            raise ValueError('Display response is not a phone operation')
        if action == 'display_response':
            if set(data) != {'action', 'mode'} or data['mode'] not in DISPLAY_RESPONSE_MODES:
                raise ValueError('Choose Smooth, Balanced or Fast')
            if not self.display_response_path:
                raise ValueError('Display response is not configured')
            display_response_write(self.display_response_path, data['mode'], self.owner)
            return {'message': 'Display response changed'}
        if action != 'startup_warning_lights' and 'hide' in data:
            raise ValueError('Startup light preference is not a phone operation')
        if action == 'startup_warning_lights':
            if set(data) != {'action', 'hide'} or type(data['hide']) is not bool:
                raise ValueError('Choose whether to hide startup warning lights')
            if not self.startup_warning_lights_path or self.startup_warning_hide_default is None:
                raise ValueError('Startup warning lights are not configured')
            display_response_write(self.startup_warning_lights_path,
                                   'hide' if data['hide'] else 'show', self.owner, ('hide', 'show'))
            return {'message': 'Startup warning lights hidden' if data['hide'] else 'Startup warning lights shown'}
        if action != 'camera' and 'enabled' in data:
            raise ValueError('Camera intent is not a phone operation')
        if action == 'camera':
            if set(data) != {'action', 'enabled'} or type(data['enabled']) is not bool:
                raise ValueError('Camera requests require one explicit Boolean')
            if not self.camera:
                raise ValueError('Manual camera controls are not configured')
            if data['enabled']:
                self.require_launcher_retired()
                if self.base.get('camera_state_config') and (self.active_mode != 'ultra' or not self.carplay_snapshot()['returnReady']):
                    raise ValueError('Connect CarPlay Ultra before opening the signed camera')
            self.camera.request(data['enabled'])
            return {'message': 'Camera requested' if data['enabled'] else 'Manual camera request cleared'}
        with self.lock:
            page_path = self.base.get('cluster_page')
            if page_path and self.owner and not self.page_adopted:
                try:
                    cluster_page_adopt(Path(page_path), self.owner); self.page_adopted = True
                except OSError:
                    pass  # Retried on the next request; reads still work for the manager.
            # BlueZ lookup is synchronous and can outlive the receiver or its
            # readiness publication. Derive status authority only after it
            # returns, using the current child and original publication date.
            if action == 'status':
                phones = self.bluez.phones()
            carplay = self.carplay_snapshot()
            map_available = bool(self.active_mode == 'standard' and carplay['returnReady'])
            styles_available = bool(self.active_mode == 'ultra' and carplay['returnReady'])
            layout_publisher = ((self.carplay.nonce, self.carplay.last['connectionGeneration'])
                                if styles_available else None)
            # Ultra media keys (HID) work without iAP/Bluetooth media controls.
            hid_media = bool(styles_available and self.layout_path)
            if action in ('status', 'cluster_format'):
                ultra_state = layout_snapshot(self.layout_path, self.owner, layout_publisher) if styles_available else None
                native_page = cluster_page_read(page_path, map_available) if page_path else None
                formats = format_snapshot(native_page, map_available, ultra_state)
            if action == 'status':
                for p in phones:
                    p.pop('path', None)
                    p.update(selected=p['address'] == self.state.selected,
                             active=p['address'] == self.active)
                audio_status = self.audio_snapshot()
                media_live = bool(self.active and self.child and self.launcher_exit_code() is None)
                now = time.monotonic()
                now_ms = int(now * 1000)
                media_status = self.media.last
                if not media_live or not media_fresh(media_status, now_ms):
                    media_status = {'available': False, 'controls': []}
                if hid_media and media_live and media_status is not None and not media_status.get('controls'):
                    media_status = dict(media_status, controls=list(HID_MEDIA_CONTROLS))
                observations = self.media_observations.snapshot(
                    self.generation, media_live, self.media.last, audio_status,
                    now_ms, time.clock_gettime_ns(time.CLOCK_MONOTONIC))
                if getattr(self, 'prephone', None):
                    if self.base.get('prephone_indicators', False):
                        prephone_vehicle, ambient_light, prephone_indicators = self.prephone.observations()
                    else:
                        prephone_vehicle, ambient_light = self.prephone.snapshots()
                        prephone_indicators = self.unavailable_indicators()
                    if not self.base.get('ambient_light', False):
                        ambient_light = self.ambient_snapshot()
                else:
                    prephone_vehicle = {
                        'configured': bool(self.base.get('prephone_publication_socket')),
                        'state': 'publication_unavailable' if getattr(self, 'prephone_error', None) else 'disabled'}
                    ambient_light = self.ambient_snapshot()
                    prephone_indicators = self.unavailable_indicators()
                camera, camera_observation = self.camera_snapshot_with_status_observation()
                return {'phones': phones, 'active': self.active, 'active_mode': self.active_mode,
                        'generation': self.generation, 'error': self.error, 'media': media_status,
                        'media_observations': observations,
                        'recovery': {'phone': self.recovery_phone if self.recovery_at is not None else None,
                                     'remainingAttempts': self.recovery_remaining if self.recovery_at is not None else 0,
                                     'schedule': self.recovery_schedule_snapshot(now),
                                     'decisions': [dict(record) for record in self.recovery_decisions]},
                        'carplay': carplay,
                        # Increases when the phone's car icon hands the screen back.
                        'phone_home': {'generation': self.generation,
                                       'connection': (self.carplay.last or {}).get('connectionGeneration'),
                                       'requests': self.carplay.phone_home_requests},
                        'audio': audio_status,
                        'pairing': self.pairing.snapshot() if self.pairing else {'supported': False},
                        'camera': camera,
                        'camera_status_observation': camera_observation,
                        'sport': self.sport.snapshot() if getattr(self, 'sport', None) else {'configured': False},
                        'prephone_vehicle': prephone_vehicle,
                        'prephone_indicators': prephone_indicators,
                        'ambient_light': ambient_light,
                        'vehicle_content': self.content.snapshot() if getattr(self, 'content', None) else {'configured': False, 'choices': [], 'requested': None, 'confirmed': False},
                        'ultra_available': bool(self.base.get('ultra_installed_ui')),
                        'fallback_running': bool(self.fallback and self.fallback.poll() is None),
                        'fallback_error': self.fallback_error,
                        'cluster_pages': cluster_pages(map_available) if page_path else (),
                        'cluster_page': native_page,
                        'ultra_cluster': ultra_state,
                        'cluster_formats': formats,
                        'native_instruments': copy.deepcopy(self.native_instruments),
                        'drive_modes': self.drive_modes.snapshot(styles_available, layout_publisher),
                        'display_response': {'configured': bool(self.display_response_path),
                                             'mode': display_response_read(self.display_response_path, self.owner)},
                        'greeting_sequence': {'configured': bool(self.greeting_path),
                                              'variant': display_response_read(self.greeting_path, self.owner, GREETING_VARIANTS) or 'off',
                                              'variants': list(GREETING_VARIANTS),
                                              'mainIntegrated': False, 'powerIntegrated': False},
                        'startup_warning_lights': {
                            'configured': bool(self.startup_warning_lights_path) and self.startup_warning_hide_default is not None,
                            'hide': {'hide': True, 'show': False}.get(
                                display_response_read(self.startup_warning_lights_path, self.owner, ('hide', 'show')),
                                self.startup_warning_hide_default)}}
            if action == 'cluster_format':
                renderer = data.get('renderer')
                required = {'action', 'renderer', 'format'}
                if renderer == 'ultra':
                    required.add('publisher')
                if set(data) != required:
                    raise ValueError('Cluster formats require the current renderer and catalogue')
                selected = format_choice(formats, renderer, data.get('format'))
                if renderer == 'native':
                    cluster_page_select(page_path, selected['page'], map_available, self.owner)
                    return {'message': 'Native cluster preference saved'}
                if (not styles_available or data['publisher'] !=
                        {'nonce': layout_publisher[0], 'connectionGeneration': layout_publisher[1]}):
                    raise ValueError('Cluster connection changed; refresh before choosing a format')
                layout_select(self.layout_path, selected['layout'], self.owner, layout_publisher)
                return {'message': 'Cluster format requested'}
            if action == 'cluster_input':
                if (not styles_available or set(data) != {'action', 'command', 'publisher'} or
                        data['publisher'] != {'nonce': layout_publisher[0], 'connectionGeneration': layout_publisher[1]}):
                    raise ValueError('Cluster connection changed; refresh before navigating')
                layout_navigate(self.layout_path, data['command'], self.owner, layout_publisher)
                return {'message': 'Cluster button requested'}
            if action == 'phone_app':
                if (not styles_available or set(data) != {'action', 'app', 'publisher'} or
                        data['app'] not in ('maps', 'music', 'phone') or
                        data['publisher'] != {'nonce': layout_publisher[0], 'connectionGeneration': layout_publisher[1]}):
                    raise ValueError('CarPlay connection changed; refresh before opening an app')
                layout_navigate(self.layout_path, 'open_' + data['app'], self.owner, layout_publisher)
                return {'message': 'App requested on the phone'}
            if action == 'cluster_content':
                if (not styles_available or not getattr(self, 'content', None) or
                        set(data) != {'action', 'page', 'publisher'} or
                        data['publisher'] != {'nonce': layout_publisher[0], 'connectionGeneration': layout_publisher[1]}):
                    raise ValueError('Connect CarPlay Ultra and choose from the current content catalogue')
                self.content.request(data['page'], layout_publisher)
                return {'message': 'Vehicle view requested' if data['page'] else 'Vehicle-view request cleared'}
            if action == 'cluster_layout':
                if not styles_available:
                    raise ValueError('Wait for CarPlay Ultra to be ready before choosing a cluster style')
                if (not {'action', 'layout', 'publisher'} <= set(data) or
                        set(data) - {'action', 'layout', 'publisher', 'palette', 'zone', 'component'} or
                        data['publisher'] != {'nonce': layout_publisher[0], 'connectionGeneration': layout_publisher[1]}):
                    raise ValueError('Cluster styles changed; choose from the current catalogue')
                layout_select(self.layout_path, data.get('layout'), self.owner, layout_publisher,
                              palette=data.get('palette'), zone=data.get('zone'), component=data.get('component'))
                return {'message': 'Changing cluster style…'}
            if action == 'cluster_page':
                if not page_path:
                    raise ValueError('Cluster page control is not configured')
                choice = data.get('page')
                if choice not in (*cluster_pages(True), 'next', 'previous'):
                    raise ValueError('Invalid cluster page')
                cluster_page_select(page_path, choice, map_available, self.owner)
                return {'message': 'Cluster page changed'}
            if action == 'disconnect':
                if set(data) not in ({'action'}, {'action', 'phone', 'generation'}):
                    raise ValueError('Disconnect requires the current phone and generation together')
                disconnect_target = self.active or (self.recovery_phone if self.recovery_at is not None else None)
                if 'phone' in data and (address(data['phone']) != disconnect_target or
                        type(data['generation']) is not int or data['generation'] != self.generation):
                    raise ValueError('Session changed; refresh before disconnecting')
                self.require_pairing_idle()
                phone = self.active
                self.stop()
                if phone:
                    self.bluez.disconnect(phone)
                return {'message': 'Disconnected'}
            phone = address(data.get('phone'))
            self.bluez.phone(phone)
            if action == 'connect':
                self.start(phone)
                return {'message': 'Starting CarPlay; accept any iPhone prompts'}
            if action == 'forget':
                self.require_launcher_retired()
                self.require_pairing_idle()
                if self.active == phone:
                    self.stop()
                    self.require_launcher_retired()
                if self.recovery_phone == phone:
                    self.cancel_recovery('cancelled_removed')
                self.bluez.forget(phone)
                # Managed sessions use only this phone's isolated HAP store.
                store = Path(self.base['pair_store']) / ('phone-' + phone.replace(':', ''))
                if store.is_symlink():
                    raise ValueError('Unsafe pairing-store path')
                if store.exists():
                    shutil.rmtree(store)
                self.state.forget(phone)
                return {'message': 'Phone removed'}
            if phone != self.active:
                raise ValueError('This phone is not the selected active source')
            if data.get('generation') != self.generation:
                raise ValueError('Session changed; refresh before sending controls')
            if action == 'audio':
                if set(data) != {'action', 'phone', 'generation', 'route', 'volumePercent', 'muted'} or type(data['generation']) is not int:
                    raise ValueError('Audio controls require the complete current session and volume/mute values')
                current = self.audio_snapshot()
                if not current['available'] or data['route'] != current['route']:
                    raise ValueError('Audio route changed or is unavailable; refresh before controlling it')
                self.audio.request(data['route'], data['volumePercent'], data['muted'])
                return {'message': 'Head-unit volume requested; waiting for mixer confirmation'}
            if action == 'media':
                self.require_launcher_retired()
                # Recheck after BlueZ phone lookup: that synchronous call can
                # outlive the child or the metadata's admission interval.
                if not self.child or self.launcher_exit_code() is not None:
                    raise ValueError('Media receiver is unavailable; reconnect before sending controls')
                if (set(data) != {'action', 'phone', 'generation', 'command'} or
                        type(data['generation']) is not int):
                    raise ValueError('Media controls require the complete current session and command')
                command = data.get('command')
                if type(command) is not str or command not in COMMANDS:
                    raise ValueError('Media control unavailable')
                media_current = media_fresh(self.media.last, int(time.monotonic() * 1000))
                if (command in HID_MEDIA_CONTROLS and hid_media and
                        not ((self.media.last or {}).get('controls') if media_current else [])):
                    transport = 'ultra-hid'
                else:
                    if not media_current or command not in (self.media.last or {}).get('controls', []):
                        raise ValueError('Media control unavailable')
                    if (self.media.last or {}).get('source') not in ('bluetooth', 'iap'):
                        raise ValueError('This media adapter has no command transport yet')
                    transport = 'bluetooth'
                token = self.media_observations.begin(
                    self.generation, command, transport, time.clock_gettime_ns(time.CLOCK_MONOTONIC))
                try:
                    if transport == 'ultra-hid':
                        layout_navigate(self.layout_path, HID_MEDIA_CONTROLS[command], self.owner, layout_publisher)
                    else:
                        self.bluez.media_command(phone, command)
                except Exception:
                    self.media_observations.finish(token, 'error', time.clock_gettime_ns(time.CLOCK_MONOTONIC))
                    raise
                self.media_observations.finish(token, 'queued' if transport == 'ultra-hid' else 'dispatched',
                                               time.clock_gettime_ns(time.CLOCK_MONOTONIC))
                return {'message': 'Media key requested' if transport == 'ultra-hid' else 'Command sent to phone',
                        'confirmed': False, 'completion': 'unknown', 'transport': transport}
            if action == 'media_update':
                self.require_launcher_retired()
                value = dict(data.get('media', {}))
                # Future iAP adapter may publish metadata/art; it must not claim
                # actionable controls until its command transport is attached.
                value['controls'] = []
                value['source'] = 'adapter'
                value.pop('iap_observation', None)
                self.media.publish(value, data.get('artwork'))
                self.external_until = time.monotonic() + 3
                return {'message': 'Media snapshot accepted'}
            raise ValueError('Unknown manager action')


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--standard-display', type=Path)
    parser.add_argument('--manager-profile', type=Path,
                        help='file-driven optional supervised manager settings')
    parser.add_argument('--validate-only', action='store_true',
                        help='read manager settings/displays without creating state or contacting BlueZ')
    parser.add_argument('--vehicle-fanout-config', type=Path,
                        help='same consumer configuration as the separately supervised vehicle fan-out service')
    parser.add_argument('--release-root', type=Path)
    parser.add_argument('--release-manifest-sha256')
    parser.add_argument('--uid', type=int)
    args = parser.parse_args()
    release_binding = None
    sealed_profile = None
    try:
        if bool(args.release_root) != bool(args.release_manifest_sha256):
            raise ValueError('A release root and manifest digest must be supplied together')
        if args.release_root:
            allowed = {'--config', '--manager-profile', '--release-root', '--release-manifest-sha256',
                       '--uid', '--vehicle-fanout-config', '--validate-only'}
            seen = set()
            for item in sys.argv[1:]:
                if item.startswith('--'):
                    flag = item.split('=', 1)[0]
                    if flag not in allowed or flag in seen:
                        raise ValueError('Sealed manager settings cannot be overridden or repeated')
                    seen.add(flag)
            if not args.manager_profile:
                raise ValueError('A sealed manager requires the exact phone_manager profile role')
            release_binding = {'path': str(args.release_root), 'sha256': args.release_manifest_sha256}
            manifest, _, sealed_profile = sealed_manager_configuration(
                args.config, args.manager_profile, release_binding, args.uid)
            args.uid = manifest['uid']
            if args.vehicle_fanout_config:
                from appliance_release import read_configuration_member
                read_configuration_member(args.vehicle_fanout_config, release_binding, 'fanout')
        elif args.uid is None:
            args.uid = int(os.environ.get('SUDO_UID', os.getuid()))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    if not 0 < args.uid < (1 << 32) - 1:
        parser.error('Choose an unprivileged display account UID')
    try:
        account = pwd.getpwuid(args.uid)
    except KeyError:
        parser.error('Choose an existing unprivileged display account UID')
    profile = None
    if args.manager_profile:
        if args.directory or args.standard_display:
            parser.error('Choose --manager-profile or explicit directory/display options')
        try:
            profile = sealed_profile or inspect_configuration(args.config, args.manager_profile, args.uid)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            parser.error(str(error))
        args.directory = Path(profile['stateDirectory'])
        args.standard_display = Path(profile['standardDisplayConfig'])
    elif not args.directory or not args.standard_display or args.validate_only:
        parser.error('Supply a manager profile, or directory and Standard display for the legacy manual launch')
    if args.validate_only:
        print(json.dumps({'configurationValid': True, 'enabled': profile['enabled'],
                          'autoConnectSelected': profile['autoConnectSelected']}))
        return
    if profile and not profile['enabled']:
        parser.error('The supervised native manager profile is disabled')
    if os.geteuid() != 0:
        parser.error('Run the manager through sudo; its UI runs as the desktop user')
    owner = args.uid, account.pw_gid
    folder = prepare_directory(Path(os.path.abspath(args.directory)), owner)
    stopping = threading.Event()
    def stop(*_):
        stopping.set()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGHUP, stop)
    # Claim the generation before constructor cleanup can touch another
    # owner's media/audio state. Keep it through receiver and camera shutdown.
    with ManagerEndpoint(folder, owner) as endpoint:
        pairing = None
        if profile and profile.get('pairing', {}).get('enabled'):
            from phone_pairing import PairingSession
            pairing = PairingSession(profile['pairing']['adapter'])
        manager = Manager(args.config, folder, args.standard_display, owner,
                          fanout_config=args.vehicle_fanout_config,
                          receiver_lock_fd=endpoint.inherited_lock_fd, pairing=pairing,
                          recovery_attempts=profile.get('receiverRecoveryAttempts', 0) if profile else 0,
                          **({'release_binding': release_binding, 'manager_profile': args.manager_profile}
                             if release_binding else {}))
        def request(command):
            # A signal can arrive while the endpoint waits for a complete
            # request. Do not admit new operations after shutdown is observed.
            if stopping.is_set():
                raise RuntimeError('Manager is stopping')
            return manager.request(command)
        try:
            if profile and profile['autoConnectSelected']:
                manager.restore_selected()
            next_tick = 0
            while not stopping.is_set():
                endpoint.serve_once(request)
                if stopping.is_set():
                    break
                if time.monotonic() >= next_tick:
                    manager.tick()
                    next_tick = time.monotonic() + 1
        finally:
            manager.shutdown()


if __name__ == '__main__':
    main()
