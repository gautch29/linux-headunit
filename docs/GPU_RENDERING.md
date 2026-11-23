<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Independent Ultra GPU renderer

The renderer reuses our own earlier GPU/Skia/Wayland code. Each original source
hash and destination is in THEME_REUSE.json. Original cloned LIVI app, transport
and helper code remains excluded. The compositor header now depends only on
scene/native-frame/presentation types, not a video-decoder implementation.

## Build on the Pi

Skia is an explicit, pinned third-party dependency, retained outside both
project directories. Its prior source/build receipt, flags and library checksum
are recorded in SKIA_BUILD.json. The `livi-cross` leaf is the historical Skia
build-output name, not an application dependency. CMake verifies the declared
revision and exact build flags; it never silently downloads or selects Skia.

```sh
cmake -S . -B build -DHEADUNIT_BUILD_THEME_GPU=ON \
  -DHEADUNIT_THEME_SKIA_SOURCE=/home/headunit/ultra-deps/skia-a8363ad14693 \
  -DHEADUNIT_THEME_SKIA_BUILD=/home/headunit/ultra-deps/skia-a8363ad14693/out/livi-cross
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The Linux dependency set includes GBM, EGL/GLES and Wayland. Portable builds
retain the script/scene and media/presentation-clock tests without requiring
GPU dependencies. GPU tests use `/dev/dri/renderD128`; when Weston is installed, optional Wayland tests launch and
clean up their own isolated headless Weston, not the user's desktop.

## Composition and timing

The renderer supports cached scene geometry/images, native DMA-buf texture
imports with producer leases retained until GPU completion, and separate
Wayland presentation feedback. Submitting a GPU frame does not advance the
phone/script clock. The owning session must consume actual presented feedback.
The media and presentation clock implementations and their original tests are
also migrated. Desktop preview has an explicit readback path; direct output
does not use that preview fallback.

Full scene traversal uses Skia Ganesh. Some path-coverage masks are still built
on the CPU and cached/uploaded, and PNG assets are decoded on the CPU. This is
not a claim that every preparation step runs on the GPU. Whole-scene CPU
rendering/readback is not part of the direct scene-composition path.

## Masked backdrops

A masked group previously isolated the blur from its ancestor pixels. The new
GPU path snapshots parent B, starts a working surface C from B, draws content
and filtered replacements there, and independently renders mask coverage M.
The final premultiplied result is `B + (C - B) * M * groupOpacity`, written with
replacement blending. It handles translucent B without compositing its alpha
twice. Nested affected groups use the same rule; ordinary groups keep their
existing path. Blur samples extend outside the panel; only output is clipped.

Ancestor clips apply once at final replacement. Applying those clips to C as
well would square antialiased edge coverage. Child clips retain their own
traversal order. Backdrop groups bypass tight ordinary-content bounds, which
can be empty for a group containing only a masked blur.

Individual filtered panels now read the immutable parent snapshot directly and
store only their transformed output rectangle. Input remains complete so blur
samples outside the panel survive. The filtered texture carries a pixel origin
back to the replacement shader. A fused, fully covering first backdrop can
snapshot only its backwards-computed filter halo and output region. Nested or
partially opaque backdrop groups keep their complete working surface: cropping
those intermediates changed Skia's sampling phase in stronger pixel comparisons.
Surfaces carry their actual pixel origins back to replacement; masks use decal
sampling outside their regional coverage. Ancestor AA still applies once.

A group beginning with a fully covering, unit-opacity rectangular backdrop can
start directly from the filtered parent. This guarded fusion eliminates the
seeded copy and intermediate replacement while keeping later paints in order.
A simple stroke mask can feed its cached coverage straight into the final
replacement shader; complex masks retain a separate surface. Ordinary isolated
source-over groups can also bound destructive descendants inside their own
transparent isolation. External destructive blending retains its full clip.

Path regions use tight curve extrema rather than off-curve Bezier control
points. The latter previously extended some short-arc regions about 90 pixels
past visible content. Conservative stroke padding and two-pixel AA guards
remain. Synthetic cropped/full GPU comparisons cover transparent backgrounds,
transforms, stroke-mask alpha/caps, nested groups and chained filters; see [current measurements](ULTRA_PERFORMANCE.md).

### Destination-out markers

Direct `destination-out` is supported inside a copied backdrop. It restores the
immutable parent rather than erasing that copied parent's alpha. With current
working pixels C, original parent B and source alpha A, the cut is:

```text
C_after_cut = C * (1 - A) + B * A
output = B + (C_after_cut - B) * mask * groupOpacity
```

All channels are premultiplied. Source coverage is rendered into a transparent
GPU surface, including its own children/mask and the appropriate opacity. An
ordinary isolated subgroup still uses ordinary destination-out against its own
transparent content. Explicitly flattened layers cut their own paints and then
draw their children using those children's blend modes. Each nested copied
group retains its immediate parent, not the frame's original background.

Coverage and the current-destination snapshot are bounded to the cut's source
region with AA guards. Their origins include any enclosing regional surface's
offset. Ancestor/own clips apply once at replacement; they are not baked into
the coverage and multiplied a second time. No GPU readback or whole-scene CPU
fallback is used for this operation.

Other destructive blends directly against a copied backdrop, and backdrop
filters inside a coverage mask, remain rejected before the framebuffer changes.
Ordinary transparent subgroups may isolate their destructive descendants. Frame validation
bounds traversal and estimated nested buffer work (128 MiB), plus cumulative
backdrop/cut work (512 MiB) across siblings. Cuts budget coverage, destination
snapshot and copy-on-write storage before drawing. These are conservative work/allocation
estimates, not a hard guarantee about driver memory; Skia's reusable resource
cache is separately limited to 128 MiB.

Pixel tests cover opaque comparison against the Cairo reference, an independent
premultiplied mixing oracle for translucent backgrounds, zero/nested opacity,
mask coverage, rotated rounded clips, blur halo sampling, and unchanged output
on rejected blends/excessive work. The CPU reference retains its explicit
unsupported-case rejection for translucent grouped backdrops; the GPU path
supplies this feature without hiding it or falling back to CPU rendering.
On 1 October, the CM5 GPU-scene suite passed 95 independent CPU/GPU comparison
cases plus translucent replacement, nested cut, regional snapshot and unchanged
output-on-rejection oracles. The richer reference-pack fixture renders all six
styles in day/night mode at 1920 × 720. This is offline composition evidence;
the richer profile still needs live phone/background qualification.
The final richer DigiRing image differs from the full-surface GPU control by
at most one 8-bit colour level, with identical alpha. The six regular and nine
Single layouts pass both appearances (30 combinations); successful offline
traversal is separate from complete live instrument qualification.

## Private layout probe

Use the same previously verified external package and local simulated vehicle
profile as THEME_MIGRATION.md. `HEADUNIT_THEME_PROBE_GPU_ONLY=/dev/dri/renderD128`
selects direct scene rendering for this diagnostic, including Modular.
`HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT` selects a catalog ID.
`HEADUNIT_THEME_PROBE_REALTIME=1` gives script uptime animations real elapsed
frame intervals during the initial 60 simulated ticks; it does not claim real
phone PTS or actual presentation. Sixty-three repeated GPU draws report cold
and warm wall times, with an explicit two-second offline completion deadline.
Final PNG readback is for inspection only.

At the time of this initial port, the compositor was not yet connected to the
independent receiver's live video/theme channels. That integration and native
video handoff now exist; sustained two-display performance remains unverified.
Current Climate/control qualification is tracked in the README and vehicle
configuration documentation, separately from these rendering measurements.

## Verification and measured limits (2026-09-26)

This section is the historical first GPU port baseline. The renderer is now
connected to the independent receiver; current CM5 live and animated-layout
measurements are in [ULTRA_PERFORMANCE.md](ULTRA_PERFORMANCE.md).

The Pi passed all 50 tests, including offscreen and isolated Wayland GPU
tests; the Mac passed all 35 portable tests. Scene comparison covers 86
CPU/GPU cases plus the independent backdrop assertions above. These runs
do not include a new GPU sanitizer build or a live phone integration test.

All six catalog layouts completed the private GPU-only probe on V3D 4.2.14.0.
The table measures whole `compose_scene` wall time for the cluster display:
19 repeated draws of the same scene, with the first three excluded from the
16 warm samples. Script startup uses simulated vehicle data and real elapsed
tick intervals. This is a static diagnostic, not sustained animated playback.

| Layout | Warm median (ms) | Warm maximum (ms) | Cold (ms) |
| --- | ---: | ---: | ---: |
| Cobranded | 62.517 | 62.616 | 369.273 |
| DigiRing | 59.089 | 59.221 | 234.882 |
| Modular | 149.198 | 161.178 | 287.927 |
| DigiBandTicks | 121.774 | 122.023 | 419.475 |
| DigiBand | 125.935 | 126.216 | 399.592 |
| MapsMedia | 58.435 | 58.535 | 154.858 |

None reaches the 16.7 ms budget for 60 fps. The slower styles also exceed the
live compositor's unchanged 100 ms completion deadline; the probe's two-second
deadline must not be mistaken for live readiness. Full-frame backdrop scratch
surfaces and scene preparation remain optimization candidates.

Successful traversal/rendering does not establish final visual correctness.
Inspection of Modular shows overlapping km/h and mph labels, and incomplete
remote/vehicle placeholders remain visible in Modular and DigiBand. Their
runtime, profile and layout inputs need investigation; the signed assets were
not modified. Private evidence is in `captures/gpu-migration/summary.json`
and the accompanying PNG/JSONL files, intentionally excluded from version
control. No sustained dual-display or live decoder handoff claim is made.

The native decoder has now been migrated and tested with synthetic encrypted
video through this compositor. NATIVE_VIDEO.md describes the hardware/GPU tests
and the fixed Pi decoder package needed for cropped HEVC output. This adds native
input coverage; it does not change the full-layout timings above.
