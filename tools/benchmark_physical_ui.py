#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Phone-free signed-theme benchmark with real Wayland presentation receipts."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
from uuid import UUID


SERVICES = ('headunit-receiver.service', 'headunit-cluster.service')


def profile_arguments(path):
    if path.stat().st_size > 8192:
        raise ValueError('Ultra profile is too large')
    fields = {}
    for line in path.read_text().splitlines():
        key, separator, value = line.partition('=')
        if not separator or not key or not value or key in fields or '\0' in line:
            raise ValueError('Invalid Ultra profile')
        fields[key] = value
    keys = ('assetDirectory', 'assetIdentifier', 'assetVersion', 'vehicleProfile')
    if any(key not in fields for key in keys):
        raise ValueError('Ultra profile lacks asset fields')
    for key in ('assetDirectory', 'vehicleProfile', 'acceptanceReference'):
        if key not in fields:
            continue
        resource = Path(fields[key])
        if not resource.is_absolute():
            raise ValueError('Ultra resource paths must be absolute')
        fields[key] = str(resource.resolve(strict=True))
    allow_expired = fields.get('allowExpiredAssetCertificate', 'false')
    if allow_expired not in ('true', 'false'):
        raise ValueError('Invalid asset certificate expiry flag')
    arguments = [fields[key] for key in keys[:3]]
    arguments += [fields.get('acceptanceReference', '-'), '--worker-gpu', fields['vehicleProfile']]
    if allow_expired == 'true':
        arguments.append('--allow-expired-leaf')
    return arguments


def systemctl(*arguments):
    prefix = [] if os.geteuid() == 0 else ['sudo', '-n']
    return subprocess.run(prefix + ['systemctl', *arguments],
                          capture_output=True, text=True, timeout=30)


