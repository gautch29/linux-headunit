<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Display transmission state

Supply one observed transmission state to the native cluster and the local
CarPlay Ultra script service. Keep the hardware vocabulary independent of the
asset's numeric display enums. This is our read-only integration API, not an
official Apple interface or a gearbox controller.

## Describe what is known

`VehicleSample::transmission` is an optional `TransmissionState` declared in
`headunit/transmission.hpp`. Its three independently optional components are:

| Component | Accepted semantic values | Meaning |
| --- | --- | --- |
| `gearbox_type` | `manual`, `automatic`, `automated_manual`, `continuously_variable` | Installed transmission technology. |
| `selector` | `park`, `reverse`, `neutral`, `drive`, `low`, `manual`, `sport` | Observed effective selector/operating state. |
| `engaged_gear` | `neutral`, `reverse`, forward gear 1–32 | Actual engaged gear, when the source can determine it. |

At least one component must be known. Missing components remain unknown; a
fully unknown state is an absent optional. Hardware type does not establish
the effective selector or actual gear. Speed and RPM do not infer a gear.
An effective `sport` transmission selector is separate from a vehicle's Sport
drive-mode identifier.

Known related components must agree:

- Park or neutral requires a known engaged gear to be neutral.
- Reverse requires a known engaged gear to be reverse.
- Drive, low, manual or sport allows a known neutral or forward gear.

This contract describes effective state. A requested shift awaiting engagement
belongs in a future command/status interface; do not publish it here as an
observed effective selector. An incoherent observation rejects the entire
batch without partially changing other readings.

## Preserve one acquisition date

The compound state is one `VehicleObservation` under the key `transmission`.
It has one original acquisition date, provenance, availability and immutable
maximum age. Related selector/gear/type values commit and expire together.
If a hardware adapter acquires these separately, it must determine a coherent
observation and its qualified date; the renderer cannot do that integration.

```cpp
// Configure this policy before constructing the cache.
VehicleAgePolicy ages;
ages.transmission = std::chrono::milliseconds(100); // Example policy only.
VehicleObservationCache source(ages);
source.begin_generation(generation, started_at, now);

TransmissionState state;
state.gearbox_type = GearboxType::manual;
state.selector = TransmissionSelector::manual;
state.engaged_gear = EngagedGear{EngagedGearKind::forward, 3};
const std::array batch{
    VehicleObservation{"transmission", state, acquired_at}
};
source.accept(generation, batch_sequence, batch, now);
```

The values in this illustration are not a measurement of the project car.
Use `VehicleObservationOrigin::simulation` for an actual fixture. To report
an acquired unavailable state, supply `std::monostate` and
`VehicleAvailability::unavailable`; do not substitute neutral or gear zero.
`withdraw()` preserves recorded acquisition watermarks. At age **≥** the limit,
the state becomes stale. Polling or a faster speed update cannot renew it.
The source's normal replay, generation and delayed-recovery rules apply to the
whole compound state. See [observations](VEHICLE_OBSERVATIONS.md).

## Transport the state with HVO2

HVO1 retains its original 34 reading/lamp records. Select HVO2 explicitly for
the 35-record schema, which adds transmission:

```cpp
VehicleObservationPacket packet{source.sample(now), ++snapshot_sequence,
    VehicleObservationWireVersion::hvo2};
const auto bytes = encode_vehicle_observation_datagram(packet);
```

The added record uses a canonical compound key:

```text
transmission=manual/manual/3,1000000000,100000000,m,a
```

This record is an illustration, not a complete datagram. The dates must match
the packet's generation/publication and the actual host clock. Components use
`-` for unknown, for example `automatic/drive/-`. A completely unavailable
value is a single `-`; `-/-/-` is not a known observation. Numeric gear keys are
canonical 1–32, without leading zeros.

HVO1 and HUV2 encoding reject transmission they cannot retain. HVO1 also
rejects nondefault transmission timing. A receiver pins its first accepted
observation version, including across source restarts; changing HVO1–HVO4
requires a new consumer instance. Fan-out forwards original bytes unchanged.
See [the complete protocol](VEHICLE_OBSERVATION_PROTOCOL.md).

If the same source also supplies observed drive mode, select HVO3; it carries
this transmission record unchanged plus the mode catalogue/active observation.
[Drive modes](DRIVE_MODES.md) documents that separate input. HVO2 cannot retain
drive-mode fields and rejects their lossy encoding.

