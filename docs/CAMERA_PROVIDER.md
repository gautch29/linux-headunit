<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Supply local camera frames

The camera foundation supplies original local frames, keeps their capture time, and protects their storage until a display finishes using them. A standalone Linux GPU owner can present a configured local camera view without an iPhone. An explicit Linux V4L2 adapter is implemented; physical capture, DMA-buf import and phone-side Ultra camera transitions still require qualification.

As of **4 October 2026**, the isolated Standard/native handoff has passed on the
CM5 with generated frames and real GPU receipts. The control transport has also
passed actual Linux process/socket checks. Neither result qualifies a camera
device or the connected Ultra feature. Use [the Ultra guide](CAMERA_ULTRA_GUIDE.md)
for the signed scene/phone-state bridge and [Standard output](STANDARD_VIDEO_OUTPUT.md)
for the separate native handoff.

## Overview

The provider API is declared in [`camera.hpp`](../include/headunit/camera.hpp), with presentation helpers in [`camera_display.hpp`](../include/headunit/camera_display.hpp):

| Component | Responsibility |
| --- | --- |
| `Configuration` | Local stream identity, capture format, destination panel and rectangle, orientation, freshness, and activation policy. |
| `Provider` | Produce a capture using the local monotonic clock. `SyntheticProvider` implements an obvious moving test pattern. |
| `CaptureProvider` | Own one capture worker, reject invalid acquisition metadata, drop old frames and publish only the newest bounded sample. Its presentation poll performs no driver work or pixel conversion. |
| `CaptureDevice` | Replaceable capture-driver boundary. The Linux implementation selects exactly one configured V4L2 device and never silently changes delivery method. |
| `Queue` | Keep one newest waiting capture, protect two pending submissions, and advance the current frame only after a real presentation receipt. |
| `Session` | Enter a manually requested camera view, save the previous main-view identifier, and return it on exit. |
| `SceneBuilder` | Fit the capture with rotation/mirroring, create cached original simulation/status labels, and provide an independent native overlay. |
| `NativeDisplay` | Compose through the existing GPU renderer and apply actual Wayland presentation receipts in the local clock domain. |

The capture producer may publish to `Queue` from another thread. A single presentation owner calls `stage`, `complete`, and `current`. It must retain each staged frame through its GPU fence. Neither the provider nor the queue touches a script VM, a compositor, or phone state.

## Configure a source

Start with [`camera.example.json`](../config/camera.example.json). It is **disabled** by default. Enable it explicitly for a bench test.

[`camera-ultra-reference.example.json`](../config/camera-ultra-reference.example.json) is a second disabled template using the inspected reference asset's `camera` input name and its 1650 × 720 camera zone at x=135 on the 1920 × 720 main canvas. This records authored geometry; it does not register or activate a phone stream.

The JSON reader rejects unknown or duplicate fields, absent required fields, non-integer dimensions, unsupported rotations, and oversized input. `synthetic-test-pattern` and `external` sources contain exactly one field, `kind`. `external` is a producer interface; it does not select a device or implement capture.

[`camera-v4l2.example.json`](../config/camera-v4l2.example.json) is a **disabled** Linux device template. A V4L2 source has exactly these four fields:

```json
"source": {
  "kind": "v4l2",
  "device": "/dev/video0",
  "pixelFormat": "yuyv",
  "delivery": "dma-buf"
}
```

| V4L2 source field | Contract |
| --- | --- |
| `device` | Canonical `/dev/videoN` path. `N` is decimal 0–4294967295 with no leading zeros. Relative paths, dot components, shell syntax and `/dev/v4l/by-id` aliases are rejected. The runtime driver opens the selected node with `O_NOFOLLOW`; configuration parsing never resolves or opens it. |
| `pixelFormat` | `yuyv`, `rgb24` or `bgr24`. YUYV requires an even capture width. The device must support the requested format; this setting is not a conversion fallback. |
| `delivery` | `dma-buf` or `cpu-copy`. DMA-buf delivery initially accepts only YUYV. RGB/BGR24 require explicit CPU-copy delivery. No automatic change of format or delivery is allowed. |

Schema validation and the appliance's `--validate-only` path do no capture-device I/O, even with `enabled=true`. Runtime activation separately starts the capture worker, which checks device identity, capabilities and negotiated format. A valid file does not prove that a device exists or that GPU import works. The installer continues to admit camera ownership only through its existing guarded renderer policy; selecting V4L2 does not enable Standard camera-service admission.

| Field | Meaning and limits |
| --- | --- |
| `streamIdentifier` | Local ASCII identity, up to 64 letters, digits, periods, underscores, or hyphens. This is not an Apple resource ID. |
| `capture.width`, `capture.height` | Capture dimensions, 64–1920 by 64–1080 pixels. |
| `capture.frameRate` | Expected cadence, 1–60 frames per second. The synthetic source emits at most one frame per turn. |
| `capture.maximumAgeMs` | 30–1000 ms; at least one configured frame interval. This is a freshness policy, not a promised camera latency. |
| `destination.panel` | `main` or `cluster`. Selecting a panel does not create or move a display. |
| `destination.canvas` | Output-coordinate canvas, up to 4096 × 2160. |
| `destination.frame` | A bounded rectangle inside that canvas. |
| `destination.rotationDegrees` | 0, 90, 180, or 270. |
| `destination.mirrorHorizontal` | Mirror the oriented image horizontally in the native scene. |
| `manualActivation` | Whether `Session::enter` may activate the local camera view. |

