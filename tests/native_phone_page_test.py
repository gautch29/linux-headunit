# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""GTK actions against an isolated fake manager; never contacts BlueZ/a phone."""
import json
import os
from pathlib import Path
import socketserver
import sys
import tempfile
import threading
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_page import PhonePage, GLib, Gtk, media_time
from phone_page_presentation import camera_snapshot
from media_state import validate_media

A, B = 'AA:BB:CC:DD:EE:01', 'AA:BB:CC:DD:EE:02'
phones = {A, B}
calls = []
active = None
active_mode = None
manager_loss = False
mutation_failure = False
backend_error = ''
recovering = None
STYLE_A, STYLE_B = '00000000-1111-2222-3333-444444444444', '00000000-1111-2222-3333-555555555555'
STYLE_CHOICES = [{'id': STYLE_A, 'name': 'W' * 256}, {'id': STYLE_B, 'name': 'Band'}] + [
    {'id': f'00000000-1111-2222-3333-{number:012d}', 'name': f'Fixture {number}: a long cluster style name'}
    for number in range(16)]
style = STYLE_A
cluster_page = 'media'
assistance_available = False
audio_sequence, audio_volume, audio_muted = 0, 65, False
audio_applied, audio_available = False, True
camera_configured, camera_requested, camera_phase = False, False, 'unavailable'
camera_error = ''
content_configured, content_requested = False, None
status_entered, status_release = threading.Event(), threading.Event()
status_release.set()
ROUTE = 'a' * 32
class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        global active, active_mode, recovering, style, cluster_page, audio_sequence, audio_volume, audio_muted, camera_requested, camera_phase, content_requested
        data = json.loads(self.rfile.readline(4096)); calls.append(data)
        action = data['action']
        if action == 'status':
            status_entered.set()
            assert status_release.wait(3), 'Fixture status was not released'
            if manager_loss:
                self.wfile.write(json.dumps({'ok': False, 'error': 'Fixture manager unavailable'}).encode()+b'\n')
                return
            reply = {'ok': True, 'phones': [{'address': a, 'name': 'Bench phone '+a[-2:] if a == A else 'Long phone name ' + 'W' * 240, 'connected': False,
                                           'active': a == active, 'selected': a == active} for a in sorted(phones)],
                     'active': active, 'active_mode': active_mode, 'generation': 1,
                     'recovery': {'phone': recovering, 'remainingAttempts': 3 if recovering else 0},
                     'cluster_pages': ['media', 'vehicle', 'trip'], 'cluster_page': cluster_page,
                     'carplay': {'state': 'ready' if active else 'disconnected', 'returnReady': bool(active)},
                     'media': {'available': False, 'controls': []}, 'error': backend_error,
                     'vehicle_content': {'configured': content_configured, 'requested': content_requested, 'confirmed': False,
                                         'choices': [{'id': 'body', 'name': 'Seats and doors'}, {'id': 'tyres', 'name': 'Tyre status'}, {'id': 'charging', 'name': 'Charging'}]},
                     'camera': {'configured': camera_configured, 'requested': camera_requested, 'phase': camera_phase, 'error': camera_error},
                     'ultra_cluster': {'nonce': 'a' * 32, 'connectionGeneration': '1',
                                       'active': style, 'pending': '', 'ready': True, 'idle': True, 'transitioning': False, 'error': '',
                                       'contents': [{'id': STYLE_A, 'zone': STYLE_A, 'name': 'DCA_Map', 'active': True},
                                                    {'id': STYLE_B, 'zone': STYLE_A, 'name': 'DCA_ADAS', 'active': False,
                                                     'available': assistance_available}],
                                       'choices': STYLE_CHOICES} if active and active_mode == 'ultra' else None}
            if active:
                reply['audio'] = {'available': audio_available, 'route': ROUTE,
                                  'volumePercent': audio_volume if audio_applied else 65,
                                  'muted': audio_muted if audio_applied else False,
                                  'pending': bool(audio_sequence and not audio_applied), 'error': ''}
                if audio_sequence and not audio_applied:
                    reply['audio'].update(pendingVolumePercent=audio_volume, pendingMuted=audio_muted)
        else:
            if mutation_failure:
                self.wfile.write(json.dumps({'ok': False, 'error': 'Explicit mutation rejected'}).encode() + b'\n')
                return
            if action == 'connect': active = data['phone']
            elif action == 'disconnect':
                assert data == {'action': 'disconnect', 'phone': active or recovering, 'generation': 1}
                active, active_mode, recovering = None, None, None
            elif action == 'forget':
                phones.remove(data['phone'])
                if active == data['phone']: active, active_mode = None, None
            elif action == 'cluster_layout':
                assert data['publisher'] == {'nonce': 'a' * 32, 'connectionGeneration': '1'}
                style = data['layout']
            elif action == 'cluster_page':
                assert data['page'] in ('media', 'vehicle', 'trip')
                cluster_page = data['page']
            elif action == 'cluster_input':
                assert set(data) == {'action', 'command', 'publisher'}
                assert data['publisher'] == {'nonce': 'a' * 32, 'connectionGeneration': '1'}
                assert data['command'] in ('up', 'down', 'select', 'back')
            elif action == 'audio':
                assert set(data) == {'action', 'phone', 'generation', 'route', 'volumePercent', 'muted'}
                assert data['phone'] == active and data['generation'] == 1 and data['route'] == ROUTE
                audio_sequence += 1; audio_volume, audio_muted = data['volumePercent'], data['muted']
            elif action == 'cluster_content':
                assert set(data) == {'action', 'page', 'publisher'}
                assert data['publisher'] == {'nonce': 'a' * 32, 'connectionGeneration': '1'}
                assert data['page'] in (None, 'body', 'tyres', 'charging')
                content_requested = data['page']
            elif action == 'camera':
                assert set(data) == {'action', 'enabled'} and type(data['enabled']) is bool
                camera_requested = data['enabled']
                if camera_requested: camera_phase = 'waiting'
            else: raise AssertionError(action)
            reply = {'ok': True, 'message': 'Test action accepted'}
        self.wfile.write(json.dumps(reply).encode()+b'\n')

