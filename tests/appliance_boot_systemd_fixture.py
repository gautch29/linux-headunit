# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Opt-in real PID1 job qualification; inert units, disposable rootless Podman only.

Run: python3 tests/appliance_boot_systemd_fixture.py --image sha256:LOCAL_IMAGE_ID
The locally loaded Linux image must contain /sbin/init (systemd), python3,
systemctl and systemd-analyze. No image is pulled and no host manager is used.
This tests the production retained generator/gate with inert legacy-like owners;
it does not qualify appliance binaries, radios or physical hardware.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import appliance_transaction as transaction
import appliance_release as release
import appliance_boot as boot

UNITS = Path('/etc/systemd/system')
STATE = Path('/var/lib/headunit-boot-fixture')
EVENTS = STATE / 'events.jsonl'
RECOVERY = boot.SERVICE
TARGET = 'fixture-boot.target'
CORE = transaction.MANAGED[:4]
CASES = ('optional-remove', 'optional-add', 'partial-write', 'rollback-pending',
         'complete', 'none', 'damaged', 'lock-conflict', 'helper-missing', 'helper-damaged', 'warm-selection')
FAILURES = ('damaged', 'lock-conflict', 'helper-missing', 'helper-damaged')
NONCE = 'HEADUNIT_BOOT_FIXTURE_NONCE'
DIAGNOSTIC_BYTES = 4096


def failed_output(label, error, redactions=()):
    """Bound retained output, never print command argv or an environment dump."""
    def bounded(value):
        raw = value if isinstance(value, bytes) else str(value or '').encode('utf-8', 'replace')
        for secret in redactions:
            if secret:
                raw = raw.replace(secret.encode(), b'<fixture-private>')
        return raw[-DIAGNOSTIC_BYTES:].decode('utf-8', 'ignore')
    print(json.dumps({'diagnostic': label, 'error': '' if isinstance(error, subprocess.CompletedProcess) else type(error).__name__,
                      'returncode': getattr(error, 'returncode', None),
                      'stdout': bounded(getattr(error, 'stdout', None)),
                      'stderr': bounded(getattr(error, 'stderr', None))}), file=sys.stderr, flush=True)


def case_diagnostics(case, name, error, nonce, mount):
    redactions = (nonce, str(mount))
    failed_output(case + ': failed command', error, redactions)
    # Dedicated inert fixture logs only. No inspect, host journal or environment.
    for label, args in (
            ('PID1 console', ['podman', 'logs', '--tail=40', name]),
            ('recovery unit', ['podman', 'exec', name, 'journalctl', '--no-pager',
                               '--output=cat', '--lines=40', '--unit=' + RECOVERY])):
        try:
            result = command(args, check=False)
        except (OSError, subprocess.SubprocessError) as unavailable:
            failed_output(case + ': ' + label, unavailable, redactions)
        else:
            failed_output(case + ': ' + label, result, redactions)


def command(args, **options):
    return subprocess.run(args, capture_output=True, text=True, timeout=15,
                          env=os.environ | {'LC_ALL': 'C', 'SYSTEMD_COLORS': '0'}, **options)


def boundary(platform, uid, mapping, container, nonce, expected, pid, init_name, boot=False):
    """Refuse the full host map and unexpected container/PID1 before writes."""
    rows = [line.split() for line in mapping.splitlines()]
    # uid_map names the parent namespace. The explicit private maps therefore
    # show parent rootless ID0 here, not the global host UID. Admit that exact
    # prepared form: outer host-root invocation is refused and rootless Podman
    # is checked before preparation; nonce/source/PID1 guards remain mandatory.
    nested = rows == [['0', '0', '1'], ['1', '1', '65536']]
    if (platform != 'linux' or uid != 0 or container != 'podman' or
            not re.fullmatch('[a-f0-9]{32}', nonce or '') or nonce != expected or
            not rows or len(rows[0]) != 3 or not all(x.isdecimal() for x in rows[0]) or
            int(rows[0][0]) != 0 or (int(rows[0][1]) == 0 and not nested) or int(rows[0][2]) != 1 or
            (boot and pid != 1) or (not boot and init_name != 'systemd')):
        raise RuntimeError('Fixture requires its disposable rootless Podman PID1 namespace')


