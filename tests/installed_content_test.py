#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Explicit signed-script/GPU fixture; synthetic clock/ACKs, no real phone.

Usage: installed_content_test.py WESTON PROBE CAMERA_FREE_PRIVATE_PROFILE NEW_EVIDENCE_DIR
Never opens the production outputs, phone, radio or camera. Captures stay private.
"""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time
from installed_camera_state_test import save, stop


def owner_snapshot():
    owners = ('headunit-compositor', 'headunit-outputs', 'headunit-phone-page',
              'headunit-cluster', 'headunit-receiver')
    return subprocess.check_output(['systemctl', 'show', *owners,
        '-p', 'Id', '-p', 'ActiveState', '-p', 'SubState', '-p', 'MainPID',
        '-p', 'NRestarts', '-p', 'ExecMainStartTimestampMonotonic'], timeout=8)


def main():
    if len(sys.argv) != 5:
        raise ValueError(__doc__)
    weston, probe, original, destination = sys.argv[1:]
    root = Path(__file__).resolve().parents[1]
    evidence = Path(destination).resolve()
    evidence.mkdir(mode=0o700)
    owners_before = owner_snapshot()
    save(evidence / 'owners-before.txt', owners_before)
    runtime = evidence / 'runtime'
    runtime.mkdir(mode=0o700)
    capture = evidence / 'ui-capture'
    capture.mkdir(mode=0o700)
    base = dict(line.split('=', 1) for line in Path(original).read_text().splitlines()
                if line and not line.startswith('#'))
    if any(base.get(k) for k in ('cameraProfile', 'cameraControlFile', 'nativePageControlFile',
                                'nativePageLeaseFile', 'nativeCameraControlFile', 'nativeCameraLeaseFile')):
        raise ValueError('Use a camera-free profile without native surface owners')
    vehicle = plistlib.loads(Path(base['vehicleProfile']).read_bytes())
    extra = plistlib.loads((root / 'config/cluster-content-script-bindings.example.plist').read_bytes())['signals']
    next_iid = max({s['iid'] for s in vehicle['signals']} | {599}) + 1
    for signal in extra:
        if any(s['path'] == signal['path'] and s.get('instance') == signal.get('instance') for s in vehicle['signals']):
            raise ValueError('Fixture must not replace existing request bindings')
        signal['iid'] = next_iid
        next_iid += 1
    vehicle['signals'] += extra
    vehicle_path = save(evidence / 'vehicle.plist', plistlib.dumps(vehicle))
    settings = json.loads((root / 'config/car-settings.cluster.example.json').read_text())
    settings['cluster_content']['request_file'] = str(evidence / 'cluster-content.request')
    settings_path = save(evidence / 'settings.json', json.dumps(settings))
    fields = dict(base, vehicleProfile=vehicle_path, deferSceneFence='false',
                  centerWaylandDisplay='content-main', clusterWaylandDisplay='content-cluster',
                  persistClusterLayout='false', benchAssistanceView='true', simulateGauges='true',
                  clusterStartupLayout='8BC9ABB1-CB20-595E-92BF-D8F099BF3515',
                  diagnostics='true', diagnosticReports='12')
    fields.pop('clusterLayoutFile', None)
    fields.pop('vehicleSocket', None)
    profile = save(evidence / 'installed.conf', '\n'.join(k + '=' + v for k, v in fields.items()) + '\n')
    displays = (root / 'config/displays-cm5-dual.conf').read_text()
    displays = re.sub(r'(main\.wayland=).*', r'\1content-main', displays)
    displays = re.sub(r'(cluster\.wayland=).*', r'\1content-cluster', displays)
    displays = re.sub(r'(?m)^\w+\.output=.*\n', '', displays)
    display_path = save(evidence / 'displays.conf', displays)
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), HEADUNIT_INSTALLED_UI_PROBE_SECONDS='20',
               HEADUNIT_INSTALLED_UI_PROBE_CONTENT_CONFIG=settings_path)
    processes = []
    try:
        for name in ('content-main', 'content-cluster'):
            with (evidence / (name + '.log')).open('xb') as log:
                os.chmod(log.name, 0o600)
                processes.append(subprocess.Popen([weston, '--backend=headless', '--renderer=gl',
                    '--width=1920', '--height=720', '--idle-time=0', '--no-config', '--socket=' + name,
                    '--shell=desktop-shell.so'], env=env, stdout=log, stderr=log, start_new_session=True))
        deadline = time.monotonic() + 10
        while not all((runtime / n).exists() for n in ('content-main', 'content-cluster')):
            if any(p.poll() is not None for p in processes) or time.monotonic() >= deadline:
                raise RuntimeError('Isolated content compositors did not start')
            time.sleep(.02)
        with (evidence / 'probe.stdout').open('xb') as out, (evidence / 'probe.stderr').open('xb') as err:
            os.chmod(out.name, 0o600)
            os.chmod(err.name, 0o600)
            run = subprocess.Popen([probe, profile, '--synthetic-clock', display_path, str(capture)],
                                   env=env, stdout=out, stderr=err, start_new_session=True)
            try:
                status = run.wait(timeout=60)
            finally:
                stop(run)
        if status:
            raise RuntimeError('Signed content fixture failed; inspect private evidence')
        report = json.loads((evidence / 'probe.stdout').read_text())
        if not report.get('contentIntentFixture') or report.get('contentLocalURLsObserved') != 3 or not report.get('contentLocalReadChecks'):
            raise RuntimeError('Not all three content intents reached the signed script vehicle reader')
        if not report.get('contentStartupChecks') or report.get('contentLocalDismissals') != 1:
            raise RuntimeError('Signed-script startup hiding and restricted Close were not checked')
        report['v3dRenderer'] = all('V3D' in (evidence / (n + '.log')).read_text()
                                    for n in ('content-main', 'content-cluster'))
        if not report['v3dRenderer']:
            raise RuntimeError('Both fixture displays must use V3D')
        # Remote UI requests travel over VDP, not as literal URLs in UI-Sync.
        # This fixture checks script reads; only a real phone can supply widget pixels.
        owners_after = owner_snapshot()
        save(evidence / 'owners-after.txt', owners_after)
        report.update(realPhone=False, physicalScanoutQualified=False,
                      activeServicesChanged=owners_before != owners_after,
                      widgetPixelsQualified=False)
        files = [Path(probe), Path(__file__), root / 'tests/installed_ui_probe.cpp',
                 root / 'src/theme/content_control.cpp', root / 'src/theme/live_ui.cpp',
                 root / 'src/theme/script_runtime.cpp', root / 'src/theme/vehicle_script_view.cpp',
                 root / 'src/theme/vehicle_configuration.cpp', root / 'src/theme/vehicle_service.cpp',
                 root / 'include/headunit/theme/content_control.hpp',
                 root / 'config/cluster-content-script-bindings.example.plist',
                 root / 'config/car-settings.cluster.example.json',
                 Path(original), Path(profile), Path(display_path), Path(settings_path), Path(vehicle_path)]
        report['sha256'] = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
        save(evidence / 'report.json', json.dumps(report, indent=2) + '\n')
        if report['activeServicesChanged']:
            raise RuntimeError('Production owner state changed during the isolated fixture')
        print(json.dumps({k: v for k, v in report.items() if k != 'sha256'}))
    finally:
        for p in processes:
            stop(p)

if __name__ == '__main__':
    main()
