<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Preserve the main panel after a cluster output failure

## Offline sanitized support summary

`tools/support_status_summary.py` is a source-only, read-only projection of an
explicitly supplied **manager `status` result object**. It does not acquire that
object, contact the manager, inspect a checkout/release, collect logs, or verify
that a supplied report is truthful or current. Existing recovery-content, PCM
and video audits retain their separate evidence contracts; release manifests
remain the responsibility of `appliance_release.py`. No general sanitized status
export existed in the qualified V87 source used for this helper.

Run it offline as the file owner with absolute paths through real directories:

```sh
python3 -B -E -s tools/support_status_summary.py \
  --input /absolute/private/status.json \
  --output /absolute/private/new-summary.json
```

The input must be a stable, singly linked, caller-owned regular file, mode 0600.
The output parent must already exist, be caller-owned and mode 0700. No path
component may be a symlink. The tool opens only the two supplied paths and their
directory chains. It creates a mode-0600 file using atomic no-replace publication;
an existing target (including the input, a hard link or a symlink) is refused.
Malformed input and failed writes publish no partial summary. Diagnostics are
fixed strings and never echo argument values, input content or filesystem errors.

The version-1 output has `schema_version: 1`,
`evidence: "offline_supplied_manager_status"`, and `current_authority: false`.
Every following leaf copies its original field/type exactly, or is null when
absent/null. `rows` contains only supplied recognized rows; an empty array means
no supplied row diagnostics, never zero readings or unfitted equipment.

| Output path | Exact copied leaves |
| --- | --- |
| `manager` | `generation` (integer), `active_mode` (standard/ultra/null), `fallback_running` (Boolean). The fallback field describes only the manager's optional child, not the independently supervised native cluster. |
| `manager.recovery` | `remainingAttempts` (integer 0–3); excludes phone identity and supplies no new retry action. |
| `manager.carplay` | `state`, `returnReady`: original manager report only; never a newly validated Ready predicate. |
| `audio` | `available`, `pending` (Booleans). Pending intent remains separate from applied state; no volume/mute/action is exported. |
| `audio.output` | `sampledMonotonicNs`, `acceptedBlocks`, `discontinuities`, `underruns` (canonical uint64 decimal strings), `fresh`, `starved` (Booleans). These are original software observations, not audible or fresh-video evidence. |
| `media_observations` | `version` (1), `generation` (integer), `live` (Boolean). |
| `media_observations.pcm` | `state`, `sampledMonotonicNs`, `acceptedBlocks`, `previousSampledMonotonicNs`, `acceptedBlocksDelta` (original exact decimal strings/null). No counter subtraction or progress inference is performed. |
| `prephone_vehicle` | `configured`, `version` (1), `state`, `clockDomain` (`host-steady`), `sourceGeneration`, `sourceStartedNs`, `sourceSampledNs`. |
| `prephone_vehicle.publisher` | `live`, `sequence`, `copiedNs`; copied IPC work does not refresh acquisition. Boot ID, nonce, PID and errors are excluded. Without identity scope, dates/generations cannot join another host, boot or owner. |
| `prephone_vehicle.rows[]` | `id`, `availability`, `acquiredNs`, `maximumAgeNs`, `origin`; original row order/dates/provenance retained. Values, labels, units and all other fields are excluded. |

The only row IDs are `fuel_percent`, `range_km`, `battery_voltage`, `ambient_c`,
`coolant_c`, `oil_c`, `oil_pressure_kpa`, `odometer_km`. State/origin strings use
only the enums already declared by the existing manager/readiness/media/native
contracts. Unsupported enum values or recognized field types are rejected;
unknown keys and unknown row IDs are omitted. Duplicate recognized rows reject.
No arbitrary human strings, phone/release identities, paths, credentials,
pairings, artwork, raw logs/captures or whole input objects enter the summary.

Bounds: 1,500,000 input bytes; depth 16 (root at zero); 256 entries per object or
array; 4,096 characters per key/string; 8,192 value/container nodes; integer
magnitude at most uint64. Parsing rejects duplicates at any depth, malformed
UTF-8/JSON, NaN/Infinity including exponent overflow, and Boolean/float values
where an integer is required. Unknown fields still undergo structural bounds.
Canonical decimal fields retain their exact strings; missing is null and real
zero is `0`/`"0"`. No report time, acquisition, clock mapping, receipt, error
total, selected deployment, rendered frame rate or physical observation is
manufactured. Historical availability/fresh flags remain historical even when
true; requested, applied, logical Ready, visible and optical states stay distinct.

The regression uses first-party synthetic status/golden fixtures, actual
`Manager.request(status)` with an explicit BlueZ substitute/no receiver, and the
actual pre-phone consumer/media observation models for dated expiry, progress
and retirement. It starts no radio/display/phone owner. The original manager,
readiness, media and audio Python suites are preserved before new checks.
This is portable Python evidence; there is no C++ sanitizer, GPU, phone, physical
car or selected-release qualification. Reports and exact platform evidence are
retained outside Git for ROOT's whole-diff review before any integration.

**native UI: new manager keys, actions, control IDs, UI pieces, services and modules
in the managed release: NONE.** The offline JSON/CLI above is not a manager API
or release inventory addition. The model remains unpublished. Future bench work
belongs solely to phone-session, after ROOT review and a new coherent complete
release qualification/helper/input/unit/assets/capacity check. That owner may
explicitly provide an already retained private status result to this offline
CLI, verify that only this whitelist is present, compare original dates and
zero/null distinctions, and retain it without inferring current authority.
No CM5 probe, status collection or physical qualification is part of this tool.

## Selected V84 scoped recovery — 14 October

Frozen `3bd3db9`: 10/10 consecutive manager disconnect/connect cases recover
advancing original both-panel Ready. Five owner invocations stay unchanged,
automatic restarts 0; all 10 journals available, failure records 0. Polling upper
bounds 10.202/14.466/10.527/12.029/10.715/16.290/11.543/16.957/22.444/11.557s.
**Overall strict harness FAIL:** 30 video timeouts and 5072 conservative CarPlay
pattern matches in 606,956,539 bytes; generic matches alone do not prove severity.
Early 2s captures show blank main/startup logos, so ten usable visual returns are
unqualified. A separate settled MapsMedia/Music run establishes visible content
and advancing admissions; see [performance](ULTRA_PERFORMANCE.md).

