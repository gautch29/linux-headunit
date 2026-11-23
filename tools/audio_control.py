# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Private, route-scoped head-unit mixer requests; never an audible receipt."""
import json
import os
from pathlib import Path
import re
import stat
import threading
import time

from phone_state import atomic_json
from private_state import read_private, unique_object

MAX_SEQUENCE = (1 << 64) - 1
STATE_KEYS = {'version', 'updatedMonotonicNs', 'available', 'route',
              'acceptedSequence', 'appliedSequence', 'requestedVolumePercent',
              'requestedMuted', 'volumePercent', 'muted', 'error'}
OUTPUT_KEYS = {'sampledMonotonicNs', 'acceptedBlocks', 'discontinuities', 'underruns', 'starved'}
TIMING_KEYS = {'scheduledAheadNs', 'maximumOwnerGapNs',
               'lastUnderrunOwnerGapNs', 'lastUnderrunLateNs', 'queuedBytes', 'lastUnderrunQueuedBytes'}
OUTPUT_MAX_AGE_NS = 1_000_000_000


def monotonic_time_ns():
    """Use the wire clock explicitly; Python's Mac default has another origin."""
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def decimal(value, positive=False):
    if (type(value) is not str or not re.fullmatch(r'0|[1-9][0-9]{0,19}', value) or
            int(value) > MAX_SEQUENCE or (positive and value == '0')):
        raise ValueError('Invalid audio sequence or timestamp')
    return int(value)


def level(volume, muted):
    if type(volume) is not int or not 0 <= volume <= 100 or type(muted) is not bool:
        raise ValueError('Audio volume must be 0..100 and mute must be boolean')


def unavailable():
    return {'available': False, 'route': '', 'pending': False}


def validate_state(value, now_ns):
    if (type(value) is not dict or set(value) not in (STATE_KEYS, STATE_KEYS | {'output'}) or
            type(value['version']) is not int or value['version'] != 1):
        raise ValueError('Invalid audio state schema')
    if type(now_ns) is not int or now_ns <= 0:
        raise ValueError('Invalid audio clock')
    updated = decimal(value['updatedMonotonicNs'], positive=True)
    if not 0 <= now_ns - updated <= 3_000_000_000:
        raise ValueError('Audio state is stale or from a future clock')
    if type(value['available']) is not bool or type(value['route']) is not str:
        raise ValueError('Invalid audio route')
    if (value['available'] and not re.fullmatch(r'[0-9a-f]{32}', value['route'])) or (not value['available'] and value['route'] != ''):
        raise ValueError('Invalid audio route')
    accepted, applied = decimal(value['acceptedSequence']), decimal(value['appliedSequence'])
    if applied > accepted:
        raise ValueError('Applied audio sequence exceeds accepted sequence')
    level(value['requestedVolumePercent'], value['requestedMuted'])
    level(value['volumePercent'], value['muted'])
    if accepted == applied and (value['requestedVolumePercent'] != value['volumePercent'] or value['requestedMuted'] != value['muted']):
        raise ValueError('Confirmed audio levels disagree at the same sequence')
    error = value['error']
    if type(error) is not str or len(error) > 128 or any(not 32 <= ord(c) <= 126 for c in error):
        raise ValueError('Invalid audio error')
    result = dict(value)
    if 'output' in value:
        output = value['output']
        if (not value['available'] or type(output) is not dict or set(output) not in (OUTPUT_KEYS, OUTPUT_KEYS | {'timing'}) or
                type(output['starved']) is not bool):
            raise ValueError('Invalid audio output observation')
        sampled = decimal(output['sampledMonotonicNs'], positive=True)
        if sampled > updated:
            raise ValueError('Audio output observation is from a future clock')
        for key in ('acceptedBlocks', 'discontinuities', 'underruns'):
            decimal(output[key])
        if 'timing' in output:
            timing = output['timing']
            if type(timing) is not dict or set(timing) != TIMING_KEYS:
                raise ValueError('Invalid audio output timing')
            for number in timing.values():
                decimal(number)
        result['output'] = dict(output, fresh=now_ns - sampled <= OUTPUT_MAX_AGE_NS)
    return result


