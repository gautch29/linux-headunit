#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Original real layer/window fixture on an OWNED software headless labwc.

No physical compositor, receiver, Bluetooth, phone or camera is contacted.
Manager replies and colored video/background markers are explicit fixtures.
The GTK clients use the real Gtk4LayerShell API and real HEADLESS monitors.
Screenshots qualify compositor stacking/withdrawal, not physical touch,
scanout, CarPlay continuity, reverse coordination or rendering performance.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import socketserver
import struct
import stat
import subprocess
import sys
import tempfile
import threading
import time


MAIN, CLUSTER = 'HEADLESS-1', 'HEADLESS-2'


class FixturePointer:
    """Real virtual-pointer protocol, restricted to the private HEADLESS seat.

    This original tiny client sends compositor input rather than emitting GTK
    signals. It is a pointer-routing oracle; it does not impersonate wl_touch.
    Protocol requests follow swaywm/wlr-protocols' virtual-pointer v2 contract.
    """
    def __init__(self, runtime, display_name, *, prefix='hu-home-wl-'):
        # Reuse the fixture capsule admission before opening any display socket.
        check(prefix in ('hu-home-wl-', 'hu-splash-wl-') and
              private_directory(runtime).parent.name.startswith(prefix), 'Pointer runtime is not isolated')
        check(Path(display_name).name == display_name and display_name.startswith('wayland-'), 'Invalid private display')
        self.connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.connection.settimeout(2)
        self.connection.connect(str(runtime / display_name))
        self.next_id, self.registry, self.globals, self.outputs = 2, None, {}, {}
        self.registry = self.new_id()
        self.send(1, 1, self.words(self.registry))  # wl_display.get_registry
        self.roundtrip()
        managers = [item for item in self.globals.items() if item[1][0] == 'zwlr_virtual_pointer_manager_v1']
        check(len(managers) == 1 and managers[0][1][1] >= 2, 'Private compositor lacks virtual-pointer v2')
        self.manager = self.bind(managers[0][0], 'zwlr_virtual_pointer_manager_v1', 2)
        for name, (interface, version) in self.globals.items():
            if interface == 'wl_output':
                check(version >= 4, 'Fixture requires named wl_output v4')
                self.outputs[self.bind(name, interface, 4)] = ''
        check(len(self.outputs) == 2, 'Pointer fixture must expose exactly two outputs')
        self.roundtrip()
        selected = [identifier for identifier, name in self.outputs.items() if name == MAIN]
        check(len(selected) == 1 and sorted(self.outputs.values()) == [MAIN, CLUSTER], 'Pointer output admission failed')
        self.pointer = self.new_id()
        self.send(self.manager, 2, self.words(0, selected[0], self.pointer))
        self.roundtrip()

    @staticmethod
    def words(*values):
        return struct.pack('=' + 'I' * len(values), *values)

    @staticmethod
    def string(value):
        data = value.encode('ascii') + b'\0'
        return FixturePointer.words(len(data)) + data + b'\0' * ((-len(data)) % 4)

    @staticmethod
    def read_string(data, offset=0):
        check(len(data) >= offset + 4, 'Truncated Wayland string')
        length = struct.unpack_from('=I', data, offset)[0]
        check(1 <= length <= 256 and len(data) >= offset + 4 + length and data[offset + 3 + length] == 0,
              'Malformed Wayland string')
        value = data[offset + 4:offset + 3 + length].decode('ascii')
        return value, offset + 4 + ((length + 3) & ~3)

    def new_id(self):
        identifier = self.next_id
        self.next_id += 1
        check(self.next_id < 256, 'Fixture Wayland object bound exceeded')
        return identifier

    def send(self, identifier, opcode, data=b''):
        check(len(data) <= 1024 and len(data) % 4 == 0, 'Invalid fixture Wayland request')
        self.connection.sendall(self.words(identifier, ((8 + len(data)) << 16) | opcode) + data)

    def bind(self, name, interface, version):
        identifier = self.new_id()
        self.send(self.registry, 0, self.words(name) + self.string(interface) + self.words(version, identifier))
        return identifier

    def exact(self, length):
        result = b''
        while len(result) < length:
            part = self.connection.recv(length - len(result))
            check(part, 'Private Wayland connection closed')
            result += part
        return result

    def roundtrip(self):
        callback = self.new_id()
        self.send(1, 0, self.words(callback))
        for _ in range(512):
            identifier, header = struct.unpack('=II', self.exact(8))
            size, opcode = header >> 16, header & 0xffff
            check(8 <= size <= 4096 and size % 4 == 0, 'Malformed private Wayland event')
            data = self.exact(size - 8)
            if identifier == 1 and opcode == 0:
                raise AssertionError('Private Wayland server rejected input fixture')
            if identifier == callback and opcode == 0:
                return
            if identifier == self.registry and opcode == 0:
                check(len(data) >= 12 and len(self.globals) < 256, 'Registry event bound exceeded')
                name = struct.unpack_from('=I', data)[0]
                interface, end = self.read_string(data, 4)
                check(len(data) == end + 4, 'Malformed global version')
                self.globals[name] = (interface, struct.unpack_from('=I', data, end)[0])
            elif identifier in self.outputs and opcode == 4:
                self.outputs[identifier] = self.read_string(data)[0]
        raise AssertionError('Private Wayland roundtrip event bound exceeded')

    def click_home(self):
        self.click_at(1280 - MARGIN - SIZE // 2, 720 - MARGIN - SIZE // 2)

    def click_at(self, x, y):
        check(type(x) is int and type(y) is int and 0 <= x < 1280 and 0 <= y < 720,
              'Pointer position outside private main output')
        now = int(time.monotonic() * 1000) & 0xffffffff
        self.send(self.pointer, 1, self.words(now, x, y, 1280, 720))
        self.send(self.pointer, 4)
        self.roundtrip()
        self.send(self.pointer, 2, self.words(now, 0x110, 1))
        self.send(self.pointer, 4)
        self.roundtrip()
        time.sleep(.04)
        self.send(self.pointer, 2, self.words((now + 40) & 0xffffffff, 0x110, 0))
        self.send(self.pointer, 4)
        self.roundtrip()

    def close(self):
        self.send(self.pointer, 8)
        self.send(self.manager, 1)
        self.roundtrip()
        self.connection.close()
GREEN, BLUE, AMBER = (38, 130, 70), (34, 80, 180), (190, 90, 28)
SIZE, MARGIN = 64, 16
MAX_IMAGE = 2048 * 2048 * 3 + 256


def check(value, message):
    if not value:
        raise AssertionError(message)


def gpu_status_snapshot(path):
    """Read one bounded publication, retaining its inode across the read.

    The writer atomically replaces this name. Even lstat can see the retired
    inode with zero links during rename; that sample is unavailable, never
    accepted. All other unsafe metadata and stable inode rewrites still fail.
    """
    def admissible(info, *, retired=False):
        check(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and
              info.st_nlink in ((0, 1) if retired else (1,)) and
              stat.S_IMODE(info.st_mode) == 0o600 and 0 < info.st_size <= 2048,
              'Unsafe GPU fixture status')

    def identity(info):
        return info.st_dev, info.st_ino

    def version(info):
        return (identity(info), info.st_mode, info.st_uid, info.st_nlink,
                info.st_size, info.st_mtime_ns, info.st_ctime_ns)

    for _ in range(4):
        try:
            descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
        except FileNotFoundError:
            return None
        except OSError as error:
            raise AssertionError('Unsafe GPU fixture status') from error
        try:
            before = os.fstat(descriptor)
            admissible(before, retired=True)
            data = b''
            while len(data) <= 2048:
                part = os.read(descriptor, 2049 - len(data))
                if not part:
                    break
                data += part
            after = os.fstat(descriptor)
            admissible(after, retired=True)
            named = path.lstat()
            admissible(named, retired=True)
            if 0 in (before.st_nlink, after.st_nlink, named.st_nlink) or identity(named) != identity(after):
                # Replacement/retirement is unavailable. Never retry an invalid
                # mode, owner, hard link, symlink or oversized publication.
                continue
            admissible(before)
            admissible(after)
            admissible(named)
            check(version(before) == version(after) == version(named) and len(data) == after.st_size,
                  'GPU fixture status changed without atomic replacement')
            return json.loads(data)
        finally:
            os.close(descriptor)
    # Existing bounded scene waits can sample again after publication settles.
    return None


def gpu_status_self_test():
    """Deterministic real-file races; no compositor or renderer is started."""
    from unittest.mock import patch

    with tempfile.TemporaryDirectory(prefix='home-status-test-') as directory:
        path = Path(directory) / 'gpu-status.json'

        def publish(phase):
            temporary = path.with_suffix('.tmp')
            temporary.write_text(json.dumps({'phase': phase}))
            temporary.chmod(0o600)
            os.replace(temporary, path)

        real_open, real_read = os.open, os.read
        for boundary in ('open', 'read'):
            publish('old')
            replaced = False

            def replace_after_open(*args, **kwargs):
                nonlocal replaced
                descriptor = real_open(*args, **kwargs)
                if not replaced:
                    replaced = True
                    publish('new')
                return descriptor

            def replace_after_read(*args, **kwargs):
                nonlocal replaced
                data = real_read(*args, **kwargs)
                if not replaced:
                    replaced = True
                    publish('new')
                return data

            with patch('os.' + boundary, replace_after_open if boundary == 'open' else replace_after_read):
                check(gpu_status_snapshot(path) == {'phase': 'new'}, 'Retired publication was accepted')

        publish('old')
        replacements = 0

        def always_replace(*args, **kwargs):
            nonlocal replacements
            descriptor = real_open(*args, **kwargs)
            replacements += 1
            publish('replacement-' + str(replacements))
            return descriptor

        with patch('os.open', always_replace):
            check(gpu_status_snapshot(path) is None and replacements == 4,
                  'Publication retry was unbounded or accepted a retired inode')

        publish('old')
        rewritten = False

        def rewrite_after_read(*args, **kwargs):
            nonlocal rewritten
            data = real_read(*args, **kwargs)
            if not rewritten:
                rewritten = True
                path.write_text('{"phase":"in-place-change"}')
            return data

        with patch('os.read', rewrite_after_read):
            try:
                gpu_status_snapshot(path)
            except AssertionError:
                pass
            else:
                raise AssertionError('In-place status mutation was accepted')

        for unsafe in ('mode', 'hardlink', 'symlink', 'oversized'):
            publish('safe')
            other = Path(directory) / 'other'
            if unsafe == 'mode':
                path.chmod(0o644)
            elif unsafe == 'hardlink':
                os.link(path, other)
            elif unsafe == 'symlink':
                os.replace(path, other)
                path.symlink_to(other)
            else:
                path.write_bytes(b'x' * 2049)
            try:
                gpu_status_snapshot(path)
            except AssertionError:
                pass
            else:
                raise AssertionError('Unsafe ' + unsafe + ' publication was accepted')
            path.unlink()
            if other.exists():
                other.unlink()
        check(gpu_status_snapshot(path) is None, 'Missing initial publication was accepted')
    print('Pinned GPU status replacement and unsafe-file checks passed')


def private_directory(path):
    path = Path(path)
    info = path.lstat()
    check(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700,
          'Fixture directory must be owned real 0700 data')
    check(path.resolve(strict=True) == path, 'Fixture directory cannot redirect through another path')
    return path


def fixture_endpoint():
    """Internal clients may only use this harness's new, token-marked runtime."""
    runtime = private_directory(os.environ['XDG_RUNTIME_DIR'])
    check(runtime.name == 'runtime' and runtime.parent.name.startswith('hu-home-wl-'), 'Not an owned fixture runtime')
    token = runtime / '.fixture-token'
    info = token.lstat()
    check(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_uid == os.getuid() and
          stat.S_IMODE(info.st_mode) == 0o600 and info.st_size == 32, 'Fixture token is unsafe')
    check(token.read_text() == os.environ.get('HEADUNIT_HOME_FIXTURE_TOKEN'), 'Fixture token mismatch')
    check(os.environ.get('WLR_BACKENDS') == 'headless' and os.environ.get('WLR_RENDERER') in ('pixman', 'gles2') and
          os.environ.get('GDK_BACKEND') == 'wayland', 'Fixture must use its own headless Wayland')
    display = os.environ.get('WAYLAND_DISPLAY', '')
    check(display.startswith('wayland-') and '/' not in display, 'Invalid fixture display name')
    endpoint = (runtime / display).lstat()
    check(stat.S_ISSOCK(endpoint.st_mode) and endpoint.st_uid == os.getuid(), 'Fixture Wayland endpoint is unsafe')
    return runtime


def load_gtk():
    fixture_endpoint()
    # Required upstream preload order: the fixed SONAME precedes every GI import.
    library = ctypes.CDLL('libgtk4-layer-shell.so.0', mode=ctypes.RTLD_GLOBAL)
    import gi
    gi.require_version('Gtk', '4.0')
    gi.require_version('Gdk', '4.0')
    gi.require_version('Gtk4LayerShell', '1.0')
    from gi.repository import Gtk, Gdk, Gio, GLib, Gtk4LayerShell as Layer
    check(Gtk.init_check(), 'GTK cannot open the owned headless display')
    check(Layer.is_supported() and Layer.get_protocol_version() >= 4, 'Real layer-shell protocol v4 is required')
    display = Gdk.Display.get_default()
    model = display.get_monitors()
    check(model.get_n_items() == 2, 'Fixture must expose exactly two real HEADLESS monitors')
    monitors = {model.get_item(i).get_connector(): model.get_item(i) for i in range(2)}
    check(set(monitors) == {MAIN, CLUSTER} and all(m.is_valid() for m in monitors.values()),
          'Fixture cannot use a physical, missing or ambiguous output')
    return library, Gtk, Gdk, Gio, GLib, Layer, display, monitors


class CommandServer(socketserver.UnixStreamServer):
    def __init__(self, path, GLib, dispatch):
        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                self.connection.settimeout(3)
                raw = self.rfile.readline(1025)
                check(len(raw) <= 1024 and raw.endswith(b'\n'), 'Invalid fixture command')
                value = json.loads(raw)
                finished, result = threading.Event(), {}

                def execute():
                    try:
                        result.update(ok=True, **dispatch(value))
                    except Exception as error:
                        result.update(ok=False, error=str(error)[:300])
                    finally:
                        finished.set()
                    return False

                GLib.idle_add(execute)
                check(finished.wait(3), 'Fixture GTK command timed out')
                self.wfile.write(json.dumps(result).encode() + b'\n')
        super().__init__(str(path), Handler)
        os.chmod(path, 0o600)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=2)


