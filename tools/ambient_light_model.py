# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Pure data adapter for ambient lighting. No GTK, manager lifecycle or I/O.

Inputs come from the validated live owner's cluster_layout.snapshot and the
C++ ambient snapshot_json. An ID-to-RGBW mapping must be qualified for that pack;
we do not guess RGB from palette names or pending/remembered selections.
"""
import copy
import re

MAX_AGE_NS = 1_000_000_000


def _integer(value, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError('Ambient integer outside bounds')
    return value


def _decimal(value):
    if type(value) is not str or not re.fullmatch(r'[1-9][0-9]{0,19}', value):
        raise ValueError('Invalid ambient monotonic date/generation')
    return int(value)


def _colour(value):
    if type(value) is not list or len(value) != 4:
        raise ValueError('Ambient colour requires RGBW')
    return [_integer(channel, 0, 255) for channel in value]


def _value(value, actual=False):
    if type(value) is not dict or set(value) != {'rgbw', 'brightness_permille', 'transition_ms'}:
        raise ValueError('Invalid ambient value')
    _colour(value['rgbw'])
    _integer(value['brightness_permille'], 0, 1000)
    _integer(value['transition_ms'], 0, 0 if actual else 5000)
    return copy.deepcopy(value)


def screen_accent(state, palette_colours, publisher, now_ns):
    """Return only a fresh settled current-publisher observed accent, or None.

    state must be supplied by cluster_layout.snapshot(path, owner, publisher).
    The returned acquisition is its ORIGINAL render observation date. Passing a
    manager's requested palette or its persisted confirmed preference is invalid.
    A new receiver nonce must create a new C++ screen-observation consumer epoch.
    """
    if not state or type(palette_colours) is not dict or len(palette_colours) > 64:
        return None
    try:
        if (type(now_ns) is not int or (state['nonce'], state['connectionGeneration']) != publisher or
                state['ready'] is not True or state['transitioning'] is not False or
                state.get('idle') is not True or state.get('pending', '') or
                state.get('confirmedActive') != state['active']):
            return None
        acquired = _decimal(state['updatedMonotonicNs'])
        generation = _decimal(state['connectionGeneration'])
        if not 0 <= now_ns - acquired < MAX_AGE_NS:
            return None
        rgbw = _colour(palette_colours[state['activePalette']])
        return {'rgbw': rgbw, 'generation': str(generation), 'acquired_ns': str(acquired),
                'max_age_ns': str(MAX_AGE_NS), 'origin': 'calculated',
                'publisher_nonce': state['nonce']}
    except (ValueError, KeyError, TypeError):
        return None


def view_model(snapshot, now_ns):
    """Claude's UI model: requested and confirmed current are separate optionals.

    Consumers date the reply at dispatch as usual and call this on each render.
    Expired/unavailable state is None, never black or the last requested colour.
    """
    unavailable = {'available': False, 'settings': None, 'requested': None,
                   'current': None, 'command': None}
    try:
        if type(snapshot) is not dict or type(snapshot.get('version')) is not int or snapshot['version'] != 1:
            return unavailable
        sampled = _decimal(snapshot['sampled_ns'])
        if type(now_ns) is not int or not 0 <= now_ns - sampled < MAX_AGE_NS:
            return unavailable
        settings = snapshot['settings']
        if (type(settings) is not dict or type(settings['enabled']) is not bool or
                settings['selection'] not in ('automatic', 'screen', 'drive_mode', 'fixed')):
            return unavailable
        _integer(settings['day_brightness_permille'], 0, 1000)
        _integer(settings['night_brightness_permille'], 0, settings['day_brightness_permille'])
        _integer(settings['transition_ms'], 0, 5000)
        requested = snapshot['requested']
        if requested is not None:
            _value(requested['value'])
            if requested['source'] not in ('screen', 'drive_mode', 'fixed', 'disabled'):
                return unavailable
            if type(requested['night']) is not bool or requested['night_source'] not in ('headlights', 'schedule', 'policy'):
                return unavailable
            if not _decimal(requested['derived_ns']) <= sampled < _decimal(requested['valid_until_ns']):
                return unavailable
            if now_ns >= _decimal(requested['valid_until_ns']):
                requested = None
        current = snapshot['current']
        if current is not None:
            _value(current['value'], actual=True)
            _decimal(current['generation'])
            if current['origin'] not in ('measured', 'calculated', 'simulation'):
                return unavailable
            confirmed = _decimal(current['confirmed_ns'])
            maximum_age = _decimal(current['max_age_ns'])
            if not 0 <= sampled - confirmed < maximum_age:
                return unavailable
            if now_ns - confirmed >= maximum_age:
                current = None
        command = snapshot['command']
        if command is not None:
            _decimal(command['identifier'])
            _decimal(command['generation'])
            _value(command['value'])
            if command['status'] not in ('accepted', 'pending', 'applied', 'rejected', 'failed', 'timed_out'):
                return unavailable
        return copy.deepcopy({'available': True, 'settings': settings, 'requested': requested,
                              'current': current, 'command': command})
    except (ValueError, KeyError, TypeError):
        return unavailable
