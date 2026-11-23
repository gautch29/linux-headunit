#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Create a private, persistent AirPlay discovery identity for lab tests."""

import os
import subprocess
import sys
import uuid
from pathlib import Path


def main(directory_name):
    directory = Path(directory_name)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(directory, 0o700)
    key = directory / "pairing-ed25519.pem"
    config = directory / "airplay-discovery.conf"
    if not key.exists():
        subprocess.run(["openssl", "genpkey", "-algorithm", "ED25519",
                        "-out", str(key)], check=True,
                       stdout=subprocess.DEVNULL)
        os.chmod(key, 0o600)
    public_der = subprocess.run(
        ["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER"],
        check=True, capture_output=True).stdout
    expected_prefix = bytes.fromhex("302a300506032b6570032100")
    if not public_der.startswith(expected_prefix) or len(public_der) != 44:
        raise RuntimeError("unexpected Ed25519 public-key encoding")
    public_key = public_der[len(expected_prefix):].hex()
    if config.exists():
        fields = dict(line.split("=", 1) for line in config.read_text().splitlines())
        pairing_id = fields["pi"]
        main_display_id = fields.get("main_display_id", str(uuid.uuid4()).upper())
        cluster_display_id = fields.get("cluster_display_id", str(uuid.uuid4()).upper())
    else:
        pairing_id = str(uuid.uuid4()).upper()
        main_display_id = str(uuid.uuid4()).upper()
        cluster_display_id = str(uuid.uuid4()).upper()
    device_id = Path("/sys/class/net/wlan0/address").read_text().strip().upper()
    fields = {
        "name": "Independent Headunit",
        "deviceid": device_id,
        "features": "0x44540380,0x61",
        "flags": "0x4",
        "model": "IndependentHeadunit",
        "protovers": "1.1",
        "srcvers": "950.7.1",
        "pi": pairing_id,
        "pk": public_key,
        "main_display_id": main_display_id,
        "cluster_display_id": cluster_display_id,
    }
    fd = os.open(config, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC,
                 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as output:
        for name, value in fields.items():
            output.write(f"{name}={value}\n")
    os.chmod(config, 0o600)
    print("Private AirPlay test identity ready")


if __name__ == "__main__":
    main(sys.argv[1])
