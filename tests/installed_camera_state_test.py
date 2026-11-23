#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Explicit private-asset signed camera check on two isolated GPU compositors.

Usage: installed_camera_state_test.py WESTON PROBE PRIVATE_PACK_PROFILE EVIDENCE_DIR [CAMERA_JSON] [--bench-adas]
The evidence directory must be new. Signed resources and private captures never
leave it; the active appliance, radio and original profile are untouched.
"""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import signal
import subprocess
import sys
import time


def save(path, data):
    with path.open('xb') as stream:
        os.chmod(path, 0o600)
        stream.write(data if isinstance(data, bytes) else data.encode())
    return str(path)


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def main():
    if len(sys.argv) < 5:
        raise ValueError(__doc__)
    weston, probe, base_path, destination = sys.argv[1:5]
    optional = list(sys.argv[5:])
    bench_adas = '--bench-adas' in optional
    if bench_adas: optional.remove('--bench-adas')
    if len(optional) > 1: raise ValueError(__doc__)
    root = Path(__file__).resolve().parents[1]
    evidence = Path(destination).resolve()
    evidence.mkdir(mode=0o700)  # Refuse reuse and preserve earlier evidence.
    evidence.chmod(0o700)
    runtime = evidence / 'runtime'
    runtime.mkdir(mode=0o700)
    base_path = Path(base_path).resolve()
    base = dict(line.split('=', 1) for line in base_path.read_text().splitlines()
                if line and not line.startswith('#'))
    if any(base.get(key) for key in ('cameraProfile', 'cameraControlFile',
                                    'nativePageControlFile', 'nativePageLeaseFile',
                                    'nativeCameraControlFile', 'nativeCameraLeaseFile')):
        raise ValueError('use a camera-free pack profile without native surface owners')
    # The private asset profile is expected to contain absolute resource paths.
    vehicle = plistlib.loads(Path(base['vehicleProfile']).read_bytes())
    extra = plistlib.loads((root / 'config/camera-script-bindings.example.plist').read_bytes())['signals']
    next_iid = max({entry['iid'] for entry in vehicle['signals']} | {499}) + 1
    for entry in extra:
        entry['iid'] = next_iid
        next_iid += 1
    vehicle['signals'] += extra
    camera = json.loads((Path(optional[0]) if optional else
                         root / 'config/camera-ultra-reference.example.json').read_text())
    camera['enabled'] = True
    control = save(evidence / 'camera-request', 'off\n')
    camera_path = save(evidence / 'camera.json', json.dumps(camera))
    vehicle_path = save(evidence / 'vehicle.plist', plistlib.dumps(vehicle))
    state = save(evidence / 'camera-state.json', (root / 'config/camera-state.example.json').read_bytes())
    page_request = save(evidence / 'page-request', 'off\n')
    page_lease = save(evidence / 'page-lease', b'')
    fields = dict(base, nativePageControlFile=page_request, nativePageLeaseFile=page_lease,
                  cameraProfile=camera_path, cameraControlFile=control,
                  cameraStatusFile=str(evidence / 'camera-status'),
                  vehicleProfile=vehicle_path, deferSceneFence='false',
                  centerWaylandDisplay='camera-main', clusterWaylandDisplay='camera-cluster',
                  diagnostics='true', diagnosticReports='16', opaqueWindows='true')
    # This fixture never polls a live style selector or writes its preferences.
    fields.pop('clusterLayoutFile', None)
    fields['persistClusterLayout'] = 'false'
    if bench_adas:
        fields['benchAssistanceView'] = 'true'
        fields['simulateGauges'] = 'true'
        fields['clusterStartupLayout'] = '8BC9ABB1-CB20-595E-92BF-D8F099BF3515'
        fields.pop('vehicleSocket', None)
    profile = save(evidence / 'installed.conf', '\n'.join(f'{key}={value}' for key, value in fields.items()) + '\n')
    displays = (root / 'config/displays-cm5-dual.conf').read_text()
    displays = re.sub(r'(main\.wayland=).*', r'\1camera-main', displays)
    displays = re.sub(r'(cluster\.wayland=).*', r'\1camera-cluster', displays)
    displays = re.sub(r'(?m)^\w+\.output=.*\n', '', displays)
    display_path = save(evidence / 'displays.conf', displays)
    environment = dict(os.environ, XDG_RUNTIME_DIR=str(runtime))
    if bench_adas: environment['HEADUNIT_CAMERA_PROBE_ADAS'] = '1'
    processes = []
    try:
        for name in ('camera-main', 'camera-cluster'):
            with (evidence / (name + '.log')).open('xb') as log:
                os.chmod(log.name, 0o600)
                processes.append(subprocess.Popen([
                    weston, '--backend=headless', '--renderer=gl', '--width=1920',
                    '--height=720', '--idle-time=0', '--no-config', '--socket=' + name,
                    '--shell=desktop-shell.so',
                ], env=environment, stdout=log, stderr=log, start_new_session=True))
        deadline = time.monotonic() + 10
        while not all((runtime / name).exists() for name in ('camera-main', 'camera-cluster')):
            if any(process.poll() is not None for process in processes) or time.monotonic() >= deadline:
                raise RuntimeError('isolated camera compositors did not start; inspect private evidence')
            time.sleep(0.02)
        with (evidence / 'probe.stdout').open('xb') as out, (evidence / 'probe.stderr').open('xb') as err:
            os.chmod(out.name, 0o600)
            os.chmod(err.name, 0o600)
            run = subprocess.Popen([probe, profile, display_path, state, control, page_request, page_lease],
                                   env=environment, stdout=out, stderr=err, start_new_session=True)
            try:
                status = run.wait(timeout=150)
            finally:
                stop(run)
        if status:
            raise RuntimeError('signed camera state probe failed; inspect private evidence')
        report = json.loads((evidence / 'probe.stdout').read_text())
        if not report.get('signedCameraStateLifecycle'):
            raise RuntimeError('probe did not report the complete fixture lifecycle')
        logs = [(evidence / (name + '.log')).read_text() for name in ('camera-main', 'camera-cluster')]
        report['v3dRenderer'] = all('V3D' in log for log in logs)
        if not report['v3dRenderer']:
            raise RuntimeError('fixture did not use V3D on both compositors')
        report['activeServicesChanged'] = False
        report['physicalCapture'] = camera['source']['kind'] == 'v4l2'
        report['realPhone'] = False
        # Retain exact fixture, source and private input hashes without contents.
        files = [Path(probe), Path(__file__), root / 'tests/installed_camera_state_probe.cpp',
                 root / 'src/theme/live_ui.cpp', root / 'src/theme/camera_state.cpp',
                 root / 'src/theme/camera_input.cpp', root / 'src/theme/camera_visibility.cpp',
                 base_path, Path(profile), Path(display_path), Path(state), Path(camera_path), Path(vehicle_path)]
        report['sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
        save(evidence / 'report.json', json.dumps(report, indent=2) + '\n')
        print(json.dumps({key: value for key, value in report.items() if key != 'sha256'}))
    finally:
        for process in processes:
            stop(process)


if __name__ == '__main__':
    main()
