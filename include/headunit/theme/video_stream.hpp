// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/scene.hpp>
#include <headunit/theme/native_frame.hpp>
#include <deque>

namespace headunit::theme::video {
// Values observed in the installed pack's InputStream/LayoutController ABI.
// These describe a local host input, not an AirPlay service/resource ID.
enum class InputType : std::uint8_t {
  camera = 0,
  settings = 1,
  widget = 2,
  iphone_video = 3,
  road_sign = 4
};
struct Input {
  std::string identifier;
  std::uint32_t width = 0, height = 0;
  double frame_duration = 1.0 / 60;
  InputType type = InputType::iphone_video;
};
struct Memory {
  std::size_t bytes = 0, native_frames = 0;
  static constexpr std::size_t limit = 128 * 1024 * 1024, native_limit = 24;
};
struct Frame {
  std::uint64_t token;
  double pts;
  std::shared_ptr<const graphics::Image> image;
  std::shared_ptr<const NativeFrame> native;
};
// Bounded decoded frames, in the host's explicitly mapped presentation domain.
// prepare_next() leases distinct frames to at most two pending compositions.
// Only presented(token), after
// the compositor confirms it, advances the current timestamp. Rendering alone
// is not presentation. All calls run on the owning display thread.
class Stream {
 public:
  Stream(Input, std::shared_ptr<Memory>);
  const Input& input() const { return input_; }
  // False or allocation failure preserves the caller's image and stream state.
  bool offer(double pts, graphics::Image&&);
  // Immutable producer backing; no full-frame copy. The shared owner must
  // retain its capture permit until the stream/GPU releases the image.
  bool offer_shared(double pts, const std::shared_ptr<const graphics::Image>&);
  bool offer_native(double pts, const std::shared_ptr<const NativeFrame>&);
  // Drop superseded queued frames whose newer replacement is already due.
  // Retain the newest due frame, all future frames, and any staged submission.
  // Does not advance current PTS or reuse a discarded token.
  std::size_t discard_obsolete(double due_pts);
  static constexpr std::size_t max_prepared = 2;
  static constexpr std::size_t max_pending = 3;
  bool can_offer_native() const;
  std::optional<double> next_unprepared_pts() const;
  std::optional<Frame> prepare_next();
  // Single-frame callers may retry the oldest outstanding preparation.
  std::optional<Frame> prepare();
  bool prepared(std::uint64_t token) const;
  void presented(std::uint64_t token);
  void cancel();  // Retain the queued frame for a later composition attempt.
  // Complete in submission order. If a newer preparation already exists,
  // discard the rejected older frame; it must never be staged behind it.
  void cancel(std::uint64_t token);
  // Ordered rejection without retrying the source frame. Local capture
  // queues discard a rejected lease rather than retaining it for a redraw.
  void discard_prepared(std::uint64_t token);
  std::optional<std::uint64_t> composition_token() const;
  // Drop displayed and buffered frames on a script-requested video reset.
  // Keep source PTS ordering and token uniqueness for subsequent input.
  void flush();
  // Host clock suspension: release current/staged/queued storage, preserving
  // source ordering and token uniqueness.
  void discard_all();
  void close();  // Terminal; replace the stream for a new negotiated session.
  bool closed() const { return closed_; }
  bool unused() const { return serial_ == 0; }
  std::optional<double> current_pts() const;
  std::optional<std::uint64_t> current_token() const {
    return current_ ? std::optional(current_->token) : std::nullopt;
  }
  // Current accepted source, independent of newer prepared compositions.
  std::optional<DecodedMetadata> current_native_metadata() const {
    return current_ && current_->native ? std::optional(current_->native->metadata) : std::nullopt;
  }
  std::optional<double> next_pts() const;
  std::shared_ptr<const graphics::Image> image() const;
  std::shared_ptr<const NativeFrame> native() const;

 private:
  Input input_;
  std::shared_ptr<Memory> memory_;
  std::deque<Frame> pending_;
  std::optional<Frame> current_;
  std::size_t prepared_count_ = 0;  // Protected prefix of pending_, bounded by max_prepared.
  std::optional<double> last_pts_;
  std::uint64_t serial_ = 0;
  bool closed_ = false;
  enum class Storage { unset, cpu, native } storage_ = Storage::unset;
};
enum class State { closed, subscribed, data_available, unsubscribed, invalid };
struct View {
  std::shared_ptr<Stream> stream;
  bool subscribed = false;
  bool active() const { return subscribed && stream && !stream->closed(); }
  State state() const;
  std::shared_ptr<const graphics::Image> image() const {
    return active() ? stream->image() : nullptr;
  }
  std::shared_ptr<const NativeFrame> native() const {
    return active() ? stream->native() : nullptr;
  }
};
}  // namespace headunit::theme::video
