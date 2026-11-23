#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Recent Trip estimates from explicit synthetic original acquisitions, not hardware."""
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

sys.path[:0] = [str(Path(__file__).resolve().parents[1] / 'tools'),
                str(Path(__file__).resolve().parent)]
from sport_telemetry import SportEngine, Observation, NS, RecentPace
from sport_source import SportSource, host_now_ns
from sport_telemetry_test import packet
from trip_statistics_test import acquire, BASE, STEP
import phone_management_test as manager_fixtures


def recent(engine, now=None):
    return engine.model(engine.last_now if now is None else now)['trip']['recent']


def reference(points, anchor):
    """Exact rational integral of each linear polynomial in original time.

    Uses the antiderivative of v(t)=a+b*t, independently of the production
    clipped-trapezoid implementation. Fixtures explicitly supply accepted edges.
    """
    floor = Fraction(anchor - 60 * NS, NS)
    area = Fraction(0)
    covered = Fraction(0)
    for (ta, va), (tb, vb) in points:
        a, b = Fraction(ta, NS), Fraction(tb, NS)
        lo, hi = max(a, floor), b
        if hi <= lo:
            continue
        slope = (Fraction(vb) - Fraction(va)) / (b - a)
        intercept = Fraction(va) - slope * a
        area += intercept * (hi - lo) + slope * (hi * hi - lo * lo) / 2
        covered += hi - lo
    return float(area / 3600), float(covered), float(area / covered) if covered else None


