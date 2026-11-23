<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Qualify complete signed clusters

This record separates an authored layout, the vehicle values that drive it, and
the iPhone content behind it. It describes our independent host, not an official
Apple SDK. Signed resources and rendered captures stay private and unchanged.

## A concrete missing drive-mode label

The inspected cluster updater reads three semantic paths:

| Path | Meaning |
| --- | --- |
| `driveState.driveMode.identifiers` | Immutable array of authored mode identifiers |
| `driveState.driveMode.currentIndex` | Zero-based selection into that array |
| `driveState.driveMode.hidden` | Optional presentation visibility |

The installed artwork declares `GT`, `sport`, `individual`, `terrain`, `track`
and `wet`. The script converts the selected index to Lua's one-based array
index. Our local profile parser previously accepted only scalar fixed values,
so it could not express the required identifier list. Declaring a gear alone
does not supply the missing drive-mode label.

`LocalProfile` now accepts `kind: "array"` only for immutable `fixedValue`
configuration. Limits are 32 primitive items, 128 bytes per string and 1 KiB
for the complete OPACK encoding. Numbers must be finite and exactly
representable under the existing scalar policy. Nested arrays, dictionaries,
binary objects and timed arrays are rejected. Fixed arrays cannot bind a
vehicle field or accept later publications; they survive a disconnect as
configuration, never as a new measurement.

The bench generator exposes an explicit fixture:

```sh
python3 tools/create_cluster_bench_profile.py \
  --extended-readings --drive-mode-fixture sport \
  --output /private/cluster-sport-fixture.plist
```

The default generator does not declare a drive mode. This option adds the
authored identifier list and a **fixed simulated selection**. It does not read
the car or add a physical drive-mode field to `VehicleSample`.

A same-palette CPU comparison added 1,324 changed pixels confined to the Sport
label at `(1547, 418)`–`(1632, 453)` on the 1920 × 720 DigiRing canvas.
Parser/runtime tests check the exact array and selection seen by `CP`, fixed
state after disconnect, refusal to update it, and all value/size limits.
Generator tests exercise every authored selection and both plist encodings.
Observed transmission now has a typed acquisition/cache and HVO2 transport;
observed drive mode has a fixed capability catalogue, timed active selection
and explicit HVO3 transport. The fixed selection above still supplies no
observed drive mode. Physical acquisition remains separate adapter work.

## Feed an observed drive mode

`--drive-mode-config` loads bounded JSON semantic-to-artwork mappings and creates
a timed active index without choosing an active mode. The vehicle source
supplies one member of its immutable semantic catalogue, independently of the
artwork's order. Unknown, stale or valid-but-unmapped modes withdraw the scalar;
index zero is never an unknown-state fallback. See [drive modes](DRIVE_MODES.md).

The 2 October regular DigiRing CPU comparison maps `comfort` to GT and `sport`
to Sport. Exact RGBA differences are confined to the mode label. Reordering
the source catalogue preserves every pixel; unmapped `eco` is pixel-identical
to unavailable authored redaction. All five explicit simulated fixtures
return the signed entry points at 1920 × 720. This establishes local mapping
and rendering, not physical acquisition, live phone acceptance or other styles.

## Feed an observed gear separately from the drive-mode fixture

`--observed-transmission` replaces fixed `M`/third gear with timed
`transmission.selector` and `transmission.gear` mappings. The local profile
selects artwork values; the source supplies one coherent state with its
original acquisition date. Unknown or expired components withdraw their
script scalars. Gearbox technology never selects `M`, and RPM/speed never
determines a gear. [Transmission](TRANSMISSION.md) documents this boundary.

The 2 October regular DigiRing CPU comparison displays third and fourth gear;
changed pixels are confined to the right-hand widget region. Missing state
produces authored redaction. Its numeric-only slot intentionally switches to
the brand logo for a known nonforward position. Exact RGBA comparisons confirm
that Sport, coolant, range and ambient labels remain byte-identical in all
four snapshots. This qualifies these local fixture cases, not every layout
or state. These are explicit simulated snapshots without
phone video, not physical transmission or live style qualification. These
2 October fixtures did not modify the deployed CM5 profile.

## Feed observed current-trip consumption

The reference updater exposes a repeated `tripComputer.trip` service with
`userVisibleLabel` and `fuelEfficiency`. Its numeric input is **mL/100 km**
(unit 674); the canonical vehicle observation is whole-trip mean **L/100 km**.
The generator explicitly converts by ×1000 and selects display unit **671 =
L/100 km**, which the inspected widget converts separately. These are local
pack unit enums, not official phone vehicle-resource IDs or sensor encodings.

