# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Optional output-pinned native underlay/Home presentation.

This module imports neither GTK nor layer-shell. The enabled wrapper must load
libgtk4-layer-shell before GTK and inject its API. Camera snapshot exclusion is
defensive UI behavior, not independent reverse-camera lease coordination; boot
admission currently excludes configured native cameras from this variant.
"""
import os
import re
import sys
import time
from pathlib import Path
import stat

STATUS_MAXIMUM_AGE_SECONDS = 5


def claim_boot_splash(directory, *, elapsed=None):
    """One attempt per real kernel boot, including after a failed/dead player.

    XDG_RUNTIME_DIR is volatile. O_EXCL prevents concurrent/restarted pages
    replaying; late service starts and uncertain clock/path state simply skip.
    No marker is stored in a release or in persistent phone/pairing state.
    """
    fd = -1
    try:
        elapsed = time.clock_gettime(time.CLOCK_BOOTTIME) if elapsed is None else elapsed
        if type(elapsed) not in (float, int) or not 0 <= elapsed <= 60:
            return False
        directory = Path(directory)
        if not directory.is_absolute() or directory.resolve(strict=True) != directory:
            return False
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        info = os.fstat(fd)
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
            return False
        marker = os.open('headunit-boot-attempt', os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                         os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=fd)
        os.close(marker)
        return True
    except (OSError, AttributeError, ValueError):
        return False
    finally:
        if fd >= 0:
            os.close(fd)


class BootSplash:
    """Live native-resolution GPU decoration below phone foregrounds.

    The existing page owns both layer-shell surfaces and GTK's renderer. No
    separate player, decoder, connection owner or framebuffer readback is used.
    Once-per-boot admission is unchanged. Time never grants phone readiness.
    """
    DURATION_SECONDS = 4.8
    FADE_START_SECONDS = 4.0

    def __init__(self, page, layer, display, outputs, Gtk, GLib, on_ready=None):
        self.GLib, self.windows, self.connections = GLib, [], []
        self.page, self.canvases = page, []
        self.deadline = self.completion = self.failure = self.fade = self.ready = 0
        self.on_ready = on_ready
        self.pending_outputs = set(outputs)
        self.started_at = None
        self.closed = False
        try:
            import cairo
            import gi
            gi.require_foreign('cairo')
            # An empty Wayland input region lets the native controls below
            # receive input. Cairo is used only for this region, not drawing.
            input_region = cairo.Region()
            from phone_icons import startup_canvas
            monitors = display.get_monitors()
            if not 1 <= len(outputs) <= 2 or len(set(outputs)) != len(outputs) or monitors.get_n_items() > 64:
                raise ValueError('Invalid startup outputs')
            targets = []
            for name in outputs:
                found = [monitors.get_item(i) for i in range(monitors.get_n_items())
                         if monitors.get_item(i).is_valid() and monitors.get_item(i).get_connector() == name]
                if len(found) != 1:
                    raise ValueError('Startup output unavailable')
                targets.append(found[0])
            for name, monitor in zip(outputs, targets):
                window = Gtk.ApplicationWindow(application=page, title='HEADUNIT startup')
                self.windows.append(window)
                window.set_decorated(False)
                layer.init_for_window(window)
                layer.set_namespace(window, 'headunit-boot-splash')
                layer.set_monitor(window, monitor)
                layer.set_layer(window, layer.Layer.BOTTOM)
                layer.set_keyboard_mode(window, layer.KeyboardMode.NONE)
                layer.set_exclusive_zone(window, 0)
                for edge in (layer.Edge.TOP, layer.Edge.BOTTOM, layer.Edge.LEFT, layer.Edge.RIGHT):
                    layer.set_anchor(window, edge, True)
                if hasattr(layer, 'set_respect_close'):
                    layer.set_respect_close(window, True)
                canvas = startup_canvas(self, name)
                self.canvases.append(canvas)
                window.set_child(canvas)
                self.connections.append((window, window.connect('close-request', self._withdrawn)))
                self.connections.append((monitor, monitor.connect('notify::valid', self._output_changed)))
                self.connections.append((monitor, monitor.connect('notify::connector', self._output_changed)))
                window.present()
                surface = window.get_surface()
                if surface is None:
                    raise ValueError('Startup surface unavailable')
                surface.set_input_region(input_region)
            self.deadline = GLib.timeout_add(6500, self._timeout)
            print('HEADUNIT boot animation started: live-vector', file=sys.stderr, flush=True)
        except Exception:
            self.close('unavailable')

    def drawn(self, output):
        # Exactly one observation per admitted output, at scene construction.
        # This is neither a Wayland commit nor a physical scanout receipt.
        if not self.closed and output in self.pending_outputs:
            try:
                date = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
                print(f'Headunit splash first scene constructed output={output} mono_ns={date}',
                      file=sys.stderr, flush=True)
            except Exception:
                pass
        if not self.closed and self.started_at is None:
            self.started_at = time.monotonic()
            self.fade = self.GLib.timeout_add(int(self.FADE_START_SECONDS * 1000), self._fade)
            self.completion = self.GLib.timeout_add(int(self.DURATION_SECONDS * 1000), self._complete)
        self.pending_outputs.discard(output)
        if not self.pending_outputs:
            self._queue_ready()

    def _queue_ready(self):
        if self.on_ready is not None and not self.ready:
            self.ready = self.GLib.idle_add(self._ready)

    def _ready(self):
        self.ready = 0
        callback, self.on_ready = self.on_ready, None
        if callback is not None:
            callback()
        return False

    def _fade(self):
        self.fade = 0
        if not self.closed:
            for canvas in self.canvases:
                canvas.resume()
        return False

    def render_failed(self):
        # Snapshot callbacks must unwind before destroying their native windows.
        if not self.closed and not self.failure:
            self.failure = self.GLib.idle_add(self._failed)

    def _failed(self):
        self.failure = 0
        self.close('render-error')
        return False

    def _complete(self):
        self.completion = 0
        self.close('complete')
        return False

    def _timeout(self):
        self.deadline = 0
        self.close('deadline')
        return False

    def _withdrawn(self, *_):
        self.close('surface-withdrawn')
        return True

    def _output_changed(self, *_):
        self.close('output-changed')

    def close(self, reason='owner-retired'):
        if self.closed:
            return
        self.closed = True
        errors = []
        for name in ('deadline', 'completion', 'failure', 'fade', 'ready'):
            source_id = getattr(self, name)
            if source_id:
                try:
                    source = self.GLib.MainContext.default().find_source_by_id(source_id)
                    if source is not None:
                        source.destroy()
                except Exception:
                    errors.append('timer')
                setattr(self, name, 0)
        for obj, handler in self.connections:
            try:
                obj.disconnect(handler)
            except Exception:
                errors.append('callback')
        self.connections.clear()
        for canvas in self.canvases:
            try:
                canvas.retire()
            except Exception:
                errors.append('canvas')
        self.canvases.clear()
        for window in self.windows:
            try:
                window.set_visible(False)
            except Exception:
                errors.append('withdrawal')
            try:
                window.destroy()
            except Exception:
                errors.append('surface')
        self.windows.clear()
        # A failed/skipped decoration must not prevent the useful interface
        # and fallback from starting. Destruction precedes this notification.
        self._queue_ready()
        print('HEADUNIT boot animation retired: ' + reason, file=sys.stderr, flush=True)
        if errors and hasattr(self.page, 'fail_resident'):
            self.page.fail_resident('Could not retire startup decoration')


def status_fresh(observed_at, maximum_age=STATUS_MAXIMUM_AGE_SECONDS):
    """An IPC/GTK delay cannot extend the source observation's authority."""
    return type(observed_at) in (int, float) and 0 <= time.monotonic() - observed_at <= maximum_age