def guard(boot=False):
    if sys.platform != 'linux':
        raise RuntimeError('Inner fixture requires its disposable Linux container')
    nonce = os.environ.get(NONCE, '')
    boundary(sys.platform, os.geteuid(), Path('/proc/self/uid_map').read_text(),
             os.environ.get('container'), nonce, Path('/fixture/nonce').read_text(),
             os.getpid(), Path('/proc/1/comm').read_text().strip(), boot)
    if Path(__file__).resolve() != Path('/fixture/tests/appliance_boot_systemd_fixture.py'):
        raise RuntimeError('Fixture source must come from the read-only isolated test mount')


def record(kind, **fields):
    data = (json.dumps({'kind': kind} | fields, separators=(',', ':')) + '\n').encode()
    fd = os.open(EVENTS, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_CLOEXEC, 0o600)
    try:
        if os.fstat(fd).st_size > 32768 or len(data) > 1024:
            raise RuntimeError('Fixture event bound exceeded')
        if os.write(fd, data) != len(data):
            raise RuntimeError('Fixture event write incomplete')
    finally:
        os.close(fd)


def events():
    if not EVENTS.exists():
        return []
    raw = EVENTS.read_bytes()
    if len(raw) > 65536:
        raise RuntimeError('Fixture event bound exceeded')
    return [json.loads(line) for line in raw.splitlines()]


def selection(case):
    if case not in CASES:
        raise ValueError('Unknown fixture case')
    previous = CORE + ('headunit-camera.service', 'headunit-phone-page.service')
    candidate = CORE + ('headunit-vehicle.service',)
    if case == 'optional-add':
        previous, candidate = candidate, previous
    return previous, candidate


def actor_unit(generation, name):
    return ('[Unit]\nDescription=Inert boot fixture owner\nDefaultDependencies=no\n'
            '[Service]\nType=oneshot\nRemainAfterExit=yes\nTimeoutStartSec=10\n'
            'ExecStartPre=/usr/bin/python3 -B -E -s /fixture/tests/appliance_boot_systemd_fixture.py '
            '--inner marker ' + generation + ' ' + name + ' preflight\n'
            'ExecStart=/usr/bin/python3 -B -E -s /fixture/tests/appliance_boot_systemd_fixture.py '
            '--inner marker ' + generation + ' ' + name + ' owner\n'
            '[Install]\nWantedBy=' + TARGET + '\n')


def auxiliary(case):
    lock = ('Requires=fixture-lock.service\nAfter=fixture-lock.service\n'
            if case == 'lock-conflict' else '')
    result = {
        TARGET: '[Unit]\nDescription=Isolated headunit boot qualification\nDefaultDependencies=no\n'
                'Wants=' + RECOVERY + '\nAfter=' + RECOVERY + '\n',
        RECOVERY + '.d/fixture.conf': '[Unit]\n' + lock + '[Service]\n'
                  'Environment=' + NONCE + '=' + os.environ.get(NONCE, '') + '\n'
                  'Environment=container=podman\n'
                  'ExecStartPre=/usr/bin/python3 -B -E -s /fixture/tests/appliance_boot_systemd_fixture.py --inner queued\n'
                  'ExecStartPost=/usr/bin/python3 -B -E -s /fixture/tests/appliance_boot_systemd_fixture.py --inner finished\n'
                  'ExecStopPost=/usr/bin/python3 -B -E -s /fixture/tests/appliance_boot_systemd_fixture.py --inner gate-ended\n',
    }
    # Environment is explicit: systemd service processes do not inherit PID1's environment.
    for name in transaction.MANAGED:
        result[name + '.d/fixture.conf'] = ('[Service]\nEnvironment=' + NONCE + '=' +
                                           os.environ.get(NONCE, '') + '\nEnvironment=container=podman\n')
    if case == 'lock-conflict':
        result['fixture-lock.service'] = (
            '[Unit]\nDefaultDependencies=no\n[Service]\nType=notify\nNotifyAccess=main\n'
            'TimeoutStartSec=10\nEnvironment=' + NONCE + '=' + os.environ.get(NONCE, '') +
            '\nEnvironment=container=podman\n'
            'ExecStart=/usr/bin/python3 -B -E -s /fixture/tests/appliance_boot_systemd_fixture.py --inner hold\n')
    return result


