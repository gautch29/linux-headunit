#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Run synthetic GPU tests against an isolated, bounded headless compositor."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def main():
    with tempfile.TemporaryDirectory(prefix="headunit-wayland-") as directory:
        wide = "--wide" in sys.argv[2:]
        capture_output = "--capture-output" in sys.argv[2:]
        child_args = [arg for arg in sys.argv[2:] if arg not in ("--wide", "--capture-output")]
        os.chmod(directory, 0o700)
        env = dict(os.environ, XDG_RUNTIME_DIR=directory, WAYLAND_DISPLAY="native-test")
        if capture_output:
            env["HEADUNIT_TEST_OUTPUT_CAPTURE"] = "1"
        log = Path(directory) / "weston.log"
        renderer = os.environ.get("HEADUNIT_THEME_TEST_WESTON_RENDERER", "gl")
        if renderer not in ("gl", "pixman"):
            raise ValueError("invalid isolated compositor renderer")
        shell = os.environ.get("HEADUNIT_THEME_TEST_WESTON_SHELL", "desktop-shell.so")
        if shell not in ("desktop-shell.so", "kiosk-shell.so"):
            raise ValueError("invalid isolated compositor shell")
        compositor = subprocess.Popen([
            sys.argv[1], "--backend=headless", f"--renderer={renderer}",
            f"--width={1920 if wide else 1280}",
            "--height=720", "--idle-time=0", "--no-config", "--socket=native-test",
            f"--log={log}", f"--shell={shell}",
            *(["--debug"] if capture_output else []),
        ], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        status = 1
        try:
            deadline = time.monotonic() + 10
            while not (Path(directory) / "native-test").exists():
                if compositor.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("isolated Wayland compositor did not start")
                time.sleep(0.02)
            env["HEADUNIT_THEME_GPU_PROBE_WAYLAND"] = "1"
            env["HEADUNIT_THEME_TEST_WESTON_PID"] = str(compositor.pid)
            status = subprocess.run(child_args, env=env, timeout=75).returncode
        finally:
            # Only the new process group created above; never another compositor.
            if compositor.poll() is None:
                os.killpg(compositor.pid, signal.SIGTERM)
                try:
                    compositor.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(compositor.pid, signal.SIGKILL)
                    compositor.wait()
            if status and log.exists():
                print(log.read_text()[-12000:], file=sys.stderr)
        return status


if __name__ == "__main__":
    sys.exit(main())
