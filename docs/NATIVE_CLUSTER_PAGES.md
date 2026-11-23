<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Native cluster center pages

## V84 disconnected format evidence — 14 October

The existing supervised headunit-cluster owner renders visibly distinct Minimal,
Sport and Full Gauges while the phone is disconnected. Saved choices agree with
the screenshots; simulated provenance remains visible. Original Trip preference
is restored, then both Ultra panels return with settled phone content. Existing
native fallback ownership is preserved. Private original captures:
`20261013/phone-session/v84-native-observed-*`. Physical providers and all live
handover/expiry/performance cases are not qualified by these three captures.

The native instrument cluster keeps speed, engine speed, key readings and active
warning lights in fixed positions. Its saved presentation can show four content pages and three native formats:

| Page | Content | Availability |
| --- | --- | --- |
| Map | The phone's live Apple Maps video across the **full width**, with the instruments drawn over it | Standard CarPlay connected |
| Media | Album artwork, track, artist and playback progress from the shared media state | Always; shows a connection prompt without a phone |
| Vehicle | Fuel, coolant and oil temperatures, oil pressure, range and battery voltage | Always; missing measurements stay unavailable |
| Trip | Acquired trip distance, elapsed/moving time, average speed, mean consumption and qualified heading | Always; absent/expired inputs show `--` |
| Minimal | Large central speed, observed gear/mode, optional RPM | Always; configured speed unit and RPM visibility apply |
| Sport | Large speed and central tachometer with configured maximum/redline | Always; this is a display format, not an applied vehicle drive mode |
| Full Gauges | Speed/RPM plus six auxiliary dials: fuel, coolant, oil temperature/pressure, range and voltage | Always; each absent/expired reading stays `--` |

The standalone backup on the cluster monitor and the Standard CarPlay cluster
use the same C++ renderer, `src/native_cluster.cpp`. The backup runs as a
separate Wayland background surface, so it remains available when the receiver
disconnects or fails. The current Ultra theme owns its own cluster layout, so
these native pages do not replace that theme while Ultra is active.

## Format data interface — 13 October source

native UI owns GTK page design. The pure `format_snapshot` / `format_choice` model
lives in `tools/cluster_layout.py`; manager `status` adds `cluster_formats`.
It reuses the existing `cluster_page` and signed `clusterLayoutFile` preference
owners. No new configuration path, release role or GTK restyling is required.

- `native`: `choices` contain `{id,name,page}`, `selected` is the saved/resolved
  preference, and `selectionKind="saved_preference"`. IDs are `map`, `media`,
  `vehicle`, `trip`, `minimal`, `sport`, `gauges`. Map is admitted only during
  fresh Standard readiness. These are preferences, not presentation receipts.
- `ultra`: `choices` contain `{id,name,layout,authoredName}` and come only from
  the existing fresh, publisher-bound validated catalogue. `selectable` requires
  ready/idle. `active`, `pending`, `confirmed`, `error` retain separate meanings;
  `confirmed` uses `confirmedActive`, never an unacknowledged local selection.
  The existing `ultra_cluster` carries the exact publisher and palette controls.

The current pack's format labels map `minimal` to DigiBand, `sport` to DigiRing,
`gauges` to Modular, and `maps_media` to MapsMedia. Match both declared UUID and
name; other declared styles retain their UUID/name. These are labels for authored
layouts, not promises that signed Modular has the native six auxiliary dials or
that changing the format changes actual vehicle drive mode. Large Map/Music
content still uses the phone's existing carousel. Unsupported layouts are absent.

```json
{"action":"cluster_format","renderer":"native","format":"sport"}
{"action":"cluster_format","renderer":"ultra","format":"sport","publisher":{"nonce":"CURRENT_NONCE","connectionGeneration":"CURRENT_GENERATION"}}
```

Native selection atomically writes the existing private page file, owned by the
display account. Existing `cluster_page` actions and CLI accept the new IDs;
Previous/Next cycles all available native choices. Ultra selection goes through
the original HUL1 owner, coalescing/phone confirmation and pack-scoped durable
preference storage. Busy, unavailable and retired publishers reject. The GTK
controls remain for native UI integration to build against this model.

Offline review commands (all outputs must remain in the private evidence folder):

```sh
headunit-native-cluster-snapshot "$EVIDENCE/minimal.ppm" --center minimal
headunit-native-cluster-snapshot "$EVIDENCE/sport.ppm" --center sport
headunit-native-cluster-snapshot "$EVIDENCE/gauges.ppm" --center gauges
headunit-native-cluster-snapshot "$EVIDENCE/unavailable.ppm" --center gauges --unavailable-vehicle
headunit-native-cluster-snapshot "$EVIDENCE/configured.ppm" --center sport --native-cluster-config /private/native-cluster.conf
```

`--sample /private/fixture.huv2` accepts a bounded simulator snapshot and explicitly
marks its review output simulated. It does not claim original physical acquisition
or HVO metadata. Runtime consumers keep the existing original per-signal dates,
age and provenance; no render clock refreshes observations.

Turn-signal design review (explicitly simulated transition history; no physical
acquisition/presentation claim):

```sh
headunit-native-cluster-snapshot "$EVIDENCE/turn-lit.ppm" --center sport --turn-signal-fixture lit
headunit-native-cluster-snapshot "$EVIDENCE/turn-mid-fade.ppm" --center sport --turn-signal-fixture mid-fade
headunit-native-cluster-snapshot "$EVIDENCE/turn-off.ppm" --center sport --turn-signal-fixture off
headunit-native-cluster-snapshot "$EVIDENCE/turn-hazard.ppm" --center sport --turn-signal-fixture hazard
headunit-native-cluster-snapshot "$EVIDENCE/turn-unknown.ppm" --center sport --turn-signal-fixture unknown
```