The provider returns the untransformed capture. `SceneBuilder` fits the complete image inside the destination rectangle while preserving its aspect ratio, applies clockwise quarter-turn rotation, then mirrors horizontally in the oriented output coordinates when requested. These transforms affect the GPU scene; the original capture pixels remain immutable. Ultra's asset-driven adapter must independently validate its geometry and supported transforms.

## Capture from Linux

[`camera_capture.hpp`](../include/headunit/camera_capture.hpp) separates the asynchronous owner from the driver. `make_provider` creates the source selected by the file only on activation. `external` creates no device adapter; its producer remains responsible for publishing frames. V4L2 requires Linux. No source is discovered or selected automatically.

The adapter accepts a single-planar streaming capture node, progressive packed images and explicit supported SDR color metadata. Requested dimensions and pixel format must be returned unchanged. The actual rational frame interval must be within 5% of the configured cadence and no faster than 60 fps. Interlaced, compressed, HDR, unknown-color, memory-to-memory and output devices are rejected. Full-range sRGB RGB/BGR24 requires the explicit CPU-copy route. YUYV accepts BT.601/709 matrices and full/limited quantization.

The driver allocates two to four MMAP capture buffers, each no larger than 32 MiB. `dma-buf` exports YUYV backing and publishes a linear native plane with its actual stride and producer lease; it performs no CPU pixel copy. Import compatibility must still be checked on the GPU. `cpu-copy` uses the negotiated packed rows to produce independently owned opaque ARGB32 pixels on the capture worker. Row padding and trailing allocation padding are never interpreted as pixels. CPU delivery is a configured alternative, not a recovery fallback. V4L2's mappings use the [required read/write mapping permissions](https://docs.kernel.org/userspace-api/media/v4l/mmap.html); consumers see immutable spans and a buffer is never requeued while leased.

Each valid sample must carry the driver's explicit monotonic timestamp and original end-of-frame or start-of-exposure reference. Unknown/copied timestamps, zero/future/non-increasing acquisition times, reference changes and repeated/reversed driver sequences retire the source generation. A forward 32-bit sequence wrap is permitted. End-of-frame and start-of-exposure measure different acquisition events; compare physical latency only with the recorded reference. See the [Linux timestamp contract](https://docs.kernel.org/userspace-api/media/v4l/buffer.html).

Dequeue is nonblocking and examines at most the allocated buffer count per turn. A normal no-frame result changes no visibility or capture age. Corrupted driver samples are dropped without qualifying their pixels, timestamps or sequence numbers; a subsequent genuinely valid frame may recover. Stale samples are discarded. Only the newest valid sample is copied/imported, and its original acquisition age is checked again after conversion. Holding four presentation leases causes backpressure without growing an unbounded capture queue.

## Stop and retire a source

Only the capture worker opens the device, negotiates, dequeues, requeues and stops streaming. A sample's final release posts an atomic return bit; the worker alone requeues it. Outstanding mappings and exported descriptors survive stream stop until their last immutable lease is released. Resource finalizers close/unmap backing without a driver ioctl.

On deactivation, presentation owners withdraw visibility, pending scenes and the camera surface first, then call `Provider::request_stop()`. They retain at most one retiring provider and collect it only after `stopped()` acknowledges driver cleanup. New activation waits for retirement and discards any final offer from the old activation before beginning a new one. This keeps driver shutdown out of ordinary display turns.

A live dequeue/reclaim fault marks the driver generation terminal and throws
before streaming teardown. The capture worker closes the Queue and publishes
failed state before calling the potentially blocking driver stop. Display
owners can therefore withdraw camera pixels promptly while `stopped()` remains
false until cleanup actually finishes. The adapter rejects further take/reclaim
calls on that failed generation without another device syscall. Partial startup
still cleans up its unpublished device resources, and held GPU backing survives
both fault withdrawal and later driver cleanup.

The source-only input retirement correction also covers failure after the last
display pump. `LocalInput::composition_fresh()` checks retained source
availability as well as the selected capture's original age, so the existing
post-composition withdrawal/redraw guard can remove a terminal source before
swap. A receipt must be accepted by the Queue before it advances the independent
Stream. If retirement already removed that token, the input withdraws its current
and all prepared frames and publishes false; independently held native backing
survives until its consumer releases it. This does not erase pixels already
presented or impose a new driver/GPU deadline.

`O_NONBLOCK` and bounded interrupted-call retries do not impose a deadline on a wedged kernel ioctl. A delayed driver therefore prevents another capture generation; final process teardown can still wait for it. Supervised recovery remains unqualified. A terminal source fault closes its Queue permanently. For configured synthetic or V4L2 sources, an explicit off → on transition creates a new provider/Queue only after the previous provider acknowledges retirement. The in-session Ultra input retains its registered Stream, increasing receipt tokens and capture-to-phone clock mapping; it replaces only the owned source. Held On does not retry a terminal failure. Late old receipts cannot advance the replacement, and outstanding GPU leases retain their original backing. Externally owned queues remain terminal. Neither native nor Ultra recovery rewrites capture timestamps.

`CaptureProvider::statistics()` distinguishes dequeued, superseded, corrupt,
stale, published and backpressured samples, negotiated format, state and a bounded
terminal error. Its last validated dequeue and last successful publication retain
the original acquisition time/reference, driver sequence and real local worker
observation time. Maximum acquisition-to-dequeue and acquisition-to-publication
ages separate the original source delay from CPU copy/adaptation work. Dequeue
timing includes valid stale samples; publication timing advances only after an
accepted Queue offer. Missing, corrupt, stale, backpressured and retired turns
never renew that publication date. These retained records describe historical
source events; readiness still belongs to the Queue's actual current age/state.

