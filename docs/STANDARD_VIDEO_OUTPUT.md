<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Manage Standard CarPlay video output

`GstPreviewOutput` separates a decoded Standard CarPlay picture from the
decoder that produces it. A local camera owner can withdraw the picture
without discarding H.264 reference frames or closing the phone session.

The receiver now offers an opt-in `wayland-gpu` backend with an owned presenter
and a file-driven camera handoff controller. The normal backend remains
`wayland-touch`. The appliance installer still rejects Standard camera services
pending physical qualification. Decoder/controller fixtures and explicit fake
presentation fixtures establish separate component contracts. The isolated
Linux qualification below now exercises actual GPU receipts and native camera
surface ownership with generated sources and an independent cluster presenter;
physical camera and two-screen phone continuity remain unqualified. This
Standard native replacement route is separate from inserting camera content
inside an existing signed Ultra scene. See
[native camera ownership](NATIVE_CAMERA_APPLIANCE.md).

The schema-2 [resident Home route](NATIVE_HOME.md) is a separate explicit use of
the owned Standard presenter. Managed Standard selects `wayland-gpu` when its
paired native-page request/lease settings are present. On 4 October 2026 the
user confirms Standard main/native gauges on the correct physical screens,
touch, Home/Back after ten seconds and returning to Ultra through the native
selector. That qualifies the tested Home route, not camera entry/exit or every
decoder/receipt fault.

## Keep decoding independent of the window

The main `wayland-touch` pipeline ends in this structure:

```text
appsrc → h264parse → decoder → bounded decoded queue → [videoconvert] → valve
                                                                       ↓
                                                        replaceable output
```

The hardware decoder omits `videoconvert`. The software decoder retains it.
The initial output is `waylandsink`; the phone-free fixture uses `fakesink`.
The ordinary cluster and legacy X11/Wayland paths keep their existing outputs.

One owner calls the lifecycle API. The streaming thread applies admission
checks through pad probes. The component borrows the pipeline and valve;
neither the parser nor decoder is replaced or moved out of PLAYING during an
output switch. The owner must stop the entire pipeline synchronously at NULL
before destroying the component, then release the pipeline. Removing a probe
alone does not wait for an already executing callback.

## Use the output lifecycle

The contract lives in
[`gst_preview_output.hpp`](../include/headunit/gst_preview_output.hpp).

| Operation | Required behavior |
| --- | --- |
| Construct | Before streaming, with a last, unlinked valve belonging to the pipeline. Install a permanent, unclocked drain. |
| `start(new_output)` | Before PLAYING, take ownership of an element/bin exposing an unlinked sink pad. This initial path makes no camera-return freshness claim. |
| `arrival_time()` | Read genuine local running time before queueing each encoded packet. Pin clock/base identity from this first observation. |
| `drain()` | Close the valve, stop/remove/finalize the old output, install an unclocked drain and reopen. Only successful completion permits destruction of a borrowed Wayland parent. |
| `restore(new_output)` | Consume a new output with no cached picture. Require a stable PLAYING pipeline, observed unit-rate TIME segment and original decoder time domain. Capture a new return boundary. |
| `restore_ready()` | Nonblocking bootstrap predictor. Observe retirement even while the pipeline is not ready; `restore()` still validates the actual boundary. |
| `stats()` | Read synchronized installation/admission counters and retirement state. A forwarded buffer is not proof of display presentation. |

These are internal C++ operations, not a phone protocol message. The opt-in
`StandardPreview` controller calls them on the receiver owner. Sink bins are
bounded to sixteen watched elements; they must not
retain external references or run independent work after reaching NULL.

## Admit video received after restoration

`GstVideoPreview::submit` captures encoded arrival at method entry, **before**
camera return/window work and the appsrc queue. A frame already delivered
cannot trigger restoration and then acquire a timestamp after its boundary.
The stamp is the actual pipeline clock minus its base time. It is local
arrival time, not iPhone capture time, a qualified Ultra clock, or an end-to-end
latency measurement.

