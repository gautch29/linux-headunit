<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Independent native Ultra video

`headunit-theme-video` reuses our decoder and codec-configuration parser, with
source hashes in THEME_REUSE.json. It takes the independent receiver's
`AirplayVideoPacket` directly. No original LIVI transport, helper, process,
screen parser or monitor IPC is required.

## Input and ownership

The wire parser retains only one incomplete packet, bounded to an eight-MiB
body plus its 128-byte header. Each feed has the same independent input limit;
a TCP read completing a maximum-size packet may also begin the next packet.
Complete packets are parsed directly from the caller's span, while returned
configuration and frame data remain owned. Fragmented packets reuse bounded
storage. Any parser exception permanently faults that instance, wipes its key
and releases retained storage; reconnection requires a new instance.

The receiver retains the raw header timestamp for diagnostics. A separate
optional `sender_ntp` is populated for authenticated frames only when the most
recent configuration enables source timestamps. Frame flags cannot enable it;
configuration changes can disable it, and a valid zero timestamp is distinct
from missing time. Auxiliary records do not advance the frame cipher nonce.

The codec parser accepts bounded AVC/HEVC records and their sample-description
wrappers. It rejects inconsistent lengths, malformed parameter arrays, duplicate
records and arbitrary trailing bytes. The decoder preserves exact source NTP
bits, including repeated timestamps, separately from GStreamer's internal PTS.

One owner thread configures, feeds and drains each decoder. Input is bounded
to eight buffers/eight MiB at appsrc and 64 pending timestamp mappings. A false
`push()` means the caller must retain the frame and retry after draining;
discarding arbitrary encoded frames can break inter-frame decoding. The owner
must enforce a stall deadline and replace the decoder after discontinuities.

Native output requires explicit DMA-buf format, modifier, plane layout and
colour information. The generic decoder defaults to three simultaneously
leased samples; its trusted owner policy permits one to four. The live native
input uses four: one actually presented sample plus three queued samples,
including at most two prepared submissions. Leases
keep the decoder's buffers alive until consumers release them, including after
decoder destruction. GPU completion must precede release; composition alone
does not advance the script's presentation time. CPU reference and metadata-only
outputs are explicit diagnostic policies, never silent native-import fallbacks.

CPU frame offers allocate their shared lifetime before moving the caller's
pixels. Allocation failure leaves the frame, token sequence, timestamp ordering
and memory accounting intact, allowing the same offer to be retried. A single
shared allocation owns both pixels and the budget lease; closing the stream
cannot release memory still held by a prepared external frame. The portable
fault fixture reproduces the preceding lost-frame failure and verifies retry
and final-lease accounting. This changes the CPU reference path, not DMA-buf
import or GPU presentation policy.

### Multiple pending submissions

`Stream::prepare_next()` protects a bounded prefix of its queue and exposes the
newest prepared frame to composition. Each `GpuDisplay` submission records the
exact source token it drew and whether it owns that token's completion. Thus a
second scene can draw a different due frame before the first scene's receipt.
Rendering does not advance `current_pts()`; only that source's accepted actual
presentation receipt does. Receipts complete in submission order.

A discarded scene transfers its source ownership to the next pending scene
only when that scene drew the **same token**. Otherwise the old frame is
cancelled; a newer protected frame remains valid. A single cancelled preparation
can be retried. Once a newer frame is prepared, cancelling the old one drops it
so source order cannot reverse. Script flush invalidates old tokens; clock
suspension discards every staged/queued/current lease. Tokens are never reused.

If the decoded queue is full, the owner leaves output in the bounded decoder
queue and retries after receipts free storage. It must not pull a sample and
then fail because the stream has no slot. Compressed ingress remains ordered
and uses backpressure; these display changes do not drop encoded deltas.

### Deferred GPU ownership

The optional deferred renderer keeps a separate immutable imported-image set,
EGL completion fence and deadline per scene. Its FIFO is bounded to the
configured one/two pending presentations. Polling an older presentation does
not wait for newer GPU commands. GPU fences release import leases; only the
matching accepted presentation receipt completes the stream token. Readback,
reference rendering and teardown drain GPU ownership before reusing resources.
Deferred completion defaults to disabled. The current CM5 bench explicitly
enables it for the live performance comparison; both physical views and touch
have passed a short check, but sustained performance and touch-to-photon
latency remain unqualified. Faster CPU submission is not completed GPU frame
latency.

## Cropped HEVC dependency fix

The installed Pi package `1.26.2-3+rpt3+deb13u1` passes 1280×720, but fails our
1278×718 native fixture: its decoder copies the cropped picture into a default
buffer that lacks the required VideoMeta. Declaring crop-meta support alone
does not fix that decoder implementation.

