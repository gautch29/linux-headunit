#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Native GTK phone/media settings page. No browser and no elevated privileges."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import stat
import struct
import time
import gi

gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, GLib, Gdk, Pango, Gio
from phone_glass import AdaptiveShell, GlassTabBar
from phone_home import Backdrop, CarView, ClusterFormats, DriveModeSettings, GreetingSettings, HomeView
from phone_glass import GlassButton
from phone_icons import icon, navigation_icon
from phone_page_style import STYLESHEET
from phone_page_presentation import carplay_snapshot, camera_snapshot, status_fresh, recovery_phone
from phone_page_sport import SportView, VIEWS, sport_snapshot


def request(path, value):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(45)
        connection.connect(str(path))
        connection.sendall(json.dumps(value).encode() + b'\n')
        with connection.makefile('rb') as stream:
            raw = stream.readline(131073)
        if len(raw) > 131072 or not raw.endswith(b'\n'):
            raise ValueError('Invalid manager reply')
        return json.loads(raw)


def label(text, style=None):
    item = Gtk.Label(label=text, xalign=0)
    item.set_ellipsize(Pango.EllipsizeMode.END)
    if style:
        item.add_css_class(style)
    return item


# Setting icons use the system tints: a white symbol on a coloured tile.
TINTS = {'#007aff': 'tint-blue', '#dc4768': 'tint-pink', '#7563bc': 'tint-indigo', '#b47822': 'tint-orange'}


def setting_icon(symbol, colour):
    tile = icon(symbol, 22, '#ffffff')
    tile.add_css_class('choice-icon')
    tile.add_css_class(TINTS.get(colour, 'tint-gray'))
    return tile


def list_group():
    """A one-column grouped list; rows join into one rounded surface."""
    flow = Gtk.FlowBox()
    flow.set_selection_mode(Gtk.SelectionMode.NONE)
    flow.set_min_children_per_line(1)
    flow.set_max_children_per_line(1)
    flow.set_row_spacing(0)
    flow.add_css_class('list-group')
    return flow


def list_row(button):
    button.add_css_class('list-row')
    child = button.get_child()
    if isinstance(child, Gtk.Label):
        child.set_xalign(0)
    return button


def media_time(milliseconds):
    """Format an observed media position; absent values never become zero."""
    if type(milliseconds) is not int or not 0 <= milliseconds <= 0xffffffff:
        return '—:—'
    seconds = milliseconds // 1000
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    return f'{hours}:{minutes:02d}:{seconds:02d}' if hours else f'{minutes}:{seconds:02d}'


