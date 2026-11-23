<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Preserve the age of every vehicle signal

Use `VehicleObservationCache` when an in-process adapter supplies readings at
different rates. Speed, temperature, each warning light, coherent transmission,
observed drive mode, current-trip statistics and qualified heading keep their own
original acquisition time. Reading the cache, forwarding its snapshot or
drawing a frame does not make an old reading current.

This is a read-only software boundary. No physical sensor driver, calibration
or actuator is installed. [HVO1–HVO4](VEHICLE_OBSERVATION_PROTOCOL.md) carry
complete acquisition snapshots between local processes; legacy HUV1/HUV2
remain separate.

## Set up a source

Include `headunit/vehicle_observation.hpp`. Configure `VehicleAgePolicy`, then
construct the cache on its owner thread. The cache copies these age limits;
they cannot change during its lifetime. Default limits are 500 ms, not a
qualification of any physical sensor. Limits must be positive and at most
24 hours.

Call `begin_generation(generation, started_at, now)` before supplying data.
The generation must be positive and strictly increase after a source restart.
Its start must be positive, no later than `now`, and no earlier than the last
host operation. A new generation clears previous values and acquisition
watermarks. It does not carry old measurements into a new connection. The
immutable drive-mode catalogue and private trip identity/reset/counter history
survive; a source restart cannot lower trip counters or revive an old acquisition.

All dates use this host's qualified `steady_clock` domain. A microcontroller's
raw ticks or wall-clock time cannot be used directly; clock mapping and its
uncertainty remain part of the future hardware adapter.

```cpp
VehicleAgePolicy ages;
ages.readings[0] = std::chrono::milliseconds(100); // Example speed policy only.
VehicleObservationCache source(ages);
source.begin_generation(1, started, now);

const std::array batch{
    VehicleObservation{"speed_kmh", 42.0, speed_acquired},
    VehicleObservation{"coolant_c", 88.0, temperature_acquired},
    VehicleObservation{"low_beam", IndicatorState::on, lamp_acquired}
};
source.accept(1, 1, batch, now);
VehicleSample sample = source.sample(now);
```

`accept` applies at most 40 distinct semantic signals atomically. Omitted
signals stay unchanged. This **delta** contract differs from HUV2, where every
datagram is a complete snapshot and omissions become unavailable. Keys and
units come from the shared reading/indicator catalogues. The additional
`transmission` key carries one typed `TransmissionState`, with a shared
`VehicleAgePolicy::transmission` age policy. Its known components must form one
coherent effective state; type alone cannot infer selector or engaged gear.
See [transmission](TRANSMISSION.md).

The additional `drive_mode` key carries a bounded `DriveModeIdentifier` that
must belong to the optional `DriveModeCatalog` copied at cache construction.
Its `VehicleAgePolicy::drive_mode` is independent of speed/transmission.
Supported modes are immutable capabilities, retained through expiry,
withdrawal and generation restart. The active selection is a dated reading,
never an inferred/default mode. See [drive modes](DRIVE_MODES.md).

`trip` carries one coherent `TripState`: stable identifier, observed reset
generation and optional distance, total/moving time, supplied average with an
explicit time basis, and whole-trip mean consumption. `heading` carries one
qualified `HeadingState`: degrees, true/magnetic north and orientation/course
source. Their independent `VehicleAgePolicy::trip` and `::heading` limits do
not renew when speed changes. [Trip and heading](TRIP_AND_HEADING.md) specifies
the bounds, coherence and source-owned accumulation/persistence policy.

`acceleration` and `gps_position` are independent typed observations with
`VehicleAgePolicy::acceleration` and `::gps_position`. The former supplies
calibrated gravity-compensated forward/left acceleration in m/s²; the latter
supplies WGS84 latitude/longitude and positive horizontal accuracy in metres.
Their original dates, provenance, expiry and withdrawal follow the same atomic
cache rules. Explicit HVO6 transports them; older encoders refuse their values
or nondefault timing. [Sport telemetry](SPORT_TELEMETRY.md) owns timer semantics
and the hardware proposal
owns wiring/calibration. Physical acquisition remains unqualified.

## Report values and absence

Every observation has an acquisition date, an origin (`measured`, `calculated`
or `simulation`), and `available` or `unavailable` availability. Available
readings carry a bounded finite `double`; lamps carry `off`, `on` or `flashing`;
transmission carries a validated compound state with independently optional
type, effective selector and actual gear; drive mode carries a supported
semantic identifier; trip and heading carry their validated compound states.
Unavailable observations carry `std::monostate`. Zero speed and an off lamp
are real known values, not absence.

