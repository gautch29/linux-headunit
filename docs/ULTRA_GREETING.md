<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Signed Ultra greeting

## Contract

The verified Aston Martin-family pack owns the genuine Apple × Aston Martin
logo animation on **Cluster_Display**. The host supplies local scene intent;
the verified scene still runs unchanged. Optional authored Linux Head Unit decoration
replaces only the manufacturer image during its genuine logos phase, preserving
the divider, Apple CarPlay Ultra artwork and their animation. It does not rewrite
the signed pack, dispatch the animation's completion action or reset a pairing.
This does not implement or qualify a main-screen Hello sequence. The native Linux Head Unit boot animation is independent.

`create_cluster_bench_profile.py --session-greeting` declares two timed local
bindings with `sessionField=scene_state` and `sessionField=scene_options` at
`uiControl.uiState.uiSceneState` and `uiControl.uiState.uiSceneOptions`.
The profile decoder admits only these exact singleton paths/kinds, requires both,
and rejects fixed values, car-source mappings, units and conversions on them.
Timed arrays remain restricted to this host-owned options field; other local
array signals retain their fixed scalar-list policy. These local IIDs are not
phone VDP characteristic IDs. Legacy profiles retain fixed Drive until explicitly
regenerated; no installed or sealed profile is edited by this change.

The sequence is attached to the existing authenticated UI-service worker:

1. The first worker of a real control connection publishes STARTUP (1) and
   empty options. It loads both full entrypoints through the existing bootstrap
   receipt/clock gates. It does not claim ignition state or phone readiness.
2. On the cluster VM owner, a bounded read-only adapter observes the pack's
   existing workspace. It first witnesses LOGOS_TRANSITION, then STARTUP after
   the signed animator's own completion. DRIVE (2) is published only with a
   qualified visible cluster scene, retained real PTS, and a healthy output.
   There is no guessed animation duration or fabricated presentation receipt.
3. Both VMs consume that shared transition through their normal local data
   subscriptions. The signed scene handler consumes the subscribed state/options
   together when either changes, preserving each original source date. Main, cluster, streams, UI-Sync, touch, audio and control keep
   their existing owners. The pack itself hides/reveals its remote artwork.
4. A replacement UI service on the **same control connection** resumes in DRIVE
   with empty options. Failure before/during startup consumes that connection's
   greeting attempt too; recovery cannot replay it. A new authenticated control
   connection is a new session startup and may show its own greeting.
5. A witnessed animation stuck for 15 seconds fails the worker, using existing
   retirement/fallback. Expiry never publishes DRIVE. Hidden/unqualified output
   cannot complete startup. Malformed workspace queries fault the VM. Retirement
   withdraws only these two local scene inputs; timed expiry also makes them
   unavailable if the owner stops publishing.

Dates on these inputs are real monotonic observations of the session owner's
current **UI intent**, not car sensor acquisition dates or fresh phone frames.
The observer never runs on the control/socket thread and does not consume or
acknowledge phone commands. Ordinary frame/bootstrap turns observe the scene;
feedback-only polls do not advance this lifecycle. The existing frame budget
bounds the query. The supported dialect is restricted at configuration admission
to the four inspected reference/Single identities and versions (985/547).

## Optional first-pairing insertion

`firstPairingAnimation=true` in the installed-UI config is default-off and
requires a session-greeting profile. The receiver grants the option only after
cryptographic pair-setup has produced a verified controller record, that
controller was absent from the real private HAP pair store, and saving the
record succeeded. The subsequent pair-verify must authenticate that same
controller identifier and public key before the Ultra factory can use it. This evidence stays on its originating control connection;
there is no persistent "first run" flag masquerading as pairing.

For that connection's first worker, the host supplies DRIVE plus
`["AllowPairingAnimation"]`. The signed workspace inserts its own startup and
pending Drive sequence. The host witnesses logos followed by signed DRIVE and
then clears the option. Failed/rejected pairing, pair-verify of an existing
record, ordinary reconnect and service replacement do not authorize this option.
If the phone pairs on one TCP connection and starts Ultra on another, this
conservative implementation does **not** transfer the grant; it uses ordinary
STARTUP instead. Cross-connection grant transfer needs a separately qualified,
authenticated controller-identity contract. Do not delete a pairing to test it.
The normal STARTUP route already shows the genuine logos for existing phones.

## Offline qualification

The portable lifecycle tests cover STARTUP/DRIVE, witnessed completion,
clock/visibility gating, first-pairing option removal, resume without replay,
reversed-clock rejection, timed expiry, withdrawal, stuck-animation failure and
profile rejection. VM tests cover the read-only constructor proxy, observed
phases and malformed/faulted workspace retirement. Generated profiles never
assert fixed pairing or startup.