def summary(report, args):
    return {
        'livePhoneTested': report['livePhoneTested'],
        'actualWaylandFeedback': report['actualWaylandFeedback'],
        'seconds': args.seconds,
        'mainOutput': args.main_output, 'clusterOutput': args.cluster_output,
        'queueDepths': [args.main_pending, args.cluster_pending],
        'feedbackPollMs': args.poll_ms,
        'overlapScriptTick': args.overlap, 'deferSceneFence': args.defer_fence,
        'opaqueWindows': args.opaque_windows,
        'clusterRenderWidth': args.cluster_render_width,
        'clusterLayout': args.cluster_layout,
        'syntheticNativeVideo': report.get('syntheticNativeVideo', False),
        'syntheticUiPeer': report.get('syntheticUiPeer', False),
        'syntheticVideoBackdrop': report.get('syntheticVideoBackdrop', False),
        'assetVideoNegotiationTested': report.get('assetVideoNegotiationTested', False),
        'receiverSocketsTested': report.get('receiverSocketsTested', False),
        'nativeVideoPanels': report.get('nativeVideoPanels', []),
        'scenePresentations': report['scenePresentations'],
        'approximateFps': [n / args.seconds for n in report['scenePresentations']],
        'approximateNativeVideoFps': [v['presented'] / args.seconds
                                     for v in report['nativeVideoPanels']]
            if report.get('syntheticNativeVideo') else None,
        'benchPanels': report['benchPanels'],
        'workerMaxTurnMs': report['workerMaxTurnMs'],
        'workerMissedSlots': report['workerMissedSlots'],
        'workerPhaseCosts': report['workerPhaseCosts'],
        'warmTraceStartsAfterDraw': 64,
        # The trace retains only the worst 32 per panel. Never describe these
        # counts as a complete histogram. Exclude startup's first 64 draws.
        'retainedWarmSlowFrames': [[t for t in panel if t['ordinal'] > 64]
                                   for panel in report['slowFrames']],
        'retainedWarmLargeGaps': [[t for t in panel if t['drawOrdinal'] > 64]
                                  for panel in report['slowGaps']],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--binary', type=Path, default=Path('build/headunit-theme-probe'))
    parser.add_argument('--output-dir', type=Path, required=True,
                        help='new private directory for full log and summary')
    parser.add_argument('--main-output', required=True)
    parser.add_argument('--cluster-output', required=True)
    parser.add_argument('--wayland-display', default='wayland-0')
    parser.add_argument('--seconds', type=int, choices=range(5, 121), default=30, metavar='5..120')
    parser.add_argument('--main-pending', type=int, choices=(1, 2), default=1)
    parser.add_argument('--cluster-pending', type=int, choices=(1, 2), default=1)
    parser.add_argument('--poll-ms', type=int, choices=range(5), default=0)
    parser.add_argument('--overlap', action='store_true')
    parser.add_argument('--defer-fence', action='store_true')
    parser.add_argument('--opaque-windows', action='store_true',
                        help='composite final windows onto black and advertise matching opaque regions')
    parser.add_argument('--cluster-render-width', type=int, choices=(1920, 1280, 960), default=1920,
                        help='offline render-resolution comparison; asset canvas remains 1920 x 720')
    parser.add_argument('--cluster-layout', type=lambda value: str(UUID(value)).upper(),
                        help='installed pack layout UUID; affects only this offline probe')
    parser.add_argument('--main-video', type=Path,
                        help='generated 1920 x 720/60 Hz all-intra HEVC fixture; enables native decoding')
    parser.add_argument('--cluster-video', type=Path,
                        help='second generated native-video fixture; both files are required')
    parser.add_argument('--pause-appliance', action='store_true',
                        help='disconnect phone; pause active receiver/backup units and restore them in finally')
    args = parser.parse_args()
    if args.main_output == args.cluster_output:
        parser.error('Choose distinct physical outputs')
    if bool(args.main_video) != bool(args.cluster_video):
        parser.error('Native video needs both generated fixture files')
    if not os.environ.get('XDG_RUNTIME_DIR'):
        parser.error('Run as the display user with XDG_RUNTIME_DIR set')
    command = [str(args.binary.resolve(strict=True)), *profile_arguments(args.profile.resolve(strict=True))]
    args.output_dir.mkdir(mode=0o700, parents=True, exist_ok=False)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith('HEADUNIT_THEME_PROBE_')}
    settings = {'BENCH_60HZ': '1', 'BENCH_SECONDS': str(args.seconds),
                'SIMULATE_GAUGES': '1', 'SLOW_FRAMES': '1',
                'MAIN_OUTPUT': args.main_output, 'CLUSTER_OUTPUT': args.cluster_output,
                'MAIN_WAYLAND': args.wayland_display, 'CLUSTER_WAYLAND': args.wayland_display,
                'MAIN_PENDING': str(args.main_pending), 'CLUSTER_PENDING': str(args.cluster_pending),
                'CLUSTER_RENDER_WIDTH': str(args.cluster_render_width)}
    if args.cluster_layout:
        settings['CLUSTER_LAYOUT'] = args.cluster_layout
    if args.main_video:
        for panel, path in (('MAIN', args.main_video), ('CLUSTER', args.cluster_video)):
            settings[panel + '_VIDEO'] = str(path.resolve(strict=True))
    if args.poll_ms:
        settings['EAGER_PRESENTATION'] = str(args.poll_ms)
    if args.overlap:
        settings['OVERLAP_SCRIPT_TICK'] = '1'
    if args.defer_fence:
        settings['DEFER_FENCE'] = '1'
    if args.opaque_windows:
        settings['OPAQUE_WINDOWS'] = '1'
    environment.update({'HEADUNIT_THEME_PROBE_' + key: value for key, value in settings.items()})
    environment['WAYLAND_DISPLAY'] = args.wayland_display
    restore = []
    child = None
    previous_handler = signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    try:
        if args.pause_appliance:
            for service in SERVICES:
                query = systemctl('is-active', service)
                state = query.stdout.strip()
                if query.returncode not in (0, 3) or state not in ('active', 'inactive', 'failed'):
                    raise RuntimeError('Could not read stable appliance state')
                if state == 'active':
                    restore.append(service)
            for service in restore:
                if systemctl('stop', service).returncode:
                    raise RuntimeError('Could not pause appliance service')
        log = args.output_dir / 'probe.log'
        with log.open('xb') as output:
            child = subprocess.Popen(command, env=environment, stdout=output,
                                     stderr=subprocess.STDOUT, start_new_session=True)
            try:
                status = child.wait(timeout=args.seconds + 45)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait(timeout=5)
        if status:
            raise RuntimeError(f'Probe failed ({status}); inspect private probe.log')
        reports = [json.loads(line) for line in log.read_text(errors='replace').splitlines()
                   if line.startswith('{"workerFullEntryPoints"')]
        if len(reports) != 1 or reports[0]['livePhoneTested'] or not reports[0]['actualWaylandFeedback']:
            raise RuntimeError('Missing or unexpected benchmark receipt report')
        if bool(args.main_video) != reports[0].get('syntheticNativeVideo', False):
            raise RuntimeError('Native video was not exercised as requested')
        result = summary(reports[0], args)
        (args.output_dir / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result))
    finally:
        failures = []
        for service in reversed(restore):
            try:
                if systemctl('start', service).returncode:
                    failures.append(service)
            except (OSError, subprocess.TimeoutExpired):
                failures.append(service)
        signal.signal(signal.SIGTERM, previous_handler)
        if failures:
            raise RuntimeError('Could not restore services: ' + ', '.join(failures))


if __name__ == '__main__':
    main()
