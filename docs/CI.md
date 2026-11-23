<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Local and hosted component checks

Hosted CI is paused for GitHub billing. Commit subjects use `[skip ci]`;
qualification records local evidence instead. The local entry point is
[`tools/ci/run-local.sh`](../tools/ci/run-local.sh), which reproduces all six
jobs in [the hosted workflow](../.github/workflows/ci.yml). It never connects to
the CM5, starts an appliance service or uses private phone/assets/identities.

## Prerequisites on macOS

Install the same portable dependencies as the macOS workflow and the pinned
formatter before running checks:

```sh
brew install cmake ninja pkg-config libplist libarchive cairo libpng openssl@3
pipx install clang-format==19.1.7
```

The runner uses installed Homebrew packages; it does not install or upgrade them
underneath other builds on a shared Mac. macOS 26 or newer and a compatible SDK
are needed for the workflow's `CMAKE_OSX_DEPLOYMENT_TARGET=26.0`. The actual
host version is recorded in the macOS log; a different host version is not
evidence from the hosted macOS 26 image.

Provide a running Docker engine (Docker Desktop or Colima) with several GB of
memory and disk space. The runner uses the current Docker context and always
requests `linux/arm64`, matching `ubuntu-24.04-arm` with `debian:trixie`. On an
Intel Mac this requires Docker's arm64 emulation and is slower. For example,
an isolated Colima profile on Apple Silicon can be started with:

```sh
brew install colima docker
colima start --profile headunit-local-ci --cpu 4 --memory 8 --disk 30 --arch aarch64 --vm-type vz
docker --context colima-headunit-local-ci info
```

Use `DOCKER_CONTEXT=colima-headunit-local-ci` when invoking the runner to select
that profile without changing the default context. The runner does not start a
VM or change Docker contexts itself. First use needs network access for Debian
packages, pinned Luau and Skia. Budget disk space for the Debian dependency image,
Skia volume and retained macOS builds.

## Run and inspect

```sh
tools/format.sh
tools/ci/run-local.sh
# One job, or lower parallelism on a busy development machine:
tools/ci/run-local.sh --job linux-sanitizers --jobs 2
tools/ci/run-local.sh --output /absolute/path/to/new-evidence-directory
```

The default runs jobs sequentially with `-j4` within each build/test, as CI does.
`--jobs` changes only that parallelism. Jobs keep running after an earlier
failure. Exit zero means every selected job passed; a failed job makes the
runner exit one after writing `summary.md`, with each job's pass/fail, exit code
and absolute log path. An unavailable dependency or Docker daemon is a failure,
not a passing skipped job. `--help` lists the supported options/job names.

`--job` requires exactly one supported nonempty name. `--jobs` accepts positive
ASCII decimal input, including leading zeros, and rejects every all-zero form
(such as `0`, `00` and `000`) without arithmetic conversion or evaluation.
Invalid inputs exit two before creating output directories or snapshots.
Run the focused CLI checks with `python3 tests/run_local_cli_test.py -v`; they
check early rejection and a real format-only job without starting Linux jobs.

The default output is `build/local-ci/<UTC timestamp>-<PID>/`. An explicit output
directory must not already exist. Each run retains a Git-tracked working-tree
snapshot, the source commit, tracked changes relative to that commit, workflow
SHA-256, job logs, the macOS build and Linux `Testing/` directories when created.
Stage new files with `git add` before running; edits and deletions of tracked
files are included. Untracked/ignored builds and private files are excluded,
and tracked symlinks are rejected. Do not edit the snapshot during a run.

| Job name | Configure/build | Test policy |
| --- | --- | --- |
| `format` | clang-format **19.1.7**, `tools/format.sh --check` | Exact pinned version required. |
| `macos` | Debug, Ninja, deployment target 26.0, libarchive/OpenSSL 3 Homebrew prefixes | Portable ctest, `--repeat until-pass:3`, matching CI's VM stall policy. |
| `linux-gcc` | Debian trixie GCC, Debug, `HEADUNIT_BUILD_THEME_VIDEO=ON` | All default Linux tests, no retries. |
| `linux-sanitizers` | Clang 19, Debug, video enabled; ASan+UBSan flags below | All default Linux tests, no retries. |
| `receiver-gcc` | GCC, RelWithDebInfo, video+GPU enabled, pinned Skia | Full receiver build; ctest `-LE hardware`, no retries. |
| `receiver-asan` | Clang 19, RelWithDebInfo, video+GPU enabled; ASan flags below | Full receiver build; ctest `-LE hardware`, no retries. |

