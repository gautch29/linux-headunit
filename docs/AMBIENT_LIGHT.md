<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Screen and drive-mode ambient lighting

Source-only controller foundation (13 October) and manager bridge (14 October 2026).
No appliance service, physical executor,
MCU firmware or LED is enabled. Native GTK pages remain with native UI. The signed
pack is read-only and unchanged. This feature is original project code.

## Controller and authority

`headunit::ambient::Controller` is a bounded owner-thread component. It holds one
ambient target, one command coordinator and one latest derived target; it does
no I/O, VM/GPU work or PWM scheduling. `Settings`, `Inputs` and `Target` are plain
value types in `include/headunit/ambient_light.hpp`. `derive` is also available
independently for previews; its result is desired state, never hardware state.

The selection rules are explicit:

| Selection | Colour source |
| --- | --- |
| `automatic` | Fresh observed screen accent, else fresh mapped observed drive mode; otherwise unavailable |
| `screen` | Fresh observed screen accent only |
| `drive_mode` | Fresh mapped observed mode only |
| `fixed` | Explicit configured RGBW colour |
| `enabled: false` | Explicit Off target (brightness zero); current remains separately confirmed |

A Sport transmission selector is not a vehicle drive mode. No active mode is
inferred from a user preference, saved style, speed or RPM. A configured mapping
for `sport` does not claim that the car supports that mode. Mode membership,
acquisition and immutable age policy come from the existing vehicle source.
Ambient consumes existing drive-mode and low-beam observations; it changes no
HVO version. Its fast colour/lamp input age must be positive and at most three
seconds. Missing, malformed, future, stale or unmapped values are unavailable.

`Inputs::screen` is an optional RGBW observation with source generation,
start, original acquisition, age, availability and provenance. Screen generation
ordering, immutable age/start, equal-date conflicts and post-withdrawal replay
are checked by the controller. Use the existing qualified vehicle receiver/cache
for car observations; the ambient component is not a replacement acquisition
cache. The screen source epoch must advance across receiver launch nonces;
a new nonce with connection generation 1 cannot match a prior consumer epoch.

Night brightness calls **the existing `night_mode_at` in `src/night_mode.cpp`**.
Use the same `HEADUNIT_NIGHT_MODE` policy as the receiver. Under `headlights`, a
fresh dated low-beam off/on reading chooses day/night; when unavailable, the
schedule supplies the fallback. The caller supplies actual local wall-clock
minutes (0–1439). Missing wall time makes the schedule unavailable, never midnight.
Forced on/off does not require wall time. A derived night decision is a host
appearance policy, not a manufactured observed car night-mode signal.
Targets preserve colour/low-beam acquisition and origin, and identify whether
night came from headlights, schedule or explicit policy. A target preview expires
at the earliest relevant input expiry or one second after derivation.

## Configuration and data for native UI integration

`ambient::parse_settings` accepts the complete bounded, strict 16 KiB JSON
configuration in [ambient-light.example.json](../config/ambient-light.example.json):
`version`, `enabled`, `selection`, `fixed_rgbw`, `day_brightness_permille`,
`night_brightness_permille`, `transition_ms`, `night_policy`, `mode_colours`.
All fields are explicit; unknown/duplicate keys, wrong types and duplicate modes
reject. RGBW is four bytes, brightness is 0–1000 permille, night brightness cannot
exceed day, transition is 0–5000 ms and mode colours have at most 32 semantic IDs.
Examples default disabled. They are illustrative colours, not signed-pack palette
measurements or calibrated LEDs. `Controller::settings(Settings)` changes policy;
`Controller::settings()` returns the complete validated settings for a UI owner.
The opt-in software-only manager binding below adds a settings action; durable
persistence and physical execution remain unbound.

`ambient::snapshot_json(controller, now)` supplies the transient model:

- `version: 1`, `sampled_ns`: original host snapshot date, decimal string.
- `settings`: enabled, selection, day/night intensity and transition summary.
  The complete fixed/mode colours and night policy remain in the validated settings
  configuration/API; a UI should show those values from that configuration.
- `requested`: optional derived value, source, derivation/expiry dates, night reason
  and colour/headlight observations with generation, original date, age and origin.
- `current`: optional independently confirmed actual RGBW/brightness, generation,
  original confirmation date, age limit and measured/calculated/simulation origin.
- `command`: exact issued value, ID, provider generation, status and failure reason.

