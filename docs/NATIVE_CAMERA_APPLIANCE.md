<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Present a local camera on the main display

Use `headunit-camera-wayland` for an opt-in, native Linux camera view. It renders directly through the project's GPU compositor on a named Wayland output. Its clock is the host's real monotonic clock; it does not fabricate a qualified AirPlay clock or send a phone a camera command.

The executable supports a clearly labelled synthetic test pattern and an explicit Linux V4L2 source. Use the disabled [V4L2 template](../config/camera-v4l2.example.json) to select one packed streaming camera, with preferred YUYV DMA-buf export or explicitly requested CPU copying. An `external` source displays **CAMERA UNAVAILABLE** because no external frame-ingress adapter is connected to this executable. Real V4L2 capture/import remains unqualified on the CM5; this is not a qualified reversing-camera installation or a phone-controlled Ultra punch-through feature.

As of **4 October 2026**, the separate Standard/native direct-component handoff
passes isolated CM5 GPU tests. The updated daemon's combined GPU/HCS1 status
lifecycle and physical camera operation have not been established. Connected
Ultra's intended camera path stays inside its existing signed main scene; see
[the Ultra guide](CAMERA_ULTRA_GUIDE.md).

## Supply four explicit files

```text
headunit-camera-wayland \
  --camera-profile /absolute/path/camera.json \
  --display-config /absolute/path/displays.conf \
  --request-file /absolute/private/runtime/camera-request \
  --surface-lease-file /absolute/private/runtime/main-surface-lease
```

All paths must be distinct, absolute and bounded to 4,000 bytes. Dot traversal components are rejected. The camera profile and display configuration are static for the lifetime of the process: restart the executable to load changes.

The camera profile uses the schema described in [Camera provider](CAMERA_PROVIDER.md). Both `enabled` and `manualActivation` must be `true` to create a camera surface. The appliance accepts only `destination.panel = "main"`. It applies the configured destination rectangle, clockwise rotation and horizontal mirroring to the capture, fitting the whole oriented image inside the rectangle.

The supplied display file must contain a valid pair of main/cluster display records and explicit `main.wayland` and `main.output` names. The executable uses only the main output. The camera profile's canvas is its logical render canvas; the GPU compositor fits that canvas, preserving its aspect ratio, into the physical fullscreen size reported by Wayland. Changing the main display's advertised dimensions does not rewrite the camera profile's canvas or destination rectangle. There is no cluster-camera route.

Configuration files must be regular files owned by the running user or root, have no group/other write permission or special permission bits, and contain at most 8 KiB. Symlinks, pipes, duplicate/unknown configuration fields and inconsistent sizes are rejected. The configuration reader checks the file identity and modification time across the read.

### Validate without mapping a display

Append `--validate-only` to the same command. Validation does not contact Wayland, open a capture device, allocate a source/GPU surface, or create the lease file—even for an enabled V4L2 profile naming an absent device. It emits one bounded JSON record:

```json
{
  "configurationValid": true,
  "enabled": false,
  "wayland": "wayland-0",
  "output": "HDMI-A-1",
  "width": 1920,
  "height": 720
}
```

The `enabled` result is **effective native activation**: `enabled && manualActivation` from the camera JSON. It is `false` if either flag is false. A valid inactive profile is accepted for validation, and running it never maps a camera surface. The opt-in appliance installer rejects an inactive result so its main renderer cannot withdraw in favour of a disabled camera.

The `width` and `height` fields are the effective **main display configuration** dimensions, not camera capture or destination dimensions. Validation cannot establish that the named output is connected or that an installed receiver supports the main-surface handoff.

### Opt in through the appliance installer

The optional native-camera service uses three installer flags:

```text
--camera-profile /absolute/path/camera.json
--camera-control-file /absolute/private/runtime/camera-request
--camera-surface-lease-file /absolute/private/runtime/main-surface-lease
```

Supply all three together with the installer's existing `--user`, `--repo` and `--receiver-config` arguments. The installer's `--camera-control-file` becomes the executable's `--request-file`. Omitting `--install` previews/validates the service definitions; `--install` writes and enables the managed units without starting them. This opt-in does not change an existing live setup by itself.

The receiver must enable its installed native Ultra main renderer, and that renderer's **InstalledUi profile** must name exactly the same private endpoints:

```text
nativeCameraControlFile=/absolute/private/runtime/camera-request
nativeCameraLeaseFile=/absolute/private/runtime/main-surface-lease
```

