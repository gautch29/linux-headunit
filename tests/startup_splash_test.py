# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Portable once-per-boot/failure admission; no compositor, phone or radio."""
import os
import contextlib
import io
import ast
import math
import xml.etree.ElementTree as ET
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_page_presentation import BootSplash, claim_boot_splash


class DecorationAdmission(unittest.TestCase):
    def setUp(self):
        self.callbacks = {}
        self.notified = []
        def add(*arguments):
            ident = len(self.callbacks) + 1
            self.callbacks[ident] = arguments[-1]
            return ident
        self.splash = BootSplash.__new__(BootSplash)
        self.splash.GLib = mock.Mock()
        self.splash.GLib.timeout_add.side_effect = add
        self.splash.GLib.idle_add.side_effect = add
        self.splash.started_at = None
        self.splash.closed = False
        self.splash.deadline = self.splash.completion = self.splash.failure = self.splash.fade = self.splash.ready = 0
        self.splash.pending_outputs = {'main', 'cluster'}
        self.splash.on_ready = lambda: self.notified.append('decoration-admitted')
        self.splash.connections = []
        self.splash.canvases = []
        self.splash.windows = []
        self.splash.page = mock.Mock()

    def test_waits_for_both_outputs_and_notifies_once(self):
        self.splash.drawn('main')
        self.assertEqual(self.splash.ready, 0)
        self.splash.drawn('cluster')
        self.assertNotEqual(self.splash.ready, 0)
        self.callbacks[self.splash.ready]()
        self.splash.drawn('main')
        self.assertEqual(self.notified, ['decoration-admitted'])
        self.assertEqual(self.splash.ready, 0)

    def test_first_scene_dates_are_bounded_and_do_not_release_the_other_output(self):
        out = io.StringIO()
        with mock.patch('time.clock_gettime_ns', side_effect=[123, 456]), contextlib.redirect_stderr(out):
            self.splash.drawn('main')
            self.splash.drawn('main')
            self.assertEqual(self.splash.ready, 0)
            self.splash.drawn('cluster')
        self.assertEqual(out.getvalue().splitlines(), [
            'Headunit splash first scene constructed output=main mono_ns=123',
            'Headunit splash first scene constructed output=cluster mono_ns=456'])
        self.assertEqual(self.notified, [])
        self.callbacks[self.splash.ready]()
        self.assertEqual(self.notified, ['decoration-admitted'])

    def test_unavailable_diagnostic_clock_does_not_invent_a_date_or_block_admission(self):
        out = io.StringIO()
        with mock.patch('time.clock_gettime_ns', side_effect=OSError('clock unavailable')), \
                contextlib.redirect_stderr(out):
            self.splash.drawn('main')
            self.splash.drawn('cluster')
        self.assertEqual(out.getvalue(), '')
        self.callbacks[self.splash.ready]()
        self.assertEqual(self.notified, ['decoration-admitted'])

    def test_failed_decoration_releases_fallback_after_destroying_surfaces(self):
        window = mock.Mock()
        window.destroy.side_effect = lambda: self.notified.append('surface-destroyed')
        self.splash.windows.append(window)
        self.splash.drawn('main')
        self.splash.close('render-error')
        self.callbacks[self.splash.ready]()
        self.assertEqual(self.notified, ['surface-destroyed', 'decoration-admitted'])
        self.assertEqual((self.splash.completion, self.splash.fade), (0, 0))


class Admission(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name).resolve()
        self.directory.chmod(0o700)

    def tearDown(self):
        self.temp.cleanup()

    def test_restart_and_concurrent_claim_do_not_replay(self):
        self.assertTrue(claim_boot_splash(self.directory, elapsed=3))
        self.assertFalse(claim_boot_splash(self.directory, elapsed=4))
        self.assertEqual((self.directory / 'headunit-boot-attempt').stat().st_mode & 0o777, 0o600)

    def test_late_start_and_unusable_clock_skip_without_marker(self):
        for elapsed in (61, -1, float('nan'), float('inf'), '3', True):
            self.assertFalse(claim_boot_splash(self.directory, elapsed=elapsed))
        with mock.patch('time.clock_gettime', side_effect=OSError('Unavailable')):
            self.assertFalse(claim_boot_splash(self.directory))
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_replaceable_directory_and_redirects_skip(self):
        self.directory.chmod(0o755)
        self.assertFalse(claim_boot_splash(self.directory, elapsed=3))
        self.directory.chmod(0o700)
        link = self.directory / 'redirect'
        link.symlink_to(self.directory, target_is_directory=True)
        self.assertFalse(claim_boot_splash(link, elapsed=3))
        self.assertFalse(claim_boot_splash('.', elapsed=3))
        self.assertFalse(claim_boot_splash(self.directory / 'missing', elapsed=3))

    def test_existing_marker_is_never_followed_or_overwritten(self):
        other = self.directory / 'other'
        other.write_text('untouched')
        (self.directory / 'headunit-boot-attempt').symlink_to(other)
        self.assertFalse(claim_boot_splash(self.directory, elapsed=3))
        self.assertEqual(other.read_text(), 'untouched')

    def test_sealed_geometry_matches_original_svg_and_trace_is_bounded(self):
        module = Path(__file__).resolve().parents[1] / 'tools/phone_icons.py'
        self.assertLessEqual(module.stat().st_size, 131072)
        scope = {'math': math}
        tree = ast.parse(module.read_text())
        for node in tree.body:
            if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name) and node.targets[0].id == '_STARTUP_POLYGONS':
                scope[node.targets[0].id] = ast.literal_eval(node.value)
            if isinstance(node, ast.FunctionDef) and node.name in ('startup_trace', 'startup_ease'):
                exec(compile(ast.Module(body=[node], type_ignores=[]), str(module), 'exec'), scope)
        root = ET.parse(module.parents[1] / 'assets/branding/headunit/logo.svg')
        original = tuple((p.attrib['id'], tuple(tuple(map(float, v.split(','))) for v in p.attrib['points'].split()))
                         for p in root.findall('.//{http://www.w3.org/2000/svg}polygon'))
        self.assertEqual(scope['_STARTUP_POLYGONS'], original)
        for _, points in original:
            self.assertEqual(scope['startup_trace'](points, 0), (points[0],))
            complete = scope['startup_trace'](points, 1)
            self.assertAlmostEqual(math.dist(complete[-1], points[0]), 0)
            previous = 0
            for step in range(101):
                trace = scope['startup_trace'](points, step / 100)
                length = sum(math.dist(a, b) for a, b in zip(trace, trace[1:]))
                self.assertGreaterEqual(length + 1e-9, previous)
                self.assertLessEqual(len(trace), len(points) + 1)
                previous = length
        for t in (-1, 0, .1, .2, 100):
            self.assertTrue(0 <= scope['startup_ease'](t, 0, .2) <= 1)


if __name__ == '__main__':
    unittest.main()