def carplay_snapshot(value):
    """Readiness comes from genuine renderer receipts, never a receiver PID."""
    if type(value) is not dict or set(value) != {'state', 'returnReady'} or \
            type(value['state']) is not str or \
            value['state'] not in ('disconnected', 'starting', 'ready', 'home', 'fault', 'stopped') or \
            type(value['returnReady']) is not bool or \
            (value['returnReady'] and value['state'] not in ('ready', 'home')):
        raise ValueError('CarPlay readiness unavailable')
    return dict(value)


def recovery_phone(value):
    """Only the current phone/budget grants a cancel target; diagnostics do not."""
    if (type(value) is not dict or not 2 <= len(value) <= 4 or
            not {'phone', 'remainingAttempts'} <= set(value) <=
            {'phone', 'remainingAttempts', 'schedule', 'decisions'} or
            type(value['remainingAttempts']) is not int or not 1 <= value['remainingAttempts'] <= 3 or
            type(value['phone']) is not str or
            re.fullmatch(r'(?:[0-9A-F]{2}:){5}[0-9A-F]{2}', value['phone']) is None):
        return None

    # Admit only the declared bounded diagnostic schemas. Their dates, state
    # and historical budgets are never consulted for current cancellation.
    import math

    def budget(number):
        return type(number) is int and 0 <= number <= 3

    def date(number):
        return number is None or (type(number) is float and math.isfinite(number) and number >= 0)

    if 'schedule' in value:
        schedule = value['schedule']
        if (type(schedule) is not dict or len(schedule) != 8 or set(schedule) !=
                {'authority', 'configuredMaximum', 'queued', 'state', 'clockDomain',
                 'observedMonotonicSeconds', 'deadlineMonotonicSeconds', 'remainingWaitSeconds'} or
                schedule['authority'] != 'software_scheduling_intent' or
                not budget(schedule['configuredMaximum']) or type(schedule['queued']) is not bool or
                type(schedule['state']) is not str or schedule['state'] not in ('disabled', 'queued', 'idle') or
                schedule['clockDomain'] not in (None, 'python-time.monotonic') or
                not all(date(schedule[key]) for key in
                        ('observedMonotonicSeconds', 'deadlineMonotonicSeconds', 'remainingWaitSeconds'))):
            return None
    if 'decisions' in value:
        decisions = value['decisions']
        if type(decisions) is not list or len(decisions) > 8:
            return None
        for record in decisions:
            if (type(record) is not dict or len(record) != 7 or set(record) !=
                    {'decision', 'generation', 'configuredMaximum', 'remainingAttempts',
                     'exitCode', 'clockDomain', 'observedMonotonicSeconds'} or
                    type(record['decision']) is not str or record['decision'] not in
                    ('queued', 'dispatch', 'child_started', 'cancelled', 'cancelled_selection',
                     'cancelled_pairing', 'cancelled_owner', 'cancelled_removed', 'budget_exhausted',
                     'not_eligible', 'retirement_required', 'attempt_failed') or
                    type(record['generation']) is not int or not 0 <= record['generation'] < 2 ** 64 or
                    not budget(record['configuredMaximum']) or not budget(record['remainingAttempts']) or
                    (record['exitCode'] is not None and
                     (type(record['exitCode']) is not int or not -(2 ** 31) <= record['exitCode'] < 2 ** 31)) or
                    record['clockDomain'] not in (None, 'python-time.monotonic') or
                    not date(record['observedMonotonicSeconds'])):
                return None
    return value['phone']


