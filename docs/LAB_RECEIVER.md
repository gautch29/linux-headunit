<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

> For the selected CM5 application release, start with [HANDOFF](HANDOFF.md)
> and [Configuration](CONFIGURATION.md). The standalone diagnostic examples
> below are not instructions to launch a competing receiver on that appliance.

# Independent lab receiver

`tools/run_lab_receiver.py` launches the independent Linux receiver, either
for a bounded diagnostic or as an unbounded supervised session (`seconds: 0`).
It starts `hostapd`, `dnsmasq`, our advertiser and Bluetooth/IP control receiver;
it does not invoke LIVI. The current retained release uses that launch path
under its phone manager. Explicit Ultra options, matching geometry and a verified
installed theme enable the live-tested Ultra route. A successful diagnostic alone
is not whole-session qualification. The AP endpoint is TCP 7000 on `wlan0`.

## Standalone diagnostic ownership

Before a real run, stop anything that owns the Wireless iAP Bluetooth profile,
TCP 7000, `hostapd`, or `dnsmasq`. Keep an Ethernet management connection;
the launcher refuses to turn the host's default-route Wi-Fi interface into an
access point. The Wi-Fi configuration, AirPlay identity, matching Ed25519 key,
and setup secret must be regular private files owned by the same non-root user.
The pairing store and capture root must be owner-only directories for that
user. The private test setup secret has now been accepted by the live iPhone;
do not publish private identity or pairing files.

After handoff, a newly paired phone may wait for the head unit to initiate
CarPlay. With `avahi-utils` installed, use the bounded lab helper:

```sh
python3 tools/request_carplay_connection.py \
  --phone-address PHONE_BLUETOOTH_MAC --identity /path/to/private-airplay-identity.conf
```