class PowerLoss(BaseException):
    pass


def prepare_boot():
    guard(boot=True)
    # Bootstrap is independent of candidate/legacy unit bytes and optional roles.
    # Stage and admit exactly the production helper/generator, before any journal.
    import pwd
    pool = Path('/opt/headunit-boot-helpers'); pool.mkdir(parents=True, mode=0o755)
    binding = release.stage({'version': 4, 'purpose': 'boot-recovery', 'boot_recovery': None,
        'execution': None, 'release_id': 'boot-helper-fixture', 'source_revision': 'a'*40,
        'repo': '/fixture', 'user': pwd.getpwnam('nobody').pw_name, 'configuration': {}, 'native_camera': None}, pool)
    with transaction.Transaction(UNITS, STATE, command) as owner:
        boot.admit({'binding': binding, 'transaction_directory': str(STATE)}, owner)
    return binding


def seed(case):
    guard(boot=True)
    if case not in CASES:
        raise ValueError('Unknown fixture case')
    STATE.mkdir(mode=0o700)
    # Refuse images with pre-existing managed owners, even inside this disposable namespace.
    for name in transaction.MANAGED:
        for directory in (UNITS, Path('/usr/lib/systemd/system'), Path('/lib/systemd/system'),
                          Path('/usr/local/lib/systemd/system'), Path('/run/systemd/system')):
            path = directory / name
            if path.exists() or path.is_symlink():
                raise RuntimeError('Image already contains a managed owner')
    for name, text in auxiliary(case).items():
        path = UNITS / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(0o644)
    binding = prepare_boot()
    previous, candidate = selection(case)
    old = {name: actor_unit('previous', name) for name in previous}
    new = {name: actor_unit('candidate', name) for name in candidate}
    for name, text in old.items():
        (UNITS / name).write_text(text)
        (UNITS / name).chmod(0o644)
        command(['systemctl', '--root=/', 'enable', name], check=True)

    def offline(args, **options):
        # Unit-file operations really update this container's install links before PID1
        # exists. Reload is deliberately unavailable at this crash-seeding boundary.
        if args == ['systemctl', 'daemon-reload']:
            return subprocess.CompletedProcess(args, 0, stdout='')
        if args[0] == 'systemctl':
            return command([args[0], '--root=/', *args[1:]], check=options.get('check', True))
        return command(args, check=options.get('check', True))

    if case != 'none':
        writer = transaction._write
        writes = 0
        def write(path, data, mode):
            nonlocal writes
            writer(path, data, mode)
            if path.parent == UNITS:
                writes += 1
                if case == 'partial-write' and writes == 2:
                    raise PowerLoss()
        def stage(args, **options):
            result = offline(args, **options)
            if args == ['systemctl', 'enable', candidate[-1]] and case != 'complete':
                raise PowerLoss()
            return result
        transaction._write = write
        try:
            with transaction.Transaction(UNITS, STATE, stage) as owner:
                owner.install(new)
        except PowerLoss:
            pass
        finally:
            transaction._write = writer
        snapshot = next(STATE.glob('transaction-*/journal.json')).parent
        if case == 'rollback-pending':
            def fail_reload(args, **options):
                if args == ['systemctl', 'daemon-reload']:
                    raise RuntimeError('Fixture interrupted rollback before enable restoration')
                return offline(args, **options)
            try:
                with transaction.Transaction(UNITS, STATE, fail_reload) as owner:
                    owner.rollback(snapshot)
            except RuntimeError:
                pass
            if json.loads((snapshot / 'journal.json').read_text())['state'] != 'rollback_pending':
                raise RuntimeError('Failed to seed the interrupted rollback boundary')
        if case == 'damaged':
            (snapshot / 'journal.json').write_text('{"version":999}')
            (snapshot / 'journal.json').chmod(0o600)
    if case in ('helper-missing', 'helper-damaged'):
        path = Path(binding['path']) / 'tools/appliance_release.py'
        if case == 'helper-missing':
            path.parent.chmod(0o755); path.unlink(); path.parent.chmod(0o555)
        else:
            path.chmod(0o644); path.write_bytes(b''); path.chmod(0o444)
    (STATE / 'case').write_text(case)
    record('seed', case=case)
    os.execv('/sbin/init', ['/sbin/init', '--unit=' + TARGET])


