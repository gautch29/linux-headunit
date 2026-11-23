// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Offline, paced H.264 benchmark. Reads only a private UHVP capture; never
// connects to the phone or display. Compile against GStreamer and h264_annexb.
#include "headunit/h264_annexb.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Frame {
  std::uint64_t pts;
  std::vector<std::uint8_t> bytes;
};
std::vector<Frame> read_capture(const char* path) {
  if (std::filesystem::file_size(path) > 8 * 1024 * 1024 + 1024)
    throw std::runtime_error("capture exceeds private diagnostic limit");
  std::ifstream stream(path, std::ios::binary);
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream), {}};
  if (bytes.size() < 5 || std::string(bytes.begin(), bytes.begin() + 5) != std::string("UHVP\1", 5))
    throw std::runtime_error("invalid capture");
  headunit::H264AnnexB converter;
  std::vector<Frame> frames;
  std::uint64_t first = 0;
  for (std::size_t offset = 5; offset < bytes.size();) {
    if (bytes.size() - offset < 13) throw std::runtime_error("truncated record");
    const auto kind = bytes[offset];
    std::uint64_t timestamp = 0;
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 8; ++i)
      timestamp |= std::uint64_t(bytes[offset + 1 + i]) << (8 * i);
    for (unsigned i = 0; i < 4; ++i)
      length |= std::uint32_t(bytes[offset + 9 + i]) << (8 * i);
    offset += 13;
    if (kind > 1 || length > bytes.size() - offset) throw std::runtime_error("invalid record");
    const auto data = std::span<const std::uint8_t>(bytes).subspan(offset, length);
    if (kind == 1)
      converter.set_configuration(data);
    else {
      if (frames.empty()) first = timestamp;
      // Unsigned difference permits NTP-era wrap. Captures have a short bound.
      const auto difference = timestamp - first;
      const auto seconds = difference >> 32;
      if (seconds > 30) throw std::runtime_error("benchmark capture span exceeds 30 seconds");
      const auto ns =
          seconds * 1000000000ULL + ((difference & 0xffffffffULL) * 1000000000ULL >> 32);
      if (!frames.empty() && ns <= frames.back().pts)
        throw std::runtime_error("benchmark requires strictly increasing source timestamps");
      frames.push_back({ns, converter.access_unit(data)});
      if (frames.size() > 1800) throw std::runtime_error("benchmark frame limit exceeded");
    }
    offset += length;
  }
  if (frames.empty()) throw std::runtime_error("no frames");
  return frames;
}
double cpu_seconds() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("cannot query CPU time");
  return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
         (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.;
}
struct Measurements {
  std::mutex mutex;
  std::unordered_map<std::uint64_t, std::size_t> by_pts;
  std::vector<Clock::time_point> submitted;
  std::vector<bool> received;
  std::vector<double> latency, steady, flush;
  std::size_t pushed = 0, decoded = 0, unmatched = 0, duplicate = 0, max_outstanding = 0;
  bool draining = false;
  std::atomic<bool> callback_failed = false;
};
GstPadProbeReturn output_probe(GstPad*, GstPadProbeInfo* info, gpointer context) noexcept {
  const auto now = Clock::now();
  auto& m = *static_cast<Measurements*>(context);
  try {
    std::lock_guard lock(m.mutex);
    const auto found = m.by_pts.find(GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info)));
    ++m.decoded;
    if (found == m.by_pts.end()) {
      ++m.unmatched;
      return GST_PAD_PROBE_OK;
    }
    const auto index = found->second;
    if (m.received[index]) {
      ++m.duplicate;
      return GST_PAD_PROBE_OK;
    }
    m.received[index] = true;
    const double ms = std::chrono::duration<double, std::milli>(now - m.submitted[index]).count();
    m.latency.push_back(ms);
    if (m.draining)
      m.flush.push_back(ms);
    else if (index >= 30)
      m.steady.push_back(ms);
  } catch (...) {
    m.callback_failed = true;
  }
  return GST_PAD_PROBE_OK;
}
void print_distribution(const char* name, std::vector<double> values) {
  std::sort(values.begin(), values.end());
  std::cout << " " << name << "_n=" << values.size();
  if (values.empty()) return;
  const auto p = [&](double q) {
    return values[std::min(values.size() - 1, std::size_t(std::ceil(q * values.size()) - 1))];
  };
  std::cout << " " << name << "_p50_ms=" << p(.5) << " " << name << "_p95_ms=" << p(.95) << " "
            << name << "_max_ms=" << values.back();
}
struct Pipeline {
  GstElement* pipeline = nullptr;
  GstElement* source = nullptr;
  ~Pipeline() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (source) gst_object_unref(source);
    if (pipeline) gst_object_unref(pipeline);
  }
};
void run(const std::vector<Frame>& frames, const std::string& mode) {
  std::string options;
  if (mode == "slice")
    options = " thread-type=slice";
  else if (mode == "single")
    options = " max-threads=1";
  else if (mode != "auto")
    throw std::runtime_error("mode must be auto, slice or single");
  Measurements m;
  m.submitted.resize(frames.size());
  m.received.resize(frames.size());
  m.latency.reserve(frames.size());
  m.steady.reserve(frames.size());
  m.flush.reserve(frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i)
    m.by_pts.emplace(frames[i].pts, i);
  Pipeline p;
  GError* error = nullptr;
  const auto description = std::string("appsrc name=source is-live=true format=time block=false ") +
                           "max-bytes=2097152 ! h264parse ! avdec_h264 name=decoder" + options +
                           " ! fakesink sync=false";
  p.pipeline = gst_parse_launch(description.c_str(), &error);
  if (error) {
    g_error_free(error);
    throw std::runtime_error("pipeline construction failed");
  }
  if (!p.pipeline) throw std::runtime_error("missing pipeline");
  p.source = gst_bin_get_by_name(GST_BIN(p.pipeline), "source");
  if (!p.source) throw std::runtime_error("missing appsrc");
  GstCaps* caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
  g_object_set(p.source, "caps", caps, nullptr);
  gst_caps_unref(caps);
  auto* decoder = gst_bin_get_by_name(GST_BIN(p.pipeline), "decoder");
  auto* pad = gst_element_get_static_pad(decoder, "src");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, output_probe, &m, nullptr);
  gst_object_unref(pad);
  gst_object_unref(decoder);
  if (gst_element_set_state(p.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    throw std::runtime_error("pipeline start failed");
  const auto begin = Clock::now();
  const auto cpu_begin = cpu_seconds();
  std::uint64_t max_queued_bytes = 0;
  double max_pacing_late_ms = 0;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const auto due = begin + std::chrono::nanoseconds(frames[i].pts);
    std::this_thread::sleep_until(due);
    const auto& frame = frames[i];
    auto* buffer = gst_buffer_new_allocate(nullptr, frame.bytes.size(), nullptr);
    if (!buffer) throw std::runtime_error("buffer allocation failed");
    gst_buffer_fill(buffer, 0, frame.bytes.data(), frame.bytes.size());
    GST_BUFFER_PTS(buffer) = frame.pts;
    const auto submitted = Clock::now();
    max_pacing_late_ms = std::max(
        max_pacing_late_ms, std::chrono::duration<double, std::milli>(submitted - due).count());
    {
      std::lock_guard lock(m.mutex);
      m.submitted[i] = submitted;
      ++m.pushed;
      m.max_outstanding = std::max(m.max_outstanding, m.pushed - m.decoded);
    }
    const auto bytes = gst_app_src_get_current_level_bytes(GST_APP_SRC(p.source));
    max_queued_bytes = std::max(max_queued_bytes, bytes);
    if (frame.bytes.size() > 2 * 1024 * 1024 || bytes > 2 * 1024 * 1024 - frame.bytes.size()) {
      gst_buffer_unref(buffer);
      throw std::runtime_error("decoder backlog limit exceeded");
    }
    if (gst_app_src_push_buffer(GST_APP_SRC(p.source), buffer) != GST_FLOW_OK)
      throw std::runtime_error("decoder rejected frame");
  }
  // Allow an ordinary frame interval before EOS. Frames retained longer are
  // marked separately; EOS-induced output is not steady interactive latency.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  {
    std::lock_guard lock(m.mutex);
    m.draining = true;
  }
  if (gst_app_src_end_of_stream(GST_APP_SRC(p.source)) != GST_FLOW_OK)
    throw std::runtime_error("EOS rejected");
  auto* bus = gst_element_get_bus(p.pipeline);
  auto* message = gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND,
                                             GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  gst_object_unref(bus);
  const bool complete = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message) gst_message_unref(message);
  if (!complete) throw std::runtime_error("decode did not complete");
  gst_element_set_state(p.pipeline, GST_STATE_NULL);
  const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
  const double cpu = cpu_seconds() - cpu_begin;
  std::lock_guard lock(m.mutex);
  if (m.callback_failed) throw std::runtime_error("measurement callback failed");
  std::cout << std::fixed << std::setprecision(3) << "mode=" << mode << " submitted=" << m.pushed
            << " decoded=" << m.decoded
            << " missing=" << std::count(m.received.begin(), m.received.end(), false)
            << " unmatched=" << m.unmatched << " duplicate=" << m.duplicate
            << " elapsed_s=" << elapsed << " cpu_s=" << cpu
            << " cpu_one_core_pct=" << 100 * cpu / elapsed << " paced_fps=" << m.decoded / elapsed
            << " max_outstanding=" << m.max_outstanding << " max_appsrc_bytes=" << max_queued_bytes
            << " pacing_late_max_ms=" << max_pacing_late_ms;
  print_distribution("all", m.latency);
  print_distribution("steady", m.steady);
  print_distribution("eos", m.flush);
  std::cout << '\n' << std::flush;
  if (m.decoded != frames.size() || m.unmatched || m.duplicate)
    throw std::runtime_error("decoded frame accounting failed");
}
}  // namespace
int main(int argc, char** argv) try {
  if (argc != 3)
    throw std::runtime_error("usage: benchmark-preview-decode PRIVATE_CAPTURE auto|slice|single");
  const auto frames = read_capture(argv[1]);
  gst_init(nullptr, nullptr);
  run(frames, argv[2]);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
