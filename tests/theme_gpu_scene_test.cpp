// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_compositor.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <headunit/theme/warning_overlay.hpp>
#include "../src/theme/skia_scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <thread>
namespace g = headunit::theme::graphics;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
std::shared_ptr<g::Layer> layer(g::Rect r, g::Color c = {}, g::Kind kind = g::Kind::layer) {
  auto v = std::make_shared<g::Layer>(kind);
  v->set_frame(r);
  v->background = c;
  return v;
}
int main(int argc, char** argv) {
  try {
    check(argc == 2, "usage: gpu-scene-test RENDER_NODE");
    constexpr unsigned w = 257, h = 193;
    const bool wayland = std::getenv("HEADUNIT_THEME_GPU_PROBE_WAYLAND") != nullptr;
    std::unique_ptr<g::GpuCompositor> gpu =
        wayland ? std::make_unique<g::GpuCompositor>(
                      w, h, g::WaylandOptions{"Headunit full scene fixture"})
                : std::make_unique<g::GpuCompositor>(w, h, argv[1]);
    // A phone image has actual output pixels while the asset places it across
    // a wider authored canvas. Mapping must preserve every pixel, including the
    // right edge, without editing the script-owned scene or reading back live video.
    auto reference_root = layer({{0, 0}, {w * 2, h}});
    auto reference_picture = layer({{0, 0}, {w * 2, h}}, {}, g::Kind::image);
    reference_picture->image =
        std::make_shared<g::Image>(g::Image{w, h, std::vector<std::uint32_t>(w * h, 0xff000000)});
    for (unsigned y = 0; y < h; ++y)
      for (unsigned x = 0; x < w; ++x)
        reference_picture->image->pixels[y * w + x] =
            x > w - 10 ? 0xffff0000
                       : (x >= 40 && x < 100 && y >= 40 && y < 100 ? 0xff00ff00 : 0xff000000);
    reference_root->insert(reference_picture, 0);
    gpu->compose_scene(g::map_scene_canvas(reference_root, {w * 2, h}, {w, h}));
    const auto mapped_pixels = gpu->readback().pixels;
    unsigned mapped_error = 0;
    std::size_t mapped_changed = 0, mapped_worst = 0;
    for (std::size_t i = 0; i < mapped_pixels.size(); ++i) {
      if (mapped_pixels[i] != reference_picture->image->pixels[i]) ++mapped_changed;
      for (unsigned c = 0; c < 4; ++c) {
        const auto difference =
            unsigned(std::abs(int((mapped_pixels[i] >> (c * 8)) & 255) -
                              int((reference_picture->image->pixels[i] >> (c * 8)) & 255)));
        if (difference > mapped_error) {
          mapped_error = difference;
          mapped_worst = i;
        }
      }
    }
    std::cout << "mappedCanvasMaxError=" << mapped_error << " changedPixels=" << mapped_changed
              << " worst=" << mapped_worst % w << ',' << mapped_worst / w << " actual=" << std::hex
              << mapped_pixels[mapped_worst]
              << " expected=" << reference_picture->image->pixels[mapped_worst] << std::dec << '\n';
    // Linear GPU sampling differs by at most a few 8-bit levels at hard edges.
    check(mapped_error <= 3, "asset canvas mapping cropped or distorted the phone image");
    check(reference_root->transform.a == 1 && reference_picture->parent.lock() == reference_root,
          "mapping mutated script scene");
    // CPU script work may update the next scene while the current GPU commands
    // finish. Its mutable image must remain an exact snapshot for this frame.
    auto overlap_root = layer({{0, 0}, {w, h}});
    auto overlap_image = layer({{0, 0}, {w, h}}, {}, g::Kind::image);
    overlap_image->image =
        std::make_shared<g::Image>(g::Image{w, h, std::vector<std::uint32_t>(w * h, 0xffff0000)});
    overlap_root->insert(overlap_image, 0);
    gpu->compose_scene(overlap_root);
    const auto before_overlap = gpu->readback().pixels;
    unsigned overlap_calls = 0;
    const auto overlapped = gpu->compose_scene(overlap_root, 100000000, [&] {
      ++overlap_calls;
      std::fill(overlap_image->image->pixels.begin(), overlap_image->image->pixels.end(),
                0xff0000ff);
    });
    check(
        overlap_calls == 1 && overlapped.overlap_ns > 0 && gpu->readback().pixels == before_overlap,
        "GPU completion work changed the frame already submitted");
    gpu->compose_scene(overlap_root);
    check(gpu->readback().pixels != before_overlap, "next GPU scene did not observe image update");
    {
      g::GpuCompositor bounded(w, h, argv[1]);
      bool rejected = false;
      try {
        bounded.compose_scene(overlap_root, 1000000,
                              [&] { std::this_thread::sleep_for(std::chrono::milliseconds(3)); });
      } catch (const std::runtime_error&) {
        rejected = true;
      }
      check(rejected, "CPU overlap exceeded the GPU completion deadline without faulting");
    }
    unsigned cases = 0, failures = 0;
    auto compare = [&](const std::shared_ptr<g::Layer>& scene, unsigned max_error = 3,
                       double max_mean = 1.0) {
      const auto cpu = g::render(scene, w, h);
      const auto timing = gpu->compose_scene(scene);
      const auto actual = gpu->readback();
      check(timing.validation_ns > 0 && timing.completion_wait_ns > 0 &&
                timing.draw_ns >= timing.completion_wait_ns,
            "scene timing phases missing or inconsistent");
      unsigned error = 0;
      std::size_t worst = 0;
      std::uint64_t sum = 0;
      for (std::size_t i = 0; i < cpu.pixels.size(); ++i)
        for (unsigned c = 0; c < 4; ++c) {
          const auto d = unsigned(std::abs(int((cpu.pixels[i] >> (8 * c)) & 255) -
                                           int((actual.pixels[i] >> (8 * c)) & 255)));
          if (d > error) {
            error = d;
            worst = i;
          }
          sum += d;
        }
      const auto mean = double(sum) / (cpu.pixels.size() * 4);
      std::cout << "case=" << ++cases << " maxError=" << error << " meanError=" << mean
                << " drawNs=" << timing.draw_ns << '\n';
      if (error > max_error || mean > max_mean)
        std::cout << "worst=" << worst % w << ',' << worst / w << " cpu=" << std::hex
                  << cpu.pixels[worst] << " gpu=" << actual.pixels[worst] << std::dec << '\n';
      const auto skia_cpu = g::SkiaScene::raster_probe(scene, w, h);
      unsigned raster_error = 0;
      for (std::size_t i = 0; i < actual.pixels.size(); ++i)
        for (unsigned c = 0; c < 4; ++c)
          raster_error = std::max(raster_error,
                                  unsigned(std::abs(int((actual.pixels[i] >> (8 * c)) & 255) -
                                                    int((skia_cpu.pixels[i] >> (8 * c)) & 255))));
      std::cout << "gpuVsSkiaRasterMaxError=" << raster_error << '\n';
      if (const auto* output = std::getenv("HEADUNIT_THEME_GPU_SCENE_OUTPUT")) {
        g::write_png(skia_cpu,
                     std::string(output) + "-" + std::to_string(cases) + "-skia-raster.png");
        g::write_png(cpu, std::string(output) + "-" + std::to_string(cases) + "-cpu.png");
        g::write_png(actual, std::string(output) + "-" + std::to_string(cases) + "-gpu.png");
      }
      if (error > max_error || mean > max_mean) {
        ++failures;
        std::cout << "pixelComparisonFailed=true case=" << cases << '\n';
      }
      if (wayland) {
        g::Image window;
        const auto receipt = gpu->present(cases, &window);
        check(receipt.presented && receipt.token == cases && window.pixels == actual.pixels,
              "Skia Wayland blit/receipt failed");
      }
      return actual;
    };
    {
      auto corners = layer({{0, 0}, {w, h}});
      auto popup = layer({{25, 20}, {140, 120}}, {1, 0, 0, 1});
      popup->corner_radius = 30;
      popup->continuous_corners = true;
      popup->rasterize = true;
      popup->clip = false;
      corners->insert(popup, 0);
      const auto smooth = compare(corners, 32, .1);
      popup->continuous_corners = false;
      check(compare(corners, 32, .1).pixels != smooth.pixels,
            "corner mutation reused stale retained coverage");
      popup->continuous_corners = true;
      popup->background = {};
      popup->kind = g::Kind::gradient;
      popup->stops = {{{0, 0, 1, 1}, 0}, {{0, 1, 0, 1}, 1}};
      popup->border_width = 4;
      popup->border = {1, 1, 1, 1};
      popup->transform = {.95, .1, -.1, .95, 0, 0};
      // Rotated antialiased border coverage differs at individual edge pixels
      // between Cairo and GPU tessellation; bound aggregate error separately.
      compare(corners, 96, .2);
      popup->stops.clear();
      popup->kind = g::Kind::layer;
      popup->border_width = 0;
      popup->clip = true;
      popup->insert(layer({{-20, -20}, {180, 160}}, {0, 1, 0, 1}), 0);
      const auto clipped = compare(corners, 64, .1);
      check(clipped.pixels[5 * w + 5] == 0 && clipped.pixels[75 * w + 75] == 0xff00ff00,
            "continuous clip leaked children or removed the interior");
      popup->corner_radius = 1000;
      compare(corners, 64, .3);
      popup->corner_radius = 0;
      compare(corners, 64, .3);
    }

    // Every native telltale is above the entire mapped asset, with immutable
    // cached tiles. Include clearing, stale input and a masked/translucent scene.
    {
      g::WarningOverlayConfiguration config;
      config.canvas = {512, 384};
      config.simulation_badge = {{8, 8}, {96, 24}};
      for (std::size_t i = 0; i < headunit::indicator_definitions.size(); ++i)
        config.indicators.push_back(
            {headunit::indicator_definitions[i].id,
             {{8 + 80 * double(i % 6), 72 + 64 * double(i / 6)}, {72, 56}}});
      g::WarningOverlay warnings(config);
      headunit::VehicleSample sample;
      sample.sampled_at = headunit::Clock::time_point{std::chrono::seconds(2)};
      sample.simulated = true;
      sample.indicators.fill(headunit::IndicatorState::on);
      auto asset = layer({{0, 0}, {w, h}}, {.15, .25, .5, .7});
      asset->clip = true;
      asset->set_mask(layer({{0, 0}, {80, 90}}, {1, 1, 1, 1}));
      asset->opacity = .4;
      warnings.scene(sample, sample.sampled_at, {w, h});
      const auto overlay =
          warnings.scene(sample, sample.sampled_at + std::chrono::milliseconds(40), {w, h});
      auto combined = g::with_scene_overlay(asset, overlay);
      // Fourfold minification of small antialiased text uses different Cairo
      // and GPU sampling footprints. Bound both peak and whole-image error.
      compare(std::const_pointer_cast<g::Layer>(combined), 8, .15);
      const auto image_builds = gpu->scene_cache_stats().image_builds;
      compare(std::const_pointer_cast<g::Layer>(combined), 8, .15);
      check(gpu->scene_cache_stats().image_builds == image_builds,
            "warning symbols uploaded again for unchanged state");
      const auto hidden = warnings.scene(sample, sample.sampled_at, {w, h}, true);
      check(!hidden, "startup GPU overlay was not hidden");
      const auto hidden_image =
          compare(std::const_pointer_cast<g::Layer>(g::with_scene_overlay(asset, hidden)));
      check(hidden_image.pixels == compare(asset).pixels,
            "startup left lamp or simulation badge pixels over the scene");
      warnings.scene(sample, sample.sampled_at, {w, h}, false);
      const auto driving =
          warnings.scene(sample, sample.sampled_at + std::chrono::milliseconds(40), {w, h}, false);
      compare(std::const_pointer_cast<g::Layer>(g::with_scene_overlay(asset, driving)), 8, .15);
      check(gpu->scene_cache_stats().image_builds == image_builds,
            "Drive restoration uploaded the symbol tiles again");
      sample.indicators.fill(headunit::IndicatorState::off);
      sample.simulated = false;
      warnings.scene(sample, sample.sampled_at + std::chrono::milliseconds(50), {w, h});
      const auto cleared =
          warnings.scene(sample, sample.sampled_at + std::chrono::milliseconds(170), {w, h});
      check(!cleared, "GPU warning did not clear");
      const auto clear_image =
          compare(std::const_pointer_cast<g::Layer>(g::with_scene_overlay(asset, cleared)));
      sample.indicators.fill(headunit::IndicatorState::on);
      const auto stale =
          warnings.scene(sample, sample.sampled_at + std::chrono::seconds(1), {w, h});
      check(stale && stale->children.size() == 2, "stale GPU warning lost no-data outlines");
      compare(std::const_pointer_cast<g::Layer>(g::with_scene_overlay(asset, stale)), 8, .15);
      gpu->compose_scene(g::with_scene_overlay(asset, {}));
      check(gpu->readback().pixels == clear_image.pixels, "cleared overlay left stale GPU pixels");
      check(asset->children.empty() && overlay->parent.expired(),
            "GPU overlay changed asset ownership");
    }
    auto root = layer({{0, 0}, {w, h}}, {.1, .2, .3, .4});
    compare(root);
    // The signed cluster has solid gauge bands clipped by a single stroke mask.
    // Compare the fast GPU clip against the independent CPU group/mask renderer,
    // including translucent descendants and a moved mask.
    auto gauge_group = layer({{18, 20}, {115, 115}});
    gauge_group->group_blending = true;
    auto gauge_band = layer({{0, 0}, {115, 115}}, {.2, .7, .9, .8});
    gauge_band->group_blending = true;
    gauge_band->corner_radius = 8;
    gauge_group->insert(gauge_band, 0);
    auto gauge_mask = layer({{0, 0}, {115, 115}}, {}, g::Kind::shape);
    gauge_mask->path = std::make_shared<g::Path>();
    gauge_mask->path->append({g::SegmentType::ellipse, {8, 8}, {99, 99}});
    gauge_mask->stroke = {1, 1, 1, .7};
    gauge_mask->line_width = 9;
    gauge_mask->stroke_start = .15;
    gauge_mask->stroke_end = .9;
    gauge_group->set_mask(gauge_mask);
    root->insert(gauge_group, 0);
    const auto simple_mask_before = gpu->scene_cache_stats().simple_mask_groups;
    compare(root, 4, 1.0);
    check(gpu->scene_cache_stats().simple_mask_groups > simple_mask_before,
          "solid stroke-mask fast path was not exercised");
    gauge_group->opacity = .6;
    gauge_band->opacity = .75;
    gauge_mask->opacity = .8;
    gauge_mask->transform = {.95, .08, -.08, .95, 3, 2};
    compare(root, 4, 1.0);
    const auto fill_mask_before = gpu->scene_cache_stats().simple_fill_mask_groups;
    gauge_mask->opacity = 1;
    gauge_mask->stroke = {};
    gauge_mask->fill = {1, 1, 1, 1};
    compare(root, 4, 1.0);
    check(gpu->scene_cache_stats().simple_fill_mask_groups > fill_mask_before,
          "solid fill-mask fast path was not exercised");
    gauge_mask->fill.a = .5;
    compare(root, 6,
            1.0);  //Partial mask alpha uses the general group path; its AA edge differs by 5/255.
    check(gpu->scene_cache_stats().simple_fill_mask_groups == fill_mask_before + 1,
          "partial fill mask entered the opaque fast path");
    root->remove(gauge_group);
    // A signed DigiBar layout uses a rectangular, normalized-edge backdrop
    // blur over an opaque cluster background. Compare its actual pixels with
    // the reference renderer; an unblurred transparent layer must fail here.
    auto backdrop_scene = layer({{0, 0}, {w, h}}, {0, 0, 0, 1});
    backdrop_scene->insert(layer({{0, 0}, {130, h}}, {1, 0, 0, 1}), 0);
    backdrop_scene->insert(layer({{130, 0}, {w - 130, h}}, {0, 0, 1, 1}), 1);
    auto blur_panel = layer({{88, 28}, {90, 130}}, {}, g::Kind::backdrop);
    auto blur = std::make_shared<g::BlurFilter>();
    blur->radius = 7;
    blur->normalized_edges = true;
    blur_panel->add_filter(blur);
    backdrop_scene->insert(blur_panel, 2);
    const auto blurred = compare(backdrop_scene, 24, 1.0);
    const auto mixed_pixel = blurred.pixels[90 * w + 126];
    check(((mixed_pixel >> 16) & 255) < 250 && (mixed_pixel & 255) > 5,
          "backdrop blur scene did not mix adjacent colors");
    // A backdrop-only mask group must sample the parent, not an empty isolation
    // layer. Compare with our independent CPU reference for an opaque backdrop.
    auto masked_blur = layer({{0, 0}, {w, h}});
    masked_blur->insert(blur_panel, 0);
    backdrop_scene->clear();
    backdrop_scene->insert(layer({{0, 0}, {130, h}}, {1, 0, 0, 1}), 0);
    backdrop_scene->insert(layer({{130, 0}, {w - 130, h}}, {0, 0, 1, 1}), 1);
    masked_blur->set_mask(layer({{90, 30}, {84, 124}}, {1, 1, 1, .5}));
    masked_blur->opacity = .5;
    backdrop_scene->insert(masked_blur, 2);
    compare(backdrop_scene, 24, 1.0);
    // Independent replacement oracle in premultiplied channels. Unlike raster
    // comparison, this detects double-alpha restoration on transparent targets.
    const auto render_gpu = [&](const std::shared_ptr<g::Layer>& scene) {
      gpu->compose_scene(scene);
      return gpu->readback();
    };
    auto translucent = layer({{0, 0}, {w, h}});
    translucent->insert(layer({{0, 0}, {130, h}}, {1, 0, 0, .5}), 0);
    translucent->insert(layer({{130, 0}, {w - 130, h}}, {0, 0, 1, .25}), 1);
    const auto before_blur = render_gpu(translucent);
    auto panel = layer({{88, 28}, {90, 130}}, {}, g::Kind::backdrop);
    panel->add_filter(blur);
    translucent->insert(panel, 2);
    const auto fully_filtered = render_gpu(translucent);
    translucent->remove(panel);
    const auto expect_mix = [&](const g::Image& actual, const g::Image& before,
                                const g::Image& after, double q,
                                const g::Image* coverage = nullptr) {
      unsigned maximum = 0;
      for (std::size_t i = 0; i < actual.pixels.size(); ++i)
        for (unsigned c = 0; c < 4; ++c) {
          const auto weight = q * (coverage ? double(coverage->pixels[i] >> 24) / 255 : 1);
          const auto a = int((before.pixels[i] >> (8 * c)) & 255),
                     b = int((after.pixels[i] >> (8 * c)) & 255);
          maximum = std::max(maximum, unsigned(std::abs(int((actual.pixels[i] >> (8 * c)) & 255) -
                                                        int(std::lround(a + (b - a) * weight)))));
        }
      std::cout << "backdropReplacementMaxError=" << maximum << '\n';
      check(maximum <= 2, "masked backdrop violated premultiplied replacement equation");
    };
    auto coverage_layer = layer({{90, 30}, {84, 124}}, {1, 1, 1, .5});
    const auto coverage = render_gpu(coverage_layer);
    auto outer = layer({{0, 0}, {w, h}});
    outer->opacity = .5;
    outer->set_mask(coverage_layer);
    outer->insert(panel, 0);
    translucent->insert(outer, 2);
    expect_mix(render_gpu(translucent), before_blur, fully_filtered, .5, &coverage);
    outer->opacity = 0;
    expect_mix(render_gpu(translucent), before_blur, fully_filtered, 0);
    outer->opacity = .5;
    // Full coverage permits a simple nested-group oracle: .5*.5*.5*.5=1/16.
    outer->set_mask(layer({{0, 0}, {w, h}}, {1, 1, 1, .5}));
    outer->remove(panel);
    auto inner = layer({{0, 0}, {w, h}});
    inner->opacity = .5;
    inner->set_mask(layer({{0, 0}, {w, h}}, {1, 1, 1, .5}));
    inner->insert(panel, 0);
    outer->insert(inner, 0);
    expect_mix(render_gpu(translucent), before_blur, fully_filtered, 1.0 / 16);
    auto uniform = layer({{0, 0}, {w, h}}, {1, 0, 0, .5});
    const auto uniform_before = render_gpu(uniform);
    translucent->remove(outer);
    uniform->insert(outer, 0);
    expect_mix(render_gpu(uniform), uniform_before, uniform_before, 1);
    // Ancestor clip transforms survive working-surface changes. No changed pixel
    // may escape a rounded, rotated clip, including a shifted child mask.
    auto clipped = layer({{40, 15}, {155, 150}});
    clipped->clip = true;
    clipped->corner_radius = 23;
    clipped->transform = {.98, .14, -.14, .98, 0, 0};
    uniform->remove(outer);
    clipped->insert(outer, 0);
    translucent->insert(clipped, 2);
    const auto clipped_result = render_gpu(translucent);
    auto clip_coverage = layer({{40, 15}, {155, 150}});
    clip_coverage->clip = true;
    clip_coverage->insert(layer({{-100, -100}, {w + 200, h + 200}}, {1, 1, 1, 1}), 0);
    clip_coverage->corner_radius = 23;
    clip_coverage->transform = clipped->transform;
    const auto clipping = render_gpu(clip_coverage);
    for (std::size_t i = 0; i < clipped_result.pixels.size(); ++i)
      if ((clipping.pixels[i] >> 24) == 0)
        check(clipped_result.pixels[i] == before_blur.pixels[i], "backdrop escaped ancestor clip");
    translucent->remove(clipped);
    uniform->insert(clipped, 0);
    expect_mix(render_gpu(uniform), uniform_before, uniform_before, 1);
    uniform->remove(clipped);
    outer->clear();
    outer->set_mask(layer({{-100, -100}, {w + 200, h + 200}}, {1, 1, 1, 1}));
    outer->opacity = 1;
    outer->insert(panel, 0);
    outer->insert(layer({{-100, -100}, {w + 200, h + 200}}, {0, 1, 0, 1}), 1);
    translucent->insert(clipped, 2);
    const auto aa_result = render_gpu(translucent);
    g::Image solid_green{w, h, std::vector<std::uint32_t>(w * h, 0xff00ff00)};
    expect_mix(aa_result, before_blur, solid_green, 1, &clipping);
    // A destructive sibling may not erase the copied destination under a
    // seeded group; reject before touching its existing framebuffer.
    auto destructive = layer({{0, 0}, {10, 10}}, {1, 0, 0, 1});
    destructive->blend = g::Blend::clear;
    outer->insert(destructive, 2);
    bool blend_rejected = false;
    try {
      gpu->compose_scene(translucent);
    } catch (const std::exception&) {
      blend_rejected = true;
    }
    check(blend_rejected && gpu->readback().pixels == aa_result.pixels,
          "unsupported backdrop blend changed output");
    outer->remove(destructive);
    auto excessive = layer({{0, 0}, {w, h}});
    for (unsigned i = 0; i < 1000; ++i) {
      auto effect = layer({{0, 0}, {1, 1}}, {}, g::Kind::backdrop);
      auto filter = std::make_shared<g::BlurFilter>();
      filter->radius = 7;
      effect->add_filter(filter);
      excessive->insert(effect, i);
    }
    bool work_rejected = false;
    try {
      gpu->compose_scene(excessive);
    } catch (const std::exception&) {
      work_rejected = true;
    }
    check(work_rejected && gpu->readback().pixels == aa_result.pixels,
          "backdrop sibling work budget was not enforced before drawing");
    auto halo_scene = layer({{0, 0}, {w, h}}, {0, 0, 0, 1});
    halo_scene->insert(layer({{80, 0}, {8, h}}, {1, 1, 1, 1}), 0);
    auto halo_group = layer({{0, 0}, {w, h}});
    halo_group->set_mask(layer({{88, 28}, {90, 130}}, {1, 1, 1, 1}));
    auto halo_panel = layer({{88, 28}, {90, 130}}, {}, g::Kind::backdrop);
    halo_panel->add_filter(blur);
    halo_group->insert(halo_panel, 0);
    halo_scene->insert(halo_group, 1);
    const auto halo = render_gpu(halo_scene);
    check((halo.pixels[90 * w + 90] & 255) > 5, "backdrop discarded blur samples outside panel");
    check(halo.pixels[90 * w + 85] == 0xffffffff && halo.pixels[90 * w + 79] == 0xff000000,
          "backdrop changed pixels outside panel");
    // Crop only filtered output storage. A full-surface GPU oracle must match
    // through transformed/edge panels, chained filters and transparent input.
    setenv("HEADUNIT_THEME_PROBE_FULL_BACKDROP_SURFACE", "1", 1);
    g::GpuCompositor full_filter(w, h, argv[1]);
    unsetenv("HEADUNIT_THEME_PROBE_FULL_BACKDROP_SURFACE");
    auto crop_scene = layer({{0, 0}, {w, h}});
    for (unsigned y = 0; y < h; y += 17)
      for (unsigned x = 0; x < w; x += 19)
        crop_scene->insert(layer({{double(x), double(y)}, {19, 17}},
                                 {double(x % 3) / 2, double(y % 5) / 4, double((x + y) % 7) / 6,
                                  .35 + double((x + y) % 3) * .25}),
                           crop_scene->children.size());
    auto crop_panel = layer({{-12, 11}, {97, 86}}, {}, g::Kind::backdrop);
    crop_panel->opacity = .7;
    auto first_filter = std::make_shared<g::BlurFilter>(),
         second_filter = std::make_shared<g::BlurFilter>();
    first_filter->radius = 7;
    second_filter->radius = 2;
    crop_panel->add_filter(first_filter);
    crop_panel->add_filter(second_filter);
    crop_scene->insert(crop_panel, crop_scene->children.size());
    for (const bool normalized : {false, true})
      for (const double radius : {.5, 7., 25.})
        for (const g::Transform transform : {g::Transform{}, g::Transform{1.3, .2, -.3, .7, 42, 20},
                                             g::Transform{.7, 0, 0, 1.4, 160, 80}}) {
          first_filter->normalized_edges = normalized;
          first_filter->radius = radius;
          crop_panel->transform = transform;
          const auto cropped_result = render_gpu(crop_scene);
          full_filter.compose_scene(crop_scene);
          const auto full_result = full_filter.readback();
          unsigned maximum = 0;
          for (std::size_t i = 0; i < full_result.pixels.size(); ++i)
            for (unsigned c = 0; c < 4; ++c)
              maximum = std::max(
                  maximum, unsigned(std::abs(int((cropped_result.pixels[i] >> (8 * c)) & 255) -
                                             int((full_result.pixels[i] >> (8 * c)) & 255))));
          std::cout << "croppedFilterMaxError=" << maximum << '\n';
          check(maximum <= 2, "cropped backdrop lost source halo or changed filter coordinates");
        }
    // Small masked groups exercise regional input copies, rather than only a
    // panel's output allocation. Use the full GPU path as an independent oracle
    // for nested and sequential halos, global edges, AA clips and transparency.
    setenv("HEADUNIT_THEME_PROBE_FULL_BACKDROP_GROUP", "1", 1);
    setenv("HEADUNIT_THEME_PROBE_DISABLE_TIGHT_PATH_BOUNDS", "1", 1);
    g::GpuCompositor full_groups(w, h, argv[1]);
    unsetenv("HEADUNIT_THEME_PROBE_DISABLE_TIGHT_PATH_BOUNDS");
    unsetenv("HEADUNIT_THEME_PROBE_FULL_BACKDROP_GROUP");
    crop_scene->remove(crop_panel);
    auto regional_group = layer({{0, 0}, {w, h}});
    regional_group->opacity = .65;
    regional_group->set_mask(layer({{56, 58}, {42, 35}}, {1, 1, 1, .7}));
    regional_group->mask->corner_radius = 9;
    crop_panel->transform = {};
    crop_panel->set_frame({{16, 18}, {165, 144}});
    regional_group->insert(crop_panel, 0);
    auto regional_inner = layer({{12, 9}, {156, 141}});
    regional_inner->opacity = .8;
    regional_inner->set_mask(layer({{36, 32}, {56, 53}}, {1, 1, 1, .6}));
    auto inner_panel = layer({{-6, -8}, {150, 130}}, {}, g::Kind::backdrop);
    auto inner_filter = std::make_shared<g::BlurFilter>();
    inner_filter->radius = 3;
    inner_panel->add_filter(inner_filter);
    regional_inner->insert(inner_panel, 0);
    regional_inner->insert(layer({{37, 31}, {17, 19}}, {.8, .3, .2, .5}), 1);
    regional_group->insert(regional_inner, 1);
    crop_scene->insert(regional_group, crop_scene->children.size());
    for (const bool normalized : {false, true})
      for (const double radius : {.5, 3., 9.})
        for (const g::Transform transform :
             {g::Transform{}, g::Transform{.85, .14, -.16, 1.05, 27, 18},
              g::Transform{1, 0, 0, 1, -73, -71}}) {
          first_filter->normalized_edges = normalized;
          inner_filter->normalized_edges = normalized;
          first_filter->radius = radius;
          regional_group->transform = transform;
          const auto regional_result = render_gpu(crop_scene);
          full_groups.compose_scene(crop_scene);
          const auto full_result = full_groups.readback();
          unsigned maximum = 0;
          for (std::size_t i = 0; i < full_result.pixels.size(); ++i)
            for (unsigned c = 0; c < 4; ++c)
              maximum = std::max(
                  maximum, unsigned(std::abs(int((regional_result.pixels[i] >> (8 * c)) & 255) -
                                             int((full_result.pixels[i] >> (8 * c)) & 255))));
          std::cout << "regionalGroupMaxError=" << maximum << '\n';
          check(maximum <= 2, "regional backdrop group changed nested halo, edge or mask coverage");
        }
    // A circular stroke mask can reuse its exact cached coverage directly in
    // the replacement shader. Check opacity, caps and transforms against the
    // independently rendered mask surface; no blur input is clipped by it.
    auto stroke_mask = layer({{0, 0}, {w, h}}, {}, g::Kind::shape);
    stroke_mask->opacity = .7;
    stroke_mask->stroke = {.4, .2, .8, .65};
    stroke_mask->line_width = 14;
    stroke_mask->path = std::make_shared<g::Path>();
    stroke_mask->path->append({g::SegmentType::arc, {83, 79}, {}, {}, 54, .3, 2.6, true});
    regional_group->transform = {};
    regional_group->set_mask(stroke_mask);
    for (const auto cap : {g::Cap::butt, g::Cap::round, g::Cap::square})
      for (const g::Transform transform : {g::Transform{}, g::Transform{.9, .1, -.1, 1.1, 7, 4}}) {
        stroke_mask->cap = cap;
        stroke_mask->transform = transform;
        const auto direct_mask_result = render_gpu(crop_scene);
        full_groups.compose_scene(crop_scene);
        const auto full_result = full_groups.readback();
        unsigned maximum = 0;
        for (std::size_t i = 0; i < full_result.pixels.size(); ++i)
          for (unsigned c = 0; c < 4; ++c)
            maximum = std::max(
                maximum, unsigned(std::abs(int((direct_mask_result.pixels[i] >> (8 * c)) & 255) -
                                           int((full_result.pixels[i] >> (8 * c)) & 255))));
        check(maximum <= 2, "direct backdrop mask shader changed coverage or opacity");
      }
    check(gpu->scene_cache_stats().backdrop_mask_shaders > 0,
          "direct backdrop mask shader was not exercised");
    // Ordinary source-over isolation can be bounded even with destructive
    // descendants. Their transparent pixels must never erase the parent.
    setenv("HEADUNIT_THEME_PROBE_DISABLE_ISOLATED_BOUNDS", "1", 1);
    g::GpuCompositor full_isolation(w, h, argv[1]);
    unsetenv("HEADUNIT_THEME_PROBE_DISABLE_ISOLATED_BOUNDS");
    auto isolated_scene = layer({{0, 0}, {w, h}}, {.2, .3, .4, .7});
    auto isolated_group = layer({{75, 57}, {64, 59}}, {.8, .4, .2, .6});
    isolated_group->opacity = .7;
    isolated_group->transform = {.95, .2, -.1, 1.1, 7, 8};
    auto isolation_cut = layer({{18, 12}, {17, 19}}, {1, .1, .4, .8});
    isolated_group->insert(isolation_cut, 0);
    isolated_scene->insert(isolated_group, 0);
    for (const auto mode :
         {g::Blend::clear, g::Blend::copy, g::Blend::source_in, g::Blend::source_out,
          g::Blend::source_atop, g::Blend::dest_in, g::Blend::dest_out, g::Blend::dest_atop}) {
      isolation_cut->blend = mode;
      const auto bounded_result = render_gpu(isolated_scene);
      full_isolation.compose_scene(isolated_scene);
      const auto full_result = full_isolation.readback();
      unsigned maximum = 0;
      for (std::size_t i = 0; i < full_result.pixels.size(); ++i)
        for (unsigned c = 0; c < 4; ++c)
          maximum =
              std::max(maximum, unsigned(std::abs(int((bounded_result.pixels[i] >> (8 * c)) & 255) -
                                                  int((full_result.pixels[i] >> (8 * c)) & 255))));
      check(maximum <= 2, "bounded isolation changed destructive child coverage");
    }
    check(gpu->scene_cache_stats().backdrop_output_clips > 0,
          "backdrop output-bound path was not exercised");
    check(gpu->scene_cache_stats().backdrop_mask_replacement_clips > 0,
          "backdrop masked replacement-bound path was not exercised");
    check(gpu->scene_cache_stats().backdrop_group_regions > 0,
          "regional backdrop groups were not exercised");
    check(gpu->scene_cache_stats().backdrop_group_fusions > 0,
          "backdrop group seed fusion was not exercised");
    check(full_groups.scene_cache_stats().backdrop_group_fusions == 0 &&
              full_groups.scene_cache_stats().backdrop_group_regions == 0,
          "reference GPU mode was lost before lazy scene creation");
    // A partial immutable input snapshot must include the filter's complete
    // backwards halo and retain global device coordinates. Compare against the
    // same renderer copying the full parent, including transparent/high-frequency
    // input, chained blurs, clamp edges and rect-preserving transforms.
    setenv("HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_SNAPSHOT_REGION", "1", 1);
    g::GpuCompositor full_snapshot(w, h, argv[1]);
    unsetenv("HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_SNAPSHOT_REGION");
    auto snapshot_scene = layer({{0, 0}, {w, h}});
    auto snapshot_picture = layer({{0, 0}, {w, h}}, {}, g::Kind::image);
    snapshot_picture->image =
        std::make_shared<g::Image>(g::Image{w, h, std::vector<std::uint32_t>(w * h)});
    for (unsigned y = 0; y < h; ++y)
      for (unsigned x = 0; x < w; ++x)
        snapshot_picture->image->pixels[y * w + x] =
            (x + y) % 3 == 0 ? 0 : ((x / 3 + y / 5) % 2 ? 0xffee5544 : 0xff1144bb);
    snapshot_scene->insert(snapshot_picture, 0);
    auto snapshot_group = layer({{0, 0}, {w, h}});
    snapshot_group->group_blending = true;
    snapshot_group->opacity = .63;
    snapshot_group->set_mask(layer({{35, 24}, {53, 49}}, {1, 1, 1, .71}));
    snapshot_group->mask->corner_radius = 11;
    auto snapshot_panel = layer({{-double(w), -double(h)}, {w * 3, h * 3}}, {}, g::Kind::backdrop);
    auto snapshot_blur = std::make_shared<g::BlurFilter>(),
         snapshot_second = std::make_shared<g::BlurFilter>();
    snapshot_second->radius = 2.5;
    snapshot_panel->add_filter(snapshot_blur);
    snapshot_panel->add_filter(snapshot_second);
    snapshot_group->insert(snapshot_panel, 0);
    snapshot_group->insert(layer({{45, 37}, {17, 19}}, {.8, .2, .4, .6}), 1);
    auto snapshot_cut = layer({{50, 40}, {13, 11}}, {1, 1, 1, .7});
    snapshot_cut->blend = g::Blend::dest_out;
    snapshot_group->insert(snapshot_cut, 2);
    snapshot_scene->insert(snapshot_group, 1);
    for (const bool normalized : {false, true})
      for (const double radius : {.5, 3., 9., 25., 64.})
        for (const g::Transform transform :
             {g::Transform{}, g::Transform{.7, 0, 0, 1.4, 101, 47},
              g::Transform{1, 0, 0, 1, -42, -31}, g::Transform{0, 1, -1, 0, 160, 0}}) {
          snapshot_blur->radius = radius;
          snapshot_blur->normalized_edges = normalized;
          snapshot_second->normalized_edges = normalized;
          snapshot_group->transform = transform;
          const auto regional_result = render_gpu(snapshot_scene);
          full_snapshot.compose_scene(snapshot_scene);
          const auto full_result = full_snapshot.readback();
          unsigned maximum = 0;
          for (std::size_t i = 0; i < full_result.pixels.size(); ++i)
            for (unsigned c = 0; c < 4; ++c)
              maximum = std::max(
                  maximum, unsigned(std::abs(int((regional_result.pixels[i] >> (8 * c)) & 255) -
                                             int((full_result.pixels[i] >> (8 * c)) & 255))));
          std::cout << "regionalSnapshotMaxError=" << maximum << '\n';
          check(maximum <= 2,
                "regional snapshot changed filter halo, phase or global edge sampling");
        }
    check(gpu->scene_cache_stats().backdrop_snapshot_regions > 0 &&
              gpu->scene_cache_stats().backdrop_snapshot_saved_pixels > 0,
          "regional immutable backdrop snapshot was not exercised");
    check(full_snapshot.scene_cache_stats().backdrop_snapshot_regions == 0,
          "reference snapshot mode was lost before lazy scene creation");
    // Cropped output work must preserve global-edge sampling and the combined
    // halo of a filter chain, including a translated/scaled panel partly outside
    // the target. Skia's raster reference keeps its full-frame path; the simple
    // independent CPU renderer does not implement global-edge filter tiling.
    auto edge_blur = layer({{-8, double(h) - 65}, {150, 80}}, {}, g::Kind::backdrop);
    edge_blur->transform = {1.05, 0, 0, .95, 3, 0};
    edge_blur->add_filter(blur);
    auto second_blur = std::make_shared<g::BlurFilter>();
    second_blur->radius = 3;
    edge_blur->add_filter(second_blur);
    backdrop_scene->clear();
    backdrop_scene->insert(layer({{0, 0}, {130, h}}, {1, 0, 0, 1}), 0);
    backdrop_scene->insert(layer({{130, 0}, {w - 130, h}}, {0, 0, 1, 1}), 1);
    backdrop_scene->insert(edge_blur, 2);
    const auto edge_result = render_gpu(backdrop_scene);
    const auto edge_reference = g::SkiaScene::raster_probe(backdrop_scene, w, h);
    unsigned edge_error = 0;
    std::uint64_t edge_sum = 0;
    for (std::size_t i = 0; i < edge_result.pixels.size(); ++i)
      for (unsigned c = 0; c < 4; ++c) {
        const auto d = unsigned(std::abs(int((edge_result.pixels[i] >> (8 * c)) & 255) -
                                         int((edge_reference.pixels[i] >> (8 * c)) & 255)));
        edge_error = std::max(edge_error, d);
        edge_sum += d;
      }
    std::cout << "backdropEdgeMaxError=" << edge_error << '\n';
    check(edge_error <= 6 && double(edge_sum) / (w * h * 4) < .1,
          "cropped backdrop lost global-edge or chained-filter samples");
    std::cout << "Backdrop parent sampling, masks, nested opacity, clips, halo and transparent "
                 "alpha passed\n";
    auto isolation = layer({{0, 0}, {w, h}}, {0, 0, 1, 1});
    auto face = layer({{10, 10}, {70, 70}}, {0, 1, 0, 1});
    isolation->insert(face, 0);
    auto band = layer({{10, 10}, {40, 30}}, {1, 0, 0, 1});
    face->insert(band, 0);
    auto cut = layer({{10, 0}, {20, 30}}, {1, 1, 1, 1});
    cut->blend = g::Blend::dest_out;
    band->insert(cut, 0);
    const auto isolated = compare(isolation);
    check(isolated.pixels[25 * w + 35] == 0xff00ff00,
          "destination-out child erased its opaque gauge parent");
    check(isolated.pixels[25 * w + 25] == 0xffff0000, "opaque gauge band lost its uncut segment");
    band->group_blending = false;
    const auto flattened = compare(isolation);
    check(flattened.pixels[25 * w + 35] == 0,
          "explicitly flattened destination-out child was isolated");
    band->group_blending = true;
    auto face_blur = layer({{0, 0}, {70, 70}}, {}, g::Kind::backdrop);
    auto face_filter = std::make_shared<g::BlurFilter>();
    face_filter->radius = 4;
    face_blur->add_filter(face_filter);
    face->insert(face_blur, 0);
    face->set_mask(layer({{0, 0}, {70, 70}}, {1, 1, 1, 1}));
    const auto isolated_blur = render_gpu(isolation);
    const auto raster_isolated_blur = g::SkiaScene::raster_probe(isolation, w, h);
    unsigned isolation_error = 0;
    std::uint64_t isolation_error_sum = 0;
    std::size_t isolation_worst = 0;
    for (std::size_t i = 0; i < isolated_blur.pixels.size(); ++i)
      for (unsigned channel = 0; channel < 4; ++channel) {
        const auto difference =
            unsigned(std::abs(int((isolated_blur.pixels[i] >> (8 * channel)) & 255) -
                              int((raster_isolated_blur.pixels[i] >> (8 * channel)) & 255)));
        isolation_error_sum += difference;
        if (difference > isolation_error) {
          isolation_error = difference;
          isolation_worst = i;
        }
      }
    std::cout << "isolatedBackdropMaxError=" << isolation_error
              << " mean=" << double(isolation_error_sum) / (w * h * 4)
              << " worst=" << isolation_worst % w << ',' << isolation_worst / w
              << " actual=" << std::hex << isolated_blur.pixels[isolation_worst]
              << " expected=" << raster_isolated_blur.pixels[isolation_worst] << std::dec << '\n';
    // GPU/raster blur kernels differ slightly around the face edge; exact
    // interior pixel oracles below still verify the destructive blend itself.
    check(isolation_error <= 12 && double(isolation_error_sum) / (w * h * 4) < .1,
          "GPU isolated backdrop differs from raster composition");
    check(isolated_blur.pixels[25 * w + 35] == 0xff00ff00,
          "isolated gauge cut erased a seeded backdrop");
    band->group_blending = false;
    const auto flattened_blur = render_gpu(isolation);
    // The Cairo backdrop reference rejects transparent filter-edge samples.
    // At these uniform interior pixels the filter cannot change the colour;
    // compare its independently isolated paint/cut result without that filter.
    face->remove(face_blur);
    const auto reference_flattened = g::render(isolation, w, h);
    face->insert(face_blur, 0);
    check(flattened_blur.pixels[25 * w + 35] == 0xff0000ff &&
              flattened_blur.pixels[25 * w + 35] == reference_flattened.pixels[25 * w + 35],
          "direct backdrop cut did not reveal the independent parent");
    check(flattened_blur.pixels[25 * w + 25] == 0xffff0000,
          "direct backdrop cut erased an uncut band");
    // Analytic oracle on a translucent parent. A partially masked/transparent
    // destination-out shape must remove only the group's replacement coverage,
    // preserving premultiplied destination channels, group opacity and AA.
    auto cut_scene = layer({{0, 0}, {w, h}});
    cut_scene->insert(layer({{0, 0}, {130, h}}, {1, 0, 0, .5}), 0);
    cut_scene->insert(layer({{130, 0}, {w - 130, h}}, {0, 0, 1, .25}), 1);
    const auto cut_parent = render_gpu(cut_scene);
    auto cut_panel = layer({{88, 28}, {90, 130}}, {}, g::Kind::backdrop);
    cut_panel->add_filter(blur);
    cut_scene->insert(cut_panel, 2);
    const auto cut_filtered = render_gpu(cut_scene);
    cut_scene->remove(cut_panel);
    auto cut_group = layer({{0, 0}, {w, h}});
    cut_group->opacity = .5;
    cut_group->set_mask(layer({{0, 0}, {w, h}}, {1, 1, 1, .5}));
    cut_group->insert(cut_panel, 0);
    auto marker = layer({{104.25, 61.5}, {49, 55}}, {}, g::Kind::shape);
    marker->fill = {1, 1, 1, .7};
    marker->path = std::make_shared<g::Path>();
    marker->path->append({g::SegmentType::ellipse, {0, 0}, {49, 55}});
    marker->opacity = .5;
    marker->set_mask(layer({{0, 0}, {49, 55}}, {1, 1, 1, .6}));
    const auto marker_coverage = render_gpu(marker);
    g::Image retained_coverage{w, h, std::vector<std::uint32_t>(w * h)};
    for (std::size_t i = 0; i < retained_coverage.pixels.size(); ++i)
      retained_coverage.pixels[i] = (255 - (marker_coverage.pixels[i] >> 24)) << 24;
    marker->blend = g::Blend::dest_out;
    cut_group->insert(marker, 1);
    cut_scene->insert(cut_group, 2);
    expect_mix(render_gpu(cut_scene), cut_parent, cut_filtered, .25, &retained_coverage);
    // Flattened destination-out paints its own source, then descendants use
    // their own modes. It must not treat a later source-over child as a cut.
    marker->set_mask({});
    marker->group_blending = false;
    marker->opacity = 1;
    marker->fill = {1, 1, 1, 1};
    marker->insert(layer({{18, 18}, {9, 9}}, {0, 1, 0, 1}), 0);
    cut_group->opacity = 1;
    cut_group->set_mask({});
    const auto pass_through_cut = render_gpu(cut_scene);
    check(pass_through_cut.pixels[82 * w + 125] == 0xff00ff00,
          "flattened backdrop cut lost source-over descendant");
    check(pass_through_cut.pixels[75 * w + 125] == cut_parent.pixels[75 * w + 125],
          "flattened backdrop cut erased translucent parent");
    // A nested copied group restores its immediate parent, which already
    // contains the outer paint. Restoring the root instead loses that paint.
    auto nested_scene = layer({{0, 0}, {w, h}}, {1, 0, 0, .5});
    auto nested_outer = layer({{0, 0}, {w, h}}, {0, 1, 0, .5});
    nested_outer->opacity = .7;
    nested_scene->insert(nested_outer, 0);
    const auto nested_parent = render_gpu(nested_scene);
    auto nested_inner = layer({{0, 0}, {w, h}});
    nested_inner->opacity = .5;
    nested_inner->set_mask(layer({{0, 0}, {w, h}}, {1, 1, 1, .5}));
    auto nested_panel = layer({{0, 0}, {w, h}}, {}, g::Kind::backdrop);
    nested_panel->add_filter(blur);
    nested_inner->insert(nested_panel, 0);
    nested_inner->insert(layer({{0, 0}, {w, h}}, {0, 0, 1, 1}), 1);
    auto nested_cut = layer({{90, 60}, {50, 60}}, {1, 1, 1, 1});
    nested_cut->blend = g::Blend::dest_out;
    nested_inner->insert(nested_cut, 2);
    nested_outer->insert(nested_inner, 0);
    const auto nested_result = render_gpu(nested_scene);
    for (unsigned c = 0; c < 4; ++c)
      check(std::abs(int((nested_result.pixels[80 * w + 110] >> (8 * c)) & 255) -
                     int((nested_parent.pixels[80 * w + 110] >> (8 * c)) & 255)) <= 1,
            "nested backdrop cut restored the wrong parent");
    // Bound deferred sibling work for cuts as well as blur filters. Validation
    // must fail before replacing the last valid output.
    for (unsigned i = 0; i < 1000; ++i) {
      auto excess_cut = layer({{0, 0}, {1, 1}}, {1, 1, 1, 1});
      excess_cut->blend = g::Blend::dest_out;
      nested_inner->insert(excess_cut, nested_inner->children.size());
    }
    bool cut_work_rejected = false;
    try {
      gpu->compose_scene(nested_scene);
    } catch (const std::exception&) {
      cut_work_rejected = true;
    }
    check(cut_work_rejected && gpu->readback().pixels == nested_result.pixels,
          "backdrop cut work budget changed valid output");
    std::cout << "Direct backdrop cuts: opaque parent, translucent replacement, mask, nested "
                 "parent and work bounds passed\n";
    auto group = layer({{11, 17}, {210, 151}}, {.7, .1, .4, .9});
    group->opacity = .5;
    root->insert(group, 0);
    group->insert(layer({{20, 12}, {100, 80}}, {1, 0, 0, .7}), 0);
    group->insert(layer({{70, 40}, {100, 80}}, {0, 1, 0, .6}), 1);
    compare(root);
    group->group_blending = false;
    compare(root);
    group->group_blending = true;
    group->set_mask(layer({{35, 14}, {96, 115}}, {1, 1, 1, .7}));
    compare(root);
    group->mask->set_mask(layer({{20, 10}, {60, 60}}, {1, 1, 1, .6}));
    compare(root);
    group->mask->hidden = true;
    compare(root);
    group->mask->hidden = false;
    group->set_mask({});
    group->clip = true;
    group->corner_radius = 14;
    compare(root, 16, 1.0);
    auto gradient = layer({{25, 26}, {111, 111}}, {}, g::Kind::gradient);
    gradient->gradient_kind = g::GradientKind::conical;
    gradient->gradient_start = {.5, .5};
    gradient->gradient_end = {1, .5};
    gradient->stops = {
        {{1, 0, 0, 1}, 0}, {{0, 1, 0, .4}, .25}, {{0, 0, 1, .8}, .75}, {{1, 1, 1, 1}, 1}};
    root->clear();
    root->insert(gradient, 0);
    compare(root, 4, 1.0);
    gradient->gradient_end = {.5, 1};
    compare(root, 4, 1.0);
    gradient->gradient_kind = g::GradientKind::linear;
    compare(root, 3, 1.0);
    gradient->gradient_kind = g::GradientKind::radial;
    compare(root, 3, 1.0);
    gradient->stops = {
        {{1, 0, 0, 1}, .1}, {{0, 1, 0, 1}, .3}, {{0, 0, 1, 1}, .3}, {{1, 1, 1, 1}, .9}};
    gradient->gradient_kind = g::GradientKind::conical;
    compare(root, 4, 1.0);
    // Fused gradient plus stroke-coverage masks, including transform/opacity
    // changes and a container chain. Compare every output pixel, not only gauges.
    auto gradient_mask = layer({{0, 0}, {111, 111}}, {}, g::Kind::shape);
    gradient_mask->path = std::make_shared<g::Path>();
    gradient_mask->path->append({g::SegmentType::ellipse, {10, 10}, {91, 91}});
    gradient_mask->stroke = {1, 1, 1, .8};
    gradient_mask->line_width = 8;
    gradient_mask->stroke_start = .13;
    gradient_mask->stroke_end = .86;
    gradient_mask->opacity = .7;
    const auto initial_cache = gpu->scene_cache_stats();
    gradient->set_mask(gradient_mask);
    compare(root, 3, 1.0);
    const auto built_cache = gpu->scene_cache_stats();
    check(built_cache.builds == initial_cache.builds + 1, "masked gradient was not retained");
    compare(root, 3, 1.0);
    check(gpu->scene_cache_stats().hits == built_cache.hits + 1 &&
              gpu->scene_cache_stats().builds == built_cache.builds,
          "unchanged gradient rebuilt");
    gradient->opacity = .4;
    compare(root, 3, 1.0);
    check(gpu->scene_cache_stats().builds == built_cache.builds,
          "opacity-only change rebuilt retained content");
    const auto additive_before = gpu->scene_cache_stats().additive_mask_fusions;
    gradient->blend = g::Blend::add;
    compare(root, 4, 1.0);
    check(gpu->scene_cache_stats().additive_mask_fusions > additive_before,
          "additive gradient mask was not fused");
    gradient->blend = g::Blend::source_over;
    gradient_mask->transform = {.9, .1, -.1, .9, 10, 0};
    compare(root, 3, 1.0);
    auto mask_container = layer({{5, 3}, {111, 111}});
    mask_container->opacity = .6;
    gradient->set_mask({});
    mask_container->insert(gradient_mask, 0);
    gradient->set_mask(mask_container);
    compare(root, 3, 1.0);
    gradient->transform = {.9, -.05, .1, 1.1, 3, 7};
    compare(root, 4, 1.0);
    gradient->corner_radius = 10;
    compare(root, 16, 1.0);  //Ineligible: must use general mask composition.
    gradient->corner_radius = 0;
    gradient->background = {.3, .2, .1, .3};
    compare(root, 4, 1.0);  //Two paints require a group.
    gradient->background = {};
    gradient_mask->hidden = true;
    compare(root, 3, 1.0);
    gradient_mask->hidden = false;
    gradient_mask->stroke_end = .4;
    compare(root, 4, 1.0);
    gradient->stops[0].color = {.3, .5, .2, .8};
    compare(root, 4, 1.0);
    gradient_mask->path->segments[0].p.x += 4;
    compare(root, 4, 1.0);
    // A label-style mask has overlapping image branches, clipping and grouped
    // opacity. Every mutable input must invalidate retained GPU pixels exactly.
    auto glyphs = layer({{0, 0}, {111, 111}});
    glyphs->opacity = .65;
    auto glyph = layer({{7, 9}, {88, 76}}, {}, g::Kind::image);
    glyph->image = std::make_shared<g::Image>(g::Image{8, 8, {}});
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        const unsigned a = 32 + ((x + y) % 8) * 28;
        glyph->image->pixels.push_back((a << 24) | (a << 16) | (a << 8) | a);
      }
    glyphs->insert(glyph, 0);
    gradient->set_mask(glyphs);
    gradient->transform = {};
    gradient->opacity = .7;
    compare(root, 4, 1.0);
    const auto glyph_cache = gpu->scene_cache_stats();
    compare(root, 4, 1.0);
    check(gpu->scene_cache_stats().hits == glyph_cache.hits + 1 &&
              gpu->scene_cache_stats().builds == glyph_cache.builds,
          "glyph mask was not retained");
    glyph->image->pixels[27] = 0xffffffff;
    compare(root, 4, 1.0);
    check(gpu->scene_cache_stats().builds == glyph_cache.builds + 1,
          "image mutation reused stale retained mask");
    auto second_glyph = layer({{30, 25}, {75, 65}}, {}, g::Kind::image);
    second_glyph->image = glyph->image;
    second_glyph->opacity = .8;
    glyphs->insert(second_glyph, 1);
    compare(root, 4, 1.0);
    glyphs->group_blending = false;
    compare(root, 4, 1.0);
    glyph->tint.a = .3;
    compare(root, 4, 1.0);
    glyph->transform = {.9, .1, -.1, .9, 4, 2};
    compare(root, 8, 1.0);
    glyphs->clip = true;
    glyphs->corner_radius = 18;
    glyphs->bounds.size = {85, 75};
    compare(root, 16, 1.0);
    glyphs->remove(second_glyph);
    compare(root, 16, 1.0);
    gradient->gradient_start = {.3, .6};
    compare(root, 16, 1.0);
    // Ancestor clip changes must apply to a retained image, never become baked
    // into it; rounded clips select the ordinary traversal.
    root->clip = true;
    root->bounds.size = {100, 105};
    compare(root, 16, 1.0);
    root->bounds.size = {w, h};
    compare(root, 16, 1.0);
    root->corner_radius = 40;
    root->bounds.size = {100, 105};
    const auto rounded_cache = gpu->scene_cache_stats();
    // The general renderer's fractional rounded-clip/fill intersection differs
    // by26/255 at a curve edge. Use the same bounded32/255 AA allowance as the
    // curved blend cases below, while retaining the1/255 whole-image mean bound.
    // Verify explicitly that this exercises ordinary traversal, not the cache.
    compare(root, 32, 1.0);
    check(gpu->scene_cache_stats().hits == rounded_cache.hits &&
              gpu->scene_cache_stats().builds == rounded_cache.builds,
          "rounded ancestor entered retained cache");
    root->clip = false;
    root->corner_radius = 0;
    root->bounds.size = {w, h};
    glyphs->hidden = true;
    compare(root, 3, 1.0);
    glyphs->hidden = false;
    compare(root, 16, 1.0);
    auto shape = layer({{0, 0}, {150, 150}}, {}, g::Kind::shape);
    shape->path = std::make_shared<g::Path>();
    shape->path->append({g::SegmentType::ellipse, {15, 15}, {90, 90}});
    shape->stroke = {1, .5, .2, 1};
    shape->line_width = 8;
    shape->stroke_start = .1;
    shape->stroke_end = .8;
    shape->cap = g::Cap::round;
    root->clear();
    root->insert(shape, 0);
    const auto circle = compare(root, 3, 1.0);
    // Independent coverage oracle at inner/outer gauge edges, including the
    // sample where coarse GPU path coverage previously missed by77/255.
    unsigned analytic_error = 0;
    for (const double angle : {.25, .37, .5, .63, .75})
      for (const double radius : {41., 49.}) {
        const int cx = std::floor(60 + radius * std::cos(angle * 2 * std::numbers::pi));
        const int cy = std::floor(60 + radius * std::sin(angle * 2 * std::numbers::pi));
        for (int y = cy - 1; y <= cy + 1; ++y)
          for (int x = cx - 1; x <= cx + 1; ++x) {
            unsigned inside = 0;
            constexpr unsigned samples = 64;
            for (unsigned sy = 0; sy < samples; ++sy)
              for (unsigned sx = 0; sx < samples; ++sx) {
                const auto dx = x + (sx + .5) / samples - 60, dy = y + (sy + .5) / samples - 60,
                           rr = std::hypot(dx, dy);
                inside += rr >= 41 && rr <= 49;
              }
            const auto coverage = double(inside) / (samples * samples);
            const auto actual = circle.pixels[std::size_t(y) * w + x];
            const std::array<double, 4> expected{
                .2 * coverage + .3 * .4 * (1 - coverage), .5 * coverage + .2 * .4 * (1 - coverage),
                coverage + .1 * .4 * (1 - coverage), coverage + .4 * (1 - coverage)};
            for (unsigned c = 0; c < 4; ++c)
              analytic_error = std::max(
                  analytic_error, unsigned(std::ceil(std::abs(double((actual >> (8 * c)) & 255) -
                                                              255 * expected[c]))));
          }
      }
    std::cout << "analyticGaugeMaxError=" << analytic_error
              << '\n';  // Cairo's0.1px path flattening reaches22/255 at these boundaries. Keep
    // a24/255 absolute limit as well as the full GPU/reference pixel comparison.
    if (analytic_error > 24) ++failures;
    // Script updates can replace a path object without changing its geometry.
    // Reuse the exact coverage then, but invalidate it when a segment changes.
    const auto stroke_before = gpu->scene_cache_stats();
    shape->path = std::make_shared<g::Path>(*shape->path);
    check(compare(root, 3, 1.0).pixels == circle.pixels, "equivalent path changed stroke pixels");
    const auto stroke_reused = gpu->scene_cache_stats();
    check(stroke_reused.stroke_hits > stroke_before.stroke_hits &&
              stroke_reused.stroke_builds == stroke_before.stroke_builds,
          "equivalent path missed coverage cache");
    shape->path->segments[0].p.x += 3;
    compare(root, 3, 1.0);
    check(gpu->scene_cache_stats().stroke_builds > stroke_reused.stroke_builds,
          "changed path reused stale coverage");
    shape->path->segments[0].p.x -= 3;
    shape->line_width = 5;
    compare(root, 3, 1.0);
    shape->stroke_end = .4;
    compare(root, 3, 1.0);
    shape->path->segments[0].c1.x = 115;
    compare(root, 3, 1.0);
    shape->transform = {.9, .12, -.13, 1.05, 18, -4};
    compare(root, 4, 1.0);
    shape->transform = {};
    shape->line_width = 8;
    shape->stroke_end = .8;
    {
      // Compare cropped exact coverage against the same GPU renderer retaining
      // every transparent pixel. Include zero/short trims, cap ends, transformed
      // additive gradients and backdrop filter halos around the cropped support.
      ::setenv("HEADUNIT_THEME_PROBE_DISABLE_COVERAGE_CROP", "1", 1);
      g::GpuCompositor uncropped(w, h, argv[1]);
      ::unsetenv("HEADUNIT_THEME_PROBE_DISABLE_COVERAGE_CROP");
      g::GpuCompositor cropped(w, h, argv[1]);
      auto fixture = layer({{0, 0}, {w, h}}, {.1, .2, .3, 1});
      auto stripes = layer({{0, 0}, {w, h}}, {}, g::Kind::image);
      stripes->image =
          std::make_shared<g::Image>(g::Image{w, h, std::vector<std::uint32_t>(w * h)});
      for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
          stripes->image->pixels[y * w + x] =
              0xff000000u | ((x * 7 % 256) << 16) | ((y * 13 % 256) << 8) | ((x + y) * 3 % 256);
      fixture->insert(stripes, 0);
      auto panel = layer({{25, 17}, {190, 158}});
      auto cover = layer({{0, 0}, {190, 158}}, {}, g::Kind::shape);
      cover->path = std::make_shared<g::Path>();
      cover->path->append({g::SegmentType::ellipse, {25, 10}, {140, 140}});
      cover->stroke = {1, 1, 1, .8};
      cover->opacity = .7;
      cover->cap = g::Cap::round;
      panel->set_mask(cover);
      auto tint = layer({{0, 0}, {190, 158}}, {}, g::Kind::gradient);
      tint->gradient_kind = g::GradientKind::conical;
      tint->gradient_start = {.5, .5};
      tint->gradient_end = {1, .5};
      tint->blend = g::Blend::add;
      tint->stops = {{{.9, .3, .1, .8}, 0}, {{.1, .7, .8, .6}, .5}, {{.5, .2, .9, .9}, 1}};
      auto blur_node = layer({{0, 0}, {190, 158}}, {}, g::Kind::backdrop);
      auto filter = std::make_shared<g::BlurFilter>();
      filter->radius = 9;
      filter->hard_edges = true;
      blur_node->add_filter(filter);
      fixture->insert(panel, 1);
      for (const double width : {3., 30., 54.})
        for (const double end : {0., .012, .13, .47, 1.})
          for (const g::Transform transform :
               {g::Transform{}, g::Transform{.8, .12, -.08, 1.05, 7, -3}})
            for (const bool backdrop : {false, true}) {
              cover->line_width = width;
              cover->stroke_end = end;
              cover->transform = transform;
              tint->blend = backdrop ? g::Blend::source_over : g::Blend::add;
              panel->clear();
              if (backdrop) panel->insert(blur_node, 0);
              panel->insert(tint, panel->children.size());
              uncropped.compose_scene(fixture);
              const auto before = uncropped.readback();
              cropped.compose_scene(fixture);
              const auto after = cropped.readback();
              unsigned maximum = 0;
              for (std::size_t i = 0; i < before.pixels.size(); ++i)
                for (unsigned c = 0; c < 4; ++c)
                  maximum = std::max(maximum,
                                     unsigned(std::abs(int((before.pixels[i] >> (8 * c)) & 255) -
                                                       int((after.pixels[i] >> (8 * c)) & 255))));
              check(maximum <= 2, "coverage crop changed trim, AA, transform or blur halo");
            }
    }
    {
      // Wide reference-style gauges exercise the tighter circular stroke bounds
      // and curved trim endpoints independently of the small scene fixture.
      constexpr unsigned large = 640;
      g::GpuCompositor wide_gpu(large, large, argv[1]);
      auto wide_root = layer({{0, 0}, {large, large}});
      auto wide_arc = std::make_shared<g::Layer>(g::Kind::shape);
      wide_arc->anchor = {0, 0};
      wide_arc->stroke = {1, .4, .2, 1};
      wide_arc->line_width = 94;
      wide_root->insert(wide_arc, 0);
      for (const bool clockwise : {true, false})
        for (const auto cap : {g::Cap::butt, g::Cap::round, g::Cap::square})
          for (const double finish : {.13, .37, .83})
            for (const g::Transform transform :
                 {g::Transform{}, g::Transform{.9, .1, -.1, .8, 75, 40}}) {
              const auto pi = std::numbers::pi;
              g::Segment arc{g::SegmentType::arc,
                             {320, 320},
                             {},
                             {},
                             233,
                             clockwise ? .75 * pi : -.75 * pi,
                             clockwise ? 2.25 * pi : -2.25 * pi,
                             clockwise};
              wide_arc->path = std::make_shared<g::Path>();
              wide_arc->path->append(arc);
              wide_arc->cap = cap;
              wide_arc->transform = transform;
              wide_arc->stroke_start = .07;
              wide_arc->stroke_end = finish;
              wide_gpu.compose_scene(wide_root);
              const auto actual = wide_gpu.readback();
              const auto start = arc.start;
              arc.start = start + (arc.end - start) * .07;
              arc.end = start + (arc.end - start) * finish;
              wide_arc->path = std::make_shared<g::Path>();
              wide_arc->path->append(arc);
              wide_arc->stroke_start = 0;
              wide_arc->stroke_end = 1;
              const auto expected = g::render(wide_root, large, large);
              unsigned maximum = 0;
              for (std::size_t i = 0; i < actual.pixels.size(); ++i)
                for (unsigned c = 0; c < 4; ++c)
                  maximum = std::max(
                      maximum, unsigned(std::abs(int((actual.pixels[i] >> (8 * c)) & 255) -
                                                 int((expected.pixels[i] >> (8 * c)) & 255))));
              check(maximum <= 3, "wide circular stroke lost curvature or cropped a cap");
            }
    }
    shape->group_blending = false;
    for (unsigned i = 0; i <= unsigned(g::Blend::minimum); ++i) {
      shape->blend = g::Blend(i);
      compare(root, 32, 1.0);
    }
    shape->group_blending = true;
    shape->blend = g::Blend::source_over;
    shape->path = std::make_shared<g::Path>();
    shape->path->append({g::SegmentType::move, {20, 20}});
    shape->path->append({g::SegmentType::cubic, {120, 110}, {60, 10}, {10, 100}});
    shape->path->append({g::SegmentType::line, {20, 130}});
    shape->path->append({g::SegmentType::close});
    shape->fill = {.3, .5, .8, .7};
    shape->stroke_start = 0;
    shape->stroke_end = 1;
    compare(root, 3, 1.0);
    auto image = layer({{15, 20}, {128, 96}}, {}, g::Kind::image);
    image->image = std::make_shared<g::Image>(g::Image{128, 96, {}});
    for (unsigned y = 0; y < 96; ++y)
      for (unsigned x = 0; x < 128; ++x)
        image->image->pixels.push_back(0xff000000u | (x * 2u << 16) | (y * 2u << 8) | 42);
    root->clear();
    root->insert(image, 0);
    compare(root);
    image->tint = {.3, .7, .8, .6};
    compare(root, 3, 1.0);
    image->image->pixels[13] = 0xffffffff;
    compare(root, 3, 1.0);  //Mutation must invalidate GPU image cache.
    image->transform = {1, .15, .1, 1, 0, 0};
    image->opacity = .4;
    compare(root, 8, 1.0);
    root->clear();
    root->insert(layer({{20, 20}, {60, 50}}, {.4, .6, .8, .8}), 0);
    auto mixed = layer({{45, 45}, {80, 65}}, {.8, .2, .5, .6});
    root->insert(mixed, 1);
    for (unsigned i = 0; i <= unsigned(g::Blend::minimum); ++i) {
      mixed->blend = g::Blend(i);
      compare(root, 3, 1.0);
    }
    // State transitions must work after Skia has changed GL bindings/stencil.
    const auto cpu = g::render(root, w, h);
    gpu->compose_reference(cpu);
    check(gpu->readback().pixels == cpu.pixels, "reference upload after Skia failed");
    g::GpuConicalGradient primitive;
    primitive.bounds = {{0, 0}, {double(w), double(h)}};
    primitive.start = {.5, .5};
    primitive.end = {1, .5};
    primitive.stops = {{{1, 0, 0, 1}, 0}, {{0, 1, 0, 1}, 1}};
    gpu->compose_conical(std::span(&primitive, 1));
    compare(root, 3, 1.0);
    auto invalid = layer({{0, 0}, {w, h}}, {1, 1, 1, 1});
    invalid->opacity = std::numeric_limits<double>::quiet_NaN();
    bool rejected = false;
    try {
      gpu->compose_scene(invalid);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "invalid scene accepted");
    compare(root, 3, 1.0);
    invalid->opacity = 1;
    invalid->blend = g::Blend::component_mask;
    rejected = false;
    try {
      gpu->compose_scene(invalid);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected, "unsupported scene accepted");
    compare(root, 3, 1.0);
    if (wayland)
      for (const bool preview : {false, true}) {
        // Declare opacity only when the final buffer really has opaque pixels.
        // This must preserve premultiplied scene colours and reference alpha,
        // including the shared-memory diagnostic path.
        g::WaylandOptions options{"Headunit opaque window fixture"};
        options.opaque_background = true;
        if (preview) options.readback_render_node = argv[1];
        g::GpuCompositor opaque(128, 96, std::move(options));
        auto transparent = layer({{0, 0}, {128, 96}});
        auto colour = layer({{19, 15}, {67, 51}}, {.8, .3, .5, .47});
        colour->corner_radius = 7;
        transparent->insert(colour, 0);
        opaque.compose_scene(transparent);
        const auto reference = opaque.readback();
        check(reference.pixels.front() == 0,
              "opaque window changed transparent scene/reference pixels");
        g::Image window;
        const auto receipt = opaque.present(1, &window);
        check(receipt.presented && window.width == reference.width &&
                  window.height == reference.height,
              "opaque window fixture was not presented");
        for (std::size_t i = 0; i < window.pixels.size(); ++i)
          check(window.pixels[i] == (reference.pixels[i] | 0xff000000),
                "opaque window changed premultiplied colour or left transparent pixels");
      }
    check(failures == 0, "GPU scene pixel/analytic comparisons failed");
    std::cout << "gpuScenePassed=true cases=" << cases << " renderer=" << gpu->renderer() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
