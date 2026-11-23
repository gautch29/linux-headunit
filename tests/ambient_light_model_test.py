# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import copy
import json
from pathlib import Path
import subprocess
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from ambient_light_model import screen_accent, view_model


class AmbientModelTest(unittest.TestCase):
    def setUp(self):
        self.state = {'nonce': 'a' * 32, 'connectionGeneration': '1', 'ready': True,
                      'idle': True, 'transitioning': False, 'pending': '',
                      'active': 'layout', 'confirmedActive': 'layout', 'activePalette': 'blue',
                      'updatedMonotonicNs': '1000000000'}
        self.publisher = ('a' * 32, '1')

    def test_only_observed_current_palette(self):
        accent = screen_accent(self.state, {'blue': [0, 0, 255, 0]}, self.publisher, 1000000001)
        self.assertEqual(accent['acquired_ns'], '1000000000')
        self.assertEqual(accent['origin'], 'calculated')
        for key, value in [('pending', 'other'), ('ready', False), ('idle', False),
                           ('transitioning', True), ('confirmedActive', 'other'),
                           ('connectionGeneration', '2'), ('nonce', 'b' * 32), ('activePalette', 'unknown')]:
            state = dict(self.state, **{key: value})
            self.assertIsNone(screen_accent(state, {'blue': [0, 0, 255, 0]}, self.publisher, 1000000001))
        for now in (999999999, 2000000000):
            self.assertIsNone(screen_accent(self.state, {'blue': [0, 0, 255, 0]}, self.publisher, now))
        for colour in ([True, 0, 0, 0], [256, 0, 0, 0], [0, 0, 0], None):
            self.assertIsNone(screen_accent(self.state, {'blue': colour}, self.publisher, 1000000001))

    def test_real_cpp_simulator_interop(self):
        root = Path(__file__).resolve().parents[1]
        run = subprocess.run([sys.argv[1], str(root / 'config/ambient-light.example.json'),
                              str(root / 'config/ambient-controls.example.json')],
                             capture_output=True, text=True, check=True, timeout=15)
        snapshots = [json.loads(line) for line in run.stdout.splitlines()]
        self.assertEqual(len(snapshots), 15)
        models = [view_model(s, int(s['sampled_ns'])) for s in snapshots]
        self.assertTrue(all(m['available'] for m in models))
        self.assertIsNone(models[0]['current'])
        self.assertEqual(models[0]['command']['status'], 'pending')
        self.assertEqual(models[5]['command']['status'], 'pending')
        self.assertEqual(models[5]['current']['origin'], 'simulation')
        self.assertEqual(models[10]['command']['status'], 'applied')
        self.assertEqual(models[11]['requested']['value']['brightness_permille'], 60)
        self.assertEqual(models[11]['current']['value']['brightness_permille'], 300)
        self.assertIsNone(models[13]['requested'])
        self.assertIsNotNone(models[13]['current'])
        self.assertIsNone(models[14]['current'])
        self.assertIsNone(models[14]['requested'])
        final = snapshots[12]
        self.assertFalse(view_model(final, int(final['sampled_ns']) + 1000000000)['available'])
        malformed = copy.deepcopy(final)
        malformed['current']['value']['transition_ms'] = 1
        self.assertFalse(view_model(malformed, int(final['sampled_ns']))['available'])
        model = view_model(final, int(final['sampled_ns']))
        model['settings']['enabled'] = False
        self.assertTrue(final['settings']['enabled'])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
