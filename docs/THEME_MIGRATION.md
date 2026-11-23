<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Theme migration record

This is the dated record of reusing our own earlier Ultra research, not a new
implementation plan. The fuller runtime, vehicle and GPU slices described below
are already in this independent repository. Use [architecture](ARCHITECTURE.md),
[provenance](PROVENANCE.md) and [current roadmap](ROADMAP.md) for present work.

## User clarification, 2026-09-26

All code authored during this project may be reused. Only the implementation
from the original cloned LIVI repository is excluded. The earlier blanket rule
against our own `native/carplay-ultra` work was too broad and is superseded.

Do not keep rebuilding the same CP APIs from scratch. Migrate our fuller native
runtime, vehicle bridge, asset installation and GPU compositor as needed, then
connect those to the independent phone receiver. Keep independently useful
new work (receiver/authentication, bounded message checks, tests) where it
improves the integration.

## First migrated slice

THEME_REUSE.json records source hashes for our scene model, Cairo reference
renderer, alpha masks, decoded-frame ownership, graphics bindings and tests.
They use `headunit::theme` and have no dependency on the cloned LIVI app/helper.
The original files are left intact. Changes from those files are reviewed in
the independent repository's commits.

The graphics binding now connects to `LuaHost` via an asset-reader callback.
Its unused AssetStore header dependency was removed. It supports the existing
scene model, including shapes, transforms, gradients, masks, images, video
views and backdrop filters. Reference rendering is for pixel checks; it does
not claim GPU presentation. `CP.PTS` advances only through an explicit host
presentation receipt, never by merely rendering a picture.

Porting the existing tests caught a Luau import-cache issue in the small host:
mutable userdata/global lookups must not be marked as constant imports. The
host now disables that optimization for the mutable script environment, as
our earlier full runtime already did.

## Original migration sequence

Source location: the project's existing `native/carplay-ultra` additions.
These paths describe our work, not upstream cloned LIVI implementation.

1. `script_runtime.cpp/.hpp`, `script_channel.cpp/.hpp` and their tests/probe:
   full CP lifecycle, 4 MiB modules, startup deadlines, monotonic uptime,
   vehicle bindings, persistence, layout selection and theme readiness.
   Prefer adapting its asset read boundary to the verified independent bundle,
   or port its already-written AssetStore with tests. Do not bypass verification.
2. `vehicle_data`, `vehicle_service`, `vehicle_script_view`,
   `local_vehicle_profile`, `sensor_input` and their original tests. Preserve
   unavailable/stale readings and explicit simulated-source configuration.
3. `asset_transfer`, `asset_signature`, `asset_store` and install/service tests:
   retain provenance, receipt policy and durable activation behavior. Reconcile
   equivalent new modules instead of maintaining two production paths.
4. `gpu_compositor`, `skia_scene`, `wayland_window`, media/presentation clocks,
   `video_decoder` and their GPU/Wayland tests. Reuse the pinned Skia build and
   its source/license receipt; do not copy Apple assets into the repository.

The earlier GPU path already supports DMA-buf video plus scene composition;
it still has explicitly unsupported effects and needs renewed timing/live
validation with our receiver. Its previous tests are evidence to reproduce,
not proof that the new integration works automatically.

The runtime, vehicle, GPU and native decoder slices below are now migrated.
NATIVE_VIDEO.md records the decoder-to-GPU checks, including the cropped HEVC
dependency fix. UI_TRANSPORT.md records the independent UI-channel adapter.
Live session ownership, clock synchronization and presentation scheduling remain
the next integration work; the roadmap is not a claim of live Ultra completion.

## First-slice validation

- 25/25 Mac tests and 33/33 Pi tests passed, including the migrated scene/video
  ownership tests. Address/undefined-behavior checks passed for graphics and
  the host after the mutable-global adaptation.
- The private verified reference package's connection/session probe still
  passes. Its unmodified Layer, ShapeLayer, Path, Rect and Transform modules
  also draw a translated rectangle through the reused graphics implementation;
  the Pi's output pixels match the expected foreground/background.
- The theme's animation helper needs bounded `loadstring` and a private writable
  `_G`. Those policies were reused from our earlier full runtime. The diagnostic
  caller declares its own globals before the theme locks new global creation.
- This slice has no running phone session and makes no new Ultra capability
  advertisement. It is a tested migration, not a completed Ultra display.

## Full runtime and vehicle slice

`headunit-theme-runtime` now reuses our original runtime, per-display script
channel, vehicle service/script view, local profiles, sensor input, and the
UI/OPACK codecs they use. Its private-package diagnostic is
`headunit-theme-probe`. The graphics bindings are shared by both hosts, with
one implementation. The smaller host remains useful for focused diagnostics;
the full runtime is the integration target.

Adaptations:

- Added a resource-reader constructor backed by the independent, immutable,
  signature-checked AssetBundle. Retained the guarded directory reader as
  explicitly trusted test-fixture input. Missing, oversized, absolute and
  traversal paths remain rejected; the 2.2 MiB generated module is supported.