`sequence_gaps` counts missing sequence values between validated driver samples,
including intervening samples discarded as corrupt. The first sequence is an
unknown baseline; a normal forward 32-bit wrap contributes no gap by itself.
Neither this counter nor a dequeue proves that the camera acquired an exposure.
Those are source-owner measurements, not physical presentation or end-to-end
latency.

The Linux `headunit-camera-capture-probe CAMERA.json` reports these bounded
diagnostics after capture-worker retirement, retaining its existing explicit
`physicalCapture=true`, `displayQualified=false` labels. `meanAgeMs` and
`maximumAgeMs` measure original acquisition age at probe Queue consumption;
`maximumDequeueAgeMs` and `maximumPublicationAgeMs` come from the capture worker
and include different events; their maxima may describe different captures.
`lastDequeue` and `lastPublication` record the genuine monotonic dates and
reference, or are null when absent. Compare their observations only when the
driver sequence/acquisition match. No camera pixels are logged/saved,
no phone or display owner runs, and no consumption token is a compositor receipt.
Injected-driver regressions cover conversion expiry, corruption, backpressure,
sequence wrap/gaps and terminal retirement without renewing publication timing.

### Fit geometry diagnostics — 14 October source

`capture_mapping(source_width, source_height, delivery_width, delivery_height,
CaptureFit)` in [camera_pixels.hpp](../include/headunit/camera_pixels.hpp) returns
the bounded `CaptureMapping` actually consumed by `adapt_capture_image` on the
capture worker. Inputs retain the existing 64–1920 × 64–1080 limits and the
existing contain/cover policy. No configuration, defaults or signed slot change.

The three rectangles use **pixel-edge coordinates**, with fractional edges
preserved: `image` is the complete scaled acquisition in delivery coordinates
(possibly extending outside delivery for cover); `source` is the visible crop
in acquisition coordinates; `destination` is the visible content in delivery
coordinates. `scale` is uniform. Sampling retains the existing half-open
pixel-centre rule, bilinear interpolation and direct-copy fast path. These
rectangles describe geometry, not integer counts of acquired/visible pixels.

For the current 640 × 360 acquisition and 1650 × 720 delivery, contain places a
1280 × 720 image at (185, 0), with the whole acquisition retained. Cover scales
to 1650 × 928.125 at (0, −104.0625), keeping acquisition y≈40.364–319.636.
The signed adapter separately validates its unchanged 1650 × 720 input at
(135, 0) on the authored 1920 × 720 canvas and rejects rotation/mirroring.
Native quarter-turn/mirror transforms and physical fullscreen fitting are
separate stages; this diagnostic does not observe either one.

The existing explicit `headunit-camera-capture-probe CAMERA.json` adds one
`captureGeometry` object **after successful actual capture and worker retirement**:

| JSON key | Meaning |
| --- | --- |
| `policy`, `adaptationEnabled` | Existing contain/cover policy and whether explicit CPU adaptation ran; without adaptation the mapping is identity. |
| `negotiatedStrideBytes` | Actual driver packed-row stride, including padding. |
| `sourceAspect`, `deliveryAspect`, `scale` | Negotiated acquisition and delivered input ratios; uniform software scale. |
| `imageRect`, `sourceRect`, `deliveryRect` | Above rectangles; each has `x`, `y`, `width`, `height`. |
| `signedSceneFit`, `opticalFit` | Always null: no scene/presentation/optical evidence exists in this probe. |

Existing `lastDequeue`/`lastPublication` remain the original acquisition,
reference, sequence and actual worker observation. Geometry does not renew any
date; it has no acquisition event of its own. Failed/insufficient acquisition
still exits unsuccessfully without a success JSON record. No pixels, device path
or signed resources are exported. This is diagnostic data for a future qualified
probe, not a requested configuration acknowledged as applied camera state.

For native UI: UI keys/actions/IDs/new controls/widgets/pages **none**. Public C++
geometry and the probe JSON above add no native UI or manager action.

Focused component qualification and source identity are retained outside Git in
`~/.cache/headunit-qualification/20261014/codex-camera-fit-validation/`.
Independent golden rectangles, portrait/border pixels, fractional edges and
extreme bounded aspects exercise the production mapper. The injected production
CaptureProvider/Queue fixture covers padded RGB rows through contain and cover,
original start-of-exposure dates, source reuse, expiry, malformed-successor
withdrawal, retained GPU leases, late receipts and negotiated-size rejection.
The Linux-only `camera-capture-probe-synthetic` CTest executes the production
probe main with only its device factory replaced. It checks both policies,
negotiated stride, JSON rectangles, acquisition-age arithmetic, post-stop output
and rejection without a success report. Its retained reports explicitly wrap
the unchanged production `physicalCapture=true` field in
`fixture=synthetic-pixels/injected-device`, `physicalEvidence=false`; that field
does not turn the injected device into hardware evidence. These are synthetic
image/clock and injected-driver checks; GPU, phone, physical lens/readability
and trigger-to-visible acceptance are separate.

Focused macOS CTests pass **7/7**; Debian trixie arm64 Clang 19 ASan+UBSan
passes **9/9**, including the actual probe main fixture and V4L2 syscall model.
Twenty complete images across contain/cover, current USB sizes, fractional edges,
portrait/extreme aspects and 1024 × 768 delivery are byte-identical to the
`37fb44b` CPU mapper. A final fixture-only lease-return wait is checked separately
in `camera-capture` on both platforms. Normal Mac 26-target/27-library linker
warnings remain recorded; no warning/leak/deadline policy changes. Full Skia was
not rerun for this CPU-only change. Manager review and the complete combined
required matrix precede any future release. Every owned build/container is
retired after verified logs, Testing, source identities and JSON retention;
real Docker INT/TERM/HUP retirement probes pass for the reused lane backend.

