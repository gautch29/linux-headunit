// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/mfi_coprocessor.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace headunit {
namespace {
constexpr std::uint8_t kDeviceVersion = 0x00;
constexpr std::uint8_t kAuthRevision = 0x01;
constexpr std::uint8_t kProtocolMajor = 0x02;
constexpr std::uint8_t kProtocolMinor = 0x03;
constexpr std::uint8_t kError = 0x05;
constexpr std::uint8_t kControl = 0x10;
constexpr std::uint8_t kResponseLength = 0x11;
constexpr std::uint8_t kResponse = 0x12;
constexpr std::uint8_t kChallenge = 0x21;
constexpr std::uint8_t kChallengeLength = 0x20;
constexpr std::uint8_t kCertificateLength = 0x30;
constexpr std::uint8_t kCertificatePart1 = 0x31;
constexpr std::size_t kMaximumCertificateSize = 640;
}  // namespace

std::uint8_t MfiCoprocessor::read_byte(std::uint8_t reg) {
  std::array<std::uint8_t, 1> value{};
  bus_.read(reg, value);
  return value[0];
}

void MfiCoprocessor::check_error() {
  const auto error = read_byte(kError);
  if (error != 0) {
    throw std::runtime_error("MFi coprocessor reported error " + std::to_string(error));
  }
}

MfiDeviceInfo MfiCoprocessor::read_info() {
  MfiDeviceInfo info{};
  info.device_version = read_byte(kDeviceVersion);
  info.authentication_revision = read_byte(kAuthRevision);
  info.protocol_major = read_byte(kProtocolMajor);
  info.protocol_minor = read_byte(kProtocolMinor);
  check_error();
  return info;
}

std::vector<std::uint8_t> MfiCoprocessor::read_certificate() {
  if (read_byte(kProtocolMajor) != 3) {
    throw std::runtime_error("MFi certificate reader requires protocol 3");
  }
  std::array<std::uint8_t, 2> length_bytes{};
  bus_.read(kCertificateLength, length_bytes);
  const auto length = (static_cast<std::size_t>(length_bytes[0]) << 8) | length_bytes[1];
  if (length == 0 || length > kMaximumCertificateSize) {
    throw std::runtime_error("MFi certificate length is out of range");
  }
  std::vector<std::uint8_t> certificate(length);
  bus_.read(kCertificatePart1, certificate);
  check_error();
  return certificate;
}

std::array<std::uint8_t, 64> MfiCoprocessor::sign_digest(std::span<const std::uint8_t, 32> digest,
                                                         std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(2)) {
    throw std::invalid_argument("MFi signature deadline must be within two seconds");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto check_deadline = [&] {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error("MFi coprocessor signature timed out");
    }
  };
  // A register transport can itself block. Check both sides of every completed
  // operation so late device success cannot authorize more work or a signature.
  // This logical deadline does not preempt a blocked kernel I2C operation.
  const auto read = [&](std::uint8_t reg, std::span<std::uint8_t> out) {
    check_deadline();
    bus_.read(reg, out);
    check_deadline();
  };
  const auto write = [&](std::uint8_t reg, std::span<const std::uint8_t> value) {
    check_deadline();
    bus_.write(reg, value);
    check_deadline();
  };
  std::array<std::uint8_t, 1> byte{};
  read(kProtocolMajor, byte);
  if (byte[0] != 3) {
    throw std::runtime_error("MFi signature reader requires protocol 3");
  }
  constexpr std::array<std::uint8_t, 2> challenge_length{0, 32};
  write(kChallengeLength, challenge_length);
  write(kChallenge, digest);
  constexpr std::array<std::uint8_t, 1> start{1};
  write(kControl, start);

  while (true) {
    try {
      read(kControl, byte);
      const auto status = byte[0];
      if (status == 0x10) break;
      if (status != 0x00 && status != 0x01) {
        throw std::runtime_error("MFi coprocessor rejected challenge");
      }
    } catch (const std::system_error&) {
      // The chip may NACK reads while computing. Never renew the original date.
    }
    check_deadline();
    std::this_thread::sleep_until(
        std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(10)));
  }

  std::array<std::uint8_t, 2> length_bytes{};
  read(kResponseLength, length_bytes);
  const auto length = (static_cast<unsigned>(length_bytes[0]) << 8) | length_bytes[1];
  if (length != 64) {
    throw std::runtime_error("MFi coprocessor returned unexpected signature length");
  }
  std::array<std::uint8_t, 64> signature{};
  read(kResponse, signature);
  read(kError, byte);
  if (byte[0] != 0) {
    throw std::runtime_error("MFi coprocessor reported error " + std::to_string(byte[0]));
  }
  return signature;
}

}  // namespace headunit
