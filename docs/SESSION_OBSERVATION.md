<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Phone session observations

This page preserves early phone/transport observations. Statements about missing
Ultra, maps or audio describe their dated checkpoint, not the current system.
Use [README](../README.md) and [HANDOFF](HANDOFF.md) for newer feature/deployment
status. Do not replay historical launch procedures against the retained release.

## Historical live baseline — 26 September 2026

With LIVI stopped, the independent AP, advertiser and receiver completed
iPhone pairing/verification, encrypted control, MFi `/auth-setup`, `RECORD`,
and main/cluster video setup. The phone's logs confirmed that its two video
encoders started. The first attempt closed the video sockets because our
parser rejected type-5 auxiliary packets; accepting those records without
advancing the frame nonce fixed that failure.

The session beginning 2026-09-26 00:08 local stayed connected for roughly four
minutes until deliberately stopped: 182 authenticated video frames, 81 timing
replies and 125 feedback requests. A subsequent capture independently decoded
109 main frames and 112 cluster frames as H.264 at 1280×720. A late main frame
showed the CarPlay home screen; the cluster remained black after a Maps route.
Both streams also passed the Pi 4 hardware decoder. On the next live test,
the user confirmed the main CarPlay window in the RDP desktop with LIVI stopped.

In the 10:24 local session, the user confirmed that clicking an app in this
window opens it. The receiver logged its first encrypted HID report, continued
past 8,000 video frames, and stayed connected until deliberately stopped.
A late cluster frame independently decoded as solid white; the stream has not
yet shown a navigation map. The cluster preview stopped at its initial
32-buffer admission limit while its raw capture continued. A revised bounded
startup allowance decoded all 619 captured cluster frames on the Pi, including
an unpaced 112-frame startup burst. Live validation of that allowance is pending.

This is an independent interactive main-screen transport/rendering milestone. It does not
establish complete Ultra operation, working audio, the Ultra native theme
renderer, Apple Climate, a visible cluster map or a fresh-pairing/reconnect
qualification. The sections below record earlier staged observations.

## Earlier development checkpoints

On 2026-09-25 the Pi captured a live Bluetooth and Wi-Fi reconnect while LIVI
handled the phone. The user initially saw no image, then confirmed that
CarPlay returned. Application logs and packet metadata showed identification,
MFi authentication, wireless handoff, and both video streams. A working
transport therefore does not by itself prove that images are presented.

## Transport map

- The phone queried BlueZ SDP. A Wireless iAP service used RFCOMM channel 3;
  a separate CarPlay service advertised channel 4. The observed initial iAP2
  data connection used channel 3 after Bluetooth encryption.
- The phone joined the Pi's Wi-Fi access point and used IPv6 link-local
  addresses. A control connection reached the Pi on TCP 7000. Other TCP flows
  carried main and cluster media, with dynamically selected ports. A small
  bidirectional UDP flow was also present.
- The available Wi-Fi packet capture begins with encrypted control frames,
  so it cannot establish the first plaintext request or the phone's pairing
  method. That requires a fresh standalone trace.
- Working-session logs record a 33-byte encrypted-control `/auth-setup`
  request and a 711-byte reply after pair-verify, before `/command` traffic.
  They omit authentication payloads.
- The same session then used a plist-based session `SETUP` with timing and
  event ports in the reply, followed by stream `SETUP` for type `130` (iAP
  data), `111` (cluster video), and `110` (main video). Each stream reply
  supplied a TCP data port. `RECORD` followed stream setup.
- RFCOMM began with the six-byte synchronization marker `FF 55 02 00 EE 10`,
  followed by `FF 5A` link frames. The frames carry a big-endian total length,
  control byte, sequence, acknowledgement, session, and additive checksums.
- Session 10 carries control messages with a `40 40` marker, big-endian length,
  and two-byte identifier. The first observed exchange included `1d00` request,
  `1d01` accessory information, `1d02` acceptance, then MFi IDs `aa00`,
  `aa01`, `aa02`, `aa03`, and `aa05`. Later IDs `4300` and `4301` correlated
  with the theme/asset exchange.

