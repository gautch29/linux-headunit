# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real GTK pairing UI against an isolated fake manager; no Bluetooth calls."""
import copy
import json
import os
from pathlib import Path
import socketserver
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_page import PhonePage, GLib, Gtk, pairing_snapshot


ADDRESS = 'AA:BB:CC:DD:EE:01'
SESSION, PROMPT_A, PROMPT_B = '1' * 32, '2' * 32, '3' * 32
LONG_ERROR = 'Pairing window expired: '.ljust(300, 'x')
RECEIVER_ERROR = 'The saved phone is no longer paired; receiver startup failed'


def profile(phase='idle', prompt=None, **changes):
    result = {'supported': True, 'phase': phase, 'session': '' if phase == 'idle' else SESSION,
              'expiresInSeconds': 0 if phase in ('idle', 'paired', 'error', 'stopping') else 90,
              'adapterName': 'Headunit bench', 'prompt': prompt, 'pairedAddress': '', 'error': ''}
    result.update(changes)
    return result


def prompt(identifier=PROMPT_A, passkey='012345'):
    return {'id': identifier, 'deviceName': 'Bench iPhone', 'address': ADDRESS, 'passkey': passkey}


def strict_schema_checks():
    valid = profile('confirmation', prompt())
    assert pairing_snapshot(valid) == valid
    result = pairing_snapshot(valid)
    result['prompt']['id'] = PROMPT_B
    assert valid['prompt']['id'] == PROMPT_A  # Snapshot does not retain mutable prompt identity.
    assert pairing_snapshot(profile('error', error=LONG_ERROR))['error'] == LONG_ERROR
    for key, value in [('supported', 1), ('phase', 'connected'), ('session', 'x' * 32),
                       ('expiresInSeconds', True), ('expiresInSeconds', 121),
                       ('adapterName', 'unsafe\nname'), ('pairedAddress', 'invalid'), ('error', '\0'), ('error', 'x' * 301)]:
        invalid = {**valid, key: value}
        try:
            pairing_snapshot(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError((key, value))
    for invalid in ({**valid, 'extra': True}, {k: v for k, v in valid.items() if k != 'prompt'},
                    {**valid, 'prompt': {**prompt(), 'passkey': '12345'}},
                    {**valid, 'prompt': {**prompt(), 'passkey': '１２３４５６'}},
                    {**valid, 'prompt': {**prompt(), 'extra': True}}):
        try:
            pairing_snapshot(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(invalid)


class Fixture:
    def __init__(self):
        self.lock = threading.Lock()
        self.pairing = None
        self.active = None
        self.error = ''
        self.loss = False
        self.phones = []
        self.calls = []
        self.status_gate = None
        self.status_waiting = threading.Event()

    def update(self, pairing, *, active=None, error='', loss=False, phones=None):
        with self.lock:
            self.pairing = copy.deepcopy(pairing)
            self.active, self.error, self.loss = active, error, loss
            if phones is not None:
                self.phones = copy.deepcopy(phones)

    def commands(self):
        with self.lock:
            return [dict(call) for call in self.calls if call['action'] != 'status']

    def response(self, value):
        with self.lock:
            self.calls.append(dict(value))
            action = value['action']
            if action == 'status':
                assert set(value) == {'action'}
                if self.loss:
                    return {'ok': False, 'error': 'Fixture manager is unavailable'}
                result = {'ok': True, 'phones': copy.deepcopy(self.phones), 'active': self.active,
                          'active_mode': None, 'generation': 1, 'error': self.error,
                          'carplay': {'state': 'ready' if self.active else 'disconnected', 'returnReady': bool(self.active)},
                          'media': {'available': False, 'controls': []}}
                if self.pairing is not None:
                    result['pairing'] = copy.deepcopy(self.pairing)
                return result
            if action == 'pairing_start':
                assert set(value) == {'action'} and not self.active
                self.pairing = profile('discoverable')
            elif action == 'pairing_cancel':
                assert set(value) == {'action', 'session'} and value['session'] == self.pairing['session']
                assert self.pairing['phase'] in ('starting', 'discoverable', 'confirmation')
                self.pairing = profile()
            elif action == 'pairing_confirm':
                assert set(value) == {'action', 'session', 'prompt', 'accepted'}
                assert type(value['accepted']) is bool
                assert value['session'] == self.pairing['session'] and value['prompt'] == self.pairing['prompt']['id']
                # An accepted/rejected request is deliberately not a completed bond.
            else:
                raise AssertionError(action)
            return {'ok': True, 'message': 'Pairing request accepted'}


def run_native_checks():
    fixture = Fixture()

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            data = json.loads(self.rfile.readline(4096))
            reply = fixture.response(data)
            if data['action'] == 'status' and fixture.status_gate is not None:
                fixture.status_waiting.set()
                assert fixture.status_gate.wait(3), 'Held pairing status was not released'
            self.wfile.write(json.dumps(reply).encode() + b'\n')

    with tempfile.TemporaryDirectory(prefix='hu-pair-') as folder:
        path = Path(folder).resolve() / 'manager.sock'
        server = socketserver.UnixStreamServer(str(path), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        app = PhonePage(path)
        errors, stage, widget_ids, phone_row = [], 0, None, None
        started = time.monotonic()

        def refresh(value, **changes):
            fixture.update(value, **changes)
            app.refresh()

        def no_command(callback):
            before = fixture.commands()
            callback()
            assert fixture.commands() == before

        def blocked_phone_actions():
            before = fixture.commands()
            pending, notice = app.pending, app.notice.get_text()
            windows = Gtk.Window.get_toplevels().get_n_items()
            assert not app.listbox.get_sensitive(), 'Saved-phone actions remain enabled while pairing owns the receiver'
            assert all(not phone_row[key].is_sensitive() for key in ('connect', 'remove'))
            # Stale programmatic callbacks must follow the same current owner
            # phase as physical taps, even though GTK suppresses disabled taps.
            phone_row['connect'].emit('clicked')
            phone_row['remove'].emit('clicked')
            for value in ({'action': 'connect', 'phone': ADDRESS},
                          {'action': 'disconnect', 'phone': ADDRESS, 'generation': 1},
                          {'action': 'mode', 'phone': ADDRESS, 'mode': 'ultra'},
                          {'action': 'forget', 'phone': ADDRESS}):
                app.action(value)
            assert Gtk.Window.get_toplevels().get_n_items() == windows
            assert fixture.commands() == before
            assert app.pending == pending and app.notice.get_text() == notice

        def tick():
            nonlocal stage, widget_ids, phone_row
            try:
                if time.monotonic() - started > 15:
                    raise AssertionError(f'Pairing UI fixture timed out at stage {stage}')
                if (app.pending and stage != 36) or not hasattr(app, 'listbox') or not app.listbox.get_first_child():
                    return True
                if stage == 0:
                    assert (app.window.get_width(), app.window.get_height()) == (1280, 720)
                    # Home is the landing view; pairing lives on Phones.
                    assert app.pages.get_visible_child_name() == 'home'
                    assert not app.add_phone.get_visible()
                    app.view_buttons['phones'].set_active(True)
                    assert not app.add_phone.get_visible() and not app.pairing_card.get_visible()
                    assert app.listbox.get_first_child().get_first_child().get_text() == 'No paired phones'
                    assert 'Bluetooth settings' in app.listbox.get_first_child().get_last_child().get_text()
                    refresh({'supported': False})
                elif stage == 1:
                    assert not app.add_phone.get_visible() and not app.pairing_card.get_visible()
                    assert 'Bluetooth settings' in app.listbox.get_first_child().get_last_child().get_text()
                    refresh(profile())
                elif stage == 2:
                    assert app.add_phone.get_visible() and app.add_phone.get_sensitive()
                    assert not app.pairing_card.get_visible()
                    assert app.listbox.get_first_child().get_last_child().get_text() == 'Tap Add phone to pair your iPhone.'
                    refresh({'supported': False}); stage = 42; return True
                elif stage == 42:
                    assert not app.status['phones'] and not app.add_phone.get_visible()
                    assert 'Bluetooth settings' in app.listbox.get_first_child().get_last_child().get_text()
                    assert not fixture.commands()
                    refresh(profile()); stage = 43; return True
                elif stage == 43:
                    assert not app.status['phones'] and app.add_phone.get_sensitive()
                    assert app.listbox.get_first_child().get_last_child().get_text() == 'Tap Add phone to pair your iPhone.'
                    app.add_phone.emit('clicked')
                    # Deliberately repeat an emitted click; a pending operation cannot queue another start.
                    app.add_phone.emit('clicked')
                    stage = 3; return True
                elif stage == 3:
                    assert fixture.commands() == [{'action': 'pairing_start'}]
                    assert app.pairing_card.get_visible() and not app.add_phone.get_sensitive()
                    assert app.pairing_adapter.get_text() == 'Headunit bench'
                    assert app.pairing_time.get_text() == '90 s remaining'
                    assert 'Settings → Bluetooth' in app.pairing_note.get_text()
                    assert 'Headunit bench' in app.pairing_note.get_text()
                    assert app.pairing_buttons['cancel'].get_sensitive()
                    assert not app.pairing_buttons['pair'].get_visible()
                    refresh(profile('confirmation', prompt()))
                elif stage == 4:
                    assert app.pairing_title.get_text() == 'Check the pairing code'
                    assert app.pairing_device.get_text() == 'Bench iPhone'
                    assert app.pairing_code.get_text() == '012345'
                    assert 'only if they match' in app.pairing_note.get_text()
                    assert app.pairing_buttons['pair'].get_sensitive() and app.pairing_buttons['reject'].get_sensitive()
                    widget_ids = tuple(app.pairing_buttons.values())
                    # Optional visual artifact from the actual native fixture,
                    # without taking a screenshot of other desktop contents.
                    if destination := os.environ.get('HEADUNIT_PAIRING_SCREENSHOT'):
                        from gi.repository import Gsk
                        paintable = Gtk.WidgetPaintable.new(app.window)
                        snapshot = Gtk.Snapshot.new()
                        paintable.snapshot(snapshot, app.window.get_width(), app.window.get_height())
                        node = snapshot.to_node()
                        assert isinstance(node, Gsk.RenderNode)
                        texture = app.window.get_renderer().render_texture(node, None)
                        assert texture.save_to_png(destination)
                    # A code mismatch is an explicit negative decision, never an automatic Pair.
                    app.pairing_buttons['reject'].emit('clicked')
                elif stage == 5:
                    assert fixture.commands()[-1] == {'action': 'pairing_confirm', 'session': SESSION,
                                                     'prompt': PROMPT_A, 'accepted': False}
                    assert app.pairing_title.get_text() == 'Rejecting pairing…'
                    assert not app.pairing_buttons['pair'].get_sensitive()
                    assert not app.pairing_buttons['reject'].get_sensitive()
                    assert app.pairing_buttons['cancel'].get_sensitive()
                    app.refresh()
                elif stage == 6:
                    assert len(fixture.commands()) == 2 and tuple(app.pairing_buttons.values()) == widget_ids
                    no_command(lambda: app.pairing_buttons['pair'].emit('clicked'))
                    refresh(profile('confirmation', prompt(PROMPT_B, '654321')))
                elif stage == 7:
                    assert app.pairing_title.get_text() == 'Check the pairing code'
                    assert app.pairing_code.get_text() == '654321'
                    assert tuple(app.pairing_buttons.values()) == widget_ids
                    assert app.pairing_buttons['pair'].get_sensitive()
                    app.pairing_buttons['pair'].emit('clicked')
                    app.pairing_buttons['pair'].emit('clicked')
                elif stage == 8:
                    assert fixture.commands()[-1] == {'action': 'pairing_confirm', 'session': SESSION,
                                                     'prompt': PROMPT_B, 'accepted': True}
                    assert len(fixture.commands()) == 3
                    assert app.pairing_title.get_text() == 'Finishing pairing…'
                    assert 'Waiting for the phone' in app.pairing_note.get_text()
                    assert not app.pairing_buttons['pair'].get_sensitive()
                    assert not app.status['phones'] and app.status['active'] is None
                    # The backend may clear the answered prompt and return to
                    # discovery while it waits for the actual trusted bond.
                    refresh(profile('discoverable'))
                elif stage == 9:
                    assert app.pairing_title.get_text() != 'Phone paired' and len(fixture.commands()) == 3
                    assert app.pairing_title.get_text() == 'Finishing pairing…'
                    phone = {'address': ADDRESS, 'name': 'Bench iPhone', 'connected': False,
                             'active': False, 'selected': False}
                    refresh(profile('paired', pairedAddress=ADDRESS), phones=[phone])
                elif stage == 10:
                    assert app.pairing_title.get_text() == 'Phone paired'
                    assert 'Connect your iPhone' in app.pairing_note.get_text()
                    assert 'Standard' not in app.pairing_note.get_text() and 'Ultra' not in app.pairing_note.get_text()
                    assert not app.pairing_buttons['pair'].get_visible() and not app.pairing_buttons['cancel'].get_visible()
                    assert ADDRESS in app.phone_widgets and app.add_phone.get_sensitive()
                    refresh(profile('idle'), active=ADDRESS)
                elif stage == 11:
                    assert not app.add_phone.get_sensitive()
                    no_command(lambda: app.add_phone.emit('clicked'))
                    refresh(profile('starting'))
                elif stage == 12:
                    assert app.pairing_title.get_text() == 'Starting pairing…'
                    assert app.pairing_buttons['cancel'].get_sensitive()
                    app.pairing_buttons['cancel'].emit('clicked')
                elif stage == 13:
                    assert fixture.commands()[-1] == {'action': 'pairing_cancel', 'session': SESSION}
                    assert not app.pairing_card.get_visible() and app.add_phone.get_sensitive()
                    refresh(profile('confirmation', prompt()))
                elif stage == 14:
                    assert app.pairing_buttons['pair'].get_sensitive()
                    refresh(profile('confirmation', prompt()), loss=True)
                elif stage == 15:
                    assert app.pairing_title.get_text() == 'Pairing unavailable'
                    assert not any(button.get_sensitive() for button in app.pairing_buttons.values())
                    assert not app.add_phone.get_sensitive()
                    no_command(lambda: app.pairing_buttons['pair'].emit('clicked'))
                    no_command(lambda: app.pairing_buttons['reject'].emit('clicked'))
                    refresh(profile('confirmation', prompt(PROMPT_B)))
                elif stage == 16:
                    assert app.pairing_buttons['pair'].get_sensitive()
                    refresh(profile('confirmation', prompt(PROMPT_B)), error='Fixture receiver error')
                elif stage == 17:
                    assert all(button.get_sensitive() for button in app.pairing_buttons.values())
                    assert app.notice.get_text() == 'Fixture receiver error'
                    malformed = profile('confirmation', prompt(PROMPT_B))
                    malformed['prompt']['passkey'] = 'invalid'
                    refresh(malformed)
                elif stage == 18:
                    assert not app.add_phone.get_sensitive() and not app.pairing_buttons['pair'].get_sensitive()
                    no_command(lambda: app.pairing_buttons['pair'].emit('clicked'))
                    refresh(profile('confirmation', prompt(PROMPT_B), expiresInSeconds=0))
                elif stage == 19:
                    assert not app.pairing_buttons['pair'].get_sensitive() and not app.pairing_buttons['reject'].get_sensitive()
                    no_command(lambda: app.pairing_buttons['pair'].emit('clicked'))
                    refresh(profile('confirmation', prompt(PROMPT_B)))
                elif stage == 20:
                    assert app.pairing_buttons['pair'].get_sensitive()
                    app.pairing_buttons['cancel'].emit('clicked')
                elif stage == 21:
                    assert not app.pairing_card.get_visible() and app.add_phone.get_sensitive()
                    assert fixture.commands()[-1] == {'action': 'pairing_cancel', 'session': SESSION}
                    refresh(profile('stopping'))
                elif stage == 22:
                    assert app.pairing_buttons['cancel'].get_visible() and not app.pairing_buttons['cancel'].get_sensitive()
                    assert not app.add_phone.get_sensitive()
                    no_command(lambda: app.pairing_buttons['cancel'].emit('clicked'))
                    refresh(profile('error', error=LONG_ERROR))
                elif stage == 23:
                    assert app.pairing_title.get_text() == 'Couldn’t pair' and app.add_phone.get_sensitive()
                    assert app.pairing_note.get_text() == LONG_ERROR
                    assert (app.window.get_width(), app.window.get_height()) == (1280, 720)
                    assert app.pairing_note.measure(Gtk.Orientation.HORIZONTAL, -1)[0] < 1000
                    assert not app.pairing_buttons['cancel'].get_visible() and not app.pairing_buttons['cancel'].get_sensitive()
                    no_command(lambda: app.pairing_buttons['cancel'].emit('clicked'))
                    refresh(profile(supported=False))
                elif stage == 24:
                    assert not app.add_phone.get_visible() and not app.pairing_card.get_visible()
                    assert len(fixture.commands()) == 5
                    # A saved-phone receiver failure remains in Manager.error.
                    # With no phones left, Add is the recovery route; it must
                    # not depend on first starting that impossible receiver.
                    refresh(profile(), error=RECEIVER_ERROR, phones=[])
                elif stage == 25:
                    assert app.add_phone.get_sensitive() and not app.pairing_card.get_visible()
                    assert app.listbox.get_first_child().get_first_child().get_text() == 'No paired phones'
                    assert app.listbox.get_first_child().get_last_child().get_text() == 'Tap Add phone to pair your iPhone.'
                    assert app.notice.get_text() == RECEIVER_ERROR
                    app.add_phone.emit('clicked')
                elif stage == 26:
                    assert len(fixture.commands()) == 6 and fixture.commands()[-1] == {'action': 'pairing_start'}
                    assert app.pairing['phase'] == 'discoverable'
                    assert app.pairing_buttons['cancel'].get_sensitive()
                    assert app.notice.get_text() == RECEIVER_ERROR
                    refresh(profile('confirmation', prompt()), error=RECEIVER_ERROR)
                elif stage == 27:
                    assert all(button.get_sensitive() for button in app.pairing_buttons.values())
                    assert app.notice.get_text() == RECEIVER_ERROR
                    app.pairing_buttons['reject'].emit('clicked')
                elif stage == 28:
                    assert len(fixture.commands()) == 7 and fixture.commands()[-1]['accepted'] is False
                    assert app.pairing_title.get_text() == 'Rejecting pairing…'
                    assert app.pairing_buttons['cancel'].get_sensitive()
                    app.pairing_buttons['cancel'].emit('clicked')
                elif stage == 29:
                    assert len(fixture.commands()) == 8 and fixture.commands()[-1] == {'action': 'pairing_cancel', 'session': SESSION}
                    assert app.add_phone.get_sensitive() and not app.pairing_card.get_visible()
                    assert app.notice.get_text() == RECEIVER_ERROR
                    refresh(profile('paired', pairedAddress=ADDRESS, error=LONG_ERROR))
                elif stage == 30:
                    assert app.pairing_title.get_text() == 'Phone paired'
                    assert 'Connect your iPhone' in app.pairing_note.get_text()
                    assert app.pairing_warning.get_visible()
                    assert app.pairing_warning.get_text() == 'Bluetooth cleanup needs attention: ' + LONG_ERROR
                    assert (app.window.get_width(), app.window.get_height()) == (1280, 720)
                    assert app.pairing_warning.measure(Gtk.Orientation.HORIZONTAL, -1)[0] < 1000
                    assert not app.pairing_buttons['cancel'].get_visible() and not app.pairing_buttons['cancel'].get_sensitive()
                    assert app.add_phone.get_sensitive()
                    refresh(profile('paired', pairedAddress=ADDRESS))
                elif stage == 31:
                    assert app.pairing_title.get_text() == 'Phone paired' and not app.pairing_warning.get_visible()
                    assert app.pairing_warning.get_text() == ''
                    assert len(fixture.commands()) == 8
                    phone = {'address': ADDRESS, 'name': 'Bench saved phone', 'connected': False,
                             'active': False, 'selected': False}
                    refresh(profile('starting'), phones=[phone])
                elif stage == 32:
                    phone_row = app.phone_widgets[ADDRESS]
                    blocked_phone_actions()
                    assert not app.pending and app.pairing_buttons['cancel'].is_sensitive()
                    assert all(button.is_sensitive() for button in app.view_buttons.values())
                    refresh(profile('discoverable'))
                elif stage == 33:
                    assert app.phone_widgets[ADDRESS] is phone_row
                    blocked_phone_actions()
                    assert not app.pending and app.pairing_buttons['cancel'].is_sensitive()
                    refresh(profile('confirmation', prompt()))
                elif stage == 34:
                    blocked_phone_actions()
                    assert not app.pending and all(button.is_sensitive() for button in app.pairing_buttons.values())
                    refresh(profile('stopping'))
                elif stage == 35:
                    blocked_phone_actions()
                    assert not app.pending and not app.pairing_buttons['cancel'].is_sensitive()
                    fixture.status_gate = threading.Event()
                    refresh(profile())
                elif stage == 36:
                    if not fixture.status_waiting.is_set():
                        return True
                    assert app.pending and app.pairing['phase'] == 'stopping'
                    blocked_phone_actions()
                    fixture.status_gate.set()
                elif stage == 37:
                    fixture.status_gate = None
                    assert app.pairing['phase'] == 'idle'
                    assert app.listbox.is_sensitive() and app.add_phone.is_sensitive()
                    assert all(app.phone_widgets[ADDRESS][key].is_sensitive() for key in ('connect', 'remove'))
                    refresh(profile('paired', pairedAddress=ADDRESS))
                elif stage == 38:
                    assert app.listbox.is_sensitive()
                    refresh(profile('error', error='Fixture pairing error'))
                elif stage == 39:
                    assert app.listbox.is_sensitive()
                    refresh(None)
                elif stage == 40:
                    assert app.listbox.is_sensitive() and not app.add_phone.get_visible()
                    refresh(profile(), loss=True)
                elif stage == 41:
                    assert not app.listbox.is_sensitive() and not app.add_phone.is_sensitive()
                    assert len(fixture.commands()) == 8
                    app.quit()
                    return False
                stage += 1
            except Exception as error:
                errors.append(error)
                app.quit()
                return False
            return True

        GLib.timeout_add(70, tick)
        try:
            code = app.run([])
            assert code == 0 and stage == 41 and not errors, (code, stage, errors)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)
        print('Native GTK pairing: guided discovery, explicit codes, prompt identity, pending versus bonded, cancel, receiver-error recovery, paired cleanup warning, saved-phone action exclusion in all active phases, held-status/stale callbacks, fresh idle recovery, older manager and failure states passed')


if __name__ == '__main__':
    strict_schema_checks()
    run_native_checks()