The cache derives `stale` at **age ≥ maximum age**. `withdraw(now)` clears all
recorded values as `disconnected`, retaining their acquisition dates and ordering
watermarks. An untouched, unobserved transmission record stays at its default
unavailable state for HVO1 compatibility. An untouched mode record also retains
its default unavailable timing for HVO1/HVO2 compatibility. A fresh speed update
cannot renew an old temperature, lamp, transmission, active mode, trip or heading.
Untouched trip/heading records retain default unavailable timing so earlier
wire versions remain usable when those extensions are absent.
Snapshots retain each signal's provenance; the aggregate simulation marker
remains set while recorded simulation observations remain in that generation.

`sampled_at` records snapshot publication time. For metadata-bearing samples,
it is not the sensor acquisition date or the freshness limit. Per-signal dates
live in `VehicleSample::observation_timing` and remain unchanged by polls.

## Reject replay without preventing recovery

Within a generation, batch sequences strictly increase and acquisition dates
cannot go backwards. An equal-date identical observation at a newer sequence
is an idempotent no-op: it cannot revive an expired or withdrawn signal.
An equal-date conflicting value, availability or origin rejects the whole
batch. Future dates, dates before the generation start, duplicates, unknown
keys and invalid values also reject the whole batch without consuming its
sequence or advancing the host clock.

A delayed observation can recover a signal when its acquisition is newer than
the previous observation and still within its age limit. Recovery is dated at
acquisition, not delivery. A newer but already expired observation stays stale.

Trip history also spans source generations. Within a reset, known distance and
time counters cannot decrease, even after partial unknown state. A higher reset
generation needs a strictly newer acquisition and permits counters to start
again. Dated individual counter watermarks prevent comparison of a newer moving
time with an older total as if both were current. Loss, expiry and generation
restart require a newer acquisition before trip data can return. A failed
history check rejects the entire batch without consuming its sequence or host
time. The cache, Unix receiver and direct Ultra bridge use the same bounded
candidate `TripProgress` contract; no reset command or persistent accumulator
is implemented here.

## Display the same current state

Native gauges, the backup cluster and the Ultra warning overlay filter values
through `current_vehicle_sample` and the shared indicator policy. Each signal
expires independently. A flashing lamp remains known during its dark phase;
unknown and stale lamps leave no active icon. Direction slots show faint neutral dashed arrow outlines to distinguish unavailable readings from known off/dark flashing phases.

`GaugeInput` publishes each bound Ultra scalar with its original acquisition
date. Related scalar conversions commit through `Service::publish_batch`,
which validates every entry before changing any value, watermark or service
time. Polls cannot republish the same observation. Age policies and timestamp
format stay fixed for one adapter lifetime; malformed snapshots and attempted
revival from arbitrary `VehicleSource` implementations are rejected atomically.

The local script service uses milliseconds. If two acquisitions fall into one
millisecond, the newer value is withheld until a genuinely later acquired
millisecond arrives; it is never assigned a fabricated future date. Profile
expiry still applies, so the effective lifetime cannot exceed either the
source's age policy or the script profile's limit. Legacy samples without
metadata retain their existing aggregate timestamp behavior.

## Keep transport and hardware claims separate

HUV1/HUV2 and the HUF1 fan-out envelope do not carry this metadata. Encoding a
metadata-bearing sample or transmission/mode/trip/heading as HUV2 is rejected rather than silently
losing fields, dates, provenance and generation. HVO1 retains its original
34-record schema; explicitly selected HVO2 adds transmission; HVO3 adds active
mode and its static catalogue; HVO4 adds trip and heading. HVO1 rejects a
transmission value or nondefault timing it cannot preserve. HVO1/HVO2 reject
mode fields or timing they cannot retain. HVO1/HVO2/HVO3 reject trip/heading
values or nondefault timing. All four versions have
strict packet validation and a bounded stateful receiver, whose first accepted
version stays fixed for its lifetime. The optional fan-out passes original bytes
unchanged; no legacy ingress stamp replaces original acquisition dates.

Physical acquisition/calibration, persistent trip accumulation/reset commands
and asynchronous applied-state commands remain future work. See
[the adapter plan](VEHICLE_ADAPTER_PLAN.md). Pack-specific artwork enums belong
in profile mappings, not the hardware interface.

## Receiver appearance admission — 15 October source repair

