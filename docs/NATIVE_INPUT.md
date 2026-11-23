<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Native phone video in the Ultra display worker

`NativeInputQueue` is the independent receiver's bounded ingress for already
authenticated `AirplayVideoPacket`s. It contains no Lua, GPU, decoder, socket or
key resources. The transport can retain this endpoint after the display worker
exits; the endpoint then rejects further packets. Exactly one `NativeInput`
may consume a queue. A screen transport can retire and reopen with a fresh
session-reserved crypto ID while retaining that queue, Runtime stream and worker.
The display thread discards retired ingress and replaces its decoder/configuration;
full endpoint closure or worker destruction remains terminal.

The opt-in installed Ultra receiver now uses this path for both phone video
streams. Live main/cluster rendering and touch are recorded separately from
the synthetic component tests below; see [the Ultra guide](CARPLAY_ULTRA_GUIDE.md)
and [current handoff](HANDOFF.md). This API does not by itself negotiate Ultra
or prove complete phone/camera support.

The receiver must retain and retry packets when `enqueue()` returns false,
applying a bounded socket-stall policy. Dropping compressed reference frames is
not allowed. The queue bounds are 64 packets, 8 MiB including 128 bytes per
packet of framing allowance, and 2 MiB per payload. Counts include the consumer's
held packet while its decoder is backpressured. Invalid/missing source time,
codec changes within a transport generation and decoder faults do not select a
CPU fallback. A fresh SETUP generation must supply its own codec configuration
and decodable video; the receiver never resumes retired compressed references.

Before evaluating an asset entry point, the worker registers a Runtime video
input and constructs `NativeInput` with that stream and the queue. Its display
options retain the input through `DisplayVideoInput`. The worker alone owns the
decoder, stream staging and GPU interaction. The separate endpoint avoids final
decoder destruction on a transport producer thread.

`GpuDisplay` pumps decoding in bounded turns. GStreamer supplies native DMA-buf
leases directly to the Runtime stream; this path does not map decoded pixels to
the CPU. Each decoder bounds native leases to the stream queue capacity plus
its current frame. Late and repeated source timestamps are decoded to preserve codec references, but do not advance
the scene. Clock suspension keeps decoding references and discards output.

A video frame is staged only when due in the synchronized phone clock and when
an active video layer in the submitted scene references its stream. Traversal
includes masks and excludes hidden, transparent or singularly transformed
subtrees. Only an accepted actual Wayland scene receipt completes the staged
source frame. Bootstrap, discarded and stale receipts cannot advance source
PTS. Clock loss, long owner pauses, delayed preview feedback and terminal errors
release staged/current/queued video. Script flushes invalidate outstanding
source tokens before their receipt can advance them.

## Scope and verification

The `theme-native-input-wayland` Pi test uses FFmpeg-generated HEVC access units,
the real Pi hardware decoder, native buffers, a script-created DPVideoLayer and
actual Wayland presentation receipts. The fixture explicitly retimes its test
access units against a **synthetic network clock**; production inputs are never
retimed. It checks queue count/byte bounds, a concurrent producer, endpoint
closure after worker destruction, future-frame gating, hidden-layer gating,
repeated timestamps, decoding during clock loss, relock, and disconnect output
withdrawal. The companion `theme-display` tests cover stale compositor receipts
and preview recovery.

This standalone fixture does not install an asset or verify a live Ultra session.
The current `ReceiverVideo`/`InstalledUi` integration routes negotiated screen
IDs to these queues, copies qualified clock snapshots and supervises
backpressure. A replacement video SETUP uses fresh keys/configuration within
the same control/UI session; replacing a consumed UI worker still requires a
new overall session. Full signed
layouts have separate live/performance records. These generated-source tests
are not evidence of 60 fps or sustained phone reliability.

### Decoder timestamp correction

The native test exposed a parser timestamp substitution when the phone repeated
an access-unit timestamp **after** its earlier decoded frame was consumed. The
old map avoided collisions only among pending frames. The decoder now keeps
pipeline PTS keys strictly increasing throughout its generation, and separately
retains the exact authenticated source timestamp. A repeated source time remains
a repeated source time for scheduling and is discarded there; it is not relabeled
as a fresh phone image. The native test exercises this sequential-repeat case,
and the existing decoder suite checks exact timestamps and simultaneous repeats.

### Validation on 2026-09-26

The complete Linux build and all 60 Pi tests passed, including real hardware
decoding, GPU pixel comparisons, actual Wayland receipts and the new native
input test (12 presented frames, plus repeated/clock-suspended decoded units).
All 41 portable Mac tests passed. These are component tests, not a new live
iPhone Ultra result. No new sanitizer run was performed for this slice.