with tempfile.TemporaryDirectory(prefix='headunit-native-page-') as folder:
    path = Path(folder)/'manager.sock'
    server = socketserver.UnixStreamServer(str(path), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
    app = PhonePage(path)
    stage = 0
    errors = []
    started = time.monotonic()
    def tick():
        global stage, active_mode, recovering, manager_loss, audio_applied, audio_available, camera_configured, camera_requested, camera_phase, camera_error, content_configured
        global mutation_failure, backend_error
        try:
            if time.monotonic()-started > 12: raise AssertionError('native page action timed out')
            if (app.pending and stage != 20) or not hasattr(app, 'listbox') or not app.listbox.get_first_child(): return True
            first = app.listbox.get_first_child()
            if stage == 0:
                assert app.window.get_width() == 1280 and app.window.get_height() == 720, (app.window.get_width(), app.window.get_height())
                assert not any(b.get_sensitive() for b in app.buttons.values())
                assert not any(b.get_sensitive() for b in app.audio_buttons.values())
                assert app.audio_value.get_text() == 'Unavailable' # Older manager replies omit audio.
                assert not app.camera_controls.get_sensitive()
                assert not app.view_buttons['camera'].get_visible()
                app.view_buttons['camera'].set_active(True)
                assert app.pages.get_visible_child_name() == 'phones' and app.view_buttons['phones'].get_active()
                assert app.phone_widgets[A]['status'].get_text() == 'Ready to connect'
                assert not any(isinstance(widget, Gtk.ToggleButton) for row in app.phone_widgets.values() for widget in row.values())
                before = len(calls)
                app.action({'action': 'mode', 'phone': A, 'mode': 'ultra'})
                assert len(calls) == before and not app.pending  # Retired commands cannot reach the manager.
                assert not app.phone_widgets[A]['disconnect'].get_visible()
                app.view_buttons['music'].set_active(True)
                assert app.pages.get_visible_child_name() == 'music'
                assert app.page_title.get_text() == 'Music'
                app.view_buttons['phones'].set_active(True)
                assert app.pages.get_visible_child_name() == 'phones'
                app.view_buttons['cluster'].set_active(True); stage = 16
            elif stage == 16:
                # All phone-free choices must fit the real fixed touchscreen,
                # including Trip. Check native widget allocation, not a mockup.
                if app.pages.get_transition_running(): return True
                assert app.window.get_width() == 1280 and app.window.get_height() == 720
                assert not app.cluster_buttons['map'].get_parent().get_visible()
                for page in ('media', 'vehicle', 'trip'):
                    button = app.cluster_buttons[page]
                    if button.get_width() == 0: return True
                    ok, bounds = button.compute_bounds(app.cluster_scroll)
                    measurement = (page, bounds.get_x(), bounds.get_y(), bounds.get_width(), bounds.get_height(),
                                   app.cluster_scroll.get_width(), app.cluster_scroll.get_height())
                    assert ok and bounds.get_x() >= 0 and bounds.get_y() >= 0, measurement
                    assert bounds.get_x() + bounds.get_width() <= app.cluster_scroll.get_width(), measurement
                    assert bounds.get_y() + bounds.get_height() <= app.cluster_scroll.get_height(), measurement
                    assert bounds.get_width() >= 300 and bounds.get_height() >= 44, measurement
                assert app.cluster_selection_marks['media'].get_visible()
                assert not app.cluster_selection_marks['trip'].get_visible()
                snapshot = os.environ.get('HEADUNIT_NATIVE_PAGE_SNAPSHOT')
                if snapshot:
                    paintable = Gtk.WidgetPaintable.new(app.window)
                    image = Gtk.Snapshot(); paintable.snapshot(image, app.window.get_width(), app.window.get_height())
                    texture = app.window.get_renderer().render_texture(image.to_node(), None)
                    assert texture.save_to_png(snapshot)
                app.cluster_buttons['trip'].emit('clicked'); stage = 17
            elif stage == 17:
                if app.status.get('cluster_page') != 'trip': return True
                assert app.cluster_selection_marks['trip'].get_visible()
                assert not app.cluster_selection_marks['media'].get_visible()
                assert app.cluster_buttons['trip'].has_css_class('selected')
                assert not any(c['action'] == 'connect' for c in calls)
                app.view_buttons['phones'].set_active(True)
                app.phone_widgets[A]['connect'].emit('clicked'); stage = 2
            elif stage == 2:
                if active != A: return True
                assert app.phone_widgets[A]['disconnect'].get_visible()
                assert app.phone_widgets[A]['status'].get_text() == 'Selected for CarPlay'  # No negotiated mode yet.
                before = len(calls); app.disconnect_phone(B); assert len(calls) == before
                active_mode = 'standard'
                status_entered.clear(); status_release.clear()
                app.refresh(); stage = 20
            elif stage == 20:
                if not status_entered.is_set(): return True
                assert app.pending_action == 'status'
                assert app.phone_widgets[A]['status'].get_text() == 'Selected for CarPlay'
                status_release.set(); stage = 21
            elif stage == 21:
                assert app.phone_widgets[A]['status'].get_text() == 'Standard CarPlay'
                active_mode = 'automatic'; app.refresh(); stage = 22
            elif stage == 22:
                assert app.phone_widgets[A]['status'].get_text() == 'Selected for CarPlay'  # Malformed/future values do not guess.
                active_mode = 'ultra'; app.refresh(); stage = 23
            elif stage == 23:
                assert app.phone_widgets[A]['status'].get_text() == 'CarPlay Ultra'
                assert app.phone_widgets[B]['status'].get_text() == 'Ready to connect'
                assert (app.window.get_width(), app.window.get_height()) == (1280, 720)
                second_card = app.listbox.get_first_child().get_next_sibling()
                phone_name = second_card.get_first_child().get_first_child().get_next_sibling().get_first_child()
                assert phone_name.get_layout().is_ellipsized(), 'A long phone name must yield space to connection controls'
                for button in app.phone_widgets[B].values():
                    ok, bounds = button.compute_bounds(app.window)
                    assert ok and bounds.get_x() >= 0 and bounds.get_x() + bounds.get_width() <= 1280
                assert not any(c['action'] == 'mode' for c in calls)
                if destination := os.environ.get('HEADUNIT_PHONE_PAGE_SNAPSHOT'):
                    paintable = Gtk.WidgetPaintable.new(app.window)
                    snapshot = Gtk.Snapshot.new()
                    paintable.snapshot(snapshot, app.window.get_width(), app.window.get_height())
                    texture = app.window.get_renderer().render_texture(snapshot.to_node(), None)
                    assert texture.save_to_png(destination)
                manager_loss = True; app.refresh(); stage = 24
            elif stage == 24:
                assert not app.status_available and not app.listbox.is_sensitive()
                assert app.notice.get_text() == 'Fixture manager unavailable'
                assert all(row['status'].get_text() == 'Connection status unavailable' for row in app.phone_widgets.values())
                assert app.retry_button.get_visible() and app.retry_button.get_sensitive()
                manager_loss = False
                app.retry_button.emit('clicked'); app.retry_button.emit('clicked')
                assert app.pending and not app.retry_button.get_sensitive()
                stage = 25
            elif stage == 25:
                assert app.phone_widgets[A]['status'].get_text() == 'CarPlay Ultra' and app.listbox.is_sensitive()
                assert not app.notice.get_text() and not app.notice.get_visible(), 'Recovered status retained a stale manager notice'
                assert not app.retry_button.get_visible()
                app.view_buttons['cluster'].set_active(True)
                assert not app.native_cluster_pages.get_visible()
                assert app.ultra_styles.get_visible() and len(app.style_buttons) == len(STYLE_CHOICES)
                buttons = {b.get_label(): b for b in app.cluster_navigation_buttons}
                buttons['Next page'].emit('clicked'); stage = 251
            elif stage == 251:
                assert app.notice.get_text() == 'Test action accepted', 'Background status cleared the action result'
                assert any(c == {'action': 'cluster_input', 'command': 'down',
                                 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}} for c in calls)
                buttons = {b.get_label(): b for b in app.cluster_navigation_buttons}
                mutation_failure = True
                buttons['Previous page'].emit('clicked'); stage = 2520
            elif stage == 2520:
                assert app.status_available and app.notice.get_text() == 'Explicit mutation rejected', 'Healthy status cleared a failed action result'
                mutation_failure = False
                backend_error = 'Explicit current manager error'
                app.refresh(); stage = 2521
            elif stage == 2521:
                assert app.notice.get_text() == backend_error
                app.refresh(); stage = 2522
            elif stage == 2522:
                assert app.notice.get_text() == backend_error, 'Current backend error was cleared while present'
                backend_error = ''
                buttons = {b.get_label(): b for b in app.cluster_navigation_buttons}
                buttons['Previous page'].emit('clicked'); stage = 252
            elif stage == 252:
                assert any(c == {'action': 'cluster_input', 'command': 'up',
                                 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}} for c in calls)
                app.style_buttons[STYLE_B].emit('clicked'); stage = 3
            elif stage == 3:
                if style != STYLE_B: return True
                if app.pages.get_transition_running(): return True
                # A larger style catalogue must scroll inside the touchscreen,
                # and long authored names must not widen the appliance window.
                assert (app.window.get_width(), app.window.get_height()) == (1280, 720)
                assert app.status['ultra_cluster']['choices'][0]['name'] == 'W' * 256  # Catalogue contract maximum.
                style_name = app.style_buttons[STYLE_A].get_child()
                assert style_name.get_text() == 'W' * 256
                assert style_name.get_layout().get_line_count() == 2 and style_name.get_layout().is_ellipsized()
                assert style_name.measure(Gtk.Orientation.HORIZONTAL, -1)[0] <= 1280
                ok, bounds = style_name.compute_bounds(app.cluster_scroll)
                assert ok and bounds.get_x() >= 0 and bounds.get_x() + bounds.get_width() <= app.cluster_scroll.get_width(), \
                    (bounds.get_x(), bounds.get_width(), app.cluster_scroll.get_width())
                adjustment = app.cluster_scroll.get_vadjustment()
                assert adjustment.get_upper() > adjustment.get_page_size(), (adjustment.get_upper(), adjustment.get_page_size())
                adjustment.set_value(adjustment.get_upper() - adjustment.get_page_size())
                stage = 18
            elif stage == 18:
                global assistance_available
                centre = {identifier: button for button, kind, identifier, _ in app.option_buttons
                          if kind == 'component'}
                assert centre[STYLE_A].get_sensitive() and centre[STYLE_A].has_css_class('selected')
                assert not centre[STYLE_B].get_sensitive()
                assert centre[STYLE_B].get_label() == 'Driving assistance · unavailable'
                assistance_available = True
                app.refresh(); stage = 181
            elif stage == 181:
                if not app.status['ultra_cluster']['contents'][1]['available']: return True
                centre = {identifier: button for button, kind, identifier, _ in app.option_buttons
                          if kind == 'component'}
                assert centre[STYLE_B].get_sensitive() and centre[STYLE_B].get_label() == 'Driving assistance'
                assert not centre[STYLE_B].has_css_class('selected')
                assistance_available = False
                app.refresh(); stage = 182
            elif stage == 182:
                if app.status['ultra_cluster']['contents'][1]['available']: return True
                centre = {identifier: button for button, kind, identifier, _ in app.option_buttons
                          if kind == 'component'}
                assert not centre[STYLE_B].get_sensitive()
                ok, bounds = app.style_buttons[STYLE_CHOICES[-1]['id']].compute_bounds(app.cluster_scroll)
                assert ok and bounds.get_y() >= 0 and bounds.get_y() + bounds.get_height() <= app.cluster_scroll.get_height(), \
                    (bounds.get_y(), bounds.get_height(), app.cluster_scroll.get_height())
                assert app.style_buttons[STYLE_B].has_css_class('selected')
                assert sum(c['action'] == 'connect' for c in calls) == 1  # Style button never reconnects.
                content_configured = True
                app.refresh(); stage = 183
            elif stage == 183:
                if not app.status.get('vehicle_content', {}).get('configured'): return True
                assert app.vehicle_content_box.get_visible()
                assert len(app.vehicle_content_buttons) == 4
                assert all(button.get_sensitive() for button in app.vehicle_content_buttons)
                adjustment = app.cluster_scroll.get_vadjustment()
                adjustment.set_value(adjustment.get_upper() - adjustment.get_page_size())
                assert not any(button.has_css_class('selected') for button in app.vehicle_content_buttons)
                app.vehicle_content_buttons[0].emit('clicked'); stage = 184
            elif stage == 184:
                if content_requested != 'body': return True
                assert not any(button.has_css_class('selected') for button in app.vehicle_content_buttons)
                app.vehicle_content_buttons[-1].emit('clicked'); stage = 185
            elif stage == 185:
                if content_requested is not None: return True
                assert sum(c['action'] == 'connect' for c in calls) == 1
                assert any(c['action'] == 'cluster_content' and c['page'] is None for c in calls)
                app.view_buttons['music'].set_active(True)
                assert all(b.get_sensitive() for b in app.audio_buttons.values())
                app.audio_buttons['up'].emit('clicked'); stage = 4
            elif stage == 4:
                if audio_sequence != 1 or not app.status.get('audio', {}).get('pending'): return True
                assert app.audio_controls.get_visible() and app.audio_route_controls.get_visible()
                assert app.audio_value.get_text() == '65%' # Request does not invent applied volume.
                assert 'Applying 70%' in app.audio_note.get_text()
                app.audio_buttons['mute'].emit('clicked'); stage = 5
            elif stage == 5:
                if audio_sequence != 2 or not app.status.get('audio', {}).get('pendingMuted'): return True
                assert audio_volume == 70 and audio_muted
                assert app.audio_value.get_text() == '65%'
                audio_applied = True; app.refresh(); stage = 6
            elif stage == 6:
                if app.status.get('audio', {}).get('pending'): return True
                assert app.audio_value.get_text() == '70% · muted'
                assert app.audio_buttons['mute'].get_label() == 'Unmute'
                audio_available = False; app.refresh(); stage = 7
            elif stage == 7:
                if app.status.get('audio', {}).get('available'): return True
                assert not app.audio_controls.get_visible() and not app.audio_route_controls.get_visible()
                assert not any(b.get_sensitive() for b in app.audio_buttons.values())
                assert app.audio_value.get_text() == 'Unavailable'
                app.view_buttons['phones'].set_active(True)
                app.phone_widgets[A]['disconnect'].emit('clicked')
                app.phone_widgets[A]['disconnect'].emit('clicked'); stage = 14
            elif stage == 14:
                if active is not None or app.status.get('active') is not None: return True
                assert A in phones and active_mode is None
                assert app.phone_widgets[A]['status'].get_text() == 'Ready to connect'
                assert not app.phone_widgets[A]['disconnect'].get_visible()
                assert app.phone_widgets[A]['connect'].get_label() == 'Connect'
                assert not any(c['action'] == 'forget' for c in calls)
                assert [c for c in calls if c['action'] == 'disconnect'] == [{'action': 'disconnect', 'phone': A, 'generation': 1}]
                recovering = A
                app.refresh()
                stage = 27
            elif stage == 27:
                if app.status.get('recovery', {}).get('phone') != A: return True
                assert app.status.get('active') is None
                assert not app.can_return()
                assert app.phone_widgets[A]['status'].get_text() == 'Retrying CarPlay…'
                assert app.phone_widgets[A]['disconnect'].get_visible()
                assert app.phone_widgets[A]['disconnect'].get_label() == 'Cancel retry'
                before = len(calls)
                app.disconnect_phone(B)
                assert len(calls) == before
                app.phone_widgets[A]['disconnect'].emit('clicked')
                stage = 28
            elif stage == 28:
                if recovering or app.status.get('recovery', {}).get('phone'): return True
                assert not app.phone_widgets[A]['disconnect'].get_visible()
                assert app.phone_widgets[A]['status'].get_text() == 'Ready to connect'
                assert len([c for c in calls if c['action'] == 'disconnect']) == 2
                app.phone_widgets[A]['connect'].emit('clicked'); stage = 15
            elif stage == 15:
                if active != A or app.status.get('active') != A: return True
                app.remove(A)
                dialogs = [w for w in Gtk.Window.get_toplevels() if isinstance(w, Gtk.MessageDialog)]
                assert len(dialogs) == 1
                dialogs[0].response(Gtk.ResponseType.ACCEPT); stage = 8
            elif stage == 8:
                if A in phones: return True
                assert B in phones
                assert any(c == {'action':'connect','phone':A} for c in calls)
                camera_configured = True; camera_phase = 'idle'; app.refresh(); stage = 9
            elif stage == 9:
                if not app.status.get('camera', {}).get('configured'): return True
                assert app.view_buttons['camera'].get_visible()
                app.view_buttons['camera'].set_active(True)
                assert app.pages.get_visible_child_name() == 'camera' and app.page_title.get_text() == 'Camera'
                assert app.camera_buttons[True].get_sensitive() and not app.camera_buttons[False].get_sensitive()
                app.camera_buttons[True].emit('clicked'); stage = 10
            elif stage == 10:
                if not app.status.get('camera', {}).get('requested'): return True
                assert app.camera_status.get_text() == 'Preparing camera' # Accepted intent is not visible.
                assert app.camera_request_status.get_text() == 'Manual request · On'
                assert not app.camera_buttons[True].get_sensitive() and app.camera_buttons[False].get_sensitive()
                camera_phase = 'active'; app.refresh(); stage = 11
            elif stage == 11:
                if app.camera_status.get_text() != 'Camera view visible': return True
                for widget in (app.camera_status, app.camera_request_status, *app.camera_buttons.values()):
                    if widget.get_width() == 0:
                        return True
                    ok, bounds = widget.compute_bounds(app.pages)
                    assert ok and bounds.get_x() >= 0 and bounds.get_y() >= 0
                    assert bounds.get_x() + bounds.get_width() <= app.pages.get_width()
                    assert bounds.get_y() + bounds.get_height() <= app.pages.get_height()
                if destination := os.environ.get('HEADUNIT_CAMERA_PAGE_SNAPSHOT'):
                    paintable = Gtk.WidgetPaintable.new(app.window)
                    snapshot = Gtk.Snapshot.new()
                    paintable.snapshot(snapshot, app.window.get_width(), app.window.get_height())
                    texture = app.window.get_renderer().render_texture(snapshot.to_node(), None)
                    assert texture.save_to_png(destination)
                app.camera_buttons[False].emit('clicked'); stage = 12
            elif stage == 12:
                if app.status.get('camera', {}).get('requested'): return True
                assert camera_phase == 'active' and app.camera_status.get_text() == 'Camera view visible'
                assert 'Manual request is off' in app.camera_note.get_text() # Possible vehicle activation stays independent.
                assert app.camera_request_status.get_text() == 'Manual request · Off'
                camera_phase = 'fault'; camera_error = 'W' * 128; app.refresh(); stage = 26
            elif stage == 26:
                if app.status.get('camera', {}).get('phase') != 'fault': return True
                assert camera_snapshot(app.status['camera'])['error'] == 'W' * 128
                assert (app.window.get_width(), app.window.get_height()) == (1280, 720)
                assert app.camera_note.get_text() == 'W' * 128
                assert app.camera_note.get_layout().get_line_count() > 1 and not app.camera_note.get_layout().is_ellipsized()
                assert app.camera_note.measure(Gtk.Orientation.HORIZONTAL, -1)[0] <= 1280
                ok, bounds = app.camera_note.compute_bounds(app.pages)
                assert ok and bounds.get_x() >= 0 and bounds.get_y() >= 0, (bounds.get_x(), bounds.get_y())
                assert bounds.get_x() + bounds.get_width() <= app.pages.get_width()
                assert bounds.get_y() + bounds.get_height() <= app.pages.get_height()
                camera_configured = False; camera_phase = 'unavailable'; camera_error = ''; app.refresh(); stage = 13
            elif stage == 13:
                if app.status.get('camera', {}).get('configured'): return True
                assert not app.camera_controls.get_sensitive()
                assert not app.view_buttons['camera'].get_visible()
                assert app.pages.get_visible_child_name() == 'phones' and app.page_title.get_text() == 'Phones'
                app.view_buttons['camera'].set_active(True)
                assert app.pages.get_visible_child_name() == 'phones' and app.view_buttons['phones'].get_active()
                assert [c for c in calls if c['action'] == 'camera'] == [{'action': 'camera', 'enabled': True}, {'action': 'camera', 'enabled': False}]
                app.quit(); return False
        except Exception as error:
            errors.append(error); app.quit(); return False
        return True
    GLib.timeout_add(100, tick)
    try:
        code = app.run([])
        assert code == 0 and stage == 13 and not errors, (code, stage, errors)
        print('Native GTK page: no mode selector/RPC, fresh negotiated mode only, held unknown/Standard/Ultra status, phone/media/style actions, requested-only vehicle views and Close, phone-free Cluster/Trip touchscreen fit, maximum accepted unbroken style/camera text stays within 1280 x 720, camera request versus presented state, cancellation and unavailable controls passed (isolated fake manager; no phone)')
    finally:
        status_release.set()
        server.shutdown();server.server_close();worker.join(timeout=2)


