<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Supply trip statistics and heading

Send an acquired current-trip snapshot and a qualified heading through the
shared vehicle source. Native instruments display supplied values; the Ultra
bridge maps only inspected script inputs. Neither renderer calculates a trip
from speed, elapsed host time, odometer or navigation messages.

This is our independent read-only integration API, not an official Apple
protocol specification. No physical trip reader, persistent trip calculator,
vehicle reset command or heading sensor is supplied by this component.

## Ordinary gauge simulation odometer — source-only, 14 October 2026

`SimulatedVehicle` is the existing ordinary gauge fixture, separate from the
sport simulator and software drive statistics below. Its odometer now integrates
its unchanged speed profile from the supplied source start:

```text
speed(t) = clamp(42 + 24 sin(0.12 t), 0, 130) km/h
odometer(t) = 123456 + [42 t + 200 (1 - cos(0.12 t))] / 3600 km
```

Here `t` is elapsed source seconds. The sinusoid always lies in **18–66 km/h**,
strictly inside the clamp, so no clipped segment needs separate integration.
Distance has a positive derivative throughout the curve. Calculation takes
constant work with no accumulated sampling error or dependence on poll frequency.
The source still rejects dates before its start; requesting an earlier date
that remains after start still returns that date's deterministic fixture.
Repeated dates return identical values. Source dates, speed/RPM, transmission,
drive mode, lamps and all other readings remain unchanged, with `simulated=true`.
No observation metadata, physical readings, provider, reset or persistence is added.
Acquired `TripState`, physical odometer contracts and software statistics are unchanged.

The production regression first failed on original source `3670763`: at 20 s
the odometer was **123456.32339509075 km**, then at 35 s it fell to
**123456.2049656531 km**. The old elapsed-time × current-speed calculation
did not integrate distance. The corrected values are **123456.32985520642 km**
and **123456.49112560118 km**. `simulated-vehicle` now checks analytical distances,
half/full periods, five independently integrated segments of actual sampled
speed, 28,800 chronological comparisons, the initial baseline, repeated dates,
sparse/out-of-order polling, pre-start rejection and current-sample expiry.
Its bounded late-time cases reach 315577200 source seconds, within existing
clock/reading bounds; they are arithmetic fixtures, not physical endurance evidence.
An original-versus-fixed probe also compares every non-odometer fixture output
at eleven source dates from start through 315576000 s.

Private source, failed and passing logs, Testing, hashes and retirement receipts
are retained under `20261014/codex-simulation-odometer/` in the qualification cache.
The focused macOS sampler/core/observation/native-cluster/gauge caller checks
pass **8/8**, as do the same eight checks on Debian ARM64 with Clang 19
ASan+UBSan, enabled leak checks and the existing external suppressions. No warning,
deadline or queue policy changed. Actual own-container INT/TERM/HUP cleanup
passes; both Mac build trees and all five own containers are absent after
verified retention. The first signal harness failed a case-sensitive Docker
error-text assertion after successful container removal; that failed log and
the independent absence audit remain retained separately from the passes.
There are no UI keys, actions, IDs or new UI pieces for native UI integration to review.
These are simulation/source component checks; no deployment, phone, camera,
audio, fresh-frame or physical-car acceptance follows.

Before adoption the manager must review the whole combined source and run its
coherent required matrix for a later immutable generation. The sole phone-session
owner can then check the existing labelled simulation on native fallback and
connected gauges while preserving main touch, Home/Back, Climate/settings, both
streams, UI-Sync, audio/control and owner identities. A rounded display is not
an arithmetic oracle: reproduce exact source behavior with
`ctest --test-dir BUILD --output-on-failure -R '^simulated-vehicle$'` on the
qualified candidate. Do not inject fixtures into a live physical provider;
actual car odometer/TripState qualification remains parked with its provider contract.

## Describe the current trip

`TripState`, declared in `headunit/trip.hpp`, describes one coherent current
trip. It has a stable semantic `identifier` and positive `reset_generation`.
Identifiers are case-sensitive ASCII, 1–32 characters, starting with a letter;
remaining characters may be letters, digits, `_` or `-`.

| Field | Canonical unit / meaning | Unavailable state |
| --- | --- | --- |
| `distance_km` | Distance accumulated for this trip | Absent optional |
| `elapsed_seconds` | Total elapsed trip time, including stops | Absent optional |
| `moving_seconds` | Trip time counted as moving by the source | Absent optional |
| `average_speed_kmh` | Average over explicitly selected total or moving time | Absent optional, with absent basis |
| `average_speed_basis` | `total` or `moving`; required exactly when average speed is known | Absent optional |
| `fuel_efficiency_l_per_100km` | Whole-trip mean consumption in litres per 100 kilometres | Absent optional |

At least one statistic must be supplied. Every known number must be finite
and nonnegative. Plausibility bounds are 9,999,999 km distance, 315,576,000 s
duration, 300 km/h average speed and 200 L/100 km consumption. These bounds
are protocol guards, not calibration, warning thresholds or car capabilities.

Moving time cannot exceed known total time. A supplied average requires known
distance and a positive known duration for its selected basis. Its difference
from `distance_km × 3600 / duration_seconds` may not exceed **0.5 km/h**, to
accept sources rounded to a whole kilometre per hour, plus a small floating-point
comparison epsilon. The validator checks
coherence; it does not fill a missing average. Zero duration never becomes a
division or an inferred zero average. Mean consumption requires positive
known trip distance; zero-distance consumption stays unavailable.

The source owns the moving-time definition and accumulation/persistence policy.
For example, it must specify whether an ignition pause ends a trip, whether a
stopped engine counts toward elapsed time and how it persists counters through
power loss. This software does not choose those car-specific policies.

## Preserve reset and acquisition history

The first accepted current-trip identifier remains fixed for each source and
consumer lifetime. Changing which trip that endpoint represents requires new
instances. A reset is an **observed** higher `reset_generation`, not a command
from the display.

- A higher reset generation requires a genuinely newer acquisition date and
  permits counters to begin again at smaller values.
- Within one reset, supplied distance, elapsed time and moving time cannot
  decrease. Counter watermarks survive partial unknown state, source loss and
  source-generation restart.
- A newly observed total elapsed time cannot be smaller than an equally old
  or older retained moving time. A newer moving counter with current total
  time unavailable cannot be compared against an older total as if both were
  current; per-counter acquisition dates preserve that distinction.
- Reset generations cannot reverse after reconnect. Source restart and trip
  reset are separate events; restarting a process must not manufacture a reset.
- Equal-date conflicts, a different trip identity or an invalid reset reject
  the entire candidate update. They cannot consume its accepted sequence/date
  or partially change another instrument.

The bounded `TripProgress` value and `update_trip_progress` helper enforce the
same contract in the acquisition cache, Unix receiver and direct Ultra source
bridge. Validation occurs on a candidate before the caller commits any state.
Absent trip observations leave the retained history intact.
Loss and source-generation restart also require a newer acquisition before
known trip data can return; an identical old snapshot cannot revive it.

Each trip snapshot has one original acquisition date, availability, provenance
and age policy. The adapter must supply a coherent snapshot; it cannot stamp
old cached statistics with a newer speed or publication date. Omit internally
unavailable statistics or retain the qualified acquisition date that applies
to the whole snapshot. Polling and heartbeats do not renew it.

## Describe a qualified heading

`HeadingState`, declared in `headunit/heading.hpp`, requires all three fields:

| Field | Meaning |
| --- | --- |
| `degrees` | Finite degrees clockwise from north, `0 ≤ degrees < 360` |
| `reference` | Explicit `true_north` or `magnetic_north` |
| `source` | Explicit `vehicle_orientation` or `course_over_ground` |

Default/unknown enums are invalid. A missing heading is an absent optional,
not zero degrees. `360` is rejected rather than silently normalized to north.
True north and magnetic north cannot be exchanged without a qualified source
conversion. Vehicle orientation and travel course are different measurements:
a stopped car may have an orientation while its travel course is unavailable.
The source decides validity at low speed; the host never derives heading from
the phone's map, a turn arrow or a stationary course value.

Heading has its own acquisition date and age policy, independently of trip,
speed and the navigation session. Supplied reference/source changes require
the same fresh observation and conflict checks as any other value.

## Acquire values through the cache

Include `headunit/vehicle_observation.hpp`. The acquired semantic keys are
`trip` and `heading`; the corresponding `VehicleSample` members are optional.
Use the existing owner-thread cache, host steady-clock domain, generation and
atomic batch contract:

```cpp
TripState trip;
trip.identifier = *parse_trip_identifier("currentTrip");
trip.reset_generation = 1;
trip.distance_km = 65.0;
trip.elapsed_seconds = 3900.0;
trip.moving_seconds = 3600.0;
trip.average_speed_kmh = 60.0;
trip.average_speed_basis = TripAverageSpeedBasis::total;
trip.fuel_efficiency_l_per_100km = 7.4;

HeadingState heading{45.0, HeadingReference::true_north,
    HeadingSource::vehicle_orientation};
const std::array batch{
    VehicleObservation{"trip", trip, acquired_at,
        VehicleObservationOrigin::simulation},
    VehicleObservation{"heading", heading, acquired_at,
        VehicleObservationOrigin::simulation}
};
source.accept(source_generation, sequence, batch, now);
```

