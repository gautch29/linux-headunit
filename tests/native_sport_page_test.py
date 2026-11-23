#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real Gtk factory/render/actions on an isolated display and manager socket.

Sport acquisitions and manager are explicit fixtures, never phone/car evidence.
The existing resident presentation fixture separately covers surface ownership.
"""
from contextlib import redirect_stderr
import io
import json
import os
from pathlib import Path
import shutil
import struct
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
# Exercise the actual resident runtime factory, including its layer library
# preload, before importing GTK. The test toplevel uses the private compositor.
from sport_telemetry import SportEngine, Observation, NS
from sport_source import host_now_ns


def load_gui():
    global library, GLib, layer, Presentation, PhonePage, Gdk, display, Gtk, Gio, main, VIEWS
    from phone_page_boot import _load_runtime
    library, GLib, layer, Presentation, PhonePage, Gdk, display = _load_runtime()
    from phone_page import Gtk, Gio, main
    from phone_page_sport import VIEWS


class ModelOwner:
    def __init__(self, origin='measured'):
        self.engine = SportEngine()
        self.mode = 'available'
        self.origin = origin
        self.lock = threading.Lock()
        now = host_now_ns()
        for index in range(6):
            self.acquire(now - (5 - index) * 200_000_000)

    def acquire(self, now):
        self.engine.accept(3, {
            'speed_kmh': Observation(36, now, origin=self.origin),
            'acceleration': Observation((0, -1), now, origin=self.origin),
            'gps_position': Observation((48, 2, 1), now, origin=self.origin)}, now)

    def snapshot(self):
        with self.lock:
            now = host_now_ns()
            if self.mode in ('available', 'simulation'):
                self.acquire(now)
            if self.mode == 'retired':
                self.engine.accept(3, {key: Observation(None, obs.acquired_ns, obs.maximum_age_ns,
                    obs.origin, 'disconnected') for key, obs in self.engine.inputs.items()}, now)
            result = self.engine.model(now)
            result.update(source='simulation' if self.mode == 'simulation' else 'observations', error='', rejectedPackets=0)
            return result

    def mutate(self, action):
        with self.lock:
            now = host_now_ns()
            if action == 'trip_reset':
                self.engine.reset_trip(now)
            else:
                self.engine.reset_session(now)
            result = self.engine.model(now)
            # Deliberately different reply data cannot replace a dated status.
            result['trip']['journeyId'] = 99999
            return result


def assert_until(predicate, message, deadline=3):
    stop = time.monotonic() + deadline
    context = GLib.MainContext.default()
    while not predicate():
        assert time.monotonic() < stop, message
        while context.pending():
            context.iteration(False)
        time.sleep(.005)


def capture(app, destination, name, *, settled=False):
    if not settled:
        assert_until(lambda: not app.pages.get_transition_running(), 'Stack transition did not finish')
        context = GLib.MainContext.default()
        app.health_tick()
        app.window.queue_draw()
        until = time.monotonic() + .06
        while time.monotonic() < until:
            while context.pending():
                context.iteration(False)
            time.sleep(.005)
    assert app.window.get_width() == 1280 and app.window.get_height() == 720, 'Page enlarged beyond the admitted viewport'
    for button in (app.sport.parent_button, *app.sport.tabs.values(), *app.sport.buttons.values()):
        if not button.get_visible():
            continue
        ok, bounds = button.compute_bounds(app.window)
        assert ok and bounds.origin.x >= 0 and bounds.origin.y >= 0
        assert bounds.origin.x + bounds.size.width <= 1280.5 and bounds.origin.y + bounds.size.height <= 720.5, button.get_name()
    assert os.environ.get('HEADUNIT_TEST_OUTPUT_CAPTURE') == '1'
    assert os.environ.get('HEADUNIT_THEME_GPU_PROBE_WAYLAND') == '1'
    assert os.environ.get('WAYLAND_DISPLAY') == 'native-test'
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    assert runtime.name.startswith('headunit-wayland-') and runtime.stat().st_uid == os.geteuid()
    raw = destination / ('raw-' + name)
    raw.mkdir(exist_ok=False)
    # The official Weston capture client reads the complete virtual output.
    # Never export a partial cached WidgetPaintable as a reviewed screenshot.
    result = subprocess.run(['weston-screenshooter'], cwd=raw,
                            env=dict(os.environ, XDG_PICTURES_DIR=str(raw)),
                            capture_output=True, timeout=5)
    (raw / 'screenshooter.log').write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, 'Owned compositor capture failed'
    images = list(raw.glob('*.png'))
    assert len(images) == 1
    data = images[0].read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n' and struct.unpack('>II', data[16:24]) == (1280, 720)
    shutil.copyfile(images[0], destination / (name + '.png'))


def run():
    destination_raw = os.environ.get('HEADUNIT_SPORT_ARTIFACTS')
    destination = Path(destination_raw) if destination_raw else Path(tempfile.mkdtemp(prefix='headunit-sport-pixels-'))
    destination.mkdir(parents=True, exist_ok=True)
    owner = ModelOwner()
    calls = []
    status_release, action_release, action_entered, status_entered = (threading.Event() for _ in range(4))
    status_release.set()
    action_release.set()
    lost = False

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            value = json.loads(self.rfile.readline(4096))
            calls.append(value)
            if value == {'action': 'status'}:
                status_entered.set()
                assert status_release.wait(3), 'Fixture held status was never released'
                reply = {'ok': False, 'error': 'Fixture manager unavailable'} if lost else {
                    'ok': True, 'phones': [], 'active': None, 'active_mode': None, 'generation': 1,
                    'carplay': {'state': 'disconnected', 'returnReady': False},
                    'camera': {'configured': False, 'requested': False, 'phase': 'unavailable', 'error': ''},
                    'media': {'available': False, 'controls': []}, 'cluster_pages': ['map', 'media', 'vehicle', 'trip'],
                    'cluster_page': 'trip', 'sport': owner.snapshot(), 'error': ''}
            else:
                assert value in ({'action': 'trip_reset'}, {'action': 'sport_session'}), value
                action_entered.set()
                assert action_release.wait(3), 'Fixture held action was never released'
                reply = {'ok': True, 'sport': owner.mutate(value['action'])}
            self.wfile.write(json.dumps(reply).encode() + b'\n')

    with tempfile.TemporaryDirectory(prefix='headunit-sport-manager-') as folder:
        path = Path(folder) / 'manager.sock'
        server = socketserver.UnixStreamServer(str(path), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        app = PhonePage(path, initial_view='sport')
        app.set_flags(Gio.ApplicationFlags.NON_UNIQUE)
        failures = []

        def checks():
            nonlocal lost
            try:
                assert_until(lambda: app.status_current() and not app.pending and hasattr(app, 'sport'), 'No fresh real socket reply')
                # Stop only automatic fixture polls; use actual manual requests
                # to make held reply/action ordering deterministic.
                for field in ('poll_source', 'health_source'):
                    GLib.source_remove(getattr(app, field))
                    setattr(app, field, 0)
                assert app.pages.get_visible_child_name() == 'sport'
                assert len(app.get_windows()) == 1
                assert set(app.view_buttons) == set(VIEWS) - {'sport'}
                assert app.view_buttons['cluster'].get_active()
                assert app.sport.buttons['trip_reset'].get_sensitive()
                app.refresh()
                assert_until(lambda: not app.pending, 'Fresh available trip reply missing')
                capture(app, destination, 'available-trip')
                app.sport.tabs['sport'].emit('clicked')
                app.refresh()
                assert_until(lambda: not app.pending, 'Fresh available sport reply missing')
                capture(app, destination, 'available-sport')
                assert app.sport.row_widgets['sport-input-speed'][2].get_text() == '36.0 km/h'
                app.sport.parent_button.emit('clicked')
                assert app.pages.get_visible_child_name() == 'cluster'
                capture(app, destination, 'cluster-entry')
                app.sport_open.emit('clicked')
                assert app.pages.get_visible_child_name() == 'sport'
                app.view_buttons['cluster'].emit('clicked')
                assert app.pages.get_visible_child_name() == 'cluster'
                app.sport_open.emit('clicked')
                assert app.pages.get_visible_child_name() == 'sport'
                app.sport.select('trip')
                # Actual executor/socket mutation is held. Additional taps and
                # unsupported GPS actions never enter an operation queue.
                old_id = app.status['sport']['trip']['journeyId']
                action_release.clear()
                action_entered.clear()
                app.sport.buttons['trip_reset'].emit('clicked')
                assert_until(action_entered.is_set, 'Action did not reach the real socket worker')
                assert app.pending_action == 'trip_reset'
                assert 'pending' in app.notice.get_text()
                assert not app.sport.buttons['sport_session'].get_sensitive()
                app.action({'action': 'sport_session'})
                app.action({'action': 'sport_line', 'line': [[48, 2], [48, 3]]})
                assert len([c for c in calls if c['action'] != 'status']) == 1
                status_release.clear()
                status_entered.clear()
                action_release.set()
                assert_until(status_entered.is_set, 'Action did not request following status')
                assert app.pending_action == 'status'
                assert app.status['sport']['trip']['journeyId'] == old_id
                assert 'accepted' in app.notice.get_text() and 'Done' not in app.notice.get_text()
                status_release.set()
                assert_until(lambda: not app.pending, 'Fresh post-reset status missing')
                assert app.status['sport']['trip']['journeyId'] != old_id
                assert app.status['sport']['trip']['journeyId'] != 99999
                assert 'data refreshed' in app.notice.get_text()
                session = app.status['sport']['sessionId']
                journey = app.status['sport']['trip']['journeyId']
                app.action({'action': 'sport_session'})
                assert_until(lambda: not app.pending, 'New sport session status missing')
                assert app.status['sport']['sessionId'] != session
                assert app.status['sport']['trip']['journeyId'] == journey
                app.sport.select('history')
                capture(app, destination, 'historical-results')
                owner.mode = 'simulation'
                owner.origin = 'simulation'
                app.refresh()
                assert_until(lambda: not app.pending, 'Simulation status missing')
                app.sport.select('sport')
                assert 'SIMULATION' in app.sport.summary.get_text()
                capture(app, destination, 'simulation-sport')
                owner.mode = 'missing'
                # Real existing acquisition expiry; no fixture clock is changed.
                until = time.monotonic() + .51
                while time.monotonic() < until:
                    while GLib.MainContext.default().pending():
                        GLib.MainContext.default().iteration(False)
                    time.sleep(.005)
                app.refresh()
                assert_until(lambda: not app.pending, 'Missing source status missing')
                assert app.sport.row_widgets['sport-input-speed'][2].get_text() == 'Unavailable'
                capture(app, destination, 'missing-sport')
                owner.mode = 'retired'
                app.refresh()
                assert_until(lambda: not app.pending, 'Retired source status missing')
                assert app.status['sport']['inputs']['speed_kmh']['availability'] == 'disconnected'
                assert not app.sport.buttons['trip_reset'].get_sensitive()
                capture(app, destination, 'retired-sport')
                app.sport.select('trip')
                capture(app, destination, 'retired-trip')
                # Failure disables both controls and clears retained history.
                lost = True
                app.refresh()
                assert_until(lambda: not app.pending, 'Failure reply missing')
                assert not app.sport.buttons['trip_reset'].get_sensitive()
                assert app.sport.model['history'][0]['value'] == 'Unavailable'
                capture(app, destination, 'manager-unavailable')
                count = len(calls)
                app.action({'action': 'trip_reset'})
                assert len(calls) == count
                lost = False
                owner.mode = 'available'
                app.refresh()
                assert_until(lambda: not app.pending, 'Fresh recovery reply missing')
                # Conservative status-age expiry applies to this leaf too.
                app.status_requested_at = time.monotonic() - 6
                app.health_tick()
                assert not app.status_available
                assert app.sport.model['trip'][0]['value'] == 'Unavailable'
                # CLI uses this same application factory; invalid view fails
                # at admission, before constructing another owner.
                with patch('phone_page.PhonePage') as factory:
                    factory.return_value.run.return_value = 0
                    assert main(['--socket', str(path), '--view', 'sport']) == 0
                    factory.assert_called_once_with(path, initial_view='sport')
                with patch('phone_page.PhonePage') as factory:
                    factory.return_value.run.return_value = 0
                    assert main(['--socket', str(path)]) == 0
                    factory.assert_called_once_with(path, initial_view='home')
                with patch('phone_page.PhonePage') as factory, redirect_stderr(io.StringIO()):
                    try:
                        main(['--socket', str(path), '--view', 'unsupported'])
                    except SystemExit as error:
                        assert error.code == 2
                    else:
                        raise AssertionError('Unknown CLI view admitted')
                    factory.assert_not_called()
                try:
                    PhonePage(path, initial_view='unsupported')
                except ValueError:
                    pass
                else:
                    raise AssertionError('Unknown view admitted')
                # Actual held socket worker completes AFTER real surfaces retire.
                status_release.clear()
                status_entered.clear()
                app.refresh()
                assert_until(status_entered.is_set, 'Late reply fixture did not reach the socket')
                app.close_ui()
                assert app.ui_retired and app.closing and not app.pending
                assert not app.get_windows()
                status_release.set()
                assert_until(lambda: all(not thread.is_alive() for thread in app.pool._threads),
                             'Actual retired socket executor did not finish')
                assert app.received('status', {'ok': True, 'sport': owner.snapshot()}, observed_at=time.monotonic()) is False
                assert not app.status_available
                assert app.ui_retired
                (destination / 'report.json').write_text(json.dumps({
                    'scope': 'Real Linux GTK/Wayland widgets and private socket; synthetic car/manager/layer boundaries',
                    'physicalDisplay': False, 'phone': False, 'car': False,
                    'factory': 'phone_page_boot._load_runtime -> phone_page.PhonePage',
                    'windowRenderer': os.environ['GSK_RENDERER'], 'artifactRenderer': 'Owned Weston output capture',
                    'viewport': [1280, 720], 'sidebarIds': list(app.view_buttons),
                    'screenshots': sorted(p.name for p in destination.glob('*.png')),
                    'mutations': [c['action'] for c in calls if c['action'] != 'status'],
                    'uiRetired': app.ui_retired, 'executorThreadsRetired': True, 'tests': 'PASS',
                }, indent=2) + '\n')
            except BaseException as error:
                failures.append(repr(error))
                raise
            finally:
                action_release.set()
                status_release.set()
                app.close_ui()
                app.quit()
            return False

        def activated(_):
            GLib.idle_add(checks)

        app.connect('activate', activated)
        try:
            app.run([])
            assert not failures, failures
        finally:
            action_release.set()
            status_release.set()
            app.close_ui()
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)
            assert not worker.is_alive()
    print('Native sport page: PASS; artifacts:', destination)


REVIEW_STATES = ('available-trip', 'available-sport', 'available-trip-bottom', 'available-sport-bottom',
                 'cluster-entry', 'historical-results', 'simulation-sport', 'missing-sport',
                 'retired-sport', 'retired-trip', 'manager-unavailable')


def render_one(name):
    """Capture one fresh owner from the complete owned virtual output."""
    assert name in REVIEW_STATES
    destination = Path(os.environ['HEADUNIT_SPORT_ARTIFACTS'])
    destination.mkdir(parents=True, exist_ok=True)
    owner = ModelOwner('simulation' if name.startswith(('simulation', 'missing', 'retired')) else 'measured')
    if name.startswith('simulation'):
        owner.mode = 'simulation'
    elif name.startswith('missing'):
        owner.mode = 'missing'
        time.sleep(.51)
    elif name.startswith('retired'):
        owner.mode = 'retired'
    elif name == 'historical-results':
        with owner.lock:
            owner.engine.reset_trip(host_now_ns())
            owner.engine.reset_session(host_now_ns())

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            value = json.loads(self.rfile.readline(4096))
            assert value == {'action': 'status'}, 'Render-only fixtures never send mutations'
            result = {'ok': False, 'error': 'Fixture manager unavailable'} if name == 'manager-unavailable' else {
                'ok': True, 'phones': [], 'active': None, 'active_mode': None, 'generation': 1,
                'carplay': {'state': 'disconnected', 'returnReady': False},
                'camera': {'configured': False, 'requested': False, 'phase': 'unavailable', 'error': ''},
                'media': {'available': False, 'controls': []}, 'cluster_pages': ['map', 'media', 'vehicle', 'trip'],
                'cluster_page': 'trip', 'sport': owner.snapshot(), 'error': ''}
            self.wfile.write(json.dumps(result).encode() + b'\n')

    with tempfile.TemporaryDirectory(prefix='headunit-sport-render-') as directory:
        path = Path(directory) / 'manager.sock'
        server = socketserver.UnixStreamServer(str(path), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        app = PhonePage(path, initial_view='cluster' if name == 'cluster-entry' else 'sport')
        app.set_flags(Gio.ApplicationFlags.NON_UNIQUE)
        failures = []
        stage = 0
        changed_at = None
        started_at = time.monotonic()

        def check():
            nonlocal stage, changed_at
            try:
                assert time.monotonic() - started_at < 8, 'Fresh render fixture timed out'
                tab = 'history' if name == 'historical-results' else 'sport' if name.endswith(('sport', 'sport-bottom')) else 'trip'
                if stage == 0:
                    if not hasattr(app, 'sport') or app.pending:
                        return True
                    for field in ('poll_source', 'health_source'):
                        GLib.source_remove(getattr(app, field))
                        setattr(app, field, 0)
                    app.sport.select(tab)
                    stage = 1
                    return True
                if stage == 1:
                    if app.pages.get_transition_running() or not app.window.get_mapped() or app.tab_bar.progress() < 1:
                        return True
                    app.refresh()
                    stage = 2
                    return True
                if stage == 2:
                    if app.pending:
                        return True
                    if name.endswith('bottom'):
                        adjustment = app.sport.scroll.get_vadjustment()
                        if adjustment.get_upper() <= adjustment.get_page_size():
                            return True
                        adjustment.set_value(adjustment.get_upper() - adjustment.get_page_size())
                    app.health_tick()
                    app.window.queue_draw()
                    changed_at = time.monotonic()
                    stage = 3
                    # Return to GTK's REAL main loop so layout, frame-clock and
                    # paint phases finish. Never capture inside a nested loop.
                    return True
                if time.monotonic() - changed_at < .15:
                    return True
                assert len(app.get_windows()) == 1
                capture(app, destination, name, settled=True)
                if name in ('available-sport', 'simulation-sport'):
                    assert app.sport.row_widgets['sport-input-speed'][2].get_text() == '36.0 km/h'
                    assert app.sport.row_widgets['sport-input-longitudinalG'][2].get_text() == '0.000 g'
                    assert app.sport.row_widgets['sport-input-lateralG'][2].get_text() == '-0.102 g'
                    assert app.sport.row_widgets['sport-input-gps'][2].get_text() == 'Available'
                    assert all(item['current'] for item in app.sport.model['inputs'].values())
                if name.startswith(('missing', 'retired')) and tab == 'sport':
                    assert app.sport.row_widgets['sport-input-speed'][2].get_text() == 'Unavailable'
                if name == 'manager-unavailable':
                    assert not app.sport.buttons['trip_reset'].get_sensitive()
                (destination / (name + '.json')).write_text(json.dumps({
                    'scope': 'Real GTK fresh isolated owner; synthetic manager/car values, no physical display/phone',
                    'state': name, 'viewport': [1280, 720], 'windowCount': len(app.get_windows()),
                    'source': app.sport.model['summary'], 'widgetsMapped': app.page_title.get_mapped(),
                    'originalSportModel': app.status.get('sport'),
                    'artifactRenderer': 'Owned Weston output capture; GTK ' + os.environ['GSK_RENDERER'],
                }, indent=2) + '\n')
            except BaseException as error:
                failures.append(repr(error))
                app.close_ui()
                app.quit()
                raise
            app.close_ui()
            app.quit()
            return False

        app.connect('activate', lambda _: GLib.timeout_add(20, check))
        try:
            app.run([])
            assert not failures, failures
        finally:
            app.close_ui()
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)
            assert not worker.is_alive() and app.ui_retired
    print('Fresh isolated sport render PASS:', name)


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--owner':
        load_gui()
        run()
    elif len(sys.argv) == 3 and sys.argv[1] == '--render-one':
        load_gui()
        render_one(sys.argv[2])
    else:
        base = Path(os.environ.get('HEADUNIT_SPORT_ARTIFACTS', tempfile.mkdtemp(prefix='headunit-sport-proof-')))
        # The controller loads no GTK and waits for each actual owner exit.
        # No screenshot owner overlaps another process/window/connection.
        jobs = [('--owner', None)] + [('--render-one', state) for state in REVIEW_STATES]
        for mode, state in jobs:
            environment = dict(os.environ, HEADUNIT_SPORT_ARTIFACTS=str(base / ('action-owner' if state is None else 'review')))
            command = [sys.executable, '-B', __file__, mode] + ([state] if state else [])
            result = subprocess.run(command, env=environment, timeout=15)
            assert result.returncode == 0, (mode, state, result.returncode)
        print('Native sport page owner + eleven fresh isolated state renders: PASS')
