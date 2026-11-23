#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Private software layer-shell graph; no CM5, GPU or physical output."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="headunit-return-layer-") as folder:
    root = Path(folder)
    runtime, config = root / "runtime", root / "config"
    runtime.mkdir(mode=0o700)
    config.mkdir(mode=0o700)
    (config / "rc.xml").write_text("<labwc_config><core><gap>0</gap></core></labwc_config>")
    for name in ("autostart", "shutdown"):
        (config / name).write_text("#!/bin/sh\nexit 0\n")
    (config / "environment").write_text("LABWC_UPDATE_ACTIVATION_ENV=0\n")
    env = dict(os.environ)
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DBUS_SESSION_BUS_ADDRESS",
                "LABWC_PID", "XAUTHORITY", "WLR_DRM_DEVICES", "WAYLAND_DEBUG"):
        env.pop(key, None)
    env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config),
               WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1", WLR_RENDERER="pixman",
               LIBSEAT_BACKEND="noop", LABWC_UPDATE_ACTIVATION_ENV="0")
    with (root / "labwc.log").open("w") as log:
        compositor = subprocess.Popen([sys.argv[1], "-C", str(config)], env=env,
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        status = 1
        try:
            deadline = time.monotonic() + 10
            socket_path = None
            while socket_path is None:
                socket_path = next((p for p in runtime.glob("wayland-*")
                                    if not p.name.endswith(".lock") and p.is_socket()), None)
                if socket_path is not None:
                    break
                if compositor.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("private layer compositor did not start")
                time.sleep(.02)
            env["WAYLAND_DISPLAY"] = socket_path.name
            env["HEADUNIT_THEME_TEST_WESTON_PID"] = str(compositor.pid)
            status = subprocess.run([sys.argv[2], "--layer"], env=env, timeout=15).returncode
        finally:
            if compositor.poll() is None:
                os.killpg(compositor.pid, signal.SIGCONT)
                os.killpg(compositor.pid, signal.SIGTERM)
                try:
                    compositor.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(compositor.pid, signal.SIGKILL)
                    compositor.wait()
        log.flush()
        if status:
            print((root / "labwc.log").read_text()[-6000:], file=sys.stderr)
    sys.exit(status)
