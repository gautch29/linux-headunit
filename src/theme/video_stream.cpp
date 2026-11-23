// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/video_stream.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace headunit::theme::video {
namespace {
void check(bool value, const char* reason) {
  if (!value) throw std::invalid_argument(reason);
}
}  // namespace
Stream::Stream(Input input, std::shared_ptr<Memory> memory)
    : input_(std::move(input)), memory_(std::move(memory)) {
  check(bool(memory_) && !input_.identifier.empty() && input_.identifier.size() <= 256 &&
            input_.identifier.find('\0') == std::string::npos,
        "invalid video input identity");
  check(std::uint8_t(input_.type) <= std::uint8_t(InputType::road_sign),
        "invalid local video input type");
  check(input_.width > 0 && input_.width <= 4096 && input_.height > 0 && input_.height <= 2160 &&
            std::isfinite(input_.frame_duration) && input_.frame_duration > 0 &&
            input_.frame_duration <= 1,
        "invalid video input dimensions or cadence");
}
bool Stream::offer(double pts, graphics::Image&& image) {
  check(!closed_, "video stream is closed");
  check(storage_ != Storage::native, "video input storage changed");
  check(std::isfinite(pts) && pts >= 0 && (!last_pts_ || pts > *last_pts_),
        "video frame timestamp is not increasing");
  check(image.width == input_.width && image.height == input_.height &&
            image.pixels.size() == std::size_t(image.width) * image.height,
        "video frame dimensions changed");
  const auto bytes = image.pixels.size() * sizeof(std::uint32_t);
  if (pending_.size() >= max_pending || bytes > Memory::limit - memory_->bytes) return false;
  check(serial_ < std::numeric_limits<std::uint64_t>::max(), "video frame sequence exhausted");
  // Reserve the queue slot before taking pixel ownership; allocation failure
  // must not silently consume a caller's frame during backpressure handling.
  pending_.push_back({serial_ + 1, pts, {}, {}});
  struct Lease {
    graphics::Image image;
    std::shared_ptr<Memory> memory;
    std::size_t bytes;
    Lease(graphics::Image&& pixels, std::shared_ptr<Memory> budget, std::size_t size) noexcept
        : image(std::move(pixels)), memory(std::move(budget)), bytes(size) {
      memory->bytes += bytes;
    }
    ~Lease() { memory->bytes -= bytes; }
  };
  try {
    // make_shared allocates the lease/control block before moving pixels. The
    // aliasing owner below does not allocate and preserves the backing lifetime.
    auto lease = std::make_shared<Lease>(std::move(image), memory_, bytes);
    const auto* pixels = &lease->image;
    pending_.back().image = std::shared_ptr<const graphics::Image>(std::move(lease), pixels);
  } catch (...) {
    pending_.pop_back();
    throw;
  }
  ++serial_;
  last_pts_ = pts;
  storage_ = Storage::cpu;
  return true;
}
bool Stream::offer_native(double pts, const std::shared_ptr<const NativeFrame>& frame) {
  check(!closed_, "video stream is closed");
  check(storage_ != Storage::cpu, "video input storage changed");
  check(std::isfinite(pts) && pts >= 0 && (!last_pts_ || pts > *last_pts_),
        "video frame timestamp is not increasing");
  check(frame && frame->lifetime && frame->metadata.width == input_.width &&
            frame->metadata.height == input_.height,
        "invalid native video input dimensions or lifetime");
  // CPU reference images have a byte budget; external decoder allocations have
  // an independent sample budget. The decoder also limits its outstanding leases.
  if (!can_offer_native()) return false;
  check(serial_ < std::numeric_limits<std::uint64_t>::max(), "video frame sequence exhausted");
  pending_.push_back({serial_ + 1, pts, {}, {}});
  struct Lease {
    std::shared_ptr<const NativeFrame> frame;
    std::shared_ptr<Memory> memory;
    Lease(std::shared_ptr<const NativeFrame> f, std::shared_ptr<Memory> m)
        : frame(std::move(f)), memory(std::move(m)) {
      ++memory->native_frames;
    }
    ~Lease() { --memory->native_frames; }
  };
  try {
    auto lease = std::make_shared<Lease>(frame, memory_);
    pending_.back().native = std::shared_ptr<const NativeFrame>(std::move(lease), frame.get());
  } catch (...) {
    pending_.pop_back();
    throw;
  }
  ++serial_;
  last_pts_ = pts;
  storage_ = Storage::native;
  return true;
}
bool Stream::offer_shared(double pts, const std::shared_ptr<const graphics::Image>& image) {
  check(!closed_, "video stream is closed");
  check(storage_ != Storage::native, "video input storage changed");
  check(std::isfinite(pts) && pts >= 0 && (!last_pts_ || pts > *last_pts_),
        "video frame timestamp is not increasing");
  check(image && image->width == input_.width && image->height == input_.height &&
            image->pixels.size() == std::size_t(image->width) * image->height,
        "invalid shared video input dimensions");
  const auto bytes = image->pixels.size() * sizeof(std::uint32_t);
  if (pending_.size() >= max_pending || bytes > Memory::limit - memory_->bytes) return false;
  check(serial_ < std::numeric_limits<std::uint64_t>::max(), "video frame sequence exhausted");
  pending_.push_back({serial_ + 1, pts, {}, {}});
  struct Lease {
    std::shared_ptr<const graphics::Image> image;
    std::shared_ptr<Memory> memory;
    std::size_t bytes;
    Lease(std::shared_ptr<const graphics::Image> i, std::shared_ptr<Memory> m, std::size_t n)
        : image(std::move(i)), memory(std::move(m)), bytes(n) {
      memory->bytes += bytes;
    }
    ~Lease() { memory->bytes -= bytes; }
  };
  try {
    auto lease = std::make_shared<Lease>(image, memory_, bytes);
    pending_.back().image = std::shared_ptr<const graphics::Image>(std::move(lease), image.get());
  } catch (...) {
    pending_.pop_back();
    throw;
  }
  ++serial_;
  last_pts_ = pts;
  storage_ = Storage::cpu;
  return true;
}
std::size_t Stream::discard_obsolete(double due_pts) {
  check(!closed_ && std::isfinite(due_pts) && due_pts >= 0, "invalid video queue deadline");
  const auto protected_count = prepared_count_;
  std::size_t dropped = 0;
  while (pending_.size() > protected_count + 1 && pending_[protected_count + 1].pts <= due_pts) {
    pending_.erase(pending_.begin() + protected_count);
    ++dropped;
  }
  return dropped;
}
std::optional<Frame> Stream::prepare() {
  check(!closed_, "video stream is closed");
  if (prepared_count_) return pending_.front();
  return prepare_next();
}
bool Stream::can_offer_native() const {
  return !closed_ && pending_.size() < max_pending && memory_->native_frames < Memory::native_limit;
}
std::optional<double> Stream::next_unprepared_pts() const {
  return prepared_count_ < pending_.size() ? std::optional(pending_[prepared_count_].pts)
                                           : std::nullopt;
}
std::optional<Frame> Stream::prepare_next() {
  check(!closed_, "video stream is closed");
  if (prepared_count_ == max_prepared || prepared_count_ == pending_.size()) return {};
  return pending_[prepared_count_++];
}
bool Stream::prepared(std::uint64_t token) const {
  if (closed_) return false;
  for (std::size_t i = 0; i < prepared_count_; ++i)
    if (pending_[i].token == token) return true;
  return false;
}
void Stream::presented(std::uint64_t token) {
  check(!closed_ && prepared_count_ && pending_.front().token == token,
        "stale or out-of-order video presentation");
  current_ = std::move(pending_.front());
  pending_.pop_front();
  --prepared_count_;
}
void Stream::cancel() {
  prepared_count_ = 0;
}
void Stream::cancel(std::uint64_t token) {
  check(!closed_ && prepared_count_ && pending_.front().token == token,
        "stale or out-of-order video cancellation");
  if (prepared_count_ > 1) pending_.pop_front();
  --prepared_count_;
}
void Stream::discard_prepared(std::uint64_t token) {
  check(!closed_ && prepared_count_ && pending_.front().token == token,
        "stale or out-of-order video rejection");
  pending_.pop_front();
  --prepared_count_;
}
std::optional<std::uint64_t> Stream::composition_token() const {
  if (prepared_count_) return pending_[prepared_count_ - 1].token;
  return current_ ? std::optional(current_->token) : std::nullopt;
}
void Stream::flush() {
  discard_all();
}
void Stream::discard_all() {
  check(!closed_, "video stream is closed");
  prepared_count_ = 0;
  current_.reset();
  pending_.clear();
}
void Stream::close() {
  closed_ = true;
  prepared_count_ = 0;
  current_.reset();
  pending_.clear();
}
std::optional<double> Stream::current_pts() const {
  return current_ ? std::optional(current_->pts) : std::nullopt;
}
std::optional<double> Stream::next_pts() const {
  return pending_.empty() ? std::nullopt : std::optional(pending_.front().pts);
}
std::shared_ptr<const graphics::Image> Stream::image() const {
  const auto* frame =
      prepared_count_ ? &pending_[prepared_count_ - 1] : (current_ ? &*current_ : nullptr);
  check(!frame || !frame->native, "native video requires GPU composition");
  return frame ? frame->image : nullptr;
}
std::shared_ptr<const NativeFrame> Stream::native() const {
  return prepared_count_ ? pending_[prepared_count_ - 1].native
                         : (current_ ? current_->native : nullptr);
}
State View::state() const {
  if (!stream) return State::invalid;
  if (stream->closed()) return State::closed;
  if (!subscribed) return State::unsubscribed;
  return stream->current_pts() || stream->next_pts() ? State::data_available : State::subscribed;
}
}  // namespace headunit::theme::video
