# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Portable production bootstrap contracts; no host manager or owner starts."""
import copy
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import appliance_boot as boot
import appliance_release as release
from appliance_transaction import Transaction, MANAGED
from appliance_transaction_test import System, body


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.pool = self.root / 'helpers'; self.pool.mkdir()
        self.units = self.root / 'systemd/system'; self.units.mkdir(parents=True)
        self.state = self.root / 'state'
        account = pwd.getpwuid(os.getuid())
        if account.pw_uid == 0: account = pwd.getpwnam('nobody')
        self.recipe = {'version': 4, 'purpose': 'boot-recovery', 'boot_recovery': None,
                       'execution': None, 'release_id': 'boot-helper-one', 'source_revision': 'a' * 40,
                       'repo': str(ROOT), 'user': account.pw_name, 'configuration': {}, 'native_camera': None}
        self.binding = release.stage(self.recipe, self.pool)
        self.descriptor = {'binding': self.binding, 'transaction_directory': str(self.state)}
        self.system = System(self.units)

    def tearDown(self):
        for directory, _, _ in os.walk(self.root): os.chmod(directory, 0o700)
        self.temporary.cleanup()

    def activate(self):
        with Transaction(self.units, self.state, self.system) as owner:
            boot.admit(self.descriptor, owner)

    def generate(self):
        output = self.root / ('generated-' + str(len(list(self.root.glob('generated-*')))))
        output.mkdir()
        result = subprocess.run([sys.executable, '-I', str(boot.generator_path(self.units)),
                                 str(output), str(output), str(output)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in MANAGED:
            self.assertEqual((output / (name + '.d/00-headunit-boot-recovery.conf')).read_text(),
                             '[Unit]\nRequires=' + boot.SERVICE + '\nAfter=' + boot.SERVICE + '\n')
        self.assertTrue((output / 'multi-user.target.wants' / boot.SERVICE).is_symlink())
        return (output / boot.SERVICE).read_text()

    def test_independent_helper_uses_existing_seal_without_application_members(self):
        value = release.verify(self.binding)
        self.assertEqual(value['purpose'], 'boot-recovery')
        self.assertEqual(value['configuration'], [])
        self.assertIsNone(value['execution'])
        self.assertEqual({item['path'] for item in value['files']},
                         set(release.inventory([], None, None, 4, 'boot-recovery')))
        self.assertTrue(all(item['mode'] == 0o444 for item in value['files']))
        from install_appliance_release import prepare
        with self.assertRaisesRegex(ValueError, 'not an application candidate'):
            prepare(self.binding)

    def test_atomic_activation_idempotent_no_manager_effects_and_new_only_scope(self):
        self.activate()
        self.activate()
        boot.verify_installed(self.descriptor, self.units)
        self.assertEqual(self.system.calls, [])
        self.assertEqual(list(self.state.glob('transaction-*')), [])
        self.assertIn('--helper-root ' + self.binding['path'], self.generate())
        self.assertNotIn('ExecStart=/bin/false', self.generate())
        self.assertEqual(set(MANAGED), set('headunit-' + n + '.service' for n in
                         ('compositor', 'outputs', 'cluster', 'receiver', 'vehicle', 'camera', 'phone-page')))

    def test_helper_change_refused_before_any_generator_or_journal_writes(self):
        self.activate()
        before = boot.generator_path(self.units).read_bytes()
        other = copy.deepcopy(self.recipe); other['release_id'] = 'boot-helper-two'
        descriptor = {'binding': release.stage(other, self.pool), 'transaction_directory': str(self.state)}
        with Transaction(self.units, self.state, self.system) as owner:
            with self.assertRaisesRegex(ValueError, 'qualified bootstrap migration'):
                boot.admit(descriptor, owner, check_only=True)
        self.assertEqual(before, boot.generator_path(self.units).read_bytes())
        self.assertEqual(self.system.calls, [])
        self.assertEqual(list(self.state.glob('transaction-*')), [])

    def test_missing_and_empty_verifier_fail_closed_for_every_owner(self):
        self.activate()
        path = Path(self.binding['path']) / 'tools/appliance_release.py'
        original = path.read_bytes(); path.chmod(0o644); path.write_bytes(b''); path.chmod(0o444)
        self.assertIn('ExecStart=/bin/false', self.generate())
        path.chmod(0o644); path.write_bytes(original); path.chmod(0o444)
        path.parent.chmod(0o755); path.unlink(); path.parent.chmod(0o555)
        self.assertIn('ExecStart=/bin/false', self.generate())
        with self.assertRaises((ValueError, OSError)):
            boot.recover(self.descriptor, self.units, self.system)
        self.assertEqual(self.system.calls, [])
        self.assertEqual(list(self.state.glob('transaction-*')), [])

    def test_unmanifested_module_and_link_fail_closed_before_helper_import(self):
        self.activate()
        directory = Path(self.binding['path']) / 'tools'
        directory.chmod(0o755)
        module = directory / 'json.py'
        module.write_text('raise RuntimeError("must never execute")\n'); module.chmod(0o444)
        directory.chmod(0o555)
        self.assertIn('ExecStart=/bin/false', self.generate())
        directory.chmod(0o755); module.unlink(); module.symlink_to('/etc/passwd'); directory.chmod(0o555)
        self.assertIn('ExecStart=/bin/false', self.generate())

    def test_competing_gate_refused_before_activation(self):
        (self.units / boot.SERVICE).write_text('[Service]\nExecStart=/bin/true\n')
        with Transaction(self.units, self.state, self.system) as owner:
            with self.assertRaisesRegex(ValueError, 'competing'):
                boot.admit(self.descriptor, owner)
        self.assertFalse(boot.generator_path(self.units).exists())
        self.assertEqual(self.system.calls, [])

    def test_held_owner_refusal_and_active_owner_refusal_before_recovery_jobs(self):
        self.activate()
        def runner(args, **options):
            if args[:2] == ['systemctl', 'show']:
                return subprocess.CompletedProcess(args, 0, stdout='active\n')
            raise AssertionError('A live owner must be refused before journal or job effects')
        with self.assertRaisesRegex(ValueError, 'already running'):
            boot.recover(self.descriptor, self.units, runner)
        self.assertEqual(list(self.state.glob('transaction-*')), [])
        with Transaction(self.units, self.state, self.system):
            with self.assertRaises(BlockingIOError):
                boot.recover(self.descriptor, self.units, runner)

    def test_first_bootstrap_refuses_live_or_queued_owner_before_generator_or_journal(self):
        for active, queued in ((True, False), (False, True)):
            with self.subTest(active=active, queued=queued):
                self.system.active = {MANAGED[0]: 'active'} if active else {}
                self.system.jobs = {MANAGED[0]: '17'} if queued else {}
                with Transaction(self.units, self.state, self.system) as owner:
                    with self.assertRaisesRegex(ValueError, 'inactive|pending'):
                        boot.admission(owner, self.descriptor)
                self.assertFalse(boot.generator_path(self.units).exists())
                self.assertEqual(list(self.state.glob('transaction-*')), [])
        self.system.active = {}; self.system.jobs = {}
        with Transaction(self.units, self.state, self.system) as owner:
            boot.admission(owner, self.descriptor)
        self.assertFalse(boot.generator_path(self.units).exists())

    def test_warm_selection_legacy_install_and_recovery_refuse_inactive_or_failed_gate(self):
        self.activate()
        from appliance_transaction import install, rollback, recover_pending
        self.system.active = {MANAGED[0]: 'active'}
        before = boot.generator_path(self.units).read_bytes()
        for state in ('inactive', 'failed'):
            self.system.gate = {'ActiveState': state, 'SubState': 'dead' if state == 'inactive' else 'failed', 'Result': 'success' if state == 'inactive' else 'exit-code'}
            for action in (lambda: install({MANAGED[0]: body('legacy')}, self.units, self.state, self.system),
                           lambda: rollback(self.state / 'transaction-absent00', self.units, self.state, self.system),
                           lambda: recover_pending(self.units, self.state, self.system)):
                with self.subTest(state=state), self.assertRaisesRegex(ValueError, 'successful active'):
                    action()
        self.assertEqual(before, boot.generator_path(self.units).read_bytes())
        self.assertEqual(list(self.state.glob('transaction-*')), [])
        self.assertFalse(any(args[1] in ('daemon-reload', 'enable', 'disable') for args in self.system.calls))
        self.system.gate = {'ActiveState': 'active', 'SubState': 'exited', 'Result': 'success'}
        snapshot = install({MANAGED[0]: body('legacy')}, self.units, self.state, self.system)
        recover_pending(self.units, self.state, self.system)
        rollback(snapshot, self.units, self.state, self.system)
        self.assertEqual(before, boot.generator_path(self.units).read_bytes())
        # A caller cannot silently fork the journal owner, even on legacy rollback.
        with self.assertRaisesRegex(ValueError, 'transaction owner'):
            install({MANAGED[0]: body('legacy')}, self.units, self.root / 'other-state', self.system)

    def test_exact_legacy_inventory_admin_path_is_compatible_before_bootstrap_and_fail_closed_after(self):
        import builtins
        from appliance_transaction import install, recover_pending
        original = builtins.__import__
        def legacy_import(name, *args, **kwargs):
            if name == 'appliance_boot':
                raise ModuleNotFoundError("No module named 'appliance_boot'", name='appliance_boot')
            return original(name, *args, **kwargs)
        with patch('builtins.__import__', side_effect=legacy_import):
            install({MANAGED[0]: body('legacy')}, self.units, self.state, self.system)
        self.activate()
        before = {path: path.read_bytes() for path in self.state.glob('transaction-*/journal.json')}
        with patch('builtins.__import__', side_effect=legacy_import), self.assertRaisesRegex(ValueError, 'current admission-aware'):
            recover_pending(self.units, self.state, self.system)
        self.assertEqual(before, {path: path.read_bytes() for path in self.state.glob('transaction-*/journal.json')})

    def test_installed_descriptor_rejects_template_damage_before_live_effects(self):
        self.activate()
        self.assertEqual(boot.installed_descriptor(self.units), self.descriptor)
        path = boot.generator_path(self.units); path.chmod(0o755)
        path.write_bytes(path.read_bytes() + b'\n# changed\n'); path.chmod(0o555)
        with Transaction(self.units, self.state, self.system) as owner:
            with self.assertRaisesRegex(ValueError, 'differs'):
                boot.admission(owner)
        self.assertEqual(self.system.calls, [])

    def test_fresh_optional_job_ids_and_nonblocking_admission(self):
        self.activate()
        old = MANAGED[0]; restored = MANAGED[-1]
        calls = []
        def runner(args, **options):
            calls.append(args)
            if args[1] == 'is-enabled':
                return subprocess.CompletedProcess(args, 0, stdout='enabled' if args[-1] == restored else 'not-found')
            if args[1] == 'list-jobs':
                name, number = (old, '71') if len([c for c in calls if c[1] == 'list-jobs']) == 1 else (restored, '99')
                return subprocess.CompletedProcess(args, 0, stdout=number + ' ' + name + ' start waiting\n')
            return subprocess.CompletedProcess(args, 0, stdout='')
        with Transaction(self.units, self.state, runner) as owner:
            result = boot.resolve(owner)
        self.assertEqual(result, {'enabled': [restored], 'cancelled': [old]})
        self.assertIn(['systemctl', 'cancel', '71'], calls)
        self.assertIn(['systemctl', '--no-block', 'start', restored], calls)
        for text in ('1 bad.service start waiting', '1 '+old+' stop waiting',
                     '1 '+old+' start waiting\n1 '+restored+' start waiting', 'x'*8193):
            with self.assertRaises(ValueError): boot.jobs(text)

    def test_versions_one_two_three_inventory_and_schema_unchanged(self):
        for version in (1, 2, 3):
            execution = None if version == 1 else {'receiver': 'phone-manager', 'resident_home': False}
            actual = release.inventory(release.REQUIRED_ROLES, None, execution, version)
            self.assertNotIn('tools/appliance_boot.py', actual)
            self.assertNotIn(release.BOOT_GENERATOR_TEMPLATE, actual)
            recipe = copy.deepcopy(self.recipe); recipe['version'] = version
            with self.assertRaisesRegex(ValueError, 'Invalid versioned'):
                release.validate_recipe(recipe)


if __name__ == '__main__': unittest.main()
