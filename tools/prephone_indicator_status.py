# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Fixed canonical lamp projection on the existing admitted native connection.

No acquisition, clock sampling, IO, actions or rendering. NativePublication owns
Linux role/nonce/clock/lifetime admission. This decoder has independent fixed
history and never promotes a vehicle publication date into a lamp acquisition.
"""
from copy import deepcopy
import struct
import threading

from prephone_consumer import DATE_MAX, AGE_MAX

PACKET_SIZE = 872
STATES = ('admitted', 'awaiting_source', 'undated_source', 'invalid_snapshot',
          'generation_mismatch', 'disconnected')
AVAILABILITY = ('available', 'missing', 'unavailable', 'stale', 'disconnected')
ORIGINS = ('measured', 'calculated', 'simulation')
VALUES = ('OFF', 'ON', 'FLASHING')
ROWS = (
    ('left_turn', 'LEFT', 'green'), ('right_turn', 'RIGHT', 'green'),
    ('hazard', 'HAZARD', 'amber'), ('high_beam', 'HIGH', 'blue'),
    ('low_beam', 'DIPPED', 'green'), ('position_lights', 'LIGHTS', 'green'),
    ('front_fog', 'FOG F', 'green'), ('rear_fog', 'FOG R', 'amber'),
    ('check_engine', 'ENGINE', 'amber'), ('oil_pressure', 'OIL', 'red'),
    ('battery_charge', 'CHARGE', 'red'), ('brake_fault', 'BRAKE', 'red'),
    ('parking_brake', 'PARK', 'red'), ('abs', 'ABS', 'amber'),
    ('stability_control', 'ESC', 'amber'), ('traction_control', 'TRACTION', 'amber'),
    ('airbag', 'AIRBAG', 'red'), ('seat_belt', 'BELT', 'red'),
    ('door_open', 'DOOR', 'red'), ('low_fuel', 'FUEL', 'amber'),
    ('coolant_warning', 'TEMP', 'red'), ('washer_low', 'WASHER', 'amber'),
    ('tyre_pressure', 'TYRES', 'amber'), ('glow_plug', 'GLOW', 'amber'),
)


def empty(state='awaiting_model'):
    return {'configured': True, 'version': 1, 'state': state, 'clockDomain': None,
            'sourceGeneration': None, 'sourceStartedNs': None, 'sourceSampledNs': None,
            'renderedNs': None, 'copiedNs': None, 'publicationSequence': None,
            'error': None,
            'rows': [dict(zip(('id', 'label', 'color'), definition),
                          availability='missing', value=None, acquiredNs=None,
                          maximumAgeNs=None, ageNs=None, origin=None) for definition in ROWS]}


def decode(raw, nonce, now):
    if (type(raw) is not bytes or len(raw) != PACKET_SIZE or
            raw[:8] != b'HUPI001\0' or type(nonce) is not bytes or len(nonce) != 32 or
            raw[8:40] != nonce or type(now) is not int or not 0 < now <= DATE_MAX):
        raise ValueError('Invalid indicator packet/nonce/clock')
    sequence, copied, state, generation, started, published = struct.unpack_from('<6Q', raw, 40)
    if (not sequence or not 0 < copied <= now or state >= len(STATES) or
            struct.unpack_from('<HHI', raw, 88) != (1, 24, PACKET_SIZE) or
            raw[96] > 1 or raw[97:104] != b'\0' * 7):
        raise ValueError('Invalid indicator version/length/date/sequence/reserved header')
    model = empty(STATES[state])
    model.update(clockDomain='host-steady' if raw[96] else None,
                 copiedNs=str(copied), publicationSequence=str(sequence))
    if state in (0, 5):
        if not generation or not 0 < started <= published <= copied or raw[96] != 1:
            raise ValueError('Invalid original indicator generation/clock/dates')
        model.update(sourceGeneration=str(generation), sourceStartedNs=str(started),
                     sourceSampledNs=str(published))
    elif (generation, started, published) != (0, 0, 0):
        raise ValueError('Unavailable indicator source has an invented identity')
    for index, row in enumerate(model['rows']):
        at = 104 + index * 32
        available, origin, value, identifier = raw[at:at + 4]
        acquired, age = struct.unpack_from('<QQ', raw, at + 16)
        if (identifier != index or raw[at + 4:at + 16] != b'\0' * 12 or
                available >= len(AVAILABILITY) or age > AGE_MAX):
            raise ValueError('Invalid indicator row/type/reserved bytes')
        if state not in (0, 5):
            if (available, origin, value, acquired, age) != (1, 255, 255, 0, 0):
                raise ValueError('Unavailable indicator source carries observations')
            continue
        status = AVAILABILITY[available]
        if (not age or acquired > published or acquired and acquired < started or
                not acquired and (origin != 255 or status not in ('missing', 'disconnected')) or
                acquired and (origin >= len(ORIGINS) or status == 'missing') or
                status == 'available' and (not acquired or value >= len(VALUES) or copied - acquired >= age) or
                status != 'available' and value != 255 or
                status == 'stale' and (not acquired or copied - acquired < age) or
                state == 5 and status != 'disconnected'):
            raise ValueError('Invalid canonical indicator availability/value/acquisition')
        row.update(availability=status, value=VALUES[value] if status == 'available' else None,
                   acquiredNs=str(acquired) if acquired else None, maximumAgeNs=str(age),
                   origin=ORIGINS[origin] if acquired else None)
    return sequence, copied, model


class IndicatorStatus:
    """Independent bounded packet/history admission; no generic model framework."""
    def __init__(self):
        self.owner = threading.get_ident()
        self.sequence = self.copied = self.last_now = 0
        self.generation = self.started = self.published = self.source_floor = 0
        self.model = empty()
        self.ages = None
        self.marks = [None] * 24
        self.retirement = None
        self.source_retired = False

    def _time(self, now):
        if (threading.get_ident() != self.owner or type(now) is not int or
                not 0 < now <= DATE_MAX or now < self.last_now):
            raise ValueError('Indicator owner/clock conflict')

    def accept(self, raw, nonce, now, source=None):
        self._time(now)
        sequence, copied, model = decode(raw, nonce, now)
        generation = int(model['sourceGeneration'] or 0)
        started = int(model['sourceStartedNs'] or 0)
        published = int(model['sourceSampledNs'] or 0)
        if (self.retirement or sequence <= self.sequence or copied < self.copied or
                generation < max(self.generation, self.source_floor)):
            raise ValueError('Indicator replay/retired source/reversed copy')
        if source is not None and (
                sequence != source.sequence or copied != source.copied or
                any(model[key] != source.model[key] for key in
                    ('state', 'sourceGeneration', 'sourceStartedNs', 'sourceSampledNs'))):
            raise ValueError('Indicator record is not the same original source copy')
        marks = list(self.marks)
        ages = self.ages
        if generation:
            if generation == self.generation:
                if self.source_retired or started != self.started or published < self.published:
                    raise ValueError('Indicator source retired/reversed/foreign')
            else:
                if started < self.published:
                    raise ValueError('Indicator successor precedes original publication')
                marks = [None] * 24
            candidate_ages = tuple(row['maximumAgeNs'] for row in model['rows'])
            if ages is not None and candidate_ages != ages:
                raise ValueError('Indicator immutable age policy changed')
            ages = candidate_ages
            for index, row in enumerate(model['rows']):
                acquired = row['acquiredNs']
                previous = marks[index]
                state = row['availability']
                if previous is not None:
                    if acquired is None or int(acquired) < int(previous['acquiredNs']):
                        raise ValueError('Indicator acquisition reversed/dropped')
                    if acquired == previous['acquiredNs'] and (
                            row['origin'] != previous['origin'] or
                            state == 'available' and (previous['availability'] != 'available' or
                                row['value'] != previous['value']) or
                            state == 'unavailable' and previous['observationState'] not in ('unavailable', 'disconnected') or
                            state == 'stale' and previous['observationState'] == 'unavailable'):
                        raise ValueError('Conflicting/revived indicator acquisition')
                if acquired is not None:
                    mark = dict(row)
                    mark['observationState'] = (previous['observationState']
                        if previous is not None and acquired == previous['acquiredNs'] and
                            state in ('stale', 'disconnected') else state)
                    if state == 'available' and now - int(acquired) >= int(row['maximumAgeNs']):
                        row.update(availability='stale', value=None)
                        mark.update(availability='stale', value=None)
                    marks[index] = mark
        # All binary, source, policy and 24 row checks complete before admission.
        self.sequence, self.copied, self.model = sequence, copied, model
        self.generation, self.started, self.published = generation, started, published
        self.ages, self.marks = ages, marks
        self.source_retired = model['state'] == 'disconnected'
        self.last_now = now

    def snapshot(self, now, source=None):
        if now is not None:
            self._time(now)
            self.last_now = now
        result = deepcopy(self.model)
        result['renderedNs'] = str(now) if now is not None else None
        mismatch = False
        if source is not None:
            self.source_floor = max(self.source_floor, int(source.get('sourceGeneration') or 0))
            mismatch = any(result[key] != source.get(key) for key in
                           ('sourceGeneration', 'sourceStartedNs'))
        if self.retirement or mismatch or now is None:
            result['state'] = self.retirement or 'generation_mismatch'
            for row in result['rows']:
                row.update(availability='disconnected', value=None)
        for index, row in enumerate(result['rows']):
            if row['acquiredNs'] is not None and now is not None:
                row['ageNs'] = str(now - int(row['acquiredNs']))
                if row['availability'] == 'available' and int(row['ageNs']) >= int(row['maximumAgeNs']):
                    row.update(availability='stale', value=None)
                    self.model['rows'][index].update(availability='stale', value=None)
                    self.marks[index].update(availability='stale', value=None)
        result['error'] = self.retirement if self.retirement in ('protocol_failure', 'clock_failure') else None
        return result

    def retire(self, state):
        self.retirement = state
