// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/live_ui.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <headunit/ultra_ui_envelope.hpp>
#include <headunit/binary_plist.hpp>
#include <map>
#include <set>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstdlib>
using namespace headunit;
using namespace headunit::theme;
using namespace std::chrono_literals;
std::uint64_t ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
int main(int argc, char** argv) try {
  if (argc == 3 && std::string_view(argv[2]) == "--validate-only") {
    const auto info = script::InstalledUi::validate_configuration(argv[1]);
    std::cout << "{\"configurationValid\":true,\"assetVersion\":" << info.version
              << ",\"expiredSigningLeafExceptionUsed\":"
              << (info.expired_signer_exception ? "true" : "false")
              << ",\"scriptsExecuted\":false,\"phoneConnected\":false}\n";
    return 0;
  }
  if ((argc != 3 && argc != 4 && argc != 5) || std::string_view(argv[2]) != "--synthetic-clock")
    throw std::invalid_argument(
        "usage: headunit-installed-ui-probe PRIVATE_PROFILE --validate-only | --synthetic-clock "
        "[DISPLAYS.conf [PRIVATE_UI_CAPTURE_DIR]]");
  unsigned run_seconds = 1;
  if (const auto* raw = std::getenv("HEADUNIT_INSTALLED_UI_PROBE_SECONDS")) {
    const std::string_view value(raw);
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), run_seconds);
    if (error != std::errc{} || end != value.data() + value.size() || run_seconds < 1 ||
        run_seconds > 20)
      throw std::invalid_argument("invalid installed UI probe duration (1..20 seconds)");
  }
  auto installed = script::InstalledUi::open(argv[1]);
  auto clock = std::make_shared<script::UiClockSnapshot>();
  AirplaySessionSetup socket;
  auto video = std::make_shared<video::ReceiverVideo>();
  video->attach(socket);
  auto touches = std::make_shared<TouchMailbox>();
  std::shared_ptr<vehicle::ContentStateSnapshot> content;
  if (const auto* settings = std::getenv("HEADUNIT_INSTALLED_UI_PROBE_CONTENT_CONFIG")) {
    const auto c = vehicle::load_configuration({}, std::filesystem::path(settings));
    if (!c.content_request || run_seconds < 8)
      throw std::invalid_argument(
          "content fixture needs declared requests and an 8..20 second run");
    installed->validate_content_request(*c.content_request);
    content = std::make_shared<vehicle::ContentStateSnapshot>(*c.content_request);
  }
  auto factory = installed->factory(
      "aabbccdd-0000-0000-0000-000000000001", "aabbccdd-0000-0000-0000-000000000002", clock, video,
      touches, argc >= 4 ? load_display_configuration(argv[3]) : default_receiver_displays(), {},
      {}, {}, false, content);
  struct Capture {
    int directory = -1;
    ~Capture() {
      if (directory >= 0) close(directory);
    }
    void write(const DataMessage& message, std::size_t index) {
      if (directory < 0) return;
      const auto name = "startup-" + std::to_string(index) + ".opack";
      const int fd = openat(directory, name.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd < 0) throw std::runtime_error("cannot create private UI capture");
      const auto count = ::write(fd, message.payload.data(), message.payload.size());
      close(fd);
      if (count < 0 || std::size_t(count) != message.payload.size())
        throw std::runtime_error("short UI capture write");
    }
  } capture;
  if (argc == 5) {
    capture.directory = open(argv[4], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (capture.directory < 0 || fstat(capture.directory, &st) || st.st_uid != getuid() ||
        (st.st_mode & 0077))
      throw std::invalid_argument("UI capture directory must be owned and private");
  }
  const auto origin = ns();
  auto configuration = factory();
  std::atomic<unsigned> content_read_checks{0}, content_local_urls{0};
  std::atomic<unsigned> content_startup_checks{0}, content_dismissals{0};
  std::atomic<bool> content_drive_ready{false};
  if (content) {
    auto present = configuration.presentation;
    configuration.presentation = [content, present, &content_read_checks, &content_local_urls,
                                  &content_startup_checks, &content_dismissals,
                                  &content_drive_ready, drive_started = std::uint64_t{0}](
                                     script::Runtime& runtime, const script::Display& display,
                                     script::UiWorker::DisplayPhase phase) mutable {
      auto turn = present(runtime, display, phase);
      const auto now = ns() / 1000000;
      const auto state = content->read(now);
      if (display.identifier == "Cluster_Display" &&
          phase == script::UiWorker::DisplayPhase::frame &&
          runtime.scene_phase() != script::Runtime::ScenePhase::drive) {
        if (state.on) throw std::runtime_error("content visible during startup");
        runtime.evaluate(R"(
          local config = CP.getVehicleDataConfig().automakerRequestContent.requestContent[1]
          assert(CP.getVehicleData({config.on.iid})[config.on.iid] == false,
                 'local content visible during startup')
        )",
                         "content-fixture-startup");
        ++content_startup_checks;
      }
      if (display.identifier == "Cluster_Display" &&
          phase == script::UiWorker::DisplayPhase::frame &&
          runtime.scene_phase() == script::Runtime::ScenePhase::drive) {
        if (!drive_started) drive_started = now;
        content_drive_ready.store(true);
        if (!state.on || now < state.decided_ms + 150 || state.decided_ms <= drive_started)
          return turn;
        runtime.evaluate(std::string(R"(
          local config = CP.getVehicleDataConfig().automakerRequestContent.requestContent[1]
          local values = CP.getVehicleData({config.on.iid, config.contentURL.iid})
          assert(values[config.on.iid] == true, 'local content On missing')
          assert(type(values[config.contentURL.iid]) == 'string', 'local content URL missing')
          assert(values[config.contentURL.iid] == ')") +
                             state.url + "')",
                         "content-fixture-read");
        const auto& pages = content->capability().pages;
        for (unsigned i = 0; i < pages.size(); ++i)
          if (pages[i].url == state.url) content_local_urls.fetch_or(1U << i);
        ++content_read_checks;
        if (std::size_t(std::popcount(content_local_urls.load())) == pages.size() &&
            !content_dismissals) {
          runtime.evaluate(R"(
            local config = CP.getVehicleDataConfig().automakerRequestContent.requestContent[1]
            assert(next(CP.setVehicleData({[config.on.iid]=true})) == nil)
            assert(CP.setVehicleData({[config.on.iid]=false})[config.on.iid] == false)
            assert(CP.getVehicleData({config.on.iid})[config.on.iid] == false)
          )",
                           "content-fixture-close");
          if (content->read(now).on) throw std::runtime_error("local Close did not retire intent");
          ++content_dismissals;
        }
      }
      return turn;
    };
  }
  script::UiWorker worker(7, std::move(configuration));
  std::uint64_t running = 0, content_renewed = 0, content_started = 0;
  std::size_t messages = 0;
  std::set<std::string> content_urls;
  const std::array<std::string, 2> ids{"aabbccdd-0000-0000-0000-000000000001",
                                       "aabbccdd-0000-0000-0000-000000000002"};
  std::map<std::string, std::int64_t> peer_sequences;
  const auto run_ns = std::uint64_t(run_seconds) * 1000000000;
  while (ns() - origin < 30000000000ULL + run_ns) {
    const auto at = ns(), elapsed = at - origin;
    const auto media = ((std::uint64_t{2208989700} + elapsed / 1000000000) << 32) +
                       ((elapsed % 1000000000) << 32) / 1000000000;
    clock->update(AirplayClockSample{media, at, at, 1000});
    const auto status = worker.status();
    if (status.state == script::UiWorker::State::faulted)
      throw std::runtime_error("installed UI: " + worker.failure_reason());
    if (status.state == script::UiWorker::State::running && !running) running = at;
    if (content && running && at / 1000000 >= content_renewed + 500) {
      const auto& pages = content->capability().pages;
      if (content_drive_ready.load() && !content_started) content_started = at;
      // A genuine post-startup lease, then two seconds per page and an OFF
      // interval. Startup time does not consume any page's qualification window.
      const auto fraction = content_started ? (at - content_started) / 2000000000ULL : 0;
      const auto now = at / 1000000;
      if (fraction < pages.size())
        content->update({true, pages[fraction].url, now, now + 2000});
      else
        content->update({false, {}, now, 0});
      content_renewed = now;
    }
    std::vector<DataMessage> replies;
    for (unsigned i = 0; i < 32 && worker.send_pending([&](const DataMessage& m) {
           capture.write(m, ++messages);
           if (content) {
             const std::string_view payload(reinterpret_cast<const char*>(m.payload.data()),
                                            m.payload.size());
             for (const auto& page : content->capability().pages)
               if (payload.find(page.url) != payload.npos) content_urls.insert(page.url);
           }
           if (capture.directory >= 0) {
             auto envelope = decode_ultra_ui_envelope(m.payload, ids);
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
               DataMessage reply;
               reply.kind = DataMessageKind::asynchronous;
               reply.type = 0x636f6d6d;
               reply.payload = encode_ultra_ui_envelope(envelope);
               replies.push_back(std::move(reply));
             }
           }
           return true;
         });
         ++i) {
    }
    for (const auto& reply : replies)
      if (!worker.receive(7, reply)) throw std::runtime_error("synthetic peer reply rejected");
    if (running && at - running >= run_ns) break;
    std::this_thread::sleep_for(2ms);
  }
  const bool running_ok = running && ns() - running >= run_ns;
  if (content) content->close(ns() / 1000000);
  video->close();
  clock->update({});
  worker.request_stop();
  while (!worker.finished())
    std::this_thread::sleep_for(2ms);
  if (!running_ok || messages < 2)
    throw std::runtime_error("installed UI did not finish both entry points");
  std::cout << "{\"installedFactoryRunning\":true,\"nativeInputsRegistered\":true,"
               "\"syntheticClock\":true,\"phoneConnected\":false,\"runSeconds\":"
            << run_seconds << ",\"contentIntentFixture\":" << (content ? "true" : "false")
            << ",\"contentLocalReadChecks\":" << content_read_checks.load()
            << ",\"contentLocalURLsObserved\":" << std::popcount(content_local_urls.load())
            << ",\"contentStartupChecks\":" << content_startup_checks.load()
            << ",\"contentLocalDismissals\":" << content_dismissals.load()
            << ",\"contentURLsInOutboundMessages\":" << content_urls.size()
            << ",\"startupMessages\":" << messages << "}\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
