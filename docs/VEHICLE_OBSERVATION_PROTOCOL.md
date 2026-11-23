<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Transport vehicle observations between processes

Use HVO1–HVO4 or explicitly selected HVO6 to send the shared vehicle model between processes on the same host.
Every reading carries its original acquisition date, age policy, provenance
and availability. Snapshot publication and receipt are separate events; the
receiver never substitutes either date for acquisition.

This protocol is implemented for local Unix datagrams and the optional fan-out
service. It has no hardware addresses, calibration coefficients or actuator
commands. Physical acquisition and microcontroller clock mapping remain open.

## Send a complete snapshot

`VehicleObservationPacket` contains a metadata-bearing `VehicleSample`, a
positive snapshot sequence and an explicit wire version. HVO1 is the default
for compatibility. Select HVO2 for transmission, or HVO3 for observed drive
mode and its immutable capability catalogue, or HVO4 for current trip and
qualified heading. Encode it with `encode_vehicle_observation_datagram`.
For example, a producer can acquire delta batches through
`VehicleObservationCache`, then encode its complete snapshot for all consumers:

```cpp
// source is an initialized VehicleObservationCache on the producer's thread.
VehicleObservationPacket packet{source.sample(now), ++snapshot_sequence,
    VehicleObservationWireVersion::hvo4};
const std::string bytes = encode_vehicle_observation_datagram(packet);
// Send bytes as one datagram to the configured adapter/fan-out socket.
```

Sequences advance per published snapshot within a generation, independently
of how many delta batches the cache accepted. A heartbeat is a new snapshot,
not a new acquisition: its sequence/publication date may advance while each
reading keeps its old date and expires normally.

HVO1 requires all 34 original reading/lamp fields exactly once. HVO2 requires
those fields plus the coherent `transmission` record: 35 in total. HVO3 adds
the `drive_mode` observation for 36 records and a mandatory `drive_modes=`
catalogue header. HVO4 retains that header and adds `trip` and `heading` for
38 records. An unobserved field still carries its age policy and unavailable
state. Omission is a malformed snapshot, not a delta or an implicit reset.
HVO1 encoding rejects a transmission value or nondefault transmission timing
rather than dropping it.
Both HVO1/HVO2 reject a mode catalogue, active mode or nondefault mode timing.
HVO1/HVO2/HVO3 reject trip/heading values and nondefault timing they cannot
preserve. Select an appropriate version explicitly; there is no lossy downgrade.

| Version | Header tokens | Observation records | Additional state |
| --- | ---: | ---: | --- |
| HVO1 | 5 | 34 | Original readings and indicators. |
| HVO2 | 5 | 35 | Coherent transmission. |
| HVO3 | 6 | 36 | Observed drive mode and immutable ordered catalogue. |
| HVO4 | 6 | 38 | Current trip and qualified heading. |
| HVO6 | 6 | 40 | HVO4 plus acceleration and GPS position. |

The older schemas retain these exact shapes. Newly added channels must be
completely unknown to an older encoder: no value or catalogue, no acquisition,
500 ms age, measured origin and unavailable state. A different age policy or
withdrawal state is meaningful metadata and must not be silently dropped.

## Sport observations (HVO6)

HVO6 keeps the exact HVO4 header and adds two mandatory records:
`acceleration=longitudinal_mps2/lateral_mps2,acquired_ns-or-,maximum_age_ns,origin,availability`
and
`gps_position=latitude_deg/longitude_deg/horizontal_accuracy_m,acquired_ns-or-,maximum_age_ns,origin,availability`.
A whole missing value is `-`. Acceleration is calibrated, gravity-compensated
vehicle-frame forward/left m/s², finite components within ±100. GPS is a qualified
WGS84 fix within ±90/±180°, with positive finite horizontal accuracy ≤10000 m.
Neither raw specific force nor an unqualified navigation fix is available.
Acquisition, age, replay, origin and retirement rules are the same as other signals.
HVO1–HVO4 retain exact shapes and refuse populated/nondefault sport metadata.
HVO5 remains reserved for the unimplemented MCU night-mode proposal; HVO6 does
not implement that separate extension. The packet bound remains 8192 bytes.
See [sport telemetry](SPORT_TELEMETRY.md) for engine rules, JSON and simulators.

