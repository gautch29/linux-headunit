<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Vehicle lights above an Ultra cluster

## V84 refinement qualification scope — 14 October

Reviewed refinement is deployed in frozen3bd3db9; the full required matrix
passes. Six isolated CM5 CPU fixtures render lit/mid-fade/off/hazard/unknown/stale
states, including larger green geometry and dashed unknown outlines. PNG display
copies preserve original PPM pixels, verified by hashes. Socket fixtures verify
source freshness/replay/expiry independently without injecting the live graph.

Live observed-phase precedence/expiry/recovery, bright-map/all-style GPU cost,
p95 and physical lamps remain explicitly parked. CPU pixels, active services and
simulated fallback do not establish those results. Original fixture receipts are
private `20261013/phone-session/v84/native-turn-fixtures/`.

Draw receiver-owned vehicle indicators above any signed cluster layout. The
overlay consumes the same timestamped sample as the Ultra gauge bridge. It does
not require the asset to implement a light, and does not modify signed resources,
the phone's video or the main panel's input mapping.

## Current-pack indicator inspection — 13 October source

Read-only inspection of the retained `44C9FA31EC / 985` signed resources finds
turn-signal input support in `VehicleDataUpdater.lua`, but no turn-signal instrument
declaration in `r/PackageData.lua`. The pack's `Indicator.lua` presents local
notifications / RequestContent, not top direction lamps. Runtime input support
alone is not authored arrow artwork. Keep the existing original shared overlay;
no signed resource is edited, copied into Git or re-signed. The inspection summary
and exact archive hash are private in `20261013/codex-cluster-signals/`.

### Lamp rendering refinement — 14 October source, V84 target

native UI's delegated design uses a solid ISO-style green shaft/head arrow, shared
in `indicator_symbols` by native and Ultra. The standard-slot symbol is 25% wider
than its predecessor, using `#2BD45B`, crisp grayscale antialiasing and a subtle
low-alpha green bloom extending roughly 1.5 times the symbol radius. The whole
arrow brightens together; there is no sequential sweep. Slots and signed inputs
are unchanged. This is source/component work, not a new bench qualification.

`TurnSignalAnimation` is a pure display-owner state machine: cubic smoothstep
rise 40 ms, decay 120 ms. `TurnSignalDisplay` exposes two intensities,
`unavailable` bits and `animating`. Native owners pass its frame through
`ClusterContent::turn_signals`, raising their ordinary 20 Hz refresh to 50 Hz
only while easing. Ultra uses the same state machine and pre-rasterized 32-step
brightness tiles. Its bounded 34×34 immutable scene table includes off and
unknown; animation updates select retained pointers with no C++ heap allocation.
Only non-turn warning/badge topology or output resizing rebuilds that table.
No per-frame blur, gradient, filter or rasterization is added to Ultra. Existing
GPU rendering, source polling and actual presentation receipts retain their owners.

Explicit fresh per-sample lamp `on`/`off` is authoritative. Only `flashing`, or an
active hazard summary without observed lamp phases, uses the shared monotonic
1.5 Hz / 50% duty fallback (90 cycles/minute). The integer nanosecond phase has
three cycles per two seconds without accumulated millisecond rounding. A fresh
explicit phase on one arrow supplies its missing/flashing hazard partner; two
explicit observations remain authoritative even if they disagree. This changes
effective display availability, never the individual sensor `known` bits.
Fresh hazard Off/expiry restores independently dated direction inputs.

Unknown, stale, future, invalid and retired source data immediately removes
all lamp energy and shows a faint neutral dashed outline of the same arrow.
It never fades as known Off. Known Off has an empty slot after the short decay.
Initial availability rises gently and an initial dark fallback sample has no
invented prior lamp energy. Source generation changes and backward animation time
reset presentation history.
No animation, redraw or phase conversion changes acquisition dates, per-signal
age policy, availability or provenance. Startup Hide suppresses outlines as
well as lamps; disabled/omitted slots draw nothing. Stateless native snapshot
calls without a supplied animation frame draw the instantaneous shared policy;
use the explicit transition-history fixture below to review easing.

Review evidence belongs in private
`20261013/codex-cluster-signals/telltale-refinement/`: native and overlay lit,
mid-fade, off, hazard and unknown snapshots plus matrix receipts. The refinement
is for the next V84 integration; V83 qualification is not interrupted.

### Refinement component qualification — 14 October

