<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Sport / track telemetry engine

## V84 live labelled simulation — 14 October

Frozen `3bd3db9`, explicit hash-bound opt-in: 298 polls over 65s, both Ultra panels
Ready and owner identities unchanged. Original input acquisition/age/origin
fields verify available and simulation. Timers 0–100/50–80/0–200/quarter-mile
complete at 4.9782/1.5/9.9782/12.0619s; dated simulated GPS laps last/best 20.000s.
History 27 at collection, rejection 0. Intentional service restart creates a new
session, preserves the line and capped 64 history; 43 exact prior entries overlap
while newer simulation events retire oldest entries. Isolated qualified CM5
MCU fixtures pass 17 freshness/replay/expiry checkpoints and 501 HVO6 sport cases.

This qualifies the simulated model/socket behavior, not physical acceleration,
GPS/IMU accuracy, car authority or native UI's sport UI. Physical providers remain
disabled/unqualified. Private original samples and restart receipts:
`20261013/phone-session/v84-sport-summary.json` and `v84-sport-restart-summary.json`.

Original source qualification, 13 October 2026. The engine, manager JSON and explicit synthetic
sources work without a phone. The source worker changed no GTK design, signed pack, native cluster format,
live service or release; V84 adoption is recorded above. native UI owns the sport page. No physical
vehicle, IMU, GNSS, calibration or timing accuracy is qualified.

## Model and ownership

`tools/sport_telemetry.py` is a pure, bounded Python engine. `Observation` holds
value, original host-monotonic acquisition nanoseconds, maximum age, origin and
availability. `SportEngine.accept(generation, observations, now)` admits atomic
updates for `speed_kmh`, `acceleration`, `gps_position`; rejects invalid, future,
reversed or conflicting acquisitions and immutable-age changes. Polling never
updates an acquisition. Missing/stale data stays null. Equal-date heartbeats
cannot revive withdrawn data or re-arm timers. Source changes retire active runs.

`tools/sport_source.py` supplies a manager-owned 50 Hz polling worker. The phone
manager only copies its small view model or changes sport session/line
settings under an independent lock. Socket reads and parsing stay on that
worker. Sport-only history I/O retains its existing worker path; optional trip
archives route both bounded history writes through one supervised disk child.
Only that child ignores service-group INT/TERM/HUP after atomic spawn-mask
inheritance and parent/account admission; the existing parent owns orderly EOF,
bounded drain/kill/reap and explicit unfinished saves. The acquisition worker
reads actual `CLOCK_MONOTONIC_RAW` on macOS and `CLOCK_MONOTONIC` on Linux,
matching the C++ vehicle clock; Python’s default macOS uptime clock is a
different domain.
No offset or receipt-time substitution is used. No VM/GPU, phone, radio or actuator work is added.

Receiver JSON **`sport_telemetry: true`** opts in. Default is off. With existing
`simulate_vehicle: true`, the worker uses the labelled, independent sport fixture
below, rather than the ordinary sinusoidal gauge demo. With a fan-out config,
add **`consumers.sport`** as a distinct private absolute socket; it must not share
the receiver/backup/Ultra/HUD endpoints. The manager requires that consumer when
fan-out is configured. Without simulation or a consumer, every input is unavailable.
The manager's root process creates state/socket ownership for the display account.

HVO1–HVO4 can supply speed alone. HVO6 adds IMU/GPS. The Python acquisition
adapter validates complete record sets, dates, origin/availability and the three
sport values, plus version, sequence and generation/publication ordering. Other
compound vehicle values are opaque to this scoped consumer; the production C++
cache/codec/receiver remains the authoritative whole-vehicle admission path.
Queued expired acquisitions cannot complete a timer. Rejected packets do not
consume sequence or alter accepted state. Silent sender death expires inputs.
The worker immediately withdraws current values on retirement while retaining
original dates/origin, and retires its own socket; an existing live/dead endpoint is an explicit
start error, never unlinked speculatively.

## Manager/page JSON for native UI integration

