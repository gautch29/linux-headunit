// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Production VideoOutput runs here against an explicit fake GstVideoPreview.
// TouchMailbox, private capture files and routing/fault containment are real.
// No GStreamer, decoder, GPU, Wayland or physical presentation is qualified.
#include <headunit/video_output.hpp>
#include <headunit/gst_video_preview.hpp>
#include <headunit/gst_preview_output.hpp>
#include <headunit/touch_mailbox.hpp>
#include <headunit/camera_control.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
using headunit::AirplayVideoPacket;
using headunit::GstVideoPreview;
using headunit::TouchInput;
using headunit::VideoOutput;
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
bool same(TouchInput a, TouchInput b) {
  return a.pressed == b.pressed && a.x == b.x && a.y == b.y;
}

struct FakePreview {
  std::string sink, decoder, wayland;
  unsigned width = 0, height = 0, submissions = 0, pumps = 0, retires = 0, destructions = 0;
  bool fail_submit = false, fail_pump = false, retired = false;
  headunit::MainPreviewOptions main;
  headunit::TouchMailbox mailbox;
  headunit::TouchMailbox::Generation generation = mailbox.begin();
  std::shared_ptr<headunit::RouteGuidance> navigation;
  std::vector<AirplayVideoPacket> packets;
  void push(TouchInput touch) {
    const std::array reports{touch};
    mailbox.push(generation, reports);
  }
  void pump() {
    ++pumps;
    if (fail_pump && !retired) {
      fail_pump = false;
      throw std::runtime_error("explicit fake optional pump failure");
    }
  }
};
struct PreviewFixture {
  std::map<const GstVideoPreview*, std::shared_ptr<FakePreview>> live;
  std::vector<std::shared_ptr<FakePreview>> created;
  std::string fail_constructor_sink;
  unsigned constructor_failures = 0;
  std::shared_ptr<FakePreview> find(const std::string& sink) const {
    for (const auto& state : created)
      if (state->sink == sink) return state;
    throw std::runtime_error("expected preview was not constructed");
  }
};
PreviewFixture* current = nullptr;
struct PreviewScope {
  PreviewFixture fixture;
  PreviewScope() {
    check(!current, "overlapping fake preview fixtures");
    current = &fixture;
  }
  ~PreviewScope() {
    check(fixture.live.empty(), "preview survived its output owner");
    current = nullptr;
  }
};
FakePreview& state(const GstVideoPreview* preview) {
  check(current != nullptr, "preview called outside its explicit fake fixture");
  const auto found = current->live.find(preview);
  check(found != current->live.end(), "unknown fake preview instance");
  return *found->second;
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    char name[] = "/tmp/headunit-standard-output-XXXXXX";
    check(::mkdtemp(name) != nullptr, "temporary directory failed");
    path = name;
    check(::chmod(path.c_str(), 0700) == 0, "temporary directory permissions failed");
  }
  ~Directory() { std::filesystem::remove_all(path); }
};
VideoOutput::Options options(const Directory& directory, const std::string& sink = "wayland-gpu",
                             bool cluster = false) {
  VideoOutput::Options value;
  value.capture_directory = directory.path.string();
  value.connection_number = 17;
  value.preview_sink = sink;
  value.preview_decoder = "avdec_h264";
  value.main_display.width = 1280;
  value.main_display.height = 720;
  value.cluster_display.width = 1920;
  value.cluster_display.height = 720;
  if (cluster) value.standard_cluster_wayland = "cluster-wayland-0";
  value.main_preview.wayland_display = "main-wayland-0";
  value.main_preview.output_name = "HDMI-A-1";
  if (sink == "wayland-gpu") {
    value.main_preview.camera_control = std::make_shared<const headunit::camera::ActivationControl>(
        (directory.path / "camera.request").string());
    value.main_preview.camera_lease_file = (directory.path / "main.lease").string();
  }
  return value;
}
AirplayVideoPacket configuration(std::uint8_t tag = 1) {
  return {AirplayVideoPacket::Kind::config, 100, {tag, std::uint8_t(tag + 1)}};
}
AirplayVideoPacket frame(std::uint64_t stamp, std::uint8_t tag) {
  return {
      AirplayVideoPacket::Kind::frame, stamp, {tag, std::uint8_t(tag + 1), std::uint8_t(tag + 2)}};
}
std::vector<AirplayVideoPacket> capture(const Directory& directory, unsigned stream) {
  std::ifstream file(directory.path / ("video-17-" + std::to_string(stream) + ".packets"),
                     std::ios::binary);
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                        std::istreambuf_iterator<char>()};
  constexpr std::array<std::uint8_t, 5> magic{'U', 'H', 'V', 'P', 1};
  check(bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin()),
        "private capture magic missing");
  std::vector<AirplayVideoPacket> result;
  for (std::size_t offset = magic.size(); offset < bytes.size();) {
    check(bytes.size() - offset >= 13 && bytes[offset] <= 1, "partial or invalid capture header");
    AirplayVideoPacket packet{
        bytes[offset] ? AirplayVideoPacket::Kind::config : AirplayVideoPacket::Kind::frame, 0, {}};
    for (unsigned i = 0; i < 8; ++i)
      packet.timestamp |= std::uint64_t(bytes[offset + 1 + i]) << (8 * i);
    std::uint32_t count = 0;
    for (unsigned i = 0; i < 4; ++i)
      count |= std::uint32_t(bytes[offset + 9 + i]) << (8 * i);
    offset += 13;
    check(count <= bytes.size() - offset, "partial capture payload");
    packet.data.assign(bytes.begin() + std::ptrdiff_t(offset),
                       bytes.begin() + std::ptrdiff_t(offset + count));
    offset += count;
    result.push_back(std::move(packet));
  }
  return result;
}
void check_capture(const Directory& directory, unsigned stream,
                   const std::vector<AirplayVideoPacket>& expected) {
  const auto actual = capture(directory, stream);
  check(actual.size() == expected.size(), "optional display fault stopped or duplicated capture");
  for (std::size_t i = 0; i < expected.size(); ++i)
    check(actual[i].kind == expected[i].kind && actual[i].timestamp == expected[i].timestamp &&
              actual[i].data == expected[i].data,
          "optional display fault changed captured packet identity");
}
int input_descriptor(const VideoOutput& output) {
  std::vector<pollfd> descriptors;
  output.append_input_poll_descriptor(descriptors);
  check(descriptors.size() == 1 && descriptors[0].events == POLLIN && descriptors[0].fd >= 0,
        "owned main preview did not retain exactly its own poll descriptor");
  check(::fcntl(descriptors[0].fd, F_GETFD) >= 0, "main input descriptor is closed");
  return descriptors[0].fd;
}
}  // namespace

