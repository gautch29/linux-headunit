#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Private, bounded Linux vehicle-snapshot fan-out with Pi ingress timestamps."""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import stat
import struct
import sys
import time

MAX_LEGACY_PACKET = 2048
MAX_PACKET = 8192  # HVO1/HVO2/HVO3/HVO4 complete acquisition snapshots; HUV1/HUV2 stay at 2048.
MAX_QUEUE_AGE_NS = 250_000_000
# SO_TIMESTAMPNS_OLD on the supported 64-bit Linux CM5. Python does not expose
# this constant on every Linux build; its 64-bit timespec is two native longs.
TIMESTAMPNS = 35
TIMESPEC = struct.Struct('@ll')


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate vehicle fan-out field')
        result[key] = value
    return result


def socket_path(value):
    if (not isinstance(value, str) or not value.startswith('/') or
            len(os.fsencode(value)) > 107 or '\0' in value or
            os.path.normpath(value) != value):
        raise ValueError('Vehicle fan-out paths must be exact absolute Unix socket paths')
    return Path(value)


def load_configuration(path, owner=None, release_binding=None):
    owner = os.getuid() if owner is None else owner
    sealed = False
    if release_binding:
        from appliance_release import configuration_member
        sealed = configuration_member(path, release_binding, 'fanout')
        if not sealed: raise ValueError('Fan-out configuration is not in its bound release')
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or
                (not sealed and (info.st_uid != owner or info.st_mode & 0o077)) or
                not 0 < info.st_size <= 4096):
            raise ValueError('Vehicle fan-out configuration must be private and owned by the display user')
        raw = os.read(fd, 4097)
        if len(raw) != info.st_size:
            raise ValueError('Vehicle fan-out configuration changed while reading')
    finally:
        os.close(fd)
    config = json.loads(raw, object_pairs_hook=unique)
    if (not isinstance(config, dict) or set(config) != {'version', 'input', 'consumers'} or
            type(config['version']) is not int or config['version'] != 1 or
            not isinstance(config['consumers'], dict) or
            not {'backup', 'receiver'} <= set(config['consumers']) or
            not set(config['consumers']) <= {'backup', 'receiver', 'ultra', 'hud', 'sport'}):
        raise ValueError('Invalid vehicle fan-out schema')
    result = {'input': socket_path(config['input']),
              'consumers': {name: socket_path(value) for name, value in config['consumers'].items()}}
    paths = [result['input'], *result['consumers'].values()]
    if len(set(paths)) != len(paths):
        raise ValueError('Vehicle fan-out input and consumer paths must all differ')
    return result


def private_parent(path, owner):
    parent = path.parent
    info = parent.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != owner or
            info.st_mode & 0o077 or parent.resolve() != parent):
        raise ValueError('Vehicle socket parent must be an existing private directory owned by its user')


def installed_ui_source(path, uid):
    # Validation runs before the native fallback starts. Reject a FIFO/device
    # without waiting for a writer, and pin the exact bounded profile bytes.
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid not in (0, uid) or
                info.st_mode & 0o022 or not 0 < info.st_size <= 8192):
            raise ValueError('Installed UI source profile is not a bounded owned regular file')
        raw = os.read(fd, 8193)
        after, named = os.fstat(fd), os.lstat(path)
        def identity(value):
            return (value.st_dev, value.st_ino, value.st_uid, value.st_gid,
                    value.st_mode, value.st_nlink, value.st_size,
                    value.st_mtime_ns, value.st_ctime_ns)

        if (len(raw) != info.st_size or identity(info) != identity(after) or
                identity(after) != identity(named) or not stat.S_ISREG(named.st_mode)):
            raise ValueError('Installed UI source profile changed while reading')
        text = raw.decode()
        if '\0' in text or '\r' in text:
            raise ValueError('Installed UI source profile changed or contains unsupported characters')
    finally:
        os.close(fd)
    fields = {}
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        key, separator, value = line.partition('=')
        if not separator or not key or not value or key in fields:
            raise ValueError('Invalid installed UI source profile')
        fields[key] = value
    return fields