def jobs(text):
    """Parse only the seven selected unit rows; never cancel unrelated manager jobs."""
    if len(text) > 8192:
        raise RuntimeError('Fixture job output bound exceeded')
    result = {}
    for line in text.splitlines():
        row = line.split()
        if (len(row) != 4 or not row[0].isdecimal() or row[1] not in transaction.MANAGED or
                row[2] != 'start' or row[3] not in ('waiting', 'running') or row[1] in result):
            raise RuntimeError('Unexpected fixture managed-job shape')
        result[row[1]] = row[0]
    if len(result) > len(transaction.MANAGED):
        raise RuntimeError('Fixture managed-job bound exceeded')
    return result


def pending_jobs():
    return jobs(command(['systemctl', 'list-jobs', '--no-legend', '--plain', '--no-pager',
                         *transaction.MANAGED], check=True).stdout)


def resolve(owner, initial):
    desired = tuple(name for name in transaction.MANAGED if owner.enabled(name) == 'enabled')
    # Reload may have retired a job. Re-query numeric IDs rather than using IDs
    # captured before recovery, and explicitly check all discarded roles again.
    queued = pending_jobs()
    cancelled = tuple(name for name in queued if name not in desired)
    if cancelled:
        owner.command(['systemctl', 'cancel', *(queued[name] for name in cancelled)])
    # Never wait synchronously here: these jobs themselves require this activating gate.
    if desired:
        owner.command(['systemctl', '--no-block', 'start', *desired])
    after = pending_jobs()
    if set(after) != set(desired):
        raise RuntimeError('Restored boot jobs differ from the enabled owner set')
    record('resolved', enabled=list(desired), cancelled=list(cancelled),
           discarded=[name for name in initial if name not in desired],
           admitted=[name for name in desired if name not in initial])
    return desired


def gate_event(action):
    guard()
    if action == 'queued':
        record('queued', names=list(pending_jobs()))
    elif action == 'finished':
        initial = next(item['names'] for item in events() if item['kind'] == 'queued')
        with transaction.Transaction(UNITS, STATE) as owner:
            desired = [name for name in transaction.MANAGED if owner.enabled(name) == 'enabled']
            if set(pending_jobs()) != set(desired):
                raise RuntimeError('Production gate did not reconstruct restored jobs')
        # Observe retirement/admission only. Actual cancel calls are reported by
        # the production gate; reload may already have retired an initial job.
        record('resolved', enabled=desired, discarded=[name for name in initial if name not in desired],
               admitted=[name for name in desired if name not in initial])
        record('recovered')
    elif os.environ.get('SERVICE_RESULT') != 'success':
        record('failed', error='ProductionGateFailure')


def marker(generation, name, phase):
    guard()
    if generation not in ('previous', 'candidate') or name not in transaction.MANAGED or phase not in ('preflight', 'owner'):
        raise ValueError('Invalid inert owner marker')
    if not any(item['kind'] == 'recovered' for item in events()):
        raise RuntimeError('Owner started before successful recovery')
    record(phase, generation=generation, name=name)


def hold():
    guard()
    with transaction.Transaction(UNITS, STATE):
        address = os.environ['NOTIFY_SOCKET']
        if address.startswith('@'):
            address = '\0' + address[1:]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
            sender.connect(address)
            sender.sendall(b'READY=1')
        time.sleep(60)  # One bounded competing lock fixture, destroyed with its container.


