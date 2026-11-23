# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual GTK widgets with explicit fake layer/monitor/manager boundaries.

This exercises our GTK integration, not Linux Wayland layer-surface behavior.
No BlueZ, receiver, physical monitor, phone or camera is contacted.
"""
import copy
import json
from pathlib import Path
import socketserver
import sys
import tempfile
import threading
import time
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_page_presentation import ResidentPresentation
from phone_page import PhonePage, GLib, Gio, Gtk


OUTPUT = 'HDMI-A-1'
ADDRESS = 'AA:BB:CC:DD:EE:01'


class Signals:
    def __init__(self):
        self.handlers = {}
        self.next_handler = 1

    def connect(self, signal, callback):
        number = self.next_handler
        self.next_handler += 1
        self.handlers[number] = (signal, callback)
        return number

    def disconnect(self, number):
        del self.handlers[number]

    def emit(self, signal, *args):
        for expected, callback in list(self.handlers.values()):
            if expected == signal:
                callback(self, *args)


class Monitor(Signals):
    def __init__(self, connector):
        super().__init__()
        self.connector, self.valid = connector, True

    def get_connector(self):
        return self.connector

    def is_valid(self):
        return self.valid


class Monitors(Signals):
    def __init__(self, monitors):
        super().__init__()
        self.monitors = list(monitors)

    def get_n_items(self):
        return len(self.monitors)

    def get_item(self, index):
        return self.monitors[index]

    def replace(self, monitors):
        removed = len(self.monitors)
        self.monitors = list(monitors)
        self.emit('items-changed', 0, removed, len(self.monitors))


class LayerBoundary:
    Layer = SimpleNamespace(BOTTOM='bottom', OVERLAY='overlay')
    Edge = SimpleNamespace(TOP='top', BOTTOM='bottom', LEFT='left', RIGHT='right')
    KeyboardMode = SimpleNamespace(NONE='none', ON_DEMAND='on-demand')

    def __init__(self, supported=True, version=4):
        self.supported, self.version = supported, version
        self.windows = {}
        self.operations = []

    def is_supported(self):
        return self.supported

    def get_protocol_version(self):
        return self.version

    def init_for_window(self, window):
        assert not window.get_realized(), 'Layer initialization must precede GTK realization'
        assert window not in self.windows
        self.windows[window] = {'anchors': {}, 'margins': {}}
        self.operations.append(('init', window))

    def set_namespace(self, window, namespace):
        self.windows[window]['namespace'] = namespace

    def set_monitor(self, window, monitor):
        assert monitor is not None and monitor.is_valid(), 'A compositor-selected default is forbidden'
        self.windows[window]['monitor'] = monitor

    def set_layer(self, window, layer):
        self.windows[window]['layer'] = layer
        self.operations.append(('layer', window, layer))

    def set_keyboard_mode(self, window, mode):
        self.windows[window]['keyboard'] = mode

    def set_exclusive_zone(self, window, zone):
        self.windows[window]['exclusive'] = zone

    def set_anchor(self, window, edge, enabled):
        self.windows[window]['anchors'][edge] = enabled

    def set_margin(self, window, edge, margin):
        self.windows[window]['margins'][edge] = margin

    def set_respect_close(self, window, enabled):
        self.windows[window]['respect_close'] = enabled


class HandoffBoundary:
    """Explicit ready renderer substitute; real file exclusion is tested apart."""
    def __init__(self): self.requested = False
    @property
    def acquired(self): return self.requested
    def request(self): self.requested = True
    def poll(self): return self.requested
    def release(self): self.requested = False


def configuration(corner='bottom-right', margin=16, size=64):
    return {'schemaVersion': 2, 'enabled': True, 'managerSocket': '/fixture/manager.sock',
            'surfaceControlFile': '/fixture/home-surface.request', 'surfaceLeaseFile': '/fixture/home-surface.lease',
            'waylandDisplay': 'wayland-fixture', 'output': OUTPUT,
            'homeButton': {'corner': corner, 'margin': margin, 'size': size}}


def camera(configured=False, requested=False, phase='unavailable', error=''):
    return {'configured': configured, 'requested': requested, 'phase': phase, 'error': error}


class ManagerFixture:
    def __init__(self):
        self.lock = threading.Lock()
        self.active = ADDRESS
        self.camera = camera()
        self.carplay = {'state': 'ready', 'returnReady': True}  # Explicit renderer substitute, independent of active PID.
        self.loss = False
        self.calls = []
        self.hold = False
        self.entered = threading.Event()
        self.release = threading.Event()
        self.completed = threading.Event()
        self.allowed_action = None
        self.hold_timeout = 2
        self.hold_status_after_action = False

    def update(self, *, value=None, active=ADDRESS, loss=False, carplay=None):
        with self.lock:
            self.camera = copy.deepcopy(value if value is not None else camera())
            self.active, self.loss = active, loss
            self.carplay = copy.deepcopy(carplay if carplay is not None else {'state': 'ready', 'returnReady': True})

    def reply(self, value):
        with self.lock:
            self.calls.append(dict(value))
            assert value == {'action': 'status'} or value == self.allowed_action, 'Home/Back must never mutate receiver or phone state'
            hold = self.hold
        if hold:
            self.entered.set()
            assert self.release.wait(self.hold_timeout), 'Fixture failed to release held status'
        with self.lock:
            if value != {'action': 'status'}:
                if self.hold_status_after_action:
                    self.hold = True
                return {'ok': True, 'message': 'Explicit fixture operation accepted'}
            if self.loss:
                reply = {'ok': False, 'error': 'Fixture manager unavailable'}
            else:
                reply = {'ok': True, 'phones': [], 'active': self.active, 'generation': 1,
                         'active_mode': None, 'pairing': {'supported': False},
                         'camera': copy.deepcopy(self.camera), 'error': '',
                         'carplay': copy.deepcopy(self.carplay),
                         'media': {'available': False, 'controls': []}}
                if self.allowed_action:
                    reply['cluster_pages'] = ['vehicle']
        return reply


def destroy_source(number):
    source = GLib.MainContext.default().find_source_by_id(number)
    if source is not None:
        source.destroy()


def run_success_fixture(path, fixture):
    main, cluster = Monitor(OUTPUT), Monitor('HDMI-A-2')
    model = Monitors([cluster, main])  # Deliberately put the wrong display first.
    api = LayerBoundary()
    presentation = ResidentPresentation(api, configuration(), handoff=HandoffBoundary(),
                                        display=SimpleNamespace(get_monitors=lambda: model, sync=lambda: None))
    app = PhonePage(path, presentation=presentation)
    errors, stage = [], 0
    started = time.monotonic()
    window = poll = health = None

    def update(value=None, **changes):
        fixture.update(value=value, **changes)
        app.refresh()

    def blocked():
        assert not presentation.foreground
        assert api.windows[app.window]['layer'] == 'bottom'
        assert api.windows[app.window]['keyboard'] == 'none'
        assert not presentation.home_window.get_visible() and not presentation.home_button.get_sensitive()
        presentation.home_button.emit('clicked')
        assert not presentation.foreground

    def tick():
        nonlocal stage, window, poll, health
        try:
            assert time.monotonic() - started < 12, ('Resident fixture timed out', stage)
            if not hasattr(app, 'back_button') or (app.pending and stage != 18):
                return True
            if stage == 0:
                window, poll, health = app.window, app.poll_source, presentation.health_source
                assert app.get_flags() & Gio.ApplicationFlags.NON_UNIQUE
                assert len(api.windows) == 2 and window.get_visible()
                assert api.windows[window]['monitor'] is main
                assert api.windows[presentation.home_window]['monitor'] is main
                assert api.windows[window]['anchors'] == dict.fromkeys(('top', 'bottom', 'left', 'right'), True)
                assert api.windows[window]['layer'] == 'bottom' and api.windows[window]['keyboard'] == 'none'
                assert api.windows[window]['exclusive'] == 0
                home = api.windows[presentation.home_window]
                assert home['layer'] == 'overlay' and home['keyboard'] == 'none' and home['exclusive'] == 0
                assert home['anchors'] == {'top': False, 'bottom': True, 'left': False, 'right': True}
                assert home['margins'] == {'top': 0, 'bottom': 16, 'left': 0, 'right': 16}
                assert presentation.home_window.get_visible() and presentation.home_button.get_sensitive()
                presentation.home_button.emit('clicked')
                assert presentation.foreground and not presentation.home_window.get_visible()
                assert api.windows[window]['layer'] == 'overlay' and api.windows[window]['keyboard'] == 'on-demand'
                update(active=None)
            elif stage == 1:
                # The manager's selected phone is not proof of video visibility.
                assert presentation.foreground and window.get_visible() and app.status['active'] is None
                app.back_button.emit('clicked')
                assert not presentation.foreground and window.get_visible()
                assert presentation.home_window.get_visible()
                app.activate(app)
                assert app.window is window and app.poll_source == poll and presentation.health_source == health
                assert len(api.windows) == 2
                update(camera(True, True, 'waiting'))
            elif stage == 2:
                blocked()
                update(camera(True, False, 'active'))  # Native/vehicle camera can outlive manual intent.
            elif stage == 3:
                blocked()
                update(camera(True, False, 'unavailable'))
            elif stage == 4:
                blocked()
                update(camera())  # Even disabled-looking status cannot erase retained native intent.
            elif stage == 5:
                blocked()
                update(camera(True, False, 'idle'))
            elif stage == 6:
                assert not presentation.foreground and presentation.home_window.get_visible()
                presentation.home_button.emit('clicked')
                assert presentation.foreground
                update(loss=True)
            elif stage == 7:
                blocked()
                update()
            elif stage == 8:
                assert not presentation.foreground and presentation.home_window.get_visible()
                presentation.home_button.emit('clicked')
                assert presentation.foreground
                update(camera(phase='invalid'))
            elif stage == 9:
                blocked()
                update(camera(True, False, 'idle'))
            elif stage == 10:
                assert presentation.home_window.get_visible()
                presentation.home_button.emit('clicked')
                presentation.last_status -= 6
                presentation._health_tick()
                blocked()
                app.refresh()
            elif stage == 11:
                assert not presentation.foreground and presentation.home_window.get_visible()
                update(camera(True, False, 'idle', 'Camera status unavailable'))
            elif stage == 12:
                blocked()
                update(camera(True, False, 'idle'))
            elif stage == 13:
                assert presentation.home_window.get_visible()
                presentation.home_button.emit('clicked')
                presentation.camera_requested()  # Explicit pending intent lowers before any RPC acknowledgement.
                blocked()
                update(camera(True, True, 'unavailable'))
            elif stage == 14:
                blocked()
                update(camera(True, False, 'stopped'))
            elif stage == 15:
                assert not presentation.foreground and presentation.home_window.get_visible()
                app.view_buttons['music'].set_active(True)
                presentation.home_button.emit('clicked')
                app.back_button.emit('clicked')
                assert app.pages.get_visible_child_name() == 'music' and window.get_visible()
                assert all(call == {'action': 'status'} for call in fixture.calls)
            elif stage == 16:
                # Keep an actual async manager reply pending across GTK shutdown.
                fixture.hold = True
                fixture.release.clear()
                fixture.completed.clear()
                app.refresh()
            elif stage == 17:
                # This stage runs only after the held read if a race allowed it
                # to finish; normal progress is handled by the special pending branch.
                raise AssertionError('Held status unexpectedly completed')
            elif stage == 18:
                if not fixture.entered.is_set():
                    return True
                app.action({'action': 'connect', 'phone': ADDRESS})
                assert not app.back_button.get_visible() and not app.listbox.get_sensitive()
                assert presentation.home_window.get_visible()  # Healthy background refresh does not blink Home.
                app.quit()
                return False
            stage = 18 if stage == 16 else stage + 1
        except Exception as error:
            errors.append(error)
            app.quit()
            return False
        return True

    source = GLib.timeout_add(70, tick)
    try:
        code = app.run([])
    finally:
        fixture.release.set()
        destroy_source(source)
    assert code == 0 and stage == 18 and not errors, (code, stage, errors)
    assert fixture.completed.wait(1), 'The actual held manager reply did not finish'
    assert app.closing and app.ui_retired and app.poll_source == 0
    assert presentation.closed and presentation.retired and presentation.health_source == 0 and not presentation.connections
    assert not window.get_visible() and not presentation.home_window.get_visible()
    assert not window.get_realized() and not presentation.home_window.get_realized()
    assert not app.get_windows() and not model.handlers and not main.handlers
    # Deliver old callbacks explicitly as well as releasing the actual worker.
    app.received('status', {'ok': True, 'camera': camera()})
    app.action({'action': 'connect', 'phone': ADDRESS})
    presentation.open()
    app.activate(app)
    until = time.monotonic() + 0.15
    while time.monotonic() < until:
        GLib.MainContext.default().iteration(False)
        time.sleep(0.005)
    assert not window.get_realized() and not presentation.home_window.get_realized()
    assert all(call == {'action': 'status'} for call in fixture.calls)


def run_output_fixture(path, *, failure=None, corner='top-left', margin=0, size=48):
    main, other = Monitor(OUTPUT), Monitor('HDMI-A-2')
    items = [other, main]
    if failure == 'missing':
        items = [other]
    elif failure == 'ambiguous':
        items.append(Monitor(OUTPUT))
    model = Monitors(items)
    api = LayerBoundary(supported=failure != 'unsupported', version=3 if failure == 'old-protocol' else 4)
    presentation = ResidentPresentation(api, configuration(corner, margin, size), handoff=HandoffBoundary(),
                                        display=SimpleNamespace(get_monitors=lambda: model, sync=lambda: None))
    app = PhonePage(path, presentation=presentation)
    errors, completed = [], False
    started = time.monotonic()

    def tick():
        nonlocal completed
        try:
            assert time.monotonic() - started < 5, 'Output fixture timed out'
            if app.pending or not presentation.home_window or not presentation.camera_known:
                return True
            if presentation.home_window.get_width() == 0 or presentation.home_window.get_height() == 0:
                return True
            state = api.windows[presentation.home_window]
            vertical, horizontal = corner.split('-')
            assert state['anchors'] == {edge: edge in (vertical, horizontal) for edge in ('top', 'bottom', 'left', 'right')}
            assert state['margins'] == {edge: margin if edge in (vertical, horizontal) else 0 for edge in ('top', 'bottom', 'left', 'right')}
            assert (presentation.home_window.get_width(), presentation.home_window.get_height()) == (size, size), \
                ('Home surface size', presentation.home_window.get_width(), presentation.home_window.get_height(), size)
            assert state['monitor'] is main and api.windows[app.window]['monitor'] is main
            presentation.home_button.emit('clicked')
            assert presentation.foreground
            completed = True
            if failure == 'lost':
                model.replace([other])
            elif failure == 'late-ambiguity':
                model.replace([other, main, Monitor(OUTPUT)])
            elif failure == 'invalidated':
                main.valid = False
                main.emit('notify::valid', None)
            elif failure == 'closed':
                presentation.home_window.emit('close-request')
            else:
                app.quit()
            return False
        except Exception as error:
            errors.append(error)
            app.quit()
            return False

    source = GLib.timeout_add(70, tick)
    try:
        code = app.run([])
    finally:
        destroy_source(source)
    assert code == 0 and not errors, (failure, code, errors)
    assert bool(app.startup_error) == bool(failure), (failure, app.startup_error)
    assert completed == (failure not in ('missing', 'ambiguous', 'unsupported', 'old-protocol'))
    assert app.closing and app.ui_retired and app.poll_source == 0 and presentation.closed and presentation.retired and presentation.health_source == 0
    assert not app.window.get_realized() and not app.window.get_visible() and not app.get_windows()
    if presentation.home_window is not None:
        assert not presentation.home_window.get_realized() and not presentation.home_window.get_visible()
    assert not model.handlers and not main.handlers
    # Neither withdrawal nor startup failure ever falls back to the cluster monitor.
    assert all(state.get('monitor') is main for state in api.windows.values())


def run_teardown_failure_fixture(path, fixture):
    """Actual windows still retire after source/disconnect/destruction failures."""
    monitor = Monitor(OUTPUT)
    model = Monitors([monitor])
    api = LayerBoundary()
    presentation = ResidentPresentation(api, configuration(), handoff=HandoffBoundary(),
                                        display=SimpleNamespace(get_monitors=lambda: model, sync=lambda: None))
    app = PhonePage(path, presentation=presentation)
    errors, stage, attempts, dialog, calls_before = [], 0, [], None, None
    started = time.monotonic()

    def tick():
        nonlocal stage, dialog, calls_before
        try:
            assert time.monotonic() - started < 5, 'Teardown fixture timed out'
            if stage == 0:
                if app.pending or not presentation.home_window or not presentation.camera_known:
                    return True
                calls_before = list(fixture.calls)
                # The explicit fake active source is enough to exercise the
                # real GTK confirmation surface; no remove RPC is authorized.
                app.remove(ADDRESS)
                cancelled = next(w for w in Gtk.Window.get_toplevels() if isinstance(w, Gtk.MessageDialog))
                cancelled.response(Gtk.ResponseType.CANCEL)
                assert not cancelled.get_realized() and fixture.calls == calls_before
                app.remove(ADDRESS)
                dialog = next(w for w in Gtk.Window.get_toplevels() if isinstance(w, Gtk.MessageDialog))
                assert dialog.get_realized()
                real_destroy = presentation.home_window.destroy
                real_disconnect = model.disconnect
                failed = {'destroy': False, 'disconnect': False, 'source': False}

                def destroy():
                    attempts.append('home')
                    if not failed['destroy']:
                        failed['destroy'] = True
                        raise RuntimeError('Explicit fake Home destruction fault')
                    real_destroy()

                def disconnect(number):
                    if not failed['disconnect']:
                        failed['disconnect'] = True
                        raise RuntimeError('Explicit fake signal disconnect fault')
                    real_disconnect(number)

                def find_source(number):
                    if not failed['source']:
                        failed['source'] = True
                        def fail_destroy():
                            raise RuntimeError('Explicit fake source removal fault')
                        return SimpleNamespace(destroy=fail_destroy)
                    return GLib.MainContext.default().find_source_by_id(number)

                presentation.home_window.destroy = destroy
                model.disconnect = disconnect
                presentation.GLib = SimpleNamespace(MainContext=SimpleNamespace(default=lambda: SimpleNamespace(find_source_by_id=find_source)))
                assert app.close_ui() is False
                assert app.closing and presentation.closed and not app.ui_retired and not presentation.retired
                assert app.startup_error and app.cleanup_error and presentation.cleanup_error
                assert app.pool_stopped and app.poll_source == 0
                assert app.window in presentation.destroyed_surfaces and not app.window.get_realized()
                assert not dialog.get_realized() and not dialog.get_visible(), 'Remove confirmation survived parent surface retirement'
                dialog.response(Gtk.ResponseType.ACCEPT)  # A retained callback cannot remove a phone after retirement.
                assert fixture.calls == calls_before and not app.pending
                assert presentation.home_window not in presentation.destroyed_surfaces and attempts == ['home']
                assert not presentation.home_window.get_visible()
                operations = list(api.operations)
                presentation.open()
                app.activate(app)
                app.received('status', {'ok': True, 'camera': camera()})
                assert api.operations == operations  # Disabled callbacks cannot undo partial cleanup.
                stage = 1
                return True
            assert app.close_ui() is True
            assert app.ui_retired and presentation.retired and attempts == ['home', 'home']
            assert app.startup_error  # A later successful retry does not turn failure into success.
            assert not app.get_windows() and not model.handlers and not monitor.handlers
            assert presentation.health_source == 0 and not presentation.connections
            assert not dialog.get_realized() and fixture.calls == calls_before
            stage = 2
            app.quit()
            return False
        except Exception as error:
            errors.append(error)
            app.quit()
            return False

    source = GLib.timeout_add(70, tick)
    try:
        code = app.run([])
    finally:
        destroy_source(source)
    assert code == 0 and stage == 2 and not errors, (code, stage, errors)


def run_readiness_fixture(path, fixture, *, home_enabled):
    """Actual GTK actions and held IPC; renderer readiness is explicitly fake."""
    monitor = Monitor(OUTPUT)
    model, api = Monitors([monitor]), LayerBoundary()
    profile = configuration()
    profile['homeButton']['enabled'] = home_enabled
    handoff = HandoffBoundary()
    presentation = ResidentPresentation(api, profile, handoff=handoff,
                                        display=SimpleNamespace(get_monitors=lambda: model, sync=lambda: None))
    fixture.allowed_action = {'action': 'cluster_page', 'page': 'vehicle'}
    calls_before = len(fixture.calls)
    fixture.update(carplay={'state': 'starting', 'returnReady': False})
    app = PhonePage(path, presentation=presentation)
    errors, stage, started = [], 0, time.monotonic()

    def change(carplay, *, active=None):
        fixture.update(carplay=carplay, active=active)
        app.refresh()

    def controls_unavailable():
        assert not app.back_button.get_visible() and not app.back_button.get_sensitive()
        assert not presentation.home_window.get_visible() and not presentation.home_button.get_sensitive()

    def tick():
        nonlocal stage
        try:
            assert time.monotonic() - started < 8, ('Readiness fixture timed out', stage)
            if not hasattr(app, 'back_button') or (app.pending and stage not in (3, 5)):
                return True
            if stage == 0:
                assert app.window.get_visible() and app.status['active'] == ADDRESS
                controls_unavailable()  # A selected/running receiver is not a ready surface.
                presentation.home_button.emit('clicked'); app.back_button.emit('clicked'); presentation.open()
                assert not handoff.requested and not presentation.foreground
                assert api.windows[app.window]['layer'] == 'bottom'
                change({'state': 'ready', 'returnReady': True})
            elif stage == 1:
                assert app.status['active'] is None  # Readiness is independent of the selected-phone field.
                assert presentation.home_window.get_visible() == home_enabled
                assert not app.back_button.get_visible()
                presentation.home_button.emit('clicked')
                assert handoff.requested == home_enabled
                if not home_enabled:
                    # Future external adapter seam, not an implemented physical button.
                    presentation.open()
                assert presentation.foreground and handoff.acquired
                assert app.back_button.get_visible() and app.back_button.get_sensitive()
            elif stage == 2:
                fixture.hold = True; fixture.entered.clear(); fixture.release.clear()
                app.refresh()
                assert app.pending and presentation.foreground and handoff.acquired
                assert app.back_button.get_visible() and app.back_button.get_sensitive()
            elif stage == 3:
                if not fixture.entered.is_set():
                    return True
                # A held read-only background refresh does not unmap Back or
                # Home, nor stop navigation independent of the IPC executor.
                assert app.back_button.get_visible() and app.back_button.get_sensitive()
                for group in (app.listbox, app.media_controls, app.audio_controls, app.native_cluster_pages,
                              app.ultra_styles, app.camera_controls):
                    assert not group.get_sensitive()
                app.back_button.emit('clicked')
                assert not presentation.foreground and not handoff.acquired
                assert presentation.home_window.get_visible() == home_enabled
                if home_enabled:
                    presentation.home_button.emit('clicked')
                else:
                    presentation.open()
                assert presentation.foreground and handoff.acquired
                app.cluster_buttons['vehicle'].emit('clicked')
                assert app.pending and handoff.acquired and presentation.foreground
                fixture.hold = False; fixture.release.set()
            elif stage == 4:
                fixture.hold = True; fixture.entered.clear(); fixture.release.clear()
                app.cluster_buttons['vehicle'].emit('clicked')
                assert app.pending and app.operation_pending() and handoff.acquired and presentation.foreground
                controls_unavailable()
            elif stage == 5:
                if not fixture.entered.is_set():
                    return True
                controls_unavailable()
                app.back_button.emit('clicked'); presentation.home_button.emit('clicked'); presentation.open()
                app.cluster_buttons['vehicle'].emit('clicked')
                assert app.pending and handoff.acquired and presentation.foreground
                fixture.update(carplay={'state': 'starting', 'returnReady': False})
                fixture.hold = False; fixture.release.set()
            elif stage == 6:
                controls_unavailable()
                assert not handoff.acquired and not presentation.foreground
                assert api.windows[app.window]['layer'] == 'bottom' and app.window.get_visible()
                change({'state': 'home', 'returnReady': True})
            elif stage == 7:
                presentation.open()
                assert app.back_button.get_visible() and presentation.foreground
                change({'state': 'fault', 'returnReady': False})
            elif stage == 8:
                controls_unavailable(); assert not handoff.acquired
                change({})  # An older/malformed readiness contract fails closed.
            elif stage == 9:
                controls_unavailable()
                presentation.home_button.emit('clicked'); presentation.open()
                assert not handoff.acquired
                change({'state': 'ready', 'returnReady': True})
            elif stage == 10:
                presentation.open(); assert presentation.foreground
                # Use an explicit test clock mutation, not a simulated video heartbeat.
                app.status_requested_at -= 6
                app.health_tick()
                controls_unavailable()
                assert not app.listbox.get_sensitive() and not handoff.acquired
                app.action({'action': 'connect', 'phone': ADDRESS})
                app.refresh()
            elif stage == 11:
                presentation.open(); assert presentation.foreground
                app.status_requested_at = time.monotonic() + 1  # Reference-clock reversal is not freshness.
                app.health_tick(); controls_unavailable()
                assert not handoff.acquired and not app.listbox.get_sensitive()
                app.refresh()
            elif stage == 12:
                fixture.update(loss=True); app.refresh()
            elif stage == 13:
                controls_unavailable()
                assert not app.status_available and not app.listbox.get_sensitive()
                added_calls = fixture.calls[calls_before:]
                assert [c for c in added_calls if c != {'action': 'status'}] == [fixture.allowed_action]
                app.quit(); return False
            stage += 1
        except Exception as error:
            errors.append(error); fixture.hold = False; fixture.release.set(); app.quit(); return False
        return True

    source = GLib.timeout_add(60, tick)
    try:
        result = app.run([])
    finally:
        fixture.hold = False; fixture.release.set(); destroy_source(source)
        fixture.allowed_action = None
    assert result == 0 and stage == 13 and not errors, (home_enabled, result, stage, errors)
    assert app.ui_retired and not app.poll_source and not app.health_source and presentation.retired
    assert not app.get_windows() and not model.handlers


def run_session_change_fixture(path, fixture, command, changes_session, *, opened=True):
    """Hold the refresh after a real async mutation acknowledgment.

    The fixture explicitly supplies renderer readiness; no actual phone or
    renderer is contacted. An old ready reply cannot outlive a retired session.
    """
    monitor = Monitor(OUTPUT)
    model, api, handoff = Monitors([monitor]), LayerBoundary(), HandoffBoundary()
    presentation = ResidentPresentation(api, configuration(), handoff=handoff,
                                        display=SimpleNamespace(get_monitors=lambda: model, sync=lambda: None))
    fixture.hold = False; fixture.update(); fixture.allowed_action = command
    fixture.hold_status_after_action = True
    calls_before = len(fixture.calls)
    app = PhonePage(path, presentation=presentation)
    errors, stage, started = [], 0, time.monotonic()

    def tick():
        nonlocal stage
        try:
            assert time.monotonic() - started < 5, ('Mutation readiness fixture timed out', stage)
            if not hasattr(app, 'back_button') or (app.pending and stage != 1): return True
            if stage == 0:
                if opened: presentation.open()
                assert presentation.foreground == opened and handoff.acquired == opened
                fixture.update(active=None if changes_session else ADDRESS,
                               carplay={'state': 'disconnected' if changes_session else 'home',
                                        'returnReady': not changes_session})
                fixture.entered.clear(); fixture.release.clear()
                app.action(command)
                assert app.operation_pending() and not app.back_button.get_visible()
            elif stage == 1:
                if not fixture.entered.is_set() or app.pending_action != 'status': return True
                # Acknowledgment is known, but its following fresh status is
                # held. Only inactive-phone preferences may retain the owner.
                assert presentation.foreground == (opened and not changes_session)
                assert handoff.acquired == (opened and not changes_session)
                assert app.status_available == (not changes_session)
                assert app.back_button.get_visible() == (opened and not changes_session)
                assert presentation.home_window.get_visible() == (not opened and not changes_session)
                app.back_button.emit('clicked')
                if changes_session:
                    presentation.home_button.emit('clicked')
                    assert not handoff.acquired and not presentation.foreground
                fixture.hold = False; fixture.release.set()
            elif stage == 2:
                assert app.status_available and not presentation.foreground and not handoff.acquired
                if changes_session:
                    assert not app.can_return() and not presentation.home_window.get_visible()
                    assert api.windows[app.window]['layer'] == 'bottom'
                fixture.update(); app.refresh()
            elif stage == 3:
                assert presentation.home_window.get_visible() and not presentation.foreground
                assert not app.back_button.get_visible() and not handoff.acquired
                assert [c for c in fixture.calls[calls_before:] if c['action'] != 'status'] == [command]
                app.quit(); return False
            stage += 1
        except Exception as error:
            errors.append(error); fixture.hold = False; fixture.release.set(); app.quit(); return False
        return True

    source = GLib.timeout_add(60, tick)
    try:
        result = app.run([])
    finally:
        fixture.hold = False; fixture.release.set(); destroy_source(source)
        fixture.allowed_action = None; fixture.hold_status_after_action = False
    assert result == 0 and stage == 3 and not errors, (command, result, stage, errors)
    assert app.ui_retired and presentation.retired and not app.get_windows() and not model.handlers


def run_modal_lower_fixture(path, fixture):
    """A real GTK confirmation must withdraw before the main lease is released."""
    monitor = Monitor(OUTPUT); model = Monitors([monitor]); api = LayerBoundary()
    handoff = HandoffBoundary()
    presentation = ResidentPresentation(api, configuration(), handoff=handoff,
        display=SimpleNamespace(get_monitors=lambda: model, sync=lambda: None))
    fixture.update()
    app = PhonePage(path, presentation=presentation)
    stage, dialog, errors = 0, None, []
    response_delivered = False
    calls_before = len(fixture.calls)
    started = time.monotonic()
    def queued_accept():
        nonlocal response_delivered
        dialog.response(Gtk.ResponseType.ACCEPT)
        response_delivered = True
        return False
    def tick():
        nonlocal stage, dialog
        try:
            assert time.monotonic() - started < 4, ('Modal withdrawal fixture timed out', stage)
            if not hasattr(app, 'window') or (app.pending and stage in (0, 3)): return True
            if stage == 0:
                presentation.open(); assert handoff.acquired and presentation.foreground
                app.remove(ADDRESS)
                dialog = next(w for w in Gtk.Window.get_toplevels() if isinstance(w, Gtk.MessageDialog))
                stage = 1
            elif stage == 1:
                assert dialog.get_realized() and dialog.get_visible()
                assert app.status_current()
                # Model an already-clicked response queued before Back withdraws
                # the real GTK parent. Keep status fresh so only dialog lifetime
                # can prevent this retired confirmation from reaching the RPC.
                GLib.idle_add(queued_accept)
                presentation.back()
                assert not presentation.foreground and not handoff.acquired
                assert not dialog.get_realized() and not dialog.get_visible(), 'Remove confirmation survived Home withdrawal'
                stage = 2
            elif stage == 2:
                # The queued click runs at idle priority; wait for it (bounded
                # by the fixture timeout) instead of assuming one 50 ms tick.
                if not response_delivered:
                    return True
                assert app.status_current()
                assert not app.pending and all(call['action'] == 'status' for call in fixture.calls[calls_before:]), 'Queued confirmation acted after dialog retirement'
                app.refresh(); stage = 3
            elif stage == 3:
                assert app.status_current() and presentation.home_window.get_visible()
                app.quit(); stage = 4; return False
        except Exception as error:
            errors.append(error); app.quit(); return False
        return True
    source = GLib.timeout_add(50, tick)
    try: result = app.run([])
    finally: destroy_source(source)
    assert result == 0 and stage == 4 and not errors, (result, stage, errors)
    assert app.ui_retired and presentation.retired and not app.get_windows() and not model.handlers


def run_delayed_status_fixture(path, fixture):
    """Actual GTK/private socket and host time; renderer/layers remain explicit fixtures."""
    monitor = Monitor(OUTPUT); model = Monitors([monitor]); api = LayerBoundary()
    handoff = HandoffBoundary()
    presentation = ResidentPresentation(api, configuration(), handoff=handoff,
        display=SimpleNamespace(get_monitors=lambda: model, sync=lambda: None))
    fixture.update(); fixture.hold_timeout = 8
    app = PhonePage(path, presentation=presentation)
    stage, held_at, errors = 0, None, []
    started = time.monotonic()
    def tick():
        nonlocal stage, held_at
        try:
            assert time.monotonic() - started < 9, ('Delayed status fixture timed out', stage)
            if not hasattr(app, 'window'): return True
            if stage == 0:
                if app.pending or not app.status_current(): return True
                destroy_source(app.poll_source); app.poll_source = 0
                presentation.open(); assert handoff.acquired and presentation.foreground
                fixture.hold = True; fixture.entered.clear(); fixture.release.clear()
                held_at = time.monotonic(); app.refresh(); stage = 1
            elif stage == 1:
                if not fixture.entered.is_set() or time.monotonic() - held_at < 5.3: return True
                assert not app.status_current() and not handoff.acquired and not presentation.foreground
                assert app.retry_button.get_visible() and not app.retry_button.get_sensitive()
                calls = len(fixture.calls)
                app.retry_button.emit('clicked'); app.retry_button.emit('clicked')
                assert len(fixture.calls) == calls and app.pending_action == 'status'
                fixture.hold = False; fixture.release.set(); stage = 2
            elif stage == 2:
                if app.pending: return True
                assert not app.status_current(), 'Late status completion revived old command authority'
                assert not presentation.home_window.get_visible() and not app.back_button.get_visible()
                assert not handoff.acquired and not app.listbox.get_sensitive()
                calls = len(fixture.calls)
                app.action({'action': 'connect', 'phone': ADDRESS})
                assert len(fixture.calls) == calls and not app.pending
                assert app.retry_button.get_visible() and app.retry_button.get_sensitive()
                app.retry_button.emit('clicked'); stage = 3
            elif stage == 3:
                if app.pending: return True
                assert app.status_current() and presentation.home_window.get_visible()
                assert not app.retry_button.get_visible()
                assert app.status_requested_at == presentation.last_status
                presentation.open(); assert handoff.acquired and app.can_return()
                app.quit(); stage = 4; return False
        except Exception as error:
            errors.append(error); fixture.hold = False; fixture.release.set(); app.quit(); return False
        return True
    source = GLib.timeout_add(40, tick)
    try: result = app.run([])
    finally:
        fixture.hold = False; fixture.release.set(); fixture.hold_timeout = 2; destroy_source(source)
    assert result == 0 and stage == 4 and not errors, (result, stage, errors)
    assert app.ui_retired and presentation.retired and not app.get_windows() and not model.handlers


def main():
    fixture = ManagerFixture()

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            value = json.loads(self.rfile.readline(4096))
            result = fixture.reply(value)
            self.wfile.write(json.dumps(result).encode() + b'\n')
            fixture.completed.set()

    with tempfile.TemporaryDirectory(prefix='hu-resident-') as folder:
        path = Path(folder).resolve() / 'manager.sock'
        server = socketserver.UnixStreamServer(str(path), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            run_success_fixture(path, fixture)
            fixture.hold = False
            fixture.update()
            for corner in ('top-left', 'top-right', 'bottom-left', 'bottom-right'):
                run_output_fixture(path, corner=corner, margin=96, size=96)
            for failure in ('missing', 'ambiguous', 'unsupported', 'old-protocol', 'lost', 'late-ambiguity', 'invalidated', 'closed'):
                run_output_fixture(path, failure=failure)
            run_teardown_failure_fixture(path, fixture)
            run_modal_lower_fixture(path, fixture)
            run_delayed_status_fixture(path, fixture)
            for enabled in (True, False):
                fixture.update()
                run_readiness_fixture(path, fixture, home_enabled=enabled)
            for command in ({'action': 'disconnect', 'phone': ADDRESS, 'generation': 1},
                            {'action': 'connect', 'phone': ADDRESS},
                            {'action': 'forget', 'phone': ADDRESS}):
                run_session_change_fixture(path, fixture, command, True)
            for command in ({'action': 'forget', 'phone': 'AA:BB:CC:DD:EE:02'},):
                run_session_change_fixture(path, fixture, command, False)
            for command in ({'action': 'disconnect', 'phone': ADDRESS, 'generation': 1},
                            {'action': 'connect', 'phone': ADDRESS}):
                run_session_change_fixture(path, fixture, command, True, opened=False)
        finally:
            fixture.release.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
    print('Native resident GTK: Home/Back, readiness/busy/expiry, delayed status completion exclusion/fresh recovery, session-mutation acknowledgment with held refresh, inactive-phone ownership retention, optional Home, exact output/corners, camera/loss exclusion, activation, Remove cancel/Home withdrawal/parent retirement and late nonaction, partial teardown retry passed; layer/monitor/renderer boundary is explicitly simulated')


if __name__ == '__main__':
    main()
