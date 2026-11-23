<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Vehicle input and cluster indicators

The backup cluster and Ultra gauge adapter use the same `VehicleSource` C++
interface. A hardware-specific process will read the car's sensors and publish
validated samples through this boundary. No CAN/GPIO/ADC driver or actuator
output is selected yet. Simulation has no route to the car.

## Available readings

All values are optional. Missing is different from zero. The accepted ranges
below are transport plausibility limits, **not warning thresholds**.

| Field | Unit | Accepted range |
| --- | --- | --- |
| `speed_kmh` | km/h | 0–300 |
| `engine_rpm` | rpm | 0–9000 |
| `fuel_percent` | % | 0–100 |
| `coolant_c` | °C | −50–180 |
| `oil_c` | °C | −50–200 |
| `oil_pressure_kpa` | kPa | 0–1500 |
| `battery_voltage` | V | 0–60 |
| `ambient_c` | °C | −60–80 |
| `range_km` | km | 0–3000 |
| `odometer_km` | km | 0–9,999,999 |

A producer supplies calibrated physical units. Raw ADC counts, CAN message IDs,
fuel-sender resistance curves and car-specific warning thresholds belong in the
future hardware adapter/configuration. The display does not guess them. Range
and odometer are supplied readings, not calculations derived by the renderer.

## Transmission

The optional compound `transmission` state separates gearbox technology,
effective selector/operating state and actual engaged gear. Known components
must agree; unknown components are not inferred from speed or RPM. It has its
own acquisition date, provenance and age policy. The native gear badge displays
known P/R/N/forward state or a selector fallback, and `--` when unavailable.
Local Ultra profiles use explicit semantic dictionaries for
`transmission.selector`, `transmission.gear` and `transmission.gearbox_type`.
See [the transmission API and mapping guide](TRANSMISSION.md).

## Observed drive modes

An optional immutable `drive_mode_catalog` lists the car's supported semantic
identifiers. The independently optional `drive_mode` is its acquired active
selection, with its own age/provenance and catalogue-membership checks. Neither
RPM, transmission selector nor selected artwork determines a mode. Native
rendering shows a mode badge only for configured vehicles; unknown/stale active
state displays `--`. The local Ultra `vehicleField: "drive_mode"` maps names
explicitly into a fixed authored identifier array and timed index. See
[drive-mode configuration and acquisition](DRIVE_MODES.md).

## Current trip and qualified heading

The optional compound `trip` carries a persistent selected-trip identifier,
explicit reset generation and optional distance, total/moving duration,
average speed with its declared denominator, and **whole-trip mean**
`fuel_efficiency_l_per_100km`. One acquisition date, provenance and age policy
cover the coherent observation. A reset can decrease totals only at a genuinely
newer acquisition; source restart, partial statistics and complete absence do
not erase prior identity/reset/counter history. The renderer does not accumulate
trips, divide counters, infer a mean or turn undefined consumption into zero.

The independent `heading` declares degrees clockwise from north in `[0, 360)`,
a true/magnetic north reference and vehicle-orientation/course-over-ground
source. If either cannot be qualified, omit the heading; an object with
unknown/invalid qualifiers is rejected instead of becoming an invented north
heading. Trip and heading expire independently. See
[the typed contract and native presentation](TRIP_AND_HEADING.md).

Only the inspected trip consumption scalar currently has a local signed-asset
binding. No Apple script paths for trip distance, time, average speed or heading
have been established. Native Trip/heading presentation and parsed local
bindings do not establish the iPhone's Current Trip center-content route.

## Indicators

Each input has four distinct states: **off**, **on**, **flashing**, or
**unavailable**. The C++ representation is `optional<IndicatorState>`; a missing
optional is unavailable. A valid off state is never inferred from an absent
sensor. These are display inputs; they do not switch the actual lights.

| Fields | Active display colour |
| --- | --- |
| `left_turn`, `right_turn`, `low_beam`, `position_lights`, `front_fog` | Green |
| `high_beam` | Blue |
| `hazard`, `rear_fog`, `check_engine`, `abs`, `stability_control`, `traction_control`, `low_fuel`, `washer_low`, `tyre_pressure`, `glow_plug` | Amber |
| `oil_pressure`, `battery_charge`, `brake_fault`, `parking_brake`, `airbag`, `seat_belt`, `door_open`, `coolant_warning` | Red |