def pairing_snapshot(value):
    """Reject incomplete/malformed optional pairing data before enabling actions."""
    fields = {'supported', 'phase', 'session', 'expiresInSeconds', 'adapterName', 'prompt', 'pairedAddress', 'error'}
    if type(value) is not dict or set(value) != fields or type(value['supported']) is not bool:
        raise ValueError('Pairing information unavailable')
    if value['phase'] not in ('idle', 'starting', 'discoverable', 'confirmation', 'paired', 'stopping', 'error'):
        raise ValueError('Invalid pairing phase')
    identifier = lambda text, empty=False: type(text) is str and ((empty and text == '') or re.fullmatch(r'[0-9a-fA-F]{32}', text))
    address = lambda text, empty=False: type(text) is str and ((empty and text == '') or re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', text))
    text = lambda field, limit=256: type(field) is str and len(field) <= limit and not any(ord(c) < 32 or ord(c) == 127 for c in field)
    if not identifier(value['session'], True) or not address(value['pairedAddress'], True) or \
            type(value['expiresInSeconds']) is not int or not 0 <= value['expiresInSeconds'] <= 120 or \
            not text(value['adapterName']) or not text(value['error'], 300):
        raise ValueError('Invalid pairing information')
    prompt = value['prompt']
    if prompt is not None:
        if type(prompt) is not dict or set(prompt) != {'id', 'deviceName', 'address', 'passkey'} or \
                not identifier(prompt['id']) or not text(prompt['deviceName']) or not address(prompt['address']) or \
                type(prompt['passkey']) is not str or (prompt['passkey'] != '' and not re.fullmatch(r'[0-9]{6}', prompt['passkey'])):
            raise ValueError('Invalid pairing confirmation')
    return {**value, 'prompt': dict(prompt) if prompt is not None else None}


class PhonePage(Gtk.Application):
    def __init__(self, path, presentation=None, initial_view='home'):
        if initial_view not in VIEWS:
            raise ValueError('Unknown native page')
        # The outer resident wrapper owns its process lock. It must never
        # remote-activate an unrelated legacy page on an arbitrary output.
        flags = Gio.ApplicationFlags.NON_UNIQUE if presentation is not None else Gio.ApplicationFlags.DEFAULT_FLAGS
        super().__init__(application_id='org.headunit.Phones', flags=flags)
        self.path = path
        self.initial_view = initial_view
        self.presentation = presentation
        self.closing = False
        self.ui_retired = False
        self.cleanup_error = ''
        self.pool_stopped = False
        self.manual_window_retired = False
        self.poll_source = 0
        self.health_source = 0
        self.startup_error = ''
        self.pool = ThreadPoolExecutor(max_workers=1)
        self.pending_action = None
        self.shortcut_launch = None
        self.rows_key = None
        self.artwork_cache = None
        self.status = {}
        self.status_available = False
        self.status_requested_at = None
        self.pairing = None
        self.pairing_decision = None
        self.pairing_fresh = False
        self.connect('activate', self.activate)
        self.connect('shutdown', lambda _: self.close_ui())

    def close_ui(self):
        """Retire surfaces and callbacks before the outer page owner releases."""
        if self.ui_retired:
            return True
        self.closing = True
        self.shortcut_launch = None
        errors = []
        for source_name in ('poll_source', 'health_source'):
            number = getattr(self, source_name)
            if not number:
                continue
            try:
                source = GLib.MainContext.default().find_source_by_id(number)
                if source is not None:
                    source.destroy()
                setattr(self, source_name, 0)
            except Exception:
                errors.append('Could not stop phone-page polling')
        self.pending_action = None
        if self.presentation is not None:
            try:
                self.presentation.close()
                if self.presentation.cleanup_error:
                    errors.append(self.presentation.cleanup_error)
            except Exception:
                errors.append('Could not retire native phone surfaces')
            surfaces_retired = self.presentation.retired
        else:
            if hasattr(self, 'window') and not self.manual_window_retired:
                try:
                    self.window.set_visible(False)
                except Exception:
                    errors.append('Could not hide the phone page')
                try:
                    self.window.destroy()
                    self.manual_window_retired = True
                except Exception:
                    errors.append('Could not destroy the phone page')
            surfaces_retired = not hasattr(self, 'window') or self.manual_window_retired
        if not self.pool_stopped:
            try:
                # An in-flight exchange may finish on the socket worker;
                # closing rejects its GTK completion. Cancel queued work
                # without blocking surface retirement on that exchange.
                self.pool.shutdown(wait=False, cancel_futures=True)
                self.pool_stopped = True
            except Exception:
                errors.append('Could not stop queued phone-page requests')
        self.ui_retired = bool(surfaces_retired and not self.poll_source and not self.health_source and self.pool_stopped)
        if errors:
            self.cleanup_error = self.cleanup_error or '; '.join(errors)[:256]
            self.startup_error = self.startup_error or self.cleanup_error
        return self.ui_retired

    def fail_resident(self, message):
        self.startup_error = message
        self.close_ui()
        self.quit()

    def back(self):
        if not self.can_return():
            return
        if self.presentation is not None:
            self.presentation.back()
        else:
            self.window.close()

    def status_current(self):
        return bool(not self.closing and self.status_available and status_fresh(self.status_requested_at))

    def can_return(self):
        if self.operation_pending() or not self.status_current():
            return False
        if self.presentation is not None:
            return self.presentation.can_return()
        try:
            carplay = carplay_snapshot(self.status.get('carplay'))
        except ValueError:
            return False
        return bool(carplay['returnReady'] and self.camera_clear())

    def operation_pending(self):
        return self.pending_action not in (None, 'status') or self.shortcut_launch is not None

    @property
    def pending(self):
        """One outstanding request; taps never create an operation queue."""
        return self.pending_action is not None

    def update_navigation(self):
        if hasattr(self, 'back_button'):
            permitted = self.can_return()
            self.back_button.set_sensitive(permitted)
            self.back_button.set_visible(permitted)

    def action_sensitivity(self):
        safe = self.status_current() and not self.pending
        if hasattr(self, 'listbox'):
            self.listbox.set_sensitive(safe and self.receiver_change_available())
        for name in ('media_controls', 'audio_controls', 'native_cluster_pages', 'ultra_styles', 'display_responses', 'startup_warning_choices'):
            if hasattr(self, name):
                getattr(self, name).set_sensitive(safe)
        for name in ('drive_modes', 'greeting'):
            if hasattr(self, name):
                getattr(self, name).widget.set_sensitive(safe)
        if hasattr(self, 'camera_controls'):
            camera = self.current_camera()
            self.camera_controls.set_sensitive(safe and camera is not None and camera['configured'])
        if hasattr(self, 'retry_button'):
            self.retry_button.set_visible(not self.status_current())
            self.retry_button.set_sensitive(not self.closing and not self.pending)
        if hasattr(self, 'sport'):
            self.sport.sensitivity()
        if hasattr(self, 'view_buttons'):
            for button in self.view_buttons.values():
                button.set_sensitive(self.status_current() and not self.operation_pending())
        if hasattr(self, 'add_phone'):
            self.pairing_sensitivity()
        if self.presentation is not None:
            # A healthy read-only refresh must not unmap the navigation button
            # or cancel a touch every two seconds. Only mutations suspend Home
            # and Back; manager freshness still expires during a stalled read.
            self.presentation.set_pending(self.operation_pending())
        self.update_navigation()
        if hasattr(self, 'home'):
            self.home.set_shortcuts(safe and self.shortcut_publisher is not None and self.can_return())

    def status_lost(self):
        self.shortcut_launch = None
        self.status_available = False
        if hasattr(self, 'sport'):
            self.sport.update(None, current=False)
        self.rows_key = None
        for row in getattr(self, 'phone_widgets', {}).values():
            row['status'].set_text('Connection status unavailable')
            row['status'].remove_css_class('connected')
        if hasattr(self, 'song'):
            self.render_media({})
            self.song.set_text('Media information unavailable')
            self.artist.set_text('Waiting for the connection manager…')
            self.media_note.set_visible(False)
            self.camera_status.set_text('Camera information unavailable')
            self.camera_request_status.set_text('Manual request · Unavailable')
            self.camera_note.set_text('Waiting for the connection manager…')
            self.camera_note.set_visible(True)
            self.home.lost()
            self.backdrop.clear()
            self.vehicle_content_box.set_visible(False)
            self.ultra_styles.set_visible(False)
            self.set_cluster_note('Cluster information unavailable. Waiting for the connection manager…')
            self.cluster_formats.unmark()
            self.car.lost()
        self.action_sensitivity()

    def current_camera(self):
        try:
            return camera_snapshot(self.status.get('camera'))
        except ValueError:
            return None

    def camera_clear(self):
        camera = self.current_camera()
        return bool(camera is not None and not camera['requested'] and not camera['error'] and
                    camera['phase'] not in ('waiting', 'active', 'fault') and
                    not (camera['configured'] and camera['phase'] == 'unavailable'))

    def health_tick(self):
        if self.closing:
            return False
        self.home.tick()
        if self.status_available and not self.status_current():
            self.status_lost()
            self.set_notice('Waiting for the connection manager…', connection=True)
            self.invalidate_pairing('Connection information expired. Refresh the connection manager to continue.')
            if self.presentation is not None:
                self.presentation.manager_lost()
        if hasattr(self, 'sport') and self.status_current():
            self.render_sport()
        if self.shortcut_launch is not None and self.shortcut_launch['phase'] == 'waiting':
            if time.monotonic() >= self.shortcut_launch['deadline']:
                self.shortcut_launch = None
                self.set_notice('CarPlay did not become ready. Open the app again.')
                self.action_sensitivity()
            elif not self.pending:
                self.action({'action': 'status'})
        return True

    def action(self, value):
        if self.closing or value.get('action') not in ('status', 'connect', 'disconnect', 'forget', 'media', 'audio',
                                                      'cluster_page', 'cluster_layout', 'cluster_input', 'cluster_content', 'camera', 'display_response', 'startup_warning_lights', 'pairing_start',
                                                      'pairing_cancel', 'pairing_confirm', 'phone_app',
                                                      'drive_mode', 'greeting_sequence', 'greeting_preview', 'trip_reset', 'sport_session'):
            return
        # A tap never queues a second operation with stale captured state.
        # Refresh is the only action allowed without a current manager reply.
        if self.pending or (value['action'] != 'status' and not self.status_current()):
            self.rows_key = None
            return
        if value['action'] in ('connect', 'disconnect', 'forget') and not self.receiver_change_available():
            return
        if value['action'] in ('trip_reset', 'sport_session'):
            if set(value) != {'action'} or not self.current_sport()['actionable']:
                return
        if value['action'] == 'camera':
            camera = self.current_camera()
            if camera is None or not camera['configured']:
                return
        if self.presentation is not None and value.get('action') == 'camera' and value.get('enabled') is True:
            self.presentation.camera_requested()
        # Capture the source affected by this request before its async reply.
        # Editing another phone's preferences leaves the current session alone.
        session_change = value['action'] in ('connect', 'disconnect') or (
            value['action'] == 'forget' and value.get('phone') == self.status.get('active'))
        self.pending_action = value['action']
        self.action_sensitivity()
        if value['action'] not in ('status', 'phone_app'):
            self.set_notice('Software statistics request pending…' if value['action'] in ('trip_reset', 'sport_session') else 'Working…')
        # The manager does not date its reply. Dispatch is the conservative
        # earliest observation bound; neither IPC nor GTK completion renews it.
        observed_at = time.monotonic()
        future = self.pool.submit(request, self.path, value)
        def done(result):
            if self.closing:
                return
            try:
                reply = result.result()
            except Exception as error:
                reply = {'ok': False, 'error': str(error)}
            if not self.closing:
                GLib.idle_add(self.received, value['action'], reply, session_change, observed_at)
        future.add_done_callback(done)

    def received(self, action, reply, session_change=False, observed_at=None):
        if self.closing:
            return False
        if type(reply) is not dict:
            reply = {'ok': False, 'error': 'Invalid connection manager reply'}
        elif action == 'status' and reply.get('ok') is True and not status_fresh(observed_at):
            reply = {'ok': False, 'error': 'Connection information expired. Refresh the connection manager to continue.'}
        self.pending_action = None
        manual_return = (action == 'phone_app' and self.shortcut_launch is not None and
                         self.shortcut_launch['phase'] == 'sent' and self.presentation is None)
        if action == 'phone_app':
            self.shortcut_launch = None
        if reply.get('ok') is not True:
            self.status_lost()
            if self.presentation is not None:
                self.presentation.manager_lost()
            self.rows_key = None
            # A failed refresh clears itself once the manager answers again;
            # a failed action keeps its reason on screen.
            self.set_notice(reply.get('error', 'Connection manager unavailable'), connection=action == 'status')
            self.invalidate_pairing('Pairing controls unavailable. Refresh the connection manager to continue.')
            self.camera_status.set_text('Camera connection unavailable')
        elif action == 'status':
            self.status = reply
            self.status_available = True
            self.status_requested_at = observed_at
            if self.presentation is not None:
                self.presentation.update_status(reply, observed_at)
                if self.closing:
                    return False
            self.render(reply)
            if self.notice.get_text() == 'Software statistics request accepted. Refreshing observed journey/session…':
                self.set_notice('Software statistics request accepted. Observed software data refreshed.')
        else:
            self.rows_key = None
            if action in ('trip_reset', 'sport_session'):
                # An action reply is not a new acquired status snapshot.
                self.set_notice('Software statistics request accepted. Refreshing observed journey/session…')
            else:
                self.set_notice('' if action == 'phone_app' else reply.get('message', 'Done'))
            if session_change:
                # A successful session mutation retires the old ready reply.
                # Release Home now so a replacement renderer can bootstrap;
                # only its following fresh status may restore navigation.
                self.status_lost()
                if self.presentation is not None:
                    self.presentation.manager_lost()
        self.action_sensitivity()
        if action == 'status':
            self.advance_shortcut()
        elif manual_return and reply.get('ok') is True:
            self.back()
        if action != 'status':
            self.action({'action': 'status'})
        return False

    def refresh(self):
        if self.closing:
            return False
        if not self.pending:
            self.action({'action': 'status'})
        return True

    def activate(self, _):
        if self.closing:
            return
        if hasattr(self, 'window'):
            if self.presentation is not None:
                self.presentation.reactivate()
            else:
                self.window.present()
            return
        css = Gtk.CssProvider()
        css.load_from_data(STYLESHEET)
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self.window = Gtk.ApplicationWindow(application=self, title='Linux Head Unit')
        self.window.set_default_size(1280, 720)
        self.window.set_decorated(False)
        # Glass surfaces float over a colour field taken from the artwork.
        self.backdrop = Backdrop()
        layers = Gtk.Overlay()
        layers.set_child(self.backdrop.widget)
        self.window.set_child(layers)
        # The tab bar is liquid glass over the backdrop layer.
        sidebar = GlassTabBar(self.backdrop)
        sidebar.set_margin_start(20)
        sidebar.set_margin_end(20)
        sidebar.set_margin_top(20)
        sidebar.set_margin_bottom(20)
        self.tab_bar = sidebar
        self.view_buttons = {}
        previous = None
        for name, title, symbol in [('home', 'Home', 'home'), ('phones', 'Phones', 'phone'), ('music', 'Music', 'music'), ('cluster', 'Cluster', 'cluster'), ('car', 'Car', 'vehicle'), ('camera', 'Camera', 'camera'), ('settings', 'Settings', 'settings')]:
            button = Gtk.ToggleButton(); button.add_css_class('nav')
            if previous: button.set_group(previous)
            previous = button
            content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
            content.set_valign(Gtk.Align.CENTER)
            image = navigation_icon(symbol)
            content.append(image)
            content.append(Gtk.Label(label=title))
            button.set_child(content)
            button.update_property([Gtk.AccessibleProperty.LABEL], [title])
            button.set_visible(name not in ('car', 'camera', 'settings'))
            button.connect('toggled', lambda b, v=name: b.get_active() and self.show_view(v))
            button.connect('toggled', lambda b: b.get_active() and self.tab_bar.select(b))
            button.connect('toggled', lambda b, s=symbol, i=image:
                           i.set_from_paintable(navigation_icon(s, b.get_active()).get_paintable()))
            sidebar.append(button); self.view_buttons[name] = button
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=24)
        root.set_hexpand(True)
        root.set_vexpand(True)
        # CarPlay placement: left of the content in landscape, below it in portrait.
        shell = AdaptiveShell(sidebar, root)
        layers.add_overlay(shell)
        layers.set_measure_overlay(shell, True)
        root.set_margin_top(40)
        root.set_margin_bottom(20)
        header = Gtk.Box(spacing=20)
        self.page_header = header
        titles = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=5); titles.set_hexpand(True)
        self.page_title = label('Phones', 'title'); titles.append(self.page_title)
        self.page_subtitle = label('Connect your iPhone.', 'muted'); self.page_subtitle.set_visible(False); titles.append(self.page_subtitle); header.append(titles)
        back = GlassButton(self.backdrop, label='Back to CarPlay'); back.add_css_class('quiet'); back.set_size_request(-1, 52); back.set_valign(Gtk.Align.CENTER)
        self.back_button = back
        back.set_visible(False); back.set_sensitive(False)
        back.connect('clicked', lambda _: self.back()); header.append(back); root.append(header)
        self.pages = Gtk.Stack(); self.pages.set_vexpand(True)
        # Hidden settings pages must not impose their height on the fixed
        # touchscreen. Music keeps its own vertical scroll for extra controls.
        self.pages.set_vhomogeneous(False)
        self.pages.set_transition_type(Gtk.StackTransitionType.CROSSFADE); self.pages.set_transition_duration(150); root.append(self.pages)
        self.home = HomeView(self)
        self.pages.add_named(self.home.widget, 'home')
        phones = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16); self.pages.add_named(phones, 'phones')
        phones_header = Gtk.Box(spacing=16)
        phones_header.set_visible(False)
        phones.append(phones_header)
        paired_title = label('PAIRED PHONES', 'eyebrow'); paired_title.set_hexpand(True); paired_title.set_visible(False); phones_header.append(paired_title)
        self.add_phone = Gtk.Button(label='Add phone'); self.add_phone.add_css_class('suggested-action')
        self.add_phone.set_visible(False); self.add_phone.set_sensitive(False)
        self.add_phone.connect('clicked', lambda _: self.pairing_action('start'))
        self.add_phone.set_valign(Gtk.Align.CENTER)
        header.append(self.add_phone)
        header.reorder_child_after(back, self.add_phone)
        scroll = Gtk.ScrolledWindow(); scroll.set_hexpand(True); scroll.set_vexpand(True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        phone_content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16); scroll.set_child(phone_content)
        # CarPlay app launchers live on the Home dashboard (Ultra sessions only).
        self.app_shortcuts = self.home.apps
        self.shortcut_publisher = None
        self.pairing_card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16); self.pairing_card.add_css_class('card')
        self.pairing_card.set_visible(False); phone_content.append(self.pairing_card)
        pairing_header = Gtk.Box(spacing=16); self.pairing_card.append(pairing_header)
        self.pairing_title = label('Add your phone', 'section'); self.pairing_title.set_hexpand(True); pairing_header.append(self.pairing_title)
        self.pairing_time = label('', 'muted'); self.pairing_time.add_css_class('small'); pairing_header.append(self.pairing_time)
        self.pairing_adapter = label('', 'section'); self.pairing_card.append(self.pairing_adapter)
        self.pairing_note = label('', 'muted'); self.pairing_note.set_wrap(True)
        self.pairing_note.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.pairing_note.set_ellipsize(Pango.EllipsizeMode.NONE); self.pairing_card.append(self.pairing_note)
        self.pairing_warning = label('', 'pairing-warning'); self.pairing_warning.set_wrap(True)
        self.pairing_warning.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.pairing_warning.set_ellipsize(Pango.EllipsizeMode.NONE); self.pairing_warning.set_visible(False)
        self.pairing_card.append(self.pairing_warning)
        self.pairing_prompt = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12); self.pairing_card.append(self.pairing_prompt)
        self.pairing_device = label('', 'section'); self.pairing_prompt.append(self.pairing_device)
        self.pairing_code = label('', 'pairing-code'); self.pairing_prompt.append(self.pairing_code)
        controls = Gtk.Box(spacing=12); self.pairing_card.append(controls)
        self.pairing_buttons = {}
        for key, title, css_class in (('pair', 'Pair', 'suggested-action'), ('reject', 'Reject', 'destructive-action'), ('cancel', 'Cancel', 'quiet')):
            button = Gtk.Button(label=title); button.add_css_class(css_class)
            button.connect('clicked', lambda _, action=key: self.pairing_action(action))
            controls.append(button); self.pairing_buttons[key] = button
        self.listbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.listbox.add_css_class('grouped')
        phone_content.append(self.listbox); phones.append(scroll)
        hint = label('Your iPhone chooses CarPlay Ultra or Standard CarPlay.', 'muted')
        hint.set_wrap(True); hint.set_ellipsize(Pango.EllipsizeMode.NONE); hint.add_css_class('small'); phones.append(hint)
        music = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16); self.pages.add_named(music, 'music')
        # The page title supplies the hierarchy; avoid a redundant section label.
        media_scroll = Gtk.ScrolledWindow(); media_scroll.set_vexpand(True)
        media_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC); music.append(media_scroll)
        self.media_box = Gtk.Box(spacing=40); self.media_box.add_css_class('card'); self.media_box.add_css_class('music-card')
        media_scroll.set_child(self.media_box)
        self.cover = Gtk.Picture(); self.cover.set_size_request(320, 320); self.cover.set_can_shrink(True); self.cover.set_content_fit(Gtk.ContentFit.CONTAIN)
        self.cover_stack = Gtk.Stack(); self.cover_stack.set_halign(Gtk.Align.CENTER); self.cover_stack.set_valign(Gtk.Align.CENTER)
        self.cover_stack.add_css_class('artwork'); self.cover_stack.set_overflow(Gtk.Overflow.HIDDEN)
        self.cover_stack.add_named(self.cover, 'art')
        empty_art = icon('music', 96, color='#9a9aa3'); empty_art.set_size_request(320, 320); self.cover_stack.add_named(empty_art, 'empty')
        self.media_box.append(self.cover_stack)
        player = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=24); player.set_hexpand(True); player.set_valign(Gtk.Align.CENTER); self.media_box.append(player)
        details = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8); player.append(details)
        self.song = label('Nothing playing', 'track'); details.append(self.song)
        self.song.set_wrap(True); self.song.set_wrap_mode(Pango.WrapMode.WORD_CHAR); self.song.set_lines(2)
        self.artist = label('Connect a phone to see your music.', 'muted'); details.append(self.artist)
        self.album = label('', 'muted'); self.album.add_css_class('small'); details.append(self.album)
        self.timeline = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8); player.append(self.timeline)
        self.progress = Gtk.ProgressBar(); self.timeline.append(self.progress)
        times = Gtk.Box(spacing=12); self.timeline.append(times)
        self.elapsed = label('—:—', 'media-time'); self.elapsed.set_hexpand(True); times.append(self.elapsed)
        self.duration = label('—:—', 'media-time'); times.append(self.duration)
        self.elapsed.update_property([Gtk.AccessibleProperty.DESCRIPTION], ['Elapsed time'])
        self.duration.update_property([Gtk.AccessibleProperty.DESCRIPTION], ['Track duration'])
        self.media_controls = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18); player.append(self.media_controls)
        self.transport = Gtk.Box(spacing=12); self.transport.add_css_class('transport'); self.media_controls.append(self.transport)
        self.buttons = {}
        for command in ('previous', 'play', 'pause', 'next'):
            button = Gtk.Button(); button.set_child(icon(command, 44 if command in ('play', 'pause') else 34, color='#ffffff')); button.set_tooltip_text(command.capitalize())
            if command in ('play', 'pause'): button.add_css_class('primary-transport')
            button.update_property([Gtk.AccessibleProperty.LABEL], [command.capitalize()])
            button.connect('clicked', lambda _, c=command: self.media_action(c))
            self.transport.append(button); self.buttons[command] = button
        row = Gtk.Box(spacing=12); self.bluetooth_volume = row; self.media_controls.append(row)
        bluetooth_volume = label('Bluetooth volume', 'muted'); bluetooth_volume.set_hexpand(True); row.append(bluetooth_volume)
        for command, title in [('volume_down', 'Lower Bluetooth volume'), ('volume_up', 'Raise Bluetooth volume')]:
            button = Gtk.Button(); button.set_child(icon(command, color='#f5f5f7')); button.add_css_class('quiet'); button.set_tooltip_text(title)
            button.update_property([Gtk.AccessibleProperty.LABEL], [title])
            button.connect('clicked', lambda _, c=command: self.media_action(c)); row.append(button); self.buttons[command] = button
        self.audio_controls = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8); player.append(self.audio_controls)
        self.audio_controls.append(label('Head-unit volume', 'muted'))
        row = Gtk.Box(spacing=10); self.audio_route_controls = row; self.audio_controls.append(row)
        self.audio_value = label('Unavailable', 'section'); self.audio_value.set_hexpand(True); row.append(self.audio_value)
        self.audio_buttons = {}
        for name, title, step in [('down', '−', -5), ('up', '+', 5)]:
            button = Gtk.Button(label=title)
            button.set_tooltip_text('Lower head-unit volume' if step < 0 else 'Raise head-unit volume')
            button.connect('clicked', lambda _, change=step: self.audio_action(change))
            row.append(button); self.audio_buttons[name] = button
        mute = Gtk.Button(label='Mute'); mute.connect('clicked', lambda _: self.audio_action(toggle=True))
        row.append(mute); self.audio_buttons['mute'] = mute
        self.audio_note = label('Waiting for a CarPlay audio route.', 'muted'); self.audio_note.add_css_class('small')
        self.audio_note.set_wrap(True); self.audio_controls.append(self.audio_note)
        self.media_note = label('Waiting for your phone', 'muted'); self.media_note.add_css_class('small')
        self.media_note.set_wrap(True); self.media_note.set_ellipsize(Pango.EllipsizeMode.NONE); player.append(self.media_note)
        cluster = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18); self.pages.add_named(cluster, 'cluster')
        self.cluster_eyebrow = label('CENTER VIEW', 'eyebrow'); cluster.append(self.cluster_eyebrow)
        self.cluster_note = label('', 'muted'); self.cluster_note.set_wrap(True); self.cluster_note.set_visible(False)
        cluster.append(self.cluster_note)
        self.cluster_scroll = Gtk.ScrolledWindow(); self.cluster_scroll.set_vexpand(True)
        self.cluster_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        cluster.append(self.cluster_scroll)
        cluster_content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.cluster_scroll.set_child(cluster_content)
        self.cluster_formats = ClusterFormats(self, list_group, list_row)
        self.native_cluster_pages = self.cluster_formats.widget
        self.cluster_buttons = self.cluster_formats.buttons
        self.cluster_selection_marks = self.cluster_formats.marks
        cluster_content.append(self.native_cluster_pages)
        self.cluster_options = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.option_catalog = None
        self.option_buttons = []
        self.ultra_styles = Gtk.FlowBox(); self.ultra_styles.set_selection_mode(Gtk.SelectionMode.NONE)
        self.ultra_styles.set_min_children_per_line(1); self.ultra_styles.set_max_children_per_line(1)
        self.ultra_styles.add_css_class('list-group')
        self.ultra_styles.set_homogeneous(True)
        self.ultra_styles.set_row_spacing(0)
        cluster_content.append(self.ultra_styles)
        self.cluster_navigation = Gtk.Box(spacing=12)
        self.cluster_navigation_buttons = []
        cluster_content.append(self.cluster_navigation)
        cluster_content.append(self.cluster_options)
        self.vehicle_content_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        cluster_content.append(self.vehicle_content_box)
        self.vehicle_content_catalog = None
        self.vehicle_content_buttons = []
        self.style_buttons = {}; self.style_catalog = None
        car_scroll = Gtk.ScrolledWindow(); car_scroll.set_vexpand(True)
        car_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.pages.add_named(car_scroll, 'car')
        self.car = CarView(list_group, list_row)
        car_scroll.set_child(self.car.widget)
        settings_scroll = Gtk.ScrolledWindow(); settings_scroll.set_vexpand(True)
        settings_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.pages.add_named(settings_scroll, 'settings')
        settings = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18); settings_scroll.set_child(settings)
        settings.set_margin_bottom(24)
        self.drive_modes = DriveModeSettings(self)
        settings.append(self.drive_modes.widget)
        self.greeting = GreetingSettings(self)
        settings.append(self.greeting.widget)
        self.sport = SportView(self, Gtk, Pango, label, list_group)
        self.pages.add_named(self.sport.widget, 'sport')
        self.sport_open = Gtk.Button()
        self.sport_open.set_name('sport-open')
        self.sport_open.add_css_class('cluster-choice')
        sport_tile = Gtk.Box(spacing=14)
        sport_tile.append(setting_icon('trip', '#b47822'))
        sport_tile.append(label('Trip, Sport & History', 'section'))
        self.sport_open.set_child(sport_tile)
        self.sport_open.update_property([Gtk.AccessibleProperty.LABEL], ['Trip, Sport & History'])
        self.sport_open.connect('clicked', lambda _: self.show_view('sport'))
        cluster_content.append(self.sport_open)
        self.view_buttons['cluster'].connect('clicked', lambda _:
            self.pages.get_visible_child_name() == 'sport' and self.show_view('cluster'))
        self.display_response_heading = label('DISPLAY RESPONSE', 'eyebrow'); settings.append(self.display_response_heading)
        self.display_response_note = label('', 'muted'); self.display_response_note.set_wrap(True)
        self.display_response_note.set_visible(False); settings.append(self.display_response_note)
        self.display_responses = Gtk.FlowBox(); self.display_responses.set_selection_mode(Gtk.SelectionMode.NONE)
        self.display_responses.set_min_children_per_line(3); self.display_responses.set_max_children_per_line(3)
        self.display_responses.set_homogeneous(True)
        self.display_responses.set_row_spacing(16); self.display_responses.set_column_spacing(16)
        settings.append(self.display_responses)
        self.display_response_buttons = {}
        self.display_response_marks = {}
        for mode, title, detail, color in (
                ('smooth', 'Smooth', 'Every frame on time. Touch feels slower.', '#007aff'),
                ('balanced', 'Balanced', 'Quicker touch with steady motion.', '#7563bc'),
                ('fast', 'Fast', 'Quickest touch. May skip a few frames.', '#dc4768')):
            button = Gtk.Button(); button.add_css_class('cluster-choice'); button.set_hexpand(True)
            row = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12); button.set_child(row)
            heading = Gtk.Box(spacing=14); heading.set_size_request(-1, 44); row.append(heading)
            badge = setting_icon(mode, color)
            heading.append(badge)
            title_label = label(title, 'section'); title_label.set_hexpand(True); title_label.set_valign(Gtk.Align.CENTER)
            heading.append(title_label)
            mark = Gtk.Label(label='✓'); mark.add_css_class('selection-mark'); mark.set_visible(False); heading.append(mark)
            self.display_response_marks[mode] = mark
            description = label(detail, 'muted'); description.set_wrap(True); description.set_lines(2)
            description.set_max_width_chars(24); row.append(description)
            button.update_property([Gtk.AccessibleProperty.LABEL, Gtk.AccessibleProperty.DESCRIPTION], [title, detail])
            button.connect('clicked', lambda _, value=mode: self.action({'action': 'display_response', 'mode': value}))
            self.display_responses.append(button); self.display_response_buttons[mode] = button
        self.display_response_hint = label('Applies to CarPlay Ultra right away.', 'muted')
        self.display_response_hint.set_wrap(True); self.display_response_hint.add_css_class('small'); settings.append(self.display_response_hint)
        self.startup_warning_heading = label('Warning lights during startup', 'section')
        settings.append(self.startup_warning_heading)
        self.startup_warning_note = label('Applies to startup and the brand × Apple animation. Driving lights always remain visible.', 'muted')
        self.startup_warning_note.set_wrap(True); settings.append(self.startup_warning_note)
        self.startup_warning_choices = Gtk.Box(spacing=0, homogeneous=True)
        self.startup_warning_choices.add_css_class('segmented')
        self.startup_warning_choices.set_halign(Gtk.Align.START)
        self.startup_warning_buttons = {}
        for hide, title in ((True, 'Hide during startup'), (False, 'Show during startup')):
            button = Gtk.Button(label=title); button.add_css_class('segment'); button.set_size_request(240, -1)
            button.connect('clicked', lambda _, value=hide: self.action({'action': 'startup_warning_lights', 'hide': value}))
            self.startup_warning_choices.append(button); self.startup_warning_buttons[hide] = button
        settings.append(self.startup_warning_choices)
        camera = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18); self.pages.add_named(camera, 'camera')
        # Camera state is the primary content heading.
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=24); card.add_css_class('card'); camera.append(card)
        self.camera_status = label('Camera controls unavailable', 'section'); card.append(self.camera_status)
        self.camera_request_status = label('Manual request · Unavailable', 'muted'); card.append(self.camera_request_status)
        self.camera_note = label('Set up a camera to use this view.', 'muted')
        self.camera_note.set_wrap(True); self.camera_note.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self.camera_note.set_ellipsize(Pango.EllipsizeMode.NONE); card.append(self.camera_note)
        self.camera_controls = Gtk.Box(spacing=16); card.append(self.camera_controls)
        self.camera_buttons = {}
        for enabled, title in ((True, 'Open camera'), (False, 'Close camera')):
            button = Gtk.Button(label=title); button.set_size_request(180, 64)
            button.connect('clicked', lambda _, value=enabled: self.action({'action': 'camera', 'enabled': value}))
            self.camera_controls.append(button); self.camera_buttons[enabled] = button
        self.camera_controls.set_sensitive(False)
        hint = label('If the camera is unavailable, check its connection and the current CarPlay session. Vehicle activation can keep the view open after closing a manual request.', 'muted')
        hint.set_wrap(True); hint.set_ellipsize(Pango.EllipsizeMode.NONE); hint.add_css_class('small'); camera.append(hint)
        footer = Gtk.Box(spacing=16)
        self.notice = label('Connecting to phone manager…', 'muted')
        self.notice.add_css_class('small')
        self.notice.set_hexpand(True)
        footer.append(self.notice)
        self.retry_button = Gtk.Button(label='Retry connection')
        self.retry_button.set_tooltip_text('Refresh connection information')
        self.retry_button.connect('clicked', lambda _: self.refresh())
        footer.append(self.retry_button)
        root.append(footer)
        self.view_buttons['home'].set_active(True)
        if self.initial_view != 'home':
            self.show_view(self.initial_view)
        if self.presentation is not None:
            try:
                self.presentation.attach(self, Gtk, Gdk)
            except Exception as error:
                self.fail_resident(str(error))
                return
        else:
            self.window.present()
        self.action_sensitivity()
        self.refresh(); self.poll_source = GLib.timeout_add_seconds(2, self.refresh)
        self.health_source = GLib.timeout_add(250, self.health_tick)

    def show_view(self, name):
        self.shortcut_launch = None
        if name not in VIEWS:
            return
        if name == 'sport':
            self.view_buttons['cluster'].set_active(True)
        elif not self.view_buttons[name].get_active():
            self.view_buttons[name].set_active(True)
            return
        if name in ('car', 'camera', 'settings') and not self.view_buttons[name].get_visible():
            self.view_buttons['phones'].set_active(True)
            return
        self.pages.set_visible_child_name(name)
        self.page_header.set_visible(name != 'home')
        # Home and Music sit on a wallpaper; glass needs something to bend.
        self.backdrop.widget.set_visible(name in ('home', 'music'))
        # The Home clock is the heading; other views keep a large title.
        self.page_title.set_visible(name != 'home')
        self.add_phone.set_visible(name == 'phones' and bool(self.pairing and self.pairing['supported']))
        self.page_title.set_text({'home': 'Home', 'phones': 'Phones', 'music': 'Music', 'cluster': 'Cluster', 'car': 'Car', 'camera': 'Camera',
                                  'settings': 'Settings', 'sport': 'Trip, Sport & History'}[name])
        self.page_subtitle.set_text({'home': '', 'phones': 'Connect your iPhone.', 'music': 'The soundtrack to your drive.',
                                     'cluster': 'Choose what the instrument cluster shows.', 'car': 'Readings from your car.',
                                     'camera': 'A view from your car.',
                                     'settings': 'Tune how the head unit feels.', 'sport': 'Acquired software estimates.'}[name])

    def sport_status_age_ns(self):
        if self.status_requested_at is None:
            return 0
        return max(0, int((time.monotonic() - self.status_requested_at) * 1_000_000_000))

    def current_sport(self):
        return sport_snapshot(self.status.get('sport'), status_current=self.status_current(),
                              status_age_ns=self.sport_status_age_ns())

    def render_sport(self):
        self.sport.update(self.status.get('sport'), current=self.status_current(), age_ns=self.sport_status_age_ns())

    def set_cluster_note(self, text):
        self.cluster_note.set_text(text); self.cluster_note.set_visible(bool(text))

    def set_notice(self, text, connection=False):
        """The notice reports action results and problems only; it is hidden otherwise.

        A connection problem clears itself on the next fresh error-free status.
        """
        self.connection_notice = connection and bool(text)
        self.notice.set_text(text); self.notice.set_visible(bool(text))

    def open_phone_app(self, app):
        """Restore CarPlay before sending one connection-bound app request."""
        if (app not in ('maps', 'music', 'phone') or self.shortcut_publisher is None or
                self.pending or not self.can_return()):
            return
        intent = {'source': (self.status.get('active'), self.status.get('generation'),
                             dict(self.shortcut_publisher)),
                  'app': app, 'phase': 'waiting', 'deadline': time.monotonic() + 3}
        self.set_notice('')
        if self.presentation is not None:
            self.back()
            if self.closing or self.presentation.foreground or self.presentation.opening:
                return
        self.shortcut_launch = intent
        self.action({'action': 'status'})

    def advance_shortcut(self):
        intent = self.shortcut_launch
        if intent is None or intent['phase'] != 'waiting':
            return
        current = (self.status.get('active'), self.status.get('generation'))
        if (not self.status_current() or time.monotonic() >= intent['deadline'] or
                current != intent['source'][:2] or self.status.get('active_mode') != 'ultra' or
                self.shortcut_publisher is not None and self.shortcut_publisher != intent['source'][2] or
                not self.camera_clear()):
            self.shortcut_launch = None
            self.action_sensitivity()
            return
        try:
            carplay = carplay_snapshot(self.status.get('carplay'))
        except ValueError:
            self.shortcut_launch = None
            self.action_sensitivity()
            return
        if carplay['state'] != 'ready' or not carplay['returnReady'] or self.shortcut_publisher is None:
            return
        # Home has already relinquished its lease. The receiver consumes app
        # input only with a qualified, healthy main presentation, so manager
        # acknowledgment while Home is foreground is not dispatch authority.
        intent['phase'] = 'sent'
        self.action({'action': 'phone_app', 'app': intent['app'],
                     'publisher': intent['source'][2]})

    def media_action(self, command):
        media = self.status.get('media') or {}
        if self.status.get('active') and media.get('available') and command in media.get('controls', ()):
            self.action({'action': 'media', 'phone': self.status['active'],
                         'generation': self.status['generation'], 'command': command})

    def disconnect_phone(self, phone):
        # Never queue a disconnect behind another operation: its captured
        # source/generation must still identify this receiver at execution.
        target = self.status.get('active') or recovery_phone(self.status.get('recovery'))
        if self.closing or self.pending or target != phone:
            return
        generation = self.status.get('generation')
        if type(generation) is not int:
            return
        self.action({'action': 'disconnect', 'phone': phone, 'generation': generation})

    def audio_action(self, step=0, toggle=False):
        audio = self.status.get('audio') or {}
        if audio.get('available') and self.status.get('active'):
            volume = audio.get('pendingVolumePercent', audio['volumePercent'])
            muted = audio.get('pendingMuted', audio['muted'])
            self.action({'action': 'audio', 'phone': self.status['active'],
                         'generation': self.status['generation'], 'route': audio['route'],
                         'volumePercent': min(100, max(0, volume + step)),
                         'muted': not muted if toggle else muted})

    def pairing_sensitivity(self):
        pairing = self.pairing
        safe = bool(self.status_current() and self.pairing_fresh and pairing and pairing['supported'] and not self.pending)
        phase = pairing['phase'] if pairing else ''
        idle = phase in ('idle', 'paired', 'error')
        self.add_phone.set_sensitive(safe and idle and not self.status.get('active'))
        self.add_phone.set_tooltip_text('Disconnect CarPlay before adding a phone.' if self.status.get('active') else 'Add a phone over Bluetooth')
        prompt = pairing['prompt'] if pairing else None
        current = (pairing['session'], prompt['id']) if pairing and prompt else None
        confirmation = bool(safe and phase == 'confirmation' and pairing['expiresInSeconds'] > 0 and pairing['session'] and prompt and
                            current != (self.pairing_decision[:2] if self.pairing_decision else None) and not self.status.get('active'))
        for key in ('pair', 'reject'):
            self.pairing_buttons[key].set_sensitive(confirmation)
        self.pairing_buttons['cancel'].set_sensitive(bool(safe and pairing['session'] and phase in ('starting', 'discoverable', 'confirmation')))

    def receiver_change_available(self):
        # Mirror the manager's require_pairing_idle contract using its current
        # snapshot; saved-phone callbacks must not interrupt Add phone.
        return not (self.pairing and self.pairing['phase'] in ('starting', 'discoverable', 'confirmation', 'stopping'))

    def pairing_action(self, kind):
        # Snapshot identity is copied at the click; no action is retained in a
        # prompt widget or queued behind a status refresh/other phone operation.
        pairing = self.pairing
        if not self.status_current() or not self.pairing_fresh or not pairing or not pairing['supported'] or self.pending:
            return
        if kind == 'start':
            if self.status.get('active') or pairing['phase'] not in ('idle', 'paired', 'error'):
                return
            self.pairing_decision = None
            self.action({'action': 'pairing_start'})
        elif kind == 'cancel':
            if not pairing['session'] or pairing['phase'] not in ('starting', 'discoverable', 'confirmation'):
                return
            self.action({'action': 'pairing_cancel', 'session': pairing['session']})
        elif kind in ('pair', 'reject'):
            prompt = pairing['prompt']
            if pairing['phase'] != 'confirmation' or pairing['expiresInSeconds'] <= 0 or not pairing['session'] or not prompt or self.status.get('active'):
                return
            identity = (pairing['session'], prompt['id'])
            if self.pairing_decision and self.pairing_decision[:2] == identity:
                return
            accepted = kind == 'pair'
            self.pairing_decision = (*identity, accepted)
            self.action({'action': 'pairing_confirm', 'session': identity[0], 'prompt': identity[1], 'accepted': accepted})

    def invalidate_pairing(self, message):
        self.pairing, self.pairing_fresh = None, False
        self.add_phone.set_sensitive(False)
        for button in self.pairing_buttons.values():
            button.set_sensitive(False)
        if self.pairing_card.get_visible():
            self.pairing_title.set_text('Pairing unavailable')
            self.pairing_note.set_text(message)
            self.pairing_prompt.set_visible(False)
            self.pairing_adapter.set_visible(False)
            self.pairing_time.set_visible(False)
            self.pairing_warning.set_visible(False)

    def render_pairing(self, data):
        value = data.get('pairing')
        if value is None or (type(value) is dict and set(value) == {'supported'} and value['supported'] is False):
            self.pairing, self.pairing_fresh = None, False
            self.add_phone.set_visible(False); self.pairing_card.set_visible(False)
            self.pairing_sensitivity()
            return
        try:
            pairing = pairing_snapshot(value)
        except (ValueError, TypeError):
            self.invalidate_pairing('Pairing information is incomplete. Refresh the connection manager to continue.')
            return
        self.add_phone.set_visible(pairing['supported'] and self.pages.get_visible_child_name() == 'phones')
        if not pairing['supported']:
            self.pairing, self.pairing_fresh = None, False
            self.pairing_card.set_visible(False); self.pairing_sensitivity()
            return
        self.pairing, self.pairing_fresh = pairing, True
        phase, prompt = pairing['phase'], pairing['prompt']
        identity = (pairing['session'], prompt['id']) if prompt else None
        if self.pairing_decision and (self.pairing_decision[0] != pairing['session'] or
                                     phase not in ('starting', 'discoverable', 'confirmation') or
                                     (identity is not None and self.pairing_decision[:2] != identity)):
            self.pairing_decision = None
        self.pairing_card.set_visible(phase != 'idle')
        self.pairing_time.set_visible(phase in ('starting', 'discoverable', 'confirmation') and pairing['expiresInSeconds'] > 0)
        self.pairing_time.set_text(f"{pairing['expiresInSeconds']} s remaining")
        self.pairing_adapter.set_text(pairing['adapterName'])
        self.pairing_adapter.set_visible(phase in ('starting', 'discoverable') and bool(pairing['adapterName']))
        self.pairing_prompt.set_visible(phase == 'confirmation' and prompt is not None)
        self.pairing_buttons['pair'].set_visible(phase == 'confirmation' and prompt is not None)
        self.pairing_buttons['reject'].set_visible(phase == 'confirmation' and prompt is not None)
        self.pairing_buttons['cancel'].set_visible(phase in ('starting', 'discoverable', 'confirmation', 'stopping'))
        titles = {'idle': 'Add your phone', 'starting': 'Starting pairing…', 'discoverable': 'Add your phone',
                  'confirmation': 'Check the pairing code', 'paired': 'Phone paired', 'stopping': 'Closing pairing…', 'error': 'Couldn’t pair'}
        self.pairing_title.set_text(titles[phase])
        instructions = {'idle': '', 'starting': 'Preparing a short Bluetooth pairing window.',
                        'discoverable': f"On your iPhone, open Settings → Bluetooth and select “{pairing['adapterName']}”.",
                        'confirmation': 'Waiting for the phone’s pairing request.',
                        'paired': 'Pairing is complete. Connect your iPhone to start CarPlay.',
                        'stopping': 'Waiting for Bluetooth discovery to stop.',
                        'error': pairing['error'] or 'Pairing didn’t finish. You can try adding the phone again.'}
        if phase == 'confirmation' and prompt:
            self.pairing_device.set_text(prompt['deviceName'] or prompt['address'])
            self.pairing_code.set_text(prompt['passkey']); self.pairing_code.set_visible(bool(prompt['passkey']))
            instructions['confirmation'] = ('Check that this code matches your iPhone. Choose Pair only if they match. Pairing allows this phone to reconnect.' if prompt['passkey'] else
                                            'Confirm this is the phone you want to add. Pair and allow this phone to reconnect.')
        if self.pairing_decision and phase in ('starting', 'discoverable', 'confirmation'):
            self.pairing_title.set_text('Finishing pairing…' if self.pairing_decision[2] else 'Rejecting pairing…')
            instructions[phase] = 'Waiting for the phone to finish pairing.' if self.pairing_decision[2] else 'Waiting for Bluetooth to close this request.'
        self.pairing_note.set_text(instructions[phase])
        cleanup_error = phase == 'paired' and bool(pairing['error'])
        self.pairing_warning.set_visible(cleanup_error)
        self.pairing_warning.set_text(f"Bluetooth cleanup needs attention: {pairing['error']}" if cleanup_error else '')
        self.pairing_sensitivity()

    def remove(self, phone):
        if self.pending or not self.status_current() or not self.receiver_change_available():
            return
        dialog = Gtk.MessageDialog(transient_for=self.window, modal=True, buttons=Gtk.ButtonsType.CANCEL,
                                   text='Remove this phone?', secondary_text='Its Bluetooth pairing and managed CarPlay keys will be deleted.')
        # A lowered/hidden page must not leave a modal over the returned main
        # surface. Unmap covers Home withdrawal as well as final destruction.
        # Disconnect when the dialog closes so Cancel retains no callback.
        retire = self.window.connect('unmap', lambda _: dialog.destroy())
        dialog.connect('unrealize', lambda _: self.window.disconnect(retire))
        dialog.add_button('Remove phone', Gtk.ResponseType.ACCEPT)
        def response(d, result):
            # A response queued before Home withdrawal may arrive after destroy.
            # Only the still-present confirmation may authorize removal.
            admitted = d.get_realized() and d.get_visible()
            d.destroy()
            if admitted and result == Gtk.ResponseType.ACCEPT:
                self.action({'action': 'forget', 'phone': phone})
        dialog.connect('response', response); dialog.present()

    def render_artwork(self, media):
        """Cache only artwork decoded from the exact bounded, published bytes."""
        texture = None
        try:
            name, digest = media.get('artwork'), media.get('artwork_id')
            if not media.get('available') or type(name) is not str or name in ('', '.', '..') or \
                    Path(name).name != name or type(digest) is not str or not re.fullmatch('[0-9a-f]{64}', digest):
                raise ValueError('Artwork publication unavailable')
            path = self.path.parent / name

            def stamp(info):
                if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or \
                        stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or not 33 <= info.st_size <= 1048576:
                    raise ValueError('Artwork file unavailable')
                return (info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_nlink,
                        info.st_size, info.st_mtime_ns, info.st_ctime_ns)

            before = stamp(path.lstat())
            key = name, digest, before
            if self.artwork_cache and self.artwork_cache[0] == key:
                texture = self.artwork_cache[1]
            else:
                descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
                with os.fdopen(descriptor, 'rb') as stream:
                    if stamp(os.fstat(stream.fileno())) != before:
                        raise ValueError('Artwork changed during open')
                    raw = stream.read(1048577)
                    if stamp(os.fstat(stream.fileno())) != before or stamp(path.lstat()) != before or len(raw) != before[5]:
                        raise ValueError('Artwork changed during read')
                if raw[:16] != b'\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR' or \
                        hashlib.sha256(raw).hexdigest() != digest:
                    raise ValueError('Artwork bytes do not match publication')
                width, height = struct.unpack('>II', raw[16:24])
                if not 1 <= width <= 1024 or not 1 <= height <= 1024:
                    raise ValueError('Artwork dimensions outside bounds')
                texture = Gdk.Texture.new_from_bytes(GLib.Bytes.new(raw))
                self.artwork_cache = key, texture
                self.backdrop.show_artwork(digest, raw)
        except (OSError, ValueError, GLib.Error):
            self.artwork_cache = None
        if texture is None:
            self.backdrop.clear()
        self.artwork_texture = texture
        self.cover.set_paintable(texture)
        self.cover_stack.set_visible_child_name('art' if texture is not None else 'empty')

    def render_media(self, data):
        try:
            session = carplay_snapshot(data.get('carplay'))['state']
        except ValueError:
            session = None
        media = data.get('media') or {}
        self.song.set_text(media.get('title') or ('Unknown title' if media.get('available') else 'Nothing playing'))
        self.artist.set_text(media.get('artist') or ('' if media.get('available') else 'Your music will appear here.'))
        self.album.set_text(media.get('album', '')); self.album.set_visible(bool(media.get('album')))
        position, duration = media.get('position_ms'), media.get('duration_ms')
        has_position = type(position) is int and 0 <= position <= 0xffffffff
        has_duration = type(duration) is int and 0 < duration <= 0xffffffff
        self.timeline.set_visible(bool(media.get('available') and (has_position or has_duration)))
        self.progress.set_visible(bool(has_position and has_duration and media.get('available')))
        self.progress.set_fraction(min(1, position / duration) if has_position and has_duration and media.get('available') else 0)
        self.elapsed.set_text(media_time(position) if media.get('available') else '—:—')
        self.duration.set_text(media_time(duration) if has_duration and media.get('available') else '—:—')
        self.render_artwork(media)
        self.home.render(data, session, self.artwork_texture)
        controls = media.get('controls', []) if media.get('available') else []
        status = media.get('status')
        for command, button in self.buttons.items():
            supported = command in controls
            button.set_sensitive(supported)
            # Prefer the useful playback action when both are supported. A
            # missing playback state cannot choose an action on the user's behalf.
            redundant = (command == 'play' and status == 'playing' and 'pause' in controls) or (
                command == 'pause' and status in ('paused', 'stopped') and 'play' in controls)
            button.set_visible(supported and not redundant)
        self.transport.set_visible(any(c in controls for c in ('previous', 'play', 'pause', 'next')))
        self.bluetooth_volume.set_visible(any(c in controls for c in ('volume_down', 'volume_up')))
        # Transport buttons already show playback state; the note explains absence only.
        self.media_note.set_text('' if media.get('available') else
                                 'Waiting for media information from your phone.' if data.get('active') and session in ('ready', 'home') else
                                 'Connect your iPhone to play music.')
        self.media_note.set_visible(bool(self.media_note.get_text()))
        audio = data.get('audio') or {}
        available_audio = bool(audio.get('available') and data.get('active'))
        self.audio_controls.set_visible(available_audio or bool(audio.get('error')))
        self.audio_route_controls.set_visible(available_audio)
        for button in self.audio_buttons.values():
            button.set_sensitive(available_audio)
        if available_audio:
            self.audio_value.set_text(f"{audio['volumePercent']}%" + (' · muted' if audio['muted'] else ''))
            desired_volume = audio.get('pendingVolumePercent', audio['volumePercent'])
            desired_muted = audio.get('pendingMuted', audio['muted'])
            self.audio_buttons['down'].set_sensitive(desired_volume > 0)
            self.audio_buttons['up'].set_sensitive(desired_volume < 100)
            self.audio_buttons['mute'].set_label('Unmute' if desired_muted else 'Mute')
            self.audio_note.set_text(audio.get('error') or
                                     (f"Applying {desired_volume}%" + (' · mute…' if desired_muted else '…') if audio.get('pending') else
                                      'Applied to the head-unit audio mixer.'))
        else:
            self.audio_value.set_text('Unavailable'); self.audio_buttons['mute'].set_label('Mute')
            self.audio_note.set_text(audio.get('error') or 'Audio output unavailable.')

    def render(self, data):
        self.render_sport()
        self.render_pairing(data)
        try:
            camera = camera_snapshot(data.get('camera'))
        except ValueError:
            camera = {}
        configured = camera.get('configured', False)
        self.view_buttons['camera'].set_visible(configured)
        if not configured and self.pages.get_visible_child_name() == 'camera':
            self.view_buttons['phones'].set_active(True)
        requested = camera.get('requested', False)
        request_state = ('On' if requested else 'Off') if camera else 'Unavailable'
        self.camera_request_status.set_text('Manual request · ' + request_state)
        phase = camera.get('phase', 'unavailable')
        self.camera_controls.set_sensitive(configured and not self.pending)
        self.camera_buttons[True].set_sensitive(not requested)
        self.camera_buttons[False].set_sensitive(requested)
        self.camera_status.set_text({'idle': 'Camera closed', 'waiting': 'Preparing camera',
                                    'active': 'Camera view visible', 'fault': 'Camera view unavailable',
                                    'stopped': 'Camera view stopped', 'unavailable': 'Camera view unavailable'}.get(
                                        phase, 'Camera information unavailable'))
        self.camera_note.set_text(camera.get('error') or ('Close the camera, check its connection, then open it again.' if phase == 'fault' else
                                  '' if phase == 'active' and requested else
                                  'Waiting for the camera display.' if requested else
                                  'Manual request is off. Vehicle activation may keep the camera open.' if phase == 'active' else
                                  'Ready to open the camera.' if configured and phase == 'idle' else
                                  'Check the camera connection and current CarPlay session.' if configured and phase == 'unavailable' else
                                  'Waiting for the camera display.' if configured else 'Set up a camera to use this view.'))
        self.camera_note.set_visible(bool(self.camera_note.get_text()))
        pairing_supported = bool(self.pairing and self.pairing['supported'])
        try:
            session = carplay_snapshot(data.get('carplay'))['state']
        except ValueError:
            session = None
        recovering = recovery_phone(data.get('recovery'))
        key = json.dumps([recovering, data.get('phones'), data.get('active'), data.get('active_mode'), session, pairing_supported], sort_keys=True)
        if key != self.rows_key:
            self.rows_key = key
            while self.listbox.get_first_child():
                self.listbox.remove(self.listbox.get_first_child())
            self.phone_widgets = {}
            self.empty_phones = None
            if not data.get('phones'):
                empty = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12); empty.add_css_class('card')
                self.empty_phones = empty
                empty.append(label('No paired phones', 'section'))
                message = label('Tap Add phone to pair your iPhone.' if pairing_supported else
                                'Pair your iPhone in Bluetooth settings to get started.', 'muted'); message.set_wrap(True)
                empty.append(message); self.listbox.append(empty)
            for phone in data.get('phones', []):
                p = phone['address']
                card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18); card.add_css_class('card')
                identity = Gtk.Box(spacing=16)
                phone_icon = icon('phone', 22, color='#ffffff'); phone_icon.add_css_class('phone-icon'); identity.append(phone_icon)
                names = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=5); names.set_hexpand(True); names.set_valign(Gtk.Align.CENTER)
                phone_name = label(phone['name'], 'section'); phone_name.set_max_width_chars(32); names.append(phone_name)
                state = 'Selected for CarPlay' if phone['active'] else 'Bluetooth connected' if phone['connected'] else 'Ready to connect'
                live = False
                if recovering == p:
                    state = 'Retrying CarPlay…'
                if phone['active'] and data.get('active') == p:
                    # Renderer readiness, not Bluetooth, says whether CarPlay runs.
                    if session in ('disconnected', 'stopped'): state = 'Waiting for your iPhone'
                    elif session == 'starting': state = 'Connecting…'
                    elif session == 'fault': state = 'CarPlay stopped unexpectedly'
                    elif data.get('active_mode') == 'ultra': state, live = 'CarPlay Ultra', True
                    elif data.get('active_mode') == 'standard': state, live = 'Standard CarPlay', True
                state_label = label(state, 'muted')
                if live or phone['connected']: state_label.add_css_class('connected')
                if live: card.add_css_class('phone-active')
                names.append(state_label); identity.append(names); card.append(identity)
                row = Gtk.Box(spacing=12)
                connect = Gtk.Button(label='Reconnect' if phone['active'] else 'Connect')
                connect.add_css_class('quiet' if live else 'suggested-action')
                connect.connect('clicked', lambda _, a=p: self.action({'action': 'connect', 'phone': a}))
                if not live:
                    row.append(connect)
                disconnect = Gtk.Button(label='Cancel retry' if recovering == p else 'Disconnect')
                disconnect.add_css_class('quiet')
                disconnect.set_visible(bool(phone['active']) or recovering == p)
                disconnect.connect('clicked', lambda _, a=p: self.disconnect_phone(a)); row.append(disconnect)
                more = Gtk.MenuButton()
                more.set_child(Gtk.Label(label='•••'))
                more.set_tooltip_text('Phone options')
                more.update_property([Gtk.AccessibleProperty.LABEL], ['Phone options'])
                options = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
                popover = Gtk.Popover()
                popover.set_child(options)
                more.set_popover(popover)
                if live:
                    options.append(connect)
                row.append(more)
                remove = Gtk.Button(label='Remove'); remove.add_css_class('destructive-action'); remove.connect('clicked', lambda _, a=p: self.remove(a)); options.append(remove)
                for option in (connect, remove):
                    option.connect('clicked', lambda _, menu=popover: menu.popdown())
                self.phone_widgets[p] = {'status': state_label, 'connect': connect,
                                        'disconnect': disconnect, 'remove': remove}
                row.set_valign(Gtk.Align.CENTER); identity.append(row); self.listbox.append(card)
        if getattr(self, 'empty_phones', None) is not None:
            self.empty_phones.set_visible(not self.pairing_card.get_visible())
        self.render_media(data)
        car_offered = self.car.render(data.get('prephone_vehicle'))
        self.view_buttons['car'].set_visible(car_offered)
        if not car_offered and self.pages.get_visible_child_name() == 'car':
            self.view_buttons['phones'].set_active(True)
        display = data.get('display_response') or {}
        configured = bool(display.get('configured'))
        for widget in (self.display_response_heading, self.display_responses, self.display_response_hint):
            widget.set_visible(configured)
        startup = data.get('startup_warning_lights') or {}
        startup_configured = bool(startup.get('configured'))
        for widget in (self.startup_warning_heading, self.startup_warning_note, self.startup_warning_choices):
            widget.set_visible(startup_configured)
        for hide, button in self.startup_warning_buttons.items():
            selected = startup_configured and startup.get('hide') is hide
            title = 'Hide during startup' if hide else 'Show during startup'
            button.set_label(title)
            if selected: button.add_css_class('selected')
            else: button.remove_css_class('selected')
        drive_configured = self.drive_modes.render(data.get('drive_modes'))
        greeting_configured = self.greeting.render(data.get('greeting_sequence'))
        any_setting = configured or startup_configured or drive_configured or greeting_configured
        self.view_buttons['settings'].set_visible(any_setting)
        if not any_setting and self.pages.get_visible_child_name() == 'settings':
            self.view_buttons['phones'].set_active(True)
        for mode, button in self.display_response_buttons.items():
            selected = configured and display.get('mode') == mode
            self.display_response_marks[mode].set_visible(selected)
            if selected: button.add_css_class('selected')
            else: button.remove_css_class('selected')
            button.update_property([Gtk.AccessibleProperty.LABEL], [mode.capitalize() + (' · Selected' if selected else '')])
        self.display_response_note.set_text('' if display.get('mode') else 'Using the installed default.')
        self.display_response_note.set_visible(configured and not display.get('mode'))
        self.cluster_formats.render(data.get('cluster_pages', ()), data.get('cluster_page'))
        ultra = data.get('ultra_cluster')
        self.shortcut_publisher = ({'nonce': ultra['nonce'], 'connectionGeneration': ultra['connectionGeneration']}
                                   if ultra else None)
        self.home.set_shortcuts(bool(ultra))
        self.native_cluster_pages.set_visible(not bool(ultra))
        self.ultra_styles.set_visible(bool(ultra))
        self.cluster_navigation.set_visible(bool(ultra))
        if ultra:
            publisher = {'nonce': ultra['nonce'], 'connectionGeneration': ultra['connectionGeneration']}
            catalog = ultra['nonce'], ultra['connectionGeneration'], tuple((c['id'], c['name']) for c in ultra['choices'])
            if catalog != self.style_catalog:
                for button in self.cluster_navigation_buttons:
                    self.cluster_navigation.remove(button)
                self.cluster_navigation_buttons = []
                # The phone uses vertical Menu controls for centre pages;
                # horizontal controls cycle the complete cluster style.
                for command, title in (('up', 'Previous page'), ('down', 'Next page'),
                                       ('select', 'Select'), ('back', 'Back')):
                    button = Gtk.Button(label=title)
                    button.set_size_request(130, 60)
                    button.connect('clicked', lambda _, value=command, owner=publisher:
                                   self.action({'action': 'cluster_input', 'command': value, 'publisher': owner}))
                    self.cluster_navigation.append(button)
                    self.cluster_navigation_buttons.append(button)
                for button in self.style_buttons.values():
                    self.ultra_styles.remove(button.get_parent())
                self.style_buttons.clear(); self.style_catalog = catalog
                # Format names from the manager label known authored layouts.
                format_names = {c['layout']: c['name'] for c in
                                ((data.get('cluster_formats') or {}).get('ultra') or {}).get('choices', ())
                                if type(c) is dict and type(c.get('layout')) is str and type(c.get('name')) is str}
                for identifier, name in catalog[2]:
                    name = format_names.get(identifier, name)
                    button = Gtk.Button()
                    button.add_css_class('list-row')
                    style_name = label(name, 'choice-label'); style_name.set_xalign(0); style_name.set_wrap(True)
                    style_name.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
                    style_name.set_lines(2); style_name.set_max_width_chars(20); button.set_child(style_name)
                    button.update_property([Gtk.AccessibleProperty.LABEL], [name])
                    button.set_tooltip_text(name)
                    button.connect('clicked', lambda _, choice=identifier, owner=publisher:
                                   self.action({'action': 'cluster_layout', 'layout': choice, 'publisher': owner}))
                    self.ultra_styles.append(button); self.style_buttons[identifier] = button
            options = (ultra['nonce'], ultra['connectionGeneration'], ultra['active'],
                       tuple((c['id'], c['name']) for c in ultra.get('palettes', [])),
                       tuple((c['zone'], c['id'], c['name'], c.get('available', True))
                             for c in ultra.get('contents', [])))
            self.cluster_options.set_visible(True)
            if options != self.option_catalog:
                child = self.cluster_options.get_first_child()
                while child:
                    following = child.get_next_sibling()
                    self.cluster_options.remove(child)
                    child = following
                self.option_catalog = options
                self.option_buttons = []
                for title, choices, kind in [('Colour', ultra.get('palettes', []), 'palette'),
                                             ('Centre content', ultra.get('contents', []), 'component')]:
                    if not choices:
                        continue
                    self.cluster_options.append(label(title.upper(), 'eyebrow'))
                    flow = list_group()
                    self.cluster_options.append(flow)
                    for choice in choices:
                        name = choice['name']
                        if kind == 'component':
                            name = {'DCA_Map': 'Map', 'DCA_ADAS': 'Driving assistance'}.get(name, name)
                        if not choice.get('available', True):
                            name += ' · unavailable'
                        button = list_row(Gtk.Button(label=name))
                        command = {'action': 'cluster_layout', 'layout': ultra['active'],
                                   'publisher': publisher, kind: choice['id']}
                        if kind == 'component':
                            command['zone'] = choice['zone']
                        button.connect('clicked', lambda _, request=command: self.action(request))
                        flow.append(button)
                        self.option_buttons.append((button, kind, choice['id'], choice.get('zone')))
            for button, kind, identifier, zone in self.option_buttons:
                active = (identifier == ultra.get('activePalette') if kind == 'palette' else
                          any(c['id'] == identifier and c['zone'] == zone and c['active']
                              for c in ultra.get('contents', [])))
                available = kind == 'palette' or any(
                    c['id'] == identifier and c['zone'] == zone and c.get('available', True)
                    for c in ultra.get('contents', []))
                button.set_sensitive(available and ultra['ready'] and ultra.get('idle', False) and not ultra['pending'])
                if active and ultra['ready'] and ultra.get('idle', False):
                    button.add_css_class('selected')
                else:
                    button.remove_css_class('selected')
            idle = ultra.get('idle', not ultra['transitioning'])
            for button in self.cluster_navigation_buttons:
                button.set_sensitive(ultra['ready'] and idle and not ultra['pending'])
            confirmed_active = ultra.get('confirmedActive', ultra['active'] if ultra['ready'] and idle else '')
            for identifier, button in self.style_buttons.items():
                button.set_sensitive(ultra['ready'] and idle)
                if identifier == confirmed_active: button.add_css_class('selected')
                else: button.remove_css_class('selected')
            self.cluster_eyebrow.set_text('CARPLAY ULTRA STYLE')
            self.cluster_eyebrow.set_visible(True)
            self.set_cluster_note(ultra['error'] or ultra.get('preferenceError', '') or ('Changing cluster style…' if ultra['pending'] or ultra['transitioning'] or ultra.get('restoring') else
                                                        'Your phone stays connected while the style changes.' if ultra['ready'] and idle else
                                                        'Waiting for the Ultra layout connection…'))
        else:
            self.cluster_options.set_visible(False)
            self.cluster_eyebrow.set_text('CENTER VIEW')
            # The native picker labels its own groups.
            self.cluster_eyebrow.set_visible(False)
            self.set_cluster_note('' if 'map' in data.get('cluster_pages', ()) else 'Map appears during Standard CarPlay.')
        vehicle_content = data.get('vehicle_content', {})
        show_content = bool(ultra and vehicle_content.get('configured'))
        self.vehicle_content_box.set_visible(show_content)
        if show_content:
            catalog = (ultra['nonce'], ultra['connectionGeneration'],
                       tuple((c['id'], c['name']) for c in vehicle_content.get('choices', [])))
            if catalog != self.vehicle_content_catalog:
                child = self.vehicle_content_box.get_first_child()
                while child:
                    following = child.get_next_sibling()
                    self.vehicle_content_box.remove(child)
                    child = following
                self.vehicle_content_buttons = []
                self.vehicle_content_catalog = catalog
                self.vehicle_content_box.append(label('VEHICLE VIEWS', 'eyebrow'))
                flow = list_group()
                self.vehicle_content_box.append(flow)
                owner = {'nonce': catalog[0], 'connectionGeneration': catalog[1]}
                for identifier, title in (*catalog[2], (None, 'Close vehicle view')):
                    button = list_row(Gtk.Button(label=title))
                    button.connect('clicked', lambda _, page=identifier, publisher=owner:
                                   self.action({'action': 'cluster_content', 'page': page, 'publisher': publisher}))
                    flow.append(button)
                    self.vehicle_content_buttons.append(button)
                note = label('Vehicle data appears when available. You can close this view at any time.', 'muted')
                note.set_wrap(True)
                self.vehicle_content_box.append(note)
            for button in self.vehicle_content_buttons:
                button.set_sensitive(ultra['ready'] and ultra.get('idle', not ultra['transitioning']))
        if data.get('error'):
            self.set_notice(data['error'])
        elif getattr(self, 'connection_notice', False) or self.notice.get_text() in ('Connecting to phone manager…', 'Waiting for the connection manager…'):
            self.set_notice('')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True, type=Path)
    parser.add_argument('--view', choices=VIEWS, default='home', help='Initial native view on the same page owner')
    args = parser.parse_args(argv)
    return PhonePage(args.socket, initial_view=args.view).run([])


if __name__ == '__main__':
    raise SystemExit(main())
