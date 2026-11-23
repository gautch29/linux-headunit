#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Stage and preflight a CM5 application release; optionally select boot services.

Neither staging nor selecting a release explicitly restarts running services.
Systemd's existing automatic-restart policy can subsequently adopt new units.
"""
import argparse
import os
from pathlib import Path
import pwd
import subprocess
import sys

from appliance_release import (ancestors, config_path, exact_path, guard_units,
                               read_recipe, require_compatible_boot_helper, stage,
                               unit_binding, verify, verify_unit_bundle)
from appliance_transaction import install, Transaction
from install_cm5_appliance import render
from receiver_configuration import load_receiver_configuration


def prepare(binding, runner=subprocess.run, unit_directory=Path('/etc/systemd/system')):
    """Use the release's launcher/checkers before admitting its unit bundle."""
    value = verify(binding)
    if value.get('purpose') == 'boot-recovery':
        raise ValueError('A recovery helper is staged independently; it is not an application candidate')
    if value.get('purpose') == 'appliance':
        require_compatible_boot_helper(value, verify(value['boot_recovery']['binding']))
    root = Path(binding['path'])
    config = config_path(root, 'receiver')
    settings = load_receiver_configuration(config)
    managed = value.get('execution', {}).get('receiver') == 'phone-manager'
    manager_profile = config_path(root, 'phone_manager') if managed else None
    page_profile = config_path(root, 'phone_page') if value.get('execution', {}).get('resident_home') else None
    fanout_path = config_path(root, 'fanout') if 'fanout' in value['configuration'] else None
    reader = ['--release-root', str(root), '--release-manifest-sha256', binding['sha256']]
    identity = {}
    if os.geteuid() == 0:
        identity = {'user': value['uid'], 'group': value['gid'],
                    'extra_groups': os.getgrouplist(value['user'], value['gid'])}
    runner([sys.executable, '-B', '-E', '-s', str(root / 'tools/run_lab_receiver.py'),
            '--config', str(config), '--release-root', str(root),
            '--release-manifest-sha256', binding['sha256'], '--dry-run'],
           check=True, capture_output=True, text=True, timeout=30, **identity)
    if managed:
        manager_command = [sys.executable, '-B', '-E', '-s', str(root / 'tools/phone_manager.py'),
                           *reader, '--config', str(config), '--manager-profile', str(manager_profile),
                           '--uid', str(value['uid']), '--validate-only']
        if fanout_path:
            manager_command += ['--vehicle-fanout-config', str(fanout_path)]
        result = runner(manager_command, check=True, capture_output=True, text=True, timeout=30,
                        **identity)
        import json
        checked = json.loads(result.stdout)
        if (type(checked) is not dict or set(checked) !=
                {'configurationValid', 'enabled', 'autoConnectSelected'} or
                checked['configurationValid'] is not True or checked['enabled'] is not True or
                type(checked['autoConnectSelected']) is not bool):
            raise ValueError('Native manager preflight did not confirm read-only enabled configuration')
    if page_profile:
        result = runner([sys.executable, '-B', '-E', '-s', str(root / 'tools/phone_page_boot.py'),
                         *reader, '--profile', str(page_profile), '--manager-profile', str(manager_profile),
                         '--receiver-config', str(config), '--validate-only'],
                        check=True, capture_output=True, text=True, timeout=30, **identity)
        import json
        checked = json.loads(result.stdout)
        if (type(checked) is not dict or set(checked) !=
                {'configurationValid', 'enabled', 'waylandDisplay', 'output'} or
                checked['configurationValid'] is not True or checked['enabled'] is not True or
                type(checked['waylandDisplay']) is not str or type(checked['output']) is not str):
            raise ValueError('Native Home preflight did not confirm read-only enabled configuration')
    if 'ultra' in value['configuration']:
        result = runner([str(root / 'build/headunit-installed-ui-probe'),
                         str(config_path(root, 'ultra')), '--validate-only'],
                        check=True, capture_output=True, text=True, timeout=30, **identity)
        import json
        checked = json.loads(result.stdout)
        if (not isinstance(checked, dict) or set(checked) != {'configurationValid', 'assetVersion',
                'expiredSigningLeafExceptionUsed', 'scriptsExecuted', 'phoneConnected'} or
                checked['configurationValid'] is not True or checked['scriptsExecuted'] is not False or
                checked['phoneConnected'] is not False or type(checked['assetVersion']) is not int or
                checked['assetVersion'] <= 0 or type(checked['expiredSigningLeafExceptionUsed']) is not bool):
            raise ValueError('Ultra preflight did not confirm bounded configuration/asset validation')
    native = value['native_camera']
    units = render(root, value['user'], value['uid'], root, config, settings,
                   fanout_path=fanout_path,
                   camera_profile=config_path(root, 'camera') if native else None,
                   camera_request=Path(native['request']) if native else None,
                   camera_lease=Path(native['lease']) if native else None,
                   release_binding=binding, phone_manager_profile=manager_profile,
                   phone_page_profile=page_profile)
    guarded = guard_units(units, binding, unit_directory)
    if unit_binding(guarded) != binding: raise ValueError('Rendered release units lost their binding')
    verify_unit_bundle(binding, guarded, unit_directory)
    return guarded


