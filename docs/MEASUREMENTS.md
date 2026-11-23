<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Hardware measurements and targets

## Current CM5 bench

The current test target is a wireless CM5, 4 GB RAM and 16 GB eMMC, running
Raspberry Pi OS Lite. Two physical HDMI outputs run under labwc/Wayland: main
1280 × 720 and cluster 1920 × 1080. Both signed Ultra logical canvases are
1920 × 720 and fitted locally. Host/build details belong in [HANDOFF](HANDOFF.md).

Use [Ultra performance](ULTRA_PERFORMANCE.md) for dated CM5 timings and their
input/receipt boundaries. Local animated scene fps, distinct incoming phone
frames, GPU duration and physical presentation are different measurements.
Some DigiRing scenes approach 60 fps; sustained distinct-source 60 fps on both
full-resolution panels remains unqualified. The user currently accepts 30 fps
for feature work. The goal remains a 16.7 ms budget at 60 fps.

A warm reboot showed the backup, and reconnect returned both phone views. Cold
power-on to useful cluster/CarPlay, retained V3 physical workflows and rollback
still need measured qualification. See [startup](APPLIANCE_STARTUP.md) and
[goal](GOAL.md). Verify power/throttling and actual connected output modes for
each run; do not compare an undervolted run to a qualified power configuration.

CM5 hardware capabilities are described in the official
[CM5 datasheet](https://datasheets.raspberrypi.com/cm5/cm5-datasheet.pdf).
This repository's current Standard preview uses CPU H.264 decoding; Ultra HEVC
has a separately tested native hardware path. Neither implies all processing
can move to the GPU. See [native video](NATIVE_VIDEO.md).

## Historical Pi 4 baseline

The initial development host was **Raspberry Pi 4 Model B Rev 1.4**. Its diagnostic
OS reached multi-user target in 21.085 seconds from kernel start (2.482 seconds
kernel, 18.603 userspace), including 7.444 seconds waiting for networking and an
RDP service. This is an earlier installation baseline, not current CM5 boot time.

An isolated Pi 4 research probe with 12 HEVC frames at 1280 × 720 measured 66
translucent overlays at 89.4 ms/frame when drawn separately and 8.36 ms/frame
when geometry was batched in order. Pixel comparison had maximum channel error
5. A two-overlay case changed from 9.60 to 8.09 ms/frame, insufficient to claim a
robust gain. Measurements included synchronous GPU completion, excluded scanout
and do not predict CM5/phone performance. Our own renderer work was later reused;
original LIVI code remains excluded. See [provenance](PROVENANCE.md).

On 26 September the independent receiver requested
`maps:/car/instrumentcluster/map` and the user confirmed Maps in the previously
white navigation window. A bounded one-hour session recorded 50,378 video frames,
667 feedback requests and nine touch reports. This was a main/navigation video
milestone, not full Ultra acceptance. [Session history](SESSION_OBSERVATION.md)
records the early transport stages; [README](../README.md) describes later work.