class AudioControl:
    def __init__(self, folder, owner, volume=100, muted=False, clock=monotonic_time_ns):
        level(volume, muted)
        if owner is None:
            raise ValueError('Head-unit audio controls require a configured owner')
        self.folder, self.owner, self.clock = Path(folder).resolve(), owner, clock
        info = self.folder.lstat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != owner[0] or
                stat.S_IMODE(info.st_mode) != 0o700):
            raise ValueError('Audio control directory must be owner-only (0700)')
        self.control_path = self.folder / 'audio-control.json'
        self.state_path = self.folder / 'audio-state.json'
        self.preference_path = self.folder / 'audio-preferences.json'
        self.volume, self.muted = volume, muted
        try:
            saved = read_private(self.preference_path, owner)
        except FileNotFoundError:
            pass
        else:
            if (type(saved) is not dict or set(saved) != {'version', 'volumePercent', 'muted'} or
                    type(saved['version']) is not int or saved['version'] != 1):
                raise ValueError('Invalid audio preferences')
            level(saved['volumePercent'], saved['muted'])
            self.volume, self.muted = saved['volumePercent'], saved['muted']
        self.lock = threading.RLock()
        self.route = ''
        self.sequence = 0
        self.pending = None

    def reset(self):
        """Retire old-session controls/state, but preserve desired preferences."""
        with self.lock:
            self.control_path.unlink(missing_ok=True)
            self.state_path.unlink(missing_ok=True)
            self.route, self.sequence, self.pending = '', 0, None

    def configuration(self):
        return {'audio_control': str(self.control_path), 'audio_state': str(self.state_path),
                'audio_volume_percent': self.volume, 'audio_muted': self.muted}

    def snapshot(self):
        with self.lock:
            try:
                value = validate_state(read_private(self.state_path, self.owner), self.clock())
            except (OSError, ValueError, TypeError, UnicodeError):
                return unavailable()
            if not value['available']:
                # Temporary unavailability cannot reset a sequence frontier.
                # A genuinely different route or owned-child reset retires it.
                return unavailable()
            if value['route'] != self.route:
                self.route, self.sequence, self.pending = value['route'], 0, None
            accepted, applied = decimal(value['acceptedSequence']), decimal(value['appliedSequence'])
            self.sequence = max(self.sequence, accepted)
            if self.pending and applied >= self.pending['sequence']:
                self.pending = None
            pending = self.pending if self.pending and self.pending['sequence'] > accepted else None
            value['pending'] = bool(pending or accepted > applied)
            if value['pending']:
                value['pendingSequence'] = str(pending['sequence']) if pending else value['acceptedSequence']
                value['pendingVolumePercent'] = pending['volume'] if pending else value['requestedVolumePercent']
                value['pendingMuted'] = pending['muted'] if pending else value['requestedMuted']
            return value

    def request(self, route, volume, muted):
        with self.lock:
            level(volume, muted)
            fresh = self.snapshot()
            if not fresh['available'] or type(route) is not str or route != fresh['route']:
                raise ValueError('Audio route changed or is unavailable; refresh before controlling it')
            sequence = max(self.sequence, decimal(fresh['acceptedSequence'])) + 1
            if sequence > MAX_SEQUENCE:
                raise ValueError('Audio sequence exhausted; reconnect the audio route')
            request = {'version': 1, 'route': route, 'sequence': str(sequence),
                       'volumePercent': volume, 'muted': muted}
            # A durable desired preference is not an applied mixer value. Only
            # the receiver's confirmed appliedSequence advances the display.
            atomic_json(self.preference_path, {'version': 1, 'volumePercent': volume, 'muted': muted}, self.owner)
            self.volume, self.muted = volume, muted
            atomic_json(self.control_path, request, self.owner)
            self.sequence = sequence
            self.pending = {'sequence': sequence, 'volume': volume, 'muted': muted}
            return request