The message interpretations are inferred from timing, application logs, and
public interface descriptions; this is not a complete CarPlay protocol
specification. All raw captures remain in ignored private directories. The
metadata summarizer prints IDs, sizes, and optional parameter layouts without
printing their values.

## Independent receiver milestones

The original C++ link reader handles split and joined RFCOMM frames and checks
lengths and checksums. A reliable-link layer tracks 8-bit sequence numbers,
cumulative acknowledgements, a bounded send window, duplicates, and bounded
retransmissions. The handshake offers the observed non-secret link parameters
and answers the phone's SYN acknowledgement.

In three short standalone phone tests, the receiver registered its own BlueZ
Wireless iAP profile on channel 3 while LIVI was paused, then restored LIVI:

1. BlueZ handed the receiver the phone's RFCOMM file descriptor. The phone
   waited for the accessory's synchronization marker.
2. After sending that marker, the independent receiver established an iAP2
   link and received the phone's `1d00` identification request.
3. With a newly authored accessory identity, the receiver sent `1d01`; the
   phone replied `1d02` (acceptance) and requested the MFi certificate with
   `aa00`.

The observed LIVI `1d01` reply contained 19 top-level parameters. Its
message-list and transport-component layouts informed the independent
identity builder. It uses its own model, name, serial, and capabilities; no
LIVI identity or code was copied. The phone's acceptance confirms this
specific identity shape, but does not prove the later wireless or Ultra stages.

The MFi exchange observed under LIVI used one parameter with ID zero for each
data message: a 607-byte certificate in `aa01`, a 32-byte phone challenge in
`aa02`, and a 64-byte signature in `aa03`. The independent receiver now routes
these messages to its own chip driver. That path builds and passes tests. The
Pi has independently read a 607-byte certificate and produced a 64-byte
signature from a synthetic digest. The phone was unavailable for the latest
standalone test, so phone acceptance of the new MFi path is the next live gate.

An isolated readiness check now runs the independent Wi-Fi AP, a bounded
dual-stack TCP listener on port 7000, and the Wireless iAP Bluetooth profile
at the same time. It also advertises a separate `_airplay._tcp` service on the
AP over IPv4 and IPv6, using a new Ed25519 pairing identity. No phone
participated; it verifies service coexistence and restoration only. The
listener's raw files and pairing key are private and ignored by Git.

No standalone Wi-Fi handoff, CarPlay session, audio, or video has been verified
yet. The diagnostic app remains the working phone receiver between bounded
tests. This repository contains no LIVI source or copied Apple theme assets.

The independent receiver now accepts the observed binary-plist session and
stream `SETUP` shapes on an authenticated encrypted control connection. It
binds a UDP timing socket, a TCP event listener, and one TCP listener per
observed stream type before advertising their ports. A synthetic client on
the Pi completed pairing, encrypted `/info`, all four `SETUP` replies, and
connections to the advertised TCP ports. The receiver exchanges 32-byte UDP clock probes with the control
peer and acknowledges `RECORD` only after stream setup. A simulated peer on
the Pi checked probe and reply handling, while the encrypted control smoke
test reached `RECORD`. Clock offset estimation and codec decoding are still
missing. No phone has tested this stage, and it is not a working CarPlay
session.

The video transport now independently derives one receive key per screen from
the verified pairing secret and the `streamConnectionID` in that screen's
`SETUP`. Its response echoes that ID. Each screen socket parses bounded
128-byte headers, clear codec configurations, and ChaCha20-Poly1305 frames
with the header authenticated. A synthetic normal-pairing session sent both
screens a codec record and an encrypted frame larger than 64 KiB; the Pi
authenticated one frame on each stream. The parser currently discards those
decoded records after counting them. Codec initialization, hardware decoding,
GPU presentation, and live phone compatibility remain unverified.

