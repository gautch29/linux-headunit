# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Private archived software journeys; one supervised disk owner, no acquisition.

SportSource's worker and ordered retirement drive the nonblocking client. The child performs all
new filesystem work. Restores are history data, never engine state or clocks.
"""
import copy
import ctypes
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import stat
import struct
import subprocess
import sys
import time
import uuid

from sport_telemetry import TripStatistics, NS, ORIGINS

SOFT_STOP_SIGNALS = {signal.SIGINT, signal.SIGTERM, signal.SIGHUP}
MAX_BYTES = 65536
MAX_IPC_BYTES = 2 * MAX_BYTES
MAX_DATE = 2**63 - 1
CLOCK = 'CLOCK_MONOTONIC_RAW' if sys.platform == 'darwin' else 'CLOCK_MONOTONIC'
UNITS = {'distance': 'km', 'duration': 's', 'speed': 'km/h', 'dates': 'ns'}
REASONS = {'User software reset', 'Source generation changed', 'Speed origin changed', 'Sport owner retired'}
STAT_KEYS = {'journeyId', 'sourceGeneration', 'origin', 'speedMaximumAgeNs', 'firstAcquiredNs', 'lastAcquiredNs',
             'distanceKm', 'coveredSeconds', 'movingSeconds', 'movingDistanceKm',
             'averageSpeedKmh', 'movingAverageSpeedKmh', 'peak', 'acquiredSpanSeconds',
             'uncoveredSeconds', 'coverageRatio', 'partial', 'limitReached',
             'endedReason', 'archivedAtNs'}
SCOPE_KEYS = {'classification', 'ownerId', 'hostBootId', 'sourceClock', 'units',
              'authority', 'method', 'maximumGapNs', 'movingThresholdKmh'}


def encode(value, maximum=MAX_BYTES):
    raw = json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('ascii')
    if len(raw) > maximum:
        raise ValueError('Trip history byte bound')
    return raw


def decode(raw, maximum=MAX_BYTES):
    if not 0 < len(raw) <= maximum:
        raise ValueError('Trip history byte bound')
    # Bound structure before the JSON parser, respecting quoted/escaped text.
    depth = 0
    quoted = escaped = False
    for char in raw:
        if quoted:
            if escaped:
                escaped = False
            elif char == 92:
                escaped = True
            elif char == 34:
                quoted = False
        elif char == 34:
            quoted = True
        elif char in (91, 123):
            depth += 1
            if depth > 8:
                raise ValueError('Trip history depth bound')
        elif char in (93, 125):
            depth -= 1
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate trip history field')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite history')))


def positive(value, maximum=MAX_DATE):
    return type(value) is int and 0 < value <= maximum


def number(value, maximum):
    return type(value) in (int, float) and math.isfinite(value) and 0 <= value <= maximum


def near(a, b):
    return math.isclose(a, b, rel_tol=1e-9, abs_tol=1e-9)


def validate_history(history):
    if not isinstance(history, list) or len(history) > TripStatistics.HISTORY_LIMIT:
        raise ValueError('Trip history record bound')
    seen = set()
    for item in history:
        if not isinstance(item, dict) or set(item) != STAT_KEYS | SCOPE_KEYS or len(encode(item)) > 3000:
            raise ValueError('Trip archive schema')
        if (item['classification'] != 'HISTORICAL' or item['authority'] != 'software-speed-estimate' or
                item['method'] != 'linear-speed-trapezoids' or item['units'] != UNITS or
                item['sourceClock'] not in ('CLOCK_MONOTONIC', 'CLOCK_MONOTONIC_RAW') or
                not isinstance(item['ownerId'], str) or not re.fullmatch('[0-9a-f]{32}', item['ownerId']) or
                not isinstance(item['hostBootId'], str) or
                not re.fullmatch('[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', item['hostBootId']) or
                type(item['maximumGapNs']) is not int or item['maximumGapNs'] != TripStatistics.MAXIMUM_GAP_NS or
                type(item['movingThresholdKmh']) not in (int, float) or
                item['movingThresholdKmh'] != TripStatistics.MOVING_THRESHOLD_KMH or
                type(item['partial']) is not bool or type(item['limitReached']) is not bool or
                item['limitReached'] and not item['partial'] or
                item['endedReason'] not in REASONS or item['origin'] not in ORIGINS or
                not positive(item['journeyId'], 2**53 - 1) or
                not positive(item['sourceGeneration'], 2**64 - 1) or
                not positive(item['speedMaximumAgeNs'], 86400 * NS)):
            raise ValueError('Trip archive authority or scope')
        key = item['ownerId'], item['journeyId']
        if key in seen:
            raise ValueError('Duplicate archived journey')
        seen.add(key)
        first, last, archived = [item[key] for key in ('firstAcquiredNs', 'lastAcquiredNs', 'archivedAtNs')]
        if not all(positive(v) for v in (first, last, archived)) or not first <= last <= archived:
            raise ValueError('Trip archive original dates')
        peak = item['peak']
        if (not isinstance(peak, dict) or set(peak) != {'speedKmh', 'acquiredNs', 'origin'} or
                not number(peak['speedKmh'], 300) or peak['origin'] != item['origin'] or
                not positive(peak['acquiredNs']) or not first <= peak['acquiredNs'] <= last):
            raise ValueError('Trip archive peak')
        span = (last - first) / NS
        if span > TripStatistics.MAXIMUM_DURATION_NS / NS and not item['limitReached']:
            raise ValueError('Trip archive duration limit state')
        if (not number(item['acquiredSpanSeconds'], MAX_DATE / NS) or
                not near(item['acquiredSpanSeconds'], span) or
                not number(item['uncoveredSeconds'], span)):
            raise ValueError('Trip archive span')
        covered = item['coveredSeconds']
        statistics = ('distanceKm', 'movingDistanceKm', 'movingSeconds', 'averageSpeedKmh', 'movingAverageSpeedKmh')
        if covered is None:
            if (any(item[key] is not None for key in statistics) or
                    (not number(item['coverageRatio'], 0) if span else item['coverageRatio'] is not None)):
                raise ValueError('Trip archive unknown statistics')
            if not near(item['uncoveredSeconds'], span):
                raise ValueError('Trip archive unknown coverage')
            continue
        if (not number(covered, TripStatistics.MAXIMUM_DURATION_NS / NS) or covered <= 0 or covered > span or
                not number(item['movingSeconds'], covered) or
                not number(item['distanceKm'], TripStatistics.MAXIMUM_DISTANCE_KM) or
                not number(item['movingDistanceKm'], item['distanceKm']) or
                not number(item['averageSpeedKmh'], 300 + 1e-9) or
                not near(item['averageSpeedKmh'], item['distanceKm'] * 3600 / covered) or
                item['averageSpeedKmh'] > peak['speedKmh'] + 1e-9 or
                not near(item['uncoveredSeconds'], span - covered) or
                not number(item['coverageRatio'], 1) or not near(item['coverageRatio'], covered / span) or
                span - covered > 1e-9 and not item['partial']):
            raise ValueError('Trip archive coverage or mean')
        moving = item['movingSeconds']
        mean = item['movingAverageSpeedKmh']
        if moving == 0:
            if mean is not None or item['movingDistanceKm'] != 0:
                raise ValueError('Trip archive unknown moving mean')
        elif (not number(mean, 300 + 1e-9) or mean < TripStatistics.MOVING_THRESHOLD_KMH or
              mean > peak['speedKmh'] + 1e-9 or not near(mean, item['movingDistanceKm'] * 3600 / moving)):
            raise ValueError('Trip archive moving mean')
    return history


def envelope(history):
    validate_history(history)
    body = {'version': 1, 'history': history}
    return encode({**body, 'sha256': hashlib.sha256(encode(body)).hexdigest()})


def restore(raw):
    value = decode(raw)
    if not isinstance(value, dict) or set(value) != {'version', 'history', 'sha256'} or type(value['version']) is not int or value['version'] != 1:
        raise ValueError('Trip history version')
    body = {'version': 1, 'history': value['history']}
    if value['sha256'] != hashlib.sha256(encode(body)).hexdigest():
        raise ValueError('Trip history hash')
    return validate_history(value['history'])


def boot_id():
    if sys.platform == 'darwin':
        buffer = ctypes.create_string_buffer(64)
        size = ctypes.c_size_t(len(buffer))
        if ctypes.CDLL(None, use_errno=True).sysctlbyname(b'kern.bootsessionuuid', buffer, ctypes.byref(size), None, 0) != 0:
            raise OSError('Host boot scope unavailable')
        if not 0 < size.value <= len(buffer):
            raise ValueError('Host boot scope bound')
        result = buffer.value.decode('ascii').strip().lower()
    else:
        with open('/proc/sys/kernel/random/boot_id', 'r', encoding='ascii') as source:
            result = source.read(37).strip()
    if not re.fullmatch('[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', result):
        raise ValueError('Host boot scope unavailable')
    return result


def metadata(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


class PrivateStore:
    """Used only in the storage child; pins directory and exclusive lock."""
    def __init__(self, folder, owner):
        self.folder = Path(folder)
        self.owner = owner
        self.directory = self.lock = None
        self.target_known = False
        self.target_mark = None
        try:
            if (not self.folder.is_absolute() or str(self.folder) != str(folder) or
                    len(str(self.folder).encode()) > 4000 or
                    any(ord(char) < 32 or ord(char) == 127 for char in str(self.folder)) or
                    str(self.folder.resolve(strict=True)) != str(self.folder)):
                raise ValueError('Trip history canonical directory required')
            self.directory = os.open(self.folder, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
            self.directory_identity = metadata(os.fstat(self.directory))[:2]
            self.check_directory()
            self.lock = os.open('.trip-history.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
                                0o600, dir_fd=self.directory)
            self.check_file(os.fstat(self.lock), empty=True)
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            # Only the lock winner may adjust a newly root-created display file.
        except BaseException:
            self.close()
            raise

    def check_directory(self):
        info = os.fstat(self.directory)
        named = self.folder.lstat()
        if str(self.folder.resolve(strict=True)) != str(self.folder):
            raise ValueError('Trip history directory became noncanonical')
        if (not stat.S_ISDIR(named.st_mode) or (named.st_dev, named.st_ino) != self.directory_identity or
                info.st_uid != self.owner[0] or info.st_gid != self.owner[1] or stat.S_IMODE(info.st_mode) != 0o700):
            raise ValueError('Trip history directory replaced or shared')
        if self.lock is not None:
            self.check_file(os.fstat(self.lock), empty=True)
            if metadata(os.fstat(self.lock)) != metadata(os.stat('.trip-history.lock', dir_fd=self.directory, follow_symlinks=False)):
                raise ValueError('Trip history lock replaced')

    def check_file(self, info, empty=False):
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != self.owner[0] or info.st_gid != self.owner[1] or
                stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or
                not (0 if empty else 1) <= info.st_size <= MAX_BYTES):
            raise ValueError('Trip history private file rejected')

    def load(self):
        self.check_directory()
        try:
            fd = os.open('trip-history.json', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self.directory)
        except FileNotFoundError:
            self.target_known = True
            self.target_mark = None
            return []
        try:
            before = os.fstat(fd)
            self.check_file(before)
            raw = os.read(fd, MAX_BYTES + 1)
            if (len(raw) != before.st_size or metadata(before) != metadata(os.fstat(fd)) or
                    metadata(before) != metadata(os.stat('trip-history.json', dir_fd=self.directory, follow_symlinks=False))):
                raise ValueError('Trip history changed during read')
            self.check_directory()
            history = restore(raw)
            self.target_mark = metadata(before)
            self.target_known = True
            return history
        finally:
            os.close(fd)

    def check_target(self):
        if not self.target_known:
            self.load()
        try:
            current = os.stat('trip-history.json', dir_fd=self.directory, follow_symlinks=False)
            self.check_file(current)
            mark = metadata(current)
        except FileNotFoundError:
            mark = None
        if mark != self.target_mark:
            raise ValueError('Trip history target replaced between writes')

    def save(self, history):
        raw = envelope(history)
        self.check_directory()
        self.check_target()
        try:
            before = os.stat('trip-history.json', dir_fd=self.directory, follow_symlinks=False)
            self.check_file(before)
        except FileNotFoundError:
            before = None
        temporary = '.trip-history-' + uuid.uuid4().hex
        try:
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.directory)
            with os.fdopen(fd, 'wb') as output:
                os.fchown(output.fileno(), *self.owner)
                os.fchmod(output.fileno(), 0o600)
                output.write(raw)
                output.flush()
                os.fsync(output.fileno())
            self.check_directory()
            try:
                current = os.stat('trip-history.json', dir_fd=self.directory, follow_symlinks=False)
            except FileNotFoundError:
                current = None
            if (None if current is None else metadata(current)) != (None if before is None else metadata(before)):
                raise ValueError('Trip history target changed')
            os.replace(temporary, 'trip-history.json', src_dir_fd=self.directory, dst_dir_fd=self.directory)
            self.target_mark = metadata(os.stat('trip-history.json', dir_fd=self.directory, follow_symlinks=False))
            os.fsync(self.directory)
        finally:
            try:
                os.unlink(temporary, dir_fd=self.directory)
            except FileNotFoundError:
                pass

    def close(self):
        for key in ('lock', 'directory'):
            fd = getattr(self, key, None)
            if fd is not None:
                os.close(fd)
                setattr(self, key, None)


def read_exact(stream, count):
    result = bytearray()
    while len(result) < count:
        part = stream.read(count - len(result))
        if not part:
            raise EOFError('Storage parent retired')
        result.extend(part)
    return bytes(result)


def read_frame(stream):
    count = struct.unpack('!I', read_exact(stream, 4))[0]
    if not 0 < count <= MAX_IPC_BYTES:
        raise ValueError('Trip IPC byte bound')
    return decode(read_exact(stream, count), MAX_IPC_BYTES)


def write_frame(stream, value):
    raw = encode(value)
    stream.write(struct.pack('!I', len(raw)) + raw)
    stream.flush()


def worker(folder, owner, parent_pid):
    store = None
    try:
        if sys.platform.startswith('linux'):
            # Kill a blocked writer if its actual supervisor dies. Check the
            # expected parent after arming, closing the spawn/death race.
            if ctypes.CDLL(None, use_errno=True).prctl(1, 9, 0, 0, 0) != 0 or os.getppid() != parent_pid:
                raise ValueError('Trip storage supervisor retired')
        # Drop the root manager identity before touching display-private storage.
        if os.geteuid() == 0 and owner[0] != 0:
            os.setgroups([])
            os.setgid(owner[1])
            os.setuid(owner[0])
            if sys.platform.startswith('linux'):
                if ctypes.CDLL(None, use_errno=True).prctl(1, 9, 0, 0, 0) != 0 or os.getppid() != parent_pid:
                    raise ValueError('Trip storage supervisor retired')
        if os.getppid() != parent_pid or (os.geteuid(), os.getegid()) != owner:
            raise ValueError('Trip storage supervisor or identity retired')
        # A service control-group stop also signals this child. Only the
        # supervisor owns orderly stop: keep this writer alive to drain its
        # final archive via EOF, with the unchanged bounded kill/reap fallback.
        # The spawn mask closes the exec/startup window; unrelated inherited
        # mask bits stay intact. Parent-death SIGKILL remains unconditional.
        for sig in SOFT_STOP_SIGNALS:
            signal.signal(sig, signal.SIG_IGN)
        signal.pthread_sigmask(signal.SIG_UNBLOCK, SOFT_STOP_SIGNALS)
        store = PrivateStore(folder, owner)
        history = store.load()
        write_frame(sys.stdout.buffer, {'kind': 'ready', 'hostBootId': boot_id(), 'history': history})
        last_trip = last_sport = None
        while True:
            request = read_frame(sys.stdin.buffer)
            if not isinstance(request, dict) or set(request) != {'revision', 'history', 'sport'} or not positive(request['revision']):
                raise ValueError('Trip storage request schema')
            # Both files are bounded independent atomic records. One ack is
            # emitted only after both requested writes have actually synced.
            trip_bytes = envelope(request['history'])
            store.check_directory()
            store.check_target()
            if trip_bytes != last_trip:
                store.save(request['history'])
                last_trip = trip_bytes
            if request['sport'] is not None:
                sport = request['sport']
                if (not isinstance(sport, dict) or set(sport) != {'version', 'sessionId', 'history', 'line'} or
                        type(sport['version']) is not int or sport['version'] != 1 or
                        not positive(sport['sessionId'], 2**53 - 1) or
                        not isinstance(sport['history'], list) or len(sport['history']) > 64):
                    raise ValueError('Sport storage request schema')
                sport_bytes = encode(sport)
                if sport_bytes != last_sport:
                    from sport_source import save_sport_record
                    store.check_directory()
                    save_sport_record(Path(folder), Path(folder) / 'sport-history.json', owner, sport)
                    store.check_directory()
                    last_sport = sport_bytes
            write_frame(sys.stdout.buffer, {'kind': 'saved', 'revision': request['revision']})
    except EOFError:
        return 0
    except (OSError, ValueError, TypeError, KeyError, RecursionError, subprocess.SubprocessError):
        write_frame(sys.stdout.buffer, {'kind': 'failed'})
        return 1
    finally:
        if store is not None:
            store.close()


class HistoryClient:
    """No disk calls in tick/offer; bounded pipes, coalescing, no automatic restart."""
    DEADLINE_SECONDS = 2.0

    def __init__(self, folder, owner):
        self.folder, self.owner = str(folder), owner
        self.process = None
        self.state = 'starting'
        self.error = ''
        self.boot = None
        self.restored = []
        self.owner_id = uuid.uuid4().hex
        self.pending = self.inflight = None
        self.output = b''
        self.input = bytearray()
        self.revision = self.saved_revision = 0
        self.deadline = None
        self.reaped = False
        self.closed = False

    def start(self):
        if self.process is not None or self.closed:
            raise ValueError('Trip storage owner already started or retired')
        # Block in the spawning thread before fork/exec; no preexec_fn in
        # the threaded manager. Child inherits this mask atomically, while
        # the parent's complete previous mask is restored even on spawn error.
        previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, SOFT_STOP_SIGNALS)
        try:
            self.process = subprocess.Popen([sys.executable, '-B', '-E', '-s', str(Path(__file__).resolve()),
                                             '--worker', self.folder, str(self.owner[0]), str(self.owner[1]), str(os.getpid())],
                                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                            close_fds=True, start_new_session=True, bufsize=0)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
        os.set_blocking(self.process.stdin.fileno(), False)
        os.set_blocking(self.process.stdout.fileno(), False)
        self.deadline = time.monotonic() + self.DEADLINE_SECONDS

    def histories(self, current):
        scope = {'classification': 'HISTORICAL', 'ownerId': self.owner_id, 'hostBootId': self.boot,
                 'sourceClock': CLOCK, 'units': UNITS, 'authority': 'software-speed-estimate',
                 'method': 'linear-speed-trapezoids', 'maximumGapNs': TripStatistics.MAXIMUM_GAP_NS,
                 'movingThresholdKmh': TripStatistics.MOVING_THRESHOLD_KMH}
        return copy.deepcopy((self.restored + [{**item, **scope} for item in current])[-TripStatistics.HISTORY_LIMIT:])

    def offer(self, history, sport=None):
        if self.closed or self.state in ('failed', 'retired') or self.boot is None:
            return
        validate_history(history)
        if self.revision >= MAX_DATE:
            self.fail('Historical storage revision limit')
            return
        self.revision += 1
        if sport is not None:
            encode(sport)
        self.pending = {'revision': self.revision, 'history': copy.deepcopy(history), 'sport': copy.deepcopy(sport)}

    def fail(self, reason):
        self.state = 'failed'
        self.error = reason
        self.pending = self.inflight = None
        self.output = b''
        self.deadline = None
        # Acquisition never waits here. Retirement performs bounded wait/reap.
        if self.process and self.process.poll() is None:
            self.process.kill()

    def tick(self):
        if self.closed or not self.process:
            return
        if self.state == 'failed':
            self.reaped = self.process.poll() is not None
            return
        try:
            for _ in range(4):
                try:
                    raw = os.read(self.process.stdout.fileno(), MAX_IPC_BYTES + 4 - len(self.input))
                except BlockingIOError:
                    break
                if not raw:
                    raise ValueError('Trip storage stopped')
                self.input.extend(raw)
                if len(self.input) >= 4:
                    length = struct.unpack('!I', self.input[:4])[0]
                    if not 0 < length <= MAX_IPC_BYTES:
                        raise ValueError('Trip storage reply bound')
                    if len(self.input) > length + 4:
                        raise ValueError('Trip storage extra reply bytes')
                    if len(self.input) == length + 4:
                        message = decode(self.input[4:], MAX_IPC_BYTES)
                        self.input.clear()
                        if not isinstance(message, dict):
                            raise ValueError('Trip storage reply schema')
                        if message.get('kind') == 'ready' and self.state == 'starting' and set(message) == {'kind', 'hostBootId', 'history'}:
                            if not isinstance(message['hostBootId'], str) or not re.fullmatch('[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', message['hostBootId']):
                                raise ValueError('Trip storage boot scope')
                            self.boot = message['hostBootId']
                            self.restored = copy.deepcopy(validate_history(message['history']))
                            self.state = 'ready'
                            self.deadline = None
                        elif message.get('kind') == 'saved' and set(message) == {'kind', 'revision'} and self.inflight is not None and positive(message['revision']) and message['revision'] == self.inflight:
                            self.saved_revision = self.inflight
                            self.inflight = None
                            self.deadline = None
                        else:
                            raise ValueError('Trip storage failed or invalid reply')
            if self.inflight is None and self.pending is not None and self.state == 'ready':
                raw = encode(self.pending, MAX_IPC_BYTES)
                self.inflight = self.pending['revision']
                self.pending = None
                self.output = struct.pack('!I', len(raw)) + raw
                self.deadline = time.monotonic() + self.DEADLINE_SECONDS
            if self.output:
                try:
                    sent = os.write(self.process.stdin.fileno(), self.output)
                    self.output = self.output[sent:]
                except BlockingIOError:
                    pass
            if self.deadline is not None and time.monotonic() >= self.deadline:
                raise ValueError('Trip storage deadline')
        except (OSError, ValueError, TypeError, KeyError, RecursionError):
            self.fail('Historical storage unavailable')

    def status(self):
        if self.closed and self.process is not None and not self.reaped:
            # A previously uninterruptible child can exit later. Observe/reap
            # that real exit without a new writer or a blocking control wait.
            self.reaped = self.process.poll() is not None
        return {'configured': True, 'state': self.state, 'error': self.error,
                'hostBootId': self.boot, 'sourceClock': CLOCK, 'ownerId': self.owner_id,
                'restoredCount': len(self.restored), 'pending': self.pending is not None or self.inflight is not None,
                'savedRevision': self.saved_revision, 'revision': self.revision, 'reaped': self.reaped}

    def close(self):
        if self.closed:
            return
        # Drain at most the existing watchdog interval; a last save is not promised.
        deadline = time.monotonic() + self.DEADLINE_SECONDS
        while self.process and self.state not in ('failed', 'starting') and (self.pending is not None or self.inflight is not None) and time.monotonic() < deadline:
            self.tick()
            time.sleep(.01)
        if self.pending is not None or self.inflight is not None or self.state == 'starting':
            self.fail('Historical storage retirement incomplete')
        self.closed = True
        if self.process:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=.25)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=.25)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    try:
                        self.process.wait(timeout=.25)
                    except subprocess.TimeoutExpired:
                        self.error = 'Historical storage owner not reaped'
                        self.state = 'failed'
            self.reaped = self.process.poll() is not None
            self.process.stdout.close()
        if not self.error:
            self.state = 'retired'


if __name__ == '__main__':
    if len(sys.argv) != 6 or sys.argv[1] != '--worker':
        raise SystemExit(2)
    raise SystemExit(worker(sys.argv[2], (int(sys.argv[3]), int(sys.argv[4])), int(sys.argv[5])))
