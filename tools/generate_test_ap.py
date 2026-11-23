#!/usr/bin/env python3
# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Generate private hostapd/dnsmasq test configuration from our own settings."""

import os
import sys
from pathlib import Path


def main(source, runtime, country_code="FR"):
    if len(country_code) != 2 or not all("A" <= c <= "Z" for c in country_code):
        raise ValueError("Wi-Fi country must be a two-letter uppercase country code")
    fields = {}
    for line in Path(source).read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in fields:
            raise ValueError("invalid or repeated Wi-Fi config field")
        fields[key] = value
    if set(fields) != {"ssid", "passphrase", "channel"}:
        raise ValueError("Wi-Fi config needs ssid, passphrase, channel")
    ssid = fields["ssid"]
    passphrase = fields["passphrase"]
    channel = int(fields["channel"])
    if not (1 <= len(ssid) <= 32 and 8 <= len(passphrase) <= 63 and
            channel == 36 and
            all(32 <= ord(char) < 127 for char in ssid + passphrase)):
        raise ValueError("unsupported test Wi-Fi settings")
    directory = Path(runtime)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(directory, 0o700)
    hostapd = ("interface=wlan0\n"
               "driver=nl80211\n"
               f"ssid={ssid}\n"
               f"country_code={country_code}\n"
               "ieee80211d=1\n"
               "hw_mode=a\n"
               f"channel={channel}\n"
               "ieee80211n=1\n"
               "ieee80211ac=1\n"
               "wmm_enabled=1\n"
               "wpa=2\n"
               "wpa_key_mgmt=WPA-PSK\n"
               "rsn_pairwise=CCMP\n"
               f"wpa_passphrase={passphrase}\n")
    dnsmasq = ("interface=wlan0\n"
               "bind-interfaces\n"
               "port=0\n"
               "dhcp-range=10.10.0.10,10.10.0.50,255.255.255.0,12h\n"
               "dhcp-option=3,10.10.0.1\n")
    for filename, content in (("hostapd.conf", hostapd),
                              ("dnsmasq.conf", dnsmasq)):
        path = directory / filename
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC,
                     0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            output.write(content)
        os.chmod(path, 0o600)
    print("Private standalone AP configuration generated")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
