<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Connection-scoped Ultra script worker

`theme::script::UiWorker` owns the reused Channel, one Luau VM per display, and
UiTransport on a dedicated thread. Signed-theme loading, entry points, tasks,
callbacks, and their destruction stay on that thread. Receiver polling can
continue during startup. Production callers must supply a reader backed by an
immutable verified AssetBundle; give each worker its own resource cache.
The POSIX owner thread has an explicit 8 MiB stack. Memory checking exposed
stack exhaustion in the instrumented interpreter with macOS's smaller default
secondary-thread stack; the worker no longer depends on that default.

Each worker belongs to one negotiated stream ID. Reconnects require a fresh
worker. Configuration runs before asset evaluation so the owner can bind vehicle
data, negotiated video inputs, storage and verification-report collection. The
full `main` entry point and initial scheduler turn run for each display; this
does not replace a failing theme with a handshake-only fixture.

## Execution-budget fault location

The runtime preserves the existing CPU budget and bounded wall ceiling. On the
first interrupt that observes exhaustion, it saves up to four relative module
names and source lines from at most twelve stack frames, before a signed
protected-call wrapper can replace the reported line. Each name is limited to
128 ASCII path characters; unnamed entry points, absolute paths and raw script
text are omitted. The fixed records require no VM allocation or pushed values.
Subsequent interrupts cannot replace the first fault location. The terminal
error appends `budgetStack` alongside CPU/wall cost, budget and resume count.
This is fault attribution, not permission to continue an over-budget VM.

The 10 October regression runs an infinite module task through a protected-call
wrapper. The previous runtime fails the new location assertion; the repaired
portable check passes, as does macOS ASan+UBSan with leak detection disabled.
These checks retain terminal rejection and confirm that raw script text is
absent. Exact source `60dc41d` also passes the six-job local matrix and seven
affected CM5 suites under GCC (36.69 s, documented bench flags) and Clang 19
ASan+UBSan (40.48 s, Linux leak detection on). Retained V43 reproduces the real
Cobranded failure at 20,072 microseconds of CPU time with five task resumes.
The first fault stack is `APL/LuaUtils.lua:398`, `APL/LuaUtils.lua:170`,
`APL/Rect.lua:46`, `DigiBarMarkerGauge.lua:325`: object copying during marker
rectangle construction. This identifies the point where the accumulated turn
budget expires; it does not measure the distribution of cost across those
five tasks or qualify a performance repair. The VM and session retire.
Private baseline/final, sanitizer and real-phone receipts are in
`~/.cache/headunit-qualification/20261010/overnight/`.

## Execution-budget checkpoint cost

VM loop/call interrupts previously read the thread CPU clock at every
checkpoint. The 10 October source follow-up checks the wall ceiling at every
interrupt, but skips the CPU-clock syscall during the first half of the current
turn's elapsed wall budget. Thread CPU cannot exhaust the full budget in that
conservative interval. After it, every interrupt reads actual thread CPU time.
Completed and yielded VM turns always perform the exact CPU check, including
time spent in native callbacks. A missing CPU clock retains the strict wall
limit. CPU limits, accumulated task-turn accounting and retirement are unchanged.

A private synthetic CM5 benchmark links the strict optimized Clang runtime and
wraps `clock_gettime` only to count calls, forwarding every request to the real
clock. Copying a 64-field table 500 times takes 13.52–13.70 ms CPU with 33,004
CPU-clock reads before the change, versus 2.41–2.45 ms with two reads after it
(three trials each). The old runtime faults all six 1,000/2,000-copy trials at
20 ms; the changed runtime completes them in 4.70–4.94/9.46–9.72 ms. These are
own-code component fixtures, not signed-scene or live-frame qualification.
Portable checks retain protected-call infinite-loop rejection, scheduler-pause
tolerance and the blocked-callback wall ceiling. A native callback consuming
10 ms of real thread CPU is rejected under a 5 ms frame budget, and the VM
remains faulted. Exact source `4592c04` passes all six local matrix jobs and
CM5 GCC seven-suite checks (36.30 s), Clang 19 ASan+UBSan seven-suite checks
(40.50 s, Linux leak detection on), and strict optimized Clang nine-suite checks
(36.42 s). Seven production binaries and 750 source inputs are frozen for V45.