Exact next bench check belongs to **phone-session only**, after manager review
and a new coherent required matrix: retain the enabled current private profile
and its hash, verify the existing camera owner has retired/device is unowned,
then run the new probe once on that profile and retain binary/source/JSON hashes.
Check actual negotiated stride and contain's (185, 0, 1280, 720) delivery rectangle
against 640 × 360 → 1650 × 720, original acquisition/reference dates and successful
retirement. Queue consumption is not a compositor receipt. Reenter through the
existing signed owner and separately check both panels/streams/UI-Sync/audio/
control, source expiry and Off restoration. An illuminated target and optical
fit, physical touch/focus and trigger-to-visible time remain parked until the
operator/equipment and actual dated presentation evidence are available. Do not
run this probe concurrently with the managed capture owner or edit a sealed role.

## Publish a capture

### Consumed runtime camera diagnostics — 14 October, source only

`CaptureProvider` copies `CaptureAcquisition` onto the same immutable `Frame`
successfully offered to its existing Queue: validated negotiated width/height/row
stride, driver sequence and the actual source publication observation. The
optional adaptation policy is set only after that CPU adaptation returns; it is
absent for unchanged delivery. The adapter's optional `CaptureMapping*` output
returns the **same** calculation used by those pixels, only after success; that
mapping is attached with its acquisition facts, never recomputed from config or
latest statistics. Original acquisition date and EOF/SOE reference
stay on the Frame. Failed negotiation, corrupted/stale samples and rejected
offers do not publish a new record. Existing producers may omit acquisition
metadata; their format/stride/driver identity is not reconstructed from config.
Queue checks bounded metadata and the original publication observation before
transferring any pixels. Legacy Frame aggregates and offer calls remain valid.

`video::LocalInput::consumption()` returns a numeric copy for the exact
`Stream::composition_token()`: either the prepared source selected for
composition or the retained source accepted by `complete(token, true)`. Queued
frames are not consumed frames. A newer rejected or corrupt sample cannot date
an older composition. The copy includes no pixel/native pointer and retains no
producer lease. It does not create a frame, clock, GPU submission or receipt.
Expired, future, flushed, Off, suspended or terminal sources return no record,
including before another pump. Retired late receipts cannot republish a record.
An owned replacement Queue increments the input-local source generation; its
lease sequence can restart, while the existing Stream tokens remain increasing.
Identify it together with the report owner/connection, not as a global driver ID.

The existing default-off `diagnostics` path adds one `cameraConsumed=<JSON|null>`
value to the **main panel's existing finite display diagnostic line**. It retains
the existing five-second cadence, 1–120 report budget, producer predecessor,
report-owner start, directly supplied publisher/connection identity and output
role. No manager status, socket, capture reader, thread, service, per-frame JSON,
file writer or GTK control is added. Only two staged records and one current
numeric record exist on LocalInput, plus optional fixed Frame metadata in the
existing four-lease Queue. Stride arrays have at most four entries. Encoding
uses fixed keys, enum strings and exact decimal strings for 64-bit identities
and dates, at the existing report cadence.

| JSON keys | Actual authority / limits |
| --- | --- |
| `authority`, `target` | `local-input-stream`, `registered-input`. The target is the registered input's **delivered pixel bounds**, not the camera profile's native destination, signed rectangle or physical fullscreen window. |
| `sourceGeneration`, `leaseSequence`, `streamToken` | Input-local Queue replacement serial, original Queue frame sequence, exact Stream prepared/current token. |
| `captureMonoNs`, `captureReference`, `simulated` | Original Frame acquisition, optional `end-of-frame` / `start-of-exposure`, producer's existing simulation flag. Injected driver tests remain synthetic even when exercising the production V4L2 flag. |
| `preparedMonoNs`, `clockEpoch` | Actual preparation observation in host steady time and qualified MediaClock suspension epoch used at mapping. Neither is acquisition time. |
| `receiptObservedMonoNs` | Null while prepared; actual LocalInput observation when `complete(token,true)` accepts it. This interface receives only token/Boolean, so this is **not** the compositor's original presentation timestamp, scanout or phone transition. Repeated snapshots preserve it. |
| `storage`, `width`, `height`, `storageWidth`, `storageHeight`, `cropX`, `cropY`, `strideBytes` | Actual selected ARGB32 image or NativeFrame visible/storage/crop/plane facts. ARGB32 rows are tightly packed; a native array reports borrowed-plane strides only, never descriptor numbers or pixels. Native format/import support remains a separate renderer contract. |
| `acquisition` | Null without CaptureProvider metadata. Otherwise `width`, `height`, `strideBytes`, `driverSequence`, `publishedMonoNs`, `adaptationFit` describe the original negotiated acquisition and successfully published conversion. Delivered ARGB stride and original packed stride are distinct. `adaptationFit` is `contain`, `cover` or null. |
| `inputWidth`, `inputHeight`, `inputRotationDegrees`, `inputMirrorHorizontal` | Actual registered delivery bounds; the existing signed LocalInput guard admits only 0/false orientation. No lens/physical orientation is inferred. |
| `captureGeometry` | Null without a consumed CPU adaptation mapping. Otherwise `authority=cpu-adapter`, `target=capture-delivery`, `coordinates=pixel-edges-before-orientation`, `scale`, `imageRect`, `sourceRect`, `deliveryRect`. Each rectangle has `x`, `y`, `width`, `height`; fractional edges are retained with 17-digit precision. This is the mapping actually used by the successful adaptation, not signed placement. |
| `signedSceneFit`, `opticalFit`, `physicalDisplayFit` | Always null. No unobserved signed/optical/window fit is invented. |