`GaugeInput` publishes the receiver's existing `VehicleFacts` only after its
source-history checks and script-service publication succeed. Previously an
otherwise well-formed conflicting low-beam snapshot changed the real receiver's
headlights decision before `pump_observed` rejected it. Rejection now preserves
the prior facts, including their original `observed_at` lease date, alongside
the unchanged admitted sample and service. This also protects the existing speed
restriction input and both legacy publication returns.

The combined `theme-gauge-input` regression uses simulated original acquisitions
from `VehicleObservationCache`, a deliberately faulty in-process `VehicleSource`,
the actual admission/service owners and `receiver_night_mode_now`. It covers
conflicts, reversed host time, changed age policy, explicit unavailable, known
Off, exact 500 ms source expiry, withdrawal, equal-acquisition revival, older
generation rejection, genuine successor recovery and failed service publication.
The original-production run fails on the actual receiver decision; its source,
failed test and retirement receipts remain outside Git.

The existing appearance contract stays unchanged: `headlights` follows admitted
fresh low beam; known Off means day, while missing/expired/withdrawn low beam uses
the configured schedule. Schedule minutes are genuine local wall time for host
policy, never a car acquisition or measured night signal. Forced Off/On, explicit
schedule windows, source age rules and the separate two-second facts lease retain
their existing semantics. Polling never renews the source acquisition. The
receiver's process-local facts store has no new phone lifecycle or physical
source identity contract.

`control_probe` still deduplicates decisions per connection and
`AirplaySessionSetup::send_night_mode` queues `setNightMode` only through its
existing RECORD/event-channel admission. Queue success is not phone acceptance.
The independent event-channel and Linux session-lifecycle fixtures exercise fake
phone/loopback transport; this regression creates no phone, GPU or display owner.
Physical low-beam acquisition, MCU clock mapping/identity, visible appearance and
phone acceptance remain unqualified. There are no new UI keys, IDs, actions or
GTK pieces; the forward model and this repair need whole-source integration and
qualification before a new immutable appliance generation.

## Verification

Portable tests exercise mixed sensor rates, exact expiry, zero/off versus
absence, provenance, atomic invalid batches, duplicates, delayed recovery,
source generations and owner-thread use. Integration checks cover original
dates in the actual script service, same-millisecond coarsening, rejection of
raw-source revival, and matching stale-field removal in native and backup
rendered images. Actual Unix-socket integration also exercises HVO1 through
the script bridge, including startup before the first packet and delayed
source restart. HVO2 socket/service checks cover coherent transmission, partial
knowledge, exact expiry, atomic replacement and rejected-snapshot sequence
preservation. HVO3 checks add catalogue membership/immutability and original
mode acquisition through the same consumer/service path. HVO4 adds coherent
trip/heading reception, lifetime history, loss/restart guards and original-date
mean-consumption publication. Native Trip pixel checks cover the supplied
statistics, heading reference/source and independent expiry; seven private
simulated snapshots cover known/unknown/zero/wrap/bounded presentation.
On 4 October 2026, the actual 64-bit CM5 Linux broker passes sixteen checks,
including kernel receive dates, original HVO2/HVO3/HVO4 bytes, queued-input expiry,
no replay to later consumers, duplicate-owner rejection and safe endpoint
cleanup. A real production CLI process is killed, restarted over its dead socket
and then stopped gracefully: only newly sent packets are forwarded, and normal
shutdown removes its endpoint. The process fixture uses isolated private sockets
and no live vehicle/display service. Its HVO payload is deliberately opaque;
that check does not qualify typed consumer parsing or a physical reading.
Physical car data, calibration and deployment into the running appliance remain
unqualified. Consumer/script and broker/process evidence stay separate.

## Explicit dated native simulation — 14 October, source only

`DatedSimulatedVehicle` is a **new, explicit SYNTHETIC fixture source**, separate
from `SimulatedVehicle`. The legacy `SimulatedVehicle` numerical generator remains
byte-for-byte unchanged; native `--simulate` retains its undated behavior
without `observation_timing`. Their native gauges retain
legacy behavior; their auxiliary projection correctly stays `undated_source`
with eight null readings. Missing acquisition authority is not a legacy bug.

