#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Compare our codec with macOS CoreUtils on synthetic values only.

The platform executable is built separately from opack_platform_probe.mm.
No Apple library is linked into, or required by, the Linux receiver.
"""
import argparse
from pathlib import Path
import plistlib
import random
import subprocess
import tempfile


def same(left, right):
    if type(left) is not type(right):
        return False
    if isinstance(left, list):
        return len(left) == len(right) and all(map(lambda pair: same(*pair), zip(left, right)))
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(same(left[key], right[key]) for key in left)
    return left == right


def cases():
    numbers = [-2**63, -2**31-1, -32769, -129, -128, -2, -1, 0, 1, 39, 40,
               127, 128, 255, 256, 32767, 32768, 65535, 65536, 2**31-1, 2**31, 2**63-1]
    values = [True, False, *numbers, 0.0, -0.0, 1.0, 1.5, -123.25, "",
              "driver", "température", "☀️", b"", b"x", b"xy"]
    yield from values
    for size in (1, 2, 32, 33, 255, 256, 65535, 65536):
        for value in ("a" * size, bytes(i % 251 for i in range(size))):
            yield value
            yield [value, value, "repeat", "repeat"]
    yield {"displayUUID": "00000000-0000-4000-8000-000000000001", "data": b"bplist00"}
    yield {"messageType": 2, "sessionID": "synthetic-session", "chunkIndex": 0,
           "data": bytes(204800), "checksum": "0" * 64}
    yield [str(i) for i in range(70)] * 2  # Wide reference indices.
    yield {str(i): [i, str(i), b"xy"] for i in range(40)}  # Terminated dictionaries.
    randomizer = random.Random(260926)

    def make(depth):
        if depth == 0 or randomizer.randrange(3) == 0:
            return randomizer.choice(values)
        if randomizer.randrange(2):
            return [make(depth - 1) for _ in range(randomizer.randrange(9))]
        return {f"field{i}": make(depth - 1) for i in range(randomizer.randrange(8))}

    for _ in range(160):
        yield make(4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform-probe", required=True, type=Path)
    parser.add_argument("--reencoder", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="headunit-opack-synthetic-") as directory:
        directory = Path(directory)
        count = 0
        for count, value in enumerate(cases(), 1):
            original = directory / f"{count}.plist"
            platform = directory / f"{count}.opack"
            ours = directory / f"{count}.ours"
            decoded = directory / f"{count}.decoded"
            original.write_bytes(plistlib.dumps(value, fmt=plistlib.FMT_BINARY))
            for command in (
                [str(args.platform_probe.resolve()), "encode", str(original), str(platform)],
                [str(args.reencoder.resolve()), str(platform), str(ours)],
                [str(args.platform_probe.resolve()), "decode", str(ours), str(decoded)],
            ):
                result = subprocess.run(command, capture_output=True)
                if result.returncode:
                    raise RuntimeError(f"synthetic case {count}, stage {command[0]} failed")
            if not same(value, plistlib.loads(decoded.read_bytes())):
                raise RuntimeError(f"synthetic case {count} changed value or type")
        print(f"Apple -> independent -> Apple: {count} synthetic cases passed")


if __name__ == "__main__":
    main()
