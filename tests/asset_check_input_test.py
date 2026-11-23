#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Synthetic inputs must reject without waiting for special-file writers."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix="headunit-asset-input-test-") as temporary:
    directory = Path(temporary)
    regular = directory / "regular"
    regular.write_bytes(b"synthetic malformed signing material")
    inputs = [str(regular)] * 4

    def check(paths, expected):
        result = subprocess.run([str(binary), *paths], capture_output=True, timeout=2)
        assert result.returncode == expected, (result.returncode, result.stderr)

    # Complete regular reads reach the verifier and report malformed signing material.
    check(inputs, 2)
    check([*inputs, "--allow-expired-leaf"], 2)
    limits = [64 * 1024 * 1024, 4096, 32768, 32768]
    for index, maximum in enumerate(limits):
        for kind in ("fifo", "symlink", "directory", "empty", "oversize"):
            invalid = directory / f"input-{index}-{kind}"
            if kind == "fifo":
                os.mkfifo(invalid, 0o600)
            elif kind == "symlink":
                invalid.symlink_to(regular)
            elif kind == "directory":
                invalid.mkdir()
            elif kind == "empty":
                invalid.touch()
            else:
                with invalid.open("wb") as output:
                    output.truncate(maximum + 1)
            paths = list(inputs)
            paths[index] = str(invalid)
            check(paths, 1)
    print("All four input roles reject FIFOs, symlinks, directories, empty and oversized files.")