## Receiver-side video backpressure

`AirplaySessionSetup::set_video_consumer` now supplies a reliable owner-thread
handoff for the native-input router. Install it before stream SETUP. The
consumer may move a packet only when returning true; false retains that exact
packet and pauses reads on its screen socket. Other screens, UI, input and timing
continue to be serviced. Callback work must itself be bounded and nonblocking.
The existing preview/capture observer runs once at ingress, not again on retries.

The transport retains at most 64 packets and 8 MiB + 16 KiB per screen (in addition
to the parser's existing bound). This accommodates one completed maximum-sized
packet plus the small records from a single 4 KiB read. Delivery attempts at most
32 packets per batch. Five seconds with no consumer progress closes that screen
and releases retained data. Consumer/parser errors also close that screen. EOF
and faults remain observable through `video_status`; accepted video listeners
are closed in this mode, preventing reuse of the same parser/crypto generation.
The router now retires only that screen generation. A fresh-key SETUP may
replace it within the same control session; retired IDs cannot be revived.

On 2026-09-26 the CM5 encrypted-loopback test passed: 300 ordered frames retained
source timestamps and bytes across a paused consumer; no socket reads occurred
while paused, the second screen continued, observer records were not duplicated,
and consumer fault, EOF and the actual five-second expiry cleaned up correctly.
The existing session and UI supervisor tests passed alongside it. This isolated transport test is not a live GPU ingress result.
The subsequent `ReceiverVideo` integration connects these callbacks to the
installed factory; see [UI integration](UI_SESSION.md) for its encrypted
two-source GPU test and [the Ultra guide](CARPLAY_ULTRA_GUIDE.md) for the
separately tested live route and remaining acceptance limits.

## Per-screen retirement and replacement — 9 October 2026

`VideoStatus` carries the reserved connection ID. `ReceiverVideo` follows that
ID independently for main and cluster; old closed tombstones cannot retire a
replacement. EOF, selective teardown, parser failure or transport backpressure
expiry clears the affected compressed ingress without closing the healthy
screen or shared UI owner. Queue closure, decoder failure and UI failure still
retire the shared owner. Each queue retains at most twelve used IDs, matching
the control session's allocation budget.

The consumer observes a generation change on the display thread, releases old
pending packets and decoded/prepared/current frames, and rebuilds the decoder.
The Runtime stream stays open and retains increasing token/source-PTS floors.
Late receipts from the retired generation cannot mark a frame presented, even
before the next decoder turn. Post-composition withdrawal clears that input's
receipt ownership in older pending scenes, then redraws without the withdrawn
source while preserving the healthy input's receipt. Deferred GPU imports remain
owned by the compositor's bounded fences. Requested `forceKeyFrame` refreshes
are tracked per video ID; a transport acknowledgement is not a decoded keyframe.

The isolated CM5 passes fourteen affected suites: encrypted two-screen hardware
video retirement/reopen with both synchronous and two-pending deferred GPU
completion, three existing native-input Wayland fixtures, queue/receipt state
including concurrent replacements, transport SETUP/lifecycle/backpressure, UI
session, three display fixtures and camera expiry redraw. The replacement
fixtures preserve RECORD, withdraw cluster on EOF while main presents, retire
main during composition while cluster presents, and reopen both with fresh
IDs/configuration and generated HEVC random-access frames. Source timing is an
explicit synthetic network-clock fixture; decoder DMA-bufs and Wayland receipts
are real. This is one compositor surface with two video layers, not a signed
phone scene or two physical panels. Real-phone restart ordering, refresh replies
and sustained video/UI/audio continuity remain unqualified.

ASan+UBSan also passes the native-input implementation and its state/concurrent
fixture with halt-on-error and leak detection; linked decoder/graphics/protocol
libraries are uninstrumented. The full receiver target builds and the encrypted
synthetic control smoke passes. This batch does not rerun the entire portable
or Linux suite. Bench flags remain as disclosed in [HANDOFF](HANDOFF.md).
Private receipts are in
`~/.cache/headunit-qualification/20261009/iphone-gap-video-retirement/`.
Source `5db4dc7` matches all 630 first-party build/test inputs on the CM5.
The control binary SHA-256 is
`9b647db79f3d883835da932b7750f6dc3098c6451d02f73613b7b6db73731662`.
V27 remains selected; this repair is source-only with no live iPhone result.
