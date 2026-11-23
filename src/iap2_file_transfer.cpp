// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_file_transfer.hpp"

#include <utility>

namespace headunit {
namespace {
constexpr std::size_t max_file = 1024 * 1024;
constexpr std::size_t max_pending = 4;
}  // namespace

Iap2TransferResult Iap2FileTransfer::receive(std::span<const std::uint8_t> packet) {
  if (packet.size() < 2) return {};
  const auto id = packet[0];
  const auto action = packet[1];
  const auto payload = packet.subspan(2);
  if (payload.size() > max_file) {
    pending_.erase(id);
    return {};
  }
  if (action == 4) {
    if (!payload.empty() || (!pending_.contains(id) && pending_.size() >= max_pending)) return {};
    pending_[id].clear();
    return {{id, 1}, {}};
  }
  if (action == 2) {
    pending_.erase(id);
    return {};
  }
  if (action == 0x80) {
    if (!pending_.contains(id) && pending_.size() >= max_pending) return {};
    pending_[id] = {payload.begin(), payload.end()};
    return {};
  }
  if (action == 0xc0) {
    pending_.erase(id);
    if (payload.empty()) return {};
    return {{id, 5}, std::pair{id, std::vector<std::uint8_t>(payload.begin(), payload.end())}};
  }
  if (action == 0 || action == 0x40) {
    const auto found = pending_.find(id);
    if (found == pending_.end() || payload.size() > max_file - found->second.size()) {
      pending_.erase(id);
      return {};
    }
    found->second.insert(found->second.end(), payload.begin(), payload.end());
    if (action == 0) return {};
    auto result = std::move(found->second);
    pending_.erase(found);
    if (result.empty()) return {};
    return {{id, 5}, std::pair{id, std::move(result)}};
  }
  return {};
}

}  // namespace headunit
