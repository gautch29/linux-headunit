// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/camera_input.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <stdexcept>
#include <iomanip>

namespace headunit::theme::video {
namespace {
std::uint64_t monotonic_now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void check(bool valid, const char* message) {
  if (!valid) throw std::invalid_argument(message);
}
}  // namespace
struct LocalInput::Impl {
  struct Capture {
    std::uint64_t token, sequence, captured;
    CameraConsumption consumption;
  };
  camera::Configuration configuration;
  std::shared_ptr<Stream> stream;
  std::shared_ptr<camera::Queue> queue;
  TimeSource now;
  VisibilitySink visibility;
  std::unique_ptr<camera::Provider> provider;
  std::deque<Capture> staged;
  std::optional<Capture> current;
  std::optional<double> last_pts;
  CameraInputStatistics stats;
  std::uint64_t issued_token = 0;
  std::uint64_t source_generation = 1;
  bool active = false, requested = false;
  Impl(camera::Configuration c, std::shared_ptr<Stream> s, std::shared_ptr<camera::Queue> q,
       TimeSource t, VisibilitySink v, InputType type)
      : configuration(std::move(c)),
        stream(std::move(s)),
        queue(std::move(q)),
        now(t ? std::move(t) : TimeSource{monotonic_now}),
        visibility(std::move(v)) {
    camera::validate_configuration(configuration);
    check(configuration.enabled && stream && !stream->closed() && queue,
          "camera input requires an enabled source, open stream and queue");
    check(stream->unused(), "camera input needs an unused stream");
    const auto& input = stream->input();
    check(input.type == type && input.identifier == configuration.stream_identifier &&
              input.width == configuration.width && input.height == configuration.height &&
              std::abs(input.frame_duration - 1. / configuration.frame_rate) < 1e-12,
          "camera stream declaration does not match its source");
    check(type == InputType::camera ||
              ((type == InputType::widget || type == InputType::settings ||
                type == InputType::road_sign) &&
               configuration.source == camera::SourceKind::external && !visibility),
          "non-camera local input requires its own external producer and no camera visibility");
    // Ultra's signed camera slot owns its layer transform. Native preview
    // handles orientation separately; do not silently ignore it in Ultra.
    check(configuration.rotation_degrees == 0 && !configuration.mirror_horizontal,
          "Ultra camera orientation requires a capture conversion adapter");
  }
  void notify(bool on, std::uint64_t at) {
    const auto capture = on ? queue->available_capture_ns(at) : std::nullopt;
    if (visibility) visibility(capture.has_value(), capture.value_or(0), at);
  }
  void suspend(std::uint64_t at) {
    stats.discarded += staged.size();
    staged.clear();
    current.reset();
    queue->discard();
    if (!stream->closed()) stream->discard_all();
    notify(false, at);
  }
  bool composed_expired(std::uint64_t at) const {
    const auto token = stream->composition_token();
    if (!token) return false;
    const Capture* capture = nullptr;
    for (const auto& pending : staged)
      if (pending.token == *token) {
        capture = &pending;
        break;
      }
    if (!capture && current && current->token == *token) capture = &*current;
    check(capture, "camera composition has no acquisition record");
    return capture->captured > at ||
           at - capture->captured > std::uint64_t(configuration.maximum_age_ms) * 1000000;
  }
  bool eligible(const MediaClock* clock, std::uint64_t at) {
    if (!active || !clock || !clock->ready(at)) {
      suspend(at);
      return false;
    }
    // A fresh queued frame cannot make an expired current/staged texture fresh.
    if (composed_expired(at)) {
      ++stats.expired;
      suspend(at);
      return false;
    }
    return true;
  }
  void collect_provider() {
    // A retiring driver may be delayed in the kernel. Never join it in a
    // display turn or reopen its device while that old owner still exists.
    if (provider && !active && provider->stopped()) provider.reset();
    if (requested && !active && !provider) {
      if (queue->availability(now()) == camera::Availability::closed) {
        // Only an explicit new activation, after the old provider retires,
        // may create a fresh owned source. External producers still own their
        // terminal queue; never reopen it underneath their outstanding offers.
        if (configuration.source == camera::SourceKind::external) return;
        queue = std::make_shared<camera::Queue>(configuration);
        ++source_generation;
      }
      queue->discard();  // A retiring worker may have finished one old offer.
      provider = camera::make_provider(configuration, *queue);
      active = true;
    }
  }
  void pump(const MediaClock* clock) {
    collect_provider();
    const auto at = now();
    if (!eligible(clock, at)) return;
    if (provider) provider->poll(at, *queue);
    if (!queue->available_capture_ns(at)) {
      suspend(at);
      return;
    }
    notify(true, at);
  }
  std::optional<std::uint64_t> stage(const MediaClock& clock) {
    const auto at = now();
    if (!eligible(&clock, at)) return {};
    if (staged.size() == Stream::max_prepared) return {};
    const auto frame = queue->stage(at);
    if (!frame) return {};
    double pts;
    try {
      pts = clock.capture_at(frame->capture_monotonic_ns, at).pts_seconds;
    } catch (const std::invalid_argument&) {
      queue->complete(frame->sequence, false);
      ++stats.unmapped;
      notify(true, at);
      return {};
    }
    // A valid bounded phase adjustment may regress mapped captures. Drop the
    // individual source rather than fabricating PTS or weakening clock rules.
    if (last_pts && pts <= *last_pts) {
      queue->complete(frame->sequence, false);
      ++stats.non_monotonic;
      notify(true, at);
      return {};
    }
    bool offered = false;
    if (frame->image) {
      const auto image = std::shared_ptr<const graphics::Image>(frame, frame->image.get());
      offered = stream->offer_shared(pts, image);
    } else if (frame->native) {
      // The alias retains the OUTER camera permit, transitively retaining the
      // original producer sample. Never mutate or close borrowed plane FDs.
      const auto native = std::shared_ptr<const NativeFrame>(frame, frame->native.get());
      offered = stream->offer_native(pts, native);
    }
    if (!offered) {
      queue->complete(frame->sequence, false);
      ++stats.backpressure;
      notify(true, at);
      return {};
    }
    const auto prepared = stream->prepare_next();
    check(prepared.has_value(), "camera input lost its offered frame");
    CameraConsumption consumed;
    consumed.source_generation = source_generation;
    consumed.lease_sequence = frame->sequence;
    consumed.stream_token = prepared->token;
    consumed.capture_monotonic_ns = frame->capture_monotonic_ns;
    consumed.prepared_monotonic_ns = at;
    consumed.clock_epoch = clock.suspension_epoch();
    consumed.capture_reference = frame->capture_reference;
    consumed.acquisition = frame->acquisition;
    consumed.simulated = frame->simulated;
    consumed.input_width = stream->input().width;
    consumed.input_height = stream->input().height;
    if (frame->image) {
      consumed.width = frame->image->width;
      consumed.height = frame->image->height;
      consumed.storage_width = consumed.width;
      consumed.storage_height = consumed.height;
      consumed.plane_count = 1;
      consumed.stride_bytes[0] = consumed.width * sizeof(std::uint32_t);
    } else {
      const auto& native = *frame->native;
      consumed.native = true;
      consumed.width = native.metadata.width;
      consumed.height = native.metadata.height;
      consumed.storage_width = native.storage_width;
      consumed.storage_height = native.storage_height;
      consumed.crop_x = native.crop_x;
      consumed.crop_y = native.crop_y;
      consumed.plane_count = native.plane_count;
      for (unsigned i = 0; i < native.plane_count; ++i)
        consumed.stride_bytes[i] = native.planes[i].stride;
    }
    staged.push_back({prepared->token, frame->sequence, frame->capture_monotonic_ns, consumed});
    last_pts = pts;
    issued_token = prepared->token;
    ++stats.staged;
    return prepared->token;
  }
  void complete(std::uint64_t token, bool presented) {
    check(token && token <= issued_token, "unknown camera input receipt token");
    const auto found =
        std::find_if(staged.begin(), staged.end(), [&](const auto& c) { return c.token == token; });
    // Suspend/flush may retire an issued source while its GPU still retains it.
    if (found == staged.end()) return;
    check(found == staged.begin(), "camera input receipt order changed");
    const bool valid = stream->prepared(token);
    // The capture worker can close the Queue after the last pump. Its
    // retirement must win before this receipt advances the independent Stream.
    if (!queue->complete(found->sequence, valid && presented)) {
      suspend(now());
      return;
    }
    if (valid && presented) {
      stream->presented(token);
      current = *found;
      current->consumption.receipt_observed_monotonic_ns = now();
      ++stats.presented;
    } else {
      if (valid) stream->discard_prepared(token);
      ++stats.discarded;
    }
    staged.pop_front();
  }
};
LocalInput::LocalInput(camera::Configuration c, std::shared_ptr<Stream> s,
                       std::shared_ptr<camera::Queue> q, TimeSource now, VisibilitySink visibility,
                       InputType type)
    : impl_(std::make_unique<Impl>(std::move(c), std::move(s), std::move(q), std::move(now),
                                   std::move(visibility), type)) {}
LocalInput::~LocalInput() {
  impl_->provider.reset();
  impl_->queue->close();
  impl_->stream->close();
}
void LocalInput::set_active(bool active) {
  impl_->requested = active;
  if (!active) {
    impl_->active = false;
    impl_->suspend(impl_->now());
    if (impl_->provider) impl_->provider->request_stop();
  }
  impl_->collect_provider();
}
const std::shared_ptr<Stream>& LocalInput::stream() const {
  return impl_->stream;
}
void LocalInput::pump(const MediaClock* clock) {
  impl_->pump(clock);
}
std::optional<std::uint64_t> LocalInput::stage(const MediaClock& clock) {
  return impl_->stage(clock);
}
void LocalInput::complete(std::uint64_t token, bool presented) {
  impl_->complete(token, presented);
}
void LocalInput::suspend() {
  impl_->suspend(impl_->now());
}
bool LocalInput::composition_fresh() const {
  const auto at = impl_->now();
  // A still-young texture can belong to a source retired during GPU work.
  // Let the display's existing withdrawal/redraw guard remove it before swap.
  if (impl_->stream->composition_token() && !impl_->queue->available_capture_ns(at)) return false;
  return !impl_->composed_expired(at);
}
camera::Availability LocalInput::availability() const {
  return impl_->queue->availability(impl_->now());
}
CameraInputStatistics LocalInput::statistics() const {
  return impl_->stats;
}
std::optional<CameraConsumption> LocalInput::consumption() const {
  const auto at = impl_->now();
  const auto availability = impl_->queue->availability(at);
  if (!impl_->active || availability == camera::Availability::closed ||
      availability == camera::Availability::clock_reversed)
    return {};
  const auto token = impl_->stream->composition_token();
  if (!token) return {};
  const Impl::Capture* selected = nullptr;
  for (const auto& pending : impl_->staged)
    if (pending.token == *token) selected = &pending;
  if (!selected && impl_->current && impl_->current->token == *token) selected = &*impl_->current;
  if (!selected || selected->captured > at ||
      at - selected->captured > std::uint64_t(impl_->configuration.maximum_age_ms) * 1000000)
    return {};
  return selected->consumption;
}
void CameraConsumption::write_json(std::ostream& log) const {
  log << "{\"authority\":\"local-input-stream\",\"target\":\"registered-input\",";
  log << "\"sourceGeneration\":\"" << source_generation << "\",\"leaseSequence\":\""
      << lease_sequence << "\",\"streamToken\":\"" << stream_token << "\",\"captureMonoNs\":\""
      << capture_monotonic_ns << "\",\"preparedMonoNs\":\"" << prepared_monotonic_ns
      << "\",\"clockEpoch\":\"" << clock_epoch << "\",\"receiptObservedMonoNs\":";
  if (receipt_observed_monotonic_ns)
    log << '"' << *receipt_observed_monotonic_ns << '"';
  else
    log << "null";
  log << ",\"captureReference\":";
  if (capture_reference)
    log << (*capture_reference == camera::CaptureReference::end_of_frame ? "\"end-of-frame\""
                                                                         : "\"start-of-exposure\"");
  else
    log << "null";
  log << ",\"simulated\":" << (simulated ? "true" : "false") << ",\"storage\":\""
      << (native ? "native" : "argb32") << "\",\"width\":" << width << ",\"height\":" << height
      << ",\"storageWidth\":" << storage_width << ",\"storageHeight\":" << storage_height
      << ",\"cropX\":" << crop_x << ",\"cropY\":" << crop_y << ",\"strideBytes\":[";
  for (unsigned i = 0; i < std::min<unsigned>(plane_count, stride_bytes.size()); ++i) {
    if (i) log << ',';
    log << stride_bytes[i];
  }
  log << "],\"acquisition\":";
  if (acquisition) {
    const auto& source = *acquisition;
    log << "{\"width\":" << source.width << ",\"height\":" << source.height
        << ",\"strideBytes\":" << source.stride_bytes
        << ",\"driverSequence\":" << source.driver_sequence << ",\"publishedMonoNs\":\""
        << source.published_monotonic_ns << "\",\"adaptationFit\":";
    if (source.adaptation_fit)
      log << (*source.adaptation_fit == camera::CaptureFit::contain ? "\"contain\"" : "\"cover\"");
    else
      log << "null";
    log << '}';
  } else
    log << "null";
  log << ",\"inputWidth\":" << input_width << ",\"inputHeight\":" << input_height
      << ",\"inputRotationDegrees\":0,\"inputMirrorHorizontal\":false,\"captureGeometry\":";
  if (acquisition && acquisition->mapping) {
    const auto& mapping = *acquisition->mapping;
    const auto precision = log.precision();
    const auto flags = log.flags();
    log << std::defaultfloat << std::setprecision(17);
    const auto rect = [&](const graphics::Rect& r) {
      log << "{\"x\":" << r.origin.x << ",\"y\":" << r.origin.y << ",\"width\":" << r.size.width
          << ",\"height\":" << r.size.height << '}';
    };
    log << "{\"authority\":\"cpu-adapter\",\"target\":\"capture-delivery\","
           "\"coordinates\":\"pixel-edges-before-orientation\",\"scale\":"
        << mapping.scale << ",\"imageRect\":";
    rect(mapping.image);
    log << ",\"sourceRect\":";
    rect(mapping.source);
    log << ",\"deliveryRect\":";
    rect(mapping.destination);
    log << '}';
    log.precision(precision);
    log.flags(flags);
  } else
    log << "null";
  log << ",\"signedSceneFit\":null,\"opticalFit\":null,\"physicalDisplayFit\":null}";
}
}  // namespace headunit::theme::video
