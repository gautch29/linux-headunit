// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/airplay_session_setup.hpp>
#include <headunit/theme/ui_worker.hpp>
#include <optional>
#include <set>

namespace headunit::theme::script {
inline constexpr const char* ultra_ui_service = "07D9F906-8D64-4B54-A808-A20BCA2C51C2";

// Receiver-thread owner of one authenticated UI service generation at a time.
// Configuration factory must be bounded: verified resource snapshots can be
// captured here, but VM/GPU construction belongs in the worker's configure hook.
// Call stop before destroying the socket session. Retain/reap this owner only
// once finished() is true; the destructor joins during orderly final shutdown.
class UiSession {
 public:
  using Factory = std::function<UiWorker::Configuration()>;
  explicit UiSession(Factory);
  void attach(AirplaySessionSetup&);  // Before initial SETUP, once only.
  void pump(AirplaySessionSetup&);    // After drain(), bounded mailbox work.
  void stop();                        // Nonblocking; no more socket access.
  bool finished() const;
  std::optional<std::uint64_t> stream_id() const { return stream_id_; }
  std::optional<UiWorker::Status> status() const;
  const std::string& failure_reason() const { return failure_; }

 private:
  void retire();
  Factory factory_;
  AirplaySessionSetup* session_ =
      nullptr;  // Owner identity only, never dereferenced on destruction.
  std::unique_ptr<UiWorker> worker_;
  std::optional<std::uint64_t> stream_id_;
  std::set<std::uint64_t> attempted_;
  std::optional<std::chrono::steady_clock::time_point> blocked_since_;
  std::string failure_;
  bool stopped_ = false;
};
}  // namespace headunit::theme::script
