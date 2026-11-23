// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/gst_video_preview.hpp"
#include "headunit/gst_preview_output.hpp"
#ifdef HEADUNIT_HAVE_STANDARD_GPU
#include "headunit/standard_preview.hpp"
#endif
#ifdef HEADUNIT_HAVE_WAYLAND_PREVIEW
#include "headunit/wayland_preview.hpp"
#endif

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/navigation.h>

#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <syncstream>
#include <string_view>
#include <stdexcept>

namespace headunit {
namespace {
std::once_flag gst_once;

std::string pipeline_for(const std::string& sink, const std::string& decoder) {
  // Only known factories, never an arbitrary GStreamer pipeline fragment.
  if (decoder != "v4l2h264dec" && decoder != "avdec_h264")
    throw std::invalid_argument("unsupported preview decoder");
  // Gated output supplies original ingress time itself. Automatic DTS at a
  // later source turn can make h264parse replace an old PTS with a new value.
  const bool own_arrival = sink == "fake" || sink == "wayland-touch" || sink == "wayland-gpu";
  std::string pipeline = "appsrc name=source is-live=true do-timestamp=" +
                         std::string(own_arrival ? "false" : "true") +
                         " format=time "
                         "block=false max-bytes=2097152 leaky-type=none ! h264parse ! " +
                         decoder + " name=decoder ! ";
  if (sink == "x11")
    return pipeline +
           "queue max-size-buffers=3 leaky=downstream ! "
           "videoconvert ! ximagesink sync=false";
  if (sink == "wayland" || sink == "wayland-touch" || sink == "wayland-cluster")
    return pipeline + "queue max-size-buffers=3 leaky=downstream ! " +
           (decoder == "avdec_h264" ? "videoconvert ! " : "") +
           (sink == "wayland-touch" ? "valve name=display-gate drop=true"
                                    : "waylandsink name=video-output sync=false");
  if (sink == "fake") return pipeline + "valve name=display-gate drop=true";
  if (sink == "wayland-gpu")
    return pipeline +
           "queue max-size-buffers=3 leaky=downstream ! "
           "videoconvert ! video/x-raw,format=BGRx,interlace-mode=progressive,"
           "pixel-aspect-ratio=1/1,multiview-mode=mono,views=1 ! "
           "valve name=display-gate drop=true";
  throw std::invalid_argument("unknown video preview sink");
}
}  // namespace

void GstVideoPreview::set_navigation(std::shared_ptr<RouteGuidance> source) {
#ifdef HEADUNIT_HAVE_WAYLAND_PREVIEW
  if (wayland_) wayland_->set_navigation(std::move(source));
#else
  (void)source;
#endif
}

GstVideoPreview::GstVideoPreview(const std::string& sink, unsigned width, unsigned height,
                                 const std::string& decoder_name,
                                 const std::string& wayland_display,
                                 MainPreviewOptions main_options)
    : width_(width), height_(height) {
  if (width < 320 || width > 8192 || height < 240 || height > 4320)
    throw std::invalid_argument("invalid preview dimensions");
  if (sink == "wayland-gpu") {
#ifndef HEADUNIT_HAVE_STANDARD_GPU
    throw std::invalid_argument("owned Standard GPU preview is not built");
#else
    if (width > 4096 || height > 2160 || main_options.output_name.empty() ||
        main_options.wayland_display.empty())
      throw std::invalid_argument(
          "owned Standard preview needs bounded dimensions and a named display/output");
    const auto* diagnostics = std::getenv("HEADUNIT_STANDARD_DIAGNOSTICS");
    standard_diagnostics_ = diagnostics && std::string_view(diagnostics) == "1";
#endif
  }
  if (sink != "wayland-gpu" &&
      (main_options.camera_control || !main_options.camera_lease_file.empty() ||
       main_options.native_page_control))
    throw std::invalid_argument("main surface handoff requires the owned Standard presenter");
  if (main_options.native_page_control &&
      (main_options.camera_control || !main_options.camera_lease_file.empty()))
    throw std::invalid_argument("native page and camera ownership cannot be combined");
  std::call_once(gst_once, [] {
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
      if (error) g_error_free(error);
      throw std::runtime_error("video preview initialization failed");
    }
  });
  const auto description = pipeline_for(sink, decoder_name);
  GError* error = nullptr;
  pipeline_ = gst_parse_launch(description.c_str(), &error);
  if (error) {
    g_error_free(error);
    if (pipeline_) gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    throw std::runtime_error("cannot create video preview pipeline");
  }
  if (!pipeline_) throw std::runtime_error("missing video preview pipeline");
  source_ = gst_bin_get_by_name(GST_BIN(pipeline_), "source");
  if (!source_) {
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    throw std::runtime_error("video preview source missing");
  }
  GstCaps* caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
  g_object_set(source_, "caps", caps, nullptr);
  gst_caps_unref(caps);
  GstElement* decoder = gst_bin_get_by_name(GST_BIN(pipeline_), "decoder");
  GstPad* output = gst_element_get_static_pad(decoder, "src");
  gst_pad_add_probe(
      output, GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad*, GstPadProbeInfo*, gpointer context) -> GstPadProbeReturn {
        ++*static_cast<std::atomic<std::size_t>*>(context);
        return GST_PAD_PROBE_OK;
      },
      &decoded_frames_, nullptr);
  gst_object_unref(output);
  gst_object_unref(decoder);
  GstPad* source_pad = gst_element_get_static_pad(source_, "src");
  gst_pad_add_probe(
      source_pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
      [](GstPad*, GstPadProbeInfo* info, gpointer context) -> GstPadProbeReturn {
        GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(event) == GST_EVENT_NAVIGATION) {
          static_cast<GstVideoPreview*>(context)->navigation(event);
          return GST_PAD_PROBE_DROP;
        }
        return GST_PAD_PROBE_OK;
      },
      this, nullptr);
  gst_object_unref(source_pad);
  if (sink == "fake" || sink == "wayland-touch" || sink == "wayland-gpu") {
    GstElement* gate = gst_bin_get_by_name(GST_BIN(pipeline_), "display-gate");
    try {
      output_ = std::make_unique<GstPreviewOutput>(pipeline_, gate);
      if (sink == "wayland-gpu") {
#ifdef HEADUNIT_HAVE_STANDARD_GPU
        StandardPreview::Options options;
        options.presenter.width = width;
        options.presenter.height = height;
        options.presenter.window.display_name = main_options.wayland_display;
        options.presenter.window.output_name = main_options.output_name;
        options.presenter.surface_lease_file = main_options.camera_lease_file;
        options.camera_control = std::move(main_options.camera_control);
        options.presenter.native_page_control = std::move(main_options.native_page_control);
        standard_ = std::make_unique<StandardPreview>(*output_, std::move(options));
#endif
      } else {
        auto* output_sink =
            gst_element_factory_make(sink == "fake" ? "fakesink" : "waylandsink", "video-output");
        if (!output_sink) throw std::runtime_error("video preview output missing");
        // Ingress timestamps require a committed PLAYING clock before the
        // first encoded frame. Waiting for sink preroll would require that
        // same frame first and leave the original decoder epoch unavailable.
        g_object_set(output_sink, "sync", FALSE, "async", FALSE, nullptr);
        output_->start(output_sink);
      }
      gst_object_unref(gate);
    } catch (...) {
      if (gate) gst_object_unref(gate);
      gst_element_set_state(pipeline_, GST_STATE_NULL);
#ifdef HEADUNIT_HAVE_STANDARD_GPU
      standard_.reset();
#endif
      output_.reset();
      gst_object_unref(source_);
      source_ = nullptr;
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
      throw;
    }
  }
