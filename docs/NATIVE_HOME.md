<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Native Home

Keep the native Phones, Music and Cluster pages available on the main
touchscreen. A small **Home** button opens the page above CarPlay; **Back**
returns to CarPlay. Neither action selects a phone, changes its Standard/Ultra
mode, starts a receiver or ends its session.

This is an optional appliance mode. The example defaults off.
Its software checks and physical qualification are recorded below separately.

## Dock proportions and nested corners — V96

The dock's outer radius follows the allocated selection radius plus its CSS
inset. On the CM5 that is 36 + 8 = 44 px. V95 incorrectly used a full 56 px
capsule around a shorter 36 px selection, pinching the gutter at the ends.
Home and Settings now share corner centers with the surrounding rim. The
preview checks those centers from actual GTK allocations at both ends, including
horizontal placement.


The landscape dock uses a wider 112 px capsule and shorter 72 px rows. Its
96×72 px sliding selection is a pill; six tabs occupy 468 px vertically with
4 px gaps and 8 px outer insets. Symbols are 40 logical pixels with 12 px medium
captions, so the symbol carries more of the visual weight. The horizontal dock
uses narrower padding to keep six targets within a portrait window.

Home keeps its composition and 24 px column gap. On 1280×720, the app column is
`(180,246,336,336)` and player `(540,246,684,336)`; the aligned header follows
those edges. Actual GTK previews check all six selections, minimum touch targets,
viewport fit, long text and empty/untimed states on the CM5 GPU. Native page,
resident Home/Back and shortcut checks pass. The V93 app-handoff sequence below
is preserved. Live CM5 clicks select Phones, Music and Settings correctly; the
Music app target reveals Apple Music in CarPlay and Home opens again.
[Current deployment](HANDOFF.md).

## Home composition and app handoff — V93

Home uses a single centered header and content grid. Time/date align with the
left app targets; phone status and Open CarPlay align with the player edge.
Maps, Music and Phone are individual glass targets with clear names, short
purpose labels and chevrons. The wide player places artwork beside track details and plain transport glyphs.
The redundant outer header and large app/connection containers are removed.

In V93 at 1280×720, the app column was 336×336, the player 700×336, and the gap 24 px.
Three 104 px targets have 12 px gaps. Timed, untimed, phone-free and maximum-text
CM5 GTK renders retain identical column bounds; header edges, touch target
heights and viewport fit are checked. Missing timing hides its row. Missing
media centers a useful message. Routine app-success captions stay hidden.

Shortcuts first lower Home through its existing lease handoff. One intent waits
up to three seconds for fresh main readiness for the same phone generation and
Ultra publisher, then sends the app request once. Temporary publisher withdrawal
can recover within the bound; replacement, camera, expiry, navigation, stale
status and retirement cancel it. This fixes the V92 live finding that an input
written while Home owns main may be consumed before dispatch is permitted.
Eleven real GTK/asynchronous IPC cases cover this order and its failure paths;
phone/renderer/layer boundaries there are simulated. Actual V93 Music and Maps
clicks were separately checked with the real phone and output captures. Home is
left open. Physical touch, audible output and sustained frame timing remain
unmeasured. [Exact deployment](HANDOFF.md).


## Player and dock refinement — V90

Dock images now use 36 logical pixels, up from 24. Following the supplied
reference, Home's player uses one glass card with an 88 px artwork header,
24/21 px track/artist labels, full-width observed progress with elapsed and
remaining times, and large plain transport glyphs. The source heading and
individual transport glass pills are removed. The clickable track header and
transport controls are siblings; each keeps native GTK focus/accessibility and
the existing guarded action. Only phone-supported actions appear. Unknown
timing hides the track and shows unavailable time labels; no local clock runs.

V90 was installed from the narrow V87-based branch. Seventeen exact-source
Mac/CM5 dashboard checks, eight Mac GPU checks, native-page and CM5 resident
Home/Back fixtures pass. Actual phone Ultra reconnection, both outputs and
installed Home are captured; the phone currently supplies track text without
artwork or timing. The populated-timeline preview uses synthetic data.
[Deployment and limits](HANDOFF.md).

## Home dashboard (Home 2.0)