The type-130 iAP stream now derives its key from the phone's `seed`, checks
each encrypted TCP record, and reassembles the inner accessory transport
packages. Authenticated iAP messages enter a bounded inbox for the receiver
to consume. A synthetic encrypted session sent one such message alongside the
two screen frames. When an established iAP2 engine is attached, the receiver
passes these messages into that same engine and sends its acknowledgements
over the encrypted event channel. A bounded queue holds replies if the event
connection opens after the iAP data connection. Without an attachment, the bounded raw inbox
remains available for diagnostics.

The receiver now also accepts the phone's separate event TCP connection. It
derives event-channel keys from the pairing secret, decrypts bounded RTSP
messages, and sends encrypted replies. A public API wraps an accessory iAP
message in a binary-plist `/command` request and queues it for nonblocking
delivery on that socket. A Pi synthetic session confirmed the phone-side key
direction and an encrypted request/reply round trip. A later integration check
established an iAP2 link over synthetic Bluetooth, switched its sequence state
to an encrypted type-130 Wi-Fi packet, and verified an encrypted event-channel
acknowledgement. The Bluetooth probe now uses the same session-engine class.
The control probe can now optionally own the Wireless iAP BlueZ profile in
its own process and attach that same engine to its IP session. A synthetic
control client also proved that reaching the 64 KiB private capture limit
does not close the live encrypted connection. The combined mode builds on the
Pi. BlueZ refused concurrent registration while LIVI owned the service, as
expected. A four-second exclusive takeover then registered the combined
receiver's Wireless iAP profile and port 7000; afterward LIVI's profile,
display, access point, and port 7000 returned. No phone participated, so
end-to-end independent Wi-Fi handoff remains unverified.

The event sender now handles a 20 KiB synthetic iAP message split across
multiple encrypted records, rather than stopping at its earlier 16 KiB bound.
The receiver records unknown Wi-Fi iAP control IDs and parameter lengths
without dumping their values. A malformed stream closes its own session
instead of terminating the entire receiver.


## 2026-09-26: independent live navigation display

Later live work supersedes the earlier unverified video/hand-off notes above:
Bluetooth/MFi authentication, Wi-Fi handoff, encrypted control, two hardware
decoded video streams, and main-screen touch input now work with LIVI stopped.
The user confirmed the interactive main display and then the cluster map.

The white cluster image was present in the captured encoded video; it was not
introduced by the Pi preview. iOS created a cluster display but did not select
a Maps scene until the receiver sent an encrypted event command:

    {type: "showUI", params: {uuid: configuredClusterUUID,
                            url: "maps:/car/instrumentcluster/map"}}

The URL was advertised by the phone. The command shape was independently
established from the installed Apple CarPlay SDK interface behavior. Our new
encoder is original code. It validates the display UUID and bounds the URL.
A synthetic test decrypts the outgoing request and verifies the dictionary and
response parsing. In the live session the phone returned status 200, created
a Maps scene on its instrument display, and the user confirmed the map visible.
iOS omitted CSeq from its responses, so the current logs do not correlate
concurrent responses to individual requests. No native Ultra theme or Climate
readiness is implied by the navigation display success.

Mac: 21 tests passed. Raspberry Pi: 29 tests passed. The live preview did not
report a decoder/output failure during this verification. The lab launcher now
allows up to one hour per session; it is still a bounded diagnostic, not a
production service.

## 2026-09-26: independent phone clock

The new bounded timing estimator qualifies the phone clock and maps authenticated
timestamps from both main and navigation video into the reused MediaClock.
An initial live test exposed idle refresh occurring only on video arrival; the
fixed receiver refreshes during regular event-loop turns. The phone reconnected
automatically; an initial timestamp from each stream mapped successfully, and
the clock stayed synchronized while more than 4,000 frames arrived, with no
clock-unavailable, clock-bridge fault or preview-failure report
during the observation. Details, uncertainty assumptions and the private
evidence location are in PHONE_CLOCK.md. Actual Ultra presentation is still
pending; the existing preview is not scheduled using this clock.