#ifdef HEADUNIT_HAVE_WAYLAND_PREVIEW
  if (sink == "wayland-touch" || sink == "wayland-cluster") {
    GstElement* output_sink = gst_bin_get_by_name(GST_BIN(pipeline_), "video-output");
    try {
      wayland_ = std::make_unique<WaylandPreview>(width, height, sink == "wayland-cluster",
                                                  wayland_display);
      wayland_->attach(output_sink);
      gst_object_unref(output_sink);
    } catch (...) {
      if (output_sink) gst_object_unref(output_sink);
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      output_.reset();
      gst_object_unref(source_);
      source_ = nullptr;
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
      throw;
    }
  }
#else
  (void)wayland_display;
  if (sink == "wayland-touch" || sink == "wayland-cluster") {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    output_.reset();
    gst_object_unref(source_);
    source_ = nullptr;
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    throw std::runtime_error("Wayland touch preview is not built");
  }
#endif
  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
#ifdef HEADUNIT_HAVE_STANDARD_GPU
    standard_.reset();
#endif
    output_.reset();
    gst_object_unref(source_);
    source_ = nullptr;
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    throw std::runtime_error("video preview could not start");
  }
}

GstVideoPreview::~GstVideoPreview() {
  if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  standard_.reset();
#endif
  output_.reset();
  if (source_) gst_object_unref(source_);
  if (pipeline_) gst_object_unref(pipeline_);
}