def assess(case, rows):
    previous, candidate = selection(case)
    expected = set(candidate if case == 'complete' else previous)
    actors = [item for item in rows if item['kind'] in ('preflight', 'owner')]
    if case in FAILURES:
        if not any(item['kind'] == 'failed' for item in rows) or actors:
            raise AssertionError('Failed recovery allowed a startup effect')
        return set()
    recovered = [i for i, item in enumerate(rows) if item['kind'] == 'recovered']
    if len(recovered) != 1:
        raise AssertionError('Expected one successful recovery gate')
    generation = 'candidate' if case == 'complete' else 'previous'
    for phase in ('preflight', 'owner'):
        selected = [item for item in actors if item['kind'] == phase]
        if (len(selected) != len(expected) or {item['name'] for item in selected} != expected or
                any(item['generation'] != generation for item in selected)):
            raise AssertionError('Missing, duplicate or competing generation startup effect')
    if any(i <= recovered[0] for i, item in enumerate(rows) if item in actors):
        raise AssertionError('Startup effect ran before recovery gate completed')
    for name in expected:
        phases = [item['kind'] for item in actors if item['name'] == name]
        if phases != ['preflight', 'owner']:
            raise AssertionError('Owner ran before its startup preflight')
    resolved = [item for item in rows if item['kind'] == 'resolved']
    if len(resolved) != 1 or set(resolved[0]['enabled']) != expected:
        raise AssertionError('Restored enabled roles were not resolved explicitly')
    if case in ('optional-remove', 'optional-add'):
        if set(resolved[0]['discarded']) != set(candidate) - expected:
            raise AssertionError('Removed optional boot jobs were not resolved')
        if set(resolved[0]['admitted']) != expected - set(candidate):
            raise AssertionError('Restored optional boot jobs were not admitted')
    return expected



def warm_selection():
    """Actual PID1 warm admission: inert owners only, never production radios.

    Briefly remove/reload the fixture generator to reproduce first activation
    with legacy owners already live; restore it atomically in guaranteed cleanup.
    A later inactive gate is reproduced by retiring those dependencies first,
    stopping only the independent inert gate, then restoring the generated graph.
    """
    guard()
    path = boot.generator_path(UNITS)
    original = path.read_bytes()
    descriptor = boot.installed_descriptor(UNITS)
    initial = events()
    journals = {item: item.read_bytes() for item in STATE.glob('transaction-*/journal.json')}
    live = selection('warm-selection')[0]
    def unchanged():
        if {item: item.read_bytes() for item in STATE.glob('transaction-*/journal.json')} != journals:
            raise AssertionError('Refused warm admission changed a journal')
        if events() != initial:
            raise AssertionError('Refused warm admission caused an owner/recovery effect')
    # First activation is forbidden even if this manager remembers an older gate.
    with transaction.Transaction(UNITS, STATE) as owner:
        path.unlink()
        try:
            try: boot.admission(owner, descriptor)
            except ValueError as error:
                if 'all owners inactive' not in str(error): raise
            else: raise AssertionError('First bootstrap admitted active legacy owners')
            if path.exists(): raise AssertionError('Refused admission activated a generator')
            unchanged()
        finally:
            transaction._write(path, original, 0o555)
    # Successful active/exited admission survives selection reload and rollback.
    updated = {name: actor_unit('candidate', name) for name in live}
    snapshot = transaction.install(updated, UNITS, STATE)
    if events() != initial:
        raise AssertionError('Warm selection implicitly started an owner')
    receiver = 'headunit-receiver.service'
    command(['systemctl', 'restart', receiver], check=True)
    additions = events()[len(initial):]
    if [(item['kind'], item.get('generation'), item.get('name')) for item in additions] != [
            ('preflight', 'candidate', receiver), ('owner', 'candidate', receiver)]:
        raise AssertionError('Warm manager restart reran recovery or started competing owners')
    transaction.rollback(snapshot, UNITS, STATE)
    if len(events()) != len(initial) + 2:
        raise AssertionError('Warm rollback implicitly started an owner')
    command(['systemctl', 'restart', receiver], check=True)
    additions = events()[len(initial) + 2:]
    if [(item['kind'], item.get('generation'), item.get('name')) for item in additions] != [
            ('preflight', 'previous', receiver), ('owner', 'previous', receiver)]:
        raise AssertionError('Legacy rollback restart did not preserve successful boot admission')
    # Reproduce the inactive-gate/live-owner boundary without weakening recovery.
    path.unlink()
    try:
        command(['systemctl', 'daemon-reload'], check=True)
        for name in live:
            requires = command(['systemctl', 'show', '--property=Requires', '--value', name], check=True).stdout.split()
            if RECOVERY in requires:
                raise AssertionError('Fixture gate dependency was not retired')
        command(['systemctl', 'stop', RECOVERY], check=True)
    finally:
        transaction._write(path, original, 0o555)
        command(['systemctl', 'daemon-reload'], check=True)
    for name in live:
        if command(['systemctl', 'show', '--property=ActiveState', '--value', name], check=True).stdout.strip() != 'active':
            raise AssertionError('Fixture preparation stopped an inert live owner')
    initial = events()
    journals = {item: item.read_bytes() for item in STATE.glob('transaction-*/journal.json')}
    for operation in (lambda: transaction.install(updated, UNITS, STATE),
                      lambda: transaction.rollback(snapshot, UNITS, STATE),
                      lambda: transaction.recover_pending(UNITS, STATE)):
        try: operation()
        except ValueError as error:
            if 'successful active boot gate' not in str(error): raise
        else: raise AssertionError('Inactive gate admitted a live selection or recovery')
        unchanged()
        if path.read_bytes() != original:
            raise AssertionError('Refused warm selection changed the stable generator')
    print(json.dumps({'warmAdmission': 'qualified-isolated-PID1',
                      'firstLiveRefused': True, 'activeGateRestartAndRollback': True,
                      'inactiveGateLiveInstallRollbackRecoveryRefused': True}))