The native cluster displays labelled schematic telltales **only when active**.
Known off and dark flashing phases have no icon. Unavailable/stale direction
readings show faint neutral dashed arrow outlines; other unavailable warnings remain hidden. A `flash` state uses a
1.5 Hz / 50% duty fallback cycle (90/minute). An adapter may instead publish explicit on/off
states when it has the car's actual flashing phase. Fuel/temperature readings
and their warning lights are separate signals: the renderer does not invent a
warning from a guessed threshold. The current layout is a development fallback,
not final OEM icon artwork or an independent instrument safety monitor.

`vehicle_indicator_snapshot` is the shared policy for the native cluster and
the optional native overlay over Ultra: it retains separate known/visible sets,
uses the original acquisition date and individual age policy when metadata
is present, or the legacy 500 ms snapshot policy otherwise. It clears
invalid, future or stale inputs. A known flashing lamp stays known during
its dark phase. All 24 indicators are tested through on/off/flash/unavailable
transitions. The file-driven overlay shares the accepted gauge sample and
GPU-composites cached original symbols above the signed scene. Its placement
and artwork are independent of asset subscriptions and phone vehicle-resource
IDs. Offline CM5 checks pass; live placement/clearing still needs qualification.

## Acquisition observations

`VehicleObservationCache` provides a read-only owner-thread adapter boundary
for mixed-rate sensors. Each of the ten readings and 24 lamps retains its
original acquisition date, immutable age policy, availability, provenance and
source generation. One additional coherent transmission observation carries
the related type/selector/gear components together. Another independent mode
observation retains its dated selection while its catalogue remains static.
Current trip and qualified heading add two independently dated compound
observations, for 38 acquisition records in the complete modern snapshot.
Delta batches are validated atomically; polls cannot renew
dates, and a fresh speed reading cannot renew a cached temperature or warning.
Native/backup rendering and Ultra script publication consume the same policy.

This component-tested path has no installed hardware driver. Its
[HVO1/HVO2/HVO3/HVO4 socket codec and receiver](VEHICLE_OBSERVATION_PROTOCOL.md) carry complete
metadata-bearing snapshots across processes without changing acquisition dates.
See [the observation contract](VEHICLE_OBSERVATIONS.md) for setup, ordering,
expiry and generation rules. HUV1/HUV2 below remain complete snapshots with
their original legacy timestamp behavior. A socket selects the transport family
from its first accepted packet; changing families requires a fresh consumer.
The observation receiver also pins its first accepted schema version and
drive-mode catalogue, including order/absence, across source generations.

## Local socket protocol

Start `headunit-cluster-wayland --vehicle-socket PATH`. Its Unix datagram socket
has owner-only permissions; use an exact absolute path in an existing private
directory owned by the consumer user. A live owner or unsafe existing path is
rejected. An owned dead socket left by a killed consumer can be reclaimed while
holding its private advisory lock; destruction removes only that owner's bound
inode. For acquisition-aware producers use [HVO1/HVO2/HVO3/HVO4](VEHICLE_OBSERVATION_PROTOCOL.md),
with 34 original fields, 35 including transmission, 36 including mode
plus its immutable catalogue header, or 38 including trip and heading, and a
maximum of 8192 bytes. The legacy HUV2 format below
is a **complete snapshot**, up to 2048 bytes:

```text
HUV2 speed_kmh=57.4 engine_rpm=2400 fuel_percent=42 coolant_c=91 battery_voltage=14.1 low_beam=on high_beam=off left_turn=flash check_engine=off
```

Fields may appear in any order. Numeric values use the units above. Indicator
values are `off`, `on`, `flash` or `-`. Numeric `-` is unavailable too. Omitted
fields become unavailable rather than retaining an earlier snapshot. Unknown
fields, duplicate keys, invalid numbers/states and oversized packets reject the
**whole packet**. A rejected packet cannot partially update speed or warnings.
`simulated=1` marks bench data visibly; omitted or `0` is not simulation.

The receiver assigns a monotonic receipt timestamp. Local display values expire
after 500 ms. Publish fresh, complete snapshots at least every 250 ms, faster
when the Ultra profile's freshness limit requires it. An adapter must invalidate
any reading whose physical source has gone stale; it must not repeatedly resend
old cached values as fresh. This receipt-timestamp protocol does not carry
per-sensor acquisition times. The separate local `SensorInput` interface remains
available for timestamped per-IID records.

When backup, Standard and Ultra run concurrently, give each consumer its own
socket and publish through the optional [vehicle distribution service](VEHICLE_DISTRIBUTION.md).
Its internal `HUF1` envelope preserves Pi ingress time across consumer queues;
queued packets do not become fresh again when a paused renderer resumes.
HVO1/HVO2/HVO3/HVO4 snapshots already have source dates and are forwarded unchanged instead.