- Added a shared, owner-thread resource cache for the same immutable verified
  bundle. It retains at most 32 MiB of resource bytes/path names and 4096 entries;
  excess reads still pass through the verified bundle. New bundles get new
  caches. This avoids reopening common resources for both display VMs.
- Preserved the CP ABI 1.4.0 target, foreground/readiness reporting, persistence,
  vehicle subscriptions, verification-region notifications, scheduler and
  layout/map selection policy. An ABI version is not a conformance claim.
- Preserved the theme-owned session/ACK sequence; the display channel adds only
  its displayUUID/data envelope, not a second sequencing layer.
- Adapted Luau destructor signatures and strict Mac/Linux compiler warnings.
  Reused our pinned numeric-index patch after the real package reproduced the
  same undefined conversion in Luau 0.739. Sanitizer tests now fail immediately
  on an error rather than merely printing a diagnostic.
- Kept local vehicle IDs explicitly unadvertised. These simulated readings do
  not supply the missing Apple Climate protocol identifiers or control a car.

Private full-package diagnostic (assets and local fixtures remain ignored):

```sh
HEADUNIT_THEME_PROBE_GAUGES=1 build/headunit-theme-probe \
  captures/reference-asset/installed 44C9FA31EC 985 \
  captures/reference-asset/accepted-reference.conf \
  --layout-local captures/theme-reuse/local-profile.plist \
  captures/theme-reuse/full
```

The previously accepted package is reopened using its exact local receipt;
this does not bypass checks for a fresh expired installation. No signed assets
are committed. To select another declared layout, set
`HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT` to its catalog ID (`--describe-layouts`).
Probe frame times, foreground and vehicle values are explicit simulations.
The live 20 ms tick budget remains unchanged; the offline probe has separate,
reported startup/frame limits. CPU PNG output does not claim GPU presentation.

### Layout coverage and remaining work

The Mac probe loaded 173 modules for Center_Display and 183 for Cluster_Display.
It rendered Cobranded, DigiRing, DigiBandTicks, DigiBand and MapsMedia with the
reused runtime and local simulated profile. The resulting images are offline
reference frames; missing remote video/content is expected in this probe.
Modular successfully initialized but the reference renderer explicitly rejected
its translucent backdrop inside a compositing group. Do not report all six
layouts working or silently remove that effect. Carry this case into GPU tests.

The live phone receiver has not yet been connected to these VMs. The GPU
compositor and its tests are now migrated, including a replacement/coverage
implementation for Modular's masked blur; see GPU_RENDERING.md. Next connect
the theme channel/lifecycle/video surfaces to the independent receiver. Keep the newer binary-plist preflight checks at the network boundary;
reconcile the diagnostic and migrated codecs before exposing the full host to
live untrusted messages. Apple Climate remains unresolved.

### Validation of the migrated runtime

- 33/33 Mac tests and 42/42 Pi tests pass, including the original vehicle,
  script, UI channel, protocol and Linux sensor tests. The added reader/cache
  tests cover shared immutable values, bounded retention, misses, traversal
  rejection and large generated modules.
- Address/undefined-behavior sanitizer checks pass for runtime/vehicle/OPACK
  and the complete signed-package layout probe with halt-on-error enabled.
- The Pi's full-package probe renders both display scenes and runs 60 simulated
  frames. With the shared cache, the observed startup took 6.9 s for center and
  5.0 s for cluster, inside the explicit 10 s diagnostic budget. This is not
  the final boot target. Cluster CPU reference rendering took about 2.13 s;
  the largest measured script tick was 21.4 ms, above the default live 20 ms
  budget. Optimize/integrate and remeasure before claiming smooth live output.
- The Pi also rendered DigiRing successfully with the same verified reader and
  local simulated profile. Its CPU reference frame took about 8.6 s, reinforcing
  that reference rendering is unsuitable for the live display path.
- No phone reconnect, original LIVI restart or live capability change was
  required for this migration. No new live Ultra/Climate result is claimed.

## GPU reuse slice

The pinned Skia/GLES compositor, Wayland window and media/presentation clocks
now build independently, with the original code mappings recorded above.
GPU_RENDERING.md records the masked-backdrop fix, tests, build instructions and
remaining live-integration limits. This supersedes the earlier note that the
GPU code was still waiting to be migrated. The CPU renderer's Modular limit
still applies; the GPU path implements that composition directly.

## Receiver UI boundary

The reused channel now has a typed envelope boundary, joined to the independent
receiver by `script::UiTransport`. It translates configured phone UUIDs to asset
display names, retains messages under transport backpressure, and invalidates
unsent messages on per-display reset. Binary-plist graph preflight now precedes
allocation in the full runtime's binary parser. The real signed-package session
probe uses this adapter for both displays. See UI_TRANSPORT.md for ownership,
validation scope and the remaining live receiver/session work.