def observe():
    guard()
    case = (STATE / 'case').read_text()
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        rows = events()
        if case in FAILURES and any(item['kind'] == 'failed' for item in rows):
            # After the gate has failed, all originally queued actor jobs must retire.
            if not pending_jobs():
                break
        elif case not in FAILURES and sum(item['kind'] == 'owner' for item in rows) == len(
                selection(case)[1 if case == 'complete' else 0]):
            break
        time.sleep(0.1)
    else:
        raise RuntimeError('Fixture boot completion deadline exceeded')
    expected = assess(case, events())
    recovery_state = command(['systemctl', 'show', RECOVERY, '--property=ActiveState', '--value'], check=True).stdout.strip()
    if recovery_state != ('failed' if case in FAILURES else 'active'):
        raise AssertionError('Recovery service state does not match job evidence')
    if case not in FAILURES:
        generation = 'candidate' if case == 'complete' else 'previous'
        with transaction.Transaction(UNITS, STATE) as owner:
            for name in transaction.MANAGED:
                if ((owner.enabled(name) == 'enabled') != (name in expected) or
                        (UNITS / name).exists() != (name in expected) or
                        (name in expected and (UNITS / name).read_text() != actor_unit(generation, name))):
                    raise AssertionError('Restored unit content/enable state differs')
            for path in STATE.glob('transaction-*/journal.json'):
                value = owner.journal(path.parent)
                if value['state'] != ('complete' if case == 'complete' else 'rolled_back'):
                    raise AssertionError('Recovery journal not durably resolved')
    if case == 'warm-selection': warm_selection()
    print(json.dumps({'case': case, 'qualified': 'isolated-systemd-jobs', 'owners': len(expected)}))


def container_command(image, name, mount, nonce, case):
    if not re.fullmatch('sha256:[a-f0-9]{64}', image):
        raise ValueError('Use an immutable locally loaded image ID')
    return ['podman', 'run', '--detach', '--pull=never', '--name', name,
            '--network=none', '--pid=private', '--ipc=private',
            '--uidmap=0:0:1', '--uidmap=1:1:65536',
            '--gidmap=0:0:1', '--gidmap=1:1:65536',
            '--cgroupns=private', '--systemd=always', '--timeout=90',
            '--volume', str(mount) + ':/fixture:ro,Z', '--env', NONCE + '=' + nonce,
            '--env', 'container=podman',
            '--entrypoint', '/usr/bin/python3', image,
            '-B', '-E', '-s', '/fixture/tests/appliance_boot_systemd_fixture.py', '--inner', 'seed', case]