`requested` can change while an earlier `command.value` is pending: they are
intentionally separate. `current` is absent before confirmation and after expiry
or provider loss. It may differ from the request when the MCU clamps output.
Even an Off request cannot set the current swatch to black before acknowledgement.
During a fade, independently dated intermediate readbacks can supply current
state while the command stays pending. Register/PWM readback is not optical proof.

`tools/ambient_light_model.py` has two pure functions, with no GTK or I/O:

```python
accent = screen_accent(validated_cluster_state, palette_colours,
                       (receiver_nonce, connection_generation), now_ns)
model = view_model(controller_snapshot, now_ns)
```

Use a publisher admitted by fresh manager CarPlay return-readiness.
Obtain `validated_cluster_state` from the existing
`cluster_layout.snapshot(path, owner, publisher)` admission or its manager status
model. The accent adapter accepts only the fresh current publisher, settled ready
scene, `confirmedActive == active`, no pending request and a mapped `activePalette`.
It carries the original `updatedMonotonicNs`, never the query time. `palette_colours`
is a bounded dictionary of **verified-pack-specific** palette IDs to four RGBW
bytes. Unknown IDs return unavailable; names like Vibrant or Blue are not RGB
measurements. Pending/requested choices and persisted confirmed preferences are
not a current accent source. This adapter does not observe Standard CarPlay pixel
accents or phone wallpaper; those have no qualified colour contract here.
A native renderer may publish its qualified actual accent through the same C++
`Accent` contract, independently of the signed scene. The drive-mode worker's
`native-profile` status establishes buffer selection, not compositor presentation;
use its RGB as UI configuration until a qualified display owner supplies observed
accent evidence. It cannot supply ambient actuator current state. No native renderer was restyled.

`view_model` returns `available`, `settings`, `requested`, `current`, `command`,
with `None` for unavailable values. It independently withdraws expired target or
actual state and rejects a stale/future model. Dates/generations are decimal
strings to preserve nanosecond/integer precision. It deep-copies returned data.
On Linux the owner uses the existing `carplay_readiness.clock_ns()` /
`CLOCK_MONOTONIC` domain. Do not substitute Python's platform-dependent uptime
or fabricate an offset. Portable interop evaluates the original C++ snapshot
at its supplied date; it does not qualify a Mac screen-publisher clock mapping.
native UI can render settings, desired/current swatches, source, pending/error and
simulation labels from this model. Keep the established one-outstanding-request
and conservative reply-age policy when wiring it into manager/native UI.

## Manager software interface — 14 October source-only

The existing `PublishingVehicle` / `PrephonePublisher` in the native cluster
owns one optional `ambient::SoftwareModel`, containing the actual existing C++
Controller. `tools/ambient_light_status.py` validates its bounded wire projection
and passes that original snapshot to the unchanged `ambient_light_model.view_model`.
Python performs no RGB, drive-mode, night or command derivation. No standalone
service, process, thread, acquisition endpoint, logger or user-space queue is added.
The same source wrapper continues into local signed gauges/native fallback.

Receiver configuration defaults to `ambient_light: false` (or omitted). Enabling
it requires the existing sealed `prephone_publication_socket` equal to the
manager's private `stateDirectory/prephone.sock`. The shared native command gains
`--ambient-light-software`; exact unit validation rejects a missing/extra flag.
The optional sealed closure requires **both** `ambient_light_status.py` and
`ambient_light_model.py` in addition to the two existing pre-phone modules.
Disabled releases do not import/seal these ambient modules. No configuration role,
service, signed asset, Apple vehicle capability or Climate write is added.

One current native role and its original Linux peer credentials, pidfd, executable,
argv, boot scope, time namespace, nonce and retirement admit both models. Original
`HUPN001` packets, eight auxiliary rows and source deltas are unchanged. The
opt-in adds one fixed **2560-byte** `HUAL001\0` model send and at most one fixed
**2432-byte** `HUALS01\0` action receive per existing 200 ms publication attempt.
A 2368-byte canonical Settings record holds at most 32 mode mappings. Reserved,
unused, duplicate, malformed and out-of-range fields reject; C++ `valid_settings`
is the final policy admission. No native file/JSON work or blocking wait occurs.
The existing 16-message manager polling bound, one-second reconnect gate, socket
buffers and two-second carrier lease remain unchanged. Backpressure drops a
model copy and rejects an unsent action; it never queues/retries a UI operation.

