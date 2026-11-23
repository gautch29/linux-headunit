#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Render CM5 boot services with optional private vehicle snapshot distribution."""
import argparse
import json
import os
from pathlib import Path
import pwd
import re
import stat
import subprocess
from vehicle_fanout import vehicle_arguments
from appliance_transaction import install as install_units, rollback as rollback_units, recover_pending as recover_units


NAMES = ('headunit-compositor', 'headunit-outputs', 'headunit-cluster',
         'headunit-receiver')


def service_path(value):
    path = Path(value)
    if not path.is_absolute() or re.search(r'[^A-Za-z0-9_./-]', str(path)):
        raise ValueError('Service paths must be absolute and contain no service syntax')
    return str(path)


def native_cluster_command(repo, settings, cluster, vehicle_args):
    """One exact native role used by renderer and optional publisher admission."""
    command = [str(repo / 'build/headunit-cluster-wayland'), '--size',
               str(cluster['width']), str(cluster['height']), '--output', cluster['output'], '--background']
    command.extend(vehicle_args)
    for key, flag in (('media_state', '--media-file'), ('cluster_page', '--page-file')):
        if settings.get(key):
            command.extend((flag, service_path(settings[key])))
    if settings.get('pre_phone_gauges'):
        command.extend(('--local-ultra-profile', service_path(settings['ultra_installed_ui'])))
    if settings.get('greeting_sequence_file'):
        command.extend(('--greeting-sequence-file', service_path(settings['greeting_sequence_file'])))
    if settings.get('prephone_publication_socket'):
        command.extend(('--prephone-publication-socket', service_path(settings['prephone_publication_socket'])))
    if settings.get('prephone_indicators', False):
        command.append('--prephone-indicators')
    if settings.get('ambient_light', False):
        command.append('--ambient-light-software')
    return command


def cluster_configuration(settings, checker):
    result = subprocess.run([str(checker), str(settings['display_config']), '--json'],
                            check=True, capture_output=True, text=True, timeout=5)
    displays = json.loads(result.stdout)
    cluster = displays[1]
    wayland = cluster['wayland'] or settings.get('wayland_display', 'wayland-0')
    if (not re.fullmatch(r'[A-Za-z0-9._-]{1,100}', wayland) or wayland in ('.', '..') or
            not cluster['output'] or cluster['width'] < 640):
        raise ValueError('Appliance backup needs a named cluster output, valid Wayland socket and width >= 640')
    if (cluster['output'] == displays[0]['output'] and
            wayland == (displays[0]['wayland'] or settings.get('wayland_display', 'wayland-0'))):
        raise ValueError('Appliance main and cluster need separate outputs')
    return cluster | {'wayland': wayland}


