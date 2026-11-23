<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Touch-to-display latency

**Current direction — 10 October 2026:** the user reports touch-to-screen lag
that does not feel premium and has resumed latency work. The first target is
the deliberate PTS hold on decoded phone frames (below). Keep the verified
wakeup and input mapping. Physical end-to-end latency is still unmeasured.

## CM5 decoder measurement — 2026-09-26

The independent main preview displays 1280 × 720 CarPlay through software H.264
decoding and a Wayland surface. The user confirmed physical touchscreen input
and correct screen proportions. Its complete touch-to-display latency has **not
yet been measured**.

An isolated offline benchmark replayed the same 281 main-screen frames on the
CM5 in three sequential runs at reduced scheduling priority (`nice 10`). It
preserved source timestamp spacing over 13.267 seconds, including a 5.417-second
idle gap. It did not restart or modify the live receiver.

| `avdec_h264` setting | Steady p50 | Steady p95 | Steady maximum | CPU, one-core equivalent |
| --- | ---: | ---: | ---: | ---: |
| Automatic, current default | 6.84 ms | 11.21 ms | 27.86 ms | 14.35% |
| `thread-type=slice` | 6.97 ms | 12.44 ms | 26.32 ms | 15.02% |
| `max-threads=1` | 7.57 ms | 12.18 ms | 18.82 ms | 15.77% |

Each run decoded 281/281 frames with no missing, duplicate or unmatched output
timestamps, and no frames held until end-of-stream. The sampled appsrc queue
was empty before every submission; at most two submitted frames were outstanding.
Steady distributions exclude the first 30 frames and any EOS-released frames.