def camera_snapshot(value):
    if type(value) is not dict or set(value) != {'configured', 'requested', 'phase', 'error'} or \
            type(value['configured']) is not bool or type(value['requested']) is not bool or \
            type(value['phase']) is not str or \
            value['phase'] not in ('idle', 'waiting', 'active', 'fault', 'stopped', 'unavailable') or \
            type(value['error']) is not str or len(value['error']) > 128 or \
            any(ord(c) < 32 or ord(c) == 127 for c in value['error']):
        raise ValueError('Camera information unavailable')
    return dict(value)


class ResidentPresentation:
    """Two GTK surfaces on one explicit connector; Home/Back change no phone state."""
    HANDOFF_POLL_MS = 25
    HANDOFF_WAIT_SECONDS = 3

    def __init__(self, layer_shell, configuration, *, display=None, handoff=None):
        if type(configuration) is not dict or configuration.get('enabled') is not True:
            raise ValueError('Resident phone presentation must be explicitly enabled')
        output = configuration.get('output')
        home = configuration.get('homeButton')
        if type(output) is not str or not output or len(output) > 128 or any(not 33 <= ord(c) <= 126 for c in output):
            raise ValueError('Resident phone page requires an explicit output connector')
        if type(home) is not dict or not {'corner', 'margin', 'size'} <= set(home) <= {'corner', 'margin', 'size', 'enabled'} or \
                ('enabled' in home and type(home['enabled']) is not bool) or \
                home['corner'] not in ('bottom-right', 'bottom-left', 'top-right', 'top-left') or \
                type(home['margin']) is not int or not 0 <= home['margin'] <= 96 or \
                type(home['size']) is not int or not 48 <= home['size'] <= 96:
            raise ValueError('Invalid native Home button configuration')
        self.layer_shell, self.output, self.home = layer_shell, output, dict(home)
        self.home_enabled = home.get('enabled', True)
        if handoff is None:
            raise ValueError('Resident Home requires a coordinated main-surface owner')
        self.handoff = handoff
        self.display = display
        self.page = self.monitor = self.monitors = self.home_window = self.home_button = None
        self.closed, self.available, self.foreground = False, False, False
        self.retired = False
        self.cleanup_error = ''
        self.destroyed_surfaces = set()
        self.camera_known, self.camera_blocked, self.retained_camera_intent = False, True, False
        self.error = ''
        self.last_status = None
        self.carplay_known, self.return_ready, self.pending = False, False, False
        self.connections = []
        self.health_source = 0
        self.handoff_source = 0
        self.opening = False
        self.opening_at = None
        # Temporary opt-in diagnosis is limited to this native Home surface.
        # Never print device identities, coordinates, or phone protocol data.
        self.input_diagnostics = os.environ.get('HEADUNIT_HOME_INPUT_DIAGNOSTICS') == '1'
        self.input_diagnostic_count = 0
        self.startup_outputs = ()
        self.boot_splash = None
        self.startup_ready = lambda: None

    def _trace_home(self, event):
        if self.input_diagnostics and self.input_diagnostic_count < 32:
            self.input_diagnostic_count += 1
            print('Native Home input ' + event, file=sys.stderr, flush=True)

    def _home_clicked(self, _):
        self._trace_home('clicked')
        if self._home_permitted():
            self.open()

    def _resolve_monitor(self):
        count = self.monitors.get_n_items()
        if count > 64:
            raise ValueError('Too many outputs to identify the native phone display')
        matches = []
        for index in range(count):
            monitor = self.monitors.get_item(index)
            if monitor is not None and monitor.get_connector() == self.output and monitor.is_valid():
                matches.append(monitor)
        if len(matches) != 1:
            raise ValueError('Configured native phone output is missing or ambiguous')
        return matches[0]

    def attach(self, page, Gtk, Gdk):
        """Configure both windows before realization. Called once by PhonePage."""
        if self.closed or self.page is not None:
            raise RuntimeError('Resident phone presentation was already attached or closed')
        self.page = page
        self.Gtk = Gtk
        # Import only after the enabled wrapper loaded layer-shell and GTK.
        from gi.repository import GLib
        self.GLib = GLib
        api = self.layer_shell
        if not api.is_supported() or api.get_protocol_version() < 4:
            raise RuntimeError('Resident phone page requires Wayland layer-shell protocol version 4')
        self.display = self.display if self.display is not None else Gdk.Display.get_default()
        if self.display is None:
            raise RuntimeError('Native phone display is unavailable')
        self.monitors = self.display.get_monitors()
        self.monitor = self._resolve_monitor()

        window = page.window
        api.init_for_window(window)
        api.set_namespace(window, 'headunit-phone-page')
        api.set_monitor(window, self.monitor)
        api.set_layer(window, api.Layer.BOTTOM)
        api.set_exclusive_zone(window, 0)
        api.set_keyboard_mode(window, api.KeyboardMode.NONE)
        for edge in (api.Edge.TOP, api.Edge.BOTTOM, api.Edge.LEFT, api.Edge.RIGHT):
            api.set_anchor(window, edge, True)
            api.set_margin(window, edge, 0)

        self.home_window = Gtk.ApplicationWindow(application=page, title='Headunit Home')
        self.home_window.set_decorated(False)
        self.home_window.add_css_class('home-window')
        self.home_window.set_default_size(self.home['size'], self.home['size'])
        api.init_for_window(self.home_window)
        api.set_namespace(self.home_window, 'headunit-home')
        api.set_monitor(self.home_window, self.monitor)
        api.set_layer(self.home_window, api.Layer.OVERLAY)
        api.set_exclusive_zone(self.home_window, 0)
        api.set_keyboard_mode(self.home_window, api.KeyboardMode.NONE)
        vertical, horizontal = self.home['corner'].split('-')
        for name in ('top', 'bottom', 'left', 'right'):
            edge = getattr(api.Edge, name.upper())
            anchor = name in (vertical, horizontal)
            api.set_anchor(self.home_window, edge, anchor)
            api.set_margin(self.home_window, edge, self.home['margin'] if anchor else 0)
        self.home_button = Gtk.Button()
        self.home_button.add_css_class('home-button')
        self.home_button.set_size_request(self.home['size'], self.home['size'])
        image = Gtk.Image.new_from_icon_name('go-home-symbolic')
        image.set_pixel_size(26)
        self.home_button.set_child(image)
        self.home_button.set_tooltip_text('Home')
        self.home_button.update_property([Gtk.AccessibleProperty.LABEL], ['Home'])
        self.home_button.connect('clicked', self._home_clicked)
        if self.input_diagnostics:
            observer = Gtk.EventControllerLegacy()
            observer.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)

            def input_event(controller, event):
                # GTK 4.18/PyGObject can emit this signal with an unboxed
                # event (None). The controller still exposes its real device.
                device = controller.get_current_event_device()
                source = device.get_source().value_nick if device is not None else 'unknown'
                kind = event.get_event_type().value_nick if event is not None else 'unboxed'
                self._trace_home('event=' + kind + ' source=' + source)
                return False  # Observe only: preserve the widget's normal dispatch.
            observer.connect('event', input_event)
            self.home_button.add_controller(observer)
        self.home_window.set_child(self.home_button)
        for surface in (window, self.home_window):
            if hasattr(api, 'set_respect_close'):
                api.set_respect_close(surface, True)
            self.connections.append((surface, surface.connect('close-request', self._surface_closed)))
        self.connections.append((self.monitors, self.monitors.connect('items-changed', self._outputs_changed)))
        for signal in ('notify::valid', 'notify::connector'):
            self.connections.append((self.monitor, self.monitor.connect(signal, self._outputs_changed)))
        self.available = True
        play_startup = self.startup_outputs and claim_boot_splash(os.environ.get('XDG_RUNTIME_DIR', ''))
        if play_startup:
            window.set_opacity(0.)
        window.present()
        if play_startup:
            self.boot_splash = BootSplash(page, api, self.display, self.startup_outputs, Gtk, GLib,
                                          on_ready=self._startup_established)
        else:
            self._startup_established()
        self._update_home()
        self.health_source = GLib.timeout_add(250, self._health_tick)

    def _startup_established(self):
        if not self.closed:
            # Process the submitted surface requests before allowing the
            # fallback to start. This is not a scanout/presentation receipt.
            self.display.sync()
            self.page.window.set_opacity(1.)
            try:
                self.startup_ready()
            except OSError:
                self.page.fail_resident('Could not notify native-page startup')

    def _output_current(self):
        try:
            return self.monitor is not None and self._resolve_monitor() is self.monitor
        except Exception:
            return False

    def _outputs_changed(self, *_):
        if not self.closed and not self._output_current():
            self._fail('Configured native phone output changed or disappeared')

    def _surface_closed(self, *_):
        if not self.closed:
            self._fail('The compositor withdrew a native phone surface')
        return True

    def _fail(self, message):
        if self.closed:
            return
        self.error = message
        page = self.page
        if page is not None:
            page.fail_resident(message)
        else:
            self.close()

    def reactivate(self):
        """Repeated application activation neither rebuilds nor raises the page."""
        if not self.closed:
            self._outputs_changed()

    def _update_home(self):
        if self.home_window is None or self.closed:
            return
        permitted = self._home_permitted()
        self.home_button.set_sensitive(permitted)
        if permitted:
            if not self.home_window.get_visible():
                self.home_window.set_visible(True)
        else:
            self.home_window.set_visible(False)
        if self.page is not None and hasattr(self.page, 'update_navigation'):
            self.page.update_navigation()

    def _status_current(self):
        return status_fresh(self.last_status)

    def _safe(self):
        return bool(not self.closed and self.available and self.camera_known and not self.camera_blocked and
                    self.carplay_known and self._status_current())

    def _home_permitted(self):
        return bool(self.home_enabled and self._safe() and self.return_ready and not self.pending and
                    not self.foreground and not self.opening)

    def can_return(self):
        """Only a healthy retained CarPlay surface can receive an explicit Back."""
        return bool(self._safe() and self.return_ready and not self.pending and self.foreground and
                    not self.opening and self.handoff.acquired)

    def set_pending(self, pending):
        self.pending = bool(pending)
        self._update_home()  # Busy controls do not relinquish an existing owner.

    def open(self):
        # Public admission seam for a future physical Home adapter. With no
        # usable CarPlay, the native page already occupies BOTTOM; never take
        # exclusive ownership that would prevent a new receiver bootstrapping.
        if not self._safe() or self.pending:
            return
        if not self._output_current():
            self._fail('Configured native phone output changed or disappeared')
            return
        if self.foreground or self.opening:
            return
        if not self.return_ready:
            self.page.window.present()
            return
        try:
            # ON asks the renderer to retire its main window. Never cover a
            # live GPU window: a hidden opaque commit receives no genuine
            # presentation receipt and cannot maintain its qualified clock.
            self.handoff.request()
            self._trace_home('withdrawal-requested')
            self.opening = True
            self.opening_at = time.monotonic()
            self._update_home()
            self.handoff_source = self.GLib.timeout_add(self.HANDOFF_POLL_MS, self._handoff_tick)
            self._handoff_tick()
        except Exception:
            self._fail('Could not request the main display for Home')

    def _handoff_tick(self):
        if self.closed or not (self.opening or self.foreground):
            return False
        try:
            if not self._safe() or not self.return_ready or not self._output_current():
                self.back()
                return False
            acquired = self.handoff.poll()
            if self.foreground:
                if not acquired:
                    raise RuntimeError('Home lost its main-surface ownership')
            elif acquired:
                # Exclusive ownership is the acknowledgement that the actual
                # renderer window has gone. The decoder/session remains alive.
                self._move_page(True)
                self._trace_home('ownership-acquired')
                self.foreground, self.opening = True, False
                self.opening_at = None
                self.page.window.present()
                self._update_home()
            elif time.monotonic() - self.opening_at > self.HANDOFF_WAIT_SECONDS:
                self.back()
                self.page.notice.set_text('CarPlay is still using the screen. Please try Home again.')
                return False
            return True
        except Exception:
            self._fail('Could not complete the Home display handoff')
            return False

    def _move_page(self, foreground):
        """Remap only our page so an occluded GTK surface commits its new layer.

        A mapped bottom surface can stop receiving frame callbacks while an
        opaque phone surface covers it. Changing layer properties and calling
        present() alone then leaves the layer change pending. Withdrawing and
        remapping this window gives GTK a fresh map/commit path; the phone and
        cluster surfaces, page widgets and configured monitor remain intact.
        """
        window, api = self.page.window, self.layer_shell
        window.set_visible(False)
        api.set_layer(window, api.Layer.OVERLAY if foreground else api.Layer.BOTTOM)
        api.set_keyboard_mode(window, api.KeyboardMode.ON_DEMAND if foreground else api.KeyboardMode.NONE)
        window.set_visible(True)

    def back(self):
        if self.closed or not self.available:
            return
        try:
            if self.foreground:
                self._move_page(False)
                # Confirm that our full overlay is withdrawn/lowered on this
                # connection before another process can recreate its main
                # window. This is a compositor roundtrip, not a video receipt.
                self.display.sync()
            self.handoff.release()
            self.foreground = False
            self.opening, self.opening_at = False, None
            if self.handoff_source:
                source = self.GLib.MainContext.default().find_source_by_id(self.handoff_source)
                if source is not None:
                    source.destroy()
                self.handoff_source = 0
            self._update_home()
        except Exception:
            self._fail('Could not lower the native phone page')

    def manager_lost(self):
        """Unknown camera state cannot retain a foreground page or Home button."""
        if self.closed:
            return
        self.camera_known, self.camera_blocked = False, True
        self.carplay_known, self.return_ready = False, False
        self.back()
        if self.page is not None and hasattr(self.page, 'status_lost'):
            self.page.status_lost()

    def camera_requested(self):
        """Explicit Open camera intent lowers immediately, before its RPC reply."""
        if self.closed:
            return
        self.retained_camera_intent, self.camera_blocked = True, True
        self.back()

    def update_status(self, reply, observed_at=None):
        if self.closed:
            return
        observed_at = time.monotonic() if observed_at is None else observed_at
        if not status_fresh(observed_at):
            self.manager_lost()
            return
        try:
            camera = camera_snapshot(reply.get('camera'))
        except ValueError:
            self.manager_lost()
            return
        try:
            carplay = carplay_snapshot(reply.get('carplay'))
            self.carplay_known, self.return_ready = True, carplay['returnReady']
            if self.return_ready and self.boot_splash is not None:
                self.boot_splash.close('phone-ready')
        except ValueError:
            self.carplay_known, self.return_ready = False, False
        phase = camera['phase']
        if camera['requested'] or phase in ('waiting', 'active', 'fault'):
            self.retained_camera_intent = True
        elif phase in ('idle', 'stopped'):
            self.retained_camera_intent = False
        self.camera_known = True
        self.camera_blocked = bool(camera['requested'] or phase in ('waiting', 'active', 'fault') or camera['error'] or
                                   (phase == 'unavailable' and (camera['configured'] or self.retained_camera_intent)))
        self.last_status = observed_at
        if self.camera_blocked or not self.carplay_known or not self.return_ready:
            self.back()
        else:
            self._update_home()  # Recovery never raises the page automatically.
            if self._phone_home_requested(reply.get('phone_home')):
                self.open()

    def _phone_home_requested(self, value):
        """True once per increase of the phone's car-icon count on one connection."""
        if (type(value) is not dict or set(value) != {'generation', 'connection', 'requests'} or
                type(value['generation']) is not int or type(value['requests']) is not int or
                value['requests'] < 0 or value['connection'] is not None and type(value['connection']) is not str):
            return False
        key = (value['generation'], value['connection'])
        previous = getattr(self, 'phone_home_seen', None)
        self.phone_home_seen = (key, value['requests'])
        return previous is not None and previous[0] == key and value['requests'] > previous[1]

    def _health_tick(self):
        if self.closed:
            return False
        self._outputs_changed()
        if self.closed:
            return False
        if not self._status_current():
            self.manager_lost()
        return True

    def close(self):
        """Stop sources and destroy both surfaces before the outer owner closes."""
        if self.retired:
            return True
        self.closed, self.available, self.foreground = True, False, False
        if self.boot_splash is not None:
            self.boot_splash.close()
        self.opening = False
        self.camera_known, self.camera_blocked = False, True
        errors = []
        if self.handoff_source:
            try:
                source = self.GLib.MainContext.default().find_source_by_id(self.handoff_source)
                if source is not None:
                    source.destroy()
                self.handoff_source = 0
            except Exception:
                errors.append('Could not stop native Home ownership polling')
        if self.health_source:
            try:
                source = self.GLib.MainContext.default().find_source_by_id(self.health_source)
                if source is not None:
                    source.destroy()
                self.health_source = 0
            except Exception:
                errors.append('Could not stop native Home health polling')
        remaining = []
        for obj, handler in self.connections:
            try:
                obj.disconnect(handler)
            except Exception:
                remaining.append((obj, handler))
                errors.append('Could not disconnect a native phone callback')
        self.connections = remaining
        if self.home_window is not None:
            try:
                self.home_button.set_sensitive(False)
            except Exception:
                errors.append('Could not disable native Home')
        surfaces = [surface for surface in (self.home_window, self.page.window if self.page is not None else None) if surface is not None]
        for surface in surfaces:
            if surface in self.destroyed_surfaces:
                continue
            try:
                surface.set_visible(False)
            except Exception:
                errors.append('Could not hide a native phone surface')
            try:
                surface.destroy()
                self.destroyed_surfaces.add(surface)
            except Exception:
                errors.append('Could not destroy a native phone surface')
        surfaces_retired = all(surface in self.destroyed_surfaces for surface in surfaces)
        if surfaces_retired and self.display is not None:
            try:
                self.display.sync()
                self.handoff.release()
            except Exception:
                # The outer owner retains the lease until it closes the GDK
                # connection. Never release over an ambiguously live overlay.
                errors.append('Could not confirm native Home withdrawal')
        self.retired = bool(not self.health_source and not self.handoff_source and not self.connections and surfaces_retired)
        if errors:
            self.cleanup_error = self.cleanup_error or '; '.join(dict.fromkeys(errors))[:256]
        if self.retired:
            self.page = None
            # GTK 4.18's Wayland monitor holds borrowed display/proxy state.
            # Let display.close() retire it while that connection is still
            # alive, rather than finalizing an externally held monitor later.
            self.monitor = self.monitors = self.display = None
        return self.retired