Legacy `HUV1 57.4 2400` remains accepted for existing adapters. It contains only
speed/RPM; all additional readings and lights are unavailable. The C++ helper
`encode_vehicle_datagram(sample)` produces HUV2 and validates all fields before
serialization. It rejects transmission, drive-mode, trip/heading fields and per-signal observation metadata,
which HUV2 cannot preserve. See `include/headunit/vehicle.hpp` for typed fields and the
shared reading/indicator catalogue.

Socket ownership protects against other Unix users, not other processes under
the same account. Production isolation and physical sensor qualification remain
separate work. No writes from this interface actuate the vehicle.

## Bench simulation and snapshots

Built-in simulation cycles a different telltale every three seconds, with
changing speed/RPM and slow fuel/temperature variation:

```sh
headunit-cluster-wayland --simulate
```

To test the same socket path a real adapter will use, run the receiver first,
then use the bounded C++ sender:

```sh
headunit-cluster-wayland --vehicle-socket /run/user/1000/headunit-private/vehicle.sock
headunit-vehicle-simulator /run/user/1000/headunit-private/vehicle.sock 30
```

A third argument selects a complete HUV2 fixture. The sender always marks its
values as simulated, sends at 20 Hz, and stops after the requested 1–3600 seconds.
Stopping it tests unavailable-state handling. The checked-in example lights
several indicators and includes low fuel:

```sh
headunit-vehicle-simulator /run/user/1000/headunit-private/vehicle.sock 30 config/vehicle-demo.huv2
headunit-cluster-snapshot preview.ppm --sample config/vehicle-demo.huv2
```

## Ultra script bindings

The installed-UI receiver selects either `simulateGauges=true` or an absolute
`vehicleSocket=/run/user/1000/headunit-private/vehicle.sock` in its private
installed-UI file. Both are off by default and cannot be enabled together. The
socket belongs to that worker generation and is removed when the worker stops.
For the fan-out route this must be the `ultra` consumer path, distinct from the
backup and receiver paths.

The existing five timed speed/RPM bindings remain required by `GaugeInput`:

- `vehicleMotion.displayedSpeed.speed` (km/h × 1000)
- `vehicleMotion.displayedSpeed.speedKMH`
- `vehicleMotion.displayedSpeed.speedMPH`
- `internalCombustionEngine.engineRPM.rotationalSpeed`
- `internalCombustionEngine.engineRPM.rotationalSpeedState`

Additional timed signals in the **local profile** can specify `vehicleField`:

```json
{
  "iid": 20,
  "path": ["fuel", "fuelLevel", "fuelLevel"],
  "kind": "real",
  "maxAgeMs": 250,
  "minimumValue": 0,
  "maximumValue": 100,
  "vehicleField": "fuel_percent"
}
```

This is a readable illustration of a plist signal dictionary, not a phone VDP
schema. The IID is receiver-owned; choose an unused one in the local profile.
Optional `vehicleScale` and `vehicleOffset` provide explicit unit conversion.
Numeric output must match the declared real/integer kind; values that cannot be
represented within the profile's bounds are invalidated instead of clamped.

Indicator bindings require `vehicleStates`, containing the exact local script
values for **off, on, flashing**, in that order. Boolean or integer values must
match the signal kind. For example, a receiver-owned boolean headlight signal
could use `[false, true, true]`. Do not assume these are Apple's numeric enums:
inspect the particular asset before selecting mappings. Input bindings cannot
be fixed values or override the five existing speed/RPM bindings.

