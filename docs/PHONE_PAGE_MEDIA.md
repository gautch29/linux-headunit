<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Native phone page, Standard cluster and media

This adds a native GTK 4 settings page, an owner-only local connection manager,
and our own C++ cluster surrounding Standard CarPlay's navigation video. These
are implemented development components. Track metadata, skip control and the
silent music stream have now been checked with a live iPhone. The managed
page's Standard/Ultra switching and Home/Back paths were live verified from the
source checkout. The selected retained V3 release passes preflight/service-path
checks but still needs its own physical workflow qualification. Physical audio
output and pairing/removal also need checks. [HANDOFF](HANDOFF.md).

## Phone page

`tools/phone_page.py` is a native GTK/Wayland application, not a browser. It lists
paired phones from BlueZ with Connect/Reconnect, Disconnect and confirmed Remove.
The phone chooses Standard or Ultra; the page shows only fresh receiver-confirmed
mode for the active phone. A background request thread keeps manager
operations out of GTK's input/render loop. Text from phones is plain text, not
markup. A persistent icon sidebar separates **Phones** (pairings and connection status)
from **Music** (artwork, track information and available controls). Switching
pages does not reconnect a phone or change its mode. Music titles wrap to at most
two lines with end ellipsis, keeping the distinguishing movement of an ordinary
long title visible. The primary playback action is highlighted; supported Play
or Pause follows observed playback state. Unsupported transport and volume
controls are omitted, while local audio errors remain visible. A callback also
checks the current media capability before dispatch. The timeline uses only
observed position/duration: absent timing stays unknown, and progress needs both
fields. There is no local playback clock or extrapolation. Long phone names
yield space to connection controls instead of widening the appliance window.

On 7 October, the actual macOS GTK fixture at 1280 × 720 checks complete two-line
text, bounded 2048-byte unbroken plain text, touchscreen-visible transport, long
phone names, supported playback actions, partial/absent timing and clearing on
unavailable/unknown metadata. Three local GTK suites pass with synthetic private
manager state; inspected Playing, Paused and phone-free screenshots confirm the
layout. Private artifacts/logs are retained under
`~/.cache/headunit-qualification/20261007/oem-pass/native-ui/`. Existing GDK
frame/thaw and GTK progress-size warnings remain, so this is not warning-clean
GTK or physical CM5 appearance evidence. These refinements are source changes,
outside selected V3.

The next-source artwork refinement uses the existing page consumer and manager
publication. The normalized PNG must match `artwork_id` in the exact captured
bytes before GTK decodes it. Reads are bounded to 1 MiB, dimensions to 1024 pixels
per side, and admission requires a stable owned 0600 single-link regular file.
The page decodes that captured buffer rather than reopening the pathname. One
cached texture is reused only while the published digest and file metadata stay
unchanged; ordinary refreshes do not reread or decode unchanged artwork.
Replacement before its matching metadata, corrupt/missing art or unsafe files
clear the cover independently of valid track text. A coherent publication
recovers it. This adds no media owner, clock or worker.

Optional Camera navigation is hidden without a configured camera. If that
configuration disappears while Camera is selected, the page returns to Phones
without sending a camera request. Empty Phones instructions follow the current
native pairing capability, including capability changes with unchanged phone
rows. [Pairing](PHONE_PAIRING.md) owns that instruction behavior.

On 7 October, the combined next-source Main/Music, Pairing, Resident and Artwork
fixtures pass on actual desktop macOS GTK with synthetic private state. The
artwork fixture confirms one open and one texture across 25 unchanged refreshes,
old-metadata/successor-cover rejection, coherent republication, a real Gdk decode
exception for a matching-hash truncated PNG, bounded dimensions/bytes,
mode/link/FIFO/directory rejection, disappearance and retirement during pinned
open. The exact tested page hash is
`d811f07cc926be71143212df198f882e04a49d7ee261e849cfca9fad3e5cfe48`.
Scoped artwork logs, screenshot and source hashes are under
`~/.cache/headunit-qualification/20261007/next-native-artwork/`;
combined fixture evidence is under
`~/.cache/headunit-qualification/20261007/oem-pass/next-native-capabilities/combined/`.
The artwork fixture is also registered in the existing opt-in Linux GTK suite;
that new check has not run on CM5. Existing GDK frame warnings remain. These
are next-source component changes: sealed V10's `eabca30` generation and its phone
run remain unchanged, and this does not establish live artwork or deployment.

An optional [Native Home startup profile](NATIVE_HOME.md) keeps these pages
underneath CarPlay and supplies a Home button on the main touchscreen. Back
coordinates main-window withdrawal and returns without changing the phone
session; manual page launches retain their
normal window-close behavior.
The optional **Camera** page requests a local view independently of phone
selection and distinguishes manual intent from fresh native display status.
Its owner renews expiring requests on a separate worker; the camera surface's
Close control uses a dedicated endpoint. See [camera controls](CAMERA_CONTROLS.md).

The **Cluster** page also lists the connected Ultra pack's primary styles when
`clusterLayoutFile` is configured. An Ultra
style request uses the existing UI session and keeps the phone connected.
The selected state follows phone confirmation. See [cluster layouts](CLUSTER_STYLE_REVIEW.md)
for the adapter, live checks and remaining defects.

When `persistClusterLayout=true` is enabled in that profile, the render owner
saves only phone-confirmed choices, separated by pack/version. The page shows
restoration progress and preference-file errors without treating a click as
success. Offline restoration and error handling are tested; live preference
restoration and per-phone cluster preferences remain to qualify.

