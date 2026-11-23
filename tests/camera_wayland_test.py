#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Strict CLI checks; optional real Wayland manual/lease lifecycle probe.

Run --lifecycle only inside theme_wayland_gpu_test.py's isolated compositor.
This never starts/stops appliance services and uses only temporary request files.
"""
import argparse
import copy
import fcntl
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


def write(path, value, mode=0o600):
    path.write_text(value)
    path.chmod(mode)


def profile(source="synthetic-test-pattern", enabled=True, manual_activation=True):
    return {
        "schemaVersion": 1,
        "enabled": enabled,
        "manualActivation": manual_activation,
        "streamIdentifier": "native-camera-test",
        "source": {"kind": source},
        "capture": {"width": 320, "height": 180, "frameRate": 30, "maximumAgeMs": 150},
        "destination": {
            "panel": "main", "canvas": {"width": 640, "height": 360},
            "frame": {"x": 80, "y": 36, "width": 480, "height": 288},
            "rotationDegrees": 90, "mirrorHorizontal": True,
        },
    }


def display_config(output):
    return (
        "main.width=640\nmain.height=360\nmain.fps=30\nmain.panel=Center_Display\n"
        f"main.wayland=native-test\nmain.output={output}\n"
        "cluster.width=640\ncluster.height=360\ncluster.fps=30\ncluster.panel=Cluster_Display\n"
    )


class Fixture:
    def __init__(self, base, binary, output):
        self.base = base
        self.binary = binary
        self.profile = base / "camera.json"
        self.display = base / "display.conf"
        self.request = base / "request"
        self.lease = base / "lease"
        self.output = output
        self.reset()

    def reset(self):
        for path in (self.profile, self.display, self.request, self.lease):
            if path.exists() or path.is_symlink():
                path.unlink()
        self.base.chmod(0o700)
        write(self.profile, json.dumps(profile()))
        write(self.display, display_config(self.output))

    def args(self):
        return [self.binary, "--camera-profile", str(self.profile), "--display-config", str(self.display),
                "--request-file", str(self.request), "--surface-lease-file", str(self.lease)]

    def validate(self, args=None, good=False):
        result = subprocess.run((args or self.args()) + ["--validate-only"],
                                capture_output=True, text=True, timeout=3)
        assert len(result.stdout) < 2048 and len(result.stderr) < 2048, "unbounded validation output"
        assert (result.returncode == 0) == good, result.stdout + result.stderr
        if good:
            value = json.loads(result.stdout)
            camera = json.loads(self.profile.read_text())
            assert value == {"configurationValid": True, "enabled": camera["enabled"] and camera["manualActivation"],
                             "wayland": "native-test", "output": self.output, "width": 640, "height": 360}
        return result


def validations(f):
    f.validate(good=True)
    assert not f.lease.exists(), "validation created ownership lease"
    control = f.base / 'manual.sock'
    control_args = f.args() + ['--manual-control-socket', str(control)]
    f.validate(control_args, good=True)
    assert not control.exists(), 'validation created or contacted a manual socket'
    small = profile()
    small['destination']['canvas'] = {'width': 64, 'height': 64}
    small['destination']['frame'] = {'x': 0, 'y': 0, 'width': 64, 'height': 64}
    write(f.profile, json.dumps(small))
    f.validate(good=True) # Camera-only canvases remain valid.
    f.validate(control_args) # Close controls require room for a touch target.
    write(f.profile, json.dumps(profile()))
    for collision in (f.request, f.lease, f.profile, f.display):
        f.validate(f.args() + ['--manual-control-socket', str(collision)])
    write(control, 'off\n'); f.validate(control_args); control.unlink()
    control.symlink_to(f.profile); f.validate(control_args); control.unlink()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(control)); control.chmod(0o600)
        f.validate(control_args, good=True) # No listen/connect is needed.
        control.chmod(0o666); f.validate(control_args)
    control.unlink()
    status = f.base / "camera.status"
    status_args = f.args() + ["--status-file", str(status)]
    f.validate(status_args, good=True)
    assert not status.exists() and not Path(str(status) + ".owner").exists(), "validation created status ownership"
    for collision in (f.request, f.lease, f.profile, f.display):
        f.validate(f.args() + ["--status-file", str(collision)])
    for endpoint in (status, Path(str(status) + ".owner")):
        endpoint.symlink_to(f.profile)
        f.validate(status_args)
        endpoint.unlink()
        os.mkfifo(endpoint, 0o600)
        f.validate(status_args)
        endpoint.unlink()
    # Device identity/capture belongs to runtime activation. Pure preflight
    # must accept a canonical configured node without opening/probing it.
    v4l2 = profile()
    v4l2['source'] = {'kind': 'v4l2', 'device': '/dev/video4294967295', 'pixelFormat': 'yuyv', 'delivery': 'dma-buf'}
    for enabled in (False, True):
        v4l2['enabled'] = enabled
        write(f.profile, json.dumps(v4l2))
        f.validate(good=True)
        assert not f.lease.exists(), 'V4L2 preflight created surface ownership'
    for source in ({'kind': 'v4l2'},
                   {'kind': 'v4l2', 'device': '/dev/./video0', 'pixelFormat': 'yuyv', 'delivery': 'dma-buf'},
                   {'kind': 'v4l2', 'device': '/dev/v4l/by-id/camera', 'pixelFormat': 'yuyv', 'delivery': 'dma-buf'},
                   {'kind': 'v4l2', 'device': '/dev/video0', 'pixelFormat': 'rgb24', 'delivery': 'dma-buf'},
                   {'kind': 'external', 'device': '/dev/video0'},
                   {'kind': 'synthetic-test-pattern', 'delivery': 'cpu-copy'}):
        changed = copy.deepcopy(v4l2); changed['source'] = source
        write(f.profile, json.dumps(changed)); f.validate()
    changed = copy.deepcopy(v4l2); changed['capture']['width'] = 319
    write(f.profile, json.dumps(changed)); f.validate()
    f.reset()
    write(f.profile, json.dumps(profile(enabled=False)))
    f.validate(good=True)  # No GPU connection, even with an unreachable named output.
    write(f.profile, json.dumps(profile(manual_activation=False)))
    f.validate(good=True)  # Valid profile, but effective native activation is disabled.
    f.reset()
    for mutation in (
        lambda p: p.update({"unrecognized": True}),
        lambda p: p.update({"schemaVersion": 2}),
        lambda p: p["destination"].update({"panel": "cluster"}),
        lambda p: p["destination"].update({"rotationDegrees": 45}),
        lambda p: p["destination"]["frame"].update({"x": 600}),
        lambda p: p["capture"].update({"frameRate": 500}),
    ):
        changed = copy.deepcopy(profile())
        mutation(changed)
        write(f.profile, json.dumps(changed))
        f.validate()
    for invalid in ('{"schemaVersion":1,"schemaVersion":1}', " " * 8193, "{}\x00", "{}"):
        write(f.profile, invalid)
        f.validate()
    f.reset()
    for invalid in (
        display_config(f.output).replace("main.output=" + f.output + "\n", ""),
        display_config(f.output).replace("main.wayland=native-test\n", ""),
        display_config(f.output) + "main.width=640\n",
        display_config(f.output) + "unknown=1\n",
    ):
        write(f.display, invalid)
        f.validate()
    f.reset()
    f.validate(f.args() + ["--camera-profile", str(f.profile)])
    f.validate(f.args() + ["--unknown", "test"])
    f.validate(f.args() + ["--validate-only"])
    f.validate(f.args()[:-1])
    relative = f.args()
    relative[2] = "camera.json"
    f.validate(relative)
    traversal = f.args()
    traversal[2] = str(f.base) + "/../" + f.base.name + "/camera.json"
    f.validate(traversal)
    collision = f.args()
    collision[-1] = str(f.request)
    f.validate(collision)
    collision[-1] = str(f.base) + "//request"
    f.validate(collision)
    ancestor = f.base / "ancestor"
    ancestor.mkdir(mode=0o700)
    alias = f.base / "ancestor-alias"
    alias.symlink_to(ancestor, target_is_directory=True)
    private = ancestor / "private"
    private.mkdir(mode=0o700)
    alias_collision = f.args()
    alias_collision[6] = str(private / "request")
    alias_collision[-1] = str(alias / "private" / "request")
    f.validate(alias_collision)
    f.reset()
    for field in ("profile", "display"):
        original = getattr(f, field)
        saved = f.base / "saved"
        original.rename(saved)
        original.symlink_to(saved)
        f.validate()
        original.unlink()
        os.mkfifo(original, 0o600)
        f.validate()  # Must never block on a pipe supplied as configuration.
        original.unlink()
        saved.rename(original)
        original.chmod(0o666)
        f.validate()
        original.chmod(0o600)
    f.base.chmod(0o755)
    f.validate()
    f.base.chmod(0o700)
    for endpoint in (f.request, f.lease):
        endpoint.symlink_to(f.profile)
        f.validate()
        endpoint.unlink()
        os.mkfifo(endpoint, 0o600)
        f.validate()
        endpoint.unlink()
        write(endpoint, "", 0o644)
        f.validate()
        endpoint.unlink()
    write(f.lease, "nonempty")
    f.validate()
    f.lease.unlink()
    write(f.lease, "")
    extra = f.base / "lease-hardlink"
    os.link(f.lease, extra)
    f.validate()
    extra.unlink()
    f.reset()


class Running:
    def __init__(self, f):
        self.f = f
        self.log = f.base / "process.log"
        self.errors = f.base / "process.err"
        self.log_handle = self.log.open("wb")
        self.error_handle = self.errors.open("wb")
        self.status = f.base / "camera.status"
        self.process = subprocess.Popen(f.args() + ["--status-file", str(self.status)],
                                        stdout=self.log_handle, stderr=self.error_handle)

    def phase(self, wanted, timeout=5):
        limit = time.monotonic() + timeout
        while time.monotonic() < limit:
            if self.status.exists():
                text = self.status.read_text()
                assert len(text) <= 128 and text.endswith("\n"), "unbounded/partial status"
                fields = text.split()
                assert len(fields) == 5 and fields[0] == "HCS1", "invalid native status"
                instance, sequence, observed = map(int, fields[1:4])
                assert 0 < instance <= observed <= time.monotonic_ns() and sequence > 0
                if fields[4] == wanted:
                    return
            time.sleep(0.01)
        raise AssertionError("missing native status phase " + wanted)

    def records(self):
        text = self.log.read_text()
        assert len(text) <= 65536, "unbounded lifecycle trace"
        # A process may currently be flushing its last line; parse only whole records.
        return [json.loads(line) for line in text.splitlines(keepends=True) if line.endswith("\n")]

    def wait(self, name, after=0, timeout=5):
        limit = time.monotonic() + timeout
        while time.monotonic() < limit:
            records = self.records()
            for i, record in enumerate(records[after:], after):
                if record["event"] == name:
                    return i, record
            if self.process.poll() is not None:
                raise AssertionError(self.log.read_text() + self.errors.read_text())
            time.sleep(0.01)
        raise AssertionError(f"missing {name}: " + self.log.read_text() + self.errors.read_text())

    def request(self, wanted):
        staged = self.f.base / "request-new"
        write(staged, "on\n" if wanted else "off\n")
        os.replace(staged, self.f.request)

    def terminate(self, requested_signal=signal.SIGTERM):
        self.process.send_signal(requested_signal)
        assert self.process.wait(timeout=5) == 0, self.errors.read_text()
        assert self.records()[-1]["event"] == "camera_stopped"
        self.phase("stopped")
        assert_shared_available(self.f.lease)

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.log_handle.close()
        self.error_handle.close()


def assert_shared_available(path):
    with path.open("rb") as lock:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        fcntl.flock(lock, fcntl.LOCK_UN)


def lifecycle(f):
    assert os.environ.get("HEADUNIT_THEME_GPU_PROBE_WAYLAND") == "1", "use isolated compositor harness"
    # A prior main renderer retains its shared lease until its surface is gone.
    write(f.lease, "")
    with f.lease.open("rb") as main_lease:
        fcntl.flock(main_lease, fcntl.LOCK_SH | fcntl.LOCK_NB)
        running = Running(f)
        try:
            running.wait("camera_idle")
            running.phase("idle")
            running.request(True)
            blocked, _ = running.wait("camera_waiting_for_main")
            running.phase("waiting")
            time.sleep(0.08)
            assert not any(r["event"] == "camera_opened" for r in running.records())
            running.request(False)
            idle, _ = running.wait("camera_idle", blocked + 1)
            running.request(True)
            blocked, _ = running.wait("camera_waiting_for_main", idle + 1)
            fcntl.flock(main_lease, fcntl.LOCK_UN)
            opened, _ = running.wait("camera_opened", blocked + 1)
            time.sleep(1.5)
            running.phase("active")
            running.request(False)
            closed, stats = running.wait("camera_closed", opened + 1)
            assert stats["freshCameraPresentations"] > 0, stats
            assert 0 < stats["maximumPending"] <= 2, stats
            assert_shared_available(f.lease)
            running.phase("idle")
            running.request(True)
            opened, _ = running.wait("camera_opened", closed + 1)
            time.sleep(0.2)
            running.terminate()  # Active surface is destroyed before lease release.
            assert_shared_available(f.lease)
        finally:
            running.close()
    f.reset()
    write(f.profile, json.dumps(profile("external")))
    running = Running(f)
    try:
        running.wait("camera_idle")
        running.request(True)
        opened, _ = running.wait("camera_opened")
        time.sleep(0.5)
        running.request(False)
        _, stats = running.wait("camera_closed", opened + 1)
        assert stats["missingPresentations"] > 0 and stats["freshCameraPresentations"] == 0, stats
        running.terminate(signal.SIGHUP)
    finally:
        running.close()
    for disabled in (profile(enabled=False), profile(manual_activation=False)):
        f.reset()
        write(f.profile, json.dumps(disabled))
        running = Running(f)
        try:
            running.wait("camera_idle")
            running.request(True)
            running.wait("camera_request_disabled")
            time.sleep(0.08)
            assert not any(r["event"] == "camera_opened" for r in running.records())
            running.terminate(signal.SIGINT)
        finally:
            running.close()
    f.reset()
    running = Running(f)
    try:
        running.wait("camera_idle")
        running.request(True)
        opened, _ = running.wait("camera_opened")
        replacement = f.base / "replacement-lease"
        write(replacement, "")
        with f.lease.open("rb") as old_lease:
            os.replace(replacement, f.lease)
            fault, _ = running.wait("camera_fault", opened + 1)
            running.phase("fault")
            assert any(r["event"] == "camera_closed" for r in running.records()[opened + 1:fault])
            fcntl.flock(old_lease, fcntl.LOCK_SH | fcntl.LOCK_NB)
            fcntl.flock(old_lease, fcntl.LOCK_UN)
        time.sleep(0.1)
        assert not any(r["event"] == "camera_opened" for r in running.records()[fault + 1:])
        running.terminate()
    finally:
        running.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--lifecycle", action="store_true")
    parser.add_argument("--output", default="headless")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="headunit-native-camera-") as directory:
        base = Path(directory).resolve()
        base.chmod(0o700)
        f = Fixture(base, str(Path(args.binary).resolve()), args.output)
        validations(f)
        if args.lifecycle:
            lifecycle(f)
    print("native camera CLI" + (" and real Wayland lifecycle" if args.lifecycle else "") + " checks passed")


if __name__ == "__main__":
    main()
