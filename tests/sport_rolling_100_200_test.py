#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Fixed rolling speed estimate; independent synthetic acquisitions, no physical timing."""
from collections import deque
from fractions import Fraction
from pathlib import Path
import socket
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path[:0] = [str(Path(__file__).resolve().parents[1] / 'tools'), str(Path(__file__).resolve().parent)]
from sport_telemetry import Observation, SportEngine, NS
from sport_source import SportSource, host_now_ns
from sport_telemetry_test import packet
import phone_management_test as manager_fixture

BASE = 10 * NS
STEP = 200_000_000


def acquire(engine, at, value, generation=1, origin='simulation', now=None, age=500_000_000):
    engine.accept(generation, {'speed_kmh': Observation(value, at, age, origin)}, at if now is None else now)


def primitive(value):
    if value is None or type(value) in (str, int, float, bool):
        return value
    if isinstance(value, dict):
        return {key: primitive(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, deque)):
        return [primitive(item) for item in value]
    if isinstance(value, set):
        return sorted(primitive(item) for item in value)
    return primitive(vars(value))


def rolling(engine, now):
    return engine.model(now)['rolling100To200']


def ramp(engine, base=BASE, generation=1, origin='simulation'):
    for index, value in enumerate((98, 110, 150, 190, 210)):
        acquire(engine, base + index * STEP, value, generation, origin)
    return rolling(engine, base + 4 * STEP)