Manager FINAL accepted exact fit source `b707370` before its import into this
isolated branch. Its bounded `CaptureMapping` is now a typed fact on the same
consumed Frame. The definition moves to `camera.hpp` so Frame metadata can copy
it; the existing `camera_pixels.hpp` API continues to expose it. Failed adaptation
leaves output untouched and publishes no mapping. Queue validates finite bounded
geometry before admitting it. External/native and unadapted sources without a
consumed mapping keep it null. Signed slot placement, optical fit and physical
display fit still require their own observations.

Focused component evidence is retained outside Git under
`20261014/codex-camera-runtime-geometry`: final Mac **7/7**, Linux Clang 19
ASan+UBSan with full Skia compile configuration **nine component passes** plus
the corrected **1/1 probe check**, including actual
`live_ui` compilation and production capture/Queue/LocalInput/Stream execution
with explicitly padded synthetic RGB and dates and the actual probe main with
its synthetic device factory. Twenty complete asymmetric contain/cover images
are byte-identical to the `d544236` mapper. Geometry defaults and signed/native
scenes are unchanged; the accepted mapper only gains a successful-return output.
The tests cover preparation
versus accepted receipt, source/delivery stride distinction, queued/corrupt/
rejected newer metadata, exact expiry before pump, script flush, late retired
receipt, terminal source, held backing and actual owned Queue replacement.

The 14 October lifecycle audit adds one combined production-input regression to
`theme_camera_input_test.cpp`: one current and two prepared native frames, then
Queue closure without another pump before the post-compose check and receipt.
The qualified V87 source fails both withdrawal checks. The correction also
preserves ordered retirement of both pending tokens, unchanged native backing
and metadata while held, and complete permit release afterward. Existing
explicit Off/On provider-replacement and blocked-driver-cleanup checks remain
separate. Original failures and focused source/component receipts are retained
outside Git under `20261014/codex-camera-lifecycle-audit/`. These modeled native
leases are not GPU imports, physical capture, scanout or connected-phone evidence;
whole-source review and a coherent release matrix precede adoption.

Original failed Mac compile evidence remains separate: adding an explicit
nullopt default repaired legacy aggregate compatibility, with no warning waiver.
The first final Linux attempt passed nine checks but failed its tenth because
the focused runner omitted the probe fixture target. Its original 9/10 report
is retained; a separate unchanged-source lane builds both actual probe targets
and passes the failed check. No failed required check is waived or called a
single 10/10 lane.
Linux retains stock warnings/leak checks and existing external LSan suppressions;
shared Skia is uninstrumented. These are software fixtures and caller compilation,
not GPU execution, physical camera/lens/display, phone, audio or fresh-fps
qualification. The manager owns combined review and the future coherent full
required matrix; frozen V85 is untouched.

For native UI: **new UI keys/actions/IDs/widgets/pages: none**. `cameraConsumed` is
private finite diagnostic data, not an admitted manager view model. Existing
Home2 composition/style and camera permissions are unchanged.

#### Exact follow-up for the sole phone-session owner

After whole-source manager review and a new coherent required matrix, seal a new
immutable generation with the existing camera profile, signed pack, read-only
aliases and geometry defaults. Enable only the existing finite diagnostics
(`diagnostics=true`, `diagnosticReports=30`); do not edit frozen V85 or start
another capture/display owner. Check selected source/helper/unit identities first.

1. Retain original main diagnostic lines and their producer chain, report-owner
   start and directly supplied publisher/connection identities. Before On,
   expect `cameraConsumed=null` even with a requested or queued source.
2. Through existing manager On, wait for a main diagnostic carrying an exact
   prepared/current token. For the currently documented CPU-copy USB profile,
   verify actual original acquisition 640×360 and **negotiated** stride, delivered
   1650×720 ARGB32/6600-byte rows, original capture/reference, driver sequence,
   source publication and preparation dates. Compare the same acquisition/driver
   identity to existing bounded capture diagnostics when available, without
   opening a competing probe. Other selected profiles use their actual facts;
   no hardcoded stride or age is a pass criterion.
3. Hold/repeat reporting: the same token preserves all original source dates and
   receipt observation. A genuinely new token must carry its own acquisition;
   report date or scene redraw cannot renew it. Confirm requested versus observed
   state separately. For an actually adapted 640×360→1650×720 contain frame,
   expect scale 2, full sourceRect `(0,0,640,360)`, imageRect/deliveryRect
   `(185,0,1280,720)` from its own CPU adaptation. Cover or different actual
   source sizes carry their own crops; unadapted/external/native mapping stays
   null. All unobserved signed/optical/physical fits remain null.
4. Use the existing Off and qualified source/lease failure checks. The next
   available report must have no consumed source; retired receipt/metadata must
   not return in a successor. Explicit Off/On may recover only after original
   provider retirement, with a new Queue source generation and new capture/token.
   Original retained GPU backing may outlive withdrawal; it must not be reported
   fresh. A successor renderer starts a new report chain.
5. Independently retain both panels/streams/UI-Sync/control/audio owners, actual
   PCM progression/underruns, Home/Back/touch/Climate and native fallback evidence.
   A camera diagnostic record does not qualify phone Close/focus, pixel visibility,
   continuous audio, trigger-to-visible latency, physical touch or fresh60.

