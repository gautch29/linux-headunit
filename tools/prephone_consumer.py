# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Fixed eight-row consumer seam for an admitted C++ vehicle summary.

No acquisition, IO, clock sampling, worker, or manager registration. The caller
must establish the actual current vehicle publisher/generation on the same host
and serialize the C++ projection off control/VM/GPU callbacks. JSON is data, not
publisher authority. This module does not decode/admit HVO or fitted equipment.
"""
from copy import deepcopy
import json
import math
import threading

LIMIT = 8192
DATE_MAX = 2**63 - 1
GENERATION_MAX = 2**64 - 1
AGE_MAX = 86_400_000_000_000
# Exact first-party version-1 projection catalogue; bounds match vehicle.hpp.
ROWS = (
    ('fuel_percent', 'energy', 'Fuel', '%', 0, 100),
    ('range_km', 'energy', 'Range', 'KM', 0, 3000),
    ('battery_voltage', 'energy', 'Battery voltage', 'V', 0, 60),
    ('ambient_c', 'temperatures', 'Outside temperature', 'C', -60, 80),
    ('coolant_c', 'temperatures', 'Coolant temperature', 'C', -50, 180),
    ('oil_c', 'temperatures', 'Oil temperature', 'C', -50, 200),
    ('oil_pressure_kpa', 'engine', 'Oil pressure', 'KPA', 0, 1500),
    ('odometer_km', 'distance', 'Odometer', 'KM', 0, 9999999),
)
FIELDS = {'version', 'state', 'clockDomain', 'sourceGeneration',
          'sourceStartedNs', 'sourceSampledNs', 'rows'}
ROW_FIELDS = {'id', 'group', 'label', 'unit', 'availability', 'value',
              'acquiredNs', 'maximumAgeNs', 'origin'}


def positive(value, maximum=DATE_MAX):
    if type(value) is not int or not 0 < value <= maximum:
        raise ValueError('Invalid vehicle consumer integer')
    return value


def decimal(value, maximum=DATE_MAX):
    if (type(value) is not str or not 1 <= len(value) <= 20 or
            not value.isascii() or not value.isdecimal() or value[0] == '0'):
        raise ValueError('Invalid exact vehicle decimal')
    return positive(int(value), maximum)


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate vehicle summary field')
        result[key] = value
    return result


def nonfinite(_):
    raise ValueError('Nonfinite vehicle summary')


def json_number(text):
    value = float(text)
    coefficient = text.lower().split('e', 1)[0]
    if not math.isfinite(value) or value == 0 and any(c in '123456789' for c in coefficient):
        raise ValueError('Unrepresentable vehicle summary number')
    return value


def decode(raw, now):
    """Parse only the qualified projection, never an HVO or generic UI model."""
    positive(now)
    if type(raw) is not bytes or not 0 < len(raw) <= LIMIT:
        raise ValueError('Invalid vehicle summary byte bound')
    try:
        model = json.loads(raw.decode('ascii'), object_pairs_hook=unique,
                           parse_constant=nonfinite, parse_float=json_number)
    except (UnicodeError, RecursionError) as error:
        raise ValueError('Invalid vehicle summary encoding/nesting') from error
    if (type(model) is not dict or set(model) != FIELDS or
            type(model['version']) is not int or model['version'] != 1 or
            model['clockDomain'] != 'host-steady' or
            model['state'] not in ('admitted', 'disconnected') or
            type(model['rows']) is not list or len(model['rows']) != len(ROWS)):
        raise ValueError('Invalid admitted vehicle summary schema')
    generation = decimal(model['sourceGeneration'], GENERATION_MAX)
    started = decimal(model['sourceStartedNs'])
    published = decimal(model['sourceSampledNs'])
    if not started <= published <= now:
        raise ValueError('Invalid vehicle source publication date')
    for row, definition in zip(model['rows'], ROWS):
        if (type(row) is not dict or set(row) != ROW_FIELDS or
                tuple(row.get(key) for key in ('id', 'group', 'label', 'unit')) != definition[:4] or
                row['availability'] not in ('available', 'missing', 'unavailable', 'stale', 'disconnected')):
            raise ValueError('Invalid fixed vehicle row')
        age = decimal(row['maximumAgeNs'], AGE_MAX)
        acquired = None if row['acquiredNs'] is None else decimal(row['acquiredNs'])
        state, value = row['availability'], row['value']
        if acquired is None:
            if row['origin'] is not None or state not in ('missing', 'disconnected'):
                raise ValueError('Undated vehicle row has observed state')
        elif not started <= acquired <= published or row['origin'] not in ('measured', 'calculated', 'simulation'):
            raise ValueError('Invalid vehicle acquisition/origin')
        if (state == 'missing' and acquired is not None or
                state == 'available' and (acquired is None or published - acquired >= age or
                    type(value) not in (int, float) or not definition[4] <= value <= definition[5] or
                    not math.isfinite(value)) or
                state != 'available' and value is not None or
                state == 'stale' and (acquired is None or now - acquired < age) or
                model['state'] == 'disconnected' and state != 'disconnected'):
            raise ValueError('Invalid vehicle value/availability')
    return model, generation, started, published


def empty(state='awaiting_source'):
    return {'version': 1, 'state': state, 'clockDomain': 'host-steady',
            'sourceGeneration': None, 'sourceStartedNs': None, 'sourceSampledNs': None,
            'rows': [dict(zip(('id', 'group', 'label', 'unit'), row[:4]),
                          availability='missing', value=None, acquiredNs=None,
                          maximumAgeNs=None, origin=None) for row in ROWS]}


class PrephoneConsumer:
    """Single existing-owner seam, fixed memory, explicit source lifetime.

    begin() accepts a caller-established opaque publisher token, never one read
    from JSON. Generations strictly increase for this consumer's lifetime. All
    dates must already be in this host's C++ steady domain. Different boots or
    different sources need a fresh consumer and fresh authority; no offset guess.
    """
    def __init__(self):
        self.owner = threading.get_ident()
        self.last_now = 0
        self.generation = 0
        self.started = 0
        self.published = 0
        self.publisher = None
        self.retired = True
        self.model = empty()
        self.ages = None
        self.watermarks = [None] * len(ROWS)

    def _time(self, now):
        positive(now)
        if threading.get_ident() != self.owner or now < self.last_now:
            raise ValueError('Vehicle consumer owner/clock conflict')

    def _authority(self, publisher, generation):
        if (type(publisher) is not str or publisher != self.publisher or
                type(generation) is not int or generation != self.generation or self.retired):
            raise ValueError('Vehicle publisher is not current')

    def begin(self, publisher, generation, started, now, clock_domain):
        self._time(now)
        positive(generation, GENERATION_MAX)
        positive(started)
        if (type(publisher) is not str or not 1 <= len(publisher) <= 128 or
                not publisher.isascii() or any(not 33 <= ord(c) <= 126 for c in publisher) or
                clock_domain != 'host-steady' or generation <= self.generation or
                not self.published <= started <= now):
            raise ValueError('Invalid vehicle publisher admission')
        self.publisher = publisher
        self.generation = generation
        self.started = started
        self.retired = False
        self.model = empty()
        self.watermarks = [None] * len(ROWS)
        self.last_now = now

    def accept(self, raw, publisher, now):
        self._time(now)
        model, generation, started, published = decode(raw, now)
        self._authority(publisher, generation)
        if started != self.started or published < self.published:
            raise ValueError('Vehicle summary publication reversed/foreign')
        ages = tuple(row['maximumAgeNs'] for row in model['rows'])
        if self.ages is not None and ages != self.ages:
            raise ValueError('Vehicle row age policy changed')
        marks = list(self.watermarks)
        for index, row in enumerate(model['rows']):
            acquired = row['acquiredNs']
            previous = marks[index]
            if previous is not None:
                if acquired is None or decimal(acquired) < decimal(previous['acquiredNs']):
                    raise ValueError('Vehicle acquisition reversed/dropped')
                if acquired == previous['acquiredNs']:
                    state = row['availability']
                    if (row['origin'] != previous['origin'] or
                            state == 'available' and (previous['availability'] != 'available' or
                                row['value'] != previous['value'] or
                                now - decimal(acquired) >= decimal(row['maximumAgeNs'])) or
                            state == 'unavailable' and previous['observationState'] not in ('unavailable', 'disconnected') or
                            state == 'stale' and previous['observationState'] == 'unavailable'):
                        raise ValueError('Conflicting/revived vehicle acquisition')
            if acquired is not None:
                mark = dict(row)
                state = row['availability']
                mark['observationState'] = (previous['observationState']
                    if previous is not None and acquired == previous['acquiredNs'] and
                        state in ('stale', 'disconnected') else state)
                marks[index] = mark
            if row['availability'] == 'available' and now - decimal(acquired) >= decimal(row['maximumAgeNs']):
                row.update(availability='stale', value=None)
                marks[index]['availability'] = 'stale'
                marks[index]['value'] = None
        # Parsing, all row transitions and policy checks finish before any commit.
        self.model = model
        self.ages = ages
        self.watermarks = marks
        self.published = published
        self.retired = model['state'] == 'disconnected'
        self.last_now = now

    def snapshot(self, now):
        self._time(now)
        result = deepcopy(self.model)
        for index, row in enumerate(result['rows']):
            if row['availability'] == 'available' and now - decimal(row['acquiredNs']) >= decimal(row['maximumAgeNs']):
                row.update(availability='stale', value=None)
                self.watermarks[index]['availability'] = 'stale'
                self.watermarks[index]['value'] = None
        self.model = deepcopy(result)
        self.last_now = now
        return result

    def retire(self, publisher, generation, now):
        self._time(now)
        self._authority(publisher, generation)
        self.retired = True
        self.model['state'] = 'disconnected'
        for index, row in enumerate(self.model['rows']):
            row.update(availability='disconnected', value=None)
            if row['acquiredNs'] is not None:
                self.watermarks[index]['availability'] = 'disconnected'
                self.watermarks[index]['value'] = None
        self.last_now = now