These numbers are explicit bench simulation. A physical adapter must provide
qualified readings and provenance instead. `VehicleAgePolicy::trip` and
`::heading` default to 500 ms; configure suitable positive limits before
constructing the source. Neither default qualifies an actual sensor.
Expiry occurs at **age ≥ maximum age** and withdraws the corresponding value.

## Transport the snapshot

Select `VehicleObservationWireVersion::hvo4` explicitly. HVO4 retains HVO3's
immutable mode-catalogue header and includes all **38** acquisition records:
the original 34, transmission, active drive mode, trip and heading. The maximum
datagram remains 8192 bytes. HVO1/HVO2/HVO3 keep their exact historical schemas
and reject fields or nondefault timing that they cannot carry. HUV2 also
rejects lossy trip/heading encoding.

Use the canonical `trip_state_key` / `parse_trip_state_key` and
`heading_state_key` / `parse_heading_state_key` helpers. Each complete compound
value remains one ordinary dated observation, not independently refreshed
scalar records. Source generation and current-trip reset remain distinct.
The slash-delimited trip key order is identifier, reset generation, distance,
elapsed time, moving time, average speed, basis and mean consumption. Heading
order is degrees, reference and source. For example:

```text
currentTrip/1/65/3900/3600/60/total/7.4
45/true_north/vehicle_orientation
```

Optional trip components use `-`; the whole absent compound uses one `-` in
its wire record. Numeric keys use canonical shortest round-tripping forms;
leading zeroes, `1.0` instead of `1`, negative zero and nonfinite values are
not accepted. Use the provided encoder rather than formatting sensor values
with a locale-dependent display formatter.
The optional fan-out forwards unchanged original bytes and keeps no cached
snapshot for a restarted consumer. [Wire contract](VEHICLE_OBSERVATION_PROTOCOL.md).

## Present native instruments

The native cluster adds a **Trip** center page alongside Map, Media and
Vehicle. It is available with or without a phone and retains the existing
gauges and active-only warning lights. Distance, elapsed/moving time, supplied
average speed, mean consumption and heading show `--` when absent or expired.
The average names its time basis; heading names its reference and source.
Display rounding does not modify the accepted input or its dates.

```sh
python3 tools/cluster_page.py --state /private/cluster-page.txt trip
```

The native phone manager exposes the same selection. The Standard renderer
hides its map surface while Trip is selected; selecting Map restores it only
when a Standard CarPlay session is available. These pages do not replace an
active signed Ultra cluster.

## Bind inspected Ultra consumption

The inspected regular/Single asset family exposes the repeated local
`tripComputer.trip` service with `userVisibleLabel` and `fuelEfficiency`.
Its consumption input uses **mL/100 km**, while the shared source uses
**L/100 km**. An explicit profile conversion of 1000 connects them.
Inspected asset unit enums are input 674 and display 671; these are artwork
configuration, not semantic hardware units or official phone identifiers.

No local signed input for trip distance, elapsed time, average speed or heading
has been established. Supplying those native values does not justify guessing
a `CP` path or phone resource. Likewise, an accepted consumption binding does
not prove that the phone supplies the photographed Trip center page. Qualify
the actual signed widget and phone content route separately.

For an explicit bench binding, generate a local profile:

```sh
python3 tools/create_cluster_bench_profile.py \
  --extended-readings --observed-trip \
  --trip-instance currentTrip --trip-label "Current Trip" \
  --output /private/observed-trip.plist
```

Instance and label are configured display names, not a measured identity or
proof of a phone-provided Trip page. The generated timed
`vehicleField: "trip.fuel_efficiency_l_per_100km"` requires an acquired
coherent trip with a known mean; it has no fixed consumption reading. This
binding uses the trip's original date and withdraws on source loss or expiry.

## Qualification boundaries

The 2 October integration passes fifteen affected portable suites, including
actual HVO4 socket → gauge → service publication, native Trip pixel boundaries,
independent exact expiry, page selection and phone management. Focused model/cache
and codec/receiver address/undefined-behavior sanitizer runs pass. Twelve
generator cases cover the inspected consumption binding and bounded configuration.
The original broker run skipped Linux-only cases on Mac. On 4 October, sixteen
actual CM5 broker checks passed kernel ingress/date/queue/ownership and restart
behavior; see [distribution](VEHICLE_DISTRIBUTION.md). Seven private 1920 × 720 native snapshots cover known,
independently unknown, zero, heading-wrap and bounded input. Two regular DigiRing
CPU fixtures return signed entry points with known/unavailable mean consumption.
That startup result does not prove a visible authored Trip widget or phone page.

Record component, native-pixel and actual socket/service evidence separately
from signed composition, CM5 deployment, live phone acceptance and physical
acquisition. Generated fixtures are explicitly simulated; no offline test can
qualify a car's moving-time policy, reset persistence or heading accuracy.
The observation path is not deployed and the richer bindings have not replaced
the regular MapsMedia bench profile. Native Trip selection on the physical
cluster, physical acquisition/persistence and live phone content remain open.
CM5 availability and current deployment belong in [HANDOFF](HANDOFF.md).

[Observation cache](VEHICLE_OBSERVATIONS.md) ·
[Native pages](NATIVE_CLUSTER_PAGES.md) · [Cluster completeness](CLUSTER_COMPLETENESS.md) ·
[Adapter plan](VEHICLE_ADAPTER_PLAN.md).

## Software drive statistics — source-only, 14 October 2026

The opt-in existing sport acquisition owner now exposes **`status.sport.trip`**.
`TripStatistics` in `tools/sport_telemetry.py` consumes only the speed observations
atomically admitted by `SportEngine`; `SportSource` retains socket, clock, lock,
worker and retirement ownership. There is no new consumer endpoint or thread.
This works without a phone when the existing `sport_telemetry` configuration is
enabled. Disabled sport remains `{"configured":false}` with no trip model.
Simulation uses the existing dedicated sport fixture and stays labelled
`simulation`; it is distinct from the ordinary simulated gauge demo. Both
explicit saved simulation choices (`simulate_vehicle` or the mutually exclusive
`simulate_dated_vehicle`) preserve this existing opt-in sport/Trip source. The
Manager constructs the same `SportSource`; native and sport acquisitions,
source starts/generations and reset authority remain independent. Sport disabled
still creates no worker; neither choice supplies physical TripState or copies
native acquisition dates into sport statistics.

These are **software speed estimates**, separate from acquired `TripState`.
The native Trip page and signed bindings still display provider-supplied values.
Nothing publishes these estimates as physical trip readings, changes an odometer,
requests an actuator or uses phone navigation/media metadata. Odometer deltas,
GPS journey distance, ignition-based journey detection and fuel/economy are explicitly parked.
Optional durable archives are described below. Their source/calibration, independent
continuity or persistence contracts need qualification before adoption. GPS and
odometer updates cannot refresh speed or complete a missing speed interval.

### Arithmetic and coverage

For successive fresh speed acquisitions in one generation and origin, integrate
linear speed with trapezoids: `(v0 + v1) / 2 × acquired_seconds / 3600` kilometres.
Admission accepts speed only in the existing finite **0–300 km/h** range. The
interval must be positive, at most **250 ms**, and strictly less than both
immutable age limits. Exact age expiry cannot count as continuity. A missing,
stale, disconnected or terminally retired input clears the previous segment;
the next fresh acquisition supplies a baseline. A heartbeat or repaint cannot
add distance, duration, a peak or freshness. Rejected atomic packets cannot
change software statistics or consume sequence.

`coveredSeconds` includes stops only inside those accepted segments. It is not
wall-clock journey duration. `acquiredSpanSeconds` is first-to-last fresh acquired
time; `uncoveredSeconds` is that span minus covered time. The model does not
estimate distance during gaps or extend span to a poll/action date. A gap or
withdrawal sets `partial`; historical totals remain readable with the source
label and waiting phase. Even full sample coverage is an interpolation estimate,
not physical timing/calibration qualification. A single fresh sample can supply
a dated peak and zero acquired span, but distance/time/averages stay null until
an accepted interval. Two stopped samples supply known zero distance/moving time;
a moving average remains unavailable until positive moving time exists.

Moving means linearly interpolated speed **strictly above 0.5 km/h**. Integrate
only that portion of each accepted segment into moving time and moving distance;
`movingAverageSpeedKmh` divides that moving distance by moving time. The covered
average divides whole covered distance by covered time. Both rising and falling
threshold crossings use the original acquisition interval; no polling date or
invented sample is inserted. Peak speed records the earliest acquisition attaining
the maximum, with its origin.

A source-generation or speed-origin change archives the old journey and starts
a separate baseline; simulated and measured values never share one total. The
pure admitted-owner contract permits a fresh baseline older than the preceding
source's last acquisition after its watermarks reset. The wire adapter also
retains its stricter generation-start/publication ordering. No cross-generation
segment or date comparison is used for accumulation. The software reset cutoff
survives source changes. Within each source, existing acquisition/conflict/age
rules remain authoritative.

### Data and action interface for native UI integration

