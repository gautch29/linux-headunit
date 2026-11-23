<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Ultra rendering measurements on CM5

## Accepted input receipt facts — 14 October, source only

The `video-receipt-identity` candidate starts from qualified integration `d544236`
and adopts only accepted report-identity source `4dfbf8d` (own dependency
`1106847`). Selection and qualification of an appliance generation belong to
phone-session; this source worker performs no bench action.

`DisplayVideoInput::complete(token, presented, receipt)` carries an optional
copied `InputPresentationReceipt`. Legacy consumers forward to their existing
bool completion. GpuDisplay constructs it only in the existing accepted scene
branch after `PresentationClock::accept`; discard/invalid epoch has no mapped
record. The original Stream token remains the separate completion argument.
The record preserves submission/current MediaClock suspension epochs at acceptance,
the original submit host sample, full Wayland `PresentationFeedback` (original
submission token, clock ID, seconds/nanoseconds, compositor sequence, refresh,
flags, actual clock correlation and poll sample), and accepted `PresentationTime`
(mapped host monotonic/media NTP/PTS/uncertainty). No timestamp is synthesized or
replaced by completion callback time, fence completion, redraw or file date.
`received_ns` is the original **poll** observation after correlation sampling;
it is not the compositor event-dispatch date or actual presentation timestamp.

NativeInput retains one numeric `NativeInputPresentation`: exact accepted
`stream_token`, adopted owner generation/optional authenticated connection ID,
source PTS, optional decoded metadata and the copied receipt. Stream's
`current_token()` and `current_native_metadata()` select the accepted frame,
independently of newer prepared compositions; the ordinary `native()` compositor
accessor deliberately retains its existing newest-prepared behavior. Native
metadata's sender NTP and derived sender timestamp are copied as originally
known, never host acquisition or global sender-report identity. CPU frames have
no decoded-source metadata. No native buffer, image or decoder lease is retained
by the diagnostic. The accessor copies under the existing ingress lock; outer
report identity/counters remain separate observations, not an atomic whole-window
transport history. Suspension/flush/source retirement/replacement/terminal closure
makes the accessor null; late invalidated tokens cannot label a successor.
A receipt token is consumed once, with ordered Stream completion and explicit
submission replay/epoch consistency guards. If the existing discarded-commit
policy transfers a staged source to the next scene drawing that exact token,
only that successor's genuine accepted commit is recorded; no presentation or
first-submission date is invented for the discarded attempt. Repeated reports of a retained token
keep its original date and do not count it as newly presented.

The existing default-off five-second finite diagnostic adds one compact JSON
field `videoPresentation`, object or `null`. It is encoded only by the existing
report owner, with no per-frame file/JSON output, collector, service or thread.
Integer identities/dates use exact decimal **strings**; numbers below are small
clock/geometry fields or existing double PTS. The object fields are:

| Fields | Meaning |
| --- | --- |
| `authority`, `hostClock` | `gpu-display-native-input`, `CLOCK_MONOTONIC`; accepted mapping provenance on Linux. |
| `streamToken`, `ownerGeneration`, `ownerConnectionId`, `sourcePtsSeconds` | Exact accepted current source and its adopted transport scope; ID zero is valid, absent ID null. Join with the outer original report owner/publisher/connection/panel and original host boot. |
| `submissionToken`, `submissionEpoch`, `currentEpoch`, `submittedMonoNs` | Original Wayland commit identity, submitted/active media suspension epochs **at acceptance**, and original submission host sample; not the report's current epoch. |
| `presentedMonoNs`, `pollMonoNs`, `mappedMediaNtp`, `mappedPtsSeconds`, `uncertaintyNs` | Actual accepted PresentationClock mapping and later poll observation, kept separately. |
| `feedbackClockId`, `feedbackSeconds`, `feedbackNanoseconds`, `feedbackSequence`, `refreshNs`, `flags` | Original compositor presentation domain/timestamp/sequence/refresh/flags; sequence is not a phone frame ID and flags are not optical proof. |
| `correlation` | Original mapping sample: `clockId`, `sourceNs`, `beforeMonoNs`, `afterMonoNs`. Native monotonic feedback keeps the single real identical source/before/after sample; RAW keeps actual bounded samples. |
| `decodedSource` | Optional exact accepted native metadata: `senderNtp`, `senderTimestampNs`, `width`, `height`; null for CPU or unknown source. |
| `senderClockMapping`, `phoneAck`, `optical` | Always null; trusted sender-report clock/stream join, acknowledgement semantics and optical evidence remain unavailable. |

`frameCorrelation` stays null: this is a latest accepted source/commit snapshot,
not an enumerated ingress/admission/staging history or complete sender join.
The accepted `e003` offline audit ignores the new additive field and remains
conservative; no audit source is imported. A five-second report can miss most
frames or span replacement and cannot supply per-frame percentiles. A source
token is neither distinct-pixel nor optical proof. No queue, deadline, reserve,
clock, ACK, GPU import/ownership or touch-response policy changes accompany it.
GTK UI keys/actions/IDs/new pieces for native UI integration: **none**.

### Focused component qualification

Final focused checks pass on coherent runtime/test bytes: macOS **7/7** (3.59s)
for production scheduling with explicit fake receipts, report encoding, both
clock owners, video ownership and camera callers; Debian ARM64 Clang19
ASan+UBSan/full-Skia **8/8** (1.46s), adding actual NativeInput/Stream generation
and two-prepared-source regression, plus the actual `live_ui.cpp` caller compile.
The dependency image lacks Weston, so the planned ninth test was not registered
in that lane; this is explicitly eight component tests. A separate unchanged-code
lane installs Weston **14.0.2** only in its owned container and passes production
Wayland shm/presentation mapping **1/1** (2.37s) with real isolated software
compositor feedback, once-only/replay rejection and original absolute timeout.
The synthetic scheduling fixture preserves distinct raw/mapped/poll values and
checks shared-source discard transfer, redraw, old epoch and replay. NativeInput
checks exact current metadata despite a newer prepared composition, full uint64
source values, generation replacement/late old tokens, flush/suspension, legacy
unknown receipt and actual admission failure. Stock warnings, clock/queue/deadline
policy and existing external leak suppressions remain unchanged; shared Skia is
uninstrumented. No GCC/full matrix, physical GPU/phone/sender, optical or fresh60
qualification is inferred.

Pinned format19.1.7 and whitespace checks pass. All **878** tracked inputs are
retained per lane; final non-Markdown bytes match all three compiled snapshots.
Independent SHA checks verify source, logs, CMake/Testing metadata and JSON before
retirement. Six owned build trees and five owned containers are absent, including
actual Docker INT/TERM/HUP cleanup exits 130/143/129. The initial proof waiter was
cancelled before any tree/container creation while the manager held the slot;
its failure transcript/receipt remains retained. Shared/private/selected/in-progress
inputs and other workers are untouched. Exact source, logs, tests and retirement
are private under `20261014/codex-video-receipt-identity/`; FINAL coordination owns
ready commit and source identity. Manager whole-branch review and a new complete
coherent required matrix remain necessary before any release.

### Phone-session owner recipe

After whole-branch review, a new complete coherent required matrix and a new
immutable V86+ generation, preserve all existing private/signed inputs and queue,
poll/fence/response settings. Enable only existing `diagnostics=true`,
`diagnosticReports=30`. Retain original private lines off-device with exact
source/release/helper/boot/invocation/publisher/connection and loss bounds.
On a known moving-main/advancing-cluster workload, inspect each panel's original
predecessor chain and object/null fields. Require original Stream/submission
identities, generation/connection scope and genuine correlation; compare raw
compositor timestamp, accepted monotonic mapping and separate poll date without
clamping or reconstructing missing stages. Idle/repeated tokens must preserve
original dates; budget completion stops reporting. Missing lines remain gaps.

Existing Home/Back withdrawal must yield null until a fresh current source receipt;
real Disconnect/reconnect has a fresh report/publisher chain. Qualify replacement,
old receipts and failures in isolated software fixtures before any deliberate live
fault; never use an old connection/epoch/token to fill a successor report. Keep
both panels/streams/UI-Sync/audio/control/main/touch/Climate/native fallback alive,
and retain independent original PCM progress/underrun and phone-error evidence.
Map sender logs only after genuine boot/stream/source-clock/ingress identity is
provided. Full-resolution fresh60, phone-timeout cause, audio continuity and
physical/optical presentation remain open; no throughput repair is claimed.

## Producer report identity — 14 October, source only

The `frame-observation` candidate extends the existing `InstalledUi` numeric
report owner. It is based on documentation `37fb44b`; selected V84 runtime
`3bd3db9` is unchanged. Existing `diagnostics=true` and
`diagnosticReports=1..120` remain the only opt-in. Reports keep their five-second
minimum spacing, five-scene admission gate and finite per-panel/session budget.
There is no per-frame output, new collector/thread/process, PCM import, queue,
deadline, reserve, ACK or GPU policy change.

Each `Ultra display diagnostic` line now carries actual producer facts:

| Key | Meaning |
| --- | --- |
| `reportSequence` | Per-panel ordinal assigned by the reporting owner, beginning at 1. It counts reports, never frames. |
| `previousReportSequence`, `previousReportMonoNs` | Exact last produced same-panel report ordinal/date; both `null` on first report. Missing extracted lines remain detectable from the predecessor, including unchanged-counter intervals. |
| `reportOwnerStartedMonoNs` | Actual construction date of this renderer's shared output/report owner. It is a local owner boundary, not boot time, a source acquisition, or a globally unique identity. |
| `reportClock` | `host-steady`, matching existing `mono_ns`; Linux uses the host monotonic domain. No phone/wall-clock mapping is inferred. |
| `publisherNoncePrivate`, `publisherConnectionGeneration` | Directly supplied receiver launch nonce and actual control-connection generation also supplied to the catalogue owner. A missing/invalid nonce leaves both `null`; standalone catalogue fallback nonces are never reconstructed. Keep these private. |
| `panel`, `outputRole` | Existing configured panel index and `main`/`cluster` role; no physical connector, scanout or visible-content claim. |
| `videoIdentityObservedMonoNs` | Separate actual host steady sample taken under the existing ingress lock. Identity observation can follow the report date; it does not date source acquisition or atomically sample report counters. |
| `videoOwnerGeneration`, `videoOwnerConnectionId` | Internal generation and optional session-reserved authenticated transport ID adopted by the display owner. ID zero is valid. Adoption alone does not prove a configured decoder or fresh frame. |
| `videoIngressGeneration`, `videoIngressConnectionId` | Current ingress generation/optional authenticated ID, copied together under its lock. Retirement keeps the last ID as a tombstone; availability must be checked. |
| `videoAvailable`, `videoClosed`, `videoOwnerCurrent` | Ingress available/nonterminal, terminal closure, and available ingress matching the adopted owner generation. A replacement awaiting adoption or retired/closed owner cannot be reported current. |
| `videoGenerationChangesWindow`, `videoGenerationDroppedWindow` | Existing owner adoption and decoded old-generation rejection counters differenced over the exact report predecessor. First report is cumulative. These do not enumerate transport events occurring between owner polls. |
| `frameCorrelation` | Always `null`: no authenticated per-frame ingress/admission/staging/submission/accepted-receipt join is exported. |

Without a native input, all video identity fields are `null`. The renderer's
actual publisher tuple plus owner construction date and panel bound a report
series; do not join different host boots or unavailable publishers by construction
date alone. Compare predecessor date/sequence with original captured lines before
using interval counts/rates. A capture may lose lines after the producer advances:
this is an explicit missing-report boundary, never a new five-second window.
An idle redraw can produce another diagnostic report without a fresh video token.
`videoPresented` still counts exact accepted source tokens, not independently
verified distinct picture contents. Report counters may span video replacement;
identity is a snapshot, not a claim that every counter in the interval belongs
to its final transport ID. Any transition window requires scoped review.

### Component evidence and retirement

The production report-series regression uses explicit synthetic steady dates to
check first-report nulls, independent panels, delayed reporting, exact
predecessors, unchanged-counter reports, finite budget and a successor with no
old predecessor. The existing production `NativeInput` state fixture exercises
real queue begin/retire/adoption, valid ID zero, stale receipts after replacement,
script flush, suspension, terminal admission failure and closure. Its native
frames/media clock and accepted flags are explicit fixtures, not decoder/GPU or
phone performance. Final macOS production-series/serializer CTest passes **1/1**
(0.23s); Debian ARM64 Clang19 ASan+UBSan passes the report, native-input state and
display-scheduling CTests **3/3** (1.38s), with stock strict warnings and existing
external leak suppressions. The actual Skia-enabled `live_ui.cpp` caller and
native-input implementation compile; shared Skia is uninstrumented. No GCC,
full matrix, real GPU/decoder, signed bundle, CM5 or phone test is claimed.
Pinned format19.1.7 and whitespace checks pass. Preliminary checks are retained
separately; final runtime/test/CMake inputs match both final lane snapshots.
Logs, Testing, source and JSON hashes are verified outside all four owned build
trees before removal. Both Linux build containers and three actual Docker
INT/TERM/HUP cleanup fixtures are retired; signal fixtures return 130/143/129.
Shared dependency/Skia caches and other workers are untouched. Private receipts
belong under `20261014/codex-frame-observation/`; final coordination lists exact
source and retirement identities.

### Explicit park and sole-owner bench check

