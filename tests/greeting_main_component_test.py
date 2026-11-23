# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import sys
import unittest
import time
from pathlib import Path
import xml.etree.ElementTree as ET
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from greeting_sequence import GreetingSequence, IgnitionObservation, POLYGONS


class Tests(unittest.TestCase):
    def test_nonblocking_completion_and_immutable_frames(self):
        now = [10.]
        sequence = GreetingSequence(lambda: now[0])
        sequence.begin('startup', 'brand')
        now[0] += .4
        captured = sequence.frame()
        self.assertGreater(captured.opacity, .9)
        svg = captured.svg(1280, 720)
        self.assertEqual(len(ET.fromstring(svg)[0]), len(POLYGONS))
        now[0] += 2
        self.assertEqual(sequence.frame().phase, 'inactive')
        self.assertEqual(captured.svg(1280, 720), svg)
        sequence.begin('goodbye', 'minimal')
        now[0] += .2
        self.assertEqual(len(ET.fromstring(sequence.frame().svg(1920, 1080))[0]), 1)
        now[0] += .71
        self.assertEqual(sequence.frame().phase, 'inactive')

    def test_off_retirement_clock_failure_and_allocation_bounds(self):
        now = [10.]
        sequence = GreetingSequence(lambda: now[0])
        sequence.begin('startup', 'off')
        self.assertEqual(sequence.frame().svg(1280, 720), '')
        for value in (9., float('nan'), float('inf'), None):
            now[0] = 10.
            sequence.begin('startup', 'brand')
            now[0] = value
            self.assertEqual(sequence.frame().phase, 'inactive')
        now[0] = 10.
        sequence.begin('startup', 'brand')
        now[0] = 10.5
        with self.assertRaises(ValueError):
            sequence.frame().svg(50000, 720)
        sequence.retire()
        self.assertEqual(sequence.frame().phase, 'inactive')

    def test_observed_ignition_edges_preserve_acquisition_and_generation(self):
        now = [10.]
        sequence = GreetingSequence(lambda: now[0])
        def observation(on, date=None, generation='hardware-1'):
            return IgnitionObservation(on, now[0] if date is None else date, 'fixture-dated-input', generation)
        self.assertFalse(sequence.observe_ignition(observation(False), 'brand'))
        now[0] += .1
        self.assertTrue(sequence.observe_ignition(observation(True), 'brand'))
        now[0] += .1
        self.assertFalse(sequence.observe_ignition(observation(False, generation='hardware-2'), 'brand'))
        now[0] += .1
        self.assertTrue(sequence.observe_ignition(observation(True, now[0]-.05, 'hardware-2'), 'brand'))
        now[0] += .1
        self.assertTrue(sequence.observe_ignition(observation(False, now[0]-.1, 'hardware-2'), 'brand'))
        self.assertEqual(sequence.started, now[0]-.1)
        self.assertEqual(sequence.frame().phase, 'goodbye')
        self.assertFalse(sequence.observe_ignition(observation(True, now[0]-1), 'brand'))
        self.assertEqual(sequence.frame().phase, 'inactive')
        self.assertFalse(sequence.observe_ignition(observation(False), 'brand'))
        self.assertFalse(sequence.observe_ignition(observation(None), 'brand'))
        self.assertFalse(sequence.observe_ignition(observation(True), 'brand'))

    def test_default_uses_actual_clock_monotonic_acquisition_domain(self):
        domain = time.CLOCK_MONOTONIC_RAW if sys.platform == 'darwin' else time.CLOCK_MONOTONIC
        before = time.clock_gettime(domain)
        sequence = GreetingSequence()
        sequence.begin('startup', 'brand')
        after = time.clock_gettime(domain)
        self.assertTrue(before <= sequence.started <= after)

    def test_shared_original_geometry(self):
        root = Path(__file__).resolve().parents[1]
        document = ET.parse(root / 'assets/branding/headunit/logo.svg')
        original = tuple(tuple(tuple(map(float, pair.split(','))) for pair in p.attrib['points'].split())
                         for p in document.findall('.//{http://www.w3.org/2000/svg}polygon'))
        self.assertEqual(POLYGONS, original)
        with self.assertRaises(ValueError):
            GreetingSequence().begin('ignition', 'brand')


if __name__ == '__main__':
    unittest.main()