The native main interface uses a graphite palette, consistent rounded panels,
large touch targets and original line icons in `tools/phone_icons.py`. No
installed desktop icon theme or Apple fonts are required. GTK handles the
short page crossfade; there is no continuously animated blur. The native
Standard cluster uses a 1920 × 720 canvas with a wide Apple Maps viewport
between speed/RPM instruments. Fine tick marks, needles and a subtle tachometer
redline lend it a restrained car-cluster character. Navigation, media and
vehicle readings share one lower rail. Only active indicators are drawn; off,
unavailable and the dark phase
of flashing indicators leave no placeholder. A separate overall vehicle-data
message still appears when the source is unavailable. The phone-free backup
uses the same native instruments and configurable center pages; Map is offered
only with a Standard CarPlay session. Media, Vehicle and Trip remain available
without a phone. Trip's physical page selection remains unqualified.
The source native cluster draws its instruments over a full-width Map and
uses the same dark neutral surfaces as the main page, with bounded two-line
Media titles/artists; [native cluster pages](NATIVE_CLUSTER_PAGES.md) owns its
design. The six inspected fresh
and unavailable Media/Vehicle/Trip rasters and scoped pixel regressions belong
to [native cluster pages](NATIVE_CLUSTER_PAGES.md); main-page fixtures alone do
not qualify physical continuity.

The September 27 visual revision was checked on macOS at 1280 × 720 with
empty and simulated media, artwork, long names and multiple phones. Offline
GTK actions, page navigation, native cluster expiry and receiver configuration
tests passed at that revision. Later source-checkout Home/Back and mode switching
were confirmed on CM5; the retained V3 build is separately awaiting physical
workflow checks. See [native Home](NATIVE_HOME.md).

`tools/phone_manager.py` owns only receiver processes it started. It communicates
with the page over a private Unix socket, checks peer UID, validates requests,
and serializes mutations. No network listener or general-purpose command runner
is exposed. Wi-Fi setup still uses the existing privileged lab launcher;
the page itself runs as the normal desktop user.

The preferred phone lives in `phones.json`, written atomically with owner-only
permissions, separately from Bluetooth bonds and protocol keys. Version-1
Standard/Ultra hints remain readable for rollback to a retained old release;
new source never consults them to choose a session. Connect validates an offer
of both configured capabilities, then replaces the owned receiver. The phone's
initial feature offer chooses Standard unless UI-Sync and HEVC agree. Agreement
alone leaves the mode unknown until the exact registered UI service SETUP
succeeds; only then is Ultra selected. No Standard main or cluster presenter
starts while the mode is unknown or Ultra.

Disconnect preserves bonds, keys and the preferred phone. Its button captures
the phone and receiver generation; replaced sessions reject it. Add phone is
available only after the receiver stops. Remove remains a confirmed operation.
There is no mode button, saved mode mutation or accepted legacy mode RPC.

The read-only label uses a fresh version-2 receiver publication bound to the
current owned launch nonce and connection generation. Missing, expired,
wrong-launch or dead-child state clears the label; a retained version-1 report
has unknown mode. Actual local GTK/private-socket and real isolated child
fixtures pass, including opposite legacy hints and late modal callbacks after
parent retirement. These tests use simulated phone/display boundaries, with
existing GTK/GDK diagnostics; they do not qualify selected V3, V5 or CM5 touch.

Managed sessions use separate `pair_store/phone-<address-without-colons>` directories.
Remove deletes that phone through BlueZ and clears only its managed protocol-key
directory and preference. Old shared diagnostic pairing records are not imported,
used or bulk-deleted. The first managed connection may need fresh phone-side
pairing. A separately enabled native **Add phone** flow supplies a temporary
incoming Bluetooth window and explicit code confirmation; its actual CM5/iPhone
qualification remains open. See [Add a phone](PHONE_PAIRING.md). An optional
default-off startup profile permits one
attempt to launch the saved selected phone's receiver; see
[Phone manager startup](PHONE_MANAGER_BOOT.md).

The manager's `active` field means a receiver is selected/running, **not** that
CarPlay has presented video. The page deliberately distinguishes this from
Bluetooth connection state. It reports receiver exit instead of claiming success.
Manual launch follows the saved receiver's lifetime. The optional supervised
manager profile requires `seconds: 0`, replaces the existing receiver service
owner and keeps the external backup service separate. Its actual Linux boot
and phone reconnection remain unqualified. A failing receiver is not restarted
indefinitely; the page reports an explicit retry.

## Standard versus Ultra

- **Ultra:** the saved signed asset, native Ultra video and configured asset
  canvases are enabled. The current reference geometry remains unchanged.
- **Standard:** Ultra advertisement and its script worker are disabled. Main
  CarPlay uses `config/displays-cm5-standard.conf` (1280 × 720 main,
  1920 × 720 cluster). The secondary
  navigation stream enters a Wayland video subsurface between our speed/RPM
  gauges. Navigation, media, vehicle readings and active indicators surround
  the map. The wide cluster's Media/Map/Vehicle pages are user verified; Trip
  remains component-tested. On 4 October the managed page's Standard selection
  shows main CarPlay and native gauges on the correct physical outputs, with
  working touch. Standard Home/Back after ten seconds and returning to Ultra
  also work, confirmed by the user.

The secondary output is selected by its configured Wayland socket and connector.
Main and cluster must target distinct outputs; they can share one compositor
socket. On the current two-screen CM5, HDMI-A-1 is the touchscreen and HDMI-A-2
is the cluster. That physical routing and the earlier native Map/Media/Vehicle
page switches are user verified. The managed Home route has now passed its own
Standard phone check; the manager and page retain their original process IDs.
The native cluster owns the background; the decoder/Wayland sink owns the map.
Phone video is not converted into our CPU gauge image. Local gauges render on a
separate bounded worker at 20 Hz, with one pending image and three compositor
buffers, so Cairo rendering and artwork decoding do not run on the receiver's
control/input thread.

