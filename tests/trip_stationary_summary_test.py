#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Covered low-speed estimates; simulated values and actual local owner paths."""
import copy
from fractions import Fraction
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

KEYS = ('coveredStationarySeconds', 'longestContinuousStationarySeconds')


def durations(model):
    return tuple(model[key] for key in KEYS)


def interval_union_reference(points):
    # Exact rational geometry, independent of production's moving accumulator:
    # clip linear polynomials to v <= 1/2 and union their dated closed ranges.
    ranges = []
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        a, b = Fraction(v0), Fraction(v1)
        if min(a, b) > Fraction(1, 2):
            continue
        start, end = Fraction(t0), Fraction(t1)
        if max(a, b) > Fraction(1, 2):
            crossing = t0 + (t1 - t0) * (Fraction(1, 2) - a) / (b - a)
            if a < b:
                end = crossing
            else:
                start = crossing
        if ranges and ranges[-1][1] == start:
            ranges[-1] = (ranges[-1][0], end)
        else:
            ranges.append((start, end))
    lengths = [end - start for start, end in ranges]
    return float(sum(lengths) / NS), float(max(lengths, default=0) / NS)


class StationaryTests(unittest.TestCase):
    def assert_durations(self, model, covered, longest):
        for actual, expected in zip(durations(model), (covered, longest)):
            if expected is None:
                self.assertIsNone(actual)
            else:
                self.assertTrue(math.isfinite(actual))
                self.assertGreaterEqual(actual, 0)
                self.assertAlmostEqual(actual, expected, delta=1e-12 * max(1, expected))
        if covered is not None:
            self.assertLessEqual(longest, covered + 1e-12)
            self.assertAlmostEqual(model['coveredStationarySeconds'] + model['movingSeconds'],
                                   model['coveredSeconds'], delta=1e-12 * max(1, model['coveredSeconds']))
        self.assertEqual(model['authority'], 'software-speed-estimate')
        json.dumps(model, allow_nan=False)

    def journey(self, points):
        engine = SportEngine()
        for stamp, speed in points:
            acquire(engine, BASE + stamp, speed)
        return engine, trip(engine, engine.last_now)

    def test_missing_single_and_genuine_zero_coverage(self):
        engine = SportEngine()
        self.assert_durations(trip(engine, BASE), None, None)
        acquire(engine, BASE, 0)
        self.assert_durations(trip(engine, BASE), None, None)
        acquire(engine, BASE + STEP, 0)
        model = trip(engine, engine.last_now)
        self.assert_durations(model, .2, .2)
        self.assertEqual(model['distanceKm'], 0)
        self.assertEqual(model['peak']['acquiredNs'], BASE)

    def test_constant_threshold_neighbours_and_admitted_upper_bound(self):
        for speed in (0, .1, math.nextafter(.5, 0), .5, math.nextafter(.5, 1), 36, 300):
            with self.subTest(speed=speed):
                _, model = self.journey([(i * STEP, speed) for i in range(101)])
                expected = 20 if speed <= .5 else 0
                self.assert_durations(model, expected, expected)

    def test_rising_falling_crossing_prefix_suffix_and_reversal(self):
        for speeds, expected in (((0, 2), (.05, .05)), ((2, 0), (.05, .05)),
                                 ((0, 2, 0), (.1, .05)), ((2, 0, 2), (.1, .1)),
                                 ((0, 1, 0, 0), (.4, .3))):
            with self.subTest(speeds=speeds):
                _, model = self.journey([(i * STEP, v) for i, v in enumerate(speeds)])
                self.assert_durations(model, *expected)

    def test_threshold_contact_has_no_duration_and_plateau_joins(self):
        for speeds, expected in (((1, .5, 1), (0, 0)), ((0, .5, 0), (.4, .4)),
                                 ((1, .5, .5, 1), (.2, .2)),
                                 ((0, .5, 1, .5, 0), (.4, .2))):
            _, model = self.journey([(i * STEP, v) for i, v in enumerate(speeds)])
            self.assert_durations(model, *expected)

    def test_nonuniform_durations_use_original_time_not_sample_counts(self):
        points = [(0, 0), (40_000_000, 2), (140_000_000, 0),
                  (340_000_000, 0), (500_000_000, 1), (600_000_000, 0)]
        _, model = self.journey(points)
        # Disjoint covered ranges [0,10ms], [115,420ms], [550,600ms].
        self.assert_durations(model, .365, .305)
        self.assert_durations(model, *interval_union_reference(points))

    def test_linear_split_invariance_with_nonuniform_threshold_points(self):
        for a, b in ((0, 2), (2, 0), (.25, .75), (.75, .25)):
            stamps = [0, 1, 999, 60_000_000, 120_000_000, 239_999_999, 240_000_000]
            points = [(t, a + (b - a) * t / stamps[-1]) for t in stamps]
            _, split = self.journey(points)
            _, whole = self.journey([(0, a), (stamps[-1], b)])
            self.assert_durations(split, *durations(whole))

    def test_many_segments_match_independent_rational_interval_union(self):
        points = [(0, 0)]
        for i in range(1, 1001):
            points.append((points[-1][0] + (i * 7919 % 200_000_000) + 1,
                           (i * 7 % 17) / 8))
        _, model = self.journey(points)
        self.assert_durations(model, *interval_union_reference(points))

    def test_delayed_original_acquisitions_polls_heartbeats_do_not_add_time(self):
        engine = SportEngine()
        acquire(engine, BASE, 0, now=BASE + 300_000_000)
        acquire(engine, BASE + STEP, 0, now=BASE + 600_000_000)
        before = trip(engine, engine.last_now)
        for i in range(100):
            now = BASE + 600_000_000 + i * 1_000_000
            engine.accept(1, {}, now)
            acquire(engine, BASE + STEP, 0, now=now)
            self.assert_durations(trip(engine, now), .2, .2)
        self.assertEqual((before['firstAcquiredNs'], before['lastAcquiredNs']), (BASE, BASE + STEP))

    def test_short_withdrawals_and_expiry_advance_break_longest_run(self):
        for cause in ('unavailable', 'stale', 'disconnected', 'expiry', 'unrelated'):
            with self.subTest(cause=cause):
                engine, _ = self.journey([(0, 0), (STEP, 0)])
                if cause in ('expiry', 'unrelated'):
                    now = BASE + STEP + 500_000_000
                    if cause == 'expiry':
                        engine.advance(now)
                    else:
                        engine.accept(1, {'acceleration': Observation((0, 0), now)}, now)
                    next_stamp = now + 1
                else:
                    obs = engine.inputs['speed_kmh']
                    acquired = obs.acquired_ns + 1 if cause == 'unavailable' else obs.acquired_ns
                    engine.accept(1, {'speed_kmh': Observation(None, acquired,
                                  origin=obs.origin, availability=cause)}, engine.last_now + 1)
                    with self.assertRaises(ValueError):
                        engine.accept(1, {'speed_kmh': obs}, engine.last_now + 1)
                    next_stamp = BASE + 300_000_000
                self.assert_durations(trip(engine, engine.last_now), .2, .2)
                self.assertEqual(engine.trip.stationary_run_ns, 0)
                acquire(engine, next_stamp, 0)
                self.assert_durations(trip(engine, engine.last_now), .2, .2)
                acquire(engine, next_stamp + STEP, 0)
                self.assert_durations(trip(engine, engine.last_now), .4, .2)
                self.assertTrue(trip(engine, engine.last_now)['partial'])

    def test_exact_age_stale_delivery_and_gap_boundary(self):
        for age, delta, delay, expected in ((STEP, STEP, 0, None),
                                          (500_000_000, STEP, 500_000_000, None),
                                          (500_000_000, 250_000_000, 0, .25),
                                          (500_000_000, 250_000_001, 0, None)):
            engine = SportEngine()
            acquire(engine, BASE, 0, age=age)
            acquire(engine, BASE + delta, 0, age=age, now=BASE + delta + delay)
            self.assert_durations(trip(engine, engine.last_now), expected, expected)
        engine, _ = self.journey([(0, 0), (STEP, 0), (600_000_000, 0), (800_000_000, 0)])
        self.assert_durations(trip(engine, engine.last_now), .4, .2)

    def test_atomic_invalid_conflict_replay_and_source_bounds(self):
        engine, before = self.journey([(0, 0), (STEP, 0)])
        bad = [Observation(value, BASE + 2 * STEP) for value in (math.nan, math.inf, -1, 301, True)]
        bad += [Observation(0, BASE), Observation(1, BASE + STEP),
                Observation(0, BASE + 3 * STEP), Observation(0, BASE + 2 * STEP, NS)]
        batches = [{'speed_kmh': obs} for obs in bad]
        batches += [{'speed_kmh': Observation(0, BASE + 2 * STEP),
                     'gps_position': Observation((100, 2, 1), BASE + 2 * STEP)}]
        for batch in batches:
            with self.assertRaises(ValueError):
                engine.accept(1, batch, BASE + 2 * STEP)
            self.assertEqual(trip(engine, engine.last_now), before)
            self.assertEqual(engine.trip.stationary_run_ns, STEP)
        acquire(engine, BASE + 2 * STEP, 0)
        self.assert_durations(trip(engine, engine.last_now), .4, .4)

    def test_generation_origin_reset_and_retire_clear_without_archive_fields(self):
        for transition in ('generation', 'older_generation', 'origin', 'reset', 'retire'):
            engine, _ = self.journey([(0, 0), (STEP, 0)])
            if transition == 'generation':
                acquire(engine, BASE + 2 * STEP, 0, generation=2)
            elif transition == 'older_generation':
                acquire(engine, BASE + 100_000_000, 0, generation=2, now=BASE + 250_000_000)
            elif transition == 'origin':
                acquire(engine, BASE + 2 * STEP, 0, origin='measured')
            elif transition == 'reset':
                engine.reset_trip(BASE + 2 * STEP)
            else:
                engine.trip.retire(BASE + 2 * STEP)
            model = trip(engine, max(BASE + 2 * STEP, engine.last_now))
            self.assert_durations(model, None, None)
            self.assertEqual(set(model['history'][0]), STAT_KEYS)
            self.assertEqual(model['history'][0]['coveredSeconds'], .2)
            self.assertEqual(engine.trip.stationary_run_ns, 0)

    def test_reset_floor_survives_generation_and_sport_reset_is_independent(self):
        engine, _ = self.journey([(0, 0), (STEP, 0)])
        engine.reset_trip(BASE + 2 * STEP)
        acquire(engine, BASE + 300_000_000, 0, generation=2, now=BASE + 2 * STEP + 1)
        acquire(engine, BASE + 2 * STEP, 0, generation=2, now=BASE + 2 * STEP + 2)
        self.assert_durations(trip(engine, engine.last_now), None, None)
        acquire(engine, BASE + 3 * STEP, 0, generation=2)
        acquire(engine, BASE + 4 * STEP, 0, generation=2)
        engine.reset_session(BASE + 4 * STEP + 1)
        self.assert_durations(trip(engine, engine.last_now), .2, .2)

    def test_limits_freeze_totals_break_run_and_require_reset(self):
        for name, limit in (('MAXIMUM_DURATION_NS', 2 * STEP), ('MAXIMUM_DISTANCE_KM', .00002)):
            with self.subTest(limit=name), patch.object(TripStatistics, name, limit):
                engine, _ = self.journey([(0, .1), (STEP, .1), (2 * STEP, .1)])
                before = trip(engine, engine.last_now)
                acquire(engine, BASE + 3 * STEP, 300)
                limited = trip(engine, engine.last_now)
                self.assertTrue(limited['limitReached'])
                self.assertEqual(durations(limited), durations(before))
                self.assertEqual(engine.trip.stationary_run_ns, 0)
                acquire(engine, BASE + 4 * STEP, 0)
                self.assertEqual(durations(trip(engine, engine.last_now)), durations(before))
                engine.reset_trip(engine.last_now + 1)
                acquire(engine, BASE + 5 * STEP, 0)
                self.assert_durations(trip(engine, engine.last_now), None, None)
                acquire(engine, BASE + 6 * STEP, 0)
                self.assert_durations(trip(engine, engine.last_now), .2, .2)

    def test_actual_socket_manager_copies_duplicate_rejection_failure_and_reap(self):
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
            source = manager.sport = SportSource(folder, socket_path=folder / 'stationary.sock')
            source.start()
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
                    first = host_now_ns()
                    sender.sendto(packet(first, value='0'), str(source.path))
                    wait(lambda: source.snapshot()['trip']['firstAcquiredNs'] == first)
                    time.sleep(.02)
                    second = host_now_ns()
                    sender.sendto(packet(second, sequence=2, value='0'), str(source.path))
                    wait(lambda: source.snapshot()['trip']['lastAcquiredNs'] == second)
                    duration = (second - first) / NS
                    model = manager.request({'action': 'status'})['sport']['trip']
                    self.assert_durations(model, duration, duration)
                    for key in KEYS:
                        model[key] = -999
                    self.assert_durations(source.snapshot()['trip'], duration, duration)
                    invalid = packet(second + 1, sequence=3, value='300', gps='100/2/1')
                    duplicate = packet(second + 1, sequence=3).replace(b'fuel_percent=', b'speed_kmh=')
                    for count, data in enumerate((invalid, duplicate, packet(first)), 1):
                        sender.sendto(data, str(source.path))
                        wait(lambda: source.snapshot()['rejectedPackets'] == count)
                        self.assertEqual(source.packet_mark[2], 2)
                        self.assert_durations(source.snapshot()['trip'], duration, duration)
                    with source.lock:
                        source.socket.close()
                    source.thread.join(timeout=2)
                self.assertFalse(source.thread.is_alive())
                retired = source.snapshot()
                self.assertEqual(retired['error'], 'Sport acquisition stopped')
                self.assertEqual(retired['trip']['phase'], 'waiting')
                self.assertEqual(retired['trip']['lastAcquiredNs'], second)
                self.assertEqual(source.engine.trip.stationary_run_ns, 0)
                self.assertIsNone(retired['inputs']['speed_kmh']['value'])
                self.assert_durations(retired['trip'], duration, duration)
            finally:
                manager.shutdown()
            self.assertFalse(source.path.exists())
            self.assertFalse(source.thread.is_alive())
            restarted = SportSource(folder)
            try:
                self.assert_durations(restarted.snapshot()['trip'], None, None)
            finally:
                restarted.close()

    def test_private_archive_export_restore_and_orderly_retirement_exclude_live_fields(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            source = SportSource(folder, trip_history=True)
            source.start()
            try:
                wait(lambda: source.snapshot()['trip']['storage']['state'] == 'ready')
                with source.lock:
                    now = host_now_ns()
                    acquire(source.engine, now - 300_000_000, 0, now=now)
                    acquire(source.engine, now - 100_000_000, 0, now=now)
                self.assert_durations(source.snapshot()['trip'], .2, .2)
                reset = source.request({'action': 'trip_reset'})
                self.assert_durations(reset['trip'], None, None)
                records = reset['trip']['history']
                validate_history(records)
                for key in KEYS:
                    incompatible = copy.deepcopy(records)
                    incompatible[0][key] = .2
                    with self.assertRaises(ValueError):
                        validate_history(incompatible)
                exported = envelope(records)
                self.assertEqual(restore(exported), records)
                wait(lambda: not source.snapshot()['trip']['storage']['pending'])
                with source.lock:
                    stamp = host_now_ns()
                    acquire(source.engine, stamp, 0)
                time.sleep(.02)
                with source.lock:
                    last = host_now_ns()
                    acquire(source.engine, last, 0)
                self.assert_durations(source.snapshot()['trip'], (last - stamp) / NS, (last - stamp) / NS)
            finally:
                source.close()
            self.assertFalse(source.thread.is_alive())
            self.assertTrue(source.storage.reaped)
            self.assert_durations(source.snapshot()['trip'], None, None)
            original = (folder / 'trip-history.json').read_bytes()
            archived = restore(original)
            self.assertEqual(encode(archived[:-1]), encode(records))
            self.assertEqual(archived[-1]['endedReason'], 'Sport owner retired')
            self.assertEqual(archived[-1]['lastAcquiredNs'], last)
            self.assertEqual(archived[-1]['coveredSeconds'], (last - stamp) / NS)
            self.assertTrue(all(key not in row for row in archived for key in KEYS))
            restarted = SportSource(folder, trip_history=True)
            restarted.start()
            try:
                wait(lambda: restarted.snapshot()['trip']['storage']['state'] == 'ready')
                model = restarted.snapshot()['trip']
                self.assertEqual(encode(model['history']), encode(archived))
                self.assertEqual((folder / 'trip-history.json').read_bytes(), original)
                self.assert_durations(model, None, None)
            finally:
                restarted.close()
            self.assertTrue(restarted.storage.reaped)
            self.assertFalse(restarted.thread.is_alive())


if __name__ == '__main__':
    unittest.main()
