# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Generated baseline H.264 fixture; never reads a real phone capture."""
import struct

FFMPEG_ARGUMENTS = [
    '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=s=320x240:r=30',
    '-frames:v', '18', '-c:v', 'libx264', '-preset', 'ultrafast',
    '-tune', 'zerolatency', '-profile:v', 'baseline',
    '-x264-params', 'aud=1:keyint=120:scenecut=0', '-f', 'h264', 'pipe:1',
]
MAXIMUM_ENCODED_BYTES = 2 * 1024 * 1024


def packets(encoded):
    """Match software_preview_test's UHVP configuration and eighteen samples."""
    if not encoded or len(encoded) > MAXIMUM_ENCODED_BYTES:
        raise ValueError('generated H.264 fixture is empty or exceeds its bound')
    starts = []
    offset = 0
    while offset + 3 < len(encoded):
        prefix = 4 if encoded[offset:offset + 4] == b'\0\0\0\1' else 3 if encoded[offset:offset + 3] == b'\0\0\1' else 0
        if prefix:
            starts.append((offset, offset + prefix))
            offset += prefix
        else:
            offset += 1
    if not starts or starts[0][0] != 0:
        raise ValueError('generated H.264 fixture is not an Annex B stream')
    nals = [encoded[payload:starts[index + 1][0] if index + 1 < len(starts) else len(encoded)]
            for index, (_, payload) in enumerate(starts)]
    if any(not nal or nal[0] & 0x80 for nal in nals):
        raise ValueError('generated H.264 fixture contains an empty NAL')
    types = [nal[0] & 31 for nal in nals]
    if types.count(7) != 1 or types.count(8) != 1 or any(kind not in (1, 5, 6, 7, 8, 9) for kind in types):
        raise ValueError('generated H.264 fixture has duplicate parameters or unrelated NAL types')
    sps = next((nal for nal in nals if nal[0] & 31 == 7), None)
    pps = next((nal for nal in nals if nal[0] & 31 == 8), None)
    if sps is None or pps is None or len(sps) < 4 or sps[1] != 66 or len(sps) > 65535 or not 2 <= len(pps) <= 65535:
        raise ValueError('generated H.264 fixture lacks bounded AVC parameters')
    config = bytes([1, *sps[1:4], 255, 225]) + struct.pack('>H', len(sps)) + sps + bytes([1]) + struct.pack('>H', len(pps)) + pps
    frames, kinds = [], []
    for nal in nals:
        kind = nal[0] & 31
        if kind == 9:
            frames.append(bytearray())
            kinds.append([])
        if not frames:
            raise ValueError('generated H.264 fixture lacks access unit delimiters')
        frames[-1].extend(struct.pack('>I', len(nal)) + nal)
        if kind in (1, 5):
            kinds[-1].append(kind)
    if len(frames) != 18 or not kinds[0] or any(kind != 5 for kind in kinds[0]) or any(not frame or any(kind != 1 for kind in frame) for frame in kinds[1:]):
        raise ValueError('generated H.264 fixture must have one initial IDR and seventeen dependent frames')
    output = bytearray(b'UHVP\1')
    output.extend(struct.pack('<BQI', 1, 0, len(config)) + config)
    for index, frame in enumerate(frames):
        output.extend(struct.pack('<BQI', 0, (900 << 32) + (index << 32) // 30, len(frame)) + frame)
    return bytes(output)