The phone owner separately qualifies signed-target clipping/readability with an
illuminated target and operator assistance; no fixture proves optical fit.

`offer_image` transfers an original premultiplied ARGB32 image on success. A `false` result leaves the producer's image intact so it can retry or discard it deliberately. Invalid input throws. A synthetic source must mark its frame as simulated.

`offer_native` retains the existing `video::NativeFrame` and its producer-sample lifetime. It does not duplicate or close borrowed DMA-buf descriptors. An external adapter must provide valid, immutable planes, supported format information, and a genuine sample lease. GPU import support remains the renderer's responsibility; accepting a mailbox frame does not prove that it can be imported.

Every frame records `capture_monotonic_ns` separately from native-frame metadata. Use the local `CLOCK_MONOTONIC` domain on Linux. Never write local capture time into a phone NTP field, and never replace capture time with queue arrival, script tick, or presentation time. `observed_ns` must belong to the same local clock and be at or after capture. Capture times must increase strictly within a queue generation. Replace the queue/provider on a source-clock discontinuity.

`Frame.capture_reference` optionally preserves the driver's acquisition reference: `end_of_frame` or `start_of_exposure`. The optional final arguments to `offer_image` and `offer_native` carry it unchanged. Absence makes no claim about exposure boundaries; existing external/synthetic publishers keep their prior behavior. These values describe the driver's original monotonic timestamp, never a replacement timestamp sampled after dequeue. `Provider::failure()` exposes a terminal capture-owner error separately from a nonblocking poll with no frame; the display owner must withdraw the invalid source. `make_provider(configuration, queue)` is the runtime source-selection boundary, separate from file validation.

## Present a frame

1. Call `stage(now_ns)` on the display owner. It returns only a fresh, due frame and protects that submission with a unique sequence token.
2. Keep the returned immutable frame alive while the GPU reads its backing storage.
3. After the actual presentation receipt, call `complete(sequence, true)`. Only this operation advances `current(now_ns)`.
4. Use `complete(sequence, false)` for a rejected submission. Complete pending receipts in submission order.
5. Keep the frame lease until the GPU fence has signalled, even if the presentation receipt or a view exit has already occurred.

Staging, drawing, and generating the synthetic pattern are not presentation. The queue keeps at most two staged submissions and four live frame leases in total. The newest waiting capture replaces an older unstaged capture; it cannot replace a frame already leased to the GPU. Holding four old frames deliberately causes backpressure. `discard` releases the queue's references, but external GPU references still count against that bound. Old cancelled receipts are ignored, and submission tokens are never recycled on entry or exit.

For CPU images the queue owns at most four 1920 × 1080 ARGB32 frames, about 31.6 MiB of pixel storage. Native frames are bounded by sample count; their true allocation cost depends on the producer's format and stride. The adapter must also bound its own capture buffers and any frames it has not yet offered.

## Handle availability and view ownership

`Availability` distinguishes missing input, fresh input, stale input, a reversed local clock, and a closed generation. `current(now_ns)` returns no image once its capture is stale or future-dated. The display must then clear the old camera texture and show an honest unavailable state. This foundation cannot erase pixels already on a physical display itself.

Availability and `available_capture_ns()` use the newest capture still retained
by the queue (waiting, staged or current). Acquisition ordering separately
retains the last offered date across rejection and withdrawal. A newer rejected
capture, even if an external GPU lease retains its storage, cannot date an older
current image as fresh. Once that current image expires, availability is stale;
without any retained source it is missing. The redundant `withdrawn` flag is
removed: `discard()` already clears all three source containers.

`Session::enter(previous_view)` stores an internal view identifier such as `carplay/ultra` or `native/home`. Repeating entry does not overwrite it. `turn(now_ns)` polls the active provider and collects completed retirement without waiting for camera I/O. `leave()` withdraws queued/current camera frames, requests asynchronous capture stop and returns the saved identifier. Entry returns false while retirement is pending or after the Queue is terminally closed. The display owner resolves the saved identifier against current connection state; for example, it must choose a valid native view if the saved phone session no longer exists. The cluster remains under its own independent ownership.

The separate [activation controller](CAMERA_ACTIVATION.md) drives the native appliance from private manual input or fresh typed transmission state. It retains original gear acquisition/provenance, bounds each outgoing request lease and requires a live native-owner acknowledgement. Its process/socket checks pass on portable fixtures and actual CM5 Linux; physical reverse acquisition, camera capture/import and display/focus qualification remain separate.

The test pattern has coloured bars and a moving checker stripe. Its frame metadata always says `simulated=true`. The eventual display must visibly label the bench source as simulated. The CPU pattern generator is a correctness fixture, not evidence of camera or full-resolution GPU performance.

## Present locally without a phone

[`camera_display.hpp`](../include/headunit/camera_display.hpp) exposes `NativeDisplay` for the existing Linux GPU build. Supply an enabled `Configuration`, its `Queue`, and explicit Wayland window/output settings. The queue must outlive the display and have exactly one staging/completion owner. Neither the display nor its probe starts or modifies a system service.

Call `turn()` regularly. Each call polls actual Wayland feedback and submits at most one new capture or changed availability scene. It does not generate frames, wait for socket readiness, or repeat a static scene to imply fresh capture frames. The source can run independently through `Provider::poll` or an external capture producer. At most two Wayland submissions remain pending.