Receiver-only service loss verifies inactive/PID0/readiness withdrawal, waits 3s,
then returns advancing Ready in 14.735s; other four owners unchanged. Cold restart
of all six appliance services verifies PID0/no jobs and returns Ready in 21.250s.
Both have zero automatic restarts. These are software polling upper bounds,
not phone-side RF loss, optical first frames or cold-power measurements.

Disconnected supervised native cluster renders distinct Minimal/Sport/Gauges
pixels with simulation provenance; Trip preference is restored before reconnect.
Its owner is headunit-cluster.service, not the manager's optional fallback child.
Two precondition-invalid harness attempts remain excluded, with no guard waiver
or second display owner. Final settled phone content and Climate dismissal have
separate pixel evidence. Original logs/receipts are private under
`20261013/phone-session/v84-reliability-ten-complete/` and service-recovery folders.

## Retained V83 phone recovery — 14 October

Ten consecutive manager disconnect/connect scenarios pass with original receipt
advancement, five unchanged owner invocations and zero automatic restarts.
Reconnect polling upper bounds:9.843,11.590,16.780,8.806,11.722,10.538,17.456,
12.301,22.537,11.671 seconds. The fixed original journal cursor gives available
journals for all ten with zero failure records. **Overall harness remains FAIL**:
32 video timeouts and3315 conservative CarPlay error-pattern matches in the
complete644,780,466-byte phone log. Method-name matches are not all actual
severity errors; raw counts are retained without weakening the acceptance rule.

A control-socket-only loss attempt does not establish a full session outage:
the IPv6 selector is first refused; corrected command ACK yields no observed
readiness withdrawal/new publisher. That repeat is explicitly interrupted and
retained as failed injection. Service-level fallback stops only the existing
receiver/manager, verifies inactive/PID0 and lost readiness, waits3s, then starts
it. Automatic phone connection returns advancing both-panel Ready in15.164s;
other four owner invocations remain unchanged, one intentional receiver
replacement, zero automatic restarts. This is not a phone-side RF-drop test.

All-six-service cold restart verifies every PID0/no queued jobs before ordered
start; first advancing Ready takes17.913s with zero automatic restarts. This
is service-level/software polling evidence, not optical first frame or physical
cold-power timing. Each step retains its own USB log, original receipts, journal
and both screenshots privately under `20261013/phone-session/`. Selected payload
is frozen0fe2ef9, manifestd2cfa857; later source needs fresh qualification.


The two Ultra panels have separate script VMs and GPU windows, but share a
connection-scoped `UiWorker` and ordered UI transport. Previously, any exception
from either display ended that worker and withdrew both panels. An optional
policy now retires a failed cluster renderer while preserving the healthy main
and the cluster's control protocol.

This is original project documentation, not an Apple specification. As of
**4 October 2026**, portable policy/two-VM transport checks and five isolated
CM5 GPU cases pass. Scenes and receiver clocks in those cases are explicit
fixtures; compositor feedback and GPU operations are real. Physical phone
continuity and native backup visibility remain unqualified. The qualification
did not enable either opt-in policy on the appliance.

## Configure the installed receiver

Add this exact key to the private installed-UI file:

```ini
isolateClusterOutputFaults=true
```

The default is `false`; other spellings and values are rejected. The installed
composition root enables containment only for the cluster. The main panel
retains its existing terminal-error behavior. The independent backup cluster
must already be running on the same cluster output's background layer; this
policy does not start it or supply vehicle readings.

## Leave the backup visible while Ultra waits

An independent optional key addresses a different gap: a new Ultra window
previously submitted black while it acquired timing and loaded its scene. That
window could cover the native backup even though no instruments were ready.

```ini
revealClusterUnderlayWhileWaiting=true
```

The default is `false`; the installed receiver applies it only to the cluster.
It can be combined with `isolateClusterOutputFaults`. The backup must already
be mapped underneath on the same output.

The output owner submits a fully transparent premultiplied buffer while
bootstrapping, after clock suspension or when the Runtime loses its root layer.
This clears the **whole final window**, including aspect-fit bars, in direct EGL
and explicit SHM preview. The opaque region is removed with that buffer's
commit. Ordinary scene commits restore the configured `opaqueWindows` policy;
resize follows the current per-buffer region policy.

Only a current-generation, accepted scene receipt sets
`GpuDisplayStats::scene_visible`. Losing the root invalidates that generation
before receipt polling. An older queued scene can still have genuinely
presented: its PTS and source-frame completions remain honest, but it cannot
restore scene eligibility or extend the qualified-scene stall budget. Root
recovery alone, a submitted frame, a transparent receipt or a discarded scene
does not qualify instrument visibility. Touch remains gated by qualified scene
visibility. Healthy control tasks may keep running at actual PTS to create a
new root.

If an overlapping script tick removes the root during composition, that same
submission becomes a real transparent buffer. Its omitted video preparations
are retained until feedback, then rejected or transferred to a newer queued
scene that drew the exact source. They are never declared presented merely
because the transparent buffer was presented.

A clear waits for older pending receipts to drain. An accepted waiting buffer
does not cause repeated empty submissions. An explicitly discarded clear may
be retried through the existing bounded feedback queue; it supplies no new PTS
and never renews the scene-stall budget. Clock loss retains the existing epoch
guards. A full/wedged queue can delay clearing until feedback or normal fault
withdrawal; this option cannot repair a graphics-driver hang.

`underlay_submissions` counts actual clear submissions, **not demonstrated
backup visibility**. The dated qualification below checks direct EGL and SHM
final buffers and actual receipts. It does not capture an independently mapped
native backup underneath or qualify a phone's transparent bootstrap/failback.
A compositor may discard an entirely transparent bootstrap; the existing
startup deadline remains authoritative and no timestamp is fabricated.