The portable Linux sanitizer job passes exactly
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`
to C/C++ compilation and executable/shared linking. The full receiver sanitizer
job uses exactly `-fsanitize=address -fno-omit-frame-pointer`, as hosted CI does;
the shared Skia archive itself is not sanitizer-instrumented.

Both Linux sanitizer jobs preserve leak checking and use exactly
`LSAN_OPTIONS=suppressions=/home/builder/src/tools/ci/lsan.supp`. The only
suppressions are the existing fontconfig process-lifetime cache and
`cairo_toy_font_face_create`. No warning exceptions, disabled leak detection or
Linux retry policy are added.

## Startup CPU-budget fixture — 13 October source

`startup_task_budget` must consume more than the normal 5 ms **thread CPU**
limit after its separately budgeted initialization. Its old 12 ms uptime loop
could finish while descheduled, consuming less than 5 ms CPU and missing the
expected rejection. An isolated Linux diagnostic reproduces that subsequent
normal-tick failure in ten runs. The fixture now uses the same actual
`CLOCK_THREAD_CPUTIME_ID` workload as the existing native-callback test (12 ms
for startup, 10 ms for normal native callbacks). The 5/100 ms budgets, production
100 ms wall ceiling and fault assertions are unchanged. Rejection failures report
the call-site line. No production VM deadline or retry policy is changed.

Original default GCC and ASan+UBSan runs retain 184/185 results, with only this
fixture failing; corrected default-policy component checks pass8/8 each and
full Skia GCC/ASan pass186/186 each. These remain separate evidence. Private logs/source are in `20261013/codex-cluster-signals/`.

## Linux ownership, caches and retirement

Docker builds a Debian trixie dependency image with the same
[`install-debian-deps.sh`](../tools/ci/install-debian-deps.sh). Its cache tag
includes that script's SHA-256. A read-only source snapshot is mounted at
`/workspace`; the hosted [`run-as-builder.sh`](../tools/ci/run-as-builder.sh)
copies it to `/home/builder/src`, assigns it to the unprivileged `builder` account
and removes other-account write permissions. Configure, build and ctest all run
there with `umask 022`; the container uses `TMPDIR=/tmp`. Root only prepares
dependencies and Skia. No privileged container, host device or GPU socket is
mounted.

Skia uses the exact hosted `build-skia.sh`, with a volume cache keyed by
`cmake/Skia.cmake`, `cmake/skia-pi.gnargs`, the builder script and arm64 platform.
A file lock serializes first builds of a shared cache. CMake validates the pinned
revision and argument receipts before compiling the receiver. The Debian cache
is reused until its recipe changes or it is explicitly removed; package versions
are recorded in logs and can differ from a future fresh hosted apt install.
Remove the specific dependency image to refresh it; remove the specific Skia
volume if its build is damaged. List only this runner's caches with:

```sh
docker image ls --filter 'reference=headunit-local-ci:*'
docker volume ls --filter 'name=headunit-local-ci-skia-arm64-'
```

After collecting `Testing/` logs, each Linux container is removed on success,
failure or a handled interrupt. Dependency images and Skia volumes are retained
for reuse; evidence directories are retained for review. Remove only the chosen
run directory and its specifically named caches when no longer needed. To stop
the example VM after testing, use `colima stop --profile headunit-local-ci`.

## Heartbeat assertion and evidence boundary

`presentation-status` uses the production `CLOCK_MONOTONIC` publisher. An
unchanged update can legitimately publish when a sanitizer or loaded runner
pauses for at least 250 ms. The test validates the actual publication interval,
clock bounds and one sequence increment, instead of requiring identical bytes
from two consecutive calls. It also waits beyond the heartbeat interval and
requires renewal, and applies the same check to unchanged negotiated mode.
After normalizing only sequence and publication date, the complete wire payload
must remain unchanged, including `returnReady` and `negotiatedMode`. Negative
fixtures alter each field after a genuine due heartbeat and require rejection.
No injected clock, fabricated receipt or production timing change is needed.

The first strict Docker run also exposed two production faults. The installed
UI profile reader now includes ownership, mode and link count in its before/after
identity, rejecting permission changes even when filesystem dates stay equal.
On Linux the manager's legacy socket liveness probe pins the original inode with
`O_PATH` until inspection/unlink completes; a replacement listener cannot reuse
that inode and evade its generation check. Other read/owner fixtures now wait
for an observed filesystem date change, bounded to one second, before expecting
mtime/ctime-based rejection. They perform real writes/chmod operations and do
not assign synthetic dates or disable failure assertions.

### Component evidence — 9 October 2026

The source snapshot for `b72fe31` passed the six jobs below on an arm64 Mac
running macOS 27 (deployment target 26), with Debian trixie arm64 in Docker.
No GCC bench warning exception or disabled Linux leak detection is used.

| Job | Result |
| --- | --- |
| Pinned format check | PASS |
| macOS portable | 141/141 |
| Debian GCC Debug/video | 165/165 |
| Clang 19 ASan+UBSan Debug/video | 165/165 |
| Full Skia receiver GCC RelWithDebInfo | 166/166, hardware tests excluded |
| Full Skia receiver ASan RelWithDebInfo | 166/166, hardware tests excluded |

The consolidated per-job summary and logs are retained outside Git under
`~/.cache/headunit-qualification/20261009/local-ci/summary.md`.
The host runner was interrupted by the manager during the full GCC job; that
isolated Docker container completed with exit zero. Its entire container log,
exit status and CTest evidence were recovered before removal. The remaining
ASan receiver job passed through `--job receiver-asan`. This is evidence across
those runs, not an uninterrupted six-job invocation. Initial failures and the
corrective runs remain retained. A deliberate unavailable-Docker-daemon check
returns failure with its log path rather than reporting a skipped pass.

Before the resource pause, isolated CM5 heartbeat/readiness checks passed 3/3
with GCC and 3/3 with ASan+UBSan using the existing Linux LSan suppressions.
The later expanded CM5 run was interrupted during GCC compilation: none of
its eight suites ran and no expanded-suite pass is claimed. The manager accepted
the strict Docker matrix for the Python Linux repairs, so those unchanged CM5
targets need no rerun. After confirming no surviving own process, the remaining
31 MB workspace was archived off-device, then only
`/home/headunit/headunit-local-ci-20261009` was removed. Archive, digest and cleanup
receipts are retained under `local-ci/review-fixes/`; other workspaces and shared
dependencies remain untouched. No CM5 builds, tests or live actions ran during
cleanup. The dedicated Docker VM remains stopped and `desktop-linux` is restored.

The subsequent review corrections have focused Mac evidence: the runner's CLI
checks pass 4/4 (all-zero/malformed decimal bounds, empty/unsupported job names,
and a valid format-only selection); `presentation-status` passes 1/1, including
both semantic-field regression cases; pinned formatting and syntax/whitespace
checks pass. Logs are retained under `local-ci/review-fixes/`. These results do
not extend the earlier full-matrix snapshot to the reviewed or integrated tree;
the manager owns integrated-tree requalification.

### Integrated seven-worker candidate — 9 October 2026

The clean manager source `6b63a5e` integrates local-ci, boot-speed, architecture,
protocol-gaps, greeting, audio and hardware-contracts from base `c8622e8`.
One uninterrupted serial invocation of `tools/ci/run-local.sh --jobs 4` passes
all six jobs on that exact tracked snapshot:

| Job | Result | CTest elapsed |
| --- | --- | --- |
| Pinned clang-format 19.1.7 | PASS | — |
| Mac portable, deployment target 26.0 | 144/144 PASS | 26.87 s |
| Debian trixie arm64 GCC Debug/video | 169/169 PASS | 28.35 s |
| Clang 19 ASan+UBSan Debug/video | 169/169 PASS | 31.20 s |
| Full Skia receiver GCC RelWithDebInfo | 170/170 PASS | 27.89 s |
| Full Skia receiver ASan RelWithDebInfo | 170/170 PASS | 28.53 s |

The Mac host is macOS 27 arm64; no Mac test retry was needed. Linux uses the
workflow flags, enabled leak checks and unchanged external LSan suppressions;
receiver jobs exclude hardware-labelled tests. No GCC bench exception is used.
Both audio admission/output-retirement and resource-permission test cases are
retained, as are terminal presentation retirement and real-clock heartbeat checks.
The final documentation-only receipt commit changes no compiled or test input.

The source snapshot, workflow hash, summary, six job logs and CTest artifacts
are retained outside Git under
`~/.cache/headunit-qualification/20261009/agent-manager/final-matrix/`.
The manager status/live queue is `codex-threads/STATUS.md` under the same dated
qualification root. Containers were retired; the dedicated CI VM is stopped
and the prior `desktop-linux` context restored. Worker CM5 GPU/GTK and sanitizer
results remain separately scoped in their topic guides. Main, selected releases,
phone state and live services were not changed by this integration.

Passing this matrix is portable/Linux component evidence, including compilation
of the full Skia receiver. It does not prove actual GPU presentation, sustained
frame rate, either phone mode, audio, car providers or a sealed deployment.
Hardware-labelled and opt-in private-input fixtures still require separate
bounded qualification under the live session owner. The runner and fixtures need
no phone test; adopting the production reader/socket repairs still needs the
normal retained-candidate qualification and grants no deployment authority here.

For the live owner, a bounded follow-up when adopting those repairs is: record
the new sealed candidate/source and retained rollback; observe the five services
and initial launch-bound readiness; allow one selected-phone connection up to
30 seconds; verify the phone-negotiated mode, both Ultra panels and main touch;
perform Home then Back with a 10-second limit per action, checking fresh readiness
and retained connection generation; Disconnect with a 10-second limit and verify
native main/cluster fallback; then make one Connect attempt, limited to 30 seconds,
and verify fresh successor readiness, both panels and touch. Stop on any timeout,
missing panel, stale readiness or owner restart; do not inject filesystem races
into the live appliance. Keep the whole run under five minutes and record actual
receipts/errors. This is a plan, not live evidence; Standard remains separately
qualified only when the phone negotiates it in an explicit owner-controlled run.

### Combined cluster and seven-worker source — 9 October 2026

Clean source `6b89b85` additionally integrates the exact native presentation-clock
repair, OpenSSL 4 compatibility, bounded HEVC capture conversion, grouped vehicle
notifications and the signed script's complete transmission updates. The
manager's final documentation receipts are merged. One uninterrupted six-job
local run passes on this tracked source snapshot:

| Job | Result | CTest elapsed |
| --- | --- | --- |
| Pinned clang-format 19.1.7 | PASS | — |
| Mac portable, deployment target 26.0 | 145/145 PASS | 27.65 s |
| Debian trixie arm64 GCC Debug/video | 170/170 PASS | 28.65 s |
| Clang 19 ASan+UBSan Debug/video | 170/170 PASS | 31.50 s |
| Full Skia receiver GCC RelWithDebInfo | 171/171 PASS | 27.95 s |
| Full Skia receiver ASan RelWithDebInfo | 171/171 PASS | 28.50 s |

The Mac host is macOS 27 arm64. Linux uses the exact workflow warning/sanitizer
flags with leak checking and unchanged external suppressions; no GCC bench
exception or Linux retry is used. Receiver jobs exclude hardware-labelled tests.
The working tree was clean when captured, and later documentation does not alter
compiled/test inputs. Source snapshot, patch receipt, workflow hash, summary,
logs and CTest artifacts are outside Git under
`~/.cache/headunit-qualification/20261009/takeover-testing/final-matrix/`.
All job containers were removed, the dedicated VM stopped and the prior
`desktop-linux` context restored. Hosted CI remains paused for billing.

This matrix qualifies portable/Linux components and the complete receiver build;
CM5 GPU/native fixtures, retained deployment and real-phone behavior require
separate evidence in [HANDOFF](HANDOFF.md). The software MFi phone trial is
separately scoped in [the MFi guide](MFI_COPROCESSOR.md#real-phone-trial--9-october-2026).