def camera_configuration(settings, uid, profile, request, lease, checker, release_binding=None):
    if not any((profile,request,lease)):
        return None
    if not all((profile,request,lease)):
        raise ValueError('Local camera service needs profile, request and surface lease files')
    paths=[service_path(value) for value in (profile,request,lease,settings['display_config'])]
    if paths[1]==paths[2]:
        raise ValueError('Camera request and surface lease must differ')
    if not (settings.get('ultra_negotiate') and settings.get('ultra_native_video') and settings.get('ultra_installed_ui')):
        raise ValueError('Local camera service currently requires the guarded Ultra main renderer')
    ui=Path(service_path(settings['ultra_installed_ui']))
    sealed = False
    if release_binding:
        from appliance_release import configuration_member
        sealed = configuration_member(ui, release_binding, 'ultra')
        if not sealed: raise ValueError('Ultra handoff profile is not in its bound release')
    fd=os.open(ui,os.O_RDONLY|os.O_CLOEXEC|os.O_NOFOLLOW|os.O_NONBLOCK)
    try:
        info=os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or (not sealed and (info.st_uid!=uid or info.st_mode&0o077)) or info.st_size>65536:
            raise ValueError('Ultra handoff profile must be private and owned by the display user')
        raw=os.read(fd,65537)
        if len(raw)!=info.st_size: raise ValueError('Ultra handoff profile changed while reading')
    finally: os.close(fd)
    fields={}
    for line in raw.decode().splitlines():
        key,separator,value=line.partition('=')
        if not separator or not key or key in fields:
            raise ValueError('Invalid Ultra handoff profile')
        fields[key]=value
    if fields.get('nativeCameraControlFile')!=paths[1] or fields.get('nativeCameraLeaseFile')!=paths[2]:
        raise ValueError('Camera service and Ultra main must use the same request and surface lease')
    # Existing signed-camera input is a separate path; do not imply a native
    # cover surface and signed input share one provider/focus owner.
    if fields.get('cameraProfile'):
        raise ValueError('Native camera service cannot also configure a signed camera input')
    identity={}
    if os.geteuid()!=uid:
        if os.geteuid()!=0: raise ValueError('Validate camera settings as their display user')
        identity={'user':uid,'group':pwd.getpwuid(uid).pw_gid}
    result=subprocess.run([str(checker),'--camera-profile',paths[0],'--display-config',paths[3],
                           '--request-file',paths[1],'--surface-lease-file',paths[2],'--validate-only'],
                          check=True,capture_output=True,text=True,timeout=5,**identity)
    value=json.loads(result.stdout)
    if (not isinstance(value,dict) or set(value)!={'configurationValid','enabled','wayland','output','width','height'} or
            value['configurationValid'] is not True or type(value['enabled']) is not bool or
            not isinstance(value['wayland'],str) or not re.fullmatch(r'[A-Za-z0-9._-]{1,100}',value['wayland']) or
            value['wayland'] in ('.','..') or not isinstance(value['output'],str) or
            not re.fullmatch(r'[A-Za-z0-9._-]{1,127}',value['output']) or
            type(value['width']) is not int or type(value['height']) is not int or
            not 320<=value['width']<=4096 or not 240<=value['height']<=2160):
        raise ValueError('Camera validator returned invalid main display settings')
    if not value['enabled']:
        raise ValueError('Guarded native camera service requires enabled manual activation')
    return value | {'profile':paths[0],'request':paths[1],'lease':paths[2],'displays':paths[3]}


