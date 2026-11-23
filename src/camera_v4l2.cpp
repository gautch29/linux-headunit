// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_capture.hpp>
#include <linux/videodev2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace headunit::camera {
namespace {
// These ioctls are bounded in retries, not in kernel execution time. A wedged
// driver cannot be given a userspace deadline by setting O_NONBLOCK.
constexpr unsigned interrupted_retries = 3, maximum_buffers = 4;
constexpr std::uint32_t capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template <class F>
int interrupted(F call) {
  for (unsigned attempt = 0; attempt < interrupted_retries; ++attempt) {
    const auto result = call();
    if (result >= 0 || errno != EINTR) return result;
  }
  return -1;
}
int control(int fd, unsigned long request, void* value) {
  return interrupted([&] { return ::ioctl(fd, request, value); });
}
void checked_control(int fd, unsigned long request, void* value, const char* message) {
  require(control(fd, request, value) == 0, message);
}
void close_once(int fd) noexcept {
  // On Linux close(EINTR) may already have released the descriptor. Retrying
  // could close a new, unrelated descriptor acquired by another thread.
  if (fd >= 0) (void)::close(fd);
}
std::uint32_t pixel_fourcc(CapturePixelFormat format) {
  switch (format) {
    case CapturePixelFormat::yuyv:
      return V4L2_PIX_FMT_YUYV;
    case CapturePixelFormat::rgb24:
      return V4L2_PIX_FMT_RGB24;
    case CapturePixelFormat::bgr24:
      return V4L2_PIX_FMT_BGR24;
  }
  throw std::runtime_error("unsupported V4L2 capture pixel format");
}
bool canonical_device(std::string_view path) {
  constexpr std::string_view prefix = "/dev/video";
  if (!path.starts_with(prefix) || path.size() <= prefix.size() || path.size() > 64) return false;
  const auto number = path.substr(prefix.size());
  if (number.size() > 1 && number.front() == '0') return false;
  std::uint32_t value = 0;
  for (const auto digit : number) {
    if (digit < '0' || digit > '9' ||
        value > (std::numeric_limits<std::uint32_t>::max() - unsigned(digit - '0')) / 10)
      return false;
    value = value * 10 + unsigned(digit - '0');
  }
  return true;
}
std::uint64_t now_ns() {
  struct timespec clock{};
  require(interrupted([&] { return ::clock_gettime(CLOCK_MONOTONIC, &clock); }) == 0 &&
              clock.tv_sec >= 0 && clock.tv_nsec >= 0 && clock.tv_nsec < 1000000000 &&
              std::uint64_t(clock.tv_sec) <=
                  (std::numeric_limits<std::uint64_t>::max() - std::uint64_t(clock.tv_nsec)) /
                      1000000000,
          "V4L2 capture monotonic clock unavailable");
  return std::uint64_t(clock.tv_sec) * 1000000000 + std::uint64_t(clock.tv_nsec);
}
std::uint64_t timestamp(const timeval& value) {
  require(value.tv_sec >= 0 && value.tv_usec >= 0 && value.tv_usec < 1000000 &&
              std::uint64_t(value.tv_sec) <= (std::numeric_limits<std::uint64_t>::max() -
                                              std::uint64_t(value.tv_usec) * 1000) /
                                                 1000000000,
          "V4L2 capture timestamp is malformed");
  const auto result =
      std::uint64_t(value.tv_sec) * 1000000000 + std::uint64_t(value.tv_usec) * 1000;
  require(result != 0, "V4L2 capture timestamp is absent");
  return result;
}

PackedCaptureFormat packed_format(const v4l2_pix_format& pixels, const Configuration& config) {
  const auto width = config.v4l2->adaptation ? config.v4l2->adaptation->width : config.width;
  const auto height = config.v4l2->adaptation ? config.v4l2->adaptation->height : config.height;
  require(pixels.width == width && pixels.height == height &&
              pixels.pixelformat == pixel_fourcc(config.v4l2->pixel_format) &&
              pixels.field == V4L2_FIELD_NONE && pixels.priv == V4L2_PIX_FMT_PRIV_MAGIC &&
              pixels.flags == 0,
          "V4L2 adjusted requested geometry, pixel layout or progressive storage");
  // Normative DEFAULT values are resolved only from an explicit supported
  // colorspace, never from resolution, a requested format, or an assumed HD.
  const auto color = pixels.colorspace;
  require(color == V4L2_COLORSPACE_SMPTE170M || color == V4L2_COLORSPACE_REC709 ||
              color == V4L2_COLORSPACE_SRGB || color == V4L2_COLORSPACE_JPEG,
          "V4L2 capture colorspace is unknown or unsupported");
  const auto transfer = pixels.xfer_func == V4L2_XFER_FUNC_DEFAULT
                            ? std::uint32_t(V4L2_MAP_XFER_FUNC_DEFAULT(color))
                            : pixels.xfer_func;
  require(transfer == V4L2_XFER_FUNC_709 || transfer == V4L2_XFER_FUNC_SRGB,
          "V4L2 capture transfer function is unsupported");
  const bool rgb = config.v4l2->pixel_format != CapturePixelFormat::yuyv;
  const auto encoding = pixels.ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT
                            ? std::uint32_t(V4L2_MAP_YCBCR_ENC_DEFAULT(color))
                            : pixels.ycbcr_enc;
  const auto quantization = pixels.quantization == V4L2_QUANTIZATION_DEFAULT
                                ? std::uint32_t(V4L2_MAP_QUANTIZATION_DEFAULT(rgb, color, encoding))
                                : pixels.quantization;
  require(
      quantization == V4L2_QUANTIZATION_FULL_RANGE || quantization == V4L2_QUANTIZATION_LIM_RANGE,
      "V4L2 capture quantization is unsupported");
  PackedCaptureFormat format;
  format.width = pixels.width;
  format.height = pixels.height;
  format.stride = pixels.bytesperline;
  format.pixel_format = config.v4l2->pixel_format;
  format.full_range = quantization == V4L2_QUANTIZATION_FULL_RANGE;
  if (rgb) {
    require(config.v4l2->delivery == CaptureDelivery::cpu_copy && color == V4L2_COLORSPACE_SRGB &&
                transfer == V4L2_XFER_FUNC_SRGB && format.full_range,
            "V4L2 RGB capture requires explicit full-range sRGB CPU-copy delivery");
  } else {
    require(encoding == V4L2_YCBCR_ENC_601 || encoding == V4L2_YCBCR_ENC_709,
            "V4L2 capture YCbCr encoding is unsupported");
    format.matrix = encoding == V4L2_YCBCR_ENC_601 ? theme::video::YuvMatrix::bt601
                                                   : theme::video::YuvMatrix::bt709;
  }
  validate_packed_capture_format(format);
  require(pixels.sizeimage >= std::size_t(format.stride) * format.height &&
              pixels.sizeimage <= maximum_packed_capture_storage,
          "V4L2 capture storage is truncated or exceeds its bound");
  return format;
}

struct Slot {
  void* mapping = MAP_FAILED;
  std::size_t bytes = 0;
  int exported = -1;
  ~Slot() {
    // Resource finalizers may run after stop(), on the final lease owner. They
    // only close/unmap retained backing: no driver ioctl or requeue occurs.
    if (mapping != MAP_FAILED) (void)::munmap(mapping, bytes);
    close_once(exported);
  }
};
struct Returns {
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
  std::atomic<std::uint32_t> bits{0};
};
struct SampleLease {
  std::shared_ptr<const Slot> slot;
  std::shared_ptr<Returns> returns;
  unsigned index;
  SampleLease(std::shared_ptr<const Slot> backing, std::shared_ptr<Returns> mailbox,
              unsigned number)
      : slot(std::move(backing)), returns(std::move(mailbox)), index(number) {}
  SampleLease(const SampleLease&) = delete;
  SampleLease& operator=(const SampleLease&) = delete;
  ~SampleLease() { returns->bits.fetch_or(std::uint32_t{1} << index, std::memory_order_release); }
};

class V4l2Device final : public CaptureDevice {
 public:
  explicit V4l2Device(Configuration config)
      : config_(std::move(config)), returns_(std::make_shared<Returns>()) {
    validate_configuration(config_);
    require(config_.enabled && config_.source == SourceKind::v4l2 && config_.v4l2 &&
                canonical_device(config_.v4l2->device),
            "V4L2 capture needs one enabled canonical video device");
    require(config_.v4l2->delivery == CaptureDelivery::cpu_copy ||
                (config_.v4l2->delivery == CaptureDelivery::dma_buf &&
                 config_.v4l2->pixel_format == CapturePixelFormat::yuyv),
            "V4L2 DMA-buf capture currently supports packed YUYV only");
  }
  ~V4l2Device() override { stop(); }
  CaptureDeviceFormat start() override {
    require(!started_, "V4L2 capture adapter cannot restart a retired generation");
    started_ = true;
    try {
      struct stat named{}, opened{};
      require(interrupted([&] { return ::lstat(config_.v4l2->device.c_str(), &named); }) == 0 &&
                  S_ISCHR(named.st_mode),
              "V4L2 configured device is not a real character node");
      fd_ = interrupted([&] {
        return ::open(config_.v4l2->device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
      });
      require(fd_ >= 0, "V4L2 configured device could not be opened");
      require(interrupted([&] { return ::fstat(fd_, &opened); }) == 0 && S_ISCHR(opened.st_mode) &&
                  named.st_dev == opened.st_dev && named.st_ino == opened.st_ino &&
                  named.st_rdev == opened.st_rdev,
              "V4L2 configured device changed during open");
      v4l2_capability capabilities{};
      checked_control(fd_, VIDIOC_QUERYCAP, &capabilities, "V4L2 capability query failed");
      const auto caps = capabilities.device_caps;
      constexpr auto forbidden = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_OUTPUT_MPLANE |
                                 V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE;
      require((capabilities.capabilities & V4L2_CAP_DEVICE_CAPS) &&
                  (capabilities.capabilities & V4L2_CAP_EXT_PIX_FORMAT) &&
                  (caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING)) ==
                      (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING) &&
                  !(caps & forbidden),
              "V4L2 node is not a single-planar streaming camera with extended format metadata");
      v4l2_format selected{};
      selected.type = capture_type;
      selected.fmt.pix.width =
          config_.v4l2->adaptation ? config_.v4l2->adaptation->width : config_.width;
      selected.fmt.pix.height =
          config_.v4l2->adaptation ? config_.v4l2->adaptation->height : config_.height;
      selected.fmt.pix.pixelformat = pixel_fourcc(config_.v4l2->pixel_format);
      selected.fmt.pix.field = V4L2_FIELD_NONE;
      selected.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
      checked_control(fd_, VIDIOC_S_FMT, &selected, "V4L2 exact format negotiation failed");
      format_.pixels = packed_format(selected.fmt.pix, config_);
      sizeimage_ = selected.fmt.pix.sizeimage;
      require(selected.type == capture_type, "V4L2 changed negotiated buffer type");
      v4l2_streamparm requested{};
      requested.type = capture_type;
      requested.parm.capture.timeperframe.numerator = 1;
      requested.parm.capture.timeperframe.denominator = config_.frame_rate;
      checked_control(fd_, VIDIOC_S_PARM, &requested, "V4L2 requested cadence negotiation failed");
      v4l2_streamparm actual{};
      actual.type = capture_type;
      checked_control(fd_, VIDIOC_G_PARM, &actual, "V4L2 actual cadence query failed");
      require(actual.type == capture_type &&
                  (actual.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) &&
                  actual.parm.capture.timeperframe.numerator &&
                  actual.parm.capture.timeperframe.denominator,
              "V4L2 actual frame interval is unavailable");
      format_.interval_numerator = actual.parm.capture.timeperframe.numerator;
      format_.interval_denominator = actual.parm.capture.timeperframe.denominator;
      v4l2_format confirmed{};
      confirmed.type = capture_type;
      checked_control(fd_, VIDIOC_G_FMT, &confirmed, "V4L2 actual format confirmation failed");
      (void)packed_format(confirmed.fmt.pix, config_);
      require(confirmed.type == capture_type &&
                  confirmed.fmt.pix.bytesperline == selected.fmt.pix.bytesperline &&
                  confirmed.fmt.pix.sizeimage == selected.fmt.pix.sizeimage &&
                  confirmed.fmt.pix.colorspace == selected.fmt.pix.colorspace &&
                  confirmed.fmt.pix.ycbcr_enc == selected.fmt.pix.ycbcr_enc &&
                  confirmed.fmt.pix.quantization == selected.fmt.pix.quantization &&
                  confirmed.fmt.pix.xfer_func == selected.fmt.pix.xfer_func,
              "V4L2 capture format changed during cadence negotiation");
      v4l2_requestbuffers buffers{};
      buffers.type = capture_type;
      buffers.memory = V4L2_MEMORY_MMAP;
      buffers.count = maximum_buffers;
      checked_control(fd_, VIDIOC_REQBUFS, &buffers, "V4L2 bounded MMAP buffer allocation failed");
      require(buffers.type == capture_type && buffers.memory == V4L2_MEMORY_MMAP &&
                  buffers.count >= 2 && buffers.count <= maximum_buffers,
              "V4L2 buffer count is outside its two-to-four bound");
      format_.delivery = config_.v4l2->delivery;
      format_.buffer_count = buffers.count;
      for (unsigned index = 0; index < format_.buffer_count; ++index) {
        v4l2_buffer buffer{};
        buffer.type = capture_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        checked_control(fd_, VIDIOC_QUERYBUF, &buffer, "V4L2 buffer storage query failed");
        require(buffer.type == capture_type && buffer.memory == V4L2_MEMORY_MMAP &&
                    buffer.index == index && buffer.length >= sizeimage_ &&
                    buffer.length <= maximum_packed_capture_storage,
                "V4L2 buffer backing is inconsistent or exceeds its bound");
        auto slot = std::make_shared<Slot>();
        slot->bytes = buffer.length;
        slots_[index] = slot;
        format_.buffer_storage_bytes = std::max(format_.buffer_storage_bytes, slot->bytes);
        if (format_.delivery == CaptureDelivery::cpu_copy) {
          for (unsigned attempt = 0; attempt < interrupted_retries; ++attempt) {
            // V4L2 requires this portable protection combination. The exposed
            // span is const; ownership prevents requeue while anyone retains it.
            slot->mapping = ::mmap(nullptr, slot->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                                   buffer.m.offset);
            if (slot->mapping != MAP_FAILED || errno != EINTR) break;
          }
          require(slot->mapping != MAP_FAILED, "V4L2 MMAP capture mapping failed");
        } else {
          v4l2_exportbuffer exported{};
          exported.type = capture_type;
          exported.index = index;
          exported.flags = O_CLOEXEC | O_RDONLY;
          checked_control(fd_, VIDIOC_EXPBUF, &exported,
                          "V4L2 DMA-buf export failed; CPU fallback is disabled");
          slot->exported = exported.fd;
          const auto descriptor_flags =
              interrupted([&] { return ::fcntl(slot->exported, F_GETFD); });
          require(exported.type == capture_type && exported.index == index && exported.plane == 0 &&
                      slot->exported >= 0 && descriptor_flags >= 0 &&
                      (descriptor_flags & FD_CLOEXEC) != 0,
                  "V4L2 exported descriptor is invalid or not close-on-exec");
          struct stat backing{};
          require(interrupted([&] { return ::fstat(slot->exported, &backing); }) == 0 &&
                      backing.st_size >= 0 && std::uint64_t(backing.st_size) >= slot->bytes &&
                      std::uint64_t(backing.st_size) <= maximum_packed_capture_storage,
                  "V4L2 exported backing does not cover the negotiated linear plane");
        }
        queue(index);
      }
      auto type = capture_type;
      streaming_ = true;
      checked_control(fd_, VIDIOC_STREAMON, &type, "V4L2 capture stream could not start");
      return format_;
    } catch (...) {
      stop();
      throw;
    }
  }
  std::optional<CaptureSample> take() override {
    require(fd_ >= 0 && streaming_ && !failed_, "V4L2 capture is stopped or failed");
    try {
      v4l2_buffer buffer{};
      buffer.type = capture_type;
      buffer.memory = V4L2_MEMORY_MMAP;
      if (control(fd_, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) return {};
        if (errno == EPIPE) throw std::runtime_error("V4L2 capture stream ended");
        throw std::runtime_error("V4L2 nonblocking capture dequeue failed");
      }
      require(buffer.type == capture_type && buffer.memory == V4L2_MEMORY_MMAP &&
                  buffer.index < format_.buffer_count && queued_[buffer.index] &&
                  !leased_[buffer.index],
              "V4L2 dequeued an invalid or already leased buffer");
      queued_[buffer.index] = false;
      if (buffer.flags & V4L2_BUF_FLAG_LAST) throw std::runtime_error("V4L2 capture stream ended");
      constexpr auto allowed = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_PREPARED |
                               V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
      require(!(buffer.flags & ~allowed) && buffer.field == V4L2_FIELD_NONE,
              "V4L2 dequeued unexpected ownership flags or interlaced storage");
      CaptureSample sample;
      sample.driver_sequence = buffer.sequence;
      if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
        // Corrupted payload is never leased or timestamp-qualified. The sole
        // capture owner requeues it; the engine records this recoverable drop.
        sample.corrupted = true;
        queue(buffer.index);
        return sample;
      }
      require((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,
              "V4L2 capture timestamp is unknown or copied from an output stream");
      const auto source = buffer.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
      require(source == V4L2_BUF_FLAG_TSTAMP_SRC_EOF || source == V4L2_BUF_FLAG_TSTAMP_SRC_SOE,
              "V4L2 capture timestamp reference is unsupported");
      sample.reference = source == V4L2_BUF_FLAG_TSTAMP_SRC_EOF
                             ? CaptureReference::end_of_frame
                             : CaptureReference::start_of_exposure;
      sample.capture_monotonic_ns = timestamp(buffer.timestamp);
      require(sample.capture_monotonic_ns <= now_ns() &&
                  (!last_capture_ || sample.capture_monotonic_ns > *last_capture_) &&
                  (!reference_ || sample.reference == *reference_),
              "V4L2 capture timestamp is future, repeated or changes reference");
      if (last_sequence_) {
        const std::uint32_t distance = buffer.sequence - *last_sequence_;
        require(distance && distance < 0x80000000U, "V4L2 capture serial is repeated or reverses");
      }
      const auto row_bytes = std::size_t(format_.pixels.width) *
                             (format_.pixels.pixel_format == CapturePixelFormat::yuyv ? 2 : 3);
      const auto visible_end =
          std::size_t(format_.pixels.stride) * (format_.pixels.height - 1) + row_bytes;
      const auto slot = slots_[buffer.index];
      require(slot && buffer.length == slot->bytes && buffer.bytesused >= visible_end &&
                  buffer.bytesused <= sizeimage_ && buffer.bytesused <= slot->bytes,
              "V4L2 capture payload is truncated or exceeds its negotiated storage");
      auto lifetime = std::make_shared<SampleLease>(slot, returns_, buffer.index);
      leased_[buffer.index] = true;
      sample.lifetime = lifetime;
      if (format_.delivery == CaptureDelivery::cpu_copy) {
        const auto packed_bytes =
            std::min(std::size_t(buffer.bytesused),
                     std::size_t(format_.pixels.stride) * format_.pixels.height);
        sample.packed = {static_cast<const std::uint8_t*>(slot->mapping), packed_bytes};
      } else {
        auto native = std::make_shared<theme::video::NativeFrame>();
        native->metadata = {0, sample.capture_monotonic_ns, format_.pixels.width,
                            format_.pixels.height};
        native->storage_width = format_.pixels.width;
        native->storage_height = format_.pixels.height;
        native->fourcc = V4L2_PIX_FMT_YUYV;
        native->modifier = 0;  // DRM_FORMAT_MOD_LINEAR, packed single-plane V4L2 storage.
        native->plane_count = 1;
        native->planes[0] = {slot->exported, 0, format_.pixels.stride};
        native->matrix = format_.pixels.matrix;
        native->full_range = format_.pixels.full_range;
        // Packed 4:2:2 YUYV has horizontally interstitial chroma; no vertical subsampling.
        native->chroma_x_cosited = false;
        native->chroma_y_cosited = false;
        native->lifetime = lifetime;
        sample.native = std::move(native);
      }
      last_capture_ = sample.capture_monotonic_ns;
      last_sequence_ = buffer.sequence;
      reference_ = sample.reference;
      return sample;
    } catch (...) {
      // Let the provider withdraw its queue before potentially blocked STREAMOFF.
      failed_ = true;
      throw;
    }
  }
  void reclaim() override {
    require(!failed_, "V4L2 capture is failed");
    if (fd_ < 0) return;
    try {
      const auto returned = returns_->bits.exchange(0, std::memory_order_acquire);
      require(!(returned & ~((std::uint32_t{1} << format_.buffer_count) - 1)),
              "V4L2 capture return mailbox has an invalid slot");
      for (unsigned index = 0; index < format_.buffer_count; ++index)
        if (returned & (std::uint32_t{1} << index)) {
          require(leased_[index] && !queued_[index], "V4L2 capture returned an unleased buffer");
          leased_[index] = false;
          queue(index);
        }
    } catch (...) {
      // Let the provider withdraw its queue before potentially blocked STREAMOFF.
      failed_ = true;
      throw;
    }
  }
  void stop() noexcept override {
    if (fd_ >= 0) {
      if (streaming_) {
        auto type = capture_type;
        (void)control(fd_, VIDIOC_STREAMOFF, &type);
      }
      close_once(fd_);
      fd_ = -1;
    }
    streaming_ = false;
    // Each outstanding immutable lease retains its own backing/export. Do not
    // REQBUFS(0), unmap leased memory or close a borrowed plane descriptor here.
    for (auto& slot : slots_)
      slot.reset();
  }

 private:
  void queue(unsigned index) {
    require(index < format_.buffer_count && slots_[index] && !queued_[index] && !leased_[index],
            "V4L2 capture attempted to requeue an owned buffer");
    v4l2_buffer buffer{};
    buffer.type = capture_type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    checked_control(fd_, VIDIOC_QBUF, &buffer, "V4L2 capture buffer requeue failed");
    queued_[index] = true;
  }
  Configuration config_;
  CaptureDeviceFormat format_;
  std::uint32_t sizeimage_ = 0;
  int fd_ = -1;
  bool started_ = false, streaming_ = false, failed_ = false;
  std::array<std::shared_ptr<Slot>, maximum_buffers> slots_{};
  std::array<bool, maximum_buffers> queued_{}, leased_{};
  std::shared_ptr<Returns> returns_;
  std::optional<std::uint64_t> last_capture_;
  std::optional<std::uint32_t> last_sequence_;
  std::optional<CaptureReference> reference_;
};
}  // namespace
std::unique_ptr<CaptureDevice> make_v4l2_capture_device(const Configuration& configuration) {
  return std::make_unique<V4l2Device>(configuration);
}
}  // namespace headunit::camera