Raspberry Pi's `1.26.2-3+rpt4+deb13u3` package includes the upstream
[zero top/left crop fix](https://github.com/GStreamer/gstreamer/commit/e7be87b3de56e8c9922359c410b5c17864cbbf29)
and the companion explicit rejection of unsupported DMA-buf cropping. The
fixture then retains genuine native buffers and matches the software decoder.
This validates right/bottom padding; arbitrary top/left cropping is not claimed.
The consumer advertises both layout and crop metadata and validates the returned
layout rather than reconstructing missing strides or guessing modifiers.

For this test, only the packaged `libgstv4l2codecs.so` was extracted into
`/home/headunit/ultra-deps/gst-v4l2-crop/plugins`. Nothing was installed over the
system plugin. The source version, archive/plugin hashes and license are recorded
in GSTREAMER_VIDEO_TEST.json. A production image should use a coherent fixed
package set; this isolated plugin is a compatibility test against the currently
installed GStreamer 1.26.2 libraries.

## Build and reproduce on the current Pi

The native decoder requires Linux and GStreamer 1.24 or later. The GPU probe
also requires the pinned Skia configuration in GPU_RENDERING.md. Node and ffmpeg
with x264/x265 are test-fixture tools, not runtime requirements.

```sh
cmake -S . -B build -DHEADUNIT_BUILD_THEME_VIDEO=ON \
  -DHEADUNIT_TEST_NATIVE_VIDEO=ON
cmake --build build -j3
GST_PLUGIN_PATH_1_0=/home/headunit/ultra-deps/gst-v4l2-crop/plugins \
GST_REGISTRY_1_0=/home/headunit/ultra-deps/gst-v4l2-crop/registry.bin \
  ctest --test-dir build --output-on-failure
```

The isolated Wayland test creates and removes its own compositor; it does not
restart or take over the user's desktop. The test plugin selection applies only
to that command and its children.

## Verification scope

### Receiver-owner retirement regression — 15 October, source only

The receiver's actual connection retirement lives in internal
`control_receiver.hpp::UltraRuntime::retire/reap`; there is no separate
`ReceiverVideoSupervisor` class. The combined Linux fixture invokes those
production methods with a real `UiSession`, `UiWorker`, `ReceiverVideo`, two
`NativeInput` endpoints and production `GpuDisplay::PendingScene` scheduling.
A single synthetic scene references both phone inputs; this is not the installed
two-panel factory or an authenticated phone session.

The explicit fake compositor holds a callback across actual receiver retirement,
then returns a modeled accepted receipt for old prepared sources. Both queues,
copied clock, touch and camera/content authority withdraw through the real owner.
Old source presentation counts cannot advance. A second gate retains fake GPU
imports during cleanup: repeated control reap calls retain the unfinished worker.
RAII releases both gates on assertion failure. Original native backing dates
survive external holds and all leases release after their final holder. A fresh
receiver endpoint/status owns separate authority; old callbacks cannot update it.

A stopped `UiSession` is terminal. Its separate existing same-session worker
replacement guard must not be confused with `UltraRuntime`'s capacity for four
retiring workers. This regression changes neither admission nor retry policy.
An independent native capture queue retains simulated provenance, acquisition
and expiry across phone retirement. A genuinely fresh queued acquisition still
cannot authorize signed `LocalInput` without its qualified phone clock. No frame
is redated, no device is selected and no missing observation becomes zero.

The compositor, native frames, clock anchor and receipt flags are software
fixtures. No decoding, DMA-buf import, GPU fence, signed resource, optical output,
phone or physical car runs. Existing original native-input tests remain unchanged;
the separately accepted direct-input regression is not imported or edited here.
The automatic shared-failure entry in file-local `pump_ultra` remains outside this
fixture. Compiling the GPU-disabled control caller does not execute that branch.
No whole automatic supervisor, physical fallback/failback, sustained reliability,
causal fps repair or fresh60 result follows. Source review found no demonstrated
production defect; this is an additive regression, with qualification and cleanup
receipts retained outside Git. New UI keys, actions, IDs and pieces: none.

In combined forward source `61b523f`, the Linux receiver-retirement fixture and
its original scheduling/worker/native-input dependencies pass strict GCC and
Clang 19 ASan+UBSan checks with GPU disabled. Mac covers the portable dependencies;
it does not run the Linux receiver fixture. No production video or fps repair is
claimed. [Combined evidence](HANDOFF.md).

Root whole-source review and a new coherent required release matrix, original GTK
qualification, helper/configuration/unit/input/asset bindings and fresh complete
capacity plus reserve precede sole-owner bench adoption. The model remains
unpublished; both streams, UI-Sync, audio, control, main/touch, native fallback,
Climate and Home/Back keep their existing owners.

### Combined native-input retirement regression — 14 October, source only

The V87 source audit found no demonstrated defect in `NativeInput` generation
admission or receipt retirement. The existing state fixture separately covered
transport replacement, old receipts and terminal frame-before-configuration
failure. It now combines those boundaries with a current source, two prepared
receipts, one waiting source and externally held native backing. A successor
transport's actual admission exception must clear current/prepared/waiting
Stream ownership and receipt tokens. Repeated polls and both late accepted
receipt flags cannot advance the presented-source count or reopen the endpoint.
Held snapshots retain their original source NTP/timestamp metadata until their
last external reference releases the backing.

A second input retains its original current and prepared source tokens at this
local boundary, advancing only on its own ordered receipt. This does not qualify
session survival: `ReceiverVideo` intentionally closes both queues when its
shared supervisor observes a terminal native endpoint. Generation checks sample
the ingress state under its mutex; they do not lock transport retirement across
GPU work or prove an atomic physical swap.

The combined case runs the production `NativeInput`, `NativeInputQueue` and
`Stream`, with explicitly synthetic native frames, media-clock anchor, transport
IDs and receipt booleans. It performs no decoding, DMA-buf import or compositor
presentation. Existing portable clock, worker, transport, Stream ownership and
fake-output scheduling fixtures remain separate evidence. The native-input
state target is Linux-only; compiling receiver/native callers is not a GPU or
signed-scene test. Qualification and cleanup receipts remain outside Git.

No production code, source dates, generation/token floors, queues, framing,
deadlines, clock policy or default frame policy changed. Original per-frame host
ingress dates, complete authenticated stage-to-receipt joins and trusted
sender-to-panel mapping remain unavailable at this base. This regression does
not diagnose low fresh fps or improve it. Real phone recovery, useful returned
pixels, optical presentation and fresh60 remain open. Root whole-diff review and
a new coherent release qualification precede any sole-owner bench adoption;
the component remains unpublished. New UI keys, actions, control IDs and pieces:
none.

The 9 October parser regression covers every split point of a synthetic
configuration/frame pair, bytewise delivery, a maximum-size packet completed
together with the next packet, zero-body heartbeat completion, authentication,
size/type rejection and terminal fault reuse. The former parser rejects the
legal maximum-size boundary; the corrected parser returns both packets.
All 131 portable suites pass, with the parser additionally checked under
macOS/CM5 ASan/UBSan and strict CM5 GCC 14.2.0. These are component tests; no decoder,
GPU, phone session or selected release is changed. Scoped timing results and
the reproducible benchmark are in [performance](ULTRA_PERFORMANCE.md#authenticated-video-parser--9-october-2026).

Fixtures are generated by ffmpeg and encrypted with a synthetic key. They pass
through the independent authenticated packet decoder, then the real software
or hardware decoder. Checks include 96 queued AVC frames, repeated and disabled
timestamps, wrapped AVC/HEVC configuration, bounded native leases and a retained
frame after decoder teardown.

The GPU checks use 12 changing HEVC frames per case at 1280×720 and 1278×718,
with two or 66 overlays. They compare pixels against software decoding and
exercise DPVideoLayer, repeated imports, transforms, opacity and masks. Native
decoder pixels are not CPU-mapped; final readbacks and a separate software
decoder supply the test oracle. Isolated Wayland checks also verify actual
presentation receipts. Their media-clock anchor is explicitly synthetic.

On 2026-09-26, all 55 Pi tests passed with the isolated fixed plugin, and all
38 portable Mac tests passed. The packet/timestamp and codec-configuration tests
also passed AddressSanitizer and UndefinedBehaviorSanitizer with halt-on-error
enabled. The GPU/driver stack itself was not sanitizer-instrumented.

Simple video-plus-overlay draws measured roughly 8 ms on this Pi. That is not
a complete cluster frame or a sustained 60 fps claim. The full signed-layout
costs and limitations in GPU_RENDERING.md still apply.

## Current integration and remaining qualification

The independent receiver now joins verified assets, connection-scoped UI
workers, bounded video ingress, hardware decoders and GPU displays outside the
network polling loop. Its synchronized clock and actual presentation receipts
drive the live bridge; see PHONE_CLOCK.md. The user confirmed main video/touch
and Maps between Ultra gauges on two physical CM5 panels. Never fabricate
`CP.PTS` from CPU uptime or decoder completion.

Normal disconnect clears the session and reveals the independent native
cluster. Deliberate worker/clock faults, sustained reliability and 60 fresh
frames/s with the real iPhone remain qualification work. The new moving
native-video fixture uses real hardware decode and physical presentation, but
its clock, UI peer and video backdrop selection are explicitly synthetic;
see [performance measurements](ULTRA_PERFORMANCE.md). Decoder/component tests
alone do not qualify a complete Ultra session, audio or Apple Climate.
