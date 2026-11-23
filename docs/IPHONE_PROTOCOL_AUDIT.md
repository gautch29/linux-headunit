<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# iPhone interface audit

Reviewed **9 October 2026**, initially against source `89fb3eb`; route tables
include the subsequent `development branch` repairs. This is an inventory
and gap review, not a complete Apple specification or a release qualification.
It covers the receiver's control dispatcher, event channel, stream setup,
iAP2 engine, UI-Sync bridge, vehicle-state service and asset-transfer component.
[Drive modes](DRIVE_MODES.md) owns the mode integration details.

The broader [missing-behavior inventory](IPHONE_PROTOCOL_GAPS.md) adds 72
prioritized work items across command semantics, audio, signed-scene inputs,
vehicle providers, optional capabilities and qualification. It accounts for all
112 recovered services and distinguishes missing code from unfinished evidence.

## Findings that change the next work

1. An observed vehicle mode can reach the local signed cluster **and the phone**.
   The inspected signed controller subscribes to the local mode and emits
   UI-Sync command `0x14` with `data.driveMode`. It is a headunit-to-phone
   presentation request, not permission for the phone to change the drivetrain.
   The selected V27 local profile has no drive-mode bindings. An isolated CM5
   probe now verifies both signed entrypoints emitting identifier-string `0x14`
   requests, reordered mappings and unknown/expiry/restart suppression. Phone
   consumption and mode-specific layouts remain open.
2. The repair source consumes bounded `modesChanged` observations on both
   sockets. Applied screen/audio/Siri/call focus is still missing. `suggestUI`
   and `disableBluetooth` now return 501 rather than diagnostic success.
   AirPlay resource/app modes remain separate from Sport/GT/Eco vehicle modes.
3. Unknown event routes and unknown command types now return 501 with bounded
   diagnostics. Late command names and method/path pairs on both channels now
   retain bounded numeric fingerprints separately from known categories, with
   totals/statuses/evictions; labels, URLs and parameters are not retained.
   The real phone must qualify this changed error policy;
   synthetic peers cannot establish compatibility.
4. Optional audio `/info` advertises type-100/101 profiles, including microphone,
   speech and telephony categories, although SETUP implements only type-102 AAC
   media. This bootstrap compatibility offer is explicitly documented in the
   source; it is still a capability/implementation mismatch to qualify.
5. A standalone asset-transfer parser exists, but production attaches UI and
   vehicle consumers only. Automatic phone-to-receiver pack replacement is not
   implemented by the installed-pack path.

## Evidence and limits

Source was frozen into the separate `development branch` worktree to
avoid mixing concurrent cluster-selector edits into this review. Runtime is a
different generation: the live receiver service referenced
`cm5-20261009-clock-v24`, while the dated handoff still described V23. The
receiver and Home services were active with zero systemd restarts when checked.
An early status snapshot said Ultra/ready; a later publication, 0.132 seconds
old, said disconnected. This pass performed no receiver restart, radio change,
display takeover, actuation or new live phone experiment. These observations
do not establish a fresh V24 end-to-end qualification or explain the disconnect.

Read-only inspection covered **245 retained `cm5-live/session-*/receiver.log`
files**, and their privately saved `command-*.plist` samples. These contain
different dates, builds, failed attempts and possibly diagnostic runs; counts
are corpus observations, not unique phone actions or current-session evidence.
The last eight sessions were also inspected separately. Only protocol names,
numeric command categories, field names/types and counts were extracted.
Credentials, phone identities, URLs, video, raw packets and signed source remain
outside Git.

| Retained corpus observation | Count | What it establishes |
| --- | ---: | --- |
| Pair-setup log entries | 523 | The pairing path was exercised; entries are individual messages, not successful pairings |
| Pair-verify log entries | 402 | Same limitation for verification |
| Auth-setup answered | 201 | Handler returned its authentication response |
| SETUP allocations | 957 | Session/stream responses, across generations |
| `/info` answered / RECORD accepted | 194 / 194 | Those control stages ran |
| Diagnostic command acknowledgements | 4,288 | Replies, not implemented command semantics |
| Feedback acknowledgements | 31,625 | Keepalive replies, not playback-clock accuracy |
| Saved command types | `modesChanged`: 1,422; `disableBluetooth`: 189; `suggestUI`: 129 | Names in bounded private samples; sampling does not cover every command |

