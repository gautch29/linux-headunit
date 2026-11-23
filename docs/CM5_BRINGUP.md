<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# CM5 bring-up and development

Updated **4 October 2026**. The bench is a wireless Compute Module 5 with 4 GB
RAM, 16 GB eMMC and the official IO board. Raspberry Pi OS Lite 64-bit Trixie
is sufficient; a desktop/RDP server is not required. [HANDOFF](HANDOFF.md) owns
current host, retained-release paths and qualification artifacts.

## Current appliance

Five supervised services make up the standard bench configuration. labwc/
Wayland owns two physical outputs, kanshi applies their modes, a separate native
cluster stays behind Ultra, and the manager owns the selected phone receiver
and optional native Home. See [startup](APPLIANCE_STARTUP.md),
[releases](APPLICATION_RELEASES.md) and [manager boot](PHONE_MANAGER_BOOT.md).

| Output | Physical mode | Input/role |
| --- | --- | --- |
| HDMI-A-1 | 1280 × 720 at 60 Hz | Main HDMI panel; USB touchscreen |
| HDMI-A-2 | 1920 × 1080 at 60 Hz | Cluster monitor; no touch |

The signed Ultra canvases are 1920 × 720 and fit without stretching. The main
fit is 1280 × 480, with matching touch mapping. Standard main uses 1280 × 720.
The panel's earlier EDID preferred 1024 × 768, which did not match its visible
proportions. The user subsequently confirmed the configured widescreen picture
and touch. Physical millimetre values remain estimates pending measurement.
[Configuration](CONFIGURATION.md) explains the different geometry owners.

The user confirmed live main/touch, both Ultra views with Maps, Standard native
pages, backup after reboot and automatic fallback after normal disconnect.
Those checks do not establish the latest retained V3 physical Home workflow,
fault failback, cold-power startup or new camera-state bridge acceptance.

## Install and access

Install 64-bit Raspberry Pi OS Lite using the official
[Compute Module guide](https://www.raspberrypi.com/documentation/computers/compute-module.html).
Set up a local service account and key-based SSH separately from the source.
Use Ethernet for management while developing the Wi-Fi access point.

## Authentication hardware

The official IO board has the standard 40-pin header. Preserve the module's
known working supply voltage when transferring the existing authentication board:

| Signal | Physical header pin |
| --- | --- |
| I²C1 SDA (GPIO2) | 3 |
| I²C1 SCL (GPIO3) | 5 |
| Ground | 6, or another ground pin |
| Supply | Same verified rail as the existing board: 1/17 for 3.3 V, 2/4 for 5 V |

These are physical pin positions, not GPIO numbers. Remove power before moving
leads and identify pin 1 directly. See the official
[IO-board datasheet](https://pip-assets.raspberrypi.com/categories/1097-raspberry-pi-compute-module-5-io-board/documents/RP-008182-DS/cm5io-datasheet).
The CM5 driver returned a certificate/signature for a synthetic digest, and
subsequent real iPhone sessions completed authentication. Those private bytes
are not logged or published. [MFi driver](MFI_COPROCESSOR.md).

## Development dependencies

A typical Lite development host needs:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends \
  build-essential cmake ninja-build pkg-config git \
  libssl-dev libplist-dev libarchive-dev libcairo2-dev libpng-dev libglib2.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libwayland-dev wayland-protocols libgbm-dev libegl-dev libgles-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-gl \
  labwc kanshi libinput-tools mesa-utils ffmpeg nodejs i2c-tools v4l-utils \
  python3-gi gir1.2-gtk-4.0 avahi-utils \
  hostapd dnsmasq-base rfkill iw
```

The retained appliance uses qualified compositor/GPU dependencies; stock package
versions need not reproduce them. Consult the selected manifest and handoff
instead of replacing the running stack. Development/test tools do not all belong
in a final image. Keep a single networking owner for the CarPlay interface.

For a **separate development checkout**, the basic native-video fixture build is:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DHEADUNIT_TEST_SOFTWARE_PREVIEW=ON \
  -DHEADUNIT_BUILD_THEME_VIDEO=ON -DHEADUNIT_TEST_NATIVE_VIDEO=ON
cmake --build build -j3
ctest --test-dir build --output-on-failure
```

GPU composition additionally needs the pinned external Skia build and explicit
options in [GPU rendering](GPU_RENDERING.md). [Native video](NATIVE_VIDEO.md)
describes hardware HEVC qualification; Standard H.264 currently uses
`avdec_h264` on CM5. Keep decoder testing distinct from whole-session acceptance.

The latest GCC 14.2 integration build has documented legacy warning exceptions;
see [HANDOFF](HANDOFF.md). Check power and disk/tmpfs capacity before benchmarking
or building. An earlier power change left a zero-byte object and required a
clean rebuild; resuming the build alone did not repair it. Private candidate
builds do not update sealed binaries or services.

## Remaining hardware qualification

- Retained V3 physical Home/Back/Disconnect, cold-power boot and rollback.
- Sustained full-resolution performance and physical fault fallback/failback.
- Audible audio device, microphone/calls and real camera capture/import.
- Actual car acquisition, calibrated inputs, HVAC and actuator executors.
- Connected Ultra camera-state acceptance, Remote UI controls/focus and complete
  session-preserving transitions.
