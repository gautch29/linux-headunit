// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/native_input.hpp>
#include <headunit/theme/video_decoder.hpp>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <array>

namespace headunit::theme::video {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
std::uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
double seconds(std::uint64_t ntp) {
  return double(ntp >> 32) + double(ntp & 0xffffffffULL) / 4294967296.;
}
constexpr std::size_t packet_cost(const AirplayVideoPacket& p) {
  return p.data.size() + 128;
}
}  // namespace
struct NativeInputQueue::Impl {
  std::mutex mutex;
  std::deque<AirplayVideoPacket> queue;
  std::size_t bytes = 0, packets = 0;  // Includes owner-held pending packet.
  bool closed = false, attached = false;
  bool available = true;  // Standalone fixtures may omit an explicit transport ID.
  std::uint64_t generation = 0;
  std::optional<std::uint64_t> connection_id;
  std::array<std::uint64_t, 12> used_ids{};
  std::size_t used_count = 0;
  void clear() {
    queue.clear();
    bytes = packets = 0;
  }
  void begin_generation(std::uint64_t id) {
    std::lock_guard lock(mutex);
    check(!closed, "native input endpoint is closed");
    if (connection_id == id) {
      check(available, "retired native input ID cannot resume");
      return;
    }
    check(used_count < used_ids.size() && std::find(used_ids.begin(), used_ids.begin() + used_count,
                                                    id) == used_ids.begin() + used_count,
          "native input ID reused or generation budget exhausted");
    used_ids[used_count++] = id;
    connection_id = id;
    ++generation;
    available = true;
    clear();
  }
  void retire_generation(std::uint64_t id) {
    std::lock_guard lock(mutex);
    if (closed) return;
    if (!connection_id) {
      // EOF may precede the first packet/consumer turn for this endpoint.
      used_ids[used_count++] = id;
      connection_id = id;
    }
    if (connection_id != id || !available) return;
    ++generation;
    available = false;
    clear();
  }
  void close() {
    std::lock_guard lock(mutex);
    closed = true;
    available = false;
    clear();
  }
  bool enqueue(AirplayVideoPacket& p) {
    std::lock_guard lock(mutex);
    check(!closed, "native input generation is closed");
    check(available, "native input transport generation is retired");
    check(p.data.size() <= max_packet_bytes, "native input packet exceeds bound");
    if (p.kind == AirplayVideoPacket::Kind::heartbeat ||
        p.kind == AirplayVideoPacket::Kind::auxiliary)
      return true;
    check(p.kind == AirplayVideoPacket::Kind::config || p.kind == AirplayVideoPacket::Kind::frame,
          "invalid native input packet kind");
    if (p.kind == AirplayVideoPacket::Kind::frame)
      check(p.sender_ntp.has_value(), "native input requires authenticated source timestamps");
    const auto cost = packet_cost(p);
    if (packets >= max_packets || cost > max_bytes - bytes) return false;
    queue.push_back(std::move(p));
    bytes += cost;
    ++packets;
    return true;
  }
};
NativeInputQueue::NativeInputQueue() : impl_(std::make_unique<Impl>()) {}
NativeInputQueue::~NativeInputQueue() = default;
bool NativeInputQueue::enqueue(AirplayVideoPacket& p) {
  return impl_->enqueue(p);
}
void NativeInputQueue::begin_generation(std::uint64_t id) {
  impl_->begin_generation(id);
}
void NativeInputQueue::retire_generation(std::uint64_t id) {
  impl_->retire_generation(id);
}
bool NativeInputQueue::available() const {
  std::lock_guard lock(impl_->mutex);
  return !impl_->closed && impl_->available;
}
void NativeInputQueue::close() {
  impl_->close();
}
bool NativeInputQueue::closed() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->closed;
}
struct NativeInput::Impl {
  std::shared_ptr<Stream> stream;
  std::string decoder_name;
  std::unique_ptr<Decoder> decoder;
  std::optional<Configuration> configuration;
  std::optional<AirplayVideoPacket> pending;
  std::optional<std::uint64_t> blocked_since, last_decoded_ntp;
  std::deque<std::uint64_t> staged_tokens;
  NativeInputStats stats;
  std::optional<NativeInputPresentation> presentation;
  std::uint64_t last_receipt_token = 0;
  std::shared_ptr<NativeInputQueue> ingress;
  std::uint64_t generation = 0;
  std::optional<std::uint64_t> connection_id;
  double advance_seconds = 0;
  TouchResponseProbe touch_response;
  Impl(std::shared_ptr<Stream> s, std::shared_ptr<NativeInputQueue> q, std::string d,
       std::uint64_t advance_ns)
      : stream(std::move(s)),
        decoder_name(std::move(d)),
        ingress(std::move(q)),
        advance_seconds(double(advance_ns) / 1e9) {
    check(stream && !stream->closed() && ingress, "native input requires an open stream and queue");
    check(advance_ns <= max_presentation_advance_ns, "native input presentation advance too large");
    auto& qstate = *ingress->impl_;
    std::lock_guard lock(qstate.mutex);
    check(!qstate.closed && !qstate.attached, "native input queue already attached or closed");
    qstate.attached = true;
    generation = qstate.generation;
    connection_id = qstate.connection_id;
  }
  bool fresh() const {
    auto& q = *ingress->impl_;
    std::lock_guard lock(q.mutex);
    return !q.closed && q.available && generation == q.generation;
  }
  bool refresh_generation() {
    std::uint64_t current = 0;
    bool available = false;
    std::optional<std::uint64_t> current_id;
    {
      auto& q = *ingress->impl_;
      std::lock_guard lock(q.mutex);
      check(!q.closed, "native input endpoint is closed");
      current = q.generation;
      current_id = q.connection_id;
      available = q.available;
    }
    if (current != generation) {
      // No decoder destruction while holding the producer's ingress lock.
      pending.reset();
      blocked_since.reset();
      decoder.reset();
      configuration.reset();
      stream->discard_all();
      generation = current;
      connection_id = current_id;
      ++stats.generation_changes;
      // Keep outstanding receipt tokens ordered, and retain source PTS floors.
      // Old receipts cannot present a frame in this replacement generation.
    }
    return available;
  }
  bool next() {
    auto& q = *ingress->impl_;
    std::lock_guard lock(q.mutex);
    check(!q.closed, "native input generation is closed");
    if (!q.available || q.generation != generation) return false;
    if (pending) return true;
    if (q.queue.empty()) return false;
    pending = std::move(q.queue.front());
    q.queue.pop_front();
    return true;
  }
  bool consume() {
    if (pending->kind == AirplayVideoPacket::Kind::config) {
      if (!pending->data.empty()) {
        auto config = screen_configuration(pending->data);
        if (configuration)
          check(*configuration == config, "native codec changed; replace input generation");
        else {
          DecoderOptions options;
          options.codec = config.codec;
          options.codec_data = config.codec_data;
          options.max_width = stream->input().width;
          options.max_height = stream->input().height;
          options.native_buffers = true;
          options.decoder = decoder_name;
          // One current sample plus the stream's three bounded queue slots.
          // With two pending draws, the old three-lease policy starves decode.
          options.max_native_leases = Stream::max_pending + 1;
          decoder = std::make_unique<Decoder>(std::move(options));
          configuration = std::move(config);
        }
      }
    } else {
      check(bool(decoder), "native frame arrived before configuration");
      if (!decoder->push(*pending)) {
        if (!blocked_since) {
          blocked_since = now();
          ++stats.backpressure;
        }
        return false;
      }
      ++stats.submitted;
    }
    {
      auto& q = *ingress->impl_;
      std::lock_guard lock(q.mutex);
      if (!q.closed && q.available && q.generation == generation) {
        q.bytes -= packet_cost(*pending);
        --q.packets;
      }
    }
    pending.reset();
    blocked_since.reset();
    return true;
  }
  bool drain(const MediaClock* clock) {
    if (!decoder) return false;
    const auto at = now();
    if (clock && clock->ready(at))
      stats.superseded += stream->discard_obsolete(due_pts(*clock, at));
    // Keep decoder output leased in its bounded queue until a pending display
    // receipt frees a slot. Never pull and then fail on a full decoded queue.
    if (clock && clock->ready(at) && !stream->can_offer_native()) return false;
    auto frame = decoder->pull_native();
    if (!frame) return false;
    ++stats.decoded;
    if (!fresh()) {
      ++stats.generation_dropped;
      return true;
    }
    if (last_decoded_ntp && frame->metadata.sender_ntp <= *last_decoded_ntp) {
      ++stats.non_monotonic;
      return true;
    }
    last_decoded_ntp = frame->metadata.sender_ntp;
    const auto received = now();
    if (!clock || !clock->ready(received)) {
      ++stats.clock_dropped;
      return true;
    }
    try {
      const auto timing = clock->schedule(frame->metadata.sender_ntp, received);
      check(stream->offer_native(timing.media_pts_seconds, frame),
            "native decoded queue exhausted");
      record_lead(timing.lead_ns);
      touch_response.decoded(received);
    } catch (const FrameOutsideWindow& late) {
      if (late.lead_ns >= 0) throw;
      ++stats.late;
    }
    return true;
  }
  // Latest PTS that may be staged now: the host's media time plus the
  // configured advance. Zero advance is strict PTS staging.
  double due_pts(const MediaClock& clock, std::uint64_t at) const {
    return seconds(clock.media_ntp_at(at)) + advance_seconds;
  }
  void record_lead(std::int64_t lead_ns) {
    if (stats.lead_samples == 0) {
      stats.min_lead_ns = lead_ns;
      stats.max_lead_ns = lead_ns;
    } else {
      stats.min_lead_ns = std::min(stats.min_lead_ns, lead_ns);
      stats.max_lead_ns = std::max(stats.max_lead_ns, lead_ns);
    }
    stats.lead_sum_ns += lead_ns;
    ++stats.lead_samples;
  }
  void complete(std::uint64_t token, bool presented, const InputPresentationReceipt* receipt) {
    check(!staged_tokens.empty() && staged_tokens.front() == token,
          "native input receipt order changed");
    if (receipt) {
      // The production mapper already validates clocks/quality. Keep identity
      // and replay guards here; never complete a newer source with an old fact.
      check(presented && receipt->feedback.presented && receipt->feedback.correlation &&
                receipt->feedback.token == receipt->mapped.token &&
                receipt->feedback.token > last_receipt_token &&
                receipt->submission_epoch == receipt->current_epoch,
            "native input mapped receipt identity changed");
      last_receipt_token = receipt->feedback.token;
    }
    if (!fresh()) stream->discard_all();
    if (!stream->prepared(token))
      ++stats.discarded;  // Script flush invalidates old submissions.
    else if (presented) {
      stream->presented(token);
      ++stats.presented;
      presentation.reset();
      if (receipt) {
        presentation = NativeInputPresentation{
            token, generation, connection_id, *stream->current_pts(), {}, *receipt};
        presentation->decoded = stream->current_native_metadata();
      }
      touch_response.presented(now());
    } else {
      stream->cancel(token);
      ++stats.discarded;
    }
    staged_tokens.pop_front();
  }
  void suspend() {
    presentation.reset();
    stats.discarded += staged_tokens.size();
    staged_tokens.clear();
    stream->discard_all();
  }
  void pump(const MediaClock* clock) {
    try {
      const auto began = now();
      touch_response.expire(began);
      for (unsigned step = 0; step < 64 && now() - began < 2000000ULL; ++step) {
        if (!refresh_generation()) return;
        const bool available = next();  // Checks terminal state even with no decoder output.
        const bool decoded = drain(clock);
        // Pulling a real decoded frame is decoder progress even if its async
        // streaming thread has not freed an appsrc slot for the retry yet.
        if (decoded && blocked_since) blocked_since = now();
        if (available) {
          if (!consume() && !decoded) break;
        } else if (!decoded)
          break;
      }
      // A bootstrap owner gap can outlast the decoder deadline while native
      // leases are waiting to be released. Retry after draining those leases
      // before deciding the decoder is still blocked. A real decoded frame or
      // successful push proves progress; an unresponsive decoder retains the
      // same limit. Empty polls cannot refresh the deadline.
      check(!blocked_since || now() - *blocked_since < 3000000000ULL, "native decoder stalled");
    } catch (...) {
      ingress->close();
      suspend();
      decoder.reset();
      pending.reset();
      throw;
    }
  }
  std::optional<std::uint64_t> stage(const MediaClock& clock) {
    if (!fresh()) return {};
    if (staged_tokens.size() == Stream::max_prepared) return {};
    const auto next_pts = stream->next_unprepared_pts();
    if (!next_pts) return {};
    const auto at = now();
    if (!clock.ready(at) || *next_pts > due_pts(clock, at)) return {};
    const auto frame = stream->prepare_next();
    check(bool(frame), "native input lost queued frame");
    staged_tokens.push_back(frame->token);
    stats.max_staged = std::max(stats.max_staged, std::uint64_t(staged_tokens.size()));
    return frame->token;
  }
};
NativeInput::NativeInput(std::shared_ptr<Stream> stream, std::shared_ptr<NativeInputQueue> queue,
                         std::string decoder, std::uint64_t presentation_advance_ns)
    : impl_(std::make_unique<Impl>(std::move(stream), std::move(queue), std::move(decoder),
                                   presentation_advance_ns)) {}
