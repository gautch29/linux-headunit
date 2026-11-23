# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Bounded now-playing snapshots for native displays and future iAP adapters."""
import base64
import hashlib
import math
from pathlib import Path
import struct
import time
from phone_state import atomic_json

COMMANDS = {'play', 'pause', 'next', 'previous', 'volume_up', 'volume_down'}
TEXT = {'title', 'artist', 'album', 'genre', 'player', 'track_id'}


IAP_FIELDS = ('track_id', 'title', 'artist', 'album', 'genre', 'duration_ms',
              'position_ms', 'status', 'player', 'artwork_transfer_id', 'artwork')


def _uint64(value, nullable=False):
    if value is None and nullable:
        return None
    if (not isinstance(value, str) or not value.isascii() or not value.isdecimal() or
            len(value) > 20 or value != str(int(value)) or int(value) > 0xffffffffffffffff):
        raise ValueError('Invalid iAP observation number')
    return value


def validate_iap_observation(value):
    """Exact bounded copied producer schema; no clocks or historical fill-ins."""
    if value is None:
        return None
    keys = {'version', 'clock', 'launchNonce', 'engineGeneration', 'connectionGeneration',
            'messageAcceptedMonotonicNs', 'artworkTransferId', 'fields'}
    if (not isinstance(value, dict) or set(value) != keys or
            type(value['version']) is not int or value['version'] != 1 or
            value['clock'] != 'CLOCK_MONOTONIC' or
            not isinstance(value['launchNonce'], str) or len(value['launchNonce']) != 32 or
            any(c not in '0123456789abcdef' for c in value['launchNonce']) or
            not isinstance(value['fields'], dict) or set(value['fields']) != set(IAP_FIELDS)):
        raise ValueError('Invalid iAP observation scope')
    transfer = value['artworkTransferId']
    if transfer is not None and (type(transfer) is not int or not 0 <= transfer <= 255):
        raise ValueError('Invalid iAP observation transfer')
    result = dict(value)
    for key in ('engineGeneration', 'connectionGeneration'):
        result[key] = _uint64(value[key])
        if result[key] == '0':
            raise ValueError('Unavailable iAP observation identity')
    result['messageAcceptedMonotonicNs'] = _uint64(value['messageAcceptedMonotonicNs'], True)
    result['fields'] = {key: _uint64(value['fields'][key], True) for key in IAP_FIELDS}
    return result


def iap_observation_model(media, live, now_ns):
    """Host acceptance age only. Session/track-bound fields have no invented TTL."""
    observation = media.get('iap_observation') if live and media.get('source') == 'iap' else None
    def age(stamp):
        return str(now_ns - int(stamp)) if stamp is not None and int(stamp) <= now_ns else None
    fields = observation['fields'] if observation else {}
    return {'available': observation is not None, 'clock': 'CLOCK_MONOTONIC',
            'scope': ({key: observation[key] for key in
                       ('launchNonce', 'engineGeneration', 'connectionGeneration', 'artworkTransferId')} |
                       {'trackId': media.get('track_id') or None}
                      if observation else None),
            'messageAcceptedMonotonicNs': observation['messageAcceptedMonotonicNs'] if observation else None,
            'messageObservedAgeNs': age(observation['messageAcceptedMonotonicNs']) if observation else None,
            'fields': {key: {'acceptedMonotonicNs': fields.get(key),
                             'observedAgeNs': age(fields.get(key))} for key in IAP_FIELDS},
            'phoneAcquiredMonotonicNs': None, 'freshnessGuaranteed': False,
            'playbackConfirmed': False}


def media_fresh(snapshot, now_ms):
    """Manager publication age only; never an iAP report acquisition date."""
    stamp = (snapshot or {}).get('updated_monotonic_ms')
    return type(stamp) is int and 0 <= now_ms - stamp <= 3000


