<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Vehicle adapter readiness and next steps

Reviewed **2 October 2026** against the current working tree. This supports
[goal 7](GOAL.md#active-work-and-acceptance) and the missing cluster inputs in
[the style review](CLUSTER_STYLE_REVIEW.md#reference-image-completeness-audit--1-october).
No physical car adapter is connected. The implemented cache adds no hardware driver,
calibration result, wiring assumption, actuator output or live configuration.

## Implemented boundary

| Area | Implemented | Still absent |
| --- | --- | --- |
| Shared telemetry | Ten optional numeric readings, 24 optional indicators, coherent transmission, observed drive mode with immutable capabilities, typed current-trip statistics and qualified heading. `VehicleObservationCache` retains original dates, age policies, provenance, availability and generations. Private trip identity/reset/counter history survives loss/restart. Legacy aggregate samples remain supported. | Physical acquisition/source-quality qualification, source-owned persistent trip calculation/reset policy and calibration. |
| Local transport | HVO1 preserves the original 34 records; explicit HVO2 adds transmission, HVO3 adds mode capabilities/selection and HVO4 adds trip/heading for 38 records. The stateful receiver preserves dates and pins version/catalogue across generations; trip history rejects replay and contradictory resets. Actual socket/Ultra-service and portable broker checks pass. HUV1/HUV2 and HUF1 retain legacy behavior. | Linux kernel qualification of HVO1–HVO4 fan-out, physical clock mapping and calibration revision. HUV2 rejects metadata and all four semantic extensions. |
| Display bridge | Native/backup gauges and Ultra warnings expire fields independently. Native gear/mode badges and the new Trip page retain unknown state. `GaugeInput` atomically publishes original dates; profiles provide explicit conversions, lamp arrays, transmission/mode dictionaries and inspected trip-mean consumption. | Live qualification of the new acquisition path and native Trip selection, all-state signed gear/mode appearance and actual Apple Trip content routing. No inspected local Apple distance/time/average/heading paths exist. |
| Climate/settings | `Provider::read(key, now_ms)` returns per-binding `Sample{value, sampled_ms}`; aliases share one read per refresh. `apply` returns validated confirmed state or fails. | Installed hardware backend, physical capabilities and asynchronous command acknowledgement. The launcher accepts only `none`/explicit `simulation`. |
| Timestamped script input | `SensorInput` accepts bounded per-IID value/invalid records over an inherited local packet socket, using CLOCK_MONOTONIC milliseconds; provider failure clears dynamic readings. | A shared semantic car adapter: these IIDs are local profile identifiers, not stable hardware addresses. |

The bench generator's transmission mode/third gear and optional
`--drive-mode-fixture` identifier list/current index are immutable artwork
fixtures. The new `--observed-transmission` option replaces the fixed transmission
fixture with timed semantic mappings; it supplies no actual car state. Typed
transmission, observed drive mode, trip and heading are implemented. The separate
`--drive-mode-config` JSON option maps semantic mode IDs to authored artwork
and supplies no active reading.

`--observed-trip` adds the inspected whole-trip consumption binding with
explicit `--trip-instance`/`--trip-label` display configuration; it supplies no
counters or mean. [Trip and heading](TRIP_AND_HEADING.md) defines that contract.
Local script paths and numeric asset enums are pack-specific; they must not
become the hardware adapter's public vocabulary.

## Dated MCU integration revision

The [9 October MCU contract](MCU_VEHICLE_CONTRACT.md) aligns the existing HVO4
semantic inputs with hardware specification `389ca82` and supplies an isolated
socket simulator for mixed rates, expiry, replay and source retirement. It
proposes explicit night-mode, serial-clock and calibration-admission interfaces
for both repositories. Shared night-mode transport, physical decoding and clock
mapping remain unimplemented; no hardware file or live service is changed.

## Semantic contracts and remaining telemetry

Transmission, drive mode, trip and heading are implemented and component-tested.
None establishes the car's equipment
or physical acquisition.

| Input | Required semantic contract | Focused acceptance cases |
| --- | --- | --- |
| Transmission | Implemented: separate gearbox type, effective selector state and actual engaged gear; one coherent acquired state, with optional components and explicit pack mappings. A requested shift is outside this read-only contract. | Portable checks cover neutral/reverse/forward, partial knowledge, incoherence rejection, atomic replacement, every mapping and expiry. Native pixels and regular DigiRing forward-gear fixtures pass; all-state signed appearance, Linux and physical/live checks remain. No inference from RPM/speed. |
| Drive mode | Implemented: bounded semantic mode IDs, immutable catalogue and optional acquired active selection with required membership. Artwork identifiers/index conversion stay in the profile. Source order is independent of authored order. Mode commands require the separate interface below. | Portable tests cover unknown/unmapped state, unsupported ID rejection, bounded configuration, atomic conflict, expiry, catalogue pinning and original service dates. Native and signed GT/Sport/redaction CPU fixtures pass; physical/live qualification remains open. |
| Trip | Implemented: stable identifier, observed positive reset generation and coherent optional distance in km, elapsed/moving time in seconds, supplied average with an explicit denominator, and whole-trip mean in L/100km. Reset/counter history survives loss/restart. The future adapter must choose and qualify accumulation, moving-time, persistence and reset policies. Range and lifetime odometer remain separate. | Portable checks cover zero denominators, mean unavailable at zero distance, partial counters, chronological contradictions, atomic rejection, higher reset with newer acquisition, loss/restart and old-snapshot revival. Native Trip pixels and actual HVO4/service timing pass. Renderers never accumulate trips or invent consumption. |
| Heading | Implemented: finite degrees clockwise from north (`0 <= degrees < 360`), explicit true/magnetic reference and orientation/course source. The adapter declares validity and omits course when it cannot determine it at low speed/stationary. | Portable checks cover north/359.9°, rejection of 360/nonfinite/out-of-range or unknown enums, independent exact expiry and reference/source labels. Sensor accuracy and physical/live qualification remain open. |

Neither the photographed trip page nor a newly populated script binding proves
that the phone supplies that page. Qualify local widgets and phone content
channels independently, with the unavailable view included.

### Mapping notes from the inspected packs

The four local regular/Single and regional variants share these inspected
script paths. These are asset mappings, not hardware telemetry fields or a
claim that every future pack has the same contract.

| Path | Observed interpretation | Adapter consequence |
| --- | --- | --- |
| `driveState.transmissionStatus.transmissionMode` and `gearPosition` | Display modes include `NONE=0`, `D=68`, `L=76`, `M=77`, `N=78`, `P=80`, `R=82`, `S=83`. | Map selector/operating state explicitly. Gearbox type alone must not select `M`. Keep selector and engaged gear coherent. |
| Drive-mode `identifiers` and `currentIndex` | Immutable identifiers plus a timed zero-based active index; the script converts to its one-based lookup. Artwork includes GT, sport/sport+, individual, terrain/terrain+, track/track+ and wet. | Require an observed active identifier from the configured car's catalogue. Available artwork does not establish actual car capabilities or catalogue order. |
| Repeated `tripComputer.trip` with `userVisibleLabel` and `fuelEfficiency` | Consumption input uses mL/100 km; `vehicleUnits.displayUnits.fuelEfficiencyUnit` selects display conversion separately. | Canonical L/100 km requires an explicit profile scale of 1000. Keep undefined consumption unavailable and qualify zero-ratio display behavior. |

The audit did not locate local signed input paths for trip distance, elapsed
time, average speed or heading. Those remain typed native/future inputs until
actual widgets or phone content channels are qualified. The native Trip page
is component-tested; this does not qualify a phone-provided Trip view. Existing repeated
service bindings already support trip instances; adding a field must not
invent a phone resource identifier.

## Acquisition time, calibration and unknown state

Direct HUV2 packets currently acquire the time passed to parsing while the
consumer drains its socket. HUF1 fixes queue-age renewal after **Pi ingress**;
it does not date the sensor observation. One fresh speed reading can therefore
share a snapshot with an old cached temperature unless the producer removes
that old field. Re-sending cached HUV2 values also looks fresh to consumers.
The new in-process cache prevents renewal through snapshot polls. The legacy
transport still requires its producer to expire every physical source before
publication. [The observation contract](VEHICLE_OBSERVATIONS.md) documents this
boundary; [HVO1–HVO4](VEHICLE_OBSERVATION_PROTOCOL.md) carry the implemented
per-signal metadata between same-host processes.

1. The cache gives each reading/lamp, coherent transmission, active drive mode,
   coherent trip and qualified heading an original acquisition time, availability,
   provenance and source generation, separate from snapshot publication time.
   In-process or same-Pi
   producers use one qualified monotonic domain. A PCB clock needs an explicit
   mapping, uncertainty and reconnect policy; do not compare its raw ticks or
   wall time directly with the receiver clock.
2. Acquisition dates survive cache reads and native/Ultra display polls.
   Bounded atomic delta batches reject replay, conflicting equal dates and
   generation reversal. Withdrawal preserves acquisition watermarks; delayed
   newer observations recover only while fresh. The Ultra service coarsens to
   milliseconds without inventing timestamps. HVO1–HVO4 fan-out forwards producer
   bytes unchanged; its consumer validates the complete schema and ordering.
   Source restart ordering uses the last accepted source publication, not a
   later consumer poll. `SocketVehicle` still accepts equal legacy
   snapshot timestamps; legacy `GaugeInput` still ignores equal milliseconds.
3. Metadata-bearing fields expire at age **≥** their individual maximum age.
   The script profile can expire a bound value earlier. Legacy aggregate
   native `fresh` retains its inclusive boundary for compatibility, while the
   timed Ultra service expires at it. Both behaviors have explicit tests.
4. Keep zero/off distinct from unavailable. Retain adapter diagnostics for
   absent, stale, invalid, uncalibrated or disconnected input without fabricating
   display values. Metadata distinguishes available/unavailable/stale/
   disconnected and measured/calculated/simulation. Physical invalid or
   uncalibrated diagnostics still belong in the eventual acquisition adapter.
5. Keep raw decoding and calibration in the adapter. A future validated
   configuration should identify source/units, calibration revision, scale or
   measured lookup curve, physical valid range, age and precision. Supply no
   invented conversion coefficients, sender curves or bus addresses. Existing
   `vehicleScale`/`vehicleOffset` convert display units; fixed gauge bounds do
   not calibrate a sensor. Warning lamps stay independent inputs unless a
   separately qualified adapter rule defines their thresholds/hysteresis.

## Command and applied-state interface

`VehicleSource`, HVO1–HVO4/HUV2 and the warning overlay are read-only. The existing
Climate/settings callback is a bounded synchronous integration point: returning
from `apply` claims confirmed applied state, and the service dates that reply
at handling time. It carries no command identity, pending status, hardware
acknowledgement time or asynchronous completion. Related physical state must
come from provider readings, not the simulator's level/power relationships.

The original `vehicle_control::Coordinator` now implements that separate bounded
API: stable target key, command identity, requested value, deadline, provider
generation and accepted/pending/applied/rejected/failed/timed-out outcomes.
Matching confirmations publish actual values with their original dates;
queueing a command never replaces a reading. Duplicate, late, out-of-order,
loss/restart and expiry policies are explicit. Strict JSON target policies
default to write-disabled, and dispatch requires separate current permission.
The read-only confirmed-state provider bridges actual values to Climate/settings.
It performs no hardware I/O. Physical interlocks, actuator delivery, persistent
identity and an asynchronous phone-response integration remain unqualified.
Keep measured cabin temperature, requested setpoint and applied actuator state
distinct. See [the control guide](VEHICLE_COMMANDS.md).

An asynchronous adapter cannot use the current callback as a successful queue
receipt. It must reject unconfirmed writes until a qualified asynchronous
phone-response integration exists. The transport's existing response-capacity
reservation protects a callback from being repeated under backpressure; it is
not a hardware delivery or exactly-once guarantee.

## Small implementation sequence

1. **Time first — component implementation complete.** The semantic per-signal
   model and fake adapter cache have no hardware I/O. Tests cover mixed-rate expiry, repeated
   reads, future/reversed time, same-time duplicates, delayed recovery after
   absence, replay and provider generation changes. Native/backup rendered
   images and the actual Ultra script service exercise the shared age policy.
   HUV1/HUV2 behavior is unchanged; encoding metadata as HUV2 rejects loss.
   The HVO1 versioned codec, receiver and cross-process Ultra-service checks
   now pass on the Mac. The broker forwards HVO1 unchanged; Linux kernel and
   live/physical qualification are not complete.
2. **Transmission — software integration complete; qualification pending.**
   Typed state, coherence validation, original per-state timing, HVO2 encoding,
   version-pinned reception and explicit local-profile mappings pass portable
   integration checks. The native badge retains unavailable state; the regular
   DigiRing fixture displays third/fourth gear, its authored reverse logo and
   missing-state redaction, with unchanged adjacent labels. Linux kernel fan-out,
   CM5 deployment, all-state live appearance and real acquisition remain open.
   See [the transmission guide](TRANSMISSION.md).
3. **Drive mode — software integration complete; qualification pending.**
   A fixed bounded capability catalogue and acquired selection flow through
   HVO3 into native badges and explicit Ultra mappings. Thirteen affected
   portable suites pass, including actual socket/service timing. Signed regular
   DigiRing CPU fixtures show GT/Sport and unknown/unmapped redaction; exact
   pixels establish source-order independence and preserve adjacent labels.
   Linux fan-out, CM5 deployment and physical/live acquisition remain open.
   See [drive modes](DRIVE_MODES.md). Fixed `--drive-mode-fixture` artwork remains
   separate from the new file-driven observed path.
4. **Trip and heading — software integration complete; qualification pending.**
   Typed coherent statistics, reset/history validation, qualified heading and
   explicit HVO4 transport pass the shared cache/socket/Ultra-service checks.
   Native Trip has independently expired unknown-state pixels and seven bounded
   simulated snapshots. The signed regular pack starts with known/unknown mean
   consumption bindings; no Apple Trip content route or other local input path
   has been established. Fifteen affected portable suites and focused model/cache/
   codec/receiver sanitizer checks pass. Physical accumulation/persistence,
   sensor acquisition, Linux fan-out, CM5 deployment and live widgets remain open.
5. **Build a read-only adapter when sources are known.** Keep its I/O owner and
   bounded cache separate from display threads. Add calibration-config checks
   using labelled fixtures, then record physical reference measurements,
   disconnect/stale behavior and all warning inputs. Hardware selection and
   physical calibration remain pending; offline fixtures cannot complete them.
6. **Command software boundary implemented; physical integration pending.**
   Fake-actuator tests cover request/applied differences, rejection, timeout,
   duplicate delivery, late acknowledgement, generation change and bounded
   history. Parsed Climate/settings service tests expose confirmations only;
   synchronous writes fail without creating a command. No hardware provider or
   launcher backend is enabled. Qualify physical interlocks/delivery and the
   deferred phone-response contract before enabling writes.

Existing test coverage referenced: `vehicle_signals_test`, `theme_gauge_input_test`,
`vehicle_configuration_test`, `theme_vehicle_service_test`,
`theme_sensor_input_test`, `socket_vehicle_test` and `vehicle_fanout_test.py`.
The control boundary adds `vehicle_commands_test`,
`vehicle_command_configuration_test` and `theme_vehicle_applied_provider_test`.
They substantiate software policies and fixture behavior. This documentation
audit runs no new hardware tests and claims no new physical qualification.