def vehicle_arguments(settings, uid, fanout_path, release_binding=None):
    simulated = settings.get('simulate_vehicle', False)
    if type(simulated) is not bool:
        raise ValueError('simulate_vehicle must be an explicit boolean')
    dated = settings.get('simulate_dated_vehicle', False)
    if type(dated) is not bool:
        raise ValueError('simulate_dated_vehicle must be an explicit boolean')
    if dated and (simulated or settings.get('vehicle_socket') or fanout_path):
        raise ValueError('Dated simulation excludes legacy simulation, vehicle socket and fan-out')
    if fanout_path:
        if simulated:
            raise ValueError('Choose simulation or vehicle fan-out')
        fanout = load_configuration(fanout_path, uid, release_binding)
        for path in [fanout['input'], *fanout['consumers'].values()]:
            private_parent(path, uid)
        if settings.get('vehicle_socket') != fanout['consumers']['receiver']:
            raise ValueError('Receiver vehicle_socket must match the fan-out receiver consumer')
        if settings.get('ultra_installed_ui'):
            ui = installed_ui_source(settings['ultra_installed_ui'], uid)
            if ui.get('simulateGauges', 'false') != 'false' or 'vehicleSocket' not in ui:
                raise ValueError('Ultra must use its own fan-out vehicleSocket, without gauge simulation')
            if socket_path(ui['vehicleSocket']) != fanout['consumers'].get('ultra'):
                raise ValueError('Installed UI vehicleSocket must match the fan-out Ultra consumer')
        elif 'ultra' in fanout['consumers']:
            raise ValueError('An Ultra fan-out consumer needs an installed UI profile')
        return ['--vehicle-socket', str(fanout['consumers']['backup'])]
    if settings.get('vehicle_socket'):
        raise ValueError('Concurrent receiver/backup vehicle sources require --vehicle-fanout-config')
    return ['--simulate-dated'] if dated else ['--simulate'] if simulated else []


def ingress_packet(data, flags, ancillary, realtime_ns, monotonic_ns):
    """Drop queued input; HVO dates are producer-owned and forwarded unchanged."""
    observed = data.startswith((b'HVO1 ', b'HVO2 ', b'HVO3 ', b'HVO4 ', b'HVO6 '))
    if (not data or len(data) > (MAX_PACKET if observed else MAX_LEGACY_PACKET) or
            flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or
            not data.startswith((b'HUV1 ', b'HUV2 ', b'HVO1 ', b'HVO2 ', b'HVO3 ', b'HVO4 ', b'HVO6 ')) or b'\0' in data):
        return None
    stamps = [payload for level, kind, payload in ancillary
              if level == socket.SOL_SOCKET and kind == TIMESTAMPNS]
    if len(stamps) != 1 or len(stamps[0]) != TIMESPEC.size:
        return None
    seconds, nanoseconds = TIMESPEC.unpack(stamps[0])
    if seconds < 0 or not 0 <= nanoseconds < 1_000_000_000:
        return None
    age = realtime_ns - (seconds * 1_000_000_000 + nanoseconds)
    if not 0 <= age <= MAX_QUEUE_AGE_NS or monotonic_ns <= age:
        return None
    # HVO packets carry acquisition and source publication dates. Parsing
    # and replay validation belong to each consumer; never overwrite them with
    # ingress time or wrap observations in the legacy envelope.
    return data if observed else b'HUF1 ' + str(monotonic_ns - age).encode('ascii') + b' ' + data