Selected V45 confirms a real Cobranded transition in a 0.895 s polling bound and
restores MapsMedia in 0.893 s on the same publisher. A separate six-style pass
confirms Cobranded again (0.897 s) and all other layouts, with both physical
output captures retained. This scopes the processing-budget repair to the
observed real-phone transitions; 20 ms, signed code, wall ceiling and terminal
fault behavior stay unchanged. Sustained fresh fps and every phone state remain
unqualified. Centre-content request acknowledgment has a separate failure
record in [cluster styles](CLUSTER_STYLE_REVIEW.md#v45-layout-and-content-checks).

The subsequent input-availability adapter exposed a separate compilation cost:
CM5 Clang ASan+UBSan repeatedly faults the first catalogue poll at about
20.28–20.37 ms CPU with no task resumes. A private phase-instrumented build
locates that poll; the fixed host adapter was lazily compiled inside its frame
turn. New source compiles it once during asset evaluation under the existing
startup budget. Normal catalogue/command turns only execute the retained
function, keeping their CPU/wall bounds and terminal fault policy. The same
live-selection fixture now uses a 5 ms normal-turn budget and passes normal
macOS and private CM5 Clang ASan+UBSan checks, including missing-input rejection
and requested-versus-active distinctions. Exact source `497ccc9` subsequently passes all six local jobs and CM5 GCC
fourteen-suite, Clang ASan+UBSan fourteen-suite and strict optimized Clang
fifteen-suite checks, including the isolated native page. The full private
headless startup fixture passes 21/21 checks. V46 reaches fresh
phone-confirmed readiness with a stable 20-second tail, but its separate
normalized input-catalogue bug requires V47. Selected V47 (`e0a3e56`) repeats
all six local matrix jobs, fourteen CM5 GCC and Clang ASan+UBSan suites,
fifteen strict optimized Clang checks and the 21-check private headless startup
fixture. All six styles, including Cobranded, confirm on the same live phone;
the 20 ms policy is unchanged. These checks do not qualify
sustained fresh fps or every phone state.

## Timing and display ownership

The optional `presentation` hook runs on the worker before evaluation, before
the initial task turn, and before subsequent script turns. The display owner
must return only its last actual compositor receipt mapped through the paired
media clock, or no timestamp when unavailable. It may pump bounded render and
feedback work. There is no default CPU-time clock or foreground assertion.
`DisplayTurn::allow_tasks=false` holds script execution while the owner keeps
pumping transport. The `bootstrap` phase must consume its initial receipt without
leaving a scene pending during other displays' module loading. Ordinarily, all
entry points load before any initial task turn. Each bootstrap wait is bounded
to ten seconds.
The `frame` phase permits ordinary scene submission. GPU_DISPLAY.md describes
the renderer now attached to this interface.

The reference theme requires an initial PTS even while not foregrounded:
LayoutController's first task concatenates it into a log message. It also emits
verification-region declarations during startup. Live startup therefore needs
the compositor/clock bootstrap and a report receiver before starting tasks.
Collecting declarations is not verification of actual pixels or gauge values.

An explicitly hidden main panel can return `defer_startup=true` in bootstrap,
with no timestamp, allowed tasks or inline work. The cluster may then finish
its own entry point and run while the main panel waits. Once ownership returns,
the main panel must obtain real presentation feedback before loading its entry
point and starting tasks, on separate ordinary owner turns. Ordinary missing
feedback does not opt into deferral or reset its ten-second deadline. Feedback
polls cannot initialize a deferred panel or authorize extra script ticks.

`State::running` means the owner loop is active. With explicit deferral, a main
entry point or first task turn can still be waiting for its own valid display.
It proves neither phone handshake, visibility, presentation, nor verification.
The display owner must still handle clock suspension, hide stale output and
select the fallback cluster on failure/disconnection.

Deferral does not change the per-display 64-message queue or the five-second
consumer-backpressure fault. A hidden, already-started panel can process control
tasks at its last actual PTS while its source clock remains qualified. This is
not a new presentation or a promise that arbitrary phone startup traffic can
wait indefinitely. See [native camera ownership](NATIVE_CAMERA_APPLIANCE.md).

The same bounded control-turn contract now supports opt-in cluster output-fault
isolation. A successfully retired cluster renderer returns normal owner turns
at its unchanged last actual PTS while that clock epoch remains ready; the
main VM and shared UI transport continue. Clock loss retires that timestamp
permanently. No message is skipped or acknowledged by the host to imitate the
failed output's script. Genuine VM/input/clock faults still end the worker;
GPU-driver hangs are not contained. The five new worker cases use synthetic
receipts, not physical fallback evidence. See
[the exact qualification and recovery contract](OUTPUT_FAULT_RECOVERY.md).

## Transport ownership and bounds

- Incoming mailbox: 64 messages, 2 MiB including headers, 512 KiB payload each.
  Messages can queue during initialization; a wrong stream ID is rejected.
- Outgoing mailbox: 32 messages, 2 MiB including headers. The runtime and
  encrypted socket queues have their own existing bounds in addition to these.
- An outgoing message becomes transport-owned when UiTransport enqueues it in
  this mailbox and calls `accepted()` on the same owner turn. Later script resets
  cannot recall already accepted transport bytes. Unaccepted adapter messages
  remain subject to its per-display generation check.
- `send_pending` offers the first complete message to a bounded nonblocking
  callback (normally `send_service_message`). False retains that exact message;
  true removes it once. Never reenter the worker from this callback.
- Undrained output and a continuously backpressured VM fail after five seconds.
  A malformed/unsupported input, script error or execution deadline is terminal.
  Diagnostics are bounded to 512 bytes and private: they may name asset resources.

`request_stop()` promptly closes ingress and clears queued output without
waiting for a running script turn. `finished()` becomes true only after VMs and
configuration/renderer captures have been released by their owner. A receiver
supervisor must retain stopped workers until then and reap them outside active
work; calling the joining destructor immediately during startup could stall its
polling thread. Trusted configuration/render callbacks must themselves be bounded.

## Socket lifecycle

AirplaySessionSetup now exposes an owner-thread `service_status()` snapshot.
It identifies negotiated, TCP-connected and closed registered DataStreams,
retaining the UUID and ID of closed services. A TCP connection alone does not
prove an authenticated application message. Closing a service discards its
queued messages, preserves its nonce seed reservation, and permits a replacement
with a new ID and seed. Snapshots include tombstones within the existing twelve
stream limit, so closure is observable even without an incoming UI message.

The receiver supervisor must dispatch only the matching service UUID/stream to
its worker, stop it on closure, and close the transport on worker failure. It
must preserve backpressure when dispatching messages from the socket inbox.
`UiSession` now implements this supervisor and an opt-in `control_probe` path
attaches it with verified installed resources and copied live clock anchors.
See UI_SESSION.md for the configuration and remaining negotiation/video gaps.
The normal phone session still runs the independent video previews.

## Validation

Synthetic tests exercise slow/gated initialization while mailbox calls continue,
count/byte bounds, both display routes, retained output under socket pressure,
stop during startup, malformed messages, startup and later task faults, and the
owner-thread presentation hook. Fixture timestamps are explicitly simulated.
Socket tests cover status transitions on negotiation/acceptance, selective
teardown, authentication failure, EOF and replacement with fresh IDs/seeds.

The private signed-asset probe accepts `--worker LOCAL_PROFILE`. It runs both
full entry points with an explicitly simulated initial PTS of zero, records
verification declarations, and keeps the ordinary 20 ms task budget. It does
not render, connect a phone, or establish live Ultra readiness. Missing timing
and report receivers caused explicit failures during development; those
dependencies must also be satisfied by the real display owner.

On 2026-09-26, all 41 portable Mac tests passed. The worker and UI transport
also passed AddressSanitizer/UndefinedBehaviorSanitizer with errors set to stop
the tests. Pi socket-lifecycle and worker tests passed, including the actual
five-second blocked-output expiry and cancellation inside the presentation hook.
The Pi's private signed-asset run completed both entry points, emitted two UI
messages and two verification declarations, and allowed 1,356 host status polls
during initialization plus one second of running. That poll count is from the
probe's host loop, not a network throughput measurement. The explicit simulated
initial PTS remained zero; this result does not establish live rendering.