The schema-2 resident Home configuration supplies paired
`native_page_control_file` and `native_page_lease_file` receiver settings.
Managed Standard then uses the owned `wayland-gpu` main presenter; the legacy
route without Home remains `wayland-touch`. The owned presenter drains decoded
output while Home is foreground, retires its window before releasing ownership,
and requires a fresh source and actual presentation receipt before accepting
touch on return. See [the Home contract](NATIVE_HOME.md) and
[Standard video ownership](STANDARD_VIDEO_OUTPUT.md).

`vehicle_socket` or `simulate_vehicle` selects the same HUV2 vehicle source used
by the backup cluster. They are mutually exclusive. Without either, measurements
are unavailable. This native Standard cluster lives with its receiver session.
A separate appliance cluster service provides phone-free takeover; actual phone
disconnect is user verified. Fault-induced recovery remains separate
qualification work.

## Media model and transports

The shared now-playing snapshot includes title, artist, album, genre, player,
track identifier/number/count, duration, elapsed time, playback state, optional
volume, artwork and supported controls. Complete updates clear old fields and
old cover references. Disconnect/source changes clear the whole snapshot. Native
cluster copies expire three seconds after manager publication; that lease does
not date the original phone metadata (see the native data trace below). The native renderer now retains an explicitly available source even when title/artist are absent, matching Music's “Unknown title” behavior while preserving status/progress/album. Its snapshot must carry an actual unsigned acquisition-date field; a missing date cannot become a zero-time observation during startup. Cairo pixel checks cover Media and Map rails, connected/phone-free views and unavailable/future/expired/missing dates. Independent gauge paths start clean, so media text cannot join instrument strokes; instrument pixels remain identical across media availability changes. Synthetic Media/Vehicle/Trip renders were inspected on 6 October; physical display and complete live metadata remain open.

The implemented Bluetooth adapter reads only the selected phone's addressed
`org.bluez.MediaPlayer1`. Play, pause, previous and next use its methods. Volume
up/down adjusts the matching `MediaTransport1.Volume` when exposed, with bounds
and a small step. Missing transports fail explicitly. These are phone/transport
controls, not head-unit mixer or amplifier controls. Separate native head-unit
volume/mute controls now use a fresh audio route and confirm applied mixer gain;
they are available only after an actual audio worker starts. Desired settings
persist independently, and pending requests remain visibly separate from
applied state. See [audio controls](AUDIO_CONTROLS.md). Commands are tied
to the active phone and manager generation so stale UI commands cannot control
a replacement session. A successful method call means dispatched, not observed
playback completion.