**Retain the automatic decoder setting.** These measurements do not establish a
benefit from changing it. GStreamer 1.26.2 explicitly chooses slice threading
when upstream is live, as our appsrc is. The initial concern that automatic
settings necessarily added frame-thread buffering was therefore incorrect.
See the [GStreamer implementation](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-libav/ext/libav/gstavviddec.c#L602-L630)
and [decoder property documentation](https://gstreamer.freedesktop.org/documentation/libav/avdec_h264.html).

### Limits of these results

- Timing covers encoded buffer submission to decoder output. It excludes
  Annex-B conversion, downstream pixel conversion, composition, presentation,
  touchscreen sampling and the phone's response time.
- Source timestamp spacing is available in the private capture; original
  network arrival times are not. This is a paced decode comparison, not a
  reproduction of network jitter or a maximum-throughput benchmark.
- CPU figures include the long idle gap and benchmark bookkeeping. They are
  not sustained 60 fps CPU estimates. One trial per setting cannot establish
  small differences or worst-case latency under production load.
- A decoded frame or acknowledged HID request is not evidence that pixels
  appeared on the panel.

## Input wakeup change

The receiver previously waited for network activity or a 10 ms polling timeout
before dispatching Wayland input. `WaylandPreview::input_fd()` now exposes a
borrowed readiness descriptor, forwarded through `GstVideoPreview`. The main
preview descriptor is appended after existing control/session descriptors, so
connection indices remain unchanged. The Wayland owner still performs all
reads and event dispatch.

This allows input socket activity to wake the receiver promptly. GStreamer
shares that display connection and can move events into the client's pending
queue before the outer poll observes readability, so the existing timeout
fallback remains necessary. The change is not a measured end-to-end speedup
or a guarantee that all 10 ms polling delays disappear.

### Live check

On 2026-09-26 the CM5 restarted with the wakeup change. After the explicit
CarPlay connection request, the user confirmed the picture returned, touch felt
slightly quicker, and animations were smooth. This is subjective feedback from
one session, not a timed A/B result. The full touch-to-presentation delay remains
unmeasured.

## Reproduce the offline comparison

On the CM5, from the repository root, with the existing GStreamer development
packages and C++ compiler installed:

```sh
mkdir -p build
c++ -O2 -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread \
  -Iinclude $(pkg-config --cflags gstreamer-app-1.0) \
  tools/benchmark_preview_decode.cpp src/h264_annexb.cpp \
  $(pkg-config --libs gstreamer-app-1.0) \
  -o build/benchmark-preview-decode

python3 tests/preview_decode_benchmark_test.py \
  build/benchmark-preview-decode /usr/bin/ffmpeg

# Use an existing private main-screen UHVP capture; never add it to Git.
capture=captures/cm5-live/session-20260926-192912/video-1-110.packets
for mode in auto slice single; do
  nice -n 10 build/benchmark-preview-decode "$capture" "$mode" || break
done
```

The benchmark accepts captures up to the receiver's 8 MiB diagnostic bound,
with at most 1,800 frames and a source timestamp span below 31 seconds. It
requires increasing timestamps, bounds compressed queue growth and EOS waiting,
and outputs statistics without writing decoded images or capture contents.
The generated-fixture test checks frame accounting in all three modes and
rejects malformed headers, truncated records, duplicate timestamps and excessive
duration. Strict CM5 compilation and these tests passed for this measurement.

## Next measurements and tuning

1. Preserve monotonic timestamps at input dispatch, HID queue/send, matching
   phone response, encoded arrival, decoded output and actual presentation.
   Report p50/p95/max stage delays and bounded queue-age statistics; avoid
   recording touch coordinates. A high-frame-rate recording of repeated taps
   and visible responses can additionally measure touchscreen and panel delay.
2. Measure event-channel small-packet delivery. Test `TCP_NODELAY` against the
   current accepted event socket behavior. Queued HID already enables POLLOUT,
   so it does not incur a second fixed 10 ms polling wait.
3. Measure decoded queue occupancy and frame age before changing its current
   three-frame maximum to one. Drop stale decoded frames if justified; do not
   discard arbitrary compressed reference frames.
4. Profile negotiated pixel formats, conversion and compositor presentation.
   Evaluate GPU conversion/import against the current software H.264 output.
   Decoder acceleration and lower display latency are separate questions.
5. Keep video initialization, capture writes and later Ultra scene work off
   the input/network loop. Current diagnostic writes are synchronous but stop
   after 8 MiB per screen; they cannot explain all sustained lag after that cap.

Change one variable at a time and retain a working configuration until live
measurements show an improvement without missed frames or touch regressions.

## Native Ultra input path

The full native Ultra renderer now forwards the main panel's touch/pointer input
to the same phone HID channel through a bounded connection mailbox. It dispatches
Wayland events on the worker's display turns, then the receiver flushes reports
on its next turn. Initially that mailbox did not wake the receiver; the
1 October change below adds this handoff wakeup. The worker still does not have
the ordinary preview's input-fd wakeup path. Full-script render time can therefore contribute input delay. Physical
Ultra picture and touch were subsequently confirmed by the user on the CM5.
End-to-end timing remains unmeasured; ordinary CarPlay's subjective latency
result above does not establish the native Ultra path's delay.

### Receiver mailbox wakeup — 1 October 2026

The native touch mailbox now exposes a borrowed nonblocking, close-on-exec pipe
descriptor. A new report wakes the receiver's existing poll loop, without a
network packet or the 10 ms fallback timeout. The worker still never writes a
phone socket. Descriptors are appended after control sockets, preserving their
connection indices. Reports remain ordered, bounded to 64, and scoped to the
current worker generation.

Draining consumes the wakeup even when HID delivery is backpressured. The
report remains queued, while the phone socket's POLLOUT and fallback timer
provide retry opportunities; an unread mailbox must not spin the receiver.
Cancellation creates a fresh wakeup for the replacement release, including
after a failed send consumed an earlier wakeup. Stale workers cannot enqueue
reports or wake a successor generation.

With installed-UI `diagnostics=true`, fixed-storage counters measure mailbox
enqueue to **accepted encrypted HID queue**, not socket delivery or visible
response. Reports include mean, maximum, and histogram percentile upper bounds,
at most once per five seconds and at most 24 reports per connection. They do
not add touch coordinates, phone content or IDs. Default diagnostics remain
off. The measurement excludes touchscreen sampling, Wayland dispatch, the
phone, decode and presentation; it must never be called physical touch latency.

The isolated benchmark starts a synthetic press/release two milliseconds into
an otherwise idle 10 ms receiver wait. Each mode uses 64 trials. Timing also
includes joining the producer after the wait; it is a controlled handoff
comparison, not the distribution of real gestures or a GPU/phone benchmark.

| Host / mode | p50 | p95 | Maximum |
| --- | ---: | ---: | ---: |
| Mac, timer only | 8.502 ms | 8.792 ms | 10.799 ms |
| Mac, mailbox wakeup | 0.095 ms | 0.152 ms | 0.378 ms |
| CM5, timer only | 7.995 ms | 8.028 ms | 8.045 ms |
| CM5, mailbox wakeup | 0.020 ms | 0.059 ms | 0.077 ms |

These results show the idle receiver wait can be removed from this handoff.
They do not establish an end-to-end improvement. The main/cluster presentation
queues, renderer policy and input mapping are unchanged for the first live
comparison. Physical response still needs its own measurement.

Reproduce without an iPhone or GPU:

```sh
cmake -S . -B build
cmake --build build --target headunit-touch-mailbox-tests headunit-touch-wakeup-benchmark
ctest --test-dir build --output-on-failure -R '^(touch-mailbox|preview-touch)$'
build/headunit-touch-wakeup-benchmark
```

Mailbox tests exercise actual cross-thread poll wakeups, socket backpressure,
cancellation after a consumed wakeup, stale generations, overload release,
descriptor cleanup and preserved fitted touch coordinates. Six relevant CM5
input/event/session/worker checks pass; portable Mac input checks also pass.

### Live wakeup check and main-queue comparison

The user reconnected, opened apps and swiped; both physical views and fitted
touch remained working. In sampled reports the mailbox-to-HID-queue mean was
about 0.13 ms and maximum 0.54 ms. Those times begin after the renderer has
dispatched input and end before the phone socket transmits it.

The next session changed only `mainPresentationQueueDepth` from 2 to 1;
cluster depth 2, polling 2 ms, deferred completion and opaque output stayed
unchanged. The table uses median interval means from nine five-second reports,
omitting the first two report windows. Both runs used real phone video, but
the app interactions and Maps source cadence were not identical: this is an
exploratory session comparison, not a controlled physical-latency A/B test.

| Metric | Main depth 2 / cluster 2 | Main depth 1 / cluster 2 |
| --- | ---: | ---: |
| Main scene fps | 59.20 | 47.53 |
| Cluster scene fps | 59.20 | 47.40 |
| Main submit → presentation | 31.99 ms | 17.64 ms |
| Cluster submit → presentation | 22.32 ms | 31.73 ms |
| Main fresh phone frames/s | 32.33 | 26.39 |
| Cluster fresh phone frames/s | 37.00 | 15.80 |

The user confirms that one queued main frame still works but feels about the
same. The shorter submission interval did not establish a touch improvement,
and both observed scene rates fell. Restore main depth 2 in the live bench,
retain the mailbox wakeup, and leave source defaults unchanged. A matched
workload and physical tap measurement are required before choosing a new
presentation policy. Shared GPU/owner scheduling may couple the two panels;
the configuration being separate does not prove their work is independent.
After restoring depth 2, the user reconnected and again confirmed both physical
views and correctly working touch. The mailbox wakeup remains installed.

## Native frame scheduling — 2026-09-26

Two avoidable waits were present: the UI worker scheduled its next turn 16 ms
**after** finishing both panels, and each GPU panel scheduled its next draw a
full interval after composition. Rendering time was added to both waits.

The worker now advances a fixed 16,666,667 ns schedule to the first slot after
its completed work. The GPU panel schedules draw starts on its configured
period. Missed slots are skipped in constant time; there is no accumulated
catch-up loop. The actual presentation receipt still limits each window to one
pending frame. These deadlines do not supply PTS or relax clock quality checks.

The synthetic worker benchmark runs a trivial script and simulates ordinary
frame work with sleeps. It measures worker turn intervals, **not** GPU render
speed or touch-to-display latency. Each case runs for 2.1 seconds, omitting the
first interval. The existing live receiver continued running during these
sequential baseline/candidate measurements on the CM5.

| Synthetic work per turn | Before median / p95 (ms) | After median / p95 (ms) |
| --- | ---: | ---: |
| 0 ms | 16.059 / 16.088 | 16.667 / 16.693 |
| 10 ms | 26.113 / 26.130 | 16.667 / 16.683 |
| 25 ms | 41.124 / 41.146 | 33.334 / 33.350 |

The zero-work rate changes from the old 16 ms period to the intended 60 Hz
schedule. Over-budget work skips slots: the 25 ms case runs at about 30 turns/s,
not an impossible 60. One short run per setting is insufficient to qualify
worst-case system latency. No claim of 60 fps full themes follows from this test.

Reproduce without an iPhone or GPU:

```sh
cmake --build build --target headunit-ui-schedule-benchmark
build/headunit-ui-schedule-benchmark
```

The frame scheduler's boundary, overrun, early-wakeup and overflow tests pass.
The CM5 also passed worker cancellation/flow, encrypted UI-session, and direct
and preview display lifecycle tests with this change. In the subsequent live CM5 check, the user reported that picture/touch still
work and response feels about the same. Both panel presentation counters kept
advancing, with native touch reports and successful phone event replies. No
post-startup clock loss or worker failure was logged during the sampled run.

The bounded five-second diagnostics show roughly 24–29 main scene presentations
per second and about 15 on the virtual cluster. These are short, approximate
compositor-counter rates, not camera measurements of physical scanout or a
controlled performance comparison. They remain below the 60 fps goal. Further
work needs timing evidence for actual script, scene, GPU and presentation work;
this scheduling change alone is not a demonstrated user-visible latency win.

The September 27 [CM5 rendering measurements](ULTRA_PERFORMANCE.md) now split
composition, Skia recording/flush and display submission. They identify the
cluster composition cost and document a rejected cache experiment; they do not
establish a touch-to-display improvement.

## Presentation advance — 10 October 2026

In Ultra mode, `NativeInput::stage` only lets a decoded phone frame into a
scene once its PTS is due on the host's media clock. The phone timestamps
frames ahead of their arrival: across three V49 connections, the first mapped
frame logged leads of +43 to +71 ms on four streams and −26/−66 ms on two
(those frames were already late and shown at once). These are single samples
per stream, not a distribution. A positive lead is time a decoded
frame waits before composition, on top of the phone, network, decode and
presentation-queue delays, and it applies to every touch response the phone
renders.

`videoPresentationAdvanceMs=N` (installed-UI config, default `0`, at most
`100`) stages a frame once its PTS is within N ms of due. Order, the decoded
queue bound, obsolete-frame discard (using the same due time) and genuine PTS
values are unchanged. Presentation receipts still report actual presentation
time to the phone, and the script still ticks with the last actual
presentation. Zero keeps strict PTS staging.

**Tradeoff.** Phone-timed UI-Sync work runs on the genuine media timeline, so
a non-zero advance can show phone video up to N ms ahead of natively rendered
scene changes keyed to the same PTS. This matters most during layout
transitions. Advancing beyond the typical lead mostly shows frames on arrival
and exposes network jitter as uneven motion. Choose the smallest value that
removes the hold.

Diagnostics (`diagnostics=true`) now add per-panel `videoLeadMeanMs` (report
window) and `videoLeadMinMs`/`videoLeadMaxMs` (since connection start): the
decoded-frame lead before any advance. These measure the hold, not physical
touch latency.

The portable state test covers strict staging at zero, staging within the
advance, holding beyond it, retaining genuine PTS and rejecting an advance over
100 ms.

### Live comparison — V51 to V53, 10 October 2026

All three releases share source `41ff01e` (V50's cluster row plus this change)
and V50's configuration with diagnostics on; only the policy differs. Values
are five-second report windows with real phone video. Interactions were not
scripted, so fresh-frame rates depend on what the user did; this is an
exploratory comparison, not a controlled physical-latency test.

| Release | Advance | Main queue | Steady main lead (ms) | Main submit → present (ms) | Main scene fps | Main fresh frames/s while interacting |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| V51 | 0 | 2 | 34.6–40.9 | 25.7–26.8 | 59.8–60.1 | 15–25 |
| V52 | 50 | 2 | 44.1–45.4 | 29.2–30.3 | 59.9–60.2 | 20–25 |
| V53 | 80 | 1 | 57.2–63.5 | 10.3–14.6 | 56.6–59.9 | 37–50 |

On V51 every decoded frame waited for its PTS: a steady 35–41 ms hold plus
about 26 ms of presentation queue. The lead is measured at decode before any
advance, and the phone's lead varies per connection (about 45 ms on the V52
connection, about 60 ms on V53's). V52 and V53 report no late frames; roughly
10–15% of decoded frames are superseded before composition on every release.
On V53 the main panel presented 88 of its first 212 decoded frames during
startup, then about 85% while interacting.

The user reports V52 "feels better" and V53 "great, feels fast", with a few
lost frames, mostly at the beginning, accepted as a trade-off. This is
subjective feedback; physical touch-to-photon latency is still unmeasured.
The cluster kept queue depth 2 throughout.

### Display response setting

The user asked for the trade-off as a native setting. The Home page has a
**Settings** dock item, shown only when the installed UI names a
`displayResponseFile`, with three fixed policies:

| Choice | Advance | Main queue depth |
| --- | ---: | ---: |
| Smooth | 0 ms | 2 |
| Balanced | 50 ms | 2 |
| Fast | 80 ms | 1 |

The page sends `display_response` to the phone manager, which atomically
writes the mode as a 0600 file owned by the display account. The Ultra worker
polls that file every 500 ms on the display owner thread and applies the policy
live: advance on both panels, queue depth on the main window and its deferred
GPU scenes. Lowering the depth never cancels a pending presentation; the next
submission waits until the queue drains. A missing, malformed, symlinked or
group-readable file leaves the current policy, which starts as the release's
installed-UI values. Each applied change logs
`Ultra display response mode=... advanceMs=... mainQueueDepth=...`.

### Passive touch-response probe — source, 11 October 2026

To measure touch response without a camera, the main panel now times the
phone's reply to ordinary taps. The phone sends no frames while its screen is
static, so on a panel that has decoded no frame for 250 ms, the first frame
decoded after a forwarded finger-down is attributed to that touch. Presses on
a busy panel are counted as skipped instead of guessed; no decoded response
within one second counts as unanswered. Drags do not start new probes.

With `diagnostics=true`, the main panel's report adds cumulative
`touchResponses`, `touchUnanswered`, `touchSkippedBusy`, mean/max
`touchToDecode` and mean/max `touchToPresent` (press forwarded → presentation
receipt observed). Reports also add per-window `videoDecodedWindow`,
`videoSupersededWindow` and `videoDiscardedWindow`, counting decoded frames that
never reached the panel.

The span includes the phone's reaction, Wi-Fi, decode, staging and the
presentation queue; receipt observation adds up to one feedback poll. It
excludes touchscreen USB sampling, Wayland dispatch before the display turn
and panel scanout, so it is a host-side response time, not touch-to-photon
latency. A phone that animates nothing for a tap is unanswered, not slow.
Portable unit tests cover idle attribution, receipt ordering, busy skips,
pending presses and timeout. No live result yet.

### First live probe results — V67, 11 October 2026

Session `session-20261011-174306`, real iPhone, MapsMedia, main screen at
CarPlay Home. The user made about 15 spaced taps in Fast, switched to Balanced
on the physical native Settings page (logged
`Ultra display response mode=balanced advanceMs=50 mainQueueDepth=2`), then
repeated. Totals: 9 attributed responses, 22 presses skipped because the panel
was still animating, 1 unanswered.

| Mode | Responses | Tap → decoded (mean) | Tap → on-screen receipt (mean) | Host time after decode | Superseded frames |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fast (80 ms, queue 1) | 6 | 87.7 ms | 109.4 ms | ≈22 ms | 25/578 (4.3%) |
| Balanced (50 ms, queue 2) | 3 | 88.9 ms | 132.5 ms | ≈44 ms | 11/545 (2.0%) |

Balanced figures are derived from cumulative counters (9 total − 6 Fast).
Maximum press-to-receipt was 184.7 ms. No frames were discarded after staging.

The phone, Wi-Fi and decode account for about 88 ms in both modes and are not
changed by these policies; Fast halves the head unit's remaining share (≈22 vs
≈44 ms) at about twice the superseded-frame rate. Samples are small (6 and 3)
and taps were not scripted; the span excludes touchscreen sampling and scanout,
so this is host-side response time, not touch-to-photon latency.
