<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Independent receiver UI integration

The optional receiver consumer joins the encrypted UI DataStream to the script
worker and native video inputs. Combined with the explicit `--ultra-negotiate`
mode below, it is the integrated Ultra receiver used on the CM5 bench. Ordinary
CarPlay remains the source default; the installed consumer alone advertises no
new features.

As of 4 October 2026, the main and cluster views, touchscreen, and selected
native Home and Standard/Ultra transitions have live bench evidence. This does
not establish a complete Ultra implementation or sustained reliability. Start
with [the Ultra guide](CARPLAY_ULTRA_GUIDE.md) and
[the current handoff](HANDOFF.md) for the current capabilities and deployment.

## Ownership and failure handling

`UiSession` registers the UI service before SETUP. It observes TCP lifecycle,
creates one worker per fresh stream ID, and closes that service on worker
failure. The integrated receiver also ends the overall control connection on a
terminal UI/video failure, so replacement starts with fresh transport, decoder
and VM generations. EOF triggers cancellation even without another application
message.
Workers remain owned until their render/VM resources have finished destruction;
the receiver reaps them without joining an active startup on its network thread.
Retired-worker admission is bounded along with the existing connection limit.

Transport delivery retains a message until the worker accepts it. A full mailbox
pauses reads on that service; output flushing continues. Each turn handles at
most 32 messages per direction, with the existing byte limits and a five-second
input-stall deadline. A fresh stream receives fresh VMs and resource caches.
Overlapping renderer generations are rejected while an old worker is retiring.

`InstalledUi` verifies the private local bundle before the receiver listens.
Its factory supplies immutable resources, local vehicle bindings and two GPU
display owners on the worker thread. Clock anchors are copied under a mutex;
worker callbacks never access the receiver's sockets. Presentation uses actual
compositor receipts and qualified phone timing, with no invented startup PTS.

