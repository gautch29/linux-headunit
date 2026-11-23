<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Optional native head-up display

The HUD is an independent, optional third **local Wayland output**. It shows
large vehicle speed, an available speed-limit sign and the next navigation
maneuver/distance on opaque black. It has no touch or keyboard input and keeps
the same native composition in Standard, Ultra and phone-free operation.
The two phone-advertised panels and signed scene/stream contracts remain unchanged.

For this first implementation, turn guidance is preferable to a full map:
it needs less illuminated area and retains clear readings. Apple's native
[iAP2 guidance recommendation](https://developer.apple.com/videos/play/wwdc2023/10150/)
matches this approach. There is no third map/video consumer in this implementation.
A future map option requires an explicit available source and fitted optical
field of view; the existing cluster map is not silently captured or duplicated.

## Output and optics

Copy [hud.example.conf](../config/hud.example.conf) for the fitted installation.
`output` is required and must differ from `main_output` and `cluster_output`.
Use the same reserved names as the actual receiver display profile. An absent
or ambiguous HUD connector creates no surface; a withdrawn output retires its
surface. The program never asks the compositor to choose a default output.
A private runtime lock permits one HUD process per display/output pair, held
through surface teardown. Process exit, including SIGKILL, releases ownership;
the stable lock file is retained for the next invocation.

`width`/`height` define the requested canvas. Initial compositor configuration
may supply the actual output size (bounded to 4096 × 2160); the renderer fits
that size. A later geometry change retires the current pool. `fps` is a 5–30 Hz local
submission ceiling, default 20. This is not a claim of measured physical scanout.
The renderer fits a 960 × 360 design without stretching the instruments.

The default horizontal reflection is only a starting profile. An upward-facing
panel, rotated module or optical engine with its own reflection can require a
different axis or no additional reflection. Configure `reflect_horizontal`,
`reflect_vertical` and clockwise `quarter_turns` (0–3) against the actual reflected
image. `scale` (0.25–1) and normalized `offset_x`/`offset_y` (−0.5–0.5) position it.
Optional normalized TL/TR/BR/BL corner coordinates apply one projective correction;
all eight values must be supplied together. Nonconvex, singular, nonfinite or
out-of-bounds corrections are rejected, and pixels outside the corrected area
remain black. Transform order is rotation, reflection, scale/offset, then the
measured corner correction.

Four-corner correction addresses geometric skew, not an arbitrary curved
windshield. Optical focus, eyebox, ghost images, luminance and nonlinear
distortion depend on the fitted module, glass/combiner and driver's eye position.
[TI's HUD optical guide](https://www.ti.com/lit/an/dlpa073a/dlpa073a.pdf) describes
these distinct optical designs and the role of the combiner/mirrors. Hardware
selection, mounting, brightness control and optical calibration belong in the
hardware repository.
No universal windshield correction is assumed.

## Observations and phone lifecycle

The native model consumes `VehicleSample` through `current_vehicle_sample`.
Original acquisition dates, per-signal expiry and simulation origin remain
attached; zero speed is a real reading, and missing/stale speed shows `--`.
Use a distinct HUD consumer endpoint in the existing [vehicle fan-out](VEHICLE_DISTRIBUTION.md):
its `consumers` permits optional `hud` alongside `backup`, `receiver` and `ultra`.
Do not bind the HUD to any other consumer's socket. The broker still forwards
original observation bytes and has no replay cache or second state model.

The existing receiver's sole iAP engine receives navigation in both modes.
Optional receiver setting `navigation_socket` publishes its `NavigationSnapshot`
to the HUD without registering another phone service. It requires `route_guidance=true`.
The endpoint must be a canonical absolute Unix path in a private directory owned
by the display/receiver account. The launcher scrubs inherited publication paths.

HNG1 is a bounded 1,536-byte local datagram, carrying publisher epoch, session
generation, original accepted guidance-change date and a separate host send date.
The send date is a live-owner lease, not a new maneuver measurement. The publisher
sends nonblocking at most four times per second. Guidance remains available when
the phone is stationary and sends no deltas, while a healthy owner continues the
lease. Cancellation/disconnect publishes unavailable state; a killed or stalled
publisher expires at two seconds. Malformed, future, truncated and replayed packets
cannot renew the lease. Each endpoint has one owner lock, bounded draining and
safe dead-socket recovery; Linux checks the sender UID. Native Home does not stop
the HUD or change the connection mode.

## Speed-limit provider

The current iAP guidance model supplies no speed-limit field. Phone map video
can contain a speed-limit sign, but this implementation does not extract it or
infer a limit from vehicle speed, roads, maneuver text or cached map artwork.
An optional `--speed-limit-file` accepts a separate dated provider observation:

```text
hud-speed-limit-v1 value=50 unit=km/h acquired_ns=123 maximum_age_ns=1000000000 origin=measured source=provider
```

This line is a schema example; `123` is not a usable current date. A real provider
must write the original host steady-clock acquisition date, bounded age policy
and explicit origin/source. Publish atomically as an owned single-link `0600`
regular file of at most 512 bytes in an owned `0700` directory. Stale, future,
malformed or missing records clear the sign. Simulated limits are visibly marked.
No physical speed-limit provider, recognition camera or map database is implemented.

## Country and sign appearance

`country=auto` is the default. A fresh provider observation can carry the current
country together with the limit, using the same acquisition date, age and source:

```text
hud-speed-limit-v2 value=55 unit=mph acquired_ns=123 maximum_age_ns=1000000000 origin=measured source=road-map country=US
```

As above, `123` is a schema placeholder. `country` is an uppercase two-letter
code, or `unknown`; `unit` is `km/h` or `mph`. The reader retains the declared
unit and converts mph to internal km/h using exactly 1.609344. Country and limit
are acquired atomically from one bounded record. A location/map provider must
resolve the **current road's country**, including border changes, and publish
the observation. The HUD does not use the phone's language, SIM, timezone or
route destination as evidence of current country. There is no GPS/reverse-geocode
adapter installed in this implementation.

For an installation without that provider, configure a country such as
`country=FR` or `country=GB`. It is an explicit fallback, not detected location;
a fresh provider country takes precedence. A withdrawn, malformed or expired
observation clears both its limit and country; only the configured fallback
remains. Without a recognized country, a plain limit value with an explicit unit
replaces the road-sign shape. Unknown provider countries never inherit a guessed
national sign. `country=unknown` and legacy v1 records can use an explicit fallback.
Vehicle speed follows the selected country's km/h or mph display unit while
retaining its original SI value, acquisition date and origin internally.
Next-turn distance uses the same display unit profile: metres/kilometres for
km/h, and feet below 0.1 mile (161 metres) or miles thereafter for mph. Display
conversion retains the original route distance and its acquisition date; zero
distance remains a known value. Portable boundary checks cover both profiles,
including the largest bounded route distance.

| Profile | Appearance | Unit |
| --- | --- | --- |
| Supported European countries, New Zealand | Red ring, white face, black number | km/h |
| Great Britain (`GB`) | Red ring, white face, black number | mph |
| Sweden and Finland (`SE`, `FI`) | Red ring, yellow face, black number | km/h |
| United States (`US`) | White rectangle, black SPEED LIMIT heading and number | mph |
| Canada (`CA`) | White rectangle, black MAXIMUM heading and number | km/h |
| Australia (`AU`) | Red ring on a white rectangular panel | km/h |
| Unknown/unsupported country | Neutral LIMIT text and number | Declared provider unit, otherwise km/h |

The white circular metric profiles are `AT BE CH CZ DE DK EE ES FR GR HR HU IE
IS IT LT LU LV MT NL NO PL PT RO SI SK NZ`. These are stylized numeric HUD signs;
country-specific fonts, temporary/electronic variants, zone/end/unrestricted
signs and supplemental vehicle/time restrictions are not synthesized.
Appearance references are the [UNECE sign convention](https://unece.org/DAM/trans/conventn/Conv_road_signs_2006v_EN.pdf),
[France's B14 definition](https://www.legifrance.gouv.fr/codes/article_lc/LEGIARTI000049319395/2024-03-24),
[UK signing system](https://www.gov.uk/government/publications/know-your-traffic-signs/the-signing-system),
[FHWA R2-1](https://mutcd.fhwa.dot.gov/htm/2009r1r2/part2/part2b.htm),
[Ontario regulatory signs](https://www.ontario.ca/document/official-mto-drivers-handbook/signs),
[Swedish C31](https://www.transportstyrelsen.se/sv/vagtrafik/trafikregler-och-vagmarken/vagmarken/forbudsmarken/hastighetsbegransning/),
[Finnish C32 drawings](https://www.traficom.fi/sites/default/files/media/regulation/LIITE_4_Tieliikennelain_merkkiryhm%C3%A4n_C_liikennemerkkien_mitoitus%202025.pdf),
[Queensland speed signs](https://www.qld.gov.au/transport/safety/signs/regulatory) and
[New Zealand signs](https://www.nzta.govt.nz/assets/resources/motsam/part-1/docs/motsam-1-section-02.pdf).

## Build and run

The portable Cairo model, calibration, record reader and snapshot tool are built
normally. Linux Wayland builds also provide `headunit-native-hud-wayland`.
Inspect the composition and calibration without a compositor:

```sh
build/headunit-native-hud-snapshot /tmp/hud-readable.ppm --fixture --upright
build/headunit-native-hud-snapshot /tmp/hud-us.ppm --fixture --upright --country US
build/headunit-native-hud-snapshot /tmp/hud-reflected.ppm --config config/hud.example.conf --fixture
build/headunit-native-hud-snapshot /tmp/hud-grid.ppm --config config/hud.example.conf --calibration-grid
```

The fixture explicitly labels simulated readings. These images exercise the same
renderer as the resident presenter; a grid is a calibration aid, not a measured
correction for an unknown windshield.

Start it only against an explicitly fitted HUD or an isolated software compositor:

```sh
build/headunit-native-hud-wayland --config /private/hud.conf \
  --vehicle-socket /private/vehicle/hud.sock \
  --navigation-socket /private/navigation/hud.sock \
  --speed-limit-file /private/navigation/speed-limit.txt
```

All data options are optional; absence displays unavailable speed and hides
guidance/limit. `--simulate-vehicle` is an explicit mutually exclusive bench source.
The resident process consumes local dated observations only; it owns no radio,
phone selection, actuator or signed UI resources.

This feature is source-only. No HUD service or configuration is added to selected
V11. Its retained release schema does not yet seal an optional HUD program/config
graph; add that graph and qualify it before managed installation. Future hardware
has not been selected. A physical third connector, simultaneous three-output
operation, optics, daylight/night readability and actual phone-to-HUD behavior
remain unqualified.

## Verification

Portable checks cover data expiry/provenance, actual zero speed, navigation status
clearing, absent limits, bounded text, reflection/rotation and projective geometry.
`HEADUNIT_TEST_NATIVE_HUD_WAYLAND=ON` enables a nonroot Linux fixture with three
isolated software labwc outputs, explicit fake vehicle input and saved screenshots.
It checks exact placement, untouched main/cluster outputs, absent-output failure,
noninteractive presentation, released-buffer recycling, bounded submission cadence
and orderly process/output retirement. It also drives the actual navigation
socket through unchanged guidance leases, cancellation, recovery and publisher
loss, and checks duplicate-owner rejection and replacement after SIGTERM/SIGKILL.
This establishes local software behavior,
not physical output timing, optics, sensors or iPhone acceptance.

On 8 October 2026, the full portable build and **129/129 CTest checks passed**.
CM5 GNU 14.2 Debug builds of the full receiver and HUD passed in the isolated
`/home/headunit/headunit-hud-20261008` workspace. The seven scoped Linux tests passed:
HUD model, configuration, navigation socket, three-output Wayland fixture,
receiver configuration, vehicle fan-out and route guidance. The Wayland fixture
uses headless labwc/pixman, with a 1280 × 720 HUD output and two untouched 960 × 360 outputs;
labwc acknowledges a centered 960 × 360 HUD surface for the requested canvas.
its reports, screenshots and protocol traces live under `build/hud-wayland-evidence/`.
The full receiver build uses the handoff's existing `-Wno-misleading-indentation`
bench exception. All five new production HUD/navigation translation units also
passed separate `-Wall -Wextra -Wpedantic -Werror` syntax checks without that exception.
Targeted Clang ASan/UBSan checks passed for the portable HUD and navigation tests;
external Cairo/core archives were not instrumented, and leak detection was off.
Selected V11 services were left running throughout; no physical output, radio,
phone session, sealed release or third-display hardware was exercised.

Country support additionally passes portable provider/configuration checks for
original mph conversion, provider precedence, border changes, unknown countries,
withdrawal/expiry and country-specific face/ring colors. The CM5 fixture reads
actual atomic v2 provider files through the resident HUD to change FR → US → GB
in place, then removes the file to clear the sign while main/cluster stay untouched.
The fixture derives pixel positions from acknowledged surface geometry, rather
than assuming the requested canvas fills the physical output mode.