Qualified source `7abfedf` is pushed on `development branch` for
V84 integration. Pinned clang-format 19.1.7 passes; macOS 157/157, default Linux
GCC 185/185, Clang ASan+UBSan 185/185 and full Skia GCC/ASan 186/186 nonhardware
checks all pass. All 721 non-Markdown inputs match all five frozen snapshots.
Stock compiler/warning/test/leak/timeout policies are unchanged. Every lane
runs through the shared host build slot; own build trees and containers are
retired after source, logs, Testing, CMake and JSON receipts are retained.

The allocation regression has a positive capture control and verifies zero C++
heap allocations across warm animation updates. Its replacement new/delete
functions share a separate translation unit so optimized GCC retains matching
C++ allocation semantics without a warning exception. Earlier Mac 155/157 and
156/157 fixture-date/duplicated diagnostic-clock failures, and the initial
optimized test-helper compilation failure, remain separately retained. The
schematic diagnostic backup now also uses the shared fallback cadence.

Actual isolated Linux HUV sender/consumer runs preserve ingress dates and
simulation provenance, record 11 left/10 paired-hazard phase transitions and
verify immediate no-data energy withdrawal after sender expiry. MCU simulator
self-test passes 17 typed socket checkpoints. CPU snapshots cover lit, mid-fade,
Off, paired hazards and unknown in the native cluster and overlays on all six
retained signed CPU style bases; native stale is also captured. These are
explicitly simulated renderings, without phone video or presentation receipts.

Optimized Linux CPU API cost: constructor 13.521 ms, first ordinary-lamp scene
prepare 0.253 ms, batched steady lookup 0.408 µs. Ten real-clock flashing-bottom-warning
state changes average 0.212 ms and peak 0.380 ms. Process resident memory rises from
4952 KiB before construction to 13144 KiB after, 14312 KiB after prepare, and stays
within 14312–14384 KiB during the three-second flashing run with an earlier tree
retained. This is bounded CPU/cache evidence, not whole-frame GPU cost or fps.
The V84 owner must still measure actual GPU uploads, warm/p95/outlier frame
cost and fresh 30 fps with both outputs, turn fades and flashing bottom lamps.

Private evidence is `20261013/codex-cluster-signals/telltale-refinement/`, including
`qualification.json`, exact source/snapshot hashes, CPU/RSS receipt and final
bench plan. No CM5 action, signed-pack mutation or GTK restyling is performed.
Real phone/GPU appearance, bright-map readability, startup/fallback/handover and
physical car providers remain unqualified. Keep V83 unchanged; adopt this only
in the fully qualified V84 successor.

## Configure the overlay

Copy [the example](../config/ultra-warnings.example.json) into the receiver's
private configuration directory. Add its absolute path to the installed-UI file:

```ini
warningOverlayProfile=/home/headunit/.config/ultra-headunit/ultra-warnings.json
```

The installed-UI file must also select its existing vehicle source, either
`simulateGauges=true` for the bench or `vehicleSocket=/absolute/consumer.sock`.
Do not enable both. Omitting `warningOverlayProfile` disables the overlay;
`enabled=false` in the JSON also disables it. Configuration is loaded and
validated before the receiver listens and takes effect on receiver restart.

The file is a bounded, owner/root-owned regular file, cannot be a symlink and
cannot be writable by group or others. The JSON schema rejects duplicate or
unknown fields, unknown/repeated signals, overlapping slots and out-of-bounds
geometry. All coordinates are logical pixels in the overlay's own canvas.

| Field | Meaning |
| --- | --- |
| `schemaVersion` | Integer `1`. |
| `enabled` | Boolean. |
| `hideDuringStartup` | Optional Boolean, defaults to `true`. Hides the complete overlay, including the simulation badge, during observed STARTUP and LOGOS and before a scene is available. Drive restores current fresh lamps. `false` keeps lamps visible during the animation. |
| `canvas.width`, `canvas.height` | Canvas dimensions; each is at most 4096. The example uses 1920 × 720. |
| `maximumAgeMs` | Freshness threshold, 50–2000 ms; example 500 ms. Does not renew a sample's timestamp. |
| `simulationBadge` | Required `x`, `y`, `width`, `height` rectangle. Shown only for a fresh sample marked simulated. |
| `indicators` | Up to 24 entries, each containing a `signal` key and a `frame` rectangle. Omitted signals are not drawn. |

