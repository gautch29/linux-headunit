#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Private manager intent; clocks are explicit software fixtures, not phone ACKs."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from cluster_layout import ContentControl

NONCE = 'a' * 32
PUBLISHER = NONCE, '7'

class ContentTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='headunit-content-', dir='/tmp')
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.endpoint = self.root / 'cluster-content.request'
        self.profile = self.root / 'settings.json'
        self.document = {'version': 1, 'experimental': True, 'sections': [], 'settings': [],
                         'cluster_content': {'request_file': str(self.endpoint), 'pages': [
                             {'id': 'body', 'widget': 'body', 'panel': 'Cluster_Display', 'zone': 'zone1'},
                             {'id': 'tyres', 'widget': 'tyres', 'panel': 'Cluster_Display', 'zone': 'zone1'}]}}
        self.write()

    def write(self):
        self.profile.write_text(json.dumps(self.document))
        self.profile.chmod(0o600)

    def test_lease_renewal_and_disconnect_do_not_confirm_a_page(self):
        control = ContentControl(self.profile, self.root)
        self.assertTrue(control.snapshot()['configured'])
        with patch('cluster_layout.clock_ns', return_value=1_000_000_000):
            control.request('body', PUBLISHER)
        self.assertEqual(self.endpoint.read_text(), f'HUC1 {NONCE} 7 1 1000 3000\nbody\n')
        self.assertEqual(self.endpoint.stat().st_mode & 0o777, 0o600)
        self.assertEqual(control.snapshot()['requested'], 'body')
        self.assertFalse(control.snapshot()['confirmed'])
        with patch('cluster_layout.clock_ns', return_value=1_500_000_000):
            control.maintain(PUBLISHER)
        self.assertEqual(self.endpoint.read_text(), f'HUC1 {NONCE} 7 1 1500 3500\nbody\n')
        with patch('cluster_layout.clock_ns', return_value=1_600_000_000):
            control.maintain((NONCE, '8'))
        self.assertEqual(self.endpoint.read_text(), f'HUC1 {NONCE} 7 2 1600 3600\n-\n')
        self.assertIsNone(control.snapshot()['requested'])
        prior = self.endpoint.read_bytes()
        with patch('cluster_layout.clock_ns', return_value=1_700_000_000):
            control.maintain((NONCE, '8'))
        self.assertEqual(self.endpoint.read_bytes(), prior)

    def test_only_declared_pages_and_current_publishers_are_admitted(self):
        control = ContentControl(self.profile, self.root)
        for page in ('media', [], True, 'body\nextra'):
            with self.assertRaises(ValueError):
                control.request(page, PUBLISHER)
        for publisher in (('bad', '7'), (NONCE, '01'), (NONCE, 7), (NONCE, '0')):
            with self.assertRaises(ValueError):
                control.request('body', publisher)
        self.assertFalse(self.endpoint.exists())
        control.request(None, PUBLISHER)
        self.assertTrue(self.endpoint.read_text().endswith('\n-\n'))

    def test_configuration_is_bounded_and_cannot_alias_other_state(self):
        changes = [lambda d: d.update(experimental=False),
                   lambda d: d['cluster_content'].update(request_file=str(self.root / 'manual.request')),
                   lambda d: d['cluster_content']['pages'][0].update(widget='media'),
                   lambda d: d['cluster_content']['pages'][1].update(zone='zone2'),
                   lambda d: d['cluster_content']['pages'][0].update(dismissible=False)]
        baseline = json.loads(json.dumps(self.document))
        for change in changes:
            self.document = json.loads(json.dumps(baseline))
            change(self.document)
            self.write()
            with self.assertRaises(ValueError):
                ContentControl(self.profile, self.root)
        self.assertFalse(self.endpoint.exists())
        self.profile.write_bytes(b' ' * 65537)
        with self.assertRaises(ValueError):
            ContentControl(self.profile, self.root)

    def test_profile_replacement_is_rejected(self):
        real_read = os.read
        def replace(fd, size):
            raw = real_read(fd, size)
            replacement = self.root / 'new.json'
            replacement.write_bytes(raw)
            replacement.chmod(0o600)
            replacement.replace(self.profile)
            return raw
        with patch('cluster_layout.os.read', side_effect=replace):
            with self.assertRaises(ValueError):
                ContentControl(self.profile, self.root)
        self.assertFalse(self.endpoint.exists())

    def test_unsafe_parent_and_sequence_exhaustion_fail_closed(self):
        control = ContentControl(self.profile, self.root)
        self.root.chmod(0o755)
        with self.assertRaises(ValueError):
            control.request('body', PUBLISHER)
        self.root.chmod(0o700)
        control.sequence = 2**64 - 1
        with self.assertRaises(ValueError):
            control.request('body', PUBLISHER)
        self.assertFalse(self.endpoint.exists())

if __name__ == '__main__':
    unittest.main()