void GstVideoPreview::submit(const AirplayVideoPacket& packet) {
  if (retired_) throw std::runtime_error("video preview is retired");
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  // An ingress observation, not appsrc admission or a phone capture clock.
  if (standard_diagnostics_ && packet.kind == AirplayVideoPacket::Kind::frame) {
    ++standard_encoded_ingress_;
    standard_last_ingress_ns_.reset();
    try {
      standard_last_ingress_ns_ = presentation_monotonic_ns();
    } catch (...) {
    }
    standard_last_ingress_running_ns_.reset();
  }
#endif
  // Capture arrival before any camera return/window handshake in pump(). A
  // packet already here must not become "new" because that work advances the
  // restoration boundary before it reaches appsrc.
  std::optional<std::uint64_t> arrival;
  if (output_ && packet.kind == AirplayVideoPacket::Kind::frame) arrival = output_->arrival_time();
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  if (standard_diagnostics_ && packet.kind == AirplayVideoPacket::Kind::frame)
    standard_last_ingress_running_ns_ = arrival;
  if (standard_) {
    check_pipeline_error();
    standard_->pump();
  }
#endif
#ifdef HEADUNIT_HAVE_WAYLAND_PREVIEW
  if (wayland_) wayland_->pump();
#endif
  if (packet.kind == AirplayVideoPacket::Kind::config) {
    if (!configuration_.empty() && configuration_ != packet.data)
      throw std::runtime_error("video configuration changed; preview restart required");
    converter_.set_configuration(packet.data);
    configuration_ = packet.data;
    return;
  }
  if (packet.kind != AirplayVideoPacket::Kind::frame) return;
  const auto data = converter_.access_unit(packet.data);
  const auto now = std::chrono::steady_clock::now();
  if (!first_frame_) first_frame_ = now;
  // A reconnect can deliver a short encoded burst while both
  // decoders and windows start. Keep every reference within the same byte
  // bound; return to the smaller steady-state frame bound after five seconds.
  const bool starting = now - *first_frame_ < std::chrono::seconds(5);
  const auto frame_limit = starting ? 128u : 32u;
  const auto queued_bytes = gst_app_src_get_current_level_bytes(GST_APP_SRC(source_));
  const auto queued_frames = gst_app_src_get_current_level_buffers(GST_APP_SRC(source_));
  if (data.size() > 2 * 1024 * 1024 || queued_bytes > 2 * 1024 * 1024 - data.size() ||
      queued_frames >= frame_limit) {
    // Losing a compressed reference frame corrupts following deltas. Stop this
    // optional preview explicitly instead of silently leaking encoded buffers.
    throw std::runtime_error(
        "video preview decoder backlog exceeded limit: bytes=" + std::to_string(queued_bytes) +
        " frames=" + std::to_string(queued_frames) +
        " decoded=" + std::to_string(decoded_frames()) + " startup=" + (starting ? "yes" : "no"));
  }
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, data.size(), nullptr);
  if (!buffer) throw std::runtime_error("video preview buffer allocation failed");
  gst_buffer_fill(buffer, 0, data.data(), data.size());
  if (arrival) {
    // Stamp BEFORE appsrc's bounded queue. A later display restoration must
    // not turn an already queued encoded frame into a new arrival.
    GST_BUFFER_PTS(buffer) = *arrival;
    // Both fields belong to this same local ingress domain. Leaving DTS
    // missing permits parser/source timestamp inference to erase provenance.
    // Neither is an iPhone capture, decode deadline or presentation timestamp.
    GST_BUFFER_DTS(buffer) = *arrival;
  }
  if (gst_app_src_push_buffer(GST_APP_SRC(source_), buffer) != GST_FLOW_OK) {
    throw std::runtime_error("video preview decoder rejected frame");
  }
  check_pipeline_error();
}

