<!--
Author: gautch29
Notice date: 2026-11-23
Provided as is, without warranty. See NOTICE.md.
-->

# Remaining full-resolution GPU work

Reviewed **1 October 2026**. This is an offline investigation, not a new CM5
performance result. The user accepts 30 fps for current feature work; the 60 fps
target remains open. This list is a performance backlog, not the current task
order. Touch-latency tuning remains deferred. The authoritative
acceptance scope is [GOAL.md](GOAL.md), and measured runs are in
[ULTRA_PERFORMANCE.md](ULTRA_PERFORMANCE.md).

## What still limits DigiBandTicks

The qualified synthetic two-panel fixture decodes approximately 60 distinct
1920 × 720 source frames/s per panel, but DigiBandTicks has reached only about
42.6 fresh source frames/s with normal completion. The recorded mean cluster
composition is 16.87 ms and the complete ordinary owner turn is 22.24 ms.
The latter also includes main rendering, script work and dispatch. Reducing
only cluster composition to 16.7 ms would therefore leave too little time for
the second panel and the rest of that serial owner turn.

The bounded deferred queue improves throughput while retaining real receipts,
but its CPU draw time excludes later GPU completion. The live DigiBandTicks
comparison reached 55.35 main / 46.49 cluster scenes/s; its Maps source supplied
14.20 fresh frames/s. These are different counters. Near-60 DigiRing scene
output and a phone source below 60 Hz do not qualify DigiBandTicks with 60
distinct source frames/s.

An existing single-panel regional-snapshot profile shows where to investigate:

| Historical CM5 animated DigiBandTicks profile | Time |
| --- | ---: |
| Complete warm composition median / p95 | 16.616 / 17.700 ms |
| Scene traversal median / p95 | 5.655 / 6.135 ms |
| Skia flush/submission median / p95 | 8.542 / 9.385 ms |
| Completion wait median / p95 | 2.306 / 3.502 ms |
| Validation, last draw only | 0.164 ms |

Each stage's median comes from its own distribution; adding these medians is
not a reconstructed frame time. Submission may include driver blocking and
must not be labeled pure CPU time. This historical profile predates the later
nested-blur restriction. Re-run the corrected implementation before using it
as a control. The profile had no native video or iPhone, so it cannot explain
all live costs.

## Portable scene audit

The local signature-checked reference pack was run through the portable Mac
probe with 60 synthetic gauge ticks and DigiBandTicks selected. Its final
cluster scene contains 245 total nodes, 231 active nodes, 16 active shapes,
14 masks, 49 clips and three enabled filters. Fourteen active source-over
shapes have a fully transparent fill; they contain 282 of the scene's 284
active path segments. Thirteen of those shapes have a visible stroke.

These counts describe the scene graph, not the number of GPU operations in a
warm draw: retained gradients and specialized masks can bypass shape content.
They do not measure rendering speed. Original audit logs and PNGs remain in
ignored private captures; only counts are recorded here. No original LIVI
implementation, active CM5 profile or service was used or changed.

Reproduce the audit from the repository root with the existing local accepted
asset/receipt and a new private output prefix:

```sh
HEADUNIT_THEME_PROBE_GAUGES=1 \
HEADUNIT_THEME_PROBE_ANIMATED=1 \
HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT=1B0484EC-1933-524D-80F8-2FC1FC7B4B2D \
HEADUNIT_THEME_PROBE_SCENE_AUDIT=1 \
build/headunit-theme-probe \
  captures/reference-asset/installed 44C9FA31EC 985 \
  captures/reference-asset/accepted-reference.conf \
  --layout-local captures/theme-reuse/local-profile.plist \
  captures/gpu-remaining-cost/digibandticks
```

## Small isolated candidate: omit empty source-over fills