Indicator tiles are 64–180 pixels wide and 48–96 pixels high. The simulation
badge is 64–180 pixels wide and 20–96 pixels high. Slots are fixed: turning off a
light never shifts other lights. Current source places the 64 × 56 direction
arrows at (816, 16) and (1040, 16) on the 1920 × 720 canvas. Other warning lamps
retain their bottom slots at y=648 and the centre gap for the signed cluster's
odometer. Fresh active hazards without observed phases flash both top arrows at 1.5 Hz;
there is no separate hazard lamp. The optional `hazard` profile entry remains
accepted for compatibility, but is never drawn. Both direction entries must be
configured to display both hazard arrows. V67 introduced these positions, retained by selected V68;
top arrows and paired flashing are observed with the phone across all six styles.
Both renderers use the same glyph inset
and green/blue/amber/red palette. The native information bar sits above the row.
Custom profiles remain supported; inspect their positions against the selected
layout's map, gauges and labels. Slot
validation prevents collisions within the overlay, not collisions with artwork
inside a signed asset.

To expose this choice in native **Settings → Warning lights during startup**, add
an external, private preference path to the installed-UI file:

```ini
startupWarningLightsFile=/home/headunit/.config/ultra-headunit/native-home/state/startup-warning-lights
```

The manager atomically saves `hide` or `show` in this owner-only configuration
file. The existing cluster display owner polls it at most twice per second;
valid choices apply without restarting CarPlay and persist across reconnects
and reboots. Until a valid choice is saved, `hideDuringStartup` supplies the
default. Missing, malformed, unsafe or symlink updates keep the display's last
valid choice. Neither preference changes vehicle observations, their acquisition
dates, lamp-test inputs or the native/Ultra driving row. This is a cosmetic
startup policy; the car remains the source of actual warning states.

The 24 signal names and their colors are defined in `indicator_definitions` in
[`vehicle.hpp`](../include/headunit/vehicle.hpp). They cover turn/hazard signals,
exterior lights and engine, brake, restraint and other warnings. Colors follow
that shared table. Original symbol artwork in
[`indicator_symbols.cpp`](../src/indicator_symbols.cpp) is shared with the
native cluster. Geometry is defined in `indicator_layout` in
[`indicator_symbols.hpp`](../include/headunit/indicator_symbols.hpp); the example
profile is checked against it. Lights have no descriptive text, card or border; the conventional
`P` and `ABS` remain part of their brake symbols. Their font outlines use grayscale
antialiasing without device-grid hinting so native rendering and the overlay's
2× tile rasterization retain the same appearance. Physical car providers and
production artwork qualification remain open.

## Feed values

Use the existing [vehicle interface](VEHICLE_INPUT.md). Each datagram is a complete
snapshot, and a missing indicator is unknown. For example, a private consumer
can receive:

```text
HUV2 speed_kmh=42 engine_rpm=1800 low_beam=on left_turn=flash check_engine=off simulated=1
```

The entire sample is validated before publication. The overlay shares the last
accepted `GaugeInput` sample, so it never binds or drains a second socket.
Separate processes still need distinct consumer sockets; one datagram socket
is not a multicast vehicle bus.

| Input | Visible result |
| --- | --- |
| Fresh `on` | Configured light appears. |
| Fresh `off` | No light. |
| Missing/unknown | Top direction slots show faint dashed arrow outlines; other lamps are hidden. No state becomes known off. |
| Fresh `flashing` | 1.5 Hz / 50% duty using the shared monotonic phase, with lamp easing. |
| Fresh `hazard=on` or `hazard=flash` | Observed lamp phases take priority; otherwise both configured arrows alternate together. |
| Stale, future or invalid sample | All lamps and simulation badge clear; configured direction slots show faint dashed arrow outlines. |
| Fresh `simulated=1` | Simulation badge appears, independently of active lights. |

An invalid source update faults the gauge adapter before publication; its existing
worker/fallback policy remains responsible for retiring the affected Ultra view.
An unchanged source timestamp cannot refresh a light. The overlay does not infer
warnings from speed, fuel or temperature thresholds: the vehicle adapter must
publish those indicators explicitly.

Hazard display policy applies only while that observation remains fresh under
its own age policy. Once it expires or turns Off, independently fresh left/right
readings control their respective arrows again. Displaying an arrow from a
hazard reading does not mark its individual sensor state known or change any
acquisition date. Startup hiding and stale clearing apply to the top arrows too.

