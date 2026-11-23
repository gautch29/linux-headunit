#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real native HUD on a private three-output software labwc; no physical I/O.

Screenshot placement, Wayland buffer releases/submission pacing and endpoint
retirement are component evidence, not scanout, optics or vehicle qualification.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

from native_home_wayland_test import ppm

OUTPUTS = ('HEADLESS-1', 'HEADLESS-2', 'HEADLESS-3')


def navigation_packet(epoch, changed, sent, available):
    """Explicit synthetic HNG1, using actual host source-change/send dates."""
    packet = bytearray(b'HNG1' + struct.pack('>QQQB', epoch, 1, sent, available))
    if not available:
        return bytes(packet)

    def text(value):
        value = value.encode('utf-8')
        packet.extend(struct.pack('>H', len(value)) + value)

    packet.extend(struct.pack('>QB', changed, 1))
    for value in ('Fixture route', 'Fixture Road', 'Fixture destination'):
        text(value)
    packet.extend(struct.pack('>BIBIBQBBHB', 1, 300, 1, 4000, 1, 420, 0, 1, 7, 1))
    packet.append(2)  # Actual fixture maneuver: turn right.
    text('Turn right onto Fixture Road')
    text('Fixture Road')
    packet.extend(struct.pack('>Bh', 1, -90))
    check(len(packet) <= 1536, 'Fixture navigation packet exceeds protocol bound')
    return bytes(packet)


def check(value, reason):
    if not value:
        raise AssertionError(reason)


def wait(predicate, label, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.05)
    raise TimeoutError(label)


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=3)


