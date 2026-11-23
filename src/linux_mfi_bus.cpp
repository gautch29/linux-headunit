// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/linux_mfi_bus.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace headunit {
namespace {
constexpr int kBusyRetries = 100;
constexpr auto kRetryDelay = std::chrono::milliseconds(5);

void transfer_with_busy_retry(int fd, i2c_rdwr_ioctl_data& transfer, const char* operation) {
  for (int attempt = 0; attempt < kBusyRetries; ++attempt) {
    if (::ioctl(fd, I2C_RDWR, &transfer) == static_cast<int>(transfer.nmsgs)) return;
    const int error = errno ? errno : EIO;
    if (error != EIO && error != EREMOTEIO && error != ENXIO) {
      throw std::system_error(error, std::generic_category(), operation);
    }
    if (attempt + 1 == kBusyRetries) {
      throw std::system_error(error, std::generic_category(), operation);
    }
    std::this_thread::sleep_for(kRetryDelay);
  }
}
}  // namespace

LinuxMfiBus::LinuxMfiBus(const std::string& device, std::uint8_t address) : address_(address) {
  if (address > 0x7f) throw std::invalid_argument("I2C address must be 7-bit");
  fd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "open I2C bus");
}

LinuxMfiBus::~LinuxMfiBus() {
  if (fd_ >= 0) ::close(fd_);
}

void LinuxMfiBus::read(std::uint8_t reg, std::span<std::uint8_t> out) {
  if (out.empty() || out.size() > 640) {
    throw std::invalid_argument("I2C register read must be 1..640 bytes");
  }
  struct i2c_msg select = {.addr = address_, .flags = 0, .len = 1, .buf = &reg};
  struct i2c_rdwr_ioctl_data select_transfer = {.msgs = &select, .nmsgs = 1};
  transfer_with_busy_retry(fd_, select_transfer, "select MFi register");
  struct i2c_msg data = {.addr = address_,
                         .flags = I2C_M_RD,
                         .len = static_cast<__u16>(out.size()),
                         .buf = out.data()};
  struct i2c_rdwr_ioctl_data data_transfer = {.msgs = &data, .nmsgs = 1};
  transfer_with_busy_retry(fd_, data_transfer, "read MFi register");
}

void LinuxMfiBus::write(std::uint8_t reg, std::span<const std::uint8_t> value) {
  if (value.empty() || value.size() > 128) {
    throw std::invalid_argument("I2C register write must be 1..128 bytes");
  }
  std::vector<std::uint8_t> bytes;
  bytes.reserve(value.size() + 1);
  bytes.push_back(reg);
  bytes.insert(bytes.end(), value.begin(), value.end());
  struct i2c_msg message = {
      .addr = address_, .flags = 0, .len = static_cast<__u16>(bytes.size()), .buf = bytes.data()};
  struct i2c_rdwr_ioctl_data transfer = {.msgs = &message, .nmsgs = 1};
  transfer_with_busy_retry(fd_, transfer, "write MFi register");
}

}  // namespace headunit
