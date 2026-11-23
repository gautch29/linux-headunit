# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import json
import os
from pathlib import Path
import sys
import stat
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from carplay_readiness import CarplayReadiness, MAXIMUM_AGE_NS
from phone_state import atomic_json


class Tests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name).resolve()
        self.now = 10_000_000_000
        self.owner = (os.getuid(), os.getgid())
        self.nonce = 'a' * 32
        self.reader = CarplayReadiness(self.root, self.owner, lambda: self.now)
        self.reader.reset(self.nonce)

    def publish(self, **changes):
        value = {'version': 1, 'nonce': self.nonce, 'sequence': '1',
                 'connectionGeneration': '1', 'updatedMonotonicNs': str(self.now),
                 'state': 'ready', 'returnReady': True}
        value.update(changes)
        atomic_json(self.reader.path, value, self.owner)
        return value

    def unavailable(self):
        self.assertFalse(self.reader.snapshot(True)['returnReady'])

    def test_child_or_bluetooth_alone_does_not_enable_return(self):
        self.unavailable()
        self.publish()
        self.assertEqual(self.reader.snapshot(False), {'state': 'disconnected', 'returnReady': False})
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.reader.reset()
        self.unavailable()

    def test_negotiated_mode_is_current_receiver_evidence_and_never_a_saved_hint(self):
        self.publish()
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.assertIsNone(self.reader.negotiated_mode)  # Legacy publication has no mode evidence.
        self.publish(version=2, negotiatedMode='ultra', sequence='2')
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.assertEqual(self.reader.negotiated_mode, 'ultra')
        self.reader.snapshot(False)
        self.assertIsNone(self.reader.negotiated_mode)
        self.reader.snapshot(True)
        self.assertEqual(self.reader.negotiated_mode, 'ultra')
        self.now += MAXIMUM_AGE_NS + 1
        self.unavailable()
        self.assertIsNone(self.reader.negotiated_mode)
        self.publish(version=2, negotiatedMode='standard', sequence='3', connectionGeneration='2')
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.assertEqual(self.reader.negotiated_mode, 'standard')
        for changes in ({'negotiatedMode': 'auto'}, {'negotiatedMode': []},
                        {'state': 'disconnected', 'returnReady': False}, {'connectionGeneration': '0'}):
            self.publish(**({'version': 2, 'negotiatedMode': 'standard', 'sequence': '4'} | changes))
            self.unavailable()
            self.assertIsNone(self.reader.negotiated_mode)
        self.publish(version=2, negotiatedMode=None, sequence='4', state='starting', returnReady=False, connectionGeneration='2')
        self.assertEqual(self.reader.snapshot(True)['state'], 'starting')
        self.assertIsNone(self.reader.negotiated_mode)

    def test_dated_ready_home_fault_and_actual_expiry(self):
        self.publish()
        self.assertEqual(self.reader.snapshot(True)['state'], 'ready')
        self.publish(sequence='2', state='home')
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.now += MAXIMUM_AGE_NS
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.now += 1
        self.unavailable()  # Reading does not refresh the producer's date.
        self.publish(sequence='3', state='fault', returnReady=False)
        self.assertEqual(self.reader.snapshot(True), {'state': 'fault', 'returnReady': False})

    def test_launch_nonce_and_connection_generation_cannot_replay(self):
        self.publish(sequence='8', connectionGeneration='3')
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.publish(sequence='7', connectionGeneration='3')
        self.unavailable()
        self.publish(sequence='9', connectionGeneration='2')
        self.unavailable()
        self.publish(sequence='9', connectionGeneration='3', nonce='b'*32)
        self.unavailable()
        self.reader.reset('b'*32)
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.publish(sequence='10', connectionGeneration='3')
        self.unavailable()  # Old launch cannot replace the newly owned launch.

    def test_same_publication_cannot_change_and_time_cannot_go_backwards(self):
        value = self.publish()
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.publish(state='home')
        self.unavailable()
        self.publish(sequence='2', updatedMonotonicNs=str(self.now-1))
        self.unavailable()
        atomic_json(self.reader.path, value, self.owner)
        self.assertTrue(self.reader.snapshot(True)['returnReady'])

    def test_incoherent_schema_future_and_numeric_aliases_reject(self):
        for changes in ({'version': True}, {'returnReady': 1}, {'state': 'starting'},
                        {'connectionGeneration': '0'}, {'sequence': '01'},
                        {'sequence': 1}, {'updatedMonotonicNs': str(self.now+1)},
                        {'sequence': str(1<<64)}, {'state': []}, {'extra': 'ignored'}):
            self.publish(**changes)
            self.unavailable()
        value = self.publish()
        del value['nonce']
        atomic_json(self.reader.path, value, self.owner)
        self.unavailable()

    def test_private_regular_single_link_bounded_endpoint(self):
        self.publish()
        os.chmod(self.reader.path, 0o644)
        self.unavailable()
        os.chmod(self.reader.path, 0o600)
        os.link(self.reader.path, self.root/'alias')
        self.unavailable()
        (self.root/'alias').unlink()
        self.reader.path.unlink()
        self.reader.path.symlink_to(self.root/'target')
        self.unavailable()
        self.reader.path.unlink()
        self.publish(extra='x'*512)
        self.unavailable()
        self.publish()
        os.chmod(self.root, 0o755)
        self.unavailable()
        os.chmod(self.root, 0o700)

    def test_duplicate_json_and_noncanonical_parent_reject(self):
        value = self.publish()
        self.reader.path.write_text(json.dumps(value)[:-1] + ',"returnReady":true}')
        self.unavailable()
        self.publish()
        alias = self.root/'dir-alias'
        alias.symlink_to(self.root, target_is_directory=True)
        reader = CarplayReadiness(alias, self.owner, lambda: self.now)
        reader.reset(self.nonce)
        self.assertFalse(reader.snapshot(True)['returnReady'])

    def test_atomic_publication_replacement_reads_the_new_fresh_snapshot(self):
        self.publish(version=2, negotiatedMode='ultra')
        self.assertTrue(self.reader.snapshot(True)['returnReady'])
        original_read = os.read
        reads = 0

        def replace_after_read(fd, maximum):
            nonlocal reads
            value = original_read(fd, maximum)
            reads += 1
            if reads == 1:
                self.publish(version=2, negotiatedMode='ultra', sequence='2')
            return value

        # Real rename unlinks the open old inode; its metadata check rejects.
        # A fresh, fully checked second read must use the new publication.
        with mock.patch('private_state.os.read', side_effect=replace_after_read):
            self.assertEqual(self.reader.snapshot(True), {'state': 'ready', 'returnReady': True})
        self.assertEqual(reads, 2)
        self.assertEqual(self.reader.last['sequence'], '2')
        self.assertEqual(self.reader.negotiated_mode, 'ultra')

    def test_publication_replacement_never_reuses_cached_readiness(self):
        original_read = os.read
        for changes in ({'state': 'fault', 'returnReady': False},
                        {'nonce': 'b' * 32},
                        {'updatedMonotonicNs': str(self.now - MAXIMUM_AGE_NS - 1)},
                        {'updatedMonotonicNs': str(self.now + 1)},
                        {'returnReady': 1}):
            self.reader.reset(self.nonce)
            self.publish(version=2, negotiatedMode='ultra')
            self.assertTrue(self.reader.snapshot(True)['returnReady'])
            reads = 0

            def replace_after_read(fd, maximum):
                nonlocal reads
                value = original_read(fd, maximum)
                reads += 1
                if reads == 1:
                    self.publish(version=2, negotiatedMode='ultra', sequence='2', **changes)
                return value

            with mock.patch('private_state.os.read', side_effect=replace_after_read):
                self.assertFalse(self.reader.snapshot(True)['returnReady'])
            self.assertEqual(reads, 2)
            if changes.get('state') == 'fault':
                self.assertEqual(self.reader.last['state'], 'fault')
            else:
                self.assertEqual(self.reader.last['sequence'], '1')

    def test_changing_publications_have_one_bounded_retry(self):
        self.publish()
        original_read = os.read
        reads = 0

        def replace_after_every_read(fd, maximum):
            nonlocal reads
            value = original_read(fd, maximum)
            reads += 1
            self.publish(sequence=str(reads + 1))
            return value

        with mock.patch('private_state.os.read', side_effect=replace_after_every_read):
            self.unavailable()
        self.assertEqual(reads, 2)
        self.assertIsNone(self.reader.last)

    def test_atomic_replacement_before_descriptor_stat_also_retries(self):
        self.publish()
        original_stat = os.fstat
        replaced = False

        def replace_before_stat(fd):
            nonlocal replaced
            value = original_stat(fd)
            if stat.S_ISREG(value.st_mode) and not replaced:
                replaced = True
                self.publish(sequence='2')
                return original_stat(fd)
            return value

        with mock.patch('private_state.os.fstat', side_effect=replace_before_stat):
            self.assertTrue(self.reader.snapshot(True)['returnReady'])
        self.assertTrue(replaced)
        self.assertEqual(self.reader.last['sequence'], '2')

    def test_retry_rechecks_private_permissions(self):
        self.publish()
        original_read = os.read
        reads = 0

        def change_mode_after_read(fd, maximum):
            nonlocal reads
            value = original_read(fd, maximum)
            reads += 1
            os.chmod(self.reader.path, 0o644)
            return value

        with mock.patch('private_state.os.read', side_effect=change_mode_after_read):
            self.unavailable()
        self.assertEqual(reads, 1)
        self.assertIsNone(self.reader.last)


if __name__ == '__main__':
    unittest.main()
