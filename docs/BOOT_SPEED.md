<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Boot and CarPlay connection speed

Source-only qualification, **9 October 2026**. This branch changes no selected
release, service, OS setting, phone state or physical output. Deployment authority
and selected V30 remain in [HANDOFF](HANDOFF.md). Component checks below do not
establish a faster boot. The live owner must run the bounded comparison below.

## Existing warm-boot timeline

The read-only capture is kernel boot
`00776006-c995-487e-87de-637b708e8a83`, selected V30. Kernel-relative Linux
monotonic dates are used throughout; firmware/power-on time is excluded. Journal
values date receipt of the log message, while connection diagnostics date their
original operation. The current sole control process's stdout was used to bind
its receiver capture to this boot, rather than choosing a historical log by
filesystem mtime. iAP and control are separate owners, both generation 1 here.

| Uptime (s) | Observation | What it establishes |
| --- | --- | --- |
| 0 | Kernel timeline origin | Does not date ignition or firmware startup. |
| 1.659 | Kernel finishes / userspace begins | From systemd's startup summary. |
| 3.448 | Retained recovery gate reports restored five-owner graph | Integrity/recovery admission; no display or phone readiness. |
| 3.637–6.442 | NetworkManager service start | 2.805 s dependency before the managed service can start. |
| 4.173 | labwc process starts | Compositor process, not its first scanout. |
| 5.660 | Output-owner process starts | Type=simple, not configuration completion. |
| 6.078 | Kanshi reports both-output profile applied | Configured modes; no startup branding pixels measured. |
| 6.107–6.645 | Page release guard success → validate-only completion | 0.538 s includes Wayland wait, validation and process/scheduling overhead. It is not validation CPU time. |
| 6.938 | Managed process starts | Static admission and selected-child preflight still follow. |
| 7.590 | startup branding surfaces constructed / startup log | Window creation, **not first visible frame**. |
| ≤7.984 | Both first scenes constructed, Wayland sync, page READY notification | Systemd records page startup complete at 7.984 s. Construction is bounded between 7.590 and 7.984 s; physical first startup branding pixels remain undated. |
| 8.598 | Fallback cluster process starts | Ordered after decoration admission; does not await animation retirement. |
| 8.600 | Multi-user startup complete | Not CarPlay readiness. |
| ≤9.187 | Bluetooth preparation and AP enablement completed; DHCP bound | Existing launcher orders these before DHCP. `AP-ENABLED` exists but has no timestamp; 9.186902 is the DHCP journal bound. |
| 9.471–10.521 | Reconnect helper control/paired inspection and discovery | 1.022 s discovery returns no target; this is a separate helper, not a receiver-thread wait. |
| 12.445 | startup branding retirement | Duration/fade completion; grants no phone readiness. |
| ≤12.995 | Wireless iAP profile accepting a phone | Radio readiness lacks an exact original timestamp. The current control process starts at 9.23 s (`/proc` start ticks, 10 ms resolution) and accepts iAP at 12.994664 s: retain that interval, not an invented ready time. |
| 12.994664 | iAP accepted | Original diagnostic date. |
| 13.239337 / 13.416913 | iAP sync / link established | Link startup occupies 0.422 s from engine creation. |
| 13.771864 | Identification accepted | Before authentication. |
| 13.773104–13.855962 | MFi certificate read | 82.858 ms. |
| 14.082028–14.535956 | MFi challenge signing | 453.927 ms. |
| 14.729402 / 14.795274 | MFi authenticated / CarPlay start prepared | Preparation is not send completion or phone acceptance. |
| 15.424 / 15.643 | Wi-Fi association / DHCP acknowledgement | Journal observation, not control readiness. |
| 15.723109 | IP control accepted | 0.928 s after start preparation. |
| 15.823246 | Encrypted control | Pair-setup and pair-verify succeeded. |
| 15.834125–16.391574 | AirPlay auth-setup | 557.449 ms; separate authenticated transaction. |
| 16.438664 / 16.469380 | Initial setup complete / RECORD accepted | Neither implies presentation readiness. |
| 16.561569 | Phone clock locked | Original qualified clock boundary. |
| 16.643389 / 16.758259 | Ultra negotiation / main stream setup | Phone-authoritative Ultra. |
| 16.880554 | First video frame observed | Encoded ingress; not decoded/presented pixels. |
| 20.763892 | Qualified presentation-ready | Existing receiver diagnostic, bound to control generation 1 and link generation 1. |
| ≤23.01 | Manager poll observes ready with no error | Historical roughly two-second poll upper bound. Compatible with 20.763892; not a second boot. |