Manager status calls `NativePublication.snapshots()` once: one bounded poll,
one actual post-poll monotonic date for both projections and independently copied
identical publisher metadata. A final kernel peer check withdraws **both** copies
if the owner retires during projection; it does not poll or admit a successor.
Original source acquisition/model dates remain attached after retirement.
Standalone `snapshot()` and `ambient_snapshot()` retain their respective return
shapes and each poll once. No clock offset, receive budget or freshness gate changes.

The model's one-second freshness and each original input's earlier expiry remain
independent of carrier liveness. A newer copy never refreshes mode/headlight
acquisitions. Mode/headlights come only from the same original dated vehicle
sample; legacy undated simulation, saved profiles, user drive-mode choices,
speed and RPM provide no such authority. The owner supplies actual local wall
minutes to the existing shared night-policy function; missing clock remains
unavailable. Screen input stays **unavailable**: no qualified admitted active-pack
palette RGBW observation is bound by this feature. A configured colour remains
configuration, not observed screen pixels or LEDs.

This owner **never** calls provider `begin_generation`, grants write permission,
acknowledges an actuator, or observes current lighting. `current` and `command`
are always null. Disabled policy can produce the Controller's explicit Off
*request*, without physical Off confirmation. Fixed policy can produce a requested
preview; Automatic/Screen/Drive mode without a qualified colour observation remain
null. No clocks, generations, dates or observations are manufactured for a preview.

### For native UI: UI keys, actions and IDs

New UI pieces and widget/control IDs: **NONE**. GTK and Home2 design remain
native UI; future controls reuse its established components.

| Interface | Meaning |
| --- | --- |
| `status.ambient_light.configured` | Optional software bridge configured; does not mean a car/provider exists. |
| `.available`, `.state` | Actual admitted Controller model is fresh. States include `disabled`, `publication_unavailable`, `awaiting_model`, `available`, `model_expired`, and the carrier's original retirement/failure reasons. |
| `.provider_available` | Always false; the Controller is not a provider. |
| `.settings` | Full settings from the actual fresh C++ Controller, or null. Contains existing config keys `version`, `enabled`, `selection`, `fixed_rgbw`, `day_brightness_permille`, `night_brightness_permille`, `transition_ms`, `night_policy`, `mode_colours`. |
| `.requested` | Original Controller target with RGBW/intensity/fade, source, night reason, original derived/expiry dates and mode/headlight acquisition/provenance; null when unavailable. |
| `.current`, `.command` | Null: no physical current/command owner is admitted by this bridge. |
| `.revision` | Opaque current publisher/policy revision token, or null. Retain it with the rendered settings and submit it unchanged. A successor cannot accept the predecessor's token. |
| `.sampled_ns`, `.clock_domain` | Original C++ model date (decimal string) and admitted `host-steady` domain. No Mac/phone clock offset. |
| `.publisher` | Existing carrier's live/sequence/original copy date/actual `hostBootId`/bounded admission error. Carrier copy time is not car acquisition or model derivation. |
| `.persistence` | `owner_lifetime`: policy is volatile and resets disabled on a newly admitted native connection. No saved-profile inference or settings file is created. |
| `.software_action` | Latest bounded software operation: decimal `identifier`, `status` (`pending`, `software_updated`, `failed`, `timed_out`), `reason`, and always-false `physical_confirmed`. No actuator ACK. |

Selection IDs are the existing `automatic`, `screen`, `drive_mode`, `fixed`.
Night policy strings and mode IDs use the existing ambient config contract.
RGBW has four integer bytes, day brightness 0–1000, night brightness 0–day,
transition 0–5000 ms, and at most 32 unique semantic mode IDs. Palette/profile
names are not colour measurements. The UI must keep unavailable swatches empty.

Submit a **complete** settings object with the last displayed revision:

```json
{"action":"ambient_light_settings","revision":"<opaque current token>","settings":{"version":1,"enabled":true,"selection":"fixed","fixed_rgbw":[9,37,83,0],"day_brightness_permille":300,"night_brightness_permille":60,"transition_ms":800,"night_policy":"headlights","mode_colours":[]}}
```

These sample bytes are explicit user configuration, not observed lighting.
The initial reply reports `software_action: pending`, `confirmed: false` and
`physical_confirmed: false`. Only an original fresh native reply with matching
request/revision/settings reports `software_updated`. That confirms software
policy alone. One pending operation rejects concurrent actions. Invalid settings,
wrong/missing/stale revisions, unavailable owners/models and transport failure
are explicit errors. Native handler results `invalid_settings`, `stale_revision`
and `expired_action` leave policy unchanged. The same existing one-second model
bound limits action admission/completion; no physical deadline is guessed.

