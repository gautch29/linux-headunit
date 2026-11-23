#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual static parser/file/ownership checks, without GTK, radios or displays."""
import copy
import fcntl
import json
import os
from pathlib import Path
import select
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from filesystem_fixture import change_metadata


ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else ROOT / 'build/headunit-display-config-check'
sys.path.insert(0, str(ROOT / 'tools'))
import phone_page_configuration as page
from phone_manager_endpoint import ManagerEndpoint


CHILD_OWNER = '''
import signal, sys
sys.path.insert(0, sys.argv[1])
from phone_page_configuration import PageOwner
with PageOwner(sys.argv[2]):
    print('ready', flush=True)
    signal.pause()
'''
CHILD_EXEC = '''
import signal
print('ready', flush=True)
signal.pause()
'''


class Tests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='native-page-', dir='/tmp')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.root.chmod(0o700)
        self.state = self.root / 'state'
        self.profile = self.root / 'page.json'
        self.value = {'schemaVersion': 2, 'enabled': True, 'managerSocket': str(self.state / 'manager.sock'),
                      'surfaceControlFile': str(self.state / 'home-surface.request'),
                      'surfaceLeaseFile': str(self.state / 'home-surface.lease'),
                      'waylandDisplay': 'wayland-0', 'output': 'HDMI-A-1',
                      'homeButton': {'corner': 'bottom-right', 'margin': 16, 'size': 64}}
        self.write_page()
        self.ultra, self.standard = self.root / 'ultra.conf', self.root / 'standard.conf'
        self.ultra.write_text(self.displays(1920))
        self.standard.write_text(self.displays(1280))
        self.ultra.chmod(0o600); self.standard.chmod(0o600)
        self.manager_path = self.root / 'manager.json'
        self.manager = {'schemaVersion': 1, 'enabled': True, 'stateDirectory': str(self.state),
                        'standardDisplayConfig': str(self.standard), 'autoConnectSelected': False}
        self.write_manager()
        self.settings = {'seconds': 0, 'media_state': str(self.state / 'media.json'),
                         'display_config': str(self.ultra), 'bin_dir': str(CHECKER.parent), 'preview_sink': 'wayland-gpu',
                         'native_page_control_file': self.value['surfaceControlFile'],
                         'native_page_lease_file': self.value['surfaceLeaseFile']}

    @staticmethod
    def displays(width):
        return (f'main.width={width}\nmain.height=720\nmain.fps=60\nmain.panel=Main\n'
                'main.wayland=wayland-0\nmain.output=HDMI-A-1\n'
                'cluster.width=1920\ncluster.height=720\ncluster.fps=30\ncluster.panel=Cluster\n'
                'cluster.wayland=wayland-0\ncluster.output=HDMI-A-2\n')

    def write_page(self):
        self.profile.write_text(json.dumps(self.value)); self.profile.chmod(0o600)

    def write_manager(self):
        self.manager_path.write_text(json.dumps(self.manager)); self.manager_path.chmod(0o600)

    def inspect(self):
        return page.inspect_settings(self.profile, self.manager_path, self.settings, os.getuid(), CHECKER)

    def directory(self):
        self.state.mkdir(mode=0o700, exist_ok=True)
        return self.state

    def owner(self):
        owner = page.PageOwner(self.directory())
        self.addCleanup(owner.close)
        return owner

    def child(self, code=CHILD_OWNER, close_fds=True):
        process = subprocess.Popen([sys.executable, '-B', '-c', code, str(ROOT / 'tools'), str(self.state)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, close_fds=close_fds)
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)
            process.stdout.close(); process.stderr.close()
        self.addCleanup(cleanup)
        return process

    def ready(self, process):
        self.assertTrue(select.select([process.stdout], [], [], 3)[0], 'Owned page fixture did not become ready')
        self.assertEqual(process.stdout.readline(), b'ready\n')

    def test_exact_schema_disabled_is_valid_and_copy_has_no_shared_mutable_fields(self):
        for enabled in (False, True):
            value = copy.deepcopy(self.value); value['enabled'] = enabled
            validated = page.validate_configuration(value)
            self.assertEqual(validated, value)
            validated['homeButton']['margin'] = 0
            self.assertEqual(value['homeButton']['margin'], 16)
        self.assertEqual(page.load_configuration(self.profile, os.getuid()), self.value)
        self.assertFalse(self.state.exists())

    def test_schema_rejects_unknown_missing_types_and_home_button_bounds(self):
        bad = []
        for key in self.value:
            value = copy.deepcopy(self.value); value.pop(key); bad.append(value)
        for key, item in (('schemaVersion', True), ('schemaVersion', 3), ('enabled', 1), ('extra', False),
                          ('homeButton', []), ('output', 0)):
            value = copy.deepcopy(self.value); value[key] = item; bad.append(value)
        for key, items in (('corner', ('center', '', 0)), ('margin', (-1, 97, True, 1.5)),
                           ('size', (47, 97, True, 64.0)), ('enabled', (0, 1, None, 'false')), ('extra', (0,))):
            for item in items:
                value = copy.deepcopy(self.value); value['homeButton'][key] = item; bad.append(value)
        for value in bad:
            with self.subTest(value=value):
                with self.assertRaises(ValueError): page.validate_configuration(value)

    def test_home_button_can_be_explicitly_disabled_without_changing_legacy_default(self):
        for enabled in (False, True):
            value = copy.deepcopy(self.value)
            value['homeButton']['enabled'] = enabled
            self.profile.write_text(json.dumps(value))
            self.assertEqual(page.load_configuration(self.profile), value)
            self.assertFalse(self.state.exists())
        self.assertNotIn('enabled', page.validate_configuration(self.value)['homeButton'])

    def test_paths_and_display_names_are_explicit_canonical_and_bounded(self):
        for field, values in (
                ('managerSocket', ('relative', '/', '/tmp//manager.sock', '/tmp/./manager.sock',
                                   '/tmp/../manager.sock', '/tmp/manager.sock/', '/tmp/a b', '/tmp/$HOME',
                                   '/tmp/$(id)', '/tmp/a;id', '/tmp/a\\b', '/tmp/é', '/tmp/a\x00', '/' + 'a' * 103)),
                ('waylandDisplay', ('', '.', '..', '/run/user/wayland-0', 'wayland 0', 'wayland;id', 'a' * 128)),
                ('output', ('', '.', '..', 'HDMI-A-1\n', 'é', 'a' * 128))):
            for item in values:
                value = copy.deepcopy(self.value); value[field] = item
                with self.subTest(field=field, item=item):
                    with self.assertRaises(ValueError): page.validate_configuration(value)
        for uid in (True, -1, 1 << 32, '1000'):
            with self.assertRaises(ValueError): page.load_configuration(self.profile, uid)

    def test_loader_rejects_duplicate_json_nonfinite_and_unsafe_real_data(self):
        for raw in ('{"schemaVersion":1,"schemaVersion":1}', '{"homeButton":{"size":64,"size":65}}',
                    '{"enabled":NaN}', '{"enabled":Infinity}', '[]', '', 'x' * 16385):
            self.profile.write_text(raw)
            with self.subTest(raw=raw[:80]):
                with self.assertRaises(ValueError): page.load_configuration(self.profile)
        self.write_page()
        # Darwin may silently drop SGID for an inherited group outside the
        # caller's membership; use our actual group so this is unsafe data.
        os.chown(self.profile, -1, os.getgid())
        for mode in (0o622, 0o660, 0o666, 0o4600, 0o2600, 0o1600):
            self.profile.chmod(mode)
            with self.subTest(mode=oct(mode)):
                self.assertEqual(stat.S_IMODE(self.profile.stat().st_mode), mode)
                with self.assertRaises(ValueError): page.load_configuration(self.profile)
        self.profile.chmod(0o600)
        alias = self.root / 'alias'; alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError): page.load_configuration(alias / self.profile.name)
        linked = self.root / 'link.json'; os.link(self.profile, linked)
        with self.assertRaises(ValueError): page.load_configuration(self.profile)
        linked.unlink()
        symlink = self.root / 'symlink.json'; symlink.symlink_to(self.profile)
        with self.assertRaises(ValueError): page.load_configuration(symlink)
        fifo = self.root / 'fifo'; os.mkfifo(fifo, 0o600)
        with self.assertRaises(ValueError): page.load_configuration(fifo)
        with self.assertRaises(ValueError): page.load_configuration(self.root)

    def test_file_mutation_during_real_read_is_rejected(self):
        read = os.read
        changed = False
        def mutate(fd, maximum):
            nonlocal changed
            result = read(fd, maximum)
            if not changed:
                changed = True
                self.profile.write_text(json.dumps(dict(self.value, enabled=False)))
            return result
        with mock.patch.object(page.os, 'read', side_effect=mutate):
            with self.assertRaises(ValueError): page.load_configuration(self.profile)
        self.assertTrue(changed)
        self.assertFalse(self.state.exists())

    def test_legacy_disabled_loads_but_enabled_legacy_cannot_cover_carplay(self):
        legacy = {key: item for key, item in self.value.items() if key not in ('surfaceControlFile', 'surfaceLeaseFile')}
        legacy.update(schemaVersion=1, enabled=False)
        self.assertEqual(page.validate_configuration(legacy), legacy)
        self.profile.write_text(json.dumps(legacy))
        self.assertEqual(page.load_configuration(self.profile), legacy)
        with self.assertRaises(ValueError): page.validate_configuration(dict(legacy, enabled=True))
        self.assertFalse(self.state.exists())

    def test_surface_admission_rejects_missing_pair_collisions_and_wrong_parent_without_writes(self):
        original = dict(self.settings)
        for changes in ({'native_page_control_file': str(self.state / 'other.request')},
                        {'native_page_lease_file': str(self.root / 'home-surface.lease')},
                        {'native_page_control_file': str(self.state / 'manager.lock')},
                        {'preview_sink': 'wayland-touch'}, {'camera_manual_profile': str(self.root / 'camera.json')},
                        {'media_state': self.value['surfaceControlFile']}):
            self.settings = original | changes
            with self.subTest(changes=changes), self.assertRaises(ValueError): self.inspect()
        for key in ('native_page_control_file', 'native_page_lease_file'):
            self.settings = dict(original); self.settings.pop(key)
            with self.subTest(missing=key), self.assertRaises(ValueError): self.inspect()
        self.settings = original
        for filename in ('manager.sock', 'page.lock', 'manager.lock', 'media.json', 'audio-state.json'):
            value = dict(self.value, surfaceControlFile=str(self.state / filename))
            with self.subTest(filename=filename), self.assertRaises(ValueError): page.validate_configuration(value)
        self.assertFalse(self.state.exists())

    def test_actual_parser_admits_both_main_canvases_without_runtime_side_effects(self):
        before = {p.name: (p.stat().st_ino, p.stat().st_mtime_ns, p.read_bytes())
                  for p in self.root.iterdir()}
        self.assertEqual(self.inspect(), self.value)
        after = {p.name: (p.stat().st_ino, p.stat().st_mtime_ns, p.read_bytes())
                 for p in self.root.iterdir()}
        self.assertEqual(before, after)
        self.assertFalse(self.state.exists())
        self.assertNotIn('gi', sys.modules)

    def test_admission_rejects_disabled_manager_page_and_other_endpoint(self):
        for target in ('manager', 'page', 'socket'):
            self.manager['enabled'] = True; self.value['enabled'] = True
            self.value['managerSocket'] = str(self.state / 'manager.sock')
            if target == 'manager': self.manager['enabled'] = False
            elif target == 'page': self.value['enabled'] = False
            else: self.value['managerSocket'] = str(self.state / 'other.sock')
            self.write_page(); self.write_manager()
            with self.subTest(target=target):
                with self.assertRaises(ValueError): self.inspect()
        self.assertFalse(self.state.exists())

    def test_actual_parser_rejects_wrong_page_or_either_display_route(self):
        for field, item in (('waylandDisplay', 'wayland-2'), ('output', 'HDMI-A-2')):
            original = self.value[field]; self.value[field] = item; self.write_page()
            with self.assertRaises(ValueError): self.inspect()
            self.value[field] = original; self.write_page()
        for path in (self.ultra, self.standard):
            original = path.read_text()
            for before, after in (('main.output=HDMI-A-1', 'main.output=DP-9'),
                                  ('main.wayland=wayland-0', 'main.wayland=wayland-2'),
                                  ('cluster.output=HDMI-A-2', 'cluster.output=HDMI-A-1'),
                                  ('cluster.width=1920', 'cluster.width=1600'),
                                  ('main.fps=60', 'unknown.field=60')):
                path.write_text(original.replace(before, after))
                with self.subTest(path=path.name, before=before):
                    with self.assertRaises((ValueError, subprocess.SubprocessError)): self.inspect()
            path.write_text(original)
        self.assertFalse(self.state.exists())

    def test_actual_parser_rejects_redirected_or_unsafe_profile_display_files(self):
        alias = self.root / 'alias'; alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            page.inspect_settings(alias / self.profile.name, self.manager_path, self.settings, os.getuid(), CHECKER)
        for path in (self.manager_path, self.standard, self.ultra):
            original = path.read_bytes(); path.unlink(); path.symlink_to(self.profile)
            with self.subTest(path=path.name):
                with self.assertRaises(ValueError): self.inspect()
            path.unlink(); path.write_bytes(original); path.chmod(0o600)
            path.chmod(0o666)
            with self.assertRaises(ValueError): self.inspect()
            path.chmod(0o600)
        self.assertFalse(self.state.exists())

    def test_admission_rejects_a_display_change_between_actual_parser_passes(self):
        import phone_manager_boot
        actual_inspect = phone_manager_boot.inspect_settings
        def change_after_admission(*arguments, **options):
            result = actual_inspect(*arguments, **options)
            self.standard.write_text(self.displays(1280).replace('cluster.width=1920', 'cluster.width=1600'))
            return result
        with mock.patch.object(phone_manager_boot, 'inspect_settings', side_effect=change_after_admission):
            with self.assertRaisesRegex(ValueError, 'changed during validation'): self.inspect()
        self.assertFalse(self.state.exists())

    def test_admission_rejects_changed_bytes_with_equal_metadata_receipts(self):
        import phone_manager_boot
        actual_inspect = phone_manager_boot.inspect_settings
        def metadata_without_times(info):
            # Explicit coarse-metadata fixture. The real files and C++ parser
            # still participate; unchanged timestamps cannot witness a rewrite.
            return (info.st_dev, info.st_ino, info.st_size, info.st_uid, info.st_mode, info.st_nlink)
        def change_after_admission(*arguments, **options):
            result = actual_inspect(*arguments, **options)
            self.standard.write_text(self.displays(1280).replace('cluster.width=1920', 'cluster.width=1600'))
            return result
        with mock.patch.object(phone_manager_boot, '_signature', side_effect=metadata_without_times), \
             mock.patch.object(page, '_signature', side_effect=metadata_without_times), \
             mock.patch.object(phone_manager_boot, 'inspect_settings', side_effect=change_after_admission):
            with self.assertRaisesRegex(ValueError, 'changed during validation'): self.inspect()
        self.assertFalse(self.state.exists())

    def test_owner_requires_existing_real_private_directory_and_safe_retained_lock(self):
        with self.assertRaises(FileNotFoundError): page.PageOwner(self.state)
        self.assertFalse(self.state.exists())
        self.directory().chmod(0o755)
        with self.assertRaises(ValueError): page.PageOwner(self.state)
        self.state.chmod(0o700)
        alias = self.root / 'alias'; alias.symlink_to(self.state, target_is_directory=True)
        with self.assertRaises(ValueError): page.PageOwner(alias)
        lock = self.state / 'page.lock'
        for mode, content in ((0o644, ''), (0o660, ''), (0o4600, ''), (0o600, 'not empty')):
            lock.write_text(content); lock.chmod(mode)
            with self.subTest(mode=oct(mode), content=content):
                with self.assertRaises(ValueError): page.PageOwner(self.state)
            self.assertEqual(lock.read_text(), content)
            self.assertEqual(stat.S_IMODE(lock.stat().st_mode), mode)
        lock.unlink()
        lock.symlink_to(self.profile)
        with self.assertRaises(ValueError): page.PageOwner(self.state)
        lock.unlink()
        lock.touch(mode=0o600); os.link(lock, self.state / 'hardlink')
        with self.assertRaises(ValueError): page.PageOwner(self.state)
        lock.unlink(); (self.state / 'hardlink').unlink()
        os.mkfifo(lock, 0o600)
        with self.assertRaises(ValueError): page.PageOwner(self.state)
        lock.unlink()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as endpoint:
            endpoint.bind(str(lock)); lock.chmod(0o600)
            with self.assertRaises(ValueError): page.PageOwner(self.state)

    def test_owner_is_exclusive_independent_of_manager_and_retains_unchanged_inode(self):
        with ManagerEndpoint(self.directory()) as manager:
            owner = self.owner(); owner.verify()
            self.assertEqual(owner.path, self.state / 'page.lock')
            info = owner.path.stat()
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
            self.assertEqual(info.st_size, 0)
            with self.assertRaisesRegex(ValueError, 'live owner'): page.PageOwner(self.state)
            owner.close(); owner.close()
            self.assertEqual(page._signature(info), page._signature(owner.path.stat()))
            with page.PageOwner(self.state) as restarted: restarted.verify()
            manager._verify_socket()
            self.assertTrue(manager.path.is_socket())
        self.assertEqual(page._signature(info), page._signature(owner.path.stat()))

    def test_real_process_owner_death_allows_restart_and_exec_does_not_inherit_lock(self):
        self.directory()
        process = self.child(); self.ready(process)
        identity = (self.state / 'page.lock').stat()
        with self.assertRaisesRegex(ValueError, 'live owner'): page.PageOwner(self.state)
        process.send_signal(signal.SIGKILL); process.wait(timeout=3)
        with page.PageOwner(self.state) as owner:
            self.assertEqual(page._signature(identity), page._signature(owner.path.stat()))
            for fd in (owner._lock_fd, owner._directory_fd):
                self.assertTrue(fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC)
            child = self.child(CHILD_EXEC, close_fds=False); self.ready(child)
            owner.close()
            # The deliberately still-running exec child did not inherit the lock.
            with page.PageOwner(self.state) as replacement: replacement.verify()
            self.assertIsNone(child.poll())

    def test_owner_detects_lock_replacement_and_never_mutates_new_generation(self):
        owner = self.owner()
        old = self.state / 'old.lock'; owner.path.rename(old)
        owner.path.touch(mode=0o600)
        replacement_before = page._signature(owner.path.stat())
        with self.assertRaises(ValueError): owner.verify()
        with page.PageOwner(self.state) as replacement:
            owner.close()
            replacement.verify()
            self.assertEqual(replacement_before, page._signature(replacement.path.stat()))
        self.assertTrue(old.exists())

    def test_owner_rejects_metadata_changes_even_after_mode_is_restored(self):
        owner = self.owner()
        before = owner.path.stat()
        owner.path.chmod(0o644)
        with self.assertRaises(ValueError): owner.verify()
        change_metadata(owner.path, lambda: owner.path.chmod(0o644), before)
        owner.path.chmod(0o600)
        with self.assertRaises(ValueError): owner.verify()
        owner.close()
        with page.PageOwner(self.state) as restarted: restarted.verify()

    def test_owner_detects_parent_replacement_or_ancestor_redirection_without_cleanup_mutation(self):
        owner = self.owner()
        moved = self.root / 'moved'; self.state.rename(moved)
        self.state.mkdir(mode=0o700)
        sentinel = self.state / 'page.lock'; sentinel.write_text('foreign generation'); sentinel.chmod(0o600)
        before = page._signature(sentinel.stat())
        with self.assertRaises(ValueError): owner.verify()
        owner.close()
        self.assertEqual(before, page._signature(sentinel.stat()))
        self.assertEqual(sentinel.read_text(), 'foreign generation')
        self.assertTrue((moved / 'page.lock').is_file())
        sentinel.unlink(); self.state.rmdir(); self.state.symlink_to(moved, target_is_directory=True)
        with self.assertRaises(ValueError): page.PageOwner(self.state)


if __name__ == '__main__':
    unittest.main()