The signed dual-VM `--greeting` probe runs both full entrypoints, supplies an
empty explicit `iPhoneVideo` input, observes the unmodified signed completion,
asserts both final DRIVE phases and checks retired inputs are unavailable.
The empty main image is expected without phone video; it is not a Hello result.
Its PTS, foreground and visibility are **simulated fixture inputs**. Theme uptime
uses actual elapsed intervals. GPU captures use only the offscreen render node;
PNG readback is inspection evidence, not compositor presentation.

```sh
python3 tools/create_cluster_bench_profile.py --session-greeting \
  --extended-readings --observed-transmission --output /private/greeting.plist
HEADUNIT_THEME_PROBE_GREETING=startup \
  build/headunit-theme-probe /private/installed 44C9FA31EC 985 \
  /private/accepted-reference.conf --greeting /private/greeting.plist /private/startup
```

Modes `startup`, `first-pairing` and `resume` are explicit offline fixtures;
`first-pairing` does **not** pair a phone. On Linux, add
`HEADUNIT_THEME_PROBE_GPU_ONLY=/dev/dri/renderD128` for offscreen GPU captures.
The probe retains an explicit 1,000 ms script/2 s GPU diagnostic budget; those
limits do not weaken the live worker or certify live frame timing.

On 9 October 2026, the Mac portable suite passed **142/142** after refreshing
CMake's stale Homebrew libplist paths. All three signed Mac dual-VM modes passed:
startup and first-pairing observed the original logos/completion and both final DRIVE phases; resume reached DRIVE
without logos. Retirement made both managed inputs unavailable. The standalone
Mac cryptographic pair-verify fixture passed valid signature/identity binding,
invalid signature rejection and unpaired-controller rejection. The live receiver
header also passed strict Mac syntax checking with both GPU/UI feature guards.
Direct Mac syntax checking of the Linux installed-UI source is unsupported by
the installed Apple libc++ (`std::osyncstream`); it is not a Linux build pass.
Pinned formatting and whitespace checks pass.

The exclusive CM5 qualification built all **11 requested targets**, including
full `headunit-control-probe` and `headunit-theme-probe`, with theme GPU/video
features enabled. The exact affected CTests passed **12/12**, serially:

```text
hap-pair-verify
theme-scene_lifecycle
theme-script_runtime
theme-local_vehicle_profile
theme-vehicle_script
theme-worker
theme-session
theme-gpu_scene
theme-gpu_scene-wayland
theme-display-wayland
theme-display-overlap-wayland
theme-display-preview-wayland
```

`theme-gpu_scene` used only `/dev/dri/renderD128`. The Wayland fixtures created
and retired their own headless Weston instances. All three signed **offscreen
GPU** greeting modes passed: startup and first-pairing witnessed original logos
and completion, resume witnessed no logos, and both final VM phases were DRIVE.
The probe explicitly asserted both scene state and options unavailable after
retirement. All eight original 1920 × 720 PNGs were retained and visually
inspected: both logo images contain the genuine Aston Martin/Apple CarPlay Ultra
artwork; every final cluster contains the expected instruments with unavailable
vehicle/transmission readings; every final main image is empty without phone
video. This does not qualify either physical panel or a main Hello sequence.

