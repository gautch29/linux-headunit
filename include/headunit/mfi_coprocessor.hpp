// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/mfi_authenticator.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

// One register operation. The Linux transport follows the chip's observed
// register-select transfer, then its separate data transfer. Only one process
// should own the coprocessor during authentication.
class MfiRegisterBus {
 public:
  virtual ~MfiRegisterBus() = default;
  virtual void read(std::uint8_t reg, std::span<std::uint8_t> out) = 0;
  virtual void write(std::uint8_t reg, std::span<const std::uint8_t> value) = 0;
};

struct MfiDeviceInfo {
  std::uint8_t device_version{};
  std::uint8_t authentication_revision{};
  std::uint8_t protocol_major{};
  std::uint8_t protocol_minor{};
};

class MfiCoprocessor final : public MfiAuthenticator {
 public:
  explicit MfiCoprocessor(MfiRegisterBus& bus) : bus_(bus) {}

  MfiDeviceInfo read_info();
  std::uint8_t protocol_major() override { return read_info().protocol_major; }
  std::vector<std::uint8_t> read_certificate() override;

  // Protocol-3 coprocessors sign a 32-byte digest. The caller is responsible
  // for deriving that digest from the phone's challenge for its protocol.
  // Positive timeout, at most two seconds, covers all completed register work.
  // Late success is rejected; the bus/kernel operation itself is not preempted.
  std::array<std::uint8_t, 64> sign_digest(
      std::span<const std::uint8_t, 32> digest,
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) override;

 private:
  std::uint8_t read_byte(std::uint8_t reg);
  void check_error();
  MfiRegisterBus& bus_;
};

}  // namespace headunit
