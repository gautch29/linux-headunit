<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Cluster layouts

## V84 remembered-layout reconnect — 14 October

Frozen `3bd3db9`, one warm manager disconnect/connect: Modular is actually
phone-confirmed before disconnect and recorded in the private pack/version
preference. Original successor samples show `restoring` Modular, then
`confirmedActive` Modular with empty `preferenceError`; its confirmed disk record
remains identical. Settled cluster pixels show useful Modular instruments and
labelled simulated assistance. Both panels/audio remain available after return;
five service invocations remain unchanged, with zero automatic restarts.

The observed original MapsMedia layout and its confirmed record are restored
through the current publisher afterward; Trip and the diagnostic manual-mode
selection remain unchanged. No explicit colour request is sent: each style uses
its observed phone palette. This is one warm layout-restoration check, not boot,
all-layout, phone-Customize or physical-touch qualification. Automatic Sport
profile confirmation reports an error on this reconnect while actual Modular
remains intact; requested profile and applied preference stay separate.

Disconnected pixels show ordinary native Trip fallback, rather than saved signed
gauges. [Local signed return](LOCAL_ULTRA_CLUSTER.md) remains open on V84.
Private original source, failed preconditions, dated status/cache metadata, USB
log and settled captures: `20261014/codex-phone-session/preference-continuation/`.

Select an installed Ultra cluster layout while the iPhone stays connected.
This reference-pack guide contains dated evidence from 30 September–11 October
2026; current findings come first and older experiments remain historical. It
documents observed behavior and our original host adapter, not an official Apple API.

## Customization and remembered state — 11 October

### Large Now Playing and content navigation — 11 October

The user's Aston Martin screenshot shows a large Now Playing area above compact
instrumentation. This fits the installed **MapsMedia** layout: read-only inspection
of the authenticated pack finds a 1920 × 720 `mapsMedia` zone with a carousel
configuration and compact local instruments below. Its declared `DCA_Map`
component is not a separately declared Music component. No signed resource was
changed. The live V68 host catalogue exposes no content choices for MapsMedia;
an empty local catalogue is not proof that its phone carousel supports only Map.