## Qualify a containable failure

`graphics::OutputFaultIsolation` is a portable, fixed-size owner-thread policy.
The GPU owner calls it only after a classified output operation reports an
error and cleanup succeeds. Initial isolation requires all of the following:

- The option is enabled and the panel's script tasks have started.
- The Runtime is healthy.
- A genuine accepted presentation PTS exists and is finite and nonnegative.
- The paired receiver clock is still ready in that presentation's epoch.

A real blank bootstrap receipt may supply that PTS after task startup. It does
not establish that gauges were visible. Submitted scenes, elapsed CPU time,
projected network time and asset verification declarations cannot qualify it.

Classified calls are GPU receipt polling, scene composition and submission,
plus the configured qualified-scene stall check. Exceptions from clock queries,
receipt-order/mapping validation, input pumping/completion, the VM, scene/overlay
construction callbacks or diagnostics remain terminal. The overlapping-script
callback temporarily leaves the output-error gate; a Lua or clock exception
there cannot be relabelled as a render failure.

## Withdraw output, retain protocol ownership

The owner destroys the failed GPU window and its imports before suspending
staged inputs. It clears pending receipts, scene visibility and stored
verification declarations, cancels touches, marks the Runtime not foregrounded
and releases any surface lease. Every input cleanup is attempted; a cleanup
failure remains terminal. The existing Runtime, report callback owner,
`NativeInput` objects and receiver queues survive successful isolation.

This distinction matters: destroying a native input closes its receiver queues
and would also stop otherwise healthy phone video. The isolated owner instead
calls `pump(nullptr)` to drain compressed deltas without preparing or publishing
new decoded output. It never retries composition, creates another window or
synthesizes a presentation receipt in this display's lifetime.

After withdrawal, the independently mapped background cluster can be exposed.
`GpuDisplayStats::faulted` describes the failed output; `output_isolated` and
`isolated_output_faults` distinguish this condition from a terminal VM failure.
The optional owner callback fires once with at most 512 characters of diagnostic
reason after successful retirement. It must remain bounded and nonblocking;
its own failure terminates the owner.

## Keep control turns honest

The isolated owner continues querying the real receiver clock. Ordinary frame
turns may run healthy script tasks at the **unchanged last actual PTS**, allowing
the scripts to consume and reply to UI messages. Feedback polls authorize no
extra task tick. No host-generated acknowledgement replaces an asset reply.
If an overlapping script tick already completed before the GPU error, the
returned turn records its duration and prevents a duplicate tick.

Clock loss or an epoch replacement permanently retires that PTS for this
policy. Relock cannot revive it. Inputs still drain, but script tasks receive no
timing authorization; the existing bounded queues and five-second stalled-
consumer limit still apply. A genuine Runtime, input or clock error remains
terminal. Ordered shared transport cannot skip an unconsumed cluster message
to reach a later main message.

Reconnect requires a new worker, Runtime, display and fresh actual receipts.
There is no automatic renderer repair within the failed display. The default
new-window bootstrap remains black. The opt-in transparent policy has the
buffer qualification below, but production failback that keeps real native
gauges visible until a fresh phone instrument scene is presented remains
unqualified.

## Limits and validation

This contains **reported output errors**, not a blocked graphics driver. GPU
destruction may wait for outstanding work before withdrawing the window. A
driver hang needs a separately qualified process/watchdog recovery boundary.
This policy also cannot guarantee usable vehicle values; the backup's original
acquisition times, expiry rules and unknown states remain authoritative.

The portable policy suite covers eight groups: default-off behavior, startup
qualification, immutable PTS, sticky clock loss, epoch replacement/fresh policy,
Runtime escalation, atomic invalid-input rejection and owner/bounded-operation
rules. Five additional real two-VM worker cases cover preserved main progress,
asset-generated replies under backpressure, both clock-retirement paths,
feedback without extra task authorization, owner cleanup, genuine script faults
and disabled containment. Their receipt timestamps are explicitly synthetic.
Seven affected portable CTest suites pass. Focused policy and worker/transport
source checks pass AddressSanitizer and UndefinedBehaviorSanitizer; linked
Runtime dependencies were not rebuilt with those instruments and Apple-platform
leak detection is unavailable. Valid policy operations allocate nothing in the
focused bounded-operation check.

The separate portable waiting-buffer checks run the production scheduler with
an explicit fake output. They cover one/two queued frames, discarded clears,
root loss during composition, stale receipts and clock loss without extending
the scene-stall budget; the fully instrumented target passes ASan/UBSan. These
checks supply neither compositor stacking nor backup-visibility evidence.

## CM5 Linux/GPU qualification — 4 October 2026

The prepared `theme_display_test.cpp` and
`theme_waiting_presentation_test.cpp` executables were freshly linked from the
coherent source, copied privately, then run under new owned headless Weston
14.0.2 instances. No physical compositor, receiver, radio or configuration was
altered. Artifacts were saved on eMMC; private runtime directories used a short
canonical path to remain within the Unix-socket pathname limit. The first
overlong runtime attempt failed before any client rendering and is retained
separately. The coherent GCC build retains its previously disclosed legacy
warning exceptions: global `-Wno-misleading-indentation` and gauge-target
`-Wno-maybe-uninitialized`. These executions do not establish a clean default
GCC build.

| Prepared case | Result | Real presentation events / discarded events |
| --- | --- | --- |
| Display, direct EGL | Pass | 15 / 0 |
| Display, overlapping script tick | Pass | 15 / 0 |
| Display, explicit GPU-to-SHM preview | Pass | 18 / 0 |
| Waiting buffers, direct EGL | Pass | 5 / 1 |
| Waiting buffers, explicit GPU-to-SHM preview | Pass | 5 / 1 |

These are the event totals across each fixture's connections, not live-session
frame-rate measurements. Direct compositor logs identify `/dev/dri/renderD128`,
Broadcom V3D 7.1.10.2, OpenGL ES 3.1 and Mesa 26.2.2. Preview uses GPU readback
on the render node and an isolated Pixman compositor.

