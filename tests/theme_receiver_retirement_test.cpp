// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Actual receiver retirement and render-owner orchestration. The compositor,
// native backing, source clock and receipt flags are explicit software fixtures.
// No InstalledUi, signed assets, GPU, phone or physical display runs here.
#include "../src/control_receiver.hpp"
#include <headunit/binary_plist.hpp>
#include <headunit/theme/camera_input.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <plist/plist.h>
#include <thread>

namespace c = headunit::control;
namespace s = headunit::theme::script;
namespace v = headunit::theme::video;
namespace g = headunit::theme::graphics;
namespace vehicle = headunit::theme::vehicle;
using namespace headunit;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
std::uint64_t ns() {
  return presentation_monotonic_ns();
}
template <class F>
void until(F condition) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!condition()) {
    check(std::chrono::steady_clock::now() < deadline, "retirement fixture deadline");
    std::this_thread::sleep_for(1ms);
  }
}
struct Gate {
  std::mutex mutex;
  std::condition_variable wake;
  bool entered = false, released = false;
  std::atomic<bool> expired = false;
  void wait() {
    std::unique_lock lock(mutex);
    entered = true;
    wake.notify_all();
    if (!wake.wait_for(lock, 3s, [&] { return released; })) expired = true;
  }
  bool waiting() {
    std::lock_guard lock(mutex);
    return entered && !released;
  }
  void release() noexcept {
    std::lock_guard lock(mutex);
    released = true;
    wake.notify_all();
  }
};
struct Probe {
  Gate callback, cleanup;
  v::ReceiverVideo::Queues queues;
  std::array<std::shared_ptr<v::NativeInput>, 2> inputs;
  std::array<std::shared_ptr<v::Stream>, 2> streams;
  std::array<std::uint64_t, 2> presented{}, discarded{};
  std::array<std::shared_ptr<const v::NativeFrame>, 2> held;
  std::array<std::array<std::weak_ptr<const void>, 4>, 2> backing;
  std::shared_ptr<MainPresentationSnapshot> presentation;
  std::uint64_t presentation_generation = 0;
  bool offer_waiting = false;
  std::atomic<bool> finished = false;
};
// All stream/input access stays on the real UiWorker, until finished is observed.
thread_local Probe* render_probe = nullptr;
struct Release {
  Probe& probe;
  ~Release() {
    probe.callback.release();
    probe.cleanup.release();
  }
};
struct Submitted {
  std::uint64_t token;
  g::WindowContent content;
};
}  // namespace
namespace headunit::theme::graphics {
struct GpuCompositor::Impl {
  Probe* probe = render_probe;
  std::deque<Submitted> pending;
  std::vector<std::shared_ptr<const video::NativeFrame>> imports;
  unsigned capacity = 1;
  bool composed = false;
};
GpuCompositor::GpuCompositor(std::uint32_t, std::uint32_t, WaylandOptions options)
    : impl_(std::make_unique<Impl>()) {
  check(impl_->probe, "fake output has no render owner");
  impl_->capacity = options.max_pending_presentations;
}
GpuCompositor::~GpuCompositor() {
  // Model uncertain GPU cleanup without releasing the original imports first.
  impl_->probe->cleanup.wait();
}
GpuTimings GpuCompositor::compose_conical(std::span<const GpuConicalGradient>, Color) {
  impl_->composed = true;
  return {};
}
GpuTimings GpuCompositor::compose_scene(const std::shared_ptr<const Layer>& root, std::uint64_t,
                                        const std::function<void()>& during) {
  check(bool(root), "fake composition without scene");
  if (during) during();
  for (const auto& stream : impl_->probe->streams)
    if (const auto frame = stream->native()) impl_->imports.push_back(frame);
  // Two submissions plus a current source; this is a bounded fake import set.
  check(impl_->imports.size() <= 6, "fake import bound exceeded");
  impl_->composed = true;
  return {};
}
GpuSceneCacheStats GpuCompositor::scene_cache_stats() const {
  return {};
}
void GpuCompositor::submit(std::uint64_t token, Image*, WindowContent content) {
  check(impl_->composed && presentation_slot_available(), "invalid fake submission");
  impl_->pending.push_back({token, content});
  impl_->composed = false;
}
bool GpuCompositor::presentation_pending() const {
  return !impl_->pending.empty();
}
bool GpuCompositor::presentation_slot_available() const {
  return impl_->pending.size() < impl_->capacity;
}
void GpuCompositor::set_presentation_queue_depth(unsigned depth) {
  impl_->capacity = depth;
}
bool GpuCompositor::preview_feedback_delayed() const {
  return false;
}
std::vector<TouchInput> GpuCompositor::take_touches(bool) {
  return {};
}
std::optional<PresentationReceipt> GpuCompositor::poll_presentation() {
  auto& p = *impl_->probe;
  if (impl_->pending.empty()) return {};
  if (p.inputs[0]->stats().presented == 1) {
    if (p.inputs[0]->stats().max_staged != 2 || impl_->pending.size() != 2) return {};
    // This callback resumes only AFTER actual UltraRuntime retirement closes
    // both ingress queues and withdraws the copied clock. GpuDisplay then runs
    // its real PendingScene completion path with this deliberately late flag.
    for (unsigned i = 0; i < 2; ++i)
      p.held[i] = p.streams[i]->native();
    p.callback.wait();
  }
  const auto item = impl_->pending.front();
  impl_->pending.pop_front();
  const auto at = ns();
  PresentationReceipt receipt;
  receipt.token = item.token;
  receipt.presented = true;
  receipt.clock_id = 1;
  receipt.seconds = at / 1000000000;
  receipt.nanoseconds = at % 1000000000;
  receipt.received_ns = at;
  receipt.refresh_ns = 16666667;
  receipt.correlation = video::ClockCorrelation{1, at, at, at};
  return receipt;
}
}  // namespace headunit::theme::graphics
namespace {
struct Peer {
  int fd = -1;
  std::uint64_t id = 0;
  ~Peer() {
    if (fd >= 0) ::close(fd);
  }
};
void initialize(AirplaySessionSetup& session) {
  session.set_secret(std::array<std::uint8_t, 32>{});
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  address.sin6_port = htons(12345);
  session.set_peer(address);
  (void)session.respond(encode_binary_plist(
      PlistValue{PlistValue::Dictionary{{"timingPort", PlistValue{std::int64_t{12345}}}}}));
}
void open_ui(AirplaySessionSetup& session, Peer& peer, std::int64_t seed) {
  using Dict = PlistValue::Dictionary;
  using Array = PlistValue::Array;
  const auto bytes = session.respond(encode_binary_plist(PlistValue{Dict{
      {"streams", PlistValue{Array{PlistValue{Dict{
                      {"type", PlistValue{std::int64_t{130}}},
                      {"seed", PlistValue{seed}},
                      {"clientTypeUUID", PlistValue{std::string(s::ultra_ui_service)}}}}}}}}}));
  plist_t reply = nullptr;
  check(plist_from_bin(reinterpret_cast<const char*>(bytes.data()), bytes.size(), &reply) ==
            PLIST_ERR_SUCCESS,
        "invalid fixture SETUP reply");
  const auto stream = plist_array_get_item(plist_dict_get_item(reply, "streams"), 0);
  std::uint64_t port = 0;
  plist_get_uint_val(plist_dict_get_item(stream, "streamID"), &peer.id);
  plist_get_uint_val(plist_dict_get_item(stream, "dataPort"), &port);
  plist_free(reply);
  peer.fd = ::socket(AF_INET, SOCK_STREAM, 0);
  check(peer.fd >= 0, "fixture socket failed");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  check(::connect(peer.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "fixture connect failed");
  session.drain();
}
std::weak_ptr<const void> offer(v::Stream& stream, unsigned quarter) {
  auto frame = std::make_shared<v::NativeFrame>();
  frame->metadata = {(std::uint64_t{899} << 32) + (std::uint64_t{quarter} << 30),
                     899000000000ULL + quarter * 250000000ULL, 2, 2};
  frame->lifetime = std::make_shared<int>(1);
  const std::weak_ptr<const void> backing = frame->lifetime;
  check(stream.offer_native(899 + quarter * .25, frame), "native fixture offer failed");
  return backing;
}
struct RenderOwner {
  Probe& probe;
  std::unique_ptr<g::GpuDisplay> display;
  explicit RenderOwner(Probe& value) : probe(value) {}
  ~RenderOwner() {
    display.reset();
    for (unsigned i = 0; i < 2; ++i) {
      if (!probe.inputs[i]) continue;
      probe.presented[i] = probe.inputs[i]->stats().presented;
      probe.discarded[i] = probe.inputs[i]->stats().discarded;
      probe.inputs[i].reset();
    }
    probe.presentation->retire(probe.presentation_generation);
    probe.finished = true;
  }
};
s::UiWorker::Configuration configuration(Probe& probe, v::ReceiverVideo::Queues queues,
                                         std::shared_ptr<s::UiClockSnapshot> clock) {
  probe.queues = queues;
  s::UiWorker::Configuration config;
  config.displays = {{"fixture", 96, 64}};
  config.routes = {{"fixture", "fixture"}};
  config.resources = [](std::string_view) {
    return std::string{R"(
      local root=CP.DPLayer.create(); root.frame=CP.DPRect(0,0,96,64)
      for _,id in {'main','cluster'} do
        local layer=CP.DPVideoLayer.create(id);layer.frame=CP.DPRect(0,0,48,64)
        layer:subscribe();root:addSublayer(layer)
      end
      CP.setRootLayer('fixture',root)
      return {}
    )"};
  };
  auto owner = std::make_shared<RenderOwner>(probe);
  config.configure = [owner, queues, clock](s::Channel& channel) {
    auto& p = owner->probe;
    render_probe = &p;
    auto& runtime = channel.runtime("fixture");
    g::GpuDisplayOptions options;
    options.window.max_pending_presentations = 2;
    for (unsigned i = 0; i < 2; ++i) {
      auto stream = runtime.add_video_input({i ? "cluster" : "main", 2, 2});
      p.streams[i] = stream;
      p.inputs[i] = std::make_shared<v::NativeInput>(stream, queues[i]);
      options.inputs.push_back(p.inputs[i]);
      for (unsigned quarter = 0; quarter < 3; ++quarter)
        p.backing[i][quarter] = offer(*stream, quarter);
    }
    owner->display = std::make_unique<g::GpuDisplay>(
        runtime, s::Display{"fixture", 96, 64}, [clock] { return clock->sample(); }, options);
  };
  config.presentation = [owner](s::Runtime&, const s::Display&, s::UiWorker::DisplayPhase phase) {
    auto& p = owner->probe;
    if (!p.offer_waiting && p.inputs[0]->stats().presented == 1) {
      for (unsigned i = 0; i < 2; ++i)
        p.backing[i][3] = offer(*p.streams[i], 3);
      p.offer_waiting = true;
    }
    const auto turn = owner->display->turn(phase);
    const auto stats = owner->display->stats();
    p.presentation->observe(p.presentation_generation, stats.scene_visible, false,
                            turn.pts.has_value() && !stats.faulted);
    return turn;
  };
  return config;
}
void local_capture_authority() {
  // Explicitly simulated independent local acquisitions. No phone clock is
  // required by the native Queue; signed LocalInput still requires its clock.
  namespace camera = headunit::camera;
  camera::Configuration config;
  config.enabled = config.manual_activation = true;
  config.source = camera::SourceKind::external;
  config.width = config.height = config.canvas_width = config.canvas_height = 64;
  config.destination = {0, 0, 64, 64};
  auto queue = std::make_shared<camera::Queue>(config);
  constexpr std::uint64_t acquired = 10000000000ULL;
  g::Image image{64, 64, std::vector<std::uint32_t>(4096, 0xff112233)};
  check(queue->offer_image(image, acquired, acquired, true), "local source offer failed");
  auto held = queue->stage(acquired);
  check(held && held->simulated && held->capture_monotonic_ns == acquired &&
            !held->capture_reference && !held->acquisition,
        "local provenance changed");
  check(queue->complete(held->sequence, true), "local native receipt rejected");
  check(queue->current(acquired + 100000000) == held && !queue->current(acquired + 151000000),
        "local acquisition expiry changed");
  image = {64, 64, std::vector<std::uint32_t>(4096, 0xff334455)};
  check(queue->offer_image(image, acquired + 160000000, acquired + 160000000, true),
        "fresh local source rejected after expiry");
  auto memory = std::make_shared<v::Memory>();
  auto stream = std::make_shared<v::Stream>(
      v::Input{config.stream_identifier, 64, 64, 1. / 30, v::InputType::camera}, memory);
  std::uint64_t now = acquired + 160000000;
  v::LocalInput input(config, stream, queue, [&] { return now; });
  input.set_active(true);
  now += 40000000;
  image = {64, 64, std::vector<std::uint32_t>(4096, 0xff556677)};
  check(queue->offer_image(image, now, now, true) && queue->available_capture_ns(now) == now,
        "null-clock fixture did not start with a fresh acquisition");
  input.pump(nullptr);
  v::MediaClock missing;
  check(!input.stage(missing) && !input.consumption() && !stream->composition_token(),
        "null phone clock authorized signed local input");
  check(held->capture_monotonic_ns == acquired && held->simulated,
        "local backing was redated during null-clock withdrawal");
}
void retirement() {
  Probe probe;
  c::UltraRuntime runtime;
  c::Connection connection;
  Release release{probe};  // Always releases both gates before worker destruction.
  connection.session_setup = std::make_unique<AirplaySessionSetup>();
  connection.ultra_video = std::make_shared<v::ReceiverVideo>();
  connection.ultra_video->attach(*connection.session_setup);
  connection.ultra_clock = std::make_shared<s::UiClockSnapshot>();
  const auto at = ns();
  connection.ultra_clock->update(AirplayClockSample{std::uint64_t{2208989700} << 32, at, at, 1000});
  connection.ultra_touches = std::make_shared<TouchMailbox>();
  auto touches = connection.ultra_touches;
  const auto touch_generation = touches->begin();
  connection.ultra_presentation = std::make_shared<MainPresentationSnapshot>();
  probe.presentation = connection.ultra_presentation;
  probe.presentation_generation = probe.presentation->begin();
  connection.camera_state = std::make_shared<vehicle::CameraStateSnapshot>();
  const auto camera_generation = connection.camera_state->begin(at);
  vehicle::ContentRequestCapability capability;
  capability.pages.push_back({"fixture", "fixture:content", "fixture", "fixture"});
  connection.content_state = std::make_shared<vehicle::ContentStateSnapshot>(capability);
  connection.content_state->allow_content(true);
  connection.content_state->update({true, "fixture:content", at / 1000000, at / 1000000 + 2000, 1});
  auto video = connection.ultra_video;
  auto clock = connection.ultra_clock;
  unsigned factories = 0;
  connection.ultra_ui = std::make_unique<s::UiSession>([&] {
    ++factories;
    return configuration(probe, video->claim(), clock);
  });
  connection.ultra_ui->attach(*connection.session_setup);
  initialize(*connection.session_setup);
  Peer peer;
  open_ui(*connection.session_setup, peer, 500);
  connection.ultra_ui->pump(*connection.session_setup);
  until([&] { return probe.callback.waiting(); });
  // Gate synchronization permits inspection of these worker-owned snapshots.
  check(probe.presentation->sample().qualified, "fixture never qualified original main status");
  const auto main_metadata = probe.held[0]->metadata;
  auto queues_closed = [&] {
    return video->closed() && probe.queues[0]->closed() && probe.queues[1]->closed();
  };
  const auto before = std::chrono::steady_clock::now();
  runtime.retire(connection);  // Actual production owner, not copied transition logic.
  check(std::chrono::steady_clock::now() - before < 1s, "control retirement blocked");
  check(queues_closed() && !connection.ultra_ui && !connection.ultra_clock &&
            !connection.ultra_touches && !connection.ultra_presentation && !clock->sample() &&
            runtime.retired.size() == 1 && runtime.has_capacity(),
        "actual receiver retirement did not withdraw its authorities");
  check(!connection.camera_state->observe(camera_generation, true, ns(), ns()),
        "late camera observation revived closed owner");
  connection.content_state->update(
      {true, "fixture:content", ns() / 1000000, ns() / 1000000 + 2000, 2});
  check(!connection.content_state->read(ns() / 1000000).on, "late content revived closed owner");
  const TouchInput down{true, 1, 1};
  touches->push(touch_generation, std::span(&down, 1));
  check(touches->drain([](const auto&) { return true; }) == 0,
        "late touch callback escaped retirement");
  local_capture_authority();
  probe.callback.release();
  until([&] { return probe.cleanup.waiting(); });
  for (unsigned poll = 0; poll < 10; ++poll) {
    runtime.reap();
    check(runtime.retired.size() == 1 && !runtime.retired.front()->finished(),
          "unfinished GPU cleanup owner was reaped");
  }
  // stopped UiSession is terminal, independently of the fleet capacity policy.
  runtime.retired.front()->pump(*connection.session_setup);
  check(factories == 1 && !runtime.retired.front()->stream_id(),
        "stopped UI session admitted another worker");
  probe.cleanup.release();
  until([&] { return runtime.retired.front()->finished(); });
  check(runtime.retired.front()->status()->state == s::UiWorker::State::stopped,
        "finished stopped session retained a live worker state");
  runtime.reap();
  check(runtime.retired.empty(), "finished worker was not reaped");
  check(probe.finished && !probe.callback.expired && !probe.cleanup.expired,
        "owner cleanup did not finish after explicit release");
  for (unsigned i = 0; i < 2; ++i) {
    check(probe.presented[i] == 1 && probe.discarded[i] == 2 && probe.streams[i]->closed() &&
              !probe.streams[i]->composition_token(),
          "late scene receipt advanced a retired source");
  }
  check(probe.held[0]->metadata.sender_ntp == main_metadata.sender_ntp &&
            probe.held[0]->metadata.timestamp_ns == main_metadata.timestamp_ns &&
            !probe.presentation->sample().qualified,
        "held backing/status retirement changed dates");
  // A successor owns distinct production endpoints and copied authority. Old
  // callbacks retain only old objects; old owner stop/reap cannot clear these.
  c::Connection successor;
  successor.session_setup = std::make_unique<AirplaySessionSetup>();
  successor.ultra_video = std::make_shared<v::ReceiverVideo>();
  successor.ultra_video->attach(*successor.session_setup);
  initialize(*successor.session_setup);
  const auto fresh = successor.ultra_video->claim();
  successor.ultra_clock = std::make_shared<s::UiClockSnapshot>();
  successor.ultra_clock->update(
      AirplayClockSample{std::uint64_t{2208989700} << 32, ns(), ns(), 1000});
  successor.ultra_presentation = std::make_shared<MainPresentationSnapshot>();
  const auto generation = successor.ultra_presentation->begin();
  successor.ultra_presentation->observe(generation, true, false, true);
  bool rejected = false;
  try {
    (void)video->claim();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  check(rejected, "retired receiver video admitted a replacement claimant");
  video->close();
  clock->update({});
  probe.presentation->observe(probe.presentation_generation, true, false, true);
  runtime.reap();
  check(fresh[0]->available() && fresh[1]->available() && !fresh[0]->closed() &&
            !fresh[1]->closed() && successor.ultra_clock->sample() &&
            successor.ultra_presentation->sample().qualified,
        "old callback mutated successor");
  for (unsigned i = 0; i < 2; ++i) {
    check(probe.backing[i][0].expired() && probe.backing[i][1].expired() &&
              !probe.backing[i][2].expired() && probe.backing[i][3].expired(),
          "retired source retained backing beyond explicit external holder");
  }
  probe.held = {};
  for (const auto& backing : probe.backing)
    for (const auto& frame : backing)
      check(frame.expired(), "released external backing leaked");
  std::cout << "Actual UltraRuntime retirement: late modeled scene receipt, retained worker, "
               "source backing, local acquisition and successor isolation passed\n";
}
}  // namespace
int main() try { retirement(); } catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