def close_display(display):
    display.flush()
    display.close()
    check(display.is_closed(), 'Owned fixture GDK connection did not close')


def run_markers():
    runtime = fixture_endpoint()
    library, Gtk, Gdk, Gio, GLib, Layer, display, monitors = load_gtk()
    app = Gtk.Application(application_id='org.headunit.TestHomeMarkers', flags=Gio.ApplicationFlags.NON_UNIQUE)
    windows, control, failure = {}, [], []

    def activate(_):
        try:
            css = Gtk.CssProvider()
            css.load_from_data(('window {border:0; padding:0;} ' + ''.join(
                f'window#{name} {{ background: rgb({color[0]},{color[1]},{color[2]}); }} '
                for name, color in [('base', AMBER), ('main', GREEN), ('cluster', BLUE)])).encode())
            Gtk.StyleContext.add_provider_for_display(display, css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
            for name, output, layer in [('base', MAIN, Layer.Layer.BACKGROUND), ('main', MAIN, Layer.Layer.TOP),
                                        ('cluster', CLUSTER, Layer.Layer.TOP)]:
                window = Gtk.ApplicationWindow(application=app, title='Original fixture ' + name)
                window.set_name(name)
                window.set_decorated(False)
                window.set_child(Gtk.Box())
                Layer.init_for_window(window)
                Layer.set_namespace(window, 'headunit-test-home-' + name)
                Layer.set_monitor(window, monitors[output])
                Layer.set_layer(window, layer)
                Layer.set_keyboard_mode(window, Layer.KeyboardMode.NONE)
                Layer.set_exclusive_zone(window, 0)
                for edge in (Layer.Edge.TOP, Layer.Edge.BOTTOM, Layer.Edge.LEFT, Layer.Edge.RIGHT):
                    Layer.set_anchor(window, edge, True)
                windows[name] = window
                window.present()

            def dispatch(value):
                check(type(value) is dict and set(value) == {'action'}, 'Invalid marker command')
                action = value['action']
                check(action in ('status', 'hide-main', 'show-main', 'quit'), 'Unknown marker command')
                if action == 'hide-main':
                    windows['main'].set_visible(False)
                elif action == 'show-main':
                    windows['main'].set_visible(True)
                elif action == 'quit':
                    for window in windows.values():
                        window.destroy()
                    app.quit()
                return {'outputs': sorted(monitors), 'mainVisible': windows['main'].get_visible()}

            control.append(CommandServer(runtime / 'markers.sock', GLib, dispatch))
        except Exception as error:
            failure.append(str(error))
            app.quit()
    app.connect('activate', activate)
    try:
        code = app.run([])
    finally:
        for server in control:
            server.close()
        monitors.clear()
        if not display.is_closed():
            close_display(display)
    check(code == 0 and not failure, failure or 'Marker application failed')


def run_page():
    runtime = fixture_endpoint()
    library, Gtk, Gdk, Gio, GLib, Layer, display, monitors = load_gtk()
    protocol = Layer.get_protocol_version()
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
    from phone_page import PhonePage
    from phone_page_presentation import ResidentPresentation
    from phone_page_configuration import PageOwner
    from main_surface_handoff import MainSurfaceHandoff
    profile = {'schemaVersion': 2, 'enabled': True, 'managerSocket': str(runtime / 'manager.sock'),
               'surfaceControlFile': str(runtime / 'home-surface.request'),
               'surfaceLeaseFile': str(runtime / 'home-surface.lease'),
               'waylandDisplay': os.environ['WAYLAND_DISPLAY'], 'output': MAIN,
               'homeButton': {'corner': 'bottom-right', 'margin': MARGIN, 'size': SIZE}}
    owner = PageOwner(runtime)
    handoff = MainSurfaceHandoff(profile['surfaceControlFile'], profile['surfaceLeaseFile'])
    release_timing={}
    release_owned=handoff.release
    def observe_release():
        # Observe the production OFF/unlock operation, keeping its entire real
        # ownership path. These dates bound release; they are not GPU receipts.
        release_timing['started']=time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        release_owned()
        release_timing['completed']=time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    handoff.release=observe_release
    presentation = ResidentPresentation(Layer, profile, handoff=handoff)
    app = PhonePage(runtime / 'manager.sock', presentation=presentation)
    controls = []

    def state():
        result = {'foreground': presentation.foreground,
                  'opening': presentation.opening, 'mainOwnership': handoff.acquired,
                  'homeVisible': bool(presentation.home_window.get_visible()),
                  'pending': app.pending, 'retired': app.ui_retired,
                  'displayClosed': display.is_closed(), 'protocol': protocol,
                  'cameraKnown': presentation.camera_known,
                  'cameraBlocked': presentation.camera_blocked,
                  'error': app.startup_error or presentation.cleanup_error}
        if not display.is_closed() and not app.closing:
            window = app.window
            if rows := getattr(app, 'phone_widgets', {}):
                controls = next(iter(rows.values()))['disconnect'].get_parent()
                menu = controls.get_last_child()
                ok, bounds = menu.compute_bounds(window)
                if ok:
                    result['phoneOptionsPoint'] = [int(bounds.get_x() + bounds.get_width() / 2),
                                                  int(bounds.get_y() + bounds.get_height() / 2)]
                result['phoneOptionsVisible'] = menu.get_popover().get_mapped()
            result['window'] = {'visible': window.get_visible(), 'mapped': window.get_mapped(),
                                'realized': window.get_realized(), 'width': window.get_width(),
                                'height': window.get_height(), 'layer': int(Layer.get_layer(window)),
                                'keyboardMode': int(Layer.get_keyboard_mode(window))}
            surface = window.get_surface()
            if surface is not None:
                monitor = display.get_monitor_at_surface(surface)
                clock = window.get_frame_clock()
                result['surface'] = {'width': surface.get_width(), 'height': surface.get_height(),
                                     'mapped': surface.get_mapped(),
                                     'output': monitor.get_connector() if monitor is not None else None,
                                     'frameCounter': clock.get_frame_counter() if clock is not None else None}
        return result

    def ready(_):
        if app.startup_error or app.closing:
            return

        def dispatch(value):
            check(type(value) is dict and set(value) == {'action'}, 'Invalid page command')
            action = value['action']
            check(action in ('status', 'home', 'back', 'refresh', 'quit'), 'Unknown page command')
            if action == 'home':
                presentation.home_button.emit('clicked')
            elif action == 'back':
                release_timing.clear()
                back_started=time.clock_gettime_ns(time.CLOCK_MONOTONIC)
                app.back_button.emit('clicked')
                back_completed=time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            elif action == 'refresh':
                app.refresh()
            elif action == 'quit':
                app.close_ui()
                app.quit()
            result=state()
            if action=='back':
                result['backStartedMonotonicNs']=back_started
                result['backCompletedMonotonicNs']=back_completed
                result['leaseReleaseStartedMonotonicNs']=release_timing.get('started',0)
                result['leaseReleaseCompletedMonotonicNs']=release_timing.get('completed',0)
            return result
        controls.append(CommandServer(runtime / 'page.sock', GLib, dispatch))
    app.connect('activate', ready)
    code = None
    retirement = {'retired': False, 'displayClosed': False, 'protocol': protocol, 'error': ''}
    try:
        code = app.run([])
    finally:
        # Mirror the production wrapper: stop GTK's application loop before
        # closing this connection. The quit RPC only accepts the request;
        # this private report is the actual retirement acknowledgement.
        try:
            app.close_ui()
            presentation.close()
            retirement.update(retired=bool(app.ui_retired and presentation.retired),
                              applicationExitCode=code,
                              error=app.startup_error or presentation.cleanup_error)
            monitors.clear()
            close_display(display)
            retirement['displayClosed'] = True
            handoff.close()
            owner.close()
        except Exception as error:
            retirement['error'] = retirement['error'] or str(error)[:300]
        # No GTK/layer/GDK calls follow verified display closure. The endpoint
        # is original private fixture data, not the live page owner or IPC.
        temporary = runtime / ('page-retired-' + secrets.token_hex(8) + '.tmp')
        raw = json.dumps(retirement).encode() + b'\n'
        check(len(raw) <= 1024, 'Oversized fixture retirement report')
        with temporary.open('xb') as stream:
            stream.write(raw)
        os.chmod(temporary, 0o600)
        temporary.replace(runtime / 'page-retired.json')
        for server in controls:
            server.close()
    check(code == 0 and retirement['retired'] and retirement['displayClosed'] and not retirement['error'],
          retirement['error'] or 'Page application failed or did not retire')


def rpc(path, action):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(4)
        client.connect(str(path))
        client.sendall(json.dumps({'action': action}).encode() + b'\n')
        with client.makefile('rb') as stream:
            raw = stream.readline(4097)
    check(len(raw) <= 4096 and raw.endswith(b'\n'), 'Invalid fixture reply')
    result = json.loads(raw)
    check(result.get('ok') is True, result)
    return result


def ppm(path):
    with Path(path).open('rb') as stream:
        raw = stream.read(MAX_IMAGE + 1)
    check(len(raw) <= MAX_IMAGE, 'Fixture screenshot exceeds its bound')
    # grim emits canonical binary PPM. Deliberately reject unsupported header
    # variants rather than inventing an image conversion fallback.
    parts = raw.split(b'\n', 3)
    check(len(parts) == 4 and parts[0] == b'P6' and parts[2] == b'255', 'Invalid grim PPM header')
    dimensions = parts[1].split(b' ')
    check(len(dimensions) == 2 and all(d.isdigit() for d in dimensions), 'Invalid grim PPM dimensions')
    width, height = map(int, dimensions)
    check(1 <= width <= 2048 and 1 <= height <= 2048 and len(parts[3]) == width * height * 3, 'Truncated/oversized PPM')
    return width, height, parts[3]


def matches(image, color, omit=None):
    width, height, pixels = image
    good = total = 0
    for y in range(height // 72, height, max(1, height // 36)):
        for x in range(width // 128, width, max(1, width // 64)):
            if omit and omit[0] <= x < omit[2] and omit[1] <= y < omit[3]:
                continue
            offset = (y * width + x) * 3
            good += all(abs(pixels[offset + channel] - color[channel]) <= 2 for channel in range(3))
            total += 1
    return good / max(total, 1)


def home_changes(image):
    width, height, pixels = image
    box = (width - MARGIN - SIZE, height - MARGIN - SIZE, width - MARGIN, height - MARGIN)
    changed = 0
    for y in range(box[1], box[3], 2):
        for x in range(box[0], box[2], 2):
            offset = (y * width + x) * 3
            changed += any(abs(pixels[offset + c] - GREEN[c]) > 2 for c in range(3))
    return box, changed


def home_corner_is_clear(image):
    """The page's empty bottom-right corner must contain no leftover Home.

    Compare the former button bounds to the adjacent blank page background,
    rather than duplicating the production CSS colors or icon implementation.
    """
    width, height, pixels = image
    left, top, right, bottom = width - MARGIN - SIZE, height - MARGIN - SIZE, width - MARGIN, height - MARGIN
    offset = ((top + SIZE // 2) * width + left - 12) * 3
    background = pixels[offset:offset + 3]
    good = total = 0
    for y in range(top, bottom, 2):
        for x in range(left, right, 2):
            offset = (y * width + x) * 3
            good += all(abs(pixels[offset + channel] - background[channel]) <= 3 for channel in range(3))
            total += 1
    return good / total > .995


def terminate(process):
    if process is not None and process.poll() is None:
        # Only the new process group this fixture created.
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=3)


def harness(args):
    check(sys.platform.startswith('linux') and os.geteuid() != 0, 'Run the isolated fixture as a nonroot Linux user')
    for binary in (args.labwc, args.grim, args.wlr_randr):
        check(Path(binary).is_absolute() and os.access(binary, os.X_OK), 'Fixture binary is unavailable: ' + binary)
    if args.gpu_probe:
        check(Path(args.gpu_probe).is_absolute() and os.access(args.gpu_probe, os.X_OK), 'Actual GPU fixture is unavailable')
    artifacts = Path(args.artifacts).absolute()
    check(artifacts.parent.resolve(strict=True) == artifacts.parent, 'Artifact parent must be canonical')
    artifacts.mkdir(mode=0o700)  # A pre-existing destination is refused.
    processes, logs, pointer = [], [], None
    report = {'scope': 'real isolated software Wayland layers; fake manager/colored video; no physical touch/CarPlay claim',
              'checks': [], 'screenshots': [], 'pageStates': []}
    with tempfile.TemporaryDirectory(prefix='hu-home-wl-', dir='/tmp') as capsule:
        capsule = private_directory(capsule)
        runtime, config = capsule / 'runtime', capsule / 'config'
        runtime.mkdir(mode=0o700); config.mkdir(mode=0o700)
        token = secrets.token_hex(16)
        (runtime / '.fixture-token').write_text(token); os.chmod(runtime / '.fixture-token', 0o600)
        (config / 'rc.xml').write_text('<?xml version="1.0"?><labwc_config><core><gap>0</gap></core></labwc_config>')
        for name in ('autostart', 'shutdown'):
            (config / name).write_text('#!/bin/sh\nexit 0\n')
        (config / 'environment').write_text('LABWC_UPDATE_ACTIVATION_ENV=0\n')
        (config / 'menu.xml').write_text('<?xml version="1.0"?><openbox_menu/>')
        env = dict(os.environ)
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DBUS_SESSION_BUS_ADDRESS', 'LABWC_PID',
                    'XAUTHORITY', 'XDG_SESSION_ID', 'WLR_DRM_DEVICES'):
            env.pop(key, None)
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config), XDG_CONFIG_DIRS=str(config),
                   WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='2', WLR_RENDERER='gles2' if args.gpu_probe else 'pixman',
                   LIBSEAT_BACKEND='noop', LABWC_UPDATE_ACTIVATION_ENV='0', GDK_BACKEND='wayland',
                   GSK_RENDERER='cairo', GTK_A11Y='none', HEADUNIT_HOME_FIXTURE_TOKEN=token)
        if args.pointer_input:
            env['HEADUNIT_HOME_INPUT_DIAGNOSTICS'] = '1'
            report['inputBoundary'] = 'actual isolated virtual-pointer v2; no wl_touch/physical touch claim'
        hold, entered, release = threading.Event(), threading.Event(), threading.Event()
        calls = []

        class Manager(socketserver.StreamRequestHandler):
            def handle(self):
                value = json.loads(self.rfile.readline(1024)); calls.append(value)
                check(value == {'action': 'status'}, 'Page/Home generated an unexpected manager mutation')
                if hold.is_set():
                    entered.set()
                    check(release.wait(15), 'Fixture manager hold was not released')
                fixture_phone = 'AA:BB:CC:DD:EE:01'
                phones = [{'address': fixture_phone, 'name': 'Fixture iPhone', 'active': True,
                           'selected': True, 'connected': True}] if args.phone_options else []
                self.wfile.write(json.dumps({'ok': True, 'phones': phones,
                                            'active': fixture_phone if phones else None,
                                            'active_mode': 'ultra' if phones else None, 'generation': 1,
                                            'ultra_available': False, 'pairing': {'supported': False}, 'error': '',
                                            # Explicit fixture admission, not inferred from PID or synthetic GPU pixels.
                                            'carplay': {'state': 'ready', 'returnReady': True},
                                            'media': {'available': False, 'controls': []},
                                            'camera': {'configured': False, 'requested': False, 'phase': 'unavailable', 'error': ''}}).encode() + b'\n')
        manager = socketserver.UnixStreamServer(str(runtime / 'manager.sock'), Manager)
        os.chmod(runtime / 'manager.sock', 0o600)
        manager_thread = threading.Thread(target=manager.serve_forever, daemon=True); manager_thread.start()

        def launch(command, name, *, trace=False):
            log = (artifacts / (name + '.log')).open('wb'); logs.append(log)
            child_env = dict(env)
            if trace:
                # Only this fixture page's isolated connection is logged.
                child_env['WAYLAND_DEBUG'] = '1'
            process = subprocess.Popen(command, env=child_env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            processes.append(process)
            return process

        def wait(predicate, label, timeout=8):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                check(all(p.poll() is None for p in processes), 'An owned fixture process exited during ' + label)
                if predicate():
                    return
                time.sleep(0.05)
            raise TimeoutError(label)

        def shot(output, label):
            destination = artifacts / (label + '-' + output + '.ppm')
            try:
                subprocess.run([args.grim, '-o', output, '-t', 'ppm', str(destination)], env=env, check=True, timeout=4,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            except subprocess.CalledProcessError as error:
                diagnostic = (error.stderr or b'').decode(errors='replace')[:512]
                raise RuntimeError('Isolated screenshot failed on ' + output + ': ' + diagnostic) from error
            return ppm(destination)

        def page_command(action, label):
            if action == 'home':
                wait(lambda: (rpc(runtime / 'page.sock', 'status')['foreground'] or
                              rpc(runtime / 'page.sock', 'status')['homeVisible']), 'fresh Home action admission')
            elif action == 'back':
                wait(lambda: not rpc(runtime / 'page.sock', 'status')['pending'], 'completed operation before Back')
            if action == 'home' and pointer is not None:
                result = rpc(runtime / 'page.sock', 'status')
                if not result['foreground']:
                    pointer.click_home()
                    wait(lambda: rpc(runtime / 'page.sock', 'status')['foreground'], 'real compositor pointer Home activation')
                    result = rpc(runtime / 'page.sock', 'status')
            else:
                result = rpc(runtime / 'page.sock', action)
            report['pageStates'].append({'label': label, 'state': result})
            return result

        def retain_pngs(label):
            for output in (MAIN, CLUSTER):
                destination = artifacts / (label + '-' + output + '.png')
                subprocess.run([args.grim, '-o', output, '-t', 'png', str(destination)], env=env,
                               check=True, timeout=4, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                report['screenshots'].append(destination.name)

        def settle(label, predicate, *, foreground=False):
            until = time.monotonic() + 5
            while time.monotonic() < until:
                image = shot(MAIN, label)
                if predicate(image) and (not foreground or home_corner_is_clear(image)):
                    cluster = shot(CLUSTER, label)
                    check(hashlib.sha256(cluster[2]).hexdigest() == cluster_hash, 'Native page changed the cluster output')
                    report['checks'].append(label)
                    retain_pngs(label)
                    return image
                time.sleep(0.05)
            raise AssertionError('Compositor pixel oracle failed: ' + label)

        try:
            compositor = launch([args.labwc, '--config-dir', str(config), '--config', str(config / 'rc.xml')], 'labwc')
            def socket_ready():
                endpoints = [p for p in runtime.glob('wayland-*') if stat.S_ISSOCK(p.lstat().st_mode)]
                if len(endpoints) == 1:
                    env['WAYLAND_DISPLAY'] = endpoints[0].name
                    return True
                return False
            wait(socket_ready, 'owned headless compositor startup')
            subprocess.run([args.wlr_randr, '--output', MAIN, '--custom-mode', '1280x720@60Hz', '--pos', '0,0',
                            '--output', CLUSTER, '--custom-mode', '1280x720@60Hz', '--pos', '1280,0'],
                           env=env, check=True, timeout=4, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if args.pointer_input:
                pointer = FixturePointer(runtime, env['WAYLAND_DISPLAY'])
            markers = launch([sys.executable, str(Path(__file__).resolve()), '--client', 'markers'], 'markers')
            wait(lambda: (runtime / 'markers.sock').exists(), 'marker client startup')
            check(rpc(runtime / 'markers.sock', 'status')['outputs'] == [MAIN, CLUSTER], 'Wrong real outputs')
            wait(lambda: matches(shot(MAIN, 'baseline'), GREEN) > .99, 'main marker presentation')
            cluster = shot(CLUSTER, 'baseline')
            check(matches(cluster, BLUE) > .99, 'Cluster marker was not presented')
            cluster_hash = hashlib.sha256(cluster[2]).hexdigest()
            if args.gpu_probe:
                # Optional actual-renderer diagnosis. The normal default above
                # remains the already-qualified GTK compositor fixture. Remove
                # its TOP main marker before creating production xdg/GPU output.
                rpc(runtime / 'markers.sock', 'hide-main')
                report['scope'] = 'actual isolated production GpuDisplay bootstrap; synthetic clock/scene and fake manager; no phone/physical output'
                report['gpuCase'] = args.gpu_case
                report['gpuRevealUnderlay'] = args.gpu_reveal
                if args.gpu_case != 'without-page':
                    page = launch([sys.executable, str(Path(__file__).resolve()), '--client', 'page'], 'page', trace=True)
                    wait(lambda: (runtime / 'page.sock').exists(), 'actual-renderer native page startup')
                    wait(lambda: rpc(runtime / 'page.sock', 'status')['homeVisible'], 'actual-renderer Home admission')
                    page_command('status', 'before-gpu')
                    if args.gpu_case in ('resident-overlay', 'resident-foreground-startup'):
                        page_command('home', 'before-gpu-overlay')
                        wait(lambda: page_command('status', 'before-gpu-wait')['foreground'], 'native Home foreground before GPU')
                    retain_pngs('before-gpu')
                cooperative = args.gpu_case in ('resident-handoff', 'resident-foreground-startup')
                gpu_command = [args.gpu_probe, 'org.headunit.ultra.main', '1' if args.gpu_reveal else '0',
                               str(args.gpu_queue), '1' if args.gpu_defer else '0']
                if cooperative:
                    gpu_command.append('handoff')
                gpu = launch(gpu_command, 'gpu', trace=True)
                status_path = runtime / 'gpu-status.json'

                def gpu_status():
                    value = gpu_status_snapshot(status_path)
                    if value is None:
                        return None
                    report['gpuStatus'] = value
                    check(value['phase'] != 'fault', 'Actual GpuDisplay failed: ' + str(value))
                    return value

                def qualified(minimum):
                    until = time.monotonic() + 8
                    while time.monotonic() < until:
                        value = gpu_status()
                        if value and value['phase'] == 'rendering' and not value['surfaceWithdrawn'] and value['scenePresentations'] >= minimum:
                            return value
                        if gpu.poll() is not None:
                            report['gpuExitCode'] = gpu.returncode
                            raise AssertionError('Actual GPU fixture exited before qualified scenes: ' + str(gpu.returncode))
                        time.sleep(0.02)
                    raise AssertionError('Actual GPU fixture did not qualify bootstrap/scenes')

                def correct_scene(label):
                    image = shot(MAIN, label)
                    cluster = shot(CLUSTER, label)
                    check(hashlib.sha256(cluster[2]).hexdigest() == cluster_hash, 'Actual GPU changed cluster output')
                    # The original wide scene is fitted, never stretched. Read
                    # actual central-band pixels rather than treating a receipt
                    # or a log as proof that the scene is visible.
                    width, height, pixels = image
                    fit = min(width / 1920, height / 720)
                    top = (height - int(720 * fit)) // 2
                    center = pixels[top * width * 3:(height - top) * width * 3]
                    check(matches((width, height - 2 * top, center), GREEN) > .99,
                          'Actual GPU scene is not visible despite qualified receipts')
                    report['checks'].append(label)
                    retain_pngs(label)

                def hold_home(label, *, startup=False, duration=10.5):
                    wait(lambda: page_command('status', label + '-ownership')['foreground'], 'exclusive Home ownership')
                    until = time.monotonic() + 3
                    while time.monotonic() < until:
                        value = gpu_status()
                        if value and value['surfaceWithdrawn']:
                            break
                        time.sleep(.02)
                    else:
                        raise AssertionError('Home mapped without actual GPU withdrawal')
                    baseline = (value['submissions'], value['presentations'], value['sceneCompositions'])
                    if startup:
                        check(value['phase'] == 'bootstrap' and baseline == (0, 0, 0),
                              'Hidden startup submitted or invented a frame')
                    until = time.monotonic() + duration
                    while time.monotonic() < until:
                        value = gpu_status()
                        check(value is not None, 'GPU fixture status unavailable while Home owns the surface')
                        check(value['surfaceWithdrawn'] and baseline == (value['submissions'], value['presentations'], value['sceneCompositions']),
                              'Withdrawn main renderer still submitted/presented a frame')
                        check(gpu.poll() is None, 'GPU renderer ended while Home owned the surface')
                        time.sleep(.05)
                    image = shot(MAIN, label)
                    check(matches(image, GREEN) < .4, 'Home did not replace actual GPU pixels')
                    check(home_corner_is_clear(image), 'Home icon remained over its foreground page')
                    cluster = shot(CLUSTER, label)
                    check(hashlib.sha256(cluster[2]).hexdigest() == cluster_hash, 'Home ownership changed cluster output')
                    report['checks'].append(label)
                    retain_pngs(label)
                    return value

                if args.gpu_case == 'resident-foreground-startup':
                    held = hold_home('startup-deferred-home-10s', startup=True)
                    report['startupDeferred'] = held
                    check(not page_command('back', 'startup-home-back')['foreground'], 'Back did not lower Home before return')
                value = qualified(3)
                correct_scene('actual-gpu-bootstrap-and-three-scenes')
                if args.gpu_case == 'resident-handoff':
                    page_command('home', 'gpu-home-request')
                    held = hold_home('actual-gpu-withdrawn-home-10s')
                    report['heldGpuStatus'] = held
                    back=page_command('back', 'gpu-home-back')
                    check(not back['foreground'], 'Back did not lower Home before return')
                    value = qualified(held['scenePresentations'] + 3)
                    check(value['surfaceRestorations'] >= 1 and value['lastPts'] > held['lastPts'],
                          'Return did not obtain a genuinely newer presented PTS')
                    started=back['backStartedMonotonicNs'];completed=back['backCompletedMonotonicNs']
                    release_start=back['leaseReleaseStartedMonotonicNs']
                    release_end=back['leaseReleaseCompletedMonotonicNs']
                    restored=value['restoredObservedNs'];scene=value['returnSceneObservedNs']
                    check(started<=release_start<=release_end<=completed and
                          release_start<=restored<=scene and scene>0,
                          'Return timing did not follow Back release and a genuine new scene')
                    report['returnTimingNs']={'backHandler':completed-started,
                                             'leaseReleaseOperation':release_end-release_start,
                                             'releaseStartToRestoredOwner':restored-release_start,
                                             'releaseCompleteToRestoredOwner':restored-release_end,
                                             'restoringOwnerTurn':value['returnTurnNs'],
                                             'restoredOwnerToQualifiedScene':scene-restored,
                                             'backToQualifiedScene':scene-started}
                    correct_scene('actual-gpu-restored-after-home')
                    for cycle in range(2):
                        label = 'gpu-repeat-' + str(cycle + 1)
                        page_command('home', label + '-request')
                        held = hold_home(label + '-withdrawn', duration=.2)
                        check(not page_command('back', label + '-back')['foreground'], 'Repeated Back did not lower Home')
                        value = qualified(held['scenePresentations'] + 3)
                        check(value['lastPts'] > held['lastPts'], 'Repeated return reused old presentation PTS')
                        correct_scene(label + '-restored')
                # Publish the stop request complete and private in one rename:
                # the probe polls and rejects a partly written or 0644 file.
                stop_fd = os.open(runtime / 'gpu-stop.tmp', os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(stop_fd, 'w') as stop: stop.write('stop\n')
                os.rename(runtime / 'gpu-stop.tmp', runtime / 'gpu-stop')
                report['gpuExitCode'] = gpu.wait(timeout=4)
                check(report['gpuExitCode'] == 0, 'Actual GPU fixture shutdown failed')
                report['passed'] = True
                return
            page = launch([sys.executable, str(Path(__file__).resolve()), '--client', 'page'], 'page', trace=True)
            wait(lambda: (runtime / 'page.sock').exists(), 'page client startup')
            wait(lambda: rpc(runtime / 'page.sock', 'status')['homeVisible'], 'initial Home admission')
            page_command('status', 'initial-home')
            settle('home-over-main', lambda image: matches(image, GREEN, home_changes(image)[0]) > .99 and home_changes(image)[1] > 200)
            check(page_command('home', 'first-open')['foreground'], 'Home button did not raise page')
            settle('page-open', lambda image: matches(image, GREEN) < .4, foreground=True)
            page_command('status', 'first-open-presented')
            if args.phone_options:
                check(pointer is not None, 'Phone options require actual private pointer input')
                point = rpc(runtime / 'page.sock', 'status')['phoneOptionsPoint']
                pointer.click_at(*point)
                wait(lambda: rpc(runtime / 'page.sock', 'status')['phoneOptionsVisible'], 'phone options actual pointer opening')
                settle('phone-options-open', lambda image: matches(image, GREEN) < .4, foreground=True)
                pointer.click_at(40, 400)
                wait(lambda: not rpc(runtime / 'page.sock', 'status')['phoneOptionsVisible'], 'phone options outside-click dismissal')
                pointer.click_at(*point)
                wait(lambda: rpc(runtime / 'page.sock', 'status')['phoneOptionsVisible'], 'phone options reopening')
            check(not page_command('back', 'first-back')['foreground'], 'Back did not lower page')
            if args.phone_options:
                wait(lambda: not rpc(runtime / 'page.sock', 'status')['phoneOptionsVisible'], 'phone options retirement on Back')
                report['phoneOptions'] = 'real private pointer open/dismiss/reopen; Back retires popup; no manager mutation'
            settle('back-to-main', lambda image: matches(image, GREEN, home_changes(image)[0]) > .99 and home_changes(image)[1] > 200)
            # Repeated real GTK Home/Back callbacks must each change actual
            # compositor pixels. Logical layer getters alone cannot pass.
            for turn in range(3):
                label = 'repeat-' + str(turn + 1)
                check(page_command('home', label + '-open')['foreground'], 'Repeated Home did not raise page')
                check(page_command('home', label + '-open-again')['foreground'], 'Repeated Home was not idempotent')
                settle(label + '-open', lambda image: matches(image, GREEN) < .4, foreground=True)
                check(not page_command('back', label + '-back')['foreground'], 'Repeated Back did not lower page')
                check(not page_command('back', label + '-back-again')['foreground'], 'Repeated Back was not idempotent')
                settle(label + '-back', lambda image: matches(image, GREEN, home_changes(image)[0]) > .99 and home_changes(image)[1] > 200)
            rpc(runtime / 'markers.sock', 'hide-main')
            settle('native-underlay', lambda image: matches(image, AMBER) < .4 and matches(image, GREEN) < .4)
            rpc(runtime / 'markers.sock', 'show-main')
            settle('main-restored', lambda image: matches(image, GREEN, home_changes(image)[0]) > .99)
            hold.set()
            rpc(runtime / 'page.sock', 'refresh')
            check(entered.wait(3), 'No actual page IPC worker was held')
            result = page_command('quit', 'quit-request-accepted')
            check(result['retired'] and not result['displayClosed'] and not result['error'],
                  'Quit callback must retire UI before requesting application exit')
            retired_path = runtime / 'page-retired.json'
            wait(lambda: retired_path.exists(), 'post-application display retirement')
            info = retired_path.lstat()
            check(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_uid == os.getuid() and
                  stat.S_IMODE(info.st_mode) == 0o600 and info.st_size <= 1024, 'Unsafe retirement report')
            retirement = json.loads(retired_path.read_bytes())
            check(set(retirement) == {'retired', 'displayClosed', 'protocol', 'error', 'applicationExitCode'} and
                  retirement['retired'] is True and retirement['displayClosed'] is True and
                  retirement['protocol'] >= 4 and retirement['applicationExitCode'] == 0 and not retirement['error'],
                  'Post-application display retirement failed: ' + str(retirement))
            report['retirement'] = retirement
            settle('page-retired-main', lambda image: matches(image, GREEN) > .99 and home_changes(image)[1] == 0)
            rpc(runtime / 'markers.sock', 'hide-main')
            settle('no-resident-underlay', lambda image: matches(image, AMBER) > .99)
            check(page.poll() is None, 'Page worker was not held during compositor withdrawal checks')
            report['heldWorkerAliveDuringWithdrawal'] = True
            check(all(value == {'action': 'status'} for value in calls), 'Home/Back changed manager state')
            report['managerReadOnlyCalls'] = len(calls)
            release.set()
            report['pageExitCode'] = page.wait(timeout=4)
            check(report['pageExitCode'] == 0, 'Page process failed after releasing fixture IPC: ' + str(report['pageExitCode']))
            rpc(runtime / 'markers.sock', 'quit')
            report['markerExitCode'] = markers.wait(timeout=4)
            check(report['markerExitCode'] == 0, 'Marker process failed during shutdown: ' + str(report['markerExitCode']))
            report['passed'] = True
        except Exception as error:
            report['failure'] = str(error)[:1024]
            report['passed'] = False
            # Preserve actual surfaces and client state before retiring this
            # owned test compositor. Evidence is diagnostic, never a pass.
            diagnostics = []
            try:
                page_command('status', 'failure')
            except Exception as diagnostic_error:
                diagnostics.append('Page status: ' + str(diagnostic_error)[:300])
            try:
                retain_pngs('failure')
            except Exception as diagnostic_error:
                diagnostics.append('Failure screenshot: ' + str(diagnostic_error)[:300])
            report['diagnosticErrors'] = diagnostics
            raise
        finally:
            release.set()
            if pointer is not None:
                try:
                    pointer.close()
                except Exception as error:
                    report['inputCleanupError'] = str(error)[:128]
                    report['passed'] = False
            for process in reversed(processes):
                terminate(process)
            manager.shutdown(); manager.server_close(); manager_thread.join(timeout=2)
            for log in logs:
                log.close()
            input_failure = False
            if args.pointer_input and report.get('passed') is True:
                trace = (artifacts / 'page.log').read_text()
                events = sum(line.startswith('Native Home input event=') and line.endswith(' source=mouse')
                             for line in trace.splitlines())
                clicks = trace.count('Native Home input clicked')
                report['inputDiagnostics'] = {'mouseEvents': events, 'clicked': clicks,
                                              'pythonTraceback': 'Traceback (most recent call last)' in trace}
                input_failure = events < 2 or clicks < 1 or report['inputDiagnostics']['pythonTraceback']
                if input_failure:
                    report['passed'] = False
                    report['failure'] = 'Actual pointer input or Home event observation was not qualified'
            (artifacts / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            check(not input_failure, 'Actual pointer input or Home event observation was not qualified')
    check(report.get('passed') is True, 'Isolated native Home check did not complete')
    print('Real isolated labwc layer/window checks passed; artifacts:', artifacts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--labwc', default='/usr/bin/labwc')
    parser.add_argument('--grim', default='/usr/bin/grim')
    parser.add_argument('--wlr-randr', default='/usr/bin/wlr-randr')
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--client', choices=('markers', 'page'), help=argparse.SUPPRESS)
    parser.add_argument('--status-self-test', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--gpu-probe', help='Explicit actual GpuDisplay bootstrap diagnostic binary; uses private headless GLES2')
    parser.add_argument('--gpu-case', choices=('without-page', 'resident-bottom', 'resident-overlay', 'resident-handoff', 'resident-foreground-startup'), default='resident-bottom')
    parser.add_argument('--gpu-reveal', action='store_true', help='Explicit transparent-bootstrap comparison; default matches opaque main receiver')
    parser.add_argument('--gpu-queue', type=int, choices=(1, 2), default=2)
    parser.add_argument('--gpu-defer', action='store_true')
    parser.add_argument('--pointer-input', action='store_true', help='Activate Home through real private compositor pointer events; not a touchscreen test')
    parser.add_argument('--phone-options', action='store_true', help='Qualify the synthetic phone options popup with private pointer input')
    args = parser.parse_args()
    if args.phone_options and (not args.pointer_input or args.gpu_probe or args.client or args.status_self_test):
        parser.error('--phone-options requires the private GTK --pointer-input fixture')
    os.umask(0o077)
    if args.status_self_test:
        gpu_status_self_test()
    elif args.client == 'markers':
        run_markers()
    elif args.client == 'page':
        run_page()
    else:
        if args.artifacts is None:
            args.artifacts = Path('/tmp') / ('headunit-home-wayland-' + secrets.token_hex(12))
        harness(args)


if __name__ == '__main__':
    main()