Native main-screen input crosses a bounded generation-scoped touch mailbox.
Its readiness descriptor wakes the receiver when reports arrive; the receiver
alone queues encrypted HID output. Backpressure consumes the notification but
retains reports for socket-readiness/timer retries. Cancellation wakes a release
and retired workers cannot revive a gesture. Optional timing diagnostics cover
only mailbox enqueue to HID queue acceptance. See [touch timing](TOUCH_LATENCY.md#receiver-mailbox-wakeup--1-october-2026).

## Standard Map startup and Ultra ownership — 12 October

V73 rejects `show_cluster_map` after actual `uiSync`/HEVC agreement. The signed
layout owns Ultra dynamic content; an unsolicited Standard Map URL no longer
changes its startup path. Standard, including a non-agreed dual-capability
session, retains its existing event/RECORD/accepted-cluster-frame admission.
The independent encrypted fixture receives a valid frame and checks the actual
Standard event payload, absence of an Ultra event, and EOF rejection. It fails
against the prior API. Live V73 logs confirm no legacy Map request, while signed
Map/style changes remain visible. This removes an ownership conflict; Music
still renders an empty page. It adds no new URI, scene ID or resource grant.

## Incoming transition timestamp compatibility — 11 October

V71/V72 normalize only authenticated incoming transition commands 1–3 whose
`data.pts` key is absent and whose `current_ntp_time` is a positive integer.
The observed signed controller expects seconds; the adapter derives them from
that original 32.32 NTP value, preserving the original field and other metadata.
It invents no receive clock. Existing `pts` of any type, invalid/missing NTP,
unrelated commands and outbound traffic remain unchanged. Bounded transport and
VM fixtures pass; real-phone transition-metadata errors are absent on V71/V72.
This is compatibility with observed private inputs, not a published Apple API.
Signed archives are unchanged, and this repair does not establish Music
availability. See [cluster review](CLUSTER_STYLE_REVIEW.md#large-now-playing-and-content-navigation--11-october).

## Local development configuration

Requires the Linux GPU and native-video build. Supply `--ultra-installed-ui /absolute/profile`
to `tools/run_lab_receiver.py`. The profile is an owned private regular file:

```ini
assetDirectory=/absolute/private/installed
assetIdentifier=44C9FA31EC
assetVersion=985
acceptanceReference=/absolute/private/accepted-reference.conf
vehicleProfile=/absolute/private/local-profile.plist
```

The bundle directory contains archive.zip, signature.der, certificate.pem and
trusted-root.pem. Prior-install references reopen only their exact accepted
bundle; they do not authorize a new expired package. Keep all these inputs out
of Git. The configuration is bounded and rejects duplicate/unknown fields,
relative paths and unsafe file ownership/permissions.

For a cluster-style bench review, `clusterStartupLayout=UPPERCASE-LAYOUT-UUID`
selects the pack's optional startup layout. Obtain an ID from that installed
pack's `headunit-theme-probe ... --describe-layouts` catalogue; IDs are
asset-specific. Omission preserves the pack's default. The override supplies
the optional `Config.json` through our host before the cluster entry point runs;
it does not modify the signed archive or the main display's configuration.
The startup override requires fresh VMs. Remote-map selection
then uses the selected layout's declared zones/components. This is a local
development selector, not a claim of implementing iPhone style preferences.

For live selection, add `clusterLayoutFile=/absolute/private/cluster-layout`.
The native page's **Cluster** view then lists the connected pack's primary
layouts. A click writes a private atomic selection; it does not restart the
receiver. The VM owner uses the installed controller's ordered UI exchange and
waits for the phone's layout transition. The page highlights confirmed active
state, rather than treating the click as a completed switch. Rapid choices
coalesce behind one in-flight transition; an unconfirmed request times out
after eight seconds and requires a new click to retry.

Catalogue status uses protocol version 2 with `nonce`, `connectionGeneration`,
`sequence` and `updatedMonotonicNs` alongside the layout fields. Decimal counters
and genuine `CLOCK_MONOTONIC` dates are encoded as strings. The manager admits only
its current receiver launch nonce/actual control connection generation during
fresh return-ready Ultra; catalogue age is bounded to three seconds independently
of file modification time. A button captures the rendered catalogue identity,
and the manager rejects old-button submissions before writing a request. The
private request is `HUL1 NONCE CONNECTION_GENERATION\nLAYOUT_UUID\n`, bounded to
128 bytes; the VM owner independently rejects wrong identities and legacy bare
UUID requests. Standalone diagnostic owners use a fresh token and an explicitly
unbound connection generation zero, which cannot match a ready managed session.

Retirement retains dated status bytes rather than conditionally unlinking a
shared pathname. The receiver's existing lifecycle withdraws readiness; old
retirement cannot erase or refresh its successor. Every VM consumes pre-existing
button requests at construction, with or without saved preferences. Portable
failure checks cover wrong launch/connection, stale/future publication, obsolete
button requests and preserved successor publication. These checks do not qualify
physical reconnect or all-pack switching.

Add `persistClusterLayout=true` to remember phone-confirmed local selections
(default false; requires `clusterLayoutFile`). A private owner-only
`FILE.confirmed.ASSET:VERSION` sidecar holds the last confirmed UUID for that
verified pack. Reconnection requests this preference through the ready session
and current catalogue; it never forces an unconfirmed local layout. Old button
requests are never replayed by a new VM. A rejected or timed-out click cannot
replace the saved preference, and loss of readiness cannot count as confirmation.
An atomic save failure keeps the accepted choice in this VM generation and
retries at most once a second while that choice remains ready, idle and declared
in the current catalogue. A later click alone cannot replace that retry candidate;
the phone must confirm its transition first. Palette retries also require that
the current pack still declares the confirmed colour. Withdrawal pauses the
retry without discarding its candidate; re-admission permits it to continue.
After a rejected restore or a failed native request, the settled scene becomes
the observation baseline. A later ready, idle, declared choice made through
CarPlay Customize can replace the saved preference even while the old native
request error remains diagnosed. A partial native result does not count as that
phone edit. These recovery paths have portable failure-injection coverage;
live all-pack recovery is still open.

Confirmed colours are also remembered independently for up to 32 layouts in a
private `FILE.confirmed.ASSET:VERSION.palettes` sidecar. The scope is the same
verified pack/version and cluster control path, with no phone identity added.
Its strict `HUP1` record rejects duplicate UUIDs, malformed colours, unknown
versions and excess entries as a whole. The existing last-selection file and
its local startup reader remain unchanged; its last confirmed choice seeds an
older installation's history without writing until a fresh confirmation.
Returning to a style through a native layout-only request offers its last
confirmed colour only while the current catalogue still declares it. Explicit
colour requests take precedence. The phone must confirm both the style and
requested colour; a partial response, rejection or timeout cannot replace the
history. A settled Customize edit can update it through the existing observation
rules. Older entries are evicted only after a qualified confirmation when the
32-layout capacity is reached.

History uses a separate bounded, coalescing disk worker with the same private
inode/directory admission and atomic replacement rules. Its maximum record is
4096 bytes; the original last-selection owner's 102-byte limit stays default.
History failures are reported through `preferenceError`, preserve the v1 last
selection, and retry at most once a second only while the paired layout/colour
remains ready, idle and catalogued. Portable and sanitizer fixtures cover
per-style restoration, scope isolation, compatibility withdrawal, partial
confirmation, corruption, migration, capacity eviction, disk failure and owner
retirement. These are component checks; connected-phone colour restoration and
power-loss durability remain to qualify.

The disk owner validates the read inode's size, modification and change dates
before and after acquisition, then rechecks the named file within its pinned
private directory. A same-size edit or filename replacement during startup is
diagnosed rather than restoring that unstable record. A production-reader
syscall fixture covers those races and interrupted-read recovery with real
private files; it does not qualify filesystem crash or power-loss behavior.

Confirmation also requires the signed workspace's explicit idle state; merely
being connected or no longer transitioning is insufficient. `confirmedActive`
is the last catalogue-valid ready/idle layout in this VM generation, separate
from the historical saved `confirmed` preference. The page uses it for its
highlight, so a local startup choice or aborted transition is not shown as a
new phone confirmation. New VMs start with no live confirmation. If the current ready catalogue removes an in-flight style while retaining a declared active layout, the owner retires that obsolete request immediately. The latest queued selection still waits for ready/idle/non-transitioning state. Removed live highlights clear without changing the historical saved preference. Actual C++ status snapshots pass through the unchanged native selector parser; the prior owner fails this catalogue-recovery regression. Portable and ASan/UBSan checks do not qualify live all-pack catalogue/reconnect behavior.

The configured startup style remains the initial/default style before restoration.
Writes use atomic replacement without synchronizing the filesystem on the render
owner; normal reconnect persistence is tested, abrupt-power-loss durability is
not promised. This is a pack/profile preference, not yet a per-phone preference.

With `diagnostics=true`, the installed runtime also emits bounded **asset-reported**
UI outcomes: reset/ready, sequence or identity rejection, layout request,
transition/watchdog and remote-video visibility. Each line contains a fixed
category, occurrence count, host monotonic time and connection/foreground flags.
Script text, private IDs and dictionaries are omitted. `diagnosticReports`
bounds events per panel (the runtime additionally caps repeat categories).
These reports help distinguish initial reset failure from a failed transition;
they do not independently prove ready state, video freshness or presentation.
Unknown pack log dialects remain silent.

The same default-off `diagnostics=true` switch enables an independent, fixed-size
command trace on the existing per-panel UI-Sync route. It inspects the already
decoded bounded plist; it creates no second session or focus owner. Each event
records panel index, actual VM message generation, command type, host monotonic
observation time and the shapes of these known fields: `tokenID`, `pts`,
`layoutID`, `activeComponents`, `targetComponents`, `presentStream`,
`dismissStream` and `isTransitioning`. Metadata commands may also report the
transition Boolean. Layout/component identities, arbitrary dictionary keys and
raw packets are omitted. Integer focus tokens appear only as
`focusTokenPrivate` in the private bench log for correlation; do not copy their
values into public evidence or Git.

`incoming_vm_queue` means the panel VM mailbox accepted the command, before
script execution. `outgoing_worker_queue` means the ordered worker output
mailbox accepted it, before encrypted transport transfer. Neither proves
script acceptance, peer receipt, camera controls or focus behavior. The trace
has a separate hard limit of 256 events per panel per transport lifetime;
the final event reports `budgetExhausted=1`. VM resets do not refill it, held
output/socket retries do not add events, and a fresh transport starts a fresh
budget. Optional sinks run on the existing owner thread and must be bounded,
nonblocking and nonreentrant; their exceptions are ignored. Unusual diagnostic
field shapes remain routed unchanged rather than becoming new protocol rules.
Portable transport/worker fixtures cover panel order, queue boundaries,
backpressure, malformed shapes, reset retirement, the cap and throwing sinks.
On 4 October those two suites passed both portable and actual CM5 builds; the
changed transport/worker sources also passed ASan/UBSan with linked dependencies
uninstrumented. The CM5 installed-UI/control-probe build retains the documented
global misleading-indentation and gauge-input-only maybe-uninitialized GCC
exceptions. Private source/binary hashes and outputs are retained under
`/home/headunit/.cache/headunit-qualification/20261004/ui-command-diagnostics/`.
The diagnostic tool was compiled without starting a live-phone run or changing
the retained appliance release; these checks do not qualify phone focus.
The [camera guide](CAMERA_ULTRA_GUIDE.md#focus-and-transition-evidence) explains
the observed focus schema and the remaining phone qualification.

This opt-in adapter currently understands the reference pack's
`LayoutController` dialect. It observes its constructor through a read-only
delegating host proxy, leaving the signed resource and returned controller
unchanged. It is not a universal pack interface. The directory must be owned
by the receiver user and not writable by other users. Selection files reject
symlinks, wrong owners, malformed IDs and unsafe permissions. A small atomic
`.status` snapshot expires from the page after three seconds. No caller-supplied
Lua or VM/socket ownership crosses this boundary. See the
[cluster style review](CLUSTER_STYLE_REVIEW.md).

`overlapScriptTick=true` is an optional rendering experiment (default false).
It runs the next script tick while the GPU finishes the already recorded frame,
using only the last accepted presentation timestamp. The GPU fence and
native-video lease still complete before the frame is submitted to Weston.
It improved main-panel throughput in one CM5 test but did not bring the
virtual cluster to 60 fps; see [measurements](ULTRA_PERFORMANCE.md).

The local profile also accepts `presentationQueueDepth=2` for both panels,
or `mainPresentationQueueDepth=2` and `clusterPresentationQueueDepth=2`
individually,
`feedbackPollMs=1..4` and `deferSceneFence=true` as independent opt-in
performance experiments (defaults: `1`, `0` and `false`). The queue assigns
each submitted scene its own actual Wayland receipt and keeps a staged native
video frame until a scene that used it is presented or cancelled. Deferred
fence cleanup retains imported decoder textures until the GPU signals
completion. Source defaults leave these options off. The current CM5 bench
explicitly enables them with opaque output: live DigiRing approaches 60 locally
animated scenes/s on both physical screens, while DigiBandTicks remains slower.
This does not qualify 60 fresh phone updates or physical touch latency; see
[the live measurements](ULTRA_PERFORMANCE.md#live-digiring-and-the-separate-touch-latency-gate).

`videoPresentationAdvanceMs=0..100` (default `0`) lets each panel stage a
decoded phone frame up to that long before its PTS is due. It applies to both
panels and is a touch-latency experiment; see
[TOUCH_LATENCY](TOUCH_LATENCY.md#presentation-advance--10-october-2026).
`displayResponseFile=/absolute/path` enables the native Settings page's
Smooth/Balanced/Fast choice, applied live over those release values; see
[TOUCH_LATENCY](TOUCH_LATENCY.md#display-response-setting).

Optional `centerWaylandDisplay` and `clusterWaylandDisplay` entries select separate
local Wayland socket names. Omission uses `WAYLAND_DISPLAY`. A one-screen bench
can place one panel on its physical compositor and the other on an explicit
headless compositor. The virtual output provides compositor presentation receipts,
not evidence of physical scanout. Two fullscreen panels on one kiosk output can
hide one another and exhaust the presentation deadline; do not disable that deadline.

Direct GPU output now obeys the compositor's acknowledged window dimensions.
Its final blit fits the original theme canvas with black bars, preserving aspect
ratio without changing script/video coordinates or adding CPU pixel copies.
The legacy shared-memory desktop preview remains a fixed-size diagnostic path.

This factory currently targets the reference pack's Center_Display and
Cluster_Display at 1920 × 720. These are theme coordinates, not a claim that
two physical panels are configured. It retains the normal GPU completion and
task budgets; the longer offline render-test deadline is not enabled here.

## Initial socket/worker evidence — 26 September 2026

On 2026-09-26 all 65 CM5 tests passed. The new socket/worker test uses encrypted
loopback messages to check 66 ordered requests and replies across mailbox
pressure, paused input, EOF, a fresh worker, a script fault and recovery with
another generation. The actual private installed bundle/profile passed receiver
startup validation on a separate unadvertised port without modifying CarPlay.
That startup check does not instantiate the GPU worker or verify live Ultra.

These initial checks did not prove live phone negotiation or rendering. The
dated bring-up below records the first phone tests; the current guide and handoff
summarize subsequent integration. General durable asset transfer and sustained
recovery remain incomplete. Apple's Climate and car-settings pages now have
separate live bench and file-driven configuration evidence in the
[vehicle-state guide](VEHICLE_STATE_GUIDE.md); this does not qualify camera resources.

## Native receiver video mode

`--ultra-native-video` additionally attaches `ReceiverVideo` before SETUP and
registers `iPhoneVideo` in each reference display before its asset entry point.
Type 110 feeds Center_Display; type 111 feeds Cluster_Display. Socket callbacks
move accepted packets into bounded queues. The worker alone creates and destroys
hardware decoders, runtime streams and GPU imports. No decoded CPU copy is added.
The ordinary preview is disabled in this explicit mode to avoid competing
surfaces/decoders; its existing capture observer remains bounded.

Both sources belong to one control session. Their queues may receive configuration
before the UI service starts. Only one worker can claim these endpoints;
a replacement UI after consumption requires a fresh overall connection. Screen
EOF/selective teardown now retires only that screen's transport generation.
A fresh-key video SETUP keeps the existing worker/Runtime stream and rebuilds
its decoder on the display thread. Old ingress and late receipts cannot revive
the retired image. The other screen and control RECORD remain owned. A closed
input endpoint, decoder failure or UI failure still cancels the shared owner;
the receiver stops its worker and closes UI/control. Real-phone automatic video
recovery, refresh ordering and fault-specific failback remain unqualified. Native main-window touch forwarding
is connected as described below and was confirmed by the user on the physical
CM5 touchscreen. This is separate
from the earlier ordinary CarPlay touch verification.

This flag alone does not advertise Ultra or alter ordinary preview negotiation.
Combined with `--ultra-installed-ui` and `--ultra-negotiate`, it supplies the
explicit HEVC/DPManaged path used in the live CM5 tests. It does not create a
software DMA-buf substitute. The ordinary preview remains a separate output route.

The CM5 `theme-receiver-video-wayland` test now verifies 12 synthetic HEVC frames
on **each** screen through encrypted loopback SETUP sockets, this router, separate
hardware decoders, native GPU inputs and actual Wayland receipts. It checks exact
source PTS, future-frame scheduling and duplicate owner rejection. Its updated
retirement checks preserve main on cluster EOF, preserve cluster on main teardown
during composition, reopen each with fresh IDs/configuration, and withdraw both
on terminal endpoint closure. The companion two-pending/deferred-fence fixture
passes too. [Native input](NATIVE_INPUT.md#per-screen-retirement-and-replacement--9-october-2026)
owns receipt/queue bounds and exact qualification scope. This is one compositor
surface with two test video layers, not two qualified physical panels or a
phone/theme handshake.

The private `headunit-installed-ui-probe PROFILE --synthetic-clock` also passed
using the actual verified installed factory: both full signed entry points ran
for at least one second, registered their native video sources and produced two
startup UI messages. It kept normal live scene/task budgets, with no offline
scene-budget override. No phone was connected to that probe; the clock was
explicitly simulated and no phone video was supplied to its empty inputs.

## Installed-asset negotiation

`--ultra-negotiate` requires both `--ultra-native-video` and
`--ultra-installed-ui`. This opt-in development mode has live bench evidence,
but is not a verified complete Ultra receiver. The reference profile selects
1920 × 720 geometry for both
logical screens. The physical screen is not reconfigured by this flag.

The Bluetooth iAP identity adds the explicit theme-support flag. After successful
MFi authentication and a positive wireless CarPlay availability message, the
engine sends CarPlayStartSession once with AP credentials, receiver endpoint and
identity, SDK interface version 19.0 and the **verified installed** asset ID and
version. Either this message or the classic Wi-Fi handoff permits the same iAP
engine to continue over the IP session. The installed SDK's binary protocol
archive provided the required parameter identifiers/types; no Apple source or
archive is included in the repository.

The capability reply adds DPManaged physical panels identified by the signed
asset's Center_Display and Cluster_Display names. Their initialVideoStreams map
to the separate receiver video UUIDs. It also adds showsInstruments and empty
uiSyncInfo and hevcInfo dictionaries. The real video dimensions and panel dimensions must agree.
Session SETUP accepts this mode only when the phone offers both uiSync and hevc,
and enables those features only after the UI and native-video consumers attach.
The installed SDK's HEVC feature accessor/parser confirms the literal hevc key.
No fileTransfer feature is enabled: a durable general installer is unfinished.
The test relies on the already-installed asset version, with exact prior-install
verification where applicable. A phone requiring transfer/new assets may fail.

Tests cover the default identity remaining unchanged, nested metadata encoding,
32-bit version/port representation, malformed availability, authenticated start,
no duplicate start, and rejection before authentication. Panel/video UUID mapping,
geometry, consumer attachment and required feature offers are also checked.
Live acceptance and rendered phone sessions are evidence separate from these
component checks. The following history records their initial bring-up.

### Initial phone bring-up — 26 September 2026

The failures, fixes and then-pending checks below are dated history. Later
physical main/cluster, input and native Home evidence is summarized in
[the current handoff](HANDOFF.md).

On September 26, 2026, the first CM5 phone attempt passed MFi authentication,
encrypted pairing, initial SETUP and RECORD. The phone accepted the enabled
uiSync/hevc feature list, then rejected `/info` because `hevcInfo` was absent.
Its diagnostic explicitly named that missing dictionary. The capability reply
now includes it, with a regression check. This progress does not establish a
working Ultra UI session; the attempt produced no video configurations or frames.

The next phone attempt accepted that correction and opened the Ultra UI channel
and both video streams. Renderer startup exposed a fullscreen size mismatch:
the kiosk compositor required 1280 × 720 while the client committed 1920 × 720.
The GPU sizing correction passes a new isolated kiosk test checking all pixels
of three aspect-preserved frames, actual presentation receipts, and explicit
socket selection. All 69 CM5 tests pass after this correction. With separate
physical/virtual outputs, the installed factory probe runs both complete scripts
and emits both startup messages with a synthetic clock. Live phone/theme output
still needs its own verification.

The next live message exposed a UI framing mismatch: the phone uses the
asynchronous `comm` DataStream type (`0x636f6d6d`), while the adapter had assumed
`cmnd`. The incoming UI adapter requires `comm`; outgoing messages use `cmnd`, as
confirmed later against our working bridge. Other service defaults are unchanged. Encrypted fragmented-message round trips and the 66-message socket
test check the corrected header in both directions. Message labels do not relax
the envelope, route, plist graph or mailbox checks.

Phone diagnostics then identified a separate layout error: generated receiver
UUIDs were being used as physical panel names, and iOS could not find those names
in its signed asset. Physical panels and UI routes now use the asset's names;
the video streams retain their independent UUIDs. The UI envelope's displayUUID
key accepts bounded panel identifiers only from the configured route set. Named
panels are case-sensitive; UUID-form routes retain their case-insensitive match.
Tests cover named-panel isolation, unknown names, case mismatches, bounds and
unchanged video UUID mapping. Live rendering remains under investigation.

The worker also retains its original failure before resource cleanup: a later
socket EOF or stop request must not erase the first error. A gated cleanup test
reproduces this race. Presentation-clock failure diagnostics include timestamps;
the existing freshness/continuity limits remain unchanged.

With named panels, the user saw the dials briefly on the physical CM5 screen.
The retained diagnostic reported a presentation timestamp 134,259 ns ahead of
the immediately sampled source clock. Wayland receipt processing now defers
such early arrivals until a real subsequent clock sample reaches that exact
timestamp. More than 2 ms early is rejected; no PTS is invented, no timestamp is
clamped, and the strict mapping and presentation deadline remain unchanged.
Tests verify defer/accept transitions, the upper bound and that strict mapping
still rejects the same receipt with a premature correlation. Sustained phone
operation after this correction needs live verification.

At 21:12 CEST on September 26, the user confirmed that dials stayed visible after
the early-receipt correction. The independent receiver's session remained open
past one minute without a UI-worker failure or video-consumer closure. Both
video streams negotiated and two incoming UI messages were accepted. Clock
synchronization briefly became unavailable and recovered; this is not a
long-duration reliability or frame-rate qualification. The physical cluster and
virtual main-panel arrangement remains a bench test. Embedded navigation and
theme input controls still require live verification.

The follow-up screen check found a white rectangle **behind** the gauges and
intermittent brief blanking. Decoded cluster samples from both the 21:11 and
21:22 sessions are black; they do not contain a hidden navigation image. The
installed cluster runtime now enables the existing signed-package map selector:
it fills only an unset zone with one unambiguous declared remote DCA_Map
component, retaining explicit selections. The receiver also queues one
`forceKeyFrame` event per connected video stream after the UI worker starts.
The phone acknowledged both requests, but the white rectangle persisted.
These changes are not an embedded-map success claim. The encrypted refresh
event test, session-setup tests and existing script selector tests pass.

A private installed-UI profile may set `diagnostics=true` to report at most
12 numeric display/video summaries per panel, five seconds apart, and one
bounded scene-layer summary. It logs counts, colors and geometry, not asset
source or captured pixels; it adds no per-frame CPU image copy. This is for
distinguishing missing/late video from local layer and clock problems.

The 21:28 diagnostic confirms that both assets create an opaque white root
background. Local scenes present, while video-presented counts remain zero.
Only the initial two phone UI messages arrive; receiver UI messages continue
outbound. Comparing our earlier working `vehicle-tunnel.cjs` reveals a
directional header mismatch: it receives `comm` and sends `cmnd`. The new adapter
had incorrectly sent `comm` too. Outbound is corrected to `cmnd`; encrypted
transport and socket tests now explicitly check different labels in each
direction. The white background is not overridden by the host.

After the outbound correction, the user confirmed the map between the gauges
in session `20260926-213209`. The phone responded beyond its initial two UI
messages and video frames continued arriving. At roughly ten seconds of
rendering, the cluster reported 248 scene presentations and 217 native-video
presentations; the main virtual panel also presented native video. Each panel
reported only its startup suspension at that point. These counters establish
actual native input presentation, not a 60 fps or long-duration qualification.
The physical screen confirmation is for the cluster; the main Ultra panel
remains on the virtual compositor. The user initially saw no flicker, but corrected that report after further
observation: intermittent flicker remained in that session. Through the first minute, both
panels retained only one startup suspension and the cluster reached 819
native-video presentations; the log later records clock loss and recovery.
Later timing diagnostics and frame measurements are recorded in
[Ultra performance](ULTRA_PERFORMANCE.md). Sustained operation and comprehensive
reconnection behavior remain unqualified.

## Native main-panel input

The installed factory enables Wayland seat input only for `Center_Display`.
Single-contact touch and primary pointer clicks use the existing independent
`PreviewTouch` mapper. Surface coordinates are logical, including buffer scaling;
the mapper fits the 1920 × 720 scene into the acknowledged surface, ignores
presses in black bars and clamps drags to scene bounds. Capability removal,
resize and touch cancellation release the contact. Cluster input is disabled.

`GpuDisplay` only accepts presses after an actual scene presentation with a
qualified clock. Clock suspension, output failure and owner destruction cancel
the gesture. A per-connection `TouchMailbox` carries copied HID reports to the
receiver; the worker never calls the socket session. It keeps at most 64 reports,
retains a report when `send_touch` cannot accept it, and ends an overloaded
gesture instead of replaying a long input backlog. Each worker gets a generation;
late reports and cancellation from a retired generation cannot change its successor.
A pending release is preserved across generation changes if the phone saw a press.

The receiver uses its existing authenticated event channel and touchscreen HID
identity. This change does not alter panel names, video UUIDs or negotiated HID
bounds. The user confirmed the native main picture and touch work on the CM5 after
the bootstrap correlation fix documented in CM5_BRINGUP.md. The receiver logged
its first native touch report and successful phone event replies. This is a
short live check of the main panel, not exhaustive control/layout validation.

Validation on 2026-09-26: Mac coordinate/mailbox tests and seven targeted CM5
checks pass (coordinate mapping, mailbox, AirPlay info/session/event channels,
and direct/preview GPU display lifecycle). The actual installed signed scripts
also start on two isolated headless GPU outputs with the input path enabled,
producing both startup messages. That probe uses a synthetic clock and no phone;
it does not inject a touch or verify physical scanout. Native input is dispatched
on worker display turns; its complete touch-to-display delay is unmeasured.

## Worker scheduling and cleanup

The worker uses a fixed 60 Hz turn schedule and skips missed slots. Work time
consumes the frame budget instead of preceding another complete interval of
sleep. The GPU draw schedule similarly starts from frame slots; receipt and
clock validation are unchanged. See TOUCH_LATENCY.md for the synthetic benchmark
and its limits.

The cleanup regression also exposed a libc++ ownership issue: moving a
`std::function` can leave a small target in its source. A second configuration
inside the thread startup object could therefore retain renderer captures until
after `finished()` became true. The worker now operates on its sole thread-owned
configuration by reference and clears it before publishing completion. The test
explicitly releases its caller-owned moved-from configuration, then checks that
fault details remain available when stop races with **unfinished** cleanup.
This regression and worker flow tests pass on both macOS and the CM5.

## Distinct frames and deferred GPU completion

Each pending scene records the exact native-video token it composed. Two
pending scenes may therefore prepare different due frames instead of repeating
one source until its first receipt. The live input has four bounded decoder
leases; clock suspension and worker replacement invalidate old staged tokens.
See [native-video ownership](NATIVE_VIDEO.md).

The optional deferred mode keeps separate immutable native-image holds and
completion fences for at most two GPU draws. Mutable Lua scenes stay on their
single owner thread. Releasing a GPU import does not acknowledge a phone frame;
only its actual accepted presentation does. Default completion remains
synchronous. The CM5 bench explicitly selects the experimental queue for the
live comparison: both views and touch passed a short phone check; sustained
performance and touch-to-photon latency remain unqualified. Current
heavy-style throughput remains below 60 fresh presented frames/s.