### Contain the output fault

All three display modes inject an invalid receiver-owned overlay into real GPU
scene validation after actual scene feedback. The failed cluster window is
withdrawn, its isolation callback fires once, its Runtime remains healthy, and
its five subsequent control turns use the unchanged last actual PTS with
unchanged submission and presentation counters. Feedback-only turns authorize
no extra script tick.
The healthy main produces another genuinely presented scene after withdrawal.
Clock loss retires the isolated PTS; relock cannot revive it. A fresh Runtime
and replacement display require new accepted bootstrap/scene receipts and a
strictly newer PTS. The unrelated main clock-phase jump remains terminal.

The SHM preview case additionally stops **only its newly created compositor**
for 3.3 seconds, then resumes it. Delayed feedback suspends timing, old receipts
are ignored, and fresh real feedback permits recovery. The corresponding
repaint-delay warning is an expected consequence of that deliberate stop.
This is not a blocked GPU-driver or physical-display test.

### Clear the full waiting buffer

The direct case reads the actual final GPU framebuffer at 1280 × 720. It checks
opaque fitted image colors and black letterbox bars, then checks that every
pixel—including the bars—is zero RGBA for the clear. The SHM case validates the
actual final 192 × 72 submitted image; its transparent clear is applied to that
GPU-readback image before submission. Both modes restore the opaque image and
preserve the two-frame queue bound after rejecting an overfull submission.

The retained protocol trace maps six valid submissions to six ordered outcomes:
tokens 1, 2, 3, 5 and 6 are genuinely presented; overwritten token 4 is genuinely
discarded. Transparent tokens 2 and 5 remove the opaque region in their commits;
the following normal submission restores it. A discarded token is not counted
as a presentation or reused as a new PTS. All five processes exit with status
zero, all owned compositor processes are retired, and no private runtime
directory remains.

Private evidence is under
`/home/headunit/.cache/headunit-qualification/20261004/output-fault-check/`:
the manifest pins copied executable hashes, `report.json` records commands,
outcomes and cleanup, and retained logs contain only synthetic fixture traffic.
These artifacts are not repository assets.

### Remaining boundary

This qualifies actual output-error containment, final-buffer clearing,
opacity-region changes, ordered feedback and the preview's deliberate compositor
stall on Linux. It does **not** establish that the intended native backup is
visible under the physical fullscreen shell, that decoded phone-video leases
survive isolation, that two-screen phone/UI transport stays connected, or that
a new phone instrument scene safely replaces the backup. Those physical and
source-ownership cases remain to be tested. Driver hangs still need the separate
process/watchdog boundary described above.

See [GPU ownership](GPU_DISPLAY.md), [the worker](UI_WORKER.md),
[native inputs](NATIVE_INPUT.md) and [appliance startup](APPLIANCE_STARTUP.md).

## Scripted daily-use reliability harness

`tools/daily_use_reliability.py` is a Mac-driven qualification client for the
existing manager. It does not install anything, select a release, edit sealed
inputs, reset pairing or start another radio/display owner. **Only the bench
owner may run its live mode in an exclusive phone window.** The 13 October
implementation is source-only; synthetic checks do not qualify daily operation.

The default run performs ten manager Disconnect/Connect cycles, then ten
Connect requests in a row without a preceding Disconnect. The manager still
owns retirement and reconnection. Every case must observe a fresh ready
publisher, then retain readiness through the configured hold with an advancing
publication sequence. A successful command acknowledgment is insufficient.
On the first scenario failure, the run stops and preserves the appliance state
for its owner. It does not retry away failures or restore an older release.
The final successful case leaves the phone connected.

Run the portable simulation first; it uses no SSH, phone collector or hardware:

```sh
python3 -B tools/daily_use_reliability.py --dry-run \
  --cycles 10 --burst-connects 10 --link-loss-cycles 1 \
  --service-restarts 1 --cold-boots 1 \
  --output "$HOME/.cache/headunit-qualification/20261013/reliability-harness/dry-$(date +%Y%m%d-%H%M%S)"
python3 -B tests/daily_use_reliability_test.py
```

`--dry-run-failure` makes the first reconnect miss readiness: it must exit 1,
retain that failed case and mark subsequent requested cases incomplete.
Every output destination must be new, real and under
`~/.cache/headunit-qualification/`. Files are private (0600), with a 0700 run
directory. Phone captures, raw journals and receipts stay there, outside Git.

With the phone trusted over USB on the Mac and the selected phone already paired
on the CM5, the bench owner can run this complete scenario set:

```sh
python3 -B tools/daily_use_reliability.py \
  --host headunit@bench.example.invalid \
  --state-directory /home/headunit/.config/ultra-headunit/native-home/state \
  --ssh-key "$HOME/.ssh/bench_ed25519" \
  --cycles 10 --burst-connects 10 --link-loss-cycles 1 \
  --link-loss-reconnect automatic --service-restarts 1 --cold-boots 1 \
  --timeout 90 --poll-seconds 0.25 --hold-seconds 2 \
  --output "$HOME/.cache/headunit-qualification/20261013/reliability-harness/bench-$(date +%Y%m%d-%H%M%S)"
```

This command first requires actual `idevicesyslog` phone lines, then connects
through the manager's selected paired phone. It never accepts or prints a phone
address, UDID or serial. The Mac needs Python 3.9+, OpenSSH and
`idevicesyslog`; the CM5 needs Python, accessible journals, and passwordless sudo
for the explicitly requested receiver service restart. Set `--host` and `--state-directory` explicitly for live runs. Keep Ethernet management
available. No macOS `timeout` utility is used; subprocess deadlines and owned
process-group retirement bound collection. SIGINT/SIGTERM writes a failed,
incomplete summary and retires only the harness's local collector processes.

The optional scenarios run after the connection batches:

- `--link-loss-cycles N` pauses for the operator to disable the phone's wireless
  link while preserving USB logging. It requires an actual manager not-ready
  observation, then pauses for link restoration. The default waits for automatic
  reconnect; `--link-loss-reconnect manager` explicitly asks the existing manager
  to reconnect. These are separate recovery methods. A missing SSH reply alone
  is insufficient proof of phone link loss.
- `--service-restarts N` restarts **only** `headunit-receiver.service`, which owns
  the existing manager. It expects a new receiver invocation and fresh publisher;
  it does not restart compositor, outputs, page or cluster. Reconnection after
  restart depends on the installed preferred-phone/boot policy; no hidden Connect
  rescues a failed restart case.
- `--cold-boots N` pauses for the operator to cold-power-cycle the CM5. It requires
  a different boot ID and new owner invocations before accepting fresh readiness.
  The harness neither sends a reboot nor controls power. Pressing Enter after
  applying power supplies an operator timing marker, **not** a measured power-on
  date. Expected SSH outages retain unavailable samples; counters in the new boot
  are measured independently. Journal cursor loss across a volatile-journal boot
  leaves journal evidence unavailable and the run fails. Retain persistent journal
  access when qualifying this scenario; do not replace the missing excerpt with
  a successful reconnect log.

For a separate 30-minute steady session, request one cycle, zero repeated
Connects, and `--hold-seconds 1800`. Hold failures remain failures. The summary
records requested/completed/successful counts and configured timing parameters.

`summary.json` schema 2 contains `passed`, `evidence` (`synthetic` or `bench`), cases,
per-owner automatic restart deltas and observed invocation changes, phone error
pattern counts, and journal coverage. `logical_recovery_passed` reports completion
of all requested logical cases separately from strict collection/error checks.
`failed_cases` counts actual failed cases and `incomplete_cases` counts requested
cases not completed. Exit 0 means the selected logical/collection checks pass;
exit 1 means a scenario, evidence collection or error check fails; exit 2 means
invalid CLI/output setup. `bench_qualified` remains false: the harness has no
current usable-content or optical observer. This applies to bench and dry runs;
`usable_content` explicitly reports unknown candidate/fresh-scene evidence.
Missing/invalid measurements are JSON null, including missing restart counters
and unavailable phone error counts. Unexpected owner replacement or automatic
restart fails the run; explicit receiver restarts and cold boots are accounted
for separately. Counter decreases within a boot are unavailable, not zero.

Read timing fields according to their evidence:

- `start_to_first_frame_s` and `start_to_first_frame_upper_bound_s` are null:
  this harness has no content-specific or optical first-visible-frame receipt.
  `start_to_readiness_upper_bound_s` dates the first observed **logical Ready**,
  backed by the original fresh receiver receipt and manager admission.
  It is a host-clock polling upper bound, including SSH,
  manager command work and poll delay. They are not exact frame or photon times.
- `reconnect_upper_bound_s` uses that same observation after Connect or the
  operator's link-restoration confirmation. Cold-power and reconnect samples
  remain distinct. `poll_transport_duration_s` and `poll_interval_s` expose
  measurement overhead; replies taking more than five seconds cannot grant Ready.
- Original CM5 monotonic dates, boot ID, launch nonce, connection generation and
  publication sequence are retained in `receipts.jsonl`. Freshness is evaluated
  against the CM5 clock; the harness never subtracts Mac and CM5 monotonic clocks.
  Manager admission is sandwiched between matching original publications.
  Heartbeat progression is a lifecycle check, not fresh video-frame-rate evidence.

The run streams bounded phone syslog directly to the Mac, records per-case CM5
journal JSON excerpts after an original cursor, and hashes retained evidence in
`evidence-sha256.json`. `phone-collector.json` records completion and retirement;
`summary.json` pins the harness source commit and script hashes. Phone patterns
include video frame timeout and CarPlay/AirPlay/iAP2 error/failure phrases. These
are conservative heuristics: inspect private logs to attribute an error, and
never dismiss their failure by changing the reported count. Collector exit,
empty/non-phone output, oversized lines, journal truncation or malformed records
makes evidence unavailable. Phone syslog is capped at 256 MiB, each journal at
32 MiB/10,000 records; the streamed receipt archive is capped at 256 MiB.
Owner summaries are computed from that archive with constant memory per owner.
Collector loss stops further manager actions. Native fallback pixels, both physical panels, touch,
stream continuity, audible audio and full-resolution fresh 60 fps still require
their respective qualification methods.

### Usable-content evidence — 14 October, source only

The production `MainPresentationSnapshot` is deliberately content-free.
`GpuDisplayStats::scene_visible` can qualify a startup root; a valid receipt can
therefore precede useful phone content. Runtime scene phase sampled later is not
the phase of an earlier submitted/accepted buffer. Cluster catalogue `idle`,
confirmed layout, retained PTS, heartbeat progression and fresh file publication
also do not establish current fresh video. No deterministic startup/handover
owner fault is demonstrated by V84's early blank/LOGOS captures. Ready, Back,
signed greeting and native fallback semantics remain unchanged.

The new `usable_content` data object contains `candidate: null`,
`fresh_scene_qualified: null`, `visual_review: "unreviewed"`,
`optical_first_frame_s: null`, a reason and status `unestablished` or
`not_logically_ready`. Unknown is never converted to false/zero or a physical
success. The probe additionally reports `fallback.optional_manager_child_running`
and `fallback.manager_native_page` as available, with `fallback.visible: null`.
The actual supervised cluster process is in `owners.headunit-cluster`;
`optional_manager_child_running: false` does not assert that service is absent.
The manager page is its reported native choice, not a durable-preference or
visible-pixel receipt. Missing values stay null. Existing Connect uses exactly
one selected private phone; Disconnect uses the observed active generation.
Already-disconnected cycle setup skips the duplicate Disconnect. No guessed
phone/generation or second fallback owner is introduced.

`tools/recovery_content_evidence.py` is an offline reporter. It has no remote,
phone, display or image-decoding operations. It can inspect the original V84
archive without repeating its ten known failing scenarios:

```sh
python3 -B tools/recovery_content_evidence.py \
  --receipts "$HOME/.cache/headunit-qualification/20261013/phone-session/v84-reliability-ten-complete/receipts.jsonl" \
  --output "$HOME/.cache/headunit-qualification/20261014/codex-usable-recovery/owner-offline-review"
```

Exit 0 means parsing/reporting succeeded, not recovery acceptance. No supplied
review means all content remains unknown. Source/script and original input hashes
are retained in the report; it never edits the original archive or summary.
Receipt archives are capped at 256 MiB, one million records and 1.5 MB per line;
at most 1000 case results and 2000 selected review anchors are retained in memory.
Malformed/partial/duplicate cases, changed inputs and missing anchors fail closed.
The report counts actual logical successes/failures without calling an incomplete
archive a completed requested soak; the original summary owns requested counts
and strict phone/journal outcomes.

An optional private `--review /private/review.json` records explicit inspection
of original images. The version-1 format is:

```json
{
  "version": 1,
  "reviews": [
    {
      "sample_line": 42,
      "panel": "main",
      "classification": "blank",
      "capture": "original-main.png",
      "sha256": "<64 lowercase hex characters from the original file>"
    }
  ]
}
```

This is a schema example; replace the line/hash with actual evidence. Paths are
relative to the review file, must remain within that directory and cannot be
redirected. Files are only bounded/hash-verified (32 MiB each); their pixels are
never guessed. The review manifest is capped at 1 MiB. Allowed classifications:
`content`, `startup`, `logos`, `blank`, `nonpresenting`, `stale_video`, `unknown`.
Only explicitly observed stale video should receive `stale_video`; an unchanged
static picture alone cannot establish staleness. A main/cluster pair attributed
to one original sample is a reviewed candidate only if both say `content`.
Any explicit startup/logos/blank/nonpresenting/stale observation rejects that
sample's visual candidate; missing/unknown panels leave it unknown. Reports count
reviewed samples, not visually qualified recovery cases. Later content never
erases earlier nonusable samples.

Each anchor must satisfy the existing production logical predicate and match the
case's first-ready boot/nonce/connection, with no sequence reversal. Reported
sequence, CM5 receipt date and Mac observation bounds are copied unchanged from
that original sample. A prior/replaced/retired/stale publisher cannot borrow a
successor's review. **The image-to-sample attribution is the review author's
assertion**, explicitly labelled `capture_binding: "reviewer_attributed"`.
It does not establish when the image was captured, simultaneous panel visibility
or physical scanout. `capture_date_ns`, fresh-scene qualification and optical
first-frame timing remain null even for two content classifications. File mtimes,
review time and logical receipt dates never supply missing capture dates.

For native UI: these are qualification JSON keys, with no new manager actions,
widget IDs, native controls or GTK pieces. The source-only change does not add a
renderer, manager, stream or current-content authority.

#### Focused evidence and exact sole-owner follow-up

The baseline production harness fails the new first-frame regression because
it copies logical Ready's 0.75 s synthetic bound into first-frame timing.
The corrected source passes 32 portable Python tests through the shared build
slot, pinned format 19.1.7 and whitespace checks. Tests cover the actual
`run_cases` driver, private Unix IPC, bounded collector retirement, rejected
state/phone preconditions, unknown fallback, visual classifications, replay,
replacement/retirement, missing source context, redirected/tampered/oversized
captures and partial archives. A 23-case synthetic CLI passes logical checks;
an injected failure exits 1 with one actual failure and 22 incomplete cases.
Offline parsing of the 513,158-byte original V84 archive preserves ten logical
successes with content/fresh-scene/optical qualification unknown. No phone log or
image needed to be read for that check. Private source hashes, baseline and
initial failed fixture logs, final checks and verified retention receipt live in
`20261014/codex-usable-recovery/`. No compiled build tree/container was created;
this is portable/tooling evidence, not a new runtime, GPU or phone qualification.

The sole phone-session owner can use this tooling with the selected V84; it
does not require a release or deployment. First run offline against retained
receipts and, if desired, supply explicit private capture reviews. Preserve
strict phone-pattern counts/severity limits in the original harness summary.
For the next **single already-planned** handover/reconnect scenario, verify the
actual selected release/source, selected private phone and current manager state
upfront, retain the original native preference (including Trip), and use the
existing owner graph. Collect bounded before/disconnected/returned main and
cluster captures and original dated status/owner/source observations. A false
optional manager-child flag must not start another fallback process.

Usable fresh-scene acceptance is explicitly **parked** until an original
capture-to-publisher binding and a content-specific accepted-buffer/source receipt
exist: submission's scene phase, output/clock/source generation, actual source
acquisition/admission and matching presentation outcome. Current counters or a
later VM phase cannot supply that association. Qualify startup → fresh content,
source expiry, root withdrawal, retirement and replacement failure against that
production owner before treating such observations as runtime authority. Optical
power/first-visible-frame timing additionally needs an actual optical reference.
Use a bounded capture window and preserve an inconclusive result at its bound;
do not insert an arbitrary settle sleep, expand deadlines or repeat all ten
known cases to manufacture acceptance. Settled Music pixels/fresh admissions
remain useful separately scoped evidence, with no claim of ten visual returns.

V82 live session on 14 October completes **ten consecutive reconnect scenarios**
with advancing fresh original readiness and unchanged five service invocations,
zero automatic restarts and 9.684–22.328-second reconnect polling upper bounds.
The overall harness remains failed: its conservative phone patterns count 82
video-timeout and 2480 CarPlay/AirPlay/iAP error lines, requiring attribution,
and a journal cursor parser mistakenly included `-- No entries --` before the
original cursor. The source repair preserves exactly one original cursor and
rejects missing/duplicate/ambiguous values; portable regressions pass. It has not
yet been repeated with valid journal collection. Do not call the overall soak
qualified from scenario success alone.