def prepare_mount(mount):
    """Normalize the private read-only snapshot, independent of caller umask.

    A rootless caller may use umask0002: mkdir defaults then produce group-
    writable directories even though the eventual bind mount is read-only.
    Production release admission checks inode permissions, so seal the copied
    first-party inputs rather than weakening that check for the fixture.
    """
    for directory in ('tests', 'tools', 'deploy', 'deploy/cm5'):
        path = mount / directory
        path.mkdir(mode=0o755)
        path.chmod(0o755)
    sources = {Path('tests') / Path(__file__).name: Path(__file__),
               **{Path('tools') / name: ROOT / 'tools' / name for name in release.BOOT_HELPER_TOOLS},
               Path(release.BOOT_GENERATOR_TEMPLATE): ROOT / release.BOOT_GENERATOR_TEMPLATE}
    for relative, source in sources.items():
        target = mount / relative
        shutil.copyfile(source, target)
        target.chmod(0o444)


def qualify(image):
    if sys.platform != 'linux' or os.geteuid() == 0:
        raise RuntimeError('Run the outer fixture as an ordinary user on Linux with rootless Podman')
    info = json.loads(command(['podman', 'info', '--format=json'], check=True).stdout)
    if info.get('host', {}).get('security', {}).get('rootless') is not True:
        raise RuntimeError('Rootless Podman is required; no host-root manager path is allowed')
    with tempfile.TemporaryDirectory(prefix='headunit-boot-fixture-') as folder:
        mount = Path(folder)
        prepare_mount(mount)
        nonce = uuid.uuid4().hex
        (mount / 'nonce').write_text(nonce)
        for case in CASES:
            name = 'headunit-boot-fixture-' + uuid.uuid4().hex
            try:
                command(container_command(image, name, mount, nonce, case), check=True)
                ready = time.monotonic() + 15
                while command(['podman', 'exec', name, '/usr/bin/test', '-S',
                               '/run/systemd/private'], check=False).returncode:
                    if time.monotonic() >= ready:
                        raise RuntimeError('Container PID1 manager did not become available')
                    time.sleep(0.1)
                # exec waits for the real PID1 jobs; infrastructure errors fail, never skip.
                result = subprocess.run(['podman', 'exec', name, '/usr/bin/python3', '-B', '-E', '-s',
                                         '/fixture/tests/appliance_boot_systemd_fixture.py', '--inner', 'observe'],
                                        capture_output=True, text=True, timeout=75, check=True)
                print(result.stdout.strip(), flush=True)
            except Exception as error:
                case_diagnostics(case, name, error, nonce, mount)
                # Suppress CalledProcessError's argv (including private fixture
                # environment arguments); the bounded streams above explain it.
                raise RuntimeError('Fixture case ' + case + ' failed; see bounded diagnostics') from None
            finally:
                command(['podman', 'rm', '--force', '--ignore', name], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image')
    parser.add_argument('--inner', nargs='+', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.inner:
        action, *values = args.inner
        if action == 'seed' and len(values) == 1:
            seed(values[0])
        elif action in ('queued', 'finished', 'gate-ended') and not values:
            gate_event(action)
        elif action == 'marker' and len(values) == 3:
            marker(*values)
        elif action == 'hold' and not values:
            hold()
        elif action == 'observe' and not values:
            observe()
        else:
            parser.error('Invalid inner action')
    elif args.image:
        try:
            qualify(args.image)
        except (OSError, subprocess.SubprocessError) as error:
            failed_output('fixture setup/cleanup command', error)
            raise RuntimeError('Fixture command failed; see bounded diagnostics') from None
    else:
        parser.error('--image is required; real systemd infrastructure is never silently skipped')


if __name__ == '__main__':
    main()
