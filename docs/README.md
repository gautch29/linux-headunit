<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Documentation

Start with the current project summary and handoff. Topic guides describe the
interfaces and implementation; their dated test results do not mean every source
change is installed or every hardware path is qualified.

## Start here

| Document | Use it for |
| --- | --- |
| [Project README](../README.md) | What the project does, what works and how to build it. |
| [Development handoff](HANDOFF.md) | Bench inspection and qualification workflow. |
| [Current goal](GOAL.md) | Priorities and the acceptance gates still open. |
| [Roadmap](ROADMAP.md) | Twelve remaining workstreams, hardware queue and deferred scope. |
| [Architecture](ARCHITECTURE.md) | Component ownership, data flow and boundaries. |
| [Source provenance](PROVENANCE.md) | First-party code and external/private resources. |

## Primary topic guides

### Configuration, startup and phone management

- [Receiver configuration](CONFIGURATION.md)
- [CM5 appliance startup and backup cluster](APPLIANCE_STARTUP.md)
- [Boot/CarPlay timeline, source optimizations and bounded measurement](BOOT_SPEED.md)
- [Linux Head Unit startup animation and live renderer](STARTUP_SPLASH.md)
- [Local startup/goodbye sequence](GREETING_SEQUENCE.md): nonblocking cluster decoration, main component and settings data.
- [Signed Ultra session greeting](ULTRA_GREETING.md): STARTUP → DRIVE and optional verified first-pair insertion.
- [Application releases and rollback](APPLICATION_RELEASES.md)
- [Phone manager startup](PHONE_MANAGER_BOOT.md)
- [Native Home and main-surface ownership](NATIVE_HOME.md)
- [Add a phone](PHONE_PAIRING.md)
- [Phone page, Standard mode and media](PHONE_PAGE_MEDIA.md)
- [Native cluster center pages](NATIVE_CLUSTER_PAGES.md)
- [Phone-free signed local cluster](LOCAL_ULTRA_CLUSTER.md): saved-choice admission, local clock and scoped CM5 evidence.
- [Optional head-up display](HUD.md): third local output, dated guidance, reflection and optical calibration.

### Media, audio and navigation

- [Audio output](AUDIO_OUTPUT.md): encrypted AAC, PCM timing, output queues and faults.
- [Head-unit audio controls](AUDIO_CONTROLS.md): gain/mute requests and confirmed state.
- [Turn-by-turn metadata](ROUTE_GUIDANCE.md): route instructions for the native cluster.

### CarPlay and Ultra protocol

- [CarPlay Ultra integration guide](CARPLAY_ULTRA_GUIDE.md)
- [OEM alignment audit](OEM_ALIGNMENT.md): public Apple design/architecture targets, current boundaries and implementation order.
- [AirPlay control endpoint](AIRPLAY_CONTROL.md)
- [iPhone interface audit](IPHONE_PROTOCOL_AUDIT.md): implemented control,
  event, iAP2, UI-Sync and vehicle routes, observed requests and missing semantics.
- [Missing iPhone behavior](IPHONE_PROTOCOL_GAPS.md): 72 technical reference IDs with affected 11 October status corrections;
  contracts/qualification inventory, not 72 active TODOs.
- [Wireless handoff and connection requests](WIRELESS_HANDOFF.md)
- [MFi authentication coprocessor](MFI_COPROCESSOR.md)
- [Asset transfer and verification](ULTRA_ASSETS.md)
- [Ultra message encoding](ULTRA_MESSAGES.md)
- [UI session integration](UI_SESSION.md)
- [UI transport](UI_TRANSPORT.md)
- [Connection-scoped script worker](UI_WORKER.md)
- [Lua theme host](LUA_HOST.md)
- [Phone clock](PHONE_CLOCK.md)
- [Session observations](SESSION_OBSERVATION.md)

### Rendering, input and cluster overlays

