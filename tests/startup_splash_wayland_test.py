#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Opt-in real GTK/vector/layer lifecycle on a private headless labwc.

The colored underlay/normal foreground are explicit native/phone substitutes.
This does not contact a physical output, receiver, radio or real phone.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
import time


def client(mode, fault, tools):
    runtime = Path(os.environ.get('XDG_RUNTIME_DIR', '/missing'))
    assert runtime.parent.name.startswith('hu-splash-wl-') and os.environ.get('WLR_BACKENDS') == 'headless', \
        'Fixture clients require the private headless harness; never use a physical session'
    if fault == 'gpu':
        os.environ['GSK_RENDERER'] = 'cairo'
    sys.path.insert(0, tools)
    ctypes.CDLL('libgtk4-layer-shell.so.0', mode=ctypes.RTLD_GLOBAL)
    import gi
    for name, version in [('Gtk', '4.0'), ('Gdk', '4.0'), ('Gtk4LayerShell', '1.0')]:
        gi.require_version(name, version)
    from gi.repository import Gtk, Gdk, GLib, Gio, Gtk4LayerShell as layer
    from phone_page_presentation import BootSplash, claim_boot_splash
    app = Gtk.Application(application_id='org.headunit.splashfixture', flags=Gio.ApplicationFlags.NON_UNIQUE)
    retained = []

    def activate(page):
        page.hold()
        display = Gdk.Display.get_default()
        monitors = display.get_monitors()
        targets = [monitors.get_item(i) for i in range(monitors.get_n_items())]
        targets.sort(key=lambda m: m.get_connector())
        if mode == 'splash':
            if fault == 'claim' and not claim_boot_splash(os.environ['XDG_RUNTIME_DIR'], elapsed=2):
                print('replay skipped', flush=True)
                page.quit()
                return
            if fault == 'missing':
                import phone_icons
                def resource(*args):
                    raise ValueError('Fixture missing vector support')
                phone_icons.startup_canvas = resource
            outputs = ('HEADLESS-1', 'HEADLESS-2') if fault != 'output' else ('HEADLESS-1', 'ABSENT')
            splash = BootSplash(page, layer, display, outputs, Gtk, GLib)
            retained.append(splash)
            if fault == 'stall':
                splash.drawn = lambda *_: None  # Explicit failure to construct a startup scene.
            if fault == 'retire':
                GLib.timeout_add(1200, lambda: (splash.close('phone-ready'), False)[1])
            if fault == 'render':
                GLib.timeout_add(1200, lambda: (splash.render_failed(), False)[1])
            if fault == 'close':
                GLib.timeout_add(1200, lambda: (splash.windows[0].emit('close-request'), False)[1])
            GLib.timeout_add(7300, lambda: (page.quit(), False)[1])
        else:
            provider = Gtk.CssProvider()
            provider.load_from_data(b'.fixture-main { background: #004080; } .fixture-cluster { background: #800040; } .fixture-phone { background: #00c040; } button.fixture-input { background: transparent; background-image: none; border: none; box-shadow: none; border-radius: 0; }')
            Gtk.StyleContext.add_provider_for_display(display, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
            for i, monitor in enumerate(targets[:1] if mode == 'phone' else targets):
                window = Gtk.ApplicationWindow(application=page)
                window.add_css_class('fixture-phone' if mode == 'phone' else ('fixture-main' if i == 0 else 'fixture-cluster'))
                window.set_decorated(False)
                if mode == 'background' and i == 0:
                    button = Gtk.Button()
                    button.add_css_class('fixture-input')
                    button.set_focus_on_click(False)
                    button.connect('clicked', lambda *_: print('native underlay received pointer', flush=True))
                    window.set_child(button)
                if mode == 'phone':
                    window.fullscreen_on_monitor(monitor)
                else:
                    layer.init_for_window(window)
                    layer.set_monitor(window, monitor)
                    layer.set_layer(window, layer.Layer.BACKGROUND)
                    layer.set_keyboard_mode(window, layer.KeyboardMode.NONE)
                    layer.set_exclusive_zone(window, 0)
                    for edge in (layer.Edge.TOP, layer.Edge.BOTTOM, layer.Edge.LEFT, layer.Edge.RIGHT):
                        layer.set_anchor(window, edge, True)
                window.present()
                retained.append(window)
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, lambda: (page.quit(), False)[1])

    app.connect('activate', activate)
    result = app.run([])
    for obj in retained:
        if isinstance(obj, BootSplash):
            obj.close()
    return result


