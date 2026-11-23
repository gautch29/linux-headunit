// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/mfi_coprocessor.hpp"

#include <algorithm>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
struct FakeBus final : headunit::MfiRegisterBus {
  std::map<std::uint8_t, std::vector<std::uint8_t>> registers;
  std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> writes;
  int pending_status_reads = 0;
  std::vector<std::uint8_t> reads;
  std::function<void(std::uint8_t, bool)> before_operation;

  void read(std::uint8_t reg, std::span<std::uint8_t> out) override {
    reads.push_back(reg);
    if (before_operation) before_operation(reg, false);
    if (reg == 0x10 && pending_status_reads-- > 0) {
      std::fill(out.begin(), out.end(), 0);
      return;
    }
    const auto& bytes = registers.at(reg);
    if (bytes.size() < out.size()) throw std::runtime_error("short fake register");
    std::copy_n(bytes.begin(), out.size(), out.begin());
  }

  void write(std::uint8_t reg, std::span<const std::uint8_t> value) override {
    writes.emplace_back(reg, std::vector<std::uint8_t>(value.begin(), value.end()));
    if (before_operation) before_operation(reg, true);
  }
};
}  // namespace

int main() {
  FakeBus bus;
  bus.registers[0x00] = {7};
  bus.registers[0x01] = {2};
  bus.registers[0x02] = {3};
  bus.registers[0x03] = {0};
  bus.registers[0x05] = {0};
  bus.registers[0x30] = {0x02, 0x61};  // 609 bytes, one certificate transfer.
  bus.registers[0x31] = std::vector<std::uint8_t>(609, 0x5b);
  bus.registers[0x10] = {0x10};
  bus.registers[0x11] = {0, 64};
  bus.registers[0x12] = std::vector<std::uint8_t>(64, 0x5a);

  headunit::MfiCoprocessor chip(bus);
  const auto info = chip.read_info();
  assert(info.device_version == 7 && info.protocol_major == 3);

  const auto cert = chip.read_certificate();
  assert(cert.size() == 609);
  assert(cert[0] == 0x5b && cert[128] == 0x5b && cert[608] == 0x5b);

  const std::array<std::uint8_t, 32> digest{};
  bus.pending_status_reads = 1;
  const auto signature = chip.sign_digest(digest, std::chrono::milliseconds(100));
  assert(signature[0] == 0x5a && signature[63] == 0x5a);
  assert(bus.writes.size() == 3);
  assert(bus.writes[0].first == 0x20 && bus.writes[0].second == std::vector<std::uint8_t>({0, 32}));
  assert(bus.writes[1].first == 0x21 && bus.writes[1].second.size() == 32);
  assert(bus.writes[2].first == 0x10 && bus.writes[2].second == std::vector<std::uint8_t>{1});

  bus.registers[0x30] = {0x02, 0x81};  // 641 exceeds the observed chip limit.
  bool rejected = false;
  try {
    (void)chip.read_certificate();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  bus.registers[0x30] = {0x02, 0x61};
  bus.registers[0x11] = {0, 63};
  rejected = false;
  try {
    (void)chip.sign_digest(digest);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  bus.registers[0x11] = {0, 64};
  // Actual elapsed bus delay at each phase must not return a late signature or
  // proceed to the next register. No fake clock marks an unfinished call done.
  const auto timed_out = [&](std::chrono::milliseconds timeout) {
    bool expired = false;
    try {
      (void)chip.sign_digest(digest, timeout);
    } catch (const std::runtime_error& error) {
      expired = std::string(error.what()) == "MFi coprocessor signature timed out";
    }
    assert(expired);
  };
  for (const auto& [delayed_register, is_write] : {std::pair<std::uint8_t, bool>{0x02, false},
                                                   {0x20, true},
                                                   {0x21, true},
                                                   {0x10, true},
                                                   {0x10, false},
                                                   {0x11, false},
                                                   {0x12, false},
                                                   {0x05, false}}) {
    bus.reads.clear();
    bus.writes.clear();
    bus.before_operation = [=](std::uint8_t reg, bool write) {
      if (reg == delayed_register && write == is_write) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
    };
    timed_out(std::chrono::milliseconds(15));
    if (is_write)
      assert(bus.writes.back().first == delayed_register);
    else
      assert(bus.reads.back() == delayed_register);
  }
  // Busy/NACK retry uses the same deadline, then a fresh attempt can recover.
  bus.before_operation = [](std::uint8_t reg, bool write) {
    if (reg == 0x10 && !write) {
      throw std::system_error(std::make_error_code(std::errc::io_error));
    }
  };
  timed_out(std::chrono::milliseconds(25));
  bus.before_operation = {};
  assert(chip.sign_digest(digest)[0] == 0x5a);

  for (const auto timeout : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1),
                             std::chrono::milliseconds(2001)}) {
    const auto reads = bus.reads.size(), writes = bus.writes.size();
    bool invalid = false;
    try {
      (void)chip.sign_digest(digest, timeout);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    assert(invalid && reads == bus.reads.size() && writes == bus.writes.size());
  }
}