NativeInput::~NativeInput() {
  impl_->ingress->close();
  impl_->stream->close();
}
const std::shared_ptr<Stream>& NativeInput::stream() const {
  return impl_->stream;
}
void NativeInput::pump(const MediaClock* clock) {
  impl_->pump(clock);
}
std::optional<std::uint64_t> NativeInput::stage(const MediaClock& clock) {
  return impl_->stage(clock);
}
void NativeInput::complete(std::uint64_t token, bool presented) {
  impl_->complete(token, presented, nullptr);
}
void NativeInput::complete(std::uint64_t token, bool presented,
                           const InputPresentationReceipt* receipt) {
  impl_->complete(token, presented, receipt);
}
std::optional<NativeInputPresentation> NativeInput::presentation() const {
  auto& q = *impl_->ingress->impl_;
  std::lock_guard lock(q.mutex);
  if (!impl_->presentation || q.closed || !q.available || impl_->generation != q.generation ||
      impl_->presentation->owner_generation != impl_->generation ||
      impl_->stream->current_token() != impl_->presentation->stream_token)
    return {};
  return impl_->presentation;
}
void NativeInput::suspend() {
  impl_->suspend();
}
bool NativeInput::composition_fresh() const {
  return impl_->fresh();
}
NativeInputIdentity NativeInput::identity() const {
  auto& q = *impl_->ingress->impl_;
  std::lock_guard lock(q.mutex);
  return {impl_->generation,
          q.generation,
          impl_->connection_id,
          q.connection_id,
          !q.closed && q.available,
          q.closed,
          !q.closed && q.available && impl_->generation == q.generation,
          now()};
}
NativeInputStats NativeInput::stats() const {
  auto result = impl_->stats;
  result.touch_response = impl_->touch_response.stats();
  return result;
}
void NativeInput::touch_pressed(std::uint64_t monotonic_ns) {
  impl_->touch_response.press(monotonic_ns);
}
void NativeInput::set_presentation_advance(std::uint64_t presentation_advance_ns) {
  check(presentation_advance_ns <= max_presentation_advance_ns,
        "native input presentation advance too large");
  impl_->advance_seconds = double(presentation_advance_ns) / 1e9;
}
}  // namespace headunit::theme::video