class MediaObservations:
    """Bounded projection of existing admitted copies, with no acquisition/I/O.

    One PCM frontier and one action per manager generation. CLOCK_MONOTONIC
    output/action dates and Python monotonic media-publication dates are separate.
    """
    def __init__(self):
        self.serial = 0
        self.reset()

    def reset(self):
        self.serial += 1
        self.action = None
        self.frontier = None
        self.generation = None

    def begin(self, generation, command, transport, now_ns):
        self.serial += 1
        token = self.serial
        self.action = {'id': token, 'generation': generation, 'command': command,
                       'transport': transport, 'requestedMonotonicNs': str(now_ns),
                       'state': 'pending', 'completion': 'unknown', 'confirmed': False,
                       'resultMonotonicNs': None, 'error': None}
        return token

    def finish(self, token, state, now_ns):
        # A retired or replaced request cannot attach its late result to the
        # successor, including a callback that reenters the manager's RLock.
        if self.action is None or self.action['id'] != token:
            return
        self.action.update(state=state, resultMonotonicNs=str(now_ns),
                           error='dispatch-failed' if state == 'error' else None)

    def snapshot(self, generation, live, media, audio, now_ms, now_ns):
        if self.generation != generation or not live:
            self.frontier = None
            if self.action and (self.action['generation'] != generation or not live):
                self.serial += 1
                self.action = None
            self.generation = generation
        media = media or {}
        fresh = media_fresh(media, now_ms)
        report = {'available': bool(live and media.get('available') and fresh),
                  'fresh': bool(live and fresh), 'source': media.get('source'),
                  'reportedState': media.get('status', 'unknown'),
                  'publishedMonotonicMs': media.get('updated_monotonic_ms'),
                  'reportAcquiredMonotonicNs': None,
                  'iapHostObservation': iap_observation_model(media, live and fresh, now_ns)}
        pcm = {'state': 'unavailable', 'route': None, 'sampledMonotonicNs': None,
               'acceptedBlocks': None, 'previousSampledMonotonicNs': None,
               'acceptedBlocksDelta': None}
        output = audio.get('output') if live and audio.get('available') else None
        if not output:
            self.frontier = None
        if output:
            # AudioControl already validates canonical uint64 values and the
            # wire clock. Preserve those strings exactly in the projection.
            route = audio['route']
            sampled = int(output['sampledMonotonicNs'])
            counts = tuple(int(output[key]) for key in ('acceptedBlocks', 'discontinuities', 'underruns'))
            identity = (sampled, counts, output['starved'], output.get('timing'))
            pcm.update(route=route, sampledMonotonicNs=output['sampledMonotonicNs'],
                       acceptedBlocks=output['acceptedBlocks'])
            if self.frontier and self.frontier[0] != route:
                self.frontier = None
            previous = self.frontier
            if not output['fresh'] or not 0 <= now_ns - sampled <= 1_000_000_000:
                pcm['state'] = 'stale'
            elif previous and (sampled < previous[1][0] or
                               any(a < b for a, b in zip(counts, previous[1][1])) or
                               sampled == previous[1][0] and identity != previous[1]):
                pcm['state'] = 'invalid-observation'
            elif previous and sampled == previous[1][0]:
                pcm['state'] = 'unchanged-sample'
            else:
                pcm['state'] = 'baseline'
                if previous and 0 <= now_ns - previous[1][0] <= 1_000_000_000:
                    delta = counts[0] - previous[1][1][0]
                    pcm.update(state='progressing' if delta else 'no-advance',
                               previousSampledMonotonicNs=str(previous[1][0]),
                               acceptedBlocksDelta=str(delta))
                # Copy the small timing object; caller mutation cannot alter
                # the retained frontier or make a changed equal-date row valid.
                self.frontier = (route, (sampled, counts, output['starved'],
                                         dict(output['timing']) if 'timing' in output else None))
        return {'version': 1, 'generation': generation, 'live': live,
                'transportReport': report, 'pcm': pcm,
                'action': dict(self.action) if self.action else None,
                'playbackConfirmed': False}