The source already caps main-control samples to the first 32 commands and 64 KiB
per connection. UI shape diagnostics also have a limit per panel. Event command
bodies are not sampled by that main-control collector. Encrypted raw captures
cannot supply missing plaintext without session keys. Absence from this corpus
is therefore **not evidence that the iPhone never sends or accepts a route**.

## Control listener: iPhone → head unit

Owner: [control_requests.cpp](../src/control_requests.cpp), bounded request
parsing in [control_request.cpp](../src/control_request.cpp). Exact URL matching
is used for named paths; SETUP/RECORD/TEARDOWN/OPTIONS dispatch by method, accepting
RTSP session targets rather than requiring a fixed URL. HTTP/1.1 and RTSP/1.0 framing are accepted. Main and event requests share bounded
start-line/header validation. Ambiguous lengths, duplicate CSeq/HKP, invalid
numeric fields, folded/malformed headers, control bytes and transfer encoding
reject before dispatch; incomplete bodies wait within the existing bound.
This is not a general REST API.

| Request | Actual behavior | Evidence / remaining gap |
| --- | --- | --- |
| `GET /info` | Binary plist: identity, displays/view areas, HID, optional audio, Ultra and configured vehicle metadata; switches to Standard description after negotiation | Live stage and component checks; no guarantee every advertised capability is implemented |
| `POST /pair-setup` | TLV8 SRP, methods 0 and optional MFi method 1; persistent and transient exchanges; encrypted control starts after the correct final plaintext response | Live normal path; synthetic method/flag coverage; split pairing unsupported |
| `POST /pair-verify` | Stored controller verification, X25519/Ed25519 and encrypted-control transition | Live and synthetic; bad identities/signatures must reject |
| `POST /auth-setup` | Encrypted control plus selected MFi provider; bounded X25519/signature/certificate response | Hardware-MFi phone path observed; software identity acceptance remains separate |
| Initial `SETUP` | Allocates timing/event ports; reads offered features; agrees configured UI/HEVC/vehicle capability | Live; incomplete Ultra offer selects Standard; actual Ultra requires registered UI stream setup |
| Stream `SETUP` | 1–12 validated entries, registered types/services, ordered replies and unpublished-allocation rollback | Component batch/failure checks; live multi-stream offers unqualified |
| `RECORD` | Empty body; authenticated configured session; starts timing/presentation authority, may precede video setup | repeated initialized RECORD is idempotent; nonempty body rejects; live repeated/reordered behavior remains unqualified |
| Empty `TEARDOWN` | Authenticated response then full control/session retirement | Implemented; failure/lifetime tests; real disconnect coverage is separately documented |
| Nonempty `TEARDOWN` | Atomic selection of 102/110/111 and classic/generic 130 | Video/classic EOF and parser failures retire single-owner cipher generations; component transport isolation; Ultra shared video graph still retires on a video close |
| `POST /command` | Shared bounded validation; records mode observations, applies supported focus requests and rejects unsupported commands | observed command shapes; mode policy/handoff/suggestion semantics incomplete |
| `POST /feedback` | Authenticated session gets empty 200 response; body has no feedback-clock interpretation | Live keepalive; playback feedback semantics absent |
| `OPTIONS` | Authenticated empty 200; no complete `Public` method catalogue | Source support; no OPTIONS count found in reviewed corpus |
| Everything else | 501, except malformed recognized inputs returning 400 | unknown methods/paths are not capabilities; unknown event routes now also reject |

`/pair-add`, `/pair-remove`, `/pair-pin-start`, `/fp-setup`, property endpoints,
GET_PARAMETER/SET_PARAMETER and FLUSH variants have no handler here. They are
examples of absent surfaces, not a claim this phone requires them. Fresh pairing,
PIN/UI policy and identity removal must be tested through the existing pairing
owner; do not invent a success route for an unsupported request.

Control sends have an absolute two-second deadline per send call; failed partial
responses retire the connection. This does not make the control owner asynchronous.
Malformed encrypted records latch failure; cipher counters cannot be restarted.

## `/command` and event socket

The event port is allocated by initial SETUP and uses distinct authenticated
event keys/counters. Owner: [airplay_session_setup.cpp](../src/airplay_session_setup.cpp)
and [airplay_event_channel.cpp](../src/airplay_event_channel.cpp).