These are local host settings, not identifiers sent to the phone. The installer
verifies the matching pair, a valid enabled native profile and a named main
output; it rejects a simultaneously configured signed-camera input. Its managed
phone-switching/resident Home route also rejects native camera services, so
these flags do not add a camera to the current Version 3 managed release.

Standard's opt-in `wayland-gpu` presenter implements source-correlated receipts,
surface leases and exact decoder restoration while retaining H.264 references.
Its isolated CM5 GPU/native handoff now passes; **installed Standard camera
admission remains closed** pending physical qualification. Use
[the Standard guide](STANDARD_VIDEO_OUTPUT.md#configure-the-receiver-controller)
for that component route. It does not qualify physical touch/focus or iPhone
Ultra camera controls.

## Request activation privately

The request and lease files need real parent directories owned by the running user with permission `0700`. Existing endpoint files must be owned regular files with permission `0600`. The lease must also be empty with exactly one hard link. The executable creates a missing lease safely; it does not create the activation request.

The request accepts legacy `on` followed by a newline or a managed `on <positive host-monotonic deadline in nanoseconds>` followed by a newline. Timed requests expire exactly at the deadline and cannot extend more than one second from a reader's current local time. The request is a bounded, single-link file of at most 24 bytes. An absent file, `off` followed by a newline, malformed data, an insecure file, or an unavailable/replaced parent directory all mean **off**. Request parsing is bounded and cannot block on a pipe or follow a final-component symlink. A controller should replace the request atomically using a temporary `0600` file in the same private directory. Never replace the lease file.

An `on` file alone expresses local activation. The optional [camera controller](CAMERA_ACTIVATION.md) can now produce it from a separate manual request or a fresh typed reverse observation. It preserves the original acquisition deadline and renews only bounded request leases. This does not imply a phone-control request or signed camera scene transition.

### Publish status for a managed controller

Add `--status-file /absolute/private/runtime/native.status` to supply the controller's `output.statusFile`. The native owner holds a separate exclusive writer lock at that path plus `.owner`, emits bounded HCS1 ASCII heartbeats and reports idle, waiting, active, fault or stopped. Active requires an actual native presentation receipt; an unavailable-status scene can qualify owner visibility without qualifying camera pixels. The status date is owner liveness, never a capture timestamp. Validation creates neither status nor its lock. See [activation and acknowledgement](CAMERA_ACTIVATION.md#understand-requests-and-acknowledgement).

## Coordinate ownership with the main renderer

A fullscreen camera window alone is insufficient: a hidden Ultra main surface can stop receiving presentation feedback and eventually fault the phone session. The installed main renderer must support the same **main-surface lease** and request file before enabling this service.

| Owner | Required ordering |
| --- | --- |
| Main renderer | Hold a shared lease while the main GPU surface exists. On native-camera request, destroy only that main surface, then release the shared lease. Keep the cluster's surface and session alive. |
| Native camera | Acquire an exclusive lease before allocating its source or GPU surface. If the lease is busy, remain idle without a window. |
| Native camera on off/fault/exit | Destroy its surface and retained GPU/source frames first, then release the exclusive lease. |
| Main renderer returning | Reacquire the shared lease before constructing its main surface. Restore only a still-valid underlying view/session. |

The executable verifies its pinned lease identity before every active render turn. A changed or invalid lease withdraws the camera and releases the old descriptor. Display/ownership and terminal capture faults withdraw the camera, publish fault if status is configured, and latch until a real off → on request. A replaced lease generation requires restarting against a coordinated lease generation; a terminal source needs a new generation. A request is rechecked after cold GPU/provider startup before its first submission. All runtime exit paths return presentation ownership before potentially slow provider destruction, including heartbeat/file failures outside the render catch. The executable never automatically starts, stops or repairs the main receiver.

Camera off destroys the mapped GPU surface and discards its frames before returning the main-surface lease. Driver shutdown runs independently in one retained capture owner. A new activation waits for that owner to finish; it cannot open a second device generation or block a normal display turn waiting for an ioctl. Final process shutdown can still wait for a wedged kernel driver; supervised recovery remains open. See [source retirement](CAMERA_PROVIDER.md#stop-and-retire-a-source).

The lock is an advisory coordination contract between cooperating processes. It cannot stop an unrelated compositor client from mapping a surface. Standard's opt-in owned output implements the same guard, but installed camera-service admission is still closed. Signed Ultra camera activation uses a separate route; this local view does not prove that the iPhone accepts Punch-through or that both routes may activate simultaneously.

## Render and handle input

The native window uses app ID `org.headunit.camera`. It consumes touch locally and has no phone HID route, so camera touches cannot open apps in a hidden CarPlay view. The optional `--manual-control-socket` adds a fitted rounded Close button. One new press submits an asynchronous manual cancellation to the dedicated owner; socket I/O never runs on the display turn. Pending/accepted/failed labels distinguish intent from actual withdrawal. Without the option, input is counted and discarded as before. See [native camera controls](CAMERA_CONTROLS.md).

Camera frames keep immutable source ownership through GPU completion and actual Wayland presentation receipts. The provider and presentation queue remain bounded; at most two Wayland submissions are pending. Capture timestamps and frame age use the real local monotonic clock. Missing or stale images show an explicit status rather than silently repeating old camera imagery. Simulation is visibly labelled. V4L2 capture and CPU conversion run on a dedicated capture worker, and never in a presentation poll.

SIGINT, SIGTERM and SIGHUP withdraw the camera and release its exclusive lease. They exit the process; SIGHUP does not reload configuration. If the compositor fails, the executable withdraws the camera and latches the request fault. It does not reconnect or restart the compositor.

## Observe bounded lifecycle events

The executable emits JSON only for lifecycle transitions. Events include `camera_idle`, `camera_waiting_for_main`, `camera_waiting_for_capture`, `camera_request_disabled`, `camera_opened`, `camera_source_fault`, `camera_closed`, `camera_fault` and `camera_stopped`. `camera_closed` includes presentation, fresh/missing/stale receipt counts, maximum pending submissions and consumed touch count. Errors contain at most 512 characters; frames and touch coordinates are not logged. `camera_source_fault` is emitted once for a closed capture generation; it is separate from a display ownership fault.

Presentation counts represent actual feedback validated against the local clock. A `camera_opened` event establishes surface creation, not visibility, focus correctness or a successful real camera capture.

## Verify without a phone

Strict configuration/CLI coverage:

```text
python3 tests/camera_wayland_test.py /absolute/path/headunit-camera-wayland
```

Optional real GPU/Wayland lifecycle coverage in the existing isolated compositor harness:

```text
python3 tests/theme_wayland_gpu_test.py /absolute/path/weston \
  python3 tests/camera_wayland_test.py /absolute/path/headunit-camera-wayland --lifecycle
```

The lifecycle probe uses only temporary files and its newly created headless compositor. It tests shared-lease waiting and cancellation, fresh synthetic presentation receipts, honest missing-source receipts, off/on reopening, disabled activation, signal cleanup and active lease replacement. It does not edit a live appliance service or test an iPhone transition. Use `--output NAME` if the isolated compositor's output has a name other than `headless`.

### Established evidence — 4 October 2026

- The [Standard/native direct-component fixture](STANDARD_VIDEO_OUTPUT.md#cm5-linuxgpu-execution--4-october-2026) passes with real GPU receipts, 18/18 dependent decoded frames, native exclusive ownership, missing/fresh/stale/recovered generated camera scenes and an independent cluster.
- The [seven controller/IPC suites](CAMERA_CONTROLS.md#cm5-control-qualification--4-october-2026) pass on Linux with an explicitly synthetic HCS1 presentation publisher. They establish command/renewal/expiry behaviour, not camera visibility.
- The updated `headunit-camera-wayland --status-file` GPU/HCS1 lifecycle is **not yet established**. Neither of the results above substitutes for that daemon run, a real camera or physical focus/touch.

The qualification runs did not install a camera profile or change running
services. Follow [the current handoff](HANDOFF.md) for the actual appliance
release rather than assuming a component test was deployed.

## Remaining integration

- Qualify the explicit V4L2 provider on Linux with actual camera backing, format/import/fence support and acquisition-reference semantics. Add other capture adapters separately.
- Qualify the installed main-only handoff, touch focus and main/cluster continuity with an actual phone and physical displays.
- Coordinate camera exit with the implemented resident native Home page. That managed route currently rejects camera combinations; camera-to-Home recovery is unqualified. Closing this executable does not restore a home page by itself.
- Qualify the implemented manual/fresh-reverse controller and opt-in native activation/exit UI with the native display and physical car adapter.
- Qualify source startup, missing/stale behaviour, disconnects and end-to-end camera latency on the target hardware.
- Validate any separate signed Ultra camera route independently. This executable does not send a fake phone protocol message or claim qualification.
