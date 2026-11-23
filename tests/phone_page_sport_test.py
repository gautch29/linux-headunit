#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Pure sport page projection against the production acquisition owner."""
import ast
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_page_sport import VIEWS, sport_snapshot
from sport_telemetry import SportEngine, Observation, NS
from sport_source import SportSource, host_now_ns

BASE = 10 * NS


def fixture(origin='simulation', speed=36):
    engine = SportEngine()
    for step in range(6):
        stamp = BASE + step * 200_000_000
        engine.accept(7, {'speed_kmh': Observation(speed, stamp, origin=origin),
                          'acceleration': Observation((0, 0), stamp, origin=origin),
                          'gps_position': Observation((48, 2, 1), stamp, origin=origin)}, stamp)
    return engine


def model(engine, now=None):
    result = engine.model(engine.last_now if now is None else now)
    result.update(source='observations', error='', rejectedPackets=0)
    return result


def rows(view, tab):
    return {row['id']: row for row in view[tab]}


class CombinedNavigationTests(unittest.TestCase):
    def test_car_settings_and_sport_navigation_share_existing_owner(self):
        source = Path(__file__).resolve().parents[1] / 'tools/phone_page.py'
        page = next(node for node in ast.parse(source.read_text()).body
                    if isinstance(node, ast.ClassDef) and node.name == 'PhonePage')
        method = next(node for node in page.body if isinstance(node, ast.FunctionDef) and node.name == 'show_view')
        namespace = {'VIEWS': VIEWS}
        exec(compile(ast.Module(body=[method], type_ignores=[]), str(source), 'exec'), namespace)

        class Widget:
            def __init__(self):
                self.active = True
                self.visible = True
                self.text = None
                self.view = None

            def get_visible(self):
                return self.visible

            def get_active(self):
                return self.active

            def set_active(self, value):
                self.active = value

            def set_visible(self, value):
                self.visible = value

            def set_text(self, value):
                self.text = value

            def set_visible_child_name(self, value):
                self.view = value

        class Page:
            pass

        fixture = Page()
        fixture.view_buttons = {name: Widget() for name in VIEWS if name != 'sport'}
        fixture.pages = Widget()
        fixture.backdrop = type('Backdrop', (), {'widget': Widget()})()
        fixture.page_header = Widget()
        fixture.page_title = Widget()
        fixture.page_subtitle = Widget()
        fixture.add_phone = Widget()
        fixture.pairing = None
        for name, title in (('car', 'Car'), ('settings', 'Settings'), ('sport', 'Trip, Sport & History'),
                            ('cluster', 'Cluster'), ('home', 'Home')):
            namespace['show_view'](fixture, name)
            self.assertEqual(fixture.pages.view, name)
            self.assertEqual(fixture.page_title.text, title)
            self.assertEqual(fixture.page_header.visible, name != 'home')
        fixture.view_buttons['car'].visible = False
        fixture.pages.view = 'home'
        namespace['show_view'](fixture, 'car')
        self.assertEqual(fixture.pages.view, 'home')
        self.assertTrue(fixture.view_buttons['phones'].active)
        namespace['show_view'](fixture, 'foreign')
        self.assertEqual(fixture.pages.view, 'home')


