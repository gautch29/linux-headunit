#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Real file/socket/lock preflight with an explicit fake GTK runtime boundary.

The C++ display parser and production wrapper execute unchanged. Fake GTK/GDK
records ownership and teardown ordering; it supplies no compositor/GPU evidence.
"""
import copy
import contextlib
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else ROOT / 'build/headunit-display-config-check'
sys.path.insert(0, str(ROOT / 'tools'))
import phone_page_boot as boot
import phone_page_configuration as configuration


class StartupNotification(unittest.TestCase):
    def test_notify_uses_the_main_process_and_is_optional_outside_systemd(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            boot.notify_started()
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'notify')
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as listener:
                listener.bind(path)
                listener.settimeout(1)
                with mock.patch.dict(os.environ, {'NOTIFY_SOCKET': path}):
                    boot.notify_started()
                self.assertEqual(listener.recv(128), b'READY=1')


class FakeRuntime:
    """Deliberate native display substitute, with real production lock checks."""
    def __init__(self, directory, mode='normal'):
        self.directory, self.mode = directory, mode
        self.events, self.sources = [], {}
        self.current_display = self
        self.closed = False
        self.presentation = self.application = None
        self.GLib = types.SimpleNamespace(PRIORITY_DEFAULT=0, unix_signal_add=self.signal,
                                         timeout_add=self.timer, source_remove=self.remove)
        self.Gdk = types.SimpleNamespace(Display=types.SimpleNamespace(get_default=lambda: self.current_display))

    def locked(self, event):
        self.events.append(event)
        try:
            contender = configuration.PageOwner(self.directory)
        except ValueError as error:
            if 'live owner' not in str(error):
                # Deliberate generation tests still retain the old FD; verify
                # that file separately rather than fabricate a current owner.
                if self.mode not in ('lock-change',): raise
        else:
            contender.close()
            raise AssertionError('Page ownership released before ' + event)

    def signal(self, priority, signum, callback):
        self.locked('signal-' + str(signum)); self.sources[signum] = callback
        return signum

    def timer(self, milliseconds, callback):
        self.locked('watchdog'); self.sources[10000] = callback
        return 10000

    def remove(self, source):
        self.locked('remove-' + str(source)); self.sources.pop(source)

    def flush(self):
        self.locked('flush')

    def close(self):
        self.locked('display-close')
        if self.mode == 'close-failure': raise RuntimeError('Deliberate display-close failure')
        self.closed = True
        self.current_display = None

    def is_closed(self):
        self.locked('closed-readback')
        return self.closed and self.mode != 'ignored-close'

    def load(self):
        self.locked('load')
        runtime = self
        class Presentation:
            error = ''
            cleanup_error = ''
            def __init__(self, layer, value, *, handoff):
                runtime.locked('presentation'); runtime.presentation = self
                handoff.verify()
            def close(self):
                runtime.locked('surfaces-retire')
                if runtime.mode == 'recorded-presentation-error': self.cleanup_error = 'Recorded presentation cleanup failure'
        class Application:
            startup_error = ''
            cleanup_error = ''
            def __init__(self, path, *, presentation):
                runtime.locked('application'); runtime.application = self
                self.presentation, self.closing = presentation, False
            def close_ui(self):
                runtime.locked('ui-close')
                if self.closing: return
                self.closing = True
                # Simulate default-display loss before outer teardown. The
                # wrapper must use its retained connection, not this entry.
                runtime.current_display = None
                if runtime.mode == 'destroy-failure': raise RuntimeError('Deliberate surface destruction failure')
                if runtime.mode == 'recorded-application-error': self.cleanup_error = 'Recorded application cleanup failure'
                self.presentation.close()
            def quit(self): runtime.locked('quit')
            def run(self, argv):
                runtime.locked('run')
                if runtime.mode in ('sigterm', 'sigint'):
                    runtime.sources[signal.SIGTERM if runtime.mode == 'sigterm' else signal.SIGINT]()
                elif runtime.mode == 'startup-failure': self.startup_error = 'Deliberate output loss'
                elif runtime.mode == 'lock-change':
                    (runtime.directory / 'page.lock').chmod(0o644)
                    runtime.sources[10000]()
                elif runtime.mode == 'socket-change':
                    (runtime.wayland / 'wayland-0').unlink()
                    (runtime.wayland / 'wayland-0').write_text('replacement')
                    runtime.sources[10000]()
                return 0
        return object(), self.GLib, object(), Presentation, Application, self.Gdk, self


class Tests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='page-boot-', dir='/tmp')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve(); self.root.chmod(0o700)
        self.state = self.root / 'state'
        self.profile, self.manager_path, self.receiver_path = (self.root / name for name in ('page.json', 'manager.json', 'receiver.json'))
        self.ultra, self.standard, self.ui = (self.root / name for name in ('ultra.conf', 'standard.conf', 'ui.conf'))
        for path, width in ((self.ultra, 1920), (self.standard, 1280)):
            path.write_text(self.displays(width)); path.chmod(0o600)
        self.ui.write_text('assetDirectory=/unopened/private/asset\nassetIdentifier=fixture\n'); self.ui.chmod(0o600)
        self.value = {'schemaVersion': 2, 'enabled': True, 'managerSocket': str(self.state / 'manager.sock'),
                      'surfaceControlFile': str(self.state / 'home-surface.request'),
                      'surfaceLeaseFile': str(self.state / 'home-surface.lease'),
                      'waylandDisplay': 'wayland-0', 'output': 'HDMI-A-1',
                      'homeButton': {'corner': 'bottom-right', 'margin': 16, 'size': 64}}
        self.manager = {'schemaVersion': 1, 'enabled': True, 'stateDirectory': str(self.state),
                        'standardDisplayConfig': str(self.standard), 'autoConnectSelected': False}
        self.settings = {'seconds': 0, 'media_state': str(self.state / 'media.json'), 'preview_sink': 'wayland-gpu',
                         'native_page_control_file': self.value['surfaceControlFile'],
                         'native_page_lease_file': self.value['surfaceLeaseFile'],
                         'display_config': str(self.ultra), 'bin_dir': str(CHECKER.parent), 'ultra_installed_ui': str(self.ui)}
        self.save()

    @staticmethod
    def displays(width):
        return (f'main.width={width}\nmain.height=720\nmain.fps=60\nmain.panel=Main\n'
                'main.wayland=wayland-0\nmain.output=HDMI-A-1\n'
                'cluster.width=1920\ncluster.height=720\ncluster.fps=30\ncluster.panel=Cluster\n'
                'cluster.wayland=wayland-0\ncluster.output=HDMI-A-2\n')

    def save(self):
        for path, value in ((self.profile, self.value), (self.manager_path, self.manager), (self.receiver_path, self.settings)):
            path.write_text(json.dumps(value)); path.chmod(0o600)

    def inspect(self):
        return configuration.inspect_boot(self.profile, self.manager_path, self.receiver_path, os.getuid(), CHECKER)

    def prepare_runtime(self, *, directory=True):
        if os.geteuid() == 0: self.skipTest('Actual runtime fixtures require a nonroot display account')
        if directory: self.state.mkdir(mode=0o700)
        self.wayland = self.root / 'runtime'; self.wayland.mkdir(mode=0o700)
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(endpoint.close)
        endpoint.bind(str(self.wayland / 'wayland-0')); endpoint.listen(1)
        environment = mock.patch.dict(os.environ, {'GDK_BACKEND': 'wayland', 'WAYLAND_DISPLAY': 'wayland-0',
                                                   'XDG_RUNTIME_DIR': str(self.wayland)})
        environment.start(); self.addCleanup(environment.stop)

    def runtime(self, mode='normal'):
        runtime = FakeRuntime(self.state, mode); runtime.wayland = self.wayland
        return runtime

    def run_fake(self, runtime):
        with mock.patch.object(boot, '_load_runtime', side_effect=runtime.load):
            return boot.run_configuration(self.value)

    def test_actual_static_boot_admission_creates_no_state_and_loads_no_gui(self):
        before = {path: (path.stat().st_ino, path.stat().st_mtime_ns, path.read_bytes()) for path in self.root.iterdir()}
        self.assertEqual(self.inspect(), self.value)
        self.assertEqual(before, {path: (path.stat().st_ino, path.stat().st_mtime_ns, path.read_bytes()) for path in self.root.iterdir()})
        self.assertFalse(self.state.exists())
        self.assertNotIn('gi', sys.modules)

    def test_real_validate_only_cli_does_not_need_runtime_root_or_gui(self):
        result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/phone_page_boot.py'), '--profile', str(self.profile),
                                 '--manager-profile', str(self.manager_path), '--receiver-config', str(self.receiver_path),
                                 '--validate-only'], capture_output=True, text=True, timeout=5,
                                env={**os.environ, 'GDK_BACKEND': 'invalid', 'WAYLAND_DISPLAY': 'wrong', 'XDG_RUNTIME_DIR': '/absent'})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {'configurationValid': True, 'enabled': True,
                                                   'waylandDisplay': 'wayland-0', 'output': 'HDMI-A-1'})
        self.assertFalse(self.state.exists())

    def bound_page(self):
        # Explicit manifest boundary substitute: real release/hash admission is
        # covered by appliance_release_test; all production page admission runs.
        from appliance_release import config_path
        (self.root / 'config').mkdir()
        paths = {'phone_page': self.profile, 'phone_manager': self.manager_path, 'receiver': self.receiver_path}
        for role, original in paths.items():
            target = config_path(self.root, role)
            target.write_bytes(original.read_bytes()); target.chmod(0o440)
        args = ['--release-root', str(self.root), '--release-manifest-sha256', 'a' * 64,
                '--profile', str(config_path(self.root, 'phone_page')),
                '--manager-profile', str(config_path(self.root, 'phone_manager')),
                '--receiver-config', str(config_path(self.root, 'receiver'))]
        manifest = {'version': 3, 'uid': os.getuid(),
                    'execution': {'receiver': 'phone-manager', 'resident_home': True}}
        return args, manifest

    def test_bound_home_validate_only_rechecks_release_before_success_without_gui_or_state(self):
        args, manifest = self.bound_page()
        out = io.StringIO()
        with mock.patch('appliance_release.verify', return_value=manifest) as verify, \
                mock.patch.object(boot, '_load_runtime') as runtime, contextlib.redirect_stdout(out):
            self.assertEqual(boot.main(args + ['--validate-only']), 0)
        self.assertEqual(verify.call_count, 2)
        self.assertTrue(json.loads(out.getvalue())['configurationValid'])
        runtime.assert_not_called(); self.assertFalse(self.state.exists())
        self.assertNotIn('gi', sys.modules)

    def test_bound_home_rejects_mixed_roles_variant_account_and_half_binding_before_admission(self):
        args, manifest = self.bound_page()
        variants = []
        for flag, path in (('--profile', self.profile), ('--manager-profile', self.manager_path),
                           ('--receiver-config', self.receiver_path)):
            changed = args.copy(); changed[changed.index(flag) + 1] = str(path)
            variants.append((changed, manifest))
        variants += [(args, manifest | {'version': 1}),
                     (args, manifest | {'version': 4}),
                     (args, manifest | {'version': 4, 'execution': manifest['execution'] | {'mode_policy': 'saved'}}),
                     (args, manifest | {'uid': os.getuid() + 1}),
                     (args, manifest | {'execution': {'receiver': 'phone-manager', 'resident_home': False}}),
                     (args[2:], manifest), (args[:2] + args[4:], manifest)]
        for selected, value in variants:
            with self.subTest(selected=selected, value=value), \
                    mock.patch('appliance_release.verify', return_value=value), \
                    mock.patch.object(boot, 'inspect_boot') as inspect, \
                    mock.patch.object(boot, 'run_configuration') as run, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(boot.main(selected), 1)
                inspect.assert_not_called(); run.assert_not_called()
        self.assertFalse(self.state.exists())

    def test_bound_phone_authority_home_rechecks_before_read_only_success(self):
        args, manifest = self.bound_page()
        manifest = manifest | {'version': 4, 'execution': manifest['execution'] | {'mode_policy': 'phone-authoritative'}}
        out = io.StringIO()
        with mock.patch('appliance_release.verify', return_value=manifest) as verify, \
                mock.patch.object(boot, '_load_runtime') as runtime, contextlib.redirect_stdout(out):
            self.assertEqual(boot.main(args + ['--validate-only']), 0)
        self.assertEqual(verify.call_count, 2)
        self.assertTrue(json.loads(out.getvalue())['configurationValid'])
        runtime.assert_not_called()
        self.assertFalse(self.state.exists())

    def test_bound_real_start_admits_before_opening_any_owner_or_display(self):
        args, manifest = self.bound_page()
        events = []
        def verify(binding):
            events.append('verify')
            return manifest
        real_inspect = boot.inspect_boot
        def inspect(*arguments):
            events.append('admit')
            return real_inspect(*arguments)
        from phone_manager_boot import _displays as real_displays
        from appliance_release import config_path
        bound_displays = config_path(self.root, 'displays')
        bound_displays.write_bytes(Path(self.settings['display_config']).read_bytes())
        bound_displays.chmod(0o440)
        def displays(*arguments):
            events.append('outputs')
            # The manifest/binary boundary is an explicit substitute here;
            # route parsing still uses the actual portable C++ checker.
            return real_displays(arguments[0], arguments[1], CHECKER)
        def run(value, *, startup_outputs):
            events.append('owner/display')
            self.assertEqual(value, self.value)
            self.assertEqual(startup_outputs, ('HDMI-A-1', 'HDMI-A-2'))
            return 0
        with mock.patch('appliance_release.verify', side_effect=verify), \
                mock.patch.object(boot, 'inspect_boot', side_effect=inspect), \
                mock.patch('phone_manager_boot._displays', side_effect=displays), \
                mock.patch('appliance_release.read_configuration_member', return_value=self.receiver_path.read_bytes()), \
                mock.patch.object(boot, 'run_configuration', side_effect=run):
            self.assertEqual(boot.main(args), 0)
        self.assertEqual(events[0:2], ['verify', 'admit'])
        self.assertEqual(events[-3:], ['verify', 'outputs', 'owner/display'])
        self.assertFalse(self.state.exists())

    def test_greeting_config_skips_legacy_cover_before_starting_page(self):
        self.settings['greeting_sequence_file'] = str(self.state / 'greeting-sequence')
        self.receiver_path.write_text(json.dumps(self.settings))
        args, manifest = self.bound_page()
        from appliance_release import config_path
        target = config_path(self.root, 'displays')
        target.write_bytes(Path(self.settings['display_config']).read_bytes())
        target.chmod(0o440)
        from phone_manager_boot import _displays as real_displays
        def displays(*arguments):
            return real_displays(arguments[0], arguments[1], CHECKER)
        with mock.patch('appliance_release.verify', return_value=manifest), \
                mock.patch('appliance_release.read_configuration_member', return_value=self.receiver_path.read_bytes()), \
                mock.patch('phone_manager_boot._displays', side_effect=displays), \
                mock.patch.object(boot, 'run_configuration', return_value=0) as run:
            self.assertEqual(boot.main(args), 0)
            self.assertEqual(run.call_args.kwargs, {'startup_outputs': ()})

    def test_real_start_invalid_profile_cannot_open_owner_display_or_notify(self):
        self.value['output'] = 'ABSENT'
        self.save()
        args = ['--profile', str(self.profile), '--manager-profile', str(self.manager_path),
                '--receiver-config', str(self.receiver_path)]
        with mock.patch.object(boot, 'run_configuration') as run, \
                mock.patch.object(boot, 'notify_started') as notify, \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(boot.main(args), 1)
        run.assert_not_called()
        notify.assert_not_called()
        self.assertFalse(self.state.exists())

    def test_bound_home_post_admission_damage_cannot_reach_owner_or_display(self):
        args, manifest = self.bound_page()
        with mock.patch('appliance_release.verify', side_effect=[manifest, ValueError('Damaged release')]), \
                mock.patch.object(boot, 'run_configuration') as run, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(boot.main(args), 1)
        run.assert_not_called(); self.assertFalse(self.state.exists())

    def test_all_manual_and_standard_camera_routes_are_rejected_without_opening_sources(self):
        original = copy.deepcopy(self.settings)
        variants = ({'camera_manual_profile': '/unopened/manual.json'},
                    {'preview_sink': 'wayland-gpu', 'standard_camera_control_file': '/unopened/request',
                     'standard_camera_lease_file': '/unopened/lease'})
        for options in variants:
            self.settings = {**original, **options}; self.save()
            with self.subTest(options=options):
                with self.assertRaisesRegex(ValueError, 'camera'): self.inspect()
        self.assertFalse(self.state.exists())

    def test_installed_ui_signed_and_native_camera_routes_and_duplicate_keys_are_rejected(self):
        for field in ('cameraProfile', 'nativeCameraControlFile', 'nativeCameraLeaseFile'):
            self.ui.write_text(field + '=/unopened/private/source\n')
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, 'camera routes'): self.inspect()
        for raw in ('nativeCameraControlFile=\nnativeCameraControlFile=/private/control\n', 'invalid', 'a=b\r\n', 'a=\x00b\n'):
            self.ui.write_text(raw)
            with self.assertRaises(ValueError): self.inspect()
        self.assertFalse(self.state.exists())

    def test_receiver_and_ui_files_cannot_be_aliased_hardlinked_or_unsafe(self):
        for path in (self.receiver_path, self.ui):
            original = path.read_bytes()
            alias = self.root / 'link'; os.link(path, alias)
            with self.assertRaises(ValueError): self.inspect()
            alias.unlink()
            path.chmod(0o666)
            with self.assertRaises(ValueError): self.inspect()
            path.chmod(0o600)
            path.unlink(); path.symlink_to(self.profile)
            with self.assertRaises(ValueError): self.inspect()
            path.unlink(); path.write_bytes(original); path.chmod(0o600)

    def test_profile_receiver_ui_generations_remain_pinned_across_production_admission(self):
        actual = configuration.inspect_settings
        for path in (self.profile, self.manager_path, self.receiver_path, self.ui):
            self.save(); self.ui.write_text('assetDirectory=/unopened/private/asset\n')
            def mutate(*args, **kwargs):
                result = actual(*args, **kwargs)
                path.write_bytes(path.read_bytes() + b'\n')
                return result
            with self.subTest(path=path.name), mock.patch.object(configuration, 'inspect_settings', side_effect=mutate):
                with self.assertRaisesRegex(ValueError, 'changed during validation'): self.inspect()

    def test_equal_metadata_does_not_hide_changed_boot_input_bytes(self):
        import phone_manager_boot
        actual = configuration.inspect_settings
        def metadata_without_times(info):
            return (info.st_dev, info.st_ino, info.st_size, info.st_uid, info.st_mode, info.st_nlink)
        for path in (self.profile, self.manager_path, self.receiver_path, self.ui):
            self.save(); self.ui.write_text('assetDirectory=/unopened/private/asset\n')
            def mutate(*args, **kwargs):
                result = actual(*args, **kwargs)
                original = path.read_bytes()
                # Change one byte only after actual production admission. The
                # final guard must reject it without parsing or starting owners.
                path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
                return result
            with self.subTest(path=path.name), \
                 mock.patch.object(phone_manager_boot, '_signature', side_effect=metadata_without_times), \
                 mock.patch.object(configuration, '_signature', side_effect=metadata_without_times), \
                 mock.patch.object(configuration, 'inspect_settings', side_effect=mutate):
                with self.assertRaisesRegex(ValueError, 'changed during validation'): self.inspect()
            self.assertFalse(self.state.exists())

    def test_disabled_and_mismatched_profiles_do_not_admit_an_explicit_service(self):
        self.value['enabled'] = False; self.save()
        with self.assertRaises(ValueError): self.inspect()
        self.value['enabled'] = True; self.manager['enabled'] = False; self.save()
        with self.assertRaises(ValueError): self.inspect()
        self.manager['enabled'] = True; self.value['output'] = 'HDMI-A-2'; self.save()
        with self.assertRaises(ValueError): self.inspect()
        self.assertFalse(self.state.exists())

    def test_missing_manager_directory_does_not_load_gui_or_create_state(self):
        self.prepare_runtime(directory=False)
        with mock.patch.object(boot, '_load_runtime') as load:
            with self.assertRaises(FileNotFoundError): boot.run_configuration(self.value)
        load.assert_not_called(); self.assertFalse(self.state.exists())

    def test_runtime_environment_and_owned_real_wayland_socket_are_required_before_gui(self):
        self.prepare_runtime()
        variants = ({'GDK_BACKEND': 'x11'}, {'GDK_BACKEND': 'wayland,x11'}, {'WAYLAND_DISPLAY': 'wayland-9'},
                    {'XDG_RUNTIME_DIR': str(self.wayland) + '/'}, {'XDG_RUNTIME_DIR': '/absent'},
                    {'XDG_RUNTIME_DIR': str(self.root) + '/./runtime'})
        with mock.patch.object(boot, '_load_runtime') as load:
            for environment in variants:
                with self.subTest(environment=environment), mock.patch.dict(os.environ, environment):
                    with self.assertRaises((OSError, ValueError)): boot.run_configuration(self.value)
            self.wayland.chmod(0o755)
            with self.assertRaises(ValueError): boot.run_configuration(self.value)
            self.wayland.chmod(0o700)
            (self.wayland / 'wayland-0').unlink(); (self.wayland / 'wayland-0').write_text('ordinary data')
            with self.assertRaises(ValueError): boot.run_configuration(self.value)
        load.assert_not_called(); self.assertFalse((self.state / 'page.lock').exists())

    def test_actual_owner_is_claimed_before_runtime_and_retained_through_display_close(self):
        self.prepare_runtime()
        runtime = self.runtime()
        self.assertEqual(self.run_fake(runtime), 0)
        self.assertEqual(runtime.events[0], 'load')
        self.assertLess(runtime.events.index('surfaces-retire'), runtime.events.index('flush'))
        self.assertLess(runtime.events.index('flush'), runtime.events.index('display-close'))
        self.assertLess(runtime.events.index('display-close'), runtime.events.index('closed-readback'))
        self.assertTrue(runtime.closed)
        with configuration.PageOwner(self.state): pass

    def test_actual_loader_preloads_fixed_library_before_gi_and_captures_display_before_run(self):
        self.prepare_runtime(); runtime = self.runtime()
        gi, repository = types.ModuleType('gi'), types.ModuleType('gi.repository')
        gi.__path__ = []
        gi.require_version = lambda name, version: runtime.locked('require-' + name + '-' + version)
        repository.GLib, repository.Gdk, repository.Gtk4LayerShell = runtime.GLib, runtime.Gdk, object()
        def initialize(): runtime.locked('gtk-init'); return True
        repository.Gtk = types.SimpleNamespace(init_check=initialize)
        phone, presentation = types.ModuleType('phone_page'), types.ModuleType('phone_page_presentation')
        library = types.ModuleType('ctypes'); library.RTLD_GLOBAL = 256
        def preload(name, *, mode):
            runtime.locked('preload')
            self.assertEqual(name, 'libgtk4-layer-shell.so.0'); self.assertEqual(mode, 256)
            values = runtime.load()
            phone.PhonePage, presentation.ResidentPresentation = values[4], values[3]
            return object()
        library.CDLL = preload
        with mock.patch.dict(sys.modules, {'ctypes': library, 'gi': gi, 'gi.repository': repository,
                                          'phone_page': phone, 'phone_page_presentation': presentation}):
            self.assertEqual(boot.run_configuration(self.value), 0)
        self.assertLess(runtime.events.index('preload'), runtime.events.index('require-Gtk-4.0'))
        self.assertLess(runtime.events.index('require-Gtk4LayerShell-1.0'), runtime.events.index('gtk-init'))
        self.assertLess(runtime.events.index('gtk-init'), runtime.events.index('run'))
        self.assertTrue(runtime.closed)

    def test_unavailable_library_fails_after_ownership_without_leaving_a_live_page(self):
        self.prepare_runtime()
        with mock.patch.object(boot, '_load_runtime', side_effect=OSError('Deliberate missing layer library')):
            with self.assertRaisesRegex(OSError, 'layer library'): boot.run_configuration(self.value)
        with configuration.PageOwner(self.state): pass

    def test_both_orderly_signal_callbacks_retire_surfaces_and_connection_before_unlock(self):
        self.prepare_runtime()
        for mode in ('sigterm', 'sigint'):
            runtime = self.runtime(mode)
            self.assertEqual(self.run_fake(runtime), 0)
            self.assertIn('quit', runtime.events)
            self.assertTrue(runtime.closed)
            with configuration.PageOwner(self.state): pass

    def test_output_startup_failure_is_nonzero_but_still_retires_connection(self):
        self.prepare_runtime(); runtime = self.runtime('startup-failure')
        with self.assertRaisesRegex(RuntimeError, 'output loss'): self.run_fake(runtime)
        self.assertTrue(runtime.closed)
        with configuration.PageOwner(self.state): pass

    def test_watchdog_detects_actual_socket_replacement_and_retires_the_owned_connection(self):
        self.prepare_runtime(); runtime = self.runtime('socket-change')
        with self.assertRaisesRegex(RuntimeError, 'Wayland socket'): self.run_fake(runtime)
        self.assertIn('quit', runtime.events); self.assertTrue(runtime.closed)
        with configuration.PageOwner(self.state): pass

    def test_watchdog_rejects_actual_owner_metadata_change_before_unlock(self):
        self.prepare_runtime(); runtime = self.runtime('lock-change')
        with self.assertRaisesRegex(RuntimeError, 'lock generation'): self.run_fake(runtime)
        self.assertIn('quit', runtime.events); self.assertTrue(runtime.closed)
        self.assertEqual((self.state / 'page.lock').stat().st_mode & 0o777, 0o644)
        (self.state / 'page.lock').chmod(0o600)
        with configuration.PageOwner(self.state): pass

    def test_destroy_failure_still_closes_the_retained_display_and_returns_failure(self):
        self.prepare_runtime(); runtime = self.runtime('destroy-failure')
        with self.assertRaisesRegex(RuntimeError, 'surface destruction'): self.run_fake(runtime)
        self.assertTrue(runtime.closed)
        self.assertIn('closed-readback', runtime.events)
        with configuration.PageOwner(self.state): pass

    def test_failed_or_ignored_display_close_keeps_lock_until_explicit_fixture_retirement(self):
        self.prepare_runtime()
        for mode in ('close-failure', 'ignored-close'):
            held = []
            actual = configuration.PageOwner
            def acquire(directory):
                owner = actual(directory); held.append(owner); return owner
            runtime = self.runtime(mode)
            try:
                with mock.patch.object(boot, 'PageOwner', side_effect=acquire):
                    with self.assertRaises((RuntimeError, ValueError)): self.run_fake(runtime)
                with self.assertRaisesRegex(ValueError, 'live owner'): actual(self.state)
            finally:
                # The deliberate failed display substitute has no real server
                # connection. Retire this test-owned FD only after proving hold.
                for owner in held: owner.close()
            with actual(self.state): pass

    def test_recorded_cleanup_errors_are_nonzero_even_when_display_retirement_succeeds(self):
        self.prepare_runtime()
        for mode in ('recorded-presentation-error', 'recorded-application-error'):
            runtime = self.runtime(mode)
            with self.assertRaisesRegex(RuntimeError, 'cleanup failure'): self.run_fake(runtime)
            self.assertTrue(runtime.closed)
            with configuration.PageOwner(self.state): pass


if __name__ == '__main__':
    unittest.main()
