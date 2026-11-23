#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Live band-distance software estimates; explicit simulation, original owner paths."""
import copy
from fractions import Fraction
import json
import math
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sport_telemetry import Observation, SportEngine, TripStatistics, NS
from sport_source import SportSource, host_now_ns
from phone_manager import Manager
from phone_manager_endpoint import ManagerEndpoint
from trip_history import STAT_KEYS, encode, envelope, restore, validate_history
from trip_statistics_test import BASE, STEP, acquire, trip
from sport_telemetry_test import packet
from trip_history_test import wait

RANGES = ((0, 30), (30, 60), (60, 90), (90, 120), (120, None))


def distances(model):
    return [row['distanceKm'] for row in model['speedBands']]


def integral_reference(points):
    # Exact rational antiderivative in speed coordinates, independent of the
    # production clipped mean/fraction computation. Dates are original ns.
    totals = [Fraction(0) for _ in RANGES]
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        a, b = sorted((Fraction(v0), Fraction(v1)))
        seconds = Fraction(t1 - t0, NS)
        for index, (low, high) in enumerate(RANGES):
            if a == b:
                if low <= a and (high is None or a < high):
                    totals[index] += a * seconds / 3600
            else:
                left = max(a, low)
                right = min(b, high) if high is not None else b
                if left < right:
                    totals[index] += (right ** 2 - left ** 2) * seconds / (2 * (b - a) * 3600)
    return [float(value) for value in totals]


