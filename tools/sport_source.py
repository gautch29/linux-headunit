# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Manager-owned bounded sport acquisition and private session history.

The socket accepts complete HVO1–4/HVO6 snapshots, validating shape/metadata
and the three sport values; irrelevant compound values remain opaque. The C++
receiver is the authoritative decoder for the complete vehicle model.
"""
from collections import deque
import json
import math
import os
import re
from pathlib import Path
import socket
import stat
import sys
import threading
import time
import uuid

from sport_telemetry import Observation, SportEngine, NS

VEHICLE_CLOCK = time.CLOCK_MONOTONIC_RAW if sys.platform == 'darwin' else time.CLOCK_MONOTONIC


def host_now_ns():
    # libc++ steady_clock on macOS uses CLOCK_MONOTONIC_RAW; Linux uses
    # CLOCK_MONOTONIC. Python's default macOS clock uses CLOCK_UPTIME_RAW.
    # Read the actual protocol domain; never infer offsets or redate captures.
    return time.clock_gettime_ns(VEHICLE_CLOCK)


READINGS = ('speed_kmh', 'engine_rpm', 'fuel_percent', 'coolant_c', 'oil_c',
            'oil_pressure_kpa', 'battery_voltage', 'ambient_c', 'range_km', 'odometer_km')
LAMPS = ('left_turn', 'right_turn', 'hazard', 'high_beam', 'low_beam', 'position_lights',
         'front_fog', 'rear_fog', 'check_engine', 'oil_pressure', 'battery_charge',
         'brake_fault', 'parking_brake', 'abs', 'stability_control', 'traction_control',
         'airbag', 'seat_belt', 'door_open', 'low_fuel', 'coolant_warning', 'washer_low',
         'tyre_pressure', 'glow_plug')
ORIGINS = {'m': 'measured', 'c': 'calculated', 's': 'simulation'}
STATES = {'a': 'available', 'u': 'unavailable', 's': 'stale', 'd': 'disconnected'}


def scalar(text):
    # Match the C++ decimal from_chars grammar, excluding Python-only +prefix
    # and underscore spellings. Value plausibility is checked by the engine.
    if not re.fullmatch(r'-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?', text):
        raise ValueError('Invalid HVO scalar spelling')
    result = float(text)
    coefficient = re.split('[eE]', text, maxsplit=1)[0]
    if result == 0 and any(c in '123456789' for c in coefficient):
        raise ValueError('Unrepresentable HVO scalar underflow')
    return result


def integer(text, maximum=9223372036854775807):
    if not text.isascii() or not text.isdecimal() or len(text) > 20:
        raise ValueError('Invalid HVO integer')
    value = int(text)
    if not 0 < value <= maximum:
        raise ValueError('HVO integer outside host domain')
    return value


def decode(data, now):
    if not 0 < len(data) <= 8192:
        raise ValueError('Invalid sport datagram bound')
    text = data.decode('ascii')
    if any(ord(c) < 32 or ord(c) > 126 for c in text):
        raise ValueError('Invalid sport datagram characters')
    tokens = text.split(' ')
    version = tokens[0]
    extra = {'HVO1': (), 'HVO2': ('transmission',),
             'HVO3': ('transmission', 'drive_mode'),
             'HVO4': ('transmission', 'drive_mode', 'trip', 'heading'),
             'HVO6': ('transmission', 'drive_mode', 'trip', 'heading', 'acceleration', 'gps_position')}.get(version)
    if extra is None:
        raise ValueError('Unsupported sport wire version')
    keys = set((*READINGS, *LAMPS, *extra))
    header = 6 if version in ('HVO3', 'HVO4', 'HVO6') else 5
    if len(tokens) != header + len(keys):
        raise ValueError('Incomplete sport snapshot')
    generation = integer(tokens[1], 18446744073709551615)
    sequence = integer(tokens[2], 18446744073709551615)
    started, published = map(integer, tokens[3:5])
    if not started <= published <= now:
        raise ValueError('Future or reversed sport publication')
    if header == 6 and not tokens[5].startswith('drive_modes='):
        raise ValueError('Missing vehicle catalogue header')
    observations = {}
    seen = set()
    for token in tokens[header:]:
        key, equal, record = token.partition('=')
        if not equal or key not in keys or key in seen:
            raise ValueError('Duplicate or unknown sport snapshot key')
        seen.add(key)
        fields = record.split(',')
        if len(fields) != 5 or not fields[0] or fields[3] not in ORIGINS or fields[4] not in STATES:
            raise ValueError('Invalid sport snapshot record')
        value, acquired, age, origin, availability = fields
        acquired = None if acquired == '-' else integer(acquired)
        age = integer(age)
        origin, availability = ORIGINS[origin], STATES[availability]
        if (age > 86400 * NS or acquired is not None and not started <= acquired <= published or
                (value != '-') != (availability == 'available') or
                availability in ('available', 'stale') and acquired is None or
                availability == 'available' and published - acquired >= age or
                availability == 'stale' and published - acquired < age):
            raise ValueError('Invalid sport snapshot timing')
        if key in ('speed_kmh', 'acceleration', 'gps_position'):
            parsed = None
            if value != '-':
                if key == 'speed_kmh':
                    parsed = scalar(value)
                else:
                    parsed = tuple(scalar(part) for part in value.split('/'))
            observations[key] = Observation(parsed, acquired, age, origin, availability)
    return version, generation, sequence, started, published, observations


def simulated_observations(elapsed, acquired):
    # Dedicated sport fixture, not a car measurement or the ordinary gauge demo.
    phase = elapsed % 40
    speed = 0.0 if phase < 2 else min(220.0, (phase - 2) * 20) if phase < 18 else 0.0
    acceleration = 20 / 3.6 if 2 < phase < 13 else 0.0
    angle = elapsed * 2 * math.pi / 20
    return {'speed_kmh': Observation(speed, acquired, origin='simulation'),
            'acceleration': Observation((acceleration, 3 * math.sin(angle)), acquired, origin='simulation'),
            'gps_position': Observation((48 + .001 * math.sin(angle),
                                        2 + .001 * math.cos(angle), 1.0), acquired, origin='simulation')}


def identity(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_nlink,
            info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def save_sport_record(folder, history_path, owner, record):
    """Existing bounded atomic writer, callable by its supervised disk owner."""
    raw = json.dumps(record, allow_nan=False, separators=(',', ':')).encode()
    if len(raw) > 65536:
        raise ValueError('Sport history bound exceeded')
    directory = os.open(folder, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    temporary = '.sport-history-' + uuid.uuid4().hex
    try:
        info = os.fstat(directory)
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != owner[0] or
                stat.S_IMODE(info.st_mode) != 0o700 or
                (info.st_dev, info.st_ino) != (folder.lstat().st_dev, folder.lstat().st_ino)):
            raise ValueError('Sport history directory changed')
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=directory)
        with os.fdopen(fd, 'wb') as output:
            os.fchown(output.fileno(), *owner)
            os.fchmod(output.fileno(), 0o600)
            output.write(raw)
            output.flush()
            os.fsync(output.fileno())
        named = folder.lstat()
        if (info.st_dev, info.st_ino) != (named.st_dev, named.st_ino):
            raise ValueError('Sport history directory replaced')
        os.replace(temporary, history_path.name, src_dir_fd=directory, dst_dir_fd=directory)
        os.fsync(directory)
    finally:
        try:
            os.unlink(temporary, dir_fd=directory)
        except FileNotFoundError:
            pass
        os.close(directory)


class SportSource:
    def __init__(self, folder, simulated=False, socket_path=None, owner=None, trip_history=False):
        if simulated and socket_path:
            raise ValueError('Choose a simulated sport source or a dedicated fan-out consumer')
        self.owner = tuple(owner) if owner is not None else (os.getuid(), os.getgid())
        if (len(self.owner) != 2 or any(type(v) is not int or v < 0 for v in self.owner) or
                os.geteuid() not in (0, self.owner[0])):
            raise ValueError('Invalid sport owner account')
        self.folder = Path(folder).resolve()
        info = self.folder.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != self.owner[0] or stat.S_IMODE(info.st_mode) != 0o700:
            raise ValueError('Sport history requires an owned private directory')
        self.history_path = self.folder / 'sport-history.json'
        self.simulated = simulated
        self.path = Path(socket_path) if socket_path else None
        self.socket = None
        self.socket_identity = None
        self.engine = SportEngine()
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.thread = None
        self.error = ''
        self.rejected = 0
        self.packet_mark = None
        self.saved_revision = -1
        self.closed = False
        self.storage = None
        self.saved_trip_journey = None
        if type(trip_history) is not bool:
            raise ValueError('Trip history opt-in must be boolean')
        if trip_history:
            from trip_history import HistoryClient
            # Preserve the caller's path spelling for child canonical admission.
            self.storage = HistoryClient(folder, self.owner)
        self._load_history()

    def _load_history(self):
        try:
            fd = os.open(self.history_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError:
            return
        except OSError:
            self.error = 'Sport history unavailable'
            return
        try:
            before = os.fstat(fd)
            if (not stat.S_ISREG(before.st_mode) or before.st_uid != self.owner[0] or
                    before.st_mode & 0o077 or before.st_nlink != 1 or not 0 < before.st_size <= 65536):
                raise ValueError('Invalid private history file')
            raw = os.read(fd, 65537)
            if len(raw) != before.st_size or identity(before) != identity(os.fstat(fd)) or identity(before) != identity(self.history_path.lstat()):
                raise ValueError('History changed during read')
            def unique(pairs):
                result = {}
                for key, value in pairs:
                    if key in result:
                        raise ValueError('Duplicate history field')
                    result[key] = value
                return result
            value = json.loads(raw, object_pairs_hook=unique,
                               parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite history')))
            if (not isinstance(value, dict) or set(value) != {'version', 'sessionId', 'history', 'line'} or
                    type(value['version']) is not int or value['version'] != 1 or
                    type(value['sessionId']) is not int or not 0 < value['sessionId'] < 2**53 or
                    not isinstance(value['history'], list) or len(value['history']) > self.engine.HISTORY_LIMIT):
                raise ValueError('Invalid history schema')
            allowed = {'sessionId', 'sourceGeneration', 'kind', 'timer', 'reason', 'atNs',
                       'startNs', 'endNs', 'seconds', 'origin', 'method', 'maximumGapNs', 'peaks', 'peakObservations', 'origins', 'line'}
            for event in value['history']:
                if (not isinstance(event, dict) or set(event) - allowed or
                        event.get('kind') not in ('acceleration', 'lap', 'aborted', 'session-ended') or
                        type(event.get('sessionId')) is not int or not 0 < event['sessionId'] <= value['sessionId'] or
                        len(json.dumps(event, allow_nan=False)) > 1000):
                    raise ValueError('Invalid session event')
                for key in ('sourceGeneration', 'atNs', 'startNs', 'endNs', 'maximumGapNs'):
                    field = event.get(key)
                    maximum = 2**64 - 1 if key == 'sourceGeneration' else 2**63 - 1
                    if field is not None and (type(field) is not int or not 0 < field <= maximum):
                        raise ValueError('Invalid historical date')
                if event.get('line') is not None:
                    if not isinstance(event['line'], list) or len(event['line']) != 2:
                        raise ValueError('Invalid historical lap line')
                    SportEngine().set_line(*event['line'])
                if event['kind'] in ('acceleration', 'lap'):
                    if (event.get('origin') not in ORIGINS.values() or
                            type(event.get('seconds')) not in (float, int) or
                            not math.isfinite(event['seconds']) or event['seconds'] <= 0 or
                            not 0 < event.get('startNs', 0) < event.get('endNs', 0) or
                            abs(event['seconds'] - (event['endNs'] - event['startNs']) / NS) > 1e-9):
                        raise ValueError('Invalid historical result')
            if value['line'] is not None:
                self.engine.set_line(*value['line'])
            self.engine.history = deque(value['history'], maxlen=self.engine.HISTORY_LIMIT)
            self.engine.session_id = value['sessionId'] + 1
        except (OSError, ValueError, TypeError, KeyError, RecursionError):
            self.error = 'Sport history rejected'
        finally:
            os.close(fd)

    def start(self):
        if self.thread is not None or self.closed:
            raise ValueError('Sport owner already started')
        if self.path:
            from vehicle_fanout import private_parent, socket_path
            self.path = socket_path(str(self.path))
            private_parent(self.path, self.owner[0])
            self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                self.socket.setblocking(False)
                self.socket.bind(str(self.path))  # Existing owner/dead endpoint is an explicit error.
                info = self.path.lstat()
                self.socket_identity = info.st_dev, info.st_ino
                os.chown(self.path, *self.owner, follow_symlinks=False)
                os.chmod(self.path, 0o600, follow_symlinks=False)
                current = self.path.lstat()
                if not stat.S_ISSOCK(current.st_mode) or (current.st_dev, current.st_ino) != self.socket_identity:
                    raise ValueError('Sport endpoint changed during binding')
            except BaseException:
                self.socket.close()
                self.socket = None
                try:
                    info = self.path.lstat()
                    if (info.st_dev, info.st_ino) == self.socket_identity:
                        self.path.unlink()
                except FileNotFoundError:
                    pass
                raise
        if self.storage:
            try:
                self.storage.start()
            except (OSError, ValueError):
                self.storage.fail('Historical storage could not start')
        self.started_ns = host_now_ns()
        self.thread = threading.Thread(target=self._run, name='sport-observations', daemon=True)
        try:
            self.thread.start()
        except BaseException:
            self.thread = None
            self.close()
            raise

    def ingest(self, data, now):
        if self.closed:
            raise ValueError('Sport owner retired')
        decoded = decode(data, now)
        version, generation, sequence, started, published, observations = decoded
        old = self.packet_mark
        if old and (version != old[0] or generation < old[1] or
                generation == old[1] and (sequence <= old[2] or started != old[3] or published < old[4]) or
                generation > old[1] and started < old[4]):
            raise ValueError('Sport packet replay or generation conflict')
        self.engine.accept(generation, observations, now)
        self.packet_mark = decoded[:5]

    def _save(self, record):
        save_sport_record(self.folder, self.history_path, self.owner, record)

    def _withdraw_inputs(self):
        # Consumer retirement is known immediately; retain original dates/origin.
        self.engine.rolling100_to_200.retire()
        self.engine.inputs = {
            key: Observation(None, obs.acquired_ns, obs.maximum_age_ns,
                             obs.origin, 'disconnected')
            for key, obs in self.engine.inputs.items()}
        self.engine.advance(max(host_now_ns(), self.engine.last_now))

    def _run(self):
        try:
            while not self.stop_event.is_set():
                record = None
                with self.lock:
                    # Sample operation time after acquiring the engine owner lock.
                    # A status read may otherwise advance the clock while we wait.
                    now = host_now_ns()
                    if self.simulated:
                        self.engine.accept(1, simulated_observations((now - self.started_ns) / NS, now), now)
                    elif self.socket:
                        for _ in range(32):
                            try:
                                data, _, flags, _ = self.socket.recvmsg(8192)
                            except BlockingIOError:
                                break
                            try:
                                if flags & socket.MSG_TRUNC:
                                    raise ValueError('Truncated sport datagram')
                                self.ingest(data, host_now_ns())
                            except (ValueError, UnicodeError, OverflowError):
                                self.rejected += 1
                    self.engine.advance(max(now, self.engine.last_now))
                    self._storage_tick()
                    if not self.storage and self.saved_revision != self.engine.revision:
                        revision = self.engine.revision
                        record = {'version': 1, 'sessionId': self.engine.session_id,
                                  'history': list(self.engine.history), 'line': self.engine.line}
                if record is not None:
                    try:
                        self._save(record)
                        self.saved_revision = revision
                        self.error = ''
                    except (OSError, ValueError):
                        self.error = 'Sport history write failed'
                self.stop_event.wait(.02)
        except BaseException:
            with self.lock:
                self.error = 'Sport acquisition stopped'
                self._withdraw_inputs()
                self.engine.advance(max(host_now_ns(), self.engine.last_now))
            if self.storage:
                self.storage.fail('Historical storage acquisition owner stopped')
                self.storage.close()

    def _storage_tick(self):
        if not self.storage:
            return
        try:
            self.storage.tick()
            if self.storage.state == 'ready' and (self.saved_trip_journey != self.engine.trip.journey_id or
                                                  self.saved_revision != self.engine.revision):
                sport = {'version': 1, 'sessionId': self.engine.session_id,
                         'history': list(self.engine.history), 'line': self.engine.line}
                self.storage.offer(self.storage.histories(list(self.engine.trip.history)), sport)
                self.saved_revision = self.engine.revision
                self.saved_trip_journey = self.engine.trip.journey_id
                self.storage.tick()
            if self.storage.state == 'failed':
                self.error = 'Private history storage unavailable'
            elif self.storage.saved_revision > 0 and self.error in ('Sport history rejected', 'Sport history unavailable',
                                                                   'Sport history write failed'):
                self.error = ''
        except (OSError, ValueError, TypeError):
            self.storage.fail('Historical storage rejected current archive')

    def request(self, data):
        with self.lock:
            if self.closed:
                raise ValueError('Sport owner retired')
            now = host_now_ns()
            if data == {'action': 'sport_session'}:
                self.engine.reset_session(now)
            elif data == {'action': 'trip_reset'}:
                self.engine.reset_trip(now)
            elif set(data) == {'action', 'line'} and data['action'] == 'sport_line':
                line = data['line']
                if not isinstance(line, list) or len(line) != 2:
                    raise ValueError('Sport line needs two GPS endpoints')
                self.engine.set_line(*line)
            else:
                raise ValueError('Invalid sport request')
            return self._model(now)

    def _model(self, now):
        model = self.engine.model(now)
        model.update(source='simulation' if self.simulated else 'observations' if self.path else 'unavailable',
                     error=self.error, rejectedPackets=self.rejected)
        if self.storage:
            model['trip']['persistence'] = 'private-archives'
            model['trip']['storage'] = self.storage.status()
            if self.storage.state in ('starting', 'ready') and self.saved_trip_journey != self.engine.trip.journey_id:
                model['trip']['storage']['pending'] = True
            model['trip']['history'] = self.storage.histories(list(self.engine.trip.history))
        return model

    def snapshot(self):
        with self.lock:
            return self._model(host_now_ns())

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        if self.socket:
            self.socket.close()
            try:
                info = self.path.lstat()
                if (info.st_dev, info.st_ino) == self.socket_identity:
                    self.path.unlink()
            except FileNotFoundError:
                pass
        with self.lock:
            now = host_now_ns()
            self.engine.reset_session(now)
            if self.storage:
                self.engine.trip.retire(now)
                self._storage_tick()
            self._withdraw_inputs()
            record = {'version': 1, 'sessionId': self.engine.session_id,
                      'history': list(self.engine.history), 'line': self.engine.line}
        # Retire the opt-in disk owner before any legacy synchronous path.
        if self.storage:
            self.storage.close()
        # History is retained on retirement; no data can revive active timers.
        elif not self.thread or not self.thread.is_alive():
            try:
                self._save(record)
            except (OSError, ValueError):
                self.error = 'Sport history write failed'
