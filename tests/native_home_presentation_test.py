#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Production Home transitions with explicit GTK, clock and lease substitutes.

These checks qualify ordering and containment, not GPU/compositor pixels.
The independent Wayland/GpuDisplay fixture supplies that evidence.
"""
import sys
from pathlib import Path
import types
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_page_presentation import ResidentPresentation, carplay_snapshot, status_fresh, recovery_phone


class Handoff:
    def __init__(self, events):
        self.events = events
        self.ready = self.requested = self.acquired = False

    def request(self):
        self.events.append('request'); self.requested = True

    def poll(self):
        self.events.append('poll')
        self.acquired = self.ready
        if self.acquired: self.events.append('withdrawal-ack')
        return self.acquired

    def release(self):
        self.events.append('off-and-release')
        self.requested = self.acquired = False


class Window:
    def __init__(self, name, events):
        self.name, self.events, self.visible = name, events, True

    def set_visible(self, value):
        self.visible = value; self.events.append(self.name + ('-show' if value else '-hide'))

    def get_visible(self): return self.visible
    def present(self): self.events.append(self.name + '-present')
    def destroy(self): self.events.append(self.name + '-destroy')


class RecoveryTests(unittest.TestCase):
    def test_only_complete_bounded_recovery_identifies_a_cancellable_phone(self):
        phone = 'AA:BB:CC:DD:EE:01'
        self.assertEqual(recovery_phone({'phone': phone, 'remainingAttempts': 3}), phone)
        for value in (None, {}, {'phone': phone}, {'phone': phone, 'remainingAttempts': True},
                      {'phone': phone, 'remainingAttempts': 0}, {'phone': phone, 'remainingAttempts': 4},
                      {'phone': None, 'remainingAttempts': 1},
                      {'phone': phone.lower(), 'remainingAttempts': 1},
                      {'phone': phone, 'remainingAttempts': 1, 'ready': True}):
            with self.subTest(value=value):
                self.assertIsNone(recovery_phone(value))


    def recovery_diagnostics(self):
        return {'schedule': {'authority': 'software_scheduling_intent', 'configuredMaximum': 3,
                             'queued': True, 'state': 'queued', 'clockDomain': 'python-time.monotonic',
                             'observedMonotonicSeconds': 100.0, 'deadlineMonotonicSeconds': 102.0,
                             'remainingWaitSeconds': 2.0},
                'decisions': [{'decision': 'queued', 'generation': 2, 'configuredMaximum': 3,
                               'remainingAttempts': 3, 'exitCode': 7,
                               'clockDomain': 'python-time.monotonic', 'observedMonotonicSeconds': 100.0}]}

    def test_declared_diagnostics_never_supply_current_phone_or_budget(self):
        from copy import deepcopy
        phone = 'AA:BB:CC:DD:EE:01'
        diagnostics = self.recovery_diagnostics()
        for fields in ({}, {'schedule': diagnostics['schedule']},
                       {'decisions': diagnostics['decisions']}, diagnostics):
            for remaining in (1, 2, 3):
                payload = {'phone': phone, 'remainingAttempts': remaining, **deepcopy(fields)}
                original = deepcopy(payload)
                self.assertEqual(recovery_phone(payload), phone)
                self.assertEqual(payload, original)
            for core in ({}, {'phone': None, 'remainingAttempts': 0},
                         {'phone': phone, 'remainingAttempts': 0},
                         {'phone': phone, 'remainingAttempts': True},
                         {'phone': phone, 'remainingAttempts': 1.0},
                         {'phone': phone.lower(), 'remainingAttempts': 1}):
                self.assertIsNone(recovery_phone({**core, **deepcopy(fields)}))
        for records in ([], diagnostics['decisions'] * 8):
            self.assertEqual(recovery_phone({'phone': phone, 'remainingAttempts': 1,
                                              'decisions': records}), phone)
        for phone_value in ('AA:BB:CC:DD:EE', 'AA-BB-CC-DD-EE-01', 'AA:BB:CC:DD:EE:GG', 1, ''):
            self.assertIsNone(recovery_phone({'phone': phone_value, 'remainingAttempts': 1,
                                               **diagnostics}))
        # Internally contradictory but typed diagnostics are observations only.
        # They cannot replace the admitted current budget, phone or readiness.
        diagnostics['schedule'].update(queued=False, state='idle', configuredMaximum=0,
                                       clockDomain=None, observedMonotonicSeconds=None,
                                       deadlineMonotonicSeconds=None, remainingWaitSeconds=None)
        diagnostics['decisions'][0].update(decision='cancelled', remainingAttempts=0,
                                           clockDomain=None, observedMonotonicSeconds=None)
        self.assertEqual(recovery_phone({'phone': phone, 'remainingAttempts': 1, **diagnostics}), phone)
        self.assertIsNone(recovery_phone({'phone': None, 'remainingAttempts': 0, **diagnostics}))

    def test_malformed_optional_diagnostics_and_unknown_fields_are_rejected(self):
        from copy import deepcopy
        core = {'phone': 'AA:BB:CC:DD:EE:01', 'remainingAttempts': 1}
        for key, bad in (('schedule', None), ('schedule', []), ('decisions', None),
                         ('decisions', {}), ('decisions', [None]),
                         ('decisions', self.recovery_diagnostics()['decisions'] * 9),
                         ('unknown', True)):
            self.assertIsNone(recovery_phone({**core, key: bad}))
        for section, changes in (
                ('schedule', {'authority': 'ready', 'configuredMaximum': True, 'queued': 1,
                              'state': 'ready', 'clockDomain': 'wall-clock',
                              'observedMonotonicSeconds': True, 'deadlineMonotonicSeconds': float('nan'),
                              'remainingWaitSeconds': -1.0, 'phone': core['phone']}),
                ('decisions', {'decision': 'ready', 'generation': True, 'configuredMaximum': 4,
                               'remainingAttempts': -1, 'exitCode': True, 'clockDomain': {},
                               'observedMonotonicSeconds': float('inf'), 'phone': core['phone']})):
            for key, bad in changes.items():
                with self.subTest(section=section, key=key):
                    payload = {**core, **deepcopy(self.recovery_diagnostics())}
                    target = payload[section] if section == 'schedule' else payload[section][0]
                    target[key] = bad
                    self.assertIsNone(recovery_phone(payload))
            original = self.recovery_diagnostics()[section]
            fields = original if section == 'schedule' else original[0]
            for key in fields:
                payload = {**core, **deepcopy(self.recovery_diagnostics())}
                target = payload[section] if section == 'schedule' else payload[section][0]
                del target[key]
                self.assertIsNone(recovery_phone(payload))

        for key, bad in (('generation', -1), ('generation', 2 ** 64),
                         ('exitCode', 2 ** 31), ('exitCode', -(2 ** 31) - 1),
                         ('exitCode', 7.0), ('remainingAttempts', True)):
            payload = {**core, **deepcopy(self.recovery_diagnostics())}
            payload['decisions'][0][key] = bad
            self.assertIsNone(recovery_phone(payload))


class PhoneHomeTests(unittest.TestCase):
    def test_phone_car_icon_opens_home_once_per_increase_on_one_connection(self):
        page = types.SimpleNamespace()
        request = lambda value: ResidentPresentation._phone_home_requested(page, value)
        self.assertFalse(request({'generation': 1, 'connection': '4', 'requests': 0}))  # baseline
        self.assertTrue(request({'generation': 1, 'connection': '4', 'requests': 1}))
        self.assertFalse(request({'generation': 1, 'connection': '4', 'requests': 1}))
        self.assertFalse(request({'generation': 1, 'connection': '5', 'requests': 2}))  # new connection
        self.assertTrue(request({'generation': 1, 'connection': '5', 'requests': 3}))
        for invalid in (None, {}, {'generation': 1, 'connection': '5', 'requests': -1},
                        {'generation': '1', 'connection': '5', 'requests': 4},
                        {'generation': 1, 'connection': 5, 'requests': 4, 'extra': 1}):
            self.assertFalse(request(invalid))


class Tests(unittest.TestCase):
    def setUp(self):
        self.events, self.notices = [], []
        self.handoff = Handoff(self.events)
        self.configuration = {'enabled': True, 'output': 'HDMI-A-1',
                              'homeButton': {'corner': 'bottom-right', 'margin': 16, 'size': 64}}
        api = types.SimpleNamespace(
            Layer=types.SimpleNamespace(OVERLAY='overlay', BOTTOM='bottom'),
            KeyboardMode=types.SimpleNamespace(ON_DEMAND='demand', NONE='none'),
            set_layer=lambda window, layer: self.events.append('layer-' + layer),
            set_keyboard_mode=lambda window, mode: self.events.append('keyboard-' + mode))
        self.value = ResidentPresentation(api, self.configuration, handoff=self.handoff)
        page = Window('page', self.events)
        home = Window('home', self.events)
        self.value.page = types.SimpleNamespace(window=page, notice=types.SimpleNamespace(set_text=self.notices.append),
                                               fail_resident=lambda error: self.events.append('failure:' + error))
        self.value.home_window = home
        self.value.home_button = types.SimpleNamespace(set_sensitive=lambda value: None)
        self.value.available = self.value.camera_known = True
        self.value.camera_blocked = False
        self.value.carplay_known = self.value.return_ready = True
        self.value.last_status = time.monotonic()
        self.value._output_current = lambda: True
        self.value.display = types.SimpleNamespace(sync=lambda: self.events.append('compositor-sync'))
        source = types.SimpleNamespace(destroy=lambda: self.events.append('source-destroy'))
        self.value.GLib = types.SimpleNamespace(timeout_add=lambda ms, callback: 101,
            MainContext=types.SimpleNamespace(default=lambda: types.SimpleNamespace(find_source_by_id=lambda _: source)))

    def test_missing_coordinated_owner_rejects_before_surfaces(self):
        with self.assertRaisesRegex(ValueError, 'coordinated'):
            ResidentPresentation(self.value.layer_shell, self.configuration)

    def test_one_click_waits_for_real_withdrawal_before_covering_main(self):
        self.value.open()
        self.assertTrue(self.value.opening)
        self.assertFalse(self.value.foreground)
        self.assertNotIn('layer-overlay', self.events)
        self.assertFalse(self.value.home_window.visible)
        self.value.open()  # No second request or second callback is needed.
        self.assertEqual(self.events.count('request'), 1)
        self.handoff.ready = True
        self.assertTrue(self.value._handoff_tick())
        self.assertTrue(self.value.foreground)
        self.assertFalse(self.value.opening)
        self.assertLess(self.events.index('withdrawal-ack'), self.events.index('layer-overlay'))
        self.value.open()
        self.assertEqual(self.events.count('request'), 1)

    def test_back_confirms_lowering_before_releasing_main(self):
        self.handoff.ready = True
        self.value.open(); self.events.clear()
        self.value.back()
        self.assertFalse(self.value.foreground or self.value.opening)
        self.assertTrue(self.value.home_window.visible)
        self.assertLess(self.events.index('page-hide'), self.events.index('layer-bottom'))
        self.assertLess(self.events.index('page-show'), self.events.index('compositor-sync'))
        self.assertLess(self.events.index('compositor-sync'), self.events.index('off-and-release'))
        self.events.clear(); self.value.back()
        self.assertNotIn('layer-bottom', self.events)

    def test_back_cancels_a_pending_request_without_mapping_overlay(self):
        self.value.open(); self.value.back()
        self.assertFalse(self.value.opening or self.handoff.requested)
        self.assertNotIn('layer-overlay', self.events)
        self.assertFalse(self.value._handoff_tick())

    def test_held_home_renews_ownership_without_remapping(self):
        self.handoff.ready = True
        self.value.open(); self.events.clear()
        for _ in range(100): self.assertTrue(self.value._handoff_tick())
        self.assertEqual(self.events.count('poll'), 100)
        self.assertNotIn('layer-overlay', self.events)
        self.assertTrue(self.value.foreground)

    def test_sync_failure_cannot_release_over_a_possibly_visible_overlay(self):
        self.handoff.ready = True
        self.value.open(); self.events.clear()
        def fail(): raise RuntimeError('Deliberate roundtrip failure')
        self.value.display.sync = fail
        self.value.back()
        self.assertNotIn('off-and-release', self.events)
        self.assertTrue(self.handoff.acquired)
        self.assertTrue(any(event.startswith('failure:') for event in self.events))

    def test_camera_or_manager_loss_withdraws_home_before_release(self):
        for method in ('camera_requested', 'manager_lost'):
            with self.subTest(method=method):
                self.setUp(); self.handoff.ready = True; self.value.open(); self.events.clear()
                getattr(self.value, method)()
                self.assertFalse(self.value.foreground)
                self.assertLess(self.events.index('compositor-sync'), self.events.index('off-and-release'))
                self.assertFalse(self.value.home_window.visible)

    def test_timeout_keeps_carplay_uncovered_and_cancels_request(self):
        self.value.last_status = 99
        with mock.patch('phone_page_presentation.time.monotonic', return_value=100): self.value.open()
        with mock.patch('phone_page_presentation.time.monotonic', return_value=104):
            self.assertFalse(self.value._handoff_tick())
        self.assertFalse(self.value.opening or self.value.foreground or self.handoff.requested)
        self.assertNotIn('layer-overlay', self.events)
        self.assertTrue(self.notices)

    def status(self, state='ready', ready=True):
        return {'camera': {'configured': False, 'requested': False, 'phase': 'unavailable', 'error': ''},
                'carplay': {'state': state, 'returnReady': ready}}

    def test_status_observation_bound_is_shared_and_never_renewed_by_completion(self):
        with mock.patch('phone_page_presentation.time.monotonic', return_value=104):
            self.value.update_status(self.status(), observed_at=100)
            self.assertEqual(self.value.last_status, 100)
            self.assertTrue(self.value.home_window.visible)
        with mock.patch('phone_page_presentation.time.monotonic', return_value=105.01):
            self.value.update_status(self.status(), observed_at=100)
            self.assertFalse(self.value.carplay_known or self.value.return_ready)
            self.assertFalse(self.value.home_window.visible)
        with mock.patch('phone_page_presentation.time.monotonic', return_value=100):
            for invalid in (None, True, '100', 101, 94, float('inf'), float('nan')):
                self.assertFalse(status_fresh(invalid))
            self.value.update_status(self.status(), observed_at=101)
            self.assertFalse(self.value.home_window.visible)

    def test_readiness_is_strict_and_never_inferred_from_selected_phone(self):
        for state in ('disconnected', 'starting', 'ready', 'home', 'fault', 'stopped'):
            self.assertEqual(carplay_snapshot({'state': state, 'returnReady': False})['state'], state)
        for bad in (None, {}, {'state': 'ready'}, {'state': 'ready', 'returnReady': 1},
                    {'state': 'starting', 'returnReady': True}, {'state': 'ready', 'returnReady': True, 'extra': 1},
                    {'state': 'unknown', 'returnReady': False}):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                carplay_snapshot(bad)
        self.value.update_status({**self.status('starting', False), 'active': 'selected-phone'})
        self.assertFalse(self.value.home_window.visible)
        self.value._home_clicked(None)
        self.assertNotIn('request', self.events)
        # A future external Home action can focus the existing native underlay
        # without claiming the screen during a receiver's bootstrap.
        self.events.clear(); self.value.open()
        self.assertIn('page-present', self.events)
        self.assertNotIn('request', self.events)
        self.assertFalse(self.handoff.requested or self.value.foreground)
        self.value.update_status({**self.status(), 'active': None})
        self.assertTrue(self.value.home_window.visible)

    def test_disabled_home_has_no_click_handoff_but_external_admission_remains(self):
        self.value.home_enabled = False
        self.value._update_home()
        self.value._home_clicked(None)
        self.assertFalse(self.value.home_window.visible)
        self.assertNotIn('request', self.events)
        self.value.open()
        self.assertTrue(self.value.opening)

    def test_pending_hides_controls_without_releasing_foreground_ownership(self):
        self.handoff.ready = True
        self.value.open()
        self.assertTrue(self.value.can_return())
        self.events.clear(); self.value.set_pending(True)
        self.assertFalse(self.value.can_return())
        self.assertTrue(self.value.foreground and self.handoff.acquired)
        self.assertNotIn('off-and-release', self.events)
        self.assertTrue(self.value._handoff_tick())
        self.value.set_pending(False)
        self.assertTrue(self.value.can_return())
        self.value.back(); self.events.clear(); self.value.set_pending(True)
        self.value._home_clicked(None); self.value.open()
        self.assertFalse(self.value.home_window.visible)
        self.assertNotIn('request', self.events)

    def test_fresh_not_ready_lowers_before_releasing_even_when_busy(self):
        self.handoff.ready = True
        self.value.open(); self.value.set_pending(True); self.events.clear()
        self.value.update_status(self.status('starting', False))
        self.assertFalse(self.value.foreground or self.value.opening or self.handoff.acquired)
        self.assertLess(self.events.index('compositor-sync'), self.events.index('off-and-release'))
        self.assertFalse(self.value.can_return() or self.value.home_window.visible)

    def test_missing_readiness_and_clock_expiry_withdraw_controls(self):
        for reply in ({'camera': self.status()['camera']}, {**self.status(), 'carplay': {'state': 'ready', 'returnReady': 0}}):
            self.setUp(); self.handoff.ready = True; self.value.open()
            self.value.update_status(reply)
            self.assertFalse(self.value.foreground or self.value.can_return() or self.value.home_window.visible)
        for elapsed in (-1, 5.001):
            self.setUp(); self.value.last_status = 100
            with mock.patch('phone_page_presentation.time.monotonic', return_value=100 + elapsed):
                self.value._health_tick(); self.value._home_clicked(None)
            self.assertFalse(self.value.carplay_known or self.value.home_window.visible)
            self.assertNotIn('request', self.events)


if __name__ == '__main__':
    unittest.main()
