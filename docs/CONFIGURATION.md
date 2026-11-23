<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Receiver configuration

This page explains which file owns each decision and how to change the **standalone lab receiver** without confusing it with the selected **Version 3 Home-readiness release**. [Application releases](APPLICATION_RELEASES.md) explains publication and rollback; machine-specific paths and service state belong in the project handoff. Keep identities, keys, Wi-Fi credentials, pairing stores, captures and signed assets outside Git.

## Choose the right configuration owner

| Owner | Inputs | Current status |
| --- | --- | --- |
| Standalone lab launcher | Saved receiver JSON, display file, private installed-UI profile and external secrets | Runs an opt-in bounded test, or a supervised `seconds: 0` session. Explicit CLI options can override saved settings. |
| Selected V3 application release | Sealed receiver/display/Ultra/manager/Home roles plus external runtime state | Selected and service-path checked on the CM5. Its static files cannot be overridden by an ad hoc CLI flag; publish a new retained release for changes. |
| Experimental camera bridge | Standalone receiver `camera_state_config` plus an enabled installed-UI camera input | Portable and isolated CM5 checks only; **not in the selected V3 release**, not accepted by an iPhone. Managed/sealed releases reject it. |

Do not run the standalone launcher alongside the managed receiver on the same Bluetooth/Wi-Fi and display outputs. For the standalone bench, copy `config/receiver-cm5.example.json` and a matching display example to a private directory, then edit paths. `config/displays-cm5-dual.conf` names two physical outputs on one Wayland socket; `config/displays-cm5.conf` is the older split-socket bench example. Neither file is the sealed release's live profile.

```sh
sudo python3 tools/run_lab_receiver.py --config /home/headunit/.config/ultra-headunit/lab/receiver.json --dry-run
```

The dry run checks resolved identity, display geometry, capability files and local AP settings before changing radio state. A live standalone run uses the same command without `--dry-run` after any competing owner is stopped. Values in the JSON file take effect on restart; phone-facing capability and display changes require a new iPhone session. [Lab receiver](LAB_RECEIVER.md).

## Receiver JSON and private identity

### Optional numeric PCM observations — 14 October source

`audio_observation_directory` is an optional canonical absolute directory in
the receiver JSON; the standalone CLI uses `--audio-observation-directory`.
Each receiver launch names a new `pcm-<local-random-id>.jsonl` destination for
`HEADUNIT_AUDIO_OBSERVATION_FILE`. The name is a storage identity, not an
acquisition clock. Parsing and launcher preflight create no file. The C++ trace
owner must admit a real UID-owned mode0700 parent and exclusively create a new
mode0600 file; redirected/unsafe destinations remain rejected by that owner.
Keep traces outside Git and check their original dates, coverage/loss and footer.

Without this explicit setting, the launcher clears an inherited observation-file
environment variable. It also preserves the existing audio/microphone admission;
this option does not change volume, sink, capture permission or PCM scheduling.
The managed receiver carries the directory from its sealed profile and chooses
a fresh destination for every child launch, so reconnect cannot overwrite an
earlier trace. Enable it only in copied validated inputs for a new immutable
generation with a compatible helper. Do not add a service override or modify the
selected generation to inject it. This adapter is source qualification, not a
deployed trace or evidence of repaired audio continuity. Exporter correctness and
actual coverage remain subject to the reviewed [audio contract](AUDIO_OUTPUT.md).

The JSON selects paths, launch behavior and presentation labels. Relative paths are resolved against the JSON file's directory. Saved inputs are bounded to 16 KiB and read from a
stable regular file without waiting for FIFO/device input. Ordinary source
configurations still allow explicit symlinks and hard links; a symlink's target
directory supplies relative settings. Replacement or mutation during loading
fails before launch or offline configuration publication. Sealed releases
retain their separate owner, permission, link and manifest checks. The private identity file owns stable pairing identifiers, display UUIDs and protocol flags. The launcher creates a session copy with requested labels; it does not regenerate pairing keys or UUIDs.

