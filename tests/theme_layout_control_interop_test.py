# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Feed actual C++ owner status to the manager/native selector's existing parser."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / 'build/headunit-theme-layout_control-tests'
sys.path.insert(0, str(ROOT / 'tools'))
from cluster_layout import snapshot, select

A = '00000000-1111-2222-3333-444444444444'
B = '00000000-1111-2222-3333-555555555555'
C = '00000000-1111-2222-3333-666666666666'


class Test(unittest.TestCase):
    def test_changed_catalogue_status_stays_usable_without_claiming_confirmation(self):
        result = subprocess.run([str(BINARY), '--selector-evidence'], check=True,
                                capture_output=True, text=True, timeout=15)
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        self.assertEqual(len(rows), 4)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'layout'
            status = path.with_name(path.name + '.status')
            for index, row in enumerate(rows):
                with self.subTest(phase=index):
                    status.write_text(json.dumps(row)); status.chmod(0o600)
                    self.assertEqual(snapshot(path), row)
                    if row['idle']:
                        select(path, C, (os.getuid(), os.getgid()))
                        self.assertEqual(path.read_text(), f"HUL1 {row['nonce']} {row['connectionGeneration']}\n{C}\n")
                    else:
                        with self.assertRaises(ValueError):
                            select(path, C)
            self.assertEqual(rows[0]['pending'], '')
            self.assertIn('not declared', rows[0]['error'])
            self.assertEqual(rows[1]['pending'], C)
            self.assertEqual(rows[2]['confirmedActive'], '')
            self.assertTrue(all(row['confirmed'] == B for row in rows[:3]))
            self.assertEqual(rows[3]['confirmed'], C)
            self.assertEqual(rows[3]['confirmedActive'], C)
            self.assertEqual(rows[3]['pending'], '')


if __name__ == '__main__':
    unittest.main()
