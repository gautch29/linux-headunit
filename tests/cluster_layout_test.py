# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from filesystem_fixture import change_metadata
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import cluster_layout
from cluster_layout import configured_path, snapshot, select
from carplay_readiness import clock_ns

A = '00000000-1111-2222-3333-444444444444'
B = '00000000-1111-2222-3333-555555555555'
NONCE = '0123456789abcdef0123456789abcdef'
PUBLISHER = NONCE, '1'


class Test(unittest.TestCase):
    def test_connection_bound_navigation_is_private_dated_and_not_confirmation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'layout'
            state = {'version': 2, 'nonce': NONCE, 'connectionGeneration': '1', 'sequence': '1',
                     'updatedMonotonicNs': str(clock_ns()), 'active': A, 'pending': '',
                     'ready': True, 'transitioning': False, 'error': '',
                     'choices': [{'id': A, 'name': 'A'}]}
            status = Path(str(path) + '.status')
            status.write_text(json.dumps(state)); status.chmod(0o600)
            with patch('cluster_layout.clock_ns', return_value=clock_ns()) as clock:
                cluster_layout.navigate(path, 'next', publisher=PUBLISHER)
                request = Path(str(path) + '.input')
                self.assertEqual(request.read_text(), f'HUI1\n{NONCE}\n1\n{clock.return_value}\n{clock.return_value}\nnext\n')
            self.assertEqual(request.stat().st_mode & 0o777, 0o600)
            self.assertEqual(snapshot(path)['active'], A)
            for command in ('play_pause', 'next_track', 'previous_track'):
                with patch('cluster_layout.clock_ns', return_value=clock_ns()):
                    cluster_layout.navigate(path, command, publisher=PUBLISHER)
                self.assertTrue(request.read_text().endswith(f'\n{command}\n'))
            for command in ('music', [], None):
                with self.assertRaises(ValueError): cluster_layout.navigate(path, command, publisher=PUBLISHER)
            with self.assertRaises(ValueError):
                cluster_layout.navigate(path, 'next', publisher=('a' * 32, '1'))
            status.write_text(json.dumps(dict(state, ready=False)))
            with self.assertRaises(ValueError): cluster_layout.navigate(path, 'select', publisher=PUBLISHER)
            status.write_text(json.dumps(state))
            path.parent.chmod(0o755)
            try:
                with self.assertRaises(ValueError): cluster_layout.navigate(path, 'back', publisher=PUBLISHER)
            finally:
                path.parent.chmod(0o700)

    def test_profile_input_policy_bounds_and_partial_reads(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            profile = root / 'ui.conf'
            target = root / 'layout'
            self.assertIsNone(configured_path(None))
            self.assertIsNone(configured_path(profile))
            self.assertIsNone(configured_path(root))
            for raw in (b'', b'other=value\n', b'\xff', b'x' * 8193):
                profile.write_bytes(raw)
                self.assertIsNone(configured_path(profile))
            line = f'clusterLayoutFile={target}\n'.encode()
            profile.write_bytes(line + b'#' * (8192 - len(line)))
            self.assertEqual(configured_path(profile), target)
            profile.write_bytes(line)
            link = root / 'linked.conf'
            link.symlink_to(profile)
            self.assertEqual(configured_path(link), target)
            # This optional source reader adds no owner/mode/hard-link policy.
            profile.chmod(0o666)
            hard = root / 'hard.conf'
            os.link(profile, hard)
            self.assertEqual(configured_path(hard), target)
            original_read = os.read
            with patch('cluster_layout.os.read', side_effect=lambda fd, size: original_read(fd, min(size, 7))):
                self.assertEqual(configured_path(profile), target)
            profile.write_text('clusterLayoutFile=relative\n')
            with self.assertRaisesRegex(ValueError, 'must be absolute'):
                configured_path(profile)

    def test_fifo_profile_and_regular_to_fifo_race_are_bounded_real_processes(self):
        program = """
import os
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
import cluster_layout
path = Path(sys.argv[2])
if sys.argv[3] == 'replace':
    actual_open = os.open
    def replace_before_open(name, flags, **options):
        if name == path.name and 'dir_fd' in options:
            path.unlink()
            os.mkfifo(path)
        return actual_open(name, flags, **options)
    cluster_layout.os.open = replace_before_open
print(cluster_layout.configured_path(path))
"""
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            profile = root / 'ui.conf'
            tools = str(Path(cluster_layout.__file__).parent)
            for route in ('fifo', 'linked_fifo', 'replace', 'relative'):
                with self.subTest(route=route):
                    profile.unlink(missing_ok=True)
                    if route in ('fifo', 'linked_fifo'):
                        os.mkfifo(profile)
                    else:
                        profile.write_text(f'clusterLayoutFile={root / "layout"}\n')
                    source = profile
                    if route == 'linked_fifo':
                        source = root / 'linked.conf'
                        source.symlink_to(profile)
                    elif route == 'relative':
                        source = Path('ui.conf')
                    result = subprocess.run([sys.executable, '-c', program, tools, str(source), route],
                                            cwd=root, capture_output=True, text=True, timeout=3)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = str(root / 'layout') if route == 'relative' else 'None'
                    self.assertEqual(result.stdout.strip(), expected)

    def test_profile_mutation_or_replacement_discards_captured_control(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            profile = root / 'ui.conf'
            raw = f'clusterLayoutFile={root / "layout"}\n'.encode()
            actual_read = os.read
            for change in ('rewrite', 'grow', 'truncate', 'replace', 'retarget', 'parent'):
                with self.subTest(change=change):
                    profile.write_bytes(raw)
                    source = profile
                    if change == 'retarget':
                        source = root / 'linked.conf'
                        source.symlink_to(profile)
                    changed = False
                    moved_parent = root.with_name(root.name + '-moved')
                    def mutate_after_read(fd, size):
                        nonlocal changed
                        chunk = actual_read(fd, size)
                        if chunk and not changed:
                            changed = True
                            if change == 'rewrite':
                                change_metadata(profile, lambda: profile.write_bytes(raw.replace(b'layout', b'otherx')))
                            elif change == 'grow':
                                with profile.open('ab') as output:
                                    output.write(b'additional=value\n')
                            elif change == 'truncate':
                                profile.write_bytes(b'')
                            elif change == 'replace':
                                replacement = root / 'new.conf'
                                replacement.write_bytes(raw)
                                replacement.replace(profile)
                            elif change == 'retarget':
                                replacement = root / 'new.conf'
                                replacement.write_bytes(raw)
                                source.unlink()
                                source.symlink_to(replacement)
                            else:
                                root.rename(moved_parent)
                                root.mkdir()
                        return chunk
                    try:
                        with patch('cluster_layout.os.read', side_effect=mutate_after_read):
                            self.assertIsNone(configured_path(source))
                    finally:
                        if source != profile:
                            source.unlink(missing_ok=True)
                        if moved_parent.exists():
                            root.rmdir()
                            moved_parent.rename(root)

    def test_nested_invalid_catalogue_is_unavailable(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'layout'
            status = Path(str(path) + '.status')
            status.write_text('[' * 2000 + '0' + ']' * 2000)
            status.chmod(0o600)
            self.assertIsNone(snapshot(path))
            with self.assertRaises(ValueError):
                select(path, A)
            self.assertFalse(path.exists())

    def test_live_choices_and_confirmation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'layout'
            profile = Path(folder) / 'ui.conf'
            profile.write_text(f'clusterLayoutFile={path}\n')
            self.assertEqual(configured_path(profile), path)
            self.assertIsNone(snapshot(path))
            with self.assertRaises(ValueError): select(path, B)
            state = {'version': 2, 'nonce': NONCE, 'connectionGeneration': '1', 'sequence': '1',
                     'updatedMonotonicNs': str(clock_ns()),
                     'active': A, 'pending': '', 'ready': True, 'transitioning': False,
                     'error': '', 'choices': [{'id': A, 'name': 'A'}, {'id': B, 'name': 'B'}]}
            status = path.with_name(path.name + '.status')
            status.write_text(json.dumps(state)); status.chmod(0o600)
            select(path, B, (os.getuid(), os.getgid()))
            self.assertEqual(path.read_text(), f'HUL1 {NONCE} 1\n{B}\n')
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(snapshot(path)['active'], A)  # Click never claims phone confirmation.
            with self.assertRaises(ValueError): select(path, 'arbitrary asset')
            with self.assertRaises(ValueError): select(path, [B])
            status.write_text(json.dumps(dict(state, updatedMonotonicNs=str(clock_ns() - 4_000_000_000))))
            os.utime(status, None)  # Touching stale bytes cannot renew their authority.
            self.assertIsNone(snapshot(path))

            with self.assertRaises(ValueError): select(path, A)
            status.write_text(json.dumps(dict(state, confirmed=B, restoring=B, preferenceError='')))
            status.chmod(0o600)
            self.assertEqual(snapshot(path)['confirmed'], B)
            for key, invalid in [('confirmed', 'invalid'), ('restoring', 3), ('confirmedActive', 'invalid'), ('idle', 'yes'), ('preferenceError', 'x' * 257)]:
                status.write_text(json.dumps(dict(state, **{key: invalid})))
                self.assertIsNone(snapshot(path))
            status.write_text(json.dumps(dict(state, idle=False, confirmedActive='', confirmed=B)))
            self.assertEqual(snapshot(path)['confirmedActive'], '')
            with self.assertRaises(ValueError): select(path, B)
            status.write_text(json.dumps(state)); status.chmod(0o622)
            self.assertIsNone(snapshot(path))
            status.unlink(); status.symlink_to(profile)
            self.assertIsNone(snapshot(path))

    def test_palette_and_content_are_catalogued_and_connection_bound(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'layout'
            status = Path(str(path) + '.status')
            state = {'version': 2, 'nonce': NONCE, 'connectionGeneration': '1', 'sequence': '2',
                     'updatedMonotonicNs': str(clock_ns()), 'active': A, 'pending': '',
                     'ready': True, 'idle': True, 'transitioning': False, 'error': '',
                     'choices': [{'id': A, 'name': 'A'}, {'id': B, 'name': 'B'}],
                     'palettes': [{'id': 'blue', 'name': 'Blue'}], 'activePalette': 'blue',
                     'contents': [{'id': B, 'zone': A, 'name': 'Map', 'active': False}]}
            status.write_text(json.dumps(state)); status.chmod(0o600)
            select(path, A, publisher=PUBLISHER, palette='blue', zone=A, component=B)
            self.assertEqual(path.read_text(), f'HUL2 {NONCE} 1\n{A}\nblue\n{A}\n{B}\n')
            self.assertFalse(snapshot(path)['contents'][0]['active'])
            previous = path.read_bytes()
            state['contents'][0]['available'] = False
            status.write_text(json.dumps(state))
            self.assertIsNotNone(snapshot(path))
            with self.assertRaisesRegex(ValueError, 'unavailable'):
                select(path, A, publisher=PUBLISHER, zone=A, component=B)
            self.assertEqual(path.read_bytes(), previous)
            state['contents'][0]['available'] = True
            status.write_text(json.dumps(state))
            select(path, A, publisher=PUBLISHER, zone=A, component=B)
            state['contents'][0]['available'] = 1
            status.write_text(json.dumps(state))
            self.assertIsNone(snapshot(path))
            del state['contents'][0]['available']
            status.write_text(json.dumps(state))
            for options in ({'palette': 'unknown'}, {'palette': []}, {'zone': A},
                            {'zone': B, 'component': B}, {'zone': [], 'component': B}):
                with self.assertRaises(ValueError): select(path, A, publisher=PUBLISHER, **options)
            with self.assertRaises(ValueError): select(path, B, zone=A, component=B)
            for changes in ({'activePalette': 'unknown'}, {'contents': state['contents'] * 2},
                            {'palettes': [{'id': '../file', 'name': 'Invalid'}]}):
                status.write_text(json.dumps(dict(state, **changes)))
                self.assertIsNone(snapshot(path))

    def test_publisher_identity_and_monotonic_dates(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'layout'
            status = Path(str(path) + '.status')
            state = {'version': 2, 'nonce': NONCE, 'connectionGeneration': '1', 'sequence': '2',
                     'updatedMonotonicNs': str(clock_ns()), 'active': A, 'pending': '',
                     'ready': True, 'idle': True, 'transitioning': False,
                     'error': '', 'choices': [{'id': A, 'name': 'A'}]}
            status.write_text(json.dumps(state)); status.chmod(0o600)
            self.assertIsNotNone(snapshot(path, publisher=PUBLISHER))
            for publisher in [('a' * 32, '1'), (NONCE, '2')]:
                self.assertIsNone(snapshot(path, publisher=publisher))
                with self.assertRaises(ValueError): select(path, A, publisher=publisher)
            self.assertFalse(path.exists())
            for changes in ({'nonce': 'A' * 32}, {'version': 1}, {'sequence': '0'},
                            {'connectionGeneration': '01'}, {'updatedMonotonicNs': str(clock_ns() + 5_000_000_000)}):
                status.write_text(json.dumps(dict(state, **changes)))
                self.assertIsNone(snapshot(path, publisher=PUBLISHER))
            status.write_text(json.dumps(state)); status.chmod(0o644)
            self.assertIsNone(snapshot(path))


if __name__ == '__main__':
    unittest.main()
