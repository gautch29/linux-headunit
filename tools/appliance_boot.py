# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""One retained boot gate for the existing appliance transaction owner.

Default admission retains the activated helper. Explicit cold V5 migration
keeps its original journal and restores the matching retained bootstrap when
recovery rolls application units back. Older helper inventories stay unchanged.
"""
import argparse
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

from appliance_release import (BOOT_GENERATOR_TEMPLATE, RESERVED_OWNER_UNITS, ancestors, exact_path,
                               read_file, verify, verify_units)
from appliance_transaction import MANAGED, Transaction, _write

SERVICE = 'headunit-boot-recovery.service'
GENERATOR = 'headunit-boot-recovery'
MAX_JOBS = 8192


def validate_descriptor(value):
    if type(value) is not dict or set(value) != {'binding', 'transaction_directory'}:
        raise ValueError('Invalid boot recovery descriptor')
    binding = value['binding']
    if (type(binding) is not dict or set(binding) != {'path', 'sha256'} or
            type(binding['sha256']) is not str or not re.fullmatch('[0-9a-f]{64}', binding['sha256'])):
        raise ValueError('Invalid recovery helper binding')
    root = exact_path(binding['path'])
    state = exact_path(value['transaction_directory'])
    if state == root.parent or root.parent in state.parents:
        raise ValueError('Recovery journals must stay outside the helper pool')
    return value


def generator_path(units):
    return exact_path(units).parent / 'system-generators' / GENERATOR


def generator_bytes(descriptor, units):
    validate_descriptor(descriptor)
    helper = verify(descriptor['binding'])
    if helper.get('purpose') != 'boot-recovery':
        raise ValueError('Bootstrap requires a verified independent recovery helper')
    binding = descriptor['binding']
    template, _ = read_file(Path(binding['path']) / BOOT_GENERATOR_TEMPLATE, 131072, {0, os.getuid()})
    text = template.decode('utf-8')
    for key, value in {'@HELPER_ROOT@': binding['path'], '@MANIFEST_SHA256@': binding['sha256'],
                       '@UNIT_DIRECTORY@': str(exact_path(units)),
                       '@TRANSACTION_DIRECTORY@': descriptor['transaction_directory']}.items():
        if text.count(key) != 1: raise ValueError('Invalid captured recovery generator template')
        text = text.replace(key, value)
    if '@' in text: raise ValueError('Unresolved recovery generator field')
    return text.encode()


def verify_installed(descriptor, units):
    expected = generator_bytes(descriptor, units)
    data, info = read_file(generator_path(units), 131072, {os.getuid()})
    if data != expected or stat.S_IMODE(info.st_mode) != 0o555:
        raise ValueError('Installed recovery bootstrap differs from its pinned contract')


def admit(descriptor, owner, check_only=False, migrate=False):
    """Atomically activate only after full verification, before candidate writes.

    The caller already holds the single existing transaction lock. No runtime
    jobs or reloads occur here. A different retained helper is refused before
    filesystem changes; existing journals retain their original five/seven scope.
    """
    if owner.lock is None: raise RuntimeError('Acquire the appliance transaction lock first')
    validate_descriptor(descriptor)
    if exact_path(descriptor['transaction_directory']) != owner.state:
        raise ValueError('Bootstrap and installer must use the same transaction owner')
    expected = generator_bytes(descriptor, owner.units)
    path = generator_path(owner.units)
    # A locally overriding gate would bypass the fail-closed generated unit.
    if (owner.units / SERVICE).exists() or (owner.units / SERVICE).is_symlink():
        raise ValueError('A competing boot recovery service is installed')
    if path.exists() or path.is_symlink():
        data, info = read_file(path, 131072, {os.getuid()})
        if data != expected or stat.S_IMODE(info.st_mode) != 0o555:
            if not migrate:
                raise ValueError('Recovery helper replacement requires an explicitly qualified bootstrap migration')
            validate_migration(descriptor, owner)
            if not check_only:
                _write(path, expected, 0o555)
        return
    ancestors(path.parent.parent, {os.getuid()})
    if path.parent.exists(): ancestors(path.parent, {os.getuid()})
    if check_only: return
    if not path.parent.exists(): path.parent.mkdir(mode=0o755)
    ancestors(path.parent, {os.getuid()})
    _write(path, expected, 0o555)



def installed_descriptor(units):
    """Recover the stable contract from its exact verified atomic entry.

    No additional state file: the generator already pins root, manifest, unit
    directory and journal owner. Parse only four literal declarations, then
    reproduce every byte from the retained verified template before use.
    """
    try: raw, info = read_file(generator_path(units), 131072, {os.getuid()})
    except FileNotFoundError: return None
    if stat.S_IMODE(info.st_mode) != 0o555:
        raise ValueError('Installed recovery bootstrap mode changed')
    text = raw.decode('utf-8')
    values = {}
    patterns = {'ROOT': r"ROOT = Path\('([A-Za-z0-9_./-]+)'\)",
                'DIGEST': r"DIGEST = '([0-9a-f]{64})'",
                'UNITS': r"UNITS = '([A-Za-z0-9_./-]+)'",
                'STATE': r"STATE = '([A-Za-z0-9_./-]+)'"}
    for name, pattern in patterns.items():
        found = re.findall('^' + pattern + '$', text, re.MULTILINE)
        if len(found) != 1: raise ValueError('Installed recovery bootstrap contract is malformed')
        values[name] = found[0]
    descriptor = {'binding': {'path': values['ROOT'], 'sha256': values['DIGEST']},
                  'transaction_directory': values['STATE']}
    if values['UNITS'] != str(exact_path(units)):
        raise ValueError('Installed bootstrap selects another unit directory')
    verify_installed(descriptor, units)
    return descriptor


def validate_migration(descriptor, owner):
    """Cold V5 migration, retaining the existing journal and all old helpers.

    Current sealed units remain the rollback authority. The new recovery owner
    aligns the bootstrap to those units after any interrupted transaction.
    No running owner or gate can authorize a bootstrap replacement.
    """
    if owner.lock is None: raise RuntimeError('Acquire the appliance transaction lock first')
    old = installed_descriptor(owner.units)
    if old is None or old == descriptor:
        raise ValueError('Migration requires a distinct installed retained helper')
    if old['transaction_directory'] != descriptor['transaction_directory'] or exact_path(descriptor['transaction_directory']) != owner.state:
        raise ValueError('Migration cannot replace the transaction owner')
    previous = verify(old['binding'])
    replacement = verify(descriptor['binding'])
    if (replacement['version'] != 5 or replacement.get('purpose') != 'boot-recovery' or
            previous.get('purpose') != 'boot-recovery' or
            exact_path(old['binding']['path']).parent != exact_path(descriptor['binding']['path']).parent or
            (previous['uid'], previous['gid'], previous['administrator_uid']) !=
            (replacement['uid'], replacement['gid'], replacement['administrator_uid'])):
        raise ValueError('Migration requires compatible independently retained V5 recovery')
    for name in MANAGED + tuple('headunit-' + n + '.service' for n in RESERVED_OWNER_UNITS) + (SERVICE,):
        state = owner.command(['systemctl', 'show', '--property=ActiveState', '--value', name]).stdout.strip()
        if state not in ('inactive', 'failed'):
            raise ValueError('Helper migration requires every owner and boot gate stopped')
    if pending_jobs(owner): raise ValueError('Helper migration requires no owner jobs')
    from appliance_release import unit_binding
    from appliance_transaction import _data
    units = {n: _data(r).decode() for n in MANAGED if (r := owner.current(n))['exists']}
    binding = unit_binding(units, owner.units)
    if not binding: raise ValueError('Helper migration requires a sealed current rollback graph')
    verify_units(binding, owner.units)


def align_boot_with_units(owner):
    """Complete rollback/bootstrap alignment under the same transaction lock.

    V5 can recover V1..V5 unit journals. When recovery restores an older sealed
    appliance, restore its exact retained generator before admitting its owners.
    Old releases' own verifiers then keep enforcing their unchanged contracts.
    """
    if owner.lock is None: raise RuntimeError('Acquire the appliance transaction lock first')
    installed = installed_descriptor(owner.units)
    if installed is None: return
    from appliance_release import unit_binding, verify_unit_bundle
    from appliance_transaction import _data
    units = {n: _data(r).decode() for n in MANAGED if (r := owner.current(n))['exists']}
    binding = unit_binding(units, owner.units)
    if not binding: return
    verify_unit_bundle(binding, units, owner.units)
    value = verify(binding)
    if value.get('purpose') != 'appliance': return
    desired = value['boot_recovery']
    if desired == installed: return
    current = verify(installed['binding'])
    if current['version'] != 5 or desired['transaction_directory'] != installed['transaction_directory']:
        raise ValueError('Bootstrap differs from the restored appliance contract')
    if exact_path(desired['binding']['path']).parent != exact_path(installed['binding']['path']).parent:
        raise ValueError('Rollback bootstrap must retain the same helper pool')
    for name in MANAGED + tuple('headunit-' + n + '.service' for n in RESERVED_OWNER_UNITS):
        if owner.command(['systemctl', 'show', '--property=ActiveState', '--value', name]).stdout.strip() not in ('inactive', 'failed'):
            raise ValueError('Bootstrap rollback alignment requires stopped owners')
    target = verify(desired['binding'])
    if (current['uid'], current['gid'], current['administrator_uid']) != (target['uid'], target['gid'], target['administrator_uid']):
        raise ValueError('Rollback bootstrap must retain the same owner accounts')
    _write(generator_path(owner.units), generator_bytes(desired, owner.units), 0o555)
    owner.command(['systemctl', 'daemon-reload'])


def admission(owner, descriptor=None, migrate=False):
    """Keep first activation cold; preserve an already successful warm gate.

    Run under the existing transaction lock before recovery or selection writes.
    Live owners never authorize recovery; an active/exited gate means recovery
    already completed at boot, and unchanged reloads preserve that admission.
    """
    if owner.lock is None: raise RuntimeError('Acquire the appliance transaction lock first')
    installed = installed_descriptor(owner.units)
    if descriptor is None:
        descriptor = installed
        if descriptor is None: return  # Exact legacy install behavior before bootstrap.
    else:
        admit(descriptor, owner, check_only=True, migrate=migrate)
        if installed is not None and installed != descriptor:
            if not migrate:
                raise ValueError('Selection cannot change the stable recovery bootstrap')
            validate_migration(descriptor, owner)
            return
    if exact_path(descriptor['transaction_directory']) != owner.state:
        raise ValueError('Selection must retain the installed transaction owner')
    active = False
    for name in MANAGED + tuple('headunit-' + name + '.service' for name in RESERVED_OWNER_UNITS):
        state = owner.command(['systemctl', 'show', '--property=ActiveState', '--value', name]).stdout.strip()
        if state not in ('inactive', 'failed', 'active'):
            raise ValueError('Selection refuses a transitioning managed owner')
        active |= state == 'active'
    if pending_jobs(owner):
        raise ValueError('Selection requires no pending managed-owner jobs')
    if active:
        if installed is None:
            raise ValueError('First bootstrap activation requires all owners inactive; use a maintenance reboot boundary')
        status = owner.command(['systemctl', 'show', '--property=ActiveState', '--property=SubState',
                                '--property=Result', SERVICE]).stdout
        if len(status) > 1024 or set(status.splitlines()) != {'ActiveState=active', 'SubState=exited', 'Result=success'}:
            raise ValueError('Warm selection requires the successful active boot gate; use a maintenance reboot boundary')


def jobs(text):
    """Only fixed owner start jobs; numeric IDs are private transient state."""
    if type(text) is not str or len(text.encode()) > MAX_JOBS:
        raise ValueError('Managed job output exceeds bound')
    result = {}; ids = set()
    for line in text.splitlines():
        row = line.split()
        if (len(row) != 4 or not re.fullmatch('[0-9]{1,10}', row[0]) or int(row[0]) == 0 or
                row[0] in ids or row[1] not in MANAGED or row[2] != 'start' or
                row[3] not in ('waiting', 'running') or row[1] in result):
            raise ValueError('Unexpected managed boot job shape')
        result[row[1]] = row[0]; ids.add(row[0])
    return result


def pending_jobs(owner):
    return jobs(owner.command(['systemctl', 'list-jobs', '--no-legend', '--plain', '--no-pager', *MANAGED]).stdout)


def resolve(owner):
    desired = tuple(name for name in MANAGED if owner.enabled(name) == 'enabled')
    # daemon-reload during rollback can retire job IDs. Never cancel cached IDs.
    queued = pending_jobs(owner)
    cancelled = tuple(name for name in queued if name not in desired)
    if cancelled: owner.command(['systemctl', 'cancel', *(queued[name] for name in cancelled)])
    # Owners require this activating gate: synchronous start would deadlock.
    if desired: owner.command(['systemctl', '--no-block', 'start', *desired])
    if set(pending_jobs(owner)) != set(desired):
        raise ValueError('Restored boot jobs differ from the enabled owner set')
    return {'enabled': list(desired), 'cancelled': list(cancelled)}


def recover(descriptor, units, runner=subprocess.run):
    validate_descriptor(descriptor)
    verify_installed(descriptor, units)  # Full helper inventory before journal/job effects.
    with Transaction(units, descriptor['transaction_directory'], runner) as owner:
        # Boot admission cannot stop or compete with any live application owner.
        for name in MANAGED + tuple('headunit-' + name + '.service' for name in RESERVED_OWNER_UNITS):
            result = owner.command(['systemctl', 'show', '--property=ActiveState', '--value', name])
            if result.stdout.strip() not in ('inactive', 'failed'):
                raise ValueError('Boot recovery refuses an already running or transitioning owner')
        owner.recover_pending()
        align_boot_with_units(owner)
        from appliance_release import unit_binding
        current = {name: owner.current(name) for name in MANAGED}
        from appliance_transaction import _data
        binding = unit_binding({name: _data(record).decode() for name, record in current.items() if record['exists']}, owner.units)
        if binding: verify_units(binding, owner.units)
        return resolve(owner)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper-root', required=True)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--unit-directory', type=Path, required=True)
    parser.add_argument('--transaction-directory', required=True)
    args = parser.parse_args()
    if not sys.platform.startswith('linux') or os.getuid() != 0 or os.geteuid() != 0:
        parser.error('Boot recovery requires Linux PID1 service execution as root')
    try:
        # A direct invocation must never become a second live recovery supervisor.
        state = subprocess.run(['systemctl', 'show', '--property=ActiveState', '--property=MainPID', SERVICE],
                               check=True, capture_output=True, text=True, timeout=15).stdout
        if set(state.splitlines()) != {'ActiveState=activating', 'MainPID=' + str(os.getpid())}:
            raise ValueError('Boot recovery must run as the activating generated gate')
        result = recover({'binding': {'path': args.helper_root, 'sha256': args.manifest_sha256},
                          'transaction_directory': args.transaction_directory}, args.unit_directory)
        import json
        print(json.dumps(result), flush=True)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        parser.error(str(error))


if __name__ == '__main__': main()
