// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/gst_preview_output.hpp>

#include <gst/gst.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace headunit {
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct Element {
  GstElement* value = nullptr;
  explicit Element(GstElement* element) : value(element) {
    if (value && g_object_is_floating(value)) gst_object_ref_sink(value);
  }
  ~Element() {
    if (value) gst_object_unref(value);
  }
  GstElement* release() {
    auto* result = value;
    value = nullptr;
    return result;
  }
};
struct Finalization {
  std::atomic<unsigned> remaining{0};
};
void watch(GstElement* element, const std::shared_ptr<Finalization>& state) {
  ++state->remaining;
  g_object_weak_ref(
      G_OBJECT(element),
      [](gpointer data, GObject*) {
        auto* retained = static_cast<std::shared_ptr<Finalization>*>(data);
        --(*retained)->remaining;
        delete retained;
      },
      new std::shared_ptr<Finalization>(state));
}
std::shared_ptr<Finalization> watch_tree(GstElement* element) {
  auto result = std::make_shared<Finalization>();
  watch(element, result);
  if (GST_IS_BIN(element)) {
    auto* children = gst_bin_iterate_recurse(GST_BIN(element));
    GValue child = G_VALUE_INIT;
    bool too_large = false;
    for (;;) {
      const auto next = gst_iterator_next(children, &child);
      if (next == GST_ITERATOR_DONE) break;
      if (next != GST_ITERATOR_OK || result->remaining >= 16) {
        too_large = true;
        break;
      }
      watch(GST_ELEMENT(g_value_get_object(&child)), result);
      g_value_reset(&child);
    }
    if (G_IS_VALUE(&child)) g_value_unset(&child);
    gst_iterator_free(children);
    require(!too_large, "preview output exceeds its bounded element inventory");
  }
  return result;
}
bool same_segment(const GstSegment& a, const GstSegment& b) {
  return a.format == b.format && a.flags == b.flags && a.rate == b.rate &&
         a.applied_rate == b.applied_rate && a.base == b.base && a.offset == b.offset &&
         a.start == b.start && a.stop == b.stop && a.time == b.time && a.duration == b.duration;
}
bool same_stream(GstEvent* a, GstEvent* b) noexcept {
  if (!a || !b) return a == b;
  const gchar *left = nullptr, *right = nullptr;
  gst_event_parse_stream_start(a, &left);
  gst_event_parse_stream_start(b, &right);
  guint left_group = 0, right_group = 0;
  const bool has_left = gst_event_parse_group_id(a, &left_group);
  const bool has_right = gst_event_parse_group_id(b, &right_group);
  GstStreamFlags left_flags = GST_STREAM_FLAG_NONE, right_flags = GST_STREAM_FLAG_NONE;
  gst_event_parse_stream_flags(a, &left_flags);
  gst_event_parse_stream_flags(b, &right_flags);
  return gst_event_get_seqnum(a) == gst_event_get_seqnum(b) && g_strcmp0(left, right) == 0 &&
         has_left == has_right && left_group == right_group && left_flags == right_flags;
}
struct StreamLock {
  GstPad* pad;
  explicit StreamLock(GstPad* value) : pad(value) { GST_PAD_STREAM_LOCK(pad); }
  ~StreamLock() { GST_PAD_STREAM_UNLOCK(pad); }
};
}  // namespace

struct GstPreviewOutput::Impl {
  GstElement* pipeline;
  GstElement* valve;
  GstElement* output = nullptr;
  GstPad* source = nullptr;
  GstPad* input = nullptr;
  gulong probe = 0;
  gulong input_probe = 0;
  mutable std::mutex mutex;
  Stats state;
  GstSegment segment{};
  GstSegment barrier_mapping{};
  std::optional<guint32> segment_id, stream_id;
  GstEvent *stream_start = nullptr, *decoder_stream_start = nullptr,
           *barrier_stream_start = nullptr;
  GstClock* clock = nullptr;
  GstClock* decoder_clock = nullptr;
  GstClockTime base = GST_CLOCK_TIME_NONE, floor = GST_CLOCK_TIME_NONE;
  GstClockTime decoder_base = GST_CLOCK_TIME_NONE;
  guint32 barrier_segment = 0;
  guint32 decoder_segment = 0;
  GstSegment decoder_mapping{};
  bool decoder_mapping_ready = false;
  std::shared_ptr<Finalization> retiring;

