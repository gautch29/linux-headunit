<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Phone manager startup

Configure one persistent phone manager to own the receiver, while the native
backup cluster remains a separate service. This optional mode is disabled in
the example. On the current CM5 it is enabled through the immutable V3
[application release](APPLICATION_RELEASES.md), with an independently supervised
[Native Home page](NATIVE_HOME.md). The manager runs through the existing
`headunit-receiver.service`.

The retained release and all five service paths were verified on 4 October.
Earlier source-checkout tests confirmed Standard/Ultra switching and Home/Back;
physical workflows on the retained readiness release remain a separate gate.
See [the handoff](HANDOFF.md) for the current deployment and next action.

## Ownership

The manager acquires `manager.lock` and establishes its private `manager.sock`
**before** constructing the phone, media, audio or camera managers. A competing
manager is refused before those components can rewrite another owner's state.

Saved phone preferences use a pinned directory and nonblocking file descriptor.
Only bounded, trusted, singly linked regular files are parsed; replacement or
in-place mutation during the read is rejected. Duplicate JSON fields, excessive
nesting and malformed selected-phone values cannot become launch preferences.
The version-1 schema and legacy mode hints remain readable for retained rollback
releases; those hints still have no mode authority.

Receiver and optional fallback logs are opened nonblocking and checked as owned,
single-link mode-0600 regular files before changing ownership or appending.
An unsafe log fails that launch attempt instead of blocking the manager or
redirecting writes. Actual FIFO process fixtures cover preferences and log
admission; linked/oversized files and concurrent preference mutation/replacement
have separate portable regression checks. These checks start no radio/display.

The owned receiver launcher explicitly inherits the same locked file descriptor.
Other subprocesses, including configuration validators, do not inherit it.
Closing or killing the manager cannot release the lock while that launcher
still holds it. Normal shutdown retires owned children before closing the
endpoint. The optional system service uses `KillMode=control-group` so a failed
manager and its launcher/receiver children belong to the same cleanup boundary.

The source-only 15 October shutdown repair checks the existing stop event again
at main-loop request admission. A client accepted before INT, TERM or HUP that
completes its request after the actual handler sets that event receives the
existing error reply instead of entering `Manager.request`. The loop also stops
before admitting its next tick, so that turn cannot launch pending recovery.
The endpoint keeps its original three-second incoming/write budgets, packet
limit, account checks and serial execution. Shutdown still retains the endpoint
lock until the owned child has actually retired.

This gate does not preempt an operation already admitted before the signal.
Such an operation keeps its existing synchronous completion and cleanup rules;
a missing reply reader does not undo a complete dispatched request. Startup
auto-connect, blocked radio/kernel work and actual systemd control-group
retirement remain separate qualification. Manager generation numbers alone are
not replay identities across process restarts: old connected clients lose their
socket lifetime, and successors require a new endpoint connection and current
status. No reply establishes phone readiness or physical applied state.

The combined forward source `61b523f` passes all seven original manager suites
on Mac and GCC, including 11 retained real CLI retirement traces per platform.
These run with the Trip-distance and receiver-retirement changes in the same
frozen source. They do not qualify systemd, radio or phone shutdown.
[Combined scope, warnings and release gates](HANDOFF.md).

`manager.lock` remains an empty, owned, single-link mode-0600 file between runs.
Do not delete it to bypass a live owner. The endpoint also refuses a live legacy
listener that predates the lock. A leftover socket is reclaimed only after an
actual `ECONNREFUSED` result, with its owner, type, permissions and generation
still matching. Uncertain liveness or replaced files fail admission.

The unselected 9 October reliability candidate scans only each new receive
chunk for the request delimiter. Highly fragmented commands therefore avoid
repeated scans of the accumulated input. The existing absolute deadline,
1,500,000-byte limit, strict JSON validation and exactly-one-newline rule remain
in force. Fifteen endpoint fixtures pass, including a 131,072-byte value received
in 17-byte fragments. A synthetic in-process 1,499,800-byte value in eight-byte
fragments took 2.361 seconds before and 0.073 seconds after; this isolates parser
CPU work and does not measure actual socket or phone latency.

The lock protects this manager and its inherited launcher, not arbitrary
manually launched receivers. Do not start a direct lab receiver alongside the
managed receiver service. Cold-boot failure handling and complete systemd
termination/recovery still require target qualification.

When the owned launcher exits, the manager uses the same retirement path as
Disconnect before processing any media-adapter hold. It clears shared media,
iAP sidecars, audio/readiness ownership and the old receiver generation while
preserving the selected phone. An exited process cannot retain Home/Back or
style authority.