The default-off saved receiver boolean `simulate_dated_vehicle: true` selects
native `--simulate-dated`. It excludes `simulate_vehicle: true`, `vehicle_socket`
and a vehicle fan-out configuration. Set the old flag false or omit it when
choosing this mode. Strict parsing, native CLI source exclusivity, shared
`vehicle_arguments`/`native_cluster_command`, exact publisher argv admission and
sealed unit/config validation preserve this choice. Mixed or duplicate native
source options reject before socket/display acquisition. Older disabled/legacy
release graphs remain verifiable. No new module, service or endpoint is added.
The existing optional `prephone_publication_socket` still needs its complete
managed module/configuration closure; selecting a fixture alone enables no IPC.

### Acquisition and lifetime

The same existing native owner constructs the source with its actual positive
host-steady start and generation **1 scoped to that source instance/publisher**.
This is not a global boot identity or a generation borrowed from the phone.
A process replacement needs fresh kernel/argv/pidfd/time-namespace admission and
a fresh manager nonce/consumer; an instance cannot restart after terminal retire.
The existing signed local-cluster caller receives this same source adapter.
There is no background acquisition thread, periodic provider service, logger or
pending queue. The connected receiver's independent vehicle configuration stays unchanged.
When `sport_telemetry` is opted in, either explicit simulation mode selects the
**existing independent** labelled sport fixture. Its original clock, source
start/generation, 20 ms worker, acquisition dates and software Trip owner remain
separate from this native source; no sport fields are added to native data.
With sport disabled, no sport source is created. Without either simulation mode,
existing unavailable/dedicated fan-out observation behavior is preserved.

At every strictly newer supplied acquisition, the source actually evaluates the
existing numerical/lamp/transmission/mode curve at that elapsed time, then
atomically admits those **newly generated** values into `VehicleObservationCache`.
It never wraps or dates an old cached legacy record. The actual generated
`sampled_at` supplies each acquisition. Equal-date calls retain the acquisition;
older/reversed dates and foreign-thread calls reject before generation. Cache
age limits are copied at construction and immutable; the default is 500 ms,
with exact expiry at age >= limit. This policy is a synthetic fixture policy,
not qualification of physical acquisition rates.

Linux runtime calls use actual C++ `Clock::now()` / `CLOCK_MONOTONIC` in the
kernel time namespace already checked by native publication admission. There is
no wall-clock/mtime offset, phone date, invented receipt or fake boot token.
Portable macOS tests use supplied fixture times; they do not qualify a runtime
mapping to the Linux manager. The Linux process fixture independently brackets
an actual C++ steady reading with `clock_gettime(CLOCK_MONOTONIC)`.

Every supported generated reading, lamp, transmission and effective mode has
`VehicleObservationOrigin::simulation`; the sample has `simulated=true`.
Numerical values and start-relative curve arithmetic match the original source.
No new fuel calculation, physical car facts, TripState, heading, IMU, GPS,
ignition or fitted-equipment state is supplied. Those absent compound records
remain unobserved, rather than dated zeroes. An available row's simulation
origin is genuine fixture provenance, never physical acceptance.

Copied snapshots and IPC heartbeats retain original acquisitions and immutable
age limits. A paused acquisition expires even while publication progresses;
missing/stale/retired values stay null. `retire(now)` withdraws cache values and
permanently stops generation for that instance, preserving acquisition/origin.
Native exceptions close publication before propagating; source-owner death,
IPC loss and publisher lease expiry retain the existing immediate terminal
withdrawal behavior. A genuine later fixture acquisition may recover after
silence; an old equal-date copy cannot. Generation replacement requires a new
source with explicit authority, not relabelling a prior record.

### Component checks and future adoption

The frozen pre-feature baseline uses the actual legacy source, C++ projection
and production Python `NativeModel`: undated identity/acquisition/provenance and
eight null rows are required. Focused source tests separately cover original
versus new numerical values, independently known initial values, all supported
origins/dates, absent physical fields, immutable ages, exact expiry, repeated and
reversed dates, owner-thread rejection, terminal nonrevival and newer-generation
HVO codec/receiver admission. Production cache/receiver/consumer regressions stay
unchanged. Linux fixtures exercise actual source -> `PublishingVehicle` ->
private kernel IPC -> `NativePublication` -> real manager status, preserving
source clocks and synthetic labels through expiry, withdrawal and process loss.
Strict configuration, native options and managed/unmanaged sealed-role tests
cover opt-in, conflicting modes, altered/missing flags and legacy graph closure.
Exact run results and retained failure/retirement receipts are recorded in the
source worker's coordination FINAL and private
`20261014/codex-dated-simulation-provider/` evidence.

