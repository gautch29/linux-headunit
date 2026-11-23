# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real GTK and asynchronous IPC; phone, renderer and layers are explicit fixtures."""
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
from phone_page import PhonePage, GLib
from phone_page_presentation import ResidentPresentation
from native_phone_page_resident_test import (
    ADDRESS, OUTPUT, Monitor, Monitors, LayerBoundary, HandoffBoundary,
    ManagerFixture, camera, configuration, destroy_source,
)


class ShortcutManager(ManagerFixture):
    def __init__(self, mode, app):
        super().__init__()
        self.mode = mode
        self.publisher = {'nonce': 'a' * 32, 'connectionGeneration': '1'}
        self.allowed_action = {'action': 'phone_app', 'app': app, 'publisher': dict(self.publisher)}
        self.hold_status_after_action = False
        self.ack_entered = threading.Event()
        self.ack_release = threading.Event()

    def reply(self, value):
        if value['action'] == 'phone_app':
            self.ack_entered.set()
            assert not self.presentation.foreground and not self.presentation.handoff.acquired, 'App sent while Home owns main'
            assert self.carplay == {'state': 'ready', 'returnReady': True}, 'App sent before renderer readiness'
            assert self.ack_release.wait(3), 'App acknowledgment was not released'
        reply = super().reply(value)
        if value['action'] == 'status' and reply.get('ok'):
            reply['active_mode'] = 'ultra'
            reply['ultra_cluster'] = dict(self.publisher, active='', pending='', ready=True,
                                          idle=True, transitioning=False, error='', choices=[]) if self.carplay['returnReady'] else None
        elif self.mode == 'rejected':
            reply = {'ok': False, 'error': 'Fixture rejects app request'}
        return reply


def run_case(app_name, mode):
    fixture = ShortcutManager(mode, app_name)
    model, layers, handoff = Monitors([Monitor(OUTPUT)]), LayerBoundary(), HandoffBoundary()
    presentation = ResidentPresentation(layers, configuration(), handoff=handoff,
        display=SimpleNamespace(get_monitors=lambda: model, sync=lambda: None))
    fixture.presentation = presentation
    with tempfile.TemporaryDirectory(prefix='hu-shortcuts-') as folder:
        path = Path(folder) / 'manager.sock'

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                command = json.loads(self.rfile.readline(4096))
                reply = fixture.reply(command)
                self.wfile.write(json.dumps(reply).encode() + b'\n')
                fixture.completed.set()

        server = socketserver.UnixStreamServer(str(path), Handler)
        server.timeout = .1
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .02}, daemon=True)
        thread.start()
        app = PhonePage(path, presentation=presentation)
        errors, stage, started = [], 0, time.monotonic()

        def tick():
            nonlocal stage
            try:
                assert time.monotonic() - started < 6, (app_name, mode, stage)
                if not hasattr(app, 'home'):
                    return True
                if stage == 0:
                    if app.pending or not app.status_current():
                        return True
                    destroy_source(app.poll_source)
                    app.poll_source = 0
                    presentation.open()
                    assert handoff.acquired and app.can_return()
                    fixture.hold = True
                    fixture.entered.clear()
                    fixture.release.clear()
                    fixture.update(carplay={'state': 'home', 'returnReady': True})
                    app.home.app_buttons[app_name].emit('clicked')
                    assert app.pending_action == 'status' and not handoff.acquired
                    app.home.app_buttons[app_name].emit('clicked')
                    stage = 1
                elif stage == 1:
                    if not fixture.entered.is_set():
                        return True
                    assert not handoff.acquired and not presentation.foreground
                    assert not fixture.ack_entered.is_set(), 'App request preceded fresh presentation status'
                    assert not any(b.get_sensitive() for b in app.home.app_buttons.values())
                    if mode == 'publisher-changed':
                        fixture.publisher = dict(fixture.publisher, nonce='b' * 32)
                    elif mode == 'phone-changed':
                        fixture.update(active='AA:BB:CC:DD:EE:02')
                    elif mode == 'camera':
                        fixture.update(value=camera(True, True, 'active'))
                    elif mode in ('not-ready', 'delayed-ready'):
                        fixture.update(carplay={'state': 'starting', 'returnReady': False})
                    elif mode == 'navigate':
                        app.view_buttons['music'].set_active(True)
                    elif mode == 'closed':
                        app.close_ui()
                    else:
                        fixture.update()
                    fixture.hold = False
                    fixture.release.set()
                    stage = 2
                elif stage == 2:
                    if mode == 'closed':
                        if not fixture.completed.is_set():
                            return True
                        assert app.shortcut_launch is None
                        app.quit()
                        return False
                    if app.pending_action == 'status':
                        return True
                    if mode in ('publisher-changed', 'phone-changed', 'camera', 'navigate'):
                        assert app.shortcut_launch is None and not fixture.ack_entered.is_set()
                        app.quit()
                        return False
                    if mode == 'not-ready':
                        if app.shortcut_launch is not None:
                            return True
                        assert not fixture.ack_entered.is_set(), 'Expired intent dispatched an app'
                        app.quit()
                        return False
                    if mode == 'delayed-ready' and not fixture.ack_entered.is_set():
                        assert app.shortcut_launch is not None
                        fixture.update()
                        return True
                    if not fixture.ack_entered.is_set():
                        return True
                    assert not handoff.acquired and app.pending_action == 'phone_app'
                    fixture.ack_release.set()
                    stage = 3
                elif stage == 3:
                    if app.pending:
                        return True
                    assert app.shortcut_launch is None and not handoff.acquired
                    assert layers.windows[app.window]['layer'] == 'bottom'
                    assert [c for c in fixture.calls if c['action'] != 'status'] == [fixture.allowed_action]
                    app.quit()
                    return False
            except Exception as error:
                errors.append(error)
                fixture.ack_release.set()
                fixture.release.set()
                app.quit()
                return False
            return True

        source = GLib.timeout_add(30, tick)
        try:
            code = app.run([])
        finally:
            fixture.ack_release.set()
            fixture.release.set()
            destroy_source(source)
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
        assert code == 0 and not errors and stage in (2, 3), (app_name, mode, stage, errors)
        assert app.ui_retired and presentation.retired and not app.get_windows()


if __name__ == '__main__':
    for app_name in ('maps', 'music', 'phone'):
        run_case(app_name, 'success')
    for mode in ('rejected', 'publisher-changed', 'phone-changed', 'camera', 'not-ready', 'delayed-ready', 'navigate', 'closed'):
        run_case('music', mode)
    print('Eleven GTK app shortcut cases passed: restore-before-dispatch, held/delayed readiness, deadline, duplicate tap, rejection, '
          'phone/publisher change, camera, readiness, navigation and closed-page retirement. '
          'Phone, layer and renderer boundaries are simulated.')
