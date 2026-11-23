# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual C++ admission/encoding -> private manager/MediaState regression.

Synthetic content and real owned child/files; no radio, display or phone.
"""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from media_state import IAP_FIELDS, iap_observation_model, validate_iap_observation
from phone_manager import Manager
from media_bridge import snapshot as bridge_snapshot

BINARY = sys.argv.pop(1)
NONCE = '0123456789abcdef0123456789abcdef'
PHONE = 'AA:BB:CC:DD:EE:01'


class Phones:
    def phones(self):
        return [{'address': PHONE}]

    def media(self, phone):
        return {'available': True, 'source': 'bluetooth', 'status': 'playing',
                'title': 'Bluetooth fixture', 'controls': ['next']}


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        before = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        result = subprocess.run([BINARY, '--publication-fixture'], check=True,
                                capture_output=True, text=True)
        after = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        self.rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(self.rows), 4)
        for row in self.rows[:3]:
            observation = row['iap_observation']
            self.assertLessEqual(before, int(observation['messageAcceptedMonotonicNs']))
            self.assertLessEqual(int(observation['messageAcceptedMonotonicNs']), after)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        display = self.root / 'standard.conf'
        display.write_text('unused boundary fixture')
        config = self.root / 'receiver.json'
        config.write_text(json.dumps({'pair_store': str(self.root / 'pairs'),
                                      'display_config': str(display)}))
        self.manager = Manager(config, self.root, display, (os.getuid(), os.getgid()), bluez=Phones())
        self.addCleanup(self.manager.shutdown)
        self.manager.state.select(PHONE)
        self.manager.active = PHONE
        self.child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        self.manager.child = self.child
        self.manager.carplay.reset(NONCE)

    def admit(self, row):
        # Own real private sidecar; same mode/owner and bounded parser as production.
        self.manager.iap_media_path.write_text(json.dumps(row))
        self.manager.iap_media_path.chmod(0o600)
        self.manager.tick()
        return self.manager.request({'action': 'status'})

    def test_actual_partial_and_periodic_publication_keep_original_field_dates(self):
        first = self.admit(self.rows[0])
        first_dates = first['media']['iap_observation']['fields']
        second = self.admit(self.rows[1])
        self.assertEqual(second['media']['status'], 'paused')
        self.assertEqual(second['media']['controls'], ['next'])
        dates = second['media']['iap_observation']['fields']
        for key in ('track_id', 'title', 'status'):
            self.assertEqual(dates[key], first_dates[key])
        self.assertIsNotNone(dates['position_ms'])
        self.assertIsNone(dates['artist'])
        third = self.admit(self.rows[2])
        self.assertEqual(third['media']['iap_observation'], second['media']['iap_observation'])
        on_disk = json.loads(self.manager.media.path.read_text())
        self.assertEqual(on_disk['iap_observation'], second['media']['iap_observation'])
        model = third['media_observations']['transportReport']['iapHostObservation']
        self.assertTrue(model['available'])
        self.assertEqual(model['scope']['trackId'], '7')
        self.assertFalse(model['freshnessGuaranteed'])
        self.assertFalse(model['playbackConfirmed'])
        self.assertIsNone(model['phoneAcquiredMonotonicNs'])
        self.assertIsNone(model['fields']['artist']['observedAgeNs'])
        # Standalone parser sees the SAME producer observation, without mtime dating.
        standalone, _ = bridge_snapshot(self.manager.iap_media_path)
        self.assertEqual(standalone['iap_observation'], second['media']['iap_observation'])

    def test_legacy_missing_dates_do_not_use_mtime_or_publication(self):
        row = dict(self.rows[0])
        row.pop('iap_observation')
        status = self.admit(row)
        model = status['media_observations']['transportReport']['iapHostObservation']
        self.assertFalse(model['available'])
        self.assertIsNone(model['messageAcceptedMonotonicNs'])
        self.assertTrue(all(value['acceptedMonotonicNs'] is None and value['observedAgeNs'] is None
                            for value in model['fields'].values()))
        self.assertTrue(status['media']['available'])

    def test_invalid_and_previous_launch_scope_cannot_date_current_metadata(self):
        for mutation in ('nonce', 'clock', 'date', 'extra', 'zero_identity'):
            with self.subTest(mutation=mutation):
                row = copy.deepcopy(self.rows[0])
                observation = row['iap_observation']
                if mutation == 'nonce':
                    observation['launchNonce'] = 'f' * 32
                elif mutation == 'clock':
                    observation['clock'] = 'filemtime'
                elif mutation == 'date':
                    observation['fields']['title'] = 0
                elif mutation == 'zero_identity':
                    observation['engineGeneration'] = '0'
                else:
                    observation['fields']['unknown'] = '10'
                status = self.admit(row)
                self.assertEqual(status['media']['title'], 'Fixture')
                self.assertEqual(status['media']['controls'], ['next'])
                self.assertNotIn('iap_observation', status['media'])
                self.assertFalse(status['media_observations']['transportReport']['iapHostObservation']['available'])

    def test_actual_child_exit_withdraws_observation_before_next_tick(self):
        self.admit(self.rows[0])
        self.child.terminate()
        self.child.wait(timeout=5)
        status = self.manager.request({'action': 'status'})
        self.assertFalse(status['media']['available'])
        self.assertFalse(status['media_observations']['transportReport']['iapHostObservation']['available'])
        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager'):
            self.manager.tick()
        self.assertFalse(json.loads(self.manager.media.path.read_text())['available'])

    def test_parser_clear_falls_back_without_old_observation(self):
        self.admit(self.rows[0])
        status = self.admit(self.rows[3])
        self.assertEqual(status['media']['source'], 'bluetooth')
        self.assertNotIn('iap_observation', status['media'])

    def test_age_has_no_ttl_future_or_missing_date_fabrication(self):
        observation = validate_iap_observation(self.rows[0]['iap_observation'])
        media = {'source': 'iap', 'iap_observation': observation}
        stamp = int(observation['fields']['title'])
        old = iap_observation_model(media, True, stamp + 100_000_000_000)
        self.assertEqual(old['fields']['title']['observedAgeNs'], '100000000000')
        self.assertFalse(old['freshnessGuaranteed'])
        future = iap_observation_model(media, True, stamp - 1)
        self.assertIsNone(future['fields']['title']['observedAgeNs'])
        self.assertEqual(future['fields']['title']['acceptedMonotonicNs'], str(stamp))
        retired = iap_observation_model(media, False, stamp + 1)
        self.assertFalse(retired['available'])
        self.assertTrue(all(row['acceptedMonotonicNs'] is None for row in retired['fields'].values()))
        # Copies cannot mutate the admitted producer observation.
        observation['fields']['title'] = None
        self.assertIsNotNone(self.rows[0]['iap_observation']['fields']['title'])
        self.assertEqual(set(retired['fields']), set(IAP_FIELDS))


if __name__ == '__main__':
    unittest.main()