Both input PTS and DTS receive that same saved local arrival. Automatic source
timestamping is disabled for the gated backends. The complete fixture caught
a real later-DTS substitution through appsrc/parser: an older frame crossed
the return floor despite its original PTS. Read-only source/parser/decoder
probes now check all eighteen generated baseline frames against their original
arrival, including a restoration triggered inside the first returning submit.
See the pinned primary [base-source timestamp logic](https://raw.githubusercontent.com/GStreamer/gstreamer/1.26.10/subprojects/gstreamer/libs/gst/base/gstbasesrc.c)
and [H.264 parser](https://raw.githubusercontent.com/GStreamer/gstreamer/1.26.10/subprojects/gst-plugins-bad/gst/videoparsers/gsth264parse.c).
This test does not qualify arbitrary reordered H.264 streams or the CM5's
actual phone/decoder path.

At restoration, the gate converts each decoded PTS through the observed TIME
segment. Only running time strictly greater than the new return boundary may
enter the new sink. Missing, older and equal timestamps are rejected. Buffer
lists are rejected rather than bypassing per-buffer validation. Hidden input
continues through a permanent drain so decoding and end-of-stream can finish.

Clock identity, base time, actual stream identifier/group/flags and segment
identity/mapping belong to one decoder generation. An observed change retires
that generation permanently; a new return boundary or another drain cannot
renew it. Clock/base identity is pinned before the first encoded queue entry,
and stream/segment identity is pinned as events arrive. Flush discontinuities
are observed at the valve input even while its closed output drops events.
Retired queues may drain, but display restoration and new arrival stamping
require a new decoder generation.

## Release the complete old output

Stopping a sink at NULL is insufficient if it still retains an embedded
window. The component unlinks/removes its output and watches finalization of
the bin and children before reporting a successful drain. An external reference
or unfinished element retirement leaves the gate closed and fails the operation.

Bounded bus cleanup discards obsolete output status messages, preserving
original ERROR/WARNING payloads with the stable pipeline as their source.
Unrelated messages retain their identity; concurrent posts may precede
reposted messages. A separate bus consumer must be coordinated by the owner.

End-of-stream arriving while the valve is closed must also reach the new
output without a subsequent frame. A short input-pad stream lock protects
reopening. If EOS is retained, the original sticky sequence is forwarded in
order to the new output, including stream start, caps, segment and EOS. It
does not inject a synthetic flush or invent a new stream identity. This also
covers a further replacement after EOS has already reached the old output.
See the primary [valve implementation](https://raw.githubusercontent.com/GStreamer/gstreamer/1.26.10/subprojects/gstreamer/plugins/elements/gstvalve.c)
and [pad implementation](https://raw.githubusercontent.com/GStreamer/gstreamer/1.26.10/subprojects/gstreamer/gst/gstpad.c).

## Use an owned presenter for camera handoff

Before enabling the Standard route:

1. Acquire the shared main-surface lease before constructing any main window,
   including a camera request that precedes the first video configuration.
2. Poll requests on owner turns even when no frame arrives. Cancel an active
   phone gesture and discard pressed input before retiring its window.
3. Drain and prove sink finalization before destroying the Wayland parent and
   releasing its lease. Keep the phone session and cluster independent.
4. On exit, acquire the shared lease, construct a new parent/output and retain
   the return barrier. Restore touch only after **actual fresh child-video
   presentation** correlated with its accepted source buffer.
5. Test repeated transitions, EOS, phone loss, camera failure and initial
   hidden startup on physical displays. Preserve explicit unavailable status.

The existing GStreamer Wayland child does not expose the required qualified
per-buffer receipt through this component. Parent/background feedback,
`forwarded`, decoded counts and window creation cannot substitute for it.
The opt-in owned presenter below supplies that source-correlated receipt path.
Physical camera/touch acceptance remains separate.

## Present Standard video in an owned GPU window

These components implement the opt-in route; they do not change the active
default or enable installed Standard camera services.

| Component | Contract |
| --- | --- |
| [`GstPreviewFrames`](../include/headunit/gst_preview_frames.hpp) | Produce bounded immutable opaque pixels and their original local arrival running time. Transfer the complete output bin to `GstPreviewOutput`; retain only a weak appsink reference. |
| [`StandardPreviewWorker`](../include/headunit/standard_preview_worker.hpp) | Own the GPU/window and shared surface lease on a separate thread. Retain submitted scene/source references through their actual receipts, and authorize phone touch only for a current-generation presented source. |
| [`StandardPreview`](../include/headunit/standard_preview.hpp) | On the control owner, poll activation, suspend input, drain output, request a fresh window, install a fresh adapter and arm its exact restoration boundary. Retain cancellation after a terminal output failure. |

The integration keeps fixed raw-video negotiation **before** the
replaceable valve:

```text
control owner: appsrc → h264parse → decoder → bounded queue
                  → videoconvert → BGRx caps → valve → GstPreviewFrames
                                                         │
                                       immutable copied image + arrival
                                                         │
GPU owner:                     latest-image mailbox → scene → Wayland
                                                         │
                                   actual wp_presentation receipt
                                                         │
control owner:                     bounded accepted/retried HID reports
```

This route uses CPU conversion/copying and GPU image upload. It is not a
DMA-buf/zero-copy decoder, a CPU scene fallback or a per-frame GPU readback.
`GstPreviewFrames` accepts progressive, single-view mono/unspecified full-frame
BGRx with square pixels and a positive validated stride; it rejects unsupported
multiview, pixel aspect, crop/storage/metadata. Negotiation fixes ordinary
unspecified input to mono; sample validation independently checks those rules. It
copies undefined X bytes to opaque alpha, preserves RGB and validates the
addressed span of every padded row before reading it. At most three decoded
samples wait in appsink; GStreamer's own current-sample cache is finalized
with that sink. The adapter retains no sample or buffer after `pull()` returns.

A real explicit unit-rate TIME segment is required. Pre-segment buffers and
buffer lists are rejected before a base sink can invent a default segment.
Missing timestamps and invalid mappings fail; equal/backward arrivals are
discarded. The adapter does **not** establish decoder epoch authority on its
own: the retained `GstPreviewOutput` gate must still reject clock, stream and
segment discontinuities. It supplies the return boundary, not a phone capture
time, Ultra clock or end-to-end latency measurement.

### Keep GPU work off the control owner

The worker creates, draws, polls and destroys its GPU/window on one dedicated
thread. Only one latest decoded image waits in its mailbox. One or two
submitted immutable scenes remain owned until their matching receipt; this
bound is explicit. Older decoded mailbox images may be coalesced; compressed
reference frames are never discarded by this worker. The renderer's own
bounded caches are separate from these submission references.

The worker polls at most five milliseconds apart even without another source
image, so static video still receives input/feedback and withdrawal requests.
No script VM, GStreamer element/sample or synthesized AirPlay clock crosses
this boundary. GPU completion uses the existing bounded synchronous-fence
policy. Graphics-driver hangs remain outside the component qualification.

### Observe a slow Home return on a private bench

`HEADUNIT_STANDARD_DIAGNOSTICS=1` enables a content-free trace in the existing
`GstVideoPreview` control owner. It is disabled by default and emits nothing
before that owner observes native Home. `Standard return diagnostic` entries
report phase changes immediately and sample an unready return at most once
every 250 ms. Steady Home and qualified video need no periodic entries. The
64-event budget belongs to the preview lifetime and never refills on repeated
Home cycles. The trace changes no source, clock, freshness, touch or window
admission rule; logging failures cannot retire the presenter.

Each entry contains actual host monotonic observation time, native Home and
controller stage, matching window/request generations, source arming and
touch readiness, decoder-epoch readiness/retirement, gate generation and
forwarded/rejected counts, and presenter accepted/submitted/presented counts.
`encodedIngress` counts frame observations at `submit` entry, before output
restoration and appsrc admission; `ingressMonoNs` is that actual host time.
`ingressRunningNs` is the original local decoder running-time stamp, and
`sourceFloor` is the installed return boundary. These are distinct domains,
neither is a phone capture time. Missing optional readings are `none`.
`ownerMonoNs` is the GPU worker's actual heartbeat; `presentedArrivalNs` is
source provenance attached to its receipt. The control readiness decision and
diagnostic worker/gate snapshots are sampled independently; an entry is not an
atomic view of the streaming and GPU threads. A failed optional ingress-clock
observation records `none` and cannot change packet delivery.
The trace contains no packet,
image, phone identity, capture path or signed asset material. Enable it only
for a bounded private bench run, with logs retained outside Git.

On 4 October the changed source passed the preview-output/controller suites
and actual isolated CM5 Standard GPU handoff (3/3). The generated H.264 wrapper
also checks default-off logging, window ACK versus source arming, unready-only
sampling, the lifetime cap and original ingress/presentation provenance with
explicit fake GPU receipts. The integrated control probe builds; no live phone
return is established by those checks. The global GCC indentation exception
remains, while the gauge object's earlier exception is removed as qualified in
[HANDOFF](HANDOFF.md).

### Separate window readiness from presented-source readiness

`status().window_ready` acknowledges construction of the exact requested
generation. It permits the control owner to restore decoder output; it is not
proof that a phone image appeared. `arm_source(generation, arrival_floor)` pins
that generation's original running-time floor once. Only an image strictly
newer than the floor may enter its mailbox. Initial visible generation 1 may
use no floor; that path makes no camera-return freshness claim. Initial hidden
startup and later returns require a real boundary.

Each submission records its exact token, generation, scene and source arrival.
`LocalPresentationClock` validates the actual receipt and real host-clock
correlation; no frame callback, window ACK, decoder counter or EGL swap can
authorize touch. Only a matching presented receipt of the current armed source
sets `touch_ready`. Discarding a static first picture can retry the **same**
source at most three times. Its arrival time and accepted-source count do not
advance, and a queued newer picture replaces the retry.

Every explicit `request_window()` advances the generation, even if the final
visibility matches its old state. Old construction ACKs, queued images,
receipts and input cannot revive a returned view. A request immediately retires
phone touch. GPU destruction and pending-scene retirement precede shared-lease
release; the acknowledged `retired` generation means there is no corresponding
window left. Construction acquires the shared lease before creating a window.
Active turns revalidate its inode/parent, including after drawing, so replacing
the lock file retires the presenter. Faults withdraw the window and release
the lease; reusing that faulted worker is prohibited.

`suspend_source(generation)` retires input and queued images before decoder
drain, while keeping the window and shared lease held. It is sticky for that
generation: a late receipt cannot reauthorize touch, and `arm_source()` cannot
undo it. Drawing checks suspension again before submitting. A subsequent
window request is required to create a fresh source generation.

### Preserve phone input under backpressure

The receiver must call `drain_touches(sender)` on its control owner with a
bounded nonblocking sender. A false return retains the report, including a
release for a previously accepted press. The callback must not reenter worker
methods. Retry it on regular control turns without relying on another physical
touch event. A readiness descriptor wakes the owner for new reports.

There is intentionally no vector-taking API that marks a release delivered
before the phone queue accepts it. Retiring/beginning a worker generation keeps
a cancellation for an actually delivered contact and rejects late reports.

### Configure the receiver controller

Use a Standard-only receiver profile with an explicit `display_config` whose
main panel declares `wayland` and `output`. For example, these optional fields
select the new route:

```json
{
  "preview_sink": "wayland-gpu",
  "display_config": "/absolute/private/displays.conf",
  "standard_camera_control_file": "/absolute/private/runtime/camera.request",
  "standard_camera_lease_file": "/absolute/private/runtime/main.lease"
}
```

This is a profile fragment, not a complete receiver configuration. Both camera
paths may be omitted to use only the owned video presenter. If either is
supplied, both are required. These absolute endpoint names are not normalized
through leaf symlinks. Their real parent must be owned `0700`; existing leaves
must be owned `0600` regular files. The request is bounded to 24 bytes and accepts
canonical `off\n`, legacy `on\n`, or `on <positive monotonic deadline>\n`.
The deadline is in host-monotonic nanoseconds. Timed requests expire and cannot
promise more than one second. The lease is
empty with one link. They must name distinct endpoints. Preflight
does not create or truncate either file. The worker safely creates a missing
lease at first use; a controller owns request creation/replacement.

The launcher selects the main panel's actual Wayland socket. It clears inherited
camera flags and exports `HEADUNIT_STANDARD_CAMERA_CONTROL` and
`HEADUNIT_STANDARD_CAMERA_LEASE` only for the explicit pair. The C++ receiver
constructs its immutable activation reader and validates the lease before
opening its control listener. A non-GPU build, Ultra mode, unnamed main output
or unsupported dimensions fails startup. Supported logical dimensions are
320–4096 pixels wide and 240–2160 pixels tall.

Only the Standard main stream uses this presenter. An explicitly configured
navigation/cluster output retains its independent legacy renderer and never
receives the main camera lease or phone-touch ownership. A failed optional
cluster constructor, submit or input pump is isolated from healthy main input
and packet capture.

### Follow the handoff states

`StandardPreview` implements these transitions:

1. Retire the receiver's active HID generation before draining decoder output.
   Finish temporary appsink/sample references, then prove complete output
   finalization. On drain failure, still withdraw/fault the GPU owner.
2. Request a hidden window. Keep cluster/control/decoder alive. The worker
   destroys its GPU before publishing matching retirement and releasing the
   shared lease; the camera's exclusive lease waits for that release. The
   controller's `hidden` stage alone does not prove withdrawal or camera visibility.
3. On camera exit, request a fresh window. Recheck camera activation before
   consuming its current-generation construction ACK or restoring output.
4. Install a fresh adapter through `restore()`, then arm that exact worker
   generation with a genuine boundary from the unchanged decoder time domain.
   Initial hidden startup must wait for observed stream/segment readiness.
5. Pass only current-generation immutable images and drain HID through the
   accepting sender. Allow phone touch only after source-qualified presentation.
   Withdraw stale ACKs and faulted owners rather than exposing old video.
6. On an optional output fault, stop its pipeline at NULL and keep its input
   owner alive. Retry a rejected cancellation on subsequent control turns
   until accepted or the connection closes. Do not restart a retired source
   or pretend that its old picture has returned.

An initial camera request starts with the permanent drain and no phone window.
Return waits for the first actual stream/segment observation; it does not
invent timing to bypass bootstrap. A matching window ACK is consumed only
after rechecking current activation. Successful `restore()` publishes its exact
`stats().arrival_floor`; the same generation is armed with that boundary.
Taking another clock reading later would change its meaning and is prohibited.

The receiver calls input draining on its normal owner turn even without new
encoded video. It uses accepting callbacks, not a vector that marks input
delivered before the HID queue accepts it. Legacy preview reports now have
their own retaining mailbox too; their cancellation survives window destruction.
Phone sender exceptions remain phone/control failures, not optional renderer
faults. Teardown stops the pipeline at NULL, destroys `StandardPreview`, then
destroys its borrowed `GstPreviewOutput` before releasing the pipeline.

The isolated fixture below qualifies GPU receipts, repeated native camera
transitions, generated-source expiry/recovery and restoration. Live phone
continuity, physical focus/touch, real capture and two-screen scanout remain
qualification gates. The installed-camera admission rule stays closed until
those checks pass.

## Verification

### Component evidence recorded before the Linux handoff

These checks use actual GStreamer/threads/private files with explicitly fake
presentation boundaries where indicated. They are distinct from the real CM5
GPU run below.

| Check | Recorded result and boundary |
| --- | --- |
| `preview-output` | Eleven lifecycle groups pass on Mac on 3 October 2026: replacement, timestamp/epoch rejection, sink finalization, preserved errors and EOS/flush. Actual GStreamer pipeline/test clock; no display proof. |
| `software-video-preview-output-handoff` | 18/18 dependent H.264 frames decode; three restorations admit twelve visible frames with the same parser/decoder. Generated source, fake sink. |
| Decoded-frame adapter | Eight actual GStreamer groups pass: pixel/stride/storage validation, immutable copies, queue bounds, timestamps/segment provenance, EOS and finalization. |
| `standard-preview-worker` | Twelve portable groups plus suspension checks: real worker ownership, generation/source floors, bounded storage/retries, leases, faults and cancellation. Explicit fake GPU/receipts. |
| `standard-preview-controller` | Nine actual GStreamer groups with a synchronous fake worker: ACKs, restoration boundary, rapid activation, missing segment, drain failure, EOS/epoch retirement and retained release. |
| `software-standard-controller-handoff` | Production `GstVideoPreview` decodes the generated stream through repeated returns; presenter remains fake. |
| `standard-video-output` | Nine routing/containment groups: independent cluster, mailbox cancellation/wakeups, optional output faults and control-sender exceptions. Explicit fake preview. |
| `standard-preview-configuration` | Fourteen loader/environment/preflight cases, including paired endpoint rejection. No display opened. |

Eight affected portable CTest suites pass. Focused strict Clang,
project-source ASan/UBSan and ThreadSanitizer checks pass; GStreamer/GLib and
other linked dependencies are uninstrumented. GNU Linux-header syntax checks
on Darwin use socket declaration shims and are not Linux execution. The reset
audit's expected GStreamer warning is not presentation evidence. These checks
did not install a receiver/profile/service. Generated-stream test registration
uses `HEADUNIT_TEST_SOFTWARE_PREVIEW=ON`.

### Qualify an isolated real-display handoff

`standard-surface-handoff-wayland` is a separate Linux qualification fixture.
It connects the actual `StandardPreview` controller, owned GPU worker,
GStreamer output and native camera display. It does not replace a compositor,
modify an appliance service or load a phone capture. Two private headless GL
compositors provide independent main and cluster endpoints.

The wrapper generates the same eighteen dependent baseline H.264 frames used
by the software-preview check. Encoded input receives genuine local running
time before handoff work; fixture packet timestamps are not presentation or
phone-clock authority. Only actual source-correlated Wayland feedback can
make the restored main output touch-ready. The cluster uses an original
independently presented fixture, rather than a signed instrument layout.

The 320 × 240 fixture requires actual progress through these cases:

| Case | Required evidence |
| --- | --- |
| Camera active before video startup | No main window/source/input qualification; native missing-video status and cluster actually present. |
| Return while the camera still owns its window | Exclusive lease prevents a Standard window or source from becoming ready. |
| Encoded frame queued before return | The retained decoder processes that same old frame; the restored gate rejects it. A genuinely newer frame receives current-generation presentation feedback before touch becomes ready. |
| Missing, fresh, stale and recovered camera | Actual native status/source receipts advance; camera ownership stays exclusive and cluster receipts keep progressing. |
| Repeated and rapid requests | Retired window generations cannot restore source or touch; each real return advances the original running-time boundary. |
| End of video while hidden | Actual bus EOS and the specific decoded-source-ended failure retire main presentation and release its lease; an unrelated GPU failure cannot count as success. |

The original source, parser and decoder remain PLAYING, with unchanged identities
and exact original ingress stamps across all eighteen frames. Success reports
decoded, admitted, rejected and actual presentation counts. This direct-component
fixture is separate from the production `GstVideoPreview` software replay with
its explicit fake worker.

Build with `HEADUNIT_BUILD_WAYLAND=ON` and `HEADUNIT_BUILD_THEME_GPU=ON`.
Test registration additionally requires Weston and FFmpeg; execution requires
the GStreamer H.264 parser/software decoder plugins and a working headless GL
renderer. These are fixture dependencies, not added receiver requirements.

```sh
ctest --test-dir /absolute/build-directory \
  -R '^standard-surface-handoff-wayland$' --output-on-failure
```

The wrapper owns a `0700` temporary runtime and every child process group.
Readiness requires live owned compositors and real sockets, not just matching
filenames. Cleanup includes descendants of already-exited tools and runs after
startup errors, probe failure, timeout or interruption. The portable
`standard-surface-handoff-wrapper` test uses explicitly fake tools and real
private Unix sockets to exercise that wrapper; it makes no GPU or codec claim.
Sixteen portable wrapper/fixture-validation cases pass, including descendants,
interruption, overflow, invalid sockets and incomplete dependent sequences.
Strict Clang and GNU C++ checks pass for the actual probe. The shared generated
stream still passes the eighteen-frame production software-preview replay and
its nine actual GStreamer/fake-presenter controller groups.

### CM5 Linux/GPU execution — 4 October 2026

The prepared fixture passes on the CM5 using a freshly built probe and private
copies of its wrapper/support module. The relevant source hashes match the
current repository and coherent source. Two exclusively owned headless Weston
14.0.2 GL instances provide the main and cluster endpoints. Both report
`/dev/dri/renderD128`, Broadcom V3D 7.1.10.2, OpenGL ES 3.1 and Mesa 26.2.2.
These are real GPU/Wayland operations, separate from the earlier fake-presenter
checks.

| Observation | Result |
| --- | --- |
| Original dependent H.264 input submitted / decoded | 18 / 18 |
| Main source images accepted / genuinely presented | 8 / 8 |
| Pre-return encoded frames decoded, then rejected at the return floor | 2 |
| Independent cluster presentations | 77 |
| Native camera/status presentations | 15 |
| Native fresh / missing / stale presentations | 8 / 6 / 1 |
| Retained protocol trace presented / discarded events | 100 / 0 |

The main output starts hidden while the native camera owns the exclusive
surface lease. Returning while that lease remains held cannot construct or
qualify a phone window. The same original source/parser/decoder remain PLAYING;
all eighteen source timestamps retain their genuine local ingress times. Two
frames deliberately held before return decode afterward and are rejected by
the new floor. Only a strictly newer, current-generation source with actual
presentation feedback authorizes `touch_ready`.

Generated camera images and actual missing/stale status scenes present while
native ownership remains exclusive. Source expiry does not restore CarPlay
under the camera. Recovery presents fresh source again. Repeated and rapid
requests retire abandoned generations without rearming their source or input.
The independent cluster keeps advancing real receipts throughout. Hidden EOS
produces the specific `Standard decoded source ended` failure, retires main
presentation and releases its lease; an unrelated GPU failure cannot satisfy
that assertion.

The probe exits zero after 2.775 seconds. The bounded wrapper reaps all four
owned subprocesses with exit status zero and removes its private runtime.
Only its runtime parent and evidence-retention location were adapted for this
run, keeping artifacts on eMMC rather than the crowded `/tmp` filesystem;
assertions, deadlines, generated codec input and presentation components were
unchanged. The coherent GCC build retains disclosed legacy warning exceptions:
global `-Wno-misleading-indentation` and gauge-target `-Wno-maybe-uninitialized`.
This execution does not establish a clean default GCC build.

Private evidence is under
`/home/headunit/.cache/headunit-qualification/20261004/standard-surface-handoff-check/`.
The source manifest and copied executable hash identify the tested code.
`artifacts/run-1/report.json` records commands, counts and process cleanup;
retained logs and generated samples contain only fixture traffic.

This establishes source-correlated presentation and exclusive surface ownership,
not physical touch/focus or an independently checked compositor pixel image.
The H.264 software decoder and camera images are generated fixtures; no phone
capture, actual camera device, signed cluster asset or hardware decoder is used.
Physical scanout, live-phone continuity, real capture and latency remain open.
It is **not** evidence of signed Ultra Punch-through: that feature must keep
the existing Ultra session and insert the camera into its main scene. No live
compositor, receiver, radio, profile or installed-admission rule was changed.