  Impl(GstElement* p, GstElement* v) : pipeline(p), valve(v) {
    require(p && GST_IS_PIPELINE(p) && v && GST_IS_ELEMENT(v), "invalid preview output pipeline");
    require(GST_OBJECT_PARENT(v) == GST_OBJECT(p), "preview valve must belong to its pipeline");
    GstState actual = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
    require(gst_element_get_state(p, &actual, &pending, 0) == GST_STATE_CHANGE_SUCCESS &&
                actual <= GST_STATE_READY && pending == GST_STATE_VOID_PENDING,
            "preview output construction must precede streaming");
    auto* property = g_object_class_find_property(G_OBJECT_GET_CLASS(v), "drop");
    require(property && G_PARAM_SPEC_VALUE_TYPE(property) == G_TYPE_BOOLEAN &&
                g_object_class_find_property(G_OBJECT_GET_CLASS(v), "drop-mode"),
            "preview output needs a GStreamer valve");
    source = gst_element_get_static_pad(v, "src");
    require(source, "preview valve has no source pad");
    try {
      input = gst_element_get_static_pad(v, "sink");
      require(input, "preview valve has no sink pad");
      require(!gst_pad_is_linked(source), "preview valve must have no downstream output");
      gst_segment_init(&segment, GST_FORMAT_UNDEFINED);
      g_object_set(valve, "drop", TRUE, "drop-mode", 0, nullptr);
      // A closed drop-all valve suppresses nonsticky flush events. Observe
      // these at its input; compare segment mappings only at the output pad.
      input_probe = gst_pad_add_probe(
          input,
          static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                                       GST_PAD_PROBE_TYPE_EVENT_FLUSH),
          [](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
            auto& self = *static_cast<Impl*>(data);
            const auto kind = GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info));
            if (kind == GST_EVENT_FLUSH_START || kind == GST_EVENT_FLUSH_STOP) {
              std::lock_guard lock(self.mutex);
              if (self.decoder_clock) self.state.decoder_epoch_retired = true;
              if (self.clock) self.state.barrier_retired = true;
              self.segment_id.reset();
            }
            return GST_PAD_PROBE_OK;
          },
          this, nullptr);
      require(input_probe != 0, "cannot observe hidden preview discontinuities");
      probe = gst_pad_add_probe(
          source,
          static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
                                       GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                                       GST_PAD_PROBE_TYPE_EVENT_FLUSH),
          [](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
            return static_cast<Impl*>(data)->filter(info);
          },
          this, nullptr);
      require(probe != 0, "cannot install preview freshness gate");
      replace(nullptr, false);
    } catch (...) {
      if (probe) gst_pad_remove_probe(source, probe);
      if (input_probe) gst_pad_remove_probe(input, input_probe);
      if (output) {
        gst_element_set_state(output, GST_STATE_NULL);
        gst_element_unlink(valve, output);
        gst_bin_remove(GST_BIN(pipeline), output);
        gst_object_unref(output);
        output = nullptr;
      }
      gst_object_unref(source);
      source = nullptr;
      if (input) {
        gst_object_unref(input);
        input = nullptr;
      }
      throw;
    }
  }
  ~Impl() {
    g_object_set(valve, "drop", TRUE, nullptr);
    // The caller has synchronously stopped the pipeline first. Probe removal
    // alone does not wait for a callback that is already executing.
    if (probe) gst_pad_remove_probe(source, probe);
    if (input_probe) gst_pad_remove_probe(input, input_probe);
    if (output) {
      gst_element_set_state(output, GST_STATE_NULL);
      gst_element_unlink(valve, output);
      gst_bin_remove(GST_BIN(pipeline), output);
      gst_object_unref(output);
    }
    if (clock) gst_object_unref(clock);
    if (decoder_clock) gst_object_unref(decoder_clock);
    if (stream_start) gst_event_unref(stream_start);
    if (decoder_stream_start) gst_event_unref(decoder_stream_start);
    if (barrier_stream_start) gst_event_unref(barrier_stream_start);
    gst_object_unref(source);
    gst_object_unref(input);
  }

  GstPadProbeReturn filter(GstPadProbeInfo* info) noexcept {
    std::lock_guard lock(mutex);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
      auto* event = GST_PAD_PROBE_INFO_EVENT(info);
      switch (GST_EVENT_TYPE(event)) {
        case GST_EVENT_STREAM_START:
          stream_id = gst_event_get_seqnum(event);
          if (decoder_stream_start && !same_stream(event, decoder_stream_start))
            state.decoder_epoch_retired = true;
          if (clock && !same_stream(event, barrier_stream_start)) state.barrier_retired = true;
          if (stream_start) gst_event_unref(stream_start);
          stream_start = gst_event_ref(event);
          break;
        case GST_EVENT_SEGMENT: {
          const GstSegment* observed = nullptr;
          gst_event_parse_segment(event, &observed);
          segment = *observed;
          segment_id = gst_event_get_seqnum(event);
          if (decoder_mapping_ready &&
              (*segment_id != decoder_segment || !same_segment(segment, decoder_mapping)))
            state.decoder_epoch_retired = true;
          if (clock && (*segment_id != barrier_segment || !same_segment(segment, barrier_mapping)))
            state.barrier_retired = true;
          break;
        }
        case GST_EVENT_FLUSH_START:
        case GST_EVENT_FLUSH_STOP:
          if (clock) state.barrier_retired = true;
          if (decoder_clock) state.decoder_epoch_retired = true;
          segment_id.reset();
          break;
        default:
          break;
      }
      // Pin identities as soon as they are observed, before the first decoded
      // buffer. Consecutive stream/segment events cannot renew queued input.
      observe_epoch();
      return GST_PAD_PROBE_OK;
    }
    // No list may bypass the per-buffer gate. The production decoder supplies
    // single buffers; a future list producer needs explicit member validation.
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
      ++state.rejected;
      return GST_PAD_PROBE_DROP;
    }
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    observe_epoch();
    bool accepted = !state.decoder_epoch_retired || state.draining;
    if (clock) {
      auto* current_clock = gst_element_get_clock(pipeline);
      const auto current_base = gst_element_get_base_time(pipeline);
      if (current_clock != clock || current_base != base) state.barrier_retired = true;
      if (current_clock) gst_object_unref(current_clock);
      const auto pts = GST_BUFFER_PTS(buffer);
      const auto running = GST_CLOCK_TIME_IS_VALID(pts) && segment.format == GST_FORMAT_TIME
                               ? gst_segment_to_running_time(&segment, GST_FORMAT_TIME, pts)
                               : GST_CLOCK_TIME_NONE;
      accepted =
          accepted && !state.barrier_retired && GST_CLOCK_TIME_IS_VALID(running) && running > floor;
    }
    if (!accepted) {
      ++state.rejected;
      return GST_PAD_PROBE_DROP;
    }
    ++state.forwarded;
    return GST_PAD_PROBE_OK;
  }

  // Called with mutex held. Pin the original decoder time domain even while
  // draining. A later return must not reinterpret buffered old PTS against a
  // smaller, newly selected base time. No drain/retry can renew this authority.
  void observe_epoch() noexcept {
    // GstPipeline distributes its clock before setting the new base time, and
    // streaming threads already run during that PAUSED->PLAYING transition.
    // Pin only a committed PLAYING domain, never the pre-PLAYING base time.
    GstState actual = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
    const bool playing =
        decoder_clock ||
        (gst_element_get_state(pipeline, &actual, &pending, 0) != GST_STATE_CHANGE_FAILURE &&
         actual == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING);
    auto* observed = gst_element_get_clock(pipeline);
    const auto origin = gst_element_get_base_time(pipeline);
    auto* after = gst_element_get_clock(pipeline);
    const bool coherent = observed && after == observed &&
                          gst_element_get_base_time(pipeline) == origin &&
                          GST_CLOCK_TIME_IS_VALID(origin);
    if (after) gst_object_unref(after);
    if (decoder_clock) {
      if (!coherent || observed != decoder_clock || origin != decoder_base)
        state.decoder_epoch_retired = true;
    } else if (coherent && playing) {
      decoder_clock = observed;
      observed = nullptr;
      decoder_base = origin;
    }
    if (decoder_clock && stream_start && !decoder_stream_start)
      decoder_stream_start = gst_event_ref(stream_start);
    if (decoder_clock && segment_id && segment.format == GST_FORMAT_TIME &&
        !decoder_mapping_ready) {
      decoder_segment = *segment_id;
      decoder_mapping = segment;
      decoder_mapping_ready = true;
    }
    if (observed) gst_object_unref(observed);
  }

  void barrier(bool fresh) {
    std::lock_guard lock(mutex);
    if (clock) {
      gst_object_unref(clock);
      clock = nullptr;
    }
    if (barrier_stream_start) {
      gst_event_unref(barrier_stream_start);
      barrier_stream_start = nullptr;
    }
    base = floor = GST_CLOCK_TIME_NONE;
    state.barrier_retired = false;
    state.arrival_floor.reset();
    if (!fresh) return;
    GstState actual = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
    require(gst_element_get_state(pipeline, &actual, &pending, 0) == GST_STATE_CHANGE_SUCCESS &&
                actual == GST_STATE_PLAYING && pending == GST_STATE_VOID_PENDING,
            "preview restore requires a stable PLAYING pipeline");
    require(segment_id && stream_id && segment.format == GST_FORMAT_TIME && segment.rate == 1.0 &&
                segment.applied_rate == 1.0,
            "preview restore requires an observed unit-rate TIME segment");
    observe_epoch();
    require(decoder_clock && decoder_mapping_ready && decoder_stream_start &&
                !state.decoder_epoch_retired,
            "preview decoder time domain retired; create a new decoder generation");
    auto* observed = gst_element_get_clock(pipeline);
    require(observed, "preview restore has no actual pipeline clock");
    const auto origin = gst_element_get_base_time(pipeline);
    const auto now = gst_clock_get_time(observed);
    auto* after_clock = gst_element_get_clock(pipeline);
    const bool coherent =
        observed == decoder_clock && origin == decoder_base && after_clock == observed &&
        gst_element_get_base_time(pipeline) == origin && GST_CLOCK_TIME_IS_VALID(now) &&
        GST_CLOCK_TIME_IS_VALID(origin) && now >= origin;
    if (after_clock) gst_object_unref(after_clock);
    if (!coherent) {
      state.decoder_epoch_retired = true;
      gst_object_unref(observed);
      throw std::runtime_error("preview clock changed during restore");
    }
    clock = observed;
    base = origin;
    floor = now - origin;
    state.arrival_floor = floor;
    barrier_segment = *segment_id;
    barrier_mapping = segment;
    barrier_stream_start = gst_event_ref(stream_start);
  }

  void remove() {
    if (!output) {
      require(!retiring || retiring->remaining == 0, "previous preview output is still retained");
      return;
    }
    // NULL alone retains an embedded waylandsink window in GStreamer 1.26.
    // Remove the bin and release the final reference before retiring its parent.
    require(gst_element_set_state(output, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE,
            "preview output could not stop");
    GstState actual = GST_STATE_VOID_PENDING;
    const auto stopped = gst_element_get_state(output, &actual, nullptr, 500 * GST_MSECOND);
    require(stopped == GST_STATE_CHANGE_SUCCESS && actual == GST_STATE_NULL,
            "preview output did not reach NULL");
    retiring = watch_tree(output);
    gst_element_unlink(valve, output);
    require(gst_bin_remove(GST_BIN(pipeline), output), "cannot remove preview output");
    // State/status messages can retain the old sink after removal. Discard only
    // its diagnostic chatter, and re-emit genuine error/warning payloads with
    // the stable pipeline as source. Unrelated messages retain their identity;
    // concurrent bus posts may precede reposted messages. This is bounded.
    auto* bus = gst_element_get_bus(pipeline);
    std::vector<GstMessage*> retained;
    bool overflow = false;
    for (unsigned i = 0; i <= 512; ++i) {
      auto* message = gst_bus_pop(bus);
      if (!message) break;
      if (i == 512) {
        retained.push_back(message);
        overflow = true;
        break;
      }
      auto* origin = GST_MESSAGE_SRC(message);
      if (origin == GST_OBJECT(output) ||
          (origin && gst_object_has_as_ancestor(origin, GST_OBJECT(output)))) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR ||
            GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
          GError* error = nullptr;
          gchar* detail = nullptr;
          const bool fatal = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
          if (fatal)
            gst_message_parse_error(message, &error, &detail);
          else
            gst_message_parse_warning(message, &error, &detail);
          auto* replacement = fatal ? gst_message_new_error(GST_OBJECT(pipeline), error, detail)
                                    : gst_message_new_warning(GST_OBJECT(pipeline), error, detail);
          gst_message_set_seqnum(replacement, gst_message_get_seqnum(message));
          retained.push_back(replacement);
          g_clear_error(&error);
          g_free(detail);
        }
        gst_message_unref(message);
      } else
        retained.push_back(message);
    }
    for (auto* message : retained)
      gst_bus_post(bus, message);
    gst_object_unref(bus);
    gst_object_unref(output);
    output = nullptr;
    require(!overflow && retiring->remaining == 0, "preview output did not fully finalize");
  }

  void replace(GstElement* selected, bool fresh) {
    Element candidate(selected);
    g_object_set(valve, "drop", TRUE, nullptr);
    // Preserve the closed gate on any failure; callers must not release a
    // surface lease until drain() returns successfully.
    remove();
    barrier(fresh);
    if (!candidate.value) {
      candidate.value = gst_element_factory_make("fakesink", nullptr);
      require(candidate.value, "preview drain sink unavailable");
      gst_object_ref_sink(candidate.value);
      g_object_set(candidate.value, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE,
                   nullptr);
    }
    auto* candidate_input = gst_element_get_static_pad(candidate.value, "sink");
    require(candidate_input, "preview output must expose a sink pad");
    const bool free = !gst_pad_is_linked(candidate_input);
    gst_object_unref(candidate_input);
    require(free && !GST_OBJECT_PARENT(candidate.value), "preview output is already attached");
    require(gst_bin_add(GST_BIN(pipeline), candidate.value), "cannot add preview output");
    output = candidate.release();
    require(gst_element_link(valve, output), "cannot link preview output");
    require(gst_element_sync_state_with_parent(output), "cannot start preview output");
    {
      std::lock_guard lock(mutex);
      ++state.generation;
      state.draining = selected == nullptr;
    }
    // A valve drops EOS while closed; reopening alone does not replay sticky
    // events. Serialize this short reopen with upstream events, then forward
    // the original sticky sequence, including EOS, to the new output. Sending
    // EOS back into an already-ended sink pad would be rejected by GStreamer.
    // EOS arriving afterward sees the open gate. No decoder lock during removal.
    StreamLock stream_lock(input);
    auto* end = gst_pad_get_sticky_event(input, GST_EVENT_EOS, 0);
    g_object_set(valve, "drop", FALSE, nullptr);
    if (end) {
      gst_event_unref(end);
      struct Replay {
        GstPad* source;
        unsigned count = 0;
        bool ok = true;
      } replay{source};
      gst_pad_sticky_events_foreach(
          input,
          [](GstPad*, GstEvent** event, gpointer context) -> gboolean {
            auto& value = *static_cast<Replay*>(context);
            if (value.count++ >= 64 || !gst_pad_push_event(value.source, gst_event_ref(*event))) {
              value.ok = false;
              return FALSE;
            }
            return TRUE;
          },
          &replay);
      if (!replay.ok) {
        g_object_set(valve, "drop", TRUE, nullptr);
        throw std::runtime_error("preview output could not transfer retained EOS");
      }
    }
  }
};

