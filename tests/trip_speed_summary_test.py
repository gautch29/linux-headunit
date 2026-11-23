#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Analytical software-speed estimates and actual local owner/storage boundaries."""
import copy
from decimal import Decimal, localcontext
import json
import math
import os
from pathlib import Path
import socket
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sport_telemetry import Observation, SportEngine, TripStatistics, NS
from sport_source import SportSource, host_now_ns
from phone_manager import Manager
from trip_history import STAT_KEYS, encode, envelope, restore, validate_history
from trip_statistics_test import BASE, STEP, acquire, trip
from sport_telemetry_test import packet
from trip_history_test import wait

KEYS = ('timeWeightedRmsSpeedKmh', 'speedStandardDeviationKmh')


def summary(model):
    return tuple(model[key] for key in KEYS)


def polynomial_integral_reference(points):
    # Independent high-precision integral of the linear polynomials. Raw square
    # subtraction is safe ONLY in this 90-digit oracle, never in production.
    with localcontext() as context:
        context.prec = 90
        area = square_area = Decimal(0)
        duration = points[-1][0] - points[0][0]
        for (t0, v0), (t1, v1) in zip(points, points[1:]):
            a, b = Decimal(v0), Decimal(v1)
            dt = Decimal(t1 - t0)
            area += dt * (a + b) / 2
            square_area += dt * (a * a + a * b + b * b) / 3
        mean = area / duration
        second = square_area / duration
        return float(second.sqrt()), float((second - mean * mean).sqrt())


