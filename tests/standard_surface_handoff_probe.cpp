// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_control.hpp>
#include <headunit/camera_display.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <headunit/gst_preview_output.hpp>
#include <headunit/h264_annexb.hpp>
#include <headunit/standard_preview.hpp>
#include <gst/app/gstappsrc.h>

#include <algorithm>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Real decoder, controller, worker, GPU fences and wp_presentation receipts.
// Sources are original generated fixtures. Isolated headless compositors do
// not qualify physical scanout, input focus/HID or iPhone interoperability.
namespace c = headunit::camera;
namespace g = headunit::theme::graphics;
using Preview = headunit::StandardPreview;
using Worker = headunit::StandardPreviewWorker;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
std::uint64_t monotonic_ns() {
  return std::uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}
struct Budget {
  const Clock::time_point end = Clock::now() + 48s;
  void check_remaining() const {
    check(Clock::now() < end, "Standard handoff exceeded its total work deadline");
  }
  template <class Step, class Done>
  void until(Step step, Done done, const char* why) const {
    const auto deadline = std::min(end, Clock::now() + 3s);
    for (;;) {
      check_remaining();
      step();
      if (done()) return;
      check(Clock::now() < deadline, why);
      std::this_thread::sleep_for(2ms);
    }
  }
  template <class Step>
  void run(Step step, std::chrono::milliseconds duration) const {
    const auto finish = Clock::now() + duration;
    while (Clock::now() < finish) {
      check_remaining();
      step();
      std::this_thread::sleep_for(2ms);
    }
  }
};

void isolated_sockets(const std::filesystem::path& main, const std::filesystem::path& cluster) {
  check(main.is_absolute() && cluster.is_absolute() && main != cluster &&
            main == main.lexically_normal() && cluster == cluster.lexically_normal() &&
            main.parent_path() == cluster.parent_path(),
        "probe needs two distinct absolute isolated Wayland sockets");
  const auto parent = main.parent_path();
  struct stat directory{};
  check(parent.filename().string().starts_with("headunit-") &&
            ::lstat(parent.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode) &&
            directory.st_uid == ::getuid() && (directory.st_mode & 07777) == 0700,
        "probe refuses a live or nonprivate compositor runtime");
  for (const auto& path : {main, cluster}) {
    struct stat info{};
    check(::lstat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) && info.st_uid == ::getuid(),
          "probe endpoint is not an owned UNIX socket");
  }
}
struct PrivateFiles {
  std::filesystem::path directory, request, lease;
  explicit PrivateFiles(const std::filesystem::path& runtime) {
    // The wrapper owns this complete runtime tree, including SIGKILL cleanup.
    auto pattern = (runtime / "standard-surface-XXXXXX").string();
    pattern.push_back('\0');
    const auto* made = ::mkdtemp(pattern.data());
    check(made, "private handoff directory unavailable");
    directory = made;
    request = directory / "camera.request";
    lease = directory / "main.lease";
    set(true);
  }
  ~PrivateFiles() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  void set(bool active) {
    const auto temporary = directory / "request-next";
    const int fd =
        ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    check(fd >= 0, "private camera request creation failed");
    const std::string_view text = active ? "on\n" : "off\n";
    std::size_t written = 0;
    bool good = ::fchmod(fd, 0600) == 0;
    while (good && written < text.size()) {
      const auto amount = ::write(fd, text.data() + written, text.size() - written);
      if (amount < 0 && errno == EINTR) continue;
      if (amount <= 0) {
        good = false;
        break;
      }
      written += std::size_t(amount);
    }
    const bool closed = ::close(fd) == 0;
    check(good && closed && ::rename(temporary.c_str(), request.c_str()) == 0,
          "private camera request replacement failed");
  }
};

