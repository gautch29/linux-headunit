# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Python request -> actual C++ file bridge/mailbox -> Python pending/applied state."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from audio_control import MAX_SEQUENCE, read_private, validate_state, monotonic_time_ns
from phone_state import atomic_json

if len(sys.argv) != 2:
    raise SystemExit('expected C++ audio bridge test executable')

with tempfile.TemporaryDirectory(prefix='headunit-audio-interop-') as directory:
    folder = Path(directory).resolve()
    os.chmod(folder, 0o700)
    owner = (os.getuid(), os.getgid())
    atomic_json(folder / 'audio-control.json', {
        'version': 1, 'route': 'e' * 32, 'sequence': str(MAX_SEQUENCE),
        'volumePercent': 0, 'muted': True,
    }, owner)
    subprocess.run([sys.argv[1], '--interop', str(folder)], check=True, timeout=15)
    pending = validate_state(read_private(folder / 'pending.json', owner), monotonic_time_ns())
    confirmed = validate_state(read_private(folder / 'audio-state.json', owner), monotonic_time_ns())
    assert pending['available'] and pending['acceptedSequence'] == str(MAX_SEQUENCE)
    assert pending['appliedSequence'] == '0' and pending['volumePercent'] == 25 and not pending['muted']
    assert pending['requestedVolumePercent'] == 0 and pending['requestedMuted']
    assert confirmed['appliedSequence'] == confirmed['acceptedSequence'] == str(MAX_SEQUENCE)
    assert confirmed['volumePercent'] == 0 and confirmed['muted'] and confirmed['route'] == 'e' * 32
    assert 'output' not in pending
    assert confirmed['output']['acceptedBlocks'] == '42' and confirmed['output']['fresh']
    assert confirmed['output']['underruns'] == '2' and confirmed['output']['starved']
print('Python/C++ private audio wire contract: pending and confirmed uint64-max sequence passed')