Transmission bindings instead use a nonempty `vehicleStates` **dictionary**:
semantic component keys map to integer or string script values. Numeric
scale/offset is rejected. Unmapped or unknown components withdraw only their
mapped scalar; related scalars publish atomically with the original compound
acquisition date. Gearbox type cannot select the inspected `transmissionMode`
path. [Transmission](TRANSMISSION.md#bind-a-signed-ultra-widget) shows the exact
shape and reference-pack mappings.

Drive-mode bindings use the same semantic dictionary shape, with case-sensitive
vehicle identifiers as keys. The inspected `currentIndex` path requires timed
integer outputs and a matching fixed authored `identifiers` array. Indices
must be unique and in bounds; vehicle catalogue order remains independent of
the pack. Unknown or valid-but-unmapped state withdraws the index instead of
selecting zero. The strict `--drive-mode-config` JSON generator creates both
declarations together and supplies no active measurement. See
[the configuration example](DRIVE_MODES.md#map-semantic-modes-to-signed-artwork).

### Bind observed trip consumption

The inspected repeated service exposes `tripComputer.trip.userVisibleLabel`
and `tripComputer.trip.fuelEfficiency`. The latter consumes **mL/100 km**,
whereas the vehicle source supplies whole-trip mean **L/100 km**. Configure an
explicit `vehicleScale: 1000`; conversion changes units, not acquisition dates
or the averaging window. A fixed same-instance label identifies the local
widget input and supplies no measured trip identity or statistics.

This fragment illustrates the three declarations; IIDs are locally allocated
and must not collide with other signals:

```json
[
  {
    "iid": 70,
    "path": ["tripComputer", "trip", "userVisibleLabel"],
    "instance": "currentTrip",
    "kind": "string",
    "maxAgeMs": 250,
    "fixedValue": "Current Trip"
  },
  {
    "iid": 71,
    "path": ["tripComputer", "trip", "fuelEfficiency"],
    "instance": "currentTrip",
    "kind": "real",
    "maxAgeMs": 250,
    "units": 674,
    "vehicleField": "trip.fuel_efficiency_l_per_100km",
    "vehicleScale": 1000
  },
  {
    "iid": 72,
    "path": ["vehicleUnits", "displayUnits", "fuelEfficiencyUnit"],
    "kind": "integer",
    "maxAgeMs": 250,
    "fixedValue": 671
  }
]
```

The inspected reference pack declares input unit **674 = mL/100 km** and display
unit **671 = L/100 km**. Its widget converts 6500 mL/100 km back to a displayed
6.5 L/100 km. These are pack-specific unit enums, not car sensor addresses or
phone vehicle-resource IDs. Other packs still need independent qualification.

Generate these bindings alongside the required speed/RPM signals:

```sh
python3 tools/create_cluster_bench_profile.py \
  --extended-readings --observed-trip \
  --trip-instance currentTrip --trip-label "Current Trip" \
  --output /private/cluster-trip.plist
```

`--observed-trip` defaults to the explicit bench instance `currentTrip` and
label `Current Trip`; `--trip-instance` and `--trip-label` can override them and
require that option. Instance names use 1–128 ASCII letters/digits/underscores/
hyphens. Labels are nonempty UTF-8 strings of at most 128 bytes without C0/C1
control characters. The generated label is immutable configuration. Consumption stays
unavailable until the source supplies a valid mean over positive distance.
No distance/time/average-speed/heading bindings or fixed consumption are added;
existing generator defaults and separate transmission/mode fixtures remain
unchanged when this option is omitted.

The generic trip scalar supports the existing bounded real/integer conversion
policy; an inexact integer result withdraws instead of rounding or clamping.
The generator uses real values and the inspected ×1000 conversion. Every mapped
mean keeps the original compound trip acquisition date. Unknown, missing mean,
stale or disconnected observations withdraw their scalar independently of
heading or fresh speed updates. Trip loss and source generation changes retain
history and require a genuinely newer acquisition for recovery, including after
an empty restart snapshot. Repeated unchanged expired snapshots remain
withdrawn; newer already-expired observations may advance hidden counter history
without becoming display values.

Neither this local binding nor a successful signed entry-point probe proves the
phone selected a Trip page. Actual iPhone center-content routing, complete Trip
page composition and physical statistics remain unqualified.

The current private asset was observed to use `fuel.fuelLevel.fuelLevel` on a
0–100 scale and `internalCombustionEngine.engineTemperature.temperature`, plus
separate display-unit/limit/state metadata. The extended bench generator also
routes these observed paths through the shared timestamped source:

| Local script path | Vehicle field / conversion |
| --- | --- |
| `environmentalConditions.exteriorConditions.temperature` | `ambient_c`, Celsius unit 1082 |
| `fuel.fuelRemainingRange.distanceKM` | `range_km`, kilometre unit 151 |
| `fuel.fuelRemainingRange.distanceMiles` | `range_km / 1.609344`, mile unit 164 |
| `tripComputer.remainingRange.distanceKM` | `range_km`, kilometre unit 151 |
| `tripComputer.remainingRange.distanceMiles` | `range_km / 1.609344`, mile unit 164 |
| `tripComputer.odometer.distanceKM` | `odometer_km`, kilometre unit 151 |
| `tripComputer.odometer.distanceMiles` | `odometer_km / 1.609344`, mile unit 164 |

These paths/units come from the inspected reference asset, not official Apple
vehicle-protocol identifiers. The display-unit selectors, limits, cold/hot
markers in the bench profile are explicit fixtures. Transmission defaults to
the older fixed `M`/third-gear fixture; `--observed-transmission` replaces it
with timed semantic mappings and supplies no measurement.
This combustion-engine bench supplies the same source range to fuel-specific
and combined-range paths; the pictured gauge labels use the latter. Configuring
only the fuel-specific pair did not produce those labels. The range widget also
requires `fuel.fuelLevel.portSideIndicator` to construct its controller: the
bench supplies the inspected pack's explicit NONE value (0), rather than
guessing the car's filler location. A future vehicle with
distinct fuel/electric/combined ranges needs correspondingly distinct readings.
Generate a private local binding file with:

```sh
python3 tools/create_cluster_bench_profile.py --extended-readings --output /private/bench.plist
```

The profile chooses bindings; `GaugeInput` chooses their values from the source.
The offline probe supplies labelled fixture measurements; a live worker uses
its configured simulator or socket. Range/odometer invalidate and expire with
their original sample just like speed and temperature. This richer profile is
offline-qualified and still needs live phone/background checks before becoming
the normal bench configuration.

The optional [native Ultra overlay](ULTRA_WARNING_OVERLAY.md) can show all 24
indicators independently of an asset's script subscriptions. It shares the
accepted gauge sample, renders only fresh active states, and uses configured
fixed slots above the signed scene. This is offline-tested; actual placement
over real phone video and every layout still needs live qualification.

This does not establish a universal
mapping for all 24 telltales. The generic bridge supports mappings when an asset
consumes them; every signal can already be shown by our local cluster. Changing
an input profile alone cannot add a missing widget to a signed layout.

The adapter preserves source timestamps. For observation metadata, each mapped
field uses its acquisition date and batch publication commits related scalars
atomically. Missing values invalidate their script
bindings, and polling cannot refresh an old sample. The profile's freshness
limit remains authoritative (250 ms in the reference profile). This local
script path is independent of Apple's separately file-driven Climate/settings
capability and provider schema.

The phone-facing vehicle service groups sibling profile bindings by service,
characteristic family and instance. A changed subscribed member resends the
subscribed group; ungrouped signals retain change-only delivery. The local
signed scene has an independent `ScriptView`, which groups verified
`driveState.transmissionStatus` updates alongside repeated instances and
`uiControl.uiState`. An unchanged Drive selector therefore accompanies a new
gear on both paths. Other single script services retain incremental updates.

Each member still uses its original acquisition date and freshness policy;
resending cannot revive an expired or withdrawn sample. Unsubscribed siblings
are omitted. The gauge-to-ScriptView regression fails on the service-only
repair, and passes with the local adapter fix. Grouped expiry, withdrawal,
disconnect, partial subscriptions and invalid group replacement also pass.
The final combined source at `089fd82` passes 145 portable and 145 macOS
ASan+UBSan CTests (Mac leaks disabled). Its combined Linux/GPU and real-phone
checks remain pending; these source checks do not deploy the repair or qualify
physical transmission data.

## Validation

Tests cover all reading limits, every indicator state, HUV1 compatibility,
HUV2 round trips and malformed/duplicate/oversized input. Renderer checks verify
that every lamp changes the image, flashing changes phase, unavailable differs
from off, and stale samples return to the unavailable view. Linux socket tests
exercise complete replacement and atomic rejection through real datagrams.
Ultra bridge tests verify fuel scaling, temperature conversion, kilometre/mile
range and odometer, negative ambient temperature, integer/boolean lamp mappings,
expiry and missing values. Trip checks additionally exercise explicit mL/100km
scaling, integer representability, zero versus undefined denominators, coherent
counter/reset rejection and unavailable mean consumption. No physical car
input is claimed yet.

The new observation/integration suites also check mixed-rate expiry at the
exact age boundary, original dates, provenance, generations, atomic service
failure, withdrawal and delayed recovery. Native/backup image checks ensure a
fresh speed update cannot keep an expired temperature or warning visible.
HVO1 actual Unix-socket and Ultra-service checks also cover early empty polls,
delayed source restart, exact acquisition retention, replay and withdrawal.
Actual HVO4 Unix-socket → `GaugeInput` → script-service checks preserve original
trip/heading dates, exercise independent expiry and unavailable mean, and reject
counter/reset conflicts atomically without consuming the valid recovery
sequence. They cover delayed source restart and empty-generation/equal-date
trip replay. Direct-source checks preserve hidden counter history through
partial/absent/expired input and accept only genuinely newer valid recovery.
These portable checks do not qualify the Linux kernel fan-out, live phone Trip
routing or physical car.

On 2026-09-27, four relevant test suites passed on the Mac and six on the CM5.
A CM5 headless Wayland smoke test delivered 160 fixture snapshots through the
actual Unix socket with zero drops; the cluster remained running after the
sender stopped. This verifies the software path, not physical sensor input or
the appearance of every signal in the current signed Ultra layout.