class Fanout:
    def __init__(self, config):
        if not sys.platform.startswith('linux') or TIMESPEC.size != 16:
            raise RuntimeError('Vehicle fan-out requires 64-bit Linux kernel receive timestamps')
        self.owner = os.getuid()
        self.config = config
        self.path = config['input']
        self.lock = None
        self.socket = None
        self.bound = None
        self.stats = {'received': 0, 'discarded': 0, 'delivered': 0, 'unavailable': 0}
        try:
            for path in [self.path, *config['consumers'].values()]:
                private_parent(path, self.owner)
            lock_path = self.path.with_name(self.path.name + '.lock')
            self.lock = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
            info = os.fstat(self.lock)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != self.owner or info.st_mode & 0o077:
                raise ValueError('Vehicle fan-out lock has an unexpected owner or type')
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                old = self.path.lstat()
            except FileNotFoundError:
                pass
            else:
                if not stat.S_ISSOCK(old.st_mode) or old.st_uid != self.owner or old.st_mode & 0o077:
                    raise ValueError('Vehicle input path has an unexpected owner or type')
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as probe:
                    try:
                        probe.connect(str(self.path))
                    except OSError as error:
                        if error.errno not in (errno.ECONNREFUSED, errno.ENOENT):
                            raise
                    else:
                        raise ValueError('Vehicle input already has a live owner')
                try:
                    current = self.path.lstat()
                except FileNotFoundError:
                    pass
                else:
                    if (current.st_dev, current.st_ino) != (old.st_dev, old.st_ino):
                        raise ValueError('Vehicle input endpoint changed during recovery')
                    self.path.unlink()
            self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            self.socket.setblocking(False)
            self.socket.setsockopt(socket.SOL_SOCKET, TIMESTAMPNS, 1)
            self.socket.bind(str(self.path))
            info = self.path.lstat()
            self.bound = (info.st_dev, info.st_ino)
            os.chmod(self.path, 0o600)
        except BaseException:
            self.close()
            raise

    def turn(self):
        # Fixed work bound. Do not cache a last sample: absent/restarted consumers
        # only receive a new adapter packet, never a replay given a new age.
        for _ in range(64):
            try:
                data, ancillary, flags, _ = self.socket.recvmsg(MAX_PACKET + 1, socket.CMSG_SPACE(TIMESPEC.size))
            except BlockingIOError:
                break
            self.stats['received'] += 1
            packet = ingress_packet(data, flags, ancillary, time.time_ns(), time.monotonic_ns())
            if packet is None:
                self.stats['discarded'] += 1
                continue
            for path in self.config['consumers'].values():
                try:
                    # Recheck before every send. A replaced/symlink/non-socket
                    # consumer never receives data from the trusted adapter.
                    private_parent(path, self.owner)
                    info = path.lstat()
                    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != self.owner or info.st_mode & 0o077:
                        raise ValueError('Vehicle consumer owner or type changed')
                    self.socket.sendto(packet, str(path))
                    self.stats['delivered'] += 1
                except (OSError, ValueError):
                    self.stats['unavailable'] += 1

    def close(self):
        if self.socket is not None:
            self.socket.close()
            self.socket = None
        if self.bound:
            try:
                info = self.path.lstat()
                if (info.st_dev, info.st_ino) == self.bound:
                    self.path.unlink()
            except FileNotFoundError:
                pass
            self.bound = None
        if self.lock is not None:
            os.close(self.lock)
            self.lock = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--release-root', type=Path)
    parser.add_argument('--release-manifest-sha256')
    args = parser.parse_args()
    os.umask(0o077)
    if bool(args.release_root) != bool(args.release_manifest_sha256):
        parser.error('A release root and manifest digest must be supplied together')
    binding = {'path': str(args.release_root), 'sha256': args.release_manifest_sha256} if args.release_root else None
    fanout = Fanout(load_configuration(args.config, release_binding=binding))
    stopping = False
    def stop(*_):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(fanout.socket, selectors.EVENT_READ)
            while not stopping:
                if selector.select(.25):
                    fanout.turn()
    finally:
        fanout.close()


if __name__ == '__main__':
    main()