The source-only [Trip/Sport/History leaf](NATIVE_HOME.md#trip-sport-and-history-leaf--source-only-14-october)
now consumes this existing model through the same native page owner. It reuses
Home2 grouped rows, segmented controls and software reset/session actions;
native UI's new-piece design review and future generation/phone qualification remain
separate. The engine and source owner are unchanged by that page.

Existing bounded Unix stream request/reply path:

- `{"action":"status"}` adds **`sport`**. Disabled is `{"configured":false}`.
- `{"action":"sport_session"}` ends the current session, retains history and
  begins a new session with cleared timers/laps/peaks. No car state is changed.
- `{"action":"trip_reset"}` resets only the separate software drive statistics,
  exposed under `sport.trip`; sport sessions/timers/laps remain independent.
  [Trip interface and coverage](TRIP_AND_HEADING.md#software-drive-statistics--source-only-14-october-2026).
- `{"action":"sport_line","line":[[48,2.0007],[48,2.0013]]}` sets a finite
  directional GPS line. These are explicit **synthetic fixture coordinates**.
  A real installation supplies its actual start/finish endpoints. Line changes
  abort the current lap and clear its last/best/reference. Endpoints must be
  valid WGS84 below ±85° latitude and 5–200 m apart.

Each sport action returns `{"sport":MODEL}`. Extra fields, malformed endpoints
and actions when disabled are rejected. Do not infer presentation acknowledgement
from these responses or from a drive-mode `sportScreenAvailable` permission.

| Model field | Meaning |
| --- | --- |
| `version`, `configured`, `sessionId`, `sourceGeneration`, `observedNs` | View-model version 1; current owner/session and actual host observation date. |
| `source`, `error`, `rejectedPackets` | `simulation`, `observations` or `unavailable`; acquisition/history errors; bounded packet-rejection counter. |
| `inputs.speed_kmh`, `.acceleration`, `.gps_position` | Each has `value` or null, `acquiredNs`, `maximumAgeNs`, `ageNs`, `origin`, `availability`. Acceleration value is `[longitudinal_mps2,lateral_mps2]`; GPS is `[latitude_deg,longitude_deg,horizontal_accuracy_m]`. |
| `gMeter.longitudinalG`, `.lateralG` | Current calibrated acceleration / 9.80665, null when unavailable. Positive is forward/left. |
| `gMeter.peaks` | Positive/negative longitudinal/lateral peaks and vector magnitude, null until observed in that direction. Historical peaks survive input loss. |
| `gMeter.peakObservations`, `.peakOrigins` | Original date and origin per peak; origins used during this session. |
| `timers` | Fixed keys `0-100`, `50-80`, `0-200`, `quarter-mile`. Each has `phase`, `elapsedSeconds`, `result`, `reason`. Phases: `waiting`, `armed`, `running`, `complete`, `invalid`. |
| `laps` | `line`, `direction`, `phase`, `elapsedSeconds`, `last`, `best`, `deltaSeconds`, `reason`. Positive delta means behind the best lap at the matched track position. |
| `trip` | Separate software speed-estimate journey, coverage and bounded history, with default-off private archives; not provider TripState. |
| `history`, `revision` | Last 64 result/abort/session events, monotonically changing history revision. |

Lap history also retains its original line geometry, even after line changes.
Results carry `sessionId`, `sourceGeneration`, `startNs`, `endNs`, `seconds`,
`origin`, `method` and the continuity limit `maximumGapNs`. Aborts have a reason
and observed host date. Current elapsed values only advance on actual new speed
or GPS acquisitions. A UI repaint cannot advance them or renew freshness.
Completed results and peaks are **historical**, labelled by their source; they do
not imply current readings remain available. All nanosecond dates use the host
monotonic domain, not UTC. Compare historical dates only inside their original
session/source; do not compare across a reboot or present them as wall-clock times.
Render null as unavailable and retain simulation labels.

## Covered acceleration summary — source preparation

This additive model has scoped Mac/Linux Python qualification and remains
**unpublished**. ROOT reviewed the complete source, regression and qualification
helpers before execution; these results do not qualify physical observations. The original current g,
peaks, timers, laps, Trip, history, configuration and ownership paths retain
their contracts. No raw IMU calibration, GTK page, action or provider is added.

`status.sport.gMeter.summary` (and existing sport action replies) adds one
process-memory estimate from already admitted calibrated, gravity-compensated
forward/left acquisitions. It integrates only covered intervals. Its metrics
remain historical when the current input is unavailable. `inputAvailable` is
separate from coverage and uses the existing acceleration freshness predicate.
Original `inputs.acceleration` supplies current availability, age, acquisition
and provenance; neither polling nor retained summary data renews that input.

| Key inside `gMeter.summary` | Type, units and interpretation |
| --- | --- |
| `version`, `authority`, `method`, `persistence` | `1`, `software-acceleration-summary`, `piecewise-linear-acquisition`, `process-memory`. |
| `sessionId`, `sourceGeneration`, `origin` | Existing session and admitted generation (or null); one measured/calculated/simulation origin, or null before an eligible baseline. |
| `maximumGapNs`, `maximumSpanNs` | Existing engine continuity setting (250000000 ns by default), explicitly new for acceleration summaries; summary-only acquisition span cap 86400000000000 ns (24 hours). No old default/maximum changes. |
| `phase`, `reason` | `waiting`, `collecting` or `limited`; bounded diagnostic reason or null. Collecting identifies an endpoint, not current hardware readiness or proven coverage. |
| `inputAvailable` | Boolean current acceleration freshness, independent of retained summary values and phase. |
| `firstAcquiredNs`, `lastAcquiredNs` | Original first/last endpoints contributing covered intervals, host-monotonic integer ns; null before coverage. Polls, singleton recovery and rejected/over-limit intervals do not move them. |
| `coveredSeconds` | Sum of admitted acquisition intervals in seconds; null before coverage. No wall/session/poll-duration extrapolation. |
| `meanLongitudinalG`, `meanLateralG` | Signed time-weighted means over covered intervals; forward/left positive. Finite g or null before coverage. |
| `rmsMagnitudeG` | RMS of the horizontal two-axis acceleration magnitude, finite nonnegative g or null before coverage. No vertical or raw specific-force value is implied. |
| `partial`, `limitReached` | Sticky observed continuity loss within this group; summary-only terminal bound/failure. False partial does not establish complete physical coverage. |

A single sample supplies no interval: all four numeric metrics and contributing
dates remain null. Two actual zero acquisitions over a covered interval produce
zero means/RMS and positive covered duration. Zero never substitutes for absence.
The containing sport version remains 1; disabled sport remains `{configured:false}`.
Dates use the existing host steady-clock domain, not UTC. A consumer must apply
the existing manager request-dispatch freshness and retain simulation labels.
**New actions, control IDs, widgets, pages and GTK changes: none.**

Only successfully admitted fresh changed observations reach this helper. Same
acquisition heartbeats contribute nothing. Consecutive endpoints must have the
same source generation/origin and strictly increasing acquisitions, with an
interval at most `maximumGapNs` and strictly smaller than both original age
limits. The previous endpoint must also still be fresh **at the next delivery**;
this conservative summary-only policy prevents delayed input replacement hiding
expiry. Exact age expiry breaks continuity; exact maximum-gap equality may
contribute when both age limits and delivery freshness allow it. Speed/GPS
updates cannot renew acceleration. No interval crosses observed missing,
stale, disconnected or expired input. Recovery requires a new baseline followed
by a genuinely newer eligible acquisition; original covered values remain dated.

An admitted generation change, including an empty update, clears the summary.
A fresh origin change clears it before binding a new baseline. Neither creates
an archive/history event or mixes the old group into new provenance. Existing
`sport_session` clears summary and establishes the actual reset date as an
admission floor: cached/queued acquisitions at or before that floor do not
contribute, including across provider generations. Current g remains governed
by its original contract, so it can still be available while summary waits.
Existing `trip_reset` and `sport_line` remain independent, apart from ordinary
expiry observed by their existing advance calls.

Known worker failure already withdraws inputs and breaks summary continuity,
retaining labelled historical coverage. Existing normal close resets the sport
session first, so summary clears before withdrawal. Restart/history loading
restores no summary. Silent sender loss establishes age expiry, not an invented
hardware-disconnect receipt. No history, file schema or persistence changes.

The summary acquisition span from its first eligible baseline is capped at
24 hours, including gaps. Equality is allowed; a later endpoint freezes the
summary before changing covered metrics/dates. Covered duration is bounded by
the same cap. No segment is clipped at an invented timestamp. Original current
g and peaks continue accepting valid observations. A new existing sport session,
admitted generation or fresh origin group clears the summary limit. Positive
signed-64-bit host dates are required by this helper; a pure-engine date beyond
that domain limits only the summary, without changing original source admission.

For endpoints u and v in g and acquired interval h seconds, linear interpolation
has signed-axis integral `h*(u_axis+v_axis)/2`. Integrating its squared vector
polynomial gives `h*(|u|² + u·v + |v|²)/3`. The implementation uses the equivalent
nonnegative-square expression `h*(|u|² + |v|² + |u+v|²)/6`, avoiding cancellation
between opposite signs. Means divide accumulated signed integrals by covered
duration; RMS is the square root of accumulated energy divided by that duration.
A trapezoid of squared endpoints would not integrate that linear vector exactly.
This is exact polynomial integration in real arithmetic, with floating-point
rounding; physical motion between acquisitions is an estimate, not measured.

One endpoint, exact integer covered nanoseconds and three fixed Neumaier sums
bound state and work. Integer timestamp differences precede float conversion.
With components bounded by the existing ±100 m/s² admission, signed integrals
remain below 9e5 g·s and energy below 1.8e7 g²·s at the summary cap. Candidate
integrals, coverage and dates commit together only after finite/bounded checks.
A failed numeric check freezes only the summary and preserves prior covered
metrics; it never raises after or reverses original engine admission. Negative
energy is rejected rather than clamped. The declared 1e-12 rounding tolerance
is a finite-bound/analytic-test tolerance, not a certified arbitrary-stream or
physical accuracy claim.

Prepared `sport-acceleration-summary-python` checks include rational polynomial
oracles for unequal spacing, signed reversal, vector rotation and subdivision;
zero/nonzero, tiny intervals/large clocks, atomic invalid batches, exact expiry,
gaps, reset floors, generation/origin changes, limits, copied models and actual
private SportSource socket/failure/close paths. Original sport/Trip/manager tests
stay unchanged. A separate retained comparison helper exercises complete
original/final SportSource and actual Manager status, masking **only this new
summary**, with exact original history bytes and owner-state comparisons. These
are simulated software checks. The original qualified source passes five checks
and 117 Python cases on each of Mac and Debian Linux. The identical feature
source passes seven checks on each platform: 136 Python cases, including 19 new
summary cases, plus the separate complete-owner comparison. All have zero skips.
The comparison retains 628 captures, including 600 new simulated acquisitions;
only the new summary is masked in manager and source models. Original status,
owner state and history bytes compare exactly. No component retry was needed.

The later combined `b6e5b2a` source also passes the nineteen summary cases in
its Mac/GCC scoped sequence. Fresh whole-owner comparisons use immediate
`791a4a2`, retaining 628 records and 600 acquisitions on each platform; only new
`gMeter.summary` is masked, with existing Trip distance and history exact.
[Combined qualification and release limits](HANDOFF.md)
own the integrated scope; no physical or phone evidence follows from it.

Before Linux components, ten actual cleanup diagnostics exercised running
INT/TERM/HUP, mixed repeats, lost create results and outer-driver forwarding.
Own containers were stopped and clients reaped. Two exact nonempty report sets,
original logs, Testing, CMake, JSON and source identities were reread before each
own build/container retirement. ROOT independently verified the retained reports
and fresh absence of all twelve owned Linux containers. Mac retains its existing
GdkPixbuf version warning. Read-only audit assumptions about removed scratch
paths, legitimate JUnit children and the comparison's full mask string were
corrected with original diagnostics retained; no source/helper change or test
rerun followed those auditor corrections.

Evidence remains outside Git in the topic's `mac-base-original-20261015/`,
`root-mac-final-20261015/` and `root-linux-20261015/` directories. ROOT's scoped
receipts identify the original source, actual selected cases and retention.
Running Python in a Clang image is not C++ sanitizer evidence. No C++/native/GPU
build or physical g-meter/IMU/phone/fps claim follows from these checks. Release
adoption needs final source review and a new coherent complete matrix/helper/
input/config/unit/assets/capacity check and immutable generation owned solely by
phone-session.

## Auto rules and numerical method

Launch timers arm after **one second** of continuous acquired speed ≤0.5 km/h.
The first subsequent rising measurement above that rest band starts 0–100,
0–200 and quarter-mile together at the last acquired rest sample. There is no
rollout subtraction or invented exact zero crossing. The 50–80 timer arms below
or at 48 km/h, starts on a rising crossing of 50, and stops on a rising crossing
of 80. It cannot retrigger around 50 until it returns to the arming band.
Target crossings use linear interpolation between original acquisitions.
Quarter-mile integrates trapezoidal vehicle speed in m/s to **402.336 m** and
solves the final segment's constant-acceleration distance crossing. Its elapsed
result therefore depends on qualified vehicle speed and wheel/slip calibration;
it is not an independently measured GPS distance.

Active speed runs abort on missing/stale speed, source/provenance change,
an acquisition interval exceeding the smaller of **250 ms** or the input age
limits, return to rest, or **120 seconds**. A fresh IMU/GPS update cannot renew
speed. A new attempt requires acquired speed meeting its arming rule again.
Results are estimates at the acquired sample resolution; this is not a claim of
certified timing precision or a manufactured physical timing receipt.

Laps use GPS only: supplied horizontal accuracy ≤**5 m**, acquisition intervals
≤the smaller of **500 ms** or signal age, same origin and no jump faster than
90 m/s plus both position accuracy bounds. Intersect consecutive positions with
the finite A→B line; only negative-to-positive cross-product direction counts.
Crossing time is interpolated between those original fixes. The first crossing
starts a lap; subsequent crossings need a **20 m** departure from the line and
at least **10 seconds** elapsed. Reversed crossings, crossings outside the
finite line and line chatter do not complete a lap. Gaps/poor fixes abort the
current lap; a fresh crossing starts a new one.

A best lap retains at most **2048** GPS points. Delta projects the current fix
onto that reference trace within 10 m plus supplied accuracy, with bounded
forward reference progress to disambiguate a closed start/finish or intersecting
track. Off-reference, lost continuity or an overflowing reference returns null,
not a fabricated delta. Source-generation/provenance changes clear the current
best reference; dated historical results remain in history.

## Session history and release graph

`stateDirectory/sport-history.json` is an owned 0600, single-link bounded file
(≤64 KiB). A worker atomically replaces and syncs file/directory state after
history/line changes. Reads reject links, special files, corruption, duplicate
fields, nonfinite results and invalid historical dates/results. A restart keeps
history/line, allocates a new session and restores no live input, active timer,
lap or peak. History write failures are visible in the model and do not create
measurement freshness. New events evict oldest entries after the 64-event limit.

Default-off `trip_history: true` adds bounded historical software journey storage
through one supervised disk child. Under this opt-in the same child runs the
existing sport writer helper; sport session records and their schema stay separate. It
requires sport telemetry and seals `trip_history.py` only through the hash-bound
receiver config. Restored trip archives cannot seed live state; new disk I/O
never blocks acquisition. [Trip archive contract and UI data](TRIP_AND_HEADING.md#private-software-journey-archives--source-only-14-october-2026)
own exact scope, failure/retirement, limits and qualification.

Only an enabled managed release seals `sport_source.py` and `sport_telemetry.py`.
The optional inventory is derived from hash-bound receiver configuration;
disabled and retained generations keep their existing inventory. Publish a new
candidate and a newly sealed helper with the matching admission code if adopting
the feature. Never copy these modules/config into a sealed generation. Existing
signed Apple resources and provider/Climate/settings declarations stay unchanged.

## Simulators and component checks

`headunit::sport_simulation_sample(started, now)` is a dated HVO6 fixture:
two seconds at rest, 20 km/h per second to 220, then rest at 18 seconds in a
40-second cycle; forward acceleration and a 20-second GPS circle are simulated.
Every actual synthetic acquisition is labelled `simulation`. The ordinary
`SimulatedVehicle`/HUV2 gauge behavior stays unchanged.

```sh
cmake --build build --target headunit-sport-observation-tests headunit-mcu-vehicle-simulator
ctest --test-dir build -R '^(sport-observation|mcu-sport-simulator|sport-telemetry-python|vehicle-observation-wire)$' --output-on-failure
python3 tests/sport_telemetry_test.py -v
build/headunit-mcu-vehicle-simulator --sport-self-test
```

The MCU self-test sends 501 acquisitions through real private AF_UNIX sockets
and the production C++ receiver, then verifies exact silence expiry/retirement.
It is post-mapping simulation, not MCU firmware or a clock mapper. Its realtime
`--sport-socket PRIVATE_SOCKET` variant runs for 25 seconds. On Linux the existing
vehicle simulator also supports `SOCKET SECONDS --sport` for a longer HVO6 run;
its legacy fixture option stays unchanged. Never point a fixture at a live input
without the exclusive bench owner's explicit integration plan.

Python checks cover analytic 0–100/50–80/0–200 and quarter-mile results, peaks,
exact expiry, gaps/restart/provenance retirement, atomic conflicts/age policies,
rest/rearm hysteresis, lap last/best/delta, closed-loop reference ambiguity,
finite-line/direction/chatter/accuracy guards, bounded history, persistence,
manager action/frequent-status validation and actual socket ownership/expiry/cleanup.
C++ checks cover typed parsing/round-trip, all older encoders refusing lossy
downgrade, independent expiry, replay/conflict/immutable policy and withdrawal.
[CI](CI.md) defines the complete portable/Linux/Skia matrix. Private dated
receipts: `~/.cache/headunit-qualification/20261013/codex-sport-telemetry/`.

## Qualification receipt — 14 October (13 October task cache)

Software feature inputs are `b8915b2`, with reviewed upstream test-only CPU-budget
repair `887db18` (`b32c966`); all **726** final non-Markdown inputs match the
corrected Linux snapshots. Hardware proposal is `19d68f5` on its matching branch.

| Check | Result |
| --- | --- |
| Pinned clang-format 19.1.7 | PASS |
| macOS portable matrix | 159/159 PASS; final Python 33/33 and 5/5 focused CTest follow-up cover later Python refinements and the CPU fixture. Production C++ inputs unchanged. |
| Linux GCC Debug, video ON | 187/187 PASS |
| Clang 19 ASan+UBSan Debug, video ON | 187/187 PASS |
| Full Skia GCC RelWithDebInfo, video/GPU ON | 188/188 nonhardware PASS |
| Full Skia ASan RelWithDebInfo, video/GPU ON | 188/188 nonhardware PASS |

The serial Linux runs retain stock compiler/warning/leak/CMake timeout policies.
Low-space qualification used private host-backed build storage, actual native
builder-owned source/home and executable 512 MiB Linux `/tmp` tmpfs. Earlier
lanes use `-j1`; final full ASan uses `-j2` through the shared HOST build slot at
its default 15 GiB floor. After each completed final lane, commands, logs,
CTest/source/cache/artifact receipts remain; completed build trees/containers
are retired. Shared images/Skia and other workers' inputs remain untouched.
This is evidence across the matrix and scoped follow-ups, not one unchanged
six-job invocation. The macOS follow-up changes only Python and the upstream
fixture relative to its full matrix.

An isolated realtime **Mac C++ HVO6 → Python** run delivers 501 simulation
acquisitions, rejects zero packets, reports 5.0/1.5/10.0 seconds for the three
speed timers and 12.08368 seconds for the capped-speed quarter-mile fixture.
Stopping the sender expires original inputs without redating; consumer teardown
removes its endpoint. An actual Debian root-manager/display-account probe
preserves UID 1000/0600 history, retires its socket and restores no live inputs.
These are real process/socket tests with **simulated values**, not real car or
GPU/phone/IMU/GNSS acceptance.

Failed and superseded attempts remain in the private cache, including early
wire/inventory repairs, clock-domain rejection, filesystem ownership/noexec/font
cache problems, loaded-run release timeouts and the obsolete wall-time budget
fixture. A compiler-switch CMake cache reset produced 184 passing cases with
video/Debug options dropped; **that run is excluded**, and the correctly
configured 187-case GCC run passes. No timeout/leak/source guard was relaxed or
required failure waived. `qualification.json`, source/input hashes and all scoped
logs under the dated cache own exact evidence; no live CM5 state was changed.

## Exact bench follow-up (phone-session owner only)

1. Merge the pushed software branch, resolve shared manager/fan-out/release
   changes with the other workers, and run the integrated required matrix.
   Keep the current signed pack. Prepare a **new** generation and helper using
   normal release recipe/preflight/publication. In that candidate's receiver
   input set `sport_telemetry: true`. Keep `simulate_vehicle: true` for the
   independent labelled sport fixture, or use an explicitly isolated/new fan-out
   source with a distinct `consumers.sport` endpoint and HVO6 for all consumers.
   Use the current phone-session source/recipe, not this older branch's baseline.
2. After the owner selects that new generation, use the existing manager socket
   to set the fixture line with the sport_line JSON above. Start a new session
   with sport_session. Poll status at 5 Hz for **65 seconds**, retaining dated
   JSON under the qualification cache. Require simulation-labelled g/current
   peaks, complete 0–100/50–80/0–200/quarter-mile results, GPS laps near 20 seconds
   and coherent last/best/delta. Reject any invalid result or unexplained worker
   error; timing is simulation evidence only. Compare acquisitions, not repaint
   counts. The model's input speed intentionally follows the dedicated sport
   fixture rather than the ordinary gauge-demo speed.
3. For a source-only isolated socket exercise, run the MCU `--sport-socket`
   variant against a temporary private sport consumer, then stop sending and
   verify current speed/g/GPS becomes null at exact original age. Repeat with
   missing values, a stale heartbeat, one rejected replay and a newer source
   generation: active timers/laps must abort and require a newly acquired arm/
   line crossing. Do not inject these packets into the currently live car input.
4. On the connected candidate observe both panels/streams/UI-Sync, main touch,
   Home/Back, Climate/settings and unchanged owner restart counts during the
   bounded sport-status poll. native UI's later GTK integration has its own visual
   acceptance. No new signed-scene or native cluster g-meter is supplied here.
5. Restart the manager only when the exclusive owner schedules it; verify
   historical results/line remain, a new session starts and live readings/runs
   are not restored. Physical speed/IMU/GNSS calibration, outage and timing
   uncertainty tests await the proposed sensor/provider module.

For the owner, after merging and qualifying its integrated tree, use a **new
private recipe** (`SPORT_RECIPE`) copied from the current candidate with a new
ID/source/helper binding and the sport opt-in. `SPORT_RELEASE`/`SPORT_DIGEST`
are the actual stage output. These are future owner commands, not actions
performed by this source-only worker:

```sh
git merge --no-ff development branch
sudo python3 -B tools/install_appliance_release.py --recipe "$SPORT_RECIPE" --stage-only
sudo python3 -B tools/install_appliance_release.py --existing-release "$SPORT_RELEASE" \
  --manifest-sha256 "$SPORT_DIGEST" --install --migrate-boot-helper
```

For the exact 65-second model check, set `SPORT_MANAGER_SOCKET` to the current
manager endpoint and `SPORT_EVIDENCE` to a new private JSONL file under the dated
qualification cache. Run as the display account after selection. The collector
sets only the synthetic line/new sport session, then retains only sport JSON:

```sh
python3 - "$SPORT_MANAGER_SOCKET" "$SPORT_EVIDENCE" <<'PY'
import json, os, socket, sys, time

def request(value):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(3)
        connection.connect(sys.argv[1])
        connection.sendall(json.dumps(value).encode() + b'\n')
        with connection.makefile('rb') as stream:
            raw = stream.readline(131073)
        if len(raw) > 131072 or not raw.endswith(b'\n'):
            raise RuntimeError('Invalid bounded manager reply')
        reply = json.loads(raw)
        if not reply.get('ok'):
            raise RuntimeError(reply.get('error', 'Manager rejected sport request'))
        return reply['sport']

request({'action': 'sport_line', 'line': [[48, 2.0007], [48, 2.0013]]})
request({'action': 'sport_session'})
fd = os.open(sys.argv[2], os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w') as output:
    deadline = time.monotonic() + 65
    while time.monotonic() < deadline:
        model = request({'action': 'status'})
        if model.get('error'):
            raise RuntimeError(model['error'])
        output.write(json.dumps(model, allow_nan=False) + '\n')
        output.flush()
        time.sleep(.2)
PY
```

The companion hardware proposal
selects BMI088/MAX-M10S as research candidates and owns wiring/calibration;
CAN-FD session framing, physical clock mapping and sourcing remain proposals.

## Fixed 100–200 km/h rolling estimate — source only, 15 October 2026

`status.sport.rolling100To200` is an additive, live process-memory estimate in
`SportEngine`. It consumes only original, fresh, changed speed observations
following the existing atomic admission. It has one previous endpoint, one
current attempt and one last completed result. It adds no provider, config,
action, control ID, GTK widget/page/style, history event or persistence field.
native UI owns later presentation using existing components. This software model
is still unpublished; no physical GPS, speed/odometer calibration, certified
track timing, public-road use or phone acceptance is established.

The four existing `timers`, their run dictionary, arming, 64-event history,
16-entry trip history and archive bytes retain their contracts. This model is
separate from the pending acceleration-summary and trip recent-pace additions;
its source qualification starts at V87 `3670763`, without adopting those fields.

A genuinely admitted speed **≤98 km/h** arms the fixed **100→200 km/h** attempt.
The next ascending bracket `before.speed < 100 ≤ after.speed` starts it; an
ascending bracket at 200 completes it. Linear interpolation uses each original
speed/acquisition pair. Rounded relative integer-nanosecond offsets are added
to original dates, preserving large absolute host dates. Both crossings may
come from one interval. Equal derived start/end dates produce **no result** and
`Insufficient acquisition resolution`; no positive duration is invented. Speeds
above 98 cannot rearm through jitter around 100. An admitted return to ≤98
retires the current attempt and arms a new one, including a real stopped zero.

Continuity uses the existing maximum speed gap (default **250 ms**) and immutable
input ages. An interval must be strictly shorter than both age policies; the
previous endpoint must still be fresh at admission. Missing, exact expiry,
withdrawal, a gap, generation/origin change and the existing **120-second** run
lifetime retire continuity. Rejected invalid/replay/future/conflicting packets
change no owner state, elapsed time or result. Other inputs and repeated
polls/heartbeats cannot start/finish or advance elapsed time. A later valid pair
must independently meet these admission/continuity rules.

The rolling acquisition cutoff survives generation/origin changes: an older
successor baseline permitted by the original pure engine cannot rearm this
model. `sport_session` clears the rolling current/last result and requires a
new rearm acquisition strictly after the reset action date. `trip_reset` leaves
it independent. Terminal SportSource failure/close retires the rolling model
for that owner lifetime; late samples or session requests cannot reopen it.
A new owner restores no rolling data. Existing source/session/trip semantics
remain unchanged.

| Field under `sport.rolling100To200` | Meaning |
| --- | --- |
| `version`, `authority`, `method` | `1`, `software-speed-estimate`, `linear-speed-interpolation` |
| `startSpeedKmh`, `endSpeedKmh`, `rearmSpeedKmh` | Fixed 100, 200, 98 km/h |
| `maximumGapNs`, `maximumRunNs` | Existing owner speed continuity and 120-second lifetime policies |
| `phase`, `reason` | `waiting`, `armed`, `running`, `complete`, `invalid` or terminal `retired`; reason explains unavailable/invalid state |
| `elapsedSeconds` | Acquired elapsed while running, result duration while complete; null after continuity loss or rearm, never advanced by polling |
| `currentRun` | Null or `{sessionId,sourceGeneration,origin,startCrossing,lastAcquiredNs}`; last date is an original accepted acquisition |
| `lastResult` | Null or `{sessionId,sourceGeneration,origin,startCrossing,endCrossing,seconds}`; positive duration only, independent of current availability |
| Each crossing | `{derivedNs,before,after}`; the interpolated crossing date is **derived**, not a raw acquisition |
| Each `before` / `after` endpoint | `{speedKmh,acquiredNs,maximumAgeNs,origin}` from the original admitted speed observation |

`invalid` stays invalid until a newly admitted rearm; `reason` may become current
unavailability when speed expires. `waiting` has no current attempt; `complete`
is a historical result while inputs may subsequently expire. `lastResult`
survives ordinary gaps, source/provenance changes and terminal input withdrawal
with its original session/generation/origin and crossing brackets. Explicit
session reset (including existing close behavior) clears it. Current reading
availability comes from `sport.inputs.speed_kmh`; a retained result never renews
that reading. Dates use the host monotonic acquisition domain, never UTC or a
phone clock; comparisons across reboot are unsupported. Render null as
unavailable, and preserve estimate/simulation/source labels.

New native UI UI pieces, actions, control IDs, providers and configuration: **none**.
Whole-source ROOT review, the coherent release matrix, exact helper/module/role/
config/argv/unit/private-input/assets validation, native UI integration and a fresh
full capacity reserve precede a new immutable generation. The sole phone-session
owner retains all bench access. No C++ caller compile qualifies GPU rendering.
