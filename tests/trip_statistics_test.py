#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Production sport-owner trip regression tests; values are explicit simulation."""
import copy
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
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sport_telemetry import SportEngine, Observation, NS
from sport_source import SportSource, host_now_ns
from sport_telemetry_test import packet
from phone_manager import Manager

BASE = 10 * NS
STEP = 200_000_000


def acquire(engine, stamp, value, generation=1, origin='simulation', now=None, age=500_000_000):
    engine.accept(generation, {'speed_kmh': Observation(value, stamp, age, origin)},
                  stamp if now is None else now)


def trip(engine, now):
    return engine.model(now)['trip']


class TripTests(unittest.TestCase):
    def test_unavailable_baseline_single_sample_and_known_stopped_interval(self):
        engine = SportEngine()
        empty = trip(engine, BASE)
        for key in ('distanceKm', 'coveredSeconds', 'movingSeconds', 'averageSpeedKmh',
                    'movingAverageSpeedKmh', 'peak', 'coverageRatio'):
            self.assertIsNone(empty[key])
        acquire(engine, BASE, 0)
        baseline = trip(engine, BASE)
        self.assertIsNone(baseline['distanceKm'])
        self.assertEqual(baseline['peak']['speedKmh'], 0)
        acquire(engine, BASE + STEP, 0)
        stopped = trip(engine, BASE + STEP)
        self.assertEqual(stopped['distanceKm'], 0)
        self.assertEqual(stopped['movingSeconds'], 0)
        self.assertEqual(stopped['averageSpeedKmh'], 0)
        self.assertIsNone(stopped['movingAverageSpeedKmh'])
        self.assertEqual(stopped['coveredSeconds'], .2)
        self.assertEqual(stopped['origin'], 'simulation')

    def test_analytic_constant_speed_and_polling_invariance(self):
        engine = SportEngine()
        for index in range(51):
            acquire(engine, BASE + index * STEP, 36)
        model = trip(engine, BASE + 10 * NS)
        self.assertAlmostEqual(model['distanceKm'], .1)
        self.assertEqual(model['coveredSeconds'], 10)
        self.assertEqual(model['movingSeconds'], 10)
        self.assertAlmostEqual(model['movingAverageSpeedKmh'], 36)
        self.assertEqual(model['coverageRatio'], 1)
        self.assertFalse(model['partial'])
        self.assertEqual(model['peak']['acquiredNs'], BASE)
        for index in range(100):
            again = trip(engine, BASE + 10 * NS + index * 1_000_000)
            for key in ('distanceKm', 'coveredSeconds', 'movingSeconds', 'peak', 'lastAcquiredNs'):
                self.assertEqual(again[key], model[key])
        # A new publication/sequence of an identical acquisition is not distance.
        acquire(engine, BASE + 10 * NS, 36, now=BASE + 10 * NS + 100_000_000)
        self.assertEqual(trip(engine, engine.last_now)['distanceKm'], model['distanceKm'])

    def test_analytic_ramp_and_moving_threshold_crossings(self):
        engine = SportEngine()
        acquire(engine, BASE, 0)
        acquire(engine, BASE + STEP, 1)
        rising = trip(engine, BASE + STEP)
        self.assertAlmostEqual(rising['distanceKm'], .1 / 3600)
        self.assertAlmostEqual(rising['movingSeconds'], .1)
        self.assertAlmostEqual(rising['movingDistanceKm'], .075 / 3600)
        self.assertAlmostEqual(rising['movingAverageSpeedKmh'], .75)
        acquire(engine, BASE + 2 * STEP, 0)
        falling = trip(engine, BASE + 2 * STEP)
        self.assertAlmostEqual(falling['movingSeconds'], .2)
        self.assertAlmostEqual(falling['distanceKm'], .2 / 3600)
        self.assertAlmostEqual(falling['movingAverageSpeedKmh'], .75)
        self.assertEqual(falling['peak']['acquiredNs'], BASE + STEP)
        # Constant acceleration from rest: 0 -> 72 km/h in two seconds,
        # distance 20 metres, all calculated from actual acquisition intervals.
        ramp = SportEngine()
        for index in range(11):
            acquire(ramp, BASE + index * STEP, 7.2 * index)
        self.assertAlmostEqual(trip(ramp, BASE + 2 * NS)['distanceKm'], .02)

    def test_delivery_delay_uses_original_acquisitions(self):
        engine = SportEngine()
        acquire(engine, BASE, 36, now=BASE + 300_000_000)
        acquire(engine, BASE + STEP, 36, now=BASE + 600_000_000)
        model = trip(engine, BASE + 600_000_000)
        self.assertEqual(model['coveredSeconds'], .2)
        self.assertAlmostEqual(model['distanceKm'], .002)
        self.assertEqual(model['firstAcquiredNs'], BASE)
        self.assertEqual(model['lastAcquiredNs'], BASE + STEP)

    def test_missing_breaks_even_short_gap_and_coverage_is_partial(self):
        engine = SportEngine()
        acquire(engine, BASE, 36)
        acquire(engine, BASE + STEP, 36)
        engine.accept(1, {'speed_kmh': Observation(None, BASE + 300_000_000,
                      origin='simulation', availability='unavailable')}, BASE + 300_000_000)
        acquire(engine, BASE + 400_000_000, 36)
        acquire(engine, BASE + 600_000_000, 36)
        model = trip(engine, BASE + 600_000_000)
        self.assertAlmostEqual(model['distanceKm'], .004)
        self.assertEqual(model['coveredSeconds'], .4)
        self.assertAlmostEqual(model['uncoveredSeconds'], .2)
        self.assertAlmostEqual(model['coverageRatio'], 2 / 3)
        self.assertTrue(model['partial'])

    def test_long_gap_and_independent_exact_age_expiry(self):
        engine = SportEngine()
        acquire(engine, BASE, 36, age=STEP)
        acquire(engine, BASE + STEP, 36, age=STEP)
        exact = trip(engine, BASE + STEP)
        self.assertIsNone(exact['distanceKm'])
        self.assertTrue(exact['partial'])
        acquire(engine, BASE + STEP + 100_000_000, 36, age=STEP)
        before = trip(engine, BASE + STEP + 299_999_999)
        expired = trip(engine, BASE + STEP + 300_000_000)
        self.assertEqual(expired['phase'], 'waiting')
        self.assertEqual(expired['distanceKm'], before['distanceKm'])
        engine.accept(1, {'gps_position': Observation((48, 2, 1), BASE + 600_000_000)},
                      BASE + 600_000_000)
        self.assertEqual(trip(engine, engine.last_now)['phase'], 'waiting')
        acquire(engine, BASE + 700_000_000, 36, age=STEP)
        self.assertEqual(trip(engine, engine.last_now)['coveredSeconds'], .1)
        # Gap exceeds the fixed 250ms bound even though both values are fresh.
        other = SportEngine()
        acquire(other, BASE, 36)
        acquire(other, BASE + 300_000_000, 36)
        self.assertIsNone(trip(other, other.last_now)['distanceKm'])

    def test_expired_queued_samples_never_supply_peak_or_distance(self):
        engine = SportEngine()
        acquire(engine, BASE, 300, now=BASE + NS)
        self.assertIsNone(trip(engine, BASE + NS)['peak'])
        acquire(engine, BASE + 2 * NS, 36)
        self.assertIsNone(trip(engine, engine.last_now)['distanceKm'])
        self.assertEqual(trip(engine, engine.last_now)['peak']['speedKmh'], 36)

    def test_rejection_is_atomic_for_trip_and_other_owner_inputs(self):
        engine = SportEngine()
        acquire(engine, BASE, 36)
        snapshot = copy.deepcopy(engine.__dict__)
        invalid = [Observation(math.nan, BASE + STEP), Observation(-1, BASE + STEP),
                   Observation(301, BASE + STEP), Observation(36, BASE - 1),
                   Observation(37, BASE, origin='simulation'),
                   Observation(36, BASE + STEP, maximum_age_ns=NS)]
        for obs in invalid:
            with self.assertRaises(ValueError):
                engine.accept(1, {'speed_kmh': obs}, BASE + STEP)
            self.assertEqual(engine.trip.model(), snapshot['trip'].model())
            self.assertEqual(engine.last_now, snapshot['last_now'])
        with self.assertRaises(ValueError):
            engine.accept(1, {'speed_kmh': Observation(72, BASE + STEP),
                              'gps_position': Observation((100, 2, 1), BASE + STEP)}, BASE + STEP)
        self.assertEqual(engine.trip.model(), snapshot['trip'].model())
        acquire(engine, BASE + STEP, 72)
        self.assertAlmostEqual(trip(engine, engine.last_now)['distanceKm'], .003)

    def test_source_generation_and_origin_do_not_mix_journeys(self):
        engine = SportEngine()
        acquire(engine, BASE, 36)
        acquire(engine, BASE + STEP, 36)
        acquire(engine, BASE + 2 * STEP, 72, generation=2)
        model = trip(engine, engine.last_now)
        self.assertIsNone(model['distanceKm'])
        self.assertEqual(model['history'][0]['endedReason'], 'Source generation changed')
        self.assertEqual(model['history'][0]['lastAcquiredNs'], BASE + STEP)
        acquire(engine, BASE + 3 * STEP, 72, generation=2, origin='measured')
        model = trip(engine, engine.last_now)
        self.assertEqual(model['origin'], 'measured')
        self.assertEqual(model['history'][-1]['origin'], 'simulation')
        self.assertEqual(model['history'][-1]['endedReason'], 'Speed origin changed')
        self.assertIsNone(model['distanceKm'])
        acquire(engine, BASE + 4 * STEP, 72, generation=2, origin='measured')
        self.assertAlmostEqual(trip(engine, engine.last_now)['distanceKm'], .004)

    def test_new_generation_fresh_older_baseline_and_reset_cutoff(self):
        engine = SportEngine()
        acquire(engine, BASE, 36)
        acquire(engine, BASE + STEP, 36)
        # The pure admitted-owner contract resets per-source watermarks.
        # Original dates inside each history entry remain incomparable across
        # generations; there is no segment from the prior source's acquisition.
        acquire(engine, BASE + 100_000_000, 72, generation=2, now=BASE + 250_000_000)
        baseline = trip(engine, engine.last_now)
        self.assertEqual(baseline['firstAcquiredNs'], BASE + 100_000_000)
        self.assertIsNone(baseline['distanceKm'])
        self.assertEqual(baseline['history'][0]['lastAcquiredNs'], BASE + STEP)
        acquire(engine, BASE + 300_000_000, 72, generation=2)
        self.assertAlmostEqual(trip(engine, engine.last_now)['distanceKm'], .004)
        acquire(engine, BASE + 400_000_000, 300, generation=3, now=BASE + NS)
        self.assertIsNone(trip(engine, engine.last_now)['peak'])
        acquire(engine, BASE + 1_100_000_000, 36, generation=3)
        self.assertIsNone(trip(engine, engine.last_now)['distanceKm'])
        engine.reset_trip(BASE + 1_200_000_000)
        acquire(engine, BASE + 1_150_000_000, 300, generation=4, now=BASE + 1_250_000_000)
        self.assertIsNone(trip(engine, engine.last_now)['peak'])
        acquire(engine, BASE + 1_300_000_000, 36, generation=4)
        self.assertIsNone(trip(engine, engine.last_now)['distanceKm'])

    def test_reset_requires_post_action_acquisition_and_sport_reset_is_independent(self):
        engine = SportEngine()
        acquire(engine, BASE, 36)
        acquire(engine, BASE + STEP, 36)
        engine.reset_trip(BASE + 300_000_000)
        acquire(engine, BASE + 250_000_000, 300, now=BASE + 310_000_000)
        model = trip(engine, engine.last_now)
        self.assertIsNone(model['peak'])
        self.assertIsNone(model['distanceKm'])
        self.assertAlmostEqual(model['history'][0]['distanceKm'], .002)
        self.assertEqual(model['history'][0]['endedReason'], 'User software reset')
        acquire(engine, BASE + 400_000_000, 36)
        acquire(engine, BASE + 600_000_000, 36)
        before = trip(engine, engine.last_now)
        engine.reset_session(BASE + 610_000_000)
        self.assertEqual(trip(engine, engine.last_now)['distanceKm'], before['distanceKm'])
        self.assertEqual(trip(engine, engine.last_now)['journeyId'], before['journeyId'])

    def test_history_is_bounded_and_model_copies_cannot_mutate_owner(self):
        engine = SportEngine()
        for index in range(21):
            stamp = BASE + index * NS
            acquire(engine, stamp, 36)
            engine.reset_trip(stamp + 1)
        model = trip(engine, engine.last_now)
        self.assertEqual(len(model['history']), 16)
        model['history'][0]['peak']['speedKmh'] = 999
        self.assertEqual(trip(engine, engine.last_now)['history'][0]['peak']['speedKmh'], 36)
        self.assertLess(len(json.dumps(model, allow_nan=False)), 16000)

    def test_capacity_stops_before_exceeding_bound_and_reset_recovers(self):
        # Exercise exact production capacity branches with small test-only
        # bounds; shipped limits have no user setting or runtime override.
        for bound, value in (('MAXIMUM_DURATION_NS', 2 * STEP),
                             ('MAXIMUM_DISTANCE_KM', .004)):
            engine = SportEngine()
            with patch.object(type(engine.trip), bound, value):
                for index in range(3):
                    acquire(engine, BASE + index * STEP, 36)
                permitted = trip(engine, engine.last_now)
                self.assertFalse(permitted['limitReached'])
                acquire(engine, BASE + 3 * STEP, 36)
                limited = trip(engine, engine.last_now)
                self.assertTrue(limited['limitReached'])
                self.assertEqual(limited['phase'], 'limited')
                self.assertEqual(limited['distanceKm'], permitted['distanceKm'])
                self.assertEqual(limited['coveredSeconds'], permitted['coveredSeconds'])
                acquire(engine, BASE + 4 * STEP, 36)
                self.assertEqual(trip(engine, engine.last_now)['coveredSeconds'], .4)
                engine.reset_trip(engine.last_now + 1)
                acquire(engine, BASE + 5 * STEP, 36)
                acquire(engine, BASE + 6 * STEP, 36)
                self.assertFalse(trip(engine, engine.last_now)['limitReached'])
                self.assertEqual(trip(engine, engine.last_now)['coveredSeconds'], .2)

    def test_manager_action_validation_and_no_vehicle_or_sport_reset(self):
        with tempfile.TemporaryDirectory() as folder:
            manager = Manager.__new__(Manager)
            manager.sport = SportSource(folder)
            acquire(manager.sport.engine, BASE, 36)
            acquire(manager.sport.engine, BASE + STEP, 36)
            session = manager.sport.engine.session_id
            with patch('sport_source.host_now_ns', return_value=BASE + 300_000_000):
                model = manager.request({'action': 'trip_reset'})['sport']['trip']
                self.assertIsNone(model['distanceKm'])
                self.assertEqual(manager.sport.engine.session_id, session)
                for request in ({'action': 'trip_reset', 'phone': 'x'},
                                {'action': 'trip_reset', 'line': []}):
                    with self.assertRaises(ValueError):
                        manager.request(request)
            manager.sport = None
            with self.assertRaises(ValueError):
                manager.request({'action': 'trip_reset'})

    def test_actual_worker_failure_withdraws_trip_immediately(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            source = SportSource(folder, socket_path=folder / 'failed.sock')
            source.start()
            sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                acquired = host_now_ns()
                sender.sendto(packet(acquired, value='36'), str(source.path))
                deadline = time.monotonic() + 2
                while source.snapshot()['trip']['peak'] is None and time.monotonic() < deadline:
                    time.sleep(.005)
                self.assertIsNotNone(source.snapshot()['trip']['peak'])
                # Terminal ingress failure in the actual owner, before age expiry.
                with source.lock:
                    source.socket.close()
                source.thread.join(timeout=2)
                self.assertFalse(source.thread.is_alive())
                model = source.snapshot()
                self.assertEqual(model['error'], 'Sport acquisition stopped')
                self.assertEqual(model['trip']['phase'], 'waiting')
                self.assertTrue(model['trip']['partial'])
                self.assertEqual(model['trip']['lastAcquiredNs'], acquired)
                self.assertEqual(model['inputs']['speed_kmh']['availability'], 'disconnected')
                self.assertIsNone(model['inputs']['speed_kmh']['value'])
            finally:
                sender.close()
                source.close()
            self.assertFalse(source.path.exists())

    def test_actual_socket_hvo_acquisitions_replay_retirement_and_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder).resolve()
            endpoint = folder / 'trip-consumer.sock'
            source = SportSource(folder, socket_path=str(endpoint))
            sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            def wait_for(predicate):
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    model = source.snapshot()
                    if predicate(model):
                        return model
                    time.sleep(.005)
                self.fail('Production acquisition worker did not publish expected trip state')
            try:
                source.start()
                first = host_now_ns()
                sender.sendto(packet(first, value='36'), str(endpoint))
                wait_for(lambda model: model['trip']['firstAcquiredNs'] == first)
                time.sleep(.02)
                second = host_now_ns()
                sender.sendto(packet(second, sequence=2, value='36'), str(endpoint))
                model = wait_for(lambda model: model['trip']['lastAcquiredNs'] == second)
                self.assertAlmostEqual(model['trip']['distanceKm'], (second - first) / NS * .01)
                self.assertEqual(model['trip']['coveredSeconds'], (second - first) / NS)
                sender.sendto(packet(first, value='300'), str(endpoint))
                rejected = wait_for(lambda model: model['rejectedPackets'] == 1)
                self.assertEqual(rejected['trip']['peak']['speedKmh'], 36)
                stale = wait_for(lambda model: model['inputs']['speed_kmh']['availability'] == 'stale')
                self.assertTrue(stale['trip']['partial'])
                self.assertEqual(stale['trip']['lastAcquiredNs'], second)
                self.assertEqual(stale['trip']['distanceKm'], model['trip']['distanceKm'])
            finally:
                source.close()
                sender.close()
            retired = source.snapshot()
            self.assertEqual(retired['inputs']['speed_kmh']['availability'], 'disconnected')
            self.assertEqual(retired['trip']['phase'], 'waiting')
            self.assertEqual(retired['trip']['lastAcquiredNs'], second)
            self.assertFalse(endpoint.exists())
            self.assertFalse(source.thread.is_alive())
            restarted = SportSource(folder, socket_path=str(endpoint))
            self.assertIsNone(restarted.snapshot()['trip']['distanceKm'])
            self.assertEqual(restarted.snapshot()['trip']['history'], [])
            restarted.close()


if __name__ == '__main__':
    unittest.main()
