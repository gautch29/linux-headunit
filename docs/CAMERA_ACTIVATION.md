<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Activate a camera from manual or vehicle state

Use `headunit-camera-controller` to drive the existing local camera view from
an independent manual request or a fresh reverse-gear observation. It uses the
shared vehicle interface and a separate native display owner. It does not
access car hardware, invent a gear reading or send an Ultra camera command.

The example profile is disabled. The controller and private file transport are
implemented; seven control/IPC suites pass on CM5 Linux as of **4 October 2026**.
Physical reverse acquisition, camera/focus behaviour and appliance deployment
remain unqualified. The controller targets the separate native camera owner,
not connected Ultra's signed Punch-through scene.

## Configure the controller

Start with [camera-controller.example.json](../config/camera-controller.example.json).
Every field is explicit. Restart the controller to reload a changed profile.

| Field | Meaning |
| --- | --- |
| `enabled` | Permit requests. A running disabled controller writes off. |
| `manual.enabled`, `manual.requestFile` | Read a separate private manual request. Manual on takes priority over reverse. |
| `reverse.enabled` | Permit the typed transmission observation to request a view. |
| `reverse.basis` | `engaged-gear` requires an actual engaged reverse gear; `selector` requires an observed reverse selector. Unknown components do not qualify. |
| `reverse.maximumAgeMs` | 30–1000 ms. Use the smaller of this age and the source's immutable transmission age policy. |
| `reverse.allowSimulation` | Default false. Opt in explicitly to a simulated transmission for bench checks. Unrelated simulated readings do not change transmission provenance. |
| `vehicleSocket` | Dedicated Unix datagram destination for unchanged broker HVO snapshots. Required when reverse is enabled. |
| `output.requestFile` | Managed request read by the native camera and cooperating main renderer. |
| `output.ownerFile` | Exclusive controller writer lock; this is separate from the main-surface lease. |
| `output.statusFile` | Native camera owner's HCS1 status file; pass this exact path as its `--status-file`. |
| `timing.requestLeaseMs` | 30–1000 ms maximum lifetime of each renewed on request. |
| `timing.statusMaximumAgeMs` | 30–1000 ms maximum age of the original native owner heartbeat. |
| `timing.startupTimeoutMs` | 250–10000 ms from the first published on to a new actual native presentation acknowledgement. Waiting heartbeats never restart it. |
| `timing.pollMs` | 1–50 ms, no more than one third of both request lease and status age. |

Paths are distinct, bounded, canonical absolute ASCII paths. The derived status
writer lock (`statusFile + ".owner"`) and vehicle source lock (`vehicleSocket +
".lock"`) must also remain separate. The controller rejects runtime aliases by
parent/file identity before creating its output. Profiles are safe regular files
of at most 8 KiB, owned by the user or root without group/other write access.

### Give reverse its own vehicle endpoint

`SocketVehicle` **binds and owns** its endpoint. It is not a subscriber to a
socket already owned by the cluster. Add the camera controller's dedicated path
to the vehicle broker's fanout destinations; keep the existing cluster/Ultra
destination unchanged. See [the vehicle adapter plan](VEHICLE_ADAPTER_PLAN.md).

HVO2 or later carries coherent transmission state, original acquisition dates,
age policy, source generation and provenance. The shared receiver checks
publication order and replay. Legacy HUV input without these metadata cannot
activate reverse. Repeated polls, broker publication and unrelated sensor
updates never make an old gear observation fresh. No RPM, speed or gearbox-type
inference is used.

## Run alongside the native owner

Create the explicit runtime parents as real user-owned `0700` directories. Do
not replace an ownership lock or reuse the main-surface lease as a controller
writer lock. No tool creates these directories automatically.

```text
headunit-camera-wayland \
  --camera-profile /absolute/path/camera.json \
  --display-config /absolute/path/displays.conf \
  --request-file /absolute/private/runtime/camera.request \
  --surface-lease-file /absolute/private/runtime/main-surface-lease \
  --status-file /absolute/private/runtime/native.status

headunit-camera-controller --profile /absolute/path/camera-controller.json
```