def render(source, user, uid, repo, config, settings, fanout_path=None, checker=None,
           camera_profile=None,camera_request=None,camera_lease=None,camera_checker=None,release_binding=None,
           phone_manager_profile=None, phone_page_profile=None):
    if phone_page_profile and not phone_manager_profile:
        raise ValueError('Native page startup requires the supervised phone manager profile')
    if phone_manager_profile and any((camera_profile, camera_request, camera_lease)):
        raise ValueError('Supervised phone mode switching needs qualified Standard camera handoff before combining camera services')
    if release_binding:
        from appliance_release import config_path, verify
        from receiver_configuration import load_receiver_configuration
        release = verify(release_binding)
        execution = release.get('execution')
        if release['version'] == 1:
            if phone_manager_profile or phone_page_profile:
                raise ValueError('Version 1 sealed releases do not contain native manager/page profiles')
        else:
            root = Path(release_binding['path'])
            expected_manager = config_path(root, 'phone_manager')
            expected_page = config_path(root, 'phone_page') if execution['resident_home'] else None
            expected_fanout = config_path(root, 'fanout') if 'fanout' in release['configuration'] else None
            if (Path(source) != root or Path(repo) != root or Path(config) != config_path(root, 'receiver') or
                    user != release['user'] or uid != release['uid'] or
                    phone_manager_profile != expected_manager or phone_page_profile != expected_page or
                    fanout_path != expected_fanout or any((camera_profile, camera_request, camera_lease))):
                raise ValueError('Managed sealed services must use their complete release configuration and account')
            if load_receiver_configuration(Path(config)) != settings:
                raise ValueError('Managed sealed services must use the same receiver configuration')
    templates = source / 'deploy' / 'cm5'
    service_path(repo); service_path(config)
    cluster = cluster_configuration(settings, checker or repo / 'build' / 'headunit-display-config-check')
    local_gauges_arg = ''
    if settings.get('pre_phone_gauges'):
        if not settings.get('ultra_installed_ui') or (cluster['width'], cluster['height']) != (1920, 720):
            raise ValueError('Pre-phone gauges require a verified Ultra profile and 1920x720 cluster')
        local_gauges_arg = '--local-ultra-profile ' + service_path(settings['ultra_installed_ui'])
    vehicle_args = vehicle_arguments(settings, uid, fanout_path, release_binding)
    if len(vehicle_args) == 2:
        service_path(vehicle_args[1])
    vehicle_arg = ' '.join(vehicle_args)
    if settings.get('prephone_publication_socket') and not phone_manager_profile:
        raise ValueError('Native publication requires the supervised manager graph')
    camera=camera_configuration(settings,uid,camera_profile,camera_request,camera_lease,
                                camera_checker or repo/'build'/'headunit-camera-wayland',release_binding)
    if phone_manager_profile:
        from phone_manager_boot import inspect_settings
        manager = inspect_settings(phone_manager_profile, settings, uid, checker)
        if not manager['enabled']:
            raise ValueError('Enable the phone manager profile explicitly before selecting its boot service')
    page = None
    if phone_page_profile:
        from phone_page_configuration import inspect_boot
        from receiver_configuration import load_receiver_configuration
        if load_receiver_configuration(Path(config)) != settings:
            raise ValueError('Native page and rendered services must use the same receiver configuration')
        page = inspect_boot(phone_page_profile, phone_manager_profile, config, uid, checker)
    substitutions = {'@CLUSTER_COMMAND@': ' '.join(native_cluster_command(repo, settings, cluster, vehicle_args)), '@USER@': user, '@UID@': str(uid), '@REPO@': str(repo),
                     '@CONFIG@': str(config), '@LOCAL_GAUGES_ARG@': local_gauges_arg,
                     '@GREETING_ARG@': '--greeting-sequence-file ' + service_path(settings['greeting_sequence_file']) if settings.get('greeting_sequence_file') else '',
                     '@MEDIA_ARG@': '--media-file ' + service_path(settings['media_state']) if settings.get('media_state') else '',
                     '@PAGE_ARG@': '--page-file ' + service_path(settings['cluster_page']) if settings.get('cluster_page') else '',
                     '@CLUSTER_WAYLAND@': cluster['wayland'], '@CLUSTER_OUTPUT@': cluster['output'],
                     '@CLUSTER_WIDTH@': str(cluster['width']), '@CLUSTER_HEIGHT@': str(cluster['height']),
                     '@VEHICLE_ARG@': vehicle_arg,
                     '@VEHICLE_ORDER@': 'Wants=headunit-vehicle.service\nAfter=headunit-vehicle.service' if fanout_path else '',
                     '@VEHICLE_FANOUT_CONFIG@': service_path(fanout_path) if fanout_path else ''}
    if phone_manager_profile:
        substitutions.update({'@MANAGER_PROFILE@': service_path(phone_manager_profile),
                              '@MANAGER_VEHICLE_ARG@': '--vehicle-fanout-config ' + service_path(fanout_path) if fanout_path else ''})
    names = NAMES + ('headunit-vehicle',) if fanout_path else NAMES
    if page:
        names += ('headunit-phone-page',)
        substitutions.update({'@PHONE_PAGE_PROFILE@': service_path(phone_page_profile),
                              '@PAGE_WAYLAND@': page['waylandDisplay'], '@PAGE_OUTPUT@': page['output']})
    if camera:
        names+=('headunit-camera',)
        substitutions.update({'@CAMERA_WAYLAND@':camera['wayland'],'@CAMERA_OUTPUT@':camera['output'],
                              '@CAMERA_PROFILE@':camera['profile'],'@CAMERA_REQUEST@':camera['request'],
                              '@CAMERA_LEASE@':camera['lease'],'@CAMERA_DISPLAYS@':camera['displays']})
    return {name + '.service': replace(templates / (('headunit-managed-receiver' if
            name == 'headunit-receiver' and phone_manager_profile else name) + '.service.in'), substitutions)
            for name in names}


