#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Live software classifiers; simulated values, actual existing owner/storage paths."""
import copy
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
from sport_telemetry import SportEngine, Observation, TripStatistics, NS
from sport_source import SportSource, host_now_ns
from phone_manager import Manager
from trip_history import STAT_KEYS, restore, validate_history, encode, envelope
from trip_statistics_test import BASE, STEP, acquire, trip
from sport_telemetry_test import packet
from trip_history_test import wait

RANGES = [(0, 30), (30, 60), (60, 90), (90, 120), (120, None)]


def seconds(model):
    return [row['seconds'] for row in model['speedBands']]


class BandTests(unittest.TestCase):
    def assert_bands(self, model, expected):
        self.assertEqual([(row['lowerKmh'], row['upperKmh']) for row in model['speedBands']], RANGES)
        self.assertEqual(len(expected), 5)
        for value, wanted in zip(seconds(model), expected):
            if wanted is None:
                self.assertIsNone(value)
            else:
                self.assertGreaterEqual(value, 0)
                self.assertAlmostEqual(value, wanted, delta=1e-12 * max(1, wanted))
        if model['coveredSeconds'] is not None:
            self.assertAlmostEqual(math.fsum(seconds(model)), model['coveredSeconds'],
                                   delta=1e-12 * max(1, model['coveredSeconds']))
        self.assertEqual(model['authority'], 'software-speed-estimate')
        json.dumps(model, allow_nan=False)

    def segment(self, start, end):
        engine = SportEngine()
        acquire(engine, BASE, start)
        acquire(engine, BASE + STEP, end)
        return engine, trip(engine, BASE + STEP)

    def test_missing_single_and_known_zero_are_distinct(self):
        engine = SportEngine()
        self.assert_bands(trip(engine, BASE), [None] * 5)
        acquire(engine, BASE, 0)
        self.assert_bands(trip(engine, BASE), [None] * 5)
        acquire(engine, BASE + STEP, 0)
        self.assert_bands(trip(engine, BASE + STEP), [.2, 0, 0, 0, 0])

    def test_constant_edges_and_neighbouring_representable_speeds(self):
        for index, (low, high) in enumerate(RANGES):
            values = [low, math.nextafter(low, math.inf)]
            if high is not None:
                values.append(math.nextafter(high, -math.inf))
            else:
                values.append(300)
            for speed in values:
                with self.subTest(speed=speed):
                    _, model = self.segment(speed, speed)
                    expected = [0] * 5
                    expected[index] = .2
                    self.assert_bands(model, expected)

    def test_full_rising_falling_multiple_edges_and_reversal(self):
        for start, end, expected in ((0, 150, [.04] * 5), (150, 0, [.04] * 5),
                                     (0, 300, [.02, .02, .02, .02, .12]),
                                     (300, 0, [.02, .02, .02, .02, .12]),
                                     (15, 105, [1 / 30, 1 / 15, 1 / 15, 1 / 30, 0]),
                                     (105, 15, [1 / 30, 1 / 15, 1 / 15, 1 / 30, 0]),
                                     (30, 120, [0, 1 / 15, 1 / 15, 1 / 15, 0])):
            with self.subTest(start=start, end=end):
                _, model = self.segment(start, end)
                self.assert_bands(model, expected)
        engine, _ = self.segment(0, 150)
        acquire(engine, BASE + 2 * STEP, 0)
        self.assert_bands(trip(engine, engine.last_now), [.08] * 5)

    def test_near_boundary_crossing_and_endpoint_only_occupancy(self):
        for index, boundary in enumerate((30, 60, 90, 120)):
            below = math.nextafter(boundary, -math.inf)
            above = math.nextafter(boundary, math.inf)
            for start, end in ((below, above), (above, below)):
                _, model = self.segment(start, end)
                expected = [0] * 5
                expected[index:index + 2] = [.1, .1]
                self.assert_bands(model, expected)
            _, model = self.segment(boundary - 1, boundary)
            expected = [0] * 5
            expected[index] = .2
            self.assert_bands(model, expected)

    def test_many_segments_and_polling_do_not_change_duration(self):
        sparse, polled = SportEngine(), SportEngine()
        for index in range(501):
            stamp = BASE + index * STEP
            value = (index % 11) * 30
            for engine in (sparse, polled):
                acquire(engine, stamp, value)
            for offset in (0, 10_000_000, 50_000_000):
                polled.accept(1, {}, stamp + offset)
                trip(polled, stamp + offset)
                acquire(polled, stamp, value, now=stamp + offset)
        self.assert_bands(trip(polled, polled.last_now), seconds(trip(sparse, sparse.last_now)))
        self.assertEqual(trip(polled, polled.last_now)['coveredSeconds'], 100)
        self.assertEqual(trip(polled, polled.last_now)['lastAcquiredNs'], BASE + 100 * NS)

    def test_original_delayed_acquisition_and_metadata(self):
        engine = SportEngine()
        acquire(engine, BASE, 0, now=BASE + 300_000_000)
        acquire(engine, BASE + STEP, 150, now=BASE + 600_000_000)
        model = trip(engine, engine.last_now)
        self.assert_bands(model, [.04] * 5)
        self.assertEqual((model['firstAcquiredNs'], model['lastAcquiredNs']), (BASE, BASE + STEP))
        self.assertEqual((model['origin'], model['sourceGeneration']), ('simulation', 1))
        self.assertEqual(model['coverageRatio'], 1)

    def test_exact_expiry_stale_queued_and_long_gap_cannot_bridge(self):
        for age, delta, delay in ((STEP, STEP, 0), (500_000_000, 300_000_000, 0),
                                  (500_000_000, STEP, 500_000_000)):
            with self.subTest(age=age, delta=delta, delay=delay):
                engine = SportEngine()
                acquire(engine, BASE, 30, age=age)
                acquire(engine, BASE + delta, 120, age=age, now=BASE + delta + delay)
                self.assert_bands(trip(engine, engine.last_now), [None] * 5)
        engine, before = self.segment(30, 30)
        trip(engine, BASE + STEP + 500_000_000)
        acquire(engine, BASE + NS, 120)
        self.assert_bands(trip(engine, engine.last_now), seconds(before))
        acquire(engine, BASE + NS + STEP, 120)
        model = trip(engine, engine.last_now)
        self.assert_bands(model, [0, .2, 0, 0, .2])
        self.assertTrue(model['partial'])
        self.assertAlmostEqual(model['coverageRatio'], 1 / 3)

    def test_explicit_withdrawal_and_old_acquisition_cannot_revive(self):
        engine, model = self.segment(0, 150)
        obs = engine.inputs['speed_kmh']
        engine.accept(1, {'speed_kmh': Observation(None, obs.acquired_ns, origin=obs.origin,
                                                 availability='disconnected')}, engine.last_now + 1)
        with self.assertRaises(ValueError):
            engine.accept(1, {'speed_kmh': obs}, engine.last_now + 1)
        acquire(engine, BASE + 300_000_000, 120)
        self.assert_bands(trip(engine, engine.last_now), seconds(model))
        acquire(engine, BASE + 400_000_000, 120)
        self.assert_bands(trip(engine, engine.last_now), [.04, .04, .04, .04, .14])

    def test_atomic_rejection_preserves_all_statistics_and_bands(self):
        engine, _ = self.segment(0, 150)
        before = copy.deepcopy(engine.__dict__)
        invalid = [Observation(math.nan, BASE + 2 * STEP), Observation(-1, BASE + 2 * STEP),
                   Observation(301, BASE + 2 * STEP), Observation(30, BASE + STEP),
                   Observation(30, BASE), Observation(30, BASE + 2 * STEP, maximum_age_ns=NS)]
        batches = [{'speed_kmh': obs} for obs in invalid]
        batches.append({'speed_kmh': Observation(0, BASE + 2 * STEP),
                        'gps_position': Observation((100, 2, 1), BASE + 2 * STEP)})
        for batch in batches:
            with self.assertRaises(ValueError):
                engine.accept(1, batch, BASE + 2 * STEP)
            self.assertEqual(engine.trip.model(), before['trip'].model())
            self.assertEqual(engine.last_now, before['last_now'])
        acquire(engine, BASE + 2 * STEP, 0)
        self.assert_bands(trip(engine, engine.last_now), [.08] * 5)

    def test_generation_origin_reset_and_retirement_clear_live_only(self):
        for transition in ('generation', 'origin', 'reset', 'retire'):
            with self.subTest(transition=transition):
                engine, _ = self.segment(0, 150)
                if transition == 'generation':
                    acquire(engine, BASE + 2 * STEP, 120, generation=2)
                elif transition == 'origin':
                    acquire(engine, BASE + 2 * STEP, 120, origin='measured')
                elif transition == 'reset':
                    engine.reset_trip(BASE + 2 * STEP)
                    acquire(engine, BASE + 300_000_000, 120, now=BASE + 2 * STEP)
                else:
                    engine.trip.retire(BASE + 2 * STEP)
                model = trip(engine, BASE + 2 * STEP)
                self.assert_bands(model, [None] * 5)
                self.assertEqual(len(model['history']), 1)
                self.assertEqual(set(model['history'][0]), STAT_KEYS)
                self.assertNotIn('speedBands', model['history'][0])
                self.assertEqual(model['history'][0]['coveredSeconds'], .2)

    def test_reset_cutoff_survives_generation_and_sport_session_is_independent(self):
        engine, _ = self.segment(0, 150)
        engine.reset_trip(BASE + 2 * STEP)
        acquire(engine, BASE + 300_000_000, 300, generation=2, now=BASE + 2 * STEP + 1)
        acquire(engine, BASE + 2 * STEP, 300, generation=2, now=BASE + 2 * STEP + 2)
        self.assert_bands(trip(engine, engine.last_now), [None] * 5)
        acquire(engine, BASE + 3 * STEP, 120, generation=2)
        acquire(engine, BASE + 4 * STEP, 120, generation=2)
        engine.reset_session(BASE + 4 * STEP + 1)
        self.assert_bands(trip(engine, engine.last_now), [0, 0, 0, 0, .2])

    def test_fresh_older_new_source_baseline_cannot_inherit_bands(self):
        engine, _ = self.segment(0, 150)
        acquire(engine, BASE + 100_000_000, 30, generation=2, now=BASE + 250_000_000)
        self.assert_bands(trip(engine, engine.last_now), [None] * 5)
        acquire(engine, BASE + 300_000_000, 30, generation=2)
        self.assert_bands(trip(engine, engine.last_now), [0, .2, 0, 0, 0])

    def test_all_limit_rejections_leave_existing_band_durations(self):
        for name, limit in (('MAXIMUM_DURATION_NS', 2 * STEP), ('MAXIMUM_DISTANCE_KM', .004)):
            with self.subTest(limit=name), patch.object(TripStatistics, name, limit):
                engine, _ = self.segment(36, 36)
                acquire(engine, BASE + 2 * STEP, 36)
                model = trip(engine, engine.last_now)
                acquire(engine, BASE + 3 * STEP, 300)
                limited = trip(engine, engine.last_now)
                self.assertTrue(limited['limitReached'])
                self.assert_bands(limited, seconds(model))
                acquire(engine, BASE + 4 * STEP, 300)
                self.assert_bands(trip(engine, engine.last_now), seconds(model))
                engine.reset_trip(engine.last_now + 1)
                acquire(engine, BASE + 5 * STEP, 36)
                acquire(engine, BASE + 6 * STEP, 36)
                self.assert_bands(trip(engine, engine.last_now), [0, .2, 0, 0, 0])

    def test_archive_schema_bytes_and_limits_are_unchanged(self):
        engine, _ = self.segment(0, 150)
        expected = {'journeyId': 1, 'sourceGeneration': 1, 'origin': 'simulation',
                    'speedMaximumAgeNs': 500000000, 'firstAcquiredNs': BASE, 'lastAcquiredNs': BASE + STEP,
                    'distanceKm': 75 * .2 / 3600, 'coveredSeconds': .2,
                    'movingSeconds': STEP * (1 - .5 / 150) / NS,
                    'movingDistanceKm': (.5 + 150) * .5 * .2 * (1 - .5 / 150) / 3600,
                    'averageSpeedKmh': 75.0, 'movingAverageSpeedKmh': 75.25,
                    'peak': {'speedKmh': 150, 'acquiredNs': BASE + STEP, 'origin': 'simulation'},
                    'acquiredSpanSeconds': .2, 'uncoveredSeconds': 0.0, 'coverageRatio': 1.0,
                    'partial': False, 'limitReached': False}
        # Exact numeric/JSON comparison protects the pre-band archive schema.
        self.assertEqual(encode(engine.trip._statistics()), encode(expected))
        engine.reset_trip(BASE + 2 * STEP)
        self.assertEqual(encode(engine.trip.history[0]), encode({**expected,
                         'endedReason': 'User software reset', 'archivedAtNs': BASE + 2 * STEP}))
        for index in range(20):
            stamp = BASE + (index + 1) * NS
            acquire(engine, stamp, 300)
            acquire(engine, stamp + STEP, 300)
            engine.reset_trip(stamp + STEP + 1)
        self.assertEqual(len(engine.trip.history), 16)
        self.assertEqual(engine.history.maxlen, 64)
        self.assertTrue(all(set(item) == STAT_KEYS for item in engine.trip.history))
        self.assertLess(len(json.dumps(engine.trip.model())), 20000)

    def test_actual_socket_manager_copy_failure_and_join(self):
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
            source = manager.sport
            source.close()
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
                    duration = (second - first) / NS
                    self.assert_bands(model, [0, duration / 3, duration / 3, duration / 3, 0])
                    model['speedBands'][0]['seconds'] = 999
                    self.assert_bands(source.snapshot()['trip'], [0, duration / 3, duration / 3, duration / 3, 0])
                    # Invalid whole datagram does not consume the sequence.
                    invalid = packet(second + 1, sequence=3, value='300', gps='100/2/1')
                    sender.sendto(invalid, str(source.path))
                    wait(lambda: source.snapshot()['rejectedPackets'] == 1)
                    self.assertEqual(source.packet_mark[2], 2)
                    with source.lock:
                        source.socket.close()
                    source.thread.join(timeout=2)
                self.assertFalse(source.thread.is_alive())
                retired = source.snapshot()
                self.assertEqual(retired['error'], 'Sport acquisition stopped')
                self.assertEqual(retired['trip']['phase'], 'waiting')
                self.assertEqual(retired['trip']['lastAcquiredNs'], second)
                self.assert_bands(retired['trip'], [0, duration / 3, duration / 3, duration / 3, 0])
                self.assertIsNone(retired['inputs']['speed_kmh']['value'])
            finally:
                manager.shutdown()
            self.assertFalse(source.path.exists())
            self.assertFalse(source.thread.is_alive())
            restarted = SportSource(folder)
            try:
                self.assert_bands(restarted.snapshot()['trip'], [None] * 5)
            finally:
                restarted.close()

    def test_actual_private_archive_export_restart_never_restores_live_bands(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            source = SportSource(folder, trip_history=True)
            source.start()
            try:
                wait(lambda: source.snapshot()['trip']['storage']['state'] == 'ready')
                with source.lock:
                    now = host_now_ns()
                    acquire(source.engine, now - 300_000_000, 0, now=now)
                    acquire(source.engine, now - 100_000_000, 150, now=now)
                self.assert_bands(source.snapshot()['trip'], [.04] * 5)
                reset = source.request({'action': 'trip_reset'})
                self.assert_bands(reset['trip'], [None] * 5)
                records = reset['trip']['history']
                validate_history(records)
                self.assertNotIn('speedBands', records[0])
                incompatible = copy.deepcopy(records)
                incompatible[0]['speedBands'] = source.snapshot()['trip']['speedBands']
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
                self.assertEqual(model['history'], records)
                self.assert_bands(model, [None] * 5)
                self.assertEqual((folder / 'trip-history.json').read_bytes(), original)
            finally:
                restarted.close()
            self.assertTrue(restarted.storage.reaped)


if __name__ == '__main__':
    unittest.main()