The 3.883339 s from first encoded video to qualified presentation includes script
entrypoint loading, GPU/decoder startup, clocks and genuine presentation feedback.
There are insufficient stage observations to allocate that interval among them.
It must not be described as a removable fixed sleep. Likewise the first physical
startup branding frame and exact complete radio-ready time cannot be recovered from these
logs. New source narrows the software boundaries; an optical recording still
supplies first visible content.

Private evidence is under
`~/.cache/headunit-qualification/20261009/boot-speed/`: sanitized `timeline.json`,
`boot-observations.jsonl`, source/log hashes and the private journal. Earlier
`comprehensive-pass/warm-v30-observations.jsonl` supplies the manager poll bound.
Historical V19/V20 timings in [STARTUP_SPLASH](STARTUP_SPLASH.md) are separate
release/OS generations and are not comparable candidate measurements.

## Ranked changes and remaining opportunities

Ranks prioritize useful, safely attributable work. Budgets are observed windows
or component timings, **not demonstrated end-to-end savings**, and cannot be added
as independent savings.

| Rank | Change / opportunity | Evidence and possible saving | State |
| --- | --- | --- | --- |
| 1 | Admit the managed configuration while NetworkManager initializes | NetworkManager takes 2.805 s; manager spawn → DHCP binding takes 2.249 s in the existing serial graph. Up to that latter window could overlap network startup, but radio/AP work and late graphics still form floors. No whole-window saving is promised. | Implemented in source. |
| 2 | Remove the resident page's duplicate validate-only process | Boot guard → preflight completion spans 0.538 s including the Wayland wait. Three read-only warm-cache validations take 269/280/297 ms. During concurrent CM5 fixture/build load, three further runs take 685/1019/1160 ms including runuser overhead; they are a different workload. | Implemented in source. |
| 3 | Attribute the 3.883 s video → presentation interval | Largest remaining measured presentation interval; removable portion unknown. Both entrypoints intentionally load before scheduled tasks to avoid one panel's timers running during another panel's `require`. | Investigate finer worker stage timing; preserve the existing VM/GPU owner and bootstrap draining. |
| 4 | Attribute NetworkManager's settings/plugin initialization | 2.516 s between settings plugin/ifupdown initialization at 3.874985 and DHCP init at 6.390209, with three Netplan permission warnings. Warnings do not prove causation. | Read-only finding. A separate retained OS experiment could remove unused provisioning integration after proving Ethernet/AP compatibility and rollback; no OS changes here. |
| 5 | Establish the profile-ready → iAP-accepted wait | Complete radio-ready is only bracketed 9.23–12.995 s. Discovery's 1.022 s is off the receiver path and returns no endpoint. | Measure first; do not lower retry/backoff or remove discovery from this evidence. |
| 6 | Authentication cost | Certificate 0.083 s, iAP signature 0.454 s, auth-setup 0.557 s; about 1.094 s of measured operations. | Preserve authentication and synchronous chip ordering. No safe deletion/parallel signature change established. Software MFi acceptance is a separate open gate. |

Completed cloud provisioning and network-online/removable-storage waits are
already disabled on this bench. Removing them again offers no saving. Do not
remove firmware/BlueZ, sealed guards, the retained gate, DHCP, output routing or
authentication to improve target timing. The first Bluetooth reconnect already
has no three-second grace; subsequent-session grace/backoff remain unchanged.

## Source ordering and failure contract

The managed unit still wants NetworkManager, Bluetooth, discovery and the native
cluster, and requires the sole compositor. It orders after Bluetooth/discovery
but no longer after NetworkManager, and no longer runs `nmcli` before the manager.
The manager and launcher still admit their sealed configuration and inherited
owner lock. Only the managed launcher's existing radio preparation then waits
for the existing NetworkManager's fixed D-Bus Version property, with a six-second
monotonic budget, ≤1 s property calls and ≤100 ms polling. A late successful
property reply is rejected. `nmcli radio wifi on` must succeed within its separate
five-second command bound before Bluetooth preparation or AP ownership starts.
Cancellation is checked before waiting, after readiness and after that command.
Failure goes through the existing launch cleanup/manager failure policy; there is
no new daemon, indefinite retry, second radio owner or readiness cache.
The direct/legacy receiver service keeps its existing network ordering/preflight.

