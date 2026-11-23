#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Static validation using real bounded profile files; no phone or service work."""
import copy
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import phone_manager_configuration as configuration


class ConfigurationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='headunit-manager-profile-', dir='/tmp')
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name).resolve()
        self.path = self.folder / 'profile.json'
        self.value = {'schemaVersion': 1, 'enabled': False, 'stateDirectory': str(self.folder / 'state'),
                      'standardDisplayConfig': str(self.folder / 'standard.conf'), 'autoConnectSelected': False}
        self.write(json.dumps(self.value).encode())

    def write(self, raw, mode=0o600):
        if self.path.exists():
            self.path.chmod(0o600)
        fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        try:
            os.write(fd, raw)
            os.fchmod(fd, mode)
        finally:
            os.close(fd)

    def test_disabled_and_enabled_static_validation_has_no_control_side_effects(self):
        for enabled in (False, True):
            value = self.value | {'enabled': enabled, 'autoConnectSelected': True}
            original = copy.deepcopy(value)
            with mock.patch.object(configuration.os, 'open', side_effect=AssertionError('Validator opened an endpoint')):
                result = configuration.validate_configuration(value)
            self.assertEqual(result, original)
            self.assertEqual(value, original)
            self.assertFalse((self.folder / 'state').exists())
            self.assertFalse((self.folder / 'standard.conf').exists())

    def test_optional_pairing_is_strict_copied_and_matches_the_receiver_controller(self):
        settings = {'seconds': 0, 'media_state': self.value['stateDirectory'] + '/media.json'}
        self.assertNotIn('pairing', configuration.validate_configuration(self.value))
        original = self.value | {'pairing': {'enabled': False, 'adapter': 'hci0'}}
        validated = configuration.validate_manager_settings(original, settings)
        validated['pairing']['enabled'] = True
        self.assertFalse(original['pairing']['enabled'])
        self.assertTrue(configuration.validate_manager_settings(validated, settings)['pairing']['enabled'])
        other = self.value | {'pairing': {'enabled': False, 'adapter': 'hci1'}}
        self.assertEqual(configuration.validate_manager_settings(other, settings), other)
        with self.assertRaisesRegex(ValueError, 'same controller'):
            configuration.validate_manager_settings(other | {'pairing': {'enabled': True, 'adapter': 'hci1'}}, settings)
        for pairing in (None, [], {}, {'enabled': True}, {'adapter': 'hci0'},
                        {'enabled': 1, 'adapter': 'hci0'}, {'enabled': False, 'adapter': 'hci01'},
                        {'enabled': False, 'adapter': '/org/bluez/hci0'},
                        {'enabled': False, 'adapter': 'hci1000'},
                        {'enabled': False, 'adapter': 'hci0', 'autoConfirm': True}):
            with self.subTest(pairing=pairing), self.assertRaises(ValueError):
                configuration.validate_configuration(self.value | {'pairing': pairing})

    def test_receiver_recovery_is_optional_and_strictly_bounded(self):
        self.assertNotIn('receiverRecoveryAttempts', configuration.validate_configuration(self.value))
        for attempts in range(4):
            value = self.value | {'receiverRecoveryAttempts': attempts}
            self.assertEqual(configuration.validate_configuration(value), value)
        for attempts in (True, False, -1, 4, 1.0, '1', None):
            with self.subTest(attempts=attempts), self.assertRaises(ValueError):
                configuration.validate_configuration(self.value | {'receiverRecoveryAttempts': attempts})

    def test_schema_exactness_and_types(self):
        for field in self.value:
            value = dict(self.value); del value[field]
            with self.subTest(missing=field), self.assertRaises(ValueError):
                configuration.validate_configuration(value)
        for change in ({'extra': False}, {'schemaVersion': True}, {'schemaVersion': 0}, {'schemaVersion': 2},
                       {'enabled': 0}, {'enabled': 'false'}, {'autoConnectSelected': 1}, {'stateDirectory': None},
                       {'standardDisplayConfig': Path(self.value['standardDisplayConfig'])}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                configuration.validate_configuration(self.value | change)
        for value in (None, [], True, 'object'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                configuration.validate_configuration(value)

    def test_service_paths_reject_syntax_and_noncanonical_spelling(self):
        for path in ('relative', '/', '/tmp/state/', '/tmp//state', '//tmp/state', '/tmp/./state', '/tmp/../state',
                     '/tmp/a b', '/tmp/a\nb', '/tmp/a\0b', '/tmp/a;b', '/tmp/$(echo)', '/tmp/a%U', '/tmp/caméra',
                     '/tmp/' + 'a' * 4000, '/tmp/' + 'a' * 256):
            for field in ('stateDirectory', 'standardDisplayConfig'):
                with self.subTest(path=path, field=field), self.assertRaises(ValueError):
                    configuration.validate_configuration(self.value | {field: path})

    def test_socket_exact_length_boundary(self):
        # Use missing canonical descendants so the boundary checks create no
        # directories. /manager.sock occupies thirteen ASCII bytes.
        suffix = '/manager.sock'
        prefix = str(self.folder) + '/'
        state = prefix + 'x' * (103 - len(suffix) - len(prefix))
        self.assertEqual(len(state + suffix), 103)
        self.assertEqual(configuration.validate_configuration(self.value | {'stateDirectory': state})['stateDirectory'], state)
        with self.assertRaises(ValueError):
            configuration.validate_configuration(self.value | {'stateDirectory': state + 'x'})

    def test_existing_state_components_are_real_directories(self):
        state = self.folder / 'state'; state.mkdir(mode=0o700)
        self.assertEqual(configuration.validate_configuration(self.value), self.value)
        state.rmdir(); state.write_text('regular')
        with self.assertRaises(ValueError):
            configuration.validate_configuration(self.value)
        state.unlink(); state.symlink_to(self.folder, target_is_directory=True)
        with self.assertRaises(ValueError):
            configuration.validate_configuration(self.value)
        state.unlink()
        alias = self.folder / 'alias'; alias.symlink_to(self.folder, target_is_directory=True)
        with self.assertRaises(ValueError):
            configuration.validate_configuration(self.value | {'stateDirectory': str(alias / 'missing')})
        alias.unlink(); alias.symlink_to(self.folder / 'missing', target_is_directory=True)
        with self.assertRaises(ValueError):
            configuration.validate_configuration(self.value | {'stateDirectory': str(alias / 'missing')})

    def test_display_contents_are_left_for_the_real_checker(self):
        display = self.folder / 'standard.conf'
        display.write_text('not a display configuration')
        self.assertEqual(configuration.validate_configuration(self.value), self.value)

    def test_profile_regular_stable_read_and_strings_preserved(self):
        self.assertEqual(configuration.load_configuration(self.path), self.value)
        self.path.chmod(0o644)
        self.assertEqual(configuration.load_configuration(self.path), self.value)
        self.assertFalse((self.folder / 'state').exists())

    def test_duplicate_missing_unknown_malformed_and_nonfinite_json(self):
        raw = json.dumps(self.value)
        for text in (raw[:-1] + ',"enabled":false}', '{}', raw[:-1] + ',"extra":false}',
                     raw.replace('false', 'NaN', 1), '[{}]', '{', 'null', raw + '{}'):
            self.write(text.encode())
            with self.subTest(text=text), self.assertRaises(ValueError):
                configuration.load_configuration(self.path)

    def test_profile_bounds_empty_exact_maximum_and_oversize(self):
        raw = json.dumps(self.value).encode()
        self.write(raw + b' ' * (16384 - len(raw)))
        self.assertEqual(configuration.load_configuration(self.path), self.value)
        for content in (b'', raw + b' ' * (16385 - len(raw))):
            self.write(content)
            with self.assertRaises(ValueError):
                configuration.load_configuration(self.path)

    def test_trusted_owner_configured_root_and_unsafe_modes(self):
        # Some systems silently clear setgid when the inherited temporary-file
        # group is not ours. Use our actual group to exercise the special bit.
        os.chown(self.path, -1, os.getgid())
        for mode in (0o602, 0o620, 0o666, 0o1600, 0o2600, 0o4600):
            self.path.chmod(mode)
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                configuration.load_configuration(self.path)
        self.path.chmod(0o600)
        owner = self.path.stat().st_uid
        with mock.patch.object(configuration.os, 'getuid', return_value=0):
            self.assertEqual(configuration.load_configuration(self.path, owner_uid=owner), self.value)
            if owner != 0:
                with self.assertRaises(ValueError):
                    configuration.load_configuration(self.path)
        # Actual root-owned fixtures are permitted when the suite itself is
        # root. Changing real file ownership is otherwise unnecessary.
        if owner == 0:
            with mock.patch.object(configuration.os, 'getuid', return_value=12345):
                self.assertEqual(configuration.load_configuration(self.path), self.value)
        for uid in (True, -1, 1 << 32, '1000'):
            with self.subTest(uid=uid), self.assertRaises(ValueError):
                configuration.load_configuration(self.path, owner_uid=uid)

    def test_symlink_hardlink_directory_fifo_and_socket_rejected_before_open(self):
        original = self.folder / 'original'; self.path.rename(original)
        self.path.symlink_to(original)
        with self.assertRaises(ValueError):
            configuration.load_configuration(self.path)
        self.path.unlink(); os.link(original, self.path)
        with self.assertRaises(ValueError):
            configuration.load_configuration(self.path)
        self.path.unlink(); self.path.mkdir()
        with self.assertRaises(ValueError):
            configuration.load_configuration(self.path)
        self.path.rmdir(); os.mkfifo(self.path, 0o600)
        began = time.monotonic()
        with mock.patch.object(configuration.os, 'open', side_effect=AssertionError('Known FIFO opened')), self.assertRaises(ValueError):
            configuration.load_configuration(self.path)
        self.assertLess(time.monotonic() - began, 0.2)
        self.path.unlink()
        with socket.socket(socket.AF_UNIX) as endpoint:
            endpoint.bind(str(self.path))
            with self.assertRaises(ValueError):
                configuration.load_configuration(self.path)

    def test_inplace_change_or_atomic_replacement_during_read_rejected(self):
        original_read = os.read
        for replacement in (False, True):
            self.write(json.dumps(self.value).encode())
            calls = []

            def changed(fd, maximum):
                raw = original_read(fd, maximum)
                if not calls:
                    calls.append(True)
                    if replacement:
                        other = self.folder / 'replacement'
                        other.write_text(json.dumps(self.value)); other.chmod(0o600)
                        os.replace(other, self.path)
                    else:
                        self.write(b'changed')
                return raw

            with self.subTest(replacement=replacement), mock.patch.object(configuration.os, 'read', side_effect=changed), self.assertRaises(ValueError):
                configuration.load_configuration(self.path)

    def test_changed_open_generation_is_rejected_and_descriptor_closed(self):
        original_open = os.open
        captured = []

        def changed(path, flags):
            other = self.folder / 'replacement'; other.write_text(json.dumps(self.value)); other.chmod(0o600)
            os.replace(other, self.path)
            fd = original_open(path, flags); captured.append(fd)
            return fd

        with mock.patch.object(configuration.os, 'open', side_effect=changed), self.assertRaises(ValueError):
            configuration.load_configuration(self.path)
        self.assertEqual(len(captured), 1)
        with self.assertRaises(OSError):
            os.fstat(captured[0])

    def test_manager_settings_persistent_session_external_cluster_and_exact_media(self):
        settings = {'seconds': 0, 'fallback_cluster': False, 'media_state': self.value['stateDirectory'] + '/media.json',
                    'other_receiver_option': 'left for integration'}
        self.assertEqual(configuration.validate_manager_settings(self.value, settings), self.value)
        settings['media_state'] = Path(settings['media_state'])
        self.assertEqual(configuration.validate_manager_settings(self.value, settings), self.value)
        del settings['fallback_cluster']
        self.assertEqual(configuration.validate_manager_settings(self.value, settings), self.value)
        for change in ({'seconds': False}, {'seconds': 1}, {'seconds': None}, {'fallback_cluster': True},
                       {'fallback_cluster': 0}, {'media_state': str(self.folder / 'another.json')},
                       {'media_state': None}, {'media_state': self.value['stateDirectory'] + '/./media.json'}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                configuration.validate_manager_settings(self.value, settings | change)
        missing = dict(settings); del missing['seconds']
        with self.assertRaises(ValueError):
            configuration.validate_manager_settings(self.value, missing)


if __name__ == '__main__':
    unittest.main()
