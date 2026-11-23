# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual display-parser and read-only CLI checks; no radios/services start."""
import json
import os
from pathlib import Path
import pwd
import select
import signal
import socket
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / 'build/headunit-display-config-check'
sys.path.insert(0, str(ROOT / 'tools'))
from phone_manager_boot import inspect_configuration, inspect_settings, prepare_directory


RUNTIME_FIXTURE = r'''
# The production CLI, display parser, endpoint and event loop run unchanged.
# Only root admission and phone business operations are explicit fixtures;
# this does not qualify privilege dropping, BlueZ, a child receiver or systemd.
import fcntl, json, os, pathlib, sys
sys.path.insert(0, sys.argv.pop(1))
import phone_manager
class PrivilegeBoundary:
    def __getattr__(self, name): return getattr(os, name)
    def geteuid(self): return 0
phone_manager.os = PrivilegeBoundary()
class PhoneBusinessFixture:
    def __init__(self, config, folder, standard_display, owner, **options):
        self.folder = folder
        self.fd = options['receiver_lock_fd']
        profile = json.loads(pathlib.Path(sys.argv[sys.argv.index('--manager-profile') + 1]).read_text())
        assert options['recovery_attempts'] == profile.get('receiverRecoveryAttempts', 0)
        self.record('construct')
        print('ready', flush=True)
    def record(self, event):
        assert (self.folder / 'manager.sock').is_socket()
        assert os.fstat(self.fd).st_ino == (self.folder / 'manager.lock').stat().st_ino
        with (self.folder / 'manager.lock').open('rb') as contender:
            try: fcntl.flock(contender.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError: pass
            else: raise AssertionError('Manager ownership was not retained')
        with (self.folder / 'business-fixture-events').open('a') as log: log.write(event + '\n')
    def restore_selected(self): self.record('restore')
    def request(self, command): return {'fixtureEcho': command}
    def tick(self): pass
    def shutdown(self): self.record('shutdown')
phone_manager.Manager = PhoneBusinessFixture
sys.argv[0] = 'phone_manager.py'
phone_manager.main()
'''


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pm-boot-', dir='/tmp')
        self.root = Path(self.temp.name).resolve()
        self.root.chmod(0o700)
        self.state = self.root / 'state'
        self.main, self.standard = self.root / 'wide.conf', self.root / 'standard.conf'
        self.main.write_text(self.displays(1920))
        self.main.chmod(0o600)
        self.standard.write_text(self.displays(1280))
        self.standard.chmod(0o600)
        self.profile = self.root / 'manager.json'
        self.value = {'schemaVersion': 1, 'enabled': False, 'stateDirectory': str(self.state),
                      'standardDisplayConfig': str(self.standard), 'autoConnectSelected': False}
        self.write_profile()
        self.settings = {'display_config': str(self.main), 'bin_dir': str(CHECKER.parent),
                         'seconds': 0, 'media_state': str(self.state / 'media.json')}
        self.config = self.root / 'receiver.json'
        self.write_settings()
        self.uid = os.getuid() or next(a.pw_uid for a in pwd.getpwall() if a.pw_uid > 0)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def displays(main_width=1280):
        return (f'main.width={main_width}\nmain.height=720\nmain.fps=60\nmain.panel=Main\n'
                'main.wayland=wayland-0\nmain.output=HDMI-A-1\n'
                'cluster.width=1920\ncluster.height=720\ncluster.fps=30\ncluster.panel=Cluster\n'
                'cluster.wayland=wayland-0\ncluster.output=HDMI-A-2\n')

    def write_profile(self):
        self.profile.write_text(json.dumps(self.value)); self.profile.chmod(0o600)

    def write_settings(self):
        self.config.write_text(json.dumps(self.settings)); self.config.chmod(0o600)

    def test_actual_parser_accepts_different_main_canvases_and_one_cluster(self):
        self.assertEqual(inspect_configuration(self.config, self.profile, self.uid, CHECKER), self.value)
        self.assertFalse(self.state.exists())

    def test_actual_parser_rejects_moved_outputs_mismatched_cluster_and_bad_fields(self):
        original = self.displays()
        for before, after in (('main.output=HDMI-A-1', 'main.output=DP-9'),
                              ('cluster.width=1920', 'cluster.width=1600'),
                              ('cluster.output=HDMI-A-2', 'cluster.output=DP-9'),
                              ('cluster.wayland=wayland-0', 'cluster.wayland=wayland-2'),
                              ('main.wayland=wayland-0', 'main.wayland=wayland-2'),
                              ('main.width=1280', 'unknown.field=1280')):
            with self.subTest(before=before):
                self.standard.write_text(original.replace(before, after))
                with self.assertRaises((ValueError, subprocess.SubprocessError)):
                    inspect_settings(self.profile, self.settings, self.uid, CHECKER)
        self.assertFalse(self.state.exists())

    def test_static_display_aliases_and_unsafe_mode_are_not_admitted(self):
        self.standard.unlink(); self.standard.symlink_to(self.main)
        with self.assertRaises(ValueError): inspect_settings(self.profile, self.settings, self.uid, CHECKER)
        self.standard.unlink(); os.link(self.main, self.standard)
        with self.assertRaises(ValueError): inspect_settings(self.profile, self.settings, self.uid, CHECKER)
        self.standard.unlink(); self.standard.write_text(self.displays()); self.standard.chmod(0o666)
        with self.assertRaises(ValueError): inspect_settings(self.profile, self.settings, self.uid, CHECKER)

    def test_boot_profile_requires_a_real_path_without_parent_redirection(self):
        alias = self.root / 'alias'; alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            inspect_settings(alias / self.profile.name, self.settings, self.uid, CHECKER)
        linked = self.root / 'linked.json'; os.link(self.profile, linked)
        with self.assertRaises(ValueError):
            inspect_settings(linked, self.settings, self.uid, CHECKER)
        linked.unlink()
        redirected = self.root / 'redirected.json'; redirected.symlink_to(self.profile)
        with self.assertRaises(ValueError):
            inspect_settings(redirected, self.settings, self.uid, CHECKER)
        self.assertFalse(self.state.exists())

    def test_cli_validates_enabled_and_disabled_without_state_or_manager(self):
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                self.value['enabled'] = enabled; self.write_profile()
                result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/phone_manager.py'),
                                         '--config', str(self.config), '--manager-profile', str(self.profile),
                                         '--uid', str(self.uid), '--validate-only'],
                                        check=True, capture_output=True, text=True, timeout=5)
                self.assertEqual(json.loads(result.stdout), {'configurationValid': True, 'enabled': enabled,
                                                            'autoConnectSelected': False})
                self.assertFalse(self.state.exists())

    def test_pairing_enabled_profile_preflight_contacts_no_radio(self):
        self.value['pairing'] = {'enabled': True, 'adapter': 'hci0'}; self.write_profile()
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/phone_manager.py'),
                                 '--config', str(self.config), '--manager-profile', str(self.profile),
                                 '--uid', str(self.uid), '--validate-only'],
                                check=True, capture_output=True, text=True, timeout=5)
        self.assertTrue(json.loads(result.stdout)['configurationValid'])
        self.assertFalse(self.state.exists())

    def test_preflight_does_not_rewrite_existing_runtime_state(self):
        self.state.mkdir(mode=0o700)
        media = self.state / 'media.json'; media.write_text('retained private state\n'); media.chmod(0o600)
        before = media.stat()
        inspect_configuration(self.config, self.profile, self.uid, CHECKER)
        after = media.stat()
        self.assertEqual((after.st_ino, after.st_mtime_ns, media.read_text()),
                         (before.st_ino, before.st_mtime_ns, 'retained private state\n'))
        self.assertEqual([p.name for p in self.state.iterdir()], ['media.json'])

    def test_state_creation_does_not_take_over_unsafe_existing_directory(self):
        owner = os.getuid(), os.getgid()
        self.assertEqual(prepare_directory(self.state, owner), self.state)
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)
        self.state.chmod(0o755)
        with self.assertRaises(ValueError): prepare_directory(self.state, owner)
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o755)
        self.state.chmod(0o700)
        with self.assertRaises(ValueError): prepare_directory(self.state, (owner[0] + 1, owner[1]))
        self.assertEqual(self.state.stat().st_uid, owner[0])

    def start_runtime_fixture(self, auto_connect):
        self.value.update(enabled=True, autoConnectSelected=auto_connect); self.write_profile()
        self.state.mkdir(mode=0o700, exist_ok=True)
        if os.geteuid() == 0:
            os.chown(self.state, self.uid, pwd.getpwuid(self.uid).pw_gid)
        child = subprocess.Popen([sys.executable, '-B', '-c', RUNTIME_FIXTURE, str(ROOT / 'tools'),
                                  '--config', str(self.config), '--manager-profile', str(self.profile),
                                  '--uid', str(self.uid)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
            child.stdout.close(); child.stderr.close()
        self.addCleanup(cleanup)
        return child

    def ready(self, child):
        self.assertTrue(select.select([child.stdout], [], [], 5)[0], 'Runtime fixture startup timed out')
        self.assertEqual(child.stdout.readline(), b'ready\n')

    def test_actual_main_loop_claims_before_constructor_and_keeps_ownership_through_shutdown(self):
        child = self.start_runtime_fixture(auto_connect=False); self.ready(child)
        events = self.state / 'business-fixture-events'
        self.assertEqual(events.read_text().splitlines(), ['construct'])
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(3); client.connect(str(self.state / 'manager.sock'))
            client.sendall(b'{"action":"explicit-fixture"}\n')
            raw = bytearray()
            while not raw.endswith(b'\n'):
                part = client.recv(4096); self.assertTrue(part); raw.extend(part)
            self.assertEqual(json.loads(raw), {'ok': True, 'fixtureEcho': {'action': 'explicit-fixture'}})
        duplicate = self.start_runtime_fixture(auto_connect=False)
        _, error = duplicate.communicate(timeout=5)
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertIn(b'already has a live owner', error)
        self.assertEqual(events.read_text().splitlines(), ['construct'])
        child.send_signal(signal.SIGTERM); child.wait(timeout=5)
        self.assertEqual(child.returncode, 0, child.stderr.read().decode())
        self.assertEqual(events.read_text().splitlines(), ['construct', 'shutdown'])
        self.assertFalse((self.state / 'manager.sock').exists())
        self.assertTrue((self.state / 'manager.lock').is_file())

    def test_actual_main_loop_passes_the_configured_recovery_policy(self):
        self.value['receiverRecoveryAttempts'] = 3
        child = self.start_runtime_fixture(auto_connect=True)
        self.ready(child)
        child.send_signal(signal.SIGTERM)
        child.wait(timeout=5)
        self.assertEqual(child.returncode, 0, child.stderr.read().decode())

    def test_actual_main_loop_attempts_selected_boot_restore_only_when_configured(self):
        child = self.start_runtime_fixture(auto_connect=True); self.ready(child)
        # The serial request is a barrier after the one configured boot call.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(3); client.connect(str(self.state / 'manager.sock'))
            client.sendall(b'{"action":"barrier"}\n'); self.assertTrue(client.recv(4096))
        child.send_signal(signal.SIGTERM); child.wait(timeout=5)
        self.assertEqual(child.returncode, 0, child.stderr.read().decode())
        self.assertEqual((self.state / 'business-fixture-events').read_text().splitlines(),
                         ['construct', 'restore', 'shutdown'])


if __name__ == '__main__':
    unittest.main()