void GstVideoPreview::check_pipeline_error() {
  if (pipeline_failed_) throw std::runtime_error("video preview pipeline is faulted");
  if (eos_seen_) throw std::runtime_error("video preview source ended");
  GstBus* bus = gst_element_get_bus(pipeline_);
  // One bounded bus owner preserves actual EOS/clock loss even while Home has
  // withdrawn the window. Filtered ERROR pops would discard those messages.
  for (unsigned count = 0; count < 64; ++count) {
    GstMessage* message = gst_bus_pop(bus);
    if (!message) break;
    const auto type = GST_MESSAGE_TYPE(message);
    if (type == GST_MESSAGE_ERROR || type == GST_MESSAGE_CLOCK_LOST) pipeline_failed_ = true;
    if (type == GST_MESSAGE_EOS) eos_seen_ = true;
    gst_message_unref(message);
  }
  gst_object_unref(bus);
  if (pipeline_failed_) throw std::runtime_error("video preview pipeline reported an error");
  if (eos_seen_) throw std::runtime_error("video preview source ended");
}

std::size_t GstVideoPreview::finish(unsigned timeout_ms) {
  if (eos_seen_) return decoded_frames();
  if (gst_app_src_end_of_stream(GST_APP_SRC(source_)) != GST_FLOW_OK)
    throw std::runtime_error("video preview EOS rejected");
  GstBus* bus = gst_element_get_bus(pipeline_);
  GstMessage* message =
      gst_bus_timed_pop_filtered(bus, static_cast<GstClockTime>(timeout_ms) * GST_MSECOND,
                                 static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  gst_object_unref(bus);
  const bool complete = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  eos_seen_ = complete;
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) pipeline_failed_ = true;
  if (message) gst_message_unref(message);
  if (!complete) throw std::runtime_error("video preview did not finish decoding");
  return decoded_frames();
}

void GstVideoPreview::navigation(GstEvent* event) {
  const auto type = gst_navigation_event_get_type(event);
  double x = 0, y = 0;
  int button = 0;
  std::lock_guard lock(touch_mutex_);
  if (type == GST_NAVIGATION_EVENT_MOUSE_BUTTON_PRESS ||
      type == GST_NAVIGATION_EVENT_MOUSE_BUTTON_RELEASE) {
    if (!gst_navigation_event_parse_mouse_button_event(event, &button, &x, &y) || button != 1)
      return;
    pointer_down_ = type == GST_NAVIGATION_EVENT_MOUSE_BUTTON_PRESS;
  } else if (type == GST_NAVIGATION_EVENT_MOUSE_MOVE) {
    if (!pointer_down_ || !gst_navigation_event_parse_mouse_move_event(event, &x, &y)) return;
  } else
    return;
  if (!std::isfinite(x) || !std::isfinite(y)) return;
  const TouchInput input{pointer_down_,
                         static_cast<std::uint16_t>(std::clamp(x, 0.0, double(width_ - 1))),
                         static_cast<std::uint16_t>(std::clamp(y, 0.0, double(height_ - 1)))};
  if (pending_touches_.size() >= 64)
    pending_touches_.back() = input;
  else
    pending_touches_.push_back(input);
}