def run_music_layout_checks():
    """Actual GTK allocations with synthetic metadata, no phone/scanout claim."""
    title = 'Piano Concerto No. 2 in C Minor, Op. 18: II. Adagio sostenuto'
    extreme_title = '<b>' + 'x' * 2041 + '</b>'  # Maximum valid UTF-8 metadata; plain text.
    assert media_time(None) == media_time(False) == media_time(-1) == '—:—'
    assert media_time(0) == '0:00' and media_time(3601000) == '1:00:01'
    media = validate_media({'available': True, 'source': 'bluetooth', 'title': title,
                            'artist': 'The Philharmonic Orchestra with the Festival Chamber Players',
                            'album': 'Rachmaninoff: Piano Concertos — The Complete Concert Recordings',
                            'status': 'playing', 'duration_ms': 600000, 'position_ms': 120000,
                            'controls': ['pause', 'next', 'previous']})
    music_calls = []

    class MusicHandler(socketserver.StreamRequestHandler):
        def handle(self):
            message = json.loads(self.rfile.readline(4096)); music_calls.append(message)
            assert message == {'action': 'status'}
            reply = {'ok': True, 'phones': [], 'active': A if media['available'] else None,
                     'active_mode': None, 'generation': 1, 'error': '', 'media': dict(media),
                     'carplay': {'state': 'ready' if media['available'] else 'disconnected',
                                 'returnReady': media['available']}}
            self.wfile.write(json.dumps(reply).encode() + b'\n')

    with tempfile.TemporaryDirectory(prefix='headunit-native-music-') as folder:
        path = Path(folder) / 'manager.sock'
        server = socketserver.UnixStreamServer(str(path), MusicHandler)
        worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
        app = PhonePage(path)
        errors, stage, shown_at = [], 0, None
        started = time.monotonic()

        def bounded_title():
            assert (app.window.get_width(), app.window.get_height()) == (1280, 720)
            assert 1 <= app.song.get_layout().get_line_count() <= 2
            assert 0 < app.song.get_height() <= 90
            scroll = app.media_box.get_parent()
            while not isinstance(scroll, Gtk.ScrolledWindow):
                scroll = scroll.get_parent()
            for widget in (app.song, app.buttons['previous'], app.buttons['pause'], app.buttons['next']):
                ok, bounds = widget.compute_bounds(scroll)
                measurement = (bounds.get_x(), bounds.get_y(), bounds.get_width(), bounds.get_height(),
                               scroll.get_width(), scroll.get_height())
                assert ok and bounds.get_x() >= 0 and bounds.get_y() >= 0, measurement
                assert bounds.get_x() + bounds.get_width() <= scroll.get_width(), measurement
                assert bounds.get_y() + bounds.get_height() <= scroll.get_height(), measurement
            assert app.buttons['pause'].is_sensitive() and not app.buttons['play'].is_sensitive()

        def snapshot(variable):
            if destination := os.environ.get(variable):
                paintable = Gtk.WidgetPaintable.new(app.window)
                image = Gtk.Snapshot.new()
                paintable.snapshot(image, app.window.get_width(), app.window.get_height())
                texture = app.window.get_renderer().render_texture(image.to_node(), None)
                assert texture.save_to_png(destination)

        def tick():
            nonlocal media, stage, shown_at
            try:
                if time.monotonic() - started > 8:
                    raise AssertionError(f'Native Music layout timed out at stage {stage}')
                if app.pending or not hasattr(app, 'song'):
                    return True
                if stage == 0:
                    app.view_buttons['music'].set_active(True)
                    shown_at = time.monotonic()
                elif stage == 1:
                    if app.pages.get_transition_running() or app.song.get_width() == 0 or time.monotonic() - shown_at < .5:
                        return True
                    bounded_title()
                    assert app.song.get_text() == title
                    assert not app.song.get_layout().is_ellipsized(), 'Normal track title loses its distinguishing movement'
                    # The full title fits on one or two lines at the Title 1 size.
                    assert app.song.get_layout().get_line_count() in (1, 2)
                    assert app.elapsed.get_text() == '2:00' and app.duration.get_text() == '10:00'
                    assert app.progress.get_visible() and app.progress.get_fraction() == .2
                    assert app.buttons['pause'].get_visible() and not app.buttons['play'].get_visible()
                    assert not app.bluetooth_volume.get_visible() and not app.audio_controls.get_visible()
                    snapshot('HEADUNIT_MUSIC_PAGE_SNAPSHOT')
                    media = validate_media({**media, 'title': extreme_title})
                    app.refresh()
                elif stage == 2:
                    bounded_title()
                    assert app.song.get_text() == extreme_title and not app.song.get_use_markup()
                    assert app.song.get_layout().get_line_count() == 2 and app.song.get_layout().is_ellipsized()
                    media = validate_media({'available': False})
                    app.refresh()
                elif stage == 3:
                    assert app.song.get_text() == 'Nothing playing'
                    assert app.artist.get_text() == 'Your music will appear here.'
                    assert app.album.get_text() == '' and not app.album.get_visible()
                    assert app.cover_stack.get_visible_child_name() == 'empty'
                    assert app.progress.get_fraction() == 0
                    assert not app.timeline.get_visible() and not app.transport.get_visible()
                    assert app.elapsed.get_text() == app.duration.get_text() == '—:—'
                    assert not any(button.is_sensitive() for button in app.buttons.values())
                    snapshot('HEADUNIT_MUSIC_EMPTY_SNAPSHOT')
                    media = validate_media({'available': True})
                    app.refresh()
                elif stage == 4:
                    assert app.song.get_text() == 'Unknown title' and app.song.get_layout().get_line_count() == 1
                    assert app.artist.get_text() == '' and not app.album.get_visible()
                    assert not any(button.is_sensitive() for button in app.buttons.values())
                    assert not app.timeline.get_visible()
                    media = validate_media({'available': True, 'title': 'A paused track', 'status': 'paused',
                                            'position_ms': 3601000, 'controls': ['play', 'pause', 'next', 'previous']})
                    app.refresh()
                elif stage == 5:
                    assert app.buttons['play'].get_visible() and not app.buttons['pause'].get_visible()
                    assert app.elapsed.get_text() == '1:00:01' and app.duration.get_text() == '—:—'
                    assert app.timeline.get_visible() and not app.progress.get_visible()
                    snapshot('HEADUNIT_MUSIC_PAUSED_SNAPSHOT')
                    media = validate_media({'available': True, 'duration_ms': 600000})
                    app.refresh()
                elif stage == 6:
                    assert app.elapsed.get_text() == '—:—' and app.duration.get_text() == '10:00'
                    assert not app.progress.get_visible() and not app.transport.get_visible()
                    before = len(music_calls)
                    app.media_action('play') # Missing capability cannot dispatch, even with an active phone.
                    assert len(music_calls) == before and not app.pending
                    media = validate_media({'available': True, 'position_ms': 0, 'duration_ms': 0,
                                            'controls': ['play', 'pause', 'volume_down']})
                    app.refresh()
                elif stage == 7:
                    assert app.buttons['play'].get_visible() and app.buttons['pause'].get_visible()
                    assert app.bluetooth_volume.get_visible() and app.buttons['volume_down'].get_visible()
                    assert not app.buttons['volume_up'].get_visible()
                    assert app.elapsed.get_text() == '0:00' and app.duration.get_text() == '—:—'
                    assert not app.progress.get_visible() # A zero duration is not a known track length.
                    app.quit()
                    return False
                stage += 1
            except Exception as error:
                errors.append(error); app.quit(); return False
            return True

        GLib.timeout_add(100, tick)
        try:
            result = app.run([])
            assert result == 0 and stage == 7 and not errors, (result, stage, errors)
        finally:
            server.shutdown(); server.server_close(); worker.join(timeout=2)
        print('Native GTK Music: bounded two-line metadata, touchscreen-visible capability-aware transport, observed/unknown timeline, track/source clearing and unavailable control rejection passed (synthetic private manager; no phone)')


run_music_layout_checks()