## Wire format

One ASCII datagram, at most **8192 bytes**, with single spaces between tokens:

```text
HVO1 <generation> <sequence> <started_ns> <published_ns> <field> ...
HVO2 <generation> <sequence> <started_ns> <published_ns> <field> ...
HVO3 <generation> <sequence> <started_ns> <published_ns> drive_modes=<catalogue-or-> <field> ...
HVO4 <generation> <sequence> <started_ns> <published_ns> drive_modes=<catalogue-or-> <field> ...
```

Each field uses this shape:

```text
<semantic-key>=<value>,<acquired_ns-or->,<maximum_age_ns>,<origin>,<availability>
```

This is a shape description; the ellipsis is not valid packet content.
Keys and numeric units come from the existing reading/indicator catalogues.
Field order is unrestricted; the encoder uses catalogue order. Header values
and acquisition/age values are exact integers. No quoting, optional spaces,
trailing newline or nested transport envelope is supported.

| Component | Meaning |
| --- | --- |
| `generation` | Positive 64-bit source generation; advances after source restart. |
| `sequence` | Positive 64-bit snapshot sequence, strictly increasing within that generation. |
| `started_ns` | Positive source-generation start in the host steady-clock domain. |
| `published_ns` | Original complete-snapshot publication date in that same domain. |
| Numeric value | Finite catalogue-bounded number, or `-` for absence. |
| Indicator value | `off`, `on`, `flash`, or `-`. |
| HVO2–HVO4 transmission value | Canonical `type/selector/gear`, with `-` for an unknown component; a single `-` for absence. |
| HVO3/HVO4 `drive_modes` header | Immutable ordered comma-separated catalogue of 1–32 distinct semantic identifiers, or `-` if unconfigured. |
| HVO3/HVO4 `drive_mode` value | A supported semantic identifier, or `-` for absence. Its age/provenance/availability follow the same per-signal shape. |
| HVO4 `trip` value | Canonical identifier/reset/distance/elapsed/moving/average/basis/fuel-efficiency; optional statistics use `-`, whole absent state uses one `-`. |
| HVO4 `heading` value | Canonical degrees/reference/source, or `-` for absence. Degrees must be `[0,360)` with explicit valid north reference and source. |
| `acquired_ns` | Positive original acquisition date, or `-` when none was recorded. |
| `maximum_age_ns` | Explicit positive per-signal limit, at most 24 hours. |
| Origin | `m` measured, `c` calculated, `s` simulation. |
| Availability | `a` available, `u` unavailable, `s` stale, `d` disconnected. |

Dates must be exactly representable in the host clock. The ordering is:
generation start ≤ acquisition ≤ publication ≤ receipt. Available fields need
a value and an acquisition younger than their age limit **at publication**.
Stale fields need an acquisition at or beyond that limit and no value.
Unavailable/disconnected fields have no value; acquisition may be retained or
absent. The aggregate simulation marker is derived from recorded per-signal
origins, not supplied as a separate wire flag.

Zero and off are known values. They must not be replaced with `-` merely
because they look inactive. A disconnected source must not report zero speed
as an inferred measurement.

For example, `transmission=automatic/drive/3,1000000000,100000000,m,a`
is one coherent acquired observation. `automatic/drive/-` retains a known
effective selector without inventing an actual gear. `-/-/-`, unsupported
keys, leading-zero numeric gears and contradictory selector/gear combinations
are rejected. See [transmission semantics](TRANSMISSION.md) for supported
components and the distinction between effective state and a shift request.

Mode identifiers are case-sensitive ASCII, begin with a letter and contain
only letters, digits, `_` or `-`, up to 64 characters. The active identifier
must belong to the declared catalogue. Catalogue capability is separate from
the dated current observation; mode loss or expiry does not remove supported
modes. See [drive modes](DRIVE_MODES.md).