The native dark theme (`tools/phone_page_style.py`) uses a black
background, opaque grouped surfaces (#1C1C1E), grey-fill capsule buttons with
blue for the primary action, the iOS text sizes, and a floating translucent tab
bar with each symbol above its label. Settings use grouped lists
with the selected row tinted (cluster styles, colours, vehicle views), coloured
setting tiles, and a segmented control for startup warning lights.

It opens on **Home** (`tools/phone_home.py`) with a compact aligned header,
three app targets at left and the player at right. Portrait stacks the columns.
The header contains the time/date and active phone status/action. An unset
clock (before 2025) hides time/date. Player progress requires observed position
and duration; absent timing hides the complete row. Only supported transport
controls appear. The track header opens native Music; the Maps/Music/Phone app
targets open their CarPlay apps after fresh main readiness as described above.
They remain visible but disabled without a usable session. The phone action
offers **Open CarPlay** when permitted, otherwise **Phones**.

Navigation follows CarPlay: a floating vertical tab bar on the left when the
window is landscape, and along the bottom when it is portrait. The window's
requested (output) size decides, and the Home dashboard stacks its clock above
its widgets in portrait. Portrait needs a panel at least about 740 px wide.

The tab bar, selected tab and Home widgets use an original GSK material
in `tools/phone_glass.py`. Each surface samples the explicit page backdrop
and combines blurred texture, rim refraction bands, tint and gradients.
Home and Music derive a 6 × 6 colour field from admitted artwork and use a
fixed gradient without artwork. The other pages use a solid background.

GTK owns layout, accessible controls, focus, touch response and tab motion.
The phone action, progress track and Back to CarPlay use the same glass.
Home transport uses original glyphs. Displayed progress requires observed
timing. The selection slides and stretches with spring overshoot.
`phone_home.py`, `phone_page_style.py` and `phone_glass.py` are sealed
resident release modules.

## Trip, Sport and History leaf — source-only, 14 October

**Cluster → Trip, Sport & History** opens one leaf on the existing `PhonePage`
stack. `tools/phone_page_sport.py` provides the pure bounded projection and
the view; it creates no window, manager, request executor or acquisition owner.
The six sidebar entries, Home dashboard, stylesheet, glass, artwork and existing
pages retain their design. **Back to Cluster** returns to the parent. The existing
**Back to CarPlay** keeps its normal presentation/lease/readiness admission.
Tapping the selected Cluster sidebar entry also returns to the parent.
native UI owns design review of these new pieces: one existing-style setting tile,
one Trip/Sport/History segmented control, grouped data rows, a parent button and
two software-statistics buttons. No new CSS, font, palette, gauge or artwork.

| UI / control ID | Behavior |
| --- | --- |
| Stack page `sport`; `sport-open` | Open the leaf from Cluster; Cluster remains the selected sidebar parent. |
| `sport-back-cluster` | Return to Cluster without a manager command. |
| `sport-tab-trip`, `sport-tab-sport`, `sport-tab-history` | Select local grouped rows; no data acquisition or manager command. |
| `sport-trip-reset` | Existing exact `{"action":"trip_reset"}`; software journey only. |
| `sport-session-new` | Existing exact `{"action":"sport_session"}`; sport timers/laps/peaks only. |
| `sport-trip-*`, `sport-input-*`, `sport-timer-*`, `sport-lap-*`, `sport-peak-*` | Read-only data rows; semantic field suffixes are listed in the module. |
| `sport-history-trip-N`, `sport-history-event-N` | Newest-first presentation positions, **not** source identities. Original identity stays in copied details. |

The page consumes existing **`status.sport`** keys `version`, `configured`,
`sessionId`, `sourceGeneration`, `observedNs`, `source`, `error`, `inputs`,
`gMeter`, `timers`, `laps`, `history`, and **`status.sport.trip`**. Copied input data preserves
the original `value/acquiredNs/maximumAgeNs/ageNs/origin/availability`; trip data
carry software journey/source/origin, first/last acquisitions, distance/time,
means, peak and coverage. Sport rows carry copied acquired speed/g/GPS status,
four fixed timers, current/last/best/delta lap and five dated historical peaks.
The detailed source contracts remain [sport telemetry](SPORT_TELEMETRY.md) and
[software trip](TRIP_AND_HEADING.md#software-drive-statistics--source-only-14-october-2026).
No new manager status key, action, GPS-line editor or phone protocol is added.

Null/missing/stale/disconnected readings show **Unavailable**; genuine acquired
zero remains zero. **SIMULATION**, software estimates, coverage and **HISTORICAL**
labels remain explicit. Covered time is acquired coverage, never a wall timer.
Original host-monotonic dates, source generations and software identities remain
in copied data and evidence. Visible rows use concise source, availability,
estimate, coverage and historical labels, without raw clock/identity dumps.
The projection copies fixed scalar fields and caps histories at the owner's
16 trip / 64 sport entries. It does not import restored counters or compare
historical dates across reboot. This baseline displays process-memory trip
history; durable history remains separate reviewed source work.

The existing 250 ms health callback conservatively spends a copied input's
remaining age budget using elapsed time since the **manager request dispatch**.
It only withdraws current readings/running timers; it never changes source dates,
copied ages, elapsed results or accumulated statistics. The existing two-second
status poll is unchanged, so short-age inputs can be unavailable between polls.
Retained totals/peaks/results remain historical while the manager is fresh;
manager loss clears all displayed authority and disables the two actions.

Both buttons use the existing one-operation executor and fresh-status gate.
Pending, accepted and error notices describe the request. An action reply's
`sport` payload cannot replace a dated status or claim a changed journey/session;
the following fresh status supplies observed identities. Reset changes neither
the car's trip/odometer nor actuators. Retirement destroys the same native
surface and cancels queued work; late replies cannot revive the leaf.

Manual development CLI `phone_page.py --socket PRIVATE_SOCKET --view sport`
selects this leaf on the same application factory. Allowed initial views are
Home, Phones, Music, Cluster, Car, Camera, Settings and Sport in the combined
forward source; unknown views are
rejected. The resident boot route still starts Home and seals
`phone_page_sport.py` unconditionally with the resident page graph, even if
sport acquisition is disabled. Forward verification admits a hash-bound old
page only if it has no sport-module import; a current page missing that module
fails. A newly qualified helper is required before a future immutable release
adopts the changed graph. No selected release is changed by this source work.

The separate forward integration preserves native UI's Car page, scrolled settings,
drive/greeting actions, notice recovery, Home/Back and unchanged glass/style
files alongside this leaf. A portable invocation of the actual `show_view`
method checks Car/settings/Sport/parent/Home and hidden-Car/unknown rejection.
The corrected Linux GTK fixture exercises the same merged runtime factory and
retains original virtual-output captures. Its initial missing layer-shell
dependency failure stays retained; adding the private library/GIR preconditions
changes neither source nor deadline. This remains component evidence, with
native UI's leaf review and resident regression repair pending. Exact source,
counts and retirement limits belong to the
[forward handoff](HANDOFF.md).

Focused qualification uses the assigned `a140236` base, with no storage/settings
branch imported. Final macOS **6/6** CTests and Debian ARM64 **10/10** CTests
pass: **173** portable Python cases on each platform, plus four actual Linux
GTK scripts (the new leaf, existing Phones/Music, Home dashboard and resident
owner). The new leaf exercises the actual boot runtime factory, private socket,
pending/rejected/late requests, one window, parent/sidebar navigation, CLI
admission and retirement. Eleven fresh sequential GTK owners retain 1280×720
available, missing, disconnected, simulation, history and manager-loss images,
including the Trip/Sport scroll ends. Available captures require a genuine new
synthetic acquisition after the actual mapped/realized/tab-ready barrier, with
unchanged age limits; visible speed, zero acceleration, lateral acceleration
and GPS availability are asserted before capture. Original virtual-output PNGs are reviewed;
all required controls remain within the viewport and sparse lists keep their
natural row height. No personal Apple fonts/symbols/artwork are copied.

The test-only `--capture-output` option enables [Weston's documented output
capture](https://cgit.freedesktop.org/wayland/weston/tree/man/weston.man) only in
the fixture's owned private compositor. Stock GTK OpenGL/llvmpipe and Weston
14.0.2 supply virtual rendering; neither is physical V3D/scanout/touch evidence.
Pinned format 19.1.7 passes. Actual Docker INT/TERM/HUP cleanup passes; original
source, logs, failed logs, CMake/Testing, raw images, JSON and verified hashes
remain outside every retired own build/container. Setup failures (missing
builder/SVG loader) and a no-node capture-fixture failure are retained. Repeated
image previews suppress duplicate regions; a PNG pixel audit confirmed intact
title bytes, so the temporary missing-glyph inference was withdrawn and no
renderer policy was changed. Evidence: private `20261014/codex-sport-page/`.

The sole phone-session owner must first obtain manager whole-branch and native UI
new-piece review, resolve the announced Cluster/settings overlap, run a **new
coherent required matrix**, and seal a **new application/helper graph**. Then
check Home → Cluster → leaf → parent/sidebar → Back to CarPlay, a ten-second
hold, software reset/session identity changes only after fresh status, exact
original acquisition/expiry, simulation/history labels, main/touch/Climate and
both streams/UI-Sync/audio/control with unchanged owners. Fault/retirement
injection stays in isolated fixtures first. Physical providers, alternative
formats, physical touch, camera/audio continuity and fresh60 remain unqualified.

## Unavailable connection information

**Retry connection** appears when the manager reply is unavailable or older
than five seconds. It requests fresh status without restarting any receiver,
radio or service. An outstanding request disables Retry; repeated taps cannot
queue another operation. A fresh reply hides it and restores the ordinary
controls.

Expiry also clears the last Music text, artwork, timeline, applied audio labels
and camera visibility/request status. Retained files and old successful replies
cannot keep those observations looking current. The selected phone rows become
unavailable, and stale confirmed cluster markers and vehicle views are withdrawn.
Recovery needs a new dated status reply.

The Camera card separates **Manual request · On/Off** from the observed view
phase. A request can remain On while the view waits or faults; vehicle activation
can keep a visible view after the manual request becomes Off. Invalid camera
snapshots cannot enable controls. The additive source-only
[`camera_status_observation`](CAMERA_CONTROLS.md#camera-owner-status-provenance--15-october-source-only)
manager field preserves original HCS1 owner-status provenance as diagnostic data;
it supplies no new UI/control or camera-pixel/readiness authority. Camera
availability remains subject to the installed graph and current session: the page no longer claims every camera
works without a phone.

Actual macOS GTK Phones/Music, Artwork and Resident fixtures cover the separate
camera intent, expired visible camera and applied audio, verified artwork
retirement, malformed camera snapshots, fresh recovery and Retry taps during a
held request. Portable Home presentation checks also pass. These use synthetic
manager/phone boundaries and retain the existing GTK/GDK diagnostics; they do
not establish CM5 touch, physical camera display or connected-phone continuity.

## Recovery decision history — source only, 15 October 2026

`status.recovery.decisions` is a copied chronological list of at most eight
software decisions from the existing Manager. The oldest entry is evicted when
a ninth arrives. Every manager starts empty; this is neither durable history nor
a complete session log. The existing `phone`, `remainingAttempts` and
[read-only schedule](PHONE_MANAGER_BOOT.md#read-only-recovery-schedule--14-october-source-only)
retain their exact meanings. Historical records never grant current authority.

| Record key | Meaning |
| --- | --- |
| `decision` | One of the fixed coarse events below. No private exception text. |
| `generation` | Existing Manager lifecycle generation when the decision is recorded. Queueing follows complete child retirement; this is not the exited child's connection generation. Values from separate manager lifetimes cannot be joined. |
| `configuredMaximum` | Existing policy maximum, default 0 and at most 3. |
| `remainingAttempts` | Existing recovery budget at queue/dispatch/cancellation/exit, or the successor budget after an actual recovery child starts. Failed attempts retain the dispatched budget; no child failure has consumed another recovery opportunity. |
| `exitCode` | Actual observed owned launcher return code for queue/exit decisions; otherwise null. Zero and negative signal codes retain their meanings. |
| `clockDomain` | `python-time.monotonic` when the actual software observation date is available; otherwise null. |
| `observedMonotonicSeconds` | Original finite nonnegative float observation at this software decision; otherwise null. Never a receiver exit, phone, acquisition, presentation or wall-clock timestamp. Reads do not renew it. |

The fixed events are `queued`, `dispatch`, `child_started`, `cancelled`,
`cancelled_selection`, `cancelled_pairing`, `cancelled_owner`,
`cancelled_removed`, `budget_exhausted`, `not_eligible`, `retirement_required`
and `attempt_failed`. `dispatch` means an existing due attempt entered the
original launch path; `child_started` means that recovery path actually created
an owned process. Neither means connected, Ready, useful content, audible sound
or physical completion. `attempt_failed` deliberately combines existing
preflight/launch exceptions because the original catch cannot reliably distinguish
them. Generic `cancelled` covers existing stop/disconnect/shutdown/superseding
Connect; the more specific reasons appear only in the owner branch that observes
them. Cancelling an absent queue records nothing.

The bounded deque is owned under the existing Manager lock. Status returns new
lists and new record dictionaries containing only primitives. Reads, polling,
repeated ticks and readiness do not add events, dispatch, refill budgets or make
history current. Original positive-exit eligibility, zero/default-disabled and
signal quarantine, 2/4/8-second delays, preferred selection, pairing, preflight,
complete retirement and generation-bound cancellation remain unchanged. No new
timer, thread, provider, coordinator, persistence or action exists. Records
contain no phone addresses/names, titles, pairing data, keys or arbitrary errors.

native UI interface: new data key `status.recovery.decisions` only; new actions,
control IDs, widgets, pages, GTK pieces and styles: **none**. Existing Home2
components may consume these observations after whole-source qualification.
The native data helper `recovery_phone` now accepts the original two-key
payload and the declared optional `schedule` and `decisions` together or
individually. Previously, either extension caused the exact-two-key parser to
return null, removing the existing native Cancel retry target for every queued
Manager recovery. This source-only compatibility repair changes no GTK calls,
controls, labels, layout, styles or actions.

Cancellation data authority remains the current uppercase six-byte colon-separated
phone address and an exact integer budget of 1–3; booleans, lowercase addresses,
missing/null phones and zero/unavailable budgets remain rejected. Unknown
top-level keys are rejected. Optional diagnostics must have exactly the declared
eight schedule keys or at most eight seven-key decision records. They admit only
the documented fixed enums, exact integer budgets 0–3, unsigned 64-bit Manager
generations, nullable signed 32-bit exit codes and nullable finite nonnegative
float dates. Missing optional fields are supported; malformed supplied diagnostics
are rejected without logging their contents. Inputs are neither mutated nor kept.

Diagnostic state, dates and historical budgets never supply or override the
current phone/budget, readiness or status freshness. A typed idle/cancelled
historical diagnostic cannot change valid core authority; a queued history cannot
create authority for a null phone or exhausted budget. The existing page still
uses its original reply-age/pending-operation guards, and the Manager endpoint
still validates the captured current phone and generation before cancellation.
A delayed cancel cannot stop a successor. No deadline, budget, backoff, retirement,
Ready, Home/Back, camera or visibility policy changes.

The regression first preserves the unchanged accepted producer's six original
Python suites and demonstrates its actual child/endpoint queued status failing
the old parser. Final focused Mac/Linux checks include actual endpoint status,
cancel, wrong-phone/generation rejection, history-only status and successor
protection, plus old two-key and malformed optional/core admission. A closed
old/new comparison retains full status and owner behavior; only the helper's
intentional parsed-phone acceptance differs. These Python-only checks are not
C++ sanitizer, GTK pixels, GPU, phone or physical evidence. For native UI: interface
keys and existing actions/IDs are unchanged; **new visual UI pieces: none**.

This model is still unpublished. Focused Python checks use actual local owned
children and private Unix endpoints with explicit synthetic BlueZ, receiver
program, display validation and scheduling clocks. They establish software
ordering/retirement, not radio, GPU, phone, car, optical, audible or physical
recovery. Root reviews the whole source and retained evidence before folding.
The sole phone-session owner then needs NEW coherent all-required release,
original GTK/native UI, helper, source/module/role/config/argv/unit/input/assets and
fresh capacity/full-reserve gates before publishing an immutable CM5 generation.

## Settings

A **Settings** dock item appears when the installed UI configures
`displayResponseFile`, drive-mode profiles or the greeting, or enables the
startup warning preference below. Its Display response card offers Smooth, Balanced and
Fast, the CarPlay Ultra trade-off between video smoothness and touch response.
The choice applies right away without reconnecting and is retained across
restarts. [TOUCH_LATENCY](TOUCH_LATENCY.md#display-response-setting) owns the
policies and their measurements.

**Warning lights during startup** offers Hide/Show for the Ultra STARTUP and
brand × Apple phases. Hide is the installed default; Drive restores current
fresh lamps. Configure `hideDuringStartup` in the warning JSON and
`startupWarningLightsFile` in installed UI to expose the saved native choice.
The manager saves the private preference without changing vehicle observations
or reconnecting the phone. [Warning overlay](ULTRA_WARNING_OVERLAY.md) owns the
configuration, freshness policy and qualified scope.

**Drive mode** is a segmented control: Automatic plus each configured profile
(`status.drive_modes`). A tap sends `drive_mode` with the current `revision`,
so a stale page cannot overwrite a newer choice. The line below names the
source and phase the manager publishes ("Car reports Sport · Shown on the
cluster", "Applying to the cluster…"); a saved choice without a fresh
publication reads "Saved. Waiting for the cluster." It changes display
profiles only, never the car. [Drive modes](DRIVE_MODES.md) owns the contract.

**Welcome animation** offers Off, startup branding and Minimal (`greeting_sequence`)
and, when an animation is chosen, Preview welcome/goodbye
(`greeting_preview`). Previews are unconfirmed requests to the cluster; the
main screen and power integration remain parked.
[Greeting sequence](GREETING_SEQUENCE.md) owns the interface.

## Cluster formats

Without CarPlay Ultra the Cluster page offers the native formats as
miniature previews (Minimal, Sport, Full Gauges), ringed in blue when chosen,
above a grouped list of centre content (Map, Media, Vehicle, Trip). Both use
the existing `cluster_page` preference and show only pages the manager lists
in `cluster_pages`; a lost status clears the selection. With CarPlay Ultra the
style list labels known authored layouts with their `cluster_formats` names
(for example "Sport · DigiRing"). The settings sections live in
`tools/phone_home.py`, so the sealed Home module set is unchanged.

## Car

A **Car** dock item appears when `status.prephone_vehicle.configured` is true.
Fuel (with a level bar), range and outside temperature are large tiles; coolant,
oil temperature, oil pressure (shown in bar), battery and odometer form a
grouped list. Only `available` rows show values, including real zero and
negative readings. Missing, unavailable, out-of-date and disconnected rows say
so in words; a failed or retired publication shows "Car readings unavailable"
with its reason and no values. Simulated origins add a SIMULATED DATA badge.
The page never infers fitted equipment, fault clearing or phone readiness.
[Native cluster pages](NATIVE_CLUSTER_PAGES.md#for-claude-status-keys-and-ui-pieces)
owns the publication contract.

## Native appearance

The page uses original monochrome vector icons, launcher tiles and system
sans-serif typography. Controls retain accessible names, keyboard focus and
ordinary GTK input. The layout and actions can be previewed using synthetic
metadata with `tools/preview_native_home.py`. A preview qualifies GTK
composition and fixture behavior within its recorded scope.

## Bench binding

The current selected release is recorded in [the handoff](HANDOFF.md). A
historical read-only check on 4 October of the immutable managed/Home release
`cm5-20261004-home-readiness-v3` verified its
manifest and complete unit binding; compositor, outputs, backup cluster, manager
and page were active. This establishes deployment, not a complete physical Home
workflow. The latest source candidate and remaining bench checks are listed in
[the handoff](HANDOFF.md); publishing source changes does not update the running
release. See [application releases](APPLICATION_RELEASES.md) for the exact binding.

## Configure the page

Start with [`config/phone-page.example.json`](../config/phone-page.example.json).
All fields are required except the backward-compatible `homeButton.enabled`
option; unknown fields and duplicate JSON keys are rejected.

| Field | Meaning |
| --- | --- |
| `schemaVersion` | Integer `2`; enabled version-1 pages are refused. |
| `enabled` | Explicitly select this page at startup. Default `false`. |
| `managerSocket` | Exact private socket of the supervised phone manager. |
| `surfaceControlFile` | `stateDirectory` plus `/home-surface.request`. |
| `surfaceLeaseFile` | `stateDirectory` plus `/home-surface.lease`. |
| `waylandDisplay` | Named Wayland socket shared by both main CarPlay modes. |
| `output` | Exact main-screen connector, such as `HDMI-A-1`. |
| `homeButton.enabled` | Show the onscreen Home affordance when usable. Boolean; defaults to `true` for existing profiles. |
| `homeButton.corner` | `bottom-right`, `bottom-left`, `top-right` or `top-left`. |
| `homeButton.margin` | Integer 0–96 logical pixels from the anchored edges. |
| `homeButton.size` | Integer 48–96 logical pixels. |

The output and socket must match the production display configuration for
**both** Standard and Ultra. The socket must equal the manager's
`stateDirectory` plus `/manager.sock`. Main and cluster remain distinct.
Paths are canonical absolute names without service syntax. Profiles are
bounded, stable, trusted single-link files without group/other write access.

The display account runs the page. It does not run as root, create the
manager's directory, start a radio or become a connection manager itself.
The independently supervised manager prepares its private state directory.

Add the same paired paths to the receiver configuration:

```json
{
  "native_page_control_file": "/var/lib/headunit/phone-manager/home-surface.request",
  "native_page_lease_file": "/var/lib/headunit/phone-manager/home-surface.lease"
}
```

Admission requires exact agreement between the page, manager and receiver.
Standard uses the owned `wayland-gpu` presenter for this route. The launcher
adds the matching `nativePageControlFile` and `nativePageLeaseFile` fields to a
private runtime copy of the Ultra profile; it never changes the signed asset
or sealed static profile. A disabled version-1 page remains readable, but its
old uncoordinated overlay cannot be enabled.

## Configure startup

Install the GTK 4 Python bindings and GTK 4 layer-shell library/introspection
package for the target distribution. On the current Debian-based CM5:

```sh
sudo apt-get install python3-gi gir1.2-gtk-4.0 \
  libgtk4-layer-shell0 gir1.2-gtk4layershell-1.0
```

Enable a [phone manager profile](PHONE_MANAGER_BOOT.md) and this page profile.
Validate the complete configuration without opening a display or radio:

```sh
python3 tools/phone_page_boot.py \
  --profile /home/headunit/.config/ultra-headunit/phone-page.json \
  --manager-profile /home/headunit/.config/ultra-headunit/phone-manager.json \
  --receiver-config /home/headunit/.config/ultra-headunit/lab/receiver-appliance.json \
  --validate-only
```

Resident-page admission retains bounded stable input bytes alongside file
identity, ownership and timestamps across its production parser passes. This
rejects a same-size rewrite even when coarse filesystem timestamps remain equal.
Page, manager, display and receiver profiles keep their 16 KiB read bounds;
the installed-UI profile keeps its existing 64 KiB bound. Symlink, special-file,
unsafe-mode and replacement checks remain in force. Portable regressions use
real files and the actual C++ parser with an explicit equal-metadata fixture;
they open no display or radio. V46 introduced this guard; selected V47 retains it.

Render the optional service bundle:

```sh
sudo python3 tools/install_cm5_appliance.py \
  --user headunit --repo /home/headunit/ultra-headunit \
  --receiver-config /home/headunit/.config/ultra-headunit/lab/receiver-appliance.json \
  --phone-manager-profile /home/headunit/.config/ultra-headunit/phone-manager.json \
  --phone-page-profile /home/headunit/.config/ultra-headunit/phone-page.json
```

Add `--install` to save and enable the bundle. Installation uses the existing
recoverable unit transaction and does not start or restart a session. The page
is a separate `headunit-phone-page.service`; the manager still owns the single
`headunit-receiver.service`. The page depends on the compositor and waits for
its named main output. A receiver failure does not stop the page.

The original direct-receiver service bundle remains the default when these
profiles are not selected. Old five- and six-unit rollback snapshots cannot
modify a later page service outside their recorded scope.

## How presentation works

The page is a full-output Wayland **bottom-layer** surface. It stays behind
CarPlay and becomes visible when no main CarPlay surface covers it. Starting
a receiver does not hide the native page: a running process is not proof that
video has appeared.

Home is a separate **overlay-layer** surface on the same explicitly selected
monitor. It appears only when a fresh manager report says CarPlay can actually
return, and no mutating operation or camera exclusion is active. Pressing it
requests the main screen and hides the small button.
The full page rises only after the renderer has withdrawn its own main window.
Back lowers the page, returns screen ownership and restores the button. Neither surface
reserves screen space, so the CarPlay canvas and touch mapping are unchanged.
Keyboard focus is requested only while the full page is opened.

The manager's `carplay` readiness object has exactly `state` and `returnReady`.
States are `disconnected`, `starting`, `ready`, `home`, `fault` and `stopped`;
only `ready` or `home` may report `returnReady: true`. A selected phone,
Bluetooth connection or running receiver is insufficient. Missing or malformed
readiness hides Home and Back. Back additionally requires the page's exclusive
main-surface acknowledgement. A fresh not-ready report lowers the page and
releases that acknowledgement, so a new receiver can bootstrap. Manager loss,
receipt-clock reversal or a status age over five seconds withdraws navigation
and disables commands rather than using a stale snapshot.
A successful Connect/Disconnect or active-phone Remove acknowledgment
also revokes the old ready reply and releases Home before the next status
request completes. Inactive-phone preferences retain the current owner. This
source fix passes eight actual GTK/private-socket cases with the refresh held
and explicit simulated renderer/layer boundaries on macOS and on actual CM5
GTK/V3D under an isolated floating-window compositor. The kiosk shell forces
the fake Home surface fullscreen, so this fixture requires the floating shell.
The source fix is not installed in V3
or physically qualified on CM5.

### Reply freshness and modal lifetime

A manager reply has no independent acquisition timestamp. The page therefore
uses the actual request dispatch date as its conservative observation bound,
carrying it through the worker future and GLib completion. IPC or GTK delay
cannot refresh old readiness. Navigation and the resident surface use the same
freshness predicate and date; expired/reversed completion disables actions and
requires a fresh reply. There is one pending action, with no secondary queue,
pending boolean or retained snapshot authority.

The page's Remove confirmation belongs to its parent surface. Parent unmap
retires the dialog before Home releases the main lease, even though GTK may
retain the parent's realized resources. Response admission checks that the
actual dialog is still visible and realized before destruction; a queued Accept
cannot act after withdrawal, including when manager status is still fresh.
This uses GTK resource lifetime rather than another modal/session state model.

The manager publishes Ultra catalogue status and admits style actions only
while its current child has a fresh launch-bound, return-ready Ultra mode.
Negotiated Ultra still starting has no catalogue/action authority. Standard,
no-phone, wrong-launch and expired-mode states keep native controls available.
The catalogue must carry that same launch nonce and control connection generation,
plus a genuine `CLOCK_MONOTONIC` publication date no older than three seconds.
An old same-pack catalogue cannot be admitted by a new ready receiver or a new
connection inside the same launch. The page captures the rendered catalogue's
identity in each style button; the manager rejects a delayed click from a previous
catalogue before writing. The bounded private request carries the same identity,
which the VM owner checks again before using the existing ordered UI exchange.

Retirement revokes eligibility through the existing child/connection/presentation
owners. It leaves dated catalogue bytes in place: old teardown never unlinks a
successor's publication, and touching an old file cannot renew its embedded date.
VM construction consumes pre-existing button requests even when preference
persistence is disabled. Only the separate verified-pack phone-confirmed
preference may restore through a ready catalogue. Portable manager/selector/C++
interop fixtures cover wrong launch, reconnect generation, delayed button/request,
stale/future dates and retirement, without qualifying physical all-pack switching.

### Offline ownership qualification — 7 October 2026

The manager, Home presentation, release and endpoint fixtures pass 65 targeted
checks. Three actual macOS GTK4 scripts pass: Phones/Music, Pairing and Resident.
The Resident script holds an actual private-socket reply for 5.3 seconds, checks
that completion cannot revive authority, then verifies fresh recovery. It also
queues Accept before fresh Back and dispatches it after actual dialog destruction
with status still fresh: no Remove RPC occurs. Current confirmed Remove, Cancel,
parent destruction, readiness loss, camera exclusion and partial teardown remain
covered. Style tests cover fresh Ultra starting, ready/home, no-phone, Standard,
wrong launch and expiry.

GTK resources/socket/timing are real; manager, monitor, layer and renderer
boundaries are explicit fixtures. This is not Linux Wayland/GPU or physical
phone evidence. Before/final logs and source hashes are retained privately at
`/tmp/native-ui-ownership-evidence/source-hashes.json`. Full logs retain existing
GTK/GDK warnings; a pass does not imply warning-free desktop integration.

The same bounded-text rule used for pairing also applies to Ultra style names
and camera fault notes: Pango wraps whole words when they fit and characters
when a single word exceeds the available width. An actual macOS GTK allocation
audit reproduced 4082-pixel and 2019-pixel window minimums with valid unbroken
256-character style names and 128-character camera errors respectively. With
these two wrap settings, all audited labels retain 1280 × 720. The existing
Phones/Music fixture checks the style's two-line ellipsis, fully wrapped camera
error and viewport bounds; Pairing and Resident fixtures also pass. Before/after
measurements and complete diagnostic logs are retained privately under
`~/.cache/headunit-qualification/20261007/oem-pass/text-width-fix/`,
with the original audit in the adjacent `text-width-audit/` directory. This is
actual GTK with synthetic manager data, not Linux Wayland or physical camera
qualification; configured cameras remain excluded from resident Home releases.

### Retained V3 physical check

On 4 October, after restoring the exact V3 manifest recorded above, the user
confirmed stable Ultra main/touch/cluster, then Home → wait ten seconds → Back,
and the corresponding Standard workflow. CarPlay returned with working touch
and the cluster continued. The user reports more than five seconds after Back
in both modes and confirmed it in a repeated Standard cycle. A read-only CM5
observer measured 7.717 seconds from its first OFF request observation to the
receiver's ready publication in that repeated cycle (20 ms polling). This is
receiver readiness timing, not GTK callback or physical pixel timing; it does
not establish which restoration stage caused the wait. The private report is
`home-return-v3-observation/report.json` in the handoff's qualification cache.
Prompt return remains open. Native Disconnect/reconnect, repeated
transitions, cold boot and rollback remain separate checks.
The later source readiness-race fix is not inferred from this V3 result.

A second observed Standard cycle took 4.903 s from first OFF observation to
ready publication; the user estimated two to five seconds. Its receiver PID,
connection generation and publisher nonce stayed unchanged. Graphics descriptor
counts returned by 172 ms, and CPU activity resembled the Home baseline.
Aggregate receiver TCP traffic continued but cannot identify main-video arrival
or decoding. This narrows the investigation without proving a window-ready ACK
or a specific slow gate. The sanitized private analysis is
`home-return-v3-proc-observation-v3/analysis.json`. The earlier short observers
missed the user's cycle and provide no return timing evidence.

### Return keyframe — 8 October

Home drains the main video while CarPlay is withdrawn, so the returned window
needs a decodable keyframe. The receiver requested one only at session startup;
on a static CarPlay home screen a V16 Ultra return therefore stayed black about
25 s after an immediate Back. `MainPresentationStatus` now carries the
generation's count of main-window returns (Ultra display restorations,
Standard departures from Home), and the control owner sends one
`forceKeyFrame` for the main stream per new return. V17 returned instantly in
a live Ultra test; Standard's return is not yet live-checked.

### Isolated return timing

The latest source fixtures on CM5 V3D measure actual host monotonic dates,
window restoration and a genuine current-generation receipt. In the isolated
signed-GPU Home fixture, Back took 1.35 ms, OFF/unlock 0.37 ms, window restoration
27.72 ms after release, and the first qualified scene another 38.19 ms: 66.88 ms
from Back. The Standard fixture's two returns measured 23.62–29.69 ms to window
readiness and 88.43–91.39 ms from release to a correlated fresh-frame receipt.
Both fixtures pass their generation, freshness and retirement checks.

These are private headless Wayland/GPU checks with synthetic sources and peer
timing, not the selected V3 real-phone return. They do not reproduce its 7.717 s
wait; cold window construction alone is therefore insufficient to explain it.
Evidence: `home-return-timing-components/` in the handoff's qualification cache.
The documented GCC bench exceptions apply.

### Receiver readiness contract

The renderer publishes a bounded private `carplay-status.json` observation. Its
eight fields are `version` (integer `2`), `nonce` (the manager's fresh 32-character
lowercase hexadecimal launch token), `sequence`, `connectionGeneration`,
`updatedMonotonicNs`, `state`, `returnReady` and `negotiatedMode`. The three
counters are canonical unsigned 64-bit decimal strings. The date uses the host's
actual `CLOCK_MONOTONIC`, independently of video PTS. The mode is `standard`,
`ultra` or null; disconnected and stopped observations clear it. The manager
also accepts the earlier seven-field version `1`, which has no negotiated mode.

The control owner atomically publishes state changes immediately and a heartbeat
at most every 250 ms. A main owner observation older than one second is unusable;
the manager rejects a wire observation older than 1.5 seconds, from another
launch, or with reversed generation/date/sequence. Re-reading identical bytes
does not refresh their date. Child death immediately revokes readiness. Static
release profiles cannot set the derived runtime path or launch token.

When status publication is configured, normal state changes, heartbeats and
explicit readiness withdrawal are mandatory. A publication error retires the
receiver's connections and Ultra resources, removes its media publication and
closes its listener before a normal failure exit with code 1. It cannot leave an
authenticated session running with a discarded publisher and stale readiness.
Terminal stopped publication during cleanup remains best effort; its failure
must not abort cleanup or replace the original error. This adds no generic
publication or receiver retry.

The portable publisher regression uses the kernel's real file-size limit and
ignored `SIGXFSZ` to force a partial write followed by `EFBIG`. It checks the
reported error code, unchanged accepted snapshot and sequence, and removal of
the failed temporary file. The actual encrypted CM5 listener fixture passes on
GCC and ASan builds, before authentication and after synthetic pairing,
verification, SETUP, two video streams and RECORD. A child-only file-size limit
forces the idle heartbeat write to fail. The baseline stays alive with frozen
status and, after restoring the test limit, still answers encrypted `/info`.
The fixed receiver exits with code 1 within two seconds, retires the connection
once and closes control/event/iAP/video sockets and listeners. These are
publisher and transport failure checks; synthetic packets do not establish
real-phone behavior or physical presentation. Current deployment and live
evidence belong in [the handoff](HANDOFF.md).

Readiness requires the authenticated current RECORD session and a genuine main
presentation receipt from the owned GPU path. Home may retain eligibility after
intentional window withdrawal only while that same decoder, clock and owner
remain healthy. Faults, teardown and shutdown withdraw it before slow resource
retirement. Status queries cannot refresh a stalled rendering worker. Legacy
preview backends without these observations remain conservatively unavailable.
This contract establishes eligibility to return to CarPlay; it is not proof of
physical scanout or a touchscreen hardware test.

`MainPresentationSnapshot::retire()` is terminal for its generation. A late
observation with that same generation cannot restore readiness, health, Home or
restoration counters; only `begin()` admits another owner. The 9 October
architecture regression fails against the previous implementation when it
publishes a visible healthy scene after retirement. Its replacement-generation
checks also preserve rejection of older observations and older retirement.
Portable and scoped Linux sanitizer evidence is summarized in
[the architecture review](ARCHITECTURE.md#architecture-review--9-october-2026).

### Manager replacement and independent native data

The source-only Linux `readiness-generation-join` regression joins the actual
manager, endpoint locks, C++ status publisher and native vehicle
publication owners.
It replaces the manager while the same independent native source process keeps
its original vehicle generation, source publication, acquisitions, age limits
and simulation/calculated provenance. A new native IPC challenge and copy date
do not create new acquisitions. Missing readings remain null; original dates
determine whether retained zero and negative readings are still available.

A killed receiver leaves its original Ready file. The fresh manager starts a
new launcher behind an explicit fixture gate and rejects that old launch's
publication while it is still inside the original 1.5-second age limit. The
test measures that limit before and after dispatch; a missed window fails as
inconclusive without refreshing bytes, replacing clocks or retrying. Only the
replacement publisher's own publication can grant its wire return eligibility.

The two existing C++ fixtures are unchanged. BlueZ, dry-run validation and the
gated receiver executable are explicit substitutes; the publisher's `r` command
is synthetic renderer eligibility, not a genuine presentation receipt. This
checks software admission across fresh manager instances and actual child/IPC
lifetimes. It does not qualify the main-loop signal boundary, sealed native-role
configuration, systemd descendant retirement, GPU/phone readiness, physical
presentation or car data. The real source and receiver clocks are unchanged;
the independent rendering/clock retirement tests retain their existing scope.

The unchanged V87 Clang baseline failed the older backpressure recovery test
when fuel was correctly stale. One passive original-case diagnostic observed
its new acquisition before IPC re-admission, with only 6.46 ms remaining before
the original 500 ms expiry at receipt. That does not establish the earlier
failure's exact chronology. The test now unblocks publication, verifies genuine
re-admission with the old dated fuel still stale/null, then requests the existing
new acquisition and verifies its new date and real zero. The 500 ms age, one-second
reconnect, 200 ms copy and three-second test waits are unchanged. Original failed
source, the prepared draft and diagnostic evidence remain outside Git.

The corrected frozen source passes focused Mac 13/13, strict Linux GCC 15/15
and Clang 19 ASan+UBSan 15/15 CTests, with no reported skips. The joined owner
case is Linux-only. C++ fixtures are unchanged and instrumented in the Clang
lane; Python execution is not sanitizer instrumentation. GPU-disabled native
and receiver callers compile separately from GPU/phone qualification. These
are software component checks, not a deployed generation or physical acceptance.

During a mutating phone operation, Home and Back are hidden and command controls
are insensitive; additional taps are discarded rather than queued with old
phone or generation data. An ordinary read-only status refresh does not blink
or unmap Home/Back or release existing screen ownership. Phone commands remain
briefly insensitive while the single request executor reads that status.

Set `homeButton.enabled` to `false` to omit the onscreen button. The native page
still appears as the bottom underlay when CarPlay is absent. Disabled-button
callbacks cannot request a handoff. The presentation's explicit `open()` method
is an admission seam for a future physical Home adapter; no GPIO, steering-wheel
button or other hardware adapter is implemented here. With valid phone-free
status, that method only presents the existing bottom page and does not take
exclusive ownership. With usable CarPlay it follows the same timed handoff as
the onscreen button.

Layer changes explicitly unmap and remap only the existing native page window.
An opaque phone surface can stop an occluded GTK page's frame callbacks; a
layer-property change alone then remains pending. Remapping gives GTK a fresh
commit path without recreating page widgets. Repeated Home or Back requests do
not repeat the remap or request. The cluster is outside this ownership boundary.

### Main-surface ownership

Covering a live opaque CarPlay GPU window can stop its genuine presentation
feedback and expire clock continuity. Layer stacking alone cannot preserve a
returnable main view. Home therefore withdraws the actual main window while
retaining the phone session, decoder and clock; it does not fabricate receipts
or extend deadlines to compensate for an occluded renderer.

The page and renderer use two private files in the manager's existing owned
0700 directory. The empty, owned 0600 lease file retains its inode between
runs. The request is atomically replaced with `off\n` or
`on <CLOCK_MONOTONIC deadline in nanoseconds>\n`; each ON is valid for 750 ms.
The page renews it during its 25 ms ownership poll. No permanent ON is written.

1. The renderer holds a shared file lock only while its main GPU window exists.
2. Home writes ON and waits nonblockingly for the exclusive lock. A timeout
   leaves CarPlay uncovered and cancels the request.
3. The renderer cancels touch, destroys its GPU/window, drains video inputs and
   releases the shared lock. Its phone session, source clock and Ultra Runtime
   stay alive. A never-started main panel can defer its bootstrap independently.
4. Exclusive acquisition acknowledges actual window withdrawal. Home maps
   its full overlay and retains that lock while open.
5. Back remaps the page to BOTTOM and synchronizes its GDK connection with the
   compositor before writing OFF and releasing ownership. CarPlay recreates
   its main window and requires fresh, genuine presentation feedback.

No synthetic video receipt or extended timing deadline is used. A page crash
releases its lock and its timed request expires. Normal shutdown retains the
handoff through surface retirement and closes it after the page's display
connection retires. Replacing the pinned directory or lease is an error;
an unavailable request cannot admit an uncoordinated renderer.

The implementation uses the
[GTK 4 layer-shell API](https://wmww.github.io/gtk4-layer-shell/gtk4-layer-shell-GTK4-Layer-Shell.html).
Its library is loaded before GTK or a Wayland library. Protocol version 4 or
newer and one unambiguous, valid named output are required. Missing protocol,
lost output or compositor withdrawal retires both surfaces and fails the
process; it never redirects the page to the cluster or another default monitor.

One retained `page.lock`, separate from `manager.lock`, protects the page
before any GTK/display effects. The file remains empty and private between
runs. A competing owner is refused. Shutdown disables polls and queued
callbacks and destroys both surfaces before releasing the lock. Repeated
application activation does not create another page or raise a hidden one.
Successful surface retirement also releases the page's monitor references
before closing its GDK connection. GTK 4.18.6's
[Wayland monitor finalizer](https://github.com/GNOME/gtk/blob/4.18.6/gdk/wayland/gdkmonitor-wayland.c)
destroys output proxies, while its
[display teardown](https://github.com/GNOME/gtk/blob/4.18.6/gdk/wayland/gdkdisplay-wayland.c)
disconnects the connection. Keeping an external monitor reference beyond that
disconnect caused the target's observed native shutdown crash; dropping those
references before disconnect fixes the reproduced exit.

## Camera compatibility

This initial resident-page mode rejects configured native and signed camera
inputs, including manual camera profiles, Standard handoff endpoints and the
Ultra `cameraProfile`, `nativeCameraControlFile` and `nativeCameraLeaseFile`
fields. Its Home overlay has not been qualified with a reversing camera.

The presentation component also lowers on busy, invalid or lost manager camera
status and never automatically raises on recovery. These are defensive UI
checks. They do not establish independent reverse-camera ownership or allow
camera coexistence. A future shared activation/surface-ownership contract must
withdraw Home before presenting reverse video and restore it after camera exit.
The standalone [camera controls](CAMERA_CONTROLS.md) remain separate work.

## Verification and remaining work

The evidence has separate boundaries:

| Evidence | What it proves | What it does not prove |
| --- | --- | --- |
| Portable configuration/ownership tests | Real private files, locks, subprocess cleanup, exact route admission, production display parsing and sealed packaging | Linux compositor behavior or phone continuity |
| Native GTK fixtures | Real widgets, late-reply/shutdown handling, readiness and mutation guards | Their renderer/layer/monitor boundaries are substitutes; they do not qualify touchscreen input |
| CM5 software-Wayland replay | Actual GTK/layer-shell windows, two private outputs, Home/Back pixel checks, output-qualified virtual-pointer events and display retirement | A physical touchscreen, phone session, scanout performance or camera |
| CM5 actual-GPU handoff probes | Main withdrawal, foreground holds of 10.5 seconds, fresh genuine receipts on return, repeated handoffs and deferred bootstrap without invented receipts | Their clock and scene are original fixtures; they are not a phone |
| Earlier source-checkout phone tests | User-confirmed Ultra and Standard Home/Back after ten seconds, correct screens/touch and Ultra → Standard → Ultra selection | Acceptance of the later immutable readiness release, cold boot or recovery |
| Current V3 deployment check | Exact manifest/unit verification and all five active retained-release service paths | Successful physical Home, Disconnect/reconnect or rollback |

The early foreground failure led to the timed ownership protocol described
above. Later real Wayland testing also exposed two GTK lifecycle requirements:
unmap/remap the occluded page to commit its new layer, and release monitor
references before disconnecting GDK. The implementation and regression fixtures
retain both fixes. Readiness fixtures prove that a background status request
keeps Home/Back available, while a mutation disables commands/navigation and
retains foreground ownership. Missing/stale/faulted observations withdraw
navigation rather than treating a live receiver process as usable video.

The shared CM5 build used disclosed GCC indentation and existing gauge-input
variant/optional diagnostic exceptions. New helper code passed strict Clang and
source sanitizer checks separately; those do not establish a clean default GCC
build or physical input qualification.

### Repeat the isolated Wayland check

Run as a nonroot Linux user with labwc, grim, wlr-randr and the GTK packages
above installed. The fixture always creates its own private runtime/configuration
and suppresses session activation updates; it never connects to a physical
compositor, receiver, camera or Bluetooth service.

```sh
python3 tests/native_home_wayland_test.py
```

It prints a fresh `/tmp/headunit-home-wayland-…` artifact directory with original
test screenshots, the page-only protocol trace and a structured report. An
explicit `--artifacts /absolute/new/directory` is supported; existing destinations
are refused. Successful reports require all pixel checks, real display retirement,
a still-held IPC worker during withdrawal, and zero page/marker exit codes.

The same check is optional in CTest:

```sh
cmake -S . -B /tmp/headunit-home-test-build \
  -DHEADUNIT_TEST_NATIVE_HOME_WAYLAND=ON
ctest --test-dir /tmp/headunit-home-test-build \
  -R '^native-home-wayland$' --output-on-failure
```

When that build also enables `HEADUNIT_BUILD_THEME_GPU`, CMake adds the actual
production `headunit-native-home-gpu-probe` and two separate checks:

```sh
ctest --test-dir /tmp/headunit-home-test-build \
  -R '^native-home-gpu-resident-(handoff|foreground-startup)$' --output-on-failure
```

These use the original synthetic clock/scene and the same private compositor.
Real receipt counts, retained/new PTS and captured pixels must all agree; a
surface layer getter or a running process is insufficient evidence.

### V47 scoped native-page check — 10 October 2026

Selected V47 (`e0a3e56`) runs the complete private headless startup fixture:
21/21 checks in 59.104 s, with original deadlines and four native GTK
regressions. Its private software compositor, synthetic underlays and GTK/GSK
OpenGL renderer do not qualify the physical display or phone.

A separate real-phone check uses a bounded Wayland virtual pointer on the
powered main output. Home opens the native Phones page while the signed cluster
continues; the Cluster page shows Map selected and “Driving assistance ·
unavailable” disabled. Clicking that disabled entry leaves the request bytes
unchanged with no pending request or error. Back restores the phone on the same
publisher, manager and connection generations. Two pairings and the selected
phone are preserved. Three explicit Disconnect/Connect checks separately
capture native Phones and the simulated backup cluster before connecting again.
The [handoff](HANDOFF.md) owns timing and failures. Private output captures and
receipts are in `overnight/functional-v47-*` and `overnight/reconnect-v47-*`.
These pointer checks do not establish USB-touch, hardware Home, Standard-mode,
cold-power or every native-page workflow acceptance.

### Atomic live readiness snapshots — source follow-up, 10 October

Unchanged V47's completed four-hour observation records 14,323/14,327 fresh
confirmed Ready samples. Four isolated samples report `starting` with no cluster
catalogue, each followed by Ready on the same manager/connection generation.
The independently read publisher is fresh at each failed sample. A bounded current receiver-log scan has no logged clock
unavailability/discontinuity or VM budget fault, and service signatures remain
unchanged. This is a recorded readiness-status dip, not proof of a transport
drop or of its cause. The completed observation retains all four as not-Ready
samples.

A deterministic real-file fixture reproduces a possible mechanism: atomic
publication unlinks an opened old inode while its private snapshot is being
checked. The earlier reader rejects that changed snapshot and returns starting,
even though a new valid publication is already available. Source now distinguishes
an observed snapshot change from ordinary invalid input, and readiness retries
that read **once**, opening and checking the current file again. The 512-byte
read bound, owned real 0700 parent, 0600 single-link file, schema, launch nonce,
sequence/generation admission and 1.5-second producer age remain unchanged.
It never reuses cached Ready or refreshes the producer's timestamp.

Fixtures cover replacement both before descriptor stat and after reading,
fresh second publication, faults, foreign launch, stale/future dates, malformed
readiness, changed permissions and two consecutive replacements. A second
failure stays unavailable; other private-state readers retain their previous
fail-closed behavior. The original replacement regression fails earlier source;
all thirteen readiness cases and five affected portable/interop checks pass
the repair. Exact source `b79da6e` passes format, 146 macOS, 171 Linux GCC,
171 Clang ASan+UBSan, 172 full-receiver GCC and 172 full-receiver sanitizer
checks, with 750 input hashes attested. It is source-only, unselected, and cannot
retrospectively turn V47's four dips into an uninterrupted pass.

### Physical acceptance

The retained readiness release is installed, but its physical workflows remain
open. The earlier packaged check showed a visible house button that did not
activate on touchscreen input, despite correctly aligned CarPlay taps. A real
output-qualified pointer event reached the button, which distinguishes compositor
routing from touchscreen qualification; it does not close that input gate.
Do not substitute earlier source-checkout success for acceptance of this release.

Complete these checks on the exact retained generation:

- Touchscreen Home/Back in Ultra and Standard, with a sustained foreground hold
  and both phone views/touch restored afterward.
- Native Disconnect/reconnect, phone-free underlay and fallback continuity.
- Pairing/removal, repeated phone-mode and multi-phone workflows, and manager
  loss/recovery.
- Cold boot without a phone and rollback to an intact retained release.

The source supports `homeButton.enabled: false`; a physical Home adapter is
still unimplemented. Camera coexistence remains excluded. The optional
`HEADUNIT_HOME_INPUT_DIAGNOSTICS=1` trace is limited to 32 Home events, contains
no coordinates or device identities and does not change dispatch. Use
[the handoff](HANDOFF.md) for the current next test, rather than inferring it from
historical test results.

### V87 manager thread observation — 14 October 2026

One read-only observation ending at 18:38:36 UTC samples the actual manager's
six threads eleven times over 10.014 seconds. Kernel boot, service invocation,
PID and process start ticks remain stable. At the actual 100 ticks/second,
one Python thread advances 107 CPU ticks, about 10.68% of one core; the manager
main thread advances 12 ticks, about 1.20%. The other Python thread advances
six ticks, about 0.60%; the D-Bus thread advances one. Two observed threads
advance zero ticks. Missing or retired threads are unavailable, never counted
as zero. Each retained thread record includes its original observation date,
start ticks, CPU ticks, scheduler counters and wait channel.

Thread names do not identify the Python task, and the final wait channel does
not explain its earlier CPU work. This is no stack attribution, performance
repair, phone-frame or PCM evidence. No service or renderer changes occurred.
Original records, observer source, stderr, summary and hashes remain outside Git
under `20261014/codex-phone-session/resumed-20261014T1836/`. Further profiling
needs a distinct reviewed question; repeating the earlier aggregate idle soak
would not establish a cause.
