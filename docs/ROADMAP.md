<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Roadmap

[GOAL](GOAL.md) defines acceptance. [README](../README.md) summarizes the
available evidence. Priorities below are for CM5 development and qualification.

## High-priority work

1. Finish connected Ultra camera Punch-through. Verify entry, withdrawal, USB
   loss/recovery, phone Close/focus and stale frame retirement with both
   displays, audio and the control session live.
2. Persist confirmed cluster preferences and present useful native gauges
   before the phone connects. Exercise fallback, takeover and restored layout
   through a real disconnect/reconnect.
3. Qualify the top turn signals with dated phase inputs, source loss, layout
   transitions and both native and signed cluster owners.
4. Qualify higher-resolution and alternate aspect-ratio display profiles.
   Verify visible framing, matching touch transform and continued cluster
   operation before claiming a supported format.

## Remaining qualification

- Measure sustained playback, voice/calls and navigation with fitted audio
  devices. Exercise focus, mute/volume and output failure.
- Repeat real-device pairing, warm recovery, cold power-on, native Home/Back
  and compositor fault failback. Count usable presented video on each panel.
- Complete Climate, settings, vehicle-resource and camera controls with
  confirmed state from qualified providers.
- Integrate the MCU and physical sensor/actuator adapters under their
  [separate Linux-facing contract](MCU_VEHICLE_CONTRACT.md).
- Measure physical touch after core integration. Ordinary display hot-plug is
  deferred.

Thirty fresh frames per second is sufficient for feature work. Sustained
full-resolution 60 fps on both panels remains a qualification goal.