The controller's output request must match the native owner's request and the
cooperating main renderer's request. The main-surface lease remains that
renderer's separate shared/exclusive handoff contract. Enable `enabled` and
`manualActivation` in the native camera source profile as well. The managed
activation transport uses that existing local activation gate even when its
semantic request comes from reverse.

Both tools accept `--validate-only`. Controller validation checks configuration
without creating endpoints, binding a vehicle socket, acquiring locks or opening
a camera/display. It does not prove the configured paths or hardware exist.
`--run-seconds 1..3600` supplies a bounded controller bench run. Signals stop
the controller and publish off; SIGHUP exits instead of reloading.

The appliance installer does **not** install or enable this controller. Its
Standard-camera rejection remains in place. Component qualification did not
change an appliance service or camera profile.

## Understand requests and acknowledgement

Manual input accepts legacy `on\n` / `off\n` and the timed grammar below.
The controller always publishes timed on requests:

```text
on <positive host-monotonic deadline in nanoseconds>\n
off\n
```

These are actual newline-terminated records. Deadlines must use canonical
unsigned decimal, exceed the local current time and be at most one second in
the future. Readers recheck every poll; exact expiry means off. Each request
is an atomic replacement of bounded, user-owned, single-link `0600` data.
Missing/unsafe/malformed files or replaced parents mean off. A killed controller
cannot renew its request, and the readers let it expire without requiring its
destructor. Manual legacy on is an intentionally persistent operator request;
the controller's managed output is always timed.

Readers allow at most four attempts when a concurrent atomic renewal replaces
the inode with another independently validated safe file. Missing/unsafe input,
in-place mutation and exhausted retries fail closed. The activation API keeps
valid off/expired intent distinct from unreadable input: input loss suppresses
the view but cannot clear a failed-attempt latch. Only a valid off from the
failed request's source (manual off/expiry or a fresh non-reverse observation)
can reset it while no request is active.

For reverse, each outgoing deadline is:

```text
min(now + requestLeaseMs,
    original transmission acquisition + source age,
    original transmission acquisition + configured reverse age)
```

The native owner publishes bounded ASCII status:

```text
HCS1 <instance-start-ns> <sequence> <observed-ns> <phase>\n
```

| Phase | Native owner meaning |
| --- | --- |
| `idle` | Camera withdrawn and its exclusive surface lease released. |
| `waiting` | Requested, but lease/source retirement/first actual presentation is pending. |
| `active` | This activation has received an actual native presentation receipt. This may be an unavailable/stale status scene; it does not promise fresh camera pixels. |
| `fault` | Disabled request, terminal capture failure or display/ownership fault. A mapped camera is withdrawn before reporting a terminal source/display fault. |
| `stopped` | Surface withdrawn and exclusive lease released on shutdown. Driver retirement may still be finishing. |

The optional native heartbeat advances at approximately 10 ms and on phase
changes. The original `observed-ns` determines liveness; rereading the file does
not renew it. Missing, unsafe, expired or replayed status stays lost until a
strictly newer snapshot arrives. A reversed local clock permanently retires
the reader/session. All dates belong to the same host monotonic clock domain.
Production liveness polling samples real local time after reading the snapshot;
the session samples its comparison time afterward too. This avoids treating a
heartbeat published during file I/O as future data. Explicit-clock fixture APIs
retain strict future-date rejection. Neither path retimestamps the heartbeat.

The controller waits off until a fresh idle owner confirms withdrawal of any
previous camera surface. Old active/waiting heartbeats cannot qualify startup. Its initial on
records the owner instance and sequence. Only a later active snapshot from that
same instance acknowledges the new request. Startup timeout, owner restart,
heartbeat loss, terminal phase or later visibility loss publishes off and
latches the failed intended request. The real manual/reverse request must go
false before retrying; publishing off itself never clears this latch.

## Verify the control boundary

- `camera-activation`: strict schema, exact transmission component/provenance,
  shared-cache expiry, source generations and original acquisition deadlines.
- `camera-activation-session`: new-owner acknowledgement, anchored startup,
  request expiry, visibility loss and fault-latch policy.
