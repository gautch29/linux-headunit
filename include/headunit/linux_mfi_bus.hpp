// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/mfi_coprocessor.hpp"

#include <cstdint>
#include <string>

namespace headunit {

class LinuxMfiBus final : public MfiRegisterBus {
 public:
  LinuxMfiBus(const std::string& device, std::uint8_t address);
  ~LinuxMfiBus() override;
  LinuxMfiBus(const LinuxMfiBus&) = delete;
  LinuxMfiBus& operator=(const LinuxMfiBus&) = delete;

  void read(std::uint8_t reg, std::span<std::uint8_t> out) override;
  void write(std::uint8_t reg, std::span<const std::uint8_t> value) override;

 private:
  int fd_ = -1;
  std::uint8_t address_ = 0;
};

}  // namespace headunit