## Composition and ownership

`WarningOverlay` rasterizes small transparent symbol tiles once on its display owner.
Active-symbol, brightness and no-data tiles are cached; image and scene objects remain immutable.
Turn animation selects prebuilt trees; non-turn topology/output changes rebuild
the bounded table. Settled state reuses the whole tree.
No complete frame is rasterized or read back for the overlay. The GPU caches
and composites the tiles with the rest of the scene.

`GpuDisplayOptions::overlay` supplies an output-space scene just before GPU
composition. `with_scene_overlay` places it above the entire mapped asset,
outside the asset's clipping, opacity, masks and filters, without changing the
script's parent links. Only the installed **cluster** display enables the
callback. Output resizing scales the overlay's canvas independently of the
signed asset canvas. The combined scene uses the existing video leases, GPU
completion guards and actual presentation receipts.

## Verification and remaining gates

Portable tests exercise the 23 displayed symbols, paired hazards, transparent tile edges, drawing-state
preservation, invalid geometry rejection, off/unknown/stale/future clearing,
flashing, simulation identification, fixed positions, immutable cached trees,
resizing, disabled/subset configuration and malformed profiles. Cross-renderer
pixel checks compare the 23 native glyphs with the Ultra tiles over the same
background and check that their frames match, arrows stay above native content
and warning lamps stay below it. Hazard checks cover bright/dark synchronization,
individual-signal restoration, aggregate/per-signal expiry and unchanged source dates.
CM5 GPU tests
compare the complete overlay with the independent CPU renderer, check that
repeated scenes do not upload symbols again, and check that clearing leaves no
old pixels. Display tests exercise the callback with real compositor receipts
and the existing clock suspension/recovery rules.

Offline checks do not qualify placement over every real phone map or signed
style. Remaining live gates are: simulate each indicator over Ultra, change
styles without reconnecting, clear and expire input, and verify that the main
picture/touch stay intact. Physical sensor data and production artwork remain
unqualified. Keep the feature status in [the goal](GOAL.md) and
[README](../README.md) separate from those acceptance checks.

### Top arrows and paired hazards — source follow-up, 11 October

Four scoped suites pass on macOS AppleClang (8.48 s), Debian arm64 GCC
(3.45 s) and Clang 19 ASan+UBSan (21.05 s, leak detection on with existing
external Cairo/fontconfig suppressions). They cover native cluster rendering,
Ultra overlay pixel parity, vehicle indicators and independently dated
observations. The transparent all-lamps preview has been visually inspected:
arrows are above the instruments and other lamps retain their bottom slots.
The first Linux attempts failed fetching Luau; retries use the cached 0.739
archive after verifying its pinned SHA-256. Both failed receipts are retained.
Pinned format and diff checks pass. Private source snapshot/logs are under
`~/.cache/headunit-qualification/20261011/top-arrows-component/`.

The source-only boundary above was superseded by V67 deployment. native UI's
real-phone qualification shows top left/right arrows and paired hazard flashing
over Map and all six phone-confirmed styles, with native phone-free arrows also
captured. [HANDOFF](HANDOFF.md) owns the exact
selected bindings and receipts. All-lamp expiry/clearing, remaining startup
cases and physical vehicle qualification remain open. Earlier bottom-row results
below describe their own retained generations.

### Bottom-row qualification — V49, 10 October

Retained bottom-row V49 freezes `20613b38ad6188706965dbc5e89175f713d6b1c9` and enables the
example overlay in its private installed-UI configuration. All 750 source inputs
are attested. Local Linux GCC passes 171/171 tests and the full Skia receiver
ASan job passes 172/172 with Linux leak detection on. Five affected macOS checks
pass normally and under ASan+UBSan (Mac leak detection off). CM5 strict optimized
Clang 19 passes twelve scoped suites in 27.13 s, including actual offscreen GPU
composition, display retirement and native/overlay pixel parity. Five affected
CM5 ASan+UBSan suites pass in 11.53 s with Linux leak detection on and only the
existing external Cairo/fontconfig suppressions. The headless startup fixture
passes 21 checks in 55.696 s; it is synthetic, not a physical boot test.

An earlier Linux pixel comparison failed for ABS because of device font hinting.
The shared font policy fixes that failure without relaxing the comparison's
mean-channel-error threshold. Final checks compare every native glyph with the
same Ultra tile and preserve caller font/drawing state.

