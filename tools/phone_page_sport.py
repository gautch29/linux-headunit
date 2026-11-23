# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Native sport leaf: bounded copied data, existing Home2 widgets only.

No acquisition, timer interpolation, persistence, phone or display ownership.
SportView receives the existing page's GTK/helpers; the projection is pure.
"""
import math


NS = 1_000_000_000
VIEWS = ('home', 'phones', 'music', 'cluster', 'car', 'camera', 'settings', 'sport')
TABS = ('trip', 'sport', 'history')
ORIGINS = ('measured', 'calculated', 'simulation')
TIMERS = ('0-100', '50-80', '0-200', 'quarter-mile')
PEAKS = ('longitudinalPositiveG', 'longitudinalNegativeG', 'lateralPositiveG',
         'lateralNegativeG', 'absoluteG')
PEAK_TITLES = ('Forward peak', 'Braking peak', 'Left peak', 'Right peak', 'Vector magnitude peak')


def number(value, low=0, high=315_576_000):
    return type(value) in (int, float) and low <= value <= high and math.isfinite(value)


def date(value):
    return type(value) is int and 0 < value < 2**64


def text(value, limit=256):
    return (value if type(value) is str and len(value) <= limit and
            not any(ord(c) < 32 or ord(c) == 127 or 0xd800 <= ord(c) <= 0xdfff for c in value) else '')


def amount(value, unit, low=0, high=315_576_000, digits=1):
    return f'{value:.{digits}f} {unit}' if number(value, low, high) else 'Unavailable'


def duration(value):
    # Acquired duration only; a redraw never adds a second.
    if not number(value):
        return 'Unavailable'
    milliseconds = int(value * 1000)
    seconds, milliseconds = divmod(milliseconds, 1000)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    return f'{hours}:{minutes:02d}:{seconds:02d}.{milliseconds:03d}'


def row(identifier, title, value, detail='', display_detail=''):
    # Original scope/date evidence remains in the projection, not driver text.
    return {'id': 'sport-' + identifier, 'title': title, 'value': value,
            'detail': detail, 'displayDetail': display_detail}


def source_label(value):
    return {'measured': 'Measured', 'calculated': 'Calculated', 'simulation': 'Simulation'}.get(value, 'Source unavailable') if type(value) is str else 'Source unavailable'


def reading_label(value):
    state = ('Current reading' if value['current'] else 'Expired snapshot' if value['copyState'] == 'expired copy' else
             {'stale': 'Stale reading', 'disconnected': 'Disconnected', 'unavailable': 'Unavailable'}.get(value['availability'], 'Unavailable'))
    return source_label(value['origin']) + ' · ' + state


def reason_label(value):
    value = text(value)
    return {'Source generation changed': 'Source changed', 'Need new fresh speed': 'Waiting for speed',
            'Need fresh speed': 'Waiting for speed', 'Speed acquisition gap': 'Speed data gap',
            'Set a start/finish line': 'No timing line configured'}.get(value, value)


def origin(value):
    return value if value in ORIGINS else 'origin unavailable'


def scope(value):
    generation = value.get('sourceGeneration')
    identity = str(generation) if type(generation) is int and 0 <= generation < 2**64 else 'unavailable'
    return f'{origin(value.get("origin"))} · source {identity}'


def acquired(value, first='firstAcquiredNs', last='lastAcquiredNs'):
    a, b = value.get(first), value.get(last)
    if date(a) and date(b) and b >= a:
        return f'Acquired {a}–{b} ns · host monotonic'
    return 'Acquisition dates unavailable'


def input_copy(value, observed, status_age_ns, key):
    """Copy only fixed fields; age can withdraw, never refresh, an observation.

    status_age_ns is elapsed since the existing manager REQUEST dispatch. It
    conservatively spends the input's remaining age budget, without redating
    acquiredNs/observedNs/ageNs or advancing a displayed measurement.
    """
    value = value if type(value) is dict else {}
    result = {key: value.get(key) for key in
              ('acquiredNs', 'maximumAgeNs', 'ageNs', 'origin', 'availability')}
    if not date(result['acquiredNs']):
        result['acquiredNs'] = None
    if type(result['maximumAgeNs']) is not int or not 0 < result['maximumAgeNs'] <= 86400 * NS:
        result['maximumAgeNs'] = None
    if type(result['ageNs']) is not int or not 0 <= result['ageNs'] < 2**64:
        result['ageNs'] = None
    if result['origin'] not in ORIGINS:
        result['origin'] = None
    if result['availability'] not in ('available', 'unavailable', 'stale', 'disconnected'):
        result['availability'] = None
    acquired_ns, maximum, age = (result[key] for key in ('acquiredNs', 'maximumAgeNs', 'ageNs'))
    valid = (date(observed) and date(acquired_ns) and acquired_ns <= observed and
             type(maximum) is int and 0 < maximum <= 86400 * NS and
             type(age) is int and age == observed - acquired_ns and
             result['origin'] in ORIGINS and result['availability'] == 'available' and
             type(status_age_ns) is int and status_age_ns >= 0)
    result['current'] = valid and age + status_age_ns < maximum
    result['copyState'] = 'current' if result['current'] else 'expired copy' if valid else 'unavailable copy'
    result['value'] = None
    if result['current']:
        raw = value.get('value')
        if key == 'speed_kmh' and number(raw, 0, 300):
            result['value'] = raw
        elif key == 'acceleration' and type(raw) in (list, tuple) and len(raw) == 2 and all(number(n, -100, 100) for n in raw):
            result['value'] = tuple(raw)
        elif (key == 'gps_position' and type(raw) in (list, tuple) and len(raw) == 3 and
              number(raw[0], -90, 90) and number(raw[1], -180, 180) and number(raw[2], 0, 10000) and raw[2] > 0):
            result['value'] = tuple(raw)
        else:
            result['current'] = False
            result['copyState'] = 'unavailable copy'
    return result


def input_detail(value, generation):
    state = value['availability'] if value['availability'] in ('available', 'unavailable', 'stale', 'disconnected') else 'unavailable'
    detail = f'Copied {state} · {value["copyState"]} · {origin(value["origin"])} · source {generation}'
    if date(value['acquiredNs']):
        detail += f' · acquired {value["acquiredNs"]} ns'
    if type(value['ageNs']) is int and value['ageNs'] >= 0:
        detail += f' · copied age {value["ageNs"] / 1_000_000:.0f} ms'
    return detail


def historical_result(value, maximum=315_576_000):
    if type(value) is not dict or not number(value.get('seconds'), 0, maximum) or not (
            date(value.get('startNs')) and date(value.get('endNs')) and value['endNs'] >= value['startNs'] and
            value.get('origin') in ORIGINS and date(value.get('sessionId')) and
            abs(value['seconds'] - (value['endNs'] - value['startNs']) / NS) <= 1e-9):
        return 'Unavailable', ''
    return amount(value['seconds'], 's', digits=3), (
        'HISTORICAL · ' + scope(value) + f' · session {value["sessionId"]} · ' +
        acquired(value, 'startNs', 'endNs') + ' · ' + text(value.get('method'), 64) +
        (f' · maximum gap {value["maximumGapNs"]} ns' if date(value.get('maximumGapNs')) else ''))


def trip_rows(value, current):
    if (type(value) is not dict or type(value.get('version')) is not int or value['version'] != 1 or
            value.get('authority') != 'software-speed-estimate' or
            value.get('method') != 'linear-speed-trapezoids'):
        return [row('trip-unavailable', 'Software trip', 'Unavailable')]
    detail = ('Acquired estimate · ' if current else 'HISTORICAL · retained estimate · ') + scope(value) + ' · ' + acquired(value)
    rows = [row('trip-journey', 'Software trip', 'Speed estimate', detail,
                source_label(value.get('origin')) + ' · estimate from acquired speed')]
    phase = value.get('phase') if value.get('phase') in ('waiting', 'collecting', 'limited') else 'unavailable'
    rows.append(row('trip-state', 'Coverage', phase.capitalize(), text(value.get('reason')), reason_label(value.get('reason'))))
    public_detail = source_label(value.get('origin')) + (' · acquired estimate' if current else ' · historical estimate')
    for key, title, unit, maximum, digits in (
            ('distanceKm', 'Covered distance estimate', 'km', 9_999_999, 3),
            ('movingDistanceKm', 'Moving distance estimate', 'km', 9_999_999, 3),
            ('averageSpeedKmh', 'Covered average speed', 'km/h', 300, 1),
            ('movingAverageSpeedKmh', 'Moving average speed', 'km/h', 300, 1)):
        rows.append(row('trip-' + key, title, amount(value.get(key), unit, high=maximum, digits=digits), detail, public_detail))
    for key, title in (('coveredSeconds', 'Covered time'), ('movingSeconds', 'Moving time'),
                       ('acquiredSpanSeconds', 'Observed span'), ('uncoveredSeconds', 'Missing coverage')):
        rows.append(row('trip-' + key, title, duration(value.get(key)), detail, public_detail))
    ratio = value.get('coverageRatio')
    coverage = f'{ratio * 100:.1f} %' if number(ratio, 0, 1) else 'Unavailable'
    rows.append(row('trip-coverageRatio', 'Sample coverage', coverage,
                    'Partial coverage' if value.get('partial') is True else 'Coverage from original acquisitions',
                    'Partial coverage' if value.get('partial') is True else 'Based on acquired samples'))
    peak = value.get('peak')
    if type(peak) is not dict or not date(peak.get('acquiredNs')):
        peak = {}
    rows.append(row('trip-peak', 'Peak speed · HISTORICAL', amount(peak.get('speedKmh'), 'km/h', high=300),
                    f'{origin(peak.get("origin"))} · acquired {peak["acquiredNs"]} ns' if peak else 'No acquired peak',
                    'Historical · ' + source_label(peak.get('origin')) if peak else 'No acquired peak'))
    return rows


def sport_snapshot(model, *, status_current=True, status_age_ns=0):
    """Bounded projection of version-1 manager data, with no retained authority."""
    empty = {'configured': False, 'actionable': False, 'summary': 'Sport information unavailable',
             'inputs': {}, 'trip': [row('trip-unavailable', 'Software trip', 'Unavailable')],
             'sport': [row('input-unavailable', 'Sport telemetry', 'Unavailable')],
             'history': [row('history-unavailable', 'Historical results', 'Unavailable')]}
    if not status_current or type(model) is not dict:
        return empty
    if model.get('configured') is False:
        return {**empty, 'summary': 'Sport telemetry is not configured'}
    if (model.get('configured') is not True or type(model.get('version')) is not int or model['version'] != 1 or
            not date(model.get('observedNs')) or not date(model.get('sessionId'))):
        return empty
    raw_error = model.get('error', '')
    if type(raw_error) is not str or text(raw_error) != raw_error:
        return empty
    generation = model.get('sourceGeneration')
    generation_text = str(generation) if type(generation) is int and 0 <= generation < 2**64 else 'unavailable'
    source = model.get('source') if model.get('source') in ('simulation', 'observations', 'unavailable') else 'unavailable'
    inputs = model.get('inputs') if type(model.get('inputs')) is dict else {}
    inputs = {key: input_copy(inputs.get(key), model['observedNs'], status_age_ns, key)
              for key in ('speed_kmh', 'acceleration', 'gps_position')}
    if any(v['origin'] == 'simulation' for v in inputs.values()) or source == 'simulation':
        header_source = 'SIMULATION'
    else:
        header_source = 'Observed data' if source == 'observations' else 'Source unavailable'
    summary = f'{header_source} · software estimates'
    error = text(model.get('error'))
    if error:
        summary += ' · ' + error
    rows = []
    speed = inputs['speed_kmh']
    speed_current = speed['current'] and type(speed['value']) in (int, float)
    rows.append(row('input-speed', 'Speed', amount(speed['value'], 'km/h', high=300) if speed_current else 'Unavailable',
                    input_detail(speed, generation_text), reading_label(speed)))
    meter = model.get('gMeter') if type(model.get('gMeter')) is dict else {}
    acceleration = inputs['acceleration']
    acceleration_current = acceleration['current'] and type(acceleration['value']) is tuple and len(acceleration['value']) == 2
    for key, title in (('longitudinalG', 'Longitudinal acceleration'), ('lateralG', 'Lateral acceleration')):
        rows.append(row('input-' + key, title,
                        amount(meter.get(key), 'g', -100, 100, 3) if acceleration_current else 'Unavailable',
                        input_detail(acceleration, generation_text), reading_label(acceleration)))
    gps = inputs['gps_position']
    gps_current = gps['current'] and type(gps['value']) is tuple and len(gps['value']) == 3
    rows.append(row('input-gps', 'GPS reading', 'Available' if gps_current else 'Unavailable', input_detail(gps, generation_text), reading_label(gps)))
    timers = model.get('timers') if type(model.get('timers')) is dict else {}
    for key in TIMERS:
        timer = timers.get(key) if type(timers.get(key)) is dict else {}
        result, detail = historical_result(timer.get('result'), 120)
        if result == 'Unavailable':
            phase = timer.get('phase') if timer.get('phase') in ('waiting', 'armed', 'running', 'invalid') else 'unavailable'
            result = amount(timer.get('elapsedSeconds'), 's', high=120, digits=3) if phase == 'running' and speed_current else phase.capitalize()
            if phase in ('running', 'armed') and not speed_current:
                result = 'Unavailable'
            detail = text(timer.get('reason')) or 'Advances only on acquired speed'
        public = 'Historical · ' + source_label(timer.get('result', {}).get('origin')) if type(timer.get('result')) is dict and detail.startswith('HISTORICAL') else reason_label(timer.get('reason'))
        rows.append(row('timer-' + key, key.replace('quarter-mile', '¼ mile'), result, detail, public))
    laps = model.get('laps') if type(model.get('laps')) is dict else {}
    rows.append(row('lap-current', 'Current lap',
                    amount(laps.get('elapsedSeconds'), 's', digits=3) if gps_current and laps.get('phase') == 'running' else 'Unavailable',
                    text(laps.get('reason')) or 'Uses the configured directional line; acquired GPS only', reason_label(laps.get('reason'))))
    for key in ('last', 'best'):
        result, detail = historical_result(laps.get(key))
        record = laps.get(key)
        public = 'Historical · ' + source_label(record.get('origin')) if type(record) is dict and detail else ''
        rows.append(row('lap-' + key, key.capitalize() + ' lap · HISTORICAL', result, detail, public))
    rows.append(row('lap-delta', 'Delta to best lap', amount(laps.get('deltaSeconds'), 's', -120, 120, 3) if gps_current else 'Unavailable',
                    'Positive means behind; no estimate without a matched acquired position', 'Positive means behind the best lap'))
    peaks = meter.get('peaks') if type(meter.get('peaks')) is dict else {}
    dates = meter.get('peakObservations') if type(meter.get('peakObservations')) is dict else {}
    for key, title in zip(PEAKS, PEAK_TITLES):
        observation = dates.get(key) if type(dates.get(key)) is dict else {}
        value = amount(peaks.get(key), 'g', -100, 100, 3) if date(observation.get('acquiredNs')) else 'Unavailable'
        rows.append(row('peak-' + key, title + ' · HISTORICAL', value,
                        f'{origin(observation.get("origin"))} · acquired {observation["acquiredNs"]} ns' if observation else 'No acquired peak',
                        'Historical · ' + source_label(observation.get('origin')) if observation else 'No acquired peak'))
    trip = model.get('trip')
    history = []
    archived = trip.get('history') if type(trip) is dict else None
    if type(archived) is list:
        for index, item in enumerate(reversed(archived[-16:])):
            if type(item) is not dict:
                continue
            history.append(row('history-trip-' + str(index), 'Software trip · HISTORICAL',
                               amount(item.get('distanceKm'), 'km', high=9_999_999, digits=3),
                               scope(item) + ' · ' + acquired(item) + ' · ' + text(item.get('endedReason')) +
                               (f' · archived observation {item["archivedAtNs"]} ns' if date(item.get('archivedAtNs')) else ''),
                               'Historical · ' + source_label(item.get('origin')) + ' · ' + reason_label(item.get('endedReason'))))
    events = model.get('history')
    if type(events) is list:
        for index, item in enumerate(reversed(events[-64:])):
            if type(item) is not dict:
                continue
            value, detail = historical_result(item)
            kind = text(item.get('timer'), 40) or text(item.get('kind'), 40) or 'Sport event'
            if not detail:
                sources = item.get('origins')
                sources = [v for v in sources[:3] if v in ORIGINS] if type(sources) is list else []
                detail = scope(item)
                if date(item.get('sessionId')):
                    detail += f' · session {item["sessionId"]}'
                if sources:
                    detail += ' · session origins ' + ', '.join(sources)
                if date(item.get('atNs')):
                    detail += f' · event observation {item["atNs"]} ns'
                if date(item.get('startNs')) and date(item.get('endNs')):
                    detail += f' · session observations {item["startNs"]}–{item["endNs"]} ns'
                detail += ' · event dates are observations, not acquisitions'
            public = 'Historical · ' + source_label(item.get('origin')) if value != 'Unavailable' else 'Historical event'
            event_value = value
            if value == 'Unavailable':
                event_value = reason_label(item.get('reason')) or ('Ended' if kind == 'session-ended' else 'Unavailable')
            history.append(row('history-event-' + str(index), ('Sport session' if kind == 'session-ended' else kind) + ' · HISTORICAL',
                               event_value,
                               detail, public))
    known_inputs = [item for item in inputs.values() if date(item['acquiredNs'])]
    withdrawn = bool(known_inputs) and all(item['availability'] == 'disconnected' for item in known_inputs)
    return {'configured': True, 'actionable': not error and source != 'unavailable' and not withdrawn, 'summary': summary,
            'provenance': {'sessionId': model['sessionId'], 'sourceGeneration': generation if type(generation) is int and 0 <= generation < 2**64 else None,
                           'observedNs': model['observedNs'],
                           'journeyId': trip.get('journeyId') if type(trip) is dict and date(trip.get('journeyId')) else None},
            'inputs': inputs, 'trip': trip_rows(trip, speed_current), 'sport': rows,
            'history': history or [row('history-empty', 'Historical results', 'No retained results',
                                      'Software trips: process memory · sport events: existing owner history')]}


class SportView:
    """One leaf on PhonePage's existing stack; no window or request executor."""
    def __init__(self, app, Gtk, Pango, label, list_group):
        self.app = app
        self.Gtk = Gtk
        self.Pango = Pango
        self.label = label
        self.model = sport_snapshot(None)
        self.tab = 'trip'
        self.rendered_rows = None
        self.widget = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        top = Gtk.Box(spacing=12)
        self.widget.append(top)
        self.parent_button = Gtk.Button(label='Back to Cluster')
        self.parent_button.set_name('sport-back-cluster')
        self.parent_button.add_css_class('quiet')
        self.parent_button.connect('clicked', lambda _: app.show_view('cluster'))
        top.append(self.parent_button)
        segments = Gtk.Box(spacing=0, homogeneous=True)
        segments.add_css_class('segmented')
        top.append(segments)
        self.tabs = {}
        for key, title in zip(TABS, ('Trip', 'Sport', 'History')):
            button = Gtk.Button(label=title)
            button.set_name('sport-tab-' + key)
            button.add_css_class('segment')
            button.connect('clicked', lambda _, name=key: self.select(name))
            segments.append(button)
            self.tabs[key] = button
        self.summary = label('', 'muted')
        self.summary.set_wrap(True)
        self.summary.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.summary.set_ellipsize(Pango.EllipsizeMode.NONE)
        self.widget.append(self.summary)
        scroll = Gtk.ScrolledWindow()
        scroll.set_vexpand(True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.widget.append(scroll)
        self.rows = list_group()
        self.rows.set_valign(Gtk.Align.START)
        scroll.set_child(self.rows)
        self.scroll = scroll
        self.controls = Gtk.Box(spacing=12)
        self.widget.append(self.controls)
        self.buttons = {}
        for key, identifier, title in (('trip_reset', 'sport-trip-reset', 'Reset software trip'),
                                       ('sport_session', 'sport-session-new', 'New sport session')):
            button = Gtk.Button(label=title)
            button.set_name(identifier)
            button.add_css_class('quiet')
            button.connect('clicked', lambda _, action=key: app.action({'action': action}))
            self.controls.append(button)
            self.buttons[key] = button
        self.note = label('Software estimates only. Resets change software statistics; car trip and odometer are independent.', 'muted')
        self.note.add_css_class('small')
        self.note.set_wrap(True)
        self.note.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.note.set_ellipsize(Pango.EllipsizeMode.NONE)
        self.widget.append(self.note)
        self.select('trip')

    def select(self, name):
        if name not in TABS:
            return
        self.tab = name
        for key, button in self.tabs.items():
            if key == name:
                button.add_css_class('selected')
            else:
                button.remove_css_class('selected')
        self.buttons['trip_reset'].set_visible(name == 'trip')
        self.buttons['sport_session'].set_visible(name == 'sport')
        self.controls.set_visible(name != 'history')
        self.scroll.get_vadjustment().set_value(0)
        self.paint()

    def update(self, model, *, current, age_ns=0):
        self.model = sport_snapshot(model, status_current=current, status_age_ns=age_ns)
        self.summary.set_text(self.model['summary'])
        self.paint()
        self.sensitivity()

    def sensitivity(self):
        safe = self.app.status_current() and not self.app.pending and self.model['actionable']
        for button in self.buttons.values():
            button.set_sensitive(safe)

    def paint(self):
        rows = self.model[self.tab]
        if self.rendered_rows == rows:
            return
        self.rendered_rows = rows
        while self.rows.get_first_child():
            self.rows.remove(self.rows.get_first_child())
        self.row_widgets = {}
        for item in rows:
            # Noninteractive grouped data rows reuse the existing list surface.
            container = self.Gtk.Box(orientation=self.Gtk.Orientation.VERTICAL, spacing=5)
            container.add_css_class('list-row')
            container.set_name(item['id'])
            heading = self.Gtk.Box(spacing=12)
            container.append(heading)
            title = self.label(item['title'].replace(' · HISTORICAL', ''))
            title.set_hexpand(True)
            heading.append(title)
            value = self.label(item['value'], 'section')
            heading.append(value)
            detail = self.label(item['displayDetail'], 'muted')
            detail.add_css_class('small')
            detail.set_wrap(True)
            detail.set_wrap_mode(self.Pango.WrapMode.WORD_CHAR)
            detail.set_ellipsize(self.Pango.EllipsizeMode.NONE)
            detail.set_visible(bool(item['displayDetail']))
            container.append(detail)
            self.rows.append(container)
            self.row_widgets[item['id']] = (container, title, value, detail)
