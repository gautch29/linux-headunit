<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Local vehicle views inside Ultra

The current priority is camera Punch-through, followed by lane and seat/door
views. Local vehicle pixels belong in the existing signed scene. Main and
cluster, both phone streams, UI-Sync, audio and control must remain alive.
A separate full-screen native surface does not establish connected Ultra
acceptance. Apple describes this composition model in
[WWDC24 architecture](https://developer.apple.com/videos/play/wwdc2024/10111/).

## Sources and slots

| View | Route | Current boundary |
| --- | --- | --- |
| Exterior camera | Signed main `camera` slot, 1650 × 720 at (135, 0) | V63 physical USB feed and managed On/Off shown inside live CarPlay; phone Close/focus remain open |
| Lane assistance | Signed cluster `DCA_ADAS`, named `adasView`, 600 × 480 in the regular reference pack | V63 phone-confirmed ADAS selection shown on the cluster; labelled simulated source |
| Seats and doors | Original local cabin renderer; pack also defines `widget://?identifier=com.apple.carBodyStatus` | V68 signed phone widget shows current-pack car/interior artwork; fitted readings and phone Close/focus remain open |

### Middle-area content inventory — 11 October

Apple's [dynamic-content design session](https://developer.apple.com/videos/play/wwdc2024/10112/)
identifies Maps, Now Playing, trip computer, tyre pressure and ADAS, plus
notifications/rich vehicle overlays. This is a platform inventory, not evidence
that every option is selectable in the current signed pack or deployed profile.

| Content | Evidence and current availability |
| --- | --- |
| Maps/navigation | Current phone catalogue declares DCA_Map; live cluster map captured. |
| ADAS/lane view | Current phone catalogue declares DCA_ADAS; V63 shows labelled simulated lanes. |
| Now Playing/media | Apple documents cluster media; the user's reference shows album art/title/artist. V74 large MapsMedia renders Apple Music (title/artist/progress/artwork) once the opt-in car `media` declaration is present. |
| Trip computer | Apple documents this content. V72 signed Current Trip page is observed between gauges; four readings are unavailable. Physical data remains open. |
| Body status: seats/doors | V68 renders current-pack car/interior artwork in its signed request overlay; physical seat/door states remain unqualified. |
| Tyre status | V68 renders car artwork and four `-- bar` readings; physical pressures remain unavailable/unqualified. |
| EV charging | Installed pack references `com.apple.charging`; no fitted widget/data route is qualified. |
| Notifications/vehicle alerts | Apple documents temporary dynamic-content overlays; the pack declares RequestContent_Zone1 overlays. Live integration remains unqualified. |

The live DigiRing picker currently exposes only Map and ADAS. Read-only metadata
inspection confirms that the five gauge styles each declare those two primary
choices plus a request-content overlay. Widget references do not establish a
selectable page, its exact readings, or phone support. The phone's actual V72
carousel also offers Trip, tyres and an empty
`com.apple.CarRadio` page, beyond these declared choices. On V74, MapsMedia
offers Music, Map and EmptyWidget; CarRadio's page fills only when the car
declares media. This is observed phone content, not an assertion that every
referenced widget is selectable. Native vertical page controls and body/tyre
overlays have captures.
Rear camera is currently a distinct main-screen slot, not a middle-cluster choice.

Do not send seat pixels under the `adasView` identity or substitute Maps video
for assistance. The car-body widget URI is an observed pack contract, not proof
that the current phone accepts our closures/restraints resources. Its exact
capabilities must be established before choosing phone-rendered versus local
cabin integration.

## Opt-in Remote UI capabilities and requests

The new source accepts `cluster_content` inside the existing car-settings JSON.
[Capability example](../config/car-settings.cluster.example.json) declares the
three widget URLs verified in the reference pack, plus read-only trip, tyre,
restraint, closure and charging readings. Omitted/null readings are unavailable;
`/info` omits their initial values while OPACK can report invalid readings.
Numeric readings require explicit unit identifiers, bounded domains and original
provider timestamps. The example's tyre/closure layout keys are fixtures, not
qualified fitted-car positions. Enum domains are bounded; physical enum meanings
and resource/layout contracts beyond the scoped V68 body/tyre rendering remain
unqualified. These opt-in capabilities do not imply that every declared page is
displayed. Selected V72 retains only body/tyre requests in the combustion bench
profile; charging is omitted.

An optional `request_file` enables the native Cluster page's vehicle-view buttons.
It must be the private manager-state `cluster-content.request` endpoint. The
receiver preflight requires matching local bindings, illustrated in
[local request bindings](../config/cluster-content-script-bindings.example.plist).
Merge these with unused local IIDs into the existing vehicle profile; never use
them as Apple phone type identifiers. The same copied connection state drives
the phone-facing RequestContent service and the signed RequestContentController.
The bridge adds no process, socket, camera stream or VM work to the control loop.

The private file is bounded to 384 bytes, owned by the display account, mode
0600, single-linked and atomically replaced beneath its private 0700 directory.
`HUC1` includes the real launch nonce, control generation, increasing explicit
command sequence, actual local monotonic renewal and two-second expiry. The
manager renews at 500 ms; redraw cannot renew a lease. Missing/unsafe files,
expiry, wrong publisher and retirement withdraw the request. Phone `On=false`
and the explicitly bound signed local Close action dismiss and retire that
command; later renewals cannot reopen it, including a concurrent control renewal. A genuinely
new user selection requires a newer command. The phone cannot write `On=true`
or replace the URL. Buttons request content and never highlight an unconfirmed
active page. Readings and request intent do not prove physical presentation.
Both the phone-facing On and local script input stay Off through startup and
co-brand scenes. A fresh real lease renewal is required after entering Drive.
Local `CP.setVehicleData` accepts only boolean false for the declared request's
On IID; all other local vehicle writes remain rejected. Off is a host withdrawal
decision, not a sensor sample or a renewed request acquisition.

Media is deliberately absent from the vehicle NowPlaying accessory: that
accessory represents vehicle-owned sources. Reflecting the phone's own track
back into it would create competing ownership. Trip capability declarations
also do not establish a selectable centre route. Actual notification controls
remain separate from RequestContent. See [OEM alignment](OEM_ALIGNMENT.md).

### Source qualification — 11 October

macOS passes 151/151 checks; isolated Linux GCC and Clang ASan+UBSan each
pass 179/179; the full Skia receiver ASan job passes 180/180 non-hardware
checks. Actual CM5 strict Clang 19 builds the receiver and explicit installed
probe; 20 affected protocol, script, worker, camera and manager checks pass.
CM5 fixtures require a private `TMPDIR` and `umask 077`: the first broader
run rejected group-writable fixture inputs/ancestors. Those failed receipts
remain retained; production admission was not weakened.

The explicit signed/GPU fixture uses two isolated V3D compositors and synthetic
phone clock/ACKs. Its final v9 run checks 338 startup frames with local/remote
intent Off, reads the exact three declared URLs in the signed script 77 times,
and verifies one restricted local Close. Before/after snapshots of the five
production owners match. This is script/intent evidence, not phone widget
pixels, physical scanout or fresh-frame-rate qualification. Earlier failed
fixtures remain private; actual phone selection, replacement, Close and
whole-session continuity remain necessary before declaring pages available.

Private evidence: Mac `~/.cache/headunit-qualification/20261011/` contains
`oem-qualified-{gcc,sanitizers,receiver}` and `oem-macos-qualified.log`; CM5
`/home/headunit/headunit-punchthrough-20261011` contains
`oem-qualified-private-ctest.log` and `oem-signed-content-v9/`. Explicit native
GTK fixture checks cover the request/Close controls without an active highlight.

### V66 deployed qualification — 11 October

Selected V66 freezes `a250862` (784 inputs). It fixes the live V65 renderer fault
on continuous corners with an original bounded two-cubic approximation shared
by Cairo and Skia. Geometry is clipped to the layer's dimensions, including
oversized radii. Tests cover fill, gradient, border, transformed child clipping,
zero radius and cache invalidation when the corner style changes. This does not
establish pixel-identical Core Animation geometry. Strict CM5 checks pass 3/3,
including an isolated V3D GPU comparison. Final macOS passes 151/151; Linux GCC
and Clang ASan+UBSan each pass 179/179; full Skia ASan passes 180/180 non-hardware
checks. These final receipts are `oem-corners-macos-qualified.log` and
`oem-corners-qualified-{gcc,sanitizers,receiver}` on Mac.

The signed v12 fixture runs while all production owners are quiescent. It checks
338 startup frames with intent Off, 121 signed-script reads of all three URLs,
and one restricted local Close; production state before/after matches. The first
v10 attempt with the live session sharing GPU resources hit its existing wall
execution budget. The v11 harness lacked the actual render group and correctly
rejected software rendering. v12 uses the display account's real supplementary
groups and passes without loosening admission or execution budgets. All failed
receipts remain private. This remains synthetic clock/ACK and script evidence.

The earlier actual V66 phone run body → tyres → native Close keeps main/cluster Ready.
Physical-output captures show the signed rounded overlay and its withdrawal back
to Map, but both widget contents are blank. The phone subscribes to request On
and URL IIDs 1132/1133 and receives three value transfers each without returned or
transferred errors; retirement coverage reports no unsupported types, retries or
faults. Request delivery is established; widget resource/rendering is still open.
Map/ADAS selection and physical USB camera On → Off are captured; camera Off
returns to the phone dashboard. No audio route was active in these observations,
so they do not qualify decoded music continuity. Unknown bench readings are not
replaced by healthy zeros or fabricated seat data.
Phone-operated Close/focus, isolated live lease-loss and physical providers remain open.

With the user-confirmed connected phone, a fresh follow-up repeats body, tyres
and native Close, then two physical USB camera On/Off cycles. All 36 sampled
states retain main/cluster Ready; five-owner PIDs/restart counts are unchanged.
Both output captures are retained per phase. The body/tyre popovers remain empty;
this is repeat request/session evidence, not a widget-content fix. No audio route
is active. Camera/content are left Off. Private evidence is
`widget-followup-live-20261011/` beneath the CM5 evidence root.

A separate V66 webcam driver-loss/recovery check detaches only the verified
GENERAL WEBCAM video interface from `uvcvideo`, then rebinds it. This is real
kernel/device failure evidence, not a physical unplug/replug. The signed camera
withdraws its pixels and shows CAMERA SOURCE CLOSED. Rebinding alone does not
retry a terminal capture while On is held; explicit Off → On produces fresh
camera pixels again, and final Off restores the phone Settings page. All 31
sampled states keep main/cluster Ready; the same five owner identities and
restart counts survive. The driver is restored and camera/content are left Off.
No audio route is active. Private captures and sanitized receipts are in
`camera-driver-recovery-v66/` under both evidence roots.

This run exposes a status bug: V66 reports a closed source as waiting even
though it requires explicit reentry. Follow-up source maps closed or reversed
source clocks to fault, missing/stale frames to waiting, and Off to idle; native
controls give close/check/reopen guidance. Eligibility is not a presentation
receipt. Component tests cover fresh capture, terminal failure, held-On behavior,
delayed retirement, explicit recovery, stale data and clock failure. The repair
is deployed in V67; phone Close/focus, physical unplug and active audio continuity
remain open. See [handoff](HANDOFF.md).

On V67, a fresh real-phone check repeats the verified webcam driver detach/rebind
against the deployed terminal-status repair. All 31 samples retain main/cluster
Ready and the same five owner PIDs/restart counts. The absent source now reports
`fault`, and rebinding while On is held leaves it faulted. Explicit Off → On
restores physical USB pixels; final Off restores the phone Settings page. Both
output captures are retained per phase. The driver is restored and camera Off;
no audio route is active. This verifies terminal status and explicit recovery,
not physical unplug, phone-operated Close/focus or native fault-guidance visibility.
Private evidence is `camera-driver-recovery-v67/` under both evidence roots.

### V68 physical USB recovery — 11 October

With the user operating the USB webcam, the configured video device actually
vanishes and returns. The camera owner reports fault and the signed main slot
shows `CAMERA SOURCE CLOSED`; both phone panels stay Ready. Reconnecting the
webcam while holding On leaves the fault visible. Explicit Off → On restores
camera pixels; final Off restores the phone dashboard. **88 sampled observations**
(76 during physical removal/return and 12 during explicit recovery) retain both
Ready panels and the same five owners/PIDs/restart counts. The user confirms
physical removal/reconnection; this is distinct from the earlier driver rebind.
No audio route is active. Camera is left Off. Private evidence is
`camera-physical-replug-v68/` on CM5/Mac; the pixel review is recorded separately.
Repeated/sustained hot-plug, active audio, phone Close/focus and isolated lease
loss remain open; one successful recovery is not complete camera acceptance.

### V68 receiver-worker loss and reconnection — 11 October

With USB camera and body content requested, only the verified sealed receiver
worker is terminated with SIGKILL. Native main/cluster fallback is visually
observed, camera/content intent is cleared and the five outer service owners
retain their PIDs/restart counts. The first probe failed before termination
because its display account could not inspect the root-owned manager's `/proc`
entry; the corrected helper verifies ancestry and exact sealed executable as
root, then signals a pinned pidfd. Neither attempt edits a release or starts a
competing radio/display owner.

This profile omits automatic receiver recovery and retains its default zero
retry attempts. The first 120-second wait does not obtain a fresh phone session,
even after a normal Connect request; its timeout receipt remains retained.
After the user reconnects, **17 both-Ready samples** verify a new publisher,
rejection of the genuinely old publisher retained from the withdrawn request,
no resumption of old camera/content intent, fresh body and tyre artwork, and
camera On/Off with USB pixels. All five outer owners remain unchanged. This is
user-assisted reconnection after worker loss, not automatic recovery, RF-loss
qualification or a sustained run. Camera/content are left Off; audio is inactive.
Evidence is `worker-loss-content-camera-v68/` on CM5/Mac, with separate retirement
and reconnection receipts. Isolated lease loss and phone-operated Close/focus
remain open.

### V68 isolated content-lease expiry — 11 October

With the normal manager content controller idle, a temporary first-party
`ContentControl` publisher takes the request endpoint using the current real
connection publisher and next command sequence. It renews genuine two-second
monotonic leases, then stops only its renewal. No service, manager, phone clock,
radio or display owner is paused. The camera remains normally managed On.
The request file remains byte-identical across the actual expiry deadline.

Reviewed captures show body artwork while leased, Map after expiry, and Map
when the same retired command receives fresh lease renewals. A new manager
command restores body artwork; native Close returns Map. Bounded clear commands
advance the manager beyond the borrowed sequence before handback. Camera USB
pixels remain visible throughout the content expiry and recovery; final Off
restores the phone dashboard. **76/76 both-Ready samples** retain the same five
owners/PIDs/restart counts. All 16 output captures are reviewed; requests end Off.

This qualifies one isolated content request expiry and retirement with a real
phone, not camera activation-lease expiry, phone Close/focus, RF loss or repeated
fault cycles. No active audio route is present in this run. The manager's
`confirmed` field remains false; pixels do not provide a phone applied ACK.
Private fixture, samples, owner hashes and separate visual-review receipt are in
`content-lease-v68/` on CM5 and Mac. The selected sealed V68 is unchanged.

### V68 camera with user-started music — 11 October

After the user starts music through CarPlay, camera On/Off and a separate
**90-second camera hold** retain both Ready panels and the same available audio
route in **203 samples**. Mixer state is fresh throughout (maximum observed age
108.2 ms), volume remains 10%, unmuted, without pending requests or errors.
All five owners/PIDs/restart counts remain unchanged. Eight reviewed captures
show early and sustained USB camera pixels, cluster Map in each phase and the
phone dashboard after Off. Camera/content end Off; music is left to the user.

This is camera/audio-route coexistence on the silent bench. The installed
mixer-status file carries no authenticated/decoded/output progress counters;
connection-retirement totals cannot prove phase-by-phase PCM continuity.
No gapless audio, audible playback, fresh video rate, capture age or physical
latency is qualified. Keep sustained decoded-audio continuity open and obtain
bounded live stream/output progress before closing it. Private fixture, samples,
owner snapshots and separate pixel receipt are `camera-music-v68/` on CM5/Mac;
the sealed V68 and its direct rollback are unchanged.

### Phone-side vehicle resources — 11 October

The pinned tyre consumer uses `CAUResource.vehicleLayout`,
`CAUResource.topDownExterior` and widget margins from a separate vehicle asset
library. Inspecting only the accessory ZIP misses the phone-side resources.
The current **regular `44C9FA31EC / 985`** outer Apple delivery contains
`CarPlayPhone.assets/Extras/manifest.json`, vehicle layouts and exterior/interior,
seat and closure artwork. Its accessory ZIP SHA-256 equals our installed
reference (`ddf1317ef051f8ac251c9d892b5ea0651a75e14dd59e0f5ac2ff13b37729d504`).
The outer download length and SHA-1 match the retained verified Apple catalogue.
No pack or signed file was changed or executed. See
[delivery provenance](ASSET_DISCOVERY.md#official-delivery-provenance).

The phone's variant agent reads exterior/interior style traits and a separately
selected vehicle layout. The framework exposes these as immutable string
characteristics in VehicleResources (`0x01200002`) / VehicleVariant
(`0x20000001`): exterior style `0x48000003`, interior style `0x48000004`,
vehicle layout `0x48000005`. Current source accepts opt-in `vehicle_resources`
in car-settings JSON with all three explicit identifiers. No provider read,
actuator write or image-archive control is attached to these fixed selectors.
Existing profiles omit the accessory. Names must be present in the matching
phone-side manifest; parsing a syntactically valid name does not prove that.

[Resource example](../config/car-settings.vehicle-resources.example.json) uses
the current pack's `DBX` entries as an explicit **bench artwork fixture**, not a
claim that the fitted car is a DBX. Its four tyre layout keys are verified in
that layout's `infos`: `tire_front_left`, `tire_front_right`, `tire_rear_left`,
`tire_rear_right`. All readings remain unavailable. The earlier `fixture_tyre`
and `fixture_closure` keys have no mapping in this layout and must not be used
for its live pixel qualification. Preserve existing settings/Climate and request
bindings when merging an example; this is not a replacement deployment profile.

`CAFGetImageArchiveControl` is a separate optional custom-image mechanism with
an `imageArchive` byte response. Its manifest describes named images and
appearance-specific file properties; it does not establish a vehicle-layout
replacement. Advertising that control is unnecessary for the first test of
the current pack's existing resources. The missing variant accessory and
unmapped fixture keys are concrete profile gaps, but remain **unproven causes**
of the blank live widgets before qualification. V68 selects the checked source
and matching private profile. After reconnection, a real-phone run performs
body → tyres → native Close and USB camera On/Off. All **27 samples** retain
both Ready panels and the same five owners/PIDs/restart counts. Captures show
body car/interior artwork, tyre car artwork with all four `-- bar` readings,
replacement and native Close back to Map. Camera shows USB pixels, then returns
to the phone dashboard. Camera/content are left Off; no audio route is active.

This combination resolves blank widget rendering on the bench. It does not
isolate which selector/layout-key change is independently necessary or qualify
physical vehicle data. Manager `ContentControl.snapshot()` still deliberately
returns `confirmed: false`; visual observation is not a phone applied-status ACK.
At that stage, phone-operated Close/focus, applied-status feedback, isolated lease
expiry and phone-loss recovery remained open. The separate isolated-expiry run
above now qualifies that one lifecycle case. Private captures/27 samples and
`visual-review-receipt.json` are in `widget-resource-v68/live/` on CM5 and Mac;
no phone capture enters Git. No custom image archive was advertised or transferred.
See [provenance](PROVENANCE.md#oem-widget-resource-research).

A live fault-injection attempt paused the manager for 3.2 seconds using real
monotonic time. The overlay withdrew, with unchanged five-owner PIDs/restart
counts, but presentation readiness subsequently faulted (`qualified scene
presentation stalled`). Manager duties beyond content renewal were also paused;
this does not isolate or qualify content expiry. The failed capture/status tail
is retained. A fresh connection must be verified after fault injection; a
redrawn ADAS frame does not establish whole-session readiness. The first new
Connect attempt found the selected phone absent and did not regain Ready. After
the user reported the phone connected, a fresh five-second `recovered-ready` tail
and both physical output captures confirmed CarPlay Ultra and cluster Ready,
MapsMedia selected, and camera/content requests Off. All five owners are active
with zero service restarts. That attempt establishes reconnection recovery;
the later isolated-content-lease run above supplies separate expiry evidence.

V66 preflight and actual V66 → exact V63 → V66 five-owner/boot-gate roundtrip pass.
[HANDOFF](HANDOFF.md) owns selected manifest, source,
helper and rollback bindings. Private live captures and sanitized receipts are
`oem-v66-live/`; signed fixture evidence is `oem-signed-content-v12/` on CM5.

`LocalInput` generalises the existing camera input adapter. `CameraInput` remains
an alias with its camera default. Non-camera widget/settings/road-sign inputs
require an explicit type, an external producer and no camera visibility callback.
They inherit bounded leases, original acquisition timestamps, qualified phone
clock mapping, ordered presentation receipts and immediate stale withdrawal.
No native clock is manufactured to make an Ultra input eligible.

## Vehicle observations

`VehicleViewProvider` accepts one pending `ViewObservation` and renders on its
own CPU worker, away from the control/VM/GPU owners. Each observation contains
source generation, increasing sequence, original monotonic acquisition and
physical/simulated provenance. The content is a bounded lane or cabin value.
The queue must outlive its producer. Stop closes that queue generation immediately;
worker cleanup acknowledges retirement. Outstanding GPU leases retain immutable
pixels, but cannot keep them eligible for a new draw.

Missing lane boundaries/engagement, seat occupancy/belt state and door state
stay unavailable. Unknown doors and seats have question marks. Simulated pixels
always include `BENCH - SIMULATED`. Rendering, copying, polling and redraw never
renew acquisition age. A changed generation needs a replacement owner/queue.
These APIs are software contracts, not a qualified CAN/MCU/ADAS provider.

The opt-in `benchAssistanceView=true` installed-UI setting registers `adasView`
only on the cluster. It requires `simulateGauges=true`, no physical vehicle socket,
and the inspected regular reference pack `44C9FA31EC:985`. It uses the existing
layout catalogue/UI-Sync route; registration or a host request is not a
phone-confirmed active component. This setting is absent from selected V61.

## USB camera adaptation

[Disabled USB example](../config/camera-ultra-usb.example.json) selects explicit
YUYV CPU-copy delivery. Existing configurations retain their exact geometry.
Optional `source.adaptation` declares the driver's acquisition width/height and
`fit`: `contain` preserves the complete field of view with opaque black bars;
`cover` removes a centred crop without stretching. The existing `capture`
width/height remain the delivered queue/slot dimensions when adaptation is used.

Conversion and bounded bilinear adaptation happen on the capture worker. The
common unscaled letterbox path copies rows directly. Acquisition date, source
reference and physical provenance survive conversion unchanged. Adaptation
rejects DMA-buf delivery; there is no silent fallback from import to copying.
Rotation and mirroring remain unqualified for this signed route.

Source-only `CaptureMapping` exposes the CPU adapter's consumed scale, complete
image placement, visible acquisition crop and delivered content rectangle.
The existing capture probe adds actual negotiated stride/aspects and this
geometry beside its original acquisition timing. It leaves signed-scene/optical
fit null; see [the diagnostic contract and owner-only follow-up](CAMERA_PROVIDER.md#fit-geometry-diagnostics--14-october-source).

`headunit-camera-capture-probe CAMERA.json` is an explicit physical-device check.
It consumes real frames for five seconds, reports original capture-to-consumption
age, and saves no pixels. Its queue completion is not a display receipt.
The connected GENERAL WEBCAM exposes `/dev/video0` capture and `/dev/video1`
metadata. On 11 October the 1280 × 720 YUYV run delivers 45 frames in five seconds,
with 87.1 ms mean/90.9 ms maximum age after the direct-copy improvement. The
640 × 360 run delivers 137 frames, with 33.9/40.0 ms age. Both deliver a 1650 × 720
image; neither measurement establishes physical screen latency or 30 fps.
The initial 720p signed run misses freshness under GPU load and remains failed
evidence. Use the smaller acquisition mode for further bench qualification.

## Camera-state correction

A physical producer exposed starvation hidden by the zero-delay synthetic camera:
negative polls repeatedly advanced an Off cutoff, then compared delayed original
capture timestamps with those newer decision timestamps. The renderer could
present pixels while the phone-facing aliases stayed false.

Off decisions and On acquisitions now keep separate ordering. Waiting/negative
reads do not move activation barriers. Actual visible-to-Off transitions,
explicit applied Off, generation replacement, expiry and retirement still date
withdrawal and reject older captures. The read-only visibility publication method
retains a separate acquisition frontier; ordinary sensor publication remains
strict. Both aliases read one provider observation per refresh. Future/reversed,
repeated and stale captures remain rejected; no timestamps are clamped.

## Qualification and remaining acceptance

Portable fixtures cover fit/crop, padding, interpolation, malformed inputs,
acquisition/reference preservation, missing values, simulation labels,
clock/source ageing, source retirement, delayed-delivery recovery and both On
aliases. The two isolated signed-GPU fixtures use copied private verified assets
and synthetic phone/clock traffic. Optional `CAMERA_JSON` selects actual USB
capture; `--bench-adas` exercises the registered lane source through a synthetic
UI-Sync transition. Native Home uses the existing main lease and must withdraw
camera pixels/On while the cluster continues, then recover fresh camera pixels.
These runs do not qualify a real phone, physical scanout, audio continuity,
Remote UI camera controls or touch/focus.

Final affected suites pass: macOS 17/17; Linux GCC 18/18 with default strict
warnings; Linux Clang 19 ASan+UBSan 18/18; CM5 Clang 19 18/18. The CM5 full-Skia
production receiver, camera capture probe and installed camera probe compile
with strict warnings. The frozen 774 tracked inputs are retained in
`source-final.json`/`source-final.tar`; subsequent documentation-only results
updates are not compiled behavior.

Private evidence lives under `headunit-punchthrough-20261011` on CM5 and
`~/.cache/headunit-qualification/20261011/punchthrough` on Mac. Failed runs are
retained beside corrected ones. `signed-usb-640-camera-v7` passes the real USB
camera lifecycle alone. `signed-usb-640-adas-v9` passes the same lifecycle plus
five or more fresh lane-view presentation receipts after a synthetic UI-Sync
selection. Its 3114 paired alias checks, two applied Off requests and 40 checks
of pending Off without a clock retain original capture dates. Both use V3D,
leave active services unchanged and explicitly report no real phone or physical
scanout qualification. Deployment binding and next action belong in HANDOFF.

The V5 source graph now pins capture, owner-sourced state and the manager's
manual profile together. `cameraStatusFile` publishes the existing signed
renderer's HCS1 eligibility heartbeat every 100 ms and stopped on retirement.
This is not a camera acquisition date or a physical presentation receipt.
Native Home uses it to return after manual camera withdrawal; unavailable status
still fails closed. The renderer never starts a separate camera display owner.

The V5 integration passes 52 portable release cases, 15 manager retirement and
camera availability cases, and 20 selected CM5 checks (33.66 s). The strict
full-Skia receiver/checkers build. `signed-usb-status-adas-v11` repeats physical
USB acquisition plus signed camera/lane, Home, mailbox Off and owner retirement
on isolated V3D compositors with synthetic peer/clock traffic. It also requires
native active/stopped status from the render owner. Failed missing-target and
wrong-umask runs are retained; corrected tests run with trusted private inputs.
No real-phone behavior follows from these fixtures.

V63 real-phone evidence: the signed main camera slot displays the physical USB
feed; the cluster remains Ready, then switches to phone-confirmed DigiRing/ADAS
and shows labelled lane graphics. Managed camera Off restores the phone dashboard
without reconnecting. Four further On/Off observations retain both Ready states
and audio route availability without error. All five owners show zero restarts.
The CM5 also passes actual cold V63 → V61 → V63 selection/boot-gate verification;
exact deployment bindings and transactions belong in HANDOFF. Private captures
and reports are under `release-v63` on CM5 and Mac `punchthrough-v5`.

The signed rear-camera frame covers the main logical canvas height; it temporarily
replaces that area's phone UI rather than stopping the session. Smaller lane
content uses the distinct cluster ADAS slot. The seat/door widget remains a
separate, unqualified capability/slot contract; it was not shown in CarPlay.

Next: qualify phone camera Close and focus/touch, sustained decoded audio,
missing/stale capture, physical USB loss/recovery and reconnection. Establish the
cabin widget/fitted-capability contract before connecting its renderer to CarPlay.