[Apple's dynamic-content design chapter](https://developer.apple.com/videos/play/wwdc2024/10112/)
explicitly includes Maps, Now Playing, trip information, tyre pressure and ADAS,
with steering-wheel controls for cycling content. The
[architecture session](https://developer.apple.com/videos/play/wwdc2024/10111/)
places Maps/audio in phone-rendered Remote UI and driving instruments in Local
UI. Large Music therefore does not require us to redraw album artwork or replace
the gauge pack. This is a supported platform design and compatible pack structure,
not yet a successful live Music selection on this appliance.

The real-phone V72 input route is qualified for scoped page navigation. A
separate Consumer Control HID device is associated with the cluster video UUID
only in native Ultra `/info`. Its descriptor uses the
[USB-IF Menu controls](https://www.usb.org/sites/default/files/hut1_5.pdf)
(Pick, Up, Down, Left, Right and Escape). Up/Down browse content; Left/Right
change layouts. Native Previous/Next buttons map to Up/Down and have actual
GTK-button and live-phone checks. Main touchscreen reports remain separate.
The manager requires the fresh publisher, both Ready outputs and idle layout;
requested input is not an applied page acknowledgement.

Live Cobranded cycles Map → labelled simulated ADAS → Current Trip → tyres →
empty CarRadio → Map. MapsMedia cycles Map → EmptyWidget → empty CarRadio → Map.
Signed UI-Sync readback identifies `com.apple.CarRadio` and `EmptyWidget` for the
blank pages. They are not confirmed Now Playing. Trip shows four unavailable
readings; tyres show four unavailable pressures. Body and tyre request overlays
also render on large Map and close back to it. No physical provider is qualified.

Main Apple Music is confirmed with advancing tracks and album artwork. Wallpaper
selection, main Maps foreground, pause/resume and short Select on blank pages
have not restored large Music; do not repeat them as untried fixes. Bounded
incoming NTP timestamp adaptation removes signed transition metadata errors.
Accepting observed `speechMode=-1` prevents rejecting whole authenticated
`modesChanged` packets; ownership updates now return 200 and the controller is
observed owning main audio. Neither repair restores the missing Music scene.
The observed carousel offers no weather or calendar page; this does not
establish a platform-wide restriction.

**Resolved on V74 — 12 October (real iPhone).** The phone's own log, read over
USB, settles the cause. DashBoard already resolves Now Playing to Apple Music,
and the phone owns mainAudio. The large carousel's media page is a
`com.apple.CarRadio` (Media app) Dynamic Content Area scene. On connection,
CarRadio logged `Received a car, but it has no media. Waiting on car` and never
created its cluster Now Playing view. It hosts that view only after the car
declares both CAF `Media` `0x03000001` and `NowPlayingInformation` `0x0F000001`.
We declared neither, so the page stayed black.

V74 adds an opt-in `media` section to car-settings. It declares one fixed
`MediaSource` `0x14000006` (identifier `0x30000019`, label `0x30000001`) and a
`NowPlaying` `0x14000001` naming that source current (`0x32000023`). Type IDs
come from the inspected CarAccessoryFramework type tables. No phone title,
artwork, playback state or unverified enum is mirrored into the car. With
mainAudio on the phone, CarRadio logs `Cluster now playing source updated to
iOS` and draws Apple Music itself. Live MapsMedia then cycles **Music → Map →
EmptyWidget (wallpaper) → Music**, with title, artist, advancing progress and
artwork; the main screen and Climate are unchanged. Captures and phone logs are
private in `20261012/now-playing-modes/`.

**EmptyWidget explained (13 October, captured phone logs).** The phone builds
each zone's carousel from widget groups and logs the result: Cobranded
`[ADAS input, Map, CarRadio, CarTirePressure, CarTrip]`; MapsMedia
`[Map, CarRadio, empty]`, with `group=charge declared=0`. The wallpaper page is
the MapsMedia "charge" group, which resolves to empty here, while Cobranded's
falls back to Trip. CarPlayAssetUI also logs `itemWillAppear dropped for
…EmptyWidget: no instrument in slots.first`. That rule is newer than the
inspected iOS 26.1 code, so no declaration is guessed; a fitted EV's Charging
accessory is the likely legitimate filler.

Still open: Cobranded's carousel, rapid choices, reconnect without Music and
pause/track-change behaviour. EmptyWidget remains a wallpaper page. The
receiver now also logs `AirPlay modes mainScreen=C/P mainAudio=C/P …` on
change; the V74 session starts at `2/2` for both and the phone then takes them.
Private source bindings, qualification and captures are in `cluster-modes-v72/`.
On 12 October V73 confines the legacy startup Map deep link to Standard after
actual feature agreement. Full component checks pass and live logs show no
legacy request, but the same large carousel remains blank. Rebuilding DigiRing
→ MapsMedia with actual Apple Music playback does not change this result.
Private V73 receipts are in `20261012/music-fix-v73/`. Phone-side scene logs are
requested through USB on the Mac. V74 below resolves the missing Music route.

The private `clusterLayoutFile + .input` request is a bounded six-line `HUI1`
record: version, launch nonce, connection generation, sequence, actual acquired
monotonic nanoseconds and action. Producer replacement pins a private directory;
the receiver admits only its owned, single-link 0600 regular file in a 0700
directory. Inputs expire after two seconds, newer stale/busy requests are consumed
without replay, and an accepted press is followed by release after 100 ms or
readiness loss. Release backpressure is retried; connection teardown retires the
HID device. Parsing, private-file validation, admission and encrypted report tests
are component evidence. V72 adds the scoped live page observations above;
Music, sustained navigation, focus and lifecycle acceptance remain open.

Keep the working input route and current signed pack. Remaining page work is
Music availability, applied readback, rapid choices, focus/dismissal and retirement,
plus fitted dated readings. Do not redirect the ADAS video input or mirror phone
metadata into the vehicle-owned radio accessory as a substitute for Now Playing.

| Content | Current evidence / remaining work |
| --- | --- |
| Map | V72 large MapsMedia and between-gauge Map captures |
| Now Playing | V74 large MapsMedia Music page live with title/artist/progress/artwork; requires the opt-in car `media` declaration |
| Trip | V72 signed page selected through native controls; four unavailable readings, fitted data open |
| Tyres / body status | V72 ordinary tyre carousel and body/tyre request overlays; artwork renders, physical data unavailable |
| ADAS | V72 signed page shows explicitly simulated bench lanes |
| Charging | Opt-in contract for fitted EVs; omitted from combustion bench profile |
| Weather / calendar | Not offered by the observed cluster loops; main widgets do not prove a cluster route |

The six authored layouts and eleven palettes remain supported customization
choices. Wallpaper behavior and remembered/offline preferences retain their
separate open qualifications below.

A read-only review of the authenticated installed pack and our host bindings,
checked against [Apple’s design session](https://developer.apple.com/videos/play/wwdc2024/10112/)
and [architecture session](https://developer.apple.com/videos/play/wwdc2024/10111/),
separates platform flexibility from the controls this pack actually exposes.
Apple describes automaker-specific gauge design and vehicle-local startup before
iPhone reconnects. Those are targets, not current appliance qualification.

The installed pack exposes six selectable layouts and eleven palettes. Gauge
fonts, frames, stroke widths, angles and tick geometry are authored in its signed
resources; the inspected configuration parameters do not expose arbitrary gauge
geometry. A palette named Custom is an authored choice, not arbitrary RGB input.
Host selection admits catalogued layout/palette/content identifiers and waits for
phone confirmation. Vehicle inputs supply readings, supported ranges/units and
authored drive-mode choices, not unrestricted artwork. The current cluster
requires dark appearance; generic appearance helpers do not prove light support.

We own the native fallback artwork, warning overlay and declared Punch-through
camera/ADAS content. The narrow startup branding greeting substitution does not establish
support for replacing signed gauges. A bespoke Ultra pack would require an
authenticated compatible package and matching phone metadata/resources. No
public pack-authoring/signing workflow was found in the reviewed Apple material;
[CarPlay for developers](https://developer.apple.com/carplay/) directs vehicle
system work to MFi. Ordinary CarPlay app instrument-cluster APIs are not a gauge
asset authoring interface. Signed resources remain unchanged and outside Git.

The signed persistence helper also stores connection and animation state. Its
production `Runtime::bind_persistent_storage` remains unbound deliberately.
V81 retains a filtered host record: last phone-confirmed declared layout
and palette, scoped to verified pack/version and cluster control path. No phone
identity, pairing, remote content or animation state is retained. Supported
appearance remains the current pack's dark configuration.

Private legacy `LAYOUT\n` and current `LAYOUT\nPALETTE\n` records are bounded
and catalogue checked. Disk reads are preloaded and write buffers copied to a
coalescing worker; temporary-file/rename and file/directory fsync stay off VM
and control ticks. Corrupt/symlink/exposed records, removed palette, interrupted
ownership, failed writes and a redirected directory have component checks.
Subsequent settled phone Customize changes are observed; first startup state
is a baseline, not a new unrequested preference. Actual phone reconnect/colour
restoration remains open while the phone is absent.

The source-only preference-file regression also combines an accepted paired
layout/colour write held at file `fsync` with actual `UiWorker` capture retirement.
Both successful sync and injected `EIO` retain the existing retirement contract:
`finished()` waits for controller/disk destruction, late worker ingress stays
closed, and an unconfirmed newer request never becomes a saved choice. A successor
controller rechecks removed layout/colour entries, rejects the old publisher's
request, and keeps foreign pack/version state separate. This adds coverage without
a production repair. Threads, private files and worker retirement are real;
controller readback, catalogues and poll dates are synthetic. It does not exercise
`InstalledUi`, a complete signed session, phone confirmation, power loss, GPU
presentation or physical reconnect. Required coherent release and bench
qualification remain separate. Exact reports and cleanup evidence stay outside Git.

V79 adds local GPU rendering on the original native background surface before
phone connection, with real local monotonic animation time and original vehicle
acquisition/freshness. The signed controller requires an empty declared remote
layer; it has no producer or offered phone frames. V81 validates saved choices in
a separate metadata VM before the display VM starts. An unknown saved layout
preserves the configured signed default; invalid resources still retire
local objects and reveal ordinary native instruments; corrupt state uses the
verified profile default. Earlier V79 MapsMedia and synthetic DigiRing/Vibrant
fixtures have actual CM5 captures, with explicitly simulated readings. The fixture
is not phone confirmation. First local presentation is 0.871 s for that fixture
and 1.046 s on V79 installed startup, not power-on or door-wake timing.
V81 unknown-cache recovery is pixel-checked on the CM5; see
[local cluster evidence](LOCAL_ULTRA_CLUSTER.md#cm5-evidence--v81-13-october-2026). Connected
handover, late phone, repeated boots, physical providers and wake/power remain
open. Private source/qualification/visual evidence: `20261013/local-gauges/`.


## Overnight V42 layout check — 10 October

The following initial check is on retained V42. The later V43/V44 evidence
is scoped separately below.

The actual five-service graph verifies as classic-page V42, source `50e9492`;
it is based on V40 and does not include cluster-controls V41's failure-feedback
repair. Both physical outputs are powered on under the user's latest overnight
instruction. The scoped initial run preserves both pairings and the same
manager/receiver publisher while confirming Modular, DigiRing, DigiBand and
DigiBandTicks in 1.08–1.11 s poll bounds. Their output captures show the centre
Map and their catalogues mark DCA_Map active. The following explicit Map
requests repeat an already-active selection: **no new Map transition is proven**.

Cobranded selection retires the signed session twice on the unchanged 20 ms
CPU policy: 20,107 µs with one task resume in the four-layout pass, then
20,061 µs with five resumes in a direct MapsMedia → Cobranded repeat. The script
wrapper reports `APL/Thread.lua:135`; that line alone does not identify the
expensive signed task or native callback. These are failures, not a six-layout
pass or evidence that the budget should be increased. Automatic reconnect
returns to confirmed MapsMedia/Vibrant with both pairings retained. A later
MapsMedia → DigiRing switch confirms, while a same-DigiRing Red request retains
Green and ends after an 8.336 s poll bound with V40's generic confirmation
error. This confirms that the selected V42 lacks the specific V41 feedback;
its subsequent restore-request receipt read the prior error before the new
request was consumed and cannot prove restoration. Fresh final state must be
checked separately.

Private dispatch/state receipts, both-panel captures and the aborted restoration
attempts are retained in `~/.cache/headunit-qualification/20261010/overnight/`
and `/home/headunit/headunit-overnight-20261010/`. The brief earlier DPMS-off
interval and its output-unavailable retirement are excluded from normal powered
presentation evidence. No physical car input or touch-latency pass is claimed.

### V43 timeout attribution

V43 freezes source `60dc41d` after all six local matrix jobs pass, plus seven
affected CM5 suites under both GCC and Clang 19 ASan+UBSan. It includes the
specific cluster-feedback repair and bounded first-fault stack recording.
After whole-owner selection it reaches fresh confirmed MapsMedia/Vibrant by
37.052 seconds from selection start and completes a separate 20-second Ready
tail. Both physical output captures are retained. This timing includes owner
retirement and restart; it is not a Connect or kernel-boot measurement.

A direct Cobranded request again retires the session on the unchanged 20 ms
policy: CPU 20,072 microseconds, five resumes. The first stack now points through
`APL/LuaUtils.lua:398` and `APL/Rect.lua:46` to
`DigiBarMarkerGauge.lua:325`, during marker rectangle creation. It identifies
where the accumulated turn budget expires, not the entire cost distribution.
The same-publisher restoration attempt aborts after retirement and proves no
restoration. A 120-second recovery observer reaches no fresh Ready. A following
same-phone Disconnect/Connect through the native manager reaches fresh confirmed
MapsMedia/Vibrant in 40.035 seconds, then completes 100 seconds without sampled
readiness loss or publisher change. Both pairings survive. This misses the
30-second recovery check. The diagnostic alone does not repair Cobranded.

The separate V43 feedback check freshly confirms DigiRing/Green in 0.895 s.
Requesting Red retains Green and reports the specific instruction to change
colours using Customize in CarPlay after 8.189 s. Restoring MapsMedia/Vibrant
consumes a new state and clears the error in 0.908 s on the same publisher.
An explicit Map request remains a no-op because Map is already active; a real
centre-content transition and the complete six-layout pass remain open.

### V44 strict Clang comparison

Retained V44 uses the same source and signed assets with Clang 19,
`-O2 -g0 -DNDEBUG`, without sanitizers or GCC warning exceptions. Seven programs
and nine component checks pass (36.57 s). The initial observer already sees
fresh Ready when it begins 52.896 s after selection start, then holds a
20-second tail. The blind interval prevents an exact startup-time comparison.

Cobranded again faults: CPU 20,074 microseconds, wall 20,106 microseconds, five
resumes, at `APL/LuaUtils.lua:1950`, `APL/LuaUtils.lua:1691`,
`APL/GradientLayer.lua:142`, `Ticks.lua:466`. The first exhausted checkpoint
moves to point/gradient/tick construction; this is no successful selection or
whole-task cost profile. Same-publisher restoration aborts on retirement.
Native-manager recovery on the same paired phone reaches fresh confirmed Ready
in 28.590 s and holds 100 seconds without sampled readiness loss or generation
change. Both pairings survive. This scoped attempt meets 30 s, while the V43
attempt did not; it does not establish a general reconnect or compiler result.
[Worker checkpoint cost](UI_WORKER.md#execution-budget-checkpoint-cost) owns the
following source investigation. The 20 ms limit and signed bundle stay intact.

### V45 layout and content checks

V45 uses source `4592c04` after the complete local matrix and affected CM5 checks
pass. It reduces early-turn CPU-clock syscall overhead without changing 20 ms
or signed resources. Direct Cobranded confirms in 0.895 s, then MapsMedia
restores in 0.893 s, on the same live publisher. Output captures show the signed
Cobranded gauges and centre Map while the main phone launcher remains alive.
Gauge values are from the retained simulated bench profile.

The first combined content pass confirms Modular, then its ADAS request expires
unconfirmed after 8.551 s. Map remains visible and active; the subsequent Map
request is a no-op and clears the error. DigiRing then confirms, but neither
remote content is marked active. Explicit Map expires after 8.368 s even though
the output capture still shows Map. The pass stops there and restores
MapsMedia/Vibrant in 1.086 s. Visible pixels do not prove a new acknowledged
content transition, and no requested ADAS state or physical provider is claimed.

A separate style-only pass completes all six layouts on the same publisher:
Modular, DigiRing, DigiBand and DigiBandTicks confirm in 1.099–1.116 s;
Cobranded repeats in 0.897 s. Each gauge catalogue reports Map active and both
outputs are captured. Initial MapsMedia is a no-op, while final restoration is
a real confirmed transition (1.086 s), completing the six distinct styles.
DigiRing/Red retains Green and reports the specific Customize instruction after
8.361 s. Restoration clears the warning. Arbitrary native palette changes and
actual Map/ADAS transitions remain open. Diagnose the missing remote-choice
acknowledgment before altering observation state or treating a request as applied.

### V47 normalized catalogue and live rejection

V46's first pass falsely reports assistance available: the signed parser
normalizes the instrument declaration while the host adapter checked only its
raw table. Source `e0a3e56` handles both forms. The normalized fixture fails
V46 source and passes the repair normally and with macOS ASan+UBSan. All six
local matrix jobs, affected CM5 checks and 21 private headless startup checks
pass before V47 is sealed and selected. Exact bindings belong in HANDOFF.

The separate V47 live pass confirms all six styles on the same publisher and
phone, with both powered output captures:

| Style | Observed confirmation bound | Centre catalogue |
| --- | --- | --- |
| MapsMedia | 0.866 s; initial no-op | Existing active style |
| Modular | 1.081 s | Map active/available; assistance inactive/unavailable |
| DigiRing | 0.872 s | Map active/available; assistance inactive/unavailable |
| DigiBand | 1.091 s | Map active/available; assistance inactive/unavailable |
| DigiBandTicks | 1.087 s | Map active/available; assistance inactive/unavailable |
| Cobranded | 1.100 s | Map active/available; assistance inactive/unavailable |

For each of the five gauge styles, a native-manager assistance request rejects
as unavailable before changing the bounded request file. Its bytes, catalogue,
publisher and phone stay unchanged, with no pending request or error. A
separate native-page check shows the disabled “Driving assistance · unavailable”
button; clicking it also preserves request bytes. DigiRing/Red keeps Green and
gives the specific CarPlay Customize instruction after 8.575 s. MapsMedia/Vibrant
restores in 1.103 s and clears the warning. Private receipts and captures are in
`overnight/cluster-styles-v47` and `functional-v47-*` under the handoff's directory.

This qualifies the observed styles and negative admission. Map stays active;
no new Map content transition is claimed. No local assistance renderer or
physical car provider is qualified, and arbitrary native palette changes remain
open. Do not substitute request acceptance or input registration for fresh
provider data or phone-confirmed active state.

## Format selection interface — 13 October source

The native manager exposes a catalogue of display-format labels backed by the
existing declared layouts and selection owner. Minimal → DigiBand, Sport →
DigiRing, Full Gauges → Modular and Large Map / Media → MapsMedia; every other
style retains its authored UUID/name. Labels do not add layouts, alter signed
geometry or assert that native/signed auxiliary gauges match. Only the matching
current catalogue can admit a request; phone-confirmed state remains separate
from local active/pending state. [Native format data interface](NATIVE_CLUSTER_PAGES.md#format-data-interface--13-october-source)
owns exact IDs/status/actions for native UI integration. GTK rendering and actual format-label
selection, reconnect and clearing checks await the bench owner.

## Catalogue

Pack `44C9FA31EC`, version `985`, declares six primary cluster styles on a
**1920 × 720** logical canvas. The physical display fits that canvas with bars
when needed. Identifiers are pack-specific.

| Style | Identifier |
| --- | --- |
| Cobranded | `6C45CC47-EC01-527A-B40D-1316AA9C936F` |
| DigiRing | `8BC9ABB1-CB20-595E-92BF-D8F099BF3515` |
| Modular | `0D38B526-1271-575D-8A92-393A86B718F9` |
| DigiBandTicks | `1B0484EC-1933-524D-80F8-2FC1FC7B4B2D` |
| DigiBand | `1B54747B-5609-52E0-A1DD-9CC8EF0B11BE` |
| MapsMedia | `DB7C6944-5DFF-58DF-94E9-91006781FA93` |

Signed resources remain unchanged; none are embedded in our code or committed.

## Palette, centre content and speed boundary — 8 October

New source exposes the verified pack's colour palette catalogue and the current
layout's primary DCA components alongside style selection. The reference pack
has eleven palettes. The five gauge layouts declare Map and driving assistance;
MapsMedia places the phone's map across the main field. Native controls send a
bounded `HUL2` request containing the existing publisher identity, layout UUID,
optional palette, and paired zone/component UUIDs. `HUL1` style requests remain
compatible. The sole VM owner sends an ordered request through the signed
session and waits for ready/idle, matching layout, palette and component state.
A matching layout alone cannot confirm an appearance change. Unsupported or
stale choices cannot become arbitrary code, resource paths or a new owner.
The status adds optional `palettes`, `contents` and `activePalette` fields.
Each new content entry also carries `available`: all input identifiers declared
by its instrument slots must be registered with this display VM. Entries from
older publishers without this field retain their previous admission behavior.
Availability does not prove input freshness, a qualified assistance provider or
phone confirmation. The signed `DCA_ADAS` requires `adasView`, which this
appliance does not register. Its native button is disabled and labelled
unavailable; selector admission preserves the previous request file on rejection,
and the VM refuses dispatch even if a direct request bypasses that selector.
Map has no local instrument slots and remains eligible. An active but unavailable
entry cannot confirm a pending request. Registration and requests never set the
active highlight; normal phone-driven observations remain necessary. The first V46 pass confirms Modular but exposes a gap: its signed
parser normalizes the declaration to `kind="inputStream"` and
`streamIdentifier`, while the adapter checked only the original table form.
Assistance therefore incorrectly reports available; the pass stops and restores
MapsMedia. The normalized-form regression fails V46 source and passes the
repair. Selected V47 completes full component qualification and the scoped
real-phone rejection and disabled-button checks above.
Palette and centre-content restoration across reconnect remains unqualified.

The 9 October native-manager admission regression now covers requests from the
page through the real manager and selector to their bounded `HUL2` file. The
manager's shared request whitelist previously omitted `palette`, `zone` and
`component`, rejecting those page controls before selector admission. The fix
admits these fields only for `cluster_layout`; stale publishers, unsupported
choices and incomplete zone/component pairs still cannot replace the request.
This portable component result does not resolve the separately observed direct
selector phone-confirmation timeouts below or qualify the physical controls.

V23 deploys these controls, but live all-option qualification fails: a
phone-confirmed MapsMedia → DigiRing switch shows a centre map on HDMI-A-2,
while same-layout Blue palette, combined Blue/Map, and ADAS requests time out
without phone confirmation. The local signed layout reports neither DCA option
active despite the visible map; its remote-only component has no local gauge.
Do not treat the local component getter as complete phone state. The current
phone Theme Selector opens a background page through Customize. User-confirmed
Set on Topography changes the local palette Green → Yellow but leaves the
physical cluster background black. The centre was blank afterward, but a prior
ADAS request had no intervening readback; this trial cannot attribute map
removal to Set. Phone palette changes arrive independently of failed native
palette requests.
A private captured cluster HEVC replay ends in black, but its last record
predates this Set test; it cannot attribute the new failure to phone or host.
Portable selector/runtime checks pass; signature-checked CM5 offline runs discover eleven palettes and
Map/ADAS on DigiRing. ADAS has no qualified physical provider. Media, weather,
calendar and other phone widgets are not separate locally declared DCA choices;
do not fabricate them by feeding media into the ADAS input. Phone widget/focus
selection and wallpaper textures need separate live qualification.

### Fresh phone/background qualification — 9 October

V25 adds optional `phoneVideo` layer diagnostics (visibility, opacity and actual
current-PTS availability); these are not freshness or GPU-receipt claims. Live
DigiRing reports visible/opacity 1/current PTS. A controlled main Landscape Set,
followed by the Color checkmark, changes the palette to Yellow, retains Map
between gauges, and leaves the cluster black. The cluster packet capture is
still growing at readback; its latest complete HEVC frame contains Map on black,
matching the physical composition. This trial places the missing Landscape
upstream of the host compositor; it does not prove the phone's root cause.

V26 stops inserting Map into every unspecified layout request. Live DigiRing
then has an empty centre; explicit Map restores its map. Phone Blue updates
real gauges, but Landscape still stays black. V27's original host adapter
observes the signed layout setter after success and retains only primary,
remote-only components declared by that layout. Its getter merges these with
local selections, with local components taking precedence. Missing/unknown
replacement selections clear the observation; new objects begin empty, and
failed setters cannot change it. The normal ready/idle checks still gate
confirmation. 131 portable checks and three CM5 runtime/selector checks pass;
no GitHub build is run under the user's explicit billing-related override.
Live V27 reports DCA_Map active after the phone-confirmed DigiRing switch and
physical readback shows the map. Same-Map selection no longer times out. Main
Now Playing shows artwork and an advancing song. An ADAS request is not active
at the scoped readback; final Map is active with no pending request/error.
The same-Map request is a no-op, not evidence of a new phone transition.
Background, phone widget selection and real between-gauge media remain open.

### Continued wallpaper investigation — 9 October

On V30, the phone's Color picker changes the physical DigiRing gauges between
Red and Blue. Committing Topography also changes the Theme Selector's preview;
the cluster stays black. The main app launcher subsequently shows Topography,
but the delay/reconnect means this does not prove the setting applies only to
main or identify why cluster resolution failed. Most black reference screenshots
do not establish that wallpaper support is absent from this style.

An earlier V27 trial also increased the signed scene's transition-metadata
rejection counter during Landscape selection. That counter groups three
different errors; it does not establish which one occurred or prove a causal
link to wallpaper. The existing opt-in `diagnosticScriptLog` can forward bounded
Metadata/LayoutController/Workspace messages into private receiver logs. It is
not enabled in selected V31. Unselected
`cm5-20261009-wallpaper-trace-v32` adds `diagnosticScriptLog=600` to the retained
V31 binaries and configuration graph; stage-only and actual old-helper preflight
pass. All 58 payload files match V31 after bound-path normalization except that
added setting. No service selection or wallpaper result is claimed for V32.

The host's bounded UI route preserves the command tree, and the signed controller
uses full-display phone video behind its gauges. A bounded HEVC inspection found
no auxiliary layers in the inspected V30 cluster capture. That older, capped
capture cannot attribute later picker actions. Source and bytecode inspection
found no cluster consumer of a local wallpaper image; inventing a host backdrop,
black-key transparency or an undocumented panel property would not qualify the
phone's setting.

USB access to the paired iPhone works alongside the wireless bench session.
The phone reports iOS 27.2; the installed simulator files are iOS 26.3. Their
wallpaper preferences are scoped by display and layout, and asset resolution
checks display/layout/wallpaper identifiers. This identifies the next diagnostic
boundary, not an iOS 27.2 diagnosis. The first bounded USB captures contain no
useful wallpaper-resolution result; a recent-archive request returns no data.
Private phone logs, signed assets and SDK inspection remain outside Git.

V30 also recorded three presentation-timeline retirements and two GPU completion
timeouts during this investigation. Their individual causes are not all proven.
V31 includes the separately tested native presentation-clock correction described
in [GPU output](GPU_DISPLAY.md). It reaches fresh Ultra readiness; after the user
takes the phone away, physical main and cluster captures show native Phones and
simulated Trip. No completed V31 wallpaper trial is claimed. Resume with a
process-focused USB trace started before one DigiRing Landscape/Topography
selection, matching fresh publisher identity, and physical/fresh encoded-frame
readback. The repository converter now accepts `--codec hevc`; saved V31
extraction passes all 2,452 frames through FFmpeg, independently of that pending
trial. Wallpaper and sustained reliability remain open.

### Original-phone comparison — V40, 9 October

The user's original already-paired iPhone is available while the sister's phone
is absent. V40 retains the same renderer and signed scene path as V39, adding
clock/Bluetooth diagnostics only. Original-phone Home → Back passes on the same
connection. An actual catalogue-bound Vibrant request is confirmed through
`activePalette`; DigiRing then MapsMedia both confirm, and MapsMedia is restored.
The physical DigiRing capture shows a **black background with a centre map**.
The retained sister-phone V39 capture shows the mountain backdrop and an empty
centre at the same DigiRing/Vibrant selection.

Layout/palette match, but centre content and release generation differ. This
is not a controlled phone-only or same-build comparison and does not establish
why the wallpaper setting differs. The new diagnostic source adds no background
pixels, wallpaper command or signed-resource override. Match the phone's centre
content and selected wallpaper/pack state on the same retained release before
attributing the difference to phone resolution or renderer behavior. No pairing
reset or USB log capture is performed. Private screen/status receipts are in
`takeover-testing/clock-reconnect-v40/original-phone/wallpaper/`.

The user's next layout check selects **Modular** on that same V40 connection.
Its physical capture also shows a black background with a centre map. Switching
layouts selects Blue; a subsequent catalogue-bound Vibrant request is not
confirmed by the phone and Blue remains active. Re-selecting Modular/Blue
confirms with a fresh matching publisher, idle catalogue and no error. Modular
is left selected. This adds a second black-background layout observation, not a
matched-palette comparison. Private capture/status evidence is under
`clock-reconnect-v40/original-phone/modular-check-877291878384/` on CM5.

### Partial choice confirmation — V41, 9 October

A same-connection V40 trial requests DigiRing/Red from Modular/Blue. The phone
completes DigiRing in about one second but sends Green in its layout transition;
Red never confirms and the existing eight-second request bound expires.
Modular/Blue is restored. This establishes a delivered layout request with a
different phone-selected palette, not a missing command or a palette repair.

Source `5498659` distinguishes a completed style with an unconfirmed colour,
centre content, or both from a style timeout. Colour feedback directs the user
to Customize in CarPlay, where phone-driven changes have prior live evidence.
A rejected appearance choice is no longer described as an undeclared layout
when that layout remains in the catalogue. The actual highlight, confirmation
checks, eight-second bound, coalescing and preference rules stay unchanged.
Partial acceptance cannot save the requested appearance or retry automatically;
a fresh confirmed selection clears the error. No signed resource is changed.

The affected five suites (`theme-script_runtime`, `theme-layout_control`,
`theme-layout-control-interop`, `phone-management`, `cluster-layout`) pass
portable macOS (1.11 s), macOS ASan+UBSan (2.31 s), CM5 GCC (1.44 s) and CM5
Clang 19 ASan+UBSan (4.05 s). Mac leak detection is off; Linux is on. The initial
Mac sanitizer run cannot load the obsolete libplist dylib cached by its old
build directory; refreshing that dependency cache resolves it. No suppression
is added. CM5 GCC retains the documented bench exceptions. Seven independent
AArch64 programs are frozen against all 750 validated source inputs; full
suite/matrix and GPU fixtures are not rerun for this feedback-only change.

V41 is selected with V40 retained for rollback. The old helper verifies its ten
original files and identical five-unit rendering. Phone-driven colour requests
remain unqualified; this change improves failure feedback and does not establish
all-layout, centre-content or reconnect qualification. The V40 live connection
drops before the attempted Home/Back check; that check is incomplete. V41
reaches no fresh confirmed Ready within the 180-second selection observer while
phone availability is unknown. The six-layout/Map and physical feedback checks
therefore remain open. All five services are healthy with zero observed
restarts; the physical native fallback shows explicit simulated gauge data,
unavailable Trip fields and no phone connected. The observer is retired. Private
receipts are in `takeover-testing/cluster-controls-v41/` and the corresponding
CM5 integration directory.

### Local rendering fidelity audit — 9 October

A portable CPU render of every layout with the exact V27 vehicle profile
reproduces the live HDMI-A-2 capture, including the grey double rectangle in
DigiRing's right dial. That shape is the pack's redacted gear text (`---`),
drawn because no transmission sample exists. Its visible seams exposed a
global decoding fault: all 2,875 pack PNGs, including raster-font atlases,
carry the tEXt chunk `hasPremultipliedAlpha=yes`, and no sampled pixel exceeds
its alpha. Cairo's PNG loader treated them as straight alpha and multiplied
again, darkening every antialiased glyph edge, icon, glow and translucent image.
The decoder now uses libpng and premultiplies only undeclared images, with
Cairo's rounding so ordinary PNGs are unchanged; declared colours are clamped
to their alpha. The redacted placeholder is now one clean pill, and a synthetic
regression fails when the declaration is ignored.

The user's photographed Aston Martin Cobranded cluster shows the wings and
`Sport` in the right dial. The signed rules show those wings only for a
non-numeric automatic state (`transmissionMode` rule 2); unavailable or numeric
gears show the gear text instead (rule 1). Any engaged gear above zero is
numeric, so Drive in 4th shows `4`, while Park in neutral shows the wings. The
drive-mode label needs a bound catalogue and selection. The bench
`SimulatedVehicle` now publishes an explicitly simulated automatic Drive state
and the reference comfort/sport catalogue with Sport selected. The HUV2 bench
sender drops these fields because that format cannot carry them. The selected
V27 profile binds transmission but not drive mode: regenerate it with
`--drive-mode-config` and `config/drive-modes.reference.example.json` before
expecting `Sport` live. Offline Cobranded renders match the photograph's dial
centre; eight DigiRing palettes and all six layouts render without masking or
colour artefacts. None of this is deployed or phone-qualified.

Live V30 still drew the redacted gear although its simulator published Drive.
The signed transmission updater rebuilds its state from the signals present in
one update batch, while our vehicle service sent only changed readings: a gear
change (3 → 4) arrived without the unchanged selector, so the pack saw no
selector and redacted. The local profile now declares update groups by service
and characteristic group, and the service resends every subscribed member of a
group when any member changes. Offline fixtures never change, which is why they
rendered correctly. A gauge-input regression fails without the grouping. The initial service fix is preserved in native UI's staged, unselected gear V31.
Later repair `089fd82` also groups the local ScriptView transmission handler,
which is independent of Service delivery. Selected combined V35 includes both;
its scoped phone captures show changing simulated gear, while physical vehicle
acquisition and the Sport label remain separately unqualified. See [HANDOFF](HANDOFF.md).

The photograph's album art, odometer row and telltale strip are not local
gauge content: the first two come from the phone's video, and the telltales
belong to the default-off [Ultra warning overlay](ULTRA_WARNING_OVERLAY.md).
The Landscape backdrop also remains phone-side. The signed controller places
full-display `iPhoneVideo` behind every gauge over an opaque root colour, and
the phone's own encoded cluster frame is Map on black. Cluster display
declarations name `background` and `interior` images, but no cluster script
consumes them, and the only local wallpaper declaration is the empty main-screen
`Wallpaper` layout. Our session information gives the cluster panel only
`DPManaged` and `showsInstruments`. No public source documents another panel
property; the next experiment needs a live phone and a reference vehicle's
session information, not a guessed key. Light-appearance offline renders show
white text on a white root because the phone video normally covers that root;
this is not a renderer defect.

The small grey circle beside speed in MapsMedia is the declared transmission
indicator (`MapsMediaGearPosition`); unavailable gear is not neutral or zero.

A signature-checked CM5 probe reproduced the direction-dependent 20 km/h bug:
19.6 formats as 20, but the stable number container reserves 164.43 pixels while
the actual digits need 197.07. From above 20 it reserves the wider container.
Our original constructor adapter rounds through the pack's own number formatter
before selecting the stable container, preserving the configured precision and
unavailable values. It does not edit the signed pack. The corrected probe passes
19 → 19.6 → 20 → 20.4 → 21 → 20 and repeated boundary crossings; synthetic
precision/unavailable cases also pass. Physical live appearance is pending.
Diagnostic flags `HEADUNIT_THEME_PROBE_CLUSTER_OPTIONS` and
`HEADUNIT_THEME_PROBE_SPEED_LABEL` are explicit offline `--layout-local` checks.

The pack contains its genuine Apple/partner logo animation on the cluster.
The new opt-in [session greeting](ULTRA_GREETING.md) supplies STARTUP → DRIVE
through timed local owner bindings and waits for the signed animator's real
completion. An optional first-pairing insertion requires verified new-controller
pair-setup/save evidence. Legacy sealed bench profiles still fix Drive; no
pairing was reset or release deployed. The guide owns the exact lifecycle,
offline evidence and bounded live plan. Main-screen Hello remains unqualified.

## Reference-image completeness audit — 1 October

The user's three attachments show four instrument presentations. Comparing
their gauge geometry with our signed metadata and saved renders gives these
**visual matches**, not a verified identifier embedded in the photographs:

| Reference | Matching installed style | Additional presentation shown |
| --- | --- | --- |
| Detailed ticked dials, with the manufacturer lettering on the RPM dial | Cobranded | Driver-assistance view, range, engine temperature, drive mode and status strip |
| Thin cyan rings over a blue landscape | DigiRing | Different palette/background, album artwork, title and artist |
| Four horizontal bars | Modular | Multicolor presentation, range/temperature labels and driver-assistance view |
| Broad beige circular bands | DigiBand | Warm gradient background and a Current Trip page |

These four **base gauge styles are already in version 985**. A different
center page, background or palette is not necessarily a different cluster
layout. The installed catalogue also contains DigiBandTicks and MapsMedia.
The separately discovered Single pack adds three actual layout declarations;
it and both CN identity variants have since shown main CarPlay and gauges on
the physical CM5 screens. Single's main touch and Climate also passed the user
check. Full layout/content/live-switch qualification remains open, and the
explicit development expiry policy is required for these fresh loads. See
[asset discovery](ASSET_DISCOVERY.md).

### What was absent in the 1 October capture

The 1 October physical HDMI-A-2 capture, with confirmed active DigiRing, showed the
Maps route in the center, dark speed text on a grey face, an unavailable gear
placeholder, and fuel/temperature symbols without their readings. The capture
is private. At that time, the active local profile published speed/RPM only.
Transmission was declared but had no sample; fuel, engine/outside temperature,
range and drive mode were not bound. The profile fixed `nightMode=false`.
The following table distinguishes that capture from subsequent component work;
it does not imply that richer fixtures have replaced the live profile.

| Missing/reference detail | Evidence and required work |
| --- | --- |
| Readable speed/gear and consistent dial faces | Corrected geometry exists, but current live contrast is still wrong. Qualify the local palette/appearance against phone video; do not treat a dark-text render as complete. Supply actual transmission samples rather than leaving the declared signal unavailable. |
| Fuel level/range, engine and outside temperature | The pack consumes these semantic inputs. HUV2 carries them, and the extended local profile now binds them. The temperature-marker GPU fault is fixed and day/night fixtures render offline; richer live appearance and phone-background qualification remain. |
| Gear and Sport/other drive-mode label | Both are locally rendered in the signed declarations. Gear needs transmission mode/position. Drive mode needs its identifiers/current selection. The shared model now supplies coherent acquired transmission and observed drive mode; semantic mappings and unknown/stale cases pass portable and signed-layout fixtures. Physical acquisition and real-phone appearance remain open. See [transmission](TRANSMISSION.md) and [drive modes](DRIVE_MODES.md). |
| Odometer, heading and trip statistics | The extended local profile now binds the typed odometer; the normal live profile is unchanged pending qualification. The shared model now supplies qualified heading and dated current-trip statistics, with a native Trip page. Only inspected whole-trip consumption has an established Ultra binding; the exact phone-rendered trip/status composition in the photographs remains unqualified. See [trip and heading](TRIP_AND_HEADING.md). |
| Blue landscape / warm gradient backdrop | Absent from the current capture. Audit phone background/content selection and its interaction with local translucent gauge platters. Do not claim the exact photographed wallpaper is available merely because the pack has a matching gauge style. |
| Album artwork/title/artist in the center | Metadata works in our native cluster, but this does not establish the signed Ultra center composition. Artwork delivery and the phone's center-content selection remain to qualify. |
| Car/lane driver-assistance graphic | The signed DCA_ADAS component explicitly references an input stream named `adasView`. We do not provide that input. This is separate from the phone's DCA_Map video; a placeholder car graphic must not imply real driver assistance. |
| Headlight and other telltales | The independent, default-off Ultra overlay now shares accepted gauge samples. All 24 active/off/flashing and unknown/stale policies pass portable and CM5 GPU fixtures. Physical placement, clearing and live style switching remain open. See [Ultra warning overlay](ULTRA_WARNING_OVERLAY.md). |

### Palettes and content are separate from the base style

The installed signed `r/ThemeData` declares **11 palettes**: Blue, Custom,
Green, Midnight, Multicolor, Orange, Red, Rose, Starlight, Vibrant and Yellow.
Its defaults are Custom for Cobranded, Green for DigiRing, Blue for Modular,
Orange for DigiBandWithTicks, Starlight for DigiBand and Vibrant for MapsMedia.
The script already handles phone palette/appearance messages. Our native
selector currently exposes layout UUIDs, not a qualified palette/background
or center-content picker. Some reference colors therefore differ from our
default selection; that alone is not a missing rendering primitive. Subsequent
offline DigiRing checks render eight palettes and reject three other catalogue
entries in the signed color lookup. Saved local palette selection is tested,
but a phone-coordinated background/content picker remains unqualified. See
[the current completeness matrix](CLUSTER_COMPLETENESS.md).

The catalogue's ordinary center zone declares DCA_ADAS and remote DCA_Map,
with request-content overlays. Our current host fills an otherwise unset,
unambiguous remote map selection. There is **no separate primary layout named
Current Trip** in the installed catalogue. Establish how the phone/vehicle
content channel supplies the photographed trip or media page before claiming
those compositions work with this version.

The blue boxes behind some words in the user's second image look like text
selection annotations; they are not a proposed part of our cluster design.

### Qualification order

1. Qualify the corrected appearance and supplied readings in a real phone
   session. Six regular and nine Single styles now pass the explicitly simulated
   day/night fixtures; that does not establish phone wallpaper or live data.
2. Qualify the implemented temperature-marker composition and extended
   fuel/temperature/range/odometer bindings with live source expiry and units.
3. Audit and expose valid palette/background and center-content choices.
   Verify Maps, media/artwork and trip content independently of gauge geometry.
4. Qualify the implemented active-only Ultra warning overlay on the physical
   phone session. Treat `adasView` as a
   separate future vehicle input, not a prerequisite for useful Maps/gauges.

Historical evidence: read-only `headunit-theme-probe --describe-layouts` on the accepted
reference bundle; bounded inspection of signed PackageData/ThemeData and
vehicle-updater paths; our saved CPU/GPU images; the current physical capture
and active local-profile configuration. This audit did not qualify new phone
content, install another pack, or make a live fix claim.

## Select a style

Add `clusterLayoutFile=/absolute/private/cluster-layout` to the installed-UI
profile. The native page's **Cluster** view shows styles from the connected
pack. Its buttons use the same boundary as the CLI:

```sh
# Inspect actual state and choices.
python3 tools/cluster_layout.py --profile /private/installed-ui.conf

# Request DigiRing without a receiver restart or phone reconnect.
python3 tools/cluster_layout.py --profile /private/installed-ui.conf \
  8BC9ABB1-CB20-595E-92BF-D8F099BF3515
```

The optional Python profile reader captures at most 8 KiB, matching the installed-UI
owner's configuration limit. Missing, nonregular, oversized, invalid UTF-8 or
changing profiles disable the local control. Reads pin the parent and resolved
file, use nonblocking opens and check exact size and identity after capture;
an unwritten FIFO cannot stall manager construction. Source profiles retain
relative input paths and explicit symlink support; `clusterLayoutFile` itself
still requires an absolute path. Portable process fixtures cover FIFO input and
a regular-file-to-FIFO race, with mutation/replacement fixtures for stable reads.
This is startup containment evidence, not physical style-switch qualification.

The CLI/page atomically writes an owner-only UUID file. `LayoutControl` polls
at most every 250 ms on the cluster VM owner and waits for a ready, foregrounded
session. `Runtime::layout_selection` asks the installed controller through its
existing ordered UI-Sync session. The phone drives the layout transition; the
host never stages a local switch ahead of the phone.

The private `.status` snapshot reports `active`, `pending`, `ready`,
`transitioning`, `error` and `choices`. The page highlights confirmed active
state. Rapid requests coalesce behind one in-flight change. Confirmation times
out after eight seconds; retry requires another click. A malformed replacement
cancels an older queued choice. Missing/stale status disables the controls.
Changing Standard/Ultra mode still requires a new phone negotiation.

The optional `persistClusterLayout=true` profile setting now stores confirmed
choices separately by verified asset identity/version. Restoration uses the
same ordered session and catalogue, after readiness returns. The status also
includes `confirmed`, `restoring` and `preferenceError`; failed confirmation or
private-file errors leave the previous preference intact. Offline checks cover
loss of readiness, in-flight transitions, timeout, reconnect, unknown styles,
pack separation and unsafe files. The scoped V84 warm layout reconnect above now passes; broader restoration remains open.
See [session configuration](UI_SESSION.md) for default and persistence limits.

`active` remains the signed controller's observed local layout. It can change
during startup or an aborted transition. The page instead highlights
`confirmedActive`, updated only for a catalogue-valid ready/idle layout in this
VM generation. `idle` records the workspace's explicit idle state; a connected
or disconnected state is not interchangeable with completed layout transition. Current catalogue removal clears an obsolete pending choice and live highlight, keeping native status usable; a newer valid request is admitted only when ready/idle. The historical saved preference survives until a new phone-confirmed choice replaces it. On 6 October, actual owner status/native-parser interoperability and delayed-catalogue recovery pass portable and sanitizer checks, with a failing prior-source regression. This is not a live all-pack switch or reconnect result.

This optional adapter understands the reference `LayoutController` dialect:
it observes the constructor through a read-only delegating proxy, preserves
arguments/results and leaves the returned controller unchanged. It accepts no
arbitrary asset path, script text or unknown layout. It is not a universal SDK.

## Live review

The independent CM5 receiver switched **Modular → DigiRing → DigiBandTicks →
DigiBand → MapsMedia** in one live phone connection. Active-state confirmation
and physical-output captures recorded the transitions; the user confirmed
live DigiRing/DigiBandTicks switches. GTK buttons and manager actions passed
isolated UI tests. Physical touchscreen use of those buttons is still untested.

| Style | Initial observation | Follow-up |
| --- | --- | --- |
| Cobranded | Earlier working gauges/map baseline. | Recheck after arc correction. |
| DigiRing | Gauge face cut away, absent gear and poor contrast. | Isolation fixes the face; new arc/gear fixture needs live check. |
| Modular | Bars visible; speed/gear looked absent. | Offline dark mode shows them; recheck live contrast. |
| DigiBandTicks | Fill occupied the wrong arc; values appeared missing. | Corrected offline fill follows the band, with speed and gear visible. |
| DigiBand | Similar fill/contrast defects in capture. | Same arc correction; recheck live. |
| MapsMedia | Map visible; user confirmed dark speed text. | Recheck appearance and local inputs. Media/artwork completeness remains unqualified. |

## Corrected host behavior

**Group isolation.** Opaque groups with destination-out children now isolate
their cuts, preserving the ancestor gauge face. Explicitly ungrouped blending
retains its flattening behavior. Independent CPU/GPU pixel oracles cover both.

**Arc coordinates.** The script arc ABI uses upward-positive angular coordinates;
our Cairo/Skia points use downward-positive y. Angles now convert once at the
script boundary. The former mapping drew a short upper arc instead of the
intended 270-degree dial sweep. Original synthetic script/pixel tests failed
before the fix and pass in both directions afterward. Native C++ paths keep
their existing convention. This contract is inferred from the reference
layout geometry and checked against its tick positions.

**Circular stroke ends.** Exact angular trimming alone did not fix the visible
flare at the orange band's endpoint. The initial binding incorrectly treated
script cap numbers as Cairo enum values. The reference-pack compatibility
mapping is `ROUND=0`, `SQUARE=1`, `BUTT=2`; convert in both directions at the Lua
boundary. Raw value 2 now produces the flush cut used by the wide progress
tracks, while native C++ `Cap` keeps its existing order and semantics. This
mapping is inferred from reference-pack geometry, not an official Apple enum
specification. Other packs still require qualification.

Wide single-circle strokes now build an annular sector with explicit inner and
outer arcs. Round/square extensions are added only when actually requested;
closed circles have no cap seam. A supersampled, independent mathematical
coverage oracle checks both radial edges near an endpoint. It fails the prior
centreline-stroke implementation and passes this one. Script pixel tests cover
raw/named caps, readback and linear end extensions; transformed GPU comparisons
cover both sweep directions and all native caps. Additional mathematical
coverage checks exercise 6-, 30- and 94-pixel ornaments at four oblique endpoints,
both sweep directions and flush/round caps; they check the inner and outer
edges independently. The graphics, script-runtime and GPU-scene suites pass on
the CM5. All six offline styles render
successfully. The latest enlarged DigiBandTicks image shows the orange and gray
radial edges meeting continuously. Phone-free inspection is not live visual
qualification.
See [draw-time measurements](ULTRA_PERFORMANCE.md).

**Isolation beneath backdrops.** A regular isolated subgroup starts transparent
even inside an ancestor's copied backdrop. Destructive children confined to
that subgroup are supported. Direct destination-out now restores the copied
backdrop's immutable parent; other unsupported destructive modes remain
rejected. GPU/raster comparisons and independent interior-pixel/mixing oracles
cover the distinction. See [the replacement equation](GPU_RENDERING.md#destination-out-markers).

## Appearance root cause — 8 October

The signed `LayoutController` derives its local appearance from the vehicle
signal `uiControl.uiAppearance.nightMode` (`true` → DARK, `false` → LIGHT,
absent → UNDEFINED), overridden only by a phone appearance preference of
Always Light/Always Dark. It also sends that target appearance to the phone.
Selected V11's sealed profile fixed `nightMode=false`, so the local gauges
used LIGHT colors (white root layer, black text). In the user's 7 October
night capture the phone drew its scene dark and the translucent gauge
platter, which turned local speed into black text on dark grey. The same
profile bound no fuel/temperature/range inputs, so those bars were empty.

Offline CM5 GPU renders of MapsMedia with the reference pack confirm both:
the V11 profile yields the light layer; a profile generated with
`create_cluster_bench_profile.py --extended-readings --observed-transmission`
(default dark, `nightMode=true`) yields white text, the fuel gauge with range
and the outside temperature. A fixed dark appearance is configuration, not a
measurement: no light sensor or headlamp provider exists yet, and a future one
can drive `nightMode` through the same binding. The live phone result of the
dark profile is not yet observed.

## Missing readings and unresolved composition

The normal live profile feeds speed/RPM but leaves gear and target-speed
samples unavailable. A separate original bench generator supplies explicit
manual/third-gear and inactive cruise fixtures. It disables secondary speed
to avoid overlapping unit labels in Modular. These are simulated fixtures,
not physical-car acquisition.

Dark phone backgrounds combined with light-mode local text explain some
apparently missing values. Offline dark-mode scenes show readable speed/gear;
that is evidence for coordinated appearance testing, not proof that every
live label is fixed. **All six layouts render on the CM5 GPU with the corrected
arc mapping and basic dark fixture. The final combination has not been tested
with the phone**, because the user ended the session before that test.

The extended profile routes fuel, coolant, outside temperature, range and
odometer through `VehicleSource`. Initially those readings appeared in the CPU
fixture, but
`TemperatureMarker` performs destination-out directly within a backdrop group;
the GPU rejected this combination in DigiRing, Modular, DigiBandTicks and
DigiBand. On 1 October the replacement path was implemented and verified with
independent opaque-parent, translucent-channel, masked-opacity and nested-parent
oracles. All six styles render with the richer fixture in both day and night
mode on the CM5 at 1920 × 720. GPU inspection shows coolant readings, ambient
temperature and correctly cut temperature markers. This fixes the composition
blocker; it does not qualify live phone backgrounds, palettes, freshness on the
physical instruments or the complete photographed feature set.

### Richer profile qualification — 1 October

The final extended fixture supplies 62% fuel, 92 °C coolant, 21 °C ambient,
340 km remaining range and 1,250.2 km odometer through the shared timestamped
source. Unit conversions, expiry and missing-value handling pass on the Mac
and CM5. Inspection of DigiRing and Modular shows the range label after both
the combined-range pair and required port-side controller were configured;
the latter uses the pack's explicit NONE fixture rather than a guessed arrow.
The odometer binding exists; not every authored layout displays it.

| Pack | Layouts | Day/night combinations | Result |
| --- | ---: | ---: | --- |
| Reference `44C9FA31EC / 985` | 6 | 12 | Both panels render at 1920 × 720 |
| `44C9FA31EC-Single / 547` | 9 | 18 | Both panels render at 1920 × 720 with explicit expired-leaf development policy |

The Single set includes the six base styles and SingleDigiRing,
SingleDigiBandTicks and SingleDigiBand. These are deterministic offline scenes,
with no phone video/background or actual presentation feedback. Rendering
success does not qualify contrast against live phone content, every reading
on every layout, live style switching or physical sensor acquisition.
Private evidence is in `captures/cluster-composition-20261001/`, including
the two `readings-*-style-results.json` matrices, scene audits and GPU images.
The normal running receiver/profile was not restarted for these checks.

## Full-screen map restoration — 2 October

The user requested MapsMedia after the CN Single pack's two-gauge view had no
map. CN Single selected the correct MapsMedia UUID and rendered its local speed
and gear, but its map area stayed empty with a real route active. Its selector
reported not ready, and early video counters showed decoding without scene
staging/presentation. Those diagnostics are bounded; silence after the report
limit is not evidence that incoming UI messages stopped.

The signed view starts its phone-video layer hidden and reveals it through
the coordinated session/layout transition. A not-ready snapshot can follow
either an incomplete handshake or a later transition timeout/reset. Establish
the timed exchange before assigning a protocol cause or changing readiness
rules; do not force video opacity or bypass clock qualification.

Returning to the original regular pack `44C9FA31EC / 985`, still in MapsMedia,
restored the full-screen map. The user confirmed it, and native cluster video
presentations advanced. This profile/layout is saved for future connections.
The alternative packs' map activation and reliable live transitions remain
unfinished; no renderer or signed-asset modification was needed for this return.

## Reproduce offline

```sh
python3 tools/create_cluster_bench_profile.py --output /private/bench.plist

HEADUNIT_THEME_PROBE_GAUGES=1 \
HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT=1B0484EC-1933-524D-80F8-2FC1FC7B4B2D \
HEADUNIT_THEME_PROBE_GPU_ONLY=/dev/dri/renderD128 \
build/headunit-theme-probe /private/installed 44C9FA31EC 985 \
  /private/accepted-reference.conf --layout-local /private/bench.plist \
  /private/style-review
```

Locally allocated IIDs are script bindings, not Apple vehicle-protocol IDs.
`--day` selects light appearance. `--extended-readings` supplies the richer
fuel/temperature/range/odometer profile; live qualification remains open. The
probe supplies deterministic simulated readings through `GaugeInput`, the live
source boundary. Optional `HEADUNIT_THEME_PROBE_SCENE_AUDIT=1` records layer
geometry, blend/group isolation, clips/masks and paths.
Captures, signed resources, maps and phone logs stay private under the ignored
CM5 `captures/cluster-style-review-20260930/` directory.

## Resume

1. Test the saved private `next-live-test.conf`: dark appearance, gear fixture,
   corrected arcs, DigiBandTicks. Check values, dial alignment and main touch.
2. Switch all six styles live and inspect comparable speed/RPM states, labels,
   remote map zones and media placeholders.
3. Qualify the richer typed readings with real phone backgrounds and appearance
   changes; direct destructive temperature-marker composition now passes offline.
4. Verify day/night synchronization and the native buttons on the touchscreen.
5. Replace transmission fixtures with typed car-source inputs when acquisition
   is defined.