| Setting | Meaning |
| --- | --- |
| `device_name`, `device_model`, `manufacturer`, `serial_number` | Shared Bluetooth/iAP2/CarPlay labels. Keep the unit's serial stable. |
| `firmware_version`, `hardware_version`, `touchscreen_name` | Presentation labels; a label cannot enable a protocol feature. |
| `wifi_config`, `wifi_country`, `identity`, `pairing_key`, `setup_secret`, `pair_store` | External authentication, handoff and persistent pairing inputs. |
| `display_config`, `ultra_installed_ui`, `ultra_negotiate`, `ultra_native_video` | Geometry, signed theme and native Ultra route. Ultra requires all matching inputs. |
| `capture_root`, `bin_dir`, `seconds` | Private diagnostics, executable directory and session duration; `0` is unbounded under supervision, otherwise 1–3600 seconds. |
| `auto_connect_phone` | Optional selected paired phone for bounded connection attempts. It cannot perform pairing or repair an established stalled session. |

Label overrides accept printable ASCII; the device name is limited to 63 characters and other labels to 127. `--dry-run` validates the derived identity without changing the Bluetooth alias or network. The AP topology is presently fixed to `wlan0`, `10.10.0.1/24` and control port 7000; the example uses channel 36. [Wireless handoff](WIRELESS_HANDOFF.md).

## MFi authentication

