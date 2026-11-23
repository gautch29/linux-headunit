#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Summary estimates from explicitly synthetic calibrated observation fixtures."""
import copy
from fractions import Fraction
import json
import math
from pathlib import Path
import socket
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from sport_telemetry import AccelerationSummary, G, NS, Observation, SportEngine
from sport_source import SportSource, host_now_ns
from sport_telemetry_test import packet

BASE = 10 * NS
METRICS = ('coveredSeconds', 'meanLongitudinalG', 'meanLateralG', 'rmsMagnitudeG')
DATES = ('firstAcquiredNs', 'lastAcquiredNs')


def acquire(engine, offset, vector, *, origin='simulation', generation=1,
            age=500_000_000, delivery=None):
    stamp = BASE + offset
    obs = Observation(vector, stamp, age, origin)
    engine.accept(generation, {'acceleration': obs}, stamp if delivery is None else BASE + delivery)


def summary(engine, now=None):
    return engine.model(engine.last_now if now is None else now)['gMeter']['summary']


def state(engine):
    result = copy.deepcopy(engine.__dict__)
    result['trip'] = copy.deepcopy(engine.trip.__dict__)
    result['trip']['recent'] = copy.deepcopy(engine.trip.recent.__dict__)
    result['acceleration_summary'] = copy.deepcopy(engine.acceleration_summary.__dict__)
    result['rolling100_to_200'] = copy.deepcopy(engine.rolling100_to_200.__dict__)
    return result


def rational_oracle(points):
    """Integrate polynomial coefficients, independent of endpoint-square formulas.

    Vectors are exact rational m/s² fixtures and times are integer nanoseconds.
    Expected conversion uses the decimal contract constant, not imported G.
    """
    gravity = Fraction('9.80665')
    duration = Fraction(0)
    integrals = [Fraction(0), Fraction(0), Fraction(0)]
    for (t0, a), (t1, b) in zip(points, points[1:]):
        seconds = Fraction(t1-t0, 1_000_000_000)
        duration += seconds
        for axis in (0, 1):
            constant = Fraction(a[axis]) / gravity
            slope = (Fraction(b[axis]) - Fraction(a[axis])) / gravity
            integrals[axis] += seconds * (constant + slope / 2)
            # Square the polynomial c+d*s and integrate each coefficient.
            coefficients = (constant * constant, 2 * constant * slope, slope * slope)
            integrals[2] += seconds * sum(value / (degree + 1)
                                           for degree, value in enumerate(coefficients))
    return (float(duration), float(integrals[0] / duration),
            float(integrals[1] / duration), float(integrals[2] / duration))


