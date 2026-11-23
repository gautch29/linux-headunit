#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Run main/native handoff with an independently presented cluster fixture.

Both compositor process groups and all endpoints belong to this invocation.
No active appliance compositor, service, phone or profile is changed.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def main():
    with tempfile.TemporaryDirectory(prefix="headunit-handoff-") as folder:
        runtime = Path(folder)
        runtime.chmod(0o700)
        env = dict(os.environ, XDG_RUNTIME_DIR=folder)
        names = ("handoff-main", "handoff-cluster")
        processes = []
        logs = []
        result = 1
        try:
            for name in names:
                log = runtime / (name + ".log")
                logs.append(log)
                processes.append(subprocess.Popen([
                    sys.argv[1], "--backend=headless", "--renderer=gl",
                    "--width=640", "--height=360", "--idle-time=0", "--no-config",
                    "--socket=" + name, "--log=" + str(log), "--shell=desktop-shell.so",
                ], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    start_new_session=True))
            deadline = time.monotonic() + 10
            while not all((runtime / name).exists() for name in names):
                if any(p.poll() is not None for p in processes) or time.monotonic() >= deadline:
                    raise RuntimeError("isolated handoff compositors did not start")
                time.sleep(0.02)
            result = subprocess.run([sys.argv[2], *(str(runtime / name) for name in names)],
                                    env=env, timeout=58).returncode
        finally:
            for process in processes:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
            if result:
                for log in logs:
                    if log.exists():
                        print(log.read_text()[-8000:], file=sys.stderr)
        return result


if __name__ == "__main__":
    sys.exit(main())