Six original signed layouts are rendered offscreen with an explicit fixed-DRIVE
simulation profile: Cobranded, DigiRing, Modular, DigiBandTicks, DigiBand and
MapsMedia. Their flattened captures are GPU-composed with all 24 lamps, two normal
lamps and cleared input. The fixed row fits beneath the pictured gauges and
leaves the centre gap unchanged. These captures have no phone video/map,
wallpaper negotiation or real vehicle data. The first fixture used the timed
session profile without a lifecycle publisher and produced blank base frames;
those captures are retained but excluded from visual acceptance.

After whole-owner selection, 41 manager samples over 40.660 s preserve both
pairings and the selected phone. All five services remain active with zero
observed restarts; sealed payload and installed-unit verification pass. Viewed
Wayland captures show the native Trip fallback, honestly unavailable Trip data,
simulated low-beam/front-fog symbols in the bottom row and the native Phones page.
The phone stays disconnected throughout; no live Ultra or physical touch pass
is claimed. Exact deployment and retained rollback are in [HANDOFF](HANDOFF.md).
Private evidence is under `headunit-qualification/20261010/cluster-telltale-row`
on the Mac and `/home/headunit/headunit-cluster-telltale-row-20261010` on CM5.

### Combined deployment — V50, 10 October

The later `cm5-20261010-stable-timeline-v49` selected source `3c84875`, which
omitted the shared row. A real native capture reproduces the lamps above the
instruments. V50 freezes merged source `b554463eea16d8e16ddcf26f59c01702c7f008d4`:
it retains that phone timeline fix and the complete row source from `20613b3`.
All seven row/configuration/parity inputs remain byte-identical to the earlier
qualified row. Both the standalone native service and the phone-manager fallback
use V50's independently built native binary; the installed Ultra configuration
enables the matching warnings role. Exact binding, checks and rollback are in
[HANDOFF](HANDOFF.md).

A real same-phone Disconnect → native Trip fallback → Connect completes with
both pairings retained. Fresh matching confirmed Ultra Ready is first observed
12.526 s after Connect and holds a ten-second tail without sampled loss. Viewed
1920 × 1080 output captures show native lights below the information bar and
live MapsMedia/Map lights in the same row. The bright low-beam glyph's bounds
are exactly `(266,845)–(300,870)` in both captures; its frame is
`(252,828,64,56)` after the 180-pixel canvas offset. Input is explicitly simulated.
All-lamp live injection, clearing/expiry and every live style remain separate
gates; the scoped pair of captures is not physical touch, car acquisition or
fresh-frame-rate qualification. Private evidence is `cluster-row-integration-v50`
on the Mac and `headunit-cluster-telltale-row-20261010/integration-v50` on CM5.

### Startup visibility — V55, 10 October

V55 freezes `02964c970505409ae9dbb14b2299860a11d90a0d`, preserving V54's Display
response controls and the complete shared row. `hideDuringStartup=true` and the
external preference path above are enabled. Runtime observes the original
signed workspace phase; STARTUP, LOGOS and unavailable scenes suppress the
complete overlay. Drive uses the current sample without altering acquisition
or flashing phase. Clearing drops cached containers without mutating earlier
submitted trees or uploading tiles again. Show retains normal overlay behavior.

Normal/ASan tests exercise hidden, shown and restored states, stale input after
startup, strict/default config and private preference refusal. A metadata-cache
fault discovered in Linux is corrected: same inode, size and modification time
cannot conceal different contents. Its deterministic regression fails against
the original reader and passes after removing that cache; reads remain bounded
to 17 bytes every 500 ms. Full local GCC (173/173), full receiver ASan (174/174)
and 23 CM5 scoped checks pass. Both native Settings choices save correctly and
fit the 1280×720 headless GTK preview, with or without Display response enabled.

Physical same-phone captures show startup branding × Apple with no lamps or simulation
badge, then live Drive/Map with the row restored. Eight inspected co-brand
frames have an entirely black reserved lamp row and badge rectangle. A separate
actual publisher/cluster check confirms a fresh, idle matching connection over
a ten-second tail. The recorder's incorrect uppercase readiness predicate is
excluded; corrected independent evidence is retained. Config, binding, rollback
and private receipts are in [HANDOFF](HANDOFF.md).
The car data is simulated. Live Show during logos, physical CAN and sustained
rate/touch qualification remain open.
