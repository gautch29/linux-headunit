# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import json
import os
import subprocess
from unittest import mock
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from cluster_page import adopt, read, select
from media_bridge import snapshot


class ClusterPageTests(unittest.TestCase):
    def test_map_is_offered_only_during_standard_carplay(self):
        with tempfile.TemporaryDirectory() as folder:
            state = Path(folder) / 'page'
            self.assertEqual(read(state, False), 'media')
            self.assertEqual(read(state, True), 'map')
            self.assertEqual(select(state, 'vehicle', False), 'vehicle')
            self.assertEqual(select(state, 'next', False), 'trip')
            self.assertEqual(select(state, 'next', False), 'minimal')
            self.assertEqual(select(state, 'next', False), 'sport')
            self.assertEqual(select(state, 'next', False), 'gauges')
            self.assertEqual(select(state, 'next', False), 'media')
            self.assertEqual(select(state, 'previous', False), 'gauges')
            self.assertEqual(read(state, True), 'gauges')
            self.assertEqual(select(state, 'map', True), 'map')
            self.assertEqual(read(state, False), 'media')
            with self.assertRaises(ValueError):
                select(state, 'map', False)

    def test_writes_and_adopts_for_the_display_account(self):
        # The renderers read as the display account: a page file left to its
        # privileged writer would silently pin every cluster to the default.
        owner = (os.getuid(), os.getgid())
        with tempfile.TemporaryDirectory() as folder:
            state = Path(folder) / 'page'
            calls = []
            with mock.patch('os.fchown', side_effect=lambda fd, uid, gid: calls.append((uid, gid))):
                self.assertEqual(select(state, 'trip', False, owner), 'trip')
            self.assertEqual(calls, [owner])
            self.assertEqual(oct(state.stat().st_mode & 0o777), '0o600')
            with mock.patch('os.chown') as chown:
                adopt(state, owner)
                chown.assert_not_called()
                adopt(state, (owner[0] + 1, owner[1]))
                chown.assert_called_once_with(state, owner[0] + 1, owner[1], follow_symlinks=False)
                link = Path(folder) / 'link'; link.symlink_to(state); chown.reset_mock()
                adopt(link, (owner[0] + 1, owner[1])); adopt(Path(folder) / 'absent', owner)
                chown.assert_not_called()

    def test_standalone_media_ignores_stale_or_untrusted_capture(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'media.iap'
            source.write_text(json.dumps({'available': True, 'title': 'Track',
                                          'artist': 'Artist', 'controls': []}))
            value, digest = snapshot(source)
            self.assertEqual((value['title'], value['source'], digest), ('Track', 'iap', ''))
            source.write_text(json.dumps({'available': True, 'iap_artwork_sha256': 'bad'}))
            with self.assertRaises(ValueError):
                snapshot(source)

    def test_native_page_and_standalone_media_reject_fifo_without_waiting(self):
        script = ('import sys\n'
                  'sys.path.insert(0, sys.argv[1])\n'
                  'from pathlib import Path\n'
                  'from cluster_page import read\n'
                  'from media_bridge import read_private\n'
                  'path=Path(sys.argv[2])\n'
                  'assert read(path, True)=="map"\n'
                  'try: read_private(path, 8192)\n'
                  'except ValueError: pass\n'
                  'else: raise AssertionError("FIFO media was admitted")\n')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fifo'
            os.mkfifo(path, 0o600)
            completed = subprocess.run([sys.executable, '-B', '-c', script,
                                        str(Path(__file__).resolve().parents[1] / 'tools'),
                                        str(path)], timeout=3, capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode())

    def test_native_page_rejects_linked_or_oversized_state(self):
        with tempfile.TemporaryDirectory() as folder:
            state = Path(folder) / 'page'
            select(state, 'trip', False)
            link = state.with_name('link')
            link.symlink_to(state)
            self.assertEqual(read(link, True), 'map')
            self.assertEqual(read(Path(folder), True), 'map')
            state.write_text('trip' + ' ' * 20)
            self.assertEqual(read(state, True), 'map')
            select(state, 'trip', False)
            hardlink = state.with_name('hardlink')
            os.link(state, hardlink)
            self.assertEqual(read(state, True), 'map')

    def test_standalone_media_rejects_nonobject_and_deeply_nested_metadata(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'media.iap'
            for content in ('null', 'true', '1', '[[]]',
                            '[' * 2000 + '0' + ']' * 2000):
                source.write_text(content)
                with self.assertRaises(ValueError):
                    snapshot(source)
            for digest in (False, 0, [], {}, None):
                source.write_text(json.dumps({'available': True, 'iap_artwork_sha256': digest}))
                with self.assertRaises(ValueError):
                    snapshot(source)


if __name__ == '__main__':
    unittest.main()
