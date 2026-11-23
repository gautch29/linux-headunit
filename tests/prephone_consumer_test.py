# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Explicit simulated clock/data through the actual bounded consumer seam.

There is no production publisher or socket/file bridge in this fixture.
"""
from copy import deepcopy
import json
from pathlib import Path
import sys
import subprocess
import threading
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prephone_consumer import PrephoneConsumer, decode, ROWS, LIMIT

START = 1_000_000_000
AGE = 500_000_000
FIXTURE = sys.argv.pop(1) if len(sys.argv) == 2 else None


def packet(generation=1, started=START, published=START + 100, values=None):
    result = {'version': 1, 'state': 'admitted', 'clockDomain': 'host-steady',
              'sourceGeneration': str(generation), 'sourceStartedNs': str(started),
              'sourceSampledNs': str(published), 'rows': []}
    for definition in ROWS:
        row = dict(zip(('id', 'group', 'label', 'unit'), definition[:4]),
                   availability='missing', value=None, acquiredNs=None,
                   maximumAgeNs=str(AGE), origin=None)
        if values and definition[0] in values:
            value, acquired, origin = values[definition[0]]
            row.update(value=value, acquiredNs=str(acquired), origin=origin,
                       availability='available' if value is not None else 'unavailable')
        result['rows'].append(row)
    return result


def raw(model):
    return json.dumps(model, allow_nan=False, separators=(',', ':')).encode('ascii')


def admitted(model=None):
    consumer = PrephoneConsumer()
    consumer.begin('existing-owner-A', 1, START, START + 100, 'host-steady')
    consumer.accept(raw(packet() if model is None else model), 'existing-owner-A', START + 100)
    return consumer


def fuel(model):
    return model['rows'][0]


class PrephoneConsumerTests(unittest.TestCase):
    def test_actual_cpp_cache_codec_receiver_json_consumer_retirement(self):
        if FIXTURE is None:
            self.fail('Registered interop fixture executable is required')
        result = subprocess.run([FIXTURE], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=5, check=True)
        self.assertLessEqual(len(result.stdout), 6 * (LIMIT + 1))
        records = result.stdout.splitlines()
        self.assertEqual(len(records), 6)
        consumer = PrephoneConsumer()
        consumer.begin('actual-fixture-owner', 7, 1_000_000_000, 1_000_000_000, 'host-steady')
        consumer.accept(records[0], 'actual-fixture-owner', 1_000_000_000)
        self.assertEqual(fuel(consumer.snapshot(1_000_000_000))['availability'], 'missing')
        consumer.accept(records[1], 'actual-fixture-owner', 1_020_000_000)
        current = consumer.snapshot(1_020_000_000)
        self.assertEqual([row['id'] for row in current['rows']],
                         ['fuel_percent', 'range_km', 'battery_voltage', 'ambient_c',
                          'coolant_c', 'oil_c', 'oil_pressure_kpa', 'odometer_km'])
        self.assertEqual(fuel(current)['value'], 0)
        self.assertEqual(fuel(current)['maximumAgeNs'], '100000000')
        self.assertEqual(current['rows'][1]['origin'], 'calculated')
        self.assertEqual(current['rows'][3]['value'], -12.5)
        self.assertEqual(current['rows'][5]['availability'], 'unavailable')
        consumer.accept(records[2], 'actual-fixture-owner', 1_110_000_000)
        expired = consumer.snapshot(1_110_000_000)
        self.assertEqual(fuel(expired)['availability'], 'stale')
        self.assertEqual(expired['rows'][3]['availability'], 'available')
        self.assertEqual(expired['sourceSampledNs'], '1020000000')
        consumer.accept(records[3], 'actual-fixture-owner', 1_111_000_000)
        self.assertEqual(consumer.snapshot(1_111_000_000)['state'], 'disconnected')
        with self.assertRaises(ValueError):
            consumer.accept(records[1], 'actual-fixture-owner', 1_112_000_000)
        consumer.begin('actual-replacement', 8, 1_112_000_000, 1_112_000_000, 'host-steady')
        with self.assertRaises(ValueError):
            consumer.accept(records[3], 'actual-fixture-owner', 1_112_000_000)
        consumer.accept(records[4], 'actual-replacement', 1_112_000_000)
        self.assertIsNone(fuel(consumer.snapshot(1_112_000_000))['value'])
        consumer.accept(records[5], 'actual-replacement', 1_113_000_000)
        self.assertEqual(fuel(consumer.snapshot(1_113_000_000))['value'], 5)

    def test_initial_and_admitted_missing_are_not_zero_or_fitted(self):
        consumer = PrephoneConsumer()
        initial = consumer.snapshot(START)
        self.assertEqual(initial['state'], 'awaiting_source')
        self.assertIsNone(initial['sourceSampledNs'])
        self.assertIsNone(fuel(initial)['maximumAgeNs'])
        consumer = admitted()
        model = consumer.snapshot(START + 100)
        self.assertEqual(model['sourceGeneration'], '1')
        self.assertTrue(all(row['availability'] == 'missing' and row['value'] is None
                            and row['origin'] is None for row in model['rows']))
        self.assertNotIn('fitted', model)
        self.assertNotIn('warningsClear', model)

    def test_zero_negative_and_calculated_range_keep_units_and_origin(self):
        model = packet(values={'fuel_percent': (0, START, 'simulation'),
                               'ambient_c': (-12.5, START + 50, 'measured'),
                               'range_km': (123.25, START + 75, 'calculated')})
        result = admitted(model).snapshot(START + 100)
        self.assertEqual(fuel(result)['value'], 0)
        self.assertEqual(result['rows'][3]['value'], -12.5)
        self.assertEqual(result['rows'][1]['origin'], 'calculated')
        self.assertEqual(result['rows'][1]['unit'], 'KM')
        self.assertEqual(result['sourceSampledNs'], str(START + 100))

    def test_independent_exact_expiry_and_poll_do_not_renew_dates(self):
        model = packet(values={'fuel_percent': (0, START, 'simulation'),
                               'ambient_c': (-5, START + 50, 'measured')})
        consumer = admitted(model)
        self.assertEqual(fuel(consumer.snapshot(START + AGE - 1))['availability'], 'available')
        expired = consumer.snapshot(START + AGE)
        self.assertEqual(fuel(expired)['availability'], 'stale')
        self.assertIsNone(fuel(expired)['value'])
        self.assertEqual(expired['rows'][3]['availability'], 'available')
        self.assertEqual(fuel(expired)['acquiredNs'], str(START))
        self.assertEqual(expired['sourceSampledNs'], str(START + 100))
        self.assertEqual(consumer.snapshot(START + AGE + 50)['rows'][3]['availability'], 'stale')

    def test_available_queued_past_expiry_is_admitted_as_stale(self):
        consumer = PrephoneConsumer()
        consumer.begin('A', 1, START, START, 'host-steady')
        model = packet(values={'fuel_percent': (0, START, 'simulation')})
        consumer.accept(raw(model), 'A', START + AGE)
        self.assertEqual(fuel(consumer.snapshot(START + AGE))['availability'], 'stale')

    def test_stale_projection_may_keep_older_original_source_publication(self):
        model = packet(values={'fuel_percent': (0, START, 'simulation')})
        fuel(model).update(value=None, availability='stale')
        consumer = PrephoneConsumer()
        consumer.begin('A', 1, START, START + AGE, 'host-steady')
        consumer.accept(raw(model), 'A', START + AGE)
        self.assertEqual(consumer.snapshot(START + AGE)['sourceSampledNs'], str(START + 100))

    def test_observed_unavailable_is_distinct_from_missing(self):
        result = admitted(packet(values={'fuel_percent': (None, START, 'measured')})).snapshot(START + 100)
        self.assertEqual(fuel(result)['availability'], 'unavailable')
        self.assertEqual(fuel(result)['origin'], 'measured')
        self.assertEqual(result['rows'][1]['availability'], 'missing')

    def test_terminal_retirement_and_old_publisher_cannot_revive(self):
        model = packet(values={'fuel_percent': (0, START, 'simulation')})
        consumer = admitted(model)
        consumer.retire('existing-owner-A', 1, START + 101)
        result = consumer.snapshot(START + 101)
        self.assertEqual(result['state'], 'disconnected')
        self.assertTrue(all(row['availability'] == 'disconnected' for row in result['rows']))
        self.assertEqual(fuel(result)['acquiredNs'], str(START))
        for publisher in ('existing-owner-A', 'replacement'):
            with self.assertRaises(ValueError):
                consumer.accept(raw(model), publisher, START + 102)
        with self.assertRaises(ValueError):
            consumer.begin('replacement', 1, START + 102, START + 102, 'host-steady')
        consumer.begin('replacement', 2, START + 101, START + 102, 'host-steady')
        self.assertIsNone(consumer.snapshot(START + 102)['sourceGeneration'])
        with self.assertRaises(ValueError):
            consumer.accept(raw(model), 'existing-owner-A', START + 103)
        fresh = packet(2, START + 101, START + 103)
        consumer.accept(raw(fresh), 'replacement', START + 103)
        self.assertEqual(fuel(consumer.snapshot(START + 103))['availability'], 'missing')

    def test_generation_start_floor_uses_source_publication_not_later_poll(self):
        consumer = admitted()
        consumer.snapshot(START + AGE)
        consumer.begin('replacement', 2, START + 101, START + AGE, 'host-steady')
        consumer.accept(raw(packet(2, START + 101, START + 102)), 'replacement', START + AGE)
        self.assertEqual(consumer.snapshot(START + AGE)['sourceSampledNs'], str(START + 102))

    def test_wrong_authority_generation_domain_and_source_start_are_atomic(self):
        consumer = admitted()
        original = consumer.snapshot(START + 100)
        for publisher, change in (('foreign', {}), ('existing-owner-A', {'sourceGeneration': '2'}),
                                  ('existing-owner-A', {'clockDomain': 'wall-clock'}),
                                  ('existing-owner-A', {'sourceStartedNs': str(START - 1)})):
            model = packet()
            model.update(change)
            with self.assertRaises(ValueError):
                consumer.accept(raw(model), publisher, START + 101)
            self.assertEqual(consumer.snapshot(START + 100), original)
        with self.assertRaises(ValueError):
            consumer.retire('foreign', 1, START + 101)
        self.assertEqual(consumer.snapshot(START + 100), original)

    def test_invalid_last_row_does_not_commit_first_row_or_clock(self):
        consumer = admitted()
        bad = packet(published=START + 200, values={'fuel_percent': (4, START + 150, 'measured')})
        bad['rows'][-1]['value'] = 0
        with self.assertRaises(ValueError):
            consumer.accept(raw(bad), 'existing-owner-A', START + 200)
        self.assertEqual(fuel(consumer.snapshot(START + 100))['availability'], 'missing')
        good = deepcopy(bad)
        good['rows'][-1]['value'] = None
        consumer.accept(raw(good), 'existing-owner-A', START + 200)
        self.assertEqual(fuel(consumer.snapshot(START + 200))['value'], 4)

    def test_last_row_conflict_does_not_consume_first_acquisition_watermark(self):
        consumer = admitted(packet(values={'fuel_percent': (0, START, 'simulation'),
                                           'odometer_km': (10, START, 'measured')}))
        bad = packet(published=START + 200,
                     values={'fuel_percent': (4, START + 150, 'simulation'),
                             'odometer_km': (11, START, 'measured')})
        with self.assertRaises(ValueError):
            consumer.accept(raw(bad), 'existing-owner-A', START + 200)
        good = packet(published=START + 140,
                      values={'fuel_percent': (5, START + 140, 'simulation'),
                              'odometer_km': (10, START, 'measured')})
        consumer.accept(raw(good), 'existing-owner-A', START + 140)
        result = consumer.snapshot(START + 140)
        self.assertEqual(fuel(result)['value'], 5)
        self.assertEqual(result['rows'][-1]['value'], 10)

    def test_equal_date_heartbeat_conflicts_and_withdrawal_nonrevival(self):
        model = packet(values={'fuel_percent': (0, START, 'simulation')})
        consumer = admitted(model)
        heartbeat = deepcopy(model)
        heartbeat['sourceSampledNs'] = str(START + 101)
        consumer.accept(raw(heartbeat), 'existing-owner-A', START + 101)
        for change in ({'value': 1}, {'origin': 'measured'}, {'availability': 'unavailable', 'value': None}):
            bad = deepcopy(heartbeat)
            fuel(bad).update(change)
            with self.assertRaises(ValueError):
                consumer.accept(raw(bad), 'existing-owner-A', START + 102)
        withdrawn = deepcopy(heartbeat)
        fuel(withdrawn).update(availability='disconnected', value=None)
        consumer.accept(raw(withdrawn), 'existing-owner-A', START + 102)
        with self.assertRaises(ValueError):
            consumer.accept(raw(heartbeat), 'existing-owner-A', START + 103)
        fresh = packet(published=START + 104, values={'fuel_percent': (2, START + 104, 'simulation')})
        consumer.accept(raw(fresh), 'existing-owner-A', START + 104)
        self.assertEqual(fuel(consumer.snapshot(START + 104))['value'], 2)

    def test_snapshot_expiry_watermark_rejects_renewed_old_value(self):
        model = packet(values={'fuel_percent': (0, START, 'simulation')})
        consumer = admitted(model)
        consumer.snapshot(START + AGE)
        with self.assertRaises(ValueError):
            consumer.accept(raw(model), 'existing-owner-A', START + AGE)
        fresh = packet(published=START + AGE, values={'fuel_percent': (3, START + AGE - 1, 'simulation')})
        consumer.accept(raw(fresh), 'existing-owner-A', START + AGE)
        self.assertEqual(fuel(consumer.snapshot(START + AGE))['value'], 3)

    def test_acquisition_and_publication_reversal_or_drop_are_atomic(self):
        consumer = admitted(packet(values={'fuel_percent': (0, START + 50, 'simulation')}))
        for model in (packet(published=START + 99), packet(),
                      packet(values={'fuel_percent': (0, START + 49, 'simulation')})):
            with self.assertRaises(ValueError):
                consumer.accept(raw(model), 'existing-owner-A', START + 101)
        self.assertEqual(fuel(consumer.snapshot(START + 100))['acquiredNs'], str(START + 50))

    def test_age_policy_is_immutable_across_generations(self):
        consumer = admitted()
        consumer.begin('B', 2, START + 101, START + 101, 'host-steady')
        model = packet(2, START + 101, START + 101)
        fuel(model)['maximumAgeNs'] = str(AGE + 1)
        with self.assertRaises(ValueError):
            consumer.accept(raw(model), 'B', START + 102)
        fuel(model)['maximumAgeNs'] = str(AGE)
        consumer.accept(raw(model), 'B', START + 101)

    def test_disconnected_projection_terminally_retires_whole_source(self):
        model = packet()
        model['state'] = 'disconnected'
        for row in model['rows']:
            row['availability'] = 'disconnected'
        consumer = admitted(model)
        self.assertEqual(consumer.snapshot(START + 100)['state'], 'disconnected')
        with self.assertRaises(ValueError):
            consumer.accept(raw(packet()), 'existing-owner-A', START + 101)

    def test_unavailable_history_survives_per_row_withdrawal(self):
        model = packet(values={'fuel_percent': (None, START, 'measured')})
        consumer = admitted(model)
        withdrawn = deepcopy(model)
        fuel(withdrawn)['availability'] = 'disconnected'
        consumer.accept(raw(withdrawn), 'existing-owner-A', START + 101)
        stale = deepcopy(model)
        fuel(stale)['availability'] = 'stale'
        with self.assertRaises(ValueError):
            consumer.accept(raw(stale), 'existing-owner-A', START + AGE)
        consumer.accept(raw(model), 'existing-owner-A', START + 101)
        self.assertEqual(fuel(consumer.snapshot(START + 101))['availability'], 'unavailable')

    def test_read_model_is_an_independent_copy(self):
        consumer = admitted(packet(values={'fuel_percent': (0, START, 'simulation')}))
        result = consumer.snapshot(START + 100)
        fuel(result).update(value=99, acquiredNs=str(START + 100))
        result['rows'].clear()
        self.assertEqual(fuel(consumer.snapshot(START + 100))['value'], 0)

    def test_exact_64bit_strings_round_trip_without_float_dates(self):
        now = 2**63 - 1
        model = packet(2**64 - 1, now - 100, now, {'fuel_percent': (0, now - 1, 'simulation')})
        consumer = PrephoneConsumer()
        consumer.begin('A', 2**64 - 1, now - 100, now, 'host-steady')
        consumer.accept(raw(model), 'A', now)
        self.assertEqual(consumer.snapshot(now), model)

    def test_wire_schema_and_bounded_hostile_json_fail_closed(self):
        good = raw(packet())
        bad = [b'', good + b' ' * LIMIT, good.decode(), b'\xff', b'[' * 3000 + b']' * 3000,
               good.replace(b'"version":1', b'"version":1,"version":1'),
               good.replace(b'"version":1', b'"version":true'),
               good.replace(b'"value":null', b'"value":NaN', 1),
               good.replace(b'"value":null', b'"value":1e9999', 1),
               good.replace(b'"value":null', b'"value":1e-9999', 1),
               good.replace(b'"sourceGeneration":"1"', b'"sourceGeneration":1'),
               good.replace(b'"sourceGeneration":"1"', b'"sourceGeneration":"01"')]
        for data in bad:
            with self.subTest(data=str(data)[:50]), self.assertRaises(ValueError):
                decode(data, START + 100)
        for change in ({'rows': []}, {'extra': 1}, {'state': 'invalid_snapshot'},
                       {'sourceGeneration': str(2**64)}, {'sourceSampledNs': str(START + 101)},
                       {'sourceStartedNs': '0'}):
            model = packet()
            model.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                decode(raw(model), START + 100)

    def test_row_bounds_catalogue_and_presence_are_exact(self):
        for change in ({'value': True, 'availability': 'available', 'acquiredNs': str(START), 'origin': 'measured'},
                       {'unit': 'L'}, {'id': 'speed_kmh'}, {'label': 'FAULT CLEAR'}, {'fitted': True},
                       {'maximumAgeNs': str(86_400_000_000_001)}, {'acquiredNs': str(START - 1)},
                       {'origin': 'simulation'}, {'availability': 'unavailable'},
                       {'availability': 'stale', 'acquiredNs': str(START), 'origin': 'measured'},
                       {'value': 101, 'availability': 'available', 'acquiredNs': str(START), 'origin': 'measured'}):
            model = packet()
            fuel(model).update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                decode(raw(model), START + 100)
        model = packet()
        model['rows'].reverse()
        with self.assertRaises(ValueError):
            decode(raw(model), START + 100)

    def test_owner_thread_clock_reversal_and_invalid_admission(self):
        consumer = admitted()
        errors = []
        def foreign():
            try:
                consumer.snapshot(START + 101)
            except ValueError as error:
                errors.append(str(error))
        thread = threading.Thread(target=foreign)
        thread.start()
        thread.join()
        self.assertEqual(len(errors), 1)
        for now in (START + 99, True, 0, 2**63):
            with self.assertRaises(ValueError):
                consumer.snapshot(now)
        for publisher, generation, started, domain in (('', 2, START + 101, 'host-steady'),
                                                       ('A', True, START + 101, 'host-steady'),
                                                       ('A', 2, START + 99, 'host-steady'),
                                                       ('A', 2, START + 101, 'wall-clock')):
            with self.assertRaises(ValueError):
                consumer.begin(publisher, generation, started, START + 101, domain)
        self.assertEqual(consumer.snapshot(START + 100)['sourceGeneration'], '1')


if __name__ == '__main__':
    unittest.main()