def validate_media(data):
    if not isinstance(data, dict) or set(data) - (TEXT | {'available', 'source', 'status', 'duration_ms', 'position_ms',
                                                          'track_number', 'track_count', 'volume', 'controls', 'artwork', 'iap_observation'}):
        raise ValueError('Invalid media snapshot')
    result = {'available': data.get('available', False), 'controls': []}
    if type(result['available']) is not bool:
        raise ValueError('Invalid media availability')
    if not result['available']:
        return result
    for key in TEXT:
        value = data.get(key, '')
        if not isinstance(value, str) or len(value.encode('utf-8')) > 2048 or '\0' in value:
            raise ValueError('Invalid media text')
        result[key] = ''.join(c for c in value if c >= ' ')
    if data.get('status', 'unknown') not in ('playing', 'paused', 'stopped', 'forward-seek', 'reverse-seek', 'error', 'unknown'):
        raise ValueError('Invalid playback state')
    result['status'] = data.get('status', 'unknown')
    result['source'] = str(data.get('source', 'adapter'))[:64]
    for key in ('duration_ms', 'position_ms', 'track_number', 'track_count'):
        value = data.get(key)
        if value is not None and (type(value) is not int or not 0 <= value <= 0xffffffff):
            raise ValueError('Invalid media position or count')
        if value is not None:
            result[key] = value
    volume = data.get('volume')
    if volume is not None:
        if type(volume) not in (int, float) or not math.isfinite(volume) or not 0 <= volume <= 1:
            raise ValueError('Invalid media volume')
        result['volume'] = volume
    controls = data.get('controls', [])
    if not isinstance(controls, list) or len(controls) > len(COMMANDS) or any(c not in COMMANDS for c in controls):
        raise ValueError('Invalid media capabilities')
    result['controls'] = sorted(set(controls))
    if data.get('iap_observation') is not None:
        result['iap_observation'] = validate_iap_observation(data['iap_observation'])
    return result


class MediaState:
    def __init__(self, path, owner=None):
        self.path, self.owner = Path(path), owner
        self.cover = self.path.with_suffix('.cover.png')
        self.last = None

    def publish(self, snapshot, artwork=None, keep_artwork=False):
        value = validate_media(snapshot)
        if (keep_artwork and artwork is None and value['available'] and self.last and
                self.last.get('source') == value.get('source') == 'iap' and
                self.last.get('track_id') == value.get('track_id') and
                self.last.get('artwork')):
            value['artwork'] = self.last['artwork']
            value['artwork_id'] = self.last['artwork_id']
        if artwork is not None:
            if not value['available'] or not isinstance(artwork, str) or len(artwork) > 1400000:
                raise ValueError('Invalid artwork')
            raw = base64.b64decode(artwork, validate=True)
            png = raw[:8] == b'\x89PNG\r\n\x1a\n' and raw[12:16] == b'IHDR'
            jpeg = raw[:2] == b'\xff\xd8' and raw[-2:] == b'\xff\xd9'
            if len(raw) < 24 or len(raw) > 1048576 or not (png or jpeg):
                raise ValueError('Artwork must be a bounded PNG or JPEG')
            if png:
                width, height = struct.unpack('>II', raw[16:24])
                if not 1 <= width <= 1024 or not 1 <= height <= 1024:
                    raise ValueError('Artwork dimensions exceed 1024 pixels')
            # Normalize by decoding before the display sees it, when available.
            from gi.repository import GdkPixbuf
            try:
                loader = GdkPixbuf.PixbufLoader.new_with_type('png' if png else 'jpeg')
                dimensions = []
                def size_prepared(active_loader, width, height):
                    dimensions.append((width, height))
                    if not 1 <= width <= 1024 or not 1 <= height <= 1024:
                        active_loader.set_size(1, 1)
                loader.connect('size-prepared', size_prepared)
                loader.write(raw)
                loader.close()
                if not dimensions or not 1 <= dimensions[0][0] <= 1024 or not 1 <= dimensions[0][1] <= 1024:
                    raise ValueError('Artwork dimensions exceed 1024 pixels')
                pixbuf = loader.get_pixbuf()
                if not 1 <= pixbuf.get_width() <= 1024 or not 1 <= pixbuf.get_height() <= 1024:
                    raise ValueError('Artwork dimensions exceed 1024 pixels')
                ok, normalized = pixbuf.save_to_bufferv('png', [], [])
            except ValueError:
                raise
            except Exception as error:
                raise ValueError('Artwork could not be decoded') from error
            if not ok:
                raise ValueError('Artwork could not be decoded')
            import os, tempfile
            fd, name = tempfile.mkstemp(dir=self.path.parent, prefix='.cover-')
            try:
                with os.fdopen(fd, 'wb') as f:
                    os.fchmod(f.fileno(), 0o600)
                    if self.owner:
                        os.fchown(f.fileno(), *self.owner)
                    f.write(normalized)
                os.replace(name, self.cover)
            finally:
                if os.path.exists(name):
                    os.unlink(name)
            value['artwork'] = self.cover.name
            value['artwork_id'] = hashlib.sha256(normalized).hexdigest()
        # Complete snapshots deliberately clear old art on track/source change.
        value['updated_monotonic_ms'] = int(time.monotonic() * 1000)
        atomic_json(self.path, value, self.owner)
        self.last = value
        return value