Select HVO4 when the source additionally supplies current-trip statistics or
heading; it retains the transmission and mode schemas unchanged. Earlier
versions reject those values or nondefault timing. [Trip and heading](TRIP_AND_HEADING.md).

## Bind a signed Ultra widget

Local plist signal dictionaries accept three source fields:

- `transmission.selector`
- `transmission.gear`
- `transmission.gearbox_type`

Their `vehicleStates` is an explicit semantic-key dictionary. Integer or string
outputs must match the signal kind and bounds. Numeric scale/offset is invalid.
Unknown or unmapped components withdraw that script scalar; they never become
zero, `M` or a guessed label. The parser rejects binding gearbox type to the
inspected `transmissionMode` path.

This JSON is a readable illustration of two **local plist** dictionaries:

```json
[
  {
    "iid": 40,
    "path": ["driveState", "transmissionStatus", "transmissionMode"],
    "kind": "integer",
    "maxAgeMs": 250,
    "vehicleField": "transmission.selector",
    "vehicleStates": {
      "park": 80, "reverse": 82, "neutral": 78, "drive": 68,
      "low": 76, "manual": 77, "sport": 83
    }
  },
  {
    "iid": 41,
    "path": ["driveState", "transmissionStatus", "gearPosition"],
    "kind": "integer",
    "maxAgeMs": 250,
    "vehicleField": "transmission.gear",
    "vehicleStates": {
      "neutral": 0, "reverse": 0,
      "1": 1, "2": 2, "3": 3, "4": 4, "5": 5, "6": 6
    }
  }
]
```

Choose unused receiver-owned IIDs and mappings for the inspected pack and the
configured car's supported gears. These numbers are observed artwork enums,
not phone vehicle-resource IDs. The reference uses a positive position for a
forward number and a nonpositive position for a selector label; the separate
selector distinguishes reverse from neutral. Some numeric-only widgets hide
nonforward gear text. That is a layout policy, not evidence of lost telemetry.

`GaugeInput` publishes the component mappings in one service batch with their
original date. Conflict checks compare the entire state, including components
that this particular profile does not render. The millisecond script clock
never invents a later acquisition when two observations share a millisecond.

## Native and diagnostic presentation

The native cluster places a compact gear badge below the RPM gauge. Effective
park displays `P`; otherwise actual gear displays `R`, `N` or a forward number.
When actual gear is unknown, a known effective selector supplies its letter.
Absent, stale, invalid or gearbox-type-only data displays `--`.

The normal bench generator preserves its older fixed `M`/third-gear artwork
fixture. Opt into observed mappings separately:

```sh
python3 tools/create_cluster_bench_profile.py \
  --extended-readings --observed-transmission --output /private/observed.plist

build/headunit-native-cluster-snapshot /private/third.ppm \
  --center vehicle --transmission-fixture manual/manual/3
```

For isolated signed CPU layout probes, combine that profile with
`HEADUNIT_THEME_PROBE_GAUGES=1` and
`HEADUNIT_THEME_PROBE_TRANSMISSION=manual/manual/3`; the literal `unavailable`
tests missing state. This diagnostic requires a local profile. Its dates,
foreground and vehicle state are synthetic; it establishes neither physical
gear acquisition nor a live iPhone transition. The ordinary vehicle simulator
does not infer or silently add transmission state.

## Qualification

Portable tests cover canonical parsing, partial knowledge, impossible
combinations, atomic replacement, provenance, acquisition conflict/replay,
exact expiry, withdrawal/recovery, version pinning and actual HVO2 Unix socket
publication through the Ultra service. Native pixel checks confine the gear
change to its badge and verify expiry while speed remains current.

The 2 October regular DigiRing CPU fixture renders third and fourth gear with
the observed mappings. Its numeric-only slot intentionally displays the brand
logo for known reverse and authored redaction for missing state. Exact RGBA
comparisons confine changes to the gear/logo/redaction region; adjacent Sport,
coolant, range and ambient labels stay identical. Sport remains a separate
fixed fixture, not an observed vehicle mode. These four cases do not establish
all layouts/states. Private captures stay outside Git. Linux HVO2 broker tests,
CM5 deployment, physical acquisition and live phone/style checks remain pending.

[Vehicle input](VEHICLE_INPUT.md) · [Adapter plan](VEHICLE_ADAPTER_PLAN.md) ·
[Cluster completeness](CLUSTER_COMPLETENESS.md).