The first implementation uses synchronous GPU fences. Deferred scene fences are rejected explicitly. The compositor finishes its GPU reads before returning, and the display additionally retains the immutable camera frame through its actual Wayland receipt. If cold GPU setup makes a capture expire before submission, the display submits a stale-status scene instead. It retains that capture's ordered token until the status receipt arrives, then cancels it after any older outstanding camera receipt. A presented status scene never advances the camera capture. Missing, stale, invalid-clock, and closed states withdraw the camera image and show readable original labels. Synthetic frames always show **SIMULATED CAMERA**.

Each generated image/native scene also aliases the outer camera-frame lease. Retaining a scene without a separate staged-frame reference therefore still counts against the four-frame producer bound. An explicitly discarded last status commit allows a later bounded retry; drawing or discarding it never counts as presentation.

`LocalPresentationClock` validates the receipt's real cross-clock correlation and maps it into the local monotonic domain directly. It does not create an AirPlay clock, phone NTP value, or qualified media sample. Discarded feedback produces no presentation time. Receipt callbacks contain bounded metadata: original capture time and optional acquisition reference, actual mapped presentation time, capture age when available, flags, and source state. Headless feedback proves compositor presentation, not physical scanout. GPU fence completion, synthetic capture rate, and presentation rate are distinct measurements.

`SceneBuilder::status_overlay(active, availability, simulated, output_size)` provides the same labels independently for Ultra or another display owner. It returns no overlay when inactive. Unavailable states clear only the configured camera rectangle; unrelated parts of the main canvas remain untouched. The caller must separately withdraw stale camera frames and update the verified asset's activation binding. This helper does not modify signed scripts or implement phone focus negotiation.

## Integration remaining

- Connect the standalone local presentation owner to the appliance's real main-view controller, including entry/exit, touch/focus restoration, and a physical screen check. `take_touches()` exposes local input but does not send it to a phone.
- Register the camera input only through asset resources whose semantics have been verified. Wire the real Ultra UI transition and controls separately from this local mailbox.
- Qualify the implemented V4L2 adapter with a selected camera, including real driver timestamps, DMA-buf import, fences, unplug/replug and new-generation recovery. CSI pipelines and additional formats are separate adapters.
- Route a fresh reverse-gear signal through the shared vehicle adapter, keeping the manual bench path explicit.
- Measure physical first-frame delay, capture-to-presentation age, stale-source handling, and restoration on the CM5.

## Verification and dated evidence

The CPU adapter reuses validated source/destination row addresses during its
existing bilinear pass. Source coordinates, four samples, fixed-point channel
weights, rounding, opaque bars, crop and consumed mapping are unchanged. All
work remains on the capture worker; no source date or freshness policy changes.
Twenty deterministic asymmetric/border/alpha contain/cover images in
`camera_pixels_test.cpp` check every output pixel against a scalar reference.
Separate GCC and Clang qualification oracles compare every output pixel and
mapping bit against the independently compiled original, including fractional
edges, extreme aspects and the identity path. The identity copy retains source
alpha; the resampling path and bars retain their original opaque alpha. The original synthetic probe still requires thirty
consumed samples in five seconds, with the same 150 ms acquisition-age limit.
Its injected pixels and measured software work do not establish physical
capture, GPU presentation or a camera/display frame-rate claim.

[`camera_provider_test.cpp`](../tests/camera_provider_test.cpp) verifies strict configuration, future/repeated/reversed/stale time handling, presentation receipt ordering, immutable backing lifetime, four-lease backpressure, unique tokens after view exits, late cancelled receipts, synthetic cadence without catch-up bursts, view restoration, native metadata preservation, and concurrent publication.

