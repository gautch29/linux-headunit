# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Optional manager-owned native publication endpoint; no vehicle acquisition.

Linux kernel peer/pidfd/role admission, bounded binary copying and view-model.
The native owner sends once from its existing source. IPC progress is distinct
from original acquisition, and no disk/JSON operation runs in that owner.
"""
from copy import deepcopy
import math
import json
from prephone_consumer import PrephoneConsumer, ROWS, DATE_MAX, AGE_MAX
import os
from pathlib import Path
import select
import secrets
import socket
import stat
import struct
import time

PACKET_SIZE = 344
LEASE_NS = 2_000_000_000
STATES = ('admitted', 'awaiting_source', 'undated_source', 'invalid_snapshot',
          'generation_mismatch', 'disconnected')
AVAILABILITY = ('available', 'missing', 'unavailable', 'stale', 'disconnected')
ORIGINS = ('measured', 'calculated', 'simulation')


def empty(state='awaiting_owner'):
    return {'configured': True, 'version': 1, 'state': state, 'clockDomain': 'host-steady',
            'sourceGeneration': None, 'sourceStartedNs': None, 'sourceSampledNs': None,
            'rows': [dict(zip(('id', 'group', 'label', 'unit'), row[:4]),
                          availability='missing', value=None, acquiredNs=None,
                          maximumAgeNs=None, origin=None) for row in ROWS]}


def decode(raw, nonce, now):
    if (type(raw) is not bytes or len(raw) != PACKET_SIZE or
            raw[:8] != b'HUPN001\0' or raw[8:40] != nonce):
        raise ValueError('Invalid native packet/nonce')
    sequence, copied, state, generation, started, published = struct.unpack_from('<6Q', raw, 40)
    if not sequence or not 0 < copied <= now <= DATE_MAX or state >= len(STATES):
        raise ValueError('Invalid native packet date/sequence/state')
    model = empty(STATES[state])
    if state in (0, 5):
        if not generation or not 0 < started <= published <= copied:
            raise ValueError('Invalid original vehicle generation/dates')
        model.update(sourceGeneration=str(generation), sourceStartedNs=str(started),
                     sourceSampledNs=str(published))
    elif (generation, started, published) != (0, 0, 0):
        raise ValueError('Unavailable source has an invented identity')
    for i, (row, definition) in enumerate(zip(model['rows'], ROWS)):
        at = 88 + i * 32
        availability, origin = raw[at:at + 2]
        value, acquired, age = struct.unpack_from('<dQQ', raw, at + 8)
        if (raw[at + 2:at + 8] != b'\0' * 6 or availability >= len(AVAILABILITY) or
                not math.isfinite(value) or age > AGE_MAX):
            raise ValueError('Malformed native row')
        status = AVAILABILITY[availability]
        if state not in (0, 5):
            if (availability, origin, value, acquired, age) != (1, 255, 0, 0, 0):
                raise ValueError('Unavailable source carries usable observations')
            continue
        if not age or acquired > published or acquired and acquired < started:
            raise ValueError('Invalid original vehicle age/acquisition')
        if (acquired == 0 and (origin != 255 or status not in ('missing', 'disconnected')) or
                acquired != 0 and (origin >= len(ORIGINS) or status == 'missing') or
                status == 'available' and (not acquired or copied - acquired >= age or
                    not definition[4] <= value <= definition[5]) or
                status != 'available' and value != 0 or
                status == 'stale' and (not acquired or copied - acquired < age) or
                state == 5 and status != 'disconnected'):
            raise ValueError('Invalid native observation availability')
        row.update(availability=status, value=value if status == 'available' else None,
                   acquiredNs=str(acquired) if acquired else None, maximumAgeNs=str(age),
                   origin=ORIGINS[origin] if acquired else None)
        if status == 'available' and now - acquired >= age:
            row.update(availability='stale', value=None)
    return sequence, copied, model


class NativeModel:
    """Transport admission feeds the accepted strict eight-row consumer."""
    def __init__(self):
        self.sequence = self.copied = 0
        self.consumer = PrephoneConsumer()
        self.model = empty()
        self.retirement = None

    def accept(self, raw, nonce, now):
        sequence, copied, model = decode(raw, nonce, now)
        if sequence <= self.sequence or copied < self.copied:
            raise ValueError('Native publication replay/reversed copy date')
        generation = int(model['sourceGeneration'] or 0)
        if generation:
            if generation != self.consumer.generation:
                self.consumer.begin(nonce.hex(), generation, int(model['sourceStartedNs']), now, 'host-steady')
            payload = dict(model)
            del payload['configured']
            # Existing supervised manager owns this bounded JSON encoding,
            # never the native source/render/control/VM/GPU callback.
            encoded = json.dumps(payload, separators=(',', ':'), allow_nan=False).encode('ascii')
            self.consumer.accept(encoded, nonce.hex(), now)
        elif self.consumer.generation:
            raise ValueError('Admitted native source lost its metadata')
        self.sequence, self.copied, self.model = sequence, copied, model
        self.retirement = None

    def snapshot(self, now):
        if self.consumer.generation:
            # Clock failure renders only a retired model. The last actual
            # consumer observation is retained, never promoted to a new receipt.
            result = self.consumer.snapshot(self.consumer.last_now if now is None else now)
            result['configured'] = True
        else:
            result = deepcopy(self.model)
        if self.retirement:
            result['state'] = self.retirement
            for row in result['rows']:
                row.update(availability='disconnected', value=None)
        return result

    def retire(self, state):
        # Publisher life is independent of genuine vehicle generation/retirement.
        # Keep the original source identity/dates for review but clear all values.
        self.retirement = state
        if self.consumer.generation and not self.consumer.retired:
            self.consumer.retire(self.consumer.publisher, self.consumer.generation,
                                 self.consumer.last_now)


class NativePublication:
    """One current exact role, one nonblocking private connection, fixed work.

    No PID from a message is authority. Peer credentials, pidfd, /proc executable
    and exact role argv establish it locally. Same time namespace makes Python
    CLOCK_MONOTONIC and production C++ steady dates comparable on Linux.
    """
    def __init__(self, path, owner, command, ambient_enabled=False, *, indicators_enabled=False):
        if not hasattr(os, 'pidfd_open') or not hasattr(socket, 'SO_PEERCRED'):
            raise ValueError('Native publication needs Linux pidfd/peer credentials')
        self.path = Path(path)
        self.owner = owner
        self.command = [str(value) for value in command]
        if (type(indicators_enabled) is not bool or
                ('--prephone-indicators' in self.command) != indicators_enabled):
            raise ValueError('Indicator publication requires matching exact native argv')
        self.indicators_enabled = indicators_enabled
        self.indicators = None
        if indicators_enabled:
            from prephone_indicator_status import IndicatorStatus
            self.indicators = IndicatorStatus()
        self.server = self.connection = None
        self.pidfd = -1
        self.peer = None
        self.nonce = None
        self.model = NativeModel()
        self.ambient_enabled = ambient_enabled
        self.ambient = None
        if ambient_enabled:
            from ambient_light_status import AmbientStatus
            self.ambient = AmbientStatus()
        self.last_now = 0
        self.connected_at = 0
        self.endpoint_identity = None
        self.lock_fd = -1
        self.boot_id = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        self.clock_namespace = os.readlink('/proc/self/ns/time')
        self.error = None
        self._open()

    def _open(self):
        import fcntl
        parent = self.path.parent
        info = parent.lstat()
        if (parent.resolve(strict=True) != parent or not stat.S_ISDIR(info.st_mode) or
                info.st_uid != self.owner[0] or stat.S_IMODE(info.st_mode) != 0o700 or
                len(str(self.path).encode()) > 103 or self.path.name != 'prephone.sock'):
            raise ValueError('Native publication needs exact private manager endpoint')
        try:
            self.lock_fd = os.open(str(self.path) + '.lock', os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
            lock = os.fstat(self.lock_fd)
            if (not stat.S_ISREG(lock.st_mode) or lock.st_nlink != 1 or
                    lock.st_uid not in (0, self.owner[0]) or stat.S_IMODE(lock.st_mode) != 0o600):
                raise ValueError('Unsafe native publication lock')
            fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                old = self.path.lstat()
            except FileNotFoundError:
                pass
            else:
                if not stat.S_ISSOCK(old.st_mode) or old.st_uid != self.owner[0] or stat.S_IMODE(old.st_mode) != 0o600:
                    raise ValueError('Unsafe native publication endpoint')
                # Lock is the exclusive manager admission; still refuse an active
                # endpoint so a legacy/foreign live owner is never unlinked.
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_NONBLOCK) as probe:
                    try:
                        probe.connect(str(self.path))
                    except (ConnectionRefusedError, FileNotFoundError):
                        pass
                    else:
                        raise ValueError('Native publication endpoint is live')
                current = self.path.lstat()
                if (current.st_dev, current.st_ino) != (old.st_dev, old.st_ino):
                    raise ValueError('Native publication endpoint changed')
                self.path.unlink()
            self.server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET | socket.SOCK_NONBLOCK | socket.SOCK_CLOEXEC)
            self.server.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
            self.server.bind(str(self.path))
            os.chmod(self.path, 0o600, follow_symlinks=False)
            if os.geteuid() == 0:
                os.chown(self.path, *self.owner, follow_symlinks=False)
            info = self.path.lstat()
            self.endpoint_identity = info.st_dev, info.st_ino
            self.server.listen(1)
        except BaseException:
            self.close()
            raise

    def _alive(self):
        if self.pidfd < 0 or select.select([self.pidfd], [], [], 0)[0]:
            return False
        # HUP/RDHUP retires even queued bytes and protects admission from a
        # peer PID reuse between SO_PEERCRED and pidfd_open.
        poller = select.poll()
        poller.register(self.connection, select.POLLHUP | select.POLLRDHUP | select.POLLERR)
        return not poller.poll(0)

    def _admit(self, connection):
        pid, uid, gid = struct.unpack('iII', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if (uid, gid) != self.owner:
            raise ValueError('Native publication peer account differs')
        pidfd = os.pidfd_open(pid, 0)
        try:
            # Bounded proc read, never arbitrary executable/argument logging.
            fd = os.open(f'/proc/{pid}/cmdline', os.O_RDONLY | os.O_CLOEXEC)
            try:
                raw = os.read(fd, 8193)
            finally:
                os.close(fd)
            if (len(raw) > 8192 or not raw.endswith(b'\0') or
                    raw[:-1].split(b'\0') != [value.encode() for value in self.command] or
                    os.readlink(f'/proc/{pid}/exe') != self.command[0] or
                    os.readlink(f'/proc/{pid}/ns/time') != self.clock_namespace or
                    select.select([pidfd], [], [], 0)[0]):
                raise ValueError('Native publication peer is not the admitted role/clock')
            poller = select.poll()
            poller.register(connection, select.POLLHUP | select.POLLRDHUP | select.POLLERR)
            if poller.poll(0):
                raise ValueError('Native publication peer already retired')
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
            nonce = secrets.token_bytes(32)
            if connection.send(nonce) != len(nonce):
                raise ValueError('Native publication handshake failed')
            admitted_at = self._observe()
            if admitted_at is None:
                raise ValueError('Native publication admission clock reversed')
            self.connection, self.pidfd, self.peer = connection, pidfd, (pid, uid, gid)
            self.nonce, self.connected_at = nonce, admitted_at
            self.model = NativeModel()
            if self.indicators_enabled:
                from prephone_indicator_status import IndicatorStatus
                self.indicators = IndicatorStatus()
            if self.ambient_enabled:
                from ambient_light_status import AmbientStatus
                self.ambient = AmbientStatus()
            self.error = None
            pidfd = -1
        finally:
            if pidfd >= 0:
                os.close(pidfd)

    def _retire(self, reason):
        self.model.retire(reason)
        if self.ambient:
            self.ambient.retire(reason)
        if self.indicators:
            self.indicators.retire(reason)
        if self.connection:
            self.connection.close()
        self.connection = None
        if self.pidfd >= 0:
            os.close(self.pidfd)
        self.pidfd = -1
        self.peer = self.nonce = None

    def _observe(self):
        """Actual Linux monotonic observation, never a pre-poll/fabricated receipt."""
        now = time.monotonic_ns()
        if type(now) is not int or not 0 < now <= DATE_MAX or now < self.last_now:
            self._retire('clock_failure')
            return None
        self.last_now = now
        return now

    def poll(self):
        now = self._observe()
        if now is None:
            return
        if self.connection and not self._alive():
            self._retire('owner_retired')
        # Accept at most ONE connection per invocation. Never queue a second owner.
        if self.server:
            try:
                connection, _ = self.server.accept()
            except BlockingIOError:
                pass
            else:
                connection.setblocking(False)
                if self.connection:
                    connection.close()
                else:
                    try:
                        self._admit(connection)
                    except (OSError, ValueError):
                        connection.close()
                        self.error = 'owner_admission_failed'
        if not self.connection:
            return
        try:
            for _ in range(16):
                try:
                    limit = 2560 if self.ambient else 872 if self.indicators else PACKET_SIZE
                    raw, ancillary, flags, _ = self.connection.recvmsg(limit + 1, socket.CMSG_SPACE(12), socket.MSG_DONTWAIT)
                except BlockingIOError:
                    break
                received_at = self._observe()
                if received_at is None:
                    return
                if not raw or not self._alive():
                    self._retire('owner_retired')
                    return
                credentials = [struct.unpack('iII', value) for level, kind, value in ancillary
                               if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS and len(value) == 12]
                if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or credentials != [self.peer] or not self._alive():
                    raise ValueError('Native publication sender/protocol changed')
                if self.indicators and raw[:8] == b'HUPI001\0':
                    self.indicators.accept(raw, self.nonce, received_at, self.model)
                elif self.ambient and raw[:8] == b'HUAL001\0':
                    self.ambient.accept(raw, self.nonce, received_at)
                else:
                    self.model.accept(raw, self.nonce, received_at)
            # Original copy date bounds liveness. A new poll/file date cannot renew it.
            now = self._observe()
            if now is None:
                return
            if now - (self.model.copied or self.connected_at) >= LEASE_NS:
                self._retire('publisher_expired')
        except (OSError, ValueError):
            self._retire('protocol_failure')

    def observations(self):
        """One bounded observation, one clock and peer scope for all opted models.

        Copying does not admit another owner or consume more queued packets.
        Recheck kernel retirement after all projections so a peer that dies
        during copying withdraws all, retaining their original source dates.
        """
        self.poll()
        now = self._observe()
        # Recheck at dispatch, even when a peer dies after the bounded read.
        if self.connection and not self._alive():
            self._retire('owner_retired')
        if self.connection and now is not None and now - (self.model.copied or self.connected_at) >= LEASE_NS:
            self._retire('publisher_expired')
        result = self._observations(now)
        if self.connection and not self._alive():
            self._retire('owner_retired')
            result = self._observations(now)
        return result

    def snapshots(self):
        # Public accepted paired tuple remains exactly two independently copied
        # projections. The shared internal observation never polls separately.
        return self.observations()[:2]

    def _observations(self, now):
        vehicle, ambient = self._snapshots(now)
        if self.indicators:
            indicators = self.indicators.snapshot(now, vehicle)
            indicators['publisher'] = deepcopy(vehicle['publisher'])
            indicators['error'] = indicators['error'] or self.error
        else:
            indicators = {'configured': False, 'state': 'disabled'}
        return vehicle, ambient, indicators

    def _snapshots(self, now):
        result = self.model.snapshot(now)
        result['publisher'] = {'live': self.connection is not None,
                               'sequence': str(self.model.sequence) if self.model.sequence else None,
                               'copiedNs': str(self.model.copied) if self.model.copied else None,
                               'hostBootId': self.boot_id, 'error': self.error}
        if self.ambient:
            ambient = self.ambient.snapshot(now)
            ambient['clock_domain'] = 'host-steady'
            ambient['publisher'] = deepcopy(result['publisher'])
        else:
            ambient = {'configured': False, 'available': False, 'provider_available': False,
                       'settings': None, 'requested': None, 'current': None, 'command': None,
                       'revision': None, 'state': 'disabled'}
        return result, ambient

    def snapshot(self):
        return self.snapshots()[0]

    def close(self):
        self._retire('owner_retired')
        if self.server:
            self.server.close()
            self.server = None
        if self.endpoint_identity:
            try:
                info = self.path.lstat()
                if (info.st_dev, info.st_ino) == self.endpoint_identity:
                    self.path.unlink()
            except OSError:
                pass
            self.endpoint_identity = None
        if self.lock_fd >= 0:
            os.close(self.lock_fd)
            self.lock_fd = -1

    def indicator_snapshot(self):
        return self.observations()[2]

    def ambient_snapshot(self):
        return self.snapshots()[1]

    def ambient_change(self, data):
        self.snapshot()
        now = self._observe()
        if not self.ambient or not self.connection or not self._alive() or now is None:
            if self.connection and not self._alive():
                self._retire('owner_retired')
            raise ValueError('Ambient software owner unavailable')
        raw, pending = self.ambient.prepare(data, now, self.nonce)
        try:
            sent = self.connection.send(raw, socket.MSG_DONTWAIT | socket.MSG_NOSIGNAL)
        except BlockingIOError as error:
            raise ValueError('Ambient settings transport busy; request not sent') from error
        except OSError as error:
            self._retire('protocol_failure')
            raise ValueError('Ambient settings transport failed') from error
        if sent != len(raw):
            self._retire('protocol_failure')
            raise ValueError('Ambient settings transport failed')
        self.ambient.sent(pending)
        return {'message': 'Ambient software settings requested', 'confirmed': False,
                'physical_confirmed': False, 'software_action': 'pending'}