Source: [BlueZ Media API](https://bluez.readthedocs.io/en/latest/media-api/).
Selected-phone title/artist and one skip action are live verified during
wireless CarPlay. Complete metadata/control availability, artwork and phone
volume remain unqualified; absent capabilities are not fabricated.

The receiver subscribes to iAP2 now-playing changes and validates bounded
partial updates before publishing a complete snapshot. The separate iAP file
transfer session reassembles at most four bounded transfers and attaches a
completed PNG/JPEG image only to its matching track transfer ID. An atomic, private
sidecar file is refreshed while that receiver session is alive. The manager
accepts it only from its own account, for its active phone, while its child
receiver is running and the file is fresh. It falls back to BlueZ on disconnect.
Metadata and artwork opens are nonblocking before checking their regular-file
type, so a leftover FIFO cannot stall the manager's polling or phone controls.
Portable process fixtures exercise both FIFO paths with actual files; invalid
metadata falls back to BlueZ, while invalid artwork preserves valid track data.
Scalar or excessively nested metadata is rejected through that same fallback;
it cannot terminate the manager's polling loop. The standalone media bridge
also checks bounded nonblocking regular files and contains malformed JSON.
Native cluster page reads reject linked, oversized and nonregular state before
using the ordinary default page. FIFO and malformed-schema process/component
fixtures cover these boundaries without a phone or display owner.
Artwork bytes are written to a separate private file and checked against a
SHA-256 digest before GdkPixbuf normalizes them to a bounded PNG for the page
and cluster. Track changes clear the cover. When BlueZ independently exposes controls for that same phone, those controls
remain usable alongside iAP metadata; absent BlueZ capabilities are not
invented. The iAP subscription and sidecar are enabled only when the managed
receiver has a media-state path. A live iPhone delivered track details and
position changes on the CM5; a BlueZ Next command changed the phone's track
and the iAP snapshot. The managed page itself was not running in that test.

`media_update` remains a local adapter ingress for other sources. It requires
the active phone and current generation, accepts a bounded complete media
snapshot, and expires back to the actual Bluetooth source after three seconds.
Optional base64 PNG artwork is limited to 1 MiB and 1024 × 1024,
decoded/normalized before storage and rendered from the private local directory.
Track changes remove the old artwork reference. Arbitrary URLs/files are not
fetched. The ingress currently advertises no actionable controls because no iAP
command adapter is attached. The GTK page and C++ cluster both render supplied
artwork. Automatic iPhone artwork transfer has an implemented, offline-tested
iAP file-transfer path, but **actual automatic iPhone artwork delivery is not
yet live-qualified**.

The file-transfer path is offline tested but the iPhone did not send artwork
in the live test. The 4 October source revision makes item/playback group
ordering independent, retires partial files on valid track/selected-cover
changes or oversized fragments, and clears mismatched standalone bridge covers
until matching SHA-256 bytes arrive. Real private-file/GdkPixbuf publication
regressions and the two C++ protocol suites pass. Malformed metadata preserves
the current assembly; unrelated transfer IDs remain independent. A delayed
complete file using a reused eight-bit ID cannot be distinguished without
stronger observed wire evidence. These source fixes are outside V3 and do not
qualify automatic phone artwork delivery. A standalone iAP command channel for play/pause/skip,
microphone, Siri and calls remains open.

### Artwork ownership across classic-channel retirement

Classic channel EOF retires that socket's cipher and partial encrypted package,
while the attached logical iAP link and its independent metadata remain alive.
Reopening requires a fresh channel seed. A valid successor item update retires
old partial artwork even when its eight-bit transfer ID is reused; malformed
metadata preserves the selected item and its valid assembly. Retained snapshots
keep their immutable artwork backing without selecting it for the successor.

The combined `airplay-session-lifecycle` regression exercises these boundaries
through encrypted loopback sockets, including old-channel ciphertext replay,
paused playback independent of item position, and healthy RECORD/negotiated
video. It uses tiny synthetic image signatures, without decoding an image or
qualifying phone artwork delivery. The delayed complete-file ambiguity above
remains unchanged. No acquisition date, playback confirmation, phone capability
or car `NowPlaying` publication is introduced.

### Media observations — 14 October source-only

The production manager had a reproducible admission gap: after its owned child
exited normally, retained active-phone/generation/capabilities could still admit
a BlueZ media command before `tick()` retired the session. The new regression
fails on `37fb44b`. Media dispatch now rechecks the live child after synchronous
BlueZ phone lookup, requires the exact integer manager generation and complete
request fields, and rejects stale, future or undated manager media publications
for BlueZ controls. Status also rechecks media age after phone-list lookup.
The normal polling retirement and signal-death whole-service guard remain the
owners of cleanup. This is a source admission repair, not an explanation for
V84's independent paused iAP report and advancing accepted PCM.

`status.media_observations` version 1 is a nonvisual projection inside the
existing `media_state.py`. It consumes `MediaState.last` and the **same single**
`audio_snapshot()` copy returned as `status.audio`. It opens no file/socket,
acquires no phone/audio data and creates no thread, logger or widget. Existing
`status.media.status` keeps the phone-reported transport state. Unavailable or
expired media has no BlueZ capabilities; a fresh ready Ultra scene independently
offers its existing HID keys. Play and Pause on that route both queue the
phone's `play_pause` toggle, not distinct state-setting commands.

For native UI review, the additive keys are:

| Key beneath `media_observations` | Meaning |
| --- | --- |
| `version`, `generation`, `live` | Schema 1, current manager generation and owned-child liveness. Liveness is not CarPlay readiness or playback. |
| `transportReport.available`, `.fresh`, `.source`, `.reportedState` | Current manager publication availability/age and unchanged metadata source/state. A stale retained report may still have `reportedState=paused` while `available=false`. |
| `transportReport.publishedMonotonicMs` | Original `MediaState` publication date in Python's `time.monotonic()` domain, or null. Maximum admitted age is three seconds, with future dates rejected. |
| `transportReport.reportAcquiredMonotonicNs` | Always null: the existing iAP/BlueZ model does not supply the original transport-report acquisition date. A heartbeat/publication cannot establish that age. |
| `pcm.state` | `unavailable`, `stale`, `baseline`, `progressing`, `no-advance`, `unchanged-sample` or `invalid-observation`. These describe observations only. |
| `pcm.route`, `.sampledMonotonicNs`, `.acceptedBlocks` | Exact admitted audio route and original output-owner date/count strings, or null. Dates use receiver `CLOCK_MONOTONIC`, independently of media publication. |
| `pcm.previousSampledMonotonicNs`, `.acceptedBlocksDelta` | Original preceding sample date and decimal-string delta only for two fresh ordered observations on the same route and manager generation; otherwise null. |
| `action` | Null or one latest admitted media-dispatch observation, detailed below. |
| `playbackConfirmed` | Always false; neither these reports nor dispatches provide a playback receipt. |

PCM comparison retains one fixed-size frontier. Both samples must be within the
existing one-second output-age limit at observation; no interpolation, local
play clock or rate is generated. Identical sample dates are `unchanged-sample`
with a null delta, so another poll cannot count the same progress again. Date
or counter regression and changed equal-date state are `invalid-observation`
and cannot replace the frontier. A new route/generation, absent legacy output,
unavailable route or child retirement starts a new baseline. A gap older than
one second also starts a baseline. Comparison history belongs to the manager,
so independent status clients can observe `unchanged-sample` after another
client already consumed that sample. PCM acceptance may include mixed voice,
muted or resource-denied blocks; progress is neither music-state authority,
gapless continuity nor an audible/device playback receipt. Original detailed
output/error counters remain in `status.audio`.

The existing action is unchanged:

```json
{"action":"media","phone":"CURRENT_PAIRED_PHONE","generation":0,"command":"play"}
```

Use the actual status generation and selected phone. `command` remains one of
`play`, `pause`, `previous`, `next`, `volume_up`, `volume_down`, admitted only by
the available existing transport. Successful replies add `confirmed:false`,
`completion:"unknown"` and `transport:"bluetooth"` or `"ultra-hid"`. Bluetooth
success means its method returned; HID success means a request was queued.
Neither implies the phone performed it. No matching metadata, PCM count or
rendered Music pixels automatically completes an action.

`action` contains `id` (manager-local serial, not a phone receipt), `generation`,
`command`, `transport`, `requestedMonotonicNs`, `resultMonotonicNs`, `state`,
`completion`, `confirmed` and `error`. Action dates are actual manager
`CLOCK_MONOTONIC` samples, not phone or PCM dates. State is `pending` during the
existing synchronous dispatch, then `dispatched` (BlueZ), `queued` (HID) or
`error`. Pending observations do not add an executor or queue. Result date is
null until a result; error is null or the bounded category `dispatch-failed`.
Completion remains `unknown` and confirmed remains false even after successful
dispatch. Admission failures do not replace the last admitted action. Stop,
generation replacement or observed child loss clears the action. A retired or
superseded token cannot attach a late result to a successor.

**New UI pieces, widget IDs, pages and actions: none.** native UI can consume these
fields in the established Home2 Music components; native GTK files/styles are
unchanged. Phone metadata is not mirrored into car `NowPlaying`. Rendered phone
content remains an independently observed authority, with no new pixel reader.
Original phone acquisition remains unavailable. The source-only
[host acceptance extension](#iap-host-acceptance-observations--14-october-source-only)
adds existing-owner scope and actual host admission dates; it does not change
`reportAcquiredMonotonicNs` or establish phone authorship. No protocol,
PCM queue/reserve/deadline, signed asset or source-clock change is introduced.

Component qualification and exact source-only retirement receipts are private
under `20261014/codex-media-consistency/`. Fixtures use the actual Manager,
MediaState and AudioControl/private state parser with substituted child/BlueZ
boundaries, original synthetic clock domains and explicit queued-HID IPC stub.
They cover normal child exit before tick/during lookup, stale/future/missing
publication, paused iAP plus advancing dated PCM, repeated/stale samples,
counter/date regression, route/generation loss, dispatch error and late results
after retirement. They do not open radio, phone, audio device, display or GTK.
The initial direct sealed-manager invocation omitted its required compiled
display checker and failed; its log remains excluded from passing evidence.
Final macOS project configure, display-checker target and focused registered
CTests pass **5/5** (92 Python cases, zero skips): media observations 16, phone
management 36, audio controls 11, manager recovery 15 and sealed manager 14.
Pinned format 19.1.7 and whitespace checks pass. Both own CMake trees were
removed after source/log/Testing/metadata hash verification; no container was
created. Linux, GTK, full receiver and combined release checks were not rerun.

Exact bench follow-up belongs only to phone-session after manager whole-diff
review, the coherent combined required matrix and a new immutable generation:

1. Record source/release identity and poll the existing manager status. With
   actual music present, retain `media`, `audio` and `media_observations`
   separately. A paused report with advancing accepted PCM must remain paused;
   confirm original route/sample dates and repeated-poll null deltas.
2. Send one supported current-generation media command. Retain its reply and
   latest action; distinguish queued HID toggle from returned BlueZ method.
   Record any actual phone report/pixels independently, without marking the
   command confirmed. Repeat only as a separately dated operation.
3. Disconnect through the existing manager owner, verify unavailable media/PCM,
   null action and rejection of the old generation. Reconnect and require a new
   baseline before comparing successor PCM. Check Home/Back, Music, Climate,
   touch, cluster, both streams, UI-Sync/audio/control and fallback remain intact.

No worker deployment or fresh bench evidence accompanies this source. Audible
sound, actual playing, phone command application and camera/audio continuity
remain open; retain the current silent-sink/physical-equipment limits.

### Native Home data trace and parked input — 14 October 2026

Native Home2 and Music read the same manager `media` object. Neither reads the
phone's signed Music scene. The native cluster independently reads `media.json`,
so status filtering alone cannot retire its metadata.

| Existing boundary | Data and identity | Date meaning and limit |
| --- | --- | --- |
| `Iap2NowPlaying` | Bounded partial item/playback updates form a session-owned complete snapshot; track/title/selected-transfer changes clear dependent fields/artwork. | Legacy snapshots have no receive date. The host acceptance extension below copies actual admission dates and publisher-supplied existing engine scope; phone acquisition remains unknown. |
| `control_probe` iAP publisher | First available connection snapshot; optional SHA-256 of the matching transferred artwork bytes in `.iap.art`. No title is extracted from video. | The `.iap` file is republished at least each second while the receiver runs. Its mtime is a publisher lease, not a phone metadata acquisition. |
| Manager iAP route | Private bounded regular `.iap` file, active phone and owned launcher; actual BlueZ controls/volume may accompany iAP text. Raw artwork hash is checked before normalization. | File mtime must be within three seconds. Optional copied host dates require the current launch nonce; existing engine and publication-connection IDs distinguish scope. Neither lease nor identity establishes phone acquisition/freshness/playback. |
| BlueZ fallback | Selected `Device1`'s `MediaControl1.Player`/`MediaPlayer1` properties; matching `MediaTransport1` volume where exposed. | Synchronous property lookup exposes no original phone acquisition date. No local progress extrapolation. |
| `MediaState` / manager status | Complete validated snapshot, normalized private artwork filename and normalized-byte SHA-256 `artwork_id`; optional fields stay absent. | `updated_monotonic_ms` dates manager publication. `media_observations.transportReport.reportAcquiredMonotonicNs` stays null. Publication and file mtime cannot establish original metadata freshness. |

Necessary sanitized retained V85 status inspection finds an available iAP source
with stopped state, empty title/artist, zero duration/position and no artwork at
the held Home observation. A separately dated Music status has title/artist/album,
paused state and known duration. The later retained Climate statuses inspected
again have empty stopped iAP data. These observations do not establish a
same-moment mismatch with signed content. No phone text, identity, artwork,
private log or signed bundle is copied into Git.

**Parked:** changing title/artwork selection or claiming original freshness needs
a same-moment native status and signed-content observation, bound to the existing
receiver/iAP owner, plus the original accepted partial metadata and matching
artwork-transfer identity/date. Those inputs are absent from the retained manager
statuses. Do not fill them from PCM activity, a periodic publication, mtime or the
signed scene. Do not mirror phone metadata into the CAR NowPlaying service,
subscribe to guessed fields, advertise another capability, or replace useful
phone content with invented labels/playback/artwork. The existing available but
untitled source remains untitled; zero timing is retained only where reported.

A separate production-owner regression demonstrates a polling retirement bug:
a real child exits normally or by SIGKILL during the substituted synchronous
BlueZ lookup, and the earlier manager republishes available iAP or BlueZ data
into `media.json`. A late reentrant lookup also overwrites already admitted
successor content. Status's liveness filter cannot protect the independent
native file consumer. Before repair, the unchanged owner passes the original
16 cases and fails all seven new scenarios across three new tests.

The source repair captures the phone, child object and manager generation for
that poll. Every publication rechecks that identity and current child liveness.
A dead current owner publishes unavailable data without a cover reference;
a replaced owner drops its late result without writing over the successor.
Normal recovery and whole-service signal quarantine remain with the existing
lifecycle owner. No metadata fields, source dates, artwork matching, capabilities,
progress, command transport or recovery policy change. This is a polling
retirement repair, not a repair or qualification of the parked phone contract.

Focused qualification passes **81/81** direct Python cases on macOS and
**81/81** on Debian trixie ARM64 as an unprivileged user, with required GdkPixbuf
present and zero skips: media observations 19, phone management 36, manager
recovery 15 and audio controls 11. These include actual local child normal exit
and SIGKILL, real private files, explicit BlueZ substitutes, exception/late
successor paths and existing malformed/FIFO/artwork/control checks. Pinned
clang-format 19.1.7 and whitespace checks pass. The existing test PyGI version
warning is retained; no warning, assertion, timeout or policy was relaxed.
The initial formatter invocation against a Git-free source copy failed before
any test; its log/source/receipt remain retained. The corrected formatter uses
the Git worktree with source bytes verified against the frozen test inputs.

The Linux runner's real INT/TERM/HUP proofs return 130/143/129, retain original
container logs/state and verify actual removal; the final Linux test container
also retires. Direct Python lanes create no CMake build tree or `Testing`
directory. Original failed and passing logs, all 876 tracked source hashes and
retirement receipts are retained outside Git under
`20261014/codex-native-media-data/`. No C++ parser, compiled/sanitized receiver,
GTK/GPU, full matrix, phone, audible or physical behavior is newly qualified.

For native UI, the data contract remains `status.media`: `available`, `source`,
`title`, `artist`, `album`, `genre`, `player`, `track_id`, `track_number`,
`track_count`, `status`, optional `duration_ms`/`position_ms`/`volume`, `controls`,
optional `artwork`/`artwork_id`, and publication-only `updated_monotonic_ms`.
Progress requires both timing fields and positive duration. The existing action
is `media` with current `phone`, `generation` and supported `command` (`play`,
`pause`, `next`, `previous`, `volume_up`, `volume_down`); availability still
selects actual BlueZ versus ready Ultra HID transport. The accepted media-owner
observation interface above remains additive. New UI keys, action/control/style
IDs, widgets, pages and other visual pieces from this investigation: **none**.
No GTK source or layout/style changes are needed.

Sole phone-session follow-up, after whole-diff review and a new coherent required
matrix on integrated source (never inject this into frozen V86):

1. Publish/select a new validated immutable generation through the existing
   owner, with current input/helper/unit/capacity gates. Retain exact source
   identity. In an isolated private fixture, repeat the real-child exit and
   successor tests against those tools; never inject fake metadata into the live
   manager or start another radio/display owner.
2. With real music, separately retain native `media`, original publication date,
   `media_observations`, audio state, and same-phase native/signed pixel evidence.
   Verify current text/progress/artwork only where supplied; do not call an
   absent original iAP date/generation known. Preserve Home/Back/Climate, main
   touch, cluster/fallback, both streams, UI-Sync, audio and control.
3. Disconnect through the existing manager; verify unavailable native media,
   cleared cover reference and rejected old-generation commands. Reconnect and
   require current-owner data before showing it again. Keep command dispatch,
   phone reports and audible playback separate. Resolving the title/artwork park
   still requires the original owner-bound iAP inputs listed above.

### iAP host acceptance observations — 14 October source-only

The existing `Iap2NowPlaying` owner now samples **host `CLOCK_MONOTONIC`** only
when a fully validated partial message commits, and separately when a bounded
PNG/JPEG is accepted for the selected artwork transfer ID. This is receiver
acceptance, not phone authorship, phone acquisition, an artwork decode/display
receipt, or played state. Protocol, subscriptions, values, controls, existing
track/title clearing and transfer retirement are unchanged. No TTL is added to
track/session-bound metadata. The historical title-change clearing heuristic
remains; it does not supply an identity.

Each supplied exposed field receives that actual commit date, including an
explicit empty string or reported zero. Missing fields retain their original
dates. Unknown fields and the unconsumed playback field 16 do not date any
exposed field. A malformed message changes neither values nor dates. A reliable
link replay has no new admission; a genuinely new accepted message may date the
fields it actually supplies even if their values are equal. Snapshot reads,
manager polls, encoder calls and the existing periodic publication cannot renew
those dates. Track changes clear dates exactly where existing values clear;
selected transfer changes clear the previous artwork date. A rejected image or
retired assembly never dates a cover. Explicit parser clear leaves all dates null. The engine-owned publication copy
withdraws dates on actual terminal engine failure/retirement, even if another
owner retains the engine. Historical parser dates and legacy metadata values
remain unchanged; the encoder omits an entirely unobserved extension.

The existing `.iap` sidecar optionally adds `iap_observation` version 1. Its
bounded encoder lives with the NowPlaying component and is called at the same
publication cadence. If the extension would exceed the existing 8192-byte reader
limit, it is omitted while preserving the previous metadata bytes. No queue,
lease, deadline, logger, thread, service, disk path or acquisition owner is added.

| Key | Actual meaning |
| --- | --- |
| `clock` | Literal `CLOCK_MONOTONIC`; distinct from Python publication time and filesystem lease dates. |
| `launchNonce` | Existing managed receiver launch nonce, checked by Manager against its current launch. |
| `engineGeneration` | Decimal string of the existing iAP engine diagnostic generation. Missing engine scope leaves the extension absent. |
| `connectionGeneration` | Decimal string of the actual selected publisher connection number. Publication context, not proof that iAP arrived on that AirPlay socket. |
| `messageAcceptedMonotonicNs` | Decimal string of the latest valid partial message admission, or null if sampling unavailable. Artwork admission does not change it. |
| `artworkTransferId` | Current selected eight-bit transfer ID, or null. It is not a globally unique transfer identity. |
| `fields` | Fixed map of `track_id`, `title`, `artist`, `album`, `genre`, `duration_ms`, `position_ms`, `status`, `player`, `artwork_transfer_id`, `artwork`; each value is its actual host admission date as a decimal string, or null. |

Manager copies admitted data into `status.media.iap_observation` and the existing
private `media.json`. Missing legacy observations remain absent. Malformed or
previous-launch observations are stripped without hiding otherwise useful
metadata or actual controls. Existing C6 phone/child/generation checks prevent
late polls from publishing into a dead or successor owner. `media_update` cannot
claim this native iAP scope. BlueZ lookup still supplies no original field dates.
`updated_monotonic_ms` stays manager publication; `.iap` mtime stays receiver
publication lease. Neither fills missing observation dates.

**For native UI:** the additive nonvisual view-model is
`status.media_observations.transportReport.iapHostObservation`. It exposes
`available`, `clock`, nullable `scope` (`launchNonce`, `engineGeneration`,
`connectionGeneration`, actual `trackId` or null, `artworkTransferId`),
`messageAcceptedMonotonicNs`, `messageObservedAgeNs`, and the fixed `fields` map
with `acceptedMonotonicNs`/`observedAgeNs` pairs. Ages are decimal-string host
nanoseconds, computed only for nonfuture dates in the same clock domain. Unknown,
legacy, future-age, expired publication and retired-owner dates are unaged/null;
original future dates are retained as supplied, without clock correction.
`phoneAcquiredMonotonicNs` remains null; `freshnessGuaranteed` and
`playbackConfirmed` remain false. An old valid metadata observation has an old
age, not a newly invented stale/expiry policy. Existing transport-report
`reportAcquiredMonotonicNs` remains null. **New actions, control/widget/style IDs,
widgets, pages and visual pieces: none.** Reuse established Home2 Music components
if adopting these data; GTK remains native UI.

Qualification is component-only under private
`20261014/codex-iap-media-provenance/`. Actual parser/engine fixtures cover partial
updates, rejection, replay, item/transfer retirement, original dates, clear and
successor state. A new registered fixture runs the actual C++ producer/encoder
through real private files and Manager/MediaState/standalone bridge with a real
owned child and explicit BlueZ substitute. It covers lease/publication repetition,
legacy/invalid/foreign identity, actual child exit and null/future/old ages.
Baseline production Mac checks pass5/5 before source changes. Final macOS
Debug focused CTests pass7/7 (87 Python cases, zero skips, plus the two C++
parser/engine suites). Debian trixie ARM64 Clang19 ASan+UBSan repeats7/7
(87 Python cases, zero skips) and compiles the actual `headunit-control-probe`
caller with GPU off. Default warnings, leak checks, existing external LSan
suppressions, queues and deadlines are unchanged. Pinned format19.1.7 and
whitespace checks pass. The first expanded Mac attempt has three fixture
failures, retained separately; size/explicit generation/whole-service signal
quarantine expectations were corrected without changing production behavior.
A final engine-copy observation withdrawal justified the final Mac/Linux
reruns. Actual runner INT/TERM/HUP container cleanup passes3/3. Every own build
and container is retired after verified source/log/Testing/JSON retention.
No full Skia, complete required matrix, GTK/pixel, phone, audible, physical
artwork delivery, camera continuity or fresh60 acceptance is claimed.

**Park:** original phone authorship/acquisition, engine creation date (not copied by this interface),
same-moment native versus signed-content joins, per-field track attribution when
the actual track ID was absent at admission (current `scope.trackId` is snapshot
context, never a historical attribution), delayed complete files using a
reused eight-bit ID, automatic phone artwork delivery, audible playback and phone
command application still lack the necessary contract/evidence. No historical
sidecar/capture is backfilled, and metadata is never mirrored to car NowPlaying.

Exact phone-session follow-up requires whole-diff manager review, a fresh coherent
complete required matrix, current helper/input/unit/capacity gates and a **new**
immutable generation. Keep selected V87 untouched. Run the registered private
fixture against exact packaged source before live use, without live fake-data
injection. With actual music, retain original host field/message/artwork dates
and launch/engine/publication-connection/track/transfer scope beside separate
publication times; a position-only update must not renew title/artist/artwork,
and repeated polls must keep every accepted date. Compare native and signed
content only at separately dated same-phase observations. Normal Disconnect
must withdraw observations and cover references; reconnect must use successor
scope and reject old-generation actions. Preserve Home/Back, main/touch,
cluster/fallback, Climate, both streams, UI-Sync/audio/control. Artwork remains
unknown if the phone does not deliver it; no second owner or guessed request.

### Audio foundation, 29 September 2026

`AudioMixer` is independent of CarPlay and the cluster renderer. It accepts
aligned 10 ms blocks of decoded 48 kHz stereo PCM on media and guidance buses.
Guidance eases the media gain toward 25% over about 5 ms and releases it over
about 100 ms; master volume, mute and signed-16-bit saturation are explicit.
A new phone session resets the mixer state. Missing buses contribute silence.
The adapter supplying blocks must align timestamps; the mixer does not guess
clock synchronization or decode a network codec.

`AudioOutput` sends mixed blocks through a nonblocking eight-block GStreamer
`appsrc` queue to an explicitly selected `fakesink`, `alsasink` or `pulsesink`.
The 1 October revision adds a literal `audio_device` configuration field and
continuous ten-millisecond PCM timestamps, plus a bounded playback-time horizon
and one-block retry instead of discarding PCM when output is temporarily full.
The silent sink obeys the same real-time schedule. See
[audio output](AUDIO_OUTPUT.md) for the interface, timing checks and outstanding
real-speaker qualification.
The CM5 build and a synthetic music-plus-guidance test pass with `fakesink`.
At the 29 September check, the CM5 had GStreamer's ALSA plugin but its HDMI
display was unplugged and default ALSA output could not open. Two displays are
now present; no audible speaker path has yet been qualified. Do not treat that
historical unplugged state as current telemetry.

With `audio_sink` set to `fakesink`, `alsasink` or `pulsesink`, the receiver
advertises a basic CarPlay audio profile plus AAC-LC stereo at 48 kHz for
buffered media stream 102. The live iPhone did not open stream 102 when only
that stream's format was advertised; it did when the basic profile accompanied
it. The current receiver handles stream 102. Low-latency categories in the
basic profile are still rejected if the phone requests them. It binds separate
UDP data/control ports, authenticates ChaCha20-Poly1305 packets, converts
their AAC payloads to RTP MPEG4-GENERIC, decodes to 10 ms PCM blocks and sends
them to the mixer/output worker. The optional stream is refused if its decoder
or selected sink cannot start. Its queue is bounded and reports rejected and
dropped blocks. Encrypted AAC fixture tests, parser tests and SETUP tests pass
on the CM5. On 29 September a live iPhone sent 6,537 authenticated packets,
none were rejected, and 13,943 ten-millisecond blocks reached `fakesink`;
35 blocks were dropped by the bounded output queue and no decoder fault was
reported. Physical playback, longer buffered sessions, retransmission/RTCP
and volume semantics still need testing.
Navigation prompt and alternate audio are not yet received. Leave `audio_sink`
unset (the default) until the chosen physical audio device is ready.

## Run on the CM5

Dependencies beyond the receiver: Python GI, GTK 4 and GdkPixbuf introspection
(`python3-gi`, `gir1.2-gtk-4.0`, `gir1.2-gdkpixbuf-2.0`, plus the SVG loader
`librsvg2-common` for the original line icons). Stop the old lab launcher
before asking the new manager to connect; it will not kill an unrelated receiver.
Keep private config and the signed asset outside Git.

```sh
sudo python3 tools/phone_manager.py \
  --config /home/headunit/.config/ultra-headunit/lab/receiver.json \
  --directory /home/headunit/.config/ultra-headunit/manager \
  --standard-display /home/headunit/ultra-headunit/config/displays-cm5-standard.conf \
  --uid 1000
```

From the desktop user's Wayland environment:

```sh
GDK_BACKEND=wayland GSK_RENDERER=opengl \
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=headunit-0 \
dbus-run-session -- python3 tools/phone_page.py \
  --socket /home/headunit/.config/ultra-headunit/manager/manager.sock
```

With a manual page launch, Back to CarPlay closes the settings window; the
receiver continues. With the optional schema-2 resident Home profile, Back
lowers the page before releasing main-surface ownership; the renderer recreates
its window and requires fresh presentation feedback. That Ultra touchscreen
handoff is now user verified. The backend acquires
its generation before touching runtime state and safely rejects a competing
owner. An owned dead socket can be reclaimed after a refused connection; never
delete its ownership lock to bypass a live manager or receiver launcher.

## Offline validation

Portable tests check per-phone persistence/isolation/removal, actual mode-specific
launcher settings, metadata validation and clearing, selected Bluetooth player
routing and unsupported controls. The CM5 additionally checks artwork decoding,
actual GTK toggle/Connect/confirmed Remove actions against an isolated fake
manager, and generated H.264 through the native map subsurface. No real phone is
unpaired or sent a media command by these tests. Both generated Standard and Ultra
configurations pass the real receiver's dry-run validation without radio changes.

```sh
cmake -S . -B build -DHEADUNIT_TEST_NATIVE_PHONE_PAGE=ON
cmake --build build -j3
ctest --test-dir build -R 'phone-management|native-phone-page|native-cluster' --output-on-failure
```