def select(units, binding, unit_directory, state_directory, runner=subprocess.run, migrate_boot_helper=False):
    """Activate the stable gate before any V4 candidate unit transaction writes."""
    value = verify(binding)
    if value.get('purpose') != 'appliance':
        if migrate_boot_helper:
            raise ValueError('Bootstrap migration requires an explicit V5 appliance')
        return install(units, unit_directory, state_directory, runner)
    from appliance_boot import admit, admission, align_boot_with_units
    if migrate_boot_helper and value['version'] != 5:
        raise ValueError('Bootstrap migration requires an explicit V5 appliance')
    with Transaction(unit_directory, state_directory, runner) as owner:
        # Helper upgrades/conflicting overrides refuse BEFORE even recovery writes.
        admission(owner, value['boot_recovery'], migrate=migrate_boot_helper)
        owner.recover_pending()
        from appliance_transaction import MANAGED, _data
        previous = {name: owner.current(name) for name in MANAGED}
        old = {name: _data(record).decode() for name, record in previous.items() if record['exists']}
        prior = unit_binding(old, owner.units)
        if prior: verify_unit_bundle(prior, old, owner.units)
        verify_unit_bundle(binding, units, owner.units)
        admit(value['boot_recovery'], owner, migrate=migrate_boot_helper)
        try:
            return owner.install(units)
        except BaseException:
            align_boot_with_units(owner)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--recipe', type=Path, help='private local-administrator JSON recipe')
    source.add_argument('--existing-release', type=Path, help='select an already published exact release')
    parser.add_argument('--manifest-sha256', help='required digest when selecting an existing release')
    parser.add_argument('--release-directory', type=Path, default=Path('/opt/ultra-headunit/releases'))
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--stage-only', action='store_true', help='publish and check release without changing services')
    action.add_argument('--install', action='store_true', help='publish, check and select exact release boot paths')
    parser.add_argument('--transaction-directory', type=Path, default=Path('/var/lib/ultra-headunit/appliance'))
    parser.add_argument('--migrate-boot-helper', action='store_true',
                        help='explicit cold V5 helper migration; all owners and boot gate must be stopped')
    args = parser.parse_args()
    if args.migrate_boot_helper and not args.install:
        parser.error('--migrate-boot-helper requires --install')
    if os.geteuid() != 0: parser.error('Publishing an appliance release requires root')
    if bool(args.existing_release) != bool(args.manifest_sha256):
        parser.error('--existing-release and --manifest-sha256 must be supplied together')
    try:
        pool = exact_path(args.release_directory)
        if args.recipe:
            recipe = read_recipe(exact_path(args.recipe))
            # A dedicated group keeps sealed configuration readable by the
            # display group; no application process owns its inode.
            pwd.getpwnam(recipe['user'])
            if not pool.exists():
                ancestors(pool.parent, {0})
                pool.mkdir(mode=0o755)
            binding = stage(recipe, pool)
            print('Published application release: ' + binding['path'], flush=True)
        else:
            selected = exact_path(args.existing_release)
            if selected.parent != pool: raise ValueError('Existing release must belong to the selected release pool')
            binding = {'path': str(selected), 'sha256': args.manifest_sha256}
            verify(binding)
            print('Selected published release: ' + binding['path'], flush=True)
        value = verify(binding)
        if value.get('purpose') == 'boot-recovery':
            if args.install: raise ValueError('A recovery helper cannot be installed as application services')
            print('Independent recovery helper verified; bootstrap is activated only with a bound application')
            print('Manifest SHA-256: ' + binding['sha256'])
            return
        units = prepare(binding)
        if args.install:
            snapshot = select(units, binding, Path('/etc/systemd/system'), args.transaction_directory,
                              migrate_boot_helper=args.migrate_boot_helper)
            print('Selected release boot services; no explicit session restart was requested')
            print('Software/configuration rollback snapshot: ' + str(snapshot))
        else:
            print('Release preflight passed; boot services were not changed')
        print('Manifest SHA-256: ' + binding['sha256'])
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        parser.error(str(error))


if __name__ == '__main__': main()