The 9 October architecture regression presents one capture, rejects a newer
one, and checks that the retained capture keeps its original date and expires
on its own age boundary. The previous implementation fails the date check.
Fresh waiting/staged captures still recover, and withdrawal rejects late
receipts while their backing remains leased. `theme_camera_input_test.cpp`
also checks the production input's visibility sink after rejection and expiry.
Scoped evidence and remaining qualification are in
[the architecture review](ARCHITECTURE.md#architecture-review--9-october-2026).

[`camera_configuration_test.cpp`](../tests/camera_configuration_test.cpp) checks source/device schema and a disabled V4L2 template without device I/O. [`camera_pixels_test.cpp`](../tests/camera_pixels_test.cpp) checks YUYV/RGB/BGR conversion against independent reference pixels, stride/padding bounds and owned-copy lifetime. [`camera_capture_test.cpp`](../tests/camera_capture_test.cpp) exercises the production worker and Queue with an injected driver: nonblocking presentation, newest-frame selection, real content, original acquisition metadata, serial wrap, corrupt/stale recovery, bounded leases, stop retention, delayed retirement and terminal negotiation/clock/sample faults. The 13 October regression injects both standard and unknown terminal exceptions
while driver stop is blocked: failed state and queue closure must already be
visible, retirement acknowledgement remains pending, GPU-held DMA backing
survives and shutdown is called once. This regression fails against the prior
worker. These fixtures are not Linux syscall or physical-camera evidence.

The source-only acquisition-owner regression `closed_capture_with_pending_native_leases`
combines boundaries previously checked separately. The real CaptureProvider and
Queue retain one current and two staged native frames while the injected driver
is in flight. Its fault outcome closes the Queue before deliberately blocked
cleanup; its explicit-stop outcome returns a valid late sample after closure
without publishing it. The existing bounded wait acknowledges retirement after
the fake stall is released, while all three external backing leases remain held.
Old receipts cannot restore availability or presentation counts. An independent
successor driver/Queue with the same configuration starts its own sequence and
fills its original four-lease pool while those old leases remain held. Original
acquisition/reference/publication dates and the exact freshness boundary stay
unchanged. Final old releases run on another thread after driver destruction,
reach only the original backing owner and invoke no driver call.

This adds regression coverage, with no demonstrated production bug or runtime
repair. Native backing/descriptors, acquisition clocks and Boolean receipt
completions are explicit fixtures; they establish no real mapping, DMA import,
GPU fence, presentation, phone state or physical recovery. The unchanged V4L2
syscall model separately checks actual adapter mapping/export finalizers and
return mailboxes. No LocalInput/NativeInput consumer retirement case is duplicated,
and no camera permission, control/focus contract, buffer limit or deadline changes.

On **6 October 2026**, [`theme_camera_recovery_test.cpp`](../tests/theme_camera_recovery_test.cpp) runs the production CameraInput, Queue, Stream and clock paths with only the source factory substituted. It checks terminal withdrawal, no retry while On is held, delayed retirement before explicit reentry, new source dates/pixels, increasing tokens, late receipts, retained GPU backing and terminal external ownership. The prior implementation fails the explicit-reentry assertion. ASan/UBSan instruments the changed input and these Queue/Stream/clock paths; external Cairo and remaining linked support are uninstrumented, with leak detection off. Strict local `live_ui.cpp` syntax checking uses libc++'s experimental syncstream support. These are portable/model checks, not the Linux GPU owner, real V4L2 recovery or connected-phone acceptance.

[`camera_v4l2_test.cpp`](../tests/camera_v4l2_test.cpp) compiles the actual adapter with only its POSIX/kernel calls replaced. The earlier eleven syscall-model groups cover negotiated row/color metadata, exact device/capability/bounds checks, partial startup cleanup, retained CPU/export backing after stop, cross-thread release mailboxes, corruption/serial wrap, acquisition/reference faults, EAGAIN, bounded EINTR retries and terminal capture. A 13 October regression additionally
checks that live dequeue and requeue failures report before STREAMOFF, terminal
adapters make no further capture calls, and explicit cleanup retains borrowed
backing. The earlier strict compilation, ASan/UBSan and source ThreadSanitizer checks pass against pinned Linux v6.12 UAPI headers on Darwin. This is not Linux ABI/kernel execution, a V4L2 device test or DMA-buf import evidence. The Linux CTest target is `camera-v4l2-syscall-model`; the GPU and physical checks remain separate.

On **13 October 2026**, six affected Mac CTests and seven Debian trixie arm64
Docker CTests pass; the same seven Linux checks pass with Clang ASan+UBSan and
existing external-library leak suppressions. The worker and V4L2 retirement
regressions separately fail against their preceding implementations. Linux
manual-owner fixtures also preserve pinned bound sockets before `listen()`,
unsafe abandoned sockets and sockets with unavailable kernel inventory. These
are injected-driver/syscall-model and actual local IPC checks; no new physical
camera, CM5 service, signed GPU or phone behavior is qualified. The cached GCC
build retains its existing warning exceptions. Receipts remain outside Git in
`~/.cache/headunit-qualification/20261013/camera-reliability/`.

[`camera_display_test.cpp`](../tests/camera_display_test.cpp) checks all quarter-turn/mirror combinations against reference pixels, aspect-preserving placement, immutable cached labels, independent unavailable overlays, camera-typed native texture staging, and malformed/reordered/stale/future clock feedback. The portable tests pass with warnings treated as errors and with AddressSanitizer and UndefinedBehaviorSanitizer.

[`camera_display_wayland_probe.cpp`](../tests/camera_display_wayland_probe.cpp) is a standalone isolated Linux GPU probe. It requires actual presented receipts for missing → fresh synthetic → stale → recovered source phases, with two submissions and four source leases as upper bounds. Run it through the existing [`theme_wayland_gpu_test.py`](../tests/theme_wayland_gpu_test.py) harness to use a private headless socket. Compilation alone does not verify GPU presentation.

On **4 October 2026**, the complete [Standard/native handoff fixture](STANDARD_VIDEO_OUTPUT.md#cm5-linuxgpu-execution--4-october-2026) passes actual CM5 GPU feedback with 18/18 dependent H.264 frames, generated camera missing/fresh/stale/recovery scenes, exclusive surface ownership and an independent cluster. The [seven Linux control/IPC suites](CAMERA_CONTROLS.md#cm5-control-qualification--4-october-2026) separately use an explicitly synthetic HCS1 publisher. These two results do not qualify the updated native daemon's combined GPU/HCS1 lifecycle, a V4L2 device/import, physical screen/focus restoration or phone-side Punch-through.

### Physical acquisition — V81, 13 October 2026

The exact full-Skia GCC matrix's capture probe runs for five seconds on the
CM5 USB camera after verifying no current device owner. It consumes 136 actual
captures, negotiated 640×360 and configured delivery 1650×720; mean source age
is 37.98 ms and maximum 46.15 ms. No pixels are saved and no display, phone or
radio owner is started. Queue consumption is explicitly not a compositor receipt.
The four non-cluster service identities/restart counts stay unchanged through
this and the separate local-cluster fixture. Terminal blocked-cleanup paths are
still injected driver/syscall evidence, not a demonstrated physical kernel hang.

Raw probe output and source/binary receipts are private under
`~/.cache/headunit-qualification/20261013/agent-forward/`. This confirms acquisition
with the repaired source; connected Punch-through, camera crop/fit, audio
continuity, phone Close/focus and repeated unplug remain separate open checks.