- [Ultra GPU renderer](GPU_RENDERING.md)
- [GPU display ownership](GPU_DISPLAY.md)
- [Standard CarPlay video output](STANDARD_VIDEO_OUTPUT.md)
- [Native Ultra video](NATIVE_VIDEO.md)
- [Native phone video input](NATIVE_INPUT.md)
- [Vehicle warning overlay](ULTRA_WARNING_OVERLAY.md)

### Vehicle data, Climate and settings

- [Vehicle input and indicators](VEHICLE_INPUT.md)
- [MCU to Linux vehicle contract](MCU_VEHICLE_CONTRACT.md): dated signal inventory,
  proposed night-mode/clock interfaces and isolated socket simulator.
- [Per-signal age and provenance](VEHICLE_OBSERVATIONS.md)
- [Vehicle observation wire protocol](VEHICLE_OBSERVATION_PROTOCOL.md)
- [Vehicle distribution](VEHICLE_DISTRIBUTION.md)
- [Vehicle commands and confirmation](VEHICLE_COMMANDS.md)
- [Ambient lighting controller and UI data](AMBIENT_LIGHT.md): dated screen/mode colour, night dimming, atomic RGBW command and simulator.
- [Transmission state](TRANSMISSION.md)
- [Drive modes](DRIVE_MODES.md)
- [Trip statistics and heading](TRIP_AND_HEADING.md)
- [Sport telemetry engine](SPORT_TELEMETRY.md): g/peaks, acceleration/lap timers,
  acquired IMU/GPS HVO6, manager JSON, history and source-only simulation.
- [Climate and car-settings overview](CLIMATE.md)
- [Climate/settings integration](VEHICLE_STATE_GUIDE.md)
- [Climate/settings configuration](VEHICLE_CONFIGURATION_GUIDE.md)
- [Vehicle capability catalogue](VEHICLE_CAPABILITIES.md)
- [Pinned iPhone vehicle type inventory](VEHICLE_PROTOCOL_CATALOGUE.json): all
  34 accessory and 112 service families, reviewed receiver support and 500
  declared characteristic accessors from pinned framework listings.

### Camera integration

- [Local lane, seat/door and camera views](VEHICLE_VIEWS.md)
- [Punch-through camera guide](CAMERA_ULTRA_GUIDE.md)
- [Camera providers](CAMERA_PROVIDER.md)
- [Manual/reverse activation](CAMERA_ACTIVATION.md)
- [Native camera controls](CAMERA_CONTROLS.md)
- [Native camera display appliance](NATIVE_CAMERA_APPLIANCE.md)

## Qualification, measurements and research

Use these for test boundaries, measured results and investigation context.
Deployment authority and current priorities remain in [HANDOFF](HANDOFF.md) and
[GOAL](GOAL.md).

- [CM5 bring-up record](CM5_BRINGUP.md)
- [Local and hosted CI](CI.md): all six jobs, Docker ownership, sanitizer flags,
  caches and component evidence limits.
- [Independent lab receiver](LAB_RECEIVER.md)
- [Hardware measurements and targets](MEASUREMENTS.md)
- [Ultra performance measurements](ULTRA_PERFORMANCE.md)
- [Remaining GPU work](GPU_NEXT_STEPS.md)
- [Touch-to-display latency](TOUCH_LATENCY.md)
- [Output fault recovery](OUTPUT_FAULT_RECOVERY.md), including the source-only
  [scripted daily-use harness](OUTPUT_FAULT_RECOVERY.md#scripted-daily-use-reliability-harness)
- [Cluster completeness qualification](CLUSTER_COMPLETENESS.md)
- [Cluster style review](CLUSTER_STYLE_REVIEW.md)
- [Climate/settings qualification](CLIMATE_QUALIFICATION.md)
- [Climate research record](CLIMATE_RESEARCH.md)
- [Asset discovery](ASSET_DISCOVERY.md)
- [Vehicle adapter plan](VEHICLE_ADAPTER_PLAN.md)
- [Earlier theme migration](THEME_MIGRATION.md)
