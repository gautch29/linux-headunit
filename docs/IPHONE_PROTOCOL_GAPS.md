<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Missing and unfinished iPhone behavior

Original audit **9 October 2026**; affected statuses reconciled **11 October 2026**
against source `2df4a10` and V66/topic evidence. The first [route audit](IPHONE_PROTOCOL_AUDIT.md)
was not the whole missing-behavior list. This review adds capability contracts,
signed-scene inputs, framework service/characteristic facts, provider writes,
rendering and qualification. There are **72 reference IDs** below, not 72 active
TODOs. Some are missing implementations; others already exist but need integration or
physical/phone evidence. Conditional capabilities are required only for a car
that actually has them. They must not all be advertised by default.

This is a technical inventory, not an active task list or a claim to know every
private Apple interface. Existing component implementations are not tasks to
rebuild; conditional/unobserved research is outside the current plan unless
a fitted capability or observed current-pack blocker needs it. The current order
of work belongs in [ROADMAP](ROADMAP.md); each linked topic owns its implementation and evidence.

## Review boundary and current state

The frozen source/release account below is the original 9 October audit. It is
not current deployment telemetry; the affected table rows incorporate later
software/results, and [HANDOFF](HANDOFF.md) owns the selected release.

- The frozen receiver review is `89fb3eb`, with the subsequent source changes
  through `123ef2d` inspected separately. The latter stops imposing a default
  Ultra center component when the caller has not requested one. That fixes an
  ownership decision; it does not establish phone widget or background success.
- The CM5 moved during this read-only review: V24 in the first pass, then V25,
  then `cm5-20261009-phone-content-v26` at the last service/configuration check.
  V26's service was active/running with zero systemd restarts. Its configuration
  used simulated vehicle readings and a simulation Climate/settings backend,
  with no configured audio profile. Its local signed-script profile contained
  39 signals, including 23 fixed configuration/fixture values, and **no drive-mode
  paths**. This review did not deploy, reconnect,
  change radios, acquire a display, actuate a car or run a new phone experiment.