def harness(args):
    check(sys.platform.startswith('linux') and os.geteuid() != 0,
          'HUD fixture requires nonroot Linux')
    for binary in (args.hud, args.labwc, args.grim, args.wlr_randr):
        check(Path(binary).is_absolute() and os.access(binary, os.X_OK), 'Missing explicit fixture binary')
    artifact_parent = Path(args.artifacts).absolute()
    check(artifact_parent.parent.resolve(strict=True) == artifact_parent.parent, 'Artifact parent must be canonical')
    artifact_parent.mkdir(mode=0o700, exist_ok=True)
    info = artifact_parent.lstat()
    check(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700 and
          artifact_parent.resolve(strict=True) == artifact_parent, 'Artifacts must be an owned real 0700 directory')
    artifacts = Path(tempfile.mkdtemp(prefix='run-', dir=artifact_parent))
    processes, logs, publisher, publisher_lock = [], [], None, None
    report = {'scope': 'isolated software HUD; synthetic vehicle/navigation; no physical I/O or optics claim',
              'checks': []}
    with tempfile.TemporaryDirectory(prefix='hu-hud-wl-', dir='/tmp') as temporary:
        capsule = Path(temporary).resolve(strict=True)
        capsule.chmod(0o700)
        runtime, config = capsule / 'runtime', capsule / 'config'
        runtime.mkdir(mode=0o700)
        config.mkdir(mode=0o700)
        (config / 'rc.xml').write_text('<?xml version="1.0"?><labwc_config><core><gap>0</gap></core></labwc_config>')
        for name in ('autostart', 'shutdown'):
            (config / name).write_text('#!/bin/sh\nexit 0\n')
        (config / 'environment').write_text('LABWC_UPDATE_ACTIVATION_ENV=0\n')
        (config / 'menu.xml').write_text('<?xml version="1.0"?><openbox_menu/>')
        env = dict(os.environ)
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DBUS_SESSION_BUS_ADDRESS',
                    'LABWC_PID', 'XAUTHORITY', 'XDG_SESSION_ID', 'WLR_DRM_DEVICES', 'WAYLAND_DEBUG'):
            env.pop(key, None)
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config), XDG_CONFIG_DIRS=str(config),
                   WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='3', WLR_RENDERER='pixman',
                   LIBSEAT_BACKEND='noop', LABWC_UPDATE_ACTIVATION_ENV='0')

        def launch(command, label, trace=False):
            log = artifacts / (label + '.log')
            descriptor = os.open(log, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            stream = os.fdopen(descriptor, 'wb')
            logs.append(stream)
            child_env = dict(env)
            if trace:
                child_env['WAYLAND_DEBUG'] = 'client'
            child = subprocess.Popen(command, env=child_env, stdout=stream, stderr=subprocess.STDOUT,
                                     start_new_session=True)
            processes.append(child)
            return child, log

        def image(output, label):
            destination = artifacts / (label + '-' + output + '.ppm')
            subprocess.run([args.grim, '-o', output, '-t', 'ppm', str(destination)], env=env,
                           check=True, timeout=4, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            destination.chmod(0o600)
            image = ppm(destination)
            check(image[:2] == ((1280, 720) if output == OUTPUTS[2] else (960, 360)),
                  'Fixture output has unexpected geometry')
            return image

        def shot(output, label):
            return hashlib.sha256(image(output, label)[2]).hexdigest()

        def navigation_pixels(label):
            # The HUD's right-hand navigation region excludes speed and the
            # simulation label. Compare pixels, without OCR or assumed wording.
            width, height, pixels = image(OUTPUTS[2], label)
            start = int(width * .56)
            region = b''.join(pixels[(row * width + start) * 3:(row + 1) * width * 3]
                              for row in range(height))
            return hashlib.sha256(region).hexdigest()

        def unchanged(label):
            for output in OUTPUTS[:2]:
                check(shot(output, label) == baseline[output], 'HUD changed main or cluster output')

        def hud_config(output):
            path = config / 'hud.conf'
            path.write_text('output=' + output + '\nwayland=' + env['WAYLAND_DISPLAY'] +
                            '\nmain_output=HEADLESS-1\ncluster_output=HEADLESS-2\n'
                            'width=960\nheight=360\nfps=20\nreflect_horizontal=false\n')
            path.chmod(0o600)
            return str(path)

        try:
            compositor, _ = launch([args.labwc, '--config-dir', str(config), '--config', str(config / 'rc.xml')], 'labwc')

            def ready():
                check(compositor.poll() is None, 'Owned software compositor exited')
                sockets = [p for p in runtime.glob('wayland-*') if stat.S_ISSOCK(p.lstat().st_mode)]
                if len(sockets) == 1:
                    env['WAYLAND_DISPLAY'] = sockets[0].name
                    return True
                return False

            wait(ready, 'private three-output compositor startup')
            command = [args.wlr_randr]
            for index, output in enumerate(OUTPUTS):
                command += ['--output', output, '--custom-mode',
                            '1280x720@60Hz' if output == OUTPUTS[2] else '960x360@60Hz',
                            '--pos', str(index * 960) + ',0']
            subprocess.run(command, env=env, check=True, timeout=4, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            baseline = {output: shot(output, 'baseline') for output in OUTPUTS}

            absent, absent_log = launch([args.hud, '--config', hud_config('HEADLESS-absent')], 'missing-output', True)
            check(absent.wait(timeout=4) == 1, 'Missing HUD must fail without choosing a fallback output')
            check('configured HUD output is unavailable' in absent_log.read_text(), 'Wrong missing-output failure')
            check('.create_surface(' not in absent_log.read_text(), 'Missing HUD created a surface')
            check(all(shot(output, 'missing-output') == baseline[output] for output in OUTPUTS),
                  'Missing HUD changed an output')
            report['checks'].append('missing named output creates no surface or fallback')

            hud, _ = launch([args.hud, '--config', hud_config(OUTPUTS[2])], 'unavailable-inputs', True)
            wait(lambda: hud.poll() is None and shot(OUTPUTS[2], 'unavailable-inputs') != baseline[OUTPUTS[2]],
                 'unavailable HUD presentation')
            unchanged('unavailable-inputs')
            duplicate, duplicate_log = launch([args.hud, '--config', hud_config(OUTPUTS[2])], 'duplicate-owner', True)
            check(duplicate.wait(timeout=3) == 1, 'Second HUD owner was admitted')
            duplicate_trace = duplicate_log.read_text()
            check('HUD output already owned or unsafe owner lock' in duplicate_trace and
                  '.create_surface(' not in duplicate_trace, 'Second HUD created a competing surface')
            check(hud.poll() is None and shot(OUTPUTS[2], 'duplicate-owner') != baseline[OUTPUTS[2]],
                  'Second invocation interrupted the original HUD')
            unchanged('duplicate-owner')
            report['checks'].append('second owner refused before surface creation; first HUD remains visible')
            hud.send_signal(signal.SIGTERM)
            check(hud.wait(timeout=3) == 0, 'HUD SIGTERM did not retire orderly')
            wait(lambda: shot(OUTPUTS[2], 'retired') == baseline[OUTPUTS[2]], 'HUD surface withdrawal')
            report['checks'].append('unavailable inputs render only on HUD; SIGTERM withdraws orderly')

            killed, _ = launch([args.hud, '--config', hud_config(OUTPUTS[2])], 'killed-owner', True)
            wait(lambda: killed.poll() is None and shot(OUTPUTS[2], 'killed-owner') != baseline[OUTPUTS[2]],
                 'replacement HUD after orderly retirement')
            killed.send_signal(signal.SIGKILL)
            check(killed.wait(timeout=3) == -signal.SIGKILL, 'Explicit fixture owner did not stop')
            wait(lambda: shot(OUTPUTS[2], 'killed-owner-retired') == baseline[OUTPUTS[2]],
                 'killed HUD surface withdrawal')
            unchanged('killed-owner-retired')

            nav_endpoint = runtime / 'navigation.sock'
            road_file = runtime / 'road-observation'
            hud, trace = launch([args.hud, '--config', hud_config(OUTPUTS[2]), '--simulate-vehicle',
                                 '--navigation-socket', str(nav_endpoint), '--speed-limit-file', str(road_file)],
                                'simulated-vehicle', True)
            wait(lambda: hud.poll() is None and shot(OUTPUTS[2], 'simulated-vehicle') != baseline[OUTPUTS[2]],
                 'explicit synthetic HUD presentation')
            report['checks'].append('killed owner releases kernel lock; fresh replacement presents on same HUD')
            wait(lambda: nav_endpoint.exists() and stat.S_ISSOCK(nav_endpoint.lstat().st_mode),
                 'actual HUD navigation endpoint')
            publisher_lock = os.open(str(nav_endpoint) + '.publisher.lock', os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            fcntl.flock(publisher_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            publisher = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            publisher.setblocking(False)
            epoch = time.monotonic_ns()
            changed = epoch
            empty_navigation = navigation_pixels('navigation-absent')

            def publish(available=True, malformed=False):
                sent = time.monotonic_ns()
                packet = navigation_packet(epoch, changed, sent, available)
                if malformed:
                    packet = b'BAD1' + packet[4:]
                check(publisher.sendto(packet, str(nav_endpoint)) == len(packet), 'Fixture navigation send failed')
                return sent

            publish()
            wait(lambda: navigation_pixels('navigation-live') != empty_navigation,
                 'actual HUD route presentation')
            # Source-change date remains fixed while a real 4 Hz sender renews
            # its live-owner lease for longer than that lease's two seconds.
            deadline = time.monotonic() + 2.5
            while time.monotonic() < deadline:
                publish()
                time.sleep(.25)
                check(hud.poll() is None and navigation_pixels('navigation-stationary') != empty_navigation,
                      'stationary route disappeared or source date required artificial renewal')
            unchanged('navigation-stationary')
            report['navigationSourceChangedNs'] = changed
            report['checks'].append('actual HUD socket receives route; fixed source date survives real 4 Hz owner lease')
            publish(False)
            wait(lambda: navigation_pixels('navigation-withdrawn') == empty_navigation,
                 'explicit navigation withdrawal')
            check(shot(OUTPUTS[2], 'vehicle-after-withdrawal') != baseline[OUTPUTS[2]],
                  'route withdrawal lost vehicle display')
            unchanged('navigation-withdrawn')
            report['checks'].append('explicit route withdrawal clears navigation and retains vehicle/output')
            changed = time.monotonic_ns()
            last_good = publish()
            wait(lambda: navigation_pixels('navigation-recovered') != empty_navigation,
                 'new source route recovery')
            # Stop healthy heartbeats. Fresh malformed datagrams must not keep
            # the previous route alive after its original send-time lease.
            deadline = last_good / 1_000_000_000 + 1.5
            while time.monotonic() < deadline:
                publish(malformed=True)
                time.sleep(.25)
            check(navigation_pixels('navigation-before-expiry') != empty_navigation,
                  'route lease expired before its two-second send-time bound')
            deadline = last_good / 1_000_000_000 + 2.6
            while navigation_pixels('navigation-expired') != empty_navigation:
                check(time.monotonic() < deadline, 'expired/malformed publisher retained HUD navigation')
                publish(malformed=True)
                time.sleep(.05)
            report['navigationExpiryObservedMs'] = (time.monotonic_ns() - last_good) / 1_000_000
            report['checks'].append('stopped owner expires; fresh malformed datagrams cannot renew navigation')
            def road(country, value, unit):
                pending = runtime / 'road-pending'
                pending.write_text(f'hud-speed-limit-v2 value={value} unit={unit} '
                                   f'acquired_ns={time.monotonic_ns()} maximum_age_ns=10000000000 '
                                   f'origin=simulation source=country-fixture country={country}\n')
                pending.chmod(0o600)
                pending.replace(road_file)

            def face(label, kind):
                width, height, pixels = image(OUTPUTS[2], label)
                configured = re.search(r'zwlr_layer_surface_v1#\d+\.configure\(\d+, (\d+), (\d+)\)',
                                       trace.read_text())
                check(configured is not None, 'HUD lacks acknowledged surface geometry')
                canvas_width, canvas_height = map(int, configured.groups())
                scale = min(canvas_width / 960, canvas_height / 360)
                def pixel(x, y):
                    # All four anchors center the requested surface. Fit uses
                    # the acknowledged canvas, which may differ from output mode.
                    px = int((width - 960 * scale) / 2 + x * scale)
                    py = int((height - 360 * scale) / 2 + y * scale)
                    offset = (py * width + px) * 3
                    return tuple(pixels[offset:offset + 3])
                if kind == 'circle':
                    r, g, _ = pixel(447, 115)
                    return r > 200 and g < 100 and pixel(447, 140) == (255, 255, 255)
                if kind == 'rectangle':
                    return pixel(404, 108) == (255, 255, 255) and pixel(447, 115) == (255, 255, 255)
                return pixel(404, 108) == (0, 0, 0) and pixel(447, 140) == (0, 0, 0)

            road('FR', 90, 'km/h')
            wait(lambda: face('country-fr', 'circle'), 'live French red-ring sign')
            road('US', 55, 'mph')
            wait(lambda: face('country-us', 'rectangle'), 'atomic border switch to US rectangular sign')
            road('GB', 50, 'mph')
            wait(lambda: face('country-gb', 'circle'), 'live UK red-ring sign')
            road_file.unlink()
            wait(lambda: face('country-withdrawn', 'absent'), 'withdrawal clears country and speed-limit sign')
            unchanged('country-withdrawn')
            report['checks'].append('actual provider file switches FR/US/GB signs atomically; withdrawal clears sign')
            unchanged('simulated-vehicle')
            subprocess.run([args.wlr_randr, '--output', OUTPUTS[2], '--off'], env=env,
                           check=True, timeout=4, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            check(hud.wait(timeout=3) == 0, 'HUD output withdrawal did not retire orderly')
            unchanged('output-withdrawn')
            check(trace.stat().st_size <= 1_048_576, 'HUD protocol trace exceeded its bound')
            text = trace.read_text()
            check('set_keyboard_interactivity(0)' in text and '.set_input_region(wl_region' in text,
                  'HUD did not request noninteractive presentation')
            check(not re.search(r'\.bind\([^\n]*"wl_seat"', text) and 'wl_touch#' not in text,
                  'HUD acquired an input device')
            releases = len(re.findall(r'wl_buffer#\d+\.release\(', text))
            check(releases >= 6, 'HUD did not recycle released shared-memory buffers')
            commits = [float(value) for value in re.findall(r'\[\s*([0-9.]+)\]\s+(?:\{[^}]*\}\s+)?-> wl_surface#\d+\.commit\(', text)]
            check(len(commits) >= 12, 'HUD made insufficient bounded presentation progress')
            # Wayland debug dates are actual client submission dates in ms.
            # Ignore the initial empty configure commit; these are not scanout receipts.
            gaps = [b - a for a, b in zip(commits[1:], commits[2:])]
            check(gaps and min(gaps) >= 49, 'HUD exceeded its configured 20 Hz submission bound')
            report.update(bufferReleases=releases, bufferCommits=len(commits) - 1,
                          minimumSubmissionIntervalMs=min(gaps))
            report['checks'].append('released buffers reused; bounded submission cadence; no input owner')
            report['checks'].append('withdrawn HUD output retires without main/cluster fallback')
        finally:
            if publisher:
                publisher.close()
            if publisher_lock is not None:
                os.close(publisher_lock)
            for child in reversed(processes):
                stop(child)
            for stream in logs:
                stream.close()
            for path in artifacts.iterdir():
                check(path.stat().st_size <= 1_048_576 or path.suffix == '.ppm', 'Fixture log exceeds bound')
            (artifacts / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            (artifacts / 'report.json').chmod(0o600)
    print('Native HUD software Wayland placement, pacing, input isolation and retirement passed; evidence: ' + str(artifacts))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hud', required=True)
    parser.add_argument('--labwc', default='/usr/bin/labwc')
    parser.add_argument('--grim', default='/usr/bin/grim')
    parser.add_argument('--wlr-randr', default='/usr/bin/wlr-randr')
    parser.add_argument('--artifacts', required=True)
    harness(parser.parse_args())