The page keeps its release guard and owned Wayland/output wait. Its actual
`phone_page_boot.main` still validates profiles/routes/account and rechecks the
sealed release before opening a page owner, importing GTK or notifying systemd.
Publication preflight remains. The cluster's notified-page ordering, bounded
failure fallback, transparent underlay and replay suppression remain intact.
New templates use no new substitution fields or sealed inventory modules;
actual compatibility with the installed retained helper must still pass before
selecting a candidate. No helper migration is authorized by these changes.

Launcher diagnostics are opt-in through the existing `connection_diagnostics`
setting: at most nine fixed `Headunit startup diagnostic` observations per
managed launcher, with genuine `CLOCK_MONOTONIC` dates for Wi-Fi wait, Bluetooth,
AP enablement, Wayland wait and receiver spawn. Spawn is not listener/profile
readiness. The existing bounded C++ diagnostics additionally use `scope=startup`,
generation 0, to date the open control listener and successful completion of
Bluetooth profile startup. Failed startup cannot emit `iap-profile-ready`. These
are transport availability observations, not qualified phone presentation. startup branding emits exactly one first-scene construction date per admitted
output. Clock/output failure emits no fabricated date and cannot change admission.
These logs contain no identity, credentials, private path or phone payload.

## Component qualification

Mac: pinned clang-format 19.1.7, whitespace check, portable Debug build and
**142/142 CTest suites** pass. OpenSSL 3.6.4 is explicitly selected through
pkg-config; libplist 2.8.0 and the existing pinned Luau source are used. Initial
runs retain evidence of moved Homebrew dylib paths, an OpenSSL 4 deprecation
failure, obsolete assertions requiring the removed preflight, and the adjusted
shared diagnostic-budget assertion after adding two startup observations. Those are
separate from the final passing build/suite; no warning suppression was added.

The new five-case command/clock fixture checks late/missing/stalled network
readiness, Wi-Fi command failure/timeout, cancellation and legacy behavior.
Actual file/checker fixtures check runtime profile admission before ownership,
post-admission integrity failure, exact guarded bundles and preserved output/
notification ordering. Diagnostic fixtures prove once-per-output emission,
no fabricated date on clock failure and unaffected admission. Phone/network
commands in these fixtures are explicit substitutes.

CM5: **10/10 affected software suites** pass with the retained V30 display
checker copied into the isolated workspace. The added C++ stage diagnostics are
separately compiled in the isolated Linux workspace (non-Skia control receiver).
**2/2** scoped C++ suites pass (`connection-diagnostics`, private-D-Bus
`bluetooth-iap-profile`); the non-Skia control receiver compiles without running
it. GCC uses the handoff bench exception `-O2 -g0 -DNDEBUG` with
`-Wno-misleading-indentation`, so this is not a default warning-clean build.
Pinned Luau source is reused from the shared protocol qualification workspace;
Skia is not rebuilt. The complete Skia receiver is not rebuilt or qualified. The release suite initially exceeded a 45-second
outer runner budget under concurrent load; its final isolated batch took 58.18 s
inside an explicit 180-second outer bound and passed without changing production
or individual test deadlines.

A private headless pixman-compositor / actual GSK OpenGL probe passes **4/4**
cases: two native-size first-scene dates/normal retirement, absent output, render
failure and stalled construction. The original full headless GTK fixture failed
its pointer-while-visible assertion during concurrent CM5 load (load average
about 12), after collecting first-scene dates and logo images. Its failed log
and partial report remain in the original `cm5-evidence-final.tar.gz`; neither
the load nor the later passing run establishes the cause. The earlier scoped
probe recorded roughly 8 snapshots/s under that load.

The exclusive-slot rerun on **2026-10-09, 16:02:33–16:03:28 UTC** passes the
**unchanged full fixture, 21/21 checks in 55.20 s**, including the formerly failed
pointer condition, retirement/failure paths and all four native GTK regressions
(page, pairing, resident page and artwork). It uses the same default private
headless pixman compositor / GSK OpenGL mode, with no individual deadline
increase or assertion change. It does not use `--gpu-compositor`, a physical
output, a radio or a phone. This is isolated GTK qualification, not physical
pointer/scanout, displayed frame-rate or end-to-end boot evidence.

All **734 tracked files** copied from `655f31ba6e289e7481e588446ac1e732d34cc1ff`
match before and after the run. Shared Skia revision/argument receipts and the
Luau archive pin were checked; all 1,432 shared Luau source files match after
normalizing only the two reviewed patches. Neither dependency is used by this
Python/GTK fixture and no build was needed. It ran with `umask 022`,
`TMPDIR=/tmp` and `nice -n 10`, with 2.9 GB free, an 11.51 MB workspace and no
competing build observed. One-minute load was 1.11 before and 0.72 afterward;
live services were left running.

