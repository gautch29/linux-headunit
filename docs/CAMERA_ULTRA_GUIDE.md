<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Integrate a Punch-through camera

## V84 phone acceptance disposition — 14 October

Frozen `3bd3db9` manager Camera On 60s/Off retains both panels/PCM and owner identities,
but controlled continuous Music adds 6 On underruns (Off phases0 each): continuity
fails. [Audio](AUDIO_OUTPUT.md#v84-measured-camera-pcm--14-october) owns measurement.
Phone camera controls/Close/focus remain blocked by the current declaration and
synchronous confirmed-write contract described below; manager Off is not phone
Close. No guessed mandatory fields, queued-intent applied acknowledgement or
signed bundle edit is adopted. This is an engineering gap, not a user gesture.
An illuminated physical target/lens check and audible sound need operator/equipment
and are parked while the user sleeps. Full camera acceptance remains open.

## Deferred confirmation foundation — 14 October, source only

The source-only `camera-confirmation` change removes the synchronous-only
software boundary: the existing vehicle Service/Transport/Session can retain a
bounded owner operation and send the original `writeResponse` only after actual
completion. `CameraStateSnapshot` supplies the existing Off request/withdrawal/
current-scene presentation contract. An optional genuine nanosecond owner-clock
callback enables its provider confirmation seam; the ordinary factory call
retains its current behavior. The receiver never executes VM/GPU work here.

This does **not** enable camera controls. Parsed camera configuration still
requires read-only On for both aliases and rejects new fields. There is no JSON,
CLI, manager action, UI key, widget, CameraButton, UserDismissible or selection
admission in this change. The regression's manually constructed writable
CameraGeneral topology is explicitly synthetic; it is not a supported phone
capability recipe. Synchronous camera `apply` still rejects without dispatch.
Focus and touch retain the existing UI-Sync owner and guard.

`Provider::supports_confirmation` / `start_confirmation` are local C++ contracts.
The camera adapter accepts only false, retains exact generation/sequence and
uses the real accepted presentation date for its completion. Eligibility false,
queue acceptance, withdrawal alone and mismatched/late receipts cannot confirm
it. Channel loss cancels the exact operation even if its sibling survives;
disconnect/destruction retires outstanding work. A receipt produced before a
camera generation retires cannot be consumed as success afterward. Retiring an
old request cannot cancel its replacement. Independent observations remain free
to change current state; an older command confirmation cannot overwrite them.
[Vehicle-state confirmation](VEHICLE_STATE_GUIDE.md#deferred-owner-confirmation--14-october-source-only)
owns queue bounds, per-IID results, deadlines and response routing.

Component checks exercise production Service/Transport/ConfiguredModel and
CameraProvider with synthetic capture dates and explicit synthetic owner receipt
calls. The encrypted Linux Session fixture checks deferred response on the
original channel and selective origin cancellation. They establish software
behavior, not a new GPU receipt, phone acceptance, Close/focus, continuous audio
or fps result. Qualification logs, exact input hashes, failed attempts, Testing
and verified tree/container retirement remain outside Git under
`~/.cache/headunit-qualification/20261014/codex-camera-confirmation/`.

Initial `b739925` focused checks pass on their matching compiled inputs: Mac **6/6**, Linux GCC
**7/7**, and Linux Clang 19 ASan+UBSan **7/7**, including encrypted loopback.
Pinned clang-format 19.1.7 passes. Linux uses stock warnings/leak checks and
existing external LSan suppressions. Real Docker INT/TERM/HUP probes retain
verified evidence before stopping/removing their own containers. Earlier runner,
fixture compile failures and pre-reservation checks remain separate/excluded.
No full receiver/Skia or combined release matrix is claimed; the manager owns it.

### Timing admission correction — source review, 14 October

Manager review holds the original `b739925`: requiring the actual owner clock
and the earlier service tick to share a millisecond rejects normal dispatch
crossing that boundary. The production provider/model/transport regression
reproduces this rejection against the original source. The corrected adapter
uses the actual fine request date, accepts elapsed processing before the original
service-tick deadline, and retains one valid fine-clock frontier across polls
and replacement operations. Future service ticks, real fine-clock reversals,
expired admission and late consumption fail without extending that deadline.
The service's coarse lower bound is not operation identity: independent Off
readback and preceding receipts stay separate from the exact generation/sequence
and current-scene Off presentation. Presentation age still uses its original
date. [Precise clock contract](VEHICLE_STATE_GUIDE.md#deferred-owner-confirmation--14-october-source-only)
and revised checks/evidence are under private `codex-camera-confirmation/timing-fix/`;
original evidence/FINAL remain intact. This adds no writable capability or focus
admission and does not change V84.

The same review identifies an exceptional ownership gap after successful start
but before batch registration. A separate production allocation-fault regression
reproduces an unregistered camera request surviving that failure. Move-only
`PendingWrite` now owns cancellation through local batch/wrapper destruction;
camera callback storage is reserved before actual admission. Failed registration
retires exact camera intent and preserves preceding independent synchronous
results. Callback-allocation failures, wrapper failure and later transport
retirement likewise leave no orphan and cannot cancel a successor. The original
FINAL/evidence and both baseline failures remain separate from revised qualification.

Revised final focused checks pass on matching compiled inputs: Mac **6/6**, Linux
GCC **7/7**, and Clang 19 ASan+UBSan **7/7**, including the encrypted session suite.
Pinned formatting and stock warnings/leak checks pass; original runner signal
handling is unchanged and all owned trees/containers retire after verified retention.
These checks cover both review repairs; timing-only passes remain separate.
No combined required matrix, real GPU/phone control or deployment is claimed.

### Exact remaining admission facts and bench follow-up

Keep the existing signed bundle and read-only declaration. Before a phone-control
opt-in can be admitted, establish CameraGeneral writable-On permissions and
required/default fields; CameraButton/group mandatory schema; action Ready reset
and repeat semantics; the lost Close setter argument/actual On-false route; and
the actual camera component's transition/focus acknowledgements through the
current UI-Sync owner. Getter/setter identifiers in the pinned inventory alone
do not supply those facts. No global camera selector or second focus broker is
justified. These are explicit engineering parks; sleeping-user gestures cannot
resolve the missing software contracts.

For the sole phone-session owner after manager review: run the coherent required
matrix on the combined source before sealing a new immutable generation. With
unchanged read-only inputs, verify both camera aliases, manager On/Off and
failure withdrawal alongside both panels, streams, UI-Sync, audio/control and
unchanged owner identities. This is compatibility evidence only. Run the existing
private signed-GPU mailbox probe separately for actual Off receipts; its retained
previous run is historical. A new provider/phone-control fixture must explicitly
bind the genuine owner clock, original-channel response and full receipt dates,
and exercise pending-without-clock, timeout, selective channel loss and generation
retirement without claiming pending or manager Off as phone Close. Actual phone
controls/Close/focus require the missing admission facts and their own new-generation
qualification; continuous PCM and fresh60 remain independent open gates.

## Retained V83 follow-up — 14 October

Manager On60s/Off on frozen0fe2ef9 keeps both Ultra panels Ready and restores
Music with unchanged owner identities. The controlled fresh-PCM repeat records
three underruns during On; [audio evidence](AUDIO_OUTPUT.md#v83-real-phone-follow-up--14-october)
owns counters and timing. Phone controls/Close/focus remain an engineering gap,
not an operator test: the typed current declaration rejects writable On and
omits CameraButton/UserDismissible. Existing pinned first-party identifiers do
not establish the mandatory/default fields, action reset or camera focus route.
The selected V83 synchronous WriteBinding cannot turn queued owner intent into
confirmed applied state. The source-only foundation above does not change that
release or qualify phone declarations/focus. Keep the admission guard until those
contracts are established and qualified in a new generation. No claim
that the signed pack itself prohibits those controls is justified yet. Dark/noisy
camera pixels still require an illuminated physical target when an operator is
available; no captures or signed content are in Git.


Current source work generalises the local input adapter for vehicle views and
adds explicit USB fit/crop plus delayed-acquisition state recovery. See
[Vehicle views](VEHICLE_VIEWS.md) for the current contract and qualification.
Historical V68 retains the V5 managed activation/state/manual graph. Physical
USB pixels and managed entry/withdrawal were captured in the real CarPlay main
scene while cluster Ready remains; the phone dashboard returns without reconnect.
Exact selection/rollback lives in [HANDOFF](HANDOFF.md); scoped tests and remaining
phone Close/focus, audio/touch and USB recovery work live in
[Vehicle views](VEHICLE_VIEWS.md).

This guide records the local camera contract observed in the four inspected
Aston Martin family packs. It describes our independent implementation and its
remaining integration work; it is not an Apple specification or a claim of
complete camera support.

## Current state — updated 14 October 2026

| Route | Implemented and checked | Still to qualify |
| --- | --- | --- |
| Connected Ultra, camera inside the signed main scene | Real-iPhone managed physical USB On/Off; V68 physical unplug/replug and explicit recovery; 90-second camera/music-route coexistence, both panels Ready and unchanged owners | Remote UI controls/Close/focus, repeated unplug, isolated lease loss and active audio continuity; scoped evidence is in [Vehicle views](VEHICLE_VIEWS.md) |
| Phone-facing camera state, added in `86a4059` | Owner-sourced bridge; combined signed-GPU lifecycle; real-phone subscriptions to both exact aliases and outbound initial Off, On and Off | Peer consumption and coordinated phone controls/focus |
| Standard or phone-free native camera | Separate GPU owner, activation/IPC and owned Standard presenter; isolated CM5 handoff | Physical capture/touch, live-phone handoff and installed camera admission |

V82's 14 October phone session retains the current signed scene and both Ready
panels through manager On/Off with genuine PCM. In its 60-second On interval,
227 original fresh-output samples accept 6,000 additional blocks with zero new
underruns/discontinuities; the preceding 30-second Off interval has four of each.
All five owner identities/restart counts remain unchanged. Imagery is dark/noisy,
so an illuminated target/lens check remains operator-dependent. Phone controls
and Close are absent in the actual image. Current declarations omit CameraButtons
and UserDismissible, keep On read-only and retain the conservative touch guard.
This is a software/contract gap; manager Off is not phone Close/focus acceptance.
The pinned facts below do not establish mandatory/default/action-reset or
camera-specific focus behavior, and queued intent cannot be reported as applied.
Private original samples, phone logs and captures are in
`20261013/phone-session/`; V83's audio repair still needs its own live repeat.

Continue with the connected Ultra route below. Do not treat the successful
Standard/native surface replacement as proof of signed Ultra Punch-through.
The state bridge and explicit Off/On recovery are deployed in the managed V5
graph retained by V68. Real webcam driver detach/rebind and reentry preserve
the same registered camera Stream and both Ready panels. Source-status overlays
read the current queue generation. Portable failure/lease/receipt checks are
recorded in [camera providers](CAMERA_PROVIDER.md#stop-and-retire-a-source).
V68 separately passes one user-operated physical USB unplug/replug and explicit
Off/On recovery with 88 both-Ready samples. Worker loss reveals native fallback;
user-assisted reconnection restores fresh requests and rejects the old publisher.
A separate 90-second camera hold with user-started music retains both Ready
panels and the same available audio route in 203 samples; this does not measure
decoded/output PCM progress. One isolated content expiry passes with camera On,
but camera activation-lease expiry is separate. See Vehicle views. Phone-controlled
Close/focus, camera lease loss and decoded-audio continuity remain unqualified.
For the next task and current appliance release, read [the handoff](HANDOFF.md)
and [the goal](GOAL.md).

## Target the integrated Ultra path

The intended connected-phone feature is **Punch-through inside Ultra**, not a
native fullscreen camera replacing the Ultra main window. The existing Ultra
connection, per-panel UI-Sync, script runtimes, phone video/audio and cluster
continue. Local camera pixels become an input to the signed main scene;
Remote UI provides the coordinated camera controls, while the signed scripts
composite and animate the camera with the other layers. This follows
[Apple's public architecture](https://developer.apple.com/videos/play/wwdc2024/10111/).

Our `cameraProfile` route registers that input without releasing the Ultra
main surface. Its local capture/visibility adapter is implemented and has
isolated GPU/signed-scene evidence. An experimental phone-facing resource
configuration now mirrors that same source into two read-only camera states.
Its complete phone acceptance, camera controls and focus remain unfinished: local
visibility alone does not make the iPhone display camera controls. Main touches
are conservatively cancelled in this experimental route until those controls
are qualified.

The separate `nativeCameraControlFile`/lease route withdraws and rebuilds only
the main GPU window. It preserves the connection/runtime, but is **not evidence
of the intended integrated Punch-through experience**. Keep its Standard and
phone-free qualification separate. Do not close the Ultra acceptance gate on
successful native surface replacement, a registered input or a synthetic peer.

## Understand the two paths

The vehicle produces camera pixels locally. The phone provides the associated
remote controls. Apple's public architecture describes these as
`AutomakerInputStreams` and `AutomakerExteriorCamera`, including camera use
before the phone connects. [Apple's architecture session](https://developer.apple.com/videos/play/wwdc2024/10111/).

Our [camera provider](CAMERA_PROVIDER.md) supplies a bounded local frame and
view-lifecycle contract. A separate native GPU owner can present it without a
phone; the optional installed-host adapter registers it in the signed main
panel and publishes local visibility. The explicit V4L2 driver is implemented;
phone-side controls, physical capture and appliance view/focus qualification
remain separate steps.
Local registration alone does not advertise exterior-camera controls to iOS.

## Configure the experimental phone camera state

Added **4 October 2026**. The standalone lab receiver accepts a third semantic
capability file alongside Climate and car settings:

```json
{
  "camera_state_config": "/absolute/private/path/camera-state.json",
  "vehicle_state_backend": "simulation",
  "ultra_negotiate": true,
  "ultra_native_video": true,
  "ultra_installed_ui": "/absolute/private/path/installed-ui.conf"
}
```

Start with [the camera-state example](../config/camera-state.example.json).
The CLI equivalent is `--camera-state-config`; the direct receiver variable is
`HEADUNIT_CAMERA_STATE_CONFIG`. The checker accepts
`headunit-vehicle-config-check CLIMATE.json|- SETTINGS.json|- CAMERA.json|-`;
the final argument is optional for existing callers.

The installed UI must also have the enabled `cameraProfile`/`cameraControlFile`
route described below. Before serving the configuration, the receiver checks
that the state UID matches the registered signed input and that its lifetime
exceeds the source's frame-age limit. The example uses 151 ms for a 150 ms
source limit. The simulation backend still supplies other bench capabilities;
**camera state comes from the actual camera input owner, not the simulator**.
The retained V5 managed graph includes native Home and signed Punch-through
activation in its immutable configuration lifecycle. The separate main-surface
replacement experiment still requires its own owner/lease graph and is not
admitted alongside this signed input.

The current source also contains an owner-side Off foundation: one bounded
request is dated through camera withdrawal and an accepted, current-scene
zero-camera presentation receipt. Pending, timeout and retirement stay distinct;
old epochs/requests and missing receipts cannot confirm applied state. Portable
camera/display scheduling and actual Linux component fixtures pass. This adds
no writable phone resource, Remote UI focus policy or real GPU qualification.

## Resource topology and provenance

These identifiers are observations from a pinned, publicly available analysis
of the iOS 26.1 CarAccessory framework. They are **not an Apple-published wire
specification or evidence of phone acceptance**. Apple's public session
establishes the architecture and resource names, not the numeric identifiers.
[Framework analysis, pinned revision](https://github.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/tree/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework).
Our parser and state bridge are original code; no framework implementation is
included in the repository.

| Resource | Type | State/value |
| --- | --- | --- |
| AutomakerInputStreams accessory | `0x0C000001` | One configured critical input |
| CriticalInputStream service | `0x1C000001` | Registered camera stream |
| InputStreamUID characteristic | `0x44000000` | Fixed string, `camera` |
| On characteristic | `0x30000002` | Read-only Boolean |
| AutomakerExteriorCamera accessory | `0x01300001` | One camera general service |
| CameraGeneral service | `0x1F000001` | Camera eligibility state |
| On characteristic | `0x30000002` | Same Boolean source, distinct IID |

The exterior accessory literal has leading zero digits: **`0x01300001` is not
`0x13000001`**. Accessory, service and characteristic IIDs are allocated uniquely
inside each immutable configuration; they are not the type identifiers.
CameraButton actions, group selection and UserDismissible remain unadvertised. Read-only research on 6 October at the same pinned revision establishes the following additional interface facts:

| Field | Observed identifier/type or behavior |
| --- | --- |
| CameraButton service | `0x1F000002`, repeated services on the exterior-camera accessory |
| ButtonAction | `0x36000010`, uint8 setter; `0` Ready, `1` PerformAction |
| Selected | `0x36000064`, Boolean getter |
| SelectedEntryIndex | `0x30000061`, uint8 getter/setter; nonzero selects child at index minus one, zero/unavailable falls back to parent |
| UserDismissible | `0x36000063`, Boolean getter; enables the phone's Close button |

The [typed camera framework](https://raw.githubusercontent.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CAFCameraButton.mm) supplies identifiers/types; [enum names](https://raw.githubusercontent.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/System/Library/PrivateFrameworks/CarAccessoryFramework.framework/CarAccessoryFramework/CarAccessoryFramework.mm) establish action values. The [phone camera consumer](https://raw.githubusercontent.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/Applications/CarCamera.app/CarCamera_01.mm) sends PerformAction and disables an action button while the observed action is nonzero; it does not locally reset Ready in the inspected branch. Its Close callback uses the On setter, but that specific argument is lost in the analysis; a separate successful content-URL callback explicitly sets On false. [Camera model analysis](https://raw.githubusercontent.com/EthanArbuckle/iPhone18-3_26.1_23B85_Restore/90aa0cfe59d9682b4265e1354c8b19ec3c7823ab/Applications/CarCamera.app/CarCamera.mm) establishes child selection and optional close-control construction. No zero-based global camera selector is justified.

Generic writes retain pending state until decoded observations arrive; transport acceptance is not applied camera state. Mandatory field/permission defaults, action reset timing, repeated-action semantics and signed-scene focus remain unresolved. Any asynchronous camera control needs the existing owner to confirm its applied result; the synchronous Provider/WriteBinding contract cannot acknowledge queue acceptance as applied. The source-only deferred confirmation seam above preserves that guard. Keep read-only resources and the touch guard until this route and phone controls are qualified. Private fact manifests pin 19 inspected files, method offsets and hashes; no implementation or phone capture is included in Git.

### Focus and transition evidence

A read-only private audit on 4 October of the verified regular version-985
pack's `LayoutController`, `FocusToken`, `FocusSystem` and `Layout` established
the following generic UI-Sync command shapes. The signed resources were not
modified or copied into this repository. These are observed pack contracts,
not an Apple specification or proof that the iPhone took a camera transition.

| Observed command | Data fields used by the inspected pack |
| --- | --- |
| GiveFocus (`0x07`), RequestFocus (`0x08`) | Dictionary with numeric `tokenID` |
| Start zone (`0x01`) | `targetComponents`, `presentStream`, `dismissStream` |
| Start layout (`0x03`) | `layoutID`, `activeComponents` |
| End zone (`0x04`) | `pts` |
| End layout (`0x06`) | `layoutID`, `pts` |

The pack's existing focus owner checks its current token before acquiring or
releasing local focus. When connected, local focus items lead it to request
focus; their absence leads it to give focus. Disconnected acquisition is local.
No additional host focus owner is justified by this evidence. The audit did not
identify a camera-specific remote component or establish camera button handling,
the phone's transition sequence, focus acknowledgement or the visible touch
route. Generic map/assistance transitions do not establish those camera contracts.

The default-off [UI command trace](UI_SESSION.md#local-development-configuration)
can make the next actual iPhone On/Off sequence reviewable without altering
focus or touch behavior. Its field shapes and private token correlation observe
incoming VM and outgoing worker **queue acceptance** only. Keep raw logs private;
publish sanitized order/shape findings without token, layout or component values.
Record camera On and Off alongside both panels' scene/video progress and the
existing resource-transfer evidence, then separately verify visible phone
controls, input/focus entry and restoration, and active audio continuity. An
exhausted diagnostic budget means later commands were not observed; it cannot
be interpreted as a missing phone response. Until that real-phone evidence
establishes the coordinated route, keep the conservative camera touch guard and
leave writable controls unadvertised.

### Bounded resource evidence

The receiver's optional fixed two-resource trace records authenticated accepted
reads/subscriptions and Boolean responses/notifications accepted by the outbound
session transport. It shares one trace across the existing two priority ports;
no raw commands, private IIDs or values other than these camera Booleans are logged.
Queued output, transport ACKs and blocked retries do not count as transferred.
Registrations may be wildcard requests; counters prove exact IID inclusion
in the accepted response, not camera-specific phone intent. These counters do
**not** establish peer receipt, visible controls or UI focus.
Portable transport fixtures and actual Linux encrypted loopback verify paired
initial Off, On and Off notifications, backpressure and rejected commands.

The bounded repeat with a real iPhone on 4 October completed the resource
cycle: **each** alias registered once, transferred its initial Off response,
then transferred one On and one Off notification, with zero resource errors.
Both signed scene counts and both unique phone-video presentation counts
advanced across each phase. Main/cluster scenes progressed 304/305 → 590/595 →
848/859, and phone presentations 64/73 → 68/172 → 90/259. The simulated camera
was visible during On, presented 207 frames and was false after Off; there were
no surface withdrawals or clock-unavailable events during these snapshots.
These counts do not qualify 60 fps or visible phone camera controls. Audio was
disabled; the earlier manual test separately confirmed camera pattern and touch.
The user confirmed both panels during the repeat, and all five V3 services
returned active afterward with the sealed release preserved.

Private phase/counter snapshots and log/binary hashes are in
`live-resource-repeat/resource-summary.json` beneath
`/home/headunit/headunit-camera-state-gpu-20261004/`. The preceding 150-second run
was partial: the phone connected near its deadline, both On notifications were
transferred but Off was not checked before shutdown. Its separate evidence is
`live-resource-trace/resource-final-summary.json`; it is not counted as another
successful full cycle.

### Preserve source state across owners

`CameraStateSnapshot` carries one copied observation per connection. The signed
camera input first applies its existing local `CameraVisibility` validation,
then publishes that result into the snapshot. The receiver thread reads one
binding for both phone On characteristics; it alone operates the vehicle-data
service and its encrypted transport. No GPU object, script runtime, socket or
mutable service crosses the worker boundary.

True carries the original capture timestamp. Heartbeats and repeated reads
cannot refresh it. Missing/stale sources, lost owner timing and worker retirement
produce explicit false, including an Off transition in the same millisecond
as the last true. Receiver freshness failures are negative decisions dated
at the actual receiver observation; they are never represented as new captures.
Camera state is checked on each receiver turn, independently of the normal
100 ms Climate/settings polling cadence. Generation checks prevent an old
worker from reviving or clearing its replacement; connection closure prevents
any later worker from reopening that state.

This state describes **fresh local camera eligibility**, not a physical scanout
receipt, completed phone transition or acknowledged camera command. The current
profile starts false and rejects phone writes. The conservative camera touch
guard remains until Remote UI camera controls and focus have been verified.

### Use a native surface before the phone connects

The optional [native camera executable](NATIVE_CAMERA_APPLIANCE.md) can own the
main output with no phone clock. Its private installed-UI handoff uses a distinct
pair of fields:

```ini
nativeCameraControlFile=/absolute/private/directory/camera-request
nativeCameraLeaseFile=/absolute/private/directory/main-surface-lease
```

These fields coordinate a main-only surface handoff, rather than registering a
signed camera layer. Do not combine this route with an enabled `cameraProfile`.
The [native appliance guide](NATIVE_CAMERA_APPLIANCE.md) owns its lease ordering,
configuration and recovery contract. Standard's opt-in owned presenter now
implements the same handoff and passes an isolated CM5 GPU/decoder test;
[Standard camera admission](STANDARD_VIDEO_OUTPUT.md) remains closed pending
physical qualification.

The [resident native Home route](NATIVE_HOME.md) is implemented and separately
live-tested. V5 managed releases admit its integrated signed Punch-through
route; the separate surface-replacement route remains excluded. Phone Close and
automatic focus return still need qualification. A camera exit may restore only
a still-valid underlying view. Neither route establishes Apple's camera controls.

## Configure the installed host

Add these optional entries to the private installed-UI profile:

```ini
cameraProfile=/absolute/private/path/camera.json
cameraControlFile=/absolute/private/directory/camera-request
```

Start from the disabled [reference template](../config/camera-ultra-reference.example.json).
An enabled profile requires `manualActivation=true`, the inspected main slot
below, and `deferSceneFence=false`. The first camera integration deliberately
uses synchronous GPU completion so a capture expiring during a cold draw can
be removed and the unavailable scene redrawn before swapping. Existing
camera-free configurations keep their own queue/fence policy.

Only the four inspected pack identity/version combinations are admitted to this
slot; an unrelated archive needs a new explicit contract review. Ultra currently
rejects nonzero rotation and mirroring instead of silently ignoring them. The
independent native camera scene already supports those transforms; conversion
into the signed slot still needs a capture adapter.

Merge [the two example local bindings](../config/camera-script-bindings.example.plist)
into the configured vehicle script profile, choosing free **local model** IIDs.
This example is a camera-only fragment, not a replacement for gauge bindings or
the phone-facing vehicle-data profile. The fixed UID and timed Boolean must
share a repeated instance. The Boolean's lifetime must exceed camera freshness
by at least one millisecond for the service's whole-millisecond expiry rule.

The control file's real parent must be owned and mode `0700`; the file must be
owned, single-link, regular and mode `0600`. Atomically replace its contents
with legacy `on\n` or a canonical `on <host-monotonic deadline-ns>\n` to request
the camera, or `off\n` to leave. Timed requests expire and cannot exceed one
second from the reader's current local time. Missing/unsafe/malformed requests
mean off; no previous on request is remembered. The optional independent
[activation controller](CAMERA_ACTIVATION.md) implements manual/fresh reverse
policy for the native route. Phone-controlled signed-camera activation remains
separate and unqualified; no reverse state is inferred from this file.

The main display polls the request on its owner thread. It registers the camera
stream before script entry-point evaluation and creates a fresh bounded source
generation. Source loss or clock suspension clears both camera queues and
publishes false. A native label identifies simulation or unavailable video.
During this experimental local-camera view, main touches are cancelled rather
than sent into the covered CarPlay app. Verified exterior-camera controls and
focus routing remain unfinished. The instrument panel is independent.

## Register a local input

The installed host ABI distinguishes a camera from the incoming phone picture:

| Input property | Camera contract supported by our host |
| --- | --- |
| `identifier` | `camera`, matching the inspected slot |
| `type` | `video::InputType::camera`, authored value 0 |
| `width`, `height` | Delivered queue bounds; 1650 × 720 matches this slot exactly. Optional V4L2 `source.adaptation` declares acquisition geometry separately; see [Vehicle views](VEHICLE_VIEWS.md#usb-camera-adaptation) |
| `frameDuration` | Actual configured provider cadence |
| `streamMode` | Buffered, value 0 |
| `parameterMode` | Incoming, value 1 |
| Opacity, position and size capability | Fixed, value 0 |

`Runtime::add_video_input` accepts this typed metadata before entry-point
evaluation. Existing phone inputs default to `iphone_video` (value 3). Unknown
types are rejected. Both stream types share the existing bounded video-layer
ownership and presentation rules. An enum value is a local script ABI value,
not a phone accessory, service or characteristic type identifier.

The inspected scripts also declare settings, widget and road-sign input kinds.
Their existence does not establish implemented phone controls for those views.
Detached streams and dynamic input capabilities are not advertised by this
host implementation.

## Match the camera canvas

The verified declarations in both substantive packs and their CN variants use:

| Item | Declaration |
| --- | --- |
| Panel | `Center_Display`, 1920 × 720 |
| Layout | Automatically attached `Overlay` |
| Camera rectangle | x=135, y=0, width=1650, height=720 |
| Input identifier | `camera` |
| Vehicle updater identifier | `punchThrough` |

The signed layout centers the input's source bounds inside the rectangle. It
does not scale them to fit. Use the disabled
[reference camera configuration](../config/camera-ultra-reference.example.json)
as the starting geometry. A physical camera with a different aspect ratio needs
an explicit fit/crop policy in a capture-conversion adapter. Preserve the
separate negotiated phone-video dimensions.

## Publish visibility

The inspected local vehicle updater consumes this semantic path:

`automakerInputStreams.criticalInputStream.*/inputStreamUID.on`

Each stream instance contributes two bindings:

| Field | Local value |
| --- | --- |
| `inputStreamUID` | Fixed string matching the registered input, `camera` |
| `on` | An explicit Boolean indicating requested, fresh local camera eligibility |

UID and visibility must share the same `ScriptBinding.instance`. The service
uses a repeated-instance array even for one camera. Local binding IIDs belong
to our model and do not reveal Apple's numeric phone-facing type identifiers.

The Overlay is attached independently of the primary cluster layout. Camera
visibility subscribes or unsubscribes its video layer; it is not a cluster-style
request. Keep camera visibility separate from live cluster selection.

On source loss, stale capture or a reversed clock, explicitly set visibility
false and clear retained camera frames. Do not substitute `CP.INVALIDVALUE`:
the inspected visibility code treats that userdata as true. Source availability,
capture age and requested activation remain separate provider state.

## Preserve timing and ownership

The source-only [consumed runtime diagnostics](CAMERA_PROVIDER.md#consumed-runtime-camera-diagnostics--14-october-source-only)
copy genuine selected Stream/Queue metadata into the existing finite main-panel
report. Original acquisition and publication dates stay separate from preparation
and accepted-receipt observation. Registered-input delivery bounds are labelled
explicitly; they do not establish the signed slot's applied fit or a physical
window target. The existing 0/false orientation guard and signed assets remain
unchanged. Fractional capture mapping comes from the actual successful CPU
adapter; signed-scene, optical and physical-display fit remain null.

Capture timestamps use local monotonic time. They do not use phone NTP time,
arrival time or render-loop time. Our camera queue records these timestamps
separately from any native buffer metadata.

The file-driven provider now includes an asynchronous Linux V4L2 adapter. It
opens only the configured device, retains original monotonic acquisition time
and EOF/SOE reference, and offers YUYV DMA-buf backing or explicitly selected
packed CPU copying. Capture work never runs in the Ultra display poll. See
[Linux capture and retirement](CAMERA_PROVIDER.md#capture-from-linux) for the
supported formats, fixed leases and new-generation fault policy. Physical
camera/import and signed-camera activation remain unqualified.

[`CameraInput`](../include/headunit/theme/camera_input.hpp) maps acquisition time
using `MediaClock::capture_at(captured, observed)`. It requires a currently
qualified clock and a retained accepted anchor that already existed at capture.
It drops a capture predating startup/relock coverage or whose mapped PTS regresses
under a valid bounded phase correction. It never clamps PTS, creates a clock,
renews capture age by polling, or changes native producer metadata.

Staging happens only when the signed scene uses the camera layer. Stream tokens
map to distinct source leases and retire in actual receipt order. Shared CPU
pixels and native imports alias the outer capture permit, retaining its producer
even after suspend. Repeated scenes keep the same source timestamp. Fresh queued
input cannot make an older displayed texture fresh. The display also checks
capture age after GPU work and redraws without expired pixels before submission.

For a camera before connection or after phone timing disappears,
[`NativeDisplay`](../include/headunit/camera_display.hpp) uses an
independent presentation path with a local clock. Do not make an
unqualified Ultra frame eligible merely to display camera pixels. Keep the
instrument display independent of camera failure.

Each GPU submission must retain immutable backing until its fence completes.
The provider bounds waiting/staged/current storage and all outstanding leases.
Only an actual presentation receipt advances its current frame. See
[provider ownership](CAMERA_PROVIDER.md#present-a-frame).

## Bring up the complete feature

1. Load an explicitly enabled camera configuration and start the synthetic
   provider. Visibly label this source as simulated.
2. Qualify the configured local input/visibility adapter in the actual signed
   scene with the phone. Verify clipping, source loss and frame clearing.
3. Qualify connected Ultra camera entry/exit within the existing signed main
   scene, with no native-surface takeover, runtime restart or session teardown.
   Retain both panels, media/control ownership and genuine presentation timing.
   Separately integrate the native owner for Standard and before/no phone.
4. Qualify the experimental read-only exterior-camera resource configuration
   with the actual phone. The inspected packs do not supply its numeric wire
   schema or camera-button definitions; independently observed identifiers are
   documented above, while button/control behavior remains unresolved.
5. Couple local visibility and remote controls through the verified phone
   channel. Route focus and touch to the visible controls, then test repeated
   entry/exit, phone loss and source failure.
6. Qualify the V4L2 adapter with a selected camera and the implemented fresh
   reverse activation controller. Measure acquisition age with its recorded EOF/SOE reference and
   trigger-to-first-presented-frame on that hardware.

## Evidence and limits

### Combined signed GPU and bounded iPhone run — 4 October 2026

The new `headunit-installed-camera-state-probe` executes the production
InstalledUi factory and state provider/model with the actual verified regular
pack on two isolated 1920 × 720 Weston outputs using CM5 V3D. It checks initial
Off, On, Off while both scenes progress, fresh re-entry, clock withdrawal and
recovery, bounded worker retirement, a replacement with fresh control/video
owners, and terminal connection closure. Camera stream current PTS advances
only through real presentation receipts. The model checks the two read-only
aliases on every receiver turn and fresh reads retain owner-sourced dates.
The successful run made **1,811 alias checks and 280 fresh dated reads**.
Timing, peer acknowledgements and capture are explicitly synthetic; this is
real Linux/GPU feedback, not a physical scanout or real phone fixture.

The 7 October source fixture additionally requests Off through the actual
`CameraStateSnapshot` mailbox while the manual activation remains On. It checks
ordered request/withdrawal/presentation dates, both progressing signed panels,
continued dismissal under the held On intent, and explicit Off→On re-entry with
new acquisition dates. A second request while the phone-clock fixture is absent
must remain pending even though eligibility is already false; restoring the
clock must produce a new camera-free accepted compositor receipt before the
request is applied. The probe never calls `owner_off` or `presented_off` itself.
Strict local Clang compilation and the actual 7 October CM5 signed-GPU run pass.
The target run confirms **two applied mailbox Off requests**, **40 pending checks
without a clock**, **1,998 alias checks** and **171 original-dated fresh reads**,
with both isolated compositor logs identifying V3D. Source/input/binary hashes
and the passing report are private in
`/home/headunit/headunit-oem-20261007/signed-camera-mailbox-off-v2/`. The camera-free
base derives from the exact selected V3 pack profile and its original vehicle
profile; only camera/Home/native-camera ownership entries are stripped before
the existing harness creates private camera bindings and removes style ownership.
The original sealed profile's digest remains unchanged.

The preceding `-v1` failure remains preserved: its fixture reused a scene floor
from before the held-On interval, so it could write On before the owner observed
explicit Off. The corrected fixture samples a fresh scene floor immediately
before that Off write; no production owner change was required. The report names
`signedCameraMailboxOff`, `appliedOffRequests` and
`pendingOffWithoutClockChecks` separately from the original lifecycle result.
This adds no phone writer, Remote UI control/focus, audio or physical camera/
scanout evidence, and changes no selected appliance release.

Run it explicitly with private resources (not part of default CTest):

```sh
python3 tests/installed_camera_state_test.py /usr/bin/weston \
  /path/to/build/headunit-installed-camera-state-probe \
  /absolute/private/camera-free-pack.conf /absolute/private/new-evidence-dir
```

The harness creates owned private input files, merges free local camera IIDs,
checks both V3D renderers, retains source/input/binary hashes and shuts down only
its own process groups. Evidence is retained at
`/home/headunit/.cache/headunit-qualification/20261004/signed-camera-state-gpu-final/`.
Both `-b` and `-final` runs passed. The preceding `-a` failure was a fixture attempting to reclaim a retired video
owner; the corrected fixture creates fresh control/video owners on replacement.
The CM5 build retains the documented global misleading-indentation and
gauge-input-object maybe-uninitialized warning exceptions.

A subsequent bounded standalone run used the actual iPhone, phone timing,
physical outputs, regular version-985 signed pack, simulated capture and the new
camera-state profile alongside the unchanged Climate/settings capabilities.
The user confirmed both CarPlay panels before entry, camera pattern plus
continuing Ultra cluster while On, then restored CarPlay and working touch
while the cluster continued after Off. GPU diagnostics recorded 910 camera
presentations; after Off, final main/cluster scene counts reached 6,869/6,895, with
camera visibility false and phone video presentation counts 156/2,533. Both
vehicle channels remained open, with one configuration request and three
registration requests; no display or vehicle-channel fault was reported.
Aggregate counts do **not** identify the subscribed camera IIDs or prove iOS
consumed the On aliases. Audio advertisement was off, so audio continuity was
not exercised. No CameraButton/UserDismissible/focus behavior or physical
capture was claimed.

Private phone evidence/profile is under
`/home/headunit/headunit-camera-state-gpu-20261004/live/`. The test stopped only the
manager and Home services, kept the compositor/output/backup owners, and used
a transient unit with automatic V3 restart after termination. V3 was restored
with its original manifest hash and all five services active. Its sealed files
were not edited and the camera candidate remains uninstalled. Four affected
portable suites pass; the new probe also passes strict Clang syntax checking.

### State bridge — 4 October 2026, `86a4059`

The 4 October state-channel checks pass on Mac strict builds and source
ASan/UBSan/ThreadSanitizer builds of the snapshot, provider, capability model
and service; linked external dependencies are not instrumented. The integrated
receiver and installed-UI probe also build on actual CM5 Linux, and its camera
state, vehicle-configuration and local-visibility suites pass (three of three).
The broader portable vehicle-protocol suite and 84 Python configuration,
phone-management and release cases pass. Private CM5 evidence is retained at
`/home/headunit/.cache/headunit-qualification/20261004/camera-state-channel/`,
including source/binary hashes and test output. The Linux build retains the
existing legacy misleading-indentation exception and the gauge-input-only GCC
maybe-uninitialized warning exception; it is not claimed as a default-flags
clean build. No phone, physical capture or new signed-GPU state-bridge run was
performed, and none of the five running appliance services was changed.

### Inspected packs and earlier local-adapter checks

The 2 October private audit inspected `PackageData`, `InputStream`,
`LayoutController`, `Layout` and `VehicleDataUpdater` from all four installed
archives. Camera-related behavior matches after excluding differing copyright
headers. Reference and Single signatures/chains were rechecked under the
explicit expired-leaf development policy; no resources were modified.

Portable tests exercise metadata, strict activation files, visibility grouping,
historical clock mapping, phase corrections, ordered receipts, real capture
age, GPU lease retention and local view restoration. The isolated Linux GPU
probes exercise real compositor feedback separately from physical scanout.
Eight affected portable suites and eleven CM5 suites pass. Twelve private
installed-profile checks cover the enabled/disabled paths and reject unsafe
or incompatible configuration. An isolated two-display installed-host run
executes the actual regular signed pack with explicit synthetic timing and
receives real presentation receipts for the registered camera input. Its fake
peer acknowledgements are a fixture, not evidence of iPhone acceptance.
The same fixture checks manual on → off → on: visibility clears while off and
new captures receive real presentation receipts after re-entry.
That earlier local-adapter qualification does not cover the new phone-facing
state bridge. Neither check qualifies physical capture, appliance focus
restoration or phone-coordinated Punch-through. Track the full acceptance gate
in [the project goal](GOAL.md).
