#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

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
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from sport_telemetry import SportEngine, Observation, NS, G
from sport_source import SportSource, decode, READINGS, LAMPS, simulated_observations, host_now_ns, VEHICLE_CLOCK
from phone_manager import Manager

BASE = 10 * NS


def speed(engine, seconds, value, origin='simulation', generation=1):
    acquired = BASE + round(seconds * NS)
    engine.accept(generation, {'speed_kmh': Observation(value, acquired, origin=origin)}, acquired)


def run_acceleration(engine):
    for i in range(41):
        speed(engine, i * .05, 0)
    for i in range(1, 261):
        speed(engine, 2 + i * .05, i)


def packet(now=BASE, sequence=1, generation=1, value='0', acceleration='0/9.80665', gps='48/2/1'):
    records = [f'{key}={value if key == "speed_kmh" else "-"},{now if key == "speed_kmh" else "-"},500000000,s,{"a" if key == "speed_kmh" else "u"}' for key in READINGS]
    records += [f'{key}=-,-,500000000,m,u' for key in LAMPS]
    records += [f'{key}=-,-,500000000,m,u' for key in ('transmission', 'drive_mode', 'trip', 'heading')]
    records += [f'acceleration={acceleration},{now},500000000,s,a',
                f'gps_position={gps},{now},500000000,s,a']
    return (f'HVO6 {generation} {sequence} {BASE} {now} drive_modes=- ' + ' '.join(records)).encode()


