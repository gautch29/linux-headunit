// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace headunit {

struct Iap2TransferResult {
  std::vector<std::uint8_t> reply;
  std::optional<std::pair<std::uint8_t, std::vector<std::uint8_t>>> completed;
};

// Session 12 carries artwork files. Each transfer and the number of concurrent
// transfers are bounded before bytes are retained or passed to image decoders.
class Iap2FileTransfer {
 public:
  Iap2TransferResult receive(std::span<const std::uint8_t> packet);
  void clear() { pending_.clear(); }

 private:
  std::map<std::uint8_t, std::vector<std::uint8_t>> pending_;
};

}  // namespace headunit