- `camera-controller-io`: real temporary files, atomic replacement, ownership,
  original status age, replay/loss guards and cross-process writer locks.
- `camera-controller-integration`: actual controller process, real HVO2 Unix
  datagrams, manual/reverse requests, heartbeat/startup failures and SIGKILL
  request expiry. Its peer status publisher is an explicit fixture. The hard-kill
  case acknowledges the actual writer's SIGSTOP before reading its last positive
  bounded lease, then verifies SIGKILL, unchanged file bytes and expiry. A live
  writer can legitimately renew that file between a read and signal delivery;
  this synchronization preserves the original lease and expiry deadlines.
- `camera-control` and Standard launcher checks: legacy compatibility, canonical
  timed requests, expiry, bounded private endpoints and hard-link rejection.
- `camera-native-owner-model`: actual daemon control loop with explicitly fake
  display/provider. Checks expiry during cold startup, surface/lease withdrawal
  before blocked driver teardown, terminal capture failure and receipt-count
  gating. Its receipt counter is not GPU presentation evidence.

### Reverse owner-loss regression — 15 October 2026, source only

The actual controller-process fixture now checks one continuous reverse-sourced
failure and recovery. After a newer same-owner Active acknowledgement, expiry of
the original native heartbeat latches failure. Stale transmission, an unavailable
replacement source generation and fresh reverse preserve that latch. Only a
genuinely newer known non-reverse observation resets it; a subsequent reverse
requires a new acknowledgement. Normal shutdown publishes Off, reaps the process, removes its
vehicle socket and releases the output writer lock. Production policy, manual
precedence, defaults and deadlines are unchanged; no runtime defect was established.

Untouched V87 `3670763` first passes the seven selected CTests on macOS, Debian
trixie arm64 GCC and Clang 19 ASan+UBSan. The same seven pass on test source
`3dbcf8b` on all three platforms, with eight controller-process groups instead
of seven and no CTest skips. The selection is `camera-activation`,
`camera-activation-session`, `camera-controller-io`,
`camera-controller-integration`, `camera-control`, `camera-native-owner-model`
and `camera-native-configuration-model`. Linux uses strict warnings and the
existing sanitizer/leak policy. Both Mac builds retain nine external-library
link warnings about newer macOS targets; they are not warning-clean builds.

The new journey uses real local monotonic dates and private production IPC, with
generated measured-labelled HVO2 observations and an explicitly fake HCS1 owner.
These do not authenticate a physical provider or establish native surface leases,
GPU receipts, camera pixels, physical reverse, phone controls/focus or audio
continuity. The native-loop caller uses fake display/provider boundaries; GPU is
off. This standalone controller remains unadmitted to the managed appliance.
The existing signed Punch-through declarations remain read-only.

Source identities, original reports, packet/heartbeat traces, failures and
verified report copies/retirement receipts remain outside Git under
`20261014/codex-camera-activation-handover-audit/` in the qualification cache.
Whole-source review and the complete coherent release gates still precede any
new immutable generation; no bench action follows from this regression.

### CM5 control qualification — 4 October 2026

The seven controller/file/IPC suites pass on actual Linux with private files,
Unix sockets, controller processes, timed renewal and killed-owner expiry.
Their HCS1 presentation publisher is explicitly a fixture. See
[the control results](CAMERA_CONTROLS.md#cm5-control-qualification--4-october-2026)
for the exact suite list and evidence location.

The isolated [Standard/native GPU handoff](STANDARD_VIDEO_OUTPUT.md#cm5-linuxgpu-execution--4-october-2026)
is a separate direct-component result with real receipts. The updated native
daemon's combined GPU/HCS1 lifecycle has **not** been established. Neither
result qualifies physical reverse acquisition, real capture/import/fences,
native touchscreen exit or phone-coordinated Ultra transitions.

The opt-in [native controls](CAMERA_CONTROLS.md) add a renewed manual lease,
phone-independent Camera page and asynchronous local Close button. Physical
open/close/focus and end-to-end camera latency remain acceptance work.