The first run stops in cycle four because its phone collector exceeds 256MiB;
that failure and unavailable counts are retained. The complete repeat's private
adapter uses an explicit finite 2GiB phone-log budget and a bounded Perl alarm,
collecting 629,683,864 bytes without overflow. Record this instrumentation
variation; the default parser/receipt/journal/deadline guards are unchanged.
Original logs/receipts/screens and reports are under
`20261013/phone-session/v82-reliability-ten[-complete]/`.

Source-only checks on 13 October: **19/19** portable Python tests pass, including
receipt freshness/replay, journal/phone parsing, missing/reset counters, all five
synthetic scenarios, bounded collector retirement/write failure, publisher
replacement during the manager read, streaming archive replay, collector-loss
action suppression and interrupted-report generation. The
registered `daily-use-reliability` CTest passes **1/1**. A 23-case synthetic CLI
run passes; an injected reconnect failure exits 1 after its first failed case
and keeps the other 22 requested cases incomplete. Pinned clang-format 19.1.7
and whitespace checks pass; no C++ source changes or new compiled receiver are
included. A read-only CM5 service inventory found all five owners active with
zero automatic restarts. No live Connect, Disconnect, restart, power cycle or
phone collection was performed; the phone-session owner retains the bench.
Private development receipts, including initial failing fixture checks and
corrected passes, remain in
`~/.cache/headunit-qualification/20261013/reliability-harness/`.


## Phone-free native-owner audit — 14 October, source only

`tools/native_fault_recovery.py` is a **separate offline observer** for one
explicitly planned stop/start case. It reuses the five-owner allowlist and
original field encoding in `reliability_probe.py`, decimal parsing/private
output admission in `daily_use_reliability.py`, and bounded stable input and
unknown-content helpers in `recovery_content_evidence.py`. It contains no SSH,
service-control, PID signalling, power, radio, display or phone backend. Default
and every supported invocation are offline. No daemon, unit, generator, release
inventory, sealed input, acquisition source or GTK component changes. V88's
freeze is untouched; this tooling is for future reviewed qualification.
The existing daily-use soak's real-phone log gates remain unchanged.

```sh
python3 -B tools/native_fault_recovery.py \
  --receipts /private/original-native-case.jsonl \
  --verification /private/original-selected-verification.json \
  --output "$HOME/.cache/headunit-qualification/20261014/phonefree-owner-review"
```

The output must be a new real private directory in the qualification cache.
Original receipts and the referenced verification record are copied byte for
byte (0600, directory 0700) before parsing; summaries exclude arbitrary manager
fields, phone addresses, pairing/media/user names and raw diagnostic text.
Successful copies are synced. Inputs must be stable regular files with no final
symlink, never FIFOs/devices; partial, oversized or replaced data fail closed.
There is no shell interpolation or execution of receipt fields. Exit 0 means
only the offline audit passed, 1 means failed/unavailable evidence, 2 means
invalid invocation/destination. Actual INT/TERM/HUP produce an interrupted
failure report and exit 130/143/129 after retaining available inputs. The tool
owns no subprocess; fixture callers reap only their own children.

### Original receipt envelope

The JSONL file contains **exactly five lines**: one explicit plan followed by
`before`, `dispatched`, `retired`, `restarted` samples, in that order. No extra
case, later success, retry or missing phase can replace the first failure.
This is a new qualification envelope for original observations, **not a
production publication or a claim that a live collector already supplies it**.
Do not fill absent fields from file mtimes, later status, another run or clocks
on a different host. Mark absent evidence and park the case instead.

The plan has exactly these fields:

| Key | Required original scope |
| --- | --- |
| `event`, `schema_version`, `case` | `plan`, integer `1`, `stop-start` |
| `owner` | Exactly one of `headunit-compositor`, `headunit-outputs`, `headunit-phone-page`, `headunit-cluster`, `headunit-receiver` |
| `planned_restarts` | Integer `1`; one intentional replacement, separately from automatic failures |
| `evidence` | `synthetic` for explicit host fixtures, or `operator_collected` for original sole-owner observations |
| `verification_sha256` | SHA-256 of the separately retained original selected-verification record |
| `scope` | `source_commit` (40 hex), `selected_manifest_sha256`, `helper_manifest_sha256` (64 hex each), `units_sha256` (exact five-owner map of original 64-hex unit hashes) |

Each sample carries `event: sample`, its exact `phase`, original UUID `boot_id`,
`remote_monotonic_ns`, and host request `host_started_ns`/`host_finished_ns`.
They retain separate clock domains; no cross-host subtraction or offset exists.
Host request duration is at most five seconds. Original device observation dates
strictly advance, and host requests cannot overlap. The single case is bounded
by 90 seconds independently in each clock domain. These are qualification
bounds, not changed runtime deadlines. The audit itself checks a ten-second
cooperative processing bound between bounded reads; kernel filesystem operations
are not a hard real-time guarantee. Receipt/verification limits are 8 MiB/2 MiB,
one line 1.5 MB, five records, one retained case. Existing phone budgets and
acceptance remain unchanged.

Samples repeat the exact plan `scope`, existing probe `owners`, sanitized
`manager`, original `receipt`, and a separately observed `boot_gate`. Each of
five owner records must retain original `Id`, `ActiveState`, `SubState`, decimal
string `MainPID`, `InvocationID` and `NRestarts`. Healthy means active/running,
PID > 1, unique across the fleet, and a 32-hex invocation. Retired means only the
selected owner is inactive/dead/PID0; its invocation may be empty or the original
systemd value. During receiver retirement the manager is unavailable (`null`).
Other samples require actual manager `active_present: false`,
`error_present: false`; missing status is not a healthy phone-free fleet.
The original phone receipt is required as a field; null is allowed, a supplied
record must explicitly withdraw `returnReady` and cannot say `state: ready`.
Neither that record nor manager status grants native pixels.

`boot_gate` carries the original `Id: headunit-boot-recovery.service`,
`ActiveState: active`, `SubState: exited`, `Result: success` and 32-hex
`InvocationID`. Its entire supplied record remains unchanged across the case.
This preserves the existing retained gate contract and does not execute boot
recovery, verify a generator, fabricate a gate-ready date or simulate a reboot.
The selected scope and verification hash are **collector-attributed**. The
opaque original verification record is retained/hash-bound, not parsed as a
new verifier or independently trusted as proof of the appliance's current seal.
The sole bench owner's existing verifier and manager review remain authoritative.

