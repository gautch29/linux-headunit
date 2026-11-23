// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/gst_video_preview.hpp>
#include <headunit/theme/receiver_clock.hpp>
#include <headunit/video_output.hpp>
#include <headunit/touch_mailbox.hpp>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace headunit {
namespace {
constexpr std::size_t max_video_capture_per_screen = 8 * 1024 * 1024;
}

struct VideoOutput::Impl {
 public:
  explicit Impl(VideoOutput::Options options)
      : directory_(std::move(options.capture_directory)),
        connection_number_(options.connection_number),
        preview_sink_(std::move(options.preview_sink)),
        preview_decoder_(std::move(options.preview_decoder)),
        main_display_(options.main_display),
        cluster_display_(options.cluster_display),
        main_preview_(std::move(options.main_preview)),
        standard_cluster_wayland_(std::move(options.standard_cluster_wayland)) {
    legacy_generation_ = legacy_touches_.begin();
    if (!options.defer_preview) select_mode(CarplayMode::standard);
  }
  void select_mode(std::optional<CarplayMode> mode) {
    if (!mode) return;
    if (*mode != CarplayMode::standard && *mode != CarplayMode::ultra)
      throw std::invalid_argument("invalid presentation mode");
    if (mode_) {
      if (mode_ != mode) throw std::logic_error("presentation mode changed within one session");
      return;
    }
    mode_ = mode;
    if (mode_ == CarplayMode::standard && !standard_cluster_wayland_.empty() &&
        preview_sink_ != "none") {
      try {
        slots_[1].preview = std::make_unique<GstVideoPreview>(
            "wayland-cluster", cluster_display_.width, cluster_display_.height, preview_decoder_,
            standard_cluster_wayland_);
        if (navigation_) slots_[1].preview->set_navigation(navigation_);
      } catch (const std::exception& error) {
        stop_preview(1, error.what());
      }
    }
  }
  ~Impl() {
    for (auto& slot : slots_)
      if (slot.fd >= 0) close(slot.fd);
  }

  unsigned drain_touches(const std::function<bool(TouchInput)>& sender) {
    if (slots_[1].preview && !slots_[1].preview_failed) {
      try {
        (void)slots_[1].preview->take_touches();
      } catch (const std::exception& error) {
        stop_preview(1, error.what());
      }
    }
    auto& main = slots_[0];
    if (preview_sink_ == "wayland-gpu") {
      if (!main.preview) return 0;
      bool sender_failed = false;
      try {
        return main.preview->drain_touches([&](TouchInput touch) {
          try {
            return sender(touch);
          } catch (...) {
            sender_failed = true;
            throw;
          }
        });
      } catch (const std::exception& error) {
        if (sender_failed) throw;
        stop_preview(0, error.what());
        return 0;  // Retained owner retries its cancellation on the next turn.
      }
    }
    if (main.preview && !main.preview_failed) {
      try {
        const auto reports = main.preview->take_touches();
        legacy_touches_.push(legacy_generation_, reports);
      } catch (const std::exception& error) {
        stop_preview(0, error.what());
      }
    }
    return legacy_touches_.drain(sender);
  }

  void append_input_poll_descriptor(std::vector<pollfd>& descriptors) const {
    if (slots_[0].preview) {
      const int fd = slots_[0].preview->input_fd();
      if (fd >= 0) descriptors.push_back({fd, POLLIN, 0});
    }
    if (preview_sink_ != "none" && preview_sink_ != "wayland-gpu")
      descriptors.push_back({legacy_touches_.wake_fd(), POLLIN, 0});
  }

  void set_navigation(std::shared_ptr<headunit::RouteGuidance> source) {
    navigation_ = std::move(source);
    if (slots_[1].preview) slots_[1].preview->set_navigation(navigation_);
  }

  void update_clock(const std::optional<headunit::AirplayClockSample>& timing) {
    if (clock_failed_) return;
    try {
      receiver_clock_.update(timing);
    } catch (const std::exception& error) {
      clock_failed_ = true;
      std::cout << "video clock diagnostic stopped: " << error.what() << '\n' << std::flush;
    }
  }