| Direction / type | Current handling | What still needs qualification |
| --- | --- | --- |
| Phone → HU: `modesChanged` | bounded session-local `appStates[]`/`resources[]` observations with genuine receive time/revision; partial array updates preserve omitted entries | Screen/audio/Siri/call/navigation resource ownership and restoration; these are not vehicle drive modes |
| Phone → HU: `suggestUI` | 501; sampled `params.urls` arrays have no semantic consumer | Interpretation of suggested content and display context; URLs remain private |
| Phone → HU: `disableBluetooth` | 501; sampled `params.deviceID` has no authenticated handoff consumer | Scope to current peer and authenticated handoff; no global adapter disable inferred |
| Phone → HU: `duckAudio` / `unduckAudio` | Validated dB/duration request for existing media owner; 501 if no accepting media stream | Component focus/mixer evidence; audible navigation/call continuity unqualified |
| Phone → HU: other valid command type | 501, fixed unsupported category | real-phone error compatibility; no type-specific implementation |
| Phone → HU: malformed focus/command plist | 400 on `/command` | shared bounded command parser; focus semantics use existing bounded decoder |
| Phone → event: any other method/path | explicit POST /command, POST /feedback and OPTIONS; other routes 501 | bounded unsupported counts; live peer compatibility open |
| HU → phone: `iAPSendMessage` | Ordered encrypted event POST with iAP frame bytes | Working handoff/replies; transport reply is distinct from iAP acknowledgement |
| HU → phone: `showUI` | Main implementation requests cluster `maps:/car/instrumentcluster/map` after RECORD and cluster frames | Earlier user-confirmed Maps; arbitrary URL acceptance not established |
| HU → phone: `hidSendReport` | Single-contact absolute touch report | Live main touch; multitouch, knobs, HID reverse controls unqualified |
| HU → phone: `forceKeyFrame` | Current video-owner refresh request by display UUID | Source/component recovery path; sending it is not proof of a new phone keyframe |
| Phone → HU: response to event POST | bounded CSeq/status registry, provisional/final responses, timeout/eviction/unmatched/drop counts | response confirms transport acceptance only; presentation/applied confirmation remains separate |

