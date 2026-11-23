// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/script_runtime.hpp>
#include <chrono>
#include <memory>
#include <headunit/theme/preference_file.hpp>

namespace headunit::theme::script {
// Optional local UI/button bridge. An atomic, private file contains the desired
// receiver launch/connection identity and layout UUID. The owner coalesces selections, sends one request at a time and
// publishes actual active state to FILE.status. No socket or VM pointer leaves
// this owner. An absent file preserves the signed pack's normal selection.
class LayoutControl {
 public:
  struct Publisher {
    std::string nonce;
    std::uint64_t connection_generation = 0;
  };
  using Clock = std::chrono::steady_clock;
  using Selection = std::function<LayoutSelection(std::optional<std::string_view>)>;
  // A nonempty verified pack scope opts in to remembering confirmed choices.
  // Stored selections are rechecked against the current catalogue and requested
  // through the phone session, never locally forced on a reconnect.
  explicit LayoutControl(std::string path, std::string preference_scope = {});
  LayoutControl(std::string path, std::string preference_scope, Publisher,
                std::shared_ptr<PreferenceFile> preferences = {});
  void wait_for_preferences();
  LayoutControl(const LayoutControl&) = delete;
  LayoutControl& operator=(const LayoutControl&) = delete;
  void poll(const Selection&, Clock::time_point);
  using ClusterSelection =
      std::function<LayoutSelection(const std::optional<ClusterSelectionRequest>&)>;
  void poll_cluster(const ClusterSelection&, Clock::time_point);

 private:
  struct Stamp {
    std::uint64_t device = 0, inode = 0, seconds = 0, nanoseconds = 0;
    bool operator==(const Stamp&) const = default;
  };
  std::string path_, desired_, pending_, error_;
  ClusterSelectionRequest desired_options_, pending_options_;
  std::string preference_path_, restoring_, confirmed_, confirmed_active_, preference_error_,
      confirmed_candidate_;
  // Palette saved with the layout; empty means the phone's own choice.
  std::string restoring_palette_, confirmed_palette_, candidate_palette_;
  std::string observed_layout_, observed_palette_;
  bool restore_settled_ = false;
  std::optional<Stamp> desired_stamp_, attempted_stamp_;
  Clock::time_point next_poll_{}, next_status_{}, pending_since_{}, next_preference_attempt_{};
  std::string last_status_;
  Publisher publisher_;
  std::uint64_t sequence_ = 0;
  std::shared_ptr<PreferenceFile> preferences_;
  std::shared_ptr<PreferenceFile> palette_preferences_;
  std::vector<std::pair<std::string, std::string>> palette_history_;
  bool palette_preferences_loaded_ = false;
  std::string palette_preference_error_, palette_candidate_layout_, palette_candidate_;
  std::uint64_t palette_revision_ = 0, saved_palette_revision_ = 0;
  std::uint64_t submitted_palette_preference_ = 0, submitted_palette_revision_ = 0;
  Clock::time_point next_palette_attempt_{};
  bool preferences_loaded_ = false;
  std::uint64_t submitted_preference_ = 0;
  std::string submitted_layout_, submitted_palette_;
  void refresh_preferences();
  void refresh_palette_preferences();
  void remember_palette(std::string_view layout, std::string_view palette);
  void persist_palette_history(Clock::time_point);
  void read_request();
  void remember_confirmed(std::string_view layout, std::string_view palette);
  void persist_confirmed(Clock::time_point);
  void publish(const LayoutSelection&, Clock::time_point);
};
}  // namespace headunit::theme::script