  void record(std::uint64_t stream_type, const headunit::AirplayVideoPacket& packet,
              const std::optional<headunit::AirplayClockSample>& timing) {
    if (packet.kind != headunit::AirplayVideoPacket::Kind::config &&
        packet.kind != headunit::AirplayVideoPacket::Kind::frame)
      return;
    if (stream_type != 110 && stream_type != 111) return;
    auto& slot = slots_[stream_type - 110];
    if (packet.kind == headunit::AirplayVideoPacket::Kind::config)
      std::cout << "video timing flags type=" << stream_type << " flags=" << packet.flags << '\n'
                << std::flush;
    if (packet.kind == headunit::AirplayVideoPacket::Kind::frame && !clock_failed_) {
      update_clock(timing);
      try {
        if (!clock_failed_ && timing && packet.sender_ntp && !slot.clock_checked &&
            receiver_clock_.clock().ready(timing->monotonic_ns)) {
          const auto mapped =
              receiver_clock_.clock().schedule(*packet.sender_ntp, timing->monotonic_ns);
          slot.clock_checked = true;
          std::cout << "video clock mapped type=" << stream_type << " lead_ns=" << mapped.lead_ns
                    << " uncertainty_ns=" << mapped.uncertainty_ns << '\n'
                    << std::flush;
        }
      } catch (const headunit::theme::video::FrameOutsideWindow& error) {
        if (slot.clock_rejections < 3) {
          ++slot.clock_rejections;
          std::cout << "video outside clock window type=" << stream_type
                    << " lead_ns=" << error.lead_ns << '\n'
                    << std::flush;
        }
      } catch (const std::exception& error) {
        clock_failed_ = true;
        std::cout << "video clock diagnostic stopped: " << error.what() << '\n' << std::flush;
      }
    }
    // One physical touchscreen: keep navigation transport/capture active without
    // creating a second fullscreen window over the main view.
    if (mode_ == CarplayMode::standard && preview_sink_ != "none" && !slot.preview_failed &&
        (!((preview_sink_ == "wayland-touch" || preview_sink_ == "wayland-gpu") &&
           stream_type == 111) ||
         slot.preview)) {
      try {
        if (!slot.preview && packet.kind == headunit::AirplayVideoPacket::Kind::config)
          slot.preview = std::make_unique<GstVideoPreview>(
              preview_sink_, stream_type == 110 ? main_display_.width : cluster_display_.width,
              stream_type == 110 ? main_display_.height : cluster_display_.height, preview_decoder_,
              stream_type == 110 ? main_preview_.wayland_display : std::string{},
              stream_type == 110 ? main_preview_ : MainPreviewOptions{});
        if (slot.preview) slot.preview->submit(packet);
      } catch (const std::exception& error) {
        stop_preview(static_cast<unsigned>(stream_type - 110), error.what());
      }
    }
    if (slot.capture_stopped) return;
    constexpr std::size_t header_size = 13;
    const auto overhead = header_size + (slot.fd < 0 ? 5 : 0);
    if (packet.data.size() > max_video_capture_per_screen - overhead ||
        slot.bytes > max_video_capture_per_screen - overhead - packet.data.size()) {
      slot.capture_stopped = true;
      return;
    }
    try {
      if (slot.fd < 0) {
        const auto path = directory_ + "/video-" + std::to_string(connection_number_) + "-" +
                          std::to_string(stream_type) + ".packets";
        slot.fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (slot.fd < 0) throw std::runtime_error("cannot create video capture");
        constexpr std::array<std::uint8_t, 5> magic{'U', 'H', 'V', 'P', 1};
        write_exact(slot.fd, magic.data(), magic.size());
        slot.bytes += magic.size();
      }
      std::array<std::uint8_t, header_size> header{};
      header[0] = packet.kind == headunit::AirplayVideoPacket::Kind::config ? 1 : 0;
      for (unsigned i = 0; i < 8; ++i)
        header[1 + i] = static_cast<std::uint8_t>(packet.timestamp >> (8 * i));
      for (unsigned i = 0; i < 4; ++i)
        header[9 + i] = static_cast<std::uint8_t>(packet.data.size() >> (8 * i));
      write_exact(slot.fd, header.data(), header.size());
      write_exact(slot.fd, packet.data.data(), packet.data.size());
      slot.bytes += header.size() + packet.data.size();
    } catch (const std::exception&) {
      slot.capture_stopped = true;
      if (slot.fd >= 0) {
        (void)ftruncate(slot.fd, static_cast<off_t>(slot.bytes));
        close(slot.fd);
        slot.fd = -1;
      }
      std::cout << "video capture stopped type=" << stream_type << '\n' << std::flush;
    }
  }