Independent [SDK/binary research](https://github.com/lvalen91/carlink_linux/blob/main/docs/CARPLAY_CAPABILITIES.md)
also identifies commands our receiver does not implement: `changeModes`,
`requestSiri`, `requestUI`, `updateFeedback`, `getLogs`, `flushAudio`,
`setNightMode`, `setLimitedUI`, `updateVehicleInformation`, `hidSetReport`,
`hidSetInputMode`, `hidCopyInputMode`, `setUpStreams` and `tearDownStreams`.
These are research leads, not observed requirements or permission to advertise
them. Preserve exact command spelling and payloads: a researched `requestUI`
contract does not establish our `showUI` route's arbitrary URL acceptance.

## Allocated streams and service routes

Owner: [airplay_session_setup.cpp](../src/airplay_session_setup.cpp). TCP/UDP ports
are negotiated allocations, not additional HTTP URLs.

| Type / route | Consumer and contract | Evidence / missing behavior |
| --- | --- | --- |
| Timing UDP | Qualified phone clock, original timestamps, epoch/discontinuity admission | Live exchanges; clock qualification is separate from keepalive |
| Event TCP | Encrypted request/reply and outbound commands above | shared command dispatcher; fresh control owner after event EOF/framing/write loss |
| 110 main / 111 cluster TCP | Authenticated video; packet 1 config, 0 frame, 2 heartbeat, bounded 5 auxiliary | Both real streams observed; auxiliary does not increment frame nonce; unknown kinds fail |
| 102 media UDP | Optional AAC-LC 48 kHz stereo, `audioFormat=0x800000`, `audioType=media`; data/control ports | Component/silent decoding; audible route not qualified |
| 100 / 101 audio | No SETUP consumer | Optional `/info` compatibility entries are not implemented audio streams |
| 130 classic iAP | Omitted UUID or `E9459FD0-BCAD-4C45-820F-1E72447EF2F2`; existing iAP engine | classic iAP selectively closes with its pending bytes; live reopen remains unqualified |
| 130 UI-Sync | `07D9F906-8D64-4B54-A808-A20BCA2C51C2`; per-display envelopes → connection-owned worker/VM | Live signed main and cluster; attached before feature offer |
| 130 normal vehicle | `3E2F3C61-AAD0-42CB-A8AA-BF22186DA62E` | Live Climate/settings simulation; same model as high channel |
| 130 high-priority vehicle | `FF4A6720-F2BE-4F56-A3E1-DB3B4E37D634` | Same; independent per-characteristic priority/subscription ownership unqualified |
| Other 130 UUID | Only explicitly registered generic services admitted | Unknown UUID rejects; no automatic service creation from phone input |
| Asset transfer | Standalone parser/sink API, no production service attachment | Preinstalled verified pack works; fresh transfer/install lifecycle remains open |

Initial SETUP recognizes `uiSync`, `hevc`, `vehicleStateProtocol`; it always
includes `altScreen` and `viewAreas` in enabled features. The UI/HEVC pair is
conditional on an attached native Ultra consumer. Other feature strings are
ignored. Feature agreement is not display readiness. A Standard selection omits
Ultra vehicle metadata; standalone explicit empty-vehicle diagnostics differ.
Audit future Siri, UI-context, corner masks, focus transfer, file/log transfer,
buffered-audio and session-management offers before agreeing them.

All stream types reserve shared key-derivation IDs across retirement; event
keys cannot restart on reconnect. Generic streams have independent cipher state, bounded
queues and single accepted TCP connections. Seeds remain reserved after close;
reopen uses a fresh seed and ID. Eight registered services and twelve total
stream allocations bound a control session. Tests cover malformed selective
teardown, authentication failure, EOF, backpressure and retirement isolation.

The standalone [asset-transfer component](ULTRA_ASSETS.md) accepts OPACK
`messageType=1` headers (`payloadType=1`, version, session ID, chunk count,
certificate/signature and optional opaque additions), then ordered
`messageType=2` chunks with the final SHA-256 checksum. Its terminal reply is
`messageType=5`, numeric `accepted=0` for committed or `5` for failed. A transport
receipt is separate. This parser does not establish a production service UUID,
consumer attachment, trust/install policy or phone receipt of the final reply.

## iAP2 control and file messages

Owner: [iap2_session_engine.cpp](../src/iap2_session_engine.cpp), shared across
Bluetooth and authenticated Wi-Fi handoff; session 10 carries control, session
12 the optional bounded artwork transfer.

| Direction / ID | Function | Support / evidence |
| --- | --- | --- |
| Phone → HU `0x1d00`; HU → phone `0x1d01` | Identification request/reply | Implemented and live |
| Phone → HU `0x1d02` | Identification accepted | Implemented; old corpus logs contain 31 previously unhandled instances; not a current missing handler |
| Phone → HU `0xaa00`; HU → phone `0xaa01` | MFi certificate request/reply | Hardware live; selected provider owns signature/identity |
| Phone → HU `0xaa02`; HU → phone `0xaa03` | Challenge/signature | Hardware live; bounded provider/component failure coverage |
| Phone → HU `0xaa04` / `0xaa05` | Authentication failure / success | Terminal failure or completed authentication; not an unconditional ACK |
| Phone → HU `0x5702`; HU → phone `0x5703` | Wi-Fi configuration handoff | Requires completed MFi; live |
| Phone → HU `0x4300`; HU → phone `0x4301` | Availability / authenticated CarPlay start | Requires MFi; installed asset metadata advertised only when configured |
| HU → phone `0x5000` / `0x5002`; phone → HU `0x5001` | Start/stop Now Playing updates | Subscription/retry and metadata parsing; live title/artist, broader media UI incomplete |
| HU → phone `0x5200` / `0x5203`; phone → HU `0x5201` / `0x5202` | Start/stop guidance; route/maneuver data | Bounded dated model; live navigation, optional native HUD remains separately qualified |
| Phone ↔ HU session 12 | Artwork fragments/receipts | Bounded optional assembler; production display/artwork completeness open |
| Other control IDs | Parameter-free numeric notices; 32 rotating unknown coverage entries, 64 notices per output/session queue and explicit drop/eviction counts | Reliable-link ACK means packet receipt, not application handling; notice overflow does not retire healthy control |

Identification advertises optional guidance/Now Playing IDs from attached
consumers. Current code handles authentication and identification outside the
optional application lists. Handshake/reliable-link acknowledgement, application
response and useful displayed content are three distinct evidence levels.
Coverage pins 13 known control IDs and counts reliability-delivered frames for
all 256 numeric session IDs; duplicates, pure ACKs and sequence gaps do not count
as fresh delivery. Retirement retains coverage and reports bounded numeric
summaries. Unknown session/control payloads get no invented application reply.

## UI-Sync command inventory

The live route is **signed-script owned**: bounded per-display envelopes →
UiWorker → ScriptRuntime/Channel → authored session/controller. The portable
`theme::Session`/`Timeline` helpers are not a universal C++ command dispatcher
for the live signed VM. Do not add a control-thread switch that duplicates it.

| ID | Meaning | Reviewed retained diagnostics |
| --- | --- | --- |
| `0x01` | Start zone transition | Incoming VM queue observed |
| `0x02` | Transition data | Incoming VM queue observed |
| `0x03` | Start layout transition | Incoming VM queue observed |
| `0x04` | End zone transition | Incoming VM queue observed |
| `0x05` | Request layout | Outgoing worker queue observed |
| `0x06` | End layout transition | Incoming VM queue observed |
| `0x07` | Give focus | Outgoing worker queue observed; token/focus completion not implied |
| `0x08` | Request focus | Declared/bridged; not found in bounded reviewed diagnostics |
| `0x09` | Transition metadata | Incoming VM queue observed |
| `0x10` | Palette change | Incoming VM queue observed; native confirmation/background gaps remain |
| `0x11` | Allow layout change | Outgoing worker queue observed |
| `0x12` | Request appearance | Declared/bridged; not found in bounded reviewed diagnostics |
| `0x13` | Appearance preference change | Incoming VM queue observed |
| `0x14` | Request drive-mode presentation | Both signed entrypoints emit authored identifier strings in the isolated observed-mode probe; not found in reviewed live diagnostics |

Queue diagnostics prove those boundaries only. Confirmation requires the
matching session, phone transition and current rendered selection. Sequence/reset/
ACK logic belongs to each display's authored session. Stale generations and
unsent output are retired; transition delivery retains original video timing.

## Vehicle-state application requests

Owners: [vehicle_transport.cpp](../src/theme/vehicle_transport.cpp) and
[vehicle_service.cpp](../src/theme/vehicle_service.cpp). `comm`/`cmnd` payloads
contain OPACK plugin ID and encoded plugin data. Both priority sockets share
one connection-owned model; configuration and subscription scopes are not URLs.

| Incoming command | Application reply / actual behavior |
| --- | --- |
| `configRequest` | `configResponse`, or transaction-free `configNotify`; initialized fixed per-session capability tree |
| `readRequest` | `readResponse`; integer IID array; current typed state or per-IID error |
| `registerRequest` | `registerResponse`; IID array or `*`; initial readings and subscriptions |
| `unregisterRequest` | `unregisterResponse`; IID array or `*`; removes subscription state |
| `writeRequest` | `writeResponse`; 1–32 unique IIDs with validated writers and independent applied values/errors; structural failures execute no callbacks |
| `controlRequest` | `controlResponse` with unsupported error; no action executor |
| Other `*Request` / other plugin ID | `generalError` with unsupported error |
| Non-request command | No application response from the service |
| HU → phone `updateNotify` | Changed/expired subscribed values; no transaction |

A synchronous transport `rply` is only an outer receipt; the application response
is a separate asynchronous `cmnd`. Capacity is reserved before calling writers
so backpressure retries do not reapply a command. Provider loss withdraws dynamic
readings. Reopen/full-disconnect lifetimes and two-priority behavior are described
in [vehicle-state integration](VEHICLE_STATE_GUIDE.md). Physical vehicle writes,
camera controls, RequestContent and notification action lifecycles are not
completed by this generic request engine.

## Broader iPhone vehicle catalogue

[VEHICLE_PROTOCOL_CATALOGUE.json](VEHICLE_PROTOCOL_CATALOGUE.json) accounts for
every entry in the pinned iOS 26.1 accessory and service type maps: **34 accessory
families and 112 service families**. Each has its numeric identifier and reviewed
receiver-support category; source URLs/revision/hashes make the scope explicit.
Only facts are retained, with no framework implementation. A research-only entry
is not advertised by this receiver, and a file-configurable entry is not a
claim that a real phone accepted every control.

Twenty service families are configurable through Climate/Seat/automaker settings;
two belong to the read-only camera experiment. The other ninety service families
remain research-only **as phone-facing resources**. Some equivalent readings,
such as speed/fuel/transmission/drive mode, exist in the separate local script
bridge; that does not implement their remote vehicle-state service.

For detailed controls and per-feature qualification use
[VEHICLE_CAPABILITIES](VEHICLE_CAPABILITIES.md),
[CLIMATE_QUALIFICATION](CLIMATE_QUALIFICATION.md),
[CAMERA_ULTRA_GUIDE](CAMERA_ULTRA_GUIDE.md) and
[CLUSTER_COMPLETENESS](CLUSTER_COMPLETENESS.md). Classes for notification/content
requests, tire/ADAS/EV data, vehicle resources, audio/media settings, closures,
lighting, paired-device/status, navigation and UI control are not silently
covered by the existing Climate page. A numeric type map alone does not establish
required characteristic payloads, permissions, state machines or phone behavior.

## Next qualification work

1. Use an explicit simulated drive-mode catalogue and signed mapping in an
   isolated fixture. Verify reordered modes, Sport/GT, unknown/expiry and
   outgoing `0x14` shape; then qualify the phone's actual transition on both
   panels in a new retained generation. Do not replace the selected release's profile in place.
2. Extend the current service/IID and numeric iAP coverage to late unknown
   command-name/route diversity. Qualify the changed unsupported-response policy with
   the phone, preserving encryption order and bounded queues.
3. Complete applied resource policy for the observed mode state and establish
   authenticated Bluetooth handoff/UI-suggestion semantics. Test
   requested versus observed screen/audio ownership, handoff and retirement;
   do not conflate AirPlay modes with a vehicle drive-mode command.
4. Reconcile optional audio offers with implemented streams before Siri/call/
   microphone work. Keep the working bootstrap profile recoverable while
   qualifying any reduced or expanded offer with the actual phone.
5. Exercise cold/fresh pairing, Standard and Ultra reconnect, Home/Back,
   customization, Climate/settings, maps/media, camera and audio actions with
   per-generation route coverage. Add newly observed interfaces to this
   inventory. No finite capture set can prove every future iOS route is covered.

## Review validation

The latest repair implementation is `49eeee4` (following `f134e94`). Current component inputs, tests and
remaining lifecycle limits are owned by
[control](AIRPLAY_CONTROL.md#protocol-repair-source--9-october-2026) and
[vehicle state](VEHICLE_STATE_GUIDE.md#supported-exchanges). V27 remains selected;
these table updates do not qualify a new release or live iPhone behavior.
The following records describe the original frozen audit, before those repairs.

An isolated Debug build of source `89fb3eb` completed on macOS with AppleClang
21, the pinned Luau dependency and the documented libarchive prefix. **42 scoped
portable suites passed**: control parsing/response deadlines, HAP, AirPlay
transport/session/clock/media codecs, iAP2, guidance, vehicle protocol/configuration/
observations, gauge input and signed-worker/transport/profile/session components.
Catalogue checks validate all 34/112 entries, unique names/types and support
categories; documentation link checks and `git diff --check` pass.

The production sealed **V24** binary also passes the existing Linux authenticated
loopback smoke: normal pairing/verification, encrypted `/info`, initial/stream
SETUP, main/event diagnostic command acknowledgement, malformed/unavailable
focus rejection with session survival, RECORD, selective-teardown presentation
survival, full teardown retirement and bad-signature rejection. The fixture
uses temporary test identities and unused port 7001, attaches an explicit empty
vehicle model, and starts no Bluetooth, Wi-Fi, GPU or physical-display owner.
It does not connect an iPhone. Binary SHA-256:
`0d532cbc3d2a6a6f662c016fbe941e0603d04bcbcd7059b692df58ee57f7d74e`;
`tools/smoke_pair_setup.py` SHA-256:
`830bdb34e17620d235448d8191dddddd961ee3ca1a77d0cd4a5af2800e11c1a3`.

These are component checks. Signed-pack command facts came from read-only static
inspection; no new phone drive-mode, Linux/GPU, audible-audio or physical-car
acceptance is claimed. Source CI on the parent commits was failing at review
start; this documentation branch does not authorize sealing a release.
