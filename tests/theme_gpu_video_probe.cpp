// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_compositor.hpp>
#include <headunit/theme/video_decoder.hpp>
#include <headunit/theme/script_runtime.hpp>
#include <headunit/theme/video_stream.hpp>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <thread>

using namespace headunit::theme;
namespace v = video;
namespace g = graphics;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
int main(int argc, char** argv) {
  try {
    check(argc == 2 || argc == 3, "usage: gpu-video-probe RENDER_NODE [PNG]");
    const bool wayland = std::getenv("HEADUNIT_THEME_GPU_PROBE_WAYLAND") != nullptr;
    const bool defer_scene_fence =
        wayland && std::getenv("HEADUNIT_THEME_GPU_PROBE_DEFER_FENCE") != nullptr;
    const bool retire_pending = std::getenv("HEADUNIT_THEME_GPU_PROBE_RETIRE_PENDING") != nullptr;
    check(!retire_pending || defer_scene_fence,
          "pending retirement fixture requires deferred Wayland scenes");
    unsigned native_retirement_cycles = 0;
    const unsigned lease_limit = std::getenv("HEADUNIT_THEME_GPU_PROBE_FOUR_LEASES") ? 4 : 3;
    std::array<std::uint8_t, 32> key{};
    std::cin.read(reinterpret_cast<char*>(key.data()), key.size());
    check(bool(std::cin), "missing fixture key");
    headunit::AirplayVideoStream stream(key);
    std::unique_ptr<v::Decoder> hardware, software;
    std::unique_ptr<g::GpuCompositor> gpu;
    std::deque<std::shared_ptr<const v::NativeFrame>> leased;
    std::map<std::uint64_t, g::Image> reference;
    std::shared_ptr<const v::NativeFrame> final_lease;
    std::vector<g::GpuQuad> overlays{{{{32, 32}, {256, 128}}, {1, 0, 0, .5}},
                                     {{{160, 96}, {256, 128}}, {0, 1, 0, .25}}};
    if (std::getenv("HEADUNIT_THEME_GPU_PROBE_OVERLAY_STRESS")) {
      // Many partly overlapping rectangles exercise the batched VBO path and
      // verify that their per-draw colours and ordering survive the upload.
      for (unsigned i = 0; i < 64; ++i)
        overlays.push_back({{{double((i * 71) % 900), double((i * 47) % 500)}, {88, 64}},
                            {double(i % 3 == 0), double(i % 3 == 1), double(i % 3 == 2),
                             .08 + double(i % 4) * .05}});
    }
    unsigned frames = 0, max_error = 0, presentations = 0;
    std::uint64_t total_draw_ns = 0;
    bool tested_bound = false, native_scene_tested = false;
    g::Image last;
    unsigned native_scene_error = 0, native_masked_error = 0;
    v::MediaClock media;
    v::PresentationClock presentation_clock;
    const auto monotonic = [] {
      return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    };
    const auto fixture_origin = monotonic();
    auto refresh_fixture_clock = [&] {
      // Deliberately synthetic media synchronization for this encoded fixture.
      // The presentation event and its cross-clock sample are real Wayland data.
      const auto at = monotonic(), elapsed = at - fixture_origin;
      const auto ntp =
          ((900 + elapsed / 1000000000) << 32) + ((elapsed % 1000000000) << 32) / 1000000000;
      media.update({true, ntp + (std::uint64_t{2208988800} << 32), at, at, at, 1000});
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    auto drain = [&](bool flush = false) {
      check(std::chrono::steady_clock::now() < deadline, "GPU probe timed out");
      while (auto frame = software->pull()) {
        check(reference.size() < 32, "reference image queue exceeded");
        reference.emplace(frame->sender_ntp, std::move(frame->image));
      }
      while (leased.size() < lease_limit) {
        auto frame = hardware->pull_native();
        if (!frame) break;
        leased.push_back(std::move(frame));
      }
      if (leased.size() == lease_limit) {
        check(!hardware->pull_native(), "native lease limit failed");
        tested_bound = true;
      }
      while (!leased.empty() && (flush || leased.size() == lease_limit)) {
        const auto& frame = leased.front();
        const auto found = reference.find(frame->metadata.sender_ntp);
        if (found == reference.end()) break;
        if (flush) final_lease = frame;
        if (frames == 0) {
          std::cerr << "native layout " << frame->storage_width << 'x' << frame->storage_height
                    << " crop " << frame->crop_x << ',' << frame->crop_y;
          for (unsigned p = 0; p < frame->plane_count; ++p)
            std::cerr << " plane " << p << " offset " << frame->planes[p].offset << " stride "
                      << frame->planes[p].stride;
          std::cerr << '\n';
        }
        if (!gpu) {
          if (wayland) {
            g::WaylandOptions options{"Headunit synthetic native video"};
            options.defer_scene_fence = defer_scene_fence;
            if (defer_scene_fence) options.max_pending_presentations = 2;
            gpu = std::make_unique<g::GpuCompositor>(frame->metadata.width, frame->metadata.height,
                                                     std::move(options));
          } else
            gpu = std::make_unique<g::GpuCompositor>(frame->metadata.width, frame->metadata.height,
                                                     argv[1]);
        }
        const auto timing = gpu->compose(frame, overlays);
        auto image = gpu->readback();
        const auto& original = found->second;
        check(image.width == original.width && image.height == original.height,
              "GPU output dimensions changed");
        unsigned error = 0;
        // Sample interiors of independently encoded colour blocks; avoid chroma
        // interpolation differences at edges. Overlay crossings are included.
        for (unsigned y = 16; y < image.height; y += 32)
          for (unsigned x = 16; x < image.width; x += 32) {
            const auto pixel = original.pixels[std::size_t(y) * image.width + x];
            std::array<double, 3> expected{double((pixel >> 16) & 255), double((pixel >> 8) & 255),
                                           double(pixel & 255)};
            for (const auto& q : overlays)
              if (x >= q.rect.origin.x && y >= q.rect.origin.y &&
                  x < q.rect.origin.x + q.rect.size.width &&
                  y < q.rect.origin.y + q.rect.size.height) {
                const std::array<double, 3> color{q.color.r, q.color.g, q.color.b};
                for (unsigned c = 0; c < 3; ++c)
                  expected[c] = color[c] * 255 * q.color.a + expected[c] * (1 - q.color.a);
              }
            const auto actual = image.pixels[std::size_t(y) * image.width + x];
            for (unsigned c = 0; c < 3; ++c)
              error = std::max(error, unsigned(std::abs(int((actual >> (16 - 8 * c)) & 255) -
                                                        int(std::lround(expected[c])))));
            check((actual >> 24) == 255, "GPU output alpha changed");
          }
        max_error = std::max(max_error, error);
        total_draw_ns += timing.draw_ns;
        if (wayland) {
          g::Image window_image;
          refresh_fixture_clock();
          const auto receipt = gpu->present(frames + 1, &window_image);
          check(receipt.presented && receipt.token == frames + 1,
                "Wayland fixture frame discarded");
          ++presentations;
          const auto mapped = presentation_clock.accept(receipt, media);
          check(mapped.has_value(), "presented fixture timestamp missing");
          check(window_image.width == image.width && window_image.height == image.height &&
                    window_image.pixels == image.pixels,
                "Wayland window blit changed pixels");
          std::cout << "{\"event\":\"presentation\",\"token\":" << receipt.token
                    << ",\"clockId\":" << receipt.clock_id << ",\"seconds\":\"" << receipt.seconds
                    << "\",\"nanoseconds\":" << receipt.nanoseconds
                    << ",\"flags\":" << receipt.flags << ",\"mediaPTS\":" << mapped->pts_seconds
                    << ",\"uncertaintyNs\":" << mapped->uncertainty_ns
                    << ",\"syntheticMediaClock\":true}\n";
        }
        std::cout << "{\"event\":\"frame\",\"ntp\":\"" << frame->metadata.sender_ntp
                  << "\",\"width\":" << image.width << ",\"height\":" << image.height
                  << ",\"fourcc\":" << frame->fourcc << ",\"modifier\":\"" << frame->modifier
                  << "\",\"planes\":" << frame->plane_count << ",\"maxChannelError\":" << error
                  << ",\"importNs\":" << timing.import_ns << ",\"drawNs\":" << timing.draw_ns
                  << "}\n";
        reference.erase(found);
        leased.pop_front();
        last = std::move(image);
        ++frames;
      }
    };
    std::array<std::uint8_t, 4096> input{};
    while (std::cin.read(reinterpret_cast<char*>(input.data()), input.size()) ||
           std::cin.gcount()) {
      auto bytes = std::span(input).first(std::cin.gcount());
      for (const auto& packet : stream.feed(bytes)) {
        const auto* message = &packet;
        {
          if (message->kind == headunit::AirplayVideoPacket::Kind::config) {
            check(!hardware, "repeated fixture configuration");
            const auto config = v::screen_configuration(message->data);
            v::DecoderOptions options;
            options.codec = config.codec;
            options.codec_data = config.codec_data;
            options.max_width = 1280;
            options.max_height = 720;
            options.decoder = "v4l2slh265dec";
            options.native_buffers = true;
            options.max_native_leases = lease_limit;
            hardware = std::make_unique<v::Decoder>(options);
            options.decoder = "avdec_h265";
            options.native_buffers = false;
            software = std::make_unique<v::Decoder>(options);
          } else if (message->kind == headunit::AirplayVideoPacket::Kind::frame) {
            check(hardware && software, "missing fixture configuration");
            while (!hardware->push(*message)) {
              drain();
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            while (!software->push(*message)) {
              drain();
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            drain();
          }
        }
      }
    }
    check(!stream.partial() && hardware && software, "incomplete fixture stream");
    hardware->finish();
    software->finish();
    do {
      // EOS can arrive before a full lease batch is decoded. Keep accumulating
      // the first batch until its real capacity check runs; flushing each
      // partial batch here made that proof depend on decoder scheduling.
      drain(tested_bound);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!hardware->drained() || !software->drained() || !leased.empty());
    check(reference.empty() && frames > 0 && tested_bound, "incomplete GPU validation");
    check(bool(final_lease), "missing final native sample lease");
    hardware.reset();
    software.reset();
#ifdef HEADUNIT_THEME_HAVE_SKIA
    // Switch native video -> Ganesh -> native video on the same context. The
    // retained decoder buffer must still produce exactly the previous pixels.
    auto scene = std::make_shared<g::Layer>();
    scene->set_frame({{0, 0}, {double(last.width), double(last.height)}});
    scene->background = {.1, .2, .3, 1};
    auto shape = std::make_shared<g::Layer>();
    shape->set_frame({{10.5, 20.5}, {300, 200}});
    shape->background = {1, .2, .4, .7};
    shape->opacity = .5;
    shape->clip = true;
    shape->corner_radius = 15;
    scene->insert(shape, 0);
    gpu->compose_scene(scene);
#endif
    gpu->compose(final_lease, overlays);
    check(gpu->readback().pixels == last.pixels,
          "decoder teardown invalidated a retained native frame");
    // Real decoded sample -> host input -> script-created DPVideoLayer. A
    // compositor receipt, not rendering or a frame callback, advances its PTS.
    script::TrustedDirectory asset{std::filesystem::temp_directory_path()};
    script::Runtime runtime(asset, {"native-test", last.width, last.height});
    auto input_stream = runtime.add_video_input({"fixture", last.width, last.height});
    runtime.evaluate(R"(
    local input=CP.enumerateInputStreams()[1]
    movie=CP.DPVideoLayer.create(input.identifier)
    movie.frame=CP.DPRect(0,0,input.width,input.height);movie:subscribe()
    CP.setRootLayer('native-test',movie)
  )");
    check(input_stream->offer_native(42.5, final_lease), "native UI input rejected fixture");
    auto prepared = input_stream->prepare();
    check(prepared && prepared->native, "native UI frame missing");
    const auto layer = runtime.scene_root();
    check(layer && layer->video, "script video layer missing");
    gpu->compose(layer->video->native(), overlays);
    check(gpu->readback().pixels == last.pixels, "native UI input changed pixels");
#ifdef HEADUNIT_THEME_HAVE_SKIA
    auto scene_input = std::make_shared<g::Layer>(g::Kind::video);
    scene_input->set_frame(layer->frame());
    scene_input->video = layer->video;
    for (const auto& q : overlays) {
      auto child = std::make_shared<g::Layer>();
      child->set_frame(q.rect);
      child->background = q.color;
      scene_input->insert(child, scene_input->children.size());
    }
    const auto pixel_error = [](const g::Image& a, const g::Image& b) {
      check(a.width == b.width && a.height == b.height, "native scene dimensions changed");
      unsigned error = 0;
      for (std::size_t i = 0; i < a.pixels.size(); ++i)
        for (unsigned c = 0; c < 4; ++c)
          error = std::max(error, unsigned(std::abs(int((a.pixels[i] >> (c * 8)) & 255) -
                                                    int((b.pixels[i] >> (c * 8)) & 255))));
      return error;
    };
    gpu->compose_scene(scene_input);
    const auto native_scene = gpu->readback();
    const auto scene_error = pixel_error(native_scene, last);
    native_scene_error = scene_error;
    std::cout << "{\"event\":\"nativeScene\",\"maxChannelError\":" << scene_error
              << ",\"cpuMapped\":false}\n";
    check(scene_error <= 1, "native scene pixels differ from direct DMA-buf composition");
    // Repeated imports must release Ganesh's borrowed image after the fence;
    // retaining an old decoder lease would exhaust the three-sample decoder.
    for (unsigned i = 0; i < 5; ++i)
      gpu->compose_scene(scene_input);
    check(gpu->readback().pixels == native_scene.pixels, "repeated native scene changed pixels");
    gpu->compose(final_lease);
    const auto native_pixels = std::make_shared<g::Image>(gpu->readback());  //Verification only.
    gpu->compose_scene(layer);
    check(pixel_error(gpu->readback(), *native_pixels) <= 1,
          "script DPVideoLayer scene changed native pixels");
    auto composite = std::make_shared<g::Layer>();
    composite->set_frame(layer->frame());
    composite->background = {.12, .08, .2, 1};
    auto transformed = std::make_shared<g::Layer>(g::Kind::video);
    transformed->video = layer->video;
    transformed->set_frame({{37, 29}, {double(last.width) - 74, double(last.height) - 58}});
    transformed->opacity = .6;
    transformed->clip = true;
    transformed->corner_radius = 17;
    composite->insert(transformed, 0);
    auto mask = std::make_shared<g::Layer>();
    mask->set_frame({{9, 7}, {double(last.width) - 100, double(last.height) - 80}});
    mask->background = {1, 1, 1, .8};
    transformed->set_mask(mask);
    gpu->compose_scene(composite);
    const auto clipped = gpu->readback();
    transformed->kind = g::Kind::image;
    transformed->image = native_pixels;
    const auto reference_scene = g::render(composite, last.width, last.height);
    transformed->kind = g::Kind::video;
    transformed->image.reset();
    const auto clipped_error = pixel_error(clipped, reference_scene);
    native_masked_error = clipped_error;
    std::cout << "{\"event\":\"nativeSceneMasked\",\"maxChannelError\":" << clipped_error
              << ",\"referenceReadbackOnly\":true}\n";
    check(clipped_error <= 16, "native scene transform/mask differs from reference");
    auto masked_gradient = std::make_shared<g::Layer>(g::Kind::gradient);
    masked_gradient->set_frame(layer->frame());
    masked_gradient->stops = {{{1, 0, 0, 1}, 0}, {{0, 0, 1, 1}, 1}};
    auto video_mask = std::make_shared<g::Layer>(g::Kind::video);
    video_mask->set_frame(layer->frame());
    video_mask->video = layer->video;
    masked_gradient->set_mask(video_mask);
    const auto before_video_mask = gpu->scene_cache_stats();
    gpu->compose_scene(masked_gradient);
    check(gpu->scene_cache_stats().hits == before_video_mask.hits &&
              gpu->scene_cache_stats().builds == before_video_mask.builds,
          "native video entered retained gradient cache");
    gpu->compose_scene(scene_input);
    native_scene_tested = true;
    unsigned extra_presentations = 0;
    if (defer_scene_fence) {
      // Submit while the borrowed decoded image is still held by the GPU.
      // Its GPU fence guards the borrow; the Stream's accepted PTS is still
      // advanced only through its own actual presentation receipt.
      gpu->compose_scene(scene_input);
      gpu->submit(frames + 1);
      check(gpu->presentation_pending(), "deferred native scene lost its pending presentation");
      std::optional<g::PresentationReceipt> deferred_receipt;
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!deferred_receipt && std::chrono::steady_clock::now() < until) {
        deferred_receipt = gpu->poll_presentation();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(
          deferred_receipt && deferred_receipt->presented && deferred_receipt->token == frames + 1,
          "deferred native scene did not reach the compositor");
      gpu->compose_scene(scene_input);
      check(gpu->readback().pixels == native_scene.pixels,
            "deferred native scene changed pixels or retained a stale image");
      // Record consecutive submissions without waiting between them. Closing the
      // source must leave its alias lease alive until the queued fences retire.
      // A fast GPU may finish the first before the second starts; do not require
      // artificial delay just to force the high-water mark to two.
      auto queued_stream = std::make_shared<v::Stream>(v::Input{"queued", last.width, last.height},
                                                       std::make_shared<v::Memory>());
      check(queued_stream->offer_native(1, final_lease) && queued_stream->prepare().has_value(),
            "queued source fixture missing");
      auto queued_root = std::make_shared<g::Layer>(g::Kind::video);
      queued_root->set_frame({{0, 0}, {double(last.width), double(last.height)}});
      queued_root->video = std::make_shared<v::View>();
      queued_root->video->stream = queued_stream;
      queued_root->video->subscribed = true;
      g::WaylandOptions queued_options{"Headunit queued native lease"};
      queued_options.defer_scene_fence = true;
      queued_options.max_pending_presentations = 2;
      g::GpuCompositor queued(last.width, last.height, std::move(queued_options));
      queued.compose_scene(queued_root);
      const auto expected_queued = queued.readback();
      queued.compose_scene(queued_root);
      queued.compose_scene(queued_root);
      const std::weak_ptr<const v::NativeFrame> borrowed = queued_stream->native();
      queued_stream->close();
      check(!borrowed.expired(), "queued GPU released a closed source before its completion fence");
      check(queued.scene_cache_stats().max_deferred_gpu_frames >= 1 &&
                queued.scene_cache_stats().max_deferred_gpu_frames <= 2,
            "deferred GPU frame queue exceeded its bound");
      check(queued.readback().pixels == expected_queued.pixels && borrowed.expired() &&
                queued.scene_cache_stats().deferred_gpu_frames == 0,
            "queued GPU fence failed to release native aliases or changed pixels");
      ++extra_presentations;
      if (retire_pending) {
        // Exercise owner destruction with actual borrowed HEVC images still
        // queued. readback/presentation would drain the fences before destruction
        // and miss release callbacks held until Ganesh context retirement.
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
          auto retiring_stream = std::make_shared<v::Stream>(
              v::Input{"retiring", last.width, last.height}, std::make_shared<v::Memory>());
          check(retiring_stream->offer_native(1, final_lease) &&
                    retiring_stream->prepare().has_value(),
                "retirement source missing");
          auto retiring_root = std::make_shared<g::Layer>(g::Kind::video);
          retiring_root->set_frame({{0, 0}, {double(last.width), double(last.height)}});
          retiring_root->video = std::make_shared<v::View>();
          retiring_root->video->stream = retiring_stream;
          retiring_root->video->subscribed = true;
          g::WaylandOptions retiring_options{"Headunit pending native retirement"};
          retiring_options.defer_scene_fence = true;
          retiring_options.max_pending_presentations = 2;
          auto retiring = std::make_unique<g::GpuCompositor>(last.width, last.height,
                                                             std::move(retiring_options));
          retiring->compose_scene(retiring_root);
          retiring->compose_scene(retiring_root);
          check(retiring->scene_cache_stats().deferred_gpu_frames >= 1,
                "retirement fixture drained every GPU submission");
          const std::weak_ptr<const v::NativeFrame> alias = retiring_stream->native();
          retiring_stream->close();
          check(!alias.expired(), "pending GPU lost its decoder alias before retirement");
          retiring.reset();
          check(alias.expired(), "retired GPU retained a decoder alias");
          ++native_retirement_cycles;
        }
      }
    }
    auto bad_frame = std::make_shared<v::NativeFrame>(*final_lease);
    bad_frame->planes[0].fd = -1;
    auto bad_stream = std::make_shared<v::Stream>(
        v::Input{"invalid-scene", last.width, last.height}, std::make_shared<v::Memory>());
    check(bad_stream->offer_native(1, bad_frame) && bad_stream->prepare().has_value(),
          "invalid descriptor fixture missing");
    auto bad_layer = std::make_shared<g::Layer>(g::Kind::video);
    bad_layer->set_frame(layer->frame());
    bad_layer->video = std::make_shared<v::View>();
    bad_layer->video->stream = bad_stream;
    bad_layer->video->subscribed = true;
    bool rejected = false;
    try {
      gpu->compose_scene(bad_layer);
    } catch (const std::exception&) {
      rejected = true;
    }
    check(rejected && gpu->readback().pixels == native_scene.pixels,
          "invalid native descriptor changed the framebuffer");
    gpu->compose_scene(scene_input);
    check(gpu->readback().pixels == native_scene.pixels,
          "invalid native descriptor poisoned the compositor");
#endif
    runtime.evaluate(
        "assert(movie:getCurrentPTS()==nil and movie:getNextPTS()==42.5 and CP.PTS==nil)");
    if (wayland) {
      // Reference comparisons and lease-retirement probes deliberately submit
      // no frames. Resume after that observed idle with a fresh real receipt,
      // retaining the clock domain, token and last-presented timeline guards.
      presentation_clock.resume_after_idle();
      refresh_fixture_clock();
      const auto receipt = gpu->present(frames + 1 + extra_presentations);
      check(receipt.presented && receipt.token == frames + 1 + extra_presentations,
            "native UI frame discarded");
      const auto mapped = presentation_clock.accept(receipt, media);
      check(mapped.has_value(), "native UI presentation timestamp missing");
      input_stream->presented(prepared->token);
      runtime.tick(mapped->pts_seconds);
      // Source-frame PTS and presentation PTS are intentionally different in the
      // fixture. Only the latter reaches CP.PTS after clock-domain conversion.
      runtime.evaluate(
          "assert(movie:getCurrentPTS()==42.5 and movie:getNextPTS()==nil and CP.PTS>=900 and "
          "CP.PTS<925)");
      ++presentations;
    } else
      input_stream->cancel();
    // Explicit reference-image upload must preserve alpha, orientation and all
    // pixels. It is not selected automatically on failed DMA-buf import.
    auto uploaded = last;
    uploaded.pixels.front() = 0x80402010;
    uploaded.pixels.back() = 0xff123456;
    gpu->compose_reference(uploaded);
    check(gpu->readback().pixels == uploaded.pixels, "reference image upload changed pixels");
    if (wayland) {
      refresh_fixture_clock();
      g::Image window_image;
      const auto receipt = gpu->present(frames + 2 + extra_presentations, &window_image);
      check(receipt.presented && window_image.pixels == uploaded.pixels,
            "reference Wayland output changed pixels");
      const auto mapped = presentation_clock.accept(receipt, media);
      check(mapped.has_value(), "reference presentation time missing");
      runtime.tick(mapped->pts_seconds);
      ++presentations;
    }
    runtime.evaluate("movie:unsubscribe()");
    check(!layer->video->native(), "unsubscribed native UI frame exposed");
    input_stream->close();
    prepared.reset();
    final_lease.reset();
    if (argc == 3) g::write_png(last, argv[2]);
    check(max_error <= 5, "GPU pixels differ from independent CPU reference");
    std::cout << "{\"event\":\"summary\",\"frames\":" << frames
              << ",\"maxChannelError\":" << max_error << ",\"overlays\":" << overlays.size()
              << ",\"meanDrawNs\":" << (total_draw_ns / frames)
              << ",\"leaseBoundTested\":true,\"nativeLeaseLimit\":" << lease_limit
              << ",\"leaseSurvivesDecoder\":true,\"nativeUiInputTested\":true,\"presentations\":"
              << presentations
              << ",\"nativeSceneTested\":" << (native_scene_tested ? "true" : "false")
              << ",\"nativeRetirementCycles\":" << native_retirement_cycles
              << ",\"nativeSceneMaxError\":" << native_scene_error
              << ",\"nativeSceneMaskedMaxError\":" << native_masked_error
              << ",\"referenceOutputTested\":true,\"presentationClockMapped\":"
              << (wayland ? "true" : "false")
              << ",\"syntheticMediaClock\":" << (wayland ? "true" : "false") << ",\"renderer\":\""
              << gpu->renderer() << "\",\"presented\":" << (wayland ? "true" : "false") << "}"
              << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "gpuProbe: " << e.what() << '\n';
    return 1;
  }
}
