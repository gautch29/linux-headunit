# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Sealed manager/launcher contracts with real release files, flock and children.

The ELF headers are explicit non-executed architecture fixtures. The actual
portable display checker substitutes only that binary boundary. BlueZ and AP
execution are never attempted; these tests do not qualify target hardware.
"""
import argparse
import json
import os
from pathlib import Path
import pwd
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / 'build/headunit-display-config-check'
sys.path.insert(0, str(ROOT / 'tools'))
import appliance_release as release
import phone_manager
import run_lab_receiver as launcher
from phone_manager_endpoint import ManagerEndpoint
ADMIT = launcher.sealed_manager_configuration

A, B = 'AA:BB:CC:DD:EE:01', 'AA:BB:CC:DD:EE:02'
NONCE = '0123456789abcdef0123456789abcdef'


def selectors(config, binding=None, **values):
    defaults = dict(config=config, release_root=None, release_manifest_sha256=None,
                    manager_profile=None, managed_phone=None, manager_lock_fd=None,
                    managed_audio_volume_percent=None, managed_audio_muted=None, managed_runtime_status_nonce=None)
    if values.get('managed_phone') is not None:
        defaults['managed_runtime_status_nonce'] = NONCE
    if binding:
        defaults.update(release_root=Path(binding['path']), release_manifest_sha256=binding['sha256'])
    return argparse.Namespace(**(defaults | values))


def flags(value, dry_run=False):
    result = []
    for key, item in vars(value).items():
        if item is not None:
            result.extend(('--' + key.replace('_', '-'), str(item)))
    if dry_run:
        result.append('--dry-run')
    return result


class Phones:
    def __init__(self): self.disconnected = []
    def phone(self, _): return {}
    def phones(self): return [{'address': p, 'name': 'Fixture', 'connected': True} for p in (A, B)]
    def disconnect(self, phone): self.disconnected.append(phone)


# Execute the production selected-settings/lock admission in a real inherited
# child process. The only substituted boundary is the non-runnable AArch64
# checker fixture; this child deliberately never activates Wi-Fi or receivers.
# Model the launcher's graceful SIGTERM exit after owned cleanup, not signal death.
CHILD = r'''
import argparse, functools, json, os, pathlib, signal, sys
command=json.loads(sys.argv[1]); checker=sys.argv[2]
script=next(pathlib.Path(v) for v in command if v.endswith('/tools/run_lab_receiver.py'))
sys.path.insert(0,str(script.parent))
import run_lab_receiver as launcher
launcher.sealed_manager_configuration=functools.partial(launcher.sealed_manager_configuration,checker=pathlib.Path(checker))
parser=argparse.ArgumentParser(allow_abbrev=False)
parser.add_argument('--config',type=pathlib.Path)
parser.add_argument('--release-root',type=pathlib.Path)
parser.add_argument('--release-manifest-sha256')
parser.add_argument('--manager-profile',type=pathlib.Path)
parser.add_argument('--managed-phone')
parser.add_argument('--manager-lock-fd',type=int)
parser.add_argument('--managed-audio-volume-percent',type=int)
parser.add_argument('--managed-audio-muted'); parser.add_argument('--managed-runtime-status-nonce')
parser.add_argument('--dry-run',action='store_true')
arguments=command[command.index(str(script))+1:]
options=parser.parse_args(arguments)
settings,binding=launcher.launch_settings(options,arguments)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
print(json.dumps({'settings':settings,'binding':binding,'lock':os.fstat(options.manager_lock_fd).st_ino},default=str),flush=True)
if not options.dry_run:
    signal.pause()
'''


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pm-release-', dir='/tmp')
        self.root = Path(self.temp.name).resolve(); self.root.chmod(0o700)
        # Darwin inherits a directory's group; /private/tmp is wheel. This is
        # our own fixture directory, not a mutation of the temporary parent.
        os.chown(self.root, -1, os.getgid())
        self.repo = self.root / 'repo'; self.repo.mkdir()
        self.source = self.root / 'source'; self.source.mkdir(mode=0o700)
        self.external = self.root / 'external'; self.external.mkdir(mode=0o700)
        self.state = self.external / 'manager'
        self.pool = self.root / 'releases'; self.pool.mkdir(mode=0o700)
        self.account = pwd.getpwuid(os.getuid())
        if self.account.pw_uid == 0:
            accounts = [a for a in pwd.getpwall() if 0 < a.pw_uid < 65536]
            if not accounts: self.skipTest('Fixture requires an unprivileged display account')
            self.account = accounts[0]
        self.owner = self.account.pw_uid, self.account.pw_gid
        self.roles = set(release.REQUIRED_ROLES) | {'phone_manager', 'standard_displays', 'ultra', 'vehicle_profile'}
        self.execution = {'receiver': 'phone-manager', 'resident_home': False}
        for name in release.inventory(self.roles, None, self.execution):
            if name.startswith('config/'): continue
            path = self.repo / name; path.parent.mkdir(parents=True, exist_ok=True)
            if name.startswith('build/'):
                raw = bytearray(64); raw[:7] = b'\x7fELF\x02\x01\x01'
                struct.pack_into('<HHI', raw, 16, 3, 183, 1)
                path.write_bytes(raw); path.chmod(0o755)
            else:
                shutil.copyfile(ROOT / name, path)
        for role in self.roles:
            path = self.source / release.ROLES[role][0]
            if role in ('displays', 'standard_displays'):
                path.write_text(self.displays(1920 if role == 'displays' else 1280))
            elif role in ('labwc', 'outputs'):
                shutil.copyfile(ROOT / 'config' / release.ROLES[role][0], path)
            else:
                path.write_text('fixture\n')
            path.chmod(0o600)
        self.profile = {'schemaVersion': 1, 'enabled': True, 'stateDirectory': str(self.state),
                        'standardDisplayConfig': str(self.source / release.ROLES['standard_displays'][0]),
                        'autoConnectSelected': False}
        (self.source / release.ROLES['phone_manager'][0]).write_text(json.dumps(self.profile))
        (self.source / release.ROLES['ultra'][0]).write_text(
            'assetDirectory=' + str(self.external / 'assets') + '\nassetIdentifier=fixture\nassetVersion=1\n'
            'vehicleProfile=' + str(self.source / release.ROLES['vehicle_profile'][0]) + '\n'
            'acceptanceReference=' + str(self.external / 'acceptance.conf') + '\n')
        self.settings = {'display_config': str(self.source / release.ROLES['displays'][0]),
                         'bin_dir': str(self.repo / 'build'), 'seconds': 0,
                         'preview_sink': 'wayland-touch', 'pair_store': str(self.external / 'pairs'),
                         'media_state': str(self.state / 'media.json'), 'direct_media': True,
                         'ultra_installed_ui': str(self.source / release.ROLES['ultra'][0]),
                         'ultra_negotiate': True, 'ultra_native_video': True,
                         'audio_sink': 'fakesink', 'audio_control': str(self.external / 'old-control'),
                         'audio_state': str(self.external / 'old-state'),
                         'audio_volume_percent': 88, 'audio_muted': False}
        (self.source / release.ROLES['receiver'][0]).write_text(json.dumps(self.settings))
        recipe = {'version': 3, 'release_id': 'managed-one', 'source_revision': 'a' * 40,
                  'repo': str(self.repo), 'user': self.account.pw_name,
                  'configuration': {role: str(self.source / release.ROLES[role][0]) for role in self.roles},
                  'native_camera': None, 'execution': self.execution}
        self.recipe = recipe
        self.binding = release.stage(recipe, self.pool)
        self.sealed = Path(self.binding['path'])
        self.config = release.config_path(self.sealed, 'receiver')
        self.manager_profile = release.config_path(self.sealed, 'phone_manager')
        self.standard = release.config_path(self.sealed, 'standard_displays')
        self.children = []

    def tearDown(self):
        for child in self.children:
            if child.poll() is None: child.kill()
            child.wait(timeout=3)
            for stream in (child.stdin, child.stdout, child.stderr):
                if stream: stream.close()
        for directory, _, _ in os.walk(self.root): os.chmod(directory, 0o700)
        self.temp.cleanup()

    @staticmethod
    def displays(width):
        return (f'main.width={width}\nmain.height=720\nmain.fps=60\nmain.panel=Main\n'
                'main.wayland=wayland-0\nmain.output=HDMI-A-1\n'
                'cluster.width=1920\ncluster.height=720\ncluster.fps=30\ncluster.panel=Cluster\n'
                'cluster.wayland=wayland-0\ncluster.output=HDMI-A-2\n')

    def admit(self, *args, **kwargs):
        return ADMIT(*args, **kwargs, checker=CHECKER)

    def launch(self, selected, arguments):
        with patch.object(launcher, 'sealed_manager_configuration', self.admit):
            return launcher.launch_settings(selected, arguments)

    def state_directory(self):
        self.state.mkdir(mode=0o700)
        if os.geteuid() == 0: os.chown(self.state, *self.owner)

    def test_recovery_policy_is_retained_and_cannot_be_overridden_at_runtime(self):
        self.profile['receiverRecoveryAttempts'] = 3
        (self.source / release.ROLES['phone_manager'][0]).write_text(json.dumps(self.profile))
        binding = release.stage(dict(self.recipe, release_id='managed-recovery'), self.pool)
        sealed = Path(binding['path'])
        config = release.config_path(sealed, 'receiver')
        profile = release.config_path(sealed, 'phone_manager')
        standard = release.config_path(sealed, 'standard_displays')
        _, _, admitted = self.admit(config, profile, binding, self.owner[0])
        self.assertEqual(admitted['receiverRecoveryAttempts'], 3)
        self.state_directory()
        with ManagerEndpoint(self.state, self.owner) as endpoint, \
                patch.object(phone_manager, 'sealed_manager_configuration', self.admit):
            options = dict(bluez=Phones(), receiver_lock_fd=endpoint.inherited_lock_fd,
                           release_binding=binding, manager_profile=profile)
            with self.assertRaisesRegex(ValueError, 'differs from the sealed profile'):
                phone_manager.Manager(config, self.state, standard, self.owner, **options)
            manager = phone_manager.Manager(config, self.state, standard, self.owner,
                                            recovery_attempts=3, **options)
            self.assertEqual(manager.recovery_attempts, 3)
            manager.shutdown()

    def test_actual_release_captured_admission_is_read_only_and_exact(self):
        manifest, base, profile = self.admit(self.config, self.manager_profile, self.binding, self.owner[0])
        self.assertEqual(manifest['execution'], self.execution)
        self.assertEqual(base['display_config'], release.config_path(self.sealed, 'displays'))
        self.assertEqual(profile['standardDisplayConfig'], str(self.standard))
        self.assertFalse(self.state.exists())
        mutable = self.external / 'managed-receiver.json'; mutable.write_bytes(self.config.read_bytes())
        with self.assertRaises(ValueError): self.admit(mutable, self.manager_profile, self.binding)
        with self.assertRaises(ValueError): self.admit(self.config, self.source / 'phone-manager.json', self.binding)
        with self.assertRaises(ValueError): self.admit(self.config, self.manager_profile, self.binding, self.owner[0] + 1)
        self.assertFalse(self.state.exists())

    def test_bound_base_preflight_cannot_execute_or_take_partial_selectors(self):
        value = selectors(self.config, self.binding)
        base, binding = self.launch(value, flags(value, True))
        self.assertEqual(binding, self.binding); self.assertEqual(base['bin_dir'], self.sealed / 'build')
        with self.assertRaisesRegex(ValueError, 'dry-run only'): self.launch(value, flags(value))
        for options in ({'manager_profile': self.manager_profile}, {'managed_phone': A},
                        {'manager_lock_fd': 9},
                        {'managed_audio_volume_percent': 20}, {'managed_audio_muted': 'false'}):
            partial = selectors(self.config, self.binding, **options)
            with self.subTest(options=options), self.assertRaises(ValueError): self.launch(partial, flags(partial, True))
        self.assertFalse(self.state.exists())

    def test_transient_profile_replace_inspect_restore_cannot_supply_unverified_values(self):
        import phone_manager_boot
        original = self.manager_profile.read_bytes()
        inspector = phone_manager_boot.inspect_settings
        def transient_profile(profile_path, settings, uid, checker):
            changed = json.loads(original)
            changed['autoConnectSelected'] = True
            changed['pairing'] = {'enabled': True, 'adapter': 'hci0'}
            self.manager_profile.chmod(0o600)
            self.manager_profile.write_text(json.dumps(changed))
            try:
                return inspector(profile_path, settings, uid, checker)
            finally:
                self.manager_profile.write_bytes(original)
                self.manager_profile.chmod(0o440)
        with patch.object(phone_manager_boot, 'inspect_settings', transient_profile):
            with self.assertRaisesRegex(ValueError, 'captured release profile'):
                self.admit(self.config, self.manager_profile, self.binding)
        self.assertEqual(self.manager_profile.read_bytes(), original)
        self.assertEqual(release.verify(self.binding)['execution'], self.execution)
        self.assertFalse(self.state.exists())

    def test_cli_overrides_duplicate_and_unbound_selectors_fail_closed(self):
        value = selectors(self.config, self.binding)
        for extra in (['--config', str(self.config)], ['--bin-dir', '/tmp'], ['--seconds=1'],
                      ['--manager-profile', str(self.manager_profile), '--manager-profile', str(self.manager_profile)],
                      ['--managed-mode', 'standard'], ['--managed-mode=ultra']):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                self.launch(value, flags(value, True) + extra)
        for key in ('manager_profile', 'managed_phone', 'manager_lock_fd', 'managed_audio_muted'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                launcher.launch_settings(selectors(self.config, **{key: A}), [])

    def test_retired_mode_cli_is_rejected_before_execution(self):
        for extra in (['--managed-mode', 'standard'], ['--managed-mode=ultra']):
            with self.subTest(extra=extra):
                result = subprocess.run([sys.executable, '-B', str(self.sealed / 'tools/run_lab_receiver.py'),
                                         '--dry-run', *extra], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn('unrecognized arguments: --managed-mode', result.stderr)
                self.assertFalse(self.state.exists())

    def test_real_owned_lock_requires_shared_description_and_pinned_generation(self):
        self.state_directory()
        with ManagerEndpoint(self.state, self.owner) as endpoint:
            fd = endpoint.inherited_lock_fd
            self.assertEqual(launcher.verify_manager_lock(fd, self.state, self.owner[0]), fd)
            independent = os.open(self.state / 'manager.lock', os.O_RDWR)
            try:
                with self.assertRaisesRegex(ValueError, 'does not own'): launcher.verify_manager_lock(independent, self.state, self.owner[0])
            finally: os.close(independent)
            for bad in (None, True, 0, 2, -1):
                with self.subTest(bad=bad), self.assertRaises(ValueError): launcher.verify_manager_lock(bad, self.state, self.owner[0])
            old = self.state / 'old.lock'; (self.state / 'manager.lock').rename(old)
            (self.state / 'manager.lock').write_bytes(b''); (self.state / 'manager.lock').chmod(0o600)
            if os.geteuid() == 0: os.chown(self.state / 'manager.lock', *self.owner)
            with self.assertRaisesRegex(ValueError, 'exact private'): launcher.verify_manager_lock(fd, self.state, self.owner[0])
            (self.state / 'manager.lock').unlink(); old.rename(self.state / 'manager.lock')
        fd = os.open(self.state / 'manager.lock', os.O_RDWR)
        try:
            with self.assertRaisesRegex(ValueError, 'not held'): launcher.verify_manager_lock(fd, self.state, self.owner[0])
        finally: os.close(fd)

    def test_shared_capabilities_audio_and_phone_isolation_leave_sealed_base_unchanged(self):
        self.state_directory()
        with ManagerEndpoint(self.state, self.owner) as endpoint:
            original = self.config.read_bytes()
            for phone in (A, B):
                value = selectors(self.config, self.binding, manager_profile=self.manager_profile,
                                  managed_phone=phone, manager_lock_fd=endpoint.inherited_lock_fd,
                                  managed_audio_volume_percent=37, managed_audio_muted='true')
                settings, _ = self.launch(value, flags(value))
                self.assertEqual(settings['selected_phone'], phone)
                self.assertEqual(settings['auto_connect_phone'], phone)
                self.assertEqual(settings['pair_store'], str(self.external / 'pairs' / ('phone-' + phone.replace(':', ''))))
                self.assertEqual(settings['media_state'], str(self.state / 'media.json'))
                self.assertFalse(settings['direct_media'])
                self.assertEqual(settings['runtime_status_file'], self.state / 'carplay-status.json')
                self.assertEqual(settings['runtime_status_nonce'], NONCE)
                self.assertEqual(settings['audio_control'], self.state / 'audio-control.json')
                self.assertEqual(settings['audio_state'], self.state / 'audio-state.json')
                self.assertEqual((settings['audio_volume_percent'], settings['audio_muted']), (37, True))
                self.assertTrue(settings['ultra_negotiate']); self.assertTrue(settings['ultra_native_video'])
                self.assertTrue(settings['standard_cluster'])
                self.assertEqual(settings['display_config'], release.config_path(self.sealed, 'displays'))
                self.assertEqual(settings['standard_display_config'], str(self.standard))
                self.assertEqual(settings['preview_sink'], 'wayland-touch')
                self.assertEqual(settings['ultra_installed_ui'], release.config_path(self.sealed, 'ultra'))
            self.assertEqual(self.config.read_bytes(), original)

    def test_invalid_managed_selector_and_audio_preferences_do_not_launch(self):
        self.state_directory()
        with ManagerEndpoint(self.state, self.owner) as endpoint:
            original = dict(manager_profile=self.manager_profile, managed_phone=A,
                            manager_lock_fd=endpoint.inherited_lock_fd)
            for extra in ({'managed_phone': A.lower()}, {'managed_phone': A + ';cmd'},
                          {'managed_audio_volume_percent': -1, 'managed_audio_muted': 'false'},
                          {'managed_audio_volume_percent': 101, 'managed_audio_muted': 'true'},
                          {'managed_audio_volume_percent': 20}, {'managed_audio_muted': 'false'},
                          {'managed_audio_volume_percent': 20, 'managed_audio_muted': 'yes'},
                          {'managed_runtime_status_nonce': None}, {'managed_runtime_status_nonce': 'A' * 32},
                          {'managed_runtime_status_nonce': '0' * 31}, {'managed_runtime_status_nonce': '0' * 33}):
                value = selectors(self.config, self.binding, **(original | extra))
                with self.subTest(extra=extra), self.assertRaises(ValueError): self.launch(value, flags(value))
            _, base, profile = self.admit(self.config, self.manager_profile, self.binding)
            no_audio = dict(base, audio_sink='none')
            with self.assertRaises(ValueError): launcher.managed_receiver_settings(no_audio, profile, A, 20, False)
            for field in ('camera_manual_profile', 'standard_camera_control_file', 'standard_camera_lease_file'):
                with self.subTest(field=field), self.assertRaises(ValueError):
                    launcher.managed_receiver_settings(dict(base, **{field: '/private/camera'}), profile, A)

    def test_real_children_receive_static_argv_and_inherit_owner_until_stop(self):
        self.state_directory(); phones = Phones()
        real_run, real_popen = subprocess.run, subprocess.Popen
        checks, launched, results = [], [], []
        def child_command(command): return [sys.executable, '-B', '-I', '-c', CHILD, json.dumps(command), str(CHECKER)]
        def receiver_command(command): return any(str(v).endswith('/tools/run_lab_receiver.py') for v in command)
        def validate(command, **options):
            if not receiver_command(command): return real_run(command, **options)
            checks.append((list(command), dict(options)))
            result = real_run(child_command(command), **options)
            results.append(json.loads(result.stdout)); return result
        def launch(command, **options):
            if not receiver_command(command): return real_popen(command, **options)
            launched.append((list(command), dict(options)))
            process = real_popen(child_command(command), **options); self.children.append(process); return process
        def wait_for_admission(count):
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                lines = (self.state / 'receiver-manager.log').read_text().splitlines()
                if len(lines) >= count:
                    return [json.loads(line) for line in lines]
                self.assertIsNone(manager.child.poll(), 'Isolated child admission failed before its ready record')
                time.sleep(.01)
            self.fail('Isolated owned child did not complete static admission before its deadline')
        with ManagerEndpoint(self.state, self.owner) as endpoint, \
                patch.object(phone_manager, 'sealed_manager_configuration', self.admit):
            phone_manager.atomic_json(self.state / 'audio-preferences.json',
                                      {'version': 1, 'volumePercent': 37, 'muted': True}, self.owner)
            manager = phone_manager.Manager(self.config, self.state, self.standard, self.owner, bluez=phones,
                                            receiver_lock_fd=endpoint.inherited_lock_fd,
                                            release_binding=self.binding, manager_profile=self.manager_profile)
            # Opposite retired hints are preserved for rollback, but neither
            # can strip configured receiver capabilities or select a mode.
            manager.state.phones = {A: 'ultra', B: 'standard'}; manager.state.save()
            poison = self.state / 'managed-receiver.json'; poison.write_text('{"bin_dir":"/untrusted"}')
            try:
                with patch.object(phone_manager.subprocess, 'run', validate), patch.object(phone_manager.subprocess, 'Popen', launch):
                    from carplay_readiness import clock_ns
                    def confirm(mode):
                        # Genuine host date + current real child's launch nonce;
                        # negotiated mode/readiness content is explicitly synthetic.
                        phone_manager.atomic_json(manager.carplay.path, {
                            'version': 2, 'nonce': manager.carplay.nonce, 'sequence': '1',
                            'connectionGeneration': '1', 'updatedMonotonicNs': str(clock_ns()),
                            'state': 'ready', 'returnReady': True, 'negotiatedMode': mode}, self.owner)
                        return manager.request({'action': 'status'})
                    manager.start(A)
                    wait_for_admission(1)
                    self.assertIsNone(manager.request({'action': 'status'})['active_mode'])
                    self.assertEqual(confirm('standard')['active_mode'], 'standard')
                    manager.start(B)
                    self.assertEqual(self.children[0].returncode, 0)
                    admitted = wait_for_admission(2)
                    new_status = manager.request({'action': 'status'})
                    self.assertIsNone(new_status['active_mode'])
                    self.assertFalse(new_status['carplay']['returnReady'])
                    self.assertEqual(confirm('ultra')['active_mode'], 'ultra')
                self.assertEqual(manager.active, B); self.assertEqual(manager.active_mode, 'ultra')
                self.assertEqual(phones.disconnected, [A, B])
                self.assertEqual(poison.read_text(), '{"bin_dir":"/untrusted"}')
                for command, options in checks + launched:
                    self.assertEqual(command[:5], ['/usr/bin/python3', '-B', '-E', '-s', '-u'])
                    self.assertEqual(command[5], str(self.sealed / 'tools/run_lab_receiver.py'))
                    self.assertEqual(command[command.index('--config') + 1], str(self.config))
                    self.assertEqual(command[command.index('--manager-profile') + 1], str(self.manager_profile))
                    self.assertEqual(options['pass_fds'], (endpoint.inherited_lock_fd,))
                    self.assertNotIn(str(poison), command)
                    self.assertNotIn('--managed-mode', command)
                self.assertEqual(results[0]['settings']['selected_phone'], A)
                self.assertEqual(results[1]['settings']['selected_phone'], B)
                self.assertEqual([v['settings']['selected_phone'] for v in admitted], [A, B])
                self.assertTrue(all((v['settings']['audio_volume_percent'], v['settings']['audio_muted']) == (37, True) for v in results))
                for value in results + admitted:
                    self.assertTrue(value['settings']['ultra_negotiate'])
                    self.assertTrue(value['settings']['standard_cluster'])
                    self.assertEqual(value['settings']['standard_display_config'], str(self.standard))
                    self.assertEqual(value['settings']['ultra_installed_ui'], str(release.config_path(self.sealed, 'ultra')))
                self.assertEqual(manager.state.phones, {A: 'ultra', B: 'standard'})
                with self.assertRaises(ValueError): ManagerEndpoint(self.state, self.owner)
            finally: manager.shutdown()
        with ManagerEndpoint(self.state, self.owner): pass

    def test_rejected_dry_run_preserves_existing_receiver_and_selection(self):
        self.state_directory(); phones = Phones()
        class Active:
            stopped = False
            def poll(self): return None
            def terminate(self): self.stopped = True
            def wait(self, timeout): return 0
        with ManagerEndpoint(self.state, self.owner) as endpoint, patch.object(phone_manager, 'sealed_manager_configuration', self.admit):
            manager = phone_manager.Manager(self.config, self.state, self.standard, self.owner, bluez=phones,
                                            receiver_lock_fd=endpoint.inherited_lock_fd,
                                            release_binding=self.binding, manager_profile=self.manager_profile)
            manager.state.select(A); old = Active(); manager.child = old
            try:
                with patch.object(phone_manager.subprocess, 'run', side_effect=subprocess.CalledProcessError(2, 'preflight')), \
                        patch.object(phone_manager.subprocess, 'Popen') as start:
                    with self.assertRaises(subprocess.CalledProcessError): manager.start(B)
                    start.assert_not_called()
                self.assertIs(manager.child, old); self.assertFalse(old.stopped)
                self.assertEqual(manager.state.selected, A); self.assertEqual(phones.disconnected, [])
                self.assertFalse((self.state / 'managed-receiver.json').exists())
            finally: manager.shutdown()

    def test_actual_launcher_owner_survives_manager_close_then_sigkill_releases(self):
        self.state_directory()
        with ManagerEndpoint(self.state, self.owner) as endpoint:
            value = selectors(self.config, self.binding, manager_profile=self.manager_profile,
                              managed_phone=A, manager_lock_fd=endpoint.inherited_lock_fd)
            command = ['/usr/bin/python3', '-B', '-E', '-s', '-u', str(self.sealed / 'tools/run_lab_receiver.py'), *flags(value)]
            child = subprocess.Popen([sys.executable, '-B', '-I', '-c', CHILD, json.dumps(command), str(CHECKER)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     pass_fds=(endpoint.inherited_lock_fd,))
            self.children.append(child)
            self.assertTrue(select.select([child.stdout], [], [], 3)[0], 'Inherited launcher admission exceeded its bound')
            ready = child.stdout.readline(16384)
            self.assertTrue(ready.endswith(b'\n'))
            self.assertEqual(json.loads(ready)['lock'], (self.state / 'manager.lock').stat().st_ino)
        # Parent endpoint is retired; the production-validated launcher FD is
        # the only remaining owner. An independent manager cannot take over.
        with self.assertRaises(ValueError): ManagerEndpoint(self.state, self.owner)
        child.kill(); child.wait(timeout=3)
        with ManagerEndpoint(self.state, self.owner): pass

    def test_manager_bound_validate_only_uses_actual_admission_without_state_or_radio(self):
        code = r'''
import functools,pathlib,sys
root,checker=sys.argv[1:3]; sys.path.insert(0,str(pathlib.Path(root)/'tools'))
import phone_manager
phone_manager.sealed_manager_configuration=functools.partial(phone_manager.sealed_manager_configuration,checker=pathlib.Path(checker))
def forbidden(*args,**kwargs): raise AssertionError('Read-only admission crossed into manager state or radio')
phone_manager.prepare_directory=forbidden; phone_manager.Manager=forbidden; phone_manager.BluezPhones=forbidden
sys.argv=['phone_manager.py',*sys.argv[3:]]
phone_manager.main()
'''
        args = ['--config', str(self.config), '--manager-profile', str(self.manager_profile),
                '--release-root', self.binding['path'], '--release-manifest-sha256', self.binding['sha256'], '--validate-only']
        def invoke(extra):
            return subprocess.run([sys.executable, '-B', '-I', '-c', code, str(self.sealed), str(CHECKER), *extra],
                                  capture_output=True, text=True, timeout=5)
        result = invoke(args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {'configurationValid': True, 'enabled': True, 'autoConnectSelected': False})
        for extra in (['--uid', str(self.owner[0] + 1)], ['--directory', str(self.state)],
                      ['--config', str(self.config)], ['--vehicle-fanout-config', str(self.source / 'unbound.json')]):
            with self.subTest(extra=extra):
                failed = invoke(args + extra)
                self.assertNotEqual(failed.returncode, 0)
                self.assertNotIn('Read-only admission crossed', failed.stderr)
        self.assertFalse(self.state.exists())

    def test_version_one_standalone_and_unsealed_configuration_remain_usable(self):
        roles = self.roles - {'phone_manager', 'standard_displays'}
        recipe = {'version': 1, 'release_id': 'legacy-one', 'source_revision': 'b' * 40,
                  'repo': str(self.repo), 'user': self.account.pw_name,
                  'configuration': {role: str(self.source / release.ROLES[role][0]) for role in roles}, 'native_camera': None}
        binding = release.stage(recipe, self.pool)
        config = release.config_path(Path(binding['path']), 'receiver')
        value = selectors(config, binding)
        for dry in (False, True):
            settings, selected = launcher.launch_settings(value, flags(value, dry))
            self.assertEqual(selected, binding)
            self.assertEqual(settings['bin_dir'], Path(binding['path']) / 'build')
        for key, item in (('manager_profile', self.manager_profile), ('managed_phone', A)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                launcher.launch_settings(selectors(config, binding, **{key: item}), flags(value) + ['--' + key.replace('_', '-'), str(item)])
        settings, binding = launcher.launch_settings(selectors(self.source / 'receiver.json'), [])
        self.assertIsNone(binding); self.assertEqual(settings['bin_dir'], self.repo / 'build')
        self.assertFalse(self.state.exists())


if __name__ == '__main__': unittest.main()