Trip and heading are separate compound acquired observations. For example,
`trip=currentTrip/1/65/3900/3600/60/total/7.4,1000000000,250000000,c,a`
and `heading=45/true_north/vehicle_orientation,1000000000,100000000,m,a`
are illustrative records, not a complete packet. Trip uses km, seconds, km/h
and whole-trip mean L/100km; heading uses degrees clockwise from explicit
true/magnetic north and distinguishes orientation from travel course.
Trip identifiers are case-sensitive ASCII identifiers of 1–32 characters.
The reset generation is a positive 64-bit number. Distance uses km; elapsed
and moving duration use seconds. Average speed requires its explicit `total`
or `moving` basis and coherent distance/duration. Fuel efficiency is the
whole-trip mean in L/100km and may increase or decrease; it is not a monotonic
fuel counter. Use `trip_state_key` and `parse_trip_state_key` for the exact
canonical eight-component representation.

Heading explicitly names `true_north` or `magnetic_north`, and
`vehicle_orientation` or `course_over_ground`. A genuine qualified zero is
north; an unknown heading is `-`, never an invented zero or unknown enum.
Use `heading_state_key` and `parse_heading_state_key` for its canonical
three-component representation. Trip and heading each have their own
acquisition date and expiry policy.

Canonical compound numbers use shortest round-tripping general notation.
No snapshot poll, map instruction, speed or source restart calculates these
values. See [trip/heading semantics](TRIP_AND_HEADING.md) for coherence limits
and source qualification.

## Receive and validate once

`parse_vehicle_observation_datagram(bytes, received_at)` validates the complete
shape, values and clock consistency without inventing dates. It does not own
replay state. `VehicleObservationReceiver` owns that state on one thread:

- Same generation: start stays fixed, sequence advances, publication cannot
  reverse, and per-field acquisition dates cannot reverse or disappear.
- Same acquisition: original value, provenance and explicit available/
  unavailable state cannot conflict. Stale/disconnected transitions withdraw
  a field without replacing its original acquisition watermark.
- A withdrawn observation cannot become available again at the same date.
  A newer, still-current acquisition may recover it, including delayed delivery.
- New generation: generation advances and its start is no earlier than the
  last accepted **source publication**, rather than a later consumer poll.
  Ordinary per-generation acquisition history clears; age policies remain
  immutable. Current-trip identity/reset/counter history survives independently.
- Every invalid snapshot is rejected atomically. It cannot consume the next
  sequence, advance the accepted clock or partially change instruments.
- The first accepted observation version stays fixed, including across source
  generations. HVO1–HVO4 changes require a new receiver, not a silent downgrade
  or schema upgrade. Equal-date conflicts compare the entire compound state,
  including transmission, trip components and heading qualification.
- The first accepted drive-mode catalogue stays fixed, including order and
  absence, across source generations. Replacement/removal requires new source
  and consumer instances; an invalid change cannot consume its sequence.
- Current-trip identity remains fixed after its first known acquisition.
  Empty/unknown startup does not bind an identity. Reset generations and the
  known distance, elapsed and moving counters cannot reverse through omitted
  components, unavailable snapshots or source restart.
- A higher trip reset permits smaller counters only with a strictly newer
  acquisition. Source generation and trip reset are separate identities:
  restarting an adapter is not evidence that the driver reset the trip.
- Trip loss, receipt/poll expiry and source restart require a new acquisition
  before recovery. Even an identical old value cannot return through a new
  generation or a new-generation empty snapshot. Changed provenance at that
  old date cannot make it new. An honest empty generation retains no fabricated
  acquisition date in its public snapshot; private trip history stays separate.

### Preserve counter chronology

The cache and receiver use the same allocation-free `TripProgress` transition
helpers. They retain each known counter and its acquisition date when a later
trip observation omits that component. Those watermarks validate history;
they are not substituted into the new public trip value.

For example, moving time of 300 seconds followed by a newer elapsed reading
of 250 seconds is contradictory and rejected, even if the newer observation
omits moving time. The reverse chronology is different: an old elapsed reading
of 250 seconds does not disprove a newer moving reading of 300 seconds when
current elapsed time is unavailable. The next equally/newer elapsed reading
must cover known moving time. No missing current duration is invented from
an older counter.

Trip history updates are candidates until the complete packet succeeds.
A conflict in a later heading record cannot consume a trip reset, counter
advance, sequence or receipt clock. `withdraw_trip_progress` preserves known
identity/counters/dates and marks recovery as requiring a newer acquisition;
`update_trip_progress` clears that mark only for a valid newer observation.

