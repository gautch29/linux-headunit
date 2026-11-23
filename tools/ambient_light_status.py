# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Admitted native Controller data and one software-policy request; no RGB math.

NativePublication supplies actual local-role/boot/nonce/credential admission and
original steady dates. This fixed codec never acquires a car/screen/provider,
starts a worker, or supplies actual lighting/command confirmation.
"""
from copy import deepcopy
import hashlib
import re
import struct

from ambient_light_model import MAX_AGE_NS, view_model
from prephone_consumer import DATE_MAX, GENERATION_MAX

SETTINGS_SIZE = 2368
PACKET_SIZE = 2560
ACTION_SIZE = 2432
SELECTIONS = ('automatic', 'screen', 'drive_mode', 'fixed')
NIGHTS = ('off', 'on', 'schedule', 'headlights')
RESULTS = ('none', 'software_updated', 'invalid_settings', 'stale_revision', 'expired_action')
FIELDS = {'version', 'enabled', 'selection', 'fixed_rgbw', 'day_brightness_permille',
          'night_brightness_permille', 'transition_ms', 'night_policy', 'mode_colours'}


def integer(value, maximum):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError('Invalid ambient software integer')
    return value


def colour(value):
    if type(value) is not list or len(value) != 4:
        raise ValueError('Ambient settings require four RGBW bytes')
    return bytes(integer(channel, 255) for channel in value)


def night(value):
    if type(value) is not str:
        raise ValueError('Invalid ambient night policy')
    if value in NIGHTS:
        return NIGHTS.index(value), 1200, 420
    match = re.fullmatch(r'schedule:([0-9]{2}):([0-9]{2})-([0-9]{2}):([0-9]{2})', value)
    if not match:
        raise ValueError('Invalid ambient night policy')
    hh, mm, eh, em = (int(part) for part in match.groups())
    if hh > 23 or eh > 23 or mm > 59 or em > 59 or (hh, mm) == (eh, em):
        raise ValueError('Invalid ambient schedule bounds')
    return 2, hh * 60 + mm, eh * 60 + em


def encode_settings(settings):
    """Shape/binary validation only; the C++ Controller validates policy again."""
    if (type(settings) is not dict or set(settings) != FIELDS or
            type(settings['version']) is not int or settings['version'] != 1 or
            type(settings['enabled']) is not bool or
            type(settings['selection']) is not str or settings['selection'] not in SELECTIONS or
            type(settings['mode_colours']) is not list or len(settings['mode_colours']) > 32):
        raise ValueError('Invalid complete ambient software settings')
    day = integer(settings['day_brightness_permille'], 1000)
    dim = integer(settings['night_brightness_permille'], day)
    transition = integer(settings['transition_ms'], 5000)
    kind, start, end = night(settings['night_policy'])
    raw = bytearray(SETTINGS_SIZE)
    raw[:4] = bytes((int(settings['enabled']), SELECTIONS.index(settings['selection']), kind,
                     len(settings['mode_colours'])))
    raw[4:8] = colour(settings['fixed_rgbw'])
    struct.pack_into('<5H', raw, 8, day, dim, transition, start, end)
    seen = set()
    for index, entry in enumerate(settings['mode_colours']):
        if (type(entry) is not dict or set(entry) != {'id', 'rgbw'} or
                type(entry['id']) is not str or
                not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,63}', entry['id']) or
                entry['id'] in seen):
            raise ValueError('Invalid/duplicate ambient mode mapping')
        seen.add(entry['id'])
        name = entry['id'].encode('ascii')
        at = 64 + index * 72
        raw[at] = len(name)
        raw[at + 4:at + 4 + len(name)] = name
        raw[at + 68:at + 72] = colour(entry['rgbw'])
    return bytes(raw)


def decode_settings(raw):
    if type(raw) is not bytes or len(raw) != SETTINGS_SIZE:
        raise ValueError('Invalid ambient settings packet bound')
    enabled, selection, kind, count = raw[:4]
    if enabled > 1 or selection >= len(SELECTIONS) or kind >= len(NIGHTS) or count > 32:
        raise ValueError('Invalid ambient settings enum')
    day, dim, transition, start, end = struct.unpack_from('<5H', raw, 8)
    policy = NIGHTS[kind]
    if kind == 2:
        policy = f'schedule:{start // 60:02}:{start % 60:02}-{end // 60:02}:{end % 60:02}'
    elif (start, end) != (1200, 420):
        raise ValueError('Invalid fixed ambient policy schedule')
    modes = []
    for index in range(count):
        at = 64 + index * 72
        size = raw[at]
        if not 1 <= size <= 64:
            raise ValueError('Invalid ambient mode length')
        try:
            name = raw[at + 4:at + 4 + size].decode('ascii')
        except UnicodeError as error:
            raise ValueError('Invalid ambient mode encoding') from error
        modes.append({'id': name, 'rgbw': list(raw[at + 68:at + 72])})
    settings = {'version': 1, 'enabled': bool(enabled), 'selection': SELECTIONS[selection],
                'fixed_rgbw': list(raw[4:8]), 'day_brightness_permille': day,
                'night_brightness_permille': dim, 'transition_ms': transition,
                'night_policy': policy, 'mode_colours': modes}
    if encode_settings(settings) != raw:
        raise ValueError('Noncanonical ambient settings/reserved bytes')
    return settings


def evidence(raw, at, sampled, expiry):
    generation, acquired, age, origin = struct.unpack_from('<4Q', raw, at)
    if not generation:
        if (acquired, age, origin) != (0, 0, 0):
            raise ValueError('Absent ambient observation carries dates')
        return None
    if not (0 < acquired <= sampled <= DATE_MAX and 0 < age <= 3_000_000_000 and
            sampled - acquired < age and expiry <= acquired + age and origin < 3):
        raise ValueError('Invalid original ambient acquisition/age')
    return {'generation': str(generation), 'acquired_ns': str(acquired),
            'max_age_ns': str(age), 'origin': ('measured', 'calculated', 'simulation')[origin]}


def decode(raw, nonce, now):
    if (type(raw) is not bytes or len(raw) != PACKET_SIZE or raw[:8] != b'HUAL001\0' or
            raw[8:40] != nonce or type(now) is not int or not 0 < now <= DATE_MAX):
        raise ValueError('Invalid ambient packet/nonce/clock')
    sequence, sampled, revision, action, result, present, derived, expiry = struct.unpack_from('<8Q', raw, 40)
    if (not sequence or not revision or not 0 < sampled <= now or result >= len(RESULTS) or
            bool(action) != bool(result) or present > 1):
        raise ValueError('Invalid ambient producer date/sequence/revision/result')
    settings = decode_settings(raw[192:])
    requested = None
    if present:
        source, is_night, night_source = raw[104:107]
        if (source not in (1, 2, 3) or is_night > 1 or night_source > 2 or
                raw[107:112] != b'\0' * 5 or raw[121:128] != b'\0' * 7 or
                raw[112] != 1 or not 0 < derived <= sampled < expiry <= derived + MAX_AGE_NS):
            raise ValueError('Invalid ambient target bounds/source')
        brightness, transition = struct.unpack_from('>2H', raw, 117)
        if brightness > 1000 or transition > 5000:
            raise ValueError('Invalid ambient target value')
        observed_colour = evidence(raw, 128, derived, expiry)
        headlights = evidence(raw, 160, derived, expiry)
        if ((source == 1) != (observed_colour is not None) or
                (night_source == 1) != (headlights is not None)):
            raise ValueError('Ambient target lacks original source evidence')
        requested = {'derived_ns': str(derived), 'valid_until_ns': str(expiry),
                     'value': {'rgbw': list(raw[113:117]), 'brightness_permille': brightness,
                               'transition_ms': transition},
                     'source': ('screen', 'drive_mode', 'fixed', 'disabled')[source],
                     'night': bool(is_night), 'night_source': ('policy', 'headlights', 'schedule')[night_source],
                     'colour_observation': observed_colour, 'headlights_observation': headlights}
    elif raw[80:192] != b'\0' * 112:
        raise ValueError('Unavailable ambient target has invented evidence')
    snapshot = {'version': 1, 'sampled_ns': str(sampled), 'settings': settings,
                'requested': requested, 'current': None, 'command': None}
    # Validate at the ORIGINAL date, even if arrival is too old for current use.
    if not view_model(snapshot, sampled)['available']:
        raise ValueError('Invalid C++ Controller view model')
    return sequence, sampled, revision, action, RESULTS[result], snapshot


def unavailable(configured=False, state='disabled'):
    return {'configured': configured, 'state': state, 'available': False,
            'provider_available': False, 'settings': None, 'requested': None,
            'current': None, 'command': None, 'revision': None, 'sampled_ns': None,
            'persistence': 'owner_lifetime', 'software_action': None}


class AmbientStatus:
    """Same manager thread/lock as NativePublication; one original model/action."""
    def __init__(self):
        self.sequence = self.sampled = self.revision = self.action = 0
        self.result = 'none'
        self.model = None
        self.retirement = None
        self.pending = None
        self.last_action = None
        self.next_action = 0
        self.nonce = None

    def token(self):
        return hashlib.sha256(self.nonce + struct.pack('<Q', self.revision)).hexdigest()

    def accept(self, raw, nonce, now):
        sequence, sampled, revision, action, result, model = decode(raw, nonce, now)
        if (self.retirement or sequence <= self.sequence or sampled <= self.sampled or
                self.nonce is not None and self.nonce != nonce or revision < self.revision or
                self.model is not None and (revision > self.revision + 1 or action < self.action or
                    revision == self.revision and model['settings'] != self.model['settings'] or
                    action == self.action and result != self.result)):
            raise ValueError('Ambient model replay/reversed/foreign policy')
        if self.pending:
            if now >= self.pending['deadline_ns']:
                self._finish('timed_out', 'completion_unknown')
            elif action == self.pending['identifier']:
                if result == 'software_updated':
                    if (revision != self.pending['revision'] + 1 or
                            encode_settings(model['settings']) != self.pending['settings_bytes']):
                        raise ValueError('Ambient policy acknowledgement conflicts with request')
                    self._finish('software_updated', None)
                elif result != 'none':
                    self._finish('failed', result)
            elif action > self.pending['identifier']:
                raise ValueError('Foreign ambient policy action')
        self.sequence, self.sampled, self.revision, self.action = sequence, sampled, revision, action
        self.result, self.model, self.nonce = result, model, nonce

    def prepare(self, data, now, nonce):
        if (type(data) is not dict or set(data) != {'action', 'revision', 'settings'} or
                data['action'] != 'ambient_light_settings'):
            raise ValueError('Use a revision-bound complete ambient software settings action')
        status = self.snapshot(now)
        if not status['available'] or nonce != self.nonce:
            raise ValueError('Ambient software owner/model unavailable')
        if self.pending:
            raise ValueError('Ambient software settings action already pending')
        if type(data['revision']) is not str or data['revision'] != status['revision']:
            raise ValueError('Ambient software settings revision is stale')
        encoded = encode_settings(data['settings'])
        if self.next_action == GENERATION_MAX:
            raise ValueError('Ambient action identifiers exhausted')
        identifier = self.next_action + 1
        raw = b'HUALS01\0' + nonce + struct.pack('<3Q', self.revision, identifier, now) + encoded
        return raw, {'identifier': identifier, 'revision': self.revision,
                     'settings_bytes': encoded, 'sent_ns': now, 'deadline_ns': now + MAX_AGE_NS}

    def sent(self, pending):
        self.next_action = pending['identifier']
        self.pending = pending
        self.last_action = {'identifier': str(pending['identifier']), 'status': 'pending',
                            'reason': None, 'physical_confirmed': False}

    def _finish(self, status, reason):
        self.last_action.update(status=status, reason=reason)
        self.pending = None

    def snapshot(self, now):
        if self.pending and (now is None or now >= self.pending['deadline_ns']):
            self._finish('timed_out', 'completion_unknown')
        result = unavailable(True, self.retirement or 'awaiting_model')
        if self.model:
            result['sampled_ns'] = str(self.sampled)
            if not self.retirement and now is not None:
                projected = view_model(self.model, now)
                result.update(projected)
                if projected['available']:
                    result.update(state='available', revision=self.token())
                else:
                    result['state'] = 'model_expired'
        result['software_action'] = deepcopy(self.last_action)
        return result

    def retire(self, state):
        self.retirement = state
        if self.pending:
            self._finish('failed', state)