def harness(args):
    assert sys.platform.startswith('linux') and os.getuid() != 0, 'Nonroot isolated Linux fixture required'
    artifacts = Path(args.artifacts).absolute()
    artifacts.mkdir(mode=0o700, exist_ok=True)
    report = {'scope': 'real GTK/GSK vector on isolated Wayland; GPU backend recorded separately; native/phone colored substitutes', 'checks': []}
    children = []
    logs = []
    with tempfile.TemporaryDirectory(prefix='hu-splash-wl-') as tmp:
        capsule = Path(tmp)
        runtime, config = capsule / 'runtime', capsule / 'config'
        runtime.mkdir(mode=0o700)
        config.mkdir(mode=0o700)
        (config / 'rc.xml').write_text('<labwc_config><core><gap>0</gap></core></labwc_config>')
        (config / 'environment').write_text('LABWC_UPDATE_ACTIVATION_ENV=0\n')
        for name in ('autostart', 'shutdown'):
            (config / name).write_text('#!/bin/sh\nexit 0\n')
        env = {k: v for k, v in os.environ.items() if k not in
               ('DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DBUS_SESSION_BUS_ADDRESS', 'LABWC_PID', 'XAUTHORITY', 'WAYLAND_DEBUG')}
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config), XDG_CONFIG_DIRS=str(config),
                   WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='2', WLR_RENDERER='pixman', GSK_RENDERER='opengl',
                   GDK_BACKEND='wayland', GTK_A11Y='none', LIBSEAT_BACKEND='noop', LABWC_UPDATE_ACTIVATION_ENV='0')
        if args.gpu_compositor:
            env.update(WLR_RENDERER='gles2', WLR_RENDER_DRM_DEVICE='/dev/dri/renderD128')

        def launch(command, name):
            stream = (artifacts / (name + '.log')).open('wb')
            logs.append(stream)
            child = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            children.append(child)
            return child

        def stop(child, kill=False):
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL if kill else signal.SIGTERM)
                child.wait(timeout=4)

        def wait(predicate, timeout=4):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if predicate():
                    return
                time.sleep(.05)
            raise AssertionError('Fixture deadline expired')

        def image(output):
            raw = subprocess.check_output(['/usr/bin/grim', '-o', output, '-t', 'ppm', '-'], env=env, timeout=4)
            return raw.split(b'\n', 3)[3]

        def same():
            return all(hashlib.sha256(image(name)).hexdigest() == baseline[name] for name in ('HEADLESS-1', 'HEADLESS-2'))

        def splash(name, fault='none'):
            return launch([sys.executable, '-B', __file__, '--client', 'splash', '--fault', fault, '--tools', args.tools], name)

        def retired(name, reason, timeout=7):
            wait(lambda: ('retired: ' + reason) in (artifacts / (name + '.log')).read_text(), timeout)
            wait(same)
            report['checks'].append(name)

        try:
            compositor = launch(['/usr/bin/labwc', '-d', '--config-dir', str(config)], 'compositor')
            wait(lambda: any(stat.S_ISSOCK(p.lstat().st_mode) for p in runtime.glob('wayland-*')))
            env['WAYLAND_DISPLAY'] = next(p.name for p in runtime.glob('wayland-*') if stat.S_ISSOCK(p.lstat().st_mode))
            subprocess.run(['/usr/bin/wlr-randr', '--output', 'HEADLESS-1', '--custom-mode', '1280x720@60Hz', '--pos', '0,0',
                            '--output', 'HEADLESS-2', '--custom-mode', '1920x1080@60Hz', '--pos', '1280,0'], env=env, check=True, timeout=4)
            background = launch([sys.executable, '-B', __file__, '--client', 'background', '--tools', args.tools], 'background')
            wait(lambda: image('HEADLESS-1')[:3] == bytes([0, 64, 128]))
            baseline = {name: hashlib.sha256(image(name)).hexdigest() for name in ('HEADLESS-1', 'HEADLESS-2')}
            normal = splash('complete')
            wait(lambda: not same())
            time.sleep(1.8)
            for output in ('HEADLESS-1', 'HEADLESS-2'):
                pixels = image(output)
                assert sum(max(pixels[i:i + 3]) > 80 for i in range(0, len(pixels), 3)) > 1500, 'No rendered logo pixels'
                subprocess.run(['/usr/bin/grim', '-o', output, str(artifacts / (output + '-animation.png'))], env=env, check=True, timeout=4)
            report['checks'].append('live-vectors-both-full-size-outputs')
            scene_lines = [line for line in (artifacts / 'complete.log').read_text().splitlines()
                           if line.startswith('Headunit splash first scene constructed ')]
            assert len(scene_lines) == 2, 'Expected one first-scene observation per output'
            scenes = [dict(field.split('=', 1) for field in line.split()[4:]) for line in scene_lines]
            assert {row['output'] for row in scenes} == {'HEADLESS-1', 'HEADLESS-2'}
            assert all(int(row['mono_ns']) > 0 for row in scenes)
            report['firstSceneConstruction'] = scenes
            report['checks'].append('bounded-first-scene-observations-both-outputs')
            from native_home_wayland_test import FixturePointer
            pointer = FixturePointer(runtime, env['WAYLAND_DISPLAY'], prefix='hu-splash-wl-')
            try:
                pointer.click_at(640, 360)
                wait(lambda: 'native underlay received pointer' in (artifacts / 'background.log').read_text())
                assert 'retired:' not in (artifacts / 'complete.log').read_text(), 'Pointer arrived after decoration retired'
            finally:
                pointer.close()
            report['checks'].append('pointer-passes-through-live-decoration')
            phone = launch([sys.executable, '-B', __file__, '--client', 'phone', '--tools', args.tools], 'phone')
            wait(lambda: image('HEADLESS-1')[:3] == bytes([0, 192, 64]))
            report['checks'].append('normal-foreground-preempts-splash')
            stop(phone)
            retired('complete', 'complete')
            stop(normal)
            fading = splash('smooth-exit')
            wait(lambda: not same())
            # At a clear corner, the opaque startup is nearly black and the
            # native main underlay is blue. Intermediate pixels demonstrate
            # an actual compositor blend, including the window background.
            wait(lambda: 16 < image('HEADLESS-1')[2] < 112, timeout=6)
            cluster_pixel = image('HEADLESS-2')[:3]
            assert 8 < cluster_pixel[0] < 128 and 0 < cluster_pixel[2] < 64, \
                'Cluster did not dissolve into its native underlay'
            assert 'retired:' not in (artifacts / 'smooth-exit.log').read_text(), \
                'Underlay appeared only after abrupt retirement'
            report['checks'].append('both-outputs-blend-before-retirement')
            retired('smooth-exit', 'complete')
            stop(fading)
            for name, fault, reason in [('missing-resource', 'missing', 'unavailable'), ('gpu-unavailable', 'gpu', 'render-error'),
                                        ('absent-output', 'output', 'unavailable'), ('early-handoff', 'retire', 'phone-ready'),
                                        ('render-failure', 'render', 'render-error'), ('window-close', 'close', 'surface-withdrawn'),
                                        ('bounded-stall', 'stall', 'deadline')]:
                child = splash(name, fault)
                retired(name, reason)
                stop(child)
            child = splash('death', 'claim')
            wait(lambda: not same())
            stop(child, kill=True)
            wait(same)
            report['checks'].append('process-death-reveals-underlays')
            child = splash('restart', 'claim')
            child.wait(timeout=4)
            assert 'replay skipped' in (artifacts / 'restart.log').read_text()
            assert same()
            report['checks'].append('restart-does-not-replay')
            benchmark = splash('unobserved-rate')
            benchmark.wait(timeout=9)  # No capture polling during moving frames.
            rates = [json.loads(line.split('Headunit splash vector output ', 1)[1])
                     for line in (artifacts / 'unobserved-rate.log').read_text().splitlines()
                     if line.startswith('Headunit splash vector output ')]
            assert {tuple(row['size']) for row in rates} == {(1280, 720), (1920, 1080)}
            assert all(row['renderer'] == 'GskGLRenderer' and row['movingSnapshots'] > 1 for row in rates)
            report['snapshotMeasurements'] = rates
            if args.gpu_compositor:
                assert all(row['movingSnapshots'] / row['movingSeconds'] >= 30 for row in rates), 'Below accepted 30 snapshot/s target'
            report['checks'].append('unobserved-native-size-snapshot-rate')
            if args.regression_tests:
                for name in ('native_phone_page_test.py', 'native_phone_pairing_page_test.py',
                             'native_phone_page_resident_test.py', 'native_phone_artwork_test.py'):
                    with (artifacts / (name + '.log')).open('wb') as log:
                        subprocess.run([sys.executable, '-B', str(Path(args.regression_tests) / name)],
                                       env=env, check=True, timeout=35, stdout=log, stderr=subprocess.STDOUT)
                    report['checks'].append(name)
            report['result'] = 'passed'
        finally:
            for child in reversed(children):
                stop(child)
            for stream in logs:
                stream.close()
            (artifacts / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', choices=['splash', 'background', 'phone'])
    parser.add_argument('--fault', default='none')
    parser.add_argument('--tools', default=str(Path(__file__).resolve().parents[1] / 'tools'))
    parser.add_argument('--artifacts', default='splash-wayland-evidence')
    parser.add_argument('--regression-tests')
    parser.add_argument('--gpu-compositor', action='store_true', help='explicit CM5 render-node headless GPU fixture')
    args = parser.parse_args()
    if args.client:
        raise SystemExit(client(args.client, args.fault, args.tools))
    harness(args)