  MainPresentationStatus main_presentation_status() {
    auto& slot = slots_[0];
    if (preview_sink_ != "wayland-gpu" || slot.preview_failed || !slot.preview) return {};
    return slot.preview->main_presentation_status();
  }

 private:
  struct Slot {
    int fd = -1;
    std::size_t bytes = 0;
    bool preview_failed = false;
    bool clock_checked = false;
    unsigned clock_rejections = 0;
    bool capture_stopped = false;
    std::unique_ptr<headunit::GstVideoPreview> preview;
  };
  void stop_preview(unsigned index, const char* error) {
    auto& slot = slots_[index];
    if (slot.preview_failed) return;
    slot.preview_failed = true;
    if (index == 0 && preview_sink_ == "wayland-gpu") {
      // Keep the owned mailbox and poll descriptor until the accepted contact's
      // cancellation is sent or the phone connection closes.
      if (slot.preview) slot.preview->retire();
    } else {
      if (index == 0) legacy_touches_.retire();
      slot.preview.reset();
    }
    std::cout << "video preview stopped type=" << 110 + index << ": " << error << '\n'
              << std::flush;
  }
  static void write_exact(int fd, const std::uint8_t* bytes, std::size_t length) {
    while (length) {
      const auto count = write(fd, bytes, length);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw std::runtime_error("video capture write failed");
      bytes += count;
      length -= static_cast<std::size_t>(count);
    }
  }
  std::string directory_;
  unsigned connection_number_;
  std::string preview_sink_;
  std::string preview_decoder_;
  AirplayDisplaySize main_display_, cluster_display_;
  MainPreviewOptions main_preview_;
  std::string standard_cluster_wayland_;
  std::shared_ptr<RouteGuidance> navigation_;
  std::optional<CarplayMode> mode_;
  std::array<Slot, 2> slots_{};
  TouchMailbox legacy_touches_;
  TouchMailbox::Generation legacy_generation_ = 0;
  theme::video::ReceiverClock receiver_clock_;
  bool clock_failed_ = false;
};

VideoOutput::VideoOutput(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
VideoOutput::~VideoOutput() = default;
unsigned VideoOutput::drain_touches(const std::function<bool(TouchInput)>& sender) {
  return impl_->drain_touches(sender);
}
void VideoOutput::append_input_poll_descriptor(std::vector<pollfd>& descriptors) const {
  impl_->append_input_poll_descriptor(descriptors);
}
void VideoOutput::set_navigation(std::shared_ptr<RouteGuidance> source) {
  impl_->set_navigation(std::move(source));
}
void VideoOutput::select_mode(std::optional<CarplayMode> mode) {
  impl_->select_mode(mode);
}
void VideoOutput::update_clock(const std::optional<AirplayClockSample>& timing) {
  impl_->update_clock(timing);
}
MainPresentationStatus VideoOutput::main_presentation_status() {
  return impl_->main_presentation_status();
}
void VideoOutput::record(std::uint64_t type, const AirplayVideoPacket& packet,
                         const std::optional<AirplayClockSample>& timing) {
  impl_->record(type, packet, timing);
}

}  // namespace headunit