struct Encoded {
  std::vector<std::uint8_t> configuration;
  std::vector<std::vector<std::uint8_t>> frames;
};
Encoded read_fixture(const char* path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  check(bool(file), "generated UHVP fixture unavailable");
  const auto length = file.tellg();
  check(length >= 5 && length <= 2097152, "generated UHVP fixture exceeds its byte bound");
  file.seekg(0);
  std::vector<std::uint8_t> bytes(std::size_t(length), 0);
  check(bool(file.read(reinterpret_cast<char*>(bytes.data()), length)),
        "generated UHVP read incomplete");
  const std::array<std::uint8_t, 5> magic{'U', 'H', 'V', 'P', 1};
  check(std::equal(magic.begin(), magic.end(), bytes.begin()), "invalid generated UHVP signature");
  const auto integer = [&](std::size_t at, unsigned width) {
    check(at <= bytes.size() && width <= bytes.size() - at, "truncated generated UHVP field");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
      value |= std::uint64_t(bytes[at + i]) << (8 * i);
    return value;
  };
  Encoded result;
  for (std::size_t at = 5; at < bytes.size();) {
    const auto kind = integer(at, 1), size = integer(at + 9, 4);
    at += 13;
    check(kind <= 1 && size && at <= bytes.size() && size <= bytes.size() - at,
          "invalid generated UHVP access unit");
    std::vector<std::uint8_t> packet(bytes.begin() + std::ptrdiff_t(at),
                                     bytes.begin() + std::ptrdiff_t(at + size));
    if (kind) {
      check(result.configuration.empty() && result.frames.empty() && packet.size() >= 7 &&
                packet[1] == 66,
            "fixture needs one initial baseline AVC configuration");
      result.configuration = std::move(packet);
    } else {
      check(!result.configuration.empty(), "dependent access unit precedes configuration");
      unsigned idr = 0, delta = 0;
      for (std::size_t nal = 0; nal < packet.size();) {
        check(packet.size() - nal >= 4, "truncated generated NAL length");
        unsigned n = 0;
        for (unsigned i = 0; i < 4; ++i)
          n = (n << 8) | packet[nal + i];
        nal += 4;
        check(n && n <= packet.size() - nal, "invalid generated NAL storage");
        idr += (packet[nal] & 31) == 5;
        delta += (packet[nal] & 31) == 1;
        nal += n;
      }
      check(result.frames.empty() ? idr > 0 : (idr == 0 && delta > 0),
            "fixture does not require retained H.264 references");
      result.frames.push_back(std::move(packet));
    }
    at += size;
  }
  check(result.frames.size() == 18, "fixture needs exactly 18 dependent H.264 frames");
  return result;
}

// Probe data remains alive until the original pipeline synchronously reaches
// NULL. No callback calls the controller, GPU or lease owner.
struct StampTrace {
  GstPad* pad = nullptr;
  gulong probe = 0;
  std::atomic<unsigned> count{0};
  std::atomic<std::uint64_t> pts{GST_CLOCK_TIME_NONE}, dts{GST_CLOCK_TIME_NONE};
  explicit StampTrace(GstElement* element) {
    pad = gst_element_get_static_pad(element, "src");
    check(pad, "original fixture element has no source pad");
    probe = gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
          auto& trace = *static_cast<StampTrace*>(data);
          auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
          trace.pts = GST_BUFFER_PTS(buffer);
          trace.dts = GST_BUFFER_DTS(buffer);
          ++trace.count;
          return GST_PAD_PROBE_OK;
        },
        this, nullptr);
    if (!probe) {
      gst_object_unref(pad);
      pad = nullptr;
      check(false, "cannot trace original H.264 timestamps");
    }
  }
  ~StampTrace() {
    if (probe) gst_pad_remove_probe(pad, probe);
    if (pad) gst_object_unref(pad);
  }
};
struct EncodedBlock {
  GstPad* pad = nullptr;
  gulong probe = 0;
  std::atomic<bool> entered{false};
  std::atomic<std::uint64_t> pts{GST_CLOCK_TIME_NONE}, dts{GST_CLOCK_TIME_NONE};
  explicit EncodedBlock(GstElement* source) {
    pad = gst_element_get_static_pad(source, "src");
    check(pad, "encoded ingress has no source pad");
    probe = gst_pad_add_probe(
        pad, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
        +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
          auto& block = *static_cast<EncodedBlock*>(data);
          auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
          block.pts = GST_BUFFER_PTS(buffer);
          block.dts = GST_BUFFER_DTS(buffer);
          block.entered = true;
          return GST_PAD_PROBE_OK;  // The public blocking probe holds this same original buffer.
        },
        this, nullptr);
    if (!probe) {
      gst_object_unref(pad);
      pad = nullptr;
      check(false, "cannot hold original encoded ingress");
    }
  }
  void release() {
    if (probe) {
      gst_pad_remove_probe(pad, probe);
      probe = 0;
    }
  }
  ~EncodedBlock() {
    release();
    if (pad) gst_object_unref(pad);
  }
};