Hardware MFi is the supported configuration and remains the default, using
`mfi_device`/`mfi_address`. The software-provider implementation is retained for
research, but its launcher flags are hidden from normal help and it is not a
phone-page setting. Work on its incomplete CarPlay continuation is deferred at
the user's request. Existing explicit research configurations still parse; see
[archived provider evidence](MFI_COPROCESSOR.md#real-phone-trial--9-october-2026).

## Display file: one geometry for phone, scene and touch

| Field | Meaning |
| --- | --- |
| `main.width/height`, `cluster.width/height` | Advertised stream and logical panel dimensions; main HID coordinates use the main logical canvas. |
| `*.fps` | Advertised ceiling and scheduler target, **not measured throughput**. |
| `*.panel` | Entry-point name that must exist in the installed signed pack. |
| `*.wayland`, `*.output` | Compositor socket and optional named `wl_output` for fullscreen placement. |
| `*.width_mm/height_mm` | Physical-size fields; Ultra requires both. Current bench values describe a fitted virtual panel and need measurement for final hardware. |
| `*.asset_width/height` | Optional authored scene canvas when it differs from the stream canvas; both fields are required together. |

The inspected pack authors both Ultra scenes at **1920 × 720**. The working main HDMI mode is **1280 × 720**. The GPU fits the full main scene into **1280 × 480**, centered with two 120-pixel inactive bars. Touch is inverse-mapped through that same fit. The example advertises main `155 × 58 mm` for the logical wide panel and cluster `200 × 75 mm`; these are bench estimates, not verified physical measurements. Advertising the main screen's approximate 16:9 dimensions with the pack's wide logical canvas produced overlapped phone artwork. A 1280-wide phone stream also arrived already cropped on its right edge. Local scaling cannot recreate missing source pixels.

The physical compositor mode is separate from the advertised stream. The dual-output bench uses labwc plus `config/kanshi-cm5-dual.conf` for HDMI modes and `config/labwc-cm5-dual-rc.xml` for touch mapping. The display parser rejects duplicate/unknown keys, unsupported sizes and partial physical/canvas dimensions before network changes. A different asset or display needs a fresh panel, video, fit and touch test; changing only `*.panel` cannot adapt a signed pack. [GPU display](GPU_DISPLAY.md), [CM5 bring-up](CM5_BRINGUP.md).

A fresh read-only V68 bench inventory (11 October) reports main `HDMI-A-1`
currently 1280 × 720 at 60 Hz, with 1024 × 768 (4:3) and smaller advertised
alternatives, and no higher advertised mode. Cluster `HDMI-A-2` is currently
1920 × 1080 at 60 Hz and advertises 2560 × 1440 at about 59.95 Hz. No mode,
negotiation or sealed profile was changed. Higher physical main resolution needs
a suitable panel; alternative-aspect rendering, camera fit and physical touch
still need controlled qualification. This inventory does not establish fresh
frame rate or negotiated video resolution. Private sanitized mode receipts are
`display-mode-inventory-v68/` on CM5/Mac.

## Installed Ultra profile and vehicle state

The private installed-UI profile selects the signed bundle, its verified identity and optional exact historical acceptance reference, local vehicle bindings, diagnostics and renderer options. Resource verification precedes execution. A historical receipt reopens only its matching package. The reference pack's `Center_Display` and `Cluster_Display` names, identifier `44C9FA31EC` and version `985` are not generic defaults. [Asset trust](ULTRA_ASSETS.md).

Local gauge input comes from an explicit simulator or vehicle socket in that installed-UI profile. Missing values are unavailable, never assumed zero. The replaceable vehicle observation protocol preserves original acquisition dates and age limits; no car sensor reader is connected. [Vehicle input](VEHICLE_INPUT.md), [observations](VEHICLE_OBSERVATIONS.md).

Phone-facing vehicle-state resources use separate `climate_config` and `car_settings_config` JSON files. Set `vehicle_state_backend: "simulation"` explicitly and keep `vehicle_state_profile: "none"`; mixing capability files with a legacy research profile is rejected. The launcher preflights them with `headunit-vehicle-config-check` before radio startup. Configuration is fixed for each phone session, while simulated values can change. Standard phone selection omits the Ultra-only capability files. Apple's Climate and settings pages have opened and retained simulated changes; physical HVAC, advanced controls and actuators have not been qualified. [Capability configuration](VEHICLE_CONFIGURATION_GUIDE.md), [Climate research](CLIMATE_RESEARCH.md).

`vehicle_state_profile` remains a legacy test selector: `none` (default), `empty`, `climate-bench`, `settings-bench` or `bench`. Prefer semantic capability files for current experiments. `vehicle_trace_writes` can temporarily log bounded numeric requests and binding names; it does not change write behavior.

## Experimental connected Ultra camera

`camera_state_config` is an optional **standalone-only** third semantic capability file; `--camera-state-config` and `HEADUNIT_CAMERA_STATE_CONFIG` are the equivalent launcher and direct-receiver paths. It requires explicit `ultra_negotiate`, `ultra_native_video`, an installed UI with an enabled `cameraProfile`/`cameraControlFile` signed-input route, and the explicit simulation backend for other capabilities. The camera On value itself is owner-sourced, not simulated. Its stream UID must match the installed `camera` input and its age limit must exceed that input's frame-age limit. The example `config/camera-state.example.json` uses **151 ms** for a **150 ms** local source.

The checker accepts `headunit-vehicle-config-check CLIMATE.json|- SETTINGS.json|- [CAMERA.json|-]`; the third argument is optional for existing callers. Two distinct read-only phone On characteristics share one per-connection dated camera observation. This is a candidate resource topology, **not iPhone acceptance**. No CameraButton actions, Remote UI focus or touch routing are established. The signed main renderer remains active so the camera can appear as Punch-through while both panels, phone streams, UI-Sync, audio and control continue. Do not combine this route with native Home/main-surface replacement. The selected V3 release rejects this field. [Camera integration and provenance](CAMERA_ULTRA_GUIDE.md).

## Optional native HUD

The separate [HUD profile](HUD.md) names a third local output and its reflection,
placement, measured corner correction and `country=auto`/country fallback.
Fresh road-provider country selects the sign appearance and km/h or mph units;
unknown countries use a neutral limit display. It does not extend `display_config`
or signed CarPlay panel geometry. Optional receiver setting `navigation_socket`
is the HUD's own private route-guidance endpoint and requires `route_guidance=true`.
Vehicle fan-out can add a distinct `hud` consumer. No HUD is installed in V11;
future managed deployment must seal its program/configuration graph.

## Home, audio and other opt-in settings

The V3 manager derives isolated per-phone Standard or Ultra child settings and owns the resident native Home page. Saved `native_page_control_file` and `native_page_lease_file` pair a Home request with a main-surface lease; they must be private, distinct and configured together. The release seals the manager, Home and readiness contracts, while live state and secrets remain external. [Native Home](NATIVE_HOME.md), [phone manager](PHONE_MANAGER_BOOT.md), [release workflow](APPLICATION_RELEASES.md).

`audio_sink` chooses `none`, `fakesink`, `alsasink` or `pulsesink`; optional `audio_device` requires a matching audible sink. `audio_volume_percent` (0–100) and `audio_muted` configure the local PCM mixer, not Bluetooth volume. External gain/mute controls require both distinct private `audio_control` and `audio_state` paths. Authenticated AAC has reached the silent sink, and mixer behavior passes component tests; live audible output, guidance mixing and calls remain unqualified. [Audio output](AUDIO_OUTPUT.md), [audio controls](AUDIO_CONTROLS.md).

`media_state`, `cluster_page` and `direct_media` support the native cluster pages and selected-phone metadata. `fallback_cluster` keeps the backup cluster independent of the phone receiver. Renderer experiments such as deferred scene fences, queue depth, opaque windows, output isolation and transparent waiting have separate measured limits and default-off failure policies. Choose them from their topic guides rather than treating a flag as evidence of readiness: [native cluster](NATIVE_CLUSTER_PAGES.md), [performance](ULTRA_PERFORMANCE.md), [output recovery](OUTPUT_FAULT_RECOVERY.md).

## Phone-authoritative mode offer

Latest managed source retains the signed Ultra `display_config` and supplies a
separate validated `standard_display_config` to Standard preview owners. Both
profiles must share main connector/socket and cluster routing/canvas; main
logical dimensions may differ. Pre-negotiation `/info` keeps the authored Ultra
capability geometry and identifiers. Actual Standard phone video/fit/touch with
that offer remains a physical qualification requirement. No headunit mode
selector or saved mode hint changes capabilities. `connection_diagnostics` is
an explicit default-off boolean enabling at most 64 content-free host-date
records in the reconnect helper; inherited diagnostic environment is cleared.

## Phone appearance and car icon

`night_mode` is optional: `off`, `on`, `schedule` (local 20:00–07:00) or
`schedule:HH:MM-HH:MM`. When set, `/info` carries `nightMode` (which tells the
phone night mode is supported) and the receiver sends `setNightMode` after
RECORD and on every change. Without it nothing is declared. The schedule uses
the CM5 local clock; no light sensor or headlight source is implied.

`oem_icon` (default off) offers a Home icon to the phone. Standard CarPlay uses
the offer; Ultra also requires the phone-side pack's per-display
`allowManufacturerAppIcon`. The current `44C9FA31EC / 985` pack omits that field,
which defaults to false, so enabling this option does **not** show the icon in
Ultra. Keep the native page's `homeButton.enabled=true` for Home access; see the
[OEM investigation](IPHONE_PROTOCOL_GAPS.md#oem-home-icon--13-october-2026).
The phone's `oem:back` releases `mainScreen`; the receiver counts that in its
version-3 readiness report (`phoneHomeRequests`), the manager exposes
`phone_home`, and the native page opens Home. Closing Home while the car still
owns the screen sends `requestUI` so CarPlay returns. Native Ultra sessions
also declare a media-key HID device; with no iAP controls, the native page's
transport buttons use it (play/pause toggle, next, previous).

`multitouch` (default off) declares a two-finger digitizer: the existing
single-finger record repeated, each with its transducer index, tip, X and Y.
Reports then carry both contacts (12 bytes) so pinch and rotate reach the
phone. Enable it only with a panel that reports at least two touch slots;
without it the descriptor and 6-byte reports are unchanged.


The installed Ultra profile accepts `clusterGreetingBrand=asset|brand`.
Omission retains the signed pack's original branding. `brand` requires timed
session scene inputs and adds authored host decoration only during the genuine
cluster logos phase. It replaces only the manufacturer image, preserving the
divider and original Apple CarPlay Ultra artwork/motion. An unknown or ambiguous
co-brand structure remains unchanged. It does not change the signed pack or main-screen Hello.
See [greeting](ULTRA_GREETING.md) for lifecycle and qualification.

## Local gauges before the phone

`pre_phone_gauges` is an opt-in appliance setting requiring `ultra_installed_ui`
and the authored 1920x720 cluster. It uses the verified pack/profile on the
existing background layer. Asset/signature or cached-choice failure falls back to
native instruments. Local monotonic animation time never becomes a phone clock;
the required remote slot is empty and has no producer. The main/control owner is
unchanged. Phone handover, physical wake and power qualification remain open.

Installed profile `persistClusterLayout=true` retains the last confirmed layout
and palette in a private, pack/version/cluster-scoped record. Reads are preloaded;
copy/coalesce writes and fsync run on a disk worker. Only idle, declared,
phone-confirmed selections can replace the preference; live commands, phone
identity, content and pairing state are excluded. A missing palette falls back to
the current pack's own choice. Legacy layout-only records remain readable.


`trip_history` is a default-off boolean requiring `sport_telemetry: true`.
In a managed generation it opts into private archived software journeys and
seals `tools/trip_history.py` through the exact hash-bound receiver configuration.
It has no path override and uses the existing private manager state directory.
It restores only labelled history, never live TripState/odometer/counters or
freshness. [Trip archive contract](TRIP_AND_HEADING.md#private-software-journey-archives--source-only-14-october-2026).