The build used shared pinned Skia/Luau, verified unchanged across **14,734 input
files**, and the disclosed GCC bench exceptions
`-O2 -g0 -DNDEBUG`/`-Wno-misleading-indentation`. It is not default strict-matrix
or live timing evidence. The subsequent
[integrated strict Docker matrix](CI.md#integrated-seven-worker-candidate--9-october-2026)
passes separately; it excludes GPU hardware tests and does not extend these
offscreen fixtures to authenticated phone or physical-panel acceptance.
Each build checked at least 1.5 GB free, used `umask 022`, `TMPDIR=/tmp` and
`nice -j2`, and the own workspace remained below 1 GB. Exact tracked-source
hashes, compiler/flags, build/CTest/probe logs and visual inspection receipts
are archived privately. After qualification, no owned process remained and only
the greeting workspace was removed; the exclusive CM5 slot is released.

An initial Mac run used a stale Homebrew libplist library path; refreshing CMake
dependency discovery fixed it. One interim portable run passed 141/142; its
runtime suite returned the generic
`check failed: rejected` diagnostic; its exact case was not identified. The
standalone rerun and subsequent full 142/142 runs passed. The signed first-pair
probe initially exposed the partial state/options delivery bug; the coupled local
UI-state update and regression check now pass. The initial CM5 builds were
interrupted by the resource pause; their logs remain
archived and are not passing evidence. The first exclusive build then rejected
the pairing fixture's `decltype(&fclose)` template argument under GCC's
`-Werror=ignored-attributes`. A lambda deleter fixed the glibc portability issue;
the original failed log is retained, with flags and assertions unchanged.
Retirement assertions were subsequently strengthened to cover both managed
inputs before the final full scoped rerun. The earlier generic Mac failure is
retained as a diagnostic receipt; its original raw CTest log had been superseded
by a later run, so the failing case remains unidentified. These component passes
supply no authenticated phone, readiness or whole-appliance acceptance.
Private receipts/captures are retained outside Git. Hosted CI is paused; this
branch is committed locally with `[skip ci]`, without publication or deployment.

## Bounded live test plan (live owner only)

This branch has not selected, installed or restarted a release, touched the
phone/screens, or rebooted the appliance. The selected generation remains the
live owner's responsibility. A new retained candidate must include the changed
receiver/runtime, a regenerated session-greeting profile and rollback inputs;
never alter V30 in place. Keep first-pairing insertion disabled for the first run.

1. Allow one existing-paired-phone connection to that candidate, with the same
   pack, display geometry, layout, gauges, Climate/settings and audio routes.
   Capture private bounded Workspace diagnostics and both panels for at most
   20 seconds from authenticated UI-service startup. Check the complete genuine
   cluster logos → outline → Drive, main picture/touch, two active streams,
   UI-Sync progress, and genuine fresh launch-bound readiness. Record logo
   completion, first useful picture and ready dates separately.
2. Within a two-minute session, run one Home/Back, one simulated Climate/settings
   round trip and one connected camera On/Off **only if** that candidate already
   admits the qualified camera owner. Keep both panels, streams, UI-Sync and
   audio/control alive. Check pending style requests remain rejected during the
   animation and normal selections work after Drive; no added mode authority.
3. One bounded disconnect/reconnect (30-second ready limit) must retire the old
   scene, reveal the existing native fallback and create a fresh session. A new
   control connection may greet once; Home/Back and same-connection service
   recovery must not replay it. If service replacement cannot be induced safely,
   retain that as component-only evidence; do not invent a phone teardown.
4. Stop on worker fault, clock loss, missing panel, stuck animation, stale
   readiness, or broken main/touch/Climate/camera. Preserve logs and native
   fallback, retire the complete owner group and use retained rollback under the
   live owner's deployment procedure. A failed animation is not ready CarPlay.
5. Optional pairing is a **separate** run only when a genuinely new phone is
   available and the user authorizes pairing it. With the option enabled,
   verify actual new-record/save evidence on the same authenticated connection,
   one signed insertion and option removal. Test rejection/cancellation and an
   existing-phone reconnect; neither may grant the option. If the phone changes
   connections, document the conservative non-insertion. Never remove/reset an
   existing pairing merely to manufacture first-pair evidence.

Cold boot/Linux Head Unit coordination, main Hello, physical ignition/vehicle providers,
sustained frame rate, audible audio continuity and cross-connection first-pair
behavior remain separate live/hardware qualifications.

## Real-phone budget failure — 9 October 2026

Combined source `6b89b85` passes the complete CM5 suite (212/212), the three
signed offscreen GPU greeting modes and the exact native-clock fixture. The
offscreen greeting probe deliberately uses a 1,000 ms diagnostic script budget;
that pass does not establish the production 20 ms frame-turn budget.

The retained hardware-authenticated V33 candidate enables the timed greeting
profile. A real iPhone completes control/clock setup and both video streams,
but the signed STARTUP transition exceeds the live VM budget: one captured turn
reports 31,600 µs CPU / 31,663 µs wall against 20 ms, with four resumed tasks.
`APL/Thread.lua:135` reports `asset execution budget exceeded`; the owner closes
both video consumers, retires the terminal Ultra/control session and reconnects.
Four captured connections repeat that failure. No complete physical logos →
Drive or ready Ultra result is claimed. The whole graph was retired and working
presentation-clock V31 restored; the candidate remains retained and unselected.

Greeting stays disabled for the combined source's default-profile qualification.
The production deadline was not increased. Before another enabled live run,
measure the cold STARTUP/Drive transition under production limits and address
its cost or qualify an explicit bounded startup-only scheduling policy. Include
the first ordinary Drive turn and timeout/retirement in that qualification;
the diagnostic-budget probe cannot substitute for it.

Private source/build, offscreen images and real receiver/retirement receipts are
in `~/.cache/headunit-qualification/20261009/takeover-testing/` and the CM5
`headunit-agent-integration-20261009` workspace. Sealed V33 preserves the exact
enabled profile used by the failed run; no signed pack or sealed payload changed.

## Deferred-resource repair — 9 October 2026

`96186cd` retains the exact resource bytes already decompressed and checked at
signed-bundle admission, within the existing 256 MiB expanded-total limit. The
compressed input is released afterward. Resource access copies admitted bytes;
it does not reopen/rescan the ZIP or relax script limits. All signature, CRC,
path/type, count/size and metadata identity checks remain in admission. See
[asset ownership](ULTRA_ASSETS.md).

The production-budget probe now starts scheduled tasks using the worker's
initial startup turn. `HEADUNIT_THEME_PROBE_GREETING_PRODUCTION=1` then enforces
20 ms normal turns. `HEADUNIT_THEME_PROBE_GREETING_DELAY_FOREGROUND=1` deliberately
foregrounds both VMs two fixture turns later, reproducing a cold resource load
outside the initial startup turn. These visibility/PTS inputs remain simulated.
Without the resource repair, that delayed fixture fails at the same signed
`APL/Thread.lua:135` path: 31,941 µs CPU / 31,955 µs wall, four resumed tasks,
20 ms budget. Individual cold resource lookups rescan the ZIP in roughly
19–21 ms. With the repair, the same real-signed CM5 offscreen GPU fixture observes
logos, reaches both final Drive phases and withdraws inputs on retirement under
20 ms. Its ordinary foreground variant also passes. The retained payload for
the inspected pack is 37,861,837 bytes; the generic worst-case cap is 256 MiB.
This changes memory retention, not authenticity or readiness rules.

## Main-screen Hello ownership

The inspected signed pack's STARTUP action explicitly runs logos only for
Cluster_Display. It contains no Hello artwork/script; the main display's welcome
comes from the phone's video. Local iOS 26.3 simulator DashBoard metadata and
method inspection identify `DBHelloLockOutView`/`DBHelloView`. Its Hello gate
requires gauge/display-plugin support and a paired vehicle without a saved
`finishedWelcome` value; completion marks the welcome finished. This is a
simulator-derived first-time-welcome contract clue, not a readback of the real
iOS 27.2 phone's private vehicle record or a successful phone animation.

Further inspection traces gauge support to `DBSessionController`'s current
supported features, supplied by `CRFeatureAvailability` through the phone's
feature service using the session's MFi certificate serial. This is distinct
from the receiver's AirPlay feature mask. Advertising DPManaged panels or
rendering the signed cluster does not independently establish this Hello gate.
The inspected gate tests whether `finishedWelcome` exists, not its Boolean
value; a saved false value is not equivalent to an absent value.

`DBDashboard.handleConnect` also calls `_setDidFinishWelcomeIfNeeded` after
setting connection-ready. If both saved `displayThemeData` and
`finishedWelcome` are absent, that path saves welcome-completed as true without
requiring Hello animation completion. If either is present, it leaves the record
unchanged. This provides another possible first-connection consumption path;
it does **not** prove the Siri-disabled connection used it on the actual phone.
Resolving the real phone's result needs its supported-feature result, main
display-plugin support, saved theme/welcome state and their connection ordering.
The simulator inspection changes no phone state and supplies no new eligibility
authority to the host. These remain clues for future missing-welcome cases;
the actual received Bonjour video below already confirms that this phone
generated its welcome.

The timed profile and verified-new-controller insertion preserve the genuine
phone/signed route. No host video replay, local replacement Hello, invented
phone command, pairing reset or arbitrary welcome-completed flag is used.
An existing paired phone can qualify the cluster startup; first-time main Hello
needs an eligible phone and an actual recorded welcome. Pairing on a different
TCP connection retains the conservative ordinary STARTUP behavior. Neither
component pass nor logo presentation alone qualifies main Hello.


## Manufacturer-only decoration

Setting `clusterGreetingBrand=brand` enables the Linux Head Unit emblem during the
observed cluster logos phase. The default `asset` uses the supplied scene
without this decoration. `BrandGreeting` searches a bounded scene tree,
requires a unique supported manufacturer/divider/Apple group, and replaces
only the manufacturer image in the host's frame copy. It retains the
divider, Apple layer, signed motion, warning overlays, UI-Sync and both streams.
Drive, a reversed clock and owner retirement withdraw the decoration.

Earlier CM5 checks exercised the original signed animation and a prior
manufacturer decoration. The current emblem's tests use a synthetic scene to
check scope, inherited motion, presentation bounds and retirement.


## New-phone main Hello result

The new phone's first physical main picture reports CarPlay unavailable because
Siri is disabled. A later reconnect shows normal CarPlay Settings. The original
short screen watch contains no handwritten Hello, but it cannot establish that
the phone never sent the welcome.

Offline inspection of that original V37 connection's retained main-video
packets **finds the genuine handwritten Bonjour animation**. The existing
bounded UHVP1 converter and FFmpeg decode all 894 captured frames. Source
frames 64–270 contain the welcome, starting 36.183 seconds after the first
captured phone PTS and spanning 3.550 seconds. Phone video first shows the
Siri-disabled error, then Bonjour, and later ordinary CarPlay content. The
source's relative timestamps are retained; they are not replaced by a claimed
head-unit presentation clock.

The physical observer took 77 paired-panel captures over its first 45-second
window from Connect, ending before this late welcome. Main remote-UI visibility
and qualified-scene/video counters continue during the connection, but there
is no Bonjour-specific compositor receipt or physical capture. Thus **phone
Hello generation is confirmed; physical main presentation remains unqualified**.
A hidden/dropped welcome has not been established. The later 20 ms VM fault
occurs about 89 seconds into the connection, after the received Bonjour, so it
does not explain that capture gap.

The inspected received-video clip is
a private phone capture
(1280 × 480, 3.583 seconds, 208 decoded playback frames). It contains only the
genuine phone welcome, with source-relative frame durations; it is not a
physical CM5 recording, generated artwork or a replay installed on the head unit.
The user requested continuing without USB logs; no phone log reader is running.
No welcome-completed record is read or changed and both pairings remain intact.

For the next eligible first-time phone test, enable Siri before pairing and
capture the main panel across the complete phone interaction, with a bounded
120-second window from Connect. Do not stop solely on renderer Ready: that can
precede phone prompts and welcome. Preserve both received video and physical
capture, with their own timing provenance. A missed window is inconclusive,
and a normal reconnect of this now-paired phone is not a fresh welcome attempt.

The V39 follow-up records three complete main-panel Disconnect/Connect sequences
with the same now-paired phone. Their inspected video tails show native Phones,
black waiting output, then normal CarPlay Settings; no Hello is observed. These
are reconnect observations, not three fresh-phone welcome attempts. Each run
recovers both panels and passes same-generation Home/Back. A subsequent warm
boot initially reaches fresh confirmed Ready, but later loses its phone clock
and retires the first connection; [PHONE_CLOCK](PHONE_CLOCK.md#v39-warm-boot-failure--9-october-2026)
owns that failure. The greeting code and selected release remain unchanged.

Only the current iPhone is available. DigiRing's mountain wallpaper is captured
again on V39 with the vibrant palette, then MapsMedia is restored and confirmed.
The comparison against the original phone remains pending. Private captures,
sampling receipts and sanitized summaries are in
`takeover-testing/hello-followup-v39/`; none is stored in Git.

## Physical main Hello — 10 October 2026

The user chose Forget This Car on the iPhone, enabled Siri and paired again
with selected V49. The receiver logs a fresh pair-setup, pair-verify and
Ultra negotiation on the same control connection, then signed STARTUP on both
panels and DRIVE about 5.6 seconds later. The retained main stream contains the
genuine handwritten **hello** (black frames, hello on a dark blue background,
fade, Maps intro, then Home). **The user saw it on the physical main panel.**
This is a user observation; there is no compositor receipt or panel capture of
the welcome itself.

The welcome follows the cluster logos instead of playing with them, which
matches the ownership above: the signed STARTUP drives only Cluster_Display and
the phone chooses when to send its one-time welcome. Host evidence bounds it
between DRIVE (20:10:59.5 CEST) and the first panel capture already showing Home
(20:11:13); a screen-capture attempt using an unsupported grim JPEG mode started
late. The capture's phone timestamps are not host elapsed time — the main
stream spans 372 s of PTS in a 108 s connection, and earlier sessions show the
same excess — so earlier PTS-relative offsets, including the 36 s Bonjour
offset above, are positions in the phone's stream, not head-unit timing.

Syncing the cluster logos with the welcome is not planned. The default-off
first-pairing insertion remains disabled and unqualified. The decoded clip is
a private phone capture
(re-timed to an assumed 30 fps for viewing; not a CM5 recording). The private
session and late panel captures stay on the CM5 under
`~/ultra-headunit/captures/cm5-live/session-20261010-201038` and
`~/hello-watch-20261010`. The new pairing is retained; nothing was reset by the
host.