class SummaryTests(unittest.TestCase):
    def assert_oracle(self, model, points):
        duration, x, y, square = rational_oracle(points)
        self.assertEqual(model['coveredSeconds'], duration)
        self.assertAlmostEqual(model['meanLongitudinalG'], x, delta=1e-12 * max(1, abs(x)))
        self.assertAlmostEqual(model['meanLateralG'], y, delta=1e-12 * max(1, abs(y)))
        self.assertAlmostEqual(model['rmsMagnitudeG']**2, square,
                               delta=1e-12 * max(1, square))

    def test_no_coverage_null_and_real_zero_or_nonzero(self):
        engine = SportEngine()
        empty = engine.model(BASE)['gMeter']['summary']
        for key in (*METRICS, *DATES):
            self.assertIsNone(empty[key])
        self.assertFalse(empty['inputAvailable'])
        for vector in ((0, 0), (G, -2*G)):
            with self.subTest(vector=vector):
                engine = SportEngine()
                acquire(engine, 0, vector, origin='measured')
                first = summary(engine)
                for key in (*METRICS, *DATES):
                    self.assertIsNone(first[key])
                self.assertTrue(first['inputAvailable'])
                acquire(engine, 200_000_000, vector, origin='measured')
                known = summary(engine)
                self.assertEqual(known['coveredSeconds'], .2)
                self.assertEqual(known['meanLongitudinalG'], vector[0]/G)
                self.assertEqual(known['meanLateralG'], vector[1]/G)
                self.assertAlmostEqual(known['rmsMagnitudeG'], math.hypot(*vector)/G)
                self.assertEqual(known['origin'], 'measured')
                self.assertFalse(known['partial'])

    def test_unequal_spacing_independent_polynomial_oracle(self):
        engine = SportEngine()
        gravity = Fraction('9.80665')
        points = [(0, (0, 0)), (100_000_000, (2*gravity, -gravity)),
                  (300_000_000, (-gravity, 3*gravity))]
        for stamp, vector in points:
            acquire(engine, stamp, tuple(float(v) for v in vector))
        model = summary(engine)
        self.assert_oracle(model, points)
        self.assertAlmostEqual(model['meanLongitudinalG'], 2/3)
        self.assertAlmostEqual(model['meanLateralG'], 1/2)
        self.assertAlmostEqual(model['rmsMagnitudeG'], 5/3)
        self.assertEqual(model['firstAcquiredNs'], BASE)
        self.assertEqual(model['lastAcquiredNs'], BASE + 300_000_000)

    def test_signed_reversal_rotation_and_axis_transform(self):
        for endpoints, square in ((((1, 0), (-1, 0)), Fraction(1, 3)),
                                  (((1, 0), (0, 1)), Fraction(2, 3))):
            for quarter_turn in (False, True):
                engine = SportEngine()
                points = []
                for stamp, (x, y) in zip((0, 200_000_000), endpoints):
                    if quarter_turn:
                        x, y = -y, x
                    vector = (Fraction('9.80665')*x, Fraction('9.80665')*y)
                    points.append((stamp, vector))
                    acquire(engine, stamp, tuple(float(v) for v in vector))
                model = summary(engine)
                self.assert_oracle(model, points)
                self.assertAlmostEqual(model['rmsMagnitudeG']**2, float(square))
                self.assertLess(model['rmsMagnitudeG'], 1)

    def test_subdivision_invariance_and_cancellation(self):
        outputs = []
        for fractions in ((0, 1), (0, Fraction(1, 4), Fraction(3, 4), 1),
                          tuple(Fraction(i, 20) for i in range(21))):
            engine = SportEngine()
            points = []
            for fraction in fractions:
                stamp = int(fraction * 200_000_000)
                vector = (Fraction(80) - 160*fraction, Fraction(-20) + 50*fraction)
                points.append((stamp, vector))
                acquire(engine, stamp, tuple(float(v) for v in vector))
            model = summary(engine)
            self.assert_oracle(model, points)
            outputs.append(model)
        for key in METRICS:
            for model in outputs[1:]:
                self.assertAlmostEqual(model[key], outputs[0][key], delta=1e-12)

    def test_many_alternating_tiny_intervals_near_host_clock_bound(self):
        engine = SportEngine()
        start = 2**63 - 10000
        points = []
        for index in range(1001):
            stamp = start + index
            vector = (Fraction(100 if index % 2 else -100), Fraction(100))
            points.append((stamp, vector))
            engine.accept(2**64-1, {'acceleration': Observation(tuple(float(v) for v in vector), stamp)}, stamp)
        model = summary(engine)
        self.assert_oracle(model, points)
        self.assertEqual(model['sourceGeneration'], 2**64-1)
        self.assertEqual(model['firstAcquiredNs'], start)
        self.assertEqual(model['lastAcquiredNs'], start + 1000)
        json.dumps(model, allow_nan=False)

    def test_polling_heartbeat_and_other_signals_add_no_coverage(self):
        engine = SportEngine()
        acquire(engine, 0, (G, 0))
        acquire(engine, 100_000_000, (G, 0))
        before = summary(engine)
        for offset in (110_000_000, 120_000_000, 130_000_000):
            engine.accept(1, {'acceleration': engine.inputs['acceleration'],
                              'speed_kmh': Observation(0, BASE+offset),
                              'gps_position': Observation((48, 2, 1), BASE+offset)}, BASE+offset)
            self.assertEqual(summary(engine), before)
        stale = summary(engine, BASE + 600_000_000)
        self.assertFalse(stale['inputAvailable'])
        self.assertTrue(stale['partial'])
        for key in (*METRICS, *DATES):
            self.assertEqual(stale[key], before[key])

    def test_original_acquisition_delivery_and_conservative_expiry(self):
        for late, expected in ((400_000_000, .2), (600_000_000, None)):
            engine = SportEngine()
            acquire(engine, 0, (G, 0), delivery=300_000_000)
            acquire(engine, 200_000_000, (G, 0), delivery=late)
            model = summary(engine)
            self.assertEqual(model['coveredSeconds'], expected)
            self.assertTrue(model['inputAvailable'])
            if expected is not None:
                self.assertEqual(model['lastAcquiredNs'], BASE+200_000_000)
            else:
                self.assertTrue(model['partial'])
        queued = SportEngine()
        acquire(queued, 0, (G, 0), delivery=NS)
        self.assertIsNone(summary(queued)['coveredSeconds'])

    def test_maximum_gap_equality_and_strict_age_expiry(self):
        for step, age, known in ((249_999_999, NS, True), (250_000_000, NS, True),
                                 (250_000_001, NS, False), (99_999_999, 100_000_000, True),
                                 (100_000_000, 100_000_000, False),
                                 (100_000_001, 100_000_000, False)):
            with self.subTest(step=step, age=age):
                engine = SportEngine()
                acquire(engine, 0, (0, 0), age=age)
                acquire(engine, step, (0, 0), age=age)
                model = summary(engine)
                self.assertEqual(model['coveredSeconds'], step/NS if known else None)
                self.assertEqual(model['partial'], not known)

    def test_expiry_without_poll_is_same_as_observed_expiry(self):
        results = []
        for poll in (False, True):
            engine = SportEngine()
            acquire(engine, 0, (G, 0), age=100_000_000)
            acquire(engine, 50_000_000, (G, 0), age=100_000_000)
            if poll:
                summary(engine, BASE+150_000_000)
            acquire(engine, 160_000_000, (G, 0), age=100_000_000)
            acquire(engine, 210_000_000, (G, 0), age=100_000_000)
            results.append(summary(engine))
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0]['coveredSeconds'], .1)
        self.assertTrue(results[0]['partial'])

    def test_withdrawal_retains_coverage_and_needs_new_baseline(self):
        for availability in ('unavailable', 'stale', 'disconnected'):
            engine = SportEngine()
            acquire(engine, 0, (G, 0))
            acquire(engine, 100_000_000, (G, 0))
            before = summary(engine)
            withdrawal = 110_000_000 if availability == 'unavailable' else 100_000_000
            engine.accept(1, {'acceleration': Observation(None, BASE+withdrawal,
                          origin='simulation', availability=availability)}, BASE+110_000_000)
            withdrawn = summary(engine)
            self.assertFalse(withdrawn['inputAvailable'])
            self.assertEqual(withdrawn['phase'], 'waiting')
            for key in (*METRICS, *DATES):
                self.assertEqual(withdrawn[key], before[key])
            with self.assertRaises(ValueError):
                acquire(engine, withdrawal, (G, 0), delivery=120_000_000)
            acquire(engine, 130_000_000, (G, 0))
            self.assertEqual(summary(engine)['coveredSeconds'], .1)
            acquire(engine, 180_000_000, (G, 0))
            self.assertEqual(summary(engine)['coveredSeconds'], .15)

    def test_generation_empty_origin_and_session_never_bridge(self):
        engine = SportEngine()
        acquire(engine, 0, (G, 0))
        acquire(engine, 100_000_000, (G, 0))
        engine.accept(2, {}, BASE+110_000_000)
        self.assertIsNone(summary(engine)['coveredSeconds'])
        # Pure admitted engine permits earlier fresh dates in a newer generation.
        acquire(engine, 50_000_000, (G, 0), generation=2, delivery=120_000_000)
        self.assertIsNone(summary(engine)['coveredSeconds'])
        acquire(engine, 150_000_000, (G, 0), generation=2)
        self.assertEqual(summary(engine)['coveredSeconds'], .1)
        for index, origin in enumerate(('measured', 'calculated', 'simulation')):
            stamp = 200_000_000 + index*100_000_000
            acquire(engine, stamp, (0, G), generation=2, origin=origin)
            model = summary(engine)
            self.assertIsNone(model['coveredSeconds'])
            self.assertEqual(model['origin'], origin)
        engine.reset_session(BASE+410_000_000)
        self.assertTrue(summary(engine)['inputAvailable'])
        acquire(engine, 405_000_000, (G, 0), generation=3, delivery=420_000_000)
        self.assertIsNone(summary(engine)['origin'])
        acquire(engine, 430_000_000, (G, 0), generation=3)
        self.assertIsNone(summary(engine)['coveredSeconds'])
        acquire(engine, 480_000_000, (G, 0), generation=3)
        self.assertEqual(summary(engine)['coveredSeconds'], .05)

    def test_line_and_trip_reset_independence_and_summary_copy(self):
        engine = SportEngine()
        acquire(engine, 0, (G, -G))
        acquire(engine, 100_000_000, (G, -G))
        before = summary(engine)
        engine.reset_trip(BASE+110_000_000)
        engine.set_line([48, 1.9995], [48, 2.0005])
        after = summary(engine)
        self.assertEqual(after, before)
        after['coveredSeconds'] = 99
        self.assertEqual(summary(engine), before)

    def test_actual_span_cap_exact_equality_and_over_limit_freeze(self):
        engine = SportEngine()
        cap = AccelerationSummary.MAXIMUM_SPAN_NS
        acquire(engine, 0, (0, 0))
        acquire(engine, cap-100_000_000, (G, 0))
        acquire(engine, cap, (G, 0))
        exact = summary(engine)
        self.assertEqual(exact['maximumSpanNs'], 86400*NS)
        self.assertEqual(exact['coveredSeconds'], .1)
        self.assertFalse(exact['limitReached'])
        acquire(engine, cap+1, (2*G, 0))
        limited = summary(engine)
        self.assertTrue(limited['limitReached'])
        self.assertEqual(limited['phase'], 'limited')
        for key in (*METRICS, *DATES):
            self.assertEqual(limited[key], exact[key])
        # Summary limiting must not stop original current g or peak admission.
        self.assertEqual(engine.model(engine.last_now)['gMeter']['longitudinalG'], 2)
        self.assertEqual(engine.peaks['longitudinalPositiveG'], 2)
        acquire(engine, cap+50_000_000, (0, G), origin='measured')
        self.assertFalse(summary(engine)['limitReached'])
        acquire(engine, cap+100_000_000, (0, G), origin='measured')
        self.assertEqual(summary(engine)['coveredSeconds'], .05)

    def test_numeric_guard_stages_whole_interval_without_owner_rejection(self):
        engine = SportEngine()
        acquire(engine, 0, (G, 0))
        acquire(engine, 100_000_000, (G, 0))
        before = summary(engine)
        for bad in (math.inf, math.nan, -1e9):
            with self.subTest(bad=bad):
                candidate = copy.deepcopy(engine)
                # Explicit helper arithmetic fault injection, not an observation.
                with patch.object(candidate.acceleration_summary, '_add', return_value=(bad, 0)):
                    acquire(candidate, 200_000_000, (2*G, 0))
                after = summary(candidate)
                self.assertTrue(after['limitReached'])
                for key in (*METRICS, *DATES):
                    self.assertEqual(after[key], before[key])
                self.assertEqual(candidate.inputs['acceleration'].acquired_ns, BASE+200_000_000)

    def test_summary_clock_domain_never_changes_original_admission(self):
        engine = SportEngine()
        stamp = 2**63
        engine.accept(1, {'acceleration': Observation((G, 0), stamp)}, stamp)
        model = engine.model(stamp)
        self.assertEqual(model['gMeter']['longitudinalG'], 1)
        self.assertTrue(model['gMeter']['summary']['limitReached'])
        self.assertIsNone(model['gMeter']['summary']['coveredSeconds'])

    def test_whole_batch_rejection_leaves_every_owner_field_unchanged(self):
        engine = SportEngine()
        acquire(engine, 0, (G, 0))
        acquire(engine, 100_000_000, (G, 0))
        before = state(engine)
        invalid = [Observation((math.nan, 0), BASE+200_000_000),
                   Observation((math.inf, 0), BASE+200_000_000),
                   Observation((101, 0), BASE+200_000_000),
                   Observation((True, 0), BASE+200_000_000),
                   Observation((0,), BASE+200_000_000),
                   Observation((G, 0), BASE+99_000_000),
                   Observation((0, 0), BASE+100_000_000, origin='simulation'),
                   Observation((G, 0), BASE+100_000_000, origin='measured'),
                   Observation((G, 0), BASE+200_000_000, maximum_age_ns=NS),
                   Observation((G, 0), BASE+300_000_000)]
        for obs in invalid:
            with self.subTest(obs=obs):
                with self.assertRaises(ValueError):
                    engine.accept(1, {'acceleration': obs}, BASE+200_000_000)
                self.assertEqual(state(engine), before)
        for generation in (1, 2):
            with self.assertRaises(ValueError):
                engine.accept(generation, {'acceleration': Observation((0, 0), BASE+200_000_000),
                              'gps_position': Observation((91, 2, 1), BASE+200_000_000)},
                              BASE+200_000_000)
            self.assertEqual(state(engine), before)
        with self.assertRaises(ValueError):
            engine.model(BASE+99_999_999)
        self.assertEqual(state(engine), before)
        acquire(engine, 200_000_000, (G, 0))
        self.assertEqual(summary(engine)['coveredSeconds'], .2)

    def test_exact_interface_keys_and_finite_json(self):
        engine = SportEngine()
        model = engine.model(BASE)['gMeter']['summary']
        expected = {'version', 'authority', 'method', 'persistence', 'sessionId',
                    'sourceGeneration', 'origin', 'maximumGapNs', 'maximumSpanNs',
                    'phase', 'reason', 'inputAvailable', 'firstAcquiredNs', 'lastAcquiredNs',
                    *METRICS, 'partial', 'limitReached'}
        self.assertEqual(set(model), expected)
        self.assertEqual(model['maximumGapNs'], 250_000_000)
        self.assertEqual(model['persistence'], 'process-memory')
        json.dumps(model, allow_nan=False)