Exact tracked source, manifest, command, runner, tool hashes/package versions,
logs, images, results and dependency validation are archived privately under
`~/.cache/headunit-qualification/20261009/boot-speed/full-headless-exclusive/`.
`exclusive-evidence.tar.gz` is 5,346,125 bytes, SHA-256
`47f76442c92a13d613643761f28776bbe18033c0555b6561f19e1f1ebef6955f`;
the retrieved archive's 734 source hashes and passing report were verified.
`slot-release.json` records no owned PID and removal of only
`/home/headunit/headunit-boot-speed-20261009` at **16:04:46 UTC**. The CM5 slot is
released. Integrated source still requires the manager's qualification.

No physical session or new release is started. Actual systemd boot ordering,
complete radio-ready timing, end-to-end savings, cold-power timing and real-phone
failure/recovery remain unverified by component suites.

## Exact bounded live comparison for the live owner

Do this only in the owner's exclusive deployment/phone window. This branch
performs none of these actions.

1. Retain V30 and its exact rollback. Publish a **new** candidate from the
   reviewed integration revision, with the same signed packs, pairing, display,
   audio and simulated Climate/settings inputs. Run candidate and installed-old-
   helper preflight/render comparison and sealed/unit verification. Stop if any
   guard/bundle differs unexpectedly; do not edit the selected release or helper.
2. Quiesce builds and other qualification clients. Keep Ethernet management and
   both physical outputs powered. Keep the same selected phone, location, charge,
   radio availability and locked/unlocked state for every run. Preserve pairing.
   Enable the existing bounded connection diagnostics in the published inputs.
   First rerun the complete private headless startup fixture after builds stop;
   require its ownership/pointer/failure cases to pass before live measurements.
3. Run **three warm V30 baselines and three warm candidate boots**, alternating
   baseline/candidate when practical, maximum **90 seconds per boot**, with one
   owner graph. Before each boot record release/source/input hashes and phone/
   bench conditions. Readiness must be fresh, launch-nonce/generation matching,
   error-free and backed by current presentation; service-active/animation-end
   never substitutes. If the first session fails, count that boot as a failure
   and report reconnect separately rather than replacing its sample.
4. For each boot retain boot ID, kernel/userspace timing, restart counters, jobs,
   journal JSON, launcher/receiver/reconnect monotonic diagnostics and the
   original readiness publisher. Poll the existing manager at **250 ms for at
   most 90 s**, dating dispatch and completion with the host clock; record the
   poll interval separately from exact receiver diagnostics. Keep phone payloads
   and private state external to Git; export only stage/count/date summaries.
5. Record both panels optically from before reboot through first useful CarPlay.
   At ≥60 fps, date first visible startup branding on each panel, first native interface and
   first useful phone content. Optical frame cadence gives uncertainty, not a
   kernel timestamp: retain a common visible/host timing reference and its error
   instead of silently subtracting camera wall time from Linux monotonic time.
   If that independent reference is unavailable, report first physical startup branding
   relative to power-on only and keep kernel-to-scene construction separate;
   do not claim an exact kernel-to-photon time.
   Confirm no HDMI console/login/pre-animation gauges. New first-scene timestamps
   are software construction evidence only. Use launcher `ap-enabled`,
   `bluetooth-ready` and C++ `scope=startup stage=iap-profile-ready` dates together
   for full radio availability; `control-listening` separately dates the socket.
   Use these original dates instead of process-active or file modification times.
6. After the last successful candidate boot, check both panels/main touch,
   Home → Back, and retained simulated Climate/settings once, **≤60 s total**.
   Require zero unexpected service restarts, no bootstrap decoder stall, no
   competing owners and genuine ready state on both panels. Stop on the first
   regression and restore the retained graph under the normal owner procedure.
7. With warm ordering stable, run **one controlled cold-power candidate boot**,
   maximum 90 s, with both panels and the same available phone. Optical power-on
   → first startup branding includes firmware; retain it separately from kernel-relative
   timing. Do not infer a cold result from the warm median.

Total measurement budget: six 90 s warm runs + one 60 s functional check + one
90 s cold run = **11.5 minutes**, excluding explicit retained-release selection/
verification. Report every run, median/range, first-session success count, stage
intervals/uncertainty and any timeout. A single faster run cannot establish a
repeatable saving. NetworkManager-absent and unsafe-profile failure handling is
covered by isolated fixtures here; any actual systemd fault exercise must use
inert private units, never disable the live management network or radio owner.