Timeout retains `completion_unknown` and never becomes success on a late reply.
A later independently fresh model may report the actual new software settings,
while the earlier operation remains timed out. Owner death, future/reversed/replayed
publication, foreign nonce/credentials, expired carrier, clock failure and shutdown
withdraw the model and fail pending work. Retained original dates never revive
usable values. New admission begins disabled under a new nonce/revision. There is
no automatic retry or physical Off inference.

### Qualification and adoption boundary

Base `3670763` has no manager ambient feature; baseline component checks cannot
be called a regression reproduction of that omission. Focused Mac and Linux
checks first cover the existing Controller/simulator/model, private publication,
manager/configuration and real sealed-file/unit closure. New checks use the actual
C++ software owner, original-date portable consumer, Linux production source/cache/
socket/publisher, admitted private IPC and actual manager endpoint. They cover
invalid/stale/future/expired/replayed input, concurrent action, handler rejection,
original mode/headlight dates and unrelated deltas, source expiry/withdrawal,
process replacement, INT/TERM/HUP, pending shutdown, module closure and unit flags.
Portable stdio and dated vehicle data are explicit fixtures; no GPU/display/phone,
physical provider/LED, observed screen colour, audio continuity or fresh fps result
follows. Exact focused results/retirement receipts are retained privately under
`20261014/codex-ambient-status/`.

The following initial feature results belong to `cad3255`, held by manager whole
review for a double-poll status bug. They remain historical component evidence.

| Focused lane | Result |
| --- | --- |
| Qualified base `3670763`, before this feature | Mac 10/10; Debian ARM64 Clang 19 ASan+UBSan 11/11 |
| New feature, AppleClang Debug | 11/11 CTests; actual C++ software owner and original-date consumer, existing manager/configuration and expanded sealed file/unit checks |
| Same new feature, Debian ARM64 Clang 19 ASan+UBSan Debug | 13/13 CTests (30.67 s); actual native caller compiled, six Linux manager/IPC/retirement cases plus portable/production checks; GPU off, video build enabled |
| Formatting and container retirement | Pinned 19.1.7 `tools/format.sh --check`; exact final Docker backend INT/TERM/HUP 3/3, exit 130/143/129, actual containers stopped/removed and clients reaped |

The first feature Mac compile stopped on an unused Linux-only opt-in field in
its inert Mac path; that source/log is retained. The corrected boundary is
explicit, with warnings still errors. No timeout, queue, leak suppression or
warning is relaxed. All three own Mac build/source trees and eight actual own
containers are absent after hash-verified source/log/Testing/CMake/JSON retention.
Final Mac/Linux non-Markdown inputs match; later evidence text is Markdown only.
These focused results are not a combined six-lane release matrix or deployment.

The shared-observation correction retains separate source, failing baseline and
qualification receipts under `20261014/codex-ambient-status/shared-observation-fix/`.
With unchanged `cad3255` production runtime, the actual manager endpoint and owned
C++ child reproduce two polls per status, 18 genuinely queued receives, and
live vehicle versus retired ambient metadata after real child death during copying.
The deterministic fixture counts actual successful sends at the unchanged cadence;
it supplies no clock or model. Additional coherence assertions also expose a
publication advancing between the two original status callbacks. Regressions cover
one shared clock/publisher scope, paired clearing and original source dates,
independent deep copies, a real disabled native graph and standalone return shapes.
Existing expiry, late/concurrent action, handler failure, replacement and shutdown
checks assert the joint status boundary as well. Original reports/hashes are retained.

Final correction checks pass: AppleClang Debug **11/11** (18.60 s) and identical
source Debian ARM64 Clang 19 ASan+UBSan **13/13** (34.39 s), including eleven actual
ambient manager/owner cases and fourteen existing native owner cases. GPU remains
off; compiling the native caller supplies no display evidence. An earlier corrected
Linux lane exposed the existing backpressure fixture acquiring fuel before reconnect;
its failed source/logs remain retained. The reviewed test now requires real admission
of a stale original acquisition, verifies its unchanged age, then requests a strictly
newer acquisition. Production reconnect, publication and freshness limits are unchanged.
Pinned 19.1.7 formatting and actual final-backend INT/TERM/HUP cleanup pass. Hash
verification covers 195 retained per-lane reports and all 174 original static
artifacts; four own Mac trees and seven own containers are absent. Final source
differs from the identical 902-file Mac/Linux snapshot only in Markdown evidence.