`dispatched` and `restarted` each require an original `dispatch` record:
`operation` (`stop` or `start`), exact `owner`, original `boot_id`, integer
`expected_pid`, `expected_invocation_id`, original device
`dispatched_monotonic_ns`, and `acknowledged: true`. Stop must name the exact
before PID/invocation; start must name inactive PID0/empty invocation authority.
Dispatch is dated after the previous observation and no later than the current
observation. An acknowledgement never substitutes for actual retirement/return.
The dispatched sample must still observe the original owner; cases where it
already exited before this observation are conservatively inconclusive.
This envelope cannot eliminate a live query/action race; no live mutation
backend is provided until that boundary has a qualified contract.

The same boot, selected source/helper/units and other four owner identities must
survive every phase. All original `NRestarts` counters remain unchanged; increases,
resets and missing counts fail instead of becoming zero. Restarted requires both
a changed PID and changed invocation for exactly the selected owner. Planned
replacement count is one; automatic deltas are separately reported as zero only
when all original observations establish that. A stopped receiver must not leave
an admitted manager. Whole-owner shutdown/start, dependency cascades, automatic
failure/SIGKILL and cold/warm reboot are **unsupported cases**, not inferred
variants of this audit. Compositor/output faults commonly involve dependent
owners and therefore need their own reviewed expected-count/authority contract.

### Results, limits and sole-owner recipe

`summary.json` reports `audit_passed`, `logical_owner_recovery_observed`,
collector evidence scope, original phase clock/PID/invocation/counter observations,
selected scope, original input/tool hashes, planned replacement and automatic
restart deltas. `logical_service_return_poll_upper_bound_s` uses only host request
bounds from the stop dispatch observation to the returned sample, including
poll/transport work. It is not a service start date, native first buffer, presented
frame, optical timing or cold boot. `bench_qualified` remains false;
`phone_acceptance`, `power_on_ns`, `soft_reboot_ns`, `boot_gate_ready_ns`,
`first_native_buffer_ns`, `first_native_present_ns`, `optical_first_frame_s`
and content candidate/fresh-scene qualification remain null. Runtime renderer,
process and Gdk status cannot fill them. Human review of an actual PNG with its
original SHA is independent evidence, using the existing content review contract;
this auditor accepts no image or inferred capture binding.

Only phone-session may collect actual appliance evidence, **after** manager
reviews this tool, the exact selected generation is independently verified and
its coherent required matrix/helper/unit/input/capacity gates pass. Do not add
this tool to a sealed release or wait/inject it into frozen V88. The exact bounded
follow-up is one existing authorized **receiver-only stop/start** experiment:

1. Retain original selected source/manifest, helper binding, five unit hashes and
   successful current boot-gate verifier output. Require all five owners healthy,
   unchanged original boot/PID/invocation/counters and actual phone-free manager
   status before the experiment. Retain the native preference, including Trip.
2. The sole owner uses its reviewed existing receiver shutdown recipe, never a
   broad PID kill or inferred successor. Before any dispatch, revalidate exact
   authority. Collect the original stop dispatch acknowledgement/date and fleet
   observation, then actual inactive/dead/PID0 retirement and absent manager.
   No automatic Connect, pairing, key/config/selected-generation change occurs.
3. Before the explicit receiver-only start, require unchanged boot/selection/
   helper/units/gate/other-owner identities and actual receiver inactivity.
   Authority change or any failed precondition stops the case; do not run cleanup
   that could stop/start a successor. Collect original start acknowledgement/date
   and one bounded healthy successor observation, with exactly one planned
   receiver replacement and unchanged automatic counters/other four owners.
4. Retain originals outside any build, form the explicit envelope without filling
   unavailable fields, then run this offline command. Keep any failed attempt as
   failed. Collect separately scoped original native main/cluster PNGs+SHA/human
   review if useful; no status/polling/content acceptance can replace them.

**PARK:** no live collector currently supplies this complete envelope, nor is
an atomic query/action authority for native faults qualified here. The recipe
specifies evidence and safety conditions; this worker does not provide a runnable
mutation command or authorize a new live fault case. Phone-session must review
its exact existing authorized recipe and original dispatch/retirement receipts
before that future run. Missing dispatched-active, inactive-invocation, source
binding or gate fields remain inconclusive; do not widen the predicate to fit a
later run. SIGKILL, all-owner shutdown, cold power and soft reboot remain parked
pending exact reviewed expected-owner/count/cleanup contracts. Content-specific
accepted native buffer/source/presentation binding and an optical power reference
remain separate missing contracts. No new UI keys, actions, IDs, controls or
visual pieces for native UI integration: **NONE**.


Focused host evidence: **71/71 Python checks on both macOS and Debian ARM64**
(14 native-audit, unchanged 32 daily-use, 13 boot recovery, 12 boot contract),
zero skips. The actual auditor CLI retains original private bytes and interrupted
failure summaries; owned child INT/TERM/HUP exit/reap checks pass on both hosts.
The Debian adapter additionally proves actual same-backend container stop/remove
and Docker client reaping for INT/TERM/HUP, then removes its test container.
Pinned clang-format **19.1.7** via `tools/format.sh --check` and whitespace pass.
An initial new-test failure expected the wrong overlapping-observation rejection;
the first guard had already rejected it. A later snapshot-only formatting attempt
failed because its source snapshot had no `.git`; the actual repository check
passes. Both attempts, original source/logs, structured fixture receipts and
verified SHA retention/retirement evidence remain outside Git under
`20261014/codex-phonefree-recovery/`. All own lane scratch trees and four containers
are retired. No C++ build/full matrix, real PID1/native fault, GPU, pixel, phone,
audio, physical or deployment qualification is claimed. Future runtime adoption
still requires the manager's coherent combined required matrix and release gates.
