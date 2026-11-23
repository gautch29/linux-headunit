// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/layout_control.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace headunit::theme::script;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
const LayoutControl::Publisher publisher{"0123456789abcdef0123456789abcdef", 1};
std::string request(std::string_view id) {
  return "HUL1 " + publisher.nonce + " 1\n" + std::string(id) + "\n";
}
constexpr auto a = "00000000-1111-2222-3333-444444444444";
constexpr auto b = "00000000-1111-2222-3333-555555555555";
constexpr auto c = "00000000-1111-2222-3333-666666666666";
std::string read(const fs::path& path) {
  std::ifstream f(path);
  return {std::istreambuf_iterator<char>(f), {}};
}
void palette_history(const fs::path& folder) {
  const auto file = folder / "history";
  const auto saved = file.string() + ".confirmed.reference:985";
  const auto history = saved + ".palettes";
  auto owner = std::make_unique<LayoutControl>(file.string(), "reference:985", publisher);
  auto now = LayoutControl::Clock::time_point(1s);
  LayoutSelection state;
  state.ready = state.idle = true;
  state.active = a;
  state.active_palette = "blue";
  state.choices = {{a, "A"}, {b, "B"}, {c, "C"}};
  state.palettes = {{"blue", "Blue"}, {"red", "Red"}};
  std::vector<ClusterSelectionRequest> requests;
  auto outcome = LayoutRequest::requested;
  const auto select = [&](const std::optional<ClusterSelectionRequest>& choice) {
    auto result = state;
    if (choice) {
      requests.push_back(*choice);
      result.result = outcome;
    }
    return result;
  };
  const auto poll = [&] {
    now += 250ms;
    owner->wait_for_preferences();
    owner->poll_cluster(select, now);
    owner->wait_for_preferences();
  };
  const auto write = [&](std::string_view layout, std::string_view palette = {}) {
    const auto next = file.string() + ".new";
    if (palette.empty())
      std::ofstream(next) << request(layout);
    else
      std::ofstream(next) << "HUL2 " << publisher.nonce << " 1\n"
                          << layout << '\n'
                          << palette << "\n\n\n";
    fs::permissions(next, fs::perms::owner_read | fs::perms::owner_write);
    fs::rename(next, file);
  };
  poll();
  write(a, "red");
  poll();
  CHECK(!fs::exists(saved) && !fs::exists(history));
  state.active_palette = "red";
  poll();
  CHECK(read(saved) == std::string(a) + "\nred\n");
  CHECK(read(history) == "HUP1\n" + std::string(a) + " red\n");
  write(b);
  poll();
  CHECK(requests.back().layout == b && requests.back().palette.empty());
  state.active = b;
  state.active_palette = "blue";
  poll();
  CHECK(read(history) == "HUP1\n" + std::string(a) + " red\n" + b + " blue\n");
  // A new VM restores v1's last choice, then can return to A's distinct colour.
  owner.reset();
  state.active = c;
  owner = std::make_unique<LayoutControl>(file.string(), "reference:985", publisher);
  poll();
  CHECK(requests.back().layout == b && requests.back().palette == "blue");
  state.active = b;
  poll();
  write(a);
  poll();
  CHECK(requests.back().layout == a && requests.back().palette == "red");
  state.active = a;
  state.active_palette.clear();
  poll();
  CHECK(read(saved) == std::string(b) + "\nblue\n");
  now += 8s;
  poll();
  CHECK(read(saved) == std::string(b) + "\nblue\n");
  CHECK(read(history).find(std::string(a) + " red\n") != std::string::npos);
  CHECK(read(file.string() + ".status").find("colour was not confirmed") != std::string::npos);
  poll();
  CHECK(read(history).find(std::string(a) + " red\n") != std::string::npos);
  // Explicit colour wins over remembered colour; paired confirmation is required.
  state.active_palette = "blue";
  write(a, "blue");
  outcome = LayoutRequest::selected;
  poll();
  CHECK(requests.back().palette == "blue");
  CHECK(read(saved) == std::string(a) + "\nblue\n");
  CHECK(read(history).find(std::string(a) + " blue\n") != std::string::npos);
  // Withdrawal omits remembered colour, without replay or history erasure.
  state.palettes = {{"red", "Red"}};
  state.active = b;
  state.active_palette = "red";
  poll();
  write(a);
  outcome = LayoutRequest::unknown;
  poll();
  CHECK(requests.back().palette.empty());
  const auto count = requests.size();
  const auto before_rejection = read(history);
  poll();
  CHECK(requests.size() == count && read(history) == before_rejection);
  // The same history cannot cross a verified pack/version scope.
  owner.reset();
  owner = std::make_unique<LayoutControl>(file.string(), "other:2", publisher);
  poll();
  write(a);
  poll();
  CHECK(requests.back().palette.empty());
  owner.reset();
  // Strict history corruption does not prevent the valid v1 last selection.
  for (const auto& invalid :
       {std::string("HUP2\n") + a + " red\n", std::string("HUP1\n") + a + " red\n" + a + " blue\n",
        std::string("HUP1\n") + a + " red bad\n", std::string("HUP1\n") + a + " red",
        std::string("HUP1\n") + "foreign red\n"}) {
    std::ofstream(history) << invalid;
    fs::permissions(history, fs::perms::owner_read | fs::perms::owner_write);
    state.palettes = {{"blue", "Blue"}, {"red", "Red"}};
    state.active = c;
    state.active_palette = "red";
    outcome = LayoutRequest::requested;
    owner = std::make_unique<LayoutControl>(file.string(), "reference:985", publisher);
    poll();
    CHECK(requests.back().layout == b && requests.back().palette == "red");
    CHECK(read(file.string() + ".status").find("Invalid private cluster colour history") !=
          std::string::npos);
    state.active = b;
    poll();
    CHECK(read(history) == "HUP1\n" + std::string(b) + " red\n");
    owner.reset();
  }
  // Independent disk failure keeps last selection usable, and retries only
  // while the same paired choice remains fresh, idle and currently catalogued.
  fs::remove(history);
  owner = std::make_unique<LayoutControl>(file.string(), "reference:985", publisher);
  state.active = b;
  state.active_palette = "red";
  outcome = LayoutRequest::selected;
  fs::create_directory(history);
  poll();
  poll();
  CHECK(read(file.string() + ".status").find("Cannot save confirmed") != std::string::npos);
  fs::remove(history);
  state.ready = false;
  now += 2s;
  poll();
  CHECK(!fs::exists(history));
  state.ready = true;
  state.palettes = {{"blue", "Blue"}};
  poll();
  CHECK(!fs::exists(history));
  state.palettes.push_back({"red", "Red"});
  poll();
  CHECK(read(history) == "HUP1\n" + std::string(b) + " red\n");
  owner.reset();
  // Legacy migration does not write merely because an unconfirmed file exists.
  fs::remove(history);
  state.ready = false;
  owner = std::make_unique<LayoutControl>(file.string(), "reference:985", publisher);
  poll();
  CHECK(!fs::exists(history));
  owner.reset();
  // The catalogue can exceed the history capacity. Preserve the most recently
  // confirmed 32 styles, and never offer a guessed colour for an evicted style.
  fs::remove(saved);
  state.ready = true;
  state.choices.clear();
  std::vector<std::string> layouts;
  for (unsigned i = 0; i < 34; ++i) {
    const auto suffix = std::to_string(i);
    layouts.push_back("00000000-1111-2222-3333-" + std::string(12 - suffix.size(), '0') + suffix);
    state.choices.push_back({layouts.back(), "Style"});
  }
  state.active = layouts.front();
  state.active_palette = "red";
  owner = std::make_unique<LayoutControl>(file.string(), "reference:985", publisher);
  poll();
  for (std::size_t i = 1; i < layouts.size(); ++i) {
    state.active = layouts[i];
    poll();
  }
  const auto capped = read(history);
  CHECK(std::count(capped.begin(), capped.end(), '\n') == 33);
  CHECK(capped.find(layouts.front()) == std::string::npos);
  CHECK(capped.find(layouts[1]) == std::string::npos);
  CHECK(capped.find(layouts.back()) != std::string::npos);
  write(layouts[1]);
  poll();
  CHECK(requests.back().palette.empty());
  owner.reset();
  std::string excessive = "HUP1\n";
  for (const auto& layout : layouts)
    excessive += layout + " red\n";
  std::ofstream(history) << excessive;
  owner = std::make_unique<LayoutControl>(file.string(), "reference:985", publisher);
  poll();
  CHECK(read(file.string() + ".status").find("Invalid private cluster colour history") !=
        std::string::npos);
}