- The retained 245-session corpus and its sampling limits are documented in
  the [route audit](IPHONE_PROTOCOL_AUDIT.md#evidence-and-limits). Neither absent
  samples nor an acknowledged command establish support.
- Every one of the **112 service classes** in the pinned public framework
  catalogue was inspected. The machine-readable
  [catalogue](VEHICLE_PROTOCOL_CATALOGUE.json) now includes **500 declared
  characteristic accessors**, getter types, type literals, source URLs and
  SHA-256 hashes, plus available shared-base-class facts. These are declarations,
  not 500 independently qualified controls. Inherited fields/actions and exact
  wire payloads require separate contracts. Two proposed shared listing names,
  `CAFSelectSetting` and `CAFNumericSetting`, were unavailable and are marked so.
- The external verified reference pack contained 193 Lua entries. Read-only
  fact extraction covered CP API names and vehicle-path literals, including
  mode, EV, assistance, notification, content and button bindings. Signed
  implementation stays outside Git. A name/path being present is not proof
  that every layout uses it or that this car supplies it.

Implementation pass, 9 October: `development branch` adds bounded
mode observations, common command dispatch, event completion tracking, repeated
RECORD, multi-stream allocation and broader selective transport retirement,
plus per-IID write results. The follow-on pass makes video/classic iAP parser
generations terminal, erases retired keys/partial buffers, rejects conflicting
equal-date provider readings and verifies both signed entrypoints' serialized
mode requests under reordering, expiry and source restart. The next pass isolates
Ultra video retirement/reopening per screen, preserving stable worker streams and
rejecting retired receipts; CM5 hardware-decoder/GPU fixtures cover both screens
and deferred completion. That pass did not change the selected release. Current selection belongs in
[HANDOFF](HANDOFF.md), not this dated implementation account. Exact component checks
and remaining contract limits belong to [control](AIRPLAY_CONTROL.md) and
[vehicle state](VEHICLE_STATE_GUIDE.md); these dated component records do not establish deployment. Later selected
generations are recorded in HANDOFF.

Status vocabulary: **Missing** means no working production implementation was
found; **Partial** means a subset exists; **Qualify** means software/component
evidence exists but the relevant real boundary is still open; **Research** means
the payload/semantics must be established before implementing or advertising it.

## Phone-log audit — 12 October

V74 adds the first phone-side evidence: the trusted iPhone's own log over USB
(`idevicesyslog`), together with the receiver's rejection counters. This found
the large-Music gate that the research inventory filed under "do not mirror
radio" (fixed in V74). Observed this session, in priority order:

| Item | Phone evidence | Status |
| --- | --- | --- |
| G20 audio flush | Each track change: `AudioMain: Sending flush command <seq>/<timestamp>`, a header-only RTSP request, previously answered 501 | **Done in V75:** FLUSH accepted with its `RTP-Info` resume point; the matching main/media stream rebuilds its decoder queue and drops late pre-resume packets. Live: 4 flushes over two skips, 0 phone errors. Audible effect unverified (no speakers) |
| G03 `disableBluetooth` | One `/command` per connection, answered 501 | **Done (V81 live):** 200 on every connection; the manager keeps Bluetooth |
| G24 night mode | Maps logs `lightLevel: Unsupported` for main and cluster scenes; `/info` has no `nightMode` | **Done (V81 live):** carkitd `night mode: YES, night mode supported: YES`; Maps `shouldUseNightMode: 1`. Headlight policy follows the simulated low beam |
| OEM icon | V81 phone parses and composites the offered Home PNG; Ultra omits the icon | **Pack restriction confirmed on iOS 27.2:** `allowManufacturerAppIcon` defaults false and is absent on both pack displays. No host-only fix; use native Home. `oem:back`/`requestUI` phone round trip remains unqualified. [Evidence below](#oem-home-icon--13-october-2026) |
| G28 input | Single-touch only; no media/telephony consumer keys | **Media keys done (V81 live):** next/play-pause control YouTube. App shortcuts (`requestUI` `music:`/`maps:`) open those apps. Multi-touch stays opt-in until a two-slot panel is fitted |
| G25 limited UI | No `limitedUI`/`limitedUIElements` | Needs a real moving/parked source |
| G58 audio settings, lighting, paired devices | CAF `AudioSettings`, `Lighting`, `PairedDevices` and related services undeclared | Need fitted hardware; phone pages may gate on them as CarRadio did |

The 12 October rejection-log gap was subsequently addressed in `e6418eb`:
rate-limited rejection notices include the fixed method/route category for
recognized routes, or `unclassified` for unknown routes. The selected V87 source
contains this implementation; this is source evidence, not a new phone check.
Unknown method/target pairs remain bounded numeric fingerprints in the separate
[route coverage](AIRPLAY_CONTROL.md#protocol-repair-source--9-october-2026) report; arbitrary request
strings are not added to rejection notices. The video-processing log also reports
repeated `Frame POC … timed out`; that belongs to the frame-pacing work, not a
missing endpoint. Phone logs are private in `20261012/now-playing-modes/live/`.

### OEM Home icon — 13 October 2026

**The current pack prevents the phone's OEM Home icon on the main Ultra display.**
V81's real-phone DashBoard session has `manufacturerIconVisible: YES`, label
Home, a selected 1066-byte PNG and successful compositing. Other processes'
configuration summaries show NO/null; they do not establish rejection of `/info`.
The cluster's NO diagnostic alone also does not establish the main policy.

The USB phone reports `iPhone16,1`, iOS **27.2 beta 3 / 24B5099f**. The matching
an Apple firmware build
was read with blacktop `ipsw 3.1.737`; DashBoard, CarKit, CarPlayAssetUI and the
CarPlayAsset symbols/disassembly remain private. The decisive call chain is:

- `DBThemeController.allowsOEMIcon(for:)` at `0x26253dbe0` passes
  `CarPlayAsset.Display.allowManufacturerAppIcon.getter` to helper `0x26253dc00`.
- Display lookup `0x26253ec90` reads `themeAssetDocument.document.package.displays`
  and matches the display's ID. Missing display returns false; a match returns
  that display's flag. There is no CAF service, session feature, layout-element
  or `/info` display-feature check in this gate.
- The getter at `0x2cd86ad4c` reads the stored boolean at offset `0x178`.
  `Display.init(from:)` at `0x2cd86ea74` decodes that field with
  `DecodableDefault<False>` (`0x2cd86f3d8`–`0x2cd86f45c`). Absence means false.

The exact official `44C9FA31EC / 985` outer archive's accessory ZIP matches the
private installed reference byte hash. Its phone-side `package.json` omits
`allowManufacturerAppIcon` for **Center_Display and Cluster_Display**. The three
catalogued CN/Single variants also omit it. These are read-only observations;
no signed resource or signature was modified. Renaming a display cannot obtain
true from this package. Changing PNG/label, repeating `oemIconVisible=true`, or
adding an AutomakerApps service does not change this gate. An AutomakerApp would
need its own supported declaration, consumer and phone qualification; it is not
an established substitute for `oem:back`.

**Available alternative:** retain the existing native main-screen Home button
(`homeButton.enabled=true`). It uses the existing main-surface lease and Back
route while the cluster/session remain alive. A phone-rendered OEM icon requires
an independently delivered compatible pack whose main display opts in; the four
known variants do not provide one. There is no justified runtime receiver fix
under the current-pack constraint. The source keeps the parsed `/info` offer,
clarifies its limitation and adds portable on/off tests for Standard and Ultra.

Validation: macOS AppleClang 21/OpenSSL 3.6.5 portable build and **156/156**
CTests pass. The `airplay-info` offer fixture is Linux-only, so that portable
result does not execute its new assertions. Integrated Linux qualification
exposed a stale 104-pixel expectation against the current original 120-pixel
source artwork; the fixture is corrected before publication. It checks enabled/
disabled offers in both modes, bounded PNG header/dimensions and retained signed
panel IDs. These are component checks, not new phone behavior.

Phone check on V81: leave camera/content Off, reach both Ready Ultra displays,
tap the native **bottom-right Home** button, wait ten seconds, tap Back, and
repeat three times. Its sealed page profile omits `homeButton.enabled` (the
validated default is true), with size 56 and margin 16.
Confirm native Home/main return, cluster continuity, touch, and unchanged owner
restart counts. The phone app grid is expected to omit OEM Home with this pack.
Standard's offered OEM icon and its `oem:back` → `phoneHomeRequests` → Home →
`requestUI` round trip require a separate connected-phone check. No release was
published/selected or service stopped/started for this investigation. Private
firmware, disassembly and qualification receipts are under
`~/.cache/headunit-qualification/20261013/codex-oem-icon/`.
No deployment is needed for these tests/documentation; the alternative is
already in V81. Integrate this branch into the next normal source generation.

## Existing protocol contracts

These are receiver correctness and observability work, rather than new optional
car equipment. Proposed behavior still needs exact peer-compatible errors and
fixtures; changing every diagnostic 200 to 501 without checking the phone is
not a qualified fix.

| ID | Gap / status | What we should handle | Existing owner / evidence |
| --- | --- | --- | --- |
| G01 | Unknown commands and event routes — Qualify | Explicit supported dispatch, bounded unsupported diagnostics and compatible errors; distinguish no-op acknowledgement from semantic acceptance | `control_requests.cpp`, `airplay_session_setup.cpp`; shared dispatcher now returns 501 for unsupported commands/routes, with fixed bounded diagnostics; real-phone error compatibility remains open |
| G02 | `modesChanged` — Partial | Screen/audio ownership, speech/call/navigation app states, restoration and session retirement, including valid partial updates | bounded resource/app observations and partial-array updates now exist, with session-local revision/time; type-102 media permission now follows current mainAudio entity independently of duck/user controls, including pre-SETUP/reopen and terminal-event retirement; per-entry receive dates survive unrelated deltas; screen/voice/call/outbound restoration remain open; unrelated to Sport/GT; [control](AIRPLAY_CONTROL.md#protocol-repair-source--9-october-2026) |
| G03 | `disableBluetooth` — Missing | Authenticated handoff for the current peer, coordinated with the existing Bluetooth owner | observed command now explicitly returns 501; authenticated peer handoff is absent; a global adapter-off action is not established |
| G04 | `suggestUI` — Missing | Supported content/display suggestions and restoration policy, including empty lists and unavailable content | observed `params.urls`; now explicitly returns 501; suggestion policy remains absent |
| G05 | Outbound event completion — Qualify | Match status/CSeq to bounded pending commands, timeout/rejection and generation retirement; retain separate presentation/applied confirmation | 64 pending commands/completions, CSeq/status matching, rejection, five-second observation timeout, eviction/drop counts and fresh-channel retirement now have component checks; replies remain transport observations |
| G06 | Audio capability mismatch — Qualify | Reconcile optional type-100/101 and microphone format offers with actual consumers; preserve proven phone bootstrap while qualifying the replacement | V66 retains V61 consumers for types 100/101 PCM/Opus and type-102 AAC, plus opt-in encrypted microphone software. The old type-102-only limitation is superseded; qualify actual phone voice/call negotiation and audible output; [audio](AUDIO_OUTPUT.md) |
| G07 | Repeated RECORD — Partial | Define peer-compatible idempotence and malformed/reordered behavior without starting another owner | repeated initialized RECORD is idempotent; nonempty bodies reject; live peer ordering/compatibility remains open |
| G08 | Selective teardown — Partial | Audio, both video streams and classic iAP retirement/reopening as well as generic services; preserve unaffected owners | atomic selection now covers 102/110/111/classic and generic 130; crypto IDs remain reserved; EOF/error retires video/classic cipher generations and clears keys/partial buffers; Ultra now preserves the shared worker/Runtime streams while retiring or fresh-key reopening one video; both-screen encrypted hardware-decoder/Wayland fixtures pass, including deferred completion; live-phone keyframe/restart ordering and UI/audio continuity remain open |
| G09 | Multi-stream SETUP — Partial | Determine whether the current phone needs atomic multi-stream arrays; validate all entries before allocating/mutating | 1..12 bounded entries now validate before allocation, collect ordered replies and roll back unpublished ports on failure; actual CM5 descriptor-exhaustion checks cover initial and later allocation rollback, healthy-owner retention, key/ID reservations and bounded retries; live multi-stream offers remain open |
| G10 | Command stream lifecycle — Research | Establish `setUpStreams` / `tearDownStreams` command payloads and required relationship with RTSP SETUP/TEARDOWN | Research candidates have no type-specific command handler; RTSP support is not command support |
| G11 | Audit coverage — Partial | Bounded method/type/service/IID summaries on both control channels, with observed/unsupported/applied/completed outcomes and sampling-drop counts | fixed categories and 32 rotating numeric fingerprints now retain late unknown command/route diversity on both sockets, with lifetime totals, status classifications, evictions and diagnostic errors; vehicle service/IID and numeric iAP coverage survive their channel lifetimes; hash collisions/eviction can merge or omit identities, and media permission now has separate requested/configured counters in retirement diagnostics; results remain dispatch/worker boundaries, not physical application or peer completion; real-phone inventory remains open; raw private data stays outside Git |
| G12 | One contract across both sockets — Partial | Identical command validation, allowed methods, lifecycle and unsupported policy where the peer contract is the same | both sockets now share command and strict start-line/header validation, including duplicate length/CSeq/HKP, unsupported transfer encoding and malformed field rejection; unknown commands/routes reject, empty OPTIONS accepts and nonempty OPTIONS rejects; complete method/lifecycle compatibility remains open |

## Complete ordinary CarPlay and Ultra integration

| ID | Gap / status | What we should handle | Existing owner / evidence |
| --- | --- | --- | --- |
| G13 | Navigation/alert audio — Qualify | Type-101 guidance and required low-latency formats/categories, start/stop and prompt interruption | Type-101 PCM/Opus into the shared playback owner is implemented and component-checked; actual Maps prompt, audible focus/mix and restoration remain open; [audio](AUDIO_OUTPUT.md) |
| G14 | Low-latency main audio — Qualify | Type-100 decode/output and negotiated formats for voice/default/alert routes | Type-100 PCM/Opus admission/decode/output is implemented and retained in V66. Qualify phone delivery/focus/restoration rather than reimplementing the consumer |
| G15 | Microphone uplink — Qualify | Capture, encoding, input format/latency, mute, discontinuity, privacy/route lifetime and retirement | Explicit ALSA/Pulse capture, PCM/Opus and input-key encrypted uplink have component checks. V66 omits capture; no physical microphone or spoken Siri/call result is qualified |
| G16 | Siri request and lifecycle — Partial | Qualified `requestSiri` triggers, speech app state and coordinated screen/audio resources | G02 resource observations and G14/G15 voice/microphone software exist; requestSiri hardware triggers, genuine phone continuity and physical capture remain open. Enhanced Siri is optional research |
| G17 | Call lifecycle — Missing | Telephony audio/input, incoming/outgoing call focus, mute/end and return to media/navigation | PhoneCall state acknowledgement does not implement a call route |
| G18 | Multi-route mix and interruption — Qualify | Audible media/guidance/Siri/call arbitration, duck/release, ramps, gain/mute and fault recovery | Types 100/101/102 share one bounded mixer/output with independent media permission, ducking and user controls; retained V61 repairs repeated voice replacement. Actual audible arbitration remains open |
| G19 | Shared audio/video clock — Partial | Carry original phone RTP timing into qualified playout/feedback, handle clock epochs and route latency | Current PCM scheduling uses local GStreamer timing; [audio](AUDIO_OUTPUT.md) |
| G20 | Audio flush/reset — Missing | Establish `flushAudio` semantics, discard the correct queued epoch and retain negotiated owners appropriately | No type-specific command handler; decoder reset alone is not peer flush handling |
| G21 | Loss and overload recovery — Partial | Qualified audio retransmission/control, packet-loss accounting, bounded overload and continuity | Peer wire/truncation/crypto outcomes, decoder admission/backpressure/failure, local PCM discard frames, optional real jitter-buffer and appsink-drop measurements, output retries and opaque control-port drains now have distinct counters; synthetic gap/duplicate/blocked-input and empty/truncated UDP checks pass; jitter classifications are not verified physical loss, older appsink drops remain unavailable, and retransmission/live continuity are open |
| G22 | Applied audio feedback — Qualify | Real speakers, device errors/recovery and accurate applied gain/mute/route reporting | Silent-sink/component checks are not audible evidence; [audio controls](AUDIO_CONTROLS.md) |
| G23 | Observed vehicle modes — Partial | Deploy explicit semantic-to-pack mappings, fresh mode observations and unknown/stale handling; qualify the mode reaching both signed cluster and phone | Both signed entrypoints emit correct identifier-string `0x14` requests in an isolated CM5 probe, including reordered mappings and unknown/expiry/restart suppression; V27 profile lacks mode bindings and live phone acceptance remains open; [drive modes](DRIVE_MODES.md) |
| G24 | Day/night appearance — Partial | Dated physical appearance source, phone preference interaction and both displays; research `setNightMode` before enabling it | Local signed night-mode binding and appearance handling exist; bench appearance is fixed configuration |
| G25 | Driving restrictions — Missing | A real parked/moving/restriction policy, supported limited UI elements and qualified `setLimitedUI` exchange | No command implementation or phone-facing restriction policy; do not infer parked state from missing speed |
| G26 | Vehicle information and handedness — Research | Required vehicle metadata updates, fitted right-hand-drive layout and display-specific behavior | `updateVehicleInformation` has no handler; optional metadata must match the actual installation |
| G27 | Reverse HID control — Missing | Qualified `hidSetReport`, `hidSetInputMode`, `hidCopyInputMode`, report IDs/modes and responses | Current production sends single-contact touch reports only |
| G28 | Physical phone input — Missing | Fitted steering-wheel/rotary/media/phone/Siri buttons, press/release/cancel and optional multitouch | Main single-touch is live; physical adapters/other descriptors are absent; hardware design belongs in its own repo |
| G29 | Signed button/action bridge — Partial | `uiInputDeviceButton` purpose/event and legitimate signed actions through one control owner | Pack references button paths; `CP.setVehicleData` currently rejects local script writes; do not turn reads into actuator authority |
| G30 | Center-content ownership — Partial | Phone-authoritative unspecified content, explicit Map selection, removal/switch/restore and confirmation | Source `123ef2d` removes unsolicited default content; live phone confirmation remains separate; [styles](CLUSTER_STYLE_REVIEW.md) |
| G31 | Palette/background coordination — Partial | Qualify layout, palette, phone image, appearance and requested zone together; expose unsupported combinations explicitly | Real palette/content requests have timed out; some global palettes are rejected by the signed layout itself |
| G32 | Greeting/co-brand startup — Qualify | Preserve signed startup and genuine phone-controlled Hello; qualify applicable once-per-session behavior | startup branding substitution preserves the Apple animation; the user observed phone-controlled Hello after Forget This Car/pairing. Generic trigger/once-per-session behavior is not arbitrary local replay. Extra first-pair insertion failed its budget and is deferred; no pairing reset is needed for current work; [greeting](ULTRA_GREETING.md) |
| G33 | UI focus/appearance/mode requests — Qualify | Real semantics and confirmation for UI-Sync requestFocus `0x08`, requestAppearance `0x12` and outbound driveMode `0x14`; rejection/timeout/restoration | Signed VM owns the live exchange; no sample is not proof that a handler is absent; [UI session](UI_SESSION.md), [drive modes](DRIVE_MODES.md) |
| G34 | Signed Media/Trip/widgets — Partial | Establish actual phone widget/center-content routing, artwork/text and complete composition; distinguish local signed components from phone-rendered widgets | Native metadata and typed trip exist; they do not prove the signed center shows them; [completeness](CLUSTER_COMPLETENESS.md) |
| G35 | Phone-delivered asset installation — Missing | Production service/feature, transfer attachment, bounded staging, signature/trust/identity checks, durable atomic commit, reply and rollback | Standalone parser exists; production installs a preverified external pack; [assets](ULTRA_ASSETS.md) |
| G36 | Vehicle-specific resources — Research | Actual vehicle image/logo/variant archives and manifests, resource lookup and compatibility | Cluster packs are not the separate vehicle-resource archive; [capabilities](VEHICLE_CAPABILITIES.md) |

## Finish the vehicle service and existing controls

| ID | Gap / status | What we should handle | Existing owner / evidence |
| --- | --- | --- | --- |
| G37 | Deferred physical writes — Missing | Correlate requests with pending/completed/failed/timed-out provider commands and original confirmed-state dates before returning success | Current phone writer is synchronous; the new confirmed-state provider intentionally rejects writes; [commands](VEHICLE_COMMANDS.md) |
| G38 | Multi-IID writes — Partial | Establish batching/atomicity/per-IID errors and response reservation before any physical side effects | 1..32 structurally validated unique IIDs now receive independent values/errors; transport reserves response capacity before callbacks; physical atomicity is not promised; real-phone batch/error compatibility remains open |
| G39 | Action/control requests — Missing | Actual control objects and action-specific permissions/completion, not scalar write aliases | `controlRequest` returns unsupported; configuration controls arrays are empty |
| G40 | Configuration changes — Research | Capability removal/change and reconnection policy with stable semantics and proper config notifications | File profiles are fixed for a connection; changing a file does not update an active model |
| G41 | Priority/subscription ownership — Partial | Determine per-channel/per-characteristic priority, registration lifetime and delivery behavior under close/reopen | Both vehicle sockets share one model/subscription set; normal channel is preferred for updates |
| G42 | Durable settings and external changes — Partial | Qualified persistence where appropriate, real external-state updates, conflicts and reconnect restoration | Equal-acquisition conflicts now withdraw readings and cannot revive via replay; live simulator is memory-only and no physical persistence/reader is installed |
| G43 | Full Climate matrix — Qualify | Per-zone cabin power/AC/max states, fan/Auto, recirculation, defrost, sync, relative temperature, seat cooling/ventilation and wheel states; retain confirmed readback under loss | Profiles build many of these; individual phone/control/hardware evidence remains incomplete; [matrix](CLIMATE_QUALIFICATION.md) |
| G44 | Vent placement — Partial | Required-only Vent comparison and a supported layout without overlap; preserve working settings airflow meanwhile | Reproduced original phone-video overlap; current workaround is live accepted |
| G45 | Temperature and synchronization truth — Partial | Measured cabin temperature distinct from setpoint, units/age, coordinated zone synchronization and realistic applied state | Bench can mirror current/target temperature; a sync Boolean does not synchronize a car |
| G46 | Remaining settings/Climate families — Research | Buttons, deep links, image selectors, nested static settings, ClimateControlsLocked and AutoClimateControl intensity | Exact action/resource/enum contracts are absent; [capability catalogue](VEHICLE_CAPABILITIES.md) |
| G47 | Rich setting policies — Research | Category, restriction class, prominence, detailed description, audio-brand resources and vehicle placement | Basic labels/order/hidden/disabled exist; richer metadata is not configured |
| G48 | Phone-facing car telemetry — Partial | Capability-gated vehicle speed/RPM/transmission/modes/fuel/conditions/trip/units as CAF services where the phone requires them | V66 includes opt-in read-only Trip/restraint/closure/tyre declarations. Physical acquisition and complete phone presentation remain open; broader speed/RPM/fuel/mode CAF families are not universally implemented. Local gauge data is separate |
| G49 | Trip completeness — Partial | Source-owned dated accumulation/reset/persistence, full trip counters and confirmed phone content route | HVO4/native trip and local fuel-efficiency binding exist; no qualified Apple distance/time/average-speed/heading bindings; [trip](TRIP_AND_HEADING.md) |
| G50 | Physical observation integration — Qualify | Deploy the existing broker/typed acquisition path, calibrated car provider, original clock mapping and generation/freshness behavior | No physical vehicle provider is qualified; legacy simulated gauges are not acquisition evidence; [adapter](VEHICLE_ADAPTER_PLAN.md) |
| G51 | Warning/indicator completeness — Qualify | Qualify active/off/flashing/stale placement on live styles and optional phone-facing indicator/seatbelt/proximity services | Shared native/Ultra artwork and startup policy exist. Source 443c319 adds top arrows and paired hazards; V66 retains bottom arrows. Deployment/live placement and physical lamp inputs remain open; [warnings](ULTRA_WARNING_OVERLAY.md) |

## Add only the capabilities the fitted vehicle needs

These gaps are conditional. For an older combustion car, EV charging is not a
missing required feature. For a car without a driver-assistance system, an ADAS
graphic must not imply one. Apple describes vehicle-provided driving state and
Remote UI in its [architecture session](https://developer.apple.com/videos/play/wwdc2024/10111/);
that architecture does not require every car to implement every service.
There is no selected physical car. The user's 1998 Defender 110 example has no
drive modes, and the software must support different vehicles by configuration.
The [single-definition compiler](VEHICLE_CONFIGURATION_GUIDE.md#one-vehicle-definition)
now prepares declared identity, local signed bindings, optional modes and the
supported capability objects without a make/model code fork. This does not
close phone-facing vehicle service contracts, complete native layout/unit policies or
physical providers. No unprovided fitted-feature list is assumed.

| ID | Gap / status | Conditional work and evidence |
| --- | --- | --- |
| G52 | EV/hybrid and charging — Partial, conditional | Source includes opt-in charging declarations/readings; the V66 combustion profile omits them. Complete EV resources/model and a physical charging provider are not qualified and are outside active combustion-bench work |
| G53 | Assistance state — Missing | Actual cruise/limiter target speed/state, gear recommendations and parking/proximity signals; fixed inactive target-speed fixtures are not observations |
| G54 | ADAS graphics — Partial, conditional | V63/V66 signed ADAS displays labelled simulated lanes from our local renderer. A real assistance system/provider remains absent; do not reimplement existing graphics or describe simulation as fitted ADAS |
| G55 | Phone camera actions/focus — Partial | CameraButton reset/action, picker/selection and coordinated Remote UI/UI-Sync focus/return; existing camera publication is read-only and does not complete actions; [camera guide](CAMERA_ULTRA_GUIDE.md) |
| G56 | Physical camera path — Qualify | V68 physical USB unplug/replug and Off/On preserve both Ready displays in 88 samples; worker loss withdraws camera/content and user-assisted reconnect restores fresh requests. Phone Close/focus, isolated lease loss, repeated/automatic recovery, fresh reverse, frame timing and sustained audio/touch remain open; [views](VEHICLE_VIEWS.md) |
| G57 | Closures, restraints and tires — Partial, conditional | V66 admits opt-in read-only closure/restraint/tyre declarations and leased body/tyre requests. Overlay entry/withdrawal works but widget contents are blank; resource schema and physical providers remain open |
| G58 | Vehicle audio settings — Missing | Actual amplifier volume/equalizer/fader/balance/source and applied state, with Siri/phone integration only when the contract is known; phone Bluetooth volume and local mixer gain are different resources |
| G59 | Vehicle lighting — Missing | Actual interior ambient-light capabilities and applied controls; a simulated comfort-light switch is not the lighting service |
| G60 | Automaker notifications — Missing | Local/dynamic/remote alerts, content, dismiss/selection/action/history and priority/lifetime; reference pack paths and framework classes exist but production model does not implement them |
| G61 | Temporary content/overlays/apps — Partial | V66 deploys nonce/generation-bound, leased RequestContent and native Close; overlay withdrawal has live evidence but widget contents are blank. Whole-manager pause did not isolate expiry. General temporary-content/automaker-app contracts are not established |
| G62 | Car navigation/location — Research | Establish the required vehicle-provided location/heading/dead-reckoning/toll/navigation-aid contracts and supported navigation feedback; receiving turn guidance is not providing GPS to the phone. HUD speed-limit input remains a separate unqualified source |
| G63 | Phone/status integration — Research | Fitted UI for paired devices/user/cellular/Wi-Fi/Bluetooth status and appropriate actions; native BlueZ listing is not the phone-facing CAF paired-device service |
| G64 | Runtime units/language — Partial | Phone/vehicle preference ownership, conversion and invalid/stale handling across speed, distance, temperature, consumption and locale; local bench units are fixed and iAP language strings currently use `en` |

## Runtime compatibility, recovery and deliberate exclusions

| ID | Gap / status | Required decision or qualification |
| --- | --- | --- |
| G65 | Signed video deferral — Missing | `DPVideoLayer.deferNextPTS` explicitly throws; reference `InputStream.lua` and `APL/VideoLayer.lua` reference it. Establish its exact call/clock semantics and exercise the calling branches before implementing; presence is not proof the live run invokes it |
| G66 | Unsupported scene operations — Partial | Resolve operations actually exercised by the current pack with visual/failure evidence |
| G67 | Phone/session/output recovery — Qualify | Real worker/clock/decoder/control loss, healthy-main retention, visible backup, fresh-scene failback, reconnect/preference restoration and sustained sessions; portable or isolated GPU evidence does not finish the physical graph; [fault recovery](OUTPUT_FAULT_RECOVERY.md) |
| G68 | Pairing/removal/boot — Qualify | Fresh Add/Pair/Reject/Remove, managed per-phone key cleanup, selected-phone recovery, cold boot and rollback on retained releases; split pairing and legacy PIN entry remain unsupported, requiredness unestablished; [pairing](PHONE_PAIRING.md) |
| G69 | Media/artwork/guidance completion — Qualify | Real artwork delivery, all supported transport/progress fields, cancellation/reconnect and optional HUD guidance publication; all 256 numeric session IDs and 13 known/32 rotating unknown control IDs now have bounded delivery/dispatch/rejection diagnostics; unknown notices contain no parameters and an idle notice consumer cannot end healthy control; live artwork/guidance completion and unknown-ID semantic policy remain open |
| G70 | Feedback/keepalive semantics — Research | Determine required `/feedback` body/response and outbound `updateFeedback` semantics, route health and clock feedback; empty keepalive 200 is not synchronized playout |
| G71 | New optional feature tokens — Research | For uiContext, cornerMasks, focusTransfer, enhancedSiri, mainBuffered, videoPlayback, file/log transfer, iAPChannel and sessionManagement, establish token + structures + consumer + failure/lifetime obligations before negotiating. Existing handling recognizes only a subset; `getLogs` needs its own bounded/private policy |
| G72 | Other transports/routes/protocol tests — Research | Explicitly decide requiredness of wired USB CarPlay, property/FLUSH/parameter/pair-add/remove/PIN/FairPlay-style routes and unknown screen opcodes. This wireless receiver does not implement those as generic compatibility promises. CAF TestingUseOnly/performance/type-test services are test apparatus, not car features to advertise |

## Account for the entire recovered service catalogue

The dated 9 October 112-service catalogue classifies 20 services as configuration
builder support, two as read-only camera experiments and 90 as research-only.
These historical labels describe **phone-facing services**, not the local signed
cluster, and predate the opt-in content builders. They are not current
implementation counts or a mandate to implement the entire catalogue.
G48 therefore does not say that our local RPM/speed gauges are missing.

The following groups partition all 90 research-only service names; the JSON
retains every numeric type and declared characteristic fact, including the
supported services. This closes the names-level inventory without presenting
an identifier table as a finished implementation contract.

| Relevant work | Research-only service names |
| --- | --- |
| G23/G48/G49/G50/G51 | DisplayedSpeed, TransmissionStatus, DriveMode, EngineRPM, EngineTemperature, FuelLevel, FuelRemainingRange, FuelConsumption, RemainingRange, Odometer, Trip, ExteriorConditions, InteriorConditions, TurnSignals, SeatBelt |
| G46/G47 | ButtonSetting, DeepLinkSetting, SingleSelectImageSetting, MultipleSelectImageSetting, ClimateControlsLocked |
| G52 | BatteryConditioning, BatteryLevel, BatteryRemainingRange, BatteryTemperature, EnergyConsumption, EnginePower, EnginePowerLevel, ChargingRate, ChargingStatus, ChargingTime, MinimumChargingLevel, TargetChargingLevel |
| G53 | GearRecommendation, TargetSpeed, ProximityAlerts |
| G55 | CameraButton, Picker |
| G57 | ClosureState, TirePressure, TirePressureMonitoringSystem |
| G58 | Equalizer, SoundDistribution, Volume, MediaSource |
| G59 | InteriorAmbientLights |
| G60 | ActionRemoteNotification, ActivityIndicator, AlertRemoteNotification, AutomakerSettingsRemoteNotification, DynamicContentElement, DynamicLocalNotification, HistoricalNotification, LocalNotification, MinimalRemoteNotification, MultiSelectRemoteNotification, RemoteNotification, SingleSelectRemoteNotification |
| G61 | AutomakerApp, OverlayView, RequestContent, RequestTemporaryContent |
| G36/G62/G63/G64 | CustomImageArchive, VehicleVariant, VehicleInformation, Route, DisplayUnits, Locale, BluetoothStatus, CellularStatus, CurrentUserStatus, PairedDevicesAction, PairedDevicesInformation, WiFiStatus |
| G24/G29/G30/G34/G48 | EngineGaugeUI, UIAppearance, UIConfiguration, UIInputDeviceButton, UIState, NowPlaying, DeepLinkStatusItem, StatusItem |
| G72 — intentionally not production car features | ProtocolPerfTest, TestControlAsync, TestControlEvent, TestControlSync, TypeTest, TypeTestIndexByPosition, TypeTestIndexByUnit, TypeTestMulti, TypeTestWithStates |

## Bounded live evidence queue

These are reference qualification cases, not a second work order. Execute only
the cases needed by the current roadmap and available equipment; prior scoped
results remain valid within their documented boundaries.

The protocol-gaps pass implements G02's existing media-consumer subset and
repairs its observation/retirement boundaries, with G11 diagnostics and G18
mixer coverage. G02/G11/G18 remain **Partial**. All other row statuses above are
unchanged by this pass. Mac portable tests pass 141/141 and affected sanitizer
suites 2/2. Following the archived incomplete CM5 build, the manager granted an
exclusive slot: the control probe and seven affected test targets build under
GCC 14.2 and Clang 19.1.7 ASan+UBSan, with **7/7 suites passing in each**
(8.43 s/8.87 s). Linux leak checks use only the existing external suppressions.
These encrypted-session and silent-sink/backpressure fixtures are component
evidence for source `72a3e5c`; the manager's combined matrix remains separate.
Receipts are archived, no owned process remains and the own workspace is removed;
the CM5 slot was released at 16:16:53 UTC on 9 October. No deployment, phone
action, physical audio acceptance or new optional offer occurs.

The following capture queue makes the remaining live dependencies explicit.
It is a plan for the exclusive live owner, **not authorization for this worktree
to run it**. For each run, first record source/release, phone model/OS, installed
profile and fresh launch/connection generation. Cap each individual discovery/transition test at one session and
three minutes (five for the audio sequence; the G67 sustained run has its own
explicit limit), at most 32 command bodies per
channel, 64 KiB each, and 64 numeric transport/service/IID summaries. Retain raw
payloads, phone identities, URLs and captures privately; commit only sanitized
shapes, counts, semantic results and dated receipts. Include one deliberate
rejection/withdrawal and a fresh reconnect where specified. Stop on an owner
fault or loss of either panel. Do not inject unestablished commands into a live
phone. A capture establishes only its named case, not all items sharing a row.

| Items still needing live evidence | Exact bounded capture / prerequisite |
| --- | --- |
| G02, G18, G22 | With a configured real output and known music, capture mainAudio `resourceID/entity/permanentEntity`, worker requested/configured permission, duck commands, user gain/mute and dated sink plus audible observations. Observe controller → accessory → controller through a legitimate native-source switch; repeat with a retained duck and user mute. Record any already accepted output tail; do not equate the ACK with silence. A screen-only update must leave audio unchanged. End by disconnect/reconnect and verify no prior denial/duck leaks. No speakers/native source are presently qualified. |
| G02, G16, G17, G33, G55 | Starting once in native Home and once in CarPlay, initiate a genuine phone-side Siri interaction, one incoming call and one accepted camera entry/exit in separate runs. Capture both control channels' modesChanged resources/appStates (including speechMode and omitted fields), UI-Sync focus request/response sequence with private token correlation, native request/lease withdrawal/return, original main/cluster receipts, HID release/renewal and audio. Observe current/permanent disagreement and restoration; never map mainScreen to cluster withdrawal or to signed camera focus from its name alone. Voice/call tests first require qualified type-100/mic consumers; camera requires its known read-only bridge. |
| G01, G04, G05, G11, G12, G69 | Open a phone app and use one legitimate Home/Back/media/guidance action. Capture actual main/event methods/types, suggestUI URL list shape (including empty if naturally emitted), original send CSeq, final status and semantic result; retain timeout/rejection/loss and fresh generation. Use observed unsupported traffic only to evaluate 400/501 compatibility. Capture artwork size/type/completion and dated guidance cancellation rather than relying on title text. |
| G03 | One normal authenticated Bluetooth → Wi-Fi handoff: capture the current peer's disableBluetooth request, exact Bluetooth owner/peer state before/after, control continuity and disconnect/reconnect. Determine peer-specific scope; no global adapter-off experiment. |
| G06, G07, G08, G09, G10, G20, G21, G70 | One normal startup/play/pause/disconnect and one genuine reconnect: capture `/info`, complete SETUP stream-array order/formats/IDs, RECORD bodies/order, selective TEARDOWN and any setUpStreams/tearDownStreams/flushAudio/feedback commands. If the phone naturally reopens one video, record fresh key/ID, first config/keyframe, unaffected-screen/UI/audio receipts and pending completions. Packet/retransmission qualification needs separately bounded network-loss injection by the live owner with private RTP/control records; parser counters alone do not prove loss. |
| G13, G14, G15, G19 | After explicit output/mic qualification, play music plus one Maps prompt, then phone-side Siri and one call in separate runs. Capture negotiated types/categories/formats, actual microphone input, downlink/uplink RTP timestamps, clock epoch and dated sink observations, audible prompt/duck/return and loss retirement. Do not offer unsupported formats to provoke discovery. |
| G23, G24, G25, G26, G48, G64 | Use an explicit fitted profile and genuine dated mode/appearance/restriction/unit/locale observations; one known-value change, one unavailable/expired value and one reconnect per feature. Capture offered CAF service/IIDs, required metadata, exact phone commands, signed serialized requests/confirmation and both panels. Mode remains absent for the Defender example. Restriction requires a real policy; missing speed cannot create parked state. |
| G27, G28, G29 | With a fitted qualified input adapter, one press/release/cancel or rotary step per descriptor: capture offered report IDs/modes, hidSetReport/InputMode/CopyInputMode shapes/status and signed button purpose/event, plus phone response and retirement release. Single-touch success does not qualify the other descriptors. |
| G30, G31, G32, G34 | With fresh matching-launch readiness, one accepted/rejected center-content or palette/background choice per signed layout; retain ordered UI-Sync request/response, original phone-encoded frame and both physical panels. For Greeting, capture its genuine startup trigger once and a reconnect, including signed event/once-per-session behavior; a local replay is insufficient. Media/Trip/widgets each need their actual content route and dated complete/missing input. |
| G35, G36 | Only after a legitimate phone-delivered transfer is observed: capture service/feature negotiation, archive manifest/identity/type, bounded chunk order/final result, trust rejection and atomic install/rollback. Keep signed archives private. Separate vehicle-resource archives from cluster packs. |
| G37, G38, G39, G40, G41, G42 | With a confirmed-state provider fixture and later physical adapter, one single write, a naturally issued multi-IID batch, supported action and external-state change. Capture exact IID/control objects, per-IID errors, pending/completed/provider dates, subscription/channel priority, close/reopen and reconnect. Configuration-change notification requires an established wire contract before testing capability removal. No physical atomicity follows from independent scalar replies. |
| G43, G44, G45, G46, G47 | One feature/zone per run against the unchanged working Climate/settings baseline: capture configuration and all relevant IIDs, phone write/readback/subscriptions, measured versus set temperature with units/dates, and phone-encoded UI. For Vent, compare required-only configuration and existing workaround without replacing the baseline. Advanced actions/images/restriction metadata first require their exact payload/enum/resource contract. Physical sync/application needs the car provider. |
| G49, G50, G51 | With a calibrated provider, capture original acquisition/clock mapping and generation, fresh/off/flashing/stale/unavailable readings, disconnect/reconnect and both signed/native output. Trip needs source-owned reset/persistence and an actual phone content route; warning fixtures do not qualify a sensor. |
| G52, G53, G54, G57, G58, G59 | First select fitted equipment and a qualified provider in the hardware repository. One declared service/characteristic per run: capture real units, valid/unavailable/error observations, applied commands where fitted, configuration admission and phone/signed presentation. ADAS additionally needs a real producer and retirement frames. Never fabricate EV, assistance, closure/tire or amplifier/lighting capabilities to obtain a sample. |
| G55, G56, G61 | One manual/reverse camera entry/exit using the established signed slot: capture camera On aliases, action/picker/reset/selection, UI-Sync focus/return, original physical capture/import/fence dates, phone/native touch and both panels' fresh streams plus active audio. Include capture-source/owner loss and withdrawal. General temporary-content URL/zone/dismissibility requires its own genuine phone route; camera entry cannot establish it. |
| G60, G62, G63 | With declared resources/providers, capture one legitimate alert/action/dismiss lifecycle, car-location/heading/navigation exchange or phone/status action per feature, exact service/IIDs and privacy-safe semantic result plus close/reconnect. These need wire-contract discovery before implementation; native device listings and received turn guidance are separate interfaces. |
| G65, G66 | Trace one accepted signed pack's actual operation/call branch and original phone clock/frame/PTS plus resulting composition/error. deferNextPTS needs the real argument/clock relationship before implementation. Unsupported rendering cases need an exact signed call and pixel reference; not every operation is required by every pack. |
| G67, G68 | Exclusive owner plans separate fault and boot runs: bounded renderer/control/clock/decoder loss with healthy-main retention, physical backup and fresh failback; fresh Pair/Reject/Remove, reconnect and cold-power/rollback with genuine readiness. Preserve pairing unless the case explicitly needs a new identity; For sustained recovery, plan a separate ten-cycle Disconnect/Connect run (two minutes maximum per cycle), followed by one 30-minute session, recording readiness/frame age, clock epochs, restarts and failures once per second. Report the exact completed count/duration; an early stop does not pass the gate. |
| G71, G72 | Observe normal capability offers and unknown routes/opcodes with the bounded inventory first. For each optional token or transport, establish structures, attached consumer, completion and retirement obligations before offering it. Wired USB needs its own hardware session. Log/file transfer needs a private bounded collection policy. Test-only CAF services remain excluded from production. |

## Evidence needed to close an item

For implemented behavior, retain the exact source/profile/release and phone/OS,
capability offer, request shape, bounded semantic result, original acquisition
or applied-state confirmation, real presentation/audio evidence where relevant,
and timeout/loss/retirement/reconnect results. A transport ACK, successful compile,
component test or callback execution closes only that boundary.

The former audit sequence is superseded by [ROADMAP](ROADMAP.md). Existing
command/resource/event diagnostics and voice consumers are foundations, not a
new implementation pass. Use these IDs for specific observed contracts within
the twelve remaining workstreams; conditional services and unobserved optional
transports are not automatically scheduled.

Research sources are the pinned framework listings in the catalogue, the external
reference-pack facts, the project's original implementation and topic evidence,
and the [independent SDK/binary command inventory](https://github.com/lvalen91/carlink_linux/blob/main/docs/CARPLAY_CAPABILITIES.md).
The latter supplies command/token research leads, not proof that the current
iPhone requests them. No original cloned LIVI source was inspected or reused.