int GstVideoPreview::input_fd() const noexcept {
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  if (standard_) return standard_->input_fd();
#endif
#ifdef HEADUNIT_HAVE_WAYLAND_PREVIEW
  if (wayland_) return wayland_->input_fd();
#endif
  return -1;
}

std::vector<TouchInput> GstVideoPreview::take_touches() {
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  if (standard_)
    throw std::logic_error("owned Standard input requires a retaining sender callback");
#endif
  if (!retired_) check_pipeline_error();
#ifdef HEADUNIT_HAVE_WAYLAND_PREVIEW
  if (wayland_) return wayland_->take_touches();
#endif
  std::lock_guard lock(touch_mutex_);
  std::vector<TouchInput> result;
  result.swap(pending_touches_);
  return result;
}

unsigned GstVideoPreview::drain_touches(const std::function<bool(TouchInput)>& sender) {
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  if (standard_) {
    if (!retired_) check_pipeline_error();
    return standard_->drain_touches(sender);
  }
#endif
  // VideoOutput retains legacy reports in its own mailbox.
  (void)sender;
  throw std::logic_error("retaining preview input requires the owned Standard presenter");
}

void GstVideoPreview::retire() noexcept {
  if (retired_) return;
  retired_ = true;
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  if (standard_) standard_->retire();
#endif
  if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
}

#ifdef HEADUNIT_HAVE_STANDARD_GPU
void GstVideoPreview::trace_standard_return(bool epoch_ready, bool healthy, bool visible) noexcept {
  try {
    if (!standard_diagnostics_ || standard_diagnostic_reports_ >= 64 || !standard_ || !output_)
      return;
    const auto state = standard_->status();
    const auto& worker = state.presenter;
    const auto gate = output_->stats();
    standard_diagnostic_home_seen_ |= state.native_page_active;
    if (!standard_diagnostic_home_seen_) return;
    const auto now = presentation_monotonic_ns();
    const auto flags =
        std::uint64_t(state.stage) | (std::uint64_t(state.native_page_active) << 3) |
        (std::uint64_t(worker.window_ready) << 4) | (std::uint64_t(worker.source_armed) << 5) |
        (std::uint64_t(worker.source_suspended) << 6) | (std::uint64_t(worker.touch_ready) << 7) |
        (std::uint64_t(worker.faulted) << 8) | (std::uint64_t(gate.draining) << 9) |
        (std::uint64_t(gate.barrier_retired) << 10) |
        (std::uint64_t(gate.decoder_epoch_retired) << 11) | (std::uint64_t(healthy) << 12) |
        (std::uint64_t(visible) << 13) | (std::uint64_t(epoch_ready) << 14);
    const bool changed = !standard_diagnostic_reports_ || flags != standard_diagnostic_flags_ ||
                         worker.window != standard_diagnostic_window_ ||
                         worker.requested != standard_diagnostic_requested_;
    // Phase transitions are immediate. Only an unready return needs periodic
    // counters; steady Home and ready video consume no ongoing trace budget.
    const bool unready_return =
        !state.native_page_active && !visible && state.stage != StandardPreview::Stage::faulted;
    if (!changed && (!unready_return || now < standard_diagnostic_next_ns_)) return;
    standard_diagnostic_flags_ = flags;
    standard_diagnostic_window_ = worker.window;
    standard_diagnostic_requested_ = worker.requested;
    standard_diagnostic_next_ns_ = now + 250000000;
    const char* stages[] = {"initial", "visible", "hidden", "returning", "faulted"};
    std::osyncstream log(std::cerr);
    log << "Standard return diagnostic event=" << ++standard_diagnostic_reports_
        << " mono_ns=" << now << " stage=" << stages[unsigned(state.stage)]
        << " nativeHome=" << state.native_page_active
        << " windowGeneration=" << state.window_generation << " window=" << worker.window
        << " requested=" << worker.requested << " retired=" << worker.retired
        << " windowReady=" << worker.window_ready << " sourceArmed=" << worker.source_armed
        << " sourceSuspended=" << worker.source_suspended << " touchReady=" << worker.touch_ready
        << " epochReady=" << epoch_ready << " healthy=" << healthy << " visible=" << visible
        << " workerFault=" << worker.faulted << " encodedIngress=" << standard_encoded_ingress_
        << " ingressMonoNs=";
    if (standard_last_ingress_ns_)
      log << *standard_last_ingress_ns_;
    else
      log << "none";
    log << " ingressRunningNs=";
    if (standard_last_ingress_running_ns_)
      log << *standard_last_ingress_running_ns_;
    else
      log << "none";
    log << " decoded=" << decoded_frames() << " gateGeneration=" << gate.generation
        << " gateForwarded=" << gate.forwarded << " gateRejected=" << gate.rejected
        << " gateDraining=" << gate.draining << " barrierRetired=" << gate.barrier_retired
        << " decoderEpochRetired=" << gate.decoder_epoch_retired << " sourceFloor=";
    if (gate.arrival_floor)
      log << *gate.arrival_floor;
    else
      log << "none";
    log << " accepted=" << worker.accepted << " rejected=" << worker.rejected
        << " submitted=" << worker.submitted << " presented=" << worker.presented
        << " ownerMonoNs=" << worker.observed_monotonic_ns << " presentedArrivalNs=";
    if (worker.presented_arrival_running_ns)
      log << *worker.presented_arrival_running_ns;
    else
      log << "none";
    log << '\n';
  } catch (...) { /* Bench logging cannot change presentation or retirement. */
  }
}
#endif

