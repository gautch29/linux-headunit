#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Run the optional owned native Home/phone page on its admitted main output."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import stat
import sys

from phone_page_configuration import PageOwner, inspect_boot, validate_configuration
from main_surface_handoff import MainSurfaceHandoff


def notify_started():
    """Release the cluster's startup ordering after decoration admission.

    This says only that the page has established or skipped its decoration;
    it is not a phone readiness or physical presentation receipt.
    """
    path = os.environ.get('NOTIFY_SOCKET')
    if path:
        if path.startswith('@'):
            path = '\0' + path[1:]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as connection:
            connection.sendto(b'READY=1', path)


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _identity(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_mode, info.st_nlink, info.st_ctime_ns)


class _WaylandEndpoint:
    """Pin only the configured real owned socket; never create a runtime path."""
    def __init__(self, configuration):
        self.fd = -1
        uid = os.geteuid()
        _require(uid != 0 and uid == os.getuid(), 'Run the native phone page as the nonroot display user')
        _require(os.environ.get('GDK_BACKEND') == 'wayland', 'Native phone page requires GDK_BACKEND=wayland')
        _require(os.environ.get('WAYLAND_DISPLAY') == configuration['waylandDisplay'],
                 'WAYLAND_DISPLAY must equal the configured main display')
        value = os.environ.get('XDG_RUNTIME_DIR', '')
        _require(value.startswith('/') and not value.endswith('/') and len(value) <= 4000 and
                 all(32 <= ord(c) <= 126 for c in value) and os.path.normpath(value) == value and
                 all(part and part not in ('.', '..') for part in value.split('/')[1:]),
                 'XDG_RUNTIME_DIR must be an explicit canonical absolute directory')
        self.directory = Path(value)
        self.name = configuration['waylandDisplay']
        self.path = self.directory / self.name
        _require(len(os.fsencode(self.path)) <= 103, 'Native-page Wayland socket exceeds its Unix path bound')
        try:
            before = self.directory.lstat()
            _require(self._private(before) and self.directory.resolve(strict=True) == self.directory,
                     'Native-page runtime must be real, owned and 0700 without redirected ancestors')
            self.fd = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
            self.directory_identity = os.fstat(self.fd)
            _require((before.st_dev, before.st_ino) == (self.directory_identity.st_dev, self.directory_identity.st_ino),
                     'Native-page runtime changed during open')
            self.socket_identity = os.stat(self.name, dir_fd=self.fd, follow_symlinks=False)
            self.verify()
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _private(info):
        return stat.S_ISDIR(info.st_mode) and info.st_uid == os.geteuid() and (info.st_mode & 0o7777) == 0o700

    def verify(self):
        _require(self.fd >= 0, 'Native-page runtime is closed')
        current, opened = self.directory.lstat(), os.fstat(self.fd)
        _require(self._private(current) and self._private(opened) and
                 (current.st_dev, current.st_ino) == (opened.st_dev, opened.st_ino) ==
                 (self.directory_identity.st_dev, self.directory_identity.st_ino) and
                 self.directory.resolve(strict=True) == self.directory,
                 'Native-page runtime directory generation changed or became unsafe')
        current = os.stat(self.name, dir_fd=self.fd, follow_symlinks=False)
        _require(stat.S_ISSOCK(current.st_mode) and current.st_uid == os.geteuid() and current.st_nlink == 1 and
                 not current.st_mode & 0o7000 and _identity(current) == _identity(self.socket_identity) and
                 _identity(self.path.lstat()) == _identity(current),
                 'Configured native-page Wayland socket changed or is not owned real socket data')

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1


def _load_runtime():
    """Preload the fixed layer-shell SONAME before any GTK/Wayland GI import.

    Follows the upstream gtk4-layer-shell Python preload requirement; no
    library path or fallback backend is accepted from the profile or CLI.
    """
    _require(not any(name in sys.modules for name in ('gi.repository.Gtk', 'gi.repository.Gdk',
                                                      'gi.repository.Gtk4LayerShell')),
             'GTK was imported before the native layer-shell preload')
    import ctypes
    library = ctypes.CDLL('libgtk4-layer-shell.so.0', mode=ctypes.RTLD_GLOBAL)
    import gi
    gi.require_version('Gtk', '4.0')
    gi.require_version('Gdk', '4.0')
    gi.require_version('Gtk4LayerShell', '1.0')
    from gi.repository import GLib, Gtk4LayerShell, Gdk, Gtk
    _require(Gtk.init_check(), 'Native-page GTK could not open the configured Wayland display')
    display = Gdk.Display.get_default()
    _require(display is not None, 'Native-page GDK display is unavailable')
    from phone_page_presentation import ResidentPresentation
    from phone_page import PhonePage
    return library, GLib, Gtk4LayerShell, ResidentPresentation, PhonePage, Gdk, display


