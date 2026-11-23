# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Offline tests of the actual opt-in Standard preview launcher configuration."""
import contextlib
import io
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from receiver_configuration import load_receiver_configuration, validate_device_settings
import run_lab_receiver as launcher


class StandardPreviewConfigurationTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name).resolve()
        self.root.chmod(0o700)
        self.settings = {"preview_sink": "wayland-gpu", "display_config": self.root / "displays.conf",
                         "standard_camera_control_file": self.root / "camera.request",
                         "standard_camera_lease_file": self.root / "main.lease"}
        self.displays = {"main.wayland": "headunit-0", "main.output": "HDMI-A-1",
                         "cluster.wayland": "headunit-0", "cluster.output": "HDMI-A-2"}

    def environment(self, settings=None, displays=None, owner=None):
        return launcher.standard_preview_environment({}, self.settings if settings is None else settings,
                    self.displays if displays is None else displays, os.getuid() if owner is None else owner)

    def endpoint(self, name, contents):
        path = self.root / name
        path.write_bytes(contents)
        path.chmod(0o600)
        return path

    def test_new_saved_fields_keep_absolute_endpoint_names(self):
        profile = self.root / "receiver.json"
        profile.write_text(json.dumps({key: str(value) for key, value in self.settings.items()}))
        loaded = load_receiver_configuration(profile)
        self.assertEqual(loaded["preview_sink"], "wayland-gpu")
        for key in ("standard_camera_control_file", "standard_camera_lease_file"):
            self.assertEqual(loaded[key], self.settings[key])
        self.assertEqual(self.environment(loaded)["HEADUNIT_STANDARD_CAMERA_LEASE"], str(self.root / "main.lease"))

    def test_missing_endpoints_are_not_created(self):
        before = tuple(self.root.iterdir())
        result = self.environment()
        self.assertEqual(tuple(self.root.iterdir()), before)
        self.assertEqual(result["HEADUNIT_STANDARD_CAMERA_CONTROL"], str(self.root / "camera.request"))
        self.assertEqual(result["HEADUNIT_STANDARD_CAMERA_LEASE"], str(self.root / "main.lease"))

    def test_existing_private_endpoints_are_not_changed(self):
        request = self.endpoint("camera.request", b"on\n")
        lease = self.endpoint("main.lease", b"")
        before = tuple((path.stat().st_ino, path.stat().st_mtime_ns, path.read_bytes()) for path in (request, lease))
        self.environment()
        after = tuple((path.stat().st_ino, path.stat().st_mtime_ns, path.read_bytes()) for path in (request, lease))
        self.assertEqual(after, before)

    def test_saved_endpoint_link_is_preserved_and_rejected_without_touching_target(self):
        target = self.endpoint("target", b"off\n")
        request = self.root / "camera.request"
        request.symlink_to(target)
        profile = self.root / "receiver.json"
        profile.write_text(json.dumps({key: str(value) for key, value in self.settings.items()}))
        loaded = load_receiver_configuration(profile)
        self.assertEqual(loaded["standard_camera_control_file"], request)
        with self.assertRaises(ValueError):
            self.environment(loaded)
        self.assertTrue(request.is_symlink())
        self.assertEqual(target.read_bytes(), b"off\n")

    def test_inherited_camera_flags_are_cleared_and_input_environment_is_unchanged(self):
        old = {"HEADUNIT_STANDARD_CAMERA_CONTROL": "old-control", "HEADUNIT_STANDARD_CAMERA_LEASE": "old-lease",
               "UNRELATED": "retained"}
        for sink in ("none", "fake", "wayland-touch", "wayland-gpu"):
            settings = {"preview_sink": sink, "display_config": self.root / "displays.conf"}
            with self.subTest(sink=sink):
                self.assertEqual(launcher.standard_preview_environment(old, settings, self.displays, os.getuid()),
                                 {"UNRELATED": "retained"})
        self.assertEqual(old["HEADUNIT_STANDARD_CAMERA_CONTROL"], "old-control")
        explicit = launcher.standard_preview_environment(old, self.settings, self.displays, os.getuid())
        self.assertEqual(explicit["HEADUNIT_STANDARD_CAMERA_CONTROL"], str(self.settings["standard_camera_control_file"]))
        self.assertEqual(explicit["UNRELATED"], "retained")

    def test_pair_backend_and_mode_requirements(self):
        changes = [{"preview_sink": sink} for sink in ("none", "fake", "x11", "wayland", "wayland-touch")]
        changes += [{"ultra_negotiate": True}, {"ultra_native_video": True}, {"display_config": None}]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.environment(self.settings | change)
        for removed in ("standard_camera_control_file", "standard_camera_lease_file"):
            settings = dict(self.settings)
            settings.pop(removed)
            with self.subTest(removed=removed), self.assertRaises(ValueError):
                validate_device_settings(settings)

    def test_gpu_without_camera_still_requires_standard_and_named_main_output(self):
        settings = {"preview_sink": "wayland-gpu", "display_config": self.root / "displays.conf"}
        self.assertEqual(self.environment(settings), {})
        for fields in ({}, {"main.wayland": "headunit-0"}, {"main.output": "HDMI-A-1"},
                       {"main.wayland": "headunit-0", "main.output": ""}):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                self.environment(settings, fields)
        for changes in ({"display_config": None}, {"ultra_negotiate": True}, {"ultra_native_video": True}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.environment(settings | changes)

    def test_invalid_endpoint_names_do_not_get_normalized_into_valid_names(self):
        bad = ("", "relative/request", "~/request", "/", str(self.root) + "/request/",
               str(self.root) + "/./request", str(self.root) + "/../request", "/bad\0name", "/bad\nname", "/" + "x" * 4000,
               True, 3, [])
        for key in ("standard_camera_control_file", "standard_camera_lease_file"):
            for value in bad:
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    validate_device_settings(self.settings | {key: value})
        profile = self.root / "receiver.json"
        raw = {key: str(value) for key, value in self.settings.items()}
        for value in ("relative/request", str(self.root) + "/./request", str(self.root) + "/../request"):
            profile.write_text(json.dumps(raw | {"standard_camera_control_file": value}))
            with self.subTest(saved=value), self.assertRaises(ValueError):
                load_receiver_configuration(profile)

    def test_parent_must_be_private_owned_and_real(self):
        for mode in (0o750, 0o755, 0o1700):
            self.root.chmod(mode)
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.environment()
        self.root.chmod(0o700)
        with self.assertRaises(ValueError):
            self.environment(owner=os.getuid() + 1)
        linked = self.root / "linked"
        linked.symlink_to(self.root, target_is_directory=True)
        changed = self.settings | {"standard_camera_control_file": linked / "request"}
        with self.assertRaises(ValueError):
            self.environment(changed)
        with self.assertRaises(FileNotFoundError):
            self.environment(self.settings | {"standard_camera_control_file": self.root / "missing" / "request"})

    def test_existing_request_rejects_unsafe_modes_and_nonregular_nodes(self):
        request = self.endpoint("camera.request", b"on\n")
        for mode in (0o640, 0o644, 0o1600):
            request.chmod(mode)
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.environment()
        request.unlink()
        request.symlink_to(self.root / "uncreated")
        with self.assertRaises(ValueError):
            self.environment()
        request.unlink()
        request.mkdir(mode=0o700)
        with self.assertRaises(ValueError):
            self.environment()
        request.rmdir()
        os.mkfifo(request, 0o600)
        with self.assertRaises(ValueError):
            self.environment()

    def test_request_size_lease_size_and_links(self):
        request = self.endpoint("camera.request", b"on\n")
        lease = self.endpoint("main.lease", b"")
        request.write_bytes(b"x" * 25)
        with self.assertRaises(ValueError):
            self.environment()
        request.write_bytes(b"off\n")
        lease.write_bytes(b"x")
        with self.assertRaises(ValueError):
            self.environment()
        lease.write_bytes(b"")
        os.link(lease, self.root / "another.lease")
        with self.assertRaises(ValueError):
            self.environment()

    def test_timed_request_preflight_preserves_bytes_and_rejects_hardlinks(self):
        request = self.endpoint("camera.request", b"on 18446744073709551615\n")
        before = request.read_bytes()
        self.environment()  # Semantic deadline/expiry checking belongs to ActivationControl.
        self.assertEqual(request.read_bytes(), before)
        os.link(request, self.root / "another.request")
        with self.assertRaises(ValueError):
            self.environment()

    def test_request_and_lease_must_be_distinct(self):
        with self.assertRaises(ValueError):
            self.environment(self.settings | {"standard_camera_lease_file": self.settings["standard_camera_control_file"]})
        # Lexically different ancestors may still identify one real parent/leaf.
        alias = self.root / "ancestor-alias"
        alias.symlink_to(self.root, target_is_directory=True)
        child = self.root / "child"
        child.mkdir(mode=0o700)
        changed = self.settings | {"standard_camera_control_file": child / "same",
                                  "standard_camera_lease_file": alias / "child" / "same"}
        with self.assertRaises(ValueError):
            self.environment(changed)

    def test_wayland_socket_preflight_uses_selected_socket_and_preserves_environment(self):
        runtime = self.root / str(os.getuid())
        runtime.mkdir(mode=0o700)
        selected = runtime / "headunit-0"
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(selected))
            inherited = {"WAYLAND_DISPLAY": "incorrect-old", "UNRELATED": "retained"}
            result = launcher.wayland_environment(inherited, os.getuid(), "headunit-0", self.root)
            self.assertEqual(result["WAYLAND_DISPLAY"], "headunit-0")
            self.assertEqual(result["XDG_RUNTIME_DIR"], str(runtime))
            self.assertEqual(result["UNRELATED"], "retained")
            self.assertEqual(inherited["WAYLAND_DISPLAY"], "incorrect-old")
            for display in ("", ".", "..", "../headunit-0", str(selected)):
                with self.subTest(display=display), self.assertRaises(ValueError):
                    launcher.wayland_environment({}, os.getuid(), display, self.root)
            with self.assertRaises(FileNotFoundError):
                launcher.wayland_environment({}, os.getuid(), "missing-0", self.root)
        selected.unlink()
        selected.write_bytes(b"")
        with self.assertRaises(RuntimeError):
            launcher.wayland_environment({}, os.getuid(), "headunit-0", self.root)

    def test_actual_launcher_dry_run_accepts_saved_gpu_and_cli_pair(self):
        profile = self.root / "receiver.json"
        for filename, content in (("wifi.conf", "ssid=OfflineTest\npassphrase=TestPassword123\nchannel=36\n"),
                                  ("identity.conf", "name=OfflineTest\n"), ("key", "private test fixture"),
                                  ("secret", "private test fixture")):
            self.endpoint(filename, content.encode())
        display = self.root / "displays.conf"
        display.write_text("\n".join(key + "=" + value for key, value in self.displays.items()))
        saved = {"wifi_config": "wifi.conf", "identity": "identity.conf", "pairing_key": "key",
                 "setup_secret": "secret", "pair_store": "pairs", "capture_root": "captures", "bin_dir": "build",
                 "display_config": "displays.conf", "preview_sink": "wayland-gpu", "standard_cluster": True}
        profile.write_text(json.dumps(saved))
        arguments = ["run_lab_receiver.py", "--config", str(profile), "--dry-run",
                     "--standard-camera-control-file", str(self.settings["standard_camera_control_file"]),
                     "--standard-camera-lease-file", str(self.settings["standard_camera_lease_file"])]
        mask = os.umask(0o077)
        try:
            with mock.patch.object(sys, "argv", arguments), mock.patch.object(launcher, "run") as checked, \
                    mock.patch.object(launcher, "check_identity_configuration"), contextlib.redirect_stdout(io.StringIO()) as output:
                launcher.main()
        finally:
            os.umask(mask)
        self.assertIn("configuration valid; no services started", output.getvalue())
        checked.assert_called_once_with(str(self.root / "build" / "headunit-display-config-check"), str(display))
        self.assertFalse((self.root / "pairs").exists())
        self.assertFalse((self.root / "captures").exists())
        self.assertFalse((self.root / "camera.request").exists())
        self.assertFalse((self.root / "main.lease").exists())


if __name__ == "__main__":
    unittest.main()
