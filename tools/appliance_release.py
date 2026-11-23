# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Versioned local application releases. Secrets and live state stay external.

The root installer copies only named first-party programs and typed static
configuration roles. A release binding pins the manifest bytes, not a mutable
``current`` symlink. This is a local administrator workflow, not OTA signing.
"""
import argparse
import ast
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import stat
import struct
import subprocess
import tempfile

MAX_MANIFEST = 65536
MAX_BINARY = 128 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024
MAX_RELEASES = 32
PUBLICATION_RESERVE = 16 * 1024 * 1024
ROLES = {
    'receiver': ('receiver.json', 16384), 'displays': ('displays.conf', 16384),
    'labwc': ('labwc-cm5-dual-rc.xml', 65536),
    'outputs': ('kanshi-cm5-dual.conf', 65536),
    'ultra': ('installed-ui.conf', 8192),
    'vehicle_profile': ('vehicle-profile.plist', 65536),
    'warnings': ('warnings.json', 65536), 'climate': ('climate.json', 65536),
    'car_settings': ('car-settings.json', 65536),
    'fanout': ('vehicle-fanout.json', 4096), 'camera': ('camera.json', 8192),
}
LEGACY_ROLES = frozenset(ROLES)
ROLES.update({'phone_manager': ('phone-manager.json', 16384),
              'standard_displays': ('standard-displays.conf', 16384),
              'phone_page': ('phone-page.json', 16384),
              'native_cluster': ('native-cluster.conf', 4096)})
CAMERA_ROLES = {'camera', 'camera_state', 'camera_manual'}
ROLES.update({'camera_state': ('camera-state.json', 65536), 'camera_manual': ('camera-manual.json', 16384)})
REQUIRED_ROLES = {'receiver', 'displays', 'labwc', 'outputs'}
RECEIVER_ROLES = {'display_config': 'displays', 'ultra_installed_ui': 'ultra',
                  'climate_config': 'climate', 'car_settings_config': 'car_settings'}
RECEIVER_ROLES['native_cluster_config'] = 'native_cluster'
CAMERA_RECEIVER_ROLES = {'camera_state_config': 'camera_state', 'camera_manual_profile': 'camera_manual'}
UI_ROLES = {'vehicleProfile': 'vehicle_profile', 'warningOverlayProfile': 'warnings',
            'cameraProfile': 'camera'}
TOOLS = ('run_lab_receiver.py', 'receiver_configuration.py', 'generate_test_ap.py',
         'media_bridge.py', 'media_state.py', 'phone_state.py', 'bluez_phones.py', 'carplay_reconnect.py',
         'request_carplay_connection.py', 'wait_wayland.py', 'vehicle_fanout.py',
         'appliance_release.py')
LEGACY_MANAGER_TOOLS = ('phone_manager.py', 'phone_manager_boot.py', 'phone_manager_configuration.py',
                 'phone_manager_endpoint.py', 'phone_pairing.py', 'cluster_page.py',
                 'cluster_layout.py', 'audio_control.py', 'camera_manual.py',
                 'install_cm5_appliance.py', 'appliance_transaction.py')
PRE_DRIVE_MANAGER_TOOLS = LEGACY_MANAGER_TOOLS + ('private_state.py', 'carplay_readiness.py')
DRIVE_MANAGER_TOOLS = ('drive_mode_profiles.py',)
MANAGER_TOOLS = PRE_DRIVE_MANAGER_TOOLS + DRIVE_MANAGER_TOOLS
LEGACY_RESIDENT_TOOLS = ('phone_page_boot.py', 'phone_page.py', 'phone_page_configuration.py',
                         'phone_page_presentation.py', 'phone_icons.py', 'main_surface_handoff.py')
HOME_PRESENTATION_TOOLS = ('phone_home.py', 'phone_page_style.py', 'phone_glass.py')
SPORT_PAGE_TOOLS = ('phone_page_sport.py',)
RESIDENT_TOOLS = LEGACY_RESIDENT_TOOLS + HOME_PRESENTATION_TOOLS + SPORT_PAGE_TOOLS
SPORT_TOOLS = ('sport_telemetry.py', 'sport_source.py')
PREPHONE_TOOLS = ('prephone_native.py', 'prephone_consumer.py')
INDICATOR_TOOLS = ('prephone_indicator_status.py',)
AMBIENT_TOOLS = ('ambient_light_status.py', 'ambient_light_model.py')
BOOT_HELPER_TOOLS = ('appliance_release.py', 'appliance_transaction.py',
                     'install_cm5_appliance.py', 'receiver_configuration.py',
                     'vehicle_fanout.py', 'phone_manager_boot.py',
                     'phone_manager_configuration.py', 'phone_page_configuration.py',
                     'appliance_boot.py')
BOOT_GENERATOR_TEMPLATE = 'deploy/cm5/headunit-boot-recovery-generator.in'

CORE_BINARIES = ('headunit-control-probe', 'headunit-airplay-advertiser',
                 'headunit-cluster-wayland', 'headunit-display-config-check',
                 'headunit-identity-config-check')
UNIT_NAMES = ('compositor', 'outputs', 'cluster', 'receiver', 'vehicle', 'camera')
# Known optional units are always inspected. Only the explicit versioned
# managed variant admits the resident page's sealed graph.
UNSEALED_UNIT_NAMES = ('phone-page',)
# A managed graph has exactly one receiver service. Inspect these reserved
# alternate owner names in managed releases, but never transaction-own or remove them.
RESERVED_OWNER_UNITS = ('managed-receiver', 'phone-manager')


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate release field')
        result[key] = value
    return result


def exact_path(value):
    if (not isinstance(value, (str, Path)) or not str(value) or
            re.search(r'[^A-Za-z0-9_./-]', str(value))):
        raise ValueError('Release paths must contain no service syntax')
    raw = str(value)
    path = Path(value)
    if not path.is_absolute() or raw != os.path.normpath(raw):
        raise ValueError('Release paths must be exact absolute paths')
    return path


def ancestors(path, owners):
    """No linked or replaceable ancestors, including above the release pool."""
    path = exact_path(path)
    owners = set(owners) | {0}
    if path.resolve() != path:
        raise ValueError('Release path contains a symlink')
    for parent in (path, *path.parents):
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid not in owners:
            raise ValueError('Release directory has an untrusted owner or type')
        # An owned entry beneath a root-owned sticky temporary directory is
        # protected against another user's rename (also useful in fixtures).
        if info.st_mode & 0o022 and not (info.st_uid == 0 and info.st_mode & stat.S_ISVTX):
            raise ValueError('Release directory is writable by another account')


def read_file(path, maximum, owners, private=False):
    path = exact_path(path)
    ancestors(path.parent, owners)
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid not in owners or
                info.st_mode & (0o077 if private else 0o022) or not 0 < info.st_size <= maximum):
            raise ValueError('Release input has an unsafe type, owner, mode, links or size')
        chunks = []; remaining = info.st_size
        while remaining:
            chunk = os.read(fd, min(remaining, 1024 * 1024))
            if not chunk:
                raise ValueError('Release input changed while reading')
            chunks.append(chunk); remaining -= len(chunk)
        final = os.fstat(fd)
        if (os.read(fd, 1) or (info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) !=
                (final.st_ino, final.st_size, final.st_mtime_ns, final.st_ctime_ns)):
            raise ValueError('Release input changed while reading')
        return b''.join(chunks), info
    finally:
        os.close(fd)


def sync(directory):
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try: os.fsync(fd)
    finally: os.close(fd)


def config_path(root, role):
    return root / 'config' / ROLES[role][0]


def binaries(roles, native_camera):
    result = list(CORE_BINARIES)
    if 'ultra' in roles:
        result.append('headunit-installed-ui-probe')
    if {'climate', 'car_settings', 'native_cluster', 'camera_state'} & set(roles):
        result.append('headunit-vehicle-config-check')
    if native_camera:
        result.append('headunit-camera-wayland')
    return result


def inventory(roles, native_camera, execution=None, version=3, purpose=None, sport_enabled=False, prephone_enabled=False, *, trip_history_enabled=False, ambient_enabled=False, indicators_enabled=False):
    if type(indicators_enabled) is not bool:
        raise ValueError('Invalid pre-phone indicator inventory flag')
    if purpose == 'boot-recovery' and indicators_enabled:
        raise ValueError('Indicator publication requires an application generation')
    if purpose == 'boot-recovery':
        helper_tools = BOOT_HELPER_TOOLS + (('punchthrough_configuration.py', 'camera_manual.py') if version == 5 else ())
        return {**{'tools/' + name: 0o444 for name in helper_tools},
                BOOT_GENERATOR_TEMPLATE: 0o444}
    managed_tools = LEGACY_MANAGER_TOOLS if version == 2 else MANAGER_TOOLS
    tools = TOOLS + (managed_tools if execution else ())
    if trip_history_enabled:
        if not sport_enabled:
            raise ValueError('Trip history requires sport telemetry')
        tools += ('trip_history.py',)
    if sport_enabled:
        if not execution or version not in (3, 4, 5):
            raise ValueError('Sport telemetry requires a managed application generation')
        tools += SPORT_TOOLS
    if prephone_enabled:
        if not execution or version not in (3, 4, 5):
            raise ValueError('Native publication requires a managed application generation')
        tools += PREPHONE_TOOLS
    if indicators_enabled:
        if not prephone_enabled or not execution or version not in (3, 4, 5):
            raise ValueError('Pre-phone indicators require managed native publication')
        tools += INDICATOR_TOOLS
    if ambient_enabled:
        if not prephone_enabled or not execution or version not in (3, 4, 5):
            raise ValueError('Ambient software model requires managed native publication')
        tools += AMBIENT_TOOLS
    if version in (4, 5): tools += ('appliance_boot.py',)
    if version == 5: tools += ('punchthrough_configuration.py',)
    templates = UNIT_NAMES + (('managed-receiver',) if execution else ())
    if execution and execution['resident_home']:
        tools += RESIDENT_TOOLS
        templates += ('phone-page',)
    return {**{'tools/' + name: 0o444 for name in tools},
            **{'build/' + name: 0o555 for name in binaries(roles, native_camera)},
            **{'deploy/cm5/headunit-' + name + '.service.in': 0o444 for name in templates},
            **{'config/' + ROLES[role][0]: 0o440 for role in roles}}


def validate_recipe(value):
    fields = {'version', 'release_id', 'source_revision', 'repo', 'user', 'configuration', 'native_camera'}
    version = value.get('version') if isinstance(value, dict) else None
    if type(version) is int and version in (2, 3, 4, 5):
        fields.add('execution')
    if version in (4, 5): fields |= {'purpose', 'boot_recovery'}
    helper = version in (4, 5) and value.get('purpose') == 'boot-recovery'
    if (not isinstance(value, dict) or set(value) !=
            fields or type(version) is not int or version not in (1, 2, 3, 4, 5) or
            not isinstance(value['release_id'], str) or
            not re.fullmatch(r'[a-z0-9][a-z0-9_-]{7,63}', value['release_id']) or
            not isinstance(value['source_revision'], str) or
            not re.fullmatch(r'[0-9a-f]{40}', value['source_revision']) or
            not isinstance(value['user'], str) or
            not re.fullmatch(r'[a-z_][a-z0-9_-]{0,31}', value['user']) or
            not isinstance(value['configuration'], dict) or
            not (set(value['configuration']) == set() if helper else
                 REQUIRED_ROLES <= set(value['configuration']) <= (LEGACY_ROLES if version == 1 else set(ROLES)))):
        raise ValueError('Invalid versioned application recipe')
    if version in (4, 5):
        if value['purpose'] not in ('appliance', 'boot-recovery'):
            raise ValueError('Invalid release purpose')
        if helper:
            if value['execution'] is not None or value['boot_recovery'] is not None or value['native_camera'] is not None:
                raise ValueError('Recovery helper cannot contain application execution or configuration')
        else:
            from appliance_boot import validate_descriptor
            validate_descriptor(value['boot_recovery'])
    if version in (2, 3, 4, 5) and not helper:
        execution = value['execution']
        if (type(execution) is not dict or set(execution) != ({'receiver', 'resident_home', 'mode_policy'} if version in (4, 5) else {'receiver', 'resident_home'}) or
                execution['receiver'] != 'phone-manager' or type(execution['resident_home']) is not bool or
                (version in (4, 5) and execution['mode_policy'] != 'phone-authoritative') or
                not {'phone_manager', 'standard_displays'} <= set(value['configuration']) or
                ('phone_page' in value['configuration']) != execution['resident_home'] or
                value['native_camera'] is not None or (version != 5 and 'camera' in value['configuration'])):
            raise ValueError('Invalid managed application execution or configuration roles')
    if version != 5 and {'camera_state', 'camera_manual'} & set(value['configuration']):
        raise ValueError('Signed camera roles require a version 5 managed release')
    if version == 5 and not helper and set(value['configuration']) & CAMERA_ROLES and not CAMERA_ROLES <= set(value['configuration']):
        raise ValueError('Signed camera requires the complete sealed camera-owner graph')
    native = value['native_camera']
    if native is not None:
        if not isinstance(native, dict) or set(native) != {'request', 'lease'} or 'camera' not in value['configuration']:
            raise ValueError('Native camera needs a profile and explicit request/lease paths')
        for path in native.values(): exact_path(path)
        if native['request'] == native['lease']:
            raise ValueError('Native camera endpoints must differ')
    exact_path(value['repo'])
    paths = [exact_path(path) for path in value['configuration'].values()]
    if len(set(paths)) != len(paths):
        raise ValueError('Static configuration roles must name distinct files')
    return value


def read_recipe(path):
    # Root is explicitly handed this local recipe. Read it without executing
    # anything, then require its owner to match the selected display account.
    owner = Path(path).lstat().st_uid
    raw, info = read_file(path, MAX_MANIFEST, {0, os.getuid(), owner}, True)
    value = validate_recipe(json.loads(raw, object_pairs_hook=unique))
    if info.st_uid not in (0, pwd.getpwnam(value['user']).pw_uid):
        raise ValueError('Release recipe must belong to root or its display account')
    return value


def ui_fields(raw):
    result = {}
    text = raw.decode('utf-8')
    if '\r' in text or '\0' in text: raise ValueError('Invalid installed UI profile characters')
    for line in text.splitlines():
        key, separator, value = line.partition('=')
        if not separator or not key or not value or key in result or '\0' in line or '\r' in line:
            raise ValueError('Invalid installed UI profile in release')
        result[key] = value
    return result


def relocate(recipe, raw, final, pool):
    """Close the typed configuration graph; never replace arbitrary text paths."""
    if recipe.get('purpose') == 'boot-recovery': return {}
    from receiver_configuration import parse_receiver_configuration, PATHS
    sources = {role: exact_path(path) for role, path in recipe['configuration'].items()}
    # Loading resolved paths from the *captured* bytes avoids rereading a
    # changing source receiver file. See parse_receiver_configuration below.
    settings = parse_receiver_configuration(raw['receiver'], sources['receiver'])
    signed_camera = recipe['version'] == 5 and CAMERA_ROLES <= set(sources)
    receiver_roles = RECEIVER_ROLES | (CAMERA_RECEIVER_ROLES if signed_camera else {})
    if not signed_camera and 'camera_state_config' in settings:
        raise ValueError('Ultra camera capabilities require a sealed camera-owner graph; application-release support is not yet implemented')
    if any(key in settings for key in ('runtime_status_file', 'runtime_status_nonce')):
        raise ValueError('Receiver readiness identity is derived per launch, never sealed as static configuration')
    execution = recipe.get('execution')
    # The first managed release graph is camera-free. Manual/native/signed
    # camera owners need their own sealed activation and lease graph.
    if not signed_camera and 'camera_manual_profile' in settings:
        raise ValueError('Manual camera profiles require the native phone manager; application-release support is not yet implemented')
    if execution and any(key in settings for key in ('standard_camera_control_file', 'standard_camera_lease_file')):
        raise ValueError('Managed application releases cannot include Standard camera routes')
    has_page_pair = any(key in settings for key in ('native_page_control_file', 'native_page_lease_file'))
    if has_page_pair and (not execution or not execution['resident_home']):
        raise ValueError('Application release page handoff requires its enabled resident Home role')
    used = set(REQUIRED_ROLES)
    external = []
    for key, role in receiver_roles.items():
        if key in settings:
            if role not in sources or settings[key] != sources[role]:
                raise ValueError('Static receiver reference is not supplied by its release role: ' + key)
            settings[key] = config_path(final, role); used.add(role)
        elif role in sources:
            raise ValueError('Unused receiver configuration role: ' + role)
    if settings.get('bin_dir') != Path(recipe['repo']) / 'build' or settings.get('seconds') != 0:
        raise ValueError('Release receiver must use the selected build and run without a time limit')
    settings['bin_dir'] = final / 'build'
    external.extend(value for key, value in settings.items() if key in PATHS and key not in receiver_roles and key != 'bin_dir')
    if 'ultra' in sources:
        fields = ui_fields(raw['ultra'])
        if execution and any(key in fields for key in (('cameraProfile', 'cameraControlFile') if not signed_camera else ()) + (
                                                       'nativeCameraControlFile', 'nativeCameraLeaseFile')):
            raise ValueError('Managed application releases cannot include native or signed camera routes')
        if signed_camera:
            from punchthrough_configuration import validate_graph
            validate_graph(settings, fields, raw['camera'], raw['camera_state'], raw['camera_manual'])
        for key, role in UI_ROLES.items():
            if key in fields:
                if role not in sources or exact_path(fields[key]) != sources[role]:
                    raise ValueError('Static Ultra reference is not supplied by its release role: ' + key)
                fields[key] = str(config_path(final, role)); used.add(role)
        if 'vehicleProfile' not in fields:
            raise ValueError('Ultra release needs its vehicle profile')
        for key in ('assetDirectory', 'acceptanceReference', 'vehicleSocket', 'clusterLayoutFile',
                    'displayResponseFile', 'startupWarningLightsFile', 'greetingSequenceFile', 'driveModeProfilesFile',
                    'cameraControlFile', 'cameraStatusFile', 'nativeCameraControlFile', 'nativeCameraLeaseFile',
                    'nativePageControlFile', 'nativePageLeaseFile'):
            if key in fields: external.append(exact_path(fields[key]))
        if fields.get('greetingSequenceFile', '') != str(settings.get('greeting_sequence_file', '')):
            raise ValueError('Cluster greeting endpoints must agree between receiver and installed UI')
        page_fields = {'nativePageControlFile': 'native_page_control_file', 'nativePageLeaseFile': 'native_page_lease_file'}
        if any(key in fields for key in page_fields):
            if not has_page_pair or any(fields.get(key) != str(settings.get(setting, '')) for key, setting in page_fields.items()):
                raise ValueError('Installed UI page handoff differs from the receiver pair')
        if recipe['native_camera']:
            if fields.get('cameraProfile') or fields.get('nativeCameraControlFile') != recipe['native_camera']['request'] or \
                    fields.get('nativeCameraLeaseFile') != recipe['native_camera']['lease']:
                raise ValueError('Native camera release must share the guarded Ultra endpoints')
        raw['ultra'] = ''.join(key + '=' + value + '\n' for key, value in fields.items()).encode()
    if 'native_cluster' in sources:
        native_fields = dict(line.split('=', 1) for line in raw['native_cluster'].decode().splitlines() if line)
        if 'drive_mode_profiles_file' in native_fields:
            external.append(exact_path(native_fields['drive_mode_profiles_file']))
            if 'ultra' in sources and fields.get('driveModeProfilesFile') != native_fields['drive_mode_profiles_file']:
                raise ValueError('Native and signed drive-mode profiles must share one private endpoint')
    if 'fanout' in sources:
        fanout = json.loads(raw['fanout'], object_pairs_hook=unique)
        if not isinstance(fanout, dict) or not isinstance(fanout.get('consumers'), dict):
            raise ValueError('Invalid release fan-out configuration')
        external.extend(exact_path(path) for path in [fanout.get('input'), *fanout['consumers'].values()])
        used.add('fanout')
    if recipe['native_camera']:
        if 'ultra' not in sources: raise ValueError('Native camera needs a guarded Ultra renderer')
        external.extend(exact_path(path) for path in recipe['native_camera'].values()); used.add('camera')
    if execution:
        from phone_manager_configuration import validate_manager_settings
        manager = json.loads(raw['phone_manager'], object_pairs_hook=unique)
        manager = validate_manager_settings(manager, settings)
        if manager['enabled'] is not True or exact_path(manager['standardDisplayConfig']) != sources['standard_displays']:
            raise ValueError('Managed release needs its enabled profile and exact separate Standard display role')
        if signed_camera:
            manual = json.loads(raw['camera_manual'], object_pairs_hook=unique)
            state_root = exact_path(manager['stateDirectory'])
            endpoints = [manual[key] for key in ('requestFile', 'ownerFile', 'statusFile', 'controlSocket')]
            if any(exact_path(path).parent != state_root / 'camera' for path in endpoints):
                raise ValueError('Signed camera endpoints must belong to the manager camera directory')
            external.extend(exact_path(path) for path in endpoints)
        external.append(exact_path(manager['stateDirectory']))
        manager['standardDisplayConfig'] = str(config_path(final, 'standard_displays'))
        used.update(('phone_manager', 'standard_displays'))
        raw['phone_manager'] = (json.dumps(manager, indent=2) + '\n').encode()
        if execution['resident_home']:
            from phone_page_configuration import validate_configuration, validate_surface_settings
            page = validate_configuration(json.loads(raw['phone_page'], object_pairs_hook=unique))
            if page['enabled'] is not True or page['managerSocket'] != manager['stateDirectory'] + '/manager.sock':
                raise ValueError('Resident release needs its enabled page and exact managed endpoint')
            validate_surface_settings(page, manager, settings)
            external.append(exact_path(page['managerSocket']))
            external.extend(exact_path(page[key]) for key in ('surfaceControlFile', 'surfaceLeaseFile'))
            used.add('phone_page')
            raw['phone_page'] = (json.dumps(page, indent=2) + '\n').encode()
    if used != set(sources):
        raise ValueError('Unreferenced static configuration role in release')
    for path in external:
        path = exact_path(path)
        if path.is_relative_to(pool) or path in sources.values():
            raise ValueError('Assets, secrets and mutable endpoints must stay outside all application releases')
    raw['receiver'] = (json.dumps(settings, default=str, indent=2) + '\n').encode()
    for role, data in raw.items():
        if len(data) > ROLES[role][1]: raise ValueError('Relocated configuration exceeds its reader bound')
    return raw


def elf_aarch64(data):
    if (len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01' or
            struct.unpack_from('<HHI', data, 16)[0] not in (2, 3) or
            struct.unpack_from('<HHI', data, 16)[1:] != (183, 1)):
        raise ValueError('Application binary must be a Linux AArch64 ELF executable')


def require_compatible_boot_helper(application, helper):
    """New publication/activation needs recovery that understands its schema.

    Payload integrity verification remains available for retained rejected
    candidates and existing older application/helper pairs.
    """
    if helper['version'] < application['version']:
        raise ValueError('Application recovery helper schema predates the application')


def publication_capacity(pool, sizes):
    """Admit the complete copy without consuming reserved filesystem blocks."""
    space = os.statvfs(pool)
    block = space.f_frsize or space.f_bsize
    if block <= 0 or space.f_bavail < 0:
        raise ValueError('Release filesystem capacity is unavailable')
    # Six directories (including the staging root), with room for directory
    # growth. File sizes include the exact manifest, each rounded separately.
    required = sum(((size + block - 1) // block) * block for size in sizes)
    required += 8 * block + PUBLICATION_RESERVE
    available = space.f_bavail * block
    if available < required:
        raise ValueError(f'Insufficient release space: need {required} bytes, have {available}; archive unused inputs explicitly')
    # Some filesystems allocate inodes dynamically and report no fixed total.
    if space.f_files and space.f_favail < len(sizes) + 6 + 16:
        raise ValueError('Insufficient release filesystem inodes')


def stage(recipe, pool):
    """Publish a complete sealed directory; no service or current pointer changes."""
    recipe = validate_recipe(recipe)
    account = pwd.getpwnam(recipe['user'])
    if account.pw_uid == 0:
        raise ValueError('The appliance display account must be unprivileged')
    if os.geteuid() != 0 and (account.pw_uid, account.pw_gid) != (os.getuid(), os.getgid()):
        raise ValueError('Staging for another account requires root')
    if recipe['version'] == 2:
        raise ValueError('New managed publication requires version 3; retained version 2 releases remain verifiable')
    pool = exact_path(pool); ancestors(pool, {0, os.getuid()})
    if recipe.get('purpose') == 'appliance':
        helper = verify(recipe['boot_recovery']['binding'])
        helper_root = exact_path(recipe['boot_recovery']['binding']['path'])
        if helper.get('purpose') != 'boot-recovery' or pool in helper_root.parents or helper_root in pool.parents:
            raise ValueError('Application bootstrap must pin an independently retained recovery helper')
        require_compatible_boot_helper(recipe, helper)
        state = exact_path(recipe['boot_recovery']['transaction_directory'])
        if state == pool or pool in state.parents:
            raise ValueError('Recovery journals must stay outside application release pool')
    if pool.stat().st_uid != os.getuid() or pool.stat().st_mode & 0o022:
        raise ValueError('Release pool must be owned by its administrator')
    lock = os.open(pool / '.release.lock', os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    stage_path = None
    try:
        info = os.fstat(lock)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1 or stat.S_IMODE(info.st_mode) != 0o600:
            raise ValueError('Unsafe application release lock')
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        final = pool / recipe['release_id']
        if final.exists() or final.is_symlink(): raise ValueError('Release identifier already exists')
        if len(list(pool.iterdir())) >= MAX_RELEASES + 1:
            raise ValueError('Release retention limit reached; archive unused releases explicitly')
        repo = exact_path(recipe['repo']); ancestors(repo, {0, os.getuid(), account.pw_uid})
        raw = {role: read_file(path, ROLES[role][1], {0, os.getuid(), account.pw_uid},
                              role in ('receiver', 'ultra'))[0]
               for role, path in recipe['configuration'].items()}
        raw = relocate(recipe, raw, final, pool)
        records = []; total = 0
        for relative, mode in inventory(raw, recipe['native_camera'], recipe.get('execution'), recipe['version'], recipe.get('purpose'),
                                        json.loads(raw['receiver']).get('sport_telemetry', False) if 'receiver' in raw else False,
                                        prephone_enabled=bool(json.loads(raw['receiver']).get('prephone_publication_socket')) if 'receiver' in raw else False,
                                        trip_history_enabled=json.loads(raw['receiver']).get('trip_history', False) if 'receiver' in raw else False,
                                        ambient_enabled=json.loads(raw['receiver']).get('ambient_light', False) if 'receiver' in raw else False,
                                        indicators_enabled=json.loads(raw['receiver']).get('prephone_indicators', False) if 'receiver' in raw else False).items():
            role = next((key for key in raw if relative == 'config/' + ROLES[key][0]), None)
            if role:
                data = raw[role]
            else:
                maximum = MAX_BINARY if relative.startswith('build/') else 131072
                data, info = read_file(repo / relative, maximum, {0, os.getuid(), account.pw_uid})
                if relative.startswith('build/'):
                    if not info.st_mode & 0o111: raise ValueError('Source application binary is not executable')
                    elf_aarch64(data)
            total += len(data)
            if total > MAX_TOTAL: raise ValueError('Application release exceeds total size bound')
            uid, gid = os.getuid(), account.pw_gid if role else os.getgid()
            records.append({'path': relative, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                            'mode': mode, 'uid': uid, 'gid': gid})
        manifest = {'version': recipe['version'], 'release_id': recipe['release_id'], 'root': str(final),
                    'source_revision': recipe['source_revision'], 'user': recipe['user'],
                    'uid': account.pw_uid, 'gid': account.pw_gid, 'administrator_uid': os.getuid(),
                    'configuration': sorted(raw), 'native_camera': recipe['native_camera'], 'files': records}
        if recipe['version'] in (2, 3, 4, 5):
            manifest['execution'] = dict(recipe['execution']) if recipe['execution'] is not None else None
        if recipe['version'] in (4, 5):
            manifest.update(purpose=recipe['purpose'], boot_recovery=recipe['boot_recovery'])
        data = (json.dumps(manifest, separators=(',', ':')) + '\n').encode()
        if len(data) > MAX_MANIFEST: raise ValueError('Release manifest exceeds bound')
        publication_capacity(pool, [record['size'] for record in records] + [len(data)])
        stage_path = Path(tempfile.mkdtemp(prefix='.stage-', dir=pool))
        for name in ('tools', 'build', 'config', 'deploy', 'deploy/cm5'):
            (stage_path / name).mkdir(mode=0o700)
        # Only retain one bounded input in memory. Pin the admitted byte graph
        # across the second read so changed sources cannot outgrow the plan.
        for record in records:
            relative = record['path']
            role = next((key for key in raw if relative == 'config/' + ROLES[key][0]), None)
            if role:
                payload = raw[role]
            else:
                maximum = MAX_BINARY if relative.startswith('build/') else 131072
                payload, _ = read_file(repo / relative, maximum, {0, os.getuid(), account.pw_uid})
            if len(payload) != record['size'] or hashlib.sha256(payload).hexdigest() != record['sha256']:
                raise ValueError('Release input changed after capacity admission')
            fd = os.open(stage_path / relative, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
            with os.fdopen(fd, 'wb') as out:
                out.write(payload)
                if os.geteuid() == 0: os.fchown(out.fileno(), record['uid'], record['gid'])
                os.fchmod(out.fileno(), record['mode']); out.flush(); os.fsync(out.fileno())
        fd = os.open(stage_path / 'manifest.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o444)
        with os.fdopen(fd, 'wb') as out:
            os.fchmod(out.fileno(), 0o444); out.write(data); out.flush(); os.fsync(out.fileno())
        for name in ('deploy/cm5', 'deploy', 'tools', 'build', 'config', '.'):
            path = stage_path / name
            if name == 'config' and os.geteuid() == 0: os.chown(path, os.getuid(), account.pw_gid)
            path.chmod(0o750 if name == 'config' else 0o555); sync(path)
        # The owned, non-writable-by-others pool plus flock excludes competing
        # installers; never reuse or replace a published release identifier.
        os.rename(stage_path, final); stage_path = None; sync(pool)
        return {'path': str(final), 'sha256': hashlib.sha256(data).hexdigest()}
    finally:
        if stage_path is not None:
            for directory, _, _ in os.walk(stage_path): os.chmod(directory, 0o700)
            shutil.rmtree(stage_path)
        os.close(lock)


def verify(binding):
    if (not isinstance(binding, dict) or set(binding) != {'path', 'sha256'} or
            not isinstance(binding['sha256'], str) or not re.fullmatch(r'[0-9a-f]{64}', binding['sha256'])):
        raise ValueError('Invalid application release binding')
    root = exact_path(binding['path'])
    data, info = read_file(root / 'manifest.json', MAX_MANIFEST, {0, os.getuid()})
    if stat.S_IMODE(info.st_mode) != 0o444 or hashlib.sha256(data).hexdigest() != binding['sha256']:
        raise ValueError('Application release manifest changed')
    value = json.loads(data, object_pairs_hook=unique)
    fields = {'version', 'release_id', 'root', 'source_revision', 'user', 'uid', 'gid',
              'administrator_uid', 'configuration', 'native_camera', 'files'}
    if type(value) is dict and type(value.get('version')) is int and value['version'] in (2, 3, 4, 5):
        fields.add('execution')
    if type(value) is dict and value.get('version') in (4, 5): fields |= {'purpose', 'boot_recovery'}
    if (not isinstance(value, dict) or set(value) != fields or type(value['version']) is not int or value['version'] not in (1, 2, 3, 4, 5) or
            value['root'] != str(root) or value['release_id'] != root.name or
            value['administrator_uid'] != info.st_uid or
            type(value['administrator_uid']) is not int or
            type(value['uid']) is not int or type(value['gid']) is not int or
            not isinstance(value['configuration'], list) or
            any(not isinstance(role, str) for role in value['configuration']) or
            len(set(value['configuration'])) != len(value['configuration']) or
            not isinstance(value['files'], list)):
        raise ValueError('Invalid application release manifest')
    # Reuse the recipe schema without treating an asserted revision as proof
    # that an ELF file was reproducibly built from that commit.
    recipe = {'version': value['version'], 'release_id': root.name, 'source_revision': value['source_revision'],
                     'repo': root, 'user': value['user'],
                     'configuration': {role: str(config_path(root, role)) for role in value['configuration'] if role in ROLES},
                     'native_camera': value['native_camera']}
    if value['version'] in (2, 3, 4, 5):
        recipe['execution'] = value['execution']
    if value['version'] in (4, 5):
        recipe.update(purpose=value['purpose'], boot_recovery=value['boot_recovery'])
    validate_recipe(recipe)
    if set(value['configuration']) - set(ROLES): raise ValueError('Unknown release configuration role')
    account = pwd.getpwnam(value['user'])
    if (value['uid'], value['gid']) != (account.pw_uid, account.pw_gid):
        raise ValueError('Application release display account changed')
    sport_enabled = False
    prephone_enabled = False
    trip_history_enabled = False
    ambient_enabled = False
    indicators_enabled = False
    if 'receiver' in value['configuration']:
        # Admit the optional graph only from the hash-bound receiver bytes.
        try:
            receiver_raw, _ = read_file(config_path(root, 'receiver'), ROLES['receiver'][1], {info.st_uid})
        except OSError as error:
            raise ValueError('Optional sport graph receiver configuration is unavailable') from error
        receiver_records = [record for record in value['files'] if isinstance(record, dict) and record.get('path') == 'config/receiver.json']
        if (len(receiver_records) != 1 or
                receiver_records[0].get('sha256') != hashlib.sha256(receiver_raw).hexdigest()):
            raise ValueError('Optional sport graph receiver configuration is not hash-bound')
        receiver_values = json.loads(receiver_raw, object_pairs_hook=unique)
        if not isinstance(receiver_values, dict) or type(receiver_values.get('sport_telemetry', False)) is not bool:
            raise ValueError('Invalid sport telemetry configuration')
        if type(receiver_values.get('trip_history', False)) is not bool:
            raise ValueError('Invalid trip history configuration')
        trip_history_enabled = receiver_values.get('trip_history', False)
        sport_enabled = receiver_values.get('sport_telemetry', False)
        prephone_enabled = bool(receiver_values.get('prephone_publication_socket'))
        if type(receiver_values.get('ambient_light', False)) is not bool:
            raise ValueError('Invalid ambient software configuration')
        ambient_enabled = receiver_values.get('ambient_light', False)
        if type(receiver_values.get('prephone_indicators', False)) is not bool:
            raise ValueError('Invalid pre-phone indicator configuration')
        indicators_enabled = receiver_values.get('prephone_indicators', False)
    expected = inventory(value['configuration'], value['native_camera'], value.get('execution'), value['version'], value.get('purpose'), sport_enabled, prephone_enabled=prephone_enabled, trip_history_enabled=trip_history_enabled, ambient_enabled=ambient_enabled, indicators_enabled=indicators_enabled)
    # The retained classic Home graph has the same release schema. Its exact
    # pinned inventory must remain readable during forward unit migration.
    # New publications always seal all three presentation modules; a partial
    # new graph cannot fall back to the classic inventory.
    presentation = {'tools/' + name for name in HOME_PRESENTATION_TOOLS}
    declared = {record.get('path') for record in value['files']
                if isinstance(record, dict) and isinstance(record.get('path'), str)}
    if (value.get('execution') or {}).get('resident_home') and not declared & presentation:
        expected = {name: mode for name, mode in expected.items() if name not in presentation}
    sport_page = {'tools/' + name for name in SPORT_PAGE_TOOLS}
    if (value.get('execution') or {}).get('resident_home') and not declared & sport_page:
        # Retained Home2/classic pages have no sport import. Inspect only their
        # hash-bound source; a current page missing its dependency must fail.
        page_raw, _ = read_file(root / 'tools/phone_page.py', 131072, {info.st_uid})
        records = [record for record in value['files'] if isinstance(record, dict) and
                   record.get('path') == 'tools/phone_page.py']
        if len(records) != 1 or records[0].get('sha256') != hashlib.sha256(page_raw).hexdigest():
            raise ValueError('Predecessor page is not hash-bound')
        try:
            imports_sport = any(
                isinstance(node, ast.ImportFrom) and node.module == 'phone_page_sport' or
                isinstance(node, ast.Import) and any(alias.name == 'phone_page_sport' for alias in node.names)
                for node in ast.walk(ast.parse(page_raw)))
        except (SyntaxError, ValueError) as error:
            raise ValueError('Invalid predecessor page source') from error
        if imports_sport:
            raise ValueError('Current page requires the complete sport page module graph')
        expected = {name: mode for name, mode in expected.items() if name not in sport_page}
    drive_modules = {'tools/' + name for name in DRIVE_MANAGER_TOOLS}
    if value.get('execution') and value['version'] >= 3 and not declared & drive_modules:
        # Only an exact predecessor graph may omit the new unconditional
        # manager dependency. Inspect its hash-bound bytes without executing it;
        # a damaged current manager cannot masquerade as that predecessor.
        manager_raw, _ = read_file(root / 'tools/phone_manager.py', 131072, {info.st_uid})
        records = [record for record in value['files'] if isinstance(record, dict) and
                   record.get('path') == 'tools/phone_manager.py']
        if len(records) != 1 or records[0].get('sha256') != hashlib.sha256(manager_raw).hexdigest():
            raise ValueError('Predecessor manager is not hash-bound')
        try:
            nodes = ast.walk(ast.parse(manager_raw))
            imports_drive = any(
                isinstance(node, ast.ImportFrom) and node.module == 'drive_mode_profiles' or
                isinstance(node, ast.Import) and any(alias.name == 'drive_mode_profiles' for alias in node.names)
                for node in nodes)
        except (SyntaxError, ValueError) as error:
            raise ValueError('Invalid predecessor manager source') from error
        if imports_drive:
            raise ValueError('Current manager requires the complete drive module graph')
        expected = {name: mode for name, mode in expected.items() if name not in drive_modules}
    directories = {'', 'tools', 'build', 'config', 'deploy', 'deploy/cm5'}
    found = set()
    allowed = directories | set(expected) | {'manifest.json'}
    for relative in sorted(directories):
        directory = root / relative
        directory_info = directory.lstat()
        if (not stat.S_ISDIR(directory_info.st_mode) or directory_info.st_uid != info.st_uid or
                stat.S_IMODE(directory_info.st_mode) != (0o750 if relative == 'config' else 0o555) or
                (relative == 'config' and directory_info.st_gid != value['gid'])):
            raise ValueError('Application release directory metadata changed')
        found.add(relative)
        with os.scandir(directory) as entries:
            for entry in entries:
                child = str(Path(entry.path).relative_to(root))
                if child not in allowed or entry.is_symlink():
                    raise ValueError('Unexpected or linked content in application release')
                found.add(child)
    if found != allowed:
        raise ValueError('Application release has missing or extra content')
    if len(value['files']) != len(expected): raise ValueError('Invalid release file inventory')
    seen = set(); total = 0
    configuration_raw = {}
    role_files = {'config/' + ROLES[role][0]: role for role in value['configuration']}
    for record in value['files']:
        if (not isinstance(record, dict) or set(record) != {'path', 'size', 'sha256', 'mode', 'uid', 'gid'} or
                not isinstance(record['path'], str) or record['path'] not in expected or record['path'] in seen or
                type(record['size']) is not int or type(record['mode']) is not int or
                type(record['uid']) is not int or type(record['gid']) is not int or
                not isinstance(record['sha256'], str) or not re.fullmatch(r'[0-9a-f]{64}', record['sha256']) or
                record['mode'] != expected[record['path']] or record['uid'] != info.st_uid):
            raise ValueError('Invalid release file record')
        seen.add(record['path'])
        maximum = MAX_BINARY if record['path'].startswith('build/') else 131072
        raw, current = read_file(root / record['path'], maximum, {info.st_uid})
        if (record['size'] != len(raw) or record['sha256'] != hashlib.sha256(raw).hexdigest() or
                (record['uid'], record['gid'], record['mode']) !=
                (current.st_uid, current.st_gid, stat.S_IMODE(current.st_mode)) or
                (record['path'].startswith('config/') and current.st_gid != value['gid'])):
            raise ValueError('Application release file changed: ' + record['path'])
        if record['path'].startswith('build/'): elf_aarch64(raw)
        if record['path'] in role_files:
            configuration_raw[role_files[record['path']]] = raw
        total += len(raw)
        if total > MAX_TOTAL: raise ValueError('Application release exceeds total size bound')
    if value['version'] in (2, 3, 4, 5):
        # Recheck the captured typed graph, not an arbitrary later reread of
        # member paths. Runtime state remains outside the entire release pool.
        relocate(recipe, configuration_raw, root, root.parent)
    if value.get('purpose') == 'appliance':
        helper = verify(value['boot_recovery']['binding'])
        if helper.get('purpose') != 'boot-recovery':
            raise ValueError('Application boot binding must select an independent recovery helper')
        helper_root = exact_path(value['boot_recovery']['binding']['path'])
        state = exact_path(value['boot_recovery']['transaction_directory'])
        if (root.parent in helper_root.parents or helper_root in root.parent.parents or
                state == root.parent or root.parent in state.parents or helper_root.parent in state.parents):
            raise ValueError('Recovery helper and runtime journals must stay outside application release pool')
    return value


def configuration_member(path, binding, role):
    """Grant the sealed-config reader path only for an exact verified member."""
    value = verify(binding)
    return role in value['configuration'] and exact_path(path) == config_path(Path(binding['path']), role)


def read_configuration_member(path, binding, role):
    """Return captured member bytes checked against the bound inventory hash."""
    value = verify(binding)
    return _read_verified_member(path, binding, role, value)


def _read_verified_member(path, binding, role, value):
    root = Path(binding['path'])
    if role not in value['configuration'] or exact_path(path) != config_path(root, role):
        raise ValueError('Configuration is not an exact member of its verified release')
    relative = 'config/' + ROLES[role][0]
    record = next(record for record in value['files'] if record['path'] == relative)
    raw, info = read_file(path, ROLES[role][1], {value['administrator_uid']})
    if ((info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode), len(raw)) !=
            (record['uid'], record['gid'], record['mode'], record['size']) or
            hashlib.sha256(raw).hexdigest() != record['sha256']):
        raise ValueError('Configuration changed after release verification')
    return raw


def read_configuration_members(binding):
    """Capture one bounded configuration graph between full integrity checks.

    No verification result survives this call. Each member still has its own
    bounded trusted read and exact inventory hash/metadata check.
    """
    value = verify(binding)
    root = Path(binding['path'])
    captured = {role: _read_verified_member(config_path(root, role), binding, role, value)
                for role in value['configuration']}
    if verify(binding) != value:
        raise ValueError('Application release changed during configuration capture')
    return value, captured


def guard_units(units, binding, unit_directory=Path('/etc/systemd/system')):
    value = verify(binding)
    execution = value.get('execution')
    if not execution and {'headunit-' + name + '.service' for name in UNSEALED_UNIT_NAMES} & set(units):
        raise ValueError('Native phone pages are not supported by application releases')
    root = exact_path(binding['path'])
    command = ('ExecStartPre=+/usr/bin/python3 -B -E -s ' + str(root / 'tools/appliance_release.py') +
               ' --verify ' + str(root) + ' --manifest-sha256 ' + binding['sha256'] +
               ' --unit-directory ' + str(exact_path(unit_directory)))
    result = {}
    for name, text in units.items():
        if text.count('[Service]\n') != 1 or ' --manifest-sha256 ' in text:
            raise ValueError('Invalid service template for sealed application release')
        if execution:
            text = re.sub(r'(?m)^(ExecStart(?:Pre)?=\+?)/usr/bin/python3 (?:-B -E -s )?',
                          r'\1/usr/bin/python3 -B -E -s ', text)
        else:
            text = text.replace('/usr/bin/python3 ', '/usr/bin/python3 -B -E -s ')
        for script in ('run_lab_receiver.py', 'vehicle_fanout.py'):
            path = str(root / 'tools' / script)
            text = text.replace(path + ' --config', path + ' --release-root ' + str(root) +
                                ' --release-manifest-sha256 ' + binding['sha256'] + ' --config')
        if execution:
            for script in ('phone_manager.py', 'phone_page_boot.py'):
                path = str(root / 'tools' / script)
                text = text.replace(path + ' ', path + ' --release-root ' + str(root) +
                                    ' --release-manifest-sha256 ' + binding['sha256'] + ' ')
        result[name] = text.replace('[Service]\n', '[Service]\n' + command + '\n')
    return result


def command_options(tokens, counts):
    result = {}; at = 0
    while at < len(tokens):
        name = tokens[at]; at += 1
        if name not in counts or name in result or at + counts[name] > len(tokens):
            raise ValueError('Unknown, repeated or incomplete bound service option')
        result[name] = tokens[at:at + counts[name]]; at += counts[name]
    return result


def manager_arguments(tokens, root, validation=False):
    counts = {'--manager-profile': 1, '--uid': 1, '--vehicle-fanout-config': 1}
    if validation: counts['--validate-only'] = 0
    options = command_options(tokens, counts)
    required = {'--manager-profile', '--uid'} | ({'--validate-only'} if validation else set())
    if (not required <= set(options) or
            options['--manager-profile'] != [str(config_path(root, 'phone_manager'))] or
            not re.fullmatch(r'[1-9][0-9]{0,9}', options['--uid'][0]) or
            int(options['--uid'][0]) >= (1 << 32) - 1 or
            ('--vehicle-fanout-config' in options and options['--vehicle-fanout-config'] != [str(config_path(root, 'fanout'))])):
        raise ValueError('Invalid bound manager service configuration paths or UID')


def resident_arguments(tokens, root, validation=False):
    counts = {'--profile': 1, '--manager-profile': 1, '--receiver-config': 1}
    if validation: counts['--validate-only'] = 0
    options = command_options(tokens, counts)
    if (set(options) != set(counts) or options['--profile'] != [str(config_path(root, 'phone_page'))] or
            options['--manager-profile'] != [str(config_path(root, 'phone_manager'))] or
            options['--receiver-config'] != [str(config_path(root, 'receiver'))]):
        raise ValueError('Invalid bound resident service configuration paths')


def unit_binding(units, unit_directory=None):
    """Extract/structurally check bindings without reading release payloads.

    Journals contain both generations, including a possibly damaged candidate.
    Full managed admission is performed separately by verify_unit_bundle on
    activation and on the rollback target, never the non-target generation.
    """
    bindings = []
    directories = []
    pattern = re.compile(r'ExecStartPre=\+/usr/bin/python3 -B -E -s ([A-Za-z0-9_./-]+)/tools/appliance_release.py --verify ([A-Za-z0-9_./-]+) --manifest-sha256 ([0-9a-f]{64}) --unit-directory ([A-Za-z0-9_./-]+)')
    for text in units.values():
        lines = [line for line in text.splitlines() if ' --manifest-sha256 ' in line and line.startswith('ExecStartPre=')]
        if not lines: bindings.append(None); continue
        if len(lines) != 1: raise ValueError('Ambiguous service release guard')
        match = pattern.fullmatch(lines[0])
        if not match or match[1] != match[2]: raise ValueError('Invalid service release guard')
        directories.append(exact_path(match[4]))
        bindings.append({'path': str(exact_path(match[2])), 'sha256': match[3]})
    if not bindings or all(binding is None for binding in bindings): return None
    if any(binding != bindings[0] for binding in bindings):
        raise ValueError('Managed services must select one coherent application release')
    if len(set(directories)) != 1 or (unit_directory is not None and directories[0] != exact_path(unit_directory)):
        raise ValueError('Managed service guards must inspect their actual unit directory')
    binding = bindings[0]
    root = Path(binding['path'])
    python = '/usr/bin/python3 -B -E -s '
    reader = ' --release-root ' + str(root) + ' --release-manifest-sha256 ' + binding['sha256'] + ' --config '
    manager_prefix = python + str(root / 'tools/phone_manager.py') + reader + str(config_path(root, 'receiver'))
    receiver_starts = [line.removeprefix('ExecStart=') for line in units.get('headunit-receiver.service', '').splitlines()
                       if line.startswith('ExecStart=')]
    managed = bool(len(receiver_starts) == 1 and receiver_starts[0].split()[:len(manager_prefix.split())] == manager_prefix.split())
    if not managed and {'headunit-' + name + '.service' for name in UNSEALED_UNIT_NAMES} & set(units):
        raise ValueError('Native phone pages are not supported by direct application releases')
    commands = {
        'headunit-compositor.service': '/usr/bin/labwc -c ' + str(config_path(root, 'labwc')),
        'headunit-outputs.service': '/usr/bin/kanshi -c ' + str(config_path(root, 'outputs')),
        'headunit-cluster.service': str(root / 'build/headunit-cluster-wayland'),
        'headunit-receiver.service': python + str(root / 'tools/run_lab_receiver.py') + reader + str(config_path(root, 'receiver')),
        'headunit-vehicle.service': python + str(root / 'tools/vehicle_fanout.py') + reader + str(config_path(root, 'fanout')),
        'headunit-camera.service': str(root / 'build/headunit-camera-wayland'),
    }
    if managed:
        if 'headunit-camera.service' in units:
            raise ValueError('Managed application releases cannot include camera services')
        commands['headunit-receiver.service'] = manager_prefix
        commands['headunit-phone-page.service'] = (python + str(root / 'tools/phone_page_boot.py') +
            ' --release-root ' + str(root) + ' --release-manifest-sha256 ' + binding['sha256'])
    if not {'headunit-' + name + '.service' for name in UNIT_NAMES[:4]} <= set(units) <= set(commands):
        raise ValueError('A bound service bundle needs the four core services')
    for name, text in units.items():
        starts = [line.removeprefix('ExecStart=') for line in text.splitlines() if line.startswith('ExecStart=')]
        if len(starts) != 1:
            raise ValueError('Service application paths differ from their release binding')
        tokens = starts[0].split(); prefix = commands[name].split()
        if tokens[:len(prefix)] != prefix:
            raise ValueError('Service application paths differ from their release binding')
        remaining = tokens[len(prefix):]
        if managed and name == 'headunit-receiver.service':
            manager_arguments(remaining, root)
        elif managed and name == 'headunit-phone-page.service':
            resident_arguments(remaining, root)
        elif name in ('headunit-cluster.service', 'headunit-camera.service'):
            if name == 'headunit-cluster.service':
                options = command_options(remaining, {'--size': 2, '--output': 1, '--background': 0,
                    '--simulate': 0, '--simulate-dated': 0, '--vehicle-socket': 1, '--media-file': 1, '--page-file': 1,
                    '--local-ultra-profile': 1, '--greeting-sequence-file': 1, '--prephone-publication-socket': 1, '--ambient-light-software': 0, '--prephone-indicators': 0})
                if not {'--size', '--output', '--background'} <= set(options) or len({'--simulate', '--simulate-dated', '--vehicle-socket'} & set(options)) > 1:
                    raise ValueError('Invalid bound cluster service arguments')
                if '--local-ultra-profile' in options and (
                        options['--local-ultra-profile'] != [str(config_path(root, 'ultra'))] or
                        options['--size'] != ['1920', '720']):
                    raise ValueError('Local gauges must use their sealed Ultra profile and authored canvas')
            else:
                options = command_options(remaining, {'--camera-profile': 1, '--display-config': 1,
                                                     '--request-file': 1, '--surface-lease-file': 1})
                if (set(options) != {'--camera-profile', '--display-config', '--request-file', '--surface-lease-file'} or
                        options['--camera-profile'] != [str(config_path(root, 'camera'))] or
                        options['--display-config'] != [str(config_path(root, 'displays'))]):
                    raise ValueError('Invalid bound camera service configuration paths')
        elif remaining:
            raise ValueError('Unexpected arguments after bound application/configuration')
        for line in text.splitlines():
            if line.startswith('ExecStartPre=/usr/bin/python3'):
                tokens = line.removeprefix('ExecStartPre=').split()
                wait = (python + str(root / 'tools/wait_wayland.py')).split()
                dry_run = (python + str(root / 'tools/run_lab_receiver.py') + reader + str(config_path(root, 'receiver')) + ' --dry-run').split()
                if tokens[:len(wait)] == wait:
                    command_options(tokens[len(wait):], {'--socket': 1, '--output': 1})
                elif tokens != dry_run:
                    if managed:
                        # Full exact order/count/content is checked at admission.
                        # This structural path deliberately needs no payload.
                        manager_validation = manager_prefix.split()
                        page_validation = commands['headunit-phone-page.service'].split()
                        if tokens[:len(manager_validation)] == manager_validation:
                            manager_arguments(tokens[len(manager_validation):], root, True)
                            continue
                        if tokens[:len(page_validation)] == page_validation:
                            resident_arguments(tokens[len(page_validation):], root, True)
                            continue
                    raise ValueError('Service preflight paths differ from their release binding')
    return binding


def verify_unit_bundle(binding, units, unit_directory=Path('/etc/systemd/system')):
    """Admit one complete bound variant before activation or target rollback."""
    if (type(units) is not dict or not units or
            any(type(name) is not str or type(text) is not str or '\0' in text or
                not 0 < len(text.encode()) <= 65536 for name, text in units.items())):
        raise ValueError('Invalid bound service bundle')
    value = verify(binding)
    if value.get('purpose') == 'boot-recovery':
        raise ValueError('Recovery helpers cannot be selected as an application unit bundle')
    if value.get('purpose') == 'appliance':
        require_compatible_boot_helper(value, verify(value['boot_recovery']['binding']))
    expected = {'headunit-' + name + '.service' for name in UNIT_NAMES[:4]}
    if 'fanout' in value['configuration']: expected.add('headunit-vehicle.service')
    if value['native_camera']: expected.add('headunit-camera.service')
    execution = value.get('execution')
    if execution and execution['resident_home']: expected.add('headunit-phone-page.service')
    if set(units) != expected or unit_binding(units, unit_directory) != binding:
        raise ValueError('Service bundle does not select one complete application release variant')
    if not execution and any(str(Path(binding['path']) / 'tools/phone_manager.py') in line
                             for line in units['headunit-receiver.service'].splitlines()
                             if line.startswith('ExecStart=')):
        raise ValueError('Version-one application releases do not contain a native manager graph')
    from receiver_configuration import parse_receiver_configuration
    root = Path(binding['path'])
    config = config_path(root, 'receiver')
    settings = parse_receiver_configuration(read_configuration_member(config, binding, 'receiver'), config)
    cluster_start = next(line for line in units['headunit-cluster.service'].splitlines()
                         if line.startswith('ExecStart='))
    local_gauges = '--local-ultra-profile' in cluster_start.split()
    cluster_options = command_options(cluster_start.removeprefix('ExecStart=').split()[1:],
        {'--size': 2, '--output': 1, '--background': 0, '--simulate': 0, '--simulate-dated': 0, '--vehicle-socket': 1,
         '--media-file': 1, '--page-file': 1, '--local-ultra-profile': 1, '--greeting-sequence-file': 1, '--prephone-publication-socket': 1, '--ambient-light-software': 0, '--prephone-indicators': 0})
    if ('--prephone-indicators' in cluster_options) != settings.get('prephone_indicators', False):
        raise ValueError('Pre-phone indicators differ from sealed receiver configuration')
    if ('--ambient-light-software' in cluster_options) != settings.get('ambient_light', False):
        raise ValueError('Ambient software owner differs from sealed receiver configuration')
    if ('--simulate-dated' in cluster_options) != settings.get('simulate_dated_vehicle', False):
        raise ValueError('Dated simulation differs from sealed receiver configuration')
    if cluster_options.get('--prephone-publication-socket', []) != (
            [str(settings['prephone_publication_socket'])] if settings.get('prephone_publication_socket') else []):
        raise ValueError('Native publication differs from sealed receiver configuration')
    if cluster_options.get('--greeting-sequence-file', []) != (
            [str(settings['greeting_sequence_file'])] if settings.get('greeting_sequence_file') else []):
        raise ValueError('Cluster greeting preference differs from its sealed receiver configuration')
    if (local_gauges != settings.get('pre_phone_gauges', False) or
            (local_gauges and 'ultra' not in value['configuration'])):
        raise ValueError('Bound local Ultra gauges must match the sealed receiver configuration')
    if execution:
        # Use the shared production renderer and sealed templates. In the
        # published guard, its whole first-party import graph is sealed too.
        # Bounded actual C++ checks enforce canvas and route coherence.
        from install_cm5_appliance import render
        rendered = render(root, value['user'], value['uid'], root, config, settings,
                          fanout_path=config_path(root, 'fanout') if 'fanout' in value['configuration'] else None,
                          release_binding=binding,
                          phone_manager_profile=config_path(root, 'phone_manager'),
                          phone_page_profile=config_path(root, 'phone_page') if execution['resident_home'] else None)
        if units != guard_units(rendered, binding, unit_directory):
            raise ValueError('Managed service commands, preflights or dependencies differ from the exact sealed renderer')
    return binding


def verify_units(binding, directory):
    """Refuse partial/mixed release installation before a new process starts."""
    value = verify(binding)
    directory = exact_path(directory)
    if value.get('purpose') == 'appliance':
        from appliance_boot import verify_installed
        verify_installed(value['boot_recovery'], directory)
    units = {}
    # Inspect unsupported resident services too. A manually inserted page must
    # not evade the coherent-bundle guard just because it is absent from the
    # sealed payload inventory.
    names = UNIT_NAMES + UNSEALED_UNIT_NAMES
    if value['version'] in (2, 3, 4, 5):
        names += RESERVED_OWNER_UNITS
    for name in names:
        filename = 'headunit-' + name + '.service'
        try: data, _ = read_file(directory / filename, 65536, {0, os.getuid()})
        except FileNotFoundError: continue
        units[filename] = data.decode('utf-8')
    try:
        verify_unit_bundle(binding, units, directory)
    except ValueError as error:
        raise ValueError('Installed services do not select one complete application release; recover the unit transaction') from error


def main():
    parser = argparse.ArgumentParser(description='Verify a pinned application release without starting services')
    parser.add_argument('--verify', type=Path, required=True)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--unit-directory', type=Path)
    args = parser.parse_args()
    try:
        binding = {'path': str(args.verify), 'sha256': args.manifest_sha256}
        if args.unit_directory: verify_units(binding, args.unit_directory)
        else: verify(binding)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error: parser.error(str(error))
    print('Application release verified')


if __name__ == '__main__': main()
