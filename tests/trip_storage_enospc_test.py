#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Explicit Linux small-tmpfs qualification of the actual private storage owner.

Run as root inside an isolated container with --tmpfs /trip-fault:size=128k,
mode=0755. Never falls back to a host directory or fills a normal filesystem.
The fixture uses production clocks, acquisition, subprocesses and file writes.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import pwd
import signal
import stat
import sys
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait(predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.01)
    raise AssertionError('Actual owner did not reach expected bounded state')


def qualify(root):
    require(sys.platform.startswith('linux') and os.geteuid() == 0,
            'Requires isolated Linux root fixture to observe actual UID drop')
    require(root.is_absolute() and root.resolve(strict=True) == root,
            'Canonical explicit tmpfs mount required')
    mounts = [line.split() for line in Path('/proc/self/mountinfo').read_text().splitlines()]
    matches = [fields for fields in mounts if fields[4] == str(root) and fields[fields.index('-') + 1] == 'tmpfs']
    require(len(matches) == 1, 'Fixture must be an actual dedicated tmpfs mount')
    capacity = os.statvfs(root)
    total_bytes = capacity.f_blocks * capacity.f_frsize
    require(65536 <= total_bytes <= 1048576, 'Fixture capacity must be 64 KiB–1 MiB')
    require(not list(root.iterdir()), 'Fixture mount must be empty and exclusively owned')
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
    from sport_source import SportSource
    from trip_history import PrivateStore, boot_id, metadata, restore

    account = pwd.getpwnam('nobody')
    owner = account.pw_uid, account.pw_gid
    folder = root / 'private'
    folder.mkdir(mode=0o700)
    os.chown(folder, *owner)
    source = successor = None
    ballast = root / 'fixture-ballast'
    probe = None
    result = {'fixture': 'actual-linux-tmpfs-enospc', 'capacityBytes': total_bytes,
              'runtimeFix': False, 'authority': 'simulated-component', 'newUiPieces': []}
    try:
        source = SportSource(folder, simulated=True, owner=owner, trip_history=True)
        source.start()
        wait(lambda: source.snapshot()['trip']['storage']['state'] == 'ready')
        wait(lambda: source.snapshot()['trip']['distanceKm'] is not None and
             source.snapshot()['trip']['distanceKm'] > 0, timeout=3)
        first_reset = source.request({'action': 'trip_reset'})
        wait(lambda: source.snapshot()['trip']['storage']['savedRevision'] > 0 and
             not source.snapshot()['trip']['storage']['pending'])
        wait(lambda: source.snapshot()['trip']['distanceKm'] is not None and
             source.snapshot()['trip']['distanceKm'] > 0)
        before = source.snapshot()
        pid = source.storage.process.pid
        thread = source.thread
        require(thread.is_alive(), 'Actual acquisition worker is alive')
        require(source.storage.process.poll() is None, 'Single writer is alive before fault')
        fields = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
        require([int(v) for v in fields['Uid'].split()] == [owner[0]] * 4, 'Actual child dropped all UID identities')
        require([int(v) for v in fields['Gid'].split()] == [owner[1]] * 4, 'Actual child dropped all GID identities')
        require(not fields['Groups'].split(), 'Actual child dropped supplementary groups')
        paths = [folder / name for name in ('.trip-history.lock', 'trip-history.json', 'sport-history.json')]
        marks = {p.name: metadata(p.stat()) for p in paths}
        original = {p.name: p.read_bytes() for p in paths}
        for p in paths:
            info = p.stat()
            require((info.st_uid, info.st_gid) == owner and stat.S_IMODE(info.st_mode) == 0o600 and
                    info.st_nlink == 1, 'Private single-link file retains actual dropped ownership')
        persisted = restore(original['trip-history.json'])
        require(len(persisted) == 1 and persisted == first_reset['trip']['history'],
                'First real reset was acknowledged with exact original archive')
        require(persisted[0]['hostBootId'] == boot_id(), 'Archive carries actual OS boot scope')
        acknowledged = before['trip']['storage']['savedRevision']
        require(acknowledged == before['trip']['storage']['revision'], 'Baseline has no unacknowledged work')
        result['baseline'] = {'storage': before['trip']['storage'], 'archive': persisted,
                              'originalFileSha256': {k: hashlib.sha256(v).hexdigest() for k, v in original.items()},
                              'originalFileMetadata': marks, 'writerUid': owner[0], 'writerGid': owner[1],
                              'sourceThreadId': thread.ident, 'sourceNativeThreadId': thread.native_id}

        # Hold only the actual writer while the isolated mount is filled. No clock,
        # file API, production deadline, buffer, UID or acquisition policy changes.
        os.kill(pid, signal.SIGSTOP)
        written = 0
        fd = os.open(ballast, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            while True:
                require(written <= total_bytes, 'Bounded ballast never exceeds isolated tmpfs capacity')
                try:
                    written += os.write(fd, bytes(4096))
                except OSError as error:
                    require(error.errno == errno.ENOSPC, 'Actual filesystem returns ENOSPC')
                    result['ballastErrno'] = error.errno
                    break
        finally:
            os.close(fd)
        require(os.statvfs(root).f_bavail == 0, 'Actual dedicated tmpfs has no free data blocks')

        # The live child still owns the real lock while stalled. A rejected
        # second constructor must close its descriptors without another writer.
        try:
            probe = PrivateStore(folder, owner)
            raise AssertionError('Live child lock must exclude independent writer')
        except BlockingIOError as error:
            require(error.errno in (errno.EAGAIN, errno.EWOULDBLOCK),
                    'Actual existing writer retains exclusive flock')
        reset = source.request({'action': 'trip_reset'})
        require(reset['trip']['storage']['pending'], 'Reset response honestly reports pending work')
        os.kill(pid, signal.SIGCONT)
        wait(lambda: source.snapshot()['trip']['storage']['state'] == 'failed')
        wait(lambda: source.snapshot()['trip']['storage']['reaped'])
        failed = source.snapshot()
        require(failed['trip']['storage']['savedRevision'] == acknowledged,
                'No acknowledgement/durable revision manufactured after failure')
        require(failed['trip']['storage']['revision'] > acknowledged and
                not failed['trip']['storage']['pending'], 'Failed work retires without a hidden queue')
        require(source.storage.process.poll() is not None, 'Production consumed actual child wait status')
        for p in paths:
            require(p.read_bytes() == original[p.name] and metadata(p.stat()) == marks[p.name],
                    'Failed replacement preserves original acknowledged files/inodes')
        require(not list(folder.glob('.trip-history-*')) and not list(folder.glob('.sport-history-*')),
                'Ordinary write failure cleans only its own temporary')

        # Lock is now retired. Exact production PrivateStore.save on the same
        # exhausted mount supplies independent OS errno without monkeypatches.
        probe = PrivateStore(folder, owner)
        probe.load()
        try:
            probe.save(reset['trip']['history'])
            raise AssertionError('Actual exhausted filesystem must reject production save')
        except OSError as error:
            require(error.errno == errno.ENOSPC, 'Production file save actually failed with ENOSPC')
            result['productionSaveErrno'] = error.errno
        finally:
            probe.close()
            probe = None
        require(not list(folder.glob('.trip-history-*')), 'Actual ENOSPC cleans its own temporary')

        snapshots = []
        for _ in range(10):
            snapshots.append(source.snapshot())
            time.sleep(.02)
        trips = [model['trip'] for model in snapshots]
        require(source.thread is thread and thread.is_alive(), 'Same actual acquisition worker survives failure')
        require(len({trip['lastAcquiredNs'] for trip in trips}) >= 5, 'Original speed acquisitions continue')
        require(all(a['lastAcquiredNs'] <= b['lastAcquiredNs'] and
                    (a['distanceKm'] or 0) <= (b['distanceKm'] or 0) and
                    (a['coveredSeconds'] or 0) <= (b['coveredSeconds'] or 0)
                    for a, b in zip(trips, trips[1:])), 'Acquired dates/estimated totals remain monotonic')
        require(trips[-1]['lastAcquiredNs'] > reset['observedNs'] and trips[-1]['distanceKm'] > 0,
                'New trip uses strictly post-action actual moving acquisitions')
        for model in snapshots:
            speed = model['inputs']['speed_kmh']
            trip = model['trip']
            require(model['source'] == 'simulation' and trip['origin'] == 'simulation' and
                    speed['origin'] == 'simulation' and speed['availability'] == 'available',
                    'Original acquisition provenance/freshness remains explicit')
            require(speed['acquiredNs'] == trip['lastAcquiredNs'], 'Snapshot preserves original speed acquisition')
            require(trip['storage']['state'] == 'failed' and trip['storage']['savedRevision'] == acknowledged,
                    'Failure remains latched with no retry or late ACK')
            require(source.storage.process.pid == pid and source.storage.process.poll() is not None,
                    'No replacement writer starts')
        result['failure'] = {'reset': reset, 'storage': failed['trip']['storage'],
                             'childReturncode': source.storage.process.returncode,
                             'snapshots': snapshots, 'ballastWrittenBytes': written}
        ballast.unlink()
        require(os.statvfs(root).f_bavail > 0, 'Capacity actually returns in the isolated tmpfs')
        time.sleep(.06)
        recovered_capacity = source.snapshot()['trip']['storage']
        require(recovered_capacity['state'] == 'failed' and recovered_capacity['reaped'] and
                recovered_capacity['savedRevision'] == acknowledged and
                source.storage.process.pid == pid and source.storage.process.poll() is not None,
                'Capacity release does not restart storage or acknowledge failed revisions')
        result['afterCapacityRelease'] = recovered_capacity
        source.close()
        retired = source.snapshot()
        require(not thread.is_alive() and source.storage.closed and source.storage.reaped,
                'Actual ordered retirement joins acquisition and reaps writer')
        require(retired['trip']['storage']['state'] == 'failed' and
                retired['trip']['storage']['savedRevision'] == acknowledged,
                'Failed final archive cannot become an acknowledged close')
        require(len(retired['trip']['history']) == 3, 'Close retains final current trip once in memory')
        require(retired['inputs']['speed_kmh']['value'] is None and
                retired['inputs']['speed_kmh']['availability'] == 'disconnected',
                'Retirement withdraws source instead of manufacturing zero')
        source.close()
        require(source.snapshot()['trip']['history'] == retired['trip']['history'], 'Repeated close is idempotent')
        require((folder / 'trip-history.json').read_bytes() == original['trip-history.json'],
                'Failed/unacknowledged archives never replace durable baseline')
        result['retirement'] = {'storage': retired['trip']['storage'], 'memoryHistoryCount': 3,
                                'sourceJoined': True, 'writerReaped': True}
        successor = SportSource(folder, owner=owner, trip_history=True)
        successor.start()
        wait(lambda: successor.snapshot()['trip']['storage']['state'] == 'ready')
        restored = successor.snapshot()
        require(restored['trip']['history'] == persisted and
                restored['trip']['storage']['restoredCount'] == 1, 'Only acknowledged baseline restores as history')
        require(restored['trip']['storage']['ownerId'] != before['trip']['storage']['ownerId'],
                'New owner scope differs; historical scope stays original')
        for key in ('firstAcquiredNs', 'lastAcquiredNs', 'distanceKm', 'coveredSeconds', 'peak', 'origin'):
            require(restored['trip'][key] is None, 'Historical restore never supplies live ' + key)
        require(restored['inputs']['speed_kmh']['value'] is None, 'No-source successor remains unavailable')
        require(successor.engine.trip.acquisition_floor == 0 and successor.engine.trip.reset_floor == 0,
                'Restore imports no live acquisition/reset watermarks')
        result['restore'] = restored
        successor.close()
        require(not successor.thread.is_alive() and successor.storage.reaped and
                successor.storage.process.returncode == 0, 'Healthy successor closes/reaps after capacity release')
        require(restore((folder / 'trip-history.json').read_bytes()) == persisted,
                'Unknown successor creates no current archive')
        result['pass'] = True
        return result
    finally:
        if probe:
            probe.close()
        for current in (source, successor):
            if current:
                current.close()
                require(not current.thread.is_alive(), 'No actual acquisition worker remains on retirement')
                require(current.storage.process.poll() is not None and current.storage.reaped,
                        'No active/unreaped writer remains on retirement')
        if ballast.exists():
            ballast.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tmpfs-root', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = qualify(args.tmpfs_root)
    except Exception as error:
        args.report.write_text(json.dumps({'pass': False, 'error': str(error)}, indent=2) + '\n')
        raise
    args.report.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print('PASS: actual small-tmpfs ENOSPC / acquisition preservation / ACK authority / retirement / historical-only restore')


if __name__ == '__main__':
    main()