struct Decoder {
  GstElement *pipeline = nullptr, *source = nullptr, *parser = nullptr, *decoder = nullptr,
             *valve = nullptr;
  std::unique_ptr<headunit::GstPreviewOutput> output;
  std::unique_ptr<Preview> preview;
  std::unique_ptr<StampTrace> source_stamps, parsed_stamps, decoded_stamps;
  headunit::H264AnnexB converter;
  unsigned submitted = 0;
  bool eos_seen = false;
  Decoder(const Encoded& input, const PrivateFiles& files, const std::string& socket,
          std::shared_ptr<const c::ActivationControl> control) {
    try {
      GError* error = nullptr;
      pipeline = gst_parse_launch(
          "appsrc name=source is-live=true do-timestamp=false format=time block=false "
          "max-bytes=2097152 leaky-type=none ! h264parse name=parser ! avdec_h264 name=decoder ! "
          "queue max-size-buffers=3 leaky=downstream ! videoconvert ! "
          "video/x-raw,format=BGRx,interlace-mode=progressive,pixel-aspect-ratio=1/"
          "1,multiview-mode=mono,views=1 ! "
          "valve name=gate drop=true",
          &error);
      const bool parsed = pipeline && !error;
      if (error) g_error_free(error);
      check(parsed, "actual H.264 fixture pipeline unavailable");
      source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
      parser = gst_bin_get_by_name(GST_BIN(pipeline), "parser");
      decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
      valve = gst_bin_get_by_name(GST_BIN(pipeline), "gate");
      check(source && parser && decoder && valve, "actual fixture lost its original elements");
      auto* caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
      check(caps, "actual encoded fixture caps unavailable");
      gst_app_src_set_caps(GST_APP_SRC(source), caps);
      gst_caps_unref(caps);
      converter.set_configuration(input.configuration);
      source_stamps = std::make_unique<StampTrace>(source);
      parsed_stamps = std::make_unique<StampTrace>(parser);
      decoded_stamps = std::make_unique<StampTrace>(decoder);
      output = std::make_unique<headunit::GstPreviewOutput>(pipeline, valve);
      Preview::Options options;
      options.presenter.width = 320;
      options.presenter.height = 240;
      options.presenter.window.display_name = socket;
      options.presenter.window.output_name = "headless";
      options.presenter.window.title = "Headunit original Standard handoff fixture";
      options.presenter.window.app_id = "org.headunit.standard-surface.main";
      options.presenter.surface_lease_file = files.lease.string();
      options.camera_control = std::move(control);
      GstState state = GST_STATE_PLAYING;
      gst_element_get_state(pipeline, &state, nullptr, 0);
      check(state == GST_STATE_NULL, "actual controller was not constructed before PLAYING");
      preview = std::make_unique<Preview>(*output, std::move(options));
      const auto initial = preview->status();
      check(initial.stage == Preview::Stage::hidden && initial.camera_active &&
                output->stats().draining && initial.presenter.window == 0 &&
                !initial.presenter.source_armed && !initial.presenter.touch_ready,
            "camera-on startup created a phone window, source or input qualification");
      check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
            "actual H.264 fixture did not enter PLAYING");
    } catch (...) {
      cleanup();
      throw;
    }
  }
  ~Decoder() { cleanup(); }
  void cleanup() noexcept {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    preview.reset();
    output.reset();
    decoded_stamps.reset();
    parsed_stamps.reset();
    source_stamps.reset();
    for (auto** object : {&valve, &decoder, &parser, &source, &pipeline}) {
      if (*object) gst_object_unref(*object);
      *object = nullptr;
    }
  }
  void errors() {
    auto* bus = gst_element_get_bus(pipeline);
    bool failed = false;
    // Filtered ERROR pops discard unrelated messages, including the actual
    // EOS needed below. Preserve EOS authority explicitly while consuming the
    // bus in bounded turns; a buffer count alone never qualifies completion.
    for (unsigned count = 0; count < 64; ++count) {
      auto* message = gst_bus_pop(bus);
      if (!message) break;
      failed = failed || GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
      eos_seen = eos_seen || GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
      gst_message_unref(message);
    }
    gst_object_unref(bus);
    check(!failed, "actual H.264 fixture reported a bus error");
  }
  void retained_playing() const {
    for (const auto& name : {"source", "parser", "decoder"}) {
      auto* current = gst_bin_get_by_name(GST_BIN(pipeline), name);
      const auto* original = std::string_view(name) == "source"   ? source
                             : std::string_view(name) == "parser" ? parser
                                                                  : decoder;
      const bool same = current == original;
      if (current) gst_object_unref(current);
      check(same, "handoff replaced an original H.264 element");
    }
    for (auto* element : {pipeline, parser, decoder}) {
      GstState state = GST_STATE_NULL;
      gst_element_get_state(element, &state, nullptr, 0);
      check(state == GST_STATE_PLAYING, "handoff stopped original H.264 reference state");
    }
  }
  std::uint64_t send(const Encoded& input, unsigned index) {
    check(index == submitted && index < input.frames.size(),
          "fixture access units reordered or repeated");
    // Genuine ingress is sampled before pump can restore a new branch/floor.
    // Neither PTS nor DTS claims a phone capture/decode/presentation clock.
    const auto ingress = output->arrival_time();
    preview->pump();
    const auto data = converter.access_unit(input.frames[index]);
    check(data.size() <= 2097152 &&
              gst_app_src_get_current_level_bytes(GST_APP_SRC(source)) <= 2097152 - data.size() &&
              gst_app_src_get_current_level_buffers(GST_APP_SRC(source)) < 3,
          "fixture encoded ingress exceeds its bound");
    auto* buffer = gst_buffer_new_allocate(nullptr, data.size(), nullptr);
    check(buffer, "encoded fixture allocation failed");
    gst_buffer_fill(buffer, 0, data.data(), data.size());
    GST_BUFFER_PTS(buffer) = ingress;
    GST_BUFFER_DTS(buffer) = ingress;
    check(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK,
          "actual fixture rejected encoded ingress");
    ++submitted;
    return ingress;
  }
  bool decoded(unsigned count, std::uint64_t ingress) const {
    if (decoded_stamps->count < count) return false;
    check(decoded_stamps->count == count && source_stamps->count == count &&
              parsed_stamps->count == count && source_stamps->pts == ingress &&
              source_stamps->dts == ingress && parsed_stamps->pts == ingress &&
              parsed_stamps->dts == ingress && decoded_stamps->pts == ingress,
          "actual H.264 path changed an original ingress timestamp or decoded count");
    retained_playing();
    return true;
  }
};

