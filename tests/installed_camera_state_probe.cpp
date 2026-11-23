// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_controller_io.hpp>
// Explicit signed-scene fixture: synthetic clock/peer/camera, real GPU feedback.
// No radio, phone, physical display or production service is opened here.
#include <headunit/theme/live_ui.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <headunit/theme/video_stream.hpp>
#include <headunit/binary_plist.hpp>
#include <headunit/ultra_ui_envelope.hpp>
#include <array>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace headunit;
using namespace headunit::theme;
using namespace std::chrono_literals;
namespace {
std::uint64_t ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void check(bool ok, const char* reason) {
  if (!ok) throw std::runtime_error(reason);
}
void activate(const std::string& path, bool on) {
  const auto temporary = path + ".probe-tmp";
  const int fd =
      open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  check(fd >= 0, "cannot create private activation request");
  const std::string value = on ? "on\n" : "off\n";
  const auto count = write(fd, value.data(), value.size());
  const auto closed = close(fd);
  check(count == ssize_t(value.size()) && closed == 0, "activation request write failed");
  check(rename(temporary.c_str(), path.c_str()) == 0, "activation request replacement failed");
}
struct Trace {
  std::mutex mutex;
  std::array<std::uint64_t, 2> receipts{};
  std::array<std::optional<double>, 2> last_pts{};
  std::uint64_t camera_receipts = 0;
  std::optional<double> camera_pts;
  bool camera_pixels = false;
  std::optional<script::ClusterContentChoice> assistance;
  std::uint64_t assistance_receipts = 0;
  std::optional<double> assistance_pts;
  void observe(script::Runtime& runtime, unsigned panel,
               const script::UiWorker::DisplayTurn& turn) {
    std::lock_guard lock(mutex);
    if (turn.pts && turn.pts != last_pts[panel]) {
      ++receipts[panel];
      last_pts[panel] = turn.pts;
    }
    if (panel) {
      for (const auto& option : runtime.cluster_selection().contents)
        if (option.name == "DCA_ADAS" && option.available) assistance = option;
    } else
      camera_pixels = false;
    unsigned visited = 0;
    const auto walk = [&](auto&& self, const std::shared_ptr<const graphics::Layer>& layer,
                          unsigned depth) -> void {
      if (!layer || depth > 64 || ++visited > 4096 || layer->hidden || layer->opacity == 0) return;
      if (panel && layer->video && layer->video->active() &&
          layer->video->stream->input().identifier == "adasView") {
        const auto pts = layer->video->stream->current_pts();
        if (pts && pts != assistance_pts && layer->video->image()) {
          ++assistance_receipts;
          assistance_pts = pts;
        }
      }
      if (!panel && layer->video && layer->video->active() &&
          layer->video->stream->input().type == video::InputType::camera) {
        const auto pts = layer->video->stream->current_pts();
        camera_pixels = bool(pts) && bool(layer->video->image() || layer->video->native());
        if (camera_pixels && pts != camera_pts) {
          ++camera_receipts;
          camera_pts = pts;
        }
      }
      for (const auto& child : layer->children)
        self(self, child, depth + 1);
    };
    walk(walk, runtime.scene_root(), 0);
  }
};
}  // namespace
int main(int argc, char** argv) try {
  check(argc == 5 || argc == 7,
        "usage: installed-camera-state-probe PRIVATE_PROFILE DISPLAYS.conf CAMERA_STATE.json "
        "PRIVATE_REQUEST");
  const bool bench_adas = std::getenv("HEADUNIT_CAMERA_PROBE_ADAS") != nullptr;
  auto installed = script::InstalledUi::open(argv[1]);
  std::unique_ptr<camera::StatusReader> native_status;
  if (!installed->camera_status_file().empty())
    native_status = std::make_unique<camera::StatusReader>(installed->camera_status_file());
  const auto capability = vehicle::load_configuration({}, {}, std::filesystem::path(argv[3]));
  const auto camera = installed->camera_configuration();
  check(camera && camera->enabled && capability.camera,
        "probe requires enabled camera and state profiles");
  const auto c = *capability.camera;
  check(c.stream_identifier == camera->stream_identifier && c.max_age_ms > camera->maximum_age_ms,
        "state profile does not match signed input");
  auto state = std::make_shared<vehicle::CameraStateSnapshot>();
  auto provider = vehicle::camera_state_provider(vehicle::simulated_provider(capability), c, state);
  vehicle::ConfiguredModel model(capability, provider);
  auto clock = std::make_shared<script::UiClockSnapshot>();
  std::unique_ptr<AirplaySessionSetup> setup;
  std::shared_ptr<video::ReceiverVideo> video;
  auto touches = std::make_shared<TouchMailbox>();
  const auto displays = load_display_configuration(argv[2]);
  const std::array<std::string, 2> ids{"aabbccdd-0000-0000-0000-000000000001",
                                       "aabbccdd-0000-0000-0000-000000000002"};
  std::shared_ptr<Trace> trace;
  std::unique_ptr<script::UiWorker> worker;
  std::map<std::string, std::int64_t> peer_sequences;
  std::optional<UltraUiEnvelope> cluster_peer_template;
  const auto origin = ns();
  bool timed = true;
  std::uint64_t alias_checks = 0, dated_reads = 0, messages = 0;
  std::uint64_t applied_off_requests = 0, pending_off_checks = 0;
  const auto start = [&] {
    // A replacement connection has a new control/video owner. A closed
    // ReceiverVideo deliberately refuses another claim of its old session.
    if (video) video->close();
    video.reset();
    setup = std::make_unique<AirplaySessionSetup>();
    video = std::make_shared<video::ReceiverVideo>();
    video->attach(*setup);
    auto factory = installed->factory(ids[0], ids[1], clock, video, touches, displays, {}, state);
    trace = std::make_shared<Trace>();
    auto config = factory();
    config.presentation = [present = config.presentation, trace, main = displays[0].panel_name](
                              auto& runtime, const auto& display, auto phase) {
      auto turn = present(runtime, display, phase);
      trace->observe(runtime, display.identifier == main ? 0 : 1, turn);
      return turn;
    };
    peer_sequences.clear();
    cluster_peer_template.reset();
    worker = std::make_unique<script::UiWorker>(7, std::move(config));
  };
  const auto pump = [&] {
    const auto at = ns(), elapsed = at - origin;
    const auto media = ((std::uint64_t{2208989700} + elapsed / 1000000000) << 32) +
                       ((elapsed % 1000000000) << 32) / 1000000000;
    clock->update(timed ? std::optional<AirplayClockSample>{{media, at, at, 1000}} : std::nullopt);
    if (worker->status().state == script::UiWorker::State::faulted)
      throw std::runtime_error("installed camera worker faulted: " + worker->failure_reason());
    std::vector<DataMessage> replies;
    for (unsigned i = 0; i < 32 && worker->send_pending([&](const DataMessage& message) {
           ++messages;
           auto envelope = decode_ultra_ui_envelope(message.payload, ids);
           auto value = decode_binary_plist(envelope.binary_plist);
           auto& dictionary = std::get<PlistValue::Dictionary>(value.value);
           const auto type = std::get<std::int64_t>(dictionary.at("type").value);
           if (type == 0 || type == 2) {
             auto& sequence = peer_sequences[envelope.display_uuid];
             if (type == 0) sequence = 0;
             dictionary["type"] = PlistValue{std::int64_t(type == 0 ? 1 : 3)};
             dictionary["asn"] = dictionary.at("psn");
             dictionary["psn"] = PlistValue{++sequence};
             dictionary.erase("data");
             dictionary.erase("noAck");
             envelope.binary_plist = encode_binary_plist(value);
             if (envelope.display_uuid == ids[1]) cluster_peer_template = envelope;
             DataMessage reply;
             reply.kind = DataMessageKind::asynchronous;
             reply.type = 0x636f6d6d;
             reply.payload = encode_ultra_ui_envelope(envelope);
             replies.push_back(std::move(reply));
           }
           return true;
         });
         ++i) {
    }
    for (const auto& reply : replies)
      check(worker->receive(7, reply), "synthetic peer reply rejected");
    // Read once through ConfiguredModel, just as the production receiver does.
    const auto now = ns() / 1000000;
    model.refresh(now);
    const std::array<std::int64_t, 2> aliases{c.input_stream_on_iid, c.camera_general_on_iid};
    const auto readings = model.service().snapshot(aliases, now);
    const auto& a = readings.at(aliases[0]);
    const auto& b = readings.at(aliases[1]);
    check(!a.error && !b.error && a.value == b.value,
          "camera aliases diverged or became unavailable");
    ++alias_checks;
    const auto sample = provider->read(c.binding, ns() / 1000000);
    if (sample && sample->value == opack::Value{true}) {
      check(sample->sampled_ms <= ns() / 1000000 &&
                ns() / 1000000 - sample->sampled_ms < c.max_age_ms,
            "camera bridge published future or expired acquisition date");
      ++dated_reads;
    }
    return a.value == opack::Value{true};
  };
  const auto until = [&](auto condition, const char* reason) {
    const auto deadline = ns() + 20000000000ULL;
    do {
      const bool on = pump();
      if (condition(on)) return;
      std::this_thread::sleep_for(5ms);
    } while (ns() < deadline);
    {
      std::lock_guard lock(trace->mutex);
      std::cerr << "fixture progress main=" << trace->receipts[0]
                << " cluster=" << trace->receipts[1] << " camera=" << trace->camera_receipts
                << " pixels=" << trace->camera_pixels << " datedReads=" << dated_reads << '\n';
    }
    throw std::runtime_error(reason);
  };
  const auto fresh = [&](bool on) {
    std::lock_guard lock(trace->mutex);
    return on && trace->camera_pixels && trace->camera_receipts >= 3 && trace->receipts[0] >= 5 &&
           trace->receipts[1] >= 5;
  };
  const auto request_off = [&] {
    const auto at = ns();
    const auto request =
        state->request_off(at, at + vehicle::CameraStateSnapshot::maximum_off_wait_ns);
    check(bool(request), "current signed camera owner rejected bounded OFF request");
    return *request;
  };
  const auto applied_off = [&](const vehicle::CameraStateSnapshot::OffRequest& request,
                               const std::array<std::uint64_t, 2>& previous) {
    until(
        [&](bool on) {
          const auto result = state->off_result(request.generation, request.sequence, ns());
          check(bool(result), "signed camera OFF result lost its owner/request identity");
          check(result->status == vehicle::CameraStateSnapshot::OffStatus::pending ||
                    result->status == vehicle::CameraStateSnapshot::OffStatus::applied,
                "signed camera OFF timed out or retired before a qualified receipt");
          if (result->status != vehicle::CameraStateSnapshot::OffStatus::applied) return false;
          check(request.requested_ns <= result->withdrawn_ns &&
                    result->withdrawn_ns <= result->presented_ns &&
                    result->presented_ns < request.deadline_ns,
                "signed camera OFF receipt dates are not genuine ordered owner dates");
          std::lock_guard lock(trace->mutex);
          return !on && !trace->camera_pixels && trace->receipts[0] > previous[0] + 3 &&
                 trace->receipts[1] > previous[1] + 3;
        },
        "mailbox OFF did not receive a camera-free scene while both signed panels continued");
    ++applied_off_requests;
  };
  activate(argv[4], false);
  start();
  until(
      [&](bool on) {
        std::lock_guard lock(trace->mutex);
        return !on && trace->receipts[0] >= 5 && trace->receipts[1] >= 5;
      },
      "signed panels did not start with camera Off");
  if (bench_adas) {
    until(
        [&](bool) {
          std::lock_guard lock(trace->mutex);
          return trace->assistance.has_value() && cluster_peer_template.has_value();
        },
        "signed DCA_ADAS did not admit the registered bench widget");
    script::ClusterContentChoice option;
    {
      std::lock_guard lock(trace->mutex);
      option = *trace->assistance;
    }
    // Explicit synthetic peer uses the existing UI-Sync channel. Never mutate
    // the local active selection or treat a host request as phone confirmation.
    for (const std::int64_t type : {1, 4}) {
      auto envelope = *cluster_peer_template;
      auto value = decode_binary_plist(envelope.binary_plist);
      auto& d = std::get<PlistValue::Dictionary>(value.value);
      d["type"] = PlistValue{std::int64_t(2)};
      d["psn"] = PlistValue{++peer_sequences[ids[1]]};
      PlistValue::Dictionary data;
      if (type == 1)
        data["targetComponents"] =
            PlistValue{PlistValue::Dictionary{{option.zone, PlistValue{option.identifier}}}};
      d["data"] = PlistValue{encode_binary_plist(PlistValue{
          PlistValue::Dictionary{{"type", PlistValue{type}}, {"data", PlistValue{data}}}})};
      envelope.binary_plist = encode_binary_plist(value);
      DataMessage message;
      message.kind = DataMessageKind::asynchronous;
      message.type = 0x636f6d6d;
      message.payload = encode_ultra_ui_envelope(envelope);
      check(worker->receive(7, message), "synthetic ADAS transition rejected");
    }
    until(
        [&](bool) {
          std::lock_guard lock(trace->mutex);
          return trace->assistance_receipts >= 5;
        },
        "signed ADAS slot did not present fresh local bench graphics");
  }
  activate(argv[4], true);
  until(fresh, "signed camera/state On did not receive real presentations");
  if (native_status)
    until(
        [&](bool) {
          const auto status = native_status->poll(500);
          return status && status->phase == camera::ControlPhase::active;
        },
        "native camera status did not follow the existing signed renderer");
  std::array<std::uint64_t, 2> before{};
  std::uint64_t camera_before = 0;
  {
    std::lock_guard lock(trace->mutex);
    before = trace->receipts;
    camera_before = trace->camera_receipts;
  }
  // Exercise the existing owner mailbox, with the manual ON request still
  // asserted. Only production withdrawal plus real zero-camera feedback can
  // acknowledge this request; neither the fixture nor a provider sets applied.
  const auto first_off = request_off();
  applied_off(first_off, before);
  {
    std::lock_guard lock(trace->mutex);
    before = trace->receipts;
    camera_before = trace->camera_receipts;
  }
  for (unsigned i = 0; i < 40; ++i) {
    check(!pump(), "still-asserted manual ON revived a dismissed camera");
    {
      std::lock_guard lock(trace->mutex);
      check(!trace->camera_pixels && trace->camera_receipts == camera_before,
            "dismissed camera retained or presented new pixels");
    }
    std::this_thread::sleep_for(5ms);
  }
  // An explicit manual OFF must be observed before ON can re-arm. Scene
  // progress establishes owner turns, rather than a guessed elapsed delay.
  {
    std::lock_guard lock(trace->mutex);
    before = trace->receipts;
  }
  activate(argv[4], false);
  until(
      [&](bool on) {
        std::lock_guard lock(trace->mutex);
        return !on && !trace->camera_pixels && trace->receipts[0] > before[0] + 3 &&
               trace->receipts[1] > before[1] + 3;
      },
      "Off failed to clear camera while keeping both scenes alive");
  activate(argv[4], true);
  until(
      [&](bool on) {
        std::lock_guard lock(trace->mutex);
        return on && trace->camera_pixels && trace->camera_receipts > camera_before + 3;
      },
      "re-entry reused old camera pixels or failed state recovery");
  const auto rearmed = provider->read(c.binding, ns() / 1000000);
  const auto first_result = state->off_result(first_off.generation, first_off.sequence, ns());
  check(rearmed && first_result && rearmed->value == opack::Value{true} &&
            rearmed->sampled_ms > first_result->presented_ns / 1000000,
        "mailbox re-entry reused an acquisition from before the camera-free receipt");
  timed = false;
  until(
      [&](bool on) {
        std::lock_guard lock(trace->mutex);
        return !on && !trace->camera_pixels;
      },
      "lost clock retained camera state/pixels");
  const auto clock_off = request_off();
  for (unsigned i = 0; i < 40; ++i) {
    check(!pump(), "unqualified clock exposed camera On during OFF");
    const auto result = state->off_result(clock_off.generation, clock_off.sequence, ns());
    check(result && result->status == vehicle::CameraStateSnapshot::OffStatus::pending,
          "missing clock or eligibility false fabricated an applied OFF receipt");
    ++pending_off_checks;
    std::this_thread::sleep_for(5ms);
  }
  {
    std::lock_guard lock(trace->mutex);
    before = trace->receipts;
    camera_before = trace->camera_receipts;
  }
  timed = true;
  applied_off(clock_off, before);
  {
    std::lock_guard lock(trace->mutex);
    before = trace->receipts;
  }
  activate(argv[4], false);
  until(
      [&](bool on) {
        std::lock_guard lock(trace->mutex);
        return !on && !trace->camera_pixels && trace->receipts[0] > before[0] + 3 &&
               trace->receipts[1] > before[1] + 3;
      },
      "explicit OFF did not re-arm after clock recovery");
  activate(argv[4], true);
  until(
      [&](bool on) {
        std::lock_guard lock(trace->mutex);
        return on && trace->camera_pixels && trace->camera_receipts > camera_before + 6;
      },
      "clock recovery failed to present fresh camera pixels");
  if (argc == 7) {
    // Native Home takes only the existing main lease. Camera visibility and
    // pixels withdraw; cluster/UI-Sync continue, then fresh capture restores.
    camera::CameraSurfaceLease native_lease(argv[6]);
    {
      std::lock_guard lock(trace->mutex);
      before = trace->receipts;
      camera_before = trace->camera_receipts;
    }
    activate(argv[5], true);
    until([&](bool on) { return !on && native_lease.acquire_exclusive(); },
          "native Home could not take the shared signed-camera main surface");
    until(
        [&](bool on) {
          std::lock_guard lock(trace->mutex);
          return !on && !trace->camera_pixels && trace->receipts[1] > before[1] + 4;
        },
        "native Home retained camera pixels or stalled cluster");
    native_lease.release();
    activate(argv[5], false);
    until(
        [&](bool on) {
          std::lock_guard lock(trace->mutex);
          return on && trace->camera_pixels && trace->camera_receipts > camera_before + 4 &&
                 trace->receipts[0] > before[0] + 4;
        },
        "native Home return did not restore fresh signed-camera acquisitions");
  }
  worker->request_stop();
  const auto retirement = ns() + 5000000000ULL;
  while (!worker->finished() && ns() < retirement)
    std::this_thread::sleep_for(5ms);
  check(worker->finished(), "camera worker retirement exceeded five seconds");
  worker.reset();
  if (native_status) {
    const auto status = native_status->poll(500);
    check(status && status->phase == camera::ControlPhase::stopped,
          "retired signed owner retained native camera eligibility");
  }
  check(provider->read(c.binding, ns() / 1000000)->value == opack::Value{false},
        "retired signed owner left camera On");
  start();
  until(fresh, "replacement signed owner did not recover camera/aliases");
  state->close(ns());
  until([](bool on) { return !on; }, "closed connection revived camera aliases");
  for (unsigned i = 0; i < 40; ++i) {
    check(!pump(), "old worker revived closed camera state");
    std::this_thread::sleep_for(5ms);
  }
  worker->request_stop();
  const auto final_retirement = ns() + 5000000000ULL;
  while (!worker->finished() && ns() < final_retirement)
    std::this_thread::sleep_for(5ms);
  check(worker->finished(), "closed worker retirement exceeded five seconds");
  worker.reset();
  activate(argv[4], false);
  video->close();
  clock->update({});
  std::cout << "{\"signedCameraStateLifecycle\":true,\"aliasChecks\":" << alias_checks
            << ",\"signedCameraMailboxOff\":true,\"appliedOffRequests\":" << applied_off_requests
            << ",\"pendingOffWithoutClockChecks\":" << pending_off_checks
            << ",\"originalDatedReads\":" << dated_reads
            << ",\"syntheticPeerMessages\":" << messages
            << ",\"syntheticClock\":true,\"syntheticCamera\":"
            << (camera->source == headunit::camera::SourceKind::synthetic_test_pattern ? "true"
                                                                                       : "false")
            << ",\"benchAssistancePresented\":" << (bench_adas ? "true" : "false")
            << ",\"nativePageCoexistence\":" << (argc == 7 ? "true" : "false")
            << ",\"phoneConnected\":false,"
               "\"physicalScanoutQualified\":false}\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