def replace(path, values):
    text = path.read_text()
    for old, new in values.items():
        text = text.replace(old, new)
    if re.search(r'@[A-Z_]+@', text):
        raise ValueError('Unresolved service template field')
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--user')
    parser.add_argument('--repo', type=Path)
    parser.add_argument('--receiver-config', type=Path)
    parser.add_argument('--vehicle-fanout-config', type=Path,
                        help='private adapter input and distinct backup/receiver/Ultra consumer sockets')
    parser.add_argument('--camera-profile',type=Path,help='optional native local-camera JSON')
    parser.add_argument('--camera-control-file',type=Path,help='owned private manual camera request')
    parser.add_argument('--camera-surface-lease-file',type=Path,help='shared native/Ultra main-surface lease')
    parser.add_argument('--phone-manager-profile', type=Path,
                        help='optional enabled JSON native manager profile; replaces the direct receiver owner')
    parser.add_argument('--phone-page-profile', type=Path,
                        help='optional enabled native Home page JSON; requires the supervised phone manager')
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--install', action='store_true', help='write and enable services, but do not start them')
    parser.add_argument('--transaction-directory', type=Path, default=Path('/var/lib/ultra-headunit/appliance'),
                        help='private root-owned snapshots of managed service units')
    action.add_argument('--rollback', type=Path, help='restore the managed-unit snapshot from a previous installation')
    action.add_argument('--recover-pending', action='store_true',
                        help='recover interrupted unit transactions without loading receiver configuration')
    args = parser.parse_args()
    if args.rollback or args.recover_pending:
        if args.user or args.repo or args.receiver_config or args.vehicle_fanout_config or \
                args.camera_profile or args.camera_control_file or args.camera_surface_lease_file or \
                args.phone_manager_profile or args.phone_page_profile:
            parser.error('Recovery actions cannot be combined with installation options')
        if os.geteuid()!=0: parser.error('Recovery actions require root')
        try:
            if args.rollback:
                rollback_units(args.rollback,Path('/etc/systemd/system'),args.transaction_directory)
            else:
                recover_units(Path('/etc/systemd/system'),args.transaction_directory)
        except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as error:
            parser.error(str(error))
        print(('Previous managed service units restored' if args.rollback else 'Pending managed-unit recovery finished') +
              '; running services were not restarted')
        return
    if not args.user or not args.repo or not args.receiver_config:
        parser.error('--user, --repo and --receiver-config are required for installation or preview')
    if not re.fullmatch(r'[a-z_][a-z0-9_-]{0,31}', args.user):
        parser.error('Invalid user name')
    account = pwd.getpwnam(args.user)
    repo = args.repo.resolve(strict=True)
    config = args.receiver_config.resolve(strict=True)
    for path in (repo, config):
        if not path.is_absolute() or re.search(r'[^A-Za-z0-9_./-]', str(path)):
            parser.error('Paths must be absolute and contain no service syntax')
    info = config.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != account.pw_uid or info.st_mode & 0o077:
        parser.error('Receiver configuration must be private and owned by the display user')
    from receiver_configuration import load_receiver_configuration
    settings = load_receiver_configuration(config)
    for key in ('media_state', 'cluster_page', 'vehicle_socket'):
        path = settings.get(key)
        if path and re.search(r'[^A-Za-z0-9_./-]', str(path)):
            parser.error(f'{key} path contains service syntax')
    if settings.get('seconds') != 0:
        parser.error('Appliance receiver configuration must set seconds to 0')
    if not (repo / 'build' / 'headunit-cluster-wayland').is_file():
        parser.error('Build the native cluster on the CM5 before installing')
    try:
        units = render(Path(__file__).resolve().parents[1], args.user, account.pw_uid, repo, config, settings,
                       args.vehicle_fanout_config.resolve(strict=True) if args.vehicle_fanout_config else None,
                       camera_profile=args.camera_profile,camera_request=args.camera_control_file,
                       camera_lease=args.camera_surface_lease_file,
                       phone_manager_profile=args.phone_manager_profile, phone_page_profile=args.phone_page_profile)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    if not args.install:
        for name in units:
            print(name + ': ready')
        return
    if os.geteuid() != 0:
        parser.error('--install requires root')
    try:
        snapshot=install_units(units,Path('/etc/systemd/system'),args.transaction_directory)
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as error:
        parser.error(str(error))
    print('Headunit services installed and enabled; start headunit-compositor.service first')
    print('Managed-unit rollback snapshot: '+str(snapshot))


if __name__ == '__main__':
    main()
