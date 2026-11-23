// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/route_guidance.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace headunit {
namespace {
constexpr std::size_t text_limit = 256, maneuver_capacity = 128;
std::vector<std::uint8_t> word(std::uint16_t n) {
  return {std::uint8_t(n >> 8), std::uint8_t(n)};
}
std::uint64_t number(const Iap2Parameter& p, std::size_t size) {
  if (p.value.size() != size) throw std::invalid_argument("invalid navigation field width");
  std::uint64_t result = 0;
  for (auto b : p.value)
    result = (result << 8) | b;
  return result;
}
std::string string(const Iap2Parameter& p) {
  auto bytes = std::span(p.value);
  if (!bytes.empty() && bytes.back() == 0) bytes = bytes.first(bytes.size() - 1);
  if (bytes.size() > text_limit) throw std::invalid_argument("navigation text too long");
  // Validate UTF-8 before handing text to Cairo. Embedded NULs are not accepted.
  for (std::size_t i = 0; i < bytes.size();) {
    auto b = bytes[i++];
    if (b == 0) throw std::invalid_argument("NUL in navigation text");
    if (b < 128) continue;
    unsigned length = b >= 0xc2 && b <= 0xdf   ? 1
                      : b >= 0xe0 && b <= 0xef ? 2
                      : b >= 0xf0 && b <= 0xf4 ? 3
                                               : 0;
    if (!length || i + length > bytes.size())
      throw std::invalid_argument("invalid navigation UTF-8");
    std::uint32_t code = b & ((1u << (6 - length)) - 1);
    for (unsigned j = 0; j < length; ++j) {
      auto next = bytes[i++];
      if ((next & 0xc0) != 0x80) throw std::invalid_argument("invalid navigation UTF-8");
      code = (code << 6) | (next & 63);
    }
    if ((length == 2 && code < 0x800) || (length == 3 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff))
      throw std::invalid_argument("invalid navigation UTF-8");
  }
  std::string result(bytes.begin(), bytes.end());
  for (auto& c : result)
    if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
  return result;
}
const Iap2Parameter* field(const std::vector<Iap2Parameter>& p, unsigned id) {
  const auto it =
      std::find_if(p.begin(), p.end(), [&](const auto& f) { return f.identifier == id; });
  return it == p.end() ? nullptr : &*it;
}
}  // namespace
Iap2Parameter route_guidance_component(const RouteGuidanceConfig& config) {
  if (config.component_id < 3 || config.name.empty() || config.name.size() > 127 ||
      std::any_of(config.name.begin(), config.name.end(),
                  [](unsigned char c) { return c < 32 || c > 126; }))
    throw std::invalid_argument("invalid route guidance component");
  auto name = std::vector<std::uint8_t>(config.name.begin(), config.name.end());
  name.push_back(0);
  const std::array<Iap2Parameter, 9> p{{{0, word(config.component_id)},
                                        {1, name},
                                        {2, word(text_limit)},
                                        {3, word(text_limit)},
                                        {4, word(text_limit)},
                                        {5, word(text_limit)},
                                        {6, word(maneuver_capacity)},
                                        {7, word(0)},
                                        {8, word(0)}}};  // No lane UI advertised yet.
  return {30, encode_iap2_parameters(p)};
}
ControlMessage start_route_guidance(const RouteGuidanceConfig& config) {
  (void)route_guidance_component(config);
  const std::array<Iap2Parameter, 3> p{{{0, word(config.component_id)}, {1, {}}, {2, {}}}};
  return {0x5200, encode_iap2_parameters(p)};
}
RouteGuidance::RouteGuidance(RouteGuidanceConfig config) : config_(std::move(config)) {
  (void)route_guidance_component(config_);
}
void RouteGuidance::clear() {
  std::lock_guard lock(mutex_);
  state_ = {};
  supported_.reset();
  order_.clear();
  maneuvers_.clear();
}
bool RouteGuidance::receive(const ControlMessage& message, Clock::time_point now) {
  if (message.identifier != 0x5201 && message.identifier != 0x5202) return false;
  try {
    if (message.parameters.size() > 16384) return false;
    const auto params = decode_iap2_parameters(message.parameters);
    if (params.size() > 128) return false;
    std::set<unsigned> seen;
    for (const auto& p : params) {
      if (p.identifier <= 0x1a && !seen.insert(p.identifier).second &&
          !(message.identifier == 0x5202 && p.identifier == 10))
        return false;
    }
    const auto* component = field(params, 0);
    if (!component || number(*component, 2) != config_.component_id) return false;
    std::lock_guard lock(mutex_);
    if (message.identifier == 0x5202) {
      const auto* index = field(params, 1);
      if (!index) return false;
      const auto id = std::uint16_t(number(*index, 2));
      auto existing = maneuvers_.find(id);
      NavigationManeuver next =
          existing == maneuvers_.end() ? NavigationManeuver{} : existing->second;
      next.index = id;
      for (const auto& p : params)
        switch (p.identifier) {
          case 2:
            next.instruction = string(p);
            break;
          case 3:
            next.type = number(p, 1);
            break;
          case 4:
            next.road = string(p);
            break;
          case 5:
            (void)number(p, 4);
            break;
          case 6:
            (void)string(p);
            break;
          case 7:
          case 8:
          case 9:
            (void)number(p, 1);
            break;
          case 10:
            (void)number(p, 2);
            break;
          case 11:
            next.exit_angle = std::int16_t(number(p, 2));
            break;
          default:
            break;  // Unsupported extension fields remain opaque.
        }
      if (existing == maneuvers_.end() && maneuvers_.size() >= maneuver_capacity) {
        const auto discard = std::find_if(maneuvers_.begin(), maneuvers_.end(), [&](const auto& m) {
          return std::find(order_.begin(), order_.end(), m.first) == order_.end();
        });
        if (discard == maneuvers_.end()) return false;
        maneuvers_.erase(discard);
      }
      maneuvers_[id] = std::move(next);
      state_.updated_at = now;
      return true;  // Maneuver descriptions do not change the current route position.
    }
    auto next = state_;
    auto supported = supported_;
    auto order = order_;
    for (const auto& p : params)
      switch (p.identifier) {
        case 1:
          next.route_state = number(p, 1);
          break;
        case 2:
          (void)number(p, 1);
          break;
        case 3:
          next.current_road = string(p);
          break;
        case 4:
          next.destination = string(p);
          break;
        case 5:
          next.arrival_unix_seconds = number(p, 8);
          break;
        case 6:
          next.remaining_seconds = number(p, 8);
          break;
        case 7:
          next.remaining_m = number(p, 4);
          break;
        case 8:
        case 11:
          (void)string(p);
          break;
        case 9:
        case 12:
          (void)number(p, 1);
          break;
        case 10:
          next.distance_m = number(p, 4);
          break;
        case 13:
          if (p.value.size() % 2 || p.value.size() > 32) return false;
          order.clear();
          for (std::size_t i = 0; i < p.value.size(); i += 2)
            order.push_back((unsigned(p.value[i]) << 8) | p.value[i + 1]);
          break;
        case 14:
        case 16:
        case 17:
          (void)number(p, 2);
          break;
        case 15:
        case 18:
          if (number(p, 1) > 1) return false;
          break;
        case 19:
          next.source = string(p);
          break;
        case 20: {
          auto value = number(p, 1);
          if (value > 1) return false;
          supported = bool(value);
          break;
        }
        default:
          break;
      }
    const bool source_changed = !state_.source.empty() && next.source != state_.source;
    const bool reset = (supported && !*supported) || next.route_state == 0 ||
                       (next.route_state == 5 && state_.route_state != 5) || source_changed ||
                       (!state_.destination.empty() && next.destination != state_.destination);
    if (reset) {
      maneuvers_.clear();
      order.clear();
      next.distance_m.reset();
      next.maneuver.reset();
      if (!field(params, 3)) next.current_road.clear();
      if (!field(params, 4)) next.destination.clear();
      if (source_changed && !field(params, 1)) next.route_state.reset();
      // Never correlate a reused index with the previous route. Only fields in
      // this update can be used until the new maneuver descriptions arrive.
      if (const auto* p = field(params, 13))
        for (std::size_t i = 0; i < p->value.size(); i += 2)
          order.push_back((unsigned(p->value[i]) << 8) | p->value[i + 1]);
      if (const auto* p = field(params, 10)) next.distance_m = number(*p, 4);
      next.remaining_m.reset();
      next.remaining_seconds.reset();
      next.arrival_unix_seconds.reset();
      if (const auto* p = field(params, 7)) next.remaining_m = number(*p, 4);
      if (const auto* p = field(params, 6)) next.remaining_seconds = number(*p, 8);
      if (const auto* p = field(params, 5)) next.arrival_unix_seconds = number(*p, 8);
    }
    if (const auto* p = field(params, 14); p && number(*p, 2) == 0) {
      order.clear();
      maneuvers_.clear();
    }
    if (order != order_ && !field(params, 10)) next.distance_m.reset();
    next.updated_at = now;
    state_ = std::move(next);
    supported_ = supported;
    order_ = std::move(order);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}
NavigationSnapshot RouteGuidance::snapshot(Clock::time_point now) const {
  std::lock_guard lock(mutex_);
  // This feed sends changes, not a periodic heartbeat. An unchanged route may
  // be silent while the session remains healthy; clear() handles disconnect.
  if (!state_.updated_at || now < *state_.updated_at || (supported_ && !*supported_) ||
      !state_.route_state || *state_.route_state == 0 || *state_.route_state > 6)
    return {};
  auto result = state_;
  result.available = true;
  result.maneuver.reset();
  // The current-list order is authoritative; do not guess from remaining distance.
  if (result.route_state == 1 || result.route_state == 6) {
    if (!order_.empty()) {
      auto it = maneuvers_.find(order_.front());
      if (it != maneuvers_.end() && it->second.type == 11 && order_.size() > 1)
        it = maneuvers_.find(order_[1]);
      if (it != maneuvers_.end()) result.maneuver = it->second;
    }
  } else
    result.distance_m.reset();
  return result;
}
std::string navigation_prompt(const NavigationSnapshot& nav) {
  if (!nav.available) return {};
  switch (nav.route_state.value_or(0)) {
    case 2:
      return "Arrived";
    case 3:
      return "Loading route";
    case 4:
      return "Finding location";
    case 5:
      return "Updating route";
    default:
      break;
  }
  std::string turn = "Next maneuver";
  if (nav.maneuver) switch (nav.maneuver->type.value_or(255)) {
      case 0:
      case 3:
      case 5:
        turn = "Continue";
        break;
      case 1:
        turn = "Turn left";
        break;
      case 2:
        turn = "Turn right";
        break;
      case 4:
        turn = "Make a U-turn";
        break;
      case 6:
        turn = "Enter roundabout";
        break;
      case 7:
        turn = "Exit roundabout";
        break;
      case 8:
        turn = "Take the exit";
        break;
      case 9:
        turn = "Take the ramp";
        break;
      case 10:
      case 12:
        turn = "Arrive at destination";
        break;
      case 11:
        turn = "Start route";
        break;
      case 13:
        turn = "Keep left";
        break;
      case 14:
        turn = "Keep right";
        break;
      case 49:
        turn = "Slight left";
        break;
      case 50:
        turn = "Slight right";
        break;
      default:
        break;
    }
  if (nav.distance_m) {
    char value[48];
    if (*nav.distance_m < 1000)
      std::snprintf(value, sizeof(value), " · %u m", *nav.distance_m);
    else
      std::snprintf(value, sizeof(value), " · %.1f km", *nav.distance_m / 1000.);
    turn += value;
  }
  return turn;
}
}  // namespace headunit