class DistanceTests(unittest.TestCase):
    def endpoint_request(self, manager, endpoint, command):
        # Keep manager callbacks on this owner. Drain the real reply concurrently
        # so a growing bounded status cannot depend on the socket send buffer.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(2)
            client.connect(str(endpoint.path))
            client.sendall(json.dumps(command).encode() + b'\n')
            received = {}

            def read_reply():
                try:
                    raw = bytearray()
                    while not raw.endswith(b'\n'):
                        chunk = client.recv(65536)
                        self.assertTrue(chunk)
                        raw.extend(chunk)
                        self.assertLessEqual(len(raw), 1_500_000)
                    received['reply'] = json.loads(raw)
                except BaseException as error:
                    received['error'] = error

            reader = threading.Thread(target=read_reply)
            reader.start()
            try:
                self.assertTrue(endpoint.serve_once(manager.request))
            finally:
                reader.join(timeout=2)
                if reader.is_alive():
                    try:
                        client.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    reader.join(timeout=2)
                self.assertFalse(reader.is_alive(), 'Reply reader did not retire')
            if 'error' in received:
                raise received['error']
            reply = received['reply']
        self.assertTrue(reply['ok'], reply)
        return reply

    def assert_distances(self, model, expected):
        self.assertEqual([(row['lowerKmh'], row['upperKmh']) for row in model['speedBands']], list(RANGES))
        self.assertEqual(len(expected), 5)
        for actual, wanted in zip(distances(model), expected):
            if wanted is None:
                self.assertIsNone(actual)
            else:
                self.assertTrue(math.isfinite(actual))
                self.assertGreaterEqual(actual, 0)
                # Explicit binary64 numerical tolerance, never normalization of
                # the old total. Absolute floor also covers tiny rounded km.
                self.assertAlmostEqual(actual, wanted, delta=1e-12 * max(1, abs(wanted)))
        if model['coveredSeconds'] is not None:
            self.assertAlmostEqual(math.fsum(distances(model)), model['distanceKm'],
                                   delta=1e-12 * max(1, model['distanceKm']))
        self.assertEqual(model['authority'], 'software-speed-estimate')
        json.dumps(model, allow_nan=False)

    def journey(self, points):
        engine = SportEngine()
        for stamp, speed in points:
            acquire(engine, BASE + stamp, speed)
        return engine, trip(engine, engine.last_now)

    def test_missing_baseline_and_genuine_covered_zero(self):
        engine = SportEngine()
        self.assert_distances(trip(engine, BASE), [None] * 5)
        acquire(engine, BASE, 0)
        self.assert_distances(trip(engine, BASE), [None] * 5)
        acquire(engine, BASE + STEP, 0)
        self.assert_distances(trip(engine, engine.last_now), [0] * 5)
        self.assertEqual(engine.trip.speed_band_distance_km, [0] * 5)

    def test_constants_boundaries_and_adjacent_representable_values(self):
        for index, (low, high) in enumerate(RANGES):
            speeds = [low, math.nextafter(low, math.inf)]
            speeds.append(math.nextafter(high, -math.inf) if high is not None else 300)
            for speed in speeds:
                with self.subTest(speed=speed):
                    _, model = self.journey([(0, speed), (STEP, speed)])
                    expected = [0] * 5
                    expected[index] = speed * .2 / 3600
                    self.assert_distances(model, expected)
                    self.assertEqual(model['speedBands'][index]['seconds'], .2)

    def test_analytic_forward_reverse_ramps_all_five_bands(self):
        for a, b in ((0, 150), (150, 0), (0, 300), (300, 0),
                     (15, 105), (105, 15), (30, 120), (120, 30)):
            with self.subTest(endpoints=(a, b)):
                points = [(0, a), (STEP, b)]
                _, model = self.journey(points)
                self.assert_distances(model, integral_reference(points))
        _, model = self.journey([(0, 0), (STEP, 150)])
        self.assert_distances(model, [15 * .04 / 3600, 45 * .04 / 3600, 75 * .04 / 3600,
                                      105 * .04 / 3600, 135 * .04 / 3600])
        # Equal time in each band must not allocate equal distance.
        self.assertGreater(distances(model)[4], 8 * distances(model)[0])

    def test_pure_integral_0_to_600_and_back_does_not_relax_public_guard(self):
        # Stress the existing internal admitted-owner arithmetic directly. The
        # public SportEngine still rejects 600; this is not an admitted reading.
        value = TripStatistics()
        value.begin_generation(1, BASE)
        for index, speed in enumerate((0, 600, 0)):
            value.accept(Observation(speed, BASE + index * STEP), BASE + index * STEP)
        self.assert_distances(value.model(), integral_reference([(0, 0), (STEP, 600), (2 * STEP, 0)]))
        engine = SportEngine()
        acquire(engine, BASE, 300)
        before = copy.deepcopy(engine.__dict__)
        for speed in (math.nextafter(300, math.inf), 600, math.inf, math.nan, -1, True):
            with self.assertRaises(ValueError):
                acquire(engine, BASE + STEP, speed)
            self.assertEqual(engine.trip.model(), before['trip'].model())
            self.assertEqual(engine.last_now, before['last_now'])

    def test_split_additivity_nonuniform_durations_and_reversal(self):
        for a, b in ((0, 300), (300, 0), (15, 135), (135, 15)):
            stamps = [0, 1, 999, 24_000_000, 48_000_000, 120_000_000, 239_999_999, 240_000_000]
            points = [(t, a + (b - a) * t / stamps[-1]) for t in stamps]
            _, split = self.journey(points)
            _, whole = self.journey([(0, a), (stamps[-1], b)])
            self.assert_distances(split, distances(whole))
            self.assert_distances(split, integral_reference(points))
        points = [(0, 0), (40_000_000, 120), (140_000_000, 30), (340_000_000, 300)]
        _, model = self.journey(points)
        self.assert_distances(model, integral_reference(points))

    def test_nearly_equal_high_speed_tiny_positive_and_one_nanosecond(self):
        for a, b in ((300, math.nextafter(300, 0)), (math.nextafter(300, 0), 300),
                     (30, math.nextafter(30, 0)), (math.nextafter(120, 0), 120),
                     (0, math.nextafter(0, 1)), (1e-100, 2e-100)):
            points = [(0, a), (1, b)]
            _, model = self.journey(points)
            self.assert_distances(model, integral_reference(points))
            self.assertEqual(model['coveredSeconds'], 1 / NS)
        _, tiny = self.journey([(0, 1e-100), (STEP, 2e-100)])
        self.assertGreater(distances(tiny)[0], 0)
        self.assertTrue(math.isclose(distances(tiny)[0], 1.5e-100 * .2 / 3600, rel_tol=1e-14))

    def test_many_intervals_match_exact_rational_integral(self):
        points = [(0, 0)]
        for index in range(1, 601):
            points.append((points[-1][0] + (index * 7919 % 200_000_000) + 1,
                           (index * 73 % 601) / 2))
        _, model = self.journey(points)
        self.assert_distances(model, integral_reference(points))
        self.assertEqual(len(model['speedBands']), 5)

    def test_polling_heartbeats_delayed_acquisition_and_sport_reset_add_nothing(self):
        engine = SportEngine()
        acquire(engine, BASE, 0, now=BASE + 100_000_000)
        acquire(engine, BASE + STEP, 150, now=BASE + STEP + 100_000_000)
        original = trip(engine, engine.last_now)
        for index in range(100):
            now = BASE + STEP + 100_000_000 + index
            acquire(engine, BASE + STEP, 150, now=now)
            self.assertEqual(distances(trip(engine, now)), distances(original))
        engine.reset_session(engine.last_now + 1)
        self.assertEqual(distances(trip(engine, engine.last_now)), distances(original))
        self.assertEqual(original['lastAcquiredNs'], BASE + STEP)

    def test_gap_exact_expiry_and_withdrawal_do_not_bridge(self):
        for kind in ('gap', 'expiry', 'withdrawal'):
            with self.subTest(kind=kind):
                engine, before = self.journey([(0, 30), (STEP, 30)])
                if kind == 'withdrawal':
                    obs = engine.inputs['speed_kmh']
                    engine.accept(1, {'speed_kmh': Observation(None, obs.acquired_ns,
                                  origin=obs.origin, availability='disconnected')}, engine.last_now + 1)
                    with self.assertRaises(ValueError):
                        engine.accept(1, {'speed_kmh': obs}, engine.last_now + 1)
                elif kind == 'expiry':
                    engine.advance(BASE + STEP + 500_000_000)
                acquire(engine, BASE + NS, 120)
                self.assert_distances(trip(engine, engine.last_now), distances(before))
                acquire(engine, BASE + NS + STEP, 120)
                expected = distances(before)
                expected[4] += 120 * .2 / 3600
                model = trip(engine, engine.last_now)
                self.assert_distances(model, expected)
                self.assertTrue(model['partial'])
                self.assertEqual(model['lastAcquiredNs'], BASE + NS + STEP)

    def test_atomic_invalid_age_and_reversed_acquisitions_leave_all_state(self):
        engine, _ = self.journey([(0, 0), (STEP, 150)])
        before = copy.deepcopy(engine.__dict__)
        batches = [({'speed_kmh': Observation(30, BASE + STEP)}, BASE + 2 * STEP),
                   ({'speed_kmh': Observation(30, BASE)}, BASE + 2 * STEP),
                   ({'speed_kmh': Observation(30, BASE + 2 * STEP, NS)}, BASE + 2 * STEP),
                   ({'speed_kmh': Observation(30, BASE + 3 * STEP)}, BASE + 2 * STEP),
                   ({'speed_kmh': Observation(30, BASE + 2 * STEP),
                     'gps_position': Observation((100, 2, 1), BASE + 2 * STEP)}, BASE + 2 * STEP)]
        for batch, now in batches:
            with self.assertRaises(ValueError):
                engine.accept(1, batch, now)
            self.assertEqual(engine.trip.model(), before['trip'].model())
            self.assertEqual(engine.last_now, before['last_now'])
        acquire(engine, BASE + 2 * STEP, 0)
        self.assert_distances(trip(engine, engine.last_now), integral_reference([(0, 0), (STEP, 150), (2 * STEP, 0)]))

    def test_generation_origin_reset_and_retirement_clear_live_without_archiving_rows(self):
        for transition in ('generation', 'origin', 'reset', 'retire'):
            with self.subTest(transition=transition):
                engine, _ = self.journey([(0, 0), (STEP, 150)])
                if transition == 'generation':
                    acquire(engine, BASE + 100_000_000, 120, generation=2, now=BASE + 250_000_000)
                elif transition == 'origin':
                    acquire(engine, BASE + 2 * STEP, 120, origin='measured')
                elif transition == 'reset':
                    engine.reset_trip(BASE + 2 * STEP)
                    acquire(engine, BASE + 300_000_000, 120, generation=2, now=BASE + 2 * STEP + 1)
                else:
                    engine.trip.retire(BASE + 2 * STEP)
                model = trip(engine, engine.last_now)
                self.assert_distances(model, [None] * 5)
                self.assertEqual(len(model['history']), 1)
                self.assertEqual(set(model['history'][0]), STAT_KEYS)
                self.assertNotIn('speedBands', model['history'][0])
                self.assertNotIn('speedBands', engine.trip._statistics())

    def test_duration_distance_and_acquired_span_limits_preserve_known_totals(self):
        for name, bound in (('MAXIMUM_DURATION_NS', 2 * STEP), ('MAXIMUM_DISTANCE_KM', .004)):
            with self.subTest(bound=name), patch.object(TripStatistics, name, bound):
                engine, _ = self.journey([(0, 36), (STEP, 36), (2 * STEP, 36)])
                before = trip(engine, engine.last_now)
                acquire(engine, BASE + 3 * STEP, 300)
                limited = trip(engine, engine.last_now)
                self.assertTrue(limited['limitReached'])
                self.assert_distances(limited, distances(before))
                acquire(engine, BASE + 4 * STEP, 0)
                self.assertEqual(distances(trip(engine, engine.last_now)), distances(before))
                engine.reset_trip(engine.last_now + 1)
                acquire(engine, BASE + 5 * STEP, 36)
                acquire(engine, BASE + 6 * STEP, 36)
                self.assert_distances(trip(engine, engine.last_now), [0, .002, 0, 0, 0])
        with patch.object(TripStatistics, 'MAXIMUM_DURATION_NS', STEP):
            engine = SportEngine()
            acquire(engine, BASE, 0)
            acquire(engine, BASE + NS, 300)
            model = trip(engine, engine.last_now)
            self.assertTrue(model['limitReached'])
            self.assert_distances(model, [None] * 5)

    def test_actual_manager_socket_copy_failed_ingress_and_retirement(self):
        class Phones:
            def phones(self):
                return []
            def media(self, phone):
                return {'available': False}
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            display = folder / 'display.conf'
            display.write_text('explicit manager fixture geometry')
            config = folder / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(folder / 'pairs'),
                                          'display_config': str(display), 'sport_telemetry': True}))
            manager = Manager(config, folder, display, (os.getuid(), os.getgid()), bluez=Phones())
            manager.sport.close()
            source = manager.sport = SportSource(folder, socket_path=folder / 'speed.sock')
            source.start()
            endpoint = ManagerEndpoint(folder)
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
                    first = host_now_ns()
                    sender.sendto(packet(first, value='30'), str(source.path))
                    wait(lambda: source.snapshot()['trip']['firstAcquiredNs'] == first)
                    time.sleep(.02)
                    second = host_now_ns()
                    sender.sendto(packet(second, sequence=2, value='120'), str(source.path))
                    wait(lambda: source.snapshot()['trip']['lastAcquiredNs'] == second)
                    model = self.endpoint_request(manager, endpoint, {'action': 'status'})['sport']['trip']
                    expected = integral_reference([(first, 30), (second, 120)])
                    self.assert_distances(model, expected)
                    model['speedBands'][1]['distanceKm'] = 999
                    self.assert_distances(source.snapshot()['trip'], expected)
                    sender.sendto(packet(second + 1, sequence=3, value='600'), str(source.path))
                    wait(lambda: source.snapshot()['rejectedPackets'] == 1)
                    self.assertEqual(source.packet_mark[2], 2)
                    self.assert_distances(source.snapshot()['trip'], expected)
                    with source.lock:
                        source.socket.close()
                    source.thread.join(timeout=2)
                self.assertFalse(source.thread.is_alive())
                retired = self.endpoint_request(manager, endpoint, {'action': 'status'})['sport']
                self.assertEqual(retired['error'], 'Sport acquisition stopped')
                self.assertEqual(retired['trip']['phase'], 'waiting')
                self.assertEqual(retired['trip']['lastAcquiredNs'], second)
                self.assert_distances(retired['trip'], expected)
                self.assertIsNone(retired['inputs']['speed_kmh']['value'])
            finally:
                endpoint.close()
                manager.shutdown()
            self.assertFalse(source.path.exists())
            self.assertFalse(endpoint.path.exists())
            self.assertFalse(source.thread.is_alive())

    def test_actual_private_history_export_restart_and_child_reap(self):
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
                self.assert_distances(source.snapshot()['trip'], integral_reference([(0, 0), (STEP, 150)]))
                reset = source.request({'action': 'trip_reset'})
                self.assert_distances(reset['trip'], [None] * 5)
                records = reset['trip']['history']
                validate_history(records)
                self.assertNotIn('speedBands', records[0])
                self.assertEqual(restore(envelope(records)), records)
                wait(lambda: not source.snapshot()['trip']['storage']['pending'])
                with source.lock:
                    first = host_now_ns()
                    acquire(source.engine, first, 0)
                time.sleep(.02)
                with source.lock:
                    last = host_now_ns()
                    acquire(source.engine, last, 0)
                self.assert_distances(source.snapshot()['trip'], [0] * 5)
            finally:
                source.close()
            self.assertFalse(source.thread.is_alive())
            self.assertTrue(source.storage.reaped)
            self.assertIsNotNone(source.storage.process.poll())
            self.assert_distances(source.snapshot()['trip'], [None] * 5)
            original = (folder / 'trip-history.json').read_bytes()
            archived = restore(original)
            self.assertEqual(encode(archived[:-1]), encode(records))
            self.assertEqual(archived[-1]['endedReason'], 'Sport owner retired')
            self.assertEqual(archived[-1]['lastAcquiredNs'], last)
            self.assertEqual(archived[-1]['coveredSeconds'], (last - first) / NS)
            self.assertTrue(all('speedBands' not in row for row in archived))
            restarted = SportSource(folder, trip_history=True)
            restarted.start()
            try:
                wait(lambda: restarted.snapshot()['trip']['storage']['state'] == 'ready')
                self.assertEqual(encode(restarted.snapshot()['trip']['history']), encode(archived))
                self.assert_distances(restarted.snapshot()['trip'], [None] * 5)
                self.assertEqual((folder / 'trip-history.json').read_bytes(), original)
            finally:
                restarted.close()
            self.assertTrue(restarted.storage.reaped)
            self.assertFalse(restarted.thread.is_alive())


if __name__ == '__main__':
    unittest.main()