def run_configuration(configuration, *, startup_outputs=()):
    """Run one admitted configuration; keep ownership through surface cleanup."""
    configuration = validate_configuration(configuration)
    _require(configuration['enabled'], 'Native phone page is disabled')
    endpoint = _WaylandEndpoint(configuration)
    owner = handoff = None
    presentation = application = GLib = Gdk = None
    owned_display = None
    sources = []
    failure = ''
    code = 1
    retired = False
    try:
        # Missing directory is a service startup/retry condition. The frontend
        # never creates the manager's state or contacts Bluetooth itself.
        owner = PageOwner(Path(configuration['managerSocket']).parent)
        endpoint.verify(); owner.verify()
        handoff = MainSurfaceHandoff(configuration['surfaceControlFile'], configuration['surfaceLeaseFile'])
        library, GLib, layer, Presentation, Application, Gdk, owned_display = _load_runtime()
        endpoint.verify(); owner.verify()
        presentation = Presentation(layer, configuration, handoff=handoff)
        presentation.startup_outputs = tuple(startup_outputs)
        presentation.startup_ready = notify_started
        application = Application(Path(configuration['managerSocket']), presentation=presentation)
        # _load_runtime retains the actual connection even if destroying the
        # windows later clears GDK's default-display entry.

        def stop():
            try:
                application.close_ui()
            finally:
                application.quit()
            return True

        for signum in (signal.SIGTERM, signal.SIGINT):
            sources.append(GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signum, stop))

        def health():
            nonlocal failure
            try:
                owner.verify(); endpoint.verify()
                if application.startup_error:
                    raise RuntimeError(application.startup_error)
            except Exception as error:
                failure = str(error)[:300]
                stop()
            return True

        sources.append(GLib.timeout_add(250, health))
        code = application.run([])
        try:
            owner.verify(); endpoint.verify()
        except Exception as error:
            failure = failure or str(error)[:300]
        failure = failure or application.startup_error or presentation.error
    finally:
        cleanup_errors = []
        if GLib is not None:
            for source in sources:
                try:
                    GLib.source_remove(source)
                except Exception as error:
                    cleanup_errors.append(str(error))
        # Surface destruction may still be queued while a slow IPC worker
        # keeps this process alive. Flush and close our private GDK connection
        # before releasing ownership; compositor receipt needs a live fixture.
        try:
            if application is not None:
                try:
                    application.close_ui()
                except Exception as error:
                    cleanup_errors.append(str(error))
            if presentation is not None:
                try:
                    presentation.close()
                except Exception as error:
                    cleanup_errors.append(str(error))
            for obj in (application, presentation):
                error = getattr(obj, 'cleanup_error', '') if obj is not None else ''
                if error:
                    cleanup_errors.append(str(error))
            # Recover a partially loaded runtime without importing new GI.
            Gdk = Gdk if Gdk is not None else sys.modules.get('gi.repository.Gdk')
            display = owned_display if owned_display is not None else (Gdk.Display.get_default() if Gdk is not None else None)
            if display is not None:
                try:
                    display.flush()
                except Exception as error:
                    cleanup_errors.append(str(error))
                finally:
                    display.close()
                _require(display.is_closed(), 'Native-page GDK connection did not close')
                retired = True
            else:
                retired = not cleanup_errors
        finally:
            endpoint.close()
            if handoff is not None and retired:
                try:
                    handoff.close()
                except Exception as error:
                    cleanup_errors.append(str(error))
            if owner is not None and retired:
                owner.close()
        if cleanup_errors:
            raise RuntimeError('Native-page cleanup: ' + '; '.join(cleanup_errors)[:250])
    if failure:
        raise RuntimeError(failure)
    _require(type(code) is int and code == 0, 'Native phone page exited unsuccessfully')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--manager-profile', required=True, type=Path)
    parser.add_argument('--receiver-config', required=True, type=Path)
    parser.add_argument('--release-root', type=Path)
    parser.add_argument('--release-manifest-sha256')
    parser.add_argument('--validate-only', action='store_true')
    arguments = parser.parse_args(argv)
    try:
        binding = None
        if bool(arguments.release_root) != bool(arguments.release_manifest_sha256):
            raise ValueError('A release root and manifest digest must be supplied together')
        if arguments.release_root:
            from appliance_release import config_path, verify
            binding = {'path': str(arguments.release_root), 'sha256': arguments.release_manifest_sha256}
            release = verify(binding)
            expected = {'receiver': 'phone-manager', 'resident_home': True}
            if release['version'] in (4, 5):
                expected['mode_policy'] = 'phone-authoritative'
            if (release['version'] not in (3, 4, 5) or
                    release.get('execution') != expected or
                    arguments.receiver_config != config_path(arguments.release_root, 'receiver') or
                    arguments.manager_profile != config_path(arguments.release_root, 'phone_manager') or
                    arguments.profile != config_path(arguments.release_root, 'phone_page') or
                    os.geteuid() != release['uid'] or os.getuid() != release['uid']):
                raise ValueError('Native Home must use its exact managed release profiles and display account')
        configuration = inspect_boot(arguments.profile, arguments.manager_profile, arguments.receiver_config)
        if binding is not None:
            # Recheck the sealed bytes after production profile/display
            # admission, before even a validate-only success or GTK/owner effects.
            verify(binding)
        if arguments.validate_only:
            print(json.dumps({'configurationValid': True, 'enabled': configuration['enabled'],
                              'waylandDisplay': configuration['waylandDisplay'], 'output': configuration['output']}))
            return 0
        if binding is not None:
            from phone_manager_boot import _displays
            panels = _displays(config_path(arguments.release_root, 'displays'), os.getuid(),
                               arguments.release_root / 'build/headunit-display-config-check')
            from appliance_release import read_configuration_member
            from receiver_configuration import parse_receiver_configuration
            receiver_path = config_path(arguments.release_root, 'receiver')
            settings = parse_receiver_configuration(
                read_configuration_member(receiver_path, binding, 'receiver'), receiver_path)
            # The opt-in sequence keeps live gauges exposed. Its main component
            # is integrated by the native UI owner; never stack the legacy opaque
            # dual-panel splash above these concurrently running instruments.
            outputs = () if settings.get('greeting_sequence_file') else tuple(panel['output'] for panel in panels
                            if panel['wayland'] == configuration['waylandDisplay'])
            return run_configuration(configuration, startup_outputs=outputs)
        return run_configuration(configuration)
    except Exception as error:
        print('Native phone page: ' + str(error)[:300], file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