class EngineTests(unittest.TestCase):
    def test_exact_analytic_acceleration_and_quarter_mile(self):
        engine = SportEngine()
        run_acceleration(engine)
        model = engine.model(BASE + 15 * NS)
        expected = {'0-100': 5, '50-80': 1.5, '0-200': 10,
                    'quarter-mile': math.sqrt(2 * 402.336 / (20 / 3.6))}
        for name, duration in expected.items():
            result = model['timers'][name]['result']
            self.assertEqual(model['timers'][name]['phase'], 'complete')
            self.assertAlmostEqual(result['seconds'], duration, places=8)
            self.assertEqual(result['origin'], 'simulation')
        self.assertEqual(len([event for event in engine.history if event['kind'] == 'acceleration']), 4)

    def test_no_launch_without_dated_rest(self):
        engine = SportEngine()
        for i in range(1, 151):
            speed(engine, i * .05, i)
        self.assertIsNone(engine.timers['0-100']['result'])
        self.assertIsNotNone(engine.timers['50-80']['result'])

    def test_input_lists_cannot_mutate_acquired_values(self):
        engine = SportEngine()
        value = [G, -G]
        engine.accept(1, {'acceleration': Observation(value, BASE)}, BASE)
        value[0] = 99
        self.assertEqual(engine.model(BASE)['gMeter']['longitudinalG'], 1)
        engine.accept(1, {'acceleration': Observation([G, -G], BASE)}, BASE)

    def test_large_json_integers_are_rejected_without_float_overflow(self):
        engine = SportEngine()
        with self.assertRaises(ValueError):
            engine.set_line([10**1000, 2], [48, 2])
        with self.assertRaises(ValueError):
            engine.accept(1, {'speed_kmh': Observation(10**1000, BASE)}, BASE)

    def test_g_meter_peaks_and_independent_exact_expiry(self):
        engine = SportEngine()
        engine.accept(1, {'acceleration': Observation((G, -2 * G), BASE, 100_000_000, 'measured'),
                          'speed_kmh': Observation(0, BASE, origin='simulation')}, BASE)
        fresh = engine.model(BASE + 99_999_999)
        self.assertAlmostEqual(fresh['gMeter']['lateralG'], -2)
        stale = engine.model(BASE + 100_000_000)
        self.assertIsNone(stale['gMeter']['lateralG'])
        self.assertEqual(stale['gMeter']['peaks']['lateralNegativeG'], -2)
        self.assertEqual(stale['inputs']['acceleration']['acquiredNs'], BASE)
        self.assertEqual(stale['inputs']['speed_kmh']['availability'], 'available')

    def test_missing_and_zero_are_distinct(self):
        engine = SportEngine()
        self.assertIsNone(engine.model(BASE)['gMeter']['longitudinalG'])
        engine.accept(1, {'acceleration': Observation((0, 0), BASE)}, BASE)
        self.assertEqual(engine.model(BASE)['gMeter']['longitudinalG'], 0)
        engine.accept(1, {'acceleration': Observation(None, BASE + 1, availability='unavailable')}, BASE + 1)
        self.assertIsNone(engine.model(BASE + 1)['gMeter']['longitudinalG'])

    def test_gap_abort_cannot_produce_result(self):
        engine = SportEngine()
        for i in range(22):
            speed(engine, i * .05, 0)
        speed(engine, 1.1, 10)
        speed(engine, 1.5, 150)
        self.assertIsNone(engine.timers['0-100']['result'])
        self.assertEqual(engine.timers['0-100']['phase'], 'invalid')
        self.assertTrue(any(event['kind'] == 'aborted' for event in engine.history))

    def test_stale_heartbeat_and_explicit_withdrawal_abort(self):
        for availability in ('stale', 'unavailable', 'disconnected'):
            engine = SportEngine()
            for i in range(22):
                speed(engine, i * .05, 0)
            speed(engine, 1.1, 10)
            stamp = BASE + 1_150_000_000
            engine.accept(1, {'speed_kmh': Observation(None, stamp, availability=availability)}, stamp)
            speed(engine, 1.2, 150)
            self.assertIsNone(engine.timers['0-100']['result'])

    def test_expired_input_does_not_complete_timer(self):
        engine = SportEngine()
        for i in range(22):
            speed(engine, i * .05, 0)
        speed(engine, 1.1, 10)
        engine.accept(1, {'speed_kmh': Observation(150, BASE + 1_200_000_000)}, BASE + 2 * NS)
        self.assertIsNone(engine.timers['0-100']['result'])

    def test_restarts_and_provenance_abort(self):
        for origin, generation in (('measured', 1), ('simulation', 2)):
            engine = SportEngine()
            for i in range(22):
                speed(engine, i * .05, 0)
            speed(engine, 1.1, 10)
            speed(engine, 1.2, 150, origin, generation)
            self.assertIsNone(engine.timers['0-100']['result'])

    def test_conflict_atomic_and_policy_immutable(self):
        engine = SportEngine()
        speed(engine, 0, 0)
        with self.assertRaises(ValueError):
            engine.accept(1, {'acceleration': Observation((G, 0), BASE),
                              'speed_kmh': Observation(1, BASE)}, BASE)
        self.assertNotIn('acceleration', engine.inputs)
        for generation in (1, 2):
            with self.assertRaises(ValueError):
                engine.accept(generation, {'speed_kmh': Observation(0, BASE + 1, 2 * NS)}, BASE + 1)
        self.assertEqual(engine.generation, 1)

    def test_equal_date_does_not_rearm_after_expiry(self):
        engine = SportEngine()
        speed(engine, 0, 0)
        engine.advance(BASE + NS)
        obs = Observation(0, BASE, origin='simulation')
        engine.accept(1, {'speed_kmh': obs}, BASE + NS)
        self.assertIsNone(engine.previous_speed)
        self.assertFalse(engine.launch_armed)

    def test_reversed_and_future_dates_rejected(self):
        engine = SportEngine()
        speed(engine, 0, 0)
        with self.assertRaises(ValueError):
            engine.accept(1, {'speed_kmh': Observation(2, BASE + 1)}, BASE)
        with self.assertRaises(ValueError):
            engine.model(BASE - 1)

    def test_rolling_rearm_hysteresis_and_return_to_rest(self):
        engine = SportEngine()
        for i, value in enumerate((47, 51, 81, 79, 51, 81)):
            speed(engine, i * .05, value)
        events = [e for e in engine.history if e['kind'] == 'acceleration']
        self.assertEqual(len(events), 1)
        speed(engine, .3, 47)
        speed(engine, .35, 51)
        speed(engine, .4, 0)
        self.assertEqual(engine.timers['50-80']['phase'], 'invalid')

    def test_rearm_clears_current_elapsed_but_keeps_dated_result(self):
        engine = SportEngine()
        for index, value in enumerate((47, 51, 81, 47)):
            speed(engine, index * .05, value)
        self.assertEqual(engine.timers['50-80']['phase'], 'armed')
        self.assertIsNone(engine.timers['50-80']['elapsedSeconds'])
        self.assertIsNotNone(engine.timers['50-80']['result'])

    def test_bounded_history_and_new_sessions(self):
        engine = SportEngine()
        for i in range(110):
            engine.reset_session(BASE + i)
        self.assertEqual(len(engine.history), engine.HISTORY_LIMIT)
        self.assertEqual(engine.session_id, 111)
        self.assertTrue(all(state['result'] is None for state in engine.timers.values()))

    def test_lap_last_best_and_delta(self):
        engine = SportEngine()
        engine.set_line([48, 2.0007], [48, 2.0013])
        for i in range(1, 1241):
            stamp = BASE + i * 50_000_000
            engine.accept(1, {'gps_position': simulated_observations(i * .05, stamp)['gps_position']}, stamp)
        model = engine.model(stamp)
        self.assertAlmostEqual(model['laps']['last']['seconds'], 20, places=6)
        self.assertAlmostEqual(model['laps']['best']['seconds'], 20, places=6)
        self.assertAlmostEqual(model['laps']['deltaSeconds'], 0, places=5)
        self.assertEqual(model['laps']['best']['origin'], 'simulation')
        self.assertEqual(model['laps']['best']['line'], engine.line)
        original_line = copy.deepcopy(engine.line)
        engine.set_line([48, 2.0006], [48, 2.0014])
        self.assertEqual([event for event in engine.history if event['kind'] == 'lap'][-1]['line'], original_line)
        engine.advance(stamp + 500_000_000)
        self.assertEqual(engine.model(stamp + 500_000_000)['laps']['phase'], 'waiting')
        self.assertIsNone(engine.lap_delta)

    def test_delta_does_not_match_finish_at_next_lap_start(self):
        engine = SportEngine()
        engine.set_line([48, 2.0007], [48, 2.0013])
        for i in range(1, 1205):
            stamp = BASE + i * 50_000_000
            engine.accept(1, {'gps_position': simulated_observations(i * .05, stamp)['gps_position']}, stamp)
            if i >= 1200 and engine.lap_delta is not None:
                self.assertLess(abs(engine.lap_delta), .1)

    def test_peak_acquisition_and_model_copy_are_preserved(self):
        engine = SportEngine()
        engine.accept(1, {'acceleration': Observation((G, -G), BASE, origin='measured')}, BASE)
        model = engine.model(BASE)
        self.assertEqual(model['gMeter']['peakObservations']['longitudinalPositiveG']['acquiredNs'], BASE)
        self.assertIsNone(model['gMeter']['peaks']['longitudinalNegativeG'])
        model['gMeter']['peakObservations']['longitudinalPositiveG']['g'] = 99
        self.assertEqual(engine.model(BASE)['gMeter']['peakObservations']['longitudinalPositiveG']['g'], 1)

    def test_line_validation_and_direction_chatter(self):
        engine = SportEngine()
        for a, b in (([91, 2], [91, 3]), ([48, 2], [48, 2]), ([48, 2], [48, 3])):
            with self.assertRaises(ValueError):
                engine.set_line(a, b)
        engine.set_line([48, 1.9995], [48, 2.0005])
        for i in range(50):
            value = (48 + (0.00001 if i % 2 else -0.00001), 2, 1)
            stamp = BASE + i * 50_000_000
            engine.accept(1, {'gps_position': Observation(value, stamp)}, stamp)
        self.assertFalse(any(event['kind'] == 'lap' for event in engine.history))

    def test_bad_gps_accuracy_and_teleport_withdraw_lap(self):
        engine = SportEngine()
        engine.set_line([48, 1.9995], [48, 2.0005])
        for i, value in enumerate(((47.99999, 2, 1), (48.00001, 2, 1), (48.00002, 2, 10))):
            stamp = BASE + i * 50_000_000
            engine.accept(1, {'gps_position': Observation(value, stamp)}, stamp)
        self.assertIsNone(engine.lap_started)
        self.assertEqual(engine.lap_reason, 'GPS accuracy exceeds 5 m')


