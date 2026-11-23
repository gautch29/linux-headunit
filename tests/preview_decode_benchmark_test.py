#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Verify paced benchmark accounting and malformed-input rejection offline."""
import pathlib
import struct
import subprocess
import sys
import tempfile

benchmark, ffmpeg = sys.argv[1:3]
encoded = subprocess.run([
    ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=s=320x240:r=30',
    '-frames:v', '18', '-c:v', 'libx264', '-preset', 'ultrafast',
    '-tune', 'zerolatency', '-profile:v', 'baseline',
    '-x264-params', 'aud=1:keyint=6', '-f', 'h264', 'pipe:1',
], check=True, capture_output=True, timeout=15).stdout
starts = []
i = 0
while i + 3 < len(encoded):
    prefix = (4 if encoded[i:i+4] == b'\x00\x00\x00\x01' else
              3 if encoded[i:i+3] == b'\x00\x00\x01' else 0)
    if prefix:
        starts.append((i, i + prefix))
        i += prefix
    else:
        i += 1
nals = [encoded[payload:starts[j+1][0] if j+1 < len(starts) else len(encoded)]
        for j, (_, payload) in enumerate(starts)]
sps = next(n for n in nals if n[0] & 31 == 7)
pps = next(n for n in nals if n[0] & 31 == 8)
config = (bytes([1, *sps[1:4], 255, 225]) + struct.pack('>H', len(sps)) + sps +
          bytes([1]) + struct.pack('>H', len(pps)) + pps)
frames = []
for nal in nals:
    if nal[0] & 31 == 9:
        frames.append(bytearray())
    assert frames
    frames[-1].extend(struct.pack('>I', len(nal)) + nal)
assert len(frames) == 18


def record(kind, timestamp, payload):
    return struct.pack('<BQI', kind, timestamp, len(payload)) + payload


header = b'UHVP\x01' + record(1, 0, config)
good = header + b''.join(record(0, (900 << 32) + (j << 32)//30, frame)
                         for j, frame in enumerate(frames))
with tempfile.TemporaryDirectory(prefix='headunit-decode-benchmark-') as directory:
    path = pathlib.Path(directory) / 'synthetic.packets'
    path.write_bytes(good)
    for mode in ['auto', 'slice', 'single']:
        result = subprocess.run([benchmark, str(path), mode], capture_output=True,
                                text=True, timeout=10)
        assert result.returncode == 0, (result.stdout, result.stderr)
        fields = dict(field.split('=', 1) for field in result.stdout.split())
        assert fields['submitted'] == fields['decoded'] == fields['all_n'] == '18'
        assert fields['missing'] == fields['unmatched'] == fields['duplicate'] == '0'
        assert float(fields['elapsed_s']) >= 17/30
        assert float(fields['all_p50_ms']) >= 0
        print(f'{mode}: paced 18 frames, exact output accounting')
    invalid = [b'bad', good[:-1], header + record(0, 0, frames[0]) * 2,
               header + record(0, 0, frames[0]) + record(0, 31 << 32, frames[1])]
    for data in invalid:
        path.write_bytes(data)
        result = subprocess.run([benchmark, str(path), 'single'], capture_output=True,
                                text=True, timeout=3)
        assert result.returncode == 1 and result.stderr, (result.stdout, result.stderr)
    print('Rejected bad headers, truncated records, duplicate timestamps and excessive duration')