namespace headunit {
// Fake previews never instantiate their private decoder output. These empty
// destructor definitions satisfy that null unique_ptr member without linking
// a second production GStreamer owner into this portable integration test.
struct GstPreviewOutput::Impl {};
GstPreviewOutput::~GstPreviewOutput() = default;

GstVideoPreview::GstVideoPreview(const std::string& sink, unsigned width, unsigned height,
                                 const std::string& decoder, const std::string& wayland,
                                 MainPreviewOptions main)
    : width_(width), height_(height) {
  check(current, "preview created outside its explicit fake fixture");
  if (current->fail_constructor_sink == sink) {
    current->fail_constructor_sink.clear();
    ++current->constructor_failures;
    throw std::runtime_error("explicit fake optional constructor failure");
  }
  auto selected = std::make_shared<FakePreview>();
  selected->sink = sink;
  selected->width = width;
  selected->height = height;
  selected->decoder = decoder;
  selected->wayland = wayland;
  selected->main = std::move(main);
  current->created.push_back(selected);
  current->live.emplace(this, std::move(selected));
}
GstVideoPreview::~GstVideoPreview() {
  ++state(this).destructions;
  current->live.erase(this);
}
void GstVideoPreview::submit(const AirplayVideoPacket& packet) {
  auto& selected = state(this);
  ++selected.submissions;
  check(!selected.retired, "production submitted video to a retired optional main");
  if (selected.fail_submit) {
    selected.fail_submit = false;
    throw std::runtime_error("explicit fake optional submit failure");
  }
  selected.packets.push_back(packet);
}
std::size_t GstVideoPreview::finish(unsigned) {
  return state(this).packets.size();
}
int GstVideoPreview::input_fd() const noexcept {
  return state(this).mailbox.wake_fd();
}
void GstVideoPreview::set_navigation(std::shared_ptr<RouteGuidance> source) {
  state(this).navigation = std::move(source);
}
std::vector<TouchInput> GstVideoPreview::take_touches() {
  auto& selected = state(this);
  selected.pump();
  std::vector<TouchInput> reports;
  selected.mailbox.drain([&](TouchInput report) {
    reports.push_back(report);
    return true;
  });
  return reports;
}
unsigned GstVideoPreview::drain_touches(const std::function<bool(TouchInput)>& sender) {
  auto& selected = state(this);
  selected.pump();
  return selected.mailbox.drain(sender);
}
void GstVideoPreview::retire() noexcept {
  auto& selected = state(this);
  ++selected.retires;
  selected.retired = true;
  selected.mailbox.retire();
}
MainPresentationStatus GstVideoPreview::main_presentation_status() {
  return {};
}
}  // namespace headunit