```sh
python3 tools/create_cluster_bench_profile.py \
  --extended-readings --observed-trip \
  --trip-instance currentTrip --trip-label "Current Trip" \
  --output /private/cluster-trip.plist
```

This adds the fixed same-instance label, timed consumption input and fixed
unit selector. Instance/label defaults are explicit bench configuration and can
be overridden; they neither choose a physical trip nor populate an iPhone
page. Without `--observed-trip`, the existing profile is unchanged. The generated
profile contains no fixed mean or guessed distance/time/average-speed/heading
Apple paths. See [the binding configuration](VEHICLE_INPUT.md#bind-observed-trip-consumption).

The typed source supplies one coherent current-trip observation, independently
of heading. Its identity, reset generation and cumulative counters have shared
lifetime validation across partial knowledge, loss and source restart. The
script adapter retains the original acquisition date; polls cannot refresh
consumption, and a new reset cannot replace totals at an old/conflicting date.
Undefined mean withdraws rather than selecting zero. A real zero mean requires
positive observed distance. Sticky loss requires a genuinely newer acquisition
for recovery; repeated expired raw snapshots remain historical, while newer
expired observations can update hidden counters without drawing a fresh value.

HVO4 socket/service and direct-source checks exercise those dates, unit
conversion, independent trip/heading expiry, atomic rejection and delayed
recovery. This establishes a local data boundary. It does **not** establish that
any chosen signed cluster actually contains this label, that its center selects
Current Trip, or that the phone supplies distance, time and average-speed rows.
Native Trip presentation is independent of that unresolved Ultra center route.

### Linux transport evidence — 4 October 2026

The isolated CM5 Linux broker passed 16 checks, including original HVO2/HVO3/HVO4
bytes, kernel receive dates, queued-input expiry, no replay to later consumers,
exclusive ownership, endpoint cleanup and production-process death/restart.
Only newly sent observations were forwarded after restart. These checks used
private sockets without a live vehicle or display service.

The broker treats HVO payloads as opaque bytes. This qualifies distribution,
not typed consumer parsing, physical acquisition, richer deployed bindings or
the phone's signed center composition. See
[vehicle observation evidence](VEHICLE_OBSERVATIONS.md) and
[vehicle distribution](VEHICLE_DISTRIBUTION.md).

## Inspect palettes without a phone

`headunit-theme-probe --describe-layouts` lists the signed palette catalogue as
`palette|identifier|displayName` rows, followed by the display/layout rows. The
reference and Single packs declare eleven palette identifiers:

`blue`, `custom`, `green`, `midnight`, `multicolor`, `orange`, `red`, `rose`,
`starlight`, `vibrant`, `yellow`.

For an isolated CPU layout probe, select a validated catalogue entry:

```sh
HEADUNIT_THEME_PROBE_GAUGES=1 \
HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT=8BC9ABB1-CB20-595E-92BF-D8F099BF3515 \
HEADUNIT_THEME_PROBE_CLUSTER_PALETTE=blue \
  build/headunit-theme-probe /private/reference-pack 44C9FA31EC 985 \
    /private/accepted-reference.conf \
    --layout-local /private/cluster-sport-fixture.plist /private/blue-digiring
```

Only `--layout` and `--layout-local` accept this diagnostic. Palette identifiers
are restricted to 1–64 ASCII letters, digits, underscores or hyphens. The
probe requires a syntactically valid, declared primary cluster UUID and an
existing palette with matching `id`; invalid inputs fail before the entry
point or handshake probe. There is no arbitrary script/path interpolation.

The diagnostic uses the signed configuration and State interfaces to seed the
selected layout's local palette before loading `main`. The inspected startup
controller reads its saved selection table before saving a startup palette
override; setting only that override left the initial local layouts using
their old defaults. Seeding the normal saved-state path makes the CPU
comparison exercise the requested palette. It does not change live persistence
or send a fabricated UI-Sync transition to a phone. The result identifies
`offlinePalette` and explicitly reports `livePhoneTested: false`.

### Offline matrix, 1–2 October 2026

All cases use the same explicit dark, extended-reading, Sport fixture at
1920 × 720, a simulated foreground/clock and the CPU renderer. These are
execution and local pixel checks, not CM5 performance measurements or proof of
the complete phone composition.

| Layout | Compared palette | Reference | Single |
| --- | --- | --- | --- |
| Cobranded | custom | Runs | Runs |
| DigiRing | blue | Runs | Runs |
| Modular | multicolor | Runs | Runs |
| DigiBandTicks | orange | Runs | Runs |
| DigiBand | starlight | Runs | Runs |
| MapsMedia | vibrant | Runs | Runs |
| SingleDigiRing | blue | Not declared | Runs |
| SingleDigiBandTicks | orange | Not declared | Runs |
| SingleDigiBand | starlight | Not declared | Runs |

An additional DigiRing sweep exercises the complete reference palette catalogue:

| DigiRing palette | Local result |
| --- | --- |
| blue, green, midnight, orange, red, rose, starlight, yellow | Runs with distinct authored colors |
| custom, multicolor, vibrant | Signed color lookup rejects the combination |

The rejected cases reach `Palette.getSolidColor` with an absent color entry.
A palette's presence in the global catalogue therefore does **not** establish
compatibility with every layout. The receiver does not replace missing colors
or rewrite signed code to make the combination appear supported. The private
report preserves the exact exception and successful PNGs; failed cases have
no retained PNG that could be mistaken for the new output.

The six shared reference/Single layouts are pixel-identical under matching
fixtures and palettes. The extra three Single declarations are actual
additional layouts, while a differently colored ring is a presentation
variant. Complete live Single style switching remains unqualified.

## What the offline canvas cannot establish

The signed LayoutController places a full-display input named `iPhoneVideo`
behind the local gauges. It starts with zero opacity and participates in the
phone's transition/foreground/video lifecycle. The local root background also
depends on coordinated appearance. A black CPU canvas has no phone image,
frame-timed transition or complete phone-side background selection.

The corrected palette sweep changes local colors and renders legible local
speed, gear and drive-mode values with the complete fixture. The empty center
still has no phone backdrop. A live comparison must coordinate palette,
appearance and phone video before judging the complete composition, including
the photographed landscape/gradient. A successful local render is not evidence
that the phone selected or supplied that wallpaper.

| Reference feature | Evidence | Remaining gate |
| --- | --- | --- |
| Speed/RPM, fuel, coolant, outside temperature and range | Existing typed bindings plus the richer simulated fixture draw these locally | Qualify the richer profile with the phone and real unavailable/stale values |
| Gear and drive-mode label | Typed transmission/HVO2 and observed mode/HVO3 with explicit mappings; native gear/mode badges and regular DigiRing gear, GT/Sport and unavailable CPU fixtures. Source catalogue order cannot select the artwork index. Linux broker distribution has the separate evidence above. | Qualify all-state signed label appearance, richer CM5 profile deployment and physical acquisition. Never call a fixture a car sample. |
| Blue landscape and warm backdrop | Matching gauge styles/palettes exist; signed full-display phone video is behind the gauges | Observe the phone's background/palette/appearance choice and resulting pixels |
| Media artwork/title/artist in the signed center | Native media model works; catalogue has center zones and request-content overlays | Identify and qualify the phone's center-content route; native media success is not Ultra composition success |
| Current Trip center page | Typed trip/HVO4 and native Trip presentation; explicit dated local `tripComputer.trip.fuelEfficiency` conversion. No separately declared primary layout named Current Trip. | Establish the phone center-content route and full composition; no distance/time/average-speed/heading Apple paths or physical trip sources have been qualified. |
| Car/lane assistance view | Signed DCA_ADAS explicitly names an input `adasView` | Supply a separate real assistance renderer/source or leave unavailable; it is not the Maps video |

The four discovered identity variants contain seven byte-identical
`r/Center_Display` resources. Swapping those variants offers no different
center-resource implementation. This comparison does not claim their hashed
images cannot contain relevant artwork, or that every phone-side resource is
known.

No missing custom-image-provider binding was proved to be responsible for the
photographed Media/Trip pages. Adding a guessed host API or protocol identifier
would not establish their path. The next useful live check is the complete
phone background/appearance and center-content exchange with one known layout,
then all nine Single layouts and live transitions.

## Private evidence and public boundaries

The local audit stores `palette-style-results.json`, rejection records,
same-palette drive-mode proof and CPU captures under the ignored
`captures/cluster-completeness-20261001/` directory. Asset identity verification
remains mandatory; the newly downloaded Single pack uses the explicitly
selected expired-leaf development policy described in
[asset verification](ULTRA_ASSETS.md). No signing-policy, runtime profile or
active phone session changed during this audit.

The later acquired-mode fixtures and exact comparisons are private under
`captures/drive-modes-20261002/`; that offline audit did not deploy its generated
profile or a new binary on the CM5. Trip/heading native fixtures and
local binding probes remain private under `captures/trip-heading-20261002/`;
they do not change that live qualification boundary.

Related records: [reference-image and live style review](CLUSTER_STYLE_REVIEW.md),
[asset discovery](ASSET_DISCOVERY.md),
[vehicle input](VEHICLE_INPUT.md), and
[Climate/settings qualification](CLIMATE_QUALIFICATION.md).
