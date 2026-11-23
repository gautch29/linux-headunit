# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import sys
from pathlib import Path
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_state import PhoneState, receiver_settings
from media_state import validate_media, MediaState
from bluez_phones import BluezPhones

A, B = 'AA:BB:CC:DD:EE:01', 'AA:BB:CC:DD:EE:02'

class Tests(unittest.TestCase):
    def quarantine_fixture(self, root):
        from phone_manager import Manager
        import json
        import os
        class Phones:
            disconnects = 0
            forgets = 0
            commands = 0
            def phone(self, phone):
                return {}
            def phones(self):
                return []
            def media(self, phone):
                return {'available': False}
            def disconnect(self, phone):
                self.disconnects += 1
            def forget(self, phone):
                self.forgets += 1
            def media_command(self, phone, command):
                self.commands += 1
        display = root / 'standard.conf'
        display.write_text('explicit geometry fixture')
        config = root / 'receiver.json'
        config.write_text(json.dumps({'pair_store': str(root / 'pairs'), 'display_config': str(display)}))
        manager = Manager(config, root, display, (os.getuid(), os.getgid()), bluez=Phones())
        manager.state.select(A)
        manager.active = A
        return manager

    def test_optional_native_publication_failure_preserves_manager_state(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            manager = self.quarantine_fixture(root)
            try:
                original = manager.request({'action': 'status'})
                self.assertEqual(original['prephone_vehicle']['state'], 'disabled')
                manager.base['prephone_publication_socket'] = root / 'prephone.sock'
                manager.open_prephone_publication(None)
                status = manager.request({'action': 'status'})
                self.assertEqual(status['prephone_vehicle'], {'configured': True, 'state': 'publication_unavailable'})
                self.assertEqual(status['active'], original['active'])
                self.assertEqual(status['generation'], original['generation'])
                self.assertFalse((root / 'prephone.sock').exists())
            finally:
                manager.shutdown()

    def test_signal_death_requires_whole_service_retirement_before_any_new_owner(self):
        from phone_manager import RETIREMENT_REQUIRED
        from unittest.mock import patch
        import os
        import signal
        import subprocess
        import time
        class Pairing:
            starts = 0
            def start(self):
                self.starts += 1
            def close(self):
                pass
            def wait_closed(self):
                pass
            def snapshot(self):
                return {'supported': False, 'phase': 'idle'}
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            manager = self.quarantine_fixture(root)
            pairing = manager.pairing = Pairing()
            marker = root / 'grandchild.pid'
            script = ('import subprocess, sys, time\n'
                      'from pathlib import Path\n'
                      'child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], '
                      'stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n'
                      'Path(sys.argv[1]).write_text(str(child.pid))\n'
                      'time.sleep(30)\n')
            launcher = subprocess.Popen([sys.executable, '-B', '-c', script, str(marker)],
                                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
            descendant = None
            try:
                limit = time.monotonic() + 3
                while not marker.exists() and time.monotonic() < limit:
                    time.sleep(.01)
                self.assertTrue(marker.exists(), 'Fixture launcher did not create its descendant')
                descendant = int(marker.read_text())
                manager.child = launcher
                launcher.kill()
                launcher.wait(timeout=3)
                self.assertLess(launcher.returncode, 0)
                os.kill(descendant, 0)
                with patch('phone_manager.subprocess.run') as preflight, \
                     patch('phone_manager.subprocess.Popen') as replacement:
                    manager.media.publish({'available': True, 'title': 'Retiring fixture',
                                           'source': 'bluetooth', 'controls': ['next']})
                    for action in ({'action': 'connect', 'phone': A},
                                   {'action': 'pairing_start'}, {'action': 'forget', 'phone': A},
                                   {'action': 'media', 'phone': A, 'generation': manager.generation,
                                    'command': 'next'},
                                   {'action': 'media_update', 'phone': A, 'generation': manager.generation,
                                    'media': {'available': True, 'title': 'Late fixture'}}):
                        with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
                            manager.request(action)
                    manager.fallback_wayland = 'fixture-wayland'
                    with self.assertRaises(RuntimeError):
                        manager.maintain_fallback()
                    preflight.assert_not_called()
                    replacement.assert_not_called()
                self.assertEqual(pairing.starts, 0)
                self.assertEqual(manager.bluez.disconnects, 0)
                self.assertEqual(manager.bluez.forgets, 0)
                self.assertEqual(manager.bluez.commands, 0)
                with patch.object(manager, 'maintain_fallback') as fallback:
                    with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
                        manager.tick()
                    fallback.assert_not_called()
                self.assertIsNone(manager.child)
                self.assertIsNone(manager.active)
                self.assertFalse(manager.media.last['available'])
                self.assertEqual(manager.request({'action': 'status'})['error'], RETIREMENT_REQUIRED)
                manager.stop()
                self.assertTrue(manager.retirement_required)
                with self.assertRaises(RuntimeError):
                    manager.start(A)
                manager.shutdown()
                # The manager does not guess descendant PIDs or kill a broad
                # process group. Actual systemd cleanup remains target evidence.
                os.kill(descendant, 0)
            finally:
                manager.shutdown()
                if launcher.poll() is None:
                    launcher.kill()
                    launcher.wait(timeout=3)
                if descendant is not None:
                    try:
                        os.kill(descendant, signal.SIGTERM)
                    except ProcessLookupError:
                        pass

    def test_signal_death_during_preflight_or_stop_cannot_admit_successor(self):
        from unittest.mock import patch
        class Child:
            returncode = None
            def __init__(self, die_during_stop):
                self.die_during_stop = die_during_stop
            def poll(self):
                return self.returncode
            def terminate(self):
                if self.returncode is None:
                    self.returncode = -9 if self.die_during_stop else 0
            def wait(self, timeout):
                return self.returncode
        for phase in ('preflight', 'stop'):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as folder:
                manager = self.quarantine_fixture(Path(folder).resolve())
                child = manager.child = Child(phase == 'stop')
                def preflight(*args, **kwargs):
                    if phase == 'preflight':
                        child.returncode = -9
                with patch('phone_manager.subprocess.run', side_effect=preflight), \
                     patch('phone_manager.subprocess.Popen') as launch:
                    with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
                        manager.start(B)
                    launch.assert_not_called()
                self.assertTrue(manager.retirement_required)
                self.assertIsNone(manager.child)
                self.assertEqual(manager.bluez.disconnects, 0)
                self.assertEqual(manager.state.selected, A)
                manager.shutdown()

    def test_forget_signal_death_during_stop_preserves_phone_and_credentials(self):
        class Child:
            returncode = None
            def poll(self):
                return self.returncode
            def terminate(self):
                self.returncode = -9
            def wait(self, timeout):
                return self.returncode
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            manager = self.quarantine_fixture(root)
            store = root / 'pairs' / ('phone-' + A.replace(':', ''))
            store.mkdir(parents=True)
            fixture = store / 'fixture.key'
            fixture.write_text('synthetic credential fixture')
            manager.child = Child()
            with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
                manager.request({'action': 'forget', 'phone': A})
            self.assertTrue(manager.retirement_required)
            self.assertEqual(manager.bluez.forgets, 0)
            self.assertEqual(manager.state.selected, A)
            self.assertTrue(fixture.exists())
            manager.shutdown()

    def test_phone_membership_preserves_legacy_hints_without_mode_api(self):
        import json
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'phones.json'
            # Values are retained solely for the old release's rollback schema.
            path.write_text(json.dumps({'version': 1, 'selected': A,
                                        'phones': {A: 'ultra', B: 'standard'}}))
            state = PhoneState(path)
            self.assertFalse(hasattr(state, 'mode')); self.assertFalse(hasattr(state, 'set_mode'))
            state.select(B); state.select(A)
            state = PhoneState(path)
            self.assertEqual(state.phones, {A: 'ultra', B: 'standard'})
            self.assertEqual(state.selected, A)
            state.forget(A)
            self.assertIsNone(state.selected); self.assertEqual(state.phones, {B: 'standard'})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            for bad in ('../phone', '', 'aa:bb:cc:dd:ee:01\n'):
                with self.assertRaises(ValueError): state.select(bad)
            state.select(A)
            self.assertEqual(PhoneState(path).selected, A)

    def test_receiver_offers_same_configured_capabilities_to_each_phone(self):
        base = {'pair_store': '/private/pairs', 'ultra_installed_ui': '/private/ui', 'display_config': '/private/ultra',
                'climate_config': '/private/climate.json', 'car_settings_config': '/private/settings.json',
                'vehicle_state_backend': 'simulation', 'camera_manual_profile': '/private/camera.json',
                'camera_state_config': '/private/camera-state.json'}
        first = receiver_settings(base, A, '/private/standard', '/private/media')
        second = receiver_settings(base, B, '/private/standard', '/private/media')
        for settings, phone in ((first, A), (second, B)):
            self.assertTrue(settings['standard_cluster']); self.assertTrue(settings['ultra_negotiate'])
            self.assertTrue(settings['ultra_native_video'])
            self.assertEqual(settings['ultra_installed_ui'], '/private/ui')
            self.assertEqual(settings['display_config'], '/private/ultra')
            self.assertEqual(settings['standard_display_config'], '/private/standard')
            self.assertEqual(settings['preview_sink'], 'wayland-touch')
            self.assertEqual(settings['climate_config'], '/private/climate.json')
            self.assertEqual(settings['car_settings_config'], '/private/settings.json')
            self.assertEqual(settings['camera_state_config'], '/private/camera-state.json')
            self.assertNotIn('camera_manual_profile', settings)
            self.assertEqual(settings['selected_phone'], phone)
        self.assertNotEqual(first['pair_store'], second['pair_store'])
        self.assertEqual(base['display_config'], '/private/ultra')
        no_asset = dict(base); del no_asset['ultra_installed_ui']
        standard = receiver_settings(no_asset, A, '/private/standard', '/private/media')
        self.assertFalse(standard['ultra_negotiate']); self.assertFalse(standard['ultra_native_video'])
        self.assertTrue(standard['standard_cluster'])
        self.assertEqual(standard['display_config'], '/private/standard')
        self.assertEqual(standard['vehicle_state_backend'], 'none')
        for key in ('ultra_installed_ui', 'climate_config', 'car_settings_config', 'camera_state_config'):
            self.assertNotIn(key, standard)

    def test_phone_preferences_reject_unsafe_and_changed_files(self):
        import json
        import os
        import subprocess
        from unittest.mock import patch
        from phone_state import atomic_json

        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'phones.json'
            saved = {'version': 1, 'selected': A, 'phones': {A: 'ultra'}}
            atomic_json(path, saved)
            self.assertEqual(PhoneState(path).selected, A)
            for content in ('null', '[[]]', 'true',
                            '{"version":1,"version":1,"selected":null,"phones":{}}',
                            '[' * 2000 + '0' + ']' * 2000):
                path.write_text(content)
                with self.assertRaises(ValueError):
                    PhoneState(path)
            for value in (False, 0, {}, []):
                path.write_text(json.dumps({**saved, 'selected': value}))
                with self.assertRaises(ValueError):
                    PhoneState(path)
            path.write_text(json.dumps({**saved, 'version': True}))
            with self.assertRaises(ValueError):
                PhoneState(path)
            for content in ('', ' ' * 65537):
                path.write_text(content)
                with self.assertRaises(ValueError):
                    PhoneState(path)
            atomic_json(path, saved)
            hardlink = path.with_name('linked')
            os.link(path, hardlink)
            with self.assertRaises(ValueError):
                PhoneState(path)
            hardlink.unlink()
            link = path.with_name('symlink')
            link.symlink_to(path)
            with self.assertRaises((OSError, ValueError)):
                PhoneState(link)
            with self.assertRaises(ValueError):
                PhoneState(Path(folder))
            original_read = os.read
            for replace in (False, True):
                atomic_json(path, saved)
                def read_then_change(fd, maximum):
                    content = original_read(fd, maximum)
                    if replace:
                        atomic_json(path, saved)
                    else:
                        path.write_text(json.dumps({**saved, 'selected': None}))
                    return content
                with patch('phone_state.os.read', side_effect=read_then_change):
                    with self.assertRaises(ValueError):
                        PhoneState(path)
            path.unlink()
            os.mkfifo(path, 0o600)
            script = ('import sys\n'
                      'sys.path.insert(0, sys.argv[1])\n'
                      'from pathlib import Path\n'
                      'from phone_state import PhoneState\n'
                      'try: PhoneState(Path(sys.argv[2]))\n'
                      'except ValueError: pass\n'
                      'else: raise AssertionError("FIFO preferences were admitted")\n')
            completed = subprocess.run([sys.executable, '-B', '-c', script,
                                        str(Path(__file__).resolve().parents[1] / 'tools'),
                                        str(path)], timeout=3, capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode())

    def test_manager_log_admission_preserves_append_and_rejects_unsafe_files(self):
        import os
        import subprocess
        from phone_manager import open_owned_log

        owner = os.getuid(), os.getgid()
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'receiver-manager.log'
            for content in (b'first\n', b'second\n'):
                fd = open_owned_log(path, owner)
                try:
                    os.write(fd, content)
                finally:
                    os.close(fd)
            self.assertEqual(path.read_bytes(), b'first\nsecond\n')
            linked = path.with_name('linked')
            os.link(path, linked)
            with self.assertRaises(ValueError):
                open_owned_log(path, owner)
            linked.unlink()
            path.chmod(0o622)
            with self.assertRaises(ValueError):
                open_owned_log(path, owner)
            path.chmod(0o600)
            linked.symlink_to(path)
            with self.assertRaises(OSError):
                open_owned_log(linked, owner)
            self.assertEqual(path.read_bytes(), b'first\nsecond\n')
            with self.assertRaises(OSError):
                open_owned_log(Path(folder), owner)
            path.unlink()
            os.mkfifo(path, 0o600)
            script = ('import os\n'
                      'import sys\n'
                      'sys.path.insert(0, sys.argv[1])\n'
                      'from phone_manager import open_owned_log\n'
                      'try: open_owned_log(sys.argv[2], (os.getuid(), os.getgid()))\n'
                      'except (OSError, ValueError): pass\n'
                      'else: raise AssertionError("FIFO log was admitted")\n')
            completed = subprocess.run([sys.executable, '-B', '-c', script,
                                        str(Path(__file__).resolve().parents[1] / 'tools'),
                                        str(path)], timeout=3, capture_output=True)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode())

    def test_manager_mode_requires_fresh_current_receiver_confirmation(self):
        from phone_manager import Manager
        from phone_state import atomic_json
        import json, os
        class Phones:
            def phones(self): return [{'address': p, 'name': 'Fixture', 'connected': True} for p in (A, B)]
            def phone(self, phone): return next(p for p in self.phones() if p['address'] == phone)
        class Child:
            alive = True
            def poll(self): return None if self.alive else 7
            def terminate(self): self.alive = False
            def wait(self, timeout): return 0
        with tempfile.TemporaryDirectory(prefix='pm-mode-', dir='/tmp') as folder:
            root = Path(folder).resolve(); root.chmod(0o700)
            owner = os.getuid(), os.getgid()
            atomic_json(root / 'phones.json', {'version': 1, 'selected': A,
                                               'phones': {A: 'ultra', B: 'standard'}}, owner)
            display = root / 'standard.conf'; display.write_text('explicit geometry fixture')
            config = root / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root / 'pairs'), 'display_config': str(display)}))
            manager = Manager(config, root, display, owner, bluez=Phones())
            # Synthetic monotonic dates and an actual private file exercise
            # production validation. Child lifetime is a fixture; no radios/video.
            now = 5_000_000_000
            manager.carplay.clock = lambda: now
            manager.active = A; manager.child = Child(); manager.generation = 7
            manager.carplay.reset('a' * 32)
            def status(): return manager.request({'action': 'status'})
            def publish(sequence, mode=None, version=2, updated=None, nonce='a' * 32):
                value = {'version': version, 'nonce': nonce, 'sequence': str(sequence),
                         'connectionGeneration': '1', 'updatedMonotonicNs': str(now if updated is None else updated),
                         'state': 'ready', 'returnReady': True}
                if version == 2: value['negotiatedMode'] = mode
                atomic_json(manager.carplay.path, value, owner)
            self.assertIsNone(status()['active_mode'])
            publish(1, version=1)
            self.assertTrue(status()['carplay']['returnReady'])
            self.assertIsNone(status()['active_mode'], 'v1 readiness cannot invent a negotiated mode')
            publish(2, 'standard')
            confirmed = status()
            self.assertEqual(confirmed['active_mode'], 'standard')
            self.assertTrue(all('mode' not in phone for phone in confirmed['phones']))
            legacy = manager.state.path.read_bytes()
            for mode in ('standard', 'ultra'):
                with self.assertRaises(ValueError): manager.request({'action': 'mode', 'phone': A, 'mode': mode})
                self.assertEqual(manager.state.path.read_bytes(), legacy)
                self.assertIsNotNone(manager.child)
            now += 1_500_000_001
            self.assertIsNone(status()['active_mode'])
            self.assertFalse(status()['carplay']['returnReady'])
            publish(3, 'ultra')
            self.assertEqual(status()['active_mode'], 'ultra')
            manager.state.select(B); manager.active = B; manager.carplay.reset('b' * 32)
            self.assertIsNone(status()['active_mode'], 'Previous child publication must not confirm a new launch')
            publish(1, 'ultra', nonce='b' * 32)
            self.assertEqual(status()['active_mode'], 'ultra', 'Saved Standard hint cannot choose this session')
            self.assertEqual(manager.state.phones, {A: 'ultra', B: 'standard'})
            manager.child.alive = False
            self.assertIsNone(status()['active_mode']); self.assertFalse(status()['carplay']['returnReady'])
            manager.stop()
            self.assertIsNone(manager.active_mode); self.assertIsNone(manager.carplay.nonce)
            self.assertIsNone(status()['active_mode'])

    def test_media_replacement_and_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            media = MediaState(Path(folder) / 'media.json')
            first = media.publish({'available': True, 'title': 'Café', 'artist': 'Artist', 'status': 'playing',
                                   'controls': ['play', 'next'], 'volume': .5})
            self.assertEqual(first['title'], 'Café')
            self.assertNotIn('artwork', first)
            cleared = media.publish({'available': False})
            self.assertNotIn('title', cleared); self.assertEqual(cleared['controls'], [])
            for value in ({'available': True, 'volume': float('nan')},
                          {'available': True, 'controls': ['erase']},
                          {'available': True, 'position_ms': -1},
                          {'available': True, 'title': 'x'*3000}):
                with self.assertRaises(ValueError): validate_media(value)
            with self.assertRaises(ValueError): media.publish({'available': True}, 'not-base64!')

    def test_iap_metadata_precedes_bluetooth_only_while_session_is_live(self):
        from phone_manager import Manager
        import json, os
        class Phone:
            def phone(self, _address): return {'address': A}
            def media(self, _phone):
                return {'available': True, 'title': 'Bluetooth title', 'status': 'playing',
                        'controls': ['play', 'pause', 'next']}
            def media_command(self, _phone, command):
                self.last_command = command
        class Process:
            def poll(self): return None
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            display = root / 'standard.conf'; display.write_text('test')
            config = root / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display)}))
            phone = Phone()
            manager = Manager(config, root, display, (os.getuid(), os.getgid()), phone)
            manager.active, manager.child = A, Process()
            manager.iap_media_path.write_text(json.dumps({'available': True, 'title': 'iPhone title',
                                                         'artist': 'Artist', 'status': 'playing'}))
            manager.tick()
            self.assertEqual(manager.media.last['title'], 'iPhone title')
            self.assertEqual(manager.media.last['source'], 'iap')
            self.assertIn('next', manager.media.last['controls'])
            manager.request({'action': 'media', 'phone': A, 'generation': manager.generation,
                             'command': 'next'})
            self.assertEqual(phone.last_command, 'next')
            manager.iap_media_path.unlink()
            manager.tick()
            self.assertEqual(manager.media.last['title'], 'Bluetooth title')
            manager.iap_media_path.symlink_to(root / 'media.json')
            manager.tick()
            self.assertEqual(manager.media.last['title'], 'Bluetooth title')
            self.assertNotEqual(manager.media.last['source'], 'iap')

    def test_malformed_iap_metadata_cannot_retire_manager(self):
        from phone_manager import Manager
        import json
        import os
        from unittest.mock import patch

        class Phone:
            def media(self, phone):
                return {'available': True, 'title': 'Bluetooth fallback'}

        class Child:
            def poll(self):
                return None

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            display = root / 'standard.conf'
            display.write_text('explicit geometry fixture')
            config = root / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root / 'pairs'),
                                          'display_config': str(display)}))
            manager = Manager(config, root, display, (os.getuid(), os.getgid()), bluez=Phone())
            manager.active = A
            manager.child = Child()
            for content in ('null', 'true', '1', '[[]]',
                            '[' * 2000 + '0' + ']' * 2000):
                manager.iap_media_path.write_text(content)
                manager.tick()
                self.assertEqual(manager.media.last['title'], 'Bluetooth fallback')
                self.assertIsNotNone(manager.child)
            for digest in (False, 0, [], {}, None):
                manager.iap_media_path.write_text(json.dumps({
                    'available': True, 'title': 'Invalid cover', 'iap_artwork_sha256': digest}))
                manager.tick()
                self.assertEqual(manager.media.last['title'], 'Bluetooth fallback')
            value = {'available': True, 'title': 'iAP title'}
            manager.iap_media_path.write_text(json.dumps(value))
            linked = root / 'linked-metadata'
            os.link(manager.iap_media_path, linked)
            manager.tick()
            self.assertEqual(manager.media.last['title'], 'Bluetooth fallback')
            linked.unlink()
            manager.iap_media_path.chmod(0o622)
            manager.tick()
            self.assertEqual(manager.media.last['title'], 'Bluetooth fallback')
            manager.iap_media_path.chmod(0o600)
            original_read = os.read
            def read_then_change(fd, maximum):
                content = original_read(fd, maximum)
                manager.iap_media_path.write_text(json.dumps({**value, 'title': 'Changed title'}))
                return content
            with patch('phone_manager.os.read', side_effect=read_then_change):
                manager.tick()
            self.assertEqual(manager.media.last['title'], 'Bluetooth fallback')
            manager.tick()
            self.assertEqual(manager.media.last['title'], 'Changed title')

    def test_iap_special_files_cannot_block_manager_polling(self):
        import json
        import os
        import subprocess
        import textwrap

        # A separate process gives the fixture a real deadline even when an
        # unsafe blocking open never returns. No radio or display is started.
        source = textwrap.dedent('''\
            import os
            import sys
            from pathlib import Path
            sys.path.insert(0, sys.argv[1])
            from phone_manager import Manager
            class Phone:
                def media(self, phone):
                    return {'available': False}
            class Child:
                def poll(self):
                    return None
            root = Path(sys.argv[2])
            manager = Manager(root / 'receiver.json', root, root / 'standard.conf',
                              (os.getuid(), os.getgid()), bluez=Phone())
            manager.active = 'AA:BB:CC:DD:EE:01'
            manager.child = Child()
            manager.tick()
            assert manager.media.last['available'] is (sys.argv[3] == 'artwork')
            assert 'artwork' not in manager.media.last
        ''')
        for target in ('metadata', 'artwork'):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                display = root / 'standard.conf'
                display.write_text('explicit geometry fixture')
                (root / 'receiver.json').write_text(json.dumps({
                    'pair_store': str(root / 'pairs'), 'display_config': str(display)}))
                metadata = root / 'media.json.iap'
                if target == 'metadata':
                    os.mkfifo(metadata, 0o600)
                else:
                    metadata.write_text(json.dumps({'available': True, 'title': 'Fixture',
                                                    'iap_artwork_sha256': 'a' * 64}))
                    os.mkfifo(root / 'media.json.iap.art', 0o600)
                completed = subprocess.run([sys.executable, '-B', '-c', source,
                                            str(Path(__file__).resolve().parents[1] / 'tools'),
                                            str(root), target], timeout=3, capture_output=True)
                self.assertEqual(completed.returncode, 0, completed.stderr.decode())

    def test_artwork_normalization_and_clear(self):
        try:
            from gi.repository import GdkPixbuf
        except ImportError:
            self.skipTest('GdkPixbuf is a Linux native-page dependency')
        import base64
        with tempfile.TemporaryDirectory() as folder:
            media = MediaState(Path(folder)/'media.json')
            pixbuf = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, False, 8, 16, 16)
            pixbuf.fill(0x327acbff)
            ok, png = pixbuf.save_to_bufferv('png', [], [])
            self.assertTrue(ok)
            value = media.publish({'available':True,'title':'Bench track'}, base64.b64encode(png).decode())
            self.assertTrue((Path(folder)/value['artwork']).exists())
            self.assertEqual(len(value['artwork_id']), 64)
            self.assertNotIn('artwork', media.publish({'available':True,'title':'Next track'}))

    def test_standalone_iap_bridge_retires_changed_cover_and_retries(self):
        import hashlib, json, signal
        from unittest.mock import patch
        import media_bridge
        try:
            import gi
            gi.require_version('GdkPixbuf','2.0')
            from gi.repository import GdkPixbuf
        except ImportError:
            self.skipTest('GdkPixbuf is a native media dependency')
        covers=[]
        for color in (0x327acbff,0xca3421ff):
            pixbuf=GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB,False,8,16,16)
            pixbuf.fill(color)
            ok,png=pixbuf.save_to_bufferv('png',[],[])
            self.assertTrue(ok); covers.append(png)
        # Only pacing and stop signals are controlled. The production bridge,
        # private reads, image normalization and snapshot publication all run.
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as folder:
                root=Path(folder)
                source=root/'iap.json'
                art=root/'iap.json.art'
                before,after=covers
                destination=root/'native.json'
                state={'available':True, 'title':'Same track', 'track_id':'7',
                       'iap_artwork_sha256':hashlib.sha256(before).hexdigest()}
                source.write_text(json.dumps(state)); art.write_bytes(before)
                published=[]
                callbacks={}
                turns=0
                def advance(_duration):
                    nonlocal turns
                    published.append(json.loads(destination.read_text()))
                    turns+=1
                    if turns==1:
                        state['iap_artwork_sha256']=hashlib.sha256(after).hexdigest()
                        source.write_text(json.dumps(state))
                        if missing: art.unlink()
                    elif turns==2:
                        art.write_bytes(after)
                    else:
                        callbacks[signal.SIGTERM](signal.SIGTERM,None)
                with patch.object(media_bridge.signal,'signal',side_effect=lambda number,callback: callbacks.update({number:callback})), \
                     patch.object(media_bridge.time,'monotonic',side_effect=lambda: float(turns)), \
                     patch.object(media_bridge.time,'sleep',side_effect=advance):
                    media_bridge.run(source,destination)
                self.assertEqual(len(published),3)
                self.assertIn('artwork',published[0])
                self.assertEqual(published[1]['title'],'Same track')
                self.assertNotIn('artwork',published[1])
                self.assertIn('artwork',published[2])
                self.assertNotEqual(published[0]['artwork_id'],published[2]['artwork_id'])

    def test_standalone_iap_bridge_private_artwork_bounds(self):
        import media_bridge
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            art=root/'art'
            art.write_bytes(b'x'*(1048576+1))
            with self.assertRaises(ValueError): media_bridge.read_private(art,1048576)
            art.unlink(); art.symlink_to(root/'target')
            (root/'target').write_bytes(b'cover')
            with self.assertRaises(OSError): media_bridge.read_private(art,1048576)

    def test_iap_artwork_file_is_bounded_and_clears_on_track_change(self):
        try:
            import gi
            gi.require_version('GdkPixbuf', '2.0')
            from gi.repository import GdkPixbuf
        except ImportError:
            self.skipTest('GdkPixbuf is a Linux native-page dependency')
        from phone_manager import Manager
        import hashlib, json, os
        class Phone:
            def media(self, _phone): return {'available': False}
        class Process:
            def poll(self): return None
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            display = root/'standard.conf'; display.write_text('test')
            config = root/'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display)}))
            manager = Manager(config, root, display, (os.getuid(), os.getgid()), Phone())
            manager.active, manager.child = A, Process()
            pixbuf = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, False, 8, 16, 16)
            pixbuf.fill(0x327acbff)
            ok, jpeg = pixbuf.save_to_bufferv('jpeg', [], [])
            self.assertTrue(ok)
            manager.iap_art_path.write_bytes(jpeg)
            source = {'available': True, 'title': 'Song', 'track_id': '7',
                      'iap_artwork_sha256': hashlib.sha256(jpeg).hexdigest()}
            manager.iap_media_path.write_text(json.dumps(source))
            manager.tick()
            self.assertIn('artwork', manager.media.last)
            self.assertTrue((root / manager.media.last['artwork']).is_file())
            first_art_id = manager.media.last['artwork_id']
            manager.tick()
            self.assertEqual(manager.media.last['artwork_id'], first_art_id)
            source = {'available': True, 'title': 'Next song', 'track_id': '8'}
            manager.iap_media_path.write_text(json.dumps(source))
            manager.tick()
            self.assertNotIn('artwork', manager.media.last)

    def test_bluez_selected_player_and_commands(self):
        class Fake(BluezPhones):
            def __init__(self): self.calls = []; self.connected = True
            def phone(self, target): return {'path': '/phoneA', 'connected': self.connected}
            def objects(self):
                return {'/phoneA': {'org.bluez.MediaControl1': {'Player': '/phoneA/player'}},
                        '/phoneA/player': {'org.bluez.MediaPlayer1': {'Track': {'Title': 'Selected track'}, 'Status': 'playing'}},
                        '/phoneB/player': {'org.bluez.MediaPlayer1': {'Track': {'Title': 'Wrong track'}}}}
            def call(self, *args): self.calls.append(args)
        source = Fake()
        self.assertEqual(source.media(A)['title'], 'Selected track')
        source.media_command(A, 'next')
        self.assertEqual(source.calls[-1], ('/phoneA/player', 'org.bluez.MediaPlayer1', 'Next'))
        with self.assertRaises(ValueError): source.media_command(A, 'volume_up')
        source.connected = False
        self.assertFalse(source.media(A)['available'])
        with self.assertRaises(ValueError): source.media_command(A, 'play')

    def test_manager_forget_and_stale_command(self):
        from phone_manager import Manager
        import json, os
        class Fake:
            def __init__(self): self.removed = []
            def phones(self): return [{'address': a, 'name': 'Test phone', 'connected': False, 'path': '/test'} for a in (A,B) if a not in self.removed]
            def phone(self, a):
                if a in self.removed: raise ValueError('not paired')
                return next(p for p in self.phones() if p['address'] == a)
            def forget(self, a): self.removed.append(a)
            def media_command(self, a, c): raise AssertionError('stale command reached phone')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            config = root / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(root/'ultra.conf'), 'ultra_installed_ui': str(root/'ui.conf'), 'auto_connect_phone': A, 'ultra_negotiate': True}))
            standard = root/'standard.conf'; standard.write_text('test')
            layout = root/'layout'; (root/'ui.conf').write_text(f'clusterLayoutFile={layout}\n')
            bluez = Fake()
            manager = Manager(config, root, standard, None, bluez)
            self.assertIn(A, manager.state.phones)
            self.assertIsNone(manager.active_mode)
            self.assertEqual(manager.state.selected, A)
            style = '00000000-1111-2222-3333-444444444444'
            status = layout.with_name(layout.name + '.status')
            status.write_text(json.dumps({'active': style, 'pending': '', 'ready': True, 'transitioning': False,
                                          'error': '', 'choices': [{'id': style, 'name': 'Round'}]}))
            status.chmod(0o600)
            self.assertIsNone(manager.request({'action': 'status'})['ultra_cluster'])
            with self.assertRaises(ValueError): manager.request({'action': 'cluster_layout', 'layout': style})
            self.assertFalse(layout.exists())
            with self.assertRaises(ValueError): manager.request({'action': 'cluster_layout', 'layout': 'unknown'})
            for phone in (A,B):
                directory=root/'pairs'/('phone-'+phone.replace(':','')); directory.mkdir(parents=True)
                (directory/'key').write_text('test-only')
            manager.request({'action':'forget','phone':A})
            self.assertFalse((root/'pairs'/('phone-'+A.replace(':',''))).exists())
            self.assertTrue((root/'pairs'/('phone-'+B.replace(':',''))/'key').exists())
            self.assertIsNone(manager.state.selected)
            manager.active=B
            with self.assertRaises(ValueError): manager.request({'action':'media','phone':B,'generation':-1,'command':'play'})
            with self.assertRaises(ValueError): manager.request({'action':'forget','phone':'../../'})

    def test_display_response_is_a_private_bounded_preference(self):
        from phone_manager import Manager
        import json, stat
        class Phones:
            def phones(self): return []
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            config = root / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(root/'ultra.conf'),
                                          'ultra_installed_ui': str(root/'ui.conf')}))
            standard = root/'standard.conf'; standard.write_text('test')
            (root/'ui.conf').write_text('vehicleProfile=/unused\n')
            unconfigured = Manager(config, root, standard, None, Phones())
            self.assertEqual(unconfigured.request({'action': 'status'})['display_response'],
                             {'configured': False, 'mode': None})
            with self.assertRaises(ValueError):
                unconfigured.request({'action': 'display_response', 'mode': 'fast'})
            preference = root/'prefs'/'display-response'
            (root/'ui.conf').write_text(f'displayResponseFile={preference}\n')
            manager = Manager(config, root, standard, None, Phones())
            self.assertEqual(manager.request({'action': 'status'})['display_response'],
                             {'configured': True, 'mode': None})
            for invalid in ({'action': 'display_response', 'mode': 'turbo'},
                            {'action': 'display_response'},
                            {'action': 'display_response', 'mode': 'fast', 'page': 'map'},
                            {'action': 'cluster_page', 'page': 'map', 'mode': 'fast'}):
                with self.assertRaises(ValueError):
                    manager.request(invalid)
            self.assertFalse(preference.exists())
            manager.request({'action': 'display_response', 'mode': 'fast'})
            self.assertEqual(preference.read_text(), 'fast\n')
            self.assertEqual(stat.S_IMODE(preference.stat().st_mode), 0o600)
            self.assertEqual(manager.request({'action': 'status'})['display_response'],
                             {'configured': True, 'mode': 'fast'})
            manager.request({'action': 'display_response', 'mode': 'smooth'})
            self.assertEqual(manager.request({'action': 'status'})['display_response']['mode'], 'smooth')

    def test_greeting_preference_and_dated_preview_without_phone_mutation(self):
        from phone_manager import Manager
        import json, os, stat, time
        class Phones:
            def phones(self): return []
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            config = root / 'receiver.json'
            standard = root / 'standard.conf'
            standard.write_text('test')
            preference = root / 'greeting-sequence'
            config.write_text(json.dumps({'greeting_sequence_file': str(preference), 'display_config': str(standard)}))
            manager = Manager(config, root, standard, (os.getuid(), os.getgid()), Phones())
            model = manager.request({'action': 'status'})['greeting_sequence']
            self.assertEqual(model['variant'], 'off')
            self.assertFalse(model['powerIntegrated'])
            self.assertFalse(model['mainIntegrated'])
            for invalid in ({'action': 'greeting_sequence', 'variant': 'aston'},
                            {'action': 'greeting_sequence', 'variant': True},
                            {'action': 'greeting_sequence', 'variant': 'brand', 'phone': A},
                            {'action': 'greeting_preview', 'phase': 'ignition-off'},
                            {'action': 'status', 'variant': 'brand'}):
                with self.assertRaises(ValueError): manager.request(invalid)
            manager.request({'action': 'greeting_sequence', 'variant': 'brand'})
            self.assertEqual(preference.read_text(), 'brand\n')
            self.assertEqual(stat.S_IMODE(preference.stat().st_mode), 0o600)
            successor = Manager(config, root, standard, (os.getuid(), os.getgid()), Phones())
            self.assertEqual(successor.request({'action': 'status'})['greeting_sequence']['variant'], 'brand')
            domain = time.CLOCK_MONOTONIC_RAW if sys.platform == 'darwin' else time.CLOCK_MONOTONIC
            before = time.clock_gettime_ns(domain)
            reply = successor.request({'action': 'greeting_preview', 'phase': 'goodbye'})
            after = time.clock_gettime_ns(domain)
            phase, acquired = Path(str(preference) + '.event').read_text().split()
            self.assertEqual(phase, 'goodbye')
            self.assertTrue(before <= int(acquired) <= after)
            self.assertFalse(reply['confirmed'])
            self.assertEqual(reply['provenance'], 'native-preview')
            self.assertIsNone(successor.child)
            manager.request({'action': 'greeting_sequence', 'variant': 'off'})
            preference.unlink()
            preference.symlink_to(standard)
            self.assertEqual(manager.request({'action': 'status'})['greeting_sequence']['variant'], 'off')
            config.write_text(json.dumps({'greeting_sequence_file': str(root / 'foreign'), 'display_config': str(standard)}))
            with self.assertRaises(ValueError):
                Manager(config, root, standard, None, Phones())

    def test_startup_warning_setting_persists_without_phone_mutation(self):
        from phone_manager import Manager
        import json, stat
        class Phones:
            def phones(self): return []
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            config = root/'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(root/'ultra.conf'),
                                          'ultra_installed_ui': str(root/'ui.conf')}))
            standard = root/'standard.conf'; standard.write_text('test')
            preference = root/'prefs'/'startup-warning-lights'
            warnings = root/'warnings.json'
            warnings.write_text(json.dumps({'enabled': True, 'hideDuringStartup': True}))
            (root/'ui.conf').write_text(f'warningOverlayProfile={warnings}\nstartupWarningLightsFile={preference}\n')
            manager = Manager(config, root, standard, None, Phones())
            self.assertEqual(manager.request({'action': 'status'})['startup_warning_lights'],
                             {'configured': True, 'hide': True})
            for invalid in ({'action': 'startup_warning_lights'},
                            {'action': 'startup_warning_lights', 'hide': 1},
                            {'action': 'startup_warning_lights', 'hide': 'false'},
                            {'action': 'startup_warning_lights', 'hide': False, 'phone': 'x'}):
                with self.assertRaises(ValueError): manager.request(invalid)
            manager.request({'action': 'startup_warning_lights', 'hide': False})
            self.assertEqual(preference.read_text(), 'show\n')
            self.assertEqual(stat.S_IMODE(preference.stat().st_mode), 0o600)
            restarted = Manager(config, root, standard, None, Phones())
            self.assertFalse(restarted.request({'action': 'status'})['startup_warning_lights']['hide'])
            self.assertIsNone(restarted.child)
            restarted.request({'action': 'startup_warning_lights', 'hide': True})
            self.assertEqual(preference.read_text(), 'hide\n')
            warnings.write_text(json.dumps({'enabled': True, 'hideDuringStartup': False}))
            preference.unlink()
            configured_show = Manager(config, root, standard, None, Phones())
            self.assertFalse(configured_show.request({'action': 'status'})['startup_warning_lights']['hide'])
            (root/'ui.conf').write_text('vehicleProfile=/unused\n')
            unconfigured = Manager(config, root, standard, None, Phones())
            self.assertFalse(unconfigured.request({'action': 'status'})['startup_warning_lights']['configured'])
            with self.assertRaises(ValueError): unconfigured.request({'action': 'startup_warning_lights', 'hide': True})

    def test_ultra_styles_require_current_negotiated_owner_and_retire_with_it(self):
        from phone_manager import Manager
        from phone_state import atomic_json
        from carplay_readiness import clock_ns
        import json, os
        class Phones:
            def phones(self): return []
        class Child:
            def poll(self): return None
            def terminate(self): pass
            def wait(self, timeout): return 0
        with tempfile.TemporaryDirectory(prefix='pm-style-', dir='/tmp') as folder:
            root = Path(folder).resolve(); root.chmod(0o700)
            owner = os.getuid(), os.getgid()
            layout = root / 'layout'; profile = root / 'ui.conf'
            profile.write_text(f'clusterLayoutFile={layout}\n')
            display = root / 'standard.conf'; display.write_text('explicit geometry fixture')
            config = root / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root / 'pairs'), 'display_config': str(display),
                                          'ultra_installed_ui': str(profile), 'cluster_page': str(root / 'page')}))
            manager = Manager(config, root, display, owner, bluez=Phones())
            style = '00000000-1111-2222-3333-444444444444'
            def catalogue(nonce='a' * 32, generation='1'):
                atomic_json(layout.with_name(layout.name + '.status'), {
                    'version': 2, 'nonce': nonce, 'connectionGeneration': generation, 'sequence': '1',
                    'updatedMonotonicNs': str(clock_ns()),
                    'active': style, 'pending': '', 'ready': True, 'transitioning': False, 'error': '',
                    'choices': [{'id': style, 'name': 'Round'}]}, owner)
            catalogue()
            def unavailable():
                reply = manager.request({'action': 'status'})
                self.assertIsNone(reply['ultra_cluster'])
                self.assertEqual(reply['cluster_formats']['ultra']['choices'], [])
                with self.assertRaises(ValueError):
                    manager.request({'action': 'cluster_format', 'renderer': 'ultra', 'format': style, 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}})
                self.assertIn('media', reply['cluster_pages'])
                with self.assertRaises(ValueError): manager.request({'action': 'cluster_layout', 'layout': style, 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}})
                with self.assertRaises(ValueError): manager.request({'action': 'cluster_input', 'command': 'next', 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}})
            unavailable()  # A fresh global catalog does not establish a live scene owner.
            manager.active = A; manager.state.select(A); manager.child = Child(); manager.carplay.reset('a' * 32)
            def publish(sequence, mode, nonce='a' * 32, age=0, state='ready', generation='1'):
                atomic_json(manager.carplay.path, {'version': 2, 'nonce': nonce, 'sequence': str(sequence),
                    'connectionGeneration': generation, 'updatedMonotonicNs': str(clock_ns() - age),
                    'state': state, 'returnReady': state in ('ready', 'home'), 'negotiatedMode': mode}, owner)
            publish(1, 'standard'); unavailable()
            manager.request({'action': 'cluster_page', 'page': 'media'})
            self.assertFalse(layout.exists())
            publish(2, 'ultra', state='starting'); unavailable()
            self.assertEqual(manager.active_mode, 'ultra')  # Negotiation alone is not scene readiness.
            publish(3, 'ultra')
            self.assertEqual(manager.request({'action': 'status'})['ultra_cluster']['active'], style)
            with self.assertRaises(ValueError): manager.request({'action': 'cluster_layout', 'layout': style})
            self.assertFalse(layout.exists())
            manager.request({'action': 'cluster_layout', 'layout': style, 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}})
            manager.request({'action': 'cluster_input', 'command': 'next', 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}})
            navigation = Path(str(layout) + '.input').read_bytes()
            self.assertTrue(navigation.startswith(f'HUI1\n{"a" * 32}\n1\n'.encode()))
            for command in ('music', None, []):
                with self.assertRaises(ValueError):
                    manager.request({'action': 'cluster_input', 'command': command, 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}})
            # Without iAP/Bluetooth controls, native transport uses Ultra media keys.
            manager.bluez.phone = lambda address: None
            for command, key in (('play', 'play_pause'), ('pause', 'play_pause'),
                                 ('next', 'next_track'), ('previous', 'previous_track')):
                reply = manager.request({'action': 'media', 'phone': A, 'generation': manager.generation, 'command': command})
                self.assertEqual(reply['message'], 'Media key requested')
                self.assertTrue(Path(str(layout) + '.input').read_text().endswith(f'\n{key}\n'))
            with self.assertRaises(ValueError):
                manager.request({'action': 'media', 'phone': A, 'generation': manager.generation, 'command': 'volume_up'})
            current = {'nonce': 'a' * 32, 'connectionGeneration': '1'}
            manager.request({'action': 'cluster_format', 'renderer': 'ultra', 'format': style, 'publisher': current})
            self.assertEqual(layout.read_text(), f'HUL1 {"a" * 32} 1\n{style}\n')
            for bad in ({'action': 'cluster_format', 'renderer': 'ultra', 'format': 'sport', 'publisher': current},
                        {'action': 'cluster_format', 'renderer': 'ultra', 'format': style, 'publisher': dict(current, nonce='b' * 32)},
                        {'action': 'cluster_format', 'renderer': 'native', 'format': 'sport', 'publisher': current},
                        {'action': 'status', 'format': 'sport'}):
                with self.assertRaises(ValueError):
                    manager.request(bad)
            for app in ('maps', 'music', 'phone'):
                manager.request({'action': 'phone_app', 'app': app, 'publisher': current})
                self.assertTrue(Path(str(layout) + '.input').read_text().endswith(f'\nopen_{app}\n'))
            for bad in ({'action': 'phone_app', 'app': 'settings', 'publisher': current},
                        {'action': 'phone_app', 'app': 'maps', 'publisher': {'nonce': 'b' * 32, 'connectionGeneration': '1'}},
                        {'action': 'phone_app', 'app': 'maps'}):
                with self.assertRaises(ValueError):
                    manager.request(bad)
            navigation = Path(str(layout) + '.input').read_bytes()
            publish(4, 'ultra', state='home')
            self.assertIsNotNone(manager.request({'action': 'status'})['ultra_cluster'])
            manager.request({'action': 'cluster_layout', 'layout': style, 'publisher': {'nonce': 'a' * 32, 'connectionGeneration': '1'}})
            preference = layout.read_bytes()
            self.assertEqual(preference, (f'HUL1 {"a" * 32} 1\n{style}\n').encode())
            # Retained catalog/selection survive, but another launch's mode is
            # unknown until its own generation-bound receiver publication.
            manager.carplay.reset('b' * 32); unavailable()
            publish(1, 'ultra', nonce='b' * 32, age=2_000_000_000); unavailable()
            publish(2, 'ultra', nonce='b' * 32)
            unavailable()  # A ready successor cannot admit the previous launch's same-pack catalogue.
            catalogue('b' * 32)
            self.assertIsNotNone(manager.request({'action': 'status'})['ultra_cluster'])
            publish(3, 'ultra', nonce='b' * 32, generation='2')
            unavailable()  # Reconnect inside one receiver launch also requires the new publisher.
            catalogue('b' * 32, '2')
            self.assertIsNotNone(manager.request({'action': 'status'})['ultra_cluster'])
            for publisher in ({'nonce': 'a' * 32, 'connectionGeneration': '1'},
                              {'nonce': 'b' * 32, 'connectionGeneration': '1'},
                              {'nonce': 'b' * 32, 'connectionGeneration': 2}):
                with self.assertRaises(ValueError):
                    manager.request({'action': 'cluster_layout', 'layout': style, 'publisher': publisher})
                with self.assertRaises(ValueError):
                    manager.request({'action': 'cluster_input', 'command': 'next', 'publisher': publisher})
            self.assertEqual(Path(str(layout) + '.input').read_bytes(), navigation)
            self.assertEqual(layout.read_bytes(), preference)  # An old rendered button never writes into its successor.
            manager.stop(); unavailable()
            self.assertEqual(layout.read_bytes(), preference)

    def test_native_cluster_options_reach_current_selector_only(self):
        from phone_manager import Manager
        from phone_state import atomic_json
        from carplay_readiness import clock_ns
        import json
        import os

        class Phones:
            def phones(self):
                return []

        class Child:
            def poll(self):
                return None

        with tempfile.TemporaryDirectory(prefix='pm-options-', dir='/tmp') as folder:
            root = Path(folder).resolve()
            root.chmod(0o700)
            owner = os.getuid(), os.getgid()
            layout = root / 'layout'
            profile = root / 'ui.conf'
            profile.write_text(f'clusterLayoutFile={layout}\n')
            display = root / 'standard.conf'
            display.write_text('explicit geometry fixture')
            config = root / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root / 'pairs'),
                                          'display_config': str(display),
                                          'ultra_installed_ui': str(profile)}))
            manager = Manager(config, root, display, owner, bluez=Phones())
            manager.active = A
            manager.state.select(A)
            manager.child = Child()
            nonce = 'a' * 32
            manager.carplay.reset(nonce)
            style = '00000000-1111-2222-3333-444444444444'
            zone = '00000000-1111-2222-3333-555555555555'
            component = '00000000-1111-2222-3333-666666666666'
            publisher = {'nonce': nonce, 'connectionGeneration': '1'}
            atomic_json(manager.carplay.path, {
                'version': 2, **publisher, 'sequence': '1',
                'updatedMonotonicNs': str(clock_ns()), 'state': 'ready',
                'returnReady': True, 'negotiatedMode': 'ultra'}, owner)
            atomic_json(layout.with_name(layout.name + '.status'), {
                'version': 2, **publisher, 'sequence': '1',
                'updatedMonotonicNs': str(clock_ns()), 'active': style,
                'pending': '', 'ready': True, 'idle': True,
                'transitioning': False, 'error': '',
                'choices': [{'id': style, 'name': 'Round'}],
                'palettes': [{'id': 'Blue', 'name': 'Blue'}],
                'contents': [{'id': component, 'zone': zone, 'name': 'DCA_Map',
                              'active': False}]}, owner)
            base_request = {'action': 'cluster_layout', 'layout': style,
                            'publisher': publisher}
            for options in ({'palette': 'Blue'}, {'zone': zone, 'component': component},
                            {'palette': 'Blue', 'zone': zone, 'component': component}):
                manager.request({**base_request, **options})
                self.assertEqual(layout.read_text(),
                                 f'HUL2 {nonce} 1\n{style}\n{options.get("palette", "")}\n'
                                 f'{options.get("zone", "")}\n{options.get("component", "")}\n')
            retained = layout.read_bytes()
            for options in ({'palette': 'unknown'}, {'zone': zone},
                            {'component': component}, {'palette': True}):
                with self.assertRaises(ValueError):
                    manager.request({**base_request, **options})
                self.assertEqual(layout.read_bytes(), retained)
            for action in ('status', 'connect', 'disconnect', 'cluster_page', 'media'):
                for field, value in (('palette', 'Blue'), ('zone', zone), ('component', component)):
                    with self.assertRaises(ValueError):
                        manager.request({'action': action, field: value})
                    self.assertEqual(layout.read_bytes(), retained)
            with self.assertRaises(ValueError):
                manager.request({**base_request, 'palette': 'Blue',
                                 'publisher': {**publisher, 'connectionGeneration': '2'}})
            self.assertEqual(layout.read_bytes(), retained)

    def test_backup_cluster_survives_phone_session_changes(self):
        from phone_manager import Manager, cluster_display, cluster_wayland_name
        import json
        class FakeProcess:
            def __init__(self): self.terminated = False
            def poll(self): return 0 if self.terminated else None
            def terminate(self): self.terminated = True
            def wait(self, timeout): return 0
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            display = root / 'display.conf'
            display.write_text('main.wayland=headunit-0\ncluster.wayland=headunit-cluster\ncluster.width=1920\ncluster.height=720\n')
            self.assertEqual(cluster_wayland_name(display), 'headunit-cluster')
            self.assertEqual(cluster_display(display), ('headunit-cluster', (1920, 720)))
            config = root / 'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display),
                                          'bin_dir': str(root), 'fallback_cluster': True}))
            manager = Manager(config, root, display, None, bluez=object())
            self.assertEqual(manager.fallback_size, (1920, 720))
            self.assertEqual(manager.fallback_command(root/'headunit-cluster-wayland'),
                             [str(root/'headunit-cluster-wayland'), '--size', '1920', '720',
                              '--media-file', str(root/'media.json')])
            manager.base['native_cluster_config'] = root / 'native-cluster.conf'
            self.assertEqual(manager.fallback_command(root/'headunit-cluster-wayland')[-2:],
                             ['--native-cluster-config', str(root / 'native-cluster.conf')])
            backup = FakeProcess()
            manager.fallback = backup
            manager.stop()
            self.assertIsNone(manager.child)
            self.assertFalse(backup.terminated)
            manager.shutdown()
            self.assertTrue(backup.terminated)
            display.write_text('cluster.wayland=../../outside\ncluster.width=1920\ncluster.height=720\n')
            with self.assertRaises(ValueError): cluster_wayland_name(display)
            display.write_text('cluster.wayland=..\ncluster.width=1920\ncluster.height=720\n')
            with self.assertRaises(ValueError): cluster_wayland_name(display)
            display.write_text('cluster.wayland=headunit-cluster\ncluster.width=1920\ncluster.height=9000\n')
            with self.assertRaises(ValueError): cluster_display(display)

    def test_physical_backup_and_receiver_never_bind_the_same_vehicle_socket(self):
        from phone_manager import Manager
        import json, os
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve()
            display=root/'display.conf'
            display.write_text('cluster.wayland=wayland-0\ncluster.width=1920\ncluster.height=720\n')
            config=root/'receiver.json'
            values={'pair_store':str(root/'pairs'),'display_config':str(display),'bin_dir':str(root),
                    'fallback_cluster':True,'vehicle_socket':str(root/'receiver.sock')}
            config.write_text(json.dumps(values))
            with self.assertRaises(ValueError):
                Manager(config,root,display,(os.getuid(),os.getgid()),bluez=object())
            bus=root/'vehicle.json'
            bus.write_text(json.dumps({'version':1,'input':str(root/'adapter.sock'),
                                      'consumers':{'backup':str(root/'backup.sock'),'receiver':str(root/'receiver.sock')}}))
            bus.chmod(0o600)
            manager=Manager(config,root,display,(os.getuid(),os.getgid()),bluez=object(),fanout_config=bus)
            command=manager.fallback_command(root/'headunit-cluster-wayland')
            self.assertIn(str(root/'backup.sock'),command)
            self.assertNotIn(str(root/'receiver.sock'),command)
            self.assertNotIn('--simulate',command)

    def test_trip_selection_through_manager_preserves_standard_map_availability(self):
        from phone_manager import Manager
        from carplay_readiness import clock_ns
        from phone_state import atomic_json
        import json, os
        class Phones:
            def phones(self): return []
        class Process:
            def poll(self): return None
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            display=root/'standard.conf'; display.write_text('test')
            page=root/'cluster-page.txt'
            config=root/'receiver.json'
            config.write_text(json.dumps({'pair_store':str(root/'pairs'),
                                          'display_config':str(display),'cluster_page':str(page)}))
            manager=Manager(config,root,display,(os.getuid(),os.getgid()),bluez=Phones())
            status=manager.request({'action':'status'})
            self.assertIn('trip',status['cluster_pages']); self.assertNotIn('map',status['cluster_pages'])
            for identifier in ('minimal', 'sport', 'gauges'):
                manager.request({'action': 'cluster_format', 'renderer': 'native', 'format': identifier})
                model = manager.request({'action': 'status'})['cluster_formats']
                self.assertEqual(model['native']['selected'], identifier)
            with self.assertRaises(ValueError):
                manager.request({'action': 'cluster_format', 'renderer': 'native', 'format': 'map'})
            manager.request({'action':'cluster_page','page':'trip'})
            self.assertEqual(manager.request({'action':'status'})['cluster_page'],'trip')
            with self.assertRaises(ValueError): manager.request({'action':'cluster_page','page':'map'})
            self.assertEqual(page.read_text(),'trip\n')
            manager.child=Process()
            self.assertNotIn('map',manager.request({'action':'status'})['cluster_pages'])
            manager.active=A; manager.state.select(A)
            manager.carplay.reset('a'*32)
            atomic_json(manager.carplay.path, {'version':2,'nonce':'a'*32,'sequence':'1',
                        'negotiatedMode':'standard',
                        'connectionGeneration':'1','updatedMonotonicNs':str(clock_ns()),
                        'state':'ready','returnReady':True},manager.owner)
            self.assertIn('map',manager.request({'action':'status'})['cluster_pages'])
            manager.request({'action':'cluster_page','page':'map'})
            self.assertEqual(manager.request({'action':'status'})['cluster_page'],'map')
            manager.request({'action':'cluster_page','page':'trip'})
            self.assertEqual(manager.request({'action':'status'})['cluster_page'],'trip')
            manager.child=None; manager.active_mode=None
            self.assertEqual(manager.request({'action':'status'})['cluster_page'],'trip')
            with self.assertRaises(ValueError): manager.request({'action':'cluster_page','page':'unknown'})
            self.assertEqual(page.read_text(),'trip\n')

    def test_headunit_audio_requires_owned_active_child_generation_and_route(self):
        from phone_manager import Manager
        import json, os, time
        class Phones:
            def phones(self): return [{'address': A, 'name': 'Phone', 'connected': True}]
            def phone(self, value):
                if value != A: raise ValueError('Not paired')
                return self.phones()[0]
            def media_command(self, *_): raise AssertionError('Head-unit audio reached Bluetooth')
            def media(self, *_): return {'available': False}
        class Child:
            returncode = 0
            running = True
            def poll(self): return None if self.running else 0
            def terminate(self): self.running = False
            def wait(self, timeout): return 0
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); display = root/'display.conf'; display.write_text('test')
            config = root/'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display), 'audio_sink': 'fakesink'}))
            manager = Manager(config, root, display, (os.getuid(), os.getgid()), Phones())
            route = 'a' * 32
            from audio_control import monotonic_time_ns
            value = {'version': 1, 'updatedMonotonicNs': str(monotonic_time_ns()), 'available': True,
                     'route': route, 'acceptedSequence': '0', 'appliedSequence': '0',
                     'requestedVolumePercent': 100, 'requestedMuted': False,
                     'volumePercent': 100, 'muted': False, 'error': ''}
            manager.audio.state_path.write_text(json.dumps(value)); manager.audio.state_path.chmod(0o600)
            self.assertFalse(manager.request({'action': 'status'})['audio']['available'])
            manager.active, manager.child = A, Child(); manager.state.select(A)
            self.assertTrue(manager.request({'action': 'status'})['audio']['available'])
            command = {'action': 'audio', 'phone': A, 'generation': manager.generation,
                       'route': route, 'volumePercent': 72, 'muted': True}
            manager.request(command)
            self.assertEqual(json.loads(manager.audio.control_path.read_text())['sequence'], '1')
            status = manager.request({'action': 'status'})['audio']
            self.assertTrue(status['pending']); self.assertEqual(status['volumePercent'], 100)
            original = manager.audio.control_path.read_bytes()
            for changes in ({'generation': -1}, {'generation': False}, {'route': 'b' * 32},
                            {'volumePercent': True}, {'muted': 1}, {'command': 'volume_up'}):
                with self.subTest(changes=changes), self.assertRaises(ValueError): manager.request(command | changes)
                self.assertEqual(manager.audio.control_path.read_bytes(), original)
            incomplete = dict(command); incomplete.pop('muted')
            with self.assertRaises(ValueError): manager.request(incomplete)
            manager.state.select(B)
            self.assertFalse(manager.request({'action': 'status'})['audio']['available'])
            with self.assertRaises(ValueError): manager.request(command)
            manager.state.select(A)
            manager.audio.state_path.write_text(json.dumps(value | {'route': 'b' * 32}))
            with self.assertRaises(ValueError): manager.request(command)
            self.assertEqual(manager.audio.control_path.read_bytes(), original)
            manager.child.running = False
            self.assertFalse(manager.request({'action': 'status'})['audio']['available'])
            with self.assertRaises(ValueError): manager.request(command | {'route': 'b' * 32})
            manager.tick()
            self.assertFalse(manager.audio.state_path.exists()); self.assertFalse(manager.audio.control_path.exists())
            self.assertTrue(manager.audio.preference_path.exists())

    def test_manual_camera_is_phone_independent_and_outside_phone_lock(self):
        from phone_manager import Manager
        from unittest.mock import patch
        import json, os, threading
        class Camera:
            requested = False
            closed = False
            def request(self, enabled): self.requested = enabled
            def snapshot(self): return {'configured': True, 'requested': self.requested, 'phase': 'active', 'error': ''}
            def snapshot_with_status_observation(self):
                from camera_manual import unavailable_status_observation
                return self.snapshot(), unavailable_status_observation()
            def close(self): self.closed = True
        class Phones:
            def phones(self): return []
            def phone(self, _): raise AssertionError('Camera operation reached a phone')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); display = root / 'display.conf'; display.write_text('test')
            config = root / 'receiver.json'; profile = root / 'camera.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display),
                                          'camera_manual_profile': str(profile)}))
            # Admission still reads the real bounded profile before the explicit
            # worker fixture. A mocked worker does not make a missing config valid.
            manual = json.loads((Path(__file__).resolve().parents[1] / 'config/camera-manual.example.json').read_text())
            for key, name in {'requestFile': 'manual.request', 'ownerFile': 'manual.owner',
                              'statusFile': 'camera.status', 'controlSocket': 'manual.sock'}.items():
                manual[key] = str(root / name)
            profile.write_text(json.dumps(manual))
            profile.chmod(0o600)
            camera = Camera()
            with patch('phone_manager.ManualCamera', return_value=camera) as constructor:
                manager = Manager(config, root, display, (os.getuid(), os.getgid()), Phones())
                constructor.assert_called_once_with(profile, (os.getuid(), os.getgid()))
            done = threading.Event(); failures = []
            def action():
                try: manager.request({'action': 'camera', 'enabled': True})
                except Exception as error: failures.append(error)
                finally: done.set()
            with manager.lock:
                thread = threading.Thread(target=action); thread.start()
                self.assertTrue(done.wait(1), 'Camera command waits for phone operation lock')
            thread.join(1); self.assertFalse(failures)
            self.assertTrue(manager.request({'action': 'status'})['camera']['requested'])
            manager.request({'action': 'camera', 'enabled': False})
            status = manager.request({'action': 'status'})['camera']
            self.assertFalse(status['requested']); self.assertEqual(status['phase'], 'active')
            for value in ({'action': 'camera'}, {'action': 'camera', 'enabled': 1},
                          {'action': 'camera', 'enabled': False, 'phone': A}, {'action': 'status', 'enabled': True}):
                with self.subTest(value=value), self.assertRaises(ValueError): manager.request(value)
            manager.shutdown(); self.assertTrue(camera.closed)

    def test_camera_status_real_manager_endpoint_publication_and_retirement(self):
        import json
        import os
        import socket
        import time
        from unittest.mock import patch
        from camera_manual import ManualCamera
        from phone_manager_endpoint import ManagerEndpoint

        temporary = self.enterContext(tempfile.TemporaryDirectory(prefix='hu-cso-', dir='/tmp'))
        root = Path(temporary).resolve()
        manager = self.quarantine_fixture(root)
        self.addCleanup(manager.shutdown)
        # Actual manager callback and endpoint; Bluetooth/receiver are explicit
        # fixtures. The dated HCS1 writer is synthetic, with no display/camera.
        with ManagerEndpoint(root) as endpoint:
            def status():
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.settimeout(1)
                    client.connect(str(endpoint.path))
                    client.sendall(b'{"action":"status"}\n')
                    self.assertTrue(endpoint.serve_once(manager.request))
                    raw = bytearray()
                    while True:
                        chunk = client.recv(65536)
                        if not chunk:
                            break
                        raw.extend(chunk)
                    value = json.loads(raw)
                    self.assertTrue(value['ok'])
                    return value

            empty = status()
            self.assertEqual(empty['camera'], {'configured': False, 'requested': False,
                                               'phase': 'unavailable', 'error': ''})
            self.assertEqual(empty['camera_status_observation']['state'], 'not_configured')
            manual = json.loads((Path(__file__).resolve().parents[1] / 'config/camera-manual.example.json').read_text())
            manual['enabled'] = True
            for key, leaf in (('requestFile', 'request'), ('ownerFile', 'owner'),
                              ('statusFile', 'status'), ('controlSocket', 'manual.sock')):
                manual[key] = str(root / leaf)
            profile = root / 'manual.json'
            profile.write_text(json.dumps(manual))
            profile.chmod(0o600)
            manager.camera = ManualCamera(profile, clock_ns=time.monotonic_ns)
            original_ns = time.monotonic_ns()
            writer = root / 'status'
            writer.write_text(f'HCS1 {original_ns} 1 {original_ns} active\n')
            writer.chmod(0o600)
            published = status()
            receipt = published['camera_status_observation']['lastObservation']
            self.assertEqual(receipt['observedNs'], str(original_ns))
            self.assertEqual(published['camera']['phase'], 'active')
            self.assertFalse(published['camera']['requested'])
            self.assertEqual(published['camera_status_observation']['availability'], 'available')
            self.assertEqual(status()['camera_status_observation']['lastObservation'], receipt)
            writer.unlink()
            lost = status()
            self.assertEqual(lost['camera']['phase'], 'unavailable')
            self.assertEqual(lost['camera_status_observation']['lastObservation'], receipt)
            self.assertEqual(lost['camera_status_observation']['availability'], 'unavailable')
            manager.base['camera_state_config'] = 'synthetic signed owner marker'
            with patch.object(manager, 'carplay_snapshot', return_value={'returnReady': False}):
                manager.active_mode = 'standard'
                suppressed = status()
                self.assertEqual(suppressed['camera']['phase'], 'stopped')
                self.assertEqual(suppressed['camera_status_observation']['state'], 'suppressed')
                self.assertIsNone(suppressed['camera_status_observation']['lastObservation'])
                manager.active_mode = 'ultra'
                self.assertEqual(status()['camera_status_observation']['state'], 'suppressed')
                manager.retirement_required = True
                self.assertEqual(status()['camera_status_observation']['state'], 'suppressed')
                manager.retirement_required = False
            before = manager.generation
            manager.stop()
            disconnected = status()
            self.assertGreater(disconnected['generation'], before)
            self.assertEqual(disconnected['camera']['phase'], 'stopped')
            self.assertFalse(disconnected['camera_status_observation']['eligible'])
            manager.base.pop('camera_state_config')
            # A native-only owner is phone independent; its existing status
            # validator still needs a new receipt after the observed loss.
            writer.write_text(f'HCS1 {original_ns} 1 {original_ns} active\n')
            writer.chmod(0o600)
            self.assertEqual(status()['camera_status_observation']['availability'], 'unavailable')
            newer = time.monotonic_ns()
            writer.write_text(f'HCS1 {newer} 1 {newer} idle\n')
            restored = status()
            self.assertEqual(restored['camera_status_observation']['lastObservation']['instanceStartNs'], str(newer))
            self.assertEqual(restored['camera_status_observation']['availability'], 'available')
            manager.shutdown()
            retired = status()
            self.assertEqual(retired['camera_status_observation']['state'], 'manual_owner_unavailable')
            self.assertFalse(retired['camera_status_observation']['eligible'])
            self.assertEqual(retired['camera_status_observation']['availability'], 'unavailable')
            self.assertEqual(retired['camera_status_observation']['lastObservation']['observedNs'], str(newer))
        self.assertFalse(endpoint.path.exists())

    def test_managed_launch_restores_desired_audio_without_claiming_applied(self):
        from phone_manager import Manager
        from unittest.mock import patch
        import json, os, time
        class Phones:
            def phone(self, _): return {}
            def disconnect(self, _): pass
            def phones(self): return []
        class Child:
            def poll(self): return None
            def terminate(self): pass
            def wait(self, timeout): return 0
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); display = root/'display.conf'; display.write_text('test')
            config = root/'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display),
                                          'audio_sink': 'fakesink', 'audio_volume_percent': 46, 'audio_muted': False}))
            owner = (os.getuid(), os.getgid())
            manager = Manager(config, root, display, owner, Phones())
            manager.active, manager.child = A, Child(); manager.state.select(A)
            from audio_control import monotonic_time_ns
            audio = {'version': 1, 'updatedMonotonicNs': str(monotonic_time_ns()), 'available': True,
                     'route': 'a' * 32, 'acceptedSequence': '0', 'appliedSequence': '0',
                     'requestedVolumePercent': 46, 'requestedMuted': False, 'volumePercent': 46,
                     'muted': False, 'error': ''}
            manager.audio.state_path.write_text(json.dumps(audio)); manager.audio.state_path.chmod(0o600)
            manager.request({'action': 'audio', 'phone': A, 'generation': manager.generation,
                             'route': 'a' * 32, 'volumePercent': 61, 'muted': True})
            manager.stop()
            self.assertFalse(manager.audio.control_path.exists()); self.assertFalse(manager.audio.state_path.exists())
            manager = Manager(config, root, display, owner, Phones())
            with patch('phone_manager.subprocess.run'), patch('phone_manager.subprocess.Popen', return_value=Child()):
                manager.start(A)
            saved = json.loads((root/'managed-receiver.json').read_text())
            self.assertEqual(saved['audio_volume_percent'], 61); self.assertTrue(saved['audio_muted'])
            self.assertEqual(Path(saved['audio_control']), manager.audio.control_path)
            self.assertEqual(Path(saved['audio_state']), manager.audio.state_path)
            self.assertFalse(manager.request({'action': 'status'})['audio']['available'])
            manager.stop()

    def test_boot_restore_keeps_selected_preference_when_start_is_rejected(self):
        from phone_manager import Manager
        from unittest.mock import patch
        import json, os
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); display = root/'standard.conf'; display.write_text('fixture')
            config = root/'receiver.json'; config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display)}))
            manager = Manager(config, root, display, (os.getuid(), os.getgid()), bluez=object())
            self.assertFalse(manager.restore_selected())
            manager.state.select(A)
            with patch.object(manager, 'start', side_effect=ValueError('phone unavailable')) as start:
                self.assertFalse(manager.restore_selected())
                start.assert_called_once_with(A)
            self.assertEqual(manager.state.selected, A)
            self.assertIsNone(manager.child)
            self.assertIn('choose Connect to retry', manager.error)

    def test_receiver_exit_retires_media_even_during_adapter_hold(self):
        from phone_manager import Manager
        from unittest.mock import patch
        import json, os, subprocess
        class Phones:
            def phone(self, _): return {}
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); display = root/'standard.conf'; display.write_text('fixture')
            config = root/'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display)}))
            manager = Manager(config, root, display, (os.getuid(), os.getgid()), bluez=Phones())
            manager.state.select(A)
            # An actual local child models only launcher lifetime. No receiver,
            # radios, signed resources or display owners are started.
            child = subprocess.Popen([sys.executable, '-c',
                                      'import sys; sys.stdin.buffer.read(1); sys.exit(7)'],
                                     stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                manager.child = child; manager.active = A; manager.active_mode = 'ultra'; manager.generation = 7
                manager.carplay.reset('1' * 32)
                with patch('phone_manager.time.monotonic', return_value=100.):
                    manager.request({'action': 'media_update', 'phone': A, 'generation': 7,
                                     'media': {'available': True, 'title': 'Retiring track', 'status': 'playing'}})
                    manager.iap_media_path.write_text('{"available":true,"title":"Retiring iAP"}')
                    manager.iap_art_path.write_bytes(b'original synthetic cover fixture')
                    manager.iap_art_hash = 'f' * 64
                    self.assertEqual(manager.external_until, 103.)
                    self.assertTrue(manager.media.last['available'])
                    child.stdin.write(b'x'); child.stdin.flush(); child.wait(timeout=5)
                    manager.tick()
                    self.assertFalse(manager.media.last['available'])
                    self.assertFalse(json.loads(manager.media.path.read_text())['available'])
                    self.assertEqual(manager.external_until, 0)
                    self.assertFalse(manager.iap_media_path.exists()); self.assertFalse(manager.iap_art_path.exists())
                    self.assertEqual(manager.iap_art_hash, '')
                    self.assertIsNone(manager.child); self.assertIsNone(manager.active); self.assertIsNone(manager.active_mode)
                    self.assertIsNone(manager.carplay.nonce); self.assertEqual(manager.generation, 8)
                    self.assertIn('Receiver stopped (7); choose Connect to retry', manager.error)
                    saved = PhoneState(manager.state.path)
                    self.assertEqual(saved.selected, A); self.assertIn(A, saved.phones)
                    with self.assertRaises(ValueError):
                        manager.request({'action': 'media_update', 'phone': A, 'generation': 7,
                                         'media': {'available': True, 'title': 'Late retired track'}})
                    manager.tick(); self.assertEqual(manager.generation, 8)
            finally:
                if child.poll() is None:
                    child.terminate(); child.wait(timeout=5)
                child.stdin.close()

    def test_owned_receiver_inherits_only_its_validated_manager_lock(self):
        from phone_manager import Manager
        from phone_manager_endpoint import ManagerEndpoint
        from unittest.mock import patch
        import json, os
        class Phones:
            def phone(self, _): return {}
            def disconnect(self, _): pass
        class Child:
            def poll(self): return None
            def terminate(self): pass
            def wait(self, timeout): return 0
        with tempfile.TemporaryDirectory(prefix='pm-launch-', dir='/tmp') as folder:
            root = Path(folder).resolve(); root.chmod(0o700)
            display = root/'standard.conf'; display.write_text('explicit validator fixture')
            config = root/'receiver.json'; config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display)}))
            owner = os.getuid(), os.getgid()
            with ManagerEndpoint(root, owner) as endpoint:
                manager = Manager(config, root, display, owner, bluez=Phones(),
                                  receiver_lock_fd=endpoint.inherited_lock_fd)
                manager.state.select(A)
                with patch('phone_manager.subprocess.run') as check, \
                        patch('phone_manager.subprocess.Popen', return_value=Child()) as launch:
                    self.assertTrue(manager.restore_selected())
                self.assertEqual(launch.call_args.kwargs['pass_fds'], (endpoint.inherited_lock_fd,))
                self.assertNotIn('pass_fds', check.call_args.kwargs)
                self.assertEqual(manager.active, A)
                self.assertIsNone(manager.active_mode)
                manager.shutdown()
            self.assertFalse((root/'manager.sock').exists())

    def test_scoped_disconnect_preserves_pairing_and_refuses_replaced_sessions(self):
        from phone_manager import Manager
        import json
        class Phones:
            def __init__(self): self.disconnected = []
            def disconnect(self, phone): self.disconnected.append(phone)
        class Child:
            def __init__(self): self.terminated = False
            def poll(self): return None if not self.terminated else 0
            def terminate(self): self.terminated = True
            def wait(self, timeout): return 0
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); display = root/'standard.conf'; display.write_text('explicit display fixture')
            pairs = root/'pairs'; pairs.mkdir(); retained = pairs/'retained-key'; retained.write_bytes(b'fixture')
            config = root/'receiver.json'
            config.write_text(json.dumps({'pair_store': str(pairs), 'display_config': str(display)}))
            phones = Phones(); manager = Manager(config, root, display, None, bluez=phones)
            manager.state.select(A)
            child = Child(); manager.child = child; manager.active = A; manager.active_mode = 'ultra'; manager.generation = 7
            for bad in ({'action': 'disconnect', 'phone': B, 'generation': 7},
                        {'action': 'disconnect', 'phone': A, 'generation': 6},
                        {'action': 'disconnect', 'phone': A, 'generation': True},
                        {'action': 'disconnect', 'phone': A},
                        {'action': 'disconnect', 'generation': 7},
                        {'action': 'disconnect', 'phone': A, 'generation': 7, 'mode': 'ultra'}):
                with self.assertRaises(ValueError): manager.request(bad)
                self.assertFalse(child.terminated); self.assertEqual(manager.active, A)
            self.assertEqual(manager.request({'action': 'disconnect', 'phone': A, 'generation': 7}), {'message': 'Disconnected'})
            self.assertTrue(child.terminated); self.assertIsNone(manager.child); self.assertIsNone(manager.active)
            self.assertEqual(phones.disconnected, [A]); self.assertEqual(manager.generation, 8)
            self.assertEqual(manager.state.selected, A); self.assertIn(A, manager.state.phones)
            self.assertIsNone(manager.active_mode)
            self.assertTrue(retained.exists()); self.assertFalse(manager.media.last['available'])
            with self.assertRaisesRegex(ValueError, 'Session changed'):
                manager.request({'action': 'disconnect', 'phone': A, 'generation': 7})
            # Retain the explicit local manager API for existing owner clients.
            self.assertEqual(manager.request({'action': 'disconnect'}), {'message': 'Disconnected'})
            self.assertEqual(phones.disconnected, [A])

    def test_pairing_rechecks_death_observed_after_initial_admission(self):
        class Pairing:
            def __init__(self):
                self.calls = []
            def start(self):
                self.calls.append('start')
        class Child:
            def __init__(self):
                self.codes = iter((None, None, -9))
            def poll(self):
                return next(self.codes, -9)
        with tempfile.TemporaryDirectory() as folder:
            manager = self.quarantine_fixture(Path(folder))
            pairing = Pairing()
            manager.pairing = pairing
            manager.child = Child()
            with self.assertRaisesRegex(RuntimeError, 'retire the whole manager service'):
                manager.request({'action': 'pairing_start'})
            self.assertEqual(pairing.calls, [])
            self.assertTrue(manager.retirement_required)

    def test_pairing_actions_are_explicit_and_cannot_compete_with_the_receiver(self):
        from phone_manager import Manager
        from unittest.mock import patch
        import json
        session, prompt = '1' * 32, '2' * 32
        class Pairing:
            def __init__(self): self.phase = 'idle'; self.calls = []
            def snapshot(self): return {'supported': True, 'phase': self.phase, 'session': session}
            def start(self): self.calls.append('start'); self.phase = 'discoverable'
            def cancel(self, value): self.calls.append(('cancel', value)); self.phase = 'idle'
            def confirm(self, value, key, accepted): self.calls.append(('confirm', value, key, accepted))
            def close(self): self.calls.append('close'); self.phase = 'idle'
            def wait_closed(self): self.calls.append('retired')
        class Phones:
            def phones(self): return []
        class Running:
            def poll(self): return None
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); display = root/'standard.conf'; display.write_text('fixture')
            config = root/'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display)}))
            pairing = Pairing(); manager = Manager(config, root, display, None, bluez=Phones(), pairing=pairing)
            self.assertTrue(manager.request({'action': 'status'})['pairing']['supported'])
            manager.child = Running()
            with self.assertRaisesRegex(ValueError, 'Disconnect CarPlay'):
                manager.request({'action': 'pairing_start'})
            self.assertEqual(pairing.calls, [])
            manager.child = None
            manager.request({'action': 'pairing_start'})
            self.assertEqual(pairing.phase, 'discoverable')
            with self.assertRaisesRegex(ValueError, 'Finish or cancel'):
                manager.start(A)
            manager.request({'action': 'pairing_confirm', 'session': session, 'prompt': prompt, 'accepted': False})
            self.assertEqual(pairing.calls[-1], ('confirm', session, prompt, False))
            manager.request({'action': 'pairing_cancel', 'session': session})
            self.assertEqual(pairing.calls[-1], ('cancel', session))
            for data in ({'action': 'pairing_confirm', 'session': session, 'prompt': prompt, 'accepted': 1},
                         {'action': 'pairing_start', 'phone': A}, {'action': 'pairing_cancel'},
                         {'action': 'pairing_confirm', 'session': session, 'accepted': True},
                         {'action': 'status', 'accepted': True}, {'action': 'camera', 'session': session}):
                before = list(pairing.calls)
                with self.subTest(data=data), self.assertRaises(ValueError): manager.request(data)
                self.assertEqual(pairing.calls, before)
            # Shutdown withdraws pending user consent before receiver cleanup.
            with patch.object(manager, 'stop', side_effect=lambda: self.assertEqual(pairing.calls[-1], 'close')):
                manager.shutdown()
            self.assertEqual(pairing.calls[-1], 'retired')

    def test_shutdown_retains_real_endpoint_until_pairing_retires_even_if_receiver_stop_fails(self):
        from phone_manager import Manager
        from phone_manager_endpoint import ManagerEndpoint
        from unittest.mock import patch
        import json, threading
        requested, waiting, retire = threading.Event(), threading.Event(), threading.Event()
        errors = []
        class Pairing:
            def close(self): requested.set()
            def wait_closed(self):
                waiting.set()
                if not retire.wait(3): raise AssertionError('Fixture pairing cleanup was not released')
        class Phones:
            def phones(self): return []
        with tempfile.TemporaryDirectory(prefix='pm-retire-', dir='/tmp') as folder:
            root = Path(folder).resolve(); root.chmod(0o700)
            display = root/'standard.conf'; display.write_text('fixture')
            config = root/'receiver.json'
            config.write_text(json.dumps({'pair_store': str(root/'pairs'), 'display_config': str(display)}))
            def shutdown():
                try:
                    with ManagerEndpoint(root):
                        manager = Manager(config, root, display, None, bluez=Phones(), pairing=Pairing())
                        with patch.object(manager, 'stop', side_effect=RuntimeError('receiver fixture still stopping')):
                            manager.shutdown()
                except Exception as error:
                    errors.append(error)
            worker = threading.Thread(target=shutdown)
            worker.start()
            try:
                self.assertTrue(requested.wait(2)); self.assertTrue(waiting.wait(2))
                self.assertTrue(worker.is_alive())
                with self.assertRaisesRegex(ValueError, 'live owner'):
                    ManagerEndpoint(root)
            finally:
                retire.set(); worker.join(3)
            self.assertFalse(worker.is_alive())
            self.assertEqual(len(errors), 1)
            self.assertRegex(str(errors[0]), 'receiver fixture still stopping')
            with ManagerEndpoint(root):
                self.assertTrue((root/'manager.sock').is_socket())

if __name__ == '__main__': unittest.main()
