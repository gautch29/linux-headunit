// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_compositor.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace g = headunit::theme::graphics;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "invalid conical command accepted");
}
// Independent area oracle for the opaque-stop, rounded-edge fixtures. Cairo
// flattens arcs at its default 0.1px tolerance, so it is not an exact coverage
// oracle at transformed curves. Integrate the analytic rectangle/circle union
// on a denser grid than the shader and check alpha against that separately.
std::pair<bool, double> edge_alpha(std::span<const g::GpuConicalGradient> paints, g::Color clear,
                                   unsigned x, unsigned y) {
  double alpha = clear.a;
  bool edge = false;
  for (const auto& p : paints) {
    if (p.opacity == 0 || p.bounds.size.width == 0 || p.bounds.size.height == 0) continue;
    for (const auto& s : p.stops)
      check(s.color.a == 1, "coverage oracle requires opaque gradient stops");
    const auto inverse = *p.to_output.inverse();
    const auto w = p.bounds.size.width, h = p.bounds.size.height;
    const auto r = std::clamp(p.corner_radius, 0., std::min(w, h) / 2);
    unsigned inside = 0;
    constexpr unsigned n = 64;
    for (unsigned sy = 0; sy < n; ++sy)
      for (unsigned sx = 0; sx < n; ++sx) {
        const auto v = inverse.apply({x + (sx + .5) / n, y + (sy + .5) / n});
        const auto lx = v.x - p.bounds.origin.x, ly = v.y - p.bounds.origin.y;
        if (lx < 0 || ly < 0 || lx > w || ly > h) continue;
        if ((lx >= r && lx <= w - r) || (ly >= r && ly <= h - r)) {
          ++inside;
          continue;
        }
        const auto cx = lx < r ? r : w - r, cy = ly < r ? r : h - r;
        if ((lx - cx) * (lx - cx) + (ly - cy) * (ly - cy) <= r * r) ++inside;
      }
    const auto coverage = double(inside) / (n * n);
    edge |= inside > 0 && inside < n * n;
    alpha = p.opacity * coverage + alpha * (1 - p.opacity * coverage);
  }
  return {edge, alpha * 255};
}
std::shared_ptr<g::Layer> reference(std::span<const g::GpuConicalGradient> paints, g::Color clear,
                                    unsigned width, unsigned height) {
  auto root = std::make_shared<g::Layer>();
  root->set_frame({{0, 0}, {double(width), double(height)}});
  root->background = clear;
  for (const auto& paint : paints) {
    auto layer = std::make_shared<g::Layer>(g::Kind::gradient);
    layer->bounds = paint.bounds;
    layer->anchor = {0, 0};
    layer->transform = paint.to_output;
    // Layer::local_transform subtracts bounds.origin even with zero anchor.
    // Compensate in the local transform, preserving arbitrary nonzero bounds.
    layer->transform =
        paint.to_output.multiply({1, 0, 0, 1, paint.bounds.origin.x, paint.bounds.origin.y});
    layer->gradient_kind = g::GradientKind::conical;
    layer->gradient_start = paint.start;
    layer->gradient_end = paint.end;
    layer->stops = paint.stops;
    layer->corner_radius = paint.corner_radius;
    layer->opacity = paint.opacity;
    // Primitive opacity is paint opacity. Group isolation of complete subtrees
    // is the scene traversal's separate responsibility.
    layer->group_blending = false;
    if (paint.mask) {
      auto mask = std::make_shared<g::Layer>(g::Kind::image);
      if (!paint.mask->empty()) {
        mask->bounds = {{0, 0}, {double(paint.mask->width()), double(paint.mask->height())}};
        mask->anchor = {0, 0};
        mask->transform = paint.to_output.inverse()->multiply(
            {1, 0, 0, 1, double(paint.mask->x()), double(paint.mask->y())});
        mask->image =
            std::make_shared<g::Image>(g::Image{paint.mask->width(), paint.mask->height(), {}});
        for (const auto a : paint.mask->pixels())
          mask->image->pixels.push_back(std::uint32_t(a) * 0x01010101u);
      }
      layer->set_mask(mask);
    }
    root->insert(layer, root->children.size());
  }
  return root;
}
int main(int argc, char** argv) {
  try {
    check(argc == 2, "usage: gpu-conical-test RENDER_NODE");
    constexpr unsigned width = 257, height = 193;
    std::unique_ptr<g::GpuCompositor> gpu;
    const bool wayland = std::getenv("HEADUNIT_THEME_GPU_PROBE_WAYLAND") != nullptr;
    if (wayland)
      gpu = std::make_unique<g::GpuCompositor>(width, height,
                                               g::WaylandOptions{"Headunit GPU conical fixture"});
    else
      gpu = std::make_unique<g::GpuCompositor>(width, height, argv[1]);
    const g::Color clear{.1, .2, .3, .4};
    g::GpuConicalGradient paint;
    paint.bounds = {{0, 0}, {101, 101}};
    paint.to_output = {1, 0, 0, 1, 10, 20};
    paint.start = {.5, .5};
    paint.end = {1, .5};
    paint.stops = {{{1, 0, 0, 1}, 0},
                   {{0, 1, 0, 1}, .25},
                   {{0, 0, 1, 1}, .5},
                   {{1, 1, 0, 1}, .75},
                   {{1, 0, 0, 1}, 1}};
    std::vector<g::GpuConicalGradient> paints{paint};
    unsigned cases = 0, worst = 0;
    std::uint64_t total_error = 0, samples = 0;
    auto compare = [&](unsigned tolerance) {
      const auto cpu = g::render(reference(paints, clear, width, height), width, height);
      const auto timing = gpu->compose_conical(paints, clear);
      const auto actual = gpu->readback();
      unsigned error = 0;
      std::uint64_t sum = 0;
      std::size_t worst_pixel = 0;
      for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
        unsigned pixel_error = 0;
        for (unsigned c = 0; c < 4; ++c) {
          const auto delta = unsigned(std::abs(int((cpu.pixels[i] >> (8 * c)) & 255) -
                                               int((actual.pixels[i] >> (8 * c)) & 255)));
          if (delta > error) {
            error = delta;
            worst_pixel = i;
          }
          sum += delta;
          pixel_error = std::max(pixel_error, delta);
        }
        if (pixel_error > 2 && tolerance > 2 &&
            std::none_of(paints.begin(), paints.end(),
                         [](const auto& p) { return bool(p.mask); })) {
          const auto [edge, alpha] = edge_alpha(paints, clear, i % width, i / width);
          check(edge, "GPU conical interior differs from reference");
          if (std::abs(double(actual.pixels[i] >> 24) - alpha) > 4)
            std::cout << "edge=" << i % width << ',' << i / width
                      << " actualAlpha=" << (actual.pixels[i] >> 24) << " oracleAlpha=" << alpha
                      << '\n';
          check(std::abs(double(actual.pixels[i] >> 24) - alpha) <= 4,
                "GPU conical edge differs from analytic coverage");
        }
      }
      worst = std::max(worst, error);
      total_error += sum;
      samples += cpu.pixels.size() * 4;
      std::cout << "case=" << ++cases << " maxError=" << error
                << " meanError=" << double(sum) / (cpu.pixels.size() * 4)
                << " drawNs=" << timing.draw_ns << '\n';
      if (error > tolerance)
        std::cout << "worstPixel=" << worst_pixel % width << ',' << worst_pixel / width
                  << " cpu=" << std::hex << cpu.pixels[worst_pixel]
                  << " gpu=" << actual.pixels[worst_pixel] << std::dec << '\n';
      check(error <= tolerance, "GPU conical pixels differ from CPU reference");
      if (wayland) {
        g::Image presented;
        const auto receipt = gpu->present(cases, &presented);
        check(receipt.presented && receipt.token == cases && presented.pixels == actual.pixels,
              "GPU conical Wayland blit/receipt failed");
      }
    };
    compare(2);  // Cardinal colours and the exact gradient centre, all pixels.
    paints[0].opacity = .5;
    paints[0].stops = {{{1, 0, 0, 1}, 0}, {{0, 0, 1, 0}, 1}};
    compare(2);
    paints[0].opacity = 1;
    paints[0].stops = {
        {{1, 0, 0, 1}, .1}, {{0, 1, 0, 1}, .25}, {{0, 0, 1, 1}, .25}, {{1, 1, 1, 1}, .9}};
    compare(2);
    paints[0].end = {.5, 1};
    compare(2);
    paints[0] = paint;
    paints[0].stops.clear();
    for (unsigned i = 0; i < 32; ++i)
      paints[0].stops.push_back(
          {{double(i % 3) / 2, double(i % 5) / 4, double(i % 7) / 6, 1}, double(i) / 31});
    compare(2);  // Exercise the last uniform slot and every interpolation segment.
    paints[0].stops = {{{.3, .7, .1, .6}, .4}};
    compare(2);
    paints[0] = paint;
    paints[0].to_output = {-1, 0, 0, 1, 220, 20};
    compare(2);
    paints[0] = paint;
    paints[0].bounds.origin = {-13, 7};
    paints[0].to_output = {.85, .4, -.25, 1.1, 60.25, 20.75};
    paints[0].corner_radius = 22;
    compare(32);
    paints.push_back(paint);
    paints[1].to_output = {-1.1, .15, .2, .7, 225.5, 45.25};
    paints[1].opacity = .35;
    paints[1].corner_radius = 12;
    compare(32);
    // Degenerate/offscreen commands cannot leave stale pixels from earlier work.
    paints[0].bounds.size.width = 0;
    paints[1].to_output.tx = 10000;
    compare(2);
    auto bad = paint;
    bad.end = bad.start;
    rejects([&] { gpu->compose_conical({&bad, 1}); });
    bad = paint;
    bad.to_output = {0, 0, 0, 0, 0, 0};
    rejects([&] { gpu->compose_conical({&bad, 1}); });
    bad = paint;
    bad.stops[0].location = .9;
    rejects([&] { gpu->compose_conical({&bad, 1}); });
    bad = paint;
    bad.stops.resize(33);
    rejects([&] { gpu->compose_conical({&bad, 1}); });
    bad = paint;
    bad.opacity = NAN;
    rejects([&] { gpu->compose_conical({&bad, 1}); });
    paints = {paint};
    compare(2);  // Validation errors did not poison the context.
    {
      g::Image coverage{53, 47, std::vector<std::uint32_t>(53 * 47)};
      for (unsigned y = 2; y < 45; ++y)
        for (unsigned x = 3; x < 50; ++x)
          coverage.pixels[y * 53 + x] =
              ((x + y) % 7 == 0 ? 0u : std::uint32_t((x * 11 + y * 17) % 256)) << 24;
      paints[0].mask = std::make_shared<g::AlphaMask>(coverage, 29, 41);
      compare(3);
      const auto uploaded = gpu->mask_stats();
      check(uploaded.uploads == 1 && uploaded.entries == 1, "mask was not cached");
      compare(3);
      check(gpu->mask_stats().uploads == uploaded.uploads, "unchanged mask was uploaded again");
      coverage.pixels.assign(53 * 47, 0xffffffff);
      compare(3);  //Source image mutation cannot change the immutable mask.
      paints[0].mask = std::make_shared<g::AlphaMask>(coverage, -20, 60);
      compare(3);
      check(gpu->mask_stats().uploads == uploaded.uploads + 1 && gpu->mask_stats().entries == 1,
            "mask replacement reused stale pixels");
      paints[0].mask =
          std::make_shared<g::AlphaMask>(g::Image{8, 8, std::vector<std::uint32_t>(64)});
      compare(2);
      check(gpu->mask_stats().entries == 0, "transparent/expired mask retained GPU storage");
      //More than32 live identities force LRU eviction without corrupting output.
      std::vector<std::shared_ptr<const g::AlphaMask>> identities;
      for (unsigned i = 0; i < 35; ++i) {
        identities.push_back(
            std::make_shared<g::AlphaMask>(g::Image{1, 1, {0xff000000}}, 40 + i, 60));
        paints[0].mask = identities.back();
        gpu->compose_conical(paints, clear);
      }
      auto stats = gpu->mask_stats();
      check(stats.entries == 32 && stats.resident_bytes == 32, "mask cache exceeded its bounds");
      paints[0].mask = identities.front();
      compare(3);
      check(gpu->mask_stats().uploads == stats.uploads + 1, "evicted mask was not uploaded again");
      identities.clear();
      paints[0].mask.reset();
      gpu->compose_conical(paints, clear);
      check(gpu->mask_stats().entries == 0 && gpu->mask_stats().resident_bytes == 0,
            "dead mask identities retained textures");
    }
    {
      const g::Image large{4096, 2160, std::vector<std::uint32_t>(4096 * 2160, 0xff000000)};
      std::vector<std::shared_ptr<const g::AlphaMask>> identities;
      for (unsigned i = 0; i < 8; ++i) {
        identities.push_back(std::make_shared<g::AlphaMask>(large));
        paints[0].mask = identities.back();
        gpu->compose_conical(paints, clear);
        check(gpu->mask_stats().resident_bytes <= 64 * 1024 * 1024,
              "mask cache exceeded its byte budget");
      }
      check(gpu->mask_stats().entries == 7,
            "mask byte budget did not evict the oldest large image");
      identities.clear();
      paints[0].mask.reset();
      gpu->compose_conical(paints, clear);
      check(gpu->mask_stats().resident_bytes == 0, "large dead masks retained storage");
    }
    g::GpuCompositor benchmark(1920, 720, argv[1]);
    std::array<g::GpuConicalGradient, 4> gauges;
    for (unsigned i = 0; i < gauges.size(); ++i) {
      gauges[i] = paint;
      gauges[i].bounds = {{0, 0}, {1000, 720}};
      gauges[i].to_output.tx = i * 280.;
      gauges[i].to_output.ty = 0;
    }
    benchmark.compose_conical(gauges);  // Compile/warm once, outside measurements.
    std::vector<std::uint64_t> elapsed;
    for (unsigned i = 0; i < 30; ++i) {
      gauges[0].end = {1, .5 + .1 * std::sin(i * .1)};
      elapsed.push_back(benchmark.compose_conical(gauges).draw_ns);
    }
    std::sort(elapsed.begin(), elapsed.end());
    const auto reference_root = reference(gauges, {}, 1920, 720);
    std::array<std::uint64_t, 3> cpu_elapsed{};
    g::Image cpu_benchmark;
    for (auto& cost : cpu_elapsed) {
      const auto began = std::chrono::steady_clock::now();
      cpu_benchmark = g::render(reference_root, 1920, 720);
      cost = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                  began)
                 .count();
    }
    std::sort(cpu_elapsed.begin(), cpu_elapsed.end());
    const auto gpu_benchmark = benchmark.readback();
    unsigned benchmark_error = 0;
    for (std::size_t i = 0; i < cpu_benchmark.pixels.size(); ++i)
      for (unsigned c = 0; c < 4; ++c)
        benchmark_error = std::max(
            benchmark_error, unsigned(std::abs(int((cpu_benchmark.pixels[i] >> (c * 8)) & 255) -
                                               int((gpu_benchmark.pixels[i] >> (c * 8)) & 255))));
    check(benchmark_error <= 2, "large GPU conical fixture differs from reference");
    auto ring = std::make_shared<g::Layer>(g::Kind::shape);
    ring->path = std::make_shared<g::Path>();
    ring->path->append({g::SegmentType::ellipse, {260, 120}, {480, 480}});
    ring->stroke = {1, 1, 1, 1};
    ring->line_width = 16;
    ring->stroke_start = .08;
    ring->stroke_end = .92;
    const auto ring_pixels = g::render(ring, 1000, 720);
    for (unsigned i = 0; i < gauges.size(); ++i)
      gauges[i].mask = std::make_shared<g::AlphaMask>(ring_pixels, i * 280, 0);
    benchmark.compose_conical(gauges);
    const auto mask_stats = benchmark.mask_stats();
    check(mask_stats.uploads == 4, "gauge masks were not uploaded once");
    std::vector<std::uint64_t> masked_elapsed;
    for (unsigned i = 0; i < 30; ++i) {
      gauges[0].end = {1, .5 + .1 * std::sin(i * .1)};
      masked_elapsed.push_back(benchmark.compose_conical(gauges).draw_ns);
    }
    check(benchmark.mask_stats().uploads == mask_stats.uploads,
          "gauge benchmark reuploaded unchanged masks");
    const auto masked_root = reference(gauges, {}, 1920, 720);
    std::array<std::uint64_t, 3> masked_cpu_elapsed{};
    for (auto& cost : masked_cpu_elapsed) {
      const auto began = std::chrono::steady_clock::now();
      cpu_benchmark = g::render(masked_root, 1920, 720);
      cost = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                  began)
                 .count();
    }
    const auto masked_gpu = benchmark.readback();
    unsigned masked_error = 0;
    for (std::size_t i = 0; i < cpu_benchmark.pixels.size(); ++i)
      for (unsigned c = 0; c < 4; ++c)
        masked_error = std::max(masked_error,
                                unsigned(std::abs(int((cpu_benchmark.pixels[i] >> (c * 8)) & 255) -
                                                  int((masked_gpu.pixels[i] >> (c * 8)) & 255))));
    check(masked_error <= 3, "masked GPU gauge pixels differ from reference");
    std::sort(masked_elapsed.begin(), masked_elapsed.end());
    std::sort(masked_cpu_elapsed.begin(), masked_cpu_elapsed.end());
    std::cout << "cases=" << cases << " maxChannelError=" << worst
              << " meanChannelError=" << double(total_error) / samples
              << " gpuMedianNs=" << elapsed[15] << " gpuP95Ns=" << elapsed[28]
              << " cpuMedianNs=" << cpu_elapsed[1] << " benchmarkMaxError=" << benchmark_error
              << " renderer=" << gpu->renderer() << " maskedGpuMedianNs=" << masked_elapsed[15]
              << " maskedGpuP95Ns=" << masked_elapsed[28]
              << " maskedCpuMedianNs=" << masked_cpu_elapsed[1]
              << " maskedMaxError=" << masked_error << " maskBytes=" << mask_stats.resident_bytes
              << " presented=" << wayland << " syntheticFixture=true fullUltra=false\n";
  } catch (const std::exception& e) {
    std::cerr << "gpuConical: " << e.what() << '\n';
    return 1;
  }
}
