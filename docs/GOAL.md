<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Current project goal

Updated **13 October 2026**. This file defines the outcome and acceptance gates;
it is intentionally not a progress log. [README](../README.md) owns feature
status, [HANDOFF](HANDOFF.md) owns the current deployment, and
[ROADMAP](ROADMAP.md) turns the gates into next actions.

## Outcome

Build an independent, efficient, configurable C++ Linux head unit targeting a
wireless CM5 with a main touchscreen, a separate instrument display and an
optional third native head-up display. The
platform must adapt to different cars through replaceable interface boards and
explicit capability profiles, including older analogue cars and newer bus-based
cars. It must manage Standard CarPlay, CarPlay Ultra, phone
connections, audio/media and a clean native interface without a phone. Boot
quickly into the interface without exposing the Linux desktop or boot console.

Deliver an OEM-quality, coherent interface. The native main and cluster should
use a consistent visual language and preserve useful controls/instruments
through phone loss, with minimal visible disruption. Keep architecture and code
powerful but simple: one clear owner per resource, direct contracts and bounded
work. Avoid parallel state models, redundant abstractions and stacked workarounds.
Coordinate software integration with the separate hardware repository.
It owns electronics, mechanical design, component choices and installation wiring.
This software project owns typed acquisition/control interfaces, capability
admission, UI behavior and Linux deployment.

Provide a replaceable, typed interface for car observations and future controls;
the carrier PCB/sensor design is still undecided. Keep a useful native cluster
when the phone or Ultra is unavailable. Its center pages include Map when
connected, Media, Vehicle and Trip. Show warning lights only when their source
state is fresh and active, using dashboard symbols rather than descriptive text.

Use no original LIVI repository code. Our own research code written here can be
reused. Keep third-party signed assets, phone captures, keys and credentials out
of Git. Preserve provenance and document the inputs and checks alongside
each source release.

## Current scope and priorities

Stay with the installed signed pack. Finish camera Punch-through; complete
the remaining visual/lifecycle checks for V67's arrows and terminal camera faults
plus V68's deployed vehicle-resource candidate;
remember cluster settings and start useful local Ultra gauges before phone
reconnection; test higher main-screen resolutions/formats. The consolidated
remaining work and order belong only in [ROADMAP](ROADMAP.md), with current
feature evidence in [README](../README.md) and release bindings in [HANDOFF](HANDOFF.md).

Preserve main/touch, both panels, native fallback, simulated Climate/settings,
the bottom warning row, signed session greeting and existing pairings. Thirty fps
is accepted for feature work; sustained full-resolution fresh 60 fps remains a
goal. Other packs, bespoke signed gauges and optional research are outside the
active plan. Physical acquisition, actuators and wake/power integration require
the separate hardware repository and actual equipment.

## Active work and acceptance

All nine gates remain open. An isolated passing fixture closes only its stated
component check. Keep earlier working configurations recoverable and record
source, inputs, environment and limits for each qualification.

### 1. Full-resolution rendering and frame timing

Render the authored 1920 × 720 Ultra main and cluster scenes efficiently on CM5.
When the phone supplies 60 fps, sustain **60 distinct fresh frames/s** on both
outputs with a **16.7 ms** frame budget. Local animation/redrawing or advertising
60 fps is not that result.

Acceptance includes warm sustained runs, p95/outlier costs, correct picture and
touch, bounded queues/memory, original phone/capture timestamps, real compositor
receipts and recovery after overload. Cover heavy styles, masks, gradients,
video, active lights and both outputs. Do not manufacture frame dates or use
stale redraws to satisfy the target. See [performance](ULTRA_PERFORMANCE.md).