Only phone-session may adopt this in a **new** generation after whole-source
manager review and the complete coherent required matrix. Its exact owner check:

1. Freeze the reviewed integration, retain all required matrix receipts, and build
   a new compatible helper. Enable `ambient_light: true` alongside the exact sealed
   manager `prephone_publication_socket`; verify both ambient/pre-phone module pairs,
   native flag, configuration hash, installed units, helper and fresh capacity.
2. Phone-free, obtain fresh manager status. Verify fresh software model, disabled
   settings/Off request, provider false and null current/command. Submit Fixed,
   Screen, Automatic and disabled settings with their original revisions; observe
   software updates and honest missing colour, without receiver/display restart.
3. Reject stale revision, malformed settings and a second pending action. Use
   existing owner-qualified failure/retirement checks to verify model expiry,
   pending failure and successor token rejection/reset; no competing native owner.
4. Check existing Home/touch/cluster/native fallback/Climate and original source
   dates. Any connected follow-up must retain both panels, video streams, UI-Sync,
   audio and control. Qualify actual runtime cost separately; component checks
   make no throughput/display claim.

Parked: durable settings, native UI controls, qualified active-pack/native accent,
physical current/command provider, MCU/LED/CAN-FD/clock/lease/acknowledgements and
physical colour/night/fade/fault/drain acceptance. This worker performs no CM5,
SSH/SCP, service, phone, release selection or signed-asset action.

## Atomic MCU command

The existing vehicle command coordinator now admits a composite
`vehicle_control::AmbientLightValue` under `ValueKind::ambient_light`.
[ambient-controls.example.json](../config/ambient-controls.example.json) declares
`cabin.ambient.rgbw`, **schemaVersion 2**, kind `ambient_light`, writable false
and current-state maximum age 2000 ms. Numeric min/max/step are forbidden for
this composite. SchemaVersion 1 remains scalar-only; Boolean/integer/real targets
are accepted under either version. The Climate applied-state adapter deliberately
withholds the composite: no unqualified Apple characteristic conversion is added.

`encode_ambient_light_value` / `decode_ambient_light_value` implement exactly nine
bytes, with unsigned network byte order:

| Bytes | Field |
| --- | --- |
| 0 | Payload version 1 |
| 1–4 | R, G, B, W (0–255) |
| 5–6 | Intensity (0–1000 permille) |
| 7–8 | Fade duration (0–5000 ms) |

Wrong length/version/bounds reject. Applied observations must have transition
zero; a transition request cannot masquerade as completed actual state.
The payload is not a complete bus message. A future executor must carry command
ID, target, epoch/provider generation, deadline and ordered reply sequence plus
qualified acquisition/report clock mapping and integrity in its transport envelope.
The selected hardware link is private CAN-FD/ISO-TP, not a car bus or CM5 GPIO.
No physical opcode/arbitration ID or clock mapper is invented.

Call `begin_generation` only for an admitted provider, then `update(inputs, now,
permitted)` on its owner thread. Permission defaults false; writable configuration
alone cannot authorize output. Returned work is one explicitly permitted dispatch.
The MCU owns the local smoothstep fade and hardware interlocks. Updates while
pending are coalesced by recomputation after completion; no PWM command stream
or automatic physical retry is generated. Each operation has a ten-second bound.
An unchanged rejected/failed/timed-out target is not retried; an explicit owner
policy is required to rearm it. New provider generations retire pending work and
use new monotonically unique command IDs. `retire` immediately withdraws current
state and fails pending work. A late reply cannot revive a timed-out/retired result.

Source loss withdraws desired colour; it does **not** fabricate a physical Off
confirmation or cancel an already dispatched operation by pretending it vanished.
The MCU must independently bound owner/link leases (hardware proposal: two seconds,
separate renewal), abort/fade off on lease loss and cut the LED branch on fault.
No transport/lease executor is implemented. No request means no new physical write.

The explicit paired hardware revision is commit **8f4a4e6** on matching branch
`development branch`:
hardware proposal.
Hardware owns strip, driver, supply, harness, firmware and physical qualification.

