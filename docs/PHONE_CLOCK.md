<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Independent receiver clock

The independent receiver now estimates the phone's media time from its UDP
timing exchanges. `AirplayClock` provides a bounded clock sample;
`theme::video::ReceiverClock` feeds it into the reused `MediaClock`. The live
diagnostic verifies both video streams against this clock without changing
their preview rendering or inventing presentation timestamps.

## Measurement

The receiver records host monotonic time immediately before sending a probe
and immediately after receiving its reply. It requires the exact negotiated
peer address and UDP port and the exact outstanding origin value. One matching
reply consumes the request. Replays cannot qualify or refresh synchronization.
The origin is only a correlation token for this calculation: host wall-clock
offset and jumps do not enter the elapsed-time measurement.

The estimate uses the [four-timestamp delay calculation in RFC 5905](https://www.rfc-editor.org/rfc/rfc5905.html#section-8):
subtract the phone's processing duration from the host's measured round trip.
At host receipt, estimated phone time is its transmit timestamp plus half the
remaining delay. Half that delay bounds the unknown forward/backward asymmetry;
two microseconds cover conversion/quantization tolerance. Socket and scheduling
delay are included in the measured round trip. These are userspace timestamps,
not hardware or kernel arrival timestamps.

Timing UDP itself has no cryptographic authentication. Checking the peer and
echoed origin is correlation, not an authenticated clock protocol. Encoded video
timestamps are separately authenticated by the video's encrypted record.

## Bounds and lifecycle

- At most one outstanding probe and eight recent accepted measurements.
- Three consistent low-delay exchanges qualify the initial clock or a clock
  reacquired after expiration. Initial acquisition has a bounded 50 ms probe
  burst until three replies are accepted or eight probes have been sent. After
  that, probe interval is 250 ms while unqualified and
  one second while qualified with a measurement under two seconds old, or
  terminally faulted. When no accepted measurement arrives for two seconds,
  probing increases to 250 ms before the ten-second expiry. Rejected replies
  do not refresh the measurement or return the cadence to one second.
- Reject exchanges over one second, reversed/impossible phone processing, and
  residual network delay over 20 ms. NTP-era wrapping is handled with integer
  differences, without converting absolute timestamps to floating point.
- Retain a sample for at most ten seconds, matching MediaClock's synchronization
  lifetime. A conservative 500 ppm drift allowance increases uncertainty with
  age. At expiration the maximum modeled uncertainty is about 15 ms, below
  MediaClock's 20 ms bound. This drift allowance is host policy, not a measured
  guarantee about every phone oscillator.
- Publish the newest accepted measurement in order, with its measured network
  uncertainty and age drift. Re-querying projects that same measurement and
  never updates its actual `last_sync_ns`. Rejected exchanges cannot renew it.
- A phase jump beyond the 20 ms policy plus both measurement uncertainties and
  drift, or an accepted-measurement gap over 30 seconds, faults this timeline.
  A new paired connection is required; subsequent timing traffic cannot silently
  revive it.

The socket owner exports `AirplaySessionSetup::timing_sample()` as a value.
The media adapter compares the phase difference beyond both measurements'
uncertainty intervals against the same 20 ms phase policy. Treating measured
network offsets as exact points could otherwise reject a change admitted by the
network clock. The per-sample 20 ms uncertainty bound, freshness, relock,
monotonicity and terminal discontinuity checks remain. Clock-fault diagnostics
report only the phase magnitude and its bound, without raw clock origins.

On the display thread, call `ReceiverClock::update()` regularly, including idle
turns without video. It projects fresh clock queries at most every 100 ms, or
immediately for a new network anchor, leaving useful history for delayed
presentation feedback. Missing/expired samples suspend the media clock. Fresh
synchronization can resume a bounded continuous timeline. Backward queries or
mapping faults latch the bridge unusable.

Only actual compositor presentation feedback may advance CP.PTS. The receiver's
clock sample and a successfully mapped encoded frame are not presentation
receipts. A production owner must hide stale Ultra output, release staged frames
and reject old receipts across clock suspension/disconnection.

## Verification on 2026-09-26

All 40 portable Mac tests pass. The Pi's timing, clock and real-socket session
tests pass, including qualification with three distinct loopback exchanges and
replay rejection. AddressSanitizer and UndefinedBehaviorSanitizer checks pass
for the timing and clock tests. This change did not repeat unrelated GPU tests.

Clock fixtures cover exact media mapping, asymmetric delay filtering, source
processing time, rejection bounds, stale queries, requalification, terminal
phase changes, NTP-era wrap and monotonic anchor selection. The display bridge
test refreshes through eight seconds without video, maps a delayed presentation,
then exercises an outage and recovery. No phone capture is embedded in tests.

The first live connection successfully mapped both streams but exposed a
diagnostic bug: updating MediaClock only on video packets let its anchor expire
during an idle period. Moving refresh into regular receiver turns fixed it.
The receiver's sample lifetime also now matches MediaClock's existing ten-second
synchronization limit instead of prematurely expiring at three seconds.

After restart, the phone reconnected automatically. Both stream configurations
enabled source timestamps (flags 278). Main and cluster timestamps were about
78 ms ahead of receipt with 1.48 ms modeled uncertainty. During the subsequent
observation, more than 4,000 frames arrived with no clock loss, clock-bridge
fault, preview failure or connection-close report. The diagnostic schedules an
initial frame from each stream and then refreshes the clock continuously; it
does not validate every later frame's scheduling deadline. These are clock/transport results;
the unchanged preview does not schedule display presentation from those times.
Private evidence is under `captures/live-clock/session-20260926-164100` on the
Pi. No original LIVI process or source is used by this receiver.

## Remaining work

Join the synchronized clock and decoded native frames to the connection-scoped
Ultra VMs and GPU output. Keep initialization and scene work off the network
polling thread. Verified asset selection, UI-service lifecycle, actual display
receipts and fallback switching must work together before advertising live
Ultra display readiness. Apple Climate remains a separate incomplete feature.

## Intermittent live blanking investigation

The user reported brief flicker after independent CM5 gauges and Maps worked.
The same session records clock loss followed by recovery. The display owner
intentionally blanks while its clock is unavailable, so this is a candidate
cause, not a confirmed rendering fix. State-transition logs now include probe,
accepted, unmatched, invalid-exchange and excessive-delay counters, plus the
last accepted measurement's age. Diagnostics do not relax acceptance limits,
refresh synchronization from queries, or alter presentation timestamps.

A narrow timing-only packet capture measured intermittent residual round trips
of 26–117 ms among normal 3–10 ms replies. Those slow replies exceed the
unchanged 20 ms acceptance limit. The probe scheduler now switches to 250 ms
after two seconds without an accepted exchange, to seek a usable measurement
before expiration. Unit tests cover the boundary, rejected replies, recovery
and unchanged ten-second expiry. This does not guarantee against a genuine
network outage; live flicker verification is still required.

The follow-up session (`20260926-214026`) received cluster video from
21:47:27 until its planned stop around 21:52:30. It recorded 257 accepted
timing replies, 4,638 aggregate video frames and no clock-loss or UI-worker
failure event after initial synchronization. This approximately five-minute
run is positive timing evidence, not visual proof that all flicker is fixed.
The user has not yet supplied a follow-up screen observation.

## Startup regression checks — 8 October 2026

Source component checks cover a legitimate uncertain phase change passing through
AirplayClock and ReceiverClock, an out-of-bound phase jump remaining terminal,
and three distinct initial measurements qualifying at 100 ms in a synthetic
clock fixture. A missing peer exhausts eight fast probes and returns to 250 ms;
queries never qualify or refresh synchronization. Real boot acceptance belongs
to STARTUP_SPLASH and HANDOFF; these fixtures do not establish phone timing.

## Accepted clock corrections — 8 October 2026

A later component regression reproduces a source/bridge inconsistency without
widening any timing policy: twelve genuine synthetic exchanges each advance
phase by 12 ms, individually inside the existing source acceptance interval.
The earlier least-delay selection retains an old low-delay measurement, hiding
those corrections until its window changes. The bridge then sees a 96 ms step
against a 32.90 ms bound and correctly retires. New source publishes each latest
accepted measurement, including its real uncertainty. The same repeated-query
bridge regression passes; bad exchanges, freshness, three-exchange acquisition,
relock and genuine terminal jumps retain their existing bounds. This may expose
more bounded network measurement jitter than the old delay-selection filter;
it does not smooth by inventing timestamps or relax the media deadline.

V22's second phone boot actually retired on a 72.25 ms step against a 26.18 ms
bound. The component regression establishes a mechanism that can cause such a
publication jump, not a replay of that private phone exchange. The new fix is
outside V23 and awaits source CI, retained deployment and fresh phone runs.

## V39 warm-boot failure — 9 October 2026

One warm reboot of unchanged selected V39 initially reaches a fresh matching
publisher and confirmed cluster by **20.807 seconds after kernel boot**.
Observation begins at 19.274 seconds, so this is a Ready observation bound,
not first-picture timing or a boot-speed comparison. Both physical panels are
captured, both pairing records and the selected phone are unchanged, and the
five running services plus successful boot-recovery gate have zero observed
systemd restarts. The retained release/unit graph verifies before and after.

The longer receiver log invalidates a first-session reliability pass. Its first
control connection is accepted at 12.205 seconds; at **35.969 seconds** the
network clock becomes unavailable and reports a terminal discontinuity.
Counters are 35 probes, 20 accepted, zero unmatched/invalid and 13 delayed;
the last accepted measurement is only 1.008 seconds old. This is a source
continuity rejection, not ten-second sample expiration. The numeric rejected
phase magnitude and limit are not logged, so no precise phone-clock diagnosis
or correction is claimed.

Both outputs suspend with `clockUnavailable=1`, `ownerGap=0` and
`feedbackDelay=0`. The cluster faults at 43.980 seconds on
`qualified scene presentation stalled`, and control retires at 44.008 seconds.
The first connection therefore fails after the short eight-second Ready tail.
The same managed receiver later accepts a new iAP/control connection at
81.626 / 83.212 seconds and returns to signed Drive. No systemd restart is
needed; this recovery does not turn the failed first connection into a pass.

The three earlier bounded reconnects and a later recorder-free reconnect have
no clock discontinuity/display fault. Keep the failing boot receipt alongside
those positive functional observations. Selected V40 exposes source fault measurements without clock origins;
its scoped evidence is below. The failing V39 receipt remains a failure. Preserve the existing timing,
freshness and retirement bounds. Sanitized events and original private captures
are retained in `takeover-testing/hello-followup-v39/` outside Git.

## V40 source diagnostics — 9 October 2026

Source `f9ac93d` adds a fixed terminal fault record to `AirplayClock::Diagnostics`:
`phase_jump` or `relock_gap`, phase magnitude and existing policy bound,
accepted-measurement age, both uncertainties, host exchange duration and phone
processing duration. The control owner logs that record once per connection.
It contains relative measurements rather than wire origins. Subsequent traffic
cannot replace the fault, refresh its rejected anchor or recover the timeline.
Acceptance, probe cadence, ten-second freshness and retirement policy are unchanged.

Portable clock/timing checks pass **2/2** normally and **2/2** with macOS
ASan+UBSan (leak detection disabled). Five scoped CM5 suites pass **5/5 GCC**
in 16.25 s and **5/5 Clang 19 ASan+UBSan** in 16.55 s with Linux leak detection
on: timing, clock, session setup, session lifecycle and Bluetooth profile.
The clock fixtures verify both signs of a 30 ms jump against the exact
22.504 ms policy bound, distinguish relock-gap rejection from a phase fault,
and preserve terminal diagnostics through later traffic. Seven AArch64 programs
build from the exact frozen source. CM5 GCC retains the documented bench flags;
these checks do not repeat the full matrix, GPU fixtures or qualify a live fix.

V40 retains the startup branding × Apple scene and adds diagnostics only. The first attempt
with the previously selected phone cannot reach transport while its availability
is unknown; the user is away. No clock reliability result comes from that run.
A later selection of the user's original paired iPhone reaches fresh confirmed
Ready in **23.129 s**. Its planned 100-second tail is interrupted by the user's
request to reboot for the animation, so that tail is incomplete. Both original-
phone receiver sessions record signed STARTUP → DRIVE and no clock discontinuity,
display fault or UI stop at the scoped review. The warm reboot reaches fresh confirmed Ready by 116.379 boot seconds and
then completes a separate continuous 100-second Ready observation without
readiness loss. The service/rollback evidence belongs in HANDOFF.

The V39 source rejection has not been replayed with numeric diagnostics.
Preserve that open investigation instead of calling V40 a clock repair.
Private source/build and live receipts are in `takeover-testing/clock-reconnect-v40/`.

## V47 warm-boot phase rejection — 10 October 2026

Three warm reboots use unchanged V47 (`e0a3e56`) and its original paired phone
with both outputs on. The second reaches fresh confirmed Ready at 24.874
kernel boot seconds, then rejects clock continuity at 35.528 s. The terminal
record measures a **35,644,560 ns** phase change against the unchanged
**23,189,476 ns** bound. The last accepted measurement is 1,003,530,456 ns old;
previous/next uncertainties are 1,200,076 / 1,487,634 ns, the host exchange
is 3,037,767 ns and phone processing is 66,500 ns. Thirty probes include
seventeen accepted and eleven delayed exchanges. This is a phase rejection,
not ten-second expiry. Its first 20-second Ready tail fails with 37 not-Ready
samples. No clock bound, anchor or retirement policy is changed.

The sole managed receiver automatically retires the old timeline and acquires
a new one: clock synchronization is logged at 57.662 boot seconds. Fresh Ready
is first observed separately at 107.227 s, followed by 120.598 s with 120/120
fresh samples, no observer error or generation change. The interval from about
46 to 107 s is unobserved; clock acquisition alone does not date scene readiness.
Recovery does not convert the first session into a pass. All five service
restart counters remain zero, and the sealed graph and original pairings verify.

Attempts 1 and 3 reach fresh Ready at 37.819 / 20.471 boot seconds and complete
20-second tails without sampled loss, error or generation change. Their bounded
receiver-log scans have one positive synchronization epoch each and no source
discontinuity or VM budget fault. Three later explicit reconnects each complete
100-second stable tails with a positive clock epoch and no source-clock or
budget fault in their separate bounded logs; their mixed timing belongs in
[wireless handoff](WIRELESS_HANDOFF.md).

Private bounded numeric clock receipts are under `overnight/clock-v47-*`;
original logs and origins stay outside Git. This narrows an intermittent
continuity failure without claiming a clock repair, sustained fps, first-picture
or cold-power qualification. The separate four-hour overnight window completes
14,400.807 s on the same publisher. Its 1,067,972-byte current-log scan contains one positive clock epoch
and zero logged clock unavailability/discontinuity or VM budget faults. Four
readiness-status dips remain failures with unproven cause; this does not erase
the warm-boot phase rejection or qualify sustained fresh video.
[HANDOFF](HANDOFF.md) owns the completed window.

### Host-time correction review and source-only diagnostic

The current CM5 boot (warm attempt 3) runs `systemd-timesyncd` and reports NTP
synchronized. Its bounded seven-entry service journal dates initial time
synchronization at 33.850 boot seconds. Between the entries at 3.604 and
33.850 s, realtime minus monotonic time changes by **−485,084,000 ns**;
adjacent entries otherwise differ by only microseconds. This is a relative
host-clock observation, not a phone phase measurement. That boot's first Ready
tail and later clock scan remain stable despite the correction. Only the current
boot journal is retained; the failed warm attempt 2's host-time correction is
unavailable. Do not attribute its phone rejection to system time from this
different boot.

AirPlay timing requests use actual host wall time as their correlation origin;
replies to peer requests also contain actual host receive/transmit wall times.
Phone-clock qualification separately maps the phone's original timestamps to
actual host monotonic measurements. A source-only monitor now brackets those
host wall reads with actual monotonic reads and emits at most eight relative
correction records per connection. It records signed delta, observation age,
read brackets and monotonic observation time; no wall or phone origin is logged.
Reads exceeding 1 ms, reversed/overlapping reads and gaps over ten seconds do
not claim a correction. The diagnostic threshold includes brackets, conversion
tolerance, conservative host drift and 20 ms; it governs logging only.

The monitor supplies no media anchor, changes no wire timestamp and never
qualifies, repairs or retires the phone clock. Component fixtures cover both
500 ms correction signs, stable readings after a correction, NTP-era wrap,
uncertain/invalid readings, gaps and the report cap. Exact source `5e49f43` passes portable timing/clock checks 2/2 normally
(0.40 s) and 2/2 with macOS ASan+UBSan (0.73 s, leak detection disabled). All
six local matrix jobs pass: formatting, macOS 146, Linux GCC 171, Linux Clang
ASan+UBSan 171, full receiver GCC 172 and full receiver ASan 172 tests. All
750 source input hashes match that commit. This is component evidence; neither
source-only diagnostic is selected or live-qualified. V47 has no such monitor. Before a future reboot, preserve both relative host corrections
and the original numeric phone fault on the same boot/timeline. Their temporal
relationship can then be investigated without weakening continuity policy.
Private host journal and bracketed readings are in `overnight/host-clock-v47-current`.

### Per-connection host timeline — integrated in V50

Source `3c84875` anchors the host NTP instant once per connection and advances
timing replies and probe origins using the monotonic clock. Wall-clock readings
continue to feed the bounded correction diagnostic. Phone-clock estimation,
continuity bounds and retirement policy remain unchanged. `AirplayTiming::ntp_at`
checks progression, sub-microsecond rounding, era wrap and rejection before the
anchor. The combined V50 source is `b554463`; timing/clock checks pass on macOS
and CM5 normally and under ASan+UBSan. HANDOFF owns the exact deployment and
full component evidence. Its one real-phone reconnect confirms bounded recovery;
warm-boot clock-correction and sustained reliability acceptance remain separate.

## V49 fresh-pairing session clock expiry — 10 October 2026

Selected V49 (monotonic per-connection timing timeline) ran one managed session
from 20:10:38 CEST, after a fresh phone pairing. All three control connections
end the same way: ten seconds without an accepted timing exchange, then
`phone clock unavailable`, both outputs suspended with `clockUnavailable=1`,
the cluster `qualified scene presentation stalled` fault about eight seconds
later and retirement. None is a phase-jump or relock-gap fault, and boot
uptime was over nine minutes, so the timesyncd correction targeted by V49
is not involved.

| Connection | Accepted → clock unavailable | Probes / accepted / delayed / unmatched | Max delay | Wi-Fi association |
| --- | --- | --- | --- | --- |
| 1 | 100.4 s | 132 / 88 / 12 / 0 | 427 ms | Stayed associated until 54 s after retirement |
| 2 | about 22 s after sync | 61 / 10 / 14 / 3 | 183 ms | Phone disassociated 10 s before expiry |
| 3 | about 18 s after sync | 48 / 8 / 10 / 0 | 163 ms | Phone disassociated 10 s before expiry |

Connections 2 and 3 are explained by the phone leaving the access point;
expiry and retirement behaved as designed. Connection 1 is the open case: the
link stayed up, yet for ten seconds every reply either exceeded the unchanged
20 ms delay limit or never arrived. It resembles the 26–117 ms residual round
trips above, but lasts the whole sample lifetime. A diagnostic `grim` panel
capture (about one PNG per second) was running on the CM5 during it, so host
CPU contention is not excluded. The 20 ms limit, ten-second expiry and
retirement policy are unchanged; widening them would hide real outages.

Next: repeat a recorder-free steady connection of at least five minutes, then
one with deliberate CPU load, logging per-exchange delay. That separates host
scheduling from Wi-Fi airtime before any policy change. Private logs are in
`~/ultra-headunit/captures/cm5-live/session-20261010-201038` on the CM5.
