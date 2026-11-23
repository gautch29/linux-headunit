# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Read the current owned receiver's dated, generation-bound presentation state."""
import re
import time
from pathlib import Path
from private_state import SnapshotChanged, read_private

FIELDS = {'version', 'nonce', 'sequence', 'connectionGeneration',
          'updatedMonotonicNs', 'state', 'returnReady'}
STATES = {'disconnected', 'starting', 'ready', 'home', 'fault', 'stopped'}
MAXIMUM_AGE_NS = 1_500_000_000


def clock_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def decimal(value, positive=False):
    if (type(value) is not str or not re.fullmatch(r'0|[1-9][0-9]{0,19}', value) or
            int(value) > (1 << 64) - 1 or (positive and value == '0')):
        raise ValueError('Invalid receiver-state unsigned decimal')
    return int(value)


class CarplayReadiness:
    def __init__(self, folder, owner, clock=clock_ns):
        self.path = Path(folder) / 'carplay-status.json'
        self.owner, self.clock = owner, clock
        self.reset()

    def reset(self, nonce=None):
        if nonce is not None and (type(nonce) is not str or not re.fullmatch(r'[0-9a-f]{32}', nonce)):
            raise ValueError('Receiver-state nonce must identify one owned launch')
        self.nonce = nonce
        self.last = None
        self.negotiated_mode = None
        self.phone_home_requests = 0

    def snapshot(self, child_alive):
        self.negotiated_mode = None
        self.phone_home_requests = 0
        if not child_alive or self.nonce is None:
            return {'state': 'disconnected', 'returnReady': False}
        unavailable = {'state': 'starting', 'returnReady': False}
        try:
            # Atomic publication may unlink an opened old inode. Retry once
            # only for that observed snapshot race, with all admission checks
            # repeated. Never use a cached Ready value or refresh its date.
            for attempt in range(2):
                try:
                    value = read_private(self.path, self.owner, 512)
                    break
                except SnapshotChanged:
                    if attempt == 1:
                        raise
            if (type(value) is not dict or type(value.get('version')) is not int or
                    value['version'] not in (1, 2, 3) or
                    set(value) != (FIELDS if value['version'] == 1 else
                                   FIELDS | {'negotiatedMode'} if value['version'] == 2 else
                                   FIELDS | {'negotiatedMode', 'phoneHomeRequests'}) or
                    value['nonce'] != self.nonce or type(value['state']) is not str or
                    value['state'] not in STATES or type(value['returnReady']) is not bool):
                raise ValueError('Invalid current receiver-state schema')
            mode = value.get('negotiatedMode')
            if (mode is not None and (type(mode) is not str or mode not in ('standard', 'ultra') or
                    value['state'] in ('disconnected', 'stopped'))):
                raise ValueError('Invalid negotiated receiver mode')
            sequence = decimal(value['sequence'], True)
            generation = decimal(value['connectionGeneration'])
            if mode is not None and generation == 0:
                raise ValueError('Receiver mode requires an established connection')
            updated = decimal(value['updatedMonotonicNs'], True)
            if not 0 <= self.clock() - updated <= MAXIMUM_AGE_NS:
                raise ValueError('Receiver state is stale or from the future')
            if value['returnReady'] and (value['state'] not in ('ready', 'home') or generation == 0):
                raise ValueError('Receiver state claims readiness without an established connection')
            if value['state'] in ('ready', 'home') and not value['returnReady']:
                raise ValueError('Receiver presentation state disagrees with its readiness')
            if self.last is not None:
                old_sequence = decimal(self.last['sequence'])
                if (sequence < old_sequence or
                        (sequence == old_sequence and value != self.last) or
                        (sequence > old_sequence and
                         (updated < decimal(self.last['updatedMonotonicNs']) or
                          generation < decimal(self.last['connectionGeneration'])))):
                    raise ValueError('Receiver state was replayed or changed within one publication')
            # Phone car-icon releases of mainScreen during this connection.
            home_requests = decimal(value['phoneHomeRequests']) if value['version'] == 3 else 0
            self.last = dict(value)
            self.negotiated_mode = mode
            self.phone_home_requests = home_requests
            return {'state': value['state'], 'returnReady': value['returnReady']}
        except (OSError, ValueError, TypeError):
            return unavailable
