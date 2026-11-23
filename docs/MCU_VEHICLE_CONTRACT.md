<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# MCU acquisition to Linux vehicle observations

Revision **2026-10-10, proposal 1 with ignition lamp-check policy**. This guide owns the cross-repository
acquisition contract and socket-simulator evidence for ROADMAP item 6.
The hardware specification was read locally, without modification, at
`ultra-headunit-hardware` commit `389ca820cd75e660f751fc6bb85297cd0a768dab`
(clean `main`): sections 4, 5 and 6, especially HW-V02–04 and HW-I02.
The software starting point is `c8622e8`. No physical provider or MCU firmware
is qualified. Existing types and HVO4 are implemented; the MCU byte protocol,
clock mapper, night-mode extension and ignition lamp-check metadata below are proposals.

## Current hardware transport and ambient command revision — 13 October

The hardware v1 architecture at `fe425e5` selects private CAN-FD between Cortex
and Spine, with ISO-TP for larger messages. The USART2/ST-LINK UART worksheet
below is **historical proposal evidence**, not the selected production transport.
Framing/clock-mapper/CAN envelope work remains unimplemented. HVO4 stays a host
snapshot, independent of that transport choice.

Paired hardware revision `8f4a4e6` / software `development branch`
adds the [ambient RGBW command](AMBIENT_LIGHT.md#atomic-mcu-command), schemaVersion2
kind `ambient_light` and bounded payload v1. It consumes existing dated drive_mode
and low_beam, reuses the host headlights/schedule appearance policy and invents no
observed night signal. No HVO5 change is needed. MCU permission, lease, fade,
actual acknowledgement and clock mapping remain explicit physical requirements.
## Current sport extension — 13 October 2026

Typed acceleration/GPS and explicitly selected **HVO6** are implemented host
observations; [Sport telemetry](SPORT_TELEMETRY.md) owns bounds, JSON, continuity
and source-only qualification. The paired hardware proposal is commit `19d68f5`
on `development branch`: BMI088 SPI plus MAX-M10S UART/TIMEPULSE on
a switched sensor module near Spine, calibrated forward/left acceleration,
qualified WGS84 fixes, independent acquired timing and invalidation. Candidate
sourcing, firmware/clock mapping and physical qualification remain open.

The current v1 two-board hardware specification (base `fe425e5`) supersedes the
older UART/ST-LINK inter-board proposal below: **private CAN-FD/ISO-TP** carries
future MCU acquisition between Spine/Cortex. Local GNSS UART and IMU SPI are
sensor links, not the inter-board transport. Do not put ASCII HVO6 on CAN-FD;
agree bounded framed session/tick/calibration/CRC fields and recalculate traffic
before firmware. The existing clock-mapping/retirement rules still apply.
Historical HVO4 serial-proposal evidence below remains scoped to its dated source.

## One acquisition owner

The MCU owns vehicle I/O, capture, electrical diagnostics, calibration identity
and output interlocks. One Linux adapter will validate framed acquisition
messages, qualify their clock mapping and installed-channel policy, then feed
`VehicleObservationCache`. Its complete HVO4 snapshots go to the existing
`SocketVehicle` consumers directly or through the unchanged fan-out broker.
Native, backup, HUD and signed-script consumers retain independent expiry checks.
No renderer reads a UART, decodes raw bus frames or recalibrates a sender.

The P0 hardware proposal uses STM32G0B1RE USART2 through ST-LINK USB VCP,
115200 8N1. Hardware section 6 proposes frames ≤1024 bytes including framing,
ordinary frames ≤128 bytes, four queued frames per direction and ≤4 KiB/s
sustained offered load. Its current traffic worksheet totals 3680 B/s.
Version/type/length, integrity parameters and channel IDs remain unassigned.
HVO4's ≤8192-byte **host snapshot is not an MCU serial frame**; do not put HVO
on the UART or silently split an atomic observation across unrelated messages.

## Typed cluster inputs

Use the existing `VehicleObservationValue`, `VehicleSample`, reading catalogue
and indicator catalogue. These bounds are software plausibility limits;
installation calibration can narrow them, never infer a physical curve from them.
Every observation carries acquisition, availability and provenance independently.

| Semantic key | Linux value / canonical units | Availability and meaning |
| --- | --- | --- |
| `speed_kmh` | Optional finite double, 0–300 km/h | Timer/bus acquisition; genuine measured/calculated zero requires a qualified no-edge/validity rule. Link loss cannot establish zero. |
| `engine_rpm` | Optional finite double, 0–9000 RPM | Qualified pulse count/bus conversion. Stopped engine and missing pulses are distinct conditions. |
| `transmission` | Optional `TransmissionState` | One coherent acquired gearbox type, effective selector and actual engaged gear, each component independently optional. Never infer gear from speed/RPM, or treat a shift request as effective selector. [Exact types](TRANSMISSION.md). |
| `drive_mode` | Optional `DriveModeIdentifier` | Acquired effective mode must belong to the immutable optional `DriveModeCatalog`. No default mode from artwork or catalogue order; an unequipped car has no catalogue. [Exact identifiers](DRIVE_MODES.md). |
| `fuel_percent` | Optional finite double, 0–100 % | Calibrated sender/bus reading. Open/short/uncalibrated is unavailable, not empty/full. |
| `coolant_c` | Optional finite double, −50–180 °C | Original capture date, including filter age. Missing is not cold. |
| `oil_c` | Optional finite double, −50–200 °C | Independent acquisition and age. |
| `ambient_c` | Optional finite double, −60–80 °C | Independent acquisition; do not substitute coolant/cabin temperature. |
| Lamps below | Optional `IndicatorState::{off,on,flashing}` per lamp | Known off is a reading. Unknown/stale leaves no active symbol; the phase of a flashing symbol does not change source freshness. |
| `night_mode` **proposed** | Optional boolean plus its own `VehicleSignalTiming` | `false` means observed day, `true` observed night; absence/expiry means unknown. Illumination voltage needs a qualified debounce/threshold/profile rule. Low beam, wall time and ambient temperature cannot establish night mode. |

The complete existing lamp keys are `left_turn`, `right_turn`, `hazard`,
`high_beam`, `low_beam`, `position_lights`, `front_fog`, `rear_fog`,
`check_engine`, `oil_pressure`, `battery_charge`, `brake_fault`,
`parking_brake`, `abs`, `stability_control`, `traction_control`, `airbag`,
`seat_belt`, `door_open`, `low_fuel`, `coolant_warning`, `washer_low`,
`tyre_pressure` and `glow_plug`. Each is independent; fuel/temperature thresholds
or hazard-to-turn synthesis require a separately qualified installation rule.
Unfitted lamps remain unavailable, not off.

### Ignition warning-light check — proposed 10 October

The user's ignition-on warning-light check belongs to the vehicle acquisition
owner, before the native/Ultra split. Both renderers consume the same fresh
lamp states in their fixed bottom slots; Linux does not run a second test timer
on boot, phone connection or a cluster-style change.

For a car with an observed ECU/discrete lamp-check sequence, the MCU forwards
the actual acquired on/off/flashing states, including the check. It does not
write to the vehicle bus or warning circuits to ask them to illuminate. For an
installation requiring an authored cluster self-test, the MCU profile must
explicitly define the fitted warning-light mask, bounded duration, ignition
edge/debounce and rearm rule. Do not assume all 24 supported symbols are fitted
or make exterior-light/turn indicators into fault warnings.

An authored self-test needs a separately dated test-active indication and
calculated provenance, distinct from acquired faults. Its composition may
illuminate the configured test mask, but must preserve actual active faults
and their flash/state policy when the test ends. No renderer may acknowledge
test completion before a genuinely presented cluster frame. A car test that
ended before the display became available cannot be replayed as current car
state or counted as a visible test. Loss/restart/expiry must not latch test lamps
or imply known-off faults. Ignition state, test-active metadata and the MCU
wire fields are not implemented in HVO4; agree their typed, dated extension
with the hardware repository before firmware or transport implementation.

The hardware spec already assigns protected ignition and warning acquisition
to the MCU and requires bulb-check pulses not to be disturbed. No circuit,
firmware, CAN identifier or physical provider changes in this software task.

The existing additional readings are oil pressure in kPa, battery voltage in V,
range and odometer in km. HVO4 also preserves coherent trip/heading when supplied.
These fields are mandatory unavailable records in a complete HVO4 snapshot when
unfitted; omitting a record is malformed, not a delta.

## Dates, policy and admission

`VehicleSignalTiming` separates original positive host `steady_clock` acquisition
from publication/receipt, immutable positive maximum age (≤24 h), origin
(`measured`, `calculated`, `simulation`) and availability (`available`,
`unavailable`, derived `stale`, explicit `disconnected`). Unavailable deltas
carry `std::monostate`, never numeric zero. Unobserved fields have no acquisition.
Age **≥ maximum age** expires a field independently. Publication, heartbeat,
fan-out, polling and drawing cannot renew it.

Hardware planning suggests fast speed/RPM/selector/lighting at 20–50 Hz with
100–250 ms age, and fuel/slow temperatures at 2–10 Hz with 1–5 s age.
These are qualification candidates, not production defaults. The simulator uses
100 ms for speed/RPM/transmission/mode/lamps and 2 s for its slow readings.
Other policies remain at the existing 500 ms default. Source and script-profile
expiry both apply; the earlier expiry wins. Policies and mode catalogue are fixed
for a consumer lifetime, including source-generation changes.

The adapter must admit source/session identity, interface revision, declared tick
rate/wrap/sleep behavior, fitted channel/type/unit map, calibration revision,
validity diagnostics and ordered message sequence before interpreting data.
Calibration mismatch or invalid/open/short signals become unavailable; detailed
fault reasons stay in adapter diagnostics because HVO4 carries no fault-code or
calibration-revision field. Discovery cannot grant actuator permission.
The commissioning record binds those identities to the installation policy;
no second semantic signal catalogue or invented CAN identifier is needed.

The cache accepts bounded **atomic deltas**; HVO4 sends **complete snapshots**.
Delta batch and published snapshot sequences are separate positive counters.
Reject duplicates, reversed sequences/acquisitions, equal-date conflicting
values, future dates, unsupported types and capability/policy changes atomically.
A rejection consumes no sequence, permitting a corrected batch at that sequence.
An unchanged equal-date observation is idempotent, never a revival after expiry
or withdrawal. A newer delayed acquisition may recover only for its remaining
original lifetime. See [observation rules](VEHICLE_OBSERVATIONS.md) and
[wire admission](VEHICLE_OBSERVATION_PROTOCOL.md).

## Proposed MCU clock and retirement agreement

MCU ticks are not Linux timestamps. The future adapter must establish a bounded
tick-to-host mapping per boot/session using real timing exchanges, extending
wrap only within that identity and qualifying drift/sleep behavior. Hardware
targets ≤2 ms uncertainty for fast channels; the simulator does not measure it.
The adapter must date a capture conservatively at the oldest possible host
acquisition in its qualified uncertainty interval. Account for filters and
timestamp quantization without rounding to a later acquisition. If that date is
before the admitted generation start, reject it; never clamp it to startup or
receipt. Persist/reconstruct increasing host generation identity across adapter
restart; a random MCU boot ID is not an ordered HVO generation number.

Both repositories should agree the timing-exchange fields, wrap width, tick
frequency, drift allowance, maximum sync silence and invalidation rule before
implementing the wire encoder/decoder. An unbounded/missing mapping withdraws
affected observations and any future command authority. Fresh transport traffic
cannot qualify a stale mapping. No synthetic clock offset or receive timestamp
may replace capture time. Physical clock/reset/load tests remain required.

Detected unplug, MCU reset, identity/calibration mismatch or terminal parser
failure calls cache `withdraw`, retaining dates/watermarks; publish a final
disconnected snapshot when the host publisher can still run. A new admitted
session begins a strictly higher host generation and clears measurements.
Its start must follow the last accepted source publication; a later consumer
poll is not a source-start floor. The immutable installation/catalogue remains.
Trip reset identity remains independent from MCU/source restart.

A Unix datagram consumer cannot detect a silent sender death immediately. If no
withdrawal arrives, values expire independently as stale; do not call them
disconnected, zero or freshly off. A future publisher lease may diagnose loss,
but cannot extend acquisition age. No pending actuator command is replayed on
reconnect. Applied-state commands remain the separate [command contract](VEHICLE_COMMANDS.md).

## Explicit interface proposals for both repositories

1. **Night mode:** Linux currently has signed `uiControl.uiAppearance.nightMode`
   bindings/fixtures, but no shared semantic night observation in `VehicleSample`
   or HVO1–HVO4. Add `std::optional<bool> night_mode` and independent timing,
   and a `bool` alternative to `VehicleObservationValue`. Extend cache validation,
   receiver watermarks and `current_vehicle_sample`; do not overload a lamp or
   numeric temperature field. Propose explicitly selected **HVO5** with the HVO4
   header and 39 mandatory records, adding
   `night_mode=true|false|-,acquired_ns-or-,maximum_age_ns,origin,availability`.
   Retain exact HVO1–HVO4 shapes; their encoders must reject populated/nondefault
   night metadata. Broker passthrough/admission, portable codec/socket tests,
   native appearance policy and signed profile mappings must be updated together.
   Unknown appearance should retain the existing configured safe appearance
   without publishing a fabricated observed `false`. This revision proposes it;
   **HVO5 and shared night-mode acquisition are not implemented here**.
2. **Hardware MCU link:** document the agreed framed schema and CRC/session/tick
   fields in hardware section 6 before firmware work. Include illumination/night
   as an optional admitted observation with independent validity and capture tick.
   Preserve ≤1024-byte bounds and the traffic worksheet; selecting a front end
   does not select calibration or supply a day/night rule. Implement the firmware
   in the hardware repo and the sole host decoder/mapper here, with shared labelled
   fixtures and explicit schema revision. Neither repo has assigned byte IDs yet.
3. **Calibration/session admission:** hardware module records already propose
   calibration/interface identities, while HVO4 intentionally contains only the
   post-admission semantic data. Agree a validated adapter commissioning record
   and diagnostics sidecar, plus mismatch withdrawal tests, rather than pretending
   that HVO4 transmits calibration identity. Changing fitted capabilities or age
   policy needs a newly admitted consumer configuration, not a heartbeat edit.

No hardware files, firmware, outputs, appliance services or selected releases
are changed by this work.

## Socket simulator and scoped evidence

`headunit-mcu-vehicle-simulator` simulates the **post-mapping Linux output of a
future MCU adapter** using actual AF_UNIX datagrams and the production HVO4
encoder/cache and `SocketVehicle`. It is not a serial parser or physical MCU.
Every recorded reading is marked `simulation`. It opens no bus/GPIO/UART,
registers no phone resources and sends no actuator commands. Night mode is not
sent because the shared transport does not yet admit it.

```sh
cmake --build build --target headunit-mcu-vehicle-simulator
build/headunit-mcu-vehicle-simulator --self-test
ctest --test-dir build -R '^mcu-vehicle-simulator$' --output-on-failure
```

The self-test creates and retires its own private endpoint and uses an explicitly
synthetic positive monotonic timeline without sleeps. Seventeen checkpoints
cover mixed rates, original acquisition/provenance, exact independent expiry,
queued-heartbeat expiry, reordering/duplicates, atomic conflicting snapshots,
sequence preservation, immutable age policy, unavailable versus zero/off,
withdrawal/non-revival, newer delayed recovery, restart, retired generation and
silent sender close. Unfitted readings stay absent and supported modes survive
measurement loss. Retirement checks that the consumer removes its endpoint.

For a separately created **isolated** consumer, run:

```sh
build/headunit-mcu-vehicle-simulator --socket /absolute/private/vehicle.sock
```

This replays the same 2.124-second synthetic acquisition schedule in the actual
host monotonic domain. The receiver must sample often enough to drain its bounded
queue. The tool requires an existing owned private socket/directory, fails on a
failed send, and prints scenario actions; socket delivery is not consumer or
display acceptance. Do not point it at the live appliance input. The older
Linux-only HUV2 simulator remains a separate legacy fixture.

On 9 October, AppleClang 21 Debug passes the **142/142** portable CTest suite,
including this fixture, with pinned clang-format 19.1.7 and whitespace checks.
The initial full run was blocked by binaries linked to libplist 2.7's old
install name after Homebrew selected 2.8. Refreshing this worktree's PLIST
CMake cache and rebuilding against 2.8 makes the complete run pass; no system
library or unrelated source was edited. The Mac realtime CLI receipt check
receives **15** bounded complete HVO4 datagrams, observes generations 1 → 2
and the deliberately retired generation, preserves the consumer-owned endpoint
and verifies nonprivate-directory rejection. Its receiver uses a 256 KiB receive
buffer; a default-buffer CLI attempt failed explicitly on send,
so this does not establish delivery under arbitrary congestion.

The pre-pause CM5 pipeline completes **7/7** scoped Linux checks with Clang 19
ASan+UBSan, `ASAN_OPTIONS=detect_leaks=1` and `UBSAN_OPTIONS=halt_on_error=1`:
`mcu-vehicle-simulator`, `socket`, `vehicle-signals`, `vehicle-observation`,
`vehicle-observation-wire`, `vehicle-observation-receiver` and `vehicle-fanout`.
Its final log is dated **17:30:48 CEST**: the remote command continued after the
manager interrupted the Codex turn to reduce concurrent builds. The follow-up
starts no CM5 builds/tests and finds no surviving process in this topic's
workspace. The simulator source and CMake input SHA-256 match the Mac inputs.
On **9 October at 17:39:34 CEST**, the manager-authorized cleanup confirms no
surviving owned process and removes only
`/home/headunit/headunit-hardware-contracts-20261009` (144 MiB before removal).
Shared Luau/Skia and other workspaces are preserved, along with all Mac evidence.
The cleanup starts no build, suite or live action; its receipt is
`cm5-cleanup.log` in the evidence directory below.

The earlier broader build including `headunit-theme-gauge-input-tests` was
interrupted before testing; **CM5 `theme-gauge-input` remains unrun here**.
Its portable Mac suite passes. Manager review accepts the seven completed Linux
suites as sufficient for this new post-mapping simulator; no broader bridge
rerun or additional build slot is needed for this change. The interrupted run
remains explicitly unqualified.
Receipts are retained outside Git under
`~/.cache/headunit-qualification/20261009/hardware-contracts/`.
No phone, physical sensor, MCU serial, calibration, GPU or clock-mapping result
is implied; these checks do not close hardware acceptance.

## Bounded future live plan

After both repositories agree and implement the missing adapter/night interface,
the live owner can publish a retained candidate with rollback and explicit
simulation bindings. No live run is needed for this component-only change.
For eventual display acceptance: acquire exclusive live ownership; capture fresh
native/Ultra baseline on both panels; select the isolated adapter fixture in the
candidate; run one bounded 10-second sequence of known/unknown speed/RPM,
selector/gear, mode, fuel/temps, off/on/flashing lamps and day/night; stop input
and observe independent expiry; restart at a higher generation and inject one
retired packet. Verify unavailable redaction, lamp clearing, no guessed gear,
dated night handling, both panels, main touch and existing Climate/settings.
Retire the fixture and record candidate source/inputs/receipts. Physical tap,
calibration, open/short, MCU unplug/reset and uncertainty under link load require
separate bench qualification; a phone session cannot establish them.