The optional `receiverRecoveryAttempts` policy allows zero to three recovery
launches after positive nonzero launcher exits. It defaults to zero, preserving
manual Connect after failure. Enabled recovery waits 2, 4 and 8 seconds between
successive failures, revalidates the current phone and retained inputs, then
uses the existing single-owner launch path with a new readiness nonce. Each
explicit Connect or configured boot launch receives one budget; readiness does
not refill it. Ordinary phone/session reconnect remains the running receiver's
responsibility and consumes no process-recovery attempt.

A clean exit, signal death, exhausted budget or failed recovery preflight/launch
requires manual intervention. Signal death may bypass the launcher's descendant
cleanup; whole-service retirement is required before retrying uncertain owners.
Disconnect, phone removal, selected-phone replacement, pairing admission and
shutdown cancel pending recovery. A retirement timeout keeps the old child and
blocks a replacement. The native Phones page shows **Retrying CarPlay…** and a
**Cancel retry** button without claiming an active session or enabling Back.
Cancellation carries the current manager generation; a delayed click cannot
stop a replacement launch.

## Configure startup

Start with [`config/phone-manager.example.json`](../config/phone-manager.example.json).
The five startup fields are required. The optional `pairing` object enables the
[Add phone flow](PHONE_PAIRING.md); it defaults off. Unknown fields and duplicate
JSON keys are rejected.

| Field | Meaning |
| --- | --- |
| `schemaVersion` | Integer `1`. |
| `enabled` | Explicit admission to managed startup. Default `false`. |
| `stateDirectory` | Canonical absolute private state directory, owned by the display account, mode 0700. Missing directories can be created at runtime. |
| `standardDisplayConfig` | Canonical absolute Standard display configuration. It can use a different main canvas from Ultra. |
| `autoConnectSelected` | Make one attempt to launch the saved selected phone's receiver after startup. Default `false`. |
| `receiverRecoveryAttempts` | Optional integer 0–3; bounded recovery launches after an owned launcher failure. Default `0`. |
| `pairing` | Optional exact object with Boolean `enabled` and controller `adapter`. The current receiver requires `hci0` when enabled. |

Profiles are bounded to 16 KiB and must be real, stable, single-link regular
files owned by root, the caller or the configured display account. Group/other
write and special mode bits are rejected. Profile field paths cannot contain spaces,
shell syntax, systemd specifiers, redirected ancestors or dot components.
The complete manager socket path must fit within 103 ASCII bytes.

The base receiver configuration must also contain:

```json
{
  "seconds": 0,
  "fallback_cluster": false,
  "media_state": "/var/lib/headunit/phone-manager/media.json"
}
```

These are additions to the existing private receiver settings, not a complete
receiver configuration. `seconds: 0` keeps the owned launcher persistent.
`media_state` must exactly equal `stateDirectory` plus `/media.json` so the
manager, native page and external backup cluster share one media snapshot.
The `fallback_cluster` field can be omitted; it must not be enabled because
`headunit-cluster.service` already owns that renderer.

Both display files are checked by the production C++ display parser. Main
views must use the same named output and Wayland socket. Both cluster views
must share their output, socket and canvas size with the external backup.
Main and cluster must target distinct outputs. Pixel differences between the
Standard main canvas and the signed Ultra main canvas are allowed.

## Validate without starting

For a prepared Linux checkout and an existing display UID:

```sh
python3 tools/phone_manager.py \
  --config /home/headunit/.config/ultra-headunit/lab/receiver-appliance.json \
  --manager-profile /home/headunit/.config/ultra-headunit/phone-manager.json \
  --uid 1000 --validate-only
```

Successful validation returns `configurationValid`, `enabled` and
`autoConnectSelected`. It reads configuration and runs the display checker;
it creates no runtime directory, endpoint, media snapshot or child receiver
and does not contact BlueZ. A disabled profile can be validated, but cannot be
selected for a running manager service.

## Select the service owner

Render the optional service bundle for review:

```sh
sudo python3 tools/install_cm5_appliance.py \
  --user headunit --repo /home/headunit/ultra-headunit \
  --receiver-config /home/headunit/.config/ultra-headunit/lab/receiver-appliance.json \
  --phone-manager-profile /home/headunit/.config/ultra-headunit/phone-manager.json
```