// Independent real presenter. Readiness comes from the production worker;
// presented/source correlation comes only from its actual Wayland receipt.
struct Cluster {
  Worker worker;
  bool armed = false;
  Clock::time_point next = Clock::now(), last_progress = Clock::now();
  std::uint64_t previous_presentations = 0, frames = 0;
  explicit Cluster(const std::string& socket)
      : worker([&] {
          Worker::Options options;
          options.width = 320;
          options.height = 240;
          options.window.display_name = socket;
          options.window.output_name = "headless";
          options.window.title = "Headunit independent cluster fixture";
          options.window.app_id = "org.headunit.standard-surface.cluster";
          return options;
        }()) {}
  void turn() {
    auto status = worker.status();
    check(!status.faulted, "independent actual cluster presenter faulted");
    if (!armed && status.window_ready && status.window == 1 && status.requested == 1) {
      check(worker.arm_source(1, {}), "independent real cluster did not arm its actual window");
      armed = true;
    }
    const auto at = Clock::now();
    if (armed && at >= next) {
      auto image = std::make_shared<g::Image>();
      image->width = 320;
      image->height = 240;
      image->pixels.assign(320 * 240, 0xff102030);
      const auto width = unsigned(30 + frames % 250);
      for (unsigned y = 60; y < 100; ++y)
        for (unsigned x = 15; x < width; ++x)
          image->pixels[y * 320 + x] = 0xff44ddbb;
      check(worker.offer(1, {monotonic_ns(), std::move(image)}),
            "independent cluster rejected a strictly new source");
      ++frames;
      next = at + 20ms;
    }
    status = worker.status();
    if (status.presented > previous_presentations) {
      previous_presentations = status.presented;
      last_progress = at;
    }
    check(at - last_progress < 2500ms,
          "independent cluster stopped actual presentation during main handoff");
    if (status.touch_ready)
      check(status.window == 1 && status.presented_arrival_running_ns.has_value(),
            "independent cluster fabricated its receipt correlation");
  }
  std::uint64_t presented() const { return worker.status().presented; }
};

