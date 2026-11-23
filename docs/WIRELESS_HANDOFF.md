<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Wireless handoff and connection requests

## Current independent receiver

`run_lab_receiver.py` owns a bounded access point, DHCP, AirPlay discovery and
our C++ Bluetooth/MFi and CarPlay receiver. The independent CM5 has completed
live pairing, authentication, encrypted control, native Ultra main/cluster
video and touchscreen input. No LIVI process is required. The launcher cleans
up its own processes and addresses on termination; this remains a bench tool,
not a production radio/boot manager.

## Authenticated Wi-Fi handoff

In the observed working iAP2 session, the phone sent parameter-free `0x5702`
after MFi authentication. The accessory replied with `0x5703`:

| Parameter | Current prototype value |
| --- | --- |
| `1` | NUL-terminated AP SSID |
| `2` | NUL-terminated AP passphrase |
| `3` | Security mode byte, observed `2` |
| `4` | Channel byte, currently `36` |

`Iap2WifiHandoff` releases these settings only after MFi authentication. It does
not log the password. The private Wi-Fi file contains `ssid`, `passphrase` and
`channel`, is limited to 512 bytes and must be a private regular file owned by
the receiver user or root. The AP generator and Bluetooth handoff share it.
The configured Wi-Fi country is passed separately to hostapd.

AirPlay discovery publishes `_airplay._tcp` on port 7000 through Avahi. It uses
our own device ID, pairing UUID and Ed25519 identity, with explicit protocol
capabilities. Bluetooth, discovery and CarPlay device labels now come from the
same saved configuration; see [CONFIGURATION.md](CONFIGURATION.md).

## Requesting CarPlay after handoff

Some live CM5 starts needed an explicit request to the selected phone's
`_carplay-ctrl._tcp` service after Wi-Fi handoff. The original manual utility,
`tools/request_carplay_connection.py`, resolves that service only on `wlan0`
and only for the selected Bluetooth-address TXT identity. It sends
`GET /ctrl-int/1/connect` with the receiver's numeric device ID and configured
source version. HTTP 200 means the request was accepted; receiver video and a
visible picture are separate checks.

The optional `auto_connect_phone` receiver setting starts
`tools/carplay_reconnect.py` alongside the receiver. It:

- Requires the selected BlueZ phone to remain paired and unblocked.
- Accepts exactly one matching IPv4 endpoint on the `10.10.0.0/24` lab network,
  excluding the receiver, network and broadcast addresses.
- Checks established port-7000 control connections before browsing or sending.
  Any active control connection occupies this single-phone bench, including
  when the phone withdraws its discovery service during an active session.
- Confirms that the receiver is listening and checks again for a control
  connection immediately before sending a request.
- Defaults to at most three requests, separated by at least 15 seconds, per
  endpoint-presence cycle. A completed session ending or fresh endpoint
  presence can begin another bounded cycle.
- Runs discovery and HTTP work outside the native video/input thread, with
  subprocess/HTTP timeouts and the launcher's overall bounded lifetime.
- Logs state transitions and request results without credentials. Failure of
  this optional helper leaves the main receiver running.

This is not Bluetooth pairing or an always-on reconnect implementation. A
phone that never completes Wi-Fi handoff cannot be reached this way. A stuck
established control connection is deliberately left for the receiver's own
session lifecycle rather than being forcibly disconnected by this helper.
The lab topology is still one selected phone, one Wi-Fi AP and fixed port 7000.

### Boot-to-CarPlay measurement