Add `--install` to save and enable that bundle using the existing recoverable
service-unit transaction. Installation does not start or restart a session.
The service name remains `headunit-receiver.service`; its direct launcher is
replaced by the manager. The compositor, output and external backup cluster
services keep their existing names and ownership. Optional vehicle fan-out
configuration is passed to the manager as before.

Startup checks the base receiver, then the manager profile, before constructing
the manager. The service restarts after failure with a bounded systemd start
rate. This is process supervision, not proof that an iPhone is connected or
that valid video has reached the displays.

If `autoConnectSelected` is enabled, the manager makes one boot attempt using
the saved selected phone. New source offers both configured capabilities and
uses the phone-negotiated mode; retained legacy hints have no authority. A failed attempt
keeps the preference and reports **Choose Connect to retry**. Receiver exit
uses the configured bounded recovery policy above; there is no indefinite child restart loop.
The existing preferred-phone selection, removal and phone-negotiation contracts are
documented in [Phone page and media](PHONE_PAGE_MEDIA.md).

## Show the native page

The page connects to `stateDirectory/manager.sock`. Run it as the display user
from its Wayland environment, using the launch example in
[Phone page and media](PHONE_PAGE_MEDIA.md#run-on-the-cm5).
An optional [Native Home profile](NATIVE_HOME.md) starts a resident page and a
main-screen Home button. Without it, this manager profile does not start GTK.
The separately enabled [Add phone flow](PHONE_PAIRING.md) supplies
incoming Bluetooth code confirmation; its actual phone qualification remains open.

## Current limits

- New Version 4 managed publication pins `mode_policy: phone-authoritative`
  and an independently retained boot helper. Version 3 includes verified static inputs,
  separate Standard geometry, runtime readiness and optional Home. Retained
  Version 2 bundles remain verifiable and execute their own sealed scripts;
  Version 1 retains the direct-receiver path.
- Managed/Home releases reject configured camera routes until Standard and
  Ultra camera ownership can follow live phone-mode changes. This profile does
  not activate the separate default-off manual camera API.
- Actual cold boot, preferred-phone reconnection, full manager termination/
  recovery, physical pairing/removal, repeated multi-phone operation and
  fallback/media continuity remain to qualify on the retained release.

## Verification

`phone-manager-client-retirement` combines the actual CLI loop, `Manager`,
private Unix endpoint and an owned tiny child inheriting the real manager lock.
Only privilege/account admission, configuration/display validation, BlueZ and
the receiver executable are substitutes. Its handler witness calls the original
CLI handler exactly once before acknowledging the signal; a partial connect is
completed only after that acknowledgement. The unmodified V87 source dispatches
that late request, spawns the child and ticks after each of INT, TERM and HUP.
The repair rejects it at admission and preserves ordinary completed requests.

The seven Python cases also cover closed reply readers without invented
readiness, same-manager stale generation rejection, malformed/oversized/EOF
input, the unchanged missing-newline budget, held-child shutdown ownership,
queued old clients across endpoint replacement, repeated old cleanup and loop
exception retirement. Original manager, endpoint, configuration, recovery,
readiness and native presentation suites remain unchanged. Scoped macOS and
Debian Python qualification is software component evidence; it does not execute
GTK/GPU, physical phone/radio/display, systemd or car paths. Original failures,
source identities, full Testing/JUnit and cleanup receipts remain outside Git
under `20261014/codex-manager-client-retirement-audit/`. The selected generation
is unchanged; whole-source review and the full coherent release gates still
precede any bench adoption.

Actual Unix/process fixtures cover competing owners, SIGKILL recovery,
inherited-lock retention, legacy listeners, socket replacement, unsafe files
and bounded strict requests. Configuration fixtures use real files and the
production C++ display checker. The production CLI is tested for side-effect-free
validation and its endpoint/signal loop; phone operations and root admission
are explicit fixture substitutes.

Earlier physical source-checkout tests confirmed both displays and touch,
Ultra Home/Back after ten seconds, Standard Home/Back and a round trip back to
Ultra. Their manager/page process IDs stayed unchanged, with no terminal UI
fault or cluster withdrawal. They establish that tested source-checkout path,
not physical acceptance of the current retained package.

The current V3 manifest, full guarded unit bundle and running service paths
pass read-only CM5 verification. Packaging does not prove an iPhone is connected,
that Home responds to touch, or that a speaker plays audio. Keep those evidence
boundaries separate when recording the remaining bench checks.

## Bounded recovery software qualification — 9 October 2026

The recovery fixture uses actual local children and the production manager's
launch/retirement path, with explicit phone and display-validation substitutes.
It checks budget exhaustion, deadlines, readiness without budget renewal,
fresh launch nonces, media/sidecar withdrawal, real inherited owner-lock
retention, cancellation, stale cancellation rejection, unsafe retirement,
preflight failure and abrupt launcher death. Retained-release fixtures verify
that the policy survives publication and cannot be overridden at runtime.
The actual GTK Phones fixture checks pending recovery and Cancel retry against
an isolated fake manager; this is software evidence, not phone or physical
radio/display recovery. V27 remains selected and does not enable this policy.

Final scoped checks pass on macOS and CM5 Linux: ten suites, 170 cases on each,
including the 13-case actual-child recovery fixture. The CM5 also passes the
actual GTK Phones/Music fixture under an isolated headless Weston compositor.
Its manager is synthetic; no bench output or radio owner changes. Existing GTK
deprecation diagnostics remain. A Linux release-fixture rerun with the documented
`umask 022` corrects an initial group-writable temporary-directory rejection;
the rejection log is retained. Source hashes match the final CM5 test inputs.
C++ formatting checks pass; no C++ binary rebuild or hosted CI run is claimed.

Logs and input hashes are outside Git at
`~/.cache/headunit-qualification/20261009/boot-recovery/`.
The final code/test archive SHA-256 is
`2ea107f86e82c9f44cca545a0c64b5047ab5a1b3ac4fe8325c209f9f20381e5e`.
CM5 transient inputs/logs are under `/tmp/headunit-boot-recovery-20261009/`;
the local receipt remains after a reboot. All five selected V27 services were
active with zero automatic restarts after testing. These checks do not establish
boot-speed improvement, real-phone recovery, physical fallback or cold-power
acceptance. GitHub builds remain deferred under the handoff's billing instruction.

## Launcher signal-death retirement

If the launcher dies from a signal, its process-group cleanup may never run.
The manager then latches a retirement error, withdraws the departed session's
readiness and media, and rejects new receiver, fallback, pairing, camera-On and
media ownership. The latch survives Disconnect and shutdown; status and the
resident local page remain usable. Forget rechecks retirement after stopping
so failed cleanup cannot delete the phone's retained credentials.

The next manager tick raises a fatal error. The managed receiver unit uses
`KillMode=control-group` and `Restart=on-failure`: systemd retires the whole
service control group before a successor manager can launch owners. A manual
source invocation requires the operator to retire its whole service boundary
before restarting; a launcher PID alone is not proof of descendant cleanup.
No broad PID search or process-group kill is added to the manager.

Portable process fixtures kill a real launcher while its real grandchild
remains alive, then verify admission rejection before the next tick, fatal
retirement, media withdrawal and cleanup availability. Additional substitutes
cover death during preflight/stop and between pairing admission polls. These
checks establish software containment, not physical phone recovery or actual
systemd restart qualification. Opt-in retry after a positive launcher failure
still relies on normal launcher cleanup; the default retry budget remains zero.

## Status after phone lookup — 14 October, source only

The production manager previously copied readiness, negotiated mode and Ultra
catalogue eligibility before synchronous BlueZ phone listing. That lookup can
take up to five seconds. Receiver death, readiness expiry or a new control
connection during it could leave old Ready in the returned status alongside
later unavailable media/audio or another connection's Home counters.

Status now completes phone listing before deriving those observations through
the existing child, selected-phone, launch-nonce, connection and original-date
checks. It does not refresh a producer date, preserve cached Ready or change
the 1.5-second readiness age. A current Ready connection still needs its own
catalogue; a prior connection's catalogue remains unavailable. Positive child
failure still retires on the next manager tick, with the existing optional
2/4/8-second recovery schedule. Signal death still latches the whole-service
retirement error and blocks replacements. Retry defaults, budgets, stop/reap,
locks and pairing admission are unchanged.

Five new regressions fail against unchanged qualified base `a140236`: actual
positive launcher exit, actual SIGKILL, original readiness expiry, a new
connection still starting and a new Ready connection with the old catalogue.
They use production Manager start/status/stop/tick, actual owned children and
private readiness/catalogue files; BlueZ, display validation and receiver
program are explicit substitutes. Only the expiry case advances an injected
fixture clock. The existing recovery checks retain cancellation, Disconnect,
removal, selected-phone replacement, pairing, failed preflight, retirement
timeout, inherited lock, budget exhaustion and stale-cancel rejection.

Focused macOS and isolated Debian ARM64 Python checks each pass **84/84**, with
zero skips: recovery 20, phone management 36, readiness 13 and endpoint 15.
Actual container INT/TERM/HUP cleanup proofs pass with client reap, retained
logs/source hashes and removal; all four owned containers are absent. No CMake
build tree was created. Pinned clang-format 19.1.7 and whitespace checks pass.
The existing GdkPixbuf PyGI warning is retained in both test logs.
These are source/process/file/socket fixtures, without compiled receiver,
GPU/GTK, real phone or full integration-matrix qualification.

For native UI, existing fields remain `generation`, `active`, `active_mode`,
`error`, `carplay.state`, `carplay.returnReady`, `recovery.phone`,
`recovery.remainingAttempts`, `phone_home.generation/connection/requests`,
`ultra_cluster` and `cluster_formats`. The existing Phones **Cancel retry**
button sends `disconnect` with the displayed `phone` and integer `generation`;
old-generation cancellation remains rejected. Connect/Remove use existing
`connect`/`forget` actions. New keys, actions, control/style IDs, widgets, pages
and visual pieces: **none**. No GTK/design/native page files change.

After manager whole-diff review, a new coherent complete required matrix and
new immutable generation, phone-session alone may qualify this on the bench.
First rerun the isolated owner fixture on exact integrated source; do not inject
fake readiness or a second BlueZ/display owner into the live appliance. In the
next bounded normal Disconnect/Connect check, retain exact release/helper/source,
current selected phone, manager generation, launch publisher and original
connection/receipt dates. Check withdrawal, fresh current return and rejection
of the old cancellation generation while preserving Home/Back, main/touch,
cluster/native fallback/Climate, both streams, UI-Sync, audio and control. Keep
the configured retry policy unchanged. Deliberate launcher signal-death testing
requires isolated whole-service containment qualification before any live use.
Ready is return eligibility; usable content, RF/USB/cold boot, optical first
frame and real phone recovery remain separate unqualified evidence.

Private baseline, exact source, focused checks and retirement receipts are
retained outside Git in `20261014/codex-reconnect-retirement/` under the
qualification cache. This repair does not alter frozen V86 qualification or
establish deployment; combined release qualification belongs to the manager
and sole phone-session owner.

## Bounded idle-cost fixture — 14 October 2026, source only

The original V87 phone-free observation reports receiver-service **MainPID**
CPU of **12.793% of one core** over 30.0155 seconds and 31 `/proc` samples.
It does not identify a function, thread, audio defect or frame-rate cause.
Phone-session separately verified a postboot `python3.13` executable with
`phone_manager.py` as entrypoint; that is not an identity receipt for the earlier
sampled PID. The selected generation and this source remain separate.

[`profile_manager_idle.py`](../tools/profile_manager_idle.py) provides a finite
local comparison. It creates its own private directory and subprocess, calls the
actual `Manager.tick` / `Manager.request` through the actual `ManagerEndpoint`,
and uses the production one-second tick and two-second status cadence. The
configured case starts the existing `SportSource` with labelled simulation and
includes audio-unavailable, native-page and greeting-preference status paths.
BlueZ is explicitly an empty fixture; no receiver, pairing, display, system
service or physical provider runs. Privileged CLI and sealed startup validation
are outside this measurement.

On Linux, `--native-fixture` accepts the existing owned
`headunit-prephone-native-fixture`. Its production `VehicleObservationCache`,
`SocketVehicle`, `PublishingVehicle` and 200 ms publisher exercise real private
IPC and unchanged peer credentials, exact argv/executable, pidfd, clock,
sequence, lease and retirement admission. Acquisitions are explicit simulated
fixture commands with genuine original steady-clock dates. This exercises the
publication path, not the actual native renderer or sealed role construction.

Each output directory must be new. CPU mode records independent process/main
thread CPU and monotonic wall intervals, Linux kernel thread counters and, when
present, the actual owned native child's `/proc` counters. Sport worker CPU is
explicitly its whole lifetime including startup and retirement. Source hashes
before/after, actual tick/status counts, interruption and retirement receipts
accompany the reports. Only completed unprofiled runs describe workload CPU.
`--mode profile` uses one cProfile instance per run. Its default times are
**wall time**, including waits. Observed Python 3.13/3.14 profiles contain calls
from both manager and sport threads; they provide no thread isolation or function
CPU attribution. The [CPython profiler](https://github.com/python/cpython/blob/v3.14.8/Modules/_lsprof.c)
uses interpreter monitoring callbacks; independent thread CPU counters retain
their separate scope. `--mode syscalls` uses Linux
`strace -f -c` only on the new fixture and descendants: aggregate counts include
startup/retirement and the C++ child, without payload/path traces. Instrumented
runs are separate and cannot stand in for unprofiled CPU.

Example local lane, with retained output outside a rebuildable tree:

```sh
~/.local/bin/headunit-build-slot --status
~/.local/bin/headunit-build-slot --name manager-idle-cost-local -- \
  python3 -B tools/profile_manager_idle.py \
    --workload configured --seconds 30 --output /absolute/new/idle-cpu
```

Use separate new outputs for `--mode profile` and Linux `--mode syscalls`. Duration is
bounded to 1–60 seconds. The tool accepts no existing manager PID, socket,
receiver profile or private phone input. SIGINT/TERM/HUP forward only to its own
process session; shutdown waits its source, joins SportSource and removes its
own endpoints/directory. The regression covers repeated signals, competing
owner rejection, invalid operation rejection and evidence overwrite refusal.
The Linux CTest additionally uses the actual native fixture.

Qualification from foundation `3670763`, with production runtime sources
unchanged: Mac recipe **3/3**, endpoint **15/15**, recovery **15/15** and media
observations **16/16** pass. Linux adds actual native owner **14/14** and native
recipe **3/3**, with the same endpoint/recovery/media checks passing. The strict
GCC `-O2 -Wall -Wextra -Wpedantic -Werror` build covers the existing native
fixture and its fifteen production source dependencies; this is a focused direct
build, not the full CMake/Skia/sanitizer matrix. Pinned format **19.1.7** passes.

| Completed unprofiled local workload | Actual wall interval | Process CPU, one core | Main thread CPU, one core |
| --- | --- | --- | --- |
| Mac sparse manager | 30.040 s | 0.641% | 0.641% |
| Mac configured manager + labelled sport | 30.023 s | 1.691% | 0.669% |
| Linux ARM64 VM configured + native publication | 30.042 s | 2.798% | 1.112% |

Each has 29 actual ticks and 15 status replies plus one rejected operation.
The separate C++ source child uses 19 kernel CPU ticks at 100 Hz over its
30.043 s bracket (about 0.632% of one core), outside the manager process total.
Ten-second cProfile and syscall runs remain separate; strace records 8,557
aggregate calls over its full fixture lifetime, including expected syscall
errors, startup and retirement. These are single bounded local fixtures, not
CM5-normalized costs, and do **not** reproduce or attribute V87's 12.793%.
No runtime optimization is claimed. New UI keys/actions/control IDs/pieces:
**none**.

A repeated native owner check exposed a test ordering fault: it acquired a
500 ms fuel sample before reconnect admission, then expected it to remain fresh
through retry. The production model correctly returned the original stale
acquisition. The corrected test first proves actual re-admission and preserved
stale dates, then sends a new acquisition and requires its newer date. No age,
retry, deadline or runtime policy changes. The original failure stays retained.
Actual repeated INT/TERM/HUP recipe checks and actual Docker cleanup proofs pass;
owned source children are waited, sport joins, endpoints withdraw, and every
owned build/container is retired after verified report retention.

Private evidence remains outside Git at
`~/.cache/headunit-qualification/20261014/codex-manager-idle-cost/`:
source snapshots, original failed logs, separate CPU/profile/syscall reports and
verified own-build/container retirement receipts. No CM5/phone/GPU/audio/fps or
physical-provider qualification is implied.

### Local Python owner mapping — 14 October, source only

Phone-session's later original V87 observation has 11 samples over
10.014445818 seconds, six stable native tasks and manager PID 815. At 100 Hz,
TID 980 advances 107 CPU ticks (10.6847% of one core), main 815 advances 12
(1.1983%) and Python task 952 advances 6 (0.5991%). **The Python task/role and
cause remain unknown.** Names, PID numbering, largest CPU deltas and final wait
channels cannot establish a Python owner. This worker takes no new bench sample.

The unchanged reviewed local report first preserved independent CPU counters
without a Python-owner/native-ID join. The host profiler now adds only
`threadOwners`, containing fixed `main` and `sport` entries. Main comes from the
actual `threading.current_thread()` and matching `get_native_id()`; Sport comes
from the actual retained `manager.sport.thread` object. No thread enumeration,
name heuristic, runtime Manager/Sport change or extra measurement thread is added.
Object IDs identify retained Python objects only within that one worker process.

Each entry copies `before`, `after` and `afterShutdown` observations, with
`observedMonotonicNs`, `objectId`, `alive`, `nativeId` and `kernelIdentity`.
These are original host-profiler observations in Python's monotonic domain,
in nanoseconds, separate from vehicle acquisitions and C++ source clocks.
An absent owner has null identity/liveness; a dead Thread has `alive=false` and
null current native/kernel identity even though Python retains its historical
`native_id`. Earlier observations keep their original dates and identities.
Main is still alive while writing the report; the parent separately waits the
actual worker process. Shutdown observation does not claim main already exited.

On Linux, kernel identity binds actual same-process PID/process start ticks,
native task ID/task start ticks and original observation dates. `kernelCpuBefore`
and `kernelCpuAfter` join only matching independently observed `/proc` records.
`kernelCpuInterval` additionally requires the same retained live Python object,
unchanged native/process/task identity and tick rate, increasing original dates
and nondecreasing CPU counters. It reports user/system tick deltas at
`ticksPerSecond`; missing rows, denial, retirement or identity mismatch yield
null joins/intervals. Unowned kernel rows keep no Python role assignment. On
macOS, actual Python native IDs remain available while kernel joins are null.

`independentCpu` for main copies the existing workload `thread_time_ns` interval.
For Sport it copies the existing worker lifetime counter and its actual entry/exit
Thread observations, checked against the retained source object. That lifetime
includes startup and retirement and is not arithmetically combined with kernel
workload intervals. cProfile remains aggregate **wall time**, with no function
CPU or thread isolation attribution. This mapping is **local fixture only**, with
no equivalence to V87 TID 980 and no runtime optimization claim.

Retained checks preserve the reviewed baseline before new identity, copy,
platform-missing, permission failure, dead/joined owner, deterministic record
reuse/mismatch, workload failure and repeated INT/TERM/HUP cases. Record mutation
tests reject mismatches; they do not claim the OS actually reused an ID. The
actual native IPC fixture keeps its original provider/freshness/retirement rules.
One initial Mac failure-path fixture used a redirected temporary path and was
corrected to its resolved private path; production guards remain unchanged.
Scoped final counts and source/retirement receipts are retained outside Git at
`~/.cache/headunit-qualification/20261014/codex-manager-thread-attribution/`.
Mac passes **5/5** focused suites (85 passed cases, one explicit Linux-only join
skip); strict Linux GCC and Clang 19 ASan+UBSan each pass **6/6** (100 cases,
no skips), adding the actual native IPC fixture. Its fifteen production C++
dependencies compile with unchanged strict warnings; Linux leak detection is
enabled without suppressions. This is a direct focused native fixture build
and CTest harness, not full project configuration or GPU qualification.
UI keys/actions/control IDs/new pieces: **none**.

The model is still unpublished. Root reviews the whole combined source before
any future sole phone-session owner adoption: all required coherent release
lanes, actual GTK/extra Trip gates where applicable, matching artifacts/helper,
configuration/module/argv/unit/input/asset identity and fresh full capacity/reserve
must pass before a new immutable generation. This source-only Python change
does not require a gratuitous C++/Skia full matrix for its focused host checks.

The future owner first runs the finite local command above in a new retained
directory (1–60 seconds), optionally with its own built native fixture on Linux.
Require fixed main/Sport object/native joins, unchanged source hashes, real owner
wait/join/withdrawal and platform nulls. It accepts no existing manager PID/socket
and is not a live appliance diagnostic. Keep actual BlueZ, pairing, camera,
native publication and status-client differences explicit. Further V87 causal
work requires a separately reviewed/authorized production identity boundary;
do not repeat the aggregate idle soak or use ptrace/Python sampling to substitute
for it. Preserve selected V87 and held V88, with no competing radio/display owner.
No physical, optical, phone, GPU, fresh-fps, audio or deployment claim follows.

## Read-only recovery schedule — 14 October, source only

`status.recovery.schedule` projects the existing manager scheduling fields after
synchronous phone lookup and receiver retirement guards, under the existing
owner lock. It uses the **same single `time.monotonic()` float-seconds sample**
already taken for status media freshness. Copying this model performs no I/O,
clock sampling, dispatch, launch, cancellation or budget consumption. The
original queued `recovery_at` is neither rounded nor renewed.

For native UI, this is software scheduling intent. It does not confirm a phone
connection, pairing, Ready, retry completion, applied state or useful pixels.
The existing `status.recovery.phone` and `remainingAttempts` retain their shape
and meaning: queued target and remaining budget, or null/zero without a queue.
`status.generation` remains the current cancellation/successor identity; no
second generation or source clock is introduced.

| Key under `status.recovery.schedule` | Meaning |
| --- | --- |
| `authority` | Always `software_scheduling_intent`. |
| `configuredMaximum` | Existing configured integer maximum, 0–3; default 0. |
| `queued` | Boolean: enabled policy with the existing scheduled deadline. This does not promise dispatch or successful preflight. |
| `state` | `disabled` for maximum 0; otherwise `queued` or `idle`. Idle means no scheduled process recovery, never connected or completed. |
| `clockDomain` | `python-time.monotonic`, or null if the supplied status observation clock is unknown/invalid. Scoped to this manager host lifetime; no boot, wall, phone or vehicle clock join. |
| `observedMonotonicSeconds` | Original status observation as a JSON float in seconds, or null. It dates the scheduling projection, not a car acquisition or receiver publication. |
| `deadlineMonotonicSeconds` | Actual original `recovery_at` float value/resolution/domain, or null without a queue, with disabled policy or unknown clock/deadline. No fabricated nanosecond precision. |
| `remainingWaitSeconds` | `max(0.0, deadline - observed)` in float seconds when known; otherwise null. An overdue **0.0** still means queued eligibility, never an attempt completed. |

Repeated observations may reduce wait while preserving the original deadline.
Equal observation dates do not renew anything. Returned dictionaries are copies;
mutating a reply cannot modify the queue. The existing page request-dispatch
bound and reply-freshness rules still apply: delayed IPC or a new redraw cannot
make an old reply authoritative. Do not locally extrapolate connection or
completion from this model. Only the existing owner tick may dispatch; existing
Disconnect/forget/selection/pairing/shutdown cancellation, 2/4/8-second backoff,
preflight, launcher cleanup, retirement quarantine and successor admission stay
unchanged. Readiness cannot replenish the budget.

**New actions, control IDs, widgets, pages, GTK pieces and style changes: none.**
Use existing generation-bound operations and existing UI components if adopting
this data. No new policy, service, wake source, persistence or radio owner exists.

Focused checks use production Manager and actual local owned child exits and a
private Unix endpoint, with explicit synthetic BlueZ/display validation/receiver
program substitutes. Injected scheduling clocks are **simulated fixtures**.
They cover original float precision, repeated/equal dates, immutable copies,
post-lookup mutations/cancellation, one reused clock sample, overdue reads
without dispatch, missing clocks, default zero/max three, positive/clean/signal
exit, queued/new-start generations, exhausted budget, failed preflight, shutdown
and existing cancellation/retirement paths. Readiness equal-date and source
withdrawal regressions remain separate checks. No compiled C++, GPU, sanitizer,
GTK, real phone, physical recovery or full release matrix is claimed.

Final focused macOS and Debian ARM64 runs each pass **109/109 cases**, zero
skips, across recovery 28, phone management 37, readiness 13, endpoint 15 and
configuration 16. Unchanged V87 first passes 80/80 cases on each platform.
The initial lifecycle fixture quarantine failure and the corrected runs remain
retained; no production retirement rule was relaxed. Final readiness retains
its original 20-second test limit. Current backend INT/TERM/HUP plus repeated
cleanup signals pass with actual client reap, verified before-delete retention
and container removal. All eight owned containers and temporary test trees are
absent; earlier backend proofs remain separately identified. Pinned format
19.1.7 and whitespace checks pass. Existing GdkPixbuf diagnostics are retained.

This model is still unpublished. Root reviews the whole combined diff; the sole
phone-session owner must first fix held V88 GTK behavior, qualify NEW coherent
source with every required release/GTK/helper/module/config/argv/unit/input/asset
check and fresh capacity/full reserve, then publish a new immutable generation.
First rerun the isolated process/endpoint fixture on that exact source. Query the
existing endpoint with `{"action":"status"}` and retain original reply receipt,
source/helper/release and manager generation privately. Default zero remains
disabled with null deadline/wait. In a separately admitted software fixture,
positive child exit plus existing tick retirement queues the original deadline;
repeated/overdue status alone cannot launch or spend budget. Allow the existing
tick, or use existing generation-bound cancellation, and verify unqueued nulls,
old-generation rejection, actual retirement and genuine successor identity.
Never inject fake readiness or another radio/display owner into the live bench.
Preserve main/touch/Home/Back/Climate, native formats/fallback, both signed panels,
streams, UI-Sync/audio/control. Actual RF/phone/optical/audio/fresh-fps evidence
remains separately unqualified. Exact never-executed owner recipe and all source,
logs, Testing, failed attempts and verified retirement manifests live outside Git
under `20261014/codex-reconnect-schedule-status/` in the qualification cache.