GstPreviewOutput::GstPreviewOutput(GstElement* pipeline, GstElement* valve)
    : impl_(std::make_unique<Impl>(pipeline, valve)) {}
GstPreviewOutput::~GstPreviewOutput() = default;
void GstPreviewOutput::start(GstElement* output) {
  GstState actual = GST_STATE_NULL;
  gst_element_get_state(impl_->pipeline, &actual, nullptr, 0);
  if (actual > GST_STATE_READY) {
    Element rejected(output);
    throw std::runtime_error("initial preview output must precede PLAYING");
  }
  impl_->replace(output, false);
}
void GstPreviewOutput::drain() {
  impl_->replace(nullptr, false);
}
void GstPreviewOutput::restore(GstElement* output) {
  if (!output) throw std::invalid_argument("preview restore needs a new output");
  impl_->replace(output, true);
}
GstPreviewOutput::Stats GstPreviewOutput::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}
bool GstPreviewOutput::restore_ready() const {
  std::lock_guard lock(impl_->mutex);
  // A pinned epoch cannot be renewed by a temporary state change. Observe it
  // even when a paused/stopped pipeline cannot yet authorize restoration.
  impl_->observe_epoch();
  GstState actual = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
  if (gst_element_get_state(impl_->pipeline, &actual, &pending, 0) != GST_STATE_CHANGE_SUCCESS ||
      actual != GST_STATE_PLAYING || pending != GST_STATE_VOID_PENDING)
    return false;
  return !impl_->state.decoder_epoch_retired && impl_->decoder_clock &&
         impl_->decoder_stream_start && impl_->decoder_mapping_ready && impl_->stream_id &&
         impl_->segment_id && impl_->segment.format == GST_FORMAT_TIME &&
         impl_->segment.rate == 1.0 && impl_->segment.applied_rate == 1.0;
}
std::uint64_t GstPreviewOutput::arrival_time() const {
  std::lock_guard lock(impl_->mutex);
  impl_->observe_epoch();
  require(impl_->decoder_clock && !impl_->state.decoder_epoch_retired,
          "preview arrival has no live original decoder epoch");
  auto* observed = gst_element_get_clock(impl_->pipeline);
  require(observed, "preview arrival has no actual pipeline clock");
  const auto base = gst_element_get_base_time(impl_->pipeline);
  const auto now = gst_clock_get_time(observed);
  auto* after = gst_element_get_clock(impl_->pipeline);
  const bool valid = observed == impl_->decoder_clock && base == impl_->decoder_base &&
                     after == observed && gst_element_get_base_time(impl_->pipeline) == base &&
                     GST_CLOCK_TIME_IS_VALID(now) && GST_CLOCK_TIME_IS_VALID(base) && now >= base;
  if (after) gst_object_unref(after);
  gst_object_unref(observed);
  if (!valid) impl_->state.decoder_epoch_retired = true;
  require(valid, "preview arrival clock changed");
  return now - base;
}

}  // namespace headunit