class ProjectionTests(unittest.TestCase):
    def test_analytic_owner_values_units_and_original_scope_are_copied(self):
        engine = fixture()
        raw = model(engine)
        before = copy.deepcopy(raw)
        view = sport_snapshot(raw)
        trip = rows(view, 'trip')
        self.assertEqual(trip['sport-trip-distanceKm']['value'], '0.010 km')
        self.assertEqual(trip['sport-trip-coveredSeconds']['value'], '0:00:01.000')
        self.assertEqual(trip['sport-trip-averageSpeedKmh']['value'], '36.0 km/h')
        self.assertIn(str(BASE), trip['sport-trip-distanceKm']['detail'])
        self.assertIn(str(BASE + NS), trip['sport-trip-distanceKm']['detail'])
        self.assertIn('source 7', trip['sport-trip-distanceKm']['detail'])
        self.assertIn('SIMULATION', view['summary'])
        self.assertEqual(view['inputs']['speed_kmh']['acquiredNs'], BASE + NS)
        self.assertEqual(raw, before)
        raw['inputs']['acceleration']['value'] = [99, 99]
        self.assertEqual(view['inputs']['acceleration']['value'], (0, 0))

    def test_missing_baseline_and_actual_stopped_zero_are_separate(self):
        engine = SportEngine()
        empty = rows(sport_snapshot(model(engine, BASE)), 'trip')
        self.assertEqual(empty['sport-trip-distanceKm']['value'], 'Unavailable')
        stopped = rows(sport_snapshot(model(fixture(speed=0))), 'trip')
        self.assertEqual(stopped['sport-trip-distanceKm']['value'], '0.000 km')
        self.assertEqual(stopped['sport-trip-movingSeconds']['value'], '0:00:00.000')
        self.assertEqual(stopped['sport-trip-movingAverageSpeedKmh']['value'], 'Unavailable')
        sport = rows(sport_snapshot(model(fixture(speed=0))), 'sport')
        self.assertEqual(sport['sport-input-longitudinalG']['value'], '0.000 g')

    def test_age_withdrawal_never_changes_acquisitions_or_advances_values(self):
        raw = model(fixture())
        fresh = sport_snapshot(raw, status_age_ns=499_999_999)
        expired = sport_snapshot(raw, status_age_ns=500_000_000)
        self.assertEqual(rows(fresh, 'sport')['sport-input-speed']['value'], '36.0 km/h')
        self.assertEqual(rows(expired, 'sport')['sport-input-speed']['value'], 'Unavailable')
        self.assertEqual(expired['inputs']['speed_kmh']['acquiredNs'], raw['inputs']['speed_kmh']['acquiredNs'])
        self.assertEqual(expired['inputs']['speed_kmh']['ageNs'], 0)
        self.assertEqual(expired['inputs']['speed_kmh']['availability'], 'available')
        self.assertEqual(expired['inputs']['speed_kmh']['copyState'], 'expired copy')
        self.assertEqual(rows(fresh, 'trip')['sport-trip-distanceKm']['value'],
                         rows(expired, 'trip')['sport-trip-distanceKm']['value'])
        self.assertIn('HISTORICAL', rows(expired, 'trip')['sport-trip-distanceKm']['detail'])
        self.assertEqual(rows(expired, 'sport')['sport-peak-absoluteG']['value'], '0.000 g')
        self.assertIn('HISTORICAL', rows(expired, 'sport')['sport-peak-absoluteG']['title'])

    def test_owner_expiry_disconnect_and_new_generation_have_no_old_current_values(self):
        engine = fixture()
        stale = sport_snapshot(model(engine, BASE + 2 * NS))
        self.assertEqual(rows(stale, 'sport')['sport-input-speed']['value'], 'Unavailable')
        engine.accept(8, {'speed_kmh': Observation(0, BASE + 3 * NS, origin='measured')}, BASE + 3 * NS)
        current = sport_snapshot(model(engine))
        self.assertEqual(rows(current, 'trip')['sport-trip-distanceKm']['value'], 'Unavailable')
        self.assertEqual(rows(current, 'sport')['sport-input-speed']['value'], '0.0 km/h')
        self.assertIn('simulation', current['history'][0]['detail'])
        self.assertIn('HISTORICAL', current['history'][0]['title'])

    def test_original_completed_timers_survive_input_loss_as_historical(self):
        engine = SportEngine()
        for step in range(71):
            stamp = BASE + step * 100_000_000
            speed = 0 if step <= 10 else min(120, (step - 10) * 2)
            engine.accept(1, {'speed_kmh': Observation(speed, stamp, origin='simulation')}, stamp)
        raw = model(engine, engine.last_now + NS)
        result = raw['timers']['0-100']['result']
        self.assertIsNotNone(result)
        displayed = rows(sport_snapshot(raw), 'sport')['sport-timer-0-100']
        self.assertEqual(displayed['value'], '5.000 s')
        self.assertIn('HISTORICAL', displayed['detail'])
        self.assertIn(str(result['startNs']), displayed['detail'])
        self.assertIn(str(result['endNs']), displayed['detail'])
        self.assertIn(result['method'], displayed['detail'])

    def test_reset_and_session_requests_do_not_share_observed_identity(self):
        engine = fixture()
        before = model(engine)
        engine.reset_trip(engine.last_now + 1)
        reset = model(engine)
        self.assertNotEqual(before['trip']['journeyId'], reset['trip']['journeyId'])
        self.assertEqual(before['sessionId'], reset['sessionId'])
        self.assertEqual(rows(sport_snapshot(reset), 'trip')['sport-trip-distanceKm']['value'], 'Unavailable')
        engine.reset_session(engine.last_now + 1)
        session = model(engine)
        self.assertNotEqual(reset['sessionId'], session['sessionId'])
        self.assertEqual(reset['trip']['journeyId'], session['trip']['journeyId'])
        history = sport_snapshot(session)['history']
        self.assertTrue(all('HISTORICAL' in item['title'] for item in history))
        ended = next(item for item in history if item['title'].startswith('Sport session'))
        self.assertIn('session origins simulation', ended['detail'])
        self.assertIn('session observations', ended['detail'])

    def test_manager_loss_disabled_malformed_and_reversed_age_fail_closed(self):
        raw = model(fixture())
        for broken in (None, {}, {'configured': False}, {'configured': True, 'version': 2},
                       {**raw, 'version': True}, {**raw, 'sessionId': False}):
            with self.subTest(broken=broken):
                view = sport_snapshot(broken)
                self.assertFalse(view['actionable'])
                self.assertEqual(view['trip'][0]['value'], 'Unavailable')
        lost = sport_snapshot(raw, status_current=False)
        self.assertFalse(lost['actionable'])
        self.assertEqual(lost['history'][0]['value'], 'Unavailable')
        for bad_age in (-1, True, float('nan')):
            self.assertEqual(rows(sport_snapshot(raw, status_age_ns=bad_age), 'sport')['sport-input-speed']['value'], 'Unavailable')

    def test_stale_disconnected_nonfinite_and_bad_dates_do_not_render_live(self):
        raw = model(fixture())
        for change in ({'availability': 'stale'}, {'availability': 'disconnected'},
                       {'value': None}, {'value': True}, {'value': float('nan')},
                       {'value': 2**4096}, {'ageNs': 2**4096}, {'maximumAgeNs': {'unknown': object()}},
                       {'acquiredNs': raw['observedNs'] + 1}, {'ageNs': 1}):
            candidate = copy.deepcopy(raw)
            candidate['inputs']['speed_kmh'].update(change)
            self.assertEqual(rows(sport_snapshot(candidate), 'sport')['sport-input-speed']['value'], 'Unavailable')

    def test_malformed_numeric_text_and_nested_metadata_stay_bounded(self):
        raw = model(fixture())
        raw['trip']['distanceKm'] = 2**4096
        self.assertEqual(rows(sport_snapshot(raw), 'trip')['sport-trip-distanceKm']['value'], 'Unavailable')
        raw['inputs']['speed_kmh']['ageNs'] = {'unknown': object()}
        projected = sport_snapshot(raw)
        self.assertIsNone(projected['inputs']['speed_kmh']['ageNs'])
        for error in ('\ud800', 'X' * 257, {'unknown': object()}, None):
            view = sport_snapshot({**raw, 'error': error})
            self.assertFalse(view['actionable'])
            self.assertEqual(view['trip'][0]['value'], 'Unavailable')

    def test_history_is_bounded_and_unknown_nested_payload_is_never_copied(self):
        raw = model(fixture())
        raw['history'] = [{'kind': 'aborted', 'reason': 'Gap', 'unknown': object()}] * 1000
        raw['trip']['history'] = [{'distanceKm': 0, 'unknown': object()}] * 1000
        projected = sport_snapshot(raw)
        self.assertEqual(len(projected['history']), 80)
        self.assertTrue(all(set(item) == {'id', 'title', 'value', 'detail', 'displayDetail'} for item in projected['history']))
        raw['error'] = 'Sport acquisition stopped'
        self.assertFalse(sport_snapshot(raw)['actionable'])

    def test_driver_text_is_concise_while_original_provenance_is_preserved(self):
        raw = model(fixture())
        view = sport_snapshot(raw)
        self.assertNotIn('session', view['summary'])
        self.assertNotIn('source 7', view['summary'])
        self.assertEqual(view['provenance']['sessionId'], raw['sessionId'])
        self.assertEqual(view['provenance']['sourceGeneration'], 7)
        self.assertEqual(view['provenance']['observedNs'], raw['observedNs'])
        for tab in ('trip', 'sport', 'history'):
            for item in view[tab]:
                self.assertNotIn(str(BASE), item['displayDetail'])
                self.assertNotIn('host monotonic', item['displayDetail'])
                self.assertNotIn('source 7', item['displayDetail'])
        data = rows(view, 'sport')['sport-input-speed']
        self.assertIn(str(BASE + NS), data['detail'])
        self.assertEqual(data['displayDetail'], 'Simulation · Current reading')
        self.assertEqual(rows(view, 'trip')['sport-trip-journey']['value'], 'Speed estimate')

    def test_actual_source_retirement_and_failed_worker_withdraw_original_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(Path(folder).resolve())
            stamp = host_now_ns()
            with source.lock:
                source.engine.accept(1, {'speed_kmh': Observation(36, stamp, origin='simulation')}, stamp)
            before = source.snapshot()
            source.close()
            retired = sport_snapshot(source.snapshot())
            self.assertEqual(retired['inputs']['speed_kmh']['availability'], 'disconnected')
            self.assertEqual(retired['inputs']['speed_kmh']['acquiredNs'], before['inputs']['speed_kmh']['acquiredNs'])
            self.assertEqual(rows(retired, 'sport')['sport-input-speed']['value'], 'Unavailable')
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(Path(folder).resolve(), simulated=True)
            with patch('sport_source.simulated_observations', side_effect=OSError('Fixture failed acquisition')) as acquisition:
                source.start()
                source.thread.join(timeout=1)
                self.assertFalse(source.thread.is_alive())
                acquisition.assert_called_once()
            failed = sport_snapshot(source.snapshot())
            self.assertIn('Sport acquisition stopped', failed['summary'])
            self.assertFalse(failed['actionable'])
            self.assertEqual(rows(failed, 'sport')['sport-input-speed']['value'], 'Unavailable')
            source.close()


if __name__ == '__main__':
    unittest.main()