`sample(now)` filters each field against its original date and retains the
original source publication date. It does not republish a sensor reading.
A consumer-filtered view can be stale now while its original source snapshot
was fresh at publication; forward original bytes, rather than re-encoding that
filtered view with a made-up publication date.

`SocketVehicle` drains at most 64 datagrams per turn and applies these rules to
HVO1–HVO4. The first accepted packet selects the observation or legacy transport
family for that socket instance; later family changes are dropped. A socket
that has not received its first packet stays uninitialized, so early Ultra
polls do not accidentally lock it into the legacy timestamp format.
Construction can happen on a control thread before exclusive handoff to a
renderer. The first `sample()` call selects the socket's sole sampling owner
and constructs its observation receiver there; later calls from another thread
are rejected. This supports the Standard cluster's existing painter handoff.

## Distribute without renewing data

The optional Linux fan-out service accepts all four observation versions within
the size limit and forwards their bytes unchanged. Kernel receive-time checks
still reject queued, future or truncated input. The broker never caches a last
snapshot for a restarted consumer. Schema and replay validation occur independently in each
consumer, so the broker does not maintain a second interpretation of the
vehicle model.

HUV1/HUV2 remain limited to 2048 bytes and keep their existing complete-snapshot
rules. Only those legacy packets receive the HUF1 ingress envelope. HUF1
wrapping HVO1–HVO4 is rejected; acquisition dates cannot be replaced by ingress
dates. HUV2 encoding refuses acquisition metadata, transmission, drive-mode,
trip or heading fields it cannot preserve.

Use one configured producer and one host clock domain per private endpoint.
Owned private directories, socket permissions and advisory locks isolate
different Unix users; this is not a remote or authenticated vehicle bus.
The validated source must choose its generation and age policies. A rebooted
consumer cannot infer a producer's historical generation from nothing; the
broker's no-replay behavior and acquisition expiry still apply.

## Verify the integration

The current strict Mac codec and receiver suites pass, including focused
AddressSanitizer/UndefinedBehaviorSanitizer runs with no findings:

- [Codec tests](../tests/vehicle_observation_wire_test.cpp): nine sections cover
  exact historical schemas, original nanosecond dates, coherent compounds,
  malformed catalogs/values/timing, maximum identities and size boundaries.
  A full 38-value snapshot with a 32-entry catalogue of 64-character mode IDs,
  a 32-character trip ID and maximum integer identities/host date round-trips.
  Exactly 8192 bytes is accepted; 8193 bytes is rejected.
- [Receiver tests](../tests/vehicle_observation_receiver_test.cpp): 18 named
  cases cover replay, full-resolution ordering, immutable version/catalogue,
  failed-later-field atomicity, counter history through omission/loss/restart,
  coherent reset, chronological partial counters, sticky same-date recovery
  rejection and independent heading expiry.
- [Broker tests](../tests/vehicle_fanout_test.py): 15 cases exist. Eight portable
  cases pass on Mac; seven Linux kernel cases are skipped there. Portable
  checks verify unchanged HVO1–HVO4 bytes, queue bounds and separate legacy
  limits. Linux fixtures include complete HVO2/HVO3/HVO4 snapshots, absent
  consumer/no-replay behavior, unsafe endpoints and queued-input rejection;
  those kernel fixtures still require a Linux run.

Actual Unix-socket checks feed snapshots into the Ultra gauge bridge and test
that faster speed updates cannot renew old temperature values. Consumer
restart, private ownership and legacy checks remain in place. The oversized
socket fixture permits a Unix kernel to reject a packet with `EMSGSIZE` before
delivery; codec tests independently establish the exact byte boundary.

These local results do not qualify physical car acquisition, CM5 deployment,
automotive power loss, actual dual-display timing or real-phone behavior with
new trip/heading inputs. Those remain separate acceptance checks.

[Observation model](VEHICLE_OBSERVATIONS.md) ·
[Vehicle distribution](VEHICLE_DISTRIBUTION.md) ·
[Adapter plan](VEHICLE_ADAPTER_PLAN.md).