The shared [lamp contract](ULTRA_WARNING_OVERLAY.md#lamp-rendering-refinement--14-october-source-v84-target)
owns easing, observed-phase precedence, fallback cadence and no-data behavior.
Final refinement source `7abfedf` passes format, Mac 157, Linux 185 in both Debug
compiler configurations, and full Skia 186 in both receiver configurations.
[Scoped qualification](ULTRA_WARNING_OVERLAY.md#refinement-component-qualification--14-october)
owns the exact source, failed attempts, snapshots and CPU/cache limits.
No GTK controls or preferences change for this refinement; the format catalogue
above remains native UI's interface. V84 is the integration target.

## Fitted instantaneous speed precision — 14 October source-only

Optional version-1 `speed_precision=whole|one_decimal` selects only the existing
instantaneous digital speed values. C++ callers use
`NativeClusterConfiguration.speed_precision` with
`NativeSpeedPrecision::{whole,one_decimal}`. Omission defaults to `whole`,
preserving original generated bytes and all default pixels. Explicit whole and
unknown `--` use the original text drawing path and maximum size. The six required
fields and all old optional units/private profile remain valid. This precision
predecessor admitted eleven distinct fields within 4096 bytes; the pressure
predecessor admitted twelve and the [temperature precision extension](#native-temperature-precision--15-october-source-only) admits thirteen. Invalid enums, duplicate,
unknown, missing-required and malformed fields fail admission.

The compiler key is optional `nativeCluster.speedPrecision`. The exact read-only
manager key is `status.native_instruments.configuration.speedPrecision`, with
`whole` or `one_decimal`. This precision predecessor contained nine safe fields;
the pressure predecessor contained ten; the [temperature precision extension](#native-temperature-precision--15-october-source-only)
and current table below contain eleven. Original hash/role/helper/identity/mode/owner guards,
private-profile exclusion, copied cached status, ten-second kill/reap, fixed
errors and receiver-configuration omission-null semantics remain intact. This is
configuration intent, not an applied/displayed/physical receipt. There is no
persistence, live editor, new action, control ID, service/module or GTK/UI piece.

The existing exact international-mile conversion precedes display formatting:
canonical km/h is divided by 1.609344 for mph, then formatted with `%.0f` or
`%.1f`. Full one-decimal text is always retained: no ellipsis, scientific notation,
selective decimals, precision fallback or source clamping. Genuine zero displays
`0`/`0.0`; original signed-zero behavior is retained. Missing, invalid, expired,
withdrawn and successor-missing speed remains `--`. Trip average/RPM/fuel/range/
temperatures, arcs, labels, lamps and all vehicle/phone/CAF/Sport/software Trip/
statistics/private history stay unchanged. More digits do not establish higher
measurement resolution.

Root's 21:16 UTC manager decision authorizes bounded functional numeric fitting
under the display-format feature mandate. It supersedes the earlier no-size-change
trial restriction without requiring another user approval. Only a known optional
one-decimal **speed value** may shrink, only if its actual Cairo ink would escape
its allocation. Sans family, bold weight, tone, baseline, center, clip, gauges,
layout and adjacent content remain original; its size never grows or falls below
the existing 124 px instantaneous instrument size.

| Value site | Horizontal allocation | Original maximum/minimum | Original baseline | Following unit baseline/size |
| --- | --- | --- | --- | --- |
| Left instrument on Map/Media/Vehicle/Trip/Full Gauges |60..540|124/124px|358|404/24px|
| Minimal center |560..1360|192/124px|330|382/28px|
| Sport center |480..1002|158/124px|326|372/26px|

All three regions start at original y=96. The value's bottom ends before the
following unit's **actual** regular-Sans Cairo ink top, also bounded by its original
region. Later reserved pills begin at 462 (instrument drive mode), 430 (Minimal)
and 442 (Sport); unit/value ordering is unchanged. Sport's right boundary is the
original tachometer center 1170 minus radius 168. The private fitter uses real text
extents, x/y bearings and centered advance, including Cairo's positioned current
point. It tests the original maximum first, then 124 px and at most twelve bounded
bisection steps, retaining only measured fitting sizes. If the floor cannot fit,
it rejects rather than changing the number or widening geometry. Actual fonts,
provider calibration and optical readability require future coherent qualification.

For native UI review: the only new data enum/key is `speedPrecision` above; the
optional private C++ speed-glyph fitter is functional rendering behavior, not a
styling or control interface. New GTK pieces/actions/IDs are **NONE**. Future UI
must identify this as static configuration and retain original acquisition and
unavailable authority. No source component is a deployed/applied-unit receipt.

Focused evidence remains outside Git in
`20261014/codex-native-speed-precision/fit-v3/`. It retains the original unsized
Linux fit failures below unchanged. The current suite preserves all original 98
actual Cairo observer/full production pixel comparisons and adds all independent
unit interactions, exact source bounds/rounding/zero/expiry/withdrawal/generation,
minimum failure and a real negative-bearing glyph case. Every possible admitted
one-decimal string in both units (including signed zero) is drawn through the
production speed-text component at all three sites. Size/ink assertions retain
exact allocations, the 124 px floor, no growth, unchanged centers/baselines and
shrink-only-if-needed. The observer delegates actual move/text calls and compares
pixels with the independently linked unmodified renderer. Four old temperature/
distance/combined/consumption tests remain byte-identical; all 28 default/Fahrenheit
file images remain exact. Final same-source focused checks pass 20/20 on Mac
(117.14 s), strict Debian ARM64 GCC (56.44 s) and Clang 19 ASan+UBSan
(232.45 s), with 238 Python cases per lane and zero skips. The private final
report owns exact platform
counts, original failed attempts, hashes, images and retirement evidence.

This model remains source-only and unpublished. Root personal whole-diff review,
a NEW coherent required release matrix, actual GTK and matching helper/module/
role/config/argv/unit/input/assets/fresh capacity/full reserve precede any new
immutable CM5 generation by sole phone-session. Native caller compilation does
not qualify GPU/phone/display/physical providers, optics or fresh fps.

### Earlier unsized constraint — accepted 5620e99 milestone

The following records the earlier unsized trial; its source, original failures,
archive and receipts remain immutable outside Git. The fitted option above is a
separately qualified continuation under the manager's bounded functional decision.


At 5620e99, unsized one-decimal speed was **unshipped**.
The scoped trial cannot fit every admitted value with the existing Linux Sport
font, size and geometry. Sport uses bold Cairo `sans` at 158 pixels, centered at
x=700, with its original clip starting at x=480. Actual Debian ARM64 Cairo draws
`100.0` at x=467..943 and `300.0` at x=460..943, so 13 and 20 pixels of the left ink
are clipped. Canonical 99.96 km/h already rounds to `100.0`; the maximum admitted
300 km/h also displays `186.4 mph` at x=467..943. Reducing source bounds, clamping
values, changing the font/geometry or displaying fewer decimals at selected
speeds would violate the authorized scope. More digits would not establish
better measurement resolution.

There are three existing instantaneous-speed drawing sites: the left instrument
on Map/Media/Vehicle/Trip/Full Gauges, the Minimal center and the Sport center.
Each frame has one. Trip average speed is a separate supplied statistic. The
trial used one common speed formatter after the existing exact 1.609344 km-per-mile
conversion and observed the actual final Cairo calls without changing clipping
or fonts. Every observed image matched the independently linked production
renderer. Across all seven pages and both units, zero, small values, fractional
values, rounding boundaries and maximum 300 retained canonical values and original
acquisition/generation/provenance. Pixel differences were confined to the speed
value ink; all other texts and positions remained exact. Original speed expiry
still produced `--`. Linux reported three allocation failures above; Mac's actual
Sans metrics fit them, which does not qualify Linux.

The unchanged reviewed temperature/distance/combined/consumption import baseline
passes 19/19 focused checks on Mac (66.68s), strict Debian ARM64 GCC (37.28s)
and Clang 19 ASan+UBSan (104.24s), with no skips. The same trial source passes
20/20 on Mac (95.49s); GCC (49.89s) and Clang 19 ASan+UBSan (140.20s) each pass
the original 19 checks and fail only the new fit requirement, reporting the same
three clipping cases. No sanitizer diagnostic or policy waiver is recorded. All 28
imported default/Fahrenheit production snapshots remain byte-identical. The four
accepted Cairo test sources and all reviewed runtime/compiler/checker/manager
source bytes are restored exactly after preserving the trial outside Git.

No `speed_precision`, `nativeCluster.speedPrecision` or
`status.native_instruments.configuration.speedPrecision` interface was shipped at that milestone.
That version-1 parser retained its 4096-byte/ten-field limit and the checker
and immutable manager projection retained eight safe fields. Private profiles,
original input hashes, helper/role/mode/owner guards, cached startup lifetime,
ten-second kill/reap and omission-null semantics remain intact. New actions,
control IDs and GTK/UI pieces are **NONE**. Whole-number speed, arcs, canonical
vehicle/phone/Sport/software Trip/statistics/history and default pixels remain
unchanged. Static unit configuration remains intent, not an applied/displayed
or physical receipt.

Failed and interrupted attempts, original partial/absent JUnit, source snapshots,
Cairo metrics, Testing/CMake/reports and serialized retention manifests remain
outside Git under `20261014/codex-native-speed-precision/`. One Mac trial was invalid
because a probe input changed during its lane; it is excluded from qualification.
The first repeated-signal cleanup proof also failed when a group signal interrupted
the Docker stop client. The corrected backend isolates attach/cleanup subprocesses;
actual running-container INT/TERM/HUP with three cleanup repeats each passes
130/143/129, retaining two verified nonempty report copies and original
partial/absent JUnit before exact own-container removal. Cleanup is separate
from component acceptance.

The later manager decision above authorizes bounded numeric fitting; the original
failed trial itself remains unshipped. Before any future CM5 generation, root must review the complete combined
diff and the sole phone-session owner must complete a new coherent required
release matrix, actual GTK, helper/module/role/config/argv/unit/input/asset bindings
and fresh capacity/full reserve gates. A future precision implementation must
then pass actual Linux text/clip checks for every admitted speed and verify the
original acquisition expiry/withdrawal/generation and unchanged default images.
It must preserve main/touch/Home/Back/Climate, native fallback and both signed
panels/video streams/UI-Sync/audio/control. No trial is an immutable-generation,
GPU/phone/physical-provider/optical/applied-unit or fresh-fps qualification.

## Native distance units — 14 October source-only

`NativeClusterConfiguration::distance_unit` uses the fixed C++ enum
`NativeDistanceUnit::{kilometers,miles}`. The existing bounded 4096-byte version-1
configuration accepts optional `distance_unit=kilometers|miles`, defaulting to
`kilometers`. All six required fields and the optional absolute
`drive_mode_profiles_file` remain supported. Duplicate, unknown and invalid
fields/enums fail admission. The vehicle compiler accepts optional
`nativeCluster.distanceUnit` with the same values; omission preserves the original
configuration bytes and default pixels.

Only existing Vehicle/Full Gauges range numerical text and `km`/`mi` labels,
and native Trip distance text and its label, change. The international mile is
exactly 1609.344 metres. Range retains whole-number formatting; Trip retains one
decimal and the existing fitting in its card. Canonical `VehicleSample` and
`TripState` distances, validation bounds, acquisition dates, age, provenance,
Full Gauges range arc and warning policy remain in kilometres. Missing, invalid,
expired and withdrawn readings display `--`; no trip accumulation or replacement
reading is inferred. There is no new odometer, outside-temperature or other widget.

The related Trip **AVERAGE SPEED** defect is corrected: the existing
`miles_per_hour`/`speed_unit=mph` setting now converts the supplied average's
display number and shows `mph`, preserving its original `TripAverageSpeedBasis`
and total/moving detail. For example, supplied 60 km/h displays `37.3 mph`.
Default km/h remains unchanged. Speed and distance settings are independent:
`speed_unit=mph` with distance omitted still shows kilometre distances. All four
explicit speed/distance combinations coexist. Mean consumption stays the supplied
canonical value; the optional [consumption display unit](#native-consumption-units--14-october-source-only) adds explicit MPG text without changing that source.

For native UI, the only new configuration keys are `distance_unit` and
`nativeCluster.distanceUnit`, with enum/default above. **New status keys, actions,
control IDs, widgets, pages and GTK pieces: none.** This is static display
configuration, not an application or physical receipt. Pre-phone auxiliary rows,
phone/CAF/Car/Sport/software Trip and external unit policies are unchanged.
The reviewed compatibility source now adds `configuration.distanceUnit` through
the same C++ parser/default alongside `configuration.temperatureUnit`; see the
[configuration status contract](#native-instrument-configuration-status--14-october-source-only).
The original distance-only milestone above added no manager key. Any optional
presentation remains native UI.

Focused same-source component checks pass **14/14** on macOS AppleClang 21
(31.31 s), strict Debian ARM64 GCC 14 (15.02 s) and Clang 19 ASan+UBSan
(47.19 s), with no skips or retries. Actual final Cairo calls verify independent
known numbers, existing text allocations and pixels confined to target text;
all fourteen default seven-page images match the qualified-base images byte for
byte. Acquired cache/Trip checks preserve original dates, basis, provenance,
expiry, withdrawal and successor absence. The original mph Trip defect and one
failed comparison fixture are retained privately; the latter was corrected by
copying its configuration rather than mutating the next case. Linux native/
Standard/fallback callers compile only. Stock leak checks and external font/Cairo
suppressions remain; macOS external library deployment-target warnings remain.
Actual INT/TERM/HUP cleanup proofs and serialized hash-verified receipts precede
retirement of each owned build/container. Exact reports, source identities,
failed logs, images and the sole-owner bench recipe remain outside Git.

This feature remains source/component only. Root whole-diff review and a new
coherent required matrix, actual GTK and matching helper/configuration/input/
units/assets/capacity checks precede adoption in a new immutable generation by
the sole phone-session bench owner. Native caller compilation does not qualify
GPU presentation, real phone behavior, physical providers or displayed units.

## Pre-phone indicator model — 14 October source-only

`headunit/prephone_indicator_summary.hpp` provides one pure fixed-size C++
projection and a separate JSON encoder. This is an unpublished source component
for native UI integration. The optional bridge below adds its runtime caller and read-only
manager publication; the pure model itself owns no endpoint, provider, thread,
action or UI piece. The existing eight-row helper,
its version-1 JSON and native publication remain byte-for-byte unchanged.
The separate dated-simulation provider is not imported; tests acquire explicit
labelled simulation observations through the production `VehicleObservationCache`.

```cpp
PrephoneIndicatorAdmission admission{
    {current_vehicle_generation, source_retired},
    PrephoneIndicatorClockDomain::host_steady};
auto model = prephone_indicator_summary(admitted_sample, admission, now);
auto json = prephone_indicator_summary_json(admitted_sample, admission, now);
```

The existing source/cache/receiver owns source admission, replay history,
immutable individual age policies, generation replacement and actual owner
retirement. The projection reuses `prephone_vehicle_summary`'s admission gate
and the shared `valid_vehicle_sample` / `current_vehicle_sample` authorities.
It cannot authenticate a producer or detect a historical same-generation replay.
`clock_domain` defaults to `unqualified`: the caller must explicitly qualify
this host's steady domain and boot scope before projecting source dates. A
foreign, unqualified or invalid domain yields `invalid_snapshot`, null source
identity/timing/provenance/values and `clockDomain=null`; no clock mapping is
invented. Qualification is caller evidence, not something the enum proves.

There are exactly 24 rows, in the existing `indicator_definitions` order, and
at most 12288 JSON bytes including all metadata. Work is bounded by one typed
snapshot and the fixed catalogue; encode off control/VM/GPU callbacks. JSON
version is **1**, independent of the eight-row schema and existing wire versions.
Top-level keys are `version`, `state`, `clockDomain`, `sourceGeneration`,
`sourceStartedNs`, `sourceSampledNs`, `renderedNs`, `rows`. Row keys are `id`,
`label`, `color`, `availability`, `value`, `acquiredNs`, `maximumAgeNs`, `ageNs`,
`origin`. All dates, durations and generation are decimal strings or null.
`renderedNs` is the caller's positive host steady projection date, or null for
a nonpositive date; it never supplies acquisition, publication or receipt age.
`sourceSampledNs` is the supplied source snapshot's original publication, which
may advance when its acquisition owner samples; serializing a retained copy
never advances it. `ageNs` is render time minus original acquisition, including
historical stale/disconnected observations; it is null without an admitted date.
`maximumAgeNs` retains the original immutable per-indicator policy, including
unobserved fields in a valid dated generation. `origin` is `measured`,
`calculated`, `simulation` or null without acquisition.

`state` uses the established `admitted`, `awaiting_source`, `undated_source`,
`invalid_snapshot`, `generation_mismatch`, `disconnected` meanings. A qualified
clock without an admitted generation is awaiting source. Invalid metadata,
nonpositive render time, future publication, dates before generation or beyond
publication, invalid enum, and inconsistent values/provenance clear the entire
source projection. Legacy simulation/aggregate-only samples are undated and
cannot acquire dates from redraws. Admitted means a valid dated source scope,
including a scope with no currently available rows; it implies no physical
qualification, fitted equipment, link health or visible presentation.

`value` is the original canonical string **OFF**, **ON**, **FLASHING**, or JSON
null. Known OFF remains an available value. Animation lit/dark phases never
alter FLASHING. `availability` is independently `missing` (never acquired),
`unavailable` (dated explicit absence), `available`, `stale` (age >= original
maximum age), or `disconnected` (withdrawn or owner retired). Every nonavailable
row has null value. Stale/withdrawn/retired rows preserve historical acquisition,
age policy and provenance only within the admitted generation. A successor's
empty generation has no predecessor acquisition; an old snapshot supplied with
the successor admission yields `generation_mismatch` and no source metadata.
Source withdrawal may produce row-level disconnected while the coherent dated
snapshot remains admitted; explicit owner retirement makes state disconnected.

| `id` | Existing `label` | Catalogue `color` |
| --- | --- | --- |
| left_turn | LEFT | green |
| right_turn | RIGHT | green |
| hazard | HAZARD | amber |
| high_beam | HIGH | blue |
| low_beam | DIPPED | green |
| position_lights | LIGHTS | green |
| front_fog | FOG F | green |
| rear_fog | FOG R | amber |
| check_engine | ENGINE | amber |
| oil_pressure | OIL | red |
| battery_charge | CHARGE | red |
| brake_fault | BRAKE | red |
| parking_brake | PARK | red |
| abs | ABS | amber |
| stability_control | ESC | amber |
| traction_control | TRACTION | amber |
| airbag | AIRBAG | red |
| seat_belt | BELT | red |
| door_open | DOOR | red |
| low_fuel | FUEL | amber |
| coolant_warning | TEMP | red |
| washer_low | WASHER | amber |
| tyre_pressure | TYRES | amber |
| glow_plug | GLOW | amber |

Catalogue colours are existing presentation definitions, not severity, fitting
or warning policy. Two turn observations do not infer hazard. No numeric
threshold, all-clear, physical fitting or control is derived. Headlight inputs
remain only the canonical `high_beam`, `low_beam`, `position_lights`, `front_fog`
and `rear_fog` observations with their original freshness. Day/night decisions
and appearance remain native UI. Actions, new UI/widget/control IDs: **none**.

Tests use actual production cache acquisition, HVO4 codec and receiver plus
typed projection/JSON assertions for all catalogue entries, OFF/ON/FLASHING,
both animation phases, independent exact expiry, repeated copies, source
withdrawal, same-date nonrevival, owner end, generation replacement, atomic
invalid acquisition and invalid metadata/clock rejection. Exact 64-bit dates
and generations, explicit simulation labels and bounded locale-independent
JSON are component evidence. Qualified-base macOS checks pass 7/7 (7.99 s).
Final same-source macOS passes 8/8 (8.18 s), strict Debian ARM64 GCC passes
10/10 (2.68 s), and Clang 19 ASan+UBSan passes 10/10 (17.98 s), with Linux
leak checking and existing external suppressions retained. There are no skips
or retries. Existing native Wayland/return callers compile on Linux; this is
not GPU execution. Two failed Mac attempts (wrong target name, then a fixture
row-key count assertion) remain retained separately; only the runner/fixture
was corrected. The actual 64-bit fixture encodes 4992 bytes; a conservative
catalogue/token/integer-width ceiling of 5627 bytes fits the 12288-byte limit.
Pinned format 19.1.7 and whitespace checks pass. Original source/logs/failed
Testing/CMake/JSON/hash receipts remain outside Git under
`20261014/codex-prephone-indicator-model/`; own build trees and containers retire
after each lane. Real backend INT/TERM/HUP cleanup checks pass 130/143/129.
No deployed native publication, GTK, GPU, physical vehicle, phone or end-to-end
acceptance follows. Synthetic measured/calculated fixture tags test provenance
metadata and do not establish physical acquisition.

The optional bridge below is separately qualified and still unpublished. Root
reviews the whole combined source and bridge contract, runs a new coherent complete required matrix and
validates matching helper/modules/source/configuration/units/assets and fresh
capacity before the sole phone-session owner publishes a new immutable
generation. Until then this model has no bench recipe to enable. Future owner
checks must retain original source admission/clock/retirement, label simulation,
compare OFF/FLASHING/absence/exact expiry and generation loss in isolated cache
fixtures, and preserve main/touch/Home/Back/Climate/native fallback, both panels,
video/UI-Sync/audio/control. Physical source and appearance acceptance are separate.

## Native consumption units — 14 October source-only

The existing native Trip **CONSUMPTION** card accepts one independent static
`NativeFuelConsumptionUnit::{liters_per_100km,us_mpg,imperial_mpg}` setting.
Optional `fuel_consumption_unit` in version-1 native configuration and compiler
`nativeCluster.fuelConsumptionUnit` use those exact enum strings; omission defaults
to `liters_per_100km` regardless of speed, temperature or distance units. Omission
preserves generated bytes and default pixels. All six required fields, private
`drive_mode_profiles_file`, temperature and distance keys remain valid. The parser
keeps its 4096-byte bound and now permits at most ten distinct fields. Unknown,
duplicate, empty, mistyped or invalid enums reject; no unit editor or persistence
is introduced.

The card labels are `L/100 km`, `mpg US` and `mpg Imperial`. Only its value/unit text
changes, through the existing Cairo allocation and fitting. The source supplies
`TripState.fuel_efficiency_l_per_100km` as the same whole-trip mean. No speed-based
consumption, default car reading, reset, new provider or phone/CAF/Car/Sport/private
history conversion is added. Original acquisitions, age, origin, source generation,
coverage, other labels/details, arcs, lamps and geometry remain authoritative.
Missing, invalid, expired and withdrawn consumption stays `--`.

Exact definitions are from [NIST SP 811 Appendix B.9](https://www.nist.gov/pml/special-publication-811/nist-guide-si-appendix-b-conversion-factors/nist-guide-si-appendix-b9)
(Imperial gallon **4.54609 L**, international mile **1.609344 km**, inch **0.0254 m**)
and [NIST Handbook 44 Appendix C](https://www.nist.gov/system/files/documents/2024/04/09/NIST%20HB44%202024%20Appendix%20C.pdf)
(US liquid gallon **231 cubic inches exactly**). Thus its derived volume is
`231 × 0.0254³ × 1000 = 3.785411784 L`. The display derives
`MPG = 100 × gallonLitres / (1.609344 × Lper100km)` using the explicitly selected
gallon; it uses no rounded copied MPG factor or ambiguous gallon definition.

Metric zero remains `0.0 L/100 km`. Inverse MPG for known zero displays **`∞`**
with the selected explicit label, preserving canonical zero. This denotes an
unbounded mathematical inverse, not a finite fuel-efficiency measurement; no
division by zero or nonfinite numeric/JSON value is created. Positive values use
one decimal below 10000 MPG and three-significant-digit scientific notation at
or above 10000 (for example `2.35e+08 mpg US`). Decimal mantissa/exponent are formed
without overflowing a double quotient, so even the smallest positive canonical
double stays finite text (up to exponent325). Rounding may raise the exponent;
canonical input is never clamped. The text buffer is bounded to32 bytes and reuses
the original card fitting, fonts and geometry.

**For native UI:** the only new read-only status key is
`status.native_instruments.configuration.fuelConsumptionUnit`, from the same
C++ enum/default as rendering. The exact safe checker/manager configuration now
has eight fields, adding this string to the prior seven. Private profile content
stays excluded. Original SHA-256, role/helper/identity/mode/ownership guards,
startup-only immutable copied configuration, ten-second helper kill/reap,
omission-null and fixed error semantics remain intact. This is configuration
intent, never an applied/displayed/physical receipt. **New actions, control IDs,
services, modules, status-refresh paths and UI/GTK pieces: NONE.**

The imported combined baseline is retained outside Git; all18 focused checks
pass before extending it. Accepted temperature/distance/combined Cairo tests remain
unchanged. New checks exercise the actual C++ parser/compiler/summary/Manager,
all independent unit combinations, exact ten-field/4096 bounds, safe typing and
private exclusion, failed staging, immutable copies/hash/retirement and production
Cairo text/pixels for positive, zero, tiny positive, unavailable and original
expiry/withdrawal/generation cases. Final same-source focused checks pass **19/19**
on macOS AppleClang21 (72.35s), strict Debian ARM64 GCC14.2 (39.45s), and
Clang19.1.7 ASan+UBSan (107.68s), with no skips, failed builds/tests or retries.
All28 imported default/Fahrenheit images remain byte-identical; six additional
production snapshot images cover explicit US/Imperial positive, zero and tiny
values. Infinity/scientific card outputs also pass visual review in the existing
allocation. Original Mac external-library deployment-target warnings and the
exact portable-checker ARM-header fixture guard remain explicit. Linux uses the
stock compiler/warning/leak/timeout policies and existing font/Cairo suppressions.
Native/Standard/fallback callers compile only; no Skia/full-matrix result is claimed.
Actual backend INT/TERM/HUP plus repeated cleanup signals pass with exits130/143/129;
all owned build trees/containers are retired after serialized, independently
reread source/log/Testing/CMake/JSON/hash retention. Receipts, source archives,
visuals and the exact future sole-owner recipe stay outside Git under
`20261014/codex-native-consumption-units/`.

This remains an unpublished source/component interface. Root whole-diff review,
a new full coherent release matrix including actual GTK, matching helper/config/
module/unit/input/assets and fresh capacity/reserve gates precede any new immutable
CM5 generation. Only phone-session may perform the retained exact bench recipe.
Caller compilation does not qualify GPU/phone behavior or physical consumption;
physical providers, optical display and applied units remain unqualified.

## Native oil-pressure units — 15 October source-only

Optional version-1 `pressure_unit=kpa|bar|psi` selects only the numeric oil-pressure
text and unit label in Vehicle and Full Gauges. `NativePressureUnit::{kpa, bar, psi}`
is independent of speed, speed precision, temperature, distance and consumption.
The default is `kpa`; omission preserves the original configuration bytes and kPa
pixels. The compiler accepts optional `nativeCluster.pressureUnit` with exactly
those strings. It emits the optional line only when supplied.

Canonical `oil_pressure_kpa` remains the sole source, bounded to **0–1500 kPa** by
the unchanged vehicle catalogue. Full Gauges retains its authored **1000 kPa** arc
maximum, including its existing behavior above that scale. Source validation,
acquisition dates, age, origin, generation, provenance, warnings and all other
readings remain unchanged. Missing, invalid, expired and withdrawn data displays
`--` with the selected label; genuine zero remains known zero.

| Setting | Label | Existing bounded formatter | Canonical zero / maximum |
| --- | --- | --- | --- |
| `kpa` (default) | `kPa` | Whole number, original path | `0` / `1500` |
| `bar` | `bar` | One decimal | `0.0` / `15.0` |
| `psi` | `psi` | Whole number | `0` / `218` |

[NIST SP 811 Appendix B.8](https://www.nist.gov/pml/special-publication-811/nist-guide-si-appendix-b-conversion-factors/nist-guide-si-appendix-b8)
defines **1 bar = 100 kPa**, **1 inch = 0.0254 m**, and standard gravity
**9.80665 m/s²** exactly. Its [footnotes 22 and 23](https://www.nist.gov/pml/special-publication-811/nist-guide-si-footnotes)
give **1 pound = 0.45359237 kg** and **1 pound-force = 4.4482216152605 N** exactly.
Thus `Pa per psi = (0.45359237 × 9.80665) / 0.0254²`, approximately
6894.757293168361. The renderer evaluates that defining relation in binary64,
converts from canonical kPa, and uses the existing 32-byte `snprintf` formatter.
Rounding is display formatting, never sensor accuracy, resolution or calibration.
Small positive values can round to zero; their source value stays unchanged.
Signed zero follows the existing formatter. No pressure reference (absolute or
gauge) is inferred or changed by unit conversion.

The original Vehicle card's complete-string fitting and Full Gauges' fixed
32/16-point numeric/unit text are retained. No font, palette, cell, geometry,
arc or threshold changes accompany the option. The full supplied string must fit
inside those existing cells; no clamping, ellipsis or alternate precision path
is introduced.

**For native UI:** only readonly
`status.native_instruments.configuration.pressureUnit` is new. The pressure predecessor
summary had **ten** safe fields and at most **twelve** distinct native fields
within the unchanged **4096-byte** bound. The temperature precision extension
below adds one enum to each inventory. All six required
fields, prior types/defaults, private-profile exclusion, SHA-256, sealed role,
helper, owner/mode, immutable startup copy/cache, ten-second helper kill/reap,
fixed errors and omission-null semantics remain. The configuration is static
intent, not an applied/displayed/physical receipt. The auxiliary
`status.prephone_vehicle.rows` catalogue keeps canonical `oil_pressure_kpa` and
its original unit/ID. **New actions, control IDs, services, modules, persistence,
GTK widgets, pages and UI pieces: NONE.**

This is still an unpublished source/component model. Root must review the whole
branch and accepted dependency chain before folding it. A future coherent full
required release matrix, actual GTK, helper/source/module/role/configuration/argv/
unit/input/assets closure and fresh complete capacity/reserve checks precede any
new immutable CM5 generation. Phone-session remains the sole bench owner; native
caller compilation is not GPU, phone, physical sensing, optical readability,
audio/camera continuity, physical wake or fresh-frame acceptance.

## Native temperature precision — 15 October source-only

Optional version-1 `temperature_precision=whole|one_decimal` selects only existing
coolant/oil numeric text in Vehicle, Full Gauges and the original bottom readings
strip across all seven native pages. The C++ enum is `NativeTemperaturePrecision`
and its field is `NativeClusterConfiguration.temperature_precision`. Omission
and explicit `whole` preserve the existing whole-number path and pixels.
`nativeCluster.temperaturePrecision` accepts the same two strings; omission
leaves generated native configuration bytes unchanged. Temperature units and all
other unit/precision options remain independent.

The existing formatter receives canonical Celsius, or `C * 1.8 + 32` for
Fahrenheit, before applying `%.0f` or `%.1f`. The original 32-byte bounded buffer
holds every admitted result. Additional digits specify display formatting, not
measurement resolution, accuracy, calibration or applied state. Genuine zero and
signed zero retain formatter authority; missing, malformed, expired and withdrawn
readings stay `--`. Celsius values, source acquisition/age/origin/generation,
provider admission, warning sources, 150/180 C dial scales and all other data
remain unchanged. Ambient and Climate temperatures gain no display or behavior.

The unchanged catalogue admits coolant **−50…180 C** and oil **−50…200 C**.
One-decimal outputs therefore range from `-50.0` to `180.0`/`200.0` in Celsius
and `-58.0` to `356.0`/`392.0` in Fahrenheit, including signed rounded zero.
Existing Vehicle cards fit complete value/unit strings through their original
42/19-point path; Full Gauges retains 32/16-point text and the bottom strip
28/15-point text. No new sizing policy, layout, baseline, center, clipping, font,
tone, arc, threshold or label is introduced. Every admitted formatted output must
fit the original cells in actual Cairo; no truncation, scientific notation,
selective decimals, clamping or narrower source domain is permitted.

**For native UI:** only readonly
`status.native_instruments.configuration.temperaturePrecision` is new, with
`whole` or `one_decimal`. The current exact safe summary has **eleven** fields;
the native parser accepts at most **thirteen** distinct fields within **4096 bytes**.
Version 1, six required fields, existing optional private profile, SHA-256,
role/helper/owner admission, private exclusion, immutable startup copy/cache,
ten-second helper kill/reap and omitted-input null semantics remain unchanged.
Configuration reports intent, not a displayed/applied/physical receipt. Canonical
auxiliary row IDs/units are unchanged. New actions, control IDs, services,
providers, persistence, GTK widgets/pages/styles and UI pieces: **NONE**.

Qualification is source/component only; receipts, failed attempts, source identity,
production Cairo images and retirement manifests belong outside Git under
`20261014/codex-native-temperature-precision/`. New configuration cases have a
separate 30-second CTest invocation; both original native status invocations and
their workloads/deadlines remain intact. Existing pressure/speed/unit tests retain
their original cases. Native caller compilation is not a GPU, phone, physical
provider, optical, fresh-frame or deployment qualification.

This model remains unpublished. Root's whole-branch/dependency review and a new
coherent full required release matrix, original GTK/native UI, helper/source/module/
role/config/argv/unit/input/assets closure and fresh complete capacity/full reserve
gates precede any sole phone-session owner's new immutable CM5 generation.

## Pre-phone auxiliary row interface — 14 October source

`include/headunit/prephone_vehicle_summary.hpp` extends the existing
`VehicleSample` data boundary with a pure, fixed eight-row projection and
bounded JSON. This fills the missing nonvisual interface for original dates,
per-field availability and provenance in richer native/Home2 content. It does
not duplicate link-health rows or replace Minimal/Sport/Full Gauges, the native
Vehicle/Trip renderer, signed inputs, or the existing format/preference model.

```cpp
// Use the SAME snapshot already admitted by the existing vehicle owner.
// admission.generation is that owner's vehicle generation, never phone generation.
VehicleSummaryAdmission admission{current_vehicle_generation, source_retired};
auto rows = prephone_vehicle_summary(admitted_sample, admission, now);
auto json = prephone_vehicle_summary_json(admitted_sample, admission, now);
```

The caller owns acquisition, atomic admission, immutable age policies, replay
history, actual source generation and retirement. The projection owns none of
those resources. It uses `valid_vehicle_sample` and `current_vehicle_sample`,
does no I/O, and adds no socket, process, thread, display owner or history.
Encode only off control/VM/GPU callbacks. Memory/work are bounded by one existing
typed snapshot and eight rows; JSON is limited to 8192 bytes and contains only
fixed first-party labels and validated scalar values. No private signed assets
or phone content are read.

### For native UI review: exact data keys and row IDs

No GTK files, widget framework, page, visual style, action, preference or service
role changes. The JSON function is an interface, **not a new manager `status`
key**. native UI may later consume these rows with the existing Home2 grouped-list
components; all new UI pieces in this source lane: **none**.

| Row `id` | `group` | `label` | Canonical `unit` |
| --- | --- | --- | --- |
| `fuel_percent` | `energy` | Fuel | `%` |
| `range_km` | `energy` | Range | `KM` |
| `battery_voltage` | `energy` | Battery voltage | `V` |
| `ambient_c` | `temperatures` | Outside temperature | `C` |
| `coolant_c` | `temperatures` | Coolant temperature | `C` |
| `oil_c` | `temperatures` | Oil temperature | `C` |
| `oil_pressure_kpa` | `engine` | Oil pressure | `KPA` |
| `odometer_km` | `distance` | Odometer | `KM` |

These are the existing typed optional readings, including range/odometer only
when actually supplied. No range calculation, odometer accumulation, fuel
economy, ADAS, tyre-pressure values or body-position facts are synthesized.
The existing warning lamps are not a whole-car fault-clear authority and are
not folded into an inferred “all clear” summary. No physical actuation.

Top-level JSON keys are `version=1`, `state`, `clockDomain="host-steady"`,
`sourceGeneration`, `sourceStartedNs`, `sourceSampledNs`, and `rows`.
Each row has `id`, `group`, `label`, `unit`, `availability`, `value`,
`acquiredNs`, `maximumAgeNs`, and `origin`. The single source generation binds
the **whole coherent row set**. Dates, ages and generation are decimal strings
or null, preserving 64-bit identity/nanosecond precision through JSON consumers.
`value` is a finite number in the listed original units or null. `origin` is
`measured`, `calculated`, `simulation`, or null without an acquisition.
Display conversions are the frontend's explicit presentation policy.

`state` is `admitted`, `awaiting_source`, `undated_source`, `invalid_snapshot`,
`generation_mismatch`, or `disconnected`. **Admitted describes a valid dated
snapshot, not current coverage, physical qualification, link health or visible
presentation.** Even an admitted snapshot may contain eight missing/stale rows.
No admitted generation means awaiting source. Legacy HUV/simulator snapshots
without per-signal dates yield undated source with no displayed values; their
aggregate ingress/render date cannot become acquisition. Established legacy
gauges retain their existing behavior. Malformed/future snapshots and foreign
generations yield no values, source identity, dates or provenance. Never attach
old rows to a successor's generation.

Row availability is independent:

- `missing`: no observation acquired for this field in the admitted generation.
- `unavailable`: a dated explicit unavailable observation; preserve its origin/date.
- `available`: the actual supplied value, including known zero and negative temperatures.
- `stale`: available acquisition expired at **age >= original maximum age**;
  preserve date/policy/origin, withdraw value.
- `disconnected`: source withdrawal, or explicit owner retirement applied to
  a still-fresh cached snapshot; preserve admitted historical dates, withdraw value.

Polling, a new speed acquisition or a serialized reply does not renew auxiliary
dates. `sourceSampledNs` remains the original snapshot publication, never a
projection time or the sensor date. A delayed UI must reproject at the actual
current host steady date, or independently hide a row at its original acquisition
expiry and reject replies for retired/foreign vehicle generations. Existing
Home2 dispatch/reply-age rules still apply. Dates cannot be compared across
reboot or an unqualified clock domain. A pure projection cannot authenticate a
producer or detect same-generation replay by itself; use the existing source's
admission/history and explicit retirement, never a historical JSON as authority.

### Focused evidence and integration boundary

The production regression runs acquired cache -> original HVO4 codec -> existing
receiver -> row/JSON projection. Six scenarios cover missing versus observed
absence and zero, mixed rates/exact expiry/delayed original clocks, atomic
invalid-batch failure, explicit cached-snapshot retirement and source withdrawal,
same-date nonrevival/fresh recovery, generation reset/late old packet rejection,
malformed/undated/future inputs, and locale-independent bounded JSON with exact
64-bit generation/dates above 2^53. No fixture establishes a physical reading.
Private logs/source identities/Testing and lane retirement receipts live under
`20261014/codex-prephone-content-model/` in the qualification cache. Focused
macOS Debug checks pass **4/4 in 1.02 s**; Debian ARM64 Clang 19 Debug
ASan+UBSan checks pass **4/4 in 0.09 s**, with Linux leak detection and existing
external suppressions retained. Each includes all six new scenarios plus the
existing cache, codec and receiver suites, with no skips or retries. The Linux
runner uses a read-only retained source, own host-mounted build and matching
nonroot host UID; this is a disclosed focused-runner layout, not a full/default
CI run. Video/GPU targets are outside this pure data scope. macOS retains the
installed libplist macOS-27-versus-target-26 linker warning; compiler `-Werror`
and pinned formatting stay unchanged. Real owned-container INT/TERM/HUP probes
pass with exit 130/143/129 through the same cleanup backend. Both build trees
and all four own containers retire after retained hashes verify. No full
matrix, CM5, phone, GTK, physical acquisition or deployment claim.

**Explicit park: manager publication and GTK consumption.** Current manager
`SportSource` validates sport fields while auxiliary compound/wire fields remain
opaque. Its ordinary simulation is a different sport fixture; treating those
strings or values as admitted native gauge facts would bypass the C++ contracts.
Do not add a second socket bind, acquisition reader or renderer to obtain rows.
The reviewed integration must project the snapshot already admitted by the
existing C++ vehicle owner, then pass a copied bounded model through an existing
approved publication path with its actual vehicle generation/retirement. This
lane creates no such publication route. Finished trip-statistics source was read
for reference only; this interface has no dependency on that branch.

Exact bench follow-up, **sole phone-session owner only**, after manager whole-diff
review and a coherent combined required matrix/new immutable generation:

1. Review the existing-owner publication contract and native UI's consumer first;
   retain source/release/input identities and original vehicle generation/domain.
   Without that bridge, these functions are component-only and no live row claim
   is possible. Keep physical provider absence explicit.
2. With an isolated admitted source fixture, publish one known zero fuel,
   negative simulated outside temperature, calculated range and missing odometer.
   Compare canonical values and original dates across repeated polls; unrelated
   speed/heartbeat updates cannot refresh them. Verify exact independent expiry,
   explicit unavailable and source loss, with values becoming null and dated
   simulation/provenance labels remaining honest.
3. Retire the owner with a fresh cached snapshot outstanding, then create a
   genuinely new source generation. Reject late old rows, preserve the old source
   identity only as historical data, start new rows missing and require new
   admitted acquisitions for recovery. Inject no fixture into selected live input.
4. Observe current main/touch/Home/Back/Climate/native fallback and Minimal/Sport/
   Full Gauges alongside both connected streams/UI-Sync/audio/control. This
   nonvisual interface alone proves no pixels, phone handover, camera/audio/fps
   acceptance, physical sensors or hardware wake.

## Component qualification — 13 October

Source `392bac1` is source-only, not a CM5 release. Pinned format 19.1.7 and
macOS 157/157 pass. Original broad default Linux GCC and ASan+UBSan finish184/185
each, failing only the old wall-time startup CPU-budget fixture; the corrected
exact-policy affected cases pass8/8 in each Debug compiler configuration.
Full Skia strict optimized GCC and ASan each pass186/186 nonhardware suites.
No warning exception, Linux leak-check disablement or assertion/deadline change
is used. Remaining builds use private Mac-backed build directories, native Linux
builder-owned source, and executable bounded512MiB/tmp tmpfs because shared
Docker storage was near full. This is a disclosed runner-layout variation.

The production HUV sender and `SocketVehicle` run through two isolated Linux
socket scenarios: left flashing and paired hazards, seven observed phase changes
each, actual ingress dates retained, sender-death expiry verified. Startup sampling
includes renderer warm-up delays; these receipts are not physical flash cadence
or presentation timing. The MCU HVO self-test passes 17 typed socket checkpoints,
including independent expiry, withdrawal, replay rejection and recovery.

Known/unavailable snapshots for all three native formats are reviewed, including
zero versus absence, configured speed units/RPM visibility and fixed lamp slots.
All six current-pack styles pass an explicitly simulated offline CPU probe; their
rendered snapshots are separately composed with the production warning overlay
for known/unavailable arrow placement. The final speed-formatting warning repair
has pixel-identical output in six known/unavailable native snapshots. No phone
clock, video or real presentation/readiness is inferred from those images.

Private receipts, exact source hashes, fixtures and PNG/PPM images are under
`~/.cache/headunit-qualification/20261013/codex-cluster-signals/`. Initial module
packaging/fixture failures, optimized GCC warning, superseded own builds and the
noexec tmpfs probe are retained separately from corrected passes. Connected
switching/persistence, startup/fallback clearing and physical vehicle evidence
remain for the exclusive bench owner. The exact deploy/check plan is
`READY_FOR_BENCH.md` in that folder and the shared coordination note.

## Configured native instruments

The [vehicle definition](VEHICLE_CONFIGURATION_GUIDE.md#one-vehicle-definition)
can generate `native-cluster.conf`. The receiver setting `native_cluster_config`
is an optional owned input, validated by `headunit-vehicle-config-check` before
launcher admission. Standard's cluster reads its explicit
`HEADUNIT_NATIVE_CLUSTER_CONFIG` once at construction; the independent fallback
receives `--native-cluster-config`. The launcher clears inherited paths when
absent. Managed releases retain it through the `native_cluster` role and relocate
the receiver reference to that sealed file. Version-1 releases cannot add this
new role.

The bounded 4096-byte format requires all six fields:

```ini
version=1
speed_unit=mph
speed_maximum_kmh=160
rpm_maximum=5000
rpm_redline=4000
show_rpm=true
```

These numbers are illustrative display parameters, not a Defender specification.
Speed is acquired in km/h; the numeric instrument converts to mph when configured
and its arc still uses the declared canonical km/h maximum. RPM maximum/redline
affect only the display, and `show_rpm=false` removes its gauge while retaining a
separately observed gear badge. Missing/expired data remains unavailable.
Unknown/duplicate/missing fields, invalid units/Boolean, nonfinite numbers,
out-of-range limits and redline above maximum reject. Direct render callers are
also validated. Omission retains the existing 260 km/h, 8000/6500 rpm bench
presentation for compatibility; it is not inferred from a vehicle identity.
The runtime reader opens without following symbolic links or waiting for pipe
writers. Only a nonempty regular file up to 4096 bytes is admitted; inode,
length and modification/change dates must remain stable through the bounded
read. A bad input fails configuration before surface ownership begins. On
9 October 2026, the strict portable reader/pixel suite passes real FIFO,
directory, symbolic-link and oversized rejection plus complete atomic file
replacement. The FIFO regression bounds its own child-process wait so an old
blocking reader reports failure without hanging the test runner.

### Native instrument configuration status — 14 October source-only

`status.native_instruments` is read-only data for native UI integration. The existing
`headunit-vehicle-config-check --native-summary` consumes at most 4096 original
bytes on stdin through the same C++ `parse_native_cluster_configuration` used
by native instruments. It emits exactly eleven safe JSON fields (below), with no
drive-profile path/content. The original capability-checking CLI is unchanged.
`--native-summary-defaults` separately exposes the C++ compatibility defaults
for component verification; manager omission does not invoke a helper or turn
those defaults into explicit configuration.

The manager admits this optional interface once during construction, before
serving requests. Sealed admission requires the exact `native_cluster` role,
matching display UID/GID, bound `build/headunit-vehicle-config-check`, and the
existing full manifest/hash/mode/owner checks. Original bounded input bytes go
to the checker, eliminating a second pathname-based parser read. Named input
identity and the helper's identity/hash are checked across validation. Unsealed
source fixtures require owned regular single-link private configuration and an
owned executable without foreign writes. The ten-second startup subprocess is
reaped on failure/timeout. This adds no worker, service, thread, queue, provider,
module, configuration flag or release inventory item; the native role already
requires that checker. Retained predecessor release graphs remain verifiable.

| Key under `native_instruments` | Meaning |
| --- | --- |
| `configured` | Whether the receiver declares `native_cluster_config`; true does not imply valid/admitted/applied. |
| `state` | `configured`, `not_configured` or `unavailable`. |
| `source` | `source_configuration` when declared; otherwise `compatibility_defaults`. Neither means measured/fitted. |
| `configurationSha256` | SHA-256 of original admitted configuration bytes, or null. Identifies configuration, never acquisition/clock/presentation/applied receipts. |
| `configuration` | Fixed C++-validated object when admitted; otherwise null. |
| `configuration.temperaturePrecision` | `whole` or `one_decimal`; static coolant/oil numeric display intent, independent of units and speed precision. |
| `configuration.pressureUnit` | `kpa`, `bar` or `psi`; static native oil-pressure text/label intent, independent of other units. |
| `configuration.speedPrecision` | `whole` or `one_decimal`; native digital speed display intent with bounded optional glyph fitting. |
| `configuration.speedUnit` | `kmh` or `mph`, as configured for native instruments. |
| `configuration.temperatureUnit` | `celsius` or `fahrenheit`, from the same `NativeClusterConfiguration.temperature_unit` enum; independent of `speedUnit`. |
| `configuration.fuelConsumptionUnit` | `liters_per_100km`, `us_mpg` or `imperial_mpg`; static native consumption display configuration, independent of other units. |
| `configuration.distanceUnit` | `kilometers` or `miles`, from the same `NativeClusterConfiguration.distance_unit` enum; independent of speed and temperature. |
| `configuration.speedMaximumKmh` | Canonical km/h display maximum, also when `speedUnit` is `mph`. |
| `configuration.rpmMaximum`, `configuration.rpmRedline` | Display parameters; valid zero redline remains zero. |
| `configuration.showRpm` | Boolean; false remains false. |
| `error` | Null or fixed `configuration_unavailable`, `validation_unavailable`, `admission_changed`; no raw paths, exception text or private content. |

Omission returns `configured=false`, `state=not_configured`,
`source=compatibility_defaults`, null configuration/hash/error. Existing C++
omission uses km/h, Celsius, kilometers, L/100 km, 260 km/h, 8000/6500 rpm and visible RPM for bench
compatibility; it is not a validated car specification. Declared invalid,
missing, foreign-role, replaced, tampered or unsafe inputs return
`state=unavailable` with null configuration/hash and a fixed error. Optional
summary failure preserves working manager/receiver/native paths.

The original admitted projection is immutable for the manager lifetime. Status
returns independent copies with no native configuration I/O, subprocess or
mtime refresh on the request/control thread. Disconnect and receiver retirement
do not invalidate configuration or confer application authority. A later disk
change cannot change the cached units; new configuration requires the existing
new-generation/startup qualification. This interface reports no current speed,
RPM, physical identity, fitted capability, phone unit, native applied state or
compositor receipt. Provider/CAF/phone protocol/GPU ownership, Sport and TripCar conversions remain
unchanged; the opt-in speed glyph behavior is scoped above. Acquired values and unavailable readings
retain their existing SI/optional contracts.

**For native UI:** keys are exactly the table above; actions, control IDs and new UI
pieces are **NONE**. This branch changes no Home/page/glass/GTK/style files.
Any future presentation should reuse established Home2 components and identify
these as configuration. A unit editor/persistence/live mutation is unsupported.

Before the consumption extension above, the combined compatibility integration preserved six required version-1 fields,
three independent optional fields (`temperature_unit`, `distance_unit`,
`drive_mode_profiles_file`), at most nine distinct fields and the 4096-byte bound.
The compiler accepts both optional unit keys together; omitted keys preserve its
original generated bytes. The existing private profile path remains a runtime
input, excluded from the summary. Explicit old configuration without the unit
options yields Celsius/kilometers independently of speed. Both units come from
the same C++ parser/enums used by the renderer. That predecessor required exactly seven safe summary fields; the consumption
extension required eight; speed precision now requires nine, retaining valid string enums, a Boolean and finite
numeric parameters. Missing/extra/duplicate/malformed or mistyped fields fail with the
existing fixed error. Original hash/role/helper/identity/mode guards, immutable
startup lifetime and ten-second kill/reap policy are unchanged. The only new
native UI interface for this compatibility delta is
`status.native_instruments.configuration.distanceUnit`; actions, control IDs
and new UI pieces remain **NONE**.

The merged-source baseline first demonstrated an admitted Fahrenheit/miles
configuration omitting `distanceUnit` and the nine-field combination rejecting
with the inherited eight-field limit. Both failures, actual checker/Manager and
sealed admission bytes are retained outside Git under
`20261014/codex-native-unit-compatibility/`. Combined checks exercise all 36
speed × temperature (omitted/C/F) × distance (omitted/km/mi) × profile combinations.
The original accepted temperature and distance text/pixel/expiry/withdrawal tests
remain byte-identical. A separate combined Fahrenheit/miles Cairo test compares
real production pixels, allows changes only within existing unit/value text,
and verifies independent acquisition expiry, withdrawal, nonrevival and source
replacement. This is source/component evidence; the model remains unpublished.

Compatibility evidence is retained separately under
`20261014/codex-native-units-status/`: before this delta, an actual Fahrenheit
config with a drive-profile path produced an admitted, working five-field
checker/Manager result but failed the new-field assertion. Its original config,
sealed role bytes, checker JSON, Manager status and hashes remain retained.
The imported baseline passes Mac 16/16; final same-source checks pass **Mac
16/16 (44.82 s)**, **strict Debian ARM64 GCC 16/16 (26.35 s)** and **Clang 19
ASan+UBSan 16/16 (57.71 s)**, with 227 Python cases per lane, no skips or retries.
The 14 checker/Manager cases include all twelve independent speed/temperature/
optional-profile combinations, exact safe-field/type/finite-value admission,
immutable copied status, strict sealed guards and real ten-second timeout
kill/reap while an unrelated child survives. Existing Cairo source expiry and
allocation checks remain unchanged; all 28 imported default/Celsius and
Fahrenheit pixel fixtures are identical. Linux native, Standard and fallback
callers compile only. The exact Mac portable-checker fixture exception and
Linux leak/suppression policies above remain unchanged. Actual container
INT/TERM/HUP cleanup passes, with serialized verified evidence before removal.
This is source/component evidence; the future root whole review, coherent
release gates and sole-owner immutable-generation recipe above still apply.

**Qualification and owner follow-up:** focused production parser/render callers,
actual checker-to-manager, strict receiver/release closure and failure/retirement
cases are retained outside Git under `20261014/codex-native-config-status/`.
Original qualified-base Mac baseline passes 9/9 CTests. The final same-code
focused Mac, strict Debian GCC and Clang 19 ASan+UBSan lanes pass 10/10 each,
including 11 new actual checker/manager cases and 207 Python cases in total per
lane, with no skips or retries. Linux also compiles the existing native/fallback
Wayland callers; no Wayland/GPU presentation fixture runs. Linux warning/leak
policies and existing external LSan suppressions are unchanged. Mac uses its
actual portable checker with only its exact fixture bytes substituted for the
appliance ARM header check; Linux exercises the actual ARM64 checker. The
initial sealed-fixture permission error is retained separately. Each lane keeps
source/logs/Testing/JSON/hash receipts outside its retired build/container.
The original baseline is separate from new interface evidence; no full combined
matrix, deployment, phone, CM5 GPU, optical or physical-car pass follows.
Future adoption needs root whole-source review, a new coherent required matrix
and matching source/helper/module/input/config/unit/capacity gates before the
sole phone-session owner publishes a new immutable generation. Frozen V88 is
untouched. On that future generation, compare status fields/hash with the exact
validated native role, verify omission and optional failure only in isolated
fixtures, then check main/touch/Home/Back/Climate/native fallback and both
streams/UI-Sync/audio/control with unchanged owners. Observe any actual native
unit presentation separately; phone units and physical fitting/applied-unit
acceptance remain parked. Never mutate a sealed file to conduct that check.

Portable parser/pixel checks, launcher cleanup, fallback arguments, single-file
compilation and sealed role relocation pass in the 136-suite run. Three focused
AppleClang ASan+UBSan suites pass. Both restored CM5 headless Standard preview
fixtures pass configured gauge construction, 18/18 synthetic H.264 frames and
invalid configuration rejection. The native parser/pixel check also passes on
the CM5. The 637-input source receipt at `c81afbf` is retained under the private
`iphone-gap-generic-configuration` evidence directory named in [control](AIRPLAY_CONTROL.md#protocol-repair-source--9-october-2026).
These are software-renderer/Wayland checks; no physical gauge,
phone acceptance, full-page unit conversion or complete layout-policy claim
follows.

## Display-only temperature unit — 14 October source

Optional `temperature_unit=celsius|fahrenheit` extends the existing bounded
version-1 native configuration. All six required fields above remain required;
omission is Celsius with identical existing pixels. The optional
`drive_mode_profiles_file` can coexist with this field. Duplicate, unknown,
empty, case-changed or other unit values reject before native surface ownership.
Direct C++ callers use `NativeTemperatureUnit::celsius` (default) or
`NativeTemperatureUnit::fahrenheit` in `NativeClusterConfiguration.temperature_unit`;
invalid enum values reject at the same renderer admission boundary.

Only existing coolant/oil numerical text and `°C`/`°F` labels change: Vehicle
cards, Full Gauges and the bottom readings bar in all seven native formats/pages.
Conversion is `C * 1.8 + 32`, then the existing `%.0f` numerical formatting
(including signed rounded zero); no fonts, colors, allocations, arcs or warning
policy change. Values such as 0 C → 32 F, -40 C → -40 F, 90 C → 194 F and
100 C → 212 F are presentation conversions. Gauge arc fractions still use
original Celsius and existing 150/180 C display maxima. Those authored scales
are not physical thresholds; temperature warning lamps retain their independently
acquired canonical state. No fitting or calibration is inferred.

Canonical `VehicleSample` readings, dates, source generation,
provenance, age policies and unavailable semantics remain Celsius. Original
independent expiry, explicit absence and withdrawal produce `--`, never 32 F
from a missing zero. Malformed/nonfinite/out-of-domain samples remain unavailable
under existing renderer validation. Ambient temperature has no existing native
allocation and gains no widget. CAF/phone/signed Ultra, Car, Sport, Trip and
physical providers are unchanged. The static sealed configuration is source
intent, not an observed applied-unit receipt or physical-unit declaration.

For native UI review: new configuration key `temperature_unit`, optional vehicle
compiler key `nativeCluster.temperatureUnit` (`celsius`/`fahrenheit`). Omitted
compiler key leaves generated native bytes unchanged. The source compatibility
integration adds only `status.native_instruments.configuration.temperatureUnit`
to the same-parser read-only summary and checker compatibility defaults, with
manager omission still null. There are **no new UI actions, control IDs, widgets,
GTK files, pages or styles**. Unit configuration is not applied state. Any future
settings presentation remains native UI and uses established Home2 components.

Focused evidence is source/component only, retained privately under
`20261014/codex-native-temperature-units/`. Original qualified `3670763` parser,
renderer, compiler/checker, model, acquisition and retirement checks pass on Mac;
14 original known/unavailable Cairo images across all seven pages are retained
with source hashes. The production renderer is independently compared with a
text-observed build of the same source calling real Cairo; exact numerical text,
labels and existing allocation extents are checked. Omission/explicit Celsius
must match defaults, and Fahrenheit differences must stay within actual original
and converted temperature glyph bounds, leaving arcs/lamps/other artwork identical.
Known values cover negative/zero/rounding and canonical bounds; original cache
expiry, withdrawal and successor-missing paths and invalid config/staging cleanup
are exercised. Final same-source checks pass **14/14 on Mac (28.51 s)**, **14/14 on strict
Debian ARM64 GCC (13.88 s)** and **14/14 on Clang 19 ASan+UBSan (41.96 s)**,
without skips or retries. All 14 default images byte-match the original baseline;
14 opted-in Fahrenheit images are retained, with Vehicle/Full Gauges reviewed.
Linux Standard/native/fallback callers compile only; no compositor runs. Stock
Linux leak checking and external fontconfig/Cairo suppressions are unchanged.
Mac retains external deployment-target linker warnings. Two early snapshot/test
build failures and a case-sensitive Docker absence-audit failure remain separate
from the passing runs. Actual INT/TERM/HUP cleanup passes 130/143/129; all own
build trees and containers are retired after evidence/hash verification.
No GPU/compositor/phone/car/fps/deployment claim.

After whole combined manager review and a new coherent required matrix/helper/
configuration/input/unit/capacity closure, **phone-session alone** may select a
new immutable generation. Check the original role bytes/hash and enum defaults,
Celsius omission versus explicit opt-in on actual native fallback/Standard pages,
known labelled simulation, unavailable versus zero, independent temperature
expiry/withdrawal/successor retirement and readable labels in all formats. Observe
that arcs/warning lamps retain Celsius policy, and preserve Home/Back/main touch,
Climate, both panels/video streams/UI-Sync/audio/control and source clocks/receipts.
Never edit a sealed config or inject fixtures into selected live vehicle input.
Phone-unit policy, physical readings, optical fit and actual applied presentation
remain separately unqualified.

## Design and code layout

One 1920 × 720 canvas is fitted without distortion into any output. Its named
regions (`cluster_layout` in `include/headunit/native_cluster.hpp`) are the
only geometry: the renderer draws inside them and the tests check against them.
Instrument colors live in one `tone` table; telltale geometry, color and inset
are shared with Ultra in `indicator_symbols.hpp`/`.cpp`. Type sizes live at their call sites; the file
reads top to bottom as shared-state readers, drawing vocabulary, components
(rings, pills, telltales, pages, bottom bar) and frame composition.

- **Instruments:** a 270° ring per side with a large digital reading — speed in
  km/h on the left, engine speed with its redline on the right. Gear and drive
  mode are labelled pills below, shown only when their sources report them.
- **Status:** active dashboard symbols in one row at the bottom, using the same
  original artwork, colors, sizes and slots as the example Ultra warning overlay.
  Each of the 24 indicators
  has a fixed slot. Off and dark flashing phases leave it empty; unavailable
  direction readings show faint neutral dashed arrow outlines while other unknown warnings stay hidden.
  Direction arrows sit at the top at x=816/1040, y=16; the centre gap matches Ultra's
  odometer space and remains clear even with all lamps active. An explicit marker identifies
  simulated or unavailable vehicle data.
- **Bottom bar:** above the telltale row, media (or phone status on the Media page),
  next-turn guidance on Map, and fuel/coolant/oil/battery readings.
- **Map:** the renderer produces a premultiplied ARGB layer whose center stays
  transparent; scrims darken only behind the instruments and the bar. The
  Standard cluster window places waylandsink on a desynchronized subsurface
  *below* that layer, so the map spans the whole width. Every other page, and
  the phone-free backup, is opaque.

V50 combines the shared row with the later phone timeline fix; the intervening
stable-timeline V49 had deployed the older top-row renderer. Viewed native Trip
fallback and live Ultra/Map captures now show the low-beam glyph at exactly the
same physical coordinates beneath the information bar. All 24 symbols are
compared with the Ultra tiles in component tests. Input remains simulated;
physical lamps and live Standard/map clearing are not qualified.
[Warning-overlay qualification](ULTRA_WARNING_OVERLAY.md#combined-deployment--v50-10-october)
owns the exact capture and test scope.

Both presenters draw at output resolution (the backup previously drew at half
resolution, which clipped small labels). The full-canvas backdrop is cached
per output size. On the CM5 a 1920 × 720 frame costs about 4.5 ms and the
visible backup uses about 13% of one core at 20 Hz (V11's half-resolution
renderer: 16%). The backup is paced by compositor frame callbacks: covered
by Ultra or Standard it receives none and redraws only once a second.

Media titles and artists wrap at words into at most two lines. Remaining
overflow uses an explicit ellipsis at whole UTF-8 boundaries. Playback progress
remains tied to the admitted dated source and is omitted when timing is
unknown. Missing vehicle/Trip/heading values continue to show `--`.

Evidence (8 October): strict Clang and CM5 GCC `-Werror` builds; the native
cluster suite passes on both, including region containment, the transparent Map
center with full metadata, opaque other pages and the existing freshness,
artwork and state-file checks. On the CM5 screens, the new backup rendered
under labwc, and a generated 1920 × 720 map stream replayed through the actual
Standard cluster path (`headunit-video-replay … wayland-cluster`, 300/300
frames) appeared full-width beneath the instruments. Neither is a phone session
or a sealed release; the selected V15 release retains V11's previous
renderer. `headunit-native-cluster-snapshot OUT.ppm --backdrop MAP.ppm --center
map` composites the Map layer over any 1920 × 720 image for design review.

For a simulated icon review, add `--indicator-fixture all` or a comma-separated
selection such as `--indicator-fixture low_beam,left_turn`. This snapshot-only
option does not change the live vehicle adapter or introduce a startup bulb
test. Portable pixel checks exercise all 24 symbols, their fixed slots and
clearing; the icon revision is not yet selected on the CM5.

The selected page is a small private text file. Change it without restarting
the display:

```sh
python3 tools/cluster_page.py --state /path/to/cluster-page.txt media
python3 tools/cluster_page.py --state /path/to/cluster-page.txt vehicle
python3 tools/cluster_page.py --state /path/to/cluster-page.txt trip
python3 tools/cluster_page.py --state /path/to/cluster-page.txt next
# The Standard CarPlay manager exposes Map only while its session is active.
python3 tools/cluster_page.py --state /path/to/cluster-page.txt --map-available map
python3 tools/cluster_page.py --state /path/to/cluster-page.txt --map-available next
```

The native phone manager has a **Cluster** page with buttons for the currently
available pages. It also accepts `cluster_page` actions through its local
manager protocol; a future steering-wheel adapter can call the same selector.
The source UI uses compact full-card touch targets in two columns, with a
checkmark only for the manager-confirmed page. Unavailable pages are hidden.
The native pages fit the 1280 × 720 main panel; a single scrolling viewport
also holds longer Ultra style catalogues, with bounded authored labels.
The standalone CLI's `--map-available` flag is for a controller that has
already verified the live Standard session. When the receiver is disconnected,
the background renderer resolves a saved Map selection to Media, so it never
shows an empty map page. The CM5's persistent configuration specifies the
media-state file and page-state file; see [configuration](CONFIGURATION.md).

The renderer admits page state only from a stable regular file of at most
16 bytes. Its media snapshot is bounded to 32 KiB. Both reads open without
following symbolic links, and reject pipes/devices, incomplete or replaced
files. Invalid page state retains the connected Map or phone-free Media default;
invalid media leaves the native instruments available with no track information.

Media strings use libplist's explicit byte length before being copied. Embedded
NULs, control characters, malformed UTF-8 and fields over 2048 bytes are rejected
individually; the remaining current metadata and instruments continue. In
particular, an escaped NUL cannot trigger the allocating libplist string
accessor's assertion or turn a filename prefix into accepted cover metadata.
On 9 October 2026, the strict portable native-cluster suite passes fresh bounded
JSON fixtures containing escaped NULs, surrogate escapes, controls and invalid
UTF-8 across title/artist/album/status/artwork fields. This is component evidence,
not CM5 deployment or phone qualification.

The Map directions card now distinguishes known full-turn, Keep and Slight
artwork, plus Continue, U-turn, start and arrival. Route-status cards clear
directional arrows and name arrival/loading/finding/updating. Portable Cairo
pixel regressions and an inspected synthetic preview pass; complete maneuver,
lane and roundabout geometry remain open. See [route guidance](ROUTE_GUIDANCE.md).

The current bench uses simulated vehicle values. It does not infer car sensor
state from CarPlay. Only active telltales are drawn; absent values remain
unavailable. The media bridge prefers bounded local iAP2 snapshots and uses
the paired phone's BlueZ AVRCP metadata when that iAP2 channel is absent. A
live CM5 reconnect supplied title and artist through Bluetooth while a song
played in CarPlay. The bridge clears the shared state when neither source has
a track. Album artwork is shown only when supplied and validated by iAP2; the
Bluetooth fallback did not provide artwork on the tested phone.

The native renderer also verifies the published `artwork_id` against the exact
bounded cover bytes before drawing them. A cover is limited to 1 MiB and a
1–1024-pixel PNG in each dimension; Cairo decodes the same captured bytes after
the header and digest checks. It does not reopen the pathname for decoding.
Cover replacement before metadata publication, a missing cover or a mismatching
digest clears cached artwork while preserving valid track text. A subsequent
coherent publication recovers it. File/hash checks run at the existing 250 ms
metadata refresh or a cover-key change; an unchanged verified hash reuses the
decoded image.

On 6 October, strict portable builds and actual Cairo pixel regressions passed
for valid media/page state, FIFO/directory/link/oversized rejection, old-hash
cover replacement, coherent recovery, disappearance and cache invalidation.
The FIFO originally blocked a current-source snapshot process; the repaired
renderer returns its useful native page. Separate ASan/UBSan renderer checks
found no issue in the renderer/test changes; core/external dependencies were
uninstrumented and leak detection was disabled. These are local
component checks, not CM5 physical fallback, live artwork or deployment evidence.

The Trip page reads the shared dated `TripState` and `HeadingState`; it does
not accumulate distance/time from the display clock or infer travel course
from Apple Maps. Average speed names its supplied denominator; heading names
true/magnetic north and vehicle orientation/course. Source reset/counter
history is validated before rendering. [Trip/heading contract](TRIP_AND_HEADING.md).
Card values and units fit together within their allocation, including the
maximum supported duration and distance. A portable pixel regression verifies
that consecutive seconds remain distinguishable for a near-ten-year duration;
the previous fixed-size text clipped those final seconds.
Portable state/selection and native-pixel checks pass separately from
the existing live Map/Media/Vehicle evidence. The actual phone-manager action
accepts Trip, preserves Map availability during Standard CarPlay and retains
Trip after disconnect; all eleven portable management cases pass.
Trip has not been qualified on
the physical CM5 screens or with actual car measurements.

The 4 October native UI refinement was rendered and exercised with actual
macOS GTK at 1280 × 720 against an explicit fake manager. Media/Vehicle/Trip
fit the viewport, Trip click/confirmation succeeds, and an 18-style long-name
catalogue can scroll to its final choice without exceeding the window.
Existing phone/audio/camera actions and resident simulated
renderer/readiness ownership checks pass. Four related portable CTest suites
also pass. The same selector/action fixture also passes on actual CM5 GTK/V3D
under an isolated headless compositor. This is source UI/component evidence; it has not replaced the
selected V3 release or qualified touch on the physical CM5. The native cluster
renderer itself is unchanged by this settings-page refinement.

## Pre-phone auxiliary consumer seam — 14 October source

`tools/prephone_consumer.py` supplies a fixed eight-row consumer for the
version-1 projection from manager-accepted `prephone-content-model` source
`3e2f14c`, now integrated into this source branch. This is a reviewable admission
seam, accepted at `af4aaca`. Its original standalone milestone added no runtime
publication. The [optional native-owner bridge](#optional-native-owner-publication--14-october-source)
now uses this exact consumer; the selected release and frozen V85 matrix remain
separate from this source.

The intended adapter is an existing admitted C++ `VehicleObservationReceiver`
(or cache) snapshot → its eight-row JSON projection encoded off control/VM/GPU
callbacks → a copied model in the existing manager acquisition owner.
`SportSource.decode` currently validates only sport values; other HVO compound
fields remain opaque. Its simulation is an independent sport fixture, not the
native gauge source. Neither is authority for publishing these rows. The broker
forwards bounded original HVO bytes and deliberately has no last-value cache;
adding a second reader or collector would not solve this admission boundary.

The standalone milestone lacked current publisher/vehicle generation admission,
terminal retirement and same-host clock scope. The bridge below supplies those
facts from the existing native role and its admitted C++ sample. JSON alone,
file mtime, phone readiness and new polls remain insufficient authority.

### Caller and lifetime contract

Construct `PrephoneConsumer` on the existing owner thread. It performs no I/O
and samples no clock; all supplied times are actual positive signed 64-bit
nanoseconds in this host's C++ `steady_clock` domain. The platform mapping is
`CLOCK_MONOTONIC_RAW` on macOS and `CLOCK_MONOTONIC` on Linux, as in SportSource.
Different boots/clock domains need fresh authority and a fresh consumer, not an
estimated offset. An opaque caller-established publisher token is bounded to
128 printable ASCII characters; it is not read from JSON and is not a
cryptographic identity.

| Call | Contract |
| --- | --- |
| `begin(publisher, generation, started, now, clock_domain)` | The existing owner has already admitted this source. Domain must be `host-steady`; generation is positive uint64 and strictly increases for this consumer lifetime. Start cannot precede the last accepted source publication or exceed `now`. A genuine delayed successor start may precede a later local poll. Starts clear rows to awaiting-source; they do not carry previous readings. |
| `accept(raw_bytes, publisher, now)` | At most 8192 ASCII JSON bytes, exact version-1 schema, eight ordered catalogue rows. Source generation/start must match explicit admission. Publication cannot reverse or be future. All validation and row transitions finish before commit; rejection changes no model, clock, policy or watermark. |
| `snapshot(now)` | Returns an independent fixed-size copy. Expiry is derived at `now` and permanently withdraws that acquisition; original source publication and per-row acquisitions are unchanged. |
| `retire(publisher, generation, now)` | Only the current admitted owner can retire. All rows become disconnected/null immediately, preserving dates/origins. Late callbacks cannot reopen that generation. A genuinely newer explicitly admitted generation starts missing and requires new data. |

Owner-thread and host-operation time checks apply to every call. Age policies
are fixed after the first accepted model and survive generation changes.
Acquisitions cannot reverse/disappear within a generation. Equal-date value,
origin and explicit available/unavailable conflicts reject atomically;
withdrawn/stale data cannot return available at the same acquisition. Explicit
unavailable history survives per-row disconnect, so it cannot become a stale
measurement without a new acquisition. Identical current heartbeats may repeat;
they create no new freshness. A newly admitted delayed row that expires in
transit is stale/null. Invalid data leaves the previous model to expire under
its original policy; it creates no new failure or fault-clear reading.

Only qualified `admitted` and terminal `disconnected` projection states are
accepted. C++ diagnostic models (`awaiting_source`, `undated_source`,
`invalid_snapshot`, `generation_mismatch`) are not replacement readings. Before
qualified data arrives, `snapshot` returns `awaiting_source` with null source
identity/dates and missing rows. This seam does not replace complete HVO
validation or the original receiver's sequence/capability/trip history checks.
The projection has no packet sequence; it cannot prove transport liveness from
identical bytes. Actual publisher loss must call `retire`, independently of
per-reading expiry.

### For native UI review

The standalone consumer adds no UI action/widget/page. The bridge adds only
`status.prephone_vehicle` data. The consumer retains the [exact projection keys, row IDs, groups and
canonical units](#for-claude-review-exact-data-keys-and-row-ids) above. Future
adoption should reuse existing Home2 grouped-list components. Unknown fitted
state remains unknown; no fitted-equipment field is supplied by this schema.
Do not label absence as “not fitted” or “no faults”.

### Focused qualification and exact integration follow-up

The registered `prephone-consumer-python` check exercises the real parser and
consumer using explicitly simulated source clocks/JSON. Its required test-only
C++ executable emits six bounded snapshots through the actual cache → HVO4
codec → receiver → summary encoder, then the same consumer checks original
dates, expiry, terminal retirement and fresh-generation recovery. It covers
known zero,
negative/measured and calculated values, missing versus observed unavailable,
independent exact expiry, delayed delivery, immutable policies, atomic invalid
last-row rejection, source/publisher replacement, terminal retirement,
equal-date conflicts/nonrevival, unavailable history, mutation isolation,
64-bit dates and hostile bounded JSON. This is component evidence only; it does
not exercise a socket/file publisher or establish working manager publication.
Final scoped macOS Debug qualification passes **5/5 CTests in 1.39 s**:
23 Python cases including the actual C++ JSON fixture, the six-scenario summary
suite, and existing cache/codec/receiver suites. There are no skips or retries.
The initial interop fixture compile failure (optional parse result used without
checking/dereferencing) is retained separately; no test ran in that attempt.
The normal installed libplist macOS-27-versus-target-26 linker warning remains;
compiler `-Werror` and pinned clang-format 19.1.7 are unchanged. All three own
lane trees were removed after source/Testing/log/CMake hash verification; no
container was created. No new Linux/sanitizer/full-matrix or transport run is
claimed for this consumer; the accepted C++ model's earlier scoped Linux evidence
above remains separate.
Qualification receipts remain outside Git under
`20261014/codex-prephone-publication/`; the final coordination note owns counts
and retained source/build-retirement identity.

The consumer's original scoped checks are historical component evidence. The
bridge below separately qualifies actual IPC and existing manager consumption;
whole-source review and a complete coherent matrix still precede admission into
a new immutable release. The original consumer follow-up contract remains:
The sole phone-session owner performs the subsequent bench check:

1. Use an isolated explicitly admitted simulation, never inject into the selected
   live physical-input endpoint: zero fuel, negative outside temperature,
   calculated range and missing odometer. Match the actual source generation,
   clock scope, publisher and release identity at both ends.
2. Repeat consumer polls and source heartbeats; original acquisitions stay fixed,
   each row expires independently at its actual age limit, and observed absence
   remains distinct from missing. File/poll dates cannot renew a row.
3. Retire while readings are fresh; all values withdraw immediately. Reject late
   old publications. Admit a genuine successor starting missing, reject mixed
   generation/domain/publisher, then recover only with new admitted data.
4. Retain original dates and source/release/transport receipts while checking
   Home/Back, Climate/settings, main/touch, all native formats/fallback and both
   connected panels/streams/UI-Sync/audio/control. GTK adoption is native UI.

No car/provider calibration, physical sensing, display, phone, camera/audio
continuity, physical wake or fresh-fps acceptance follows from this seam.

## Optional native-owner publication — 14 October source

`PublishingVehicle` forwards the **same single** `VehicleSource::sample(now)` in
`src/cluster_wayland.cpp`, then copies the fixed eight-row projection. The same
adapter is passed to `run_local_cluster`, so native fallback and phone-free
signed sampling share this publication route. It does not bind another vehicle
socket, read hardware, own a display/radio, or change the returned sample.

The native callback sends a fixed **344-byte** packet through a connected,
nonblocking private Unix `SOCK_SEQPACKET` connection. It performs no file/JSON
work, logging, worker startup or waiting. One attempt per 200 ms maximum, one
receive/send per attempt and a one-second reconnect gate bound optional work.
Backpressure skips that copy; no pending user-space queue is retained. Existing
source/render queues and deadlines are unchanged. The existing supervised
manager owns bounded decoding, the accepted `PrephoneConsumer` and status JSON.
There is no storage child/thread, disk snapshot or join to stall native shutdown.

The private `HUPN001\0` packet is fixed, little-endian and versioned; it is not
HVO, an Apple message or a portable remote-host clock protocol:

| Byte offset | Field |
| --- | --- |
| 0 / 8 | Eight-byte magic / 32-byte manager-issued nonce. |
| 40 / 48 | uint64 transport sequence / actual native copy monotonic nanoseconds. |
| 56 / 64 / 72 / 80 | uint64 summary state / vehicle generation / original start / source publication. Unknown identities/dates encode zero and remain null in JSON. Summary codes: admitted 0, awaiting 1, undated 2, invalid 3, mismatch 4, disconnected 5. |
| 88 + 32 × row index | Availability byte (available 0, missing 1, unavailable 2, stale 3, disconnected 4), origin byte (measured 0, calculated 1, simulation 2, absent 255), six zero reserved bytes, IEEE754 double, uint64 original acquisition, uint64 immutable maximum age. Absent value encodes zero with its unavailable flag; genuine zero remains available. Row order is the existing catalogue. |

Exact size, reserved bytes, enum/number/date bounds, nonce and history reject
atomically before a model becomes usable. No socket buffer is resized.

### Admission, clocks and retirement

The default-off receiver setting `prephone_publication_socket` must equal the
sealed manager's `stateDirectory/prephone.sock`, an exact path of at most 103
bytes in its real display-owned 0700 directory. Rendering uses one shared
`native_cluster_command` for the exact native role and admission. The private
0600 endpoint and exclusive lock are manager-owned. Only a new reviewed managed
release can admit it; legacy manual manager launches cannot enable this route.
The optional inventory seals **both** `prephone_native.py` and the unchanged
accepted `prephone_consumer.py`; omitted modules, foreign paths and removed or
changed native flags fail release/unit validation. Disabled commands and module
inventory remain equivalent to the baseline.

Before accepting data, Linux `SO_PEERCRED`, `pidfd_open`, exact `/proc` executable
and argv, and matching kernel time namespace admit the configured display user's
native role. A live pidfd and socket HUP/RDHUP checks protect process death/PID
reuse; per-message credentials reject an inherited or transferred writer. The
manager issues a fresh 32-byte nonce for one current connection. Every packet
must echo it with a strictly increasing sequence and original native copy date;
second owners are rejected. No claimed PID/token in a payload is authority.
An old connection cannot update its successor. This is local role admission,
not protection against a privileged or same-account attacker modifying code.

Linux C++ `steady_clock` and Python `CLOCK_MONOTONIC` share this qualified host
and time namespace. The actual kernel boot ID scopes manager lifetime. There is
no wall-clock conversion, clock offset, remote-host or phone-clock inference.
Vehicle generation/start, source publication, per-row acquisition, immutable age
and provenance come from **the same admitted C++ snapshot**. The manager observes genuine monotonic time after admission and **each successful
receive**, then advances its checked frontier for lease and status dispatch. A
copy acquired after poll start but before receipt is valid; a genuinely future
copy or reversed consumer clock retires it. `poll()` has no supplied timestamp
argument, so synthetic test clocks cannot silently mix with real receipt dates.
IPC sequence and
`copiedNs` date publication work only; they never refresh a reading. Each row
expires at its own original limit, even during progressing IPC.

The consumer independently validates original row/policy/history transitions.
Malformed/foreign/mixed/replayed packets terminally retire the current
publication, with null values. Peer exit/HUP or a copy age of **two seconds**
retires every row immediately when observed by manager poll/status, including
long-age readings. Status rechecks death/lease at dispatch. This publication
lease is separate from source freshness and existing renderer deadlines.
Source withdrawal remains each original `disconnected` row with acquisition
history; genuine newer source generations start missing and need admitted data.
Native source exceptions close IPC before propagating their existing failure.
INT/TERM/HUP and normal native destruction close IPC; manager shutdown retires
and closes its endpoint/lock, preserving a replacement inode. No child is added.

`awaiting_source`, `undated_source`, `invalid_snapshot` and
`generation_mismatch` never supply usable rows. The existing `--simulate` source
is legacy/undated: publication labels it `undated_source` instead of fabricating
acquisitions or a vehicle generation. Actual socket HVO snapshots carry dated
simulation/calculated/measured provenance. There is no qualified physical car
provider, calibration or independent upstream terminal receipt: native source
silence causes original-age expiry, not a fabricated hardware-disconnect claim.
Optional connection/admission/configuration failure retains useful native/main/
Home/Climate/phone behavior, with `publication_unavailable` data if setup fails.

### For native UI: status keys and UI pieces

| Interface | Values/meaning |
| --- | --- |
| `status.prephone_vehicle.configured`, `.state` | Default `false/disabled`; setup failure `true/publication_unavailable`. Configured models also report source diagnostics, `admitted`, `owner_retired`, `publisher_expired`, `protocol_failure` or `clock_failure`. Failure/retirement never grants usable values. |
| `.version`, `.clockDomain`, `.sourceGeneration`, `.sourceStartedNs`, `.sourceSampledNs`, `.rows` | Existing version-1 catalogue and original source metadata. Dates/generation/ages are exact decimal strings or null. Missing source identity stays null. |
| `.rows[]` | Existing `id/group/label/unit/availability/value/acquiredNs/maximumAgeNs/origin`; IDs `fuel_percent`, `range_km`, `battery_voltage`, `ambient_c`, `coolant_c`, `oil_c`, `oil_pressure_kpa`, `odometer_km`. Groups `energy`, `temperatures`, `engine`, `distance`. Canonical units/labels above are unchanged. |
| `.publisher.live`, `.sequence`, `.copiedNs`, `.hostBootId`, `.error` | Actual current role/IPC lifetime, delivered sequence and original copy observation; kernel boot scope and bounded admission error. This is neither car/provider readiness nor reading freshness. Nonce/PID/argv are not exposed. |

**New UI actions, IDs for controls, widgets, pages and GTK changes: none.** native UI
may use established Home2 grouped-list components and style. Treat missing,
observed unavailable, expired and disconnected distinctly; real zero/negative
values stay real. Do not infer fitted equipment, fault clearing, actuator state,
phone readiness or fresh frames. Existing manager-reply freshness still applies.

### V87 selected native publication — 14 October

Manager-reviewed source3670763 and new helperV11 are selected in immutable V87;
the complete format/macOS/Linux GCC/ASan+UBSan/full-Skia GCC/ASan matrix passes.
The exact sealed `stateDirectory/prephone.sock` opt-in uses the existing native
renderer. Actual eight-second before/after samples (37 each) show original copy
dates and sequences progressing, `undated_source`, eight missing/null rows and
null vehicle generation/start/publication. Legacy displayed simulation is not
a dated reading or physical car provider.

One deliberate manager stop/start withdraws its old socket; the same native
renderer reconnects with the new manager. The other four service owners remain
unchanged, with zero automatic restarts; the socket is display-owned0600 and the
exclusive manager lock is root-owned0600, as admitted by the existing contract.
Initial observer incorrectly requiring display ownership for that lock failed
before any service stop; its script/samples/failure remain retained separately.
Selected payload/helper/units and the successful boot gate reverify afterward.
Private original status/endpoint/owner receipts are under
`20261014/codex-phone-session/v87-native-publication-second/`.

Viewed compositor screenshots show useful native Home and native fallback gauges
with an explicit SIMULATED label. Home retains a stale manager socket-error notice
after successful status recovery; that defect and signed local warm handback
remain open. Current CarPlay is disconnected,
PCM unavailable and USB syslog reports no device. This qualifies local undated
publication/endpoint retirement, not phone acceptance, dated HVO deployment,
GTK adoption, optical/physical output or audible behavior. Existing native UI
interfaces above are unchanged; the isolated dated-value/expiry fixtures remain
component evidence.

### Focused checks and sole-owner bench recipe

Final portable Debug qualification passes **12/12 CTests in 13.70 s**.
Clang 19 Linux ASan+UBSan passes **14/14 focused CTests in 21.33 s**; final
admission-FD hardening separately passes the actual owner/options **2/2 in
10.97 s**, including 14 real kernel/process/manager cases. All final
non-Markdown bytes match the last Mac and Linux-owner input snapshots. There are
no retries or skips in these focused runs. The existing libplist-27-versus-Mac
26 target linker warning is retained; no compiler/leak/timeout exception was
added. A stock unused-field compile failure and sandbox permission fixture
failure remain retained. The two deterministic healthy-copy clock regressions
fail on the original pre-poll-time code and pass with genuine post-receive time.

Reports/source/Testing/logs/failed attempts and hash-verified retirement receipts
remain private under `20261014/codex-prephone-native-publication/`. Every own
rebuildable tree/container is retired; actual Docker INT/TERM/HUP cleanup passes
130/143/129. Shared dependencies/Skia/private/selected inputs remain untouched.
No full matrix, GPU-enabled local signed caller, compositor, car or phone run is
claimed; the integration owner qualifies those together before admission.
Fixtures use actual
C++ cache/HVO4/`SocketVehicle`/forwarding publisher → kernel IPC → strict consumer
→ real `Manager.request(status)`. They cover copied-model isolation, original
zero/negative/calculated/missing data, independent expiry, withdrawal, genuine
source/owner replacement, stale-buffer death, live stall, send-EAGAIN recovery,
wrong role/second owner/inherited writer, signals and endpoint replacement.
Actual native caller compilation/options and exact sealed-module/role failures
are separate component checks. No GPU presentation/physical/phone result follows.

After manager whole-source review and a **new coherent complete required matrix**,
phone-session alone qualifies a new immutable V86-or-later generation/helper:

1. Keep V85 frozen/sealed inputs unchanged. Add only the optional private socket
   setting to the new receiver role, matching its manager state directory. Verify
   new module inventory, exact native command, payload/helper/unit/source binding
   and current capacity/reserve before selection; no live CLI override.
2. With the ordinary legacy simulator, require `undated_source` and eight null
   rows while native/local gauges remain useful. Check late manager start and
   manager restart: the same source/render owner reconnects using a fresh nonce,
   and old rows cannot survive retirement. This case does not prove dated values.
3. Run the retained isolated HVO fixture with its own private sockets/processes,
   never inject into the active car/phone input. Verify known zero, negative
   outside, calculated range, missing odometer, original dates and exact expiry;
   withdrawal/new source, publisher stall/death/late bytes and successor start
   missing. Available deployed rows need an actually admitted dated native HVO
   source plus its complete fan-out/configuration closure, separately qualified.
4. On the connected new generation preserve main/touch/Home/Back/Climate,
   native formats/fallback, both panels/video streams/UI-Sync/audio/control and
   genuine source clocks/receipts. Retain scoped original status/lifecycle data;
   GTK/physical sensing, wake timing, camera/audio continuity and fresh60 remain
   separate open acceptance work.

## New opt-in dated synthetic rows — 14 October, source only

The existing native vehicle owner may now explicitly select `--simulate-dated`
(saved strict receiver flag `simulate_dated_vehicle`, default false). This
separate synthetic acquisition source feeds the same native/local source and
optional eight-row publisher, with actual source generation, original per-field
dates, immutable ages and `origin="simulation"`. It adds no physical provider or
Trip/heading/IMU/GPS/fitted data. Legacy `--simulate` remains undated and correctly
publishes eight null auxiliary rows. See the
[acquisition/lifetime and adoption contract](VEHICLE_OBSERVATIONS.md#explicit-dated-native-simulation--14-october-source-only).

For native UI: **new UI keys, row IDs, actions, controls, widgets and pages: none**.
Existing `status.prephone_vehicle` keys and the eight row IDs above are unchanged.
Only the new configuration/CLI names require integration review. Reuse the
established Home2 grouped rows and style. Display simulation from actual row
provenance; an unavailable value alone cannot establish active fixture mode,
fitted equipment or a physical provider. Expired/retired copied rows retain
simulation origin with null values. No GTK/Home/phone-page/style assets change.
This source has no live Car-page, native-display, phone or physical acceptance;
new combined qualification/helper/configuration closure precedes owner adoption.


## Optional indicator publication — 14 October source-only

Default-off `prephone_indicators: true` requires the existing sealed
`prephone_publication_socket` equal to the private manager endpoint. The exact
native role gains `--prephone-indicators`; missing, extra or duplicate flags
reject before admission. The optional inventory requires
`prephone_indicator_status.py` plus **both** existing pre-phone modules, and
both ambient modules when ambient is independently opted in. Missing/partial
module groups reject. The positional `inventory(..., prephone_enabled)` meaning
is preserved; ambient and indicator options are keyword-only. Omission/false
retains the previous command and inventory and imports no indicator module.
Old retained graphs remain verifiable. The flag/status key describes software
intent, never fitted/provider/applied state.

`PublishingVehicle` takes the same one original `VehicleSource::sample(now)`.
At the existing maximum 200 ms copy cadence, `PrephonePublisher` projects every
opted model from that same sample and original copy date. The eight-row
`HUPN001` encoder, size **344**, API and version are unchanged. The new bounded
encoder performs fixed binary work, without JSON/disk/VM/GPU work, another
acquisition, connection/socket, service, thread, provider, user-space queue or
clock mapping. Linux role/argv/executable/credentials/pidfd/time namespace/nonce
admission and the original two-second carrier lease/reconnect policy remain.

### Fixed private binary contract

`HUPI001\0` is exactly **872 bytes**, little-endian, version **1**, with exactly
24 catalogue-ordered 32-byte records. No text or arbitrary IDs arrive on wire.

| Offset | Field |
| --- | --- |
| 0 / 8 | Eight-byte magic and the existing 32-byte connection nonce. |
| 40 / 48 | uint64 sequence and original native copy nanoseconds, shared with the corresponding vehicle record. |
| 56 / 64 / 72 / 80 | uint64 established summary state, original source generation/start/publication. Zero absent identities/dates become null. |
| 88 / 90 / 92 | uint16 version 1, uint16 row count 24, uint32 exact length 872. |
| 96 / 97–103 | Clock marker (0 unqualified, 1 host-steady), seven zero reserved bytes. Available source metadata requires marker 1. |
| 104 + 32 × index | Availability byte (available 0, missing 1, unavailable 2, stale 3, disconnected 4); origin (measured 0, calculated 1, simulation 2, absent 255); canonical value (OFF 0, ON 1, FLASHING 2, absent 255); exact catalogue index byte; twelve zero reserved bytes; uint64 original acquisition at +16; immutable maximum age at +24. |

Exact type/length/version/count/index/reserved bytes, enums, nonce, sequence,
copy/source/acquisition chronology and 24-hour maximum age are validated before
atomic packet admission. The accepted pure model remains the C++ source gate.
The existing native carrier labels legacy data `undated_source`, preserving
null identity/acquisitions/values; it does not invent a generation to admit it.
The receiver independently retains fixed per-row sequence/acquisition/value/
origin/withdrawal and immutable-policy history. Equal-date repeated copies do
not renew observations. Stale/withdrawn acquisitions cannot return current at
that date; a genuine matching successor begins missing.

A lamp record must match its preceding accepted vehicle sequence, copy date,
state and original source tuple. A bounded poll can stop after a new-generation
vehicle record but before its lamp record, or an optional lamp send can be
skipped under backpressure. Status immediately returns old lamps as null and
`generation_mismatch`, retaining **their own** historical source/acquisition
identity. It never copies the vehicle's new dates into old rows. Usable values
wait for a matching new source record. Invalid/reversed packets retire the
connection and all projections; valid source records admitted earlier in the
poll retain their historical dates. Independent packet admission is not a
multi-record transaction.

`NativePublication.observations()` is one internal triple-copy boundary.
Manager status calls it once when indicators are enabled: **at most 16 total
successful recvmsgs across vehicle, ambient and indicators**, one actual
post-poll checked clock, and independently deep-copied publisher metadata.
The final actual kernel peer-liveness check withdraws **all three** if death
occurs between any copies. It does not poll again or admit a replacement.
The existing external `snapshots()` still returns the original two-item
`(vehicle, ambient)` tuple; `snapshot()` and `ambient_snapshot()` shapes/actions
are unchanged. A standalone indicator snapshot also observes once.

### native UI read-only status

`status.prephone_indicators` adds no action, control ID, widget, page, GTK piece
or appearance policy. Rows reuse the immutable 24 IDs/labels/colors in the model
catalogue above. Colors imply no fitted severity; hazard is only an original
canonical input. Beam/position/fog observations make no day/night decision.

| Key | Meaning |
| --- | --- |
| `configured`, `version`, `state` | Software opt-in, version 1, and source/model state. Disabled is false/disabled with empty rows; setup failure is true/publication_unavailable. Before a packet: awaiting_model. Admitted/undated/source diagnostics and owner_retired/publisher_expired/protocol_failure/clock_failure retain their precise scope. |
| `clockDomain`, `sourceGeneration`, `sourceStartedNs`, `sourceSampledNs` | Admitted original host-steady source domain and original source identity/publication, decimal strings or null. Historical old identity is retained on a generation mismatch. |
| `renderedNs`, `copiedNs`, `publicationSequence` | Shared actual post-poll projection date, this indicator record's original native copy date and independent sequence. These are not acquisition or presentation receipts. |
| `rows[]` | Exactly 24 configured model rows: `id`, `label`, `color`, `availability`, `value`, `acquiredNs`, `maximumAgeNs`, `ageNs`, `origin`. Values OFF/ON/FLASHING or null; age is projection minus original acquisition. Known OFF is available; dark animation never changes FLASHING. Unavailable rows never claim zero or fault clearing. |
| `publisher` | Independent copy of the existing shared carrier's `live`, `sequence`, `copiedNs`, actual `hostBootId`, bounded `error`. Its latest vehicle copy can be newer than this indicator record; this does not refresh lamps. Nonce/PID/argv remain private. |
| `error` | Bounded setup/admission/protocol/clock error or null. No provider, physical link, applied state or phone readiness is inferred. |

Disabled/setup-failed status has version 1, null source/render/copy/publisher
fields and empty rows; it imports no optional decoder. Configured packet models
retain all 24 catalogue rows after retirement. Existing manager-reply freshness
still applies. Simulated observations say `origin: simulation`; source dates,
copy dates and rendering dates remain separate. Stale redraws are not observed
frames. Legacy simulation has 24 null values.

### Qualification and future sole-owner adoption

The first accepted-pair/model baselines pass Mac **12/12 (20.12 s)** and strict
Debian ARM64 GCC **15/15 (30.06 s)** before implementation. The first feature Mac
attempt retains three failures: a new fixture expected an unadmitted legacy pure
model to be undated instead of awaiting admission (two CTests share it), and a
receiver fixture omitted required display configuration. The native legacy
carrier diagnostic and fixtures were corrected; no admission generation, bound,
timeout, warning or sanitizer policy was relaxed. Final identical **813 non-Markdown inputs** pass Mac **14/14 (21.11 s)**,
strict Debian ARM64 GCC **18/18 (48.98 s)** and Clang 19 ASan+UBSan
**18/18 (69.86 s)**. Each Linux lane runs all thirteen new actual-owner cases
(17.98/18.44 s), with no skipped cases or retries. Stock compiler warnings,
Linux leak checking and the existing external font/Cairo suppressions remain.
Native Wayland/return callers compile; GPU execution is unrun. An initial GCC
owner 30-second timeout and the named diagnostic's malformed-length/EOF fixture
failures remain retained. The corrected fixture keeps a real socket live during
length rejection and a root-held writer open during actual PDEATHSIG/waitpid;
the original 30-second limit is unchanged. Current backend INT/TERM/HUP plus
repeated signals during guarded cleanup passes 130/143/129. All own build trees
and fourteen own containers are absent after independently verified serialized
retention; earlier runner manifests retain their original scope/log-growth
annotations. Exact results and cleanup identities are retained outside Git under
`20261014/codex-prephone-indicator-publication/` and in the final coordination
receipt. Source/native caller compilation is not GPU or appliance execution.

The fixtures exercise production cache/HVO/SocketVehicle/publisher → genuine
kernel credentials/pidfd/nonce → real Manager endpoint. They retain OFF versus
absence and FLASHING independent of phase; original TTL, exact portable expiry,
source withdrawal/new generation, queued record-16 generation boundary with
10-second lamp policies, blocked new indicator copies, atomic invalid last row,
reserved/truncated/oversized/replay/clock faults, separate deep copies and real
child TERM/wait between all projections. Existing paired/action/volatile policy,
backpressure stale-admitted barrier, retirement/signal/lease/role/config/release
checks remain. The fixture's explicitly requested Linux PDEATHSIG and real
adopted waitpid qualify that isolated test process path, not a new appliance
parent-death policy. Portable supplied clocks/malformed clock packets are
labelled test inputs, not invented live receipts.

Root whole-combined review, all required coherent release lanes (including the
held V88 GTK repair), exact matching new helper/module/configuration/input/unit/
asset closure and fresh capacity precede **any** adoption. Future phone-session
already containing accepted model840 and ambientf5 cherry-picks only the new
bridge commit, then alone publishes a new immutable generation. Never edit
selected V87 or held V88 inputs in place.

1. Enable only the new generation's strict receiver flag and existing private
   publication socket. Verify exact native argv/module/unit/hash/helper binding
   and capacity. Keep the same owners and existing Climate/Home/formats/fallback.
2. Ordinary legacy simulation must report undated/null source identity and 24
   null lamps. In isolated dated cache fixtures, verify OFF/FLASHING/missing,
   original acquisitions/TTL, withdrawal and generation changes. Do not inject
   fixtures into the selected vehicle/phone input.
3. Observe actual native death/stall/owner-close, queued records and successor
   admission; all three projections withdraw together and policy resets volatile.
   Match original dates/publisher scope, never redraw/copy time as acquisition.
4. In the newly qualified connected generation, preserve both panels/video,
   UI-Sync/audio/control, main/touch, saved profiles, Home2, Climate and native
   fallback. GTK/appearance, physical/fitted car inputs, optical display/phase,
   wake, audio continuity and fresh fps remain separately open. No physical
   acceptance follows from this source bridge.