The latest task prioritizes real boot-to-picture and first-session reliability.
Start radio preparation alongside graphics; do not equate a running manager or
TCP listener with connected CarPlay. A warm V19 kernel boot with the paired phone
confirmed close/unlocked accepted Bluetooth at 13.588 s, authenticated MFi at
14.621 s, accepted control at 15.582 s and produced real presentation readiness
at 19.968 s, about 5.15 s after startup branding completion. An earlier run with uncertain
continued phone availability needed retries; it is not a useful fast-boot baseline.
A later OS-cleanup run accepted Bluetooth at 12.362 s but its first Ultra session
failed on decoder backpressure, forcing reconnect. The bootstrap draining fix is
component-tested and four V20 first sessions stayed alive. V20 warm readiness
was 18.208 / 19.949 / 21.726 s after OS ordering cleanup; an earlier Bluetooth
retry run took 66.963 s. V21's measured boot still reproduced a startup decoder
stall, then a reconnect media-clock fault, finally reaching ready at 54.306 s
on its third session. V21 changes admission Python only, retaining all V20
compiled programs. First-session reliability remains open. Preserve
backoff and genuine readiness while addressing these distinct delays.
[Startup](STARTUP_SPLASH.md#boot-speed-and-first-session-reliability) owns the
boot graph, scoped evidence and OS settings; HANDOFF owns current deployment.

### Bluetooth iAP retirement and accessory reconnect

bluetoothd keeps its own duplicate of every RFCOMM descriptor it hands to the
profile owner. Retiring a transport with `close()` alone therefore left the
iAP link up: the iPhone and BlueZ both still considered iAP connected, the
phone never started a new session, and `ConnectProfile` reported success
without a new connection. The receiver now `shutdown()`s the socket before
closing it, so retirement really ends the link.

With a selected phone, the receiver also registers a client-role profile for
the phone's own iAP service (`00000000-deca-fade-deca-deafdecacafe`). While no
control session, transport or engine exists, the first selected-phone attempt
after receiver startup calls `ConnectProfile` immediately and asynchronously.
After a session it waits 3 s for the phone to reconnect by itself, then calls
`ConnectProfile` asynchronously from the
control loop. Failures back off 4, 8, 16, 32 and then 64 s; a successful
request waits 10 s for its connection before trying again. BlueZ delivers the
channel through the usual `NewConnection` path, under the same selected-phone
check. A cancelled request may complete after the owner is destroyed and never
touches it. The Linux Bluetooth fixture covers the grace period, backoff, no
paging while a transport exists, peer EOF despite a retained duplicate
descriptor, and destruction with a request in flight.

Boot-polish source additionally qualifies the immediate first selected-phone
attempt and retained later-session grace in the CM5 synthetic BlueZ fixture
(15.25 s). This is component evidence; real boot-to-picture timing belongs to
the exact selected generation and its fresh connection diagnostics, not this
fixture's elapsed time.

Live on 7 October 2026 (V11 candidate and diagnostic builds, bench iPhone),
the phone returned after every forced teardown, about 4–5 s from retirement to
the next iAP acceptance, and flipping the iPhone's Ultra setting took effect on
the head-unit-initiated reconnect in both directions. This replaces the
earlier dependence on the phone reopening iAP; the Wi-Fi request helper above
remains for phones that complete handoff but do not connect.

The 9 October V39 follow-up with the current paired phone recovers both panels
in three bounded Disconnect/Connect runs, but Connect to fresh confirmed Ready
takes **62.362 / 61.493 / 61.403 seconds**. All three fail the 30-second check;
functional recovery and same-generation Home/Back pass. The actual iAP accepts
are 51.056 / 49.801 / 49.800 seconds after each receiver's control listener,
before MFi or scene loading. These are different measurement origins and must
not be treated as boot or first-picture timing.

One targeted repeat without a recorder takes **63.356 seconds**. A private
error-only D-Bus observer records two `org.bluez.Error.InProgress` replies and
one `org.bluez.Error.Failed`, all addressed to `headunit-control-probe`.
This locates the repeated delay in Bluetooth connection establishment; it does
not establish why BlueZ remains busy or justify changing retry/clock policy.
Both pairings are preserved. The four reconnect sessions record signed
STARTUP → DRIVE without VM-budget, display-fault or UI-stop events. Their
private evidence is under `takeover-testing/hello-followup-v39/`.

### V40 classified reconnect diagnostics — 9 October 2026

Source `f9ac93d` records each failed asynchronous `ConnectProfile` call's actual
elapsed monotonic duration, a fixed error category and a small whitelist of
known daemon reasons. Unknown names become `other_remote` and bodies become `other`; device identities
and arbitrary GError text are never printed. Timeout, grace, cancellation,
backoff and connection authority remain unchanged. The Linux fake-BlueZ fixture
checks busy and failed replies, unknown-name/body privacy and the existing
retirement/cancellation paths; it passes in both scoped GCC and Clang sanitizer runs.

A bounded read-only D-Bus trace correlates method serials privately and confirms
that the observed busy/failed replies belong to **ConnectProfile**. V40's first
attempt expires locally at approximately 20 seconds while BlueZ continues its
operation. Subsequent calls get `br-connection-busy`; the original operation
later returns `le-connection-abort-by-local`. The initial run cannot qualify
reconnect time because phone availability is unknown and no transport arrives.
The user's original phone subsequently connects on the first attempt and reaches
fresh confirmed Ready in 23.129 s. Its later warm reboot also takes the timeout/
busy path, establishing that this delay is not confined to the sister's phone.

BlueZ 5.79 [device source](https://github.com/bluez/bluez/blob/5.79/src/device.c)
rejects profile connection while connection/discovery work is pending, and can
fall back from failed classic service discovery to a low-energy connection.
That is a possible mechanism for these observed replies; no HCI/page-timeout
capture establishes the initiating radio failure. Do not blindly shorten retries
or disconnect unrelated profiles. Keep numeric clock diagnosis separate.
The exact selected binding, rollback and completed live observation are in HANDOFF.

### Classic page before ConnectProfile — 10 October 2026

**Cause.** A V40 Disconnect → Connect took **73.4 s** to CarPlay ready. A
passive `btmon` capture shows the receiver's first `ConnectProfile` page
time out after 5.12 s (two seconds after the disconnect the iPhone was not
yet page-scannable). BlueZ 5.82 then falls back to a Low Energy connection
for dual-mode devices (`device.c`: a BR/EDR `EHOSTDOWN` calls
`device_connect_le`). Both paired iPhones are dual-mode and hold LE identity
keys. That LE attempt scans for **41 s**, can never carry iAP, and keeps
the device busy: the receiver's retries get `br-connection-busy` and back
off. The original call finally fails as `le-connection-abort-by-local`. The
next classic page connects in 1.3 s.

**Repair** (`f47558e`, bound corrected in `50e9492`). The receiver first
opens a nonblocking classic L2CAP connection to the phone's SDP channel
(PSM 1, BR/EDR address type), then calls `ConnectProfile` over that link
and releases its descriptor when BlueZ replies. A failed page now ends in
the kernel (`EHOSTDOWN` after the page timeout) and takes the existing
backoff without BlueZ, so no LE fallback can start. A bounded watchdog
(20 s, injectable 0.1–60 s) catches a link that never completes; 8 s proved
too short because the iPhone answered its first L2CAP Information Request
4 s after the link came up. A phone that reopens iAP itself while being
paged is not asked for a second connection. The unprivileged receiver user
can open the socket; no OS or BlueZ configuration changes.

**Evidence.** Same Disconnect → Connect harness with `btmon`, same original
iPhone, 9–10 October:

| Release | Connect → CarPlay ready | Radio sequence |
| --- | --- | --- |
| V40 | 73.4 s | page timeout, 41 s LE scan, second page |
| V41 (8 s bound) | 11.0 / 21.5 / 10.6 s | 21.5 s run: link up, bound expired during slow L2CAP reply, one backoff |
| V42 (20 s bound) | 13.4 / 13.5 / 11.8 / 17.0 s | one classic page each, no LE scan, no failed attempt |

After V41/V42 selection the phone also connected on its first page within
about 8 s of receiver start. The Linux fake-BlueZ fixture covers success,
open failure, a page that never answers, self-reconnect during a page and
the bound's validation (17.6 s on the CM5). Private captures and harness:
`/home/headunit/headunit-claude-bt-20261009/`. This is reconnect evidence with
one phone; warm/cold boot timing and the sister phone remain to measure.

The overnight follow-up cancels an unanswered page immediately when the
selected phone independently delivers an accepted transport. Previously the
page descriptor and watchdog survived until page readiness or its deadline;
the watchdog could report a reconnect failure after the phone had connected.
An actual `ConnectProfile` request already in flight keeps its existing reply
lifecycle. The strengthened private-bus Linux regression leaves the synthetic
page unanswered, accepts the phone transport, checks immediate descriptor
closure, and waits beyond the page bound without a page failure or second
profile request. It fails on the previous source and passes with the fix
(17.86 s, strict GCC). This is a component race test, not a new live-phone
reconnect measurement. Private baseline/final receipts are under
`~/.cache/headunit-qualification/20261010/overnight/page-race/`.

Retained V43 combines those source changes after the exact six-job local matrix
and seven affected CM5 GCC/Clang sanitizer suites pass. Whole-owner selection
reaches first observed fresh confirmed Ready at 37.052 s from selection start;
this includes stopping/restarting the graph and is not a Connect timing.
Its deliberate Cobranded request retires the VM at the existing budget. The
subsequent 120-second observer reaches no fresh Ready, with repeated kernel
page timeouts and the normal backoff. A same-phone Disconnect/Connect through
the native manager then reaches fresh confirmed MapsMedia/Vibrant in 40.035 s,
and completes a 100-second Ready tail without sampled loss or connection change.
Both pairings remain. This is recovery from a failed session, exceeds 30 s, and
does not extend V42's four faster reconnect observations to all failure paths.
Retained V44 uses the same source with strict optimized Clang. After its separate
Cobranded retirement, a native-manager same-phone Disconnect/Connect reaches
fresh confirmed Ready in 28.590 s, then holds 100 seconds with no sampled loss
or generation change and both pairings preserved. This scoped recovery meets
30 s; the two individual GCC/Clang attempts do not establish a compiler-caused
reconnect improvement. The initial V44 observer starts after Ready, so its
52.896 s since selection start is an observation bound, not Connect latency.
The runtime fault and compiler investigation belong in
[cluster styles](CLUSTER_STYLE_REVIEW.md#v44-strict-clang-comparison).

Retained V45 adds the qualified early-turn CPU-clock overhead reduction.
Its immediate observer reaches fresh Ready by 35.691 s from whole-owner
selection start, then holds 20 seconds without sampled loss/generation change.
This is owner-switch readiness, not a new Connect timing. Cobranded and all six
styles now confirm on the same publisher; centre-content acknowledgment has a
separate failure. Later V47 qualification is scoped below.

### V47 repeated reconnect checks — 10 October 2026

Three explicit native-manager Disconnect/Connect runs use unchanged selected
V47 (`e0a3e56`), the same phone and two retained pairings. Each confirms
disconnect before Connect, capturing native Phones on the main output and
the simulated native cluster with unavailable Trip data and no phone on the
second output. Both displays stay on. Confirmation requires a fresh matching
receiver publisher and idle phone-confirmed cluster with no pending choice or
error, rather than a running service or a new clock alone.

| Attempt | Connect dispatch → first observed fresh confirmed Ready | 100-second tail |
| --- | --- | --- |
| 1 | 41.320 s; misses 30 s | No sampled loss or connection change |
| 2 | 19.108 s; meets 30 s | No sampled loss or connection change |
| 3 | 37.740 s; misses 30 s | No sampled loss or connection change |

All three preserve pairings/selected phone and finish fresh Ready. Their
separate bounded receiver-log scans record positive synchronization and no
clock unavailability/discontinuity or VM budget fault. These are scoped phone
checks, not full wireless reliability, physical touch or fresh-video acceptance.
Private receipts are in `overnight/reconnect-v47-reconnect-{1,2,3}` and
`clock-v47-reconnect-*` under the handoff's qualification directory.

Attempt 1's existing numeric stage record places profile readiness at 1.777 s,
iAP acceptance at 30.964 s, encrypted control at 33.450 s, clock acquisition
at 34.147 s and presentation Ready at 35.151 s; confirmed cluster idle is
observed at 41.320 s. Its classic-page failures are 5.163 s (`page-timeout`,
four-second retry) and 7.175 s (`other`, eight-second retry). The exact second
native errno is absent from V47, so its cause remains unknown. This localizes
most of that attempt's delay before transport without inventing a phone state.

Attempt 2 has no logged page failure: iAP is accepted at 7.906 s and presentation
Ready at 12.899 s, before confirmed idle at 19.108 s. Attempt 3 abandons an
unanswered page at its actual 20.006 s timer bound and retries after four
seconds; iAP is accepted at 27.429 s and presentation Ready at 31.738 s.
That timer does not supply a kernel errno and is not evidence of ETIMEDOUT.
The phone's screen, charging and radio-availability state are unobserved.

A source-only follow-up appends `native_errno` to the fixed failure record.
Immediate opener errors and actual `SO_ERROR`/`getsockopt` failures retain their
numeric native result; timer expiry and D-Bus failures say `unavailable`.
No arbitrary error body, identity, radio owner, retry timing or clock policy is
added. The private D-Bus/loopback fixture checks immediate EHOSTDOWN/EIO,
an actual asynchronous ECONNREFUSED result, absent timer/D-Bus errno and
preserved backoff/retirement. It fails the earlier source and passes repaired
strict Linux GCC in 19.43 s. Source `98a8f0f` passes all six exact local matrix
jobs: formatting, macOS 146, Linux GCC 171, Linux Clang ASan+UBSan 171 and
full receiver GCC/ASan 172 each, with all 750 input hashes attested. V47 remains
selected, and this follow-up cannot recover the missing error in its old logs.

## Connection time and lifecycle contract

Treat startup as successive observed boundaries: accepted Bluetooth transport,
iAP sync/link, identification, MFi certificate/challenge, Wi-Fi handoff
preparation, IP control accept, pair verification/encrypted control, `/info`,
initial/stream SETUP, RECORD, qualified clock, first video and qualified main
presentation. An authenticated request, prepared response or accepted RECORD
is not proof of a picture. Phone scene loading and GPU presentation stay with
their existing workers; the trace does not create another readiness state.

`run_lab_receiver.py --connection-diagnostics` explicitly enables both receiver
and reconnect diagnostics. An inherited environment cannot enable the launcher's
trace. The receiver records at most one observation of each of 39 fixed stages
per owner, with a 512-row process ceiling; the reconnect helper has a 64-row
ceiling. Dates are actual host `CLOCK_MONOTONIC` nanoseconds. iAP and control
have independent generation numbers; `linkGeneration` correlates them only
once the actual iAP engine has been attached. Zero means no correlation is known.
Raw phone identifiers, credentials, packet bodies and asset paths are absent.
Missing/reversed dates and output errors cannot change connection state.

An engine's prepared Wi-Fi/start stage is before transport transfer. The end
of a control response is after its actual socket write. Repeated pairing,
`/info` and stream SETUP exchanges record their first matching request/response;
a pairing response-end is not the whole multi-round handshake completion. Use
`encrypted-control`, ingestion and qualified readiness for those later boundaries.
First video counts
mean receiver ingestion; `presentation-ready` means the existing qualified
main receipt/status was observed, not a physical scanout measurement.

Successful SETUP also dates the existing owner's mode decision: `negotiated-standard`
means the UI-Sync/HEVC pair was not agreed; `ultra-requested` means the pair was
agreed but the exact UI service is not selected yet; `negotiated-ultra` means
that service SETUP succeeded. `main-stream-setup` and `cluster-stream-setup`
date the first accepted video listener of each type. These stages precede the
response write and do not establish an attached stream, video or presentation.
Compare them within one control generation, rather than mixing a prior Ultra
session's video counts with a later Standard attempt.

`setup-features` dates the validated initial offer, with a fixed received-feature
mask in `result`: UI-Sync is 1, HEVC is 2 and vehicle-state protocol is 4. Unknown
feature names are not logged. This is received support, separate from agreed
features and actual mode. Control `retired` records its immediate owner exit
category in `result`: 1 peer EOF, 2 socket error, 3 invalid control request,
4 accepted full TEARDOWN, 5 owner exception/failure and 6 receiver shutdown.
Zero remains unspecified for older traces and other owner scopes.


### Quick page retry and link-up request — V48, 10 October 2026

V47's slow reconnects and boots spent their delay before iAP transport:
classic pages the phone did not answer (5.16 s each) followed by the 4, 8, 16 s
backoff, plus two 20 s page-bound expiries. A V41 capture had shown the latter:
the radio link came up after 4 s, but the iPhone answered the probe channel's
L2CAP request slowly. Source `d6871d1` (branch `development branch`,
on `development branch`) changes two things in the existing owner:

- For 90 s after the receiver becomes idle (start, or end of a session) an
  unanswered page (kernel page timeout or the page bound) is retried after
  1 s; other failures, and all failures after the window, keep the backoff.
- The profile watches the selected phone's `Device1.Connected`; once BlueZ
  reports the link, iAP is requested over it immediately instead of waiting
  for the probe channel. The page descriptor stays open until BlueZ replies.

The Linux fake-BlueZ fixture (21 s on the CM5, now reporting failing lines)
covers the quick retry, backoff after the window and the link-up request.
Retained V48 `cm5-20261010-connect-speed-v48`, manifest
`c98d970b4308977f07c21a7a5bc53967725846efb01c30f286789c49aaacacfe`, is
V47's configuration with this source; it is selected (rollback
`transaction-pn8vk4sk` restores V47). Same phone, harness and criterion as the
V42 table (manager CarPlay state `ready`, a few seconds earlier than the
stricter fresh-confirmed-cluster criterion above):

| Measurement | Result |
| --- | --- |
| Receiver start → presentation-ready (selection) | 10.1 s, first page answered |
| Disconnect → Connect → ready, five runs | 10.9 / 9.9 / 7.7 / 7.9 / 8.3 s, no failed page |
| Warm boot 1, kernel → presentation-ready | 13.9 s (iAP accepted 9.7 s), no failed page |
| Warm boot 2, kernel → presentation-ready | 36.9 s: iAP profile ready 9.0 s, two unanswered pages retried after 1 s each, iAP accepted 28.7 s |

None of the five reconnects failed a page, so they do not exercise the new
retry; they show that nothing regressed. Boot 2 does: the same two failures
would have waited 4 + 8 s on V47. Both boots were taken immediately after an
active session, so the iPhone may still have held the previous link; a
genuine ignition cycle starts from a long-idle phone. Cold power, the second
phone and the stricter Ready criterion remain to measure. The V47 baseline in
the same conditions could not be repeated because the session dropped (clock
fault below) and the phone stopped answering pages.

Two further observations: every remaining slow case is the phone not
answering pages, not the head unit; and the open first-session clock fault
(`clockUnavailable`, "qualified scene presentation stalled") retired a V47
session about 74 s after it connected, forcing a reconnect. That fault, not
connection latency, now causes most visible disconnects. Boot also leaves
about 3.8 s between receiver start (5.1 s) and iAP profile readiness (9.0 s);
paging earlier would need the Wi-Fi AP to be ready by handoff and is not
changed here.

### Scheduling and failure ownership

Bluetooth's current transport descriptor participates in the control loop's
poll alongside the control/media/input descriptors. Read/write readiness wakes
the loop immediately; GLib remains its sole Bluetooth dispatcher. Before an IP
connection, its idle poll cap is 50 ms to service the existing 50 ms timer, replacing
the unrelated 100 ms wait. Established control retains its 10 ms cap. These are
host scheduling bounds, not a claimed end-to-end phone latency.

The Bluetooth owner sets its transport nonblocking and retains ordered unsent
bytes in its own bounded FIFO: at most 64 frames and 64 KiB of pending bytes.
Writable interest exists only while bytes remain; partial writes retain the
same frame/offset. A pending queue has a two-second local deadline, which partial
progress cannot renew indefinitely. Full queues, failed writes, authentication
failure and link retry exhaustion retire the transport and engine. A subsequent
connection creates a fresh generation. Retirement is irreversible in the engine
itself, including when an IP consumer retains a shared reference: later Bluetooth
or Wi-Fi input, sends and ticks reject it. A genuinely authenticated Bluetooth-to-
Wi-Fi handoff preserves only a live engine. No exception crosses a GLib C callback.
Wire/MFi retry policy and phone feature negotiation are unchanged.

The unselected 9 October reliability candidate fills link headers and bodies
in bounded chunks instead of recalculating the header checksum for each payload
byte. Marker resynchronization, checksums, the 65,535-byte frame limit and output
ownership remain unchanged. Regressions cover maximum frames, embedded markers,
corrupted bodies followed by valid acknowledgements, one-byte input and empty
input during a partial frame. A deterministic 2,000-stream differential check
against the preceding parser matched emitted frames and retained byte counts.

`headunit-iap-link-benchmark` generates 1,024 frames with 16-KiB payloads before
timing. Seven runs include checksums, framing, payload equality and retirement;
no radio, phone or rendering participates. On CM5 GCC 14.2 `-O2` with strict
warnings, the 4-KiB-feed median fell from 160.701 ms to 18.502 ms (8.7 times
faster for this CPU fixture). Complete-frame medians were 159.047/18.257 ms;
64-KiB-feed medians were 161.975/19.514 ms. Existing V27 services remained
running. These batch measurements do not establish a phone connection-time
improvement. Configure a Release build, then reproduce with:

```sh
cmake --build build --target headunit-iap-link-benchmark
build/headunit-iap-link-benchmark
```

The same candidate removes vector-front erasure from event and registered-service
socket output. A partial send advances a bounded cursor; append compacts only
when necessary. [AirPlay control](AIRPLAY_CONTROL.md) owns the wire/admission
contract and partial-write failure fixtures.

`headunit-pending-bytes-benchmark` compares the preceding vector-erasure operation
with the cursor. Each of seven runs drains eight deliberately full 2-MiB queues
in 4-KiB portions, checking every emitted byte. Queue construction and allocation
are outside drain timing. There are no sockets, encryption, radio, phone, GPU or
presentation receipts in this fixture. On CM5 GCC 14.2 `-O2` with strict warnings,
after compilation/tests finished and with V27 services still active:

| Drain implementation | Median batch time | Seven-run range |
| --- | ---: | ---: |
| Vector-front erasure | 184.037 ms | 179.126–189.069 ms |
| Bounded cursor | 3.496 ms | 2.198–5.287 ms |

This is about 98% less CPU time for the synthetic full-queue drain. Actual
socket writes, queue sizes and phone behavior determine the live benefit;
no connection-time or frame-rate improvement is established. Reproduce in a
Release build with:

```sh
cmake --build build --target headunit-pending-bytes-benchmark
build/headunit-pending-bytes-benchmark
```
The reliable-link receiver validates the session/payload pair before applying a
cumulative acknowledgement. Rejected frames cannot discard pending control or
authentication transmissions. Once retry exhaustion times out a link, later
input cannot deliver data, send acknowledgements or mutate its pending window.
Portable failure fixtures cover both malformed data combinations, preserved
retransmission, subsequent valid pure acknowledgement and irreversible timeout.
These are link-component checks, not physical radio or phone qualification.

The existing IP response sender also uses one absolute two-second deadline for
each send call. Partial progress, readiness races and interrupted waits cannot
renew it; socket failure or timeout rejects the partially sent response and the
existing control owner retires that connection. Response diagnostics still date
actual successful writes. Plaintext header/body use separate calls, and the
sender is still synchronous on the control thread. This is a failure bound,
not an asynchronous response queue or measured phone-speed improvement.

### Historical sampled startup

The bounded V7 real-phone run used source `26e5fd5` and restored the iPhone's
Ultra option. A sanitized observer supplies the following first observations:

| Interval | Observed duration |
| --- | --- |
| Bluetooth connected → established IP control | 5.310 s |
| Established control → first fresh qualified-ready publication | 4.291 s |
| Bluetooth connected → qualified-ready publication | 9.601 s |

These are sampled observer dates with no measured V3 comparison; they are not
tap-to-pixel timings. The user reported faster startup, but the new scheduling
and retirement changes have no measured V3 comparison; dated traces below
locate candidate startup boundaries without establishing a speed improvement. Standard still fails before video in unselected
V7/V8. Correct negotiated Standard display geometry has component evidence and
has not resolved that physical failure. See [current deployment](HANDOFF.md).

## Earlier reliability snapshot — 7 October 2026

The earlier reliability snapshot passes 126 portable and 169 actual CM5 suites.
Its frozen inputs remain separate from later 172- and 175-suite generations. The
Bluetooth fixture uses an isolated private D-Bus and socketpairs with the real
profile implementation. It checks immediate wake, ordered partial writes,
backpressure deadlines, invalid descriptors, authentication/callback failure,
retained-reference retirement and fresh generations. An authenticated handoff
uses explicit synthetic MFi registers and preserves its live engine; it does
not exercise physical I²C or a phone. IP response socketpairs cover successful
partial completion, stopped/trickling readers, real signals interrupting waits
and peer closure without SIGPIPE or renewed deadlines.

The final Linux control probe also passes the existing synthetic authenticated
pairing/event/control/video-ingestion smoke. Twenty-seven observed trace rows
have genuine non-reversing host monotonic dates across sixteen stages. With no
iAP engine attached, correlation stays zero; ingestion does not generate
presentation readiness. This checks integrated response/encryption ordering,
not wireless startup speed. [HANDOFF](HANDOFF.md) records frozen inputs, logs,
bench compiler exceptions and the unchanged selected release.

## V9 dated real-phone observations — 7 October 2026

Retained V9 authenticated Standard, accepted initial SETUP, answered `/info`
and accepted RECORD. A nonempty TEARDOWN received 400, then that control
connection retired before any main-video stream SETUP. No decoder or video
startup interval exists for that attempt. The older trace lacks the received
feature mask and retirement category; it cannot establish the failure's cause.

The next Ultra control generation reached qualified main readiness. Its trace
contains 61 dated observations, correlated to the actual iAP generation:

| Interval | Duration |
| --- | --- |
| iAP accepted → MFi authenticated | 1.941 s |
| iAP accepted → qualified main readiness | 6.547 s |
| Control accepted → qualified main readiness | 4.336 s |
| First video ingestion → qualified main readiness | 2.943 s |

These are host observations, not physical scanout or a comparison against V3.
The bounded guard retired early on a process-binding check; subsequent analysis
reproduced its handling of a terminal process. The user Home observation cannot
be assigned to the candidate from the saved request samples. V3 restoration
and working picture/Home/Back are confirmed. [HANDOFF](HANDOFF.md) records the
exact candidate and the corrected retained V10 generation. V10 adds the fixed
received-feature mask and immediate retirement category to locate its next
real-phone comparison.

## Validation

Policy tests cover retry exhaustion, presence changes, active-session exclusion
and rearming after disconnect. Endpoint tests reject other phones/interfaces,
non-AP addresses and invalid ports. A real loopback HTTP server verifies the
request path, identity/version headers, accepted replies and rejected replies.
These checks pass on the Mac and CM5. Live observation also confirmed that iOS
can withdraw its discovery service while the receiver continues streaming.
Automatic initiation by this helper still needs a live request followed by
video/picture confirmation; a session started by iOS itself does not prove it.

During the first live helper trial, the user disconnected/reconnected Bluetooth
and confirmed that CarPlay returned automatically. The receiver logged its
normal authenticated iAP2 start, and its existing IP control connection remained
active: the helper correctly issued no additional HTTP request. This verifies
non-interference with that reconnect, not live recovery from a missing session.
Retry interval and attempt count are explicit saved settings; the current
defaults are 15 seconds and three attempts.
