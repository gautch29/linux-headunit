// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/cluster_return.hpp>
#include <headunit/presentation_feedback_timeout.hpp>
#include <headunit/theme/local_cluster_selection.hpp>
#include <headunit/theme/preference_file.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>

using namespace headunit;
using namespace std::chrono_literals;
namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("cluster return check failed");
}
const auto epoch = Clock::time_point{} + 10s;  // Explicit simulated owner dates.
struct Fixture {
  Clock::time_point now = epoch;
  bool stopped = false;
  unsigned local_runs = 0, native_runs = 0, live = 0, failures = 0;
  std::vector<std::string> order;
  struct Presenter {
    Fixture& owner;
    std::string name;
    Presenter(Fixture& value, std::string kind) : owner(value), name(std::move(kind)) {
      check(owner.live++ == 0);
      owner.order.push_back(name + " start");
    }
    ~Presenter() {
      --owner.live;
      owner.order.push_back(name + " retire");
    }
  };
  void run(const std::function<void()>& local,
           const std::function<void(ClusterReturn&, std::uint64_t)>& native,
           bool enable_output_retry = true) {
    run_cluster_presenters(
        [&] {
          ++local_runs;
          Presenter presenter(*this, "local");
          local();
        },
        [&](ClusterReturn& recovery, std::uint64_t generation) {
          ++native_runs;
          Presenter presenter(*this, "native");
          native(recovery, generation);
        },
        [&] { return now; }, [&] { return stopped; },
        [&](std::string_view error, bool) {
          check(live == 0 && !error.empty());
          ++failures;
        },
        enable_output_retry);
    check(live == 0);
  }
  void eligible(ClusterReturn& recovery, std::uint64_t generation) {
    now += 1s;
    recovery.committed(generation, 1, now);
    now += 20ms;
    recovery.frame_done(generation, 1, now);
  }
};
void callback_guards() {
  ClusterReturn recovery;
  auto generation = recovery.begin_native(epoch, true);
  bool overlap = false;
  try {
    (void)recovery.begin_native(epoch, true);
  } catch (const std::logic_error&) {
    overlap = true;
  }
  check(overlap);
  // Cover/absence, an old submission exposed late, foreign and future callbacks
  // cannot grant retry. A submitted buffer is not itself a callback or receipt.
  recovery.committed(generation, 1, epoch);
  check(!recovery.request_return(epoch + 5s));
  recovery.frame_done(generation, 1, epoch + 2s);
  check(!recovery.request_return(epoch + 2s));
  recovery.committed(generation, 2, epoch + 2s);
  recovery.frame_done(generation + 1, 2, epoch + 2s);
  recovery.frame_done(generation, 1, epoch + 2s);
  recovery.frame_done(generation, 2, epoch + 1s);
  check(!recovery.request_return(epoch + 2s));
  recovery.frame_done(generation, 2, epoch + 2100ms);
  check(!recovery.request_return(epoch + 2099ms));
  check(!recovery.request_return(epoch + 2350ms));     // Exact freshness boundary.
  recovery.frame_done(generation, 2, epoch + 2400ms);  // Duplicate never renews.
  check(!recovery.request_return(epoch + 2400ms));
  recovery.committed(generation, 3, epoch + 3s);
  recovery.frame_done(generation, 3, epoch + 3020ms);
  check(recovery.request_return(epoch + 3020ms));
  check(!recovery.request_return(epoch + 3020ms));
  check(recovery.retire_native());
  recovery.frame_done(generation, 3, epoch + 4s);
  check(!recovery.request_return(epoch + 4s));
  auto successor = recovery.begin_native(epoch + 4s, true);
  recovery.committed(successor, 1, epoch + 5s);
  recovery.frame_done(generation, 1, epoch + 5s);
  recovery.frame_done(successor, 1, epoch + 5s);
  check(!recovery.request_return(epoch + 5s));  // One retry for this lifetime.
  check(!recovery.retire_native());
}
void default_behavior() {
  Fixture fixture;
  fixture.run([] { throw PresentationFeedbackTimeout(); },
              [&](ClusterReturn& recovery, std::uint64_t generation) {
                fixture.eligible(recovery, generation);
                check(!recovery.request_return(fixture.now));
              },
              false);
  check(fixture.local_runs == 1 && fixture.native_runs == 1 && fixture.failures == 1);
}
void aborted_return_preserves_owner() {
  Fixture fixture;
  fixture.run([] { throw PresentationFeedbackTimeout(); },
              [&](ClusterReturn& recovery, std::uint64_t generation) {
                fixture.eligible(recovery, generation);
                check(recovery.request_return(fixture.now));
                recovery.abort_return(generation + 1);  // Foreign abort cannot disarm this attempt.
                recovery.abort_return(generation);
                for (unsigned token = 2; token < 1000; ++token) {
                  fixture.now += 1s;
                  recovery.committed(generation, token, fixture.now);
                  recovery.frame_done(generation, token, fixture.now);
                  check(!recovery.request_return(fixture.now));
                }
              });
  check(fixture.local_runs == 1 && fixture.native_runs == 1);
}
void return_and_exhaustion() {
  Fixture fixture;
  fixture.run(
      [&] {
        if (fixture.local_runs == 1) throw PresentationFeedbackTimeout();
      },
      [&](ClusterReturn& recovery, std::uint64_t generation) {
        fixture.eligible(recovery, generation);
        check(recovery.request_return(fixture.now));
      });
  check(fixture.local_runs == 2 && fixture.native_runs == 1 && fixture.failures == 1);
  check(fixture.order == std::vector<std::string>{"local start", "local retire", "native start",
                                                  "native retire", "local start", "local retire"});
  Fixture repeated;
  repeated.run([] { throw PresentationFeedbackTimeout(); },
               [&](ClusterReturn& recovery, std::uint64_t generation) {
                 repeated.eligible(recovery, generation);
                 check(recovery.request_return(repeated.now) == (repeated.native_runs == 1));
                 // Continuous real callback progress cannot reset the exhausted budget.
                 for (unsigned token = 2; token < 1000; ++token) {
                   repeated.now += 1s;
                   recovery.committed(generation, token, repeated.now);
                   recovery.frame_done(generation, token, repeated.now);
                   check(!recovery.request_return(repeated.now));
                 }
               });
  check(repeated.local_runs == 2 && repeated.native_runs == 2 && repeated.failures == 2);
}
void permanent_faults_and_retirement() {
  for (const auto* fault : {"invalid pack", "GPU completion", "VM fault", "lost output"}) {
    Fixture fixture;
    fixture.run([&] { throw std::runtime_error(fault); },
                [&](ClusterReturn& recovery, std::uint64_t generation) {
                  fixture.eligible(recovery, generation);
                  check(!recovery.request_return(fixture.now));
                });
    check(fixture.local_runs == 1 && fixture.native_runs == 1);
  }
  Fixture covered;
  covered.run([] { throw PresentationFeedbackTimeout(); },
              [&](ClusterReturn& recovery, std::uint64_t generation) {
                // A live output/socket/redraw with no callback retains native.
                for (unsigned token = 1; token < 1000; ++token) {
                  covered.now += 1s;
                  recovery.committed(generation, token, covered.now);
                  check(!recovery.request_return(covered.now));
                }
              });
  check(covered.local_runs == 1);
  Fixture terminal;
  terminal.run([] { throw PresentationFeedbackTimeout(); },
               [&](ClusterReturn& recovery, std::uint64_t generation) {
                 terminal.eligible(recovery, generation);
                 check(recovery.request_return(terminal.now));
                 terminal.stopped = true;  // Shutdown wins even after eligibility.
               });
  check(terminal.local_runs == 1);
  Fixture local_stop;
  local_stop.run(
      [&] {
        local_stop.stopped = true;
        throw PresentationFeedbackTimeout();
      },
      [](ClusterReturn&, std::uint64_t) { check(false); });
  check(local_stop.native_runs == 0);
  Fixture output_loss;
  bool failed = false;
  try {
    output_loss.run([] { throw PresentationFeedbackTimeout(); },
                    [&](ClusterReturn& recovery, std::uint64_t generation) {
                      output_loss.eligible(recovery, generation);
                      check(recovery.request_return(output_loss.now));
                      throw std::runtime_error("native output loss during retirement");
                    });
  } catch (const std::exception&) {
    failed = true;
  }
  check(failed && output_loss.local_runs == 1 && output_loss.live == 0);
}
constexpr auto first = "00000000-1111-2222-3333-444444444444";
constexpr auto second = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
headunit::theme::script::ResourceReader resources() {
  return [](std::string_view name) -> std::string {
    if (name == "r/PackageData.lua")
      return R"(return {displays={{id='Cluster_Display',layouts={
      {id='00000000-1111-2222-3333-444444444444',layoutType='primary'},
      {id='AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE',layoutType='primary'}}}}})";
    if (name == "r/ThemeData.lua") return "return {palettes={}}";
    throw std::runtime_error("controller must not run during selection admission");
  };
}
void changed_preference_on_return() {
  using namespace headunit::theme::script;
  char name[] = "/tmp/headunit-cluster-return-XXXXXX";
  check(mkdtemp(name) != nullptr);
  const auto directory = std::filesystem::path(name);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  const auto path = (directory / "confirmed").string();
  PreferenceFile writer(path);
  writer.wait();
  writer.save(std::string(first) + '\n');
  writer.wait();
  Fixture fixture;
  fixture.run(
      [&] {
        PreferenceFile reader(path);  // Same production preload as each local entry.
        reader.wait();
        auto record = reader.snapshot();
        check(record.loaded && record.error.empty() && record.data);
        auto selection = local_cluster_selection(resources(), first, *record.data);
        check(selection.layout == (fixture.local_runs == 1 ? first : second));
        if (fixture.local_runs == 1) throw PresentationFeedbackTimeout();
      },
      [&](ClusterReturn& recovery, std::uint64_t generation) {
        writer.save(std::string(second) + '\n');  // New actual atomic private input.
        writer.wait();
        fixture.eligible(recovery, generation);
        check(recovery.request_return(fixture.now));
      });
  check(fixture.local_runs == 2 && writer.snapshot().data == std::string(second) + '\n');
  // Admission faults on the successor never create a third presenter.
  Fixture invalid;
  invalid.run(
      [&] {
        if (invalid.local_runs == 1) throw PresentationFeedbackTimeout();
        (void)local_cluster_selection(resources(), "undeclared", std::nullopt);
      },
      [&](ClusterReturn& recovery, std::uint64_t generation) {
        invalid.eligible(recovery, generation);
        check(recovery.request_return(invalid.now) == (invalid.native_runs == 1));
      });
  check(invalid.local_runs == 2 && invalid.native_runs == 2);
}
}  // namespace
int main() try {
  callback_guards();
  default_behavior();
  aborted_return_preserves_owner();
  return_and_exhaustion();
  permanent_faults_and_retirement();
  changed_preference_on_return();
  std::cout
      << "production cluster owner: bounded return, admission, freshness and retirement passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
