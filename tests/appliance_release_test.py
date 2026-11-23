# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Local release/file and transaction tests; no Linux binaries or radios start."""
import copy
import errno
from contextlib import contextmanager
import json
import os
from pathlib import Path
import pwd
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / 'build/headunit-display-config-check'
sys.path.insert(0, str(ROOT / 'tools'))
import appliance_release as release
import appliance_transaction as transaction
from appliance_transaction_test import System, PowerLoss, body
from install_appliance_release import prepare
from receiver_configuration import load_receiver_configuration, parse_receiver_configuration
from run_lab_receiver import private_file, python_helper_command, receiver_owner
from run_lab_receiver import (managed_receiver_settings, native_page_environment,
                              native_page_ui, sealed_manager_configuration)
from vehicle_fanout import load_configuration


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.repo = self.root / 'repo'
        for directory in ('tools', 'build', 'deploy/cm5'):
            (self.repo / directory).mkdir(parents=True)
        for name in release.TOOLS:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        for name in release.UNIT_NAMES:
            shutil.copyfile(ROOT / 'deploy/cm5' / ('headunit-' + name + '.service.in'),
                            self.repo / 'deploy/cm5' / ('headunit-' + name + '.service.in'))
        # Explicit architecture/header fixture only, never executed. Production
        # preflight uses the actual staged executable; this test substitutes the
        # real portable display parser for this non-runnable ELF fixture.
        elf = bytearray(64); elf[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HHI', elf, 16, 3, 183, 1)
        for name in (*release.CORE_BINARIES, 'headunit-vehicle-config-check', 'headunit-camera-wayland', 'headunit-installed-ui-probe'):
            path = self.repo / 'build' / name; path.write_bytes(elf); path.chmod(0o755)
        self.source = self.root / 'source'; self.source.mkdir(mode=0o700)
        self.external = self.root / 'state'; self.external.mkdir(mode=0o700)
        self.account = pwd.getpwuid(os.getuid())
        if self.account.pw_uid == 0:
            # Library publication fixtures also work in root-run Linux CI;
            # production display processes must still be unprivileged.
            candidates = [account for account in pwd.getpwall() if 0 < account.pw_uid < 65536]
            if not candidates: self.skipTest('No unprivileged account for sealed release fixture')
            self.account = candidates[0]
            os.chown(self.external, self.account.pw_uid, self.account.pw_gid)
        self.pool = self.root / 'releases'; self.pool.mkdir(mode=0o755)
        self.units = self.root / 'units'; self.units.mkdir(mode=0o700)
        self.journals = self.root / 'transactions'
        self.system = System(self.units)
        self.settings = {
            'display_config': str(self.source / 'displays.conf'),
            'bin_dir': str(self.repo / 'build'), 'seconds': 0,
            'preview_sink': 'wayland-touch', 'simulate_vehicle': True,
            'wifi_config': str(self.external / 'wifi.conf'),
            'identity': str(self.external / 'identity.conf'),
            'pairing_key': str(self.external / 'pairing.pem'),
            'setup_secret': str(self.external / 'setup-secret'),
            'pair_store': str(self.external / 'pairs'),
            'capture_root': str(self.external / 'captures'),
            'media_state': str(self.external / 'media.json'),
            'cluster_page': str(self.external / 'page.txt'),
        }
        for name in ('wifi.conf', 'identity.conf', 'pairing.pem', 'setup-secret'):
            (self.external / name).write_text('private fixture, never copied\n')
            (self.external / name).chmod(0o600)
        displays = ('main.width=1280\nmain.height=720\nmain.fps=60\nmain.panel=Main\n'
                    'main.wayland=wayland-0\nmain.output=HDMI-A-1\n'
                    'cluster.width=1920\ncluster.height=720\ncluster.fps=30\ncluster.panel=Cluster\n'
                    'cluster.wayland=wayland-0\ncluster.output=HDMI-A-2\n')
        (self.source / 'displays.conf').write_text(displays)
        for role, name in (('labwc', 'labwc-cm5-dual-rc.xml'), ('outputs', 'kanshi-cm5-dual.conf')):
            shutil.copyfile(ROOT / 'config' / name, self.source / release.ROLES[role][0])
        self.recipe = {'version': 1, 'release_id': 'release-one', 'source_revision': 'a' * 40,
                       'repo': str(self.repo), 'user': self.account.pw_name,
                       'configuration': {role: str(self.source / release.ROLES[role][0])
                                         for role in release.REQUIRED_ROLES}, 'native_camera': None}
        self.write_receiver()
        self.validations = []

    def tearDown(self):
        for directory, _, _ in os.walk(self.root): os.chmod(directory, 0o700)
        self.temporary.cleanup()

    def write_receiver(self):
        path = self.source / 'receiver.json'; path.write_text(json.dumps(self.settings)); path.chmod(0o600)

    def test_version_four_application_pins_independent_boot_helper_and_survives_legacy_rollback(self):
        self.managed()
        helpers = self.root / 'helpers'; helpers.mkdir()
        helper = release.stage({'version': 4, 'purpose': 'boot-recovery', 'boot_recovery': None,
            'execution': None, 'release_id': 'boot-helper-one', 'source_revision': 'a'*40,
            'repo': str(ROOT), 'user': self.account.pw_name, 'configuration': {}, 'native_camera': None}, helpers)
        # Legacy generation retains its exact payload/guards before V4 admission.
        legacy = self.publish('legacy-release')
        with self.portable_checkers():
            old = prepare(legacy, self.dry_run, self.units)
        with self.portable_checkers():
            transaction.install(old, self.units, self.journals, self.system)
            self.recipe.update(version=4, purpose='appliance', boot_recovery={
                'binding': helper, 'transaction_directory': str(self.journals)})
            self.recipe['execution']['mode_policy'] = 'phone-authoritative'
            shutil.copyfile(ROOT / 'tools/appliance_boot.py', self.repo / 'tools/appliance_boot.py')
            candidate = self.publish('candidate-release')
            new = prepare(candidate, self.dry_run, self.units)
            from install_appliance_release import select
            from appliance_boot import generator_path, verify_installed
            snapshot = select(new, candidate, self.units, self.journals, self.system)
            verify_installed(self.recipe['boot_recovery'], self.units)
            release.verify_units(candidate, self.units)
            retained = generator_path(self.units).read_bytes()
            transaction.rollback(snapshot, self.units, self.journals, self.system)
            self.assertEqual(generator_path(self.units).read_bytes(), retained)
            release.verify_units(legacy, self.units)
            self.assertTrue(all((self.units / name).read_text() == text for name, text in old.items()))
        self.assertFalse(any('start' in call or 'restart' in call or 'stop' in call for call in self.system.calls))
        # An application-pool helper cannot satisfy independent retention.
        self.recipe['boot_recovery']['binding'] = candidate
        with self.assertRaisesRegex(ValueError, 'independently retained'):
            self.publish('wrong-helper-release')

    def camera_graph(self):
        self.ultra()
        capture = json.loads((ROOT / 'config/camera-ultra-usb.example.json').read_text())
        capture['enabled'] = True
        camera = self.add_role('camera', json.dumps(capture).encode())
        state = self.add_role('camera_state', (ROOT / 'config/camera-state.example.json').read_bytes())
        manual = json.loads((ROOT / 'config/camera-manual.example.json').read_text())
        manual['enabled'] = True
        for key, name in {'requestFile': 'manual.request', 'ownerFile': 'manual.owner',
                          'statusFile': 'signed.status', 'controlSocket': 'manual.sock'}.items():
            manual[key] = str(self.external / 'camera' / name)
        manual_path = self.add_role('camera_manual', json.dumps(manual).encode())
        self.settings.update(camera_state_config=str(state), camera_manual_profile=str(manual_path),
                             vehicle_state_backend='simulation')
        self.write_receiver()
        with Path(self.recipe['configuration']['ultra']).open('a') as stream:
            stream.write('cameraProfile=' + str(camera) + '\ncameraControlFile=' + manual['requestFile'] + '\ncameraStatusFile=' + manual['statusFile'] + '\n')

    def boot_candidate(self, version, name):
        helpers = self.root / 'helpers'
        helpers.mkdir(exist_ok=True)
        helper = release.stage({'version': version, 'purpose': 'boot-recovery', 'boot_recovery': None,
            'execution': None, 'release_id': 'helper-' + name, 'source_revision': 'a'*40,
            'repo': str(ROOT), 'user': self.account.pw_name, 'configuration': {}, 'native_camera': None}, helpers)
        self.recipe.update(version=version, purpose='appliance', boot_recovery={
            'binding': helper, 'transaction_directory': str(self.journals)})
        self.recipe['execution']['mode_policy'] = 'phone-authoritative'
        for tool in ('appliance_boot.py', 'punchthrough_configuration.py'):
            shutil.copyfile(ROOT / 'tools' / tool, self.repo / 'tools' / tool)
        candidate = self.publish(name)
        return candidate, self.bundle(candidate), copy.deepcopy(self.recipe['boot_recovery'])

    def test_version_five_camera_graph_is_closed_and_requires_explicit_cold_migration(self):
        from install_appliance_release import select
        from appliance_boot import verify_installed, generator_path
        self.managed(resident=True)
        old, old_units, old_boot = self.boot_candidate(4, 'previous-v4')
        with self.portable_checkers():
            select(old_units, old, self.units, self.journals, self.system)
        old_generator = generator_path(self.units).read_bytes()
        self.camera_graph()
        new, new_units, new_boot = self.boot_candidate(5, 'candidate-v5')
        captured = release.verify(new)
        self.assertTrue(release.CAMERA_ROLES <= set(captured['configuration']))
        sealed = load_receiver_configuration(Path(new['path']) / 'config/receiver.json')
        self.assertEqual(sealed['camera_state_config'], Path(new['path']) / 'config/camera-state.json')
        for refused in ('implicit', 'live-owner', 'live-gate', 'queued-job'):
            with self.subTest(refused=refused), self.portable_checkers():
                self.system.active['headunit-receiver.service'] = 'active' if refused == 'live-owner' else 'inactive'
                self.system.gate['ActiveState'] = 'active' if refused == 'live-gate' else 'inactive'
                self.system.jobs = {'headunit-receiver.service': '1'} if refused == 'queued-job' else {}
                with self.assertRaises(ValueError):
                    select(new_units, new, self.units, self.journals, self.system,
                           migrate_boot_helper=refused != 'implicit')
                self.assertEqual(generator_path(self.units).read_bytes(), old_generator)
                release.verify_units(old, self.units)
        self.system.active.clear(); self.system.jobs.clear(); self.system.gate['ActiveState'] = 'inactive'
        with self.portable_checkers():
            snapshot = select(new_units, new, self.units, self.journals, self.system, migrate_boot_helper=True)
            verify_installed(new_boot, self.units)
            release.verify_units(new, self.units)
            transaction.rollback(snapshot, self.units, self.journals, self.system)
            verify_installed(old_boot, self.units)
            self.assertEqual(generator_path(self.units).read_bytes(), old_generator)
            release.verify_units(old, self.units)
        self.assertFalse(any(call[1] in ('start', 'stop', 'restart') for call in self.system.calls if call[0] == 'systemctl'))

    def test_version_five_interrupted_bootstrap_before_journal_aligns_to_current_units(self):
        from install_appliance_release import select
        from appliance_boot import admit, align_boot_with_units, verify_installed
        self.managed()
        old, old_units, old_boot = self.boot_candidate(4, 'previous-v4')
        with self.portable_checkers():
            select(old_units, old, self.units, self.journals, self.system)
        new, new_units, new_boot = self.boot_candidate(5, 'candidate-v5')
        with self.portable_checkers(), transaction.Transaction(self.units, self.journals, self.system) as owner:
            admit(new_boot, owner, migrate=True)  # Durable bootstrap, no candidate journal yet.
            verify_installed(new_boot, self.units)
            align_boot_with_units(owner)
            verify_installed(old_boot, self.units)
            release.verify_units(old, self.units)
        self.system.fail = lambda args: args == ['systemctl', 'enable', 'headunit-receiver.service']
        with self.portable_checkers(), self.assertRaises(RuntimeError):
            select(new_units, new, self.units, self.journals, self.system, migrate_boot_helper=True)
        verify_installed(old_boot, self.units)
        with self.portable_checkers():
            release.verify_units(old, self.units)

    def test_version_five_camera_rejects_older_helper_before_publication(self):
        self.managed()
        retained, _, _ = self.boot_candidate(4, 'retained-v4')
        self.camera_graph()
        self.recipe['version'] = 5
        before = set(self.pool.iterdir())
        with self.assertRaisesRegex(ValueError, 'recovery helper schema'):
            self.publish('refused-v5')
        self.assertEqual(set(self.pool.iterdir()), before)
        self.assertEqual(release.verify(retained)['version'], 4)
        compatible, units, _ = self.boot_candidate(5, 'compatible-v5')
        self.assertEqual(release.verify(compatible)['version'], 5)
        self.assertEqual(release.unit_binding(units, self.units), compatible)

    def test_retained_incompatible_helper_verifies_but_cannot_prepare(self):
        self.managed()
        self.boot_candidate(4, 'retained-v4')
        self.camera_graph()
        self.recipe['version'] = 5
        # Explicit historical fixture models an application sealed before the
        # publication repair. Integrity verification remains diagnostic evidence.
        with patch.object(release, 'require_compatible_boot_helper'):
            retained = self.publish('retained-incompatible-v5')
        self.assertEqual(release.verify(retained)['version'], 5)
        before = len(self.validations)
        with self.assertRaisesRegex(ValueError, 'recovery helper schema'):
            self.bundle(retained)
        self.assertEqual(len(self.validations), before)
        with patch.object(release, 'require_compatible_boot_helper'), \
                patch('install_appliance_release.require_compatible_boot_helper'):
            units = self.bundle(retained)
        with self.assertRaisesRegex(ValueError, 'recovery helper schema'):
            release.verify_unit_bundle(retained, units, self.units)
        self.assertEqual(list(self.units.iterdir()), [])
        self.assertEqual(self.system.calls, [])

    def test_version_five_rejects_partial_or_conflicting_camera_graph_before_publication(self):
        self.managed()
        self.camera_graph()
        self.recipe['version'] = 5
        self.recipe['purpose'] = 'appliance'
        self.recipe['boot_recovery'] = {'binding': {'path': str(self.root / 'helpers/helper'), 'sha256': 'a'*64},
                                        'transaction_directory': str(self.journals)}
        self.recipe['execution']['mode_policy'] = 'phone-authoritative'
        for role in release.CAMERA_ROLES:
            altered = copy.deepcopy(self.recipe)
            del altered['configuration'][role]
            with self.subTest(role=role), self.assertRaisesRegex(ValueError, 'complete sealed'):
                release.validate_recipe(altered)
        from punchthrough_configuration import validate_graph
        settings = parse_receiver_configuration((self.source / 'receiver.json').read_bytes(), self.source / 'receiver.json')
        fields = release.ui_fields(Path(self.recipe['configuration']['ultra']).read_bytes())
        raw = {role: Path(self.recipe['configuration'][role]).read_bytes() for role in release.CAMERA_ROLES}
        validate_graph(settings, fields, raw['camera'], raw['camera_state'], raw['camera_manual'])
        for change in ('native-owner', 'different-endpoint', 'writable-state', 'stale-state', 'malformed-state'):
            bad_fields = copy.deepcopy(fields)
            bad_state = json.loads(raw['camera_state'])
            if change == 'native-owner': bad_fields['nativeCameraControlFile'] = str(self.external / 'other')
            if change == 'different-endpoint': bad_fields['cameraControlFile'] = str(self.external / 'other')
            if change == 'writable-state': bad_state['general']['on']['writable'] = True
            if change == 'stale-state': bad_state['general']['on']['max_age_ms'] = 150
            if change == 'malformed-state': bad_state['general'] = []
            with self.subTest(change=change), self.assertRaises(ValueError):
                validate_graph(settings, bad_fields, raw['camera'], json.dumps(bad_state).encode(), raw['camera_manual'])

    def test_launch_readiness_identity_cannot_be_sealed_as_static_configuration(self):
        self.settings.update(runtime_status_file=str(self.external / 'carplay-status.json'),
                             runtime_status_nonce='a'*32)
        self.write_receiver()
        with self.assertRaisesRegex(ValueError, 'derived per launch'):
            release.stage(self.recipe, self.pool)

    def add_role(self, role, data):
        path = self.source / release.ROLES[role][0]
        path.write_bytes(data); path.chmod(0o600)
        self.recipe['configuration'][role] = str(path)
        return path

    def ultra(self):
        profile = self.add_role('vehicle_profile', b'private original local profile fixture\n')
        ui = self.add_role('ultra', ('assetDirectory=' + str(self.external / 'assets') +
                                   '\nassetIdentifier=fixture\nassetVersion=1\nvehicleProfile=' + str(profile) +
                                   '\nacceptanceReference=' + str(self.external / 'accepted.conf') +
                                   '\nclusterLayoutFile=' + str(self.external / 'layout.conf') + '\n').encode())
        self.settings |= {'ultra_installed_ui': str(ui), 'ultra_negotiate': True, 'ultra_native_video': True}
        self.write_receiver()
        return ui

    def managed(self, resident=False):
        self.recipe['version'] = 3
        self.recipe['execution'] = {'receiver': 'phone-manager', 'resident_home': resident}
        standard = self.add_role('standard_displays', (self.source / 'displays.conf').read_bytes())
        self.add_role('phone_manager', (json.dumps({'schemaVersion': 1, 'enabled': True,
            'stateDirectory': str(self.external), 'standardDisplayConfig': str(standard),
            'autoConnectSelected': False, 'pairing': {'enabled': True, 'adapter': 'hci0'}}) + '\n').encode())
        if resident:
            self.settings.update(preview_sink='wayland-gpu', native_page_control_file=str(self.external / 'home-surface.request'),
                                 native_page_lease_file=str(self.external / 'home-surface.lease'))
            self.write_receiver()
            self.add_role('phone_page', (json.dumps({'schemaVersion': 2, 'enabled': True,
                'managerSocket': str(self.external / 'manager.sock'), 'waylandDisplay': 'wayland-0',
                'surfaceControlFile': str(self.external / 'home-surface.request'),
                'surfaceLeaseFile': str(self.external / 'home-surface.lease'),
                'output': 'HDMI-A-1', 'homeButton': {'corner': 'bottom-right', 'margin': 16, 'size': 64}}) + '\n').encode())
        for name in release.MANAGER_TOOLS + (release.RESIDENT_TOOLS if resident else ()):
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        for name in ('managed-receiver',) + (('phone-page',) if resident else ()):
            filename = 'headunit-' + name + '.service.in'
            shutil.copyfile(ROOT / 'deploy/cm5' / filename, self.repo / 'deploy/cm5' / filename)

    def publish(self, name='release-one'):
        recipe = copy.deepcopy(self.recipe); recipe['release_id'] = name
        return release.stage(recipe, self.pool)

    def dry_run(self, command, **options):
        self.validations.append(command)
        if len(command) > 4 and Path(command[4]).name == 'phone_manager.py':
            self.assertIn('--validate-only', command)
            self.assertIn('--release-root', command)
            return subprocess.CompletedProcess(command, 0, stdout=json.dumps({
                'configurationValid': True, 'enabled': True, 'autoConnectSelected': False}))
        if len(command) > 4 and Path(command[4]).name == 'phone_page_boot.py':
            self.assertIn('--validate-only', command)
            self.assertIn('--release-root', command)
            return subprocess.CompletedProcess(command, 0, stdout=json.dumps({
                'configurationValid': True, 'enabled': True, 'waylandDisplay': 'wayland-0', 'output': 'HDMI-A-1'}))
        if Path(command[0]).name == 'headunit-installed-ui-probe':
            self.assertEqual(command[2], '--validate-only')
            self.assertTrue(Path(command[0]).is_relative_to(self.pool))
            return subprocess.CompletedProcess(command, 0, stdout=json.dumps({
                'configurationValid': True, 'assetVersion': 1, 'expiredSigningLeafExceptionUsed': False,
                'scriptsExecuted': False, 'phoneConnected': False}))
        self.assertIn('--dry-run', command)
        self.assertTrue(Path(command[4]).is_relative_to(self.pool))
        self.assertEqual(options['timeout'], 30)
        return subprocess.CompletedProcess(command, 0, stdout='fixture dry-run accepted')

    @contextmanager
    def portable_checkers(self):
        real_run = subprocess.run
        def run(command, **options):
            self.validations.append(command)
            if Path(command[0]).name == 'headunit-camera-wayland':
                return subprocess.CompletedProcess(command, 0, stdout=json.dumps({
                    'configurationValid': True, 'enabled': True, 'wayland': 'wayland-0',
                    'output': 'HDMI-A-1', 'width': 1280, 'height': 720}))
            self.assertTrue(Path(command[0]).is_relative_to(self.pool))
            return real_run([str(CHECKER), *command[1:]], **options)
        with patch('install_cm5_appliance.subprocess.run', run):
            yield

    def bundle(self, binding):
        with self.portable_checkers():
            return prepare(binding, self.dry_run, self.units)

    def test_release_copies_only_programs_and_static_configuration(self):
        binding = self.publish(); value = release.verify(binding); root = Path(binding['path'])
        self.assertEqual(value['configuration'], sorted(release.REQUIRED_ROLES))
        settings = load_receiver_configuration(root / 'config/receiver.json')
        self.assertEqual(settings['bin_dir'], root / 'build')
        self.assertEqual(settings['display_config'], root / 'config/displays.conf')
        self.assertEqual(settings['identity'], self.external / 'identity.conf')
        self.assertEqual(settings['media_state'], self.external / 'media.json')
        self.assertFalse((root / 'current').exists())
        self.assertTrue(all('private fixture' not in path.read_text(errors='replace')
                            for path in root.rglob('*') if path.is_file()))
        units = self.bundle(binding)
        self.assertEqual(release.unit_binding(units), binding)
        self.assertIn('-B -E -s', units['headunit-receiver.service'])
        self.assertIn('--release-root ' + str(root), units['headunit-receiver.service'])
        for name, body in units.items(): (self.units / name).write_text(body)
        release.verify_units(binding, self.units)
        self.assertFalse((root / 'tools/__pycache__').exists())
        subprocess.run([sys.executable, '-B', '-I', '-c',
                        'import sys; from pathlib import Path; root=Path(sys.argv[1]); '
                        'sys.path.insert(0,str(root/"tools")); '
                        'import media_bridge,bluez_phones,carplay_reconnect,run_lab_receiver; '
                        'assert all(Path(m.__file__).is_relative_to(root) for m in '
                        '(media_bridge,bluez_phones,carplay_reconnect,run_lab_receiver))', str(root)],
                       check=True, capture_output=True, timeout=5)
        release.verify(binding)

    def test_ultra_and_capability_references_relocate_without_copying_assets(self):
        self.ultra()
        climate = self.add_role('climate', b'{"fixture":true}')
        self.settings |= {'climate_config': str(climate), 'vehicle_state_backend': 'simulation'}
        self.write_receiver()
        binding = self.publish(); root = Path(binding['path'])
        fields = release.ui_fields((root / 'config/installed-ui.conf').read_bytes())
        self.assertEqual(fields['vehicleProfile'], str(root / 'config/vehicle-profile.plist'))
        self.assertEqual(fields['assetDirectory'], str(self.external / 'assets'))
        self.assertNotIn('assets', [record['path'] for record in release.verify(binding)['files']])
        with self.assertRaises(ValueError): private_file(root / 'config/installed-ui.conf', os.getuid())
        private_file(root / 'config/installed-ui.conf', os.getuid(), binding, 'ultra')
        with self.assertRaises(ValueError): private_file(root / 'config/displays.conf', os.getuid(), binding, 'ultra')

    def test_local_gauges_require_exact_bound_profile_and_receiver_opt_in(self):
        self.assert_local_gauges_bound_to_receiver()

    def test_managed_local_gauges_require_exact_bound_profile_and_receiver_opt_in(self):
        self.managed()
        self.assert_local_gauges_bound_to_receiver()

    def assert_local_gauges_bound_to_receiver(self):
        self.ultra()
        self.settings['pre_phone_gauges'] = True
        self.write_receiver()
        binding = self.publish()
        root = Path(binding['path'])
        units = self.bundle(binding)
        name = 'headunit-cluster.service'
        profile = ' --local-ultra-profile ' + str(release.config_path(root, 'ultra'))
        self.assertIn(profile, units[name])
        self.assertEqual(release.unit_binding(units, self.units), binding)
        with self.portable_checkers():
            self.assertEqual(release.verify_unit_bundle(binding, units, self.units), binding)
        for replacement in (profile + '.foreign',
                            ' --local-ultra-profile ' + str(release.config_path(root, 'receiver')),
                            ' --local-ultra-profile /tmp/foreign/installed-ui.conf',
                            profile + profile,
                            ' --local-ultra-profile'):
            with self.subTest(replacement=replacement):
                altered = units | {name: units[name].replace(profile, replacement)}
                with self.assertRaises(ValueError):
                    release.unit_binding(altered, self.units)
        missing = units | {name: units[name].replace(profile, '')}
        self.assertEqual(release.unit_binding(missing, self.units), binding)
        with self.assertRaises(ValueError):
            release.verify_unit_bundle(binding, missing, self.units)
        self.settings['pre_phone_gauges'] = False
        self.write_receiver()
        disabled = self.publish('local-gauges-disabled')
        disabled_units = self.bundle(disabled)
        disabled_profile = ' --local-ultra-profile ' + str(
            release.config_path(Path(disabled['path']), 'ultra'))
        self.assertNotIn('--local-ultra-profile', disabled_units[name])
        altered = disabled_units | {name: disabled_units[name].replace(
            '--background', '--background' + disabled_profile)}
        self.assertEqual(release.unit_binding(altered, self.units), disabled)
        with self.assertRaises(ValueError):
            release.verify_unit_bundle(disabled, altered, self.units)

    def test_native_gauge_configuration_is_sealed_relocated_and_required(self):
        self.managed()
        native = self.add_role('native_cluster', b'version=1\nspeed_unit=mph\n')
        self.settings['native_cluster_config'] = str(native)
        self.write_receiver()
        binding = self.publish()
        root = Path(binding['path'])
        settings = load_receiver_configuration(root / 'config/receiver.json')
        self.assertEqual(settings['native_cluster_config'], root / 'config/native-cluster.conf')
        self.assertIn('build/headunit-vehicle-config-check', [record['path'] for record in release.verify(binding)['files']])
        private_file(settings['native_cluster_config'], os.getuid(), binding, 'native_cluster')
        with self.assertRaises(ValueError):
            private_file(native, os.getuid(), binding, 'native_cluster')
        recipe = copy.deepcopy(self.recipe)
        recipe['configuration'].pop('native_cluster')
        with self.assertRaisesRegex(ValueError, 'release role'):
            release.relocate(recipe, {role: Path(path).read_bytes() for role, path in recipe['configuration'].items()},
                             self.pool / 'unused-generation', self.pool)

    def test_manual_camera_profile_is_not_silently_external_or_started(self):
        # The retained receiver unit does not run the manual owner. Reject both
        # enabled and disabled profiles instead of promising unavailable UI.
        profile = self.external / 'manual-camera.json'
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                profile.write_text(json.dumps({'enabled': enabled}))
                profile.chmod(0o600)
                self.settings['camera_manual_profile'] = str(profile)
                self.write_receiver()
                with self.assertRaisesRegex(ValueError, 'native phone manager'):
                    self.publish()
                self.assertEqual(list(self.pool.iterdir()), [self.pool / '.release.lock'])

    def test_ultra_camera_state_cannot_escape_the_sealed_configuration_graph(self):
        profile = self.external / 'camera-state.json'
        profile.write_text('{}'); profile.chmod(0o600)
        self.settings.update(camera_state_config=str(profile), ultra_negotiate=True,
                             ultra_native_video=True, ultra_installed_ui=str(self.source/'ui.conf'),
                             vehicle_state_backend='simulation')
        self.write_receiver()
        with self.assertRaisesRegex(ValueError, 'sealed camera-owner graph'):
            self.publish()
        self.assertEqual(list(self.pool.iterdir()), [self.pool / '.release.lock'])

    def test_native_manager_and_page_remain_outside_the_sealed_graph(self):
        for role in ('phone_manager','phone_page'):
            with self.subTest(role=role):
                recipe=copy.deepcopy(self.recipe)
                recipe['configuration'][role]=str(self.source/'receiver.json')
                with self.assertRaisesRegex(ValueError,'Invalid versioned application recipe'):
                    release.stage(recipe,self.pool)
                self.assertEqual(list(self.pool.iterdir()),[])
        binding=self.publish();root=Path(binding['path'])
        names={record['path'] for record in release.verify(binding)['files']}
        self.assertNotIn('deploy/cm5/headunit-phone-page.service.in',names)
        self.assertNotIn('tools/phone_page_boot.py',names)
        self.assertNotIn('tools/phone_manager.py',names)
        units=self.bundle(binding)
        altered=dict(units)
        altered['headunit-receiver.service']=altered['headunit-receiver.service'].replace(
            'ExecStart=/usr/bin/python3 -B -E -s '+str(root/'tools/run_lab_receiver.py'),
            'ExecStart=/usr/bin/python3 -B -E -s '+str(root/'tools/phone_manager.py'))
        self.assertNotEqual(altered,units)
        with self.assertRaises(ValueError):release.unit_binding(altered,self.units)
        page='headunit-phone-page.service'
        with self.assertRaisesRegex(ValueError,'Native phone pages'):
            release.guard_units(units|{page:body('unsupported resident')},binding,self.units)
        # Unsealed development installs still own this service normally.
        self.assertIsNone(release.unit_binding({page:body('development resident')}))
        # Copying a real release guard onto the new service cannot make the
        # unsupported graph look like a qualified sealed service.
        for text in (body('unsupported resident'),units['headunit-receiver.service']):
            with self.assertRaises(ValueError):
                release.unit_binding(units|{page:text},self.units)

    def test_sport_opt_in_seals_optional_modules_and_retains_legacy_inventory(self):
        self.managed()
        original = self.publish('sport-disabled')
        self.assertFalse({'tools/' + name for name in release.SPORT_TOOLS} &
                         {record['path'] for record in release.verify(original)['files']})
        self.settings['sport_telemetry'] = True
        self.write_receiver()
        for name in release.SPORT_TOOLS:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        enabled = self.publish('sport-enabled')
        self.assertTrue({'tools/' + name for name in release.SPORT_TOOLS} <=
                        {record['path'] for record in release.verify(enabled)['files']})
        for count in (0, 1):
            with self.subTest(partial_sport_modules=count):
                with patch.object(release, 'SPORT_TOOLS', release.SPORT_TOOLS[:count]):
                    partial = self.publish('partial-sport-' + str(count))
                with self.assertRaises(ValueError):
                    release.verify(partial)
        # Disabling the opt-in by changing bytes cannot hide an optional graph;
        # admission reads the pinned receiver bytes before inventory selection.
        receiver = Path(enabled['path']) / 'config/receiver.json'
        body = receiver.read_bytes()
        values = json.loads(body); values['sport_telemetry'] = False
        receiver.chmod(0o640); receiver.write_text(json.dumps(values)); receiver.chmod(0o440)
        with self.assertRaisesRegex(ValueError, 'hash-bound'):
            release.verify(enabled)
        receiver.chmod(0o640); receiver.write_bytes(body); receiver.chmod(0o440)
        self.assertEqual(release.verify(original)['version'], 3)
        with self.assertRaisesRegex(ValueError, 'managed'):
            release.inventory(self.recipe['configuration'], None, None, 3, sport_enabled=True)

    def test_trip_history_opt_in_exact_hash_bound_module_closure(self):
        self.managed()
        self.settings['sport_telemetry'] = True
        self.write_receiver()
        for name in release.SPORT_TOOLS:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        legacy = self.publish('trip-history-disabled')
        self.assertNotIn('tools/trip_history.py', {r['path'] for r in release.verify(legacy)['files']})
        self.settings['trip_history'] = True
        self.write_receiver()
        with self.assertRaises((OSError, ValueError)):
            self.publish('trip-history-missing-module')
        shutil.copyfile(ROOT / 'tools/trip_history.py', self.repo / 'tools/trip_history.py')
        enabled = self.publish('trip-history-enabled')
        self.assertIn('tools/trip_history.py', {r['path'] for r in release.verify(enabled)['files']})
        module = Path(enabled['path']) / 'tools/trip_history.py'
        raw = module.read_bytes()
        module.chmod(0o644)
        module.write_bytes(raw + b'\n')
        module.chmod(0o444)
        with self.assertRaises(ValueError):
            release.verify(enabled)
        module.chmod(0o644)
        module.write_bytes(raw)
        module.chmod(0o444)
        receiver = Path(enabled['path']) / 'config/receiver.json'
        raw = receiver.read_bytes()
        values = json.loads(raw)
        values['trip_history'] = False
        receiver.chmod(0o640)
        receiver.write_text(json.dumps(values))
        receiver.chmod(0o440)
        with self.assertRaisesRegex(ValueError, 'hash-bound'):
            release.verify(enabled)
        receiver.chmod(0o640)
        receiver.write_bytes(raw)
        receiver.chmod(0o440)
        self.assertEqual(release.verify(legacy)['version'], 3)
        with self.assertRaisesRegex(ValueError, 'sport'):
            release.inventory(self.recipe['configuration'], None, self.recipe['execution'], 3,
                              sport_enabled=False, trip_history_enabled=True)

    def test_private_trip_and_native_publication_keep_both_optional_graphs(self):
        self.managed()
        self.settings.update(sport_telemetry=True, trip_history=True,
                             prephone_publication_socket=str(self.external / 'prephone.sock'))
        self.write_receiver()
        for name in (*release.SPORT_TOOLS, 'trip_history.py'):
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        with self.assertRaises((OSError, ValueError)):
            self.publish('trip-native-missing-publication')
        for name in release.PREPHONE_TOOLS:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        binding = self.publish('trip-native-combined')
        names = {record['path'] for record in release.verify(binding)['files']}
        self.assertTrue({'tools/' + name for name in (*release.SPORT_TOOLS, *release.PREPHONE_TOOLS,
                                                      'trip_history.py')} <= names)
        units = self.bundle(binding)
        self.assertIn('--prephone-publication-socket ' + str(self.external / 'prephone.sock'),
                      units['headunit-cluster.service'])
        with self.portable_checkers():
            release.verify_unit_bundle(binding, units, self.units)
        for name in ('trip_history.py', 'prephone_native.py'):
            with self.subTest(missing_module=name):
                module = Path(binding['path']) / 'tools' / name
                raw = module.read_bytes()
                module.chmod(0o644)
                module.write_bytes(raw + b'\n')
                module.chmod(0o444)
                with self.assertRaises(ValueError):
                    release.verify(binding)
                module.chmod(0o644)
                module.write_bytes(raw)
                module.chmod(0o444)
        release.verify(binding)

    def test_retained_v2_inventory_is_exact_and_new_publication_requires_v3(self):
        self.managed(True)
        legacy = release.inventory(self.recipe['configuration'], None, self.recipe['execution'], 2)
        current = release.inventory(self.recipe['configuration'], None, self.recipe['execution'], 3)
        self.assertEqual(set(current) - set(legacy),
                         {'tools/private_state.py', 'tools/carplay_readiness.py', 'tools/drive_mode_profiles.py'})
        self.assertFalse(set(legacy) - set(current))
        self.recipe['version'] = 2
        self.assertIs(release.validate_recipe(self.recipe), self.recipe)
        before = set(self.pool.iterdir())
        with self.assertRaisesRegex(ValueError, 'requires version 3'):
            release.stage(self.recipe, self.pool)
        self.assertEqual(set(self.pool.iterdir()), before)

    def test_retained_v2_verifies_and_rolls_back_from_v3_with_exact_inventory(self):
        # Reconstruct the old manifest inventory as a non-executed file fixture.
        # Portable checker substitutions do not qualify the old runtime ABI.
        import hashlib
        self.managed(True)
        old = self.publish('retained-v2'); root = Path(old['path'])
        manifest = root / 'manifest.json'
        value = json.loads(manifest.read_bytes())
        added = {'tools/private_state.py', 'tools/carplay_readiness.py', 'tools/drive_mode_profiles.py'}
        (root / 'tools').chmod(0o755)
        for name in added: (root / name).unlink()
        (root / 'tools').chmod(0o555)
        value['version'] = 2
        value['files'] = [item for item in value['files'] if item['path'] not in added]
        manifest.chmod(0o644)
        manifest.write_text(json.dumps(value, sort_keys=True) + '\n')
        manifest.chmod(0o444)
        old['sha256'] = hashlib.sha256(manifest.read_bytes()).hexdigest()
        self.assertEqual(release.verify(old)['version'], 2)
        old_units = self.bundle(old)
        with self.portable_checkers(): transaction.install(old_units, self.units, self.journals, self.system)
        new = self.publish('current-v3'); new_units = self.bundle(new)
        with self.portable_checkers(): snapshot = transaction.install(new_units, self.units, self.journals, self.system)
        with self.portable_checkers(): transaction.rollback(snapshot, self.units, self.journals, self.system)
        self.assertEqual({name: (self.units / name).read_text() for name in old_units}, old_units)
        # Neither new dependency may disappear from the v3 graph.
        for name in added:
            target = Path(new['path']) / name
            body = target.read_bytes(); target.parent.chmod(0o755); target.unlink()
            with self.assertRaises(ValueError): release.verify(new)
            target.write_bytes(body); target.chmod(0o444); target.parent.chmod(0o555)
        # A damaged retained target still prevents rollback.
        with self.portable_checkers():
            pending = transaction.install(new_units, self.units, self.journals, self.system)
        (root / 'tools').chmod(0o755); (root / 'tools/phone_manager.py').unlink()
        with self.assertRaises(ValueError), self.portable_checkers():
            transaction.rollback(pending, self.units, self.journals, self.system)

    def test_classic_home_inventory_survives_forward_presentation_migration(self):
        # File/unit fixtures only: no GTK, phone or physical presentation runs.
        self.managed(True)
        page = self.repo / 'tools/phone_page.py'
        current_page = page.read_bytes()
        # Explicit nonexecuted predecessor file fixture, before sport existed.
        page.write_bytes(b'from phone_page_presentation import ResidentPresentation\n')
        with patch.object(release, 'RESIDENT_TOOLS', release.LEGACY_RESIDENT_TOOLS):
            classic = self.publish('classic-home')
        page.write_bytes(current_page)
        value = release.verify(classic)
        self.assertFalse({'tools/' + name for name in release.HOME_PRESENTATION_TOOLS} &
                         {record['path'] for record in value['files']})
        old_units = self.bundle(classic)
        with self.portable_checkers():
            transaction.install(old_units, self.units, self.journals, self.system)
        current = self.publish('glass-home')
        names = {record['path'] for record in release.verify(current)['files']}
        self.assertTrue({'tools/' + name for name in release.HOME_PRESENTATION_TOOLS} <= names)
        # A forward selection must validate the existing classic unit bundle.
        new_units = self.bundle(current)
        with self.portable_checkers():
            release.verify_units(classic, self.units)
            transaction.install(new_units, self.units, self.journals, self.system)
            release.verify_units(current, self.units)
        for count in (1, 2):
            with self.subTest(partial_modules=count):
                partial = release.LEGACY_RESIDENT_TOOLS + release.HOME_PRESENTATION_TOOLS[:count] + release.SPORT_PAGE_TOOLS
                with patch.object(release, 'RESIDENT_TOOLS', partial):
                    incomplete = self.publish('partial-home-' + str(count))
                with self.assertRaises(ValueError):
                    release.verify(incomplete)

    def test_sport_leaf_dependency_is_sealed_and_retained_home2_is_hash_bound(self):
        self.managed(True)
        page = self.repo / 'tools/phone_page.py'
        current_page = page.read_bytes()
        predecessor_tools = release.LEGACY_RESIDENT_TOOLS + release.HOME_PRESENTATION_TOOLS
        page.write_bytes(b'from phone_home import HomeView\n')
        with patch.object(release, 'RESIDENT_TOOLS', predecessor_tools):
            predecessor = self.publish('pre-sport-home2')
        page.write_bytes(current_page)
        old_names = {item['path'] for item in release.verify(predecessor)['files']}
        self.assertNotIn('tools/phone_page_sport.py', old_names)
        current = self.publish('sport-home2')
        names = {item['path'] for item in release.verify(current)['files']}
        self.assertIn('tools/phone_page_sport.py', names)
        # A current import graph can never claim predecessor admission.
        with patch.object(release, 'RESIDENT_TOOLS', predecessor_tools):
            incomplete = self.publish('missing-sport-home2')
        with self.assertRaisesRegex(ValueError, 'complete sport page module graph'):
            release.verify(incomplete)
        retained = Path(predecessor['path']) / 'tools/phone_page.py'
        retained.chmod(0o644)
        retained.write_bytes(current_page)
        retained.chmod(0o444)
        with self.assertRaisesRegex(ValueError, 'hash-bound'):
            release.verify(predecessor)

    def test_pre_drive_manager_inventory_migrates_without_admitting_partial_current_graph(self):
        # Explicit non-executed historical file/unit fixture. Live predecessor
        # verification is separate from this portable inventory regression.
        self.managed(True)
        manager = self.repo / 'tools/phone_manager.py'
        current_source = manager.read_bytes()
        manager.write_bytes(b"from private_state import private_read\n")
        with patch.object(release, 'MANAGER_TOOLS', release.PRE_DRIVE_MANAGER_TOOLS):
            old = self.publish('pre-drive-manager')
        manager.write_bytes(current_source)
        self.assertNotIn('tools/drive_mode_profiles.py',
                         {record['path'] for record in release.verify(old)['files']})
        old_units = self.bundle(old)
        with self.portable_checkers():
            transaction.install(old_units, self.units, self.journals, self.system)
        new = self.publish('drive-manager')
        new_units = self.bundle(new)
        with self.portable_checkers():
            release.verify_units(old, self.units)
            transaction.install(new_units, self.units, self.journals, self.system)
            release.verify_units(new, self.units)
        with patch.object(release, 'MANAGER_TOOLS', release.PRE_DRIVE_MANAGER_TOOLS):
            missing = self.publish('missing-current-drive')
        with self.assertRaisesRegex(ValueError, 'complete drive module graph'):
            release.verify(missing)
        # The remaining old manager dependencies are still mandatory.
        for name in ('private_state.py', 'carplay_readiness.py'):
            with self.subTest(missing_module=name):
                partial = tuple(item for item in release.MANAGER_TOOLS if item != name)
                with patch.object(release, 'MANAGER_TOOLS', partial):
                    broken = self.publish('partial-drive-' + name.replace('.', '-'))
                with self.assertRaises(ValueError):
                    release.verify(broken)

    def test_optional_native_publication_seals_module_endpoint_and_exact_role(self):
        self.managed()
        self.settings['prephone_publication_socket'] = str(self.external / 'prephone.sock')
        self.write_receiver()
        for name in release.PREPHONE_TOOLS:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        binding = self.publish('native-publication')
        value = release.verify(binding)
        names = {record['path'] for record in value['files']}
        self.assertIn('tools/prephone_native.py', names)
        self.assertFalse((self.external / 'prephone.sock').exists())
        units = self.bundle(binding)
        self.assertIn('--prephone-publication-socket ' + str(self.external / 'prephone.sock'),
                      units['headunit-cluster.service'])
        with self.portable_checkers():
            release.verify_unit_bundle(binding, units, self.units)
        broken = dict(units)
        broken['headunit-cluster.service'] = broken['headunit-cluster.service'].replace(
            '--prephone-publication-socket ' + str(self.external / 'prephone.sock'), '')
        with self.assertRaises(ValueError):
            with self.portable_checkers():
                release.verify_unit_bundle(binding, broken, self.units)
        broken = dict(units)
        broken['headunit-cluster.service'] = broken['headunit-cluster.service'].replace('prephone.sock', 'foreign.sock')
        with self.assertRaises(ValueError):
            with self.portable_checkers():
                release.verify_unit_bundle(binding, broken, self.units)
        (Path(binding['path']) / 'tools').chmod(0o755)
        (Path(binding['path']) / 'tools/prephone_native.py').unlink()
        with self.assertRaises(ValueError):
            release.verify(binding)

    def test_combined_trip_ambient_indicator_closures_preserve_dated_native_role(self):
        self.managed()
        self.settings.update(sport_telemetry=True, simulate_vehicle=False,
                             simulate_dated_vehicle=True,
                             prephone_publication_socket=str(self.external / 'prephone.sock'))
        optional = release.PREPHONE_TOOLS + release.AMBIENT_TOOLS + release.INDICATOR_TOOLS + ('trip_history.py',)
        for name in release.SPORT_TOOLS + optional:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        for trip in (False, True):
            for ambient in (False, True):
                for indicators in (False, True):
                    with self.subTest(trip=trip, ambient=ambient, indicators=indicators):
                        self.settings.update(trip_history=trip, ambient_light=ambient,
                                             prephone_indicators=indicators)
                        self.write_receiver()
                        binding = self.publish(f'combined-flags-{int(trip)}-{int(ambient)}-{int(indicators)}')
                        names = {record['path'] for record in release.verify(binding)['files']}
                        expected = set(release.PREPHONE_TOOLS)
                        if trip:
                            expected.add('trip_history.py')
                        if ambient:
                            expected.update(release.AMBIENT_TOOLS)
                        if indicators:
                            expected.update(release.INDICATOR_TOOLS)
                        self.assertEqual(names.intersection({'tools/' + name for name in optional}),
                                         {'tools/' + name for name in expected})
                        units = self.bundle(binding)
                        command = units['headunit-cluster.service']
                        self.assertIn(' --simulate-dated', command)
                        self.assertNotIn(' --simulate ', command)
                        self.assertEqual('--ambient-light-software' in command, ambient)
                        self.assertEqual('--prephone-indicators' in command, indicators)
                        with self.portable_checkers():
                            release.verify_unit_bundle(binding, units, self.units)
                        tools = Path(binding['path']) / 'tools'
                        for name in expected:
                            module = tools / name
                            original = module.read_bytes()
                            tools.chmod(0o755)
                            module.unlink()
                            tools.chmod(0o555)
                            with self.subTest(missing=name), self.assertRaises(ValueError):
                                release.verify(binding)
                            tools.chmod(0o755)
                            module.write_bytes(original)
                            module.chmod(0o444)
                            tools.chmod(0o555)
                        release.verify(binding)

    def test_native_publication_and_trip_history_have_independent_sealed_closures(self):
        self.managed()
        self.settings['sport_telemetry'] = True
        optional = ('prephone_native.py', 'prephone_consumer.py', 'trip_history.py')
        for name in release.SPORT_TOOLS + optional:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        for native, trip in ((False, False), (True, False), (False, True), (True, True)):
            with self.subTest(native=native, trip=trip):
                if native:
                    self.settings['prephone_publication_socket'] = str(self.external / 'prephone.sock')
                else:
                    self.settings.pop('prephone_publication_socket', None)
                self.settings['trip_history'] = trip
                self.write_receiver()
                binding = self.publish(f'combined-optional-{int(native)}-{int(trip)}')
                names = {record['path'] for record in release.verify(binding)['files']}
                expected = ({'tools/prephone_native.py', 'tools/prephone_consumer.py'} if native else set())
                if trip:
                    expected.add('tools/trip_history.py')
                self.assertEqual(names.intersection({'tools/' + name for name in optional}), expected)
                if native and trip:
                    tools = Path(binding['path']) / 'tools'
                    for name in optional:
                        module = tools / name
                        raw = module.read_bytes()
                        tools.chmod(0o755)
                        module.unlink()
                        tools.chmod(0o555)
                        with self.subTest(missing=name), self.assertRaises(ValueError):
                            release.verify(binding)
                        tools.chmod(0o755)
                        module.write_bytes(raw)
                        module.chmod(0o444)
                        tools.chmod(0o555)
                    release.verify(binding)

    def test_optional_ambient_software_exact_closure_and_owner_flag(self):
        self.managed()
        self.settings['prephone_publication_socket'] = str(self.external / 'prephone.sock')
        for name in release.PREPHONE_TOOLS + release.AMBIENT_TOOLS:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                self.settings['ambient_light'] = enabled
                self.write_receiver()
                binding = self.publish('ambient-' + str(enabled).lower())
                value = release.verify(binding)
                names = {record['path'] for record in value['files']}
                for module in release.AMBIENT_TOOLS:
                    self.assertEqual('tools/' + module in names, enabled)
                units = self.bundle(binding)
                self.assertEqual('--ambient-light-software' in units['headunit-cluster.service'], enabled)
                with self.portable_checkers():
                    release.verify_unit_bundle(binding, units, self.units)
                broken = dict(units)
                body = broken['headunit-cluster.service']
                broken['headunit-cluster.service'] = (body.replace(' --ambient-light-software', '')
                    if enabled else body.replace('--background', '--background --ambient-light-software'))
                with self.assertRaises(ValueError):
                    with self.portable_checkers():
                        release.verify_unit_bundle(binding, broken, self.units)
        for missing in release.AMBIENT_TOOLS:
            binding = self.publish('ambient-missing-' + missing.replace('.', '-'))
            (Path(binding['path']) / 'tools').chmod(0o755)
            (Path(binding['path']) / 'tools' / missing).unlink()
            with self.assertRaises(ValueError):
                release.verify(binding)

    def test_optional_indicator_triple_closure_all_flag_combinations_and_exact_native_role(self):
        self.managed()
        self.settings['prephone_publication_socket'] = str(self.external / 'prephone.sock')
        modules = release.PREPHONE_TOOLS + release.AMBIENT_TOOLS + release.INDICATOR_TOOLS
        for name in modules:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        for ambient in (False, True):
            for indicator in (False, True):
                with self.subTest(ambient=ambient, indicator=indicator):
                    self.settings.update(ambient_light=ambient, prephone_indicators=indicator)
                    self.write_receiver()
                    binding = self.publish('triple-' + str(ambient).lower() + '-' + str(indicator).lower())
                    value = release.verify(binding)
                    names = {record['path'] for record in value['files']}
                    for module in release.INDICATOR_TOOLS:
                        self.assertEqual('tools/' + module in names, indicator)
                    for module in release.AMBIENT_TOOLS:
                        self.assertEqual('tools/' + module in names, ambient)
                    units = self.bundle(binding)
                    self.assertEqual('--prephone-indicators' in units['headunit-cluster.service'], indicator)
                    with self.portable_checkers():
                        release.verify_unit_bundle(binding, units, self.units)
                    for mutation in ('missing', 'duplicate') if indicator else ('extra',):
                        broken = dict(units)
                        body = broken['headunit-cluster.service']
                        broken['headunit-cluster.service'] = (body.replace(' --prephone-indicators', '')
                            if mutation == 'missing' else body.replace('--background', '--background --prephone-indicators'))
                        with self.assertRaises(ValueError):
                            with self.portable_checkers():
                                release.verify_unit_bundle(binding, broken, self.units)
        for missing in modules:
            binding = self.publish('triple-missing-' + missing.replace('.', '-'))
            (Path(binding['path']) / 'tools').chmod(0o755)
            (Path(binding['path']) / 'tools' / missing).unlink()
            with self.assertRaises(ValueError):
                release.verify(binding)
        # Missing any member BEFORE publication also rejects the exact closure.
        for missing in modules:
            path = self.repo / 'tools' / missing
            original = path.read_bytes()
            path.unlink()
            try:
                with self.assertRaises((ValueError, OSError)):
                    self.publish('triple-source-missing-' + missing.replace('.', '-'))
            finally:
                path.write_bytes(original)

    def test_optional_indicator_default_bytes_positional_inventory_and_strict_flags(self):
        from install_cm5_appliance import native_cluster_command
        self.managed()
        roles = self.recipe['configuration']
        execution = self.recipe['execution']
        baseline = release.inventory(roles, None, execution, 3, None, False, True)
        disabled = release.inventory(roles, None, execution, 3, None, False, True,
                                     ambient_enabled=False, indicators_enabled=False)
        self.assertEqual(baseline, disabled)
        self.assertTrue({'tools/' + name for name in release.PREPHONE_TOOLS} <= set(baseline))
        self.assertFalse(any('tools/' + name in baseline for name in release.INDICATOR_TOOLS))
        cluster = {'width': 1920, 'height': 720, 'output': 'fixture'}
        old = native_cluster_command(self.repo, self.settings, cluster, ['--simulate'])
        self.settings['prephone_indicators'] = False
        self.assertEqual(old, native_cluster_command(self.repo, self.settings, cluster, ['--simulate']))
        with self.assertRaises(ValueError):
            release.inventory(roles, None, execution, indicators_enabled=True)
        self.settings['prephone_indicators'] = True
        self.write_receiver()
        with self.assertRaises(ValueError):
            self.publish('indicators-without-publication')
        self.settings['prephone_publication_socket'] = str(self.external / 'prephone.sock')
        self.write_receiver()
        self.recipe.pop('execution')
        with self.assertRaises(ValueError):
            self.publish('indicators-unmanaged')
        self.managed()
        for invalid in (1, 'true', None):
            self.settings['prephone_indicators'] = invalid
            self.write_receiver()
            with self.assertRaises(ValueError):
                self.publish('indicators-invalid-' + str(invalid))

    def test_optional_ambient_requires_boolean_managed_native_publication(self):
        self.settings['ambient_light'] = True
        self.write_receiver()
        with self.assertRaises(ValueError):
            self.publish('ambient-without-publication')
        self.settings['prephone_publication_socket'] = str(self.external / 'prephone.sock')
        self.write_receiver()
        with self.assertRaises(ValueError):
            self.publish('ambient-unmanaged')
        self.managed()
        for invalid in (1, 'true', None):
            self.settings['ambient_light'] = invalid
            self.write_receiver()
            with self.assertRaises(ValueError):
                self.publish('ambient-invalid-' + str(invalid))

    def test_unmanaged_dated_simulation_cannot_be_injected_or_removed(self):
        legacy = self.publish('legacy-native-no-dated-injection')
        units = self.bundle(legacy)
        broken = dict(units)
        broken['headunit-cluster.service'] = broken['headunit-cluster.service'].replace('--simulate', '--simulate-dated')
        with self.assertRaisesRegex(ValueError, 'Dated simulation differs'):
            release.verify_unit_bundle(legacy, broken, self.units)
        self.settings['simulate_vehicle'] = False
        self.settings['simulate_dated_vehicle'] = True
        self.write_receiver()
        dated = self.publish('unmanaged-dated-native')
        units = self.bundle(dated)
        release.verify_unit_bundle(dated, units, self.units)
        broken = dict(units)
        broken['headunit-cluster.service'] = broken['headunit-cluster.service'].replace('--simulate-dated', '')
        with self.assertRaisesRegex(ValueError, 'Dated simulation differs'):
            release.verify_unit_bundle(dated, broken, self.units)

    def test_dated_simulation_preserves_opted_sport_module_closure(self):
        self.managed()
        self.settings['simulate_vehicle'] = False
        self.settings['simulate_dated_vehicle'] = True
        for enabled in (False, True):
            with self.subTest(sport_telemetry=enabled):
                self.settings['sport_telemetry'] = enabled
                self.write_receiver()
                if enabled:
                    for name in release.SPORT_TOOLS:
                        shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
                binding = self.publish('dated-sport-' + str(enabled).lower())
                value = release.verify(binding)
                names = {record['path'] for record in value['files']}
                optional = {'tools/' + name for name in release.SPORT_TOOLS}
                self.assertEqual(names & optional, optional if enabled else set())
                units = self.bundle(binding)
                self.assertIn('--simulate-dated', units['headunit-cluster.service'])
                with self.portable_checkers():
                    release.verify_unit_bundle(binding, units, self.units)
                if enabled:
                    tools = Path(binding['path']) / 'tools'
                    tools.chmod(0o755)
                    for name in release.SPORT_TOOLS:
                        member = tools / name
                        body = member.read_bytes()
                        member.unlink()
                        with self.assertRaises(ValueError):
                            release.verify(binding)
                        member.write_bytes(body)
                        member.chmod(0o444)
                    tools.chmod(0o555)
                    release.verify(binding)

    def test_dated_simulation_sealed_native_role_and_legacy_graph(self):
        self.managed()
        self.settings['simulate_vehicle'] = False
        self.settings['simulate_dated_vehicle'] = True
        self.settings['prephone_publication_socket'] = str(self.external / 'prephone.sock')
        self.write_receiver()
        for name in release.PREPHONE_TOOLS:
            shutil.copyfile(ROOT / 'tools' / name, self.repo / 'tools' / name)
        binding = self.publish('dated-native-simulation')
        units = self.bundle(binding)
        self.assertIn(' --simulate-dated', units['headunit-cluster.service'])
        self.assertNotIn(' --simulate ', units['headunit-cluster.service'])
        with self.portable_checkers():
            release.verify_unit_bundle(binding, units, self.units)
        for replacement in ('', '--simulate', '--vehicle-socket /private/foreign.sock',
                            '--simulate-dated --simulate', '--simulate-dated --simulate-dated'):
            broken = dict(units)
            broken['headunit-cluster.service'] = broken['headunit-cluster.service'].replace('--simulate-dated', replacement)
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                with self.portable_checkers():
                    release.verify_unit_bundle(binding, broken, self.units)
        # Old release inventories/commands remain verifiable when the opt-in is absent.
        self.settings.pop('simulate_dated_vehicle')
        self.settings['simulate_vehicle'] = True
        self.write_receiver()
        legacy = self.publish('legacy-native-simulation')
        legacy_units = self.bundle(legacy)
        self.assertIn(' --simulate ', legacy_units['headunit-cluster.service'])
        self.assertNotIn('--simulate-dated', legacy_units['headunit-cluster.service'])
        with self.portable_checkers():
            release.verify_unit_bundle(legacy, legacy_units, self.units)

    def test_optional_native_publication_refuses_unmanaged_or_foreign_state_path(self):
        self.settings['prephone_publication_socket'] = str(self.external / 'prephone.sock')
        self.write_receiver()
        with self.assertRaises(ValueError):
            self.publish('unmanaged-publication')
        self.managed()
        self.settings['prephone_publication_socket'] = str(self.source / 'prephone.sock')
        self.write_receiver()
        with self.assertRaises(ValueError):
            self.publish('foreign-publication')

    def test_v3_manager_and_resident_inventory_close_the_static_graph(self):
        for resident in (False, True):
            with self.subTest(resident=resident):
                self.managed(resident)
                before = {path.name for path in self.external.iterdir()}
                binding = self.publish('managed-home' if resident else 'managed-only')
                value = release.verify(binding); root = Path(binding['path'])
                self.assertEqual(value['version'], 3)
                self.assertEqual(value['execution'], self.recipe['execution'])
                names = {record['path'] for record in value['files']}
                self.assertEqual(names, set(release.inventory(value['configuration'], None, value['execution'])))
                self.assertTrue({'tools/' + name for name in release.MANAGER_TOOLS} <= names)
                self.assertIn('deploy/cm5/headunit-managed-receiver.service.in', names)
                self.assertEqual('tools/phone_page_boot.py' in names, resident)
                self.assertEqual('deploy/cm5/headunit-phone-page.service.in' in names, resident)
                manager = json.loads(release.read_configuration_member(root / 'config/phone-manager.json', binding, 'phone_manager'))
                self.assertEqual(manager['standardDisplayConfig'], str(root / 'config/standard-displays.conf'))
                self.assertEqual(manager['stateDirectory'], str(self.external))
                self.assertNotEqual(root / 'config/displays.conf', root / 'config/standard-displays.conf')
                self.assertEqual((root / 'config/displays.conf').read_bytes(), (root / 'config/standard-displays.conf').read_bytes())
                self.assertEqual({path.name for path in self.external.iterdir()}, before)
                self.assertFalse(any(path.name in ('manager.sock', 'phones.json', 'effective.json', 'media.json')
                                     for path in root.rglob('*')))

    def test_resident_surface_pair_is_external_exact_and_requires_safe_enabled_home(self):
        self.managed(True)
        ui = self.ultra()
        self.settings['preview_sink'] = 'none'; self.write_receiver()
        ui.write_bytes(ui.read_bytes() + ('nativePageControlFile=' + self.settings['native_page_control_file'] + '\n'
                                         'nativePageLeaseFile=' + self.settings['native_page_lease_file'] + '\n').encode())
        before = {path.name for path in self.external.iterdir()}
        binding = self.publish('surface-home'); root = Path(binding['path'])
        self.assertIn('tools/main_surface_handoff.py', {record['path'] for record in release.verify(binding)['files']})
        settings = load_receiver_configuration(root / 'config/receiver.json')
        self.assertEqual(settings['native_page_control_file'], self.external / 'home-surface.request')
        self.assertEqual(settings['native_page_lease_file'], self.external / 'home-surface.lease')
        self.assertEqual({path.name for path in self.external.iterdir()}, before)
        self.assertFalse(any(path.name in ('home-surface.request', 'home-surface.lease') for path in root.rglob('*')))
        page_path = self.source / release.ROLES['phone_page'][0]
        page_raw, settings_raw, ui_raw = page_path.read_bytes(), copy.deepcopy(self.settings), ui.read_bytes()
        for change in ('missing-pair', 'legacy-enabled', 'wrong-page', 'camera', 'wrong-ui', 'no-resident'):
            with self.subTest(change=change):
                value = json.loads(page_raw); self.settings = copy.deepcopy(settings_raw); recipe = copy.deepcopy(self.recipe)
                if change == 'missing-pair': self.settings.pop('native_page_lease_file')
                elif change == 'legacy-enabled':
                    value['schemaVersion'] = 1; value.pop('surfaceControlFile'); value.pop('surfaceLeaseFile')
                elif change == 'wrong-page': value['surfaceControlFile'] = str(self.external / 'manager.lock')
                elif change == 'camera': self.settings['camera_manual_profile'] = str(self.external / 'camera.json')
                elif change == 'wrong-ui': ui.write_bytes(ui_raw.replace(b'home-surface.request', b'other.request'))
                else: recipe['execution']['resident_home'] = False; recipe['configuration'].pop('phone_page')
                self.write_receiver(); page_path.write_text(json.dumps(value)); recipe['release_id'] = 'bad-' + change
                try:
                    with self.assertRaises(ValueError): release.stage(recipe, self.pool)
                finally:
                    page_path.write_bytes(page_raw); ui.write_bytes(ui_raw)
        self.settings = settings_raw; self.write_receiver()

    def test_released_home_preflight_loads_its_actual_closed_graph_without_runtime_effects(self):
        self.managed(True)
        binding = self.publish('isolated-home'); root = Path(binding['path'])
        # Only the executable checker is substituted: the release contains
        # explicit non-runnable AArch64 headers in this portable fixture. The
        # released production boot, readers, verifier and real display parser
        # run in an isolated interpreter with no GTK/radio boundary injected.
        checker = self.root / 'portable-display-checker'
        shutil.copyfile(CHECKER, checker); checker.chmod(0o755)
        identity = {}
        if os.geteuid() == 0:
            self.root.chmod(0o711)
            identity = {'user': self.account.pw_uid, 'group': self.account.pw_gid,
                        'extra_groups': os.getgrouplist(self.account.pw_name, self.account.pw_gid)}
        hostile = self.root / 'hostile'; hostile.mkdir()
        marker = hostile / 'executed'
        (hostile / 'sitecustomize.py').write_text('from pathlib import Path; Path(' + repr(str(marker)) + ').write_text("bad")')
        environment = dict(os.environ, PYTHONPATH=str(hostile))
        before = {path.name: (path.read_bytes(), path.stat().st_ino, path.stat().st_mode)
                  for path in self.external.iterdir() if path.is_file()}
        code = ('import functools,json,sys; from pathlib import Path; root=Path(sys.argv[1]); '
                'sys.path.insert(0,str(root/"tools")); '
                'import phone_manager,phone_pairing,phone_page_boot,main_surface_handoff; '
                'assert all(Path(m.__file__).is_relative_to(root) for m in '
                '(phone_manager,phone_pairing,phone_page_boot,main_surface_handoff)); '
                'phone_page_boot.inspect_boot=functools.partial(phone_page_boot.inspect_boot,checker=Path(sys.argv[3])); '
                'result=phone_page_boot.main(["--release-root",str(root),"--release-manifest-sha256",sys.argv[2],'
                '"--profile",str(root/"config/phone-page.json"),'
                '"--manager-profile",str(root/"config/phone-manager.json"),'
                '"--receiver-config",str(root/"config/receiver.json"),"--validate-only"]); '
                'assert "gi" not in sys.modules and "phone_page" not in sys.modules; '
                'raise SystemExit(result)')
        result = subprocess.run([sys.executable, '-B', '-I', '-c', code, str(root), binding['sha256'], str(checker)],
                                check=True, capture_output=True, text=True, timeout=10,
                                env=environment, **identity)
        self.assertEqual(json.loads(result.stdout), {'configurationValid': True, 'enabled': True,
                         'waylandDisplay': 'wayland-0', 'output': 'HDMI-A-1'})
        self.assertFalse(marker.exists())
        self.assertEqual({path.name: (path.read_bytes(), path.stat().st_ino, path.stat().st_mode)
                          for path in self.external.iterdir() if path.is_file()}, before)
        self.assertFalse(any((self.external / name).exists() for name in
                             ('page.lock', 'manager.lock', 'manager.sock', 'home-surface.request', 'home-surface.lease')))
        self.assertFalse((root / 'tools/__pycache__').exists())
        release.verify(binding)

    def test_sealed_home_derivation_offers_both_phone_selected_modes_without_static_changes(self):
        self.managed(True); self.ultra()
        self.settings['preview_sink'] = 'none'; self.write_receiver()
        binding = self.publish('derived-home'); root = Path(binding['path'])
        config = release.config_path(root, 'receiver')
        _, base, profile = sealed_manager_configuration(config, release.config_path(root, 'phone_manager'),
                                                        binding, self.account.pw_uid, CHECKER)
        source_ui = release.read_configuration_member(release.config_path(root, 'ultra'), binding, 'ultra')
        static = {path: path.read_bytes() for path in (root / 'config').iterdir()}
        expected_pair = (self.external / 'home-surface.request', self.external / 'home-surface.lease')
        settings = managed_receiver_settings(base, profile, 'AA:BB:CC:DD:EE:FF')
        self.assertEqual((settings['native_page_control_file'], settings['native_page_lease_file']), expected_pair)
        self.assertEqual(settings['preview_sink'], 'wayland-gpu')
        self.assertEqual(settings['standard_display_config'], str(root / 'config/standard-displays.conf'))
        self.assertEqual(settings['ultra_installed_ui'], root / 'config/installed-ui.conf')
        environment = native_page_environment({'HEADUNIT_NATIVE_PAGE_CONTROL': '/foreign/request',
            'HEADUNIT_NATIVE_PAGE_SURFACE_LEASE': '/foreign/lease'}, settings,
            {'main.wayland': 'wayland-0', 'main.output': 'HDMI-A-1'})
        self.assertEqual(environment, {'HEADUNIT_NATIVE_PAGE_CONTROL': str(expected_pair[0]),
                                      'HEADUNIT_NATIVE_PAGE_SURFACE_LEASE': str(expected_pair[1])})
        derived = release.ui_fields(native_page_ui(source_ui, settings))
        self.assertEqual(derived['nativePageControlFile'], str(expected_pair[0]))
        self.assertEqual(derived['nativePageLeaseFile'], str(expected_pair[1]))
        self.assertNotIn('nativePageControlFile', release.ui_fields(source_ui))
        self.assertEqual({path: path.read_bytes() for path in (root / 'config').iterdir()}, static)
        self.assertFalse(any((self.external / name).exists() for name in ('home-surface.request', 'home-surface.lease')))
        release.verify(binding)

    def test_home_handoff_dependency_missing_cannot_publish_partial_release(self):
        self.managed(True)
        (self.repo / 'tools/main_surface_handoff.py').unlink()
        with self.assertRaises(FileNotFoundError): self.publish()
        self.assertFalse((self.pool / 'release-one').exists())
        self.assertFalse(any(path.name.startswith('.stage-') for path in self.pool.iterdir()))

    def test_managed_executable_inventory_requires_independent_regular_source_files(self):
        self.managed(True); self.ultra()
        self.add_role('climate', b'{"fixture":true}')
        self.settings.update(climate_config=self.recipe['configuration']['climate'],
                             vehicle_state_backend='simulation'); self.write_receiver()
        self.assertEqual(set(release.binaries(self.recipe['configuration'], None)), {
            'headunit-control-probe', 'headunit-airplay-advertiser', 'headunit-cluster-wayland',
            'headunit-display-config-check', 'headunit-identity-config-check',
            'headunit-installed-ui-probe', 'headunit-vehicle-config-check'})
        binary = self.repo / 'build/headunit-identity-config-check'
        original = binary.read_bytes(); target = self.root / 'proven-binary'
        target.write_bytes(original); target.chmod(0o755)
        for kind in ('symlink', 'hardlink'):
            with self.subTest(kind=kind):
                binary.unlink()
                if kind == 'symlink': binary.symlink_to(target)
                else: os.link(target, binary)
                with self.assertRaises((OSError, ValueError)): self.publish('rejected-' + kind)
                self.assertFalse((self.pool / ('rejected-' + kind)).exists())
                self.assertFalse(any(path.name.startswith('.stage-') for path in self.pool.iterdir()))
        binary.unlink(); binary.write_bytes(original); binary.chmod(0o755)
        binding = self.publish('regular-binaries')
        self.assertEqual(release.verify(binding)['execution'], self.recipe['execution'])

    def test_v2_execution_requires_exact_profiles_and_a_separate_standard_role(self):
        self.managed(True)
        original = copy.deepcopy(self.recipe)
        changes = ('version', 'unknown-execution', 'wrong-receiver', 'nonboolean-home',
                   'missing-manager', 'missing-standard', 'missing-home', 'unselected-home', 'same-standard-file')
        for change in changes:
            with self.subTest(change=change):
                value = copy.deepcopy(original)
                if change == 'version': value['version'] = 1
                elif change == 'unknown-execution': value['execution']['unexpected'] = True
                elif change == 'wrong-receiver': value['execution']['receiver'] = 'run_lab_receiver'
                elif change == 'nonboolean-home': value['execution']['resident_home'] = 1
                elif change == 'missing-manager': value['configuration'].pop('phone_manager')
                elif change == 'missing-standard': value['configuration'].pop('standard_displays')
                elif change == 'missing-home': value['configuration'].pop('phone_page')
                elif change == 'unselected-home': value['execution']['resident_home'] = False
                else: value['configuration']['standard_displays'] = value['configuration']['displays']
                with self.assertRaises(ValueError): release.validate_recipe(value)
        manager_path = self.source / 'phone-manager.json'
        manager = json.loads(manager_path.read_bytes())
        for wrong in (str(self.source / 'displays.conf'), str(self.external / 'unsealed-displays.conf')):
            manager['standardDisplayConfig'] = wrong; manager_path.write_text(json.dumps(manager))
            with self.assertRaises(ValueError): self.publish()

    def test_v2_external_state_and_profile_relationships_are_not_relocated(self):
        self.managed(True)
        manager_path, page_path = self.source / 'phone-manager.json', self.source / 'phone-page.json'
        manager, page = json.loads(manager_path.read_bytes()), json.loads(page_path.read_bytes())
        for change in ('disabled-manager', 'disabled-page', 'wrong-socket', 'release-state', 'wrong-media'):
            with self.subTest(change=change):
                changed_manager, changed_page = copy.deepcopy(manager), copy.deepcopy(page)
                self.settings['media_state'] = str(self.external / 'media.json')
                if change == 'disabled-manager': changed_manager['enabled'] = False
                elif change == 'disabled-page': changed_page['enabled'] = False
                elif change == 'wrong-socket': changed_page['managerSocket'] = str(self.external / 'other.sock')
                elif change == 'release-state':
                    state = self.pool / 'live-state'
                    changed_manager['stateDirectory'] = str(state)
                    changed_page['managerSocket'] = str(state / 'manager.sock')
                    self.settings['media_state'] = str(state / 'media.json')
                else: self.settings['media_state'] = str(self.external / 'other-media.json')
                manager_path.write_text(json.dumps(changed_manager)); page_path.write_text(json.dumps(changed_page))
                self.write_receiver()
                with self.assertRaises(ValueError): self.publish()
        self.assertFalse((self.pool / 'live-state').exists())

    def test_v2_rejects_camera_owners_without_a_sealed_handoff_graph(self):
        self.managed(True)
        for kind in ('manual', 'standard', 'native', 'signed'):
            with self.subTest(kind=kind):
                self.settings.pop('camera_manual_profile', None)
                self.settings.pop('standard_camera_control_file', None)
                self.settings.pop('standard_camera_lease_file', None)
                self.settings['preview_sink'] = 'wayland-touch'
                if kind == 'manual': self.settings['camera_manual_profile'] = str(self.external / 'manual.json')
                elif kind == 'standard':
                    self.settings |= {'preview_sink': 'wayland-gpu',
                        'standard_camera_control_file': str(self.external / 'camera-request'),
                        'standard_camera_lease_file': str(self.external / 'camera-lease')}
                else:
                    ui = self.ultra()
                    with ui.open('a') as output:
                        output.write(('nativeCameraControlFile' if kind == 'native' else 'cameraControlFile') +
                                     '=' + str(self.external / 'camera-request') + '\n')
                self.write_receiver()
                with self.assertRaises(ValueError): self.publish()

    def test_configuration_capture_rechecks_bound_hash_after_verification(self):
        self.managed()
        binding = self.publish(); root = Path(binding['path']); path = root / 'config/phone-manager.json'
        with self.assertRaises(ValueError): release.read_configuration_member(path, binding, 'receiver')
        original = release.read_file
        calls = 0
        def mutate_after_capture(selected, *args, **kwargs):
            nonlocal calls
            result = original(selected, *args, **kwargs)
            if Path(selected) == path:
                calls += 1
                if calls == 1:
                    path.chmod(0o640)
                    path.write_bytes(result[0].replace(b'"autoConnectSelected": false', b'"autoConnectSelected": true'))
                    path.chmod(0o440)
            return result
        with patch.object(release, 'read_file', mutate_after_capture):
            with self.assertRaisesRegex(ValueError, 'changed after release verification'):
                release.read_configuration_member(path, binding, 'phone_manager')
        self.assertEqual(calls, 2)

    def test_configuration_graph_capture_rejects_program_changed_during_capture(self):
        self.managed()
        binding = self.publish()
        binary = Path(binding['path']) / 'build/headunit-control-probe'
        original = release._read_verified_member
        changed = False
        def change_program_after_member(*args):
            nonlocal changed
            raw = original(*args)
            if not changed:
                changed = True
                data = bytearray(binary.read_bytes())
                data[-1] ^= 1
                binary.chmod(0o755)
                binary.write_bytes(data)
                binary.chmod(0o555)
            return raw
        with patch.object(release, '_read_verified_member', change_program_after_member):
            with self.assertRaisesRegex(ValueError, 'Application release file changed'):
                release.read_configuration_members(binding)
        self.assertTrue(changed)

    def test_configuration_graph_capture_returns_every_exact_sealed_role(self):
        self.managed()
        binding = self.publish()
        manifest, captured = release.read_configuration_members(binding)
        self.assertEqual(set(captured), set(manifest['configuration']))
        self.assertEqual(json.loads(captured['phone_manager'])['autoConnectSelected'], False)
        for role, raw in captured.items():
            self.assertEqual(raw, release.config_path(Path(binding['path']), role).read_bytes())

    def test_v2_full_bundle_admits_exact_managed_and_home_commands(self):
        for resident in (False, True):
            with self.subTest(resident=resident):
                self.managed(resident)
                binding = self.publish('commands-home' if resident else 'commands-manager')
                units = self.bundle(binding)
                expected = {'headunit-' + name + '.service' for name in release.UNIT_NAMES[:4]}
                if resident: expected.add('headunit-phone-page.service')
                self.assertEqual(set(units), expected)
                self.assertEqual(release.unit_binding(units, self.units), binding)
                with self.portable_checkers(): self.assertEqual(release.verify_unit_bundle(binding, units, self.units), binding)
                for text in units.values():
                    self.assertNotIn('-B -E -s -B -E -s', text)
                    first = next(line for line in text.splitlines() if line.startswith('ExecStartPre='))
                    self.assertIn('/tools/appliance_release.py --verify ', first)
                manager = units['headunit-receiver.service']
                self.assertIn('/tools/phone_manager.py --release-root ', manager)
                self.assertIn(' --manager-profile ' + binding['path'] + '/config/phone-manager.json', manager)
                self.assertIn(' --uid ' + str(self.account.pw_uid), manager)
                self.assertIn('KillMode=control-group', manager)
                if resident:
                    page = units['headunit-phone-page.service']
                    self.assertIn(' --receiver-config ' + binding['path'] + '/config/receiver.json', page)
                    self.assertIn('Environment=GDK_BACKEND=wayland', page)
                    self.assertNotIn(' --validate-only', page)

    def test_v2_fanout_is_a_complete_separately_supervised_role(self):
        self.managed(True)
        self.settings['simulate_vehicle'] = False
        self.settings['vehicle_socket'] = str(self.external / 'receiver.sock')
        fanout = self.add_role('fanout', json.dumps({'version': 1,
            'input': str(self.external / 'car.sock'), 'consumers': {
                'backup': str(self.external / 'backup.sock'),
                'receiver': str(self.external / 'receiver.sock')}}).encode())
        self.write_receiver()
        binding = self.publish(); units = self.bundle(binding)
        self.assertIn('headunit-vehicle.service', units)
        self.assertIn(' --vehicle-fanout-config ' + binding['path'] + '/config/vehicle-fanout.json',
                      units['headunit-receiver.service'])
        self.assertIn(' --vehicle-socket ' + str(self.external / 'backup.sock'), units['headunit-cluster.service'])
        with self.portable_checkers():
            release.verify_unit_bundle(binding, units, self.units)
            missing = dict(units); missing.pop('headunit-vehicle.service')
            with self.assertRaises(ValueError): release.verify_unit_bundle(binding, missing, self.units)
        self.assertEqual(json.loads(fanout.read_bytes())['input'], str(self.external / 'car.sock'))
        self.assertFalse((self.external / 'car.sock').exists())

    def test_v2_exact_bundle_rejects_missing_extra_and_modified_execution(self):
        self.managed(True)
        binding = self.publish(); units = self.bundle(binding)
        mutations = ('missing-home', 'extra-camera', 'wrong-uid', 'wrong-profile', 'direct-receiver',
                     'missing-wayland-wait', 'extra-preflight', 'radio-before-guard', 'wrong-user',
                     'wrong-environment', 'wrong-ordering', 'duplicate-start')
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                changed = dict(units); receiver = 'headunit-receiver.service'; page = 'headunit-phone-page.service'
                if mutation == 'missing-home': changed.pop(page)
                elif mutation == 'extra-camera': changed['headunit-camera.service'] = changed[page]
                elif mutation == 'wrong-uid': changed[receiver] = changed[receiver].replace(' --uid ' + str(self.account.pw_uid), ' --uid 999999')
                elif mutation == 'wrong-profile': changed[receiver] = changed[receiver].replace('/config/phone-manager.json', '/config/phone-manager.json-other')
                elif mutation == 'direct-receiver': changed[receiver] = changed[receiver].replace('/tools/phone_manager.py', '/tools/run_lab_receiver.py')
                elif mutation == 'missing-wayland-wait': changed[page] = '\n'.join(
                    line for line in changed[page].splitlines() if '/tools/wait_wayland.py' not in line) + '\n'
                elif mutation == 'extra-preflight': changed[page] = changed[page].replace('ExecStart=', 'ExecStartPre=/usr/bin/true\nExecStart=')
                elif mutation == 'radio-before-guard': changed[receiver] = changed[receiver].replace('[Service]\n', '[Service]\nExecStartPre=/usr/bin/nmcli radio wifi on\n')
                elif mutation == 'wrong-user': changed[page] = changed[page].replace('User=' + self.account.pw_name, 'User=root')
                elif mutation == 'wrong-environment': changed[page] = changed[page].replace('Environment=WAYLAND_DISPLAY=wayland-0', 'Environment=WAYLAND_DISPLAY=wayland-other')
                elif mutation == 'wrong-ordering': changed[page] = changed[page].replace('After=headunit-compositor.service', 'Before=headunit-compositor.service')
                else: changed[receiver] += '\nExecStart=/usr/bin/true\n'
                with self.portable_checkers():
                    with self.assertRaises(ValueError): release.verify_unit_bundle(binding, changed, self.units)
        self.assertEqual(self.system.calls, [])

    def test_v2_published_guard_rejects_incomplete_variant_before_any_platform_loader(self):
        self.managed(True)
        binding = self.publish(); units = self.bundle(binding); root = Path(binding['path'])
        for name, text in units.items():
            if name != 'headunit-phone-page.service': (self.units / name).write_text(text)
        command = [sys.executable, '-B', '-E', '-s', str(root / 'tools/appliance_release.py'),
                   '--verify', str(root), '--manifest-sha256', binding['sha256'], '--unit-directory', str(self.units)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('complete application release', result.stderr)
        self.assertFalse((root / 'tools/__pycache__').exists())
        self.assertFalse((self.external / 'manager.sock').exists())
        self.assertFalse((self.external / 'page.lock').exists())

    def test_v2_reserved_alternate_owner_units_are_rejected_without_mutation(self):
        self.managed(True)
        binding = self.publish(); units = self.bundle(binding); root = Path(binding['path'])
        for name, text in units.items(): (self.units / name).write_text(text)
        command = [sys.executable, '-B', '-E', '-s', str(root / 'tools/appliance_release.py'),
                   '--verify', str(root), '--manifest-sha256', binding['sha256'], '--unit-directory', str(self.units)]
        for name in release.RESERVED_OWNER_UNITS:
            with self.subTest(name=name):
                alternate = self.units / ('headunit-' + name + '.service')
                alternate.write_text(units['headunit-receiver.service']); alternate.chmod(0o640)
                before = alternate.stat(); data = alternate.read_bytes()
                with self.assertRaises(ValueError): release.verify_units(binding, self.units)
                result = subprocess.run(command, capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('complete application release', result.stderr)
                self.assertEqual((alternate.stat().st_ino, alternate.stat().st_mode), (before.st_ino, before.st_mode))
                self.assertEqual(alternate.read_bytes(), data)
                self.assertNotIn(alternate.name, transaction.MANAGED)
                self.assertEqual(self.system.calls, [])
                alternate.unlink()

    def test_v2_staging_is_not_display_qualification_and_prepare_checks_both_routes(self):
        self.managed(True)
        standard = self.source / 'standard-displays.conf'
        original = standard.read_text()
        for number, replacement in enumerate(('main.output=HDMI-A-other', 'cluster.width=1280')):
            with self.subTest(replacement=replacement):
                old = 'main.output=HDMI-A-1' if replacement.startswith('main.') else 'cluster.width=1920'
                standard.write_text(original.replace(old, replacement))
                binding = self.publish('bad-display-' + str(number))
                release.verify(binding)  # Static bytes/closure alone are valid.
                with self.assertRaises(ValueError): self.bundle(binding)
        self.assertFalse((self.external / 'manager.sock').exists())
        self.assertEqual(self.system.calls, [])

    def test_v2_structural_journal_binding_survives_non_target_payload_damage(self):
        self.managed(True)
        old = self.publish('managed-old'); old_units = self.bundle(old)
        with self.portable_checkers(): snapshot_old = transaction.install(old_units, self.units, self.journals, self.system)
        self.assertTrue(snapshot_old.exists())
        new = self.publish('managed-new'); new_units = self.bundle(new)
        with self.portable_checkers(): snapshot = transaction.install(new_units, self.units, self.journals, self.system)
        for relative in ('build/headunit-control-probe', 'config/standard-displays.conf',
                         'tools/install_cm5_appliance.py', 'manifest.json'):
            path = Path(new['path']) / relative
            old_mode = path.stat().st_mode & 0o777
            path.chmod(old_mode | 0o200); path.write_bytes(b'damaged non-target payload'); path.chmod(old_mode)
        with patch.object(release, 'read_file', side_effect=AssertionError('Structural binding must not read payloads')):
            self.assertEqual(release.unit_binding(new_units, self.units), new)
        with self.portable_checkers(): transaction.rollback(snapshot, self.units, self.journals, self.system)
        self.assertEqual({name: (self.units / name).read_text() for name in old_units}, old_units)
        with self.portable_checkers(): release.verify_units(old, self.units)
        self.assertTrue(all(command[1] not in ('start', 'stop', 'restart') for command in self.system.calls))

    def test_v2_required_manager_import_missing_cannot_publish_partial_release(self):
        self.managed()
        (self.repo / 'tools/phone_pairing.py').unlink()
        with self.assertRaises(FileNotFoundError): self.publish()
        self.assertFalse((self.pool / 'release-one').exists())
        self.assertFalse(any(path.name.startswith('.stage-') for path in self.pool.iterdir()))

    def test_v2_optional_home_removal_and_rollback_preserve_external_state(self):
        self.managed(True)
        marker = self.external / 'phones.json'; marker.write_bytes(b'externally owned preferences\n'); marker.chmod(0o600)
        original = marker.stat()
        old = self.publish('with-home'); old_units = self.bundle(old)
        with self.portable_checkers(): transaction.install(old_units, self.units, self.journals, self.system)
        self.managed(False)
        self.recipe['configuration'].pop('phone_page')
        self.settings.pop('native_page_control_file')
        self.settings.pop('native_page_lease_file')
        self.write_receiver()
        new = self.publish('without-home'); new_units = self.bundle(new)
        with self.portable_checkers(): snapshot = transaction.install(new_units, self.units, self.journals, self.system)
        self.assertFalse((self.units / 'headunit-phone-page.service').exists())
        with self.portable_checkers():
            release.verify_units(new, self.units)
            transaction.rollback(snapshot, self.units, self.journals, self.system)
            release.verify_units(old, self.units)
        self.assertEqual((self.units / 'headunit-phone-page.service').read_text(), old_units['headunit-phone-page.service'])
        self.assertEqual(marker.read_bytes(), b'externally owned preferences\n')
        self.assertEqual((marker.stat().st_ino, marker.stat().st_mode), (original.st_ino, original.st_mode))

    def test_guard_inspects_manually_inserted_native_page_before_any_new_process(self):
        binding=self.publish();root=Path(binding['path']);units=self.bundle(binding)
        for name,text in units.items():(self.units/name).write_text(text)
        release.verify_units(binding,self.units)
        page=self.units/'headunit-phone-page.service'
        page.write_text(body('manual resident outside sealed release'));page.chmod(0o640)
        before=(page.read_bytes(),page.stat().st_mode,page.stat().st_ino)
        with self.assertRaisesRegex(ValueError,'complete application release'):
            release.verify_units(binding,self.units)
        command=[sys.executable,'-B','-E','-s',str(root/'tools/appliance_release.py'),
                 '--verify',str(root),'--manifest-sha256',binding['sha256'],
                 '--unit-directory',str(self.units)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=5)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('complete application release',result.stderr)
        self.assertEqual((page.read_bytes(),page.stat().st_mode,page.stat().st_ino),before)
        page.unlink()
        release.verify_units(binding,self.units)
        # Unsupported special/empty entries are rejected too, not treated as
        # absence merely because the sealed manifest has no corresponding role.
        for kind in ('empty','symlink','fifo'):
            with self.subTest(kind=kind):
                if kind=='empty':page.touch(mode=0o600)
                elif kind=='symlink':page.symlink_to(self.units/'headunit-receiver.service')
                else:os.mkfifo(page,0o600)
                with self.assertRaises((OSError,ValueError)):release.verify_units(binding,self.units)
                page.unlink()
        release.verify_units(binding,self.units)

    def test_transaction_rejects_page_added_to_bound_bundle_without_mutating_units(self):
        binding=self.publish();units=self.bundle(binding)
        transaction.install(units,self.units,self.journals,self.system)
        before={path.name:(path.read_bytes(),path.stat().st_mode,path.stat().st_ino) for path in self.units.iterdir()}
        journals_before=set(self.journals.iterdir());self.system.calls.clear()
        for page in (body('unsealed resident'),units['headunit-receiver.service']):
            with self.subTest(bound='--manifest-sha256' in page):
                with self.assertRaises(ValueError):
                    transaction.install(units|{'headunit-phone-page.service':page},self.units,self.journals,self.system)
                self.assertEqual({path.name:(path.read_bytes(),path.stat().st_mode,path.stat().st_ino)
                                  for path in self.units.iterdir()},before)
                self.assertEqual(set(self.journals.iterdir()),journals_before)
                self.assertFalse(any(call[0]=='systemd-analyze' or
                                     call[:2] in (['systemctl','disable'],['systemctl','enable'],['systemctl','daemon-reload'])
                                     for call in self.system.calls))
        release.verify_units(binding,self.units)

    def test_unclosed_roles_aliases_and_unsafe_endpoint_locations_fail(self):
        self.ultra()
        original = copy.deepcopy(self.recipe)
        self.recipe['configuration'].pop('vehicle_profile')
        with self.assertRaises(ValueError): self.publish()
        self.recipe = copy.deepcopy(original)
        self.settings['media_state'] = str(self.pool / 'release-one/config/receiver.json'); self.write_receiver()
        with self.assertRaises(ValueError): self.publish()
        self.settings['media_state'] = str(self.external / 'media.json'); self.write_receiver()
        self.recipe['configuration']['warnings'] = self.recipe['configuration']['displays']
        with self.assertRaises(ValueError): self.publish()
        self.recipe = copy.deepcopy(original)
        self.add_role('warnings', b'{}')
        with self.assertRaises(ValueError): self.publish()
        self.assertEqual(list(self.pool.iterdir()), [self.pool / '.release.lock'])

    def test_fanout_and_native_camera_configuration_are_sealed_and_bound(self):
        self.settings.pop('simulate_vehicle')
        self.settings['vehicle_socket'] = str(self.external / 'receiver.sock')
        fanout = self.add_role('fanout', json.dumps({'version': 1, 'input': str(self.external / 'adapter.sock'),
                   'consumers': {'backup': str(self.external / 'backup.sock'), 'receiver': str(self.external / 'receiver.sock')}}).encode())
        self.write_receiver(); binding = self.publish()
        target = release.config_path(Path(binding['path']), 'fanout')
        with self.assertRaises(ValueError): load_configuration(target)
        self.assertEqual(load_configuration(target, release_binding=binding)['input'], self.external / 'adapter.sock')
        self.assertIn('headunit-vehicle.service', self.bundle(binding))
        self.recipe['configuration'].pop('fanout'); self.settings.pop('vehicle_socket'); self.ultra()
        self.add_role('camera', b'{}')
        native = {'request': str(self.external / 'camera-request'), 'lease': str(self.external / 'camera-lease')}
        self.recipe['native_camera'] = native
        ui = Path(self.recipe['configuration']['ultra'])
        with ui.open('a') as stream:
            stream.write('nativeCameraControlFile=' + native['request'] + '\nnativeCameraLeaseFile=' + native['lease'] + '\n')
        camera = self.publish('release-camera')
        self.assertIn('headunit-camera.service', self.bundle(camera))

    def test_manifest_payload_metadata_and_extra_imports_are_checked(self):
        for mutation in ('data', 'mode', 'extra', 'link', 'hardlink', 'fifo', 'manifest', 'missing'):
            with self.subTest(mutation=mutation):
                binding = self.publish('release-' + mutation); root = Path(binding['path'])
                path = root / 'config/receiver.json'
                if mutation == 'data': path.chmod(0o640); path.write_text('{}'); path.chmod(0o440)
                if mutation == 'mode': path.chmod(0o640)
                if mutation in ('link', 'hardlink', 'fifo', 'missing'):
                    path.parent.chmod(0o750 | 0o200); path.unlink()
                    if mutation == 'link': path.symlink_to(self.source / 'receiver.json')
                    if mutation == 'hardlink': os.link(self.source / 'receiver.json', path)
                    if mutation == 'fifo': os.mkfifo(path, 0o440)
                    path.parent.chmod(0o750)
                if mutation == 'extra':
                    (root / 'tools').chmod(0o755); (root / 'tools/extra.py').write_text(''); (root / 'tools').chmod(0o555)
                if mutation == 'manifest':
                    path = root / 'manifest.json'; path.chmod(0o644); path.write_text('{}'); path.chmod(0o444)
                with self.assertRaises((ValueError, FileNotFoundError)): release.verify(binding)
                if mutation == 'hardlink':
                    path.parent.chmod(0o750 | 0o200); path.unlink(); path.parent.chmod(0o750)

    def test_wrong_architecture_and_source_special_files_are_rejected(self):
        binary = self.repo / 'build/headunit-control-probe'
        good = binary.read_bytes()
        binary.write_bytes(b'not a Linux binary')
        with self.assertRaises(ValueError): self.publish()
        binary.write_bytes(good)
        path = self.source / 'receiver.json'; data = path.read_bytes(); path.unlink(); os.mkfifo(path, 0o600)
        with self.assertRaises(ValueError): self.publish()
        path.unlink(); path.write_bytes(data); path.chmod(0o600)
        other = self.source / 'hardlink'; os.link(path, other)
        with self.assertRaises(ValueError): self.publish()
        other.unlink()
        link = self.root / 'pool-link'; link.symlink_to(self.pool)
        with self.assertRaises(ValueError): release.stage(self.recipe, link)

    def test_captured_receiver_bytes_are_not_replaced_by_a_second_read(self):
        path = self.source / 'receiver.json'; captured = path.read_bytes()
        path.write_text('{}')
        parsed = parse_receiver_configuration(captured, path)
        self.assertEqual(parsed['bin_dir'], self.repo / 'build')

    def test_exact_unit_tokens_reject_suffixes_duplicates_and_wrong_namespace(self):
        binding = self.publish(); units = self.bundle(binding)
        changes = (
            ('headunit-compositor.service', 'labwc-cm5-dual-rc.xml', 'labwc-cm5-dual-rc.xml-other'),
            ('headunit-outputs.service', 'kanshi-cm5-dual.conf', 'kanshi-cm5-dual.conf-other'),
            ('headunit-receiver.service', ' --config ' + str(Path(binding['path']) / 'config/receiver.json') + '\n',
             ' --config ' + str(Path(binding['path']) / 'config/receiver.json') + ' --config /tmp/other.json\n'),
            ('headunit-cluster.service', ' --size 1920 720', ' --size 1920 720 --size 1280 720'),
            ('headunit-cluster.service', ' --unit-directory ' + str(self.units), ' --unit-directory /tmp/other-units'),
        )
        for name, original, replacement in changes:
            with self.subTest(name=name, replacement=replacement):
                altered = dict(units); self.assertIn(original, altered[name])
                altered[name] = altered[name].replace(original, replacement)
                with self.assertRaises(ValueError): release.unit_binding(altered, self.units)
        for path in ('/tmp//release', '/tmp/./release', '/tmp/release/../other'):
            with self.assertRaises(ValueError): release.exact_path(path)

    def test_greeting_endpoint_agrees_with_sealed_profile_and_unit(self):
        self.ultra()
        endpoint = str(self.external / 'greeting-sequence')
        self.settings['greeting_sequence_file'] = endpoint
        self.write_receiver()
        with self.assertRaisesRegex(ValueError, 'greeting endpoints'):
            self.publish('mismatched-greeting')
        with Path(self.recipe['configuration']['ultra']).open('a') as stream:
            stream.write('greetingSequenceFile=' + endpoint + '\n')
        binding = self.publish('greeting')
        units = self.bundle(binding)
        option = ' --greeting-sequence-file ' + endpoint
        self.assertIn(option, units['headunit-cluster.service'])
        with self.portable_checkers():
            self.assertEqual(release.verify_unit_bundle(binding, units, self.units), binding)
        for replacement in ('', option + option, ' --greeting-sequence-file /tmp/foreign',
                            ' --greeting-sequence-file'):
            altered = units | {'headunit-cluster.service': units['headunit-cluster.service'].replace(option, replacement)}
            with self.assertRaises(ValueError):
                with self.portable_checkers():
                    release.verify_unit_bundle(binding, altered, self.units)

    def test_local_gauge_service_binds_only_its_sealed_profile(self):
        self.ultra()
        self.settings['pre_phone_gauges'] = True
        self.write_receiver()
        binding = self.publish()
        units = self.bundle(binding)
        profile = str(release.config_path(Path(binding['path']), 'ultra'))
        option = ' --local-ultra-profile ' + profile
        self.assertIn(option, units['headunit-cluster.service'])
        self.assertEqual(release.unit_binding(units, self.units), binding)
        for replacement in (' --local-ultra-profile /tmp/other.conf',
                            option + option, ' --local-ultra-profile'):
            altered = dict(units)
            altered['headunit-cluster.service'] = altered['headunit-cluster.service'].replace(option, replacement)
            with self.assertRaises(ValueError):
                release.unit_binding(altered, self.units)
        altered = dict(units)
        altered['headunit-cluster.service'] = altered['headunit-cluster.service'].replace(
            ' --size 1920 720', ' --size 1280 720')
        with self.assertRaises(ValueError):
            release.unit_binding(altered, self.units)

    def test_published_python_guard_runs_and_launcher_rejects_overrides_before_checkers(self):
        binding = self.publish(); root = Path(binding['path']); units = self.bundle(binding)
        for name, text in units.items(): (self.units / name).write_text(text)
        command = [sys.executable, '-B', '-E', '-s', str(root / 'tools/appliance_release.py'),
                   '--verify', str(root), '--manifest-sha256', binding['sha256'],
                   '--unit-directory', str(self.units)]
        subprocess.run(command, check=True, capture_output=True, timeout=5)
        bad = subprocess.run([sys.executable, '-B', '-E', '-s', str(root / 'tools/run_lab_receiver.py'),
                              '--config', str(root / 'config/receiver.json'), '--release-root', str(root),
                              '--release-manifest-sha256', binding['sha256'], '--bin-dir', '/tmp/other', '--dry-run'],
                             capture_output=True, text=True, timeout=5)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn('cannot be overridden', bad.stderr)
        self.assertFalse((root / 'tools/__pycache__').exists())

    def test_child_helpers_ignore_inherited_python_imports_and_leave_package_clean(self):
        binding = self.publish(); root = Path(binding['path'])
        hostile = self.root / 'hostile'; hostile.mkdir()
        marker = hostile / 'executed'
        (hostile / 'sitecustomize.py').write_text('from pathlib import Path; Path(' + repr(str(marker)) + ').write_text("bad")')
        environment = dict(os.environ); environment['PYTHONPATH'] = str(hostile)
        for script in ('media_bridge.py', 'carplay_reconnect.py'):
            subprocess.run(python_helper_command(root / 'tools' / script, '--help'),
                           env=environment, check=True, capture_output=True, timeout=5)
        self.assertFalse(marker.exists())
        self.assertFalse((root / 'tools/__pycache__').exists())
        release.verify(binding)

    def test_sealed_launch_group_comes_from_account_not_identity_file(self):
        binding = self.publish(); value = release.verify(binding)
        info = SimpleNamespace(st_uid=value['uid'], st_gid=value['gid'] + 1)
        self.assertEqual(receiver_owner(info), (info.st_uid, info.st_gid))
        self.assertEqual(receiver_owner(info, binding), (value['uid'], value['gid']))
        info.st_uid += 1
        with self.assertRaises(ValueError): receiver_owner(info, binding)

    def test_ultra_semantic_preflight_failure_selects_no_service(self):
        self.ultra(); binding = self.publish()
        def rejected(command, **options):
            if Path(command[0]).name == 'headunit-installed-ui-probe':
                raise subprocess.CalledProcessError(1, command, stderr='invalid warning/profile fixture')
            return self.dry_run(command, **options)
        with self.assertRaises(subprocess.CalledProcessError): prepare(binding, rejected)
        self.assertEqual(list(self.units.iterdir()), [])
        release.verify(binding)

    def test_root_preflight_checks_run_as_the_declared_display_account(self):
        self.ultra(); self.managed(resident=True)
        binding = self.publish()
        captured = []
        def runner(command, **options):
            captured.append((command, options))
            return self.dry_run(command, **options)
        # Stop after the four read-only admissions. Rendering/installing units
        # is outside this test; no attempt to assume root occurs on the host.
        with patch('install_appliance_release.os.geteuid', return_value=0), \
                patch('install_appliance_release.render', side_effect=RuntimeError('after admission')):
            with self.assertRaisesRegex(RuntimeError, 'after admission'):
                prepare(binding, runner, self.units)
        self.assertEqual(len(captured), 4)
        for command, options in captured:
            self.assertEqual(options['user'], self.account.pw_uid)
            self.assertEqual(options['group'], self.account.pw_gid)
            self.assertEqual(options['extra_groups'], os.getgrouplist(self.account.pw_name, self.account.pw_gid))
            self.assertEqual(options['timeout'], 30)
        self.assertEqual(list(self.units.iterdir()), [])

    def test_published_identifier_and_promotion_failure_never_overwrite(self):
        binding = self.publish()
        with self.assertRaises(ValueError): self.publish()
        with patch('appliance_release.os.rename', side_effect=OSError('promotion failed')):
            with self.assertRaises(OSError): self.publish('release-failed')
        release.verify(binding)
        self.assertFalse((self.pool / 'release-failed').exists())
        self.assertFalse(any(path.name.startswith('.stage-') for path in self.pool.iterdir()))
        self.assertEqual(list(self.units.iterdir()), [])

    def test_low_space_is_rejected_before_staging_even_with_root_reserved_blocks(self):
        space = SimpleNamespace(f_frsize=4096, f_bsize=4096, f_bavail=0,
                                f_bfree=100000, f_files=100000, f_favail=100000)
        with patch.object(release.os, 'statvfs', return_value=space), \
                patch.object(release.tempfile, 'mkdtemp') as create:
            with self.assertRaisesRegex(ValueError, 'Insufficient release space'):
                self.publish()
            create.assert_not_called()
        self.assertEqual([path.name for path in self.pool.iterdir()], ['.release.lock'])
        self.assertEqual(list(self.units.iterdir()), [])

    def test_capacity_rounds_individual_files_and_preserves_reserve_and_inodes(self):
        reserve_blocks = release.PUBLICATION_RESERVE // 4096
        space = SimpleNamespace(f_frsize=4096, f_bsize=4096,
                                f_bavail=reserve_blocks + 8 + 2,
                                f_files=100000, f_favail=24)
        with patch.object(release.os, 'statvfs', return_value=space):
            release.publication_capacity(self.pool, [1, 4096])
            with self.assertRaisesRegex(ValueError, 'space'):
                release.publication_capacity(self.pool, [1, 4097])
            space.f_favail = 23
            with self.assertRaisesRegex(ValueError, 'inodes'):
                release.publication_capacity(self.pool, [1, 4096])
            space.f_files = 0
            release.publication_capacity(self.pool, [1, 4096])
            space.f_frsize = 0
            release.publication_capacity(self.pool, [1, 4096])
            space.f_bsize = 0
            with self.assertRaisesRegex(ValueError, 'capacity is unavailable'):
                release.publication_capacity(self.pool, [1])

    def test_capacity_measurement_failure_cannot_create_candidate(self):
        with patch.object(release.os, 'statvfs', side_effect=OSError('unavailable')), \
                patch.object(release.tempfile, 'mkdtemp') as create:
            with self.assertRaises(OSError):
                self.publish()
            create.assert_not_called()
        self.assertEqual([path.name for path in self.pool.iterdir()], ['.release.lock'])

    def test_source_change_after_capacity_admission_is_not_published(self):
        real_capacity = release.publication_capacity
        def changed(pool, sizes):
            real_capacity(pool, sizes)
            path = self.repo / 'tools/run_lab_receiver.py'
            path.write_bytes(path.read_bytes() + b'\n# changed fixture\n')
        with patch.object(release, 'publication_capacity', side_effect=changed):
            with self.assertRaisesRegex(ValueError, 'changed after capacity'):
                self.publish()
        self.assertEqual([path.name for path in self.pool.iterdir()], ['.release.lock'])

    def test_space_loss_during_copy_cleans_candidate_and_preserves_selected_payload(self):
        old = self.publish()
        with patch.object(release.os, 'fsync', side_effect=OSError(errno.ENOSPC, 'fixture full')):
            with self.assertRaises(OSError):
                self.publish('failed-space-race')
        release.verify(old)
        self.assertFalse((self.pool / 'failed-space-race').exists())
        self.assertFalse(any(path.name.startswith('.stage-') for path in self.pool.iterdir()))
        self.assertEqual(list(self.units.iterdir()), [])

    def test_two_releases_rollback_restores_exact_binary_and_config_paths(self):
        old = self.publish(); old_units = self.bundle(old)
        transaction.install(old_units, self.units, self.journals, self.system)
        self.settings['device_name'] = 'Updated head unit'; self.write_receiver()
        new = self.publish('release-two'); new_units = self.bundle(new)
        snapshot = transaction.install(new_units, self.units, self.journals, self.system)
        journal = json.loads((snapshot / 'journal.json').read_bytes())
        self.assertEqual(journal['releases'], {'previous': old, 'next': new})
        # Damage the *candidate*, then recover using the intact retained target.
        path = Path(new['path']) / 'build/headunit-control-probe'
        path.chmod(0o755); path.write_bytes(b'damaged'); path.chmod(0o555)
        transaction.rollback(snapshot, self.units, self.journals, self.system)
        self.assertEqual({name: (self.units / name).read_text() for name in old_units}, old_units)
        release.verify_units(old, self.units)
        self.assertEqual(load_receiver_configuration(Path(old['path']) / 'config/receiver.json').get('device_name'), None)
        self.assertTrue(all(command[1] not in ('start', 'stop', 'restart') for command in self.system.calls))

    def test_missing_rollback_target_and_mixed_unit_paths_refuse_mutation(self):
        old = self.publish(); transaction.install(self.bundle(old), self.units, self.journals, self.system)
        new = self.publish('release-two'); new_units = self.bundle(new)
        snapshot = transaction.install(new_units, self.units, self.journals, self.system)
        old_path = Path(old['path']) / 'build/headunit-cluster-wayland'
        old_path.parent.chmod(0o755); old_path.unlink(); old_path.parent.chmod(0o555)
        before = len(self.system.calls)
        with self.assertRaises((FileNotFoundError, ValueError)): transaction.rollback(snapshot, self.units, self.journals, self.system)
        self.assertEqual(len(self.system.calls), before)
        altered = dict(new_units)
        altered['headunit-cluster.service'] = altered['headunit-cluster.service'].replace(
            str(Path(new['path']) / 'build/headunit-cluster-wayland'), '/tmp/other/build/headunit-cluster-wayland')
        with self.assertRaises(ValueError): release.unit_binding(altered)
        with self.assertRaises(ValueError): transaction.install(altered, self.units, self.journals, self.system)

    def test_power_loss_guard_refuses_mixed_services_then_explicit_recovery(self):
        old = self.publish(); old_units = self.bundle(old)
        transaction.install(old_units, self.units, self.journals, self.system)
        new = self.publish('release-two'); new_units = self.bundle(new)
        original = transaction.Transaction.set_unit; count = 0
        def interrupted(owner, name, record):
            nonlocal count
            original(owner, name, record); count += 1
            if count == 2: raise PowerLoss()
        with patch.object(transaction.Transaction, 'set_unit', interrupted):
            with self.assertRaises(PowerLoss): transaction.install(new_units, self.units, self.journals, self.system)
        for binding in (old, new):
            with self.assertRaises(ValueError): release.verify_units(binding, self.units)
        # Recovery admits only the retained target. Even the new candidate's
        # manifest may be missing; no current configuration/preflight is needed.
        candidate=Path(new['path']);candidate.chmod(0o755);(candidate/'manifest.json').unlink();candidate.chmod(0o555)
        transaction.recover_pending(self.units, self.journals, self.system)
        release.verify_units(old, self.units)
        self.assertEqual({name: (self.units / name).read_text() for name in old_units}, old_units)

    def test_recovery_only_refuses_missing_retained_target_before_unit_changes(self):
        old=self.publish();transaction.install(self.bundle(old),self.units,self.journals,self.system)
        new=self.publish('release-two');new_units=self.bundle(new)
        def interrupted(args,**options):
            result=self.system(args,**options)
            if args[:2]==['systemctl','enable']:raise PowerLoss()
            return result
        with self.assertRaises(PowerLoss):transaction.install(new_units,self.units,self.journals,interrupted)
        target=Path(old['path'])/'build/headunit-cluster-wayland'
        target.parent.chmod(0o755);target.unlink();target.parent.chmod(0o555)
        before={p.name:p.read_bytes() for p in self.units.iterdir()};self.system.calls.clear()
        with self.assertRaises((FileNotFoundError,ValueError)):
            transaction.recover_pending(self.units,self.journals,self.system)
        self.assertEqual(self.system.calls,[])
        self.assertEqual({p.name:p.read_bytes() for p in self.units.iterdir()},before)
        self.assertEqual(sum(json.loads(p.read_bytes())['state']=='applying'
                             for p in self.journals.glob('transaction-*/journal.json')),1)

    def test_preflight_failure_and_tamper_before_applying_preserve_services(self):
        old = self.publish(); old_units = self.bundle(old)
        transaction.install(old_units, self.units, self.journals, self.system)
        new = self.publish('release-two'); new_units = self.bundle(new)
        with self.assertRaises(subprocess.CalledProcessError):
            prepare(new, lambda *_args, **_options: (_ for _ in ()).throw(subprocess.CalledProcessError(1, 'dry-run')))
        path = Path(new['path']) / 'config/displays.conf'
        def tamper(): path.chmod(0o640)
        self.system.verify_hook = tamper
        with self.assertRaises(RuntimeError): transaction.install(new_units, self.units, self.journals, self.system)
        release.verify_units(old, self.units)
        self.assertEqual({name: (self.units / name).read_text() for name in old_units}, old_units)


if __name__ == '__main__': unittest.main()