class RecentTests(unittest.TestCase):
    def test_partial_early_window_unknown_and_real_zero(self):
        e = SportEngine()
        m = recent(e, BASE)
        for key in ('distanceKm', 'coveredSeconds', 'averageSpeedKmh',
                    'anchorAcquiredNs', 'observedSpanSeconds', 'coverageRatio'):
            self.assertIsNone(m[key])
        self.assertTrue(m['partial'])
        self.assertFalse(m['currentInputAvailable'])
        acquire(e, BASE, 0)
        self.assertIsNone(recent(e)['distanceKm'])
        acquire(e, BASE + STEP, 0)
        m = recent(e)
        self.assertEqual(m['distanceKm'], 0)
        self.assertEqual(m['averageSpeedKmh'], 0)
        self.assertEqual(m['coveredSeconds'], .2)
        self.assertEqual(m['observedSpanSeconds'], .2)
        self.assertAlmostEqual(m['coverageRatio'], .2 / 60)
        self.assertTrue(m['partial'])
        self.assertTrue(m['currentInputAvailable'])

    def test_exact_cut_within_ramped_interval_independent_reference(self):
        e = SportEngine()
        points = []
        previous = None
        for i in range(601):
            stamp = BASE + i * 230_000_000
            value = (i * 13) % 301
            acquire(e, stamp, value)
            if previous:
                points.append((previous, (stamp, value)))
            previous = (stamp, value)
        m = recent(e)
        distance, seconds, mean = reference(points, stamp)
        self.assertAlmostEqual(m['distanceKm'], distance, places=13)
        self.assertEqual(m['coveredSeconds'], seconds)
        self.assertAlmostEqual(m['averageSpeedKmh'], mean, places=11)
        self.assertEqual(seconds, 60)
        self.assertFalse(m['partial'])
        self.assertEqual(m['derivedWindowStartNs'], stamp - 60 * NS)
        self.assertLess(m['firstContributingAcquiredNs'], m['derivedWindowStartNs'])
        self.assertEqual(m['lastContributingAcquiredNs'], stamp)
        self.assertEqual(m['anchorAcquiredNs'], stamp)
        self.assertEqual(m['origin'], 'simulation')
        self.assertEqual(m['sourceGeneration'], 1)
        self.assertLess(len(json.dumps(m, allow_nan=False)), 1800)

    def test_exact_boundary_drops_old_interval_and_long_gap_prunes(self):
        e = SportEngine()
        for i in range(302):
            acquire(e, BASE + i * STEP, 36)
        m = recent(e)
        self.assertEqual(m['retainedIntervals'], 300)
        self.assertEqual(m['firstContributingAcquiredNs'], BASE + STEP)
        acquire(e, BASE + 130 * NS, 72)
        m = recent(e)
        self.assertEqual(m['retainedIntervals'], 0)
        self.assertIsNone(m['distanceKm'])
        self.assertEqual(m['observedSpanSeconds'], 60)
        self.assertEqual(m['uncoveredSeconds'], 60)
        self.assertTrue(m['partial'])

    def test_duplicates_invalid_atomic_replay_future_out_of_order(self):
        e = SportEngine()
        acquire(e, BASE, 36)
        acquire(e, BASE + STEP, 72)
        before = copy.deepcopy(vars(e.trip.recent))
        acquire(e, BASE + STEP, 72, now=BASE + STEP + 1)
        self.assertEqual(vars(e.trip.recent), before)
        for obs in (Observation(math.nan, BASE + 2 * STEP),
                    Observation(math.inf, BASE + 2 * STEP),
                    Observation(300, BASE + 2 * STEP + 1),
                    Observation(36, BASE), Observation(73, BASE + STEP, origin='simulation')):
            with self.assertRaises(ValueError):
                e.accept(1, {'speed_kmh': obs}, BASE + 2 * STEP)
            self.assertEqual(vars(e.trip.recent), before)
        with self.assertRaises(ValueError):
            e.accept(1, {'speed_kmh': Observation(36, BASE + 2 * STEP),
                         'gps_position': Observation((100, 2, 1), BASE + 2 * STEP)}, BASE + 2 * STEP)
        self.assertEqual(vars(e.trip.recent), before)

    def test_stale_poll_missing_and_gap_never_bridge_or_move_anchor(self):
        e = SportEngine()
        acquire(e, BASE, 36)
        acquire(e, BASE + STEP, 36)
        before = recent(e)
        stale = recent(e, BASE + STEP + 500_000_000)
        self.assertFalse(stale['currentInputAvailable'])
        for key in set(before) - {'currentInputAvailable'}:
            self.assertEqual(stale[key], before[key])
        acquire(e, BASE + 800_000_000, 36)
        self.assertEqual(recent(e)['coveredSeconds'], .2)
        e.accept(1, {'speed_kmh': Observation(None, BASE + 900_000_000,
                 origin='simulation', availability='unavailable')}, BASE + 900_000_000)
        acquire(e, BASE + NS, 36)
        acquire(e, BASE + NS + STEP, 36)
        m = recent(e)
        self.assertEqual(m['coveredSeconds'], .4)
        self.assertAlmostEqual(m['uncoveredSeconds'], .8)
        acquire(e, BASE + 2 * NS, 36, now=BASE + 3 * NS)
        self.assertEqual(recent(e)['anchorAcquiredNs'], BASE + NS + STEP)

    def test_exact_age_and_maximum_gap_policies(self):
        for gap, age, covered in ((STEP, STEP, None), (250_000_000, 500_000_000, .25),
                                  (250_000_001, 500_000_000, None)):
            e = SportEngine()
            acquire(e, BASE, 36, age=age)
            acquire(e, BASE + gap, 36, age=age)
            self.assertEqual(recent(e)['coveredSeconds'], covered)

    def test_origin_generation_reset_and_archive_stay_separate(self):
        e = SportEngine()
        acquire(e, BASE, 36)
        acquire(e, BASE + STEP, 36)
        old = recent(e)
        acquire(e, BASE + 2 * STEP, 72, origin='measured')
        self.assertIsNone(recent(e)['distanceKm'])
        self.assertEqual(recent(e)['origin'], 'measured')
        acquire(e, BASE + 3 * STEP, 72, origin='measured')
        acquire(e, BASE + 2 * STEP + 1, 36, generation=2, now=BASE + 3 * STEP + 1)
        self.assertIsNone(recent(e)['distanceKm'])
        self.assertEqual(recent(e)['anchorAcquiredNs'], BASE + 2 * STEP + 1)
        e.reset_trip(BASE + 4 * STEP)
        acquire(e, BASE + 3 * STEP + 2, 36, generation=3, now=BASE + 4 * STEP + 1)
        self.assertIsNone(recent(e)['anchorAcquiredNs'])
        # Actual current input availability is independent of post-reset eligibility.
        self.assertTrue(recent(e)['currentInputAvailable'])
        acquire(e, BASE + 5 * STEP, 0, generation=3)
        acquire(e, BASE + 6 * STEP, 0, generation=3)
        self.assertEqual(recent(e)['distanceKm'], 0)
        for record in e.trip.model()['history']:
            self.assertNotIn('recent', record)
        self.assertNotIn('recent', e.trip._statistics())
        m = recent(e)
        m['distanceKm'] = 999
        self.assertEqual(recent(e)['distanceKm'], 0)
        self.assertEqual(old['distanceKm'], .002)

    def test_real_production_hard_capacity_and_loss_ages_out(self):
        e = SportEngine()
        for i in range(4100):
            acquire(e, BASE + i * 1_000_000, 36)
        m = recent(e)
        self.assertEqual(m['retainedIntervals'], 4096)
        self.assertTrue(m['limited'])
        self.assertTrue(m['partial'])
        self.assertEqual(m['coveredSeconds'], 4.096)
        self.assertAlmostEqual(m['uncoveredSeconds'], .003)
        self.assertLess(m['coverageRatio'], 1)
        self.assertEqual(len(e.trip.recent.intervals), 4096)
        self.assertEqual(e.trip.model()['coveredSeconds'], 4.099)
        acquire(e, BASE + 65 * NS, 36)
        self.assertFalse(recent(e)['limited'])
        self.assertTrue(recent(e)['partial'])
        self.assertIsNone(recent(e)['distanceKm'])
        e.reset_trip(BASE + 66 * NS)
        self.assertEqual(recent(e)['retainedIntervals'], 0)

    def test_existing_trip_limit_blocks_unaccepted_interval(self):
        e = SportEngine()
        with patch.object(type(e.trip), 'MAXIMUM_DISTANCE_KM', .002):
            for i in range(3):
                acquire(e, BASE + i * STEP, 36)
        self.assertTrue(e.trip.limit_reached)
        self.assertEqual(recent(e)['coveredSeconds'], .2)
        self.assertEqual(recent(e)['retainedIntervals'], 1)

    def test_actual_manager_source_adapter_status_reset_and_retirement(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw).resolve()
            manager = manager_fixtures.Tests().quarantine_fixture(root)
            source = manager.sport = SportSource(root)
            now = BASE
            try:
                with patch('sport_source.host_now_ns', side_effect=lambda: now):
                    for i in range(3):
                        now = BASE + i * STEP
                        source.ingest(packet(now, sequence=i+1, value='36'), now)
                    m = manager.request({'action': 'status'})['sport']['trip']['recent']
                    self.assertEqual(m['coveredSeconds'], .4)
                    self.assertEqual(m['anchorAcquiredNs'], now)
                    original = copy.deepcopy(source.packet_mark)
                    with self.assertRaises(ValueError):
                        source.ingest(packet(BASE, value='300'), now)
                    self.assertEqual(source.packet_mark, original)
                    now += 1
                    reply = manager.request({'action': 'trip_reset'})['sport']['trip']
                    self.assertIsNone(reply['recent']['distanceKm'])
                    self.assertNotIn('recent', reply['history'][-1])
                    for i in range(2):
                        now += STEP
                        source.ingest(packet(now, sequence=4+i, value='0'), now)
                    before = source.snapshot()['trip']['recent']
                    source.close()
                    retired = manager.request({'action': 'status'})['sport']['trip']['recent']
                    self.assertFalse(retired['currentInputAvailable'])
                    for key in set(before) - {'currentInputAvailable'}:
                        self.assertEqual(retired[key], before[key])
                    self.assertEqual(retired['distanceKm'], 0)
                    self.assertIsNone(SportSource(root).snapshot()['trip']['recent']['distanceKm'])
                    manager.sport = None
            finally:
                manager.sport = None
                manager.shutdown()

    def test_actual_socket_worker_retirement_retains_original_estimate(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw).resolve()
            source = SportSource(root, socket_path=root/'recent.sock')
            sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                source.start()
                first = host_now_ns()
                sender.sendto(packet(first, value='36'), str(source.path))
                deadline = time.monotonic() + 2
                while source.snapshot()['trip']['recent']['anchorAcquiredNs'] != first and time.monotonic() < deadline:
                    time.sleep(.005)
                time.sleep(.02)
                last = host_now_ns()
                sender.sendto(packet(last, sequence=2, value='36'), str(source.path))
                deadline = time.monotonic() + 2
                while source.snapshot()['trip']['recent']['anchorAcquiredNs'] != last and time.monotonic() < deadline:
                    time.sleep(.005)
                before = source.snapshot()['trip']['recent']
                self.assertEqual(before['anchorAcquiredNs'], last)
                self.assertAlmostEqual(before['distanceKm'], (last-first)/NS*.01)
                # Terminal ingress failure in the same actual owner before expiry.
                with source.lock:
                    source.socket.close()
                source.thread.join(timeout=2)
                self.assertFalse(source.thread.is_alive())
                retired = source.snapshot()['trip']['recent']
                self.assertFalse(retired['currentInputAvailable'])
                self.assertEqual(retired['anchorAcquiredNs'], last)
                self.assertEqual(retired['distanceKm'], before['distanceKm'])
            finally:
                source.close()
                sender.close()
            self.assertFalse(source.path.exists())
            self.assertFalse(source.thread.is_alive())


if __name__ == '__main__':
    unittest.main()