`SkiaScene::Impl::content` currently converts every shape path to `SkPath`
and submits a fill before drawing its cached stroke. A fully transparent fill
with `SkBlendMode::kSrcOver` contributes no pixels. Place path conversion and
the fill draw inside a condition that preserves all fills with a nonzero alpha
or another blend mode. Keep the stroke path independent. This removes only
empty source-over paint work; transforms, masks, antialiasing, clipping and
stroke coverage remain intact.

Checking only `fill.a == 0` is insufficient. Transparent source/copy, clear
and unbounded blend operations can alter destination pixels. Check the actual
content paint mode, including the source-over mode used inside isolated
groups, and keep destructive group composition unchanged.

This is a concrete first experiment because its correctness boundary is
small. The scene audit establishes up to 14 eligible shape contents, not a
measured speedup or a prediction that it supplies the missing owner headroom.
No renderer change is included in this investigation: the local Mac build
cannot compile or exercise the pinned Linux Skia/EGL path, and a CM5 GPU oracle
and timing comparison are required before accepting it.

The next step is a same-binary probe-only old/new comparison of this condition
on the corrected renderer. Run the existing GPU scene/Wayland oracles with
extra explicit transparent source-over, copy, clear and source-in cases;
compare all six final 1920 × 720 style PNGs without relaxing pixel limits.
Then perform old/new/new/old animated DigiBandTicks runs with at least 60 warm
draws each. Accept it only if the completed median and p95 improve beyond run
variation and the other styles do not regress. Retain existing resolution,
blur quality, budgets and default policy.

## Larger candidates and their separate gates

### Native import reuse

Every `GpuCompositor::compose_scene` currently creates an EGL image and
external texture for each visible `NativeFrame`; `SkiaScene::draw` creates a
borrowed Skia image. Each submission retains its own objects until its fence
signals. Repeating an unchanged source repeats this import/preparation work.

A bounded import record shared by submissions using the same immutable source
is a candidate. It must preserve an independent producer lease for every
pending read, invalidate expired/replaced sources without raw-pointer aliasing,
and retire EGL/Skia resources only on their context owner after all relevant
fences. A resident cache must not hold extra decoder leases and starve the
four-lease decoder. Existing tests already cover repeated pixels, stream-close
lease release and deferred native input; a shared-import implementation needs
explicit two-submission sharing and replacement/teardown tests as well.

First measure repeated-source import/preparation time and count actual reuse.
Also run the two full-size 60 Hz moving sources. Import reuse can improve an
idle or 20–30 Hz phone source without improving a 60-fresh-source workload,
where each draw receives a different frame. Copying native video into a cached
RGB surface adds work on every fresh frame and needs a separate A/B result.
Neither candidate should be accepted from scene fps alone.

### Independent GPU owners

Independent main/cluster owners could overlap CPU traversal and driver work.
They cannot remove the GPU's pixel/filter workload, and their benefit is
unmeasured. The mutable script graph, its paths/images and video views must
remain on the VM owner. A handoff needs bounded immutable scene snapshots,
immutable sampled source identities and explicit receipt routing back to the
matching source token. Copying `shared_ptr<const Layer>` does not freeze its
mutable children or referenced image/path objects.

Measure snapshot build time, bytes and retained source leases before a live
split. At most the configured pending snapshots may remain queued. Rejection,
disconnect, script reset and clock suspension must retire the correct
snapshots without advancing PTS. Actual presentation receipts remain the
only acceptance clock. A faster isolated main owner is insufficient evidence
of faster cluster GPU completion.

## Completion gate

After a useful component improvement, re-run the corrected two-panel fixture
with two distinct 1920 × 720 / 60 Hz sources, no per-frame readback, real
presentation receipts and complete warm draw/gap distributions. Report
decoded, staged, fresh-presented and scene rates separately, plus lease/queue
bounds, late/clock-drop counters, thermal state and service restoration.
Then qualify sustained real-phone use at the source cadence it actually sends.
The goal remains open until both outputs sustain 60 distinct presented source
frames/s when their sources send 60/s; cold starts, recovery and visible
correctness remain separate acceptance checks.