Final focused Debug checks pass **macOS 11/11 (14.71 s)**, **Linux GCC 14/14
(23.06 s)** and **Linux Clang 19 ASan+UBSan 14/14 (26.50 s)** on identical final
non-Markdown inputs. No CTest retries or skipped CTests; the model-only Python
suite explicitly excludes its 14 kernel-owner cases, which run separately on
Linux. The new actual dated/legacy Linux publisher suite has four cases. Native
and return callers compile; headless compositor dependencies are absent, so no
compositor/presentation check is claimed. Linux retains default strict warnings
and leak policy. macOS retains external libplist/Cairo/OpenSSL dylib warnings
for macOS 27 libraries against deployment target 26. Pinned format 19.1.7 passes.
The first new decoder fixture compile failure and two existing backpressure
fixture failures remain retained separately. Recovery now awaits the first
actual admitted stale packet before generating a genuinely new recovery sample;
source age, publication/reconnect deadlines and production sender/consumer are
unchanged. Actual container INT/TERM/HUP retirement passes 130/143/129; retained
source/log/Testing/CMake/JSON hashes verify before every own build/container is
removed. Shared/private/selected inputs are untouched.

This is source/component work only. Before any live adoption, the integration
owner reviews the whole combined source, preserves ambient/native UI changes and
runs the complete coherent required matrix, including signed local callers.
Phone-session alone may then qualify a **new immutable generation and helper**
with matching configuration/input/module/unit/source/capacity receipts. Never
inject this mode into frozen V88 or edit a sealed generation.

For that future owner check: first retain the legacy `undated_source`/eight-null
baseline; then select the new reviewed native mode in new sealed inputs and
require all eight rows to carry genuine `simulation` origin and source-owned
acquisitions. Compare increasing acquisitions with actual new values; repeated
status/IPC polls cannot renew a held sample. Use isolated private fixtures for
silence/exact expiry, retirement, wrong role, late bytes and replacement, never
inject into a live physical vehicle endpoint. Preserve native fallback/formats,
Home/Back, Climate/settings, main/touch, both panels/video streams/UI-Sync/audio/
control and owner identities. Phone, display/optical, car calibration, camera/
audio continuity and fresh60 acceptance remain separate open gates.

### Sport/Trip source-choice follow-up

The actual saved-config Manager constructor on `d6030f0` reproduced unavailable
sport inputs under dated simulation while legacy simulation remained useful.
The source-choice correction selects the same existing SportSource for either
explicit synthetic mode. Six real constructor/worker cases cover Sport on/off,
legacy/no-source/fan-out observations, original acquisitions, actual Trip
progress/software reset, expiry and retirement. A fifth Linux native-publisher
case verifies independent native/Sport identities, reset and native-owner loss.
Joint dated-mode/Sport on/off sealed-module closure also passes. Final focused
checks: macOS **14/14**, GCC **17/17**, Clang 19 ASan+UBSan **17/17**; no full
matrix, GTK, physical provider or live qualification is implied. Original d603
reports and its failing baseline remain immutable; new source/retirement receipts
are under `20261014/codex-dated-simulation-provider/sport-path-fix/`. The future
combined private-trip graph must retain this source choice and its module closure.

## Typed receiver death and endpoint replacement — 14 October source-only

The existing socket test now combines `VehicleObservationCache` → original HVO4
bytes → `SocketVehicle` with actual child admission, queued input, SIGKILL,
`waitpid` and private dead-endpoint replacement. The former killed-owner case
only queued legacy HUV1 and never admitted a typed snapshot before death. This
is added regression coverage; no production defect or runtime repair is claimed.

The child returns its admitted snapshot as identical original HVO4 bytes before
death. Its retained value-copy keeps original acquisition, age and provenance,
but explicit retired admission removes pre-phone values and successor admission
rejects its generation. A replacement starts missing, discards the dead socket's
queue, admits a new empty generation and rejects a late old-generation packet.
Fresh known zero and an unobserved temperature remain distinct. A later-field
conflict cannot partly commit fresh speed or consume the valid recovery sequence.
Original fuel expiry remains independent of that speed update. A separate live
endpoint retains its own source and expires fields at their original limits.

Acquisition dates, clocks and readings in this case are **simulated**. Kernel
socket ownership, process death, reaping and endpoint replacement are actual host
software operations. The projection is read-only; it does not independently
authenticate a producer or discover death. A new consumer cannot infer an unseen
producer generation history. No physical vehicle provider, native/GTK pixels,
GPU, signed local-to-phone handover, installed release or phone behavior is
qualified by this case. New native UI keys, actions, IDs and UI pieces: **none**.
