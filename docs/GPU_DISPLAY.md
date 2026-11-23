<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# GPU output on the Ultra script owner

`graphics::GpuDisplay` joins the reused GPU scene compositor, independent
ReceiverClock, and presentation-clock mapper to UiWorker. Its scheduling is
adapted from our own earlier DisplayOutput class; source provenance and the
original file hash are recorded in THEME_REUSE.json. No original LIVI app or
pipe transport was imported.

The optional owner-thread `decorate_scene` callback returns a separate authored
scene before canvas mapping, including recomposition after stale video withdrawal.
It never mutates the script tree. Manufacturer-only startup branding branding uses it to
retain the signed Apple/divider layers and original animation; output-coordinate
warning overlays still compose last. See [greeting](ULTRA_GREETING.md).

Construct one output per Runtime before evaluating the asset. A ClockSource
supplies an immutable snapshot from the independent receiver, or no sample when
unavailable. All GPU/Runtime/clock operations and destruction stay on the worker
thread. The configuration's `presentation` hook calls `GpuDisplay::turn(phase)`.
The output retains bounded verification-region declarations; it does not claim
that an independent instrument monitor has verified the displayed pixels.

The serial worker paces ordinary turns from their actual start. An over-budget
turn resumes promptly without waiting for another fixed-phase slot; a cheap
turn still starts at least one configured period after the preceding start.
This removes avoidable overrun idle, not the scene/receipt gates or GPU completion
requirement. The per-display draw deadline retains its phase, and extra feedback
turns cannot advance script tasks. Source-only evidence and the connected
measurement plan are in [performance](ULTRA_PERFORMANCE.md#phone-frame-timeouts-and-shared-owner-pacing--13-october-2026).

## Actual presentation and startup

Default startup composes a black GPU frame and submits it asynchronously.
The opt-in cluster underlay policy instead clears to transparent; both paths
retain the same genuine-receipt gate. Until the
compositor reports an actual presentation and its timestamp maps through the
paired media clock, the hook withholds script execution. A successful swap or
frame callback is insufficient. No CPU elapsed-time fallback supplies CP.PTS.

The worker ordinarily loads all display entry points, then starts their scheduled
tasks. Explicit main-surface deferral is described below. Bootstrap hooks consume a real initial receipt without submitting a
scene, even if main has already created a root layer. This prevents another
display's slow startup from leaving an unresolved scene presentation behind.
Bootstrap has a ten-second wait bound and can be cancelled without blocking
network ingress/stop calls. Lua execution keeps its separate existing budgets.

Ordinary turns collect once-only presentation feedback, render the current
Runtime scene directly on the GPU, and submit at most one outstanding frame per
window by default. The script turn follows, using the latest actual presentation time.
An opt-in queue permits two frames for performance testing. Each commit keeps
its own ordered receipt and native-video staging record. If an older commit is
discarded, its staged frame remains leased only if the next queued scene used
it. Source defaults keep the single-frame gate. The tested CM5 profile explicitly
uses two-frame queues; short live phone/touch comparisons and physical DigiRing
measurements are recorded in [performance](ULTRA_PERFORMANCE.md). Those tests
do not establish sustained 60 fresh phone frames/s.
Receipt mapping precedes the next clock query: installing a newer anchor first
can make it later than the already-stamped receipt's arrival time. Synchronization
is then checked again before exposing any PTS to scripts.

On **9 October 2026**, native `CLOCK_MONOTONIC` feedback now uses one actual
read for source/before/after correlation. The compositor and host already share
that domain; the former three-read midpoint introduced a synthetic offset that
could reverse receipts with equal presentation timestamps. `CLOCK_MONOTONIC_RAW`
still uses bounded cross-domain sampling. No timestamp is clamped or fabricated,
and stale, future, uncertainty and retirement checks retain their limits.
Monotonic and media timeline failures now have distinct diagnostics.
The native-clock assertion fails against the former code in an isolated headless
labwc fixture and passes with the fix in GCC and ASan+UBSan. Five affected
presentation checks pass in each CM5 build after relinking all their fixtures.
V31 deploys this correction; it does not establish sustained phone reliability
or resolve the separate wallpaper issue.

## Copied input receipt interface — source only, 14 October

The optional typed `InputPresentationReceipt` accompanies existing staged-source
completion after accepted scene feedback. It preserves original Wayland
submission/raw feedback/clock correlation, accepted host/media mapping, poll
date and submitted/active MediaClock suspension epoch. Discard/invalid epoch
passes no record. Legacy camera/input consumers retain bool completion.
NativeInput's one current-token numeric snapshot is copied at the existing
finite report cadence; no pixel/lease ownership or scheduling changes.
[Receipt fields and qualification](ULTRA_PERFORMANCE.md#accepted-input-receipt-facts--14-october-source-only)
own the exact interface, retirement limits and sole-owner bench recipe.

## Loss, idle and teardown

Loss of synchronization or a clock suspension epoch invalidates pending receipts
and the published PTS. The VM is marked not foregrounded and its tasks are held.
A receiver-owned black frame replaces the default output when no presentation
is pending; the optional cluster underlay policy uses a transparent clear.
Neither receipt is reported as asset presentation. A fresh, mapped bootstrap
receipt is required before scripts resume. Protocol queues remain bounded while
the display waits; they can still trigger the worker's stalled-consumer policy.

A long owner pause also requires a fresh receipt while retaining presentation
token/domain/time-order guards. Hidden desktop previews retain their one pending
compositor update, but invalidate it for script timing and resume through a new
bootstrap. By default, terminal mapping/render errors withdraw the window and
fault the worker. Opt-in `isolateClusterOutputFaults` contains only classified
cluster output errors after genuine presentation and script startup. It
withdraws that window while retaining healthy protocol tasks and native inputs;
the main display can continue. Clock, VM, input and cleanup failures remain
terminal. Clock loss permanently retires the isolated display's control PTS.
No output retry or driver-hang recovery is provided. Portable checks and five
isolated CM5 GPU cases pass as of 4 October 2026; physical backup visibility,
phone continuity and failback remain unqualified. See
[output fault recovery](OUTPUT_FAULT_RECOVERY.md).

Direct output performs no per-frame CPU readback. Explicit desktop preview uses
the compositor's existing GPU-to-shm copy path. The default scene-completion bound
is 100 ms; frame cadence defaults to approximately 60 Hz, which is a scheduling
request, not measured full-theme throughput. Slow frames reduce actual cadence.

## Withdraw the main surface for a native owner

An optional `GpuDisplayOptions::surface_lease` provides bounded owner-thread
acquire/release callbacks. Acquire is polled even while held: a false result
requests withdrawal. The main renderer destroys its internal GPU window, then
clears pending source preparations and releases the lease. Its original
`GpuDisplay`, Runtime/report binding, decoder and stream generation survive.
The separate cluster display does not use this lease.

During withdrawal, the owner continues sampling the network clock and draining
compressed video without preparing decoded output for a scene. Its started
script can service controls at the last actual qualified PTS while that clock
remains valid; it never creates another presentation timestamp. A short restore
with no receipt cannot erase that last actual timestamp. An unstarted hidden
panel returns explicit bootstrap deferral with no PTS or tasks, letting the
cluster start. Source-clock loss/epoch change still invalidates timing.

Reacquisition creates a fresh internal GPU window and requires actual mapped
feedback before visible work resumes. Old pending receipts are discarded;
submission tokens remain monotonic across window generations. This avoids both
hidden-window feedback timeout and illegally rebinding verification after theme
evaluation. Release callbacks must not throw. Destruction never accesses the
Runtime, which can already have been destroyed by the worker's Channel cleanup.

The [native camera appliance](NATIVE_CAMERA_APPLIANCE.md) supplies the shared/
exclusive camera lease contract. A retained on request still withdraws main.
The [resident Home route](NATIVE_HOME.md) uses a separate acknowledged handoff
and has live CM5 checks. Standard's [owned video output](STANDARD_VIDEO_OUTPUT.md)
also implements surface leases and passes an isolated native-camera handoff;
installed Standard camera admission remains closed. Automatic camera/fault-to-
Home recovery is unqualified, and the managed Home route rejects camera
combinations. Connected Ultra camera insertion uses the separate signed-scene
path described in [the camera guide](CAMERA_ULTRA_GUIDE.md).

## Component evidence

The Pi passed direct Wayland and desktop-preview owner tests using a synthetic
phone clock and real compositor feedback. They cover bootstrap without an early
PTS, scene presentation, a 3.1-second module-loading pause, clock suspension with
an outstanding scene, stale-receipt rejection, fresh recovery, and a terminal
clock step. The preview test additionally stops only its isolated compositor for
3.3 seconds and resumes it; missing feedback never advances the asset clock.
Worker tests also verify that absent display readiness blocks entry-point loading
and later script turns, while stop/status calls and the transport owner remain
responsive.
All 41 portable Mac tests passed, and the updated worker test passed with
AddressSanitizer/UndefinedBehaviorSanitizer. GPU owner tests ran on the Pi's
normal build; no new GPU sanitizer build was performed.

The private `headunit-theme-probe ... --worker-gpu LOCAL_PROFILE` starts both full
signed display scripts through the worker and GPU owner in an isolated Wayland
compositor. Its last run observed 4 center and 3 cluster scene presentations,
two outgoing UI messages and two verification declarations. It uses a synthetic
phone synchronization source, **real Wayland feedback**, an explicit two-second
offline scene-completion budget and a 5 Hz render cap. This is not a live phone,
physical two-panel, 60-fps or instrument-verification result. No Apple asset bytes
are included in the repository.

On **4 October 2026**, five isolated Linux/GPU output-fault and waiting-buffer
cases pass with real feedback and final-buffer pixel checks. These extend the
earlier component evidence, not physical native-backup qualification. Exact
counts, scope and retained evidence are in [output fault recovery](OUTPUT_FAULT_RECOVERY.md#cm5-linuxgpu-qualification--4-october-2026).

## Current receiver integration

The opt-in installed Ultra receiver routes negotiated main/cluster streams to
native decoder queues before asset startup and supplies qualified clock
snapshots to these display owners. Only receipts for scenes using a staged
input complete those frames. Main CarPlay, signed gauges/maps and touch have
separate live CM5 evidence; sustained reliability and full feature coverage
remain open. See [the Ultra guide](CARPLAY_ULTRA_GUIDE.md),
[native-input ownership](NATIVE_INPUT.md) and [the current handoff](HANDOFF.md).

X11/ordinary previews and synthetic `--worker-gpu` probes are separate paths.
Neither an offline probe nor successful local camera surface replacement
establishes phone-coordinated Punch-through, general asset transfer or physical
backup/failback.