class SourceSummaryTests(unittest.TestCase):
    def test_packet_rejection_sequence_and_history_schema_unchanged(self):
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(folder)
            source.ingest(packet(BASE, sequence=1), BASE)
            source.ingest(packet(BASE+100_000_000, sequence=2), BASE+100_000_000)
            before = state(source.engine)
            mark = source.packet_mark
            malformed = packet(BASE+200_000_000, sequence=3).replace(b'gps_position=48/2/1', b'gps_position=91/2/1')
            with self.assertRaises(ValueError):
                source.ingest(malformed, BASE+200_000_000)
            self.assertEqual(source.packet_mark, mark)
            self.assertEqual(state(source.engine), before)
            source.ingest(packet(BASE+200_000_000, sequence=3), BASE+200_000_000)
            self.assertEqual(summary(source.engine)['coveredSeconds'], .2)
            source.engine.reset_session(BASE+210_000_000)
            record = {'version': 1, 'sessionId': source.engine.session_id,
                      'history': list(source.engine.history), 'line': source.engine.line}
            source._save(record)
            self.assertNotIn('summary', source.history_path.read_text())
            restored = SportSource(folder)
            self.assertEqual(list(restored.engine.history), list(source.engine.history))
            self.assertIsNone(restored.engine.model(BASE+220_000_000)['gMeter']['summary']['coveredSeconds'])

    def test_actual_private_worker_failure_and_normal_close_retirement(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            source = SportSource(folder, socket_path=folder/'summary.sock')
            sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            source.start()
            try:
                now = host_now_ns()
                sender.sendto(packet(now, sequence=1), str(source.path))
                sender.sendto(packet(now+1, sequence=2), str(source.path))
                deadline = time.monotonic() + 2
                while source.snapshot()['gMeter']['summary']['coveredSeconds'] is None and time.monotonic() < deadline:
                    time.sleep(.005)
                before = source.snapshot()['gMeter']['summary']
                self.assertEqual(before['coveredSeconds'], 1/NS)
                with source.lock:
                    source.socket.close()
                source.thread.join(timeout=2)
                self.assertFalse(source.thread.is_alive())
                failed = source.snapshot()
                self.assertEqual(failed['error'], 'Sport acquisition stopped')
                self.assertFalse(failed['gMeter']['summary']['inputAvailable'])
                self.assertEqual(failed['inputs']['acceleration']['availability'], 'disconnected')
                for key in (*METRICS, *DATES):
                    self.assertEqual(failed['gMeter']['summary'][key], before[key])
                source.close()
                self.assertFalse(source.path.exists())
                self.assertIsNone(source.snapshot()['gMeter']['summary']['coveredSeconds'])
            finally:
                sender.close()
                source.close()


if __name__ == '__main__':
    unittest.main()