The trip-model change itself introduced no GTK or signed inputs. A separate
source-only [native leaf](NATIVE_HOME.md#trip-sport-and-history-leaf--source-only-14-october)
now presents these estimates/coverage/history with existing Home2 components
and the existing manager action. native UI reviews its new pieces; deployment,
physical providers and durable history remain separate. Action ID:

```json
{"action":"trip_reset"}
```

The existing manager route returns `{"sport":MODEL}`; the new model is its `trip`
member. The action accepts exactly `action`, rejects when sport is disabled,
archives only software statistics and clears their baseline/peak/counters.
Only acquisitions **strictly after the actual reset action date** may enter the
new journey; queued pre-reset samples and cached input cannot restore it.
`inputs.speed_kmh` may still display its independently fresh current observation.
`sport_session` continues resetting sport timers/laps/g-peaks independently and
does not reset software trip. Neither action changes provider TripState.

| Key under `sport.trip` | Unit / authority |
| --- | --- |
| `version`, `authority`, `method` | `1`, `software-speed-estimate`, `linear-speed-trapezoids` |
| `speedMaximumAgeNs` | Original admitted speed age policy in ns, or null before acquisition |
| `journeyId`, `sourceGeneration`, `origin` | Process-local software identity, provider generation, `measured` / `calculated` / `simulation` or null |
| `firstAcquiredNs`, `lastAcquiredNs` | Original fresh speed acquisition dates or null; host steady domain, never UTC |
| `distanceKm`, `movingDistanceKm` | Estimated covered whole / moving distance in km; null without a covered interval |
| `coveredSeconds`, `movingSeconds` | Acquired covered / moving duration in seconds; null without a covered interval |
| `speedBands` | Live-only fixed five rows `{lowerKmh,upperKmh,seconds}`; canonical km/h bounds, seconds null until covered, final upper bound null means unbounded |
| `averageSpeedKmh`, `movingAverageSpeedKmh` | Covered / moving mean in km/h; null without its positive denominator |
| `peak` | Null or `{speedKmh,acquiredNs,origin}` from one original fresh sample |
| `acquiredSpanSeconds`, `uncoveredSeconds`, `coverageRatio` | Acquired first-to-last span, uncovered span in seconds, covered/span fraction; fraction null for zero/unknown span |
| `partial`, `phase`, `reason`, `limitReached` | Gap/withdrawal flag; `waiting`, `collecting` or `limited`; explanatory reason and capacity flag |
| `maximumGapNs`, `movingThresholdKmh` | Fixed 250000000 ns and 0.5 km/h policies |
| `historyLimit`, `persistence`, `history` | 16, `process-memory`, last 16 archived statistics with `endedReason` and actual host action/transition `archivedAtNs` |

Archive dates are action/transition observations, not acquired journey endpoints.
History retains original first/last/peak dates. By default a restart restores no software
trip values/history; optional archived history below restores no live values.
Existing persisted sport history keeps its previous schema.
The current journey has fixed scalar state and one previous sample; no speed trace
or unbounded event queue exists. Covered distance has a 9,999,999 km ceiling;
covered duration has a 315,576,000 second ceiling. A larger acquired span also
stops accumulation, preserving actual dates rather than fabricating a capped
endpoint. A would-exceed segment is not added; `limited` retains the covered
statistics and requires software reset. Historical identity/dates are
process-local, and comparisons across reboot are not supported. Render null as
unavailable and always show estimate, coverage and simulation labels.

### Live speed bands — 14 October source

`status.sport.trip.speedBands` adds exactly five ordered read-only rows to the
live journey. Each row has `lowerKmh`, `upperKmh`, `seconds` and the live-only
`distanceKm` described below:

| Index | Canonical km/h range |
| --- | --- |
| 0 | [0, 30) |
| 1 | [30, 60) |
| 2 | [60, 90) |
| 3 | [90, 120) |
| 4 | [120, infinity); `upperKmh` is null |

These fixed ranges are software classifier definitions, never fitted speed
limits, driver scores or car properties. Render them with the parent
`authority: software-speed-estimate`, `journeyId`, `sourceGeneration`, `origin`,
original `firstAcquiredNs`/`lastAcquiredNs`, `coveredSeconds`, `coverageRatio`,
`partial` and `phase`. There is no separate acquisition or freshness clock.
The admission ceiling remains 300 km/h; the final classifier's unbounded
definition does not relax that input guard. Bounds are lower-inclusive and
upper-exclusive; touching an edge at one endpoint adds no duration at that edge.

Every `seconds` is null before an accepted covered interval, including a single
fresh sample. After coverage becomes known, unvisited bands have genuine zero
seconds. Constant stopped speed belongs to the first band. A constant edge speed
belongs to the band beginning at that edge. For a rising or falling linear
segment, each bin receives its overlapped speed width divided by the whole
segment's speed width, times the original acquisition delta. The calculation
runs only inside the existing continuous segment block after its capacity
guards. Summed band seconds match `coveredSeconds` within floating-point
tolerance; no gap, poll date or invented boundary acquisition is inserted.

All existing missing/stale/exact-age, replay/atomic-rejection, gap, provenance,
generation, reset-cutoff, limit and retirement rules apply unchanged. Partial
journeys retain only their known covered durations. New journeys clear the
five counters with the existing `_clear()`. `sport_session` remains independent.
No new action, control ID, UI piece, GTK page/style or signed input is added.

The distribution is **live-journey-only**, even when private archives are
enabled. It is outside `_statistics()`, every 16-entry trip archive and the
64-event sport history, and is never persisted, exported or reconstructed.
Strict archive keys/schema/encoding, storage restore/export and reset cutoffs
remain unchanged from the accepted archive implementation. A restart restores
historical records only; all five live durations remain null until new coverage.

Focused checks use the production engine, real SportSource socket/worker,
Manager copies and supervised private storage. They cover constant and adjacent
representable edge speeds, analytic rising/falling multi-edge ramps, reversal,
known zero, delayed acquisitions, polling invariance, exact expiry/withdrawal,
older new-generation baselines, reset cutoffs, atomic rejection, limits, bounded
history, death/join and archive/export/restart compatibility. A separate retained
comparison checks 200 original whole status/archive snapshots byte-for-byte
against the frozen pre-band source after removing only `trip.speedBands`.
Final focused CTest harnesses pass 10/10 on macOS (195 Python cases pass,
two explicit Linux-kernel/root-only skips) and Debian ARM64 (197 pass, no skips),
including all 16 band cases and the two relevant release-inventory cases.
Pinned clang-format 19.1.7 and whitespace checks pass. The first broad Mac
release fixture lacked its required C++ display checker; its failure is retained
separately from the scoped inventory checks. Two initial band-fixture mistakes
(encoding an already encoded export and exceeding a test-only distance ceiling
on reset recovery) are corrected without production changes or waived guards.
Current Docker backend INT/TERM/HUP, each with three repeated cleanup signals,
stops its own container, reaps its client and verifies absence. Source/logs/
Testing/JSON/hash manifests are serialized and reread before each own deletion.
The source-only qualification and failed fixture attempts are retained outside
Git under `20261014/codex-trip-speed-bands/`; these are simulated portable/Linux
Python component checks, without C++ sanitizer, GPU, phone or physical claims.

This model is still unpublished. After root's whole combined review, the sole
phone-session owner must qualify a new coherent source and every required
release lane, actual GTK integration, matching artifacts/helper/modules/config/
argv/units/inputs/assets and fresh capacity/full reserve before publishing a
new immutable generation. On that candidate, use the existing sport opt-in and
labelled fixture or qualified isolated source; poll `status` at 5 Hz for 30
seconds, retaining sport JSON. Check the fixed rows and labels, sum versus
covered time, original acquisitions, known zero and gaps; repeated status must
not add time. Send the existing `trip_reset` once, require all five null and a
new first acquisition strictly after its original reply `sport.observedNs`.
The archived record must have no band field. With private storage enabled,
verify existing archive/export/restore and unavailable new live distribution
on an isolated no-source restart, plus actual owner retirement/reap. Never inject
fixtures into selected vehicle input. Retain both panels/streams/UI-Sync/audio/
control, main touch, Home/Back, Climate/settings, native formats/fallback and
unchanged owner/restart observations. Physical calibration and native UI's later
presentation remain separate qualifications; this worker performs no bench work.

### Live distance per speed band — 15 October source

`status.sport.trip.speedBands[*].distanceKm` is a finite nonnegative software
distance estimate in canonical kilometres, independent of display units. All
five rows are null until an existing covered acquisition interval is accepted.
After that interval, unvisited bands have known zero distance; two genuine
stopped acquisitions establish known zero in every band. A single sample,
missing speed, repeated poll or stale redraw establishes no covered distance.

Each original continuous linear speed segment is partitioned at the existing
30/60/90/120 km/h thresholds. For each positive covered fraction `f` of that
segment, the band adds `(clippedStartSpeed + clippedEndSpeed) / 2 * seconds *
f / 3600`. The clipped endpoints belong to the original interpolation; they
are not new acquisitions. Rising and falling segments give the same integral.
For example, 0→150 km/h over 200 ms spends 40 ms in every band, but its band
distances are respectively `15/45/75/105/135 * .04 / 3600` km. Allocating the
whole distance in proportion to band time would give the wrong result.

Five fixed binary64 accumulators are cleared by the existing journey reset.
Their sum agrees with the unchanged whole covered distance within numerical
rounding; it is never forced to that total. Tests use an explicit absolute/
relative tolerance `1e-12 * max(1, expectedKm)`. Tiny distances may round to
zero; decimal digits establish no physical accuracy. The public 0–300 km/h
admission guard, original interval/age/gap eligibility, capacity checks, and all
old distance/time/moments/stationary/band-seconds arithmetic remain unchanged.
No interval crosses a gap, source generation, origin change, reset or retirement.
Loss retains covered estimates under the parent's waiting/partial authority;
it cannot extend coverage. Parent journey identity, original acquisitions,
generation, provenance, coverage, phase and limits remain authoritative.

This row field stays outside `_statistics()`, every historical journey, sport
history, durable schema, export and restore. Restart restores archives only;
new live band distances start null. There is no new action, ID, UI piece, GTK
widget/page/style, signed input, provider, clock, thread or persistence owner.
It implies no engine idle, fuel use, physical distance or calibration claim.

`tests/trip_speed_band_distance_test.py` covers analytic ramps and reversals,
constant thresholds, additivity, tiny and adjacent high-speed values, rational
integral references, null/zero, rejection, limits and original owner retirement.
Actual SportSource/Manager status copies and private history/export/restore/reap
fixtures exercise the existing paths. Retained 600-acquisition comparisons
against the exact accepted dependency source mask only this new key in the five
live rows; all old status and archive bytes must match exactly. Original Trip,
band, moments, stationary, history/storage/export, Sport and Manager assertions
remain intact. Source/report/Testing/signal/retention receipts live outside Git
under `20261014/codex-trip-speed-band-distance/`; qualification is simulated
Mac/Debian Python component evidence, without C++ sanitizer or GPU claims.
The scoped CTest harness passes 13/13 on both platforms: macOS discovers 244
Python cases, with 242 passing and two existing Linux-kernel/root-account skips;
Debian ARM64 passes all 244, including all 14 new cases. Both platforms retain
the exact 600-acquisition status/archive comparison. Current-backend actual
Docker INT/TERM/HUP plus mixed repeats preserve absent/partial reports, stop the
container and reap the client; these are cleanup proofs, not component passes.

Combined forward source `61b523f` also passes the exact fresh 600-acquisition
comparison against `e33cb90` on Mac and GCC while running all seven manager
suites. Both whole outputs are byte-identical across platforms; only the five
new live `distanceKm` keys are masked. Original statistics and archives remain
exact. This is simulated Python evidence, outside the C++ sanitizer lane.
[Combined scope and release gates](HANDOFF.md).

This model remains unpublished. Root whole-source review precedes folding;
the sole phone-session owner needs a new coherent complete required release
matrix, actual GTK integration, matching helper/modules/config/argv/units/
inputs/assets and fresh full capacity/reserve before immutable publication.
The exact future owner recipe is retained outside Git in `BENCH_PLAN.md`.
native UI presentation and physical provider/accuracy acceptance remain separate.

### Live speed distribution summary — 14 October source

Two read-only fields extend the existing `status.sport.trip` live journey:

| Key | Canonical unit / meaning |
| --- | --- |
| `timeWeightedRmsSpeedKmh` | km/h; square root of the time-weighted second speed moment over covered linear intervals |
| `speedStandardDeviationKmh` | km/h; population standard deviation over exactly those intervals, including stops |

Both are null until at least one existing covered acquisition interval has been
accepted. A valid constant speed has genuine zero deviation; a known stopped
interval has both values zero. These are software-speed-estimate context, never
a driving score, speed limit, comfort/safety metric, fitted car property or
measured consumption. No action, control ID, GTK change, widget, page, layout,
style, signed input, acquisition/provider or new UI piece is added.

Use the existing parent `authority`, `journeyId`, `sourceGeneration`, `origin`,
original first/last acquisitions, speed age policy, `coveredSeconds`, coverage,
`partial` and `phase`. There is no new clock, date or freshness authority. The
same missing/stale/exact-age, replay/atomic-rejection, gaps, generation/origin,
reset-cutoff, capacity and owner-retirement rules apply. Source loss retains the
covered aggregate with its existing waiting/partial context; it never extends
the covered period. New journeys clear the summary, and `sport_session` remains
independent. The original finite 0–300 km/h admission guard is unchanged.

For a covered interval of original duration `w` and endpoint speeds `a,b`, the
linear trajectory has mean `(a+b)/2` and within-interval population variance
`(a-b)^2/12`. The implementation merges weighted central moments after the
existing duration/distance guards. For prior duration `W`, prior centered sum
`M2`, prior mean `m` and segment mean `s`, it adds
`w * (segmentVariance + (s-m)^2 * W/(W+w))` to `M2` and updates the mean by
`(s-m) * w/(W+w)`. Deviation is `sqrt(M2/(W+w))`; RMS is computed with `hypot`
from the resulting mean and deviation. No point-sample averaging or subtraction
of nearly equal raw squared totals is used, and no result is silently clamped.

Three fixed scalars retain one anchor speed, a mean offset from that
anchor and `M2` in `(km/h)^2 * ns`. Subtracting the fixed first covered endpoint
before forming segment means retains tiny differences near 300 km/h. The
existing covered duration stays an integer; the new arithmetic uses binary64
floats. The unchanged 315,576,000 s ceiling and 0–300 km/h input guard bound a
conservative `M2` magnitude below `2.85e22`, far inside finite binary64 range.
There is no trace, extra queue or growing sample collection. Nonnegative within-
and between-segment contributions avoid a spurious negative variance.

JSON uses the ordinary shortest round-tripping decimal representation of those
floats, with no extra decimal quantization. The internal shifted mean can retain
a fractional-ULP offset that the recomposed RMS cannot represent: adjacent speeds
near 300 km/h can have positive tiny deviation while the RMS rounds to 300.
Extremely small squared differences can underflow to zero in binary64; these
remain known rounded estimates rather than unavailable data. Integer nanosecond
weights and printed decimal digits do not establish sensor, physical timing or
calibration precision. The existing distance/average and
speed-band arithmetic and returned bytes are unchanged.

These fields are **live-only**, outside `_statistics()`, every 16-trip archive,
64-sport history, private file and export. Nothing persists, restores or
reconstructs them. A fresh process starts both null even when historical records
restore. Archive keys/schema/bytes, reset/archive/restore, private writer,
acknowledgement/error/sync and actual reap semantics remain unchanged.

Focused ordinary-Python tests in `tests/trip_speed_summary_test.py` cover
analytical constants/stops/ramps, uneven acquisition deltas, split invariance,
near-300 adjacent floats and a high-precision independent integral reference.
They also use the actual SportSource worker/socket, Manager immutable copies,
private writer/export/restore and reap boundaries. A retained comparison checks
600 whole statuses and archive snapshots against imported production, omitting
only the two new live fields. Existing band/archive/owner tests remain intact.
Source identities, exact counts/skips, failed attempts, original Testing/JUnit,
signal/cleanup and hash receipts stay outside Git under
`20261014/codex-trip-speed-summary/` in the qualification cache. This is simulated
portable/Linux component evidence; Python is not described as sanitized C++.
No GPU, CM5, phone, physical or fresh60 acceptance follows.

The model remains unpublished. Root must review the final whole source before
folding; phone-session alone may then qualify a NEW coherent complete release
matrix, actual GTK/Trip, matching artifacts/helper/module/config/argv/units/
inputs/assets and fresh full capacity/reserve before a new immutable generation.
On that future candidate poll existing status at 5 Hz for 30 s, retain original
parent metadata/covered intervals, and check null/zero/constant/ramped/gap cases
without adding polling time. Use only a separate isolated source for fixtures.
Use existing `trip_reset` once and require both null plus a strictly post-action
new acquisition; no new action is added. An isolated no-source restart with
private history must restore archives only, keep these live fields null and
actually reap the retired storage owner. Preserve both panels/streams/UI-Sync/
audio/control, main/touch/Home/Back/Climate/settings, cluster and fallback. The
exact future sole-owner recipe is retained as `BENCH_PLAN.md` outside Git.
### Recent pace — source-only, 15 October 2026

The additive live **`status.sport.trip.recent`** block estimates distance and
mean speed over a fixed nominal 60-second window. It consumes only intervals
already accepted by the unchanged Trip integrator, including its original
speed, generation, origin, freshness, maximum-gap and capacity decisions.
It adds no provider, configuration, action, ID or UI piece. native UI owns visual
adoption. This software estimate is neither a vehicle odometer nor current GPS,
fuel, physical calibration or an applied car state.

`RecentPace` is a separate live accumulator with at most **4,096 intervals**.
Each retains both original dated observations and their age/origin; the parent
journey supplies the generation. Whole-journey arithmetic, moving threshold,
timers/g-meter, reset policy, history, persistence, export and archive schemas
are unchanged. The recent block never enters `_statistics()` or history.

The anchor is the latest original speed acquisition admitted to the current
Trip journey, including a fresh baseline without coverage. Status reads, stale
input, publications and redraws cannot move it. Intervals ending at or before
anchor minus 60 seconds retire. The first remaining piecewise-linear interval
is clipped analytically at that cutoff: the cutoff is a derived integration
boundary, never a newly acquired reading. Missing/expired data and excessive
gaps contribute no interval; there is no bridge across withdrawal. Source
origin/generation transitions and software trip reset clear the recent window
with the existing journey policy. Reset still excludes queued acquisitions at
or before the action date. Sport-session reset remains independent. Owner
retirement retains historical estimates and withdraws current availability.

| Key under `sport.trip.recent` | Meaning |
| --- | --- |
| `version`, `authority`, `method` | `1`, `software-speed-estimate`, `linear-speed-trapezoids` |
| `nominalWindowSeconds`, `intervalCapacity`, `retainedIntervals` | Fixed `60`, fixed `4096`, current bounded interval count |
| `sourceGeneration`, `origin` | Current journey provenance, or null before admission |
| `anchorAcquiredNs` | Latest original admitted speed acquisition, or null |
| `derivedWindowStartNs`, `derivedWindowEndNs` | Nominal cutoff (bounded below by host epoch zero) and anchor, or null; these describe integration bounds, not new acquisitions |
| `firstContributingAcquiredNs`, `lastContributingAcquiredNs` | Original endpoints of contributing retained intervals, or null; the first may precede the derived cutoff |
| `distanceKm`, `coveredSeconds`, `averageSpeedKmh` | Clipped covered distance, accepted duration and time-weighted mean; all null until a positive covered interval; genuine stopped coverage yields zero distance and mean |
| `observedSpanSeconds`, `uncoveredSeconds` | Lesser of 60 seconds and original first-to-anchor span, and that span minus retained covered time; null without an anchor |
| `coverageRatio` | Covered duration / nominal 60 seconds, null without an anchor; a first baseline has zero coverage |
| `partial`, `limited` | Coverage below the nominal window; capacity loss still intersecting the window |
| `currentInputAvailable` | Independently fresh current speed input, even when a post-reset baseline is not yet eligible; never freshness authority for retained estimates |

If more than 4,096 intervals fit inside the window, the oldest are discarded
and `limited`/`partial` remain true while the lost portion intersects it. Covered
time and distance describe only retained intervals; observed span does not
shrink to disguise loss. Once original new acquisitions move the cutoff beyond
all lost intervals, `limited` clears; gaps/short coverage still keep `partial`.
An unavailable current input can coexist with retained full historical coverage.
The old whole-journey guard still stops unaccepted intervals before the recent
accumulator. No wall-clock duration, readiness, frame rate or hardware claim
follows from any field. Numbers are finite, canonical km/km/h/seconds; dates
remain exact original host-monotonic integer nanoseconds within the existing
owner clock scope. No coordinates, phone identity or paths are added.

Focused checks use explicitly synthetic data and actual Manager/SportSource
publication, reset, socket ingress and retirement. Independent rational
polynomial integration checks clipping; a new deterministic 130-second timeline
contains 651 distinct original fixture acquisitions, duplicate publications,
expiry, generation and software resets. Baseline/final captures compare the
whole source-owner state and complete manager status after removing only the
new public recent block and the declared `TripStatistics.recent` accumulator;
all old journey fields and original history/archive bytes must match exactly.
Reports, failed attempts, source identities and retirement manifests stay outside
Git under `20261014/codex-trip-recent-pace/`. Python checks do not establish C++
sanitizer, GPU, GTK, deployment, real-time driving or physical acceptance.

For future adoption, root reviews the whole combined source first. The sole
phone-session owner then requires the coherent complete release matrix and exact
source/helper/module/config/role/argv/unit/input/asset bindings, native UI's GTK
qualification and fresh capacity/reserve before any new immutable generation.
This model remains unpublished. On that future candidate, use the existing
opt-in labelled simulation or qualified sport fan-out, without another owner:
retain at least 120 seconds of sport status at 5 Hz and compare distinct original
acquisitions with independent clipped interval integration, coverage and source
labels. Repeated polls must leave the anchor/statistics unchanged. In an isolated
synthetic source check stopped zero, silence/expiry, replay rejection, gaps,
origin/generation transitions and retirement; retained estimates must stay dated
and unavailable current input must remain explicit. Exercise the existing
`trip_reset` once, retaining its action date, unchanged sport session/provider
state and recent nulls until eligible post-action coverage. Old archived records
must contain no recent block. Preserve both panels, streams, UI-Sync, audio,
control, main touch/Home/Back, native fallback/formats and Climate/settings with
unchanged owners. These are future sole-owner checks, not actions or physical
claims by this source-only worker.

### Focused evidence and exact owner follow-up

Private source/receipts: `~/.cache/headunit-qualification/20261014/codex-trip-statistics/`.
The focused tests exercise the production SportEngine/SportSource/manager,
analytic constant/ramped speed and moving crossings, unavailable/stopped baseline,
original delayed dates, exact expiry, missing/long gaps, atomic later-field
rejection, generation/provenance transition, pre-reset queued acquisitions,
independent sport reset, bounded history/capacity and real private HVO sockets.
Worker ingress failure and consumer retirement withdraw immediately without
redating; a new instance restores no software measurements. Capacity tests use
small scoped test-only ceilings to reach the unchanged production guards.
Final focused checks pass **131/131** on macOS. Debian ARM64 runs the same
131 cases: **128 pass, 3 existing GdkPixbuf dependency cases skip** in the
phone-management suite; all 16 trip and 33 sport cases pass. The other suites
cover 36 phone-management, 15 recovery, 16 configuration and 15 endpoint cases.
Actual CMake registration/CTest passes **2/2** for trip and sport; retained
Testing/cache/log hashes verify before the own rebuildable tree retires. Pinned `tools/format.sh --check` passes with clang-format 19.1.7.
Actual own-container INT/TERM/HUP cleanup passes; logs/inspect/source hashes are
retained and containers retire after each lane. An initial Mac fixture used an
uncanonical `/var` socket parent and failed the unchanged private-parent guard;
the corrected canonical endpoint passes. A Git-free snapshot formatter invocation
fails its `git ls-files` prerequisite; the exact-source Git worktree format check
passes. Both failed logs remain retained. These checks are portable/Linux
simulated component evidence only; no full C++/GPU matrix is claimed.

The manager reviews the complete branch, resolves announced shared-file hunks and
runs the coherent required matrix before the sole phone-session owner publishes
any new immutable generation. No source-only check selects or edits V84.
For that future candidate:

1. Keep the existing sport opt-in, labelled simulation or already qualified
   dedicated sport fan-out source; no extra socket/config role is needed. Poll
   `{"action":"status"}` at 5 Hz for 30 seconds and retain only sport JSON.
   Compare unique original speed acquisitions and admitted continuity intervals
   with estimated distance/covered duration; repeated polls must not add time.
   Check known stopped intervals, dated peak, simulation label and partial gaps.
2. Send `{"action":"trip_reset"}` once and retain its `sport.observedNs` reply.
   Require cleared software values, archived previous journey, unchanged sport
   session/timers/laps and current provider state. New first acquisition must be
   strictly later than that reset date. Provider odometer/TripState stay unchanged.
3. On an isolated temporary source, repeat missing speed, exact silence expiry,
   rejected replay/invalid packet and new-generation recovery. Require waiting,
   historical partial totals, original dates and new baseline without gap bridging.
   Stop the consumer; require endpoint removal and immediate disconnected speed.
   Do not inject fixture packets into the currently selected live car input.
4. On the candidate observe both panels/streams/UI-Sync/audio/control, main touch,
   Home/Back, Climate/settings and unchanged owners/restarts while polling/resetting.
   Phone navigation/content must not create car measurements. native UI's later
   frontend integration and physical providers each need separate qualification.

Ignition/power, odometer/GNSS calibration and actual car trip acceptance await
qualified physical contracts/equipment; no camera/audio/fps acceptance is claimed.


### Live stationary-duration summaries — 15 October source

Two read-only fields extend `status.sport.trip`:

| Key | Canonical unit / meaning |
| --- | --- |
| `coveredStationarySeconds` | Seconds of covered linear speed at or below the existing 0.5 km/h moving threshold; complement of existing covered moving time |
| `longestContinuousStationarySeconds` | Longest contiguous covered portion at or below that threshold, including interpolated crossing portions |

Both are null until an existing covered acquisition interval is admitted. Two
genuine zero-speed acquisitions contribute duration; a covered journey entirely
above the threshold has both values zero. A single acquisition, missing speed,
poll, heartbeat or stale redraw adds no duration. These are covered low-speed
software estimates, not engine idle, ignition, physical stops, traffic cause,
fuel cost, a driving score or a safety diagnosis. They supply no stop count,
event list or current instantaneous idle claim.

The same positive original acquisition intervals, 250 ms maximum gap, strict
original age limits, source/reset floors, finite 0–300 km/h input range and
duration/distance bounds remain authoritative. A rising interval contributes
its low-speed prefix; a falling interval contributes its low-speed suffix. For
example, 0→2 km/h over an admitted 200 ms contributes 50 ms, as does its reverse.
The two low-speed portions of 0→2→0 are separate runs; those of 2→0→2 join at
the middle original acquisition. Threshold contact alone has zero duration;
a continuous plateau exactly at 0.5 km/h counts fully.

Any above-threshold portion breaks the current run. So do an acquisition gap,
exact expiry, unavailable/stale/disconnected input, capacity retirement and
owner withdrawal. Retained covered summaries stay readable with the original
waiting/partial context; the next fresh sample supplies only a baseline.
Factory state, source generation/origin transitions, software reset and orderly
private-owner archival clear both summaries with the original journey. The
existing post-reset cutoff and independent `sport_session` action are unchanged.
No interval bridges unknown time or different source authority.

`coveredStationarySeconds` is `(covered_ns - moving_ns) / 1e9`, preserving the
existing moving arithmetic exactly. Two additional fixed scalars retain current
and longest low-speed runs in nanosecond units. Falling suffixes use their
endpoint speed difference directly to avoid subtracting a crossing fraction
near one. All portions are binary64 linear estimates anchored to original
acquisitions, never new measured timestamps or clock receipts. Floating-point
rounding can make the independently accumulated longest run differ by tiny
rounding amounts from the complementary covered total; no physical timing or
sensor precision follows from printed digits. No sample array, new clock,
threshold setting, provider, model service or writer is added.

Both fields live outside `_statistics()`, the 16-trip archive, sport history,
private writer, export and restore, just like the existing live RMS/deviation.
Old archive/schema/export bytes and historical source authority remain unchanged.
A restart restores only old labelled archives and starts both new live values
null. Parent origin/generation, original first/last acquisition, immutable speed
age, coverage, phase and partial labels remain the context for these values.
No action, control ID, GTK page/widget/style, signed input or new UI piece is
added. native UI can later consume the existing copied Sport status contract.

`tests/trip_stationary_summary_test.py` uses analytical constants/crossings,
uneven and split intervals, an independent rational interval-union oracle,
threshold contacts, known zero, all-moving and unavailable baselines. It covers
atomic malformed/conflicting input and actual duplicate/replayed socket packets,
exact expiry/gaps/advance/recovery, original delayed dates, generation/origin,
reset floors and limits. Actual SportSource/Manager copied-status, worker-failure
retirement/join and private archive/export/restore/child-reap fixtures exercise
the existing owners. Retained comparisons against exact accepted `a235cca`
check 600 whole status/archive snapshots byte-for-byte, masking only these two
new live fields. Qualification and failure/retention receipts are outside Git
under `20261014/codex-trip-stationary-summary/`. These are simulated portable and
Debian Python component checks; no C++ sanitizer, GPU, phone or physical claim.

This model remains unpublished. Root must review the final whole branch before
folding. The sole phone-session owner must then qualify a NEW coherent complete
required release matrix, actual GTK integration, artifacts/helper/modules/roles/
config/argv/units/inputs/assets and fresh full capacity before publishing any new
immutable CM5 generation. On that candidate, keep the existing sport opt-in and
labelled fixture or qualified isolated source. Poll `status` at 5 Hz for 30 seconds;
compare unique original speed acquisitions and continuous covered intervals with
the complementary stationary/moving totals and longest low-speed run. Repeated
polls add nothing. Send the existing `trip_reset` once; require both values null,
no new fields in the archived record and a new baseline strictly after the reply's
original `sport.observedNs`. In an isolated private source, repeat exact expiry,
withdrawal, gap, rejected packet and generation recovery; require no run across
the break. With private storage enabled, verify archive/export/restore and actual
writer reap while the new live values start null on a no-source restart. Never
inject fixtures into selected vehicle input. Preserve both panels/streams/UI-Sync/
audio/control, main touch/Home/Back/Climate/settings, native formats/fallback and
owner/restart observations. native UI presentation and physical calibration remain
separate acceptance; no bench action is performed by this source worker.

## Private software journey archives — source-only, 14 October 2026

Receiver configuration **`trip_history: true`** requires **`sport_telemetry: true`**;
its default is false. The manager passes it to the existing SportSource. No GTK
page, widgets, styles, signed bindings or new vehicle subscriber are added. The
physical TripState/odometer and sport session/reset routes remain independent.
Only a new managed generation with its matching reviewed helper may opt in.
The hash-bound receiver config determines whether `tools/trip_history.py` enters
that generation's exact sealed inventory, alongside the existing sport modules.
Disabled and legacy inventories remain unchanged.

### Archive scope and restore

`stateDirectory/trip-history.json` stores at most **16** archived journeys in at
most **65,536 bytes**. A record is labelled `classification: HISTORICAL`, with
`authority: software-speed-estimate`, `method: linear-speed-trapezoids`, fixed
250 ms maximum gap and 0.5 km/h moving threshold, explicit units (km, s, km/h,
ns), a random software `ownerId`, and the actual OS `hostBootId` from Linux's
boot ID or macOS's boot-session UUID. `sourceClock` names `CLOCK_MONOTONIC` on
Linux and `CLOCK_MONOTONIC_RAW` on macOS. These are scope labels, never a clock
conversion or invented UTC/ignition date. `speedMaximumAgeNs` retains the
original admitted speed age policy. Each record retains the original source
generation/origin, first/last/peak acquisition dates, coverage, null statistics
and actual archive action/transition date. `(ownerId, journeyId)` scopes identity
across process restarts; journeyId alone remains process-local.

Restore is a copied history list alongside the engine. It never imports live
counters, acquisition watermarks, reset cutoffs, peaks, current generation,
previous samples or freshness. An archived date from the same or a different
boot stays historical and must not be compared to current acquisition or UI
observation time. A new live journey starts unknown and needs actual fresh speed.
Original archived records are retained unchanged, then the latest current-owner
archives evict oldest records at 16. Current-owner archive labels can have a null
boot ID if storage startup fails; those memory records cannot be written as a
qualified archive. No live source value becomes zero to accommodate a failure.

User `trip_reset` archives current software statistics and retains the existing
strict post-action acquisition cutoff. Source-generation and origin changes
archive separate journeys. Orderly SportSource retirement archives a known
current journey once with `endedReason: Sport owner retired`; repeated close does
not duplicate it. A process crash/power loss before archival can lose the current
journey. There is no automatic ignition journey, periodic live checkpoint,
provider trip reset, fuel/economy inference or crash-to-fresh restore.

### Storage and failure ownership

One child supervised by SportSource owns trip writes and, under this opt-in,
existing sport-history writes;
it owns no acquisition/socket/radio/display/VM/GPU. Existing sport-history file/schema and restore semantics remain unchanged;
its writer helper runs in the same child so synchronous legacy fsync cannot
delay acquisition or watchdog supervision. The default-off writer is unchanged. The acquisition worker uses nonblocking private
pipes with length-prefixed JSON (131,072-byte frames containing two individually
65,536-byte-bounded records), one inflight write and one
coalesced newest snapshot. It never waits for the child's disk work. No automatic
child restart, detached writer, event logger, unbounded queue or extra thread is
added. Only archive-list or existing sport-history/line changes submit snapshots;
the child skips unchanged file records after its initial write. Polling/current
samples alone do not write a trip trace. The files are independent atomic
records, not a multi-file transaction; acknowledgement follows all requested
changed-file syncs. A partial two-file failure is explicitly unavailable.

Startup and each inflight write have a **2-second** watchdog, measured with real
supervisor elapsed time only, unrelated to acquisition clocks. Expiry/error
latches storage failure and kills the child while acquisition continues. Close
allows at most the remaining fixed drain budget and then bounded child
EOF/TERM/KILL waits (250 ms each). `reaped` becomes true only after actual process
exit/wait; an uninterruptible process remains explicitly failed/unreaped and is
never automatically replaced. A terminal acquisition failure explicitly fails
and reaps storage too; its useful waiting memory statistics retain their original
dates, with no successful final-archive promise. Linux arms actual parent-death SIGKILL before and
after privilege drop, with the expected supervisor PID race check. Linux also
retires the child when its spawning thread exits; production starts it on the
existing long-lived manager thread. macOS uses
pipe EOF and explicit supervisor retirement. Storage subprocess privileges drop
to the configured display UID/GID before touching its files.

A service control-group stop signals both manager and storage child, even in
separate sessions. The spawning thread atomically blocks INT/TERM/HUP before
fork/exec and restores its complete original mask in `finally`, including
unrelated inherited bits. After parent/account admission and Linux parent-death
arming/rearming, only the child installs ignore dispositions and unblocks those
three signals. The parent still handles stop normally; EOF/bounded drain/kill/
actual reap owns child retirement. The existing manager handles TERM/INT
orderly; the source fixture installs an actual orderly parent HUP handler as well.
This repair leaves the shipped parent HUP disposition unchanged. No unsafe threaded `preexec_fn`, cgroup escape,
new owner, IPC budget or queue change is used. Healthy orderly close can attempt
and acknowledge the final current archive before writer exit; a failed/unfinished
save stays explicit. This adds no crash/power-loss durability guarantee.

The child admits a canonical, owned **0700** directory, pins its descriptor and
identity, and takes a nonblocking exclusive flock on `.trip-history.lock` (owned
**0600**, regular, single link). Reads reject symlinks, hard links, FIFOs, shared
mode/owner, replacement races, oversize files, duplicate fields, depth >8,
nonfinite/out-of-range numbers, incompatible versions, invalid scope/units,
coherence/date errors and duplicate journey identities. Acquired/peak speed keeps
the strict 0–300 km/h bound; derived means allow only the existing 1e-9 km/h
numerical comparison epsilon at that upper bound and retain their exact original
values. No number is clamped or rewritten to fit storage. The exact schema-1
envelope includes SHA-256 of canonical version/history JSON; it is a corruption
check, not authentication against the owning account. A new file is written to
one unique 0600 temporary, fsynced, atomically replaced under the pinned directory
and followed by directory fsync. Target/directory/lock changes reject the write.
Only the child's own temporary is cleaned during ordinary error handling; a
hard-killed write can leave one bounded private temporary. No other owner's
files or retained evidence are cleaned.

Corrupt/incompatible/shared storage is latched unavailable, left untouched and
never automatically overwritten. Disk failure also latches unavailable; the
current memory trip and newly archived memory history remain useful. Failed
fsync after rename can leave complete new bytes without a durable success
receipt. The model reports failure and does not invent one. No durable status
is inferred from an action response, a filename, mtime or successful repaint.

### Data interface for native UI integration

Existing actions/IDs remain `status` and `trip_reset`; replies remain
`{sport: MODEL}`. Existing `sport.trip` statistics and `history` are reused.
No new dispatcher whitelist, UI controls, pages, styles or widget framework is
added. native UI should reuse established Home2 components for any later display.
Additional data:

| Key | Meaning |
| --- | --- |
| `sport.trip.speedMaximumAgeNs` | Original live speed age policy, null before acquisition |
| `sport.trip.persistence` | `private-archives` when opted in; otherwise `process-memory` |
| `sport.trip.storage.configured`, `.state`, `.error` | True; `starting`, `ready`, `failed` or `retired`; sanitized storage error |
| `.hostBootId`, `.sourceClock`, `.ownerId` | Current storage boot/clock/software-owner scope, never dates |
| `.restoredCount` | Number of loaded archive records |
| `.pending`, `.revision`, `.savedRevision` | Dirty/coalesced/inflight work, submitted revision and last actual synced child acknowledgement; not acquisition sequence |
| `.reaped` | Actual child retirement observed, not inferred from a request |
| `sport.trip.history[].classification`, `.hostBootId`, `.sourceClock`, `.ownerId`, `.units`, `.authority`, `.method`, `.maximumGapNs`, `.movingThresholdKmh`, `.speedMaximumAgeNs` | Historical labels and exact original software acquisition policy/scope alongside existing archive statistics |

Render null as unavailable, retain simulation/estimate/partial labels, and never
show historical monotonic dates as a wall-clock calendar or current age.

### Focused qualification and future bench recipe

Production private-file/child/SportSource/model tests are registered as
`trip-history-python`. Focused retained receipts live outside Git under
`~/.cache/headunit-qualification/20261014/codex-trip-history/`. Source-only
macOS discovers **194** focused Python cases: **192 pass**, with two explicit
Linux-parent-death/root-account skips. Linux ARM64 discovers the same **194**:
**191 pass**, with three existing GdkPixbuf dependency skips; all **24** trip
archive cases execute/pass, including actual stopped-child kernel parent-death
SIGKILL after display UID/GID drop/rearm and both files' private ownership.
Registered Mac CTest passes **9/9**, including **68** complete release checks
and optional module/config/hash closure, with a real built portable display
checker. Pinned `tools/format.sh --check` passes with clang-format 19.1.7.

The retained original `mac-shutdown-baseline` reproduces loss of the final
current journey for each actual TERM/INT/HUP delivered to both stopped storage
child and parent. The parent actual signal handler requests orderly close;
production consumes the child's real wait status, and absence is checked
separately. After repair each scenario persists the final journey with original
dates/distance and child exit zero. Startup holds the actual spawned child before
worker admission, proves all three signals pending, then exercises the production
worker's ignore/unblock and write. Future signals, unchanged caller-thread and
main-thread mask bits/parent handlers, inherited descriptor closure, actual failed-exec mask/descriptor
restoration and actual pipe closure are also covered. The fixture models service
control-group delivery with actual OS signals to both owned processes; its parent
handler performs production `SportSource.close()` and `HistoryClient.close()`.
The observer records the OS wait status consumed by production retirement.
The retained failed `linux-shutdown-reviewed-final` exposed premature retirement
of the test spawning thread; the final fixture keeps its actual owner alive
until child close/reap, then explicitly releases and joins it. Production
parent-death protection and all watchdogs remain unchanged.

Actual private-file/source/manager tests cover restart/other boot, original
metadata/units and unknown new baseline, reset cutoff and independent sport
session/line restore, gap/known-zero distinction, 16-record eviction, malformed
schema/hash/number/depth/size, no-follow/shared-owner refusal, reader/directory/
lock/target replacement, short writes, file/directory sync failure, stopped
storage with advancing acquisition, terminal acquisition and orderly retirement.
One valid maximum-speed archive initially failed the new derived-mean bound;
the original failed source/log and analytic reproduction are retained. Corrected
round-trip preserves original floating values, with the stated 1e-9 comparison
epsilon; no acquired speed, date or arithmetic is changed.

Actual own-container INT/TERM/HUP cleanup passes (130/143/129). All source,
Testing, logs, failed attempts and JSON receipts are retained/hash-verified
outside the build before each owned Mac tree/container retires. Refreshed
shutdown signal proofs and per-lane retirement manifests are retained separately
from the original branch receipts. Shared
caches/other workers/private inputs are untouched. No full C++/Skia matrix is
claimed; manager whole-branch review and a coherent required matrix remain
mandatory before admission. No phone, GPU, car, camera/audio/fps or deployment
acceptance follows.

After manager whole-branch review and its coherent required matrix, the sole
phone-session owner may build a **new** helper/generation from its current
integration source. Set `sport_telemetry: true` and `trip_history: true` in a new
hash-bound private receiver input. Verify the optional `trip_history.py` inventory
and compatible helper/config closure before admission; keep frozen V85 intact.
On that candidate:

1. Poll `status.sport.trip` at 5 Hz for 30 seconds with the existing labelled sport
   simulation or qualified isolated source. Wait for storage ready. Check unique
   original speed acquisitions/coverage; status polls must not accumulate or
   write samples. Send one `trip_reset`, require one HISTORICAL archive and a new
   first acquisition strictly after the reply's original `sport.observedNs`.
   Sport timers/session and provider TripState/odometer must remain unchanged.
2. Require `pending: false`, `savedRevision == revision` and a positive actual
   save revision. Schedule one normal manager service restart using the existing
   sole owner. Require the final known current journey to appear once in history
   after the healthy writer drain; failed/unfinished saves stay explicit.
   Require copied historical records with unchanged original dates/units/origin/
   coverage/boot scope, a new ownerId, and no restored live baseline or peak.
   Simulation may already supply new samples by the first status poll: their
   first acquisition must belong to the new process; inspect the no-source
   isolated component separately to prove unknown startup.
3. In an isolated private fixture only, restart with another original boot scope,
   corrupt/hash-incompatible/symlink/shared storage, a failed write and a stopped
   storage child. Require history-only restore or explicit failed storage,
   preserved useful current trip, advancing acquisitions during blocked storage,
   and actual child reap/endpoint retirement. Run the deterministic stopped-child
   normal service-group TERM/INT/HUP fixture plus startup mask, inherited FD and
   parent-death/UID-drop regressions against the exact sealed candidate modules;
   retain actual wait status separately from PID absence. Never inject/corrupt
   the candidate's selected live state or private phone inputs to run these fixtures.
4. On the connected candidate retain both panels/streams/UI-Sync/audio/control,
   main touch/Home/Back/Climate/settings, unchanged resource owners and restart
   counts. native UI's UI adoption and physical calibration remain separate checks.

No claim of durable current-journey recovery after abrupt power loss is made.

### Actual isolated ENOSPC — 14 October, source only

`tests/trip_storage_enospc_test.py` exercises the accepted `f271674` storage
owner on the qualified `3670763` integration foundation. The unchanged runtime
passes the production baseline; this work adds coverage, without a storage
repair or automatic recovery claim. The Linux fixture requires an empty,
explicit **64 KiB–1 MiB tmpfs mount** and root supervision of the real
unprivileged writer. It refuses ordinary filesystems before filling anything.
Qualification uses an owned container with a **128 KiB** tmpfs; no host volume,
selected state, physical car or phone input is filled or changed.

After one actual reset/save acknowledgement, the fixture stops only the writer,
exhausts that tmpfs and requests another software reset. The UID/GID-dropped
production child reports sanitized storage failure and is actually reaped.
An independent `PrivateStore.save` on the same exhausted mount returns actual
`ENOSPC` (28); the public child reply does not expose a syscall errno. The prior
trip/sport files retain exact bytes, modes, ownership and inodes, and ordinary
error handling removes its own temporary. `savedRevision` stays at the prior
acknowledgement; failed work leaves no pending queue, replacement writer or late
success receipt.

The **same real SportSource worker** keeps fresh original speed acquisitions,
clock/provenance, moving distance and covered duration flowing during failure.
Freeing tmpfs capacity leaves storage failed; it does not retry or manufacture an
acknowledgement. Close joins acquisition, withdraws readings, reaps storage and
retains unsaved reset/retirement history only in memory. A separately started,
no-source instance restores only the acknowledged `HISTORICAL` record, preserving
its boot/clock/owner/date scope while live values, peak and acquisition/reset
watermarks remain unknown/empty. The exclusive lock retires with the old child.

This qualifies a **pre-rename capacity failure**. Actual EIO devices, crash or
power-loss durability, two-file partial commits and post-rename directory-sync
failure are separate boundaries. Complete committed file bytes without an
acknowledgement can be crash-unknown; filename presence never establishes a save
receipt. A validated record loaded later is historical, never a current baseline.
The existing short-write, sync, corruption, replacement, stopped-writer,
service-group signal and Linux parent-death tests retain their stated scopes.

The default suite never fills a filesystem. Inside an isolated root-run Linux
container, explicitly mount `/trip-fault` with
`--tmpfs /trip-fault:rw,size=128k,mode=0755`, then either run the script with
`--tmpfs-root /trip-fault --report /reports/enospc.json`, or configure
`HEADUNIT_TRIP_ENOSPC_TMPFS=/trip-fault` and select CTest `trip-storage-enospc`.
The CMake option is qualification-only and empty by default. Use the host build
slot at its unchanged 15 GiB floor; retain reports/source/logs/Testing before
retiring the owned build and stopping/removing the container, including on
INT/TERM/HUP. Do not point this fixture at appliance state.

Retained component evidence is under
`~/.cache/headunit-qualification/20261014/codex-trip-storage-faults/`.
The source-only Mac closure passes five registered CTests (183 Python cases:
181 pass, two explicit Linux/root-only skips), including the real portable display
checker and combined trip/native-publication release inventory, in **19.43 s**.
Debian ARM64 default GCC passes **six CTests in 15.94 s**: all 183 Python cases
execute/pass, plus the actual ENOSPC fixture (**2.64 s**). Actual child UID drop,
parent-death, soft signals, descriptor/mask restoration and retirement are
included. Both lanes preserve normal compiler/warning/test policies, with no
CTest retry or skipped CTest. Build/Testing/source/log/JSON hashes verify before
the two owned build trees retire. Final own-container INT/TERM/HUP cleanup
passes with runner exits 130/143/129 and actual stop/removal/client reap.
Earlier runner failures and incorrectly labelled intermediate signal summaries
remain retained and explicitly excluded. No full
C++/Skia matrix, physical storage/car, GTK, deployment, sustained phone session,
camera/audio or fresh-frame acceptance follows.

For native UI: **new UI keys, actions, control IDs, widgets, pages and styles: none**.
Existing `status.sport.trip.storage` fields, `trip_reset` and `sport_session`
remain unchanged. Whole combined manager review, coherent required matrix and
new helper/config/module/unit/input/capacity qualification precede any sole
phone-session adoption. Preserve the dated-simulation Sport source choice and
ambient/native-status owners when root resolves shared integration hunks.

## Offline private trip-history export — source-only, 14 October 2026

`tools/trip_history_export.py` exports one explicitly selected software history
archive. It is an offline source tool, with no manager action/status key, IPC,
service, provider, acquisition, GTK change or release inventory entry. Its
accepted private-history dependency remains separately opt-in and unpublished
until whole-source release qualification. This export does not restore live
counters, reset cutoffs, peaks, acquisition baselines or freshness.

```sh
python3 -B -E -s tools/trip_history_export.py \
  --input /private/owned-state/trip-history.json \
  --output-directory /private/owned-exports/new-export
```

Both paths must be absolute, canonical and free of symlink parents. Input and
output parents must already be owned by the executing UID/GID with mode 0700.
The input must be a regular 0600 file of that UID/GID with exactly one link,
containing 1–65,536 bytes and at most 16 records. The tool pins directory/file
descriptors, checks original device/inode/owner/group/mode/link/size/mtime/ctime,
compares two bounded reads, and rechecks identity through publication. It never
creates the input storage lock or changes the input file. A concurrent healthy
archive replacement makes this attempt fail; obtain a stable explicit archive
and choose another new output directory rather than overwriting a result.

`trip_history.restore` is the sole strict schema, checksum and arithmetic
authority. Unknown/duplicate fields, malformed/truncated/oversized JSON,
corruption, invalid dates/coverage/scope and excess records are rejected. A
valid empty history exports an explicit zero count. Records remain unchanged,
including identity, reset/archive reason and date, original acquisition/peak,
source generation, boot ID, clock, origin, coverage, partial flags and nulls.
No unavailable average or reading becomes zero. Different source/boot/clock
records remain separate HISTORICAL facts; no date comparison/conversion or
physical journey is inferred.

The new directory is created with mode 0700 and must not exist, even empty or
as a symlink/file. The only final file is `trip-history-export.json` (0600,
single link). Version-1 JSON has exactly:

| Key | Meaning |
| --- | --- |
| `version`, `kind`, `classification` | `1`, `ultra-headunit-trip-history-export`, `HISTORICAL` |
| `sourceSchema` | `ultra-headunit-trip-history-v1` |
| `inputSha256` | SHA-256 of the original input bytes, including its formatting |
| `recordCount`, `history` | Original validated count and unchanged records |

The complete encoded export is bounded to 67,584 bytes. It has no export clock,
new acquisition, current-boot stamp or applied/physical-trip claim. JSON integers
retain exact 64-bit dates/generations; downstream consumers must use an integer
parser that preserves that precision rather than JavaScript Number.

A random exclusive temporary file receives bounded writes and file fsync.
A same-directory atomic hard link publishes complete JSON with no overwrite;
any existing destination wins and is preserved. The temporary link is removed,
then output and parent directories are synced and identities checked again.
Soft INT/TERM/HUP returns 130/143/129 with descriptor/owned-temporary cleanup,
including a signal pending during ownership registration. Failures attempt to
remove only this attempt's file/directory inodes. Foreign replacements are
preserved. Directory replacement can leave an empty relocated owned directory;
permission revocation, cleanup failure, SIGKILL, abrupt process death or power
loss can leave artifacts. A completed file may be visible before directory
sync/success, and failure/interrupt always leaves durability unknown. This is
not a power-loss guarantee or an immutable export against a privileged or
same-account writer.

Successful stdout contains only `recordCount`, `inputSha256`, `exportSha256` and
the canonical output `path`; no record contents. Refusal/failure/interrupt uses
fixed sanitized stderr. Keep exported records and all qualification fixtures,
reports and logs outside Git. Existing live archive bytes/identity stay intact.

For native UI: **new manager keys, action/control IDs, widgets, pages, styles and
UI pieces: none.** Any later UI export action needs its own scoped authority
and review; this command creates none.

### Focused checks and future sole-owner follow-up

Owned synthetic CLI fixtures cover original archive/schema baselines, valid and
empty history, maximum/mixed scopes, exact 64-bit integers/null/partial coverage,
malformed/unknown/duplicate/corrupt/oversized input, owner/group/mode/link/symlink/
special-file refusal, input/output replacement, atomic destination races,
short/failed writes, permission and file/directory/parent sync failure, and real
INT/TERM/HUP at read/write/atomic-link boundaries. They compare input bytes and
identity and preserve foreign outputs. Mac/Linux evidence and original failed
attempts/source/Testing/JSON/hash/retirement receipts remain outside Git in
`20261014/codex-trip-history-export/`. No C++ sanitizer, full matrix, GPU, GTK,
CM5, phone, deployment or physical-trip qualification follows from Python tests.
Final same-source macOS and Debian ARM64 each pass three focused Python suites
(24 storage, 33 sport, 15 export cases). macOS passes 69/72 with two existing
Linux/privileged storage skips and one actual foreign UID/GID export skip;
Linux passes all 72 with no skips. These use a direct CTest Python harness, not
a full project configure/build. The new constructor-return signal regression
first reproduces an owned-descriptor leak in the original exporter; masked
registration inside its cleanup scope fixes it. Current backend INT/TERM/HUP
proofs pass with repeated signals during retirement. Initial Docker absence
receipt failures remain retained; they do not count as completed lane passes.
Pinned format 19 and whitespace checks pass; no runtime policy is waived.

Future integration already containing the accepted private-history dependencies
cherry-picks only the new export commit. Root reviews the whole combined diff.
Before any CM5 adoption, the sole phone-session owner requires the complete
coherent release matrix, actual GTK checks, matching source/helper/config/input/
modules/argv/units/assets and fresh capacity/full reserve, including the held
V88 repair. This offline tool adds no sealed module or service. From the reviewed
source on the owner's host, first use a synthetic 0700/0600 fixture and a new
0700 export parent. Compare independent input/export SHA-256, count, exact
records/nulls/64-bit dates and unchanged input identity; refuse a repeat to the
same output. Only a separately authorized explicit stable archive may later be
exported; do not inject, corrupt or read selected private state for a test.
Keep main/touch/Home/Back, native formats/fallback, Climate/settings, both panels,
video/UI-Sync/audio/control, original source dates and owners independent.
The model remains unpublished and physical car/calibration/power-loss claims
remain open.