Establish supported higher output resolutions and alternative aspect ratios,
starting with the main screen. Qualify signed-canvas and phone-video constraints,
complete picture and readable layout, scaling/cropping, accurate touch, native
pages and camera fit in Standard and Ultra, with the cluster still active.
Measure resource use, fresh-frame rate and latency; retain a working fallback
configuration and record actual phone/display evidence for each supported
format. Physical panel selection belongs in the hardware repository. See the
[high-priority checklist](ROADMAP.md#high-priority-work)
and [configuration](CONFIGURATION.md).

### 2. Reconnect, output faults and automatic fallback

Recover after actual phone disconnect, reconnect and terminal worker/input/clock
failure. The native cluster must remain usable on the cluster output while the
healthy main/control path is retained when safe. Failback requires a fresh,
qualified scene; old buffers or clock epochs must not revive visibility.

Acceptance includes physical backup visibility, fresh-scene return, bounded
retirement, retry limits and long-duration operation. Normal disconnect handoff
is live verified; isolated output-withdrawal/clear-buffer tests do not complete
physical fault recovery. See [recovery](OUTPUT_FAULT_RECOVERY.md).

### 3. Complete cluster appearance and car information

Qualify every usable declared layout in the current pack and its supported
appearance/palettes. The present cluster declares dark appearance; do not force
unsupported light gauges or arbitrary palette combinations. Match gauge curves,
masks, labels, fills and backgrounds to authored artwork. Check Map, Media and Trip content rather than
counting script load as visual success.

Supply fuel, range, temperatures, odometer, transmission and observed drive mode
through typed dated inputs. Unknown/stale readings remain visibly unavailable.
Qualify the original 24-light overlay over real Ultra maps; show only fresh
active states, with correct flashing and clearing across switches. Position the
left/right turn signals at the top in native and Ultra clusters; hazard lights
flash both arrows together, without a separate hazard-specific lamp. Fresh
observed on/off phases take priority; flashing summaries use one 90/minute
fallback phase. Solid green arrows use gentle whole-lamp rise/decay; unknown
and stale inputs show faint dashed outlines, never a filled lamp or known Off.
Retain the
existing bottom warning row pending the user's final layout decision; other
vehicle fault/warning indicators are independent of hazards. Verify synchronized
hazard flashing, turn/hazard transitions, expiry/clearing and
placement across styles, Map, startup and fallback without collisions.
Optional ignition lamp checks must share vehicle/MCU authority and preserve actual
faults; no Linux boot or phone event may invent a current car lamp state.
Optional ADAS imagery is not an implemented car feature. See
[cluster completeness](CLUSTER_COMPLETENESS.md) and
[warning overlay](ULTRA_WARNING_OVERLAY.md).

### 4. Complete media and audio

Deliver actual audible phone music to the chosen hardware route. Qualify title,
artist, album/artwork, progress, play/pause/previous/next and local gain/mute.
Preserve bounded PCM scheduling and correct media/audio generation ownership.

Then implement and qualify navigation guidance mixing, focus/ducking,
microphone/Siri and call audio. Silent decoding, synthetic PCM and a Bluetooth
track skip do not prove an audible live path. See [audio output](AUDIO_OUTPUT.md)
and [audio controls](AUDIO_CONTROLS.md).

### 5. Climate/settings capabilities and current-pack compatibility

Current scope stays with the installed signed pack, as agreed on 11 October.
Qualification of other packs and bespoke gauge artwork is deferred for now.

Use semantic configuration files to describe the car's actual zones, seats,
ranges and supported controls. Apple Climate/settings must open, show only
configured capabilities and retain confirmed applied state. Qualify every
advanced control independently; physical providers must confirm their readings
and writes.

Keep the compatible simulated bench profile. **Restore airflow direction to
Climate without icon overlap**; its temporary placement in car settings is a
known lower-priority workaround. Cover driver/passenger, seats, steering wheel,
power/Auto and failure/recovery, without inventing wire identifiers.

Qualify installation, UI exchange, Maps, touch/controls and switching for the
current authenticated pack. Strict verification stays default; the explicit
development exception applies only to the expired signing leaf and preserves
chain/archive checks. Other identities/packs are not current acceptance work.
See [vehicle configuration](VEHICLE_CONFIGURATION_GUIDE.md),
[Climate matrix](CLIMATE_QUALIFICATION.md) and [asset verification](ULTRA_ASSETS.md).

### 6. Native phone management, Home and appliance startup

Qualify paired-phone listing, Add/confirm/reject/remove, explicit Connect/
Disconnect and preferred-phone reconnect. The iPhone chooses Standard or Ultra;
the headunit offers both configured capabilities and shows only fresh,
receiver-confirmed mode. Legacy saved mode hints cannot choose the session.
Buttons appear and accept input only when their action is usable.
A future physical Home button must use the same readiness/ownership route;
the on-screen button remains configurable.

Home entry withdraws the main renderer before covering it. Back requires fresh
real presentation and a usable connected session. Verify the selected generation
physically, including repeated Home/Back/Disconnect, both mode handoffs, manager
recovery and cold boot. Historical V3 checks do not qualify a later release. Measure Bluetooth connection to useful CarPlay as well
as power-on to useful backup and CarPlay, hide Linux
boot/desktop visuals, bound retries and qualify fix-forward recovery on the bench. See
[Home](NATIVE_HOME.md), [manager boot](PHONE_MANAGER_BOOT.md),
[application releases](APPLICATION_RELEASES.md) and [startup](APPLIANCE_STARTUP.md).

Remember the verified Local UI asset package and phone-confirmed compatible
gauge configuration so useful local Ultra gauges appear at vehicle wake/display
activation, before iPhone detection or reconnection and with no phone present.
Qualify disconnected script/timing behavior, fresh vehicle readings, cold boot,
warm wake, durable preference recovery, invalid-cache fallback and seamless
ownership transfer to the connected session. Never replay stale phone content or
fabricate phone timing. Physical door-open/approach wake and power policy belong
to the hardware interface; measure wake-to-useful-gauges independently of
reconnection. See the [high-priority checklist](ROADMAP.md#high-priority-work)
and its WWDC source for this open requirement.

Optional startup/goodbye decoration must leave useful gauges, warning lights and
CarPlay readiness independent of its animation duration. Qualify Off and each
variant, main/cluster integration, actual fresh ignition/wake events, immediate
retirement and on/off boot timing separately. Cosmetic requests cannot confirm
power state or extend the hardware owner's cutoff deadline. See
[local sequence](GREETING_SEQUENCE.md).

### 7. Physical vehicle adapters and future controls

Replace simulation with calibrated observations from the eventual PCB/sensor
interface. Preserve original acquisition time, availability, source generation
and immutable age policy across native/Ultra consumers and process restarts.
Deploy the tested HVO distribution path. Never guess selector/engaged gear or
an unavailable reading from RPM/speed.

Qualify trip identity/reset, accumulation/persistence and referenced heading.
Keep command permission, interlocks, bounded dispatch and confirmed applied
state separate from observations. Future windows/locks require a physical
executor and delivery/recovery semantics; a requested state cannot masquerade
as applied. See [observations](VEHICLE_OBSERVATIONS.md),
[distribution](VEHICLE_DISTRIBUTION.md) and [commands](VEHICLE_COMMANDS.md).

For fitted ambient lighting, follow a qualified observed screen accent or drive-mode
palette and the shared headlights/night appearance policy. Require smooth MCU-owned
fades, explicit current permission, bounded commands and independently dated actual
acknowledgements, with reset/link-loss/expiry and parked-drain qualification. Source-only
controller/simulator evidence does not establish LED or phone acceptance.

### 8. Reliable live cluster switching

Switch every usable declared style of the current pack through the existing
ordered UI-Sync session without phone reconnect or receiver restart. Connect
native selectors and configurable physical actions to one request path.

Show pending/confirmed/error state, coalesce rapid requests and handle rejection,
timeout or disconnect while retaining useful instruments/map/main/touch. Persist
only phone-confirmed choices by verified pack/version and restore through a ready
session/catalogue. Six regular-pack styles have historical real-phone confirmation
on one publisher; explicit centre-content transitions, supported palette changes,
current-pack recovery and preference restoration remain open. See [UI session](UI_SESSION.md).

### 9. Whole-session Ultra Punch-through UI and camera

Put locally captured camera pixels into the existing signed main scene, and
vehicle assistance/cabin pixels into their verified appropriate signed slots, while
**the entire CarPlay Ultra session continues**. Both signed panels, phone video,
UI-Sync, audio and control remain alive. Coordinate entry/exit with the phone's
camera resources and Remote UI controls/focus. No receiver restart or native
main-window replacement is part of connected Ultra acceptance.

Extend the qualified signed-GPU bridge lifecycle and manual real-phone
entry/exit to verified phone resource consumption, controls/focus and active
audio continuity. Preserve original capture dates, strict freshness, explicit
Off and generation retirement. Establish controls/focus from observed contracts;
current read-only resources are not that acceptance.

Add real physical capture/import and manual/fresh-reverse activation, bounded
leases, missing/stale/simulated state, source loss, death recovery and correct
return. Measure trigger-to-camera and frame age; phone clock must not govern the
standalone native camera. Seal the complete activation/owner graph before a new
managed release admits it. Keep Standard/no-phone native camera qualification
separate. See [camera integration](CAMERA_ULTRA_GUIDE.md),
[provider](CAMERA_PROVIDER.md) and [activation](CAMERA_ACTIVATION.md).

## Completion and evidence rules

A gate is complete only after its end-to-end behavior and failure path are
qualified. State whether evidence is portable, actual Linux/GPU, simulated or
real phone/physical hardware. Retained manifests establish which generation ran;
compile success and a service being active do not establish visible behavior.

Update the topic guide's scoped evidence, README feature status, this checklist
and the handoff when ownership/deployment changes. Archive obsolete planning
history instead of appending another transcript to the current goal. Do not
mark the entire project complete because one experiment passes or a work session
ends.