namespace {
void headless() {
  Directory directory;
  PreviewScope previews;
  VideoOutput output(options(directory, "none"));
  check(output.drain_touches([](TouchInput) {
    throw std::runtime_error("headless output invented input");
    return true;
  }) == 0,
        "headless output forwarded touch");
  std::vector<pollfd> descriptors;
  output.append_input_poll_descriptor(descriptors);
  check(descriptors.empty() && previews.fixture.created.empty(),
        "headless output constructed a display");
  const auto config = configuration(), data = frame(110, 3);
  output.record(110, config, std::nullopt);
  output.record(110, data, std::nullopt);
  check_capture(directory, 110, {config, data});
}
void main_only_routing() {
  Directory directory;
  PreviewScope previews;
  auto selected = options(directory);
  const auto control = selected.main_preview.camera_control;
  VideoOutput output(std::move(selected));
  check(previews.fixture.created.empty(),
        "Standard GPU presenter was created before main configuration");
  const auto main_config = configuration(), cluster_config = configuration(7),
             cluster_frame = frame(130, 11);
  output.record(111, cluster_config, std::nullopt);
  output.record(111, cluster_frame, std::nullopt);
  check(previews.fixture.created.empty(), "navigation video created another main GPU window");
  output.record(110, main_config, std::nullopt);
  check(previews.fixture.created.size() == 1,
        "main configuration did not create exactly one owned presenter");
  const auto main = previews.fixture.find("wayland-gpu");
  check(main->width == 1280 && main->height == 720 && main->decoder == "avdec_h264" &&
            main->wayland == "main-wayland-0",
        "main dimensions, decoder or Wayland route changed");
  check(main->main.wayland_display == "main-wayland-0" && main->main.output_name == "HDMI-A-1" &&
            main->main.camera_control == control &&
            main->main.camera_lease_file == (directory.path / "main.lease").string(),
        "immutable main camera/output options were lost");
  check(input_descriptor(output) == main->mailbox.wake_fd(),
        "main borrowed an unrelated input descriptor");
  output.record(112, cluster_frame, std::nullopt);
  output.record(110, {AirplayVideoPacket::Kind::heartbeat, 140, {99}}, std::nullopt);
  check_capture(directory, 110, {main_config});
  check_capture(directory, 111, {cluster_config, cluster_frame});
  check(!std::filesystem::exists(directory.path / "video-17-112.packets"),
        "unrelated video stream was captured");
}
void explicit_cluster_routing() {
  Directory directory;
  PreviewScope previews;
  VideoOutput output(options(directory, "wayland-gpu", true));
  check(previews.fixture.created.size() == 1,
        "explicit native cluster was not constructed independently");
  const auto cluster = previews.fixture.find("wayland-cluster");
  check(cluster->width == 1920 && cluster->height == 720 &&
            cluster->wayland == "cluster-wayland-0" && cluster->decoder == "avdec_h264",
        "native Standard cluster routing or dimensions changed");
  check(cluster->main.wayland_display.empty() && cluster->main.output_name.empty() &&
            !cluster->main.camera_control && cluster->main.camera_lease_file.empty(),
        "cluster inherited main camera ownership");
  output.record(110, configuration(), std::nullopt);
  output.record(111, configuration(5), std::nullopt);
  output.record(111, frame(120, 6), std::nullopt);
  const auto main = previews.fixture.find("wayland-gpu");
  cluster->push({true, 1900, 700});
  main->push({true, 400, 300});
  std::vector<TouchInput> sent;
  check(output.drain_touches([&](TouchInput report) {
    sent.push_back(report);
    return true;
  }) == 1,
        "main input or cluster pumping failed");
  check(sent.size() == 1 && same(sent[0], {true, 400, 300}) && cluster->pumps == 1,
        "cluster input leaked into the phone's main HID route");
  check(cluster->submissions == 2 && previews.fixture.created.size() == 2,
        "navigation video escaped its explicit cluster window");
}
void main_fault(bool submit_fault) {
  Directory directory;
  PreviewScope previews;
  VideoOutput output(options(directory, "wayland-gpu", true));
  const auto config = configuration(), initial = frame(110, 5), faulty = frame(120, 8),
             later = frame(130, 11);
  output.record(110, config, std::nullopt);
  output.record(110, initial, std::nullopt);
  output.record(111, config, std::nullopt);
  const auto main = previews.fixture.find("wayland-gpu"),
             cluster = previews.fixture.find("wayland-cluster");
  const int descriptor = input_descriptor(output);
  const TouchInput press{true, 913, 287};
  main->push(press);
  std::vector<TouchInput> sent;
  auto accept = [&](TouchInput report) {
    sent.push_back(report);
    return true;
  };
  check(output.drain_touches(accept) == 1 && same(sent.back(), press),
        "initial held phone touch was not accepted");
  main->push({true, 920, 295});  // Undelivered motion must be replaced by cancellation.
  if (submit_fault) {
    main->fail_submit = true;
    output.record(110, faulty, std::nullopt);
  } else {
    main->fail_pump = true;
    check(output.drain_touches(accept) == 0, "failed optional pump forwarded stale input");
  }
  check(main->retired && main->retires == 1 && main->destructions == 0,
        "optional main failure discarded its retained contact cancellation");
  check(input_descriptor(output) == descriptor,
        "retired main lost its borrowed input poll descriptor");
  const unsigned submissions = main->submissions;
  output.record(110, later, std::nullopt);
  output.record(110, configuration(20), std::nullopt);
  output.record(111, later, std::nullopt);
  main->push({true, 100, 100});  // Late callbacks from retired generation cannot revive input.
  unsigned rejected = 0;
  for (unsigned retry = 0; retry < 3; ++retry)
    check(output.drain_touches([&](TouchInput report) {
      check(same(report, {false, press.x, press.y}),
            "cancellation identity was replaced by queued/stale motion");
      ++rejected;
      return false;
    }) == 0,
          "backpressured cancellation was counted as accepted");
  check(rejected == 3 && sent.size() == 1,
        "backpressure did not retain the same cancellation across turns");
  check(output.drain_touches(accept) == 1 && sent.size() == 2 &&
            same(sent.back(), {false, press.x, press.y}),
        "held contact release was lost after optional main failure");
  check(output.drain_touches(accept) == 0, "contact cancellation was duplicated");
  check(main->submissions == submissions && previews.fixture.created.size() == 2,
        "retired optional main was recreated or accepted more video");
  check(!cluster->retired && cluster->destructions == 0 && cluster->submissions == 2 &&
            cluster->pumps >= 6,
        "optional main failure stopped healthy native cluster ownership");
  check_capture(
      directory, 110,
      submit_fault
          ? std::vector<AirplayVideoPacket>{config, initial, faulty, later, configuration(20)}
          : std::vector<AirplayVideoPacket>{config, initial, later, configuration(20)});
  check_capture(directory, 111, {config, later});
}
void sender_failure_is_not_optional_output_failure() {
  Directory directory;
  PreviewScope previews;
  VideoOutput output(options(directory));
  output.record(110, configuration(), std::nullopt);
  const auto main = previews.fixture.find("wayland-gpu");
  main->push({true, 100, 200});
  bool thrown = false;
  try {
    output.drain_touches(
        [](TouchInput) -> bool { throw std::logic_error("explicit sender failure"); });
  } catch (const std::logic_error&) {
    thrown = true;
  }
  check(thrown && !main->retired && main->retires == 0,
        "phone sender failure was mislabeled as an optional display failure");
  std::vector<TouchInput> sent;
  check(output.drain_touches([&](TouchInput report) {
    sent.push_back(report);
    return true;
  }) == 1 && same(sent[0], {true, 100, 200}),
        "throwing sender lost the retained contact");
}
void cluster_fault_is_contained() {
  Directory directory;
  PreviewScope previews;
  VideoOutput output(options(directory, "wayland-gpu", true));
  const auto config = configuration(), data = frame(120, 5);
  output.record(110, config, std::nullopt);
  output.record(111, config, std::nullopt);
  const auto main = previews.fixture.find("wayland-gpu"),
             cluster = previews.fixture.find("wayland-cluster");
  const int descriptor = input_descriptor(output);
  cluster->fail_pump = true;
  main->push({true, 700, 400});
  std::vector<TouchInput> sent;
  check(output.drain_touches([&](TouchInput report) {
    sent.push_back(report);
    return true;
  }) == 1 && same(sent[0], {true, 700, 400}),
        "optional cluster fault stopped healthy main input");
  check(cluster->destructions == 1 && !main->retired && input_descriptor(output) == descriptor,
        "cluster fault changed main surface/input ownership");
  output.record(110, data, std::nullopt);
  output.record(111, data, std::nullopt);
  check(previews.fixture.created.size() == 2 && main->submissions == 2 && cluster->submissions == 1,
        "cluster failure was resurrected or stopped main video");
  check_capture(directory, 110, {config, data});
  check_capture(directory, 111, {config, data});
}
void legacy_release_survives_preview_destruction() {
  Directory directory;
  PreviewScope previews;
  VideoOutput output(options(directory, "wayland-touch", true));
  const auto config = configuration(), faulty = frame(120, 5), later = frame(130, 8);
  output.record(110, config, std::nullopt);
  output.record(111, config, std::nullopt);
  const auto main = previews.fixture.find("wayland-touch"),
             cluster = previews.fixture.find("wayland-cluster");
  std::vector<pollfd> descriptors;
  output.append_input_poll_descriptor(descriptors);
  check(descriptors.size() == 2 && descriptors[0].fd == main->mailbox.wake_fd() &&
            descriptors[1].fd != descriptors[0].fd && descriptors[1].events == POLLIN,
        "legacy input did not expose its separate retaining mailbox wakeup");
  const int retained_descriptor = descriptors[1].fd;
  const TouchInput press{true, 431, 213};
  main->push(press);
  std::vector<TouchInput> sent;
  auto accept = [&](TouchInput report) {
    sent.push_back(report);
    return true;
  };
  check(output.drain_touches(accept) == 1 && same(sent.back(), press),
        "legacy DOWN did not reach the phone sender");
  main->push({false, press.x, press.y});
  check(output.drain_touches([&](TouchInput report) {
    check(same(report, {false, press.x, press.y}),
          "legacy release changed coordinates before fault");
    return false;
  }) == 0,
        "legacy false sender was counted as accepting release");
  main->fail_submit = true;
  output.record(110, faulty, std::nullopt);
  check(main->destructions == 1,
        "optional legacy fault retained the destroyed preview instead of its independent mailbox");
  descriptors.clear();
  output.append_input_poll_descriptor(descriptors);
  check(descriptors.size() == 1 && descriptors[0].fd == retained_descriptor &&
            ::fcntl(retained_descriptor, F_GETFD) >= 0,
        "preview destruction discarded legacy cancellation wakeup");
  check(::poll(descriptors.data(), descriptors.size(), 0) == 1 && (descriptors[0].revents & POLLIN),
        "legacy fault did not wake delivery of retained cancellation");
  unsigned rejected = 0;
  for (unsigned retry = 0; retry < 3; ++retry)
    check(output.drain_touches([&](TouchInput report) {
      check(same(report, {false, press.x, press.y}),
            "legacy fault resurrected or changed held contact");
      ++rejected;
      return false;
    }) == 0,
          "legacy backpressured release was counted as sent");
  output.record(110, later, std::nullopt);
  output.record(111, later, std::nullopt);
  main->push({true, 1, 2});  // An obsolete producer has no route to the retained mailbox.
  check(rejected == 3 && output.drain_touches(accept) == 1 && sent.size() == 2 &&
            same(sent.back(), {false, press.x, press.y}),
        "legacy contact release was lost after preview destruction");
  check(output.drain_touches(accept) == 0 && sent.size() == 2,
        "legacy release was duplicated or old producer revived");
  check(previews.fixture.created.size() == 2 && !cluster->retired && cluster->destructions == 0 &&
            cluster->submissions == 2,
        "legacy main fault recreated its preview or stopped healthy native cluster");
  check_capture(directory, 110, {config, faulty, later});
  check_capture(directory, 111, {config, later});
}
void cluster_constructor_failure_is_contained() {
  Directory directory;
  PreviewScope previews;
  previews.fixture.fail_constructor_sink = "wayland-cluster";
  VideoOutput output(options(directory, "wayland-gpu", true));
  check(previews.fixture.constructor_failures == 1 && previews.fixture.created.empty(),
        "explicit failing cluster fixture did not exercise constructor containment");
  const auto config = configuration(), data = frame(120, 7);
  output.record(110, config, std::nullopt);
  output.record(111, config, std::nullopt);
  output.record(110, data, std::nullopt);
  output.record(111, data, std::nullopt);
  const auto main = previews.fixture.find("wayland-gpu");
  main->push({true, 310, 480});
  std::vector<TouchInput> sent;
  check(output.drain_touches([&](TouchInput report) {
    sent.push_back(report);
    return true;
  }) == 1 && same(sent[0], {true, 310, 480}) &&
            input_descriptor(output) == main->mailbox.wake_fd(),
        "optional cluster construction failure stopped main input ownership");
  check(previews.fixture.created.size() == 1 && previews.fixture.constructor_failures == 1 &&
            main->submissions == 2 && !main->retired,
        "failed cluster was retried or routed into the main GPU window");
  check_capture(directory, 110, {config, data});
  check_capture(directory, 111, {config, data});
}
void phone_selected_mode() {
  {
    Directory directory;
    PreviewScope previews;
    auto selected = options(directory, "wayland-gpu", true);
    selected.defer_preview = true;
    VideoOutput output(std::move(selected));
    const auto config = configuration(), data = frame(200, 8);
    output.record(110, config, std::nullopt);
    output.record(111, config, std::nullopt);
    output.select_mode(std::nullopt);
    check(previews.fixture.created.empty() && !output.main_presentation_status().qualified,
          "unknown mode started or qualified a Standard owner");
    output.select_mode(headunit::CarplayMode::ultra);
    output.record(110, data, std::nullopt);
    output.record(111, data, std::nullopt);
    check(previews.fixture.created.empty(), "Ultra selection started competing Standard previews");
    check_capture(directory, 110, {config, data});
    check_capture(directory, 111, {config, data});
  }
  {
    Directory directory;
    PreviewScope previews;
    auto selected = options(directory, "wayland-gpu", true);
    selected.defer_preview = true;
    VideoOutput output(std::move(selected));
    output.select_mode(headunit::CarplayMode::standard);
    check(previews.fixture.created.size() == 1,
          "selected Standard did not create exactly its cluster");
    output.select_mode(headunit::CarplayMode::standard);
    output.record(110, configuration(), std::nullopt);
    check(previews.fixture.created.size() == 2,
          "selected Standard main did not wait for actual configuration");
    bool rejected = false;
    try {
      output.select_mode(headunit::CarplayMode::ultra);
    } catch (const std::logic_error&) {
      rejected = true;
    }
    check(rejected && previews.fixture.created.size() == 2,
          "mode switch admitted competing owners");
  }
}
}  // namespace

int main() try {
  headless();
  main_only_routing();
  explicit_cluster_routing();
  main_fault(true);
  main_fault(false);
  sender_failure_is_not_optional_output_failure();
  cluster_fault_is_contained();
  legacy_release_survives_preview_destruction();
  cluster_constructor_failure_is_contained();
  phone_selected_mode();
  std::cout << "Standard VideoOutput routing, optional-fault containment, captures and retained "
               "HID release passed (explicit fake preview)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