class SummaryTests(unittest.TestCase):
    def assert_summary(self, model, rms, deviation, relative=2e-13):
        for actual, wanted in zip(summary(model), (rms, deviation)):
            if wanted is None:
                self.assertIsNone(actual)
            elif wanted == 0:
                self.assertEqual(actual, 0)
            else:
                self.assertTrue(math.isfinite(actual))
                self.assertGreaterEqual(actual, 0)
                self.assertAlmostEqual(actual, wanted, delta=abs(wanted) * relative)
        self.assertEqual(model['authority'], 'software-speed-estimate')
        json.dumps(model, allow_nan=False)

    def journey(self, points):
        engine = SportEngine()
        for stamp, speed in points:
            acquire(engine, BASE + stamp, speed)
        return engine, trip(engine, engine.last_now)

    def test_missing_single_and_known_stopped_interval(self):
        engine = SportEngine()
        self.assert_summary(trip(engine, BASE), None, None)
        acquire(engine, BASE, 0)
        self.assert_summary(trip(engine, BASE), None, None)
        acquire(engine, BASE + STEP, 0)
        model = trip(engine, engine.last_now)
        self.assert_summary(model, 0, 0)
        self.assertEqual(model['coveredSeconds'], .2)
        self.assertIsNone(model['movingAverageSpeedKmh'])

    def test_constant_speed_including_threshold_and_source_upper_bound(self):
        for speed in (.1, .5, 36, math.nextafter(300, 0), 300):
            with self.subTest(speed=speed):
                engine, model = self.journey([(index * STEP, speed) for index in range(1001)])
                self.assert_summary(model, speed, 0)
                self.assertEqual(model['coveredSeconds'], 200)
                self.assertEqual(model['peak']['acquiredNs'], BASE)

    def test_full_range_linear_rise_fall_and_reversal(self):
        for speeds in ((0, 300), (300, 0), (0, 300, 0)):
            with self.subTest(speeds=speeds):
                _, model = self.journey([(i * STEP, value) for i, value in enumerate(speeds)])
                self.assert_summary(model, math.sqrt(30000), math.sqrt(7500))
                self.assertAlmostEqual(model['averageSpeedKmh'], 150)

    def test_uneven_original_acquisition_weights_not_point_sample_mean(self):
        _, model = self.journey([(0, 0), (20_000_000, 120), (200_000_000, 60)])
        # First ramp covers 20ms, second 180ms: E[v]=87, E[v^2]=8040.
        self.assert_summary(model, math.sqrt(8040), math.sqrt(471))
        self.assertAlmostEqual(model['averageSpeedKmh'], 87)

    def test_multisegment_plateaus_and_ramps_include_stops(self):
        _, model = self.journey([(0, 0), (100_000_000, 0), (150_000_000, 120),
                                 (350_000_000, 120), (500_000_000, 0)])
        self.assert_summary(model, math.sqrt(7680), math.sqrt(2496))
        self.assertAlmostEqual(model['averageSpeedKmh'], 72)
        self.assertEqual(model['coveredSeconds'], .5)
        self.assertLess(model['movingSeconds'], model['coveredSeconds'])

    def test_linear_interval_split_invariance_with_uneven_parts(self):
        for start, end in ((0, 300), (300, 0), (.25, 299.75)):
            stamps = [0, 1, 999, 1_000_000, 70_000_000, 239_999_999, 240_000_000]
            speeds = [start + (end - start) * t / stamps[-1] for t in stamps]
            _, whole = self.journey([(0, start), (stamps[-1], end)])
            _, split = self.journey(list(zip(stamps, speeds)))
            self.assert_summary(split, *summary(whole))
            self.assertEqual(split['coveredSeconds'], whole['coveredSeconds'])

    def test_near_300_adjacent_floats_have_positive_real_deviation(self):
        upper = 300.0
        lower = math.nextafter(upper, 0)
        engine, model = self.journey([(i * 1_000_000, lower if i % 2 == 0 else upper)
                                      for i in range(10001)])
        self.assert_summary(model, upper, (upper - lower) / math.sqrt(12), relative=1e-12)
        self.assertEqual(model['coveredSeconds'], 10)
        self.assertLess(summary(model)[1], 1e-12)

    def test_near_constant_uneven_segments_match_high_precision_integrals(self):
        for baseline, delta in ((299.999999, 1e-8), (120, math.ulp(120)), (1e-9, 1e-12)):
            points = [(0, baseline), (1, baseline + delta), (19_000_000, baseline),
                      (200_000_000, baseline + 2 * delta), (230_000_000, baseline - delta)]
            _, model = self.journey(points)
            self.assert_summary(model, *polynomial_integral_reference(points), relative=2e-12)

    def test_delayed_delivery_heartbeats_and_polling_never_add_weight(self):
        engine = SportEngine()
        acquire(engine, BASE, 30, now=BASE + 300_000_000)
        acquire(engine, BASE + STEP, 120, now=BASE + 600_000_000)
        before = trip(engine, engine.last_now)
        self.assert_summary(before, math.sqrt(6300), math.sqrt(675))
        for index in range(100):
            now = BASE + 600_000_000 + index * 1_000_000
            engine.accept(1, {}, now)
            acquire(engine, BASE + STEP, 120, now=now)
            self.assertEqual(summary(trip(engine, now)), summary(before))
        self.assertEqual((before['firstAcquiredNs'], before['lastAcquiredNs']), (BASE, BASE + STEP))

    def test_expiry_withdrawal_and_long_gaps_do_not_bridge(self):
        for interruption in ('expiry', 'withdrawal', 'gap'):
            with self.subTest(interruption=interruption):
                engine, before = self.journey([(0, 30), (STEP, 30)])
                if interruption == 'expiry':
                    trip(engine, BASE + STEP + 500_000_000)
                elif interruption == 'withdrawal':
                    obs = engine.inputs['speed_kmh']
                    engine.accept(1, {'speed_kmh': Observation(None, obs.acquired_ns, origin=obs.origin,
                                                             availability='disconnected')}, engine.last_now + 1)
                    with self.assertRaises(ValueError):
                        engine.accept(1, {'speed_kmh': obs}, engine.last_now + 1)
                acquire(engine, BASE + NS, 120)
                self.assertEqual(summary(trip(engine, engine.last_now)), summary(before))
                acquire(engine, BASE + NS + STEP, 120)
                model = trip(engine, engine.last_now)
                self.assert_summary(model, math.sqrt(7650), 45)
                self.assertEqual(model['coveredSeconds'], .4)
                self.assertTrue(model['partial'])
                self.assertAlmostEqual(model['coverageRatio'], 1 / 3)

    def test_exact_age_and_expired_queued_intervals_are_unavailable(self):
        for age, delay in ((STEP, 0), (500_000_000, 500_000_000)):
            engine = SportEngine()
            acquire(engine, BASE, 0, age=age)
            acquire(engine, BASE + STEP, 300, age=age, now=BASE + STEP + delay)
            self.assert_summary(trip(engine, engine.last_now), None, None)

    def test_atomic_rejection_keeps_complete_old_state_and_valid_recovery(self):
        engine, before = self.journey([(0, 0), (STEP, 120)])
        invalid = [Observation(math.nan, BASE + 2 * STEP), Observation(-1, BASE + 2 * STEP),
                   Observation(301, BASE + 2 * STEP), Observation(1, BASE),
                   Observation(1, BASE + STEP), Observation(1, BASE + 2 * STEP, maximum_age_ns=NS),
                   Observation(1, BASE + 3 * STEP)]
        batches = [{'speed_kmh': obs} for obs in invalid]
        batches.append({'speed_kmh': Observation(300, BASE + 2 * STEP),
                        'gps_position': Observation((100, 2, 1), BASE + 2 * STEP)})
        for batch in batches:
            with self.assertRaises(ValueError):
                engine.accept(1, batch, BASE + 2 * STEP)
            self.assertEqual(trip(engine, engine.last_now), before)
        acquire(engine, BASE + 2 * STEP, 0)
        self.assert_summary(trip(engine, engine.last_now), math.sqrt(4800), math.sqrt(1200))

    def test_generation_origin_reset_retirement_clear_live_without_archiving_summary(self):
        for transition in ('generation', 'older_generation_baseline', 'origin', 'reset', 'retire'):
            engine, _ = self.journey([(0, 30), (STEP, 120)])
            if transition == 'generation':
                acquire(engine, BASE + 2 * STEP, 60, generation=2)
            elif transition == 'older_generation_baseline':
                acquire(engine, BASE + 100_000_000, 60, generation=2, now=BASE + 250_000_000)
            elif transition == 'origin':
                acquire(engine, BASE + 2 * STEP, 60, origin='measured')
            elif transition == 'reset':
                engine.reset_trip(BASE + 2 * STEP)
                acquire(engine, BASE + 300_000_000, 300, now=BASE + 2 * STEP)
            else:
                engine.trip.retire(BASE + 2 * STEP)
            model = trip(engine, max(engine.last_now, BASE + 2 * STEP))
            self.assert_summary(model, None, None)
            self.assertEqual(len(model['history']), 1)
            self.assertEqual(set(model['history'][0]), STAT_KEYS)
            self.assertTrue(all(key not in model['history'][0] for key in KEYS))

    def test_reset_cutoff_survives_new_source_and_session_reset_is_independent(self):
        engine, _ = self.journey([(0, 30), (STEP, 120)])
        engine.reset_trip(BASE + 2 * STEP)
        acquire(engine, BASE + 300_000_000, 300, generation=2, now=BASE + 2 * STEP + 1)
        acquire(engine, BASE + 2 * STEP, 300, generation=2, now=BASE + 2 * STEP + 2)
        self.assert_summary(trip(engine, engine.last_now), None, None)
        acquire(engine, BASE + 3 * STEP, 36, generation=2)
        acquire(engine, BASE + 4 * STEP, 36, generation=2)
        before = trip(engine, engine.last_now)
        engine.reset_session(BASE + 4 * STEP + 1)
        self.assertEqual(summary(trip(engine, engine.last_now)), summary(before))
        self.assert_summary(before, 36, 0)

    def test_limits_refuse_whole_segment_and_reset_needs_new_coverage(self):
        for name, value in (('MAXIMUM_DURATION_NS', 2 * STEP), ('MAXIMUM_DISTANCE_KM', .004)):
            with self.subTest(limit=name), patch.object(TripStatistics, name, value):
                engine, before = self.journey([(0, 36), (STEP, 36), (2 * STEP, 36)])
                acquire(engine, BASE + 3 * STEP, 300)
                limited = trip(engine, engine.last_now)
                self.assertTrue(limited['limitReached'])
                self.assertEqual(summary(limited), summary(before))
                acquire(engine, BASE + 4 * STEP, 300)
                self.assertEqual(summary(trip(engine, engine.last_now)), summary(before))
                engine.reset_trip(engine.last_now + 1)
                acquire(engine, BASE + 5 * STEP, 36)
                self.assert_summary(trip(engine, engine.last_now), None, None)
                acquire(engine, BASE + 6 * STEP, 36)
                self.assert_summary(trip(engine, engine.last_now), 36, 0)

    def test_actual_socket_manager_copy_rejected_packet_worker_failure_and_reap(self):
        class Phones:
            def phones(self):
                return []
            def media(self, phone):
                return {'available': False}
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            display = folder / 'display.conf'
            display.write_text('explicit manager geometry fixture')
            config = folder / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(folder / 'pairs'),
                                          'display_config': str(display), 'sport_telemetry': True}))
            manager = Manager(config, folder, display, (os.getuid(), os.getgid()), bluez=Phones())
            manager.sport.close()
            source = manager.sport = SportSource(folder, socket_path=folder / 'speed.sock')
            source.start()
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
                    first = host_now_ns()
                    sender.sendto(packet(first, value='30'), str(source.path))
                    wait(lambda: source.snapshot()['trip']['firstAcquiredNs'] == first)
                    time.sleep(.02)
                    second = host_now_ns()
                    sender.sendto(packet(second, sequence=2, value='120'), str(source.path))
                    wait(lambda: source.snapshot()['trip']['lastAcquiredNs'] == second)
                    model = manager.request({'action': 'status'})['sport']['trip']
                    self.assert_summary(model, math.sqrt(6300), math.sqrt(675))
                    self.assertEqual(model['lastAcquiredNs'], second)
                    for key in KEYS:
                        model[key] = -999
                    self.assert_summary(source.snapshot()['trip'], math.sqrt(6300), math.sqrt(675))
                    sender.sendto(packet(second + 1, sequence=3, value='300', gps='100/2/1'), str(source.path))
                    wait(lambda: source.snapshot()['rejectedPackets'] == 1)
                    self.assertEqual(source.packet_mark[2], 2)
                    self.assert_summary(source.snapshot()['trip'], math.sqrt(6300), math.sqrt(675))
                    with source.lock:
                        source.socket.close()
                    source.thread.join(timeout=2)
                self.assertFalse(source.thread.is_alive())
                retired = source.snapshot()
                self.assertEqual(retired['error'], 'Sport acquisition stopped')
                self.assertEqual(retired['trip']['phase'], 'waiting')
                self.assertIsNone(retired['inputs']['speed_kmh']['value'])
                self.assert_summary(retired['trip'], math.sqrt(6300), math.sqrt(675))
            finally:
                manager.shutdown()
            self.assertFalse(source.path.exists())
            self.assertFalse(source.thread.is_alive())
            self.assert_summary(source.snapshot()['trip'], math.sqrt(6300), math.sqrt(675))
            restarted = SportSource(folder)
            try:
                self.assert_summary(restarted.snapshot()['trip'], None, None)
            finally:
                restarted.close()

    def test_private_writer_export_restore_never_reconstructs_live_summary(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            source = SportSource(folder, trip_history=True)
            source.start()
            try:
                wait(lambda: source.snapshot()['trip']['storage']['state'] == 'ready')
                with source.lock:
                    now = host_now_ns()
                    acquire(source.engine, now - 300_000_000, 0, now=now)
                    acquire(source.engine, now - 100_000_000, 120, now=now)
                self.assert_summary(source.snapshot()['trip'], math.sqrt(4800), math.sqrt(1200))
                reset = source.request({'action': 'trip_reset'})
                self.assert_summary(reset['trip'], None, None)
                records = reset['trip']['history']
                validate_history(records)
                for key in KEYS:
                    incompatible = copy.deepcopy(records)
                    incompatible[0][key] = 0
                    with self.assertRaises(ValueError):
                        validate_history(incompatible)
                exported = envelope(records)
                self.assertEqual(restore(exported), records)
                wait(lambda: not source.snapshot()['trip']['storage']['pending'])
            finally:
                source.close()
            self.assertFalse(source.thread.is_alive())
            self.assertTrue(source.storage.reaped)
            original = (folder / 'trip-history.json').read_bytes()
            self.assertEqual(restore(original), records)
            restarted = SportSource(folder, trip_history=True)
            restarted.start()
            try:
                wait(lambda: restarted.snapshot()['trip']['storage']['state'] == 'ready')
                model = restarted.snapshot()['trip']
                self.assertEqual(encode(model['history']), encode(records))
                self.assertEqual((folder / 'trip-history.json').read_bytes(), original)
                self.assert_summary(model, None, None)
            finally:
                restarted.close()
            self.assertTrue(restarted.storage.reaped)
            self.assertFalse(restarted.thread.is_alive())


if __name__ == '__main__':
    unittest.main()