## Component evidence — 13 October 2026

Production feature inputs remain byte-identical to `1aa4359`; final test source
`771c7f9` includes upstream measured-thread-CPU fixture `b32c966` and the camera
actual-loop read-bound time repair. Pinned clang-format19.1.7 passes. Mac broad
**158/158** passes, plus both corrected fixture suites **2/2** on current source.

| Linux lane | Result and scope |
| --- | --- |
| GCC Debug/video | Initial185/186, unchanged30s release-test timeout; corrected current release/camera/budget checks3/3 under original limits |
| Clang19 ASan+UBSan Debug/video | All186 cases covered across broad runs plus current three-case3/3 correction; initial noexec tmpfs, camera read race and release timeout retained |
| Full Skia GCC RelWithDebInfo | 187/187 nonhardware, plus current camera/budget/release3/3 after the fixture correction |
| Full Skia ASan RelWithDebInfo | 187/187 nonhardware on exact final non-Markdown inputs |

This is qualification across **split runs**, not an uninterrupted all-pass
six-job invocation. Targeted reruns use serial CTest with unchanged limits.
No deadline, expected failure, warning or leak check is relaxed. Initial failures
and stopped own builds remain in private evidence. The inherited startup fixture
now burns actual thread CPU under the same5ms frame/100ms startup limits. The
camera fixture uses production time sampled after a bounded status read and
collects two real sequence advances within the same500ms heartbeat window;
original publication dates remain unchanged.

To respect shared disk capacity, the remaining Linux jobs use private Mac-backed
build directories, a real builder-owned Linux source copy, unprivileged UID501
and executable512MiB Linux/tmp tmpfs. Stock compiler/sanitizer/CTest selection
and external leak suppressions remain unchanged. An actual ownership/socket
probe validates native source/tmp semantics. The initial private tmpfs was
noexec; its failed fixtures are retained before the corrected exec run. The own
inactive Mac build was fully hash-verified in a private archive before its
redundant directory was retired. Other owners' caches/containers stay intact.

Evidence, source snapshots, logs, Testing trees and `qualification.json`:
`~/.cache/headunit-qualification/20261013/codex-ambient-light/`.
The retained standalone ARM64 Debug simulator SHA-256 is
`7aa3f734c29453e72f41f38c6b0b1f8b40918c9f56e62c426284634ed65d2760`.
These are portable/Linux component and explicitly simulated results, with no
CM5 GPU, connected-phone, MCU, LED or physical-car result. No release was sealed.

## Checks and bench handoff

Focused portable checks cover screen/mode/fixed/Off authority, dated night
fallback, independent expiry, screen replay/conflicts, strict configuration and
payload bounds, pending coalescing, interlock/not-writable denial, actual clamps,
provider loss/replacement, deadline and late reply. The Python interop check runs
the actual C++ simulator, retaining intermediate simulation readbacks, pending
versus applied and retirement. These are component/simulated checks only.

The simulator is standalone and opens no radio, display, MCU, GPIO or live socket:

```sh
ctest --test-dir build -R '^ambient-light' --output-on-failure
build/headunit-ambient-light-simulator \
  config/ambient-light.example.json config/ambient-controls.example.json \
  > /private/ambient-simulation.jsonl
```

It explicitly overrides enabled/writable only in its own simulation instance,
prints that override to stderr and dates observations with the actual host
steady clock. Fifteen JSON snapshots show unavailable current before dispatch,
intermediate simulated fade readings while pending, final applied, dimmed night
request with prior day actual, colour-source withdrawal and provider retirement.
This does not establish a physical actuator or connected accent observation.

Phone-session owns the bench. For isolated CM5 component qualification, transfer
only the qualified simulator and the two public examples to a private scratch
folder, run that command and verify the 15 snapshots; no live services or release
selection are needed. To adopt the library in a future application, merge the
branch, run the integrated matrix, and publish a **new** validated generation
through the sole bench owner, preserving signed assets and the current graph.
Until a physical owner/transport and qualified palette maps exist, leave ambient
writes disabled and do not add a second vehicle/radio/display owner.

Remaining acceptance: qualified signed/native accent maps, coherent runtime adoption of
the optional software bridge,
settings persistence/UI, MCU link/lease/clock mapping/firmware, fitted power/driver,
actual LED colour/night/fade observation, reboot/fault/lease tests and parked drain.
No installed behavior or phone/LED/physical-car acceptance is claimed.
