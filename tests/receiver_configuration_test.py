# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import json
import contextlib
import io
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from filesystem_fixture import change_metadata
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from receiver_configuration import load_receiver_configuration, configured_identity, validate_device_settings
from generate_test_ap import main as generate_ap


class ReceiverConfigurationTest(unittest.TestCase):
    def test_trip_history_is_boolean_opt_in_and_requires_sport(self):
        validate_device_settings({'trip_history': False})
        validate_device_settings({'sport_telemetry': True, 'trip_history': True})
        with self.assertRaises(ValueError):
            validate_device_settings({'trip_history': True})
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'receiver.json'
            for value in (1, 'true', None):
                path.write_text(json.dumps({'display_config': 'display.conf', 'sport_telemetry': True, 'trip_history': value}))
                with self.assertRaises(ValueError):
                    load_receiver_configuration(path)

    def test_dated_simulation_is_strict_opt_in_native_only_and_exclusive(self):
        from receiver_configuration import parse_receiver_configuration
        from vehicle_fanout import vehicle_arguments
        from install_cm5_appliance import native_cluster_command
        from phone_manager import Manager
        base = {'display_config': '/private/displays.conf'}
        parse = lambda values: parse_receiver_configuration(json.dumps(values).encode(), Path('/private/receiver.json'))
        self.assertEqual(vehicle_arguments(parse(base), os.getuid(), None), [])
        self.assertEqual(vehicle_arguments(parse(dict(base, simulate_vehicle=True)), os.getuid(), None), ['--simulate'])
        settings = parse(dict(base, simulate_dated_vehicle=True))
        self.assertEqual(vehicle_arguments(settings, os.getuid(), None), ['--simulate-dated'])
        cluster = {'width': 1920, 'height': 720, 'output': 'HDMI-A-2'}
        command = native_cluster_command(Path('/sealed'), settings, cluster, ['--simulate-dated'])
        self.assertEqual(command, ['/sealed/build/headunit-cluster-wayland', '--size', '1920', '720',
                                   '--output', 'HDMI-A-2', '--background', '--simulate-dated'])
        # The SAME native role is used by manager admission and generated units.
        for bad in (1, 'true', None, [], {}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse(dict(base, simulate_dated_vehicle=bad))
        for extra in ({'simulate_vehicle': True}, {'vehicle_socket': '/private/vehicle.sock'}):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                parse(dict(base, simulate_dated_vehicle=True, **extra))
        with self.assertRaisesRegex(ValueError, 'fan-out'):
            vehicle_arguments(settings, os.getuid(), Path('/not-opened/fanout.json'))
        with self.assertRaises(ValueError):
            vehicle_arguments(dict(simulate_dated_vehicle=1), os.getuid(), None)
        # Unmanaged fallback calls the same native CLI; no receiver control source.
        manager = object.__new__(Manager)
        manager.fallback_size = (1920, 720)
        manager.media = type('Media', (), {'path': Path('/private/media.json')})()
        manager.base = settings
        manager.greeting_path = None
        manager.fallback_vehicle_args = ['--simulate-dated']
        self.assertEqual(manager.fallback_command('/sealed/build/headunit-cluster-wayland')[-1], '--simulate-dated')

    def test_vehicle_checker_uses_admitted_runtime_owner_under_root_launcher(self):
        from unittest.mock import patch
        from run_lab_receiver import check_vehicle_capabilities
        settings = {'bin_dir': '/sealed/build', 'car_settings_config': '/sealed/settings.json'}
        with patch('run_lab_receiver.os.geteuid', return_value=0), \
                patch('run_lab_receiver.subprocess.run') as run:
            check_vehicle_capabilities(settings, (1234, 2345))
            command = run.call_args.args[0]
            self.assertEqual(command, ['/sealed/build/headunit-vehicle-config-check', '-',
                                       '/sealed/settings.json', '-'])
            options = run.call_args.kwargs
            self.assertEqual((options['user'], options['group'], options['extra_groups']),
                             (1234, 2345, ()))
            self.assertTrue(options['check']); self.assertEqual(options['timeout'], 10)
        with patch('run_lab_receiver.os.geteuid', return_value=1234), \
                patch('run_lab_receiver.subprocess.run') as run:
            check_vehicle_capabilities(settings, (1234, 2345))
            self.assertNotIn('user', run.call_args.kwargs)
            check_vehicle_capabilities({}, (1234, 2345))
            self.assertEqual(run.call_count, 1)

    def test_startup_diagnostics_are_optional_real_dates_and_cannot_fail_a_launch(self):
        from unittest.mock import patch
        from run_lab_receiver import startup_diagnostic
        out = io.StringIO()
        with patch('time.clock_gettime_ns', return_value=123) as clock, contextlib.redirect_stdout(out):
            startup_diagnostic(False, 'ap-enabled')
            clock.assert_not_called()
            startup_diagnostic(True, 'ap-enabled')
            clock.assert_called_once_with(__import__('time').CLOCK_MONOTONIC)
        self.assertEqual(out.getvalue(), 'Headunit startup diagnostic mono_ns=123 stage=ap-enabled\n')
        out = io.StringIO()
        with patch('time.clock_gettime_ns', side_effect=OSError('Unavailable')), contextlib.redirect_stdout(out):
            startup_diagnostic(True, 'ap-enabled')
        self.assertEqual(out.getvalue(), '')
        with patch('builtins.print', side_effect=BrokenPipeError):
            startup_diagnostic(True, 'ap-enabled')

    def test_special_receiver_inputs_reject_before_offline_publication(self):
        script = ('import sys\nfrom pathlib import Path\n'
                  'sys.path.insert(0, sys.argv[1])\n'
                  'from receiver_configuration import load_receiver_configuration\n'
                  'from configure_vehicle import compile_configuration\n'
                  'root=Path(sys.argv[2])\n'
                  'try:\n'
                  '    if sys.argv[3] == "load":\n'
                  '        load_receiver_configuration(root / "receiver.json")\n'
                  '    else:\n'
                  '        definition={"schemaVersion":1,"vehicle":{"make":"Fixture","model":"Fixture","year":2020},"ultraSignals":[]}\n'
                  '        compile_configuration(definition, root / "receiver.json", root / "new", root / "unused-checker")\n'
                  'except ValueError:\n    pass\n'
                  'else:\n    raise AssertionError("special receiver input admitted")\n')
        for action in ('load', 'compile'):
            with self.subTest(action=action), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                os.mkfifo(root / 'receiver.json', 0o600)
                result = subprocess.run([sys.executable, '-B', '-c', script,
                                         str(Path(__file__).resolve().parents[1] / 'tools'),
                                         str(root), action], capture_output=True, timeout=2)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertFalse((root / 'new').exists())
                self.assertFalse(list(root.glob('.vehicle-config-*')))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(ValueError):
                load_receiver_configuration(root)
            path = root / 'receiver.json'
            for raw in (b'', b' ' * 16385):
                path.write_bytes(raw)
                with self.assertRaises(ValueError):
                    load_receiver_configuration(path)
            raw = b'{"display_config":"display.conf"}'
            path.write_bytes(raw + b' ' * (16384 - len(raw)))
            self.assertEqual(path.stat().st_size, 16384)
            self.assertEqual(load_receiver_configuration(path)['display_config'],
                             (root / 'display.conf').resolve())

    def test_pre_phone_gauges_need_explicit_verified_profile(self):
        from receiver_configuration import validate_device_settings
        validate_device_settings({'pre_phone_gauges': False})
        with self.assertRaises(ValueError):
            validate_device_settings({'pre_phone_gauges': True})
        validate_device_settings({'pre_phone_gauges': True, 'ultra_installed_ui': '/private/ultra.conf'})

    def test_indicator_flag_requires_existing_publication_and_exact_boolean(self):
        validate_device_settings({'prephone_indicators': False})
        with self.assertRaises(ValueError):
            validate_device_settings({'prephone_indicators': True})
        validate_device_settings({'prephone_indicators': True,
                                  'prephone_publication_socket': '/private/state/prephone.sock'})
        for ambient in (False, True):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path = root / 'receiver.json'
                for indicators in (False, True):
                    path.write_text(json.dumps({'display_config': 'displays.conf', 'ambient_light': ambient, 'prephone_indicators': indicators,
                                                'prephone_publication_socket': '/private/state/prephone.sock'}))
                    self.assertEqual(load_receiver_configuration(path)['prephone_indicators'], indicators)
                for invalid in (0, 1, 'false', None, []):
                    path.write_text(json.dumps({'prephone_indicators': invalid,
                                                'prephone_publication_socket': '/private/state/prephone.sock'}))
                    with self.assertRaises(ValueError):
                        load_receiver_configuration(path)

    def test_night_mode_policy_is_validated_before_launch(self):
        validate_device_settings({'limited_ui': 'speed'})
        for value in ('on', '', True):
            with self.assertRaises(ValueError):
                validate_device_settings({'limited_ui': value})
        for value in ('off', 'on', 'headlights', 'schedule', 'schedule:20:00-07:00', 'schedule:09:30-17:00'):
            validate_device_settings({'night_mode': value})
        for value in ('', 'auto', 'ON', 1, True, 'schedule:', 'schedule:24:00-07:00',
                      'schedule:20:60-07:00', 'schedule:20:00-20:00', 'schedule:2000-0700'):
            with self.assertRaises(ValueError):
                validate_device_settings({'night_mode': value})

    def test_stable_source_links_permissions_and_partial_reads_stay_compatible(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            target = root / 'source'
            target.mkdir()
            config = target / 'receiver.json'
            config.write_text('{"display_config":"display.conf"}')
            config.chmod(0o644)  # Source inputs retain their existing owner/mode policy.
            link = root / 'linked.json'
            link.symlink_to(config)
            hard = root / 'hard.json'
            os.link(config, hard)
            self.assertEqual(load_receiver_configuration(link)['display_config'], target / 'display.conf')
            self.assertEqual(load_receiver_configuration(hard)['display_config'], root / 'display.conf')
            original_read = os.read
            with patch('receiver_configuration.os.read', side_effect=lambda fd, n: original_read(fd, min(n, 3))):
                self.assertEqual(load_receiver_configuration(link)['display_config'], target / 'display.conf')

    def test_receiver_mutations_do_not_rebind_captured_values(self):
        from unittest.mock import patch
        import receiver_configuration as reader
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            config = root / 'receiver.json'
            raw = b'{"display_config":"display.conf"}'
            link = root / 'linked.json'
            link.symlink_to(config)
            original_read = os.read
            for kind in ('rewrite', 'replace', 'retarget'):
                config.write_bytes(raw)
                changed = False
                def change_after_read(fd, maximum):
                    nonlocal changed
                    chunk = original_read(fd, maximum)
                    if not changed:
                        changed = True
                        if kind == 'rewrite':
                            replacement_bytes = raw.replace(b'display.conf', b'changed.conf')
                            change_metadata(config, lambda: config.write_bytes(replacement_bytes))
                        else:
                            replacement = root / 'replacement.json'
                            replacement.write_bytes(raw)
                            if kind == 'replace':
                                os.replace(replacement, config)
                            else:
                                link.unlink()
                                link.symlink_to(replacement)
                    return chunk
                with self.subTest(kind=kind), patch('receiver_configuration.os.read', side_effect=change_after_read):
                    with self.assertRaises(ValueError):
                        load_receiver_configuration(link)
                link.unlink()
                link.symlink_to(config)
            config.write_bytes(raw)
            original_open = os.open
            def replace_with_fifo(name, flags, *args, **kwargs):
                if name == config.name:
                    config.unlink()
                    os.mkfifo(config, 0o600)
                return original_open(name, flags, *args, **kwargs)
            with patch('receiver_configuration.os.open', side_effect=replace_with_fifo):
                with self.assertRaises(ValueError):
                    load_receiver_configuration(config)
            config.unlink()
            config.write_bytes(raw)
            other = root / 'other'
            other.mkdir()
            replacement = other / 'receiver.json'
            replacement.write_bytes(raw)
            original_parse = reader.parse_receiver_configuration
            def replace_after_capture(data, path, **kwargs):
                config.unlink()
                config.symlink_to(replacement)
                return original_parse(data, path, **kwargs)
            with patch('receiver_configuration.parse_receiver_configuration', side_effect=replace_after_capture):
                self.assertEqual(load_receiver_configuration(config)['display_config'], root / 'display.conf')

    def test_software_mfi_requires_explicit_complete_provider_and_scrubs_inheritance(self):
        from run_lab_receiver import mfi_environment
        inherited = {'KEEP': '1', 'HEADUNIT_MFI_BACKEND': 'software',
                     'HEADUNIT_MFI_PRIVATE_KEY': '/other/key', 'HEADUNIT_MFI_CERTIFICATE': '/other/cert',
                     'HEADUNIT_MFI_DEVICE': '/dev/i2c-9', 'HEADUNIT_MFI_ADDRESS': '50'}
        self.assertEqual(mfi_environment(inherited, {}),
                         {'KEEP': '1', 'HEADUNIT_MFI_BACKEND': 'hardware',
                          'HEADUNIT_MFI_DEVICE': '/dev/i2c-1', 'HEADUNIT_MFI_ADDRESS': '16'})
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            config = root / 'receiver.json'
            values = {'display_config': 'display.conf', 'mfi_backend': 'software',
                      'mfi_private_key': 'identity.pk8', 'mfi_certificate': 'certificate.p7b'}
            config.write_text(json.dumps(values))
            loaded = load_receiver_configuration(config)
            self.assertEqual(loaded['mfi_private_key'], root / 'identity.pk8')
            self.assertEqual(mfi_environment(inherited, loaded),
                             {'KEEP': '1', 'HEADUNIT_MFI_BACKEND': 'software',
                              'HEADUNIT_MFI_PRIVATE_KEY': str(root / 'identity.pk8'),
                              'HEADUNIT_MFI_CERTIFICATE': str(root / 'certificate.p7b')})
            for changes in ({'mfi_backend': 'auto'}, {'mfi_backend': 'hardware'},
                            {'mfi_backend': None}, {'mfi_private_key': None}, {'mfi_certificate': None},
                            {'mfi_certificate': loaded['mfi_private_key']},
                            {'mfi_private_key': '/tmp//key'}, {'pairing_key': loaded['mfi_private_key']}):
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    validate_device_settings(loaded | changes)
            linked = root / 'linked.pk8'; linked.symlink_to(root / 'identity.pk8')
            config.write_text(json.dumps(values | {'mfi_private_key': str(linked)}))
            self.assertEqual(load_receiver_configuration(config)['mfi_private_key'], linked)

    def test_software_mfi_preflight_uses_selected_receiver_before_launch_and_never_hardware(self):
        from run_lab_receiver import check_software_mfi_identity
        from unittest.mock import patch
        import os
        with patch('run_lab_receiver.subprocess.run') as run:
            check_software_mfi_identity({}, (os.getuid(), os.getgid()))
            run.assert_not_called()
            check_software_mfi_identity({'mfi_backend': 'software', 'bin_dir': '/test/build',
                                        'mfi_private_key': '/private/key.pk8',
                                        'mfi_certificate': '/private/cert.p7b'}, (os.getuid(), os.getgid()))
            self.assertEqual(run.call_args.args[0], ['/test/build/headunit-control-probe',
                '--check-software-mfi', '/private/key.pk8', '/private/cert.p7b'])
            self.assertEqual(run.call_args.kwargs['timeout'], 5)

    def test_invalid_software_identity_stops_launcher_before_radio_or_session_creation(self):
        import run_lab_receiver as launcher
        import subprocess
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            display = root / 'display.conf'; display.write_text('main.wayland=wayland-0\nmain.output=MAIN\n')
            identity = root / 'identity.conf'; identity.write_text('name=Test\n')
            profile = root / 'receiver.json'
            values = {key: str(root / key) for key in ('wifi_config', 'pairing_key', 'setup_secret',
                       'pair_store', 'capture_root', 'bin_dir', 'mfi_private_key', 'mfi_certificate')}
            values.update(display_config=str(display), identity=str(identity), mfi_backend='software')
            profile.write_text(json.dumps(values))
            with patch.object(sys, 'argv', ['run_lab_receiver.py', '--config', str(profile), '--dry-run']), \
                    patch.object(launcher, 'run'), patch.object(launcher, 'private_file'), \
                    patch.object(launcher, 'check_software_mfi_identity', side_effect=subprocess.CalledProcessError(1, 'checker')), \
                    patch.object(launcher, 'generate_ap_config') as ap, patch.object(launcher, 'launch') as launch, \
                    patch.object(launcher, 'private_directory') as directory:
                with self.assertRaises(SystemExit) as stopped: launcher.main()
                self.assertEqual(stopped.exception.code, 2)
                ap.assert_not_called(); launch.assert_not_called(); directory.assert_not_called()

    def test_hud_navigation_is_opt_in_bounded_distinct_and_scrubbed(self):
        from run_lab_receiver import navigation_environment
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); path = root / 'receiver.json'
            values = {'display_config': 'display.conf', 'route_guidance': True,
                      'navigation_socket': str(root / 'hud-navigation.sock')}
            path.write_text(json.dumps(values))
            loaded = load_receiver_configuration(path)
            self.assertEqual(loaded['navigation_socket'], root / 'hud-navigation.sock')
            inherited = {'KEEP': '1', 'HEADUNIT_NAVIGATION_SOCKET': '/other'}
            self.assertEqual(navigation_environment(inherited, {}), {'KEEP': '1'})
            self.assertEqual(navigation_environment(inherited, loaded),
                             {'KEEP': '1', 'HEADUNIT_NAVIGATION_SOCKET': values['navigation_socket']})
            for change in ({'route_guidance': False}, {'navigation_socket': 'relative.sock'},
                           {'navigation_socket': '/tmp//hud.sock'}, {'navigation_socket': '/' + 'x' * 103},
                           {'vehicle_socket': values['navigation_socket']},
                           {'media_state': values['navigation_socket'] + '.lock'},
                           {'media_state': values['navigation_socket'] + '.publisher.lock'}):
                with self.subTest(change=change), self.assertRaises(ValueError):
                    validate_device_settings(values | change)

    def test_connection_diagnostics_only_follow_explicit_configuration(self):
        from run_lab_receiver import connection_diagnostic_environment
        inherited = {"HEADUNIT_CONNECTION_DIAGNOSTICS": "1", "UNCHANGED": "value"}
        self.assertEqual(connection_diagnostic_environment(inherited, False), {"UNCHANGED": "value"})
        self.assertEqual(connection_diagnostic_environment({}, True), {"HEADUNIT_CONNECTION_DIAGNOSTICS": "1"})
        self.assertEqual(inherited, {"HEADUNIT_CONNECTION_DIAGNOSTICS": "1", "UNCHANGED": "value"})

    def test_early_radio_start_waits_for_the_display_user_socket_and_propagates_failure(self):
        from run_lab_receiver import wait_for_wayland
        from unittest.mock import patch
        with patch('run_lab_receiver.subprocess.run') as run:
            wait_for_wayland((1000, 1000), 'wayland-0')
            command = run.call_args.args[0]
            options = run.call_args.kwargs
            self.assertEqual(command[-4:], ['--socket', 'wayland-0', '--timeout', '15'])
            self.assertEqual((options['user'], options['group']), (1000, 1000))
            self.assertEqual(options['extra_groups'], ())
            self.assertEqual(options['env']['XDG_RUNTIME_DIR'], '/run/user/1000')
            self.assertEqual(options['timeout'], 17)
            run.side_effect = subprocess.TimeoutExpired(command, 17)
            with self.assertRaises(subprocess.TimeoutExpired):
                wait_for_wayland((1000, 1000), 'wayland-0')

    def test_runtime_presentation_configuration_is_paired_bounded_and_scrubbed(self):
        from run_lab_receiver import presentation_environment
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); path = root / 'receiver.json'
            values = {'display_config': 'display.conf', 'runtime_status_file': str(root / 'carplay-status.json'),
                      'runtime_status_nonce': '0123456789abcdef0123456789abcdef'}
            path.write_text(json.dumps(values)); loaded = load_receiver_configuration(path)
            self.assertEqual(loaded['runtime_status_file'], root / 'carplay-status.json')
            inherited = {'KEEP': '1', 'HEADUNIT_PRESENTATION_STATUS_FILE': '/other',
                         'HEADUNIT_PRESENTATION_STATUS_NONCE': 'e' * 32}
            self.assertEqual(presentation_environment(inherited, {}), {'KEEP': '1'})
            environment = presentation_environment(inherited, loaded)
            self.assertEqual(environment['HEADUNIT_PRESENTATION_STATUS_FILE'], values['runtime_status_file'])
            self.assertEqual(environment['HEADUNIT_PRESENTATION_STATUS_NONCE'], values['runtime_status_nonce'])
            self.assertFalse((root / 'carplay-status.json').exists())
            for key in ('runtime_status_file', 'runtime_status_nonce'):
                bad = dict(values); bad.pop(key)
                with self.subTest(key=key), self.assertRaises(ValueError): validate_device_settings(bad)
            for changes in ({'runtime_status_nonce': 'A' * 32}, {'runtime_status_nonce': '0' * 31},
                            {'runtime_status_nonce': '0' * 33}, {'runtime_status_nonce': True},
                            {'runtime_status_file': '/tmp//status'}, {'runtime_status_file': '/tmp/../status'},
                            {'runtime_status_file': 'relative'}, {'runtime_status_file': '/tmp/$status'},
                            {'media_state': values['runtime_status_file']},
                            {'audio_state': values['runtime_status_file'] + '.owner', 'audio_control': '/tmp/audio', 'audio_sink': 'fakesink'}):
                with self.subTest(changes=changes), self.assertRaises(ValueError): validate_device_settings(values | changes)

    def test_runtime_endpoint_link_is_kept_for_its_owner_to_reject(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); target = root / 'keep'; target.write_text('unchanged')
            linked = root / 'status'; linked.symlink_to(target)
            path = root / 'receiver.json'
            path.write_text(json.dumps({'display_config': 'd.conf', 'runtime_status_file': str(linked),
                                        'runtime_status_nonce': '0' * 32}))
            self.assertEqual(load_receiver_configuration(path)['runtime_status_file'], linked)
            self.assertEqual(target.read_text(), 'unchanged')

    def test_native_page_pair_schema_environment_and_mode_conversion(self):
        from run_lab_receiver import native_page_environment
        from phone_state import receiver_settings
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); path = root / 'receiver.json'
            base = {'display_config': str(root / 'display.conf'), 'preview_sink': 'wayland-gpu',
                    'pair_store': str(root / 'pairs'), 'native_page_control_file': str(root / 'home-surface.request'),
                    'native_page_lease_file': str(root / 'home-surface.lease')}
            path.write_text(json.dumps(base))
            loaded = load_receiver_configuration(path)
            self.assertFalse((root / 'home-surface.request').exists())
            fields = {'main.wayland': 'wayland-0', 'main.output': 'HDMI-A-1'}
            inherited = {'HEADUNIT_NATIVE_PAGE_CONTROL': '/other', 'HEADUNIT_NATIVE_PAGE_SURFACE_LEASE': '/other', 'KEEP': '1'}
            self.assertEqual(native_page_environment(inherited, {}, {}), {'KEEP': '1'})
            routed = native_page_environment(inherited, loaded, fields)
            self.assertEqual(routed['HEADUNIT_NATIVE_PAGE_CONTROL'], base['native_page_control_file'])
            for key in ('native_page_control_file', 'native_page_lease_file'):
                bad = dict(base); bad.pop(key)
                with self.subTest(key=key), self.assertRaises(ValueError): validate_device_settings(bad)
            for changes in ({'preview_sink': 'wayland-touch'}, {'native_page_control_file': '/tmp//x'},
                            {'native_page_control_file': '/tmp/../x'}, {'native_page_control_file': '/tmp/$x'},
                            {'native_page_control_file': '/tmp/other/x'}, {'native_page_control_file': base['native_page_lease_file']},
                            {'camera_manual_profile': '/tmp/camera'}, {'media_state': base['native_page_control_file']}):
                with self.subTest(changes=changes), self.assertRaises(ValueError): validate_device_settings(base | changes)
            standard = receiver_settings(base, 'AA:BB:CC:DD:EE:01', root / 'standard.conf', root / 'media.json')
            self.assertEqual(standard['preview_sink'], 'wayland-gpu')
            legacy = {k: v for k, v in base.items() if not k.startswith('native_page_')}
            self.assertEqual(receiver_settings(legacy, 'AA:BB:CC:DD:EE:01', root / 'standard.conf', root / 'media.json')['preview_sink'], 'wayland-touch')
            offered = receiver_settings(base | {'ultra_installed_ui': str(root / 'installed-ui.conf')},
                                        'AA:BB:CC:DD:EE:01', root / 'standard.conf', root / 'media.json')
            self.assertTrue(offered['ultra_negotiate'])
            self.assertEqual(offered['standard_display_config'], str(root / 'standard.conf'))
            self.assertEqual(native_page_environment(inherited, offered, fields)['HEADUNIT_NATIVE_PAGE_CONTROL'],
                             base['native_page_control_file'])

    def test_native_page_ultra_overlay_is_bounded_exact_and_camera_free(self):
        from run_lab_receiver import native_page_ui, native_page_environment
        base = {'display_config': '/private/displays', 'ultra_installed_ui': '/private/ui',
                'ultra_native_video': True, 'ultra_negotiate': True,
                'native_page_control_file': '/private/home-surface.request',
                'native_page_lease_file': '/private/home-surface.lease'}
        raw = b'assetDirectory=/private/assets\nassetIdentifier=fixture\nassetVersion=1\nvehicleProfile=/private/vehicle\n'
        output = native_page_ui(raw, base)
        self.assertEqual(native_page_ui(output, base), output)
        self.assertEqual(native_page_ui(raw, {}), raw)
        with self.assertRaisesRegex(ValueError, 'explicit receiver pair'):
            native_page_ui(raw + b'nativePageControlFile=/private/home-surface.request\n', {})
        self.assertIn(b'nativePageControlFile=/private/home-surface.request\n', output)
        self.assertEqual(native_page_environment({'HEADUNIT_NATIVE_PAGE_CONTROL': '/other'}, base,
                         {'main.wayland': 'wayland-0', 'main.output': 'HDMI-A-1'}), {})
        for extra in (b'nativePageControlFile=/wrong\n', b'nativePageLeaseFile=/wrong\n', b'cameraProfile=/private/camera\n',
                      b'nativeCameraControlFile=/private/camera\n', b'nativePageControlFile=/private/home-surface.request\n'):
            with self.subTest(extra=extra), self.assertRaises(ValueError): native_page_ui(raw + extra, base)
        with self.assertRaises(ValueError): native_page_ui(b'x' * 8193, base)
        self.assertNotIn(b'nativePage', raw)

    def test_launcher_dry_run_rejects_orphan_ui_before_radio_or_runtime_writes(self):
        import os
        from unittest.mock import patch
        import run_lab_receiver as launcher
        with tempfile.TemporaryDirectory(prefix='headunit-page-', dir='/tmp') as folder:
            root = Path(folder).resolve(); root.chmod(0o700)
            displays = root / 'displays.conf'
            displays.write_text('main.wayland=wayland-0\nmain.output=HDMI-A-1\n')
            ui = root / 'ui.conf'
            ui.write_text('assetDirectory=/private/assets\nassetIdentifier=fixture\nassetVersion=1\nvehicleProfile=/private/vehicle\n'
                          'nativePageControlFile=/private/home-surface.request\nnativePageLeaseFile=/private/home-surface.lease\n')
            ui.chmod(0o600)
            saved = {'display_config': str(displays), 'bin_dir': str(root / 'build'),
                     'pair_store': str(root / 'pairs'), 'capture_root': str(root / 'captures'), 'ultra_installed_ui': str(ui)}
            for key in ('identity', 'wifi_config', 'pairing_key', 'setup_secret'):
                path = root / key; path.write_text('fixture\n'); path.chmod(0o600); saved[key] = str(path)
            config = root / 'receiver.json'; config.write_text(json.dumps(saved))
            before = {p.name: p.read_bytes() for p in root.iterdir()}
            previous_umask = os.umask(0o077); os.umask(previous_umask)
            try:
                # The display checker is the only explicit substitute here;
                # the actual private file, CLI and bounded UI readers run.
                with patch.object(sys, 'argv', ['run_lab_receiver.py', '--config', str(config), '--dry-run']), \
                        patch.object(launcher, 'run') as checker, patch.object(launcher, 'launch') as activate, \
                        patch.object(launcher, 'generate_ap_config') as ap:
                    with self.assertRaisesRegex(ValueError, 'explicit receiver pair'): launcher.main()
                    self.assertEqual(checker.call_count, 1)
                    self.assertEqual(Path(checker.call_args.args[0]).name, 'headunit-display-config-check')
                    activate.assert_not_called(); ap.assert_not_called()
            finally: os.umask(previous_umask)
            self.assertEqual({p.name: p.read_bytes() for p in root.iterdir()}, before)

    def test_manual_camera_profile_preserves_final_name(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve(); profile = root / 'camera.json'
            profile.symlink_to(root / 'other.json')
            saved = root / 'receiver.json'
            saved.write_text(json.dumps({'display_config': 'display.conf', 'camera_manual_profile': 'camera.json'}))
            self.assertEqual(load_receiver_configuration(saved)['camera_manual_profile'], profile)
            saved.write_text(json.dumps({'display_config': 'display.conf', 'camera_manual_profile': True}))
            with self.assertRaises(ValueError): load_receiver_configuration(saved)

    def test_saved_profile(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "receiver.json"
            path.write_text(json.dumps({"display_config": "displays.conf", "seconds": 120,
                                        "ultra_native_video": True, "preview_decoder": "avdec_h264"}))
            settings = load_receiver_configuration(path)
            self.assertEqual(settings["display_config"], (path.parent / "displays.conf").resolve())
            self.assertEqual(settings["seconds"], 120)
            self.assertTrue(settings["ultra_native_video"])

    def test_device_labels_preserve_pairing_identity(self):
        source = "name=Old name\nmodel=Old model\npi=existing-uuid\npk=existing-key\nfeatures=0x1\n"
        resolved = configured_identity(source, {"device_name": "My Headunit", "manufacturer": "My Workshop",
                                               "hardware_version": "CM5"})
        fields = dict(line.split("=", 1) for line in resolved.splitlines())
        self.assertEqual(fields["name"], "My Headunit")
        self.assertEqual(fields["manufacturer"], "My Workshop")
        self.assertEqual(fields["hardware"], "CM5")
        self.assertEqual(fields["pi"], "existing-uuid")
        self.assertEqual(fields["pk"], "existing-key")
        self.assertEqual(fields["features"], "0x1")
        self.assertEqual(fields["model"], "Old model")
        self.assertIn("name=Old name", source)
        self.assertEqual(configured_identity(source, {}), source)

    def test_device_values_reject_injection_and_oversize(self):
        for value in ("", "bad\nname=other", "bad\x00", "bad\x7f", "x" * 64, 3):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    validate_device_settings({"device_name": value})
        with self.assertRaises(ValueError):
            configured_identity("name=a\nname=b\n", {})

    def test_device_profile_loading(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "receiver.json"
            path.write_text(json.dumps({"display_config": "d.conf", "device_name": "My Car",
                                        "serial_number": "Prototype 1", "hardware_version": "CM5"}))
            settings = load_receiver_configuration(path)
            self.assertEqual(settings["device_name"], "My Car")
            self.assertEqual(settings["hardware_version"], "CM5")

    def test_explicit_audio_device(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "receiver.json"
            for sink, device in (("alsasink", "hw:CARD=Headunit,DEV=0"),
                                 ("pulsesink", "alsa_output.usb-Headunit.analog-stereo")):
                path.write_text(json.dumps({"display_config": "d.conf", "audio_sink": sink,
                                            "audio_device": device}))
                self.assertEqual(load_receiver_configuration(path)["audio_device"], device)
            for changes in ({"audio_sink": "none"}, {"audio_sink": "fakesink"},
                            {"audio_device": "bad\nroute"}, {"audio_device": "bad\0route"},
                            {"audio_device": "x" * 256}, {"audio_device": 2}):
                path.write_text(json.dumps({"display_config": "d.conf", "audio_sink": "alsasink",
                                            "audio_device": "default"} | changes))
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    load_receiver_configuration(path)

    def test_private_headunit_audio_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'receiver.json'
            base = {'display_config': 'd.conf', 'audio_sink': 'fakesink', 'audio_control': 'audio-control.json',
                    'audio_state': 'audio-state.json', 'audio_volume_percent': 37, 'audio_muted': True}
            path.write_text(json.dumps(base))
            loaded = load_receiver_configuration(path)
            self.assertEqual(loaded['audio_control'], (path.parent / 'audio-control.json').resolve())
            self.assertEqual(loaded['audio_volume_percent'], 37); self.assertTrue(loaded['audio_muted'])
            for changes in ({'audio_control': 'audio-state.json'}, {'audio_sink': 'none'},
                            {'audio_volume_percent': True}, {'audio_volume_percent': -1},
                            {'audio_volume_percent': 101}, {'audio_muted': 1}, {'audio_muted': 'false'}):
                path.write_text(json.dumps(base | changes))
                with self.subTest(changes=changes), self.assertRaises(ValueError): load_receiver_configuration(path)
            for removed in ('audio_control', 'audio_state'):
                value = dict(base); value.pop(removed); path.write_text(json.dumps(value))
                with self.assertRaises(ValueError): load_receiver_configuration(path)
            for key, value in (('audio_volume_percent', 30), ('audio_muted', False)):
                with self.assertRaises(ValueError): validate_device_settings({key: value})

    def test_launcher_audio_environment_is_explicit_and_clears_inheritance(self):
        from run_lab_receiver import audio_environment
        inherited = {key: 'old' for key in ('HEADUNIT_AUDIO_SINK', 'HEADUNIT_AUDIO_DEVICE',
                      'HEADUNIT_AUDIO_CONTROL_FILE', 'HEADUNIT_AUDIO_STATE_FILE',
                      'HEADUNIT_AUDIO_VOLUME_PERCENT', 'HEADUNIT_AUDIO_MUTED',
                      'HEADUNIT_MICROPHONE_SOURCE', 'HEADUNIT_MICROPHONE_DEVICE',
                      'HEADUNIT_AUDIO_OBSERVATION_FILE')}
        inherited['OTHER_SETTING'] = 'preserved'
        self.assertEqual(audio_environment(inherited, {'audio_sink': 'none'}), {'OTHER_SETTING': 'preserved'})
        self.assertEqual(inherited['HEADUNIT_AUDIO_MUTED'], 'old')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            value = audio_environment(inherited, {'audio_sink': 'alsasink', 'audio_device': 'default',
                        'audio_control': root/'control', 'audio_state': root/'state',
                        'audio_volume_percent': 0, 'audio_muted': False})
            self.assertEqual(value['HEADUNIT_AUDIO_CONTROL_FILE'], str((root/'control').resolve()))
            self.assertEqual(value['HEADUNIT_AUDIO_STATE_FILE'], str((root/'state').resolve()))
            self.assertEqual(value['HEADUNIT_AUDIO_VOLUME_PERCENT'], '0')
            self.assertEqual(value['HEADUNIT_AUDIO_MUTED'], '0')
            self.assertEqual(value['HEADUNIT_AUDIO_SINK'], 'alsasink')
            self.assertEqual(value['HEADUNIT_AUDIO_DEVICE'], 'default')
            self.assertEqual(audio_environment({}, {'audio_sink': 'fakesink', 'audio_muted': True})['HEADUNIT_AUDIO_MUTED'], '1')

    def test_audio_observation_admission_keeps_private_path_and_names_each_launch(self):
        from run_lab_receiver import audio_environment, managed_receiver_settings
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            directory = root / 'not-created-by-preflight'
            path = root / 'receiver.json'
            path.write_text(json.dumps({'display_config': 'display.conf',
                                        'pair_store': 'synthetic-pair-store',
                                        'audio_observation_directory': str(directory)}))
            saved = load_receiver_configuration(path)
            self.assertEqual(saved['audio_observation_directory'], directory)
            managed = managed_receiver_settings(
                saved, {'stateDirectory': '/private/state',
                        'standardDisplayConfig': '/private/standard'}, 'AA:BB:CC:DD:EE:01')
            self.assertEqual(managed['audio_observation_directory'], directory)
            inherited = {'HEADUNIT_AUDIO_OBSERVATION_FILE': '/unadmitted/old.jsonl'}
            first = audio_environment(inherited, saved)['HEADUNIT_AUDIO_OBSERVATION_FILE']
            second = audio_environment(inherited, saved)['HEADUNIT_AUDIO_OBSERVATION_FILE']
            self.assertNotEqual(first, second)
            self.assertEqual(Path(first).parent, directory)
            self.assertRegex(Path(first).name, r'^pcm-[0-9a-f]{32}\.jsonl$')
            self.assertFalse(directory.exists())
            self.assertEqual(inherited['HEADUNIT_AUDIO_OBSERVATION_FILE'], '/unadmitted/old.jsonl')
            self.assertNotIn('HEADUNIT_AUDIO_OBSERVATION_FILE', audio_environment(inherited, {}))
            linked = root / 'linked-directory'
            linked.symlink_to(directory)
            path.write_text(json.dumps({'display_config': 'display.conf',
                                        'audio_observation_directory': str(linked)}))
            self.assertEqual(load_receiver_configuration(path)['audio_observation_directory'], linked)

    def test_audio_observation_rejects_unsafe_directory_in_saved_and_direct_settings(self):
        from run_lab_receiver import audio_environment
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'receiver.json'
            for value in ('relative', '', '/tmp/../trace', '/tmp/./trace', '/tmp//trace',
                          '/tmp/trace/', '/tmp/trace\nEnvironment=BAD', '/tmp/%s',
                          '/tmp/' + 'a' * 256, '/tmp/\x00trace', True, 3):
                with self.subTest(value=repr(value)):
                    path.write_text(json.dumps({'display_config': 'display.conf',
                                                'audio_observation_directory': value}))
                    with self.assertRaises(ValueError):
                        load_receiver_configuration(path)
                    with self.assertRaises(ValueError):
                        audio_environment({}, {'audio_observation_directory': value})

    def test_microphone_configuration_is_explicit_and_real(self):
        from run_lab_receiver import audio_environment
        base = {'audio_sink': 'fakesink', 'microphone_source': 'alsasrc', 'microphone_device': 'hw:USB,0'}
        result = audio_environment({}, base)
        self.assertEqual(result['HEADUNIT_MICROPHONE_SOURCE'], 'alsasrc')
        self.assertEqual(result['HEADUNIT_MICROPHONE_DEVICE'], 'hw:USB,0')
        for change in ({'microphone_source': 'appsrc'}, {'microphone_source': 'audiotestsrc'},
                       {'microphone_source': 'none'}, {'microphone_device': 'bad\nname'}, {'audio_sink': 'none'}):
            with self.assertRaises(ValueError):
                audio_environment({}, {**base, **change})

    def test_audio_sidecar_leaf_is_not_resolved_through_a_link(self):
        from run_lab_receiver import audio_environment
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            target = root / 'other.json'; target.write_text('untouched')
            linked = root / 'audio-state.json'; linked.symlink_to(target)
            path = root / 'receiver.json'
            path.write_text(json.dumps({'display_config': 'd.conf', 'audio_sink': 'fakesink',
                                       'audio_control': 'audio-control.json', 'audio_state': 'audio-state.json'}))
            loaded = load_receiver_configuration(path)
            self.assertEqual(loaded['audio_state'], linked)
            self.assertEqual(audio_environment({}, loaded)['HEADUNIT_AUDIO_STATE_FILE'], str(linked))
            self.assertEqual(target.read_text(), 'untouched')

    def test_vehicle_state_profiles(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "receiver.json"
            for profile in ("none", "empty", "climate-bench", "settings-bench", "bench"):
                path.write_text(json.dumps({"display_config": "d.conf", "vehicle_state_profile": profile}))
                self.assertEqual(load_receiver_configuration(path)["vehicle_state_profile"], profile)
            for profile in ("climate", True, 1, None, []):
                path.write_text(json.dumps({"display_config": "d.conf", "vehicle_state_profile": profile}))
                with self.assertRaises(ValueError):
                    load_receiver_configuration(path)

    def test_route_guidance_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"receiver.json"
            path.write_text(json.dumps({"display_config":"display.conf", "route_guidance":True,
                                        "route_guidance_component_id":42,"route_guidance_name":"Cluster directions"}))
            self.assertTrue(load_receiver_configuration(path)["route_guidance"])
            for key,value in (("route_guidance",1),("route_guidance_component_id",2),("route_guidance_component_id",65536),("route_guidance_name","bad\nname")):
                path.write_text(json.dumps({"display_config":"display.conf",key:value}))
                with self.assertRaises(ValueError):load_receiver_configuration(path)

    def test_vehicle_capability_files(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "receiver.json"
            values = {"display_config": "display.conf", "climate_config": "climate.json",
                      "car_settings_config": "settings.json", "vehicle_state_backend": "simulation"}
            path.write_text(json.dumps(values))
            loaded = load_receiver_configuration(path)
            self.assertEqual(loaded["climate_config"], (path.parent / "climate.json").resolve())
            self.assertEqual(loaded["car_settings_config"], (path.parent / "settings.json").resolve())
            for changes in ({"vehicle_state_backend": "none"}, {"vehicle_state_backend": "car"},
                            {"vehicle_state_profile": "bench"}, {"climate_config": True}):
                with self.subTest(changes=changes):
                    path.write_text(json.dumps(values | changes))
                    with self.assertRaises(ValueError): load_receiver_configuration(path)
            with self.assertRaises(ValueError): validate_device_settings({"vehicle_state_backend": "simulation"})

    def test_camera_state_file_requires_standalone_native_ultra(self):
        from run_lab_receiver import vehicle_capability_environment, managed_receiver_settings
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "receiver.json"
            values = {"display_config": "display.conf", "camera_state_config": "camera-state.json",
                      "vehicle_state_backend": "simulation", "ultra_negotiate": True,
                      "ultra_native_video": True, "ultra_installed_ui": "installed-ui.conf"}
            path.write_text(json.dumps(values))
            loaded = load_receiver_configuration(path)
            camera_file = (path.parent / "camera-state.json").resolve()
            self.assertEqual(loaded["camera_state_config"], camera_file)
            inherited = {"HEADUNIT_CAMERA_STATE_CONFIG": "/old/camera.json",
                         "HEADUNIT_NATIVE_CLUSTER_CONFIG": "/old/native-cluster.conf",
                         "HEADUNIT_CLIMATE_CONFIG": "/old/climate.json", "KEEP": "1"}
            self.assertEqual(vehicle_capability_environment(inherited, {}), {"KEEP": "1"})
            routed = vehicle_capability_environment(inherited, loaded)
            self.assertEqual(routed["HEADUNIT_CAMERA_STATE_CONFIG"], str(camera_file))
            self.assertNotIn("HEADUNIT_CLIMATE_CONFIG", routed)
            self.assertNotIn("HEADUNIT_NATIVE_CLUSTER_CONFIG", routed)
            native = path.parent / 'native-cluster.conf'
            self.assertEqual(vehicle_capability_environment(inherited, {'native_cluster_config': native}),
                             {'KEEP': '1', 'HEADUNIT_NATIVE_CLUSTER_CONFIG': str(native)})
            self.assertEqual(inherited["HEADUNIT_CAMERA_STATE_CONFIG"], "/old/camera.json")
            for changes in ({"ultra_negotiate": False}, {"ultra_native_video": False},
                            {"ultra_installed_ui": None}, {"vehicle_state_backend": "none"},
                            {"vehicle_state_profile": "bench"}):
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    validate_device_settings(loaded | changes)
            validate_device_settings(loaded | {"native_page_control_file": "/private/home.request",
                                               "native_page_lease_file": "/private/home.lease"})
            path.write_text(json.dumps(values | {"camera_state_config": True}))
            with self.assertRaises(ValueError): load_receiver_configuration(path)
            with self.assertRaisesRegex(ValueError, "camera"):
                managed_receiver_settings(loaded, {"stateDirectory": "/private/state",
                                                   "standardDisplayConfig": "/private/standard"},
                                          "AA:BB:CC:DD:EE:01")

    def test_camera_state_cli_preflight_passes_third_file(self):
        import run_lab_receiver as launcher
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            profile = root / "receiver.json"
            profile.write_text(json.dumps({"display_config": str(root / "display.conf"),
                "bin_dir": str(root / "build"), "wifi_config": str(root / "wifi.conf"),
                "identity": str(root / "identity.conf"), "pairing_key": str(root / "pair.key"),
                "setup_secret": str(root / "setup.key"), "pair_store": str(root / "pairs"),
                "capture_root": str(root / "captures"), "ultra_installed_ui": str(root / "ui.conf"),
                "ultra_negotiate": True, "ultra_native_video": True,
                "camera_state_config": str(root / "camera-state.json"),
                "vehicle_state_backend": "simulation"}))
            # Capability admission now follows the trusted runtime account
            # lookup, so the fixture must supply a real owned identity entry.
            (root / 'identity.conf').write_text('synthetic identity ownership fixture')
            (root / 'identity.conf').chmod(0o600)
            class PreflightSeen(Exception): pass
            with patch.object(sys, "argv", ["run_lab_receiver.py", "--config", str(profile), "--dry-run"]), \
                    patch.object(launcher, "run") as display_check, \
                    patch.object(launcher.subprocess, "run", side_effect=PreflightSeen) as vehicle_check:
                with self.assertRaises(PreflightSeen): launcher.main()
                self.assertEqual(display_check.call_count, 1)
                self.assertEqual(vehicle_check.call_args.args[0], [
                    str(root / "build/headunit-vehicle-config-check"), "-", "-",
                    str(root / "camera-state.json")])

    def test_wifi_country_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "wifi.conf"
            path.write_text("ssid=TestHeadunit\npassphrase=TestPassword123\nchannel=36\n")
            output = Path(folder) / "ap"
            generate_ap(str(path), str(output), "DE")
            self.assertIn("country_code=DE\n", (output / "hostapd.conf").read_text())
            self.assertIn("ssid=TestHeadunit\n", (output / "hostapd.conf").read_text())
            for value in ("de", "FRA", "FR\nssid=bad", ""):
                with self.assertRaises(ValueError):
                    generate_ap(str(path), str(output), value)

    def test_reject_ambiguous_or_mistyped(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "receiver.json"
            for value in ('[]', '{}', '{"display_config":"a","display_config":"b"}',
                          '{"display_config":"a","seconds":true}',
                          '{"display_config":"a","ultra_negotiate":"false"}',
                          '{"display_config":"a","main_width":1920}'):
                with self.subTest(value=value):
                    path.write_text(value)
                    with self.assertRaises(ValueError):
                        load_receiver_configuration(path)


    def test_greeting_environment_is_explicit_and_scrubs_foreign_owner(self):
        from run_lab_receiver import vehicle_capability_environment
        inherited = {'HEADUNIT_GREETING_SEQUENCE_FILE': '/foreign/intent', 'KEEP': '1'}
        self.assertEqual(vehicle_capability_environment(inherited, {}), {'KEEP': '1'})
        self.assertEqual(vehicle_capability_environment(inherited, {'greeting_sequence_file': Path('/private/state/greeting-sequence')}),
                         {'KEEP': '1', 'HEADUNIT_GREETING_SEQUENCE_FILE': '/private/state/greeting-sequence'})

if __name__ == "__main__":
    unittest.main()