c::Configuration camera_configuration() {
  c::Configuration config;
  config.enabled = true;
  config.manual_activation = true;
  config.source = c::SourceKind::external;
  config.stream_identifier = "original-standard-camera-fixture";
  config.width = 320;
  config.height = 240;
  config.canvas_width = 320;
  config.canvas_height = 240;
  config.destination = {0, 0, 320, 240};
  config.maximum_age_ms = 600;
  return config;
}
}  // namespace

int main(int argc, char** argv) try {
  const Budget budget;
  check(argc == 4,
        "usage: standard-surface-handoff-probe GENERATED_UHVP MAIN_SOCKET CLUSTER_SOCKET");
  isolated_sockets(argv[2], argv[3]);
  const auto encoded = read_fixture(argv[1]);
  gst_init(nullptr, nullptr);
  PrivateFiles files(std::filesystem::path(argv[2]).parent_path());
  auto control = std::make_shared<c::ActivationControl>(files.request.string());
  c::CameraSurfaceLease native_lease(files.lease.string()), observer(files.lease.string());
  const auto camera_config = camera_configuration();
  c::Queue camera_frames(camera_config);
  Cluster cluster(argv[3]);
  check(native_lease.acquire_exclusive(), "camera startup lacks its exclusive main lease");
  Decoder main(encoded, files, argv[2], control);
  std::unique_ptr<c::NativeDisplay> native;
  bool retired = false;
  std::uint64_t native_actual_receipts = 0, fresh_receipts = 0, missing_receipts = 0,
                stale_receipts = 0;
  struct ReturnTiming {
    std::uint64_t generation = 0, release_start = 0, release_end = 0, window_ready = 0;
  };
  std::optional<ReturnTiming> return_timing;
  unsigned timed_returns = 0;
  auto next_capture = Clock::now();

  const auto lease_excludes_shared = [&] {
    check(native_lease.holds(), "native surface lost its exclusive lease");
    const bool overlapped = observer.acquire_shared();
    if (overlapped) observer.release();
    check(!overlapped, "native exclusive ownership overlapped a shared main lease");
  };
  const auto start_native = [&] {
    check(!native && native_lease.acquire_exclusive(),
          "native camera opened before real main withdrawal");
    lease_excludes_shared();
    camera_frames.discard();
    next_capture = Clock::now();
    c::DisplayOptions options;
    options.window.display_name = argv[2];
    options.window.output_name = "headless";
    options.window.app_id = "org.headunit.standard-surface.camera";
    options.gpu_completion_timeout_ns = 2000000000;
    options.receipt = [&](const c::DisplayReceipt& event) {
      lease_excludes_shared();
      if (event.presented) {
        check(event.presentation_monotonic_ns.has_value(),
              "native camera invented local receipt time");
        ++native_actual_receipts;
        if (event.fresh_camera) {
          check(event.source == c::Availability::ready, "native ready receipt has wrong source");
          ++fresh_receipts;
        }
        if (event.source == c::Availability::missing) ++missing_receipts;
        if (event.source == c::Availability::stale) ++stale_receipts;
      }
    };
    native = std::make_unique<c::NativeDisplay>(camera_config, camera_frames, std::move(options));
  };
  const auto stop_native = [&] {
    check(native && native_lease.holds(), "native cleanup lost its exclusive lease");
    native.reset();
    lease_excludes_shared();
    native_lease.release();
  };
  const auto step = [&] {
    budget.check_remaining();
    cluster.turn();
    main.errors();
    if (!retired) main.preview->pump();
    if (native) {
      lease_excludes_shared();
      const auto state = main.preview->status();
      check(!state.presenter.window_ready && state.presenter.window == 0 &&
                !state.presenter.source_armed && !state.presenter.touch_ready,
            "phone window or touch qualified while native camera holds exclusive ownership");
      native->turn();
      check(!native->statistics().faulted && native->statistics().maximum_pending <= 2,
            "native camera presentation ownership exceeded its bound");
    }
    const auto state = main.preview->status();
    check(!state.presenter.faulted, "real main worker faulted during isolated handoff");
    if (state.presenter.touch_ready) {
      const auto floor = main.output->stats().arrival_floor;
      check(!state.camera_active && state.stage == Preview::Stage::visible &&
                state.presenter.window_ready && state.presenter.window == state.window_generation &&
                state.presenter.requested == state.window_generation && floor &&
                state.presenter.presented_arrival_running_ns &&
                *state.presenter.presented_arrival_running_ns > *floor,
            "main input qualified without a current-generation newer-source actual receipt");
    }
  };
  const auto wait = [&](const auto& done, const char* why) {
    budget.until(step, done, why);
  };
  const auto hidden = [&] {
    const auto state = main.preview->status();
    return state.stage == Preview::Stage::hidden &&
           state.presenter.retired >= state.window_generation && state.presenter.window == 0 &&
           !state.presenter.source_armed && !state.presenter.touch_ready &&
           main.output->stats().draining;
  };
  const auto returning = [&] {
    const auto state = main.preview->status();
    const auto floor = main.output->stats().arrival_floor;
    return state.stage == Preview::Stage::visible && state.presenter.window_ready &&
           state.presenter.source_armed && state.presenter.window == state.window_generation &&
           state.presenter.requested == state.window_generation && floor.has_value();
  };
  const auto send = [&](unsigned index, bool visible) {
    const auto before = main.preview->status().presenter.accepted;
    const auto cluster_before = cluster.presented();
    const auto fresh_submit = monotonic_ns();
    const auto stamp = main.send(encoded, index);
    wait([&] { return main.decoded(index + 1, stamp) && cluster.presented() > cluster_before; },
         "original dependent decoder or independent actual cluster did not progress");
    if (visible) {
      wait(
          [&] {
            const auto state = main.preview->status();
            return state.presenter.window == state.window_generation &&
                   state.presenter.touch_ready &&
                   state.presenter.presented_arrival_running_ns == stamp &&
                   state.presenter.accepted == before + 1;
          },
          "fresh decoded main source lacks an actual source-correlated presentation receipt");
      if (return_timing) {
        const auto observed = monotonic_ns();
        const auto timing = *return_timing;
        const auto state = main.preview->status();
        check(state.window_generation == timing.generation &&
                  timing.release_start <= timing.release_end &&
                  timing.release_end <= timing.window_ready &&
                  timing.window_ready <= fresh_submit && fresh_submit <= observed &&
                  timed_returns < 2,
              "Standard return timing lost its real generation or event order");
        std::cout << "standardReturnTiming generation=" << timing.generation
                  << " nativeRetireAndLeaseReleaseNs=" << timing.release_end - timing.release_start
                  << " releaseToWindowReadyNs=" << timing.window_ready - timing.release_end
                  << " freshSubmitToQualifiedReceiptNs=" << observed - fresh_submit
                  << " releaseToQualifiedReceiptNs=" << observed - timing.release_end << '\n';
        return_timing.reset();
        ++timed_returns;
      }
    } else
      check(main.preview->status().presenter.accepted == before,
            "hidden dependent H.264 frame entered the main presenter");
    return stamp;
  };
  const auto produce_camera = [&] {
    if (Clock::now() < next_capture || !camera_frames.can_offer()) return;
    g::Image image{320, 240, std::vector<std::uint32_t>(320 * 240, 0xff3388bb)};
    const auto capture = monotonic_ns();
    check(camera_frames.offer_image(image, capture, capture, true),
          "original generated camera capture rejected");
    next_capture = Clock::now() + 20ms;
  };
  const auto blocked_return = [&](unsigned index) {
    EncodedBlock held(main.source);
    const auto accepted = main.preview->status().presenter.accepted;
    const auto old = main.send(encoded, index);
    wait([&] { return held.entered.load(); }, "original pre-return encoded frame did not block");
    check(held.pts == old && held.dts == old && main.decoded_stamps->count == index,
          "blocked encoded frame lost its original ingress or decoded early");
    files.set(false);
    main.preview->pump();
    const auto generation = main.preview->status().window_generation;
    const auto cluster_before = cluster.presented();
    budget.run(step, 120ms);
    check(main.preview->status().stage == Preview::Stage::returning &&
              main.preview->status().presenter.window == 0 &&
              !main.preview->status().presenter.source_armed &&
              !main.preview->status().presenter.touch_ready &&
              main.preview->status().presenter.accepted == accepted &&
              cluster.presented() > cluster_before,
          "main return bypassed the live native exclusive lease or stopped the cluster");
    const auto cluster_before_return = cluster.presented();
    const auto release_start = monotonic_ns();
    stop_native();
    const auto release_end = monotonic_ns();
    wait([&] { return returning() && cluster.presented() > cluster_before_return; },
         "actual returned main window/source or independent cluster did not progress");
    check(!return_timing, "a Standard return measurement was not retired");
    return_timing = ReturnTiming{generation, release_start, release_end, monotonic_ns()};
    const auto floor = main.output->stats().arrival_floor;
    check(main.preview->status().window_generation == generation && floor && old <= *floor &&
              main.preview->status().presenter.accepted == accepted &&
              !main.preview->status().presenter.touch_ready,
          "actual return fabricated touch or failed to publish its original source floor");
    const auto rejected = main.output->stats().rejected;
    const auto cluster_before_reject = cluster.presented();
    held.release();
    wait(
        [&] {
          return main.decoded(index + 1, old) && main.output->stats().rejected > rejected &&
                 cluster.presented() > cluster_before_reject;
        },
        "pre-floor encoded frame did not actually decode and pass through the rejecting output "
        "gate");
    check(main.preview->status().presenter.accepted == accepted &&
              !main.preview->status().presenter.touch_ready,
          "pre-floor decoded frame qualified a returned source or touch");
    return *floor;
  };

  start_native();
  wait(hidden, "initial camera-on hidden generation did not retire");
  wait([&] { return missing_receipts > 0 && cluster.presented() >= 3; },
       "initial native missing UI or independent cluster did not present");
  for (unsigned index = 0; index < 3; ++index)
    send(index, false);
  main.retained_playing();
  const auto first_floor = blocked_return(3);
  send(4, true);

  files.set(true);
  main.preview->pump();
  wait(hidden, "first actual main withdrawal did not complete");
  start_native();
  send(5, false);
  send(6, false);
  const auto before_fresh = fresh_receipts, cluster_before_fresh = cluster.presented();
  budget.until(
      [&] {
        produce_camera();
        step();
      },
      [&] {
        return fresh_receipts >= before_fresh + 3 && cluster.presented() > cluster_before_fresh;
      },
      "native fresh-source actual camera receipts did not advance");
  const auto before_stale = stale_receipts, cluster_before_stale = cluster.presented();
  wait([&] { return stale_receipts > before_stale && cluster.presented() > cluster_before_stale; },
       "native stale-source status or independent cluster was not actually presented");
  const auto before_missing = missing_receipts, cluster_before_missing = cluster.presented();
  camera_frames.discard();
  wait(
      [&] {
        return missing_receipts > before_missing && cluster.presented() > cluster_before_missing;
      },
      "native missing-source status or independent cluster was not actually presented");
  check(hidden() && control->poll(),
        "camera source expiry restored CarPlay instead of retaining local camera ownership");
  const auto before_recovery = fresh_receipts, cluster_before_recovery = cluster.presented();
  budget.until(
      [&] {
        produce_camera();
        step();
      },
      [&] {
        return fresh_receipts >= before_recovery + 3 &&
               cluster.presented() > cluster_before_recovery;
      },
      "native camera failed actual fresh-source recovery");

  // Real asynchronous ACK, but deliberately no new video/receipt. A camera
  // edge during this unqualified return must retire the new window generation.
  files.set(false);
  main.preview->pump();
  budget.run(step, 80ms);
  const auto cluster_before_abandoned = cluster.presented();
  stop_native();
  wait([&] { return returning() && cluster.presented() > cluster_before_abandoned; },
       "rapid comparison did not obtain an actual returned main window/cluster advance");
  const auto abandoned = main.preview->status().window_generation;
  const auto second_floor = *main.output->stats().arrival_floor;
  check(second_floor > first_floor && !main.preview->status().presenter.touch_ready,
        "unqualified actual window advanced input or renewed an old boundary");
  files.set(true);
  main.preview->pump();
  wait(hidden, "unqualified returned window did not retire on camera edge");
  check(main.preview->status().window_generation > abandoned,
        "rapid camera edge reused an abandoned window generation");
  start_native();
  send(7, false);
  const auto accepted_before_rapid = main.preview->status().presenter.accepted;
  for (unsigned count = 0; count < 2; ++count) {
    const auto before = main.preview->status().window_generation;
    files.set(false);
    main.preview->pump();
    step();
    files.set(true);
    main.preview->pump();
    step();
    check(main.preview->status().window_generation >= before + 2 &&
              !main.preview->status().presenter.touch_ready,
          "rapid requests coalesced away retirement or rearmed old touch");
  }
  wait(hidden, "rapid camera generations did not settle without a phone surface");
  files.set(false);
  main.preview->pump();
  budget.run(step, 80ms);
  const auto cluster_before_rapid_return = cluster.presented();
  stop_native();
  wait([&] { return returning() && cluster.presented() > cluster_before_rapid_return; },
       "main or independent cluster did not recover after rapid camera requests");
  check(*main.output->stats().arrival_floor > second_floor &&
            main.preview->status().presenter.accepted == accepted_before_rapid &&
            !main.preview->status().presenter.touch_ready,
        "rapid return accepted abandoned source/receipt or floor");
  for (unsigned index = 8; index < 13; ++index)
    send(index, true);

  files.set(true);
  main.preview->pump();
  wait(hidden, "repeated main withdrawal did not complete");
  start_native();
  send(13, false);
  const auto repeated_floor = blocked_return(14);
  check(repeated_floor > second_floor,
        "repeated main return failed to strengthen its original arrival floor");
  send(15, true);
  send(16, true);

  // Actual EOS while hidden. The independent cluster remains live while the
  // old decoder generation's attempted return terminally retires its window.
  files.set(true);
  main.preview->pump();
  wait(hidden, "EOS camera withdrawal failed");
  start_native();
  send(17, false);
  check(main.submitted == 18 && main.decoded_stamps->count == 18,
        "handoff lost a dependent H.264 reference");
  check(gst_app_src_end_of_stream(GST_APP_SRC(main.source)) == GST_FLOW_OK,
        "actual hidden decoder EOS rejected");
  wait([&] { return main.eos_seen; }, "hidden actual decoder/drain did not finish EOS");
  const auto cluster_before_fault = cluster.presented();
  files.set(false);
  main.preview->pump();
  budget.run(step, 80ms);
  stop_native();
  budget.until(
      [&] {
        budget.check_remaining();
        cluster.turn();
        main.errors();
        if (!retired) try {
            main.preview->pump();
          } catch (const std::exception& error) {
            const auto state = main.preview->status();
            retired = std::string_view(error.what()) == "Standard decoded source ended" &&
                      state.stage == Preview::Stage::faulted && !state.presenter.faulted;
            if (!retired) throw;
          }
      },
      [&] {
        const auto state = main.preview->status();
        check(!state.presenter.faulted,
              "GPU/window failure cannot qualify expected EOS retirement");
        return retired && state.presenter.window == 0 &&
               state.presenter.retired >= state.window_generation && !state.presenter.touch_ready &&
               !state.presenter.source_armed && cluster.presented() > cluster_before_fault;
      },
      "hidden EOS return kept a phone window/touch or stopped independent cluster receipts");
  check(native_lease.acquire_exclusive(),
        "terminal actual main retirement leaked its shared surface lease");
  lease_excludes_shared();
  native_lease.release();
  main.retained_playing();
  check(camera_frames.statistics().maximum_outstanding_leases <= c::Queue::maximum_leases &&
            native_actual_receipts >= 8 && fresh_receipts >= 6 && missing_receipts >= 2 &&
            stale_receipts >= 1,
        "actual native camera ownership or missing/fresh/stale/recovery coverage incomplete");
  main.preview->retire();
  check(gst_element_set_state(main.pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE,
        "explicit final pipeline NULL failed");
  (void)main.preview->drain_touches([](headunit::TouchInput) { return true; });
  const auto main_stats = main.preview->status().presenter;
  const auto gate_stats = main.output->stats();
  check(timed_returns == 2 && !return_timing, "Standard fresh-return timing coverage incomplete");
  std::cout << "submitted=" << main.submitted << " decoded=" << main.decoded_stamps->count.load()
            << " mainAccepted=" << main_stats.accepted << " mainPresented=" << main_stats.presented
            << " gateRejected=" << gate_stats.rejected
            << " clusterPresented=" << cluster.presented()
            << " nativePresented=" << native_actual_receipts << " cameraFresh=" << fresh_receipts
            << " cameraMissing=" << missing_receipts << " cameraStale=" << stale_receipts << '\n';
  std::cout << "18/18 dependent H.264 frames retained; real Standard GPU source-correlated "
               "returns, native exclusive camera "
               "missing/fresh/stale/recovery, rapid generations, independent actual cluster "
               "receipts and hidden EOS retirement passed. "
               "Isolated headless presentation; no physical scanout/input/phone claim.\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Standard surface handoff: " << error.what() << '\n';
  return 1;
}