class SourceTests(unittest.TestCase):
    def test_source_uses_actual_vehicle_clock_domain(self):
        before = time.clock_gettime_ns(VEHICLE_CLOCK)
        observed = host_now_ns()
        after = time.clock_gettime_ns(VEHICLE_CLOCK)
        self.assertLessEqual(before, observed)
        self.assertLessEqual(observed, after)

    def test_hvo_decode_and_atomic_replay_rejection(self):
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(folder)
            source.ingest(packet(), BASE)
            with self.assertRaises(ValueError):
                source.ingest(packet(), BASE)
            bad = packet(BASE + 1, 2).replace(b'gps_position=48/2/1', b'gps_position=91/2/1')
            with self.assertRaises(ValueError):
                source.ingest(bad, BASE + 1)
            self.assertEqual(source.packet_mark[2], 1)
            source.ingest(packet(BASE + 1, 2), BASE + 1)
            self.assertEqual(source.packet_mark[2], 2)

    def test_full_uint64_source_counters_and_exact_small_accuracy(self):
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(folder)
            source.ingest(packet(sequence=2**64-1, generation=2**64-1, gps='48/2/1e-9'), BASE)
            self.assertEqual(source.packet_mark[1:3], (2**64-1, 2**64-1))
            self.assertEqual(source.engine.inputs['gps_position'].value[2], 1e-9)
            source.engine.reset_session(BASE + 1)
            source._save({'version': 1, 'sessionId': source.engine.session_id,
                          'history': list(source.engine.history), 'line': None})
            self.assertFalse(SportSource(folder).error)

    def test_bad_wire_metadata_and_shape(self):
        for data in (packet() + b' ', packet().replace(b'500000000,s,a', b'0,s,a', 1),
                     packet().replace(b'gps_position=', b'acceleration='),
                     packet().replace(b'0/9.80665', b'nan/0'),
                     packet().replace(b'0/9.80665', b'+0/9.80665'),
                     packet().replace(b'0/9.80665', b'0/9_806'),
                     packet().replace(b'0/9.80665', b'1e-999/0'),
                     packet().replace(b'HVO6', b'HVO5'), b'x' * 8193):
            with self.assertRaises((ValueError, UnicodeError)):
                decoded = decode(data, BASE)
                SportEngine().accept(decoded[1], decoded[5], BASE)

    def test_private_socket_delivery_stale_and_retirement(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            path = folder / 'sport.sock'
            source = SportSource(folder, socket_path=path)
            source.start()
            sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            now = host_now_ns()
            sender.sendto(packet(now), str(path))
            deadline = time.monotonic() + 2
            while source.packet_mark is None and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertIsNotNone(source.packet_mark)
            self.assertEqual(source.snapshot()['gMeter']['lateralG'], 1)
            time.sleep(.51)
            self.assertIsNone(source.snapshot()['gMeter']['lateralG'])
            source.close()
            sender.close()
            self.assertFalse(path.exists())
            self.assertFalse(source.thread.is_alive())

    def test_consumer_retirement_withdraws_fresh_values_without_redating(self):
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(folder)
            now = host_now_ns()
            source.engine.accept(1, {'acceleration': Observation((G, 0), now)}, now)
            source.close()
            model = source.snapshot()
            self.assertIsNone(model['gMeter']['longitudinalG'])
            self.assertEqual(model['inputs']['acceleration']['availability'], 'disconnected')
            self.assertEqual(model['inputs']['acceleration']['acquiredNs'], now)

    def test_duplicate_owner_and_foreign_endpoint_preserved(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw).resolve()
            source = SportSource(folder, socket_path=folder / 'sport.sock')
            source.start()
            duplicate = SportSource(folder, socket_path=folder / 'sport.sock')
            with self.assertRaises(OSError):
                duplicate.start()
            source.close()

    def test_persisted_history_is_historical_only(self):
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(folder)
            run_acceleration(source.engine)
            record = {'version': 1, 'sessionId': 1, 'history': list(source.engine.history), 'line': None}
            source._save(record)
            restored = SportSource(folder)
            self.assertEqual(len(restored.engine.history), 4)
            self.assertEqual(restored.engine.session_id, 2)
            self.assertTrue(all(state['result'] is None for state in restored.engine.timers.values()))
            self.assertFalse(restored.engine.inputs)

    def test_peak_rich_history_fits_private_file_bound(self):
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(folder)
            for index in range(70):
                stamp = BASE + index * NS
                source.engine.accept(1, {'acceleration': Observation((100, 100), stamp)}, stamp)
                source.engine.accept(1, {'acceleration': Observation((-100, -100), stamp + 1)}, stamp + 1)
                source.engine.reset_session(stamp + 2)
            source._save({'version': 1, 'sessionId': source.engine.session_id,
                          'history': list(source.engine.history), 'line': None})
            self.assertLessEqual(source.history_path.stat().st_size, 65536)
            restored = SportSource(folder)
            self.assertEqual(len(restored.engine.history), source.engine.HISTORY_LIMIT)
            self.assertFalse(restored.error)

    def test_reject_corrupt_and_symlink_history(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'sport-history.json'
            path.write_text('{bad')
            path.chmod(0o600)
            self.assertEqual(SportSource(folder).error, 'Sport history rejected')
            path.unlink()
            path.symlink_to('/dev/zero')
            self.assertEqual(SportSource(folder).error, 'Sport history unavailable')

    def test_frequent_status_reads_do_not_retire_simulated_worker(self):
        with tempfile.TemporaryDirectory() as folder:
            source = SportSource(folder, simulated=True)
            source.start()
            try:
                for _ in range(2000):
                    self.assertNotEqual(source.snapshot()['error'], 'Sport acquisition stopped')
                time.sleep(.05)
                self.assertIsNotNone(source.snapshot()['inputs']['speed_kmh']['value'])
            finally:
                source.close()

    def test_manager_request_validation_and_model(self):
        manager = Manager.__new__(Manager)
        with tempfile.TemporaryDirectory() as folder:
            manager.sport = SportSource(folder)
            self.assertTrue(manager.request({'action': 'sport_session'})['sport']['configured'])
            model = manager.request({'action': 'sport_line', 'line': [[48, 1.9995], [48, 2.0005]]})
            self.assertIsNotNone(model['sport']['laps']['line'])
            for data in ({'action': 'sport_session', 'phone': 'private'},
                         {'action': 'sport_line', 'line': []},
                         {'action': 'status', 'line': []}):
                with self.assertRaises(ValueError):
                    manager.request(data)
            manager.sport = None
            with self.assertRaises(ValueError):
                manager.request({'action': 'sport_session'})


if __name__ == '__main__':
    unittest.main()