class RollingTests(unittest.TestCase):
    def test_independent_piecewise_reference_and_original_brackets(self):
        engine = SportEngine()
        model = ramp(engine)
        result = model['lastResult']
        # Independent rational interpolation from the fixture's known segments.
        start = BASE + round(Fraction(100 - 98, 110 - 98) * STEP)
        end = BASE + 3 * STEP + round(Fraction(200 - 190, 210 - 190) * STEP)
        self.assertEqual(model['phase'], 'complete')
        self.assertEqual(result['startCrossing']['derivedNs'], start)
        self.assertEqual(result['endCrossing']['derivedNs'], end)
        self.assertEqual(result['seconds'], (end - start) / NS)
        self.assertEqual(result['sourceGeneration'], 1)
        self.assertEqual(result['sessionId'], 1)
        self.assertEqual(result['origin'], 'simulation')
        for crossing, pair in ((result['startCrossing'], ((98, BASE), (110, BASE + STEP))),
                               (result['endCrossing'], ((190, BASE + 3 * STEP), (210, BASE + 4 * STEP)))):
            for key, (value, at) in zip(('before', 'after'), pair):
                self.assertEqual(crossing[key], {'speedKmh': value, 'acquiredNs': at,
                                                'maximumAgeNs': 500_000_000, 'origin': 'simulation'})
        self.assertEqual(model['authority'], 'software-speed-estimate')

    def test_linear_ramps_with_independent_durations_and_large_exact_dates(self):
        for base in (BASE, 9_000_000_000_000_000_000):
            for slope in (20, 80, 125):
                with self.subTest(base=base, slope=slope):
                    engine = SportEngine()
                    # v(t)=80+slope*t; 50ms acquisitions, independently 100/slope seconds.
                    for index in range(1 + (240 - 80) * 20 // slope):
                        acquire(engine, base + index * 50_000_000, 80 + slope * index / 20)
                    result = rolling(engine, engine.last_now)['lastResult']
                    self.assertAlmostEqual(result['seconds'], 100 / slope, places=8)
                    self.assertEqual(result['startCrossing']['derivedNs'], base + round(Fraction(20, slope) * NS))
                    self.assertEqual(result['endCrossing']['derivedNs'], base + round(Fraction(120, slope) * NS))

    def test_no_start_above_rearm_band_or_partial_initial_crossing(self):
        engine = SportEngine()
        for index, value in enumerate((99, 101, 150, 201)):
            acquire(engine, BASE + index * STEP, value)
        model = rolling(engine, engine.last_now)
        self.assertEqual(model['phase'], 'waiting')
        self.assertIsNone(model['lastResult'])
        self.assertIsNone(model['elapsedSeconds'])

    def test_exact_rearm_hysteresis_and_jitter_never_retrigger(self):
        engine = SportEngine()
        result = ramp(engine)['lastResult']
        at = engine.last_now
        for value in (99, 101, 99, 110, 201):
            at += STEP
            acquire(engine, at, value)
            self.assertEqual(rolling(engine, at)['lastResult'], result)
            self.assertNotEqual(rolling(engine, at)['phase'], 'running')
        at += STEP
        acquire(engine, at, 98)
        self.assertEqual(rolling(engine, at)['phase'], 'armed')
        at += STEP
        acquire(engine, at, 100)
        self.assertEqual(rolling(engine, at)['phase'], 'running')
        self.assertEqual(rolling(engine, at)['lastResult'], result)

    def test_both_thresholds_one_interval_and_one_ns_resolution(self):
        for interval in (STEP, 1):
            with self.subTest(interval=interval):
                engine = SportEngine()
                acquire(engine, BASE, 98)
                acquire(engine, BASE + interval, 300)
                result = rolling(engine, BASE + interval)['lastResult']
                self.assertGreater(result['seconds'], 0)
                self.assertEqual(result['startCrossing']['before']['acquiredNs'], BASE)
                self.assertEqual(result['endCrossing']['after']['acquiredNs'], BASE + interval)
                expected_start = BASE + round(Fraction(2, 202) * interval)
                expected_end = BASE + round(Fraction(102, 202) * interval)
                self.assertEqual(result['seconds'], (expected_end - expected_start) / NS)

    def test_collapsed_crossing_dates_do_not_invent_duration_or_result(self):
        engine = SportEngine()
        acquire(engine, BASE, 98)
        acquire(engine, BASE + 1, 101)
        acquire(engine, BASE + 2, 300)
        model = rolling(engine, BASE + 2)
        self.assertEqual(model['phase'], 'invalid')
        self.assertEqual(model['reason'], 'Insufficient acquisition resolution')
        self.assertIsNone(model['elapsedSeconds'])
        self.assertIsNone(model['lastResult'])

    def test_only_new_acquisitions_advance_elapsed_or_complete(self):
        engine = SportEngine()
        acquire(engine, BASE, 98)
        acquire(engine, BASE + STEP, 110)
        first = rolling(engine, BASE + STEP)
        for now in (BASE + STEP, BASE + STEP + 1, BASE + STEP + 100_000_000):
            engine.accept(1, {'speed_kmh': engine.inputs['speed_kmh']}, now)
            self.assertEqual(rolling(engine, now), first)
        acquire(engine, BASE + 2 * STEP, 150)
        self.assertGreater(rolling(engine, engine.last_now)['elapsedSeconds'], first['elapsedSeconds'])
        self.assertIsNone(rolling(engine, engine.last_now)['lastResult'])

    def test_stale_exact_expiry_and_withdrawal_require_new_rearm(self):
        for state in ('expiry', 'unavailable', 'disconnected'):
            with self.subTest(state=state):
                engine = SportEngine()
                acquire(engine, BASE, 98)
                acquire(engine, BASE + STEP, 110)
                old = engine.inputs['speed_kmh']
                if state == 'expiry':
                    now = old.acquired_ns + old.maximum_age_ns
                    model = rolling(engine, now)
                else:
                    now = BASE + 2 * STEP
                    withdrawn_at = now if state == 'unavailable' else old.acquired_ns
                    engine.accept(1, {'speed_kmh': Observation(None, withdrawn_at, origin='simulation',
                                                             availability=state)}, now)
                    model = rolling(engine, now)
                self.assertEqual(model['phase'], 'invalid')
                self.assertEqual(rolling(engine, now)['phase'], 'invalid')
                acquire(engine, now + STEP, 210)
                self.assertIsNone(rolling(engine, engine.last_now)['lastResult'])

    def test_gap_age_boundary_and_delayed_stale_predecessor_do_not_bridge(self):
        for interval, age, delay in ((250_000_001, 500_000_000, 0),
                                     (200_000_000, 200_000_000, 0),
                                     (200_000_000, 500_000_000, 300_000_000)):
            with self.subTest(interval=interval, age=age, delay=delay):
                engine = SportEngine()
                acquire(engine, BASE, 98, age=age)
                acquire(engine, BASE + interval, 210, now=BASE + interval + delay, age=age)
                self.assertIsNone(rolling(engine, engine.last_now)['lastResult'])
                self.assertNotEqual(rolling(engine, engine.last_now)['phase'], 'running')

    def test_rejected_atomic_replay_future_clock_and_policy_leave_whole_owner_exact(self):
        engine = SportEngine()
        acquire(engine, BASE, 98)
        acquire(engine, BASE + STEP, 110)
        attempts = [(1, {'speed_kmh': Observation(210, BASE)}, BASE + 2 * STEP),
                    (1, {'speed_kmh': Observation(210, BASE + 3 * STEP)}, BASE + 2 * STEP),
                    (1, {'speed_kmh': Observation(210, BASE + 2 * STEP)}, BASE),
                    (1, {'speed_kmh': Observation(210, BASE + 2 * STEP, 1)}, BASE + 2 * STEP),
                    (1, {'speed_kmh': Observation(210, BASE + 2 * STEP),
                         'acceleration': Observation((float('nan'), 0), BASE + 2 * STEP)}, BASE + 2 * STEP)]
        for generation, observations, now in attempts:
            with self.subTest(observations=observations):
                before = primitive(engine)
                with self.assertRaises(ValueError):
                    engine.accept(generation, observations, now)
                # Compare every primitive, including the new FSM, without object identity.
                self.assertEqual(before, primitive(engine))

    def test_generation_and_origin_change_do_not_carry_run_or_old_successor_authority(self):
        for changed in ('generation', 'origin'):
            with self.subTest(changed=changed):
                engine = SportEngine()
                acquire(engine, BASE, 98)
                acquire(engine, BASE + STEP, 110)
                acquire(engine, BASE + 2 * STEP, 210,
                        generation=2 if changed == 'generation' else 1,
                        origin='measured' if changed == 'origin' else 'simulation')
                self.assertIsNone(rolling(engine, engine.last_now)['lastResult'])
        engine = SportEngine()
        acquire(engine, BASE, 98)
        acquire(engine, BASE + STEP, 110)
        # Original pure engine permits an older generation baseline; rolling must not borrow it.
        acquire(engine, BASE, 98, generation=2, now=BASE + STEP)
        acquire(engine, BASE + 2 * STEP, 210, generation=2)
        self.assertIsNone(rolling(engine, engine.last_now)['lastResult'])

    def test_session_reset_clears_result_and_requires_strictly_post_action_rearm(self):
        engine = SportEngine()
        self.assertIsNotNone(ramp(engine)['lastResult'])
        now = engine.last_now + 50_000_000
        engine.reset_session(now)
        self.assertIsNone(rolling(engine, now)['lastResult'])
        acquire(engine, now, 98)
        acquire(engine, now + STEP, 210)
        self.assertIsNone(rolling(engine, engine.last_now)['lastResult'])
        acquire(engine, now + 2 * STEP, 98)
        acquire(engine, now + 3 * STEP, 210)
        self.assertEqual(rolling(engine, engine.last_now)['lastResult']['sessionId'], 2)

    def test_known_stopped_zero_is_real_rearm_but_missing_never_zero(self):
        engine = SportEngine()
        self.assertEqual(rolling(engine, BASE)['phase'], 'waiting')
        self.assertIsNone(rolling(engine, BASE)['lastResult'])
        acquire(engine, BASE, 0)
        self.assertEqual(rolling(engine, BASE)['phase'], 'armed')
        acquire(engine, BASE + STEP, 210)
        self.assertGreater(rolling(engine, engine.last_now)['lastResult']['seconds'], 0)
        self.assertEqual(engine.inputs['speed_kmh'].value, 210)

    def test_timeout_and_return_to_rearm_band_retire_current_run(self):
        engine = SportEngine()
        acquire(engine, BASE, 98)
        acquire(engine, BASE + STEP, 110)
        acquire(engine, BASE + 2 * STEP, 0)
        self.assertEqual(rolling(engine, engine.last_now)['phase'], 'armed')
        self.assertIsNone(rolling(engine, engine.last_now)['lastResult'])
        # Genuine continuous acquired 150km/h for 120s cannot create a result.
        acquire(engine, BASE + 3 * STEP, 150)
        for index in range(1, 602):
            acquire(engine, BASE + (3 + index) * STEP, 150)
        model = rolling(engine, engine.last_now)
        self.assertIsNone(model['lastResult'])
        self.assertNotEqual(model['phase'], 'running')
        self.assertEqual(model['reason'], 'Timeout')

    def test_retained_result_is_historical_copied_bounded_and_never_persisted(self):
        engine = SportEngine()
        result = ramp(engine)['lastResult']
        stale = rolling(engine, engine.last_now + 500_000_000)
        self.assertEqual(stale['lastResult'], result)
        self.assertIsNone(stale['elapsedSeconds'])
        stale['lastResult']['startCrossing']['before']['speedKmh'] = -1
        self.assertEqual(rolling(engine, engine.last_now)['lastResult'], result)
        self.assertEqual(set(engine.timers), {'0-100', '50-80', '0-200', 'quarter-mile'})
        self.assertTrue(all(event.get('timer') != '100-200' for event in engine.history))
        for index in range(20):
            ramp(engine, engine.last_now + STEP)
        self.assertNotIn('history', rolling(engine, engine.last_now))
        self.assertNotIn('history', vars(engine.rolling100_to_200))

    def test_actual_manager_status_and_trip_reset_preserve_independent_result(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            manager = manager_fixture.Tests().quarantine_fixture(folder)
            manager.active = None
            manager.sport = SportSource(folder)
            try:
                now = BASE + 4 * STEP
                ramp(manager.sport.engine)
                with patch('sport_source.host_now_ns', side_effect=lambda: now):
                    result = manager.request({'action': 'status'})['sport']['rolling100To200']['lastResult']
                    self.assertIsNotNone(result)
                    self.assertEqual(manager.request({'action': 'trip_reset'})['sport']['rolling100To200']['lastResult'], result)
                    manager.request({'action': 'sport_session'})
                    self.assertIsNone(manager.request({'action': 'status'})['sport']['rolling100To200']['lastResult'])
            finally:
                manager.shutdown()
            self.assertNotIn('rolling100To200', (folder / 'sport-history.json').read_text())

    def test_actual_socket_worker_terminal_failure_and_replacement_no_live_restore(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            source = SportSource(folder, socket_path=folder / 'rolling.sock')
            sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            source.start()
            try:
                def send(value, sequence):
                    acquired = host_now_ns()
                    sender.sendto(packet(acquired, value=str(value), sequence=sequence), str(source.path))
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        model = source.snapshot()
                        if model['inputs']['speed_kmh']['acquiredNs'] == acquired:
                            return acquired, model
                        time.sleep(.005)
                    self.fail('actual acquisition owner did not publish original sample')
                send(98, 1)
                time.sleep(.01)
                acquired, model = send(110, 2)
                self.assertEqual(model['rolling100To200']['phase'], 'running')
                with source.lock:
                    source.socket.close()
                source.thread.join(timeout=2)
                self.assertFalse(source.thread.is_alive())
                model = source.snapshot()
                self.assertEqual(model['error'], 'Sport acquisition stopped')
                self.assertEqual(model['inputs']['speed_kmh']['availability'], 'disconnected')
                self.assertEqual(model['inputs']['speed_kmh']['acquiredNs'], acquired)
                self.assertIsNone(model['rolling100To200']['currentRun'])
                self.assertIsNone(model['rolling100To200']['lastResult'])
                later = host_now_ns()
                source.ingest(packet(later, value='98', sequence=3), later)
                later = host_now_ns()
                source.ingest(packet(later, value='210', sequence=4), later)
                self.assertEqual(source.snapshot()['rolling100To200']['phase'], 'retired')
            finally:
                sender.close()
                source.close()
            self.assertFalse(source.path.exists())
            replacement = SportSource(folder)
            try:
                self.assertIsNone(replacement.snapshot()['rolling100To200']['lastResult'])
            finally:
                replacement.close()

    def test_historical_result_survives_source_change_and_terminal_withdrawal_with_original_identity(self):
        with tempfile.TemporaryDirectory() as raw:
            source = SportSource(Path(raw).resolve())
            try:
                result = ramp(source.engine)['lastResult']
                now = source.engine.last_now + STEP
                acquire(source.engine, now, 210, generation=2, origin='measured')
                self.assertEqual(rolling(source.engine, now)['lastResult'], result)
                with patch('sport_source.host_now_ns', return_value=now):
                    source._withdraw_inputs()
                    model = source.snapshot()
                    self.assertEqual(source.snapshot()['rolling100To200'], model['rolling100To200'])
                self.assertEqual(model['rolling100To200']['lastResult'], result)
                self.assertEqual(model['rolling100To200']['phase'], 'retired')
                self.assertEqual(model['inputs']['speed_kmh']['availability'], 'disconnected')
                self.assertIsNone(model['rolling100To200']['elapsedSeconds'])
                self.assertEqual(result['sourceGeneration'], 1)
                self.assertEqual(result['origin'], 'simulation')
            finally:
                source.close()

    def test_closed_owner_new_model_cannot_revive_via_late_session_or_acquisition(self):
        with tempfile.TemporaryDirectory() as raw:
            source = SportSource(Path(raw).resolve())
            try:
                now = BASE
                with patch('sport_source.host_now_ns', side_effect=lambda: now):
                    source.close()
                    closed_engine = primitive(source.engine)
                    closed_mark = source.packet_mark
                    closed_rejected = source.rejected
                    with self.assertRaisesRegex(ValueError, '^Sport owner retired$'):
                        source.request({'action': 'sport_session'})
                    self.assertEqual(primitive(source.engine), closed_engine)
                    for sequence, value in enumerate(('98', '210'), 1):
                        now += STEP
                        with self.assertRaisesRegex(ValueError, '^Sport owner retired$'):
                            source.ingest(packet(now, value=value, sequence=sequence), now)
                        self.assertEqual(primitive(source.engine), closed_engine)
                        self.assertEqual(source.packet_mark, closed_mark)
                        self.assertEqual(source.rejected, closed_rejected)
                        self.assertTrue(source.closed)
                    self.assertEqual(source.snapshot()['rolling100To200']['phase'], 'retired')
                    self.assertIsNone(source.snapshot()['rolling100To200']['lastResult'])
                    # Exercise the estimator tombstone behind the public owner guard.
                    # Fresh post-reset dates prevent the acquisition floor from
                    # concealing a revived estimator.
                    source.engine.reset_session(now)
                    for value in (98, 110, 150, 190, 210):
                        now += STEP
                        acquire(source.engine, now, value)
                        self.assertEqual(source.snapshot()['rolling100To200']['phase'], 'retired')
                        self.assertIsNone(source.snapshot()['rolling100To200']['lastResult'])
            finally:
                source.close()


if __name__ == '__main__':
    unittest.main()