Full per-frame/sender causality, fresh60 and timeout-free acceptance remain
**parked/open**. The successor [accepted input receipt interface](#accepted-input-receipt-facts--14-october-source-only)
now copies the existing mapped receipt and current source facts. Decoded native
metadata still lacks a complete authenticated ingress/admission identity, and
no bounded report joins all earlier stages to original sender observations. This milestone does not add
an incomplete per-frame logger or reconstruct missing dates from sample order,
mtime, bitrate or decoder completion. The accepted offline audit remains
conservative until separately reviewed support consumes these new fields.

After whole-diff manager review, a complete coherent required matrix and a new
immutable receiver generation, phone-session alone can use existing
`diagnostics=true`, `diagnosticReports=30` for a known moving-main/advancing-cluster
workload and continuous music Off30/CameraOn60/Off30. Capture private original
lines from session start off-device, retain source/release/input/boot/invocation
identities and unchanged response/poll/fence/queue policy. Verify each panel's
first null predecessor, exact subsequent predecessor chain and directly embedded
publisher tuple against the original catalogue tuple. Split missing lines,
owner/clock changes and transition windows; never fill them. Verify ordinary
Home/Back preserves the report owner while real Disconnect/reconnect creates a
fresh series/publisher. Use isolated component fixtures for late old receipt and
video generation replacement, rather than injecting transport faults into the
live session. Preserve both panels/streams/UI-Sync/audio/control/main touch/native
fallback/Climate/settings. No report identity alone proves recovery, content,
optical presentation, receiver cause or 60fps. UI keys/actions/IDs/new pieces for
native UI: **none**; only private numeric diagnostics and the C++ identity accessor.

## V84 settled moving-source measurement — 14 October

Frozen `3bd3db9`, publisher-bound MapsMedia/Music, eight-second settle then 60s:
116 original both-Ready polls and 11 whole 5s windows per panel. Actual fresh means
main 18.434fps (17.395–19.992), cluster 31.628fps (30.717–32.400); decoded 1036/1774,
superseded 20/32, no reported clock drops/backpressure. Initial/final pixels show
Music content/progress and labelled simulated instruments. Compose window means
2.988/8.672ms; since-start maxima 11.507/50.678ms. No p95 samples are available.
Phone capture 46,606,047 bytes contains 3 actual frame-POC timeout messages.

Fresh 60/timeout-free acceptance remains open. The separate camera workload was
idle/static (main 0–0.26/cluster 0 fresh fps, no ongoing decoder admissions); its
zero timeout notices are not a repair or moving-throughput pass. Ten reconnect
Ready receipts with early blank/logo pixels are not ten visible-video returns.
No unproven ACK, source-clock, queue, deadline or GPU change is adopted. Missing
causal observations remain as described in the retained V83 diagnosis. Private
original windows/screens/capture: `20261013/phone-session/v84-moving-summary.json`.

## Strict producer-chain audit — 14 October, source only

The optional `--strict-producer-chain` mode extends accepted offline tooling
`6f3763c`. Its producer dependency is the exact 19-field `frame-observation`
contract in manager-accepted `4dfbf8d` (14 October review).
This branch imports no runtime code. Selected V84 `3bd3db9` and its original
reports remain legacy evidence; enabling strict mode cannot supply missing
fields. No new bench evidence or deployment is claimed. UI keys/actions/IDs/new
pieces for native UI integration: **none**.

```sh
python3 -B tools/video_attribution.py --strict-producer-chain \
  /private/evidence/new-generation-manifest.json > /private/evidence/strict-video-audit.json
```

The private schema-1 manifest is unchanged. Strict mode requires non-null
`clock_domain` and `owner_scope`: anonymous labels backed by the owner's actual
host/clock/invocation evidence. These labels are trusted operator declarations,
not measured or invented boot identities. Never compare reports and publisher
samples from different hosts or owners merely because dates or nonce strings
look alike. Each capture is independent, even when its labels repeat.

The strict parser consumes these producer fields in addition to legacy counters:

| Fields | Strict use |
| --- | --- |
| `reportSequence`, `previousReportSequence`, `previousReportMonoNs` | Per-panel sequence 1..120. First predecessor fields must both be literal `null`; later reports must name sequence minus one and an earlier original host date. Join only when that exact report/date is present. |
| `reportOwnerStartedMonoNs`, `reportClock` | Exact local owner construction date no later than report time; `host-steady` only. Both panels in one capture must share the owner and directly supplied publisher. Construction time is not boot time or global uniqueness. |
| `publisherNoncePrivate`, `publisherConnectionGeneration` | Direct supplied 32-lowercase-hex launch nonce and exact uint64 control generation, including zero. Require a complete non-null tuple and exact equality to original sampled cluster publisher in the same trusted declared scope. It is producer-reported/local-launch binding, **not cryptographic attestation**. Nonces never appear in output/errors. |
| `outputRole`, existing `panel` | Configured `main`/0 or `cluster`/1; reject role/panel disagreement. This is not connector/optical identity. |
| `videoIdentityObservedMonoNs`, `videoOwnerGeneration`, `videoIngressGeneration`, `videoOwnerConnectionId`, `videoIngressConnectionId`, `videoAvailable`, `videoClosed`, `videoOwnerCurrent` | Separate original dated snapshot, with adopted owner and current ingress kept distinct. Connection ID zero is valid; optional IDs remain null. Flags must satisfy the producer current-owner predicate. All snapshot fields may be null for absent input; absent video counters stay unavailable. |
| `videoGenerationChangesWindow`, `videoGenerationDroppedWindow` | Original adoption/old-decoded rejection counts; required with a non-null identity snapshot, absent/null when no input. These do not enumerate all ingress transitions. |
| `frameCorrelation` | Must remain literal `null`; no existing complete per-frame correlation is exported. |

Success retains original arithmetic and adds `strict_producer_chain=true` and
`producer_chains`: per-capture/panel report count, owner construction date,
first/last actual sequences, `complete_from_first_report` and `rejected_joins`.
That completeness means only the retained first-through-last report chain; an
unobserved tail cannot be certified. Every accepted window fills
`producer_window_start_mono_ns` from the actual predecessor and adds
`producer_chain.{sequence,previous_sequence,owner_started_mono_ns,clock,
configured_output_role,sample_publisher_matches_in_declared_scope}`.
The attribution labels explicitly distinguish producer-reported tuple matching
from attestation and exact producer predecessor from extract adjacency.

Each window's `video_identity_observations` holds the exact eight raw numeric/
nullable snapshot fields at `before` and `current`, plus the two original
`generation_window_counts`. Snapshot observation can follow report/counter time;
it is not source acquisition or an atomic sample of all window counters. Never
assign every frame/window to the final ID, including when endpoint IDs match
and adoption counts are zero. `entire_window_transport_identity` stays null.
`authenticated_transport_generation` and the complete frame ingress/admission/
submission/accepted-receipt chain, sender/host clock mapping and timeout cause
remain null. No fresh60, content uniqueness, causal, optical or recovery claim.

Missing initial reports, skipped reports (even unchanged zero counters), wrong
predecessor dates, missing panels and absent video counters make the audit
`incomplete`/exit 2. A bad join contributes no frames or duration; valid later pairs remain separately
usable. All chain gaps in the selected range are reported, even outside sampled
phase edges. Missing/malformed required producer fields, replay/reorder/reset,
foreign owner/publisher, role mismatch or unsupported clock make it `invalid`/
exit 1. There is no sort, deduplication, gap filling or clock bridge. New owners
and host clock boundaries need separate captures/original inclusive line ranges.
All existing whole-file hash, line-range, uint64 date, parser/read/mutation and
output privacy bounds remain in force. Legacy default output remains unchanged,
including null producer start dates and conservative unknown attribution.

### Qualification and future private manifest recipe

On macOS the production-tool suite passes **39/39** (19 legacy, 20 strict),
zero skips, in 0.387s. The actual existing project CTest registration passes
**1/1** in 0.45s after a portable configure; no C++ target is compiled. Explicit
synthetic producer fields/steady dates cover missing unchanged-counter reports,
first/predecessor/owner/publisher/panel/clock rejection, delayed original dates,
retirement/new scopes, unavailable input, video replacement, exact uint64s and
bounded parser/read/hash/mutation/CLI behavior. These are Python component
checks, not producer/GPU/phone performance evidence.

The retained original V83 42-pair and V84 22-pair legacy audit output is
**byte-for-byte identical** to accepted `6f3763c`, including all conservative
null attribution (output SHA-256
`4262353e927dceaf630b062aeb4239644e9d2a6434816753c434e31aa5998ff5`).
Strict mode deliberately rejects those old reports with `missing_producer_fields`;
that expected exit 1 is a negative check, not failed qualification or new evidence.
No raw phone capture is read/exported. Pinned format19.1.7 and whitespace pass.

One foreground shared-slot/default15GiB lane retains all 860 source hashes,
logs/results, full legacy output, and 15 CMake/Testing files outside its build.
Hashes are independently reverified; the owned CMake tree is absent. No container
was created, so container signal cleanup is inapplicable. No failed qualification
attempt, Linux/application/full matrix, GPU, phone or bench test is claimed.
Private receipts are under `20261014/codex-video-report-chain/`. Final source
matches the qualified tool/tests/CMake; only documentation qualification prose
changes afterward. Manager reviews this five-file extension and runs the complete
coherent required matrix before any new runtime release.

Phone-session alone, after producer/tool review and a future qualified immutable
generation, should retain original finite diagnostics from session start,
source/release/input identities and independently observed host/owner scope.
Keep full original report and sample files private, hashing whole files and
selecting explicit original `[first,last]` lines only at real scope boundaries.
Use the existing schema-1 recipe below with non-null scope labels justified by
those retained identities; preserve each sample's original `remote_sampled_ns`
and cluster tuple. Do not retrofit V83/V84 lines or infer producer fields from
catalogue sequence/order/file dates. First run legacy and strict modes separately
and retain their manifests, hashes, output and exit codes. Expect missing
predecessors to remain incomplete rather than substituting a five-second span.

Use moving main/advancing cluster and continuous music Off30/CameraOn60/Off30,
within existing `diagnostics=true`, `diagnosticReports=30` limits. Ordinary
Home/Back should preserve the producer series; Disconnect/reconnect must start
a new publisher/series and a separate capture. Split actual clock/owner changes;
review dated video replacement/retirement snapshots without whole-window ID
attribution. Keep both streams/UI-Sync/audio/control, main/touch/cluster/native
fallback/Climate alive. No fault injection, second owner, radio/display command
or new acquisition belongs in this offline tool. Full frame/sender correlation
and real new-generation data are explicitly parked pending that sole owner.

## Offline original-report audit — 14 October, source only

`tools/video_attribution.py` audits existing numeric display reports against
original `remote_sampled_ns` phase samples. It opens no socket, phone, display or
remote command and imports no PCM collector. This is source-only tooling from
`37fb44b`, distinct from selected V84 runtime `3bd3db9`. It changes no production
queues, deadlines, GPU, clocks, source tokens, ACKs or diagnostic budget.
UI keys/actions/IDs/new pieces for native UI integration: **none**.

The concrete missing observation is a dated identity join. `ReceiverVideo`
admits the authenticated transport's reserved connection ID to `NativeInputQueue`;
`NativeInput` replaces decoders off the control thread, retains authenticated
sender NTP ordering and maps decoded frames through the qualified media clock.
`Stream` owns its own staged tokens; `GpuDisplay` completes exact tokens on real
receipts. The V83/V84 five-second `live_ui.cpp` diagnostic reports aggregate
admission/decoded/presented counters and the original report host date, but omit
the publisher, transport ID, per-frame join and the producer's previous report
date/sequence. Manager catalogue sequences are not video frame IDs. Phone-local
encoder ACK dates and sender-to-panel/host-clock mapping remain absent.

The audit preserves `previous_report_mono_ns` and `report_mono_ns` exactly. It
leaves `producer_window_start_mono_ns` **null**, because an adjacent extracted
line alone does not prove the producer's previous report. Counter and printed-rate
checks detect many omitted predecessors; a gap with unchanged counters can be
undetectable. `whole_report_pairs`, `audited_report_span_ns` and
`weighted_fresh_fps` describe arithmetically consistent adjacent reports wholly
inside the sampled phase, under the declared extract scope. They are not a
complete-capture or per-frame correlation certificate. Printed-rate means and
original per-window counters remain separate from counter-derived rates.

The legacy/default private schema-1 manifest contains `captures` (1–16). Each has:

| Field | Contract |
| --- | --- |
| `label`, `source_revision` | Anonymous label and declared full 40-character source commit. Source declaration is not verified deployment. |
| `clock_domain`, `owner_scope` | Anonymous observed scope labels or null. Never a manufactured boot/session identity; no join across captures. |
| `reports` | `{ "path": "original-diagnostics.txt", "sha256": "<64 lowercase hex>" }`; relative to the manifest or absolute. Entire original file is hash checked. |
| `report_line_range` | null for the whole extract, or inclusive original `[first, last]` lines chosen explicitly at owner/clock boundaries. No sorting, deduplication or silent reset repair. |
| `phases` | 1–16 ordered `{ "label": "off-baseline", "samples": { "path": "original-samples.json", "sha256": "<hash>" } }` entries. Samples contain original dates, both Ready flags and cluster nonce/connectionGeneration/sequence. Existing decimal-string IDs remain exact integers internally. |

Keep manifests/reports private. The output omits paths, publisher nonces and
raw sample contents. It retains original line numbers, input hashes, integer
nanosecond dates, counter/rate arithmetic, source declarations and explicit null
correlations. `display_report_to_sample_publisher` is labelled
`operator_scope_only_not_embedded_in_report`; unchanged sampled publishers
cannot independently authenticate a display line. Samples are status polls,
never frame counts or optical presentation. Fresh counters count accepted
source tokens, not independently verified distinct picture contents.

```sh
python3 -B tools/video_attribution.py /private/evidence/manifest.json \
  > /private/evidence/video-audit.json
python3 -B tests/video_attribution_test.py
```

Exit 0 means `consistent_windows`; 2 means `incomplete` (missing panel/usable
interval or rejected interval), with partial rows explicitly retained; 1 means
`invalid` and emits only a fixed error category. Reject changed hashes/files,
nonregular/symlink inputs, duplicate/malformed/nonfinite numbers, reordered
dates/sequences, cumulative resets, mixed publishers and overlapping phases.
Every capture starts a fresh predecessor state; retired reports cannot bridge
into a successor. Clock/surface counter changes are retained separately, not
relabelled as phone clock errors or transport failures. Pipeline residuals may
be negative because work crosses report boundaries; no artificial balance.
Bounds: 128 KiB manifest, 16 MiB per report file, 2 MiB per sample file, 64 MiB
total referenced bytes, 32 KiB lines, 8,192 reports/samples per file. Parsing,
hashing and JSON output happen only in this offline process.

### Retained evidence and component limits

The existing V83 original extract reproduces 42 pairs (5/11/5 per panel) and
the published fresh means/counters. The V84 moving extract is **not** one ordered
owner segment: after lines 1–144 it appends two earlier startup sections with
decreasing host dates/reset scene counters. The audit rejects that whole-file
join. An explicit `[1, 144]` selection, preserving the full original file hash
and original line numbers, reproduces 11 pairs per panel and means
18.433782/31.627573 fps, decoded 1,036/1,774 and superseded 20/32. The exclusion
is an evidence-assembly boundary, not a demonstrated receiver fault. No phone
capture is read by this tool, and no timeout is assigned to a phase or panel.

The focused production-tool suite covers original-date arithmetic, phase edges,
redraw/fresh separation, missed predecessors, missing fields/panels, reordered
and duplicate reports, reset/retired segments, publisher restart, source-clock
counters, unbalanced pipeline counts, uint64 precision, malformed JSON/numbers,
bounded file reads, mutation/hash/symlink/FIFO rejection and sanitized CLI
success/incomplete/failure. Its fixtures use explicit synthetic clocks and no
phone/GPU. The 19-case suite passes, including cross-phase publisher-sequence
reversal, explicit unavailable totals and production byte/record budgets;
the actual repository registration passes 1/1 focused CTest. Pinned
clang-format 19.1.7 and whitespace checks pass. Original retained-data audits are separate offline consistency
evidence. Private logs/source hashes/manifests/receipts live under
`20261014/codex-video-attribution/`; no application build or hardware test is
implied. Manager runs the coherent required matrix before a new release.

### Explicit park and exact owner follow-up

**Runtime cause/repair, fresh60 and timeout-free acceptance remain parked/open.**
This bounded offline milestone does not establish a narrow production defect.
Missing contract/evidence: original dated frame identity through ingress,
decoder/admission, staging/submission and accepted presentation; authenticated
video generation, exact report predecessor and trusted sender/host/panel mapping.
Do not add another logging framework or merge the pending PCM branch here.
Any runtime observation interface must be reviewed with the existing collector
owner, retain source dates, keep disk/encoding off control/VM/GPU callbacks and
be qualified on a new immutable generation by phone-session alone.

For that sole owner, after manager review and coherent qualification:

1. Retain source/release/input hashes, host boot/receiver invocation and original
   launch/connection/video generations, negotiated geometry/rate and unchanged
   response/poll/fence/queue policy. Give each contiguous owner/host-clock segment
   its own capture; retain full-file hashes and any explicit original line range.
2. Use a known moving main workload and advancing cluster content, then the same
   continuous music Off30s/CameraOn60s/Off30s workload. Keep both streams/UI-Sync/
   control/audio, native fallback, main/touch and Climate alive; stop on owner or
   panel failure. Existing `diagnostics=true`, `diagnosticReports=30` is finite;
   do not edit a selected release or restart merely to extend the budget.
3. Capture original host-monotonic samples and numeric owner reports off-device
   from session start, plus bounded private phone logs. Status/catalogue sequences
   and file dates grant no video freshness. Keep phone wall/media and host clocks
   separate until a genuine mapping exists. Without trusted stream mapping,
   sender rates/timeouts remain unassigned; an observed host wall-clock pair alone
   does not synchronize the phone clock.
4. Run this offline audit on the originals. Retain rejected joins and uncovered
   spans. For an eventual collector, require the producer's actual previous report
   date/sequence and publisher/video generation; then original frame source and
   receipt identities. Do not recover missing IDs from bitrate, frame order, mtime
   or redraws. A missed report or retiring generation must stay explicit.
5. Qualify ordinary retirement/reconnect and Home/Back separately; record whether
   late old-frame/receipt rows were rejected and a successor received fresh inputs.
   Compare sender, ingress, decoder and presentation only where actual identities
   and clock domains match. A causal fix requires a deterministic production-owner
   failure/retirement regression before changing policy.

## V83 fresh-frame measurement — 14 October

Selected frozen0fe2ef9, original receiver monotonic windows entirely inside
controlled Music Off30s/CameraOn60s/Off30s:

| Interval | Main fresh fps mean | Cluster fresh fps mean | Whole windows per panel |
| --- | --- | --- | --- |
| Off baseline | 15.466 | 31.880 | 5 |
| Camera On | 15.610 | 29.422 | 11 |
| Off return | 15.069 | 31.099 | 5 |

Redraw means are about60/57/60fps and do not count as fresh video. Main decoded
rates are about17fps for this Music workload; this is not a maximum-throughput
animated-screen benchmark. On includes138 cluster superseded inputs, one
backpressure event per panel and no clock drops. The bounded63,375,203-byte
phone log has40 actual frame-POC timeout messages; phase attribution is unavailable.
Fresh60 and timeout-free acceptance remain open. Both panels/PCM stay available,
but On adds three audio underruns. Exact private windows/counters/screens under
`20261013/phone-session/` retain the selected generation; no GPU/phone success is
inferred from source-only pacing checks.


This is a dated measurement record, not the current deployment or task order.
Runs below use different inputs, clocks, renderer options and outputs; compare
them only within their stated scope. The user accepts 30 fps for current feature
work while sustained fresh-source 60 fps remains a goal. See [GOAL](GOAL.md) and
[HANDOFF](HANDOFF.md) before choosing a new experiment or changing the appliance.

## Retained V83 fresh-video diagnosis — 14 October 2026

The phone-session owner collected this evidence on qualified V83 source
`0fe2ef9`, release digest
`d2cfa857bd62e14d55447b6b7a83147f86bb9926e8588632dde893fb56768293`.
The original `284c6c6` pacing repair is integrated there. This follow-up
fast-forwards that source branch to V83 and changes documentation only; all
**735 non-Markdown tracked inputs** match its qualified snapshot and retained
source/artifact hashes. It adopts no pending V84 source and requests no deployment
or bench slot. Phone-session alone owns integration, qualification and live work.

### Original windows and receiver observations

The repeat uses a newly started continuous track and Camera Off 30s/On 60s/Off 30s.
The earlier track-ended/paused attempt is excluded. The original 114/226/114
status polls are all both-Ready on one publisher, with identical before/after
service-owner records. Those **454 polls contain 120 distinct manager receipt
sequences**, not 454 new video frames. Source-frame counts below come from the
receiver's actual accepted staged-token receipts.

The complete retained display extract contains **172** original reports. For
each panel, select a report only when both it and the preceding same-panel
report fall inside the original phase's first/last CM5 monotonic sample dates.
This reproduces **5/11/5 whole windows per panel: 42 windows total**. Covered
spans are approximately 25.02/55.09/25.02 seconds, rather than the full nominal
30/60/30. The reported means below are means of the original window rates;
counter-derived rates using actual spans agree within printed precision.

| Phase / panel | Windows | Decoder admissions | Decoded | Fresh presented | Superseded | Admission fps mean | Fresh fps mean | Scene redraw fps mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Off baseline / main | 5 | 421 | 423 | 387 | 38 | 16.824 | 15.466 | 60.024 |
| Off baseline / cluster | 5 | 799 | 800 | 798 | 1 | 31.921 | 31.880 | 60.006 |
| On / main | 11 | 957 | 959 | 860 | 100 | 17.371 | 15.610 | 57.523 |
| On / cluster | 11 | 1,760 | 1,758 | 1,621 | 138 | 31.945 | 29.422 | 57.320 |
| Off return / main | 5 | 413 | 413 | 377 | 35 | 16.508 | 15.069 | 59.998 |
| Off return / cluster | 5 | 782 | 782 | 778 | 4 | 31.259 | 31.099 | 59.999 |

Every selected window reports zero late, clock-dropped, non-monotonic and
cancelled/discarded frames. Suspension, clock-epoch/unavailable, owner-gap,
feedback-delay and surface-withdrawal/restoration counters do not increase.
Camera On records **one new decoder-backpressure episode per panel**, versus
zero in both Off phases. These are episode counts, not blocked duration, TCP
backpressure or compressed-ingress occupancy. Admission, decode, supersede and
presentation counters are different pipeline stages; outstanding frames cross
window boundaries, so their totals are not an exact per-window balance.

Off-phase cluster fresh presentation closely follows its approximately
31–32-fps decoder supply, including individual fresh windows above 32 fps.
That does not demonstrate a fixed 30-fps receiver ceiling. Main admits only
about 16.5–17.4 fps in these workloads, so its 15-fps presentation cannot establish
an inability to display 60 supplied frames/s. Camera On adds real receiver-side
selection loss: the cluster's 138 superseded frames amount to about 2.5 frames/s
across its measured span, while admissions remain near 32 fps.

`Stream::discard_obsolete` intentionally retires older unprepared frames once a
newer due frame exists, preserving prepared-token ownership. These counters
prove that selection happened; they do not prove that its policy or lifetime
handling is defective. No deterministic narrow production-owner bug follows
from aggregate counts alone.

### Rendering cost, sender evidence and limits

Original per-window composition means for main/cluster are approximately
**3.210/7.362 ms Off**, **6.334/8.251 ms On**, and **3.162/7.358 ms Off return**.
Present-to-poll means rise from **1.838/1.252 ms** to **11.731/13.825 ms** On,
then return to **1.887/1.228 ms**. Completion-wait means rise from
**2.379/2.755 ms** to **2.935/2.942 ms**. This associates camera work with longer
composition/poll delays and more superseding; it does not locate a thread stall
or establish causality. The retained extracts omit worker-turn/task costs,
deadline traces and per-frame arrival/decode/staging dates. Per-panel means are
not a measured combined owner budget or a p95/outlier distribution. Scene
redraws near 60 fps and the earlier synthetic worker benchmark are separate
from fresh phone frames.

Inspecting eight nonoverlapping **1-MiB byte slices** of the bounded
63,375,203-byte phone capture yields **38 complete encoder reports, 34 active**.
Fourteen active reports transmit 11.94–19 fps (median 16), with median sender
drop 37 and repeat 48.995 fps. Twenty transmit 20.03–35 fps (median 31.005), with
median drop 0 and repeat 16.5 fps. These are deliberately limited byte samples,
not representative whole-capture statistics or assigned main/cluster streams.
For example, one actual report has Input/Enc 54, Tx 16, drop 38, repeat 50 fps;
another has Input/Enc 31, Tx 31, drop 0, repeat 16 fps.

The sender itself therefore reports reductions/repeats before transmission,
consistent with adaptive or content-driven production/selection. The samples
do not prove that explanation: network pressure or receiver ingress service
could influence upstream decisions. Privacy-redacted stream labels, no reliable
phase clock mapping and missing original socket/ingress dates prevent causal or
per-panel attribution. Subtracting separately aggregated repeat/drop rates does
not recover distinct displayed pictures.

The owner's complete bounded-capture summary counts **40 Frame-POC timeout
messages**, with **no phase attribution**. The follow-up slices independently
contain one notice; they do not recount all 40. The earlier local
encoder-reference acknowledgement finding remains scoped as below. These notices
are not compositor losses or proof of a missing receiver ACK, and no timeout
resolution or live speedup is claimed.

### Explicit park and remaining evidence

**Runtime repair and fresh 60/timeout-free acceptance remain parked/open.** The
original pacing fix stays intact; no queue/deadline expansion, new ACK/opcode,
fabricated source clock/receipt or GPU redesign is justified. Thirty fps remains
accepted for feature work. This diagnosis does not block the reviewed V84 train.

On a suitable future phone-session-owned run, retain original-dated, bounded
records on the existing owners: a known moving-source workload, negotiated
geometry/fps, actual response/advance/queue/fence policies, authenticated video
generation and an observed sender-to-panel mapping. Capture receiver socket
progress/TCP or ingress backpressure, decoder admission/output-ready and exact
source timestamps, due/stage/supersede decisions with queue/lease occupancy,
plus submitted-token/accepted-receipt dates. Retain worker turn/task costs and
draw/deadline traces in the same windows. Keep phone wall time and host
monotonic domains distinct until an actual correlation is recorded.

That evidence can distinguish source adaptation, transport/burst arrival,
decoder/lease service, configured due-frame selection and shared-owner/output
pacing. Another manager poll or cumulative retirement total is insufficient.
Any needed diagnostic source change requires bounded numeric logging, exact-source
qualification and a new immutable generation through the phone owner; any later
repair needs a deterministic production-owner regression and lifetime/failure
checks. No immediate deployment is requested for this documentation.

Private `20261014/video-fps-v83/` retains the independent consistency audit,
42 numeric original windows, input/slice hashes and sender samples. The audit,
pinned 19.1.7 format check and Git whitespace check pass. There is no new compiler
matrix or production regression claim; no heavy lane, build tree or container
was created. The qualified V83 matrix is retained component evidence only.

## Phone frame timeouts and shared-owner pacing — 13 October 2026

This is a source-only investigation on `development branch`, based on
`141c88e`. No CM5 services, manager actions, releases or display modes were
changed. Read-only inspection found all five services active with zero restarts;
that is an owner snapshot, not video qualification. The phone-session agent owns
all connected tests and deployment. Thirty fps remains accepted for features.
Read-only inspection of V81's bound display configuration finds both panels at
**1920×720, `maxFPS=60`**. Thus a configured 30-fps ceiling does not explain that
generation's observations; an advertised maximum is not a source-rate promise.

### What the retained phone logs establish

The V81 USB syslog covers 21:18:46–21:37:39 on 13 October and contains **527**
VideoProcessing frame-POC timeout notices. Reported thresholds range from
2–36 ms, with expiry ages mostly 16 or 48 ms. The earlier V75 capture on
12 October (23:02:08–23:17:07) contains **118** of the same notices. These are
phone encoder-reference events, separate from our decoder's three-second stall,
UI transport deadlines and actual compositor receipt deadlines.

In V81's initial 37-second window, 74 encoder reports have median input,
encoded and transmitted rates of **33.96 fps**, median repeat rate **16.98 fps**
and zero reported drops. In the later 21:30–21:37 window, 473 active reports
have median input **33.00**, transmitted **32.99**, repeat **18.00 fps** and zero
reported drops. An additional 443 reports in that window have no transmitted
frames. The whole capture includes transient transmission at **60.17 fps**;
there is no demonstrated hard phone-side 30-fps limit. V75 likewise varies:
active report medians are input **39.02**, transmission **30.99**, repeats
**28.01** and drops **6.00 fps**.

Those figures are sender reports, including repeated pictures. They do not
identify main versus cluster: the stream label is privacy-redacted, and changing
target bitrate is not a reliable stream ID. They also cannot be converted into
distinct displayed-picture counts by subtracting separately aggregated repeats
and drops. They establish that the phone often reports transmission at roughly
33 fps already; these are not receiver socket measurements. Neither advertised
`maxFPS=60` nor local signed animation makes that a 60-fps fresh-source workload.

### What the timeout means

Read-only inspection of the privately cached phone-build firmware
(`24B5099f`, matching the retained phone investigation) locates the exact notice
in VideoProcessing's encoder reference bookkeeping. The helper consumes
acknowledged tokens, marks outstanding references expired when their media-time
age exceeds twice the configured or smoothed acknowledgement interval, and
also expires a reference older than an acknowledged display-order frame.

The TCP AirPlaySender path's `vdsink_PushFrame` calls `CMBufferQueueEnqueue`,
then collects `RequireAcknowledgementToken` attachments and asynchronously
emits the **local** `FramesAcknowledged` event. This happens before the separate
screen-stream dequeue/transport path; it does not wait for our decoder or a
Wayland presentation receipt. The timeout therefore means that the phone's
encoder bookkeeping has not consumed its local acknowledgement within that
policy. Delayed encoder/sink/event scheduling is a plausible explanation; its
precise runtime trigger is still unmeasured. Receiver TCP backpressure may
indirectly affect sender queues, but these notices alone do not prove that.

Our authenticated `/feedback` response is an empty keepalive 200, with no
playout-clock semantics. Signed UI-Sync acknowledgements, TCP acknowledgements,
encoder-reference acknowledgements and Wayland presentation feedback are
separate contracts. There is no evidence here justifying a guessed video ACK,
synthetic presentation date or new `updateFeedback` payload. Private string,
cross-reference and disassembly receipts stay outside Git; no firmware
implementation is copied into the receiver.

### Receiver cause repaired in source

Both signed panels share one VM/GPU owner. Their bounded compressed queues
feed asynchronous GStreamer decoders; decoder output retains four native
leases per panel, three pending decoded slots and at most two prepared source
tokens. Backpressure retains compressed deltas for retry. Only a receipt for
the exact staged token increments `videoPresented`/`freshVideoFps`. Neither a
redraw of the current token nor an encoder repeat proves new picture content.

The worker previously rounded completed work to the next fixed 16.667-ms slot.
Combined main/cluster work of **22 ms** consequently waited until **33.333 ms**
before the next ordinary turn: an avoidable **11.333-ms idle**, limiting that
workload to 30 turns/s. Historical live measurements below show combined draw
and script work crossing one period, so this is a relevant local bottleneck,
although it does not explain every V81 source or presentation-rate limit.
With `feedbackPollMs` enabled, some of that interval can already service
receipts or submit another scene; those polls never advance script tasks.
Thus 30 ordinary worker turns/s is not itself a 30-presentation ceiling. The
synthetic comparison isolates ordinary turns with feedback polling disabled;
the connected comparison must retain and record the actual polling policy.

The worker now schedules from its actual turn start: next start is the later
of start plus one period and completion. Under-budget work consumes its
remaining budget; overruns resume promptly. There is no catch-up debt, and a
cheap turn after a stall still waits one period from its own start. Per-display
rate limits, receipt capacity, source dates, GPU fences, suspension, retirement
and script PTS gates keep their existing authority. `missedSlots` now counts
extra period budgets consumed by ordinary turns, not dropped phone frames.

The deterministic regression compares 300 synthetic 22-ms turns: **6.600 s**
with owner pacing versus **10.0000002 s** with the former fixed-phase policy.
It also checks under-budget pacing, exact boundaries, late wakeups, a five-second
stall followed by cheap work, invalid ordering/period and overflow.
An interleaved Mac production-worker probe (10-ms main plus 12-ms cluster
sleeps, 101 turns) measures **29.98–30.00 Hz before**, **38.21–38.40 Hz after**
on this loaded host. Requested sleep cost is not measured GPU cost, and this
probe has **zero phone frames and zero presentations**. No live speedup or
resolved phone timeout is claimed.

Local validation passes the complete macOS portable suite (**156/156**), Linux
ARM64/GCC full-Skia nonhardware suite (**185/185**), the three directly affected
Mac frame/worker/display-scheduling checks, and the bounded scheduler regression
under Apple Clang ASan+UBSan with strict warnings. These are component checks,
including existing suspension, fault and retirement paths; no connected phone,
physical GPU, camera or audio acceptance is inferred.

The Linux job retains its normal flags, pinned Skia and hardware exclusion, but
uses a private Mac-backed build directory and an unprivileged builder UID
matching the host. Its original CTest log-directory failure and the alternate
wrapper's initially unwritable builder home remain retained separately. After
correcting that home ownership, the complete suite passes in **82.52 s**, with
no CTest retry option or warning relaxation. The initial missing-header compile
failure and stopped incomplete builds are also retained; stale-binary results
are excluded. The standalone Mac scheduler sanitizer check disables leak
detection; it is not evidence from the full Linux sanitizer matrix.

Reproduce the synthetic cases without any phone, signed bundle or display:

```sh
cmake --build build --target headunit-ui-schedule-benchmark
build/headunit-ui-schedule-benchmark
ctest --test-dir build -R '^(frame-schedule|theme-worker|theme-display-scheduling)$' --output-on-failure
```

The existing bounded display report now includes its local `mono_ns`, decoder
`videoSubmittedFps`, and interval counts for submitted, decoded, superseded,
discarded, late, clock-dropped, non-monotonic and decoder-backpressure events.
Submission is decoder admission, not socket receipt or display success.
`videoBackpressureWindow` counts new blocked episodes, not blocked time. A
single previous stats snapshot replaces the former parallel counter arrays.
Report frequency/limits stay unchanged; the first report supplies cumulative
counts, with rates only after a real measured interval.

### Measurement plan for the bench owner

1. Record the selected release/source/digest, both configured `maxFPS` values,
   negotiated video geometry/codecs, display modes, response policy, queue depths,
   feedback polling and fence policy. Use the same signed pack and phone workload.
   Record the phone build and any observed low-power/thermal state as well.
   Collect any available current-generation baseline before changing it. No
   restoration or rollback run is needed.
2. Integrate this branch into the bench owner's latest source, qualify the merged
   source, rebuild the full-Skia receiver (`headunit-control-probe`) and publish a
   new immutable generation through the existing release owner. Keep all private
   roles, pairings, camera/audio policy and the other owners consistent. Replace
   a binary only through new generation publication; this branch ships no ARM64
   release artifacts or new helper/configuration contract.
3. For a two-minute test set the new private installed-UI input's `diagnostics=true`
   and `diagnosticReports=30` (existing options) before sealing. Keep the current
   response/fence/poll/queue policies. Capture reports from session start, off-device;
   discard warm-up windows explicitly. Do not raise queue bounds or enable deferred
   fences for this initial comparison.
4. With both panels Ready, run separate, dated windows for MapsMedia's normal Map,
   a moving/scrolling main workload with cluster Map still active, and a heavier
   DigiRing or DigiBandTicks layout. Keep camera Off first; then repeat the same
   workload with camera On/Off and active decoded music under the phone-session
   owner's camera/audio plan. Record main/touch/Climate, cluster, audio progress,
   clock suspensions, service restarts, temperature/throttling and memory.
5. Capture a bounded USB phone syslog on the Mac in the same windows, for example
   `perl -e 'alarm 120; exec @ARGV' idevicesyslog`, redirected to the dated private
   cache. Alarm exit is the capture limit, not a receiver fault. Retain original
   dates privately, exclude identities/raw payloads from Git, and align local
   `mono_ns` through an observed host clock/date pair rather than inventing phone
   time. Phone stream labels may remain unavailable: report that limitation.
6. For each panel compare decoder admissions and decoded counts, **freshVideoFps**,
   scene fps, owner Hz/budget overruns, completion wait, submit-to-present and
   present-to-poll time. Attach phone input/encoded/transmission/repeat/drop rates
   to a panel only with an observed stream mapping; otherwise report them without
   a panel label. If phone transmission, admissions and decodes stay near 33 with
   no decoder backpressure, source cadence is the immediate observed limit.
   Decoder backpressure alone does not measure TCP or compressed-ingress stalls.
   If admissions exceed fresh presentations while superseded
   or discarded counts grow, investigate staging/presentation. Decoder backlog/
   backpressure with a slower decoded rate implicates decode/lease or owner service.
   Long cluster calls plus delayed main receipt polling demonstrate shared-owner
   interference; long submit-to-present with prompt polling implicates output pacing.
7. Count frame-POC notices per capture duration and compare their ages/thresholds
   with phone rates. An increased receiver rate with unchanged timeout notices
   separates the local pacing repair from phone encoder acknowledgement scheduling.
   A correlated change is evidence to investigate, not proof of a wire-ACK contract.
   Check ordinary Disconnect/reconnect and Home/Back retirement through the existing
   bench owner; old source tokens must not qualify new readiness. Finish with
   camera/content Off and a fresh both-Ready observation.

Full-resolution 60 distinct fresh frames/s remains open. The remaining limits
include actual phone production/content cadence, synchronous full-theme GPU
work on the shared owner and physical compositor pacing; their contributions
need the paired measurements above.

## Authenticated video parser — 9 October 2026

The source candidate `development branch`, based on `bf5f8d8`,
parses complete wire packets without first copying them into staging storage.
It retains only an incomplete packet and reserves its bounded size once the
header arrives. This also fixes rejection when a TCP read completes a legal
maximum-size packet and contains the next packet. Parsing faults permanently
close the instance and release its key/storage. See [native video](NATIVE_VIDEO.md).

`tools/benchmark_video_parser.cpp` generates 512 authenticated frames with
64-KiB payloads and successive synthetic nonces before timing. Each of seven
runs includes parsing, decryption, returned-payload equality checks and packet
retirement. No phone, decoder, GPU or compositor receipts participate. The CM5
comparison compiles this same fixture with the `bf5f8d8` and candidate parser
sources using GCC 14.2.0 `-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror`, linking
system OpenSSL. Existing appliance services continue running, so timings are
component observations under bench load.

| Feed size | Before median | Candidate median | Observation |
| --- | ---: | ---: | --- |
| One complete packet (65,680 bytes) | 54.166 ms | 51.112 ms | 5.6% less time |
| 16 KiB | 54.388 ms | 54.245 ms | Effectively unchanged |
| 4 KiB | 54.258 ms | 54.335 ms | Effectively unchanged |

These are batch times for 512 frames, not per-frame presentation latency.
Complete-packet ranges are 53.909–54.552 ms before and 51.066–51.155 ms after.
The live receiver currently reads at most **4 KiB** per call; this experiment
therefore establishes no live frame-rate or boot/connection-speed improvement.
It does not justify increasing socket reads without measuring control-thread
fairness and existing bounded video inbox admission.

To reproduce the synthetic candidate benchmark, configure a Release build with
the portable dependency options in README, then run:

```sh
cmake --build build --target headunit-video-parser-benchmark
build/headunit-video-parser-benchmark
```

The boundary and retirement regressions pass portable and strict CM5 GCC
component checks; all 136 portable suites pass and the parser passes macOS/CM5
ASan/UBSan. V27 remains selected. No real-phone, rendering-rate, cold-boot or
sustained-session qualification is implied.

## Distinct source submissions and exact coverage — 1 October

The display now records the actual video token drawn by every pending scene.
`NativeInput` can prepare two distinct due frames, completing each only through
its own actual receipt. Rejected ownership transfers only to a scene using the
same source token. The stream protects both preparations and retains bounded
ordered queues. Clock suspension and script flush invalidate old tokens.

Three decoder leases were insufficient for a current frame, two pending draws
and a queued replacement: the first token-based comparison accumulated late
cluster frames. Native input now selects **four** bounded leases (one current
plus three queued); the generic decoder still defaults to three. When the
decoded queue is full, output stays in the bounded decoder queue until space is
available. Compressed ingress is retained/retried. Hardware tests verify both
three/four-lease limits, and the two-pending test exercises distinct leases,
actual accepted/discarded receipts and clean clock/disconnect recovery.

### Moving video on both physical outputs

Same full-size synthetic video/clock/UI fixture described below, with two
pending scenes and 2 ms polling. Normal completion waits for the GPU fence;
deferred completion is a separately labelled experiment, not the live default.

| Run | Presented scenes/s, main / cluster | Fresh source frames/s, main / cluster | Mean draw, main / cluster | Mean submit → presentation, main / cluster |
| --- | ---: | ---: | ---: | ---: |
| Before source fix, DigiRing, 40 s | 59.83 / 45.18 | 29.83 / 22.53 | 2.82 / 11.44 ms | — |
| Two source tokens/four leases, DigiRing, 30 s | 57.00 / 57.03 | 55.67 / 56.50 | 2.96 / 11.69 ms | — |
| Two source tokens/four leases, DigiBandTicks, 30 s | 40.40 / 40.40 | 40.23 / 40.23 | 3.02 / 18.27 ms | — |
| Deferred completion, DigiRing, 25 s | 59.80 / 59.64 | 59.20 / 59.36 | 0.56 / 7.55 ms* | 22.92 / 25.08 ms |
| Exact coverage crop, DigiRing, 30 s | 59.07 / 58.47 | 58.70 / 55.07 | 2.93 / 10.57 ms | 18.22 / 21.29 ms |
| Exact coverage crop, DigiBandTicks, 30 s | 41.13 / 41.07 | 40.93 / 40.90 | 3.01 / 17.82 ms | 10.70 / 9.98 ms |
| Crop + deferred completion, DigiBandTicks, 30 s | 53.83 / 44.70 | 53.17 / 44.07 | 0.62 / 10.31 ms* | 27.05 / 24.35 ms |

\*Deferred draw wall time excludes later GPU completion; it is **not** a
completed-frame cost or proof of meeting a 16.7 ms GPU deadline. Actual receipts
and source counters still gate the result. Overlapping script work during the
normal fence wait measured 56.88 / 57.20 fresh frames/s for DigiRing and
42.12 / 42.12 for DigiBandTicks; it did not complete the goal either.

The four-lease runs decoded roughly 60 source frames/s per panel, without late
or clock-dropped frames or producer backpressure. Max staged source count is
two. Source supersession still occurs when the renderer misses an update.
The replay's bounded polling introduces source-timestamp jitter; results are
whole-run rates including warm-up, not an assertion that every refresh is met.
No screenshot readback was performed in these throughput runs. This is **not
live iPhone qualification** or a measurement of touch-to-photon latency.

Ordinary worker-turn reports now separate display and script costs. Before
coverage cropping, DigiBandTicks averaged 23.77 ms per ordinary turn, including
about 2.23 ms of cluster scripts; deferred completion reduced the CPU-observed
turn but did not make the cluster's actual throughput 60. Feedback-only turns
are separate and excluded from that worker average. Next investigate remaining
GPU work and independent GPU owners with immutable scene snapshots, preserving
VM ownership, real receipts and buffer lifetime. Splitting threads must be
measured, not assumed to fix GPU throughput.

### Exact coverage support

Gauge coverage is already rasterized exactly for its edge/cap oracle. Scan that
cached coverage once and remove only fully transparent padding. Preserve all
nonzero antialias pixels, device coordinates and nearest sampling. Clip fused
gradients to that support and use it to bound simple stroke-masked backdrop
output; filter input still retains its backwards-computed halo. Empty coverage
is cached as empty. No arc approximation, blur reduction or resolution change.

Same-binary uncropped/cropped animated 60-warm-draw comparisons, no video:

| Style | Uncropped median / p95 | Cropped median / p95 |
| --- | ---: | ---: |
| Cobranded | 10.72 / 12.03 ms | 10.59 / 11.65 ms |
| DigiRing | 9.54 / 11.30 ms | 9.29 / 9.49 ms |
| Modular | 16.70 / 17.84 ms | 12.78 / 13.57 ms |
| DigiBandTicks | 18.05 / 19.92 ms | 17.77 / 18.70 ms |
| DigiBand | 14.93 / 16.71 ms | 14.62 / 14.84 ms |
| MapsMedia | 6.44 / 7.01 ms | 5.43 / 5.53 ms |

All six final 1920 × 720 images are **byte-identical** to the uncropped controls.
Independent GPU cases compare zero/short/full trims, thin/wide round caps,
transformed masks, additive gradients and backdrop halos to uncropped output
within 2/255; broader analytic/CPU oracles also pass. Twelve relevant CM5
graphics, native-video, receiver and Wayland checks pass. The Mac graphics and
script-runtime suites pass. Power remained unthrottled (`get_throttled=0x0`).
The benchmark restores active appliance services. Signed packs, original
fixture data, screen images and private logs remain outside Git.

### Regional immutable backdrop snapshots

Fused masked groups now determine their needed input **before** preserving the
parent image. Use Skia's reverse filter bounds for the complete chained blur
halo, join the output bounds, and intersect only with the actual source edges.
Keep the snapshot's device origin in filtering and final replacement. Groups
with subsequent/nested backdrops retain the earlier full-source path. This
avoids full-screen copy-on-write work for small effects; it changes neither
resolution nor blur quality. The diagnostic control is
`HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_SNAPSHOT_REGION`.

Same-binary animated 60-warm-draw comparisons, with coverage cropping enabled:

| Style | Full input snapshot median / p95 | Regional input snapshot median / p95 |
| --- | ---: | ---: |
| Cobranded (no eligible effect) | 10.48 / 11.56 ms | 10.32 / 11.36 ms |
| DigiRing | 9.17 / 9.69 ms | 9.02 / 9.24 ms |
| Modular | 13.16 / 13.58 ms | 12.11 / 12.54 ms |
| DigiBandTicks | 17.59 / 18.58 ms | 16.62 / 17.70 ms |
| DigiBand | 14.87 / 15.02 ms | 14.25 / 14.39 ms |
| MapsMedia | 5.42 / 5.57 ms | 4.81 / 4.93 ms |

All six final 1920 × 720 stills are byte-identical to the same-binary full-input
controls. Forty additional GPU cases cover chained filters, transparent and
high-frequency source pixels, global edges, clamp/decal sampling and
rect-preserving transforms, within 2/255. All twelve relevant CM5 checks pass
after rebuilding the affected binaries. Power remains unthrottled.

With both moving native streams and normal completion, DigiBandTicks reaches
**42.67 / 42.63 fresh source frames/s** over 30 s (42.87 / 42.83 scenes/s).
Cluster completed draw averages 16.87 ms and ordinary owner turns 22.24 ms,
including the main panel and scripts. There are no late frames, clock drops or
producer backpressure. This still misses 60 fps; the isolated median does not
include the second panel, scripts or the upper tail. No iPhone was involved.

### Bounded GPU submissions and stronger comparison controls

The optional deferred mode now owns a FIFO of at most the configured one/two
GPU submissions. Each submission holds its own immutable imported-image set,
EGL fence and absolute completion deadline. Finishing the older presentation
no longer waits for unrelated newer GPU work. Full queues wait only for their
oldest fence. Scene and video PTS still advance through accepted actual
presentation receipts, not fence completion. Defaults remain synchronous.

Exploratory 30-second moving-video runs, before the nested-blur qualification
restriction below, measured the following full-resolution results. Both panels
used two pending scenes and 2 ms polling. These are synthetic input/clock
experiments on physical screens, not live CarPlay results.

| Deferred policy | Style | Fresh source frames/s, main / cluster | CPU-observed draw, main / cluster | Mean submit → presentation, main / cluster |
| --- | --- | ---: | ---: | ---: |
| One outstanding GPU scene; blocking receipt poll | DigiBandTicks | 48.80 / 48.80 | 0.66 / 10.73 ms | 21.04 / 20.90 ms |
| One GPU scene; nonblocking receipt poll | DigiBandTicks | 55.30 / 45.27 | 0.78 / 13.74 ms | 27.59 / 26.09 ms |
| Two independently leased GPU scenes | DigiBandTicks | 53.77 / 47.20 | 0.64 / 11.02 ms | 25.39 / 25.19 ms |
| Two independently leased GPU scenes | DigiRing | 56.43 / 59.50 | 0.52 / 7.35 ms | 22.20 / 24.03 ms |

Deferred draw excludes later GPU completion. Neither policy proves a completed
16.7 ms frame deadline. The queue reached its bound of two in the heavy-style
run; a fast GPU may retire a frame before a second becomes outstanding.
Hardware-video regression checks retain source leases after stream teardown
until the owning fence retires, then verify the resulting pixels and lease
release. The permanent two-pending native-input test also runs deferred mode.

An opaque final-window experiment measured 50.30 / 47.07 fresh frames/s with
DigiBandTicks. Both it and a subsequent transparent-window control reported
ZERO_COPY for every accepted scene receipt: direct scanout was already in use.
Opacity did not add GPU headroom. Keeping the window EGL drawable current
through offscreen composition also produced no useful gain and was reverted.
`opaqueWindows` defaults to disabled and has final-window pixel checks. The
subsequent live comparison below did find a gain with the running backup
cluster; the isolated fixture result cannot predict that environment.

The GPU oracle previously captured diagnostic environment switches when its
Skia scene was created lazily on first draw, after the tests had restored the
environment. It now captures them at compositor construction. Assertions verify
that the full-source comparison actually takes the full-source path. The six
style PNG comparisons above used separate processes with their controls held
for the entire run and remain valid.

The corrected oracle exposed a 3/255 nested-blur difference where its existing
limit is 2/255. Nested/partially opaque groups now preserve the complete working
surface: merely retaining their mathematical input halo did not preserve Skia's
intermediate blur sampling phase. Only the qualified fully covering first
backdrop uses the fused regional path. The threshold was not relaxed. All 14
relevant CM5 checks, including both GPU scene paths, pass after the restriction;
the additional deferred hardware-video check passes as well. Future measurements
must use this corrected implementation. Heavy-style live 60 fps remains open.

Live diagnostic windows additionally report `freshVideoFps`, derived from
accepted source tokens, `zeroCopyScenes`, `maxStaged` and `maxGpuQueued`. Scene
fps is reported separately: an idle phone may send fewer fresh video frames
than the locally animated gauges present.

### Real iPhone and opaque cluster output — 1 October

The user reconnected with Apple Maps active and confirmed both physical views
and touch after each output policy. DigiBandTicks remained selected. Normal
completion, two pending scenes per panel and 2 ms feedback polling were held
constant; the independent backup cluster remained running. The CM5 reported
no throttling. Eleven five-second rate windows per panel were collected; the
following values are medians of the last nine interval means/rates, not
per-frame percentiles or a touch-to-photon measurement.

| Final window policy | Main / cluster scene fps | Main / cluster completed composition | Main / cluster submit → presentation | Main / cluster presentation → poll |
| --- | ---: | ---: | ---: | ---: |
| Preserve alpha | 30.80 / 29.00 | 4.29 / 25.22 ms | 23.62 / 34.71 ms | 14.50 / 8.96 ms |
| Opaque over black | 42.70 / 42.71 | 2.94 / 17.27 ms | 11.25 / 11.13 ms | 32.85 / 18.11 ms |

With preserved alpha, all main receipts but no cluster receipts reported
ZERO_COPY. With the opaque final blit and matching Wayland region, both panels
reported ZERO_COPY for every scene. The live backup/compositor environment
therefore differs from the isolated fixture, where direct delivery was already
available without the flag. Scene pixels and touch coordinates are unchanged;
transparency is resolved onto black only in the final window.

The Maps stream presented about 13.54 / 20.17 distinct source frames/s under
the two policies. The main image was largely idle, often sending no fresh
updates in a window. These source rates are reported separately from locally
animated scene fps and do not establish a 60-Hz source. Each panel retained its
one startup owner-gap suspension, with no subsequent clock/drop suspension in
the reporting windows. Startup late frames were 86/95 and 101/100 respectively,
then stopped increasing. Long-run recovery remains unqualified.

Opaque output improves live GPU/display throughput; it is not proof of lower
touch latency. Main presentation-to-poll delay increased while actual display
delivery shortened. The heavy layout still misses the 16.7 ms budget. The
bounded deferred queue then measured 55.35 main / 46.49 cluster scene fps.
The user again confirmed both views and touch. CPU-observed composition was
0.68 / 11.67 ms, excluding deferred GPU completion; submit-to-presentation was
29.02 / 28.26 ms and presentation-to-poll 4.20 / 2.72 ms. Both GPU queues
reached their bound of two, all scene receipts reported ZERO_COPY, and neither
panel gained another suspension during its eleven reporting intervals. This
is a throughput gain, not 60 fps or a proven touch-latency improvement. The
Maps source presented 14.20 fresh frames/s; its changing source cadence is a
separate qualification from animated scene output.

### Live DigiRing and the separate touch-latency gate

The next real-phone session selected DigiRing with the same opaque output,
two pending presentations per panel, 2 ms feedback polling and bounded deferred
GPU queue. Forty-eight five-second diagnostic reports per panel cover roughly
four minutes. The first comparison window uses the same nine interval means
after the first two rate windows as the heavy-style results above.

| Metric | Main | Cluster |
| --- | ---: | ---: |
| Early warm scene fps | 59.78 | 59.79 |
| Last nine windows, median scene fps | 59.49 | 59.32 |
| Early warm fresh phone frames/s | 0.00 | 22.80 |
| Early warm CPU-observed composition | 0.63 ms | 7.20 ms |
| Early warm submit → presentation | 23.44 ms | 25.21 ms |
| Early warm presentation → poll | 2.26 ms | 0.84 ms |

All 13,636 main and 13,635 cluster scene receipts reported ZERO_COPY. Each
panel retained one startup owner-gap suspension, with no additional suspension
or clock drop during these windows. Startup late-frame counters settled at
90/106. Deferred CPU composition excludes later GPU completion. The mostly
idle main and variable-rate Maps source do not qualify 60 fresh phone frames/s.
This establishes near-60 locally animated DigiRing output on the actual pair
of displays; it does not qualify every style, cold starts or long-run recovery.

The user still reports laggy touch despite smooth animation. The measured
submit-to-presentation interval covers only part of that path. Input, HID,
phone processing, video transport and decoding must be measured separately;
the complete physical tap-to-visible-result delay needs an end-to-end check.
Compare a shorter main queue without changing the cluster queue and preserve
actual receipts/native leases. Do not claim a latency improvement from fps,
CPU draw time or faster receipt polling alone. This is now a separate
first-priority acceptance gate in [the current goal](GOAL.md).

### Rejected animated-gradient shortcut

A separate experiment drew changed stroke-masked gradients directly rather
than rebuilding retained intermediate images. Every final six-style PNG was
byte-identical to its control, and additional GPU comparisons passed. However,
DigiBandTicks median/p95 worsened from 16.85/17.82 to 17.70/18.68 ms;
Cobranded worsened from 10.46/11.58 to 11.63/12.73 ms. Unchanged intermediate
keys were being drawn through a complex shader repeatedly.

An adaptive version cached a changed key once it repeated. Two interleaved
runs per policy removed most of the regression but produced no useful gain:
DigiBandTicks control medians 17.03/16.99 ms versus 17.05/17.31 ms;
Cobranded control 10.45/10.30 ms versus 10.49/10.56 ms. Cache builds stayed
unchanged. The shortcut and its special clip path were removed. Do not infer
that fewer intermediate allocations necessarily mean faster GPU execution.
The next substantial candidates are avoiding repeated conversion/drawing of
unchanged native input and reducing shared-owner/context costs, with explicit
pixel, producer-lease and frame-cadence checks.

## Moving native-video fixture — 1 October, queue investigation

The offline benchmark now includes two original, moving **1920 × 720 / 60 Hz**
all-intra HEVC fixtures. The normal video framer authenticates the generated
packets once; replay retimes the decoded access units at the bounded native
input queues. `v4l2slh265dec` produces native buffers for the same GPU display
owner used by the receiver. The signed entry points continue drawing their
gauges, masks and filters over an explicitly injected test backdrop inside the
reference pack's opaque backing container. Real Wayland receipts qualify the
result on both physical outputs. No per-frame scene readback is enabled.

This fixture has **no iPhone, wireless/socket negotiation or phone-driven asset
video selection**. Its clock and UI acknowledgements are simulated. The
backdrop augmentation is confined to the diagnostic executable and does not
modify the signed files or live receiver. The generator accepts no phone
capture/secret: its key is an explicit synthetic marker. It repeats independent
keyframes; compressed delta-reference behavior is not qualified by this loop.

A first injection below the pack's opaque container was decoded but visually
occluded. Those runs are excluded from the following visible-video results.
Private captures of both screens at two instants subsequently showed changing
test video on main and behind the signed gauges: 66.7% and 63.8% of output
pixels changed respectively. Screenshot readbacks were confined to that visual
verification run; they can perturb a few of its measured frames.

| Style / run | Presented scenes/s, main / cluster | Newly presented source frames/s, main / cluster | Mean completed draw, main / cluster |
| --- | ---: | ---: | ---: |
| DigiRing, 40 s without screenshot readback | 59.83 / 45.18 | 29.83 / 22.53 | 2.82 / 11.44 ms |
| DigiBandTicks, 30 s visual verification | 40.23 / 40.23 | 20.03 / 20.03 | 2.93 / 18.52 ms |

Both runs use full-size scenes, two pending presentations per panel and 2 ms
feedback polling. About 60 source frames/s were decoded per panel, with no
late/clock-dropped frames or producer backpressure. Power remained healthy
(`get_throttled=0x0`); active appliance services were restored after each run.

### Original source limit, now fixed

Before the token-based fix, native input permitted one staged frame while the
display permitted two pending scenes. When the first scene still awaited its
receipt, the second scene drew the **same** staged source frame. Only the
submission that staged it owned its completion; the later draw held the same
frame until its own scene receipt. `GpuDisplay` deliberately avoided a second
`stage()` while that ownership bit was pending. These runs therefore accepted
roughly one fresh source frame for
every two presented scenes, despite decoding around 60/s. This is a concrete
queue-policy limit in that implementation, not evidence that the phone
is restricted to that lower rate.

The token/lease fix above has passed the offline fixture and component checks;
the subsequent real-phone checks are recorded above. Separately, heavy styles still need GPU/owner
headroom: DigiBandTicks' mean cluster stages here were 5.13 ms traversal,
3.58 ms submission and 9.13 ms completion wait. The single owner adds main
drawing, video pumping, scripts and dispatch to that cost.

### Reproduce

Build with GPU and native-video options enabled; Node and FFmpeg with `libx265`
are needed only to generate these original fixtures. Keep all outputs private:

```sh
node tools/create_video_bench_fixture.cjs /private/main.fixture main
node tools/create_video_bench_fixture.cjs /private/cluster.fixture cluster

XDG_RUNTIME_DIR=/run/user/1000 python3 tools/benchmark_physical_ui.py \
  --profile /private/installed-ui.conf \
  --main-output HDMI-A-1 --cluster-output HDMI-A-2 \
  --main-pending 2 --cluster-pending 2 --poll-ms 2 --seconds 30 \
  --cluster-layout 1B0484EC-1933-524D-80F8-2FC1FC7B4B2D \
  --main-video /private/main.fixture --cluster-video /private/cluster.fixture \
  --output-dir /private/new-native-bench --pause-appliance
```

Reports distinguish `approximateFps` (scenes) from
`approximateNativeVideoFps` (distinct accepted source frames). Without video,
the latter is null. Native decoding, presentation, supersession, import and
visible-layer turn counters are recorded; visibility counters alone cannot
prove an unoccluded image. The parser/replay tests cover authentication failure,
truncation, wrong fixture markers, missing timestamps, frame-count limits,
chunk boundaries and bounded ingress pressure. This is a diagnostic fixture,
not a completed live-60-fps qualification.

## Full-resolution regions and cap alignment — 1 October, follow-up

The user correctly rejected the first purported endpoint fix: the orange band
still flared outside its gray circular track. The root mismatch was the script
cap ABI: reference raw value 2 was being cast to native square. The binding
now converts the inferred `ROUND=0, SQUARE=1, BUTT=2` script order explicitly.
Native C++ cap semantics remain unchanged. Wide circular ribbons also construct
their inner and outer arcs independently; a mathematical supersampling oracle
checks both near their endpoints. See [the visual correction](CLUSTER_STYLE_REVIEW.md).

### Full-size GPU changes

- Bound ordinary transparent isolation even when descendants use destructive
  blends; keep external destructive blending unbounded.
- Compute path extrema on the curve, excluding off-curve Bezier controls.
  Short bottom arcs previously inflated working regions by about 90 pixels.
- Walk masked backdrop paint order backwards to retain each transformed/chained
  blur's input halo. Allocate only that conservative working region and the
  final mask's output region; carry their origins into nested replacement.
- Fuse a fully covering opaque first backdrop directly into group initialization
  when later paints have no backdrop dependencies. Skip the redundant seed and
  intermediate replacement. Complex groups keep the general path.
- Feed simple stroke-mask coverage directly into final replacement, retaining
  complex mask surfaces. Three alternating same-binary DigiBandTicks pairs
  measured 17.68–17.86 ms medians versus 17.93–18.47 ms without this fusion.

All of these keep the **1920 × 720** output and original blur radii. The phone
was unavailable; no decoded phone video is included. Power remained healthy
(`get_throttled=0x0`). Active receiver/backup services were paused only for each
probe and restored afterward. Private logs, plots, signed files and images
remain outside Git.

### Animated signed styles

One 60-warm-frame control/final sample per style (three warmup draws excluded):

| Style | Full working surfaces / loose bounds, median / p95 | Regional/fused, median / p95 |
| --- | ---: | ---: |
| Cobranded | 11.42 / 12.66 ms | 11.07 / 12.31 ms |
| DigiRing | 17.51 / 19.33 ms | 9.78 / 11.46 ms |
| Modular | 38.13 / 38.94 ms | 16.08 / 17.41 ms |
| DigiBandTicks | 34.82 / 36.55 ms | 17.86 / 19.56 ms |
| DigiBand | 34.68 / 36.38 ms | 15.00 / 16.67 ms |
| MapsMedia | 14.43 / 14.99 ms | 6.81 / 7.02 ms |

Controls share the corrected cap/ribbon geometry with the final renderer; their
flags restore full groups, loose path bounds and unbounded isolation. All six
final PNGs differ from those controls by at most **1/255 per colour channel**.
Cobranded and MapsMedia are byte-identical. Separate GPU oracles cover sequential
and nested halos, transparent inputs, global edges, rounded mask alpha, skew/
scale/translation, all native stroke caps, destructive isolated descendants and
unchanged output on rejected unsafe scenes. The existing eight graphics,
script, offscreen-GPU and isolated-Wayland tests pass; additional mask/region
comparisons are included in the GPU scene suite. Hardware-decoded native-video
GPU composition and receiver Wayland video tests also pass; these component
fixtures do not replay moving phone video through both signed panels.

### Two physical outputs

A 20-second installed-UI fixture with full-size DigiBandTicks, no phone video,
two pending frames per panel and 2 ms polling presented **50.05 main / 49.65
cluster scenes/s**. The earlier same-size fixture presented 29.55 / 29.50.
Mean cluster drawing dropped from **29.78 ms to 16.16 ms**. Warm retained slow
records still reach **19.73 ms**, and some presentation gaps remain about 34 ms.
A separate 20-second DigiRing run presented **59.95 main / 59.90 cluster
scenes/s**, with 9.83 ms mean cluster drawing and no retained warm draw above
16 ms. This uses the same full-size/two-frame policy and no decoded video.

Those records retain only the worst 32 frames per panel; they are not a complete
histogram. Cold drawing remains over budget (about 57 ms here).

This does **not** meet a strict 16 ms deadline or establish stable live 60 fps.
The new mean breakdown is 4.51 ms traversal, 3.61 ms submission and 7.47 ms GPU
completion wait. The shared owner still adds about 1.15 ms for the main panel,
plus script/dispatch work. Cobranded, DigiRing and MapsMedia have comfortable
offline drawing margin; the larger styles need more headroom and live checks.

Overlapping script work with GPU completion increased this short physical
fixture to **52.85 / 52.85 scenes/s**, but retained warm draws still reach
19.42 ms. It does not meet the target and the installed overlap policy remains
unchanged pending a phone check.

A separate Release (`-O3`) build passed CPU/script/GPU tests but gave no
repeatable DigiBandTicks advantage over RelWithDebInfo (`-O2`): three paired
medians were 17.70–18.20 ms versus 17.74–17.95 ms. It remains an experiment;
the normal appliance build and compiler policy are unchanged.

### Diagnostic controls

The same binary can restore individual prior paths with:
`HEADUNIT_THEME_PROBE_FULL_BACKDROP_GROUP`,
`HEADUNIT_THEME_PROBE_DISABLE_TIGHT_PATH_BOUNDS`,
`HEADUNIT_THEME_PROBE_DISABLE_ISOLATED_BOUNDS`,
`HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_GROUP_FUSION`, and
`HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_MASK_SHADER` (set to `1`). These compare
rendering paths without editing the pack or its phone-facing configuration.

The remaining gates are a realistic moving native-video fixture, headroom in
the heavier signed layouts, and a real-phone two-display qualification. A
higher configured rate alone is not a throughput improvement.

## Circular gauges and blur output — 1 October 2026

The phone was unavailable. The CM5 reported `get_throttled=0x0`, with both
physical HDMI outputs connected. Receiver and backup services were paused for
measurements and restored afterward. Private resources and images stay outside
Git. These results do not establish new live-phone throughput or touch latency.

### Changes retained

- **Curved trimming:** single circular contours trim by exact angular distance,
  retaining a curved Cairo path. Flatten-then-trim previously left chords near
  the ends of wide bands. Clockwise/counterclockwise arcs preserve the requested
  butt, round or square cap. General paths/noncircular ellipses keep length
  trimming; this does not change square-cut caps into rounded caps.
- **Tighter coverage:** single smooth circles have no miter joins. Coverage
  allocation and group bounds use half the stroke width as padding, multiplied
  by sqrt(2) for square caps, instead of the general miter-limit margin.
- **Direct filtering:** filter the immutable parent snapshot without first
  copying it to another full-screen surface and copying its backdrop again.
  Local filter transforms, chained filters, alpha and edge modes are preserved.
- **Bounded output storage:** allocate only the transformed blur panel output,
  rounded outward with a two-pixel guard. Its input snapshot remains complete,
  including the halo. Translate the output shader back into parent pixels before
  the established premultiplied replacement operation.

All six animated layouts completed at 1920 × 720. Each row is one 60-warm-frame
sample before/after the combined changes, excluding three warmup frames.
These are completed draw times, not delivered phone fps.

| Style | Before median / p95 | After median / p95 | Median reduction |
| --- | ---: | ---: | ---: |
| Cobranded | 11.72 / 13.24 ms | 11.15 / 12.46 ms | 5% |
| DigiRing | 20.15 / 22.54 ms | 15.88 / 17.64 ms | 21% |
| Modular | 40.65 / 41.40 ms | 31.90 / 32.71 ms | 22% |
| DigiBandTicks | 40.33 / 43.59 ms | 31.20 / 32.86 ms | 23% |
| DigiBand | 40.94 / 43.79 ms | 30.61 / 32.51 ms | 25% |
| MapsMedia | 15.15 / 15.39 ms | 11.76 / 12.22 ms | 22% |

The curve fix changes geometry deliberately. Comparing blur paths with the
same corrected geometry, all six final images differ by at most one 8-bit
colour level; cropping their filter output then produces byte-identical images.
Synthetic GPU tests compare cropped/full outputs through transparent input,
chained filters, normalized/decal edges, radii 0.5/7/25 and translated/skewed/
nonuniformly scaled panels. All 18 comparisons were identical. Existing mask,
rounded-clip, halo and alpha oracles pass. Wide 94-pixel strokes match independent
short-arc oracles in both directions/all cap styles, including transformed GPU
output. Seven targeted graphics/display tests passed on CM5; the GPU scene
oracle also passes with the legacy blur path.

### Both physical displays

Each run lasts 20 seconds, with two pending presentations per panel and 2 ms
receipt polling. There is no decoded phone video; synchronization is simulated
but Wayland presentation receipts are real.

| Cluster / internal size | Main / cluster presentations per second | Mean cluster draw | Retained warm draws above 16 ms |
| --- | ---: | ---: | ---: |
| Cobranded, 1920 × 720 | 59.50 / 59.45 | 9.91 ms | 0 |
| DigiBandTicks, 1920 × 720 | 29.55 / 29.50 | 29.78 ms | 24 |
| DigiBandTicks, 1280 × 480 | 51.60 / 51.60 | 15.56 ms | 9 |
| DigiBandTicks, 960 × 360 | 59.90 / 59.85 | 11.47 ms | 0 |

Slow tracing retains only the worst 32 records per panel. These counts exclude
the first 64 draws and are not complete histograms. Cold draws reach 35–65 ms.
Lower resolution keeps the authored canvas and physical screen fit, but reduces
font/image detail. It is a **probe option only**; the live configuration remains
full resolution. Stable 60 fps with phone video and a strict startup deadline
remain open. The next gate is a realistic offline native-video fixture and a
real-phone comparison of the same layouts.

The heavy full-size layout spends about 18.74 ms waiting for the GPU, 5.23 ms
recording and 5.20 ms submitting. Its main panel needs only 1.15 ms but also
runs near 30 fps because the shared owner waits for the cluster turn.

### Options reviewed

| Option | Finding / next decision |
| --- | --- |
| Curves, smaller coverage textures, direct/cropped blur | Implemented; measured improvement without reducing blur quality. |
| Bypass repeatedly invalidated gradient caches | Rejected and experimental code removed. Three alternating same-binary runs made Cobranded medians 11.32–11.80 ms versus 10.72–11.23 ms with normal retention, and worsened p95. DigiBandTicks also worsened. |
| Lower internal render resolution | Measured above; a quality/performance choice, not enabled live. |
| Independent main/cluster render owners | Promising for main cadence; requires ordered UI-Sync, owner-safe scripts/messages and separate EGL ownership. Does not reduce cluster pixel/filter work or double the physical GPU. |
| Analytical GPU stroke coverage | CPU stroke raster averages only 0.27–0.50 ms here. Future precision-qualified work cannot by itself remove the measured 19 ms GPU wait. |
| Crop group working/mask surfaces | Further lead; every nested blur halo and global edge condition must survive. Current group inputs stay complete. |
| More buffering / deferred fences | Earlier live experiments gained throughput at a latency cost. Keep tested two-frame bench scheduling and synchronous completion. |
| More MSAA / distance-field paths | No demonstrated gain; current coverage-AA avoids known gauge-edge errors. Changes need independent pixel qualification. |
| Direct DRM/KMS | May remove final output composition, but does not eliminate expensive scene blur or the shared-owner delay. |
| Real-time Linux / CPU tuning | No current throttling. Scheduler changes cannot shorten GPU filter execution; measure CPU jitter before changing the OS. |

### Repeat the comparisons

Diagnostic flags `HEADUNIT_THEME_PROBE_LEGACY_BACKDROP_FILTER=1` and
`HEADUNIT_THEME_PROBE_FULL_BACKDROP_SURFACE=1` select the old copy path and
full-output allocation respectively. No asset is edited. GPU-only reports now
include warm median/p95 traversal, submission and completion timings.

The physical probe supports `--cluster-layout UUID` and
`--cluster-render-width {1920,1280,960}`:

```sh
XDG_RUNTIME_DIR=/run/user/1000 python3 tools/benchmark_physical_ui.py \
  --profile /absolute/private/installed-ui.conf \
  --main-output HDMI-A-1 --cluster-output HDMI-A-2 \
  --main-pending 2 --cluster-pending 2 --poll-ms 2 --seconds 20 \
  --cluster-layout 1B0484EC-1933-524D-80F8-2FC1FC7B4B2D \
  --cluster-render-width 1920 --pause-appliance \
  --output-dir captures/new-private-run
```

The probe restores previously active units in `finally` and never changes the
installed profile, phone advertisement or pack.

## Live baseline — 27 September 2026

The independent receiver ran the installed signed main and cluster scripts with
real iPhone HEVC video, synchronized phone time, hardware decoding and direct
GPU composition. Both outputs were virtual Wayland compositors because the
physical touchscreen was unpowered. No LIVI process was involved.

The bounded five-second reports measured:

| Stage | Main panel | Cluster panel |
| --- | ---: | ---: |
| Presented scenes per second | about 25–29 | about 15 |
| Mean complete scene composition | about 2.3 ms | about 26 ms |
| Mean final display submission | about 0.15 ms | about 0.16 ms |
| Mean Skia traversal/recording | about 0.04 ms | about 6.7–7 ms |
| Mean Skia flush/submission | about 0.22 ms | about 2.8–3.2 ms |

These are stage timings and compositor receipts, not camera measurements of
physical refresh or touch-to-display latency. They do not demonstrate 60 fps.
Both panels share one script/render owner, so an expensive cluster composition
can also delay main-panel work. Final display submission is not the dominant
measured cost; increasing a configured frame-rate number will not fix this.
The run continued receiving more than 16,000 video frames without a reported
post-startup clock loss or UI worker failure. This is one bench session, not a
long-duration reliability qualification or proof that visible flicker is gone.

## First frame-time improvement — 28 September 2026

Priority one is now animated-cluster rendering. With the receiver and Weston
stopped, the CM5's V3D 7.1 GPU drew the installed signed 1920 × 720 cluster
using simulated moving gauges and no phone video. The scene contains sixteen
masked gauge regions. Six are a single solid rounded-rectangle paint behind a
stroke-shaped mask. Those six now use a GPU coverage clip instead of two
intermediate GPU layers each. Complex/multi-paint masks keep the original
isolated-group renderer. The existing gradient cache remains enabled: disabling
it in a separate control experiment was slower.

An offline probe flag, `HEADUNIT_THEME_PROBE_DISABLE_SIMPLE_MASK_GROUPS=1`,
re-enables the old path only when `HEADUNIT_THEME_PROBE_GPU_ONLY` is also set.
Eight alternating runs of the same binary (four per path, each with 60 warm
animated frames) measured a median of run medians of **23.47 ms old** and
**18.01 ms optimized**, a 23% reduction. The optimized path ran six times per
frame. In the final rendered 1920 × 720 image, 206 of 1,382,400 pixels differed
and no channel differed by more than one 8-bit level. The separate GPU scene
test compares a transformed, translucent version of this mask to the CPU
reference. Neither result includes an iPhone map/video stream or physical
display presentation, so live throughput, flicker and latency are still open.

The probe's `HEADUNIT_THEME_PROBE_MASK_AUDIT=1` option prints only layer
structure and mask geometry, without asset source or pixels. It was used to
identify the six eligible groups; the private pack stays outside Git.

## Additive gauge-gradient fusion — 28 September 2026

The next profile counted 140 stroke-coverage builds during 63 frames, taking
24 ms **in total** (about 0.38 ms per frame), so stroke rasterization was not
the remaining bottleneck. Two additive masked gradients still created full
intermediate groups every frame. The renderer now applies each stroke-coverage
mask inside the gradient shader and draws the result with Skia's additive blend.
Only single-paint gradients with a simple stroke mask qualify; all other blend
and mask combinations retain their established group path.

`HEADUNIT_THEME_PROBE_DISABLE_ADDITIVE_MASK_FUSION=1` selects the old path only
inside the GPU-only offline probe. Six alternating runs of the same CM5 binary
(three per path, 60 warm animated frames each) measured:

| Animated 1920 × 720 cluster | Old path | Fused path |
| --- | ---: | ---: |
| Median of run medians | 17.74 ms | **11.99 ms** |
| Median of run 95th percentiles | 18.89 ms | **13.18 ms** |
| Highest warm frame across the three runs | 19.87 ms | **13.88 ms** |

The final image changed at 441 of 1,382,400 pixels, with a maximum difference
of one 8-bit level in any channel. An independent CPU-reference scene test also
covers the additive mask. Thus this **offline, no-video cluster** is under the
16.67 ms interval for 60 fps in all measured warm frames. It does not establish
60 presented frames/s with real phone video, synchronized main-panel work, or
the final display. Those remain the next performance gate.

Two additional runs paced the synthetic scene updates at 60 Hz (outside the
measured composition call). They measured 11.99 ms median, 13.18 ms p95 and
13.95 ms highest warm frame, consistent with the unpaced result. This still
does not include decoding, phone video import or display presentation.

## Other signed cluster styles — 28 September 2026

DigiRing also has two opaque, filled-path masks over a single paint. The same
coverage-clip approach now handles them. Two alternating runs of the same CM5
binary reduced DigiRing's median from 21.18 ms to 19.50 ms, with the 95th
percentile falling from 22.93 ms to 21.33 ms. The image changed at 1,365 pixels
out of 1,382,400, by no more than two 8-bit levels per channel. The GPU test
compares transformed and partially transparent fill masks against the CPU
reference; partially transparent masks remain on the original group path.
`HEADUNIT_THEME_PROBE_DISABLE_FILL_MASK_CLIP=1` selects the old path only in
the GPU-only probe.

One final no-video, moving-gauge run of each installed style measured:

| Style | Median | 95th percentile | Warm maximum |
| --- | ---: | ---: | ---: |
| Cobranded (selected default) | 11.74 ms | 13.03 ms | 13.67 ms |
| DigiRing | 19.63 ms | 21.34 ms | 21.53 ms |
| MapsMedia | 23.13 ms | 23.21 ms | 23.25 ms |
| DigiBandTicks | 48.38 ms | 51.29 ms | 51.72 ms |
| DigiBand | 49.40 ms | 51.50 ms | 53.54 ms |
| Modular | 58.46 ms | 59.71 ms | 59.79 ms |

Only the selected default clears the requested 16 ms composition target so
far. The other styles have different scene effects; DigiRing and MapsMedia
include masked backdrop blurs that still snapshot and process full-frame GPU
surfaces. Their output must be preserved before attempting a bounded-region
blur implementation. The larger styles also need separate scene profiles.
These figures omit real phone video, parallel main-panel activity and display
presentation; they are not projected live frame rates.

## Live iPhone check — 28 September 2026

The optimized selected Cobranded layout ran on the independent CM5 receiver
with a real iPhone, an Apple Maps route, hardware-decoded Ultra video, the main
panel on the physical touchscreen and the cluster on a headless 1920 × 720
Wayland output. The user confirmed the main picture and touch work. The full
1920 × 720 signed layout is fitted into the 1280 × 720 physical display, making
the picture look short. A subsequent inspection showed a separate defect:
the iPhone's 1920 × 720 source video itself contained overlapping icons and
unused height because the advertised physical size was 155 × 87 mm. Matching
that *logical* wide panel's aspect at 155 × 58 mm corrected the source layout;
the user confirmed the whole view and touch work. The cluster was not visible
to the user in this arrangement.

Ten consecutive five-second diagnostic intervals after startup showed about
**40 main** and **20 cluster** scene presentations per second. The cluster's
mean complete composition was **14.3–14.6 ms**, versus about 26 ms in the
27 September live baseline. The main averaged about 2.4 ms. Cluster scene
traversal/recording alone took about 6.2–6.5 ms. About 100 cluster owner turns
per five seconds found a prior presentation pending. The cluster's lifetime
composition maximum was 87.4 ms, including startup, so the 16 ms *maximum*
target is not yet met. Both panels reported one startup suspension, no later
clock-epoch/unavailable or delayed-feedback events, and no terminal display
fault in the observed interval. The receiver continued to accept navigation
updates and cluster video frames.

This establishes a live composition improvement, but **not 60 fps**. Both
panels share one script/render owner; their combined drawing, script work and
presentation gating need further measurement. The headless cluster also means
this session cannot verify visible gauge/map quality or flicker.

After correcting the logical panel's physical aspect to 155 × 58 mm, a fresh
live reconnect fixed the iPhone's icon layout and preserved touch. Its first
steady five-second reports showed about **20 presentations/s on each panel**,
about 2.3 ms mean main composition and 14.6–14.8 ms mean cluster composition.
Each panel also reported about 100 turns per five seconds with an earlier
presentation pending. This slower main rate is real; the 40/20 result above
belongs to the earlier, malformed iPhone layout. The available counters show
presentation gating and shared-owner scheduling, but do not yet separate their
individual time costs or prove which one sets the final rate.

### Shared-owner timing after the corrected panel layout

The same physical-screen plus headless-cluster arrangement was run again with
the iPhone connected. The new optional five-second owner report covers both
display calls and both script ticks, as well as scheduled 60 Hz slots missed.
Several steady reports measured **about 200–201 worker turns in five seconds**,
or **40 turns/s**, and about **100 missed scheduled slots** per interval.
The mean whole turn was 12.2–12.6 ms, but the longest turn in each interval was
20.5–22.1 ms. Averages hide the heavy/light alternation: both panels reported
about 100 turns per five seconds with a prior presentation pending, and each
presented about 20 scenes/s. This is a plausible explanation for the lower
rate, not proof that a particular compositor or GPU operation is responsible.

| Owner-thread stage, mean per worker turn | Main | Cluster |
| --- | ---: | ---: |
| Display call, including composition when one occurs | about 1.3 ms | about 7.5 ms |
| Script tick | about 0.27 ms | about 3.4 ms |

These per-turn means include turns that did not compose because a previous
scene was still pending. On actual submitted frames, the selected cluster's
complete composition was about 14.6 ms: around 6.5 ms in Skia traversal and
1.6 ms in Skia flush. The main composition remained about 2.3 ms. There was no
thermal throttle in this run (`get_throttled=0x0`). The measurements concern
software turns and compositor receipts, not camera-measured display latency.

A second live reconnect added validation and GPU-completion timings to the
same five-second report. In four steady samples, cluster composition stayed
near 14.5–14.6 ms, of which scene validation was about **0.21 ms**, Skia
traversal about **6.4–6.5 ms**, Skia flush about **1.7–1.8 ms**, and the
synchronous GPU fence wait about **5.7–5.8 ms**. These are nested sub-stages;
do not add them to the complete composition time. The main panel's fence wait
was about 1.7 ms. The shared owner still ran at about 40 turns/s, with roughly
100 missed slots per five seconds and maximum turns of 23.6–24.9 ms in these
samples. The cluster presented about 20 scenes/s. Its video continued to
advance; no terminal display fault appeared during the observed interval.

This identifies the synchronous fence wait as a substantial owner stall, but
it does not make the fence optional: native phone buffers must remain leased
until the GPU has finished reading them. An eventual overlap or deferred
completion design must preserve that lifetime and bounded failure handling.
The cluster's 6.4 ms CPU traversal also matters to the total 16.67 ms budget.

### GPU/script overlap and presentation receipts — 28 September

An opt-in `overlapScriptTick=true` profile starts the GPU fence, runs the next
script tick using the last **accepted** presentation timestamp, then waits for
the fence with the same absolute completion deadline. Native video buffers and
GPU textures remain leased until that fence completes. The normal setting is
false. Direct and Wayland pixel/lifetime tests, the signed offline scripts,
and a live iPhone reconnect passed with the option enabled; the main picture
and touch worked. This is still an experiment, not the default policy.

In repeated steady five-second live intervals, the shared owner completed
about **250 turns** (50/s), missing about **50 scheduled 60 Hz slots**, with
the longest turn generally 19–21 ms. The main panel presented about **30
scenes/s** and the virtual cluster about **20 scenes/s**. The cluster's mean
complete composition remained about **14.4–14.6 ms**, including roughly
**3.4–3.6 ms** of overlapped script work. The CPU-observed post-work fence wait
fell from roughly **5.8 ms** to **2.3–2.4 ms**. These nested measurements must
not be added together as separate frame costs.

Receipt timing isolates the remaining bottleneck. On presented scenes, the
cluster averaged **25.4 ms** from submission to the compositor's actual
presentation timestamp, followed by **9.9 ms** until the owner polled that
receipt. The main panel averaged **18.3 ms** and **12.9 ms**, respectively.
Weston reported a **16.67 ms** refresh interval. Thus the virtual cluster
often misses a refresh before it can submit the next scene, and polling adds
another delay. This is an inference from the timestamps, not a direct GPU
timestamp or physical-panel scanout measurement. It explains why reducing
the fence stall raised main throughput but did not lift the cluster above
about 20 presentations/s.

A second, short live experiment inserted an owner-thread receipt check about
3 ms after each scheduled turn. The iPhone picture and touch still worked,
but the headless cluster stayed near **20 scenes/s** and its
presentation-to-poll interval fell only from roughly **9.9 ms** to
**8.5–9.4 ms** across the steady reports. The extra check could consume a
receipt but could not start a new scene until the next scheduled turn; heavy
cluster turns also left little idle time for it. Its code and private profile
setting were removed. The result argues for reducing shared-owner work or
reworking panel scheduling before adding more receipt polls.

The next live session separated scene preparation and native video import
from scene traversal. With GPU/script overlap enabled, the cluster averaged
**14.7–14.9 ms** per completed composition. The mean scene traversal was
**6.5–6.6 ms**, GPU flush about **1.6 ms**, and post-overlap fence wait about
**2.4–2.6 ms**. Preparation, including cache pruning and the backdrop-path
prepass, averaged only **0.18–0.20 ms**; native video import averaged
**0.11–0.12 ms**. The virtual cluster still presented about **20 scenes/s**
and its submit-to-presentation delay remained around **25.4 ms**. The main
panel reached about **40 scenes/s** in this session, versus 30/s in the
earlier one, so its rate is not a stable benchmark yet. These stage numbers
are CPU wall times on the renderer owner, not GPU timestamps. They focus the
next renderer investigation on the actual scene traversal and its dynamic
groups rather than the inexpensive preparation/import stages.

## Diagnostics

### Long-session stability investigation — 28 September

The later CM5 session `session-20260927-224831` received about 18,800 video
frames before the cluster worker stopped with `GPU scene completion deadline
exceeded`. Its earlier periodic samples showed one startup suspension per
panel and no subsequent phone-clock-loss message. This identifies a terminal
GPU wait in that session; it does **not** prove that every visible flicker has
the same cause.

The display now counts clock unavailability, clock-epoch replacement, owner
scheduling gaps and delayed compositor feedback separately. The optional
diagnostics emit at most 16 event reports per panel when those counts change,
including a monotonic timestamp. A terminal display fault emits one numeric
snapshot. The compositor's existing completion deadline is unchanged; on
failure it now reports the fence result, draw time, wait time and limit. The
receiver's clock-transition log uses the same monotonic time domain. These
details let the next live test correlate a blank with timing, presentation or
GPU completion without recording phone content.

After a terminal Ultra worker or native-video failure, the receiver now closes
the stale AirPlay control connection. The prior long session continued
acknowledging feedback after its renderer had stopped, which prevented the
selected-phone reconnect helper from treating it as disconnected. A replacement
session still requires fresh authentication and stream setup; automatic return
to a visible picture has not yet been confirmed with an iPhone.

The private installed-asset profile's existing `diagnostics=true` enables at
most twelve reports per panel, five seconds apart, after initial presentation.
The reports now include interval presentation rate, mean composition and final
submission cost, lifetime maxima, turns waiting for a presentation, Skia phase
costs and cache hit/build deltas. They log no phone image, touch coordinates,
road names or asset source.

`GpuDisplayStats` counts completed compositions separately from successful scene
submissions and actual scene presentations. Composition can complete while a
clock transition prevents submission; none of these counters supplies script
PTS. The real-presentation, clock-quality and GPU completion limits remain in
place. The installed receiver still defaults to one pending frame; a bounded
two-frame mode is available for offline testing.

`GpuCompositor::compose_scene` additionally reports validation and GPU completion
wait time. Validation is outside its pre-existing `draw_ns` timing; completion
wait is included in draw time. The owner reports the full composition call's
wall duration. Skia recording/flush costs are sub-stages, not extra time to add
to the full composition figure. This is CPU-observed timing, not a GPU timestamp
query or a claim to separate every driver operation.

## Reproducible offline layout benchmark

`headunit-theme-probe` now labels its synthetic incoming UI messages with the
same asynchronous `comm` header as the live phone. Its old default header was
rejected by the corrected transport before it could reach layout measurement.
The private signed-pack handshake and both full entry points pass with the fix.

The GPU-only layout probe reports 60 warm samples after three initial frames,
cache counts and validation/recording/flush/completion-wait sub-stages. Set:

```sh
HEADUNIT_THEME_PROBE_GPU_ONLY=/dev/dri/renderD128 \
HEADUNIT_THEME_PROBE_GAUGES=1 \
HEADUNIT_THEME_PROBE_ANIMATED=1 \
build/headunit-theme-probe "$ASSET_DIRECTORY" "$ASSET_ID" "$ASSET_VERSION" \
  "$ACCEPTANCE_REFERENCE" --layout-local "$VEHICLE_PROFILE" "$OUTPUT_PREFIX"
```

All variables refer to private local files/settings. Keep outputs under ignored
`captures/`. Animated mode requires the local gauge profile and advances speed,
RPM and the explicitly synthetic script timeline between measured draws. Omit
`HEADUNIT_THEME_PROBE_ANIMATED` for frozen gauges. This is an offline fixture,
not real sensors or a phone session. Normal live script/scene budgets are not
relaxed; the probe retains its explicit offline two-second cold-draw limit.

The first experiments ran alongside the live receiver, so GPU contention affects
wall times. Use the same conditions for comparisons, and stop competing GPU work
for controlled absolute measurements. The main script without a phone does not
represent a populated live main panel.

## Rejected cache experiment

An experimental extension retained unchanged source-over layer groups on the
GPU after two identical observations. Pixel comparisons, alpha/mutation checks
and direct/Wayland rendering passed, but the signed cluster showed no useful
throughput improvement. The still-gauge baseline median was about 31 ms and the
moving-gauge baseline about 35 ms under concurrent live load; candidate results
were about 32–37 ms and 35–36 ms respectively. Extra cache admission checks also
increased the trivial offline main view's cost.

The extension and its experiment-only tests were removed. Existing specialized
masked-gradient caching remains unchanged. No live speedup is claimed, and the
candidate was never deployed to the phone receiver. Private binaries, logs and
captures remain outside Git.

## Validation

The final CM5 build passes direct and preview display lifecycle tests, GPU
pixel comparisons both offscreen and through Wayland, and the two-stream
native receiver-video test (5/5). Timing checks require populated, consistent
phase counters without imposing machine-specific speed thresholds. The actual
private signed pack also completes both frozen and animated offline probes.

## Next optimization gate

### Presentation-path measurements — 28 September 2026 (offline CM5)

The signed cluster was run for eight seconds with simulated moving gauges and
actual Wayland presentation feedback, but without phone video. Its source
layout remained 1920 × 720. The main panel in this fixture is nearly blank,
so main-panel rates do not represent a real CarPlay session. These are headless
Weston results, not measurements of a physical second display.

| Variant | Cluster presentations / 8 s | Mean cluster composition | Mean submit to actual presentation |
| --- | ---: | ---: | ---: |
| Single worker, default Weston repaint (7 ms) | 206–207 | about 12.3 ms | about 25.4 ms |
| Single worker, 1 ms repaint, receipt poll every 2 ms | 233–236 | about 12.2 ms | about 20.4–20.8 ms |
| Two independent worker processes, 1 ms repaint | 229 | 13.0 ms | 20.7 ms |
| Both panels on one worker, 1 ms repaint | 230 | 12.7 ms | 20.7 ms |
| 1280 × 480 internal cluster draw, then scaled | 254 | 7.5 ms | 22.6 ms |
| 960 × 360 internal cluster draw, then scaled | 256 | 5.9 ms | 24.1 ms |
| Deferred GPU fence, 1920 × 720 | 252 twice | about 7.6 ms | about 22.8 ms |

The cluster's own rendering and display handoff set the limit here. Merely
splitting the two scripts across workers did not change its rate. Reducing
rendered pixels or moving the GPU fence out of the submission path shortened
drawing but made the display handoff longer. A separate scene-only
precomposition attempt reached only 255 presentations; it made the handoff
slower and was removed. A negative Weston repaint window was slower too.

In the full-resolution moving-cluster baseline with 1 ms repaint, mean CPU
scene traversal was 5.6–5.7 ms, Skia flush 1.5–1.6 ms, and GPU completion
wait 4.5–4.6 ms. Stroke rasterization was about 0.3 ms per frame, so it is not
the dominant cost. The optional receipt poll and deferred-fence paths are
disabled in the installed live receiver. The deferred-fence path retains
borrowed native textures until an actual GPU fence signals; its direct-Wayland
native-video fixture passes, but it has not been tested with an iPhone.

Those results motivated the bounded two-frame presentation queue described
below. Any 60 fps claim still requires live phone video and a physical cluster
display; a no-video headless benchmark alone cannot qualify it.

### Bounded presentation pipeline — offline CM5 prototype

The virtual Weston output labeled 60 Hz reports a 16.67 ms nominal refresh,
yet even a 64 × 64 two-colour EGL fixture often presents its frames about
25 ms apart. Setting a separate **benchmark-only** headless Weston to
`--refresh-rate=120000` brings actual simple-frame intervals to about 15 ms.
Its advertised refresh is then 8.33 ms, so neither advertised value is a
reliable measurement of this headless display. Physical HDMI has not been
substituted or reconfigured.

The renderer now has an opt-in queue of at most two Wayland frames. Every
commit keeps its own token and `wp_presentation` feedback; a discarded
commit never produces a script timestamp. The video input records which
commit staged a decoded frame. If Weston discards that commit while the next
queued scene uses the same frame, its lease follows that next scene. A GPU
fence continues to protect borrowed hardware-decoder buffers. Normal live
configuration still uses one pending frame.

With the signed 1920 × 720 cluster and simulated moving gauges on the faster
benchmark output, one panel presented **280 frames in eight seconds** with the
old one-frame gate and **469** with two pending frames. Deferring the GPU fence
alone produced 332; combining it with two pending frames produced 473. In an
offline two-panel run on separate fast virtual outputs, one shared worker
presented 464 nearly blank main-panel frames and 454 animated cluster frames;
two separate worker processes presented 480 and 468 respectively. These
numbers exclude live iPhone video and do not validate 60 fps CarPlay Ultra.

Longer runs exposed the difference between average throughput and a reliable
16.67 ms deadline. With both panels on one worker for 60 seconds, the nearly
blank main panel presented 3,455 scenes (57.6/s) and the animated signed
cluster presented 3,449 (57.5/s). The cluster had 872 presentation gaps above
20 ms, 88 above 25 ms, and a longest gap of 52.9 ms. Running the cluster alone
for 30 seconds produced 1,789 presentations (59.6/s), with a longest gap of
77.4 ms. Its slowest composition was 65.7 ms: Skia traversal and flush each
peaked near 30 ms. Deferring the GPU fence in a separate 30-second cluster run
produced 1,792 presentations (59.7/s), but the longest presentation gap was
still 77.3 ms. It reduced average owner work, not that rare stall.

The virtual compositor itself has timing variation: a nearly empty panel on
its own reached exactly 1,800 presentations in 30 seconds, yet its longest
gap was 23.7 ms. Neither the two-frame queue nor a second worker proves an
unbroken 60 fps cadence. The next live test should compare one queued main
frame and two queued cluster frames on the actual outputs, then investigate
long Skia/GPU stalls separately.

The synthetic native-video tests pass with two pending frames, with and
without deferred GPU-fence cleanup. They cover hardware decoding, two routed
streams, per-frame presentation receipts and source-frame lease completion.
An iPhone session remains necessary to check live map video, startup,
touch response, physical output timing and sustained stability before the
two-frame queue is enabled in the installed profile.

For the slower layouts, continue measuring masked-backdrop and group work
against reference pixels. One sparse-backdrop candidate was slower in four
alternating Pi runs and was removed. For the selected Cobranded layout, the
GPU/script overlap experiment preserved frame leases and timing rules but
left the virtual cluster near 20 presentations/s. The detailed stage profile
now points to 6.5 ms of scene traversal: isolate dynamic retained groups,
backdrop processing and mask draws before changing the renderer. Then revisit
panel scheduling and presentation delay without fabricating presentation
times. The opt-in two-frame queue improved offline throughput but a longer
two-panel run still showed occasional presentation gaps above 50 ms. Record
frame-time distributions and visible quality on a physical cluster before
claiming a 16.67 ms maximum or sustained 60 fps.
Physical-panel and touch latency checks remain necessary after any measured
improvement.

### Slow-frame trace and bounded stroke cache — 29 September 2026

The CM5 screen and iPhone were unavailable, so these tests used the installed
signed 1920 × 720 cluster script, simulated moving speed/RPM, and two headless
GL/Wayland outputs. The CM5 reported `get_throttled=0x50000` throughout: the
current undervoltage/throttle bits were clear, while historical event bits
remain set from the earlier inadequate supply. No new kernel undervoltage
message appeared during the run. These figures cannot qualify physical HDMI
or live phone-video cadence.

An opt-in, content-free trace now records slow scene draws and large actual
`wp_presentation` gaps, including draw phases, cache work, and the time from
submit to presentation. The cold cluster's slowest traced draw took 55–64 ms
at startup: it built 8 retained groups and 11–14 stroke masks, with roughly
21–30 ms of scene traversal and 29 ms of Skia flush. In a later two-panel run,
only one warm cluster draw in the retained top-32 trace exceeded 16 ms, at
16.12 ms. Main draws were under the trace threshold; its large presentation
gaps therefore were not caused by drawing the main scene.

The stroke-coverage cache previously scanned and erased expired source paths
on *every* frame. It now keeps coverage by exact geometry and transform until
its existing 64-entry/64 MiB LRU budget needs space. This permits a new path
object with identical geometry to reuse the same coverage, while changed
geometry rebuilds it. In both orders of paired 15-second cluster-only runs,
mean composition improved by about 1.2–1.3 ms (12.08 to 10.88 ms in the
reversed-order pair). The two 63-frame animated main and cluster GPU snapshots
had identical SHA-256 hashes before and after the change. The GPU scene test
also checks equal-path reuse and mutation invalidation. A post-turn feedback
scheduling experiment did not reliably improve both panels and was removed.

With the final cache change, one shared worker, two pending frames and the
benchmark-only accelerated outputs, the 30-second run presented **1,705 main**
and **1,798 cluster** scenes. Cluster mean composition was 12.26 ms; the main
scene took 0.93 ms. The main had 129 gaps above 25 ms, the cluster 12. One
large main gap was followed by a 16.6 ms delay before the worker polled its
receipt; warm cluster gaps above 25 ms had about 15.4 ms from submit to actual
presentation. Splitting panels into separate worker *processes* in an earlier
offline run brought the nearly blank main panel to 1,801 presentations in
30 seconds, but a live single-session split also needs safe routing of the
shared UI channel. That architecture remains unimplemented and unverified.
There is no proven 16.67 ms maximum yet.

### Two physical HDMI panels — 30 September 2026

Both CM5 HDMI panels are now connected, under labwc on `wayland-0`. Main is
on HDMI-A-1 at 1280 × 720; cluster is on HDMI-A-2 at 1920 × 1080. The signed
scene still renders at 1920 × 720 and fits the output without stretching;
the taller physical output does not increase the scene framebuffer size.
The CM5 reported `get_throttled=0x0` before and after these comparisons.
The receiver and backup cluster were paused to remove competing work, and
restored afterward. No phone video is involved; the main fixture is nearly
blank, while the selected signed cluster animates simulated speed/RPM.

The probe now targets explicit physical outputs and uses each panel's usual
application identifier. Its per-panel queue options permit independent
main/cluster comparisons. Every count below comes from actual Wayland
presentation receipts, rather than a configured refresh rate.

| Queue / feedback policy | Duration | Main presentations | Cluster presentations | Cluster mean composition |
| --- | ---: | ---: | ---: | ---: |
| One pending frame each, normal polling, run 1 | 10 s | 298 | 298 | 11.78 ms |
| One pending frame each, normal polling, run 2 | 10 s | 581 | 299 | 11.50 ms |
| Two pending frames each, normal polling, run 1 | 10 s | 591 | 591 | 11.09 ms |
| Two pending frames each, normal polling, run 2 | 10 s | 576 | 576 | 11.19 ms |
| Main one / cluster two, poll every 2 ms | 15 s | 727 | 889 | 10.94 ms |
| Both two, poll every 2 ms | 15 s | 890 | 890 | 11.05 ms |
| Both two, poll every 2 ms, script/GPU overlap | 15 s | 892 | 891 | 11.21 ms |
| Both two, poll every 2 ms, deferred fence | 15 s | 894 | 893 | 6.48 ms |
| Both two, poll every 2 ms, sustained run | 60 s | 3,588 | 3,589 | 11.11 ms |

With one frame pending, the cluster stays near 30 presentations/s despite
an approximately 11 ms draw. Main throughput varies with receipt/scheduling
phase. Two pending frames plus the shorter receipt-poll interval reach about
59 presentations/s on both panels. All three both-two/2-ms 15-second runs
had no traced composition over 16 ms or presentation gap over 20 ms after
the first 64 draws; the complete gap counts were small enough to fit in the
trace's 32-entry retention limit. Cold compositions still peaked at roughly
52–67 ms across the four scheduling variants. This does not meet a strict
16 ms maximum from startup.

The 60-second run averaged **59.80 main / 59.82 cluster presentations/s**.
All nine cluster gaps above 20 ms occurred within the first 64 draws; the
main had one warm gap of 33.99 ms. Neither panel had a traced warm draw over
16 ms. Startup cluster composition peaked at 57.17 ms. This is stronger
physical-output evidence than the shorter runs, but still omits phone video
and does not establish an unbroken deadline or live touch latency.

Queueing trades throughput against delay. In the both-two/2-ms run, mean
cluster submit-to-presentation was 20.48 ms, followed by 2.12 ms before the
owner polled its receipt. Deferred fencing shortened CPU-observed composition
but increased submit-to-presentation to 25.52 ms; it is not an established
latency improvement. Composition times contain nested stages, and deferred
composition excludes GPU completion. Neither these figures nor the worker's
scheduled-turn counters measure touch-to-photon latency.

During these offline runs the **normal installed phone profile stayed unchanged**,
at one pending frame per panel and ordinary polling. The user's subsequent
reconnect confirmed both Ultra views return. The next gate was a reversible live
one-versus-two queue comparison with real phone map video, sustained receipt
counts and touch checks. The nearly blank main fixture cannot qualify real
CarPlay throughput. See the later live comparison below for the resulting
bench profile and the user's decision to defer further 60 fps work.

#### Reproduce the physical-display fixture

Run as the CM5 display user with the existing compositor and both outputs
powered. Use a new capture directory for each run:

```sh
XDG_RUNTIME_DIR=/run/user/1000 python3 tools/benchmark_physical_ui.py \
  --profile /home/headunit/.config/ultra-headunit/lab/installed-ui-main-touch.conf \
  --main-output HDMI-A-1 --cluster-output HDMI-A-2 \
  --main-pending 2 --cluster-pending 2 --poll-ms 2 --seconds 60 \
  --output-dir captures/physical-ui-run --pause-appliance
```

`--pause-appliance` disconnects the phone by stopping the receiver and backup
cluster, then restores only the units that were active when the run began,
including after probe failure or interruption. It leaves compositor/output
services and installed configuration intact. Without that option, stop
competing receiver/cluster work separately. The display user's service
management needs non-interactive sudo access.

The tool records a private log and a content-free `summary.json`. The summary
includes real presentation counts, draw stages, receipt delays, queue settings
and retained slow-frame traces. Traces retain at most the worst 32 entries per
panel, so they are not complete histograms. Presentation count divided by run
duration approximates throughput and includes startup; it does not certify
every individual refresh deadline. Private assets and captures stay outside Git.

### Installed UI factory and live deadline diagnostics — 30 September 2026

The actual `InstalledUi::factory` was then checked on the same physical outputs,
with the signed pack, registered native input queues and a synthetic clock/UI
peer, but **no phone and no decoded video**. Temporary copies of the installed
profile selected the two policies; the normal profile stayed intact. Each run
continued for 15 seconds after the worker entered its running state. The
receiver and backup were restored afterward.

| Factory policy | Main steady five-second reports | Cluster steady five-second reports | Cluster mean composition |
| --- | ---: | ---: | ---: |
| One pending frame each, normal polling | 29.90 / 30.09 fps | 29.98 / 30.02 fps | 10.97 / 11.27 ms |
| Two pending frames each, 2 ms polling | 60.02 / 59.69 fps | 59.66 / 59.43 fps | 10.61 / 10.74 ms |

This checks the production factory's option handling and diagnostics rather
than only the independent benchmark fixture. It does not reproduce live
hardware decoding or real clock/network load. The two-frame test still had
one warm main gap and three warm cluster gaps above 20 ms in its later report.
No warm draw exceeded 16 ms in either policy. Cold cluster drawing peaked
near 40 ms: about 21–22 ms traversal, 14 ms flush and 4 ms GPU completion,
with eight retained-group and eleven stroke-mask builds.

With `diagnostics=true`, the live factory now collects fixed-size,
content-free deadline windows. Each existing display report adds:

- `Ultra deadline diagnostic`: complete counts of draws at least 16 ms and
  presentation gaps at least 20 ms in that reporting interval, plus the counts
  after draw 64. “Warm” is this explicit cutoff, not a claim that every cache
  has finished warming.
- `Ultra slow draw`: the worst interval draw and, if different, worst warm draw,
  with validation, preparation, traversal, flush, GPU wait, import and cache
  work. Stages are nested within total composition.
- `Ultra long gap`: the worst interval gap and, if different, worst warm gap,
  with actual submit-to-presentation and presentation-to-poll delays, refresh
  interval and queued frames.

These callbacks retain no layer, image, asset or decoded frame, do no per-frame
logging, and reset their counters after each report. Display reporting remains
bounded to twelve intervals per panel per session by default;
`diagnosticReports=1..120` explicitly selects a longer bench window. The cold maximum therefore
no longer hides a smaller warm outlier, and complete interval counts avoid the
offline trace's top-32 truncation. Disabled diagnostics attach no callbacks.

The standalone factory probe accepts
`HEADUNIT_INSTALLED_UI_PROBE_SECONDS=1..20` (default 1) for bounded multi-report
checks. Its output explicitly says `syntheticClock=true` and
`phoneConnected=false`; these results must not be labeled a live CarPlay test.

### Bound backdrop blur output work — 30 September 2026

Backdrop layers previously filtered the full scene even when the replacement
covered only a small panel. The GPU path now bounds the **filter output** to
the transformed panel rectangle, with a two-pixel antialiasing margin. It
keeps the complete immutable source; Skia expands the input halo for the
filter chain. The final replacement still applies panel/ancestor clipping
once, preserving premultiplied alpha and sampling outside the panel. The
raster comparison path retains full-frame filtering.

Four interleaved runs per style (old/new/new/old) used animated gauges,
real-time pacing and 60 warm draws each. These runs kept the receiver and
native backup active while waiting for the phone; **no phone connected**.
They are a controlled implementation comparison with competing appliance
work, not the isolated physical scheduling fixture or live CarPlay evidence.
The CM5 reported no throttling. Times include GPU completion.

| Style | Old warm median, two runs | Bounded warm median, two runs | Old / bounded warm p95 |
| --- | ---: | ---: | ---: |
| DigiRing | 21.22 / 21.93 ms | 19.43 / 19.27 ms | 25.01–25.17 / 22.58–22.86 ms |
| MapsMedia | 25.54 / 25.83 ms | 19.54 / 19.18 ms | 27.75–27.92 / 22.05–22.47 ms |
| Cobranded, no backdrop filters | 11.56 / 11.73 ms | 12.08 / 12.16 ms | 14.50–14.61 / 15.38–15.42 ms |

MapsMedia and Cobranded final PNGs were byte-identical between policies.
DigiRing differed on 272 of 1,382,400 pixels, by at most four 8-bit channel
levels; mean absolute channel error was 0.000060. Both interleaved comparisons
produced the same difference. DigiRing applied one blur per draw, MapsMedia
two and Cobranded zero. MapsMedia's reported GPU resource-cache usage fell
from 63.65 MB to 52.43 MB; DigiRing rose from 99.81 MB to 102.37 MB, so this
is not a universal memory reduction.

The GPU scene tests verify outside-panel halo sampling, masks, nested opacity,
rounded/rotated ancestor clips and transparent replacement. A new chained
blur test crosses the framebuffer edge with a translated/scaled panel and
compares against full-frame Skia raster filtering (maximum channel error 4).
The simpler independent CPU renderer cannot reference global-edge tiling;
its existing supported comparisons remain in place. Both GPU scene and
Wayland scene suites pass on the CM5.

For probe-only A/B comparison, set `HEADUNIT_THEME_PROBE_GPU_ONLY` and
`HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_OUTPUT_CLIP=1` to restore the old path.
The switch is ignored outside that explicit probe mode. The probe JSON reports
`backdropOutputClips`. Existing work validation and frame leases are unchanged.
Neither blur-heavy style meets the warm 16 ms maximum yet. This change is
component-tested; live receiver qualification remains a separate step.

### Backdated preview receipts after a compositor stall

The CM5 recovery suite exposed a failure in the explicit CPU-readback preview:
after pausing its isolated compositor for 3.3 seconds, the compositor resumed
with several old repaint timestamps. The display ignored the first overdue
receipt but immediately cleared its preview-wait state. It then tried to map
the next backdated receipt, which correctly failed the strict one-second
presentation-age limit.

An observed preview outage now remains withdrawn through discarded or
backdated receipts until a recent actual presented receipt arrives. The next
bootstrap still needs its own qualified presentation before tasks resume.
Direct output and `PresentationClock` keep their existing timeout, clock-domain,
token and age checks; no stale timestamp is relabeled or converted into a PTS.
The CM5 direct-display, overlap, preview-recovery, presentation-clock and
two-stream native-video checks pass after the fix. The preview test also checks
that an actual feedback-delay suspension occurred, stale feedback was ignored,
and the resumed PTS did not regress. This is a tested preview recovery fix,
not evidence that all live Ultra faults recover.

### Bound masked backdrop replacement — 30 September 2026

For a backdrop-containing group with a bounded source-over mask, the final
replacement shader now runs only within the transformed mask support plus
two antialiasing pixels. Full background/working images preserve blur halos
and nested sampling. Unsupported mask bounds keep the old path; raster
comparison remains unoptimized. GPU-only probes can select the old path with
`HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_MASK_CLIP=1`, and report uses through
`backdropMaskReplacementClips`.

Old/new/new/old runs with moving gauges, 60 warm draws/run and the normal
receiver/backup active, **without a phone**, measured:

| Style | Previous warm median, two runs | Bounded warm median, two runs | Previous / bounded warm p95 |
| --- | ---: | ---: | ---: |
| MapsMedia | 19.36 / 19.67 ms | 16.72 / 16.67 ms | 22.28–22.32 / 19.43–19.84 ms |
| DigiRing | 19.07 / 19.90 ms | 18.30 / 18.29 ms | 22.96–23.36 / 21.20–21.54 ms |

Both style comparisons produced byte-identical final PNGs. CM5 GPU scene,
Wayland scene, rebuilt presentation, native-input and native-video fixtures
pass. Neither style meets a strict warm 16 ms maximum: bounded MapsMedia
peaked at 20.42–20.89 ms. This code was not linked into the live receiver
during the following scheduling comparison.

### Live iPhone, both physical outputs — 30 September 2026

The phone opened both native video streams with simulated gauges and the
backup service running; the CM5 reported no throttling. Each policy produced
eleven five-second rate reports per panel. The table uses the median of the
last nine interval means/rates, omitting the first two rate windows; these
are not per-frame percentiles or physical touch measurements.

| Policy | Main / cluster presentation rate | Main / cluster composition | Main / cluster submit-to-presentation |
| --- | ---: | ---: | ---: |
| One pending frame each, normal polling | 20.00 / 20.00 fps | 4.21 / 17.81 ms | 31.68 / 25.28 ms |
| Two pending frames each, 2 ms polling | 34.33 / 29.79 fps | 4.31 / 19.34 ms | 30.72 / 34.04 ms |

One-frame rates ranged 19.89–20.12 main and 19.96–20.04 cluster; two-frame
rates ranged 33.56–34.89 main and 29.38–30.68 cluster. Cluster native-video
presentation counts advanced to 651 and 608 respectively. The mostly idle
main sent few video changes; scene rate does not mean that many distinct
phone frames. Each panel had one startup owner-gap suspension, with no
increase during the report window. This does not qualify sustained stability.

The two-frame policy increased main presentation-to-poll delay from 14.06 to
23.05 ms and cluster submit-to-presentation by about 9 ms. Native phone video
does not reproduce the no-video fixture's 59–60 fps. Roughly 7.6 ms cluster
traversal and 9.2 ms completion wait remain substantial; a 2 ms polling setting
cannot interrupt a long draw already executing on the shared owner.

A deferred-fence session passed the user's picture/touch check but initially
stayed near 31 fps with roughly 50–53 ms submit-to-presentation, and later
varied as low as 22–29 fps. Its shorter CPU draw excludes GPU completion.
It is **not** a latency improvement and was turned off. A backup-service A/B
was aborted after receiver startup stalled; no timing conclusion is drawn
from it. The backup's observed 19% CPU use is not proof it causes the limit.

The user accepts approximately **30 fps for this phase** and has deferred
further tuning to functional features. The CM5 bench keeps two frames/2 ms
polling with deferred fencing off; source defaults remain one frame/normal
polling. Stable 60 fps, a cold-start 16.7 ms maximum and measured physical
touch-to-photon latency remain unachieved.
