# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Actual publisher/reader wire and expiry; readiness is explicit fixture input."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from carplay_readiness import CarplayReadiness, MAXIMUM_AGE_NS


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary).resolve()
    nonce = 'a' * 32
    reader = CarplayReadiness(root, (os.getuid(), os.getgid()))
    reader.reset(nonce)
    child = subprocess.Popen([sys.argv[1], '--interop', str(root), nonce],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert child.stdout.readline().strip() == 'initial'
        assert not reader.snapshot(child.poll() is None)['returnReady']

        def command(value, state, ready):
            child.stdin.write(value + '\n'); child.stdin.flush()
            assert child.stdout.readline().strip() == value
            assert reader.snapshot(child.poll() is None) == {'state': state, 'returnReady': ready}

        command('r', 'ready', True)
        command('h', 'home', True)
        command('f', 'fault', False)
        command('n', 'starting', False)
        command('r', 'ready', True)
        # Process ownership clears eligibility immediately, even if its last
        # real publication remains within its freshness interval.
        child.kill(); child.wait(timeout=2)
        assert not reader.snapshot(False)['returnReady']
        assert reader.snapshot(True)['returnReady']  # Explicit wire-only oracle.
        time.sleep(MAXIMUM_AGE_NS / 1_000_000_000 + .05)
        assert not reader.snapshot(True)['returnReady']  # Date did not refresh.
    finally:
        if child.poll() is None:
            child.kill(); child.wait(timeout=2)
        child.stdin.close(); child.stdout.close()
print('Actual C++/Python readiness wire, transitions, launch ownership and expiry pass.')
