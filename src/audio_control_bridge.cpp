// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_control_bridge.hpp>

#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>

namespace headunit::audio_control {
namespace {
bool route_valid(std::string_view route) {
  return route.size() == 32 && std::all_of(route.begin(), route.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
std::string route_token() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), bytes.size()) != 1)
    throw std::runtime_error("audio route identity unavailable");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(32);
  for (const auto value : bytes) {
    result += hex[value >> 4];
    result += hex[value & 15];
  }
  return result;
}
std::string diagnostic(std::string_view reason) {
  std::string result;
  result.reserve(std::min<std::size_t>(reason.size(), 128));
  for (const auto c : reason.substr(0, 128))
    result += c >= 32 && c <= 126 ? c : '?';
  return result;
}
}  // namespace
Bridge::Bridge(std::string request_path, std::string state_path, unsigned owner_uid,
               RouteFactory route_factory)
    : owner_(std::this_thread::get_id()),
      request_path_(std::move(request_path)),
      state_path_(std::move(state_path)),
      owner_uid_(owner_uid),
      route_factory_(std::move(route_factory)) {
  const std::filesystem::path request(request_path_), state(state_path_);
  if (!request.is_absolute() || !state.is_absolute() ||
      request.lexically_normal() == state.lexically_normal())
    throw std::invalid_argument("audio control/state require distinct absolute paths");
  if (!route_factory_) route_factory_ = route_token;
}
void Bridge::check_owner() const {
  if (owner_ != std::this_thread::get_id())
    throw std::logic_error("audio file bridge used by another owner");
}
void Bridge::poll(std::span<const std::shared_ptr<Mailbox>> routes, std::uint64_t monotonic_ns) {
  check_owner();
  if (!monotonic_ns || monotonic_ns < last_poll_ns_)
    throw std::invalid_argument("audio bridge requires ordered real monotonic time");
  last_poll_ns_ = monotonic_ns;
  State next;
  std::shared_ptr<Mailbox> selected;
  unsigned available = 0;
  for (const auto& route : routes) {
    if (route && route->snapshot().available) {
      ++available;
      selected = route;
    }
  }
  try {
    if (available > 1) throw std::runtime_error("More than one active audio route");
    if (selected) {
      if (selected != active_) {
        const auto token = route_factory_();
        if (!route_valid(token) || token == last_route_)
          throw std::runtime_error("Audio route identity did not change");
        next.route = token;
        last_route_ = token;
      } else
        next.route = state_.route;
      next.control = selected->snapshot();
      if (next.control.available) {
        try {
          if (const auto request = read_private_request(request_path_, owner_uid_)) {
            if (request->route != next.route)
              throw std::runtime_error("Audio route changed; refresh controls");
            if (request->command.sequence == next.control.requested.sequence &&
                request->command != next.control.requested)
              throw std::runtime_error("Audio request sequence was reused");
            // Stale requests are not re-applied. Mailbox publication is atomic;
            // only the audio owner can later confirm the actual mixer setting.
            (void)selected->request(request->command);
          }
        } catch (const std::exception& error) {
          next.error = diagnostic(error.what());
        }
        next.control = selected->snapshot();
      }
      if (!next.control.available) next.route.clear();
      active_ = std::move(selected);
    } else
      active_.reset();
  } catch (const std::exception& error) {
    active_.reset();
    next = {};
    next.error = diagnostic(error.what());
  }
  state_ = std::move(next);
  try {
    // The worker may publish a newer observation while this poll copies it.
    // Date the file after the copy; never clamp or refresh its acquisition date.
    write_private_state(state_path_, owner_uid_, state_, monotonic_time_ns());
    publication_error_.clear();
  } catch (const std::exception& error) {
    publication_error_ = diagnostic(error.what());
  }
}
const State& Bridge::state() const {
  check_owner();
  return state_;
}
const std::string& Bridge::publication_error() const {
  check_owner();
  return publication_error_;
}
}  // namespace headunit::audio_control
