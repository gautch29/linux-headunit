// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Compiles the production driver with only its POSIX/kernel boundary replaced.
// This is a syscall-model test, not Linux ABI, device, DMA import or GPU proof.
#include <headunit/camera_capture.hpp>
#include <linux/videodev2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace fixture {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
template <class F>
void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "expected driver rejection");
}
struct Kernel {
  static constexpr int device_fd = 91, export_base = 900;
  static constexpr unsigned slots = 4, offset_step = 65536;
  std::thread::id owner = std::this_thread::get_id();
  std::uint64_t now = 10'000'000'000ULL;
  std::uint32_t caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_EXT_PIX_FORMAT;
  std::uint32_t capabilities_extra = V4L2_CAP_DEVICE_CAPS;
  unsigned count = slots, query_bytes = 12288;
  std::uint32_t interval_numerator = 1001, interval_denominator = 30000;
  bool character = true, changed_node = false, opened = false, streaming = false;
  int mmap_failure_index = -1, export_failure_index = -1, descriptor_flags = FD_CLOEXEC;
  unsigned long fail_request = 0, interrupt_request = 0;
  unsigned interruptions = 0;
  int dequeue_errno = EAGAIN;
  std::function<void(v4l2_pix_format&)> adjust, confirm_adjust;
  v4l2_pix_format format{};
  std::array<std::vector<std::uint8_t>, slots> storage;
  std::array<bool, slots> queued{}, mapped{}, exported{};
  std::deque<v4l2_buffer> dequeues;
  std::atomic<unsigned> calls{0}, opens{0}, qbufs{0}, streamoffs{0}, closes{0}, unmaps{0},
      violations{0};
  Kernel() {
    for (auto& bytes : storage)
      bytes.resize(query_bytes);
  }
  bool owner_call() {
    ++calls;
    if (owner == std::this_thread::get_id()) return true;
    ++violations;
    errno = EPERM;
    return false;
  }
  v4l2_buffer frame(unsigned index = 0, std::uint32_t serial = 1,
                    std::uint64_t time = 9'000'000'000ULL) const {
    v4l2_buffer value{};
    value.index = index;
    value.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    value.memory = V4L2_MEMORY_MMAP;
    value.length = query_bytes;
    value.bytesused = format.sizeimage;
    value.field = V4L2_FIELD_NONE;
    value.sequence = serial;
    value.flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
    value.timestamp.tv_sec = static_cast<decltype(value.timestamp.tv_sec)>(time / 1'000'000'000ULL);
    value.timestamp.tv_usec =
        static_cast<decltype(value.timestamp.tv_usec)>((time % 1'000'000'000ULL) / 1000);
    return value;
  }
  bool released() const {
    return !opened && !streaming &&
           std::none_of(mapped.begin(), mapped.end(), [](bool n) { return n; }) &&
           std::none_of(exported.begin(), exported.end(), [](bool n) { return n; });
  }
};
Kernel* kernel = nullptr;
struct Scope {
  Kernel state;
  Scope() {
    check(!kernel, "nested fake kernel");
    kernel = &state;
  }
  ~Scope() { kernel = nullptr; }
};
}  // namespace fixture

int fixture_lstat(const char* path, struct stat* value) {
  auto& k = *fixture::kernel;
  if (!k.owner_call()) return -1;
  fixture::check(std::string_view(path) == "/dev/video7", "probed another device");
  *value = {};
  value->st_mode = k.character ? S_IFCHR : S_IFLNK;
  value->st_dev = 3;
  value->st_ino = 7;
  value->st_rdev = 81;
  return 0;
}
int fixture_open(const char* path, int flags) {
  auto& k = *fixture::kernel;
  if (!k.owner_call()) return -1;
  fixture::check(std::string_view(path) == "/dev/video7" &&
                     flags == (O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW),
                 "capture open lacks exact security flags");
  ++k.opens;
  k.opened = true;
  return fixture::Kernel::device_fd;
}
int fixture_fstat(int fd, struct stat* value) {
  auto& k = *fixture::kernel;
  if (!k.owner_call()) return -1;
  *value = {};
  if (fd == fixture::Kernel::device_fd) {
    value->st_mode = S_IFCHR;
    value->st_dev = 3;
    value->st_ino = k.changed_node ? 8 : 7;
    value->st_rdev = 81;
  } else {
    fixture::check(
        fd >= fixture::Kernel::export_base && fd < fixture::Kernel::export_base + int(k.count),
        "invalid exported stat");
    value->st_size = k.query_bytes;
  }
  return 0;
}
int fixture_ioctl(int fd, unsigned long request, void* data) {
  auto& k = *fixture::kernel;
  if (!k.owner_call()) return -1;
  fixture::check(fd == fixture::Kernel::device_fd && k.opened, "ioctl after capture closed");
  if (request == k.interrupt_request && k.interruptions) {
    --k.interruptions;
    errno = EINTR;
    return -1;
  }
  if (request == k.fail_request) {
    errno = EIO;
    return -1;
  }
  if (request == VIDIOC_QUERYCAP) {
    auto& cap = *static_cast<v4l2_capability*>(data);
    cap.device_caps = k.caps;
    cap.capabilities = k.caps | k.capabilities_extra;
  } else if (request == VIDIOC_S_FMT) {
    auto& value = *static_cast<v4l2_format*>(data);
    fixture::check(value.type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
                       value.fmt.pix.field == V4L2_FIELD_NONE &&
                       value.fmt.pix.priv == V4L2_PIX_FMT_PRIV_MAGIC,
                   "wrong exact format request");
    auto& p = value.fmt.pix;
    p.bytesperline = p.width * (p.pixelformat == V4L2_PIX_FMT_YUYV ? 2 : 3) + 16;
    p.sizeimage = p.bytesperline * p.height + 128;
    p.colorspace =
        p.pixelformat == V4L2_PIX_FMT_YUYV ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SRGB;
    if (k.adjust) k.adjust(p);
    k.format = p;
  } else if (request == VIDIOC_G_FMT) {
    auto& value = *static_cast<v4l2_format*>(data);
    value.fmt.pix = k.format;
    if (k.confirm_adjust) k.confirm_adjust(value.fmt.pix);
  } else if (request == VIDIOC_S_PARM) {
    auto& value = *static_cast<v4l2_streamparm*>(data);
    fixture::check(value.type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
                       value.parm.capture.timeperframe.numerator == 1 &&
                       value.parm.capture.timeperframe.denominator == 30,
                   "wrong requested cadence");
  } else if (request == VIDIOC_G_PARM) {
    auto& value = *static_cast<v4l2_streamparm*>(data);
    value.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    value.parm.capture.timeperframe = {k.interval_numerator, k.interval_denominator};
  } else if (request == VIDIOC_REQBUFS) {
    auto& value = *static_cast<v4l2_requestbuffers*>(data);
    fixture::check(value.count == 4 && value.memory == V4L2_MEMORY_MMAP &&
                       value.type == V4L2_BUF_TYPE_VIDEO_CAPTURE,
                   "unbounded allocation or wrong buffer type");
    value.count = k.count;
  } else if (request == VIDIOC_QUERYBUF) {
    auto& value = *static_cast<v4l2_buffer*>(data);
    value.length = k.query_bytes;
    value.m.offset = value.index * fixture::Kernel::offset_step;
  } else if (request == VIDIOC_EXPBUF) {
    auto& value = *static_cast<v4l2_exportbuffer*>(data);
    fixture::check(value.flags == (O_CLOEXEC | O_RDONLY) && value.plane == 0,
                   "wrong DMA export flags");
    if (int(value.index) == k.export_failure_index) {
      errno = EIO;
      return -1;
    }
    k.exported.at(value.index) = true;
    value.fd = fixture::Kernel::export_base + int(value.index);
  } else if (request == VIDIOC_QBUF) {
    auto& value = *static_cast<v4l2_buffer*>(data);
    fixture::check(value.index < k.count && !k.queued.at(value.index),
                   "requeued a buffer still owned by driver");
    k.queued.at(value.index) = true;
    ++k.qbufs;
  } else if (request == VIDIOC_DQBUF) {
    if (k.dequeues.empty()) {
      errno = k.dequeue_errno;
      return -1;
    }
    auto& value = *static_cast<v4l2_buffer*>(data);
    value = k.dequeues.front();
    k.dequeues.pop_front();
    if (value.index < k.count) k.queued.at(value.index) = false;
  } else if (request == VIDIOC_STREAMON) {
    fixture::check(
        std::all_of(k.queued.begin(), k.queued.begin() + k.count, [](bool v) { return v; }),
        "stream began before queueing");
    k.streaming = true;
  } else if (request == VIDIOC_STREAMOFF) {
    ++k.streamoffs;
    k.streaming = false;
    k.queued.fill(false);
  } else {
    throw std::runtime_error("unexpected driver ioctl");
  }
  return 0;
}
int fixture_fcntl(int fd, int command) {
  auto& k = *fixture::kernel;
  if (!k.owner_call()) return -1;
  fixture::check(fd >= fixture::Kernel::export_base && command == F_GETFD,
                 "unexpected descriptor operation");
  return k.descriptor_flags;
}
void* fixture_mmap(void* start, std::size_t length, int protection, int flags, int fd,
                   off_t offset) {
  auto& k = *fixture::kernel;
  if (!k.owner_call()) return MAP_FAILED;
  const auto index = unsigned(offset / fixture::Kernel::offset_step);
  fixture::check(!start && length == k.query_bytes && protection == (PROT_READ | PROT_WRITE) &&
                     flags == MAP_SHARED && fd == fixture::Kernel::device_fd &&
                     offset % fixture::Kernel::offset_step == 0 && index < k.count,
                 "invalid capture mapping");
  if (int(index) == k.mmap_failure_index) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  k.mapped.at(index) = true;
  return k.storage.at(index).data();
}
int fixture_munmap(void* address, std::size_t bytes) {
  auto& k = *fixture::kernel;
  for (unsigned i = 0; i < k.count; ++i)
    if (address == k.storage.at(i).data()) {
      fixture::check(k.mapped.at(i) && bytes == k.query_bytes, "invalid or duplicate unmap");
      k.mapped.at(i) = false;
      ++k.unmaps;
      return 0;
    }
  throw std::runtime_error("unmap of unknown capture backing");
}
int fixture_close(int fd) {
  auto& k = *fixture::kernel;
  ++k.closes;
  if (fd == fixture::Kernel::device_fd) {
    fixture::check(k.opened, "double device close");
    k.opened = false;
  } else {
    const auto index = unsigned(fd - fixture::Kernel::export_base);
    fixture::check(index < k.count && k.exported.at(index), "unknown or duplicate export close");
    k.exported.at(index) = false;
  }
  return 0;
}
int fixture_clock_gettime(clockid_t clock, struct timespec* time) {
  auto& k = *fixture::kernel;
  if (!k.owner_call()) return -1;
  fixture::check(clock == CLOCK_MONOTONIC, "capture used another clock");
  time->tv_sec = static_cast<decltype(time->tv_sec)>(k.now / 1'000'000'000ULL);
  time->tv_nsec = static_cast<decltype(time->tv_nsec)>(k.now % 1'000'000'000ULL);
  return 0;
}

// Include all headers first. These names replace only calls in the original
// translation unit, leaving real libc declarations and Linux UAPI untouched.
#define lstat fixture_lstat
#define open fixture_open
#define fstat fixture_fstat
#define ioctl fixture_ioctl
#define fcntl fixture_fcntl
#define mmap fixture_mmap
#define munmap fixture_munmap
#define close fixture_close
#define clock_gettime fixture_clock_gettime
#define make_v4l2_capture_device fixture_make_v4l2_capture_device
#include "../src/camera_v4l2.cpp"
#undef make_v4l2_capture_device
#undef clock_gettime
#undef close
#undef munmap
#undef mmap
#undef fcntl
#undef ioctl
#undef fstat
#undef open
#undef lstat

namespace fixture {
using namespace headunit::camera;
Configuration configuration(CaptureDelivery delivery = CaptureDelivery::cpu_copy,
                            CapturePixelFormat pixel = CapturePixelFormat::yuyv) {
  Configuration value;
  value.enabled = true;
  value.source = SourceKind::v4l2;
  value.width = value.height = 64;
  value.frame_rate = 30;
  value.maximum_age_ms = 150;
  value.v4l2 = V4l2Configuration{"/dev/video7", pixel, delivery};
  return value;
}
auto device(const Configuration& config = configuration()) {
  return fixture_make_v4l2_capture_device(config);
}
void factory_and_paths() {
  Scope f;
  auto d = device();
  check(f.state.calls == 0, "factory opened a device");
  for (const auto* path : {"/dev/video", "/dev/video07", "/dev/video-1", "/dev/video7/../video8",
                           "/dev/video4294967296"}) {
    auto c = configuration();
    c.v4l2->device = path;
    rejects([&] { (void)device(c); });
  }
  check(f.state.calls == 0, "invalid configuration caused device calls");
}
void cpu_rows_and_leases() {
  Scope f;
  auto& k = f.state;
  auto d = device();
  const auto format = d->start();
  check(format.buffer_count == 4 && format.buffer_storage_bytes == k.query_bytes &&
            format.pixels.stride == 144 &&
            format.pixels.matrix == headunit::theme::video::YuvMatrix::bt709 &&
            !format.pixels.full_range && format.interval_numerator == 1001 &&
            format.interval_denominator == 30000,
        "lost negotiated storage or actual cadence");
  for (unsigned row = 0; row < 64; ++row)
    for (unsigned col = 0; col < 64; col += 2) {
      auto* p = k.storage[0].data() + row * 144 + col * 2;
      p[0] = 16;
      p[1] = 128;
      p[2] = 235;
      p[3] = 128;
    }
  k.dequeues.push_back(k.frame());
  auto sample = d->take();
  check(sample && !sample->native && sample->packed.size() == 144 * 64 &&
            sample->capture_monotonic_ns == 9'000'000'000ULL,
        "CPU sample padding or original timestamp wrong");
  const auto image = copy_capture_image(format.pixels, sample->packed);
  check(image.pixels.front() == 0xff000000U && image.pixels[1] == 0xffffffffU &&
            image.pixels.back() == 0xffffffffU,
        "padded capture rows decoded incorrectly");
  auto held = sample->lifetime;
  sample.reset();
  d->reclaim();
  check(k.qbufs == 4, "retained capture was overwritten");
  const auto calls = k.calls.load();
  std::thread release([held = std::move(held)]() mutable { held.reset(); });
  release.join();
  check(k.calls == calls && k.qbufs == 4 && k.violations == 0,
        "release invoked capture calls off worker");
  d->reclaim();
  d->reclaim();
  check(k.qbufs == 5, "return mailbox lost or duplicated lease");
  d->stop();
  check(k.released() && k.streamoffs == 1 && k.unmaps == 4, "CPU stop leaked backing");
  rejects([&] { (void)d->start(); });
}
void cpu_stop_retains_backing() {
  Scope f;
  auto& k = f.state;
  auto d = device();
  (void)d->start();
  k.dequeues.push_back(k.frame());
  auto sample = d->take();
  auto held = sample->lifetime;
  const auto* bytes = sample->packed.data();
  sample.reset();
  d->stop();
  d.reset();
  check(!k.opened && !k.streaming && k.mapped[0] && k.unmaps == 3 && bytes == k.storage[0].data(),
        "stop invalidated held capture");
  const auto calls = k.calls.load();
  std::thread release([held = std::move(held)]() mutable { held.reset(); });
  release.join();
  check(k.calls == calls && k.released() && k.unmaps == 4,
        "final CPU backing cleanup invoked driver or leaked");
}
void dma_stop_retains_descriptor() {
  Scope f;
  auto& k = f.state;
  auto d = device(configuration(CaptureDelivery::dma_buf));
  const auto format = d->start();
  k.dequeues.push_back(k.frame());
  auto sample = d->take();
  auto native = sample->native;
  check(sample->packed.empty() && native && native->metadata.sender_ntp == 0 &&
            native->metadata.timestamp_ns == 9'000'000'000ULL && native->metadata.width == 64 &&
            native->metadata.height == 64 && native->plane_count == 1 && native->modifier == 0 &&
            native->fourcc == V4L2_PIX_FMT_YUYV && native->planes[0].fd == Kernel::export_base &&
            native->planes[0].offset == 0 && native->planes[0].stride == format.pixels.stride &&
            native->crop_x == 0 && native->crop_y == 0 && !native->chroma_x_cosited &&
            !native->chroma_y_cosited && native->matrix == headunit::theme::video::YuvMatrix::bt709,
        "DMA capture lost native original metadata");
  sample.reset();
  d->reclaim();
  check(k.qbufs == 4, "native lease did not retain driver slot");
  d->stop();
  d.reset();
  check(k.exported[0] && !k.exported[1] && k.unmaps == 0, "stop closed borrowed DMA descriptor");
  const auto calls = k.calls.load();
  std::thread release([native = std::move(native)]() mutable { native.reset(); });
  release.join();
  check(k.calls == calls && k.released() && k.violations == 0,
        "final DMA cleanup leaked or invoked driver");
}
void color_metadata() {
  for (const auto space : {V4L2_COLORSPACE_SMPTE170M, V4L2_COLORSPACE_REC709, V4L2_COLORSPACE_SRGB,
                           V4L2_COLORSPACE_JPEG}) {
    Scope f;
    f.state.adjust = [=](auto& p) {
      p.colorspace = space;
    };
    auto d = device();
    const auto format = d->start();
    check(format.pixels.matrix == (space == V4L2_COLORSPACE_REC709
                                       ? headunit::theme::video::YuvMatrix::bt709
                                       : headunit::theme::video::YuvMatrix::bt601) &&
              format.pixels.full_range == (space == V4L2_COLORSPACE_JPEG),
          "normative color default resolution wrong");
    d->stop();
    check(f.state.released(), "color case leaked");
  }
  for (const auto pixel : {CapturePixelFormat::rgb24, CapturePixelFormat::bgr24}) {
    Scope f;
    f.state.query_bytes = 16384;
    for (auto& bytes : f.state.storage)
      bytes.resize(f.state.query_bytes);
    auto d = device(configuration(CaptureDelivery::cpu_copy, pixel));
    const auto format = d->start();
    check(format.pixels.full_range && format.pixels.pixel_format == pixel,
          "explicit RGB negotiation wrong");
    d->stop();
    check(f.state.released(), "RGB negotiation leaked");
  }
}
void rejects_formats() {
  const std::vector<std::function<void(v4l2_pix_format&)>> mutations{
      [](auto& p) { ++p.width; },
      [](auto& p) { p.pixelformat = V4L2_PIX_FMT_NV12; },
      [](auto& p) { p.field = V4L2_FIELD_INTERLACED; },
      [](auto& p) { p.priv = 0; },
      [](auto& p) { p.flags = 1; },
      [](auto& p) { p.colorspace = V4L2_COLORSPACE_DEFAULT; },
      [](auto& p) { p.colorspace = V4L2_COLORSPACE_BT2020; },
      [](auto& p) { p.xfer_func = V4L2_XFER_FUNC_SMPTE2084; },
      [](auto& p) { p.ycbcr_enc = V4L2_YCBCR_ENC_BT2020; },
      [](auto& p) { p.quantization = 99; },
      [](auto& p) { p.bytesperline = 1; },
      [](auto& p) { p.sizeimage = 1; },
      [](auto& p) {
        p.sizeimage = maximum_packed_capture_storage + 1;
      }};
  for (const auto& mutation : mutations) {
    Scope f;
    f.state.adjust = mutation;
    auto d = device();
    rejects([&] { (void)d->start(); });
    check(f.state.released(), "format failure leaked");
  }
  Scope f;
  f.state.confirm_adjust = [](auto& p) {
    ++p.bytesperline;
  };
  auto d = device();
  rejects([&] { (void)d->start(); });
  check(f.state.released(), "changed format leaked");
}
void rejects_nodes_capabilities_and_bounds() {
  for (unsigned n = 0; n < 10; ++n) {
    Scope f;
    auto& k = f.state;
    switch (n) {
      case 0:
        k.character = false;
        break;
      case 1:
        k.changed_node = true;
        break;
      case 2:
        k.caps |= V4L2_CAP_VIDEO_M2M;
        break;
      case 3:
        k.caps &= ~V4L2_CAP_VIDEO_CAPTURE;
        break;
      case 4:
        k.caps &= ~V4L2_CAP_STREAMING;
        break;
      case 5:
        k.caps &= ~V4L2_CAP_EXT_PIX_FORMAT;
        break;
      case 6:
        k.count = 1;
        break;
      case 7:
        k.count = 5;
        break;
      case 8:
        k.query_bytes = 1024;
        break;
      case 9:
        k.query_bytes = maximum_packed_capture_storage + 1;
        break;
    }
    auto d = device();
    rejects([&] { (void)d->start(); });
    check(k.released(), "node/capacity rejection leaked");
  }
}
void partial_startup_cleanup() {
  for (unsigned n = 0; n < 6; ++n) {
    Scope f;
    auto& k = f.state;
    const bool dma = n >= 2 && n <= 4;
    if (n == 0) k.mmap_failure_index = 2;
    if (n == 1) k.fail_request = VIDIOC_G_PARM;
    if (n == 2) k.export_failure_index = 2;
    if (n == 3) k.descriptor_flags = -1;
    if (n == 4) k.descriptor_flags = 0;
    if (n == 5) k.fail_request = VIDIOC_STREAMON;
    auto d = device(configuration(dma ? CaptureDelivery::dma_buf : CaptureDelivery::cpu_copy));
    rejects([&] { (void)d->start(); });
    check(k.released(), "partial startup leaked storage");
    if (n == 5) check(k.streamoffs == 1, "failed STREAMON was not retired");
    check(dma ? k.unmaps == 0 : k.exported == std::array<bool, 4>{},
          "automatic delivery fallback occurred");
  }
}
void corrupted_and_serial_wrap() {
  Scope f;
  auto& k = f.state;
  auto d = device();
  (void)d->start();
  auto bad = k.frame();
  bad.flags = V4L2_BUF_FLAG_ERROR;
  bad.timestamp = {};
  bad.bytesused = 0;
  k.dequeues.push_back(bad);
  const auto dropped = d->take();
  check(dropped && dropped->corrupted && !dropped->lifetime && !dropped->native &&
            dropped->packed.empty() && k.qbufs == 5,
        "corrupted buffer was leased or treated as terminal");
  k.dequeues.push_back(k.frame(0, 0xffffffffU));
  auto first = d->take();
  first.reset();
  d->reclaim();
  k.dequeues.push_back(k.frame(0, 0, 9'001'000'000ULL));
  auto wrap = d->take();
  check(wrap && wrap->driver_sequence == 0, "valid driver serial wrap rejected");
  wrap.reset();
  d->reclaim();
  d->stop();
  check(k.released(), "corrupted/wrapped flow leaked");
}
void rejects_timestamps_and_storage() {
  const std::vector<std::function<void(v4l2_buffer&)>> mutations{
      [](auto& p) { p.flags = V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN; },
      [](auto& p) { p.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY; },
      [](auto& p) { p.flags |= V4L2_BUF_FLAG_TSTAMP_SRC_MASK; },
      [](auto& p) { p.timestamp = {}; },
      [](auto& p) { p.timestamp.tv_sec = -1; },
      [](auto& p) { p.timestamp.tv_usec = 1000000; },
      [](auto& p) { p.timestamp.tv_sec = 11; },
      [](auto& p) { p.bytesused = 1; },
      [](auto& p) { ++p.length; },
      [](auto& p) { p.bytesused = p.length + 1; },
      [](auto& p) { p.index = 4; },
      [](auto& p) { p.flags |= V4L2_BUF_FLAG_QUEUED; },
      [](auto& p) {
        p.field = V4L2_FIELD_TOP;
      }};
  for (const auto& mutation : mutations) {
    Scope f;
    auto& k = f.state;
    auto d = device();
    (void)d->start();
    auto frame = k.frame();
    mutation(frame);
    k.dequeues.push_back(frame);
    rejects([&] { (void)d->take(); });
    check(k.streamoffs == 0, "faulting dequeue ran teardown before reporting failure");
    const auto calls = k.calls.load();
    rejects([&] { (void)d->take(); });
    rejects([&] { d->reclaim(); });
    check(k.calls == calls, "terminal adapter reused the capture device");
    d->stop();
    check(k.released(), "bad timestamp/storage did not retire capture");
  }
  for (unsigned n = 0; n < 4; ++n) {
    Scope f;
    auto& k = f.state;
    auto d = device();
    (void)d->start();
    k.dequeues.push_back(k.frame());
    auto first = d->take();
    first.reset();
    d->reclaim();
    auto frame = k.frame(0, 2, 9'001'000'000ULL);
    if (n == 0) frame.timestamp = k.frame().timestamp;
    if (n == 1) frame.sequence = 1;
    if (n == 2) frame.sequence = 0;
    if (n == 3) frame.flags |= V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
    k.dequeues.push_back(frame);
    rejects([&] { (void)d->take(); });
    d->stop();
    check(k.released(), "changed timestamp/reference/serial did not retire");
  }
}
void reclaim_fault_before_cleanup() {
  Scope f;
  auto& k = f.state;
  auto d = device();
  (void)d->start();
  k.dequeues.push_back(k.frame());
  auto held = d->take();
  held.reset();
  k.fail_request = VIDIOC_QBUF;
  rejects([&] { d->reclaim(); });
  check(k.streamoffs == 0 && k.opened, "reclaim blocked failure reporting with driver cleanup");
  const auto calls = k.calls.load();
  rejects([&] { (void)d->take(); });
  rejects([&] { d->reclaim(); });
  check(k.calls == calls, "failed reclaim admitted later device operations");
  d->stop();
  check(k.released() && k.streamoffs == 1, "failed reclaim did not clean up once retired");
}
void nonblocking_retries_and_terminal() {
  for (unsigned n = 0; n < 5; ++n) {
    Scope f;
    auto& k = f.state;
    auto d = device();
    (void)d->start();
    if (n == 0) {
      check(!d->take() && k.opened, "EAGAIN was terminal");
      k.interrupt_request = VIDIOC_DQBUF;
      k.interruptions = 2;
      k.dequeues.push_back(k.frame());
      auto sample = d->take();
      check(bool(sample), "bounded interrupted dequeue lost available capture");
      sample.reset();
      d->reclaim();
      d->stop();
    } else {
      if (n == 1) k.dequeue_errno = EPIPE;
      if (n == 2) {
        auto last = k.frame();
        last.flags |= V4L2_BUF_FLAG_LAST;
        k.dequeues.push_back(last);
      }
      if (n == 3) {
        k.interrupt_request = VIDIOC_DQBUF;
        k.interruptions = 20;
      }
      if (n == 4) {
        k.dequeues.push_back(k.frame());
        auto held = d->take();
        k.dequeues.push_back(k.frame(0, 2, 9'001'000'000ULL));
        rejects([&] { (void)d->take(); });
        check(k.streamoffs == 0, "invalid re-dequeue delayed fault with driver cleanup");
        d->stop();
        check(!k.opened && k.mapped[0], "invalid re-dequeue released retained backing");
        held.reset();
        d.reset();
        check(k.released(), "invalid re-dequeue leaked held backing");
        continue;
      }
      rejects([&] { (void)d->take(); });
      if (n == 3) check(k.interruptions == 17, "EINTR retries were unbounded");
      check(k.streamoffs == 0, "terminal dequeue blocked its fault report with cleanup");
      d->stop();
    }
    check(k.released() && k.violations == 0, "nonblocking or terminal cleanup failed");
  }
}
}  // namespace fixture

int main() {
  try {
    unsigned passed = 0;
    const std::array<std::pair<const char*, void (*)()>, 12> tests{
        {{"unopened factory and canonical paths", fixture::factory_and_paths},
         {"CPU padded rows and deferred worker returns", fixture::cpu_rows_and_leases},
         {"CPU stop retains immutable backing", fixture::cpu_stop_retains_backing},
         {"DMA original metadata and borrowed descriptor lifetime",
          fixture::dma_stop_retains_descriptor},
         {"negotiated SDR defaults and RGB delivery", fixture::color_metadata},
         {"exact format and unsupported metadata rejection", fixture::rejects_formats},
         {"configured node, capability and capacity rejection",
          fixture::rejects_nodes_capabilities_and_bounds},
         {"partial startup cleanup and no automatic fallback", fixture::partial_startup_cleanup},
         {"recoverable corruption and serial wrap", fixture::corrupted_and_serial_wrap},
         {"timestamp, reference, ownership and storage rejection",
          fixture::rejects_timestamps_and_storage},
         {"reclaim fault reporting before driver cleanup", fixture::reclaim_fault_before_cleanup},
         {"nonblocking dequeue, bounded retries and terminal retirement",
          fixture::nonblocking_retries_and_terminal}}};
    for (const auto& [name, test] : tests) {
      test();
      ++passed;
      std::cout << "PASS " << name << '\n';
    }
    std::cout << passed
              << " V4L2 syscall-model groups passed; no Linux kernel, camera, DMA import or GPU "
                 "qualification.\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