int main(int argc, char* argv[]) {
  try {
    char name[] = "/tmp/headunit-layout-control-XXXXXX";
    CHECK(mkdtemp(name));
    const fs::path folder(name), file = folder / "choice", status = folder / "choice.status";
    struct Cleanup {
      fs::path p;
      ~Cleanup() { fs::remove_all(p); }
    } cleanup{folder};
    LayoutControl control(file.string(), {}, publisher);
    auto now = LayoutControl::Clock::time_point(1s);
    LayoutSelection state;
    state.ready = true;
    state.idle = true;
    state.active = a;
    state.choices = {{a, "A"}, {b, "B"}, {c, "C"}};
    std::vector<std::string> requests;
    auto select = [&](std::optional<std::string_view> request) {
      auto result = state;
      if (request) {
        requests.emplace_back(*request);
        result.result =
            *request == state.active ? LayoutRequest::selected : LayoutRequest::requested;
      }
      return result;
    };
    auto poll = [&] {
      now += 250ms;
      control.wait_for_preferences();
      control.poll(select, now);
      control.wait_for_preferences();
    };
    auto write = [&](std::string_view id) {
      const auto next = file.string() + ".new";
      std::ofstream(next) << request(id);
      fs::permissions(next, fs::perms::owner_read | fs::perms::owner_write);
      fs::rename(next, file);
    };
    poll();
    CHECK(requests.empty() && read(status).find(a) != std::string::npos);
    state.ready = false;
    write(b);
    poll();
    CHECK(requests.empty());
    state.ready = true;
    poll();
    CHECK(requests == std::vector<std::string>{b});
    for (unsigned i = 0; i < 4; ++i) {
      poll();
    }
    CHECK(requests.size() == 1);  // No duplicate request before phone confirmation.
    write(c);
    poll();
    CHECK(requests.size() == 1);  // Rapid buttons coalesce behind the current transition.
    state.active = b;
    poll();
    CHECK(requests == std::vector<std::string>({b, c}));
    state.active = c;
    poll();
    CHECK(read(status).find("\"pending\":\"\"") != std::string::npos);
    state.transitioning = true;
    write(a);
    poll();
    CHECK(requests.size() == 2);
    state.transitioning = false;
    poll();
    CHECK(requests.back() == a);
    now += 9s;
    poll();
    CHECK(read(status).find("not confirmed") != std::string::npos);
    poll();
    CHECK(requests.size() == 3);
    write(a);
    poll();
    CHECK(requests.size() == 4);  // Clicking again retries a timed-out choice.
    state.active = a;
    poll();
    write("not a layout");
    poll();
    CHECK(requests.size() == 4 && read(status).find("Invalid") != std::string::npos);
    fs::remove(file);
    fs::create_symlink(status, file);
    poll();
    CHECK(requests.size() == 4);
    fs::remove(file);
    write(b);
    fs::permissions(file, fs::perms::others_write, fs::perm_options::add);
    poll();
    CHECK(requests.size() == 4);  // Do not accept a writable file from another process user.
    fs::remove(file);
    state.ready = false;
    write(b);
    poll();
    write("not a layout");
    poll();
    state.ready = true;
    poll();
    CHECK(requests.size() == 4);  // A malformed replacement cancels a queued selection.
    // Reconnect preferences are pack-scoped confirmations, not the last click.
    fs::remove(file);
    state.ready = true;
    state.transitioning = false;
    state.active = a;
    requests.clear();
    const auto preference = file.string() + ".confirmed.reference:985";
    LayoutControl persistent(file.string(), "reference:985", publisher);
    auto persistent_poll = [&] {
      now += 250ms;
      persistent.wait_for_preferences();
      persistent.poll(select, now);
      persistent.wait_for_preferences();
    };
    write(b);
    persistent_poll();
    CHECK(requests == std::vector<std::string>{b} && !fs::exists(preference));
    state.active = b;
    state.ready = false;
    persistent_poll();
    CHECK(!fs::exists(preference) &&
          read(status).find(std::string("\"pending\":\"") + b + '"') != std::string::npos);
    CHECK(read(status).find(std::string("\"confirmedActive\":\"") + a + '"') != std::string::npos);
    state.ready = true;
    state.transitioning = true;
    persistent_poll();
    CHECK(!fs::exists(preference));
    state.transitioning = false;
    state.idle = false;
    persistent_poll();
    CHECK(!fs::exists(preference));
    CHECK(read(status).find(std::string("\"confirmedActive\":\"") + a + '"') != std::string::npos);
    state.idle = true;
    state.transitioning = false;
    persistent_poll();
    CHECK(read(preference) == std::string(b) + '\n');
    CHECK((fs::status(preference).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write));
    write(c);
    persistent_poll();
    CHECK(requests.back() == c);
    now += 9s;
    persistent_poll();
    CHECK(read(preference) == std::string(b) + '\n');
    const auto count = requests.size();
    state.active = a;
    state.idle = false;
    LayoutControl reconnected(file.string(), "reference:985", publisher);
    now += 250ms;
    reconnected.wait_for_preferences();
    reconnected.poll(select, now);
    reconnected.wait_for_preferences();
    CHECK(requests.size() == count &&
          read(status).find("\"confirmedActive\":\"\"") != std::string::npos);
    state.idle = true;
    now += 250ms;
    reconnected.wait_for_preferences();
    reconnected.poll(select, now);
    reconnected.wait_for_preferences();
    CHECK(requests.size() == count + 1 &&
          requests.back() == b);  // Restore B; never retry failed C on VM restart.
    state.active = b;
    now += 250ms;
    reconnected.wait_for_preferences();
    reconnected.poll(select, now);
    reconnected.wait_for_preferences();
    now += 250ms;
    reconnected.wait_for_preferences();
    reconnected.poll(select, now);
    reconnected.wait_for_preferences();
    CHECK(requests.size() == count + 1);
    // A different pack/version does not inherit either the old request or preference.
    LayoutControl other(file.string(), "single:547", publisher);
    now += 250ms;
    other.wait_for_preferences();
    other.poll(select, now);
    other.wait_for_preferences();
    CHECK(requests.size() == count + 1 && !fs::exists(file.string() + ".confirmed.single:547"));
    write(c);
    now += 250ms;
    reconnected.wait_for_preferences();
    reconnected.poll(select, now);
    reconnected.wait_for_preferences();
    CHECK(requests.back() == c);
    state.active = c;
    now += 250ms;
    reconnected.wait_for_preferences();
    reconnected.poll(select, now);
    reconnected.wait_for_preferences();
    CHECK(read(preference) == std::string(c) + '\n');
    // Unknown saved choices leave the working layout and previous preference intact.
    state.active = a;
    LayoutControl unknown(file.string(), "reference:985", publisher);
    const auto reject = [&](std::optional<std::string_view> request) {
      auto result = state;
      if (request) result.result = LayoutRequest::unknown;
      return result;
    };
    now += 250ms;
    unknown.wait_for_preferences();
    unknown.poll(reject, now);
    unknown.wait_for_preferences();
    CHECK(read(status).find("not declared") != std::string::npos &&
          read(preference) == std::string(c) + '\n');
    // Rejected restore preserves the old preference until the phone actually
    // changes to another supported choice; Customize must remain usable.
    now += 250ms;
    unknown.poll(reject, now);
    unknown.wait_for_preferences();
    CHECK(read(preference) == std::string(c) + '\n');
    state.active = b;
    state.ready = false;
    now += 250ms;
    unknown.poll(reject, now);
    unknown.wait_for_preferences();
    CHECK(read(preference) == std::string(c) + '\n');
    state.ready = true;
    state.transitioning = true;
    now += 250ms;
    unknown.poll(reject, now);
    unknown.wait_for_preferences();
    CHECK(read(preference) == std::string(c) + '\n');
    state.transitioning = false;
    now += 250ms;
    unknown.poll(reject, now);
    unknown.wait_for_preferences();
    CHECK(read(preference) == std::string(b) + '\n');
    // A transient storage failure must not discard an accepted phone choice.
    // Force atomic rename failure without depending on user/root permissions.
    const auto retry_file = folder / "retry", retry_status = folder / "retry.status";
    const auto retry_preference = folder / "retry.confirmed.reference:985";
    state.active = a;
    requests.clear();
    LayoutControl retry(retry_file.string(), "reference:985", publisher);
    const auto retry_poll = [&] {
      now += 250ms;
      retry.wait_for_preferences();
      retry.poll(select, now);
      retry.wait_for_preferences();
    };
    retry_poll();
    std::ofstream(retry_file) << request(b);
    fs::permissions(retry_file, fs::perms::owner_read | fs::perms::owner_write);
    retry_poll();
    CHECK(requests == std::vector<std::string>{b});
    fs::create_directory(retry_preference);
    state.active = b;
    retry_poll();
    retry_poll();  // Cached disk outcome is published on the next renderer poll.
    CHECK(read(retry_status).find("Cannot save confirmed") != std::string::npos);
    fs::remove(retry_preference);
    retry_poll();
    CHECK(!fs::exists(retry_preference));  // Failed writes retry at most once a second.
    std::ofstream(retry_file.string() + ".new") << request(c);
    fs::permissions(retry_file.string() + ".new", fs::perms::owner_read | fs::perms::owner_write);
    fs::rename(retry_file.string() + ".new", retry_file);
    retry_poll();
    CHECK(!fs::exists(retry_preference) && requests == std::vector<std::string>({b, c}));
    state.ready = false;
    now += 2s;
    retry_poll();
    CHECK(!fs::exists(retry_preference));
    state.ready = true;
    state.idle = false;
    retry_poll();
    CHECK(!fs::exists(retry_preference));
    state.idle = true;
    state.choices = {{a, "A"}, {c, "C"}};
    retry_poll();
    CHECK(!fs::exists(retry_preference));
    state.choices = {{a, "A"}, {b, "B"}, {c, "C"}};
    retry_poll();
    CHECK(read(retry_preference) == std::string(b) + '\n');
    retry_poll();
    CHECK(read(retry_status).find("\"preferenceError\":\"\"") != std::string::npos &&
          requests.size() == 2);
    state.active = c;
    retry_poll();
    CHECK(read(retry_preference) == std::string(c) + '\n');
    // A failed save cannot later retry a palette withdrawn from the catalogue,
    // even when the signed scene still reports that old palette as active.
    const auto palette_file = folder / "palette-retry";
    const auto palette_preference = folder / "palette-retry.confirmed.reference:985";
    LayoutControl palette_retry(palette_file.string(), "reference:985", publisher);
    state.active = a;
    state.active_palette = "red";
    state.palettes = {{"red", "Red"}, {"blue", "Blue"}};
    const auto palette_poll = [&] {
      now += 250ms;
      palette_retry.wait_for_preferences();
      palette_retry.poll(select, now);
      palette_retry.wait_for_preferences();
    };
    palette_poll();
    std::ofstream(palette_file) << request(b);
    fs::permissions(palette_file, fs::perms::owner_read | fs::perms::owner_write);
    palette_poll();
    fs::create_directory(palette_preference);
    state.active = b;
    palette_poll();
    fs::remove(palette_preference);
    state.palettes = {{"blue", "Blue"}};
    now += 2s;
    palette_poll();
    CHECK(!fs::exists(palette_preference));
    state.palettes.push_back({"red", "Red"});
    palette_poll();
    CHECK(read(palette_preference) == std::string(b) + "\nred\n");
    state.active_palette.clear();
    state.palettes.clear();
    // Readiness/idle alone cannot confirm a layout removed from the catalogue.
    const auto invalid_file = folder / "invalid-catalogue";
    LayoutControl invalid_catalogue(invalid_file.string(), "reference:985", publisher);
    state.active = a;
    requests.clear();
    std::ofstream(invalid_file) << request(c);
    fs::permissions(invalid_file, fs::perms::owner_read | fs::perms::owner_write);
    now += 250ms;
    invalid_catalogue.wait_for_preferences();
    invalid_catalogue.poll(select, now);
    invalid_catalogue.wait_for_preferences();
    CHECK(requests == std::vector<std::string>{c});
    state.active = c;
    state.choices = {{a, "A"}, {b, "B"}};
    now += 250ms;
    invalid_catalogue.wait_for_preferences();
    invalid_catalogue.poll(select, now);
    invalid_catalogue.wait_for_preferences();
    CHECK(!fs::exists(invalid_file.string() + ".confirmed.reference:985"));
    now += 9s;
    invalid_catalogue.wait_for_preferences();
    invalid_catalogue.poll(select, now);
    invalid_catalogue.wait_for_preferences();
    CHECK(read(invalid_file.string() + ".status").find("not confirmed") != std::string::npos);
    state.choices = {{a, "A"}, {b, "B"}, {c, "C"}};
    // An obsolete live request/highlight must not invalidate the native selector's
    // entire status, or block the latest declared choice behind an eight-second wait.
    const auto catalogue_file = folder / "catalogue",
               catalogue_status = folder / "catalogue.status";
    const auto catalogue_preference = folder / "catalogue.confirmed.reference:985";
    std::ofstream(catalogue_preference) << b << '\n';
    fs::permissions(catalogue_preference, fs::perms::owner_read | fs::perms::owner_write);
    LayoutControl catalogue(catalogue_file.string(), "reference:985", publisher);
    state.active = a;
    state.ready = true;
    state.idle = true;
    state.transitioning = false;
    requests.clear();
    const auto catalogue_poll = [&] {
      now += 250ms;
      catalogue.wait_for_preferences();
      catalogue.poll(select, now);
      catalogue.wait_for_preferences();
    };
    const auto evidence = [&] {
      catalogue_poll();  // Publish the asynchronous storage outcome.
      if (argc == 2 && std::string_view(argv[1]) == "--selector-evidence")
        std::cout << read(catalogue_status);
    };
    catalogue_poll();
    CHECK(requests == std::vector<std::string>{b});
    std::ofstream(catalogue_file) << request(c);
    fs::permissions(catalogue_file, fs::perms::owner_read | fs::perms::owner_write);
    state.choices = {{a, "A"}, {c, "C"}};
    state.ready = false;
    catalogue_poll();
    CHECK(read(catalogue_status).find(std::string("\"pending\":\"") + b + '"') !=
          std::string::npos);
    state.ready = true;
    state.idle = false;
    state.transitioning = true;
    catalogue_poll();
    CHECK(requests == std::vector<std::string>{b} &&
          read(catalogue_preference) == std::string(b) + '\n');
    CHECK(read(catalogue_status).find("\"pending\":\"\"") != std::string::npos);
    CHECK(read(catalogue_status).find("not declared") != std::string::npos);
    evidence();
    state.transitioning = false;
    now += 9s;
    catalogue_poll();
    CHECK(requests == std::vector<std::string>{b} &&
          read(catalogue_status).find("not confirmed") == std::string::npos);
    state.idle = true;
    catalogue_poll();
    CHECK(requests == std::vector<std::string>({b, c}));
    evidence();
    CHECK(read(catalogue_preference) == std::string(b) + '\n');
    state.active = c;
    state.choices = {{c, "C"}};
    state.idle = false;
    state.transitioning = true;
    catalogue_poll();
    CHECK(read(catalogue_status).find("\"confirmedActive\":\"\"") != std::string::npos);
    evidence();
    CHECK(read(catalogue_preference) == std::string(b) + '\n');
    state.idle = true;
    state.transitioning = false;
    catalogue_poll();
    CHECK(read(catalogue_preference) == std::string(c) + '\n');
    evidence();
    now += 9s;
    catalogue_poll();
    CHECK(requests == std::vector<std::string>({b, c}));
    state.choices = {{a, "A"}, {b, "B"}, {c, "C"}};
    // Untrusted preference files are ignored and diagnosed, never followed.
    fs::remove(preference);
    fs::create_symlink(status, preference);
    LayoutControl unsafe(file.string(), "reference:985", publisher);
    now += 250ms;
    unsafe.wait_for_preferences();
    unsafe.poll(select, now);
    unsafe.wait_for_preferences();
    CHECK(read(status).find("Cannot read saved") != std::string::npos);
    fs::remove(preference);
    std::ofstream(preference) << b << '\n';
    fs::permissions(preference, fs::perms::owner_all | fs::perms::others_read);
    LayoutControl exposed(file.string(), "reference:985", publisher);
    now += 250ms;
    exposed.wait_for_preferences();
    exposed.poll(select, now);
    exposed.wait_for_preferences();
    CHECK(read(status).find("Invalid private saved") != std::string::npos);
    bool invalid_scope = false;
    try {
      LayoutControl bad(file.string(), "../another-pack", publisher);
    } catch (const std::invalid_argument&) {
      invalid_scope = true;
    }
    CHECK(invalid_scope);
    // A delayed selector request cannot cross a receiver launch or connection.
    const auto bound_file = folder / "bound", bound_status = folder / "bound.status";
    auto owner = std::make_unique<LayoutControl>(bound_file.string(), "", publisher);
    state.active = a;
    state.ready = true;
    state.idle = true;
    state.transitioning = false;
    requests.clear();
    auto bound_poll = [&] {
      now += 250ms;
      owner->wait_for_preferences();
      owner->poll(select, now);
      owner->wait_for_preferences();
    };
    auto bound_write = [&](std::string_view bytes) {
      std::ofstream(bound_file.string() + ".new") << bytes;
      fs::permissions(bound_file.string() + ".new", fs::perms::owner_read | fs::perms::owner_write);
      fs::rename(bound_file.string() + ".new", bound_file);
    };
    bound_poll();
    CHECK(read(bound_status).find("\"nonce\":\"" + publisher.nonce + '"') != std::string::npos);
    CHECK(read(bound_status).find("\"connectionGeneration\":\"1\"") != std::string::npos);
    bound_write("HUL1 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1\n" + std::string(b) + "\n");
    bound_poll();
    CHECK(read(bound_status).find("\"error\":\"\"") != std::string::npos);
    bound_write("HUL1 " + publisher.nonce + " 2\n" + std::string(b) + "\n");
    bound_poll();
    CHECK(read(bound_status).find("\"error\":\"\"") != std::string::npos);
    bound_write("HUL2 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1\n" + std::string(b) + "\nred\n\n\n");
    bound_poll();
    CHECK(read(bound_status).find("\"error\":\"\"") != std::string::npos);
    CHECK(requests.empty());
    // Foreign identity does not hide a malformed payload or header.
    bound_write("HUL2 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1\n" + std::string(b) + "\nred bad\n\n\n");
    bound_poll();
    CHECK(read(bound_status).find("Invalid cluster selection") != std::string::npos);
    bound_write("HUL1 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 18446744073709551616\n" + std::string(b) +
                "\n");
    bound_poll();
    CHECK(read(bound_status).find("Invalid cluster selection") != std::string::npos);
    bound_write(std::string(b) + "\n");
    bound_poll();
    CHECK(read(bound_status).find("Invalid cluster selection") != std::string::npos);
    CHECK(requests.empty());
    bound_write(request(b));
    bound_poll();
    CHECK(requests == std::vector<std::string>{b});
    CHECK(read(bound_status).find("\"error\":\"\"") != std::string::npos);
    // Restart without persistence consumes no prior button request.
    owner.reset();
    CHECK(fs::exists(bound_status));  // Retained bytes have no current manager authority.
    owner = std::make_unique<LayoutControl>(bound_file.string(), "", publisher);
    bound_poll();
    CHECK(requests.size() == 1);
    auto successor = std::make_unique<LayoutControl>(bound_file.string(), "",
                                                     LayoutControl::Publisher{publisher.nonce, 2});
    now += 250ms;
    successor->wait_for_preferences();
    successor->poll(select, now);
    successor->wait_for_preferences();
    const auto successor_publication = read(bound_status);
    CHECK(successor_publication.find("\"error\":\"\"") != std::string::npos);
    CHECK(successor_publication.find("\"pending\":\"\"") != std::string::npos);
    CHECK(requests.size() == 1);
    now += 1s;
    successor->wait_for_preferences();
    successor->poll(select, now);
    successor->wait_for_preferences();
    CHECK(read(bound_status).find("\"error\":\"\"") != std::string::npos);
    CHECK(requests.size() == 1);
    const auto latest_successor_publication = read(bound_status);
    owner.reset();
    CHECK(read(bound_status) ==
          latest_successor_publication);  // Retirement never erases a successor.
    CHECK(read(bound_status).find("\"connectionGeneration\":\"2\"") != std::string::npos);
    bound_write(request(c));
    now += 250ms;
    successor->wait_for_preferences();
    successor->poll(select, now);
    successor->wait_for_preferences();
    CHECK(requests.size() == 1);
    successor.reset();
    CHECK(fs::exists(bound_status));  // A dated observation expires; retirement cannot refresh it.
    const auto appearance_file = folder / "appearance";
    LayoutControl appearance(appearance_file.string(), "", publisher);
    state.palettes = {{"blue", "Blue"}, {"red", "Red"}};
    state.active_palette = "blue";
    state.contents = {{b, c, "Map", false}};
    unsigned appearance_requests = 0;
    const auto choose_appearance = [&](const std::optional<ClusterSelectionRequest>& request) {
      auto result = state;
      if (request) {
        CHECK(request->layout == a && request->palette == "red" && request->zone == b &&
              request->component == c);
        ++appearance_requests;
        result.result = LayoutRequest::requested;
      }
      return result;
    };
    const auto appearance_poll = [&] {
      now += 250ms;
      appearance.wait_for_preferences();
      appearance.poll_cluster(choose_appearance, now);
      appearance.wait_for_preferences();
    };
    const auto appearance_status = appearance_file.string() + ".status";
    state.contents[0].available = false;
    appearance_poll();
    CHECK(read(appearance_status).find("\"available\":false") != std::string::npos);
    state.contents[0].available = true;
    std::ofstream(appearance_file) << "HUL2 " << publisher.nonce << " 1\n"
                                   << a << "\nred\n"
                                   << b << '\n'
                                   << c << '\n';
    fs::permissions(appearance_file, fs::perms::owner_read | fs::perms::owner_write);
    appearance_poll();
    CHECK(appearance_requests == 1);
    appearance_poll();
    CHECK(read(appearance_status).find("\"pending\":\"" + std::string(a)) != std::string::npos);
    state.active_palette = "red";
    appearance_poll();
    CHECK(read(appearance_status).find("\"pending\":\"" + std::string(a)) != std::string::npos);
    state.contents[0].active = true;
    state.contents[0].available = false;
    appearance_poll();
    CHECK(read(appearance_status).find("\"pending\":\"" + std::string(a)) != std::string::npos);
    state.contents[0].available = true;
    appearance_poll();
    CHECK(read(appearance_status).find("\"pending\":\"\"") != std::string::npos);
    CHECK(appearance_requests == 1);

    // Reproduce a phone accepting the style but keeping a different palette
    // and/or centre choice. Partial acceptance must not save a preference,
    // invent confirmation, or send the request again after timeout.
    const auto partial_file = folder / "partial";
    LayoutControl partial(partial_file.string(), "partial", publisher);
    const auto partial_status = partial_file.string() + ".status";
    const auto partial_preference = partial_file.string() + ".confirmed.partial";
    unsigned partial_requests = 0;
    auto partial_result = LayoutRequest::requested;
    const auto partial_select = [&](const std::optional<ClusterSelectionRequest>& request) {
      auto result = state;
      if (request) {
        ++partial_requests;
        result.result = partial_result;
      }
      return result;
    };
    const auto partial_poll = [&] {
      now += 250ms;
      partial.wait_for_preferences();
      partial.poll_cluster(partial_select, now);
      partial.wait_for_preferences();
    };
    const auto partial_write = [&] {
      const auto next = partial_file.string() + ".new";
      std::ofstream(next) << "HUL2 " << publisher.nonce << " 1\n"
                          << b << "\nred\n"
                          << b << '\n'
                          << c << '\n';
      fs::permissions(next, fs::perms::owner_read | fs::perms::owner_write);
      fs::rename(next, partial_file);
    };
    const auto partial_timeout = [&](std::string_view expected) {
      partial_write();
      partial_poll();
      const auto sent = partial_requests;
      now += 8s;
      partial_poll();
      CHECK(read(partial_status).find(expected) != std::string::npos);
      CHECK(read(partial_status).find("\"pending\":\"\"") != std::string::npos);
      CHECK(!fs::exists(partial_preference));
      partial_poll();
      CHECK(partial_requests == sent);
    };
    state.active = a;
    state.active_palette = "blue";
    state.contents[0].active = false;
    partial_write();
    partial_poll();
    state.active = b;
    now += 8s;
    partial_poll();
    CHECK(read(partial_status).find("colour and centre content were not confirmed") !=
          std::string::npos);
    CHECK(read(partial_status).find("\"confirmedActive\":\"" + std::string(b)) !=
          std::string::npos);
    CHECK(!fs::exists(partial_preference));
    partial_poll();
    CHECK(partial_requests == 1);
    state.contents[0].active = true;
    partial_timeout("colour was not confirmed. Change colours using Customize in CarPlay");
    state.active_palette = "red";
    state.contents[0].active = false;
    partial_timeout("centre content was not confirmed");
    state.active = a;
    partial_timeout("style was not confirmed");
    state.active = b;
    state.ready = false;
    partial_write();
    partial_poll();
    const auto before_ready = partial_requests;
    state.ready = true;
    partial_poll();
    CHECK(partial_requests == before_ready + 1);
    state.ready = false;
    now += 8s;
    partial_poll();
    CHECK(read(partial_status).find("style was not confirmed") != std::string::npos);
    state.ready = true;
    partial_result = LayoutRequest::unknown;
    partial_write();
    partial_poll();
    CHECK(read(partial_status).find("Cluster choice is not supported by this pack") !=
          std::string::npos);
    state.contents[0].active = true;
    partial_result = LayoutRequest::selected;
    partial_write();
    partial_poll();
    CHECK(read(partial_status).find("\"error\":\"\"") != std::string::npos);
    CHECK(read(partial_preference) == std::string(b) + "\nred\n");
    // Restore the confirmed palette as well as the layout, never content IDs.
    state.active = a;
    state.active_palette = "blue";
    LayoutControl colour_restore(partial_file.string(), "partial", publisher);
    unsigned colour_requests = 0;
    auto colour_select = [&](const std::optional<ClusterSelectionRequest>& choice) {
      auto result = state;
      if (choice) {
        ++colour_requests;
        CHECK(choice->layout == b && choice->palette == "red");
        CHECK(choice->zone.empty() && choice->component.empty());
        result.result = LayoutRequest::requested;
      }
      return result;
    };
    now += 250ms;
    colour_restore.wait_for_preferences();
    colour_restore.poll_cluster(colour_select, now);
    colour_restore.wait_for_preferences();
    CHECK(colour_requests == 1);
    state.active = b;
    state.active_palette = "red";
    now += 250ms;
    colour_restore.wait_for_preferences();
    colour_restore.poll_cluster(colour_select, now);
    colour_restore.wait_for_preferences();
    // A settled phone-side Customize choice is also remembered.
    state.active_palette = "blue";
    now += 250ms;
    colour_restore.wait_for_preferences();
    colour_restore.poll_cluster(colour_select, now);
    colour_restore.wait_for_preferences();
    CHECK(read(partial_preference) == std::string(b) + "\nblue\n");
    state.palettes = {{"red", "Red"}};
    LayoutControl removed_colour(partial_file.string(), "partial", publisher);
    now += 250ms;
    removed_colour.poll_cluster(
        [&](const auto& choice) {
          auto result = state;
          if (choice) {
            CHECK(choice->layout == b && choice->palette.empty());
            result.result = LayoutRequest::requested;
          }
          return result;
        },
        now);
    // Truncated, invalid and overlong records cannot cause substring underflow
    // or restore an unqualified request. Legacy layout-only records still work.
    for (const auto& raw :
         {std::string(b) + "\n\n", std::string(b) + "\nred", std::string(b) + "\nred bad\n",
          std::string(b) + "\n" + std::string(65, 'x') + "\n"}) {
      std::ofstream(partial_preference) << raw;
      LayoutControl corrupt(partial_file.string(), "partial", publisher);
      now += 250ms;
      corrupt.wait_for_preferences();
      corrupt.poll_cluster(
          [&](const auto& choice) {
            CHECK(!choice);
            corrupt.wait_for_preferences();
            return state;
          },
          now);
      CHECK(read(partial_status).find("Invalid private saved") != std::string::npos);
    }
    // Coalescing stays bounded; retirement commits the last accepted record.
    const auto async_path = folder / "async";
    {
      PreferenceFile store(async_path.string());
      store.wait();
      CHECK(store.snapshot().loaded && !store.snapshot().data);
      for (unsigned i = 0; i < 100; ++i)
        store.save(std::string(a) + "\nred\n");
      const auto sequence = store.save(std::string(b) + "\nblue\n");
      store.wait();
      CHECK(store.snapshot().completed == sequence && store.snapshot().error.empty());
      CHECK(read(async_path) == std::string(b) + "\nblue\n");
      store.save(std::string(c) + "\nred\n");
    }
    CHECK(read(async_path) == std::string(c) + "\nred\n");
    // A redirected directory is rejected without touching its replacement.
    const auto original_parent = folder / "store-parent";
    fs::create_directory(original_parent);
    const auto moved_parent = folder / "store-parent-old";
    PreferenceFile pinned((original_parent / "record").string());
    pinned.wait();
    fs::rename(original_parent, moved_parent);
    fs::create_directory(original_parent);
    std::ofstream(original_parent / "sentinel") << "preserved";
    pinned.save(std::string(a) + "\n");
    pinned.wait();
    CHECK(!pinned.snapshot().error.empty());
    CHECK(!fs::exists(original_parent / "record") && !fs::exists(moved_parent / "record"));
    CHECK(read(original_parent / "sentinel") == "preserved");
    palette_history(folder);
    std::cout << "layout control: confirmation, coalescing, timeout, scoped preferences and "
                 "private files passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