MainPresentationStatus GstVideoPreview::main_presentation_status() {
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  if (!retired_ && standard_ && output_) {
    try {
      check_pipeline_error();
      const bool epoch_ready = output_->restore_ready();
      const auto gate = output_->stats();
      const auto state = standard_->status();
      const auto& worker = state.presenter;
      const auto now = presentation_monotonic_ns();
      const bool owner_fresh = worker.observed_monotonic_ns &&
                               worker.observed_monotonic_ns <= now &&
                               now - worker.observed_monotonic_ns <= 1000000000;
      const bool healthy = epoch_ready && !gate.decoder_epoch_retired && !gate.barrier_retired &&
                           owner_fresh && !worker.faulted &&
                           state.stage != StandardPreview::Stage::faulted;
      const bool visible = healthy && state.stage == StandardPreview::Stage::visible &&
                           worker.touch_ready && worker.window_ready &&
                           worker.window == state.window_generation &&
                           worker.requested == state.window_generation &&
                           worker.presented_arrival_running_ns.has_value() &&
                           worker.presented > 0 && !worker.source_suspended;
      if (visible) main_presented_ = true;
      if (!healthy) main_presented_ = false;
      const bool home = state.native_page_active && !state.camera_active &&
                        state.stage == StandardPreview::Stage::hidden;
      // Count each departure from Home so the control owner can request a
      // keyframe; the returned window only accepts frames after its boundary.
      if (home)
        main_hidden_ = true;
      else if (main_hidden_ && state.stage != StandardPreview::Stage::hidden) {
        main_hidden_ = false;
        ++main_returns_;
      }
      trace_standard_return(epoch_ready, healthy, visible);
      return {visible || (home && healthy && main_presented_), healthy, home, main_returns_};
    } catch (...) {
      main_presented_ = false;
      retire();
      trace_standard_return(false, false, false);
      return {};
    }
  }
#endif
  return {};
}

}  // namespace headunit