It resolves only the explicitly selected phone on `wlan0` in the lab AP subnet,
then requests `/ctrl-int/1/connect` with the receiver's device ID. It sends one
request and checks HTTP acceptance; this is not proof that video starts, nor
an automatic reconnect manager. This missing initiation step was confirmed
with the CM5's fresh pairing and iPhone logs. The request/header format is also
documented in the independent [Magic Box protocol analysis, TCP stream 1](https://etd.ohiolink.edu/acprod/odb_etd/ws/send_file/send?accession=osu1721409456103959&disposition=inline).

First validate the inputs without touching the radio:

```sh
python3 tools/run_lab_receiver.py --dry-run \
  --wifi-config /path/to/private-wifi.conf \
  --identity /path/to/private-airplay-identity.conf \
  --pairing-key /path/to/private-ed25519.pem \
  --setup-secret /path/to/private-setup-secret \
  --pair-store /path/to/private-pair-store \
  --capture-root /path/to/private-captures \
  --bin-dir build
```

When the radio and port are free, run the same command without `--dry-run`
using `sudo`, optionally with `--seconds 90` (bounded range 1–3600; `0`
is reserved for a supervised persistent run). The launcher needs root to set
the AP address and start `hostapd`/`dnsmasq`; it runs the C++ receiver and
advertiser as the identity file's owner. It creates a dated private session
directory and keeps logs and bounded control captures there. After the timer,
Ctrl-C, SIGTERM, or SIGHUP it stops its child processes and removes only the addresses
or interface state it added. Inspect that session's logs for connection
progress. Do not publish them: captures contain pairing data and phone video.

`--preview-sink x11 --x-display :10` enables hardware H.264 decoding into test
windows in the owner's RDP X display. `--preview-sink wayland` selects a
Wayland sink; use `--wayland-display SOCKET` for a non-default compositor.
The launcher validates the owner's runtime directory and Wayland socket.
For a physical touchscreen, use `--preview-sink wayland-touch`. This creates
one fullscreen main surface, embeds the video at its original aspect ratio,
and forwards Wayland touch or left-mouse gestures through our CarPlay HID
channel. Touches in the black bars are ignored; an active drag is clamped to
the video bounds. Additional fingers are ignored, and cancellation, resizing
or queue overload releases the active contact. The navigation stream remains received. This minimal example does not configure
the separate native Standard cluster; the current two-output profile does. See
[Standard/native pages](NATIVE_CLUSTER_PAGES.md). An isolated compositor test
checks embedded H.264 playback. The user confirmed live picture and physical
touch on the CM5's HDMI/USB panel on 26 September 2026.
The X11 preview includes pixel conversion for its test windows. The Wayland
path can negotiate hardware buffers, but zero-copy has not yet been measured.
The default decoder is Pi 4's `v4l2h264dec`. For initial CM5 bring-up, select
`--preview-decoder avdec_h264` explicitly and install GStreamer's libav plugin.
This is CPU H.264 decoding, not the native DMA-buf Ultra path. This Standard preview advertises H.264. Opt-in Ultra negotiation uses the
separate live-tested HEVC/native GPU route; sustained performance qualification
remains open. See [Ultra integration](CARPLAY_ULTRA_GUIDE.md). The direct receiver accepts `HEADUNIT_PREVIEW_DECODER` as well.
Only `v4l2h264dec` and `avdec_h264` are accepted; arbitrary pipeline fragments
are rejected. The selected decoder is logged at receiver startup.

Use `--main-width 1664 --main-height 832` for the wider 2:1 desktop preview.
These values change the display advertised to the phone and its touchscreen
range together; the preview uses the same bounds for mouse input. Defaults
remain 1280 × 720, and the navigation display stays 1280 × 720. Restart the
receiver and reconnect the phone after changing the dimensions. The receiver
also accepts `HEADUNIT_MAIN_WIDTH` / `HEADUNIT_MAIN_HEIGHT` when run directly.
The X11 sink opens at the received video's native size. This is separate from
RDP desktop sizing, which the Mac client negotiates. The earlier Pi 4
1728 × 1084 diagnostic desktop matched the Mac working area; it is not the
current CM5 display setup.

### Historical desktop check — 26 September 2026

Live check: the iPhone supplied a 1664 × 832 main window and
the unchanged navigation window. A desktop capture verified the wider dashboard,
and touch reports were acknowledged. Two initial receiver starts exited early
(the second was confirmed SIGSEGV); the following run under a debugger remained
connected past 2,600 aggregate frames. The crash cause is still unresolved.
Private sessions are `captures/live-clock/session-20260926-165150`, `165302`,
and `165407` (the latter two share the same full session prefix).

Each connection records at most 8 MiB per screen in owner-only
`video-CONNECTION-TYPE.packets` files. Recording stops at a whole-record
boundary once full. Capture failures do not stop decoding or the phone
connection. `tools/video_packets_to_annexb.py INPUT OUTPUT` converts a private
H.264 capture to an owner-only Annex B file. Add `--codec hevc` for Ultra HEVC
captures. The converter supports bounded bare/QuickTime configuration records,
rejects malformed framing and reads at most the capture limit plus one byte.
It creates a new private output and retires its partial output on failure;
existing or replaced evidence is preserved. Eight synthetic checks cover both
codecs, parameter/record truncation, codec mismatch, output privacy and failure
retirement. The 9 October saved V31 cluster capture converts three configurations
and 2,452 frames; FFmpeg decodes all 2,452 at 1920 × 720 without an error.
This validates extraction of that older private capture, not a later wallpaper
action or original presentation timing. `headunit-video-replay INPUT fake`
exercises the C++ converter and hardware decoder and reports submitted and
decoded frame counts; `x11` or `wayland` can display the replay. An optional
fourth argument (0..128) sends that many initial frames without pacing to
exercise decoder startup under a burst. A fifth argument selects the decoder,
for example `headunit-video-replay FILE fake 0 avdec_h264`.
Configure `HEADUNIT_TEST_SOFTWARE_PREVIEW=ON` to run the generated H.264 software
preview test; it requires FFmpeg with libx264 and GStreamer with libav.

The encoded preview queue is bounded to 2 MiB, with 128 buffers allowed for
the first five seconds and 32 afterward. Exceeding a bound stops that preview
explicitly; it never discards a compressed reference frame and carries on with
corrupted deltas. Decoded output can drop old frames. Automatic preview recovery
and keyframe requests remain to be implemented.

The first 32 inbound `/command` bodies per connection are also saved as private
`command-CONNECTION-NUMBER.plist` files, up to 64 KiB total. These are diagnostic
protocol samples and may contain personal phone data. Capture failure leaves
the receiver running; no values from these bodies are printed in routine logs.

The UHVP1 file starts with `UHVP` and byte 1. Each record contains a one-byte
kind (1 configuration, 0 frame), an eight-byte little-endian timestamp, a
four-byte little-endian payload length, then that payload. Timestamps remain
in the phone's domain; replay uses a fixed test pace rather than claiming
original presentation timing.

On 2026-09-26 the complete independent launcher received both phone video
streams, and the user confirmed the main CarPlay window with LIVI stopped.
The user also confirmed that clicking an app opens it through our own HID
reports. The second screen initially received a solid white image from the phone.
After adding an explicit showUI command selecting Maps on the configured cluster
UUID, the user confirmed that the navigation map appeared as well. The request
is sent once per control connection, after RECORD and the first cluster frame;
it is not a repeated focus override. Event response status is logged without
printing its body. Audio, Ultra theme rendering, and production boot/reconnect
behavior are not complete.

CM5 bring-up preparation, 2026-09-26: the explicit software preview decoded
18/18 generated H.264 access units on the Pi 4; the same fixture also decoded
18/18 through its hardware decoder. The replay rejects unsupported decoder
strings. This confirms the software selection and framing, not CM5 performance
or physical touchscreen output.
